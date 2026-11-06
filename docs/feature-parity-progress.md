# Feature parity implementation progress

Tracker: [#121](https://github.com/semihguresci/VulkanSceneRenderer/issues/121).
First implementation batch, validated on 2026-10-25. Changes are in the local
working tree; no commit, push or pull request was created by this batch.

## Implemented

| Issue | Result |
| --- | --- |
| [#87](https://github.com/semihguresci/VulkanSceneRenderer/issues/87) | Shared pre-resource glTF version/capability validation, structured contextual diagnostics, required/optional distinction, and valid optional-image fallbacks. |
| [#88](https://github.com/semihguresci/VulkanSceneRenderer/issues/88) | Bounded scene edit history, undo/redo controls and shortcuts, inspector/viewport gesture grouping, exact hierarchy and matrix replay, primitive node detach/restore, and physics-safe validation. |

See [glTF capabilities](gltf-capabilities.md) and
[scene edit history](editor-history.md) for behavior and limitations.
Independent implementation/review agents checked both features. Review found
and resolved optional texture decoding and malformed-index issues, partial
iridescence reporting, and a hierarchy ordering bug that could block redo.

The build also exposed Windows Ninja command-shell parsing of pipe-separated
runtime DLL paths. Runtime staging now passes a generated per-target/config
manifest, retaining the existing script argument as a compatibility fallback.

## Validation

The complete renderer and focused test targets build in
`out/build/feature-parity` using MSVC, Ninja, C++23 and the Vulkan SDK.
The isolated configuration enables Jolt physics and disables the unrelated
OpenVDB and TinyUSDZ optional integrations. Their separate ongoing work remains
in the original checkout. No placeholder source files or skipped-test success
results were used. Ordinary CMake generation succeeds for this configuration.

All **213 cases across nine CTest executables pass**:

| Executable | Cases |
| --- | --- |
| `edit_history_tests` | 14 |
| `scene_edit_history_tests` | 15 |
| `scene_graph_edit_tests` | 8 |
| `gltf_capabilities_tests` | 21 |
| `gltf_loader_tests` | 9 |
| `physics_scene_tests` | 75 |
| `scene_graph_tests` | 30 |
| `camera_controller_transform_tests` | 15 |
| `scene_hierarchy_ui_tests` | 26 |

Coverage includes required extension rejection before missing-resource reads;
JSON/GLB versions; default/bounded optional PNG fallbacks; overflowed image
indices; exact affine and parent/sibling restoration; undo/redo branching and
eviction; primitive removal/restoration; scene replacement; prospective-parent
physics validation; and actual multi-frame ImGui drag input. Text-input double
click behavior is retained; the two-drag test separates distinct gestures by
ImGui's normal double-click interval.

Native deferred and forward smoke captures both exit successfully with TAA,
1x MSAA, hard ray-query shadows and Vulkan validation enabled on an NVIDIA
GeForce RTX 2080 SUPER. Both produce PNG/telemetry output with zero VUID or
synchronization errors. The driver reports three existing unused-vertex-input
performance warnings per startup. These are rendering smoke tests; detailed
history behavior is covered by the CPU and ImGui tests above.

Evidence is local under `out/feature-parity/runtime`: `smoke-results.json`,
`deferred-scene.png`, `forward-scene.png`, their telemetry and stdout/stderr
logs. CTest XML is under `out/build/feature-parity/test_results`.
`git diff --check` passes.

## Export, animation and deformation iteration

The next P1 iteration is validated on 2026-10-25. All changes remain local and
uncommitted. Issues #89, #90 and #91 remain open with explicit progress and
remaining scope; tracker #121 retains the complete feature backlog.

| Issue | Implemented behavior | Remaining work |
| --- | --- | --- |
| [#89](https://github.com/semihguresci/VulkanSceneRenderer/issues/89) | Save/Save As/Ctrl+S and `--save-scene`; runtime hierarchy, names, matrices, mesh visibility, added primitives, standard materials/textures and live lights; portable resources and staged atomic replacement. | Animated/deformed source reconciliation, custom material baking and later linked-asset integration. Unsupported cases fail explicitly. |
| [#90](https://github.com/semihguresci/VulkanSceneRenderer/issues/90) | STEP/LINEAR/CUBICSPLINE clips, quaternion and packed-weight sampling, playback UI/CLI, source-associated runtime TRS, numeric material/light/camera targets and authored cameras. | Complete pointer target set including texture transforms, skin/morph rendering and animation-preserving export. |
| [#91](https://github.com/semihguresci/VulkanSceneRenderer/issues/91) | Validated deformation import/CPU reference and production Vulkan compute pass with current/previous streams and explicit barriers. | Scene draw/velocity binding, bounds/picking/provider and ray/BLAS reconciliation, missing-normal/tangent regeneration and visual consumer checks. The GPU pass is not yet connected to runtime draws. |

The source capability registry now reports animation-pointer partial support
with a warning and recognizes authored mesh visibility. Independent review
found and resolved loss of manual sun overrides during export and directional
shadow cascades using the viewport camera after a source-camera selection.
Source cameras now supply the cascade view, matching near/far values and a
finite shadow projection while infinite main rendering remains unchanged.

See [scene export](gltf-scene-export.md), [animation](gltf-animation.md) and
[deformation](gltf-deformation.md) for APIs, controls, format policy and limits.

### Validation

The full renderer builds successfully with the same isolated MSVC/Ninja/C++23
configuration. **All 299 cases in 15 CTest executables pass**, including the
original 213 cases and these additional suites:

| Executable | Cases |
| --- | --- |
| `gltf_scene_export_tests` | 8 |
| `gltf_material_export_tests` | 8 |
| `gltf_animation_tests` | 27 |
| `gltf_deformation_tests` | 32 |
| `scene_light_export_tests` | 6 |
| `gltf_deformation_gpu_tests` | 5 |

The GPU tests ran with `CONTAINER_RUN_GPU_GLTF_DEFORMATION=1`; they were not
skipped. Production compute output is compared with independent CPU numerical
results for multiple targets/influence sets, independent instances, mirrored
and tiny scales, previous poses, resets, frame reuse and recreation. Vulkan
synchronization validation reports zero errors.

`tests/validation/gltf_scene_export_runtime_regression.py` performs a native
source → GLB → glTF round trip with matched viewer settings. Deferred reload
mean absolute pixel errors are approximately 0.00000193 and 0.00000707 on the
0–255 scale; the 99.9th percentile is zero. The forward reload also succeeds.
The original Cornell filename activates a diagnostic viewer preset, so the
regression uses a neutral source filename and explicit viewer settings rather
than mistaking that preset for serialized scene appearance.

`tests/validation/gltf_animation_runtime_regression.py` passes all 16 native
captures across deferred and forward techniques. Telemetry and independent
pixel assertions verify actual node matrices, material red-to-blue changes,
light intensity/brightness and source camera FOV. Four additional source-camera
captures with directional lighting pass after the cascade fix. These runs have
no VUID or synchronization failures.

Evidence is in `out/feature-parity/export-matched-runtime`,
`out/feature-parity/animation-runtime` and
`out/feature-parity/animation-camera-shadow-runtime`; reports include the
commands, image metrics and relevant executable hashes. CTest XML remains in
`out/build/feature-parity/test_results`. `git diff --check` passes.

## Runtime deformation and texture animation iteration

The following checkpoint on 2026-10-25 supersedes the earlier integration
status above. Deformation now has per-instance expanded geometry and resolved
mesh/joint ownership; both raster techniques bind current GPU vertices and
temporal velocity binds the previous submitted pose. CPU bounds, production
picking and ray providers use the same evaluated geometry. Pose history advances
only after successful queue submission, while seeks/topology changes reset it.
Local mirrored skin winding has an independent ray-facing flag, preserving
the existing behavior for a later mirrored model transform.

Animation pointers now update offset/scale/rotation for all 18 implemented
texture infos, clearcoat-normal scale and existing specular/glossiness factors.
GPU handles, channels, UV sets and authored optional rest objects survive.

The full renderer and **371 cases across 20 focused CTest executables pass**.
This includes all earlier suites, 34 animation cases, 9 light-export cases,
18 scene deformation cases, 10 public-loader cases, 8 temporal-binding cases,
22 ray-scene CPU cases, 5 real GPU deformation cases and 4 real GPU ray-query
cases. GPU tests ran with their enabling environment variables, rather than
being counted as skipped success. Shader and renderer compilation succeeded.

`out/validation/gltf-deformation-runtime/results.json` records a complete run
of **92 native captures**: 32 independent baked-reference pairs, 10 moving/
paused velocity checks, and 8 receiver-only controls. Fixtures exercise defaults
without clips, morphing, nested joints, inverse mesh cancellation, independent
shared skins/weights, normalized multi-set influences, mirrored/nonuniform
frames, picking and previous geometry in both raster techniques. The ray
oracle independently intersects light-facing triangles against floor-to-light
segments; the open mirrored quad prevents an exit face from masking a winding
error. Worst reference ROI MAE is 0.0000981/255, maximum velocity-component
error is 0.0478 pixels, and ray lit-region P95 error is zero. No Vulkan or
synchronization errors occurred. `summary.json` condenses the numerical results.

Four additional native texture-motion captures pass in both raster techniques:
5.36% of pixels change and mean absolute change is 6.73/255, with unchanged
geometry and no validation errors. Evidence is in
`out/feature-parity/animation-texture-runtime/results.json`.

Animated/skin/morph export is being implemented and independently reviewed.
Imported light overlays preserve animated ancestry; review additionally found
ancestor sun priority and scaled light-sampler cases, which have fixes and
12 isolated CPU tests pending the next shared rebuild. Those extra results are
not included in the 371-case checkpoint above. Missing-base normal/tangent
regeneration remains an explicit #91 limitation. Changes remain local and
uncommitted, and all affected implementation issues remain open.

## Animated export and missing-frame checkpoint

The next validated checkpoint on 2026-10-25 builds the full renderer and passes
**398 cases across 21 focused executables**, with real Vulkan deformation and
ray-query tests enabled and no skipped cases. Animated export now retains bind
geometry, influences, morph targets, keys and independent instance skins and
weights. Explicit constant restore tracks preserve cross-clip rest behavior
after saving from a nonzero pose. Graph correction parents and remapped
material/light/camera pointers retain edited hierarchy and imported units.

**265 native captures** pass on executable SHA-256
`e12aa3ad0def51b3c23d40ede10541b82b0530850852cb99f1a51f61b2b32e26`:

- GLB: 82 captures, 38 source/reload pairs, all pixel exact.
- ASCII glTF: 59 captures, 28 source/reload pairs, all pixel exact.
- Deformation: 124 captures, 42 independent reference pairs, 14 moving/paused
  velocity checks and 12 receiver-only controls, including directional shadows
  and missing-base normal/tangent regeneration.

There are zero Vulkan or synchronization validation failures. ASCII resource
audits verify relative content-addressed buffers, embedded PNGs, unchanged
source sampler/accessor policies, pointer material remapping and independent
skins/default weights. Artifacts are in
`out/feature-parity/animated-export-runtime/results.json`,
`out/feature-parity/animated-export-gltf-runtime/results.json` and
`out/validation/gltf-deformation-final-summary.json`.

The missing-frame path generates flat normals/Mikk tangents after morphing and
before skinning, then publishes evaluated current/previous vertices through the
production GPU streams. Authored base frames continue to use GPU evaluation.
Source vertex order remains available for portable export.

The resource audit found optional texture-transform metadata being dropped.
That preservation fix and a static/skin-only UV1 tangent generation fix are
implemented with focused tests and pending the next shared build. P2 #95 filter
selection is also implemented and undergoing GPU/native validation. These new
changes are not included in the checkpoint counts above.

The subsequent renderer build passes **427 cases across 24 focused test
executables**, including 13 public-loader cases, 9 material-export cases,
8 sampler-policy cases, 14 MaterialX cases and 3 real Vulkan sampler cases.
All enabled GPU tests run with zero skips. The broader convention suite passes
194/195 cases; its one stale UI reparent assertion has been updated to the
history delegation introduced in batch 1 and awaits the next test rebuild.

On unchanged executable SHA-256
`cb527cd8607a45a62bac0845559b86137f664e8ae239af753901cc3051b86e52`,
the metadata follow-up passes 18 captures/8 pixel-exact source-reload pairs in
both formats, with explicit assertions on retained transform `extras` and
sampler policies. The static/skin-only UV1 suite passes 36 captures/12
pixel-exact analytical comparisons and 12 deliberately wrong-UV0 controls.
Source geometry order and skin-only current GPU frame checks pass. These runs
have zero Vulkan/synchronization failures; reports are in
`out/feature-parity/animated-export-texture-metadata-runtime/results.json` and
`out/feature-parity/gltf-tangent-uv-runtime/results.json`.

Sampler filtering subsequently passes **96 captures/74 checks**, including
all six minification policies, both magnification filters, base/mip behavior,
simultaneous shared-image/different-sampler probes, MASK/BLEND coverage and
animated material factors. All 46 save/reload pairs are pixel exact. Hard ray
shadows and DDGI both run in each technique, with 1,536 traced DDGI rays per
smoke capture. There are zero validation/synchronization errors and the
executable hash is unchanged. Reports are in
`out/feature-parity/sampler-filter-runtime/results.json` and
`out/feature-parity/sampler-shared-visibility/results.json`.

Canonical sampler keys retain all 108 combinations of wrap/min/mag policy.
Actual scene descriptors use dense active slots with checked device limits,
while sampler handles remain in a bounded lazy cache across reloads. Explicit
non-mip policies retain mip zero without overriding the requested min filter;
anisotropy is disabled so nearest/linear semantics remain authored. Incoming
replacement policies are checked before live assets retire, and direct image
cache identity includes color space. #95 scope and acceptance checks are
recorded as locally complete; the issue remains open pending integration.

## Accessor and vertex-alpha checkpoint

The 2026-10-25 renderer build passes **650 cases across 26 executables**, with
zero failures or skips. This includes all 195 rendering conventions and actual
Vulkan deformation, material sampler and ray-query execution. The earlier stale
reparent assertion now passes against the history-backed implementation.

On executable SHA-256
`cdc23f9806e19c06a4894b306d7d40ee259a7ccb27adf31346f5871df1d90d64`,
**236 native captures** pass with zero Vulkan/synchronization errors:

- Accessors: 40 captures/30 checks cover dense-plus-sparse and sparse-only data,
  signed normalized/raw quantization, experimental DOUBLE attributes and keys,
  combined sparse morph/skin, and portable save/reload in both techniques.
  Positive image comparisons are pixel exact.
- Vertex alpha: 72 captures/50 checks cover FLOAT, normalized U8/U16, MASK,
  BLEND, OPAQUE, transmission, animated morph/skin, raster/ray shadows and DDGI.
  Independent texture-alpha references have maximum positive mean error
  0.01205/255. OPAQUE transmission ignores vertex alpha; MASK/BLEND retain
  coverage. Each hard-shadow/DDGI witness traces 2,048 rays. GPU picking
  readback is not covered by this native harness.
- Deformation regression: 124 captures retain 42 independent reference pairs,
  14 moving/paused velocity checks and 12 receiver controls, including missing
  normal/tangent frames and directional shadows. Worst reference ROI error
  remains 0.0000981/255.

The checked accessor view preserves source bytes and handles valid bufferless
zero accessors, strict sparse ranges/ordering, matrix column alignment, exact
integer indices and checked float conversion. DOUBLE animation interpolation
remains binary64, while runtime float properties reject overflow/underflow
before publication. Pose application, seeks, clip changes and rest restoration
roll back atomically on validation failure. Portable source buffers now retain
eight-byte alignment for DOUBLE accessors. Vertex alpha survives the 72-byte
vertex ABI, deformation, raster material coverage, ray extraction and export.

Evidence and report hashes are in
`out/feature-parity/accessor-validation-checkpoint.json`; the underlying reports
are `out/feature-parity/accessor-runtime/results.json`,
`out/feature-parity/vertex-alpha-runtime/results.json` and the three
`out/validation/gltf-deformation-accessor-*` result directories. All 92 shader
binaries match the manifest captured during the run through its completion.
#96 scope and acceptance checks are recorded as locally complete; the issue
remains open pending integration. Unsupported quantized morph semantics and
experimental float64 restrictions remain explicit capability diagnostics.

## Compressed geometry checkpoint

The next integrated build passes **675 cases across 29 executables**, with zero
failures or skips and real Vulkan tests enabled. Decoder-disabled integration
separately passes 48 cases across four executables. The compression helper also
passes all three disabled-backend cases privately, and installed-package lookup
and upstream notice staging are verified.

Optional Draco and meshopt decoding normalize detached models into checked
accessor storage before geometry publication. Source preflight checks declared
ranges and conservative aggregate decoded storage with a default 256 MiB cap.
Legal URI-less meshopt placeholders use one-byte parser stand-ins, so they do
not allocate full destination-sized zero buffers. The Draco decoder runs only
in an adjacent hidden Windows worker with enforced memory/process/time limits,
strict typed response validation and directory-identity-safe cleanup. The
renderer link rule contains no Draco library. Disabled required backends fail
before resource reads; valid optional fallbacks remain usable.

On renderer SHA-256
`e81b0d956b3f17aef9e2364a6c1ce149dba812f6e52cbd27ca32651e06dd46cd`,
**50 validated native captures/45 checks** pass across five scenes in both
raster techniques. EXT and release-candidate KHR meshopt cover external,
embedded and GLB BIN storage, attribute/triangle/index modes and combined
animation/skin/morph. The Draco fixture covers lit normal mapping, raw
normalized UV/RGB/RGBA, MASK alpha and ordinary uncompressed tangents. Every
positive source/reference and plain glTF/GLB reload comparison is pixel exact;
negative controls have mean error at least 9.019/255. There are zero Vulkan or
synchronization failures. All 93 pre-run manifest entries (renderer, worker,
fixture generator and 90 shader binaries) remain unchanged.

Six extra captures from two fixture-contract corrections are retained and
excluded from the 50 passing count: the fixture initially supplied
nonorthogonal tangents and expected RGB colors to remain VEC3 after runtime
export. Corrected checks account for sanitized tangent frames and RGBA with
implicit alpha one. Neither correction changed production code or binaries.

Review also corrected packed narrow Draco attribute alignment, disabled tagged
fallbacks, untagged destination buffers shared by EXT/KHR views, aliased Draco
attribute IDs and cleanup through replaced directories. CPU coverage includes
all supported meshopt filters and versions, malformed payloads, limits, source
immutability, portable metadata and real worker allocation denial/timeouts.

Evidence: `out/feature-parity/compression-validation-checkpoint.json`,
`out/feature-parity/compression-runtime-matrix.json` (SHA-256
`5996141abfd9062c34e147c7043f69844c30ef059426a1b1b0a9de0c4c2657dd`),
`out/feature-parity/compression-native-manifest.json`,
`docs/gltf-compression.md` and `docs/gltf-draco-worker.md`.
#93 scope and acceptance are locally complete; it remains open pending
integration. Draco triangle strips and non-Windows bounded decoding remain
explicitly unsupported; KHR meshopt remains an explicitly partial release
candidate capability.

## Typed texture and material variant checkpoint

The integrated build passes **794 cases across 36 executables**, with zero
failures or skips and actual Vulkan deformation, sampler, ray-query and typed
upload execution. A separate texture-codec-disabled integration passes 59 cases
across five executables. Isolated #98 parser/scene-attachment work also passes
14 CPU cases; it is not yet connected to runtime rendering.

#94 now uses typed CPU image data and device-profiled Vulkan uploads. PNG16,
Radiance HDR, DDS, KTX1/KTX2, optional Basis ETC1S/UASTC and static WebP retain
precision, channel swizzles and authored mip levels. Native compressed formats
require both enabled logical-device features and sampled/filterable/transfer
support; supported CPU/transcode fallbacks are explicit. Authored single or
partial mip pyramids never regenerate. GPU readback verifies exact R16/float
and compressed-block bytes plus distinct linear128 versus sRGB188 generated
mip values. Per-image limits remain 256 MiB; aggregate decoded runtime texture
preparation is 512 MiB, covering the existing 272 MiB decoded Sponza fixture.

Image selection validates alternate/core references and codec availability
before resource reads. Images decode from detached, compression-normalized
storage; image buffer-view validation and private parser stand-ins prevent
TinyGLTF from forming unsafe spans into meshopt placeholders. New MIME base64
data URIs normalize only in detached parser JSON, retaining payload and plus
signs. Source image bytes survive external-file changes/deletion. Color/data
roles and sampler policies share images correctly without collapsing authored
texture identity: per-material imported slot provenance preserves original
texture/image/sampler metadata when resources deduplicate.

#97 now imports validated named material variants, exposes undoable UI/default
selection and `--gltf-variant`, and updates material references, alpha/sidedness
routing, ray generations, temporal epochs and probe state. Changing generated
normal-map UV sets stages regenerated static/bind tangent frames before graph
publication. Static, skinned and morphed cases share the same source primitive
identity. Portable export retains/remaps variants and alternate materials;
saving makes the selected look the reloaded file's core Default assignment.

One unchanged renderer, SHA-256
`ab019867be34735e8cce236442f2508b86b8eaddba42719d8f30b306ccf62cc2`,
passes **183 selected native captures / 137 checks**:

- Variants: 105 captures/81 checks, all pixel exact, including static/skin/morph,
  both lit raster techniques, deferred normals, undo/redo/default/CLI selection,
  GLB/ASCII reloads, shared instances, alpha/double-sided classification,
  temporal invalidation, ray instance/generation refresh and active DDGI.
- Textures: 78 captures/56 checks across ten format cases, both raster
  techniques, independent negative controls, MASK/BLEND, and GLB/ASCII reloads
  after deleting the owned external images. Every positive comparison is pixel
  exact except ETC1S, which differs by at most one display code (interior mean
  0.3333/255). Compressed-alpha hard-shadow/DDGI witnesses are pixel exact and
  trace 2,048 rays each. The smallest negative-control interior error is
  26.8333/255. GPU picking has no native capture action in this harness.

There are zero Vulkan/synchronization validation failures, and all 94 native
manifest entries (three executables and 91 shader binaries) remain unchanged.
An initial unpaired texture capture is retained separately: its KTX2 data-URI
reference exposed and led to the production parser fix. Six superseded BLEND
captures are also retained: the independent opaque oracle's farther blue
receiver needed enough width to cover the nearer quad under perspective. The
corrected six captures pass stronger full-image checks on the same binary.
These seven captures are excluded from the selected183 count. The earlier
105-capture variant run on the preceding binary is preserved separately.

Dependency pins, decoder-only linkage, optional OFF behavior, upstream notices
and packaging flags are documented in `docs/texture-image-data.md` and
`docs/third-party-notices.md`; package notice selection was checked in enabled
and disabled configurations. #94/#97 scope and acceptance are locally complete,
and the #89 export/#93 compressed-image followups are validated. Issues remain
open pending integration; all changes remain local and uncommitted.

Evidence: `out/feature-parity/texture-variant-validation-checkpoint.json`,
`out/feature-parity/texture-data-uri-cpu-checkpoint.json`,
`out/feature-parity/texture-data-uri-native-manifest.json`,
`out/feature-parity/texture-runtime-matrix.json` (SHA-256
`9dea60f319d6f8083c2f0efa88fa3d46a11cc5e063975d74cea4184b63806248`),
`out/feature-parity/material-variants-runtime-data-uri/results.json`,
`out/texture-disabled-check/results.json`, and `out/instancing-check/results.json`.

## DDS alternate-source follow-up

`MSFT_texture_dds` now routes and validates its alternate image and remaps that
image index on export. The source preference matches the reference renderer:
BasisU, DDS, WebP, core. The new build passed **797 cases / 36 executables** with
all GPU paths enabled, plus **61 cases / 5 executables** with optional codecs
disabled; zero failures or skips. **10 native captures / 8 checks** passed across
both raster techniques, including authored mips and GLB/ASCII reload after DDS
source deletion. All positive full images were pixel-exact; the negative control
had interior MAE 154.667/255. All ten logs were free of VUID/SYNC errors and all
94 frozen executable/SPIR-V entries matched before/after. Renderer SHA-256:
`f6c39cc056b234c0fe3a09d1504afc7f8f773c71edbf004b164b02e4d4933724`.

Evidence: `out/feature-parity/dds-source-validation-checkpoint.json`,
`dds-source-cpu-checkpoint.json`, `dds-source-native-manifest.json`,
`dds-source-runtime/results.json`, and
`out/texture-disabled-check/dds-source-results.json`. Prior 183-capture evidence
remains associated with its earlier binary and is not recounted on this build.

## Next work

The manual texture portability regression is fixed. Recognized encoded
containers remain exact; ordinary formats without a portable MIME signature
retain 8/16-bit pixels and export as PNG. TGA alpha and PSD adjacent 16-bit
values pass, as do GLB/ASCII PNG round trips for one through four channels and
multiple DEFLATE blocks. The full **800 cases / 36 executables** pass with all
real GPU paths enabled and zero failures/skips. Evidence:
`out/feature-parity/portable-image-cpu-checkpoint.json`. This CPU/export follow-up
has its own binary; earlier native captures retain their recorded provenance.

Shared-geometry instancing (#98) is locally implemented and validated: **820
cases / 38 executables**, **63 codec-disabled cases / 5 executables**, and **82
native captures / 88 checks** on renderer
`dea080752e7c846cd09980ed6bc45a31297299c4e66d5bea15bed3fd91392397`.
All positive full images are pixel-exact across both raster techniques, including
static/skin/morph ordinary-node references, inherited visibility, parent/owner
animation, hard shadows/DDGI, and GLB/ASCII reload with animated skinning. All
four instance CPU picks resolve correctly. Source geometry is shared; the skin
source uses one deformation stream versus four in the ordinary-node reference.
Six TLAS placements share two static BLAS or three deformed-owner/receiver BLAS.
All 82 process logs have zero VUID/SYNC errors and all 94 frozen native entries
remain unchanged. Four diagnostic captures with a helper assertion or differing
selection-overlay oracle are excluded and recorded. Singular instance transforms
retain an explicit preflight baking limitation; GPU picking readback has no
native capture action. See [mesh instancing](gltf-instancing.md) and
`out/feature-parity/instancing-validation-checkpoint.json`.

The user-reported selection-outline defect is corrected. Radial vertex
extrusion produced 5-pixel horizontal edges and 2-pixel vertical edges in the
reported scene. Eight constant screen-pixel offsets dilate the existing stencil
silhouette and retain its interior mask. **Eight native captures / eight checks**
measure exactly **5 pixels on all four sides**, including the original scene,
wide/portrait/square windows, different depths, nonuniform scale and 4x MSAA.
The focused rendering convention and overlay suites pass; native logs have no
VUID/SYNC errors and the separate 94-entry manifest remains unchanged. Evidence:
`out/feature-parity/selection-outline-validation-checkpoint.json`, renderer
`ba8a5c7a57a4a310f9c4e135a0d26be2cdf0808ab8546e7bea61ee4cbaa1eca1`.

Continue #99 multi-scene state and the remaining P2/P3 issues.
The first runtime multi-scene checkpoint now passes **860 cases / 39 executables**
and **42 native captures / 34 checks** in both raster techniques. Default/CLI
selection, repeated distinct/shared/empty views, one graph and shared geometry,
active light identities, temporal resets, scene metadata/global forest and every
scene through GLB and ASCII reload pass; positive full images are pixel-exact.
All 30 distinct native logs have zero VUID/SYNC errors and the 94-entry manifest
is unchanged. Seven additional outline captures still measure five pixels on
every side. Evidence: [scene selection](gltf-scenes.md) and
`out/feature-parity/scenes-fresh-validation-checkpoint.json`, renderer
`bd7bdabd9980fd586bcb8497dcdee6a8bdd52cc5688189eba3b205851c1b87d3`.
Deformed/ray/GI and live interaction witnesses remain next; #99 is not complete.
The instancing framing-bounds follow-up passed **830 cases / 39 executables**
with all real GPU paths enabled and **42 native captures / 44 checks**, including
both raster techniques, portable reloads, hidden owners and hard-ray/DDGI
witnesses. Global bounds agree with repeated-node references within 1e-5;
positive full images are pixel-exact. Evidence:
`out/feature-parity/instancing-bounds-validation-checkpoint.json`, renderer
`dfa4f5e5df64f74f9dd05106cc1f3e471f14ccfa38173912818516418dcf7c5c`.
That build also validates the pure multi-scene catalog, active hierarchy/UI and
deformation-view foundations. Runtime scene selection and all-scenes export
integration are in progress and have not yet passed application validation.
Custom material baking, post-skin edits requiring baking and unsupported Object
Model pointers retain explicit diagnostics. The active goal covers the entire
tracker. All changes remain local and uncommitted; implementation issues remain
open.

## Multi-scene completion checkpoint (#99)

#99 scope and acceptance are locally complete with documented limits. The native
state, deformation and ray/GI coverage below supersedes the earlier pending
multi-scene validation notes. The issue remains open pending integration, and the
full #121 goal continues with the remaining features.

Renderer `370f33bb103096889c88f312f57c1dd2a3f7a3122ead15b8db4bd19268ed4914`
has **866 passing cases / 39 executables**, all four real Vulkan paths enabled,
zero failures/skips, and **116 selected native captures / 92 checks**:

- The **12-capture / 12-check** state matrix proves that scene changes pause a
  playing clip at its existing .5 pose, clear authored-camera selection and edit
  history, and retain variant selection plus an edited mesh transform. The final
  edited pose/look and animated all-scenes GLB reload are pixel-exact against an
  independent baked reference in both raster techniques.
- The **104-capture / 80-check** settled matrix covers skin/morph owners, shared
  source geometry, repeated A/B/Both/Empty views, CLI selection, production ray
  instances and DDGI publication. Its 72 positive image comparisons are all
  pixel-exact; eight wrong-scene controls differ by at least 11.4129/255 mean
  error. Forty-eight nonempty samples have settled submitted deformation history
  and zero pose delta; 16 empty samples release deformation. Twenty-eight
  nonempty ray samples publish TLAS/BLAS and execute DDGI, and eight empty samples
  clear ray instances and DDGI work. DDGI intensity is zero in these deterministic
  oracle runs, so they prove execution/publication rather than a visual GI effect.

All 46 distinct logs are free of VUID/SYNC errors, and all reports verify the same
94-entry native manifest before/after execution. Raw XML independently totals
866 tests. The test run precedes a telemetry-only history-depth field addition;
unit executables are unchanged and the native state matrix verifies that field.
Evidence: `out/feature-parity/scenes-adapter-cpu-checkpoint.json`,
`scenes-adapter-tests/*.xml`, `scenes-state-validation-checkpoint.json`,
`scenes-deformation-validation-checkpoint.json`,
`scenes-state-runtime/results.json`,
`scenes-deformation-settled-raster-runtime/results.json`,
`scenes-deformation-settled-ray-runtime/results.json`, and
`scenes-state-native-manifest.json`.

The earlier **42 captures / 34 checks**, including every scene through GLB and
ASCII reload, retain renderer
`bd7bdabd9980fd586bcb8497dcdee6a8bdd52cc5688189eba3b205851c1b87d3`
and their separate static checkpoint/manifest. They are not recounted on the
current build. The original unordered-light publication diagnostics, 13 captures
with an overly strict history-reset assertion, and a superseded 52-capture
immediate-switch raster matrix remain excluded. The latter matrix had exact
pixels/zero pose delta; the selected matrix additionally proves settled history.

Admission rejection is covered by transactional pure preflight tests, with no
native UI rejection action. GPU picking readback remains unavailable to the
capture harness. Physics keeps paused bindings and blocks Play/Step for inactive
primary bodies. Exporting a manual sun that replaces a shared authored sun gives
an explicit error requiring a separate source node. See
[scene selection](gltf-scenes.md) for publication, editing and all-scenes export
contracts. All changes remain local and uncommitted.


### Node flags implementation checkpoint — 2026-10-25

#100 now imports strict bounded local visibility/selectability/hoverability metadata before resource I/O, applies independent graph inheritance, routes CPU and GPU nearest hits through the same ancestor helpers, exposes editor controls and grouped undo/capture edits, and preserves authored presence/defaults/extras/unknown keys in GLB/ASCII. Visibility includes attached lights and retains cameras. Stable light-binding export prevents hidden/reordered lights from aliasing edits, preserves hidden-light edits/ranges and all authored suns, and uses bounded iterative deep-forest walks. Pure scene extraction uses the asset budget rather than the active draw budget.

Renderer `71e1b6327cde69f39ce1581b15169fd64c21df27279888ca59f3a409aa481b39`: **904 cases / 40 executables**, all four real-Vulkan paths enabled, zero failures/skips. **52 native captures / 48 checks** pass across both raster techniques, with and without hard-ray/DDGI: all 44 positive comparisons are pixel exact; four negative controls have mean error at least 6.1102/255. Captures verify independent same-hit selection/hover routing and blocked roots, hidden mesh/light restoration, camera availability, grouped undo/redo and both-format metadata. Twenty distinct logs have zero VUID/SYNC errors; every 94-entry native manifest matches before/after. Twenty-two ray states publish expected TLAS4/5 placements and DDGI27probes/108rays; intensity0 explicitly proves execution/publication only.

Reference-style ancestor routing is an approximation: trace-through, behavior-graph events and boolean animation-pointer channels are not implemented. GPU picking readback has no capture action. Selection and hover capabilities remain Partial. Inactive-scene capacity and live scene/mode switching are separate follow-ups underway. Evidence: `docs/gltf-node-flags.md`, `out/feature-parity/node-flags-validation-checkpoint.json`, `node-flags-cpu-checkpoint.json`, `node-flags-native-manifest.json`, `node-flags-runtime/results.json`, `node-flags-ray-runtime/results.json`. Changes are local/uncommitted; issues remain open pending integration; tracker #121 remains active.


### Node flags completion and scene-capacity follow-up — 2026-10-25

The frozen `71e1b6327cde69f39ce1581b15169fd64c21df27279888ca59f3a409aa481b39` build now passes **83 native captures / 84 checks**, in addition to the **904 cases / 40 executables** checkpoint. This includes the prior node-flag raster/ray52/48, live scene+renderer switching13/14, oversized inactive-scene save/reload6/10, and existing multi-scene animation/camera/history/material state12/12. All60 positive full-image comparisons are pixel exact. Two expected CLI attempts to activate4097draws under the4096active limit fail with the correct diagnostic; both portable formats still retain that inactive scene, flags, metadata and light ranges. Forty-two distinct logs have zero VUID/SYNC errors and every94-entry manifest matches. No diagnostic captures are counted.

#100's local flags, editor controls, ancestor routing, mode/scene preservation and portable/history behavior are validated. Behavior-graph events depend on #103; trace-through and boolean animation-pointer channels remain explicit limits. The selection/hover registry remains Partial. Evidence: `out/feature-parity/node-flags-completion-checkpoint.json`, `node-flags-scene-switch-runtime/results.json`, `inactive-scene-capacity-runtime/results.json`, `node-flags-scenes-state-runtime/results.json`, `docs/gltf-node-flags.md`. Changes remain local/uncommitted and issues stay open pending integration. Continuing #92 anisotropy/diffuse-transmission; its parser and independent numerical references are isolated foundations, not yet renderer support. Tracker #121 remains active.

### Extended materials completion checkpoint — 2026-10-25

#92 now imports strict anisotropy and diffuse-transmission factors/textures,
preserves per-slot source/sampler/UV metadata through animation and portable
export, and evaluates shared layered terms in forward, deferred, transparent
and DDGI hit shading. Deferred coverage uses two lazy auxiliary outputs, global
availability and premultiplied diffuse storage for MSAA resolve. Dynamic
rendering now synchronizes implicit depth/stencil STORE and DONT_CARE writes.
Both registry entries are Partial; masking/NDF caps, bent IBL, resolved-material
MSAA and mixed stochastic dielectric limits are documented in
[extended materials](gltf-extended-materials.md).

Renderer `5b3f617a1bf9ddbc2a295cb09e88d2102a2a86e54b05a46fa0a091bde5787b43`
passes **1056 cases / 49 executables**, with nine real GPU suites and zero
failures/skips. Independent references include 24 Python tests, 83 GPU math
points, 11 actual 4x resolve cases and two GPU depth/stencil tests covering
seven store combinations. **132 selected native captures / 92 image checks /
16 execution checks / four export inspections** pass on the same binary.
Of 52 equivalent-image comparisons, 47 are pixel exact and five differ by at
most one byte. Both raster techniques pass backside raster/hard-ray occlusion,
texture/UV/mirrored-frame controls, transparency, environment/area lighting,
GPU morphing and animated GLB/ASCII round trips.

Sixteen DDGI states publish three instances, 27 probe updates and 216 rays per
update with committed history and timed trace/update passes. Fourteen positive
intensity samples and two zero-intensity controls prove visible GI in both
techniques, with receiver ROI mean differences above 125/255. The 3x3x3 lattice
is explicitly authored; no automatic-fit or atlas-readback claim is made.
All 132 distinct native logs are strict-validation clean and all 100 frozen
inputs match. Earlier #92 diagnostic captures are excluded; the strict log
audit also confirms the prior #99/#100 selected logs remain clean.

Evidence: `out/feature-parity/pbr92-completion-validation-checkpoint.json`,
`pbr92-depth-store-validation-checkpoint.json`,
`pbr92-depth-store-native-manifest.json`, `pbr92-strict-log-audit.json` and
the four selected native reports listed in the completion checkpoint.
Changes remain local/uncommitted and issues stay open pending integration.
Continuing #106 merge/import: its detached CPU planner passes 30 private tests;
source/runtime identity, staged resource ownership and capture integration are
under development, uncompiled and excluded from the #92 binary evidence.
The full tracker #121 remains active.

### Merge foundations and outline follow-up — 2026-10-26

#106 now has a bounded detached resident merge planner (33 cases), explicit
source/runtime material and primitive identities, prepared graph append with
stable identity and earlier history, playback-preserving animation source
extension, and scoped texture batches/leases. Procedural runtime slots remain
explicitly unmapped. Canonical incoming scenes and selected placement/joint
clones retain separate source identities while sharing geometry resources.

The coordinated checkpoint passes **1251 cases / 56 executables**, with nine
real Vulkan GPU suites and zero failures, disabled cases or skips. The texture
ownership suite passes 15 cases, including rollback, stale leases, final-reader
retirement and throwing/cancelling progress callbacks. Two initial test-only
host-event gates triggered modern Vulkan validation; they were replaced with
enabled timeline semaphores and that suite was rebuilt and rerun successfully.
The other 55 suites passed unchanged. Evidence:
`out/feature-parity/merge106-foundation-validation-checkpoint.json` and its
explicit copied XML set. Live merge publication, complete resource generations,
history/UI/export integration and native merge validation remain pending.
Capture merge requests fail explicitly until integration exists; no #106
acceptance box is complete.

Renderer `5b9bd4e98e239e71e98c0a658bd90101de71e2473266d8896feb044536f2200c`
also fixes the omitted forward mesh-selection outline by reusing the common
pixel-based stencil pass. **14 native captures / 14 checks** across forward and
deferred raster measure exactly five pixels on all four edges, including
aspect-ratio, distance, nonuniform-scale and MSAA4 controls. All 14 logs are
strict-validation clean and all 101 frozen inputs match. Evidence:
`out/feature-parity/selection-outline-both-runtime/results.json` and
`merge106-foundation-native-manifest.json`. These captures are separate from
the earlier #92/#99/#100 matrices. Changes remain local/uncommitted; #106 and
the full tracker #121 remain active.

### Merge composition and resource generations — 2026-10-26

#106 now composes one authoritative resource archive with the current evaluated
source, source-ordered deformation/instancing, runtime geometry with procedural
gaps, and a detached graph. Incoming implicit materials have an independent
glTF default; old edits, variant frames, optional scene defaults and per-scene
root order survive. Graph append and tail undo/redo preserve live identity,
restore exact active views, evaluate stale retained transforms during preparation,
and publish without allocation. Self-contained history commands skip redundant
post-publication refresh.

Detached SceneManager, SceneController and LightingManager generations own fresh
buffers and descriptor pools while retaining old resources for reversible
exchange. World, controller and manager object addresses stay stable. Geometry
and object-buffer readbacks verify actual GPU bytes, including diagnostic tails,
initial arena replacement, procedural resize and upload-error rollback. Separate
ray generations, prepared DDGI reset, paired provider/registry exchange and staged
command retirement support the remaining coordinator work. Earlier texture and
source helpers remain in the validated set.

The renderer builds and the selected final suite set passes **1403 cases / 66
executables / 13 real Vulkan GPU executables**, with zero failures, disabled
cases or skips. Initially 64/66 executables passed; two test-device fixtures
omitted production-required Vulkan feature enables. Enabling runtime descriptor
arrays in the DDGI fixture and buffer device address/synchronization2 in the
lighting fixture made both rebuilt suites pass with validation unchanged.
No production change followed the full run. Initial failing logs/XML are retained
separately. Evidence: `out/feature-parity/merge106-generation-validation-checkpoint.json`
and its explicit 66-suite XML set.

The final renderer `d9f87c6087b9ed3fe099e61d43c650f35fe9c2db630278540d6bb6fa0ec2f3f4`
also passes **six fresh native outline captures/checks**, covering wide, portrait
and MSAA4 in both raster techniques. Every left/right/top/bottom stroke measures
exactly **five pixels**; all six logs are strict-validation clean and all 101
frozen executable/SPIR-V inputs match before/after. Evidence:
`out/feature-parity/selection-outline-generation-runtime/results.json` and
`merge106-generation-native-manifest.json`. Earlier outline cohorts retain their
own binary provenance.

**Live frontend merge publication is still pending.** GUI/capture actions,
coordinated history/resource publication, culling/auxiliary resource staging and
the native merge/export/reload matrix are not yet complete. Capture merge still
fails explicitly; no #106 scope or acceptance box is claimed complete. Changes
remain local/uncommitted and the complete tracker #121 goal stays active.

### Live import/merge completion — 2026-10-26

#106 is now implemented and validated locally. GUI Import / Merge glTF and typed capture requests use the bounded CPU worker, preserve authored placement precision, and publish the complete graph, asset, controller, lighting, culling, ray, DDGI, auxiliary and provider generation together. Existing scene content, graph identities, selections and earlier edit history survive. Undo/redo retains asset leases without rereading source files and restages frame-dependent resources against the current swapchain.

Renderer `2a2f048434dcfd09ab7a723bf524d379bf76f7e97c45ce98d9ba27f858db5826` passes **1421 cases / 68 executables / 14 real Vulkan GPU executables**, zero failures, disabled cases or skips. **124 fresh native captures** pass across both raster techniques: static merge48, dynamic merge34, resource/publication36 and outline6. They include 50 static image checks, 32 session checks, 48 dynamic/oracle checks, 64 grouped resource checks, six portable export inspections and six outline checks. All **56 distinct process logs** are strict-validation clean and all **112 frozen runtime/harness inputs** match before/after. Earlier diagnostic cohorts retain separate provenance and are excluded.

The native matrix covers independently indexed colliding textures/materials, two retained imports, previous visibility history, missing/malformed/capacity failures, exact GLB/ASCII portable reloads, morph+skin and mirrored GPU instances, retained animation/camera/light state, variants, saved-pose exports and restored authored animation channels. Resource witnesses cover GUI progress, hard-ray shadows, real DDGI work, frame-resource rebuilds and the first extended-material import. Eighteen same-composition comparisons independently verify unchanged measured VMA allocation counts/bytes, with no shape skips; sixteen history/read checks verify failed imports do not publish or erase history and redo performs no source reread.

The yellow-outline follow-up measures **5 pixels on left, right, top and bottom** for wide, portrait and MSAA4 captures in both renderers. The pixel offsets use the actual framebuffer width and height independently.

Evidence: `out/feature-parity/merge106-completion-validation-checkpoint.json`, `merge106-final-test-results.json` and its explicit copied XML set, `merge106-final-cohort-manifest.json`, `merge106-ownership-checks.json`, and the static/dynamic/resource/outline cohort reports referenced by the checkpoint. The exact executable is preserved in `out/feature-parity/validated-binaries/`.

Limits: settled VMA equality does not prove complete driver-object/descriptor-pool leak freedom; direct GPU pick readback and frontend allocation-failure injection were not exercised. Native live playback is held at speed zero; advancing authored animations is covered by reloads and existing animation/deformation tests. Intensity-zero DDGI witnesses prove execution/publication, not visible indirect illumination. These instrumentation limits do not represent missing merge behavior. All changes remain local/uncommitted and issues stay open pending integration. #107 rename and empty-node creation are now being implemented; their later source changes are excluded from this build. The full tracker goal remains active.

### #107 rename and empty-node milestone — 2026-10-26

Rename and empty-child creation now work through hierarchy controls, typed capture actions and edit history. Imported primitive selections resolve to their stable source owner. UTF-8 labels are validated before replacing a pending request; delayed requests cannot modify a replacement graph. Empty nodes retain graph identity, exact optional active views and selected-scene membership through undo/redo and later scene switches. Scene root registration uses detached, lifetime-bound receipts and publishes after the matching graph proof.

Renderer `7d4e555bdda0584106af1f28a5bd810fc7d982ebcb3c6c55f4aeca4bdb726b2b` passes **42 native captures / 42 state checks / 40 image checks / eight portable export inspections** across both raster techniques. All 16 process logs are strict-validation clean; all 117 frozen executable, SPIR-V, DLL and Python inputs match. Six further outline captures on this same binary measure exactly **5 pixels on every edge**, including wide, portrait and MSAA4.

The first coordinated run passed **70 of 71 executables (1461 cases in passing suites)**, including the real scene-manager membership tests. The remaining 14-case clone-planner executable had five fixture failures: missing animation accessor bounds and nonfinite equality assertions. Test-only repairs are prepared for the next build; this checkpoint does not claim a fully passing 71-suite run. One initial native cohort exposed lazy no-UI history fields omitted before any edit; its verifier was corrected with explicit baseline/fresh-reload guards and mutation checks. That diagnostic cohort is excluded; all seven of its captured states also passed offline review.

Evidence: `out/feature-parity/node-authoring-rename-empty-validation-checkpoint.json`, `node-authoring-initial-test-results.json` and its copied XML set, `node-authoring-verified-runtime/results.json`, `selection-outline-node-authoring-runtime/results.json`, and `node-authoring-native-manifest-v2.json`.

**#107 remains in progress.** Subtree duplication and deletion are not complete in this binary. The bounded clone planner, clone-specific animation extension, stable-slot retirement proofs and export compactor are being integrated; their later source changes are unbuilt and excluded from this native evidence. Changes remain local/uncommitted; the issue and tracker remain open.

### #107 subtree deletion milestone — 2026-10-26

Subtree deletion now uses stable retired graph slots, with exact hierarchy/selection/history restoration and shared mesh/material ownership retained. Retired content is excluded from active scenes, draws, deformation, authored lights/cameras, interaction, physics attachment and export. Portable export compacts node references only after light overlays; retained skin dependencies reject before mutation. Deletion pauses animation; undo/redo restores the deletion-time global pose and remains paused.

Renderer `e2ac2181a6932ee8a3b35d828411eaf3108edd82ecd44408efa2e1227951bb19` passes **1523 cases / 72 executables / 14 real Vulkan executables**, with zero failures or skips. Fresh deletion validation passes **52 captures / 50 state checks / 42 image checks / four portable export inspections** across both raster techniques. Eight further captures verify actual hard-ray TLAS changes and timed RayShadowTrace, DdgiTrace and DdgiUpdate execution through delete/undo/redo. Two retained-skin processes reject for the expected guard; all process logs remain strict-validation clean.

The outline follow-up passes **14 fresh captures**, measuring exactly **5 pixels on all four edges** for landscape, wide, portrait, square, nonuniform scale, distance and MSAA4 in both renderers. Together the current-binary cohorts contain **74 captures / 30 distinct strict-clean logs**. The 117-file deletion and 114-file outline manifests match before/after, and the executable is preserved.

Evidence: `out/feature-parity/node-retirement-validation-checkpoint.json`, `node-retirement-repaired-test-results.json` and its copied XML, `node-retirement-e2ac-verified-runtime/results.json`, `node-retirement-e2ac-ray-runtime/results.json` and `selection-outline-e2ac-runtime/results.json`. The initial animation fixture incorrectly expected four deformed vertices despite its missing tangent requiring six triangle corners. That failed cohort is preserved; all 25 available captures passed independent offline review after the narrow verifier repair, and the acceptance evidence uses fresh cohorts.

Limits: retirement refresh follows history publication rather than a full GPU generation transaction; native guards cover retained skins, while broader physics and allocation-failure cases remain CPU-tested or unexercised. Intensity-zero DDGI proves execution, not image quality. **#107 remains in progress for subtree duplication.** Graph-driven clone helpers, projected animation, resource-sharing generation and UI/capture integration added after this executable are unbuilt and excluded from this evidence. All changes remain local/uncommitted; issue and tracker remain open.

### #107 subtree duplication backend checkpoint — 2026-10-26

Subtree duplication now copies the actual edited graph: nested empty/procedural nodes, reparented primitives, GPU instances, node flags and names. Mesh/material identities and texture leases remain shared. External skin/joint dependencies receive nonvisual contexts; originals, unrelated scene content and clip channels remain intact. Generation history restores each side's animation clock/pose, selection and exact scene-root membership. Primitive creation and reparenting now own reversible membership receipts, including deletion and reused-tail-ID cases.

Renderer `9685f53e466674ef6cd734c0002b2ddc8048e6ea15c45b893e12d0fcdff5c4c9` passes **1610 cases / 75 executables / 14 real Vulkan executables**, zero failures/skips. Native duplication passes **152 captures / 120 state checks / 116 image checks / eight portable export inspections** across both renderers. Two additional moved-skin export attempts reject for the existing explicit baking guard; live bindings still pass. Eight captures verify actual positive RayShadowTrace/DdgiTrace/DdgiUpdate GPU timestamps and advancing content through clone/undo/redo.

Fourteen further outline captures again measure exactly **5 pixels on all four edges**. The combined checkpoint contains **166 captures / 68 strict-clean logs**. Evidence: `out/feature-parity/node-clone-backend-validation-checkpoint.json`, `node-clone-repaired-test-results.json` and copied XML, plus its five clone and outline reports. The exact executable is preserved. An initial seven-reference animation run stopped before its live process on a harness filename collision; it is excluded, and the original script/report are archived. Repaired acceptance used fresh outputs.

**#107 remains in progress:** true inline hierarchy rename and regression checks of earlier authoring/import workflows are pending. Later inline UI source edits are excluded from this binary. Native animation uses explicit seeks at speed zero; deformation samples are CPU bound-stream telemetry; intensity-zero DDGI proves execution, not image quality. All changes remain local/uncommitted; issue and tracker remain open.

### #107 authoring completion — 2026-10-26

#107 is implemented and validated locally: empty-node creation, true inline hierarchy rename (F2/context), subtree duplicate/delete, selection/history and portable serialization. Inline input submits one validated request; Escape/blur cancels. Stale graph/topology/name/view changes cancel the draft, animated transforms remain live, imported primitive selections edit their stable owner, and a 10,002-row hierarchy stays clipped. Physics/product/joint dependencies reject before mutation rather than leaving broken references.

Final renderer `a0b090fcc2839eb311d079f70f740481ba887d65212393378aeb9dd0c16940e9` passes **1617 cases / 75 executables / 14 real Vulkan executables**, zero failures/disabled/skips. This includes all 36 hierarchy tests and seven new real ImGui interaction cases. **14 fresh outline captures on this final binary measure exactly 5 pixels on every edge** in both renderers, including wide/portrait, distance, nonuniform scale and MSAA4.

The preserved earlier backend `9685f53e466674ef6cd734c0002b2ddc8048e6ea15c45b893e12d0fcdff5c4c9` passes the **386-capture duplication/outline/earlier-authoring/deletion/import matrix** with 150 strict-clean logs. Its import audit verifies 18 settled VMA equalities and 16 history/read checks. Together the two explicitly versioned builds provide **400 accepted captures / 164 strict-clean logs**; these are not claimed as one binary. Initial filename-collision diagnostics and default-deferred import subsets are preserved and excluded.

Evidence: `out/feature-parity/node-authoring-completion-validation-checkpoint.json`, `node-inline-rename-test-results.json` and copied XML, `selection-outline-a0b0-runtime/results.json`, and the linked backend/prior-workflow checkpoints. Both executables are preserved. Existing moved-skin baking-required export guards and the bounded animation/physics/GPU instrumentation limits remain explicit in those checkpoints.

Changes remain local/uncommitted; issue and tracker stay open pending integration. Continuing **#108 material/texture authoring**; its isolated pure planner is source-only, unregistered/unbuilt and excluded from this completion. The full tracker goal remains active.

### Continuous selection stroke correction — 2026-10-26

The yellow outline now fills a complete five-pixel disk around the selected silhouette. Eight boundary translations had left gaps on thin geometry: a preserved one-pixel native baseline missed 1,128 required stroke pixels. Eighty nonzero integer offsets now form a continuous opaque stroke, batched as instances with explicit object-group indexing and checked whole-object overflow splitting. The stencil mask and shared forward/deferred implementation remain in use.

Renderer `752878943d80ea2517fccd9bd561697cb22463caadd14a388a2badb80d7f2759` passes **30 fresh Vulkan captures / 30 strict-clean logs**: twelve complete-stroke pixel checks, fourteen equal-width checks and four selected-node/instance isolation checks. Every stroke comparison has zero pixel mismatches; every measured horizontal/vertical edge is exactly **5 pixels**. Both rendering modes cover thin bars, rings, corners, sloped geometry, portrait/wide extents and MSAA4. The 116 executable/SPIR-V/DLL/Python inputs match before/after. This is one application with two raster rendering modes, sharing the corrected outline code.

**367 cases / nine CPU test executables pass**, with zero failures/disabled/skips. Evidence: `out/feature-parity/selection-stroke-validation-checkpoint.json`, `selection-stroke-repaired-test-results.json` and copied XML, and the three `selection-stroke-7528-*-runtime/results.json` reports. The exact renderer is preserved. The geometric stencil policy is unchanged; native isolation verifies source/graph identities and pixels rather than instrumenting the exact object-buffer row. Selected vertex work increases from eight to eighty translations; CPU draw submissions are batched, while large-selection GPU cost has not been benchmarked.

### #108 detached authoring foundations — 2026-10-26

The material planner is now registered/built and has immutable proof validation, bounded nonowning source views and current-clock edit rebasing. Player preparation preserves clips, playback controls and unrelated rest values; material-only evaluation samples active tracks without rewinding other animation. Untracked duplicates keep their evaluated creation appearance. Canonical frame templates reuse unchanged UV policy and replace tangents on a normal-map UV switch, preserving deformation mappings and explicit canonical-topology export rules.

**62 focused CPU cases pass** (32 planner/proof/rebase, 14 player/material-animation, 16 frame-policy), within the 367-case related run above. Evidence: `out/feature-parity/material-authoring-foundation-validation-checkpoint.json`. One initial duplicate-clear test had the wrong expected rest value; its raw failure is preserved, and the narrow test repair now asserts the trusted frozen creation value by exact bits, original tracked-rest restoration and exclusion of the untracked ID from dirty materials. Production was unchanged by that repair.

**#108 remains incomplete:** renderer/SceneManager publication, classification, UI/gesture/history integration, assignment/resource lifecycle, texture/sampler editing and per-binding runtime/export integration remain. Actual Mikk/evaluator work requires a separate budget, and legacy textured-zero shader promotion needs correction with GPU coverage. The thirty native captures above validate the outline, not material authoring. All changes remain local/uncommitted; the full tracker goal stays active.

### #108 material conversion and factor evaluation — 2026-10-26

Detached single-material conversion now validates one bounded source record and reconstructs all 23 texture slots from explicit source-indexed color/data roles. It retains source identity when GPU handles alias, resets removed extensions, and rejects referenced roles that need upload. Scene loading, reload, merge and clone retain the role tables, including archive-only suffix holes. The initial importer now follows glTF's omitted-metallic default of one. Forward/deferred shaders, DDGI arithmetic, transmission classification and export preserve literal zero/black factors; a texture does not enable a disabled layer. A positive transmission factor of 0.0005 still selects transparent routing.

Renderer `eff1443916fdfb1fd5498da8e362bf0ef6eb3915ad28443bba804c4216188e88` passes **1709 cases / 79 executables / 14 real Vulkan executables**, with zero failures/disabled/skips. The initial run had one new constant-irradiance assertion fail by one binary32 rounding unit; only that ten-case GPU suite was rebuilt and rerun with bounded literal RGB comparison, retaining exact eligible-route equality, coverage and rejected-zero checks. The other 78 passing suites were retained. Original XML and build/link diagnostics are preserved; pure material arithmetic now lives separately from scene-graph animation code.

Fresh native validation passes **58 captures / 86 checks / 16 portable GLB inspections / 58 strict-clean logs** across both raster modes: 38 textured-zero/black and positive-control captures, six omitted-metallic/default controls, and fourteen outline-width captures. Zero-factor images match independent untextured references exactly; positive controls visibly differ. Every measured outline edge remains exactly five pixels, including MSAA4. All 145 frozen executable/SPIR-V/DLL/Python/source inputs match before/after, and the renderer is preserved. The old 7528 renderer reproduced incorrect textured-zero transmission classification in a separate two-capture negative control, excluded from acceptance.

Evidence: `out/feature-parity/material-zero-conversion-validation-checkpoint.json`, `material-conversion-zero-repaired-test-results.json` with copied XML, and the three native `results.json` reports referenced by the checkpoint. GLB inspections check serialized bound material factors and texture presence; reload rendering is outside this cohort. Native factor witnesses use isolated direct lighting and MSAA1; actual DDGI arithmetic/receiver behavior is covered by the GPU contract tests.

**#108 remains in progress:** material inspector/gesture/history, transactional live GPU publication, assignment/duplication, texture/sampler lifecycle and per-binding runtime/export consumers remain. The converter and retained identities are foundations for that integration. The separate 7528 continuous-disk/binding oracle remains versioned evidence; this new executable's fourteen outline captures check widths. Changes remain local/uncommitted; issue and tracker stay open.

### #108 staged material publication components — 2026-10-26

Selected-material preparation now owns a fresh packed table, material buffer and independent primary/auxiliary descriptor sets. Publication updates the authored archive, current-clock evaluated record, runtime material, export metadata/bindings and prepared animation player together. Existing geometry/resource tables remain resident. Undo preparation uses the current clock and preserves unrelated evaluated values and original curves. Declarations remain monotonic when an extension is undone.

Object-only routing stages opacity, alpha mask, sidedness and material flags while preserving object order, geometry/deformation pointers, World and geometry revisions. Separate light-only ray descriptor staging retains lighting buffers and CPU light state. Tokens reject stale/foreign/consumed generations; full-generation exchanges preserve the new object ownership. A strict typed material-edit capture payload/schema covers all 32 numeric properties and 13 extension toggles, default resets, zero/black/false values and gesture phases.

**431 cases / nine executables / three real Vulkan executables pass**, with zero failures/disabled/skips. The run includes 17 new Vulkan staging cases, eleven parser cases, an immediate-before proof case and three allocation-hook cases. Actual Windows C++ allocation denial surrounds selected-record/player publication and observes zero attempts. A positive control confirms that ordinary `std::swap(tinygltf::Material)` allocates MSVC map sentinels despite its declared `noexcept`; publication uses scratch materials constructed during preparation and move assignment. CPU/descriptor budgets are checked before detached allocation and descriptor scratch reserves exact capacities.

Evidence: `out/feature-parity/material-publication-stages-validation-checkpoint.json`, `material-publication-stages-test-results.json` and copied XML. This build compiles/tests the staged components; the application executable remains the validated `eff14439...` binary and has not been relinked. GPU fixtures verify buffers, descriptor staging, ownership and publication; rendered material-edit consumption remains a native integration gate.

**#108 remains incomplete:** the frontend transaction coordinator, inspector, history and resource lifecycle are still being connected. Capture parsing is tested but the application does not dispatch the new action yet. Live slider history also needs a proved coalescing command that retains raw metadata and intermediate whole-extension ownership; reconstructing only the final numeric patch would lose those guarantees. All changes remain local/uncommitted.

### #108 frontend integration compile and focused validation — 2026-10-26

The application now dispatches typed material capture edits and connects a selected-surface material inspector, guarded ordered UI requests, current-clock undo/redo and flat proved slider coalescing. The coordinator stages selected material data/descriptors, routing, auxiliary PBR activation, ray/light bindings, DDGI resets and provider/runtime bindings before publication. A read-only cache-authority query replaces an unconditional ECS rebuild; rejected new Begin/Commit preparation preserves the preceding pending gesture. Material sampling restores tracked leaves from current authored rest before applying the selected clip, including when a clip has been cleared, while retaining untracked evaluated values.

The main renderer and ten focused targets compile successfully. Renderer `abec06e916e6e0878ca9bb74c8410589d0151b02c32032257cd2afc856c253a4` is preserved locally. **458 cases / ten executables / three real Vulkan executables pass**, zero failures/disabled/skips. The suite includes nine new coalescing cases, four current-clock replay cases and fourteen material GUI cases, eight using real ImGui input. Existing routing tests additionally assert current/stale cache authority and unchanged ECS/revisions during staging. Evidence: `out/feature-parity/material-authoring-integration-validation-checkpoint.json`, `material-authoring-integration-test-results.json` and copied XML.

**Native live-edit rendering and save/reload acceptance have not run on this executable.** The new frozen harness has 27 self-checks and five generate-only cohorts; those are not rendered acceptance. Shared edits, current-clock history, gestures, first auxiliary-pass activation, ray/DDGI execution and portable save/reload remain the next native gate. Texture replacement/sampler editing, material assignment/duplication/resource lifecycle and other #108 scope remain incomplete. Issue/tracker boxes remain unchecked and all changes remain local/uncommitted.

### Full editor design documentation — 2026-10-26

`docs/ui-ux-design-guide.md` records the full feature-preservation map, workspace composition, visual tokens, interaction/accessibility contracts, capability statuses and migration checklist. Per the user's explicit update, the UI direction uses Dear ImGui docking and GLFW with optional permissively licensed libraries instead of Qt. The older player/editor plan now agrees. This is design guidance; it does not claim the proposed shell or unavailable authoring tools are implemented.

### #108 existing-source live material edits validated — 2026-10-26

Preserved renderer `abec06e916e6e0878ca9bb74c8410589d0151b02c32032257cd2afc856c253a4` now passes **1768 cases / 80 executables / 14 real Vulkan executables**, with zero failures/disabled/skips. All related executables were rebuilt and rerun after frontend integration. This includes the material Inspector's fourteen cases, eight with real ImGui input, as well as current-clock sampler, proved gesture composition, selected-record publication and no-op cache-authority coverage.

Fresh native runs across forward and deferred pass **104 captures / 168 checks / six portable export inspections / 36 strict-clean logs**, covering shared factors, alpha mask/blend and sidedness routing, literal textured-zero and positive transmission, first deferred anisotropy auxiliary-pass execution, unlit transitions, current-clock undo after seek/clip clear/reselect, gesture End/Cancel and intermediate extension removal/recreation, GLB/ASCII portable reload, actual ray/DDGI dispatch and invalid-request diagnostics. The six exports are four GLBs and two ASCII glTFs. All **181 frozen executable/SPIR-V/DLL/Python/source inputs** verify before/after and after the broad rebuild/tests; generated fixtures and portable resources are separately audited by each harness report.

The first native run stopped after nineteen captures because its export checker required an explicit `alphaMode: OPAQUE`, which TinyGLTF validly omits. Both preserved exports retained the intended values. A serialized-core-only default adapter fixed the checker; strict live authored/evaluated records, extension leaves, texture container identities and opaque metadata remain checked. Forty-two harness self-checks pass, including fifteen added default-omission/negative cases. The original failed result, V1 harness and manifest remain preserved; all five cohorts reran freshly with V2. Production did not change for this correction.

Evidence: `out/feature-parity/material-authoring-live-edit-validation-checkpoint.json`, `material-authoring-integration-broad-test-results.json` with copied XML, and `material-authoring-abec06e9-native-results.json` referencing all five native reports. Authored/evaluated records and the packed-entry hash are CPU evidence, not direct GPU buffer readback. The ray/DDGI cohort uses zero GI intensity to prove execution, epochs and resource continuity; it does not establish indirect-light image accuracy. Invalid requests establish explicit rejection and actual before-captures only, not an uncaptured postfailure state.

**#108 remains incomplete and open.** The next implementation is real Assign existing / Make unique, including implicit/runtime materialization, variant growth and append/assignment history. Texture import/replacement, sampler editing, reference-safe material/image/texture lifecycle, per-binding UV/frame consumers and canonical export, separately evaluated base/clearcoat normals and finite evaluator/Mikk budgets remain. These are not removed from the issue's scope. All changes remain local/uncommitted.

The remaining clearcoat design was corrected against the [Khronos extension specification](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_clearcoat): base and coat normal maps share tangent space, while matching UV coordinates are recommended. Independent UV sampling does not require a second UV-derived tangent basis. The [sample renderer's normal evaluation](https://github.com/KhronosGroup/glTF-Sample-Renderer/blob/main/source/Renderer/shaders/material_info.glsl) applies that shared basis and uses the unperturbed surface normal when the coat map is absent. The actual remaining consumers need separate sampled base/coat normals and that fallback; per-binding frame variants remain keyed only by canonical primitive, bind revision and base-normal UV policy. No shader change is claimed by this design correction.

### #108 appended materials and per-instance binding components — 2026-10-27

Material preparation now accepts duplicate, explicit append and proved Make unique commands. Duplicate preserves separate authored/evaluated snapshots; Make unique freezes the current evaluated appearance into both new rests, retains its exact original before snapshots and rejects a stale origin. Neither operation copies animation channels. Detached bounded material tables, explicit source-to-runtime mapping, variant count growth, packed buffer and all-frame descriptors publish together without changing geometry, images or samplers. Source and runtime IDs can differ and contain procedural gaps.

Immutable per-instance binding commands target actual renderable graph leaves, preserve shared geometry and publish all binding IDs with one graph revision. Fresh replay tokens reject stale, foreign, retired and malformed contexts. The binding-aware object stage handles prospective material counts and candidate graph revisions, retains an owning proof, and updates existing ECS material components while preserving their entity/component identities. Inactive binding edits remain valid. Preparation validates exact selected active object-to-ECS correspondence; missing or duplicate witnesses reject before GPU allocation. Legacy factor-only routing remains supported.

The application and fourteen focused targets build. Preserved renderer `383590389a5190fc948b9b1cb2013e087229bcd438b09bf770bebf9cae276c20` passes **375 cases / fourteen executables / two real Vulkan executables**, zero failures/disabled/skips. Coverage includes 23 binding cases plus allocation-hook controls, eight added variant-growth cases, six added pure material cases, seven added material-generation Vulkan cases and five added routing Vulkan cases. The combined real Make unique append, graph assignment and routing publication observes zero allocation attempts under Windows allocation denial; subsequent transfer readback checks both object buffers against published IDs, alongside ECS/cache/geometry/variant invariants. Material-buffer comparison uses mapped VMA bytes and is not a shader execution/readback claim.

Source review repaired stale Make unique provenance, hidden inactive TinyGLTF container accounting, and missing selected ECS witness coverage before acceptance. The first build then exposed two nonexistent test accessor calls. The first test run passed twelve executables but failed new variant-budget exception expectations and a helper that tried to transfer-read a retained legacy vertex buffer without TRANSFER_SRC usage. Test-only repairs use the exposed graph-node IDs, the variant helper's actual exception type and valid object-buffer readbacks, retaining separate geometry identity checks. All fourteen suites reran freshly. Original failed sources, manifest, build log and all fourteen XML results are preserved. Eighty-six enumerated relevant source/validation inputs verify before/after; this is not a complete compiler dependency graph.

Evidence: `out/feature-parity/material-assignment-foundation-validation-checkpoint.json`, `material-assignment-foundation-test-results.json` and copied XML. This is component acceptance, not native rendered assignment acceptance. The preceding 104 native live-factor captures remain evidence for the separately preserved `abec06e9...` renderer.

**#108 remains incomplete and open.** Next are owned appended-material tail detach/restore at the current animation time, frontend Assign/Make unique commands and history, per-binding frame consumers, and native rendering/portable reload acceptance. Runtime/implicit materialization UI, texture import/replacement, samplers, reference-safe resource lifecycle, separate sampled clearcoat normals in the shared tangent space and bounded evaluator/Mikk execution remain required. Changes remain local/uncommitted; no issue scope is removed.

### Owned appended-material undo/redo components — 2026-10-27

Material append preparation now issues an opaque owned receipt. Undo removes only its exact owned source/runtime tail after a bounded source, variant, animation and graph reference audit; redo restores the same IDs from the current retained prefix and animation state. Branch vacancy cookies reject stale receipts when another append reuses those numbers, while nested undo/redo and merge/clone retain authority. Variant shrink and animation-rest removal preserve unrelated current controls, rests and original tracks. Unsupported or unknown retained references reject before publication.

Preserved renderer `a87474073411e551ef5ab4839645da67608f97f95bb22269f317d0a10f90410a` passes **424 cases / fifteen executables / two real Vulkan executables**, with zero failures/disabled/skips. All fifteen suites ran freshly after the application and test build. New coverage exercises owned detach/restore, same-ID replacement branches, nested history, current animation prefix edits, stale curves, inactive/retired references, prospective binding-first publication and allocation-denied final exchanges. The earlier uncompiled audit draft had a missing namespace brace; that source and failed build log remain preserved.

Evidence: `out/feature-parity/material-tail-validation-checkpoint.json`, `material-tail-test-results.json` and copied XML. Eighty-nine relevant source/validation inputs and 133 execution inputs verify before/after; 134 runtime entries include the preserved renderer. A verified **251-entry frozen-input archive** preserves this checkpoint. These are component fixtures, not native rendered Assign/Make unique acceptance. The source/graph audit still requires an outer proof for provider references and complete renderer retirement/publication. The preceding curve-compatibility check may allocate bounded scratch; final exchanges do not.

**#108 remains incomplete and open.** Frontend Assign/Make unique controls, capture dispatch and owned history are next, including consistent provider facts. Runtime/implicit materialization, per-binding normal-UV geometry/deformation/ray consumers and canonical export, texture import/replacement, samplers, reference-safe resource lifecycle, separate sampled clearcoat normals in the shared tangent space, finite evaluator/Mikk execution and native rendered/portable-reload acceptance remain required. Scope/acceptance boxes stay unchecked; changes remain local/uncommitted.

### Full editor design references expanded — 2026-10-28

Version 1.1 of `docs/ui-ux-design-guide.md`, its Word companion and `docs/player-editor-ux-plan.md` now maps proven Blender, Unreal Editor, Substance 3D Painter, Revit, 3ds Max, AutoCAD and Fusion interactions to one consistent Scene/Viewport/Inspector shell. The guide specifies familiar task workspaces, project and BIM browsing, contextual properties, material assignment, texture-slot organization, precision transforms, snapping, command discovery and persistent layouts. Official reference links accompany the concrete mappings. Existing feature-preservation tables and implementation statuses remain intact; the proposed presentation does not claim new modeling, painting or CAD kernels.

The permissive Dear ImGui/GLFW direction remains. The Word guide was rendered and every one of its 17 pages visually inspected, with 511 ordered source blocks and 14 tables. The packaged renderer lacked bundled LibreOffice; isolated invisible Word COM and bundled Poppler produced the verified pages. QA: `out/ui-ux-design-guide/reference-v1.1/final-qa.json`. This is design documentation, not a claim that the full proposed editor shell has been implemented.

### Assign and source-backed Make Unique validated — 2026-10-28

The selected-surface inspector now exposes guarded Assign and Make Unique actions, with explicit source/runtime pairs and a separate strict capture command. Make Unique freezes the current evaluated appearance into an independent unanimated material. Bounded history retains CPU binding/append receipts and resource leases; replay freshly stages against the current animation time and frame resources. A failed successor leaves the preceding material gesture intact.

Assignment-only publication creates fresh scene descriptors without copying material definitions. The shared coordinator stages object/ECS routing, provider metadata, culling/shadow, auxiliary PBR, ray/light and DDGI consumers before its graph → material/descriptors → controller publication. Provider extraction now uses actual instance material IDs, draw classification and multiplicity, while preserving geometry primitive counts and authoritative empty views. Tail removal also audits retained BIM objects/metadata and pins known provider batch contents.

Preserved renderer `b4b6ccb26e52929f67ac7591d5692b93afce6892a9871a0858b4a895da645dad` passes **1913 cases / 82 executables / 14 real Vulkan executables**, zero failures/disabled/skips. A fresh focused run passes 577 cases / 17 executables. Two initial compile failures were new tests using private/nonexistent accessors; the first focused run failed two new popup tests that guessed row coordinates. Public-view and exact ImGui item-ID test repairs passed freshly. Actual controller draw-membership telemetry was added so native routing checks do not mistake graph classifications for draw evidence. Original failed sources, logs and all 17 initial XML results remain preserved.

Fresh forward/deferred validation passes **124 captures / 124 CPU publication checks / 122 image checks / eight portable export inspections / 58 Vulkan validation-clean process logs**. It covers isolated translucent material edits, Assign/history, same-ID tail undo/redo, current-clock frozen duplication and unrelated prefix edits, resize/MSAA replay, six GLB and two ASCII glTF exports/reloads, and explicit invalid-request rejection. Native pixels use independent literal unlit controls; actual controller membership, records and hashes are CPU evidence. Invalids have real before-images, with no uncaptured postfailure-state claim.

Evidence: `out/feature-parity/material-binding-live-validation-checkpoint.json`, `material-binding-broad-test-results.json` with copied XML, and `material-binding-b4b6ccb2-native-results.json` referencing all five native cohorts. All 97 enumerated relevant sources, 201 broad execution inputs and 234 native inputs verify at their recorded gates; the source list is not a complete compiler dependency graph. A verified **850-entry validation archive** preserves current inputs, failed sources/logs/XML and all native evidence. Concurrent unrelated OpenVDB CMake additions were inspected and retained. Changes remain local/uncommitted.

**#108 remains incomplete and open.** Runtime/implicit Make Unique needs explicit 23-slot source/resource provenance and valid archive/live resource domains. Assignments requiring a different normal UV basis reject until per-binding static/deformed/current-previous/ray frame consumers, canonical export and finite evaluator/Mikk execution are implemented. Texture import/replacement, sampler editing, reference-safe resource lifecycle, separate sampled clearcoat normals, native binding ray/DDGI execution and end-to-end application UI acceptance remain required. Scope/acceptance boxes stay unchecked. The full UI guide is now v1.1 with Blender, Unreal, Painter, Revit, 3ds Max, AutoCAD and Fusion reference patterns and the permissive no-Qt stack; its proposed shell is not claimed as implemented.

### Runtime materialization and practical tool navigation — 2026-10-28

Make Unique now accepts certified runtime/implicit materials through explicit provenance for all 23 texture slots. Paired file/byte loaders retain independent authored/evaluated source IDs, color/data roles, source cookies and allocation leases. BIM producers supply provenance while their source and role are known; merge, clone and owned undo/redo retain it. Source identity is never inferred from an aliased GPU handle. A primitive with no glTF material now receives its own strict metallic-one default, separate from the editor's metallic-zero default.

Review repaired post-promotion image/sampler indices and a stale final-publication guard: an unused texture import between material preparation and exchange now rejects without allocation or consumption. Actual sampled-image and sampler capacity is checked before decode/upload. Pure materialization checks exact forward conversion, bounded metadata and explicit source roles; unsupported remaining runtime representations still reject and remain implementation work.

The requested immediate UI improvement uses the existing ImGui panel: a fixed **Go to** menu and direct **Selection**/**Material** shortcuts reach developed tools and open their relevant headers. Every original body control and BIM lifecycle update remains in order. No docking/workspace shell or new dependency was introduced. [Current editor controls](current-editor-controls.md) documents practical workflows separately from the future design guide; README links it.

Preserved renderer `3956ae905bac03ea055983413c143218dcc36632998c3a59bdd8e8e8c13aa25d` passes a fresh **1948 cases / 83 executables / 14 real Vulkan executables**, zero failures/disabled/skips. Before the UI integration, one broad test failed three obsolete cache-spelling assertions. Those checks were replaced by behavioral sampler-policy/source-identity assertions in the existing GPU case; both affected suites passed, followed by the complete fresh final run. The earlier normalized-export image oracle failure, original sources/logs/XML and the pre-review 611-case component pass remain preserved.

Fresh native forward/deferred validation passes **228 captures / 228 CPU publication checks / 230 image checks / 14 portable export inspections / 110 Vulkan validation-clean process logs**. Four new implicit-material cohorts contribute 104 captures, including a separate identity preflight and an independent literal metallic-one reference before mutation. Five existing source-backed binding cohorts contribute 124 captures. Coverage includes isolated edits, Assign/history, current-clock replay, same-ID tail restoration, resize/MSAA, ten GLB and four ASCII glTF exports with portable reload, and explicit invalid-request rejection.

Real Windows application interaction separately verifies eleven observations: persistent navigation, Material/Selection access, reopening a collapsed transform, returning to import/save from nonzero scroll, actual Make Unique changing shared users from two to one, Ctrl+Z restoring two, and animation/DDGI/telemetry routes. Asset-specific BIM/variant workflows and all GUI paths were not manually exercised. The temporary scene was closed without saving it.

Evidence: `out/feature-parity/runtime-material-live-validation-checkpoint.json` (SHA256 `60f1a708496d398064b22b3249148be18a94f4f64469d19a4c195786395bfcfe`), `runtime-material-ui-broad-test-results.json` with copied XML, `runtime-material-ui-native-results.json` and all nine reports, plus `runtime-material-ui-manual-results.json`. All **104 enumerated relevant sources / 202 execution inputs / 307 native inputs** verify at their recorded gates; the source set is not a complete compiler dependency graph. A verified **1356-entry validation archive**, SHA256 `2c381940ffd870669f4cf44e014a9c662c790a330b863aa7690b8d015df8a7e4`, preserves current inputs, native evidence and earlier component/failure records. Its preservation report is `runtime-material-live-validation-preservation.json`.

**#108 remains incomplete and open.** The new native materialization fixtures are untextured implicit glTF materials; the 23-slot/manual/BIM coverage is component evidence. CPU catalog/draw-membership/hash checks are not GPU buffer readback, and invalid cohorts check preceding images plus rejection rather than uncaptured postfailure state. Texture import/replacement/clear and sampler editing need an atomic resource/material transaction and editor controls, followed by reference-safe deletion. Remaining runtime representations, per-binding normal-UV frame consumers/canonical export, separate base/coat normal evaluation, finite evaluator/Mikk execution, textured/BIM native and binding ray/DDGI acceptance remain required. The actual descriptor-exhaustion boundary was not forced. Logical byte/work limits do not cap private codec scratch or driver overhead. All changes remain local/uncommitted; the full tracker goal stays active.

### Texture, mapping and sampler authoring — 2026-10-29

The existing Material inspector now exposes image import/replacement, retained-source selection, slot clearing, UV0/UV1 mapping, editable legacy channels and sampler settings for the 23 registered texture slots. Apply/Reset/Cancel and inline errors use the shared editor backend. Undo/redo retains owned resources and source identities without rereading files, preserves the current animation clock and survives frame-resource recreation. Successful resource/material/descriptor/routing publication is coordinated; failed successors retain the preceding gesture and redo state.

Packed specular/glossiness RGB and linear alpha share the Color resource in import, editing, provenance and telemetry; separate legacy roughness stays Data. The UI keeps the empty packed-map control visible beside a legacy fallback. Source-backed SG save now preserves the evaluated source definition only after exact runtime conversion and explicit authored/evaluated source-ID proofs; the runtime-only approximation still requires baking. The project ASCII/GLB writer retains known legacy fields and their original parameter locations. Native image comparison additionally exposed index-only legacy loading: the importer now preserves canonical source precedence, UV transforms and channels, including opacity alpha selection. No checker tolerance was relaxed for that fix.

Preserved renderer `c3a34a1edbcc20ddf71de5f83939d8250cfd51bf231d322c3834aef0a364393d` passes a fresh **2066 cases / 86 executables / 15 real Vulkan executables**, with zero failures, skips or disabled cases. The new source-backed SG export helper has eight cases; importer regressions cover both legacy maps, all eight aliases, precedence, full mapping/channel parity and invalid-metadata rejection before publication. Initial test fixture and macro failures are preserved separately.

Fresh acceptance on that same executable passes **414 captures / 414 CPU publication checks / 276 image checks / 14 portable exports and reloads / 168 strict-validation-clean process logs** across seven cohorts in forward and deferred raster. The exports are ten GLBs and four ASCII glTFs. Coverage includes all23 binding/mapping/clear source rows, SG coupling, file replacement, UV1 and sampler changes, current-clock history, resize/MSAA, copied-location reload, actual after-failure gesture/resource/history preservation and explicit invalid-request rejection. The final all-slot opacity comparison is pixel exact against its independently loaded reference after the loader fix.

The final executable separately passes eight real Windows UI observations plus one serialized Save As inspection: prospective legacy/packed controls, typed import, Ctrl+Z/Ctrl+Y, navigation and a fresh GLB save. Independent inspection verifies the edited packed left material, unchanged legacy right material, opaque metadata, exact embedded image pixels and no external payload URIs. The original fixture remains unchanged. Thirteen texture observations on c98219ef and six SG observations on 4a6c1ca0 retain their earlier provenance; these are UI subsets and are not added to native capture totals.

Evidence: `out/feature-parity/material-texture-live-validation-checkpoint.json` (SHA256 `901b1eae6369ddaddcb88a403ef2f5026d685e565796f75cd8518a185b2ee838`), `material-texture-broad-test-results-attempt17.json` with its copied XML, `material-texture-live-native-results.json` referencing all seven reports, and `material-texture-final-ui-manual-results.json`. All **121 enumerated relevant sources / 205 execution inputs / 327 native inputs** verify at the recorded gates; this is not a complete compiler dependency graph. Versioned source/component archives and the failed59-capture legacy-opacity diagnostic are preserved; `material-texture-live-validation-preservation.json` inventories the final archive and diagnostics.

**#108 remains incomplete and open.** Slot clearing preserves the source resource catalog. Reference-safe material/image/texture lifecycle, remaining exact runtime representations, per-binding normal-UV static/deformed/current-previous/ray consumers and canonical export, separate sampled base/coat normals in the shared tangent space, finite evaluator/Mikk execution/storage budgets and further textured/BIM/ray/DDGI/UI acceptance remain required. All23 source/export binding coverage does not prove all23 shader arithmetic paths; catalog/draw/hash telemetry is CPU evidence, not GPU buffer readback. Dedicated decode failure has actual AFTER captures; fatal invalid requests provide preceding state plus rejection. Real descriptor exhaustion was not forced, and logical budgets do not cap private codec scratch or driver overhead. Changes remain local/uncommitted; tracker #121 stays active.

### Resource lifecycle backend foundations — 2026-10-29

Arbitrary unused material-reference auditing now covers explicit source/runtime
pairs, runtime-only rows, hidden/inactive/retired graph nodes, raw and parsed
variants, material LODs and unsupported material-pointer suffixes. Unknown
normative schemas reject conservatively. Detached material export compaction
removes arbitrary rows after source-to-runtime projection, remaps known
references and preserves geometry/image payload allocations. Animation rest
updates preserve the current playback state while rejecting tracked rows.

The new bounded lifecycle catalog separates immutable incarnation, retirement
command authority and live/history/preparation/GPU retention. Prospective pins
publish atomically with new rows; cancellation and stale/foreign rejection
leave live counters unchanged. Per-image GPU leases allow selective reclamation
after aggregate batch ownership is released. Initial image ownership transfers
only through explicit checked adoption; temporary acquisition preserves rollback.
TextureManager supports stable tombstones and erases all cache aliases targeting
a retired row. [Resource lifecycle contracts](resource-lifecycle.md) explains
the ownership and publication boundaries.

Preserved renderer
`817ea7399a3e8e689edd58c178fe09cb11908ea68e08f3dddc60eddefc0afbf5`
passes **2150 cases / 89 executables / 15 real Vulkan executables**, with zero
failures, skips or disabled cases. Fresh XML timestamps and CTest completion,
147 enumerated source inputs and 208 execution inputs verify. Compaction now
updates the caller-owned detached model: review found that returning TinyGLTF
by move after edits could allocate an MSVC map sentinel. Its regression sweeps
allocation denial across every staging boundary and requires exact unchanged
input on rejection. The deliberate first build interruption and subsequent
same-line GoogleTest macro compile failure remain preserved with their sources
and logs.

Texture-tool nonregression on that same executable passes **130 captures /
130 CPU state checks / 140 image comparisons / four GLB exports and reloads /
74 strict-validation-clean process logs** in forward and deferred raster.
Core, current-clock history and actual after-failure preservation cohorts all
pass. This is not operational resource deletion or new UI acceptance. The
preceding c3a34a1e full 414-capture/23-slot/UI acceptance retains its separate
binary provenance.

Evidence: `out/feature-parity/resource-lifecycle-817ea739-attempt3-validation-checkpoint.json`
(SHA256 `e82f10286285457cfd151e7de2ef4bcbe0841503398a21c88ae4e6ce0114126f`),
`resource-lifecycle-broad-test-results-attempt3.json` and copied XML,
`resource-lifecycle-native-results-attempt3.json`, plus the **872-entry** verified
archive (SHA256 `7288afdf01248a30358bc3c2755830b6965ea84f6f22007b142dcf6ad21e2f2c`).
All 356 enumerated native inputs verify at the recorded gates; source coverage
is not a complete compiler dependency graph.

**#108 and #121 remain open, with all remaining scope intact.** Operational
arbitrary material/image/texture deletion still needs complete source,
private-cache, provider/BIM and history audits; stable fallback descriptors;
coherent scene/ray/DDGI publication; dense export in both save paths; editor
actions; and end-to-end acceptance. The material integration review records
exact hooks; the texture review is a partial consumer inventory, not a completed
proof. Remaining exact runtime representations, per-binding normal-UV consumers
and canonical export, separate base/coat normals, finite evaluator/Mikk budgets
and further textured/BIM/ray/DDGI coverage remain required. Logical budgets do
not cap private codec/driver overhead. Changes remain local/uncommitted; the
full tracker goal stays active.

### Operational unused material deletion — 2026-10-29

The existing ImGui Material controls now include an independent **Material
catalog → Delete unused material** workflow, usable without selecting a surface.
Used, default and unproved material rows show a disabled action and a reason.
Deletion filters the selected row from authoring while preserving live numeric
IDs; Undo/Redo restores its exact incarnation and retained definition. Queued
actions revalidate the authored revision and explicit source/runtime pair.
Complete source/graph/private-cache checks and conservative external BIM/provider
checks guard publication. Full asset merges fork material authority; equivalent
subtree clones share it and preserve the internal allocation high-water mark.
Both save entry points compact retired material rows only in the detached export.

The frontend's native run found an actual budget defect: clamping the finite
512 MiB staging budget to remaining 256 MiB retained-history capacity made a
normal nonnull animation player reject valid deletion. Removing that clamp
preserves the separate finite staging limit and actual retained-command budget.
Fixture/oracle repairs are recorded separately: decoded versus encoded image
expectations, strict empty-journal normalization, binary32 placement expectations
with zero tolerance, and a runtime fixture using the Lit mode supported by both
renderers. Failed/partial attempts, original reports and source/binary records
remain archived. The final native checker passes 85 pure self-checks.

Frozen renderer
`fb51a99211b757953634fe4800199af3cb6977c3f6666151af031f6042cbafd1`
passes **2172 cases / 90 executables / 16 real Vulkan executables**, with zero
failures, skips or disabled cases. Fresh copied XML and CTest completion verify.
The five new material cohorts in forward/deferred pass **122 captures /
122 CPU checks / 138 image comparisons / four exports and reloads /
40 validation-checked processes**. Exports comprise two ASCII glTFs and two GLBs.
Coverage includes arbitrary middle-row deletion, source-backed and runtime-only
producers, current-clock history, branch append/incarnation safety, actual
after-rejection preservation, raw variants, animation pointers and portable save.

Separate existing texture nonregression on the same executable passes
**130 captures / 130 CPU checks / 140 image comparisons / four GLB exports and
reloads / 74 processes**. These remain separate cohort records; the preceding
full 414-capture texture milestone retains its original binary provenance.

Actual Windows editor acceptance records **10 UI observations and two independent
serialized Save As inspections**: catalog access with no selection, protected/used
feedback, Delete, Ctrl+Z/Ctrl+Y, and Undo after Save As. The two new GLBs contain
dense five/six-row material tables with the expected deletion/restoration,
references, variants, opaque extras and geometry. Exact semantic checks use
explicit binary32/default normalization and reject five negative controls;
numeric tolerance is zero. The generated source is unchanged, the app exited 0
and its window closed. The log has no errors and six known unused-vertex-attribute
validation warnings. This fixture is static/unlit and does not establish
BIM/ray/DDGI or image-deletion UI acceptance.

Evidence: `out/feature-parity/material-retirement-fb51a992-attempt7-validation-checkpoint.json`
(SHA256 `4a5c7f7777f5b35c0b48b85f9c4321831ca746fa051ea56bc353669b29613566`),
the copied C++ XML, `material-retirement-native-results-attempt7.json`,
`material-retirement-ui-manual-results.json`, and the verified **877-entry /
431,406,797-byte** primary archive (SHA256
`ef1e345d340105700454099ee8177197c25700e711ce1bede8560f2775d2bac8`).
The separate **412-entry** texture archive has SHA256
`28f4e70fa92f150663df3ee185ff2636113ee2d519e7361b33f4e2dc7fdf9987`.
All **164 enumerated source / 210 execution / 375 native inputs** verify again;
the enumerated source set is not a complete compiler dependency graph.
The supplemental final preservation record inventories both archives and tracker
snapshots. Current controls and contracts are documented in
[current-editor-controls.md](current-editor-controls.md) and
[resource-lifecycle.md](resource-lifecycle.md).

**#108, #121 and the full tracker goal remain open and active.** Operational
arbitrary unused image/texture/sampler deletion still requires all live/history/
preparation consumers, descriptor fallback safety, selective ownership adoption
and dense resource export. Remaining exact runtime representations, per-binding
normal-UV static/deformed/current-previous/ray streams and canonical export,
separate sampled base/coat normals, finite evaluator/Mikk execution/storage
budgets and further textured/BIM/ray/DDGI acceptance remain required. CPU
catalog/hash telemetry is not GPU readback; logical budgets do not bound private
codec/driver overhead. Changes remain local/uncommitted.

### Stable texture sampling and retained readers — 2026-10-29

SceneManager now uses one bounded sampling plan for stable image descriptors,
dense sampler slots and GPU metadata. A retired middle row resolves to a private
owned fallback; later textures retain their numeric addresses. Empty catalogs
have one initialized fallback descriptor while retaining zero source/runtime
resource IDs. Active null, stale, foreign or mismatched allocations reject.
Revision, phase, incarnation, image/view, sampler and metadata checks prevent
equal fallback views from hiding stale preparations.

The private white one-texel image has a dedicated batch lease outside source
catalogs and caches. Live assets, prepared bindings/material/resources and
retained commands hold its owner; merge and clone payloads carry it through
exchange. Existing resource consumers, byte accounting and role/source checks
are tombstone-aware. Owned-tail Undo copies the exact prefix, including dead
rows and erased cache aliases. Incoming merge images acquire their candidate
lease before full descriptor validation.

Resource staging preserves the supported immediate-import workflow with an
exact published-prefix proof: old rows and GPU metadata/samplers remain unchanged,
revision growth equals successful appends, and new active image capabilities
validate. Tokens record the old published plan scope and reject an intervening
metadata refresh even when row count and buffer handles stay unchanged.
Ordinary binding/factor paths remain strict. Merge exchange also now reuses
preconstructed TinyGLTF model scratch; generic model swaps can allocate MSVC
map sentinels despite declared noexcept moves. The private scratch starts and
remains canonical empty storage, preserving exact history byte accounting.

Frozen renderer
`f7741ca11dd2b574d8ad27e8da2f4a9174e18e7a0ae67701523fe04ff6b0b0af`
passes **2198 cases / 92 executables / 17 real Vulkan executables**, with zero
failures, skips or disabled cases. The new sampling target contains 11 Vulkan
cases and four pure controls. It samples actual production descriptor sets and
GPU metadata, checks literal first/fallback/last colors, distinguishes nearest/
linear and clamp/repeat behavior, poisons output before dispatch, exercises
zero/all-dead tables, resource-prefix rejection, factor/resource history,
textured merge exchange/Undo/Redo, staged clone sampling, preparation allocation
denial and a retained fallback descriptor after detaching the live owner. This
does not establish full graph-history or in-flight reader retirement by itself.
Existing allocator suites retain their separate submitted-reader checks.

On the same executable, five material cohorts in forward/deferred pass
**122 captures / 122 CPU checks / 138 image comparisons / four exports+reloads /
40 processes** (two ASCII glTFs and two GLBs). Separate existing texture core,
history and failure-preservation cohorts pass **130 captures / 130 CPU checks /
140 image comparisons / four GLB exports+reloads / 74 processes**. The combined
252 captures remain separated by family. The native checker passes 85 pure
self-checks. Native catalog/hash telemetry is CPU evidence; the dedicated new
Vulkan suite performs the actual GPU sampling/readback described above.

Attempts1/2 are preserved. Attempt1 exposed overly strict resource staging and
the model-swap allocation termination; attempt2 fixed those but reported three
history-byte mismatches from the scratch's default three-character version.
The repairs retain exact accounting and no-allocation assertions. No tolerance
or safety oracle was relaxed. The failed component archives have SHA256
`278444678a9c10a313fb12ceed291c2599a6ba855fb018def3a3bfbde194e24f`
and `733d422cc0426b1604d2cc4df989de84cfa61a909777d41b024d23e29f3e14fe`.

Evidence: `out/feature-parity/texture-sampling-attempt3-validation-checkpoint.json`
(SHA256 `236ee6b56dd20e41a2c4bf9a67d19465070e5ce26763456a3055e3758be92b73`),
copied fresh XML, all eight native reports, and the **1257-entry / 78,277,975-byte**
verified archive (SHA256
`fe7687ef7896d39a6bd5f67ed6ebceba165692a5518ce00ae02c474422581814`).
All **176 enumerated source / 213 execution / 390 native inputs** verify again;
this is not a complete compiler dependency graph. The final preservation record
adds failed-run diagnoses, tracker snapshots and the next ownership audit.
No new Windows UI acceptance was performed for this backend increment; the
preceding material Delete UI evidence retains its fb51a992 provenance.

**#108, #121 and the full goal stay active.** User image/texture/sampler Delete
still requires complete source/history reference authority, per-image ownership
migration/adoption, remaining reader integration, dense resource export, editor
controls and acceptance. Aggregate main-scene/history leases still prevent
selective reclamation. The next ownership audit inventories those retention
sites and requires recomputing live image coverage across alias-tail Undo.
Remaining exact runtime representations, per-binding normal-UV streams and
canonical export, separate sampled base/coat normals, finite evaluator/Mikk
execution/storage budgets and further textured/BIM/ray/DDGI coverage remain
required. Logical budgets do not bound private codec/driver overhead. No scope
or acceptance box is removed or completed here. Changes remain local/uncommitted.

### Per-image ownership and selected history retention — 2026-10-29

Main-scene allocation ownership now uses complete immutable per-image sets
instead of aggregate upload leases. Every active stable runtime alias maps to an
exact allocator identity/incarnation and image/view capability; aliases reuse
the same pin. Live assets, prepared scene/material/resource descriptors and
merge/clone generations retain complete corresponding sets. Initial group-zero
images keep reset ownership through fallible preparation and transfer only at
the checked final adoption boundary. New upload batches retain a transient
rollback owner until independent pins exist, then release it outside publication.
The private fallback keeps its separate dedicated one-image owner.

Append, material retirement, resource, assignment, texture, factor and gesture
histories retain selected actual-image endpoint unions without retaining a full
parent catalog. Undo of a sampler alias recomputes complete surviving coverage;
it never subtracts a shared prefix image's owner. Exact ownership identity and
coverage join stale-preparation guards. Existing raster/ray/DDGI retirement
coordination and explicit allocator force-reset contracts remain required.

Merge/clone staging also preserves committed immediate CPU imports through the
existing exact published-prefix proof. Candidates have complete sampling plans;
tokens capture the live owner and old sampling revision/row count. A metadata
refresh rejects stale preparation even when outer revisions and owner identity
remain unchanged. Ordinary binding/factor staging remains strict.

Renderer
`74afce7ebeb6d3a7fa917e672b9318fd2b4653dba1afeedea48f3ab281ccc119`
passes **2209 cases / 92 executables / 17 real Vulkan executables**, with zero
failures, skips or disabled cases and copied fresh XML. The sampling/ownership
target now contains **22 Vulkan cases + four pure controls**, including actual
group-zero initial adoption and deferred-adoption cancellation, selective
batch/member reclamation, independent selected subsets, alias-tail Undo/Redo,
prepared descriptor sampling after the live owner detaches, selected material
receipts, factor/Clear/assignment endpoints, capability/budget/allocation denial,
and merge/clone prefix/phase proof. These tests release full holders before
checking actual allocator capabilities. Actual initialize/reload failure before
final adoption is reviewed but is not directly fault-injected by these tests;
helper cancellation and successful initial publication have runtime coverage.

Fresh native regressions on that executable pass **122 material captures /
122 CPU checks / 138 image comparisons / four exports+reloads / 40 processes**,
plus separate **130 texture captures / 130 CPU checks / 140 image comparisons /
four GLB exports+reloads / 74 processes**. Material exports are two ASCII glTFs
and two GLBs. All eight cohorts pass in forward/deferred; the native checker's
85 pure self-checks pass. Native telemetry is CPU evidence; dedicated Vulkan
tests provide the actual sampling/allocation evidence. No new Windows ImGui or
complete BIM/provider/ray/DDGI deletion acceptance is claimed.

Attempt1's two failures and original executable remain preserved: the root's
new overly strict merge guard rejected a supported pending import, and the new
GLB fixture lacked the URI needed for TinyGLTF to encode its image. The fixes
retain strict candidate validation, add a prefix/phase regression, and preserve
the production loader and reclamation assertions. No oracle tolerance changed.
The failed **314-entry / 72,553,339-byte** archive has SHA256
`661fdd5346cfeffd541743b4bef07240e9b10c1c8fcc2ed9c3427d21d6587974`.

Evidence: `out/feature-parity/image-ownership-attempt2-validation-checkpoint.json`
(SHA256 `717c9599070569b32c5969c39bc87adabb52e51cda37962a3f6f55373d8b08b0`),
and the verified **1264-entry / 78,454,708-byte** archive (SHA256
`12534317c82183b57af12f38499e29d7bbf9e7b9686f0bd840e88c870e24bc07`).
All **183 enumerated source / 213 execution / 397 native inputs** verify; this
is not a complete compiler dependency graph. The final supplement records tracker
snapshots, the diagnosis and two read-only next-increment audits.

**#108, #121 and the full goal remain active.** Per-image ownership does not
authorize logical image/texture/sampler Delete. Remaining work includes separate
source/evaluated/runtime identity domains and lifecycle pins; complete bounded
source, alternate-cache, BIM/provider, animation and history reference proof;
stable retirement and Undo authority; dense resource export in both save paths;
portable payload preservation; simple editor controls and acceptance. Current
LiveDependency rules require texture retirement before separate proved orphan
image/sampler retirement; an atomic cascade is not implemented. The new audits
are inventories/design guidance, not exhaustive proofs or runtime acceptance.
All other #108/#121 requirements, including exact runtime representations,
normal-UV streams/export, separate base/coat normals and finite evaluator/Mikk
execution/storage bounds, remain required. Changes remain local/uncommitted.


### Logical resource identities and selected source history — 2026-10-29

Scene resources now have independent Archive/Evaluated Image, Texture and Sampler
identities, plus RuntimeTexture identities. Current table addresses map to
owner-issued handles; internal slots remain monotonic across owned-tail Undo and
new numeric-index branches. Selected source history retains immutable exact
records and encoded bufferView slices, including every known authored image
alternative and source sampler. It shares selected controls/payloads rather than
retaining a complete parent catalog. Actual GPU image ownership stays separate.

The bounded raw-reference extractor inspects shadowed core/extension/legacy
material bindings and all authored texture image alternatives, not just the
currently evaluated 23 slots. Strict proofs reject unproved normative schemas.
Compatibility extraction preserves exact opaque witnesses and known references;
selected opaque metadata in materials, textures, images, samplers and retained
buffer views holds a separate blocker, including empty known-reference sets.
Extras remain bounded annotations. Neither incomplete extraction nor pin counts
alone authorize deleting a resource.

Material/resource receipts and frontend factor/gesture/Clear/mapping/assignment
histories retain selected logical references and exact Material History pins.
Resource commands also capture their selected Material incarnation: a fresh
material at the same numeric source index cannot receive an older resource
Redo. Before-readers hold GpuReader pins; future descriptor preparations use
Preparation pins. Pending publication/cancellation, original-incarnation resource
Undo/Redo, and fresh-domain rejection are integrated. Full merge forks resource
authority; a material-equivalent clone shares the current generation. Old owners
remain retained through allocation-free publication.

The generic lifecycle now supports exact outgoing LiveDependency-owner release,
transition-staged restoration, and phase-less dependency replacement. It checks
residual and late pins before commit; failed/abandoned preparations leave old
owners intact. These primitives do not yet install every production outgoing
edge. `dependencyAuthorityComplete()` deliberately remains false. The next audit
identifies the shared source-catalog mutation/readiness split needed for paired
resource/material publication; committing two independent source-catalog
transitions would invalidate the second one.

Trusted existing mutable-source edits are observed before fresh preparation by
explicit bounded reconciliation. Compatible metadata gets a new immutable
version at the same handle, preserving unrelated current prefix extras through
Undo/Redo. Evaluated-only suffix removal retires exact handles; later appends
receive fresh internal slots. Binary/image-format/runtime-capability/sampler-core
replacement, known texture-dependency changes and mixed append/retirement are
rejected. Old prepared pointers/revisions and full payload matches become stale;
final guards never reconcile or allocate. A retained history identity need not
match one historical metadata pointer, and cannot rewind an unrelated prefix.

Renderer
`f9e35417111e94e0e30701635af74920c7964ff7417b2f10b722d44865a96992`
passes **2262 cases / 94 executables / 17 real Vulkan executables**, zero
failures/skips/disabled, with copied fresh XML. There are 11 new generic
transaction cases; 12 raw-reference cases plus three allocation controls; 20
logical-generation cases plus three allocation controls; and four new Vulkan
integration cases. The combined sampling/ownership target now has 26 Vulkan
cases and four pure controls. New coverage includes pending cancellation,
selected CPU ownership, opaque blockers, metadata versions, suffix/branch
incarnations, same-index material replacement, full-merge domain rejection and
original resource replay, with allocation-denied commits and actual sampling.

All eight native cohorts pass on that same binary: **122 material captures /
122 CPU checks / 138 image comparisons / four exports+reloads / 40 processes**,
plus separate **130 texture captures / 130 CPU checks / 140 image comparisons /
four GLB exports+reloads / 74 processes**. Material exports are two ASCII glTFs
and two GLBs. The native checker passes 85 pure self-checks. CPU telemetry is not
GPU readback; the dedicated Vulkan suites provide the actual sampling/allocation
evidence. No new manual Windows ImGui acceptance, complete external-provider
proof or full BIM/ray/DDGI deletion acceptance is claimed.

Attempt1's 2257 cases had nine failures, preserved with its exact executable:
missing trusted source reconciliation, changed budget-error admission semantics,
a one-time invisible registry reservation in an allocation sweep, and two new
tests incorrectly expecting an abandoned redo branch to revive. Repairs preserve
the existing safety oracles; original successful replay is checked before branch
replacement and wrong-incarnation/abandoned-branch rejection remains checked
afterward. The failed **318-entry / 73,564,786-byte** archive has SHA256
`9af247b129e591d23a79aa99ad2179e6c7be08ed714af991ec3fcd32a3dc53c5`.

Evidence: `out/feature-parity/resource-authority-attempt2-validation-checkpoint.json`
(SHA256 `ce7c5114973dcb1df90c98cbbe3e267539f49c4f78e325a27a2bacba1d05a0c9`),
and the verified **1279-entry / 79,592,892-byte** archive (SHA256
`c7194f495e46ccc27278a1af9f77812b6a459bcd0069e7035d6453b5198401ec`).
All **194 enumerated source / 215 execution / 410 native inputs** verify; this
is not a full compiler dependency graph. The final supplement preserves the
failure diagnosis, tracker snapshots and the new live-dependency audit. That
audit is implementation guidance, not completed reference proof or acceptance.

**#108, #121 and the full goal remain active.** Production outgoing dependencies
and complete source/private-cache/provider/animation/history reference authority
still need integration. Arbitrary source masks/tombstones, retirement and Undo
authority, dense image/texture/sampler export in both save paths, exact portable
payloads, simple Delete controls and acceptance remain required. No user-facing
unused image/texture/sampler Delete is enabled. All other original requirements,
including exact runtime representations, per-binding normal-UV streams/export,
separate base/coat normals and finite evaluator/Mikk execution/storage budgets,
remain required. Logical bounds exclude private codec/driver overhead. Changes
remain local and uncommitted.


## Live resource dependencies and clone restaging — 2026-10-29

Implemented independent live controls for raw source texture-to-image/sampler
edges, color/data roles and opaque source records. Active material definitions
own their authored/evaluated/export/rest/runtime/provenance targets and only
the serializer fallback-map cells they actually use. History remains separate
selected retention. Bounded weak owner ledgers, exact outgoing controls, pending
successors and one sealed source-catalog mutation prevent partial publication.
Unchanged material states and unrelated role controls share their existing
owners. Standalone resource Undo cannot remove a material's bound tail; a
validated paired material/resource successor can publish that removal together.

Bootstrap, immediate texture producers and manual material creation publish
their dependencies. Shared clone history freshly prepares against today's live
controls after intervening material edits. Exact source payload/role/cookie and
incarnation guards reject stale branches instead of rewinding unrelated source
changes. Independent valid merge generations preserve their pointers. Readiness
checks compare the caller's current animation-player pointer before using it;
consumed preparation is released outside the allocation-free asset exchange.

Renderer `b0bf51f2b26b5cd70cead06381dd5a83c68276eab848e4b872eef804a47a59f8`
passes **2285 cases / 94 executables / 17 Vulkan executables**, zero
failures/skips/disabled, with fresh copied XML. The 23 added cases cover late
outgoing-owner attachment, pending cancellation, source/role/material controls,
5,000 independent textured owners, unaffected role reuse, exact future-owner
survival, paired Undo, manual textured producers, clone replay after factor Undo,
stale/denied preparation and replaced players. Existing allocation-denial and
malformed-input/state-preservation assertions remain.

Fresh native acceptance passes **122 material captures / 122 CPU checks /
138 image comparisons / four exports+reloads / 40 processes**, plus separate
**130 texture captures / 130 CPU checks / 140 image comparisons / four GLB
exports+reloads / 74 processes**. Material exports are two ASCII glTFs and two
GLBs. All eight forward/deferred cohorts and 85 pure checker self-checks pass.
CPU telemetry is not GPU readback; the dedicated Vulkan suites separately cover
actual sampling and allocation ownership. No new manual Windows ImGui or
complete BIM/provider/ray/DDGI resource-deletion acceptance is claimed.

Attempt1's one failing case (two assertions) exposed the new early invalid-input
exception escaping the existing merge/clone runtime-error boundary. Production
now normalizes input/limit failures at that fallible boundary while preserving
`bad_alloc`; no test oracle was relaxed. A separate Windows PowerShell 5.1
suite-array wrapper was rejected by the strict result collector. Explicit
`string[]` recording and pwsh repair that runner issue. Its exact 94 suite
values, all 2285 XML cases, source, executable and diagnostics remain archived;
the failed **318-entry / 74,006,423-byte** archive SHA256 is
`d05e07b4fd4d82a23f1af0044ff085e9835b6fc1dfb63d57582e52b902b728d4`.

Evidence: `out/feature-parity/live-dependencies-attempt2-validation-checkpoint.json`
(SHA256 `1e7ded9f56d21900e8f96112d5b9aacab51a91fdebe28f82349be9c2f338d0c3`),
and the verified **1279-entry / 80,077,960-byte** archive (SHA256
`cd21b444678bc56d0bdcbbd613f8e3c5574732155a90cc6df8e12a59c80cfd02`).
All **194 enumerated source / 215 execution / 410 native inputs** verify; this
is not a full compiler dependency graph. The next-step source audit identifies
resource filtering **before** image packing or external-image I/O as a required
part of both dense save paths. It is guidance, not implemented acceptance.

**#108, #121 and the full goal remain active.** Complete raw-document/animation,
private-cache revival and bounded external/provider/rest authority remains
unproved; `dependencyAuthorityComplete()` stays false. Arbitrary source phase
masks, selected retirement/Undo, cache-cookie/provenance invalidation, both
dense resource export paths, exact portable payloads, simple Delete controls
and their acceptance remain required. No unused image/texture/sampler Delete
is enabled. All other original scope, including runtime/height representations,
per-binding normal-UV streams/export, separate base/coat normals, finite
evaluator/Mikk execution/storage caps and further textured/BIM/ray/DDGI coverage,
remains required. Changes remain local and uncommitted.


## Selected source retirement and dense resource export — 2026-10-29

Implemented stable independent Archive/Evaluated Image, Texture and Sampler
phase masks, nonallocating phase queries and selected original-authority
retirement/restore receipts. Active source owners cannot reference inactive
targets. Exact original runtime role and evaluated endpoints prevent numeric
address reuse from rebinding a restored source. One sealed mutation releases
only validated outgoing owners; all residual material/source dependencies block.
Unrelated current rows, roles, cookies and mappings survive replay. Whole phase
masks are validated once, with selected reference checks instead of quadratic
full-mask scans per owner.

The SceneManager source-only transaction prepares raw A/E/rest witnesses,
material owners and selected cache/export deltas before publication. It checks
current material dependencies before clearing mappings. Inactive cache
tombstones keep stable addresses; Undo restores only exact selected cells.
Prospective exporter mappings are rechecked when material owners restage.
Successful publication invalidates competing preparations; abandoned/failed
preparations preserve earlier ready tokens. Imports, trusted source appends,
merge and clone propagate source phases. Fresh resource preparation captures
phase tables after trusted append normalization. Existing texture choices skip
inactive rows; no new Delete control is exposed.

The shared exporter projects source resources densely before image URI access
or encoding, after buffer/geometry and owner projection. Supported typed core,
extension and shadowed legacy bindings, codec alternatives and sampler IDs
remap consistently. Inactive detached material/node owners and their channels
are removed from reference consideration; unknown active normative schemas
reject. Survivor payload allocations remain intact in pure compaction. Both
save coordinators use the common builder. Component tests reopen manager GLB
and ASCII outputs and test missing retired image paths through the shared
exporter. New manual frontend Save As acceptance is not claimed.

Raw animation references now use an explicit supported property grammar;
unknown suffixes under known roots remain incomplete and retain known owner
identities. Recognized providers expose bounded borrowed reference views so
final material-provider matching no longer copies snapshot strings. Unknown
providers and legacy count-only nonempty mesh providers fail closed. These
pieces do not establish complete external resource authority.

**This is source-only retirement. Runtime sampled rows and actual Vulkan image
leases stay in service. GPU-image/payload reclamation and an operational Delete
UI remain required.** Dense resource IDs do not remove old image byte ranges
from shared binary buffers. Canonical Vulkan samplers remain a bounded cache.

Renderer `e481f5f3cbc420974114c3a8534536674b8abd168f0abe09663f0226b07743af`
passes **2337 cases / 96 executables / 18 Vulkan executables**, zero
failures/skips/disabled, fresh copied XML. The 52 added cases include source
phase/receipt guards, 5,000-row explicit holes, source-only cache/history,
coexisting candidates, exact endpoint ABA rejection, trusted appends and
merge/clone, dense export, pointer grammar and borrowed provider allocation
checks. The source retirement target has eight actual Vulkan cases plus three
allocator controls. Strict allocation-denial publication and failure/state
oracles remain intact.

Fresh native regressions pass **122 material captures / 122 CPU checks /
138 image comparisons / four exports+reloads / 40 processes**, plus **130 texture
captures / 130 CPU checks / 140 image comparisons / four GLB exports+reloads /
74 processes**. Material exports are two ASCII glTFs and two GLBs. All eight
forward/deferred cohorts and 85 pure checker self-checks pass. These native
cohorts regress existing editor features; they do not exercise a new source
Delete UI. CPU telemetry is not GPU readback. No new manual Windows UI or full
BIM/provider/ray/DDGI deletion acceptance is claimed.

Failed attempts remain preserved before repair. Attempt1 was a new test's
assignment to a correctly nonassignable borrowed view, repaired with in-place
reconstruction while preserving its runtime reference. Source review separately
found and fixed phase capture before trusted append normalization, with direct
fresh-preparation coverage. Attempt2's two failures required an editor root in
the new merge fixture and explicit preparation invalidation in the new source
coordinator; generic default/live-dependency guards remain unchanged. Attempt3's
one allocation-test failure counted a one-time registry reserve. Warming one
sibling and requiring two equal positive measurements repairs the measurement;
all three failure cutpoints and prior-ready/state assertions remain. Production
is identical between attempts3 and4.

Evidence: `out/feature-parity/source-retirement-attempt4-validation-checkpoint.json`
(SHA256 `cbf4dced6e6e1622a6afe822846d155fd737c62b3f440d67dbced3e313a9da2e`)
and verified **1293-entry / 83,583,784-byte** archive SHA256
`fcf2b80f1d27027ab1bbcad9f986e4683924e2a98e2b53fe6c3eae2c44401630`.
All **204 enumerated source / 217 execution / 422 native inputs** verify; this
is not a complete compiler dependency graph. The failed build archive SHA256 is
`80718ea7361e648783086bb2d3c4f5936fd93088e4e43078a46dacdb9387efb3`;
failed component archives are
`84fcba559a3101e0f8634b00bd69eb46d805c67361901c6fbb3858608fd9cd70`
and `659cc9511ac82c8d458f304676b0b2f4fce30bb17fa1a6bbf601aa45c7fec209`.
The next-runtime/UI audit is implementation guidance, not acceptance.

**#108, #121 and the full goal remain active.** Remaining work includes exact
runtime sampled-row retirement/restore, actual image/payload reclamation after
readers/history, resource-only coordination without an authored material,
complete external/raw/cache/legacy-tail authority, simple Delete/capture/history
controls and new native/UI acceptance. `dependencyAuthorityComplete()` stays
false. All other original scope, including runtime/height representations,
per-binding normal-UV streams/export, separate base/coat normals, finite
evaluator/Mikk execution/storage caps and further textured/BIM/ray/DDGI coverage,
remains required. Changes are local and uncommitted.


## Runtime resource deletion and usable editor controls — 2026-10-29

Implemented and validated another #108/#121 slice. Both issues and the full
iterative goal remain open. Changes are local and uncommitted.

The existing ImGui panel now has a small independent Resources catalog with
selection, Delete unused resource, blocker feedback and the existing Undo/Redo
history. It works without a selected surface or authored material zero. The
capture API resolves current scene-owned identities with `--no-ui` as well.
Usage: `docs/editor-resource-workflow.md`.

Runtime retirement and original restoration preserve stable rows, exact
incarnations, aliases and source color/data roles. One bounded transaction
selects proved orphan source/runtime/image/sampler neighbors and preserves shared
neighbors. History retains selected actual image leases; preparations retain old
full owners and descriptor pools until outside publication. Both material edits
and resource edits use the shared descriptor preparation/writer. Runtime changes
publish frame and dependent renderer bindings; source-only changes avoid pool
and epoch churn. The preview cache holds weak owners and no logical pins.

Original import suspension preserves earlier Delete receipts through nested
Undo/Redo while replacement branches still invalidate old redo authority.
Loaded core texture replacement/clear also removes its selected legacy mirror;
strict raw-reference checks continue to count all remaining shadowed references.
Undo restores exact original aliases. Read precedence, unrelated slots, mapping
metadata and independent SG/MR definitions remain intact.

## Accepted validation

- Renderer SHA256:
  `4634f18904fb2608c3676109f77e104a11f54aa9f508c8637df9e7e7ea52e011`.
- **2,390 cases / 96 executables / 18 real Vulkan executables**; no failures,
  skips or disabled cases, with fresh copied XML. This adds **53 cases** over
  the accepted source-only baseline.
- The source resource suite has **16 real Vulkan cases + 3 allocation controls**.
  It samples future/current descriptors, protects survivor/shared/data-role and
  private fallback images, and proves final image capability invalidation plus
  VMA allocation count/byte reduction after the last selected owner drops.
  It also covers zero authored materials and loaded textured replacement with
  published A/E/private export/player-rest alias and exact Undo checks.
- Other added cases cover original runtime aliases and tail identity, nested
  branch vacancies, generic registration-owned suspension, noalloc publication,
  strict pointer grammar, GUI actions and typed capture rejection.

| Native family | Captures | Processes | CPU catalog checks | Image checks | Exports/reloads |
| --- | ---: | ---: | ---: | ---: | ---: |
| Material regressions | 122 | 40 | 122 | 138 | 4 |
| Texture regressions | 130 | 74 | 130 | 140 | 4 |
| New resource workflow | 78 | 16 | 68 | 78 | 4 |
| Total | 330 | 130 | 320 | 356 | 12 |

All nine native cohorts pass strict validation in forward/deferred raster. The
new workflow exercises ordinary Delete/Undo/Redo, older import replay, used/stale
failure atomicity, unchanged complete raster images and dense portable glTF/GLB
save/reload. The twelve exports comprise four glTF and eight GLB files. The 99
pure checker self-checks use unchanged Python inputs; their reuse is recorded
explicitly and is not native acceptance. GUI widget execution is covered by the
test suite; new Windows desktop Delete interaction was not performed.

Checkpoint:
`out/feature-parity/runtime-retirement-attempt6-validation-checkpoint.json`, SHA256
`48fe86f019661a3fae955c9c690bc92705ffaac2eb1294cb7c72dfa2735d3f5e`.
The verified **1,492-entry / 85,911,431-byte** primary archive has SHA256
`1a7ad27f7d884dd60956e01d529a81d1771c953cef1759c163638c513cc98d3b`.
The **211 enumerated source / 217 execution / 429 native input files** verify;
this enumeration is not a complete compiler dependency graph.

Five failed attempts remain preserved: fixture member-name compilation, nested
vacancy plus misplaced GUI assertion failures, headless dispatcher rejection,
the correctly blocked stale loaded-core alias, and duplicate test trace names.
Production fixes preserve strict dependency and branch-safety checks. Original
GPU/native oracles remain; the new GUI assertions were moved to their intended
resource queue test.

## Remaining scope

CPU source image payloads and the fixed canonical sampler cache remain retained.
`dependencyAuthorityComplete()` remains false. Complete external/unknown-schema,
BIM/ray/DDGI resource Delete acceptance, portable payload edge cases, runtime and
height representations, normal-UV streams/export, separate base/coat normals,
evaluator/Mikk execution/storage caps and every other original #108/#121 item
remain active. No original tracker checkbox is changed.

The next source audits identify the analogous Material append/Delete history
gap and persistent authored camera editing/current-view writeback for #109.
Those audits establish neither implementation nor acceptance.

## Material creation/Delete history and camera projection foundation — 2026-10-29

The material history gap is repaired. Make unique, a visible factor edit,
Assign original and Delete unused material can be undone and redone with the
same original material identity and Delete receipt. Original creation owns a
registration-born append capability; independent Delete commands retain normal
endpoint authority. Competing Delete cycles and stale prepared generations still
reject. Source-backed and genuine runtime-origin creation use the same bridge.

Stronger Vulkan tests exposed allocating work in existing final publication
checks. Material preparation now retains a bounded exact animation witness of
raw metadata, selected strided/sparse encoded inputs, decoder target context and
decoded curves/diagnostics. Both current and prospective players are checked.
Material authoring has allocation-free exact receipt/context/binding matchers.
Readiness and direct exchange reject changed raw channels, selected bytes and a
same-clock replacement player without publishing or allocating. Whole geometry
buffers and image payloads are not copied into the curve witness.

The new `GltfCameraAuthoring` module prepares perspective/orthographic projection
patches with independent authored/evaluated records, shared-node uses, exact
metadata, optional far/aspect semantics, finite budgets and immutable receipts.
Changed animated lens fields reject. This is a CPU foundation for #109: camera
editor controls, current-view writeback, current-player/rest ownership, Undo and
persistent save publication are not implemented by this module. The application
already consumes authored camera selection and parent-world/rolled projections.
See `docs/gltf-camera-authoring.md` and the next integration audit below.

Accepted renderer SHA256:
`6dcc1dbc0603f1ac8c1bc5330a0817d69cea1348e96289d2f2842ba71f08536d`.

| Fresh validation | Result |
| --- | --- |
| Selected component matrix | **2,435 cases / 97 executables / 18 real Vulkan executables**; zero failed, skipped or disabled. |
| Added cases over the 2,390-case milestone | **45**: camera 19 semantic + 3 controls; animation witness 10 + 3 controls; material matchers 4 + 3 controls; material GPU 3. |
| Focused current totals | Camera 22, material animation 49, material authoring 55, material retirement GPU 11 (8 Vulkan scenarios + 3 controls). |
| Native material matrix | **208 captures / 62 processes / 208 CPU catalog checks / 230 image checks / 8 exports and reloads**, forward and deferred raster; 4 glTF and 4 GLB exports. |
| New creation/Delete cohort within that native total | 86 captures / 22 processes / 86 catalog checks / 92 image checks / 4 exports and reloads; visible edited color, current clock, all four original history commands, used-Delete rejection and repeated original receipt replay. |
| Pure checker calibration | 92 checks; distinct from native acceptance, unchanged checker inputs across later C++ repairs. |
| Frozen enumerated inputs | 221 source / 218 execution / 440 native; verified again before collection. This is not the complete compiler dependency graph. |

The previously accepted `4634f189` renderer reproduced the final Redo Delete
failure before the repair. Its reproduction and an initial fixture setup error
are preserved separately. Attempt 1 completed 2,414 cases with three publication
failures, exposing allocating final validation. Attempt 2 completed 2,435 cases
with one new test setup failure: descriptors were prepared twice for the same
transaction. The fixture now commits its already prepared transaction directly
under allocation denial. Both failed runs were archived before repair; no
production guard or stale-input oracle was weakened. Attempt 3 passed the full
component and native matrix.

Evidence: `out/feature-parity/material-history-attempt3-validation-checkpoint.json`
SHA256 `5202a4f6d156d31ab157b8a68832786aaa242ff91cd542348af91ce3064db7cd`;
verified 909-entry, 164,154,189-byte archive SHA256
`f701f26fd4a58239a772d418928a8936470bc0e403c97d2e651f3509f7375cbf`.
Usage: `docs/editor-resource-workflow.md`. Source reviews:
`out/feature-parity/material-history-review.md` and
`out/feature-parity/material-history-next-camera-integration.md`.

No new Windows desktop Delete interaction or camera editor/save acceptance was
performed. Prior resource lifetime/native acceptance remains separate. CPU source
image payload cleanup, complete external/provider/BIM/ray/DDGI resource coverage,
portable payload edge cases, normal/displacement/UV representations, evaluator
caps and every remaining original #108/#109/#121 requirement stay active.
`dependencyAuthorityComplete()` remains false. Original issue checkboxes remain
unchanged. Changes are local and uncommitted.

## Usable authored camera projection editor — 2026-10-30

The existing **Scene Controls → glTF Animation → Camera** panel now exposes
perspective/orthographic projection, vertical FOV in degrees, automatic/fixed
aspect, near/far clipping, infinite far and independent orthographic X/Y
magnitudes. **Apply camera edit** creates one Undo/Redo action. Shared camera
definitions show their use count. Animated lens fields explain their read-only
status; unanimated fields remain editable during playback, and a draft survives
ordinary animation sampling. No new panel, UI dependency or renderer was added.

The selected transaction publishes authored/evaluated/player-rest records with
private definition and node-binding witnesses. Original history commands rebase
only their owned fields against the current animation clock. Stale source,
graph, player, encoded curve, namespace or viewport proofs reject before
publication. Final guards and selected-record exchanges allocate nothing.
Camera revisions are independent of material/resource revisions and animation
sampling; projection edits preserve graph poses, descriptors and geometry.

Named/indexed capture selection and explicit projection patches use the same
backend. Valid frame-one selection now initializes the current graph's
animation/rest owner before the first lazy animation update. Both public save
paths use the shared exporter. Its bounded staged compatibility rewrite omits
the typed perspective infinite-far sentinel instead of writing invalid
`zfar: 0`, including camera-only scenes. Positive finite far, orthographic
zero near, opaque metadata and the complete GLB BIN/unknown-chunk tail survive.

Accepted renderer SHA256:
`877ad3064aef14b960a2fe24391dfcf4dcd7913e4dcd7bab819ca5d56cc5d242`.

| Validation | Result |
| --- | --- |
| Component gate, executed in attempt 5 | **2,487 cases / 99 executables / 19 real Vulkan executables**; zero failed, skipped or disabled. |
| Added cases over the 2,435-case milestone | **52**: camera planner/rest 13, UI 14, backend/GPU 12, capture grammar 9, raw serialization 4. |
| Focused camera totals | Planner/rest 35; UI 14 including three actual headless ImGui widget cases; backend 12 including eight Vulkan scenarios, one CPU inverse contract and three controls. |
| Fresh native camera matrix, attempt 8 | **104 captures / 44 processes / 104 CPU state checks / 126 image checks / 4 portable exports and reloads**, forward and deferred raster; two glTF and two GLB. |
| Material regression matrix | **208 captures / 62 processes / 208 CPU state checks / 230 image checks / 8 exports and reloads**, four glTF and four GLB. |
| Prior animation regression matrix | **20 captures / 20 processes / 10 changed-image pairs**, five clips in both raster paths. |
| Total native work | **332 captures / 126 processes / 12 exports and reloads**; six glTF and six GLB. |
| Pure checker calibration | **137** checks, separately recorded; not native acceptance. |
| Frozen enumerated inputs | **156 component / 221 execution / 391 native**; not the complete compiler dependency graph or machine environment. |

The C++/shader/header/configuration and execution files in the accepted native
run are byte-identical to the passing attempt-5 component gate. That gate was
explicitly reused rather than rerun after native checker repairs. The only
enumerated source delta is the orchestration Python assertion for two added
scene-aware literal controls, proven by an exact replacement against the
earlier frozen source archive. See
`out/feature-parity/camera-editor-attempt8-component-reuse.json`.

Earlier development and failed runs are preserved before repairs. They include
a fixture root precondition, the real frame-one selection ordering and
infinite-far serialization defects, and three checker assumptions: flat versus
canonical owner/primitive hierarchy, scene-switch selection outlines, and
source versus serialized material slots. Corrected oracles retain exact RGB
comparison and strict semantic geometry/material/animation ownership. Original
checker bytes and calibration records are preserved. A separate collection-only
script reads the legacy animation report's nested `checks` fields; the frozen
native inputs and all completed outputs remained unchanged.

Evidence: `out/feature-parity/camera-editor-attempt8-validation-checkpoint.json`
SHA256 `81559b7cf3ba5ac440cb82ad4079bfbd1a4482f8b41d8723db2a1c8a0b997740`; verified
1297-entry, 661,888,676-byte primary archive SHA256
`fe95d9a1e6c792a45ddc2d9febcc7ba928d85bdcbaafd04f5ce652355eb0c20d`. The final preservation supplement
also retains every enumerated native input's actual bytes, documentation and
exact issue readbacks.

Usage: `docs/gltf-camera-authoring.md` and `docs/current-editor-controls.md`.
Read-only next-slice designs:
`out/feature-parity/camera-write-view-next-slice.md` and
`out/feature-parity/source-image-payload-next-slice.md`.

No Windows desktop camera interaction was performed; UI evidence is headless
ImGui interaction plus backend/native rendering. **Update from view** and
editing animated lens rest values remain open in #109. CPU source image payload
cleanup, complete external/provider/BIM/ray/DDGI resource acceptance,
normal/displacement/UV representations, evaluator limits and all other original
#108/#109/#121 scope remain active. `dependencyAuthorityComplete()` remains
false. Original issue checkboxes are unchanged. Changes are local and
uncommitted.

## Write viewport camera pose, keep lens — 2026-10-30

The existing camera controls now retain a weak authoring destination when the
user returns to **Viewport camera**. **Write viewport pose (keep lens)** writes
the actual unjittered navigation pose to that camera node and leaves free
navigation active. It preserves the shared lens, sibling camera poses, parent
placement and the current world scale/shear/reflection residual. Roll and the
checked parent-world-to-local conversion use the existing camera evaluator.
One changed write is one shared Undo/Redo action; replay preserves today's
clock, parent transform, lens and unowned weights. No-op writes add no entry.

Selected authored/evaluated/player-rest pose arrays and bounded prepared graph,
deformation, ECS, light and bounds payloads publish through checked exchanges.
Node births distinguish original restored nodes from equal fresh replacements.
Opaque ECS contexts stay with their live owner. Current source/view/namespace,
curve, raw graph/light and consumer proofs reject stale preparation without a
partial edit. History retains selected receipts, not a complete model/player.

| Accepted gate, attempt 10 | Result |
| --- | --- |
| Renderer SHA256 | `61428d94bf1ed771647ce9dadadcd80fc8c89f26bb3ae3a8c88345095eb351ea` |
| Fresh component gate | **2,587 cases / 107 executables / 22 real Vulkan executables**, zero failed/skipped/disabled. |
| Added over the 2,487-case camera-editor milestone | **100**: 93 cases in eight new pose/lifetime/UI/ECS/GPU targets, 3 capture grammar cases, 4 shared light-export regressions. |
| Dedicated native pose matrix | **150 captures / 70 processes / 150 CPU state checks / 172 image checks / 6 portable exports and reloads**, both raster paths; 2 glTF and 4 GLB. |
| Existing projection regression | **104 captures / 44 processes / 104 states / 126 images / 4 exports and reloads**. |
| Material regressions | **208 captures / 62 processes / 208 states / 230 images / 8 exports and reloads** across six cohorts. |
| Existing animation regression | **20 captures / 20 processes / 10 changed-image pairs**, five clips in both raster paths. |
| Native aggregate | **482 captures / 196 processes / 18 exports and reloads**. |
| Pure checker calibration | **324 checks**, separate from native acceptance. |
| Frozen inputs | 174 curated component inputs, 229 execution files and 418 native inputs. The curated list is not a complete compiler dependency or machine-environment inventory. |

Validation found and fixed two production defects. Tiled-light scratch
replacement now invalidates prepared pose lighting before retiring buffers;
same-grid resizes remain valid and legacy history can still rebind current
scratch. The shared light exporter now constructs carrier locals with checked
double affine algebra and verifies actual binary32 world recomposition. This
prevents a saved carrier's homogeneous `1` rounding down and failing the strict
loader. Both glTF/GLB round trips pass. Extreme overflow and finite wrong-world
cases reject atomically, while ordinary translation cancellation stays usable.

The native checker now follows the actual normal-capture light schema, exact
cross-clip STEP-rest export contract and canonical imported-light carrier
hierarchy. It retains strict original keys/metadata, raw affine rows, semantic
targets and full-RGB comparisons. Original checker versions, failed outputs,
diagnoses and independent negative calibrations are preserved. Attempts 6–8
reused the earlier component gate only after exact source/execution equality;
the accepted attempt-10 component gate and native runs are fresh.

Evidence: `out/feature-parity/camera-pose-attempt10-validation-checkpoint.json`
SHA256 `c94bedce2b80967a86abc1700e32855224c55f713d9254c4f3f9e0c254a70f32`.
Primary archive: 1,776 entries, 639,274,640 bytes, SHA256
`0f385a1517f4bf3949a7f2aaae7c3f6fe664276b8b2e19d224d7fbb86001b2e6`.
The final supplement retains all 418 enumerated native inputs' actual bytes,
docs and exact issue readbacks. The two light-export files added to the curated
scope after attempt 8 have only explicitly labeled reconstructed earlier
preimages; their old raw bytes were not captured. Failed binaries, logs, models,
checker and all previously enumerated inputs remain preserved.

Usage: `docs/gltf-camera-authoring.md` and `docs/current-editor-controls.md`.
No Windows desktop interaction was performed; UI acceptance uses actual
headless ImGui widgets. Native pose fixtures cover primary glTF scenes, not
every external/provider/BIM/ray/DDGI combination. Independent physics/provider
transform owners and selected-node pose animation reject with a reason.
Animated lens-rest authoring, selected pose animation authoring, complete
resource acceptance and all other original tracker scope remain open.
The next requested P1 increment is existing light editing/gizmo Undo/Redo;
preparation is in `out/feature-parity/light-history-*-prep.md`. Changes are local
and uncommitted; original issue checkboxes remain unchanged.


## Light editing and gizmo history

Scene Controls records light field edits, manual light creation, and completed
viewport gizmo drags in shared Undo and Redo. Cancel restores the preview.
Undoing an imported override reveals the source at the current animation time.
World-space light gizmos use the world axes. Native validation caught a
spotlight rotating around its own beam when world Y was selected.

Renderer `37771d53` passed 2,683 cases in 114 executables, including 24 Vulkan
executables. The native light suite passed 100 captures in 42 processes, 100
state checks, 122 image checks, and four glTF/GLB exports and reloads across
deferred and forward raster. This includes animated override replay and
selection through exported light carrier nodes. The broader regression run
uses the same build. Evidence is in
`out/feature-parity/light-edit-attempt8-light-checkpoint.json`.

The complete light-build regression passed on renderer 37771d53, including
light, camera pose, camera projection, materials and animation. It covered
582 captures in 238 processes and 22 portable exports and reloads. The frozen
inputs and result summaries are archived in
`out/feature-parity/light-edit-attempt8-frozen-inputs.zip`.

The subsequent normal build added the Vulkan depth probe and forward
transparent-pick pass. See `docs/gltf-deformation.md` for the native depth
verification.

Portable scalar texture export (#89) now supports custom runtime transmission
channels on retained core Data images. The export copies the chosen channel
to red in a detached PNG and preserves the sampler, UV transform, factor,
and other consumers of the original image. Legacy bindings keep their
existing retained-source path. Authored mip chains, alternate codecs and
unproven texture roles still reject the repack.

The material and scene export suites passed 56 cases. The Vulkan-backed
material resource suite passed 36 cases, including a runtime transmission
material that shared its original image with occlusion. Both glTF and GLB
reloaded with the expected texel bytes and sampler, and save preserved the
live source model. Logs: `out/feature-parity/scalar-export-build.log` and
`out/feature-parity/scalar-export-gpu.log`.

The detached scalar repack now covers all nine fixed-channel extension slots
in the existing material exporter. Equivalent source/channel copies share
one export texture within a save. The Vulkan-backed resource suite passed
36 cases, including glTF and GLB reload checks for all nine slots sharing
one Data image. The original occlusion texels and sampler remain intact.
Log: `out/feature-parity/scalar-export-all-slots-gpu.log`.

Vulkan renderer `6a55765a` rendered both saved formats identically to a
reference with independently encoded scalar PNGs in deferred and forward
raster. A reference that sampled the original red channel for transmission
changed 10,440 pixels in each path. The rendered comparison directly
exercises transmission. The Vulkan material resource test checks the saved
texels for all nine slots. Set `CONTAINER_SCALAR_EXPORT_FIXTURE_DIR` when
running that test to retain the exports for
`tests/validation/gltf_scalar_export_runtime_regression.py`.

## Display tone mapping and grading

The **Post-process** section offers the existing ACES fit, per-channel
Reinhard, linear clipping, Hejl–Burgess-Dawson Filmic, Hable Uncharted 2,
the minimal AgX fit, and Khronos PBR Neutral in both raster techniques.
Captures can select these with
`--tone-map aces|reinhard|clip|filmic|uncharted|agx|neutral`,
`--saturation F` and `--contrast F`.

The final display pass composites scene color and bloom in HDR, applies
exposure, then evaluates the selected tone curve. Reinhard uses
`x / (1 + x)` per nonnegative channel. Clipping clamps each channel to
`[0, 1]`. Grading mixes display-linear RGB with Rec. 709 luminance using
saturation in `[0, 2]`, then applies contrast in `[0.5, 2]` around linear
0.18. Both controls default to 1. The sRGB attachment performs the transfer
function once. Raw and data diagnostic views bypass the new controls.
Filmic and AgX decode their display-encoded results to linear light before
grading. Uncharted 2 uses the reference exposure bias of 2, white reference
11.2 and display power 1/2.2 before the same decode. PBR Neutral takes
nonnegative Rec. 709 and compresses the peak after its shadow offset.
The AgX selection is a matrix and sigmoid approximation of the base look.

The **Color Grading** section offers a white balance temperature of
2,000–15,000 K, CIE 1960 Duv tint, vibrance, and separate shadow and highlight
RGB multipliers. White balance adapts the specified illuminant to D65 in
scene-linear light before the tone curve. The CPU computes the three
CMCCAT2000 gains once for the pass. Vibrance modifies less saturated colors
more strongly. Split toning blends the two RGB multipliers by display-linear
luminance. The corresponding capture flags are `--white-balance`, `--tint`,
`--vibrance`, `--shadow-tone`, and `--highlight-tone`.

**Save Preset** and **Load Preset** in **Color Grading** write and read a
versioned JSON file containing the tone curve and all grading controls.
The preset is scene-independent. Exposure stays in its existing controls.
The preset path can be absolute or relative to the working directory and its
parent directory must exist. Loading invalid data preserves the active grade.

Use `--display-grade FILE` to apply the same preset to a capture. Grading
options are applied in command-line order, so flags after the preset override
its values. Use `--save-display-grade FILE` to write the resolved grade and
exit before starting Vulkan.

Vulkan build `65b1624d` passed 209 convention and post-process tests. On
an RTX 2080 SUPER, 12 native captures across forward and deferred raster
matched 48 independently calculated sRGB samples within two code values.
Neutral ACES was pixel-identical to the two pre-change baseline captures.
There were no Vulkan validation errors. The regression is
`tests/validation/display_grade_runtime_regression.py`.

Renderer `09084ff1` passed 22 captures and 88 analytic RGB comparisons
across both Vulkan raster techniques. The expanded chart covers all seven
curves and checks that Filmic grading operates in display-linear light.
Neutral ACES still matches the pre-change captures pixel for pixel.
All 209 convention and post-process tests passed. The new fragment
also passed standalone SPIR-V validation for Vulkan 1.3.

Renderer `efc0b988` passed 210 convention and post-process tests and 30
Vulkan captures covering 120 analytic chart samples. Both raster techniques
matched the expected white balance, tint, vibrance and split toning. The
neutral ACES captures still matched the pre-change baselines exactly.
SPIR-V validation and disassembly confirmed the push layout through byte 127.

Renderer `f383c1b4` passed 233 convention, post-process and DDGI preset
tests. The shared atomic writer retained DDGI's replacement and concurrency
behavior. The grading preset checks covered Unicode paths, exact round trips,
CLI override order, malformed inputs and blocked replacement. Forty-eight
Vulkan captures across the raster techniques passed 344 analytic chart and
HDR ramp comparisons within two sRGB code values. Both preset reload images
matched the explicit-flag images exactly, and neutral ACES matched the
pre-change baselines. There were no Vulkan validation errors.

## HDR environment file loading

`--environment FILE` selects an equirectangular OpenEXR or Radiance HDR image.
Both codecs feed the existing Vulkan cubemap, irradiance and specular
prefilter passes. File reads preserve Unicode paths and the decoded pixels
retain their linear values. The loader bounds encoded and decoded data at
256 MiB and each dimension at 16,384 texels, subject to the device limit.
Single-part, non-deep EXR is supported. Non-finite values and values outside
the half-float cubemap range are rejected. Explicit startup selection reports
an error on failure. The bundled EXR remains the default.

Renderer `55f7a1c2` passed 196 convention tests and eight Vulkan captures.
Equivalent HDR and EXR panoramas rendered identically in forward and deferred
raster. A different panorama changed the background and IBL receiver.
The bundled default matched both pre-change captures pixel for pixel.
Malformed, missing, oversized and out-of-range files failed cleanly under
Vulkan validation. See `tests/validation/hdr_environment_runtime_regression.py`.

In **Scene Controls → Environment**, enter an EXR or HDR path and select
**Load Environment**. File decoding runs on one background worker. The
render thread submits Vulkan map generation with its own fence. It keeps
the current environment active until a later frame observes completion.
Publication runs after the frame fences and async DDGI readers retire,
then updates both raster and probe descriptors and resets both histories.
A rejected file preserves the active map and histories.
Map generation shares the graphics queue, so it can still increase GPU
frame time during a load.

Renderer `bae471bb` passed 196 convention tests, the eight startup
captures, and 24 live captures across four Vulkan processes. Each live
sequence exercised four replacements, one malformed file and a resize.
Both raster techniques passed with ray queries disabled and with actual
asynchronous DDGI updates, with synchronization validation enabled.
The malformed load kept both the active file and the existing history.

The **Environment** section also offers rotation around world +Y,
background blur, and a solid background color. Positive yaw rotates a sky
feature from +Z toward +X. Every environment lookup uses the inverse yaw,
including diffuse irradiance, specular and clearcoat reflections, transmission
fallbacks, and DDGI miss rays. The cubemaps stay resident while yaw changes.
Blur selects a mip from the prefiltered map for the visible sky. The solid
color replaces the visible sky in scene-linear RGB and uses the normal
exposure and tone mapping. Both background controls are independent of IBL
and probe transport.

Captures can reproduce those settings with `--environment-yaw DEGREES`,
`--background-blur F`, and `--background-color R G B`.

Renderer `41fdd5ba` passed 196 convention tests and 50 sampled Vulkan
captures covering startup, presentation and live changes. At 90 degrees,
the sky region matched a baked panorama exactly and the reflective receiver
matched within one sRGB code value. Blur and solid color preserved every
receiver pixel in the tested region. Live yaw changes reset temporal and
asynchronous DDGI histories, with 13 probe updates published in each raster
path during the live sequence. The startup cohort still matches the original
bundled EXR.

Renderer `1e12e41f` passed 196 convention tests and 82 sampled Vulkan
captures covering the same startup and presentation checks plus fenced live
generation. Each of the four live processes captured a frame with the old
maps while the new generation was submitted, then verified publication,
history reset, invalid-file rejection, and resize. The two async DDGI
processes passed with Vulkan synchronization validation enabled.

## Procedural plain and gradient skies

In **Scene Controls → Environment → Procedural Sky**, select a mode,
choose scene-linear colors and select **Apply Sky**. Plain uses one
color over the sphere. Gradient interpolates by elevation from the horizon
to the zenith or ground. The Vulkan compute pass writes the procedural
radiance directly into the existing environment cubemap and derives the
same irradiance and prefiltered maps used for HDR files. Live changes use
the same fenced publication and history resets as HDR replacement.

Captures can use `--sky plain|gradient` with `--sky-horizon R G B`,
`--sky-zenith R G B` and `--sky-ground R G B`. Colors are in `[0,1]`
before environment intensity. HDR file selection remains available in
the same panel.

Renderer `14644f31` passed 196 convention tests and 24 sampled Vulkan
captures. Plain and gradient skies matched equivalent EXR references
within two sRGB code values across both raster paths. Live EXR → plain →
gradient → EXR transitions reset temporal and async DDGI history with
synchronization validation enabled. Each raster path published ten async
probe updates. The bundled EXR still matched the pre-change captures.
These procedural modes reuse the existing IBL maps without additional LUTs.

**Atmosphere** adds spherical single scattering to the same compute pass.
Rayleigh and Mie density fall exponentially with altitude. Ozone absorbs
light within a band centered at 25 km. The pass integrates extinction
along the viewing and solar paths and includes a Lambertian ground term.
The sky bake evaluates six 128×128 faces with 24 view samples and 12
solar samples per view sample. It uses no additional atmosphere LUTs.

Sun azimuth is measured from +Z toward +X. The controls also set elevation,
solar irradiance, observer height, planet radius, atmosphere thickness
and the three density multipliers. The environment maps contain scattered
sunlight and ground reflection. The visible sky draws a 0.267° solar disk
using the transmitted irradiance divided by the disk's projected solid angle.
The disk is limited to the HDR render target's finite range and stays out of
the lighting bake. With **Sun Lights Scene** enabled,
the directional light uses the same transmitted irradiance and follows sky yaw.
Environment intensity scales both. The directional intensity CLI override
acts as an extra direct-light multiplier in this mode. Disable the link with
**Sun Lights Scene** or `--sky-no-direct-sun` to keep independent directional
lighting. Multiple scattering remains open.

Renderer `885a86b3` with atmosphere compute shader `c8708bbe`
passed 196 convention tests and 44 sampled Vulkan captures. The 18
atmosphere captures matched 90 sky samples from higher-resolution CPU
quadrature within one sRGB code value. Live HDR, procedural sky and
atmosphere transitions passed on both raster paths with async DDGI and
synchronization validation. GPU generation, including IBL filtering,
took 1.75–2.60 ms on the RTX 2080 SUPER. The atmosphere adds no LUTs,
uses a 1 MiB transient radiance cubemap and retains the existing IBL maps.

Renderer `da64eefe` passed 196 convention tests and 56 sampled Vulkan
captures after linking the sun. Both raster paths matched the independently
integrated solar irradiance and the visible disk color. They also verified
yaw alignment, planet occlusion and independent directional lighting when
the link is disabled. Live async DDGI transitions installed the linked sun
on atmosphere publication and restored the independent light on return to HDR.

**Use Location and Time** in the Atmosphere controls derives the geometric
sun position from latitude, longitude, a local date and time, and an
explicit UTC offset. North is +Z and east is +X before environment yaw.
The calculation uses the Gregorian calendar from 1901 through 2099 and
handles offsets across midnight. The offset is local time minus UTC, in
minutes. Set it to the offset in effect at the intended instant, including
daylight saving where applicable.

Use `--sky atmosphere --sky-location LAT LON --sky-date YYYY-MM-DD
--sky-time HH:MM --sky-utc-offset MINUTES` for the same capture controls.
The derived direction drives the bake, visible disk and linked direct light.
The calculation follows the [NOAA solar calculator equations](https://gml.noaa.gov/grad/solcalc/calcdetails.html).

Renderer `d4c64dc1` passed 196 convention tests and 22 sampled Vulkan
captures. Both raster paths matched the Colorado solar position reference
within 0.3 degrees, rendered UTC rollover representations identically, and
verified the midnight sun and polar night. Invalid calendar dates, times,
coordinates and offsets were rejected. The manual solar lighting regression
also passed.

**Procedural Sky → Sky Presets** saves and loads a versioned JSON file
with the mode, colors, atmosphere parameters and manual or geolocated
sun controls. Loading fills the draft. Select **Apply Sky** to publish it.
The preset path is relative to the working directory unless absolute,
and the parent folder must already exist. Environment intensity,
rotation and background controls are configured separately.

For captures, `--sky-preset FILE` loads the same settings. Later sky
flags can override them. `--save-sky-preset FILE` saves the resolved sky
and exits before Vulkan starts.

Renderer `43fbfa51` passed 196 convention tests and 14 Vulkan captures.
Preset captures matched the explicit sky flags pixel for pixel in both
raster paths. The preset regression checked Unicode paths, round trips,
override order, malformed input and failed publication. The shared
display-grading preset validation and solar-time captures also passed.

**Aerial Scale (m/unit)** converts camera-to-surface distance to metres
for atmosphere transport. Set it to 1 for a metre-scale scene or 0 to
disable the effect. The raster shaders attenuate surface radiance and
add scattered sunlight along that finite ray segment before compositing
and tone mapping. Opaque and transparent geometry use the same atmosphere.
Orthographic views use parallel camera rays. The sky background continues
to use the full atmosphere bake.

Use `--sky-aerial-scale F` in captures. Sky presets store the scale in
version 2. Version 1 presets load with aerial perspective disabled.

Renderer `eaaaa377` passed 196 convention tests and 34 sampled Vulkan
captures. Aerial transport matched the CPU reference within three sRGB
code values for opaque and transparent surfaces in both raster paths.
Zero scale matched four pre-change images pixel for pixel. The upgraded
preset and the full sky-radiance regression also passed.

**Use Authored Sky** in the Procedural Sky panel previews the selected
glTF scene's optional [OMI sky metadata](https://github.com/omigroup/gltf-extensions).
`--gltf-sky` opts in at startup and can be combined with `--gltf-scene N`.
The preview supports plain and gradient colors in `[0,1]`, gradient
curves in `[0.001,1]`, and a unit rotation quaternion. The selected
scene's sky index defaults to zero if no reference is supplied.

The extension is a draft. Required declarations still fail the glTF
capability gate. Optional physical and panorama entries are diagnosed,
and this preview does not evaluate draft ambient or solar appearance
fields. Malformed entries keep the current environment, or use the
bundled default at startup. After changing glTF scenes, select **Use
Authored Sky** again to preview that scene's settings. Version 3 sky
presets retain the gradient curves and orientation. Older presets
load with a linear, upright gradient.

Renderer `094b9e66` passed 196 convention tests and 36 sampled Vulkan
captures. Both raster techniques matched the authored plain sky exactly
and the tilted gradient within one sRGB code value of the independently
encoded EXR. Optional malformed and unsupported entries matched the
opt-out fallback. The required declaration was diagnosed, and the sky
preset migration and asynchronous DDGI transition regressions passed.

**Multiple Scattering** in the Atmosphere controls adds indirect
sunlight to the baked sky, its image-based lighting, and the aerial
perspective on opaque and transparent geometry. The capture flag is
`--sky-multiple-scattering`. Version 4 presets save the checkbox.
Versions 1–3 load with it off.

The implementation approximates higher scattering orders with
isotropic radiance and a bounded geometric series. A 16×8 table
samples solar zenith cosine and altitude, with denser spacing near
the horizon and ground. The CPU integrates 24 directions per entry.
The table occupies 2 KiB in the existing environment bake texture
and adds 2 KiB to each lighting uniform buffer. The bake and raster
shaders interpolate the same values. The fenced environment
publication pairs the table with the corresponding sky maps and sun
settings. The approximation uses local feedback and includes direct
sunlight reflected by the ground in the higher-order source.
[Atmosphere rendering paper](https://sebh.github.io/publications/egsr2020.pdf)

Renderer `7db734b9` passed 196 convention tests and 84 Vulkan
captures on an RTX 2080 SUPER. The higher-order capture matrix
checked sky radiance, opaque and transparent aerial haze, a solid
background override, zero solar irradiance, night, and zero
scattering. Both raster paths measured the same RGB gains at the
sampled pixels. Presets reproduced the enabled CLI settings pixel
for pixel. Live transitions from multiple scattering through plain,
gradient, single scattering and EXR reset temporal and asynchronous
DDGI history in both raster techniques. The single-scattering
references and bundled EXR baseline also passed.

The enabled sky bake and IBL generation took 1.95–2.60 ms on the
GPU across six normal captures. Building the table took 9.47 ms
per call in a standalone Release benchmark on the test host. For
the default atmosphere, the coarse table's relative L1 error was
8.13% across RGB entries against a denser evaluation of the same
isotropic model using 256 directions and 48 path and solar samples.
The shader changes ship through the existing SPIR-V directory,
and the table is generated from settings at runtime.

Capture logs and telemetry are under `out/feature-parity/multiple-*`.

### Scene image export (#115)

Use **Scene Controls → Export Image** to save a **Scene EXR**,
**Viewport PNG**, or **Window PNG**. The CLI uses `--screenshot PATH`
for EXR or window PNG and `--screenshot-viewport` for viewport PNG.
Select Lit view for scene EXR. Every image gets a `.capture.json`
sidecar describing its dimensions, color encoding, and exposure settings.

EXR saves float RGB from the scene composite after transparency and
before bloom, exposure, and grading. It takes the same temporal
resolve as the display pass when TAA is active. The Vulkan post-process
shader writes the requested radiance into a host-readable buffer, and
the capture waits on the submitted frame's fence before encoding.
Viewport PNG uses the regular display transform and omits the
post-process editor overlays for the capture frame. Window PNG includes
the application's rendered UI.

Reference path-tracer sample-series export is tracked with #47.

Renderer `72200a51` passed 196 rendering convention tests, two
screenshot recorder tests, and 20 synchronization-validated Vulkan
captures. The forward and deferred EXRs reloaded opaque and blended
emissive radiance, preserved orientation, and were pixel-identical
across different display settings. Both paths captured TAA after a
live swapchain resize and saved Unicode filenames. The viewport PNG
changed with the display settings and excluded the GUI present in
the window PNG. Four standard PNGs matched the previous renderer
pixel for pixel. Evidence is under
`out/feature-parity/linear-capture-verified` and
`out/feature-parity/linear-capture-png-regression`.

### Runtime Slang shader reload (#113)

Press **F5** or select **Reload Shaders** in Scene Controls. A source
build needs its original CMake build directory, Slang compiler, and
shader sources. The normal shader build records compiler dependencies
and interface reflection for each graphics stage. The background
reload compares dependency contents and compiles affected stages
into a temporary generation. The renderer checks their reflected
interfaces against the build before creating a candidate pipeline set.

The renderer publishes that set at a frame boundary after retiring
submitted Vulkan work, then resets temporal and DDGI history.
Compilation or pipeline creation failure preserves the current
generation. The error message identifies the failed shader. Rebuild
the application for interface changes. Packaged builds report that
the source-build manifest is unavailable when reload is requested.
Reload covers pipelines created by the raster graphics builder and
the ray-shadow trace/filter and DDGI trace/update compute pipelines.
Ray-shadow and DDGI candidates use the existing descriptor layouts.
The renderer retires asynchronous DDGI work before publishing a
prepared reset and the replacement pipelines. The ray-shadow manager
also retains the active shader directory for later scene generations.
The shared pipeline manager also reloads compute used by GPU culling,
BIM, tiled lighting, GTAO, environment baking, bloom, exposure, TAA,
glTF deformation, and OpenVDB. It compares the candidate SPIR-V
against each live compute pipeline and stages replacements for changed
binaries. Detached culling generations share the same pipeline slot.
New compute owners use the active shader generation when they are
created after a reload.

Reload also stages temporal velocity cull variants, auxiliary
anisotropy/diffuse-transmission cull variants, and the DDGI diagnostic
overlay. These owners retain their existing layouts and rendering
formats during a reload. Later TAA initialization and auxiliary
resource recreation use the active shader directory.

Changes to the environment conversion, irradiance convolution,
specular prefilter or BRDF integration shader now trigger a detached
bake. The active environment retains its encoded HDR bytes or complete
procedural sky settings. The bake uses that snapshot and candidate
pipelines, then publishes the new textures with the shader generation
after readers retire. Descriptor bindings and DDGI environment
snapshots refresh before the previous textures are released.

Compilation or baking errors retain the active generation. Unchanged
bake pipelines reuse their current textures. Retaining the encoded
HDR source adds its file size to host memory, bounded by the existing
256 MiB input limit. Capture telemetry records C-handle RAII object
counts and VMA allocation counts and bytes for reload retirement checks.

Renderer `cd65f8e9` passed 196 convention tests and 16 Vulkan
captures across six synchronization-validated processes. Editing
an include in an isolated source tree rebuilt its two dependent
Slang stages and changed rendered pixels in both raster paths.
The published graphics set survived resize, a change to 4× MSAA,
and a second reload. Ray-shadow tracing and asynchronous DDGI
executed on both paths, and the reload reset DDGI history. Invalid
source, a descriptor binding change, and a missing build manifest
each kept the previous image and returned a diagnostic. Evidence
is under `out/feature-parity/shader-reload-verified`.

Renderer `bd3bf81d` passed 196 convention tests and 22 Vulkan
captures across nine synchronization-validated processes. A live
ray-shadow trace edit changed the directional visibility debug
sample from 232 to 115 in both raster paths. Each path reset
ray-shadow history and published asynchronous DDGI updates after
loading a candidate that included an edited DDGI update shader.
A malformed DDGI candidate preserved the previous ray output and
generation. The graphics, resize, MSAA, interface rejection and
packaged-build regressions also passed. Evidence is under
`out/feature-parity/shader-reload-compute-verified`.

Renderer `200cff87` passed 196 convention tests and 30 Vulkan
captures across eleven synchronization-validated processes. In
both raster paths, editing temporal composition replaced one
managed compute pipeline and changed the center red sample from
206 to 115. The result survived a live resize. Reloading the same
source again replaced zero managed compute pipelines. The graphics,
ray-shadow, asynchronous DDGI, and failure-preservation captures
also passed. Evidence is in
`out/feature-parity/shader-reload-managed`. The Release validation
build had OpenVDB disabled.

Renderer `391ba59b` passed 196 convention tests and 43 Vulkan
captures across sixteen synchronization-validated processes.
Editing temporal velocity changed the center motion-view red sample
from 188 to 218 in both raster paths and survived live resize.
Editing auxiliary diffuse transmission changed the deferred blue
sample from 215 to 172, including after a 4× MSAA recreation.
Both raster paths also displayed the edited DDGI probe markers.
The preceding graphics, compute, history reset, interface rejection
and packaged-build checks passed on the same executable. Evidence
is in `out/feature-parity/shader-reload-graphics-verified`.

Renderer `a4dca1fa` passed 196 convention tests and 79 Vulkan
captures across 27 synchronization-validated processes. Both
raster techniques showed live edits to HDR conversion, curved
gradient skies, diffuse irradiance, specular prefiltering, and the
BRDF lookup table. The HDR cases removed the source file after
loading and still matched the expected rebaked sky, using the
retained encoded snapshot.

Ten repeated-bake comparisons retained exactly the same C-handle
RAII object inventory and VMA allocation count and bytes. A further
reload sequence switched deferred → forward → deferred and resized
with stable image samples and resource inventory. Earlier graphics,
compute, diagnostic, interface rejection and packaged-build checks
passed on the same binary. Evidence is under
`out/feature-parity/shader-reload-complete`. These counters cover
objects in the C-handle registry and VMA allocations. Direct
Vulkan-Hpp owners retain their own RAII lifetime, and driver-internal
memory is outside this measurement.

The same binary passed six further Radiance/OpenEXR captures.
Equivalent files matched pixel for pixel, alternate sources changed
the sky and IBL, and malformed, missing, oversized and
unrepresentable inputs were rejected. Evidence is under
`out/feature-parity/shader-reload-hdr-regression`.

## GPU hierarchy propagation foundation (#119) — 2026-11-04

Added an opt-in Vulkan compute path with topological levels, parent indices,
sparse local-transform uploads and per-image buffers. It writes current model
and inverse-transpose normal matrices into the existing object SSBO for both
raster techniques. CPU mirrors, bounds, winding routing, previous models and
temporal identities retain their current contracts. Skinning/morph evaluation
and CPU ray-provider extraction remain separate.

Successful graphics submission commits the world cache. Unchanged hierarchies
skip level dispatches; changed compute pipelines invalidate that cache.
Discarded recordings cannot publish uncomputed worlds. CPU switches and
fallback restore each image's CPU object data before TAA adds submitted history.
Resize retires the old buffers, and scene replacement rebinds new object SSBOs.
Capacity, topology, affine/finite/singular and device-limit guards report a CPU
fallback. The UI adds one checkbox near the top of Scene Controls; the CPU path
remains the default. CLI: `--gpu-transforms`. See `docs/gpu-hierarchy-transforms.md`.

Final renderer `1547a44a954d4af8d58fd3a34607a3231d8882b2eb9aad6a7235472fe327c928`
passed **200 cases across two executables**: 196 rendering conventions and four
real Vulkan numerical/metadata/capacity/retirement tests. GPU readback agrees
with an independent CPU matrix reference for broad, unordered, deep, mirrored,
nested and reparented forests. Complete object records retain metadata,
conservative spheres, previous models and temporal identities. Additional
checks cover image independence, sparse upload counts, rejected inputs,
scene replacement and prepared-but-unsubmitted poses.

The same binary passed **334 fresh captures / 39 synchronization-validated
process logs**. Three 108-capture cohorts cover ordinary raster, hard ray
shadows with asynchronous DDGI, and continuous animation with TAA. Each runs
both raster techniques with static/skinned/morphed EXT_mesh_gpu_instancing,
live CPU/GPU switching, animation seeks, resizing and scene replacement;
the non-TAA cohorts also change to MSAA4. All **162 CPU/GPU image pairs match
pixel for pixel**. The ray cohort requires active ray/DDGI support, positive
RayShadowTrace/DdgiTrace timestamps and published asynchronous GI updates.
DDGI uses zero intensity and proves execution/synchronization, not GI quality.

Ten further captures edit an isolated hierarchy shader and reload it in both
raster techniques, then disable and re-enable GPU propagation. CPU restoration
recovers the original image; GPU reactivation recovers the edited image.
Settled C-handle object inventories and VMA allocation counts/bytes agree
before/after the switches. The packaged-build reload diagnostic also passes.
All accepted process logs have no Vulkan or synchronization errors.

The 50,000-node, seven-level numerical benchmark changes 516 local records
per iteration. Six iterations average **1.1617 ms** for the independent CPU
matrix reference, **1.8339 ms** for hierarchy preparation, **41,280 bytes** of
hierarchy input uploads and **0.6526 ms** on the GPU. These are component costs;
the CPU mirror and ordinary object SSBO uploads still run. This milestone
does not demonstrate an end-to-end acceleration and does not change defaults.

Evidence: `out/feature-parity/hierarchy-acceptance/results.json`, its preserved
executable/component log, `hierarchy-final-raster/results.json`,
`hierarchy-final-ray/results.json`, `hierarchy-final-taa/results.json`, and
the three logs/ten captures under `hierarchy-final-reload`. The Release build
enables physics and disables OpenVDB. Earlier diagnostic cohorts are excluded.
The first numerical run exposed eager ternary evaluation of an invalid root
parent address; an explicit branch fixed it before all accepted evidence.

**#119 remains open.** Direct GPU ray/TLAS instance publication and removal
of redundant CPU/object-upload work still need a coordinated generation path
and end-to-end benchmarks. Changes remain local and uncommitted.

## GPU ray-instance and surface publication (#119) — 2026-11-04

The optional hierarchy path now feeds current GPU object matrices into private
TLAS instance inputs. `RayInstanceTransformPass` writes the row-major Vulkan
3×4 records and matching column-major closest-hit model/normal/handedness data
before AS construction. Masks, custom IDs, facing flags and BLAS addresses come
from the existing CPU metadata. Primary draw/instance indices select the actual
object SSBO rows; CPU-only providers in a mixed scene retain fallback matrices.
The outputs are device-local and immutable with their generation. Source SSBOs
are needed through graphics publication only; asynchronous GI reads private
outputs after its existing graphics-timeline wait.

Packed inputs use 24 bytes per instance plus 48 bytes for each CPU-only fallback.
GPU-bound records skip the CPU 64-byte AS-instance upload and optional 112-byte
closest-hit transform upload. The numerical mixed-provider fixture uploads
120 bytes for three instances, compared with 528 bytes for those two CPU
transform tables. This excludes the retained ordinary object SSBO and other
metadata uploads; it is not an end-to-end bandwidth or performance claim.

Final renderer `4ca8ccc7deab97e13941ece974dc0615902967ee9c26e5ee7978ce93a54f55bf`
passes **290 cases / eight executables / six Vulkan executables**, with no
failures or skipped cases. This includes independent GPU ray queries using
deliberately different CPU/GPU matrices, masks/custom IDs, normal/handedness
readback, a CPU-only fallback instance, and retained old TLAS poses after a
new generation publishes. The extraction cache also preserves geometry while
GPU bindings switch, and maps actual draw/instance order across providers.
Broader surface-manager, DDGI transport/material/manager and convention tests
pass. One older surface fixture needed its required runtime-descriptor-array
feature enabled, matching production; the earlier failing run is excluded.

The same binary passes **466 fresh captures / 55 strict-clean
synchronization-validated process logs**. Four 108-capture cohorts exercise
ordinary raster, hard ray shadows with asynchronous zero-intensity GI,
continuous animation/TAA, and visible synchronous GI. Both techniques use
animated static/skinned/morphed instancing through CPU/GPU switches, resize,
MSAA and scene replacement. The first **162 image pairs are pixel-identical**;
the **54 visible-GI pairs differ by at most one channel value**, with worst
whole-image MAE `0.0000101273/255`. These are bounded raster/GI fixtures, not a
general transport-quality assessment.

Eighteen additional captures exercise live hierarchy and TLAS compute edits,
CPU restoration/GPU reactivation and the packaged-build diagnostic. Moving
TLAS inputs alone changes the receiver's directional-visibility sample
**0 → 232 → 0 → 232** in each raster technique. Settled C-handle inventories
and VMA allocation counts/bytes remain equal after reactivation. An early
diagnostic occluder faced the wrong visibility direction; the accepted fixture
uses an explicitly two-sided thin sheet beside the camera's receiver sample.

Sixteen light-only captures animate authored intensity **2 ↔ 80** and lamp
position. Ray generation **1** remains unchanged in both techniques, with
visible direct-light differences and exact restoration. Intensity changes
dispatch no hierarchy levels; changing the light node's position updates its
graph transform without rebuilding ray geometry/instances. Asynchronous GI
continues to publish while this occurs (zero intensity for this witness).

Evidence: `out/feature-parity/gpu-instance-acceptance/results.json`, its
preserved executable/component log, the four `gpu-instances-final-*` reports,
`gpu-instances-reload-verified`, and `gpu-instances-light-motion-verified`.
The Release build enables physics and disables OpenVDB. All accepted evidence
uses the exact final binary above; earlier diagnostic cohorts are excluded.

**#119 remains open** for redundant CPU/object-upload reduction and the
complete-path large-scene performance assessment. The CPU mirror and ordinary
object uploads still run, and CPU remains the default. Changes are local and
uncommitted; the full tracker goal remains active.

## Complete GPU object publication and large-scene assessment (#119) — 2026-11-04

The optional GPU hierarchy path now publishes entire 224-byte render-object
records. CPU uploads contain changed 112-byte metadata tails (IDs, world bounds,
submitted previous models and temporal tokens); diagnostic objects without a
graph binding also carry their CPU model/normal prefix. Per-image comparison
skips unchanged inputs. The primary controller/frontend suppress full CPU
object-buffer writes while GPU publication is prepared. TAA prepares history
before metadata publication. Setup or metadata failure restores complete CPU
records for the acquired image; successful GPU recovery resumes compact inputs.
The existing checkbox remains the only UI control and CPU stays the default.

Final renderer `57238e46e9f0e1d36879c595ba9b2c7b18895adcdfef24ead4656cd524bf28df`
passes **291 cases / eight executables / six Vulkan executables**, without
failures, disabled or skipped cases. Poisoned output buffers prove complete
GPU publication, including nonsymmetric previous/diagnostic matrices and exact
integer metadata. Sparse metadata tests verify initial, unchanged and modified
uploads. The 50,000-node component benchmark averages CPU reference **1.1562 ms**,
hierarchy preparation **1.6515 ms**, local/topology/binding input **41,280 bytes**,
metadata **5,600,000 bytes / 0.7461 ms**, and GPU **0.5999 ms** over six iterations.
This is an independent matrix reference and component measurement, not total
renderer cost. Metadata uses mapped VMA `AUTO` placement, allowing host-visible
device memory where available.

The same binary passes **498 captures / 63 strict-clean synchronization-validated
native process logs**. Four 108-capture cohorts cover raster, hard ray shadows
with asynchronous zero-intensity GI, continuous animation/TAA, and visible
synchronous GI. Both raster techniques exercise static/skinned/morphed
instancing, CPU/GPU switches, animation, resize and scene replacement; the
non-TAA cases also change MSAA. All **162 raster/ray/TAA pairs are pixel-identical**.
The **54 visible-GI pairs differ by at most one channel value**, with worst
whole-image MAE `0.0000101273/255`.

Twenty-six shader/package captures include live hierarchy/TLAS edits, CPU
restoration, GPU reactivation and a new missing-compute-shader recovery test.
The latter moves a real object during CPU fallback and restores the staged
shader before GPU recovery; fallback writes 224 bytes and recovered steady
frames write zero full CPU object bytes and zero unchanged metadata bytes.
Both raster techniques pass. Sixteen light-only captures retain ray generation
1 through intensity and position animation, with exact visible restoration.

The remaining 24 validated captures render a continuously animated
**20,000-instance** glTF grid, with hard ray shadows, on both techniques and
both CPU/GPU paths. All 12 image pairs are exact. Each warmed GPU frame copies
**2,240,000 metadata bytes + 80 local bytes**, with **zero full CPU object-buffer
bytes**, compared with the CPU path's **4,480,000 object bytes**. GPU TLAS inputs
cover all 20,000 instances and reuse one shared BLAS without rebuilding geometry.
These counters cover the measured primary writers, not physical PCIe traffic,
initial scene staging, sidecar providers or every renderer upload.

Another **48 captures / eight native processes** measure raster-only and
hard-ray paths with validation disabled. Each CPU/frame-graph median uses
35 warmed, uncaptured frames; hierarchy/metadata medians use six samples.
All 24 additional CPU/GPU image pairs are exact. Release measurements on an
RTX 2080 SUPER, in milliseconds:

| Path | Technique | CPU-path frame | GPU-path frame | Hierarchy CPU preparation | Metadata CPU preparation | Hierarchy GPU |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Raster | Deferred | 32.997 | 32.725 | 0.738 | 0.743 | 2.463 |
| Raster | Forward | 34.366 | 31.099 | 0.721 | 0.371 | 1.331 |
| Hard ray shadows | Deferred | 49.186 | 45.133 | 0.724 | 0.372 | 1.268 |
| Hard ray shadows | Forward | 42.460 | 44.626 | 0.706 | 0.365 | 0.604 |

The reports also retain scene-update, command-record and frame-graph GPU costs.
Capture mode serializes resource preparation; these figures are not unrestricted
interactive frame rates. Graph GPU timing excludes the separately timed hierarchy
pass and does not represent total GPU latency. This single measurement batch
shows mixed results and establishes no general speedup. CPU graph/ECS mirrors,
bounds, picking, physics and ray extraction/validation remain for their consumers.

Evidence: `out/feature-parity/gpu-metadata-acceptance/results.json`, the preserved
executable/build/component logs and eight XML reports, four `gpu-metadata-final-*`
reports, `gpu-metadata-reload-verified`, `gpu-metadata-light-verified`, and three
`gpu-metadata-large-*` reports. The checkpoint records 34 selected sources and
103 compiled shaders. All accepted evidence uses the exact final binary above.
Earlier diagnostic binaries/cohorts are excluded. The initial recovery verifier
used the wrong translation-key spelling, then sampled outside the moving object;
the corrected schema and interior sample pass in fresh outputs. Release validation
enables physics and disables OpenVDB; unrelated OpenVDB work remains unvalidated.

**#119 is implemented and validated locally; it remains open pending integration.**
All original scope/acceptance items are satisfied with the measured limitations
above. Changes remain local/uncommitted; the full tracker goal remains active.

## Reference path-tracing transport and camera core (#47) — 2026-11-04

Started the reference-rendering dependency for #116/#117. Added an engine-owned
Slang include for diffuse/isotropic GGX reflection, visible-normal sampling,
complete mixture PDFs, power-heuristic MIS, deterministic counter-addressed
random values, compensated Russian roulette and unclamped HDR Welford moments.
Zero roughness uses a true discrete mirror event; ordinary GGX null outcomes
are distinct from invalid/unrepresentable numerical rejections. Separate
inverse projection/view matrices produce perspective/orthographic rays with
reverse-Z, negative-height viewport mapping, roll, near/far intervals and stable
directions in large world coordinates. No UI or dependency was added.

The actual Slang functions pass **20 real Vulkan compute/readback cases** under
synchronization validation. Independent double-precision BRDF/PDF/throughput
references, reciprocity, delta/continuous probability measures, camera matrices,
HDR variance and invalid-input guards pass. The 131,072-sample rough-white-metal
furnace gives **0.306951882**, against analytic **0.306852819**. Environment MIS
passes analytic Lambert/metal and numerical mixed-BSDF checks. Across 16 seeds,
Lambert estimator RMS drops from **0.018990539 at 64 samples** to **0.001594961
at 16,384 samples**. Batch-size/reordering preserves random values exactly.

All existing engine shaders rebuild successfully. The coordinated acceptance
passes **236 cases / four executables / three Vulkan executables**, with no
failures, disabled or skipped cases; existing DDGI transport/material and
rendering conventions remain passing. The renderer and all 103 engine shader
hashes are unchanged from #119's accepted binary. Earlier diagnostic runs are
excluded: the first GPU run exceeded a half-ppm unit-direction assertion by
0.004 ppm; a one-ppm float/trigonometric tolerance passes in fresh outputs.

Evidence: `docs/reference-path-tracing.md` and
`out/feature-parity/path-transport-acceptance/results.json`, with preserved
component executables/SPIR-V, build/component logs, XML and selected sources.
Release validation enables physics and disables OpenVDB.

**#47 remains in progress.** This checkpoint validates numerical building
blocks, not scene traversal or reference images. The path-tracing technique
remains unavailable until shared scene/material/light integration, actual
transport, accumulation lifetime/reset, graph/presentation/capture integration
and the full scene-quality/performance acceptance are implemented. Original
issue scope/checkboxes are unchanged. Changes remain local/uncommitted; the
full tracker goal remains active.

## Shared-scene reference path-tracing backend (#47) — 2026-11-04

`ReferencePathTracePass` now runs actual inline ray queries against the shared
immutable scene generations and bindless material/texture/light descriptors.
It reconstructs closest-hit surfaces, applies alpha coverage and sidedness,
and traces diffuse/GGX paths with emission, environment, directional/point/spot
and rectangle/disk lighting. Analytic emitter intersections and matching PDFs
support both camera/BSDF hits and next-event MIS. The last bounce retains full
next-event weight. Raster bounce terms, LTC, DDGI, TAA, denoising and radiance
clamping do not enter raw transport.

Three typed RGBA32F images retain accepted means/counts, Welford M2/flags and
separate display output. Unsupported/numerically rejected paths do not pollute
raw moments; diagnostic display colors identify them. Preparation uses retired
per-image input slots, and successful queue submission alone commits sample
addresses/layout state. Camera/clipping, immutable geometry bindings,
material/light/environment revisions and transport settings reset incompatible
history. Batch-size changes and larger sample stops preserve exact sequences;
lower stops restart. Resource budgets/storage ranges reject before use, and
discarded recordings cannot advance history.

The production pass passes **20 real Vulkan scene/readback cases**. Independent
targets verify Lambert energy **0.4999761 vs 0.5**, point falloff **0.3170859 vs
0.3170733** at distance two and **0.0795007 vs 0.0794999** at distance four,
rectangle MIS **0.2384790 vs 0.2394565**, disk MIS **0.1992372 vs 0.2**, and
emissive-mesh indirect transport **0.2404175 vs 0.2394565**. Additional fixtures
exercise actual shadow blockers and exact restoration, spots/range, visible HDR
emitters, textures, vertex/material alpha, two-sided normals, buffer bounds,
diagnostics, image slots, exact batching/sample stops and allocation retirement.

A Cornell-style room with colored walls, diffuse and rough-metal boxes and a
ceiling emitter produces finite HDR means/moments with **512 accepted samples
at every pixel**, zero diagnostic flags and eight bounce levels at **128×128**.
With batches of 16 samples, the warmed GPU median is **9.81600 ms per batch**;
owned VMA allocations total **787,232 bytes**. These validation-enabled backend
costs exclude AS builds, CPU extraction and presentation; they are not full
application frame times. Raw float images remain separate from the exposure-one
Reinhard/sRGB preview.

Renderer `4e0b7929050794aaa35a93211c66266961f08582afc3c4ce3cc0dd7823a5c752`
builds with 104 shader assets. The final coordinated run passes **331 cases /
ten executables / eight Vulkan executables**, zero failures/disabled/skips.
The 103 older engine shader hashes remain unchanged. **72 fresh native captures /
eight strict-clean process logs** additionally exercise both raster techniques,
animated instancing, GPU hierarchy switches, MSAA, resize, scene replacement and
hard-ray/asynchronous-GI execution. All **36 CPU/GPU pairs are pixel-identical**.
Those application captures exercise existing renderers, not the new reference
viewport, which is not yet connected.

Evidence: `docs/reference-path-tracing.md`,
`out/feature-parity/reference-path-acceptance/results.json`, preserved executables,
SPIR-V, XML/source snapshots/build/component logs, `reference-path-verified-artifacts`,
and the two `reference-path-*-regression` reports. Physics is enabled; OpenVDB is
disabled. Earlier diagnostic builds are excluded from final acceptance.

**#47 remains in progress.** The backend now renders real reference scenes;
application technique selection, graph/presentation/capture wiring, viewport
coordinates, live authoring history keys, remaining material transport and the
full scene-quality/performance matrix remain. Unsupported transmission/layers
and volume/medium propagation are explicit limitations, not a reduction of the
full objective. No UI controls or dependencies were added. Changes remain local
and uncommitted; the full tracker goal remains active.

## Application reference viewport and numerical boundary fixes (#47) — 2026-11-05

Connected the progressive Vulkan backend to the existing Rendering Engine selector
and shared depth/picking/HDR presentation graph. Runtime bindings publish its owned
RGBA32F display to exposure and bloom; EXR export reads the raw float32 mean directly.
Forward/deferred raster remain available. Ray-query and resource failures choose a
complete raster fallback. The UI adds only a sample/diagnostic status line.

Reference mode uses the active navigation/authored camera without TAA jitter and
requests shared surface generations independently of DDGI. History keys include
scene, material/texture, lighting/environment and pipeline changes; display exposure
does not discard samples. Counts advance only after successful submission. Empty
scenes use a background shader without statically accessed AS descriptors. Unsupported
surfaces, semantic BIM, volumes and atmosphere/solar transport are explicit diagnostics.
The raw environment cube is retained, and prior graphics readers retire before replacement.
Readiness policy prevents shared UI enable flags from reactivating raster transport,
DDGI, temporal filtering or OIT in the reference graph.

Native validation exposed and reproduced two numerical boundary bugs. Summed
barycentric products could round constant white above one; shared affine color/alpha
interpolation now preserves constants and bounds rounding to the vertex-value hull.
Float32 pixel-plus-jitter addition could round past the final image row/column;
subpixel coordinates now stay within their owning pixel. Radiance remains unclamped.
A 1280×800 GPU fixture retains all 13 samples at every pixel and exactly matches an
independent constant-alpha-mask representation at the cutoff.

Renderer `a78f4ed4e93eebbec0fe99ee951d0fd34668a01137784d595d6aca1354247331`
builds with **105 shader assets** and passes **574 cases / 15 executables / eight
Vulkan executables**, zero failures/disabled/skips. The reference backend contributes
24 scene cases alongside 20 transport-math cases. Repeated Cornell component cost is
**9.85834 ms per 16-sample batch**, at 128×128 / 512 samples / eight bounce levels,
with **787,232 owned VMA bytes**; this excludes AS, CPU and presentation costs.

**36 application reference captures / 16 clean process logs** validate camera/material
changes, material/light/gizmo undo/redo, renderer switches, resize, GPU hierarchy,
full UI, exact stops, empty/HDR environment scenes, bloom, diagnostics and capability
fallback. Raw EXR retains **200,000 / 100,000 / 50,000** radiance without display
exposure/tone-map/bloom contamination. Point-light energy matches 1/π at distance two,
doubles with intensity and falls to one quarter at distance four; replayed images are
exact. A further **72 raster/ray regression captures / eight clean logs** pass, with
all **36 CPU/GPU pairs pixel-identical**. Total: **108 captures / 24 logs**, same binary.

Evidence: `docs/reference-path-tracing.md`,
`out/feature-parity/reference-viewport-acceptance/results.json`, preserved executables,
SPIR-V/XML/source snapshots, logs, raw images and PNG/EXR captures. Physics is enabled;
OpenVDB is disabled. **#47 remains in progress** for remaining material/transmission,
volume/atmosphere, filtering/normal-energy, convergence/comparison and broad scene
acceptance. No external dependencies were added; changes remain local/uncommitted.
The full tracker objective remains active.

## Camera validation and raw EXR annotations (#47) — 2026-11-05

The navigation camera passes 900 yaw/pitch poses. Independent emissive-quadrant
application fixtures validate rotated/rolled, perspective finite/infinite,
orthographic and clipped cameras. Raw raster EXR now omits selection strokes and
editor inspection/debug annotations while retaining native scene point/curve
geometry. Display outlines remain five pixels across all 14 regression captures,
including MSAA 4×.

Renderer `35336c56c5530c91320e8e4665428c9eddb64a47e7c7c73adb5559bfe00bde4d`
with the pre-transparency shader set passes **591 cases / 16 executables / eight
Vulkan executables**, plus **156 native captures / 72 process logs** across six
cohorts. All 36 CPU/GPU raster/ray comparison pairs are exact. Frozen evidence:
`out/feature-parity/reference-camera-checkpoint/results.json`. This checkpoint
precedes the shader-only transparency fix below.

## Path-traced alpha transparency in the user's IFCX scene (#47) — 2026-11-05

Reproduced the reported magenta paths using the original
`ACCA Building/esempio_01 edificius.ifcx` at its supplied absolute Debug model path.
Alpha-blend opacity was previously diagnosed as unsupported. The Vulkan inline
ray-query shader now samples coverage for primary, secondary and shadow rays,
including vertex alpha, opacity factors and texture channels. Endpoints are exact;
alpha masks retain their cutoff and support double-sided shading. Decisions use
stable sample/candidate addresses, preserving deterministic batching and M2.
Emissive layers use the same coverage. Optical refraction, absorption and the
other material-layer gaps remain in scope and explicitly diagnosed.

At 640×360 / 128 samples / eight bounce levels, this model changes from **35,559
unsupported samples to zero**, with **zero rejected samples**. A real shader reload
increments the generation, restarts history and reproduces the same 64-sample image.
The source IFCX is referenced by path/hash without copying it into the evidence.

Six new Vulkan cases raise the backend to 30 scene cases alongside 21 transport-math
cases. The coordinated run passes **597 cases / 16 executables / eight Vulkan
executables**, with zero failures/disabled/skips. **39 native captures / 18 logs**
validate the updated viewport suite, ACCA scene and reload. The executable SHA stays
`35336c56c5530c91320e8e4665428c9eddb64a47e7c7c73adb5559bfe00bde4d`;
the updated shader hashes and all evidence are recorded separately in
`out/feature-parity/reference-alpha-acceptance/results.json`.

Use **Reload Shaders (F5)** in an existing editor, or restart this build, then select
**Rendering Engine → Path tracing**. No UI complexity or dependency was added.
No new comparative GPU performance claim is made during concurrent interactive
use. Physics is enabled and OpenVDB is disabled for these runs. **#47 and #121 stay
open**; remaining transmission/medium, filtering/normal-energy, convergence and
broad scene acceptance work is unchanged. Changes remain local/uncommitted.

## Thin-walled optical transmission (#47) — 2026-11-05

Added a sampled thin-sheet GGX transmission lobe alongside the existing
reflection/diffuse mixture. Base/vertex/texture tint, transmission factor/channel,
roughness, IOR roughness scaling, material sidedness and alpha coverage remain
distinct inputs. Fully metallic surfaces ignore inactive transmission. Zero
transmission reproduces the original opaque sampling exactly.

Smooth panes transmit straight through and do not consume scattering bounces or
invoke roulette. Light connections integrate tint through multiple smooth panes;
MIS includes the complete null-choice probability chain and retains the last
real scattering point. Rough panes use actual scattered paths. The supported
64-crossing bound produces an explicit unsupported diagnostic if exceeded.
No UI, descriptor ABI, allocation contract or dependency was added.

Six additional Vulkan math cases and ten production scene cases raise acceptance
to **613 cases / 16 executables / eight Vulkan executables**, zero failures/disabled/
skips; backend 40 scene cases and 27 transport-math cases. Independent checks:
rough-sheet furnace **0.306856132 vs 0.306852819**, opposite-side point energy
**0.079320996 vs 0.079319480**, two-pane area MIS **0.134172648 vs 0.134318610**.
**46 native captures / 22 logs** validate real loading, raw EXR, reload and
transmission-edit undo/redo. All 35 prior scene-only viewport images are exact;
the full-UI capture is checked separately. The original ACCA IFCX is also
pixel-identical to its prior fixed 128-sample image, with zero unsupported or
rejected samples. Its source is referenced by path/hash without copying it.

Executable SHA remains
`35336c56c5530c91320e8e4665428c9eddb64a47e7c7c73adb5559bfe00bde4d`;
only two reference shader assets change, with the other 103 unchanged. Evidence:
`out/feature-parity/reference-thin-acceptance-v2/results.json` and
`docs/reference-path-tracing.md`, preserved binaries, SPIR-V/XML/source snapshots,
logs and raw/native images. Failed diagnostic attempts are excluded. No comparative
GPU timing claim during concurrent user-editor execution. Physics enabled,
OpenVDB disabled. **#47 remains open** for solid-volume refraction/absorption,
dispersion and the other material/medium/quality/comparison gaps. Changes remain
local/uncommitted; the full tracker objective remains active.

## Closed solid-glass refraction and absorption (#47) — 2026-11-05

The Vulkan reference renderer now traces smooth/rough dielectric refraction,
total internal reflection, nested relative IORs and homogeneous absorption over
actual world distances. Primary containment inference supports cameras inside
media before opaque hits. Both volume faces are retained regardless of sidedness;
baked thickness textures do not replace measured geometry. Null index-matched
crossings preserve the existing scattering budget and MIS chain. Roulette is
compensated for refractive radiance scaling. Material edits, reload and their
history resets remain integrated without new UI.

The bounds are eight concurrent media, 128 initial boundary-probe hits and 64 null
crossings. Open/coverage-holed volumes, incompatible overlaps, inspection clipping,
excess bounds and bending delta-light caustic connections are explicitly diagnosed.
These and dispersion/participating scattering remain within the original scope.

**646 cases / 17 executables / nine Vulkan executables** pass with zero failures,
disabled or skipped cases: 56 scene cases plus 41 transport-math cases and three managed-pipeline lifecycle cases. Independent
math/geometry fixtures validate Snell/TIR, refractive PDFs/weights/reciprocity, nested
IOR cancellation, mixed rough surfaces, black/infinite absorption and both camera
and shadow segment distances. Camera-plus-light absorption measures
**(0.083690018, 0.327844026, 0.626906812)** against independent
**(0.083680815, 0.327812551, 0.626850928)**.

**160 native captures / 68 logs** pass, including 24 imported solid-volume captures,
material attenuation undo/redo, reload, mirrored/nonuniform transforms, cameras,
old thin/viewport cases and raster/ray regressions. All 35 previous scene-only
viewport images, all 36 CPU/GPU pairs and the original fixed ACCA image remain
pixel-identical. ACCA retains all 128 samples with zero diagnostics and is referenced
by path/hash without copying its source. At 64 samples, deep-interface application
results are validated in expectation under roulette; component tests separately
verify exact IOR cancellation with roulette disabled.

Renderer SHA:
`2a51954b23744615fac8fbfe481e4ffab83d76229f86c96a271c8f10db256b47`.
Only two reference shaders change; the other 103 engine assets are unchanged.
Evidence: `out/feature-parity/reference-volume-mixture-acceptance/results.json`,
`docs/reference-path-tracing.md`, binaries, SPIR-V/XML, selected source snapshots,
logs and raw/native images. Restart an older running editor to load the new volume
frame flag. No additional UI/dependency or image allocation contract. No comparative
GPU timing claim during concurrent editor/build activity. Physics enabled, OpenVDB
disabled. #47 and #121 remain open; incremental publication is continuing.

A follow-up material audit corrected the conductor/dielectric mixture: TIR no
longer whitens metallic reflectance, transmitted light is weighted once, and
coincident reflection deltas merge their full probability masses. Five additional
math cases and two production scenes cover the correction. The earlier 636-case
archive is superseded for material-mixture acceptance and is preserved only as
scoped historical evidence.


## Shared ray-opacity Git publication — 2026-11-05

Pushed `fde8af21561997c409d23a753a7cd87d2947b107` on `codex/ui-scene-virtualization`: glTF vertex opacity, 64-byte ray triangle metadata, masked shadow/probe-GI coverage, stable closest-hit color interpolation, loader and GPU contracts. Fifteen selected files were published; unrelated editor, physics and OpenVDB work remains local. The raster alpha stream and full reference viewport/solid-volume dependency closure are later publication milestones.

A clean source snapshot matches all 982 committed source files, builds the existing application, passes 105 cases in seven selected components (five Vulkan executables), and validates all 92 SPIR-V modules. The broader 194-case source-convention suite has the same eight failures as the unchanged parent; these are not counted as passing. Evidence: `out/feature-parity/ray-alpha-publication-acceptance/results.json` (137 verified artifacts, full committed source export, binaries/SPIR-V/XML/logs and baseline comparison). The five new loader cases also pass in the full local build: `out/feature-parity/ray-alpha-local-loader-acceptance/results.json`. This does not replace or expand the historical 646-case/160-capture volume checkpoint. #47/#121 remain open and their bodies now record both publication and local implementation scope.


## Raster opacity Git publication — 2026-11-05

Pushed `cee2bfa64ed005f30ee49e373df169b9ebd9a97c` on `codex/ui-scene-virtualization`: 19 selected files preserve vertex alpha through raster coverage and the separate TAA velocity layout, keep optical transmission independent of alpha mode, and prevent textures from enabling zero-factor layers. Three additional GPU contracts bring the math/material-input target to 44 cases. The full local build also passes the 44 math/material-input and five loader cases.

The clean publication snapshot matches all 982 committed source files, builds the application, passes 108 cases in seven selected executables (five Vulkan), and validates 92 SPIR-V modules plus six ordinary-raster capability boundaries. Eight failures in the broader 194-case convention suite exactly match the parent and remain recorded. Eighty native PNG captures/process logs across 46 oracle checks pass: static MASK/BLEND/OPAQUE coverage, opaque glass, zero/absent transmission, directional raster/ray shadows, MSAA 4x and populated TAA history. Transparent-picking/local-shadow layouts are build validated; native picking/animation is outside this checkpoint.

Evidence: `out/feature-parity/raster-alpha-publication-acceptance/results.json`, 431 verified artifacts including full committed source export, binaries/SPIR-V/XML/logs, fixtures/native images and frozen harnesses. Renderer SHA-256 `54be641d135725709458f9baeea7ed6b0f2a0489898a9332e05551883cbc7cf2`; failed exploratory runs are excluded. No original IFC model copied. #47 and #121 record the publication. Full reference viewport/solid-volume integration still needs coherent shared material/lighting data and frontend publication; the original tracker scope remains active.


## Vulkan path-trace producer Git publication - 2026-11-05

Pushed `33ed60dd1a37ddd999992850445e399804e9ca1d` on `codex/ui-scene-virtualization`: 30 selected files publish the complete currently developed progressive Vulkan GPU producer, paired material/lighting layouts, mirrored ray-facing correction and robust shader-reload history identity. The editor viewport integration and its remaining owner dependencies are still local. The published pipeline manager retains the legacy compute API until existing owners migrate together.

The clean publication snapshot matches all 989 committed source files, builds the application, passes 200 cases in nine selected components (six Vulkan executables), and validates all 95 SPIR-V modules. Eighty native raster captures and process logs pass 46 oracle checks and match the preceding published layout checkpoint pixel for pixel. The broader source-convention suite retains the same eight parent failures, excluded from the passing count.

The full local editor build separately passes 656 cases in 18 components (nine Vulkan executables). Thirty-four new-renderer captures in 26 process logs validate volume/refraction, thin-surface, reload/history and original ACCA integration; one additional accepted-baseline ACCA capture matches exactly. The original IFCX was referenced at the user-provided path and hash, without copying it. ACCA reports zero unsupported and zero rejected paths; all 105 local engine SPIR-V modules match the preceding corrected-volume checkpoint.

Frozen evidence: `out/feature-parity/reference-backend-publication-acceptance/results.json`, 863 verified artifacts including the full committed source export, selected source snapshots, application/test binaries, SPIR-V, XML, native images/logs and frozen harnesses. This is evidence, not a standalone Windows runtime package. Published-profile renderer SHA-256 `afdb3a48737a35b320f6bbe9ffd363bd46a5aac4b4991a96d6d9fd3d9ac1cece`; local-editor renderer SHA-256 `6ad3a613d6435acc041ced013c669b20920661471641a3356b075d7349ab7256`.

#47 and #121 were updated and verified open, preserving their full original scope. Remaining work includes coherent frontend publication, emissive-mesh next-event sampling, remaining material/medium families, filtering/shading-normal energy and convergence/comparison acceptance. Remote head and empty index were verified. No comparative GPU timing claims during concurrent editor/build activity.


## Managed compute-owner Git publication - 2026-11-05

Pushed `57612ff784360f7c897004a1426b1b466f66dcc7` on `codex/ui-scene-virtualization`: 21 selected files migrate all 20 existing PipelineManager compute slots across eight owners. The raw compute-creation overload is removed. Explicit environment destruction now retires both GTAO slots and their layouts/descriptors. The migration preserves actual shader instructions, resource layouts and developed feature behavior; frontend and reload-controller publication remains pending.

Three new production-owner Vulkan cases verify bloom retirement/recreation, automatic exposure histogram/adaptation before and after shader publication, and all six environment/IBL/GTAO registrations. The clean source snapshot matches all 990 committed files and passes 203 cases in ten executables (seven Vulkan). All 95 SPIR-V modules match the prior producer. The broader convention suite retains exactly its eight parent failures, excluded from the passing count. Ninety native captures/process logs include 86 new-renderer runs and four prior-renderer baselines: 80 compatibility images match exactly, active bloom matches in both raster modes with a measurable on/off effect, and the original ACCA IFCX matches exactly in both raster modes with bloom enabled.

The full local editor separately passes 659 cases in 19 executables (ten Vulkan), following a serial accepted build. All 105 engine SPIR-V modules are unchanged. One original ACCA replay at 128 samples matches the frozen prior editor image exactly, with zero unsupported and rejected samples. The original IFCX was referenced in place, without copying its bytes.

Frozen evidence: `out/feature-parity/managed-owners-publication-acceptance/results.json`, 710 verified artifacts including full committed source export, selected source, binaries/SPIR-V/XML/logs, native captures and harnesses. The archive is evidence, not a standalone runtime package; local integration contains selected provenance rather than a full local source export. Published renderer SHA-256 `9eb6edbef80f65863ffe1609d16bb419ecdaef7157dfebd86194d4b3c495b533`; local editor `dff1b1b723561bba62e784625024ca87531dcf76047777bf9008fe0ccf697e4b`. Exploratory native/build runs are excluded: a forward-renderer pass-name assertion was corrected, and 33 intermediate objects from an interrupted overlapping local build were discarded before the final serial build and component run.

#47, #113 and #121 now record the publication and remain open with their full original scopes intact. The viewport, environment/sampler/deformation dependencies and reload controller still need coherent publication; the remaining path-transport/material, emissive-mesh sampling, filtering, convergence/comparison and broad scene acceptance gaps remain active. Remote head and empty index are verified. No comparative GPU timing claims.


## Environment and typed-decoder Git publication - 2026-11-05

Pushed `af57aabd9a25a1bc418bd6f4e2814566c331ca78` on `codex/ui-scene-virtualization`: 36 selected files publish complete developed HDR/sky generation, typed CPU texture decoding, guarded staged commands and codec notice packaging. The environment owns retained unfiltered radiance, irradiance/specular maps, asynchronous CPU decode/GPU publication, procedural sky/solar metadata and multiple-scattering LUTs, and detached shader rebaking from retained source data. Radiance revisions survive Vulkan handle reuse; BRDF-only rebakes preserve the radiance revision, while publication revisions reject stale bake candidates. The local reference frontend includes the radiance revision in history identity.

The clean export matches all 1009 committed files and passes 234 cases in 11 selected executables (seven Vulkan), including 26 typed-decoder cases and eight real-owner cases. The codec-disabled profile passes 23 cases with two expected optional KTX skips. All 96 SPIR-V modules validate: only the equirectangular generator changes from the parent, plus the new sampling probe. The same eight existing broader convention failures remain separately recorded, excluded from the passing count.

Ninety publication-profile native captures/logs include 86 new-renderer runs and four prior-executable baselines. Eighty compatibility images match the preceding checkpoint exactly; active bloom and original ACCA raster comparisons also match in both techniques. The full local editor separately passes 690 cases in 20 executables (ten Vulkan). Eighteen sky captures/ten logs and 32 live HDR captures/two logs verify replacement/rejection, resize, presentation and temporal/DDGI history. One separate ACCA path-traced replay at 128 samples matches frozen accepted pixels with zero unsupported/rejected samples; all 105 local engine shaders are unchanged. Original IFCX bytes are read in place and excluded.

The package smoke test retains all 25 codec notice files, records enabled backends, rejects a missing notice before staging, and passes two packaged plus two source-runtime comparisons pixel-exactly on only Windows system paths. Pinned KTX/WebP build/notice source files match SHA-256 archives (755 and 258 files). Its committed source export was verified clean through an isolated Git index; root editor WIP and the root index were preserved. This is a smoke-test package, not an official release; runtime metadata retains the tested precommit build identity.

Frozen evidence: `out/feature-parity/environment-publication-acceptance/results.json`, 966 verified artifacts including full committed source export, binaries/SPIR-V/XML/logs, native captures, disabled-codec evidence, upstream notices and the smoke-test ZIP. Public renderer SHA-256 `3ed92008806612eeb8bd4646c405faf57253eeaced038ed4186e7f414e0b90fd`; local editor `df89caddf1af5a1195743cea5843ae95a47e595381244d5d690d2003fc652fb8`. Local integration carries selected source provenance rather than a full local source export. `localIntegration.nativeCaptures` records the original-model replay; the two additional sky/HDR cohorts are in `additionalNativeResults`. Exploratory dependency-build/package attempts are excluded; accepted package creation, archive hashes, notice rejection and native execution are verified.

#47, #94, #111, #113 and #121 record the publication and stay open with their original scopes. Actual viewport, scene-lighting/presentation controls, reload controller and typed GPU upload/glTF texture authoring still need coherent publication. Atmosphere/solar reference transport, mesh-emitter sampling, remaining material/medium families, filtering, convergence/comparison and broad acceptance remain gaps. Remote head and empty index are verified; no comparative GPU timing claims.

## Display grading and image export Git publication - 2026-11-05

Pushed `87ec502fb334e94894b89a31220dfc9f74955786` on
`codex/ui-scene-virtualization`: 43 focused files publish seven tone operators,
white balance/tint/vibrance/split toning, validated atomic presets, scene-linear
RGB EXR, viewport/window PNG, explicit capture metadata and simple UI controls.
Windows Unicode arguments and capture filenames are preserved. Explicit exposure
initializes the editor. Interactive picking now transitions both packed depth and
stencil aspects, correcting the validation error observed during the UI check.

The clean export matches all 1022 committed source files. All 314 selected cases
in 16 executables pass, including seven Vulkan executables. All 96 SPIR-V modules
validate; only the post-process fragment module changes. The broader 194-case
convention suite retains precisely the parent's eight failures, excluded from
the passing count.

Publication acceptance contains 48 grading captures, 16 EXR/PNG captures, three
interactive exports and two parent-renderer baselines. All 344 analytic chart/ramp
comparisons pass, with neutral ACES preserving prior pixels. Coverage includes
transparent emission, raw radiance above one, display-independent EXR, Unicode
paths, resize with TAA and UI exclusion. Windows UI checks exercised picking,
three export buttons, live ACES-to-AgX switching, compact grading, preset save/load
and normal closure without Vulkan validation errors.

The full local editor separately passes 690 cases in 20 executables, ten Vulkan.
Two 128-sample EXRs preserve analytic RGB [4,2,1] across display changes and now
identify `path-trace-mean` correctly. The original ACCA IFCX replay retains 128
samples with zero unsupported/rejected samples and matches frozen accepted
pixels. Its source remains in place and is excluded from the evidence.

Packaging retains the permissive tone-curve license and 25 codec notices. Four
packaged and two source-runtime captures match exactly on a Windows-system-only
PATH, including raw EXR in both raster modes. This is a packaging smoke test,
not an official release; binaries retain their tested precommit build metadata.

Evidence: `out/feature-parity/display-capture-publication-acceptance/results.json`,
765 verified artifacts including committed source export, selected source,
binaries/SPIR-V/XML/logs, accepted captures, harnesses and package. Published
renderer SHA-256 `01be3a177e89175cde73bd7d31a4990a8e597e23a6dc5bddadc5d8a131f62d35`;
local editor `aacb9d104c298738926ca3a12158dd8354115a7aae59aa58e2e40bc75d2f0078`.
Exploratory failed export/UI runs are excluded. No comparative GPU timing claims.

#112 is now completed and closed: its original scope and acceptance criteria are
published and verified. #115 remains open for converged reference sample-series
coordination with #47. #47 and #121 retain their original remaining scopes,
including full viewport publication, remaining transport/material/media,
atmosphere/solar transport, emissive-mesh sampling, filtering, convergence and
broad scene acceptance. Other developed asset/editor/environment/reload features
still need coherent publication. Remote head and empty index are verified.

## Typed Vulkan texture upload and ownership Git publication - 2026-11-05

Pushed `b2299baac7337becfbf596d24d8b7db75fb0adee` on
`codex/ui-scene-virtualization`: 24 focused files publish typed GPU uploads,
authored mip preservation, enabled-device compression format checks, allocation
batches, image leases, incarnation checks, and stable transactional texture slots.
Invalid component mappings reject before allocation. Normalized/float material
samplers use compatible floating-point transparent black. The source export
matches all 1033 committed files; the index is empty after publication.

All 381 selected cases in 19 executables, eight Vulkan, pass without failures or
skips. The full local editor separately passes 757 cases in 23 executables,
eleven Vulkan. Independent shader sampling covers authored mips, component
mapping, signed HDR, sRGB decoding with linear alpha and authored BC3 coverage.
The disabled-codec profile passes 90 cases with two expected optional KTX skips.
All 97 public SPIR-V modules validate: 96 parent modules remain byte-identical,
and only the test probe is added. All 105 local engine modules remain unchanged.
The broader convention suite retains exactly the parent's eight known failures,
excluded from the passing counts.

All 80 compatibility captures match frozen prior pixels exactly. Six native
PNG16 captures compare both raster renderers against independent material-only
references and explicit eight-bit truncation controls. Two actual parent-binary
captures reproduce its precision loss, with maximum channel difference
0.001953125. The full local editor retains two display-independent 128-sample
EXRs with analytic RGB [4,2,1], plus the original ACCA IFCX replay at 128 samples
with zero unsupported/rejected samples and exact accepted pixels. The original
IFCX remains in place and is absent from the evidence archive.

Packaging retains 25 codec notices and the permissive tone-curve notice. Four
packaged and two source-runtime captures match exactly on a Windows-system-only
PATH, including external PNG16 through both raster renderers. Missing notices
reject before packaging. This is a packaging smoke test, not an official release;
binaries retain their tested precommit build metadata.

Frozen evidence: `out/feature-parity/texture-upload-publication-acceptance/results.json`,
981 verified artifacts, including the complete committed source export, selected
source, binaries/SPIR-V/XML/logs, native current/prior images, the actual parent
executable, disabled-codec results and package. Published renderer SHA-256
`2ca9c0d83aac0568bcaa371308b5246a98ca1a598bb5f0de2ea11cd3ff36adb2`;
full local editor
`84a3ace91de60b462d8148a5a4fd78dee82f11dddbe54e5901de8c45667fc615`.
Local integration includes selected source provenance, not a complete local
source export. No comparative GPU timing claims.

#94, #95, #108, #47 and #121 now record the publication; their original bodies
and open states were verified after updating. Complete glTF encoded-source
selection, content/role mapping, dense sampler descriptors, live GPU table
publication and interactive material/texture authoring still need coherent
publication. The legacy scene publisher remains active/append-only: retirement
requires matching fallback image/sampler/metadata publication before old leases
release. See `docs/texture-upload-and-ownership.md`. #112 remains completed; all
other original goal scopes remain active.

## glTF sampler and scene descriptor Git publication - 2026-11-05

Pushed `c2d3fc0db3e01f6a6acebb597547b9b346d7d149`: 41 focused files publish
all six minification filters, both magnification filters and independent wrap
axes through 108 canonical policies, texture cache identity, device-bounded
dense sampler slots and matching GPU texture metadata. Samplers are lazy;
incoming replacement budgets reject before retiring live scene images. Stable
image slots resolve inactive/empty rows to a private fallback and retain actual
allocation ownership. Every affected production material shader now uses the
dense domain, removing the former nine-entry array/clamp from raster, depth,
picking, overlays, ray shadows and DDGI.

The final gate passes 398 selected cases in 22 executables, ten Vulkan, with no
failures or skips. Independent compute sampling covers all 108 policies and
indices beyond the old wrap prefix. Five scene-descriptor tests exercise middle
retirement, empty/all-retired catalogs, invalid capabilities, stale metadata and
replacement-budget preservation. The three newly built full-local sampler
executables separately pass 17 cases; this is not a broader local-suite claim.
All 99 public SPIR-V modules validate: 28 existing material-consumer modules
change, 69 are byte-identical and two sampling probes are added. The 105 prior
local engine modules remain unchanged; its additional scene probe matches the
validated public probe. The broader convention suite retains exactly the eight
parent failures, excluded from passing counts.

Public native acceptance has 42 captures and 46 checks across both raster paths:
nearest/linear magnification, distinct mip policies, shared images, MASK/BLEND,
twelve simultaneous authored policies plus default, and active hard-ray/DDGI.
Four captures from the actual parent executable reproduce ignored filtering in
both renderers. Full-local acceptance adds 18 edit/save/reload/ray captures,
including eight pixel-exact save/reload pairs. The original ACCA IFCX remains
pixel-exact to the accepted prior image at 128 samples with zero unsupported or
rejected samples. It is read in place and excluded from the evidence archive.

Frozen evidence: `out/feature-parity/sampler-publication-acceptance/results.json`,
877 verified artifacts. The source export contains all 1046 canonical Git blobs;
selected publication sources are byte-exact and unchanged text LF/CRLF variants
in the compiled export are recorded. The archive includes source, binaries,
SPIR-V, XML/logs, captures, actual parent executable and harnesses. Published
renderer SHA-256 `52494bda4aaa91939acc8c1c0169b77a75d77d927da1e1296de2cba1d1a813fa`;
full local editor `376d1d76aa8a2c886ece51071e698360ab8b4a3b793514285d2e827964e1e8ce`.
Local evidence includes binary/test/native provenance, not a full local source
export. Binaries retain their tested precommit metadata. This milestone makes
no packaging or comparative GPU timing claim.

The documentation follow-up `1628014` updates the earlier ownership guide to
link to the now-published scene consumer. It changes exactly that Markdown file;
renderer code remains identical to the accepted feature commit. Receipt:
`out/feature-parity/sampler-publication-documentation-acceptance/results.json`.
Both commits are pushed and the index is empty.

#94, #95, #108, #47 and #121 record the publication; original bodies and open
states were verified after updating. Core sampler filtering is published; the
developed material-edit path still needs publication with #108. Complete glTF
encoded-source selection, mixed color/data-role mapping and interactive resource
authoring remain. Ray footprints/filtering and every other original transport,
asset/editor/environment/reload and tracker scope remain active. #112 stays
completed. See `docs/material-sampler-publication.md`.

## Published retained glTF source and typed texture import — 2026-11-05

Pushed `48036da5da59d1cc5ccac34fe65e290a76a02ce6` and packaging follow-up
`4dd19c4d351cf60b5a3d0880a05bd2e3d2bf28e2` to
`origin/codex/ui-scene-virtualization`. The index is empty. The follow-up
changes only the packager and two guides; renderer code is identical.

The actual scene now imports retained external, data URI and GLB image bytes
through a device-profile-aware typed Vulkan upload. Material texture roles
keep linear data separate from color while compatible image bytes and mips
can share ownership across sampler aliases. Checked image and descriptor
demand fails before a malformed replacement can retire the live scene. Added
a native factor probe so a bound texture cannot promote an authored zero.
Source loader publication also includes checked accessors, meshopt and the
bounded Draco worker. The published static consumer explicitly rejects
required scene features whose runtime consumers are still local.

The frozen acceptance receipt is
`out/feature-parity/gltf-texture-publication-acceptance/results.json`:
1778 verified artifacts, 1097 canonical Git blobs, 72 selected source paths,
635 public focused cases across 37 executables (10 Vulkan), 254 full-local
integration cases, 78 disabled-codec passes with two expected skips, and
99 validated public SPIR-V modules. The same eight broader convention
failures reproduce at the parent. Current native evidence includes 129 public
captures, two actual-parent role controls, 88 local editor captures and
12 pixel-exact package captures on a Windows-system-only PATH. The original
ACCA IFCX is read in place, never archived; its 128-sample reference capture
remains pixel-exact to the accepted prior image with no unsupported/rejected
samples. The packaged archive includes 27 codec/compression notice files,
the tone curve notice, four linked guides and the verified Draco worker.

Closed #94 as completed on the feature branch. Verified tracker updates to
#47, #87, #93, #95, #96, #108 and #121 without changing their open states;
#112 remains completed. Next: finish native compressed and accessor coverage
and verify live-scene preservation on malformed compressed replacements,
then publish the developed editor material/save/history and remaining glTF
runtime consumers. The outstanding reference path tracing and other tracker
scopes remain active.

## Native compression and vertex accessor acceptance — 2026-11-05

Pushed `fc0ce2ad12270a4fd265a04921ebad391a920274` to
`origin/codex/ui-scene-virtualization` and verified the empty index. The commit
adds an encoder-only meshopt filter fixture, independent Vulkan render
comparisons, and a rejected scene-reload regression. The published and full
local renderer binary hashes match their previous accepted builds.

The live Vulkan reload test rejects a truncated EXT meshopt stream and an
out-of-range Draco buffer-view index. Both diagnostics identify the source.
Original positions, indices, and both frame descriptor handles survive.
The compute probe samples all three existing textures in each frame slot
after each rejected replacement. The disabled-codec suite separately verifies
required compression rejection and valid meshopt fallback. Draco support
covers triangle lists and diagnoses strips.

The gate passed 135 focused cases (98 enabled CPU, 29 disabled CPU, eight
Vulkan scene cases), 106 published captures, and 172 full local captures.
Geometry references and compressed export/reload images match exactly.
The compression matrix covers EXT/KHR external and embedded data, three
modes, and four filters, including an animated joint driven by QUATERNION
data. Accessor captures include dense+sparse, sparse-only, quantized, and
float64 geometry, plus sparse morph/skin in the local editor. The vertex
alpha matrix includes OPAQUE, MASK, BLEND, transmission and raster/hard-ray
shadows in forward and deferred rendering. No Vulkan validation or
synchronization errors were detected.

Evidence with verified Git, binary, XML, and capture identities:
`out/feature-parity/geometry-native-acceptance.json`. GitHub #93 and #96
closed as completed and #121 updated and verified open. Publication of the
developed animation, deformation, history, save, and material editing
consumers remains in scope along with the other tracker and path tracing work.

## Native glTF playback and authored camera publication — 2026-11-05

Pushed `0043b113418befbc2b5eb1395d4a51934c7d1b15`. Staged 28 selected
source files from the verified build export and checked the other 1081
source files against the parent Git blobs. The index is empty. The receipt
with test binary, source and capture identities is
`out/feature-parity/gltf-playback-publication/results.json`.

The published Scene Controls panel selects clips and authored cameras,
plays/pauses, loops, changes speed and scrubs. Checked STEP, LINEAR and
CUBICSPLINE tracks drive real runtime node transforms, shader material
factors/texture transforms, punctual lights, and authored camera
projections. Rigid motion refreshes object/provider state while material,
light and lens discontinuities reset the relevant history. Camera view
and frustum drive the viewport and cascades, including a finite
shadow projection for an infinite perspective lens.

Required animation pointers are admitted for implemented runtime targets.
Malformed clips and unsupported required targets produce asset/channel
diagnostics before a failed replacement can retire the visible scene.
Successful replacement clears old clip and camera bindings. The published
viewport diagnoses unsupported weight tracks pending the Vulkan skin/morph
consumer. Camera authoring transactions and the broader editor workflows
remain separate.

The clean export passed 238 focused cases (45 dedicated animation tests and
two native Vulkan executables) and 44 animation captures, including 4 with
all directional shadow cascades active. Five current static raster images
matched the prior renderer exactly, including the original ACCA IFCX.
All 99 SPIR-V modules match the parent and the same eight broader convention
failures reproduced. The full local editor read the original IFCX in place
and matched the accepted 128-sample path trace with zero unsupported/rejected
samples. Four additional local captures show that animated BLEND emissive
endpoints match independently authored static reference materials at 128
samples. The published renderer's path-tracing backend tests ran
separately from the full local viewport.

Updated #47, #87, #90, #91, #109 and #121. All remain open. The next runtime
integration is GPU skinning and morph deformation across draws, picking,
bounds, shadows, ray geometry and submitted vertex history. All other goal
scopes remain active.

## Published Vulkan skinning and morph deformation — 2026-11-06

Pushed `120d96524bf776a2c0d348b5afd0a8be76899e08` to
`origin/codex/ui-scene-virtualization`. The Git index is empty. The 37 staged
files match the verified source export, and the other 1088 source files match
the parent. Acceptance evidence is in
`out/feature-parity/gltf-deformation-publication/results.json`.

Both raster techniques draw the current Vulkan morph-then-skin stream. The
previous submitted stream feeds temporal velocity. CPU bounds, selection rays
and shared ray acceleration use the matching evaluated pose. GPU transparent
pick depth works in both raster modes. A failed missing-joint replacement
reports its source and preserves the current animation and image. Valid reloads
match independent baked pose and rest references. Viewport PNG capture omits
selection and hover strokes so the live selection can survive an image capture
without appearing in it.

The final binary passed 336 focused cases in 17 executables, including five
Vulkan executables, plus 173 native captures and 20 GPU depth probes at 1x and
4x MSAA. All 100 SPIR-V modules validated. The same eight broader convention
failures reproduced. Five static comparisons were pixel exact. Additional
coverage includes mirrored hard-ray and raster receivers, compressed and sparse
animated data, regenerated surface frames, and required morph-weight pointers.

The full local editor opened the original ACCA IFCX in place and switched from
deferred raster to path tracing and back in the same lit view. The path view
reached 128 samples with no visible magenta patches or diagnostic warning. The
separate fixed replay recorded zero unsupported and rejected samples. Evidence
is in `out/feature-parity/gltf-deformation-acca-ui-recheck/results.json`.

GitHub #90 and #91 closed as completed. #47, #87, and #121 were updated and
verified open. The next build prepares the reference viewport on the exact
pushed source, with the remaining transport, editor and tracker scopes active.

## Published reference path-tracing viewport — 2026-11-06

Pushed `11e3c71c7c39325ca0c3fd92da0ac247cda1f125` to
`origin/codex/ui-scene-virtualization` and verified the remote head and empty
index. The build used 19 selected files on top of `120d965`, with the other
1110 source files verified against Git. Acceptance is frozen in
`out/feature-parity/reference-viewport-publication/results.json`.

Path tracing now appears in the existing Rendering Engine selector. The
viewport uses the Vulkan ray-query device contract and shared posed surface
scene, owns progressive float32 history, reports unsupported and rejected
paths, and exports the raw mean to EXR. Sampling settings, explicit renderer
switches and runtime fallback are available through the regular entry points.

The pushed viewport reads the original ACCA IFCX in place and reproduces the
accepted local 128-sample image with zero unsupported and rejected samples.
Switching to raster and back reproduces the 16- and 128-sample captures.
Animated blended emission and skin/morph endpoints match independently baked
assets pixel for pixel. The regression verifies exact sample limits, resize,
query-disabled raster fallback and unclamped 200000/100000/50000 EXR values.
Five static raster comparisons are exact. The gate passed 77 focused cases,
34 native captures and validation for 91 unchanged shader binaries. The same
eight broader convention failures remain.

GitHub #47 and #121 were updated and verified open. Remaining work includes
transport and convergence checks, editor publication for light/gizmo history,
and the other tracker items. The tested viewport executable is
`out/build/reference-viewport-publication/VulkanSceneRenderer.exe`.

## Published Vulkan light-history backend — 2026-11-06

Pushed `9fe3efdb6758bcbd5b9966dd6b621fec014b115d` to
`origin/codex/ui-scene-virtualization` and verified the remote commit. The
accepted export contains 21 selected files and 1117 files verified against the
parent. Evidence is in `out/feature-parity/light-history-publication/results.json`.

The backend prepares and exchanges the edited Vulkan light generation together
with its ECS state. Authored targets carry their scene and node identities.
The receipt preserves the original optional override and exact owned payload
through undo, redo, and continuous edits. Eight native Vulkan cases read actual
per-frame descriptors and buffers across both frame slots. The full gate passed
93 focused cases, 18 captures, and validation for the new probe shader. The 91
production shader binaries match the previous publication. The same eight
broader convention failures reproduced.

The original ACCA IFCX was read in place. The accepted 128-sample image matched
pixel for pixel with zero unsupported or rejected samples. The scripted
window capture switched from deferred raster to path tracing and back with the
editor drawn. The live UI check reached deferred raster and exposed a startup
layout issue before the window lost focus.

#88 remains open for editor controls and gizmo integration, shared node and
material history, and scene reload and downstream rendering acceptance.

## Published editor startup layout — 2026-11-06

Pushed `ab5ecb54de890103a3b9256ede57499de3acece6` to
`origin/codex/ui-scene-virtualization` and verified the remote head and empty
index. Three selected files match the validated export. The 1135 other
source files match the parent. The receipt is
`out/feature-parity/viewport-layout-publication/results.json`.

The editor opens in Lit mode. The Viewport panel starts in the lower right
and opens collapsed in compact windows. Renderer Telemetry starts collapsed
in the upper right on a fresh layout. The Rendering Engine, Display, and
model controls are visible in the 1280x720 and 640x360 window captures.
Explicit Overview also passed. The scripted ACCA switch reached 128 path
samples with no unsupported or rejected samples and returned to deferred
raster. The source IFCX was read in place. The broader convention suite
reported the same eight baseline failures.

The frozen executable is
`out/build/light-history-publication/VulkanSceneRenderer-viewport-layout.exe`.

## Published light inspector and gizmo Undo/Redo — 2026-11-06

Pushed `cbf8e269b89b4458cd14d91aa4e28b46689e3e6e` to
`origin/codex/ui-scene-virtualization`. The remote head and empty index were
verified. The accepted source export matches all 22 staged files and 1123
unchanged Git blobs. Evidence is in
`out/feature-parity/light-editor-publication/results.json`.

Scene Controls shows Undo and Redo above the existing model controls. Light
inspector values, manual light creation and light gizmo edits publish through
one bounded history. A drag records one command on release. Escape and focus
loss cancel a preview. Selection and panel deactivation settle active drafts.
Scene replacement retires both history branches. TAA starts a fresh epoch,
DDGI stages its per-image light snapshot capacity and history reset, and
recorders bind the new Vulkan light generation before drawing.

The final executable passed 137 focused cases and 74 native captures. Both
raster techniques matched independently authored light endpoints pixel for
pixel. Other checks cover the current animation clock, progressive path
replay, TAA, async DDGI, stale requests, reload and a rendered UI capture.
The original ACCA IFCX was read in place and matched the accepted image at
128 samples with zero unsupported or rejected samples. All 91 production
SPIR-V files match the prior release. The same eight convention failures
reproduced. Desktop input was exercised in the ImGui and input lifecycle test
fixtures. The full editor capture switched history via script.

The frozen executable is
`out/build/light-history-publication/VulkanSceneRenderer-light-editor.exe`.
#88 remains open for scene-node transforms and gizmos, visibility, reparenting,
primitives and the remaining shared authoring workflows. The full #121 goal
remains active.

## Published scene-node Undo/Redo — 2026-11-06

Pushed `449cd798cd49545e46ce3ab11eb7af0c08f366b1` to
`origin/codex/ui-scene-virtualization` and verified the remote head and empty
index. The 22 published files match the accepted export, and the other 1125
source blobs match the parent. Evidence is in
`out/feature-parity/node-editor-publication/results.json`.

The shared scene history now covers node transforms, visibility, reparenting,
and gizmo translation, rotation and scaling. Gizmo and inspector gestures
coalesce. Replay restores the selection, exact local matrices and sibling
order. World-space dragging converts through the parent basis. Reparenting
retains world placement and imported light identity. The graph, ECS and object
draw records are prepared before exchange. TAA history resets, async DDGI
publishes, and the path tracer receives the current scene instance data.

The accepted build passed 139 focused tests and 165 scripted Vulkan captures.
These include 91 scene-node captures, 64 light-history regressions and ten
path-tracing regressions. The original ACCA IFCX was read in place and matches
the accepted 128-sample capture with zero unsupported and rejected samples.
All 91 production SPIR-V modules match the previous renderer. The same eight
broader convention failures reproduced.

The frozen executable is
`out/build/light-history-publication/VulkanSceneRenderer-node-editor.exe`.
Transforming or reparenting a node driven by the active glTF pose clip requires
clearing that clip first. Primitive creation and remaining authoring and
physics history remain open under #88. The full #121 goal remains active.

### 2026-11-06 BIM room volume visibility

Published `7bad96a` on `codex/ui-scene-virtualization`. IFC room geometry starts hidden and remains available under BIM > Layer Visibility > Room volumes. The original ACCA file matched the comparison with the room volumes excluded, pixel for pixel, in all three techniques. Hiding the volumes restored daylight in the path-traced interior. Point-light raster shadow tests passed in forward and deferred. The eight broader convention failures match the prior baseline. Results are in `out/feature-parity/room-volume-publication/results.json`.

### 2026-11-06 procedural primitive history

Published `9d17e71` on `codex/ui-scene-virtualization`. Adding a primitive records shared history. Undo removes its mesh and hierarchy entry, including the editor root when creation added one. Redo restores the same node identity, mesh, and selection. The runtime suite exercised forward and deferred raster, path tracing, interleaved transforms and light edits, skinning and morphing, TAA, and asynchronous DDGI. The original ACCA regression passed. Results are in `out/feature-parity/primitive-editor-publication/results.json`.

### 2026-11-06 portable scene save

Published `2512786` on `codex/ui-scene-virtualization`. Scene Controls now offers Save and Save As for GLB and glTF. The editor saves the current primary scene and retains other authored scenes. GLB embeds its resources. glTF uses a neighboring content-addressed binary dependency. Save stages output before replacing the destination.

The accepted renderer passed 113 focused tests and 18 pixel-identical save and reload comparisons. The original ACCA IFCX was read in place and its viewport regression passed. The eight convention failures match the previous baseline. Issue #89 remains open for scaled light ranges in inactive scenes and linked asset export under #102. Results are in `out/feature-parity/scene-save-publication/results.json`.

### 2026-11-06 multi-scene export lighting

Published `a92d52f` on `codex/ui-scene-virtualization`. The renderer accepts `--gltf-scene N` at startup. Export includes the scaled light ranges and animation keys of inactive scenes. Imported light edits follow shared source nodes. Empty authored scenes can be selected and saved.

The accepted build passed 113 focused tests, four pixel-identical scene export comparisons, and ten ACCA/reference viewport captures. The original ACCA model was read in place. The same eight convention failures reproduced. Evidence is in `out/feature-parity/multiscene-light-publication/results.json`. Live switching and linked asset export remain open.

### 2026-11-06 live glTF scene switching

Published `7fad844` on `codex/ui-scene-virtualization`. Scene Controls now selects named glTF scenes. The complete graph keeps shared identities, preserves per-scene authoring, and refreshes lights, cameras, deformation, ray instances and probe history as the selected scene changes. A switch pauses playback and clears undo, camera selection and transient editor state. Saves retain edits to inactive scenes.

The accepted build passed 115 focused tests, 38 switch/save captures, 39 independent deformation captures including ray shadows and DDGI, ten ACCA/reference viewport captures, and two editor screenshots. The original ACCA IFCX was read in place. The same eight convention failures reproduced. Results are in `out/feature-parity/live-gltf-scene-publication/results.json`.

### 2026-11-06 glTF node visibility and picking

Published `1273a08` on `codex/ui-scene-virtualization`. Scene Hierarchy now edits visible, selectable and hoverable flags. Visibility includes attached lighting, while CPU and GPU picking use independent inherited ancestor routes. Imported light edits and history keep their source identity through hide/show. Both save formats retain flag metadata and hidden lights.

The accepted build passed 152 focused tests, including the hierarchy widgets, plus 37 native feature captures and the prior light, node, scene-save and ACCA regressions. The same eight convention failures reproduced. Results are in `out/feature-parity/node-visibility-publication/results.json`. Full trace-through, behavior events and boolean animation-pointer channels remain pending.

### 2026-11-06 Extended glTF materials

Published `2efd7e9` for #92. Forward, deferred, transparent and DDGI rendering now use anisotropy and diffuse transmission. The clean build passed 175 focused cases, 24 independent Python cases, 92 SPIR-V validations and 151 native captures. Six remaining convention failures predate this change. Original ACCA path tracing matches the last published render pixel for pixel. Active extended lobes still require reference path sampling. The report is in `out/feature-parity/extended-pbr-publication/results.json`.

### 2026-11-06 Import and startup hardening

Published `1a980c7`. Bounded IFC style and sweep traversal, validated IFCX PCD counts and sizes before allocation, removed the unused file sink, checked the swapchain image fence result and reused the default scene token. The isolated build passed 258 cases and 12 native captures. Two curved IFC test failures reproduce against the parent. All five IFCX model tests read the existing files in place. The original ACCA viewport is pixel-identical to `2efd7e9`. Evidence is in `out/feature-parity/code-review-reproduction/publication.json`.

### 2026-11-06 Runtime shader reload publication

Published `ddeb2bb` for #113. The editor reloads Slang through F5 and Scene Controls. The compiler has a two-minute deadline and shutdown cancels its process tree. The isolated Windows build passed 132 focused cases, 68 native Vulkan captures and 92 unchanged SPIR-V module validations. Six broader source-convention failures match the parent. The full local editor separately passed four compiler cases and 79 reload captures. The original ACCA image matches `1a980c7` pixel for pixel. The receipt is in `out/feature-parity/shader-reload-timeout/results.json`.

### 2026-11-06 Secondary-ray texture footprints (#82)

Ray-hit texturing now carries ray-cone footprints matching the vk_gltf_renderer reference: camera rays start from the angular spacing between neighbouring samples (or the sample's world size for orthographic projection), continued segments inherit the slant-corrected footprint of the surface they left, and per-triangle UV-to-world density converts that diameter into per-coordinate-set explicit gradients through each texture transform's linear rows. DDGI probe rays derive an equivalent cone from their equal-solid-angle ray count, and refracted continuations inherit the surface footprint; coverage candidates and shadow connections keep base-mip reads exactly like the reference renderer's shadow paths.

Validation: 48 path-transport GPU cases (four new footprint contracts: slant geometry, per-set texel density, equal-solid-angle spread and authored-mip gradient selection) pass with zero validation errors, plus 24 ray-scene CPU, six ray-scene GPU and twelve DDGI transport GPU cases. A same-lineage baseline rebuild proves the untextured Cornell reference capture stays byte-identical, a grazing checker-plane capture differs only through footprint filtering, and a deferred capture with hard ray shadows and fitted DDGI runs validation-clean. Evidence is in `out/feature-parity/ray-footprint-validation/`.
