"""Verify opt-in, device-wide NVML telemetry and unchanged headless captures."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

from PIL import Image

from display_capture_test_support import validation_log_error


FRAMES = 480
SAMPLE_FRAMES = (1, 120, 240, 360, FRAMES)
SENSORS = ("utilizationPercent", "driverUsedMemoryBytes", "driverTotalMemoryBytes", "graphicsClockMHz",
           "memoryClockMHz", "temperatureCelsius", "powerMilliwatts")
UNAVAILABLE = {"unsupported-device", "pci-unavailable", "library-unavailable", "initialization-failed",
               "device-unavailable", "sensors-unavailable", "worker-unavailable"}
TRANSIENT = {"initializing", "publishing"}
POLLING = {"available", "sensors-unavailable"}
STALE_AFTER_MS = 3000
CPU_PHASES = ("Frame", "Frame wait")
IDENTITY = ("gpu", "vendorId", "deviceId", "driverVersion", "vulkanApiVersion")


def timing(telemetry):
    history = [item["cpuPhasesMs"] for item in telemetry["frameHistory"]
               if not item["cpuPhasesMs"].get("Screenshot")]
    assert len(history) >= 30, len(history)
    samples = {field: [item[field] for item in history] for field in CPU_PHASES}
    assert all(math.isfinite(value) and value >= 0 for values in samples.values() for value in values), samples
    return dict(frames=len(history), median={field: statistics.median(values) for field, values in samples.items()},
                p95={field: statistics.quantiles(values, n=20, method="inclusive")[-1]
                     for field, values in samples.items()})


def completed_profile(state):
    profiler = state["gpuProfiler"]
    available = profiler["available"]
    sources = ("timestamp queries", "VK_KHR_performance_query")
    assert type(available) is bool and profiler["source"] in (*sources, "none"), profiler
    assert profiler["recordingSerial"] >= 0 and profiler["resultLatencyFrames"] >= 0, profiler
    gpu_time = state["gpuKnownMs"]
    assert math.isfinite(gpu_time) and gpu_time >= 0, gpu_time
    passes = {item["name"]: item["gpuMs"] for item in state["passes"] if item["active"] and item["gpuTimed"]}
    assert all(math.isfinite(value) and value >= 0 for value in passes.values()), passes
    if available:
        assert profiler["source"] in sources and profiler["recordingSerial"] > 0, profiler
        assert gpu_time > 0 and max(passes.values(), default=0) > 0, (gpu_time, passes)
    return dict(**profiler, gpuKnownMs=gpu_time, measuredPasses=len(passes))


def validate_sample(sample):
    status = sample["status"]
    assert sample["backend"] == "nvml" and sample["scope"] == "device", sample
    assert status in UNAVAILABLE | TRANSIENT | POLLING, sample
    pci = sample["pci"]
    if pci is not None:
        limits = {"domain": 0xFFFFFFFF, "bus": 0xFF, "device": 0x1F, "function": 7}
        assert pci.keys() == limits.keys() and all(type(pci[key]) is int and 0 <= pci[key] <= upper
                                                  for key, upper in limits.items()), sample
    timestamp, age, duration, stale = (sample[key] for key in ("sampleUnixMilliseconds", "sampleAgeMilliseconds",
                                                              "pollMicroseconds", "sampleStale"))
    if timestamp is None:
        assert age is duration is stale is None and status not in POLLING, sample
    else:
        assert status not in TRANSIENT and type(timestamp) is int and timestamp > 0, sample
        assert type(age) is int and age >= 0 and type(duration) is int and duration >= 0, sample
        assert stale is (status in POLLING and age >= STALE_AFTER_MS), sample
    values = [sample[sensor] for sensor in SENSORS]
    if status != "available":
        assert all(value is None for value in values), sample
        return
    assert pci is not None and any(value is not None for value in values), sample
    assert all(value is None or (type(value) is int and value >= 0) for value in values), sample
    utilization, used, total, *_ = values
    assert utilization is None or utilization <= 100, sample
    assert (used is None and total is None) or (used is not None and total is not None and 0 <= used <= total and total > 0), sample


def summarize_samples(samples, require_available):
    for sample in samples:
        validate_sample(sample)
    available = {sample["sampleUnixMilliseconds"] for sample in samples if sample["status"] == "available"}
    if require_available or available:
        assert len(available) >= 2, f"Expected two observed NVML updates: {samples}"
    else:
        assert any(sample["status"] in UNAVAILABLE for sample in samples), samples
    polls = {sample["sampleUnixMilliseconds"]: sample["pollMicroseconds"] for sample in samples
             if sample["sampleUnixMilliseconds"] is not None}
    # UTC can move while the steady-clock worker remains rate limited. Retain
    # observed completion gaps and poll durations as measurements.
    timestamps = list(polls)
    durations = list(polls.values())
    return dict(observedPolls=len(polls), completionGapsUnixMilliseconds=[b - a for a, b in zip(timestamps, timestamps[1:])],
                pollMicroseconds=durations, medianPollMicroseconds=statistics.median(durations) if durations else None,
                maxPollMicroseconds=max(durations, default=None), supportedSensors=[sensor for sensor in SENSORS
                if any(sample[sensor] is not None for sample in samples)])


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--require-available", action="store_true", help="Require real NVML sensors on the selected GPU")
    parser.add_argument("--pairs", type=int, default=1, help="Number of 480-frame disabled/enabled pairs, alternating launch order")
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("--pairs must be at least 1")
    renderer, output, model = (p.resolve() for p in (args.renderer, args.output, args.model))
    renderer_hash, model_hash = file_hash(renderer), file_hash(model)
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
        started = time.perf_counter()
        run = subprocess.run(command, cwd=renderer.parent, env=env, capture_output=True,
                             text=True, errors="replace", timeout=240)
        elapsed = time.perf_counter() - started
        log = run.stdout + run.stderr
        log_path = output / f"{label}.log"
        log_path.write_text(log, encoding="utf-8")
        assert run.returncode == 0 and not validation_log_error(log), log_path

        def name(frame):
            return label if frame == FRAMES else f"{label}.frame-{frame:04d}"

        records = [json.loads((output / (name(frame) + ".telemetry.json")).read_text(encoding="utf-8"))
                   for frame in SAMPLE_FRAMES]
        assert all(state["validationEnabled"] is True and ("gpuHardware" in state) == enabled for state in records), log_path
        return [output / (name(frame) + ".png") for frame in SAMPLE_FRAMES], records, dict(
            command=command, log=str(log_path), elapsedSeconds=elapsed,
            cpuMilliseconds=timing(records[-1]), completedGpuProfile=completed_profile(records[-1]))

    pairs = []
    hardware, build = None, None
    for index in range(args.pairs):
        order = ("disabled", "enabled") if index % 2 == 0 else ("enabled", "disabled")
        prefix = "" if args.pairs == 1 else f"pair-{index + 1:02d}-"
        runs = {mode: capture(prefix + mode, mode == "enabled") for mode in order}
        reference, observed = (runs[mode][1] for mode in ("disabled", "enabled"))
        for state in (*reference, *observed):
            identity = {field: state[field] for field in IDENTITY}
            if hardware is None:
                hardware, build = identity, state["build"]
            assert identity == hardware and state["build"] == build, "Capture device or renderer revision changed"
        for disabled, enabled in zip(runs["disabled"][0], runs["enabled"][0], strict=True):
            with Image.open(disabled) as expected, Image.open(enabled) as actual:
                assert expected.format == actual.format == "PNG" and expected.size == actual.size == (800, 600), enabled
                assert expected.convert("RGBA").tobytes() == actual.convert("RGBA").tobytes(), enabled

        samples = [state["gpuHardware"] for state in observed]
        worker = summarize_samples(samples, args.require_available)
        metrics = {mode: runs[mode][2] for mode in order}
        off_profile, on_profile = (metrics[mode]["completedGpuProfile"] for mode in ("disabled", "enabled"))
        assert all(off_profile[key] == on_profile[key] for key in ("available", "source")), (off_profile, on_profile)
        off, on = (metrics[mode]["cpuMilliseconds"] for mode in ("disabled", "enabled"))
        delta = {stat: {field: on[stat][field] - off[stat][field] for field in CPU_PHASES} for stat in ("median", "p95")}
        pairs.append(dict(launchOrder=order, runs=metrics, pixelExactFrames=SAMPLE_FRAMES, samples=samples,
                          worker=worker, enabledMinusDisabledCpuMilliseconds=delta))
        print(f"Verified pair {index + 1}/{args.pairs}: {len(SAMPLE_FRAMES)} matching frames", file=sys.stderr, flush=True)

    assert (file_hash(renderer), file_hash(model)) == (renderer_hash, model_hash), "Renderer or fixture changed"
    report = dict(gpu=hardware["gpu"], pixelExact=True, samples=pairs[0]["samples"],
                  medianCpuMilliseconds={mode: pairs[0]["runs"][mode]["cpuMilliseconds"]["median"]
                                         for mode in ("disabled", "enabled")},
                  hardware=hardware, build=build, rendererSha256=renderer_hash, modelSha256=model_hash,
                  processCount=2 * args.pairs, captureCount=2 * args.pairs * len(SAMPLE_FRAMES), pairs=pairs)
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
