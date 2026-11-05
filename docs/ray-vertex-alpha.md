# Ray-scene vertex opacity

Ray metadata preserves COLOR_0 alpha separately from RGB. Vertices default to alpha one; the glTF loader retains the fourth component of VEC4 colors and VEC3 colors remain opaque. RayTriangleData is now 64 bytes: three UV0/UV1 vectors followed by three corner alpha values and a reserved component. CPU extraction, shadow traversal and optional closest-hit surface reconstruction share this layout. Rebuild host code and all ray shaders together.

Binary shadow traversal multiplies interpolated vertex alpha with the material, base-color texture and opacity texture factors for MASK coverage. OPAQUE coverage ignores alpha. Closest-hit attributes retain alpha for BLEND and transmission consumers. The ray-query GPU contract checks different barycentric coordinates and the opaque exception; CPU tests cover extraction, reconstruction, defaults and nonfinite rejection. Constant closest-hit colors use affine interpolation bounded to the authored vertex hull to prevent rounding a white material above unit reflectance.

This commit publishes shared ray metadata and loader support. The raster vertex-alpha stream, full reference viewport backend and solid-volume integration are separate publication milestones. Existing raster behavior is unchanged here.

Validation: a clean source snapshot builds the complete renderer and passes 105 cases across seven component executables, including five Vulkan executables. The broader 194-case source-convention suite has eight existing failures; comparison with the unchanged parent produces the same eight failures. Those unrelated checks remain a separate follow-up.
