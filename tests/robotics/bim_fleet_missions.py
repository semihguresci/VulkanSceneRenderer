"""Verify a deterministic CPU fleet mission and native twin replay against IFC."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile


NAMESPACE = "doorway"
LOWER = "robot/lower-inspector"
UPPER = "robot/upper-survey"
DOOR_ID = "bim/7:doorway/guid/9:door-guid"
NANOS_PER_SECOND = 1_000_000_000
ARTIFACTS = {"report.json", "telemetry.json"}


def run(tool, *arguments):
    return subprocess.run([str(tool), *map(str, arguments)], cwd=tool.parent,
                          capture_output=True, encoding="utf-8", errors="replace", timeout=30)


def success(tool, *arguments):
    result = run(tool, *arguments)
    assert result.returncode == 0, (result.args, result.returncode, result.stdout, result.stderr)
    return result


def publish_json(path, value):
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")
    return path


def artifacts(output):
    assert {file.name for file in output.iterdir()} == ARTIFACTS
    return tuple(json.loads((output / name).read_bytes()) for name in ("report.json", "telemetry.json"))


def check_recording(twin, output, report, recording, identities, layer, scenario, *, contact_robot=None):
    expected_version = 4 if contact_robot else 2
    assert report["schema"] == "container.bim-fleet-report" and report["version"] == 1
    assert report["coordinateConvention"] == "ROS REP-103"
    assert recording["schema"] == "container.bim-twin-telemetry" and recording["version"] == expected_version
    assert report["sourceNamespace"] == recording["sourceNamespace"] == NAMESPACE
    assert report["sceneSource"] == recording["sceneSource"]
    assert recording["sceneSource"]["layers"] == [layer]
    assert abs(recording["sceneSource"]["metersPerUnscaledRendererUnit"] - 1) < 1e-6
    assert report["clockId"] == recording["clockId"] == "fleet-fixed-step"
    assert report["ticksPerSecond"] == scenario["ticksPerSecond"]
    assert report["waypointToleranceMeters"] == scenario["waypointToleranceMeters"]

    def ns(tick):
        return tick * NANOS_PER_SECOND // report["ticksPerSecond"]

    recording_file = output / "telemetry.json"
    verified = json.loads(success(twin, "verify", recording_file).stdout)
    assert verified["sceneSource"] == recording["sceneSource"]
    assert verified["recordingVersion"] == expected_version and verified["eventCount"] == len(recording["events"])
    assert verified["firstTimestampNanoseconds"] == 0
    assert verified["lastTimestampNanoseconds"] == ns(report["ticks"])
    start = json.loads(success(twin, "sample", recording_file, 0).stdout)
    finish = json.loads(success(twin, "sample", recording_file, ns(report["ticks"])).stdout)
    initial_tracks = {(row["event"]["entityId"], row["event"]["kind"]): row for row in start["tracks"]}
    final_tracks = {(row["event"]["entityId"], row["event"]["kind"]): row for row in finish["tracks"]}
    assert set(initial_tracks) == set(final_tracks) == {
        (robot, kind) for robot in identities for kind in ("robot-pose", "planned-path", "task-status")}

    by_robot = {robot["robotId"]: robot for robot in scenario["robots"]}
    assert [robot["robotId"] for robot in report["robots"]] == sorted(identities)
    for result in report["robots"]:
        robot_id = result["robotId"]
        specification = by_robot[robot_id]
        assert result["taskId"] == specification["taskId"]
        pose = initial_tracks[(robot_id, "robot-pose")]["event"]["payload"]
        assert math.dist(pose["position"], specification["initialFeet"]) < 1e-5
        assert "contact" not in pose
        assert initial_tracks[(robot_id, "planned-path")]["event"]["payload"]["points"] == [
            specification["initialFeet"], *specification["waypoints"]]
        assert initial_tracks[(robot_id, "task-status")]["event"]["payload"] == {
            "taskId": result["taskId"], "state": "running", "progress": 0}

        poses = {row["timestampNanoseconds"]: row["payload"] for row in recording["events"]
                 if row["entityId"] == robot_id and row["kind"] == "robot-pose"}
        positions = {timestamp: pose["position"] for timestamp, pose in poses.items()}
        assert sorted(positions) == [ns(tick) for tick in range(report["ticks"] + 1)]
        assert all(abs(position[2] - specification["initialFeet"][2]) < 1e-5 for position in positions.values())
        contacts = [pose["contact"] for pose in poses.values() if "contact" in pose]
        if robot_id == contact_robot:
            assert len(contacts) == result["blockedTicks"] > 0
            for contact in contacts:
                assert contact["otherEntityId"] == DOOR_ID
                x, y, z = contact["positionMeters"]
                assert math.isclose(x, -0.03, abs_tol=0.003) and abs(y) < 0.05 and 1.2 <= z <= 2.8, contact
                normal = contact["normal"]
                assert normal[0] < -0.99 and math.isclose(math.hypot(*normal), 1, abs_tol=1e-6), contact
        else:
            assert not contacts

        for index, arrival in enumerate(result["arrivals"]):
            assert arrival["waypointIndex"] == index
            assert arrival["position"] == positions[ns(arrival["tick"])]
            assert math.dist(arrival["position"], specification["waypoints"][index]) <= report["waypointToleranceMeters"]
        pose_track = final_tracks[(robot_id, "robot-pose")]
        final_pose = pose_track["event"]["payload"]
        assert not pose_track["stale"] and final_pose == poses[ns(report["ticks"])]
        assert {key: final_pose[key] for key in ("position", "yaw")} == result["finalPose"]
        assert len(pose_track["observedPath"]) > 1

        task = final_tracks[(robot_id, "task-status")]["event"]
        completed = result["completionTick"] is not None
        assert task["timestampNanoseconds"] == ns(result["completionTick"] if completed else report["ticks"])
        assert task["payload"] == {"taskId": result["taskId"], "state": "succeeded" if completed else "timed-out",
                                   "progress": len(result["arrivals"]) / len(specification["waypoints"])}
        if completed:
            assert len(result["arrivals"]) == len(specification["waypoints"])
            assert result["completionTick"] == result["arrivals"][-1]["tick"]
            assert all(position == result["finalPose"]["position"]
                       for timestamp, position in positions.items() if timestamp >= ns(result["completionTick"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fleet", type=Path, required=True)
    parser.add_argument("--twin", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    fleet, twin, fixtures = args.fleet.resolve(), args.twin.resolve(), args.fixtures.resolve()
    model, mission = fixtures / "doorway.ifc", fixtures / "doorway-fleet.json"
    source_bytes = model.read_bytes()
    raw_mission = mission.read_bytes()
    layer = {"byteSize": len(source_bytes), "sha256": hashlib.sha256(source_bytes).hexdigest()}
    scenario = json.loads(raw_mission)

    with tempfile.TemporaryDirectory(prefix="bim-fleet-") as directory:
        work = Path(directory)
        completed, repeated, timed_out = (work / name for name in ("completed", "repeated", "timed-out"))
        success(fleet, model, mission, completed, NAMESPACE)
        success(fleet, model, mission, repeated, NAMESPACE)
        report, recording = artifacts(completed)
        assert report["completed"] and 0 < report["ticks"] < scenario["maxTicks"]
        assert report["ticks"] == max(robot["completionTick"] for robot in report["robots"])
        assert all(robot["blockedTicks"] == 0 and robot["lastObstruction"] == "none" for robot in report["robots"])
        check_recording(twin, completed, report, recording, {LOWER, UPPER}, layer, scenario)
        for name in ARTIFACTS:
            assert (completed / name).read_bytes() == (repeated / name).read_bytes(), name

        canonical = work / "reimport.json"
        success(twin, "import", completed / "telemetry.json", canonical)
        assert canonical.read_bytes() == (completed / "telemetry.json").read_bytes()

        closed_scenario = {**scenario, "openDoorIds": []}
        closed_mission = publish_json(work / "closed.json", closed_scenario)
        repeated_timeout = work / "repeated-timeout"
        for output in (timed_out, repeated_timeout):
            timeout = run(fleet, model, closed_mission, output, NAMESPACE)
            assert timeout.returncode == 2 and "timed out" in timeout.stdout, (timeout.stdout, timeout.stderr)
        for name in ARTIFACTS:
            assert (timed_out / name).read_bytes() == (repeated_timeout / name).read_bytes(), name
        blocked, partial = artifacts(timed_out)
        assert not blocked["completed"] and blocked["ticks"] == scenario["maxTicks"]
        lower, upper = blocked["robots"]
        assert lower["robotId"] == LOWER and lower["completionTick"] is None
        assert len(lower["arrivals"]) == 1 and lower["finalPose"]["position"][0] < 0
        assert lower["lastObstruction"] == "blocked" and lower["blockingEntityId"] == DOOR_ID
        assert lower["blockedTicks"] > 0
        assert upper["robotId"] == UPPER and upper["completionTick"] is not None
        check_recording(twin, timed_out, blocked, partial, {LOWER, UPPER}, layer, closed_scenario,
                        contact_robot=LOWER)
        canonical_contact = work / "contact-reimport.json"
        success(twin, "import", timed_out / "telemetry.json", canonical_contact)
        assert canonical_contact.read_bytes() == (timed_out / "telemetry.json").read_bytes()

        for name, invalid, diagnostic in (
            ("unknown-field", {**scenario, "typo": 0}, "unknown field typo"),
            ("noninteger-clock", {**scenario, "ticksPerSecond": 60.5}, "ticksPerSecond must be"),
            ("duplicate-robot", {**scenario, "robots": [scenario["robots"][0]] * 2}, "Duplicate fleet robot identity"),
        ):
            bad_mission = publish_json(work / f"{name}.json", invalid)
            missing = work / name
            result = run(fleet, model, bad_mission, missing, NAMESPACE)
            assert result.returncode == 1 and diagnostic in result.stderr, (result.stdout, result.stderr)
            assert not missing.exists()

        empty = work / "existing-empty"
        empty.mkdir()
        result = run(fleet, model, mission, empty, NAMESPACE)
        assert result.returncode == 1 and "fresh directory" in result.stderr and not list(empty.iterdir())

        race = work / "concurrent"
        with ThreadPoolExecutor(max_workers=2) as pool:
            attempts = [pool.submit(run, fleet, model, mission, race, NAMESPACE) for _ in range(2)]
            assert sorted(attempt.result().returncode for attempt in attempts) == [0, 1]
        assert set(file.name for file in race.iterdir()) == ARTIFACTS
        for name in ARTIFACTS:
            assert (race / name).read_bytes() == (completed / name).read_bytes()
        assert not list(work.glob(".bim-fleet-staging-*"))

    assert model.read_bytes() == source_bytes and mission.read_bytes() == raw_mission
    print("PASS shared-clock fleet tasks, deterministic contact telemetry, and native replay")


if __name__ == "__main__":
    main()
