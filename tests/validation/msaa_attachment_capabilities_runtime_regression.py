"""Check raster MSAA startup, live selection and picking under Vulkan Profiles."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from display_capture_test_support import validation_log_error
from frame_resource_initialization_runtime_regression import (
    START, TECHNIQUES, capture_command, check_capture, make_sequence, require,
)


VALIDATION = "VK_LAYER_KHRONOS_validation"
PROFILES = "VK_LAYER_KHRONOS_profiles"
PROFILE_NAME = "VP_CONTAINER_msaa_test"
RESOLVE_VUID = "VUID-VkRenderingInfo-pDepthAttachment-06104"
CASES = {
    "device": {},
    "paired-resolve": {
        "independentResolveNone": False,
        "independentResolve": False,
        "supportedDepthResolveModes": ["VK_RESOLVE_MODE_SAMPLE_ZERO_BIT"],
        "supportedStencilResolveModes": ["VK_RESOLVE_MODE_SAMPLE_ZERO_BIT"],
    },
    "integer-color-1x": {"framebufferIntegerColorSampleCounts": ["VK_SAMPLE_COUNT_1_BIT"]},
}


def phases(multisampled):
    return (
        ("startup", 2, START, multisampled, True, {}),
        ("live-1x", 4, START, 1, True, {"samples": 1}),
        ("live-request-4x", 6, START, multisampled, True, {"samples": 4}),
        ("restore-1x", 8, START, 1, True, {"samples": 1}),
    )


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def layer_files(directory, name):
    manifest = directory / f"VkLayer_{name.removeprefix('VK_LAYER_')}.json"
    layer = json.loads(manifest.read_bytes())["layer"]
    require(layer["name"] == name, f"Unexpected Vulkan layer in {manifest}")
    library = (directory / layer["library_path"]).resolve(strict=True)
    return library, {"apiVersion": layer["api_version"], "manifestSha256": sha(manifest),
                     "librarySha256": sha(library)}


def layer_environment(output, overrides, validation, profiles, *, profile_name=PROFILE_NAME,
                      property_structure="VkPhysicalDeviceVulkan12Properties"):
    settings = ["khronos_validation.validate_sync = true"]
    layer_paths = [validation]
    instance_layers = [VALIDATION]
    env = os.environ.copy()
    env.pop("VK_LOADER_LAYERS_ENABLE", None)
    if overrides:
        profile = {
            "$schema": "https://schema.khronos.org/vulkan/profiles-0.8.2-276.json#",
            "capabilities": {"test": {"properties": {property_structure: overrides}}},
            "profiles": {profile_name: {
                "version": 1, "api-version": "1.2.0", "label": "Container Vulkan capability regression",
                "description": "Simulate constrained physical-device properties",
                "contributors": {"Container": {"company": "Container"}},
                "history": [{"revision": 1, "date": "2026-11-13", "author": "Container", "comment": "Initial test"}],
                "capabilities": ["test"],
            }},
        }
        path = output / f"{profile_name}.json"
        path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
        layer_paths.append(profiles)
        # Vulkan Validation must see physical-device properties after Profiles has overridden them.
        instance_layers.append(PROFILES)
        settings.extend([
            "khronos_profiles.profile_emulation = true",
            f"khronos_profiles.profile_file = {path.as_posix()}",
            f"khronos_profiles.profile_name = {profile_name}",
            "khronos_profiles.simulate_capabilities = SIMULATE_PROPERTIES_BIT",
            "khronos_profiles.emulate_portability = false",
            "khronos_profiles.debug_actions = DEBUG_ACTION_STDOUT_BIT",
        ])
        env.update(VK_KHRONOS_PROFILES_PROFILE_FILE=str(path), VK_KHRONOS_PROFILES_PROFILE_NAME=profile_name,
                   VK_KHRONOS_PROFILES_SIMULATE_CAPABILITIES="SIMULATE_PROPERTIES_BIT",
                   VK_KHRONOS_PROFILES_EMULATE_PORTABILITY="false",
                   VK_KHRONOS_PROFILES_DEBUG_ACTIONS="DEBUG_ACTION_STDOUT_BIT",
                   VK_KHRONOS_PROFILES_DEBUG_REPORTS="DEBUG_REPORT_NOTIFICATION_BIT,DEBUG_REPORT_WARNING_BIT,DEBUG_REPORT_ERROR_BIT")
    (output / "vk_layer_settings.txt").write_text("\n".join(settings) + "\n", encoding="utf-8")
    return dict(env, VK_LAYER_PATH=os.pathsep.join(map(str, layer_paths)),
                VK_INSTANCE_LAYERS=os.pathsep.join(instance_layers), VK_LOADER_DEBUG="layer",
                VK_LAYER_SETTINGS_PATH=str(output))


def verify_layers(log, validation_library, profiles_library, *, profile_name=PROFILE_NAME):
    for kind in ("Insert instance", "Inserted device"):
        match = re.search(rf'{kind} layer "{VALIDATION}" \(([^\r\n]+)\)', log)
        require(match and Path(match[1]).resolve() == validation_library, "The pinned validation layer was not used")
    match = re.search(rf'Insert instance layer "{PROFILES}" \(([^\r\n]+)\)', log)
    if profiles_library is None:
        require(not match, "Profiles was enabled for the unmodified device control")
    else:
        require(match and Path(match[1]).resolve() == profiles_library and
                f"Overriding device capabilities with the '{profile_name}' profile capabilities." in log and
                "simulate_capabilities: SIMULATE_PROPERTIES_BIT" in log and "PROFILES ERROR:" not in log,
                "The pinned Profiles layer did not apply the expected property override")


def resolve_policy(capture, case):
    image = Path(capture["image"])
    state = json.loads(image.with_suffix(".telemetry.json").read_bytes())
    supported = state["supportedMsaaSamples"]
    depth, stencil = state["msaaDepthResolveMode"], state["msaaStencilResolveMode"]
    if case == "integer-color-1x":
        require(supported == [1], f"Integer pick attachment still permits MSAA: {supported}")
    else:
        require(1 in supported and 4 in supported, f"The fixture requires a device supporting 4x MSAA: {supported}")
    if capture["msaa"] == 1:
        require((depth, stencil) == (0, 0), f"1x should have no depth or stencil resolve: {(depth, stencil)}")
    elif case == "paired-resolve":
        require((depth, stencil) == (1, 1), f"The constrained profile requires SAMPLE_ZERO for both aspects: {(depth, stencil)}")
    else:
        require((depth, stencil) in ((8, 0), (1, 0), (8, 8), (1, 1)),
                f"Unexpected reverse-Z depth/stencil resolve modes: {(depth, stencil)}")
    return {"supportedSamples": supported, "depthResolveMode": depth, "stencilResolveMode": stencil}


def run_capture(renderer, model, sequence, output, technique, capture_phases, env, libraries, case, baseline):
    image = output / f"{technique}.png"
    log = image.with_suffix(".log")
    command = capture_command(renderer, model, sequence, image, technique, startup_samples=4)
    with log.open("w", encoding="utf-8") as stream:
        child = subprocess.run(command, cwd=renderer.parent, env=env, stdout=stream, stderr=subprocess.STDOUT,
                               timeout=180, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    raw = log.read_text(encoding="utf-8", errors="replace")
    verify_layers(raw, *libraries)
    violations = re.findall(r"\bVUID-[\w-]+", raw)
    if baseline and case == "paired-resolve":
        require(set(violations) == {RESOLVE_VUID} and "SYNC-HAZARD" not in raw,
                f"Expected the previous independent-resolve failure: {log}")
    else:
        require(not validation_log_error(raw), f"Vulkan diagnostics: {log}")
    require(child.returncode == 0, f"Vulkan capture exited {child.returncode}: {log}")
    captures = [check_capture(output, technique, phase, capture_phases[-1][1]) for phase in capture_phases]
    startup, reduced, increased, restored = captures
    require(startup["decodedSha256"] == increased["decodedSha256"] and
            reduced["decodedSha256"] == restored["decodedSha256"], f"Live MSAA transitions changed output: {log}")
    if not baseline:
        for capture in captures:
            capture.update(resolve_policy(capture, case))
    return {"technique": technique, "command": command, "log": str(log), "validationVuids": sorted(set(violations)),
            "validationVuidCount": len(violations),
            "captures": captures}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--validation-layer", type=Path, help="directory containing the Khronos validation layer")
    parser.add_argument("--profiles-layer", type=Path, help="directory containing the Khronos Profiles layer")
    parser.add_argument("--case", action="append", choices=CASES, help="limit the run to selected profile cases")
    parser.add_argument("--managed-output", action="store_true", help="write to a fresh run directory under --output")
    parser.add_argument("--verify-baseline", action="store_true", help="verify the old 4x choices and validation failure")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_MSAA_CAPABILITIES") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_MSAA_CAPABILITIES=1 to run GPU captures")
        return 77
    validation = args.validation_layer or os.environ.get("CONTAINER_MSAA_VALIDATION_LAYER")
    profiles = args.profiles_layer or os.environ.get("CONTAINER_MSAA_PROFILES_LAYER")
    if not validation or not profiles:
        parser.error("provide --validation-layer and --profiles-layer or their environment variables")

    from gltf_pick_depth_runtime_regression import write_scene

    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    validation, profiles = Path(validation).resolve(strict=True), Path(profiles).resolve(strict=True)
    validation_library, validation_info = layer_files(validation, VALIDATION)
    profiles_library, profiles_info = layer_files(profiles, PROFILES)
    output.mkdir(parents=True, exist_ok=args.managed_output)
    if args.managed_output:
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        print(f"Captures: {output}", flush=True)
    model, sequence = output / "planes.gltf", output / "msaa.json"
    write_scene(model)
    make_sequence(sequence, phases(4))
    report = {"rendererSha256": sha(renderer), "fixtureSha256": sha(model), "sequenceSha256": sha(sequence),
              "baseline": args.verify_baseline, "layers": {VALIDATION: validation_info, PROFILES: profiles_info},
              "cases": {}, "passed": False}
    try:
        for case in args.case or CASES:
            case_output = output / case
            case_output.mkdir()
            overrides = CASES[case]
            env = layer_environment(case_output, overrides, validation, profiles)
            libraries = validation_library, profiles_library if overrides else None
            case_phases = phases(1 if case == "integer-color-1x" and not args.verify_baseline else 4)
            result = {"propertyOverrides": overrides, "runs": []}
            report["cases"][case] = result
            for technique in TECHNIQUES:
                run = run_capture(renderer, model, sequence, case_output, technique, case_phases, env, libraries,
                                  case, args.verify_baseline)
                result["runs"].append(run)
                print(f"{case} {technique}: samples {[c['msaa'] for c in run['captures']]}, GPU picking verified", flush=True)
        require(sha(renderer) == report["rendererSha256"], "Renderer executable changed during capture")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
