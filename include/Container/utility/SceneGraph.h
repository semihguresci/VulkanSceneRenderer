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
  std::string name{};
  std::vector<uint32_t> children{};
};

class SceneGraph {
 public:
  static constexpr uint32_t kInvalidNode =
      std::numeric_limits<uint32_t>::max();

  uint32_t createNode(const glm::mat4& localTransform, uint32_t materialIndex,
                      bool renderable = false,
                      uint32_t primitiveIndex = kInvalidNode,
                      std::string name = {});
  bool setParent(uint32_t child, std::optional<uint32_t> parentIndex);
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
};

}  // namespace container::scene
