"""Exercise the native BIM twin CLI without a renderer or physics device."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


NS = 1_000_000_000
SCENE_SOURCE = {
    "layers": [{"sha256": "a" * 64, "byteSize": 250}, {"sha256": "b" * 64, "byteSize": 140}],
    "metersPerUnscaledRendererUnit": 1.0,
}


def write_json(path, value):
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")
    return path


def event(kind, entity, when, source, payload):
    return {"kind": kind, "entityId": entity, "timestampNanoseconds": when, "source": source, "payload": payload}


def log(events, *, clock="simulation-fixed-step", scene_source=SCENE_SOURCE):
    return {"schema": "container.bim-twin-telemetry", "version": 1, "sourceNamespace": "native IFC",
            "sceneSource": scene_source, "clockId": clock, "staleAfterNanoseconds": 2 * NS, "events": events}


def simulation():
    def time(tick):
        return {"tick": tick, "seconds": tick / 2}

    def pose(tick):
        return {"position": [2 * tick, 0, 0], "yaw": .2 * tick}

    def imu(tick):
        return {**time(tick), "specificForce": [0, 0, 9.81], "angularVelocity": [0, 0, 0],
                "encoderCounts": {"left": 0, "right": 0}, "encoderRadians": {"left": 0, "right": 0},
                "odometry": pose(tick)}

    mount = {"xyz": [0, 0, .5], "quaternionXyzw": [0, 0, 0, 1]}
    scan = {**time(1), "worldFromSensor": mount,
            "returns": [{"status": "valid", "entityId": "wall", "rangeMeters": 2.5}]}
    return {"schema": "container.bim-robotics-simulation", "version": 1,
            "coordinateConvention": "ROS REP-103", "ticksPerSecond": 2, "sourceNamespace": "native IFC",
            "robotId": "rover", "sceneSource": SCENE_SOURCE,
            "history": [{**time(tick), "base": pose(tick)} for tick in range(3)],
            "driveSensors": {"samples": [imu(0), imu(2)], "samplesPerSecondMilliHz": 1000, "firstTick": 0,
                             "imuFrame": "base"},
            "lidar": {"scans": [scan], "scansPerSecondMilliHz": 1000, "firstTick": 1,
                      "azimuthSamples": 1, "raysPerScan": 1, "elevationAngles": [0],
                      "minRangeMeters": .1, "maxRangeMeters": 10, "baseFromSensor": mount}}


def run(tool, *args, diagnostic=None):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True, timeout=15)
    if diagnostic is None:
        assert result.returncode == 0, (args, result.stderr.decode(errors="replace"))
    else:
        assert result.returncode != 0 and diagnostic in result.stderr.decode(errors="replace"), (args, result.stderr)
    return result.stdout


def find_track(sample, entity_id, kind):
    return next(track for track in sample["tracks"]
                if (track["event"]["entityId"], track["event"]["kind"]) == (entity_id, kind))


def verify_recording(tool, work):
    report = write_json(work / "simulation.json", simulation())
    plan = event("planned-path", "rover", 0, "operator:plan", {"points": [[0, 1, 0], [4, 1, 0]]})
    opened = event("bim-asset-state", "door-guid", NS // 2, "operator:asset", {"state": "open"})
    supplement = write_json(work / "supplement.json", log([opened, plan]))
    output, repeated = work / "recorded.json", work / "repeated.json"
    authored = report.read_bytes(), supplement.read_bytes()
    run(tool, "record", report, output, "--supplement", supplement)
    run(tool, "record", report, repeated, "--supplement", supplement)
    assert output.read_bytes() == repeated.read_bytes()
    assert (report.read_bytes(), supplement.read_bytes()) == authored
    data = json.loads(output.read_bytes())
    assert data["version"] == 1
    assert data["sceneSource"] == SCENE_SOURCE and data["clockId"] == "simulation-fixed-step"
    assert len(data["events"]) == 8
    assert [row["timestampNanoseconds"] for row in data["events"]] == sorted(
        row["timestampNanoseconds"] for row in data["events"])
    provenance = {(row["entityId"], row["source"]) for row in data["events"]}
    assert {("rover", "simulation:base"), ("rover/imu", "simulation:imu"),
            ("rover/lidar", "simulation:lidar"), ("rover", "operator:plan"),
            ("door-guid", "operator:asset")} == provenance
    checked = json.loads(run(tool, "verify", output))
    assert checked["schema"] == "container.bim-twin-verification"
    assert (checked["eventCount"], checked["firstTimestampNanoseconds"], checked["lastTimestampNanoseconds"]) == (8, 0, NS)
    mid = json.loads(run(tool, "sample", output, NS // 4))
    assert mid["schema"] == "container.bim-twin-sample" and mid["timestampNanoseconds"] == NS // 4
    pose = find_track(mid, "rover", "robot-pose")
    assert pose["event"]["payload"]["position"] == [1, 0, 0] and not pose["stale"], pose
    assert abs(pose["distanceToPlanMeters"] - 1) < 1e-10, pose
    assert find_track(mid, "rover/imu", "sensor-status")["event"]["payload"] == {"state": "observed"}
    assert all(track["event"]["entityId"] != "door-guid" for track in mid["tracks"])
    after = json.loads(run(tool, "sample", output, 4 * NS))
    assert all(track["stale"] for track in after["tracks"])
    assert find_track(after, "door-guid", "bim-asset-state")["event"]["payload"] == {"state": "open"}
    assert find_track(after, "rover/lidar", "sensor-status")["event"]["source"] == "simulation:lidar"
    assert find_track(after, "rover", "robot-pose")["observedPath"][-1] == [4, 0, 0]
    run(tool, "record", report, output, diagnostic="new BIM twin recording")
    assert output.read_bytes() == repeated.read_bytes()

    rejected = work / "rejected.json"
    reordered_source = {**SCENE_SOURCE, "layers": list(reversed(SCENE_SOURCE["layers"]))}
    mismatch = write_json(work / "other-scene.json", log([plan], scene_source=reordered_source))
    run(tool, "record", report, rejected, "--supplement", mismatch, diagnostic="sceneSource identity")
    wrong_clock = write_json(work / "other-clock.json", log([plan], clock="wall-clock"))
    run(tool, "record", report, rejected, "--supplement", wrong_clock, diagnostic="clockId")
    wrong_kind = write_json(work / "other-kind.json", log([event(
        "robot-pose", "rover", 0, "external", {"position": [0, 0, 0], "yaw": 0})]))
    run(tool, "record", report, rejected, "--supplement", wrong_kind, diagnostic="only planned-path")
    truncated = simulation()
    truncated["history"][-1]["tick"] = 7
    bad_report = write_json(work / "late-error.json", truncated)
    run(tool, "record", bad_report, rejected, diagnostic="tick")
    assert not rejected.exists()

    alarm = event("operator-alarm", "rover", 0, "operator:alarm",
                  {"alarmId": "door-clearance", "severity": "warning", "message": "Door clearance",
                   "active": True, "position": [0, 0, .5]})
    spatial_supplement = write_json(work / "operator-supplement.json", {**log([alarm]), "version": 3})
    spatial_output = work / "operator-recording.json"
    run(tool, "record", report, spatial_output, "--supplement", spatial_supplement)
    spatial = json.loads(spatial_output.read_bytes())
    assert spatial["version"] == 3
    assert [entry for entry in spatial["events"] if entry["kind"] == "operator-alarm"] == [alarm]
    assert [entry for entry in spatial["events"] if entry["source"].startswith("simulation:")] == [
        entry for entry in data["events"] if entry["source"].startswith("simulation:")]


def verify_contact_recording(tool, work):
    door = "bim/7:doorway/guid/9:door-guid"
    contact = {"otherEntityId": door, "positionMeters": [-.03, 0, 2.081], "normal": [-1, 0, 0]}
    report = simulation()
    report["history"][1].update(obstruction="blocked", blockingEntityId=door, contact=contact)
    report["history"][2].update(obstruction="none", blockingEntityId="")
    source = write_json(work / "contact-simulation.json", report)
    output = work / "contact-recording.json"
    run(tool, "record", source, output)
    recording = json.loads(output.read_bytes())
    assert recording["version"] == 4
    checked = json.loads(run(tool, "verify", output))
    assert checked["recordingVersion"] == 4 and checked["eventCount"] == 6
    for timestamp in (NS // 4, NS // 2, 3 * NS // 4, NS):
        sample = json.loads(run(tool, "sample", output, timestamp))
        pose = find_track(sample, "rover", "robot-pose")["event"]["payload"]
        if NS // 2 <= timestamp < NS:
            assert pose["contact"] == contact
        else:
            assert "contact" not in pose

    canonical = work / "canonical-contact.json"
    run(tool, "import", output, canonical)
    assert canonical.read_bytes() == output.read_bytes()

    report["history"][1]["contact"] = {**contact, "normal": [0, 0, 0]}
    malformed = write_json(work / "invalid-contact-simulation.json", report)
    rejected = work / "invalid-contact-recording.json"
    run(tool, "record", malformed, rejected, diagnostic="contact normal")
    assert not rejected.exists()


def verify_external_import(tool, work):
    last = (1 << 64) - 1
    start = event("sensor-status", "inspection/imu", last - 1, "external-feed", {"state": "offline"})
    end = event("sensor-status", "inspection/imu", last, "external-feed", {"state": "online"})
    input_log = write_json(work / "external.json", log([end, start], clock="external-clock"))
    canonical = work / "canonical.json"
    run(tool, "import", input_log, canonical)
    assert json.loads(canonical.read_bytes())["events"] == [start, end]
    canonical_again = work / "canonical-again.json"
    run(tool, "import", canonical, canonical_again)
    assert canonical.read_bytes() == canonical_again.read_bytes()
    checked = json.loads(run(tool, "verify", canonical))
    assert checked["lastTimestampNanoseconds"] == last and checked["clockId"] == "external-clock"
    sampled = json.loads(run(tool, "sample", canonical, last))
    assert find_track(sampled, "inspection/imu", "sensor-status")["event"] == end
    run(tool, "sample", canonical, last + 1, diagnostic="TIMESTAMP_NS")
    run(tool, "sample", canonical, "-1", diagnostic="TIMESTAMP_NS")

    invalid_input = work / "invalid.json"
    rejected = work / "invalid-output.json"
    write_json(invalid_input, log([start, {**start, "source": "conflicting-feed"}]))
    run(tool, "import", invalid_input, rejected, diagnostic="duplicate")
    write_json(invalid_input, log([{**start, "payload": {"other": "ignored"}}]))
    run(tool, "import", invalid_input, rejected, diagnostic="unknown field")
    write_json(invalid_input, log([{**start, "timestampNanoseconds": 1.5}]))
    run(tool, "import", invalid_input, rejected, diagnostic="nonnegative 64-bit integer")
    duplicate_key = json.dumps(log([start])).replace('"version": 1', '"version": 1, "version": 1')
    invalid_input.write_text(duplicate_key, encoding="utf-8")
    run(tool, "import", invalid_input, rejected, diagnostic="duplicate key")
    assert not rejected.exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    args = parser.parse_args()
    tool = args.tool.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="container-bim-twin-") as directory:
        verify_recording(tool, Path(directory))
        verify_contact_recording(tool, Path(directory))
        verify_external_import(tool, Path(directory))
    print("BIM twin recording, canonical import, provenance, replay and validation passed")


if __name__ == "__main__":
    main()
