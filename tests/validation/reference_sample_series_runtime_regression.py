"""Validate exact, deterministic scene-linear reference capture checkpoints."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np

from display_capture_test_support import emitter_fixture, validation_log_error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer = args.renderer.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")

    emitter = output / "emitter.gltf"
    material = {
        "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]},
        "emissiveFactor": [1, .5, .25],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4}},
        "doubleSided": True,
    }
    emitter_fixture(emitter, material)

    def capture(name, model, *options, error=None):
        command = [
            str(renderer), "--model", str(model), "--hidden", "--no-ui",
            "--validation", "--width", "320", "--height", "240", "--no-bloom",
            "--render-technique", "path-tracing", "--path-bounces", "2",
            "--path-seed", "23", "--environment-intensity", "0",
            "--directional-intensity", "0", "--camera-position", "0", "0", "4",
            "--camera-target", "0", "0", "0", "--camera-fov", "45", *options,
        ]
        result = subprocess.run(command, cwd=renderer.parent, capture_output=True,
                                text=True, errors="replace", timeout=180,
                                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        log = result.stdout + result.stderr
        log_path = output / f"{name}.log"
        log_path.write_text(log, encoding="utf-8")
        assert not validation_log_error(log), f"Vulkan validation error in {log_path}"
        if error:
            assert result.returncode != 0 and error in log, f"Expected {error!r}, see {log_path}"
        else:
            assert result.returncode == 0, f"Renderer exited with {result.returncode}, see {log_path}"

    def read_image(path):
        data = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        assert data is not None and data.shape == (240, 320, 3) and data.dtype == np.float32
        assert np.isfinite(data).all()
        np.testing.assert_allclose(data[120, 160], [1, 2, 4], atol=.008, rtol=0)
        return data

    checkpoints = [1, 5, 13, 73]
    series = ",".join(map(str, checkpoints))
    images = []
    for batch in (3, 7):
        stem = f"reference-α-{batch}"
        base = output / f"{stem}.exr"
        capture(stem, emitter, "--path-spp", str(batch), "--path-sample-series", series,
                "--screenshot", str(base))
        assert not base.exists()
        expected_paths = [output / f"{stem}.samples-{count:08}.exr" for count in checkpoints]
        assert set(output.glob(f"{stem}.samples-*.exr")) == set(expected_paths)
        batch_images = []
        for count, path in zip(checkpoints, expected_paths):
            metadata = json.loads(Path(f"{path}.capture.json").read_text(encoding="utf-8"))
            assert metadata["source"] == "path-trace-mean" and metadata["colorSpace"] == "linear-sRGB"
            assert not (metadata["includesUi"] or metadata["exposureApplied"] or metadata["toneMappingApplied"])
            reference = metadata["referencePath"]
            assert reference["attemptedSamplesPerPixel"] == reference["requestedSamplesPerPixel"] == count
            assert reference["acceptedSamplesPerPixel"] == count and not reference["hasDiagnostics"]
            assert reference["unsupportedSamples"] == reference["rejectedSamples"] == 0
            assert reference["historyResets"] == 1 and reference["sampleLimit"] == checkpoints[-1]
            assert reference["sampleLimitReached"] == (count == checkpoints[-1])
            assert reference["samplesPerFrame"] == batch and reference["seed"] == 23
            assert reference["maxBounces"] == 2
            telemetry = json.loads(path.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
            assert telemetry["referencePath"]["attemptedSamples"] == count
            batch_images.append(read_image(path))
        images.append(batch_images)

    assert not np.array_equal(images[0][0], images[0][-1]), "The edge fixture must vary with sample count"
    for left, right in zip(*images):
        assert np.array_equal(left, right), "Changing batch size changed reference radiance"

    single = output / "single.exr"
    capture("single", emitter, "--display-mode", "lit", "--path-spp", "4", "--path-samples", "73",
            "--warmup-frames", "0", "--capture-frame", "19", "--screenshot", str(single))
    assert np.array_equal(read_image(single), images[0][-1]), "Series changed the accumulated sample sequence"

    def adaptive_case(name, target, starting_batch, limit, *, capture_each_frame):
        base = output / f"{name}.exr"
        sequence = output / f"{name}.json"
        events = [{"frame": 7, "environmentYaw": 45}] if target == 1000 else []
        frame_count = 12
        frames = list(range(1, frame_count + 1))
        sequence.write_text(json.dumps({"schemaVersion": 1, "frames": frame_count,
                                        "sampleFrames": frames if capture_each_frame else [frame_count],
                                        "telemetryFrames": frames,
                                        "events": events}), encoding="utf-8")
        capture(name, emitter, "--display-mode", "lit", "--path-spp", str(starting_batch),
                "--path-samples", str(limit), "--path-target-ms", str(target),
                "--capture-sequence", str(sequence), "--screenshot", str(base))
        paths = [output / f"{name}.frame-{index:04}.telemetry.json" for index in frames[:-1]]
        paths.append(base.with_suffix(".telemetry.json"))
        states = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
        previous_count = 0
        previous_batch = 0
        previous_epoch = 0
        previous_serial = 0
        decisions = []
        schedule = []
        for frame, state in enumerate(states):
            history = state["frameHistory"]
            current = history[-1]
            assert current["frameIndex"] == frame, paths[frame]
            captured = capture_each_frame or frame == frame_count - 1
            assert (current["cpuPhasesMs"]["Screenshot"] > 0) == captured, paths[frame]
            if captured:
                # The screenshot fence makes this exact submission available
                # even before its timing can drive the next batch.
                assert current["gpuFrameIndex"] == frame and current["gpuGraphicsMs"] > 0, paths[frame]
                assert current["gpuResultLatencyFrames"] == 0, paths[frame]
            reference = state["referencePath"]
            count, batch = reference["attemptedSamples"], reference["lastSubmittedSamples"]
            scheduled = reference["scheduledSamplesPerFrame"]
            epoch = reference["historyResets"]
            if epoch != previous_epoch:
                assert epoch == previous_epoch + 1
                previous_count = previous_batch = 0
            assert count == previous_count + batch and count <= limit
            assert 1 <= scheduled <= 64 and batch == min(scheduled, limit - previous_count)
            assert reference["unsupportedSamples"] == reference["rejectedSamples"] == 0
            if previous_count == 0:
                assert scheduled == starting_batch and reference["lastTimedLightingMs"] is None
            elif previous_count < limit:
                # Live Lighting and the controller must see the same retired
                # result from the immediately preceding submitted frame.
                previous = history[-2]
                serial = state["gpuProfiler"]["recordingSerial"]
                assert previous["frameIndex"] == previous["gpuFrameIndex"] == frame - 1, paths[frame]
                assert previous["gpuResultLatencyFrames"] == (0 if capture_each_frame else 1), paths[frame]
                assert serial == previous["gpuRecordingSerial"] and serial > previous_serial, paths[frame]
                assert state["gpuProfiler"]["resultLatencyFrames"] == 1, paths[frame]
                lighting = next(item for item in state["passes"] if item["name"] == "Lighting")
                measured = reference["lastTimedLightingMs"]
                assert measured == lighting["gpuMs"] and measured > 0
                estimate = math.floor(previous_batch * reference["adaptiveTargetMs"] / measured)
                expected = min(64, previous_batch * 2, max((previous_batch + 1) // 2, estimate))
                assert scheduled == expected
                previous_serial = serial
                decisions.append(scheduled)
            schedule.append((epoch, count, batch))
            previous_count, previous_batch = count, batch
            previous_epoch = epoch
        assert decisions and previous_count == limit and previous_batch == 0
        assert previous_epoch == 1 + len(events)
        if target == 1000:
            assert max(decisions) == 64
        assert np.array_equal(read_image(base), images[0][checkpoints.index(limit)])
        return schedule

    captured = adaptive_case("adaptive-grow", 1000, 40, 73, capture_each_frame=True)
    telemetry_only = adaptive_case("adaptive-grow-telemetry", 1000, 40, 73, capture_each_frame=False)
    # The generous budget clamps to 64 SPP, and each epoch needs only 33 more
    # samples after its first batch. Compare the submitted schedule across modes.
    assert captured == telemetry_only
    adaptive_case("adaptive-budget", .1, 4, 13, capture_each_frame=True)

    capture("duplicate", emitter, "--path-sample-series", "1,1", "--screenshot", str(single),
            error="strictly increasing counts")
    capture("frame-conflict", emitter, "--path-sample-series", series, "--capture-frame", "2",
            "--screenshot", str(single), error="cannot be combined")
    capture("adaptive-conflict", emitter, "--path-sample-series", series, "--path-target-ms", "10",
            "--screenshot", str(single), error="cannot be combined")

    unsupported = output / "sheen.gltf"
    material["extensions"]["KHR_materials_sheen"] = {"sheenColorFactor": [1, 1, 1]}
    unsupported_model = emitter_fixture(unsupported, material)
    unsupported_model["extensionsUsed"].append("KHR_materials_sheen")
    unsupported.write_text(json.dumps(unsupported_model), encoding="utf-8")
    capture("unsupported", unsupported, "--path-sample-series", "1", "--screenshot",
            str(output / "unsupported.exr"), error="unsupported and 0 rejected samples")
    assert not list(output.glob("unsupported.samples-*"))

    def checkpoint_paths(base, count):
        image = base.with_name(f"{base.stem}.samples-{count:08}.exr")
        return image, Path(f"{image}.capture.json"), image.with_suffix(".telemetry.json")

    def check_checkpoint(base, count, seed):
        image, sidecar, telemetry = checkpoint_paths(base, count)
        read_image(image)
        reference = json.loads(sidecar.read_bytes())["referencePath"]
        assert reference["acceptedSamplesPerPixel"] == count and reference["seed"] == seed
        assert json.loads(telemetry.read_bytes())["referencePath"]["attemptedSamples"] == count

    for lane, blocked_index in (("blocked-sidecar", 1), ("blocked-telemetry", 2)):
        directory = output / lane
        directory.mkdir()
        base = directory / "reference.exr"
        first = checkpoint_paths(base, 1)
        second = checkpoint_paths(base, 5)
        existing = {}
        if blocked_index == 2:
            prior = checkpoint_paths(output / "reference-α-3.exr", 5)
            for source, destination in zip(prior[:2], second[:2]):
                existing[destination] = source.read_bytes()
                destination.write_bytes(existing[destination])
        blocker = second[blocked_index]
        blocker.mkdir()
        marker = blocker / "keep.txt"
        marker.write_text("existing directory", encoding="utf-8")
        capture(lane, emitter, "--path-spp", "3", "--path-seed", "29", "--path-sample-series", "1,5",
                "--screenshot", str(base), error="Cannot publish reference checkpoint")
        check_checkpoint(base, 1, 29)
        assert marker.read_text(encoding="utf-8") == "existing directory"
        assert set(directory.iterdir()) == {*first, *existing, blocker}
        for path, previous in existing.items():
            assert path.read_bytes() == previous, f"Failed checkpoint overwrote {path}"
        if existing:
            marker.unlink()
            blocker.rmdir()
            capture("publication-retry", emitter, "--path-spp", "3", "--path-seed", "31",
                    "--path-sample-series", "1,5", "--screenshot", str(base))
            check_checkpoint(base, 1, 31)
            check_checkpoint(base, 5, 31)
            assert set(directory.iterdir()) == {*first, *second}

    print("PASS exact reference checkpoints, GPU-timed adaptive batches, and deterministic export", flush=True)
    print("PASS failed checkpoint publication preserves complete earlier checkpoints and existing outputs", flush=True)


if __name__ == "__main__":
    main()
