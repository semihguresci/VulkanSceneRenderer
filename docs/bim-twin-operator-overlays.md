# Inspect live operator overlays in BIM

The BIM Twin Replay panel shows planned and observed paths, poses, camera
frusta, occupied cells and operator alarms. Positions are in the building's
REP-103 frame in metres. The viewport applies the BIM import scale and model
translation when placing the annotations. [Coordinate conventions](coordinate-conventions.md#semantic-bim-export-for-robotics)

## Review the doorway fixture

Run from the repository root with the Windows Release build configured.

```powershell
$build = 'out/build/windows-release'
cmake --build $build --target BimRoboticsTwin VulkanSceneRenderer
$model = 'tests/fixtures/robotics/doorway.ifc'
$feed = 'tests/fixtures/robotics/doorway-operator.jsonl'
$snapshot = 'out/doorway-operator.json'
& "$build/BimRoboticsTwin.exe" ingest $model $feed $snapshot doorway doorway-demo
& "$build/VulkanSceneRenderer.exe" --model $model --import-scale 2.5 `
    --bim-twin-replay $snapshot --bim-twin-time-ns 1000000000 `
    --render-technique deferred-raster --display-mode lit `
    --camera-position 8 12 18 --camera-target 0 3.7 0 --camera-fov 34
```

At 1.000 s, the amber path and cyan trail show the plan and actual motion.
The warning triangle marks the doorway monitor's position, the blue wireframe
shows the camera's viewing volume, and the violet cells show occupancy. Scrub
to 1.450 s to see the warning clear. Switch to **Forward raster** in Scene
Controls to inspect the same overlays. A UI-inclusive PNG contains the panel
and annotations. A viewport-only PNG or scene-linear EXR contains the rendered
scene. Capture telemetry still records the sampled operator state and the
positions in renderer units for alignment checks.

- `operator-alarm` uses `alarmId`, `severity` (`info`, `warning`, or
  `critical`), `message`, `active`, and `position`. Each entity can report
  several independent alarms. An event with `active: false` clears the
  corresponding marker and remains visible in the timeline.
- `sensor-frustum` uses `width`, `height`, `fx`, `fy`, `cx`, `cy`,
  `nearMeters`, `farMeters`, and `worldFromCamera`. The four image edges
  define the pinhole frustum. The 16 transform entries are column-major
  and map camera axes +X right, +Y up and −Z forward to building metres.
  Frusta show the calibrated pinhole volume before lens distortion.
- `occupancy-grid` uses `origin`, `yaw`, `resolutionMeters`, `width`,
  `height`, and `cells`. Rows advance along local +Y and columns along
  +X after the grid rotates about building +Z by `yaw` radians. The origin
  is the corner of cell `(0, 0)`. Use `#` for occupied, `.` for free, and
  `?` for unknown. Each dimension is at most 128 cells.

The viewer hides stale spatial alarms, frusta, and occupancy while retaining
their timestamp, source and stale status in the panel. Drawing is bounded to
128 alarms, 128 frusta and 1,024 runs of adjacent occupied cells per frame.
Recordings with these operator events use version 3. JSONL uses the existing
version-1 event envelope. The three event kinds can also be added to
a simulated recording with `BimRoboticsTwin record --supplement`.
[Ingest robot telemetry](bim-twin-ingest.md) · [Add a replay supplement](bim-twin-replay.md#add-planned-paths-and-bim-asset-state)

## Follow updates from an adapter

Launch with `--bim-twin-replay $snapshot --bim-twin-follow` to watch the file
and start at its latest observation. **Follow latest** follows each accepted
snapshot's end time. Clear the checkbox to continue inspecting a previous
timestamp while updates arrive. Supplying `--bim-twin-time-ns` starts in this
review mode.

Publish a complete recording to a fresh path in the same directory, then
atomically replace the watched snapshot. Use the same source fingerprints,
metre scale, namespace and clock throughout the session. For example, after
ingesting the updated feed to `out/doorway-next.json`, use this command.

```powershell
python -c "import os; os.replace('out/doorway-next.json', 'out/doorway-operator.json')"
```

The renderer checks for a changed file revision twice a second and parses
updates on a background thread. If a snapshot is invalid, from another source
or clock, or ends earlier than the last accepted snapshot, the panel reports
the error and keeps the previous data. Freshness is measured against the
selected telemetry time. A live adapter supplies time advances in its own
clock domain.

## Verify the overlays and exports

Run the opt-in renderer regression with the Vulkan validation layer and
Python's Pillow, NumPy and OpenCV packages available. The test checks
Forward and Deferred screenshots, metre-to-renderer alignment, repeatable
overlay masks, alarm clearing and staleness. It compares clean PNG and
scene-linear EXR pixels against captures with replay disabled.

```powershell
$env:CONTAINER_RUN_GPU_BIM_TWIN = '1'
ctest --test-dir $build -C Release -R '^bim_twin_replay_gpu_regression$' --output-on-failure
```
