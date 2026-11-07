#pragma once

#include <Container/geometry/Model.h>
#include <Container/geometry/GltfLegacyMaterialSerialization.h>
#include <Container/geometry/GltfResourceExportCompaction.h>
#include <Container/utility/SceneGraph.h>

#include <tiny_gltf.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace container::geometry::gltf {

struct GltfLinkedCompositionInfo;
struct GltfLinkedPlacementEdit;
struct GltfLinkedHybridCarrier;

struct GltfExportResourceIndexMaps {
  std::vector<int32_t> images, textures, samplers;
};

struct GltfLinkedPortableExportOptions {
  // Payloads will be replaced by URI references after projection.
  std::span<const uint8_t> linkedGraphNodes, omittedAnimations;
  GltfExportResourceIndexMaps* resourceIndexMaps{};
};

// Borrowed CPU state. The caller keeps every referenced object alive for the
// duration of buildExportModel/writeSceneExport; neither function edits it.
struct SceneExportSnapshot {
  const scene::SceneGraph& graph;
  // Bind/rest CPU geometry, with source vertex order retained; GPU
  // evaluated skin/morph output must not be supplied as the bind geometry.
  // Primitive identities use the legacy prefix or the explicit map below.
  const geometry::Model& geometry;
  // Original accessor/animation/mesh/skin tables and resident source resources.
  // Nodes/lights/cameras may contain the current evaluated source pose, used
  // to separate editor corrections from absolute authored animation tracks.
  const tinygltf::Model* source{nullptr};
  // Original .gltf/.glb filename, used to resolve external source resources.
  std::filesystem::path sourcePath{};
  // Runtime node -> source node; -1 denotes an editor-created/synthetic node.
  std::span<const int32_t> sourceNodeIndices{};
  // Optional complete material table indexed by SceneNode::materialIndex.
  // Texture indices must refer to source's preserved texture table. An empty
  // span uses source.materials, or a default material for a procedural scene.
  std::span<const tinygltf::Material> materials{};
  // Source material index -> index in the supplied runtime material table.
  // Empty uses unchanged source material indices. Required when a source
  // KHR_animation_pointer channel targets a relocated runtime material.
  std::span<const int32_t> sourceMaterialToRuntimeIndices{};
  // Optional unchanged authored source defaults, in the same index space as
  // source. Clips receive constant restore channels for properties animated
  // only by other clips, preserving clip switching despite saved-pose defaults.
  const tinygltf::Model* animationRestSource{nullptr};
  // Optional complete scene catalog, indexed like source.scenes. Nodes in each
  // set are runtime authoring roots below synthetic editor/import prefixes.
  // The full graph is serialized, independently of its active renderable view.
  // Shared roots in different scenes retain one output identity; duplicates
  // within a scene are invalid. Empty span preserves legacy single-scene export.
  std::span<const std::vector<uint32_t>> runtimeSceneRoots{};
  int32_t defaultSceneIndex{0};
  // Runtime geometry ID -> source mesh-major primitive ID; -1 is procedural.
  // Empty retains the single-asset source prefix. Explicit maps cover every
  // runtime slot and every source definition exactly once.
  std::span<const int32_t> runtimePrimitiveToSourceIndices{};
  // Resource IDs refer to source's preserved resource tables. Material activity
  // is in the supplied runtime export table; node activity is derived from the
  // graph. Spans are borrowed for this call. Filtering precedes image I/O.
  GltfResourceExportMasks sourceResourceMasks{};
  std::span<const uint8_t> omitNodeMeshes{};
  GltfExportResourceIndexMaps* resourceIndexMaps{};
};

// Rebuilds current geometry and hierarchy in glTF-native coordinates. Images
// and buffers become one in-memory buffer, ready for a portable GLB or glTF.
// Source animations, skins, morph targets, cameras, lights and resource metadata
// are retained and remapped. Runtime node indices remain stable; transform
// correction parents may be appended. Throws when state cannot be represented
// faithfully (for example a post-skin primitive edit requiring baking).
[[nodiscard]] tinygltf::Model buildExportModel(
    const SceneExportSnapshot& snapshot);

// Writes .glb (self-contained) or .gltf (with one content-addressed .bin beside
// it). Stages all output beside the destination before atomic file replacement.
// Existing destination/dependencies and the live snapshot survive failures.
// Parent directory must already exist. Throws std::runtime_error on failure.
void writeSceneExport(const SceneExportSnapshot& snapshot,
                      const std::filesystem::path& destination);

// Writes a detached, already packed buildExportModel result. Callers may add
// current light/camera definitions and node associations before saving. Resource
// buffer views must still reference its single in-memory buffer; this function
// does not resolve or repack external resources and never changes the model.
void writeExportModel(const tinygltf::Model& model,
                      const std::filesystem::path& destination,
                      const GltfLegacyMaterialSerializationLimits& legacyLimits = {});

// Atomic companion-manifest save. Authored external files remain read-only.
void writeLinkedSceneManifest(const GltfLinkedCompositionInfo&,
    std::span<const GltfLinkedPlacementEdit>, size_t selectedScene,
    const std::filesystem::path& destination);

struct GltfLinkedHybridExport {
  const GltfLinkedCompositionInfo& composition;
  std::span<const GltfLinkedHybridCarrier> carriers; // output IDs before node compaction
  std::span<const uint8_t> prunedGraphNodes;
  std::span<const size_t> linkedContexts;
  std::span<const int32_t> sourceMaterialToExport, retiredMaterials, animatedLinkedMaterials;
  const GltfExportResourceIndexMaps& resourceIndexMaps;
};

void writeLinkedHybridScene(tinygltf::Model model, const GltfLinkedHybridExport&,
                            const std::filesystem::path& destination);

}  // namespace container::geometry::gltf
