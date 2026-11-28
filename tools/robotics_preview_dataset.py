"""Generate and verify the BIM robotics preview's synthetic doorway dataset."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / "tests" / "fixtures" / "robotics" / "doorway.ifc"
NAMESPACE = "doorway-preview"
TICK_RATE = 12
LAST_TICK = 7
TICKS_PER_CHUNK = 3
CAMERA_TICKS = [0, 2, 4, 6]
IMU_TICKS = [1, 4, 7]
LIDAR_TICKS = [2, 6]
MOUNT = {"xyz": [0, 0, 0.7]}
INTRINSICS = {
    "width": 127, "height": 95, "fx": 72, "fy": 64, "cx": 54.5, "cy": 38.5,
    "nearMeters": 0.2, "farMeters": 6,
}


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")


def read_object(path):
    try:
        result = json.loads(path.read_bytes())
    except (OSError, ValueError) as error:
        raise RuntimeError(f"Could not read {path}\n{error}") from error
    if not isinstance(result, dict):
        raise RuntimeError(f"Expected a JSON object in {path}")
    return result


def write_inputs(out):
    scenario = {
        "version": 1,
        "initialFeet": [-1.5, 0, 1.2],
        "ticks": LAST_TICK,
        "ticksPerSecond": TICK_RATE,
        "commands": [{"tick": 0, "linear": 0.2, "angular": 0}],
        "driveSensors": {"samplesPerSecondMilliHz": 4000, "firstTick": 1},
        "lidar": {
            "scansPerSecondMilliHz": 3000, "firstTick": 2, "azimuthSamples": 32,
            "minRangeMeters": 0.05, "maxRangeMeters": 6, "mount": MOUNT,
        },
    }
    calibration = {
        "schema": "container.sensor-camera",
        "version": 1,
        "intrinsics": INTRINSICS,
        "baseFromCamera": MOUNT,
        "timing": {
            "ticksPerSecond": TICK_RATE, "framesPerSecondMilliHz": 6000,
            "firstTick": 0, "lastTick": LAST_TICK,
        },
    }
    write_json(out / "scenario.json", scenario)
    write_json(out / "camera.json", calibration)


def diagnostics(path):
    with path.open("rb") as stream:
        stream.seek(max(0, path.stat().st_size - 8192))
        lines = stream.read().decode("utf-8", errors="replace").splitlines()
    return "\n".join(lines[-24:])


def run_stage(label, executable, arguments, logs, timeout, *, stdout_path=None):
    log = logs / f"{label}.log"
    command = [str(executable), *map(str, arguments)]
    command_line = subprocess.list2cmdline(command) if os.name == "nt" else shlex.join(command)
    print(f"{label} (log {log})", flush=True)

    def run(stdout, stderr):
        return subprocess.run(
            command, cwd=executable.parent, stdin=subprocess.DEVNULL,
            stdout=stdout, stderr=stderr, timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        ).returncode

    try:
        with log.open("xb") as trace:
            trace.write(f"$ {command_line}\n".encode("utf-8"))
            trace.flush()
            if stdout_path is None:
                code = run(trace, subprocess.STDOUT)
            else:
                with stdout_path.open("xb") as output:
                    code = run(output, trace)
        if code == 0:
            return
        reason = f"exited with status {code}"
        if os.name == "nt" and (code & 0xFFFFFFFF) == 0xC0000135:
            reason += " (a runtime DLL is missing)"
    except subprocess.TimeoutExpired:
        reason = f"timed out after {timeout} seconds"
    except OSError as error:
        reason = str(error)

    detail = f"{label} {reason}. Log at {log}"
    if stdout_path is not None and stdout_path.is_file():
        detail += f"\nStandard output at {stdout_path}"
    if log.is_file():
        detail += "\n" + diagnostics(log)
    raise RuntimeError(detail)


def verify_replay(dataset, report, manifest_path, replay_path):
    manifest = read_object(manifest_path)
    expected = {
        "schema": "container.robotics-dataset",
        "version": 1,
        "sourceNamespace": NAMESPACE,
        "ticksPerSecond": TICK_RATE,
        "tickCount": LAST_TICK + 1,
        "ticksPerChunk": TICKS_PER_CHUNK,
        "chunkCount": 3,
        "cameraFrameCount": len(CAMERA_TICKS),
        "imuSampleCount": len(IMU_TICKS),
        "lidarScanCount": len(LIDAR_TICKS),
    }
    if manifest != read_object(dataset / "dataset.json"):
        raise RuntimeError(f"Verification output differs from {dataset / 'dataset.json'}")
    for field, value in expected.items():
        if manifest.get(field) != value:
            raise RuntimeError(f"Expected {field}={value!r} in {manifest_path}, got {manifest.get(field)!r}")

    try:
        rows = [json.loads(line) for line in replay_path.read_bytes().splitlines()]
    except (OSError, ValueError) as error:
        raise RuntimeError(f"Could not read {replay_path}\n{error}") from error
    expected_ticks = list(range(LAST_TICK + 1))
    if any(not isinstance(row, dict) for row in rows) or [row.get("tick") for row in rows] != expected_ticks:
        raise RuntimeError(f"Expected one replay row per committed tick in {replay_path}")
    history = read_object(report).get("history")
    if [row.get("base") for row in rows] != history or rows[0]["base"] == rows[-1]["base"]:
        raise RuntimeError(f"Replay did not preserve the simulated motion in {report}")
    for channel, ticks in (("camera", CAMERA_TICKS), ("imu", IMU_TICKS), ("lidar", LIDAR_TICKS)):
        observed = [row["tick"] for row in rows if row.get(channel) is not None]
        if observed != ticks:
            raise RuntimeError(f"Expected {channel} samples at ticks {ticks} in {replay_path}, got {observed}")
    return expected


def generate(out, bin_dir, timeout):
    suffix = ".exe" if os.name == "nt" else ""
    simulator, renderer, recorder = (
        bin_dir / (name + suffix) for name in ("BimRoboticsSim", "VulkanSceneRenderer", "BimRoboticsDataset")
    )
    missing = [str(binary) for binary in (simulator, renderer, recorder) if not binary.is_file()]
    if missing:
        raise RuntimeError("Missing preview binaries\n" + "\n".join(missing) +
                           "\nRun from the extracted preview or pass --bin-dir pointing to the Release executables.")
    if not MODEL.is_file():
        raise RuntimeError(f"Missing the preview doorway fixture at {MODEL}")
    try:
        out.mkdir(parents=True)
    except FileExistsError as error:
        raise RuntimeError(f"Output already exists at {out}. Choose a new directory.") from error

    logs = out / "logs"
    logs.mkdir()
    write_inputs(out)
    report = out / "simulation.json"
    camera = out / "camera-capture"
    dataset = out / "dataset"
    manifest_path = logs / "verify.json"
    replay_pending = logs / "replay.partial.jsonl"
    replay = out / "replay.jsonl"

    run_stage("simulate", simulator, (MODEL, report, NAMESPACE, out / "scenario.json"), logs, timeout)
    run_stage("capture", renderer, (
        "--model", MODEL, "--import-scale", "2.5", "--hidden", "--no-ui",
        "--render-technique", "deferred-raster", "--display-mode", "lit", "--msaa", "1", "--no-taa",
        "--no-bloom", "--no-ray-query", "--no-ddgi", "--sky", "plain", "--sky-horizon", "0", "0", "0",
        "--environment-intensity", "0", "--directional-intensity", "3", "--directional-direction", "0", "0", "-1",
        "--bounce-intensity", "0", "--sensor-camera", out / "camera.json", "--sensor-trajectory", report,
        "--sensor-output", camera, "--sensor-namespace", NAMESPACE,
    ), logs, timeout)
    run_stage("record", recorder, ("record", camera, report, dataset, "--ticks-per-chunk", TICKS_PER_CHUNK),
              logs, timeout)
    run_stage("verify", recorder, ("verify", dataset), logs, timeout, stdout_path=manifest_path)
    run_stage("replay", recorder, ("replay", dataset), logs, timeout, stdout_path=replay_pending)
    counts = verify_replay(dataset, report, manifest_path, replay_pending)
    summary_pending = logs / "summary.partial.json"
    write_json(summary_pending, {
        "schema": "container.robotics-preview-dataset",
        "version": 1,
        "dataset": "dataset/dataset.json",
        "replay": "replay.jsonl",
        "tickCount": counts["tickCount"],
        "cameraFrameCount": counts["cameraFrameCount"],
        "imuSampleCount": counts["imuSampleCount"],
        "lidarScanCount": counts["lidarScanCount"],
    })
    replay_pending.replace(replay)
    summary_pending.replace(out / "summary.json")
    print(f"Verified dataset at {dataset / 'dataset.json'}\nReplay at {replay}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="New directory for the dataset, inputs and diagnostics")
    parser.add_argument("--bin-dir", type=Path, default=ROOT,
                        help="Directory containing the Release executables (default: extracted preview root)")
    parser.add_argument("--timeout", type=int, default=180, help="Timeout in seconds per stage (default: 180)")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        generate(args.output.resolve(), args.bin_dir.resolve(), args.timeout)
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
