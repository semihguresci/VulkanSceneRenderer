# glTF mesh instancing

`EXT_mesh_gpu_instancing` uses shared mesh-major geometry and one scene primitive
child per instance/primitive. The original source node remains the animation,
visibility and skin/morph owner. Authored child nodes are instantiated once
under that owner. There is no additional non-instanced fallback draw.

The detached loader validates extension schemas and accessor IDs before external
resource reads. It accepts integral decimal and exponent encodings for the
instancing fields defined as integers and validates their storage references
before TinyGLTF can narrow them. [glTF 2.0 JSON encoding](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#json-encoding).

The checked-accessor parser supports FLOAT translation
and scale, FLOAT or normalized signed BYTE/SHORT rotation, sparse/strided storage,
and identity defaults for omitted TRS components. Custom attributes must share the
instance count; custom-only attributes establish identity instances. Custom IDs
remain source references and exact integer components, including values beyond
binary32 integer precision. Quaternions must be unit within storage precision
and are normalized before constructing `T*R*S`.

World transforms follow `nodeWorld * instanceLocal`. Nested nonuniform scales
and rotations may produce shear. Culling uses a conservative spectral-norm bound
rather than the largest matrix-column length. Mirrored transforms use the existing
raster winding and ray-facing conventions. Each draw has a distinct graph ID,
source mesh/primitive/instance identity and useful label; production CPU picks
resolve those graph IDs. Standard visibility inheritance applies to all children.

Static vertices/indices and reusable BLAS are shared. Skin/morph evaluation uses
one stream, weight set and palette per original owner/primitive, shared by its
instance children. Different owners retain independent deformation. Submitted
previous poses are keyed by owner/primitive, so removing a representative draw
does not reset surviving instance history. Vulkan object transforms and TLAS
placements remain per draw, while shared pose geometry feeds raster, picking,
shadows, DDGI and temporal consumers.

Default CPU limits are one million instances, four million expanded primitives
and 128 MiB of matrix output. Decoded matrices count against finite aggregate
loader input budgets. Runtime graph construction is detached and checks table
bounds, affine/finite composition, graph IDs and the configured object limit
(default 4096) before retiring live assets. Singular instance transforms currently
require geometry baking: they are explicitly rejected during detached runtime
preflight because the Vulkan TLAS path requires invertible transforms. The
capability registry reports partial support for this limitation.

Export emits singleton instance attributes on mesh nodes, preserving the order
of skinning followed by instancing. Meshless source groups lose the consumed
extension. Source custom attributes are copied as exact stored components,
including sparse/zero bases and normalized signed extrema; they do not pass
through floating-point conversion. Attribute accessors are shared across the
primitives of one source instance. Original extension metadata is retained.
Static/morph instance edits use an outer correction matrix. Post-skin primitive
edits or reparenting that require baking retain an explicit export diagnostic.

CPU coverage includes checked accessor schemas, defaults and budgets; detached
hierarchy composition, authored children and visibility; shared deformation and
submitted history; conservative shear bounds; and GLB/ASCII skin/morph export
with exact custom integer and normalized-byte values.

Native regression entry point:
`tests/validation/gltf_instancing_runtime_regression.py`. It generates independent
ordinary-node references; skinned references duplicate joint subtrees under each
ordinary transform, preserving transform order without using the extension.
Separate modes cover parent/owner animation, authored visibility, hard ray shadows
and DDGI. The `--gpu-picking` run samples the rendered Vulkan pick-ID attachment
through the viewport readback path in both raster techniques. It checks that the
four independently projected pixels resolve to distinct graph nodes and their
source node, mesh, primitive and instance IDs, then checks the same pixels after
hiding and restoring the owner. A separate sample hides every draw. Picking
uses the ID attachment only when a pass wrote it in the completed frame. A
rendered background reports ID `0`, and an unavailable GPU result reports
`null` and lets viewport picking use the CPU fallback. Capture sequences expose
both routes under `depthProbe.sceneInteraction`.

Run the native checks from the repository root with a Release renderer, Python
with NumPy and Pillow, `VK_LAYER_KHRONOS_validation`, and Vulkan ray-query
support for the ray cohort.

```powershell
$renderer = 'out/build/windows-release/VulkanSceneRenderer.exe'
$check = 'tests/validation/gltf_instancing_runtime_regression.py'
python $check --renderer $renderer --output out/validation/instancing/picks --gpu-picking
python $check --renderer $renderer --output out/validation/instancing/animated --animated --roundtrip
python $check --renderer $renderer --output out/validation/instancing/hidden --hidden-owner
python $check --renderer $renderer --output out/validation/instancing/ray --ray
```

The November 12, 2026 Release run on an NVIDIA GeForce RTX 2080 SUPER passed
501 test cases across 15 suites without skips. It passed 98 checks across 92
native captures with zero Vulkan or synchronization validation errors. All 30
positive image comparisons were pixel-exact. The ray fixtures recorded six
TLAS placements with two static BLAS or three deformed-owner and receiver BLAS.
[Windows NVIDIA verification report](../tests/visual-regression/instancing/windows-nvidia/mesh-instancing-results.json).

Global framing bounds also compose every instance and exclude hidden source
subtrees. The follow-up checkpoint passed 830 cases across 39 executables with
all real GPU paths enabled, and 42 native captures / 44 checks in both raster
techniques. Bounds agree with ordinary-node references within 1e-5; positive
images and portable reloads are pixel-exact. Evidence:
`out/feature-parity/instancing-bounds-validation-checkpoint.json` (renderer
`dfa4f5e5df64f74f9dd05106cc1f3e471f14ccfa38173912818516418dcf7c5c`).

The input contract follows the
[Khronos instancing extension](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Vendor/EXT_mesh_gpu_instancing).
Engine matrix, winding and depth conventions remain in
[coordinate conventions](coordinate-conventions.md).
