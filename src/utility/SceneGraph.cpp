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
      activeScene_(other.activeScene_), revision_(other.revision_), identity_(nextGraphIdentity()) {}

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
  activeScene_ = std::exchange(other.activeScene_, std::nullopt);
  activeRenderables_.clear();
  activeRenderablesRevision_ = ~uint64_t{0};
  revision_ = std::exchange(other.revision_, 0);
  identity_ = std::exchange(other.identity_, replacementIdentity);
  return *this;
}

uint32_t SceneGraph::createNode(const glm::mat4& localTransform,
                                uint32_t materialIndex, bool renderable,
                                uint32_t primitiveIndex, std::string name) {
  SceneNode node{};
  node.localTransform = localTransform;
  node.worldTransform = localTransform;
  node.parent = kInvalidNode;
  node.materialIndex = materialIndex;
  node.primitiveIndex = primitiveIndex;
  node.renderable = renderable;
  node.visible = true;
  node.name = std::move(name);
  node.incarnation = nextGraphIdentity();
  return appendNode(std::move(node));
}

uint32_t SceneGraph::appendNode(SceneNode node) {
  if (nodes_.size() >= kInvalidNode)
    throw std::length_error("Scene node indices exhausted");
  const uint32_t nodeIndex = static_cast<uint32_t>(nodes_.size());
  const bool renderable = node.renderable;
  nodes_.push_back(std::move(node));
  roots_.push_back(nodeIndex);
  if (renderable) {
    renderableNodes_.push_back(nodeIndex);
  }
  ++revision_;
  return nodeIndex;
}

bool SceneGraph::removeLastLeaf(uint32_t index, uint64_t incarnation) {
  const auto* node = getNode(index);
  if (!node || size_t(index) + 1 != nodes_.size() ||
      node->incarnation != incarnation || !node->children.empty())
    return false;
  auto& siblings = node->parent == kInvalidNode ? roots_ : nodes_[node->parent].children;
  std::erase(siblings, index);
  if (node->renderable) unregisterRenderable(index);
  nodes_.pop_back();
  ++revision_;
  return true;
}

bool SceneGraph::restoreLastLeaf(uint32_t index, SceneNode node, size_t sibling) {
  if (index != nodes_.size() || !node.incarnation || !node.children.empty() ||
      std::ranges::any_of(nodes_, [&](const auto& live) {
        return live.incarnation == node.incarnation;
      }))
    return false;
  const auto parent = node.parent == kInvalidNode ? std::nullopt : std::optional(node.parent);
  if (parent && *parent >= nodes_.size()) return false;
  if (parent && nodes_[*parent].sceneMembership &&
      (!node.sceneMembership || *nodes_[*parent].sceneMembership != *node.sceneMembership)) return false;
  const auto& siblings = parent ? nodes_[*parent].children : roots_;
  if (sibling > siblings.size()) return false;
  node.parent = kInvalidNode;
  appendNode(std::move(node));
  if (parent) return setParentAt(index, parent, sibling);
  std::rotate(roots_.begin() + sibling, roots_.end() - 1, roots_.end());
  return true;
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
         compatibleSceneParent(child, newParent) && !isDescendant(child, newParent);
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
  activeScene_.swap(candidate.activeScene_);
  activeRenderables_.swap(candidate.activeRenderables_);
  std::swap(activeRenderablesRevision_, candidate.activeRenderablesRevision_);
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

void SceneGraph::setSelectable(uint32_t node, bool selectable) {
  if (node >= nodes_.size() || nodes_[node].selectable == selectable) return;
  nodes_[node].selectable = selectable;
  ++revision_;
}

void SceneGraph::setHoverable(uint32_t node, bool hoverable) {
  if (node >= nodes_.size() || nodes_[node].hoverable == hoverable) return;
  nodes_[node].hoverable = hoverable;
  ++revision_;
}

uint32_t SceneGraph::nearestInteractionAncestor(uint32_t node, bool SceneNode::*flag) const {
  if (!isNodeActive(node)) return kInvalidNode;
  auto target = node;
  size_t steps = 0;
  for (auto current = node; current != kInvalidNode; current = nodes_[current].parent) {
    if (current >= nodes_.size() || steps++ >= nodes_.size()) return kInvalidNode;
    if (!(nodes_[current].*flag)) target = nodes_[current].parent;
  }
  while (const auto* adapter = getNode(target)) {
    if (!adapter->interactionPassthrough) break;
    target = adapter->parent;
  }
  const auto* resolved = getNode(target);
  return resolved && !resolved->interactionBoundary && isNodeActive(target) ? target : kInvalidNode;
}

uint32_t SceneGraph::nearestSelectableAncestor(uint32_t node) const {
  return nearestInteractionAncestor(node, &SceneNode::selectable);
}

uint32_t SceneGraph::nearestHoverableAncestor(uint32_t node) const {
  return nearestInteractionAncestor(node, &SceneNode::hoverable);
}

bool SceneGraph::isNodeEffectivelyVisible(uint32_t nodeIndex) const {
  if (!isNodeActive(nodeIndex)) return false;

  uint32_t current = nodeIndex;
  while (current != kInvalidNode && current < nodes_.size()) {
    const SceneNode& node = nodes_[current];
    if (!node.visible) return false;
    current = node.parent;
  }
  return true;
}

void SceneGraph::setSceneMembership(uint32_t node, std::shared_ptr<const std::vector<uint32_t>> scenes) {
  nodes_.at(node).sceneMembership = std::move(scenes);
  ++revision_;
}

void SceneGraph::setActiveScene(uint32_t scene) {
  if (activeScene_ == scene) return;
  activeScene_ = scene;
  ++revision_;
}

bool SceneGraph::isNodeInScene(uint32_t node, uint32_t scene) const {
  const auto* item = getNode(node);
  return item && item->sceneMembership && std::ranges::binary_search(*item->sceneMembership, scene);
}

bool SceneGraph::isNodeActive(uint32_t node) const {
  const auto* item = getNode(node);
  return item && (!activeScene_ || !item->sceneMembership || isNodeInScene(node, *activeScene_));
}

bool SceneGraph::compatibleSceneParent(uint32_t child, uint32_t parent) const {
  const auto* node = getNode(child);
  const auto* target = getNode(parent);
  if (!node || !target) return false;
  return !target->sceneMembership || node->sceneMembership == target->sceneMembership ||
      (node->sceneMembership && *node->sceneMembership == *target->sceneMembership);
}

const std::vector<uint32_t>& SceneGraph::renderableNodes() const {
  if (!activeScene_) return renderableNodes_;
  if (activeRenderablesRevision_ != revision_) {
    activeRenderables_.clear();
    for (const auto node : renderableNodes_)
      if (isNodeActive(node)) activeRenderables_.push_back(node);
    activeRenderablesRevision_ = revision_;
  }
  return activeRenderables_;
}

void SceneGraph::updateWorldTransforms() {
  auto pending = roots_;
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    auto& node = nodes_[index];
    node.worldTransform = node.parent == kInvalidNode ? node.localTransform :
        nodes_[node.parent].worldTransform * node.localTransform;
    pending.insert(pending.end(), node.children.begin(), node.children.end());
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
