# Screen-space reflections

In the deferred renderer's Lit view, select `--screen-reflections low` or
`--screen-reflections high` at launch. For headless captures, also pass
`--display-mode lit`. Low and high allow up to 24 and 72 cell advances per
pixel, respectively. Rougher surfaces use fewer advances. Receiver departure
and mip descent share a separate allowance of two probes per selected mip
level. `confidence` displays the hit blend weight. `distance` displays the
dominant hit distance normalized to the 40 m trace limit. `source` shows the
screen contribution in green and the optional ray contribution in blue.
`history` displays the pixels that reused a previous reflection sample.
The default is `off`.

The pass snapshots the opaque scene-linear HDR buffer after lighting. It
projects each reflection ray into the current reverse-Z depth grid, clips it
to the view frustum, and traverses cells of a depth-bounds pyramid. Each cell
stores the nearest and farthest surfaces over its footprint in reverse-Z. The
marcher skips a cell when the ray stays entirely in front of the nearest
surface or behind the farthest surface. The hierarchy starts at half
resolution and reads scene depth directly at full resolution. Projected ray length and roughness
set the mip limit within each quality mode. Reciprocal clip W
interpolation supplies the view-space depth for the final check. A bounded
thickness test and the hit surface normal reject silhouette mismatches and
back faces. The full-resolution trace remains available if the optional
pyramid is unavailable.

See [Efficient GPU Screen-Space Ray Tracing](https://jcgt.org/published/0003/04/04/)
and [FidelityFX Stochastic Screen-Space Reflections](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/).

For a valid hit, the pass blends the hit radiance into the receiver's base
specular IBL using the existing split-sum BRDF. It removes the corresponding
fraction of the original specular IBL. Rough surfaces use a depth- and
normal-aware color filter, and confidence falls to zero by roughness 0.55.
Confidence also fades near the screen edge and the trace limit. Misses keep
the original lit pixels. Direct light, diffuse GI, clearcoat, and sheen keep
their existing evaluation.

The pass handles opaque and alpha-masked receivers. Transparency still
composites after the opaque reflection pass. Transmission receivers and aerial
perspective use the existing lighting route. Forward rendering and ray-shadow
visibility views bypass the reflection pass.

## Trace diagnostics

Use `--screen-reflections hit-uv --screenshot hit.exr` to read the
hit texel's center UV in R and G and the hit distance in scene metres in B.
Misses write zero across all three channels. `--screen-reflections miss-reason`
encodes the screen trace result in RGB. In a scene-linear EXR, red, green,
and blue carry bits 0, 1, and 2 of the reason code. Both views show the
current screen ray before temporal filtering or ray-query fallback.

| Code | Color | Result |
| --- | --- | --- |
| 0 | Black | Valid screen hit |
| 1 | Red | Ineligible receiver |
| 2 | Green | Invalid or degenerate projected ray |
| 3 | Yellow | Ray reached the screen or frustum edge |
| 4 | Blue | Ray reached the distance limit without an intersection |
| 5 | Magenta | Full-resolution depth or thickness test rejected the candidate |
| 6 | Cyan | Hit normal faced away from the incoming ray |
| 7 | White | Traversal exhausted its step budget |

![Mirror fixture with the screen hits in black, clipped exits in yellow and thickness rejections in magenta](images/screen-space-reflections/miss-reasons.png)

## Hi-Z validation

The numeric fixture reads scene-linear EXR and checks the reflected card against
the projected pixel and physical hit distance. At 480×360 it measured 1.980 m
against 1.985 m analytically. It also checks a thin card at 483×363, roughness
attenuation, a clipped screen exit, and the depth-pyramid fallback when the
reduction shader is unavailable.

The 1920×1080 Low fixture projects a 506-pixel ray to the red card. The new
marcher reaches it both through empty space and past a foreground strip whose
screen projection crosses the ray. Both cases measured a mean red channel of
182 over the 7×7 receiver patch, up from 47 with reflections off.

| Full-resolution trace | Depth-bounds traversal |
| --- | --- |
| ![Low-quality fixture before Hi-Z with the card above a gray receiver](images/screen-space-reflections/low-before.png) | ![Same fixture after Hi-Z with the red card reflected below](images/screen-space-reflections/low-hiz.png) |

On an RTX 2080 SUPER with driver 610.88, median High pass time at 480×360 fell
from 0.156 ms to 0.133 ms over 24 measured frames with Vulkan synchronization
validation. The pyramid adds 1,976,832 bytes and raises total pass allocation
with history from 10,321,920 to 12,298,752 bytes.

Run the fixture with Python, NumPy, Pillow, OpenCV with OpenEXR support, and an
available Khronos validation layer.

```powershell
python -B tests/validation/screen_reflections_runtime_regression.py `
  --renderer <renderer.exe> --output build/ssr-validation --managed-output `
  --marcher --validation-manifest <VkLayer_khronos_validation.json>
```

## Reflection history

Low and high quality vary the reflected direction over a small roughness-scaled
cone on each submitted frame. A spatial filter samples the screen hit
neighborhood and rejects depth, normal, and material boundaries. The temporal
filter reprojects the receiver into the
previous jittered camera and tests reconstructed position, normal, material,
hit material and source, hit distance, confidence, and radiance agreement.
It averages only the signed specular correction. A current miss immediately
restores the existing IBL. Exposure, fog, transparency, and TAA or FSR are
applied later.

Camera cuts, material or light edits, resolution or technique changes, and
shader replacement invalidate reuse. The previous camera and reflection
history advance on a successful graphics submission. Moving receivers that
cannot match the previous surface use their current reflection sample. This
history uses the last submitted frame even when TAA is off.

Two packed RGBA32UI history images add 32 bytes per render pixel. History
falls back to the deterministic trace if the 128 MiB budget or image
allocation prevents it. The mirror direction and the confidence, distance,
and source diagnostics remain deterministic.

## Off-screen fallback

Combine `--screen-reflections high` or `low` with
`--screen-reflections-ray-query` to fill screen misses and low-confidence
samples with the closest ray hit. The fallback reuses the shared acceleration
structure and surface attributes. The screen contribution keeps priority and
the ray spends only the remaining blend weight. A ray miss keeps the original
IBL in that weight. A compatible device can build the shared scene even with
raster shadows selected. Without an available ray shader or ray scene, the
screen-only pipeline remains active. Screenshot telemetry records whether the
ray mode was requested, available, and active.

The fallback evaluates glTF base color, vertex color, normal and material
textures, unlit and emissive radiance, and a single metallic-roughness lobe.
It uses one reflected ray per eligible pixel. Lit hits sample the existing IBL
and shadow one directional light, up to four or eight punctual lights, and up
to two or four area lights in low or high mode. Area lights use their centers
for this visibility estimate. Directional shadow tests use the 40 m range.
Alpha masks and section clipping apply to both hit and shadow queries.
Blended and refractive geometry remains in the transparent pass. Additional
surface lobes, recursive reflections, and volumetric transport use their
existing rendering routes.

The pass owns an RGBA16F opaque snapshot per swapchain image when enabled.
Screenshot telemetry reports actual reflection allocation bytes, depth
pyramid bytes and mip count, and whether the submitted reflection pass used the
pyramid. The `ScreenReflections` GPU interval includes pyramid generation and
reflection shading. Ray surface allocation and build costs appear in shared
ray-scene telemetry. Runtime shader reload and the Windows package include
both reflection kernels and the depth-bounds reduction kernel.

The 480×360 off-screen mirror fixture on an RTX 2080 SUPER measured 0.181 ms
for the screen pass and 0.235 ms with ray fallback. The opaque snapshots used
4,423,680 bytes in both cases, and the fixture's shared ray scene used 18,368
bytes. The fallback revealed the off-screen card and respected its opaque
occluder, alpha cutout, and directional shadow. The full 19-capture Vulkan
validation run also verified pixel-identical fallback with ray queries
disabled. The captures and timings are recorded in
`out/feature-parity/screen-reflections-ray-native/results.json`.

The history run passed 57 captures across 28 Vulkan processes. The thin red
target fell from a mean red value of 190 to 47 on the first frame after
disocclusion, and stayed at 47 with and without TAA. The history view also
confirmed reuse on an off-screen ray hit. The lifecycle captures verified
camera cuts, material and light edits, resize, and raster technique switches.
With history enabled at 480×360, the off-screen pass measured 0.18 ms with
screen tracing and 0.24 ms with ray fallback. Total reflection allocation
was 10,321,920 bytes. Evidence is in
`out/feature-parity/screen-reflections-temporal-native/results.json`.
