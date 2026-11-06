"""Validate depth-aware froxel fog and raster shadow visibility in native captures."""

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


WIDTH, HEIGHT = 320, 200
TECHNIQUES = ("deferred-raster", "forward-raster")


def fixture(path, *, step=False, blocker=False, punctual=False):
    asset = Asset()
    model = asset.model
    model["nodes"], model["meshes"] = [], []

    def quad(name, positions, normal, material):
        indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)
        primitive = {"attributes": {"POSITION": asset.accessor("VEC3", positions),
                                   "NORMAL": asset.accessor("VEC3", [normal] * 4)},
                     "indices": indices, "material": material}
        model["nodes"].append({"mesh": len(model["meshes"]), "name": name})
        model["meshes"].append({"primitives": [primitive]})

    back_depth = -2 if step else -4
    quad("Back plate", [[-3, -1.9, back_depth], [3, -1.9, back_depth],
                        [3, 1.9, back_depth], [-3, 1.9, back_depth]], [0, 0, 1], 0)
    if step:
        quad("Foreground plate", [[-3, -1.9, 3], [-.1, -1.9, 3], [-.1, 1.9, 3], [-3, 1.9, 3]], [0, 0, 1], 0)
    if blocker:
        quad("Side blocker", [[-1.4, -.7, -.4], [-1.4, .7, -.4], [-1.4, .7, 5.4], [-1.4, -.7, 5.4]], [1, 0, 0], 1)
    camera = len(model["nodes"])
    model["nodes"].append({"camera": 0, "translation": [0, 0, 6]})
    model["cameras"] = [{"type": "orthographic", "orthographic": {"xmag": 3.2, "ymag": 2,
                                                                     "znear": .1, "zfar": 25}}]
    model["nodes"].append({"translation": [-2.6, 0, 2.5], "extensions": {"KHR_lights_punctual": {"light": 0}}})
    model["extensions"] = {"KHR_lights_punctual": {"lights": [{"type": "point", "range": 14,
                                                                        "intensity": 64 if punctual else 0}]}}
    model["extensionsUsed"] = ["KHR_lights_punctual", "KHR_materials_unlit"]
    model["materials"] = [
        {"extensions": {"KHR_materials_unlit": {}}, "pbrMetallicRoughness": {"baseColorFactor": [.5, .5, .5, 1]}},
        {"doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [.1, .1, .1, 1], "metallicFactor": 0}},
    ]
    model["scenes"] = [{"nodes": list(range(len(model["nodes"])))}]
    asset.write(path)
    return path, camera


def read_pixels(path):
    pixels = cv2.imdecode(np.frombuffer(path.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
    assert pixels is not None and pixels.dtype == np.float32 and np.isfinite(pixels).all(), path
    return pixels[..., :3]


def fog_pass(state):
    return next(record for record in state["passes"] if record["name"] == "VolumetricFog")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    depth_scene = fixture(output / "depth-step.gltf", step=True)
    clear_directional = fixture(output / "clear-directional.gltf")
    blocked_directional = fixture(output / "blocked-directional.gltf", blocker=True)
    clear_local = fixture(output / "clear-local.gltf", punctual=True)
    blocked_local = fixture(output / "blocked-local.gltf", blocker=True, punctual=True)
    if args.generate_only:
        return

    captures = {}

    def capture(label, asset, technique, mode, *, density=.2, albedo=.9, sun=0, history=None):
        source, camera = asset
        stem = output / label
        image, log = stem.with_suffix(".exr"), stem.with_suffix(".log")
        command = [str(renderer), "--model", str(source), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
                   "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom", "--no-ddgi",
                   "--render-technique", technique, "--display-mode", "lit", "--ray-shadows", "raster",
                   "--environment-intensity", "0", "--directional-intensity", str(sun),
                   "--directional-direction", "1", "0", "0", "--volumetric-fog", mode, "--fog-density", str(density),
                   "--fog-albedo", str(albedo), "--fog-anisotropy", "0", "--fog-distance", "15",
                   "--screenshot", str(image)]
        if history:
            script = stem.with_suffix(".sequence.json")
            script.write_text(json.dumps(history), encoding="utf-8")
            command += ["--camera-fov", "45", "--capture-sequence", str(script)]
        else:
            command += ["--gltf-camera-node", str(camera), "--warmup-frames", "1", "--capture-frame", "2"]
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        log_text = log.read_text(encoding="utf-8", errors="replace")
        assert not validation_log_error(log_text), log
        assert "Volumetric fog unavailable" not in log_text, log
        frames = history["sampleFrames"] if history else [1]
        results = []
        evidence = []
        for frame in frames:
            prefix = stem if frame == frames[-1] else output / f"{label}.frame-{frame:04d}"
            png_or_exr = Path(str(prefix) + ".exr")
            telemetry = Path(str(prefix) + ".telemetry.json")
            state = json.loads(telemetry.read_text(encoding="utf-8"))
            active = mode != "off" and density > 0
            assert fog_pass(state)["active"] == active, (label, fog_pass(state))
            settings = state.get("volumetricFog", {})
            assert bool(settings.get("allocationBytes", 0)) == active, (label, settings)
            results.append((read_pixels(png_or_exr), state))
            evidence.append({"image": str(png_or_exr), "telemetry": str(telemetry), "pass": fog_pass(state), "fog": settings})
        captures[label] = {"frames": evidence, "log": str(log)}
        return results if history else results[0]

    measured = {}
    near_roi = (slice(84, 116), slice(55, 108))
    far_roi = (slice(84, 116), slice(200, 259))
    for technique in TECHNIQUES:
        off, _ = capture(technique + "-off", depth_scene, technique, "off")
        zero, _ = capture(technique + "-zero", depth_scene, technique, "high", density=0)
        assert np.array_equal(off, zero), technique
        transmission, _ = capture(technique + "-transmittance", depth_scene, technique, "transmittance")
        absorbed, _ = capture(technique + "-absorbed", depth_scene, technique, "high", albedo=0)
        measured[technique] = []
        for roi, depth in ((near_roi, 3), (far_roi, 8)):
            expected = math.exp(-.2 * depth)
            measured[technique].append(float(transmission[roi].mean()))
            assert np.allclose(transmission[roi], expected, atol=.001, rtol=0), (technique, depth)
            assert np.allclose(absorbed[roi], off[roi] * expected, atol=.001, rtol=.002), (technique, depth)

    shadow_roi = (slice(91, 109), slice(176, 220))
    shadows = {}
    for label, clear, blocked, sun in (("directional", clear_directional, blocked_directional, 4),
                                       ("point", clear_local, blocked_local, 0)):
        lit, _ = capture(label + "-clear", clear, "deferred-raster", "scattering", sun=sun)
        occluded, state = capture(label + "-blocked", blocked, "deferred-raster", "scattering", sun=sun)
        assert next(record for record in state["passes"] if record["name"] ==
                    ("ShadowCascade0" if label == "directional" else "LocalShadowDepth"))["active"], label
        bright, dark = float(lit[shadow_roi].mean()), float(occluded[shadow_roi].mean())
        assert bright > .01 and dark < .85 * bright, (label, bright, dark)
        shadows[label] = {"clear": bright, "occluded": dark}

    camera_x = [0, .15, .3, 3.3, 3.45, 3.5]
    history = {"schemaVersion": 1, "frames": 6, "sampleFrames": [1, 2, 3, 4, 5, 6],
               "camera": [{"frame": index + 1, "position": [x, 0, 6], "target": [x, 0, 0]}
                          for index, x in enumerate(camera_x)],
               "events": [{"frame": 6, "resize": [256, 160]}]}
    trajectory = capture("history", blocked_directional, "deferred-raster", "low", sun=4, history=history)
    accepted = [state["volumetricFog"]["historyReprojected"] for _, state in trajectory]
    assert accepted == [False, True, True, False, True, False], accepted
    assert trajectory[-1][0].shape[:2] == (160, 256), trajectory[-1][0].shape

    report = {"passed": True, "analyticTransmittance": measured, "shadowScattering": shadows,
              "historyReprojected": accepted, "captures": captures}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS froxel attenuation, directional and local shadows, zero-density bypass, and fog history")


if __name__ == "__main__":
    main()
