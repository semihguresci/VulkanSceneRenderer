# Current editor controls

This guide covers the controls present in the current editor. Use it to work with developed features now. The broader [UI/UX design guide](ui-ux-design-guide.md) describes future interface work; it is not a list of controls already available.

## Find a tool

**Rendering Engine → Path tracing** selects the progressive Vulkan reference
renderer on supported hardware. Its status line shows attempted samples and
unsupported/numerical diagnostics. Camera, scene, material and light edits restart
accumulation. Exposure remains a display control. This renderer currently supports
a surface subset; see [reference path tracing](reference-path-tracing.md) for limits
and command-line sample/bounce settings. Forward and deferred raster remain available
in the same chooser.

Alpha-blend opacity and double-sided alpha masks are supported in path tracing,
including shadow rays. Thin-walled optical transmission supports tint, roughness,
textures and multiple panes. Closed homogeneous solid glass supports refraction
and distance-based absorption, including nested volumes and cameras inside them.
Dispersive solids bend red, green, and blue paths differently. See
[reference path tracing](reference-path-tracing.md) for topology and material limits.
Use **Reload Shaders (F5)** to load rebuilt shader files into an open editor.

The **Scene Controls** window has a fixed menu bar:

- **Go to** scrolls to an existing section and opens its main header where needed. It does not switch renderers, change selection or change the scene.
- **Selection** jumps directly to the selected object's controls and opens its transform section.
- **Material** jumps directly to the material inspector, including Assign and Make unique.

The rest of the window remains one scrollable panel. Resize or move it as needed. Go to remains available while scrolling. **Renderer telemetry** focuses and expands the existing telemetry window. Unavailable glTF scene, material-variant and BIM routes explain what must be loaded.

## Load, merge and save

Open **Go to → Import / save**.

1. Set **Import scale** before loading. Choose a **Sample model**, or enter **Model path** and click **Load model**. Load replaces the primary model; **Reload Default** and **Unload model** operate on that primary model too.
2. To add content to the current scene, use **Import / Merge glTF...**. Enter a `.gltf` or `.glb` path, choose a parent name, and use the file's default scene or enter a scene index starting at zero. Click **Import**.
3. Enter a `.glb` or `.gltf` path in **Save path** and click **Save As**. After a successful save, **Save scene** or **Ctrl+S** saves to that destination.

**Model path** accepts an absolute path to an existing IFC/IFCX model, including
models under another build directory. Testing an existing model needs no copy.
Raw scene-linear EXR export excludes editor selection/inspection annotations;
display image captures retain the normal editor presentation.

Scene export covers all scenes in the primary asset. BIM/USD/VDB sidecars, physics setup and renderer settings use their own formats. Read status/error messages after an operation; a format limitation or failed save is not a successful export.

Use **Go to → glTF scenes** to select a scene from a loaded glTF asset. That chooser is unavailable when no scene list is supplied by the current asset.

The same section has read-only **glTF asset metadata** and **glTF scene metadata** trees. For an imported selection, the Selection and Material sections show separate XMP packets for the node, mesh and current material. Open **Raw packet JSON** for the complete packet, including the namespace context. Missing or invalid references show a message in their own tree. XMP remains separate from the BIM property inspector.

## Select and edit objects

Open **Go to → Scene hierarchy**. Select a row to inspect it. The hierarchy's visibility, selectability and hoverability controls have different meanings; parent state can affect a child.

- Press **F2** on a selected hierarchy item for inline rename; Enter commits and Escape cancels. The context menu also offers **Rename...**.
- Use the context menu for **Add empty child...**, **Duplicate subtree...** and **Delete subtree...**. **Add empty node** is also available in the hierarchy panel.
- Use the parent chooser for reparenting. Use the row's focus action to frame an object.
- **Add Primitive**, near the import/animation controls, creates a quad, plane, cube, sphere, triangular prism, pyramid or torus.

Click **Selection** for the selected node's numeric transform. The **Mesh** chooser selects a renderable node. **Go to → Camera / scene transforms** exposes camera and whole-scene transforms.

The separate **Viewport** window provides Select, Move, Rotate and Scale, Local/World space, axis constraints and snapping. Its **Navigation** chooser supports Revit and Blender mouse conventions; hover it for the exact mouse gestures.

Use **Undo (Ctrl+Z)** and **Redo (Ctrl+Y)** at the top of Scene Controls for recorded edits. Hover their buttons to see the action names. Simulation transport and renderer quality settings are separate from scene-edit history.

## Edit materials

Select an object, then click **Material**. For an object/group with several renderable surfaces, choose the intended **Surface** first.

- The inspector states how many surfaces share the material. Changing its factors, name, alpha mode, sidedness or material features affects every surface using that definition.
- To reuse another definition, choose it in **Assign material**, then click **Assign**. This changes the selected surface binding.
- To edit only this surface, click **Make unique**, then edit the new material. For an animated source material, this creates a copy of its current appearance without copying its animation tracks.
- Numeric fields support dragging and exact entry. **Reset** restores that property's schema default. **Material features** exposes the supported extension toggles.
- Animated factors show authored rest and current evaluated values. Editing the rest value does not rewrite the animation curve; playback can continue to drive that factor. An extension referenced by animation cannot be removed unsafely.

Some procedural or implicit materials initially have no editable source definition. Make unique is available only when the application can prove their texture/source bindings. Unsupported or stale provenance can leave it disabled or cause the operation to report an error. Assign remains a separate operation. Normal-map assignments that require a different per-surface tangent/UV frame are currently rejected.

Expand **Texture slots** under **Material**, then open a slot. Rows show its file/source identity and **sRGB color** or **Linear data** role. Enter a full local path in **Image path**, then click **Import image** for an empty slot or **Replace from file** for a bound slot. Alternatively, choose a retained source in **Existing image** and click **Use selected texture**. **Clear slot** removes that material's binding while preserving other uses of the image. Changes affect every surface sharing the material; use **Make unique** first to edit only the selected surface. Maps work alongside the material's factors, alpha mode and enabled features.

Under **UV mapping**, edit **Coordinates** (UV0/UV1), **Offset**, **Scale** and **Rotation (radians)**. **Channel** appears only for bindings with editable channels; standard slots keep their defined channels. **Apply mapping** publishes the draft, **Cancel mapping** discards it, and **Reset mapping** applies mapping defaults. Under **Sampler**, edit **Minification**, **Magnification**, **Wrap U** and **Wrap V**, then use **Apply sampler** or **Cancel sampler**. To restore sampler defaults, select **Default (trilinear)**, **Default (linear)** and **Repeat** for both wraps, then apply. Mapping and sampler settings require a bound texture.

Successful actions enter the existing **Undo (Ctrl+Z)** / **Redo (Ctrl+Y)** history. Undo/redo uses the current animation clock. Export the edited primary asset through **Save As**; **Save scene** and Ctrl+S reuse the last successful save path. Rejected texture edits report an error beside **Texture slots**. Correct the path/settings and retry.

A normal map must use coordinates present on every affected surface and compatible with its retained tangent frame, including retained default and variant uses. Changes requiring a new per-surface normal UV frame are rejected. Specular-glossiness materials expose one shared specular/glossiness source, labeled **sRGB color / linear alpha**: RGB supplies specular color and alpha supplies glossiness. Their dormant metallic-roughness slot is disabled with an explanation. A separate legacy roughness map retains its **Linear data** role. Clearing a binding does not delete unused images or texture records from the resource catalog.

For authored alternatives, open **Go to → Material variants** and select a variant. **Default** or **Restore default materials** restores the default bindings. This chooser requires an asset containing variants; variants and manual Assign are different operations.

To remove an unused definition, open **Material → Material catalog**, choose the
material, and click **Delete unused material**. This chooser also works without
an object selected and includes runtime materials. The button explains why a
material is protected or still referenced. Defaults, retained scene bindings,
variants, animation targets and external provider/BIM references can prevent
deletion. Hidden objects still count as users.

**Undo** restores the definition; **Redo** deletes it again. Other material IDs
stay stable in the editing session, and retired rows disappear from Assign and
catalog choices. Save As writes a dense material table and remaps surviving
references. Deleting a material retains its images and textures for other uses
and history; image/texture record deletion remains separate work.

## Play animation or choose an authored camera

Open **Go to → Animation / authored cameras**. Choose a **Clip**, then use **Play/Pause**, **Loop**, **Speed** and **Time**. Negative speed plays backwards. Choose **Rest pose** to leave clip playback.

If the asset contains cameras, use **Camera** in this section to choose one, or return to **Viewport camera**. Open **Animation diagnostics** when the asset reports messages. These controls provide playback and camera selection; they do not provide keyframe or curve authoring.

An authored camera selection also exposes **Projection**, **Vertical FOV
(degrees)**, automatic/fixed aspect, near/far or **Infinite far**, and separate
orthographic horizontal/vertical magnitudes. Click **Apply camera edit** to
record one Undo/Redo action. Shared camera definitions show their use count.
Animated lens fields explain why they are read-only; unanimated fields remain
editable during playback. Both save paths retain the lens and its authored rest.

To move an authored camera from the free view, select it in **Camera**, verify
**Camera pose → Destination**, then choose **Viewport camera**. Navigate and
click **Write viewport pose (keep lens)**. The write keeps you in the free
viewport and preserves the destination lens; choose the authored camera again
to see its framing. Only that node moves, with its children, under the same
parent. Other nodes sharing its lens keep their poses.

One changed write is one shared Undo/Redo action. Replay uses the current parent
pose and preserves the animation clock. The destination can expire after a
reload or node replacement; reselect the authored camera if it disappears.
Selected-node TRS/matrix animation, active edit conflicts and physics/provider
ownership can block a write, with a reason in the panel. Animated lens-rest and
pose-keyframe authoring remain separate work. See
[Authored camera editing](gltf-camera-authoring.md) for the complete workflow
and supported scope.

Saving an animated scene keeps its current pose as the file's defaults while retaining its clips and original keys. Clearing playback after reload returns to those saved defaults. The playback clock itself is not saved. See [glTF scene export](gltf-scene-export.md) for the full export contract.

## Physics, volumes and BIM

**Physics:** open **Go to → Physics**. Select an object, choose its motion/body settings, then use **Attach body**, **Replace body** or **Detach body**. The panel has its own Play/Pause, Single step, Reset, Clear bodies and Create demo controls. **Joints** provides the supported body/world connection, limits and motor controls. Availability and attachment restrictions are shown in the panel; physics can be disabled in the build. Its playback is independent of glTF clip playback.

**Volumes:** open **Go to → Volumes / OpenVDB tools**, even when no volume is loaded. Choose an existing operation and fill its typed inputs/settings, then use its run/output controls. Feature availability depends on the build and selected operation; inspect its status and result. The current operation list and limits are authoritative. See [OpenVDB feature status](openvdb-feature-status.md) for capability boundaries.

When a volume is loaded, **OpenVDB volume** exposes visibility, geometry shadows, density/scattering, emission and ray-step settings. Temperature controls require temperature data. Available sequences expose their own frame, pause, loop, speed and loading/cache state.

**BIM:** load a supported BIM model, then use **Go to → BIM**. Its existing sections include elements, spatial/storey browsing, search/filters, properties/IFC relationships, schedules/quantities, model comparison, selection sets, phases, layers, clipping, measurements, viewpoints/BCF and drawing export. Expand the relevant section. Controls depend on the loaded metadata and selection; unavailable data is reported in place. BIM measurements/drawing exports and primary glTF scene export have different scopes.

## Rendering, lighting and diagnostics

| Go to destination | What to use there |
| --- | --- |
| **Rendering / display / overlays** | Choose an available **Rendering Engine** (forward, deferred or path tracing), display visualization, Editor/Render Preview mode, MSAA, vertex/light overlays, normal validation and wireframe settings. Unsupported options explain their availability. |
| **Lights** | Add/select directional, point, spot or area lights and edit the selected light. Field and gizmo drags each create one Undo action on release. **Esc** cancels an active drag. **Lighting Settings** includes directional/environment/bounce intensity and area-light integration/quality. |
| **Shadows** | Choose supported Raster, Ray hard or Ray soft visibility and tune the existing quality/bias/budget settings. The panel reports ray-query support/effective status. |
| **Global illumination (DDGI)** | Inspect support/activity, enable or pause probe GI, adjust tracing and volume settings, and use the existing volume Apply/Cancel/fit controls. Keep unapplied volume drafts distinct from active settings. |
| **Exposure / anti-aliasing / bloom** | Edit manual/automatic exposure, TAA and bloom. TAA requires MSAA 1x; use **Reset temporal history** when needed. DDGI sits between the TAA and bloom sections in the existing scroll layout. |
| **Diagnostics / render passes** | Inspect culling and technique information; enable/disable optional render passes. Protected passes stay locked and dependencies can disable optional passes. **F8** freezes the culling camera. |
| **Renderer telemetry** | Read CPU phases, workload/culling, render graph and synchronization measurements in the separate window. GPU timing describes completed work and may have limited coverage. |

**Vulkan runtime capture** reports capture status. Capture must be armed before Vulkan instance creation; this section is not a live Record switch.

**GPU hierarchy transforms** near the top of Scene Controls enables the optional compute path. CPU state remains current for editing, picking and physics. The CPU path is the default; see [behavior and current limits](gpu-hierarchy-transforms.md).

If a control is disabled, hover it and read the adjacent status. The navigation menu exposes existing controls; it does not bypass their asset, device, provenance or build requirements.
