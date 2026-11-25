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
It also changes a position in a private glTF copy and verifies a new cold entry
followed by a warm hit. The cold run starts with an empty derived cache. The
operating system file cache and the existing BIM metadata cache stay unmanaged.

Add `--validation-manifest PATH` to run Vulkan synchronization validation, or
`--no-ray-query` to exercise raster capability fallback. Use `--plan-only` to
prepare the commands and check inputs without launching the renderer.
