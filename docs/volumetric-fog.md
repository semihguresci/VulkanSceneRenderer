# Volumetric fog

In the Lit view of either raster renderer, open **Scene Controls → Go to
→ Volumetric Fog**. Choose **Low** or **High** to enable homogeneous
single-scattering fog, or **Off** to bypass it. The same modes are available
through `--volumetric-fog low`, `--volumetric-fog high`, and
`--volumetric-fog off`.

**Extinction** (`--fog-density`) sets attenuation per scene unit from 0 to 4.
**Scattering albedo** (`--fog-albedo`) sets the scattering fraction from 0 to 1.
**Anisotropy** (`--fog-anisotropy`) sets the Henyey–Greenstein phase parameter
from −0.9 to 0.9. Positive values concentrate light toward the incident
propagation direction. **Distance** (`--fog-distance`) limits the view-space
integration depth from 0.5 to 5,000 scene units. Off and zero extinction bypass
the pass and release its buffers. If the device lacks the required RGBA16F
storage operations or the fog pipelines cannot initialize, the renderer
reports the problem and continues on the normal raster path.

The diagnostic modes use the high grid.

- **Transmittance** (`transmittance`) shows the fraction of the background light that reaches the camera.
- **Scattering** (`scattering`) shows the scene-linear light added by the fog.
- **Froxel grid** (`froxels`) marks screen-space tile boundaries in cyan. The bands blend from blue to amber as the depth slice where the ray stops moves farther away.

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
depth, including the partial final slice. The opaque blend favors neighboring
froxel columns that stop at a similar depth. If no column matches a thin
surface, the pass still integrates to that pixel's exact depth. In perspective
views, the composite adjusts interpolated scattering to the opacity along the
actual pixel ray. A background pixel integrates to the fog distance. The pass
also applies transmittance and scattering at the depth of each alpha-blended
or transmissive surface in the existing order-independent transparency list.
Each transparent layer uses its own depth and standard spatial weights. The
resolve keeps its shaded opacity, so fog behind foreground glass is visible
in proportion to the glass's remaining transparency.
The glass material's existing transmission approximation shades the
surface before fog is applied along the camera ray. Sparse OpenVDB volumes
retain their own interleaved surface composition. This homogeneous medium
uses single scattering with opaque geometry shadows.

Temporal reprojection uses the previous submitted froxel field and rejects
out-of-frustum samples and large local radiance changes. Settings, camera
cuts, scene and lighting edits, shader replacement, viewport resize, and
technique changes invalidate the relevant history. Two froxel fields and
one cumulative field use 16 bytes per cell each. Low and High share buffers
sized for the larger grid so preset changes can reuse the allocation.
Per-frame uniform data and the actual VMA allocation total appear in capture
telemetry alongside the `VolumetricFog` GPU pass time.

## Capture sequences

Set `volumetricFogMode` in a capture sequence event to change the mode from
that frame onward. It accepts the same values as `--volumetric-fog` and
preserves extinction, albedo, anisotropy, and distance. A mode change resets
fog history.

```json
{
  "schemaVersion": 1,
  "frames": 12,
  "sampleFrames": [1, 4, 8, 12],
  "events": [
    {"frame": 4, "volumetricFogMode": "low"},
    {"frame": 8, "volumetricFogMode": "froxels"},
    {"frame": 12, "volumetricFogMode": "off"}
  ]
}
```

Save the sequence as `fog-modes.json` and run it from the renderer directory.

```powershell
.\VulkanSceneRenderer.exe --hidden --no-ui --model models/validation/taa_scene.gltf --display-mode lit --volumetric-fog off --capture-sequence fog-modes.json --screenshot fog.png
```

## Verification

The CTest targets cover parameter bounds and bypass on the CPU, and compare
native scene-linear EXRs from both raster techniques against analytic
attenuation and scattering. The fixtures check pixel-identical zero density,
perspective ray length, signed phase response, directional and point-light
blockers, and isolated and overlapping alpha and glass layers. The rectangle
and disk fixtures compare scattering with numerical integration, including
occlusion and one-sided emission. The GPU regression requires a display,
the Khronos validation layer, and Python with NumPy and OpenCV EXR support.
It captures pass timestamps, VMA allocation bytes, and temporal history.

```powershell
$env:CONTAINER_RUN_GPU_VOLUMETRIC_FOG = '1'
ctest --test-dir out/build/windows-release --output-on-failure -R '^volumetric_fog_'
```

The temporal regression compares motion, cuts, and resize against captures
at the same poses with history reset. It also tests a dark foreground
silhouette against a bright fog background and verifies preset and debug
switches in sRGB viewport PNGs. The EXRs, PNGs, telemetry, and validation
logs are indexed by `test_results/volumetric-fog-temporal/run-*/results.json`
under the build directory.

The single-scattering regression saves EXRs, validation logs, and a
`results.json` report under `test_results/volumetric-fog/run-*`. The clear
and blocked EXRs share the same camera and lighting. Its script can also
run directly with `--with-mode-switches` to cover FSR, reference path
tracing, and viewport resize on supported systems.
[Froxel integration reference](https://advances.realtimerendering.com/s2015/)

## Temporal and depth-edge measurements

On the GeForce RTX 2080 SUPER, 84 motion and silhouette EXRs and 18
viewport PNGs passed with Khronos synchronization validation. Cut and
resize frames matched the corresponding history-reset frames exactly.
During the held poses after motion, the largest 95th-percentile positive
residual in the shadow region was 0.01172 scene-linear units.

The covered-edge excess is the mean fog at the dark foreground edge
minus the mean fog in the dark interior at the same depth. The baseline
and depth-aware versions used the same scene and camera.

| Quality | Previous edge excess | Depth-aware edge excess | Deferred median (ms) | Forward median (ms) |
| --- | ---: | ---: | ---: | ---: |
| Low | 0.181700 | 0.006949 | 0.045 | 0.037 |
| High | 0.146257 | 0.018810 | 0.062 | 0.063 |

Edge measurements were the same in both techniques. Median times cover
seven timed frames per technique and quality at 320×200. Both techniques
allocated 3,073,920 bytes, falling to 1,968,000 bytes at 256×160. The
preset switch test returned to a pixel-identical Off viewport and released
the fog allocations. In an isolated runtime copy missing the injection
shader, switching from Off to Low and High reported the fallback and
kept the same fog-off pixels in both techniques.

## Earlier single-scattering measurements

On a GeForce RTX 2080 SUPER, 54 captures passed with Vulkan synchronization
validation enabled. Both raster techniques allocated 3,073,920 bytes at
320×200. The deferred viewport resize to 256×160 reduced the allocation
to 1,968,000 bytes. Timed fog passes at 320×200 measured the following
GPU durations across the regression fixtures.

| Technique | Frames timed | Minimum (ms) | Median (ms) | Maximum (ms) |
| --- | ---: | ---: | ---: | ---: |
| Deferred raster | 24 | 0.038 | 0.059 | 0.454 |
| Forward raster | 20 | 0.041 | 0.059 | 0.423 |

At an extinction of 0.2 per unit, the captured transmittance was 0.54834
at three units and 0.20178 at eight units. In the shadowed region, the
blocker reduced mean directional scattering by 70.7% and point-light
scattering by 99.9% in both techniques. Samples from the center and all
four corners of the 120° camera views agreed with analytic transmittance
and scattering within 0.00043 scene-linear units.
With the same deferred raster fixture and executable, the shader change
reduced the maximum top-left error from 0.005301 to 0.000120 in low quality
and from 0.002493 to 0.000120 in the high-quality scattering view.
Removing the injection shader from an isolated runtime copy produced a
fallback warning and a pixel-identical fog-off capture in each raster
technique.

An earlier run with overlapping alpha and glass layers measured
0.057–0.075 ms at the same viewport and allocation size. The largest mean
RGB error against the analytic layer fixtures was 0.000155 across both
raster techniques. Evidence for that run and FSR, reference, and resize
switches is in `out/feature-parity/volumetric-fog-transparent-native/results.json`.
