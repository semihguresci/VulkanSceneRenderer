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
        (output / f"{name}.log").write_text(log, encoding="utf-8")
        assert not validation_log_error(log), log
        if error:
            assert result.returncode != 0 and error in log, log
        else:
            assert result.returncode == 0, log

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

    for name, target, starting_batch, limit in (("adaptive-grow", 1000, 40, 73),
                                                ("adaptive-budget", .1, 4, 13)):
        base = output / f"{name}.exr"
        sequence = output / f"{name}.json"
        events = [{"frame": 7, "environmentYaw": 45}] if target == 1000 else []
        sequence.write_text(json.dumps({"schemaVersion": 1, "frames": 12,
                                        "sampleFrames": list(range(1, 13)),
                                        "events": events}), encoding="utf-8")
        capture(name, emitter, "--display-mode", "lit", "--path-spp", str(starting_batch),
                "--path-samples", str(limit), "--path-target-ms", str(target),
                "--capture-sequence", str(sequence), "--screenshot", str(base))
        paths = [output / f"{name}.frame-{index:04}.telemetry.json" for index in range(1, 12)]
        paths.append(base.with_suffix(".telemetry.json"))
        states = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
        previous_count = 0
        previous_batch = 0
        previous_epoch = 0
        decisions = []
        for state in states:
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
                lighting = next(item for item in state["passes"] if item["name"] == "Lighting")
                measured = reference["lastTimedLightingMs"]
                assert measured == lighting["gpuMs"] and measured > 0
                estimate = math.floor(previous_batch * reference["adaptiveTargetMs"] / measured)
                expected = min(64, previous_batch * 2, max((previous_batch + 1) // 2, estimate))
                assert scheduled == expected
                decisions.append(scheduled)
            previous_count, previous_batch = count, batch
            previous_epoch = epoch
        assert decisions and previous_count == limit and previous_batch == 0
        assert previous_epoch == 1 + len(events)
        if target == 1000:
            assert max(decisions) == 64
        assert np.array_equal(read_image(base), images[0][checkpoints.index(limit)])

    capture("duplicate", emitter, "--path-sample-series", "1,1", "--screenshot", str(single),
            error="strictly increasing counts")
    capture("frame-conflict", emitter, "--path-sample-series", series, "--capture-frame", "2",
            "--screenshot", str(single), error="cannot be combined")
    capture("adaptive-conflict", emitter, "--path-sample-series", series, "--path-target-ms", "10",
            "--screenshot", str(single), error="cannot be combined")

    unsupported = output / "clearcoat.gltf"
    material["extensions"]["KHR_materials_clearcoat"] = {"clearcoatFactor": 1}
    unsupported_model = emitter_fixture(unsupported, material)
    unsupported_model["extensionsUsed"].append("KHR_materials_clearcoat")
    unsupported.write_text(json.dumps(unsupported_model), encoding="utf-8")
    capture("unsupported", unsupported, "--path-sample-series", "1", "--screenshot",
            str(output / "unsupported.exr"), error="unsupported and 0 rejected samples")
    assert not list(output.glob("unsupported.samples-*"))
    print("PASS exact reference checkpoints, GPU-timed adaptive batches, and deterministic export", flush=True)


if __name__ == "__main__":
    main()
