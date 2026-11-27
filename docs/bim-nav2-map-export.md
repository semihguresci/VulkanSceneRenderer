# Export a BIM floor to Nav2

`BimRoboticsNavMap` exports a two-dimensional occupancy map for one BIM floor.
It uses the requested robot capsule, the selected floor band and the door
state to classify cells. The exporter uses the headless BIM physics build.

## Export a doorway map

From Developer PowerShell, configure the [standard build](build-and-test.md)
with physics enabled.

```powershell
cmake --preset windows-release -DENABLE_PHYSICS=ON
cmake --build out/build/windows-release --target BimRoboticsNavMap BimRoboticsCollision
```

Save this request as `out/doorway-nav2-request.json`.

```json
{
  "schema": "container.bim-navigation-map-request",
  "version": 1,
  "sourceNamespace": "doorway",
  "originMeters": [-3.1, -3.1],
  "sizeCells": [62, 62],
  "resolutionMeters": 0.1,
  "floorMeters": 1.2,
  "floorToleranceMeters": 0.02,
  "radiusMeters": 0.25,
  "heightMeters": 1.6,
  "openDoorIds": ["bim/7:doorway/guid/9:door-guid"],
  "waypoints": [
    {"id": "start", "positionMeters": [-1.55, 0.05], "yawRadians": 0.0},
    {"id": "goal", "positionMeters": [1.55, 0.05], "yawRadians": 0.0}
  ]
}
```

```powershell
$build = 'out/build/windows-release'
$fixtures = 'tests/fixtures/robotics'
& "$build/BimRoboticsNavMap.exe" "$fixtures/doorway.ifc" `
    out/doorway-nav2-request.json out/doorway-nav2
```

Use a fresh output directory. The exporter validates waypoint clearance and
support on the selected floor. To export the closed-door state, use an empty
`openDoorIds` array. For other models, obtain canonical door occurrence IDs
from the semantic scene export. `metersPerRendererUnit` can supply verified
scale when the source lacks unit evidence. [Semantic BIM export](ifc-import.md#export-a-semantic-scene-for-robotics)

`map.yaml` and `map.pgm` are a trinary Nav2 map. White is free, black is
occupied, and grey is unknown. A free cell requires floor support across its
entire footprint and clearance from inflated obstacles. The PGM starts with
the top row. Nav2 publishes
the grid from its lower-left origin in map-frame metres. `waypoints.json`
stores the planar poses, selected floor elevation, source fingerprints, and
the PGM SHA-256. Use the floor elevation when mapping a planned XY pose back
into the BIM collision world. [BIM coordinate conventions](coordinate-conventions.md#semantic-bim-export-for-robotics)

## Verify real Nav2 paths

Export the acceptance fixtures from Developer PowerShell.

```powershell
python -B tests/robotics/bim_nav2_map.py --exporter "$build/BimRoboticsNavMap.exe" `
    --collision "$build/BimRoboticsCollision.exe" --fixtures $fixtures --output out/nav2-cases
```

The exporter prints the generated run directory under `out/nav2-cases`.
On Ubuntu 24.04 with the ROS 2 Jazzy apt repository configured, copy that
directory to `out/nav2-cases` so it directly contains `cases.json` and the
per-case maps. Install and source the Nav2 dependencies. [ROS 2 Jazzy installation](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html)

```bash
sudo apt-get install ros-jazzy-rclpy ros-jazzy-tf2-ros-py ros-jazzy-nav2-map-server \
    ros-jazzy-nav2-planner ros-jazzy-nav2-navfn-planner python3-yaml
source /opt/ros/jazzy/setup.bash
python3 -B tests/robotics/bim_nav2_planner.py --maps out/nav2-cases --output out/nav2-paths.json
```

The runner activates the Nav2 map server and NavFn planner in isolated
localhost ROS domains. It checks the map server's `/map` and the planner's
static costmap against the image, then requests a path between explicit
`map`-frame poses. The map already accounts for capsule clearance. The
planner uses the static layer, prohibits travel in unknown cells, and uses
zero goal tolerance. A disconnected fixture must return an empty path and
Nav2's `NO_VALID_PATH` code. The results include the returned stamped poses,
map image hash, action status and error code. Per-case logs appear next to
the results by default. [Jazzy planning action](https://github.com/ros-navigation/navigation2/blob/jazzy/nav2_msgs/action/ComputePathToPose.action)

Copy `out/nav2-paths.json` back to the exporter host and replay the exact
returned XY segments against the original BIM with the requested capsule and
floor. The Windows verifier checks the open-door crossing, closed-door detour
or disconnection, upper floor and unit-converted fixtures.

```powershell
python -B tests/robotics/bim_nav2_map.py --collision "$build/BimRoboticsCollision.exe" `
    --fixtures $fixtures --output out/nav2-cases --verify-nav2 out/nav2-paths.json
```
