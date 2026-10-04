# Vulkan Coordinate Setup

This document defines the coordinate, depth, viewport, winding, and normal
conventions for VulkanSceneRenderer.

Primary references:
- [Vulkan Guide: Depth](https://docs.vulkan.org/guide/latest/depth.html)
- [Vulkan Spec: Polygon Rasterization](https://github.khronos.org/Vulkan-Site/spec/latest/chapters/primsrast.html)
- [glTF 2.0: doubleSided materials](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#double-sided)

The most important rule is to keep these spaces separate:

1. World and view space
2. Clip and NDC space
3. Framebuffer coordinates
4. Sampled image UV coordinates

Most past bugs came from mixing the last three.

## Implementation Anchors

Use these files as the concrete implementation points for the conventions below:

- `include/Container/common/CommonMath.h` defines projection helpers.
- `src/renderer/core/FrameRecorder.cpp` sets scene and shadow viewports.
- `src/renderer/pipeline/GraphicsPipelineBuilder.cpp` defines front-face and cull state.
- `shaders/brdf_common.slang` reconstructs world positions from scene depth.
- `shaders/shadow_common.slang` converts shadow NDC to atlas UV.
- `shaders/lighting_structs.slang` holds shared reverse-Z depth helpers.
- `include/Container/renderer/temporal/TemporalConventions.h` and
  `shaders/temporal_common.slang` define temporal projection/reprojection math.

Temporal velocity, jitter, history grids, and reset rules are specified in
[Temporal rendering contracts](temporal-rendering.md). Consult that contract
before adding motion-vector or history consumers.

When adding a pass, add a short local comment at the conversion point if it
depends on viewport orientation, reverse-Z depth, or glTF double-sided normal
rules.

## 1. World and View Space

The engine uses a right-handed world:

| Property | Value |
| --- | --- |
| Handedness | Right-handed |
| +X | Right |
| +Y | Up |
| Camera forward | -Z |
| Camera view matrix | `glm::lookAt`, no handedness fixup |

Implications:
- World-space geometry, normals, tangents, and light directions stay in a
  right-handed basis.
- The projection matrix is not allowed to hide a Y flip.
- Any mirrored transform with `det(model3x3) < 0` changes the tangent-frame
  handedness and must be accounted for when building the bitangent sign.

## 2. Clip Space and Reverse-Z

Vulkan uses `Zc` in the clip volume `0 <= Zc <= Wc` by default. After
perspective divide:

- `x_ndc` and `y_ndc` are in `[-1, 1]`
- `z_ndc` is in `[0, 1]`

VulkanSceneRenderer uses reverse-Z:

| Quantity | Value |
| --- | --- |
| Near plane | `z_ndc = 1` |
| Far plane | `z_ndc = 0` |
| Depth clear | `0.0` |
| Depth compare | `VK_COMPARE_OP_GREATER_OR_EQUAL` |

Rules:
- `perspectiveRH_ReverseZ()` and `orthoRH_ReverseZ()` must not flip Y.
- Reverse-Z changes only depth mapping. It does not change winding or normal
  orientation.

## 3. Framebuffer Coordinates

Vulkan front-face classification is defined from the polygon area in
framebuffer coordinates. Framebuffer coordinates are not the same thing as NDC.

Framebuffer convention:

| Property | Value |
| --- | --- |
| Origin | Upper-left |
| +X | Right |
| +Y | Down |

That means there are two legal viewport setups in this renderer:

### Scene passes

Scene-facing graphics passes use a negative-height viewport:

```cpp
viewport.x = 0.0f;
viewport.y = float(height);
viewport.width = float(width);
viewport.height = -float(height);
viewport.minDepth = 0.0f;
viewport.maxDepth = 1.0f;
```

Effect:
- `y_ndc = +1` maps to the top of the framebuffer.
- The projection matrix stays clean.
- Scene raster state still keeps glTF-native CCW triangles as front faces.
  Do not compensate with `VK_FRONT_FACE_CLOCKWISE`; that routes visible
  single-sided surfaces through their back faces and stores inward-facing
  G-buffer normals.

### Shadow passes

Shadow cascades use a positive-height viewport:

```cpp
viewport.x = 0.0f;
viewport.y = 0.0f;
viewport.width = float(shadowMapSize);
viewport.height = float(shadowMapSize);
viewport.minDepth = 0.0f;
viewport.maxDepth = 1.0f;
```

Effect:
- `y_ndc = -1` maps to the top row of the shadow image.
- Shadow NDC to atlas UV uses the default Vulkan mapping with no extra Y flip.

## 4. UV <-> NDC Conversion

This is the part that must stay explicit.

### Scene framebuffer UV to scene NDC

Use this for world-position reconstruction from the main depth buffer:

```slang
float2 ndc;
ndc.x = uv.x * 2.0 - 1.0;
ndc.y = 1.0 - uv.y * 2.0;
```

Equivalent form:

```slang
float2 ndc = uv * 2.0 - 1.0;
ndc.y = -ndc.y;
```

### Scene NDC to scene framebuffer UV

Use this when a world-space sample is projected back into the main scene
buffers:

```slang
float2 uv;
uv.x = ndc.x * 0.5 + 0.5;
uv.y = 0.5 - ndc.y * 0.5;
```

### Shadow NDC to shadow atlas UV

Shadow cascades use a positive viewport, so they do not reuse the scene Y flip:

```slang
float2 uv = shadowNdc.xy * 0.5 + 0.5;
```

The inverse is:

```slang
float2 shadowNdc = shadowUv * 2.0 - 1.0;
```

## 5. Winding and Culling

Global rasterization defaults:

| Setting | Value |
| --- | --- |
| Scene front face | `VK_FRONT_FACE_COUNTER_CLOCKWISE` |
| Shadow front face | `VK_FRONT_FACE_COUNTER_CLOCKWISE` |
| Opaque scene cull | `VK_CULL_MODE_BACK_BIT` |
| Shadow cull | `VK_CULL_MODE_BACK_BIT` |

Rules:
- Negative-height scene viewports are part of the scene UV/NDC convention, but
  scene and shadow raster front-face state both preserve glTF's CCW winding.
- Reverse-Z is unrelated to front-face classification.

glTF-specific rule:
- Imported vertex normals are used to repair bad triangle winding before the
  mesh is flattened into renderer buffers.
- When `doubleSided == false`, back-face culling stays enabled after import
  repair.
- When `doubleSided == true`, back-face culling is disabled and back-face
  normals are reversed before lighting.
- Mirrored node transforms route through front-cull pipeline variants because
  the transform, not the mesh data, flips framebuffer winding.

## 6. Normals and Tangent Space

G-buffer normal convention:

| Property | Value |
| --- | --- |
| Stored space | World space |
| Encoding | `normal * 0.5 + 0.5` |
| Decoding | `encoded * 2.0 - 1.0` |

Normal-map convention:

| Property | Value |
| --- | --- |
| Normal map space | Tangent space |
| Tangent source | glTF tangent attribute |
| Tangent sign | `tangent.w` |
| Bitangent | `cross(N, T) * tangentSign` |
| glTF normal scale | Multiply tangent-space `xy` before normalization |
| glTF occlusion strength | `mix(1.0, occlusionTex.r, strength)` |

Rules:
- The normal matrix is `transpose(inverse(model3x3))`.
- The tangent must be orthogonalized against the transformed geometric normal.
- Mirrored transforms multiply the tangent handedness by the sign of
  `det(model3x3)`.
- For double-sided lighting, negate the final lighting normal on back faces.

## 7. Pass-by-Pass Checklist

| Pass | Viewport | Depth | UV/NDC rule | Notes |
| --- | --- | --- | --- | --- |
| Depth prepass | Negative height | Reverse-Z write | None | Alpha mask only |
| G-buffer | Negative height | Reverse-Z equal/read | None | Must receive full vertex layout including normal+tangent |
| Directional light | Negative height | Samples main depth | Scene UV -> scene NDC | Shadow sampling uses positive-height shadow rule |
| Point light | Negative height | Samples main depth | Scene UV -> scene NDC | Same reconstruction as directional |
| Tiled point light | Negative height | Samples main depth | Scene UV -> scene NDC | Same reconstruction as directional |
| GTAO | Compute | Samples main depth | Scene NDC -> scene UV when reprojecting | Y flip required |
| Frustum cull | Compute | N/A | Clip planes come from Slang row access on `viewProj` | Do not use GLSL-style column extraction |
| Occlusion cull | Compute | Samples Hi-Z | Scene NDC -> scene UV | Y flip required; sphere bounds must use projection scale and nearest-point reverse-Z depth |
| Tile light cull | Compute | Samples main depth | Scene UV -> scene NDC for tile corners | Y flip required |
| Shadow depth | Positive height | Reverse-Z write | None | Uses light-space orthographic projection and matches alpha-mask discard |
| Shadow lookup | N/A | Samples shadow atlas | Shadow NDC -> shadow UV | No scene-style Y flip |
| Post-process | Negative height | Samples scene buffers | Usually derive from `SV_Position` | Avoid unnecessary NDC conversion; depth debug views must linearize reverse-Z instead of using `1 - depth` |

## 8. Current Engine Decisions

These are the conventions the code should implement everywhere:

- Right-handed world and view space
- Reverse-Z depth with clean projection matrices
- Negative-height viewport for scene-facing passes only
- Positive-height viewport for shadow cascades
- Counter-clockwise front faces for scene passes
- Counter-clockwise front faces for shadow passes
- World-space normals in the G-buffer
- glTF tangent handedness and double-sided lighting rules

If a new pass needs depth reconstruction or reprojection, pick one of the
conversion rules in section 4 and write it explicitly in code. Do not infer it
from another pass.
