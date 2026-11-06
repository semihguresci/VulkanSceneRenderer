# Save editable scenes as glTF and GLB

In **Scene Controls**, enter a destination in **Save path** and choose **Save As**.
**Save scene** and **Ctrl+S** reuse the last successful destination for the active asset.

A GLB contains its binary data and images. A glTF uses a single neighboring
`container-<hash>-<length>.bin` dependency. Move the glTF and its dependency together.
The destination directory must exist. Save stages files in the destination directory
and replaces the scene only after serialization and dependency installation succeed.
Existing exports can continue to use earlier dependencies.

The export uses the editable primary scene. It preserves the current transforms,
hierarchy, local visibility, procedural geometry, material and texture references,
authored cameras, and current authored or manually edited lights. It retains bind
geometry, skin and morph data, and animation clips. BIM, USD, VDB and renderer
settings use separate files.

The active scene becomes the saved default. Other source scenes retain their
geometry, metadata, and authored lighting. Import scale is included in light
ranges and animated range keys. An edit to a light on a shared source node
appears in every scene that contains that node. Save reports an error if the
current shared hierarchy cannot be represented safely in all source scenes.

Use `--gltf-scene N` to choose a scene by its zero-based index when launching the
application. Without that option, the file's authored default is used, or scene
zero if the file leaves the default unspecified.

For a batch export, add `--hidden --save-scene edited.glb` to a renderer command.
When a capture sequence is present, the scene is saved after its final frame.
The save uses the animation pose at the time of export as its starting state.
