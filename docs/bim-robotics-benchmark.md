# Benchmark large BIM robotics workloads

The benchmark drives a differential-drive robot through two BIM scenes. It
cooks and installs collision geometry, runs fixed physics ticks and LiDAR,
captures calibrated RGB, metric depth and BIM IDs through Vulkan, then writes
and verifies a synchronized dataset.

The first scene uses the same Tekla House STEP IFC as the BIM import and
large-scene renderer benchmarks. The second generates a 32×32 grid of IFC
rooms with 12,288 uniquely identified products and five shared source
geometries. This contrasts a detailed authored building with a scene that
stresses occurrence count and shape reuse. [BIM import performance](bim-performance.md)

## Build and run

In Developer PowerShell, use a Release build with physics enabled, a GPU
with Vulkan support, and Python 3.11 or newer. Build the three native
programs in the same CMake directory.

```powershell
cmake --preset windows-release -DENABLE_PHYSICS=ON
cmake --build out/build/windows-release --target BimRoboticsSim VulkanSceneRenderer BimRoboticsDataset
python -B tools/bim_robotics_benchmark.py --build-dir out/build/windows-release `
    --tekla-ifc 'C:/Models/TeklaHouse.ifc' --output out/bim-robotics-benchmark
```

Use `TeklaHouse.ifc` from the pinned buildingSMART revision
`1a63082ada967c683cfacee2005f8f749c8e1b79`. The runner checks the file's
SHA-256. Omit `--tekla-ifc` if the pinned file is already present under the
build directory's `models/buildingSMART-IFC5-development/examples/Tekla House/`.
The external building sample stays in its original location.
[Tekla House IFC source](https://github.com/buildingSMART/IFC5-development/blob/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/Tekla%20House/TeklaHouse.ifc)

The default run starts three fresh processes per scene and operation. It runs
60 physics ticks at 60 Hz and collects 11 camera frames and 11 scans from
tick 0 through tick 60. Each scan casts 768 beams. Captures use 640×480
Lit Deferred raster with native resolution, 1× MSAA, and disabled TAA, ray
queries, and DDGI. Both runs use a fixed, supported drive path and check that
the robot moved without an obstruction. Use a fresh output directory for each
run. `--synthetic-grid`, `--ticks`, `--width`, `--height`, and
`--iterations` control workload size.

## Read the report

`benchmark.json` records the host and Vulkan device, driver and build
identity, source and executable SHA-256 checksums, complete invocations and
measured samples. Every iteration writes its camera calibration, simulation
scenario, trajectory, frame capture, synchronized dataset and process logs
under the output directory.

| Measurement | Boundaries |
| --- | --- |
| CPU preparation | Source read and fingerprinting, IFC import, semantic scene generation, collision cooking, walkable surfaces and live collision installation are measured separately with a steady clock. Both preprocessing caches are disabled. OS file caching is uncontrolled. |
| Physics and LiDAR | Fixed-tick wall time excludes sensor observation callbacks. LiDAR measures complete scan sampling and live collision-world queries before JSON conversion. |
| RGB readback | GPU timestamps include the scene-linear post-process shader, its draw and the RGB host visibility barrier. The shader writes directly to mapped storage. |
| Depth and labels readback | GPU timestamps include depth layout transitions, the metric depth and ID compute pass, and the host visibility barrier. |
| CPU capture work | RGB and depth buffer invalidation, EXR encoding, writes, and the final in-flight fence wait are reported separately. |
| Memory | Process memory reports the largest kernel high-water mark observed during each process on Windows or Linux. Renderer VMA reports live allocation and device-local bytes at capture completion. |
| Dataset throughput | The recorded dataset size divided by recorder wall time includes process startup, validation, and disk writes. Verification is timed separately and must confirm the completed dataset. |

The first frame is included in the recorded GPU and host samples. Missing GPU
timestamps or process high-water marks are represented by nulls, and the
report counts the available samples. Inspect the device information before
comparing runs. The report gives medians and nearest-rank 95th percentiles.
Disk cache, GPU driver, power policy and measurement collection can affect
results. This benchmark measures one headless simulation worker and one
camera stream at a time. The renderer scale work documents interactive
residency and upload budgeting separately. [Scene-scale tracking issue](https://github.com/semihguresci/VulkanSceneRenderer/issues/45)

For a standalone simulation or capture command, append
`--benchmark TIMINGS.json` to `BimRoboticsSim` or
`--sensor-benchmark TIMINGS.json` to the renderer's sensor-camera command.
The optional timing sidecars preserve the existing trajectory and camera
dataset formats. The native simulator marks cache-skipped preprocessing
stages with null timings.
[Synchronized dataset recording](bim-robotics-datasets.md)

## Windows reference run

Windows 11, AMD Ryzen 9 3900X, 32 GiB RAM, and an NVIDIA GeForce RTX 2080 SUPER
with driver 610.88. A native MSVC Release build completed and verified all six
datasets. Each scene produced 33 paired GPU timing samples across three runs.
[Hardware-stamped report](../tests/benchmarks/results/bim-robotics-windows.json)

| Median | Tekla House IFC | Synthetic 32×32 |
| --- | ---: | ---: |
| IFC import | 3.90 s | 0.26 s |
| Collision cook | 69.37 ms | 6.96 ms |
| Mean physics tick | 0.068 ms | 0.106 ms |
| Mean LiDAR scan | 6.36 ms | 11.24 ms |
| GPU RGB readback scope | 0.51 ms | 0.51 ms |
| GPU depth and labels readback scope | 33.61 ms | 33.11 ms |
| Peak renderer process residency | 5,625 MiB | 568 MiB |
| Dataset recording throughput | 60.5 MiB/s | 60.7 MiB/s |
