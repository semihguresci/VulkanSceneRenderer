# Sample scene rendering review

Reviewed on 2026-10-04 with the native Visual Studio Release build, NVIDIA
GeForce RTX 2080 SUPER, driver 591.86 and Vulkan 1.4.325. These results concern
the current working tree after the TAA implementation, rather than the already
published preview binary.

## Fixed scene lighting problems

The default Cornell sample intentionally set directional and environment
intensity to zero. The model reload path kept those settings when replacing
Cornell with BIM content. Without authored lights, that content rendered almost
black. The same BIM file opened directly was illuminated.

Startup and reload now share a scene lighting policy. Isolated Cornell samples
retain their authored local lighting. Other scenes and mixed glTF/BIM scenes
use the viewer defaults: directional intensity 2 and environment intensity 1.
Each setting follows automatic defaults until the user edits Lighting Settings.
Edited values and explicit command-line zero overrides survive subsequent loads. Bloom,
artistic bounce fill and the local shadow layer budget follow the same scene
transition policy.

The Cornell shadow fixture also had two problems:

- Full-strength artistic bounce added untraced illumination to occluded
  receivers. The isolated samples now start with bounce intensity zero; the
  lighting controls still offer the approximation and explain its limitation.
- Eight shadow layers fit only one six-face visibility cube. The area light
  therefore used a single emitter origin. The isolated samples now use the
  existing 24-layer atlas budget, allowing four distributed emitter origins.
  The atlas allocation size is unchanged; recording additional shadow maps
  increases GPU work for these samples.

Both the ordinary and yellow Cornell assets receive these defaults, including
when loaded by an absolute Windows path. Explicit directional colour, direction
and intensity overrides continue to apply.

## Regression results

The new GPU regression executes Cornell -> BIM -> Cornell in both deferred and
forward rendering. It also checks direct BIM loading, deliberately zero lighting
and mixed providers. Capture telemetry now reports effective lighting values,
light counts, the layer budget and the active layer count.

| Check | Deferred | Forward |
| --- | --- | --- |
| Switched BIM versus directly loaded BIM, linear RGB MAE | 0 | 0 |
| BIM surface mean linear RGB | 0.6146 | 0.6151 |
| Explicit zero intensity survives reloads | Pass | Pass |
| Mixed providers receive viewer illumination | Pass | Pass |
| Vulkan VUID / synchronization hazards | None | None |

All seven scene-default unit cases, 194 rendering convention cases, temporal
state/capture tests and analytic shadow-sampling tests pass. The forward 1x/4x
MSAA surface check also passes.

All four independent Cornell probes now pass:

| Probe | Result | Required |
| --- | --- | --- |
| Floor shadow / lit luminance | 0.3089 | <= 0.70 |
| Umbra -> penumbra luminance delta | 0.3651 | >= 0.08 |
| Penumbra -> lit luminance delta | 0.2878 | >= 0.08 |
| Red wall target-colour fraction | 0.8205 | >= 0.65 |
| Green wall target-colour fraction | 0.7167 | >= 0.65 |

Before the fixes, the shadow ratio was 0.8876 and the penumbra ordering failed.
Against the historical Cornell golden, image MAE improved from 0.05112 to
0.00360 and the 95th-percentile error improved from 0.21554 to 0.01041.
**The historical image comparison still fails** its different-pixel-fraction
budget: 0.3319 versus 0.01. The reference and tolerances were preserved. This
does not count as a passing golden-image test.

## Remaining findings

### P1: STEP IFC silently loses unsupported shape representations

Tekla House's STEP IFC capture contains isolated components, while the IFCX
capture contains a substantially more complete building. This is independent
of light intensity: both captures report directional intensity 2 and
environment intensity 1.

[`IfcTessellatedLoader.cpp`](../src/geometry/IfcTessellatedLoader.cpp) accepts
triangulated face sets and selected extruded solids in geometry traversal. The
Tekla source contains 2,786 Boolean results, 448 Boolean clipping results, 3,887
swept disk solids and 14 faceted B-reps that this traversal does not handle.
Hello Wall also contains six polygonal face sets. Import completion currently
does not expose an unsupported-shape warning to the user.

Add representation diagnostics and explicit partial-import status. Supporting
these shapes needs geometry conversion/CSG work, rather than a lighting change.

### P2: Forward rendering has a black environment background

The same coverage sample has an HDR sky in deferred rendering and a black
background in forward rendering, with and without TAA. Forward surfaces still
receive environment illumination. The
[`forward lighting recorder`](../src/renderer/forward/ForwardRasterLightingPassRecorder.cpp)
clears HDR scene colour to black and does not record a background sky draw.
Add environment-background rendering to the forward technique and test sky
pixels independently of surface coverage.

### P2: Area-light penumbrae still show discrete visibility bands

Four emitter visibility samples restore the expected penumbra and pass the
fixture probes, but their discrete levels remain visible around the blocker.
The atlas/shader budget currently allows at most four complete cubes for one
area light. Higher-quality visibility needs more samples, stochastic sampling
with temporal accumulation, or traced visibility. The artistic bounce slider
can conceal these bands, but does not resolve their cause.

## Capture coverage and limitations

Captured the two Cornell variants, open-wall and closed-room blockers,
equivalent BIM scene, Hello Wall IFC/IFCX, Tekla House IFC/IFCX, PCERT architecture,
the native IFCX point cloud, and the mirrored/cutout/transparency coverage fixture
in both techniques with TAA. The PCERT sample is one composition layer; it is
not a complete federated scene when opened alone. The hall-style automatic
camera intentionally starts inside elongated models, so the building captures
are interior views rather than full-model overviews.

Local captures and logs are under `out/sample-review/`. The automated scene
lighting results are under `out/build/visual-studio/test_results/scene-lighting/`.
Optional glTF sample collections were not available in this checkout and were
not included. Validation reports the existing unused vertex-location warnings;
no VUID or synchronization hazards appeared in the captures.

Run from a Visual Studio Developer Console:

```powershell
cmake --build out/build/visual-studio --config Release --target VulkanSceneRenderer scene_lighting_defaults_tests rendering_convention_tests lighting_shadow_sampling_tests
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R "^(scene_lighting_defaults_tests|rendering_convention_tests|lighting_shadow_sampling_tests|temporal_convention_tests|temporal_capture_tests)$"
$env:CONTAINER_RUN_GPU_SCENE_LIGHTING = '1'
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^scene_lighting_gpu_regression$'
```

To retain the independent Cornell probe results and historical comparison:

```powershell
$env:CONTAINER_RUN_GPU_VISUAL_REGRESSION = '1'
$env:CONTAINER_VISUAL_REGRESSION_PLATFORM = 'windows-nvidia'
$env:CONTAINER_VISUAL_REGRESSION_SCENE = 'cornell_box_local_light_occlusion'
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^visual_regression_gpu_tests$'
```
