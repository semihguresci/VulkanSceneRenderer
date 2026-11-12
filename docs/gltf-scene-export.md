# glTF scene export

The CPU export API in `Container/geometry/GltfSceneExport.h` saves the current
primary glTF scene and its editor-created geometry. `buildExportModel` creates a
detached `tinygltf::Model`; `writeSceneExport` writes `.gltf` or `.glb`. Neither
function changes the scene graph, geometry, source model, selection, history, or
renderer resources.

`writeExportModel` writes an already packed detached model. The frontend can
replace imported light definitions with the current edited light entities and
append their current world-space nodes before publishing that model. It never
reopens source resources or changes the supplied model.

## Snapshot contract

`SceneExportSnapshot` borrows the current `SceneGraph`, runtime `Model`, and
optional imported `tinygltf::Model`. The caller must keep these alive and stable
through the call. `sourcePath` is the original source filename and resolves
external image/buffer URIs. The optional `sourceNodeIndices` array covers every
runtime node; `-1` marks synthetic primitive nodes and editor-created nodes.
Source node identities carry camera/light associations and source metadata into
the exported hierarchy. An optional complete material table uses runtime
`SceneNode::materialIndex` indices and preserves the source texture indices.
An empty material span uses the imported materials, or one default material for
a wholly procedural scene.

The geometry must contain bind vertices in original mesh-major/primitive-major
order, followed by any procedural additions. The source retains original
accessor, animation, mesh, and skin tables; its nodes/lights/cameras may carry
the current evaluated pose. `sourceMaterialToRuntimeIndices` maps original
material indices to the supplied runtime material table for animation pointers.
`SceneNode::meshOwnerGraphNode` identifies each imported primitive's original
skin/morph owner independently of editor reparenting.

`animationRestSource` optionally supplies the unchanged authored source defaults
in the same source index space. The application supplies its import archive.
This lets exported clips retain their original rest behavior while the main
node/material/light/camera tables describe the currently saved pose.

`GltfMaterialExport.h` combines resources from independently imported assets and
converts the complete runtime material table. It validates finite values, glTF
factor ranges, scalar texture channels, UV0/UV1, and mapped resource indices.
Runtime opacity factors are folded into base-color alpha, and HDR emission is
expressed through `KHR_materials_emissive_strength`. Runtime texture-backed
factor defaults and the specular-map RGB fallback are expressed explicitly so
ordinary glTF viewers receive the same material inputs. Generated material and
nested texture-transform extensions are declared in `extensionsUsed`.

Every runtime node is serialized, including groups, synthetic primitive
children, hidden nodes, and procedural additions. Node names, local matrices,
child/root ordering, material assignments, and visibility come from the current
graph. Mesh instances share serialized geometry where possible; vertices stay
in mesh-local coordinates. Matrices remain column-major, right-handed, +Y up,
with no viewport or projection conversion. Hidden nodes use the optional
`KHR_node_visibility` extension with `visible: false`, preserving the local
visibility flag independently of ancestors.

Runtime triangle streams supply `POSITION`, `COLOR_0`, `TEXCOORD_0`,
`TEXCOORD_1`, and indices. Imported `NORMAL` and `TANGENT` retain their original
presence: missing normals keep both frame attributes absent, and missing
tangents keep `TANGENT` absent. Morph playback can then regenerate the same
missing frames after reload. Procedural geometry includes its generated frames.
Import-time geometry repairs are exported as ordinary glTF triangles.
The primary scene hierarchy is retained rather than flattened into a single
world-space mesh. Source cameras, lights, materials, material extensions,
textures, samplers, and optional extras/extensions are retained where their
references remain meaningful.

## Animation, skins, and morph targets

Source animation samplers and accessor payloads are preserved, including STEP,
LINEAR, and CUBICSPLINE interpolation and optional metadata. Standard TRS
channels and node/material/light/camera `KHR_animation_pointer` targets are
remapped into exported index spaces. A multi-primitive source mesh becomes
separate editable mesh nodes, so its weights channels are duplicated onto each
emitted morph instance. Component pointers such as `/nodes/0/weights/1` retain
their component selection. Animated texture-transform objects remain present
even when the current offset/rotation/scale equals the schema default.

An animated node remains a TRS target at its stable runtime index. When its
current editor matrix differs from the evaluated source TRS, an appended parent
carries the constant correction `editorLocal * inverse(sourcePoseLocal)`.
This preserves current matrices and shear while retaining authored absolute TRS
curves. Ordinary runtime nodes keep their indices; callers adding light data
can mark appended correction nodes with source identity `-1`. Playback applies
the preserved curves beneath the editor correction; saving does not resample or
rewrite animation keys.

Original joint/weight attribute sets and morph target accessors are retained on
rebuilt bind geometry. Skin joint/skeleton references use exported node indices;
inverse-bind data remains unchanged. Each emitted primitive gets its original
instance skin and current node weights, with original mesh defaults preserved.
This retains shared meshes instantiated with different skins or morph weights.
The current pose is saved as default data while clips remain available for
subsequent playback. With `animationRestSource`, each clip receives constant
one-key STEP channels for animated properties that it omits but another clip
uses. Partial vector channels receive restores only for missing animated
components. These channels use authored rest values and schema defaults, so
switching to a material-only clip does not leave a transform at the pose that
happened to be active during saving. Original keys, samplers, and metadata stay
unchanged. Clearing playback returns to the file's saved pose defaults. Save
does not record the viewer's playback clock.

## Portable resources and replacement

Source buffers and encoded image resources are packed into one aligned binary
buffer. External images, image data URIs, and GLB image buffer views are
supported. External source resources must remain readable until export has
finished; an unavailable resource fails before destination replacement. Image
buffer views preserve encoded source bytes. In-memory 8-bit images without an
encoded source can be encoded as PNG.

GLB output is self-contained. ASCII glTF output has one adjacent binary
dependency named `container-<content hash>-<byte length>.bin`; image data lives
in buffer views in that dependency. Copy the `.gltf` and referenced `.bin`
together. Neither format depends on the original source directory.

The destination parent directory must already exist. Export first builds an
independent model, then serializes all files in a uniquely named staging
directory beside the destination. ASCII dependencies are installed under
immutable content-addressed names and verified before reuse. The completed
scene file replaces the destination with the platform's atomic file rename
operation. Old dependencies are retained so another exported glTF can continue
to reference them. A failed final replacement may leave an unused dependency;
it cannot overwrite an earlier dependency or change the earlier scene file.
Temporary staging directories are removed on success and failure.

## Saving from the application

The model panel exposes a destination path, **Save As**, and **Save scene**.
Choose a `.glb` or `.gltf` path and use **Save As** for the first save. A successful
save becomes the destination for **Save scene** and **Ctrl+S**. Keyboard save is
routed when a text field is not consuming keyboard input. Loading or unloading
the primary model clears its remembered save destination. A failed save reports
the error while retaining the previous destination and current edits.

The command line accepts `--save-scene PATH`. In an interactive run it exports
when the application loop exits; combined with `--screenshot`, it exports after
the capture loop. For example:

```powershell
VulkanSceneRenderer.exe --model source.gltf --screenshot preview.png --save-scene edited.glb
```

Both paths explicitly save the primary editable scene. BIM/USD/VDB sidecars,
physics configuration, viewer camera/exposure/environment settings, and renderer
presets keep their own formats. Imported glTF camera definitions remain part of
the scene. Current edited imported lights and manual lights are serialized;
generated diagnostic lighting is excluded. Imported light IDs use original
source traversal and stable source node identities, so graph reparenting cannot
redirect an edit to another lamp. Light-only child nodes carry current world
poses without moving meshes that share an imported light node. Extra untouched
source lights, including those beyond the active renderer capacity, remain
associated with their original source nodes.

Supported `KHR_interactivity` references are remapped through the final node,
material and resource projections. Punctual-light pointers target the definition
actually bound to the exported light child. Equivalent shared placements keep
one target; divergent definitions or incompatible property conversions reject
before destination replacement. Unreferenced retirement does not by itself
block saving a supported graph. Dynamic object IDs still require a complete,
unchanged index domain. See [behavior graph export](gltf-interactivity.md).

Imported light clones retain authored pointer animation as absolute values, so
animating a field replaces its saved UI override during playback. Pointer ranges
are converted from authored units by the import scale; HDR color/intensity
tracks are adjusted together when the saved UI color is normalized into glTF's
color range. Clearing playback restores the exported edited rest table. The
active imported sun keeps its animated ancestry. Competing ancestor directional
bindings move onto identity children after the active branch to preserve the
same active-sun traversal policy. A manual sun override remains independent of
imported pointer channels.

## Current scope

glTF `KHR_xmp_json_ld` packets and their asset, scene, node, mesh, material,
image and animation references are preserved for exported records. Packet
indices stay stable when scene tables are compacted. Import reports malformed
packets or out-of-range references and keeps the asset renderable.

Open the glTF metadata sections in Scene controls to inspect the asset,
active scene, or selected animation. Select a hierarchy node or click a
surface to inspect the node, mesh, and surface material packets. The material
and resource catalogs also show the selected source material or image packet.
The read-only summary shows scalar values, lists, and language tags. Expand
**Raw packet JSON** and choose **Copy JSON** for the complete packet.
[KHR_xmp_json_ld](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_xmp_json_ld)

When Save As writes a generated primitive node for a source mesh, that node
shares the authored node's packet reference. Picking the primitive after
reopening a portable glTF or GLB resolves the same node metadata.
Native tests check viewport selection, packet contents, and pixel-exact
round trips in both raster paths. [Windows NVIDIA results](../tests/visual-regression/gltf-xmp/windows-nvidia/gltf-xmp-results.json)

Import / Merge keeps the destination's packet indices and appends incoming
packets in source order. The incoming document packet is attached to the new
placement node. Imported scenes and objects keep their own attachments, and
invalid references cannot resolve to packets from another source. Conflicting
properties on the XMP package wrapper fail before publishing the merge.

The experimental glTF 2.1 URI profile imports linked content from read-only
source files. Standard Save As flattens the live scene into an editable glTF
2.0 or GLB copy with portable resources. Reload that copy to work independently
of the linked files. Both save routes protect the original inputs, including
paths that point to the same files.
Portable Save As also supports a scene merged from different linked manifests.
A focused regression checks colliding relative resource names and alias keys,
merge undo/redo, source-free glTF/GLB reload and protection of both input roots.
All 24 captures and 52 checks pass in both raster techniques, including 20
pixel-exact comparisons against independent static controls. Evidence is under
`build/codex-gltf-validation/linked-merged-export-native/results.json`.
The hierarchy marks linked payloads read-only and offers **Make Editable**
for their shared placement family. Detaching the family clears scene edit
history. Save Linked As writes the detached family and its edits inline while
keeping the other families linked.
The `extras.containerLinkedSource` provenance records the original relative URI
and node index and carries no file dependency. Flattened linked children share
one packet table per source and alias context. Each placement exposes its
child document and selected-scene XMP on metadata nodes beside the source
roots. A linked skin binds to the joints in its own placement, while linked
animations share their sample data across placements. Save Linked As keeps
these definitions in the referenced source files.

**Save Linked As** and `--save-linked-scene PATH` write a companion ASCII
`.gltf` beside the primary manifest so each relative URI keeps its meaning.
With all families linked, the companion preserves the original URI, alias,
and external-asset tables. It records the selected scene and edits to primary
node names and local transforms.

After Make Editable, the companion also includes the current editable
geometry, materials, and hierarchy. It keeps only the URI links and alias
rows needed by the remaining families, and reloads their clips and resources
from those files. Save from the authored rest pose. The writer verifies
remaining dependencies before replacing the destination. Detached dependency
files can be absent. Use portable Save As for an active animation, a selected
material variant, an interactive primary scene, or a copy independent of all
linked sources. A hybrid companion can retain links from the primary
manifest. Live links from merged manifests and linked lights with scaled
ranges or emitter sizes require portable Save As.

This API exports one primary runtime scene. The UI integration must reject
mixed BIM/USD content, or clearly name a primary-scene-only export scope.
Animation/joint references outside the exported scene, missing material/owner
mappings, and animation pointer roots that cannot be remapped safely fail
before destination replacement. An editor correction at a singular animated
pose also fails if the current matrix differs from that pose. No animation
channel is silently removed.

A required cross-clip rest value must fit a finite glTF FLOAT accessor. For
example, an omitted `KHR_materials_volume.attenuationDistance` means infinity;
when a different clip needs a constant restore for it, export fails explicitly.
The property can still be exported when every clip already animates it.

glTF places skinned meshes through their joints. Export gives a transformed or
reparented skinned primitive a private joint hierarchy and retains its inverse
binds, vertex influences, morph targets, and animation. An editor transform
with shear is written as a product of TRS-compatible node transforms. Identity
primitive children continue to share their original skin. For an edited GPU
instance the private rig carries its placement and the saved mesh retains any
custom per-instance attributes.

Shared owner and joint ancestors cancel out before export computes the inverse
owner path. Translation, rotation, and nonzero STEP or constant scale can be
inverted while retaining their animation interpolation. Export fails before
destination replacement when the saved mesh world or a required inverse is
singular, a continuously varying scale would require a reciprocal curve, or
`KHR_interactivity` writes a transform that the private hierarchy would have
to duplicate.

Known separate roughness/metalness/height/opacity maps and their legacy channel
and factor fields are retained when exact source-binding proofs are available.
The project writer preserves their original parameter locations in ASCII glTF
and GLB. Runtime-only custom inputs without that proof, unsupported channel
overrides and material LOD references without a material mapping still reject.

Source-backed specular-glossiness materials export their retained evaluated
definition after exact forward conversion proves every runtime value and binding.
Texture references are remapped through the owner's explicit authored/evaluated
source-ID pairs, including dormant and legacy references. Unknown normative
metadata rejects when source IDs require remapping; opaque extras are preserved.
The runtime-only specular-glossiness approximation still requires baking rather
than an inferred inverse conversion. Unlit output omits incompatible lighting
extensions whose inputs are inactive while unlit shading is selected.
The renderer's additive emission on unlit materials also requires baking and is
refused when active, because standard glTF unlit ignores emissive inputs.

Unknown optional metadata is preserved on copied source objects, but arbitrary
third-party extensions that contain node/buffer/primitive indices or their own
external URIs cannot be remapped generically. Known source node level-of-detail
references are remapped when all referenced nodes remain in the exported scene.
Compression declarations are removed from rebuilt runtime primitives because
they describe obsolete source representations. Material-variant definitions
and primitive mappings are retained with remapped material IDs; the currently
rendered assignment becomes the saved core material and reloads as Default.
Source geometry arrays remain available to preserved supported metadata; they
are not used as a substitute for current edited geometry. Imported scenes other
than the active primary scene are not included.

## Verification

`tests/geometry/gltf_scene_export_tests.cpp` reloads exported files through the
validated glTF loader. It covers hierarchy and matrices, local visibility,
procedural append, material edits, UV selection/texture transforms, source
lights/cameras/extras, PNG resources from external files/data URIs/GLB buffers,
cross-directory portability after removing the source directory, retained live
state, failed writes, and overwrites that preserve earlier dependencies.
It also compares skinned/morphed world vertices at multiple animation times
after both glTF and GLB reload, including shared mesh instances, skin/joint
remapping, normalized integer weights, duplicated weights channels, material
pointer relocation, camera/light pointers, interpolation metadata, and shear
correction parents. Failed animated saves preserve the previous destination.
Additional regressions switch between clips with whole-vector and component
TRS/material targets, preserve saved defaults, reject nonfinite restore values,
and compare regenerated deformation normals with missing base attributes after
both glTF and GLB reload. Edited-skin cases compare positions, normals, and
tangent frames at the saved pose and across clip switches, including shear,
mirrored reparenting, and an animated mesh owner.

The native subtree-clone binding regression saves a moved skinned primitive
and its duplicate in both formats. It checks reciprocal STEP scale channels
and compares the reloaded deformation and raster output against independently
baked reference geometry.

The native animated-export regression additionally saves from a nonzero pose
and compares animation endpoints after reload in both raster techniques.
`out/feature-parity/animated-export-runtime/results.json` records 82 GLB
captures/38 source-reload pairs;
`out/feature-parity/animated-export-gltf-runtime/results.json` records 59 ASCII
glTF captures/28 pairs. All 66 pairs are pixel exact on the checkpoint binary.
Fixtures include five numeric/TRS clips, texture transforms, authored cameras,
scaled light ranges, default deformation, shared skins/weights and combined
skin/morph deformation. Reports reject missing bindings, animation diagnostics,
Vulkan/synchronization errors and any executable change during a run.

Texture-transform reconciliation preserves authored `extras` and unknown
optional extension metadata while replacing current offset/scale/rotation and
UV selection. The metadata follow-up in
`out/feature-parity/animated-export-texture-metadata-runtime/results.json`
passes 18 captures/8 pixel-exact pairs across both formats and techniques,
including explicit assertions on the live emitted material's retained
transform metadata and unchanged sampler policies.

GPU content deduplication does not merge authored texture identities during
save. Unchanged material slots retain their own source texture, image and
sampler metadata even when equal pixels share one runtime resource. Edited
bindings resolve through the current resource table instead.
