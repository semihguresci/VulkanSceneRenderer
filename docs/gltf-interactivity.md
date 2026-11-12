# glTF behavior graphs

The primary glTF loader supports a bounded profile of
[KHR_interactivity](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_interactivity/Specification.adoc).
It compiles the extension's default graph from `graphs` and uses its `graph`
index when present. Graph evaluation starts on the first rendered update.
Unknown operations and malformed graphs produce a contextual diagnostic.
Required unsupported graphs fail loading. Optional unsupported graphs remain
static and show the diagnostic in Scene Controls.

Open **Scene Controls → glTF Animation → Behavior graph** to pause, resume, or
reset evaluation. Pause freezes the clock and discards queued input. Reset
restores the original property values recorded by the current run, clears pending
events and clips, and pauses. Press Play to run the start handlers again.
The custom event picker sends the declared default payload. A primary asset
replacement creates a fresh instance. Switching glTF scenes pauses the
existing graph and clears its pending input.

| Area | Supported operations |
| --- | --- |
| Events | `event/onStart`, `event/onTick`, `event/send`, `event/receive`, `event/stopPropagation`, companion node selection and hover-in/out |
| Flow | `flow/sequence`, `flow/branch`, `flow/setDelay`, `flow/cancelDelay` |
| Variables | `variable/get`, `variable/set`, `variable/interpolate` |
| Pointers | `pointer/get`, `pointer/set`, numeric `pointer/interpolate` |
| Animation | `animation/start`, `animation/stop`, `animation/stopAt` |
| Math | `math/add`, `math/sub`, `math/mul`, `math/div`, `math/min`, `math/max`, `math/clamp`, `math/mix`, `math/eq`, `math/lt`, `math/le`, `math/gt`, `math/ge`, `math/select`, `math/combine2`, `math/combine3`, `math/combine4`, `math/sin`, `math/cos`, `math/abs`, `math/neg`, `math/isNaN`, `type/intToFloat`, `type/floatToInt`, `ref/eq` |

Supported value types are booleans, signed integers, scalar and two- to
four-component floats, and reference strings. Pointer templates substitute
nonnegative indices or the index from a reference. The writable numeric table
is shared with animation pointers. It includes node TRS and morph weights,
core and implemented extension material properties, texture transforms,
punctual lights, and authored camera lens parameters. The existing parent
object and a finite numeric starting value must be available. Boolean reads
and writes support the local `visible`, `selectable`, and `hoverable` fields
of their respective node extensions. A present extension with an omitted
field reads as true. Reset restores that omission when the first write used
the default. The pointer socket type must match the property. A mismatched
or unsupported read returns `isValid=false`, and an invalid write routes
through the `err` flow. Boolean writes update inherited flags, visible light
bindings, picking, and hover routing through the scene update.

Handlers execute in node order and complete synchronously. Sequence outputs
use lexicographic socket order. The first tick reports NaN for the previous
interval and starts the clock at zero. Later frames process queued input,
due delays, interpolations, and clip samples before tick handlers. Easing uses
cubic Bézier control points and node rotations use quaternion interpolation.
An explicit `variable/set` or `pointer/set` ends the matching interpolation
and discards its pending completion flow.

`flow/setDelay` exposes the latest scheduled reference on `lastDelay`, with
`delay` supported as a legacy spelling. `flow/cancelDelay` cancels one reference.
A flow into the `cancel` input on `flow/setDelay` clears that node's pending
activations and resets `lastDelay` to null.

Concurrent graph clips apply in start order. Restarting a clip moves it to the
end of that order. Stopping a clip immediately retains its current pose.
`animation/stopAt` owns the completion flow when its time is reached before
the clip's natural end in the direction of playback. Graph clip endpoints are
finite times within ±86,400 seconds, speed is positive and finite, and scheduled
stop times are finite. Manually selected clips retain their independent
animation controls and apply afterward when both routes are active.

Viewport selection and hover use the renderer's existing eligible ancestor
route. Matching source ancestors receive events from the hit toward the root.
Moving between siblings leaves their shared ancestors hovered. Nearest-hit
routing considers the hit and its eligible ancestors.
Selection supplies the actual picked point and camera origin when available.

The compiler caps the default graph at 2,048 nodes, 512 entries per type,
variable, event or declaration table, and 64 sockets of each kind per node.
Data links point backward and flow links point forward. Event dispatch still
permits logical cycles, so the runtime caps work at 4,096 activations or value
evaluations and 96 nested calls per frame. It also bounds pending events,
delays, interpolations and clips. An unhandled runtime error pauses evaluation
with the operation or budget diagnostic. Existing completed writes are
published at the frame boundary.

Scene export retains the supported graphs and remaps node events, Object Model
pointers, and typed references to the detached glTF indices. It retains TRS
for graph transform targets so they remain writable on reload. A saved file
starts a fresh graph from its saved property values. Graph-driven clips keep
their authored channels so export-added animation restores cannot overwrite
properties set by the graph. Constant references follow retained node,
material, camera, animation and light identities through export compaction.
Unreferenced node, material and texture-resource retirement can therefore be
saved with the graph intact.

Punctual-light pointers follow the actual exported bindings, including edited
light children. Equivalent shared bindings retain one graph target. Divergent
bindings reject when one source definition would need multiple destinations.
Save also rejects ambiguous morph targets and referenced light properties whose
unit or HDR conversion cannot preserve the graph's authored values.

Import / Merge can retain a destination graph or adopt one incoming graph.
An incoming graph's node references target the selected placement, while
shared material, light, camera, and clip indices follow the merged tables.
A destination graph keeps its live state through a compatible merge. Undo
removes a newly adopted graph and redo creates a fresh instance. Independent
graphs and references outside the incoming placement reject before publication.
Dynamic object IDs require an unchanged complete index domain and cannot
consume remapped authored or picked references. Save also requires an
unchanged object table for dynamic templates. The renderer reports the
affected reference when it cannot prove a safe mapping.

Graph authoring and other KHR_interactivity operations remain outside this
profile.

Capture sequences can issue `interactivityPlaying`, `interactivityReset`,
and `interactivityEvent` at deterministic frames. The companion
`gltf_interactivity_runtime_regression.py` fixture checks an authored graph
in both Vulkan raster techniques. `gltf_interactivity_tests` covers numeric
and Boolean writes, synchronous order, custom and node events, easing, clip lifecycle,
malformed graphs, and the execution budget. Use `--scheduler-only` to exercise
delay cancellation and explicit writes during interpolation in both techniques.
Use `--viewport-events-only` to replay viewport hover and selection and check
that GPU picking and CPU hit results agree. The graph changes the material on
hover entry, hover exit and selection of the authored node. A background click
clears selection while preserving the material color.
The native regression accepts `--portability` to check saved GLB reload and
Import / Merge, including undo and redo. CPU cases cover independent-graph
rejection, missing placement targets, and typed reference remapping.

`gltf_interactivity_deformation_runtime_regression.py` starts and stops a morph
clip through custom graph events in both raster techniques. It checks GPU pose
history against independently evaluated weights and compares raster captures
with baked meshes. Stopping holds the last pose and resetting restores the
original pose and resets deformation history.

`gltf_interactivity_export_runtime_regression.py` additionally exercises node
deletion, material and texture-resource retirement, undo/redo, and portable glTF
and GLB replay in both raster techniques. Independent graph-free fixtures are
the exact image controls. Hierarchy, decoded geometry, light bindings, graph
references, camera matrices and history identities are inspected separately.
The retained selection handler is checked structurally; this suite does not
exercise native pointer picking.
