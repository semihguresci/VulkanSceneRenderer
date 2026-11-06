# Physics joints

Fixed joints preserve the relative placement of two bodies. Hinges preserve an
anchor and allow rotation about one axis; sliders allow translation along one
axis. Hinge coordinates and motor velocities use radians and radians/second;
slider coordinates use scene units and scene units/second. The renderer's usual
meter-based imports therefore give meter-based slider settings.

## Use the controls

1. Attach bodies through **Scene Controls → Physics**. At least one endpoint
   must be dynamic; static/kinematic pairs cannot form a simulated mechanism.
2. Expand **Joints**, select the first attached body and choose **Use selected
   as first**, or choose **Use world** for a fixed world anchor.
3. Select the second attached body. Choose Fixed, Hinge or Slider, then set the
   anchor, world axis and optional limits/motor. **Auto anchor between bodies**
   uses the midpoint of their centers, or the selected body's center for World.
4. Choose **Connect bodies**, then **Play** or **Single step**. Joint rows report
   hinge angles in degrees and slider travel in scene units. To change a motor
   during playback, choose its joint type, edit motor settings and use the row's
   **Apply motor** button. **Remove joint** releases the constraint.

Hinge inputs use degrees in the UI and radians in capture/API settings. A zero
world axis, nonfinite settings or invalid limits disable connection. Use Fixed
to lock all motion; enabled movable-joint limits must have positive width and
contain zero. Unchecked limits allow a full hinge revolution or unconstrained
slider travel. Position motor targets are clamped to enabled limits.
Limited hinge position motors follow the allowed angular interval when the
shortest periodic route would cross a hard stop. Internal intermediate goals
guide that motion while the public target remains unchanged. A full
`[-pi, +pi]` interval follows Jolt's unrestricted full-revolution behavior.

**Apply motor** checks only the selected motor mode's settings. An invalid draft
anchor, axis or limit range for a new connection does not block updating an
existing motor or turning it off. Hidden targets and spring fields remain in
the editor for later use without affecting another motor mode.

Creating a joint while playing preserves pending kinematic motion for the next
simulation tick, including contact with unrelated bodies. Its canonical frames
use the endpoints' current physics poses. While paused, creation synchronizes
authored endpoint transforms immediately.

Connected endpoints do not collide by default. This prevents their contacts
from fighting a weld or overlapping hinge. **Connected bodies collide** opts
that joint into contact; if another joint between the same pair still disables
contact, the pair remains filtered until its last such joint is removed. Other
bodies keep colliding normally. Adding/removing filtering invalidates cached
contacts and wakes the endpoints so sleeping mechanisms respond immediately.

Motor and limit edits wake the connected dynamic bodies before the next solve.
Powered hinge and slider motors also keep their dynamic mechanism awake while
there is commanded travel, including speeds below the ordinary sleep threshold.
Position goals use a `1e-4` scene-unit/radian tolerance, widened to two float ULPs
at large coordinates. An outward velocity drive at its enabled hard stop may
sleep; reversing the motor or changing its limits resumes motion. Off, zero
velocity, zero-force and settled position drives add no wake, and stopping or
removing the drive restores ordinary sleeping. Motors in one component share
one traversal per tick using the world's reserved scratch storage.

Creating or removing a joint also wakes every affected dynamic link. Removal
wakes both surviving components when it splits a mechanism, including when
removing an endpoint body retires its joints. A stationary kinematic endpoint
stays asleep, so a separate mechanism sharing that support is not activated.
The same applies to an activating dynamic pose change, a changed nonzero
dynamic or kinematic linear/angular velocity, and a moved static or kinematic
support. Every dynamic link in the affected component participates in the first
constraint snapshot; waking only the nearest endpoint could otherwise leave a
farther joint asleep while its neighboring links move.

Wake traversal stops at world, static and stationary kinematic supports. A motor
edit therefore leaves a separate sleeping mechanism on the same support alone.
Moving a support, including starting its kinematic velocity, seeds each directly
connected dynamic branch. An already-moving kinematic driver retains its motion.
Prescribed kinematic velocities keep those branches participating even at very
low speeds. Stopping both velocity channels restores ordinary sleeping. Driver
membership is sparse and retired on body removal and world clear; it adds no
per-frame scan of the world's static body population.
Unchanged or zero velocity assignments and unchanged kinematic targets,
including quaternion-sign-equivalent rotations, add no component wake.
A dynamic pose update with `activate=false`
also retains its existing activation policy. These rules preserve which bodies
participate in the solve; they do not eliminate iterative solver error.

Joint traversal uses storage reserved when the world is constructed. Walking
chains, cycles and reused body slots does not allocate during joint removal.
Nearby-contact wakes also traverse the complete affected dynamic components.
Inserting, removing or teleporting a support therefore releases every link
before the first solve, including links outside the support's contact bounds.
Each neighborhood is collected before activation and traversed once; stationary
kinematic hits do not wake separate mechanisms attached to them.
New static, kinematic and dynamic colliders use the same contact-neighborhood
wake before the next solve. A newly active collider alone cannot guarantee
that a sleeping mechanism's distant joints enter the first constraint snapshot.

Joint IDs remain stable across reset, collider replacement and scale edits.
Reset restores authored body poses and pauses; the current motor and limit
settings remain. Attachment points follow their node's geometry when its scale
or collider center changes. Axis directions remain in the body's rigid frame
under a resize; they do not acquire nonuniform stretch. World anchors stay in
world coordinates. Removing an endpoint, unloading its model or replacing a
graph removes the connected joints. A stale first-body UI selection clears
automatically after that body is replaced or reset.

World joints use an internal static endpoint near their anchor so local solver
calculations retain precision at large world coordinates. CPU regressions check
centimeter corrections at both `-1,000,000` and `+1,000,000` for all three joint
types. These endpoints consume no configured body slots and retire with their
constraints. Public world frames and joint IDs retain their existing meaning;
scene and rendered matrices still use floats.

Deterministic captures exercise the same scene joint path as the controls.
For example, after attaching the two endpoint bodies:

```json
{
  "frame": 3,
  "physicsJointCreate": {
    "type": "hinge",
    "bodyA": {"node": 1},
    "bodyB": {"bimObject": 0},
    "anchor": [0, 3, 0],
    "axis": [0, 0, 1],
    "limits": [-0.5, 0.5],
    "motor": {"mode": "velocity", "targetVelocity": 1, "maxForceOrTorque": 1000}
  },
  "physicsAction": "play"
}
```

Each endpoint selects exactly one primary scene node or BIM/USD object. A BIM
selection resolves the attached whole-product body, including all fragments.
Only body A may select the fixed world with `{"world":true}`. Axis vectors are
normalized, and the capture path constructs a perpendicular reference normal.
Enabled limits must have positive width and contain the authored zero
coordinate; hinge limits remain within plus/minus pi. Motor modes are `off`,
`velocity` and `position`.

`physicsJointMotorVelocity` changes the selected joint's velocity motor without
reanchoring it. `physicsJointAction` accepts `remove` for the selected joint or
`clear` for all joints. Create, motor update and removal are separate events.
Physics reset restores body placement, pauses simulation and retains the scene
joint identity. Replacing a body's collider recreates its backend constraints
from the canonical body-local joint frames. Detaching an endpoint or unloading
the model retires its connected joints.

## Native checks

The opt-in runner creates its own glTF and DotBIM fixtures. A static primary
anchor connects to either a primary bar or a product composed of two separated
BIM fragments. The default matrix has twelve raster cases: three joint types,
two body providers and forward/deferred rendering. `--include-ray-taa` adds two
rotating BIM hinge cases to check ray/temporal rendering, transformed normals and
bounds under actual joint motion.

```powershell
$env:CONTAINER_RUN_GPU_PHYSICS = '1'
ctest --test-dir out/build/visual-studio -C Release -R '^physics_joint_regression$' --output-on-failure
```

Each case records eleven frames. Checks independently derive hinge angles from
the rendered rotation and slider displacement from the body position, verify
locked motion and both motor stops, and compare physics, scene and rendered
transforms. They also validate reset, explicit removal, recreation, body
replacement, endpoint detach and unload. BIM cases check both fragments,
semantic identity, inverse-transpose normal matrices and bounds calculated
from independently authored cube geometry. All captures require clean Vulkan
synchronization validation.

A focused native probe can use `--joint hinge --provider bim --technique
deferred-raster`. `--check-existing` revalidates saved telemetry/images and
clears an old pass before running the checks. Run GPU captures without another
renderer or concurrent builds.

Moving a static endpoint wakes its connected bodies, including sleeping bodies
outside the endpoint's contact bounds. For large placement changes, move the
connected assembly together or reset the authored mechanism. Teleporting one
endpoint does not teleport the other bodies, and Jolt's iterative solver can
retain joint error after a large jump. Gradual kinematic movement is preferable
when the mechanism should follow a moving support.

CPU parser tests reject ambiguous endpoints, nonfinite or incorrectly typed
values, invalid limits/motors and conflicting commands. The Python checker tests
exercise stale rendered models/normals/bounds, missing motor reversal, lost
constraints during replacement, identity changes on reset and dangling joints
after detach. Native results are measured separately from these CPU checks.

## Measured Windows validation

The initial joint integration passed Visual Studio Debug and Release application
builds, along with 165 selected C++ cases per configuration: 96 physics (30 world,
32 scene, 16
auxiliary bindings and 18 widgets), 38 capture-event, 14 object-transform and
17 BIM cache cases. The physics cases include a three-link dynamic chain under
gravity, body/collider replacement rollback, off-center scale/reset anchors,
sleeping contact invalidation and a 500 scene-units/s CCD sphere stopped by a
0.05-unit wall. Four offline dependency cases and 41 Python checker cases passed;
one optional JSON Schema check skipped because `jsonschema` was unavailable.

The Release joint matrix passed all 14 cases and 154 captures. The existing
rigid-body floor-contact suite passed four more cases and 16 captures, including
its ray-shadow footprint guard. A Debug BIM hinge probe passed 11 captures.
All native runs completed without Vulkan validation errors. Build logs, test
XML, executable hashes and native reports are under
`out/physics-joints-validation`; `validation-summary.json` records the measured
scope. Disabled-backend frontend/application objects also compiled; a full
disabled executable was not built or launched.
