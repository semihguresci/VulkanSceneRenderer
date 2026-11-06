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
restores the numeric values first written by the current run, clears pending
events and clips, and pauses. Press Play to run the start handlers again.
The custom event picker sends the declared default payload. A primary asset
replacement creates a fresh instance. Switching glTF scenes pauses the
existing graph and clears its pending input.

| Area | Supported operations |
| --- | --- |
| Events | `event/onStart`, `event/onTick`, `event/send`, `event/receive`, `event/stopPropagation`, companion node selection and hover-in/out |
| Flow | `flow/sequence`, `flow/branch`, `flow/setDelay`, `flow/cancelDelay` |
| Variables | `variable/get`, `variable/set`, `variable/interpolate` |
| Numeric pointers | `pointer/get`, `pointer/set`, `pointer/interpolate` |
| Animation | `animation/start`, `animation/stop`, `animation/stopAt` |
| Math | `math/add`, `math/sub`, `math/mul`, `math/div`, `math/min`, `math/max`, `math/clamp`, `math/mix`, `math/eq`, `math/lt`, `math/le`, `math/gt`, `math/ge`, `math/select`, `math/combine2`, `math/combine3`, `math/combine4`, `math/sin`, `math/cos`, `math/abs`, `math/neg`, `math/isNaN`, `type/intToFloat`, `type/floatToInt`, `ref/eq` |

Supported value types are booleans, signed integers, scalar and two- to
four-component floats, and reference strings. Pointer templates substitute
nonnegative indices or the index from a reference. The writable numeric table
is shared with animation pointers. It includes node TRS and morph weights,
core and implemented extension material properties, texture transforms,
punctual lights, and authored camera lens parameters. The existing parent
object and a finite numeric starting value must be available. A read of an
unsupported or mismatched path returns `isValid=false`. Pointer writes update
the existing transform, material, light, camera and deformation publication
paths. Visibility, selectability, hoverability and general object references
are outside this pointer profile.

Handlers execute in node order and complete synchronously. Sequence outputs
use lexicographic socket order. The first tick reports NaN for the previous
interval and starts the clock at zero. Later frames process queued input,
due delays, interpolations, and clip samples before tick handlers. Easing uses
cubic Bézier control points and node rotations use quaternion interpolation.
Concurrent graph clips apply in start order. Stopping a clip retains its
current pose. Manually selected clips retain their independent animation
controls and apply afterward when both routes are active.

Viewport selection and hover use the renderer's existing eligible ancestor
route. Matching source ancestors receive events from the hit toward the root.
Moving between siblings leaves their shared ancestors hovered. The existing
nearest-hit picking path approximates tracing past ineligible geometry.
Selection supplies the actual picked point and camera origin when available.

The compiler caps the default graph at 2,048 nodes, 512 entries per type,
variable, event or declaration table, and 64 sockets of each kind per node.
Data links point backward and flow links point forward. Event dispatch still
permits logical cycles, so the runtime caps work at 4,096 activations or value
evaluations and 96 nested calls per frame. It also bounds pending events,
delays, interpolations and clips. An unhandled runtime error pauses evaluation
with the operation or budget diagnostic. Existing completed writes are
published at the frame boundary.

Scene export retains the supported graphs and remaps node events, numeric
pointers, and typed references to the detached glTF indices. It retains TRS
for graph transform targets so they remain writable on reload. A saved file
starts a fresh graph from its saved property values. Graph-driven clips keep
their authored channels so export-added animation restores cannot overwrite
properties set by the graph. Save currently rejects
direct punctual-light references, since the light exporter may split a
definition across edited bindings. It also rejects ambiguous morph targets
and saves that require node or resource retirement compaction.

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
writes, synchronous order, custom and node events, easing, clip lifecycle,
malformed graphs, and the execution budget. The native regression accepts
`--portability` to check saved GLB reload and Import / Merge, including
undo and redo. CPU cases cover independent-graph rejection, missing placement
targets, and typed reference remapping.
