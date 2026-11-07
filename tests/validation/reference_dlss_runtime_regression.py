"""Validate Vulkan DLSS Ray Reconstruction, raw fallback and viewport history."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")

import cv2
import numpy as np
from PIL import Image

from display_capture_test_support import Asset, validation_log_error


ROOT = Path(__file__).resolve().parents[2]
EXTENT = (640, 360)
RESIZED = (704, 400)
LOW_SAMPLE_FRAMES = (1, 4, 8)
MOTION_FRAMES = (1, 3, 6, 9, 11, 12, 15, 18, 21, 23, 25, 27, 29, 32, 35)


def write_motion_glass_fixture(path):
    """Create three independently placed surfaces with stable first-hit identities."""
    asset = Asset()
    material_base = {"metallicFactor": 0, "roughnessFactor": 1, "baseColorFactor": [0, 0, 0, 1]}
    material_specular = {"KHR_materials_specular": {"specularFactor": 0}}
    asset.model["materials"] = [
        {"name": "Blue emitting backdrop", "pbrMetallicRoughness": material_base,
         "emissiveFactor": [.1, .28, .7], "extensions": material_specular},
        {"name": "Red motion marker", "pbrMetallicRoughness": material_base,
         "emissiveFactor": [1, .035, .015], "extensions": {
             **material_specular, "KHR_materials_emissive_strength": {"emissiveStrength": 4}}},
        {"name": "Tinted thin glass", "pbrMetallicRoughness": {
            **material_base, "baseColorFactor": [.35, .9, .55, 1], "roughnessFactor": 0},
         "extensions": {**material_specular, "KHR_materials_ior": {"ior": 1},
                        "KHR_materials_transmission": {"transmissionFactor": 1}}},
    ]
    asset.model["extensionsUsed"] = sorted({name for material in asset.model["materials"]
                                            for name in material["extensions"]})
    placements = (
        ("Backdrop", -9, 9, -6, 6, -6),
        ("Moving red card", -1.02, -.32, -.57, .57, -3),
        ("Glass card", .65, 1.35, -.8, .8, -2.5),
    )
    asset.model["nodes"] = []
    asset.model["scenes"] = [{"nodes": list(range(len(placements)))}]
    for index, (name, left, right, bottom, top, z) in enumerate(placements):
        positions = [[left, bottom, z], [right, bottom, z], [right, top, z], [left, top, z]]
        primitive = dict(attributes=dict(POSITION=asset.accessor("VEC3", positions),
                                         NORMAL=asset.accessor("VEC3", [[0, 0, 1]] * 4)),
                         indices=asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True),
                         material=index)
        asset.model["meshes"].append({"primitives": [primitive]})
        asset.model["nodes"].append({"name": name, "mesh": index})
    asset.write(path)


def motion_sequence(use_dlss):
    # Dolly more than five units in one frame while keeping the red card visible.
    camera_xz = ((1, 0, 0), (3, 0, 0), (6, .22, 0), (10, .22, 0),
                 (11, -2.5, 5.1), (29, -2.5, 5.1))
    events = [{"frame": 13, "resize": list(RESIZED)}, {"frame": 26, "technique": "deferred-raster"},
              {"frame": 28, "technique": "path-tracing"}]
    if use_dlss:
        events += [{"frame": frame, "referenceDlss": quality} for frame, quality in
                   ((16, "balanced"), (19, "performance"), (22, "off"), (24, "quality"), (33, "quality"))]
        events.append({"frame": 30, "referenceOidn": "fast"})
    return {
        "schemaVersion": 1, "frames": MOTION_FRAMES[-1], "sampleFrames": list(MOTION_FRAMES),
        "camera": [{"frame": frame, "position": [x, 0, z], "target": [x, 0, z - 1]}
                   for frame, x, z in camera_xz],
        "objectName": "Moving red card",
        "objectTranslation": [{"frame": frame, "translation": [x, 0, 0]}
                              for frame, x in ((1, 0), (6, 0), (9, .88), (29, .88))],
        "events": sorted(events, key=lambda event: event["frame"]),
    }


def fixed_sequence(frames):
    return {"schemaVersion": 1, "frames": max(frames), "sampleFrames": list(frames)}


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


@dataclass
class Capture:
    state: dict
    pixels: np.ndarray
    image: Path

    @property
    def path(self):
        return self.state.get("referencePath", {})

    @property
    def dlss(self):
        return self.path["dlss"]


def assert_mode(sample, quality, active, extent):
    state = sample.dlss
    assert state["quality"] == quality and state["active"] is active, state
    assert sample.pixels.shape == (extent[1], extent[0], 3), sample.image
    if active:
        assert state["outputExtent"] == list(extent), state
        assert len(state["renderExtent"]) == 2 and all(0 < source < target for source, target in
                zip(state["renderExtent"], extent, strict=True)), state
        assert state["historyResets"] >= 1 and state["historyFrames"] >= 1, state
        assert state["allocatedBytes"] > 0, state
        assert state["vendorAllocatedBytes"] is None or state["vendorAllocatedBytes"] >= 0, state
    elif quality != "off":
        assert state["status"] and state["status"].lower() not in ("off", "ready"), state


def pixel_exact(first, second):
    assert first.pixels.shape == second.pixels.shape and first.pixels.dtype == second.pixels.dtype
    if first.pixels.dtype == np.float32:
        np.testing.assert_array_equal(first.pixels.view(np.uint32), second.pixels.view(np.uint32))
    else:
        np.testing.assert_array_equal(first.pixels, second.pixels)
    return {"pixelBitExact": True, "encodedFileExact": sha256(first.image) == sha256(second.image)}


def display_quality(raw, reconstructed, converged):
    target = converged.pixels.astype(np.float32) / 255
    assert float(np.std(target)) > .025, "The converged scene has insufficient signal"
    rows = {}
    for frame in LOW_SAMPLE_FRAMES:
        reference = raw[frame].pixels.astype(np.float32) / 255
        output = reconstructed[frame].pixels.astype(np.float32) / 255
        assert float(np.std(output)) > .02, reconstructed[frame].image
        rows[frame] = {
            "rawRmse": float(np.sqrt(np.mean(np.square(reference - target)))),
            "rrRmse": float(np.sqrt(np.mean(np.square(output - target)))),
            "rawVsRrMae": float(np.mean(np.abs(reference - output))),
            "rawAttemptedSamples": raw[frame].path["attemptedSamples"],
            "rrHistoryFrames": reconstructed[frame].dlss["historyFrames"],
        }
    assert rows[LOW_SAMPLE_FRAMES[-1]]["rawVsRrMae"] > .002, rows
    assert all(row["rrRmse"] < row["rawRmse"] for row in rows.values()), rows
    return rows


def red_mask(pixels):
    rgb = pixels.astype(np.float32) / 255
    return (rgb[:, :, 0] > .3) & (rgb[:, :, 0] > 1.7 * np.maximum(rgb[:, :, 1], rgb[:, :, 2]))


def motion_quality(raw, reconstructed):
    results = {}
    for frame in (3, 6, 9, 11, 12, 15, 18, 21, 25, 29):
        a, b = red_mask(raw[frame].pixels), red_mask(reconstructed[frame].pixels)
        assert min(np.count_nonzero(a), np.count_nonzero(b)) > 512, (frame, reconstructed[frame].image)
        iou = float(np.count_nonzero(a & b) / np.count_nonzero(a | b))
        raw_x = float(np.where(a)[1].mean())
        rr_x = float(np.where(b)[1].mean())
        assert iou > .7 and abs(raw_x - rr_x) < 12, (frame, iou, raw_x, rr_x)
        results[frame] = {"redMaskIou": iou, "rawCenterX": raw_x, "rrCenterX": rr_x}
    for name, first, second, minimum in (("camera", 3, 6, 15), ("object", 6, 9, 70), ("cut", 9, 11, 100)):
        expected = results[second]["rawCenterX"] - results[first]["rawCenterX"]
        actual = results[second]["rrCenterX"] - results[first]["rrCenterX"]
        assert abs(expected) > minimum and abs(expected - actual) < 14, (name, expected, actual)

    def tint_delta(sample):
        image = sample.pixels.astype(np.float32) / 255
        height, width = image.shape[:2]
        rows = slice(round(height * .35), round(height * .65))
        through = image[rows, round(width * .73):round(width * .78)].mean(axis=(0, 1))
        outside = image[rows, round(width * .57):round(width * .62)].mean(axis=(0, 1))
        return through - outside

    native_tint, rr_tint = tint_delta(raw[3]), tint_delta(reconstructed[3])
    signal = float(np.linalg.norm(native_tint))
    assert signal > .04 and float(np.dot(native_tint, rr_tint)) > .5 * signal * signal, (native_tint, rr_tint)
    return {"frames": results, "thinGlassNativeDisplayDelta": native_tint.tolist(),
            "thinGlassRrDisplayDelta": rr_tint.tolist()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", type=Path, default=ROOT / "models/validation/cornell_box_local_light.gltf")
    parser.add_argument("--expect-unavailable", action="store_true", help="Require bit-exact disabled SDK fallback")
    parser.add_argument("--suite", choices=("full", "smoke"), default="full")
    args = parser.parse_args()
    renderer, output, model = (args.renderer.resolve(strict=True), args.output.resolve(),
                               args.model.resolve(strict=True))
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))
    report_path = output / "results.json"
    report = {"passed": False, "sdkFallbackExpected": args.expect_unavailable,
              "executableSha256": sha256(renderer), "runs": {}}

    def write_report():
        report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")

    def capture(label, quality, config, *, selected_model=model, extension=".png", samples=4, batch=1,
                position=(0, 1, 4), target=(0, 1, 0), bounces=5):
        sequence = output / f"{label}.sequence.json"
        sequence.write_text(json.dumps(config), encoding="utf-8")
        command = [str(renderer), "--hidden", "--validation", "--no-ui", "--screenshot-viewport",
                   "--width", str(EXTENT[0]), "--height", str(EXTENT[1]), "--msaa", "1",
                   "--model", str(selected_model), "--import-scale", "1", "--display-mode", "lit",
                   "--render-technique", "path-tracing", "--no-taa", "--no-bloom", "--no-ddgi",
                   "--environment-intensity", "0", "--directional-intensity", "0", "--exposure", ".25",
                   "--camera-position", *map(str, position), "--camera-target", *map(str, target),
                   "--camera-fov", "42", "--fixed-dt", ".016666667", "--path-seed", "17",
                   "--path-bounces", str(bounces), "--path-spp", str(batch), "--path-samples", str(samples),
                   "--path-dlss", quality, "--capture-sequence", str(sequence),
                   "--screenshot", str(output / f"{label}{extension}")]
        log = output / f"{label}.log"
        run = report["runs"][label] = {"log": str(log), "command": command, "captures": {}}
        write_report()
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                     timeout=300, env=env, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        log_text = log.read_text(encoding="utf-8", errors="replace")
        if process.returncode or validation_log_error(log_text):
            raise RuntimeError(f"Vulkan RR capture failed ({process.returncode}): {log}")

        frames = {}
        for frame in config["sampleFrames"]:
            name = label if frame == config["frames"] else f"{label}.frame-{frame:04d}"
            image = output / f"{name}{extension}"
            telemetry = output / f"{name}.telemetry.json"
            state = json.loads(telemetry.read_text(encoding="utf-8"))
            metadata = json.loads(Path(f"{image}.capture.json").read_text(encoding="utf-8"))
            if extension == ".exr":
                pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
                assert pixels is not None and pixels.dtype == np.float32 and pixels.ndim == 3, image
                assert np.isfinite(pixels).all(), image
                pixels = np.ascontiguousarray(pixels[:, :, ::-1])
                assert metadata["source"] == "path-trace-mean", metadata
            else:
                with Image.open(image) as decoded:
                    pixels = np.asarray(decoded.convert("RGB"))
            sample = frames[frame] = Capture(state, pixels, image)
            for key in ("gpu", "driverVersion", "vulkanApiVersion"):
                report.setdefault(key, state[key])
            if state["technique"] == "path-tracing":
                assert sample.path["active"] and sample.path["rejectedSamples"] == 0, sample.path
                assert sample.path["unsupportedSamples"] == 0, sample.path
            run["captures"][frame] = {
                "image": str(image), "telemetry": str(telemetry), "technique": state["technique"],
                "source": metadata["source"],
                "rawAttemptedSamples": sample.path.get("attemptedSamples"), "rr": sample.path.get("dlss"),
                "oidn": sample.path.get("oidn"), "optix": sample.path.get("optix"),
                "gpuKnownMs": state["gpuKnownMs"],
            }
        active_times = [sample.dlss["lastGpuMs"] for sample in frames.values()
                        if sample.path.get("dlss", {}).get("active") and (sample.dlss["lastGpuMs"] or 0) > 0]
        run["rrMedianGpuMs"] = statistics.median(active_times) if active_times else None
        run["medianKnownGpuMs"] = statistics.median(sample.state["gpuKnownMs"] for sample in frames.values())
        write_report()
        print(f"{label}: {len(frames)} Vulkan-validation-clean captures", flush=True)
        return frames

    try:
        low = fixed_sequence(LOW_SAMPLE_FRAMES)
        raw = capture("cornell-native", "off", low)
        reconstructed = capture("cornell-quality", "quality", low)
        for frame, attempts in zip(LOW_SAMPLE_FRAMES, (1, 4, 4), strict=True):
            assert_mode(raw[frame], "off", False, EXTENT)
            assert_mode(reconstructed[frame], "quality", not args.expect_unavailable, EXTENT)
            assert raw[frame].path["attemptedSamples"] == attempts, raw[frame].path
            assert reconstructed[frame].path["attemptedSamples"] == attempts, reconstructed[frame].path

        native_exr = capture("raw-native", "off", fixed_sequence((8,)), extension=".exr")[8]
        requested_exr = capture("raw-requested", "quality", fixed_sequence((8,)), extension=".exr")[8]
        assert_mode(native_exr, "off", False, EXTENT)
        assert requested_exr.pixels.shape == (EXTENT[1], EXTENT[0], 3), requested_exr.image
        report["rawExr"] = pixel_exact(native_exr, requested_exr)

        if args.expect_unavailable:
            report["displayFallback"] = {frame: pixel_exact(raw[frame], reconstructed[frame])
                                         for frame in LOW_SAMPLE_FRAMES}
            report["capabilityReason"] = reconstructed[8].dlss["status"]
        else:
            assert reconstructed[8].dlss["historyFrames"] > reconstructed[4].dlss["historyFrames"], reconstructed[8].dlss
            assert report["runs"]["cornell-quality"]["rrMedianGpuMs"] is not None, reconstructed[8].dlss
            if args.suite == "full":
                converged = capture("cornell-converged", "off", fixed_sequence((16,)), samples=128, batch=8)[16]
                assert converged.path["attemptedSamples"] == 128, converged.path
                report["displaySpaceQuality"] = display_quality(raw, reconstructed, converged)
                witness = output / "motion-glass.gltf"
                write_motion_glass_fixture(witness)
                controls = dict(selected_model=witness, position=(0, 0, 0), target=(0, 0, -1), bounces=2)
                motion_raw = capture("motion-native", "off", motion_sequence(False), **controls)
                motion_rr = capture("motion-rr", "quality", motion_sequence(True), **controls)
                expected = {1: "quality", 3: "quality", 6: "quality", 9: "quality", 11: "quality", 12: "quality",
                            15: "quality", 18: "balanced", 21: "performance", 23: "off",
                            25: "quality", 29: "quality", 32: "off", 35: "quality"}
                for frame, mode in expected.items():
                    sample = motion_rr[frame]
                    assert_mode(sample, mode, mode != "off", EXTENT if frame < 13 else RESIZED)
                    assert sample.path["oidn"]["quality"] == ("fast" if frame == 32 else "off"), sample.path
                    assert not sample.path["optix"]["requested"], sample.path
                assert motion_rr[27].state["technique"] == "deferred-raster", motion_rr[27].state
                raster_path = motion_rr[27].path
                assert not raster_path.get("active") and not raster_path.get("dlss", {}).get("active"), raster_path
                moving_states = [motion_rr[frame].dlss for frame in (3, 6, 9)]
                assert len({entry["historyResets"] for entry in moving_states}) == 1, moving_states
                assert all(after["historyFrames"] > before["historyFrames"] for before, after in
                           zip(moving_states, moving_states[1:])), moving_states
                cut, following = motion_rr[11].dlss, motion_rr[12].dlss
                assert cut["historyResets"] == moving_states[-1]["historyResets"] + 1, cut
                assert following["historyResets"] == cut["historyResets"], following
                assert (cut["historyFrames"], following["historyFrames"]) == (1, 2), (cut, following)
                for frame in (15, 18, 21, 25, 35):
                    assert motion_rr[frame].dlss["historyResets"] == 1, motion_rr[frame].dlss
                restored = motion_rr[29].dlss
                assert restored["historyFrames"] == 2 and restored["historyResets"] in (
                    1, motion_rr[25].dlss["historyResets"] + 1), restored
                source_extents = [motion_rr[frame].dlss["renderExtent"] for frame in (15, 18, 21)]
                assert all(quality > balanced > performance for quality, balanced, performance in
                           zip(*source_extents, strict=True)), source_extents
                report["motionAndGlass"] = motion_quality(motion_raw, motion_rr)
        assert sha256(renderer) == report["executableSha256"], "Renderer binary changed during capture"
        report["passed"] = True
    finally:
        write_report()
    print(f"PASS Vulkan DLSS Ray Reconstruction{' fallback' if args.expect_unavailable else ''}: {report_path}")


if __name__ == "__main__":
    main()
