#pragma once

#include <tiny_gltf.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace container::geometry::gltf {

struct GltfMergeLimits {
  uint64_t maxTableEntries{1'000'000};
  uint64_t maxTotalObjects{4'000'000};
  uint64_t maxNodes{1'000'000};
  uint64_t maxScenes{65'536};
  uint64_t maxPrimitives{1'000'000};
  uint64_t maxReferences{8'000'000};
  uint64_t maxRootReferences{1'000'000};
  uint64_t maxAnimationChannels{1'000'000};
  uint64_t maxPlacementNodes{1'000'000};
  uint64_t maxClonedChannels{1'000'000};
  // Includes payloads retained by the two inputs plus the detached output.
  uint64_t maxResidentBytes{512ull * 1024 * 1024};
  // Conservative serialized metadata estimate, including placement copies.
  uint64_t maxMetadataBytes{64ull * 1024 * 1024};
  // Sum of accessor count * components * sizeof(double), before parsing clips.
  uint64_t maxAccessorDecodedBytes{512ull * 1024 * 1024};
  // Per-channel clip expansion, including duplicated placed channels.
  uint64_t maxAnimationDecodedBytes{256ull * 1024 * 1024};
  uint32_t maxMetadataDepth{64};
};

struct GltfMergeOptions {
  std::string parentName{"Imported scene"};
  std::optional<size_t> destinationScene;
  std::optional<size_t> incomingScene;
  // Column-major glTF-native affine matrix. No geometry or inverse binds bake.
  std::optional<std::array<double,16>> placementMatrix;
};

struct GltfMergePrimitiveIndex { int32_t mesh{-1}; int32_t primitive{-1}; };

struct GltfMergeIndexMaps {
  std::vector<int32_t> buffers, bufferViews, accessors, images, samplers, textures;
  std::vector<int32_t> materials, meshes, nodes, skins, cameras, lights;
  std::vector<int32_t> animations, scenes, variants;
  std::map<std::string,std::vector<int32_t>> extensionLights;
  // Incoming mesh-major primitive identities; never runtime geometry indices.
  std::vector<std::vector<GltfMergePrimitiveIndex>> primitives;
};

struct GltfMergePlacement {
  // Incoming source index -> placed destination index; -1 outside the closure.
  std::vector<int32_t> nodes, skins;
  // Transform/joint closure nodes without selected-scene visual attachments.
  std::vector<bool> contextOnly;
};

struct GltfMergeSummary {
  uint64_t objects{}, primitives{}, references{}, rootReferences{};
  uint64_t animationChannels{}, placementNodes{}, clonedChannels{};
  uint64_t payloadBytes{}, peakPayloadBytes{}, metadataBytes{}, accessorDecodedBytes{}, animationDecodedBytes{};
};

struct GltfMergePlan {
  tinygltf::Model model;
  GltfMergeIndexMaps imported;
  GltfMergePlacement placement;
  GltfMergeSummary summary;
  int32_t parentNode{-1}, destinationScene{-1}, incomingScene{-1};
  // Generator/origin information does not overwrite the destination asset.
  tinygltf::Asset incomingAsset;
};

// Pure detached authoring merge. Inputs must be validated, resident, normalized
// loader results (compression already consumed). Rechecks bounded references,
// accessors, metadata, clips and the global strict forest without source writes
// or I/O. All incoming scenes retain canonical parentless roots; the chosen
// scene is cloned beneath one named parent in the destination scene. Skins and
// node-directed channels follow the placement closure, resources stay shared.
// Optional -1/default materials remain -1. Existing destination IDs are stable.
// Resident image payloads are authoritative: output image URIs are cleared so
// later export cannot resolve a secondary asset against the primary base path.
// Unknown extension references, conflicting root metadata, unsupported animation
// targets and nonrepresentable placement fail with contextual runtime_error.
// This does not publish renderer state, preserve session playback, or add undo.
[[nodiscard]] GltfMergePlan PlanGltfMerge(
    const tinygltf::Model& base, const tinygltf::Model& incoming,
    const GltfMergeOptions& options = {}, const GltfMergeLimits& limits = {});

} // namespace container::geometry::gltf
