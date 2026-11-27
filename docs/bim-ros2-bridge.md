# Drive a BIM robot from ROS 2 Jazzy

The optional `bim_ros_bridge` package runs the fixed-step BIM simulator as a
child process. It translates ROS messages over a local, versioned JSONL
stream. The simulator owns the collision world, drive, pose, and sensor
sampling. ROS, Vulkan presentation, and editor UI are unnecessary for the
native process.

## Build the simulator and the ROS package

Configure the renderer repository with `ENABLE_PHYSICS=ON` using the
[standard build](build-and-test.md). This example uses the Windows Release
directory. A Linux build uses the same targets with its own build directory.

```powershell
$build = 'out/build/windows-release'
$fixtures = 'tests/fixtures/robotics'
cmake --build $build --config Release --target BimRoboticsSim BimRoboticsNavMap BimRoboticsCollision
python -B tests/robotics/bim_nav2_map.py --exporter "$build/BimRoboticsNavMap.exe" `
    --collision "$build/BimRoboticsCollision.exe" --fixtures $fixtures --output out/ros2-maps
```

On Ubuntu 24.04 or WSL 2, use a ROS 2 Jazzy environment with the ROS apt
repository configured. Install the optional adapter and demo dependencies,
then build the package from the repository root. [Jazzy installation](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html)

```bash
sudo apt-get install python3-colcon-common-extensions python3-yaml \
  ros-jazzy-rclpy ros-jazzy-tf2-ros-py ros-jazzy-launch-ros \
  ros-jazzy-nav2-map-server ros-jazzy-nav2-planner ros-jazzy-nav2-navfn-planner \
  ros-jazzy-nav2-controller ros-jazzy-nav2-regulated-pure-pursuit-controller \
  ros-jazzy-rosbag2 ros-jazzy-rosbag2-storage-mcap
source /opt/ros/jazzy/setup.bash
colcon --log-base out/ros2/log build --paths ros2/bim_ros_bridge \
  --build-base out/ros2/build --install-base out/ros2/install
source out/ros2/install/local_setup.bash
```

Use the Linux `BimRoboticsSim` path or a Windows `BimRoboticsSim.exe` on a
drive mounted in WSL with executable interoperability enabled. The adapter
converts scene and configuration paths with `wslpath` for a Windows child.
For the Windows verifier, run the demo from the same checkout under
`/mnt/c` so the evidence is visible to PowerShell. With a separate Linux
checkout, copy the demo evidence and map bundle with the original IFC files.
Keep the exact IFC bytes so the source fingerprints still match.

## Run the doorway demo

Set `sim` to the built simulator and `maps` to the exact `run-*` directory
printed by the exporter. This directory contains `cases.json` and the
generated fixture maps.

```bash
python3 -B tests/robotics/bim_ros2_nav2_demo.py --sim-executable "$sim" \
  --maps "$maps" --fixtures tests/fixtures/robotics --output out/ros2-doorway-run
```

The demo starts the bridge, the Jazzy map server, NavFn, and the Regulated
Pure Pursuit controller in a local ROS domain. It verifies the BIM map
published by the server, asks the planner for two waypoint legs, and submits
each returned path to `FollowPath`. The controller sends planar `/cmd_vel`
commands to the real simulator. The robot crosses the authored open door and
stops at the two requested waypoints. The demo records an MCAP bag with
simulation time and plays the recorded messages in a second ROS domain to
compare their payloads. [Nav2 FollowPath](https://github.com/ros-navigation/navigation2/blob/jazzy/nav2_msgs/action/FollowPath.action) · [Rosbag2](https://github.com/ros2/rosbag2/blob/jazzy/README.md)

With Windows native tools, run the physical verifier from Developer
PowerShell. Replace `<printed-id>` with the exact directory suffix reported
by the exporter. Use the same generated map bundle and demo directory.

```powershell
$maps = "out/ros2-maps/run-<printed-id>"
python -B tests/robotics/bim_ros2_drive_verify.py --simulator "$build/BimRoboticsSim.exe" `
    --collision "$build/BimRoboticsCollision.exe" --evidence out/ros2-doorway-run `
    --maps $maps --fixtures $fixtures
```

On Linux, pass the corresponding executable paths without `.exe`.
`navigation.json` records waypoint errors and the paths sent to Nav2.
`drive.jsonl` records the actual applied command and committed pose per tick.
`drive-verification.json` records capsule sweeps of the driven segments and
byte-matched RGB-D checks from native command replay. `bag-verification.json`
records the ROS timestamps, frame agreement, calibration, and bag playback
checks. Use a new output directory for every run.

## Launch the bridge on its own

```bash
ros2 launch bim_ros_bridge bridge.launch.py sim_executable:="$sim" \
  scene:=tests/fixtures/robotics/doorway.ifc namespace:=doorway \
  scenario:=tests/fixtures/robotics/doorway-ros2-drive.json \
  camera:=tests/fixtures/robotics/doorway-ros2-camera.json \
  map_manifest:="$maps/doorway-open/waypoints.json" trace:=out/bridge-drive.jsonl
```

The scenario supplies the spawn, initial door state, and tick budget.
The bridge exits cleanly at the scenario budget. Pass `max_steps:=N` to
the launch file or `--max-steps N` to the bridge to stop earlier. Zero
publishes just the initial observation. Use `--stream` on `BimRoboticsSim`
to use JSONL without ROS. This mode accepts `describe`, `observe`, `step`,
and `close` requests. The description reports the scenario budget as
`tickLimit`. A `step` supplies the next integer tick,
`linearMetersPerSecond`, and `angularRadiansPerSecond`.
Batch commands and batch time schedules belong to the existing report mode.
The optional map manifest checks the source fingerprints, door state, capsule
dimensions, and selected floor before the ROS graph starts.

| Topic | Meaning |
| --- | --- |
| `/clock` | Simulated ROS time starts at one second. Each committed native tick adds its exact integer nanosecond offset. |
| `/tf`, `/tf_static` | REP-103 `map → odom → base_link → camera_optical_frame`. `map → odom` aligns the odometry frame to the BIM spawn. |
| `/odom` | Encoder-based pose, planar covariance, and measured twist in the base frame. |
| `/cmd_vel` | Planar `geometry_msgs/Twist` in metres and radians per second. The bridge sends zero velocity when the last valid command is older than 0.5 seconds. |
| `/camera/image_raw`, `/camera/camera_info` | Scheduled RGB8 and calibration in the camera optical frame, with the same native tick stamp. |
| `/camera/depth/image_raw`, `/camera/depth/camera_info` | Little-endian `32FC1` axial metres and the same optical calibration. A missed ray gives NaN. |

The CPU RGB-D camera traces the live collision geometry, including the door
state, and colors hits from authored BIM material factors when available.
It supports pinhole calibration and resolutions up to 256 × 256. Use the
[hidden Vulkan camera capture](sensor-camera-datasets.md) for full render
materials or lens and sensor appearance effects. [ROS frame conventions](https://www.ros.org/reps/rep-0103.html) · [Depth image representation](https://www.ros.org/reps/rep-0118.html)
