# glTF scene selection

The primary asset retains one complete authoring graph and one set of decoded
geometry, materials and images. Selecting a scene changes an active-root view;
it does not reload the file or recreate source-node identities. A root shared by
several scenes has one global authoring identity. Transform, material and
imported-light edits on that identity remain shared.

The authored default is selected initially, with scene zero used when no default
is specified. `--gltf-scene N` selects a zero-based scene index at startup. The
**glTF scene** combo in Scene Controls shows names and indices, including empty
scenes. A capture-sequence event such as `{"frame": 3, "gltfScene": 1}` uses the
same switch path. A file without a scene table retains the legacy implicit view.

## Publication and editing

The catalog validates the global strict forest, scene roots and default index
before resource I/O or TinyGLTF index narrowing. Shared roots across scenes and
duplicate display names are valid. Child scene roots, repeated references in one
scene, multiple parents, disconnected cycles and invalid numeric indices fail
with context. Metadata is bounded to 65,536 scenes, one million source nodes and
one million aggregate scene-root references.

The complete CPU graph is independently bounded to 262,144 renderables and
524,288 graph nodes. A selected view must fit `AppConfig::maxSceneObjects`
(normally 4096). Planning finishes before changing the active view, so a rejected
selection retains the previous graph and edits. World transforms use an explicit
worklist, including for deep inactive trees.

Hierarchy rows, draws, deformation owners, cameras and provider extraction use
the selected view. Synthetic context ancestors stay visible in the hierarchy
without activating their unrelated children. Bounds and authored light tables
are refreshed. New primitives and children detached to the editor root retain
membership in the scene where they were authored. Undo/redo temporarily detached
tail nodes do not become roots of another scene.

A switch retires graphics and asynchronous GI readers, pauses animation at its
current pose, clears authored-camera selection and refreshes the camera choices.
It clears selection, hover, viewport drag, capture-object bindings and edit
history, then refreshes deformation/providers and resets temporal history.
Source geometry and graph identities remain resident across repeated switches.

Imported light overrides use a stable source-node plus area-dialect binding;
scene-local ordinal arrays adapt that shared state to each active table. Manual
lights remain local to their scene. Generator-setting fingerprints prevent stale
generated overrides from being restored after global settings change. GPU/editor
light tables retain authored order when publishing the same records into ECS.

Physics pauses while retaining bindings, joints, authored settings and poses.
Play/Step is refused while an attached primary body belongs to an inactive scene;
switch back or detach it before resuming. BIM physics bindings retain their
separate provider ownership. Per-scene body suspension is not implemented.

## Saving every scene

GLB and ASCII saves serialize the complete authoring graph and every source
scene, including inactive edits, names, extras and extensions. The selected scene
becomes the saved default. Materials used only by inactive nodes are converted
as well. Shared roots reuse one output identity and one composed import/editor
prefix placement, preserving glTF's global single-parent forest. Animation
correction nodes remain inside those placements.

Light export evaluates each scene from the current source pose and graph worlds;
it retains only explicit overrides and manual content between visits. This
avoids freezing an earlier animation pose in a cached evaluated light table.
Every imported binding is overlaid once, including unvisited scene range scaling
and content beyond the current GPU light-table capacity. Conflicting edits on a
shared binding fail transactionally. A manual sun replacing a source sun shared
between scenes currently receives an explicit export diagnostic; preserving that
scene-local override requires a separate source light node. Existing post-skin
or custom-material baking limits also remain explicit.

Authored scene roots keep their global parent when an asset has multiple scenes.
Their transforms and children remain editable. This prevents reparenting one
shared root from silently changing another scene's membership.

## Validation checkpoint

Issue #99 scope and acceptance are locally complete with the limits below; it
remains open pending integration. The complete tracker #121 goal continues.

The current checkpoint uses renderer
`370f33bb103096889c88f312f57c1dd2a3f7a3122ead15b8db4bd19268ed4914`.
**866 cases across 39 executables** pass with all four real Vulkan test paths
enabled and zero failures/skips. The retained XML files independently total the
same 866 cases. That run precedes a telemetry-only `historyDepth` addition; the
unit executables are unchanged and native captures verify the added field.

On that same renderer, the state and settled deformation matrices total **116
native captures / 92 checks** across both raster techniques:

- **12 captures / 12 checks** exercise a playing clip paused at its existing .5
  pose during scene changes. Authored-camera selection and recorded edit history
  clear, while the global variant and edited mesh transform remain. The final
  pose/look and animated all-scenes GLB reload match an independent baked
  reference pixel-exactly.
- **104 captures / 80 checks** exercise skin and morph owners sharing source
  geometry, repeated A/B/Both/Empty views, CLI selection and independently pruned
  scene references. All **72 positive image comparisons are pixel-exact**. Eight
  wrong-scene negative controls have mean error at least **11.4129/255**.
  Forty-eight nonempty samples prove settled submitted current/previous streams
  with zero pose delta and cleared deformation-history reset; 16 empty samples
  prove deformation cleanup. The ray matrix checks production TLAS/BLAS and DDGI
  publication in 28 nonempty samples and cleanup in eight empty samples. DDGI
  dispatches run with lighting intensity zero for deterministic image oracles;
  these are publication/execution witnesses, not visual GI-contribution tests.

All **46 distinct process logs** have zero VUID/SYNC errors. Each report verifies
the same **94-entry executable/SPIR-V manifest** before and after execution.
Evidence: `out/feature-parity/scenes-adapter-cpu-checkpoint.json`,
`scenes-adapter-tests/*.xml`, `scenes-state-validation-checkpoint.json`,
`scenes-state-runtime/results.json`, `scenes-deformation-validation-checkpoint.json`,
`scenes-deformation-settled-raster-runtime/results.json`,
`scenes-deformation-settled-ray-runtime/results.json`, and
`scenes-state-native-manifest.json`.

The prior static checkpoint retains separate provenance: renderer
`bd7bdabd9980fd586bcb8497dcdee6a8bdd52cc5688189eba3b205851c1b87d3`,
**860 cases / 39 executables** and **42 native captures / 34 checks**. It covers
authored default/CLI selection, repeated distinct/shared/empty scenes, stable
graph/source resources, active light identities, temporal resets, scene metadata,
the strict forest and every scene through GLB and ASCII reload. Positive images
are pixel-exact, all 30 logs are clean and its own 94-entry manifest is unchanged.
See `scenes-fresh-validation-checkpoint.json`, `scenes-fresh-runtime/results.json`
and `scenes-fresh-native-manifest.json`; those captures are not recounted on the
current renderer. An earlier 34-capture static checkpoint is also preserved.

Diagnostic evidence is retained and excluded from passing counts. The original
static oracle/CLI captures exposed unordered ECS light publication. The first
13 deformation captures incorrectly demanded a settled history flag while fixed
camera keys reset temporal state each tick, although pose delta was zero. A
subsequent 52-capture raster matrix had exact pixels and zero delta immediately
after switches but did not establish settled submitted history; the current
three-frame-settled matrices supersede it. Exclusion reasons are recorded in
`scenes-deformation-runtime/results.json` and
`scenes-deformation-raster-runtime/results.json`.

Capacity rejection has transactional pure-planner coverage; no native UI action
exercises that rejection. GPU picking readback has no native capture action.
Physics retains paused bindings and refuses Play/Step while a primary body is
inactive. A manual sun replacing a shared authored sun remains an explicit
transactional export error requiring separate source nodes.
