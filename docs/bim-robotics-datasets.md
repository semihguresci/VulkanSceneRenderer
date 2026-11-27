# Record synchronized BIM robotics datasets

`BimRoboticsDataset` combines an aligned RGB, depth and segmentation capture
with the exact `BimRoboticsSim` report used by the camera. The result carries
the base pose, camera intrinsics and pose, IMU and encoder samples, LiDAR scans,
and the BIM GUID and label mapping. The recorder and replay tool run on the
CPU without Jolt, a display or a Vulkan device. Producing the inputs uses the
physics-enabled simulator and the renderer's hidden Deferred capture.

## Produce matching inputs

Build the input producers and recorder from a configured Release build with
`ENABLE_PHYSICS=ON`. The commands use the checked-in doorway fixture and its
camera mount. Run them from the repository root in Developer PowerShell.

```powershell
cmake --build out/build/windows-release --config Release --target BimRoboticsSim VulkanSceneRenderer BimRoboticsDataset
$scenario = Get-Content -Raw tests/fixtures/robotics/doorway-drive.json | ConvertFrom-Json -AsHashtable
$scenario.ticks = 60
$scenario.openDoorIds = @()
$scenario.driveSensors = @{ samplesPerSecondMilliHz = 20000 }
$scenario.lidar = @{
    scansPerSecondMilliHz = 7000
    azimuthSamples = 8
    elevationAngles = @(0)
    minRangeMeters = 0.05
    maxRangeMeters = 10
    mount = @{ xyz = @(0, 0, 0.7) }
}
$scenario | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 out/dataset-scenario.json
& './out/build/windows-release/BimRoboticsSim.exe' `
    tests/fixtures/robotics/doorway.ifc out/dataset-simulation.json doorway out/dataset-scenario.json
```

Use the `camera-replay.json` calibration from the camera guide. It captures
at 10 Hz on the 60 Hz simulation clock through tick 60. Capture RGB, depth
and labels from the same BIM input and use the same namespace in both
commands. [Camera calibration and replay](sensor-camera-datasets.md#replay-fixed-tick-base-poses)

```powershell
& './out/build/windows-release/VulkanSceneRenderer.exe' `
    --model tests/fixtures/robotics/doorway.ifc `
    --sensor-camera camera-replay.json --sensor-trajectory out/dataset-simulation.json `
    --sensor-output out/dataset-camera --sensor-namespace doorway `
    --render-technique deferred-raster --msaa 1 --no-taa
```

The recorder checks the exact simulation report bytes, the ordered BIM source
content and the physical length scale. Keep the simulation report unchanged
between camera capture and recording. Each tick describes the committed base
pose in building-local REP-103 metres. LiDAR uses the simulator collision scene.
The camera replays base poses against the imported BIM geometry.

## Record, verify and replay

```powershell
$recorder = './out/build/windows-release/BimRoboticsDataset.exe'
& $recorder record out/dataset-camera out/dataset-simulation.json out/doorway-dataset --ticks-per-chunk 16
& $recorder verify out/doorway-dataset
& $recorder replay out/doorway-dataset
```

`record` requires a fresh destination. `verify` checks the completed bundle
and prints its manifest. `replay` emits one validated JSON line per tick to
standard output. Check its exit status before accepting a replay, since an
error in a later chunk can follow rows from earlier chunks. Each camera row
includes a relative image name resolved against the dataset directory, so
the directory can be moved as a unit.

## Version 1 layout

- `dataset.json` is the versioned completion marker. It records the clock,
  stream counts and calibration, ordered chunk checksums, the BIM source
  identity, and the SHA-256 of the simulation report.
- `chunk-00000000.jsonl` and subsequent chunks hold one row per committed
  simulation tick. Every row has `tick`, `seconds` and `base`. The `camera`,
  `imu` and `lidar` entries contain samples from that tick or `null` when the
  corresponding schedule has no sample. `base` retains the simulator's full
  committed state, including its `base.position` and `base.yaw` fields.
- `frame-00000000.exr` and subsequent images retain the original scene-linear
  FLOAT RGB and axial depth in metres, plus exact UINT instance and semantic
  IDs. The camera row retains the source image digest, intrinsics, metric
  `worldFromCamera`, and the original timing and noise settings.
- `labels.json` maps the EXR IDs to their BIM occurrence, authored GUID, and
  semantic class. The manifest records its checksum. LiDAR returns retain
  their original entity identifiers.

Chunks contain 64 ticks by default. `--ticks-per-chunk` accepts 1 through
1024, and the final chunk can be shorter. The recorder streams the input report
and spools observations to disk. It processes one observation from each
stream at a time, and it copies images as files. The reader verifies chunks
and EXRs incrementally and retains only the manifests and the current row.
A directory without `dataset.json` is incomplete. Version 1 accepts up to
8192 committed ticks and 1024 camera frames, consistent with the camera
playback limits.
