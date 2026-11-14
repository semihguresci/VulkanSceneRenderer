"""Validate analytic ground and compositing matte against projected blocker geometry."""

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


WIDTH, HEIGHT = 192, 144


def fixture(path):
    asset = Asset()
    positions = [[-.3, 1, .3], [.3, 1, .3], [.3, 1, -.3], [-.3, 1, -.3]]
    asset.model["meshes"] = [{"primitives": [{
        "attributes": {"POSITION": asset.accessor("VEC3", positions),
                       "NORMAL": asset.accessor("VEC3", [[0, 1, 0]] * 4)},
        "indices": asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True),
        "material": 0}]}]
    asset.model["materials"] = [{
        "doubleSided": True,
        "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1], "metallicFactor": 0},
        "extensions": {"KHR_materials_specular": {"specularFactor": 0}}}]
    asset.model["extensionsUsed"] = ["KHR_materials_specular"]
    asset.model["cameras"] = [{"type": "orthographic", "orthographic": {
        "xmag": 2, "ymag": 1.5, "znear": .1, "zfar": 100}}]
    asset.model["nodes"] = [{"mesh": 0}, {"camera": 0, "translation": [0, 4, 0],
                                         "rotation": [-math.sqrt(.5), 0, 0, math.sqrt(.5)]}]
    asset.model["scenes"] = [{"nodes": [0, 1]}]
    asset.write(path)


def region(image, x):
    center = round(WIDTH / 2 + x * WIDTH / 4)
    return image[HEIGHT // 2 - 4:HEIGHT // 2 + 4, center - 2:center + 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model = output / "blocker.gltf"
    fixture(model)

    def run_capture(name, mode, height=0, intensity=1, direction=(-.6, -.8, 0), *, sequence=None):
        image = output / f"{name}.exr"
        command = [
            str(renderer), "--model", str(model), "--gltf-camera-node", "1", "--import-scale", "1",
            "--hidden", "--no-ui", "--validation", "--no-taa", "--no-bloom", "--msaa", "1",
            "--width", str(WIDTH), "--height", str(HEIGHT), "--sky", "plain", "--sky-horizon", "0", "0", "0",
            "--environment-intensity", "0",
            "--directional-direction", *map(str, direction), "--directional-color", "1", "1", "1",
            "--display-mode", "lit", "--render-technique", "path-tracing", "--path-spp", "8",
            "--path-samples", "16", "--path-bounces", "1",
            "--path-ground", mode, "--path-ground-height", str(height), "--path-ground-color", ".8", ".2", ".1",
            "--screenshot", str(image),
        ]
        if intensity is not None:
            command += ["--directional-intensity", str(intensity)]
        if sequence is None:
            command += ["--capture-frame", "2", "--warmup-frames", "0"]
        else:
            command += ["--capture-sequence", str(sequence)]
        if mode == "shadow-matte":
            command += ["--path-firefly-clamp", ".1"]
        run = subprocess.run(command, cwd=renderer.parent, capture_output=True, text=True, errors="replace",
                             timeout=180, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        log = run.stdout + run.stderr
        (output / f"{name}.log").write_text(log, encoding="utf-8")
        assert run.returncode == 0 and not validation_log_error(log), log
        return image

    def read_capture(image, mode, height, *, samples=16, resets=1, submitted=8):
        metadata = json.loads(Path(f"{image}.capture.json").read_text(encoding="utf-8"))
        settings = metadata["referencePath"]
        assert settings["attemptedSamplesPerPixel"] == samples and not settings["hasDiagnostics"], metadata
        assert settings["sampleLimit"] == 16 and settings["sampleLimitReached"] == (samples == 16), metadata
        assert settings["historyResets"] == resets and settings["lastSubmittedSamples"] == submitted, metadata
        assert settings["ground"]["mode"] == mode, metadata
        if mode != "off":
            assert settings["ground"]["height"] == height, metadata
        if mode == "shadow-matte":
            assert metadata["source"] == "path-trace-shadow-matte" and metadata["colorSpace"] == "linear-mask"
            assert not (metadata["includesUi"] or metadata["exposureApplied"] or metadata["toneMappingApplied"]), metadata
            assert settings["fireflyClamp"] == 0, metadata
        else:
            assert metadata["source"] == "path-trace-mean", metadata
        pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        assert pixels is not None and pixels.shape == (HEIGHT, WIDTH, 3), image
        assert pixels.dtype == np.float32 and np.isfinite(pixels).all(), image
        if mode == "shadow-matte":
            assert (pixels >= 0).all() and (pixels <= 1).all(), image
            np.testing.assert_array_equal(pixels[:, :, 0], pixels[:, :, 1])
            np.testing.assert_array_equal(pixels[:, :, 0], pixels[:, :, 2])
        return pixels[:, :, ::-1]

    def capture(name, mode, height=0, intensity=1, direction=(-.6, -.8, 0)):
        return read_capture(run_capture(name, mode, height, intensity, direction), mode, height)

    off = capture("ground-off", "off")
    np.testing.assert_array_equal(off, 0)
    surface = capture("ground-surface", "surface")
    np.testing.assert_array_equal(region(surface, -.85), 0)
    red, green, blue = region(surface, .85).mean(axis=(0, 1))
    assert red > green * 2 and green > blue > 0, (red, green, blue)

    matte = capture("shadow-matte", "shadow-matte")
    np.testing.assert_array_equal(region(matte, -.85), 1)
    np.testing.assert_array_equal(region(matte, .85), 0)
    np.testing.assert_array_equal(region(matte, 0), 0)
    raised = capture("raised-matte", "shadow-matte", height=.5)
    np.testing.assert_array_equal(region(raised, -.85), 0)
    np.testing.assert_array_equal(region(raised, -.37), 1)
    flipped = capture("reversed-light", "shadow-matte", direction=(.6, -.8, 0))
    np.testing.assert_array_equal(region(flipped, -.85), 0)
    np.testing.assert_array_equal(region(flipped, .85), 1)
    np.testing.assert_array_equal(capture("no-light", "shadow-matte", intensity=0), 0)
    print("PASS analytic ground, light/height response, and linear EXR shadow matte", flush=True)

    sequence = output / "live-shadow-matte.json"
    move_frame, light_frame = 4, 7
    checkpoints = (3, move_frame, 6, light_frame, 9)
    sequence.write_text(json.dumps({
        "schemaVersion": 1, "frames": checkpoints[-1], "sampleFrames": checkpoints,
        "events": [
            {"frame": move_frame, "nodeTransform": {"node": {"sourceNode": 0},
                                           "matrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1.7, 0, 0, 1]}},
            {"frame": light_frame, "lightEdit": {"target": {"type": "directional", "source": "generated", "index": 0},
                                       "phase": "commit", "values": {"intensity": 0}}},
        ],
    }), encoding="utf-8")
    # Let the light editor own intensity during the sequence. A CLI override takes precedence.
    image = run_capture("live-shadow-matte", "shadow-matte", intensity=None, sequence=sequence)
    for frame, samples, resets, submitted in zip(
        checkpoints, (16, 8, 16, 8, 16), (1, 2, 2, 3, 3), (0, 8, 0, 8, 0), strict=True
    ):
        path = image if frame == checkpoints[-1] else image.with_name(f"{image.stem}.frame-{frame:04d}.exr")
        pixels = read_capture(path, "shadow-matte", 0, samples=samples, resets=resets, submitted=submitted)
        state = json.loads(path.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
        intensity = state["lighting"]["directionalIntensity"]
        assert (intensity > 0 if frame < light_frame else intensity == 0), state["lighting"]
        if frame == checkpoints[0]:
            np.testing.assert_array_equal(pixels, matte)
        elif frame < light_frame:
            np.testing.assert_array_equal(region(pixels, -.85), 0)
            np.testing.assert_array_equal(region(pixels, .85), 1)
        else:
            np.testing.assert_array_equal(pixels, 0)
    print("PASS blocker and light edits reset and reconverge shadow matte at the exact sample stop", flush=True)


if __name__ == "__main__":
    main()
