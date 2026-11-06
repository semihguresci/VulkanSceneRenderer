# CPU rigid-body physics

The renderer integrates [Jolt Physics 5.6.0](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.6.0)
for CPU rigid-body simulation. It is statically linked, MIT licensed and built
by default. Rendering and physics share the right-handed, +Y-up world described
in [the coordinate conventions](coordinate-conventions.md). Gravity defaults
to `(0, -9.81, 0)`; imported geometry must use an appropriate scene scale for
meter-based simulation.

## Try the demo

From the directory containing a current build:

```powershell
.\VulkanSceneRenderer.exe --physics-demo
```

The demo adds a static floor and three dynamic boxes beside the loaded scene,
starts simulation, and frames the group unless an explicit camera was supplied.
Use **Scene Controls → Physics → Reset** to restore its initial transforms and
velocities, then **Play** to replay it. The same group can be created with
**Create demo** in the panel. After **Clear bodies**, the demo action reuses its
existing geometry. Ordinary scenes acquire bodies only through an
explicit attachment or demo request.

To use existing geometry:

1. Select a mesh in the primary scene hierarchy, or a BIM/USD element in the
   viewport or **BIM Elements** table.
2. Expand **Physics** and choose **Static**, **Kinematic** or **Dynamic**.
3. Set mass, friction and restitution, then choose **Attach body**.
4. Choose **Play**, **Pause** or **Single step**. Single step pauses and advances
   exactly one simulation tick. **Reset** restores attachment-time transforms
   and initial velocities and pauses playback.

Static bodies provide fixed scenery. Kinematic bodies follow authored node
transforms and can move dynamic bodies through contacts. Dynamic bodies receive
gravity and collision response; their simulated poses update scene transforms.
At the low-level API, prescribed kinematic linear and angular velocities remain
active until explicitly stopped, including commands below the ordinary sleep
threshold. Connected dynamic joint links participate in each solve, while
unrelated sleeping mechanisms stay asleep. Scene-authored kinematic targets
still stop through the next target command once their motion is complete.
Jolt's per-step angular integration threshold still applies to tiny rotations.
Moving a dynamic body through the editor wakes nearby bodies at its old and new
positions, so removing a sleeping stack's support releases its rider. The
low-level `setPose(..., activate=false)` call retains its explicit inactive-edit
policy for dynamic bodies.
Contact-neighborhood wakes include every connected dynamic joint link before
the next solve, even when only the bottom link touches an edited support.
Stationary kinematic neighbors remain traversal boundaries for unrelated
mechanisms.
When one render frame requires several fixed simulation ticks, a kinematic
target is traversed over those ticks. It is not applied as a one-tick jump,
which could miss contacts at lower rendering frame rates. Single step also
honors pending paused edits to a dynamic body's position and rotation before
advancing its simulation.
Small authored rotations, including `0.01` and `0.02` degrees, synchronize with
static and paused dynamic colliders and survive Play or Single step. Pose
comparison uses quaternion component distance and treats `q` and `-q` as the
same orientation.
Editing a dynamic body's transform during playback pauses simulation so the
authored edit can be applied. **Replace body** validates the replacement before
retiring the old collider and preserves the original authored reset transform.
An unchanged transform, including a gizmo movement rounded away by snapping,
keeps playback and its accumulated partial simulation tick intact. It does not
publish scene transforms or trigger object and lighting refreshes.
Viewport, scene-root and selected-object transform controls validate proposed
physics transforms before pausing or publishing an edit. Unsupported shear,
singular transforms and invalid reset/joint frames leave the rendered geometry,
collider and playback state unchanged. Visual-only branches retain their usual
transform behavior.
Attached BIM/USD entries also preflight their fragment publication: matrix
conditioning, winding, normals, bounds and object-buffer readiness use the same
validation as the renderer's actual transform update, before pausing playback or
resizing a collider.
Hierarchy groups and attached BIM/USD product entries expose **Node Transform**
controls even when the scene contains no renderable primary meshes. Mesh
selection waits for the selected node's current transform snapshot before
showing its numeric editor.
Transform preflight and dynamic-body detection walk attached bodies' ancestry
instead of every visual descendant. Scene transform publication and rendering
retain their existing dependence on scene size.
**Detach body** and **Clear bodies** restore the attachment-time transforms and
remove the corresponding physics bindings.

During playback, resizing a kinematic collider or replacing it with another
kinematic body retains its current physical origin and rotation. The next
advancing tick sweeps it toward the authored target, including when several
fixed ticks run in one frame. Paused replacements apply their authored pose
immediately. Single step preserves the sweep for automatic scale rebuilds.

Selecting an existing body loads its own motion, mass, material, gravity and
CCD settings into the editor. Unsaved edits remain while that body stays
selected. Returning to a body after selecting another object reloads its
attached settings, so replacing a floor cannot inherit a dynamic body's draft.

BIM, IFC/IFCX and USD elements use the same physics world as primary scene
meshes. All fragments sharing the selected product's identity attach to one
body and retain their relative placements, materials and semantic identity.
Select any fragment to replace or detach that product's body. The named product
entry in the scene hierarchy exposes its transform for authored kinematic
motion; editing a dynamic entry pauses playback as it does for primary meshes.
These entries retain their internal parent; reparenting them or parenting other
scene nodes beneath them is rejected. Product identity follows the imported
catalog's GUID/source-ID precedence, so a shared source ID alone cannot merge
otherwise distinct products.
Moving an entry in the transform inspector preserves its existing local basis,
including compensation for a rotated, nonuniformly scaled parent. Explicit
rotation and scale edits continue to use the inspector's TRS controls.
Colliders are explicit: attach static bodies to the floors or other scenery
that should support moving elements.

## Current scope

The scene panel attaches oriented box colliders derived from each selected
mesh's local vertex bounds. Very thin dimensions receive a minimum half extent
of `0.005` scene units. These colliders approximate the mesh; they do not follow
concave details or holes. Mass is editable for dynamic bodies; friction and
restitution are shared surface parameters.

Dynamic bodies also expose **Gravity factor** (`0` disables gravity; `1` uses
world gravity) and **Continuous collision detection**. CCD sweeps fast linear
motion against obstacles; it does not guarantee detection of every collision
caused by rapid rotation. These are attachment/replacement settings. Reset
retains the most recently attached body settings and restores its authored pose.
Dynamic attachment and replacement reject a gravity factor when its resulting
float acceleration or squared magnitude is unrepresentable. Negative factors,
zero gravity and large representable values remain supported; static/kinematic
bodies do not evaluate their unused gravity factors.

Reset builds and validates all replacement bodies and joints before retiring the
live simulation. A rejected collider or joint frame retains the current poses,
body handles, joint identities and playback state. This also works when every
configured body slot is occupied.

Initial velocities and velocity setters use Jolt's speed limits: 500 scene
units/second for translation and `0.25 * pi * 60` radians/second for rotation.
Larger finite vectors are shortened while retaining direction. Settings
snapshots and capture telemetry retain the requested initial vectors; the live
body uses the bounded values. Nonfinite inputs remain errors.
Target-based `moveKinematic` can exceed these speed caps, but rejects a target
or duration whose required linear/angular velocity calculations overflow.
Rejection preserves the body's pose, velocity and sleep state.

The **Joints** panel supports fixed, hinge and slider mechanisms between attached
bodies, including primary meshes and BIM/USD products. A body may also attach
to the fixed world. Movable joints support limits and velocity/position motors.
See [Physics joints](physics-joints.md) for the UI workflow, frame conventions,
collision filtering and capture tests.

The lower-level `PhysicsWorld` API also supports spheres, Y-axis capsules and
static indexed triangle meshes. Those shapes are covered by CPU tests but are
not collider choices in the current panel. Triangle meshes retain their own
geometry and use counterclockwise, single-sided triangles for simulation.

World capacity is checked against both Jolt's body-ID range and the derived
collision budget before allocation. The current double-precision Windows build
supports at most 2,236,962 configured bodies with four contact slots per body;
the ordinary default remains 16,384. Oversized configurations throw an error
instead of asserting in Debug or silently reducing the budget in Release.

For BIM/USD products the collider is the aggregate world-space box at attachment
time. It includes empty space between fragments and approximates concave shapes
and openings. The entire product moves rigidly; it does not acquire independent
bodies for its individual fragments. Existing imported units and scales apply.
Noninvertible or projective fragment matrices are rejected before attachment or
replacement, so an unsupported rendered pose cannot leave a live physics body
that fails its first GPU publication.
The integration does not provide a collision character controller, BIM
navigation, vehicles, soft bodies or saved physics scene assets. Joint families
beyond fixed, hinge and slider are not exposed.

Runtime element poses update GPU instance matrices, normal transforms, culling
spheres, picking/focus bounds, section caps and coordination bounds. Only changed
object slots are uploaded; imported text, geometry and search publications stay
unchanged. Provider bounds conservatively contain both original and moved
objects. Imported schedules, semantic relationships and authored floor-plan
overlays retain their source placements. Runtime simulation does not rewrite
the imported file. A global BIM model translation rebases physics reset poses
and pauses dynamic playback while preserving velocity.

Transform bindings preserve collider center offsets, rotation and signed scale
through scene parents. Scale edits rebuild the collider while retaining its
current velocity. Singular or sheared transforms and mixed dynamic/non-dynamic
ancestor bindings are rejected with a panel error. Replacing or unloading a
scene retires old bindings before reused node indices can affect another model.
A failed load that rebuilds the previous model also retires those bindings:
restored graph nodes do not restore runtime bodies or joints. Preparation
failures that leave the existing graph intact preserve its simulation state.
Publication synchronization also retires an obsolete auxiliary world after
reset or resizing has changed its body handles, while preserving bodies already
attached to a new graph at reused node indices.

The [scene diversity suite](physics-scene-diversity.md) exercises nested and
mirrored glTF transforms, ramps, asymmetric BIM products, mixed-provider
mechanisms, unchanged repository assets and fast-body CCD. It checks rendered
geometry and lifecycle behavior in the running application.
The [moving-support suite](physics-moving-supports.md) checks contact between a
kinematic platform and a dynamic rider from different glTF/BIM providers,
including lift, hold, clear and unload in both render techniques.
The [scale suite](physics-scale-validation.md) checks growing static colliders
against sleeping BIM bodies and small source-unit scale edits. Static collider
insertion wakes dynamic bodies in its new bounds as well as retiring the old
collider's contacts. Scale comparisons use relative tolerances so source units
do not hide valid collider edits.

Hierarchy edits are checked before changing the graph, including the stored
authored transforms used by Reset. Unsupported edits leave playback and body
and joint state intact. Move/Rotate preserve imported reflection signs and scale
magnitudes. BIM/USD proxy inspector edits reject reflection changes before
mutation because their existing draw categories require stable triangle winding.

## Timing and ownership

`PhysicsScene` accumulates application time at a fixed `1/60` second tick, with
at most eight substeps per update. It accepts at most `0.25` seconds from one
frame and drops excess whole ticks after the substep limit. Paused time is not
replayed on resume. Capture runs use the existing deterministic application
timestep rather than the ImGui frame interval.

Play accepts pending paused dynamic transforms on its first successful physics
tick, including when several short render frames precede that tick. Later ticks
integrate the retained velocity. Detaching another body preserves pending edits
and compensates nested dynamic children when an ancestor's transform is restored.
Kinematic targets continue to travel through contacts. The
[pending-edit suite](physics-pending-edits.md) exercises these transitions using
primary glTF bodies and unrelated BIM attachments.

Demo recreation tracks the nodes it created rather than hierarchy row order.
Reordering those children preserves their floor/box roles. If membership changes,
a new demo is placed clear of existing primary geometry, leaving the edited group
and any imported meshes intact.

### Rejected operations

Authored synchronization checks every binding's transform, collider pose and
derived extents before moving or resizing the first body. Detach prepares the
remaining bodies' local transforms and the target's restored world transform
before retiring its body or joints. Invalid joint settings and unsupported
endpoint motion pairs are rejected before pending scene edits are synchronized.
These checks preserve live bindings when those validations fail; allocation,
backend shape creation and solver failures have separate failure behavior.

Nonstatic body creation checks Jolt's generated inverse mass and inverse inertia
before publishing a body. Position motors also require representable float
frequency-derived spring coefficients. This rejects the reproduced tiny-mass
and tiny-frequency crashes and overflowing coefficient inputs. Off and Velocity
motors retain unused finite spring drafts. These guards cover coefficient
generation and inertia inversion; arbitrary extreme combinations of effective
mass and timestep are not covered by that guarantee.

Unlimited hinge position-motor targets are reduced to an angle within `[-pi, pi]`
before reaching Jolt. This avoids its iterative angle wrapping stalling on large
finite values. Limited hinge targets continue to clamp to their configured
interval; slider targets retain their linear displacement.

`PhysicsWorld` owns the Jolt system, shapes and body handles without Vulkan
types in its public interface. Each public body handle is a process-unique
64-bit value; stale handles remain invalid after destruction, clear or use in
another world. Jolt registration and factory lifetime are shared safely across
multiple worlds, and each world finishes its worker jobs before a step returns.
Positions retain double precision while shape geometry and scene matrices use
floats. The default worker count uses at most four background threads.

Bindings use graph publication identity and stable node indices. The renderer's
EnTT entities remain a derived render mirror, so rebuilding that mirror cannot
invalidate body identity. Physics writes CPU scene transforms; existing frame
fences still govern GPU object-buffer uploads and the renderer's motion history.

Capture telemetry includes a `physics` object with availability, initialization,
playback state, body/active-body counts, completed steps, substeps, dropped time
and the current body poses when a physics world exists.

## Build and tests

Use a Visual Studio Developer Command Prompt and the existing configured build:

```powershell
cmake -S . -B out/build/visual-studio -DENABLE_PHYSICS=ON
cmake --build out/build/visual-studio --config Release --target VulkanSceneRenderer physics_world_tests physics_scene_tests physics_controls_widget_tests physics_element_bindings_tests bim_object_transform_tests temporal_capture_tests
ctest --test-dir out/build/visual-studio -C Release -R "^(physics_.*|bim_object_transform_tests|temporal_capture_tests)$" --output-on-failure
```

See [Build and Test](build-and-test.md) for the initial Visual Studio generator,
Vulkan SDK and vcpkg setup. Use `-DENABLE_PHYSICS=OFF` to omit Jolt and its runtime
implementation. The Physics panel remains present with disabled actions and an
explanation, and `--physics-demo` reports that the backend is unavailable.

The dependency is pinned to commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a` and a verified archive SHA-256 in
`cmake/PhysicsDependencies.cmake`. An initial enabled configuration downloads
that archive. For an offline source tree, configure with
`-DFETCHCONTENT_SOURCE_DIR_JOLT=C:/Dependencies/JoltPhysics`. An explicitly
disabled build performs no Jolt fetch. Jolt uses the project's shared MSVC C
runtime, double positions and an x86 SSE2 baseline; its sample applications,
GPU backends, profiling and object serialization are excluded.

The registered tests cover:

- Falling/resting contacts, sphere/capsule/static-mesh shapes, kinematics,
  quaternion/position conversion, invalid inputs, body capacity and stale IDs.
- Repeated and concurrent independent world lifetimes.
- Fixed-step partitioning, pause/reset, transformed and mirrored parents,
  collider resizing, nested bindings and graph replacement.
- Paused edits of sleeping dynamic bodies and kinematic supports, including
  wakeup near the old/new support bounds, unchanged-pose sleep and unaffected
  distant bodies. Existing moving-body edit tests preserve both velocities.
- Actual ImGui button, motion-choice and parameter interactions, including
  disabled builds and missing callbacks.
- Auxiliary product grouping, fragment ownership, cross-provider collisions,
  root rebasing, replacement failure, safe publication retirement and bounded
  reuse of the nonrenderable scene entries that represent physics bodies.
- Four offline dependency configuration cases: disabled backend, supplied local
  source, missing upstream target and missing license. These use local fixtures
  and require no Jolt download or compiler.

CPU tests do not need a Vulkan device or display. Their executables follow the
project's shared test output directory, so run Release tests before a Debug build
overwrites those binaries.

The complete four-case native regression is opt-in and requires the renderer,
a display, a Vulkan runtime with ray-query support, and the Python image-analysis
dependencies used by the model validation harness. CPU physics tests do not
require ray-query support:

```powershell
$env:CONTAINER_RUN_GPU_PHYSICS = '1'
ctest --test-dir out/build/visual-studio -C Release -R "^physics_runtime_regression$" --output-on-failure
```

It checks actual demo motion, floor contact, rendered captures and unload across
forward/deferred techniques with raster and ray-query/TAA cases. Without the
environment opt-in it reports an explicit skip. Artifacts are written beneath
`test_results/physics-runtime-Release` in the build directory. Run without
concurrent renderer processes or builds.

The separate BIM/USD regression generates a glTF floor and fragmented DotBIM
and USD products, so it requires no downloaded models:

```powershell
$env:CONTAINER_RUN_GPU_PHYSICS = '1'
ctest --test-dir out/build/visual-studio -C Release -R "^physics_auxiliary_regression$" --output-on-failure
```

Its eight cases cover both import formats, forward/deferred rendering and
raster/ray-query with TAA. Each case checks nine captures for whole-product
motion, collision with a primary-scene floor, unchanged unrelated products,
rendered matrices, normal transforms, bounds and semantic identity. Reset,
detach, model reload and unload must preserve or retire the appropriate bodies.
Results are written beneath `test_results/physics-auxiliary-Release`.
The runner's `--source-id-collision --format bim` option adds a focused check
where GUID-less fragments and a separate GUID-bearing product share a source
ID. They must retain separate bodies and move independently.

The latest robustness review fixed prescribed kinematic velocities being erased
when slow bodies went to sleep. A sparse driver registry now keeps commanded
motion and connected dynamic mechanisms active until an explicit stop, without
scanning the static body population each tick. BIM/USD transform edits also
preflight the renderer's exact fragment publication policy before changing the
graph, collider or playback state. Seven new C++ regressions cover the two
fixes, driver lifecycle, fragment preview and accepted/rejected editor batches.

Visual Studio Debug and Release application builds and all 298 selected C++
cases passed in each configuration. All 139 applicable Python checker cases
passed, with one optional JSON Schema skip.

Native validation passed 45 cases and 398 captures across glTF/BIM scene
diversity, fixed/hinge/slider joints and moving supports, including the Debug
chain fixture. No capture recorded a physics or Vulkan validation error.
`out/physics-robustness-audit/validation-summary.json` verifies the source,
application, test executable, CPU probe and capture hashes for this run.

The saved baseline reproduces both defects. In the ten-second CPU motion
controls, a `0.01`-unit/s command previously stopped at about `0.005` units;
the fixed body reaches `0.1` units and matches the explicitly awakened control.
Angular commands and attached followers receive the same checks. A rotated BIM
fragment scaled by `[1000, 0.001, 0.001]` previously resized its collider before
render publication failed. It now rejects before the edit and retains the body,
graph, rendered model and accumulated playback time. A moderate scale control
continues to publish successfully. These are CPU public-API probes; source and
library snapshots, test XML and executable hashes are under
`out/physics-robustness-audit`.

The preceding motor/editor review fixed slow hinge and slider drives falling asleep
before reaching their goals, and limited hinge position motors choosing a
shortcut blocked by a hard stop. Powered mechanisms now retain progress while
stopped, unpowered and settled drives can sleep. Full-revolution hinge settings
retain Jolt's periodic behavior. Unchanged editor transforms preserve playback
and accumulated partial ticks. The mesh picker clips its rows while retaining
original node IDs and selection snapshots. Eight new C++ regression cases cover
these paths, including real ImGui selection interactions.

Visual Studio Debug and Release application builds and all 291 selected C++
cases passed in each configuration. All 139 applicable Python checker cases
passed, with one optional JSON Schema skip.

Native validation passed 45 cases and 398 captures across glTF/BIM scene
diversity, fixed/hinge/slider joints and moving supports, including the Debug
chain fixture. No capture recorded a physics or Vulkan validation error. The
verified `out/physics-transition-audit/validation-summary.json` binds test XML,
probe results, source manifests, application hashes and capture sidecars.

Saved-library CPU probes reproduce the motor and no-op editor defects. Corrected
slow motors match fully awakened controls; the limited hinge reaches its
`-3` radian goal with approximately `5.7e-6` radians residual instead of remaining
at the opposite hard stop. Full-range controls agree in both directions. The
Release mesh-picker benchmark reduces the million-entry warmed open-widget
median from `235.34 ms` to `7.1 microseconds`; scene construction, popup-opening
focus lookup and rendering are excluded. Build-time source manifests, binary
hashes and probe evidence are under `out/physics-transition-audit`.

The preceding edge-case review fixed newly inserted kinematic and dynamic
colliders waking sleeping joint mechanisms after the first constraint
snapshot. All collider motion types now wake overlapping dynamic components
before the first solve. Numeric transform controls also support nonrenderable
BIM/USD product and group nodes; changing the mesh picker waits for a fresh
transform snapshot before exposing the editor. Seven new C++ cases cover these
fixes and sparse hierarchy queries.

Visual Studio Debug and Release application builds and all 283 selected C++
cases passed in each configuration. All 139 applicable Python checker cases
passed, with one optional JSON Schema skip. Native validation passed 45 cases
and 398 captures across scene diversity, fixed/hinge/slider mechanisms and
moving supports, including the Debug chain fixture. No capture recorded a
physics or Vulkan validation error. The verified `validation-summary.json`
binds these results and the benchmark to source and binary hashes.

The saved library reproduces the insertion defect, and the unchanged-behavior
extracted UI widget reproduces
the missing inspectors and stale selection snapshot. Corrected insertion
probes match fully awakened control mechanisms on their first tick.

The Release transform-preflight benchmark uses 100,000 and 1,000,000 visual
descendants with one attached body inside or outside the edited group. Across
32 measured calls per case, the million-node median fell from `14.4–18.6 ms`
to `0.3–1.7 microseconds`. Preflight and the pause decision now inspect binding
ancestry rather than every visual descendant. These timings measure validation
only, not complete editor frames, scene publication or rendering. Evidence is
under `out/physics-edge-review`.

The preceding detailed review fixed editor transforms being published before
physics could reject unsupported shear. All three editor transform paths now
share an exact camera preview and validate affected live/reset collider and
joint frames before changing graph data or playback. World-anchored fixed,
hinge and slider joints also retain centimeter corrections at georeferenced
coordinates through internal static endpoints that consume no configured body
slots. Dynamic body creation rejects unrepresentable derived gravity
acceleration before publication. Ten new C++ regressions cover these fixes,
valid controls, canonical frame preservation and anchor cleanup.

Visual Studio Debug and Release application builds and all 276 selected C++
cases passed in each configuration. All 139 applicable Python checker cases
passed, with one optional JSON Schema skip. Native validation passed 45 cases
and 398 captures covering the full scene-diversity, fixed/hinge/slider and
moving-support matrices, plus the Debug chain fixture. No capture recorded a
physics or Vulkan validation error. The verified `validation-summary.json`
binds these results to the current source and application hashes.

The saved baseline reproduces all three defects: a render/collider split after
rejected edits, centimeter joint error at large coordinates, and a Jolt
assertion for overflowing gravity.
Corrected CPU probes reduce the joint residual to approximately `1.54e-8` scene
units and reject the unsafe gravity factor before creating a body. These
precision and extreme-input checks run at the CPU API level; scene/render
matrices retain their existing float precision. Evidence is under
`out/physics-detailed-review`.

The preceding contact review fixed contact-neighborhood wakes leaving distant
dynamic joint links asleep during their first solve. Static support insertion, removal
and teleport now collect nearby dynamic bodies and traverse their complete
joint components before activation. The traversal uses reserved world storage;
stationary kinematic hits do not wake unrelated branches. A second fix retains
small authored rotations that a rounded float quaternion dot product had
discarded. Four new C++ regressions cover these fixes, including Play and
Single step, and six checker regressions cover the new glTF/BIM chain fixture.

Visual Studio Debug and Release application builds and all 266 selected C++
cases passed in each configuration. Native validation passed 45 cases and 398
captures: the full scene-diversity and fixed/hinge/slider matrices, moving
supports, and the new chain fixture in Debug. All captures were free of physics
and Vulkan validation errors. All 139 applicable Python checker cases passed,
with one optional JSON Schema skip. Before-fix tests reproduce both defects;
the old executable wakes only one chain link, while corrected captures wake
all three before playback resumes and show their matching fall. Source/binary
manifests, test XML and the verified `validation-summary.json` are under
`out/physics-contact-review`. The small-rotation probe and failing scene-test
evidence are preserved under `out/physics-rotation-review`.

The preceding lifecycle review fixed joint creation/removal leaving distant dynamic links
asleep during their first solve, and activating dynamic teleports leaving
sleeping riders floating after their support moved away. Joint traversal now
uses reserved world storage, including in removal paths. Five new C++ cases
cover creation, removal, cycles, body-slot reuse, old/new contact neighborhoods,
and the paused Move-to-Play/Single-Step workflow. Five checker regressions cover
the new mixed glTF/BIM runtime fixture.

Visual Studio Debug and Release application builds and all 262 selected C++
cases passed in each configuration. Native validation passed 43 cases and 380
captures: the complete scene-diversity and fixed/hinge/slider matrices, moving
supports, and the new support-teleport case in Debug. All captures were free of
physics and Vulkan validation errors. All 133 applicable Python checker cases
passed, with one optional JSON Schema skip. The old executable reproduced the
floating rider; corrected captures show its paused wake and subsequent fall.
Source/binary manifests, test XML, before/after evidence and the verified
`validation-summary.json` are under `out/physics-joint-lifecycle-review`.

The preceding velocity review fixed the low-level kinematic linear/angular velocity setters:
starting a sleeping driver now wakes all connected dynamic links before the
first constraint solve. A new CPU regression compares both velocity starts
against explicitly awakened control mechanisms and verifies an unrelated body
remains asleep. Zero and unchanged velocity assignments retain their behavior.

Visual Studio Debug and Release application builds and all 257 selected C++
cases passed in each configuration. Focused native checks passed eight cases
and 76 captures: BIM hinges in forward/deferred raster and ray-query/TAA,
plus glTF/BIM moving supports in both raster techniques. These runtime checks
exercise the existing scene joint/contact paths; the direct velocity-setter
regression runs at the CPU API level. All captures were free of physics and
Vulkan validation errors. All 128 applicable Python checker cases passed,
with one optional JSON Schema skip. Source/binary hashes, test XML, the original
failing regression and the verified summary are under
`out/physics-topology-review`.

The preceding rebuild review added eleven C++ regressions and nine checker cases. Kinematic
scale rebuilds and replacements preserve pending collision motion, explicit
actuation wakes every connected dynamic joint link before its first solve, and
inspector moves preserve compensated BIM proxy bases under edited parents.
New glTF/BIM captures reproduce both missed contacts in the previous executable
and verify the corrected physics, rendered matrices and body identities through
reset, detach and unload. The checker also covers contact-induced rotation.

Visual Studio Debug and Release application builds and all 256 selected C++
cases passed in each configuration. The runtime suites passed 50 cases and 441
captures overall, covering scene diversity, fixed/hinge/slider mechanisms,
moving supports, collider scale changes and pending edits. All captures were
free of physics and Vulkan validation errors. All 128 applicable Python checker
cases passed; one optional JSON Schema case skipped because `jsonschema` was
unavailable. Disabled-backend frontend, application and camera integration
objects compiled; a complete disabled executable was not built. Build-time
source manifests, binary hashes, capture sidecars and the verified
`validation-summary.json` are under `out/physics-transition-review`.

The previous motor review added seven C++ regressions and eight checker cases. Existing
motor updates no longer depend on new-joint geometry drafts or hidden fields
from another motor mode. Creating a joint during playback preserves pending
kinematic contact motion. A mixed glTF/BIM capture reproduced the old missed
collision and verified the corrected pushed-body position, from `x=0` to `x=3`.

Visual Studio Debug and Release application builds and all 245 selected C++ cases
passed in each configuration. The runtime suites passed 41 cases and 366
captures overall, including the complete fixed/hinge/slider matrix, scene
diversity, moving supports and pending edits. There were no physics or Vulkan
validation errors. All 119 applicable Python checker cases passed; one optional
JSON Schema case skipped because `jsonschema` was unavailable. Build-time source
manifests, binary hashes, captures and `validation-summary.json` are under
`out/physics-motor-review`.

The earlier numeric and scene-validation review passed Visual Studio Debug and
Release application builds and 238 selected C++ cases in each configuration,
including 41 world/joint
and 54 scene cases. Ten new regression cases cover inverse-inertia and spring
validation, large hinge targets, rejected authored batches, detach publication
and invalid joint requests. The Release runtime suites and two Debug pending-edit
cases passed 40 cases and 317 captures overall, including glTF/BIM hinges,
cross-provider contacts, moving supports, scale edits, scene replacement and
unload. All captures passed without physics or Vulkan validation errors.

Four offline dependency cases and 111 Python checker cases also passed; one
optional JSON Schema case skipped because `jsonschema` was unavailable. Disabled
frontend and application objects compiled; a complete disabled executable was
not built. Build-time source manifests, executable hashes, capture sidecars and
the verified summary are under `out/physics-boundary-review`.

Initial Windows/Visual Studio validation passed all 49 CPU tests in both Debug
and Release (18 world, 25 scene, six widgets). Four offline dependency cases and
ten Python image/report checker tests also passed. An actual disconnected
`ENABLE_PHYSICS=OFF` configuration, compilation of the disabled
frontend and application objects, and all six widget tests passed. A complete
disabled executable was not built or launched.

The Release native suite passed all four cases and 16 captures with Vulkan
synchronization validation, matching physics/scene/render matrices, gravity and
contact checks, 180 fixed steps and complete unload. Visual inspection confirmed
settled boxes and localized contact shadows. Its image guard excludes projected
box/contact-shadow bounds before comparing the remaining lit floor: the
ray/TAA precision defect darkened 3,021 of 6,048 remote floor pixels (49.95%);
both corrected ray/TAA techniques darkened zero, below the 5% limit. Captures and
reports are under `out/physics-native-validation`, with the original failure
retained under `out/physics-native-validation-before-fix`.

After the sleep/wake and GUI lighting fixes, both Visual Studio application
builds and all 49 CPU tests passed again, and the four-case native suite passed
all 16 captures with clean synchronization validation. The actual **Create
demo** button was checked in the running application: its directional intensity
retains `2.0` through GUI writeback, while an explicit
`--directional-intensity 0` remains `0.0`. This UI check is manual; the native
capture suite starts the demo through the command line. The new build logs,
per-configuration test XML/executable hashes and native results are under
`out/physics-review-fixes`.

The BIM/USD attachment update passed Visual Studio Debug and Release application
builds and 132 selected C++ cases in each configuration: 64 physics, 14 runtime
element transforms, 29 capture-event, 17 BIM cache and eight depth-copy cases.
Four offline dependency cases and 29 Python checker cases passed; one optional
JSON Schema case skipped because `jsonschema` was unavailable. The Release
auxiliary suite passed all eight cases and 72 captures with clean synchronization
validation. A separate shared-source-ID regression passed nine more captures.
Manual UI checks covered viewport selection, whole-product attachment, playback,
reset, detach and switching selection to the physics demo. Viewport selection
was repeated in forward/deferred rendering after correcting packed depth/stencil
readback barriers, with no Vulkan validation errors. Disabled-backend integration
objects also compiled. Build logs,
per-configuration test XML/executable hashes and native results are under
`out/physics-auxiliary-validation`.

## Windows packages

Enabled builds stage the upstream MIT notice at
`third_party_licenses/Jolt-MIT.txt`. The Windows packaging script reads the build
cache, requires this notice when physics is enabled and copies it to
`THIRD_PARTY_LICENSES/Jolt-MIT.txt` in the archive. Jolt is statically linked;
there is no extra physics DLL to install. The package also contains this guide
and records `physics_enabled` and `physics_engine` in `build-info.json`.

The script locates the build cache beside a Ninja executable or above a Visual
Studio `Release` directory. If the runtime was staged elsewhere, pass
`-BuildDirectory` explicitly along with the existing packaging arguments.
