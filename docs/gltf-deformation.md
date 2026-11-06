# glTF skinning and morph targets

Imported meshes use authored default morph weights as soon as the scene
loads. A node's weights take precedence over its mesh defaults. Clips can
animate morph weights directly or through a supported animation pointer.
Skin palettes use the current hierarchy and inverse-bind matrices. The
compute pass applies morphs before skinning and preserves vertex color,
alpha, texture coordinates, and tangent handedness. Mesh instances keep
independent poses when their owners or joint palettes differ.

Forward and deferred raster, directional and local shadows, and the GPU
transparent picker consume the current vertex stream. Opaque temporal
velocity also consumes the previous submitted stream. A seek or incompatible
history change uses the current pose for both streams until a new frame
submits. CPU bounds, selection rays and ray-query acceleration use the
matching evaluated vertex positions. Mirrored skins retain their facing
in both raster and ray queries. Mixed or collapsed triangles use conservative
culling.

The Vulkan pass packs an immutable bind stream with morph targets, skin
influences and a map from output vertices to instances. Each swapchain
image has two 72-byte-per-vertex outputs and a pose buffer. Unmapped
vertices and the diagnostic geometry share the same output layout.
CPU evaluation also provides the geometry needed for bounds, picking and
ray builds. A mesh with missing base normals or tangents may need expanded
triangle corners and CPU frame regeneration before the GPU publishes
the vertices. Those cases can upload a full current and previous vertex
stream for the affected primitive. Static scenes use the regular geometry
buffers.

The loader bounds decoded source data. Scene preparation rejects missing
joint bindings, non-invertible skin owner transforms and an expanded
vertex count beyond the device's storage limit before replacing the
current scene. The pass also checks packed immutable data, pose bytes
and 32-bit addressing. Skins and morphs currently use triangle primitives.

Use **Scene Controls → glTF Animation** to pick a clip or scrub a pose.
The native regressions compare the Vulkan draws with independently baked
geometry and probe depth, shadow coverage, CPU picking and motion vectors.
The compute tests compare raw vertex outputs with known poses and verify
the history on frame reuse.
