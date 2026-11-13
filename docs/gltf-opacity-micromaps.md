# Experimental glTF opacity micromaps

Launch with `--experimental-opacity-micromaps` to use authored opacity
micromaps for supported static glTF alpha-mask triangles. The renderer enables
`VK_EXT_opacity_micromap` when the selected ray-query device advertises both the
extension and its feature. Other devices and the default launch use sampled
alpha. The flag only affects ray traversal. Raster material evaluation retains
its existing coverage path.

Declare `EXT_mesh_opacity_micromap` in `extensionsUsed` and supply the core
alpha-mask material and texture. The loader rejects a declaration in
`extensionsRequired` before reading dependent resources. The acceleration
profile requires the core fallback for techniques and devices that sample alpha.

The loader follows the `EXT_mesh_opacity_micromap` asset layout consumed by the
pinned NVIDIA renderer. It validates source JSON before integer narrowing,
then checks packed data, triangle records, usage histograms, primitive
triangle counts, and signed/unsigned lookup values. Checked legacy stride and
integer encodings are normalized to the Vulkan EXT input layout. Invalid maps
or primitive associations report the reason and use sampled alpha.
[Pinned renderer](https://github.com/nvpro-samples/vk_gltf_renderer/blob/174625d5c621c088bf3610377999f9dbdcf5c3b5/src/gltf_scene_omm.cpp) · [glTF draft](https://github.com/KhronosGroup/glTF/pull/2623)

The original static mask, cutoff, base alpha, texture identity, sampler, and
texture transform must still match. The supported profile uses ordinary
base-color PNG and JPEG uploads. It uses the offline author's coverage
classification for that texture. Textureless masks, blended materials,
transmission, vertex colors, and additional opacity inputs use sampled alpha.
A shared geometry range also falls back when
any active placement changes the mask. Geometry edits, local deformation,
compressed topology, and repaired winding fall back. The linked-assets
flattener and portable save/merge use the core glTF material and geometry and
remove the micromap extension associations. Portable output can retain
unreferenced bytes from source buffers.
Undoing a merge restores eligible authored maps with the original geometry.
Redo restores the merged scene's sampled-alpha route.

Proven transparent microtriangles can be discarded by traversal. Other hits
still reach the normal shader candidate path for material and clipping checks.
Forward MSAA uses sampled alpha for the whole ray scene because its receiver
identity check needs to see masked triangles at raster sample locations.
Path tracing, DDGI, deferred shadows, and forward rendering at one sample can
use eligible micromaps. [Vulkan EXT attachment](https://docs.vulkan.org/refpages/latest/refpages/source/VkAccelerationStructureTrianglesOpacityMicromapEXT.html)

Capture telemetry reports whether the feature was requested and enabled, how
many maps and BLAS attachments were built, how many attachments fell back, and
map storage bytes. The existing acceleration/input/scratch and retained-memory
counters include micromap allocations. A map and its BLAS stay alive until the
last referencing frame retires.

The Vulkan contract exercises actual packed two- and four-state records and
all four special indices. On the RTX 2080 SUPER, its 4,096-ray fixture matched
sampled alpha in hit, identity, and distance. One run measured 34.784 µs with
sampled alpha and 32.512 µs with micromaps. The corresponding total allocations
were 14,080 and 18,176 bytes, including 640 bytes of map storage. These figures
describe this fixture and device.

Run the CPU `gltf_opacity_micromap_tests` and `ray_scene_tests`. Set
`CONTAINER_RUN_GPU_RAY_QUERY=1` for `ray_scene_gpu_tests`.

The renderer test checks pixel parity with sampled alpha, material edits,
merge undo and redo, clipping, malformed data, and portable export. Run it on a ray-query
device with opacity micromaps. Supply the Vulkan Profiles layer to also simulate
a device that exposes the extension with its `micromap` feature disabled.

```powershell
$env:CONTAINER_RUN_GPU_GLTF_OPACITY_MICROMAPS = '1'
$env:CONTAINER_OPACITY_MICROMAP_VALIDATION_LAYER = "$env:VULKAN_SDK/Bin"
$env:CONTAINER_OPACITY_MICROMAP_PROFILES_LAYER = "$env:VULKAN_SDK/Bin"
ctest --test-dir out/build/windows-release -C Release -R '^gltf_opacity_micromap_gpu_regression$' --output-on-failure
```
