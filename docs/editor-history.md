# Scene edit history

Scene Controls provides **Undo (Ctrl+Z)** and **Redo (Ctrl+Y)**. Ctrl+Shift+Z
also redoes an edit. A focused text input keeps its ordinary text undo.
Button tooltips name the next action, and rejected actions report their reason
in the scene status.

The first implementation covers primary scene-node transforms, hierarchical
visibility, reparenting, and procedural primitive creation. One inspector or
viewport drag is one action. Independent gestures remain separate, a new edit
discards the redo branch, and replacing or unloading the scene clears history.
History retains up to 128 actions.

Undo restores exact local matrices, parentage and sibling/root order. It does
not decompose imported affine transforms back into TRS. Replaying an edit uses
the same physics/BIM transform validation as authoring it, and rejected replay
does not advance the history cursor. Dynamic-body edits pause simulation where
required; simulation itself is not recorded as a sequence of editor commands.

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

`EditHistory` owns bounded command storage and gesture merging.
`SceneEditHistory` validates and records graph changes and detects graph
replacement by identity. `RendererFrontendEdits` connects replay with selection,
physics checks, object/provider refresh, asynchronous DDGI synchronization and
temporal invalidation. GPU publication occurs after frame readers retire;
input-time drags only queue publication.

Build and run `edit_history_tests`, `scene_edit_history_tests`,
`scene_graph_edit_tests`, `scene_hierarchy_ui_tests`, and `physics_scene_tests`.
These cover branching/eviction, rejected commands, exact affine replay,
interleaved reparent/primitive redo, retained topology, real multi-frame ImGui
drags, graph replacement and prospective-parent physics validation.
The native `tests/validation/primitive_history_runtime.py --reuse-only`
regression checks geometry counts and independent placement through divergent
adds and replay.
