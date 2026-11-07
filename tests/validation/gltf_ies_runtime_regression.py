"""Verify asymmetric photometry through the native Vulkan raster and save paths."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset, load_asset
from gltf_extended_materials_runtime_regression import validation_log_error


IES = "EXT_lights_ies"
PUNCTUAL = "KHR_lights_punctual"
SIZE = 320
PROFILE = b"IESNA:LM-63-2002\n[TEST] Asymmetric fixture\nTILT=NONE\n1 -1 1 3 2 1 2 0 0 0\n1 1 12\n0 45 90\n0 90\n64 64 0\n1 1 0\n"


def fixture(path, *, attachment, rotation=0, transparent=False, multiplier=1):
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
            source = {"bufferView": asset.view(PROFILE), "mimeType": "application/x-ies-lm-63"}
        else:
            source = {"uri": "asymmetric.ies" if attachment == "external" else "bad.ies"}
        model["extensions"][IES] = {"lights": [source]}
        light[IES] = {"light": 0, "multiplier": multiplier}
    asset.write(path)


def image_pixels(path):
    image = cv2.imdecode(np.frombuffer(path.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
    assert image is not None and image.dtype == np.float32 and np.isfinite(image).all(), path
    return image[..., :3]


def axis_luminance(image):
    center, distance, radius = SIZE // 2, 76, 4
    positive_x = image[center - radius:center + radius, center + distance - radius:center + distance + radius]
    positive_y = image[center - distance - radius:center - distance + radius, center - radius:center + radius]
    return [float(positive_x.mean()), float(positive_y.mean())]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    source_profile = output / "asymmetric.ies"
    source_profile.write_bytes(PROFILE)
    (output / "bad.ies").write_bytes(PROFILE.replace(b"TILT=NONE", b"TILT=missing.ies"))
    source, rotated, blend, punctual, invalid, dimmed, unlit = [output / (name + ".gltf") for name in
        ("source", "rotated", "blend", "punctual", "invalid", "dimmed", "unlit")]
    fixture(source, attachment="external")
    fixture(rotated, attachment="buffer-view", rotation=math.pi / 2)
    fixture(blend, attachment="buffer-view", transparent=True)
    fixture(punctual, attachment="punctual")
    fixture(invalid, attachment="invalid")
    fixture(dimmed, attachment="buffer-view", multiplier=.5)
    fixture(unlit, attachment="buffer-view", multiplier=0)
    if args.generate_only:
        return

    captures = {}

    def capture(label, model, technique, *, save=None, camera_node=2):
        stem = output / label
        image, log, telemetry = stem.with_suffix(".exr"), stem.with_suffix(".log"), stem.with_suffix(".telemetry.json")
        command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
                   "--width", str(SIZE), "--height", str(SIZE), "--msaa", "1", "--no-taa", "--no-bloom", "--no-ddgi",
                   "--gltf-camera-node", str(camera_node), "--render-technique", technique, "--display-mode", "lit",
                   "--ray-shadows", "raster", "--environment-intensity", "0", "--directional-intensity", "0",
                   "--sky", "plain", "--sky-horizon", "0", "0", "0", "--warmup-frames", "1", "--capture-frame", "2",
                   "--screenshot", str(image)]
        if save:
            command += ["--save-scene", str(save)]
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        text = log.read_text(encoding="utf-8", errors="replace")
        assert not validation_log_error(text), log
        pixels = image_pixels(image)
        frame = json.loads(telemetry.read_text(encoding="utf-8"))
        lighting = {item["name"]: item["gpuMs"] for item in frame["passes"]
                    if item["gpuTimed"] and item["name"] in {"Lighting", "ForwardOpaque", "OitResolve", "TileCull"}}
        captures[label] = {"axisLuminance": axis_luminance(pixels), "log": str(log), "image": str(image),
                           "telemetry": str(telemetry), "lightingPassGpuMilliseconds": lighting}
        return pixels, text

    saved = output / "portable.glb"
    for technique in ("deferred-raster", "forward-raster"):
        original, _ = capture(technique + "-measured", source, technique, save=saved if technique == "deferred-raster" else None)
        turned, _ = capture(technique + "-rotated", rotated, technique)
        alpha, _ = capture(technique + "-transparent", blend, technique)
        for pixels, bright, dark in ((original, 0, 1), (turned, 1, 0), (alpha, 0, 1)):
            axes = axis_luminance(pixels)
            assert axes[bright] > .02 and axes[bright] > 10 * axes[dark], (technique, axes)

    reduced, _ = capture("half-output", dimmed, "deferred-raster")
    full = image_pixels(output / "deferred-raster-measured.exr")
    baseline, _ = capture("zero-output", unlit, "deferred-raster")
    expected = baseline + (full - baseline) * .5
    assert np.allclose(reduced, expected, atol=.001, rtol=.003), np.max(np.abs(reduced - expected))
    fallback, _ = capture("punctual-control", punctual, "deferred-raster")
    rejected, warning = capture("invalid-fallback", invalid, "deferred-raster")
    assert "Only TILT=NONE is supported" in warning, warning
    assert np.array_equal(fallback, rejected)

    saved_model, _ = load_asset(saved)
    assert saved_model["extensions"][IES]["lights"][0]["uri"].startswith("data:application/x-ies-lm-63;base64,")
    source_profile.rename(output / "moved-source.ies")
    camera = next(index for index, node in enumerate(saved_model["nodes"]) if "camera" in node)
    reloaded, _ = capture("portable-reload", saved, "deferred-raster", camera_node=camera)
    assert np.array_equal(full, reloaded), np.max(np.abs(full - reloaded))
    report = {"passed": True, "profilePayloadBytes": (4 + 3 + 2 + 6) * 4,
              "multiplierMaxAbsoluteError": float(np.max(np.abs(reduced - expected))), "captures": captures}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS IES orientation, multiplier, transparency, fallback and portable save in {len(captures)} captures")


if __name__ == "__main__":
    main()
