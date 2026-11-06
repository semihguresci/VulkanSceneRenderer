#pragma once

#include <Container/geometry/GltfMerge.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace container::geometry::gltf {

struct GltfSubtreeCloneOptions {
  // Omitted uses the live effective scene (zero when defaultScene is absent).
  std::optional<size_t> destinationScene;
  // Omitted preserves the original label. A supplied user label uses the
  // shared authoring UTF-8/name policy; source labels remain opaque metadata.
  std::optional<std::string> rootName;
};

struct GltfSubtreeCloneRestrictions {
  // Caller-provided SOURCE identities with physics/product/joint dependencies
  // that this source-only planner cannot duplicate. Applies to visual subtree
  // and required joint/ancestor closure. Graph-only proxies must be rejected by
  // caller policy before invoking this helper, never guessed from source IDs.
  std::span<const int32_t> unsupportedSourceNodes;
};

struct GltfSubtreeClonePlan {
  tinygltf::Model authoredSource;
  tinygltf::Model evaluatedSource;
  // Dense old SOURCE identity -> new SOURCE identity, -1 outside the closure.
  std::vector<int32_t> nodes, skins;
  std::vector<bool> contextOnly;
  // Original channels remain exact prefixes. Remapped core/node-pointer
  // channels reuse their original samplers and accessors in the same clip.
  std::vector<size_t> originalChannelCounts;
  std::vector<int32_t> contextRoots;
  std::vector<size_t> affectedScenes;
  int32_t root{-1}, parent{-1};
  size_t destinationScene{};
  uint64_t clonedNodes{}, clonedSkins{}, clonedChannels{};
  uint64_t peakResidentBytes{}, metadataBytes{}, animationDecodedBytes{};
};

// Pure detached sibling duplication of a source-authored subtree. Inputs obey
// the same validated/resident/normalized archive + evaluated-prefix contract
// as PlanGltfSessionSources; archive-only resource suffixes remain authoritative.
// Old node/resource identities and original channel bytes remain stable. Mesh,
// material, accessor, texture, camera and light resources are referenced once;
// only nodes, used skins and node-directed animation channels are appended.
// External skin joints/skeleton and their ancestor chains are cloned without
// visual attachments. Their original world placement is retained. Parentless
// duplicates enter only the selected scene; a child inherits its parent's
// scene memberships, including shared-root scenes, with joint contexts added
// to each affected scene. Exact authored/live defaultScene values survive.
// No playback sampling, graph publication, runtime mapping, physics inference
// or I/O. Unknown closure extensions/unsupported channels reject rather than
// guessing references. Existing AnimationPlayer::withExtendedSource cannot
// consume changed old clips: a clone-specific player staging proof is needed.
// GltfMergeLimits bounds both inputs, both model outputs and cloned metadata;
// maxPlacementNodes/maxClonedChannels bound this operation's appended work.
[[nodiscard]] GltfSubtreeClonePlan PlanGltfSubtreeClone(
    const tinygltf::Model& evaluatedSource,
    const tinygltf::Model& authoredArchive, int32_t sourceRoot,
    const GltfSubtreeCloneOptions& options = {},
    const GltfSubtreeCloneRestrictions& restrictions = {},
    const GltfMergeLimits& limits = {});

} // namespace container::geometry::gltf
