#pragma once

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace container::scene {

struct SceneNode {
  glm::mat4 localTransform{1.0f};
  glm::mat4 worldTransform{1.0f};
  uint32_t parent = std::numeric_limits<uint32_t>::max();
  uint32_t materialIndex = 0;
  uint32_t primitiveIndex = std::numeric_limits<uint32_t>::max();
  bool renderable = false;
  bool visible = true;
  // Identity of an authored node. Generated primitive children have no source node.
  int32_t sourceNodeIndex{-1};
  // Authored mesh owner survives reparenting of an individual primitive.
  uint32_t meshOwnerGraphNode{std::numeric_limits<uint32_t>::max()};
  std::string name{};
  std::vector<uint32_t> children{};
};

class SceneGraph {
 public:
  static constexpr uint32_t kInvalidNode =
      std::numeric_limits<uint32_t>::max();

  SceneGraph();
  SceneGraph(const SceneGraph&);
  SceneGraph& operator=(const SceneGraph&);
  SceneGraph(SceneGraph&&);
  SceneGraph& operator=(SceneGraph&&);

  uint32_t createNode(const glm::mat4& localTransform, uint32_t materialIndex,
                      bool renderable = false,
                      uint32_t primitiveIndex = kInvalidNode,
                      std::string name = {});
  bool setParent(uint32_t child, std::optional<uint32_t> parentIndex);
  bool setParentAt(uint32_t child, std::optional<uint32_t> parentIndex, size_t sibling);
  [[nodiscard]] std::optional<size_t> siblingIndex(uint32_t node) const;
  [[nodiscard]] std::optional<glm::mat4> worldPreservingLocalTransform(
      uint32_t child, std::optional<uint32_t> parentIndex) const;
  // Exchange an already evaluated copy with the same node membership.
  // The live graph's identity and node incarnations survive a pose edit.
  void exchangeNodeEdit(SceneGraph& candidate) noexcept;
  bool setParentPreserveWorldTransform(
      uint32_t child, std::optional<uint32_t> parentIndex);
  void setLocalTransform(uint32_t nodeIndex, const glm::mat4& localTransform);
  void setRenderable(uint32_t nodeIndex, bool renderable);
  void setVisible(uint32_t nodeIndex, bool visible);
  void updateWorldTransforms();

  [[nodiscard]] const std::vector<uint32_t>& rootNodes() const {
    return roots_;
  }
  [[nodiscard]] const std::vector<uint32_t>& renderableNodes() const {
    return renderableNodes_;
  }
  [[nodiscard]] const SceneNode* getNode(uint32_t nodeIndex) const;
  [[nodiscard]] SceneNode* getNode(uint32_t nodeIndex);
  [[nodiscard]] size_t nodeCount() const { return nodes_.size(); }
  [[nodiscard]] uint64_t revision() const { return revision_; }
  [[nodiscard]] uint64_t identity() const { return identity_; }
  [[nodiscard]] uint64_t nodeIncarnation(uint32_t node) const noexcept {
    // Nodes append throughout this graph's lifetime. Graph replacement gets
    // a different identity even when a new node reuses the same index.
    return node < nodes_.size() ? uint64_t(node) + 1 : 0;
  }
  [[nodiscard]] bool isNodeEffectivelyVisible(uint32_t nodeIndex) const;

 private:
  [[nodiscard]] bool canSetParent(uint32_t child,
                                  std::optional<uint32_t> parentIndex) const;
  [[nodiscard]] bool isDescendant(uint32_t ancestor,
                                  uint32_t candidate) const;
  void updateWorldRecursive(uint32_t nodeIndex,
                            const glm::mat4& parentTransform);
  void registerRenderable(uint32_t nodeIndex);
  void unregisterRenderable(uint32_t nodeIndex);

  std::vector<SceneNode> nodes_{};
  std::vector<uint32_t> roots_{};
  std::vector<uint32_t> renderableNodes_{};
  uint64_t revision_{0};
  uint64_t identity_;
};

}  // namespace container::scene
