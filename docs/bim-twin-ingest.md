# Ingest robot telemetry for BIM replay

`BimRoboticsTwin ingest` takes a stream of timestamped messages and produces
a recording for the BIM Twin Replay viewport. The same message and receipt
types are available to adapters through `Container/robotics/TwinIngest.h`.
Each session uses one verified BIM model and one clock domain.

## Submit a JSONL feed

Build `BimRoboticsTwin` from the repository root, then save one message per
line in `out/doorway-events.jsonl`. Here the timestamps are nanoseconds on
the gateway clock and poses use the building-local REP-103 frame in metres.

```jsonl
{"schema":"container.bim-twin-event","version":1,"clockId":"gateway-monotonic","receivedTimestampNanoseconds":1000000000,"event":{"timestampNanoseconds":950000000,"entityId":"robot/base","source":"robot:controller","kind":"robot-pose","payload":{"position":[-1,0,1.2],"yaw":0}}}
{"schema":"container.bim-twin-event","version":1,"clockId":"gateway-monotonic","receivedTimestampNanoseconds":1000000000,"event":{"timestampNanoseconds":950000000,"entityId":"robot/base","source":"robot:diagnostics","kind":"robot-health","payload":{"state":"nominal","batteryPercent":78}}}
{"schema":"container.bim-twin-event","version":1,"clockId":"gateway-monotonic","receivedTimestampNanoseconds":1050000000,"event":{"timestampNanoseconds":1000000000,"entityId":"robot/base","source":"planner:inspection","kind":"task-status","payload":{"taskId":"inspect-entrance","state":"running","progress":0.5}}}
{"schema":"container.bim-twin-event","version":1,"clockId":"gateway-monotonic","receivedTimestampNanoseconds":1050000000,"assetReference":"ifc-guid","event":{"timestampNanoseconds":1000000000,"entityId":"door-guid","source":"gateway:door-contact","kind":"bim-asset-state","payload":{"state":"open"}}}
```

```powershell
cmake --build out/build/windows-release --target BimRoboticsTwin
$tool = 'out/build/windows-release/BimRoboticsTwin.exe'
$model = 'tests/fixtures/robotics/doorway.ifc'
& $tool ingest $model out/doorway-events.jsonl out/doorway-telemetry.json doorway gateway-monotonic
& $tool sample out/doorway-telemetry.json 1000000000
```

The positional arguments give the model, input feed, fresh output path,
BIM namespace, and clock ID. `--stale-after-ns N` changes the freshness
window from its default of two seconds. If the source has no declared
metric units, supply verified scale evidence with
`--metres-per-renderer-unit N`. Ingestion records the exact IFC bytes or
ordered IFCX source layers and the physical scale used to resolve the model.

| Kind | Event payload |
| --- | --- |
| `robot-pose` | `position` in REP-103 metres and `yaw` in radians about +Z. |
| `robot-health` | Text `state` and optional `batteryPercent` in the inclusive range 0–100. |
| `task-status` | `taskId`, text `state`, and optional `progress` in the inclusive range 0–1. |
| `bim-asset-state` | Text `state` for a BIM asset. |
| `sensor-status` | Text `state` for a sensor. |
| `planned-path` | `points` in REP-103 metres. An empty array clears that robot's plan. |

For BIM asset state, `entityId` defaults to the stable occurrence ID from
`BimRoboticsScene`. Set `assetReference` to `ifc-guid` to use an authored
IFC GUID. A unique GUID resolves to the canonical occurrence ID. An
ambiguous GUID requires an explicit occurrence ID. Receipts retain both the
input reference and resolved identity. Asset events remain telemetry
annotations. [Semantic BIM identity guide](ifc-import.md#export-a-semantic-scene-for-robotics)

## Ordering and receipts

- Every receipt timestamp must be in the configured clock. Receipt times on
  that clock advance monotonically, including when an observation is
  rejected. An observation from the future or older than the freshness
  window is rejected. An age equal to the window is valid.
- A fresh event arriving behind the latest observation on its track is
  accepted into history and marked `accepted-out-of-order`. The latest view
  keeps the newer observation. Each task ID has its own track for that
  robot, so different tasks can report at the same instant.
- An identical fresh event on the same track and timestamp is a `duplicate`.
  A differing event at that key is a `conflict`. This check compares the
  resolved entity, producer, and payload. A duplicate adds no history row.
- The CLI reports each accepted, rejected, duplicate, or malformed row in
  its stdout JSON, with a disposition code. A mixed feed publishes the
  accepted subset to a new file. If zero events are accepted, the command
  returns a failure before publishing a recording.

The typed `latest(nowNanoseconds)` view returns the latest observation per
track at or before the requested time, with a stale flag. Timeline replay can
interpolate between fresh pose samples. Logs with health or task events use
recording version 2. Operator alarms, sensor frusta and occupancy grids use
version 3, with the same JSONL envelope. [Spatial payloads](bim-twin-operator-overlays.md#review-the-doorway-fixture)

The viewer also opens existing version-1 logs. The feed
limits are 512 KiB per row, 64 MiB total and 65,536 rows.
[Open a recording in the BIM viewport](bim-twin-replay.md)
