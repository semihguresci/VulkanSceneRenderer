"""Verify asymmetric photometry through the native Vulkan raster and save paths."""

import argparse
import base64
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset, finite_json, load_asset, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error


IES = "EXT_lights_ies"
PUNCTUAL = "KHR_lights_punctual"
SIZE = 320
PROFILE = b"IESNA:LM-63-2002\n[TEST] Asymmetric fixture\nTILT=NONE\n1 -1 1 3 2 1 2 0 0 0\n1 1 12\n0 45 90\n0 90\n64 64 0\n1 1 0\n"
LEGACY_PROFILE = b"IESNA:LM-63-1995\n[TEST] Reserved field fixture\nTILT=NONE\n1 -1 1 3 2 1 2 0 0 0\n1 .25 12\n0 45 90\n0 90\n64 64 0\n1 1 0\n"
FULL_PROFILE = (b"IESNA:LM-63-2002\n[TEST] Signed azimuth fixture\nTILT=NONE\n"
                b"1 -1 1 3 5 1 2 0 0 0\n1 1 12\n0 45 90\n0 90 180 270 360\n"
                b"64 64 0\n16 16 0\n8 8 0\n32 32 0\n64 64 0\n")
HDR_ABSOLUTE_TOLERANCE = .001
HDR_RELATIVE_TOLERANCE = .003
PREVIEW_SAMPLES = {
    13: "initial", 19: "preview-half", 25: "preview-zero", 31: "cancel",
    37: "commit", 43: "undo", 49: "redo", 55: "source-node-transform-only",
}
PROFILE_RELOAD_SAMPLES = {
    4: "first", 10: "second", 16: "third", 22: "fourth", 28: "fifth", 34: "first-reopened",
}
PROFILE_MERGE_SAMPLES = {4: "before", 13: "merged", 22: "undone", 31: "redone"}
PROFILES_PER_RELOAD = 64


def fixture(path, *, attachment, rotation=0, transparent=False, multiplier=1, profile=PROFILE, spot=False):
    asset = Asset()
    model = asset.model
    corners = [[-3, -3, 0], [3, -3, 0], [3, 3, 0], [-3, 3, 0]]
    model["meshes"] = [{"primitives": [{"attributes": {
        "POSITION": asset.accessor("VEC3", corners),
        "NORMAL": asset.accessor("VEC3", [[0, 0, 1]] * 4)},
        "indices": asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True), "material": 0}]}]
    model["materials"] = [{"alphaMode": "BLEND" if transparent else "OPAQUE",
        "pbrMetallicRoughness": {"baseColorFactor": [.4, .4, .4, .7 if transparent else 1],
                                "metallicFactor": 0, "roughnessFactor": 1}}]
    model["cameras"] = [{"type": "orthographic", "orthographic": {"xmag": 3, "ymag": 3, "znear": .1, "zfar": 30}}]
    model["nodes"] = [{"mesh": 0, "name": "Diffuse receiver"},
                      {"name": "Measured fixture", "translation": [0, 0, 3],
                       "rotation": [0, 0, math.sin(rotation / 2), math.cos(rotation / 2)]},
                      {"camera": 0, "translation": [0, 0, 5]}]
    model["scenes"] = [{"nodes": [0, 1, 2]}]
    model["extensions"], model["extensionsUsed"] = {}, []
    light = model["nodes"][1].setdefault("extensions", {})
    if spot or attachment in {"punctual", "invalid"}:
        model["extensionsUsed"].append(PUNCTUAL)
        punctual = {"type": "point", "intensity": 16.0}
        if spot:
            punctual.update(type="spot", spot={"innerConeAngle": math.radians(5), "outerConeAngle": math.radians(10)})
        model["extensions"][PUNCTUAL] = {"lights": [punctual]}
        light[PUNCTUAL] = {"light": 0}
    if attachment != "punctual":
        model["extensionsUsed"].append(IES)
        if attachment == "buffer-view":
            source = {"bufferView": asset.view(profile), "mimeType": "application/x-ies-lm-63"}
        else:
            source = {"uri": "asymmetric.ies" if attachment == "external" else "bad.ies"}
        model["extensions"][IES] = {"lights": [source]}
        light[IES] = {"light": 0, "multiplier": multiplier}
    asset.write(path)


def write_light_sequence(path):
    target = {"type": "point", "source": "imported", "sourceNode": 1}

    def edit(frame, phase, intensity=None):
        request = {"target": target, "phase": phase}
        if phase != "commit":
            request["gesture"] = 101
        if intensity is not None:
            request["values"] = {"intensity": intensity}
        return {"frame": frame, "lightEdit": request}

    events = [
        {"frame": 1, "lightSelection": target},
        edit(14, "begin", .5),
        edit(20, "update", 0),
        edit(26, "cancel"),
        edit(32, "commit", .5),
        {"frame": 38, "sceneHistory": "undo"},
        {"frame": 44, "sceneHistory": "redo"},
        {"frame": 50, "expectLightEditFailure": True,
         "lightEdit": {"target": target, "phase": "commit",
                       "gizmo": {"tool": "translate", "axis": "x", "amount": [1, 0, 0]}}},
    ]
    path.write_text(json.dumps({"schemaVersion": 1, "frames": max(PREVIEW_SAMPLES),
                               "sampleFrames": list(PREVIEW_SAMPLES), "events": events},
                               indent=2) + "\n", encoding="utf-8")


def write_profile_reload_sequence(directory, source):
    # Successive assets cross the 256-entry cache limit together, while each
    # individually fits. Distinct candela values prevent cross-asset aliasing.
    models = []
    for group in range(5):
        model = json.loads(source.read_bytes())
        profiles = []
        for slot in range(PROFILES_PER_RELOAD):
            gain = 1 + group + slot / PROFILES_PER_RELOAD
            text = PROFILE.replace(b"64 64 0\n1 1 0",
                                   f"{64 * gain:g} {64 * gain:g} 0\n{gain:g} {gain:g} 0".encode("ascii"))
            profiles.append({"uri": "data:application/x-ies-lm-63;base64," + base64.b64encode(text).decode("ascii")})
        model["extensions"][IES]["lights"] = profiles
        model["nodes"][1]["rotation"] = [0, 0, math.sqrt(.5), math.sqrt(.5)] if group % 2 else [0, 0, 0, 1]
        for slot in range(1, PROFILES_PER_RELOAD):
            model["scenes"][0]["nodes"].append(len(model["nodes"]))
            model["nodes"].append({"translation": [0, 0, 3],
                                   "extensions": {IES: {"light": slot, "multiplier": 0}}})
        path = directory / f"measured-profile-set-{group}.gltf"
        path.write_text(json.dumps(model), encoding="utf-8")
        models.append(path)
    events = []
    for index, model in enumerate([*models[1:], models[0]], start=1):
        events.extend(({"frame": 1 + 6 * index, "reload": str(model)},
                       {"frame": 2 + 6 * index, "cameraSelection": {"sourceNode": 2}}))
    sequence = directory / "profile-reload-history.json"
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": max(PROFILE_RELOAD_SAMPLES),
                                    "sampleFrames": list(PROFILE_RELOAD_SAMPLES), "events": events}),
                        encoding="utf-8")
    return models[0], sequence


def write_profile_merge_sequence(directory):
    fixture_path = directory / "additional-measured-light.gltf"
    profile = PROFILE.replace(b"64 64 0", b"160 160 0")
    uri = "data:application/x-ies-lm-63;base64," + base64.b64encode(profile).decode("ascii")
    fixture_path.write_text(json.dumps({
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Additional measured fixture", "translation": [0, 0, 3],
                   "rotation": [0, 0, math.sqrt(.5), math.sqrt(.5)], "extensions": {IES: {"light": 0}}}],
        "extensionsUsed": [IES], "extensions": {IES: {"lights": [{"uri": uri}]}},
    }), encoding="utf-8")
    sequence = directory / "profile-merge-history.json"
    sequence.write_text(json.dumps({
        "schemaVersion": 1, "frames": max(PROFILE_MERGE_SAMPLES), "sampleFrames": list(PROFILE_MERGE_SAMPLES),
        "events": [
            {"frame": 5, "guiWaitForLoad": True, "merge": {"path": str(fixture_path), "parentName": "Measured rig"}},
            {"frame": 14, "sceneHistory": "undo"},
            {"frame": 23, "sceneHistory": "redo"},
        ],
    }), encoding="utf-8")
    return sequence


def verify_preview_state(state, name, multiplier, gesture, publications, commits, replays, undo, redo):
    edits = state["lightEdits"]
    expected_error = ("Move or rotate the IES fixture through its source node"
                      if name == "source-node-transform-only" else "")
    require(not edits["truncated"] and edits["error"] == expected_error,
            f"Wrong IES light edit diagnostic in {name}: {edits['error']!r}")
    lights = [light for light in edits["lights"] if light["source"] == 1]
    require(len(lights) == 1 and state["lighting"]["pointLightCount"] == 1,
            f"Wrong imported light count in {name}")
    light = lights[0]
    require(light["type"] == 1 and light["sourceNode"] == 1 and light["extension"] == IES and light["selected"],
            f"Wrong selected IES fixture in {name}")
    require(light["range"] == 0 and light["intensity"] == multiplier and light["color"] == [1, 1, 1],
            f"Wrong IES light values in {name}")
    require(np.allclose(light["direction"], [0, 0, -1], atol=1e-6) and
            np.allclose(light["position"], [0, 0, 3], atol=1e-6), f"IES transform changed in {name}")
    require([edits[key] for key in ("gesture", "publications", "commits", "replays")] ==
            [gesture, publications, commits, replays], f"Wrong IES edit state in {name}")
    require(state.get("sceneEditHistory", {"undo": 0, "redo": 0}) == {"undo": undo, "redo": redo},
            f"Wrong IES edit history in {name}")


def verify_saved_profile(model):
    saved_profile = model["extensions"][IES]["lights"][0]["uri"]
    require(saved_profile.startswith("data:application/x-ies-lm-63;base64,"),
            "Portable export did not embed its IES profile")
    require(base64.b64decode(saved_profile.split(",", 1)[1], validate=True) == PROFILE,
            "Portable export changed the original IES profile bytes")


def image_pixels(path):
    image = cv2.imdecode(np.frombuffer(path.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
    require(image is not None and image.ndim == 3 and image.dtype == np.float32 and np.isfinite(image).all() and
            image.shape[:2] == (SIZE, SIZE) and image.shape[2] >= 3, "Invalid scene-linear EXR " + str(path))
    return image[..., :3]


def axis_luminance(image):
    center, distance, radius = SIZE // 2, 76, 4
    positive_x = image[center - radius:center + radius, center + distance - radius:center + distance + radius]
    positive_y = image[center - distance - radius:center - distance + radius, center - radius:center + radius]
    return [float(positive_x.mean()), float(positive_y.mean())]


def angular_intensity_ratio(image, *, rotated=False):
    # These pixel centres have world positions (1.434375, .009375, 0)
    # and (.009375, 1.434375, 0) under the fixture's orthographic camera.
    # Equal distances and isotropic receiver BRDFs cancel in their ratio.
    # Both vertical angles lie below 45 degrees, where each C-plane is flat.
    fraction = math.atan2(.5, 76.5) / (math.pi / 2)
    expected = (64 * (1 - fraction) + fraction) / (64 * fraction + 1 - fraction)
    x_value, y_value = float(image[159, 236].mean()), float(image[83, 160].mean())
    bright, dark = (y_value, x_value) if rotated else (x_value, y_value)
    require(bright > .02 and dark > 0, "Missing direct IES photometry samples")
    actual = bright / dark
    require(math.isclose(actual, expected, rel_tol=.005),
            f"Wrong interpolated IES angular ratio: {actual}, expected {expected}")
    return {"measuredRatio": actual, "expectedRatio": expected, "relativeTolerance": .005}


def full_profile_response(image, quarter_turns):
    # Successive 90-degree rotations of the same pixel centre keep the incident
    # angle, distance and BRDF fixed. Each azimuth is just past a measured plane.
    plane_candela = np.array([64., 16., 8., 32.])
    fraction = math.atan2(.5, 76.5) / (math.pi / 2)
    expected = np.roll(plane_candela * (1 - fraction) + np.roll(plane_candela, -1) * fraction, quarter_turns)
    measured = np.array([image[y, x].mean() for y, x in ((159, 236), (83, 159), (160, 83), (236, 160))])
    require(np.min(measured) > 0, "Full IES profile did not illuminate every sample")
    expected /= expected.max()
    measured /= measured.max()
    require(np.allclose(measured, expected, rtol=.005, atol=0),
            f"Signed IES azimuth mismatch: {measured}, expected {expected}")
    return {"measured": measured.tolist(), "expected": expected.tolist()}


def image_agreement(actual, expected, *, exact=False):
    require(actual.shape == expected.shape == (SIZE, SIZE, 3) and
            np.isfinite(actual).all() and np.isfinite(expected).all(), "Wrong IES comparison image")
    error = np.abs(actual - expected)
    matches = np.array_equal(actual, expected) if exact else np.allclose(
        actual, expected, atol=HDR_ABSOLUTE_TOLERANCE, rtol=HDR_RELATIVE_TOLERANCE)
    require(matches, f"IES HDR comparison failed, maximum absolute error {error.max()}")
    return {"maximumAbsoluteError": float(error.max()), "meanAbsoluteError": float(error.mean()),
            "rootMeanSquareError": float(np.sqrt(np.mean(np.square(error)))), "exact": exact}


def runtime_inputs(renderer):
    directory = Path(__file__).resolve().parent
    helpers = {Path(module.__file__).resolve() for module in tuple(sys.modules.values())
               if getattr(module, "__file__", None) and Path(module.__file__).resolve().parent == directory}
    return {renderer, *renderer.parent.glob("*.dll"), *(renderer.parent / "spv_shaders").glob("*.spv"), *helpers}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Keep each run in a fresh output subdirectory")
    parser.add_argument("--require-opt-in", action="store_true", help="Require CONTAINER_RUN_GPU_IES=1 for native runs")
    parser.add_argument("--reload-only", action="store_true", help="Run only the repeated profile replacement captures")
    args = parser.parse_args()
    if args.require_opt_in and not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_IES") != "1":
        print("Set CONTAINER_RUN_GPU_IES=1 to run native IES lighting validation.", flush=True)
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    print("IES fixtures and captures " + str(output), flush=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    source_profile = output / "asymmetric.ies"
    source_profile.write_bytes(PROFILE)
    (output / "bad.ies").write_bytes(PROFILE.replace(b"TILT=NONE", b"TILT=missing.ies"))
    source, rotated, blend, punctual, invalid, dimmed, unlit, legacy = [output / (name + ".gltf") for name in
        ("source", "rotated", "blend", "punctual", "invalid", "dimmed", "unlit", "legacy-1995")]
    fixture(source, attachment="external")
    fixture(rotated, attachment="buffer-view", rotation=math.pi / 2)
    fixture(blend, attachment="buffer-view", transparent=True)
    fixture(punctual, attachment="punctual")
    fixture(invalid, attachment="invalid")
    fixture(dimmed, attachment="buffer-view", multiplier=.5)
    fixture(unlit, attachment="buffer-view", multiplier=0)
    fixture(legacy, attachment="buffer-view", profile=LEGACY_PROFILE)
    spot, invalid_spot, measured_spot = [output / (name + ".gltf") for name in
        ("spot", "invalid-spot", "measured-spot")]
    fixture(spot, attachment="punctual", spot=True)
    fixture(invalid_spot, attachment="invalid", spot=True)
    fixture(measured_spot, attachment="buffer-view", spot=True)
    signed_fixtures = {}
    for quarter_turns in (0, 1):
        signed = output / f"full-profile-{quarter_turns}.gltf"
        fixture(signed, attachment="buffer-view", profile=FULL_PROFILE, rotation=quarter_turns * math.pi / 2)
        signed_fixtures[quarter_turns] = signed
    light_sequence = output / "light-history.json"
    write_light_sequence(light_sequence)
    reload_source, reload_sequence = write_profile_reload_sequence(output, source)
    merge_sequence = write_profile_merge_sequence(output)
    fixtures = (set(output.glob("*.gltf")) | set(output.glob("*.ies")) |
                {output / "vk_layer_settings.txt", light_sequence, reload_sequence, merge_sequence})
    captures = {}
    report = {"passed": None if args.generate_only else False, "generatedOnly": args.generate_only,
              "rendererExecuted": False, "nativeValidationPerformed": False, "captures": captures,
              "checks": [], "profilePayloadBytes": (4 + 3 + 2 + 6) * 4,
              "fixtureSha256": {str(path): sha(path) for path in sorted(fixtures)}}

    def persist():
        (output / "results.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")

    persist()
    if args.generate_only:
        return 0
    require(renderer.is_file(), "Missing renderer " + str(renderer))
    runtime_files = runtime_inputs(renderer)
    require(any(path.suffix == ".spv" for path in runtime_files), "No resident runtime SPIR-V")
    frozen = {str(path): sha(path) for path in sorted(runtime_files | fixtures)}
    relocated = {}
    exports = {}
    report["inputSha256Before"] = frozen

    def verify():
        require(runtime_inputs(renderer) == runtime_files, "Runtime DLL, SPIR-V or helper inventory changed")
        actual = {}
        for name, expected_hash in frozen.items():
            path = Path(relocated.get(name, name))
            require(path.is_file() and sha(path) == expected_hash, "Frozen native input changed " + str(path))
            actual[str(path)] = expected_hash
        for name, expected_hash in exports.items():
            require(sha(Path(name)) == expected_hash, "Portable IES export changed " + name)
        return actual

    def capture(label, model, technique, *, save=None, camera_node=2, sequence=None, samples=PREVIEW_SAMPLES):
        verify()
        stem = output / label
        image, log, telemetry = stem.with_suffix(".exr"), stem.with_suffix(".log"), stem.with_suffix(".telemetry.json")
        command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
                   "--width", str(SIZE), "--height", str(SIZE), "--msaa", "1", "--no-taa", "--no-bloom", "--no-ddgi",
                   "--gltf-camera-node", str(camera_node), "--render-technique", technique, "--display-mode", "lit",
                   "--ray-shadows", "raster", "--environment-intensity", "0", "--directional-intensity", "0",
                   "--bounce-intensity", "0",
                   "--sky", "plain", "--sky-horizon", "0", "0", "0", "--warmup-frames", "12", "--capture-frame", "13",
                   "--screenshot", str(image)]
        if save:
            command += ["--save-scene", str(save)]
        if sequence:
            command += ["--capture-sequence", str(sequence)]
        report["rendererExecuted"] = True
        persist()
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer"),
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        text = log.read_text(encoding="utf-8", errors="replace")
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text and not validation_log_error(text),
                "Khronos layer loading or strict Vulkan validation failed " + str(log))
        if sequence:
            for sample_frame, sample_name in samples.items():
                sample_image = image if sample_frame == max(samples) else image.with_name(
                    f"{image.stem}.frame-{sample_frame:04d}{image.suffix}")
                inspect_capture(f"{label}-{sample_name}", sample_image, sample_image.with_suffix(".telemetry.json"),
                                technique, log, command)
        else:
            pixels = inspect_capture(label, image, telemetry, technique, log, command)
        verify()
        persist()
        return None if sequence else pixels, text

    def inspect_capture(label, image, telemetry, technique, log, command):
        require(image.is_file() and telemetry.is_file(), "Missing fresh IES capture " + str(image))
        pixels = image_pixels(image)
        frame = json.loads(telemetry.read_text(encoding="utf-8-sig"))
        require(finite_json(frame) and frame["technique"] == technique and frame["resolution"] == [SIZE, SIZE] and
                frame["msaaSamples"] == 1 and not frame["taa"]["enabled"] and
                frame["scene"]["primaryObjectCount"] == 1, "Wrong IES capture configuration")
        require(all(frame["lighting"][key] == 0 for key in
                    ("environmentIntensity", "directionalIntensity", "bounceIntensity")),
                "IES photometry requires isolated direct lighting")
        active = {item["name"]: item for item in frame["passes"] if item["active"]}
        required_pass = "OitResolve" if label.endswith("-transparent") else "Lighting"
        require(required_pass in active and active[required_pass]["gpuTimed"] and
                active[required_pass]["gpuMs"] > 0, "Missing timed IES lighting pass " + required_pass)
        lighting = {item["name"]: item["gpuMs"] for item in frame["passes"]
                    if item["active"] and item["gpuTimed"] and
                    item["name"] in {"Lighting", "ForwardOpaque", "OitResolve", "TileCull"}}
        captures[label] = {"axisLuminance": axis_luminance(pixels), "log": str(log), "image": str(image),
                           "telemetry": str(telemetry), "command": command, "validationClean": True,
                           "lightingPassGpuMilliseconds": lighting,
                           "vmaAllocationBytes": frame["gltfMerge"]["vmaAllocationBytes"],
                           "vmaAllocationCount": frame["gltfMerge"]["vmaAllocationCount"],
                           "build": frame["build"]}
        return pixels

    def verify_profile_reloads():
        for technique in ("deferred-raster", "forward-raster"):
            label = technique + "-profile-reload"
            _, log = capture(label, reload_source, technique, sequence=reload_sequence, samples=PROFILE_RELOAD_SAMPLES)
            counts = []
            first_pixels = None
            reference_luminance = None
            for index, name in enumerate(PROFILE_RELOAD_SAMPLES.values()):
                sample = captures[f"{label}-{name}"]
                state = json.loads(Path(sample["telemetry"]).read_text(encoding="utf-8-sig"))
                require(state["lighting"]["pointLightCount"] == PROFILES_PER_RELOAD,
                        f"Measured fixtures went missing during {name} in {technique}")
                pixels = image_pixels(Path(sample["image"]))
                angle = angular_intensity_ratio(pixels, rotated=name in {"second", "fourth"})
                # x and y have equal magnitude at this pixel, making the
                # quadrant-symmetric profile's value independent of the roll.
                luminance = float(pixels[159, 160].mean())
                if index == 0:
                    first_pixels = pixels
                    reference_luminance = luminance
                expected_luminance = reference_luminance * (index + 1 if index < 5 else 1)
                require(math.isclose(luminance, expected_luminance, rel_tol=.005),
                        f"Stale IES intensity during {name} in {technique}: {luminance}, expected {expected_luminance}")
                if name == "first-reopened":
                    image_agreement(pixels, first_pixels, exact=True)
                counts.append(sample["vmaAllocationCount"])
                report["checks"].append({"check": "profile-scene-replacement", "technique": technique,
                                         "sample": name, "angularIntensity": angle, "luminance": luminance,
                                         "expectedLuminance": expected_luminance})
            require(max(counts) - min(counts) < PROFILES_PER_RELOAD,
                    f"Retired IES buffers accumulated across replacements in {technique}: {counts}")
            require("IES profile cache exceeded its resident" not in log, f"Unexpected IES fallback in {technique}")
            report.setdefault("replacementVmaAllocationCounts", {})[technique] = counts

    def verify_profile_merge_history():
        label = "deferred-raster-profile-merge"
        capture(label, source, "deferred-raster", sequence=merge_sequence, samples=PROFILE_MERGE_SAMPLES)
        images = {}
        for stage, count in (("before", 1), ("merged", 2), ("undone", 1), ("redone", 2)):
            sample = captures[f"{label}-{stage}"]
            state = json.loads(Path(sample["telemetry"]).read_text(encoding="utf-8-sig"))
            require(state["lighting"]["pointLightCount"] == count, f"Wrong measured light count after {stage}")
            images[stage] = image_pixels(Path(sample["image"]))
        require(axis_luminance(images["merged"])[1] > 2 * axis_luminance(images["before"])[0],
                "The merged profile did not light its expected axis")
        report["checks"].append({"check": "profile-merge-undo",
                                 **image_agreement(images["undone"], images["before"], exact=True)})
        report["checks"].append({"check": "profile-merge-redo",
                                 **image_agreement(images["redone"], images["merged"], exact=True)})

    try:
        verify_profile_reloads()
        if args.reload_only:
            report["inputSha256After"] = verify()
            report.update(passed=True, nativeValidationPerformed=True)
            print(f"PASS IES profile retirement and replacement in {len(captures)} captures")
            return 0
        verify_profile_merge_history()
        saved = output / "portable.glb"
        for technique in ("deferred-raster", "forward-raster"):
            original, _ = capture(technique + "-measured", source, technique,
                                  save=saved if technique == "deferred-raster" else None)
            if technique == "deferred-raster":
                require(saved.is_file(), "Missing portable IES export")
                exports[str(saved)] = sha(saved)
            turned, _ = capture(technique + "-rotated", rotated, technique)
            alpha, _ = capture(technique + "-transparent", blend, technique)
            for label, pixels, bright, dark in (("measured", original, 0, 1), ("rotated", turned, 1, 0),
                                               ("transparent", alpha, 0, 1)):
                axes = axis_luminance(pixels)
                require(axes[bright] > .02 and axes[bright] > 10 * axes[dark], "Wrong IES beam " + str((technique, axes)))
                report["checks"].append({"check": "asymmetric-beam", "technique": technique, "fixture": label,
                                         "axisLuminance": axes,
                                         "angularIntensity": angular_intensity_ratio(pixels, rotated=label == "rotated")})
            legacy_image, _ = capture(technique + "-legacy-1995", legacy, technique)
            report["checks"].append({"check": "1995-reserved-field", "technique": technique,
                                     **image_agreement(legacy_image, original, exact=True)})

        for label in ("measured", "rotated", "transparent"):
            forward = image_pixels(output / ("forward-raster-" + label + ".exr"))
            deferred = image_pixels(output / ("deferred-raster-" + label + ".exr"))
            report["checks"].append({"check": "cross-technique-agreement", "fixture": label,
                                     "absoluteTolerance": HDR_ABSOLUTE_TOLERANCE,
                                     "relativeTolerance": HDR_RELATIVE_TOLERANCE,
                                     **image_agreement(forward, deferred)})

        for quarter_turns, model in signed_fixtures.items():
            label = f"full-profile-{quarter_turns}"
            deferred, _ = capture("deferred-raster-" + label, model, "deferred-raster")
            forward, _ = capture("forward-raster-" + label, model, "forward-raster")
            for technique, pixels in (("deferred-raster", deferred), ("forward-raster", forward)):
                report["checks"].append({"check": "signed-profile-orientation", "technique": technique,
                                         "quarterTurns": quarter_turns, **full_profile_response(pixels, quarter_turns)})
            report["checks"].append({"check": "cross-technique-agreement", "fixture": label,
                                     **image_agreement(forward, deferred)})

        reduced, _ = capture("half-output", dimmed, "deferred-raster")
        full = image_pixels(output / "deferred-raster-measured.exr")
        baseline, _ = capture("zero-output", unlit, "deferred-raster")
        expected = baseline + (full - baseline) * .5
        multiplier = image_agreement(reduced, expected)
        report["multiplierMaxAbsoluteError"] = multiplier["maximumAbsoluteError"]
        report["checks"].append({"check": "linear-instance-multiplier", **multiplier})
        fallback, _ = capture("punctual-control", punctual, "deferred-raster")
        rejected, warning = capture("invalid-fallback", invalid, "deferred-raster")
        require("Only TILT=NONE is supported" in warning, "Missing unsupported TILT diagnostic")
        report["checks"].append({"check": "unsupported-optional-fallback", **image_agreement(fallback, rejected, exact=True)})
        measured = captures["deferred-raster-measured"]
        control = captures["punctual-control"]
        report["memory"] = {"packedProfilePayloadBytes": report["profilePayloadBytes"],
                            "measuredMinusPunctualVmaBytes": measured["vmaAllocationBytes"] - control["vmaAllocationBytes"],
                            "measuredMinusPunctualVmaAllocations": measured["vmaAllocationCount"] - control["vmaAllocationCount"],
                            "evidence": "Packed payload arithmetic and observed whole-renderer VMA totals"}

        for technique in ("deferred-raster", "forward-raster"):
            original = image_pixels(Path(captures[technique + "-measured"]["image"]))
            control_spot, _ = capture(technique + "-spot", spot, technique)
            fallback_spot, warning = capture(technique + "-invalid-spot", invalid_spot, technique)
            measured_with_spot, _ = capture(technique + "-measured-spot", measured_spot, technique)
            require(float(control_spot[158:162, 158:162].mean()) > .05 and max(axis_luminance(control_spot)) < 1e-5,
                    "Spot control did not light the centre while excluding the measured off-axis samples")
            require("Only TILT=NONE is supported" in warning, "Missing invalid IES spot fallback diagnostic")
            report["checks"].append({"check": "spot-fallback", "technique": technique,
                                     **image_agreement(fallback_spot, control_spot, exact=True)})
            report["checks"].append({"check": "ies-supersedes-companion-spot", "technique": technique,
                                     **image_agreement(measured_with_spot, original, exact=True)})

            if technique == "deferred-raster":
                half, zero = reduced, baseline
            else:
                half, _ = capture(technique + "-half-output", dimmed, technique)
                zero, _ = capture(technique + "-zero-output", unlit, technique)
            edited = output / (technique + "-edited.glb")
            history_label = technique + "-light-history"
            capture(history_label, measured_spot, technique, save=edited, sequence=light_sequence)
            preview_expectations = [
                ("initial", original, 1, 0, 0, 0, 0, 0, 0),
                ("preview-half", half, .5, 101, 1, 0, 0, 0, 0),
                ("preview-zero", zero, 0, 101, 2, 0, 0, 0, 0),
                ("cancel", original, 1, 0, 3, 0, 0, 0, 0),
                ("commit", half, .5, 0, 4, 1, 0, 1, 0),
                ("undo", original, 1, 0, 5, 1, 1, 0, 1),
                ("redo", half, .5, 0, 6, 1, 2, 1, 0),
                ("source-node-transform-only", half, .5, 0, 6, 1, 2, 1, 0),
            ]
            for sample_name, expected_image, *expected_state in preview_expectations:
                sample = captures[f"{history_label}-{sample_name}"]
                state = json.loads(Path(sample["telemetry"]).read_text(encoding="utf-8-sig"))
                verify_preview_state(state, sample_name, *expected_state)
                report["checks"].append({"check": "editor-photometric-preview", "technique": technique,
                                         "sample": sample_name, "multiplier": expected_state[0],
                                         **image_agreement(image_pixels(Path(sample["image"])), expected_image, exact=True)})
            exports[str(edited)] = sha(edited)
            edited_model, _ = load_asset(edited)
            verify_saved_profile(edited_model)
            edited_lights = [node["extensions"] for node in edited_model["nodes"]
                             if IES in node.get("extensions", {})]
            require(len(edited_lights) == 1 and edited_lights[0][IES]["light"] == 0 and
                    edited_lights[0][IES]["multiplier"] == .5,
                    "Saved IES fixture lost the committed multiplier")
            edited_spot = edited_model["extensions"][PUNCTUAL]["lights"][edited_lights[0][PUNCTUAL]["light"]]
            spot_model, _ = load_asset(spot)
            require(edited_spot == spot_model["extensions"][PUNCTUAL]["lights"][0],
                    "Saved IES fixture changed the companion spot")
            edited_camera = next(index for index, node in enumerate(edited_model["nodes"]) if "camera" in node)
            edited_pixels, _ = capture(technique + "-edited-reload", edited, technique, camera_node=edited_camera)
            report["checks"].append({"check": "edited-ies-reload", "technique": technique,
                                     **image_agreement(edited_pixels, half, exact=True)})

        saved_model, _ = load_asset(saved)
        verify_saved_profile(saved_model)
        verify()
        moved_profile = output / "moved-source.ies"
        source_profile.rename(moved_profile)
        relocated[str(source_profile)] = str(moved_profile)
        report["relocatedInput"] = {"original": str(source_profile), "current": str(moved_profile),
                                    "sha256": frozen[str(source_profile)]}
        require(not source_profile.exists(), "Portable reload still has the original external IES path")
        camera = next(index for index, node in enumerate(saved_model["nodes"]) if "camera" in node)
        reloaded, _ = capture("portable-reload", saved, "deferred-raster", camera_node=camera)
        report["checks"].append({"check": "portable-reload", **image_agreement(full, reloaded, exact=True)})
        require(len(captures) == 43 + 2 * len(PROFILE_RELOAD_SAMPLES) + len(PROFILE_MERGE_SAMPLES),
                "Incomplete IES capture set")
        report["inputSha256After"] = verify()
        report.update(passed=True, nativeValidationPerformed=True)
    except Exception as error:
        report["failure"] = str(error)
        raise
    finally:
        persist()
    print(f"PASS IES photometry, spot fallback, editor preview and portable save in {len(captures)} captures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
