# Derived asset cache and streaming diagnostics

`--asset-cache-dir PATH` enables a persistent cache for CPU-derived scene data.
Use the same directory across launches. `--geometry-lod-error 2` enables
geometry LOD generation and `--texture-streaming` enables texture mip residency.
Texture upload limits and scene memory budgets continue to apply to cached loads.

The primary cache stores reduced indices and primitive ranges. Its key covers the
packed vertex and index content, primitive boundaries, generator version, and
generation options. A changed external glTF buffer that affects loaded geometry
produces a new entry. The BIM cache stores derived floor-plan and LOD ranges,
meshlet data, and object bounds. BIM imports still resolve source geometry and
dependencies. A warm entry skips the corresponding derived work and preserves
canonical meshes, material assignments, product identity, and storey metadata.

Entries include a version, source key, size limit, and payload checksum. Readers
also validate ranges and indices before publication. Missing, incompatible,
corrupt, and unwritable entries fall back to preparing data from the source.
Cached data follows the same GPU upload and reader retirement paths as freshly
prepared data. Hardware acceleration structures are rebuilt on the device.

## Live diagnostics

Open **Renderer Telemetry** and expand **Asset streaming** to inspect resident and
staging usage, peaks and limits, submitted LODs, and pending uploads. The
**Viewport markers** control shows active mesh LODs or the coarsest streamed mip
bound by each material. An amber mip marker indicates that screen demand calls
for a finer level. The marker layout uses projected object bounds and one
nearest center per screen cell to reduce clutter.

Use `--asset-overlay geometry-lod` or `--asset-overlay texture-residency` to set
the initial marker mode from the command line. Capture the window PNG with
`--screenshot` to include the markers.

Capture telemetry reports cache status, source keys, preparation times and cache
I/O under `assetCache`. `assetStartup` records initial scene setup and the
elapsed time from renderer initialization to the first normal graphics
submission. `raySceneBuildCosts` retains recent AS generations, their BLAS
build and reuse counts, allocation costs, and the GPU duration from the
submission that recorded the build. The GPU duration is null when the profiler
cannot return a matching result.

## Large-scene benchmark

Use the original buildingSMART Tekla House IFCX and Khronos A Beautiful Game glTF
assets with their dependencies. The benchmark records the source hashes and
keeps the original files intact. [Tekla House sample](https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/Tekla%20House) · [Khronos A Beautiful Game](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/ABeautifulGame)

```powershell
python -B tests/validation/large_scene_asset_benchmark.py `
  --renderer build/VulkanSceneRenderer.exe `
  --tekla "path/to/TeklaHouse.ifcx" `
  --gltf "path/to/ABeautifulGame.gltf" `
  --require-ray --output build/asset-cache-benchmark
```

The runner compares full-detail reference, fresh-cache and populated-cache
processes at matched cameras. It records startup and steady GPU timings, AS
rebuilds, scene and VMA memory, triangle counts, mip residency, and PNG quality.

Use `--asset-memory-mib` and `--asset-upload-mib` to set finite allocation
limits. `--unbounded-reference` keeps the full-resident control while applying
those limits to the streamed runs. `--interactive-reload` queues a model
replacement through Scene Controls, checks that the previous scene keeps
rendering during the first copy batch, and records the copy limit and longest
model service call when the new scene publishes.

It also changes a position in a private glTF copy and verifies a new cold entry
followed by a warm hit. The cold run starts with an empty derived cache. The
operating system file cache and the existing BIM metadata cache stay unmanaged.

Add `--validation-manifest PATH` to run Vulkan synchronization validation, or
`--no-ray-query` to exercise raster capability fallback. Use `--plan-only` to
prepare the commands and check inputs without launching the renderer.
Increase `--warmup` if a timed view arrives before its derived LODs or texture
uploads settle. The Windows chess run used 900 warm-up frames at 640 × 360.

### Bounded interactive runs

On the RTX 2080 SUPER with Khronos synchronization validation, each scene
continued to render the previous asset during CPU preparation and partial
geometry upload. The next asset became visible after the last copy completed.
The upload limit was 1 MiB per service tick. A Beautiful Game copied 34.6 MiB
in 36 chunks and Tekla House copied 49.6 MiB in 50 chunks. The peak resident
measurements include the overlap between old and new scene allocations.

| Asset | Resident / upload limits, MiB | Peak resident, reference → warm, MiB | Peak upload, MiB | Warm initial scene / first submission, ms | Median detail graphics, reference → warm, ms | Detail image RMSE (0–255) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A Beautiful Game glTF | 160 / 32 | 773.33 → 111.77 | 28.06 | 8,528 / 11,445 | 2.795 → 2.497 | 0.771 |
| Tekla House IFCX | 128 / 48 | 99.27 → 99.27 | 44.64 | 2,258 / 5,100 | 10.810 → 10.986 | 0 |

Moving glTF texture preparation to the existing worker reduced the longest
warm model-service call for A Beautiful Game from 6,656.74 ms to 205.71 ms
at the same budgets. The renderer still installs GPU resources on the render
thread. Tekla's longest warm service call was 38.61 ms. The final retired
detail samples reported `OcclusionCull` and `Lighting` GPU times of
0.0104 / 0.8900 ms for A Beautiful Game and 0.2924 / 1.8680 ms for Tekla.
The reload AS builds took 10.075 ms and 2.506 ms respectively. The matched
graphics medians use 24 retired submissions. Tekla used 144 warm-up frames
per view at 960 × 540.
