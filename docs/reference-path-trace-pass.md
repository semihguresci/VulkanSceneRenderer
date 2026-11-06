# Vulkan reference path-trace producer

`ReferencePathTracePass` is the graphics-queue compute producer for progressive
reference rendering. It contains the complete currently developed transport
kernel: diffuse/GGX reflection, thin-sheet transmission, closed homogeneous
dielectrics, nested media, traced-distance Beer absorption, stochastic alpha
coverage, unlit surfaces and direct environment, point, spot, area, directional
and mesh lighting with MIS.

The shared layouts are updated together in host and Slang code:

| Payload | Bytes | Contract |
| --- | ---: | --- |
| `GpuMaterial` | 1184 | Existing material offsets retained with appended extended and experimental inputs |
| `LightingData` | 2224 | Existing 80-byte prefix retained; environment and LUT storage appended |
| `GpuTextureMetadata` | 16 | Sampler index at byte 0, actual bound sampler count at byte 4 |
| `RayTriangleData` | 64 | UV0/UV1 and corner opacity |
| `RaySurfaceTriangleData` | 192 | Optional closest-hit attributes |
| `RaySurfaceInstanceData` | 112 | Model and normal transforms |
| `ReferencePathTraceData` | 384 | Camera, clipping, bounds, sampling, and ground inputs |

`AtmosphereLutData.h` owns only the shared storage type. Sky authoring and solar
calculation are not dependencies of the GPU data header. The current published
scene owner reports its actual sampler table size; subsequent sampler-policy
work can use the same metadata contract. Local deformation winding uses its own
object flag, independently of instance-model mirrors, for both TLAS facing and
CPU/shader surface reconstruction.

Create resources with the shared material and light descriptor layouts. Both
sets must expose their consumed bindings to compute, with valid ranges and
resources for the whole submission. Supply a fence-retained surface scene from
`RayShadowManager::surfaceBindings()`, or explicitly declare an empty scene.
Pass the matching host material and ray-instance metadata to enable mesh-light
sampling. Each retired frame slot uploads the eligible emissive instance IDs.
`limits.z` is the material count; instance/triangle counts come from the borrowed
scene bindings. `limits.w` uses section=1, orthographic=2, cube environment=4,
unsupported scene=8 and volume-material presence=16. The volume bit enables
camera-inside medium discovery; callers must set it when solid materials exist.

Prepare only a fence-retired input slot. Record, submit successfully and call
`commitFrame()` before preparing the next frame. The three persistent RGBA32F
images contain raw mean/count, second moment and diagnostic display. Queue
ordering and barriers serialize their updates across slots; this is not an
independent concurrent producer on several queues. Readers must retire before
recreation/destruction. A reader changing image layout must restore GENERAL.
Diagnostic readback also requires the slot's graphics fence to retire.

Camera, clipping, settings, bindings, caller radiometric revision and retained
pipeline-content identity invalidate accumulated history. Shader preparation,
abandoned reloads and unsubmitted recording do not advance sample addresses.
Retained content identity survives independently of Vulkan handles and does not
retain a GPU allocation. Unchanged shader bytes retain history.

Thin null transmission has a 64-crossing bound; medium discovery has a 128-boundary
bound and the medium stack holds eight nested volumes. Unsupported paths are
shown in diagnostic display and counted separately from rejected numeric samples;
diagnostic colors never enter raw moments. Incompatible overlaps, open/clipped
solid media, coverage holes in solid boundaries, bending delta connections to
point/directional lights, participating media and remaining material
layers are still gaps. Filtering, shading-normal energy, convergence/comparison
and broad scene acceptance remain part of issue #47.

Build `reference_path_trace_gpu_tests`, `path_transport_gpu_tests` and
`managed_compute_pipeline_gpu_tests`. Enable their environment gates and run CTest
with synchronization validation. The production GPU scenes cover refraction,
nested absorption, camera-inside rays, TIR, conductor mixtures, raw moments,
resource lifetimes and repeated shader publication. Raster compatibility and
source-convention checks are recorded separately from transport acceptance.

The clean publication gate passes 200 cases in nine component executables,
including six Vulkan executables, and validates 95 generated SPIR-V modules.
The empty-scene shader and ordinary raster shaders retain their non-ray
capability boundary. Eighty native raster/AA/shadow captures pass with the new
shared layouts; all 80 match the previous published-layout captures exactly.
The broader 194-case convention suite retains its eight existing parent
failures, separately recorded.

The full local editor build passes a separate 656-case gate in 18 executables
(nine Vulkan), 24 volume and nine thin-sheet native captures, and a replay of the
original ACCA IFCX at 128 samples with zero diagnostics. That replay matches
both the previous accepted executable and the older frozen image exactly. The
baseline comparison is recorded separately; no IFCX model is copied. Frozen
publication evidence is `out/feature-parity/reference-backend-publication-acceptance/results.json`.
