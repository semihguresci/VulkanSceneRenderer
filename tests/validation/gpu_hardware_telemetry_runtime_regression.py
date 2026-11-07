"""Verify opt-in, device-wide NVML telemetry and unchanged headless captures."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess

from PIL import Image, ImageChops


FRAMES = 480
SAMPLE_FRAMES = (1, 120, 240, 360, FRAMES)
SENSORS = ("utilizationPercent", "driverUsedMemoryBytes", "driverTotalMemoryBytes", "graphicsClockMHz",
           "memoryClockMHz", "temperatureCelsius", "powerMilliwatts")
UNAVAILABLE = {"unsupported-device", "pci-unavailable", "library-unavailable", "initialization-failed",
               "device-unavailable", "sensors-unavailable", "worker-unavailable"}


def timing(telemetry):
    history = [item["cpuPhasesMs"] for item in telemetry["frameHistory"]
               if not item["cpuPhasesMs"].get("Screenshot")]
    assert len(history) >= 30, len(history)
    return {field: statistics.median(item[field] for item in history) for field in ("Frame", "Frame wait")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--require-available", action="store_true", help="Require real NVML sensors on the selected GPU")
    args = parser.parse_args()
    renderer, output, model = (p.resolve() for p in (args.renderer, args.output, args.model))
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    sequence = output / "capture-sequence.json"
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": FRAMES, "sampleFrames": SAMPLE_FRAMES}))
    env = dict(os.environ, CONTAINER_MONITOR_GPU="1", VK_LAYER_SETTINGS_PATH=str(output))

    def capture(label, enabled):
        image = output / f"{label}.png"
        command = [str(renderer), "--model", str(model), "--hidden", "--no-ui", "--validation", "--import-scale", "1",
                   "--width", "800", "--height", "600", "--no-taa", "--no-bloom", "--no-ddgi", "--msaa", "1",
                   "--ray-shadows", "raster", "--sky", "plain", "--exposure", "1", "--screenshot-viewport",
                   "--capture-sequence", str(sequence), "--screenshot", str(image)]
        if enabled:
            command.append("--gpu-hardware-telemetry")
        run = subprocess.run(command, cwd=renderer.parent, env=env, capture_output=True,
                             text=True, errors="replace", timeout=240)
        log = run.stdout + run.stderr
        (output / f"{label}.log").write_text(log, encoding="utf-8")
        assert run.returncode == 0, log
        assert not any(marker in log for marker in ("Validation Error", "SYNC-HAZARD", "VUID-")), log

        def name(frame):
            return label if frame == FRAMES else f"{label}.frame-{frame:04d}"

        records = [json.loads((output / (name(frame) + ".telemetry.json")).read_text(encoding="utf-8"))
                   for frame in SAMPLE_FRAMES]
        return image, records

    reference_image, reference = capture("disabled", False)
    observed_image, observed = capture("enabled", True)
    assert all("gpuHardware" not in state for state in reference)
    with Image.open(reference_image) as expected, Image.open(observed_image) as actual:
        assert ImageChops.difference(expected.convert("RGB"), actual.convert("RGB")).getbbox() is None

    samples = [state["gpuHardware"] for state in observed]
    for state, sample in zip(observed, samples):
        assert sample["backend"] == "nvml" and sample["scope"] == "device", sample
        assert "gpuProfiler" in state and "passes" in state and "gpuKnownMs" in state
        if sample["sampleUnixMilliseconds"] is not None:
            assert sample["sampleUnixMilliseconds"] > 0 and sample["sampleAgeMilliseconds"] >= 0, sample
            assert sample["pollMicroseconds"] >= 0, sample
        if sample["status"] in UNAVAILABLE:
            assert all(sample[sensor] is None for sensor in SENSORS), sample
    real = [sample for sample in samples if sample["status"] == "available"]
    if args.require_available or real:
        assert real, samples
        timestamps = sorted({sample["sampleUnixMilliseconds"] for sample in real})
        assert len(timestamps) >= 2 and all(b - a >= 999 for a, b in zip(timestamps, timestamps[1:])), timestamps
        for sample in real:
            assert set(sample["pci"]) == {"domain", "bus", "device", "function"}, sample
            assert any(sample[sensor] is not None for sensor in SENSORS), sample
            assert all(sample[sensor] is None or sample[sensor] >= 0 for sensor in SENSORS), sample
            utilization = sample["utilizationPercent"]
            assert utilization is None or utilization <= 100, sample
    else:
        assert any(sample["status"] in UNAVAILABLE for sample in samples), samples

    report = {"gpu": observed[-1]["gpu"], "pixelExact": True, "samples": samples,
              "medianCpuMilliseconds": {"disabled": timing(reference[-1]), "enabled": timing(observed[-1])}}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
