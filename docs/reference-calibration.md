# Reference convergence and raster calibration

## Run the calibration

Build the Release renderer and enable Vulkan validation. Use a device with
inline ray queries, a Khronos validation layer manifest with a library path
absolute or relative to the manifest, and Python with NumPy, Pillow and OpenCV
with OpenEXR support. The CPU-only self-check
exercises the fixture generator, statistical gates and EXR/PNG analysis.

```powershell
$harness = 'tests/validation/reference_scene_convergence_runtime_regression.py'
python -B $harness --self-check

$build = 'out/build/windows-release'
$env:CONTAINER_RUN_GPU_REFERENCE_CONVERGENCE = '1'
ctest --test-dir $build --output-on-failure -R '^reference_scene_convergence_gpu_regression$'
```

CTest uses the `VK_KHRONOS_VALIDATION_JSON` path from CMake and creates a fresh
run in the build tree's `test_results/reference-convergence` directory. For a
standalone run, use the actual executable and validation manifest paths.

```powershell
$renderer = Join-Path $build 'VulkanSceneRenderer.exe'
$layer = Join-Path $env:VULKAN_SDK 'Bin/VkLayer_khronos_validation.json'
$output = Join-Path $build 'reference-convergence-review'
$manifest = "$output.freeze.json"
python -B $harness --freeze-manifest --renderer $renderer --layer-manifest $layer --manifest $manifest
python -B $harness --renderer $renderer --layer-manifest $layer --manifest $manifest --output $output
python -B $harness --analyze-only --output $output
```

Use a new output directory for each standalone run. Keep the frozen inputs at
their recorded paths to replay the analysis. The receipt pins the renderer,
SPIR-V, runtime dependencies, validation layer and fixture code. It also records
the generated scene hashes, launch arguments and capture file hashes.

Every screenshot sidecar and telemetry record identifies the executable build
and records the primary scene graph, geometry, object data and material
revisions. Path-traced captures also record the submitted content revision and
accumulation resets. These stamps describe the running process. Use the frozen
asset hashes and camera/lighting settings for comparison across processes. The
path-traced EXR comes from the linear accumulation before display grading and
denoising, with exact requested and accepted samples at each sample checkpoint.
Use the report's raw HDR metrics and the matched PNGs to compare reference with
forward and deferred raster.

The Cornell, area light, blocker, roughness and interior fixtures use four
independent seeds at each low-sample checkpoint and two independent 4096-spp
targets. A planar zero-roughness mirror reflects two off-camera emissive
patches with known radiance. The analytic check compares each channel inside
the reflected patches and their black surround, excluding pixels that can
straddle an edge. The reference GPU component tests separately check closed-form
area-emitter energy, blocker visibility and exact emitter radiance.

For scene edits and capability fallback, run the native reference regression
with the same renderer and Vulkan layer.

```powershell
$env:VK_LAYER_PATH = Split-Path $layer
python -B tests/validation/reference_path_runtime_regression.py --renderer $renderer --output (Join-Path $build 'reference-edits-review')
```

The edit sequence checks fresh accumulation on visibility changes, undo and
redo, primary and BIM provider replacement, environment replacement and
material edits. The renderer keeps accumulating through display-only changes.
Hiding or filtering out the last ray-visible object leaves a valid empty scene
and renders the background. A visible ray-incompatible surface still reports
its unsupported status.

On the 320 × 240 edit fixture, the old build returned a black frame and counted
998,400 unsupported samples at 13 spp after the last object was hidden. The
updated build rendered the expected linear sky RGB `[1, 0.5, 0.25]` with zero
unsupported samples. Undo restored the original float32 image bit for bit.
[Before and after images](https://github.com/semihguresci/VulkanSceneRenderer/blob/main/docs/images/reference/hidden-scene-before-after.png)

## November 2026 verification

The Release renderer completed 102 captures in 44 processes on a GeForce RTX
2080 SUPER with driver 610.88 and Khronos synchronization validation 1.4.363.1.
The 84 raw EXRs and 18 display PNGs passed validation and frozen replay across
144 pinned inputs. The native edit suite passed 63 captures in 19 processes,
including the forced fallback to Deferred raster. All 163 reference and
transport GPU component tests passed.

The table compares each renderer's scene-linear RGB RMSE with the mean of the
two independent path-traced targets. The reported GPU times are median retired
reference Lighting pass times at the 512-spp checkpoint. The mirror time comes
from its warmed 32-spp display capture. The time measures the latest dispatch at
that checkpoint, and the EXRs and telemetry record the full sample count.

| Scene | Path at 32 spp | Path at 128 spp | Path at 512 spp | Forward raster | Deferred raster | Reference Lighting (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Cornell | 0.044231 | 0.022186 | 0.012431 | 1.081814 | 1.081110 | 98.75 |
| Area light | 0.011381 | 0.005738 | 0.002929 | 0.082610 | 0.088461 | 18.77 |
| Blocker | 0.006110 | 0.003071 | 0.001571 | 0.103862 | 0.108653 | 30.05 |
| Roughness | 0.023379 | 0.011546 | 0.006157 | 0.089955 | 0.090305 | 49.14 |
| Interior | 0.041546 | 0.021116 | 0.010745 | 0.880692 | 0.880071 | 125.76 |
| Mirror | See analytic result below | | | 0.085931 | 0.089430 | 13.12 |

The mirror's maximum absolute channel error in either seed was
`2.87e-8` against an absolute tolerance of `5e-5`. The renderer owned about
3 MiB of reference resources per scene. Borrowed acceleration structures used
4,608 to 180,480 bytes across the fixtures. The receipt includes per-capture
allocations, timings, scene revisions, sample diagnostics, and raw EXR hashes.

## Earlier baseline

The original native scene matrix passed 95 captures across 40 processes on the RTX 2080
SUPER with driver 610.88, Vulkan validation 1.4.363.1 and renderer
`dfc887d53da4bb81ac75196f7d554ce0e9bf40e8110aaac72a6c592c8b9e84a4`.
The frozen receipt, generated fixtures, launch commands, image comparisons and
calibration guide are retained under
`build/codex-gltf-validation/reference-convergence-sized-dfc887-native`.
The harness is `tests/validation/reference_scene_convergence_runtime_regression.py`.

Four independent seeds at 32, 128 and 512 spp use the mean of two separate
4096-spp seeds as a finite, noisy target. Values below are scene-linear RGB
RMSE, averaged across the four low-sample seeds; target noise is estimated from
the two high-sample images. These are empirical image/seed statistics, not a
pixelwise monotonicity or formal heavy-tail confidence guarantee.

| Scene | 32 spp | 128 spp | 512 spp | Measured target noise |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 0.044231 | 0.022186 | 0.012431 | 0.002441 |
| Area lights | 0.011381 | 0.005738 | 0.002929 | 0.000710 |
| Blocker | 0.006110 | 0.003071 | 0.001571 | 0.000381 |
| Roughness | 0.023379 | 0.011546 | 0.006157 | 0.001370 |
| Interior | 0.041546 | 0.021116 | 0.010745 | 0.002727 |

Compare raw linear EXRs for energy differences. Match camera, import units,
lighting, clipping and exposure before comparing engines. The captured display
views use manual exposure 0.5, Reinhard and disabled bloom, TAA, upscaling and
DDGI, at the same unjittered camera and resolution. Raster PNGs retain the
editor's selected-node outline; raw scene-linear captures omit it. For clean
interactive display comparisons, clear selection or use Render Preview.

Use the area-light and roughness fixtures to inspect LTC fit differences, and
the blocker fixture to distinguish hard raster visibility from finite-area path
visibility. To calibrate DDGI, freeze a matching raster configuration with DDGI
enabled, let probes converge and compare indirect regions with the raw target.
This baseline keeps DDGI off and does not claim DDGI quality acceptance. For
reflections, use matching roughness/occlusion views and account for unsupported
or off-screen SSR paths. The standard-error view estimates sampling noise;
it does not measure transport bias.

The receipt retains per-capture GPU pass timings and owned reference allocations
(about 3.15 MB for these fixtures). Timings include validation and concurrent CPU
build activity; they are fixture measurements without a comparative performance
claim. The first checkpoint can precede a retired GPU timing and records it as
unavailable. Later checkpoints supply actual measurements for every scene.
The documented material/medium limits and numerical diagnostic counters apply.

After the remaining library upgrades, renderer
`5796c721b60b3967619008f72fc4de67e33e0b6bc53870798b271f5fc2ca292a`
replayed the Cornell seed-23 checkpoints at 32/128/512 spp. All decoded float32
bits and encoded EXR bytes match this accepted baseline exactly. The bridge
receipt is
`build/codex-gltf-validation/latest-all-5796-native-v1/reference-smoke/receipt.json`.
