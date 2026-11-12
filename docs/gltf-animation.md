# glTF animation runtime

The primary glTF scene has clip selection, play/pause, looping, playback speed
(including reverse playback), and a timeline in **Scene Controls → glTF
Animation**. Selecting **Rest pose** restores authored node arrays and the
original material/light/camera objects targeted by any clip. Selecting another
clip restores that same source pose before applying its first sample. Clip
timelines start at zero; channels whose first key is later hold their first
value until that key.

For deterministic captures, select a zero-based clip and time explicitly:

```powershell
.\VulkanSceneRenderer.exe --model scene.gltf --animation-clip 0 --animation-time 0.5 --animation-paused --hidden --no-ui --screenshot frame.png
```

`--animation-speed` sets a finite playback multiplier. Without
`--animation-paused`, the selected clip advances using the application's normal
or fixed capture delta. A clip is selected only when requested by the UI or
`--animation-clip`; loading a scene preserves its initial static presentation.
`--gltf-camera-node N` selects the startup scene's authored camera attached to
source node `N`, independently of clip selection. A model replacement starts
in the viewport camera and the new model's rest pose. For a scripted capture,
use camera and animation selection events to select bindings on the replacement.

## Sampling and source data

`GltfAnimation` reads real animation accessors into immutable clip tracks.
Sampling supports `STEP`, `LINEAR`, and `CUBICSPLINE`, node translation/rotation/
scale, and packed morph weights. Linear rotations use normalized shortest-arc
slerp. Cubic quaternion interpolation preserves the authored components and
tangents, scales derivatives by the adjacent key interval, and normalizes only
the sampled quaternion. Cubic weights use complete in-tangent/value/out-tangent
arrays for each key. Sampling never rewrites key values or flips authored
quaternion signs.

Accessors use the shared checked reader, supporting dense buffers, valid
strides, sparse zero bases and sparse overrides, and the normalized integer
component types permitted for rotation and weights. Pointers also accept
numeric integer output accessors, converted before interpolation. Explicitly
declared experimental `KHR_accessor_float64` retains binary64 time, values,
cubic derivatives and interpolation. Unsafe sampled properties are rejected
before publication to float-backed runtime fields. Validation checks indices, component/type/count
agreement, byte ranges/alignment, arithmetic overflow, finite values, strictly
increasing nonnegative times, and input min/max at FLOAT precision. Sparse
indices must increase strictly and remain inside the accessor count. Decoded
accessors are bounded to 16,777,216 scalar components, with an aggregate 128 MiB
limit for decoded track arrays across the library, to bound sparse expansion.
Finite loader budgets also account for those arrays. See
[accessor import](gltf-accessors.md) for shared layout, quantization and binary64
rules. Invalid supported channels report clip/channel context. Unsupported
pointer properties produce visible diagnostics and are omitted.

Duplicate targets are rejected within a clip, including a whole vector and one
of its elements, repeated elements, and a core node target overlapping a pointer
to the same property. Disjoint vector components may use separate channels.

## Pointer coverage

The runtime implements a typed subset of
[`KHR_animation_pointer`](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_animation_pointer/README.md),
using node, material, light, and camera source indices. The supported paths
are listed below.

| Source prefix | Mutable property suffixes |
| --- | --- |
| `/nodes/{i}/` | `translation`, `rotation`, `scale`, `weights` |
| `/nodes/{i}/extensions/` | `KHR_node_visibility/visible`, `KHR_node_selectability/selectable`, `KHR_node_hoverability/hoverable` |
| `/materials/{i}/` | `pbrMetallicRoughness/baseColorFactor`, `pbrMetallicRoughness/metallicFactor`, `pbrMetallicRoughness/roughnessFactor`, `emissiveFactor`, `alphaCutoff`, `normalTexture/scale`, `occlusionTexture/strength` |
| `/materials/{i}/extensions/KHR_materials_emissive_strength/` | `emissiveStrength` |
| `/materials/{i}/extensions/KHR_materials_ior/` | `ior` |
| `/materials/{i}/extensions/KHR_materials_dispersion/` | `dispersion` |
| `/materials/{i}/extensions/KHR_materials_transmission/` | `transmissionFactor` |
| `/materials/{i}/extensions/KHR_materials_volume/` | `thicknessFactor`, `attenuationDistance`, `attenuationColor` |
| `/materials/{i}/extensions/KHR_materials_clearcoat/` | `clearcoatFactor`, `clearcoatRoughnessFactor`, `clearcoatNormalTexture/scale` |
| `/materials/{i}/extensions/KHR_materials_specular/` | `specularFactor`, `specularColorFactor` |
| `/materials/{i}/extensions/KHR_materials_sheen/` | `sheenColorFactor`, `sheenRoughnessFactor` |
| `/materials/{i}/extensions/KHR_materials_iridescence/` | `iridescenceFactor`, `iridescenceIor`, `iridescenceThicknessMinimum`, `iridescenceThicknessMaximum` |
| `/materials/{i}/extensions/KHR_materials_pbrSpecularGlossiness/` | `diffuseFactor`, `specularFactor`, `glossinessFactor` |
| `/extensions/KHR_lights_punctual/lights/{i}/` | `color`, `intensity`, `range`, `spot/innerConeAngle`, `spot/outerConeAngle` |
| `/cameras/{i}/perspective/` | `yfov`, `aspectRatio`, `znear`, `zfar` |
| `/cameras/{i}/orthographic/` | `xmag`, `ymag`, `znear`, `zfar` |

Texture infos below also accept `/extensions/KHR_texture_transform/offset`,
`/extensions/KHR_texture_transform/scale` (both VEC2), and
`/extensions/KHR_texture_transform/rotation` (scalar radians), appended to the
listed material-relative path:

| Material-relative texture-info path | Runtime texture slot(s) |
| --- | --- |
| `pbrMetallicRoughness/baseColorTexture` | Base color |
| `pbrMetallicRoughness/metallicRoughnessTexture` | Packed metallic/roughness and its roughness/metalness slots |
| `normalTexture`, `occlusionTexture`, `emissiveTexture` | Normal, occlusion, emissive respectively |
| `extensions/KHR_materials_specular/specularTexture`, `specularColorTexture` | Specular amount, specular color |
| `extensions/KHR_materials_transmission/transmissionTexture` | Transmission |
| `extensions/KHR_materials_clearcoat/clearcoatTexture`, `clearcoatRoughnessTexture`, `clearcoatNormalTexture` | Clearcoat, clearcoat roughness, clearcoat normal |
| `extensions/KHR_materials_volume/thicknessTexture` | Thickness |
| `extensions/KHR_materials_sheen/sheenColorTexture`, `sheenRoughnessTexture` | Sheen color, sheen roughness |
| `extensions/KHR_materials_iridescence/iridescenceTexture`, `iridescenceThicknessTexture` | Iridescence amount, film thickness |
| `extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture`, `specularGlossinessTexture` | Base color; packed roughness and specular color |

For grouped extension rows, each texture name shares the shown extension prefix.
Slot precedence matches material import: legacy diffuse and packed
specular/glossiness textures override the corresponding core/specular slots.
Legacy factors use the same existing material approximation as import; animation
does not change the selected material workflow.

Vector properties other than quaternion rotation can target individual elements
with a final `/{component}` token. Material extension objects and texture
objects must exist. Texture-transform animation additionally requires the
existing `KHR_texture_transform` object. Missing offset/scale/rotation leaves
use their schema defaults of `[0,0]`, `[1,1]`, and `0`. Component pointers preserve
the other component, and selecting rest pose restores the exact authored
optional objects and leaves. Applying a sample changes only the numeric leaf;
texture indices, `texCoord`, other extensions, and extras remain authored.
Malformed supported parents/vectors are rejected with channel context.
Optional camera `aspectRatio`/`zfar` and light `range` must be defined in the
source to be targeted. The three Boolean node flag pointers use SCALAR
`UNSIGNED_BYTE` output and `STEP` interpolation. An existing extension object
can omit its Boolean property to use the default `true` value. Zero keys
evaluate to `false` and nonzero keys evaluate to `true`. [glTF node flags](gltf-node-flags.md)

The runtime diagnoses pointers to other Boolean or integer fields, including
texture indices and UV-set selection, as well as extras and anisotropy fields
that the shaders do not represent.

## Renderer publication

Source node TRS updates resolve through `SceneNode::sourceNodeIndex`, including
joint/group nodes. Runtime primitive children and editor-created descendants
retain their transforms and identities. Graph world transforms are refreshed
before object data and scene providers are published. Changed material factors
update the existing runtime material and GPU material buffer. Per-slot animated
UV offset/scale/rotation update that same material without replacing uploaded
texture handles, samplers, UV-set selection or packed-channel interpretation.
The shader applies scale, rotation, then offset according to
[`KHR_texture_transform`](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_texture_transform/README.md).
Authored punctual lights are
recollected from current graph transforms, including parent edits; the synthetic
editor root is removed from these records because lighting applies that anchor
separately. The root association uses a dedicated flag and survives renaming.

The **Camera** selector chooses a source camera node, including its world
transform and roll. Source perspective aspect ratios and orthographic
magnitudes are respected. Projections follow the engine's right-handed,
reverse-Z conventions with no matrix Y flip. An absent perspective `zfar` uses
the infinite reverse-Z projection and a finite 1,000,000-unit lighting/culling
span; the source camera remains unchanged. Camera data, the active ECS camera,
and temporal projection metadata use the same selected source camera.
Directional cascades also receive that camera's world view, near/far and
projection. Infinite main projections use a finite projection of the same
authored frustum for cascade-corner reconstruction; this keeps shadow bounds
finite while preserving the main camera's infinite reverse-Z depth mapping.

Animation runs after shared GPU readers retire; asynchronous DDGI work is
synchronized before changing shared scene data. Timeline jumps, loop wraps,
camera selection, mutable shading and deformations invalidate incompatible
temporal history. Rigid TRS playback retains stable object identities so the
existing previous-transform path can represent ordinary motion. Source model
replacement resets the player and camera selection with graph identity.

Morph and skin deformation publication is a separate renderer consumer of the
same updated source nodes and weights. The animation sampler itself supplies no
GPU deformation or previous-vertex history. Skin/morph rendering support must be
assessed with the deformation path and its validation, independently of CPU
sampling success. The current scene exporter rejects animated source scenes;
playback does not claim animation authoring or animation-preserving export.

## Validation

`gltf_animation_tests` includes independent known-value interpolation fixtures,
non-unit cubic intervals/derivatives, quaternion antipodes and tangent
normalization, grouped morph weights, normalized integer decoding, dense and
sparse data, malformed accessor/timeline cases, typed pointer values and
overlaps, rest restoration, playback controls and large finite wrapping.
Runtime fixtures verify source-node association, editor-child preservation,
graph replacement, numeric material/texture-handle separation, source-camera
roll, finite/infinite perspective reverse-Z and orthographic projection.
Texture fixtures cover all 18 shader-backed core/extension texture infos,
whole-vector and component interpolation, non-unit cubic derivatives, STEP
rotation, absent/malformed parents, exact optional metadata/rest restoration,
packed-channel preservation, clearcoat normal scale, and legacy factors.

The opt-in native regression creates a self-contained fixture and captures TRS,
material, punctual-light, camera-pointer and texture-motion clips at times zero and one in both
raster techniques:

```powershell
python tests/validation/gltf_animation_runtime_regression.py --renderer out/build/feature-parity/VulkanSceneRenderer.exe --output out/animation-runtime
```

It requests synchronization validation, requires fresh images and telemetry,
checks the graph/runtime material/light/camera values, and requires meaningful
pixel differences. Material captures additionally check red-to-blue output;
light captures require increased brightness. The report records commands,
executable/image hashes and measured differences. Run this after building the
application and staging its shaders/assets.
Repeat `--clip N` to restrict the clip set. A focused camera/cascade smoke run
uses `--clip 3 --directional-intensity 1`, retaining both raster techniques.
`--clip 4` runs a separate embedded-PNG texture-motion fixture with an asymmetric
color pattern, fixed UVs and a fractional offset from `[0.05,0.05]` to
`[0.42,0.2]`. A clamped sampler and nonuniform scale avoid whole-period wrapping
or a uniform texture hiding a missing update. The check requires supported
pointer samples, unchanged node geometry, meaningful pixel differences and
clean validation in both raster techniques. Earlier four clips retain their
original fixture.

Measured local validation on 2026-10-25 passed all 37 animation CPU/runtime
tests, including the texture-transform and binary64 expansions. Python syntax, fixture
structure and the embedded PNG were validated independently. Native texture
motion also passes four native captures after the coordinated renderer rebuild:
both raster techniques change 5.36% of pixels, with mean absolute change
6.73/255, while geometry remains unchanged. No Vulkan/synchronization errors
were reported. Commands, hashes and metrics are in
`out/feature-parity/animation-texture-runtime/results.json`. The earlier
native measurements below cover the original four clips.
The first native matrix passed all eight clip/technique pairs (16
captures). Pixel changes between the two paused times were:

| Clip | Deferred changed pixels | Forward changed pixels | Semantic assertion |
| --- | ---: | ---: | --- |
| Translation | 15.52% | 13.73% | Source and graph translation move from −0.6 to +0.6 |
| Material base color | 6.67% | 6.67% | Existing runtime material and visible output change from red to blue |
| Punctual-light intensity | 6.67% | 6.67% | Runtime intensity changes from 2 to 80 and rendered brightness increases |
| Authored-camera field of view | 29.03% | 27.71% | Selected source camera projection follows 0.5/1.0-radian keys |

The final binary was additionally checked after connecting authored camera
matrices to directional cascades. Both raster techniques passed the camera-only
run with directional intensity 1 (four fresh captures), with changed-pixel
fractions of 28.44% deferred and 26.95% forward. No VUID or synchronization
validation failures were reported in either run. Evidence is recorded in
`out/feature-parity/animation-runtime/results.json` and
`out/feature-parity/animation-camera-shadow-runtime/results.json`, including
the distinct executable hashes and complete commands for each run.

The interpolation behavior follows the
[glTF animation specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#animations)
and its cubic/linear interpolation definitions. These native results establish
TRS and the tested numeric material/light/camera/texture pointer paths.
Skin/morph rendering and previous-vertex history have separate
[deformation runtime checks](gltf-deformation.md); CPU sampling alone does not
establish their renderer support.
