# OpenVDB feature completion ledger

Audit baseline: 25 October 2026, OpenVDB 12.0.1. The request is to complete the
integration, including the previously requested simulation, physically complete
multiple scattering, vector/point workflows and authoring UI. The integration is
**not complete**. Existing rendering and processing tests establish the behavior
already delivered; they do not establish coverage of every upstream capability.

This is a finite inventory of capability families. Each installed public header
in `tools`, `points`, `math` and `io` is assigned below. A family can contain
several operations, schemes or data representations. An implemented operation
does not make the rest of that family implemented. New operations discovered
inside an existing family must be added to its acceptance checks, not silently
treated as covered by a related operation.

Status meanings:

- **Implemented**: the stated workflow has a public API, its applicable CLI/UI
  route, and recorded validation. This says nothing about a broader neighboring
  family.
- **Partial**: some useful behavior exists, but the row lists concrete gaps.
  Work in progress stays partial until it is integrated and validated.
- **Pending**: no complete application workflow has been demonstrated.
- **Inapplicable**: a library implementation detail or a different host process
  cannot be a standalone Container workflow. The reason is explicit; related
  user-facing capabilities remain open in their own rows.

Priorities order implementation; they do not remove features from the request.
P0 covers explicit outstanding user requirements and dependencies for the next
batch. P1 covers remaining core OpenVDB workflows. P2 covers optional packages,
backends and deployment expansion. A pending P2 row is still pending.

## Audited sources and current evidence

The installed public-header inventory contains 51 top-level `tools` headers,
23 `points` headers, 24 `math` headers and nine `io` headers. Local primary
sources are under
`out/openvdb-deps/x64-windows/include/openvdb`. NanoVDB headers come from the
pinned OpenVDB 12.0.1 source tree selected by `cmake/NanoVdbDependencies.cmake`.
The optional AX sources exist in that upstream checkout, but AX headers/library
are not installed in the current OpenVDB prefix. No AX CMake target or execution
route exists in Container.

Repository evidence inspected:

- `include/Container/geometry/OpenVdb*.h` and their implementations: import,
  density loading, sequences, CPU processing, particles, advection, segmentation,
  OBJ export and inspection.
- `tools/openvdb_process.cpp`: the existing CPU command routes.
- `src/renderer/effects/OpenVdbVolumeRenderer.cpp`, `shaders/openvdb*.slang`,
  frontend loading/playback and `src/utility/GuiManager.cpp`: rendering,
  lighting, transparency, asynchronous uploads and current volume controls.
- The dependency modules, Windows staging/packaging, CPU suites and validation
  scripts described in [the usage guide](openvdb.md).

The point/calculus/solver batch passed 24 enabled CPU/configuration/CLI groups
in 32.82 seconds and 21 disabled groups in 6.03 seconds. The subsequent expanded
batch passed its enabled CPU/CLI module groups, followed by the packaging check
after the renderer rebuild, and all 25 disabled groups in 7.63 seconds. This adds
point radius previews, five Platonic solids, coupled smoke stepping and the CPU
spatial-transport reference. Logs remain under `out/openvdb-validation`.

The rebuilt renderer passed 21 targeted captures: 12 point/smoke captures in
77.036 seconds, three interchange captures in 15.742 seconds, and six dense/native
transport captures in 26.197 seconds. All 41 subprocess commands succeeded with
zero Vulkan validation errors. Advected markers match an independently shifted
reference exactly; static Float/Double smoke images match exactly in both tested
rendering paths. Results are `expanded-{points,interchange,transport}-runtime/`
under that evidence directory. Earlier evidence includes the 98-capture suite.

The vector/sequence application regression now passes nine captures and sixteen
commands in 34.684 seconds with zero Vulkan validation errors. Signed glyph
displacements, physical tube radius, native MAC display and visible buoyant
density evolution are checked independently. Both paused sequence frames match
the corresponding static file exactly. Evidence is
`vector-sequence-runtime/run-20261025T153409Z-57100-c8d745b8/results.json`.
The standalone physical GPU reference is registered in the main build: all
eleven tests and SPIR-V validation pass in 2.14 seconds. It does not yet replace
the interactive approximation.

The latest shared batch passes all 41 enabled CPU/configuration/CLI/UI/shader
groups in 46.50 seconds and all 38 disabled groups in 11.49 seconds. It validates
the stored-Float mesh guard, native attribute/group editing and selection,
dense interchange, multiresolution, SDF transport/morphing, fluid sources,
coupled viscosity/source addons and sequence progress/cancellation. Logs are
`expanded-workflows-cpu.log` and `dense-selection-disabled.log`. Native point
editing has 17 feature cases, selection/statistics 13, dense interchange ten and
multiresolution ten.

The next shared batch passes all 43 enabled groups in 43.66 seconds and all
40 disabled groups in 13.40 seconds, with the Windows package runtime check
passing in 2.72 seconds. It adds native file authoring, strict JSON specs and
real CLI/UI workflows, fixes Windows Unicode argument handling, and validates
direct integer-to-Float point attribute conversion. Evidence is
`file-authoring-final-cpu.log`, `file-authoring-disabled.log` and
`file-authoring-package.log`. Image export and the Python bridge were validated
in the subsequent batch below.

The image/filter/Python batch passes all 48 enabled groups in 45.52 seconds
and all 45 disabled groups in 13.86 seconds. Main CPU/CLI/UI proof includes
scene-linear EXR readback, physical image jobs, ordinary masked field filtering
and the 18-case typed Python bridge suite. The Python universal wheel also
passes an isolated installed-package native round trip. Evidence is
`image-filter-python-cpu.log`, `image-filter-python-disabled.log` and
`out/openvdb-review/python/`. The main GPU transport reference (eleven cases)
and image suite (five cases) pass together in 3.28 seconds, with no skipped
cases or Vulkan validation errors. Eleven physical image CLI cases pass with
CPU/GPU Unicode paths and independent EXR readback. The always-run shader stage
repairs a deliberately stale SPIR-V copy without relinking its executable.
Evidence is `image-main-gpu.log`,
`image-main-cli/run-20261025T171206Z-6d7bcc2200/results.json` and
`image-stage-without-link.json`. The expanded point-mask/composite/slice/native
preview OFF build passes all 50 groups in 16.35 seconds. The rebuilt application
passes all 54 enabled CPU/configuration/CLI/UI/shader groups in 57.51 seconds
and the clean-PATH Windows package runtime check in 2.80 seconds. Ten new
application captures across thirteen commands pass in 38.302 seconds with zero
Vulkan validation errors. They check visible scalar slices, named physical point
radii, adjacent Int64/string colors, group removal, opacity and forward/deferred
parity. Evidence is `composite-preview-cpu.log`, `composite-preview-disabled.log`,
`composite-preview-package.log` and
`extended-preview-runtime/run-20261025T181804Z-10556-36cf2d70/results.json`.
Real CLI proof exposed and fixed a Unicode dense-input filename bug; the dense
suite now has eleven feature cases. MaskGrid inspection correctly reports its
Boolean value type, while native tests verify its distinct MaskTree storage.
The topology/native sampling batch passes 57 enabled groups in 59.98 seconds,
53 disabled groups in 21.58 seconds and the package runtime check in 2.58 seconds.
Fifteen native topology and eleven native sampling cases now have main API,
CLI/UI/OFF proof. The panel has 61 choices. Tests exposed and fixed compact mask
tile values and paged point-array preservation before ZIP cloning/writing.
Aggregate point/field topology limits, exact nearest integers, quadratic
polynomials and dynamic-range rejection, remote point offsets, group selection
and unselected signed-zero/metadata retention are covered. Evidence is
`topology-sampling-cpu.log`, `topology-sampling-disabled.log`,
`topology-sampling-main-cli.log` and `topology-sampling-package.log`.
The subsequent NanoVDB batch passes 61 enabled CPU/configuration/CLI/UI/shader/
package groups, 56 disabled groups and all 20 live GPU cases. Native interchange
has 28 full-build cases and native statistics twelve. Independent production
dependency discovery builds without OpenVDB/TBB/CUDA pass 22 interchange and
eleven statistics cases, with codecs enabled or disabled. Seven CLI groups run
there (one VDB-conversion-only skip). Ten official SDK11 ABI32.6 fixtures validate;
ten raw copy/statistics outputs read back in that SDK. Native file writing,
conversion, compression, CRC coverage/repair and application routes now have
proof; broader format/validity/device features remain open. Evidence is
`nano-integrity-{cpu,disabled,gpu}.log` under `out/openvdb-validation`, plus
`out/openvdb-review/nano-{standalone,legacy-sdk}/`.

The native value/mask edit batch passes all 61 enabled CPU/configuration/CLI/UI/
shader/package groups in 123.89 seconds and all 56 disabled groups in 26.11
seconds. Topology now has 31 feature cases (15 occupancy, ten native value edits,
six masked edits), plus three availability cases. Eleven real CLI groups and
26 panel cases include the new settings snapshots and all 70 operation choices.
All 20 live transport/image/upload GPU cases pass in 5.48 seconds, without GPU
skips or VUIDs. The application is rebuilt with both new authoring workflows.
Evidence is `masked-values-{cpu,disabled,gpu}.log`, `masked-values-main-cli.log`
and `masked-values-ui.log` under `out/openvdb-validation`.

The SDF background/exact-transform batch passes all 61 enabled groups in
144.74 seconds, all 56 disabled groups in 18.27 seconds and all 20 live GPU
cases in 5.35 seconds, without GPU skips or VUIDs. Topology has 39 feature
cases, including seven signed-background/flood cases and one cross-workflow
exact-map regression. Fourteen CLI groups and 27 panel cases cover all 71
operation choices. Twelve dense and 28 NanoVDB cases retain distant scales and
sub-tolerance shears exactly. Native grid copies and affine factories avoid SDK
map simplification; nontrivial fog advection rejects unstable upstream copies
with a recentering diagnostic. Evidence is `sdf-background-{cpu,disabled,gpu}.log`,
`sdf-background-main-cli.log`, `sdf-background-ui.log`,
`exact-affine-dense-tests.log` and `exact-affine-nano-full-tests.log`.

The metadata inspection follow-up passes all 61 enabled groups in 126.42
seconds and all 56 disabled groups in 17.28 seconds. Sixteen native inspection
cases cover exact native transform records, affine coefficient-derived voxel
sizes, stable duplicate ordering, shared-tree instance metadata, malformed
offset/count rejection and metadata-only Unicode-parent reads without tree
payloads. The application is rebuilt. Evidence is
`exact-inspection-{cpu,disabled}.log` and `exact-inspection-tests.log`.

The scalar moments/histogram batch passes all 61 enabled groups in 124.90
seconds and all 56 disabled groups in 16.60 seconds. Inspection now has 23
feature cases; eight real tool CLI groups and 28 panel tests include moments,
optional exact-boundary histograms and immutable settings. Point statistics
retain their thirteen passing feature cases after sharing the fixed exact
accumulator. An independent Fraction/1800-digit Decimal oracle matches all
1,500 histogram and 1,200 weighted-moment cases, including extreme values,
adjacent doubles, subnormals and UInt64 weights. The application is rebuilt.
Evidence is `inspection-statistics-{cpu,disabled}.log`,
`inspection-statistics-{main-cli,ui,point}.log` and
`out/openvdb-review/exact-statistics/results.json`.

The native SDF filter batch passes 61 available main CPU/configuration/CLI/UI/
shader groups in 133.63 seconds, all 57 disabled groups in 26.61 seconds and
all twenty live transport/image/upload GPU cases in 6.95 seconds, without GPU
skips or VUIDs. Seventeen CLI groups and thirty panel cases cover thirteen
native filter/tracker modes, alpha masks, five spatial schemes, three time
schemes and explicit band/trim controls. The application is rebuilt with 72
panel choices. The configured main suite has 62 groups: its topology test
executable was quarantined by Bitdefender after linking, so this batch does
not establish a full 62-group pass. The quarantine entry identifies
`Gen:Variant.Application.FCA.3835`; whether this is a false positive remains
unconfirmed. No security settings or quarantined payload were changed.
Evidence is `sdf-filters-{cpu-available,disabled,gpu,main-cli,ui}.log`.

The reviewed normalization/inspection follow-up adds a native activity mask
for normalization, preserving all unselected world-value/activity bits across
nine mask types, and avoids the SDK's small absolute-determinant affine
constructor during inspection. A combined native proof passes 46 feature
cases with two enabled-build availability skips in 6.429 seconds. It includes
Double intermediate-scaling underflow rejection. Source and CLI/UI routes are
integrated and pass the coordinated follow-up below. Evidence is
`sdf-normalization-mask-native-tests.log`.

The sphere-fitting/sweeping follow-up passes all 64 available enabled groups
in 143.27 seconds, all 60 disabled groups in 19.67 seconds, and all twenty live
GPU cases in 6.05 seconds without skips or VUIDs. At that checkpoint, the
configured enabled suite had 65 groups and the Bitdefender-quarantined topology
executable was the single unresolved validation gap. Sixteen native sphere-fitting cases and
seventeen fast-sweeping cases cover their API/CLI/UI/OFF workflows. Eighteen
topology/normalization CLI groups, seven new sphere/sweeping command groups and
33 panel cases pass. The application is rebuilt with 75 choices. Point sampling
now has fifteen native cases covering joint file metadata, full delayed-buffer
preflight, smaller-axis world precision and MAC product underflow. An unrelated
scene-authoring CMake registration changed after initial configuration;
regeneration resolved its application link failure without source edits.
Evidence is `sweeping-spheres-{cpu-available,disabled,gpu}.log`,
`sweeping-spheres-{new-cli,topology-cli,ui}.log` and
`sweeping-spheres-app-repair.log` under `out/openvdb-validation`.

The region/query/fracture batch passes all 68 available enabled groups in
182.68 seconds and all 64 disabled groups in 24.14 seconds. The twenty selected
transport/image/upload GPU cases pass in 4.98 seconds without GPU skips or
VUIDs. Sixteen region, eight surface-query and twelve fracture feature cases
have native/API/CLI/UI/OFF proof. The new command harness runs five methods:
four pass and the OFF-only method skips in the enabled run. All 36 panel cases
pass in 7.097 seconds across 78 operation choices. Evidence is
`regions-surface-fracture-{cpu-available,disabled,gpu,ui,main-cli}.log` and
`regions-surface-fracture-ui.xml` under `out/openvdb-validation`.
The configured enabled suite has 69 groups; the available-group run excludes
the topology executable. The user-allowed existing executable subsequently
passes CTest in 3.72 seconds (3.75 seconds real), but its 01:32:13 timestamp
predates the implementation's 01:48:54 timestamp. That historical runtime pass
does not validate the current source. Refreshed write-open/link still fails
Access Denied/LNK1104, leaving the current fresh-binary validation gap open.
No security settings or bypass were used. Evidence is `topology-allowed-tests.log` and
`topology-allowed-build.log`; prior quarantine notes remain historical.

The field-advection/potential-flow/point-index and discrete-slice batch passes
all 74 available enabled groups in 187.49 seconds, all 70 disabled groups in
28.20 seconds and all twenty selected transport/image/upload GPU cases in
5.94 seconds without GPU skips or VUIDs. The application is rebuilt with 82
choices; all forty panel cases pass in 7.727 seconds, including immutable
settings and exact integer preview-range snapshots. The configured enabled
suite has 75 groups; only the current topology binary remains excluded for
the fresh-link access failure described above.

Native field-advection/potential-flow/point-index feature counts are seventeen/
thirteen/twelve, with OFF availability proof. Focused enabled CLI methods pass
8/8/6 cases respectively, each with one expected OFF-only skip, in
9.030/7.943/4.664 seconds. The complete coordinated run includes these command
harnesses. The initial 70-group OFF result (32.07 seconds) and the earlier
forty-case panel result (6.913 seconds) remain historical checkpoints.
Evidence is `advection-flow-index-slice-{cpu-available,disabled,gpu}.log`,
`advection-flow-index-slice-{authoring,slice_preview,field_advection,potential_flow,point_neighbors}.xml`
and `advection-flow-index-slice-build.log` under `out/openvdb-validation`.

Discrete Bool/Mask/Int32/Int64/Vec3I slices have thirteen native feature passes
plus OFF availability. Eight hidden validated renderer captures pass in
30.486 seconds: Int64 IDs above 2^53, Bool stored values, Mask activity and
Vec3I components are pixel-exact against four independently authored Float
references; adjacent IDs remain visibly distinct. Native fixture bytes stay
unchanged, and no VUID, validation error or synchronization hazard appears.
The opt-in fixture helper also passes ON/OFF compilation, Unicode-path and
exclusive-output checks. Retained proof is
`out/openvdb-validation/discrete-preview-runtime/run-20261026T074742Z-47860-325e4f96/results.json`.

The next closed-contour rebuild milestone passes all 76 available enabled
CPU/configuration/CLI/UI/shader/package groups in 182.04 seconds and all 72
OFF groups in 29.36 seconds. The application is rebuilt with 83 authoring
choices; all 41 panel cases pass in 7.055 seconds. Twelve native rebuild feature
cases pass with the availability check skipped in the enabled build, and the
OFF availability case passes. Its CLI route is included in the coordinated
run. The enabled configuration contains 77 groups; the fresh topology link
remains blocked as described above. Earlier GPU/capture evidence belongs to
the preceding 82-choice milestone; this rebuild milestone changes CPU authoring.
Evidence is `out/openvdb-validation/level-set-rebuild-{build,cpu-available,disabled}.log`
and `level-set-rebuild-{authoring,native}.xml`.

The surface-field/native-point-motion/generic-transfer milestone is integrated
into the rebuilt 86-choice application. Its 21/13/16 native feature cases and
disabled availability checks pass. Focused real CLI checks pass 10/7/7 enabled
cases, respectively, with the expected OFF-only skips, in 9.557/15.144/8.926
seconds. All 44 panel cases pass in 7.829 seconds. All 82 available enabled
CPU/configuration/CLI/UI/shader/package groups pass in 233.95 seconds, and all
78 OFF groups pass in 34.28 seconds. The enabled configuration includes one
additional topology group whose current executable cannot be freshly linked
while Windows denies replacing it.

A subsequent slice correction preserves nearest-cell selection immediately
below half-cell ties for Int64, Bool, Mask and Vec3I. The nineteen native slice
feature cases pass in 1.816 seconds, with OFF availability also passing.
The application was rebuilt with that correction; all twenty GPU
upload/transport/image cases pass in 5.27 seconds and all eight rendered
discrete-preview captures pass without validation errors. The broad 82-group
run precedes this correction; these targeted native/GPU/application checks
validate the correction. Evidence is under `out/openvdb-validation/`:
`surface-motion-transfer-{cpu-available,disabled,authoring-focused}.log`,
`surface-motion-{surface,point}-cli.log`, `surface-motion-transfer-cli.log`,
`slice-midpoint-enabled.{log,xml}`, `slice-midpoint-app-retry.log`,
`surface-motion-transfer-gpu.log`, and
`discrete-preview-runtime/run-20261026T192213Z-53600-5c04a22d/results.json`.

The ordinary-field-resampling milestone passes all 84 available enabled
CPU/configuration/CLI/UI/shader/package groups in 258.77 seconds and all 80 OFF
groups in 40.97 seconds. The application is rebuilt with 87 authoring choices;
all 46 panel cases pass in 8.454 seconds. Eighteen integrated native resampling
feature cases pass in 1.972 seconds, with the enabled availability case skipped;
OFF availability passes. Six real CLI cases pass with one OFF-only skip in
10.433 seconds. The broad run also includes the corrected nineteen-case slice
suite. One additional configured topology group remains excluded because the
restored executable runs but Windows still denies replacing it during linking.
No GPU/rendering change is claimed for this CPU authoring milestone; the twenty
GPU cases and eight rendered captures above belong to the preceding rebuilt
slice-correction milestone. Evidence is under `out/openvdb-validation/`:
`field-resampling-{build,cpu-available,disabled,native,cli,authoring-focused}.log`
and the corresponding native/authoring XML files. Remaining V05 variants stay
open in the ledger.

The diagnostics/boundary milestone passes all 89 available enabled
CPU/configuration/CLI/UI/shader/package groups in 279.35 seconds and all 85 OFF
groups in 44.33 seconds. The application is rebuilt with 91 authoring choices;
all 52 panel cases pass in 9.035 seconds. Integrated diagnostics has 26 native
feature cases and 12 actual CLI cases; boundary projection/replay has 23 native
feature cases and nine actual CLI cases. Eight integrated provenance cases
verify derived-grid invalidation and exact-copy exceptions for the five reserved
boundary keys. These enabled native suites skip only their OFF availability
case. Actual disabled CLI proofs pass three diagnostics and two boundary cases.
The boundary test was freshly rebuilt after its portable environment-guard fix.
Fresh previews from the rebuilt application pass eight captures in 46.0 seconds
with zero Vulkan validation errors, exact independent Float-reference parity,
and visibly ordered adjacent Int64 colors. These are new rendered checks; the
twenty GPU unit cases above remain evidence from the earlier slice milestone.
One additional configured topology group still lacks a fresh link because
Windows denies replacing the restored executable. Linux validation remains
pending: the reviewed nine-module ON/OFF container harness is prepared but UNRUN
because the Docker engine is unavailable. Evidence is under
`out/openvdb-validation/diagnostics-boundaries-*`, including the milestone JSON,
native/authoring XML and `diagnostics-boundaries-discrete-runtime/`
`run-20261026T204836Z-24460-0da920f0/results.json`. Broader diagnostics, boundaries,
simulation and backend requirements remain open in the ledger.

The native-point/Nano-primitive milestone passes all 93 available enabled
CPU/configuration/CLI/UI/shader/package groups in 301.88 seconds and all 89 OFF
groups in 47.18 seconds. The application is rebuilt with 93 authoring choices;
all 57 panel cases pass in 10.117 seconds. Twenty native-advection feature cases
and all thirteen unchanged affine-Motion cases pass; their enabled availability
cases skip. All fourteen primitive tests pass, including its availability check.
The actual main Tool passes eight advection CLI cases and nine primitive CLI
cases, with one OFF-only skip in each. Actual OFF CLI proof passes two and three
cases respectively. The advection fixture/readback-only repair is documented;
it does not fix the two adjacent legacy Unicode/tiny-map routes.
An independently built full native-I/O-only Tool has nativeIo=true and
openVdbConversion/openvdbAvailable=false, with no OpenVDB/TBB DLL dependency.
It passes 47 native cases, nine primitive CLI cases and six existing Nano CLI
cases, plus the native-advection unavailable route. Exact sources, binary hashes,
logs/XML and the 91-to-92-to-93 authoring chain are recorded under
`out/openvdb-validation/native-point-nano-*` and
`out/openvdb-review/native-io-full-tool/`. Rendering evidence belongs to the
preceding 91-choice milestone; these additions are CPU authoring workflows.
The topology fresh-link and Linux runtime gaps above remain unresolved, and
the broader point/Nano families remain partial.

The active-region/ray-query milestone passes all 97 available enabled
CPU/configuration/CLI/UI/shader/package groups in 332.32 seconds and all 93 OFF
groups in 57.90 seconds. The application is rebuilt with 96 authoring choices;
all 64 panel cases pass. Eleven native active-region cases and eighteen native
ray-query cases pass, plus seven and ten actual CLI feature cases respectively;
their enabled availability cases skip. The tracked independent SDK fixture
producer supplies all nine native value types, exact integer/nonfinite payloads,
binary metadata, weak Double crossings and coarse active-leaf/tile spans. Region
reports retain exact typed values and full counts under bounded record quotas.
SDF ray hits use approximate native HDDA/chord refinement; volume spans cover
coarse active leaves/tiles rather than exact voxel occupancy or fog integrals.
Both read-only report routes suppress stale output/model/restart actions.

This milestone also repairs the two legacy routes left open by the preceding
93-choice checkpoint. Exact tiny PointData maps and encoded offsets now reach
direct attribute and native neighbor commands without unit-map re-encoding.
Legacy velocity output uses checked ASCII SDK scratch followed by exclusive
Unicode publication, including Unicode parent paths. Four direct regression
groups execute twelve real Tool commands against the original tiny-map fixture;
the strengthened tracked native-advection CLI passes eight feature cases plus
one OFF-only skip. Six legacy native suites pass 95 feature cases; source and
foreign output bytes are preserved. Hosts without a writable ASCII TEMP or
working-directory scratch route still receive an explicit unsupported-path
diagnostic. The fresh topology link remains blocked by Windows after the user's
allow action, and Linux runtime validation remains unrun. No new GPU capture
proof is claimed. Exact sources/binaries, logs and native/authoring XML are
recorded in `out/openvdb-validation/region-ray-milestone.json`,
`region-ray-final-*` and `out/openvdb-review/legacy-route-fixes/`.

The value-component/surface-reference milestone passes all 101 available enabled
CPU/configuration/CLI/UI/shader/package groups in 341.86 seconds and all 97 OFF
groups in 29.59 seconds. The application is rebuilt with 98 authoring choices;
all 71 panel cases pass in 11.487 seconds. Eight native component cases and
sixteen native surface-reference cases pass, plus eight and eleven actual CLI
feature cases respectively; enabled availability cases skip. The component CLI
independently reads all nine value types, inactive/nonselected finite payloads,
exact maps and binary metadata. Its corrected canonical fixture checks both
pressure's three membership keys and boundary fractions' full five keys rather
than assuming clamp diagnostics exist on pressure.

Component outputs retain the whole source field's values while selecting one
connected activity component, without asserting separate physical solids. The
CPU reference uses native perspective/orthographic center rays, matte/normal/
headlight response and eleven linear FLOAT EXR channels, with independent
standard-library pixel/header/camera/depth/status readback. A real default32
sphere probe found that the inherited 200M ray-work cap was insufficient; the
surface-specific default is now 2B, ordinary ray-query defaults are unchanged,
and the real CLI regression uses no image/work overrides. EXR results cannot
become VDB model/restart inputs, including custom executors and later draft
changes. All variable report data is prepared before publication. Source and
foreign output bytes are preserved; native/OFF/Unicode/cap failure paths pass.
Exact source/binary hashes, XML and logs are recorded under
`out/openvdb-validation/value-surface-*`, especially `value-surface-milestone.json`.
The topology fresh-link and Linux runtime gaps remain open. This milestone adds
CPU image proof; it does not claim fresh Vulkan captures or close the remaining
families in the ledger.

The subsequent particle-authoring/fog checkpoint rebuilt the application with
99 choices and passed 77 panel cases. Its focused, pre-policy source/binary scope
is retained in `out/openvdb-validation/particle-authoring-fog-proof.json`.

The current particle/fluid/fog milestone on 28 October rebuilds the application
with 101 authoring choices and passes all 107 available enabled CPU/configuration/
CLI/UI/shader/package groups in 378.40 seconds, plus all 103 OFF groups in
52.71 seconds. All 92 panel cases pass in 13.754 seconds. The main build passes
14 particle-geometry, 24 boundary-projection, 25 boundary-fluid-step and 21
fog-integral native feature cases; actual CLI cases pass 10, 13 and 9 for
particle geometry, boundary fluid and fog respectively. Enabled availability
cases skip. Source and binary hashes, complete group logs and native XML are
recorded in `out/openvdb-validation/particle-fluid-fog-milestone.json`.

This integrates three independently reproduced repairs: exact cofactor-zero
guards reject lost weak inverse components; the boundary-fluid stored/strict
policy makes restart conditions explicit without hidden projection or weak-value
erasure; and particle artifacts use exact native descriptor spelling for record
separator names. Boundary-fluid authoring snapshots all eight restart roles and
the submitted policy; fog authoring is read-only and exposes no output/model/
restart actions. The exact enabled topology executable remains inaccessible
after the user's allow action; it is the single excluded CPU group, without a
renamed/relocated substitute. Linux execution and fresh GPU/display proof remain
open. Weighted point statistics and fixed-boundary sequences are ongoing staged
work, not part of this passed milestone or completion of the full integration.

## Capability ledger

### Build, files and representations

| ID | Family and upstream source | Status | Existing behavior and remaining acceptance checks | Priority |
|---|---|---|---|---|
| B01 | Optional dependency, runtime staging and packaging | Partial | Windows ON/OFF, module/config discovery, runtime DLL staging, Nano header discovery and notices are validated. Complete clean Linux ON/OFF builds and dependency/runtime packaging checks; retain matching Debug dependency/CRT coverage. The shared native writer and legacy velocity route now validate ASCII SDK scratch before exclusive Unicode publication; direct Unicode parent/filename and tiny PointData-map regressions pass. Audit remaining narrow-name wrappers and hosts without a writable ASCII scratch route; this proof does not establish every native path configuration. Optional components need independent discovery and unavailable diagnostics. | P1 |
| F01 | Grid type/representation coverage (`Grid`, `Types`, point trees) | Partial | Native authoring preserves Float/Double/Bool/Mask/Int32/Int64/Vec3I/Vec3S/Vec3D/PointData grids with selection and metadata/compression controls; dense interchange covers the four real scalar/vector types. Surface/density display remains Float/Double, with separate vector/point previews. Broader type conversions and associated loss diagnostics remain open; vectors, normals, positions and staggered data retain distinct semantics. | P0 |
| F02 | Metadata and diagnostics (`Statistics`, `Count`, `Diagnostics`, file metadata) | Partial | Twenty-four native inspection cases and shared API/CLI/UI/OFF/package proof cover bounded metadata, exact native affine records, coefficient-derived voxel sizes, stable duplicate ordering, per-instance metadata, encoded offset/count checks and archive UUID matching before scalar tree loading. Weighted Float/Double extrema/sum/count/mean/population deviation/RMS and optional 1..4096-bin exact-boundary histograms retain sparse tile multiplicity, empty/constant behavior and report caps; fixed exact accumulators also serve native point statistics. An independent 2700-case rational/high-precision oracle matches. Typed stored-value, symmetric-SDF/gradient, normalized-fog and PointData diagnostics plus Bool union failure masks, bounded examples and explicit unavailable coverage now pass 26 native feature cases, 12 actual CLI cases, disabled availability and the rebuilt 91-choice application/panel proof. Broader field summaries/statistics, diagnostic overlays and explicit repair choices remain open. Higher moments and broader histogram policies remain open. | P1 |
| F03 | General VDB file authoring (`io/File`, `Archive`, `Stream`, `Queue`, compression) | Partial | Native APIs and strict version 1 JSON specs expose bounded copy/select/rename/remove/merge, typed file/grid metadata and ZIP/Blosc/none compression with explicit half/active-mask loss opt-ins. Eleven native and seven spec groups plus CLI/UI/OFF/package checks pass, including exact integer metadata, Unicode spec-relative paths, registered point codecs/strides/groups, empty files, shared caps and no-overwrite cleanup. Seekable supported-version inputs are required. Stream conversion and asynchronous file-library output remain open. | P1 |
| F04 | Dense interchange and multiresolution (`Dense`, `DenseSparseTools`, `MultiResGrid`) | Implemented | Native Float/Double/Vec3S/Vec3D CVDBARR import/export retains finite bits, x-fastest order, absolute origin, affine map, background and explicit activity; cropped data/custom metadata omissions are reported. Bounded full-weighting levels support metadata inspection, fractional world sampling and all/single-level output for ordinary/fog fields and collocated world vectors. Twelve dense and ten multiresolution cases plus real CLI/UI/OFF routes pass; Unicode input filenames, remote inactive leaves, transformed nonsymmetric arrays, signed zero, subnormals and shared retained/scratch/work limits are covered. SDF/MAC/position/normal semantics are explicitly rejected. | P1 |

### Scalar fields, surfaces and topology

| ID | Family and upstream source | Status | Existing behavior and remaining acceptance checks | Priority |
|---|---|---|---|---|
| S01 | Scalar isosurface import and indexed mesh export (`VolumeToMesh`) | Implemented | Float/Double level-set or scalar isosurfaces enter the scene and export OBJ positions/normals/groups. Validate winding under reflections, transforms, isovalue/adaptivity, duplicate selection, empty/mixed unsupported inputs, budgets and exclusive output; current suites cover this contract. Generic attribute transfer is S03. | P1 |
| S02 | Closed mesh to narrow-band SDF (`MeshToVolume`) | Implemented | Supported scene loaders feed bounded closed-mesh FloatGrid SDF generation. World-instance transforms, distance signs, source preservation and failure diagnostics are tested. Unsigned/open-mesh conversion and custom attribute transfer are S03. | P1 |
| S03 | Extended mesh/volume conversion and sphere fitting (`MeshToVolume`, `VolumeToSpheres`) | Partial | Unsigned/open triangle-mesh distances and native nearest-triangle IDs pass eleven feature tests plus scene-loader/CLI/UI/OFF checks, including transformed-instance provenance, duplicate/permutation ties and shared budgets. The stored-Float guard rejects unreproducible instance transforms. Bounded native Float/Double volume-to-spheres JSON output now has sixteen native cases plus CLI/UI/OFF proof: deterministic approximate unit/index-conditioned fitting, fog isosurface rebuild, physical post-radius limits, metadata and shared adaptive-stage budgets. Generic mesh attribute transfer remains open. | P1 |
| S04 | Procedural level sets (`LevelSetSphere`, `LevelSetPlatonic`) | Implemented | Sphere plus tetrahedron/cube/octahedron/dodecahedron/icosahedron APIs, CLI and authoring controls are integrated. Eight primitive tests and all five real CLI solid workflows pass the enabled/OFF batch. Explicit canonical scale/center/world voxel/band rules, signs, bounds, mesh readback and conservative workspace caps are covered. | P1 |
| S05 | SDF Boolean CSG (`Composite`) | Implemented | Union, intersection and difference with compatible transforms/bands are available and tested. Arithmetic/min/max compositing of general fields belongs to V02. | P1 |
| S06 | SDF editing/tracking (`LevelSetFilter`, `LevelSetTracker`, `LevelSetRebuild`) | Partial | Thirteen native Float/Double filter/tracker modes expose mean/Gaussian/median, curvature/Laplacian flow, fillet, world offset, normalization/track/prune and band resize/dilate/erode, five spatial and three temporal schemes, trim modes and Float/Double alpha masks. Full stored-box/growth/buffer preflight, unit/index conditioning, exact affine/file metadata, exclusive output and near-interface gradient quality are tested. Activity-masked normalization supports all nine nonpoint mask types and preserves every unselected world value/activity bit; its native, CLI/UI and OFF proofs pass. Broader native Float/Double closed-contour rebuilding now has API/CLI/UI routes for explicit isovalues, asymmetric target-voxel bands, preserved or explicit uniform orthogonal target maps, conditioned small/remote coordinates and native type/custom metadata. Twelve native feature cases and coordinated CLI/UI/OFF proof cover fog contours, high Double isovalues, refinement, reflected/rotated targets, caps and immutable submissions. Native Float mesh geometry remains approximate and can merge overlaps or close bubbles. General diagnostic validity/repair and topology-preserving repair remain open. | P1 |
| S07 | Connected components/interiors (`LevelSetUtil`) | Partial | Existing zero-surface segmentation and sixteen native mask/interior/enclosure/cavity cases retain their API/CLI/UI/OFF proof. Native value-preserving six-connected components now add eight native and eight actual CLI feature cases, including all nine types, active false/zero/nonfinite values, inactive continuations, exact maps/metadata, deterministic names and joint pre-expansion/output budgets. Each non-Mask output retains the entire source field and changes component activity; Mask has activity alone. Empty activity writes one explicit inactive copy. The rebuilt 98-choice application and 71-case panel proof include this route. These outputs do not certify isolated solids/SDFs or physical density/mass partitions. Optional fill-mask constraints, isosurface-mask extraction, broader asymmetric/dense surface segmentation and cavity-safe solid exports remain open. | P1 |
| S08 | Surface measurement and closest-point/curvature queries (`LevelSetMeasure`, `GridOperators`) | Partial | Existing world area/volume, average mean curvature, Gaussian integral and estimated Euler pass twelve feature tests. Eight new Float/Double native cases plus API/CLI/UI/OFF proof cover approximate native surface-point-cloud distance/position, active-footprint signed interpolation, closest-point normal and mean/Gaussian curvature, planes/spheres/torus signs, rotation/reflection/world units, remote-origin conditioning and shared query/index/work limits. Empty batches return validated typed metadata without an index. Validity flags/null JSON distinguish unavailable stencils. Euler remains a resolution-dependent estimate; point-cloud proximity is not exact triangle/implicit projection. Twenty-one additional native cases and actual CLI/UI/OFF proof cover grid-wide mean/Gaussian curvature, world normals and native local CPT with Bool validity outputs, exact active footprints, promotion, rotation/reflection, remote precision, empty retained leaves, metadata and shared limits. CPT uses x-phi times the unnormalized gradient at the source cell; exact surface projection and broader geometry/validity policies remain open. | P1 |
| S09 | Level-set transport and morphing (`LevelSetAdvect`, `LevelSetMorph`, tracker) | Partial | Float SDF advection and morph APIs pass twelve feature groups, CLI/UI/OFF checks and independent review. Signed world-velocity advection, target relaxation, preserved affine maps/bands, unit-voxel conditioning, HJWENO5/TVD-RK2 tracking, CFL/work/growth caps and Float progress diagnostics are covered. Alternate schemes, time-varying/MAC velocity, masks and intermediate-frame authoring remain open; Float time can round the final unresolved remainder. | P1 |
| S10 | Fracture (`LevelSetFracture`) | Partial | Twelve native cases and API/CLI/UI/OFF proof cover matching Float/Double CSG fracture, uniform world-rigid cutter instances/pivots, overlap policy, deterministic residual/fragment names, retained small negative fragments, optional surface segmentation and exact negative-sample partition accounting including new cutter-zero boundaries. Exact maps/custom source metadata, Double inverse sampling, source preservation, aggregate growth/piece/workspace/work caps and exclusive ZIP publication are tested. Unsegmented cuts retain nested cavities; segmented negative-exterior cavity surfaces reject. Distances/continuous volume are approximate. Cavity-safe value-preserving solid labels, point-driven cutter inputs, broader map/band resampling, fragment distance repair and continuous-volume validation beyond sample counts remain open. | P1 |
| S11 | Fast sweeping and extrapolation (`FastSweeping`, `TopologyToLevelSet`) | Partial | Two APIs expose fog/SDF conversion, reinitialization, band dilation, mask extension, topology-to-Float SDF and matching-precision scalar/world-vector extension. Seventeen native cases and real CLI/UI/OFF proof cover analytic fields, all nine activity types, one-sided preservation, disconnected rejection, unit-map conditioning, native underflow and shared dense-domain/tree/work caps. Fixed sweep counts yield approximate distances without certifying convergence/closure; conservative sparse limits, supported field precision/semantics and topology Float output are explicit. Integer/mixed-precision extension and broader backend/type variants remain open. | P1 |
| V01 | General scalar/vector filtering (`Filter`) | Implemented | Float/Double and supported collocated world vectors have mean/Gaussian/median/offset API/CLI/UI routes with Float/Double alpha masks, exact affine-map matching, iterations and explicit tile voxelization. Fourteen feature cases plus real CLI/UI/OFF tests cover kernels, native Float weights, lexicographic vector median, inactive/background bit preservation, semantic-tag rejection and pre-expansion/workspace/work budgets. SDF/MAC filtering remains in its separate families. | P1 |
| V02 | Field arithmetic/compositing (`Composite`, `Merge`, `ValueTransformer`) | Implemented | Ordinary Float/Double/Vec3S/Vec3D sum/product/divide/min/max/blend/active-merge and copy/affine/absolute/clamp have API/CLI/UI/OFF proof with 22 native cases and independent dense CLI readback. Exact type/map/semantic matching, independent activity policies, whole logical-field/background arithmetic, active-only/all-value scope, inward native clamp bounds, metadata, signed zero, remote inactive structure and shared work/tree/writer limits are explicit. No implicit resampling or SDF/MAC reinterpretation occurs. Division rejects zero background/inactive denominators; nonfinite/range loss fails before publication. | P1 |
| V03 | Topology editing and masks (`Activate`, `Clip`, `Mask`, `Morphology`, `Prune`, `SignedFloodFill`, `ChangeBackground`) | Partial | Fifteen occupancy, ten value-edit, six masked-edit and seven SDF background/flood feature cases have API/CLI/UI/OFF proof, plus an exact-map regression. These cover 6/18/26-neighbor morphology/tile policies, bit-exact activity edits/pruning, inclusive index fill/clip, exact inactive background replacement and masked clipping. Native types retain exact Int64, signed zero, inactive values, affine coefficients, vector frames and metadata. Float/Double LEVEL_SET background normalization and signed flood fill retain every active value; native asymmetric widths, closed-field sign-seed limitations, root-span overflow and pre-growth budgets are explicit. Width changes do not expand the active band or repair missing/open surfaces. Ordinary/fog edits output UNKNOWN class; exact pruning retains SDF/staggered class. Tolerant matching/pruning, broader world/frustum clip domains and geometric interior/surface repair remain open. | P1 |
| V04 | Differential vector/scalar calculus (`GridOperators`, `math/Operators`) | Implemented | Gradient, divergence, curl, Laplacian, magnitude, normalization and vector sampling have tested API/CLI/UI routes. The main module has 13 enabled feature cases, including affine/world units, background/inactive rules, tile bounds, tiny/large normalization, translated sampling and remote-index precision rejection. The 24-group enabled and 21-group OFF batches pass. S08 retains the other geometric operators. | P0 |
| V05 | Sampling, transforms and vector transformations (`Interpolation`, `GridTransformer`, `VectorTransformer`, `VelocityFields`) | Partial | Native point-column sampling now exposes exact integer/Boolean nearest and floating/world-vector nearest/linear/quadratic modes, independent affine maps, compensated remote offsets and explicit native MAC reconstruction, with fifteen native cases and CLI/UI/OFF proof. Existing direct scalar/vector queries, density resampling and velocity transport cover narrower contracts. Eighteen new native cases and independent peer review cover affine ordinary nine-type nearest/four-real-type higher-order resampling, explicit world deformation and all five WORLD vector semantics, independent activity, exact Int64/Bool/Mask/inactive bits, conditioned maps and shared typed memory/work/metadata/writer bounds. Its real CLI, 87th authoring choice and rebuilt application pass all 84 available enabled groups, 80 OFF groups and 46 panel cases. Broader direct batch queries, antialias/conservative policies, nonlinear/frustum transforms and staggered/MAC resampling remain open. | P1 |
| V06 | Spatial queries and validation (`FindActiveValues`, `RayIntersector`, `RayTracer`, `Diagnostics`, proximity) | Partial | Active-region records/counts retain eleven native/seven CLI cases; approximate SDF hits/normals and coarse activity spans retain eighteen/ten. CPU SDF reference images retain sixteen/eleven and panel/EXR proof. Nodal Float/Double fog integrals pass 21 main native/nine actual CLI cases and read-only authoring within the rebuilt 101-choice application milestone of 107 enabled and 103 OFF groups: inactive/background values, exact rational cell events/complements, positive cubic Simpson sums, WORLD I/tau, thin absorption and explicit opaque-underflow status under shared source/report/primitive bounds. Panel summaries display the first 64 completed results without truncating native queries. Native inverse/direction remain approximate; logical caps are not an RSS sandbox. Exact voxel occupancy, broader affine maps/kernels, stronger root guarantees, AA and broader shading remain open. | P1 |

### Particles, point grids and simulation

| ID | Family and upstream source | Status | Existing behavior and remaining acceptance checks | Priority |
|---|---|---|---|---|
| P01 | Particle spheres and trails to SDF/masks (`ParticlesToLevelSet`) | Partial | Native sampled tapered trails expose signed time, spacing, origin phase, radius policy and Float SDF/native MaskGrid/both output. Fourteen main native and ten actual CLI cases pass in the current milestone of 107 enabled and 103 OFF groups; the rebuilt 101-choice application passes 92 panel cases. Strict SAX input, complete-union center corrections, exact maps, Float rounding, joint swept/tree/partition/writer limits, immutable snapshots and per-grid actions are covered. Mask valueType is bool; its enum preserves native identity. Occupancy tests centers strictly inside stamps; final samples need not reach endpoints. Exact record-separator artifact selectors now pass native action tests. Source-ID/attribute ownership, continuous trajectories, cell-intersection masks and generic PointData input remain open. | P1 |
| P02 | PointData creation/readback (`PointDataGrid`, `PointConversion`) | Implemented | Standard P/v/pscale JSON/VDB creation/readback, CLI round-trip and authoring routes pass the coordinated enabled/OFF batch. Tests cover world/index position precision, strict descriptors/codecs, transforms, point/topology/attribute caps and independent upstream readback. Delayed-loading and legacy allocation limits are documented. This standard-layout contract rejects additional attributes/groups; generic preservation and editing remain P03. | P0 |
| P03 | Point attributes, strings and groups (`AttributeArray`, `AttributeArrayString`, `AttributeSet`, `AttributeGroup`, `PointAttribute`, `PointGroup`, `PointDelete`) | Partial | Eight typed native attribute Add/Set/Rename/Drop operations and group editing pass 17 feature cases; strict JSON has ten and selection/statistics 13, with shared CLI/UI/OFF validation. Exact int64, strings/dictionaries, inactive traversal, registered codecs/metadata/flags, mixed signed zero, fixed strides, protected P and transactional outputs are covered. Group keep/delete preserves native attributes; empty results archive the schema in metadata. Dynamic-stride compaction, broader compression choices and repopulation policies remain open. | P0 |
| P04 | Point queries, masks, sampling and statistics (`PointCount`, `PointMask`, `PointSample`, `PointStatistics`, filters/iterators) | Partial | Standard v/pscale and generic native-column sampling, selected-group count/statistics and original-point-voxel MaskGrid/optional exact Int64 counts have API/CLI/UI/OFF proof. Fifteen sampling cases cover exact nearest integers, floating/world-vector nearest/linear/quadratic, native MAC, independent transforms, inactive records, group Add/Set, metadata and unselected bits under shared source/field limits. Ten mask cases cover nonlinear source maps and empty-schema archives without decoding/repartitioning P. Native extrema preserve exact types and bounded exact accumulators yield means/population deviations. Weighted statistics and broader query/filter policies remain open. | P1 |
| P05 | Point scattering, motion and advection (`points/PointScatter`, `PointMove`, `PointAdvect`; `tools/PointScatter`, `PointAdvect`) | Partial | Seeded density-weighted scattering and signed RK2/RK4 point advection pass 16 enabled feature tests together with standard sampling, real CLI/UI workflows and OFF availability in the coordinated batch. General native P deformation/repartition now preserves registered arbitrary fixed-stride attributes/codecs, IDs, strings/groups, descriptor/grid/file metadata, hidden flags and inactive records. Thirteen native cases and actual CLI/UI/OFF proof cover selected affine movement, reflected targets, codec trials/exact no-ops, activity collision rejection/explicit OR, payload verification, archives, Unicode and shared limits. This changes P only. Generic fixed-layout static-velocity RK2/RK4 advection now adds twenty native feature cases, actual CLI/UI/OFF proof and the rebuilt 93-choice application. Independent maps, raw WORLD or declared local-MAC velocity, inactive records, group-selected motion, one final repartition, separate velocity-index travel/endpoint-codec diagnostics and aggregate retained topology/work caps are covered. Replication/automatic IDs, dynamic-stride movement and time-varying velocity remain open; the older standard-layout route remains available. | P0 |
| P06 | Point rasterization and transfer (`PointRasterizeSDF`, `PointRasterizeTrilinear`, `PointRasterizeFrustum`, `PointTransfer`) | Partial | Native standard P/v/pscale to physical-radius SDF and fixed eight-cell CIC mass/density, normalized velocity and radius splats pass eleven isolated groups, shared CLI/UI and OFF checks. Double mass conservation, companion density, inactive records, metadata, precision/range limits and aggregate unique-cell/node preflight are covered. The fixed splat kernel is radius-independent. Sixteen additional native cases and actual CLI/UI/OFF proof now cover generic eight-type numeric CIC weighted sum/mean, positive mass columns or constants, Double/explicit Float storage, exact dyadic cancellation/tiny support, Int64 loss opt-in, Double weight/world-density companions, groups/inactive records, exact target maps and shared native scratch/node/work/writer limits. Zero/cancelled cells keep positive-weight activity; stored vector components receive no implicit frame conversion. Arbitrary kernels, MAC/frustum transfer and selected dynamic/strided channels remain open. | P0 |
| P07 | Point indexing, partitioning and particle atlas (`PointIndexGrid`, `PointPartitioner`, `PointsToMask`, `ParticleAtlas`) | Partial | Twelve native cases, focused CLI and OFF/UI proof cover native center/particle-intersection queries, original references including inactive points, inclusive rounded-Double boundaries, groups, adaptive multi-radius atlas levels and optional stable 8³ buckets. Native registered P codecs stay unmodified. Explicit-map MaskGrid occupancy/optional exact Int64 counts rebucket centers rather than marking original stored voxels or particle volume. Source/attribute/metadata, native index nodes, query/hit/bucket and writer budgets are shared. Two point-index panel choices and the rebuilt 82-choice application pass the coordinated enabled/OFF proof. Reusable index lifecycles and broader partition/filter/batch policies remain open; raw PointPartitioner sort controls are not exposed. | P1 |
| M01 | Passive scalar/vector transport (`VolumeAdvect`, `VelocityFields`) | Partial | Seventeen new native cases plus focused CLI/OFF/UI proof cover ordinary Float/Double scalar and collocated world Vec3S/D channels with six adapted schemes (semi, midpoint, RK3/RK4, MacCormack/BFECC), componentwise clamp/revert/none correction limits, final activity masks, explicit tiles and world/native-MAC velocity sampling. Double tracing avoids native pruning/position narrowing; exact no-ops and unselected bits survive. Signed time, per-step/total travel, full multipass halos, joint-map precision and source/output/work budgets are explicit; overshoot/round-trip/rounding diagnostics are not conservation/error certificates. The older Float-fog route retains its narrower contract. The rebuilt application and coordinated enabled/OFF proof pass. Time-varying velocities and broader transport/layout policies remain open; passive transport is not a fluid solver. | P0 |
| M02 | Scalar Poisson solver (`PoissonSolver`, `ConjGradient`) | Implemented | `WritePoissonSolution`, its eleven enabled feature cases, CLI/UI routes and OFF behavior pass the coordinated batch: Float/Double active domains, zero Dirichlet exterior, world-unit scaling, iteration/tolerance/workspace caps, independent stored-output residual, manufactured solutions, nonunit voxels, disconnected domains, tiles and failure cleanup. Projection is separate M04. | P0 |
| M03 | Potential flow (`PotentialFlow`) | Partial | Thirteen native cases plus focused real CLI/OFF/UI proof cover uniform world far-field/collider velocities on a bounded native18 exterior shell, projected-normal Neumann cells and zero-potential outer Dirichlet faces. Float/Double source conditioning, exact maps, world flow/potential/domain/projected-boundary outputs, full stored seven-point residual and global iteration caps are checked. World relative motion precedes conditioning; unresolvable stored total-flow precision rejects while exact co-motion stays exact. Boundary normal error and validity-gated interior divergence/curl are diagnostics; native boundary substitution drops tangential perturbation derivatives. The rebuilt application and coordinated enabled/OFF proof pass. Spatial boundary fields, compatible pure-Neumann gauges and caller-defined domains remain open. This is irrotational boundary-driven flow, not arbitrary velocity pressure projection. | P1 |
| M04 | Incompressible projection with forces and boundaries (application solver using upstream building blocks) | Partial | Closed MAC projection retains nineteen native cases. Fixed-domain boundary/canonical replay now passes 24 main native cases, CLI/UI/OFF/provenance and 107-group integration proof. A stable-source audit reproduced four lost weak inverse components on tiny represented rotations in direct/replay APIs; an exact two-product cofactor-zero identity guard now rejects those maps without weakening original oracles. Other nonzero inverse rounding stays approximate. Moving geometry, cut-cell/no-slip/variable-density policies, full factored inverse support and broader boundaries remain open; this does not close M05. | P0 |
| M05 | Complete simulation stepping (application behavior) | Partial | Closed-domain RK2 retains viscosity/source/sequence CLI/UI/OFF proof. Separate fixed-boundary scalar/MAC stepping passes 25 main native and 13 actual CLI cases plus complete authoring/restart proof within 107 enabled/103 OFF groups. Conservative donor flux, reservoirs, advective MAC transport, WORLD forces and one projection per attempt retain rejected-trial accounting and actual dt semantics. A reproduced successful near-rest output rejected by its own next input gate is repaired by explicit TransportStored default and RequireSolenoidal entry/endpoint policy, preserving actual finite compressibility under existing CFL/range/balance limits without a hidden solve. Nine-grid restart snapshots all canonical six plus density/temperature selectors and policy. Moving geometry/free surfaces, cut cells, vorticity, point coupling and boundary sequences remain open; the sequence design is staged work only. | P0 |

### NanoVDB, optional packages and application workflows

| ID | Family and upstream source | Status | Existing behavior and remaining acceptance checks | Priority |
|---|---|---|---|---|
| N01 | Native NanoVDB conversion and sampling (`CreateNanoGrid`, PNanoVDB, samplers/HDDA) | Partial | Float/Double VDB becomes checked native Float32 Vulkan density/temperature buffers with transforms and conservative empty skipping. Add remaining relevant value/quantization/index/point representations and conversion policies; test CPU/GPU parity and errors appropriate to each representation. | P1 |
| N02 | NanoVDB interchange, integrity and tooling (`io/IO`, `NanoToOpenVDB`, `GridValidator`, `GridChecksum`, `GridStats`) | Partial | Native eight-type read/inspect/validate, raw/segmented writing, mixed-codec CRC repair and exact VDB round trips are implemented with bounded offsets, ownership, long names, metadata extensions and conservative shared caps. Independent native-only production dependency discovery has 22 passing cases, without OpenVDB/TBB/CUDA, plus seven command groups (one conversion-only skip). ZIP/BLOSC/NONE, Unicode parents, exact Int64, inactive signed zero, vector frames, opaque grid/file metadata, tiny affine shear and upstream inactive-only origin recovery have isolated proof. Seven CLI/UI operations are wired. The full build has 28 native interchange and 12 statistics cases, including nonempty Boolean leaves and retained metadata. The coordinated batch passes 61 CPU/config/CLI/UI/shader/package groups, 56 disabled groups, and 20 live GPU cases without GPU skips or VUIDs. Ten independent official-SDK11 ABI32.6 fixtures validate; ten raw copy/statistics outputs read back in that older SDK. Both legacy CRC conventions, tiny-tail/blind-data coverage gaps and strict full-coverage rejection are explicit. Broader representations, validity reports and further adversarial/cross-platform coverage remain open. | P1 |
| N03 | NanoVDB authoring/device tools (`GridBuilder`, primitives, CUDA tools/device buffers) | Partial | Source-free CPU sphere, axis-aligned box and WORLD-Y torus authoring now has Float/Double SDF/fog API, real CLI and immutable authoring-panel routes. Fourteen native tests, nine actual CLI cases, disabled proof and the rebuilt 93-choice application/panel batch pass. A full native-I/O-only Tool proves generation without OpenVDB/TBB. Double-first geometry, all-stored Float value audit, explicit bounded Float-map caches, zero fog exterior including absent root tiles, native statistics/CRC/codecs and joint preallocation/exclusive Unicode output are covered. General shapes/maps, quantized/index/point authoring, CUDA/device buffers and broader backend parity remain open. | P2 |
| X01 | AX expressions (`openvdb_ax`) | Pending | Add optional LLVM/AX dependency discovery, bounded volume/point expression execution, attribute bindings, compile/runtime diagnostics and enabled/OFF packaging. Validate scalar/vector/point expressions and user-controlled loops/resources with explicit limits; absence of installed AX is a dependency gap. | P2 |
| X02 | Python interoperability (optional upstream bindings) | Implemented | The supported stdlib-only container_openvdb package exposes typed CVDBARR/array buffers, all eight point columns/groups, standard point clouds, selected counts/statistics/filtering and native file authoring through versioned runtime discovery. Eighteen tests cover independent binary fixtures, IEEE/int64 preservation, malformed/runtime failures, Unicode and real native round trips; an installed universal wheel passes an isolated native proof. It is a documented compiled-tool data bridge; upstream pyopenvdb is optional and unused. | P2 |
| R01 | Bounded GPU density/thermal lighting, shadows and transparency | Implemented | Dense/native previews, overlapping media, directional/point/spot/area lights, geometry/self shadows, emission/temperature and transparent surface interleaving are validated within documented fixed budgets. Async replacement and resize/MSAA/technique lifecycle tests exist. R02 tracks the remaining physical requirement. | P0 |
| R02 | Physically complete multiple scattering (application renderer) | Partial | The interactive renderer retains its local bounded extra-order approximation. CPU spatial transport has sixteen reference tests; Float32 GPU transport has eleven main tests plus shader/OFF proof. Offline pinhole image APIs, strict scene jobs, CLI/UI and linear FLOAT EXR/uncertainty output have shared CPU/OFF/package proof, five main validated GPU image cases and eleven CPU/GPU CLI cases, including indirect wall lighting, convergence, Unicode paths and independent EXR readback. Stale shader staging repairs without executable relinking. Finite-order bias is explicit. The viewport adapter, broader lights/geometry/surface BSDFs, indirect temperature emission and OIT remain open. | P0 |
| R03 | Vector/point visualization and field diagnostics | Partial | Physical-radius standard points, vector glyphs/streamlines, scalar/vector sample planes and typed native-point previews have API/CLI/UI/asynchronous model routes. Earlier eight/nine slice/native-point cases and ten application captures cover named radii, exact point Int64 auto-ranging, string palettes, groups, opacity and forward/deferred parity. Slices now have nineteen native cases plus OFF availability, covering adjacent representable half-cell coordinates, explicit raw stored WORLD components for all five native vector tags and adding Bool/Mask/Int32/Int64 scalar and world Vec3I nearest samples, exact signed64 ranges/auto-ranging and shared Unicode/full-payload preflight; merged CLI/UI controls pass the rebuilt application and eight new validated captures, with pixel-exact Float reference parity and distinct adjacent Int64 colors. Existing thirteen vector cases and nine captures cover signed world displacement, tube radius, affine/MAC conventions, clipping and generated sequences. Wider diagnostic overlays and nonlinear/frustum preview maps remain open. | P0 |
| R04 | Authoring UI for current and new operations | Partial | The rebuilt application has 101 choices and all 92 panel cases pass; all 107 available enabled groups and 103 OFF groups pass at the current recorded source/binary checkpoint. Particle artifacts retain exact submitted per-grid selectors, including native record separators. Boundary-fluid artifacts retain canonical six plus density/temperature roles and policy through draft switches, suppress invalid generic outputs and support explicit restart/inspection. Read-only fog authoring snapshots rays/options, completes all native queries before bounded display and suppresses output/model/restart actions even for custom executors. Immutable/async/report/EXR/ordinary-output controls remain tested. Broader authoring and R05 lifecycle/cancellation requirements stay open. | P0 |
| R05 | Simulation/expression/point authoring experience | Pending | Extend authoring to the pending solver, time-step, point/group/attribute and AX workflows. Require retained settings, clear domain/boundary/units controls, jobs/cancellation, bounded undo/state and deterministic sequence preview/export. | P0 |
| H01 | DCC-host plugins (Houdini/Maya/Wolfram host code) | Inapplicable | These execute inside different proprietary host processes and are not the installed standalone OpenVDB library API. No such host process/SDK is part of Container. Equivalent file, authoring and visualization workflows remain explicitly pending above; this row cannot close them. A requested host plugin would require its own host/SDK integration. | P2 |
| H02 | Standalone commands for every vector/matrix/iterator/allocation helper | Inapplicable | `math`, tree traversal, host buffers, compression and temporary-file helpers are library substrate, not independent end-user workflows. They remain available through the dependency and must be exercised by the features using them. Matrix/transform/calculus/solver capabilities are explicitly tracked in V04/V05/M02; no helper exemption closes those rows. | P1 |

## Public-header coverage map

This map prevents useful but less visible tool families from disappearing from
the ledger. It accounts for the 107 installed top-level headers in the four
audited directories. `points/impl` files implement the corresponding public
headers and do not create separate user workflows.

| Public headers, relative to `openvdb/` | Ledger coverage |
|---|---|
| `tools/LevelSetSphere.h`, `LevelSetPlatonic.h` | S04 |
| `tools/MeshToVolume.h`, `VolumeToMesh.h`, `VolumeToSpheres.h` | S01-S03 |
| `tools/LevelSetFilter.h`, `LevelSetTracker.h`, `LevelSetRebuild.h` | S06/S09 |
| `tools/LevelSetUtil.h`, `LevelSetMeasure.h` | S07/S08 |
| `tools/LevelSetAdvect.h`, `LevelSetMorph.h`, `LevelSetFracture.h` | S09/S10 |
| `tools/FastSweeping.h`, `TopologyToLevelSet.h` | S11 |
| `tools/Filter.h`, `Composite.h`, `Merge.h`, `ValueTransformer.h` | V01/V02 and S05 |
| `tools/Activate.h`, `ChangeBackground.h`, `Clip.h`, `Mask.h`, `Morphology.h`, `Prune.h`, `SignedFloodFill.h` | V03 |
| `tools/GridOperators.h`, `GridTransformer.h`, `Interpolation.h`, `VectorTransformer.h`, `VelocityFields.h` | V04/V05/S08/M01 |
| `tools/Count.h`, `Statistics.h`, `Diagnostics.h`, `FindActiveValues.h`, `NodeVisitor.h` | F02/V06/H02 |
| `tools/Dense.h`, `DenseSparseTools.h`, `MultiResGrid.h` | F04 |
| `tools/ParticlesToLevelSet.h`, `ParticleAtlas.h`, `PointAdvect.h`, `PointIndexGrid.h`, `PointPartitioner.h`, `PointScatter.h`, `PointsToMask.h` | P01/P05/P07 |
| `tools/PoissonSolver.h`, `PotentialFlow.h`, `VolumeAdvect.h` | M01-M05 |
| `tools/RayIntersector.h`, `RayTracer.h` | V06/R02 reference work |
| `points/PointDataGrid.h`, `PointConversion.h`, `StreamCompression.h` | P02/P03 |
| `points/AttributeArray.h`, `AttributeArrayString.h`, `AttributeGroup.h`, `AttributeSet.h`, `PointAttribute.h`, `PointGroup.h`, `PointDelete.h` | P03 |
| `points/IndexFilter.h`, `IndexIterator.h`, `PointCount.h`, `PointMask.h`, `PointSample.h`, `PointStatistics.h` | P04/P03 |
| `points/PointAdvect.h`, `PointMove.h`, `PointScatter.h` | P05 |
| `points/PointRasterizeFrustum.h`, `PointRasterizeSDF.h`, `PointRasterizeTrilinear.h`, `PointTransfer.h` | P06 |
| `math/ConjGradient.h`, `Operators.h`, `FiniteDifference.h`, `Stencils.h` | M02-M04/V04/S08 |
| `math/Transform.h`, `Maps.h`, `LegacyFrustum.h` | V05/F02/R01 |
| `math/BBox.h`, `Coord.h`, `DDA.h`, `Proximity.h`, `Ray.h`, `Stats.h` | V06/F02/H02 |
| `math/Half.h`, `Math.h`, `Mat.h`, `Mat3.h`, `Mat4.h`, `QuantizedUnitVec.h`, `Quat.h`, `Tuple.h`, `Vec2.h`, `Vec3.h`, `Vec4.h` | Representation/numeric substrate F01/N01/H02 |
| `io/Archive.h`, `Compression.h`, `DelayedLoadMetadata.h`, `File.h`, `GridDescriptor.h`, `Queue.h`, `Stream.h`, `TempFile.h`, `io.h` | F02/F03/B01/H02 |

## Solver contracts and required distinctions

The installed Poisson implementation supplies the integrated scalar solver.
`tools::poisson::solveWithBoundaryConditions` accepts a scalar tree, boundary
functor, `math::pcg::State`, interrupter and a staggered-stencil choice. It builds
an integer index tree, RHS/solution vectors, interior mask, sparse matrix and
incomplete-Cholesky preconditioner (falling back to Jacobi). Its result is a tree;
the wrapper must retain transform/custom metadata and explicitly inspect
`state.success`. Upstream currently constructs a result even after failure.

The bounded application API is `WritePoissonSolution` with
Float/Double active cells defining the solve domain, zero Dirichlet exterior,
uniform affine voxels, and a hard one-million-unknown ceiling. The source must
remain unchanged. Use the `staggered=true` branch for the ordinary nearest-face
seven-point Laplacian and multiply the world-space RHS by `h*h` before solving
the index-space system. Rotation/reflection preserve this isotropic operator;
nonuniform scale/shear require a different operator and must fail clearly in
this first route. Expose finite relative residual and iteration count; a failed
solve writes no output. It additionally verifies the residual after output
rounding, so a converged Double solve cannot silently promise a tighter
tolerance than its stored Float result meets. Eleven enabled feature cases and
the coordinated main ON/OFF and CLI/UI batch pass for M02.

For projection, stencil matching is essential. In this installed version,
the default collocated Poisson branch uses neighbors two voxels away, matching
central-difference divergence composed with central-difference gradient; its
coefficients omit the `1/(4*h*h)` factor. The staggered branch uses immediate
neighbors. Applying arbitrary central derivative helpers to a nearest-neighbor
pressure solution does not establish incompressibility. Boundary correction
halos and velocity layout must agree with the matrix; all-Neumann domains also
need compatibility and a pressure gauge. The current closed-domain MAC workflow
uses the matching graph operator and verifies the full stored-field residual,
including gauge cells. Broader boundary policies remain M04 work.

`PotentialFlow.h` supplies a different useful workflow:
`createPotentialFlowMask`, `createPotentialFlowNeumannVelocities`,
`computeScalarPotential` and `computePotentialFlow`. It creates an exterior
domain around a collider, solves boundary-driven Laplace potential, then derives
flow. Its approximated outer-boundary handling is documented in the upstream
code. It must not be presented as pressure projection of an arbitrary turbulent
velocity field or as a complete fluid timestep.

Required solver acceptance checks:

- Manufactured *discrete* solutions with exact zero exterior, including nonunit
  voxel size; verify both field error and an independently evaluated residual.
- Irregular/disconnected domains, zero RHS, active zero values, empty input and
  bounded tile expansion; no hidden use of inactive/background values as RHS.
- Uniform rotated/reflected maps, unsupported nonuniform maps, coordinate
  limits, finite tiny/large RHS and sum/scaling overflow.
- A deliberate insufficient iteration cap must fail without a partial output;
  an existing output and original source remain intact.
- Preflight index-tree, mask, matrix, preconditioner, Krylov vectors, output tree
  and expansion scratch before allocation. One-worker execution does not remove
  matrix/vector memory. The active-count index type is signed 32-bit upstream.
- For projection specifically, require substantial RMS/max divergence reduction
  over the promised domain, consistent wall/open-boundary flux, unchanged
  divergence-free fields and force/time/scale tests. Solver convergence alone
  does not satisfy these checks.

## Completion gate

Keep this file current after each validated batch. A row moves to implemented
only when its stated gaps have real API/workflow coverage and evidence, or is
split into smaller explicitly tracked rows without losing an outstanding
capability. Do not close simulation, full multiple scattering, point/vector
workflows or authoring UI by citing an approximation or an unavailable package.
Only an explicit user scope change can remove those requirements.

For every new workflow, record the public entry point, applicable CLI/UI route,
enabled and disabled behavior, numeric/transform/background/attribute semantics,
preflight and post-operation limits, no-overwrite/cleanup behavior, and CPU
verification. Add GPU/runtime verification when rendering or resource ownership
changes. Document optional dependency discovery/package notices and clean
configuration checks. Deterministic seeds, cancellation and job lifecycle must
be tested for long-running workflows. Keep platform validation gaps visible.

The current completion decision is **open** while any requested partial or
pending row remains. Existing approximations and backend limitations stay
documented rather than being relabeled as completion.

