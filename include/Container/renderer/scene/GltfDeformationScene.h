#pragma once

#include <Container/geometry/Model.h>
#include <Container/renderer/scene/GltfDeformationPass.h>
#include <Container/utility/SceneGraph.h>

#include <cstdint>
#include <memory>
#include <limits>
#include <span>

namespace container::renderer {

enum class GltfDeformationOrientation {
  Preserved,
  Reversed,
  MixedOrDegenerate,
};

struct GltfDeformationBounds {
  glm::vec3 min{0};
  glm::vec3 max{0};
  glm::vec3 center{0};
  float radius{0};
  bool valid{false};
};

struct GltfDeformationDrawRange {
  uint32_t graphNode{scene::SceneGraph::kInvalidNode};
  // Historical name: this is the runtime bind-geometry slot, not a source ID.
  uint32_t sourcePrimitive{scene::SceneGraph::kInvalidNode};
  uint32_t meshOwnerGraphNode{scene::SceneGraph::kInvalidNode};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  uint32_t vertexOffset{0};
  uint32_t vertexCount{0};
  uint32_t instance{scene::SceneGraph::kInvalidNode};
  GltfDeformationBounds bounds{};
  GltfDeformationOrientation orientation{GltfDeformationOrientation::Preserved};

  [[nodiscard]] bool deformed() const {
    return instance != scene::SceneGraph::kInvalidNode;
  }
};

struct GltfDeformationSceneUpdate {
  bool topologyChanged{false};
  bool geometryChanged{false};
  bool historyReset{false};
};

// CPU reconciliation shared by the raster job builder, bounds, picking and
// CPU ray-scene extraction. Static source ranges remain intact; deforming
// instances append independent vertices and globally remapped index ranges.
// This module does not replace runtime render bindings by itself.
class GltfDeformationScene {
 public:
  GltfDeformationScene();
  ~GltfDeformationScene();
  GltfDeformationScene(const GltfDeformationScene&) = delete;
  GltfDeformationScene& operator=(const GltfDeformationScene&) = delete;

  // Replaces the imported source asset and invalidates submitted pose history.
  // Data is owned; input model/data need not outlive this call. Empty/default
  // and procedural scenes are valid and need no dynamic/GPU instance streams.
  // Optional source deformation definition -> runtime geometry IDs are exact,
  // injective, and may leave procedural holes. Empty retains the source prefix.
  void configure(geometry::gltf::DeformationData data,
                   const geometry::Model& sourceGeometry,
                   std::span<const uint32_t> sourceDeformationToRuntime = {},
                   size_t maxExpandedVertices = std::numeric_limits<uint32_t>::max());

  // Graph world matrices must already be current. meshOwnerGraphNodes is
  // indexed by runtime graph-node index and contains the owning runtime mesh
  // GROUP node ID, or kInvalidNode. Its explicit association survives primitive
  // reparenting; current parentage is never used to guess a skin owner.
  // The owner's sourceNodeIndex selects the glTF node/skin/default weights.
  // evaluatedSource optionally supplies current node/mesh morph weights after
  // animation. Missing/ambiguous joint identities or deleted owners throw.
  // Hierarchy/identity edits reconcile bindings without resetting history when
  // the instance identities/order and source geometry remain compatible.
  GltfDeformationSceneUpdate update(
      const scene::SceneGraph& graph,
      std::span<const uint32_t> meshOwnerGraphNodes,
      const tinygltf::Model* evaluatedSource = nullptr,
      bool resetHistory = false);

  // Call only after successful queue submission. Repeated update() calls,
  // failed/abandoned frames and history resets never advance submitted pose.
  void commitSubmitted();

  [[nodiscard]] std::span<const geometry::Vertex> bindVertices() const;
  [[nodiscard]] std::span<const geometry::Vertex> currentVertices() const;
  [[nodiscard]] std::span<const geometry::Vertex> previousVertices() const;
  [[nodiscard]] std::span<const uint32_t> indices() const;
  [[nodiscard]] const GltfDeformationDrawRange* drawRange(uint32_t graphNode) const;
  [[nodiscard]] std::span<const GltfDeformationDrawRange> drawRanges() const;
  [[nodiscard]] std::span<const GltfDeformationInstance> gpuInstances() const;
  [[nodiscard]] std::span<const GltfDeformationPose> gpuPoses() const;
  [[nodiscard]] uint64_t geometryRevision() const;
  [[nodiscard]] uint64_t topologyRevision() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace container::renderer
