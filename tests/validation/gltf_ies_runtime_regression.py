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
HDR_ABSOLUTE_TOLERANCE = .001
HDR_RELATIVE_TOLERANCE = .003


def fixture(path, *, attachment, rotation=0, transparent=False, multiplier=1, profile=PROFILE):
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
    if attachment in {"punctual", "invalid"}:
        model["extensionsUsed"].append(PUNCTUAL)
        model["extensions"][PUNCTUAL] = {"lights": [{"type": "point", "intensity": 16.0}]}
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
    fixtures = set(output.glob("*.gltf")) | set(output.glob("*.ies")) | {output / "vk_layer_settings.txt"}
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

    def capture(label, model, technique, *, save=None, camera_node=2):
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
        report["rendererExecuted"] = True
        persist()
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer"),
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        text = log.read_text(encoding="utf-8", errors="replace")
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text and not validation_log_error(text),
                "Khronos layer loading or strict Vulkan validation failed " + str(log))
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
        verify()
        persist()
        return pixels, text

    try:
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

        saved_model, _ = load_asset(saved)
        saved_profile = saved_model["extensions"][IES]["lights"][0]["uri"]
        require(saved_profile.startswith("data:application/x-ies-lm-63;base64,"),
                "Portable export did not embed its IES profile")
        require(base64.b64decode(saved_profile.split(",", 1)[1], validate=True) == PROFILE,
                "Portable export changed the original IES profile bytes")
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
        require(len(captures) == 13, "Incomplete IES capture set")
        report["inputSha256After"] = verify()
        report.update(passed=True, nativeValidationPerformed=True)
    except Exception as error:
        report["failure"] = str(error)
        raise
    finally:
        persist()
    print(f"PASS IES versions, orientation, renderer agreement, multiplier, fallback and portable save in {len(captures)} captures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
