#!/usr/bin/env python3
"""Replay the recorded commands and sweep the actual Nav2-driven BIM trajectory."""

import argparse
import base64
import json
import math
from pathlib import Path
import subprocess
import tempfile

from bim_nav2_map import CASES, load_map, read_json, sha256, verify_sweeps, write_json
from bim_nav2_planner import Waypoint


GOAL_TOLERANCE = .14


def doorway_itinerary(goal):
    return Waypoint(.65, goal.y, 0.0), goal


def load_trace(path, scenario):
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    assert 1 < len(rows) <= scenario["ticks"] + 1, "Unexpected drive trace length"
    hz = scenario["ticksPerSecond"]
    for tick, row in enumerate(rows):
        assert row["schema"] == "container.bim-ros-bridge-trace" and row["version"] == 1, row
        assert row["tick"] == tick and row["timestampNanoseconds"] == tick * 1_000_000_000 // hz, row
        assert row["rosTimestampNanoseconds"] == row["timestampNanoseconds"] + 1_000_000_000, row
        assert row["obstruction"] == "none" and not row["blockingEntityId"] and row["contact"] is None, row
        assert (row["command"] is None) == (tick == 0), row
    assert any(row["command"]["linearMetersPerSecond"] > 0 for row in rows[1:]), "No real drive commands"
    return rows


def replay_commands(executable, source, scenario_path, camera_path, manifest, rows):
    requests = [{"op": "describe"}, {"op": "observe"}]
    requests.extend({"op": "step", "tick": row["tick"], **row["command"]} for row in rows[1:])
    requests.append({"op": "close"})
    data = "".join(json.dumps(request, allow_nan=False) + "\n" for request in requests)
    result = subprocess.run([executable, "--stream", source, manifest["sourceNamespace"], scenario_path,
                             "--camera", camera_path], input=data, capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stderr
    replies = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(replies) == len(requests) and all(reply["ok"] for reply in replies)
    assert replies[0]["sceneSource"] == manifest["sceneSource"] and replies[0]["openDoorIds"] == manifest["openDoorIds"]
    frames = 0
    for row, reply in zip(rows, replies[1:-1], strict=True):
        for key in ("tick", "timestampNanoseconds", "base", "odometry", "twist", "obstruction", "blockingEntityId"):
            assert row[key] == reply[key], (row["tick"], key)
        assert (row["camera"] is None) == (reply["camera"] is None), row["tick"]
        if row["camera"] is not None:
            expected, camera = row["camera"], reply["camera"]
            assert expected["sampleIndex"] == camera["sampleIndex"], row["tick"]
            assert expected["rgb8Sha256"] == sha256(base64.b64decode(camera["rgb8Base64"], validate=True)), row["tick"]
            assert expected["depth32fc1Sha256"] == sha256(base64.b64decode(camera["depth32fc1Base64"], validate=True)), row["tick"]
            frames += 1
    assert frames > 0, "No camera frames in the command replay"
    return frames


def verify_waypoint_visits(manifest, navigation, route):
    goal = next(entry for entry in manifest["waypoints"] if entry["id"] == "goal")
    itinerary = doorway_itinerary(Waypoint(*goal["positionMeters"], goal["yawRadians"]))
    recorded = navigation["goals"]
    assert len(recorded) == len(itinerary), "The Nav2 trace has the wrong number of waypoint results"
    errors, ticks = [], []
    next_tick = 0
    for waypoint, result in zip(itinerary, recorded, strict=True):
        requested = [waypoint.x, waypoint.y]
        assert result["requestedMeters"] == requested, ("Unexpected Nav2 waypoint", result, requested)
        actual = result["actualMeters"]
        assert len(actual) == 3 and abs(actual[2] - manifest["floorMeters"]) <= manifest["floorToleranceMeters"], result
        actual_xy = actual[:2]
        error = math.dist(requested, actual_xy)
        assert error <= GOAL_TOLERANCE and math.isclose(error, result["positionErrorMeters"],
                                                        rel_tol=0, abs_tol=1e-9), result
        candidates = (tick for tick in range(next_tick, len(route))
                      if math.dist(route[tick], requested) <= GOAL_TOLERANCE)
        tick = min(candidates, key=lambda index: math.dist(route[index], actual_xy), default=None)
        assert tick is not None and math.dist(route[tick], actual_xy) < .025, (
            "No matching committed sample for Nav2 waypoint in itinerary order", result)
        errors.append(error)
        ticks.append(tick)
        next_tick = tick + 1
    assert math.dist(route[-1], goal["positionMeters"]) <= GOAL_TOLERANCE, "Final pose left the goal tolerance"
    return errors, ticks


def verify_motion(collision, fixtures, case, manifest, grid, navigation, route):
    assert navigation["imageSha256"] == manifest["imageSha256"] and navigation["sceneSource"] == manifest["sceneSource"]
    assert math.dist(route[0], navigation["startMeters"][:2]) < .02
    goal_errors, goal_ticks = verify_waypoint_visits(manifest, navigation, route)
    assert all(grid.covers_segment(a, b) for a, b in zip(route, route[1:])), "Driven robot left mapped free cells"
    crossings = [a[1] - a[0] * (b[1] - a[1]) / (b[0] - a[0])
                 for a, b in zip(route, route[1:]) if a[0] <= 0 < b[0]]
    assert crossings and all(abs(y) < .35 for y in crossings), crossings
    with tempfile.TemporaryDirectory(prefix="bim-ros2-drive-sweeps-") as scratch:
        swept = verify_sweeps(collision, fixtures, Path(scratch), case, manifest, route)
    assert swept > 0, "The robot did not move"
    return {"sweptDriveSegments": swept, "drivenDistanceMeters": sum(math.dist(a, b) for a, b in zip(route, route[1:])),
            "doorwayCrossingsMeters": crossings, "goalErrorsMeters": goal_errors, "goalTraceTicks": goal_ticks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--collision", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True, help="Completed Jazzy demo output directory")
    parser.add_argument("--maps", type=Path, required=True, help="The same BIM map bundle used by the demo")
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    args.simulator, args.collision = args.simulator.resolve(strict=True), args.collision.resolve(strict=True)
    case = next(case for case in CASES if case.id == "doorway-open")
    manifest, grid = load_map(case, args.fixtures, args.maps / case.id)
    scenario_path = args.fixtures / "doorway-ros2-drive.json"
    camera_path = args.fixtures / "doorway-ros2-camera.json"
    rows = load_trace(args.evidence / "drive.jsonl", read_json(scenario_path))
    assert all(abs(row["base"]["position"][2] - manifest["floorMeters"]) <= manifest["floorToleranceMeters"]
               for row in rows), "Committed robot poses left the selected BIM floor"
    route = [row["base"]["position"][:2] for row in rows]
    metrics = verify_motion(args.collision, args.fixtures, case, manifest, grid,
                            read_json(args.evidence / "navigation.json"), route)
    frames = replay_commands(args.simulator, args.fixtures / case.source, scenario_path, camera_path, manifest, rows)
    metrics.update({"committedTicks": len(rows) - 1, "reproducedCameraFrames": frames})
    write_json(args.evidence / "drive-verification.json", metrics)
    print(json.dumps(metrics, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
