# BIM robotics preview builds and evidence

The preview runs two doorway workflows from the same BIM source. Start with
the package for your platform and follow the matching quickstart. [BIM robotics preview tutorial](bim-robotics-preview.md)

| Platform | Package | Runtime and result |
| --- | --- | --- |
| Windows 10 or 11 x64 | Robotics preview ZIP | Vulkan 1.4 GPU and driver, x64 Microsoft Visual C++ v14 Redistributable, and Python 3.10 or newer. The app records scene-linear RGB, axial depth in metres, BIM instance and semantic IDs, and synchronized motion and sensor samples. Dataset verification and replay run on the CPU. |
| Ubuntu 24.04 x64, including WSL 2 | Nav2 preview tarball | CPU-only tools and a bundled bridge built with Colcon against system ROS 2 Jazzy and Nav2. The demo exports a floor map, drives the simulated robot, and verifies the MCAP replay and the physical path. |

Both packages use the project-authored MIT-licensed IFC fixtures. Keep the
included third-party notices and the package directory structure when you
extract an archive. [Preview assets and limits](bim-robotics-preview.md#limits-and-asset-licenses)

## Download a workflow build

Open a successful **Robotics Physics Validation** run that was dispatched
with a preview version. In **Artifacts**, download the package for your
platform.

- `robotics-preview-windows-x64` contains the Windows ZIP.
- `robotics-preview-linux-x64` contains the Ubuntu Nav2 tarball.

Use downloads from the same run for matching source revisions. Sign in to
GitHub to download the artifacts before they expire.
[Workflow runs](https://github.com/semihguresci/VulkanSceneRenderer/actions/workflows/robotics-physics-validation.yml) · [GitHub artifact downloads](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/download-workflow-artifacts)

With repository write access, use **Run workflow** on the intended ref. Set
`preview_version` to a three-part version with an optional `v` prefix and
prerelease suffix, such as `v0.3.0-preview.1`.
[Run a workflow manually](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)

Each workflow artifact contains the package, its `.sha256` checksum file,
and `source-commit.txt`. Check the source commit against the run and verify
the checksum before extracting the package. Use `Get-FileHash -Algorithm SHA256`
on Windows or `sha256sum` on Ubuntu. The packaged `build-info.json` lists the
staged payload fingerprints. [Build and package from source](bim-robotics-preview.md#build-from-source)

## Reproduce the demonstrations

| Demonstration | What to verify | Recipe and evidence |
| --- | --- | --- |
| Windows doorway dataset | The example records eight committed ticks, four Vulkan RGB-D frames, three IMU and encoder observations, and two LiDAR scans. `summary.json` confirms the verified dataset and its aligned replay. | [Dataset quickstart](bim-robotics-preview.md#record-a-dataset-on-windows) · [Extracted ZIP validation](https://github.com/semihguresci/VulkanSceneRenderer/pull/378) |
| Ubuntu doorway navigation | The local relocated Jazzy run reached both waypoints within 0.116 m, passed 594 native capsule sweeps, and reproduced 226 RGB-D frames with matching MCAP payloads. See `preview-summary.json` in the run output. | [Nav2 quickstart](bim-robotics-preview.md#drive-through-the-doorway-on-ubuntu) · [Hosted archive validation](https://github.com/semihguresci/VulkanSceneRenderer/actions/runs/46137954643) · [Local run results](https://github.com/semihguresci/VulkanSceneRenderer/pull/378) |
| Tekla House inspection from source | The robot drives a 2 m route and stops at an authored concrete column. The demonstration validates 300 moving segments, checks the column in LiDAR and RGB-D, and exports a portable OpenUSD replay. The tutorial pins the external IFC and offers a CPU-only run. | [Inspection and replay tutorial](bim-tekla-robotics-demo.md) · [Demonstration validation](https://github.com/semihguresci/VulkanSceneRenderer/pull/380) |

## Measured performance

The Windows benchmark ran on a Ryzen 9 3900X and an RTX 2080 SUPER with
driver 610.88. It verified six datasets and collected 33 paired GPU timestamp
samples per scene. These medians describe the reference workloads. [Benchmark instructions](bim-robotics-benchmark.md) · [Hardware-stamped samples](../tests/benchmarks/results/bim-robotics-windows.json)

| Median | Tekla House IFC | Synthetic IFC with 12,288 products |
| --- | ---: | ---: |
| IFC import | 3.90 s | 0.26 s |
| Physics tick | 0.068 ms | 0.106 ms |
| LiDAR scan | 6.36 ms | 11.24 ms |
| GPU depth and label readback | 33.61 ms | 33.11 ms |
| Peak renderer process residency | 5,625 MiB | 568 MiB |
| Dataset recording throughput | 60.5 MiB/s | 60.7 MiB/s |

Jazzy Nav2 completed all 12 held-out navigation trials with zero collisions.
The untrained policy completed 6 of 12 with six collisions. After 24,000
training transitions per seed, it completed 4 of 12 with one collision.
The Windows learned-only run reproduced the 48 held-out outcome records, and
the full Jazzy rerun reproduced all 12 frozen checkpoints byte for byte.
[Navigation benchmark recipe](bim-navigation-policy-benchmark.md) · [Checked results](../tests/benchmarks/results/bim-navigation-policy-jazzy.json) · [Rerun evidence](https://github.com/semihguresci/VulkanSceneRenderer/pull/381)

## Regression coverage

- The C++23 targets separate scene import, collision, fixed-step simulation,
  sensors, and replay. They carry metre-based REP-103 poses and stable IFC
  identities. Adapters connect the native tools to ROS and training libraries.
  [Architecture decision and coordinate contracts](adr/0001-robotics-module-boundaries.md)
- The four hosted Windows Release profiles cover the independent Jolt and
  URDF options. The checked-in IFC and robot fixtures cover fixed-tick replay,
  metric depth, labels, contacts, and an articulated LiDAR golden with
  independently computed poses and ranges. The workflow retains test
  inventories and JUnit results. [Validation profiles and fixtures](robotics-validation.md) · [Completed hosted validation](https://github.com/semihguresci/VulkanSceneRenderer/actions/runs/46137954643)
- The preview workflow packages and checks the Windows archive on a CPU
  runner and runs the relocated Jazzy archive in the hosted Linux job.
  Local Windows GPU tests exercise camera capture, synchronized dataset
  replay, and the Forward and Deferred physics demo with Khronos validation
  on an RTX 2080 SUPER.
  [Hosted Jazzy and physical checks](https://github.com/semihguresci/VulkanSceneRenderer/actions/runs/46137954643) · [Local GPU validation](https://github.com/semihguresci/VulkanSceneRenderer/pull/359)
