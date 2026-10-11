#!/usr/bin/env python3
"""Repeated native DDGI timing sweeps with a static, warmed scene.

Build the Release renderer first. Relative --exe/--output/--scene paths resolve
against the repository. Each technique runs GI off, a warmed field paused using
a sequence event, and low/default/full-volume update budgets in rotated orders.
Default measurements use 120 warmup frames, six settling frames, 90 consecutive
GPU-timestamp samples and three independent launches per mode.

PNG captures/readbacks serialize sampled frames. Results describe this capture
workload, not interactive frame rate or throughput. GPU timings come from prior
completed work; settling isolates the pause transition. Lighting's paused-minus-
off difference estimates the complete GI lighting route, not an isolated lookup
instruction cost. Intensity zero still performs lookup and is not the baseline.
Warmup completes the initial sweep; it does not prove radiometric convergence.
Paused telemetry publishes an empty batch and a separate retained scheduling
cursor; that cursor, update frame, content epoch, history reset count and
allocations verify retention independently of delayed GPU timestamps.
"""

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time


REPO_ROOT = Path(__file__).resolve().parents[3]
PROBE_COUNT = 343
RAYS_PER_PROBE = 128
BUDGETS = {"off": 128, "paused": 128, "low": 16, "default": 128, "high": 343}
MODES = tuple(BUDGETS)
TECHNIQUES = ("deferred-raster", "forward-raster")
ERROR_PATTERN = re.compile(
    r"VUID-|SYNC-HAZARD|\[error\]|\[critical\]|\bValidation Error\b|\bException\b",
    re.IGNORECASE,
)
LIMITATIONS = [
    "PNG readback and file encoding serialize samples and perturb GPU duty cycle; no FPS/throughput claim.",
    "GPU pass timestamps describe previous completed work, whereas DDGI counters describe the current submission.",
    "Paused-minus-off Lighting measures a complete lighting route difference, not an isolated lookup kernel.",
    "Warmup and a complete fresh sweep do not establish radiometric convergence.",
    "Paused firstProbe/nextProbe describe an empty batch; schedulingCursor and other DDGI counters describe current submitted state, independently of delayed GPU timestamps.",
    "GPU-known time sums instrumented passes; it excludes presentation and uninstrumented GPU work.",
    "Single-device results do not establish a hardware-independent performance threshold.",
]


@dataclass(frozen=True)
class Case:
    technique: str
    mode: str
    repeat: int

    @property
    def name(self):
        return f"perf-{self.technique.split('-')[0]}-{self.mode}-r{self.repeat + 1}"


@dataclass(frozen=True)
class Config:
    warmup: int = 120
    settle: int = 6
    samples: int = 90
    repeats: int = 3
    width: int = 1024
    height: int = 768
    validation: bool = False

    @property
    def final_frame(self):
        return self.warmup + self.settle + self.samples

    @property
    def measurement_frames(self):
        return list(range(self.warmup + self.settle + 1, self.final_frame + 1))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def repository_path(value):
    path = Path(value).expanduser()
    return (path if path.is_absolute() else REPO_ROOT / path).resolve()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def validate_config(config):
    require(config.warmup >= math.ceil(PROBE_COUNT / BUDGETS["low"]),
            "warmup must complete a sweep at the low update budget (at least 22 frames)")
    require(config.settle >= 6, "at least six settling frames are required")
    require(config.samples >= 2 and config.repeats >= 1, "samples must be >=2 and repeats >=1")
    require(config.final_frame <= 10000, "capture sequence cannot exceed 10,000 frames")
    require(1 <= config.width <= 16384 and 1 <= config.height <= 16384,
            "dimensions must be in [1,16384]")


def statistic(values):
    ordered = sorted(values)
    require(bool(ordered) and all(math.isfinite(x) for x in ordered),
            "statistics require nonempty finite values")
    index = (len(ordered) - 1) * .95
    low, high = math.floor(index), math.ceil(index)
    p95 = ordered[low] + (ordered[high] - ordered[low]) * (index - low)
    return {"samples": len(ordered), "median": statistics.median(ordered),
            "p95": p95, "min": ordered[0], "max": ordered[-1]}


def make_cases(techniques, config):
    cases = []
    for repeat in range(config.repeats):
        # Rotating both technique and mode order spreads launch/clock drift.
        rotated_techniques = techniques[repeat % len(techniques):] + techniques[:repeat % len(techniques)]
        for technique in rotated_techniques:
            order = MODES[(repeat * 2) % len(MODES):] + MODES[:(repeat * 2) % len(MODES)]
            cases.extend(Case(technique, mode, repeat) for mode in order)
    return cases


def sequence_for(case, config):
    sequence = {"schemaVersion": 1, "frames": config.final_frame,
                "sampleFrames": [config.warmup, *config.measurement_frames]}
    if case.mode == "paused":
        sequence["events"] = [{"frame": config.warmup + 1, "ddgiPaused": True}]
    return sequence


def paths_for(case, config, output):
    for frame in [config.warmup, *config.measurement_frames]:
        stem = case.name if frame == config.final_frame else f"{case.name}.frame-{frame:04d}"
        yield frame, output / f"{stem}.png", output / f"{stem}.telemetry.json"


def command_for(case, config, exe, scene, output):
    budget = BUDGETS[case.mode]
    return [str(exe), "--model", str(scene), "--width", str(config.width),
            "--height", str(config.height), "--hidden", "--no-ui",
            "--validation" if config.validation else "--no-validation",
            "--no-bloom", "--no-taa", "--msaa", "1", "--fixed-dt", str(1 / 60),
            "--render-technique", case.technique,
            "--display-mode", "lit", "--camera-position", "0", "1", "3.5",
            "--camera-target", "0", "1", "0", "--camera-fov", "50", "--exposure", ".04",
            "--environment-intensity", "0", "--directional-intensity", "0",
            "--no-ddgi" if case.mode == "off" else "--ddgi",
            "--ddgi-origin", "-1.05", "-.1", "-1.05", "--ddgi-spacing", ".35", ".35", ".35",
            "--ddgi-counts", "7", "7", "7", "--ddgi-rays", str(RAYS_PER_PROBE),
            "--ddgi-probes-per-frame", str(budget), "--ddgi-ray-budget", str(budget * RAYS_PER_PROBE),
            "--ddgi-intensity", "1", "--ddgi-debug", "off", "--ray-shadows", "raster",
            "--area-lighting", "sampled", "--area-light-samples", "25",
            "--screenshot", str(output / f"{case.name}.png"),
            "--capture-sequence", str(output / f"{case.name}.sequence.json")]


def compact_frame(frame, telemetry):
    passes = {}
    for entry in telemetry["passes"]:
        require(entry["name"] not in passes, f"duplicate pass {entry['name']}")
        if entry["gpuTimed"]:
            require(isinstance(entry["gpuMs"], (int, float)) and
                    math.isfinite(entry["gpuMs"]) and entry["gpuMs"] >= 0,
                    f"invalid GPU timestamp for {entry['name']}")
            passes[entry["name"]] = entry["gpuMs"]
    require("Lighting" in passes and passes["Lighting"] > 0, "Lighting GPU timestamps unavailable")
    total = telemetry["gpuKnownMs"]
    require(isinstance(total, (int, float)) and math.isfinite(total) and total > 0,
            "GPU-known timestamps unavailable")
    require(abs(sum(passes.values()) - total) <= max(.001, total * .001),
            "GPU-known time does not equal the instrumented pass sum")
    return {"frame": frame, "passesGpuMs": passes, "gpuKnownMs": total,
            "ddgi": telemetry["ddgi"], "rayShadows": telemetry["rayShadows"],
            "resolution": telemetry["resolution"], "technique": telemetry["technique"],
            "cameraPosition": telemetry["camera"]["position"],
            "taaEnabled": telemetry["taa"]["enabled"],
            "gpuProfiler": telemetry.get("gpuProfiler"),
            "gpu": telemetry["gpu"], "driverVersion": telemetry["driverVersion"],
            "vulkanApiVersion": telemetry["vulkanApiVersion"],
            "deviceIdleWaits": telemetry["deviceIdleWaits"],
            "swapchainRecreates": telemetry["swapchainRecreates"],
            "selectedMemoryBytes": {
                "probes": telemetry["ddgi"]["totalAllocatedBytes"],
                "sharedRayScene": telemetry["rayShadows"]["totalAllocatedBytes"],
                "localShadowAtlas": telemetry["lighting"]["localShadowAtlasAllocatedBytes"],
                "ltcTables": telemetry["lighting"]["ltcAllocatedImageBytes"],
                "taa": telemetry["taa"]["allocatedImageBytes"],
            }}


def check_frames(case, config, frames):
    require(len(frames) == config.samples + 1, "missing warmup/sample telemetry")
    checkpoint, measurements = frames[0], frames[1:]
    require(checkpoint["frame"] == config.warmup, "missing warmup checkpoint")
    require([f["frame"] for f in measurements] == config.measurement_frames,
            "incorrect measurement frame range")
    for sample in frames:
        prefix = f"frame {sample['frame']}: "
        gi = sample["ddgi"]
        require(sample["resolution"] == [config.width, config.height] and
                sample["technique"] == case.technique, prefix + "unexpected render workload")
        require(sample["cameraPosition"] == [0, 1, 3.5] and not sample["taaEnabled"],
                prefix + "camera/TAA workload changed")
        require(sample["deviceIdleWaits"] == checkpoint["deviceIdleWaits"] and
                sample["swapchainRecreates"] == checkpoint["swapchainRecreates"],
                prefix + "resource/swapchain recreation contaminated measurements")
        require(sample["selectedMemoryBytes"] == checkpoint["selectedMemoryBytes"],
                prefix + "allocation changed after warmup")
        require(sample["gpu"] == checkpoint["gpu"] and
                sample["driverVersion"] == checkpoint["driverVersion"],
                prefix + "GPU/device changed")
        profiler = sample["gpuProfiler"]
        if profiler is not None:
            require(profiler["available"] and profiler["resultLatencyFrames"] <= config.settle,
                    prefix + "GPU profiler unavailable or latency exceeds settling interval")
        require(gi["debugView"] == 0, prefix + "debug draw contaminated measurements")
        require(gi["settings"]["intensity"] == 1 and
                gi["settings"]["maxProbesPerFrame"] == BUDGETS[case.mode] and
                gi["settings"]["maxRaysPerFrame"] == BUDGETS[case.mode] * RAYS_PER_PROBE,
                prefix + "unexpected GI settings")
        cursor = gi.get("schedulingCursor")
        require(type(cursor) is int and 0 <= cursor < PROBE_COUNT,
                prefix + "missing/invalid retained scheduling cursor")
        if case.mode == "off":
            require(not gi["enabled"] and not gi["active"] and
                    gi["tracedRays"] == 0 and gi["updatedProbes"] == 0 and
                    gi["updateFrame"] == 0 and gi["firstProbe"] == 0 and
                    gi["nextProbe"] == 0 and cursor == 0,
                    prefix + "GI-off baseline still performs probe updates")
            continue
        require(gi["enabled"] and gi["supported"] and gi["active"] and
                gi["contentEpoch"] == 1 and gi["refreshRemainingProbes"] == 0,
                prefix + "GI unsupported/inactive or initial sweep incomplete")
        require(sample["rayShadows"]["generation"] == 1,
                prefix + "static scene unexpectedly rebuilt its acceleration structures")
        paused = case.mode == "paused" and sample is not checkpoint
        require(gi["paused"] == paused, prefix + "pause event did not apply")
        if paused:
            require(gi["updatedProbes"] == 0 and gi["tracedRays"] == 0 and
                    gi["updateFrame"] == checkpoint["ddgi"]["updateFrame"] and
                    gi["contentEpoch"] == checkpoint["ddgi"]["contentEpoch"] and
                    gi["historyResets"] == checkpoint["ddgi"]["historyResets"] and
                    gi["firstProbe"] == 0 and gi["nextProbe"] == 0 and
                    cursor == checkpoint["ddgi"]["schedulingCursor"],
                    prefix + "paused field updated or discarded warmed history")
        else:
            budget = BUDGETS[case.mode]
            require(gi["updatedProbes"] == budget and gi["tracedRays"] == budget * RAYS_PER_PROBE and
                    gi["updateFrame"] == sample["frame"], prefix + "active update budget changed")
            # These counters publish the current successful submission, while
            # passesGpuMs can describe an earlier fence-retired frame.
            require(gi["firstProbe"] == ((sample["frame"] - 1) * budget) % PROBE_COUNT and
                    gi["nextProbe"] == (sample["frame"] * budget) % PROBE_COUNT and
                    cursor == gi["nextProbe"],
                    prefix + "active scheduling cursor does not match submitted round-robin batch")
            require("DdgiTrace" in sample["passesGpuMs"] and "DdgiUpdate" in sample["passesGpuMs"],
                    prefix + "probe pass GPU timestamps unavailable")


def summarize(case, config, frames):
    measured = frames[1:]
    pass_names = sorted(set.intersection(*(set(f["passesGpuMs"]) for f in measured)))
    summary = {"passesGpuMs": {name: statistic([f["passesGpuMs"][name] for f in measured])
                               for name in pass_names},
               "gpuKnownMs": statistic([f["gpuKnownMs"] for f in measured]),
               "selectedMemoryBytes": measured[0]["selectedMemoryBytes"],
               "gpuProfiler": measured[0]["gpuProfiler"],
               "initialSweepFinishedByFrame": config.warmup,
               "warmedUpdateFrame": frames[0]["ddgi"]["updateFrame"],
               "warmedSchedulingCursor": frames[0]["ddgi"]["schedulingCursor"],
               "finalSchedulingCursor": frames[-1]["ddgi"]["schedulingCursor"]}
    if case.mode not in ("off", "paused"):
        sweep_frames = math.ceil(PROBE_COUNT / BUDGETS[case.mode])
        summary["completeSweepSubmittedFrames"] = sweep_frames
        summary["completeSweepSimulationMs"] = sweep_frames * 1000 / 60
        summary["sweepGpuKnownMsEstimate"] = sweep_frames * summary["gpuKnownMs"]["median"]
    return summary


def capture(case, config, exe, scene, output, environment, timeout):
    write_json(output / f"{case.name}.sequence.json", sequence_for(case, config))
    command = command_for(case, config, exe, scene, output)
    paths = list(paths_for(case, config, output))
    for _, png, telemetry in paths:
        png.unlink(missing_ok=True)
        telemetry.unlink(missing_ok=True)
    record = {"name": case.name, "mode": case.mode, "repeat": case.repeat + 1,
              "technique": case.technique, "command": command,
              "commandLine": subprocess.list2cmdline(command), "cwd": str(exe.parent),
              "log": str(output / f"{case.name}.log"), "frames": [], "failures": []}
    started = time.monotonic()
    try:
        run = subprocess.run(command, cwd=exe.parent, env=environment, capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=timeout)
        log = run.stdout + run.stderr
        record["returnCode"] = run.returncode
    except subprocess.TimeoutExpired as error:
        def decoded(value):
            return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""
        log = decoded(error.stdout) + decoded(error.stderr)
        record["returnCode"] = None
        record["failures"].append(f"renderer timed out after {timeout} seconds")
    Path(record["log"]).write_text(log, encoding="utf-8")
    record["durationSeconds"] = round(time.monotonic() - started, 3)
    record["validationErrors"] = [line for line in log.splitlines() if ERROR_PATTERN.search(line)]
    if record["returnCode"] != 0 or record["validationErrors"]:
        record["failures"].append("renderer failed or logged validation/runtime errors")
    for frame, png, telemetry in paths:
        try:
            with png.open("rb") as stream:
                require(stream.read(8) == b"\x89PNG\r\n\x1a\n", f"missing/invalid PNG {png}")
            record["frames"].append(compact_frame(frame, json.loads(telemetry.read_text(encoding="utf-8"))))
        except (OSError, ValueError, KeyError, TypeError, AssertionError) as error:
            record["failures"].append(f"frame {frame}: {error}")
    if len(record["frames"]) == len(paths):
        try:
            check_frames(case, config, record["frames"])
            record["summary"] = summarize(case, config, record["frames"])
        except (KeyError, TypeError, AssertionError) as error:
            record["failures"].append(str(error))
    record["passed"] = not record["failures"]
    print(json.dumps({"case": case.name, "passed": record["passed"],
                      "failures": record["failures"], "summary": record.get("summary")}), flush=True)
    return record


def matched_comparisons(records):
    comparisons = []
    for technique in TECHNIQUES:
        repeats = sorted({r["repeat"] for r in records if r["technique"] == technique})
        for repeat in repeats:
            pair = {r["mode"]: r for r in records if r["passed"] and
                    r["technique"] == technique and r["repeat"] == repeat}
            if "off" not in pair or "paused" not in pair:
                continue
            off, paused = pair["off"], pair["paused"]
            for key in ("gpu", "driverVersion", "vulkanApiVersion", "resolution", "cameraPosition"):
                require(off["frames"][1][key] == paused["frames"][1][key],
                        f"cannot match off/paused workload: {key}")
            off_ms = off["summary"]["passesGpuMs"]["Lighting"]["median"]
            paused_ms = paused["summary"]["passesGpuMs"]["Lighting"]["median"]
            comparisons.append({"technique": technique, "repeat": repeat,
                                "offLightingMedianMs": off_ms, "pausedLightingMedianMs": paused_ms,
                                "lightingRouteDeltaMs": paused_ms - off_ms})
    return comparisons


def mode_summaries(records):
    groups = []
    for technique in TECHNIQUES:
        for mode in MODES:
            selected = [r for r in records if r["passed"] and
                        r["technique"] == technique and r["mode"] == mode]
            if not selected:
                continue
            samples = [f for r in selected for f in r["frames"][1:]]
            first = samples[0]
            for sample in samples:
                for key in ("gpu", "driverVersion", "vulkanApiVersion", "resolution",
                            "cameraPosition", "selectedMemoryBytes"):
                    require(sample[key] == first[key], f"cannot pool {technique}/{mode}: {key} changed")
            common_passes = set.intersection(*(set(f["passesGpuMs"]) for f in samples))
            groups.append({"technique": technique, "mode": mode,
                           "completedRepeats": len(selected),
                           "pooledGpuKnownMs": statistic([f["gpuKnownMs"] for f in samples]),
                           "repeatMedianGpuKnownMs": statistic([r["summary"]["gpuKnownMs"]["median"]
                                                                for r in selected]),
                           "pooledPassesGpuMs": {name: statistic([f["passesGpuMs"][name] for f in samples])
                                                 for name in sorted(common_passes)},
                           "repeatMedianPassesGpuMs": {
                               name: statistic([r["summary"]["passesGpuMs"][name]["median"] for r in selected])
                               for name in sorted(common_passes)},
                           "selectedMemoryBytes": first["selectedMemoryBytes"]})
    return groups


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-performance")
    parser.add_argument("--scene", default="models/validation/cornell_box_local_light.gltf")
    parser.add_argument("--technique", choices=(*TECHNIQUES, "both"), default="both")
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--settle", type=int, default=6)
    parser.add_argument("--samples", type=int, default=90)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument("--validation", action="store_true", help="enable sync validation; default timings omit validation")
    parser.add_argument("--timeout", type=float, default=300, help="maximum seconds per renderer launch")
    args = parser.parse_args()
    config = Config(args.warmup, args.settle, args.samples, args.repeats,
                    args.width, args.height, args.validation)
    try:
        validate_config(config)
        require(math.isfinite(args.timeout) and args.timeout > 0, "timeout must be positive and finite")
    except AssertionError as error:
        parser.error(str(error))
    exe, scene, output = map(repository_path, (args.exe, args.scene, args.output))
    if not exe.is_file() or not scene.is_file():
        parser.error("built renderer executable and scene file must exist")
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    overrides = {}
    if config.validation:
        (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
        overrides["VK_LAYER_SETTINGS_PATH"] = str(output)
        environment.update(overrides)
    techniques = TECHNIQUES if args.technique == "both" else (args.technique,)
    results = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
               "exe": str(exe), "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
               "scene": str(scene), "sceneSha256": hashlib.sha256(scene.read_bytes()).hexdigest(),
               "config": config.__dict__, "fixedTimestepSeconds": 1 / 60,
               "percentileMethod": "linear interpolation at (sampleCount-1)*0.95",
               "measurementClass": "repeated native capture GPU pass timing",
               "sustainedSampling": config.repeats >= 3 and config.samples >= 90,
               "limitations": LIMITATIONS, "environmentOverrides": overrides, "cases": []}
    path = output / "performance-results.json"
    for case in make_cases(techniques, config):
        results["cases"].append(capture(case, config, exe, scene, output, environment, args.timeout))
        results["passed"] = all(r["passed"] for r in results["cases"])
        write_json(path, results)
    try:
        results["modeSummaries"] = mode_summaries(results["cases"])
        results["lightingRouteComparisons"] = matched_comparisons(results["cases"])
        results["lightingRouteDeltaMs"] = {
            technique: statistic([p["lightingRouteDeltaMs"] for p in results["lightingRouteComparisons"]
                                  if p["technique"] == technique])
            for technique in techniques
            if any(p["technique"] == technique for p in results["lightingRouteComparisons"])}
    except (KeyError, TypeError, AssertionError) as error:
        results["comparisonFailure"] = str(error)
        results["passed"] = False
    results["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    write_json(path, results)
    print(f"{'PASS' if results['passed'] else 'FAIL'}: {len(results['cases'])} cases; {path}")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
