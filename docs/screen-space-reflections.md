# Screen-space reflections

In the deferred renderer's Lit view, select `--screen-reflections low` or
`--screen-reflections high` at launch. For headless captures, also pass
`--display-mode lit`. The modes cap each pixel at 24 or 72 depth
samples. `confidence` displays the hit blend weight. `distance` displays the
dominant hit distance normalized to the 40 m trace limit. `source` shows the
screen contribution in green and the optional ray contribution in blue.
`history` displays the pixels that reused a previous reflection sample.
The default is `off`.

The pass snapshots the opaque scene-linear HDR buffer after lighting. It
projects each reflection ray into the current reverse-Z depth grid, clips it
to the view frustum, and steps along its dominant screen axis. Reciprocal clip
W interpolation supplies the depth at each sample. A bounded thickness test
and the hit surface normal reject silhouette mismatches and back faces.
See [Efficient GPU Screen-Space Ray Tracing](https://jcgt.org/published/0003/04/04/).

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

The pass owns one RGBA16F opaque snapshot per swapchain image when enabled.
The screenshot telemetry reports actual allocation bytes and a separate
`ScreenReflections` GPU interval. Ray surface allocation and build costs appear
in the shared ray-scene telemetry. Both shader variants are included in runtime
shader reload and the Windows package.

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
