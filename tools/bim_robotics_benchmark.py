"""Measure the native BIM simulation, sensor capture and synchronized dataset pipeline."""

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
from statistics import median
import subprocess
import sys
import time

from benchmark_bim_import import build_identity, file_identity, positive_int
from bim_robotics_benchmark_fixtures import (
    DEFAULT_SYNTHETIC_SIDE, MAX_SYNTHETIC_SIDE, TEKLA_RELATIVE_PATH, TEKLA_SHA256,
    camera_spec, simulation_scenario, source_namespace, write_synthetic_ifc,
)
from bim_robotics_benchmark_memory import ProcessMemory, host_identity


CPU_PHASES = (
    "sourceReadAndFingerprint", "bimImport", "semanticScene", "collisionCook",
    "walkableSurfaces", "collisionInstall", "simulationSetup", "physicsTicks", "lidarSampling",
)
CAMERA_CPU_METRICS = ("cpuRgbInvalidateMs", "cpuDepthLabelsInvalidateMs", "cpuEncodeMs", "cpuWriteMs")
CAMERA_GPU_METRICS = ("gpuRgbReadbackMs", "gpuDepthLabelsReadbackMs")
CAMERA_BYTE_METRICS = ("rgbReadbackBytes", "depthLabelsReadbackBytes", "outputBytes")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_json(path):
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")


def stats(values):
    ordered = sorted(values)
    if not ordered:
        return None
    return {
        "samples": len(ordered),
        "min": ordered[0],
        "median": median(ordered),
        "p95": ordered[math.ceil(0.95 * len(ordered)) - 1],
        "max": ordered[-1],
    }


def measured(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def run_child(executable, arguments, directory, name, timeout):
    command = [str(executable), *map(str, arguments)]
    stdout = directory / f"{name}.stdout.log"
    stderr = directory / f"{name}.stderr.log"
    deadline = time.monotonic() + timeout
    with stdout.open("xb") as out, stderr.open("xb") as err:
        start = time.perf_counter_ns()
        with subprocess.Popen(command, cwd=executable.parent, stdout=out, stderr=err,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as child:
            with ProcessMemory(child.pid) as memory:
                try:
                    while True:
                        memory.sample()
                        elapsed_before_exit = time.perf_counter_ns()
                        if child.poll() is not None:
                            break
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise RuntimeError(f"{name} exceeded {timeout} seconds. See {stderr}")
                        try:
                            child.wait(timeout=min(remaining, 0.02))
                        except subprocess.TimeoutExpired:
                            continue
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait()
                elapsed = (elapsed_before_exit - start) / 1e9
                peak_memory = memory.metrics()
    require(child.returncode == 0, f"{name} exited with code {child.returncode}. See {stderr} and {stdout}")
    require(elapsed > 0, f"{name} returned an invalid wall-clock duration")
    return {"command": command, "wallSeconds": elapsed, "processMemory": peak_memory,
            "stdout": stdout.name, "stderr": stderr.name}


def find_executable(build, configuration, name):
    if os.name == "nt":
        name += ".exe"
    for candidate in (build / configuration / name, build / name):
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"{name} was not built in {build} or its {configuration} subdirectory")


def validate_cpu_timing(timing, ticks, scans, rays):
    require(timing.get("schema") == "container.bim-robotics-cpu-benchmark" and timing.get("version") == 1,
            "Unexpected CPU benchmark sidecar version")
    counts = timing["counts"]
    require(timing["assetCacheStatus"] == "disabled", "Benchmark expected uncached BIM preprocessing")
    for key, expected in (("physicsTicks", ticks), ("lidarScans", scans), ("lidarRays", rays)):
        require(counts[key] == expected, f"CPU sidecar {key} differs from the verified observations")
    phases = timing["timingsNanoseconds"]
    for key in CPU_PHASES:
        require(measured(phases[key]), f"Missing or invalid cold preprocessing or CPU timing for {key}")
    require(ticks > 0 and scans > 0 and rays > 0, "Benchmark must execute physics ticks and LiDAR observations")


def scheduled_camera_ticks(calibration):
    timing = calibration["timing"]
    first, last = timing["firstTick"], timing["lastTick"]
    period_numerator = timing["ticksPerSecond"] * 1000
    rate = timing["framesPerSecondMilliHz"]
    sample_count = (last - first) * rate // period_numerator + 1
    return [first + (index * period_numerator + rate - 1) // rate for index in range(sample_count)]


def validate_camera_timing(timing, expected_ticks, dimensions):
    require(timing.get("schema") == "container.sensor-camera-benchmark" and timing.get("version") == 1,
            "Unexpected camera benchmark sidecar version")
    require((timing["width"], timing["height"]) == dimensions, "Camera timing dimensions differ from calibration")
    frames = timing["frames"]
    require([row["sampleIndex"] for row in frames] == list(range(len(expected_ticks))) and
            [row["tick"] for row in frames] == expected_ticks, "Camera timing is missing a scheduled frame")
    available = timing["gpuTiming"]["available"]
    require(isinstance(available, bool) and timing["device"]["name"], "Missing Vulkan device or timing status")
    timed_frames = 0
    for row in frames:
        for key in CAMERA_CPU_METRICS:
            require(measured(row[key]), f"Missing host timing for {key}")
        gpu_pair = [row[key] for key in CAMERA_GPU_METRICS]
        timed = all(measured(value) for value in gpu_pair)
        require(timed or all(value is None for value in gpu_pair), "Inconsistent RGB and depth GPU timing pair")
        timed_frames += timed
        for key in CAMERA_BYTE_METRICS:
            require(type(row[key]) is int and row[key] > 0, f"Missing byte count for {key}")
    require(available == (timed_frames > 0) and timing["gpuTiming"]["timedFrames"] == timed_frames,
            "Inconsistent GPU timing availability or frame count")
    memory = timing["memory"]
    require(0 <= memory["deviceLocalBytes"] <= memory["allocationBytes"] and measured(timing["finalFenceWaitMs"]),
            "Invalid renderer memory or final fence-wait measurement")


def run_case(scene, model, directory, executables, args, expected_products=None):
    directory.mkdir()
    ns = source_namespace(scene)
    scenario, camera = directory / "scenario.json", directory / "camera.json"
    scenario_spec = simulation_scenario(scene, args.ticks)
    calibration = camera_spec(args.ticks, args.width, args.height)
    write_json(scenario, scenario_spec)
    write_json(camera, calibration)
    report, cpu_json = directory / "simulation.json", directory / "cpu-benchmark.json"
    captured, gpu_json = directory / "camera", directory / "camera-benchmark.json"
    dataset = directory / "dataset"
    run = lambda key, command, label: run_child(executables[key], command, directory, label, args.timeout)

    simulation = run("simulator", [model, report, ns, scenario, "--benchmark", cpu_json], "simulate")
    capture = run("renderer", [
        "--model", model, "--sensor-namespace", ns, "--sensor-camera", camera, "--sensor-trajectory", report,
        "--sensor-output", captured, "--sensor-benchmark", gpu_json, "--render-technique", "deferred-raster",
        "--msaa", "1", "--no-taa", "--no-ray-query", "--no-ddgi",
    ], "capture")
    recorded = run("dataset", ["record", captured, report, dataset], "record")
    verified = run("dataset", ["verify", dataset], "verify")

    manifest = read_json(directory / verified["stdout"])
    require(manifest == read_json(dataset / "dataset.json"), "Dataset verification did not report the written manifest")
    report_data = read_json(report)
    lidar = report_data["lidar"]["scans"]
    history = report_data["history"]
    require(manifest["tickCount"] == args.ticks + 1 and len(history) == args.ticks + 1,
            "Simulation or dataset lost a committed tick")
    require(all(row["obstruction"] == "none" for row in history) and
            any(row["base"]["position"] != history[0]["base"]["position"] for row in history),
            "Benchmark robot did not travel the unobstructed fixture route")
    require(manifest["sourceNamespace"] == ns and manifest["lidarScanCount"] == len(lidar),
            "Dataset lost its BIM namespace or LiDAR scans")
    camera_timing = read_json(gpu_json)
    cpu_timing = read_json(cpu_json)
    expected_ticks = scheduled_camera_ticks(calibration)
    require(manifest["cameraFrameCount"] == len(expected_ticks), "Dataset dropped scheduled camera observations")
    camera_files = sorted(captured.glob("frame-*.json"))
    require([read_json(path)["tick"] for path in camera_files] == expected_ticks,
            "Completed camera captures differ from the scheduled ticks")
    require(manifest["simulationReportSha256"] == file_identity(report)["sha256"],
            "Dataset provenance does not match its simulation report")
    require(any(hit["entityId"] for scan in lidar for hit in scan["returns"]),
            "LiDAR observed no BIM objects")
    validate_cpu_timing(cpu_timing, args.ticks, len(lidar), sum(len(scan["returns"]) for scan in lidar))
    validate_camera_timing(camera_timing, expected_ticks, (args.width, args.height))
    if expected_products is not None:
        require(cpu_timing["counts"]["bimElements"] == cpu_timing["counts"]["collisionColliders"] == expected_products,
                "Generated IFC lost modeled products during import or collision cooking")

    produced = sum(path.stat().st_size for path in dataset.iterdir() if path.is_file())
    images = sum(path.stat().st_size for path in dataset.glob("frame-*.exr"))
    require(0 < images < produced, "Dataset lacks image or metadata payloads")
    recorded["throughput"] = {
        "outputBytes": produced, "imageBytes": images,
        "outputMiBPerSecond": produced / (1024 * 1024 * recorded["wallSeconds"]),
        "cameraFramesPerSecond": manifest["cameraFrameCount"] / recorded["wallSeconds"],
        "timelineRowsPerSecond": manifest["tickCount"] / recorded["wallSeconds"],
    }
    return {"simulation": simulation, "capture": capture, "record": recorded, "verify": verified,
            "simulationSha256": manifest["simulationReportSha256"], "counts": {
                "timelineRows": manifest["tickCount"], "cameraFrames": manifest["cameraFrameCount"],
                "lidarScans": manifest["lidarScanCount"],
            }, "cpuTiming": cpu_timing, "cameraTiming": camera_timing}


def summarize(runs):
    cpu = [run["cpuTiming"] for run in runs]
    camera = [run["cameraTiming"] for run in runs]
    frames = [row for timing in camera for row in timing["frames"]]
    require(all(timing["device"] == camera[0]["device"] for timing in camera), "Vulkan device changed across repeats")
    first_gpu = camera[0]["gpuTiming"]
    common_gpu = {key: value for key, value in first_gpu.items() if key not in ("timedFrames", "available", "status")}
    require(all(all(timing["gpuTiming"][key] == value for key, value in common_gpu.items()) for timing in camera),
            "GPU timestamp scope or device timing support changed")
    require(len({run["simulationSha256"] for run in runs}) == 1, "Repeated scenarios changed simulation output")
    require(all(timing["counts"] == cpu[0]["counts"] for timing in cpu), "Simulation workload changed")
    samples = sum(timing["gpuTiming"]["timedFrames"] for timing in camera)
    return {
        "simulationCounts": cpu[0]["counts"],
        "gpuTiming": {**common_gpu, "status": sorted({timing["gpuTiming"]["status"] for timing in camera})},
        "cpuStageMs": {key: stats(timing["timingsNanoseconds"][key] / 1e6 for timing in cpu)
                       for key in CPU_PHASES},
        "meanPhysicsTickMs": stats(timing["timingsNanoseconds"]["physicsTicks"] /
                                   (1e6 * timing["counts"]["physicsTicks"]) for timing in cpu),
        "meanLidarScanMs": stats(timing["timingsNanoseconds"]["lidarSampling"] /
                                 (1e6 * timing["counts"]["lidarScans"]) for timing in cpu),
        "cameraFrameMs": {key: stats(row[key] for row in frames if row[key] is not None)
                          for key in (*CAMERA_CPU_METRICS, *CAMERA_GPU_METRICS)},
        "cameraFrameBytes": {key: stats(row[key] for row in frames) for key in CAMERA_BYTE_METRICS},
        "cameraFinalFenceWaitMs": stats(timing["finalFenceWaitMs"] for timing in camera),
        "cameraVmaMiBAtCompletion": {
            key: stats(timing["memory"][key] / (1024 * 1024) for timing in camera)
            for key in ("allocationBytes", "deviceLocalBytes")},
        "wallSeconds": {phase: stats(run[phase]["wallSeconds"] for run in runs)
                        for phase in ("simulation", "capture", "record", "verify")},
        "peakProcessResidentMiB": {
            phase: stats(run[phase]["processMemory"]["peakResidentBytes"] / (1024 * 1024)
                         for run in runs if run[phase]["processMemory"]["peakResidentBytes"] is not None)
            for phase in ("simulation", "capture", "record", "verify")},
        "datasetThroughput": {key: stats(run["record"]["throughput"][key] for run in runs)
                              for key in runs[0]["record"]["throughput"]},
        "device": camera[0]["device"],
        "build": camera[0]["build"],
        "gpuReadbackSamples": samples,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path, help="Configured CMake build directory with all tools built")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--tekla-ifc", type=Path, help="Pinned TeklaHouse.ifc, defaults to the fetched build fixture")
    parser.add_argument("--synthetic-grid", default=DEFAULT_SYNTHETIC_SIDE, type=positive_int,
                        help="Side length in rooms of the generated IFC grid")
    parser.add_argument("--iterations", default=3, type=positive_int, help="Fresh processes per scene")
    parser.add_argument("--ticks", default=60, type=positive_int)
    parser.add_argument("--width", default=640, type=positive_int)
    parser.add_argument("--height", default=480, type=positive_int)
    parser.add_argument("--timeout", default=600, type=positive_int, help="Maximum seconds per child process")
    parser.add_argument("--output", required=True, type=Path, help="Fresh directory for results and raw artifacts")
    args = parser.parse_args(argv)
    require(12 <= args.ticks < 8192, "Use 12..8191 physics ticks")
    require(len(scheduled_camera_ticks(camera_spec(args.ticks, args.width, args.height))) <= 1024,
            "The requested ticks exceed the 1024-frame native camera schedule limit")
    require(args.synthetic_grid <= MAX_SYNTHETIC_SIDE,
            f"The synthetic grid side must be in [1,{MAX_SYNTHETIC_SIDE}]")
    require(args.width <= 8192 and args.height <= 8192 and args.width * args.height <= 8_388_608,
            "Camera extent exceeds the native sensor limits")

    build = args.build_dir.resolve()
    executables = {role: find_executable(build, args.config, name) for role, name in {
        "simulator": "BimRoboticsSim", "renderer": "VulkanSceneRenderer", "dataset": "BimRoboticsDataset",
    }.items()}
    tekla = (args.tekla_ifc or build / "models" / TEKLA_RELATIVE_PATH).resolve()
    tekla_identity = file_identity(tekla)
    require(tekla_identity["sha256"] == TEKLA_SHA256, f"Tekla House differs from the pinned IFC fixture: {tekla}")

    output = args.output.resolve()
    output.mkdir(parents=True)
    synthetic = output / "synthetic.ifc"
    synthetic_count = write_synthetic_ifc(synthetic, args.synthetic_grid)
    identities = {"tekla": tekla_identity, "synthetic": file_identity(synthetic)}
    models = {"tekla": tekla, "synthetic": synthetic}
    runs = {scene: [] for scene in models}
    summary = {
        "schema": "container.bim-robotics-benchmark", "version": 1,
        "recordedAtUtc": datetime.now(timezone.utc).isoformat(),
        "host": host_identity(),
        "executables": {role: build_identity(exe, build, args.config) for role, exe in executables.items()},
        "protocol": {"configuration": args.config, "iterations": args.iterations, "physicsTicks": args.ticks,
                     "cameraSize": [args.width, args.height], "syntheticGrid": args.synthetic_grid,
                     "syntheticProductCount": synthetic_count, "processes": "fresh, sequential, OS cache uncontrolled",
                     "preprocessingCache": "disabled", "cameraMode": "lit-deferred-native-1x-msaa-no-taa",
                     "gpuTiming": "native GPU timestamps when supported"},
        "sources": identities, "scenes": runs,
    }
    for number in range(1, args.iterations + 1):
        order = list(models) if number % 2 else list(reversed(models))
        for scene in order:
            print(f"Running {scene}, iteration {number}/{args.iterations}", flush=True)
            directory = output / f"{scene}-{number:02d}"
            run = run_case(scene, models[scene], directory, executables, args,
                           expected_products=synthetic_count if scene == "synthetic" else None)
            run["directory"] = directory.name
            runs[scene].append(run)
    for scene, model in models.items():
        require(file_identity(model) == identities[scene], f"Input changed during the benchmark: {model}")
    for role, executable in executables.items():
        require(file_identity(executable)["sha256"] == summary["executables"][role]["sha256"],
                f"Native executable changed during the benchmark: {executable}")
    summary["summary"] = {scene: summarize(samples) for scene, samples in runs.items()}
    tekla_summary, synthetic_summary = (summary["summary"][scene] for scene in models)
    for key in ("device", "build"):
        require(tekla_summary[key] == synthetic_summary[key], "Renderer build or device changed between scenes")
    write_json(output / "benchmark.json", summary)
    print(f"Saved {output / 'benchmark.json'}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        sys.exit(f"BIM robotics benchmark: {error}")
