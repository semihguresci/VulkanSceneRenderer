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

`GpuMaterial` is **1040 bytes**; existing offsets remain unchanged. Its appended
tail contains factors at byte 896, transmission color at 912, texture indices at
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

Thin-sheet and rough/volume stochastic dielectric continuation rejects active
new factors or nonfinite factor/rotation data. This conservative authored-factor
guard can reject a positive factor even when its texture samples black.
Unsupported mixtures remain blockers through existing fallback behavior. This
slice adds no anisotropic dielectric sampling, diffuse continuation PDFs, MIS,
caustics or full path tracer. Diffuse transmission changes no alpha/coverage,
picking, ray-candidate acceptance or binary shadow-skip policy.

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

The [depth-store checkpoint](../out/feature-parity/pbr92-depth-store-validation-checkpoint.json)
records **1056 cases in 49 executables**, zero failures/skips, nine real GPU
suites, 24 Python references, 83 math GPU points and 11 actual 4x resolve cases.
Two additional GPU tests cover seven depth/stencil store combinations, including
preserved-value readback. Dynamic rendering accounts for implicit STORE and
DONT_CARE writes when ending a read-only depth/stencil scope.
Its [100-input frozen manifest](../out/feature-parity/pbr92-depth-store-native-manifest.json)
identifies renderer SHA256
`5b3f617a1bf9ddbc2a295cb09e88d2102a2a86e54b05a46fa0a091bde5787b43`.
Required-extension JSON/GLB acceptance passes. The
[completion checkpoint](../out/feature-parity/pbr92-completion-validation-checkpoint.json)
records **132 native captures, 92 image checks, 16 execution checks and four
export inspections**, all passing on that same binary:

- Both raster techniques cover strict zero/black factors, independent rotated
  tangents, texture channels/color spaces, UV transforms, mirrored frames,
  deterministic mixed-material 1x/4x coverage, area/environment lighting,
  transparency, GPU morphing and animated GLB/ASCII save/reload.
- Of 52 equivalent-image comparisons, 47 are pixel exact; the remaining five
  differ by at most one byte. All 24 main negative controls are nonvacuous.
  Backside occluders block both finite-range raster and hard-ray lights.
- Sixteen DDGI captures across both techniques publish three ray instances,
  27 probe updates and 216 rays per update with timed trace/update work and
  committed history. Fourteen use intensity 4; two use intensity 0 as controls.
  GI changes every pixel in the receiver ROI, with mean differences above
  125/255. The independently authored 3x3x3 volume is not an automatic scene fit.

All 132 distinct logs pass the strict scanner and all 100 frozen inputs match
before/after. Native capture does not read back probe atlases, material buffers
or GPU picking. Its material deformation fixture covers morphing; skinning has
separate existing deformation evidence.

Earlier capture attempts are diagnostic runs with report/oracle problems and a
depth-store synchronization error. The stricter
[log audit](../out/feature-parity/pbr92-strict-log-audit.json) supersedes their
earlier validation-clean fields. The capture harness now rejects newer
validation-layer hazard wording as well as VUID/SYNC messages.

Changes remain local and uncommitted. Subsequent merge/import foundations are
uncompiled and are not part of this frozen binary's evidence.
