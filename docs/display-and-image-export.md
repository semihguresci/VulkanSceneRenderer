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
| Viewport PNG | Displayed image with tone mapping and grading, excluding editor overlays. |
| Window PNG | Displayed image including enabled editor overlays. |

Each image has a `.capture.json` sidecar recording dimensions, color space,
source, display settings, and whether UI is included. Transparent raster
surfaces are composited before EXR readback. Export errors appear in Scene
Controls; interactive errors leave the application running.

Both published raster renderers use the same display and capture path. The
progressive Vulkan path-tracing producer is available separately; integrating
the complete path-tracing viewport remains a later publication step. When that
viewport is active in the development editor, EXR metadata identifies its source
as `path-trace-mean` and includes sample and diagnostic counts.

Command-line examples:

```text
VulkanSceneRenderer.exe --tone-map agx --white-balance 5100 --vibrance .5
VulkanSceneRenderer.exe --tone-map neutral --save-display-grade grade.json
VulkanSceneRenderer.exe --display-grade grade.json
VulkanSceneRenderer.exe --display-mode lit --capture-frame 9 --screenshot scene.exr
VulkanSceneRenderer.exe --capture-frame 9 --screenshot-viewport --screenshot viewport.png
```

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
```

They check analytic tone curves and emission, transparent coverage, preset
validation, raw-radiance independence, resize with TAA, and overlay exclusion on
both raster renderers. The separate `reference_capture_metadata_runtime_regression.py`
requires the development editor's integrated path-tracing viewport.

Adapted tone curves retain their permissive notices in
`tone-curves-Apache-2.0.txt`; Windows packages include that license.
