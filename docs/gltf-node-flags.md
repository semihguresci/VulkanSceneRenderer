# glTF node flags

In **Scene Controls > Scene Hierarchy**, the **Flags** column shows **V** for
visibility, **S** for selection and **H** for hover. Hover over a checkbox for
its full label. The checkboxes edit local values and support undo and redo.
An ancestor can disable a flag even when the selected node's local flag is on.

Hidden nodes omit their meshes and lights from rendering, shadows and ray
instances. Authored cameras remain available. Light edits follow the source
node when hide/show changes the order of visible lights.

Viewport selection and hover start with the closest geometric hit. Each
checks its own flag and can route to an eligible ancestor above a blocked
branch. A blocked root yields no target. This is the reference renderer's
ancestor-routing approximation. The glTF capability registry reports partial
support for selectability and hoverability. Full trace-through behavior and
behavior-graph events are tracked separately.

GLB and glTF save local flags and retain authored defaults, extra properties
and metadata. Container marks its exported editor transforms and scene
placements in node `extras` to preserve picking across save and reload.
Hidden lights and their edits stay in the saved scene.

The Khronos specifications define the three authored flags.
[Visibility](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_node_visibility/README.md) ·
[Selectability](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_node_selectability/README.md) ·
[Hoverability](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_node_hoverability/README.md)
