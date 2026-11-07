# Volumetric fog

`--volumetric-fog low` and `--volumetric-fog high` enable homogeneous
single-scattering fog in the forward and deferred raster renderers.
`--fog-density` sets extinction per scene unit from 0 to 4. `--fog-albedo`
sets the scattering fraction from 0 to 1. `--fog-anisotropy` sets the
Henyey–Greenstein phase parameter from −0.9 to 0.9. Positive values concentrate
light toward the incident propagation direction. `--fog-distance` limits the
view-space integration depth from 0.5 to 5,000 scene units. `off` and zero
density bypass the pass and allocate no fog buffers. A device without
RGBA16F storage support uses the normal raster path and reports the fallback.

`transmittance` displays the remaining background fraction and `scattering`
displays the scene-linear light added by the fog. Both use the high grid.
The low grid starts at 16×16 pixels and 32 depth slices. The high grid starts
at 8×8 pixels and 64 slices. The renderer increases the tile size as needed
to keep each grid below 1.2 million froxels. Quadratic depth placement puts
more samples close to the viewer. Low and high inject up to 16 and 32 point
or spot lights and up to four and eight area lights respectively, in
published light-table order. IES fixtures use their authored angular
distribution. Rectangle and disk emitters use four equal-area samples, their
one-sided emission, and a bounded near-field solid-angle estimate. Directional
lights and eligible local lights sample the existing reverse-Z raster shadow
atlases. Area lights average visibility across the available atlas sample
origins.

The pass injects lighting, integrates each froxel column with Beer–Lambert
attenuation, and composites the fog into HDR before temporal reconstruction,
exposure, bloom, and UI. Composition stops at each pixel's resolved opaque
depth, including the partial final slice. A background pixel integrates to
the fog distance. The pass also applies transmittance and scattering at
the depth of each alpha-blended or transmissive surface in the existing
order-independent transparency list. The resolve keeps each surface's
shaded opacity and sorts the layers as usual, so fog behind foreground
glass is visible in proportion to the glass's remaining transparency.
The glass material's existing transmission approximation shades the
surface before fog is applied along the camera ray. Sparse OpenVDB volumes
retain their own interleaved surface composition. This homogeneous medium
uses single scattering with opaque geometry shadows.

Temporal reprojection uses the previous submitted froxel field and rejects
out-of-frustum samples and large local radiance changes. Settings, camera
cuts, scene and lighting edits, shader replacement, viewport resize, and
technique changes invalidate the relevant history. Two froxel fields and
one cumulative field use 16 bytes per cell each. Per-frame uniform data and
the actual VMA allocation total appear in capture telemetry alongside the
`VolumetricFog` GPU pass time.

The native regression is `tests/validation/volumetric_fog_runtime_regression.py`.
It compares the scene-linear attenuation with independent analytic values
and checks zero-density bypass, shadows, and temporal history. It also
checks attenuation and added scattering for isolated and overlapping alpha
and glass layers in both raster techniques. The rectangle and disk fixtures
compare scene-linear scattering with numerical integration, including
occlusion and one-sided emission. `--with-mode-switches` adds
coverage for FSR, reference path tracing, and viewport resize on supported systems.
[Froxel integration reference](https://advances.realtimerendering.com/s2015/)

On the RTX 2080 SUPER at 320×200, the native fog pass measured 0.038–0.062 ms
and allocated 3,073,920 bytes. The captured transmittance was 0.54834 at
three units and 0.20178 at eight units for an extinction of 0.2 per unit.
The same fixtures verified directional and point shadow response, camera
motion history, cut rejection, and viewport resize. Evidence is in
`out/feature-parity/volumetric-fog-native/results.json`.

On the same GPU and viewport, fog with overlapping alpha and glass layers
measured 0.057–0.075 ms at the same allocation size. The largest mean RGB
error against the analytic layer fixtures was 0.000155 across both raster
techniques. Evidence for the layered fixtures and FSR, reference, and resize
switches is in `out/feature-parity/volumetric-fog-transparent-native/results.json`.
