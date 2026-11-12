# Scene hierarchy and BIM browser performance

The implementation of [#75](https://github.com/semihguresci/VulkanSceneRenderer/issues/75)
avoids resubmitting the complete expanded hierarchy and rebuilding BIM element
searches on every frame. It complements [BIM frame caching](bim-frame-cache.md).

## Hierarchy rows and identity

`SceneHierarchyCache` owns a visible preorder with stable node IDs, depth and
labels. The widget clips fixed-height rows and submits only the viewport range.
Expansion changes rebuild the preorder; selection, focus and visibility operate
on original node IDs. Reparenting applies after the current table finishes so
its rows stay valid. An open parent popup keeps its owner row submitted when the
main list scrolls.
Fixed-height hierarchy and BIM element rows render single-line labels, with the
original multiline values retained in hover tooltips. Parent popups reveal the
current parent when opened, including parents near the end of a large graph.

The key uses graph address, graph identity and hierarchy revision. Creating,
reparenting or renaming a node advances hierarchy revision; transform and
visibility edits do not rebuild static rows. Copy/move replacement receives an
independent graph identity, including replacement at the same address with an
equal revision/count. Valid existing nodes retain their expansion through
ordinary topology edits. Row controls use the graph identity so a held click
from a replaced model cannot act on a reused node ID. Replacement and node
retirement dismiss the affected row popup. Model inspection reset clears the
UI caches.

Use `SceneGraph` mutation methods for tracked fields. Editing a node directly
through its legacy mutable accessor bypasses revision publication.

## Mesh picker and selected transforms

The Mesh combo lists renderable nodes and clips its fixed-height rows to the
popup viewport. `None` remains available above the clipped list. Choices use
original graph node IDs, including when groups separate meshes or renderable
membership changes their order.

Closed frames validate the selected mesh through its node record in O(1) time.
When the popup appears, locating a selected mesh's row takes O(N) time; that row
is included for default focus and scrolling. Subsequent open frames submit only
the visible rows instead of iterating over every mesh.

The numeric transform inspector also accepts nonrenderable hierarchy entries,
including groups and BIM/USD physics proxies, even in a scene with no meshes.
Changing the Mesh combo defers the inspector until the next frame supplies the
new selection's transform snapshot.

The Release CPU widget benchmark uses the same probe before and after this
change, with 16 measured calls per state and nonrenderable groups interspersed
among the meshes. Median times are:

| Mesh entries | Closed picker before → after (µs) | Open picker before (ms) | Open picker after (µs) |
| ---: | ---: | ---: | ---: |
| 100,000 | 7.7 → 1.2 | 40.36 | 7.3 |
| 1,000,000 | 62.4 → 1.2 | 235.34 | 7.1 |

These are warmed widget submission timings, excluding scene construction,
popup-opening focus lookup and rendering. Real ImGui interactions verify
scrolling to the selected final mesh, selecting `None`, and selecting the final
row using its original graph ID. The probe retains exactly two selection
callbacks, no transform callbacks and unchanged graph data. Source, library and
executable hashes accompany the results under `out/physics-transition-audit`.

## BIM search and relationship tables

`BimMetadataStorage` owns compact runtime records. Eleven repeated fields use
model-scoped 32-bit IDs: type, object type, storey name/ID, material name/category,
discipline, phase, fire rating, load bearing and status. Exact, case-sensitive
text equality resolves hash candidates. Empty labels use ID zero. GUIDs, source
IDs and display names remain in a separate packed byte buffer with independent
field boundaries; arbitrary source identifiers and embedded nulls are preserved.
There is no persistent owning DTO mirror. Properties, bounds and numeric IDs
retain their previous values. Construction-only dictionary keys are retired when
the publication is sealed; unloading releases the records, pools and properties.

`BimElementMetadata` remains an owning import/test DTO. `BimMetadataTableView`
resolves borrowed text rows from either DTO fixtures or compact runtime storage.
Views must not outlive the model or cross a metadata publication change. Derived
inspection tables, relationship labels and identity indexes copy values they
retain; the UI only borrows immutable rows while drawing.

`BimElementSearchCache` keeps normalized GUID/source-ID/display-name text packed.
Type, storey and material values share a separate normalized dictionary and three
32-bit IDs per entity. A changed query searches each distinct label once, then
checks row IDs and the three packed fields. Discovery is bounded to 262,144
labels or 64 MiB of unique label bytes. If that limit is reached, or dictionary
storage would be larger, the cache retains all six fields in packed form.

Substring matching preserves the previous field-by-field OR semantics. Queries
are not trimmed/tokenized and cannot span fields. Original GUID identity stays
case-sensitive; browser search retains its case-insensitive byte normalization.
Empty queries reuse the full ordered index; unchanged or case-equivalent queries
reuse matches. Selection and focus use each element's original object index.
Clear releases normalized pools, ID arrays and query storage while preserving
cumulative diagnostics. New-query work remains O(N); warmed frames do not rescan.

The interned search representation uses approximately 44 bytes per entity for
three offsets, three 32-bit label IDs and an identity index, plus packed unique
field text, dictionary bytes/offsets and membership bits. High-cardinality
fallback uses the earlier six-offset representation. Filter-result capacity and
query storage are additional. Capacity estimates exclude allocator bookkeeping.

Production inspection state supplies the BIM manager identity and completed
metadata publication revision. That revision advances on clear before progress
callbacks, and after a complete installation, even when storage is reused.
Geometry revision alone is insufficient because it advances before metadata
construction and does not advance on clear. A closed panel releases its search
storage when the publication changes or unloads. It builds the next index when
the BIM Elements browser opens. Read-only widget fixtures may use the metadata
span as source identity. In-place replacements must publish a new revision or
reset the inspection state.

Relationship searches, selected/spatial edge indices, property filtering and
property-set summaries are cached by publication, graph, query and selection.
Wrapped relationship, spatial, property and storey tables display at most 100
rows per page, with first/previous/next/last and page-number controls. This keeps
full wrapped values readable without assuming a constant row height. Query,
selection and publication changes reset relevant pages and table scrolling.
The scroll reset stays pending while its table is clipped.
The Plan-storey combo clips its uniform-height rows.

Cached normalized text and row labels consume persistent CPU memory in exchange
for avoiding repeated string allocation. Capacity estimates include owned
vector/string capacity and may conservatively count inline string characters;
they are not exact allocator or process-memory measurements.

## Tests and measurements

Build and run the real ImGui widgets without a platform or Vulkan backend:

```powershell
cmake --build out/build/visual-studio --config Release --target scene_graph_tests scene_hierarchy_ui_tests bim_element_search_cache_tests bim_element_browser_widget_tests bim_relationship_browser_cache_tests technique_debug_model_tests
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^(scene_graph_tests|scene_hierarchy_ui_tests|bim_element_search_cache_tests|bim_element_browser_widget_tests|bim_relationship_browser_cache_tests|technique_debug_model_tests)$'
```

This checkout currently shares test executable/DLL output paths across Visual
Studio configurations. When switching between Debug and Release in the same
build directory, force the test executables to relink or use separate build
directories; `ctest -C` alone does not rebuild or replace stale binaries.

The regressions cover over 10,000 nodes/elements, final-row scrolling and
selection/focus, expansion and reparenting, case folding, empty results,
same-storage replacement, unload/reset and stable storage across unchanged
frames. Paging tests cover the final partial page, shrinking results and large
page numbers.

The opt-in benchmark executes full CPU ImGui frames (`NewFrame`, production
widgets and `Render`) using a 10,001-entry synthetic fixture. Hierarchy expansion
and text input use actual mouse/keyboard events. There is no GPU backend or
Vulkan validation in this measurement. Fixture creation, cache installation,
interaction and 24 warmup frames are excluded. Seven timing batches run without
allocation counters; a separate warmed pass counts C++ and ImGui allocations.
Timing medians describe seven batched means, not individual-frame percentiles.

Both variants use the same `malloc`-backed global C++ allocation replacements
and ImGui allocation hooks. Requested bytes per frame are cumulative allocation
requests rather than peak/live memory. Retained ImGui requested memory and the
new cache-capacity estimates are reported separately. Native renderer frame
times additionally include platform/GPU work and cannot be inferred from these
widget measurements.

To preserve a matching baseline, run this **before rebuilding** the old source
from a Visual Studio developer shell. The capture stores committed headers and
the existing Release libraries; those libraries must have been built from the
requested baseline. It does not modify the active source checkout:

```powershell
python tests/benchmarks/capture_scene_browser_baseline.py
python tests/benchmarks/build_scene_browser_baseline.py
cmake -S . -B out/build/visual-studio -DENABLE_SCENE_BROWSER_BENCHMARK=ON
cmake --build out/build/visual-studio --config Release --target scene_browser_benchmark
```

Run `out/scene-browser-validation/baseline/scene_browser_baseline.exe` and
`out/build/visual-studio/benchmarks/Release/scene_browser_benchmark.exe` with the
packaged renderer DLL directory on `PATH`. Each prints JSON. An optional first
argument selects a fixture count from 100 through 10,000,000. Keep build/test processes idle
during measurements, and use the same fixture count/toolchain for both variants.

### Initial Release widget measurements, before packed storage

These historical measurements precede the packed-index optimization below.
Measured on October 17, 2026 with Visual Studio 2026 Insiders, AMD Ryzen 9 3900X
(12 cores / 24 threads), ImGui 1.91.8 and a 10,001-entry fixture. The baseline is
commit `cb8406a937ce1223e9d7239e9fbe1532d2c05d0c`, including the #74 caches.

| View | Before (µs/frame) | After (µs/frame) | C++ requested bytes before | C++ requested bytes after |
| --- | ---: | ---: | ---: | ---: |
| Hierarchy, root expanded | 12,284.225 | 27.846 | 3,415,158 | 768 |
| Hierarchy, root collapsed | 8.859 | 7.379 | 40,247 | 0 |
| Hierarchy panel closed | 2.916 | 2.959 | 0 | 0 |
| BIM Elements, empty query | 72.782 | 40.238 | 80,431 | 384 |
| BIM Elements, unchanged filtered query | 9,108.175 | 10.515 | 2,320,207 | 48 |
| BIM Elements panel closed | 2.869 | 2.889 | 0 | 0 |

Closed-panel timings are effectively unchanged. Steady ImGui allocation requests
are zero in every case. The expanded hierarchy retains an estimated 1,034,105
cache bytes; its collapsed root retains 570,089 bytes. The BIM search index
retains about 4,240,448 estimated bytes. These estimates exclude allocator
bookkeeping and conservatively include inline character capacity. This is a
CPU memory tradeoff, not a GPU-memory measurement. Unload/reset releases the
large caches.

The hierarchy benchmark builds rows once and once more for the expansion event;
its timing batches do not rebuild them. BIM metadata is normalized once per
fixture, with query results rebuilt only for the initial/typed query. Widget
frames still allocate small amounts for visible row labels; these results do
not claim a zero-allocation UI or an application-wide FPS multiplier.

At this initial browser-cache stage, Visual Studio Debug and Release application
builds pass. The six related targets contain 100 cases, all passing independently
in each configuration without skips. Four native forward/deferred raster/hard-ray
captures, taken before the later ray-precision correction, have clean
synchronization validation and byte-identical PNGs versus the #74 baseline;
scene/camera signatures, pass topology and all 87 shader payloads are unchanged
for this metadata-only comparison.
Four forward/deferred raster/hard-ray + TAA lifecycle runs produce 68 captures
with clean validation, covering repeated load, failed-load recovery and unload.

The [measurement record](../tests/benchmarks/results/scene-browser-windows.json)
contains every timing repetition, allocation/memory result, test case name,
native comparison and source/library/executable hash. It identifies the locally
validated implementation before publication. Raw native images, XML reports,
logs and full telemetry remain under ignored `out/scene-browser-validation`.

### Large entity tests

The scale runner tests 100,000, 1,000,000 and 10,000,000 entities, running each
case in a separate process. The `compact` fixture retains the complete production
`SceneNode` or `BimElementMetadata` structure, unique IDs and all six searchable
fields, with short values to limit unrelated string allocation. The `rich`
fixture retains the original longer text. These are CPU browser tests; they do
not load geometry or measure Vulkan rendering at those entity counts.

```powershell
python tests/benchmarks/run_scene_browser_scale.py --exe out/build/visual-studio/benchmarks/Release/scene_browser_benchmark.exe --counts 100000 1000000 10000000 --fixture compact
```

Put the renderer's Release DLL directory on `PATH` when running the executables.
The runner records OS peak working set and process commit, sampled private commit,
timings, allocation counts and verification flags. It preserves completed cases
on timeout, allocation failure or invalid output. `--baseline-exe` accepts a
matching captured executable for comparisons; `build_scene_browser_baseline.py
--current-ui` builds against a captured modern UI before packed storage.

Actual mouse and keyboard events verify middle/final row selection and focus,
all six search fields, empty/no-match recovery, unchanged-cache reuse and BIM
unload/reset. Closed/collapsed cases mark row interaction as inapplicable.
Fixture creation, those checks and warmups are excluded from steady frame timing.

The fast `scene_browser_scale_runner_tests` CTest guards orchestration and result
validation. The expensive `scene_browser_scale_stress` CTest is enabled with the
benchmark build option and skips unless explicitly opted in:

```powershell
$env:CONTAINER_RUN_LARGE_SCENE_BROWSER_TESTS = '1'
ctest --test-dir out/build/visual-studio -C Release -R '^scene_browser_scale_stress$' --output-on-failure
```

Measured on October 17, 2026 on the same Ryzen 9 3900X with 32 GiB RAM. All 36
sequential runs passed (six views, three entity counts, before/after packed
storage). The baseline uses captured, matching headers/libraries from the
initial #75 implementation; both executables use the same expanded benchmark.
These comparisons isolate packed storage, whereas the earlier 10,001-entry
comparison also measures hierarchy virtualization and query caching.

| BIM entries | Index capacity estimate before → packed (MB) | OS peak working set before → packed (MB) |
| ---: | ---: | ---: |
| 100,000 | 29.60 → 9.98 | 88.99 → 79.02 |
| 1,000,000 | 296.00 → 101.78 | 801.90 → 703.68 |
| 10,000,000 | 2,960.00 → 1,037.78 | 7,929.90 → 6,967.69 |

Values use decimal MB and the empty-query BIM view. The previous index's
conservative estimate counts inline string character capacity in addition to
the string objects; therefore its estimated reduction overstates the physical
memory saving. The OS working-set measurement shows about 962 MB (12.1%) less
peak resident memory for the complete 10-million-entry process, including its
unchanged 5,920 MB metadata vector. OS peaks include fixture setup, verification,
unload/reload and timing; sampled private commit and OS peak commit are also
recorded separately.

| Entries | Packed expanded hierarchy (µs/frame) | Packed empty-query BIM (µs/frame) | Packed cached filtered BIM (µs/frame) |
| ---: | ---: | ---: | ---: |
| 100,000 | 23.391 | 24.290 | 9.645 |
| 1,000,000 | 23.467 | 24.663 | 9.599 |
| 10,000,000 | 23.958 | 24.174 | 9.512 |

These are steady widget CPU times after installation and filtering. They do
not include the O(N) cost of building the index or scanning a changed query.
The compact fixtures request no C++ or ImGui allocations in the warmed frames.
All three counts pass actual middle/final row selection and focus, six-field
searches, empty/no-match recovery, cache reuse and unload/reset where applicable.
The packed implementation builds in Visual Studio Debug and Release and passes
103 related C++ cases in each configuration, plus 14 scale-runner guard tests.
The [scale measurement record](../tests/benchmarks/results/scene-browser-scale-windows.json)
preserves all timing repetitions, OS memory high-water marks and sample counts, verification flags,
source hashes and captured baseline library hashes.

### Compact model metadata and categorical search IDs

A second 36-case matrix compares the compact runtime metadata and interned
search labels with the earlier packed search index over owning metadata. Both
executables use the same expanded benchmark source and compact fixture. The
baseline retains the complete 592-byte owning DTO per entity; the current
runtime record is 216 bytes, with separate packed identity/name text and shared
categorical labels. The fixture retains unique identifiers and all six search
fields; its first, middle and last rows also have distinct categorical values.

All 36 runs passed at 100,000, 1,000,000 and 10,000,000 entities. The following
values use decimal MB and the empty-query BIM view:

| BIM entries | Source capacity estimate before → interned (MB) | Search index before → interned (MB) | OS peak working set before → interned (MB) |
| ---: | ---: | ---: | ---: |
| 100,000 | 59.20 → 24.32 | 9.98 → 6.48 | 79.01 → 40.19 |
| 1,000,000 | 592.00 → 247.00 | 101.78 → 66.78 | 703.67 → 315.60 |
| 10,000,000 | 5,920.00 → 2,513.16 | 1,037.78 → 687.78 | 6,967.68 → 3,105.56 |

The complete 10-million-entry BIM browser process uses about 3.86 GB less peak
resident memory (55.4%). Source and index columns are owned-capacity estimates,
not process-memory measurements. OS peaks include construction, correctness
interactions and unload/reload, as well as timing. The fixture has empty property
lists and does not build the renderer's complete relationship graph, GUID lookup
tables, geometry, textures or GPU resources; these figures do not predict the
memory of a 10-million-object rendered scene.

| Entries | Interned expanded hierarchy (µs/frame) | Interned empty-query BIM (µs/frame) | Interned cached filtered BIM (µs/frame) |
| ---: | ---: | ---: | ---: |
| 100,000 | 24.132 | 24.877 | 9.794 |
| 1,000,000 | 23.711 | 25.212 | 9.638 |
| 10,000,000 | 23.681 | 24.925 | 9.740 |

Warmed frames request no C++ or ImGui allocations in these compact cases. The
matrix verifies middle/final row interaction, all six searchable fields, original
identities, unchanged-cache reuse and unload/reset. It records setup-inclusive
process duration separately; the steady frame numbers exclude construction,
changed queries and verification. They do not establish a cold-load or
changed-query speedup. The normalized pool contains 12 distinct search labels in
this fixture; high-cardinality metadata may instead select the packed fallback.

Both Visual Studio Debug and Release application builds pass, together with
358 related C++ cases in 14 targets per configuration and the 14 scale-runner
guards. These include normalized label sharing, membership bit boundaries,
empty and embedded-NUL fields, bounded high-cardinality fallback, source
identity, reused storage, and unload/reset coverage.

The [interning measurement record](../tests/benchmarks/results/scene-browser-interning-windows.json)
retains the complete matrix, case names and source/library/executable hashes.
Its benchmark phase is identified independently from subsequent application
builds and native rendering checks, so concurrent physics integration is not
mistaken for the measured metadata snapshot.

Four further native captures validate interning before the separate ray-shadow
precision fix: scene/camera signatures, pass topology, all 87 shader payloads
and all RGB pixels match the #74 baseline exactly, with clean synchronization
validation. This frozen comparison uses the pre-fix executable; later renderer
validation is recorded as a distinct phase with expected shader changes.

After that renderer fix, four fresh captures retain the scene/camera signatures,
pass topology and clean synchronization validation. Three of the 87 shader
payloads change. Raster PNGs remain byte-identical; the hard-ray images change
by mean absolute RGB differences of 0.1501 (deferred) and 0.0752 (forward) on
the 8-bit scale. The measurement record reports those differences separately
from the strict metadata identity check. Four final forward/deferred raster
and hard-ray + TAA lifecycle runs also pass all 68 captures, covering repeated
load, failed-load recovery and unload with clean synchronization validation.
