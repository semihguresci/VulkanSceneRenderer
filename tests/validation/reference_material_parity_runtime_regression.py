"""Check material diagnostics against actual fixed-seed reference captures."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from display_capture_test_support import Asset, emitter_fixture, validation_log_error


WIDTH, HEIGHT, FOV, CAMERA_Z = 192, 144, 20, 4
SAMPLES, SEED = 32, 314
EMITTER_X, EMITTER_Y, EMITTER_Z = (1.6, 2.4), (-.4, .4), 2.5
BASE, EMISSION = (.7, .5, .3), (4, 2, 1)
NORMAL_TEXEL = (204, 128, 255)
FOG_DENSITY = .2
OPT_IN = "CONTAINER_RUN_GPU_REFERENCE_MATERIAL_PARITY"

# Reusable per-material checks. These are coverage links, not results from
# this run. The new image tests below exercise the previously silent paths.
MATERIAL_COVERAGE = {
    "Alpha and thin transmission": {
        "native": "tests/validation/reference_transmission_runtime_regression.py",
        "checks": "Tinted sheets, rough IOR-one transport, texture channels, open volume rejection",
    },
    "Solid glass": {
        "native": "tests/validation/reference_volume_runtime_regression.py",
        "checks": "World-distance Beer–Lambert attenuation, absorption from inside and nested media",
    },
    "Clearcoat and normal maps": {
        "native": "tests/validation/reference_clearcoat_runtime_regression.py",
        "checks": "Factor channels, independent base/coat normals, clearcoat-disabled control",
    },
    "Specular glossiness": {
        "native": "tests/validation/reference_specular_glossiness_runtime_regression.py",
        "checks": "Authored color and glossiness, linear alpha, separate UV1 transforms",
    },
    "Anisotropy, diffuse transmission, dispersion and mip selection": {
        "gpu": "tests/renderer/lighting/reference_path_trace_gpu_tests.cpp",
        "checks": "Rotated anisotropy, textured diffuse lobe, spectral Snell refraction, minified sheet MIS",
    },
    "Experimental retroreflection and thin scattering": {
        "native": "tests/validation/gltf_experimental_materials_runtime_regression.py",
        "checks": "Thin material energy, malformed input fallback, closed-volume fallback provenance",
    },
}


def matte(base):
    return {
        "pbrMetallicRoughness": {"baseColorFactor": [*base, 1], "metallicFactor": 0, "roughnessFactor": 1},
        "extensions": {"KHR_materials_specular": {"specularFactor": 0}},
    }


def emissive_material():
    material = matte((0, 0, 0))
    material["emissiveFactor"] = [component / 4 for component in EMISSION]
    material["extensions"]["KHR_materials_emissive_strength"] = {"emissiveStrength": 4}
    return material


def mesh_light_fixture(path, *, sheen=False, blocked=False, unlit=False):
    asset = Asset()
    asset.model["nodes"] = []
    asset.model["materials"] = []
    asset.model["scenes"][0]["nodes"] = []

    def quad(name, x, y, z, material, *, front=1):
        index = len(asset.model["nodes"])
        positions = [[x[0], y[0], z], [x[1], y[0], z], [x[1], y[1], z], [x[0], y[1], z]]
        order = [0, 1, 2, 0, 2, 3] if front > 0 else [0, 2, 1, 0, 3, 2]
        asset.model["meshes"].append({"primitives": [{
            "attributes": {
                "POSITION": asset.accessor("VEC3", positions),
                "NORMAL": asset.accessor("VEC3", [[0, 0, front]] * 4),
            },
            "indices": asset.accessor("SCALAR", order, 5123, indices=True),
            "material": index,
        }]})
        asset.model["nodes"].append({"name": name, "mesh": index})
        asset.model["scenes"][0]["nodes"].append(index)
        asset.model["materials"].append(material)

    quad("Lambert receiver", (-3, 3), (-3, 3), 0, matte(BASE))
    emitter = emissive_material()
    if sheen:
        emitter["extensions"]["KHR_materials_sheen"] = {"sheenColorFactor": [.8, .3, .15]}
    if unlit:
        emitter["extensions"]["KHR_materials_unlit"] = {}
    quad("Off-camera emitter", EMITTER_X, EMITTER_Y, EMITTER_Z, emitter, front=-1)
    if blocked:
        # Near the emitter, this covers every receiver-to-emitter connection
        # while staying outside the camera projection.
        blocker = matte((0, 0, 0))
        blocker["doubleSided"] = True
        quad("Off-camera blocker", (1, 3), (-1, 1), 2.25, blocker)
    asset.model["extensionsUsed"] = sorted({
        name for material in asset.model["materials"] for name in material["extensions"]
    })
    asset.write(path)


def filtered_material_fixture(path):
    import numpy as np

    from gltf_accessor_runtime_regression import Asset as TextureAsset

    asset = TextureAsset()
    model = asset.model
    checks = (np.indices((64, 64)) // 4).sum(axis=0) % 2
    checker = np.stack([checks * 255, checks * 0, (1-checks) * 255, checks * 0 + 255], axis=2)
    checker_index = asset.image(checker.astype(np.uint8))
    model["samplers"][checker_index] = {"minFilter": 9984, "magFilter": 9728, "wrapS": 10497, "wrapT": 10497}
    normal_index = asset.image(np.tile([*NORMAL_TEXEL, 255], (2, 2, 1)).astype(np.uint8))
    model["materials"] = [matte((.5, 0, .5)), matte((1, 1, 1)), matte((1, 1, 1))]
    for material in model["materials"][1:]:
        material["pbrMetallicRoughness"]["baseColorTexture"] = {
            "index": checker_index, "texCoord": 0,
            "extensions": {"KHR_texture_transform": {"texCoord": 1, "scale": [256, 256], "offset": [.2, .1]}},
        }
    model["materials"][2]["normalTexture"] = {"index": normal_index}
    model["extensionsUsed"] = ["KHR_materials_specular", "KHR_texture_transform"]

    normals = asset.accessor("VEC3", [[0, 0, 1]] * 4)
    tangent = asset.accessor("VEC4", [[1, 0, 0, 1]] * 4)
    # UV0 stays inside a single checker patch. Only the UV1 override and
    # its transform can reach the averaged mip.
    uv0 = asset.accessor("VEC2", [[.0001, .0001], [.00015, .0001], [.00015, .00015], [.0001, .00015]])
    indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)
    model["nodes"] = []
    for index, (lo, hi) in enumerate(((-1.5, -.30), (-.30, .30), (.30, 1.5))):
        positions = asset.accessor("VEC3", [[lo, -1, 0], [hi, -1, 0], [hi, 1, 0], [lo, 1, 0]])
        uv1 = asset.accessor("VEC2", [[lo/4, -.25], [hi/4, -.25], [hi/4, .25], [lo/4, .25]])
        model["nodes"].append({"mesh": index})
        model["meshes"].append({"primitives": [{
            "attributes": {
                "POSITION": positions, "NORMAL": normals, "TANGENT": tangent, "TEXCOORD_0": uv0, "TEXCOORD_1": uv1,
            },
            "indices": indices, "material": index,
        }]})
    ymag = CAMERA_Z * math.tan(math.radians(FOV / 2))
    model["cameras"] = [{"type": "orthographic", "orthographic": {
        "xmag": ymag * WIDTH / HEIGHT, "ymag": ymag, "znear": .1, "zfar": 100,
    }}]
    model["nodes"].append({"name": "Matched orthographic camera", "camera": 0, "translation": [0, 0, CAMERA_Z]})
    model["scenes"][0]["nodes"] = list(range(len(model["nodes"])))
    asset.write(path)


def rectangle_reference(x, y):
    """Independent midpoint integral for the parallel Lambertian planes."""
    import numpy as np

    count = 64
    ex = EMITTER_X[0] + (np.arange(count) + .5) * (EMITTER_X[1] - EMITTER_X[0]) / count
    ey = EMITTER_Y[0] + (np.arange(count) + .5) * (EMITTER_Y[1] - EMITTER_Y[0]) / count
    distance_squared = (ex[None, :] - x)**2 + (ey[:, None] - y)**2 + EMITTER_Z**2
    area = (EMITTER_X[1] - EMITTER_X[0]) * (EMITTER_Y[1] - EMITTER_Y[0])
    return np.multiply(BASE, EMISSION) * area / math.pi * np.mean(EMITTER_Z**2 / distance_squared**2)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--openvdb-tool", type=Path, help="Exercise static volume diagnostics with an OpenVDB-enabled build")
    args = parser.parse_args()
    if args.require_opt_in and not args.generate_only and os.environ.get(OPT_IN) != "1":
        print(f"Set {OPT_IN}=1 to run the native reference material checks.", flush=True)
        return 77

    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    print(f"Reference material fixtures and captures {output}", flush=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    fixtures = {name: output / f"{name}.gltf" for name in ("mesh", "sheen", "blocked", "unlit", "panel", "filtered")}
    mesh_light_fixture(fixtures["mesh"])
    mesh_light_fixture(fixtures["sheen"], sheen=True)
    mesh_light_fixture(fixtures["blocked"], sheen=True, blocked=True)
    mesh_light_fixture(fixtures["unlit"], sheen=True, unlit=True)
    panel_material = emissive_material()
    panel_fixture = emitter_fixture(fixtures["panel"], panel_material)
    panel_fixture["extensionsUsed"] = sorted(panel_material["extensions"])
    fixtures["panel"].write_text(json.dumps(panel_fixture), encoding="utf-8")
    filtered_material_fixture(fixtures["filtered"])
    if args.generate_only:
        return 0

    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    import cv2
    import numpy as np

    runtime = [renderer, *(renderer.parent / "spv_shaders").glob("reference*.spv")]
    runtime_hashes = {str(path): sha256(path) for path in runtime}
    report = {
        "schemaVersion": 1, "passed": False, "runtimeSha256": runtime_hashes,
        "fixtureSha256": {str(path): sha256(path) for path in fixtures.values()},
        "existingMaterialCoverage": MATERIAL_COVERAGE, "captures": {}, "checks": {},
        "openVdb": "not requested" if args.openvdb_tool is None else "pending",
    }
    result = output / "results.json"

    def capture(name, fixture, *flags, suffix=".exr", frames=(4,), events=()):
        sequence = output / f"{name}.sequence.json"
        sequence.write_text(json.dumps({
            "schemaVersion": 1, "frames": max(frames), "sampleFrames": list(frames), "events": list(events),
        }), encoding="utf-8")
        log = output / f"{name}.log"
        command = [
            str(renderer), "--model", str(fixture), "--import-scale", "1",
            "--hidden", "--no-ui", "--validation", "--width", str(WIDTH), "--height", str(HEIGHT),
            "--msaa", "1", "--no-taa", "--no-bloom", "--no-ddgi", "--ray-shadows", "raster",
            "--render-technique", "path-tracing", "--display-mode", "lit",
            "--camera-position", "0", "0", str(CAMERA_Z), "--camera-target", "0", "0", "0",
            "--camera-fov", str(FOV), "--environment-intensity", "0", "--directional-intensity", "0",
            "--tone-map", "neutral", "--exposure", "1", "--screenshot-viewport",
            "--path-spp", "8", "--path-samples", str(SAMPLES), "--path-seed", str(SEED), "--path-bounces", "1",
            "--capture-sequence", str(sequence), "--screenshot", str(output / f"{name}{suffix}"), *flags,
        ]
        record = {"command": command, "log": str(log), "frames": []}
        report["captures"][name] = record
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=360, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer"),
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        content = log.read_text(encoding="utf-8", errors="replace")
        assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in content, log
        assert not validation_log_error(content), log
        images = []
        for frame in frames:
            stem = output / (name if frame == frames[-1] else f"{name}.frame-{frame:04d}")
            image = Path(f"{stem}{suffix}")
            telemetry = Path(f"{stem}.telemetry.json")
            state = json.loads(telemetry.read_bytes())
            metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
            pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
            assert pixels is not None and pixels.shape[:2] == (HEIGHT, WIDTH), image
            expected_source = "viewport" if suffix == ".png" else (
                "path-trace-mean" if state["technique"] == "path-tracing" else "raster-scene-composite")
            assert metadata["source"] == expected_source and not metadata["includesUi"], metadata
            assert pixels.dtype == (np.uint8 if suffix == ".png" else np.float32) and np.isfinite(pixels).all(), image
            assert state["validationEnabled"], telemetry
            path = state.get("referencePath")
            if path:
                raw = metadata["referencePath"]
                assert path["active"] and path["seed"] == SEED and path["attemptedSamples"] == SAMPLES, path
                assert path["allocatedBytes"] > 0 and path["rejectedSamples"] == 0, path
                assert raw["attemptedSamplesPerPixel"] == SAMPLES and raw["unsupportedSamples"] == path["unsupportedSamples"], raw
                assert raw["status"] == path["status"], raw
            record["frames"].append({
                "image": str(image), "telemetry": str(telemetry), "technique": state["technique"],
                "referencePath": path, "lightingPass": next(p for p in state["passes"] if p["name"] == "Lighting"),
            })
            images.append((pixels[..., :3][..., ::-1], state))
        return images

    def check_reference(sample, *, unsupported=False, reason=None):
        pixels, state = sample
        assert state["technique"] == "path-tracing", state["technique"]
        path = state["referencePath"]
        assert path["unsupportedSamples"] == (WIDTH * HEIGHT * SAMPLES if unsupported else 0), path
        if reason:
            assert reason in path["status"], path
        if unsupported and pixels.dtype == np.float32:
            np.testing.assert_array_equal(pixels, 0)
        return pixels

    try:
        ordinary = check_reference(capture("mesh-supported", fixtures["mesh"])[0])
        unlit = check_reference(capture("mesh-unlit", fixtures["unlit"])[0])
        np.testing.assert_array_equal(unlit, ordinary)
        roi = np.s_[HEIGHT//2-6:HEIGHT//2+6, WIDTH//2-6:WIDTH//2+6]
        projection = 2 * CAMERA_Z * math.tan(math.radians(FOV / 2)) / HEIGHT
        offsets = (np.arange(-6, 6) + .5) * projection
        expected = np.mean([rectangle_reference(x, y) for x in offsets for y in offsets], axis=0)
        measured = ordinary[roi].mean(axis=(0, 1))
        np.testing.assert_allclose(measured, expected, atol=.00015, rtol=.025)
        report["checks"]["visibleEmitterLambertIntegral"] = {
            "measuredRgb": measured.tolist(), "expectedRgb": expected.tolist(),
            "absoluteTolerance": .00015, "relativeTolerance": .025,
        }
        check_reference(capture("mesh-unsupported", fixtures["sheen"])[0], unsupported=True)
        marker = check_reference(capture("mesh-unsupported-display", fixtures["sheen"], suffix=".png")[0],
                                 unsupported=True)
        assert np.all(marker[roi][..., [0, 2]] > 170) and np.all(marker[roi][..., 1] < 65), marker[roi].mean(axis=(0, 1))
        blocked = check_reference(capture("mesh-blocked", fixtures["blocked"])[0])
        np.testing.assert_array_equal(blocked, 0)
        report["checks"]["meshLightSupport"] = {
            "visibleUnsupportedSamples": WIDTH * HEIGHT * SAMPLES, "visibleUnsupportedRawMaximum": 0,
            "viewportDiagnosticRgb": marker[roi].mean(axis=(0, 1)).tolist(),
            "occludedUnsupportedSamples": 0, "unlitMatchesSupported": True,
        }

        filtered = {}
        literal = np.array([1, 0, 1]) / math.pi
        normal = np.array(NORMAL_TEXEL) * (2 / 255) - 1
        tilted = literal * normal[2] / np.linalg.norm(normal)
        # The glTF image uses the sRGB endpoints; normalized image storage can
        # round their linear average to a neighboring 8-bit sRGB code.
        for camera, flags in (("perspective", ()), ("orthographic", ("--gltf-camera-node", "3"))):
            sample = capture(f"filtered-{camera}", fixtures["filtered"], "--directional-intensity", "2",
                             "--directional-direction", "0", "0", "-1", "--directional-color", "1", "1", "1", *flags)[0]
            pixels = check_reference(sample)
            clip_w_from_world_z = sample[1]["camera"]["unjitteredViewProjColumns"][2][3]
            assert clip_w_from_world_z == (-1 if camera == "perspective" else 0), sample[1]["camera"]
            regions = {
                "literal": (slice(WIDTH//8, WIDTH//4), literal),
                "checker": (slice(WIDTH//2-10, WIDTH//2+10), literal),
                "normal": (slice(3*WIDTH//4, 7*WIDTH//8), tilted),
            }
            filtered[camera] = {}
            for name, (region, expected) in regions.items():
                image = pixels[HEIGHT//2-12:HEIGHT//2+12, region]
                tolerance = .0001 if name == "literal" else .006
                np.testing.assert_allclose(image, np.broadcast_to(expected, image.shape), atol=tolerance, rtol=0)
                filtered[camera][name] = {
                    "expectedRgb": expected.tolist(), "meanRgb": image.mean(axis=(0, 1)).tolist(),
                    "maxAbsoluteError": float(np.abs(image - expected).max()), "absoluteTolerance": tolerance,
                }
        report["checks"]["primaryMipAndNormal"] = filtered

        events = ({"frame": 5, "volumetricFogMode": "high"}, {"frame": 9, "volumetricFogMode": "off"})
        frames = capture("fog-switch", fixtures["panel"], "--fog-density", str(FOG_DENSITY), "--fog-albedo", "0",
                         frames=(4, 8, 12), events=events)
        panel = check_reference(frames[0])
        np.testing.assert_allclose(panel[roi], np.broadcast_to(EMISSION, panel[roi].shape), atol=.0001, rtol=0)
        check_reference(frames[1], unsupported=True, reason="raster froxel fog transport")
        np.testing.assert_array_equal(check_reference(frames[2]), panel)
        resets = [state["referencePath"]["historyResets"] for _, state in frames]
        assert resets == [1, 2, 3], resets
        zero = check_reference(capture("fog-zero-density", fixtures["panel"], "--volumetric-fog", "high",
                                       "--fog-density", "0")[0])
        np.testing.assert_array_equal(zero, panel)
        raster, state = capture("fog-raster-fallback", fixtures["panel"], "--no-ray-query", "--volumetric-fog", "high",
                                "--fog-density", str(FOG_DENSITY), "--fog-albedo", "0")[0]
        assert state["technique"] == "deferred-raster" and state["volumetricFog"]["active"], state
        expected_raster = np.array(EMISSION) * math.exp(-FOG_DENSITY * CAMERA_Z)
        measured_raster = raster[roi].mean(axis=(0, 1))
        np.testing.assert_allclose(measured_raster, expected_raster, atol=.006, rtol=0)
        report["checks"]["fogTransport"] = {
            "historyResets": resets, "disabledAndZeroDensityExact": True, "rasterFallbackActive": True,
            "rasterMeasuredRgb": measured_raster.tolist(), "rasterExpectedRgb": expected_raster.tolist(),
            "rasterAbsoluteTolerance": .006,
        }

        if args.openvdb_tool is not None:
            tool = args.openvdb_tool.resolve(strict=True)
            sphere, density = output / "sphere.vdb", output / "density.vdb"
            commands = [
                [str(tool), "--sphere", str(sphere), "--radius", "1", "--voxel-size", ".2"],
                [str(tool), "--fog", str(sphere), str(density), "--grid-name", "density"],
            ]
            report["openVdb"] = {"commands": commands, "passed": False}
            for index, command in enumerate(commands):
                with (output / f"openvdb-fixture-{index}.log").open("w", encoding="utf-8") as stream:
                    subprocess.run(command, cwd=tool.parent, stdout=stream, stderr=subprocess.STDOUT,
                                   check=True, timeout=60)
            assert density.is_file(), density
            report["fixtureSha256"][str(density)] = sha256(density)
            volume_flags = ("--vdb-volume", str(density), "--vdb-grid", "density")
            volume = capture("vdb-static", fixtures["panel"], *volume_flags)[0]
            check_reference(volume, unsupported=True, reason="OpenVDB volume transport")
            disabled = capture("vdb-disabled", fixtures["panel"], *volume_flags, "--no-vdb-volume")[0]
            np.testing.assert_array_equal(check_reference(disabled), panel)
            report["openVdb"]["passed"] = True

        assert all(sha256(Path(path)) == digest for path, digest in runtime_hashes.items()), "Runtime changed during capture"
        report["passed"] = True
    finally:
        result.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS reference material diagnostics, fog switching and raster fallback", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
