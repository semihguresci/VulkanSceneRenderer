# BIM robotics preview

The preview includes two reproducible workflows using an authored IFC doorway.
The Windows dataset workflow renders synchronized color, depth, and BIM labels
along a simulated robot trajectory. The Ubuntu workflow exports a floor map,
drives the robot with ROS 2 Jazzy and Nav2, and verifies the recorded motion.
See the build dashboard for workflow downloads, checksums, supported platforms,
and measured results. [BIM robotics preview builds and evidence](https://github.com/semihguresci/VulkanSceneRenderer/blob/main/docs/bim-robotics-preview-release.md)
For a larger IFC example with a recorded inspection route and portable replay,
see the [Tekla House robotics demonstration](https://github.com/semihguresci/VulkanSceneRenderer/blob/main/docs/bim-tekla-robotics-demo.md).

## Record a dataset on Windows

Extract the entire Windows x64 robotics preview ZIP. You need Windows 10 or
11 x64, a GPU with a Vulkan 1.4 driver, the x64 Microsoft Visual C++ v14
Redistributable, and Python 3.10 or newer on PATH. The extracted preview
includes the executables, shaders, runtime files, and IFC fixture.
[Microsoft redistributable](https://aka.ms/vc14/vc_redist.x64.exe)

Run this in PowerShell from the directory containing `VulkanSceneRenderer.exe`.
Use a new output path for each run.

```powershell
python -B tools/robotics_preview_dataset.py out/doorway-dataset
```

The runner generates the scenario and camera calibration, runs the native BIM
simulator, captures each scheduled camera frame through Vulkan, and records,
verifies, and replays the dataset. It uses a hidden window on the current
desktop. Each stage has a 180-second timeout. Allow more time for device
startup with `--timeout SECONDS` if needed.

- `out/doorway-dataset/dataset/dataset.json` is the verified dataset manifest.
  Keep the accompanying EXR files, chunk JSONL files, and `labels.json`
  together when moving the dataset.
- `out/doorway-dataset/replay.jsonl` contains the committed ticks and aligned
  observations. The example produces eight ticks, four camera frames, three
  IMU and encoder observations, and two LiDAR scans.
- `scenario.json`, `camera.json`, `simulation.json`, and `camera-capture/` under
  the output directory retain the inputs and camera artifacts.
- The runner writes `summary.json` after checking the dataset and replay.
  Check `logs/` for the process that failed if the run stops.

The EXR channels contain scene-linear RGB, axial depth in metres, and integer
instance and semantic IDs. Resolve the IDs against `labels.json` to find their
BIM occurrences. The dataset verifier and replay tool work on the CPU.
[Dataset format](https://github.com/semihguresci/VulkanSceneRenderer/blob/main/docs/bim-robotics-datasets.md)

```powershell
.\BimRoboticsDataset.exe verify out/doorway-dataset/dataset
```

## Drive through the doorway on Ubuntu

Use Ubuntu 24.04 x64 with the ROS 2 Jazzy apt repository configured. An Ubuntu
24.04 WSL 2 installation can run the Linux preview. Keep the extracted package
and the run output on the Linux filesystem, such as under your home directory.
Run the commands from the extracted Nav2 preview directory containing
`bin/` and `tools/`. Use a new output path for each run.
[ROS 2 Jazzy installation](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html)

```bash
sudo apt update
sudo apt install ros-jazzy-ros-base python3-colcon-common-extensions python3-yaml \
  ros-jazzy-rclpy ros-jazzy-tf2-ros-py ros-jazzy-launch-ros \
  ros-jazzy-nav2-map-server ros-jazzy-nav2-planner ros-jazzy-nav2-navfn-planner \
  ros-jazzy-nav2-controller ros-jazzy-nav2-regulated-pure-pursuit-controller \
  ros-jazzy-rosbag2 ros-jazzy-rosbag2-storage-mcap
source /opt/ros/jazzy/setup.bash
python3 -B tools/bim_nav2_preview.py --output out/nav2-doorway
```

The runner builds the packaged ROS bridge with Colcon in a temporary directory.
It exports occupancy maps from the packaged IFC fixtures and uses the authored
open-door case for the drive. NavFn plans two waypoint legs and Regulated Pure
Pursuit sends velocity commands to the native simulator. The runner checks the
ROS bag replay and sweeps the committed path against the original BIM collision
geometry. Keep the IFC files byte-for-byte unchanged so the map, simulator,
and verifier agree on the source. Use `--timeout SECONDS` to extend the default
75-second live Nav2 deadline. The native tools run without a graphics device
or display.

- `out/nav2-doorway/maps/run-*/` contains the exported maps, waypoints, and BIM
  source fingerprints.
- `out/nav2-doorway/navigation/navigation.json` contains the planned paths and
  waypoint results. `drive.jsonl` contains the commands and committed poses.
- `out/nav2-doorway/navigation/observations/` contains the MCAP bag.
  `bag-verification.json` and `drive-verification.json` record the bag playback
  and native physical checks.
- `out/nav2-doorway/preview-summary.json` confirms the completed run and reports
  goal errors, sweep counts, and camera frames reproduced from native commands.
  If the run stops early, check the stage logs in `out/nav2-doorway/logs/`.

## Build from source

Set up CMake, Ninja, the compiler, Vulkan 1.4 development files, Slang, and
vcpkg before configuring. The shared CMake configure finds Vulkan and Slang
even when you build only the headless Linux targets. The Windows commands
use Developer PowerShell and the single-configuration Ninja preset.
[Build prerequisites](https://github.com/semihguresci/VulkanSceneRenderer/blob/main/docs/build-and-test.md)

Enable `ENABLE_PHYSICS` for the simulator and map tools. These examples leave
optional URDF and TinyUSDZ support off and use the checked-in IFC fixtures.
Before packaging on Windows, prepare the pinned Vulkan loader and license
described in the build prerequisites and set `$loader` to that directory.

```powershell
$build = 'out/build/windows-robotics'
cmake --preset windows-release -B $build `
    "-DCONTAINER_VULKAN_LOADER_ROOT=$loader" `
    -DENABLE_PHYSICS=ON -DENABLE_ROBOTICS_URDF=OFF -DENABLE_TINYUSDZ_USD_LOADER=OFF `
    -DENABLE_TESTS=ON -DENABLE_OPENVDB=OFF `
    -DENABLE_SAMPLE_MODEL_DOWNLOAD=OFF -DENABLE_BIM_SAMPLE_MODEL_DOWNLOAD=OFF `
    -DENABLE_USD_SAMPLE_MODEL_DOWNLOAD=OFF
cmake --build $build --config Release --target VulkanSceneRenderer OpenVdbTransportRender BimRoboticsSim `
    BimRoboticsNavMap BimRoboticsCollision BimRoboticsDataset
python -B tools/robotics_preview_dataset.py out/doorway-dataset --bin-dir $build
```

Use the packaging script to stage the same build for a clean installation.
Set `-Version` to your release version.

```powershell
./cmake/package_windows.ps1 -Version 0.1.0-preview `
    -RuntimeDirectory $build -BuildDirectory $build `
    -VcpkgInstalledDirectory "$build/vcpkg_installed" -IncludeRoboticsPreview
```

The example writes `VulkanSceneRenderer-0.1.0-preview-windows-x64-robotics-preview.zip`
and its `.zip.sha256` file under `out/packages/`. With the same Windows
configuration, run the CPU robotics regression profile from the source checkout.

```powershell
./tests/robotics/run_validation.ps1 -BuildDirectory $build -Profile physics-only
```

On Ubuntu, build just the native executables with the Linux Release preset.
The hosted Jazzy workflow shows its pinned vcpkg and Vulkan header revisions
and packages for shared configuration.
[Hosted Jazzy build recipe](https://github.com/semihguresci/VulkanSceneRenderer/blob/main/.github/workflows/robotics-physics-validation.yml)

```bash
build=out/build/linux-preview
cmake --preset linux-release -B "$build" \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DENABLE_PHYSICS=ON -DENABLE_ROBOTICS_URDF=OFF -DENABLE_TINYUSDZ_USD_LOADER=OFF \
  -DENABLE_TESTS=OFF -DENABLE_VULKAN_VALIDATION_LAYERS=OFF -DENABLE_RENDERER_MCP=OFF \
  -DENABLE_SAMPLE_MODEL_DOWNLOAD=OFF -DENABLE_BIM_SAMPLE_MODEL_DOWNLOAD=OFF \
  -DENABLE_USD_SAMPLE_MODEL_DOWNLOAD=OFF
cmake --build "$build" --target BimRoboticsSim BimRoboticsNavMap BimRoboticsCollision
source /opt/ros/jazzy/setup.bash
python3 -B tools/bim_nav2_preview.py --native-bin "$build" --output out/nav2-doorway
```

Package the native Linux Release build from the source checkout.
Use the static `x64-linux` vcpkg triplet as shown above. The packager checks
that the tools depend only on libraries supplied by the Linux system.

```bash
python3 -B tools/package_bim_nav2_preview.py --build-dir "$build" \
  --version 0.1.0-preview --output out/packages
```

The example writes `VulkanSceneRenderer-0.1.0-preview-linux-x64-nav2-preview.tar.gz`
under `out/packages/`, prints its SHA-256, and includes the file hashes and
runtime dependencies in the archive's `build-info.json`. If vcpkg libraries
and notices were installed in a separate directory, pass it with
`--vcpkg-installed-dir`.

## Limits and asset licenses

- The examples use the fixed-step differential-drive simulator and one
  navigation floor at a time. They use the project-authored fixtures covered by
  the root MIT license. No external BIM gallery assets are needed.
- The Vulkan dataset path uses native-resolution Deferred raster with 1× MSAA
  and TAA disabled. Depth and labels cover opaque and alpha-mask geometry. The
  camera replays base poses against the imported BIM snapshot.
- The ROS bridge streams the CPU collision camera at resolutions up to
  256 × 256. It uses pinhole calibration and collision material colors. Door
  state changes affect the live collision scene. The Vulkan dataset camera uses
  the separate renderer capture path.
- Keep the dependency notices with either preview package, including Jolt's
  MIT notice. ROS 2 and Nav2 come from the configured Ubuntu repositories under
  their respective licenses. [Project notices](https://github.com/semihguresci/VulkanSceneRenderer/blob/main/docs/third-party-notices.md)
