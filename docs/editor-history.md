# Scene edit history

Scene Controls provides **Undo (Ctrl+Z)** and **Redo (Ctrl+Y)**. Ctrl+Shift+Z
also redoes an edit. A focused text input keeps its ordinary text undo.
Button tooltips name the next action, and rejected actions report their reason
in the scene status.

The shared history covers scene-node transforms, flags, reparenting and
procedural primitives, along with node, material, light and resource authoring.
It retains up to 128 committed actions and clears on scene replacement or
unload. An inspector or viewport drag previews live and commits one action when
it ends. An edit that changes the scene discards the redo branch. A drag that
returns to its starting matrix or is cancelled with **Esc** in the viewport
keeps the redo branch.

Undo restores exact local matrices, parentage and sibling/root order. Stored
matrices preserve imported affine transforms, including shear. Replaying an edit
uses the same physics/BIM validation as authoring it. Rejected replay keeps the
history cursor in place. Editing a dynamic body pauses simulation where required.
Selection cleared by a selectability or reparent edit returns when replay makes
the same node selectable again and the selection is still empty. Playback checks
node identities so a newly created node at a reused index cannot receive a stale
edit. A selected imported light follows its source binding as the visible lights
are reordered. Undo can restore its selection after an ancestor visibility edit
when the selection is still empty.

Undoing primitive creation removes the appended nodes from the hierarchy,
picking and renderable lists. Redo restores their original node IDs and
selection. Matching built-in primitive geometry stays resident until the
scene is replaced. Repeated adds and redo share that geometry, while each
node keeps its own transform, material, and selection identity. Starting a
new branch after undo can reuse the same mesh without reuploading its vertex
and index data. Detach any physics bodies attached after
creation before undoing that creation, so no collider/joint can reference a
removed node. External topology changes which cannot be safely reconciled
cause replay to refuse the operation.

Select a light in **Lights** or pick its viewport icon. Editing a field,
using a transform gizmo, or adding a manual light uses the same Undo and
Redo controls. A drag previews changes and becomes one action when released.
Press **Esc** to cancel a drag. A drag that returns to its starting value
keeps the existing redo branch.

Imported lights are edited as renderer overrides. Undoing the first override
reveals the imported light at the current animation time. Redo reapplies the
edited fields to the current light. **Save As** exports the visible light edits
with the scene.

`EditHistory` owns bounded command storage. `SceneEditHistory` validates and
records graph changes, retains the exact endpoints of an active transform
gesture, and detects graph replacement by identity. `RendererFrontendEdits`
connects replay with selection,
physics checks, object/provider refresh, asynchronous DDGI synchronization and
temporal invalidation. GPU publication occurs after frame readers retire;
input-time drags only queue publication.

Build and run `edit_history_tests`, `scene_edit_history_tests`,
`scene_graph_edit_tests`, `scene_hierarchy_ui_tests`, and `physics_scene_tests`.
These cover branching/eviction, rejected commands, exact affine replay,
interleaved reparent/primitive redo, retained topology, real multi-frame ImGui
drags, graph replacement and prospective-parent physics validation.
The native `tests/validation/scene_node_history_runtime.py` and
`tests/validation/primitive_history_runtime.py` regressions cover forward and
deferred raster replay, selection, geometry reuse, and publication to TAA, DDGI
and ray acceleration. `tests/validation/physics_pending_edit_regression.py
--case transform-history` verifies a Jolt collider stays aligned with its node
through transform, reparent, replay and reload in a physics-enabled build.
