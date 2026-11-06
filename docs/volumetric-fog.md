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
or spot lights respectively, in published light-table order. IES fixtures
use their authored angular distribution. Directional and eligible punctual
lights sample the existing reverse-Z raster shadow atlases.

The pass injects lighting, integrates each froxel column with Beer–Lambert
attenuation, and composites the fog into HDR before temporal reconstruction,
exposure, bloom, and UI. Composition stops at each pixel's resolved opaque
depth, including the partial final slice. A background pixel integrates to
the fog distance. Transparent surfaces use their existing later resolve
and overlay the fogged opaque image. Area light injection, self-shadowing,
and multiple scattering are outside this homogeneous medium profile.

Temporal reprojection uses the previous submitted froxel field and rejects
out-of-frustum samples and large local radiance changes. Settings, camera
cuts, scene and lighting edits, shader replacement, viewport resize, and
technique changes invalidate the relevant history. Two froxel fields and
one cumulative field use 16 bytes per cell each. Per-frame uniform data and
the actual VMA allocation total appear in capture telemetry alongside the
`VolumetricFog` GPU pass time.

The native regression is `tests/validation/volumetric_fog_runtime_regression.py`.
It compares the scene-linear attenuation with independent analytic values
and checks zero-density bypass, shadows, and temporal history.
[Froxel integration reference](https://advances.realtimerendering.com/s2015/)

On the RTX 2080 SUPER at 320×200, the native fog pass measured 0.038–0.062 ms
and allocated 3,073,920 bytes. The captured transmittance was 0.54834 at
three units and 0.20178 at eight units for an extinction of 0.2 per unit.
The same fixtures verified directional and point shadow response, camera
motion history, cut rejection, and viewport resize. Evidence is in
`out/feature-parity/volumetric-fog-native/results.json`.
