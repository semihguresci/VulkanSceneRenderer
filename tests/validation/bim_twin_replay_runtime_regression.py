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
COLORS = {
    "plan": (255, 183, 64),
    "observed": (32, 231, 211),
    "pose": (165, 243, 91),
    "asset": (239, 114, 249),
}


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
            sequence=None):
    image = work / f"{name}.png"
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


def ink_counts(path):
    from PIL import Image

    def srgb(component):
        linear = component / 255
        encoded = 12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055
        return round(encoded * 255)

    with Image.open(path) as image:
        require(image.size == (WIDTH, HEIGHT), f"Unexpected size for {path}")
        # Sample the center of the viewport, clear of the controls and labels.
        colors = image.convert("RGB").crop((380, 140, 1080, 600)).getcolors(WIDTH * HEIGHT)
    # UI colors can pass through the swapchain's sRGB encode.
    return {name: sum(count for count, pixel in colors
                      if any(all(abs(a - b) <= 6 for a, b in zip(pixel, target))
                             for target in (rgb, tuple(map(srgb, rgb)))))
            for name, rgb in COLORS.items()}


def verify_ingested_status(args, work, model, recording):
    source = json_at(recording)
    robot = next(event["entityId"] for event in source["events"] if event["kind"] == "robot-pose")
    events = [event for event in source["events"] if event["entityId"] != UNRESOLVED_ID]
    events.extend({"kind": kind, "timestampNanoseconds": 2_200_000_000, "entityId": robot,
                   "source": "gateway:plant", "payload": payload}
                  for kind, payload in (
                      ("robot-health", {"state": "nominal", "batteryPercent": 78}),
                      ("task-status", {"taskId": "inspect", "state": "running", "progress": .75}),
                      ("task-status", {"taskId": "report", "state": "queued"}),
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
    image, state = capture(args, work, "replay-ingested", model, "deferred-raster", ingested, LATE)
    replay = state["bimTwinReplay"]
    require(replay["sourceMatched"] and replay["resolvedAssetCount"] == 2, replay)
    health = next(track for track in replay["tracks"] if track["kind"] == "robot-health")
    tasks = {track["taskId"]: track for track in replay["tracks"] if track["kind"] == "task-status"}
    require(health["state"] == "nominal" and health["batteryPercent"] == 78 and
            health["source"] == "gateway:plant" and not health["stale"], health)
    require(len(tasks) == 2 and tasks["inspect"]["state"] == "running" and tasks["inspect"]["progress"] == .75 and
            tasks["report"]["state"] == "queued" and tasks["report"]["progress"] is None, tasks)
    return {"image": image.name, "accepted": receipt["accepted"], "health": health, "tasks": tasks}


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
    ingested = verify_ingested_status(args, work, model, recording)
    require(sha256(model) == before, "Replay modified the source IFC")
    write_json(work / "results.json", {"version": 1, "passed": True, "sourceSha256": before,
                                      "rawViewportSha256": sha256(viewport), "captures": summary,
                                      "afterChangedSourceReload": detached, "ingested": ingested})
    print(f"PASS BIM digital twin playback, alignment, and provenance ({work})", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
