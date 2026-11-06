# glTF skinning and morph deformation

Issue #91 now connects imported skins and morph targets to native runtime
draws, temporal velocity, bounds, CPU picking and ray geometry. Independent
baked-mesh native comparisons cover the integration as described below. The
remaining limitations in this document still apply; successful compute-only
tests are not sufficient evidence that every runtime consumer is correct.

## Imported data and numerical reference

`include/Container/geometry/GltfDeformation.h` exposes:

- `LoadDeformationData(source, runtimePrimitives)` extracts deformation data
  from a `tinygltf::Model`, using the existing loader's sanitized runtime
  vertices. Primitive order is mesh-major, then primitive-major, matching
  `Model::meshes()`. `sourceMesh` and `sourcePrimitive` retain source indices;
  `nodes` and `skins` retain the source array index spaces.
- `ComputeSkinPalette(skin, meshNodeWorld, nodeWorldMatrices)` produces
  `inverse(meshNodeWorld) * jointWorld * inverseBind`. The result is node-local;
  the ordinary subsequent model transform cancels the owning mesh node's
  transform as glTF skinning requires. Mesh and joint worlds must include the
  same outer import/root transform.
- `EvaluatePrimitive(primitive, morphWeights, skinPalette)` is the CPU
  numerical reference. It returns new vertices without mutating bind geometry.

All consecutive `JOINTS_n`/`WEIGHTS_n` sets are retained. Joint accessors accept
unnormalized unsigned bytes or unsigned shorts; weights accept floats or
normalized unsigned bytes/shorts. Weights are normalized across every set,
not separately per group of four. Negative, non-finite and all-zero joint
weights are rejected. Every joint index, including zero-weight entries, is
checked against each referencing node's skin.

Inverse bind matrices are column-major float `MAT4` values. Missing matrices
default to identity. An accessor may contain more matrices than joints, as
the glTF specification permits. Non-finite or non-affine matrices and invalid
node, skin, joint or accessor references are rejected.

Morph targets retain `POSITION`, `NORMAL` and `TANGENT` float `VEC3` deltas;
an omitted semantic has zero displacement. Accessor count must equal the bind
vertex count. Interleaved and sparse deformation accessors are supported with
range, alignment, stride and sparse-index checks. The existing base geometry
loader has its own accessor support restrictions.

Node default weights override mesh defaults; omitted weights resolve to zero.
Morph weights can be negative or greater than one and need not sum to one.
These defaults apply even when the model has no animation clips.

Morphing precedes skinning. Positions use the weighted linear skin transform.
Normals use its inverse transpose; the implementation evaluates equivalent
cofactors after scaling the matrix to avoid a determinant cutoff tied to world
units. Tangents use its linear transform and are orthogonalized against the
normalized normal. Mirrored skin transforms multiply `tangent.w` by the
determinant sign. The later object transform still handles its own mirror sign.
Singular skin blends produce a deterministic finite fallback normal/tangent
frame. Palette inversion/composition uses double precision on the CPU before
uploading floats.

The ordinary reference consumes sanitized bind frames and supplied morph deltas.
`GltfDeformationFrames` handles missing base frames for morphing primitives.
It expands indexed triangle corners while retaining a source-vertex map and
every influence/target attribute. After applying all morph weights it generates
flat normals when base `NORMAL` is absent, then generates MikkTSpace tangents
using the normal texture's selected UV0/UV1 set. Missing normals cause supplied
tangents and tangent deltas to be ignored. Skinning follows frame generation.
An authored base normal or tangent omitted by an individual target retains its
base value in the ordinary authored-frame path.

## Vulkan compute API

`GltfDeformationPass` lives in
`include/Container/renderer/scene/GltfDeformationPass.h`; its production kernel
is `shaders/gltf_deformation.slang`, entry point `computeMain`.

1. Construct with the Vulkan device, allocation manager and pipeline manager.
   A second constructor accepts their existing `VulkanMemoryManager` directly,
   including for standalone numerical validation.
2. Call `createResources(shaderDir, frameCount)`. The shader is loaded from
   `shaderDir/spv_shaders/gltf_deformation.comp.spv`.
3. Call `setScene(initialVertices, instances)`. An instance specifies a source
   `DeformationPrimitive` and an output vertex offset. Instance ranges must be
   distinct and inside the supplied complete scene stream. Bind vertices are
   copied into those ranges; unmapped scene vertices are copied unchanged.
4. After waiting for a frame slot's fence, call
   `uploadFrame(frameIndex, poses)`, with one pose per instance. Spans are copied
   immediately and need not outlive the call.
5. Call `record(commandBuffer, frameIndex)` on the graphics queue before any
   raster, velocity, readback or acceleration structure consumer.
6. Bind the `BufferSlice` from `currentVertices(frameIndex)` for ordinary draws.
   `previousVertices(frameIndex)` has the same layout and ranges for velocity.

Each pose supplies current/previous morph weights and skin palettes. Empty
current weights use mesh defaults; callers supply resolved node defaults for
node overrides. An empty palette leaves an instance unskinned. Empty previous
inputs use the corresponding current input. `resetHistory` evaluates previous
geometry at the current pose, for seeks, clip changes and first frames.
Previous data must represent the last successfully submitted frame, rather
than the previous use of the same swapchain/frame slot.

For exceptional missing-base-frame instances, optional `evaluatedVertices` and
`previousEvaluatedVertices` spans supply complete final node-local CPU vertices.
The host copies their exact 72-byte ABI into the existing mapped pose buffer;
compute publishes them through the same output buffers and visibility barriers.
Empty previous vertices use current vertices; resets also copy current to
previous. The pass rejects a morphing primitive with missing base frames when
its required evaluated current vertices are absent. Ordinary authored-frame
instances continue to run morphing and skinning in the compute shader.

The exceptional path runs frame regeneration and final skinning on the CPU,
then publishes evaluated vertices through compute. It therefore adds CPU work
and up to two full vertex uploads per moving instance. Static/skin-only primitives
generate missing tangents once in the base loader, using the normal texture's
selected UV0/UV1 set, including a `KHR_texture_transform.texCoord` override.
MikkTSpace generation and geometric repair use the same selection. UV selection
retains valid authored tangents; repair also preserves valid tangents beside
malformed vertices. Unsupported UV selectors fail when frame generation is
required. Skin-only deformation then uses the ordinary GPU path.

Shared primitives reuse packed targets and influences while instance jobs
retain independent weights, palettes and output ranges. No influence-set or
morph-target count is silently truncated. Packed byte addressing and the
device's `maxStorageBufferRange` impose explicit checked storage limits.

## Vertex ABI and synchronization

The output is the exact 72-byte `geometry::Vertex`:

| Field | Byte offset | Bytes |
| --- | ---: | ---: |
| Position | 0 | 12 |
| Color | 12 | 12 |
| UV0 | 24 | 8 |
| UV1 | 32 | 8 |
| Normal | 40 | 12 |
| Tangent including handedness | 52 | 16 |
| Color alpha (defaults to 1) | 68 | 4 |

RGB and every earlier field retain their offsets. Raster attribute location 8
reads alpha; locations 6/7 remain previous position/normal. Morphing and skinning
preserve the complete RGBA color in both current and previous streams, including
the CPU frame-regeneration override path.

Compile-time assertions enforce the size and offsets. Four raw byte-address
storage buffers avoid `std430` vector padding: immutable scene inputs, per-frame
pose inputs, current output and previous output. Matrices are read as four
column vectors; no matrix transpose or coordinate-system correction is added.
Changing `Vertex` requires updating this kernel's byte ABI as well.

Immutable inputs are uploaded once; weights and palettes have independent
mapped upload buffers for every frame slot. Outputs use device-preferred
memory with storage, vertex and transfer-source usage. Buffer device address
and acceleration-structure input usage are included when enabled by the
device.

The caller retires all GPU readers before `setScene`, resource recreation or
destruction, and waits for the specific frame slot before `uploadFrame`.
The pass does not add device-wide idle waits. Recording inserts visibility
and reuse barriers for host writes/earlier readers before compute, followed by
compute-write visibility for vertex fetch, shader reads, transfer readback and,
when enabled, acceleration structure builds. Cross-queue ownership transfers
are not provided: recording and consumers use the graphics queue.

## Validation

`gltf_deformation_tests` has 32 CPU tests with independently known positions
and frames: inverse-bind rest poses, nested joints, inverse mesh cancellation,
morph order, static/default weights, independent shared-mesh instances,
integer weights, multiple influence sets, interleaved/sparse data and malformed
accessors. Nonuniform, mirrored, tiny/large and singular transforms exercise
normal/tangent handling.

`gltf_deformation_gpu_tests` has six opt-in tests that execute the production
compute pass, read back both vertex streams and compare them with the CPU
reference. They cover 257 vertices across dispatch boundaries, twelve joint
influences, three morph targets, previous poses, distinct shared-mesh instances,
history reset, frame-slot reuse/recreation, morph-only instances, collapsed
skin blends, evaluated-frame byte copies and invalid uploads. Vulkan synchronization validation is enabled;
the validated run completed with zero validation errors.

For the Visual Studio build:

```powershell
cmake --build out/build/visual-studio --config Release --target gltf_deformation_tests gltf_deformation_gpu_tests
ctest --test-dir out/build/visual-studio -C Release -R '^gltf_deformation_tests$' --output-on-failure
$env:CONTAINER_RUN_GPU_GLTF_DEFORMATION = '1'
ctest --test-dir out/build/visual-studio -C Release -R '^gltf_deformation_gpu_tests$' --output-on-failure
```

The GPU target's `CONTAINER_GLTF_DEFORMATION_SHADER_DIR` compile definition
selects the build shader directory. An environment variable of the same name
can override it for an isolated run.

## Scene reconciliation and runtime consumers

`GltfDeformationScene` owns the bind/current/previous CPU streams and remapped
indices. Source static ranges stay in place; each renderable skin or morph
instance receives an appended independent range. Plain static/procedural
scenes do not acquire dynamic streams. Per-instance local bounds and skin
orientation are evaluated from the current pose. Uniform reversed skin
orientation adjusts culling; mixed or degenerate skin blends disable culling.
Object transforms retain their separate determinant sign.

The `meshOwnerGraphNodes` span is indexed by runtime graph node and contains
the owning runtime mesh-group graph node ID. The owner's `sourceNodeIndex`
selects the source glTF skin and resolved node weights. This association is
retained explicitly across primitive reparenting; the current parent is not
used as the skin owner. Missing or ambiguous required owners/joints are errors.
Palettes use the owning group world transform, while the actual renderable
child world transform remains in ordinary object data.

`update()` evaluates current geometry and exposes GPU instance/pose spans.
`commitSubmitted()` advances previous geometry only after successful queue
submission. Abandoned updates retain the last submitted pose. Explicit resets
make current and previous geometry equal until submission. Compatible graph
identity/hierarchy changes preserve surviving history. Geometry revisions
advance for actual palette/weight changes rather than unrelated graph edits.
Twenty-one scene-state tests cover these contracts, including indexed corner
expansion, regenerated normal history, UV1 tangent handedness and preservation
of the ordinary authored-frame GPU path. Six production compute/readback tests
include exact fallback byte copies, current/previous/reset behavior and rejected
missing or malformed frame uploads. The frame-generation module has twelve
independent CPU tests.

The frontend uploads expanded bind/index streams when topology changes, then
records compute before raster/velocity consumers. Current geometry is bound
as the ordinary vertex stream; previous geometry supplies the dedicated
previous-position/normal vertex binding. SceneController bounds and actual
CPU triangle picking use evaluated CPU streams. Ray scene extraction uses
the same current geometry and geometry revisions for surface data and BLAS
rebuilds. The frontend commits history after submission, and timeline/temporal
resets propagate into previous deformation poses.

## Native runtime regression

`tests/validation/gltf_deformation_runtime_regression.py` generates embedded
glTF fixtures and independently bakes static reference meshes with NumPy.
It never calls the renderer's CPU deformation evaluator. Cases include mesh
defaults without animation, node overrides, morph animation, nested-joint
motion with inverse-mesh cancellation, shared meshes with independent skins
and weights, normalized integer weights in two influence sets, nonuniform and
mirrored transforms, and combined morph/skin deformation. Both deferred and
forward raster paths compare lit captures. Mirrored/nonuniform cases also
compare the deferred normal debug view; forward does not expose that diagnostic.

Native telemetry verifies the actual current/previous GPU bindings, distinct
vertex/index ranges, resolved weights, first position/normal/tangent, local
bounds, orientation and the instance world matrix. It projects a camera-facing
triangle and calls the actual production CPU picker, checking its node and
world hit against the deformed triangle.

Moving TAA velocity captures are compared with independently projected
previous/current triangle positions and a paused zero-motion control. Hard
ray cases compare against static baked references and a receiver-only scene;
an independent segment/triangle oracle predicts lit and shadowed floor pixels.
The mirrored ray fixture is an open single-sided quad so an incorrect facing
flag cannot be hidden by a closed solid's exit face. The visibility oracle
checks single-sided light-facing geometry; an open plane lit from behind
correctly casts no shadow.

```powershell
python tests/validation/gltf_deformation_runtime_regression.py `
  --renderer out/build/feature-parity/VulkanSceneRenderer.exe `
  --output out/validation/gltf-deformation-runtime --suite all
```

The full default matrix contains 32 dynamic/static reference pairs, 10
moving/paused velocity checks and 8 receiver-only controls: 92 native captures.
Use `--suite geometry`, `--suite velocity`, or `--suite rays`, with optional
`--case`, `--technique` and `--time` filters for focused runs. `--generate-only`
writes fixture files without starting the renderer. JSON results record exact
counts, numerical/image metrics, executable SHA-256, commands and artifact
paths. Each native launch rejects Vulkan/synchronization validation errors,
and a changed executable hash rejects a run spanning a rebuild.

A separate directional raster-shadow suite checks that cascades consume the
current GPU vertex stream. It removes point lights and IBL, enables a directional
light with direction `(-0.45, -1, -0.3)`, disables ray shadows and verifies actual
shadow-cascade pass activity. Positive and mirrored casters compare with baked
references and receiver-only controls in both raster techniques: four reference
pairs plus four controls, twelve additional captures.

```powershell
python tests/validation/gltf_deformation_runtime_regression.py `
  --renderer out/build/feature-parity/VulkanSceneRenderer.exe `
  --output out/validation/gltf-deformation-raster-shadows --suite raster-shadows
```

The validated Windows run on an NVIDIA GeForce RTX 2080 SUPER completed all 92
main captures, 12 directional raster-shadow captures and 20 missing-frame
fallback captures on one unchanged executable, with zero Vulkan or
synchronization validation errors. The main reference comparisons had maximum
ROI mean absolute error `0.0000981` on an 8-bit channel scale and no pixels above
the failure threshold. Maximum velocity component error was `0.0478` pixels;
paused controls stayed below `0.066` pixels in magnitude. Independently predicted
ray shadows covered at least 1,291 exposed receiver pixels. Directional raster
shadows covered 647 mirrored/1,151 positive receiver pixels, with median
darkening of 26–28.33 channel levels and zero 95th-percentile error on lit pixels.
Artifacts and per-capture commands are in
`out/validation/gltf-deformation-runtime-final/results.json`,
`out/validation/gltf-deformation-raster-shadows-final/results.json` and
`out/validation/gltf-deformation-fallback-runtime/results.json` for that run.
`out/validation/gltf-deformation-final-summary.json` combines all 124 captures:
42 independent reference pairs, 14 moving/paused velocity checks and 12
receiver-only controls.

The separate `--suite fallback` matrix creates an indexed shared-corner mesh
with missing normals, two nonplanar position targets at partial weights and
intentionally ignored supplied tangent data, followed by nonuniform skinning.
A second single-triangle fixture omits tangents and uses a UV1 normal texture.
NumPy independently computes triangle cross-product normals and analytical
UV derivatives for the baked reference; the production helper is never called.
Source four-vertex geometry remains unchanged while the first instance expands
to six runtime corners. Lit/normal debug, bounds, real CPU picking and temporal
velocity checks comprise six reference pairs and four moving/paused controls:
twenty additional native captures. All six reference pairs were pixel-identical
in the validated run; the largest velocity component error was `0.0454` pixels.

```powershell
python tests/validation/gltf_deformation_runtime_regression.py `
  --renderer out/build/feature-parity/VulkanSceneRenderer.exe `
  --output out/validation/gltf-deformation-fallback-runtime --suite fallback
```

Static and skin-only normal-map UV selection has a separate analytical native
regression. Four generated triangle cases cover direct UV1 selection and
texture-transform UV1 overrides. Each missing-tangent mesh is compared with an
explicitly authored `T = -Y, w = +1` frame and a deliberately wrong UV0 frame
`T = +Y, w = -1`. The script verifies these frames from the UV Jacobian without
calling MikkTSpace or the production deformation evaluator, and keeps source
vertex/index order identical between fixtures. Skin cases also check the
analytical transformed tangent and GPU source/expanded vertex counts.

```powershell
python tests/validation/gltf_tangent_uv_runtime_regression.py `
  --renderer out/build/feature-parity/VulkanSceneRenderer.exe `
  --output out/feature-parity/gltf-tangent-uv-runtime
```

The validated run passed all twelve analytical reference pairs and twelve
wrong-UV0 controls across 36 native captures, with zero Vulkan/synchronization
validation errors. All reference pairs were pixel-identical. Wrong frames
changed at least 8.62% of pixels. Lit output runs in both raster techniques;
the normal diagnostic runs in deferred rendering because forward rendering
does not advertise it. Results record that unsupported combination explicitly,
along with the frozen executable hash, metrics and capture commands.
`--generate-only` prepares fixtures without native validation.

Runtime validation must be repeated after changes to stream binding, geometry
publication, temporal history, frame fallback or ray facing.

For `EXT_mesh_gpu_instancing`, draw children of one owner/primitive share its
deformation stream, palette, weights and submitted previous pose. Distinct
owners retain independent evaluation. Removing a representative draw preserves
the surviving owner's submitted history. Instance-local transforms apply after
this shared owner-local pose; see [mesh instancing](gltf-instancing.md).

The normative input rules follow the
[glTF 2.0 mesh, morph target and skin specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#geometry).
Engine transform and tangent rules follow
[Coordinate and rendering conventions](coordinate-conventions.md).

## GPU depth readback for deformed picking

Forward raster now records the shared transparent-picking pass after the
depth prepasses and before it transitions scene depth for shader sampling.
The pass clears the pick ID attachment and writes fresh IDs and depth.

A capture event can request `"depthProbe": [x, y]` for a screenshot frame.
The telemetry records `opaque` and `pick` from the submitted frame using the
viewport readback path. Coordinates are framebuffer pixels and the depth
values use reverse-Z. An unavailable sample is reported as JSON null.

The native fixture checks a transparent morph surface in front of an opaque
receiver. Both raster techniques passed at 1x and 4x MSAA, including a
swapchain resize, background depth, out-of-bounds coordinates, and a later
frame without a probe. Renderer `1cc387f1` passed 180 cases across six test
executables and 20 native depth comparisons. Evidence:
`out/feature-parity/pick-depth-normal-checkpoint.json`.
