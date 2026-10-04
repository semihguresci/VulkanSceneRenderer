# Temporal rendering contracts

This is the contract for [TAA epic #39](https://github.com/semihguresci/VulkanSceneRenderer/issues/39),
established by [issue #48](https://github.com/semihguresci/VulkanSceneRenderer/issues/48).
Runtime TAA is available in both raster techniques with 1x samples. It is opt-in
(`--taa`); the default is the existing non-temporal raster route. Camera/object
snapshots, signed velocity, scene-linear HDR history and diagnostics share the
contracts below. See [validation results](taa-validation.md) for measured limits.
It builds on [coordinate conventions](coordinate-conventions.md).

## Running TAA

```powershell
.\VulkanSceneRenderer.exe --taa --msaa 1 --display-mode lit
.\VulkanSceneRenderer.exe --taa --render-technique forward-raster --display-mode lit
.\VulkanSceneRenderer.exe --hidden --no-ui --taa --model models/validation/taa_scene.gltf --width 640 --height 360 --capture-sequence models/validation/taa_object.json --screenshot motion.png --fixed-dt 0.016666667
```

Launch from the executable directory for relative capture-sequence paths. Each
selected frame creates a PNG and a `.telemetry.json` sidecar; intermediate images
carry `.frame-XXXX` suffixes. Keyframes use simulation frame numbers; failed
acquisition retries do not advance the submitted history or jitter. Minimized and
explicitly skipped frames preserve the last submission. Capture events can inject
an acquire out-of-date result before acquisition or a suboptimal result after real
presentation, exercising recovery without leaking a signaled semaphore.

Controls: `--taa-history-weight` (default 0.9, range 0–0.98),
`--taa-variance-gamma` (1.25, range 0.5–4), `--taa-depth-absolute` (0.02 world units,
range 0–1000), `--taa-depth-relative` (0.01, range 0–1), `--taa-jitter-seed`
(default 0), and `--taa-reset-frame` (successful submission number, 0 disables).
Invalid/nonfinite values and TAA with MSAA greater than 1 produce errors.
The UI offers the same settings, reset, effective resolution, allocation size,
resolve timing and reset reason. Weight 0 provides the reconstructed current-frame
fallback. History age starts at 1; useful convergence takes roughly 8–32 frames.

`--display-mode taa-velocity` maps signed UV motion around neutral gray, preserving
direction. `taa-age` shows age/64; `taa-blend` shows the actual weight;
`taa-reactive` shows bounded reactive coverage. `taa-rejection` uses green for
accepted, gray for first/reset, black for uncovered, magenta for invalid, blue
for offscreen, yellow for identity, red for depth and cyan for reactive rejection.
Lit and overview accumulate scene HDR; other raw views
bypass temporal work and jitter. UI and transform/light gizmos render afterwards
on the stable camera. HDR geometry/debug overlays and native point/curve displays
use a conservative current-frame fallback because they lack matching velocity.

## Runtime policy and costs

Rigid triangles, alpha masks (including height displacement), mirrored and double
sided glTF surfaces, native BIM triangles and USD triangle imports use the shared
velocity pass with depth equality. BIM point/curve triangle placeholders also
participate; native point lists/line lists, skinning/deformation without previous
vertices and sky have no valid surface history. Stable node/primitive identities
and monotonically assigned provider lifetime IDs survive compaction and filtering.
Material revision rejection is local. Section coverage resets history; changing
BIM LOD policy invalidates only that provider. Current GPU meshlet LOD selection
controls residency metadata, not replacement triangle topology; future topology
replacement must supply a changed object/lifetime revision.

Every covered OIT layer and emissive material requests current color; a dilated
mask includes the reconstruction footprint, and old reactive coverage prevents
borrowing history after a transparent layer moves away. OIT overflow rejects
history conservatively across the frame. Alpha-tested holes retain their exact
opaque coverage. Layered transparency/refraction has no per-layer temporal
reprojection in this version and can still alias. Rapid highlight changes reduce
weight, while Catmull-Rom reconstruction and variance bounds limit blur/ringing.
No mandatory sharpening is applied.

History contains two sets of color (8), depth/reactivity (8), identity (4) bytes
per pixel: **40 bytes/pixel** total. Current attachments contain motion (16),
identity (4), reactive (1), HDR composite (8) and diagnostics (8): **37 bytes/pixel
per swapchain image**. The payload is `width × height × (40 + 37 × imageCount)`;
telemetry also reports actual VMA allocation bytes including alignment. Resources
are allocated only on first TAA use, retained while disabled, and recreated on
extent/image-count changes. Disabled TAA executes no velocity/compose/resolve
passes. The extra velocity rasterization and two HDR compute dispatches trade
bandwidth and memory for a shared implementation independent of G-buffer MRT
limits. Full-width motion/depth preserve signed range and expected-depth precision.

All histories stay in `GENERAL`. Sync2 barriers order all previous graphics-queue
read/write consumers before the next compute access, including RAW and WAR across
submissions. Per-image/parity descriptors prevent mutation of in-flight bindings.
The existing frame-fence policy retires shared scene buffers; TAA adds no per-frame
device-wide idle. Resize/destruction retires existing submitted work before views,
descriptors or VMA allocations are freed. A submission failure is fatal and never
commits pending temporal state.

## Frame and surface identity

`renderFrameId` is a monotonically increasing 64-bit identifier for a frame whose
temporal passes were recorded and whose graphics submission succeeded. The
previous frame is the preceding successful submission, independently of
simulation ticks, swapchain image index, and CPU frames in flight. An acquisition
failure or abandoned recording does not advance snapshots or the jitter
sequence. Commit current snapshots once after successful submission, not once
per depth/G-buffer/forward pass. A presentation failure after submission still
leaves a rendered history; swapchain recreation invalidates it before reuse.
Queue synchronization must make that submission's history writes available to
the next temporal consumer; CPU snapshot commit is not GPU completion.

A renderable surface is identified by `(providerId, stableObjectId, generation)`.
Neither a compacted indirect-draw index nor a recycled object-buffer slot is a
stable identity. Generation changes on destruction/reuse. Providers retain
identity across filtering, LOD routing, compaction, and ordinary rigid motion.
Extraction supplies current transforms and geometry/material revisions; the
temporal owner matches them to the last submitted snapshot. A newly visible
surface without a previous snapshot has invalid history, even if its current
transform is copied into the previous slot for safe upload.

Required camera snapshot fields:

| Field | Meaning |
| --- | --- |
| `view`, `projectionUnjittered`, `viewProjectionUnjittered` | Current RH camera; reverse-Z; no matrix Y flip |
| `projectionJittered`, `viewProjectionJittered`, inverse | Current raster sampling and depth reconstruction |
| Previous versions of the above | Last successfully submitted frame, never the previous swapchain image |
| Render/display extents; near/far and projection kind | Frame-local reconstruction parameters and compatibility checks |
| Current/previous jitter in render pixels and UV | Known sample displacement, separate from physical motion |
| Frame ID, reset epoch, pre-exposure | History metadata and radiance-domain compatibility |

Per-object snapshots require current and previous model transforms, stable
identity, geometry/material revisions, and `previousSurfaceValid`. Both clips
must describe the same local surface point transformed with the corresponding
camera and model matrices. For future deformation/skinning, a previous model
matrix alone is insufficient: retain previous deformed positions or reject
history. Normal movement does not invalidate history. Spawn, teleport declared
as a discontinuity, topology/LOD correspondence changes, and material changes
invalidate the affected surface. With only provider-wide revision counters,
reject that provider conservatively; do not guess per-object correspondence.

## Coordinates, velocity, and jitter

All image UVs have an upper-left origin, +U right and +V down. Scene passes use
a negative-height viewport. Consequently:

```text
sceneUv = (ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)
sceneNdc = (uv.x * 2 - 1, 1 - uv.y * 2)
```

Physical motion is the signed displacement **previous unjittered UV minus
current unjittered UV**:

```text
currentClip  = currentViewProjectionUnjittered * currentModel * localPosition
previousClip = previousViewProjectionUnjittered * previousModel * localPosition
velocityUv   = previousUnjitteredUv - currentUnjitteredUv
velocityPixels = velocityUv * renderExtent
```

Velocity is not restricted to `[0, 1]` and must not be saturated. A surface moving
right/down has negative U/V velocity. A stationary camera and surface have zero
velocity regardless of jitter. Invalid history is a separate validity bit, not
a zero-velocity sentinel. Shader implementations must interpolate the clips or
surface position correctly before the perspective divide; interpolating
already-divided vertex velocities is not a general perspective-correct solution.

Jitter is the projected image displacement in **render pixels**, +X right and
+Y down. A centered 32-sample Halton(2,3) cycle supplies bounded, zero-mean subpixel jitter. Its conversion
and application are:

```text
jitterUv  = jitterPixels / renderExtent
jitterNdc = (2 * jitterUv.x, -2 * jitterUv.y)
jitteredClip.xy = unjitteredClip.xy + jitterNdc * unjitteredClip.w
jitteredClip.zw = unjitteredClip.zw
```

Equivalently, `projectionJittered = clipTranslation * projectionUnjittered`,
where the identity translation has **GLM column/row indices**
`[3][0] = jitterNdc.x` and `[3][1] = jitterNdc.y`. Slang matrix indexing remains
row/column even with column-major storage; its translation entries are
`[0][3]` and `[1][3]`. This is a clip-space translation, not a Y flip.
Jittered inverse matrices reconstruct main raster depth. Unjittered matrices
produce velocity and stable camera metadata. Keep frustum culling conservative
over the jitter footprint; do not cull edge geometry with a smaller frustum.

## Reprojection and history grids

First delivery stores **resolved color history on an unjittered pixel grid**.
Retained main depth, validity, and identity describe the **previous jittered
raster grid**. Their sample addresses differ. For a surface sampled at the
current raster UV:

```text
previousUnjitteredUv = currentRasterUv + velocityUv - currentJitterUv
colorHistoryUv      = previousUnjitteredUv
previousDepthUv     = previousUnjitteredUv + previousJitterUv
```

The shared `reprojectToPreviousGrid` helper takes the previous grid's jitter:
zero for resolved color, previous camera jitter for raw depth/identity. This
is the single place that applies jitter correction. Do not subtract jitter
from an already unjittered velocity or add previous jitter to resolved color.

The resolve reconstructs current color on the unjittered output grid. For an
output center `outputUv`, its corresponding current raster location is
`outputUv + currentJitterUv`; obtain compatible color, motion, coverage, and
surface depth there. Then color reprojection simplifies to `outputUv +
velocityUv`. A raster pixel center used directly as `currentRasterUv` produces
a result located at `currentRasterUv - currentJitterUv`, so it cannot be written
as though it were the unjittered output center. The spatial reconstruction and
edge handling in `temporal_resolve.slang` preserve this distinction.

Homogeneous clips must be finite with `w > 0`. Divided reverse-Z depth must be
finite in `[0, 1]`; UV may project outside the image. An address is usable only
when both coordinates are finite and `0 <= uv < 1`. Reject off-screen addresses
before sampling; sampler clamping must not turn them into valid history. Raw
depth/identity use point sampling or a conservative tap selection, never a
bilinear blend of unrelated surfaces. Color filtering/clipping must account
for invalid taps near edges and discontinuities. Failed checked helpers return
finite zero payloads with `valid = false`; consumers must honor that flag.
The two low-level NDC/UV conversion helpers assume finite, representable inputs.

## Depth, rejection, and resets

Stored main depth is reverse-Z device depth (`clip.z / clip.w`), near 1, far 0,
clear 0. Depth 0 is mathematically projectable but uncovered sky/background
must never become valid geometry history. First delivery uses current color for
sky/background, with invalid motion/history. Rotational sky reprojection can
be added explicitly later.

For disocclusion testing, transform the **current surface's corresponding
previous position** with the previous unjittered camera/model matrices. Its
expected previous reverse-Z depth is returned with the motion vector. Jitter
does not change clip Z/W. Compare this to depth sampled at `previousDepthUv`,
using the previous frame's projection parameters. Do not compare current depth
to previous depth across camera/object movement. For finite perspective near
`n`, far `f`, positive view distance is:

```text
distance = n * f / (depth * (f - n) + n)
```

For orthographic projection it is `f - depth * (f - n)`. Infinite-far
perspective, if introduced, needs its own declared reconstruction rule.
Reject nonfinite reconstruction, uncovered samples, mismatched stable identity
or revisions, invalid object/frame history, and disocclusion based on absolute
plus relative **view-distance** tolerance (defaults: 0.02 world units and 1%).
Projection tests cover perspective/orthographic depth ranges. Velocity alone is
not disocclusion evidence.
Future normal checks can reduce confidence but cannot repair missing identity
or invalid depth. Never blend across a reset or missing metadata.

| Event | Required action |
| --- | --- |
| First frame, camera cut, scene replacement, device/resource recreation | Reset all history; seed with reconstructed current color |
| Render/display extent, aspect, FOV, near/far, projection kind change | Reset; do not resample incompatible history in the first delivery |
| Render technique, AA mode, history format or radiance encoding change | Reset before the next temporal pass |
| Object creation/removal/reuse, geometry/LOD discontinuity, material revision | Reject affected surfaces; provider-wide rejection when finer evidence is unavailable |
| Ordinary camera/object motion, next jitter sample | Retain history subject to reprojection and rejection |
| Display exposure or tone-mapping adjustment | Retain scene-linear history |
| Incompatible/invalid pre-exposure metadata | Reset; otherwise rescale as below |
| Skipped render / failed acquisition or submission | Keep last successful snapshots; do not advance frame/jitter IDs |

## Radiance, masks, formats, and modes

Accumulate scene-linear HDR after opaque lighting and transparency composition,
before bloom, display exposure, tone mapping, gamma encoding, and UI. Current
HDR storage is not pre-exposed: `preExposure = 1`. If pre-exposure is introduced,
`storedColor = sceneRadiance * preExposure` and old history must be multiplied
by `currentPreExposure / previousPreExposure` before neighborhood clipping or
blending. Require finite positive values and a representable positive ratio;
reject incompatible color spaces or encodings. Display exposure is applied
after temporal reconstruction and is separate from this storage conversion.

Motion validity is binary. Confidence and reactivity are `[0, 1]` masks:
confidence 0 forbids history, confidence 1 permits it subject to rejection;
reactivity 1 requests current color, reactivity 0 permits normal accumulation.
Depth/identity rejection still takes precedence over both. Transparent OIT
surfaces without trustworthy correspondence must be reactive; emissive,
animated, and rapidly changing lighting can reduce history weight. #57 defines
mask generation and coverage, including mixed opaque/transparent pixels.

Runtime resources, checked against queried Vulkan format features:

| Resource | Format / grid | Access contract |
| --- | --- | --- |
| Motion + expected previous depth + validity | `R32G32B32A32_SFLOAT`, current raster | XY signed UV; Z previous reverse-Z depth; W binary validity |
| Resolved HDR color / color history | `R16G16B16A16_SFLOAT`, unjittered | Linear floating-point sampled/storage images |
| Retained depth + reactivity | `R32G32_SFLOAT`, previous raster | Raw reverse-Z depth and old reactive coverage |
| Retained identity | `R32_UINT`, previous raster | Epoch-stable surface token for identity/revisions; 0 uncovered |
| Confidence / reactive | `R8_UNORM`, current raster | Normalized masks with a declared reconstruction filter |

Surface tokens must compare consistently across current and retained metadata.
Assign a new token when identity/generation or its compatible geometry/material
revision changes; never recycle a token while matching history can survive.
Reset the epoch before token exhaustion/reuse. The token is a GPU representation
of stable identity, not the provider's draw index.

Keep persistent history slots separate from swapchain attachments. Negotiate
color-attachment, sampled-image, storage-image, and transfer capabilities for
the operations actually used. If a preferred format is unsupported, explicitly
select a compatible wider format or disable TAA with a reason. The velocity
issue #51 chooses attachment/pass layout against device MRT limits; adding
another G-buffer attachment must not silently exceed them. Ping-pong slots
need read/write ordering across in-flight frames and retirement before resize
or destruction. Vulkan-Hpp RAII owns engine Vulkan handles; VMA owns image
allocations. History is never read and overwritten concurrently.

Initial support: native-resolution **1x-sample** forward and deferred raster,
with rigid glTF and BIM/USD motion as implemented by #52/#53. TAA off preserves
existing MSAA choices. A request for TAA with MSAA >1 must be explicitly rejected
with an actionable setting conflict; do not silently change either setting.
Combined TAA/MSAA, dynamic resolution, temporal upscaling, and deforming geometry
without previous vertex data are outside the first delivery. Unsupported
surface motion falls back to current color; missing required resource/device
support disables the mode explicitly. The controls issue #58 exposes this
policy when the runtime feature exists.

## Ownership and integration

| Component | Runtime responsibility |
| --- | --- |
| `CameraData` / `lighting_structs.slang` | 464-byte ABI: current raster/inverse, current/previous unjittered and previous raster/inverse, jitter UV, frame/epoch/validity and extent |
| `ObjectData` / `object_data_common.slang` | 224-byte ABI: current/previous rigid model, lifetime identity and compatible surface token |
| `TemporalState` | Pending and submitted snapshot maps; commits only after successful graphics submission |
| `TemporalManager` | Three-MRT dynamic velocity pass, HDR OIT compose, compute resolve, ping-pong histories, descriptors and queue barriers |
| `FrameResourceManager` | Five current-frame attachments per swapchain image, allocated lazily for TAA |
| Shared render graph registration | `Lighting -> TemporalVelocity -> TemporalResolve -> Exposure -> Bloom -> PostProcess` in both techniques |
| `post_process.slang` | Uses resolved HDR with OIT already composed when TAA runs; native path retains its original OIT composition |
| Deterministic capture runner | Fixed-step keyframes, lifecycle events, selected PNGs and effective configuration/timing/memory JSON sidecars |

The architecture above is implemented by `TemporalManager` and `TemporalState`. Shared helper result structs are local math values, not buffer layouts.
Do not upload their C++/Slang `bool` fields as a shared ABI.

## Future consumers

[Upscaling epic #44](https://github.com/semihguresci/VulkanSceneRenderer/issues/44)
can consume scene-linear HDR, unjittered physical velocity, reverse-Z depth,
render/display extents, jitter in render pixels, projection parameters, reset
epoch, successful-frame delta time, exposure/pre-exposure, confidence, and
reactive masks. An adapter converts units/sign/grid to the selected SDK instead
of changing this renderer contract. For example, the
[AMD FidelityFX temporal-upscaling inputs](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)
accept jitter-free motion with a scale converting UV displacement to render
pixels; pre-exposure and inverted-depth settings must also be declared. SDK
jitter sign is adapter-specific: our jitter means image displacement, which
can differ from an SDK's sample-location offset. Temporal upscaling replaces
the native TAA resolve; it does not run a second accumulation after it.

Future reflection/GI/shadow denoisers share identity, matrices, motion, depth,
frame/reset metadata, and rejection conventions. Their signal history and
filters remain separate; a valid color history does not prove valid radiance
or visibility history for another effect.

## Validation

`TemporalConventions.h` and `temporal_common.slang` contain matching projection,
jitter, physical motion, reprojection, and pre-exposure helpers.
`temporal_convention_tests` checks named framebuffer corners, independently
calculated 90-degree perspective/orthographic cases, aspect changes, camera and
object movement, static jitter, the distinct color/depth grids, off-screen
history, invalid clips, and exposure conversion. Invalid checked results have
finite payloads. Assertions use analytic expected values, not a second copy of
the helper formulas.

Building the test also compiles `tests/shaders/temporal_contract.slang` with the
engine's Slang/SPIR-V 1.6 and column-major flags, referencing every helper through
dynamic buffers. If `spirv-val` is available, CTest registers
`temporal_contract_spirv_validation` for Vulkan 1.4. This probe validates shader
compilation and SPIR-V legality; it does not execute a GPU parity test or claim
TAA image-quality validation. The opt-in `temporal_regression.py` suite supplies
runtime sequence, reference, lifecycle and performance evidence.

In a Visual Studio Developer Command Prompt, after configuring the project:

```powershell
cmake --build out/build/windows-release --config Release --target temporal_convention_tests
ctest --test-dir out/build/windows-release -C Release --output-on-failure -R "^temporal_"
```
