"""Verify the transport ingestion workflow against authored IFC and IFCX IDs."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


DOOR_ID = "bim/7:doorway/guid/9:door-guid"
CLOCK = "gateway-monotonic"
ROBOT = "robot/base"


def run(tool, arguments, success=True):
    result = subprocess.run([str(tool), *map(str, arguments)], cwd=tool.parent,
                            capture_output=True, encoding="utf-8", timeout=30)
    assert (result.returncode == 0) == success, (result.args, result.returncode, result.stderr, result.stdout)
    return result


def delivery(kind, timestamp, received, payload, *, entity=ROBOT, clock=CLOCK,
             source="gateway:doorway", asset_reference=None):
    row = {
        "schema": "container.bim-twin-event", "version": 1, "clockId": clock,
        "receivedTimestampNanoseconds": received,
        "event": {"kind": kind, "timestampNanoseconds": timestamp, "entityId": entity,
                  "source": source, "payload": payload},
    }
    if asset_reference is not None:
        row["assetReference"] = asset_reference
    return row


def write_rows(file, rows):
    file.write_text("\n".join(row if isinstance(row, str) else json.dumps(row) for row in rows) + "\n",
                    encoding="utf-8")


def identity(path):
    data = path.read_bytes()
    return {"sha256": hashlib.sha256(data).hexdigest(), "byteSize": len(data)}


def sample(tool, recording, when):
    data = json.loads(run(tool, ["sample", recording, when]).stdout)
    return data, {track["event"]["kind"]: track for track in data["tracks"] if track["event"]["kind"] != "task-status"}


def test_ifc(tool, model, work):
    def pose(timestamp, received, position):
        return delivery("robot-pose", timestamp, received, {"position": position, "yaw": 0})

    late = pose(930, 1005, [-1.0, 0.0, 1.2])
    retry = dict(late, receivedTimestampNanoseconds=1006)
    conflicting = copy.deepcopy(retry)
    conflicting["event"]["payload"]["position"][0] = 20
    asset = delivery("bim-asset-state", 1011, 1013, {"state": "open"}, entity="door-guid",
                     asset_reference="ifc-guid")
    same_asset = copy.deepcopy(asset)
    same_asset["assetReference"] = "entity-id"
    same_asset["receivedTimestampNanoseconds"] = 1014
    same_asset["event"]["entityId"] = DOOR_ID
    rows = [
        pose(950, 1000, [0.5, 0.0, 1.2]),
        late,
        retry,
        conflicting,
        pose(900, 1007, [99, 0, 0]),
        pose(1010, 1008, [99, 0, 0]),
        dict(pose(1001, 99_999, [99, 0, 0]), clockId="other-clock"),
        pose(1001, 1007, [99, 0, 0]),
        delivery("task-status", 1000, 1010, {"taskId": "inspect", "state": "running", "progress": .25}),
        delivery("task-status", 1000, 1010, {"taskId": "report", "state": "queued"}),
        delivery("task-status", 1009, 1011, {"taskId": "inspect", "state": "running", "progress": .5}),
        delivery("robot-health", 1011, 1013, {"state": "low-battery", "batteryPercent": 12.5}),
        asset,
        same_asset,
        delivery("bim-asset-state", 1012, 1014, {"state": "open"}, entity="unknown", asset_reference="ifc-guid"),
        "} invalid JSON",
        "x" * (512 * 1024 + 8),
        delivery("robot-health", 1014, 1015, {"state": "invalid", "batteryPercent": -1}),
        delivery("bim-asset-state", 1020, 1022, {"state": "closed"}, entity=DOOR_ID),
    ]
    expected = [
        "accepted", "accepted-out-of-order", "duplicate", "conflict", "stale-observation",
        "future-observation", "clock-mismatch", "receipt-time-regression", "accepted", "accepted",
        "accepted", "accepted", "accepted", "duplicate", "unknown-asset", "malformed-row",
        "malformed-row", "invalid-event", "accepted",
    ]
    feed, log = work / "ifc-telemetry.jsonl", work / "recording.json"
    write_rows(feed, rows)
    original = identity(model)
    result = json.loads(run(tool, ["ingest", model, feed, log, "doorway", CLOCK, "--stale-after-ns", 100]).stdout)
    assert result["schema"] == "container.bim-twin-ingest-result" and result["outputPublished"]
    assert result["lines"] == len(rows) and result["accepted"] == 8 and result["acceptedOutOfOrder"] == 1
    assert result["duplicates"] == 2 and result["rejected"] == 9, result
    receipts = result["receipts"]
    assert [receipt["line"] for receipt in receipts] == list(range(1, len(rows) + 1))
    assert [receipt["disposition"] for receipt in receipts] == expected, receipts
    for receipt in (receipts[12], receipts[13], receipts[18]):
        assert receipt["resolvedEntityId"] == DOOR_ID and receipt["originalGuid"] == "door-guid", receipt
    assert receipts[12]["inputEntityId"] == "door-guid" and receipts[12]["source"] == "gateway:doorway"
    assert receipts[12]["receivedTimestampNanoseconds"] == 1013 and receipts[12]["timestampNanoseconds"] == 1011

    data = json.loads(log.read_bytes())
    assert data["schema"] == "container.bim-twin-telemetry" and data["version"] == 2, data
    assert data["sceneSource"]["layers"] == [original] and data["sourceNamespace"] == "doorway"
    assert data["clockId"] == CLOCK and data["staleAfterNanoseconds"] == 100
    events = data["events"]
    assert len(events) == 8 and [event["timestampNanoseconds"] for event in events[:2]] == [930, 950]
    assert [event["entityId"] for event in events if event["kind"] == "bim-asset-state"] == [DOOR_ID, DOOR_ID]
    replay, tracks = sample(tool, log, 1019)
    assert tracks["robot-pose"]["event"]["payload"]["position"] == [.5, 0, 1.2]
    assert tracks["robot-health"]["event"]["payload"]["batteryPercent"] == 12.5
    assert tracks["bim-asset-state"]["event"]["payload"]["state"] == "open"
    tasks = {track["event"]["payload"]["taskId"]: track["event"]["payload"]
             for track in replay["tracks"] if track["event"]["kind"] == "task-status"}
    assert tasks == {"inspect": {"taskId": "inspect", "state": "running", "progress": .5},
                     "report": {"taskId": "report", "state": "queued"}}, tasks
    _, stale = sample(tool, log, 1121)
    assert all(track["stale"] for track in stale.values()), stale
    assert stale["bim-asset-state"]["event"]["payload"]["state"] == "closed"

    repeat = work / "recording-repeat.json"
    run(tool, ["ingest", model, feed, repeat, "doorway", CLOCK, "--stale-after-ns", 100])
    assert repeat.read_bytes() == log.read_bytes() and identity(model) == original
    output_before = identity(log)
    run(tool, ["ingest", model, feed, log, "doorway", CLOCK], success=False)
    assert identity(log) == output_before
    wrong = work / "wrong-clock.jsonl"
    write_rows(wrong, [rows[6]])
    missing = work / "no-accepted-log.json"
    failed = json.loads(run(tool, ["ingest", model, wrong, missing, "doorway", CLOCK], success=False).stdout)
    assert not missing.exists() and not failed["outputPublished"] and failed["receipts"][0]["disposition"] == "clock-mismatch"


def test_ifcx(tool, scene_exporter, work):
    base, assembly = work / "doors.ifcx", work / "assembly.ifcx"
    base.write_text(json.dumps({"data": [
        {"path": "template", "attributes": {"bsi::ifc::class": "IfcDoor", "bsi::ifc::globalId": "shared-door-guid"},
         "children": {"Body": "mesh"}},
        {"path": "mesh", "attributes": {"usd::usdgeom::mesh": {
            "faceVertexIndices": [0, 1, 2], "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]}}},
    ]}), encoding="utf-8")
    assembly.write_text(json.dumps({"imports": [{"uri": base.name}], "data": [
        {"path": "root", "children": {"First": "one", "Second": "two"}},
        {"path": "one", "inherits": ["template"]},
        {"path": "two", "inherits": ["template"]},
    ]}), encoding="utf-8")
    before = [identity(assembly), identity(base)]
    manifest = work / "ifcx-scene.json"
    run(scene_exporter, [assembly, manifest, "plant", "--metres-per-renderer-unit", .5])
    ids = [entity["id"] for entity in json.loads(manifest.read_bytes())["entities"] if entity["guid"] == "shared-door-guid"]
    assert len(ids) == 2 and ids[0] != ids[1], ids

    rows = [delivery("bim-asset-state", 100, 100, {"state": "open"}, entity="shared-door-guid",
                     asset_reference="ifc-guid")]
    rows.extend(delivery("bim-asset-state", 110, 110, {"state": state}, entity=entity)
                for entity, state in zip(ids, ("open", "closed")))
    feed, log = work / "ifcx-telemetry.jsonl", work / "ifcx-recording.json"
    write_rows(feed, rows)
    result = json.loads(run(tool, ["ingest", assembly, feed, log, "plant", CLOCK, "--metres-per-renderer-unit", .5]).stdout)
    receipts = result["receipts"]
    assert result["accepted"] == 2 and result["rejected"] == 1, result
    assert receipts[0]["disposition"] == "ambiguous-asset", receipts
    assert [receipt["resolvedEntityId"] for receipt in receipts[1:]] == ids
    assert all(receipt["originalGuid"] == "shared-door-guid" for receipt in receipts[1:])
    data = json.loads(log.read_bytes())
    assert data["sceneSource"] == {"layers": before, "metersPerUnscaledRendererUnit": .5}, data
    assert [identity(assembly), identity(base)] == before


def test_operator_overlays(tool, model, work):
    alarm = {"alarmId": "doorway-obstruction", "severity": "warning", "message": "Doorway blocked",
             "active": True, "position": [-.3, 0, 1.2]}
    second_alarm = {**alarm, "alarmId": "low-clearance", "severity": "info", "message": "Low clearance"}
    frustum = {"width": 640, "height": 480, "fx": 525, "fy": 510, "cx": 307, "cy": 232,
               "nearMeters": .05, "farMeters": 2.5,
               "worldFromCamera": [0, -1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1.5, .2, 1.9, 1]}
    grid = {"origin": [-1.5, -.5, 1.2], "yaw": .3, "resolutionMeters": .25,
            "width": 3, "height": 2, "cells": "#?.#.."}
    rows = [
        delivery("robot-pose", 1000, 1000, {"position": [-1.5, 0, 1.2], "yaw": 0}),
        delivery("robot-health", 1000, 1000, {"state": "ready", "batteryPercent": 80}),
        delivery("operator-alarm", 1000, 1000, alarm),
        delivery("operator-alarm", 1000, 1000, second_alarm),
        delivery("sensor-frustum", 1000, 1000, frustum, entity="robot/base/camera"),
        delivery("occupancy-grid", 1000, 1000, grid, entity="doorway/map"),
        delivery("operator-alarm", 1010, 1011, {**alarm, "active": False}),
        delivery("sensor-frustum", 1011, 1011, {**frustum, "width": 1 << 32}),
        delivery("sensor-frustum", 1012, 1012, {**frustum, "worldFromCamera": [1] * 15}),
        delivery("operator-alarm", 1013, 1013, {**alarm, "active": 1}),
        delivery("operator-alarm", 1014, 1014, {**alarm, "severity": "unrecognized"}),
        delivery("occupancy-grid", 1015, 1015, {**grid, "cells": "!....."}),
    ]
    feed, log = work / "operator-overlays.jsonl", work / "operator-overlays.json"
    write_rows(feed, rows)
    receipt = json.loads(run(tool, ["ingest", model, feed, log, "doorway", CLOCK,
                                    "--stale-after-ns", 100]).stdout)
    assert receipt["accepted"] == 7 and receipt["rejected"] == 5 and receipt["outputPublished"], receipt
    assert [row["disposition"] for row in receipt["receipts"][7:]] == [
        "malformed-row", "malformed-row", "malformed-row", "invalid-event", "invalid-event"]
    assert "32-bit integer" in receipt["receipts"][7]["detail"]
    assert "16 column-major entries" in receipt["receipts"][8]["detail"]
    assert "boolean" in receipt["receipts"][9]["detail"]

    data = json.loads(log.read_bytes())
    assert data["version"] == 3 and len(data["events"]) == 7, data
    checked = json.loads(run(tool, ["verify", log]).stdout)
    assert checked["recordingVersion"] == 3 and checked["eventCount"] == 7, checked
    for when, active in ((1000, True), (1010, False)):
        replay = json.loads(run(tool, ["sample", log, when]).stdout)
        assert replay["version"] == 3 and replay["timestampNanoseconds"] == when, replay
        events = [row["event"] for row in replay["tracks"]]
        payloads = {event["kind"]: event["payload"] for event in events if event["kind"] != "operator-alarm"}
        alarms = {event["payload"]["alarmId"]: event["payload"] for event in events if event["kind"] == "operator-alarm"}
        assert payloads["sensor-frustum"] == frustum and payloads["occupancy-grid"] == grid, payloads
        assert alarms == {"doorway-obstruction": {**alarm, "active": active},
                          "low-clearance": second_alarm}, alarms
    canonical = work / "operator-overlays-reimported.json"
    run(tool, ["import", log, canonical])
    assert log.read_bytes() == canonical.read_bytes()

    for version in (1, 2):
        for kind in ("operator-alarm", "sensor-frustum", "occupancy-grid"):
            lower = work / f"unsupported-{kind}-v{version}.json"
            events = [row for row in data["events"] if row["kind"] == kind]
            lower.write_text(json.dumps({**data, "version": version, "events": events}), encoding="utf-8")
            output = lower.with_name(f"imported-{lower.name}")
            error = run(tool, ["import", lower, output], success=False)
            assert f"{kind} requires recording version 3" in error.stderr and not output.exists(), error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--scene-exporter", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="twin-ingest-") as output:
        work = Path(output)
        model = (args.fixtures / "doorway.ifc").resolve()
        test_ifc(args.tool.resolve(), model, work)
        test_ifcx(args.tool.resolve(), args.scene_exporter.resolve(), work)
        test_operator_overlays(args.tool.resolve(), model, work)
    print("PASS canonical telemetry receipts, v2/v3 replay, and IFC/IFCX entity resolution", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
