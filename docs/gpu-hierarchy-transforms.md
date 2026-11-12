# Optional Vulkan hierarchy propagation

Enable **Scene Controls → GPU hierarchy transforms**, or launch with
`--gpu-transforms`. The CPU path remains the default. Current model/normal
matrices, complete render-object records, ray-instance transforms and optional
closest-hit transform data are published on the GPU. CPU mirroring remains;
the active GPU path skips full per-frame CPU object-buffer writes.

## Current behavior

`GpuHierarchyPass` orders the authoring forest into topological levels without
recursion. A compute dispatch evaluates each level after its parents, then a
second phase writes complete 224-byte `ObjectData` records into the renderer's
existing object SSBO. It combines current model/inverse-transpose normal
matrices with compact CPU-authored metadata. Both forward and deferred raster paths,
including shadow, selection and velocity consumers, use that SSBO.

Each swapchain image owns independent local, topology, world, binding and
metadata buffers. Only changed local records are uploaded on ordinary pose updates.
Topology changes rebuild the level order; changed object bindings update their
mapping. A successful graphics submission commits the world cache with the
compute shader identity used to record it. Unchanged hierarchies skip level
dispatches. Each image recomputes its worlds after a shader replacement, even
if it was inactive during several reloads. Discarded recordings cannot
publish uncomputed worlds.

World matrices follow GLM/glTF column vectors in the right-handed world. Normal
matrices use cofactors divided by the determinant, preserving mirrored and
nonuniformly scaled transforms. Roots take an explicit branch before parent
buffer addressing; the invalid-parent sentinel is never a storage-buffer index.

The CPU graph, ECS objects, conservative bounds, winding routes, picking,
physics, light/camera extraction and ray-scene topology continue to reconcile as
before. GPU propagation preserves object/material identities, bounds,
`previousModel` and temporal tokens. Skinning/morphing remains a separate pass;
its evaluated geometry and CPU bounds retain their existing contract.

Disabling propagation or falling back restores each previously GPU-written
image's CPU object data before TAA prepares motion history. This also removes
intentional transform changes introduced by a live shader edit. The shader is
registered with the ordinary managed compute reload path.

## Lifetime and synchronization

Prepare an image only after its graphics fence retires. Input flushes precede
the compute pass; barriers order prior readers/writers, hierarchy levels, and
the subsequent raster/compute/transfer consumers. Graphics submission commits
the cache. Resize retires the device and destroys the old image resources;
scene replacement rebinds the existing per-image pass to the new object SSBO.
The compute pipeline requires six storage-buffer bindings. Metadata uses VMA
`AUTO` with mapped sequential host access, allowing host-visible device memory
when available; placement remains device-dependent.

`RayInstanceTransformPass` writes private row-major Vulkan 3×4 instance records
from the current GPU object SSBO before TLAS construction. CPU templates retain
custom IDs, masks, facing flags and BLAS addresses. Optional closest-hit outputs
receive the matching column-major model, normal columns and handedness. These
outputs live in device-local memory and remain immutable with the generation;
this is a new-generation TLAS build, not an in-place refit.

The primary mesh provider supplies GPU object indices in actual draw/instance
order. CPU-only providers in the same TLAS retain explicit fallback matrices.
One GPU object stream is supported per extracted ray scene. Packed inputs cost
24 bytes per instance, plus 48 bytes for each CPU-only fallback. GPU-bound
instances no longer upload a CPU-authored 64-byte AS instance record or an
additional 112-byte closest-hit transform record. CPU extraction/validation
and other renderer uploads remain outside this byte count.

The generation owns the packed input and descriptor pool; a retained producer
state keeps its pipeline layout alive. Source object buffers are needed through
the graphics submission only. Later ray queries and asynchronous DDGI read
private generation outputs. The existing graphics-timeline wait makes them
visible to DDGI's second queue in the same graphics queue family.

GPU path selection and shader-generation changes invalidate the ray-instance
generation without invalidating unchanged BLAS geometry. Light-only animation
retains the ray generation and cached hierarchy levels. Source bounds, material
and deformation flags retain their CPU contracts.

Invalid parent indices, cycles, nonfinite/nonaffine or singular local matrices,
oversized storage ranges and capacity failures select the CPU fallback with
a diagnostic. Limits are 262,144 nodes, 4,096 hierarchy levels, 1,000,000 object
bindings and the device's storage-buffer range; the effective limit is the
smallest applicable bound. Inactive and retired nodes remain part of the
stable authoring forest.

## Measurements and validation

Each mapped object uploads a changed 112-byte metadata tail containing IDs,
conservative world bounds, the submitted previous model and temporal tokens.
Unchanged tails are reused per image. Diagnostic objects without a graph node
also upload a changed 112-byte CPU model/normal prefix. Compute writes every
output field even when worlds and metadata are retained. TAA prepares previous
models before metadata publication; setup or metadata failure restores complete
CPU records for the acquired image, including prepared temporal history.

Capture telemetry has a `hierarchyTransforms` object. `uploadedBytes` counts
this pass's local/topology/binding input copies. `objectMetadataBytes` and
`objectMetadataMs` measure compact metadata copies and CPU preparation.
`cpuObjectUploadBytes` counts the primary controller/frontend full-record writes
for the frame, including fallback and temporal uploads. Initial scene staging,
sidecar providers and other writers are outside these counters; the values are
copied payload bytes, not physical PCIe traffic. `prepareMs` measures hierarchy
CPU preparation. `previousGpuMs` is a completed earlier use of the same image's
timestamp queries, not the current capture frame. `dispatchedLevels` distinguishes
an updated hierarchy from a retained-world object publication.

`gpu_hierarchy_tests` executes the production Slang pipeline on Vulkan with
synchronization validation and reads back worlds and complete object records.
Set `CONTAINER_RUN_GPU_HIERARCHY=1` to run it. Tests cover unordered forests,
broad and deep trees, nested rotations, nonuniform/mirrored scales, reparenting,
scene replacement, image independence, sparse uploads, rejected inputs and
unsubmitted preparation. Poisoned outputs verify complete record publication;
nonsymmetric previous/diagnostic matrices verify byte layout. The 50,000-node
test reports an independent CPU matrix-reference time, preparation time,
hierarchy/metadata input bytes, metadata CPU cost and actual GPU timestamps.
These are component measurements, not an end-to-end speedup.

`tests/validation/hierarchy_transform_runtime_regression.py` compares CPU/GPU
images on both raster techniques with animated static/skinned/morphed
instancing, live path switches, MSAA, resize and scene replacement. `--ray`
also exercises hard ray shadows and asynchronous DDGI with zero GI intensity;
that DDGI mode verifies execution and synchronization, not indirect-light quality.
`--taa` instead uses continuous animation and TAA at MSAA 1x to check submitted
motion history through CPU/GPU switches and scene replacement.
The `hierarchy` section of `runtime_shader_reload_regression.py` edits an
isolated shader source, reloads it, then verifies CPU restoration and GPU
reactivation without modifying workspace shaders. It also moves GPU TLAS inputs
through a live compute edit and checks that an actual directional shadow
disappears, returns on CPU fallback, and disappears again on GPU reactivation.
It also removes the isolated compiled hierarchy shader, verifies full CPU
restoration while an object moves, then restores the shader and verifies GPU
recovery. No workspace shaders or production fault-injection hooks are used.

`ray_scene_gpu_tests` additionally uses deliberately different valid CPU/GPU
matrices to prove actual ray queries follow GPU publication. It checks masks,
IDs, normal/handedness readback, a CPU-only fallback instance and retained older
TLAS poses after publishing a newer generation. `--gi` on the native hierarchy
regression enables visible synchronous GI. The separate
`gpu_light_transform_reuse_runtime_regression.py` animates authored light
intensity and position. Intensity changes retain cached hierarchy levels;
position changes update the light node. Both retain the ray generation and
produce visible, exactly reversible direct-light changes.

## Large-scene assessment and limits

`hierarchy_large_scene_runtime_benchmark.py` runs a continuously animated
20,000-instance glTF grid on both raster paths, with optional hard ray shadows.
`--max-scene-objects N` raises the existing scene capacity for the run; the
ordinary default remains 4,096, and device/graph limits still apply. The benchmark
checks visible motion, image agreement, upload counters and shared BLAS reuse.
Use a validation-enabled run for correctness and `--no-validation` separately
for timing. CPU and graph timings summarize 35 warmed, uncaptured frames;
hierarchy timings summarize six samples. Screenshot frames are excluded from
the CPU timing summary. Capture mode serializes resource preparation and these
figures do not represent unrestricted interactive frame rates. Frame-graph GPU
time excludes the separately reported hierarchy pass and is not total GPU latency.

The CPU mirror and CPU extraction/validation remain in place for existing
consumers. Reducing object payload does not establish a general speedup: matrix
validation, input comparison, metadata preparation and compute all have costs.
CPU remains the default. The versioned measurements and acceptance evidence
are recorded in `docs/feature-parity-progress.md`.
