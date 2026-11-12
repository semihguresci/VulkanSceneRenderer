# Display grading and image export

Scene Controls keeps the existing layout. In **Post-process**, choose ACES,
Reinhard, Clipping, Filmic, Uncharted 2, AgX, or Khronos PBR Neutral. Saturation
and contrast are directly visible. Expand **Color Grading** for white balance,
tint, vibrance, shadow/highlight colors, reset, and preset save/load. Defaults
preserve the previous ACES output.

Expand **Export Image** and enter an output path without an extension:

| Action | Output |
| --- | --- |
| Scene EXR | Floating-point RGB scene radiance in linear sRGB, before bloom, exposure, grading and editor overlays. Requires Lit display mode. |
| Viewport PNG | Displayed image with tone mapping and grading, excluding the editor UI and post-process overlays. Scene diagnostics such as selection outlines can still appear. |
| Window PNG | Displayed image including enabled editor overlays. |

Each image has a `.capture.json` sidecar recording dimensions, color space,
source, display settings, and whether UI is included. Transparent raster
surfaces are composited before EXR readback. The temporal resolve discards prior
color when switching between the editor view and EXR capture so selection
outlines stay out of raw exports. Export errors appear in Scene Controls, and
interactive errors leave the application running.

Both raster renderers and the progressive path-tracing viewport use the same
display and capture path. Path-traced EXR metadata identifies its source as
`path-trace-mean` and includes sample and diagnostic counts.
An analytic shadow matte exports as `path-trace-shadow-matte` with
`linear-mask` values for compositing. The sidecar records ground mode and
the effective firefly clamp.

Command-line examples:

```text
VulkanSceneRenderer.exe --tone-map agx --white-balance 5100 --vibrance .5
VulkanSceneRenderer.exe --tone-map neutral --save-display-grade grade.json
VulkanSceneRenderer.exe --display-grade grade.json
VulkanSceneRenderer.exe --display-mode lit --capture-frame 9 --screenshot scene.exr
VulkanSceneRenderer.exe --capture-frame 9 --screenshot-viewport --screenshot viewport.png
VulkanSceneRenderer.exe --render-technique path-tracing --path-spp 8 --path-sample-series 4,16,64 --screenshot reference.exr
```

`--path-sample-series` exports raw EXR checkpoints from one accumulation with
playback held at the starting pose. Give up to 64 strictly increasing counts
from 1 to 16,777,216. The last count becomes the sample limit. In the example,
the first image is `reference.samples-00000004.exr`. Each checkpoint has a
capture sidecar with the exact requested and accepted samples per pixel, the
seed and bounce limit, plus a telemetry file. The renderer shortens a batch
as needed to reach the requested count. It stops the series on an accumulation
reset or on unsupported or rejected samples at a checkpoint. It also stops if
it cannot publish a complete checkpoint. Earlier checkpoints remain available
for analysis. Use a fixed camera and scene, and omit `--path-samples`, `--capture-frame`,
`--warmup-frames`, `--capture-sequence`, and `--path-target-ms` for this mode.
The scene fixtures and independent target seeds for measuring convergence are
described in the [reference calibration guide](reference-calibration.md).

Presets contain all nine version 1 fields. Loading rejects duplicate or unknown
fields, unsupported versions, nested malformed data, oversized input, and values
outside the UI ranges. Saving validates before replacing an existing file and
uses atomic publication. Later command-line options override loaded presets.

Vulkan readback uses a storage buffer bound only for the capture frame, a
fragment-to-host memory dependency, and completion of the submitting frame's
fence before encoding. The regular descriptor is restored after completion.
Host and Slang push-constant layouts are paired at 128 bytes.

The native regression scripts require Python, NumPy, Pillow and, for EXR checks,
OpenCV with OpenEXR support. Run on a Vulkan device with validation layers:

```text
python tests/validation/display_grade_runtime_regression.py PATH_TO_EXE OUTPUT_DIRECTORY
python tests/validation/scene_linear_capture_runtime_regression.py PATH_TO_EXE OUTPUT_DIRECTORY
python tests/validation/scene_linear_selection_history_runtime_regression.py PATH_TO_EXE OUTPUT_DIRECTORY
python tests/validation/reference_sample_series_runtime_regression.py PATH_TO_EXE OUTPUT_DIRECTORY
```

They check analytic tone curves and emission, transparent coverage, preset
validation, raw-radiance independence, resize with TAA, and overlay exclusion on
both raster renderers, including selection outlines from earlier frames. The
reference series test checks exact checkpoints,
batch-size independence, GPU-timed adaptive batches, and rejection of diagnosed
paths. It also checks that blocked sidecars preserve earlier checkpoints and
existing outputs, and that a retry can publish the full checkpoint.

Adapted tone curves retain their permissive notices in
`tone-curves-Apache-2.0.txt`; Windows packages include that license.
