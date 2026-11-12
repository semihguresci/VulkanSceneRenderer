# glTF node visibility and interaction flags

The loader reads `KHR_node_visibility`, `KHR_node_selectability` and
`KHR_node_hoverability` on every source node, including inactive scenes. Each
optional local boolean defaults to true and inherits independently through
ancestors. A false parent cannot be overridden by a true child. These are
ratified Khronos extensions:
[visibility](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_node_visibility),
[selectability](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_node_selectability)
and [hoverability](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_node_hoverability).

Raw JSON validation runs before resource I/O and integer narrowing. Extension
objects and boolean properties are strict; authored extensions must appear in
`extensionsUsed`. Local storage is bounded to one million source nodes and
16 MiB, and counts against the aggregate import budget. Unknown properties and
extension metadata remain untouched.

Source groups retain local flags. Synthetic primitive and instance children
start with true defaults and inherit their source group's values. Visibility
filters visual meshes, shadows, ray placements and attached lights. Cameras
remain available below hidden parents, and visibility does not change logical
selectability or hoverability.

Selection and hover use the pinned reference renderer's approximation: resolve
the nearest geometric hit, find the highest ancestor that disables the relevant
flag, then route to its parent. A blocked source root returns no target rather
than selecting a synthetic import/editor root. CPU and GPU ID picking use the
same routing helpers. This does not trace through excluded geometry to find a
farther eligible hit. The capability registry reports selection and hover as
partial. The [behavior graph runtime](gltf-interactivity.md) uses the same
eligible ancestor route for viewport events.

`KHR_animation_pointer` can animate each local flag through the corresponding
node extension property. The enclosing extension object must exist and can
use the default-true value. Animation uses unsigned-byte SCALAR STEP keys.
Zero means false and a nonzero byte means true. Seeking and playback publish
flag changes through the same visibility and interaction updates as editor
edits. Clearing the clip restores the tracked authored flags, including
defaulted properties. Save retains the pointer channels and uses the same
Boolean encoding for cross-clip restore tracks. Behavior graphs can also read
and write the local flags with Boolean sockets. [Behavior graph pointer rules](gltf-interactivity.md)

The hierarchy's Visible, Selectable and Hoverable controls edit local flags.
Undo/redo retains all three independently; grouped capture edits are one history
command. A capture event can use
`{"frame":2,"nodeFlags":{"sourceNode":3,"visible":false,"hoverable":true}}`.
Edits and their undo/redo update existing authored flag properties in the
evaluated glTF model, so behavior pointers read the same local values.
Use `{"frame":3,"viewportPointer":{"action":"select","pixel":[320,180]}}`
or `"hover"` to run the normal viewport handler on the completed frame. The
behavior graph processes that input on the next frame. A sequence with these
events uses scripted pointer input and retains the fixed timestep and camera track.
Scene switches retain local edits while clearing selection and hover. Publication
invalidates hover caches, refreshes visible lights/providers and resets temporal
history. Light overrides follow source-node/dialect identities when visibility
changes their compact runtime ordinals. Hide and show preserve an authored
light's edit history while that light's source binding exists.

GLB and ASCII saves retain authored extension presence, empty/defaulted objects,
explicit true values, unknown properties and extras. Editing a default true
flag does not add an extension unless its new value is false. Source metadata
is not copied to synthetic geometry children. Full light export includes hidden
bindings and their edits/range scaling, while the current visible table selects
the active authored sun. Generated scene placement nodes carry renderer metadata
that keeps them out of ancestor routing after reload. The authored root remains
editable, including when its flags originally blocked selection and hover.

## Validation

Renderer `71e1b6327cde69f39ce1581b15169fd64c21df27279888ca59f3a409aa481b39`
passes **904 cases across 40 executables**, with all four real Vulkan paths
enabled and no failures/skips. Coverage includes strict pre-I/O parsing, local
defaults and inheritance, source/synthetic identity, active views, graph copy /
move / undo tails, real hierarchy checkbox clicks, grouped history, portable
metadata, stable light-binding export and 10,000-node light hierarchies with
transactional malformed-forest rejection.

**52 native captures / 48 checks** pass in both raster techniques, with and
without hard-ray shadows and DDGI. All 44 positive comparisons against
independently pruned/ordinary references and portable reloads are pixel-exact;
four negative controls differ by at least 6.1102/255 mean error. The matrix checks
the same nearest hit's independent selection/hover routes, blocked-root
rejection, hidden visual/light restoration, camera availability, history counts
and authored-presence/metadata preservation through both file formats. The ray
matrix checks TLAS counts of four hidden versus five restored instances,
positive BLAS and 27 updated DDGI probes / 108 rays. DDGI intensity is zero in
this matrix: it proves execution/publication rather than visual GI contribution.
All 20 distinct process logs contain zero VUID/SYNC errors; all 94 frozen native
manifest entries remain unchanged. No diagnostic captures are counted.

Evidence: `out/feature-parity/node-flags-validation-checkpoint.json`,
`node-flags-cpu-checkpoint.json`, `node-flags-native-manifest.json`,
`node-flags-runtime/results.json` and `node-flags-ray-runtime/results.json`.
The completion checkpoint adds **13 scene/mode captures / 14 checks**, including
edited inactive flags, repeated scene changes, a deferred-to-forward switch,
stable graph/geometry references and both-format reloads. All eight positive
images match independently pruned references exactly. A **six-capture / ten-check**
capacity witness saves and reloads an inactive scene with 4,097 draws while a
one-draw scene stays active; both formats retain hidden flags, metadata and light
ranges. Two CLI attempts to activate the oversized scene fail with the expected
capacity diagnostic. The existing **12-capture / 12-check** animation/camera/
history/material scene-state witness also passes on this same binary.

In total, this binary passes **83 captures / 84 checks**, 60 pixel-exact positive
comparisons and two expected CLI rejections, with 42 distinct logs containing
zero VUID/SYNC errors and all 94 manifest entries unchanged. Evidence:
`out/feature-parity/node-flags-completion-checkpoint.json`,
`node-flags-scene-switch-runtime/results.json`,
`inactive-scene-capacity-runtime/results.json` and
`node-flags-scenes-state-runtime/results.json`. All changes remain local /
uncommitted at that checkpoint. The [behavior graph runtime](gltf-interactivity.md)
adds selection and hover events under #103.
