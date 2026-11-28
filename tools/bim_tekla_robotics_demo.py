"""Run, verify and export the pinned Tekla House robot demonstration."""

import argparse
import json
import math
import os
from pathlib import Path
import sys
from tempfile import mkdtemp

from benchmark_bim_import import file_identity, positive_int
from bim_robotics_benchmark_fixtures import TEKLA_RELATIVE_PATH, TEKLA_SHA256, camera_spec, simulation_scenario
from bim_robotics_camera_exr import camera_pixel, compare_camera_frames
from bim_robotics_replay_usd import write_replay_stage
from robotics_preview_dataset import read_object, run_stage, write_json


NAMESPACE = "tekla-robotics-demo"
TICK_RATE = 60
LAST_TICK = 570
CAMERA_INTERVAL = 30
LIDAR_AZIMUTH_SAMPLES = 96
START = [6.5, -20.75, 12.45]
CORNER = [7.5, -20.75, 12.45]
GOAL = [7.5, -19.75, 12.45]
GOAL_TOLERANCE_METRES = .02
SEED = 104729
COLUMN_GUID = "1GPKJW000_dp4pD3WuCZ8u"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_inputs(directory):
    scenario = simulation_scenario("tekla", LAST_TICK)
    scenario.update(initialFeet=START, seed=SEED, commands=[
        {"tick": 0, "linear": .4, "angular": 0},
        {"tick": 150, "linear": 0, "angular": math.pi / 4},
        {"tick": 270, "linear": .4, "angular": 0},
        {"tick": 420, "linear": 0, "angular": -math.pi / 4},
        {"tick": 540, "linear": 0, "angular": 0},
    ])
    scenario["lidar"].update(azimuthSamples=LIDAR_AZIMUTH_SAMPLES, elevationAngles=[0], seed=SEED)
    scenario["driveSensors"]["seed"] = SEED
    calibration = camera_spec(LAST_TICK, 320, 240)
    calibration["timing"]["framesPerSecondMilliHz"] = TICK_RATE * 1000 // CAMERA_INTERVAL
    write_json(directory / "scenario.json", scenario)
    write_json(directory / "camera.json", calibration)


def check_motion(report, source):
    history = report["history"]
    require(report["schema"] == "container.bim-robotics-simulation" and report["version"] == 1 and
            report["coordinateConvention"] == "ROS REP-103" and
            report["sourceNamespace"] == NAMESPACE and report["seed"] == SEED and
            report["ticksPerSecond"] == TICK_RATE and report["sceneSource"]["layers"] == [source],
            "The simulation report does not match the pinned source and schedule")
    require([row["tick"] for row in history] == list(range(LAST_TICK + 1)) and report["state"] == history[-1],
            "The simulation lost a committed tick or reported a different final pose")
    for row in history:
        require(row["obstruction"] == "none" and not row["blockingEntityId"],
                f"The robot met {row['obstruction']} at tick {row['tick']}: {row['blockingEntityId']}")
    points = [row["base"]["position"] for row in history]
    for tick, expected in ((0, START), (150, CORNER), (270, CORNER), (420, GOAL), (LAST_TICK, GOAL)):
        require(math.dist(points[tick], expected) < GOAL_TOLERANCE_METRES,
                f"Unexpected position at tick {tick}: {points[tick]}, expected {expected}")
    require(math.isclose(history[420]["base"]["yaw"], math.pi / 2, abs_tol=1e-6) and
            math.isclose(history[-1]["base"]["yaw"], 0, abs_tol=1e-6), "The robot missed its inspection heading")
    return points


def check_route(output, invoke, model, points):
    queries = output / "collision-queries.json"
    report = output / "collision.json"
    walks = [{"from": a, "to": b} for a, b in zip(points, points[1:]) if math.dist(a, b) > 1e-6]
    # The simulator and collision CLI use the same default upright capsule.
    goal = points[-1]
    blocked = {"from": goal, "to": [goal[0] + 2, *goal[1:]]}
    write_json(queries, {"version": 1, "walks": [*walks, blocked]})
    invoke("check-route", "BimRoboticsCollision", (model, report, NAMESPACE, queries))
    results = read_object(report)["walks"]
    require(len(walks) > 0 and len(results) == len(walks) + 1,
            "The collision checker missed a moving segment or the blocked control")
    for index, (query, result) in enumerate(zip(walks, results)):
        require(result["from"] == query["from"] and result["to"] == query["to"] and result["reachable"] and
                result["supportSamples"], f"Segment {index} failed collision or support checks: {result['reason']}")
    control = results[-1]
    require(control["from"] == blocked["from"] and control["to"] == blocked["to"] and not control["reachable"] and
            control["reason"] == "blocked" and (control.get("blockingHit") or {}).get("entityId", "").endswith(COLUMN_GUID),
            "The collision checker let the blocked control pass the inspection column")
    return len(walks)


def check_column_observation(report):
    scans = report["lidar"]["scans"]
    require(scans and scans[-1]["tick"] == LAST_TICK and len(scans[-1]["returns"]) == LIDAR_AZIMUTH_SAMPLES,
            "The simulation report lacks the scheduled final LiDAR scan")
    scan = scans[-1]
    hit = scan["returns"][LIDAR_AZIMUTH_SAMPLES // 2]
    require(hit["status"] == "valid" and hit["entityId"].endswith(COLUMN_GUID) and
            math.isclose(hit["rangeMeters"], 1.5, abs_tol=.01), "The final forward ray missed the Tekla inspection column")
    return {"guid": COLUMN_GUID, "sourceId": "#14449", **hit}


def check_camera_capture(output):
    manifest = read_object(output / "dataset.json")
    frames = sorted(output.glob("frame-*.json"))
    expected = list(range(0, LAST_TICK + 1, CAMERA_INTERVAL))
    metadata = [read_object(frame) for frame in frames]
    require(manifest["frameCount"] == len(expected) and [frame["tick"] for frame in metadata] == expected,
            "The camera capture missed a scheduled tick")
    hashes = [file_identity(frame.with_suffix(".exr"))["sha256"] for frame in frames]
    require(len(set(hashes)) > 1, "The camera did not record changing observations along the route")
    labels = read_object(output / "labels.json")
    require(labels["sourceNamespace"] == NAMESPACE and
            any(row["guid"] == COLUMN_GUID and row["occurrenceId"] == "#14449" for row in labels["instances"]),
            "The camera label table lost the pinned Tekla column occurrence")
    return hashes, metadata, manifest


def capture_and_record(output, invoke, model, report, native_history, validation):
    first = output / "camera-capture"
    repeat = output / "camera-repeat"
    for name, directory in (("capture", first), ("capture-repeat", repeat)):
        invoke(name, "VulkanSceneRenderer", (
            "--model", model, "--hidden", "--no-ui", "--render-technique", "deferred-raster",
            "--msaa", "1", "--no-taa", "--no-ddgi", "--no-ray-query", "--no-bloom",
            "--sensor-camera", output / "camera.json", "--sensor-trajectory", report,
            "--sensor-output", directory, "--sensor-namespace", NAMESPACE,
            "--validation" if validation else "--no-validation",
        ))
    hashes, frames, original_manifest = check_camera_capture(first)
    repeated_hashes, repeated_frames, repeated_manifest = check_camera_capture(repeat)
    require((frames, original_manifest) == (repeated_frames, repeated_manifest),
            "Repeated captures changed the camera calibration, poses or label manifest")
    intrinsic = frames[0]["intrinsics"]
    width, height = intrinsic["width"], intrinsic["height"]
    max_rgb_error = 0.0
    for index, (original, repeated) in enumerate(zip(hashes, repeated_hashes)):
        if original != repeated:
            name = original_manifest["frameImagePattern"] % index
            max_rgb_error = max(max_rgb_error, compare_camera_frames(first / name, repeat / name, width, height))

    labels = read_object(first / "labels.json")
    column = next(row for row in labels["instances"] if row["guid"] == COLUMN_GUID and row["occurrenceId"] == "#14449")
    final_image = first / (original_manifest["frameImagePattern"] % (len(frames) - 1))
    center = camera_pixel(final_image, width, height, width // 2, height // 2)
    require(center["InstanceID"] == column["instanceId"] and center["SemanticID"] == column["semanticId"] and
            math.isclose(center["Z"], 1.38, abs_tol=.02) and max(center[key] for key in ("R", "G", "B")) > 0,
            "The final camera ray did not see the Tekla inspection column at the expected depth")

    dataset = output / "dataset"
    verified = output / "logs" / "verify.json"
    replay = output / "dataset-replay.jsonl"
    invoke("record", "BimRoboticsDataset", ("record", first, report, dataset))
    invoke("verify", "BimRoboticsDataset", ("verify", dataset), stdout_path=verified)
    invoke("replay", "BimRoboticsDataset", ("replay", dataset), stdout_path=replay)
    manifest = read_object(verified)
    require(manifest == read_object(dataset / "dataset.json") and manifest["cameraFrameCount"] == len(hashes) and
            manifest["simulationReportSha256"] == file_identity(report)["sha256"],
            "The verified dataset does not match the simulation and camera capture")
    rows = [json.loads(line) for line in replay.read_bytes().splitlines()]
    require([row["base"] for row in rows] == native_history and
            [row["camera"]["sha256"] for row in rows if row.get("camera")] == hashes,
            "Dataset replay changed the committed pose or camera observations")
    return {"dataset": "dataset/dataset.json", "frameCount": len(hashes), "recordedFrameSha256": hashes,
            "byteIdenticalFrames": sum(a == b for a, b in zip(hashes, repeated_hashes)),
            "maxLinearRgbAbsoluteDifference": max_rgb_error, "inspectionCenterPixel": center}


def run_demo(output, bin_dir, model, timeout, *, cpu_only=False, validation=False):
    identity = file_identity(model)
    require(identity["sha256"] == TEKLA_SHA256, f"Expected the pinned Tekla House IFC at {model}")
    source = {"byteSize": identity["bytes"], "sha256": identity["sha256"]}
    names = ["BimRoboticsSim", "BimRoboticsUsd", "BimRoboticsCollision"]
    if not cpu_only:
        names += ["VulkanSceneRenderer", "BimRoboticsDataset"]
    suffix = ".exe" if os.name == "nt" else ""
    binaries = {name: bin_dir / (name + suffix) for name in names}
    missing = [str(path) for path in binaries.values() if not path.is_file()]
    require(not missing, "Build the required Release executables:\n" + "\n".join(missing))
    output.mkdir(parents=True)
    logs = output / "logs"
    logs.mkdir()

    def invoke(label, tool, arguments, *, stdout_path=None):
        run_stage(label, binaries[tool], arguments, logs, timeout, stdout_path=stdout_path)

    write_inputs(output)
    scenario = output / "scenario.json"
    report_file, repeat = output / "simulation.json", output / "simulation-repeat.json"
    for label, destination in (("simulate", report_file), ("simulate-repeat", repeat)):
        invoke(label, "BimRoboticsSim", (model, destination, NAMESPACE, scenario))
    require(report_file.read_bytes() == repeat.read_bytes(), "The seeded simulation changed between repeat runs")
    report = read_object(report_file)
    points = check_motion(report, source)
    sweeps = check_route(output, invoke, model, points)
    column = check_column_observation(report)

    scene = output / "scene.usda"
    replay = output / "replay.usda"
    invoke("export-bim", "BimRoboticsUsd", (model, scene, NAMESPACE))
    write_replay_stage(replay, scene.name, report)
    cameras = None if cpu_only else capture_and_record(output, invoke, model, report_file, report["history"], validation)
    pending_summary = logs / "summary.partial.json"
    write_json(pending_summary, {
        "schema": "container.tekla-robotics-demo", "version": 1, "source": source, "sourceNamespace": NAMESPACE,
        "scenario": scenario.name, "simulation": report_file.name, "cameraCalibration": "camera.json",
        "entryStage": replay.name, "scene": scene.name, "seed": SEED, "tickCount": len(points),
        "distanceMetres": sum(math.dist(a, b) for a, b in zip(points, points[1:])),
        "goalErrorMetres": math.dist(points[-1], GOAL), "verifiedMovingSegments": sweeps,
        "inspectionTarget": column,
        "stageSha256": {path.name: file_identity(path)["sha256"] for path in (scene, replay)}, "cameras": cameras,
    })
    pending_summary.replace(output / "summary.json")
    print(f"Verified Tekla robotics run at {output / 'summary.json'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True, help="Release executable directory")
    parser.add_argument("--ifc", type=Path, help="Pinned TeklaHouse.ifc (defaults to the build's sample)")
    parser.add_argument("--output", type=Path, required=True, help="New output directory")
    parser.add_argument("--timeout", type=positive_int, default=300, help="Timeout per stage in seconds")
    parser.add_argument("--cpu-only", action="store_true", help="Verify and export the route without Vulkan captures")
    parser.add_argument("--validation", action="store_true", help="Enable Vulkan validation during camera captures")
    parser.add_argument("--ctest", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    bin_dir = args.bin_dir.resolve()
    sample_root = os.environ.get("CONTAINER_IFC_SAMPLE_ROOT")
    model = args.ifc or (Path(sample_root) / "Tekla House" / "TeklaHouse.ifc" if sample_root else
                         bin_dir / "models" / TEKLA_RELATIVE_PATH)
    if args.ctest and args.ifc is None and not sample_root and not model.is_file():
        print("SKIP: pinned Tekla House IFC missing. Set CONTAINER_IFC_SAMPLE_ROOT to the buildingSMART examples directory.")
        return 77
    try:
        output = args.output.resolve()
        if args.ctest:
            output.mkdir(parents=True, exist_ok=True)
            output = Path(mkdtemp(prefix="ctest-", dir=output)) / "demo"
        run_demo(output, bin_dir, model.resolve(), args.timeout, cpu_only=args.cpu_only, validation=args.validation)
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"Tekla robotics demo failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
