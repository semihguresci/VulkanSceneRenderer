#pragma once

#include <Container/geometry/GltfProjectedSubtreeClone.h>
#include <Container/utility/GltfMergeGraph.h>

namespace container::scene {

struct GraphCloneProjectionInput {
  const SceneGraph& graph;
  const tinygltf::Model& source;
  uint32_t root{SceneGraph::kInvalidNode};
  // Complete current runtime view roots in source catalog order.
  std::span<const std::vector<uint32_t>> sceneRoots;
  size_t selectedScene{};
  uint64_t runtimePrimitiveCount{}, runtimeMaterialCount{};
  std::span<const uint32_t> sourcePrimitiveToRuntime{};
  std::optional<std::string> rootName;
};

struct GraphCloneProjectionLimits {
  GltfMergeGraphLimits graph{};
  uint64_t maxWorkspaceBytes{128ull*1024*1024};
  uint64_t maxOutputBytes{256ull*1024*1024};
  uint64_t maxReferences{1'000'000}, maxSceneRootReferences{1'000'000}, maxScenes{65'536};
};

struct GraphCloneProjectionPlan {
  SceneGraph graph;
  // Original graph ID -> appended graph ID, invalid outside visual/dependency
  // closure. newToOldGraph is indexed by (newGraphId - firstNode).
  std::vector<uint32_t> oldToNewGraph, newToOldGraph;
  geometry::gltf::GltfSubtreeCloneProjection sourceProjection;
  geometry::gltf::GltfProjectedCloneOwnershipProof ownership;
  std::vector<std::vector<uint32_t>> sceneRoots, addedSceneRoots;
  std::vector<uint32_t> dependencyRoots;
  uint32_t firstNode{}, root{SceneGraph::kInvalidNode};
  uint64_t workspaceBytes{}, outputBytes{}, clonedRenderables{};
};

// Copy the exact non-retired runtime subtree, retaining graph-only empty and
// procedural nodes, per-node edits and shared primitive/material identities.
// A primitive whose immutable mesh owner lies outside the visual subtree gets
// a nonvisual binding-owner copy; joint/ancestor closure is transform-only.
// No external visual primitives are pulled into the copy. New source groups
// receive IDs in sourceProjection order; graph-only nodes keep -1. Copied
// owners without copied primitives drop their source mesh binding in that
// projection, preventing invented draws/unused morph-channel targets.
// Retained payloads/world caches and existing edge order remain exact; only
// new child edges are appended to retained parents. Worlds must be current;
// new contexts retain their current ancestor placement and instance inverses
// are checked before publication. The result publishes via prepareAppend.
// No source/resource decoding, geometry copies, I/O or live mutation.
[[nodiscard]] GraphCloneProjectionPlan BuildGraphCloneProjection(
    const GraphCloneProjectionInput& input, const GraphCloneProjectionLimits& limits = {});

} // namespace container::scene
