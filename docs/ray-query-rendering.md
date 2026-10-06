# Optional ray-query shadows

Both forward and deferred rendering can use geometry-based shadow visibility.
`--ray-shadows hard` traces a directional ray and selected point/spot lights;
area lights use their centre. `--ray-shadows soft` samples rectangle and disk
emitters and filters their visibility with independent temporal/spatial history.
The default remains raster shadows.

```powershell
.\VulkanSceneRenderer.exe --ray-shadows soft --ray-shadow-samples 8 --display-mode lit
.\VulkanSceneRenderer.exe --render-technique forward-raster --ray-shadows hard
.\VulkanSceneRenderer.exe --ray-shadows soft --no-ray-query
```

The last command exercises the unsupported-device raster route. Vulkan 1.4 does
not itself imply ray support. The device must expose
`VK_KHR_acceleration_structure`, `VK_KHR_ray_query`,
`VK_KHR_deferred_host_operations`, enabled buffer device addresses and both
extension feature bits. Missing optional features preserve raster device
selection. Only the separate trace compute module requires `spvRayQueryKHR`;
unsupported devices never create its pipeline. All packaged lighting modules
read a visibility texture without requiring ray capabilities.

## Quality and diagnostics

- `--ray-shadow-samples`: 1–32 emitter samples per selected area light; default 8.
- `--ray-shadow-light-budget`: 0–8 local lights; default 8. Up to the first four
  point/spot lights are selected, then up to four area lights use the remaining
  budget. Remaining lights use their existing raster shadows. Directional
  visibility has a separate single-ray budget.
- `--no-ray-shadow-denoise`: show current sampled visibility without filtering.
- `--ray-shadow-debug-layer`: 0 shaded; 1 directional; 2–5 point/spot;
  6–9 area-light visibility; 10 configured sampling budget. Budget RGB encodes
  emitter samples / 32, selected local lights / 8 and the directional query.

The Shadows UI offers the same controls, capability/fallback status, allocated
memory, BLAS build/reuse counts and instance counts. The GPU pass profiler shows
`RaySceneBuild`, `RayShadowTrace` and `RayShadowFilter`. Capture telemetry includes
`rayShadows`, actual VMA allocation sizes, retained generation memory and reset
counts. Timestamp results describe completed GPU work; an unchanged scene's build
pass does no AS work. Capture a dynamic sequence to measure TLAS rebuilding.

The highest budget is 1 directional + 4 point/spot + 4 × 32 area rays per covered
pixel. Zero-intensity and out-of-range local lights skip queries; emitter samples
with zero contribution skip traversal too. The budget view
shows the configured limit, rather than a hardware traversal-instruction count.

## Shared scene and lifetime contract

`RaySceneAcceleration` is a reusable BLAS/TLAS service, independent of shadow
shading. `RaySceneExtraction` adapts primary glTF and auxiliary BIM/USD indexed
triangles to its checked input contract. Provider-local draw ranges identify
geometry; repeated instances share a BLAS. Only referenced vertices are compacted,
and triangle metadata retains both UV sets. Extraction precedes camera/frustum,
Hi-Z and drawing-budget culling, so off-screen geometry still blocks rays.
Semantic object/layer visibility and section/box clipping remain effective.
Native point and polyline primitives have no triangle shadow representation.

Providers advance geometry revisions on changed positions/indices or reloads.
Geometry changes rebuild affected BLAS; transforms, visibility and sidedness
build a new TLAS while matching BLAS are reused. Validation rejects invalid
indices, nonfinite positions, duplicate provider identities and singular or
nonaffine transforms before recording. Empty scenes build valid empty TLAS.

Each immutable generation owns its TLAS, VMA input/scratch/storage allocations,
BLAS references and material metadata. Frame slots retain generations through GPU
fence retirement. Shared allocations are counted once in aggregate telemetry.
The device and allocator outlive all generations; AS handles are destroyed before
their VMA backing buffers. Builds, queries and filters run outside dynamic
rendering on the graphics queue with synchronization2 dependencies. The service
adds no queue/device idle to normal updates. Existing reload/resize retirement
protects replacement resources. Failed recording/submission must abandon the
command buffer before releasing its generation.

Vulkan's row-major 3×4 instance ABI is populated explicitly from GLM transforms.
Engine GLM-to-Slang uploads remain column-major. Mirrored instances receive no
extra winding flip because Vulkan tests facing in object space. Double-sided
materials disable triangle facing culling.
Visibility rays travel from receiver to light, so they cull query-front faces
to match the raster shadow camera's back culling in the opposite direction.

## Materials and supported shadow policy

Non-opaque candidates allow hit-level section and box clipping. Solid surfaces
commit immediately after clipping. Alpha masks interpolate triangle UV0/UV1,
apply the shared texture transforms and samplers, combine material alpha,
opacity factors and opacity texture channels, and compare the material cutoff.
Inline queries sample mip zero because raster derivatives are unavailable.
Raster coverage dithering at antialiased mask edges therefore differs; mask
interiors, transforms, UV selection and cutoff conventions are shared.

Blended and transmissive blockers do not cast binary ray shadows. Transparent
receivers retain raster visibility because their depth layers differ from the
opaque visibility buffer. This policy does not approximate coloured transmission
or volumetric attenuation. Height-displaced materials trigger a complete raster
fallback until displaced vertex positions can be shared with AS builds; the UI
reports that fallback rather than tracing the undeformed mesh.

## Visibility history

Only soft emitter visibility is filtered. Exact directional, point and spot
visibility is preserved, including thin blockers. The temporal pass reconstructs
current world position from reverse-Z depth, reprojects with the last submitted
jittered camera matrix, tests depth/normal compatibility and clamps history to a
compatible current neighbourhood. A subsequent spatial pass filters compatible
neighbours. Depth derivatives select the nearer valid neighbour on each axis to
avoid crossing silhouettes or background pixels.

Geometry/transforms, semantic visibility, materials, lights, clipping and quality
changes invalidate the entire visibility history. This conservative policy avoids
reusing illumination from moving blockers or lights. Camera motion reprojects
static surfaces; visibility history operates with TAA disabled too. Only a
successfully submitted frame advances history. Raster mode and resize invalidate
history; visibility resources remain alive across mode switches until frame
retirement. TAA keeps its separate scene-linear colour history.
Changing ray mode, sample/light budgets, filtering or debug output also resets
TAA colour history before the frame snapshot is created.

## Validation and costs

```powershell
$env:CONTAINER_RUN_GPU_RAY_QUERY = '1'
$env:CONTAINER_RUN_GPU_RAY_SHADOW = '1'
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R 'ray_scene|ray_shadow_gpu'
```

The backend probe checks real sided/mirrored/scaled/distant traversal, masks,
finite rays, candidate rejection, BLAS reuse and old-generation lifetime. Runtime
captures compare shadows against independent segment/box or plane intersections
and converged emitter integration. They also exercise textured UV1 cutouts,
off-screen/thin blockers, motion, sample/filter budgets, empty scenes, glTF/BIM/
USD/IFC reloads, mode/technique changes, MSAA, resize and TAA. GPU tests require a
validation layer and skip unsupported ray traversal rather than treating it as a
passed quality comparison.

960×540 measurements on RTX 2080 SUPER (Visual Studio Release,
2026-10-06): rectangle/disk penumbra mean absolute visibility error 0.0062–0.0116
against 256×256 independent emitter integration; hard point mismatch 0; forced
raster fallback pixel-identical in both techniques. With 32 emitter samples,
query time was 0.70–0.79 ms and filtering 0.47–0.74 ms. The seven-instance fixture used
20,480 bytes AS storage, 4,096 bytes input and 14,848 bytes retained scratch;
all live allocations including visibility/history images totalled 108,177,616
bytes. These are actual VMA allocations rather than requested payload sizes.
Image cost scales with resolution and layer allocation alignment. Measurements
are fixture/device-specific; raster shadow atlases remain allocated for fallback
lights and transparent receivers.

The complete runtime suite passed 138 evidence records in 319 seconds, including
0.3 m, 0.8 m and 1.4 m emitters. At eight samples, filtering reduced penumbra
error from 0.1057–0.1059 to 0.0147–0.0149. A continuously moving blocker reused all
seven BLAS while rebuilding TLAS: GPU build time 0.054–0.058 ms, queries
0.750–0.760 ms and filtering 0.451–0.461 ms. Retaining the in-flight generations
raised total allocations to 108,189,936 bytes. Geometry/light changes rejected
history immediately; settled visibility error was below 0.016. Validation and
synchronization checks stayed clean across all lifecycle captures.

The same isolated rectangle-light fixture, with matched exposure and camera:

| Raster area shadows | Ray-query area shadows, 32 samples and filtering |
| --- | --- |
| ![Raster rectangle visibility](images/ray-shadows/raster-rectangle.png) | ![Ray-query rectangle visibility](images/ray-shadows/ray-rectangle.png) |

Runtime evidence is written to `out/build/visual-studio/test_results/ray-shadows`.
The Windows packaging script requires both ray compute SPIR-V files. The package
smoke runner verifies extracted forward/deferred queries and forced fallback
from an unrelated directory with SDK/build-tool environment paths removed.

See [epic #41](https://github.com/semihguresci/VulkanSceneRenderer/issues/41),
[coordinate conventions](coordinate-conventions.md), the
[Khronos ray-query sample](https://docs.vulkan.org/samples/latest/samples/extensions/ray_queries/README.html)
and [Slang inline queries](https://docs.shader-slang.org/en/stable/external/core-module-reference/types/rayquery-03/tracerayinline-058.html).
