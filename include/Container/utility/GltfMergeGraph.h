#pragma once

#include <Container/geometry/GltfMerge.h>
#include <Container/utility/GltfSceneInstancing.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace container::scene {

struct GltfMergeGraphLimits {
  GltfMeshAttachmentLimits hierarchy{262144, 524288};
  uint64_t maxActiveRenderableObjects{4096};
};

struct GltfMergeGraphCandidate {
  SceneGraph graph;
  // Complete merged source-node identity -> stable runtime group identity.
  std::vector<uint32_t> sourceNodeToGraph;
  // Source catalog order; these are view roots, not global graph roots.
  std::vector<std::vector<uint32_t>> sceneRoots;
  std::vector<uint32_t> canonicalRoots;
  uint32_t editorRoot{SceneGraph::kInvalidNode};
  uint32_t importRoot{SceneGraph::kInvalidNode};
  uint32_t placementRoot{SceneGraph::kInvalidNode};
};

// Pure detached assembly for an already validated authoring merge. Existing
// node payloads/IDs, procedural geometry, sibling/root/renderable order and the
// active-root view (with the narrow context refinement below) survive. Only new
// source groups and their synthetic children are extracted from the complete
// generated hierarchy.
// One fresh import wrapper applies session scale once beneath the editor root;
// the named placement is the only appended active root. Canonical imports stay
// outside that active view. When an editor root itself was selected, refine it
// into its original retained children, preserving source/renderable membership.
// Other selected ancestors cannot hide new descendants and are refused.
// An empty base receives a new editor root; a nonempty base requires exactly
// one nonrenderable editor root. Retained world matrices must already be current.
//
// Materials and primitive IDs are complete mesh-major source-ordered tables,
// including old sources and inactive canonical content. Limits distinguish
// aggregate authoring storage from the active device object budget. All input
// validation and expansion checks precede copying the live graph; failures do
// not mutate it. After graph-dependent staging, publish through
// base.prepareAppend(std::move(result.graph)) and base.publishAppend(proof).
[[nodiscard]] GltfMergeGraphCandidate BuildGltfMergeGraphCandidate(
    const SceneGraph& base, const geometry::gltf::GltfMergePlan& plan,
    size_t retainedSourceNodeCount,
    std::span<const uint32_t> primitiveMaterialIndices,
    std::span<const uint32_t> sourcePrimitiveToRuntime,
    const geometry::gltf::GltfInstancingData& mergedInstancing,
    const glm::mat4& importTransform = glm::mat4(1.0f),
    const GltfMergeGraphLimits& limits = {});

} // namespace container::scene
