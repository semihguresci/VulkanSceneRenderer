"""Validate froxel attenuation, transparent layers, and raster shadows in native captures."""

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
BACK_COLOR = (.24, .32, .55)
BLEND_COLOR = (.8, .15, .1)
GLASS_COLOR = (.12, .71, .24)


def fixture(path, *, step=False, blocker=False, punctual=False, transparent=False,
            area=None, away=False):
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

    back_depth = -2 if step or transparent else -4
    quad("Back plate", [[-3, -1.9, back_depth], [3, -1.9, back_depth],
                        [3, 1.9, back_depth], [-3, 1.9, back_depth]], [0, 0, 1], 0)
    if step:
        quad("Foreground plate", [[-3, -1.9, 3], [-.1, -1.9, 3], [-.1, 1.9, 3], [-3, 1.9, 3]], [0, 0, 1], 0)
    if transparent:
        quad("Alpha blend", [[-3, -1.9, 3], [.65, -1.9, 3], [.65, 1.9, 3], [-3, 1.9, 3]], [0, 0, 1], 2)
        quad("Transmissive glass", [[-.65, -1.9, 1], [3, -1.9, 1], [3, 1.9, 1], [-.65, 1.9, 1]], [0, 0, 1], 3)
    if blocker:
        quad("Side blocker", [[-1.4, -.7, -.4], [-1.4, .7, -.4], [-1.4, .7, 5.4], [-1.4, -.7, 5.4]], [1, 0, 0], 1)
    camera = len(model["nodes"])
    model["nodes"].append({"camera": 0, "translation": [0, 0, 6]})
    model["cameras"] = [{"type": "orthographic", "orthographic": {"xmag": 3.2, "ymag": 2,
                                                                     "znear": .1, "zfar": 25}}]
    model["nodes"].append({"translation": [-2.6, 0, 2.5], "extensions": {"KHR_lights_punctual": {"light": 0}}})
    model["extensions"] = {"KHR_lights_punctual": {"lights": [{"type": "point", "range": 14,
                                                                        "intensity": 64 if punctual else 0}]}}
    if area:
        # An emitter on the left of the view faces toward +X. The side
        # blocker lies between it and the sampled part of the medium.
        model["nodes"][-1]["rotation"] = [0, math.sqrt(.5) * (1 if away else -1), 0, math.sqrt(.5)]
        model["extensions"]["KHR_lights_punctual"]["lights"][0].update(
            intensity=64, color=[1, .8, .6],
            extras={"areaLight": {"shape": area, "width": .8, "height": .8,
                                  "radius": .4, "range": 14}})
    model["extensionsUsed"] = ["KHR_lights_punctual", "KHR_materials_unlit"]
    model["materials"] = [
        {"extensions": {"KHR_materials_unlit": {}}, "pbrMetallicRoughness": {"baseColorFactor": [.5, .5, .5, 1]}},
        {"doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [.1, .1, .1, 1], "metallicFactor": 0}},
    ]
    if transparent:
        model["extensionsUsed"].append("KHR_materials_transmission")
        model["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = [*BACK_COLOR, 1]
        model["materials"].extend([
            {"alphaMode": "BLEND", "extensions": {"KHR_materials_unlit": {}},
             "pbrMetallicRoughness": {"baseColorFactor": [*BLEND_COLOR, .5]}},
            {"extensions": {"KHR_materials_transmission": {"transmissionFactor": 1}},
             "emissiveFactor": GLASS_COLOR,
             "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1], "metallicFactor": 0}},
        ])
    model["scenes"] = [{"nodes": list(range(len(model["nodes"])))}]
    asset.write(path)
    return path, camera


def expected_area_scattering(shape):
    # Independent midpoint integration along the central orthographic ray.
    # The four emitter samples have equal area and the fog is isotropic.
    depth = (np.arange(4096) + .5) * (10 / 4096)
    sample_offset = .2 if shape == "rect" else .4 * (4 / (3 * math.pi))
    area = .8 * .8 if shape == "rect" else math.pi * .4**2
    solid_angle = np.zeros_like(depth)
    for z in (2.5 - sample_offset, 2.5 + sample_offset):
        distance_sq = 2.6**2 + sample_offset**2 + (z - (6 - depth))**2
        source_cosine = 2.6 / np.sqrt(distance_sq)
        range_fade = np.maximum(1 - distance_sq / 14**2, 0)**2
        solid_angle += 2 * (area / 4) * source_cosine * range_fade / distance_sq
    irradiance = (64 / (4 * math.pi)) * solid_angle
    integrated = 10 * np.mean(np.exp(-.2 * depth) * (.2 * .9) * irradiance)
    return integrated * np.array([1, .8, .6])


def read_pixels(path):
    pixels = cv2.imdecode(np.frombuffer(path.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
    assert pixels is not None and pixels.dtype == np.float32 and np.isfinite(pixels).all(), path
    return pixels[..., :3]


def fog_pass(state):
    return next((record for record in state["passes"] if record["name"] == "VolumetricFog"), None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--with-mode-switches", action="store_true",
                        help="Also validate switching FSR raster, reference path tracing, and viewport size")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    depth_scene = fixture(output / "depth-step.gltf", step=True)
    layered_scene = fixture(output / "transparent-layers.gltf", transparent=True)
    clear_directional = fixture(output / "clear-directional.gltf")
    blocked_directional = fixture(output / "blocked-directional.gltf", blocker=True)
    clear_local = fixture(output / "clear-local.gltf", punctual=True)
    blocked_local = fixture(output / "blocked-local.gltf", blocker=True, punctual=True)
    area_fixtures = {
        shape: (fixture(output / f"{shape}-area-clear.gltf", area=shape),
                fixture(output / f"{shape}-area-blocked.gltf", area=shape, blocker=True),
                fixture(output / f"{shape}-area-away.gltf", area=shape, away=True))
        for shape in ("rect", "disk")
    }
    if args.generate_only:
        return

    captures = {}

    def capture(label, asset, technique, mode, *, density=.2, albedo=.9, sun=0, history=None, fsr=None):
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
        if fsr:
            command += ["--fsr", fsr]
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
            raster = state["technique"] in TECHNIQUES
            active = raster and mode != "off" and density > 0
            fog = fog_pass(state)
            assert bool(fog and fog["active"]) == active, (label, fog)
            settings = state.get("volumetricFog", {})
            if raster:
                assert bool(settings.get("allocationBytes", 0)) == active, (label, settings)
            results.append((read_pixels(png_or_exr), state))
            evidence.append({"image": str(png_or_exr), "telemetry": str(telemetry), "pass": fog, "fog": settings})
        captures[label] = {"frames": evidence, "log": str(log)}
        return results if history else results[0]

    measured = {}
    transparent = {}
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

        label = technique + "-transparent-"
        off, state = capture(label + "off", layered_scene, technique, "off")
        assert next(record for record in state["passes"] if record["name"] == "OitResolve")["active"], label
        zero, _ = capture(label + "zero", layered_scene, technique, "high", density=0)
        assert np.array_equal(off, zero), label
        absorbed, _ = capture(label + "absorbed", layered_scene, technique, "high", albedo=0)
        scattering, _ = capture(label + "scattering", layered_scene, technique, "scattering", sun=4)
        lit, _ = capture(label + "lit", layered_scene, technique, "high", sun=4)
        alpha_blend, glass = (.5, 3, BLEND_COLOR), (.35, 5, GLASS_COLOR)
        transparent[technique] = {}
        for region, roi, foreground in (("blend", near_roi, (alpha_blend,)),
                                        ("glass", far_roi, (glass,)),
                                        ("overlap", (slice(84, 116), slice(148, 172)), (alpha_blend, glass))):
            expected = np.zeros((3, 3))
            revealage = 1.0
            for alpha, depth, rgb in (*foreground, (1, 8, BACK_COLOR)):
                transmittance = math.exp(-.2 * depth)
                color = np.asarray(rgb)
                # Isotropic directional scattering with the default sun tint.
                scatter = np.array([1, .96, .9]) * (4 * .9 / (4 * math.pi)) * (1 - transmittance)
                expected += revealage * alpha * np.array([color, color * transmittance, scatter])
                revealage *= 1 - alpha
            expected_rgb_by_capture = dict(zip(("off", "absorption", "scattering"), expected))
            expected_rgb_by_capture["lit"] = expected[1] + expected[2]
            transparent[technique][region] = {}
            for capture_name, pixels in (("off", off), ("absorption", absorbed), ("scattering", scattering), ("lit", lit)):
                expected_rgb = expected_rgb_by_capture[capture_name]
                actual_rgb = pixels[roi][..., ::-1]  # OpenCV loads EXR in BGR order.
                assert np.allclose(actual_rgb, expected_rgb, atol=.004, rtol=0), (
                    technique, region, capture_name, actual_rgb.mean(axis=(0, 1)), expected_rgb)
                transparent[technique][region][capture_name] = {
                    "expectedRgb": expected_rgb.tolist(), "measuredRgb": actual_rgb.mean(axis=(0, 1)).tolist()}

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

    area_scattering = {}
    center = (slice(96, 104), slice(156, 164))
    for technique in TECHNIQUES:
        area_scattering[technique] = {}
        for shape, (clear, blocked, away) in area_fixtures.items():
            label = f"{technique}-area-{shape}"
            lit, _ = capture(label + "-clear", clear, technique, "scattering")
            occluded, state = capture(label + "-blocked", blocked, technique, "scattering")
            back, _ = capture(label + "-away", away, technique, "scattering")
            assert next(record for record in state["passes"] if record["name"] == "LocalShadowDepth")["active"], label
            expected = expected_area_scattering(shape)
            rgb = lit[center][..., ::-1].mean(axis=(0, 1))
            bright, dark = float(lit[shadow_roi].mean()), float(occluded[shadow_roi].mean())
            back_maximum = float(np.max(np.abs(back[shadow_roi])))
            assert np.allclose(rgb, expected, rtol=.06, atol=.001), (label, rgb, expected)
            assert bright > .01 and dark < .85 * bright, (label, bright, dark)
            assert back_maximum < .0001, (label, back_maximum)
            area_scattering[technique][shape] = {
                "expectedRgb": expected.tolist(), "measuredRgb": rgb.tolist(),
                "clear": bright, "occluded": dark, "backFaceMaximum": back_maximum,
            }

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
              "areaScattering": area_scattering, "transparentLayers": transparent,
              "historyReprojected": accepted, "captures": captures}
    if args.with_mode_switches:
        modes = {"schemaVersion": 1, "frames": 10, "sampleFrames": [2, 4, 6, 8, 10],
                 "camera": [{"frame": 1, "position": [0, 0, 6], "target": [0, 0, 0]}],
                 "events": [{"frame": 3, "technique": "forward-raster"},
                            {"frame": 5, "technique": "path-tracing", "fsr": "native"},
                            {"frame": 7, "technique": "deferred-raster", "fsr": "quality"},
                            {"frame": 9, "resize": [256, 160]}]}
        trajectory = capture("mode-switches", layered_scene, "deferred-raster", "high", albedo=0,
                             history=modes, fsr="quality")
        observed = []
        for pixels, state in trajectory:
            technique, fsr_mode = state["technique"], state["fsr"]["mode"]
            if technique in TECHNIQUES:
                passes = {record["name"] for record in state["passes"] if record["active"]}
                assert {"OitClear", "OitResolve", "TemporalResolve", "VolumetricFog"} <= passes, passes
                y, x = (edge // 2 for edge in pixels.shape[:2])
                expected = transparent[technique]["overlap"]["absorption"]["expectedRgb"]
                assert np.allclose(pixels[y-3:y+3, x-3:x+3, ::-1], expected, atol=.015, rtol=0), state["fsr"]
            observed.append([technique, fsr_mode])
        assert observed == [["deferred-raster", "quality"], ["forward-raster", "quality"], ["path-tracing", "native"],
                            ["deferred-raster", "quality"], ["deferred-raster", "quality"]], observed
        assert trajectory[-1][0].shape[:2] == (160, 256), trajectory[-1][0].shape
        report["modeSwitches"] = observed
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS froxel attenuation, alpha blend and glass, area emitters, raster shadows, zero-density bypass, and fog history")


if __name__ == "__main__":
    main()
