# Sample scene rendering review

Reviewed on 2026-10-04 with the native Visual Studio Release build, NVIDIA
GeForce RTX 2080 SUPER, driver 591.86 and Vulkan 1.4.325. These results concern
the current working tree after the TAA implementation, rather than the already
published preview binary.

## Fixed scene lighting problems

### UI model loading follow-up (2026-10-05)

The Scene Controls queue exposed a separate ordering error: lighting and bloom
were copied into the UI before processing the requested model load. GUI writeback
then restored the old scene's defaults. A reproduced Cornell-to-IFCX switch
loaded 14 objects but kept directional/environment intensity at zero. Model
requests now execute before publishing controls and opening the ImGui frame.
Detailed importer failures are retained and logged rather than replaced with a
generic failure message. BIM startup/reload uses an exterior overview, avoiding
the hall heuristic placing the camera inside a thin wall's bounds. The UI also
honors the initial `--display-mode` selection.

`gui_model_loading_gpu_regression` exercises the same selection/request methods
as the widgets with the UI enabled; it does not simulate OS mouse clicks. Both
renderers pass Cornell → native Hello Wall → IFCX → IFCX metadata layer → two
failed loads → Tekla IFCX → Tekla STEP → Cornell. Native imports represent 4/4
and 10,042/10,042 products. The UI-loaded IFC image matches direct startup exactly
outside the UI controls; failed-load restoration RGB MAE is at most 1e-8. Captures
contain no VUID or synchronization hazards. The five selected CPU suites pass
369 cases with the downloaded/archived assets enabled and no skips. The two
Python asset tests additionally verify LFS URL encoding, payload integrity,
cache reuse and preservation of pointers after failed downloads.

The classic IFC archive originally contained 171 Git LFS pointers, all of which
were materialized and SHA-256/size-verified before staging. An additional UI
sample selection loads `basin-tessellation.ifc` from that collection in both
renderers (1/1 represented product). This verifies payload recovery and the
classic sample-picker path; it does not establish importer coverage for all
171 files.

Run the GPU check with `CONTAINER_RUN_GPU_GUI_MODEL_LOADING=1` and CTest filter
`^gui_model_loading_gpu_regression$`. Captures are under
`test_results/gui-model-loading/`. The original zero-intensity reproduction is
retained locally under `out/gui-model-review/repro.*`.

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

### P1: STEP IFC geometry recovery (#61)

The original Tekla House STEP capture contained isolated components, while IFCX
contained a substantially more complete building. This was independent
of light intensity: both captures report directional intensity 2 and
environment intensity 1.

The original traversal accepted triangulated face sets and selected extruded
solids. The Tekla source contains 2,786 Boolean results, 448 Boolean clipping
results, 3,887 swept disk solids and 14 faceted B-reps; Hello Wall contains six
polygonal face sets. Unsupported shapes originally disappeared without warnings.

The native #61 implementation adds polygonal faces, planar faceted B-reps,
circular/hollow and structural profiles with fillets, curved profile outlines,
straight/curved swept disks and Manifold Boolean/clipping/opening subtraction.
It exposes complete/partial/failed status and per-product diagnostics. Hello Wall
imports 4/4 represented products and Tekla imports 10,042/10,042 with none skipped.
Native polylines, indexed line/arc curves and geometric curve sets now recover
11 Hello Wall and 43 Tekla curve instances. Both samples report complete imports
with zero representation warnings. These product counts differ from source
representation occurrences.
The upgraded Tekla STEP capture recovers the represented building and completes
in 15.02 seconds on the review machine. Quadratic relationship-graph construction
was fixed; its stage now takes 1.86 seconds. Four fresh fixed-camera captures and
unsupported-only/fallback startup checks remain free of VUID/synchronization
hazards. Six curve-only captures in deferred/forward, with MSAA 1/4 and TAA,
verify unlit source colors and native line submission. The forward native-primitive
pass and primitive-only depth/color initialization are now implemented.
Native handlers now cover every IFC 4.3 concrete curve family, including conics,
rational B-splines, trims/composites, offsets, surface curves, polynomial curves,
all six spirals, gradients and segmented reference/cant curves. Independent
analytic fixtures and isolated buildingSMART road/railway alignment checks pass.
An eight-panel curve gallery also renders in deferred and forward, with MSAA
and TAA checks, without VUID or synchronization hazards. Pcurve bases now also
include extrusion/revolution surfaces and rectangular/curve-bounded trims, with
hole-crossing checks and cycle rejection. Solid/hollow spline sweeps, closed-loop
seams and mitered bends have independent volume and topology checks. Four six-product
sweep captures in deferred/forward, with and without TAA, show no seam-cap
artifacts or VUID/synchronization hazards. Polygonal fillets, sectioned
surface meshes/pcurves, and boxed/polygon-bounded half-space operations are now
implemented. Their independent volume, area and topology checks pass. A further six-product gallery
renders in both techniques with TAA off/on, without VUID/synchronization hazards.
Sloped L/U/I profiles, faceted cavity shells and planar advanced B-reps are now
implemented. Tests cover radians/degrees, signed tapers, tangent fillets,
rotated profiles, line/polyline/spline edge trimming, face holes, face colors,
multiple cavities, millimetre units and Boolean operands. Invalid winding,
broken edge connectivity, touching/nested/outside cavities and open shells
reject before geometry buffers are changed. The six-product profile/B-rep
gallery renders in deferred/forward with TAA off/on without VUID or
synchronization hazards; the cut-open cavity exposes its inward-facing walls.
The reviewed Hello Wall and Tekla files still report complete imports.
Curved advanced faces now reuse elementary, explicit-knot spline and supported
swept surface evaluators. Independent volumes, shared edges, holes, unit scales,
explicit seams and surface-normal checks pass. The six-product curved gallery
renders smoothly in deferred/forward with TAA off/on; its toroidal opening and
spline curvature remain visible. Captures under `out/ifc-review/curved-brep/`
contain no VUID/synchronization hazards.
Periodic bands now accept different boundary seam locations and unequal cyclic
sampling without moving shared cap edges. Spherical pole vertex loops mesh
closed spheres and caps, including caps larger than a hemisphere, with smooth
pole normals. Rotated and unit-scaled spheres, inward cavities, paired edges
and independent analytic volumes pass the regression checks.
The six-product periodic/pole gallery renders in deferred/forward with TAA
off/on, all products complete and no VUID/synchronization hazards. Captures under
`out/ifc-review/periodic-poles/` show smooth poles and continuous shared seams.
Sectioned surfaces now support ordered tag splits/merges and multiway branches,
retaining every source corner and authored station, including zero-width
profile segments. Planar polygonal sharp joins
reuse shared half-angle miters, including branched cross sections. Tests check
sloped crowns, chord accuracy, analytic areas, metre/millimetre units, reversed
and rotated joins, exact edge uses, Euler count and one continuous boundary.
Unsafe miters, folded patches, intersecting directrices and reordered tag runs
reject atomically. A nine-product gallery containing four surfaces, four branch
pcurve overlays and a floor renders in deferred/forward with TAA off/on without
VUID/synchronization hazards. Captures are under `out/ifc-review/sectioned-gaps/`.
Derived and mirrored profiles now preserve placed/nested transforms in swept
surfaces and extruded solids, including inherited section tags and void loops.
Independent bounds, volumes, inward/outward normals, mirrored miters, inverses,
Boolean cuts and scaled circle/indexed-arc/spline chord checks pass.
The seven-product derived-profile gallery imports completely in deferred/forward
with TAA off/on without VUID/synchronization hazards. Inspecting the mirrored
sectioned miter exposed a forward culling mismatch; matching opaque lighting
pipelines now preserve visible back faces and reflected instances in both scene
and BIM draws. A separate opt-in pixel regression covers these culling routes
and hidden single-sided back faces in glTF and IFCX. Captures and reports are
under `out/ifc-review/derived-profiles/` and `out/ifc-review/forward-culling/`.
All 138 IFC importer/core cases and ten selected suites pass in the final
Visual Studio Release build; ten optional USD cases lack their sample assets.
Guide-curve transitions, missing/reordered tag runs, curved/nonplanar sharp
sectioned joins, unsupported swept profile families,
non-spherical singular charts and edge loops passing through spherical poles
remain outside coverage.
Hello Wall body surfaces and representative shared Tekla structural
bodies pass independent cross-format surface checks. The IFCX export contains
no reinforcing-bar meshes and is not a complete geometry oracle.
See [IFC coverage and limitations](ifc-import.md) for fixed-camera captures,
representation limits and verification. Further native IFC coverage is tracked
in [#67](https://github.com/semihguresci/VulkanSceneRenderer/issues/67).

### Fixed: Forward HDR environment background (#62)

Forward lighting now fills uncovered reverse-Z depth samples with the HDR
environment, using the same ray reconstruction, orientation, intensity and
finite-value handling as deferred lighting. The draw uses the existing camera,
lighting and environment descriptors and their declared graph reads. It runs
after opaque lighting and before transparent composition, with depth writes
disabled and a depth-equal-zero test at the selected MSAA sample count.

The native Visual Studio Release build and forward recorder/technique tests
pass. On 2026-10-05 the opt-in GPU regression also passed: uncovered sky pixels
match deferred exactly with TAA off/on and forward MSAA 4x. Isolated cutout and
glass captures have whole-image RGB MAE 0.00097 and 0.00117 respectively;
their P99 error is zero. Resize, camera motion and technique changes preserve
the sky, and static covered surfaces retain valid temporal motion. The existing
Cornell forward MSAA surface regression passes. Captures contain no VUID or
synchronization hazards. Sky pixels keep the existing current-frame temporal
fallback rather than contributing geometry motion.

Run `forward_sky_gpu_regression` with `CONTAINER_RUN_GPU_FORWARD_SKY=1`.
Its captures and measured results are under
`out/build/visual-studio/test_results/forward-sky/`. The original black-sky
capture is retained locally at `out/ray-query/pre-sky-forward.png`.

### Fixed: Sampled area-light penumbra bands (#63)

Multiple-origin layers previously uploaded zero source radius, leaving nearly
hard shadow edges that averaged to discrete levels. The raster path now divides
the emitter into equal-area domains and applies blocker-distance PCSS filtering
within them. It preserves emitter shape/aspect ratio, receiver-plane depth and
cube seams, and shares the helper across deferred, forward and transparency.
Fast, balanced and high modes use 16/32/64 filter taps per origin. Balanced is
the default; no extra atlas or visibility history allocation is required.

Independent segment/AABB visibility profiles cover rectangle/disk emitters,
size/height/aspect changes, transparent receivers and cube seams in both paths.
All 48 static records and 24 moving-light/blocker TAA comparisons pass. Balanced
penumbra MAE is 0.0092--0.0166, versus 0.0962--0.1268 for the fixed-origin mutation;
its plateau fraction is at most 0.05, versus up to 1.0 before filtering. High
quality has no detected plateaus. No VUID or synchronization hazards occur with
ray-query device features disabled. See [area shadows](area-shadows.md) for the
approximation limits, controls, sample/memory budgets and reproduction.

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
