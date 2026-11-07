#pragma once

#include <Container/geometry/GltfMerge.h>

#include <cstdint>
#include <span>
#include <vector>

namespace container::geometry::gltf {

struct GltfNodeExportCompactionPlan {
  tinygltf::Model model;
  // Indices belong to the input model's node/skin tables, including any export
  // correction wrappers. These are not runtime graph or geometry identities.
  std::vector<int32_t> oldToNewNodes, oldToNewSkins;
  // Inverse of the surviving node map, useful for gathering light/source
  // provenance after export wrappers and retired graph slots are projected.
  std::vector<int32_t> newToOldNodes;
  uint64_t retiredNodes{}, droppedSkins{}, droppedChannels{};
  uint64_t peakPayloadBytes{}, metadataBytes{}, animationDecodedBytes{};
};

// Detached export-only compaction of a validated, resident, normalized model.
// Retirement must be unique, in range, and closed over descendants: surviving
// children are never silently reparented or given different world transforms.
// Surviving nodes/skins keep their relative order. Scenes (including empty
// scenes), metadata and defaultScene survive; all geometry/resource/animation
// sampler/accessor tables retain their original indices and payload bytes.
//
// A skin referencing retired joints/skeleton is refused if any owner survives;
// otherwise that affected skin is removed and survivor skin bindings remapped.
// Unused unaffected skins remain. Core and supported node-pointer channels to
// retired nodes are removed; other channels keep their order and samplers.
// Known node-reference-free extensions are preserved opaquely; unsupported
// extension reference schemas (including LOD/audio) reject rather than guessing
// from JSON field names. Extras are always opaque and are never rewritten.
//
// Checks count/reference/metadata, resident-copy and decoded-animation bounds
// before output model copying. No source mutation, resource loading, playback
// sampling, runtime publication or I/O. This alone does not implement deletion.
[[nodiscard]] GltfNodeExportCompactionPlan CompactGltfExportNodes(
    const tinygltf::Model& source, std::span<const int32_t> retiredNodes,
    const GltfMergeLimits& limits = {});

} // namespace container::geometry::gltf
