# Vertex opacity across raster and ray tracing

Ray metadata preserves COLOR_0 alpha separately from RGB. Vertices default to alpha one; the glTF loader retains the fourth component of VEC4 colors and VEC3 colors remain opaque. RayTriangleData is now 64 bytes: three UV0/UV1 vectors followed by three corner alpha values and a reserved component. CPU extraction, shadow traversal and optional closest-hit surface reconstruction share this layout. Rebuild host code and all ray shaders together.

Binary shadow traversal multiplies interpolated vertex alpha with the material, base-color texture and opacity texture factors for MASK coverage. OPAQUE coverage ignores alpha. Closest-hit attributes retain alpha for BLEND and transmission consumers. The ray-query GPU contract checks different barycentric coordinates and the opaque exception; CPU tests cover extraction, reconstruction, defaults and nonfinite rejection. Constant closest-hit colors use affine interpolation bounded to the authored vertex hull to prevent rounding a white material above unit reflectance.

Raster pipelines bind alpha at location 8, leaving locations 6/7 available for previous deformed position/normal data. Depth, G-buffer, forward shading, directional/local shadows, transparent picking, object-normal display and temporal velocity use the same material/vertex/texture opacity product. OPAQUE coverage ignores that product after evaluation; MASK and BLEND retain their authored coverage. Rebuild the executable and shader assets together. The importer keeps optical transmission independent of alpha mode; a texture cannot enable a layer whose factor is zero. The full reference viewport backend and solid-volume integration are separate publication milestones.

Flat MASK alpha uses the inclusive glTF cutoff in raster and ray paths. Raster
passes use ordered coverage where the evaluated alpha varies. Untextured masks
interpolate the signed distance from the cutoff. Textured masks recover
constant vertex alpha within float32 interpolation precision before applying
texture alpha. A surviving MASK fragment is fully covered before the raster
shading pass applies optical transmission. [glTF alpha coverage](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#alpha-coverage)

Validation: a clean source snapshot builds the complete renderer and passes 108 cases across seven selected component executables, including five Vulkan executables. There are 80 native PNG captures and process logs across 46 oracle checks: MASK/BLEND/OPAQUE coverage, opaque optical glass, zero-factor/absent-layer equivalence, directional raster/ray shadows with an active probe-GI trace, MSAA 4x and populated TAA history. Independent alpha textures and analytic receiver regions provide the native reference; alpha texture interpolation differences stay below 0.011 mean code values in the targeted AA cohort. Ordinary raster shaders retain their non-ray capability boundary, and all 92 generated SPIR-V modules validate. Transparent picking/local-shadow vertex layouts are build validated; the native shadow oracle exercises directional shadows. The new material-factor and loader checks also pass in the full local feature-parity build (44 + 5 cases).

The broader 194-case source-convention suite has the same eight existing failures as the unchanged parent, with no new failures. They remain a separate follow-up. Frozen publication evidence is `out/feature-parity/raster-alpha-publication-acceptance/results.json`; failed exploratory runs are excluded from acceptance.
