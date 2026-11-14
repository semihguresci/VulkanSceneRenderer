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
| Viewport PNG | Displayed image with tone mapping and grading, excluding the editor UI and its gizmos. Scene diagnostics such as selection outlines and enabled BIM floor-plan linework can still appear. |
| Window PNG | Displayed image including enabled editor overlays. |

Each image has a `.capture.json` sidecar recording dimensions, color space,
source, display settings, and whether UI is included. Transparent raster
surfaces contribute to scene HDR before bloom, auto exposure, and EXR readback
with or without TAA. The temporal resolve discards prior
color when switching between the editor view and EXR capture so selection
outlines stay out of raw exports. Export errors appear in Scene Controls, and
interactive errors leave the application running.

Both raster renderers and the progressive path-tracing viewport use the same
display and capture path. Path-traced EXR metadata identifies its source as
`path-trace-mean` and includes sample and diagnostic counts.
An analytic shadow matte exports as `path-trace-shadow-matte` with
`linear-mask` values for compositing. The sidecar records ground mode and
the effective firefly clamp.

In Deferred raster, Forward raster, and Path tracing, the BIM floor plan appears in display
PNGs and stays out of raw EXR. Path tracing keeps its accumulated radiance
when floor-plan settings change. [BIM floor-plan overlay](reference-path-tracing.md#bim-floor-plan-overlay)

Command-line examples:

```text
VulkanSceneRenderer.exe --tone-map agx --white-balance 5100 --vibrance .5
VulkanSceneRenderer.exe --tone-map neutral --save-display-grade grade.json
VulkanSceneRenderer.exe --display-grade grade.json
VulkanSceneRenderer.exe --display-mode lit --capture-frame 9 --screenshot scene.exr
VulkanSceneRenderer.exe --capture-frame 9 --screenshot-viewport --screenshot viewport.png
VulkanSceneRenderer.exe --auto-exposure --capture-frame 32 --screenshot viewport.png
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

On an interactive Windows desktop, the opt-in visibility test compares PNG and
EXR output with the renderer window visible, half covered, and hidden. It uses
the local renderer MCP endpoint to request captures after startup.

```powershell
$env:CONTAINER_RUN_GPU_CAPTURE_VISIBILITY = '1'
ctest --test-dir out/build/windows-release -C Release -R '^scene_linear_capture_visibility_gpu_regression$' --output-on-failure
```

The separate Windows clipping regression uses a test-only Vulkan layer. It
checks the clipping flag at startup and after resize, then emulates a partly
or fully clipped drawable when the test forces `clipped = VK_TRUE`. The
forced controls detect missing PNG pixels and stale EXR values. An application
that requests `VK_FALSE` must preserve both PNG scopes and every EXR pixel. The
desktop test above exercises actual window visibility on the installed driver.

```powershell
cmake --build out/build/windows-release --config Release --target swapchain_capture_clip_layer VulkanSceneRenderer
$env:CONTAINER_RUN_GPU_CAPTURE_CLIPPING = '1'
ctest --test-dir out/build/windows-release -C Release -R '^swapchain_capture_clipping_gpu_regression$' --output-on-failure
```

The Windows RTX 2080 SUPER run passed 81 captures with Vulkan synchronization
validation. Scene-linear EXRs matched clean controls in both raster renderers
with TAA, FSR and DLSS. The 32-, 128- and 512-sample Cornell EXRs matched the
accepted convergence baseline byte for byte. [Windows Vulkan results](../tests/visual-regression/scene-linear-export/windows-nvidia/scene-linear-export-results.json)

Adapted tone curves retain their permissive notices in
`tone-curves-Apache-2.0.txt`; Windows packages include that license.

## Render a queue of stills and turntables

Save the scenes using the editor's **Save As** command. Create a JSON job
list like the example below. Paths are relative to the list's directory.
`cameraNode` is the glTF source node index of a camera in the saved default
scene. An orbit uses a world-space target, a horizontal radius, and a
vertical offset from the target, in scene units with +Y up.

```json
{
  "schemaVersion": 1,
  "jobs": [
    {
      "name": "Interior still",
      "scene": "interior.glb",
      "renderer": "path-tracing",
      "width": 1280,
      "height": 720,
      "output": "renders/interior.exr",
      "cameraNode": 4,
      "sampling": { "samples": 64, "samplesPerFrame": 8, "bounces": 6, "seed": 1 }
    },
    {
      "name": "Turntable",
      "scene": "interior.glb",
      "renderer": "forward-raster",
      "width": 960,
      "height": 540,
      "output": "renders/orbit.png",
      "orbit": { "target": [0, 1, 0], "radius": 6, "height": 2, "frames": 24 }
    }
  ]
}
```

From the repository root, use Python 3.10 or newer and a built renderer.
The capture runner needs a desktop session and Vulkan. Add `--validation`
to enable the Vulkan validation layer.

```powershell
python tools/render_queue.py validate jobs.json
python tools/render_queue.py run --renderer out/build/windows-release/VulkanSceneRenderer.exe jobs.json
```

Use `validate --save-as another-folder/jobs.json` to save a copy with paths
that still point to the original scenes and destinations. Keep the source
scenes and external assets available when reopening the list.

Choose `forward-raster`, `deferred-raster`, or `path-tracing` for either
output format. Raster jobs default to one sample, TAA off, and no warm-up.
Set `sampling.msaa`, `sampling.taa`, or `sampling.warmupFrames` to change
those settings. TAA requires 1× MSAA. Path-tracing jobs default to 64 samples
per pixel, four per frame, eight bounces, and a seed of one. Each view
starts a fresh accumulation. Orbit `fov` defaults to 45 vertical degrees.

Turntables write `orbit.0001.png`, `orbit.0002.png`, and so on, with one
view per angle and no duplicate closing frame. Each image has the standard
capture and telemetry sidecars. The queue checks every planned image and
sidecar before rendering. Jobs with existing or duplicate destinations
fail while the other jobs can run. Ctrl+C cancels the active job and keeps
completed frames. The queue then advances to the next job and returns 130
when it finishes. For a retry, choose an unused destination or move the
completed frames and their sidecars before rendering that job again.
