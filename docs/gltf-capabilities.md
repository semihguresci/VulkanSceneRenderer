# glTF runtime capabilities

Every glTF/GLB file import passes through the capability gate in
`GltfModelLoader`, including geometry, scene/BIM/volume interchange consumers
and the MaterialX bridge's file-based material import. Source JSON metadata is
checked before external buffers, image decoding or runtime mesh/material
creation. `LoadSourceModel` applies the same policy to material-only assets.
The encoded input and geometry budgets remain active for bounded imports.

The runtime supports glTF **2.0**. `asset.version` and an authored
`asset.minVersion` must be decimal `major.minor` strings. Other major versions
are rejected. A newer 2.x version is accepted with a warning when its minimum
is absent or 2.0; a newer minimum is rejected. An explicit empty minimum is
invalid. This is an engine policy, independent of TinyGLTF's parser behavior.

**Experimental linked assets.** Start the editor with
`--experimental-gltf-linked-assets` to opt into the URI subset of the glTF
2.1 draft pinned by `vk_gltf_renderer` at `174625d5c621`. It reads
`files`, `externalAssets`, `node.externalAsset` and URI-backed `files[].aliases`
using the draft layout. A linked manifest must declare version 2.1 and may
require 2.0 or 2.1. Primary load and Import / Merge use the same profile.
Without the option, linked manifests fail with an opt-in diagnostic. This
profile leaves the normal glTF 2.0 extension and codec gates in place.
[Pinned reference](https://github.com/nvpro-samples/vk_gltf_renderer/tree/174625d5c621c088bf3610377999f9dbdcf5c3b5),
[external-assets draft](https://github.com/KhronosGroup/glTF/issues/2586),
[unified file references draft](https://github.com/KhronosGroup/glTF/issues/2590),
[packaging draft](https://github.com/KhronosGroup/glTF/issues/2589).

Referenced `.gltf` and version-2 `.glb` files resolve relative to the owning manifest.
Aliases redirect a child buffer or image URI to a file in the owner table.
The primary asset's canonical directory bounds every linked file and external
resource read, including aliases and symbolic links. The profile rejects
absolute paths, network URIs, malformed percent escapes, embedded linked files,
missing references and cycles before scene publication. A detached import has
limits of 16 link levels, 64 source/alias contexts, 4,096 placements, 262,144
nodes and 512 MiB of encoded input. The existing merge resource limits also apply.

Repeated placements share mesh, buffer, texture and material definitions from
the same canonical source and alias context. The selected child scene is copied
as distinct node identities under each placement, with its original transforms
and source URI/node provenance. Linked child animations and skins require a
later profile. Unsupported merge metadata, including XMP packet tables, still
rejects composition. See [scene export](gltf-scene-export.md) for save behavior.

`GltfCapabilities.cpp` is the explicit runtime registry. Parsing an extension's
JSON does not establish runtime support. An unknown or unsupported required
extension rejects the asset. An unsupported optional extension warns and uses
the asset's ordinary glTF fallback; such a fallback must actually exist and be
valid. An optional extension cannot make compressed data decodable.

Primary glTF loading and editor merge use the scene-capable gate. The stock
diagnostic scene loads each component through a static composition that keeps
geometry and materials. It reports instancing, animation pointers, variants,
node flags, authored lights and XMP as unavailable before reading resources.
Startup and reload preflight use the same restrictions. The BIM glTF fallback
publishes decoded primitives as a single default-material object. It accepts
supported geometry extensions and reports required scene or material extensions
as unavailable. Optional extensions in either path produce an asset-scoped
warning and retain the ordinary geometry or material fallback.

Supported capabilities include:

- `KHR_texture_transform`: per-slot offset, rotation and scale, including
  selection of UV0 and UV1.
- `KHR_materials_unlit`, `KHR_materials_specular`, `KHR_materials_ior`,
  `KHR_materials_dispersion`, `KHR_materials_emissive_strength`,
  `KHR_materials_transmission`, `KHR_materials_volume`,
  `KHR_materials_clearcoat`, and `KHR_materials_sheen`, with their implemented
  factors and texture slots.
- `KHR_lights_punctual`: point, directional and spot lights, including range
  and spot cones.
- `KHR_node_visibility`: local authored visibility inherited by scene meshes.

Implemented approximations are accepted even when required, with an explicit
`partial_extension_support` warning. `KHR_materials_pbrSpecularGlossiness`
converts into the runtime metallic-roughness workflow. `KHR_lights_area`,
`EXT_lights_area` and `WEBGI_lights_area` use the existing supported area-light
dialects and renderer approximation. The registry describes import and runtime
feature availability; it does not promise exact reference-image equivalence
across lighting techniques.

`KHR_materials_iridescence` imports its factors and thickness textures, with a
cosine-tint approximation rather than thin-film interference. It is also
accepted with an explicit partial-support warning.

`KHR_animation_pointer` is partially supported with that same explicit warning.
The animation runtime evaluates numeric node, material, punctual-light and
camera targets described in [glTF animation](gltf-animation.md). Unknown pointer
properties are diagnosed. The implemented texture slots also support animated
offset, scale and rotation. GPU skin/morph evaluation consumes authored defaults
and sampled weight channels in both raster techniques; its rendering consumers
and remaining fallback limits are described in [glTF deformation](gltf-deformation.md).

Texture support follows compiled backends. `ENABLE_TEXTURE_KTX` enables
`KHR_texture_basisu` ETC1S/UASTC transcoding; `ENABLE_TEXTURE_WEBP` enables static
`EXT_texture_webp`. Missing required backends fail before resource reads.
Unavailable optional alternatives require a valid core fallback. AVIF remains
unsupported. The shared selector validates all authored indices and uses enabled
BasisU, then WebP, then core. Runtime preflight decodes retained source bytes
with material color/data roles and the enabled physical-device format profile
before replacing a live scene. See [image source contracts](gltf-image-sources.md)
and [typed texture decoding](texture-image-data.md) for precision, container,
mip, budget and fallback rules.

`KHR_materials_variants` supports validated named mappings, an undoable selector
and default reset, GPU classification/history updates, regenerated tangent frames
when a selected normal texture changes UV sets, and portable metadata. In-session
defaults retain the imported assignments. Saving writes the selected look as the
core primitive material, so the reloaded file's default is that saved look. See
[material variants](gltf-material-variants.md).

`KHR_mesh_quantization` is a partial capability: declared integer POSITION/UV
and supported morph layouts use checked conversion; unsupported morph semantics
are rejected explicitly. Experimental `KHR_accessor_float64` requires an
explicit required declaration, preserves source bytes and binary64 animation,
and warns about checked binary32 geometry conversion. Both use the shared
dense/sparse reader described in [glTF accessors](gltf-accessors.md).

Geometry compression is optional and disabled in the default build. Enabling
`ENABLE_GLTF_MESHOPT` supports ratified EXT buffer-view modes/filters and the
explicitly partial KHR release-candidate extension. `ENABLE_GLTF_DRACO` provides
triangle-list decoding through the bounded Windows worker, with an explicit
partial-support warning for unsupported primitive modes. The registry reflects
the configured backend; missing required decoders reject before resource reads.
See [compressed geometry](gltf-compression.md) for budgets, fallback rules,
dependency pins and portable normalization.

`MSFT_texture_dds` supports alternate DDS sources and export source remapping.
Its draft status and supported 2D DDS format/device limits are reported as partial
support. BasisU takes precedence over DDS, followed by WebP and the core source.

`KHR_materials_anisotropy` and release-candidate
`KHR_materials_diffuse_transmission` are partial capabilities, including when
required. Factors, linear direction/alpha textures, sRGB transmission color,
per-slot UV transforms, animation and portable source identities are supported.
Raster masking, environment integration, resolved MSAA materials and mixed
stochastic dielectric transport retain the explicit limits in
[extended materials](gltf-extended-materials.md).

`EXT_mesh_gpu_instancing` shares source geometry, per-owner deformation and BLAS
while publishing distinct transforms, bounds, identities and ray placements.
Its registry entry is partial because singular transforms require geometry
baking before Vulkan publication. See [mesh instancing](gltf-instancing.md).

AVIF image decoding remains unsupported. Core TRS animation
publishes runtime node transforms. Skinning and morph-target rendering use
independent owner/primitive ranges, evaluated bounds/picking and current/previous GPU
vertex streams. Missing-base normals/tangents regenerate after morphing and
before skinning through a CPU fallback published to the same current/previous
GPU streams; authored base frames retain GPU evaluation. See the deformation
document for supported UV sets and frame policy. Future decoder integration
must update the registry and tests together; a dependency being installed is
insufficient.

Errors and warnings identify the asset path, a stable diagnostic code and the
extension or version involved. Load-time warnings go to the import log;
`GltfLoadResult::capabilityDiagnostics` also retains structured warnings for
callers. Required extensions take precedence over optional declarations and
duplicate names produce one diagnostic.

Build and run `gltf_capabilities_tests` and `gltf_loader_tests` for CPU policy
and loader coverage. The capability tests exercise JSON and GLB fixtures,
source-only and geometry routes, unavailable decoders, optional fallback,
version/minimum checks, contextual diagnostics and bounded input regressions.
