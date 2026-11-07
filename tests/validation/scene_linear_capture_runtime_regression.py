"""Validate OpenEXR radiance and display PNG capture on the Vulkan raster paths."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np
from PIL import Image, ImageChops

from display_capture_test_support import emitter_fixture, validation_log_error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    records = []
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    model = output / "radiance.gltf"
    material = {
        "emissiveFactor": [.5, .25, .125],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}},
        "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]},
    }
    opaque_asset = emitter_fixture(model, material)
    blend_model = output / "transparent-radiance.gltf"
    blend_asset = emitter_fixture(blend_model, {
            **material, "alphaMode": "BLEND",
            "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, .5]},
    })
    for path, asset in ((model, opaque_asset), (blend_model, blend_asset)):
        asset["nodes"][0]["translation"] = [0, .4, 0]
        path.write_text(json.dumps(asset), encoding="utf-8")
    for technique in ("deferred-raster", "forward-raster"):
        def capture(name, size, exposure, tone, *flags, ui=False, resize=False, blend=False):
            image = output / f"{technique}-{name}"
            for artifact in (image, Path(str(image) + ".capture.json"), image.with_suffix(".telemetry.json")):
                artifact.unlink(missing_ok=True)
            w, h = size
            initial_w, initial_h = (320, 240) if resize else size
            command = [
                str(renderer), "--hidden", "--validation", "--no-bloom",
                "--width", str(initial_w), "--height", str(initial_h), "--msaa", "1",
                "--warmup-frames", "8", "--capture-frame", "9",
                "--render-technique", technique, "--display-mode", "lit", "--exposure", str(exposure),
                "--tone-map", tone, "--no-ray-query", "--ray-shadows", "raster",
                "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0",
                "--camera-fov", "45", "--import-scale", "1", "--environment-intensity", "0",
                "--directional-intensity", "0", "--model", str(blend_model if blend else model),
                "--screenshot", str(image),
                *flags,
            ]
            if not ui:
                command.append("--no-ui")
            if resize:
                sequence = image.with_suffix(".sequence.json")
                sequence.write_text(json.dumps({"schemaVersion": 1, "frames": 9,
                    "sampleFrames": [9], "events": [{"frame": 9, "resize": [w, h]}]}))
                command += ["--capture-sequence", str(sequence)]
            log = image.with_suffix(image.suffix + ".log")
            with log.open("w", encoding="utf-8") as stream:
                result = subprocess.run(command, cwd=renderer.parent, stdout=stream,
                    stderr=subprocess.STDOUT, timeout=120,
                    env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
            if result.returncode or validation_log_error(log.read_text(encoding="utf-8", errors="replace")):
                raise RuntimeError(f"Capture failed, see {log}")
            metadata = json.loads(Path(str(image) + ".capture.json").read_bytes())
            records.append(dict(image=str(image), log=str(log), command=command, metadata=metadata))
            viewport = "--screenshot-viewport" in flags
            includes_ui = ui and not viewport and image.suffix == ".png"
            if (metadata["width"], metadata["height"], metadata["includesUi"]) != (w, h, includes_ui):
                raise RuntimeError(f"Incorrect capture metadata for {image}")
            if resize:
                telemetry = json.loads(image.with_suffix(".telemetry.json").read_bytes())
                if not telemetry["swapchainRecreates"] or not any(
                    item["name"] == "TemporalResolve" and item["active"]
                    for item in telemetry["passes"]
                ):
                    raise RuntimeError(f"Resize or temporal resolve was not exercised for {image}")
            if image.suffix == ".exr":
                bgr = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
                if bgr is None or bgr.shape != (h, w, 3) or bgr.dtype != np.float32:
                    raise RuntimeError(f"Could not reload float RGB radiance from {image}")
                if (metadata["colorSpace"] != "linear-sRGB" or metadata["exposureApplied"] or
                        metadata["toneMappingApplied"] or metadata["source"] != "raster-scene-composite"):
                    raise RuntimeError(f"Incorrect EXR color metadata for {image}")
                rgb = bgr[:, :, ::-1]
                assert np.isfinite(rgb).all()
                expected = [2, 1, .5] if blend else [4, 2, 1]
                np.testing.assert_allclose(rgb[h // 2, w // 2], expected, atol=.008, rtol=0)
                np.testing.assert_allclose(rgb[h // 4, w // 2], expected, atol=.008, rtol=0)
                np.testing.assert_allclose(rgb[3 * h // 4, w // 2], [0, 0, 0], atol=.008, rtol=0)
                return rgb
            if (metadata["source"] != ("viewport" if viewport else "window") or
                    metadata["manualExposure"] != exposure):
                raise RuntimeError(f"Incorrect display metadata for {image}")
            with Image.open(image) as result_image:
                return result_image.convert("RGB")

        first = capture("linear.exr", (320, 240), 1, "aces", "--no-taa")
        second = capture("changed-\u0131.exr", (320, 240), .25, "reinhard", "--no-taa")
        np.testing.assert_array_equal(first, second)
        capture("taa-wide.exr", (384, 200), .25, "reinhard", "--taa", resize=True)
        capture("transparent.exr", (320, 240), 1, "aces", "--no-taa", blend=True)
        first = capture("viewport.png", (320, 240), 1, "aces", "--no-taa", "--screenshot-viewport")
        second = capture("viewport-changed.png", (320, 240), .25, "reinhard", "--no-taa", "--screenshot-viewport")
        if not ImageChops.difference(first, second).getbbox():
            raise RuntimeError(f"{technique}: display capture ignored grading")
        viewport = capture("viewport-with-ui.png", (640, 480), 1, "aces",
                           "--no-taa", "--screenshot-viewport", ui=True)
        window = capture("window-with-ui.png", (640, 480), 1, "aces", "--no-taa", ui=True)
        if not ImageChops.difference(viewport, window).getbbox():
            raise RuntimeError(f"{technique}: window did not include GUI")
        print(f"{technique}: float RGB round trip and display transform passed", flush=True)

    assert hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_hash
    (output / "results.json").write_text(json.dumps(dict(
        passed=True, rendererSha256=renderer_hash, captures=len(records), records=records,
        analyticEmission=True, blendCoverage=True, resizeWithTaa=True, uiExclusion=True,
    ), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
