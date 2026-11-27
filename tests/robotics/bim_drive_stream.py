"""Exercise the headless BIM drive and camera stream against an authored IFC door."""

import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile


SCHEMA = "container.bim-drive-stream"
DOOR = "bim/7:doorway/guid/9:door-guid"
FEET = [-1.55, 0.05, 1.2]
FREQUENCY = 60
LAST_TICK = 180
CAMERA_INTERVAL = 6
INTRINSICS = {"width": 9, "height": 7, "fx": 6, "fy": 6, "cx": 4.5, "cy": 3.5,
              "nearMeters": 0.05, "farMeters": 3.5}


def step(tick, speed=1.0, yaw_rate=0.0):
    return {"op": "step", "tick": tick, "linearMetersPerSecond": speed, "angularRadiansPerSecond": yaw_rate}


def run_stream(executable, model, scenario, camera):
    requests = [{"op": "describe"}, {"op": "observe"}, step(2), {"op": "observe"},
                step(1, speed=0), step(1), {"op": "observe"}]
    for tick in range(2, LAST_TICK + 1):
        requests.append(step(tick))
        if tick == CAMERA_INTERVAL:
            requests.extend([{"op": "observe"}, {"op": "observe"}])
    requests.extend([step(LAST_TICK + 1), {"op": "observe"}, {"op": "close"}])

    request_lines = "".join(json.dumps(request, allow_nan=False) + "\n" for request in requests)
    command = [executable, "--stream", model, "doorway", scenario, "--camera", camera]
    result = subprocess.run(command, input=request_lines, capture_output=True, encoding="utf-8", timeout=40,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    assert result.returncode == 0, (scenario.name, result.returncode, result.stderr)
    replies = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(replies) == len(requests), (scenario.name, len(replies), len(requests), result.stderr)
    for request, reply in zip(requests, replies, strict=True):
        assert reply["schema"] == SCHEMA and reply["version"] == 1, reply
        assert reply["op"] == request["op"] and type(reply["ok"]) is bool, reply
    return replies


def check_description(description, model, door_open):
    source_data = model.read_bytes()
    assert description["ok"] and description["ticksPerSecond"] == FREQUENCY, description
    assert description["sourceNamespace"] == "doorway" and description["robotId"] == "robot/base", description
    assert description["openDoorIds"] == ([DOOR] if door_open else []), description
    assert description["sceneSource"]["layers"] == [
        {"sha256": hashlib.sha256(source_data).hexdigest(), "byteSize": len(source_data)}
    ], description
    assert description["sceneSource"]["metersPerUnscaledRendererUnit"] > 0, description
    assert description["capsule"] == {"radiusMeters": 0.25, "heightMeters": 1.6}, description
    camera = description["camera"]
    assert {key: camera[key] for key in INTRINSICS} == INTRINSICS, camera
    assert camera["baseFromCamera"] == [[0, -1, 0, 0], [0, 0, 1, 0], [-1, 0, 0, 0], [0, 0, 0.7, 1]], camera


def sample_data(reply):
    return {key: value for key, value in reply.items() if key not in ("schema", "version", "op", "ok")}


def transform_point(matrix, point):
    return [matrix[3][row] + sum(matrix[axis][row] * point[axis] for axis in range(3)) for row in range(3)]


def assert_vector_near(actual, expected):
    assert len(actual) == len(expected) and all(math.isclose(a, b, abs_tol=1e-6)
                                                for a, b in zip(actual, expected)), (actual, expected)


def check_sample(sample, tick):
    assert sample["ok"] and sample["tick"] == tick, (sample["ok"], sample.get("tick"), tick)
    assert sample["timestampNanoseconds"] == tick * 1_000_000_000 // FREQUENCY, sample
    assert math.isclose(sample["seconds"], tick / FREQUENCY, abs_tol=1e-15), sample
    transforms = sample["transforms"]
    map_from_odom = transforms["mapFromOdometry"]
    odom_from_base = transforms["odometryFromBase"]
    for matrix in (map_from_odom, odom_from_base):
        assert len(matrix) == 4 and all(len(column) == 4 for column in matrix), matrix
        assert [column[3] for column in matrix] == [0, 0, 0, 1], matrix
    assert_vector_near(map_from_odom[3][:3], FEET)
    assert_vector_near(odom_from_base[3][:3], sample["odometry"]["position"])
    world_base = transform_point(map_from_odom, odom_from_base[3][:3])
    assert_vector_near(world_base, sample["base"]["position"])
    world_forward = transform_point(map_from_odom, transform_point(odom_from_base, [1, 0, 0]))
    yaw = math.atan2(world_forward[1] - world_base[1], world_forward[0] - world_base[0])
    assert math.isclose(math.remainder(yaw - sample["base"]["yaw"], math.tau), 0, abs_tol=1e-6), sample
    assert all(math.isfinite(rate) for rate in sample["twist"].values()), sample


def decode_camera(sample):
    tick = sample["tick"]
    camera = sample["camera"]
    if tick % CAMERA_INTERVAL:
        assert camera is None, (tick, "unexpected image on an unscheduled tick")
        return None
    assert camera["sampleIndex"] == tick // CAMERA_INTERVAL, (tick, camera["sampleIndex"])
    rgb = base64.b64decode(camera["rgb8Base64"], validate=True)
    depth = base64.b64decode(camera["depth32fc1Base64"], validate=True)
    pixels = INTRINSICS["width"] * INTRINSICS["height"]
    assert len(rgb) == pixels * 3 and len(depth) == pixels * 4, (tick, len(rgb), len(depth))
    distances = struct.unpack(f"<{pixels}f", depth)
    for index, value in enumerate(distances):
        if math.isnan(value):
            assert rgb[3 * index:3 * index + 3] == b"\0\0\0", (tick, index, value)
        else:
            assert INTRINSICS["nearMeters"] - 1e-6 <= value <= INTRINSICS["farMeters"] + 1e-6, (tick, index, value)
    return distances


def check_run(replies, model, door_open):
    description, initial, skipped, after_skip, stationary, duplicate, after_duplicate = replies[:7]
    check_description(description, model, door_open)
    check_sample(initial, 0)
    assert initial == after_skip and not skipped["ok"] and "next tick" in skipped["error"], skipped
    assert not duplicate["ok"] and "next tick" in duplicate["error"], duplicate
    assert after_duplicate["ok"] and sample_data(stationary) == sample_data(after_duplicate), after_duplicate
    assert stationary["twist"] == {"linearMetersPerSecond": 0, "angularRadiansPerSecond": 0}, stationary
    assert_vector_near(stationary["base"]["position"], FEET)

    samples = [reply for reply in replies if reply["op"] == "step" and reply["ok"]]
    assert len(samples) == LAST_TICK, len(samples)
    initial_depth = decode_camera(initial)
    assert any(math.isfinite(value) for value in initial_depth), initial_depth
    if door_open:
        assert any(math.isnan(value) for value in initial_depth), initial_depth
    for tick, sample in enumerate(samples, 1):
        check_sample(sample, tick)
        decode_camera(sample)

    moving_camera = samples[CAMERA_INTERVAL - 1]["camera"]
    assert moving_camera["depth32fc1Base64"] != initial["camera"]["depth32fc1Base64"], (
        "depth did not advance with the camera pose")
    cached_frames = [reply for reply in replies if reply["op"] == "observe" and reply["tick"] == CAMERA_INTERVAL]
    assert len(cached_frames) == 2 and all(frame["ok"] and sample_data(frame) == sample_data(samples[CAMERA_INTERVAL - 1])
                                            for frame in cached_frames), "same-tick RGB-D was not cached"
    beyond_limit, final_observation, close = replies[-3:]
    assert not beyond_limit["ok"] and "tick limit" in beyond_limit["error"], beyond_limit
    assert final_observation["ok"] and sample_data(final_observation) == sample_data(samples[-1]), final_observation
    assert close["ok"], close

    assert samples[CAMERA_INTERVAL - 1]["twist"]["linearMetersPerSecond"] > 0, "robot did not start moving"
    if door_open:
        assert samples[-1]["base"]["position"][0] > 1.0, "robot did not drive through the open door"
        assert all(sample["obstruction"] == "none" for sample in samples), "open door reported a collision"
    else:
        assert any(sample["obstruction"] == "blocked" and sample["blockingEntityId"] == DOOR for sample in samples), (
            "closed door did not block the robot")
        radius = description["capsule"]["radiusMeters"]
        assert max(sample["base"]["position"][0] for sample in samples) + radius < 0, "capsule crossed the closed door"
        assert samples[-1]["twist"] == {"linearMetersPerSecond": 0, "angularRadiansPerSecond": 0}, samples[-1]
    return initial_depth[len(initial_depth) // 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    model = (args.fixtures / "doorway.ifc").resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="bim-drive-stream-") as temporary:
        directory = Path(temporary)
        camera = directory / "camera.json"
        camera.write_text(json.dumps({"schema": "container.sensor-camera", "version": 1, "intrinsics": INTRINSICS,
            "baseFromCamera": {"xyz": [0, 0, 0.7]}, "timing": {"ticksPerSecond": FREQUENCY,
                "framesPerSecondMilliHz": 10_000, "firstTick": 0, "lastTick": LAST_TICK}}), encoding="utf-8")
        center = {}
        for door_open in (True, False):
            scenario = directory / ("open.json" if door_open else "closed.json")
            scenario.write_text(json.dumps({"version": 1, "ticks": LAST_TICK, "initialFeet": FEET,
                "openDoorIds": [DOOR] if door_open else []}), encoding="utf-8")
            center[door_open] = check_run(run_stream(executable, model, scenario, camera), model, door_open)
        assert math.isnan(center[True]), ("open door obstructed the center camera ray", center[True])
        assert math.isclose(center[False], 1.52, abs_tol=0.005), ("wrong central closed-door depth", center[False])
    print("Verified committed drive ticks, camera payloads, and the open and closed IFC door")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
