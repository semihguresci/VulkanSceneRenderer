"""Check manual thin-lens focus against two independently projected stripe targets."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset
from gltf_extended_materials_runtime_regression import validation_log_error
from reference_camera_runtime_regression import world_pose


WIDTH, HEIGHT = 240, 160
TARGETS = ((2.5, 24, 104), (6.5, 136, 216))


def fixture(path, orthographic=False):
    asset = Asset()
    positions, indices = [], []
    for depth, left, right in TARGETS:
        pixel = 2 * (4 if orthographic else depth) * math.tan(math.pi / 8) / HEIGHT
        for x in range(left, right, 16):
            first = len(positions)
            positions.extend([[(px - WIDTH / 2) * pixel, (HEIGHT / 2 - py) * pixel, -depth]
                              for px, py in ((x, 125), (x + 8, 125), (x + 8, 35), (x, 35))])
            indices.extend(first + i for i in (0, 1, 2, 0, 2, 3))
    asset.model["meshes"] = [{"primitives": [{
        "attributes": {"POSITION": asset.accessor("VEC3", positions),
                       "NORMAL": asset.accessor("VEC3", [[0, 0, 1]] * len(positions))},
        "indices": asset.accessor("SCALAR", indices, 5123, indices=True), "material": 0}]}]
    asset.model["materials"] = [{"pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1], "metallicFactor": 0},
                                 "emissiveFactor": [1, 1, 1],
                                 "extensions": {"KHR_materials_specular": {"specularFactor": 0}}}]
    asset.model["extensionsUsed"] = ["KHR_materials_specular"]
    asset.model["cameras"] = [{"type": "perspective", "perspective": {
        "yfov": math.pi / 4, "aspectRatio": WIDTH / HEIGHT, "znear": 0.1, "zfar": 100}}]
    if orthographic:
        half_height = 4 * math.tan(math.pi / 8)
        asset.model["cameras"] = [{"type": "orthographic", "orthographic": {
            "xmag": half_height * WIDTH / HEIGHT, "ymag": half_height, "znear": 0.1, "zfar": 100}}]
    matrix = world_pose(True).T.reshape(-1).tolist()
    asset.model["nodes"] = [{"mesh": 0, "matrix": matrix}, {"camera": 0, "matrix": matrix}]
    asset.model["scenes"] = [{"nodes": [0, 1]}]
    asset.write(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model = output / "focus-targets.gltf"
    fixture(model)
    orthographic = output / "ortho-focus-targets.gltf"
    fixture(orthographic, orthographic=True)

    def capture(name, aperture=None, focus=None, use_ortho=False):
        image = output / f"{name}.exr"
        command = [str(renderer), "--model", str(orthographic if use_ortho else model),
                   "--gltf-camera-node", "1", "--import-scale", "1",
                   "--hidden", "--no-ui", "--validation", "--no-taa", "--no-bloom", "--msaa", "1",
                   "--width", str(WIDTH), "--height", str(HEIGHT), "--directional-intensity", "0",
                   "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
                   "--display-mode", "lit", "--render-technique", "path-tracing", "--path-spp", "8",
                   "--path-samples", "64", "--path-bounces", "1", "--path-seed", "19",
                   "--warmup-frames", "0", "--capture-frame", "8", "--screenshot", str(image)]
        if aperture is not None:
            command += ["--path-aperture", str(aperture), "--path-focus-distance", str(focus)]
        process = subprocess.run(command, cwd=renderer.parent, capture_output=True, text=True, errors="replace",
                                 timeout=180, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        log = process.stdout + process.stderr
        (output / f"{name}.log").write_text(log, encoding="utf-8")
        assert process.returncode == 0 and not validation_log_error(log), log
        meta = json.loads(Path(f"{image}.capture.json").read_text(encoding="utf-8"))["referencePath"]
        assert meta["attemptedSamplesPerPixel"] == 64 and not meta["hasDiagnostics"], meta
        effective_aperture = 0 if use_ortho else aperture or 0
        assert math.isclose(meta["apertureRadius"], effective_aperture, abs_tol=1e-6), meta
        assert meta["focusDistance"] == (focus if effective_aperture else None), meta
        pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        assert pixels is not None and pixels.shape == (HEIGHT, WIDTH, 3), image
        assert pixels.dtype == np.float32 and np.isfinite(pixels).all(), image
        return pixels[:, :, 0]

    pinhole = capture("pinhole")
    np.testing.assert_array_equal(capture("zero-aperture", 0, 6.5), pinhole)
    ortho = capture("orthographic", use_ortho=True)
    np.testing.assert_array_equal(capture("orthographic-aperture", 0.3, 6.5, use_ortho=True), ortho)
    near, far = (capture(name, 0.3, depth) for name, (depth, _, _) in zip(("near-focus", "far-focus"), TARGETS))
    checks = []
    for index, (_, left, right) in enumerate(TARGETS):
        roi = np.s_[48:112, left + 12:right - 12]
        sharp = pinhole[roi]
        focused, blurred = ((near, far) if index == 0 else (far, near))
        assert sharp.min() == 0 and sharp.max() == 1
        focus_error = float(np.abs(focused[roi] - sharp).mean())
        blur_error = float(np.abs(blurred[roi] - sharp).mean())
        assert focus_error < 0.015 and blur_error > 0.20, (index, focus_error, blur_error)
        checks.append({"target": index, "focusError": focus_error, "defocusError": blur_error})
    (output / "results.json").write_text(json.dumps({"passed": True, "targets": checks}, indent=2), encoding="utf-8")
    print("PASS exact zero-aperture parity and independent near/far focus targets", flush=True)


if __name__ == "__main__":
    main()
