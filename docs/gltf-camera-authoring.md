# Authored glTF camera editing

The renderer already lists and selects camera-bearing source nodes, evaluates
their complete parent world transforms and roll, and supports authored
perspective and orthographic projections. Camera selection lives in the existing
animation controls. Startup captures can select a source node with
`--gltf-camera-node`.

## Edit a camera lens

In **Scene Controls**, expand **glTF Animation**, then choose an authored camera
in **Camera**. The lens editor uses the existing Dear ImGui panel:

1. Choose **Perspective** or **Orthographic** in **Projection**.
2. For perspective, set **Vertical FOV (degrees)**, **Near plane**, and either
   **Auto aspect** or a fixed aspect ratio. **Infinite far** keeps the authored
   far plane omitted; disable it to enter a finite **Far plane**.
3. For orthographic, set **Horizontal magnitude** and **Vertical magnitude**
   independently, then the near/far planes.
4. Click **Apply camera edit**. One Apply is one shared Undo/Redo action.

The editor shows when several source nodes share a camera definition. A lens
edit affects all those instances, while their node poses, hierarchy and roll
stay unchanged. Editing a lens field targeted by animation changes its authored
default. The active clip keeps supplying its sampled value. Choose **Rest pose**
to see the edited default. Undo and redo preserve the current playback time and
the original animation keys. Playback also retains unfinished lens drafts.

A camera with lens animation keeps its projection type. Animated optional
aspect and far fields retain a finite authored value so their animation
pointers keep resolving. Their numeric defaults remain editable.

Use **Ctrl+Z / Ctrl+Y** or the existing Undo/Redo controls. Replay preserves the
current animation clock and unrelated current values. Both **Save scene** and
**Save As** use the shared authored/evaluated camera records and animation rest
ownership; the editor adds no export-only lens override. Changing a lens does
not replace camera buffers, material descriptors or geometry.

Both `.gltf` and `.glb` saves keep **Auto aspect** and **Infinite far** omitted
in the file. A finite far plane remains a positive stored value. Camera and
lens metadata survive the shared export path. Choose **Rest pose** before
saving to persist edited defaults. Saving a selected clip records its current
evaluated pose as the file's defaults.

## Write the viewport pose to a camera

Open **Go to → Animation / authored cameras**, then expand **glTF Animation**:

1. In **Camera**, select the authored camera node you want to move.
2. Check its name under **Camera pose → Destination**.
3. Change **Camera** to **Viewport camera**. The destination remains selected
   for authoring while you navigate freely.
4. Navigate to the position and direction you want, check **Destination**, then
   click **Write viewport pose as default**.

The write changes the node's authored default (rest) pose and leaves you in
**Viewport camera**. Choose the authored camera again to view through it. Its
lens stays unchanged, so its FOV, aspect, clipping planes or orthographic
magnitudes can give different framing from the free viewport.

Translation, rotation and scale animation are supported on the destination,
including targets in an unselected clip. Active keys keep controlling their
targeted components; other edited defaults appear immediately. Choose
**Rest pose**, or a clip that does not target those components, to see their
edited defaults. A fully animated camera can keep its current sampled pose
while the write changes its defaults. No keys are created or rewritten.

A pose write affects only the chosen node. Other nodes sharing its camera
definition keep their poses; a separate lens edit still affects all those uses.
The node stays under the same parent, and its attached mesh, lights and children
follow its current evaluated transform. The parent-to-local solve preserves the
existing world scale/shear residual while matching the requested default eye,
direction and roll. Nodes with transform-animation targets retain a checked
TRS representation, including when their clip is not selected.

Each changed write adds one **Write viewport camera default** action to shared
Undo/Redo, even when active keys keep the visible pose unchanged. Replay
restores the original default local endpoint against the **current** parent
transform and samples the current clip at the current playback time. It
preserves the clock, original keys, lens and unrelated current values.
An animated parent can therefore place a replayed camera at a different world
position from the one it had when first written. Undo retains the original
optional matrix/TRS representation. Animated-node writes use TRS; unanimated
nodes retain the checked matrix path. Morph weights remain unchanged. A true
no-op adds no history entry.

**Save scene** and **Save As** retain the camera node pose, lens, hierarchy,
shared associations and metadata through the normal `.gltf`/`.glb` paths.
Clips and their original keys remain available; the playback clock is not saved.
Choose **Rest pose** before saving to make the edited defaults the file's main
pose. Saving during clip playback retains the current evaluated pose as its
main defaults, following the shared animated-scene export contract.
See [glTF scene export](gltf-scene-export.md) for saved defaults and playback
after reload.

The destination is a weak selection of a particular asset, source binding and
node lifetime. Reloading, unloading or replacing that node can invalidate it;
a matching name does not retarget a pending write. If the destination disappears
or the editor reports a changed target, select the authored camera again and
return to **Viewport camera**. Finish pending lens/playback actions or active
transform/material gestures before writing. Reasons and errors appear in the
**Camera pose** section.

Animated ancestors and selected-node weight animation are also allowed; the
write uses today's parent pose and preserves weights. Matrix animation,
quaternion-component pointers and unsupported selected target schemas still
reject. A parent transform and requested view that require genuine local shear
cannot be represented as an animated node's TRS: that specific write fails
atomically, without changing defaults, history or playback.

For a read-only linked destination, use **Make Editable** first. With physics
enabled, detach body/physics-provider bindings in the camera subtree first.
External-provider states must pass the existing publication guards; this action
does not author a second physics, BIM or provider transform format. Invalid,
singular, ill-conditioned or unrepresentable transforms and stale source/view
proofs reject before a pose action is published.

Capture scripts can use `cameraSelection` with exactly one of `sourceNode`, an
exact unique authored node `nodeName`, or `viewport: true`. `cameraProjection`
addresses an active source node and applies an explicit typed patch.
`cameraPose: {sourceNode, fromViewport: true}` writes that node from the current
free viewport; an optional `expectedCameraRevision` rejects a stale request.
The current-frame navigation track is applied before the pose write. Capture
FOV values use radians, and `null` clears only perspective aspect/far. Use
separate events for selection, projection/pose edits, history and timeline
actions; see `tests/fixtures/rendering/temporal_capture.schema.json`.

## Projection planner

`GltfCameraAuthoring` prepares a selected camera projection patch without
mutating a model, node, animation player or GPU state. Authored and evaluated
camera IDs are explicit inputs; a caller must resolve their correspondence.
The result retains independent Before/After records and a private receipt.

- Perspective field of view, near, optional aspect and optional far are supported.
  Clearing aspect means viewport aspect; clearing far means an infinite camera.
  Neither the current viewport aspect nor the finite shadow/culling span becomes
  a stored authored value automatically.
- Orthographic horizontal and vertical magnitudes stay independent. The editor
  planner supports positive magnitudes, a nonnegative near plane and ordered
  finite clipping planes. Projection values must remain renderable in binary32.
- A camera definition can be shared by several nodes. The plan retains all its
  node references, including inactive scenes; it does not move any node.
- Opaque metadata, dormant projection fields and raw JSON caches are preserved.
  Invalid selected camera pointers reject. Animated leaves update authored
  defaults while the player prepares their current sampled projection.
  Unrelated node TRS animation permits projection edits.
- Finite storage, metadata, node and animation limits apply before selected
  record copies. The final receipt predicate checks exact records, shared uses
  and relevant animation targets without allocating. Public plan mutation and
  stale input reject.

The planner follows the engine's RH, +Y-up, -Z-forward and reverse-Z conventions;
the viewport supplies the Y flip. Schema references:
[perspective](https://github.com/KhronosGroup/glTF/blob/main/specification/2.0/schema/camera.perspective.schema.json)
and [orthographic](https://github.com/KhronosGroup/glTF/blob/main/specification/2.0/schema/camera.orthographic.schema.json).

## Publication and remaining scope

The SceneManager prepares one selected authored/evaluated/player-rest
transaction. Private camera definition identities and namespace witnesses
separate shared clones from fresh merges; ready or replayed edits reject stale
source, graph, player, curve or viewport proofs. Selected raw strided/sparse
camera animation bytes and decoded camera tracks are retained without copying
a model, geometry buffer, complete animation library or other rest tables.
Final guards and selected-row exchanges allocate nothing. Camera authoring
revision is separate from animation sampling and material/resource revisions.

Pose requests additionally prove the selected node and parent lifetimes and
the current unjittered navigation view. The selected source/rest receipt owns
only matrix/TRS fields. Detached graph, world, deformation, object, lighting,
bounds and consumer bindings are prepared before publication; history retains
selected endpoints rather than an entire scene or animation library.

The shared light exporter can move an imported light binding to a child carrier
under its retained source owner, keeping the original light definition and
metadata. Carrier transforms use an exact affine homogeneous row and a checked
float world recomposition. This keeps the serialized light reloadable beneath
scaled/sheared ancestors without moving its mesh owner.

Selected-node TRS default-pose writing is implemented. Its isolated copied-source
CPU gate passed **43 cases: 23 planner and 20 rest-transaction tests**, using
GLM 1.0.3 and Vulkan 1.4.363 headers. The main manager gate passes 13 cases on
Vulkan. Native acceptance passes 54 captures, 54 state checks, 64 image checks
and four source-independent GLTF/GLB exports across both raster techniques.
It covers full TRS and partial-pointer defaults, current-clock history, shared
lenses and child lights, exact curve/restoration preservation and atomic local
shear rejection. Evidence is in
`build/codex-gltf-validation/camera-pose-animation-v7-native/results.json`,
renderer `dfc887d53da4bb81ac75196f7d554ce0e9bf40e8110aaac72a6c592c8b9e84a4`.
This does not establish full pose support for every
external provider, BIM, physics or ray/DDGI configuration. UI tests use the
actual ImGui widgets headlessly; no Windows desktop interaction acceptance is
claimed. CPU evidence is in
`build/codex-gltf-validation/camera-pose-cpu/verification.md`.
Validation provenance is recorded in
[feature parity progress](feature-parity-progress.md).
