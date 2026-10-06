"""Opt-in area-shadow profiles against converged segment/AABB visibility."""
from __future__ import annotations
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("SKIP: area-shadow profiles require numpy and Pillow")
    raise SystemExit(77)


def hdr(path):
    with Image.open(path) as image:
        srgb = np.asarray(image.convert("RGB"), dtype=np.float64) / 255
    mapped = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
    # Invert the renderer's ACES rational curve; exposure cancels in ratios.
    a, b, c = 2.43 * mapped - 2.51, .59 * mapped - .03, .14 * mapped
    return (-b - np.sqrt(np.maximum(b * b - 4 * a * c, 0))) / (2 * a)


def floor_points(telemetry, row, width=960, height=540):
    camera = telemetry["camera"]
    inverse = np.linalg.inv(np.array(camera["unjitteredViewProjColumns"]).T)
    x = np.arange(width)
    ndc = np.column_stack((2 * (x + .5) / width - 1,
                           np.full(width, 1 - 2 * (row + .5) / height),
                           np.ones(width), np.ones(width)))
    near = ndc @ inverse.T
    near = near[:, :3] / near[:, 3:4]
    origin = np.array(camera["position"])
    direction = near - origin
    points = origin + direction * (-origin[1] / direction[:, 1:2])
    valid = (np.abs(points[:, 0]) < .90) & (points[:, 2] > .38) & (points[:, 2] < .98)
    return points[valid], valid


def reference(points, size, height, disk=False, resolution=128, source_x=0):
    # Independent uniform area integration. Rays intersect the complete solid
    # blocker, including side faces; no production PCSS/filter code is reused.
    q = (np.arange(resolution) + .5) / resolution
    a, b = np.meshgrid(q, q)
    if disk:
        x, z = np.sqrt(a) * np.cos(2 * np.pi * b), np.sqrt(a) * np.sin(2 * np.pi * b)
    else:
        x, z = 2 * a - 1, 2 * b - 1
    emitter = np.column_stack((source_x + x.ravel() * size[0] / 2,
                               np.full(x.size, height), .18 + z.ravel() * size[1] / 2))
    lower, upper = np.array([-.24, 0, -.15]), np.array([.24, .72, .36])
    results = []
    for start in range(0, len(points), 16):
        p = points[start:start + 16, None, :]
        direction = emitter[None, :, :] - p
        with np.errstate(divide="ignore", invalid="ignore"):
            t0, t1 = (lower - p) / direction, (upper - p) / direction
        enter = np.maximum(np.minimum(t0, t1).max(axis=2), 1e-6)
        leave = np.minimum(np.maximum(t0, t1).min(axis=2), 1 - 1e-6)
        blocked = enter <= leave
        distance_sq = np.sum(direction * direction, axis=2)
        weight = np.maximum(1 - distance_sq / 3.2 ** 2, 0) ** 2 * height / distance_sq ** 1.5
        results.extend(np.sum(weight * ~blocked, axis=1) / np.sum(weight, axis=1))
    return np.array(results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", default="")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_AREA_SHADOW") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_AREA_SHADOW=1")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    original = json.loads((exe.parent / "models/validation/cornell_box_local_light.gltf").read_text())
    results = []

    oracle_cache = {}

    def fixture(name, size, height, disk, blocker=True, source_x=0):
        doc = copy.deepcopy(original)
        primitives = doc["meshes"][0]["primitives"]
        doc["meshes"][0]["primitives"] = [primitives[0]]
        if name.startswith("transparent"):
            doc["materials"][0]["alphaMode"] = "BLEND"
            doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][3] = .7
        doc["meshes"].append({"primitives": primitives[5:]})
        if blocker:
            doc["nodes"].append({"name": "Shadow blocker", "mesh": 1})
            doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
        light = doc["extensions"]["KHR_lights_punctual"]["lights"][0]
        light["extras"]["areaLight"].update(shape="disk" if disk else "rect",
            width=size[0], height=size[1], radius=size[0] / 2)
        doc["nodes"][1]["translation"][1] = height
        doc["nodes"][1]["translation"][0] = source_x
        # Preserve the isolated Cornell scene lighting preset for generated files.
        path = output / name / "models/validation/cornell_box_local_light.gltf"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(doc))
        return path

    def capture(name, model, technique, quality, taa=False, sequence=None):
        screenshot, log = output / (name + ".png"), output / (name + ".log")
        for stale in [screenshot, *output.glob(name + ".frame-*.png")]:
            stale.unlink(missing_ok=True)
        command = [str(exe), "--model", str(model), "--hidden", "--no-ui", "--validation",
            "--no-ray-query",
            "--no-bloom", "--msaa", "1", "--width", "960", "--height", "540",
            "--display-mode", "lit", "--camera-position", "0", "1.05", "4.1",
            "--camera-target", "0", ".95", "0", "--camera-fov", "36",
            "--environment-intensity", "0", "--directional-intensity", "0",
            "--exposure", ".03", "--render-technique", technique,
            "--taa" if taa else "--no-taa", "--area-shadow-quality", str(quality),
            "--screenshot", str(screenshot)]
        if sequence:
            command += ["--capture-sequence", str(sequence)]
        with log.open("w") as stream:
            process = subprocess.run(command, cwd=exe.parent,
                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                stdout=stream, stderr=subprocess.STDOUT, timeout=180)
        text = log.read_text(errors="replace")
        assert process.returncode == 0 and "VUID-" not in text and "SYNC-HAZARD" not in text, log
        telemetry = json.loads(screenshot.with_suffix(".telemetry.json").read_text())
        assert telemetry["lighting"]["activeLocalShadowLayers"] == 24, telemetry["lighting"]
        assert telemetry["lighting"]["bounceIntensity"] == 0, telemetry["lighting"]
        assert telemetry["lighting"]["areaShadowQuality"] == quality
        return screenshot, telemetry

    cases = [("rect", (.8, .8), 1.86, False, 0),
             ("small-near", (.4, .4), 1.5, False, 0),
             ("wide", (1.2, .4), 1.86, False, 0),
             ("disk", (.8, .8), 1.86, True, 0),
             ("transparent", (.8, .8), 1.86, False, 0),
             ("cube-seam", (.4, .4), 1.5, False, .6)]
    for name, size, height, disk, source_x in cases:
        if args.case and name != args.case:
            continue
        blocked = fixture(name, size, height, disk, source_x=source_x)
        clear = fixture(name + "-clear", size, height, disk, False, source_x)
        for technique in ("deferred-raster", "forward-raster"):
            prefix = name + "-" + technique
            unoccluded, telemetry = capture(prefix + "-clear", clear, technique, 2)
            clear_hdr = hdr(unoccluded)
            profiles = []
            for row in (485, 490, 495):
                points, mask = floor_points(telemetry, row)
                key = (size, height, disk, source_x, row)
                if key not in oracle_cache:
                    oracle_cache[key] = (points,
                        reference(points, size, height, disk, 256, source_x),
                        reference(points, size, height, disk, 128, source_x))
                cached_points, expected, coarse = oracle_cache[key]
                assert np.max(np.abs(points - cached_points)) < 1e-5
                convergence = float(np.abs(expected - coarse).mean())
                assert convergence < .003, (name, row, convergence)
                penumbra = (expected > .05) & (expected < .95)
                assert penumbra.sum() > 10, (name, row, "missing penumbra")
                profiles.append((row, mask, expected, penumbra, convergence))
            errors = {}
            bands = {}
            for quality in (0, 1, 2, 3):
                image, telemetry = capture(prefix + f"-q{quality}", blocked, technique, quality)
                actual_hdr = hdr(image)
                samples, expected_values = [], []
                plateaus, windows = 0, 0
                for row, mask, expected, penumbra, convergence in profiles:
                    # Mean per-channel ratios reduce 8-bit inverse-tonemap noise.
                    actual = np.mean(actual_hdr[row, mask] / np.maximum(clear_hdr[row, mask], 1e-5), axis=1)
                    samples.extend(actual[penumbra]); expected_values.extend(expected[penumbra])
                    for start in range(len(expected) - 9):
                        span = slice(start, start + 9)
                        if penumbra[span].all() and np.ptp(expected[span]) > .06:
                            windows += 1
                            plateaus += np.ptp(actual[span]) < .025
                error = np.abs(np.array(samples) - np.array(expected_values))
                errors[quality] = float(error.mean())
                bands[quality] = plateaus / max(windows, 1)
                result = dict(case=name, technique=technique, quality=quality,
                    penumbraMae=errors[quality], p95=float(np.percentile(error, 95)),
                    samples=len(samples), plateauFraction=bands[quality],
                    referenceConvergence=max(p[-1] for p in profiles),
                    gpuKnownMs=telemetry["gpuKnownMs"], lighting=telemetry["lighting"],
                    validationClean=True)
                results.append(result)
                (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                print(name, technique, quality, result["penumbraMae"], "bands", bands[quality], flush=True)
            # The intentionally fixed-origin mode must fail the improvement gate.
            assert errors[1] < errors[0] * .8 and errors[1] < .04, (prefix, errors)
            assert errors[2] < errors[0] * .65, (prefix, errors)
            assert errors[3] < errors[0] * .65, (prefix, errors)
            assert errors[2] < .075 and errors[3] < .075, (prefix, errors)
            if bands[0] > .25:
                assert bands[1] < .25 and bands[2] < .25 and bands[3] < .20, (prefix, bands)

    # Exercise fresh shadow maps with moving geometry and edited light position,
    # including a hold after motion stops. No visibility-specific history exists.
    if not args.case:
        moving_model = fixture("motion", (.8, .8), 1.86, False)
        samples = [1, 8, 16, 20, 24, 32]
        for motion in ("blocker", "light"):
            script = dict(schemaVersion=1, frames=32, sampleFrames=samples)
            if motion == "blocker":
                script.update(objectName="Shadow blocker", objectTranslation=[
                    dict(frame=1, translation=[0, 0, 0]),
                    dict(frame=8, translation=[-.25, 0, 0]),
                    dict(frame=16, translation=[.25, 0, 0]),
                    dict(frame=20, translation=[0, 0, 0])])
            else:
                script["events"] = [dict(frame=frame, areaLightPosition=[x, 1.86, .18])
                    for frame, x in ((1, 0), (8, -.25), (16, .25), (20, 0))]
            sequence = output / (motion + "-motion.json")
            sequence.write_text(json.dumps(script))
            for technique in ("deferred-raster", "forward-raster"):
                reference_image, _ = capture(motion + "-" + technique + "-taa0",
                    moving_model, technique, 2, sequence=sequence)
                candidate_image, _ = capture(motion + "-" + technique + "-taa1",
                    moving_model, technique, 2, taa=True, sequence=sequence)
                for frame in samples:
                    def frame_path(image):
                        if frame == 32:
                            return image
                        return image.with_name(image.stem + f".frame-{frame:04d}.png")
                    expected_path, actual_path = frame_path(reference_image), frame_path(candidate_image)
                    with Image.open(expected_path) as image:
                        expected = np.asarray(image.convert("RGB"), dtype=np.float64) / 255
                    with Image.open(actual_path) as image:
                        actual = np.asarray(image.convert("RGB"), dtype=np.float64) / 255
                    # Floor in front of the blocker: geometry never moves here.
                    region = (slice(485, 525), slice(260, 700))
                    error = np.abs(expected[region] - actual[region])
                    mae, p95 = float(error.mean()), float(np.percentile(error, 95))
                    assert mae < (.006 if frame >= 24 else .025), (motion, technique, frame, mae)
                    assert p95 < (.025 if frame >= 24 else .10), (motion, technique, frame, p95)
                    reference_telemetry = json.loads(expected_path.with_suffix(".telemetry.json").read_text())
                    if motion == "light" and frame in (8, 16):
                        x = -.25 if frame == 8 else .25
                        assert abs(reference_telemetry["lighting"]["areaLightPositions"][0][0] - x) < 1e-5
                    results.append(dict(motion=motion, technique=technique, frame=frame,
                        taaMae=mae, taaP95=p95, validationClean=True))
                    print(motion, technique, frame, "TAA MAE", mae, "P95", p95, flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
