# Run a BIM fleet mission

`BimRoboticsFleet` moves named differential-drive robots along ordered waypoints
in a shared, fixed-step BIM collision world. The CPU-only tool records task
progress and committed robot poses on one simulation clock. The checked-in
doorway mission sends an inspector through an open door on the lower floor
while a second robot surveys the upper floor.

## Run the doorway mission

Build with physics enabled and run from the repository root. The output
directory must be new and its parent must already exist.

```powershell
cmake --build out/build/windows-release --target BimRoboticsFleet BimRoboticsTwin
$bin = 'out/build/windows-release'
$model = 'tests/fixtures/robotics/doorway.ifc'
$mission = 'tests/fixtures/robotics/doorway-fleet.json'
$output = 'out/doorway-fleet'
& "$bin/BimRoboticsFleet.exe" $model $mission $output doorway
& "$bin/BimRoboticsTwin.exe" verify "$output/telemetry.json"
& "$bin/BimRoboticsTwin.exe" sample "$output/telemetry.json" 2000000000
```

Set `openDoorIds` to `[]` in a copy of the mission to close the doorway. The
upper robot can still finish, and the lower robot records the authored door
as its obstruction when the mission deadline is reached. Door state is
applied to the collision world. The IFC or IFCX source is opened for reading.

## Mission JSON

Use `schema` `container.bim-fleet-mission` and `version` `1`. Distances and
feet positions use the building-local ROS REP-103 frame in metres with +Z up.
Yaw is measured in radians about +Z. A waypoint is reached when the committed
feet position is within the configured 3D distance tolerance.

| Field | Meaning |
| --- | --- |
| `robots` | One to 16 entries, each with a unique `robotId`, a `taskId`, `initialFeet` as `[x, y, z]`, and one to 256 `waypoints` in travel order. |
| `initialYaw` | Optional yaw on a robot entry. Defaults to `0`. |
| `maxLinearMetersPerSecond`, `maxAngularRadiansPerSecond` | Optional speed limits on a robot entry. They default to `0.8` and `3.0`, and must be positive and no greater than `2.0` and `10.0`, respectively. |
| `ticksPerSecond`, `maxTicks` | Optional shared clock rate and mission deadline. They default to `60` and `1200`, with allowed ranges 1–10,000 and 1–8191, respectively. The combined budget must fit a replay journal. |
| `waypointToleranceMeters` | Optional arrival tolerance. Defaults to `0.08` and must be positive and no greater than one metre. |
| `openDoorIds` | Optional array of canonical BIM door IDs that start open. The fixture namespace must match the namespace used to derive these IDs. |
| `metersPerRendererUnit` | Optional positive scale for a source with unknown metric units. |

The waypoint supervisor follows supplied routes, so use waypoints that meet
building clearance and elevation constraints. Peer robots share the collision
world and can block each other. The journal time for tick `t` is
`floor(t × 1,000,000,000 / ticksPerSecond)` nanoseconds in `fleet-fixed-step`.

## Inspect and replay the result

The tool publishes both files together in the new directory. It returns **0**
when all tasks complete, **2** when a valid run reaches the deadline with an
unfinished task, and **1** for a request or publication failure.

- `report.json` uses `container.bim-fleet-report` version 1. It records whether
  the fleet completed and the number of shared ticks. Each robot has a final
  pose, nullable completion tick, zero-based waypoint arrivals with committed
  positions, blocked tick count, and its last recorded obstruction and blocking
  entity ID when available.
- `telemetry.json` uses the [BIM twin recording format](bim-twin-replay.md).
  Each robot has a planned path, actual poses at shared clock ticks, and
  `running`, `succeeded`, or `timed-out` task records with waypoint-based
  progress. A robot pose can include the measured contact from a blocked
  collision query. The next pose without a contact clears the marker. Peer
  avoidance has no measured contact point. Both files carry the same ordered
  IFC or IFCX fingerprints, source namespace, units evidence, and clock.

Use the journal with `VulkanSceneRenderer --model MODEL.ifc --bim-twin-replay
OUTPUT_DIRECTORY/telemetry.json` for spatial review. Identical inputs on the
same executable and platform yield byte-identical reports and journals.
