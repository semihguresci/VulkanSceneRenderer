# BIM statistics and scene-provider caching

The follow-up to [#74](https://github.com/semihguresci/VulkanSceneRenderer/issues/74)
removes steady-frame scans of imported geometry categories, provider bounds,
triangle-batch metadata and unchanged filtered LOD totals. It complements the
[inspection table cache](bim-performance.md), which caches schedules and model
comparison inputs.

`BimStatisticsCache` publishes geometry-category counts, draw counts, base
bounds and immutable triangle batches after a complete model installation.
Clear invalidates them before progress callbacks. Reinstall publishes a fresh
generation even if geometry or object revisions happen to match an earlier
model. Residency, buffer sizes and compaction counters/readiness are read live.
Root translation offsets cached base bounds without modifying topology.

Budget/LOD totals reuse the complete semantic, selection, phase-timeline and
draw-budget filter key, plus object, installation/LOD and visibility revisions.
Changing those inputs recomputes the totals. GPU visibility results remain on
the GPU; this cache adds no visibility readback.

Mesh provider topology uses source identity, primitive storage/count, geometry
revision, material revision and material count. Instance count/revision and
bounds are refreshed without rebuilding topology. Material creation, uploads
and asset reset advance the aggregate material revision. Source mutations must
publish their corresponding revision; in-place edits with unchanged revisions
are outside the cache contract.

Providers, synchronization inputs, registry snapshots and ray-extraction inputs
share `ImmutableSceneTriangleBatches`. Each snapshot owns its immutable data,
so unloading a model or replacing its provider cannot invalidate a retained
frame. Registry and extraction result containers still allocate a bounded
amount of metadata proportional to the number of providers, rather than the
number of triangle batches.

## Validation and measurements

Build the application, `scene_provider_extraction_tests` and
`bim_statistics_cache_tests` in Visual Studio Debug or Release, then run:

```powershell
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^(scene_provider_extraction_tests|bim_statistics_cache_tests|bim_draw_filter_state_tests|bim_inspection_cache_tests)$'
```

The regressions cover complete filter keys, live GPU fields, equal-size geometry
and material replacement, clear/reinstall, product identity, changing bounds,
root translation and snapshot lifetime through unload.

The CPU microbenchmark is opt-in and has no timing thresholds in CTest. It runs
production provider synchronization, registry snapshots and ray extraction at
10, 8,790 and 100,000 synthetic primitives. Setup and warmup are excluded. Seven
timing repeats run without allocation counters; a separate warmed pass records
allocation counts and requested bytes. Native scene captures measure the
original manager's statistics/bounds work as well as complete rendering.
Reported timing medians are medians of seven batched means, rather than
individual-frame latency percentiles. The short cache-hit queries reach the
8,192-iteration cap and describe hot-cache behavior. Both benchmark variants
replace global C++ allocation with the same `malloc` backend even while counters
are disabled; their absolute timings are benchmark-executable measurements.

From a Visual Studio developer environment:

```powershell
python tests/benchmarks/capture_bim_cache_baseline.py
cmake -S . -B out/build/visual-studio -DENABLE_BIM_FRAME_CACHE_BENCHMARK=ON -DCONTAINER_BIM_CACHE_BASELINE_INCLUDE_DIR=F:/Projects/Container/out/bim-frame-cache-validation/before-include
cmake --build out/build/visual-studio --config Release --target bim_frame_cache_benchmark bim_frame_cache_baseline
out/build/visual-studio/benchmarks/Release/bim_frame_cache_baseline.exe
out/build/visual-studio/benchmarks/Release/bim_frame_cache_benchmark.exe
```

Use your checkout's absolute baseline include directory in the configure
command. Repeat with Debug to assess debug-library overhead. The baseline
captures only the five provider headers from the preceding implementation;
it leaves the active checkout intact. The BIM provider microbenchmark excludes
the old manager's batch construction and count scans, so it understates the
complete prior cost. The new benchmark also reports cached production BIM
statistics/topology and budget/LOD queries.

Results are machine-specific. Compare Scene update, GUI and Resource growth
phases after warmup, excluding screenshots and separating frame waits and
presentation from CPU work; do not infer an FPS gain from isolated cache times.

### Provider microbenchmark results

Measured with Visual Studio 2026 Insiders on an AMD Ryzen 9 3900X
(12 cores / 24 threads). The baseline is commit
`7fd895a8209ee718af85a9ea4cd0289a1c28a17a`.
The following medians are microseconds per provider publication,
synchronization, snapshot and ray-extraction call at 8,790 synthetic primitives:

| Configuration | Provider | Before (µs) | After (µs) | Requested bytes before | Requested bytes after |
| --- | --- | ---: | ---: | ---: | ---: |
| Debug | Mesh | 1,209.59 | 19.24 | 1,197,145 | 1,088 |
| Debug | BIM | 332.57 | 19.14 | 880,515 | 1,088 |
| Release | Mesh | 303.54 | 0.49 | 1,196,337 | 576 |
| Release | BIM | 215.24 | 0.50 | 879,787 | 576 |

At all three sizes (10, 8,790 and 100,000), the after implementation requests
1,088 bytes in 37 allocations per Debug call and 576 bytes in seven allocations
per Release call. The warmed statistics/topology and budget/LOD cache-hit
queries allocate nothing. These are requested bytes through C++ allocation
functions, rather than process memory usage or GPU allocations. Installation
still builds and owns the model topology once.

### Native Tekla House results

On the same CPU and an NVIDIA GeForce RTX 2080 SUPER, eight configurations were
captured before and after the change. Each measurement averages 99 warmed frames
without screenshots. The open-UI cases use the same Scene Controls and Renderer
Telemetry window layout:

| Configuration | Technique | Scene update before → after (ms) | GUI before → after (ms) |
| --- | --- | ---: | ---: |
| Release | Deferred | 0.092 → 0.069 | 0.317 → 0.223 |
| Release | Forward | 0.092 → 0.067 | 0.306 → 0.209 |
| Debug | Deferred | 1.847 → 1.463 | 3.136 → 1.717 |
| Debug | Forward | 1.479 → 1.471 | 1.872 → 1.573 |

Debug forward Scene update is effectively unchanged. Frame totals include
variable presentation waits, so these results establish a CPU metadata/GUI
improvement rather than a general FPS increase.

All eight configurations retain identical scene/camera signatures and 87 shader
payloads. The four scene-only forward/deferred raster/hard-ray PNGs are
pixel-identical. Pass names, activation and status match; asynchronous GPU
timestamp-readiness is excluded from this topology comparison. Open-UI images
contain changing timing text and are excluded from the pixel comparison.

Both Visual Studio application configurations build successfully. Four cache
test targets pass in Debug, and 16 related CPU/GPU targets pass in Release with
the DDGI and ray-query GPU opt-ins enabled and no skipped targets. Eight native
model lifecycle runs produce 84 captures with clean synchronization validation:
forward/deferred raster and hard-ray + TAA load/reload/failure/unload sequences,
mixed-provider replacement, and asynchronous cancellation. They verify counts,
selection identity, camera and acceleration-structure state across model
changes. Repeat this selected lifecycle matrix with:

```powershell
$env:CONTAINER_RUN_GPU_GUI_MODEL_LOADING = '1'
python -B tests/validation/gui_model_loading_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --suite lifecycle --modes raster rays-taa --technique both --output out/bim-frame-cache-lifecycle
```

The [measurement record](../tests/benchmarks/results/bim-frame-cache-windows.json)
contains all benchmark repetitions and allocation measurements, eight native
comparisons, fixture/binary/source hashes and the passing test/lifecycle cases.
Raw images, logs and full telemetry remain in the ignored
`out/bim-frame-cache-validation` directory. Source hashes identify the locally
validated implementation before its publication.
