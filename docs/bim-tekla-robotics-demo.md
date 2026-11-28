# Tekla House robot inspection and replay

The demonstration imports the buildingSMART Tekla House STEP IFC, drives a
differential-drive robot along two 1 m legs, and stops facing an authored
concrete column. The native simulator commits 570 ticks at 60 Hz with seed
104729. The collision tool checks 300 moving segments and verifies that
extending the route would hit the inspection column. The final forward LiDAR
beam measures about 1.50 m to IFC product `#14449`, GUID
`1GPKJW000_dp4pD3WuCZ8u`.

Use `TeklaHouse.ifc` from buildingSMART revision
`1a63082ada967c683cfacee2005f8f749c8e1b79`. The runner verifies its SHA-256
before starting the simulation. [Pinned Tekla House STEP IFC](https://github.com/buildingSMART/IFC5-development/blob/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/Tekla%20House/TeklaHouse.ifc)

## Run the demonstration

Use a Developer PowerShell with the Release build prerequisites configured.
The full run needs a Vulkan device and creates hidden windows for the camera
captures. Choose a new output directory for each run. [Build prerequisites](build-and-test.md)

```powershell
$build = 'out/build/windows-robotics'
cmake --preset windows-release -B $build -DENABLE_PHYSICS=ON -DENABLE_TESTS=ON
cmake --build $build --target BimRoboticsSim BimRoboticsCollision BimRoboticsUsd BimRoboticsDataset VulkanSceneRenderer
python -B tools/bim_tekla_robotics_demo.py --bin-dir $build `
    --ifc 'C:/Models/TeklaHouse.ifc' --output out/tekla-inspection
```

The runner repeats the native simulation and compares the committed reports
byte for byte. It captures 20 calibrated camera frames twice, requires exact
metric depth and BIM IDs, and compares scene-linear RGB within 0.1% relative
or 0.00001 absolute error. It checks the inspection column at the final frame
center and records and replays the verified dataset. `summary.json` records
the source identity, distance, final
pose error, checksums, and observation artifacts. If a run stops, inspect its
stage logs under `logs/`.

Pass `--cpu-only` to check the route, LiDAR observation, and OpenUSD export
without starting the renderer. Use `--validation` for a camera run on a build
with the Khronos Vulkan validation layer configured.

## Replay the robot in OpenUSD

Open `replay.usda` in a USD viewer. Keep its adjacent `scene.usda` when moving
the replay. The static Tekla meshes, materials, source GUIDs, and collision
proxies are embedded in `scene.usda`. The reference Tekla stage is 4.96 GB.
The replay adds a visual robot proxy with each committed native pose and
heading. Use `scenario.json` with the pinned IFC to rerun the physics
simulation. `camera.json` contains the sensor calibration and
`simulation.json` contains the recorded trajectory.
[BIM OpenUSD coordinate and identity contract](robotics-openusd.md)

With `usd-core` installed, verify the composed stage and compare its pose
samples to the native simulation report.

```powershell
python -B tests/validation/bim_tekla_robotics_demo_roundtrip.py --demo out/tekla-inspection
```

To run the CPU regression through CTest, set `CONTAINER_IFC_SAMPLE_ROOT` to
the `examples` directory of the pinned buildingSMART checkout. The CPU
regression also uses the build's fetched sample if it is available.

```powershell
$env:CONTAINER_IFC_SAMPLE_ROOT = 'C:/Models/IFC5-development/examples'
ctest --test-dir $build -R '^bim_tekla_robotics_demo_(tests|cpu)$' --output-on-failure
```
