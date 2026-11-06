#include <Container/utility/SceneGraph.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace container::scene {

namespace {

uint64_t nextGraphIdentity() {
  static std::atomic<uint64_t> next{1};
  auto identity = next.load(std::memory_order_relaxed);
  do {
    if (identity == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("Scene graph identities exhausted");
  } while (!next.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed));
  return identity;
}

bool matrixFinite(const glm::mat4& matrix) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(matrix[column][row])) {
        return false;
      }
    }
  }
  return true;
}

bool transformBasisInvertible(const glm::mat4& matrix) {
  if (!matrixFinite(matrix)) {
    return false;
  }
  float maxColumnLength = 0.0f;
  for (int column = 0; column < 3; ++column) {
    const glm::vec3 basisColumn(matrix[column]);
    maxColumnLength = std::max(maxColumnLength, glm::length(basisColumn));
  }
  if (maxColumnLength <= 0.0f) {
    return false;
  }

  const float determinant = glm::determinant(glm::mat3(matrix));
  if (!std::isfinite(determinant)) {
    return false;
  }

  const float relativeThreshold = std::numeric_limits<float>::epsilon() *
                                  maxColumnLength * maxColumnLength *
                                  maxColumnLength * 64.0f;
  return std::abs(determinant) > relativeThreshold;
}

}  // namespace

SceneGraph::SceneGraph() : identity_(nextGraphIdentity()) {}

SceneGraph::SceneGraph(const SceneGraph& other)
    : nodes_(other.nodes_), roots_(other.roots_), renderableNodes_(other.renderableNodes_),
      revision_(other.revision_), identity_(nextGraphIdentity()) {}

SceneGraph& SceneGraph::operator=(const SceneGraph& other) {
  if (this != &other) *this = SceneGraph(other);
  return *this;
}

SceneGraph::SceneGraph(SceneGraph&& other) : SceneGraph() { *this = std::move(other); }

SceneGraph& SceneGraph::operator=(SceneGraph&& other) {
  if (this == &other) return *this;
  const auto replacementIdentity = nextGraphIdentity();
  nodes_ = std::move(other.nodes_);
  roots_ = std::move(other.roots_);
  renderableNodes_ = std::move(other.renderableNodes_);
  revision_ = std::exchange(other.revision_, 0);
  identity_ = std::exchange(other.identity_, replacementIdentity);
  return *this;
}

uint32_t SceneGraph::createNode(const glm::mat4& localTransform,
                                uint32_t materialIndex, bool renderable,
                                uint32_t primitiveIndex, std::string name) {
  const uint32_t nodeIndex = static_cast<uint32_t>(nodes_.size());
  SceneNode node{};
  node.localTransform = localTransform;
  node.worldTransform = localTransform;
  node.parent = kInvalidNode;
  node.materialIndex = materialIndex;
  node.primitiveIndex = primitiveIndex;
  node.renderable = renderable;
  node.visible = true;
  node.name = std::move(name);
  nodes_.push_back(std::move(node));
  roots_.push_back(nodeIndex);
  if (renderable) {
    renderableNodes_.push_back(nodeIndex);
  }
  ++revision_;
  return nodeIndex;
}

bool SceneGraph::canSetParent(uint32_t child,
                              std::optional<uint32_t> parentIndex) const {
  if (child >= nodes_.size()) {
    return false;
  }
  if (!parentIndex.has_value()) {
    return true;
  }
  const uint32_t newParent = parentIndex.value();
  return newParent < nodes_.size() && newParent != child &&
         !isDescendant(child, newParent);
}

bool SceneGraph::setParent(uint32_t child, std::optional<uint32_t> parentIndex) {
  if (parentIndex && *parentIndex >= nodes_.size()) return false;
  const auto& destination = parentIndex ? nodes_[*parentIndex].children : roots_;
  return setParentAt(child, parentIndex, destination.size());
}

bool SceneGraph::setParentAt(uint32_t child, std::optional<uint32_t> parentIndex,
                             size_t sibling) {
  if (!canSetParent(child, parentIndex)) return false;
  auto& node = nodes_[child];
  const auto parent = parentIndex.value_or(kInvalidNode);
  if (node.parent == parent) return false;
  auto& destination = parentIndex ? nodes_[parent].children : roots_;
  if (sibling > destination.size()) return false;
  // Insert before detaching. The uint32_t vector has a strong allocation
  // guarantee and grows geometrically on ordinary append.
  destination.insert(destination.begin() + sibling, child);
  auto& previous = node.parent == kInvalidNode ? roots_ : nodes_[node.parent].children;
  std::erase(previous, child);
  node.parent = parent;
  ++revision_;
  return true;
}

std::optional<size_t> SceneGraph::siblingIndex(uint32_t node) const {
  if (node >= nodes_.size()) return std::nullopt;
  const auto parent = nodes_[node].parent;
  const auto& siblings = parent == kInvalidNode ? roots_ : nodes_[parent].children;
  const auto position = std::ranges::find(siblings, node);
  if (position == siblings.end()) return std::nullopt;
  return size_t(position - siblings.begin());
}

std::optional<glm::mat4> SceneGraph::worldPreservingLocalTransform(
    uint32_t child, std::optional<uint32_t> parentIndex) const {
  if (!canSetParent(child, parentIndex)) return std::nullopt;
  const auto parentWorld = parentIndex ? nodes_[*parentIndex].worldTransform : glm::mat4(1.0f);
  if (!transformBasisInvertible(parentWorld)) return std::nullopt;
  auto local = glm::inverse(parentWorld) * nodes_[child].worldTransform;
  const auto affine = [](const glm::mat4& m) {
    return m[0].w == 0 && m[1].w == 0 && m[2].w == 0 && m[3].w == 1;
  };
  if (affine(parentWorld) && affine(nodes_[child].worldTransform)) {
    for (int column = 0; column < 3; ++column) local[column].w = 0;
    local[3].w = 1;
  }
  return matrixFinite(local) ? std::optional(local) : std::nullopt;
}

bool SceneGraph::setParentPreserveWorldTransform(
    uint32_t child, std::optional<uint32_t> parentIndex) {
  updateWorldTransforms();
  const auto local = worldPreservingLocalTransform(child, parentIndex);
  if (!local || !setParent(child, parentIndex)) return false;
  nodes_[child].localTransform = *local;
  updateWorldTransforms();
  return true;
}

void SceneGraph::exchangeNodeEdit(SceneGraph& candidate) noexcept {
  nodes_.swap(candidate.nodes_);
  roots_.swap(candidate.roots_);
  renderableNodes_.swap(candidate.renderableNodes_);
  std::swap(revision_, candidate.revision_);
}

bool SceneGraph::isDescendant(uint32_t ancestor, uint32_t candidate) const {
  if (ancestor >= nodes_.size() || candidate >= nodes_.size()) return false;

  uint32_t current = candidate;
  while (current != kInvalidNode && current < nodes_.size()) {
    if (current == ancestor) return true;
    current = nodes_[current].parent;
  }
  return false;
}

void SceneGraph::setLocalTransform(uint32_t nodeIndex,
                                   const glm::mat4& localTransform) {
  if (nodeIndex >= nodes_.size()) return;
  if (nodes_[nodeIndex].localTransform == localTransform) return;
  nodes_[nodeIndex].localTransform = localTransform;
  ++revision_;
}

void SceneGraph::setRenderable(uint32_t nodeIndex, bool renderable) {
  if (nodeIndex >= nodes_.size()) return;
  if (nodes_[nodeIndex].renderable == renderable) return;
  nodes_[nodeIndex].renderable = renderable;
  if (renderable) {
    registerRenderable(nodeIndex);
  } else {
    unregisterRenderable(nodeIndex);
  }
  ++revision_;
}

void SceneGraph::setVisible(uint32_t nodeIndex, bool visible) {
  if (nodeIndex >= nodes_.size()) return;
  if (nodes_[nodeIndex].visible == visible) return;
  nodes_[nodeIndex].visible = visible;
  ++revision_;
}

bool SceneGraph::isNodeEffectivelyVisible(uint32_t nodeIndex) const {
  if (nodeIndex >= nodes_.size()) return false;

  uint32_t current = nodeIndex;
  while (current != kInvalidNode && current < nodes_.size()) {
    const SceneNode& node = nodes_[current];
    if (!node.visible) return false;
    current = node.parent;
  }
  return true;
}

void SceneGraph::updateWorldTransforms() {
  for (uint32_t root : roots_) {
    updateWorldRecursive(root, glm::mat4(1.0f));
  }
}

const SceneNode* SceneGraph::getNode(uint32_t nodeIndex) const {
  if (nodeIndex >= nodes_.size()) return nullptr;
  return &nodes_[nodeIndex];
}

SceneNode* SceneGraph::getNode(uint32_t nodeIndex) {
  if (nodeIndex >= nodes_.size()) return nullptr;
  return &nodes_[nodeIndex];
}

void SceneGraph::updateWorldRecursive(uint32_t nodeIndex,
                                      const glm::mat4& parentTransform) {
  SceneNode& node = nodes_[nodeIndex];
  node.worldTransform = parentTransform * node.localTransform;
  for (uint32_t child : node.children) {
    updateWorldRecursive(child, node.worldTransform);
  }
}

void SceneGraph::registerRenderable(uint32_t nodeIndex) {
  if (std::ranges::contains(renderableNodes_, nodeIndex)) {
    return;
  }
  renderableNodes_.push_back(nodeIndex);
}

void SceneGraph::unregisterRenderable(uint32_t nodeIndex) {
  std::erase(renderableNodes_, nodeIndex);
}

}  // namespace container::scene
