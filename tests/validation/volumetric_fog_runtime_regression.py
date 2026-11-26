"""Validate froxel attenuation, scattering, shadows, and GPU cost in native captures."""

import argparse
import json
import math
import os
from pathlib import Path
from statistics import median
import subprocess
import sys
import tempfile

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")


WIDTH, HEIGHT = 320, 200
TECHNIQUES = ("deferred-raster", "forward-raster")
BACK_COLOR = (.24, .32, .55)
BLEND_COLOR = (.8, .15, .1)
GLASS_COLOR = (.12, .71, .24)
SUN_COLOR = (1, .96, .9)
OPT_IN = "CONTAINER_RUN_GPU_VOLUMETRIC_FOG"
PERSPECTIVE_FOV = math.radians(120)
PERSPECTIVE_DEPTH = 10
FOG_DISTANCE = 15
IES = "EXT_lights_ies"
IES_PROFILE = (b"IESNA:LM-63-2002\n[TEST] Asymmetric fog fixture\nTILT=NONE\n"
               b"1 -1 1 3 5 1 2 0 0 0\n1 1 12\n0 45 90\n0 90 180 270 360\n"
               b"64 64 0\n8 8 0\n16 16 0\n24 24 0\n64 64 0\n")


def fixture(path, *, step=False, blocker=False, punctual=False, transparent=False,
            area=None, away=False, perspective=False, ies_rotation=None):
    from gltf_accessor_runtime_regression import Asset

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

    back_depth = -2 if step or transparent else 6 - PERSPECTIVE_DEPTH
    back_width, back_height = (34, 22) if perspective else (3, 1.9)
    if ies_rotation is None:
        quad("Back plate", [[-back_width, -back_height, back_depth], [back_width, -back_height, back_depth],
                            [back_width, back_height, back_depth], [-back_width, back_height, back_depth]],
             [0, 0, 1], 0)
    else:
        # Leave the measured rays open beyond the nearby geometry so a shadow
        # ending at the caster bounds produces a measurable far-end tail.
        quad("Side reference plate", [[-3, -.25, 0], [-2.6, -.25, 0],
                                       [-2.6, .25, 0], [-3, .25, 0]], [0, 0, 1], 0)
    if step:
        quad("Foreground plate", [[-3, -1.9, 3], [-.1, -1.9, 3], [-.1, 1.9, 3], [-3, 1.9, 3]], [0, 0, 1], 0)
    if transparent:
        quad("Alpha blend", [[-3, -1.9, 3], [.65, -1.9, 3], [.65, 1.9, 3], [-3, 1.9, 3]], [0, 0, 1], 2)
        quad("Transmissive glass", [[-.65, -1.9, 1], [3, -1.9, 1], [3, 1.9, 1], [-.65, 1.9, 1]], [0, 0, 1], 3)
    if blocker and ies_rotation is not None:
        # Intercept the +X lobe without covering the receiver pixels or the +Y lobe.
        quad("IES beam blocker", [[.55, -.45, -3.75], [.55, .45, -3.75],
                                  [.55, .45, 3], [.55, -.45, 3]], [1, 0, 0], 1)
    elif blocker:
        quad("Side blocker", [[-1.4, -.7, -.4], [-1.4, .7, -.4], [-1.4, .7, 5.4], [-1.4, -.7, 5.4]], [1, 0, 0], 1)
    camera = len(model["nodes"])
    model["nodes"].append({"camera": 0, "translation": [0, 0, 6]})
    if perspective:
        model["cameras"] = [{"type": "perspective", "perspective": {"yfov": PERSPECTIVE_FOV,
                                                                   "aspectRatio": WIDTH / HEIGHT,
                                                                   "znear": .1, "zfar": 25}}]
    else:
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
    if ies_rotation is not None:
        half_angle = math.radians(ies_rotation) / 2
        model["nodes"][-1] = {"name": "Measured fog fixture", "translation": [0, 0, 3],
                              "rotation": [0, 0, math.sin(half_angle), math.cos(half_angle)],
                              "extensions": {IES: {"light": 0}}}
        model["extensions"] = {IES: {"lights": [{"bufferView": asset.view(IES_PROFILE),
                                                 "mimeType": "application/x-ies-lm-63"}]}}
        model["extensionsUsed"][0] = IES
    back_color = 0 if perspective else .5
    model["materials"] = [
        {"extensions": {"KHR_materials_unlit": {}},
         "pbrMetallicRoughness": {"baseColorFactor": [back_color, back_color, back_color, 1]}},
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
    import numpy as np

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


def expected_perspective_fog(density, albedo, sun):
    import numpy as np

    # A pinhole ray reaches the plate at a fixed Z but travels farther off-axis.
    # For an isotropic, spatially constant source, the in-scatter integrates to
    # the directional radiance times albedo, phase, and the removed fraction.
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH] + .5
    scale = math.tan(PERSPECTIVE_FOV / 2)
    horizontal = (2 * x / WIDTH - 1) * scale * WIDTH / HEIGHT
    vertical = (1 - 2 * y / HEIGHT) * scale
    length = PERSPECTIVE_DEPTH * np.sqrt(1 + horizontal**2 + vertical**2)
    transmittance = np.exp(-density * length)[..., None]
    scattering = (1 - transmittance) * (sun * albedo / (4 * math.pi)) * np.array(SUN_COLOR)
    return np.broadcast_to(transmittance, scattering.shape), scattering


def expected_ies_scattering(roi, rotation, density):
    import numpy as np

    # Integrate individual orthographic camera rays independently of the
    # froxel grid. The unit-distance candela table depends on the emitter's
    # polar and azimuth angles. Incoming light is unattenuated to match
    # single scattering with geometry shadows, and the camera ray attenuates.
    rows, columns = np.mgrid[roi[0].start:roi[0].stop, roi[1].start:roi[1].stop]
    x = ((columns + .5) / WIDTH * 2 - 1) * 3.2
    y = (1 - (rows + .5) / HEIGHT * 2) * 2
    radius_sq = x * x + y * y
    azimuth = np.mod(np.degrees(np.arctan2(y, x)) - rotation, 360)
    candelas = np.interp(azimuth, [0, 90, 180, 270, 360], [64, 8, 16, 24, 64])
    # The light is three units in front of the camera. Open rays integrate
    # through the complete fog distance.
    depth = (np.arange(4096) + .5) * (FOG_DISTANCE / 4096)
    emitter_depth = depth - 3
    polar = np.degrees(np.arctan2(np.sqrt(radius_sq)[..., None], emitter_depth))
    vertical = np.interp(polar, [0, 45, 90], [1, 1, 0])
    attenuation = np.exp(-density * depth) / (radius_sq[..., None] + emitter_depth**2)
    scattering = candelas * np.mean(vertical * attenuation, axis=-1) * (FOG_DISTANCE * density * .9 / (4 * math.pi))
    return float(scattering.mean())


def verify_ies_scattering(capture, fixtures):
    import numpy as np

    regions = {"right": (slice(97, 103), slice(217, 223)),
               "above": (slice(37, 43), slice(157, 163))}
    cases = {"unrotated": (0, .2), "rotated": (90, .2), "lower-density": (0, .05),
             "blocked": (0, .2)}
    results = {}
    for technique in TECHNIQUES:
        result = {}
        for name, (rotation, density) in cases.items():
            label = f"{technique}-ies-fog-{name}"
            pixels, state = capture(label, fixtures[name], technique, "scattering", density=density)
            measured_lights = [light for light in state["lightEdits"]["lights"] if light.get("extension") == IES]
            assert len(measured_lights) == state["lighting"]["pointLightCount"] == 1, (label, measured_lights)
            assert measured_lights[0]["range"] == 0, (label, measured_lights)
            assert state["lighting"]["directionalIntensity"] == state["lighting"]["environmentIntensity"] == 0, label
            result[name] = {}
            for region, roi in regions.items():
                rgb = pixels[roi][..., ::-1].mean(axis=(0, 1))
                entry = {"measuredRgb": rgb.tolist()}
                if name != "blocked":
                    expected = expected_ies_scattering(roi, rotation, density)
                    assert np.allclose(rgb, expected, rtol=.08, atol=.0015), (label, region, rgb, expected)
                    entry["expectedRgb"] = [expected] * 3
                result[name][region] = entry
            if name == "blocked":
                assert next(record for record in state["passes"] if record["name"] == "LocalShadowDepth")["active"], label
                assert state["lighting"]["activeLocalShadowLayers"] == 6, label
                bright = np.array(result["unrotated"]["right"]["measuredRgb"])
                dark = np.array(result[name]["right"]["measuredRgb"])
                assert np.all(dark < .006 * bright + .0003), (label, bright, dark)
                assert np.allclose(result[name]["above"]["measuredRgb"],
                                   result["unrotated"]["above"]["measuredRgb"], rtol=.1, atol=.0015), label
        unrotated = result["unrotated"]
        rotated = result["rotated"]
        assert unrotated["right"]["measuredRgb"][0] > 4 * unrotated["above"]["measuredRgb"][0], technique
        assert rotated["above"]["measuredRgb"][0] > 2 * rotated["right"]["measuredRgb"][0], technique
        assert result["lower-density"]["right"]["measuredRgb"][0] < .65 * unrotated["right"]["measuredRgb"][0], technique
        results[technique] = result
    deferred, forward = (results[technique] for technique in TECHNIQUES)
    for name in cases:
        for region in regions:
            assert np.allclose(deferred[name][region]["measuredRgb"], forward[name][region]["measuredRgb"],
                               rtol=.01, atol=.0005), (name, region, deferred[name][region], forward[name][region])
    return results


def read_pixels(path):
    import cv2
    import numpy as np

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
    parser.add_argument("--managed-output", action="store_true", help="Keep each run in a fresh output subdirectory")
    parser.add_argument("--require-opt-in", action="store_true", help=f"Require {OPT_IN}=1 for native runs")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--with-mode-switches", action="store_true",
                       help="Also validate switching FSR raster, reference path tracing, and viewport size")
    modes.add_argument("--ies-only", action="store_true", help="Run just the measured-light fog captures")
    args = parser.parse_args()
    if args.require_opt_in and not args.generate_only and os.environ.get(OPT_IN) != "1":
        print(f"Set {OPT_IN}=1 to run native volumetric fog validation.", flush=True)
        return 77
    import numpy as np

    from gltf_extended_materials_runtime_regression import validation_log_error

    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    print(f"Volumetric fog fixtures and captures {output}", flush=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    ies_clear = fixture(output / "ies-clear.gltf", ies_rotation=0)
    ies_fixtures = {"unrotated": ies_clear, "lower-density": ies_clear,
                    "rotated": fixture(output / "ies-rotated.gltf", ies_rotation=90),
                    "blocked": fixture(output / "ies-blocked.gltf", ies_rotation=0, blocker=True)}
    if not args.ies_only:
        depth_scene = fixture(output / "depth-step.gltf", step=True)
        perspective_scene = fixture(output / "perspective-plate.gltf", perspective=True)
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

    def capture(label, asset, technique, mode, *, density=.2, albedo=.9, anisotropy=0,
                sun=0, sun_direction=(1, 0, 0), history=None, fsr=None):
        source, camera = asset
        stem = output / label
        image, log = stem.with_suffix(".exr"), stem.with_suffix(".log")
        command = [str(renderer), "--model", str(source), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
                   "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom", "--no-ddgi",
                   "--render-technique", technique, "--display-mode", "lit", "--ray-shadows", "raster",
                   "--environment-intensity", "0", "--directional-intensity", str(sun),
                   "--directional-direction", *map(str, sun_direction), "--directional-color", *map(str, SUN_COLOR),
                   "--volumetric-fog", mode, "--fog-density", str(density), "--fog-albedo", str(albedo),
                   "--fog-anisotropy", str(anisotropy), "--fog-distance", str(FOG_DISTANCE),
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
                           timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer"),
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        log_text = log.read_text(encoding="utf-8", errors="replace")
        assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log_text, f"Khronos validation was not loaded: {log}"
        assert not validation_log_error(log_text), f"Vulkan validation failed: {log}"
        assert "Volumetric fog unavailable" not in log_text, log
        frames = history["sampleFrames"] if history else [1]
        # A fresh frame or resource transition can precede GPU query readback.
        transition_frames = {1, *(event["frame"] for event in (history or {}).get("events", []))}
        results = []
        evidence = []
        for frame in frames:
            prefix = stem if frame == frames[-1] else output / f"{label}.frame-{frame:04d}"
            capture_image = Path(str(prefix) + ".exr")
            telemetry = Path(str(prefix) + ".telemetry.json")
            state = json.loads(telemetry.read_text(encoding="utf-8"))
            active_technique = state["technique"]
            assert history or active_technique == technique, (label, active_technique)
            raster = active_technique in TECHNIQUES
            active = raster and mode != "off" and density > 0
            fog = fog_pass(state)
            assert bool(fog and fog["active"]) == active, (label, frame, fog)
            settings = state.get("volumetricFog", {})
            assert settings.get("active", False) == active, (label, frame, settings)
            render_extent = state["fsr"]["renderExtent"]
            if active:
                render_width, render_height = render_extent
                # Fixture extents fit the high grid's 8x8 tiles and 64 slices.
                # Allocation includes two history fields, one cumulative field and per-frame uniforms.
                cells = math.ceil(render_width / 8) * math.ceil(render_height / 8) * 64
                assert settings["allocationBytes"] > cells * 3 * 16, (label, frame, render_extent, settings)
                if fog["gpuTimed"]:
                    assert math.isfinite(fog["gpuMs"]) and fog["gpuMs"] > 0, (label, frame, fog)
                else:
                    assert history and frame in transition_frames, (label, frame, fog)
            elif raster:
                assert settings.get("allocationBytes", 0) == 0, (label, frame, settings)
            results.append((read_pixels(capture_image), state))
            evidence.append({"image": str(capture_image), "telemetry": str(telemetry), "pass": fog, "fog": settings,
                             "technique": active_technique, "renderExtent": render_extent})
        captures[label] = {"frames": evidence, "log": str(log), "gpu": state["gpu"], "build": state["build"]}
        return results if history else results[0]

    def write_report(report):
        gpu_metrics = {}
        for technique in TECHNIQUES:
            active_frames = [frame for sample in captures.values() for frame in sample["frames"]
                             if frame["technique"] == technique and frame["pass"]["active"]]
            times = [frame["pass"]["gpuMs"] for frame in active_frames if frame["pass"]["gpuTimed"]]
            gpu_metrics[technique] = {
                "timedFrames": len(times), "minPassMs": min(times), "medianPassMs": median(times), "maxPassMs": max(times),
                "allocationBytes": sorted({frame["fog"]["allocationBytes"] for frame in active_frames})}
        report.update(passed=True, captures=captures, gpuMetrics=gpu_metrics)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    ies_scattering = verify_ies_scattering(capture, ies_fixtures)
    if args.ies_only:
        write_report({"iesScattering": ies_scattering})
        print("PASS measured-light fog orientation, density, shadows, and GPU telemetry")
        return

    measured = {}
    transparent = {}
    center = (slice(96, 104), slice(156, 164))
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
                scatter = np.array(SUN_COLOR) * (4 * .9 / (4 * math.pi)) * (1 - transmittance)
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

    perspective = {}
    perspective_density, perspective_albedo, perspective_sun = .035, .9, 4
    perspective_transmission, perspective_scatter = expected_perspective_fog(
        perspective_density, perspective_albedo, perspective_sun)
    # The outermost pixels lie past the nearest froxel centers. Sampling all four
    # corners catches under-integration from clamping to those center rays.
    regions = {
        "center": center,
        "topLeft": (slice(0, 3), slice(0, 3)),
        "topRight": (slice(0, 3), slice(-3, None)),
        "bottomLeft": (slice(-3, None), slice(0, 3)),
        "bottomRight": (slice(-3, None), slice(-3, None)),
    }
    for technique in TECHNIQUES:
        perspective[technique] = {}
        for mode, expected in (("transmittance", perspective_transmission),
                               ("scattering", perspective_scatter), ("low", perspective_scatter)):
            # Low uses lit output. The receiver is black and unlit, so only fog contributes.
            label = f"{technique}-perspective-{mode}"
            pixels, _ = capture(label, perspective_scene, technique, mode,
                                density=perspective_density, albedo=perspective_albedo, sun=perspective_sun)
            rgb = pixels[..., ::-1]
            assert rgb.shape == expected.shape, (label, rgb.shape)
            errors = np.abs(rgb - expected)
            perspective[technique][mode] = {}
            for name, roi in regions.items():
                max_error = float(errors[roi].max())
                assert max_error < .0006, (label, name, max_error, rgb[roi].mean(axis=(0, 1)),
                                           expected[roi].mean(axis=(0, 1)))
                perspective[technique][mode][name] = {
                    "expectedRgb": expected[roi].mean(axis=(0, 1)).tolist(),
                    "measuredRgb": rgb[roi].mean(axis=(0, 1)).tolist(), "maxAbsoluteError": max_error}

    shadow_roi = (slice(91, 109), slice(176, 220))
    shadows = {}
    phase_scattering = {}
    for technique in TECHNIQUES:
        shadows[technique] = {}
        for light, clear, blocked, sun, shadow_pass in (
                ("directional", clear_directional, blocked_directional, 4, "ShadowCascade0"),
                ("point", clear_local, blocked_local, 0, "LocalShadowDepth")):
            label = f"{technique}-{light}"
            lit, _ = capture(label + "-clear", clear, technique, "scattering", sun=sun)
            occluded, state = capture(label + "-blocked", blocked, technique, "scattering", sun=sun)
            assert next(record for record in state["passes"] if record["name"] == shadow_pass)["active"], label
            bright, dark = float(lit[shadow_roi].mean()), float(occluded[shadow_roi].mean())
            assert bright > .01 and dark < .85 * bright, (label, bright, dark)
            shadows[technique][light] = {"clear": bright, "occluded": dark}

        phase_scattering[technique] = {}
        # A -Z camera ray and incoming sun propagation (0.8, 0, -0.6)
        # give a scattering angle cosine of -0.6. The plate is 10 units away.
        cosine, depth = -.6, 10
        for phase_name, anisotropy in (("positive", .6), ("negative", -.6)):
            label = f"{technique}-phase-{phase_name}"
            pixels, _ = capture(label, clear_directional, technique, "scattering", sun=4,
                                sun_direction=(.8, 0, -.6), anisotropy=anisotropy)
            phase = (1 - anisotropy**2) / (4 * math.pi * (1 + anisotropy**2 - 2 * anisotropy * cosine)**1.5)
            expected = np.array(SUN_COLOR) * (4 * .9 * phase) * (1 - math.exp(-.2 * depth))
            rgb = pixels[center][..., ::-1]
            assert np.allclose(rgb, expected, atol=.004, rtol=0), (label, rgb.mean(axis=(0, 1)), expected)
            phase_scattering[technique][phase_name] = {
                "anisotropy": anisotropy, "cosine": cosine, "expectedRgb": expected.tolist(),
                "measuredRgb": rgb.mean(axis=(0, 1)).tolist()}

    area_scattering = {}
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
    assert (trajectory[-1][1]["volumetricFog"]["allocationBytes"] <
            trajectory[0][1]["volumetricFog"]["allocationBytes"]), "Fog allocation did not shrink after resize"

    report = {"analyticTransmittance": measured, "shadowScattering": shadows, "phaseScattering": phase_scattering,
              "areaScattering": area_scattering, "iesScattering": ies_scattering, "transparentLayers": transparent,
              "perspectiveFog": perspective, "historyReprojected": accepted}
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
    write_report(report)
    print("PASS froxel attenuation, perspective fog, phase scattering, transparent layers, area and measured lights, "
          "raster shadows, zero-density bypass, fog history, and GPU telemetry")


if __name__ == "__main__":
    sys.exit(main())
