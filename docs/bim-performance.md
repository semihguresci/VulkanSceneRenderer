# BIM inspection performance

Tekla House IFCX has 8,790 render objects. The former GUI path built schedule
and comparison inputs from all imported metadata and aggregated three schedules
on every frame, before testing whether Scene Controls or its BIM sections were
visible. Visual Studio Debug amplified the repeated string allocations and map
lookups: the measured GUI phase took 295.4 ms, while known GPU pass time was 8.2 ms.

`BimInspectionCache` now owns the derived tables and keeps their spans stable
until the model identity, geometry revision or metadata storage changes. Unload
clears them. Imported metadata bounds are immutable in the current contract;
future metadata/bounds edits must advance the revision. Runtime GPU color edits
do not rebuild these tables.

Measurements on October 11, 2026, NVIDIA GeForce RTX 2080 SUPER, Vulkan 1.4.325,
800x600, deferred raster, open GUI panels, TAA and validation disabled:

| Measurement | Before | After |
| --- | ---: | ---: |
| Debug GUI | 295.4 ms | 1.98 ms |
| Debug CPU frame excluding screenshot | 302.1 ms | 15.80 ms |
| Release GUI | 13.27 ms | 0.27 ms |
| Release steady CPU frame | Not sampled as a history | 8.86 ms |
| Release steady CPU frame P95 | Not sampled as a history | 9.45 ms |

Before values are captured-frame phase measurements. After values average
completed frames after the first 20 warmup frames, excluding screenshots:
24 Debug frames and 99 Release frames. No temporal sequence was used, so frame
synchronization remained inside the measured draw-frame timer. Debug used
`/Od /Ob0 /RTC1`; Release used `/O2 /Ob2`. These are single-device measurements,
not universal frame-rate guarantees.

CPU Frame is elapsed draw-frame time, including waits, presentation and
screenshot readback/PNG writing. Release presentation waits account for about
7.5 ms after the cache fix. Synchronization validation increases command
recording from under 1 ms to about 17.5 ms. At 2560x1440, a screenshot alone
takes about 330 ms; steady Release frames average 11.35 ms. Screenshot telemetry
now exports bounded `frameHistory` with all CPU phases so capture overhead can
be excluded from steady-state summaries.

The scene-only before/after images were pixel-identical. Visual Studio Release
and Debug builds passed, with tests for cache reuse, replacement, unload, bounds
updates, schedule/comparison semantics and independent telemetry phase history.
Native reload/replacement/cancellation checks also passed synchronization
validation. Detailed local evidence is in `out/tekla-cpu-profile`.

Remaining CPU, UI, import and rendering improvements are tracked in
[renderer follow-ups](renderer-follow-ups.md).

The subsequent stage caches model statistics, provider topology/bounds and unchanged
filtered LOD totals. See [BIM frame caching](bim-frame-cache.md) for its lifetime
contract, regressions and allocation benchmark.
Expanded hierarchy rendering and BIM element searches are optimized separately;
see [scene browser performance](scene-browser-performance.md) for measurements
with over 10,000 entries.

## Import preparation

On Tekla House IFCX, the instrumented baseline spent 2.28 s building the
relationship graph in Release. Real-object indexing took 18 ms. Inference took
2.26 s and made 27,562,568 class-compatibility checks across 8,788 label
lookups. Many objects use their IFC class as the display label. Resolving a
type with that same label repeatedly scanned the same incompatible objects.
The graph now caches the first compatible node or the miss for each shared
label and requested class during inference. Exact GUID and source ID matches
still take precedence. Explicit relationship endpoints reuse scratch vectors
and merge sorted identity postings in node order.

Each baseline floor-plan projection visited 2,775,390 triangles across 8,788
eligible elements. The source-elevation projection also visited 8,326,170
indices to find element and storey base elevations. A single Release probe
measured 163 ms for ground and 243 ms for source elevation, including temporary
storage cleanup. Both projections now share eligibility, range lookup,
triangle traversal and horizontal quantization. They retain separate
deduplication and ordered output. The preparation worker still handles both
projections and graph construction for replacement, with cooperative
cancellation during traversal and indexing.

### Reproduction

Build the profiling baseline at `7276fb4` and the optimized revision in
separate build directories with the same dependency configuration. Repeat
`--model` for each IFC and IFCX input from buildingSMART IFC5-development
revision [`1a63082`](https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples).

```powershell
cmake --build $before --target BimImportBenchmark BimImportAllocationBenchmark
cmake --build $after --target BimImportBenchmark BimImportAllocationBenchmark
python tools/benchmark_bim_import.py --before "$before/BimImportBenchmark.exe" `
  --after "$after/BimImportBenchmark.exe" --model $model `
  --processes 3 --iterations 2 --output timing.json
python tools/benchmark_bim_import.py --before "$before/BimImportAllocationBenchmark.exe" `
  --after "$after/BimImportAllocationBenchmark.exe" --model $model `
  --processes 1 --iterations 1 --output allocations.json
```

The report records executable and source-file SHA-256 hashes, the loaded
source path, CMake flags and compiler commands when exported, import counts,
and ordered semantic digests. The timing executable prepares scenes on the
CPU with the default allocator. Timing excludes semantic hashing, scene
destruction, GPU upload, rendering and Vulkan validation. A fresh-process
sample is its first import. A warm sample repeats the import in that process.
Fixture checks warmed the OS file cache before measurement, and the OS managed
the cache during collection. Working set and commit are process high-water
marks sampled before semantic hashing.
The separate allocation executable counts successful C++ allocation requests
and requested bytes on the preparation thread, excluding direct C allocator
calls and DLL-private allocators.

### Measurements

Windows x64, Ryzen 9 3900X, MSVC 19.51.36260.0 and Ninja. Release used
`/O2 /Ob2 /DNDEBUG /MD` and reports medians of three fresh processes per
variant and fixture. Debug used `/Z7 /Ob0 /Od /RTC1` and one process per
variant and fixture. The timing runs repeated each import once in the same
process. Allocation counts came from one import per variant and fixture.
The Debug executable used the cached dependencies' Release ABI with `/MD` and
`_ITERATOR_DEBUG_LEVEL=0`. For that configuration, map imported Debug targets
to `Release;` and use `CMAKE_PROJECT_INCLUDE` with
`set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)` and
`add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)`.

Fresh-process Tekla House results, before → after. Times are in seconds.

| Fixture | Build | Preparation | Floor plans | Relationships | Peak commit (MiB) |
| --- | --- | ---: | ---: | ---: | ---: |
| STEP | Release | 7.99 → 7.92 | 1.72 → 1.63 | 1.71 → 1.76 | 4,394 → 4,072 |
| STEP | Debug | 160.80 → 132.37 | 84.45 → 56.87 | 15.91 → 15.28 | 4,395 → 4,073 |
| IFCX | Release | 4.54 → 2.29 | 0.398 → 0.393 | 2.29 → 0.023 | 202.3 → 202.1 |
| IFCX | Debug | 57.91 → 30.79 | 14.71 → 10.57 | 22.97 → 0.156 | 202.6 → 202.7 |

The Release Tekla STEP floor-plan stage requested 2,174 MiB before the change
and 1,313 MiB after it. The Release Tekla IFCX graph went from 11.9 million
C++ allocation requests to 130,014. Full fresh and warm timings, per-stage
allocations, peak memory, source hashes and semantic digests for both formats
of Hello Wall and Tekla House are in the
[measurement record](../tests/benchmarks/results/bim-import-preparation-windows.json).

All four fixtures matched their baseline counts and ordered geometry,
floor-plan, metadata, graph and diagnostic digests in Release and Debug. The
STEP fixtures loaded directly as IFC. Native Vulkan validation captures
matched pixel-for-pixel across forward and deferred raster at 400×360 with a
fixed timestep and manual exposure. Scene telemetry matched in all eight
capture pairs. The relationship, floor-plan and loader regressions passed.
Ten GUI lifecycle cases and two active cancellation cases passed with Vulkan
synchronization validation enabled. Five auxiliary loader tests skipped
because their additional sample assets were unavailable.
