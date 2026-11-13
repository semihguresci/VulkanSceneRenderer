"""Compare authored glTF opacity micromaps with the sampled-alpha renderer."""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from display_capture_test_support import emitter_fixture
from frame_resource_initialization_runtime_regression import require
from msaa_attachment_capabilities_runtime_regression import PROFILES, VALIDATION, layer_files, sha


EXTENSION = "EXT_mesh_opacity_micromap"
FLAG = "--experimental-opacity-micromaps"
OPT_IN = "CONTAINER_RUN_GPU_GLTF_OPACITY_MICROMAPS"
PROFILE_NAME = "VP_CONTAINER_no_opacity_micromap"
PROFILE_FEATURES = {"VkPhysicalDeviceOpacityMicromapFeaturesEXT": {"micromap": False}}
os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")


def quad(left, right, low, high, z):
    return [[left, low, z], [right, low, z], [right, high, z], [left, high, z]]


def fixture(path, malformed=False):
    import numpy as np
    from gltf_accessor_runtime_regression import Asset

    asset = Asset()
    source = asset.model
    texture = asset.image(np.array([[[180, 90, 40, 0], [180, 90, 40, 255]]], dtype=np.uint8))

    def primitive(positions, coordinates, material):
        indices = [i + first for first in range(0, len(positions), 4) for i in (0, 1, 2, 0, 2, 3)]
        return {"attributes": {"POSITION": asset.accessor("VEC3", positions),
                               "NORMAL": asset.accessor("VEC3", [[0, 0, 1]] * len(positions)),
                               "TEXCOORD_0": asset.accessor("VEC2", coordinates)},
                "indices": asset.accessor("SCALAR", indices, 5123, indices=True), "material": material}

    receiver = primitive(quad(-2.5, 2.5, -2, 2, 0), [[.5, .5]] * 4, 0)
    blocker = primitive(quad(-.9, -.25, -.55, .55, 1) + quad(.25, .9, -.55, .55, 1),
                        [[.25, .5]] * 4 + [[.75, .5]] * 4, 1)
    data = asset.view(bytes((0, 1)))
    triangles = asset.view(struct.pack("<IHHIHH", 0, 0, 1, 1, 0, 1))
    lookup = asset.accessor("SCALAR", [0, 0, 1, 1], 5125)
    blocker["extensions"] = {EXTENSION: {"micromap": (1 << 32) if malformed else 0,
                                          "micromapIndices": lookup}}
    source["extensions"] = {EXTENSION: {"micromaps": [{"data": data, "triangles": triangles,
        "usageCounts": [2], "usageLevels": [0], "usageFormats": [1]}]}}
    source["meshes"] = [{"primitives": [receiver]}, {"primitives": [blocker]}]
    source["nodes"] = [{"mesh": 0, "name": "Receiver"}, {"mesh": 1, "name": "Baked cutout"},
                       {"camera": 0, "translation": [0, 0, 4]}]
    source["scenes"] = [{"nodes": [0, 1, 2]}]
    source["cameras"] = [{"type": "orthographic", "orthographic": {"xmag": 2, "ymag": 1.5,
                                                                      "znear": .1, "zfar": 20}}]
    source["materials"] = [
        {"pbrMetallicRoughness": {"baseColorFactor": [.65, .7, .75, 1], "metallicFactor": 0}},
        {"alphaMode": "MASK", "alphaCutoff": .5, "doubleSided": True,
         "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "baseColorTexture": {"index": texture},
                                 "metallicFactor": 0}},
    ]
    source["extensionsUsed"] = [EXTENSION]
    asset.write(path)


def merge_fixture(path):
    model = emitter_fixture(path, {"emissiveFactor": [0, 1, 0],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}}})
    model["nodes"][0].update(name="Visible merge witness", translation=[1.35, 0, 1.5], scale=[.4, .55, 1])
    path.write_text(json.dumps(model), encoding="utf-8")


def pixels(path):
    import cv2
    import numpy as np
    from PIL import Image

    if path.suffix == ".exr":
        values = cv2.imdecode(np.frombuffer(path.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
        assert values is not None and values.dtype == np.float32 and np.isfinite(values).all(), path
        return values
    with Image.open(path) as image:
        return np.array(image.convert("RGB"))


def layer_environment(output, validation_dir, profiles_dir=None):
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer",
               OPENCV_IO_ENABLE_OPENEXR="1")
    settings = ["khronos_validation.validate_sync = true"]
    if validation_dir is not None:
        paths = [validation_dir] if profiles_dir is None else [validation_dir, profiles_dir]
        env.pop("VK_LOADER_LAYERS_ENABLE", None)
        env.update(VK_LAYER_PATH=os.pathsep.join(map(str, paths)),
                   VK_INSTANCE_LAYERS=os.pathsep.join([VALIDATION] if profiles_dir is None else [VALIDATION, PROFILES]))
    if profiles_dir is not None:
        profile = {
            "$schema": "https://schema.khronos.org/vulkan/profiles-0.8.2-276.json#",
            "capabilities": {"test": {"features": PROFILE_FEATURES}},
            "profiles": {PROFILE_NAME: {
                "version": 1, "api-version": "1.2.0", "label": "Container opacity micromap fallback regression",
                "description": "Disable the optional opacity micromap feature while retaining ray query",
                "contributors": {"Container": {"company": "Container"}},
                "history": [{"revision": 1, "date": "2026-11-13", "author": "Container", "comment": "Initial test"}],
                "capabilities": ["test"],
            }},
        }
        path = output / f"{PROFILE_NAME}.json"
        path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
        profile_settings = {
            "profile_emulation": "true", "profile_file": path.as_posix(), "profile_name": PROFILE_NAME,
            "simulate_capabilities": "SIMULATE_FEATURES_BIT", "emulate_portability": "false",
            "default_feature_values": "DEFAULT_FEATURE_VALUES_DEVICE",
            "unknown_feature_values": "UNKNOWN_FEATURE_VALUES_DEVICE", "debug_actions": "DEBUG_ACTION_STDOUT_BIT",
            "debug_reports": "DEBUG_REPORT_NOTIFICATION_BIT,DEBUG_REPORT_WARNING_BIT,DEBUG_REPORT_ERROR_BIT",
        }
        for key, value in profile_settings.items():
            settings.append(f"khronos_profiles.{key} = {value}")
            env[f"VK_KHRONOS_PROFILES_{key.upper()}"] = value
    (output / "vk_layer_settings.txt").write_text("\n".join(settings) + "\n", encoding="utf-8")
    return env


def verify_layer(log, name, pinned_library=None):
    inserted = re.findall(rf'Insert(?:ed)? (instance|device) layer "{name}" \(([^\r\n]+)\)', log)
    expected = {"instance", "device"} if name == VALIDATION else {"instance"}
    require(expected <= {kind for kind, _ in inserted} and (pinned_library is None or
            all(Path(path).resolve() == pinned_library for _, path in inserted)), f"Expected Vulkan layer {name}")


def verify_capability(states, *, requested, device_enabled):
    for state in states:
        require(state["supported"] and state["active"] and state["opacityMicromaps"] == {
            "requested": requested, "deviceEnabled": device_enabled}, f"Unexpected ray capability: {state}")
        if not device_enabled:
            build = state["build"]
            require(all(build[key] == 0 for key in ("opacityMicromaps", "opacityMicromapsBuilt",
                                                     "opacityMicromapBlas", "opacityMicromapBytes")),
                    f"Disabled opacity micromaps allocated resources: {build}")


def check_merge_round_trip(capture, model, incoming):
    import numpy as np

    events = [{"frame": 2, "guiWaitForLoad": True, "merge": {"path": str(incoming),
               "parentName": "Opacity micromap merge witness"}},
              {"frame": 3, "sceneHistory": "undo"}, {"frame": 4, "sceneHistory": "redo"}]
    baseline, disabled, _, baseline_telemetry = capture("merge-baseline", model, enable=False, events=events)
    images, enabled, _, telemetry = capture("merge-micromaps", model, enable=True, events=events)
    verify_capability(disabled, requested=False, device_enabled=False)
    verify_capability(enabled, requested=True, device_enabled=True)
    for states in (baseline_telemetry, telemetry):
        merge_states = [state["gltfMerge"] for state in states]
        require([state["outcome"] for state in merge_states] == ["idle", "committed", "committed", "committed"]
                and all(not state["error"] for state in merge_states), f"Merge did not commit: {merge_states}")
        require([state["sourceReadCount"] for state in merge_states] == [0, 1, 1, 1]
                and [state["generationPublications"] for state in merge_states] == [0, 1, 2, 3],
                f"Merge history reread or failed to publish: {merge_states}")
        history = [state.get("sceneEditHistory", {"undo": 0, "redo": 0}) for state in states]
        require([(state["undo"], state["redo"]) for state in history] == [(0, 0), (1, 0), (0, 1), (1, 0)], history)
        instances = [state["rayShadows"]["build"]["instances"] for state in states]
        require(instances == [2, 3, 2, 3], f"Merge did not add and replay the expected geometry: {instances}")
    for reference, actual in zip(baseline, images, strict=True):
        np.testing.assert_array_equal(reference, actual)
    np.testing.assert_array_equal(images[0], images[2])
    np.testing.assert_array_equal(images[1], images[3])
    changed_pixels = int(np.count_nonzero(np.any(images[0] != images[1], axis=2)))
    require(changed_pixels > 100, "The merge had no visible effect")
    count = [state["build"]["opacityMicromapBlas"] for state in enabled]
    require(count == [1, 0, 1, 0], f"Merge/Undo/Redo micromap attachments: {count}")
    return {"opacityMicromapBlas": count, "changedPixels": changed_pixels, "pixelExact": True,
            "sourceReads": [0, 1, 1, 1], "generationPublications": [0, 1, 2, 3]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="write to a fresh run directory under --output")
    parser.add_argument("--validation-layer-dir", type=Path, help="directory containing the pinned Khronos validation layer")
    parser.add_argument("--profiles-layer-dir", type=Path, help="directory containing Khronos Profiles for the fallback lane")
    parser.add_argument("--merge-only", action="store_true", help="isolate the Merge, Undo, and Redo regression")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get(OPT_IN) != "1":
        print(f"SKIP: set {OPT_IN}=1 to run GPU captures")
        return 77
    if not args.generate_only and args.renderer is None:
        parser.error("--renderer is required for GPU captures")
    validation = args.validation_layer_dir or os.environ.get("CONTAINER_OPACITY_MICROMAP_VALIDATION_LAYER")
    profiles = args.profiles_layer_dir or os.environ.get("CONTAINER_OPACITY_MICROMAP_PROFILES_LAYER")
    if not args.generate_only and profiles and not validation:
        parser.error("--profiles-layer-dir requires --validation-layer-dir or its environment variable")

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=args.managed_output)
    if args.managed_output:
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        print(f"Captures: {output}", flush=True)
    model, malformed, incoming = (output / name for name in ("cutout.gltf", "malformed.gltf", "incoming.gltf"))
    fixture(model)
    fixture(malformed, malformed=True)
    merge_fixture(incoming)
    if args.generate_only:
        return 0

    import numpy as np
    from gltf_accessor_runtime_regression import load_asset
    from gltf_extended_materials_runtime_regression import validation_log_error

    renderer = args.renderer.resolve(strict=True)
    validation_dir = Path(validation).resolve(strict=True) if validation else None
    profiles_dir = Path(profiles).resolve(strict=True) if profiles else None
    validation_library, profiles_library = None, None
    layer_receipts = {}
    if validation_dir is not None:
        validation_library, layer_receipts[VALIDATION] = layer_files(validation_dir, VALIDATION)
    if profiles_dir is not None:
        profiles_library, layer_receipts[PROFILES] = layer_files(profiles_dir, PROFILES)
    native_env = layer_environment(output, validation_dir)
    report = {"rendererSha256": sha(renderer), "fixtureSha256": {file.name: sha(file) for file in (model, malformed, incoming)},
              "layers": layer_receipts, "runs": {}, "unsupportedDeviceSimulated": False, "passed": False}

    def capture(name, source, *, enable, technique="path-tracing", events=(), save=None, samples=1, camera_node=2,
                work=output, env=native_env, profiled=False):
        count = max((event["frame"] for event in events), default=1)
        stem = work / name
        suffix = ".exr" if technique == "path-tracing" else ".png"
        image = stem.with_suffix(suffix)
        sequence = stem.with_suffix(".sequence.json")
        sequence.write_text(json.dumps({"schemaVersion": 1, "frames": count, "sampleFrames": list(range(1, count + 1)),
                                        "events": events}), encoding="utf-8")
        command = [str(renderer), "--model", str(source), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
                   "--width", "256", "--height", "192", "--msaa", str(samples), "--no-taa", "--no-bloom", "--no-ddgi",
                   "--gltf-camera-node", str(camera_node), "--render-technique", technique, "--display-mode", "lit",
                   "--ray-shadows", "hard", "--no-ray-shadow-denoise", "--environment-intensity", "0",
                   "--directional-intensity", "2", "--directional-direction", "1", "0", "-1",
                   "--directional-color", "1", "1", "1", "--path-spp", "8", "--path-samples", "8", "--path-bounces", "1",
                   "--capture-sequence", str(sequence), "--screenshot", str(image)]
        if enable:
            command.append(FLAG)
        if save is not None:
            command += ["--save-scene", str(save)]
        log = stem.with_suffix(".log")
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=240, env=env)
        text = log.read_text(encoding="utf-8", errors="replace")
        require(not validation_log_error(text), f"Vulkan diagnostics in {log}")
        verify_layer(text, VALIDATION, validation_library)
        if profiled:
            verify_layer(text, PROFILES, profiles_library)
            require(f"Overriding device capabilities with the '{PROFILE_NAME}' profile capabilities." in text and
                    "simulate_capabilities: SIMULATE_FEATURES_BIT" in text and "PROFILES ERROR:" not in text,
                    f"Vulkan Profiles did not apply the feature override: {log}")
        else:
            require(f'layer "{PROFILES}"' not in text, f"Unexpected Vulkan Profiles layer: {log}")
        images, states, captures, telemetry_rows = [], [], [], []
        for frame in range(1, count + 1):
            current = stem if frame == count else work / f"{name}.frame-{frame:04d}"
            image_path = Path(f"{current}{suffix}")
            telemetry_path = Path(f"{current}.telemetry.json")
            metadata_path = Path(f"{image_path}.capture.json")
            images.append(pixels(image_path))
            telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
            telemetry_rows.append(telemetry)
            require(telemetry["validationEnabled"] and telemetry["technique"] == technique
                    and telemetry["msaaSamples"] == samples,
                    f"Unexpected technique or validation state: {telemetry_path}")
            states.append(telemetry["rayShadows"])
            if technique == "path-tracing":
                path = telemetry["referencePath"]
                assert path["attemptedSamples"] == 8 and not path["unsupportedSamples"] and not path["rejectedSamples"], path
            captures.append({"frame": frame, "artifactsSha256": {
                item.relative_to(output).as_posix(): sha(item) for item in (image_path, telemetry_path, metadata_path)}})
        report["runs"][name] = {"rayShadows": states, "command": command, "build": telemetry["build"],
                                "log": log.relative_to(output).as_posix(), "logSha256": sha(log), "captures": captures}
        return images, states, text, telemetry_rows

    events = [{"frame": 2, "materialEdit": {"sourceMaterial": 1, "values": [{"property": "alphaCutoff", "value": 0}]}},
              {"frame": 3, "sceneHistory": "undo"}, {"frame": 4, "sectionPlane": [0, 0, -1, .5]}]

    def check_sampled_alpha_parity():
        baseline, states, _, _ = capture("reference-baseline", model, enable=False, events=events)
        verify_capability(states, requested=False, device_enabled=False)
        saved = output / "portable.glb"
        accelerated, states, _, _ = capture("reference-micromaps", model, enable=True, events=events, save=saved)
        verify_capability(states, requested=True, device_enabled=True)
        require([state["build"]["opacityMicromapBlas"] for state in states] == [1, 0, 1, 1], states)
        require(states[0]["build"]["opacityMicromapBytes"] > 0, states[0])
        for raw, accelerated_image in zip(baseline, accelerated, strict=True):
            np.testing.assert_array_equal(raw, accelerated_image)
        require(np.count_nonzero(accelerated[0] != accelerated[1]) > 100, "The alpha cutoff edit had no visible effect")
        require(np.count_nonzero(accelerated[0] != accelerated[3]) > 100, "The section plane had no visible effect")
        np.testing.assert_array_equal(accelerated[0], accelerated[2])

        fallback, bad_states, bad_log, _ = capture("malformed-fallback", malformed, enable=True)
        verify_capability(bad_states, requested=True, device_enabled=True)
        require("opacity_micromap_fallback" in bad_log and bad_states[0]["build"]["opacityMicromapBlas"] == 0,
                "Malformed authored data was not diagnosed and rejected")
        np.testing.assert_array_equal(fallback[0], baseline[0])
        portable, _ = load_asset(saved)
        require(EXTENSION not in portable.get("extensionsUsed", []) and EXTENSION not in portable.get("extensions", {})
                and all(EXTENSION not in prim.get("extensions", {}) for mesh in portable["meshes"]
                        for prim in mesh["primitives"]), "Portable GLB retained authored micromap data")
        saved_camera, = (index for index, node in enumerate(portable["nodes"]) if "camera" in node)
        reloaded, reload_states, _, _ = capture("portable-reload", saved, enable=True, camera_node=saved_camera)
        verify_capability(reload_states, requested=True, device_enabled=True)
        require(reload_states[0]["build"]["opacityMicromapBlas"] == 0, reload_states)
        np.testing.assert_array_equal(reloaded[0], baseline[0])
        report["portableGlbSha256"] = sha(saved)

        controls = [{"frame": 2, "sectionPlane": [0, 0, -1, .5]}]
        for technique, samples, count in (("deferred-raster", 1, 1), ("forward-raster", 1, 1), ("forward-raster", 4, 0)):
            name = f"{technique}-1x" if technique == "forward-raster" and samples == 1 else technique
            raw, states, _, _ = capture(f"{name}-baseline", model, enable=False, technique=technique, events=controls, samples=samples)
            verify_capability(states, requested=False, device_enabled=False)
            accelerated, states, _, _ = capture(f"{name}-micromaps", model, enable=True, technique=technique, events=controls, samples=samples)
            verify_capability(states, requested=True, device_enabled=True)
            require([state["build"]["opacityMicromapBlas"] for state in states] == [count, count], states)
            require(np.count_nonzero(raw[0] != raw[1]) > 100, f"The section plane had no visible effect in {name}")
            for reference, actual in zip(raw, accelerated, strict=True):
                np.testing.assert_array_equal(reference, actual)

        if profiles_dir is not None:
            profiled = output / "simulated-unavailable"
            profiled.mkdir()
            profiled_env = layer_environment(profiled, validation_dir, profiles_dir)
            fallback, states, _, _ = capture("feature-unavailable", model, enable=True, work=profiled,
                                             env=profiled_env, profiled=True)
            verify_capability(states, requested=True, device_enabled=False)
            np.testing.assert_array_equal(fallback[0], baseline[0])
            report["unsupportedDeviceSimulated"] = True
            report["profileSha256"] = sha(profiled / f"{PROFILE_NAME}.json")

    try:
        if not args.merge_only:
            check_sampled_alpha_parity()
        report["mergeRoundTrip"] = check_merge_round_trip(capture, model, incoming)
        require(sha(renderer) == report["rendererSha256"], "Renderer executable changed during the run")
        report["captureCount"] = sum(len(run["captures"]) for run in report["runs"].values())
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS opacity micromap merge history" if args.merge_only else
          "PASS authored opacity micromaps, alpha edits, clipping, portable fallback, and merge history", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
