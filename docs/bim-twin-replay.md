# Replay robot telemetry in the BIM viewport

Record a simulated run or import a timeline from an external producer.
The BIM viewport shows the sampled robot pose, the observed trail, the active
plan, sensor states and asset annotations. Scrubbing the timeline changes
the annotations while preserving the imported building. [Inspect live operator overlays](bim-twin-operator-overlays.md) · [Replay a simulated fleet mission](bim-fleet-missions.md)

## Record and replay a doorway run

Run these commands from the repository root in Developer PowerShell. The
recorder and viewer can replay an existing log without Jolt. Building and
running the simulator example requires `ENABLE_PHYSICS=ON`.

```powershell
cmake --preset windows-release -DENABLE_PHYSICS=ON
$build = 'out/build/windows-release'
cmake --build $build --target BimRoboticsSim BimRoboticsTwin BimRoboticsScene VulkanSceneRenderer
$fixture = 'tests/fixtures/robotics'
$model = "$fixture/doorway.ifc"
$work = 'out/twin-doorway'
New-Item -ItemType Directory $work

$scenario = Get-Content -Raw "$fixture/doorway-drive.json" | ConvertFrom-Json -AsHashtable
$scenario.driveSensors = @{ samplesPerSecondMilliHz = 20000 }
$scenario.lidar = @{
    scansPerSecondMilliHz = 10000
    azimuthSamples = 16
    maxRangeMeters = 10
    mount = @{ xyz = @(0, 0, 0.7) }
}
$scenario | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 "$work/scenario.json"
& "$build/BimRoboticsSim.exe" $model "$work/simulation.json" doorway "$work/scenario.json"
& "$build/BimRoboticsTwin.exe" record "$work/simulation.json" "$work/timeline.json"
& "$build/VulkanSceneRenderer.exe" --model $model --bim-twin-replay "$work/timeline.json" `
    --bim-twin-time-ns 500000000 --camera-position 3.2 4.8 7.2 --camera-target 0 1.48 0
```

The replay starts paused. Use **BIM Twin Replay** to play, pause, reset
or scrub time. The starting cursor is clamped to the recorded range. The
viewport annotations include robot position and heading in BIM context.
Known BIM assets use their imported bounds, and unresolved IDs are reported
in the panel. These annotations appear in UI-inclusive PNGs. Viewport-only
and scene-linear captures continue to show the rendered scene.

`BimRoboticsTwin verify LOG.json` validates the recording and reports its
time range. `BimRoboticsTwin sample LOG.json TIMESTAMP_NS` emits a sampled
state as JSON for inspection. Simulation ticks convert to integer nanoseconds
by rounding down. The recorder retains committed base poses. IMU and LiDAR
events report when the simulator produced each observation.

## Add planned paths and BIM asset state

A supplemental log shares the simulation's clock, source namespace, ordered
BIM fingerprint and freshness timeout. It can add planned paths and asset
state to a simulated run. Use a fresh output path for each recording.

```powershell
$report = Get-Content -Raw "$work/simulation.json" | ConvertFrom-Json -AsHashtable
$supplement = @{
    schema = 'container.bim-twin-telemetry'
    version = 1
    sourceNamespace = $report.sourceNamespace
    sceneSource = $report.sceneSource
    clockId = 'simulation-fixed-step'
    staleAfterNanoseconds = 2000000000
    events = @(
        @{
            timestampNanoseconds = 0
            entityId = $report.robotId
            kind = 'planned-path'
            source = 'operator:doorway-route'
            payload = @{ points = @(@(-1.5, 0.18, 1.2), @(1.5, 0.18, 1.2)) }
        },
        @{
            timestampNanoseconds = 1200000000
            entityId = 'bim/7:doorway/guid/9:door-guid'
            kind = 'bim-asset-state'
            source = 'operator:door-inspection'
            payload = @{ state = 'open' }
        }
    )
}
$supplement | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 "$work/supplement.json"
& "$build/BimRoboticsTwin.exe" record "$work/simulation.json" "$work/annotated.json" `
    --supplement "$work/supplement.json"
```

Load `annotated.json` as the replay file to inspect both paths and the door
marker. The pose diagnostic measures the distance to the nearest point on the
active planned path when both tracks are fresh. Asset events annotate the
view and leave the authored door geometry and properties intact.

## Import external telemetry

Write a `container.bim-twin-telemetry` recording and pass it to
`BimRoboticsTwin import INPUT.json NEW_LOG.json`. Events use unsigned integer
nanoseconds in a single declared `clockId` and retain their producer
`source`. An external adapter can normalize its timestamps to this domain
before importing. The recorder has no middleware dependency.

| Event kind | Stable identity and payload |
| --- | --- |
| `robot-pose` | Robot ID with `position: [x, y, z]` in REP-103 metres and `yaw` in radians about +Z. |
| `planned-path` | Same robot ID with `points: [[x, y, z], …]` in the same frame. An empty plan clears the path. |
| `sensor-status` | Sensor ID with text `state`. |
| `bim-asset-state` | BIM semantic entity ID with text `state`. Use the entity's ID from the semantic scene export. |
| `robot-health` | Robot ID with text `state` and optional `batteryPercent`. Available in version 2. |
| `task-status` | Robot ID with `taskId`, text `state` and optional `progress` from zero to one. Available in version 2. |
| `operator-alarm`, `sensor-frustum`, `occupancy-grid` | Spatial operator events available in version 3. [Payload fields and fixture](bim-twin-operator-overlays.md#review-the-doorway-fixture) |

The importer sorts out-of-order observations and rejects two events with the
same track and timestamp. Each robot's task and each entity's alarm have their own tracks. Replay holds
the latest eligible state. It interpolates position and shortest-arc yaw only between robot
samples whose spacing is within the freshness timeout. State becomes stale
once its age exceeds that timeout. Before a track's first event it has no
state. A recording accepts up to 65,536 events and 32 MiB of JSON. The
viewport trail uses up to 2,048 measured poses per robot.

The viewer matches the recording against the actual BIM source bytes and
metric scale. Its coordinate conversion accounts for the renderer's
`--import-scale`, so the overlay remains aligned when the model is enlarged
for inspection. [Coordinate conventions](coordinate-conventions.md#semantic-bim-export-for-robotics)
· [Export semantic BIM identities](ifc-import.md#export-a-semantic-scene-for-robotics)

Use message ingestion for an arriving feed that needs explicit duplicate,
age and asset-resolution diagnostics. [Ingest robot telemetry](bim-twin-ingest.md)
