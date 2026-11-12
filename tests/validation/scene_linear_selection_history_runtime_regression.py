"""Verify scene-linear EXR stays free of selection strokes after temporal rendering."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np
from PIL import Image

from display_capture_test_support import Asset, validation_log_error


WIDTH, HEIGHT = 320, 240
CAPTURE_FRAME = 9
TECHNIQUES = ("deferred-raster", "forward-raster")
CHROMA_TOLERANCE = .002


def fixture(path):
    asset = Asset()

    def primitive(rectangles):
        positions, colors, indices = [], [], []
        for left, right, bottom, top, z, gray in rectangles:
            first = len(positions)
            positions.extend(((left, bottom, z), (right, bottom, z),
                              (right, top, z), (left, top, z)))
            colors.extend(((gray, gray, gray),) * 4)
            indices.extend(first + index for index in (0, 1, 2, 0, 2, 3))
        return dict(attributes=dict(
            POSITION=asset.accessor("VEC3", positions),
            NORMAL=asset.accessor("VEC3", [(0, 0, 1)] * len(positions)),
            COLOR_0=asset.accessor("VEC3", colors)),
            indices=asset.accessor("SCALAR", indices, 5123, indices=True), material=0)

    # This one surface has a stable depth/identity underneath the foreground
    # stroke. Its fine grayscale variation also gives a valid history tap room
    # to survive the temporal resolve's current-neighborhood RGB clipping.
    stripe_width = .025
    stripes = [(-3 + stripe_width * index, -3 + stripe_width * (index + 1),
                -3, 3, -1, .9 if index % 2 else .03) for index in range(240)]
    foreground = [(-.63, .63, -.47, .47, 0, .16)]
    asset.model.update(
        scenes=[dict(nodes=[0, 1])],
        nodes=[dict(name="Fullscreen striped backdrop", mesh=0),
               dict(name="Foreground selection", mesh=1)],
        meshes=[dict(primitives=[primitive(stripes)]),
                dict(primitives=[primitive(foreground)])],
        materials=[dict(extensions={"KHR_materials_unlit": {}})],
        extensionsUsed=["KHR_materials_unlit"],
    )
    asset.write(path)


def write_sequence(path, selected):
    path.write_text(json.dumps({"schemaVersion": 1, "frames": CAPTURE_FRAME,
        "sampleFrames": [CAPTURE_FRAME], "events": [{"frame": 1, "nodeGizmo": {
            "operation": "select", "node": {"graphNode": selected}}}]}), encoding="utf-8")


def capture(renderer, output, model, technique, label, sequence, suffix, upscaler):
    image = output / f"{technique}-{label}.{suffix}"
    sidecar = Path(str(image) + ".capture.json")
    telemetry = image.with_suffix(".telemetry.json")
    for artifact in (image, sidecar, telemetry):
        artifact.unlink(missing_ok=True)
    aa = ["--no-taa", f"--{upscaler}", "quality"] if upscaler else ["--taa", "--taa-history-weight", ".98"]
    command = [str(renderer), "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-ddgi",
        "--sky", "plain", "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", *aa,
        "--no-ray-query", "--ray-shadows", "raster",
        "--render-technique", technique, "--display-mode", "lit", "--exposure", "1",
        "--camera-position", "0", "0", "3", "--camera-target", "0", "0", "0",
        "--camera-fov", "45", "--import-scale", "1", "--environment-intensity", "0",
        "--directional-intensity", "0", "--model", str(model),
        "--capture-sequence", str(sequence), "--screenshot", str(image)]
    log = image.with_suffix(image.suffix + ".log")
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, stdout=stream,
            stderr=subprocess.STDOUT, timeout=120,
            env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
    if result.returncode or validation_log_error(log.read_text(encoding="utf-8", errors="replace")):
        raise RuntimeError(f"Capture failed, see {log}")
    state = json.loads(telemetry.read_bytes())
    temporal = state[upscaler]["active"] if upscaler else state["taa"]["enabled"]
    if (state["technique"] != technique or not state["validationEnabled"] or not temporal or
            state["taa"]["submittedFrame"] < CAPTURE_FRAME or
            not any(item["name"] == "TemporalResolve" and item["active"] for item in state["passes"])):
        raise RuntimeError(f"Requested temporal renderer did not execute for {image}")
    if suffix == "png":
        with Image.open(image) as encoded:
            pixels = np.asarray(encoded.convert("RGB"), dtype=np.int16)
    else:
        metadata = json.loads(sidecar.read_bytes())
        if (metadata["source"] != "raster-scene-composite" or metadata["colorSpace"] != "linear-sRGB" or
                metadata["includesUi"] or metadata["exposureApplied"] or metadata["toneMappingApplied"]):
            raise RuntimeError(f"Unexpected scene-linear export metadata for {image}")
        bgr = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        if bgr is None or bgr.dtype != np.float32 or not np.isfinite(bgr).all():
            raise RuntimeError(f"Invalid float RGB export in {image}")
        pixels = bgr[:, :, ::-1]
    if pixels.shape != (HEIGHT, WIDTH, 3):
        raise RuntimeError(f"Unexpected image shape in {image}: {pixels.shape}")
    return pixels


def probe(renderer, output, model, technique, foreground, backdrop, upscaler):
    png = capture(renderer, output, model, technique, "selection", foreground, "png", upscaler)
    stroke = (png[:, :, 0] - png[:, :, 2] > 35) & (png[:, :, 1] - png[:, :, 2] > 15)
    stroke_pixels = np.count_nonzero(stroke)
    selected = capture(renderer, output, model, technique, "foreground-history", foreground, "exr", upscaler)
    control = capture(renderer, output, model, technique, "backdrop-history", backdrop, "exr", upscaler)
    if np.max(np.ptp(control, axis=2)) > CHROMA_TOLERANCE:
        raise RuntimeError(f"{technique}: backdrop selection unexpectedly colored the scene")
    np.testing.assert_allclose(control[HEIGHT // 2, WIDTH // 2], .16, atol=.002, rtol=0)
    if np.ptp(control[HEIGHT // 2, :WIDTH // 4, 0]) < (.1 if upscaler else .3):
        raise RuntimeError(f"{technique}: grayscale backdrop variation is missing")
    chroma = np.ptp(selected, axis=2)
    difference = np.max(np.abs(selected - control), axis=2)
    return dict(technique=technique, displayStrokePixels=int(stroke_pixels),
                contaminatedPixels=int(np.count_nonzero(chroma > CHROMA_TOLERANCE)),
                differingPixels=int(np.count_nonzero(difference > CHROMA_TOLERANCE)),
                maximumChroma=float(chroma.max()), maximumDifference=float(difference.max()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--technique", action="append", choices=TECHNIQUES)
    parser.add_argument("--upscaler", choices=("fsr", "dlss"), help="Exercise an installed optional backend instead of TAA")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True)
    model = output / "grayscale.gltf"
    fixture(model)
    foreground, backdrop = output / "foreground.json", output / "backdrop.json"
    # The scene graph inserts each renderable primitive after its authored
    # transform node. Selection stroke draws belong to those primitive nodes.
    write_sequence(foreground, 3)
    write_sequence(backdrop, 1)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    results = []
    for technique in args.technique or TECHNIQUES:
        result = probe(renderer, output, model, technique, foreground, backdrop, args.upscaler)
        results.append(result)
        print(json.dumps(result), flush=True)
    if hashlib.sha256(renderer.read_bytes()).hexdigest() != renderer_hash:
        raise RuntimeError("Renderer changed during the capture regression")
    witnessed = all(result["displayStrokePixels"] >= 100 for result in results)
    clean = all(not (result["contaminatedPixels"] or result["differingPixels"]) for result in results)
    (output / "results.json").write_text(json.dumps(dict(
        passed=witnessed and clean, rendererSha256=renderer_hash, captures=3 * len(results),
        temporalBackend=args.upscaler or "taa", techniques=results), indent=2) + "\n", encoding="utf-8")
    if not witnessed:
        raise RuntimeError("Missing orange selection stroke in PNG witness")
    if not clean:
        raise RuntimeError("Selection history changed raw scene radiance")


if __name__ == "__main__":
    main()
