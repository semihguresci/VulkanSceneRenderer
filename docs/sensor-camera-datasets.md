# Capture calibrated BIM camera datasets

`VulkanSceneRenderer --sensor-camera` records aligned colour, depth and labels
from one IFC, IFCX or BIM model. It renders through a hidden window without
editor UI and exits after the scheduled captures. A Vulkan-capable GPU is
required. The capture route uses Lit Deferred raster, native resolution,
1× MSAA and disabled TAA. Depth and labels cover opaque and alpha-mask
surfaces. Alpha-blended surfaces are unsupported.

Run the examples from the repository root in Developer PowerShell with a
current build:

```powershell
cmake --build out/build/windows-release --config Release --target VulkanSceneRenderer
```

## Capture one static view

Save this as `camera-static.json`:

```json
{
  "schema": "container.sensor-camera",
  "version": 1,
  "intrinsics": {
    "width": 640,
    "height": 480,
    "fx": 500,
    "fy": 500,
    "cx": 320,
    "cy": 240,
    "nearMeters": 0.05,
    "farMeters": 15
  },
  "worldFromBase": { "xyz": [-1.5, 0, 1.2], "rpy": [0, 0, 0] },
  "baseFromCamera": { "xyz": [0, 0, 0.7] },
  "timing": {
    "ticksPerSecond": 60,
    "framesPerSecondMilliHz": 10000,
    "firstTick": 0,
    "lastTick": 0
  }
}
```

Capture the checked-in millimetre IFC doorway fixture:

```powershell
& '.\out\build\windows-release\VulkanSceneRenderer.exe' `
    --model tests/fixtures/robotics/doorway.ifc `
    --sensor-camera camera-static.json `
    --sensor-output out/doorway-camera-static `
    --sensor-namespace doorway `
    --render-technique deferred-raster --msaa 1 --no-taa
```

The base is 1.5 m before the doorway, on its 1.2 m floor. The camera is
mounted 0.7 m above that base and faces base +X by default. The output
directory must be new, including for a repeated capture.

`fx` and `fy` are focal lengths in pixels. `cx` and `cy` are principal-point
coordinates measured from the top-left image edge. Pixel centres are
`(column + 0.5, row + 0.5)`. Near and far distances are positive optical-axis
distances in metres. Positions use the building-local REP-103 frame: +X
forward, +Y left, +Z up. `rpy` angles use radians and rotation order
`Rz(yaw) × Ry(pitch) × Rx(roll)`.

Camera coordinates use +X right, +Y up and −Z forward. The default mount maps
camera right to base −Y, camera up to base +Z and camera forward to base +X.
Optional `baseFromCamera.rpy` rotates this default orientation about the base
axes. Exported `worldFromCamera` matrices retain these camera axes and use
column-major layout. See the [coordinate conventions](coordinate-conventions.md#semantic-bim-export-for-robotics).

## Replay fixed-tick base poses

With `ENABLE_PHYSICS=ON`, generate a trajectory using `BimRoboticsSim`:

```powershell
cmake --build out/build/windows-release --config Release --target BimRoboticsSim
& '.\out\build\windows-release\BimRoboticsSim.exe' `
    tests/fixtures/robotics/doorway.ifc out/doorway-trajectory.json doorway `
    tests/fixtures/robotics/doorway-drive.json
```

Save a second camera spec as `camera-replay.json`, using the same intrinsics
and mount as above. Remove `worldFromBase` and replace `timing` with:

```json
"timing": {
  "ticksPerSecond": 60,
  "framesPerSecondMilliHz": 10000,
  "firstTick": 0,
  "lastTick": 60
}
```

```powershell
& '.\out\build\windows-release\VulkanSceneRenderer.exe' `
    --model tests/fixtures/robotics/doorway.ifc `
    --sensor-camera camera-replay.json `
    --sensor-trajectory out/doorway-trajectory.json `
    --sensor-output out/doorway-camera-replay `
    --sensor-namespace doorway `
    --render-technique deferred-raster --msaa 1 --no-taa
```

This records 11 views at ticks 0, 6, …, 60, with timestamps 0, 0.1, …, 1 s.
The example stops before the camera crosses the rendered door. Trajectory
replay consumes committed base positions and yaw. The BIM geometry remains
the imported snapshot. Door state changes and other physics motion in a
simulation report are not replayed into the rendered scene.

The trajectory must be a version-1 `container.bim-robotics-simulation` report
with contiguous history starting at tick zero, REP-103 coordinates and times
matching `tick / ticksPerSecond`. The camera tick rate must match the report.
A supplied trajectory and `worldFromBase` are mutually exclusive.

`framesPerSecondMilliHz: 10000` means 10 Hz. The first capture occurs at
`firstTick`. Each subsequent capture uses the first fixed tick at or after
its ideal time. Timestamps come from that tick, independently of render or
file-write duration. Without a trajectory, omitted timing captures one static
frame at tick zero. With a trajectory, omitted timing uses its tick rate and
history extent, at up to 10 Hz. Explicit rates can reach one frame per tick.

## Read the output

Each sample index has two files, starting with `frame-00000000.exr` and
`frame-00000000.json`. The EXR channels share one top-left pixel grid:

| Channel | EXR type | Meaning |
| --- | --- | --- |
| `R`, `G`, `B` | 32-bit `FLOAT` | Scene-linear RGB in linear-sRGB primaries. HDR values are retained. |
| `Z` | 32-bit `FLOAT` | Positive axial depth, `-camera.z`, in metres. Zero means background or invalid depth. |
| `InstanceID` | 32-bit `UINT` | Exact instance label. Zero means background or invalid depth. |
| `SemanticID` | 32-bit `UINT` | Exact semantic class label. Zero means background or invalid depth. |

Read the label channels as integers to preserve every ID. `labels.json`, with
schema `container.bim-sensor-labels` version 1, maps instances to semantic
entities, authored GUIDs, source occurrences and representations. Its class
table maps semantic IDs to IFC classes, including `UNCLASSIFIED` for class 1.
Numeric IDs belong to the asset snapshot. Resolve their meaning through this
manifest. `--sensor-namespace` defaults to the model's filename stem.

Each version-1 `container.sensor-camera-frame` JSON records tick, seconds,
sample index, intrinsics, metric `worldFromCamera`, channel conventions,
distortion and noise settings, and invalid-pixel counters. Labels and frame
metadata are independent of the image encoding.

Files are written atomically without replacing existing files. `dataset.json`
is written after all scheduled images and metadata have completed. Its
version-1 `container.sensor-camera-dataset` record gives the frame count,
filename patterns, label manifest, colour space and surface policy. Treat this
file as the completion marker. A directory left without it is incomplete.
Choose a fresh destination for another run.

## Add lens distortion and depth noise

These optional top-level camera-spec fields apply after the aligned ideal
capture:

```json
"distortion": { "k1": -0.08, "k2": 0.01, "p1": 0.001, "p2": 0, "k3": 0 },
"depthNoise": { "stdDevMeters": 0.002, "seed": 17 }
```

Brown–Conrady coefficients use normalized image coordinates with +X right and
+Y down. The exporter pulls each distorted output centre back into the ideal
image. RGB uses bilinear interpolation. Depth and both labels use the same
nearest source pixel. Out-of-image, singular or locally folded mappings write
zero to all channels.

Depth noise is independent Gaussian error in metres, deterministically keyed
by seed, sample index and output pixel. Depth outside the inclusive
`[nearMeters, farMeters]` window becomes zero together with both labels. RGB
is retained. Background depth stays zero. Zero noise and omitted distortion
preserve the ideal channels. Reusing a seed reproduces the CPU noise samples.
GPU colour can vary across drivers or builds.

Declared native IFC length units supply the metric scale automatically. For
IFCX or BIM input without declared metric units, pass
`--sensor-meters-per-renderer-unit SCALE` with a finite positive value. This
value states how many metres one renderer unit represents, including any
`--import-scale` applied to the geometry. The label manifest records the
effective scale used for camera placement and depth conversion.

The spec permits dimensions from 1 to 8192 per axis, at most 8,388,608 pixels,
and at most 1024 captures through tick 8191. Device readback limits can reduce
the usable image size. Capture requires drawable BIM geometry with declared
metric scale or an explicit unit override, and valid labels for visible surfaces. Screenshot, diagnostic
capture, scene export and other capture-sequence modes are mutually exclusive
with this dataset mode.
