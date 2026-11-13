"""Exercise OIT and clustered lighting under a simulated 128 MiB descriptor limit."""

import argparse
from collections import Counter
import json
from math import ceil
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from display_capture_test_support import Asset, validation_log_error
from frame_resource_initialization_runtime_regression import TECHNIQUES, require
from msaa_attachment_capabilities_runtime_regression import (
    PROFILES, VALIDATION, layer_environment, layer_files, sha, verify_layers,
)


LIMIT = 128 * 1024 * 1024
PROFILE = "VP_TEST_STORAGE_RANGE_128MIB"
RANGE_VUID = "VUID-VkWriteDescriptorSet-descriptorType-00333"
LARGE = (3840, 2160)
SMALL = (1600, 900)
STRESS = (1600, 1000)
NODE_BYTES = 32
MAX_NODES = LIMIT // NODE_BYTES


def write_scene(path, *, stress=False):
    asset = Asset()
    model = asset.model
    indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)
    normals = asset.accessor("VEC3", [(0, 0, 1)] * 4)

    def plane(z, size, material):
        corners = [(-size, -size, z), (size, -size, z), (size, size, z), (-size, size, z)]
        return dict(attributes=dict(POSITION=asset.accessor("VEC3", corners), NORMAL=normals),
                    indices=indices, material=material)

    model["materials"] = [
        {"pbrMetallicRoughness": {"baseColorFactor": [.8, .1, .1, 1]}},
        {"alphaMode": "BLEND", "pbrMetallicRoughness": {"baseColorFactor": [.1, .8, .1, .6]}},
    ]
    model["meshes"] = [{"primitives": [plane(0, 1, 0)]}, {"primitives": [plane(1, 1, 1)]}]
    light = "KHR_lights_punctual"
    model["extensionsUsed"] = [light]
    model["extensions"] = {light: {"lights": [{"type": "point", "intensity": 48, "range": 10}]}}
    model["nodes"] = [{"mesh": 0, "name": "Opaque receiver"}, {"mesh": 1, "name": "Transparent overlay"},
                      {"name": "Point-light witness", "translation": [0, 0, 4], "extensions": {light: {"light": 0}}}]
    if stress:
        # Four fullscreen layers behind the ordinary overlay demand 6.4M+ nodes.
        model["meshes"].append({"primitives": [plane(z, 8, 1) for z in (.2, .4, .6, .8)]})
        model["nodes"].append({"mesh": 2, "name": "Overflow layers", "translation": [30, 0, 0]})
    model["scenes"][0]["nodes"] = list(range(len(model["nodes"])))
    asset.write(path)


def main_phases():
    return (
        ("startup-4k", 2, LARGE, {}),
        ("opaque-4k", 3, LARGE, {"nodeFlags": {"sourceNode": 1, "visible": False}}),
        ("shrink", 4, SMALL, {"resize": list(SMALL), "nodeFlags": {"sourceNode": 1, "visible": True}}),
        ("resize-4k", 5, LARGE, {"resize": list(LARGE)}),
    )


def stress_phases():
    transform = {"node": {"sourceNode": 3}, "matrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]}
    return (("before-growth", 2, STRESS, {}), ("layers-introduced", 3, STRESS, {"nodeTransform": transform}),
            ("after-growth", 9, STRESS, {}), ("still-bounded", 13, STRESS, {}))


def write_sequence(path, phases):
    frames = [frame for _, frame, _, _ in phases]
    events = [dict(frame=frame, depthProbe=[width // 2, height // 2], **options)
              for _, frame, (width, height), options in phases]
    path.write_text(json.dumps(dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames, events=events),
                               indent=2) + "\n", encoding="utf-8")


def cluster_sizes(extent):
    # TileLightGrid is two uint32 values. Each of 16 depth slices reserves 128 uint32 light indices.
    clusters = ceil(extent[0] / 16) * ceil(extent[1] / 16) * 16
    return clusters * 8, clusters * 128 * 4


def check_policy(state, extent, phase, passes):
    require(state["storageBufferRangeLimit"] == LIMIT, "Renderer did not receive the 128 MiB profile limit")
    oit, clustered = state["oit"], state["clusteredLighting"]
    expected_grid, expected_indices = cluster_sizes(extent)
    limited = expected_indices > LIMIT
    active = not limited and state["technique"] == "deferred-raster"
    require((clustered["tileGridBytes"], clustered["lightIndexListBytes"]) == (expected_grid, expected_indices), clustered)
    require(clustered["status"] == ("storageRangeExceeded" if limited else "available") and
            clustered["active"] == active, clustered)
    require(("TileCull" in passes) == active, passes)
    expected_nodes = 2 * extent[0] * extent[1]
    requested, nodes = oit["requestedNodeCapacity"], oit["nodeCapacity"]
    require(oit["maxNodeCapacity"] == MAX_NODES and requested >= expected_nodes and
            nodes == min(requested, MAX_NODES) and oit["nodeBufferBytes"] == nodes * NODE_BYTES <= LIMIT and
            oit["storageRangeLimited"] == (requested > MAX_NODES or oit["overflowed"]), oit)
    if phase in {name for name, *_ in main_phases()}:
        require(requested == expected_nodes and oit["storageRangeLimited"] == limited and
                not oit["overflowed"] and oit["growCount"] == 0, oit)
    return {"storageBufferRangeLimit": state["storageBufferRangeLimit"], "oit": oit, "clusteredLighting": clustered}


def read_capture(path, phase, technique, *, baseline):
    from gltf_pick_depth_runtime_regression import projected_depth
    from PIL import Image

    name, frame, requested, _ = phase
    state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    extent = tuple(state["resolution"])
    # Old decorated Win32 startup and resize may lose a title-bar-height strip.
    require(extent == requested or (baseline and requested == LARGE and extent[0] == LARGE[0] and
                                    2100 <= extent[1] < LARGE[1]), f"Expected {requested}, captured {extent}: {path}")
    require(state["validationEnabled"] and state["technique"] == technique and state["msaaSamples"] == 1 and
            state["fsr"]["renderExtent"] == list(extent) and state["lighting"]["pointLightCount"] == 1, path)
    require((metadata["width"], metadata["height"]) == extent and metadata["source"] == "viewport" and
            not metadata["includesUi"], path)
    active = {p["name"] for p in state["passes"] if p["active"]}
    transparent = name != "opaque-4k"
    require(({"OitClear", "OitResolve", "TransparentPick"} <= active if transparent else "OitResolve" not in active),
            f"Incorrect OIT passes for {path}: {sorted(active)}")
    probe = state["depthProbe"]
    require(probe["pixel"] == [requested[0] // 2, requested[1] // 2] and probe["opaque"] is not None and
            abs(probe["opaque"] - projected_depth(state, 0)) < 2e-6, probe)
    if transparent:
        interaction = probe["sceneInteraction"]
        require(probe["pick"] is not None and abs(probe["pick"] - projected_depth(state, 1)) < 2e-6 and
                interaction["rawGpuId"] > 0 and interaction["gpu"]["hit"]["sourceNode"] == 1, probe)
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == extent, path)
        center = image.convert("RGB").getpixel((extent[0] // 2, extent[1] // 2))
    policy = check_policy(state, extent, name, active) if not baseline else {}
    return {"phase": name, "frame": frame, "extent": extent, "centerRgb": center, "passes": sorted(active),
            "swapchainRecreates": state["swapchainRecreates"], "depthProbe": probe, "revision": state["build"]["revision"],
            "pngSha256": sha(path), **policy}


def capture(renderer, output, technique, phases, env, libraries, *, baseline):
    output.mkdir()
    stress = phases[0][0] == "before-growth"
    model, sequence, image = output / "planes.gltf", output / "capture.json", output / "capture.png"
    write_scene(model, stress=stress)
    write_sequence(sequence, phases)
    width, height = phases[0][2]
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
               "--no-taa", "--no-ray-query", "--no-ddgi", "--no-bloom", "--msaa", "1", "--display-mode", "lit",
               "--width", str(width), "--height", str(height), "--render-technique", technique,
               "--camera-position", "0", "0", "6", "--camera-target", "0", "0", "0", "--camera-fov", "45",
               "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
               "--bounce-intensity", "0", "--directional-intensity", "0", "--exposure", "1", "--fixed-dt", ".016666667",
               "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(image)]
    if not baseline:
        command.append("--borderless")
    log = output / "renderer.log"
    with log.open("w", encoding="utf-8") as stream:
        run = subprocess.run(command, cwd=renderer.parent, env=env, stdout=stream, stderr=subprocess.STDOUT,
                             timeout=360, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    raw = log.read_text(encoding="utf-8", errors="replace")
    require(run.returncode == 0, f"Renderer exited {run.returncode}: {log}")
    verify_layers(raw, *libraries, profile_name=PROFILE)
    observed = Counter(map(int, re.findall(r"\.range \((\d+)\) is greater than maxStorageBufferRange \(134217728\)", raw)))
    captures = []
    for phase in phases:
        path = image if phase == phases[-1] else output / f"capture.frame-{phase[1]:04d}.png"
        captures.append(read_capture(path, phase, technique, baseline=baseline))
    if baseline:
        extent = captures[0]["extent"]
        expected = {cluster_sizes(extent)[1], 2 * extent[0] * extent[1] * NODE_BYTES}
        require(set(re.findall(r"\bVUID-[\w-]+", raw)) == {RANGE_VUID} and "SYNC-HAZARD" not in raw and
                set(observed) == expected, f"Expected prior oversized cluster and OIT writes: {observed}: {log}")
    else:
        require(not validation_log_error(raw), f"Vulkan validation diagnostics: {log}")
    if stress:
        before, _, grown, stable = [entry["oit"] for entry in captures]
        require(before["nodeCapacity"] == 2 * STRESS[0] * STRESS[1] and before["growCount"] == 0 and
                not before["storageRangeLimited"] and not before["overflowed"], before)
        require(grown["nodeCapacity"] == MAX_NODES and grown["growCount"] == 1 and grown["overflowed"] and
                stable == grown, f"Expected one growth to the cap and stable overflow: {grown}, {stable}")
    else:
        startup, opaque, small, resized = captures
        require([row["swapchainRecreates"] for row in captures] == [0, 0, 1, 2], captures)
        require(opaque["centerRgb"][0] > 40 and opaque["centerRgb"][0] > opaque["centerRgb"][1] + 15,
                f"Opaque receiver lost its authored point light: {opaque['centerRgb']}")
        for entry in (startup, small, resized):
            r, g, _ = entry["centerRgb"]
            require(g > r + 25 and g > opaque["centerRgb"][1] + 25,
                    f"Transparent overlay or local point light missing at {entry['phase']}: {entry['centerRgb']}")
        require(startup["pngSha256"] == resized["pngSha256"], "Startup and resized 4K scenes differ")
    print(f"{output.name}: {[entry['extent'] for entry in captures]} verified", flush=True)
    return {"technique": technique, "command": command, "log": str(log), "fixtureSha256": sha(model),
            "sequenceSha256": sha(sequence), "oversizeDescriptorWrites": dict(observed), "captures": captures}


def confirm_profile(vulkaninfo, output, env, libraries):
    info, log = output / "vulkaninfo.txt", output / "vulkaninfo.log"
    command = [str(vulkaninfo), "--text", "--output", str(info)]
    with log.open("w", encoding="utf-8") as stream:
        run = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=90,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    require(run.returncode == 0, f"vulkaninfo exited {run.returncode}: {log}")
    observed = set(map(int, re.findall(r"\bmaxStorageBufferRange\s*=\s*(\d+)", info.read_text(errors="replace"))))
    require(observed == {LIMIT}, f"Vulkan Profiles did not report {LIMIT} bytes: {observed}")
    raw = log.read_text(encoding="utf-8", errors="replace")
    verify_layers(raw, *libraries, profile_name=PROFILE)
    return {"path": str(vulkaninfo), "sha256": sha(vulkaninfo), "reportedStorageBufferRange": LIMIT}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--validation-layer", type=Path, help="directory containing the Khronos validation layer")
    parser.add_argument("--profiles-layer", type=Path, help="directory containing the Khronos Profiles layer")
    parser.add_argument("--vulkaninfo", type=Path, help="path to vulkaninfo or vulkaninfoSDK")
    parser.add_argument("--managed-output", action="store_true", help="write to a fresh run directory under --output")
    parser.add_argument("--technique", action="append", choices=TECHNIQUES)
    parser.add_argument("--verify-baseline", action="store_true", help="require oversized descriptor writes on the old renderer")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_STORAGE_RANGE") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_STORAGE_RANGE=1 to run GPU captures")
        return 77
    validation = args.validation_layer or os.environ.get("CONTAINER_STORAGE_VALIDATION_LAYER")
    profiles = args.profiles_layer or os.environ.get("CONTAINER_STORAGE_PROFILES_LAYER")
    if not validation or not profiles:
        parser.error("provide --validation-layer and --profiles-layer or their environment variables")
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    validation, profiles = Path(validation).resolve(strict=True), Path(profiles).resolve(strict=True)
    vulkaninfo = args.vulkaninfo or os.environ.get("CONTAINER_STORAGE_VULKANINFO")
    if not vulkaninfo:
        vulkaninfo = next((p for p in (profiles / "vulkaninfoSDK.exe", profiles / "vulkaninfo.exe") if p.is_file()),
                          shutil.which("vulkaninfo"))
    if not vulkaninfo:
        parser.error("provide --vulkaninfo or CONTAINER_STORAGE_VULKANINFO")
    validation_library, validation_info = layer_files(validation, VALIDATION)
    profiles_library, profiles_info = layer_files(profiles, PROFILES)
    output.mkdir(parents=True, exist_ok=args.managed_output)
    if args.managed_output:
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        print(f"Captures: {output}", flush=True)
    env = layer_environment(output, {"limits": {"maxStorageBufferRange": LIMIT}}, validation, profiles,
                            profile_name=PROFILE, property_structure="VkPhysicalDeviceProperties")
    libraries = validation_library, profiles_library
    report = {"rendererSha256": sha(renderer), "baseline": args.verify_baseline,
              "layers": {VALIDATION: validation_info, PROFILES: profiles_info}, "runs": [], "passed": False}
    try:
        report["vulkaninfo"] = confirm_profile(Path(vulkaninfo).resolve(strict=True), output, env, libraries)
        for technique in args.technique or TECHNIQUES:
            report["runs"].append(capture(renderer, output / technique, technique, main_phases(), env, libraries,
                                          baseline=args.verify_baseline))
        if not args.verify_baseline:
            report["runs"].append(capture(renderer, output / "oit-growth", "deferred-raster", stress_phases(),
                                          env, libraries, baseline=False))
        require(sha(renderer) == report["rendererSha256"], "Renderer executable changed during capture")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
