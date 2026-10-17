# Model loading and UI regression tests

Scene Controls replaces the current model when Load model or a sample model is
selected. Unload model clears both primary glTF and auxiliary BIM/USD content.
File decoding and BIM floor-plan preparation run on a serial background worker;
the current scene remains interactive while those CPU jobs run. New requests
replace queued requests, and Unload cancels a pending load without waiting for
its importer or allowing that model to appear later. GPU installation and
unload cleanup run on the render thread with a live loading view and window
event checkpoints. Uploads wait on their own fences rather than idling the
whole graphics queue. Discarded CPU payloads are destroyed on the worker.
The startup `--bim-model` option is applied at startup; subsequent UI replacements
do not reload that sidecar. Successful replacements clear model selection,
inspection filters, imported light edits and GI history. Malformed or missing
glTF/BIM inputs are decoded before live assets are retired, preserving the
current scene, camera and lighting when parsing fails.
Primary geometry arenas reuse their existing capacity after the previous GPU
readers retire, preventing repeated reloads from growing their allocations.

Scene Controls stays inside the viewport work area when a saved window size or
position exceeds the screen. The scene hierarchy, sample-model catalog and BIM
element browser have bounded scrolling. The element browser supports search,
selection and camera focus for every imported object, including objects absent
from the primary scene graph. BIM relationship and spatial tables expose all
rows rather than truncating the displayed data.

BIM schedules and comparison tables are cached by model identity, geometry
revision and metadata storage. Reload and unload invalidate the cache; unchanged
frames reuse the owning table data. This avoids rebuilding static string/map
aggregations for every imported element while the inspection panels are closed.
The Tekla House performance measurements and remaining work are documented in
[BIM performance](bim-performance.md).
Static category counts, bounds, provider triangle batches and unchanged filtered
LOD totals are also cached; their live-field and lifetime contracts are documented
in [BIM frame caching](bim-frame-cache.md).

Run the CPU UI and capture tests after a Release build:

```powershell
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^(async_model_preparation_tests|materialx_integration_tests|bim_relationship_graph_tests|bim_inspection_cache_tests|bim_statistics_cache_tests|scene_provider_extraction_tests|renderer_telemetry_tests|technique_debug_model_tests|temporal_capture_tests|editable_light_tests|gui_model_loading_harness_tests)$'
```

The UI tests run real ImGui frames with generated catalogs and hierarchies,
scroll to their last entries, and simulate item selection without a Vulkan
window. Capture parser tests check one-shot unload events and reject conflicting
model actions. Python harness tests verify that stale images, missing telemetry,
runtime errors and failed assertions cannot produce a passing report.
The Python capture and harness tests require NumPy and Pillow.

Run the native model suites on a Vulkan-capable Windows desktop:

```powershell
$env:CONTAINER_RUN_GPU_GUI_MODEL_LOADING = '1'
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^gui_model_(loading|lifecycle)_gpu_regression$'
```

Both suites use the same deferred GUI request methods as the widgets, with the
application's ImGui frame lifecycle active. They do not automate OS mouse clicks.
The legacy suite covers discovered IFC/IFCX sample selection and startup versus
UI rendering. The lifecycle suite covers repeated glTF/BIM loads, failed-load
recovery, idempotent unload and mixed startup replacement in forward and deferred
rendering, with raster fallback, hard ray shadows plus TAA, and asynchronous DDGI
plus TAA. It checks geometry and hierarchy counts, selection, camera, lighting,
temporal epochs, acceleration structures, GI state and stable geometry arena
capacity. Synchronization validation must be clean, and each expected image and
telemetry file must be newly produced.
Request telemetry also checks submitted preparation/progress frames and event
processing. Captures normally settle a model event without advancing the scene
script, continuing to render the loading view while waiting. Setting
`guiWaitForLoad: false` leaves that GUI event asynchronous; the cancellation cases unload it on
the following frame and verify that its discarded model never becomes active.
The worker's CPU tests gate real threads with promises, covering nonblocking
polling, superseded requests, failures, unload cancellation and worker-thread
payload destruction.

For a standalone run with evidence in an ignored output directory:

```powershell
python -B tests/validation/gui_model_loading_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --suite all --output out/model-lifecycle-validation
```

GPU suites are opt-in and report CTest skip code 77 when not enabled. The
application's packaged `models` directory must contain the validation scenes and
buildingSMART samples. Asynchronous GI may use its reported serialized fallback
on devices without a suitable compute queue.
