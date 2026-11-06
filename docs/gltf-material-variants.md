# glTF material variants

`KHR_materials_variants` defines named, asset-wide selections that replace
materials on individual mesh primitives. A primitive without a mapping for the
selected variant uses its ordinary `primitive.material`. With no active variant,
every primitive uses that authored default. An omitted material means glTF's
implicit default material; it does not mean material zero.

The CPU implementation is in `GltfMaterialVariants.h/.cpp`. It has no renderer,
GPU, UI or mutable material-table dependency. SceneManager applies its validated
plans through the existing scene editing path.

In the Scene Browser, choose **Material variant** or **Restore default
materials**. Each selection is one undoable edit, using the same Undo/Redo
controls as transforms and visibility. To select a variant for a deterministic
capture or command-line session, use `--gltf-variant N` with a zero-based index.
`--gltf-variant -1` (or `default`) selects Default. `--save-scene scene.glb`
saves that appearance with the full named variant table.

## Parsing and selection

`ParseMaterialVariants(source)` builds an immutable library. It records the
source mesh/primitive identity, authored default, named variant indices and
sparse material mappings. It checks root/primitive extension shapes, required
fields, table bounds, material IDs and duplicate variant assignments across a
primitive's complete mapping array. Diagnostics include the offending mesh,
primitive and mapping. A failure does not modify the source model or resources.

Variant identity is its source array index. Names are presentation strings:
duplicate names and empty names are legal in the ratified schema. A selector
must use stable index IDs even when labels match. Default is a separate
`std::nullopt` selection; named variant index zero is the first variant.

`planMaterialVariantSelection(library, selection, sourceMaterialToRuntime,
implicitDefaultRuntime)` validates the selection and complete material
translation before returning assignments. Every source primitive appears in the
plan, including primitives that revert to defaults because the selection has no
mapping. Repeated switching always resolves from immutable imported defaults.
Material IDs remain shared: the operation neither clones materials nor resets
their editable factors, textures or animation values.

An empty translation table uses source material IDs unchanged. A nonempty table
must cover every source material with a valid runtime ID, including alternate
materials that are not initially drawn. The implicit default has its own runtime
ID supplied separately. A scene manager must import the complete primary asset
material/texture table before offering selections.

## Runtime integration contract

The scene manager should retain one library and one optional selected index for
the primary imported glTF. Build a complete selection plan first, then translate
each source mesh/primitive assignment to all of its imported graph instances.
The loader's mesh-major, primitive-major source identity and deformation source
primitive metadata can provide that association. Keep source
`primitive.material` unchanged during the open editing session so Default keeps
its authored meaning even after many selections or primitive reparenting.

Apply material IDs through the same scene edit path used for individual material
assignments. That path must refresh GPU references, opaque/masked/blended
classification, double-sided and ray instance classification, shadow/lighting
draw lists, temporal history and probe state when relevant. Selection undo must
capture exact prior node material IDs and the prior optional selected index;
recomputing a previous named selection alone would lose intervening per-node
edits. Sidecar BIM/USD/procedural nodes without a primary source primitive are
outside the primary asset's selection.

Imported graph primitives retain explicit source mesh/primitive IDs independent
of parenting and names. The graph edit helper stores exact assignments and graph
identity, checks every playback target before mutation, and routes changed
material IDs through revision-tracked graph mutation. Scene reload replaces the
graph identity and clears old edit history.

Material pointer animation continues to target the stable imported material
table. Selecting a variant changes which material a draw references; it does
not retarget channels to newly allocated copies or reset playback. Alpha and
other animated material properties must keep using the existing dynamic
classification refresh.

When a selected normal map changes the UV set used by a missing tangent frame,
SceneManager regenerates the bind tangents from its immutable imported geometry
using the loader's MikkTSpace/fallback implementation. It stages replacement
geometry, deformation bind data and CPU streams before changing live graph
assignments. Source vertex/index order, morph deltas, skin influences and
authored tangents remain intact. The geometry revision triggers matching GPU
bind/current/previous stream rebuilds and ray/probe/temporal invalidation;
undo/redo resolves and regenerates the restored UV policy too.

## Save and reload policy

Saving retains the **current rendered material assignment** in each exported
`primitive.material`, just as an individual material edit does. The saved file
has no standardized active variant selection. On reload, the viewer begins at
Default and Reset restores that saved core assignment. The earlier pre-save
default is not retained as hidden selection history. Within an open editing
session, its imported defaults and undo history remain stable.

`CopyMaterialVariantMetadataForExport` copies the original root definitions and
per-primitive mapping metadata onto rebuilt output primitives. It remaps
alternate material IDs through the complete exported material table, supports
multiple emitted instances of one source primitive, and leaves current core
material assignments untouched. Unknown root/variant/mapping extras and
extensions, mapping order and variant indices are preserved. Validation and
allocation of replacement metadata finish before the destination is changed.

The export caller must supply the source-to-emitted primitive map and retain all
alternate material resources. Materials edited while one variant is active must
still be serialized after switching away: converting only materials referenced
by currently rendered nodes would lose those edits. The helper does not rewrite
resource buffers or texture tables.

## Verification

Generated CPU fixtures cover multi-primitive selective switching, implicit
defaults, repeated switching, shared material identity, one mapping selecting
several variants, duplicate/empty names, malformed declarations and tables,
duplicate/bounds failures, missing alternate material translations, metadata
remapping, instanced primitives, atomic failure and ASCII glTF/GLB reload with
saved appearance/default policy and unchanged resource bytes. Native GPU
invalidation and UI behavior use scene-manager integration checks. The native
`gltf_material_variants_runtime_regression.py` generates independent analytical
static/skin/morph frame oracles and multi-primitive shared-instance alpha/culling
oracles. Its sequences switch, undo, redo, restore defaults, and save/reload the
selected appearance. Deferred classification captures also enable ray queries
and DDGI. Reports record renderer hashes, pixels, semantic state and validation
logs; generation alone does not claim native validation.

The validated checkpoint in
`out/feature-parity/material-variants-runtime-data-uri/results.json` passes 105 native
captures and 81 checks with pixel-exact results on renderer
`ab019867be34735e8cce236442f2508b86b8eaddba42719d8f30b306ccf62cc2`.
It covers both raster techniques, deferred normal output, static/skin/morph UV
changes, undo/redo/default restoration, CLI selection, GLB/ASCII save and reload,
shared primitive instances, alpha/culling classification, temporal epochs,
ray scene refresh and active DDGI. All 94 executable/shader manifest entries
remain unchanged and no Vulkan or synchronization validation errors occur.

Primary references: [ratified specification](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_variants/README.md),
[root schema](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_variants/schema/glTF.KHR_materials_variants.schema.json),
[primitive schema](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_variants/schema/mesh.primitive.KHR_materials_variants.schema.json).
The pinned NVIDIA reference at `174625d5c621c088bf3610377999f9dbdcf5c3b5`
provides [variant switching](https://github.com/nvpro-samples/vk_gltf_renderer/blob/174625d5c621c088bf3610377999f9dbdcf5c3b5/src/gltf_scene.cpp#L1998).
