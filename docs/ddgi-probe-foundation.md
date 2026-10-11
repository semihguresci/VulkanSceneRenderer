# Dynamic diffuse global illumination

The experimental runtime for [epic #42](https://github.com/semihguresci/VulkanSceneRenderer/issues/42)
adds up to four axis-aligned world-space irradiance/visibility volumes. It is disabled
by default. Use `--ddgi` in a current source build; existing releases do not
contain these shaders. The original implementation follows the
[DDGI paper](https://jcgt.org/published/0008/02/01/) and
[production extensions](https://jcgt.org/published/0010/02/01/), with no SDK
runtime dependency or copied SDK source.

## Runtime and lighting policy

`RaySceneBuild -> DdgiTrace -> DdgiUpdate -> Lighting` runs on the graphics
queue by default, alongside the existing ray-shadow passes. Optional asynchronous
probe updates use a compatible second queue while lighting reads completed history;
details and queue fallback are described below. GI shares their acceleration
structures and immutable surface metadata. Raster shadows can remain enabled
without allocating full-resolution ray-shadow history images solely for GI.
Vulkan-Hpp RAII owns descriptors, views, samplers, pipelines and layouts; VMA
owns image/buffer allocations. All passes use Vulkan 1.4 synchronization2.

Probe rays store incident scene-linear radiance before exposure/tone mapping.
The irradiance estimator is `4*pi/N * sum(L * max(NdotOmega,0))` for rotated
uniform-sphere Fibonacci rays. The atlas stores physical irradiance E;
Lambertian evaluation applies receiver albedo and `E/pi` exactly once. The
historical environment fallback has a different brightness convention, so
switching routes can change brightness even under a constant environment.

Valid opaque receivers replace diffuse environment lighting, the ambient floor
and approximate bounce terms with probe diffuse lighting in forward/deferred
passes, including clustered and per-light deferred paths. Specular IBL and
material/GTAO occlusion remain. Fully occluded valid probes return black GI
without restoring ambient lighting. Outside-volume, invalid or uninitialized
regions retain the existing lighting fallback.

Transparent, thin, unlit, displaced and declared transmission receivers use
the fallback. Authored alpha-mask cutouts and section/box clipping apply to
closest-hit traversal. Transparent triangle geometry is included for GI but
unsupported secondary transport conservatively supplies black radiance and a
finite occluder distance. An actually black transmission texel can act as an
opaque secondary reflector. Active height displacement disables the shared
ray scene; BIM semantic-color diagnostic modes use raster lighting. Native
points/polylines have no triangle AS representation. General glossy reflection,
caustics and mip footprints are outside this diffuse route; ray material textures
use base-mip sampling. The optional glass policies below add straight-through
thin-sheet or bounded refractive secondary paths; transmissive primary receivers
keep fallback.

Direct-hit lighting supports directional, punctual and rectangle/disk emitters,
emission, vertex colors, normal maps and diffuse energy attenuation from
supported material layers. Sun shadow queries include distant occluders.
At most 32 punctual and 32 area light samples are evaluated per hit; above that
count, stratified light-index sampling with count/sample weighting includes all
sources in expectation. Each area selection uses four emitter samples. Large
light counts therefore add convergence noise rather than silently omit later
lights. Previous-history lookup at secondary hits amortizes additional diffuse
bounces; there is no current-atlas feedback in a trace dispatch.

## Volume and controls

All coordinates and biases are world-space values in the right-handed +Y-up
basis. Origin is the first probe center. Coverage is inclusive through
`origin + spacing * (counts - 1)`, with X-fast indices. Fit all three parameters
to the intended receivers, including floors. CPU-uploaded actual rounded probe
centers preserve interpolation at large BIM coordinates. GPU lookup accepts a
1/1024-cell margin at the bounds and clamps interpolation to the volume. This
bounded coverage extension accommodates rasterized-depth reconstruction error,
so deferred rendering does not reject a boundary receiver accepted by the
forward path. Visibility still uses the original receiver position.

For a first setup, use **Probe volume and ray quality → Fit probe volume to
scene** in the DDGI panel, or `--ddgi-fit-scene` at startup. The fit uses current
world-space triangle vertices from glTF, BIM and USD, including instance
transforms. Hidden objects and BIM layer/semantic filters are respected;
frustum and draw-budget culling do not shrink the fitted volume. Diagnostic
gizmos and unused vertices are excluded. Curve/point-only scenes need manual
volume settings because they have no ray-query triangle geometry.

The fit chooses equal spacing on all axes, at least three probes per axis,
and a margin around the scene. `--ddgi-fit-probes N` limits the total probes
(default 2048, input minimum 27). Outside padding requires at least four probes
on each nonzero-length axis: a scene with extent on all axes needs at least 64
probes. An insufficient budget reports the required minimum. Fitting also
respects the configured probe payload
budget; a tighter budget produces coarser spacing. Trace distance covers the
fitted volume diagonal. Normal, view and visibility biases are respectively
6%, 2% and 10% of spacing, and diagnostic sphere radius is 12%. Ray quality,
frame budgets, pause, intensity, hysteresis and memory limits are retained.
The panel reports the resulting spacing and payload so large-site coverage
can be judged before enabling GI. Automatic coverage of an entire BIM site
can be too coarse for individual rooms; use a smaller manual volume where
needed. A 0.001 world-unit minimum cell (1 mm when units are metres) and
float-precision limits bound scene fitting;
microscopic transport remains outside the validated contract.

Fitting is explicit and happens once. It does not enable GI or keep following
scene changes; refit after moving or loading a model. Invalid live fits show
an error and retain the last working settings. Startup errors stop with an
actionable diagnostic. The CLI rejects combining scene fitting with explicit
volume, distance, bias or probe-radius options so those values are never
silently overwritten. Other DDGI quality options can be combined with fitting.

```powershell
.\VulkanSceneRenderer.exe --model models/validation/cornell_box_local_light.gltf `
  --ddgi --ddgi-fit-scene --ddgi-fit-probes 1024
```

Defaults are origin `(-1.5,-0.1,-1.5)`, spacing `(0.5,0.5,0.5)`, counts `(7,5,7)`,
128 rays/probe, 128 probes/frame, 16,384 rays/frame, max distance 20, hysteresis
0.95, normal/view/visibility biases 0.03/0.01/0.05 and intensity 1. Use `--help`
for every option and numeric range. Explicit invalid/NaN/infinite settings fail
validation, even when disabled. A zero frame budget pauses updates without
advancing the round-robin cursor; already-valid history remains readable.

Manual setup for the two-unit Cornell fixture (PowerShell):

```powershell
.\VulkanSceneRenderer.exe --model models/validation/cornell_box_local_light.gltf `
  --ddgi --ddgi-origin -1.05 -0.1 -1.05 --ddgi-spacing .35 .35 .35 `
  --ddgi-counts 7 7 7 --ddgi-rays 128 --ddgi-probes-per-frame 128 `
  --ddgi-ray-budget 16384 --environment-intensity 0 --directional-intensity 0
```

`--ddgi-intensity 0` retains probe coverage and suppresses approximate bounce,
which provides a direct-light-only comparison under the same geometry and
lighting route. Scale the volume, trace distance and biases with scene units.
Relocation movement detection uses cell-relative tolerances. Lookup rescales
spatial directions and Chebyshev ratios before squaring, and uses actual
coincident positions for the center exception. The ray-origin offset still
has a world-space floor, so microscopic scene scaling is not a validated
end-to-end transport contract.
Use detail volumes for finer room coverage within a coarse architectural/BIM volume.

## Saved scene presets and multiple volumes

The panel offers **Save scene GI preset** and **Load scene GI preset**. Presets
store accepted volume, tracing, quality, pause, enable, debug and memory settings;
they do not store GPU history. The default Windows folder is
`%LOCALAPPDATA%/VulkanSceneRenderer/ddgi`. Loading is explicit. Scene identity
includes the primary and auxiliary model paths and both import scales. A preset
for another scene or scale is rejected before replacing accepted settings.
Relative packaged assets resolve beside the executable when absent from the
working directory. Saves replace the previous file atomically.

CLI equivalents are `--ddgi-preset PATH` and `--ddgi-save-preset PATH`. Explicit
DDGI flags override loaded settings regardless of their command-line order.
Saving after `--ddgi-fit-scene` records the fitted volume rather than the startup
placeholder. Versioned JSON is bounded to 64 KiB and rejects invalid/unknown or
duplicate fields, unsupported versions and invalid combined resource budgets.

**Add detail volume** creates a centered half-spacing lattice inside the selected
volume. Select a volume, edit its origin/spacing/counts, then Apply; detail volumes
can be removed while the primary scene volume remains. All four volumes share
one packed atlas, one probe/ray frame budget and one memory ceiling. Scene fitting
replaces the primary lattice and preserves detail volumes within that ceiling.

The finest valid volume takes precedence. Its outer `volumeBlendCells` cells
smoothly hand off to coarser valid support; missing fine data falls back to coarse
data, while valid occluded black remains support instead of restoring sky fallback.
A sole covering volume retains full
response at its boundary. Normal/view/visibility biases scale with each lattice's
minimum cell spacing. Pad room volumes beyond receivers by at least the blending
band to avoid mixing coarse lighting at room boundaries. Dense probes improve
visibility resolution; they do not guarantee zero leakage for every thin/open
BIM topology.

## Thin-glass secondary transport

Enable **Transmit light through thin glass** or `--ddgi-thin-glass`; disable a
saved opt-in with `--no-ddgi-thin-glass`. Supported sheets explicitly declare
material transmission, zero thickness, roughness at most 0.1, zero metallic,
full opacity and no normal map, displacement, unlit or layered extensions.
Alpha blend alone never identifies glass. Colored transmission is attenuated by
tint, authored transmission and dielectric Fresnel on both faces; alpha=1
transmission materials remain transmissive. Rays retain their original distance
limit and traverse at most eight sheets. An opaque wall behind glass still
occludes, and exceeding the crossing cap conservatively returns black.

This policy applies to probe rays and their direct-light visibility. It does not
add refraction, volumetric absorption, caustics or transparent receiver GI. Thick,
rough or otherwise unsupported transmission remains a conservative occluder.

## Extended scene setup and secondary transport

All four additions below are explicit opt-ins. Scene presets now save schema
version 2, including `cameraFollow`, `asyncCompute` and `dielectricTransmission`.
Version 1 presets remain readable with those three additions disabled. Re-saving
upgrades the file to version 2. Missing, unknown, duplicate or invalid fields are
still rejected; the migration does not silently infer new settings. An accepted
preset stores configuration and fixed detail volumes, rather than GPU history or
the current runtime position of a scrolling primary grid.

### Fit detail volumes to selected geometry

Select a mesh node/subtree or BIM room/element, then choose **Fit detail volume
to selected room or element**. The operation appends a volume around current
transformed triangle bounds. A mesh selection includes its descendants; a BIM
selection includes all rendered fragments of the same product. Empty selections
and curve/point-only elements report an error without replacing the existing
volumes. The primary grid and existing details remain in place. This is a
one-shot placement operation; it does not identify rooms automatically throughout
an entire building or track later changes to the selected geometry.

The new volume uses equal cell spacing and pads receivers by at least one cell
and the configured blend band. Its probe budget and the shared payload ceiling
can produce coarser spacing. The operation retains ray quality, frame budgets and
bias controls, and increases trace distance when required by the padded grid.
It requires a free slot among the four total volumes and does not enable GI.
The placement planner also bounds the total lattice to 65,536 probes. If spacing
cannot resolve the selected world coordinates or padding cannot fit, it reports
the required budget/rebase adjustment.

At startup, use `--ddgi-detail-node NAME --ddgi-detail-probes N`; the default
detail budget is 512. A BIM GUID can identify an element when display names are
ambiguous. Node/element names must resolve uniquely across the loaded providers.
Although the CLI accepts budgets from 27, the actual padded volume requires more
probes, depending on its blend band. Startup detail fitting follows optional
whole-scene fitting and precedes `--ddgi-save-preset`, so saved settings include
the accepted detail volume.

```powershell
.\VulkanSceneRenderer.exe --model scene.gltf --ddgi --ddgi-fit-scene `
  --ddgi-detail-node "Room 101" --ddgi-detail-probes 512 `
  --ddgi-save-preset room-scene-gi.json
```

### Camera-follow scrolling

Enable **Scroll scene volume with camera** or `--ddgi-camera-follow`; disable
a saved opt-in with `--no-ddgi-camera-follow`. The primary scene grid moves on
whole-cell steps to keep its center near the camera. Counts and spacing remain
fixed. Detail volumes retain their world positions. Camera movement does not
invalidate all transported light or start a new content epoch.

History is remapped only where the previous and new CPU-rounded world probe
positions match exactly. Overlapping nodes preserve atlas/state history and
scheduling age; newly exposed cells start uninitialized and are prioritized for
refresh. A disjoint jump clears the primary grid's coverage. Pause retains the
overlapping history but leaves exposed cells unavailable until updates resume.
Scrolling does not move the scene or acceleration structure, and changing this
mode or editing the lattice can still retire work and recreate resources.
An unresolvable camera position retains the previous grid and reports
`camera-follow held`; scrolling resumes when the camera returns to resolvable
coordinates. Rebase large-coordinate scenes when their cell spacing loses float
precision.
Telemetry reports scroll count, preserved probes and the runtime primary origin
separately from the configured preset origin.

### Rough and solid dielectric transport

Enable **Refract rough and solid glass** or `--ddgi-dielectric`; disable a saved
opt-in with `--no-ddgi-dielectric`. Explicit material transmission, full opacity,
nonmetallic material and IOR in [1,3] are required. Roughness in [0,1] controls
sampled visible GGX interface normals. Normal-mapped, displaced, unlit, dispersive
or layered/specular-modified materials retain conservative blocking behavior.
Alpha blend alone does not identify a transmitting dielectric.

An authored positive `KHR_materials_volume.thicknessFactor` selects a volume
boundary. Probe rays refract at entry/exit using Snell's law and dielectric
Fresnel, apply the radiance IOR conversion, and preserve total internal reflection.
Absorption uses authored attenuation color/distance and actual traced distance in
world units. The thickness texture is ignored for ray transport, following the
[Khronos volume specification](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_volume).
A zero factor retains a thin surface: rough transmission scatters the continuing
direction without declaring an enclosed medium. The separate thin-glass opt-in
keeps its previous smooth, straight-through behavior when this mode is disabled.

The bounded traversal supports one closed homogeneous volume at a time, whose
entry/exit boundaries belong to the same ray instance. Nested/intersecting media,
open or malformed volumes, opaque terminal geometry and an exhausted eight-hit
interface/reflection budget stop conservatively. Rays preserve their original
probe direction and finite total world-distance budget; refracted visibility
moments use path distance. Curved-path backfaces do not provide evidence that
the original probe lies inside an opaque solid. Matching IORs leave direction
unchanged; zero attenuation channels remain black without producing NaNs.

This secondary path adds no transparent receiver GI, primary raster refraction,
scattering, dispersion, caustics or non-TIR reflected branch. Direct-light queries
retain a fixed connection to the selected emitter and conservatively block
rough/refractive glass; a refracted shadow direction would otherwise miss that
emitter. The interface sampling follows the
[pbrt dielectric model](https://www.pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF).

### Asynchronous probe updates and queue fallback

Enable **Asynchronous probe updates** or `--ddgi-async`; disable a saved opt-in
with `--no-ddgi-async`. Where supported, DDGI trace/update submissions run on a
second compute-capable queue from the graphics family. Timeline synchronization
orders shared acceleration/surface data, lighting snapshots and completed atlas
publication. Graphics lighting continues reading published history while a new
update is pending. Probe state is also buffered; incomplete results do not become
visible history.

Only one GI writer is pending at a time. Frame probe/ray ceilings are preserved,
and scheduler/history publication advances after successful submission and
completion. Scene/material changes, pause, disabling GI, resource edits and
scroll remaps synchronize outstanding work where required. This mode does not
promise faster rendering: overlapping compute and graphics compete for hardware
resources, and performance depends on the device and workload.

Devices without a compatible second queue or enabled timeline semaphore retain
serialized updates. The requested setting remains saved, while `asyncActive`
and the status message report the actual mode. Telemetry also separates
`asyncSubmittedUpdates` and `asyncPublishedUpdates`. No separate queue-family
ownership transfer is required because this route uses the graphics family;
dedicated queues from other families are outside the current implementation.

The extended fixture runner is `run_dielectric.py`; its matched active GI1/GI0
captures compare clear/rough/absorbing volumes with disabled, opaque-terminal,
malformed-open and nested-volume controls in both raster routes. GPU transport
contracts independently check Snell entry/exit, TIR, IOR matching, GGX sample
directions and finite absorption. These checks complement CPU placement, scroll,
preset migration and queue lifecycle tests. Run native/GPU workloads serially:

```powershell
python -B tests/visual-regression/ddgi/run_extensions.py
python -B tests/visual-regression/ddgi/run_dielectric.py --output out/ddgi-dielectric-validation
```

## Interactive controls and probe diagnostics

Open **Probe global illumination (DDGI)** in the scene controls to enable GI,
pause updates, adjust frame budgets or change intensity, trace distance,
hysteresis and lookup biases. Pause preserves both budgets and submitted
history. Geometry/light changes while paused still mark a fresh sweep pending;
resume updates it from the retained cursor. Trace distance and bias edits start
a new transport epoch; intensity, hysteresis and diagnostic edits preserve it.

The volume origin/spacing/counts and rays per probe use an Apply/Cancel draft.
Invalid edits leave the accepted configuration intact and show the failing
field. Lattice/ray-quality changes, capacity growth or enabling a field prepared
with disabled resources retire all submitted GPU work before rebuilding shared
resources. These changes can briefly stall the application. Reducing a frame
budget or pausing keeps allocations and lighting history. The panel shows the
current fallback reason, fresh-sweep progress, allocation bytes and completed
trace/update GPU times. Appearance/structural edits reset TAA where necessary.

Diagnostic spheres use actual relocated world-space probe centers and sampled
main-scene reverse-Z depth. They draw after tone mapping in both raster
techniques, before the GUI, with no effect on probe transport or TAA history.
CLI equivalents are `--ddgi-debug off|irradiance|visibility|classification|relocation`,
`--ddgi-probe-radius F` and `--ddgi-pause`.

| View | Display |
| --- | --- |
| Irradiance | Directional `E/pi`, mapped with `x/(1+x)` at a fixed diagnostic exposure |
| Visibility | Directional mean hit distance / max trace distance, blue to yellow |
| Classification | Gray uninitialized, green active/valid, red inactive, orange awaiting valid rays, magenta HDR clamp |
| Relocation | Movement / maximum diagonal relocation, blue to yellow; sphere centers include relocation |

Probe spheres occlude against opaque scene geometry. Overlapping spheres use
instance draw order rather than a separate probe depth buffer; use a small
radius to inspect dense lattices. Visibility shows the first moment, not a
complete variance or ray reference visualization. Transparent surfaces use the
main opaque depth behavior. Debug draw cost is included in PostProcess.

## Updates, visibility and lifetime

Two padded octahedral atlas histories store RGBA16F irradiance and RG32F first/
second distance moments. One seam texel surrounds every tile. Before selected
probe updates, the manager copies prior atlases so untouched tiles survive;
the update kernel computes interiors and folded borders without cross-thread
image hazards. Finite half-float overflow is clamped to 65,504 and flagged in
probe state. Maximum ray distance is finite for miss moments.

Probe state records classification, validity, update frame, relocation and
flags. Majority backface hits classify enclosing solids; relocation is bounded
to 0.45 cell spacing on each axis. A moved probe stays invalid until retraced.
Interpolation rejects inactive data and combines geometric/normal weights with
variance-based distance visibility. Thin walls, coarse spacing, too few rays,
insufficient trace distance and biased receiver placement can still cause
leaks or darkening; this is an approximate field, not a path-traced reference.

Scheduling emits complete probe ray sets within both shared budgets. For P probes and
U updates/frame, the minimum complete-sweep time is `ceil(P/U)` submitted frames (defaults:
245/128 = two without priority reordering). At least half the nonzero update
slots advance a persistent round-robin sweep; remaining slots favor changed
geometry/light influence, then probes in front of the camera and older samples.
Worst-case coverage is `ceil(P/ceil(U/2))` submitted frames. Geometry priorities
use old and new transformed triangle bounds expanded by trace distance, and
local lights use both previous and current influence bounds. Probe selections
are unique and may be non-contiguous. Relocated probes need later sweeps. Hysteresis and previous-field
multi-bounce feedback add convergence time. Scene/provider/material revisions,
transforms, semantic visibility/clipping, light data and environment changes
start a new content epoch. The cursor and submitted atlases survive; refreshed
probes replace old-epoch samples without hysteresis, while untouched probes
retain bounded stale lighting until their turn. Previous-field bounce feedback
pauses until a complete fresh sweep finishes. Continuous motion therefore
cannot repeatedly restart at probe zero and starve later probes.

Camera movement alone preserves GI. A content refresh does not force a TAA
history reset; existing structural, camera and lighting reset rules still apply.
Cursor, fresh-sweep progress and atlas publication occur only after a successful
submission.

Per-image uniform/update buffers and descriptor sets are written after that
image's fence retires. Global histories, state and ray scratch are serialized
with explicit transfer/compute/fragment memory dependencies across frames.
Resize preparation requires all frame fences retired before replacing resources.
The two atlas copies are not independent storage for concurrent queues.

## Budgets and fallback

`--ddgi-memory-mb` defaults to 64 MiB and covers probe resources. The CPU planner
counts atlas padding and both histories, state (32 bytes/probe), uploaded base
positions (16 bytes/probe), bounded ray scratch (32 bytes/ray) and update records
(16 bytes/update). Actual VMA allocations, including per-image uploads, are
checked too; a resource/device limit failure selects initialized raster fallback
resources. Planning excludes descriptor/driver metadata and shared AS/material
allocations.

`--ddgi-surface-memory-mb` defaults to 128 MiB per compact optional surface
payload: 192 bytes/triangle plus 112 bytes/instance, beyond the existing
48-byte UV triangle and 16-byte shadow instance metadata. Limits are checked
before extracting/uploading optional surfaces. A rejected GI payload preserves
compact shadow metadata. Invalid optional attributes or an unrepresentable
normal transform also disable GI with a reported reason while preserving the
compact shadow scene. The manager remembers the failed scene revision and
retries after geometry, topology, transforms or material revisions change.
Invalid required geometry still fails compact extraction.
Old AS/surface generations remain fence-retained;
multiple live generations can exceed that per-generation payload cap and are
included in shared-ray allocation telemetry. Providers must advance geometry
revisions for changed UVs, colors, normals and tangents as well as positions.

Unsupported ray queries, atlas formats, storage features or descriptor limits
keep raster lighting valid. Separate descriptor-free lighting shader variants
preserve devices with only four descriptor sets or the baseline sampler limits.
`--no-ray-query --ddgi` exercises the capability fallback.

Screenshot/sequence telemetry includes `ddgi`: support/active state, reason,
actual probe allocation bytes, refresh count (`historyResets`), content epoch,
probes remaining in the fresh sweep, update frame, scheduled probe/ray counts
and the scheduled batch indices. A paused batch has zero first/next indices;
`schedulingCursor` separately reports the retained next probe after submission,
including pause/disable frames, and is visible in the controls. Capture events
`ddgiPaused` and `ddgiEnabled` accept booleans and use the live manager settings.
`ddgiFitProbes` accepts an integer budget and queues one scene fit at its named
frame, through the same path as the UI button. `ddgi.sceneFit` records the
source world bounds, requested budget, fitted probe count, exact CPU payload
and status. Its `applied` flag means the last successful fitted lattice still
matches the current volume; manual volume edits clear that match. The stored
bounds describe that fit, so refit to update them after subsequent scene motion.
`gpuProfiler` reports timestamp availability, source and result latency.
Existing graph telemetry times `DdgiTrace`, `DdgiUpdate` and
`Lighting`; shared AS memory/build times are reported with ray-scene telemetry.
Lookup cost is included in Lighting, not an independently measured pass.

## Validation and remaining epic work

Build through the Visual Studio developer environment:

```powershell
cmake -S . -B out/build/visual-studio
cmake --build out/build/visual-studio --config Release --parallel 4 --target VulkanSceneRenderer ddgi_volume_fit_tests ddgi_scene_bounds_tests ddgi_probe_volume_tests ddgi_settings_tests ray_scene_tests ray_shadow_surface_gpu_tests ddgi_gpu_tests ddgi_manager_gpu_tests ddgi_lookup_gpu_tests technique_debug_model_tests render_graph_tests temporal_capture_tests deferred_directional_lighting_recorder_tests forward_raster_lighting_pass_recorder_tests
ctest --test-dir out/build/visual-studio -C Release -R '^(ddgi_volume_fit_tests|ddgi_scene_bounds_tests|ddgi_probe_volume_tests|ddgi_settings_tests|technique_debug_model_tests|ray_scene_tests|render_graph_tests|temporal_capture_tests|deferred_directional_lighting_recorder_tests|forward_raster_lighting_pass_recorder_tests)$' --output-on-failure
$env:CONTAINER_RUN_GPU_DDGI='1'
ctest --test-dir out/build/visual-studio -C Release -R '^(ddgi_gpu_tests|ddgi_manager_gpu_tests|ddgi_lookup_gpu_tests)$' --output-on-failure
ctest --test-dir out/build/visual-studio -C Release -R '^ddgi_lookup_spirv_validation$' --output-on-failure
$env:CONTAINER_RUN_GPU_RAY_QUERY='1'
ctest --test-dir out/build/visual-studio -C Release -R '^ray_shadow_surface_gpu_tests$' --output-on-failure
```

The opt-in GPU tests execute the production update SPIR-V, testing E=pi*Li,
distance moments, seam borders, untouched probes, hysteresis, invalid/backface/
unsupported classification, paused updates and content-epoch replacement
without disturbing untouched probes under synchronization validation.
Partial invalid ray sets retain the generated-direction denominator; finite
HDR overflow saturates the half-float atlas and publishes its clamp flag.
CPU tests cover indexing/rounded interpolation, bounded scheduling, overflow,
settings validation, optional surface reconstruction and cache generations.
Runtime scene captures must additionally verify transport and graph integration.
The ray-shadow GPU suite covers optional attribute/normal-transform rejection,
stable compact fallback across frame slots, recovery after repairs and required
geometry failures in raster, hard and soft shadow modes.
The manager GPU suite submits the production trace/update kernels, then checks
live enable/disable, pause/resume, transport refreshes, rejected edits and safe
resource growth with an outstanding submission. CPU GUI cases verify accepted
settings and rejection recovery across frame synchronization.
The isolated lookup GPU suite includes the production lookup shader and checks
constant physical irradiance, facing and moment visibility at metre and 1e-8
scales, valid occluded black, visible/blocked probe mixtures, and missing-data
fallback. This tests lookup arithmetic independently of the ray-origin floor.

Run the native regression matrix after building:

```powershell
python tests/visual-regression/ddgi/run_runtime.py
python tests/visual-regression/ddgi/run_runtime.py --surface-validation-only --output out/ddgi-surface-validation
python tests/visual-regression/ddgi/run_runtime.py --debug-only --output out/ddgi-debug-validation
python tests/visual-regression/ddgi/run_material_quality.py --output out/ddgi-material-validation
python tests/visual-regression/ddgi/run_volume_fit.py --output out/ddgi-volume-fit-validation
```

It captures forward/deferred color bleeding and exact-boundary receivers,
camera/light/object changes, continuous motion, paused and one-probe scheduling,
capability fallback and optional-surface-budget rejection with ray shadows.
The recorded Windows/NVIDIA results are in
[tests/visual-regression/ddgi/windows-nvidia](../tests/visual-regression/ddgi/windows-nvidia).
The October 9, 2026 RTX 2080 SUPER run passed 130 CPU tests, seven production
GPU tests and eleven native cases with no validation errors. Its 343-probe,
128-update volume allocated about 2.9 MiB and swept in three submitted frames;
see the recorded results for commands, hashes, images and measurement limits.
The subsequent surface-validation fix passed 19 CPU cases, 13 ray-shadow GPU
cases and two native fallback cases without validation errors. Its separate
records preserve the original capture snapshot.

## Architectural quality and repeated timing measurements

The follow-up matrix generates five metre-scale, opaque architectural scenes:
a doorway, 3 cm and 24 cm sealed partitions, an open control, and a neutral-wall
control. Normal/fine/coarse grids have 637/4,225/225 probes at 0.5/0.25/0.75 m
spacing. Their 240/1,592/97 submitted warmup frames match average scheduled
visits per probe. All use fixed manual exposure 0.04, a white rectangle source,
and zero environment/sun intensity. Paired primary intensities 1/0 retain
identical probe coverage; disabling DDGI entirely restores approximate bounce
lighting and cannot isolate its physical contribution.

All 15 native quality cases passed without validation/runtime errors. The
doorway's indirect-only back-wall receiver measured luminance 0.0596, versus
0.0758 in an independent CPU Lambertian reference. Lit-room indirect red/green
was 1.436 versus 1.0 for the neutral control. The normal and fine grids kept the
sampled sealed-room receivers black in both partition widths; the forward
normal-grid capture agreed with deferred. The coarse grid leaked luminance
0.0321 through the 3 cm partition, 8.24% of the open-room control. Its matched
zero-intensity control was black, so this is actual probe transport leakage.
The regression's 1% sealed/open limit applies to the normal/fine fixtures;
coarse leakage is recorded as a limitation rather than a passing leak bound.

The camera/light/partition stress sequence verifies exact content epochs and
AS generations, continued round-robin progress, visible receiver response and
completed fresh sweeps after motion stops. The 637-probe/256-update field
sweeps in three submitted frames; hysteresis and amortized secondary bounces
still need more frames to converge. These bounds describe visibility refresh,
not immediate radiometric convergence or a measured wall-clock latency.

References intersect analytic quads and integrate the rectangle source
independently of the probe shaders. Point measurements use 16,384 seeded
paths and eight scattering vertices; full reference images use 192x128,
128 paths/pixel and the same camera/exposure. Saved linear arrays and noise
estimates accompany the PNGs. PBR/Fresnel differences, finite bounce depth,
Monte Carlo noise, and point-versus-screen-patch averaging prevent pixel-golden
equivalence. PNG-derived native radiance additionally carries 8-bit/ACES
inversion error. Single-sided opaque walls are the validated fixture policy;
double-sided solids and other BIM material policies need scene-specific checks.

Repeated Cornell measurements use 1024x768, three launches per mode/technique,
120 warmup frames, six settling frames and 90 GPU samples per launch (2,700
measured samples). Validation is disabled for timing; the separate quality
matrix enables synchronization validation. A fresh 30-launch run after the
lookup scale fix passed all checks. RTX 2080 SUPER pooled pass medians:

| Updated probes/frame | Sweep frames | Probe allocation MiB | Trace ms, deferred/forward | Update ms, deferred/forward |
| --- | --- | --- | --- | --- |
| 16 | 22 | 2.46 | 0.0831 / 0.0832 | 0.1738 / 0.1736 |
| 128 | 3 | 2.90 | 0.0840 / 0.0848 | 0.1858 / 0.1857 |
| 343 | 1 | 3.75 | 0.1301 / 0.1326 | 0.2478 / 0.2479 |

The warmed, paused 128-update allocation remains 2.90 MiB. The median of three
matched paused-minus-off Lighting medians is +0.589 ms deferred and +0.551 ms
forward. This estimates the complete lighting-route change, including replaced
fallback terms, rather than an isolated lookup instruction cost. Readback/PNG
encoding serializes the capture workload and changes GPU duty cycle; these
numbers do not establish interactive frame rate. Timestamp results lag counters
by one completed frame; settling isolates the pause transition. Saved records
include per-frame times, median/p95 distributions, repeat order, memory,
commands, hashes, and measurement limits.
The readable saved performance summary is accompanied by a gzip archive of
all raw captured frames and timestamps. The separate `lookup-material-*`
evidence records preserve this latest snapshot; earlier measurements remain
unchanged in their original files. Paused measurements now assert the actual
retained cursor directly, independently of timestamp result latency.

```powershell
python tests/visual-regression/ddgi/run_quality.py
python tests/visual-regression/ddgi/render_reference.py --output out/ddgi-quality-cpu-images --samples 128
python tests/visual-regression/ddgi/run_performance.py
python -m unittest discover -s tests/visual-regression/ddgi -p 'test_*.py'
```

## Material policy and live lifecycle validation

The final material matrix passed twelve native launches on the RTX 2080 SUPER
with synchronization validation and no validation/runtime errors. Seven static
fixtures compare matched single-/double-sided closed partitions, zero-thickness
sheets and doorways against an open control. Two additional sheet captures
retain the warmed probe field and set only primary indirect intensity to zero.
The same snapshot passed 112 focused CPU cases, 56 Python harness checks and
21 Vulkan GPU cases: eight update, seven manager and six isolated lookup tests.
Twenty runtime SPIR-V modules and the lookup contract also passed Vulkan 1.4
validation. The isolated lookup tests cover metre and 1e-8 world-unit scales;
they do not establish microscopic ray transport support.

For the single +X-facing sheet with the source in the -X room, the shaded back
receiver measured total luminance 0.1689122, or 43.2738% of the open control.
Its zero-primary-indirect control remained at 0.1605250, or 41.1250%. The matched
primary DDGI difference is 0.00838716, or 2.1487% of the open control. Most of the
response therefore persists in the direct-light route: from the source the
sheet is a back face culled by the one-sided raster shadow pass; the reciprocal
receiver-to-light query encounters a front face and applies the matching
front-face cull. This follows the existing one-sided shadow convention. An
opaque sheet that must block light from both sides should use `doubleSided` or
closed solid thickness. The double-sided sheet and both closed-partition
fixtures kept the sampled shaded receivers black. These are measured material
diagnostics, not a zero-leak guarantee for every sheet or double-sided solid.

Double-sided hits do not provide the enclosing-backface evidence used to
classify or relocate probes out of solids. Open sheets have no well-defined
enclosed interior, so changing classification from the material flag alone
would introduce false inside classifications. These fixtures document that
policy on one device; complex BIM geometry and material policies still need
scene-specific checks. PNG-derived luminance carries quantization and tone-map
inversion error. A zero-primary control can retain legacy ambient terms where
probe coverage is unavailable; its signed matched difference isolates the
primary probe contribution.

Forward and deferred each passed a 24-frame lifecycle sequence that starts with
disabled fallback resources, enables the field, pauses/resumes, disables it and
enables it again. Sixteen submitted update frames retained the round-robin
cursor at probe 274 and 5,353,008 bytes of probe allocations after initial
enable. Pause and disable retained history, update counters and scheduling;
surface scene reconstruction on enable may advance the content epoch. A
separate `--no-ray-query` launch retained the requested settings, reported the
capability fallback and rendered with active GI/ray shadows disabled. The
material results record commands, executable and runtime shader hashes,
receiver estimates and every lifecycle frame.

The Windows package copies the compiled shader directory and now explicitly
requires the trace/update/debug modules and all descriptor-free fallback
variants. It also includes this guide. Packaging and publication follow the
existing committed-source release workflow; the evidence records validated
source snapshots and executable/shader hashes. Additional hardware and complex materials still
need scene-specific validation; the extended queue mode is described above.

## Scene-fit validation

The October 10 follow-up adds one-shot scene fitting and passes the Visual
Studio Release build, 98 focused C++ CPU cases, 69 Python cases and seven
manager GPU cases. Twelve native captures cover three import scales in both
raster techniques, equivalent glTF/BIM/USD geometry, disabled GI, capability
fallback and a refit in the exact frame that an architectural partition moves.
Eleven invalid CLI combinations reject before Vulkan initialization. The
minimum 64-probe Cornell fit loads under a 1 MiB cap with 462,848 bytes of
planned payload and 466,624 bytes of actual allocations on this device.

Tests cover current transformed bounds, inclusive padded coverage, scaled
biases, count/memory limits, float-resolvable BIM coordinates and the
nonmonotonic padded diagonal near the trace-distance limit. Native capture
events accept exact integral JSON numbers as the capture schema specifies.
The scene-fit records in the Windows/NVIDIA evidence directory preserve all
earlier snapshots. This follow-up verifies setup and history/resource behavior;
it adds no lighting-quality or timing measurements.

## Preset, priority and room-volume validation

The October 11 follow-up passes the Visual Studio Release build, 155 focused
C++ CPU cases, 31 Vulkan GPU cases and 99 Python harness cases. The GPU cases
were explicitly enabled and none skipped. Twenty-two runtime lighting/debug
SPIR-V modules and both DDGI contract modules also pass Vulkan 1.4 validation.
Preset coverage includes concurrent saves, transient Windows reader locks,
bounded failure that preserves the old file, scene/scale identity, malformed
input and CLI override order. Volume and scheduler tests cover packed offsets,
shared budgets, overlapping support, valid black, dirty old/new geometry bounds,
camera priority, fairness under continuous changes and transactional submission.

Native checks use the RTX 2080 SUPER with Vulkan synchronization validation:

| Runner | Rendered captures | Input rejections | Coverage |
| --- | --- | --- | --- |
| `run_features.py` | 11 | 5 | Fitted preset save/load, overrides in both orders and raster routes, packed debug, moving geometry, pause/disable/capability fallback |
| `run_glass.py` | 24 | 0 | Matched primary-GI-zero controls, opt-in/opt-out glass, opaque terminal wall, crossing cap, unsupported thick glass; both raster routes |
| `run_volume_quality.py` | 8 | 0 | Coarse versus padded fine room volume, sealed 3 cm partition and matched open controls; both raster routes |
| `run_volume_fit.py` | 12 | 11 | Existing glTF/BIM/USD, import-scale, moving/refitted geometry and fallback regressions |

For the closed thin partition, the coarse grid's sampled shaded receiver was
8.22% of the open control in deferred and 8.19% in forward. Adding the padded
fine room volume reduced that patch to black in the quantized captures while
retaining 90.9% of the lit-side response in each route. The two grids contain
3,600 probes and share a 256-probe / 32,768-ray frame budget. These results
demonstrate a measured improvement for a closed single-sided solid; they do
not establish zero leakage for arbitrary open or double-sided BIM geometry.

The supported clear pane's isolated primary indirect response was 94.4% of
the open control in deferred and 94.8% in forward. Disabling thin-glass
transport reduced it to about 0.11%. An opaque terminal wall remained black.
The estimates subtract matched zero-primary-indirect captures and carry
PNG quantization and tone-map inversion error. No timing claim is made.

Fresh JSON, PNG, commands, logs and executable hashes are written beneath
`out/ddgi-features-validation` and `out/ddgi-glass-validation`; archived evidence
under `tests/visual-regression/ddgi/windows-nvidia` is unchanged. The glass
record also stores runtime shader hashes. Its lighting shaders remained
identical through the final preset-save fix and executable rebuild.

```powershell
# GPU test suites require an explicit opt-in; run serially on the rendering GPU.
$env:CONTAINER_RUN_GPU_DDGI = '1'
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -j 1 `
  -R '^(ddgi_.*tests|ddgi_python_regressions|ddgi_lookup_spirv_validation|technique_debug_model_tests|temporal_capture_tests)$'
python -B tests/visual-regression/ddgi/run_features.py
python -B tests/visual-regression/ddgi/run_glass.py
python -B tests/visual-regression/ddgi/run_volume_quality.py
python -B tests/visual-regression/ddgi/run_volume_fit.py --output out/ddgi-features-validation/fit-regression
```

Python image checks use NumPy and Pillow. Native runners require the built
executable and a ray-query-capable GPU; run them sequentially. Unit/harness
checks can also run independently with `python -B -m unittest discover -s
tests/visual-regression/ddgi -p 'test_*.py'`.

## Extended-feature validation

The October 11 extended-feature snapshot passes the Visual Studio Release build,
205 focused C++ CPU cases, 42 Vulkan GPU cases and 121 Python harness cases.
All GPU cases executed, with no skips. Twenty-two runtime lighting/debug modules
and both DDGI contract modules pass Vulkan 1.4 SPIR-V validation. Validation uses
the RTX 2080 SUPER and Vulkan synchronization validation; it establishes behavior
on this device without making a performance claim.

The manager tests hold an async compute submission behind a host-controlled
timeline while a subsequent graphics submission completes. They also read back
actual irradiance and probe-state history across a paused scroll, test publication
and pause/resume, retain static detail volumes, and recover from unresolvable
camera coordinates and invalid light snapshot counts.

| Runner | Rendered cases | Input rejections | Coverage |
| --- | --- | --- | --- |
| `run_extensions.py` | 12 | 3 | Tight glTF/BIM/USD selection fitting, cell scrolling, async publication, pause/resume and moving geometry |
| `run_dielectric.py` | 32 | 0 | Matched clear/rough/absorbing solid glass, disabled transport and terminal/unsupported volume controls |
| `run_features.py` | 11 | 5 | Existing presets, override order, packed volumes, geometry motion and fallback |
| `run_glass.py` | 24 | 0 | Existing smooth thin-glass behavior and crossing limits |
| `run_volume_quality.py` | 8 | 0 | Existing coarse/fine room-volume lighting comparisons |
| `run_volume_fit.py` | 12 | 11 | Existing glTF/BIM/USD fitting, import scales, refit and fallback |

Each rendered case is a renderer launch; sequence cases save multiple frame
captures. In both raster routes, the camera-follow fixture moves exactly one
cell at frame 35, retains 728 of 819 probes, and finishes refreshing exposed
cells without changing its 4,915,776-byte allocation or content epoch. The async
route remains active and publishes completed updates after pause/resume. Selected
detail fits preserve the primary lattice and tightly pad the selected triangles.

The solid clear-glass receiver's isolated primary indirect luminance is
0.07991/0.08071 in deferred/forward, versus 0.22153/0.22274 for the open control.
Rough glass produces 0.06444/0.06482. The absorbing fixture selectively reduces
red while retaining blue; disabled, opaque-terminal, malformed and nested-volume
controls pass the bounded leakage thresholds. These values subtract matched
GI1/GI0 captures and carry PNG quantization and tone-map inversion error.
The existing coarse/fine room comparisons retain their earlier measured results.

Commands, logs, PNGs, telemetry, executable/shader hashes and the aggregate report
are saved beneath `out/ddgi-extensions-validation`. Earlier evidence under
`tests/visual-regression/ddgi/windows-nvidia` is preserved. These are source-build
validation records; published binaries follow the separate release workflow.

```powershell
$env:CONTAINER_RUN_GPU_DDGI = '1'
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -j 1 `
  -R '^(ddgi_.*tests|ddgi_python_regressions|ddgi_.*spirv_validation|technique_debug_model_tests|temporal_capture_tests)$'
python -B tests/visual-regression/ddgi/run_extensions.py
python -B tests/visual-regression/ddgi/run_dielectric.py --output out/ddgi-extensions-validation/dielectric
```
