# Validate BIM robotics

The Windows Release CI matrix exercises each combination of Jolt physics
and URDF support. All four profiles run the native BIM import and OpenUSD export,
camera and dataset, drive and LiDAR, and dependency-boundary tests. The jobs use the
committed fixtures and run on hosted CPU runners.

| Profile | Jolt | URDF | Additional coverage |
| --- | --- | --- | --- |
| `full` | On | On | Fixed-step replay, URDF collision and kinematics, articulated LiDAR golden, and CPU render-graph and BIM routing tests |
| `physics-only` | On | Off | Jolt contacts, fixed-step BIM replay, motion and sensor tests |
| `urdf-only` | Off | On | URDF import and articulated kinematics with the physics backend disabled |
| `minimal` | Off | Off | Scene preparation, sensor configuration, and dataset replay with both optional modules disabled |

Each profile uploads its exact test inventory and CTest JUnit results. The
physics profiles also produce a fixed-step replay summary. Failure artifacts
include CTest output and CMake configure diagnostics. The driver checks the
configured feature switches, test registrations, and completed test cases.

## Run a CPU profile locally

Use Developer PowerShell and the repository's pinned vcpkg and Vulkan SDK
setup. This example uses a dedicated build directory for the full profile.

```powershell
$build = 'out/build/robotics-validation/full'
cmake --preset windows-release -B $build `
    -DENABLE_TESTS=ON -DENABLE_PHYSICS=ON -DENABLE_ROBOTICS_URDF=ON `
    -DENABLE_SAMPLE_MODEL_DOWNLOAD=OFF -DENABLE_BIM_SAMPLE_MODEL_DOWNLOAD=OFF `
    -DENABLE_USD_SAMPLE_MODEL_DOWNLOAD=OFF
./tests/robotics/run_validation.ps1 -BuildDirectory $build -Profile full
```

For another profile, use its feature switches from the table and a separate
build directory. The script writes `robotics-validation/profile.json` and
`robotics-validation/ctest.junit.xml` below that directory. Results from the
fixed-step replay are under `test_results/bim-fixed-step-replay/` when Jolt
is enabled.

The articulated LiDAR golden uses the millimetre `lidar-corner.ifc` fixture
at renderer import scale 2.5 and the existing two-link URDF arm. The sensor
is offset and tilted on the moving forearm. At six fixed ticks the test
checks the metric REP-103 sensor pose, hit point, range, and authored IFC
wall identity against independent numeric witnesses. The scan transitions
between perpendicular walls as the arm rotates, which makes axis and
scale mistakes observable. [Coordinate conventions](coordinate-conventions.md#semantic-bim-export-for-robotics)

The optional OpenUSD round-trip test checks the generated stage with an external
SDK. The same fixture can run in Isaac Sim and verify collision queries.
[Export and validate a robotics OpenUSD scene](robotics-openusd.md)

## Run the optional Vulkan smoke tests

Use the full profile on a machine with a Vulkan-capable GPU, a window system,
and the Khronos validation layer. Build the renderer with validation support
and stage the simulator, recorder, and renderer assets.

```powershell
cmake -S . -B $build -DENABLE_VULKAN_VALIDATION_LAYERS=ON
cmake --build $build --config Release --target VulkanSceneRenderer BimRoboticsSim BimRoboticsDataset
$env:VK_LAYER_PATH = Join-Path $env:VULKAN_SDK 'Bin'
python -B tests/validation/run_robotics_gpu_smoke.py --build-dir $build --config Release `
    --output out/robotics-gpu-smoke
```

The smoke runner enables the camera and synchronized-dataset regression tests,
requires both to execute, and saves JUnit, the combined CTest log, and copied
native capture evidence in the fresh output directory. The tests run the
actual renderer with synchronization validation. They check metric depth,
occlusion, exact labels, fixed-tick alignment, and portable replay. The
hosted CPU jobs run the smoke runner's automated tests.

Add `--include-physics-demo` and use a fresh output path to also check the
Jolt demo in the Deferred and Forward raster techniques with raster shadows
and with ray shadows plus TAA. This mode requires ray-query support, Pillow,
NumPy, and the staged Cornell validation fixture. It checks falling and
settling bodies, render transforms, shadow coverage, and unload cleanup.
