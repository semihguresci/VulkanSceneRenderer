#pragma once

#include <Container/geometry/GltfInstancing.h>
#include <Container/utility/SceneGraph.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace container::scene {

struct GltfMeshAttachment {
  uint32_t ownerNode{SceneGraph::kInvalidNode};
  // Ordinary meshes may have no source mapping. An instanced mesh requires
  // both IDs, and the owner must carry the same sourceNodeIndex.
  int32_t sourceNodeIndex{-1};
  int32_t sourceMeshIndex{-1};
  uint32_t primitiveBase{};
  // One already-resolved runtime material ID for each source mesh primitive.
  std::span<const uint32_t> materialIndices{};
  const geometry::gltf::NodeInstancing* instancing{};
  // Source-local primitive -> runtime geometry ID. Empty retains primitiveBase.
  std::span<const uint32_t> runtimePrimitiveIndices{};
};

struct GltfMeshAttachmentLimits {
  // Aggregate limits include the nodes/renderables already in the graph.
  uint64_t maxRenderableObjects{4096};
  uint64_t maxGraphNodes{SceneGraph::kInvalidNode};
  // Exclusive table bounds. Callers with the tables should pass their sizes;
  // the defaults still reject the engine's invalid uint32 sentinel.
  uint64_t primitiveRangeCount{std::numeric_limits<uint32_t>::max()};
  uint64_t runtimeMaterialCount{std::numeric_limits<uint32_t>::max()};
};

struct AttachedGltfPrimitive {
  uint32_t graphNode{SceneGraph::kInvalidNode};
  // Ordinary children have no EXT instance identity. Instanced children keep
  // exact source IDs even before SceneNode has an instance-identity field.
  std::optional<geometry::gltf::SourceInstanceIdentity> sourceInstance{};
};
enum class GltfHierarchyScope { ActiveScene, CompleteAsset };

// Appends one child per ordinary primitive, or one child per primitive and
// instance, with shared primitive indices and instance-local transforms.
// Instance-major order keeps every instance's mesh primitives adjacent.
// Authored child-node recursion stays with the caller; this never transforms
// the owner or its existing children. It does not update world transforms.
// All input, transform, table and expansion validation finishes before graph
// mutation, so validation failures preserve topology, identity and revisions.
[[nodiscard]] std::vector<AttachedGltfPrimitive> AttachGltfMeshPrimitives(
    SceneGraph& graph, const GltfMeshAttachment& attachment,
    const GltfMeshAttachmentLimits& limits = {});

// Builds a detached scene, sharing the source mesh-major primitive table.
// Material IDs and the optional runtime IDs use that same source-flat order;
// an explicit runtime map covers every source primitive exactly once.
// A failed hierarchy/expansion/transform validation cannot retire the live scene.
[[nodiscard]] SceneGraph BuildGltfSceneHierarchy(
    const tinygltf::Model& source, const geometry::gltf::GltfInstancingData& instancing,
    std::span<const uint32_t> primitiveMaterialIndices,
    const glm::mat4& importTransform = glm::mat4(1.0f),
    const GltfMeshAttachmentLimits& limits = {},
    GltfHierarchyScope scope = GltfHierarchyScope::ActiveScene,
    std::span<const uint32_t> sourcePrimitiveToRuntime = {});

} // namespace container::scene
