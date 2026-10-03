# Renderer PBR Implementation

This document records the renderer's glTF PBR material pipeline, the supported
features, and the implementation rules for extending it. It is the current
source of truth for material import, GPU layout, shader evaluation, known
limitations, and validation.

## Current Status

The renderer supports the requested PBR stack:

- Metallic-roughness, normal, emissive, and occlusion maps.
- `KHR_materials_specular`.
- `KHR_materials_clearcoat`.
- `KHR_materials_sheen`.
- `KHR_materials_transmission` plus `KHR_materials_volume`.
- `KHR_materials_iridescence`.
- `KHR_materials_dispersion`.
- Multiple-scattering compensation for specular IBL.

The renderer is deferred-first. Opaque and alpha-mask materials write a compact
G-buffer; transparent and transmission materials use the forward/OIT path where
the full object material data is available. This is the most important design
constraint when adding new material features.

## Plan Review

The proposed plan is directionally correct:

- Use an uber-shader style material path instead of one shader per glTF
  extension.
- Keep textures bindless so glTF materials can reference different textures
  without rebinding descriptors per draw.
- Split reusable Slang code into material evaluation, BRDF, normal mapping,
  IBL, and shared data-layout includes.
- Keep opaque, masked, and blended materials as pipeline buckets rather than
  creating a pipeline for each material feature.
- Treat glTF PBR as a layered material model: base metallic-roughness plus
  optional specular, clearcoat, sheen, transmission, volume, iridescence, IOR,
  emissive strength, and unlit behavior.

The parts that need adjustment for this renderer:

- The renderer is currently deferred-first, not Forward+. Opaque and alpha-mask
  objects write a G-buffer, then lighting is applied in deferred passes.
  Transparent materials use a forward path with OIT.
- Material data is stored in a `GpuMaterial` SSBO. Per-object `ObjectData`
  carries transform data, normal matrix, bounds, and an `objectInfo` vector.
- Descriptor sets already group scene data and bindless textures together in
  the scene descriptor set. Do not redesign descriptor ownership without also
  updating `SceneManager`, `FrameResourceManager`, and pipeline layouts.
- Transmission must not be named or treated as generic refraction. glTF
  `KHR_materials_transmission` is a thin-surface transmission factor. True
  volume behavior comes from `KHR_materials_volume`, where thickness and
  attenuation are applied.
- Opaque deferred shading stores compact per-pixel channels plus material
  metadata. Directional, point, and tiled lighting decode the material index and
  fetch `GpuMaterial` for factor-level layered BRDF terms that do not fit in the
  G-buffer.

## Current Renderer Shape

Frame order:

```text
Depth prepass
  -> Hi-Z / occlusion cull
  -> G-buffer
  -> shadow cascades
  -> tile light cull
  -> GTAO
  -> deferred lighting
  -> transparent OIT
  -> exposure histogram/adaptation
  -> bloom
  -> post-process and debug UI
```

Important files:

- CPU material model: `include/Container/utility/Material.h`
- GPU object and material upload layouts: `include/Container/utility/SceneData.h`
- glTF and MaterialX import bridge: `src/utility/MaterialXIntegration.cpp`
- Material resolution and texture descriptors: `src/utility/SceneManager.cpp`
- Object data upload: `src/renderer/scene/SceneController.cpp`
- Shared shader object layout: `shaders/object_data_common.slang`
- Shared shader material layout: `shaders/material_data_common.slang`
- Shared PBR material helpers: `shaders/pbr_material_common.slang`
- Shared BRDF helpers: `shaders/brdf_common.slang`
- Opaque material evaluation: `shaders/gbuffer.slang`
- Transparent material evaluation: `shaders/forward_transparent.slang`

## Implementation Ownership

| Responsibility | File |
|---|---|
| CPU material factors and texture indices | `include/Container/utility/Material.h` |
| Host-to-GPU object/material layouts and material flags | `include/Container/utility/SceneData.h` |
| glTF extension parsing and texture color-space classification | `src/utility/MaterialXIntegration.cpp` |
| Material resolution into `GpuMaterial` data and object material indices | `src/utility/SceneManager.cpp` and `src/renderer/scene/SceneController.cpp` |
| Shader-side `ObjectData` and `GpuMaterial` mirrors | `shaders/object_data_common.slang` and `shaders/material_data_common.slang` |
| Texture channel sampling and material parameter helpers | `shaders/pbr_material_common.slang` |
| GGX BRDF, Fresnel, F0, IBL compensation | `shaders/brdf_common.slang` |
| Opaque and alpha-mask material packing | `shaders/gbuffer.slang` |
| Deferred directional and IBL lighting | `shaders/deferred_directional.slang` |
| Deferred point-light accumulation | `shaders/point_light.slang` and `shaders/tiled_lighting.slang` |
| Transparent layered material evaluation | `shaders/forward_transparent.slang` |

When adding a material field, update the CPU `Material`, GPU `GpuMaterial`,
shader `GpuMaterial`, static layout assertions, material upload code, importer,
and at least one shader path in the same change. `ObjectData` only needs to
change when object-level draw data changes.

## Data Model

The current material data path is:

```text
glTF material
  -> container::material::Material
  -> container::gpu::GpuMaterial
  -> shaders/material_data_common.slang::GpuMaterial

Scene object/draw
  -> container::gpu::ObjectData.objectInfo.x
  -> shaders/object_data_common.slang::ObjectBuffer.objectInfo.x
  -> uMaterials[objectInfo.x]
```

`ObjectData` carries transform data, the normal matrix, bounding sphere, and a
packed object metadata vector. `objectInfo.x` is the material index; `yzw` are
reserved for future per-object draw metadata. Full material factors, texture
indices, and feature flags live in `GpuMaterial`. This keeps draw submission
object-index based while allowing many objects to share the same uploaded
material record.

Use `glm::uvec4 objectInfo` on the C++ side and `uint4 objectInfo` on the Slang
side for this metadata block. Keeping the metadata as a 16-byte vector makes
the host and shader layouts naturally agree under structured-buffer layout
rules and leaves explicit room for future fields. Do not use `uint3` as padding:
Slang/SPIR-V can give `uint3` 16-byte alignment, which moves the following
`float4` and changes the array stride.

Current feature data includes:

- Base color, opacity, alpha cutoff, double-sided state.
- Metallic, roughness, normal scale, occlusion strength.
- Emissive color and `KHR_materials_emissive_strength`.
- `KHR_texture_transform` offset/rotation/scale and `texCoord` selection for
  core base color, normal, metallic-roughness, occlusion, and emissive slots.
- Separate roughness, metalness, specular intensity, specular color, height,
  opacity, transmission, clearcoat, sheen, volume, and iridescence textures.
- IOR, dispersion, attenuation color, attenuation distance, thickness, and
  iridescence thickness range.
- Workflow flags for alpha mask, alpha blend, double-sided, specular-glossiness,
  and unlit.

The material SSBO reduces object-buffer bandwidth when many objects share a
material and gives draw shaders a single full-material fetch path. Deferred
lighting still depends on what the G-buffer stores unless a later pass also
receives a material or object index.

## Descriptor Strategy

The scene descriptor set should keep one stable bindless material model:

- Camera uniform buffer.
- Object SSBO with transform data, normal matrix, `objectInfo`, and bounds.
- Material SSBO with `GpuMaterial` factors, texture indices, core texture
  transforms, and feature flags.
- Shared material sampler array for glTF wrap-mode combinations.
- Bindless sampled image array for material textures.

The material SSBO stores texture references as bindless sampled-image indices;
it does not store texels. Actual image sampling stays in the existing
`Texture2D materialTextures[]` array. Texture indices use `0xffffffff` as
invalid, so shaders can infer most feature presence from a valid texture index
or non-default factor. A separate feature bit is still useful when
interpretation changes, such as specular-glossiness where alpha is glossiness
rather than roughness.

Core glTF texture metadata is stored per material. `GpuMaterial` carries a
precomputed affine transform for base color, normal, metallic-roughness,
occlusion, and emissive textures. The importer preserves glTF defaults
(`texCoord = 0`, offset `0,0`, rotation `0`, scale `1,1`) and applies
`KHR_texture_transform` in glTF order: scale, then rotation, then offset. When
the extension supplies `texCoord`, that value overrides the base texture-info
coordinate set. Runtime sampling supports UV0 and UV1; higher authored
coordinate sets currently fall back to UV1 in shaders until the vertex format
is extended further.

Current descriptor layout:

```text
set 0, binding 0: CameraData UBO
set 0, binding 1: ObjectData SSBO
set 0, binding 2: SamplerState materialSamplers[]
set 0, binding 3: GpuMaterial SSBO
set 0, binding 4: GpuTextureMetadata SSBO
set 0, binding 5: Texture2D materialTextures[]
```

`ObjectData.objectInfo.x` is the only material pointer needed by draw shaders.
G-buffer, depth, shadow, debug, and transparent passes fetch `GpuMaterial` with
that index when they need material factors, texture indices, or feature flags.
Cull shaders should continue to read only object transform/bounds data unless
they explicitly need material-driven displacement or visibility behavior.
`GpuTextureMetadata.samplerIndex` selects the sampler for each texture object,
so glTF `wrapS` and `wrapT` settings survive even when several texture objects
reference the same image with different sampler state. The bindless
sampled-image array remains the final binding because it uses Vulkan variable
descriptor count semantics.

If texture metadata becomes necessary later, add a separate compact texture
metadata SSBO rather than replacing the bindless sampled-image array. That
metadata SSBO can describe sampler policy, color-space flags, or source asset
properties, while `GpuMaterial` continues to reference textures by bindless
image index.

The include-only Slang files for both object and material layouts are shader
build dependencies, so editing shared ABI files triggers shader rebuilds.

## Shader Organization

The current organization keeps shared data layouts and material helpers in
include-only Slang files:

```text
shaders/
  object_data_common.slang
  material_data_common.slang
  lighting_structs.slang
  brdf_common.slang
  area_light_common.slang
  pbr_material_common.slang
  surface_normal_common.slang
  alpha_mask_common.slang
  gbuffer.slang
  forward_transparent.slang
  deferred_directional.slang
  point_light.slang
  tiled_lighting.slang
```

`object_data_common.slang` is the single source for shader-side `ObjectBuffer`,
and `material_data_common.slang` is the single source for shader-side
`GpuMaterial`. Any change to `container::gpu::ObjectData` or
`container::gpu::GpuMaterial` must be reflected there and in the static layout
asserts in `SceneData.h`.

`pbr_material_common.slang` owns texture sampling and material parameter
evaluation. Pass shaders should assemble the inputs and call helpers rather
than reimplementing glTF texture channel rules.

`brdf_common.slang` owns BRDF math, Fresnel, GGX distribution, geometry terms,
F0 from IOR/specular, and world-position reconstruction.
`area_light_common.slang` owns the shared rectangle/disk area-light frame and
four-sample emitter integration used by opaque deferred and forward transparent
lighting.

## PBR Evaluation

Base material evaluation should follow glTF channel conventions:

- Base color texture is sRGB. Alpha contributes to material alpha.
- Metallic-roughness texture is data/UNORM. Roughness is G, metallic is B.
- Core base color, metallic-roughness, normal, occlusion, and emissive samples
  use their imported texture transform and selected UV set in G-buffer and
  forward transparent shading.
- Normal, occlusion, clearcoat, thickness, sheen roughness, iridescence, and
  transmission textures are data/UNORM.
- Emissive, specular color, sheen color, and diffuse/base-color textures are
  color/sRGB.
- Occlusion uses R and blends with `occlusionStrength`.
- Clearcoat roughness uses G.
- Sheen roughness uses A.
- Volume thickness uses G.
- Iridescence thickness uses G.

Direct lighting uses Cook-Torrance GGX:

- GGX/Trowbridge-Reitz normal distribution.
- Smith masking-shadowing.
- Schlick Fresnel.
- Metallic workflow diffuse suppression.
- F0 derived from IOR and `KHR_materials_specular` for dielectrics, then mixed
  with base color for metals.
- Rectangular and disk area lights integrate 25 samples across the emitter.
  Rectangles use tensor-product Gauss-Legendre quadrature; disks use
  Gauss-Legendre quadrature in squared radius with five angles per ring.
- Area-light shadows render complete perspective cubes from one to four
  emitter positions. Each emitter sample consumes six local-shadow layers;
  the shared layer budget determines the available sample count. Point/spot
  allocations consume their existing share first, then remaining layers are
  distributed across eligible area lights. Lights without six remaining
  layers receive no shadow map.
- Point and area shadow filters remap taps across cube faces and compare depth
  in the destination face. Deferred shadow bias reconstructs geometric normals
  with explicit depth neighbors, avoiding derivatives after divergent returns.
- Forward opaque and transparent lighting evaluate the uploaded point/spot
  light count, bounded by the 8,192-light storage capacity.

IBL remains required:

- Irradiance cubemap for diffuse.
- Prefiltered environment cubemap for specular.
- BRDF LUT for split-sum specular.
- Multiple-scattering compensation for rough specular IBL. The renderer uses
  the BRDF LUT to estimate missing GGX single-scatter energy and feeds that
  energy back into the specular environment term.
- GTAO/occlusion applied to indirect diffuse and specular occlusion.

## Supported PBR Features

Current renderer support for the requested PBR stack:

| Feature | Current support |
|---|---|
| Metallic-roughness | Core glTF factors and packed G/B metallic-roughness texture in deferred and forward paths |
| Normal | Tangent-space normal map with scale, double-sided face correction, and clearcoat normal in forward transparent path |
| Emissive | Emissive texture/factor plus `KHR_materials_emissive_strength` |
| Occlusion | R-channel occlusion texture with `occlusionStrength`, combined with GTAO in deferred lighting |
| Specular | `KHR_materials_specular` intensity/color import; forward keeps color F0, deferred stores per-pixel dielectric F0 color in a dedicated specular G-buffer attachment |
| Clearcoat | Factor, roughness, textures, and clearcoat normal; compact opaque approximation, deferred direct-light factor-level clearcoat lobe, and forward second specular lobe |
| Sheen | Sheen color/roughness factors and textures with grazing-angle cloth approximation in forward and deferred direct lighting |
| Transmission + volume | Thin-surface transmission, thickness texture/factor, attenuation color/distance, and Beer-Lambert absorption in forward transparent path |
| Iridescence | Factor, IOR, thickness range/texture with angle/thickness tint approximation |
| Dispersion | Optional per-channel IOR spread for transmitted forward transparent sampling |
| Multiple scattering compensation | GGX specular IBL compensation in deferred and forward paths, including clearcoat IBL |

## Feature Details

Metallic-roughness:

- Factors come from core glTF `pbrMetallicRoughness`.
- The packed texture follows glTF channels: G is roughness, B is metallic.
- Separate legacy roughness and metalness textures are still accepted for
  compatibility.

Normal:

- Normal maps are sampled as data textures.
- `normalTexture.scale` is applied in shader.
- Double-sided materials flip tangent-space evaluation on back faces.
- Clearcoat can use its own normal map in the forward transparent path.

Emissive:

- Emissive factor and texture are multiplied together.
- `KHR_materials_emissive_strength` allows values beyond `[0, 1]`.
- Unlit materials route base color through the emissive/no-light path.

Occlusion:

- Occlusion uses the R channel and blends with `occlusionStrength`.
- Deferred lighting combines material AO with GTAO.

Specular:

- `KHR_materials_specular` intensity and color are imported.
- Forward shading keeps colored F0.
- Deferred shading writes per-pixel dielectric F0 color into the specular
  G-buffer target. Directional, point, and tiled deferred lighting read that
  attachment, so texture-sourced `specularColorTexture` variation survives the
  deferred path instead of collapsing to a scalar magnitude.

Clearcoat:

- Factor, roughness, factor textures, roughness texture, and clearcoat normal
  texture are imported.
- Forward transparent shading evaluates a second specular lobe.
- Opaque G-buffer shading bakes roughness/F0 channels for every deferred light.
  Directional, point, and tiled deferred lighting also fetch `GpuMaterial` and
  evaluate a factor-level clearcoat direct lobe using the material clearcoat
  factor and roughness.

Sheen:

- Sheen color and roughness factors/textures are imported.
- The current shader uses a grazing-angle cloth reflection approximation.
- Opaque G-buffer shading keeps compact emissive/F0 channels. Directional,
  point, and tiled deferred lighting add a factor-level sheen direct
  contribution from `GpuMaterial`.

Transmission and volume:

- Transmission is treated as glTF thin-surface transmission, not generic
  refraction.
- Volume adds thickness and Beer-Lambert attenuation.
- The high-quality path is currently forward transparent/OIT.

Iridescence:

- Factor, IOR, thickness range, and thickness texture are imported.
- The shader uses an angle/thickness tint approximation.
- Deferred direct lighting applies the factor-level iridescence tint to
  dielectric F0 using the decoded material index. The compact G-buffer fallback
  still stores only the scalar F0 and pre-tinted base color.

Dispersion:

- Dispersion is optional and only affects transmitted forward transparent
  sampling.
- It offsets per-channel IOR and samples the environment with separated RGB
  directions.

Multiple scattering compensation:

- Specular IBL uses `EvaluateSpecularIblMultipleScattering()` in
  `brdf_common.slang`.
- Deferred lighting, forward transparent lighting, and clearcoat IBL all use
  the compensated split-sum result.
- Direct lights still use the standard single-scattering Cook-Torrance BRDF.

## glTF Extension Semantics

Supported importer target:

| Extension | Import data | Shader behavior |
|---|---|---|
| Core metallic-roughness | base color, metallic, roughness, normal, AO, emissive, alpha | Deferred and forward |
| `KHR_texture_transform` | offset, rotation, scale, alternate `texCoord` | Supported for core glTF texture slots in G-buffer and forward transparent paths |
| `KHR_materials_pbrSpecularGlossiness` | diffuse, specular, glossiness, textures | Converted to metallic-roughness approximation; glossiness alpha converted to roughness |
| `KHR_materials_specular` | specular intensity and specular color | Modifies dielectric F0 |
| `KHR_materials_clearcoat` | clearcoat, roughness, normal | Adds second specular lobe in forward; compact approximation in G-buffer |
| `KHR_materials_transmission` | transmission factor/texture | Thin-surface transmission term |
| `KHR_materials_volume` | thickness, attenuation color/distance | Beer-Lambert absorption for transmitted color |
| `KHR_materials_sheen` | sheen color and roughness | Grazing-angle cloth reflection approximation |
| `KHR_materials_iridescence` | factor, IOR, thickness range/texture | Angle/thickness color-shift approximation |
| `KHR_materials_emissive_strength` | emissive multiplier | Multiplies emissive output beyond `[0, 1]` |
| `KHR_materials_unlit` | unlit flag | Emits base color through emissive/no-light path |
| `KHR_materials_ior` | IOR | Controls dielectric F0 and transmission eta |
| `KHR_materials_dispersion` | dispersion | Offsets per-channel eta for transmitted color |

Do not conflate these:

- Transmission is thin-surface light passage.
- Volume adds interior absorption.
- IOR changes Fresnel and volume refraction direction.
- Dispersion is wavelength-dependent refraction and should remain optional.

## Deferred Versus Forward

Opaque deferred path:

- `gbuffer.slang` evaluates base material inputs and writes compact material
  channels.
- Deferred lighting sees albedo, alpha, normal, metallic, roughness, AO,
  emissive, and scalar dielectric F0. The F0 channel is evaluated from IOR,
  `KHR_materials_specular` intensity, and the magnitude of specular color before
  packing, so deferred lights are not locked to the default 0.04 dielectric F0.
- The material G-buffer metadata preserves the resolved material index.
  Directional, point, and tiled deferred lighting fetch `GpuMaterial` through
  the scene material SSBO when they need full material flags or factors that do
  not fit in the compact channels.
- All deferred direct-light passes use shared deferred BRDF helpers to recover
  factor-level colored dielectric specular, clearcoat direct lighting, sheen
  direct lighting, and iridescence F0 tint from that material record.

Transparent forward path:

- `forward_transparent.slang` has access to full object/material data.
- It can evaluate clearcoat, sheen, iridescence, transmission, volume, IOR, and
  dispersion more directly.
- OIT stores the final transparent color and alpha.

Implemented material-index bridge:

- `gbuffer.slang` packs the material index into the material target metadata.
- `deferred_directional.slang`, `point_light.slang`, and
  `tiled_lighting.slang` decode that index and fetch `GpuMaterial` from the
  scene descriptor set.
- This keeps compact G-buffer channels for per-pixel data while letting all
  deferred direct-light passes share factor-level layered material terms.

## Pipeline Strategy

Current buckets should stay:

- Depth prepass.
- Shadow depth.
- Opaque G-buffer.
- Alpha-mask G-buffer.
- Transparent forward/OIT.
- Debug/normal/wireframe passes.

Do not create a pipeline per extension. Use object/material flags and texture
indices inside the shader. Later, when profiling shows branch cost matters, add
coarse permutations:

- Base opaque PBR.
- Normal-mapped opaque PBR.
- Layered opaque PBR.
- Transparent/transmission PBR.
- Unlit.

Specialization constants are useful for coarse variants, but they are not a
replacement for per-material data in glTF scenes.

## Import Rules

The importer should:

- Give `KHR_materials_pbrSpecularGlossiness` precedence over core
  metallic-roughness when present.
- Preserve alpha mode: OPAQUE, MASK, BLEND.
- Promote opaque materials to blend when opacity or transmission requires it.
- Classify texture color spaces by usage before upload.
- Store extension factors even if no texture exists.
- Resolve extension textures directly from glTF texture indices when possible.
- Keep legacy `refraction` names as import compatibility, but store them in
  transmission fields internally.

## Debug Views

Recommended debug output modes:

- Base color.
- Normal.
- Metallic.
- Roughness.
- AO.
- Emissive.
- Specular/F0.
- Clearcoat.
- Sheen.
- Transmission.
- Thickness/volume absorption.
- IOR.
- Alpha.
- Diffuse only.
- Specular only.
- IBL only.

These views are more valuable than trying to visually inspect every extension
through final lit output.

## Implementation Roadmap

Phase 1 - Stabilize current PBR:

- Keep shared `ObjectBuffer` in `object_data_common.slang`.
- Keep shared `GpuMaterial` in `material_data_common.slang`.
- Keep shared material helpers in `pbr_material_common.slang`.
- Keep transmission terminology in CPU and shader fields.
- Build sample-model tests for imported extension factors and texture indices.

Phase 2 - Improve correctness:

- MikkTSpace tangent generation is used for glTF assets without trustworthy
  tangents, with the deterministic geometry path kept as a degenerate fallback.
- `KHR_texture_transform` and alternate texture coordinate handling are applied
  to non-core extension textures, including specular, transmission, clearcoat,
  sheen, volume, and iridescence texture slots.
- Colored specular propagation for texture-sourced F0 uses the dedicated
  deferred specular/F0 G-buffer attachment.
- Add reference scene screenshots for Khronos sample models.

Phase 3 - Material SSBO (complete):

- Material data is split out of `ObjectData` into a `GpuMaterial` SSBO.
- `ObjectData.objectInfo.x` stores the material index per object/draw.
- Draw shaders fetch `GpuMaterial` through `uMaterials[objectInfo.x]`.
- Feature flags live in the material record. Deferred directional, point, and
  tiled light passes now decode the G-buffer material index and fetch the same
  material record.

Phase 4 - Better glass:

- Add screen-space transmission/background sampling for thin glass.
- Use volume thickness from mesh scale or thickness texture consistently.
- Make dispersion optional and low-cost by default.

Phase 5 - Optimization:

- Profile branch divergence in the uber-shader.
- Add coarse shader permutations only where profiling proves value.
- Consider compact 16-bit or packed material fields after correctness is stable.

## Validation Checklist

Build and tests:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug"
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && ctest --test-dir out\build\windows-debug --output-on-failure"
```

Visual sample set:

- `DamagedHelmet` for base metallic-roughness.
- `SpecularTest` for `KHR_materials_specular`.
- `ClearCoatTest` and `ClearcoatWicker` for clearcoat.
- `SheenCloth` and `GlamVelvetSofa` for sheen.
- `TransmissionTest` and `TransmissionRoughnessTest` for transmission.
- `DragonAttenuation` or `MosquitoInAmber` for volume attenuation.
- `IridescenceDielectricSpheres` for iridescence.
- `EmissiveStrengthTest` for emissive strength.
- `UnlitTest` for unlit.
- `SpecGlossVsMetalRough` for specular-glossiness conversion.

Realistic-rendering regression metadata:

- CPU fixture validation lives in `tests/realistic_rendering_validation_tests.cpp`.
- Fixture schema and planned scene metadata live under
  `tests/fixtures/rendering/`.
- Future GPU baselines should be written as
  `tests/visual-regression/golden/{platform}/{scene-id}.png`; candidates and
  diffs should stay under `test_results/visual-regression/`.
- Material screenshot scenes should reuse the fixture material names where
  possible so PBR changes can be reviewed against the same calibrated gray,
  chrome, rough metal, clearcoat, and thin-transmission references.
