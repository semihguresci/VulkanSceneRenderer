# Extended glTF materials

The renderer accepts `KHR_materials_anisotropy` and
`KHR_materials_diffuse_transmission` with **Partial** capability status.
[Anisotropy is ratified](https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/2.0/Khronos/KHR_materials_anisotropy/README.md);
[diffuse transmission is a Release Candidate](https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/2.0/Khronos/KHR_materials_diffuse_transmission/README.md).
Support covers the fields and approximations below; it does not promise future
revisions of the candidate extension.

## Inputs, defaults and texture roles

| Input | Default | Interpretation |
| --- | --- | --- |
| `anisotropyStrength` | `0` | Finite `[0,1]`, multiplied by texture B. |
| `anisotropyRotation` | `0` | Finite radians counterclockwise from tangent toward signed bitangent. |
| `anisotropyTexture` | `(1,.5,1)` | Linear RGB; RG maps to signed direction, B supplies strength. Missing texture points along +T. |
| `diffuseTransmissionFactor` | `0` | Finite `[0,1]`, multiplied by texture A. |
| `diffuseTransmissionTexture` | White | Linear alpha; RGB does not supply the standard scalar. |
| `diffuseTransmissionColorFactor` | `(1,1,1)` | Linear `[0,1]` RGB, independent of base/vertex color. |
| `diffuseTransmissionColorTexture` | White | sRGB RGB, decoded to linear before multiplication; alpha is ignored. |

All three slots support individual samplers, UV0/UV1 and
`KHR_texture_transform` offset, scale, rotation and `texCoord` override. Higher
effective UV sets receive an explicit diagnostic. Anisotropy uses the mesh's
authored/generated tangent frame, including handedness and double-sided
orientation, projected onto the final normal. Selecting another texture UV does
not create a separate derivative tangent basis.

Zero factors and black colors remain zero with textures. There is no
texture-backed promotion to one or white. Black transmission color still removes
the corresponding reflected diffuse energy. The metadata gate checks declarations,
types, indices, ranges and finite float representability before resource reads.
Both extensions exclude unlit and specular-glossiness materials in imported glTF.

## Raster lighting and environment

Forward and deferred directional, point/spot and area lighting use shared
extended terms. With an active new effect, reflected diffuse is weighted by
`(1-t)*(1-d)` and opposite-side diffuse by `(1-t)*d`, where `t` is specular
transmission and `d` is diffuse transmission. Fresnel, metalness and existing
layers apply separately. Independent transmitted color does not tint specular
reflection. Matching geometric/shading hemispheres guard backlighting, and shadow
bias points toward the emitter.

When both **sampled** new factors are zero, raster math takes the exact legacy
isotropic/layer branch, retains its established reflection weighting, and returns
the original IBL normal. Semantic visualization and unlit shading suppress these
physical effects.

The anisotropic distribution uses elliptical GGX with the existing roughness
floor, isotropic Schlick masking and denominator cap. The capped NDF is not
normalized physical GGX near the cap; correlated anisotropic Smith visibility is
not implemented. Clearcoat remains isotropic. IBL bends the normal while sampling
the existing isotropic prefiltered environment and split-sum LUT; it does not
evaluate an anisotropically prefiltered integral. Backside diffuse queries
opposite environment irradiance and separate DDGI coverage without front AO/GTAO.

## GPU storage and deferred MSAA

`GpuMaterial` is **1216 bytes** with the experimental surface tail and authored
specular/glossiness factors. Diffuse RGBA begins at byte 1184 and specular RGB
with glossiness in alpha begins at byte 1200. These appended values feed the
[reference BSDF](reference-path-tracing.md). The
anisotropy and diffuse-transmission portion contains factors at byte 896,
transmission color at 912, texture indices at
928/932/936 and three 32-byte transforms at 944/976/1008.
`anisotropyDiffuseFactors.w` is **scene-wide auxiliary availability**, identical
in every material, rather than authored glTF data. A resolved ordinary material
may share extended coverage, so its local factors cannot gate auxiliary loads.

Deferred rendering lazily allocates two `RGBA16F` auxiliary outputs per frame
slot: world tangent/strength and diffuse color/factor. Under MSAA the pass uses
the original matching multisample depth, then resolves its color attachments.
It replays all opaque owners in G-buffer order, including ordinary, diagnostic
and BIM geometry, with matching coverage/culling. Ordinary/unlit/semantic owners
write zero; discarding them could leave an earlier equal-depth extended owner.

Diffuse storage is `(color*factor,factor)`. Resolve averages this premultiplied
contribution; decoding divides RGB only for positive factor. Lighting still uses
a **resolved-material BSDF**, not per-sample transport. Mixed tangent/material
coverage remains an approximation.

## DDGI and ray limits

Probe-hit shading samples the same roles/transforms at base LOD. It uses the
oriented interpolated normal and signed tangent before normal mapping, rather
than substituting the triangle face normal.

**DDGI baseline change:** secondary-hit direct lighting previously evaluated
reflected diffuse only. It now includes full isotropic PBR specular at zero
anisotropy, then anisotropic PBR when active. This avoids a discontinuous
specular contribution and intentionally changes ordinary-material hit radiance.

Feedback reads the previous published atlas separately for front and back. The
back query uses `(-Ng,-Ns,-V)`; reflected feedback uses base color and
`(1-t)*(1-d)`, while back feedback uses independent color and `(1-t)*d`.
Material AO attenuates front feedback only. Shaded hits add no extra environment
sample; misses keep the existing environment path.

DDGI's thin-sheet and rough/volume stochastic dielectric continuation rejects active
new factors or nonfinite factor/rotation data. This conservative authored-factor
guard can reject a positive factor even when its texture samples black.
Unsupported mixtures remain blockers through existing fallback behavior. Diffuse
transmission preserves alpha coverage, picking, ray-candidate acceptance and
binary shadow policy.

## Reference path tracing

The reference tracer samples anisotropic reflection with the visible-normal GGX
distribution and evaluates diffuse transmission with a cosine-weighted BTDF.
Both use the filtered texture inputs and the same BSDF probabilities for
continuation and direct-light MIS. Active diffuse transmission in an authored
volume and anisotropic reflection on refractive volume interfaces are
diagnosed. See [reference path tracing](reference-path-tracing.md).

## Experimental retroreflection and scattering

Launch with `--experimental-gltf-materials` and select **Path tracing**. The
option selects the material field definitions used by `vk_gltf_renderer` at
`174625d5c621`. These are draft profiles. Optional declarations use the base
material when the option is off. Required declarations need explicit opt-in.
The loader checks field types, ranges, effective UVs and texture references
before resource reads. A malformed optional layer falls back to the remaining
material and reports the affected material and field. Malformed required layers
fail loading. Valid source fields and texture identities survive Import / Merge
and portable save. [Pinned renderer](https://github.com/nvpro-samples/vk_gltf_renderer/tree/174625d5c621c088bf3610377999f9dbdcf5c3b5),
[retroreflection draft](https://github.com/KhronosGroup/glTF/pull/2610),
[scattering draft](https://github.com/KhronosGroup/glTF/pull/2579).

| Extension | Evaluated fields |
| --- | --- |
| `KHR_materials_retroreflection` | `retroreflectionFactor` defaults to zero and is multiplied by the linear R texture channel. |
| `KHR_materials_scatter` | `scatterStrengthFactor` defaults to zero and is multiplied by the linear A texture channel. `multiscatterColorFactor` defaults to white and multiplies the sRGB-decoded RGB texture. `scatterAnisotropy` defaults to zero and lies strictly between −1 and 1. |
| `KHR_materials_volume_scatter` | Legacy alias. Strength defaults to one and color to black. `multiscatterColor` is accepted. The current extension takes precedence when both layers are valid. |

The three texture slots support individual samplers, UV0/UV1 and texture
transforms. The path tracer mixes the ordinary front-side BRDF with its
mirrored-view evaluation for retroreflection. Sampling follows both proposals,
including smooth delta reflection. The original transmission evaluation stays
separate and direct-light MIS uses the complete mixture PDF.

On a thin dielectric, scattering redistributes part of coherent transmission
to two Lambertian lobes. The forward share is `(1 + g) / 2`. The reflected
share receives the rest. The original specular-transmission tint and other
diffuse contributions stay independent. Zero effective strength preserves the
underlying material. The reference path tracer retains ordinary refraction and
absorption when active draft scattering or retroreflection reaches a closed
volume. It reports the skipped layer in the renderer log and raw EXR metadata.
Raster and DDGI use the existing base-material response. The tracer's other
layer limits still apply. [Participating-media transport (#47)](https://github.com/semihguresci/VulkanSceneRenderer/issues/47)

The [Windows validation report](../tests/visual-regression/gltf-experimental-materials/windows-nvidia/gltf-experimental-materials-results.json)
records the fallback checks and fixed-lighting comparisons against NVIDIA's
[v2026.11 release](https://github.com/nvpro-samples/vk_gltf_renderer/releases/tag/v2026.11).

## Export, animation and validation

Portable glTF/GLB export retains extension metadata, authored default presence,
samplers and slot-specific source identities, including distinct source textures
sharing one deduplicated GPU image. Imported provenance has 23 slots. Custom
scalar channels on packed RGB slots, non-alpha diffuse-factor channels and invalid
UV/numeric data fail export instead of changing the material. Existing atomic
writing protects live state and previous output.

`KHR_animation_pointer` supports the new scalar factors/rotation, RGB color and
individual components, plus offset/scale/rotation of all three texture transforms.
It does not add animated texture indices, samplers or UV selectors. Validation
and application remain atomic. Authored rest values and cross-clip restore
channels are preserved; saved defaults retain the current appearance.

`gltf_extended_materials_tests` checks import and material layout. The Vulkan
contracts `extended_pbr_gpu_tests`, `extended_pbr_resolve_gpu_tests`, and
`ddgi_extended_material_gpu_tests` exercise shader math, four-sample coverage,
and probe-hit material evaluation. Set `CONTAINER_RUN_GPU_EXTENDED_PBR=1` and
`CONTAINER_RUN_GPU_DDGI=1` to run the contracts on the GPU with Vulkan validation.

The [raster regression](../tests/validation/gltf_extended_materials_runtime_regression.py)
checks both techniques, including zero factors, rotated tangents, texture
channels and UV transforms, sidedness, mixed-material coverage, lighting,
transparency, morphing and portable export. The `shadows` cohort also supports
`--hard-ray` for the backside-occluder controls.

The [DDGI regression](../tests/validation/gltf_extended_materials_ddgi_runtime_regression.py)
checks textured and scalar equivalents, a rotated tangent control, and a
visible indirect response against zero GI intensity. Each technique uses an
explicit 3×3×3 probe volume and requires timed GPU passes and committed probe
history.

Run each script with `--self-check` to check its fixtures and assertions
offline. Native runs take `--renderer`, `--output`, and `--manifest`. Use a new
output directory and a JSON `files` object mapping absolute paths to SHA-256
digests for the renderer and its runtime inputs. Include the shader modules
used by the requested cohort and keep the executable, shaders, DLLs, and assets
fixed for the entire run. The scripts verify the manifest and write their
capture results to `results.json`.
