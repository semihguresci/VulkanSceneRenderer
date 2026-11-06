# Kinematic supports across model providers

`tests/validation/physics_moving_support_regression.py` exercises contact between
glTF and BIM physics bodies in four native cases: each platform provider runs in
forward and deferred rendering. A glTF kinematic platform carries a fragmented
BIM dynamic rider, then a BIM kinematic platform carries a glTF dynamic rider.
The BIM product contains two adjacent mesh fragments sharing one product GUID.

Each 190-frame sequence lets the rider settle, raises the platform 1.5 meters
over one second, holds it, clears the physics bindings and unloads both model
providers. Eight PNG and telemetry captures cover each lifecycle. The BIM lift
uses the existing global `bimTranslation` capture track and contains only
kinematic auxiliary bodies; its dynamic rider belongs to the primary provider,
so authoring that movement must keep simulation playing. The glTF lift uses an
`objectTranslation` track targeting the imported platform's source parent;
the body remains attached to its renderable primitive. Moving the source parent
keeps the world lift independent of the platform's half-height scale.
The capture selects that parent by its imported name as well as its node index,
so unload must suspend name resolution when the old target no longer exists.

```powershell
$env:CONTAINER_RUN_GPU_PHYSICS = '1'
python -B tests/validation/physics_moving_support_regression.py `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --output out/physics-moving-supports --require-opt-in
```

Use `--provider bim --technique deferred-raster` for a focused probe. The
provider names identify the kinematic platform. `--check-existing` revalidates
saved reports and removes old pass results before checking them. Run native
captures without concurrent builds or other renderer processes.

The checker verifies the authored lift height independently of telemetry,
contact at the known platform top and rider half-height, the dynamic rider's
1.5-meter rise, continuous playback and fixed-step progress. Geometry checks
derive BIM bounds and inverse-transpose normals from the original authored
fragment vertices and compare actual rendered models against proxy transforms.
Primary rendered models must agree with their physical mesh centers. Semantic
product identity, fragment membership and imported geometry counts must remain
stable through clear; unload must remove both bodies and geometry. Real PNG
hashes must change between the settled and elevated poses.

An explicit capture `guiUnload` suspends primary object-name lookup, translation
and visibility tracks even though keyframes continue to be sampled. A later
`reload`, `guiReload` or `guiSampleModel` event reactivates them and reacquires
the authored transform from the new target. Invalid names and node indices in
an active track remain errors. The glTF lift's animated unload is the native
regression for this lifecycle behavior.

The suite uses the current box-collider approximation. Its contact checks are
cross-provider so translating a model cannot move both the support and rider
without physics. Reset while a capture translation track continues to author
the platform is intentionally covered separately by the reset/parent-placement
tests; this suite covers lift, hold, clear and unload.

CPU checks in `test_physics_moving_support_validation.py` cover valid records,
paused playback, missing contact motion, stale rendered transforms/bounds/
normals, identity changes, incomplete matrices and retained lifecycle state.
Native GPU execution is opt-in and binds its report to the executable SHA-256.

## Measured Windows validation

Visual Studio Debug and Release application builds passed with 203 focused C++
cases in each configuration: 35 world, 37 scene, 21 auxiliary bindings, 23
widgets, 41 capture-event, 14 object-transform, 17 BIM cache and 15
hierarchy/reload cases. Six new C++ regressions cover batched kinematic contact
motion, pending paused edits on Single step, atomic kinematic-target rejection
and obsolete-world retirement after Reset or resizing changes body handles.
Five Python checker suites passed 77 cases; one optional JSON Schema case
skipped because `jsonschema` was unavailable. Four offline dependency cases
also passed.

The Release moving-support matrix passed all four cases and 32 captures. The
existing scene-diversity matrix passed all 16 cases and 113 captures, including
ray shadows and TAA. Both moving-support arrangements also passed in Debug
deferred rendering, producing 16 more captures. All 161 captures completed
without Vulkan validation errors. The glTF lift uses a named animation target
so its successful unload exercises suspension of a genuinely retired target.
Representative images show the riders remaining in contact with their elevated
platforms; telemetry sidecars match the recorded reports.

Build logs, per-configuration test XML and executable hashes, pre-fix failures,
native evidence and `validation-summary.json` are under
`out/physics-motion-review`. The summary binds each native run to its tested
binary and records the relevant source hashes.
