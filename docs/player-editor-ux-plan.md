# Player and Editor UX Plan

This plan defines the user experience direction for splitting the renderer into
two primary modes: a focused player/renderer mode and a full editor mode. The
goal is to grow the project toward a professional scene creation tool using
familiar workflows from Blender, Unreal Editor, Substance 3D Painter, Revit,
3ds Max, AutoCAD and Fusion, while preserving its existing BIM and renderer
engineering tools.

## Product Direction

The application should be built around one shared scene engine with two
different workspaces on top of it:

- Player / Renderer Mode: view, present, interact with, and render scenes.
- Editor Mode: create, organize, modify, inspect, and save scenes.

Player mode should feel confident and minimal. Editor mode should feel powerful
and inspectable. AI should accelerate editing, but every AI action should remain
previewable, reversible, and understandable.

## UI Framework Direction

The editor should use open source libraries with permissive licenses. The user
selected this direction on 26 October 2026, superseding the earlier Qt proposal.
Use Dear ImGui docking and the existing GLFW and Vulkan integration for the
application shell. The detailed component, workspace, feature coverage and
license requirements are in [UI UX Design Guide](ui-ux-design-guide.md).

Follow the guide's established editor patterns: top workspace tabs, a left Scene
tree, a central viewport, a right contextual Inspector and task panels below.
Use Unreal-style synchronized selection and quick asset access, Blender-style
workspace layouts, and Painter-style material and texture organization. Apply
Revit project and property organization to BIM, 3ds Max numeric transforms and
view controls to scene editing, AutoCAD command and snap feedback to precision
tools, and Fusion contextual tool options to active tasks. Keep one shared
selection and command history. These are proposed UI requirements; the detailed
guide preserves each capability's implementation status.

Recommended stack:

- Dear ImGui (MIT): menus, toolbars, inspectors, virtualized tables and the
  docking shell, using a pinned docking revision.
- GLFW (Zlib): application windows, input, DPI and Vulkan surface integration.
- Application-owned workspace state: dockable and tabbed panels, saved layouts
  and workspace presets; floating windows require lifecycle validation.
- Optional ImGuizmo (MIT), ImPlot (MIT) and Native File Dialog Extended (Zlib):
  adopt for a specific need, with exact pins and packaged license notices.
- Vulkan renderer: central and secondary realtime viewports, player mode
  viewports, editor overlays, picking, gizmos, render targets, and final render
  output.
- Scene engine: scene graph or ECS, assets, serialization, cameras, lights,
  materials, BIM metadata, animation data, and runtime state.
- Viewport manager: creation, docking, layout state, render target ownership,
  per-view camera state, and per-view configuration.
- Editor command layer: undo/redo, command history, AI-proposed changes, and
  user-authored scene modifications.

The ownership boundaries should stay clear:

```text
Dear ImGui and GLFW own the desktop application shell.
ImGui docking and application workspace state own panel layout and presets.
Vulkan owns viewport rendering.
The viewport manager owns view instances, targets, and view configuration.
The scene engine owns scene data.
Editor commands own mutations and undo/redo.
AI produces proposed commands, not direct hidden scene edits.
```

This keeps the renderer independent from the UI framework and allows the same
scene engine to support both editor and player workflows.

## UX Principles

- Keep the current task obvious: the user should always know whether they are
  viewing the scene or editing it.
- Make everything selectable, inspectable, editable, reusable, and undoable.
- Separate navigation, selection, transformation, rendering, and AI actions so
  they do not fight for the same input.
- Prefer a simple default workspace with optional professional panels instead
  of showing every advanced feature at once.
- Treat AI output as proposed scene changes, not hidden automatic edits.
- Preserve renderer-engineering workflows such as debug views and performance
  inspection, but avoid exposing them as the main user path.

## Mode Model

### Player / Renderer Mode

Player mode is for consuming the scene.

Expected UX:

- Minimal chrome around the viewport.
- Camera navigation, scene navigation, and playback controls.
- Render-quality controls such as realtime, preview, and final render.
- Screenshot, recording, and render/export commands.
- Optional scene bookmarks or camera paths.
- Optional overlays for debug, statistics, or selected presentation metadata.
- No accidental editing of scene objects.

Useful controls:

- Play / pause scene simulation or animation.
- Switch active camera.
- Toggle walkthrough, orbit, and cinematic navigation.
- Adjust quality presets.
- Export still image, animation, or frame capture.
- Return to editor mode at the same camera position and selection context.

### Editor Mode

Editor mode is for authoring the scene.

Expected UX:

- A central viewport with professional 3D editing controls.
- A scene hierarchy for selecting and organizing objects.
- An inspector for transforms, materials, renderer settings, metadata, and
  custom properties.
- An asset browser for models, materials, textures, cameras, lights, prefabs,
  subscenes, and AI-generated assets.
- Toolbars for transform tools, snapping, coordinate space, pivots, overlays,
  and selection modes.
- Undo/redo and visible history for user and AI edits.
- A contextual AI assistant that can modify the current selection, create new
  scene content, organize assets, and propose improvements.

Editor mode should support direct manipulation and numeric precision. The user
should be able to drag a gizmo, type exact values, or ask AI to propose a
change.

## Recommended Workspace Layout

The default editor layout should be:

```text
Editor Main Window
| Menu Bar: File Edit Scene Add Render AI Window Help          |
| Toolbar: Select Move Rotate Scale Snap Mode                  |
|--------------------------------------------------------------|
| Hierarchy Dock | Vulkan Viewport              | Inspector    |
|                | - selection                  | Dock         |
| - World        | - transform gizmos           |              |
|   - Scene A    | - snapping overlays          | - transform  |
|   - Building   | - camera navigation          | - material   |
|   - Lights     | - debug overlays             | - metadata   |
|   - Cameras    |                              | - renderer   |
|--------------------------------------------------------------|
| Assets / Timeline / Console / History / AI Proposal Docks    |
```

This layout gives the user a familiar professional editor shape while keeping
the first screen focused on the actual scene, not a landing page or wizard.
The docking system should allow users to save and restore this layout as a
workspace preset.

## Multi-View Window Support

The editor should support multiple view windows, not only one central viewport.
Each view should be dockable, tabbed, split, or floating through the advanced
docking system.

Each view window should have its own view configuration:

- View identity: stable view id, title, docking state, and saved layout state.
- Target: full scene, subscene, prefab, selected object, render camera, asset
  preview, material preview, AI proposal preview, or debug render target.
- Camera: free editor camera, scene camera, orthographic top/front/side view,
  player camera, light camera, reflection probe, or custom saved camera.
- Render mode: lit, unlit, wireframe, normals, depth, material channels,
  lighting-only, post-process output, BIM metadata visualization, or final
  render preview.
- Renderer configuration: resolution scale, quality preset, samples, exposure,
  tone mapping, post-processing, overlays, clipping planes, section boxes, and
  debug flags.
- Interaction mode: editor selection, transform gizmo, orbit inspection,
  player simulation, locked presentation, or read-only preview.
- Synchronization: shared selection by default, with optional pinned selection,
  pinned camera, or independent time/playback state per view.

Example view layouts:

- Main perspective view plus top/front/side orthographic views.
- Editor viewport beside a player camera preview.
- Lit viewport beside wireframe, depth, normals, or render graph debug output.
- Current scene beside an AI proposal preview before applying changes.
- Building model view beside a floor plan, section view, or elevation view.
- Asset preview dock beside the main scene.
- Final render preview floating on a second monitor while editing continues in
  the main window.

Multi-view support should be handled through a dedicated viewport manager. The
renderer should receive explicit view descriptors instead of assuming a single
global framebuffer. This allows each view to have independent swapchain or
offscreen render targets while sharing scene data, GPU resources, and editor
selection state where appropriate.

## Scene Authoring Model

Use a scene model that supports nested, reusable content.

- Scene: a complete editable world or level.
- Subscene: a scene nested inside another scene.
- Prefab / Blueprint: a reusable object group with stable defaults.
- Instance: a placed copy of a prefab or subscene with local overrides.
- Component: a behavior, render property, metadata block, or capability
  attached to an object.

This supports workflows similar to:

- Unreal Engine levels and blueprints.
- Unity prefabs.
- Blender collections.
- Revit families and building metadata.
- Maya grouped assets and animation rigs.

The user should be able to add a subscene to another scene, select it as a
single object, expand it in the hierarchy, override local properties, and later
open it for deeper editing.

## Core Editor Workflows

### Add Content

The Add menu should support:

- Scene
- Subscene
- Empty object
- Mesh/model
- BIM/imported building model
- Light
- Camera
- Material
- Environment
- Animation/camera path
- AI-generated object or scene

Adding content should create a selected object, place it predictably, and show
its editable properties immediately in the inspector.

### Select and Inspect

Selection should work from both the viewport and hierarchy.

- Single click selects.
- Double click frames the object.
- Multi-select supports group operations.
- The inspector shows the combined editable properties for the selection.
- Metadata should be searchable and usable for BIM/Revit-style workflows.

### Transform and Organize

Editor mode should provide:

- Move, rotate, and scale gizmos.
- Local/world transform spaces.
- Pivot controls.
- Grid, angle, and surface snapping.
- Duplicate, group, parent, unparent, hide, lock, and rename.
- Layers, collections, or tags for large-scene organization.

### Test in Player Mode

Switching to player mode should be immediate.

- Keep camera position when switching.
- Preserve selected scene context when returning to editor mode.
- Prevent editing while in player mode.
- Make it easy to test lighting, navigation, animation, and final rendering.

## AI Assistant UX

AI should behave like a contextual editor assistant, not just a chat window.

Example workflow:

```text
User prompt:
"Add a modern glass office tower next to this building."

AI proposes:
+ New object group: Office_Tower_01
+ Materials: glass, steel, concrete
+ Lights adjusted for reflections
+ Camera marker added

Actions:
[Preview] [Apply] [Edit Prompt] [Reject]
```

AI edits should be represented as structured proposed changes:

- Created objects
- Deleted objects
- Modified properties
- Generated materials
- Imported assets
- Scene hierarchy changes
- Lighting/render setting changes
- Metadata changes

The user should be able to preview the result, inspect the diff, apply it,
reject it, or revise the prompt. Applied AI changes should become normal undo
steps in the history.

Useful AI commands:

- Generate object or scene.
- Modify selected object.
- Explain selected object.
- Optimize scene.
- Improve realism.
- Convert blockout/sketch content into detailed scene content.
- Suggest lighting.
- Find problems.
- Create a Revit-like floor plan from a model.
- Generate or assign materials.
- Batch rename or organize the hierarchy.
- Create animation or camera paths.

## Workspace Presets

Start with one simple editor layout, then add workspace presets as features
grow:

- Layout: general scene arrangement and hierarchy work.
- Modeling: mesh and object editing.
- Architecture: BIM, Revit-like metadata, floors, rooms, and measurements.
- Lighting: lights, environment, exposure, and render previews.
- Animation: timeline, keyframes, cameras, and playback.
- Simulation: physics, interactive state, and runtime behavior.
- Rendering: render graph, quality, passes, and export.
- AI Assist: prompt history, generated assets, proposals, and scene analysis.

Workspace presets should rearrange tools without changing the underlying scene
or hiding essential mode controls.

## Implementation Phases

### Phase 1: Mode Foundation

- Create the Dear ImGui and GLFW application shell using permissive libraries.
- Integrate a pinned Dear ImGui docking revision and application workspace state.
- Embed the Vulkan renderer as the central viewport.
- Add a viewport manager that can create one or more docked/floating view
  windows.
- Store per-view camera, target, render mode, and quality configuration.
- Add explicit Editor and Player modes.
- Add a visible mode switch.
- Define input ownership for mode-specific controls.
- Preserve camera state across mode switches.
- Disable object editing in player mode.

### Phase 2: Basic Editor Shell

- Add scene hierarchy panel.
- Add object selection.
- Add inspector panel for transform and basic render properties.
- Add Add menu for lights, cameras, imported models, and empty objects.
- Add undo/redo for editor actions.

### Phase 3: Scene Composition

- Add subscene and prefab concepts.
- Add instancing and local overrides.
- Add hierarchy organization tools.
- Add save/load support for editor-authored scene composition.

### Phase 4: Professional Tools

- Add transform gizmos.
- Add snapping and measurement tools.
- Add multi-view presets such as perspective plus top/front/side orthographic
  views.
- Add per-view render modes, overlays, clipping planes, and debug targets.
- Add material editing.
- Add camera path and timeline tools.
- Add workspace presets.

### Phase 5: AI-Assisted Editing

- Add contextual AI panel.
- Represent AI output as structured proposed scene changes.
- Add preview/apply/reject workflow.
- Add AI changes to undo/redo history.
- Add scene analysis, cleanup, and optimization commands.

## Success Criteria

- A new user can open the app, load a scene, switch between player and editor
  modes, add content, select it, inspect it, modify it, and return to player
  mode without losing context.
- A technical user can still access renderer debugging and performance tools.
- Users can open multiple docked or floating view windows with different
  targets, cameras, render modes, overlays, and quality settings.
- Large scenes remain manageable through hierarchy, search, organization,
  metadata, and subscene composition.
- AI accelerates authoring without reducing user control or making silent,
  destructive changes.
