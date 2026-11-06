# VulkanSceneRenderer UI UX Design Guide

Version 1.1 | 28 October 2026 | Audience: designers and application contributors

## 1 Purpose and authority

Design one professional desktop application that makes the renderer's existing scene, BIM, lighting, animation, physics, volume and diagnostic workflows discoverable. The viewport remains central, with contextual tools around it. A material inspector alone does not represent this application.

This guide is the design reference for future UI work. It specifies the intended experience, component rules, feature placement and acceptance gates. It does not claim that a redesigned shell, docking, multiple views or the remaining authoring features already ship. The repository and recorded runtime validation remain authoritative for implementation status.

**User requirements:** cover previously developed features and UI elements; adopt familiar designs from successful editor software; use open source libraries with permissive licenses; do not use Qt. This framework decision supersedes the earlier Qt direction in [Player and Editor UX Plan](player-editor-ux-plan.md). Preserve that plan's shared engine, Editor and Player modes, reversible commands and future view ownership model.

The earlier in-chat mockup was an HTML/CSS/JavaScript interaction concept for materials. It was neither a screenshot nor a replacement UI implementation. Its narrow feature coverage and simulated rendering must not become the specification for this redesign.

Use these status labels in design reviews:

| Status | Meaning |
| --- | --- |
| Existing | A corresponding implementation or current UI route is present in the inspected worktree. Release validation still needs its own evidence. |
| Integration | Source work is being connected or tested; do not present it as a completed release feature. |
| Proposed UI | A new presentation of existing capabilities, including docking and workspace layouts. |
| Planned capability | The underlying feature is incomplete or absent. Show it in planning material, not as a working control. |

## 2 Permissive library architecture

Retain the existing C++ Vulkan application and its GLFW and Dear ImGui integration. Build the professional shell with a pinned Dear ImGui docking revision, application-owned panels and a shared command layer. Keep the renderer independent from the panel layout. A modern appearance comes from consistent components, typography, spacing, behavior and information hierarchy.

| Library | Role | Verified upstream license | Adoption rule |
| --- | --- | --- | --- |
| Dear ImGui | Main shell, docking, menus, tables, inspector and toolbars | MIT | Reuse existing integration; audit docking support before enabling it. |
| GLFW | Windows, input, DPI and Vulkan surface integration | Zlib | Reuse the current dependency and input ownership. |
| ImGuizmo | Optional transform gizmo implementation | MIT | Adopt only if it preserves existing picking, snapping and history contracts. |
| ImPlot | Optional frame timing and performance plots | MIT | Use for real measurements; do not introduce decorative metrics. |
| Native File Dialog Extended | Optional native open and save dialogs | Zlib | Return typed requests to the application; dialogs do not mutate scene state. |
| Lucide | Optional coherent icon assets | ISC plus retained MIT notices for Feather-derived icons | Preserve the complete upstream license and inherited notices. |

Licenses were checked against upstream files on 26 October 2026. Before adding a dependency, pin an exact version or commit, recheck that revision's license and transitive dependencies, record it in third-party notices and package the required notices. Do not introduce GPL, LGPL, AGPL, source-available, paid-only or proprietary UI dependencies into this stack. This constraint concerns the proposed UI stack; it does not relabel existing engine dependencies.

Prefer application-owned small components over a large collection of unmaintained widget packages. Every optional library needs a concrete role and a fallback. Use system-installed fonts without redistributing them; a bundled font requires its own recorded redistribution license. No runtime web service is required for the shell.

Upstream license sources: [Dear ImGui](https://github.com/ocornut/imgui/blob/master/LICENSE.txt), [GLFW](https://github.com/glfw/glfw/blob/master/LICENSE.md), [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo/blob/master/LICENSE), [ImPlot](https://github.com/epezent/implot/blob/master/LICENSE), [Native File Dialog Extended](https://github.com/btzy/nativefiledialog-extended/blob/master/LICENSE), [Lucide](https://github.com/lucide-icons/lucide/blob/main/LICENSE). See the official [Dear ImGui docking guide](https://github.com/ocornut/imgui/wiki/Docking) for integration details.

## 3 Product and workspace model

Use one shared scene engine, one selection context and one history. Rendering technique, workspace, interaction mode and display visualization are different concepts and need separate controls.

| Control | Values or scope | What it changes |
| --- | --- | --- |
| Interaction mode | Editor, Player | Whether scene authoring is permitted. |
| Workspace | Layout, Materials, Lighting, BIM, Animation, Simulation, Volumes, Diagnostics | Which panels are arranged around the viewport. |
| Render technique | Deferred raster, Forward rendering | The implemented rendering path. |
| Display visualization | Supported lit and diagnostic views | What the viewport displays. |
| Playback source | Animation clip, physics, volume sequence | Which independent clock a transport control operates. |

Standalone ray tracing, path tracing, Gaussian splatting and radiance-field techniques are currently unavailable. Ray-query shadows and DDGI belong to the existing raster paths. Do not label them as additional completed renderers. Switching a workspace must never switch techniques or silently change lighting, camera, selection or simulation state.

### Established editor patterns

Use Blender, Unreal Editor, Substance 3D Painter, Revit, 3ds Max, AutoCAD and Fusion as interaction references. Reproduce their familiar panel relationships and task flows in the application's permissive UI stack. The application choices below are proposed UI requirements; the feature preservation map continues to determine which underlying capabilities are existing, in integration or planned.

| Reference | Established pattern | Application choice |
| --- | --- | --- |
| [Blender workspaces](https://docs.blender.org/manual/en/5.1/interface/window_system/workspaces.html) | Topbar workspace tabs select task-specific arrangements of editors. Its Layout workspace combines a viewport, Outliner, Properties and Timeline. | Put named workspace tabs across the top. Keep scene browsing, a central viewport and contextual properties together; open the relevant transport or diagnostics below. |
| [Unreal Editor interface](https://dev.epicgames.com/documentation/unreal-engine/unreal-editor-interface?lang=en-US) | Viewport and Outliner share selection. Details exposes properties of that selection. A temporary Content Drawer gives quick access to assets. | Synchronize the Scene tree and viewport selection with the right Inspector. Offer a bottom Assets drawer for quick access and allow the asset browser to stay pinned as a dock. |
| [Painter Texture Set list](https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/interface/texture-set/texture-set-list) and [channel view](https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/interface/layer-stack/layer-stack) | Selecting a material ID focuses its settings; channel selection narrows the displayed material context. | In Materials, show the selected surface, shared material and user count above grouped PBR factors and texture slots. Channel filters focus the supported slots. Painting and layer compositing are separate capabilities from this inspector design. |

### CAD and BIM interaction patterns

Apply the Autodesk references to precision, project organization and contextual tools. Keep the same Scene, Inspector and lower-dock framework in every workspace. Adopting these interactions does not add parametric BIM authoring, modifier stacks, sketch constraints or a CAD solid kernel to the tracked renderer capabilities.

| Reference | Established pattern | Application choice |
| --- | --- | --- |
| [Revit interface tour](https://app.learn-one.autodesk.com/learn/ondemand/tutorial/revit-user-interface-tour) | A sortable, filterable Project Browser navigates project views. The Properties palette presents selected-element parameters, or current-view properties when nothing is selected. | Give BIM a Project browser organized by model, storey, category and saved view. Keep selection properties and view settings explicitly labeled in the Inspector; retain schedules, relationships, filters and units. |
| [3ds Max viewport layouts](https://help.autodesk.com/cloudhelp/2026/ENU/3DSMax-Basics/files/GUID-26518D1C-3F68-4D77-AAD1-3A78F0948B5C.htm) and [Transform Type-In](https://help.autodesk.com/cloudhelp/2026/ENU/3DSMax-Basics/files/GUID-B9779BBF-5B04-44EF-95EC-465DD7267707.htm) | Labeled viewports provide view choices. Numeric transform entry supports precise values and offsets in a stated coordinate system. | Keep perspective and orthographic choices, navigation controls and active-view identity visible. Pair gizmos with precise X Y Z entry, units, Local or World labels and explicit absolute or offset operation. Multiple viewports retain their proposed status. |
| [AutoCAD command entry](https://help.autodesk.com/cloudhelp/2026/ENU/AutoCAD-Core/files/GUID-BB0C3E79-66AF-4557-9140-D31B4CF3C9CF.htm) and [object snaps](https://help.autodesk.com/cloudhelp/2026/ENU/AutoCAD-Core/files/GUID-76B81C1A-373E-4BCD-975A-789FB36C89FE.htm) | Search suggestions help discover commands. Snap markers and tooltips identify precise point targets; persistent and one-point snaps have distinct behavior. | Make command search show action names and current options. In supported transform and measurement tools, show snap type, target marker and numeric precision. Separate persistent snapping from a temporary override and keep the next required input visible. |
| [Fusion workspaces](https://help.autodesk.com/cloudhelp/ENU/Fusion-GetStarted/files/GS-WORKSPACES.htm) | Purpose-driven workspaces group related tools; contextual tabs appear while their associated command is active. | Keep navigation and selection available, then add focused controls for active tools such as Measure or Section. Show the active tool, its options and Finish or Cancel in a consistent place; return to the previous workspace context when it ends. |

The default composition below combines these references into one consistent editor. Keep the Scene tree on the left and the Inspector on the right across workspaces. Keep common names such as Scene, Assets, Inspector, Timeline, History and Diagnostics. Use one application component style for all panels, with consistent spacing, icons and focus states.

### Familiar workflows

1. **Select and inspect.** Selecting in the viewport or Scene tree reveals the same object in the Inspector. Keep its name and selection breadcrumb visible while changing property groups. Selecting a material surface explicitly identifies the binding being edited.
2. **Assign or isolate a material.** Show the current material name, a searchable material picker, the shared-user count and Make Unique together. Assign changes the selected surface binding; shared-property edits identify all affected users. Make Unique creates an independent definition and leaves the selected surface in context.
3. **Edit a texture slot.** Each supported role has a named slot, thumbnail or empty state, file identity and replace action. Open preview, color-space information, UV controls and sampler settings from that slot. Keep failed imports visible without losing the previous texture. Use the capability status while these authoring actions remain incomplete.
4. **Animate.** Switching to Animation brings the clip list and timeline into the lower dock while preserving scene selection and camera. The transport names the active clock. Playback controls stay distinct from future keyframe and curve authoring tools.
5. **Inspect rendering.** Switching to Diagnostics reveals pass and resource lists with CPU and GPU timing below. Selecting a pass focuses its real parameters and measurements. Keep this engineering detail in its task workspace.
6. **Find and restore tools.** Menus show shortcut labels and search offers the same commands. Persist panel sizes, docking and the active workspace. Restore Workspace returns that preset's panels without changing scene data, camera or selection. A quick Assets drawer and a pinned Assets dock share the same asset browser and selection context.
7. **Navigate a BIM project.** Choose a model, storey or saved view from the Project browser, then inspect the selected elements or the active view. Keep category filters and hidden-state reasons visible. Show schedules and measurements in the lower dock while retaining the same scene selection.
8. **Place and measure precisely.** Choose the coordinate space and units, enter an absolute value or offset, and use the supported snap targets with visible feedback. Numeric entry and dragging use the same reversible command. Tool prompts identify the next point or value; Finish and Cancel have consistent outcomes.

### Default editor composition

```text
File  Edit  Scene  Add  View  Render  Tools  Window  Help
Project and save state | Workspace | Editor or Player | Commands
Select Move Rotate Scale | Local World | Snap | Renderer
----------------------------------------------------------------
Scene and Assets |             Viewport             | Inspector
Search and tree  | Camera  Display  AA  Overlays     | Selection
Scenes and layers|                                  | Properties
Materials        |          Scene content           | Material
Lights and cameras|                                  | Metadata
----------------------------------------------------------------
Animation | BIM tables | History | Jobs | Diagnostics | Proposals
Selection and units | Last operation | Frame timing | Job status
```

At 1440 by 900 logical pixels, start with a 264-pixel left dock, a 344-pixel right dock, a 44-pixel main toolbar and a collapsed lower dock. Open a lower dock to approximately 160 pixels when the task needs it. These are initial layout targets, not fixed dimensions. Splitters, user font scale and minimum viewport area take priority.

At 1280 by 720, tab Scene and Assets together, keep one contextual inspector, and open bottom panels on demand. At smaller window sizes, collapse one side dock before shrinking controls. Do not squeeze every feature into permanently visible panels. Save named layouts and provide a reliable Restore workspace command. Restoring panels changes layout only.

### Workspace presets

| Workspace | Left dock | Inspector | Lower dock |
| --- | --- | --- | --- |
| Layout | Scene, Assets | Transform and object properties | History on demand |
| Materials | Material users and assets | PBR groups and texture slots | Resource diagnostics on demand |
| Lighting | Lights and cameras | Light, shadows, environment and GI | Probe diagnostics on demand |
| BIM | Spatial structure, filters and selection sets | Properties and relationships | Quantities, measurements or BCF topics |
| Animation | Scene and camera list | Selected animation target information | Clip playback timeline |
| Simulation | Bodies and joints | Body or joint parameters | Physics transport and diagnostics |
| Volumes | Grids, sources and outputs | Volume appearance or operation parameters | Operations, sequences and jobs |
| Diagnostics | Render resources and passes | Selected pass or resource | CPU and GPU timing, graph and logs |

Player mode retains camera navigation, selected camera, scene playback, quality and supported capture actions. Hide authoring gizmos and disable authoring commands. Return to Editor without discarding the user's camera and selection context. This complete mode model is proposed; current Editor and Render Preview viewport controls are the migration starting point.

## 4 Feature preservation map

Every existing control needs a destination before the old Scene Controls window is retired. Searchability supplements a visible home; a command palette must not be the only way to find a capability.

### Project and scene authoring

| Capability to preserve | New home | Required behavior |
| --- | --- | --- |
| Model path, sample model search, import scale, load, default reload and unload | File menu and Assets dock | Distinguish replacing the primary model from merging content. Show import warnings and progress. |
| Import and merge glTF or GLB, parent label and incoming scene | Import dialog | Keep the current scene on failure. Name the merge parent and make undo discoverable. |
| Save and Save As | File menu and project header | State the output format and save scope. Expose failures without clearing the dirty state. |
| glTF scene selection and material variants | Scene dock header and Inspector | Preserve authored identities and show Default explicitly. |
| Hierarchy selection, framing, rename, empty, duplicate, delete and reparent | Scene tree, context menu and Scene menu | Preserve stable IDs, inline F2 rename and undo semantics. |
| Visible, selectable and hoverable flags | Separate tree columns and object properties | Show inherited state and local state separately. Do not merge the three flags into one lock. |
| Quad, plane, cube, sphere, triangular prism, pyramid and torus | Add menu | Select the created object and reveal its transform. |
| Object and root transforms, local and world space, axes and snapping | Viewport toolbar and Inspector | Direct manipulation and numeric entry use the same command path. |
| History | Edit menu and History dock | Name the affected action and object. Preserve one drag as one history step. |

### Materials and animation

| Capability | New home | Status and design requirement |
| --- | --- | --- |
| Imported PBR and extension material inspection | Material Inspector | Existing rendering support; identify source material and actual surface binding. |
| All 32 registered numeric properties and 13 known extension toggles | Grouped Material Inspector | Integration work in #108. Show common factors first and conditional extension groups below. |
| Name, alpha mode, cutoff, sidedness and schema defaults | Material Inspector | Integration work. Offer numeric precision and per-property reset. |
| Shared material user count and binding chooser | Inspector header | Integration work. Group selection must reveal which renderable surface is being edited. |
| Assign, duplicate, isolate, create and remove material resources | Assets and Material Inspector | Planned or incomplete #108 scope. Do not show these as working actions yet. |
| Texture import, replacement, samplers and unused-resource deletion | Texture slot editor and Assets | Planned or incomplete #108 scope. Preserve imported bindings meanwhile. |
| Rest pose, clip selection, play, pause, loop, signed speed and time scrub | Animation dock | Existing playback controls. State the selected clip and clock explicitly. |
| Authored camera selection and playback diagnostics | Viewport camera menu and Animation dock | Preserve current camera association and show diagnostics on demand. |
| Keyframes, curve editing and camera-path authoring | Future Animation tools | Playback does not establish these authoring capabilities. |

### Lighting and rendering

| Capability to preserve | New home | Required behavior |
| --- | --- | --- |
| Forward and deferred technique selection | Viewport or Render toolbar | Show unavailable modes with a reason in an optional capabilities view. |
| Perspective, orthographic, authored cameras, standard views, orbit and framing | Viewport camera menu and navigation widget | Keep a clear active camera label. Preserve Revit and Blender navigation choices. |
| Editor and Render Preview, MSAA and display visualization | Viewport toolbar | Separate interaction, sampling and visualization controls. |
| Vertex and light overlays, normal validation and wireframe debug | Overlays popover | Preserve width, depth test, intensity, color and diagnostic settings. |
| Directional, point, spot and area lights | Lights list and Light Inspector | Keep imported and manually added lights distinguishable. |
| Directional, environment and bounce intensities, area integration and emitter geometry | Lighting Inspector | Use physical quantities where the backend defines them; label scene units elsewhere. |
| Raster shadows, hard and soft ray queries, denoise, local budgets, biases, PCSS and cascades | Shadows panel | Show effective mode and device support; retain all advanced quality controls. |
| DDGI enable, pause, glass transport, scrolling and async updates | GI panel | Distinguish enabled, supported, active and paused states. |
| DDGI probe budgets, rays, biases, hysteresis, visualizations, scene and detail volumes | GI panel and diagnostic drawer | Preserve fit to scene or selection, Apply, Cancel and preset save/load. |
| Manual and automatic exposure, metering and adaptation | Post process panel | Common exposure first; advanced metering in a disclosure. |
| TAA, history controls, reset reason and MSAA interaction | Anti aliasing panel | Explain the current TAA requirement for MSAA 1x at the disabled control. |
| Bloom threshold, knee, intensity and radius | Post process panel | Preserve neutral or disabled behavior. |
| Optional and protected render passes | Diagnostics workspace | Explain dependencies and protected passes instead of allowing an invalid configuration. |
| Environment status and existing IBL intensity | Lighting panel | HDR file browsing, rotation and physical sky remain tracked work; do not invent existing controls. |

### BIM coordination

| Capability to preserve | New home | Required behavior |
| --- | --- | --- |
| Element browser, selection, focus and type counts | BIM Elements dock | Virtualized rows, stable identity and selection synchronization. |
| Import information, warnings, georeference and units | Model Information panel | Keep original and display units visible; show precision and origin context. |
| Search, type, storey, material and related filters | BIM Filters dock | Removable active-filter chips and a clear reset action. |
| Spatial browser, storeys, floor slicing and plan overlays | Spatial dock and viewport view controls | Selection, floor visibility and camera projection remain separate. |
| Semantic colors and legend | Visualization controls | Label the active coloring mode and retain a readable legend. |
| Properties, extended properties and IFC relationships | BIM Inspector | Searchable property names, typed values and navigable related elements. |
| Schedules, quantities and model comparison | BIM Tables dock | Show baseline and current model identities and comparison status. |
| Selection sets and member lists | Selection Sets panel | Keep membership distinct from temporary selection. |
| Phase timeline, future ghosts and layer visibility | BIM timeline and Layers panel | Explain why an element is hidden; avoid silently clearing filters. |
| LOD and streaming | BIM model performance panel | Show real runtime statistics and bounded controls. |
| Section plane, section box, caps, hatch styles and markers | Section tool and Inspector | Keep clipping drafts, applied clipping and overlays understandable. |
| Elevation views and SVG drawing export with paper scale | Views and Drawing Export dialog | Clearly state vector output, scale and source view. |
| Measurements, snapping, A B C points and annotations | Measure tool and Measurements dock | Preserve measured units, selected annotation and precise values. |
| Viewpoints, BCF topics, archive or folder import and export, comments and labels | Coordination dock | Preserve viewpoint restoration and distinguish topic metadata from scene edits. |

### Physics and volumes

| Capability to preserve | New home | Required behavior |
| --- | --- | --- |
| Static, kinematic and dynamic bodies; attach, replace and detach | Physics Inspector | Show selected body's status, collision representation and attachment target. |
| Mass, friction, restitution, gravity and continuous collision | Physics Inspector | Validate before replacing a live body. Label units and restrictions. |
| Play, pause, single step, reset, clear bodies and create demo | Physics transport | Keep reset-to-attachment behavior explicit. Do not merge this transport with clip playback. |
| Fixed, hinge and slider joints, world or body anchoring | Joints panel | Show both endpoints and selected joint. Preserve axes, limits and collision options. |
| Position and velocity motors, force or torque, frequency and damping | Joint Inspector | UI degrees and capture/API radians must be converted and labeled correctly. |
| OpenVDB visibility, geometry shadows, density and scattering | Volume Inspector | Describe current multiple-scattering controls according to their actual approximation. |
| Volume emission, temperature and ray step | Volume Inspector | Show temperature controls only when the selected grid supplies temperature. |
| Volume sequence frame, pause, loop, speed, cache and loading state | Volume transport and Jobs dock | Distinguish requested, loaded and displayed frame indices. |
| Existing OpenVDB operation selector and typed parameters | Volumes Tools dock | Organize by task family while preserving current operation names and limits. |
| SDF, mesh, particles, Booleans, filters, morphing, fracture and queries | Geometry and SDF tools | Reuse validated operation routes rather than duplicating algorithms in widgets. |
| Point attributes and groups, transfer and advection; scalar and vector fields | Points and Fields tools | Preserve grid selectors, data types, bounded inputs and report output. |
| Dense arrays, multiresolution data, NanoVDB interchange and statistics | Data and Interchange tools | Show format, integrity and unsupported-feature diagnostics. |
| Smoke, sources, viscosity, projection and physical image jobs | Simulation and Transport tools | Reflect the feature ledger's partial and pending boundaries. |
| Points, glyphs, streamlines, slices and native point previews | Preview selector | Show exactly which output is displayed and how it was generated. |
| Run operation, sequence progress, cancellation, restart and preview loading | Jobs dock | Sequence cancellation exists; do not promise universal cancellation of every operation. |

The volume operation list is evolving. Do not freeze a count into navigation or claim every upstream OpenVDB function is supported. The inspected source and the latest validated ledger have different operation counts. [OpenVDB feature status](openvdb-feature-status.md) remains the capability boundary, including pending AX and partial solver, transport and authoring families.

### Diagnostics and output

| Capability to preserve | New home | Required behavior |
| --- | --- | --- |
| FPS, CPU frame time, known GPU time, p95 and history | Diagnostics summary | Label sampling windows and incomplete GPU timing coverage. |
| CPU phases, workload, culling, render graph and synchronization | Diagnostics tabs | Keep exact pass names, source of timing, latency and availability. Preserve Freeze culling camera and its F8 shortcut alongside frustum and occlusion counts. |
| Import, shader, resource and operation messages | Logs and Jobs dock | Show actionable errors and paths with copy and reveal actions where supported. |
| Existing scene and drawing exports | File and Export dialogs | List what each format actually preserves. |
| Deterministic capture sequences and regression tooling | Tools entry and future job integration | CLI support alone must not be presented as an existing interactive capture queue. |
| GFXReconstruct runtime capture status | Capture diagnostics | Current capture must be armed before Vulkan instance creation. Do not draw an unsupported live Record toggle. |
| HDR image output and other unfinished capture features | Planned output section | Keep their tracker status visible in design specifications. |

## 5 Screen and interaction specifications

### Scene tree and Inspector

The tree is a working surface, with search above it and object actions in its context menu. Use icon, name and distinct visibility, selectable and hoverable columns. Reveal the selected row even when most rows are clipped. Search must preserve a path to matched descendants. F2 starts inline rename; Enter commits and Escape restores the prior name. Keep long imported labels readable through tooltips and a copy-name action.

The Inspector header shows the selected object, its type and a breadcrumb. Sections depend on selection: Transform, Material, Light, BIM, Physics or Volume. Do not show every section empty. If inspector pinning is added, label the pinned object explicitly so viewport selection does not appear broken. Mixed-selection editing is future work unless backed by real multi-object commands; do not simulate it by editing only one object.

### Material authoring

Show a surface binding chooser for groups, the material name, shared-user count and the editing scope. Place Base color, Metallic and Roughness first, followed by Emission, Alpha and Sidedness. Put extension properties in named expandable groups with the enable control and its parameters together. Numeric controls support dragging and exact typing. Use schema defaults for Reset, including omitted optional leaves and unlimited attenuation distance.

When a curve controls a factor, show its authored rest value and current evaluated value separately. Label an edit as changing the rest value; it must not silently rewrite animation keys. Disallow unsafe removal of extensions referenced by animation and explain the affected binding.

The future texture slot component contains a thumbnail, slot name, source path or embedded identity, color or data role, UV set, transform and sampler summary. Replace opens a validated staging flow. Loading and failure states retain the old texture preview. Apply changes all shared users unless the user first chooses Make unique. Import, assign, duplicate, detach and delete must be separate commands with correct reference checks. These texture controls are a specification for remaining #108 work, not a completed UI claim.

### BIM coordination

Opening BIM mode exposes spatial structure and filters without hiding the selected element's properties. The lower dock switches among Quantities, Compare, Measurements and BCF Topics. A selected topic can restore a saved viewpoint; make that camera and clipping change explicit. A section tool displays its manipulator in the viewport and its exact values in the Inspector. Keep model units beside measurement and paper-scale controls.

### Simulation and playback

Each transport names its clock: Animation clip, Physics or Volume sequence. Show the current source, play state and reset meaning. Playback must not steal keyboard input from text entry. A physics body edit during simulation follows the existing pause and synchronization policy. Destructive body or joint changes disclose affected connections, with unsupported actions disabled for a specific reason.

The animation timeline initially supplies clip playback and scrubbing. Do not show editable keyframe tracks until keyframe authoring exists. Volume timeline controls show cache and displayed-frame information so asynchronous updates do not appear as a wrong scrub position.

### Lighting and diagnostics

Common lighting controls remain near the viewport; resource budgets and probe internals live in advanced groups. DDGI changes that need rebuilding use a draft with Apply and Cancel. A telemetry refresh must never erase an unfinished draft. Show an effective-state summary such as Enabled but unavailable with a clear reason.

Diagnostics is a task workspace, not a permanent wall of counters. Plot actual frame timing with ImPlot if adopted. Selecting a render pass reveals dependencies, settings and measured results. Keep engineering resource IDs and raw allocation details inside diagnostics rather than ordinary material or BIM workflows.

### Import and save

Import dialogs distinguish Replace primary model and Merge into scene before the user chooses options. Display format support, incoming scene and import scale with units. Preserve the old scene until preparation succeeds. Keep warnings accessible after the dialog closes.

Save and Save As must disclose scope. Existing glTF export, BIM drawings, BCF topics, GI presets and volume files have separate output routes. Physics scene serialization and comprehensive session or renderer-preference persistence must be specified separately when implemented. Do not imply that Save scene serializes every open tool and simulation. New workspace persistence may store layout and view preferences separately from authored assets.

## 6 Visual design system

Use a quiet neutral interface, restrained blue active states and the established yellow viewport selection. Favor alignment and typography over nested cards and decorative borders. Support both dark and light themes, with the same hierarchy and behavior.

| Token | Dark theme starting value | Light theme starting value |
| --- | --- | --- |
| Window background | #15171C | #F4F6F9 |
| Panel surface | #1D2027 | #FFFFFF |
| Raised surface | #272C35 | #E9EDF3 |
| Main text | #E7ECF4 | #202633 |
| Secondary text | #AAB4C2 | #566174 |
| Active control accent | #78AFFF | #205CCA |
| Structural divider | #3A414D | #CAD2DE |
| Viewport selection | #FFCE45 | #A17100 |

These are initial design tokens. Measure contrast in every actual normal, selected, hover, error and disabled state before accepting a theme. Do not assume all token combinations are valid.

Use a 4-pixel spacing grid: 4 within tight controls, 8 between related fields, 12 within sections, 16 between groups and 24 between major regions. Default UI text is 14 logical pixels; secondary information is at least 12. Use a readable system sans serif, tabular digits for changing numerical values, and monospace only for IDs, paths and logs. Avoid all-caps paragraphs and low-contrast labels.

Default desktop input rows are 28 to 32 logical pixels high. Primary actions are 32 to 36 pixels. Small icons can be 16 to 18 pixels inside larger hit regions. Use consistent 4-pixel control corners and 6-pixel popover corners. Reserve stronger borders for focus, selection or validation.

The yellow object outline must remain a continuous, symmetric stroke with equal horizontal and vertical thickness in framebuffer pixels. Preserve the currently validated five-pixel stroke behavior in both raster paths. Do not replace it with a small set of visibly separated offset copies. UI DPI scaling and viewport framebuffer dimensions must be handled independently.

Use familiar, consistent icons with text for primary actions. An unlabeled icon cannot be the only route to an important capability. Tooltips describe the action, shortcut and unavailable reason; they do not contain essential instructions that disappear on keyboard or touch use.

## 7 Input accessibility and error states

Use logical Tab and Shift Tab navigation, arrow navigation inside lists and menus, visible focus and predictable Enter and Escape behavior. Keep application shortcuts inactive while a text field or numeric editor owns the same input. Provide explicit controls for actions otherwise available only by dragging.

Adopt WCAG 2.2 as a measurable design benchmark: normal text contrast at least 4.5 to 1; large text at least 3 to 1; pointer targets at least 24 by 24 logical pixels or adequate separation, with 44 by 44 targets preferred for touch-focused layouts. Native desktop accessibility still requires explicit testing. ImGui keyboard support alone does not establish screen-reader or Windows UI Automation support; plan an accessibility adapter and test it before making a compliance claim.

Sources: [W3C contrast guidance](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html), [W3C target size guidance](https://www.w3.org/WAI/WCAG22/Understanding/target-size-minimum.html), [Microsoft keyboard interactions](https://learn.microsoft.com/en-us/windows/apps/develop/input/keyboard-interactions).

Keep these states visually and behaviorally distinct:

| State | Presentation and recovery |
| --- | --- |
| No selection | Explain what can be selected; do not fill the Inspector with inactive fields. |
| Empty scene | Offer Load, Merge or Add primitive and retain recent-source access. |
| Busy operation | Show stage and progress when known; offer only cancellation that is actually supported. |
| Invalid draft | Keep the draft and error beside the relevant field; retain applied state. |
| Unsupported device or build | State the missing capability and the active fallback. |
| Stale target | Reject the old request and refresh the binding; never edit a new selection accidentally. |
| Save or import failure | Preserve live content and unsaved changes; show a useful diagnostic and retry route. |
| Success | Use a brief status message; do not interrupt editing with a modal. |

Preserve the existing Ctrl Y and Ctrl Shift Z redo alternatives, F2 inline rename and F8 culling-camera freeze. Additional shortcut assignments are proposals until audited against current input bindings: Ctrl S Save, Ctrl Shift S Save As, Ctrl Z Undo, Delete Delete selection, F Frame selection and Ctrl P Commands. Tool shortcuts must respect existing navigation profiles; do not assign a key merely because another application uses it.

## 8 Command and runtime contracts

Widgets emit typed intents. The application validates identities and capabilities, prepares changes, retires GPU readers when needed and publishes one coherent result. A property editor must not directly mutate resident GPU resources or model data from its draw function.

One drag produces one undo entry. Begin previews, Update changes the preview, End commits the command and Escape cancels to the command's original state. A failed new edit must preserve the previous pending gesture and history cursor. Undo and redo of animated material rests must evaluate at the current clock while retaining authored curves and untouched values.

UI requests carry stable graph, asset, selection, binding and revision identity as appropriate. Labels and row indices are display information, not mutation authority. Refreshing telemetry or switching panels must not discard active text, numeric or probe-volume drafts. Finish or cancel drafts deliberately when their owning context changes.

Large scene trees, material lists, BIM elements, relationships, properties and job results use virtualized or paged views. Cache compact presentation snapshots by relevant revision. Do not clone full glTF models, animation libraries or volume data just to draw a panel. Keep filtering and background work bounded, and make main-thread publication explicit.

Floating ImGui tool windows and multiple independent Vulkan scene views are different features. Add tool-window platform support only after lifecycle and DPI tests. Add independent scene views through a Viewport Manager with stable view IDs, owned targets, per-view camera and explicit shared versus per-view settings. Both capabilities require implementation and validation; a split visual mockup proves neither.

## 9 Implementation sequence

1. Inventory every current control and assign it a destination using this map. Record the source route, command, persistence scope and capability status.
2. Establish permissive dependency pins, theme tokens and reusable field, row, section, status and toolbar components. Preserve existing behavior.
3. Add one editor dockspace with the current single Vulkan viewport, Scene, Inspector and a tabbed lower dock. Keep a temporary route to unmigrated controls.
4. Move scene loading, hierarchy, transforms, history and save workflows first. Validate input ownership and selection synchronization.
5. Migrate Materials, Lights, Shadows, DDGI, exposure, TAA, bloom and overlays without dropping advanced settings.
6. Migrate the full BIM workspace, including drawing export, comparison, quantities, relationships, phases, layers, clipping, measurements and BCF.
7. Migrate independent animation, physics, joint and volume transports. Move data operations into Volumes Tools and Jobs with accurate cancellation states.
8. Migrate diagnostics and capture status. Add workspace persistence and keyboard discovery after every panel has a stable identity.
9. Implement the remaining material, camera, environment and output capabilities through their tracker issues. Add UI only when real commands and failure handling exist.
10. Add independent views, fuller Player mode, reusable scene composition and previewable AI proposals in later bounded phases. They remain future capabilities until demonstrated.

Do not combine a shell migration with a renderer rewrite. Keep forward and deferred feature parity, imported resources and tested editor semantics intact. Remove the old monolithic Scene Controls window only after feature-by-feature parity is verified.

## 10 Acceptance checklist

- [ ] Every feature in the preservation map has a visible home, command route and tested capability state.
- [ ] The UI dependency graph uses the approved permissive licenses, exact pins and packaged notices; no Qt dependency is introduced.
- [ ] Scene replacement, merge, rename, create, duplicate, delete, reparent, flags, transforms, undo and save retain their current behavior.
- [ ] Both raster paths show consistent selection strokes, material appearance, transparency and shadow behavior after edits.
- [ ] Shared material scope, animation-controlled values and texture import failure states are understandable and correct.
- [ ] All BIM browsers, filters, quantities, comparison, sections, measurements, drawings and BCF workflows remain reachable.
- [ ] Animation, physics and volume transports have explicit independent clock labels and correct reset behavior.
- [ ] Every migrated OpenVDB operation retains its typed settings, limits, output and actual cancellation contract.
- [ ] Diagnostics display measured data and availability; planned renderers and capture features never appear as working controls.
- [ ] Keyboard-only editing, focus order, Escape, numeric entry, navigation profiles and shortcut conflicts are tested.
- [ ] Layouts are checked at 1280 by 720, 1440 by 900 and 1920 by 1080, with 100, 150 and 200 percent DPI, both themes and long Unicode names.
- [ ] Large-scene virtualization, visible selection reveal, bounded search and draft preservation are measured against the existing baseline.
- [ ] Failed operations retain applied state and usable history; stale requests cannot edit a replacement scene or selection.
- [ ] Workspace restoration preserves scene data, camera, selection and supported view preferences without changing simulation state.
- [ ] Any future multi-view implementation proves resource lifetime, picking, camera independence and resize behavior before release.

## 11 Maintaining this guide

Keep this Markdown file as the editable source of truth and regenerate the companion Word guide after substantive revisions. Design screenshots and HTML concepts must identify simulated controls and their implementation status. Do not use mockups, passing component tests or an operation count as evidence that an entire workflow is complete.

For each migrated panel, retain a short record: old source route, new panel, command IDs, status, supported selections, save scope, input tests and visual/runtime evidence. Update the feature map when a capability is added, removed or becomes unsupported. Revisit dependency licenses at every version change.

### Repository implementation references

- [Current UI and panel assembly](../src/utility/GuiManager.cpp) and [UI state and requests](../include/Container/utility/GuiManager.h).
- [Frontend coordination](../src/renderer/core/RendererFrontend.cpp) and [material transaction integration](../src/renderer/core/RendererFrontendMaterials.cpp).
- [Registered render techniques](../src/renderer/core/RenderTechnique.cpp).
- [Player and Editor UX Plan](player-editor-ux-plan.md) and [feature parity progress](feature-parity-progress.md).
- [Scene browser performance](scene-browser-performance.md), [editor history](editor-history.md) and [scene export](gltf-scene-export.md).
- [Animation](gltf-animation.md), [deformation](gltf-deformation.md), [extended materials](gltf-extended-materials.md), [material variants](gltf-material-variants.md) and [node flags](gltf-node-flags.md).
- [BIM performance](bim-performance.md), [physics integration](physics-integration.md) and [physics joints](physics-joints.md).
- [OpenVDB usage](openvdb.md) and [OpenVDB feature status](openvdb-feature-status.md).
- [Coordinate conventions](coordinate-conventions.md), [temporal rendering](temporal-rendering.md), [ray-query rendering](ray-query-rendering.md) and [third-party notices](third-party-notices.md).

Current tracking: [#108 material and texture authoring](https://github.com/semihguresci/VulkanSceneRenderer/issues/108), [#109 cameras](https://github.com/semihguresci/VulkanSceneRenderer/issues/109), [#110 sky](https://github.com/semihguresci/VulkanSceneRenderer/issues/110), [#111 HDR environments](https://github.com/semihguresci/VulkanSceneRenderer/issues/111), [#112 tone mapping](https://github.com/semihguresci/VulkanSceneRenderer/issues/112) and [#121 parity tracker](https://github.com/semihguresci/VulkanSceneRenderer/issues/121). Recheck live issue state before treating this dated design map as a progress report.
