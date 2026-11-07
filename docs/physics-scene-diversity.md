# Physics integration across different scenes

`tests/validation/physics_scene_diversity_regression.py` checks twelve distinct
conditions in forward and deferred rendering. Its fixtures cover:

- An off-center glTF mesh under nested rotated and scaled parents, including
  nonuniform collider scale and reset through the parent transform.
- A mirrored parent with the same off-center geometry and preserved winding.
- A tilted ramp, downslope motion and contact with a horizontal floor.
- An asymmetric BIM L assembly and a separate tetrahedral product, with a
  global model translation, reset and replacement.
- A slider joining a primary anchor to the fragmented BIM assembly, including
  a translated model, explicit zero gravity and collider replacement.
- The original repository `taa_scene.gltf` and `taa_equivalent.bim` files,
  loaded unchanged and recorded by source SHA-256. Each receives a moving
  mechanism, with a GUI model replacement between them.
- Continuous collision detection against a five-centimeter wall. A
  twenty-centimeter body starts five meters away at 500 units/second, which
  would cross the wall entirely in one 1/60-second step without CCD.
- A glTF kinematic pusher moving through four physics ticks while an unrelated
  world joint is created. Contact must push a BIM cube rather than teleporting
  the pusher past it. Reset, endpoint detach and unload are also checked.
- A glTF kinematic pusher moving from `-2` to `+2` while its uniform scale
  changes from `1` to `1.2`. Rebuilding its collider must preserve the pending
  contact sweep and push a zero-gravity BIM cube to approximately `x = 3.1`.
- A BIM kinematic pusher receiving a root translation of `+4` and a body
  replacement with edited friction in the same frame. The replacement must
  preserve its pending sweep and push a primary glTF cube to approximately
  `x = 3`, with reset and both providers' detach/unload checked afterwards.
- A sleeping dynamic glTF support moved away from a BIM rider while paused.
  The rider must wake without advancing time, fall when playback resumes, and
  retain its fragment placement, normals and bounds. Reset, detach and unload
  also check the moved source parent and product identity.
- Three sleeping dynamic glTF boxes joined by two fixed joints, supported by
  a BIM platform that touches only the bottom box. Moving the platform away
  while paused must wake the entire chain before the next solve. The resumed
  chain must fall together; reset, middle-body detach, platform detach and
  unload check joint and body retirement.

The default matrix contains twenty-four raster cases and 185 captures.
`--include-ray-taa` adds a deferred mixed-provider mechanism and a forward
repository-model replacement, giving twenty-six cases and 203 captures.
Each case writes PNGs, effective
telemetry and a report.
No external models or downloads are needed.

```powershell
$env:CONTAINER_RUN_GPU_PHYSICS = '1'
python -B tests/validation/physics_scene_diversity_regression.py `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --output out/physics-scene-diversity --include-ray-taa --require-opt-in
```

Use `--scene ccd-thin-wall --technique deferred-raster` for a focused probe.
Use `--scene dynamic-support-teleport` for the paused dynamic support edit. It
runs at one fixed tick per playing frame and checks the paused wake separately
from subsequent falling motion.
Use `--scene contact-wake-chain` for contact waking a connected mechanism.
It also runs at one fixed tick per playing frame and checks all three links
while paused, then compares their resumed motion with analytic free fall.
Use `--scene joint-during-kinematic-motion` to check contact during joint
creation; this fixture selects four physics ticks per render frame.
The two collider rebuild probes also select four ticks per frame:

```powershell
python -B tests/validation/physics_scene_diversity_regression.py `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --output out/physics-kinematic-resize --scene kinematic-resize-contact `
  --technique deferred-raster
python -B tests/validation/physics_scene_diversity_regression.py `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --output out/physics-kinematic-replacement --scene kinematic-replacement-contact `
  --technique forward-raster
```

Add `--ccd-speed 600` to verify a requested speed above the default 500 reaches
the body without clamping. The option accepts finite float32 speeds of at least
500; the fixture records the requested velocity and checks it against telemetry.
`--check-existing` rechecks saved evidence, clearing the previous pass before
validation. Run native checks without concurrent renderer processes or builds.

The checker uses the original authored mesh vertices and hierarchy transforms.
It verifies off-center collider positions against the mesh center, mirrored
scale, independent floor heights, both fragments' actual rendered matrices,
inverse-transpose normals, and bounds recalculated from the original geometry.
Mechanism and lifecycle checks cover body/joint preservation on reset and
replacement, retirement on detach/unload, and actual capture changes during
visible motion. CCD checks the first simulation step against the known wall
face, so a body that tunnels and later returns cannot pass.
Jolt's default linear cast allows penetration of
`min(0.02, 0.25 * innerRadius)`: two centimeters for this body's ten-centimeter
inner radius. The near-face check includes exactly that allowance plus twenty
micrometers for floating-point rounding. A body center beyond `-0.10498` fails;
crossing the wall cannot satisfy the bound.

The rebuild contact cases require complete primary render matrices and compare
BIM placement, normals and bounds with the original geometry. Their nine
capture frames check the first contact, continued motion, four-tick counters,
reset and retirement. Telemetry `physics.bodies[].id` identifies the current
physical body: it changes on collider rebuild, replacement and reset, while
the scene node and BIM product/fragment membership stay stable. These
process-unique IDs are not compared across application runs.

The contact-wake chain samples nine frames covering creation, sleep, the paused
platform edit, resumed falling motion, reset, both detach operations and unload.
It requires both fixed joints to retain their IDs through reset, complete
primary render matrices, consistent world anchors, and independent BIM bounds
and normal transforms. Detaching the middle link must retire both joints.

These scenes retain the current box-collider approximation for attached mesh
products. The tetrahedron exercises imported geometry, identity and transformed
bounds; its collision shape is its bounding box. The CPU checker tests exercise
legitimate scene records and reject stale bounds, lost off-center placement,
changed mirrored transforms, joint identity loss and tunneling.

## Measured Windows validation

The contact-wake review passed all 26 Release cases and 203 captures, plus the
new chain fixture's nine Debug captures. The previous executable woke only one
of three sleeping links when the BIM platform moved away; corrected captures
wake all three while paused, then show matching free fall across the chain.
All 47 checker tests pass, including six new regressions for this fixture.
Before/after captures, telemetry and source/binary hashes are under
`out/physics-contact-review`.

The joint/contact lifecycle review passed all 24 Release cases and 185 captures,
plus the dynamic support edit's nine Debug captures. The old executable left
the BIM rider at `y = 1.50` after its glTF support moved away; the corrected
capture wakes it while paused and lets it fall to the floor at approximately
`y = 0.48` after playback resumes. The checker suite passed all 41 cases,
including five regressions for this fixture. Before/after images, telemetry,
source/binary hashes and the wider results are under
`out/physics-joint-lifecycle-review`.

The kinematic rebuild review passed all 22 Release cases and 167 captures,
including both new contact cases in forward and deferred rendering. The resize
and replacement cases also passed nine Debug captures each. Previous Release
captures left the pushed bodies at `x = 0`; the corrected Debug captures moved
them to approximately `x = 3.06` and `x = 3.0`, respectively. The checker accepts
contact-induced rotation while requiring physics, proxy and rendered geometry
to agree. Its 36 CPU checker tests pass. Evidence is under
`out/physics-transition-review`.

The previous measured review passed all 18 Release diversity cases and 131
captures, plus the joint-creation contact case's nine Debug captures. The fixture
compares the first contact frame, complete rendered matrices, independent BIM geometry,
four-tick timing and joint/reset/detach/unload state. Checker regressions reject
missed contact, missing primary render matrices and changing overlay counts.
The wider validation results are in [the physics guide](physics-integration.md#build-and-tests),
with local evidence under `out/physics-motor-review`.

Visual Studio Debug and Release application builds passed, with 175 focused C++
cases in each configuration: 103 physics (31 world, 33 scene, 16 element bindings
and 23 widgets), 41 capture-event, 14 object-transform and 17 BIM cache cases.
The four Python checker suites passed 55 cases; one optional JSON Schema case
skipped because `jsonschema` was unavailable. Four offline dependency
configuration cases also passed.

The Release diversity matrix passed all 16 cases and 113 captures. The existing
rigid-body suite passed four more cases and 16 captures, including its ray-shadow
footprint guard. A Debug BIM translation/reset/replacement probe passed nine
captures. All 138 captures completed without Vulkan validation errors. The
reports bind their evidence to the tested executable's SHA-256; PNGs, telemetry
sidecars, independent analytic checks and representative rendered images were
also inspected. Logs, per-configuration C++ XML and executable hashes, native
reports and `validation-summary.json` are under `out/physics-scene-review`.

The review fixed sleeping jointed bodies failing to wake after a static endpoint
moved, body-editor drafts retaining the previous selection's settings, and
replacement silently clamping valid existing values or discarding initial
velocities. Regression tests cover these fixes. Large single-endpoint teleports
can still leave residual joint error; see [joint placement guidance](physics-joints.md).

## Further integration review

The next review added ten C++ regressions and passed 197 focused cases per
Visual Studio configuration, including Debug assertions: 33 world, 35 scene,
19 element bindings, 23 widgets, 41 capture-event, 14 object-transform, 17 BIM
cache and 15 hierarchy/reload cases. The four Python checker suites passed 60
cases; the optional JSON Schema check still skipped. Four offline dependency
configuration cases and disabled-backend frontend/application compilation
also passed.

Fixes cover transactional Reset when backend collider or joint validation fails,
safe creation and editing of overspeed velocities, capacity-budget validation,
rejecting unpublishable BIM fragment models, clearing stale auxiliary records
after graph replacement, and retiring physics after a failed model commit
rebuilds the previous graph. Reload regressions exercise real bodies/joints
with simulated preparation failure, rollback and reused graph indices.

The Release matrix passed all 16 cases and 113 captures again. Debug BIM
translation/reset/replacement passed nine more captures, and the Debug CCD
probe with `--ccd-speed 600` passed six. Its telemetry retains the requested
velocity, while the body is safely limited by Jolt and stopped at the thin wall.
All 128 captures completed without Vulkan validation errors. Build logs, test
XML, executable hashes and reports are under `out/physics-review-followup`;
`validation-summary.json` binds the results to the tested binaries and sources.
