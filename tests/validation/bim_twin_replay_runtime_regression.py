"""Replay a real simulated trajectory against its BIM source in Vulkan."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from display_capture_test_support import validation_log_error


WIDTH, HEIGHT = 1280, 800
IMPORT_SCALE = 2.5
DOOR_ID = "bim/7:doorway/guid/9:door-guid"
PROP_ID = "bim/7:doorway/guid/9:prop-guid"
UNRESOLVED_ID = "external-fixture/unknown-asset"
EARLY = 500_000_000
LATE = 2_250_000_000
OPERATOR_EVENT = 400_000_000
ALARM_CLEAR = 2_500_000_000
CLEARED = 2_750_000_000
VIEWPORT_REGION = (380, 140, 1080, 600)
COLORS = {
    "plan": (255, 183, 64),
    "observed": (32, 231, 211),
    "pose": (165, 243, 91),
    "asset": (239, 114, 249),
}
OPERATOR_COLORS = {
    "warning": (255, 133, 78), "critical": (255, 76, 103),
    "frustum": (108, 160, 255), "occupancy": (179, 151, 255),
    "collision": (255, 57, 49),
}
COLLISION = {"otherEntityId": DOOR_ID, "positionMeters": [-.03, .42, 1.72], "normal": [-1, 0, 0]}
STALE_CONTACT_ROBOT = "fixture/inspection-robot"
STALE_CONTACT = {"otherEntityId": DOOR_ID, "positionMeters": [-.03, -.47, 1.58]}
WARNING = {"alarmId": "route-deviation", "severity": "warning", "message": "Deviation from plan",
           "active": True, "position": [.65, -.65, 1.68]}
CRITICAL = {"alarmId": "door-obstruction", "severity": "critical", "message": "Doorway obstruction",
            "active": True, "position": [1.08, .92, 2.15]}
FRUSTUM = {
    "width": 160, "height": 120, "fx": 105.0, "fy": 95.0, "cx": 63.5, "cy": 43.25,
    "nearMeters": .35, "farMeters": 4.5,
    "worldFromCamera": [.6, -.8, 0, 0, 0, 0, 1, 0, -.8, -.6, 0, 0, .48, -1.15, 1.86, 1],
}
GRID = {"width": 4, "height": 3, "origin": [-1.35, -1.75, 1.31], "yaw": math.pi / 6,
        "resolutionMeters": .32, "cells": ".#?." ".#.." "#?.."}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def json_at(path):
    return json.loads(path.read_bytes())


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(executable, args, log, env=None):
    with log.open("w", encoding="utf-8") as errors:
        result = subprocess.run([str(executable), *map(str, args)], cwd=executable.parent, env=env,
                                stdout=subprocess.PIPE, stderr=errors, timeout=120,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return result.returncode, result.stdout.decode("utf-8", errors="replace")


def make_recording(args, work, model):
    scenario = json_at(args.fixtures / "doorway-drive.json")
    scenario["driveSensors"] = {"samplesPerSecondMilliHz": 20_000}
    scenario["lidar"] = {"scansPerSecondMilliHz": 10_000, "azimuthSamples": 16, "maxRangeMeters": 10,
                         "mount": {"xyz": [0, 0, .7]}}
    scenario_path, simulation = work / "scenario.json", work / "simulation.json"
    write_json(scenario_path, scenario)
    code, _ = run(args.simulator, [model, simulation, "doorway", scenario_path], work / "simulate.log")
    require(code == 0, f"Simulation failed: {work / 'simulate.log'}")
    report = json_at(simulation)
    robot = report["robotId"]
    plan = {"points": [[-1.5, .18, 1.2], [0, .18, 1.2], [1.5, .18, 1.2]]}

    def event(tick_time, entity, kind, payload):
        return {"timestampNanoseconds": tick_time, "entityId": entity, "source": "test:doorway-status",
                "kind": kind, "payload": payload}

    supplement = work / "supplement.json"
    write_json(supplement, {
        "schema": "container.bim-twin-telemetry", "version": 1,
        "sourceNamespace": report["sourceNamespace"], "sceneSource": report["sceneSource"],
        "clockId": "simulation-fixed-step", "staleAfterNanoseconds": 2_000_000_000,
        # Intake ordering deliberately differs from timestamp order.
        "events": [
            event(1_800_000_000, robot, "planned-path", plan),
            event(1_200_000_000, DOOR_ID, "bim-asset-state", {"state": "open"}),
            event(0, DOOR_ID, "bim-asset-state", {"state": "closed"}),
            event(2_000_000_000, UNRESOLVED_ID, "bim-asset-state", {"state": "inspection-pending"}),
            event(0, PROP_ID, "bim-asset-state", {"state": "inspected"}),
            event(0, robot, "planned-path", plan),
        ],
    })
    recording = work / "recording.json"
    code, _ = run(args.tool, ["record", simulation, recording, "--supplement", supplement], work / "record.log")
    require(code == 0, f"Recording failed: {work / 'record.log'}")
    manifest = work / "semantic-scene.json"
    code, _ = run(args.scene_exporter, [model, manifest, "doorway"], work / "semantic.log")
    require(code == 0, f"Semantic export failed: {work / 'semantic.log'}")
    return recording, report, json_at(manifest)


def capture(args, work, name, model, technique, recording=None, cursor=None, raw=False, failure=None,
            sequence=None, extension="png"):
    image = work / f"{name}.{extension}"
    command = [
        "--model", model, "--import-scale", IMPORT_SCALE, "--hidden", "--validation",
        "--width", WIDTH, "--height", HEIGHT, "--msaa", 1, "--no-taa", "--no-bloom", "--no-ddgi",
        "--no-ray-query", "--render-technique", technique, "--display-mode", "lit",
        "--camera-position", 8, 12, 18, "--camera-target", 0, 3.7, 0, "--camera-fov", 34,
        "--fixed-dt", ".016666667", "--screenshot", image,
    ]
    if sequence is None:
        command.extend(("--warmup-frames", 5, "--capture-frame", 6))
    else:
        command.extend(("--capture-sequence", sequence))
    if recording is not None:
        command.extend(("--bim-twin-replay", recording, "--bim-twin-time-ns", cursor))
    if raw:
        command.append("--screenshot-viewport")
    log = work / f"{name}.log"
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(work), VK_LOADER_DEBUG="layer")
    code, output = run(args.renderer, command, log, env)
    log_text = log.read_text(encoding="utf-8", errors="replace") + output
    require(not validation_log_error(log_text), f"Vulkan validation error: {log}")
    if failure:
        require(code != 0 and failure.lower() in log_text.lower() and not image.exists(),
                f"Expected renderer rejection for {name}: {log}")
        return None, None
    require(code == 0 and image.is_file(), f"Renderer capture failed: {log}")
    require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in log_text,
            f"Capture used no Vulkan validation layer: {log}")
    metadata = json_at(image.with_suffix(f".{extension}.capture.json"))
    require((metadata["width"], metadata["height"], metadata["includesUi"]) ==
            (WIDTH, HEIGHT, extension == "png" and not raw), metadata)
    if extension == "exr":
        require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB" and
                not metadata["toneMappingApplied"] and not metadata["exposureApplied"], metadata)
    state = json_at(image.with_suffix(".telemetry.json"))
    require(state["technique"] == technique, f"Unexpected capture technique: {name}")
    return image, state


def metric_to_renderer(point, meters_per_unscaled_unit):
    x, y, z = point
    scale = IMPORT_SCALE / meters_per_unscaled_unit
    return [-y * scale, z * scale, -x * scale]


def close_point(actual, expected, label):
    require(len(actual) == len(expected) and all(math.isclose(a, b, abs_tol=.0002, rel_tol=0)
            for a, b in zip(actual, expected)), f"{label}: {actual} != {expected}")


def check_state(state, simulation, semantic_scene, timestamp):
    replay = state["bimTwinReplay"]
    require(replay["sourceMatched"] and not replay["playing"] and replay["timestampNanoseconds"] == timestamp, replay)
    tracks = {(row["kind"], row["entityId"]): row for row in replay["tracks"]}
    robot = tracks["robot-pose", simulation["robotId"]]
    rates = simulation["ticksPerSecond"]
    tick = timestamp * rates // 1_000_000_000
    base = simulation["history"][tick]["base"]
    meters = simulation["sceneSource"]["metersPerUnscaledRendererUnit"]
    close_point(robot["positionMeters"], base["position"], "Robot pose")
    close_point(robot["rendererPosition"], metric_to_renderer(base["position"], meters), "Renderer pose")
    require(robot["source"] == "simulation:base" and not robot["stale"], robot)
    require(robot["observedPointCount"] == tick + 1, robot)
    require(math.isclose(robot["distanceToPlanMeters"], .18, abs_tol=.001), robot)
    for sensor in ("imu", "lidar"):
        status = tracks["sensor-status", f'{simulation["robotId"]}/{sensor}']
        require(not status["stale"] and status["source"] == f"simulation:{sensor}", status)
    asset = tracks["bim-asset-state", DOOR_ID]
    entity = next(row for row in semantic_scene["entities"] if row["id"] == DOOR_ID and row["guid"] == "door-guid")
    placement = entity["roboticsWorldFromLocal"]
    # The IFC door's profile is centered, and its extrusion is 2,060 mm.
    center = [placement[3][axis] + 1030 * placement[2][axis] for axis in range(3)]
    close_point(asset["rendererPosition"], metric_to_renderer(center, meters), "Door marker")
    require(asset["state"] == ("closed" if timestamp == EARLY else "open"), asset)
    missing = timestamp == LATE
    require(replay["resolvedAssetCount"] == 2 and replay["unresolvedAssetCount"] == int(missing), replay)
    require(tracks["bim-asset-state", PROP_ID]["stale"] == missing, "Prop state freshness did not follow replay time")
    if missing:
        require(tracks["bim-asset-state", UNRESOLVED_ID]["rendererPosition"] is None, replay)
    return {"timestampNanoseconds": timestamp, "robot": robot, "asset": asset}


def srgb(component):
    linear = component / 255
    encoded = 12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055
    return round(encoded * 255)


def ink_counts(path):
    from PIL import Image
    with Image.open(path) as image:
        require(image.size == (WIDTH, HEIGHT), f"Unexpected size for {path}")
        # Sample the center of the viewport, clear of the controls and labels.
        colors = image.convert("RGB").crop(VIEWPORT_REGION).getcolors(WIDTH * HEIGHT)
    # UI colors can pass through the swapchain's sRGB encode.
    return {name: sum(count for count, pixel in colors
                      if any(all(abs(a - b) <= 6 for a, b in zip(pixel, target))
                             for target in (rgb, tuple(map(srgb, rgb)))))
            for name, rgb in COLORS.items()}


def operator_masks(path):
    import numpy as np
    from PIL import Image

    with Image.open(path) as image:
        require(image.size == (WIDTH, HEIGHT), f"Unexpected size for {path}")
        pixels = np.asarray(image.convert("RGB").crop(VIEWPORT_REGION), dtype=np.int16)
    return {name: np.logical_or(np.all(np.abs(pixels - rgb) <= 6, axis=2),
                                np.all(np.abs(pixels - tuple(map(srgb, rgb))) <= 6, axis=2))
            for name, rgb in OPERATOR_COLORS.items()}


def clip_projected_pixel(state, renderer_position):
    matrix = state["camera"]["unjitteredViewProjColumns"]
    clip = [sum(column[row] * component for column, component in zip(matrix, (*renderer_position, 1), strict=True))
            for row in range(4)]
    require(clip[3] > 0 and 0 <= clip[2] <= clip[3], f"Operator fixture point is behind the editor camera: {clip}")
    return (WIDTH * (.5 + .5 * clip[0] / clip[3]), HEIGHT * (.5 - .5 * clip[1] / clip[3]))


def nearby_ink(mask, point):
    x, y = map(round, point)
    left, top, right, bottom = VIEWPORT_REGION
    require(left + 9 <= x < right - 9 and top + 9 <= y < bottom - 9,
            f"Operator fixture point left the unobstructed capture region: {point}")
    return int(mask[y - top - 8:y - top + 9, x - left - 8:x - left + 9].sum())


def expected_frustum_corners(meters_per_unscaled_unit):
    camera = FRUSTUM["worldFromCamera"]
    corners = []
    for depth in (FRUSTUM["nearMeters"], FRUSTUM["farMeters"]):
        for u, v in ((0, 0), (FRUSTUM["width"], 0), (FRUSTUM["width"], FRUSTUM["height"]),
                     (0, FRUSTUM["height"])):
            ray = ((u - FRUSTUM["cx"]) * depth / FRUSTUM["fx"],
                   (FRUSTUM["cy"] - v) * depth / FRUSTUM["fy"], -depth, 1)
            point = [sum(camera[column * 4 + axis] * value for column, value in enumerate(ray))
                     for axis in range(3)]
            corners.append(metric_to_renderer(point, meters_per_unscaled_unit))
    return corners


def grid_point(col, row, meters_per_unscaled_unit):
    x, y, z = GRID["origin"]
    theta, step = GRID["yaw"], GRID["resolutionMeters"]
    return metric_to_renderer((x + step * (col * math.cos(theta) - row * math.sin(theta)),
                               y + step * (col * math.sin(theta) + row * math.cos(theta)), z),
                              meters_per_unscaled_unit)


def check_collision_capture(path, state, robot, meters, cursor, mask):
    poses = {track["entityId"]: track for track in state["bimTwinReplay"]["tracks"] if track["kind"] == "robot-pose"}
    measurements = {}
    for entity, payload, observed_at in ((robot, COLLISION, LATE),
                                         (STALE_CONTACT_ROBOT, STALE_CONTACT, OPERATOR_EVENT)):
        row = poses[entity]
        cleared = entity == robot and cursor == CLEARED
        stale = entity == STALE_CONTACT_ROBOT and cursor == CLEARED
        require(row["stale"] == stale and row["eventTimestampNanoseconds"] == (CLEARED if cleared else observed_at), row)
        if cleared:
            require("contact" not in row, f"Complete pose did not clear its contact: {row}")
            measurements[entity] = {"cleared": True}
            continue

        contact = row.get("contact")
        require(contact is not None, f"Missing sampled collision contact: {row}")
        require({key: value for key, value in contact.items() if key != "rendererPosition"} == payload, contact)
        position = metric_to_renderer(payload["positionMeters"], meters)
        close_point(contact["rendererPosition"], position, f"Collision contact for {entity}")
        if not stale:
            require(nearby_ink(mask, clip_projected_pixel(state, position)) >= 5,
                    f"Missing collision pixels near the projected contact for {entity}: {path}")
        measurements[entity] = {"contact": contact, "stale": stale}
    return measurements


def check_operator_capture(path, state, source, cursor):
    replay = state["bimTwinReplay"]
    require(replay["sourceMatched"] and not replay["playing"] and replay["timestampNanoseconds"] == cursor, replay)
    meters = source["sceneSource"]["metersPerUnscaledRendererUnit"]
    alarms = {track["alarmId"]: track for track in replay["tracks"] if track["kind"] == "operator-alarm"}
    frustum = next(track for track in replay["tracks"] if track["kind"] == "sensor-frustum")
    grid = next(track for track in replay["tracks"] if track["kind"] == "occupancy-grid")
    expired = cursor == CLEARED
    require(set(alarms) == {WARNING["alarmId"], CRITICAL["alarmId"]}, alarms)
    mask = operator_masks(path)
    counts = {name: int(ink.sum()) for name, ink in mask.items()}
    robot = next(event["entityId"] for event in source["events"] if event["kind"] == "robot-pose")
    collisions = check_collision_capture(path, state, robot, meters, cursor, mask["collision"])
    for name, payload in (("warning", WARNING), ("critical", CRITICAL)):
        row = alarms[payload["alarmId"]]
        cleared = expired and name == "critical"
        expected = dict(payload, active=not cleared)
        require(all(row[field] == value for field, value in expected.items()), row)
        require(row["source"] == "gateway:safety" and row["stale"] == (expired and name == "warning") and
                row["eventTimestampNanoseconds"] == (ALARM_CLEAR if cleared else OPERATOR_EVENT), row)
        position = metric_to_renderer(payload["position"], meters)
        close_point(row["rendererPosition"], position, f"{name} alarm")
        if not expired:
            require(nearby_ink(mask[name], clip_projected_pixel(state, position)) >= 5,
                    f"Missing {name} around its projected world position: {path}")

    require(frustum["source"] == "gateway:camera" and frustum["stale"] == expired and
            frustum["eventTimestampNanoseconds"] == OPERATOR_EVENT, frustum)
    corners = expected_frustum_corners(meters)
    require(len(frustum["rendererCorners"]) == len(corners), frustum)
    for index, (actual, expected) in enumerate(zip(frustum["rendererCorners"], corners, strict=True)):
        close_point(actual, expected, f"Frustum corner {index}")

    require(grid["source"] == "gateway:navigation" and grid["stale"] == expired and
            grid["eventTimestampNanoseconds"] == OPERATOR_EVENT, grid)
    require(all(grid[key] == GRID[key] for key in ("width", "height", "origin", "yaw", "resolutionMeters")), grid)
    close_point(grid["rendererOrigin"], grid_point(0, 0, meters), "Occupancy grid origin")
    require(grid["occupiedCells"] == GRID["cells"].count("#"), grid)
    if expired:
        require(not any(counts.values()), f"Cleared/expired overlays remain visible: {path}: {counts}")
    else:
        # One far-plane corner falls above the image. The visible portion
        # of the connecting edge must survive clipping at the frame border.
        require(clip_projected_pixel(state, corners[4])[1] < 0, "Frustum no longer exercises clipping")
        edge = [.75 * a + .25 * b for a, b in zip(corners[0], corners[4], strict=True)]
        require(nearby_ink(mask["frustum"], clip_projected_pixel(state, edge)) >= 5,
                f"Missing visible part of a frustum edge with an off-screen end: {path}")
        for col in (.5, 1.5, 2.5):
            count = nearby_ink(mask["occupancy"], clip_projected_pixel(state, grid_point(col, 0, meters)))
            require(count >= 5 if col == 1.5 else count == 0,
                    f"Occupancy outline does not match occupied/free/unknown cells: {path}: {col}: {count}")
        require(min(counts.values()) > 10, f"Missing active operator overlays: {path}: {counts}")
    trajectories = ink_counts(path)
    require(trajectories["plan"] > 10 and trajectories["observed"] > 10 and trajectories["pose"] > 10,
            f"Robot paths or pose disappeared with operator overlays: {path}: {trajectories}")
    return {"image": path.name, "cursorNanoseconds": cursor, "overlayPixels": counts,
            "trajectoryPixels": trajectories, "alarms": alarms, "frustum": frustum, "occupancy": grid,
            "collisions": collisions}, mask


def verify_raw_exr(plain, replay):
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    import cv2
    import numpy as np

    def decode(path):
        pixels = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(pixels is not None and pixels.shape == (HEIGHT, WIDTH, 3) and pixels.dtype == np.float32 and
                np.isfinite(pixels).all() and pixels.max() > .01, f"Invalid scene-linear capture: {path}")
        return pixels

    expected, actual = decode(plain), decode(replay)
    np.testing.assert_array_equal(actual, expected, err_msg=f"Operator overlays entered raw scene-linear render: {replay}")
    return hashlib.sha256(actual.tobytes()).hexdigest()


def verify_operator_rendering(args, work, model, ingested, source, deferred_baseline, deferred_active):
    import numpy as np

    results = {}
    for technique in ("deferred-raster", "forward-raster"):
        tag = technique.split("-")[0]
        active, state = deferred_active if tag == "deferred" else capture(
            args, work, "operator-forward-active", model, technique, ingested, LATE)
        active_result, masks = check_operator_capture(active, state, source, LATE)
        repeat, repeat_state = capture(args, work, f"operator-{tag}-repeat", model, technique, ingested, LATE)
        require(repeat_state["bimTwinReplay"] == state["bimTwinReplay"], f"Repeated replay state changed: {repeat}")
        repeat_masks = operator_masks(repeat)
        for name, mask in masks.items():
            np.testing.assert_array_equal(mask, repeat_masks[name], err_msg=f"Operator mask varied at same time: {name}")

        cleared, cleared_state = capture(args, work, f"operator-{tag}-cleared", model, technique, ingested, CLEARED)
        cleared_result, _ = check_operator_capture(cleared, cleared_state, source, CLEARED)
        baseline = deferred_baseline if tag == "deferred" else capture(
            args, work, "operator-forward-baseline-viewport", model, technique, raw=True)[0]
        replay_raw, raw_state = capture(args, work, f"operator-{tag}-viewport", model, technique, ingested, LATE,
                                        raw=True)
        require(sha256(replay_raw) == sha256(baseline), f"Operator overlays entered raw viewport capture: {replay_raw}")
        plain_exr, _ = capture(args, work, f"operator-{tag}-baseline-linear", model, technique, extension="exr")
        replay_exr, linear_state = capture(args, work, f"operator-{tag}-linear", model, technique, ingested, LATE,
                                           extension="exr")
        for raw_capture_state in (raw_state, linear_state):
            require(raw_capture_state["bimTwinReplay"]["tracks"] == state["bimTwinReplay"]["tracks"],
                    "Clean capture lost sampled BIM telemetry")
        results[technique] = {"active": active_result, "cleared": cleared_result,
                              "sameTimestampPngBytes": sha256(active) == sha256(repeat),
                              "sameTimestampOperatorMasks": True, "rawViewportSha256": sha256(replay_raw),
                              "rawLinearPixelsSha256": verify_raw_exr(plain_exr, replay_exr)}
    return results


def verify_ingested_status(args, work, model, recording, deferred_baseline):
    source = json_at(recording)
    robot = next(event["entityId"] for event in source["events"] if event["kind"] == "robot-pose")
    events = [event for event in source["events"] if event["entityId"] != UNRESOLVED_ID]
    active_pose = next(event for event in events if event["kind"] == "robot-pose" and event["entityId"] == robot and
                       event["timestampNanoseconds"] == LATE)
    events[events.index(active_pose)] = dict(active_pose, payload=dict(active_pose["payload"], contact=COLLISION))
    events.extend({"kind": kind, "timestampNanoseconds": 2_200_000_000, "entityId": robot,
                   "source": "gateway:plant", "payload": payload}
                  for kind, payload in (
                      ("robot-health", {"state": "nominal", "batteryPercent": 78}),
                      ("task-status", {"taskId": "inspect", "state": "running", "progress": .75}),
                      ("task-status", {"taskId": "report", "state": "queued"}),
                  ))
    events.extend({"kind": kind, "timestampNanoseconds": time, "entityId": entity, "source": producer,
                   "payload": payload}
                  for kind, time, entity, producer, payload in (
                      ("operator-alarm", OPERATOR_EVENT, robot, "gateway:safety", WARNING),
                      ("operator-alarm", OPERATOR_EVENT, robot, "gateway:safety", CRITICAL),
                      ("operator-alarm", ALARM_CLEAR, robot, "gateway:safety", dict(CRITICAL, active=False)),
                      ("sensor-frustum", OPERATOR_EVENT, f"{robot}/camera", "gateway:camera", FRUSTUM),
                      ("occupancy-grid", OPERATOR_EVENT, "doorway/local-map", "gateway:navigation", GRID),
                      ("robot-pose", OPERATOR_EVENT, STALE_CONTACT_ROBOT, "gateway:odometry",
                       {"position": [-.33, -.47, 1.2], "yaw": 0, "contact": STALE_CONTACT}),
                  ))
    feed = work / "external-telemetry.jsonl"
    with feed.open("w", encoding="utf-8") as stream:
        for event in sorted(events, key=lambda row: row["timestampNanoseconds"]):
            row = {"schema": "container.bim-twin-event", "version": 1, "clockId": source["clockId"],
                   "receivedTimestampNanoseconds": event["timestampNanoseconds"], "event": event}
            if event["entityId"] == DOOR_ID:
                row["event"] = dict(event, entityId="door-guid")
                row["assetReference"] = "ifc-guid"
            stream.write(json.dumps(row) + "\n")
    ingested = work / "ingested.json"
    code, output = run(args.tool, ["ingest", model, feed, ingested, source["sourceNamespace"], source["clockId"]],
                       work / "ingest.log")
    require(code == 0, f"Ingestion failed: {work / 'ingest.log'}")
    receipt = json.loads(output)
    require(receipt["accepted"] == len(events) and receipt["rejected"] == 0, receipt)
    require(json_at(ingested)["version"] == 4, "Collision contacts did not enter a version-4 replay")
    image, state = capture(args, work, "replay-ingested", model, "deferred-raster", ingested, LATE)
    replay = state["bimTwinReplay"]
    require(replay["sourceMatched"] and replay["resolvedAssetCount"] == 2, replay)
    health = next(track for track in replay["tracks"] if track["kind"] == "robot-health")
    tasks = {track["taskId"]: track for track in replay["tracks"] if track["kind"] == "task-status"}
    require(health["state"] == "nominal" and health["batteryPercent"] == 78 and
            health["source"] == "gateway:plant" and not health["stale"], health)
    require(len(tasks) == 2 and tasks["inspect"]["state"] == "running" and tasks["inspect"]["progress"] == .75 and
            tasks["report"]["state"] == "queued" and tasks["report"]["progress"] is None, tasks)
    rendering = verify_operator_rendering(args, work, model, ingested, source, deferred_baseline, (image, state))
    return {"image": image.name, "accepted": receipt["accepted"], "health": health, "tasks": tasks,
            "operatorOverlays": rendering}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("tool", "simulator", "scene-exporter", "renderer", "fixtures", "output"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_BIM_TWIN") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_BIM_TWIN=1 for live digital twin playback")
        return 77
    for key, value in vars(args).items():
        setattr(args, key, value.resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    (work / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="ascii")
    model = args.fixtures / "doorway.ifc"
    before = sha256(model)
    recording, simulation, scene = make_recording(args, work, model)

    baseline, _ = capture(args, work, "baseline-viewport", model, "deferred-raster", raw=True)
    viewport, _ = capture(args, work, "replay-viewport", model, "deferred-raster", recording, LATE, raw=True)
    require(sha256(baseline) == sha256(viewport), "Replay changed the unannotated BIM viewport pixels")
    summary = []
    for tag, technique, timestamp in (("early", "deferred-raster", EARLY),
                                      ("late", "deferred-raster", LATE),
                                      ("forward", "forward-raster", LATE)):
        image, state = capture(args, work, f"replay-{tag}", model, technique, recording, timestamp)
        measurements = check_state(state, simulation, scene, timestamp)
        colors = ink_counts(image)
        require(colors["plan"] > 10 and colors["observed"] > 10 and
                colors["pose"] > 10 and colors["asset"] > 10, f"Replay overlays missing: {tag}: {colors}")
        summary.append({"image": image.name, "technique": technique, "overlayPixels": colors, **measurements})
    changed = work / "changed-source.ifc"
    changed.write_bytes(model.read_bytes() + b"\n")
    capture(args, work, "wrong-source", changed, "deferred-raster", recording, EARLY,
            failure="BIM twin replay")
    reload_sequence = work / "reload.json"
    write_json(reload_sequence, {"schemaVersion": 1, "frames": 8, "sampleFrames": [8],
                                 "events": [{"frame": 3, "reload": str(changed)}]})
    _, reloaded = capture(args, work, "changed-during-replay", model, "deferred-raster", recording, EARLY,
                          sequence=reload_sequence)
    detached = reloaded["bimTwinReplay"]
    require(not detached["sourceMatched"] and not detached["playing"] and not detached["tracks"] and
            detached["timestampNanoseconds"] == EARLY, detached)
    wrong_scale = work / "wrong-scale.json"
    bad_recording = json_at(recording)
    bad_recording["sceneSource"]["metersPerUnscaledRendererUnit"] *= 2
    write_json(wrong_scale, bad_recording)
    capture(args, work, "wrong-scale", model, "deferred-raster", wrong_scale, EARLY,
            failure="metre scale differs")
    ingested = verify_ingested_status(args, work, model, recording, baseline)
    require(sha256(model) == before, "Replay modified the source IFC")
    write_json(work / "results.json", {"version": 1, "passed": True, "sourceSha256": before,
                                      "rawViewportSha256": sha256(viewport), "captures": summary,
                                      "afterChangedSourceReload": detached, "ingested": ingested})
    print(f"PASS BIM digital twin playback, alignment, and provenance ({work})", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
