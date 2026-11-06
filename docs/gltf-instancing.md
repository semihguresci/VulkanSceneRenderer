# glTF mesh instancing

`EXT_mesh_gpu_instancing` uses shared mesh-major geometry and one scene primitive
child per instance/primitive. The original source node remains the animation,
visibility and skin/morph owner. Authored child nodes are instantiated once
under that owner. There is no additional non-instanced fallback draw.

The detached loader validates extension schemas and accessor IDs before external
resource reads. Its immutable checked-accessor parser supports FLOAT translation
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
and DDGI. Native GPU selection readback has no capture action; production CPU
picks and per-instance graph identities are measured.

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
