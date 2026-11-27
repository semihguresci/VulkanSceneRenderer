"""Record and reopen a synchronized dataset from the native simulator and renderer."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from bim_sensor_camera_runtime_regression import INTRINSICS, NAMESPACE, capture, read_exr, write_json


RATE = 12
TICKS = 7
CHUNK_SIZE = 3
IMPORT_SCALE = 2.5
MOUNT_Z = 0.7
CAMERA_TICKS = (0, 2, 4, 6)
IMU_TICKS = (1, 4, 7)
LIDAR_TICKS = (2, 6)


def run(executable, arguments, log, expected_error=None):
    result = subprocess.run(
        [str(executable), *map(str, arguments)],
        cwd=executable.parent,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if expected_error is None:
        assert result.returncode == 0, (log, result.returncode, result.stderr)
    else:
        assert result.returncode != 0 and expected_error in result.stderr, (log, result.stderr)
    return result.stdout


def fingerprint(path):
    payload = path.read_bytes()
    return {"sha256": hashlib.sha256(payload).hexdigest(), "byteSize": len(payload)}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def jsonl(text):
    return [json.loads(line) for line in text.splitlines()]


def close(value, expected, tolerance=1e-12):
    assert math.isclose(value, expected, rel_tol=tolerance, abs_tol=0), (value, expected)


def check_camera_source(camera, report_path, report, model):
    manifest = read_json(camera / "dataset.json")
    evidence = fingerprint(model)
    assert report["sceneSource"]["layers"] == manifest["sceneSource"]["layers"] == [evidence]
    assert manifest["simulationReportSha256"] == fingerprint(report_path)["sha256"]
    close(manifest["sceneSource"]["metersPerUnscaledRendererUnit"],
          report["sceneSource"]["metersPerUnscaledRendererUnit"], 1e-6)

    assert manifest["frameCount"] == len(CAMERA_TICKS)
    labels = read_json(camera / "labels.json")
    close(labels["metersPerRendererUnit"] * IMPORT_SCALE,
          manifest["sceneSource"]["metersPerUnscaledRendererUnit"])
    assert labels["sourceNamespace"] == report["sourceNamespace"] == NAMESPACE
    classes = {row["semanticId"]: row["ifcClass"] for row in labels["classes"]}
    instances = {row["guid"]: row for row in labels["instances"] if row["guid"]}
    for guid, semantic_class in (("wall-guid", "IFCWALL"), ("door-guid", "IFCDOOR"),
                                 ("lower-slab", "IFCSLAB")):
        row = instances[guid]
        assert row["occurrenceId"] and row["representationIds"] and row["instanceId"] > 0, row
        assert classes[row["semanticId"]] == semantic_class, row
    expected_ids = {(row["instanceId"], row["semanticId"]) for row in labels["instances"]} | {(0, 0)}
    foreground_ids = {instances[guid]["instanceId"] for guid in ("wall-guid", "door-guid")}
    planes = read_exr(camera / "frame-00000000.exr")
    actual_ids = set()
    for ids, semantic, depths in zip(planes["InstanceID"], planes["SemanticID"], planes["Z"], strict=True):
        for instance, label, depth in zip(ids, semantic, depths, strict=True):
            assert (instance, label) in expected_ids and math.isfinite(depth)
            assert bool(instance) == (depth > 0)
            actual_ids.add(instance)
    assert actual_ids & foreground_ids, actual_ids
    return manifest


def check_replay(dataset, manifest, rows, camera, report):
    assert manifest == read_json(dataset / "dataset.json")
    assert manifest["sceneSource"] == report["sceneSource"]
    assert manifest["ticksPerSecond"] == RATE
    assert manifest["tickCount"] == len(rows) == TICKS + 1
    assert manifest["ticksPerChunk"] == CHUNK_SIZE and manifest["chunkCount"] == 3
    assert manifest["cameraFrameCount"] == len(CAMERA_TICKS)
    assert manifest["imuSampleCount"] == len(IMU_TICKS) and manifest["lidarScanCount"] == len(LIDAR_TICKS)
    assert (dataset / "labels.json").read_bytes() == (camera / "labels.json").read_bytes()
    for index, expected_size in enumerate((3, 3, 2)):
        path = dataset / f"chunk-{index:08}.jsonl"
        assert manifest["chunks"][index] == fingerprint(path), path
        assert jsonl(path.read_text(encoding="utf-8")) == rows[index * CHUNK_SIZE:index * CHUNK_SIZE + expected_size]

    imu = {row["tick"]: row for row in report["driveSensors"]["samples"]}
    lidar = {row["tick"]: row for row in report["lidar"]["scans"]}
    assert tuple(imu) == IMU_TICKS and tuple(lidar) == LIDAR_TICKS, (tuple(imu), tuple(lidar))
    assert any(hit["entityId"] for scan in lidar.values() for hit in scan["returns"])
    cameras = {}
    for index, tick in enumerate(CAMERA_TICKS):
        stem = f"frame-{index:08}"
        frame = read_json(camera / (stem + ".json"))
        image = stem + ".exr"
        assert frame["tick"] == tick and frame["sampleIndex"] == index
        assert frame["intrinsics"] == INTRINSICS
        assert frame["worldFrame"] == "REP-103 metres"
        close(frame["seconds"], tick / RATE)
        position = report["history"][tick]["base"]["position"]
        for axis, expected in enumerate((*position[:2], position[2] + MOUNT_Z)):
            assert math.isclose(frame["worldFromCamera"][3][axis], expected, abs_tol=1e-12)
        assert (dataset / image).read_bytes() == (camera / image).read_bytes(), image
        cameras[tick] = {"frame": frame, "image": image, **fingerprint(camera / image)}

    for tick, row in enumerate(rows):
        assert row["tick"] == tick
        close(row["seconds"], tick / RATE)
        assert row["base"] == report["history"][tick], tick
        assert row["imu"] == imu.get(tick), tick
        assert row["lidar"] == lidar.get(tick), tick
        assert row["camera"] == cameras.get(tick), tick


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("recorder", "simulator", "renderer", "fixtures", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--managed-output", action="store_true")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_ROBOTICS_DATASET") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_ROBOTICS_DATASET=1 to run the live robotics dataset pipeline")
        return 77

    simulator, renderer, recorder = (path.resolve() for path in (args.simulator, args.renderer, args.recorder))
    model = args.fixtures.resolve() / "doorway.ifc"
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=args.managed_output)
    if args.managed_output:
        work = Path(tempfile.mkdtemp(prefix="run-", dir=work))
    print(f"Robotics dataset pipeline artifacts: {work}", flush=True)
    scenario = work / "scenario.json"
    write_json(scenario, {
        "version": 1, "initialFeet": [-1.5, 0, 1.2], "ticks": TICKS, "ticksPerSecond": RATE,
        "commands": [{"tick": 0, "linear": 0.2, "angular": 0}],
        "driveSensors": {"samplesPerSecondMilliHz": 4000, "firstTick": 1},
        "lidar": {"scansPerSecondMilliHz": 3000, "firstTick": 2, "azimuthSamples": 8,
                  "minRangeMeters": 0.05, "maxRangeMeters": 6,
                  "mount": {"xyz": [0, 0, MOUNT_Z]}},
    })
    report_path = work / "simulation.json"
    run(simulator, (model, report_path, NAMESPACE, scenario), work / "simulate.log")
    report = read_json(report_path)
    assert report["history"][0]["base"] != report["history"][-1]["base"]

    spec = work / "camera.json"
    write_json(spec, {
        "schema": "container.sensor-camera", "version": 1, "intrinsics": INTRINSICS,
        "baseFromCamera": {"xyz": [0, 0, MOUNT_Z]},
        "timing": {"ticksPerSecond": RATE, "framesPerSecondMilliHz": 6000, "firstTick": 0, "lastTick": TICKS},
    })
    (work / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="ascii")
    camera = work / "camera"
    capture(renderer, work, model, spec, camera, trajectory=report_path, scale=IMPORT_SCALE, log_name="capture.log")
    camera_manifest = check_camera_source(camera, report_path, report, model)

    dataset = work / "dataset"
    run(recorder, ("record", camera, report_path, dataset, "--ticks-per-chunk", CHUNK_SIZE), work / "record.log")
    manifest = json.loads(run(recorder, ("verify", dataset), work / "verify.json"))
    assert manifest["sensors"]["camera"] == camera_manifest
    rows = jsonl(run(recorder, ("replay", dataset), work / "replay.jsonl"))
    check_replay(dataset, manifest, rows, camera, report)

    alternate_report = work / "unmatched-simulation.json"
    alternate_report.write_bytes(report_path.read_bytes() + b"\n")
    rejected = work / "unmatched-dataset"
    run(recorder, ("record", camera, alternate_report, rejected, "--ticks-per-chunk", CHUNK_SIZE),
        work / "unmatched.log", "capture must replay the exact supplied simulation report")
    assert not (rejected / "dataset.json").exists()

    # Remove the old paths for the generator inputs as well as the dataset.
    inputs = work / "moved inputs"
    inputs.mkdir()
    camera.rename(inputs / camera.name)
    report_path.rename(inputs / report_path.name)
    relocated = work / "relocated dataset"
    dataset.rename(relocated)
    assert json.loads(run(recorder, ("verify", relocated), work / "relocated-verify.json")) == manifest
    assert jsonl(run(recorder, ("replay", relocated), work / "relocated-replay.jsonl")) == rows
    write_json(work / "summary.json", {"cameraTicks": CAMERA_TICKS, "imuTicks": IMU_TICKS,
                                      "lidarTicks": LIDAR_TICKS, "chunkSizes": (3, 3, 2),
                                      "sceneSource": manifest["sceneSource"]})
    print(f"PASS native simulation, calibrated BIM camera, and portable dataset replay ({work})", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
