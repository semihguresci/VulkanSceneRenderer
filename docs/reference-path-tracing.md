# Progressive reference path tracing (#47)

Status: **the progressive Vulkan reference backend is connected to the application
viewport and existing Rendering Engine selector**. It renders a supported surface
subset; full #47 material and scene acceptance remains in progress. The UI adds
one sample/diagnostic status line. No external dependencies were added.

## Application use and presentation

Choose **Rendering Engine → Path tracing** in Scene Controls. Vulkan inline ray
queries, acceleration structures, device addresses and bindless descriptors must
be enabled. This compute implementation requires neither a ray-tracing pipeline
nor floating-point image atomics. Unavailable ray queries select deferred raster;
resource-allocation failure also selects the complete deferred graph with a reason.

For deterministic captures, use `--render-technique path-tracing --path-spp 4
--path-samples 512 --path-bounces 8 --path-seed 1`. Samples per frame are bounded
to 1–64, the sample limit to 1–16,777,216, and bounce levels to 1–32. The defaults
are 1, 65,536, 8 and seed 1. Capture sequences can switch to `path-tracing`.

`--path-target-ms 8` enables adaptive batching against the previous completed
reference Lighting pass on the GPU. The budget accepts 0.1–1000 milliseconds.
`--path-spp` supplies the first batch after an accumulation reset and the
fallback when a GPU timing is unavailable. Each adjustment stays in 1–64 samples
and changes by at most a factor of two. The budget covers the reference pass. Presentation, other passes
and one-sample cost can limit the overall frame rate. Sample addresses and the
exact accumulation limit stay deterministic. Telemetry records the scheduled
batch, submitted sample count and timing used for the adjustment. Omit
`--path-target-ms` when exporting a fixed `--path-sample-series`.

Open **Reference camera lens** to set the perspective aperture radius and focal
plane distance in scene units. The same controls are `--path-aperture` and
`--path-focus-distance` for captures. The focal plane is perpendicular to the
camera axis, and the active perspective clip planes still apply. Zero aperture
uses the original pinhole ray. Orthographic cameras keep their original ray
generation. Effective lens changes restart accumulation. Reference capture
metadata records the aperture radius and reports a null focus distance when the
lens is inactive.

**Autofocus at viewport center** measures the nearest visible mesh or analytic
ground surface along one pinhole ray. The probe honors camera and scene clipping, alpha masks,
and a fixed 50% threshold for blended opacity. The next retired-frame
result updates the perspective focal plane if the camera, geometry, and
manual focus are still current. A miss leaves focus in place. A capture
sequence can request the same action with `"referenceAutofocus": true`.

`--path-firefly-clamp 4` or **Firefly clamp** limits each completed sample
to a peak RGB value of four. It scales all channels equally before updating
the mean and moments. The default of zero retains unclamped transport.
Changing the clamp resets accumulation. EXR metadata records the effective
limit, since a positive limit biases the accumulated result.

### Analytic ground and shadow matte

**Reference ground** enables a one-sided, infinite Y-up plane at an editable
world height. Surface mode shades it with an editable linear base color,
metallic factor, and roughness. It participates in primary, bounced, and
visibility rays and honors scene and camera clipping. No mesh is added to
the scene. The equivalent flags are `--path-ground surface`,
`--path-ground-height`, `--path-ground-color`, `--path-ground-metallic`, and
`--path-ground-roughness`.

`--path-ground shadow-matte` exports a grayscale direct-light shadow mask.
The tracer accumulates blocked and unoccluded direct irradiance separately,
then resolves their ratio weighted by visible plane coverage. Visible
foreground geometry and rays missing the plane contribute zero coverage.
The matte includes the environment, punctual, analytic area, and emissive
mesh proposals and follows the tracer's existing transparency and visibility
limits. It is a compositing mask for direct shadows. Use an EXR export for
the linear 0–1 values. The firefly clamp applies to radiance captures only.
The sidecar identifies the output as
`path-trace-shadow-matte` with color space `linear-mask`. Ground mode and
effective placement or material changes reset accumulation.

The technique shares the forward graph's depth, picking, editor overlays and HDR
presentation. Surface generations are requested independently of DDGI, including
transparent candidates so unsupported materials are diagnosed rather than omitted.
The reference Lighting callback runs after the shared scene build. Its RGBA32F
display image is published to runtime resource bindings for exposure and bloom;
the native raster attachment remains owned by the frame-resource manager.
Explicit barriers transition display GENERAL → SHADER_READ_ONLY → GENERAL around
presentation. TAA jitter and resolve, DDGI, ray-shadow filtering and raster OIT
are disabled for reference transport. Exposure, bloom and tone mapping remain
display operations and do not invalidate the accumulated means.

EXR export reads the raw RGBA32F mean directly through the existing linear-capture
storage buffer. It does not round through the raster RGBA16F color attachment.
Capture metadata includes attempted samples, history epoch and diagnostic counts;
PNG uses the separate diagnostic display image. Attempts are not a guarantee that
every pixel accepted that many samples. Magenta marks unsupported paths and yellow
marks numerical rejection; these colors never enter raw mean or M2 storage.

Separate inverse view/projection matrices come from the active navigation or
authored glTF camera. The current application viewport is the complete framebuffer
at origin zero, with negative height. Resizing recreates reference resources after
reader retirement. Camera, clipping, scene/material/texture, lighting, environment
and pipeline changes reset history; display exposure and tone mapping do not.
Switching away and back explicitly invalidates history. Submission advances sample
addresses; recording alone does not. Per-image diagnostic epochs prevent retired
slots from adding counts from an older history.

The environment manager retains the unfiltered radiance cube alongside its IBL
filters and retires graphics readers before replacement. The cube uses the existing
environment bake precision; extreme HDR environment precision remains a limitation.
An explicit background-only shader variant handles empty scenes without statically
accessing acceleration-structure descriptors. A nonempty scene whose surface
generation is unavailable produces an unsupported diagnostic, not an unobstructed
background. Semantic BIM coloring, volume transport, atmosphere/solar transport and
the material capabilities listed below are reported as unsupported in this mode.

Two viewport-scale GPU regressions also cover floating-point boundaries. Shared
closest-hit color interpolation uses affine differences and its vertex-value hull,
preserving constant white/alpha exactly instead of rounding white above one.
The alpha-coverage helper uses the same constant-preserving form. Primary subpixel
coordinates are kept inside their owning pixel when float32 addition would round
onto the next row or column. Neither change clamps path radiance. A 1280×800
point-lit fixture retains all 13 attempted samples at every pixel and matches an
independently authored constant-alpha-mask representation exactly, including at
the cutoff. The graph's readiness policy also prevents shared UI enable flags from
reactivating raster transport, DDGI or temporal filtering in reference mode.

## Transport building blocks

`shaders/path_transport_common.slang` is an engine-owned Slang include, kept
separate from raster BRDF approximations, LTC, DDGI feedback and display curves.
It provides:

- Diffuse plus isotropic GGX reflection with Schlick Fresnel, height-correlated
  Smith masking and a complete diffuse/specular mixture PDF. Perceptual
  roughness is squared to obtain alpha; exact zero roughness is a discrete mirror
  event. Positive roughness has no implicit floor.
- GGX visible-normal sampling. Below-surface outcomes retain their null
  probability rather than being resampled. Invalid/unrepresentable numerical
  inputs have a separate rejection flag; callers must report them rather than
  quietly counting them as converged black samples.
- A stable power-heuristic MIS weight and throughput-compensated Russian
  roulette with an explicit minimum depth. Roulette changes path survival, not
  physical radiance or material parameters.
- Counter-addressed random values keyed by pixel, sample index, bounce,
  dimension and seed. GPU readbacks verify identical sequences after dispatch
  reordering and batch-size changes. This is not a cross-vendor bit-exact
  rendering guarantee.
- Scene-linear float32 Welford mean and second central moment, capped at
  16,777,216 samples so integer sample counts remain exactly representable.
  Initialization ignores old storage. Invalid samples, moment overflow and the
  sample limit reject without mutating accepted state. No exposure, denoising
  or firefly/radiance clamp is applied.

BSDF directions point away from the surface in a right-handed orthonormal
frame with normal +Z. The caller must supply evaluated material parameters and
orient a permitted double-sided surface before sampling. This library does
not yet resolve glTF textures, layered materials, alpha, transmission, emission
or material capability diagnostics.

## Camera rays

The camera helper consumes separate column-major inverse projection and inverse
view matrices. It maps viewport-local subpixel positions to NDC using the
negative-height Vulkan viewport and reverse-Z near/far values 1/0. Subpixel
sampling is applied once, without an extra half-pixel or projection Y flip.
Perspective rays originate at the camera and include the near/far distance
interval. Orthographic rays originate on the near plane. Infinite-far
perspective projection uses the largest finite float as its maximum distance.

Direction construction occurs in view space before rotation into the world;
it avoids subtracting almost identical large world positions. GPU fixtures
verify perspective, orthographic, camera roll, finite/infinite far planes,
near/far clipping intervals and world translation up to 300 million units.
Invalid matrices, zero extents and out-of-viewport coordinates reject before
unsafe homogeneous division. See `coordinate-conventions.md`.

## Transport-core checkpoint

Build `path_transport_gpu_tests`, set `CONTAINER_RUN_GPU_PATH_TRANSPORT=1`, and
run its CTest executable. The test executes the actual Slang include on Vulkan
with synchronization validation and CPU readback. It requires a Vulkan compute
device; it does not exercise acceleration structures or scene traversal yet.

Twenty cases compare BRDF/PDF values and sample throughput with independent
double-precision formulas, verify reciprocity and probability measures, and
exercise invalid input, exact delta reflection, small positive roughness,
roulette, unclamped HDR moments, random sequences and camera conventions.
The unit-environment rough-white-metal fixture at roughness one measures
**0.306951882** over 131,072 samples, against analytic **1 - ln(2) = 0.306852819**.
Separate paired environment/BSDF MIS estimates match analytic Lambert/metal
results and numerical mixed-BSDF quadrature within measured sampling tolerances.

Sixteen independent seeds produce the following observed Lambert-environment
MIS RMS errors; these are estimator measurements, not rendered-image quality:

| Samples per seed | RMS error |
| ---: | ---: |
| 64 | 0.018990539 |
| 256 | 0.010170139 |
| 1,024 | 0.005681992 |
| 4,096 | 0.002178011 |
| 16,384 | 0.001594961 |

The earlier transport-core checkpoint compiled all 103 existing engine shaders and passed
**236 cases / four executables / three Vulkan executables**, without failures,
disabled or skipped tests. This includes existing DDGI transport/material and
rendering convention regressions. At that checkpoint, the renderer executable
and engine shader hashes were identical to the accepted #119 build. Evidence:
`out/feature-parity/path-transport-acceptance/results.json`, preserved test
executables, SPIR-V, XML results and build/component logs. Physics is enabled
and OpenVDB is disabled in this validation configuration.

## Shared-scene compute backend

`ReferencePathTracePass` and `reference_pathtrace.slang` now consume the existing
immutable `RaySurfaceGpuBindings`, shared material/texture descriptors and shared
light buffers. The backend uses inline ray queries, alpha-mask and stochastic
alpha-blend candidate tests,
section/box clipping, actual surface reconstruction and single/double-sided
normal orientation. Material evaluation includes base/vertex color, metallic and
roughness textures, specular strength/color/IOR, normal maps, emissive textures
and thin-walled transmission factors/textures.
Ray cones (Akenine-Möller et al. 2019) start from the angular spacing between
neighboring perspective samples or the world-space pixel width for orthographic
projection. Scattered rays inherit the slant-corrected footprint of the surface
they leave. Straight-through segments keep the cone's cross section. At each hit,
the world diameter gives a gradient for each UV set, and the texture transform's
linear part maps the gradient into the sampled texture. Coverage and volume
boundary tests use the base mip. BSDF paths and direct-light connections carry
matching cones through transparent sheets so their tint and MIS probabilities
agree. Normal mapping uses the interpolated shading frame with geometric
hemisphere rejection, without a separate shading-normal energy correction.

Paths combine diffuse/GGX scattering with next-event sampling and power-heuristic
MIS. Environment sampling uses a constant scene-linear color or a borrowed
radiance cube, including orientation and intensity. Directional and point/spot
lights retain authored units and range/cone rules; point falloff uses the actual
inverse squared distance. Rectangle/disk emitters have matching area/solid-angle
PDFs and analytic ray intersections, so both camera and BSDF rays can see them.
Emissive and unlit meshes use a separate next-event proposal. It selects an
emitting instance, a triangle, and a uniform point on that triangle. The
world-space area, facing, and selection probabilities define the solid-angle
PDF for both direct connections and BSDF hits. The direct proposal uses the
same clipping, mip-zero coverage, ray-cone emission filtering, and transparent
visibility rules as traced hits. Solid volume emitters are reached by BSDF
paths. The last allowed bounce uses full next-event weight because there is no
continuing BSDF proposal.
The transport uses no raster bounce term, LTC, DDGI feedback, TAA, or denoising.
The optional firefly clamp applies only after the complete path is evaluated.

The supported surface subset currently diagnoses specular/glossiness, clearcoat,
sheen, iridescence and active height displacement. Unsupported paths do not enter
raw moments. Magenta display pixels identify unsupported paths and yellow identifies
invalid/unrepresentable numerical input. Display diagnostics remain separate
from accepted raw scene-linear means and moments. Unlit surfaces contribute
textured base and authored emission, respect opacity coverage, and terminate
scattering.

Anisotropic reflection uses the mip-filtered strength and the rotated mesh
tangent projected onto the final shading normal. Its elliptical GGX
distribution, Smith visibility and visible-normal sampling share the same two
roughness axes. Thin specular transmission retains its isotropic lobe. A
perfectly smooth surface with active anisotropic reflection has a singular
perpendicular axis and produces an unsupported-path diagnostic. [Khronos
anisotropy](https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/2.0/Khronos/KHR_materials_anisotropy/README.md)

Diffuse transmission splits the remaining diffuse energy after specular
transmission. The authored alpha factor and independently sampled tint feed a
cosine-weighted lobe on the opposite hemisphere. Its PDF enters both direct
lighting MIS and continuation weights. On a sheet with both diffuse and specular
transmission, the straight-through visibility probability multiplies the specular
null probability by the probability of selecting the non-diffuse branch.
Active anisotropic refraction and
diffusely transmitting volume boundaries remain diagnosed. [Khronos diffuse
transmission](https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/2.0/Khronos/KHR_materials_diffuse_transmission/README.md)

KHR_materials_dispersion is transported spectrally: after the first dispersive
refractive interface, one uniformly sampled RGB channel carries the path with a
3x weight, and refraction, Fresnel, absorption and every later boundary use
that channel's IOR from the same channel mapping the raster transmission
shading uses. Thin sheets have no refraction and keep the authored IOR; the
emission of the dispersive interface itself stays full RGB.

Closed homogeneous glass volumes support refraction and absorption, including
properly nested media and cameras inside them. See the solid-volume checkpoint
below for explicit topology, clipping, capacity and caustic limits.

The pass owns three explicit RGBA32F storage images: mean/count, second central
moment/diagnostic flags, and display. Typed image formats avoid requiring storage
image reads/writes without a format. Resources require the existing Vulkan 1.4
engine baseline with synchronization2, ray queries and compatible bindless
material/light layouts. The minimal bindings need ten storage buffers, three
storage images and 4,097 sampled images across the compute pipeline. Image
dimensions, storage ranges, sampling settings and the default 512 MiB image
payload budget are checked before use. Empty scenes use a separate background
shader without statically accessed acceleration-structure descriptors.

Use one ordered graphics-queue producer: prepare a fence-retired input slot,
record, submit, then commit before preparing another frame. Successful submission
alone advances sample addresses and image-layout state. Discarded recordings do
not advance them. Camera/clipping data, immutable scene bindings, an explicit
material/light/environment content revision, environment handles, transport
settings and pipeline identity determine history compatibility. Batch-size and
increased sample-stop changes preserve the sequence; lowering the stop below
the current count starts a new sequence. Source descriptors/AS generations remain
borrowed through the graphics submission. Recreation/destruction requires all
readers to retire.

`attemptedSamples()` reports global sample addresses. Actual accepted counts
are stored per pixel in mean alpha; unsupported/rejected paths can make them
smaller. Moment RGB stores Welford M2, so sample variance is M2/(count-1) for
count greater than one. Read diagnostic counters only after the slot's fence
retires. `allocatedBytes()` measures owned VMA image/input allocations, excluding
borrowed scene resources, driver pipeline/descriptor storage and CPU state.

Application integration obtains surface bindings after the image's
`RayShadowManager::recordBuild`, then prepares/records the reference pass before
post-processing. The current backend extent describes the whole camera image;
future docked viewport offsets/cropping need an explicit presentation contract.
The backend represents triangle surfaces and analytic lights, not participating
media/volumes or atmosphere propagation along scene rays.

### Backend validation checkpoint

Twenty additional real Vulkan scene/readback cases exercise the production pass,
shared AS builds and bindless descriptors under synchronization validation.
They cover sky HDR, textured closest hits, actual shadow blockers/restoration,
directional/point/spot units, finite range, rectangle/disk MIS, visible analytic
emitters, emissive-mesh indirect bounces, alpha coverage, facing, buffer bounds,
history resets, exact batching/stops, discarded recordings and allocation retirement.

| Fixture | GPU result | Independent target |
| --- | ---: | ---: |
| Lambert under unit environment, 512 samples | 0.4999761 | 0.5 |
| Point light at distance 2, averaged ROI | 0.3170859 | 0.3170733 |
| Point light at distance 4, averaged ROI | 0.0795007 | 0.0794999 |
| Parallel rectangle emitter, 512 samples | 0.2384790 | 0.2394565 |
| Parallel disk emitter, 512 samples | 0.1992372 | 0.2 |
| Emissive mesh reached by a second bounce, 1,024 samples | 0.2404175 | 0.2394565 |

The Cornell-style fixture has colored walls, a diffuse box, a rough metal box and
a ceiling emitter. At **128×128, 512 accepted samples per pixel, eight bounce
levels and 16 samples per batch**, all raw means/moments are finite, every pixel
retains its complete sample count, and diagnostic flags are zero. The warmed
median is **9.81600 ms per batch** on the RTX 2080 SUPER with validation enabled.
Owned VMA allocations total **787,232 bytes**. These are backend component costs,
excluding AS construction, CPU extraction, presentation and unrestricted
interactive frame scheduling. The median is a single device/run measurement.

Renderer `4e0b7929050794aaa35a93211c66266961f08582afc3c4ce3cc0dd7823a5c752`
builds with the new backend and 104 engine shader assets. The coordinated run
passes **331 cases / ten executables / eight Vulkan executables**, with no
failures, disabled or skipped cases. Seventy-two fresh native captures in eight
strict-clean process logs additionally verify existing forward/deferred raster,
GPU hierarchy switches, MSAA, resize, scene replacement and hard-ray/async-GI
execution. All 36 CPU/GPU image pairs are pixel-identical. These application
captures do not claim viewport path-tracing integration.

Evidence: `out/feature-parity/reference-path-acceptance/results.json`, preserved
component/renderer executables, SPIR-V, XML, source snapshots and logs;
`reference-path-verified-artifacts` contains raw float means/moments/display
images. Preview PNGs use exposure one, Reinhard mapping and sRGB encoding;
they do not modify raw radiance. Physics is enabled and OpenVDB is disabled.

## Application viewport checkpoint — 2026-11-05

Renderer `a78f4ed4e93eebbec0fe99ee951d0fd34668a01137784d595d6aca1354247331`
builds with **105 engine shader assets**. The coordinated run passes **574 cases
in 15 executables**, including **eight Vulkan executables**, with no failures,
disabled or skipped cases. The backend now has 24 scene/readback cases alongside
the 20 transport-math cases. Its repeated 128×128 / 512-sample Cornell fixture has
a warmed GPU median of **9.85834 ms per 16-sample batch** and **787,232 owned VMA
bytes** on the RTX 2080 SUPER with validation enabled; these remain component costs.

**36 full-application captures in 16 clean process logs** verify exact sample
stopping, camera/material resets, material/light/gizmo undo and redo, renderer
switches, resize, GPU hierarchy, the complete UI, empty scenes, HDR environment
sampling, capability fallback and unsupported diagnostics. EXR captures retain
RGB radiance **200,000 / 100,000 / 50,000**, independent of display exposure,
tone mapping and bloom. A point light matches **1/π** at distance two, doubles
with intensity, and falls to one quarter at distance four. Replayed light/gizmo
and material states reproduce identical images. The full UI records zero rejected
samples and keeps DDGI, ray-shadow filtering and TAA inactive in reference mode.

An additional **72 raster/ray-query regression captures in eight clean process
logs** exercise both existing renderers, CPU/GPU hierarchy paths, instancing,
MSAA, resize, replacement and asynchronous GI. All **36 CPU/GPU pairs are
pixel-identical**. Together this is **108 native captures / 24 process logs** on
the same renderer binary. The checks reject VUIDs and synchronization hazards;
ordinary unused-vertex-attribute notices are not presented as validation failures.

Evidence: `out/feature-parity/reference-viewport-acceptance/results.json`, preserved
renderer/component executables, SPIR-V, XML, source snapshots, logs, raw backend
images and native PNG/EXR captures. Reproduce with the archived build/validation
scripts and `tests/validation/reference_path_runtime_regression.py`. Physics is
enabled and OpenVDB is disabled. The issue remains in progress for the full material,
medium, quality/comparison and performance matrix. Changes remain local/uncommitted.

## Camera and raw capture checkpoint — 2026-11-05

The navigation camera passes a 900-pose yaw/pitch grid against its actual forward
direction and camera origin. Independent full-application emissive-quadrant
fixtures validate identity and rotated/rolled cameras, finite/infinite perspective,
orthographic projection and near/far clipping. The camera suite produces 18 native
captures, including four raster comparisons with bounds for half-float storage.

Raw scene-linear raster captures now omit editor selection strokes, wireframe,
normal/geometry diagnostics, technical elevation and BIM inspection annotations.
Native point/curve geometry remains part of the scene. Display captures retain
the selection stroke; all 14 outline regression captures measure five pixels,
including MSAA 4×. This fixes orange outlines contaminating raw EXR reference data.

Renderer `35336c56c5530c91320e8e4665428c9eddb64a47e7c7c73adb5559bfe00bde4d`
with the pre-transparency shader set passes **591 cases / 16 executables / eight
Vulkan executables**. Six native cohorts total **156 captures / 72 process logs**;
all 36 CPU/GPU raster/ray image pairs are exact. The immutable evidence is
`out/feature-parity/reference-camera-checkpoint/results.json`. Its shader hashes
identify this checkpoint separately from the following shader-only fix.

## Alpha transparency checkpoint — 2026-11-05

Alpha-blend surfaces now use stochastic coverage on primary, secondary and
shadow rays. Coverage multiplies material alpha, vertex alpha, opacity factor,
base-color texture alpha and the selected opacity-texture channel. Alpha zero
passes through exactly, alpha one preserves opaque results, and alpha masks keep
their cutoff rule. Double-sided masked surfaces use the oriented shading frame.
Ignored candidates consume no scattering bounce. Emissive blend surfaces follow
the same coverage rule, including indirect paths.

Coverage decisions are addressed by pixel, absolute sample index, bounce, ray
kind and stable instance/triangle identity. Unspecified candidate traversal order
cannot advance a random stream. Shadow decisions use a separate ray-kind address.
Sample batches and retired input-slot changes reproduce identical means and M2.
This implements alpha coverage; optical transmission, refraction and volume
absorption remain explicit unsupported material paths.

Six additional production Vulkan cases cover coverage endpoints, two emissive
layers, partial point-light shadows, double-sided masks, vertex/texture opacity
channels and exact batching. At 4,096 samples, the two-layer emission fixture
measures **(0.995788, 0.748993, 0.376556)** against **(1, 0.75, 0.375)**.
The backend now passes 30 scene cases, with 21 separate transport-math cases.

The user's original `ACCA Building/esempio_01 edificius.ifcx` was loaded directly
from the Debug models directory. At 640×360 / 128 samples / eight bounce levels,
unsupported samples fall from **35,559 to zero**, with **zero rejected samples**.
Its opacity values include 0.01, 0.13 and 0.8. Two additional 64-sample captures
before/after shader reload are pixel-identical; the reload increments the pipeline
generation and resets accumulation. The original model is referenced by path/hash
and is not copied into the acceptance archive.

The coordinated run passes **597 cases / 16 executables / eight Vulkan executables**,
with zero failures, disabled or skipped cases. **39 native captures / 18 process
logs** cover the updated viewport suite, the original ACCA scene and shader reload.
Renderer SHA remains `35336c56c5530c91320e8e4665428c9eddb64a47e7c7c73adb5559bfe00bde4d`;
the acceptance archive records the updated SPIR-V hashes. No new comparative GPU
timing is claimed while the user's editor also runs.

Evidence: `out/feature-parity/reference-alpha-acceptance/results.json`, preserved
executables/SPIR-V/XML, source snapshots, logs and raw/native images. The failing
ACCA baseline is retained separately as comparison evidence, excluded from the
accepted capture counts. Use **Reload Shaders (F5)** in the running editor or
restart this build to load the fix. No new UI or dependency was added.

## Thin-walled transmission checkpoint — 2026-11-05

`PtThinBsdf` extends the reflection/diffuse transport with a GGX lobe in the
opposite hemisphere. The reflection model stays unchanged; transmission reduces
the diffuse base and carries the base-color/vertex/texture tint. A fully metallic
surface has no transmitted lobe. Thin sheets retain the same surrounding medium
on both sides: smooth transmission travels straight through. Roughness spreads
the transmitted directions, with the IOR scaling used by the Khronos sample
renderer. This follows the thin-wall distinction in
[KHR_materials_transmission](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_transmission/README.md)
and its [sample roughness mapping](https://github.com/KhronosGroup/glTF-Sample-Renderer/blob/main/source/Renderer/shaders/functions.glsl).
The reference kernel uses the existing height-correlated Smith GGX convention
for both hemispheres; it has no multiple-microfacet-scattering compensation.

Sampling uses the complete reflection/diffuse/transmission mixture density.
Zero-roughness transmission is a discrete event, excluded from continuous PDFs.
Ordinary below-hemisphere VNDF outcomes remain null samples, not numerical faults.
Transmission zero delegates to the original opaque kernel exactly.

Smooth sheet crossings preserve the last real scattering point and multiply its
BSDF proposal density by each null-choice probability. Light connections integrate
sheet tint and use that same probability chain in MIS. Rough sheets require actual
scattered paths; they are not replaced by straight shadow transparency. Smooth
crossings do not consume scattering bounces or invoke roulette. Vertex addresses
still advance, so choices on successive panes use distinct random values.
Up to **64 smooth crossings per path/connection** are supported. Exceeding the
bound reports unsupported paths instead of silently discarding transmitted light.

Six new Vulkan math cases verify independent double-precision evaluation/PDFs,
reciprocity, full mixture sampling and deltas, furnace energy, sphere quadrature,
invalid-input rejection and exact opaque compatibility. At 131,072 samples, the
rough white-sheet BSDF estimator gives **0.306856132**, and the environment MIS
estimator gives **0.306703369**, against analytic **1 − ln(2) = 0.306852819**.

Ten additional production scene cases cover exact tint through multiple panes,
colored point shadows, opposite-side point lighting, rough environment MIS,
transmission texture channels/backfaces, alpha plus optical transmission, metallic
inactivity, exact batching/M2, the crossing bound and two-pane area-light MIS.
The rough point fixture measures **0.079320996** against **0.079319480**.
The two reflective-pane area fixture measures **0.134172648** with MIS and
**0.134180805** with light sampling alone, against independent quadrature
**0.134318610**. Tolerances and sample counts are recorded in the tests.

The coordinated suite now passes **613 cases / 16 executables / eight Vulkan
executables**, zero failures/disabled/skips: 40 production scene cases and 27
transport-math cases. **46 native captures / 22 process logs** cover the viewport,
thin-sheet loading/export/reload/history and the original ACCA model. Raw EXR
preserves exact **(1.6, 0.6, 0.2)** radiance through two tinted sheets at one allowed
scattering bounce. Reload and transmission-edit undo/redo reproduce identical
images. All 35 scene-only captures from the prior alpha viewport suite are
pixel-identical; the full-UI capture is validated separately. ACCA remains
pixel-identical to its prior fixed image at 128 samples, with zero diagnostics.

The renderer executable SHA is still
`35336c56c5530c91320e8e4665428c9eddb64a47e7c7c73adb5559bfe00bde4d`.
Only the two reference SPIR-V assets change from the alpha checkpoint; the other
103 engine shader hashes remain unchanged. The image/input allocation contract
is unchanged. No additional UI, resource binding or dependency was introduced.
No comparative GPU performance claim is made during concurrent editor use.
Evidence: `out/feature-parity/reference-thin-acceptance-v2/results.json`, source/XML,
executables, SPIR-V, raw float images and native captures/logs. Failed diagnostic
attempts are excluded. Physics is enabled; OpenVDB is disabled.

Solid volumes with active transmission, active dispersion and other unsupported
layers still produce diagnostics. These require the refractive dielectric and
medium-tracking work described by
[KHR_materials_volume](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_volume/README.md).
This checkpoint does not claim their completion or close #47.

## Solid-volume refraction and absorption checkpoint — 2026-11-05

The application now marks frames containing volume materials, so ordinary scenes
skip primary-medium inference. Inline queries retain both sides of a volume
boundary independently of `doubleSided`. An oriented boundary probe determines
the initial enclosing media even when an opaque object is hit before the volume's
exit. The probe is independent of primary near/far geometry tests. Compatible
homogeneous medium definitions can span several mesh primitives/material rows.

Each ray carries a bounded stack of enclosing media. Entry uses the current
medium's IOR and the new medium's IOR; exit restores the enclosing medium instead
of assuming air. Surface tint, transmission textures/factors, metallic/diffuse
mixtures and roughness remain part of boundary scattering. The conductor and
dielectric BSDFs use separate, weighted values and proposal densities. Coincident
smooth reflection deltas merge their probability masses. TIR preserves conductor
color, and transmitted light receives the dielectric fraction once per boundary. The dielectric kernel
uses Snell refraction, the refractive half-vector Jacobian, complete mixture PDFs,
glTF Schlick exit-angle handling and unscaled total internal reflection. Radiance
throughput includes the squared IOR ratio; roulette uses a compensated IOR scale.
Index-matched transmission is a straight-through null event even for rough surfaces.

Absorption uses actual traced world distance between surface/light points,
including transformed geometry, and applies to camera, secondary and finite-light
segments. A baked thickness texel does not change a solid into a sheet or replace
its measured path length. Black attenuation, zero distance and infinite attenuation
distance preserve their limits without a color clamp. These choices follow the
[KHR_materials_volume distinction between geometry and baked thickness](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_volume/README.md).

The current bounds are **eight concurrent media**, **128 boundary hits for initial
containment inference**, and the existing **64 null crossings**. Open volumes,
incompatible non-nested overlaps, volume coverage holes, inspection-clipped volume
scenes and excess capacity are diagnosed. Primary clipping that removes a required
entry can also produce an unsupported boundary transition. Bending specular
connections from point/directional lights need caustic connection work; a direct
shadow query that encounters such a smooth refractor reports unsupported rather
than returning a misleading opaque reference. Index-matched shadow connections,
finite lights inside a medium and rough scattering paths are supported. Dispersion,
participating scattering and the remaining material layers are still pending.

Fourteen additional Vulkan math cases, sixteen production scene cases and three
managed-pipeline lifecycle cases raise the coordinated run to **646 cases / 17
executables / nine Vulkan executables**, with
zero failures, disabled or skipped cases. The backend has **56 scene cases** and
**41 transport-math cases**. Independent Snell-predicted interior emitters verify
actual bent rays and total internal reflection with specular scale zero. Nested
IOR factors cancel exactly with roulette disabled. A 25% metallic surface under
TIR gives **(3.208192, 1.703072, 0.950512)** from its colored conductor/dielectric
mixture, and an index-matched two-boundary shadow retains **0.375²** of the clear
light in the authored test. These catch conductor whitening and double metallic
attenuation. Rough BTDF evaluation, PDFs,
weights and radiance reciprocity agree with independent double-precision math;
environment MIS agrees with sphere quadrature on both sides of an interface.

| Fixture | GPU mean RGB | Independent target RGB |
| --- | --- | --- |
| Camera and finite-light absorption | (0.083690018, 0.327844026, 0.626906812) | (0.083680815, 0.327812551, 0.626850928) |
| Index-matched shadow interval | (0.064458269, 0.094084260, 0.112593654) | (0.064460663, 0.094085629, 0.112594081) |

**160 native captures / 68 process logs** additionally validate imported glTF
volumes, material edits/undo/redo, reload, both camera types, prior viewport cases,
both raster renderers, CPU/GPU hierarchy, ray-query regression and the user's
original ACCA IFCX. The 24 solid-volume captures include nonuniform world scaling,
mirroring, nested/camera-inside media, Snell/TIR emitters and invalid topology.
At 64 samples, application roulette makes deeper nested-interface results
stochastic; their image means match the target while component cases separately
verify exact IOR cancellation. Baked-zero thickness still gives the traced
two-unit absorption result **(1, 0.125, 1)**. Absorption edits produce **(2, 0.5, 1)**;
undo/redo and shader reload reproduce identical images. All 35 preceding scene-only
viewport images, all 36 CPU/GPU raster/ray pairs and the fixed 128-sample ACCA image
remain exact. ACCA has zero unsupported or rejected samples; its source is referenced
by path/hash without copying it into the archive.

Renderer SHA is
`2a51954b23744615fac8fbfe481e4ffab83d76229f86c96a271c8f10db256b47`.
The other 103 engine shader assets are unchanged; two reference SPIR-V assets
change from the thin-sheet checkpoint. The application now supplies the volume
frame flag, so **restart the current executable** to use this milestone in an
editor that was already open. Subsequent shader edits can still use F5.
No new UI or dependency was added. The image/input allocation contract is unchanged;
the Cornell fixture still owns **787,232 VMA bytes**. Its current timing is retained
in the component log, without a comparative GPU performance claim during concurrent
editor/build activity. Physics is enabled; OpenVDB is disabled.

Evidence: `out/feature-parity/reference-volume-mixture-acceptance/results.json`, preserved
binaries, SPIR-V/XML, selected implementation snapshots, logs and raw/native images.
Failed diagnostic attempts and the superseded 636-case checkpoint are excluded. The implementation remains local pending
its incremental publication; the mathematical foundation is already published.
This milestone does not close #47 or the full tracker.

## Remaining implementation

The full #47 scope remains open. Continue in dependency order:

1. Complete material transport beyond the currently diagnosed subset, including
   overlapping/clipped volume handling, caustic connections and participating media.
   Validate normal-map/texture
   filtering approximations and broader live shader-reload/reset behavior.
2. Extend sample/convergence/variance telemetry and raster/reference comparison.
   Validate application Cornell boxes, area
   lights, blockers, roughness sweeps and interiors, including edits, resize,
   camera cuts, technique switches, memory and GPU timing.

#116 denoising and reconstruction remain open.

## Mathematical references

- [Heitz, Sampling the GGX Distribution of Visible Normals, JCGT 7(4), 2018](https://jcgt.org/published/0007/04/01/).
- [PBRT 4e: microfacet distributions, masking and BSDF probability densities](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory).
- [PBRT 4e: path throughput, light/BSDF MIS and Russian roulette](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer).
- [Khronos: Vulkan ray-query foundation](https://docs.vulkan.org/samples/latest/samples/extensions/ray_queries/README.html).
- [Khronos: explicit Slang storage-image formats and synchronization](https://docs.vulkan.org/guide/latest/storage_image_and_texel_buffers.html).
