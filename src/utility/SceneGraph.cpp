#include <Container/utility/SceneGraph.h>
#include <Container/utility/SceneNodeAuthoringPolicy.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <stdexcept>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace container::scene {

void SceneGraph::setMaterialIndex(uint32_t nodeIndex,uint32_t materialIndex) {
  auto* node=getNode(nodeIndex);
  if (!node || node->retired || node->materialIndex==materialIndex) return;
  node->materialIndex=materialIndex;
  ++revision_;
}

namespace {

uint64_t nextSceneGraphIdentity() {
  static std::atomic<uint64_t> nextIdentity{1u};
  return nextIdentity.fetch_add(1u, std::memory_order_relaxed);
}

uint64_t nextSceneNodeIncarnation() {
  static std::atomic<uint64_t> next{1};
  auto value=next.load(std::memory_order_relaxed);
  for (;;) {
    if (value==UINT64_MAX) throw std::overflow_error("Scene node lifetime namespace exhausted");
    if (next.compare_exchange_weak(value,value+1,std::memory_order_relaxed)) return value;
  }
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

bool tailPoseValid(std::span<const SceneNodeLocalPose> pose, size_t nodeCount) {
  if (pose.empty()) return true;
  if (pose.size() != nodeCount) return false;
  for (size_t index = 0; index < pose.size(); ++index)
    if (pose[index].node != index || !matrixFinite(pose[index].localTransform)) return false;
  return true;
}

bool placementBudgetValid(std::span<const SceneNode> nodes,
    std::span<const uint32_t> roots, std::span<const uint32_t> renderables,
    const std::optional<std::vector<uint32_t>>& activeRoots,
    const std::optional<std::vector<uint32_t>>& targetRoots,
    const ScenePlacementLimits& limits) {
  if (!limits.maxNodes || !limits.maxEdges || !limits.maxPayloadBytes ||
      nodes.size() >= SceneGraph::kInvalidNode || nodes.size() > limits.maxNodes) return false;
  uint64_t bytes = 0, edges = 0;
  const auto charge = [&](uint64_t count, uint64_t stride) {
    if (count > (limits.maxPayloadBytes - bytes) / stride) return false;
    bytes += count * stride; return true;
  };
  const auto references = [&](uint64_t count) {
    if (count > limits.maxEdges - edges) return false;
    edges += count; return true;
  };
  // One detached graph plus forest/world/membership traversal and replacement
  // child-list capacity. Account before allocating or cloning any payload.
  if (!charge(1, sizeof(SceneGraph)) || !charge(nodes.size(), sizeof(SceneNode) + sizeof(uint64_t) + 32 * sizeof(uint32_t)) ||
      !charge(roots.size(), 3 * sizeof(uint32_t)) || !charge(renderables.size(), sizeof(uint32_t))) return false;
  for (const auto* view : {&activeRoots, &targetRoots})
    if (*view && (!references((*view)->size()) || !charge((*view)->size(), 2 * sizeof(uint32_t)))) return false;
  for (const auto& node : nodes)
    if (!references(node.children.size()) || !charge(node.children.size(), 3 * sizeof(uint32_t)) ||
        !charge(node.name.size(), 1) || !charge(1, 1)) return false;
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

uint64_t topologyFingerprint(std::span<const SceneNode> nodes,
                             std::span<const uint32_t> roots,
                             std::span<const uint32_t> renderables,
                             bool includeRetired = true) {
  uint64_t fingerprint = 14695981039346656037ull;
  const auto append = [&](uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
      fingerprint ^= (value >> (byte * 8u)) & 0xffu;
      fingerprint *= 1099511628211ull;
    }
  };
  append(nodes.size());
  for (const auto &node : nodes) {
    append(node.parent);
    append(node.children.size());
    for (const auto child : node.children)
      append(child);
    append(node.renderable);
    append(node.meshOwnerGraphNode);
    append(static_cast<uint32_t>(node.sourceNodeIndex));
    append(static_cast<uint32_t>(node.sourceMeshIndex));
    append(static_cast<uint32_t>(node.sourcePrimitiveIndex));
    append(static_cast<uint32_t>(node.sourceInstanceIndex));
    append(node.editorSceneRoot);
    append(node.interactionRootBoundary);
    if (includeRetired) append(node.retired);
  }
  append(roots.size());
  for (const auto root : roots)
    append(root);
  append(renderables.size());
  for (const auto renderable : renderables)
    append(renderable);
  return fingerprint;
}

void validateAppendForest(std::span<const SceneNode> nodes,
                          std::span<const uint32_t> roots,
                          std::span<const uint32_t> renderables) {
  const auto reject = [](const char* message) {
    throw std::invalid_argument(std::string("SceneGraph append: ") + message);
  };
  if (nodes.size() > SceneGraph::kInvalidNode)
    reject("node count exceeds addressable indices");
  std::vector<uint8_t> incoming(nodes.size(), 0);
  std::vector<uint8_t> rootSeen(nodes.size(), 0);
  std::vector<uint8_t> renderableSeen(nodes.size(), 0);
  for (size_t index = 0; index < nodes.size(); ++index) {
    const auto& node = nodes[index];
    if (node.parent != SceneGraph::kInvalidNode &&
        (node.parent >= nodes.size() || node.parent == index))
      reject("invalid parent reference");
    if (node.meshOwnerGraphNode != SceneGraph::kInvalidNode &&
        (node.meshOwnerGraphNode >= nodes.size() || node.meshOwnerGraphNode == index ||
         (!node.retired && nodes[node.meshOwnerGraphNode].retired)))
      reject("invalid or retired live mesh owner");
    if (!node.retired && node.parent != SceneGraph::kInvalidNode && nodes[node.parent].retired)
      reject("live node has a retired parent");
    if (node.retired && (node.editorSceneRoot || node.interactionRootBoundary))
      reject("structural editor/import wrappers cannot be retired");
    if (node.sourceNodeIndex < -1 || node.sourceMeshIndex < -1 ||
        node.sourcePrimitiveIndex < -1 || node.sourceInstanceIndex < -1 ||
        ((node.sourceMeshIndex < 0) != (node.sourcePrimitiveIndex < 0)) ||
        (node.sourceInstanceIndex >= 0 && node.sourceMeshIndex < 0))
      reject("invalid source identity");
    for (const auto child : node.children) {
      if (child >= nodes.size() || child == index ||
          nodes[child].parent != index || incoming[child])
        reject("invalid, duplicate or inconsistent child reference");
      incoming[child] = 1;
    }
  }
  for (const auto root : roots) {
    if (root >= nodes.size() || nodes[root].parent != SceneGraph::kInvalidNode || rootSeen[root])
      reject("invalid or duplicate root reference");
    rootSeen[root] = 1;
  }
  for (const auto id : renderables) {
    if (id >= nodes.size() || !nodes[id].renderable || renderableSeen[id])
      reject("invalid or duplicate renderable reference");
    renderableSeen[id] = 1;
  }
  for (size_t index = 0; index < nodes.size(); ++index) {
    const bool isRoot = nodes[index].parent == SceneGraph::kInvalidNode;
    if (bool(rootSeen[index]) != isRoot || bool(incoming[index]) == isRoot ||
        bool(renderableSeen[index]) != nodes[index].renderable)
      reject("root, parent, child or renderable index is missing");
  }
  // Each valid forest node has exactly one incoming edge or root entry. A
  // parent-first traversal therefore visits every reachable node once; any
  // remaining component is a disconnected cycle. No repeated child searches,
  // ancestor walks or recursive calls are needed, including for wide trees.
  std::vector<uint32_t> pending(roots.begin(), roots.end());
  pending.reserve(nodes.size());
  for (size_t next = 0; next < pending.size(); ++next) {
    const auto& children = nodes[pending[next]].children;
    pending.insert(pending.end(), children.begin(), children.end());
  }
  if (pending.size() != nodes.size())
    reject("global forest contains a disconnected cycle");
}

void validateRetirementBudget(std::span<const SceneNode> nodes,
    std::span<const uint32_t> roots, std::span<const uint32_t> renderables,
    const std::optional<std::vector<uint32_t>>& activeRoots,
    const SceneRetirementLimits& limits) {
  const auto reject = [](const char* message) {
    throw std::invalid_argument(std::string("SceneGraph retirement: ") + message);
  };
  if (!limits.maxNodes || !limits.maxEdges || !limits.maxPayloadBytes)
    reject("budgets must be positive");
  if (nodes.size() >= SceneGraph::kInvalidNode || nodes.size() > limits.maxNodes)
    reject("node budget exceeded");
  uint64_t bytes = 0, edges = 0;
  const auto charge = [&](uint64_t count, uint64_t stride) {
    if (count > (limits.maxPayloadBytes - bytes) / stride) reject("detached storage budget exceeded");
    bytes += count * stride;
  };
  // Candidate payload plus forest/cache/work/receipt scratch, before any copy.
  charge(nodes.size(), sizeof(SceneNode) + 2 * sizeof(uint64_t) + 32 * sizeof(uint32_t));
  charge(roots.size(), sizeof(uint32_t));
  charge(renderables.size(), sizeof(uint32_t));
  if (activeRoots) charge(activeRoots->size(), 3 * sizeof(uint32_t));
  for (const auto& node : nodes) {
    if (node.children.size() > limits.maxEdges - edges) reject("edge budget exceeded");
    edges += node.children.size();
    charge(node.children.size(), sizeof(uint32_t));
    charge(node.name.size(), 1); charge(1, 1);
  }
  if (activeRoots && activeRoots->size() > limits.maxEdges - edges)
    reject("active view metadata budget exceeded");
}

void validateNewAppendNode(const SceneNode& node, size_t index, size_t count) {
  const auto reject = [](const char* message) {
    throw std::invalid_argument(std::string("SceneGraph append: ") + message);
  };
  if (!matrixFinite(node.localTransform) || !matrixFinite(node.worldTransform))
    reject("new node local/world transform is nonfinite");
  if ((node.meshOwnerGraphNode != SceneGraph::kInvalidNode && node.meshOwnerGraphNode >= count) ||
      node.meshOwnerGraphNode == index)
    reject("new node has an invalid mesh owner");
  if (node.sourceNodeIndex < -1 || node.sourceMeshIndex < -1 ||
      node.sourcePrimitiveIndex < -1 || node.sourceInstanceIndex < -1 ||
      ((node.sourceMeshIndex < 0) != (node.sourcePrimitiveIndex < 0)) ||
      (node.sourceInstanceIndex >= 0 && node.sourceMeshIndex < 0))
    reject("new node has an invalid source identity");
}

} // namespace

struct SceneGraph::PreparedAppend::State {
  explicit State(SceneGraph&& value) noexcept : candidate(std::move(value)) {}
  SceneGraph candidate;
  const SceneGraph* owner{};
  uint64_t identity{}, revision{}, hierarchyRevision{};
  size_t retainedNodeCount{};
  bool consumed{false};
};

SceneGraph::PreparedAppend::PreparedAppend() noexcept = default;
SceneGraph::PreparedAppend::~PreparedAppend() = default;
SceneGraph::PreparedAppend::PreparedAppend(PreparedAppend&&) noexcept = default;
SceneGraph::PreparedAppend& SceneGraph::PreparedAppend::operator=(PreparedAppend&&) noexcept = default;
SceneGraph::PreparedAppend::operator bool() const noexcept {
  return state_ && !state_->consumed;
}
const SceneGraph* SceneGraph::PreparedAppend::candidateGraph() const noexcept {
  return *this ? &state_->candidate : nullptr;
}

struct SceneGraph::PreparedTailChange::State {
  explicit State(SceneGraph&& value) noexcept : candidate(std::move(value)) {}
  SceneGraph candidate;
  std::optional<DetachedTail> detached;
  const SceneGraph* owner{};
  uint64_t identity{}, revision{}, hierarchyRevision{};
  size_t nodeCount{};
  bool consumed{false};
};

SceneGraph::PreparedTailChange::PreparedTailChange() noexcept = default;
SceneGraph::PreparedTailChange::~PreparedTailChange() = default;
SceneGraph::PreparedTailChange::PreparedTailChange(PreparedTailChange&&) noexcept = default;
SceneGraph::PreparedTailChange& SceneGraph::PreparedTailChange::operator=(PreparedTailChange&&) noexcept = default;
SceneGraph::PreparedTailChange::operator bool() const noexcept {
  return state_ && !state_->consumed;
}
const SceneGraph* SceneGraph::PreparedTailChange::candidateGraph() const noexcept {
  return *this ? &state_->candidate : nullptr;
}
std::optional<SceneGraph::DetachedTail>
SceneGraph::PreparedTailChange::takeDetachedTail() noexcept {
  if (!state_ || !state_->consumed || !state_->detached) return std::nullopt;
  auto result = std::move(state_->detached);
  state_->detached.reset();
  return result;
}

struct SceneGraph::PreparedPlacement::State {
  explicit State(SceneGraph&& value) noexcept : candidate(std::move(value)) {}
  SceneGraph candidate;
  const SceneGraph* owner{};
  uint64_t identity{}, revision{}, hierarchyRevision{};
  size_t nodeCount{};
  bool consumed{false};
};

SceneGraph::PreparedPlacement::PreparedPlacement() noexcept = default;
SceneGraph::PreparedPlacement::~PreparedPlacement() = default;
SceneGraph::PreparedPlacement::PreparedPlacement(PreparedPlacement&&) noexcept = default;
SceneGraph::PreparedPlacement& SceneGraph::PreparedPlacement::operator=(PreparedPlacement&&) noexcept = default;
SceneGraph::PreparedPlacement::operator bool() const noexcept { return state_ && !state_->consumed; }
const SceneGraph* SceneGraph::PreparedPlacement::candidateGraph() const noexcept {
  return *this ? &state_->candidate : nullptr;
}

struct SceneGraph::SubtreeRetirement::State {
  uint64_t graphIdentity{}, topology{};
  size_t nodeCount{};
  uint32_t root{kInvalidNode};
  std::vector<uint32_t> nodes;
  std::vector<uint64_t> incarnations;
  std::vector<uint8_t> before;
  std::optional<std::vector<uint32_t>> activeRoots;
};

SceneGraph::SubtreeRetirement::SubtreeRetirement() noexcept = default;
SceneGraph::SubtreeRetirement::~SubtreeRetirement() = default;
SceneGraph::SubtreeRetirement::SubtreeRetirement(SubtreeRetirement&&) noexcept = default;
SceneGraph::SubtreeRetirement& SceneGraph::SubtreeRetirement::operator=(SubtreeRetirement&&) noexcept = default;
SceneGraph::SubtreeRetirement::operator bool() const noexcept { return bool(state_); }
uint32_t SceneGraph::SubtreeRetirement::rootNode() const noexcept {
  return state_ ? state_->root : kInvalidNode;
}
std::span<const uint32_t> SceneGraph::SubtreeRetirement::nodes() const noexcept {
  return state_ ? std::span<const uint32_t>(state_->nodes) : std::span<const uint32_t>{};
}

struct SceneGraph::PreparedRetirement::State {
  explicit State(SceneGraph&& value) noexcept : candidate(std::move(value)) {}
  SceneGraph candidate;
  std::optional<SubtreeRetirement> receipt;
  const SceneGraph* owner{};
  uint64_t identity{}, revision{}, hierarchyRevision{};
  size_t nodeCount{};
  bool consumed{false};
};
SceneGraph::PreparedRetirement::PreparedRetirement() noexcept = default;
SceneGraph::PreparedRetirement::~PreparedRetirement() = default;
SceneGraph::PreparedRetirement::PreparedRetirement(PreparedRetirement&&) noexcept = default;
SceneGraph::PreparedRetirement& SceneGraph::PreparedRetirement::operator=(PreparedRetirement&&) noexcept = default;
SceneGraph::PreparedRetirement::operator bool() const noexcept { return state_ && !state_->consumed; }
const SceneGraph* SceneGraph::PreparedRetirement::candidateGraph() const noexcept {
  return *this ? &state_->candidate : nullptr;
}
std::optional<SceneGraph::SubtreeRetirement> SceneGraph::PreparedRetirement::takeRetirement() noexcept {
  if (!state_ || !state_->consumed || !state_->receipt) return std::nullopt;
  auto result = std::move(state_->receipt);
  state_->receipt.reset();
  return result;
}

SceneGraph::PreparedRetirement SceneGraph::prepareRetireSubtree(uint32_t root,
    const SceneRetirementLimits& limits) const {
  validateRetirementBudget(nodes_, roots_, renderableNodes_, activeRoots_, limits);
  validateAppendForest(nodes_, roots_, renderableNodes_);
  if (root >= nodes_.size() || nodes_[root].retired)
    throw std::invalid_argument("SceneGraph retirement: missing or already retired root");
  SubtreeRetirement receipt;
  receipt.state_ = std::make_unique<SubtreeRetirement::State>();
  auto& saved = *receipt.state_;
  saved.graphIdentity = identity_; saved.nodeCount = nodes_.size(); saved.root = root;
  saved.topology = topologyFingerprint(nodes_, roots_, renderableNodes_, false);
  saved.activeRoots = activeRoots_;
  saved.nodes.push_back(root);
  for (size_t next = 0; next < saved.nodes.size(); ++next) {
    const auto& node = nodes_[saved.nodes[next]];
    if (node.editorSceneRoot || node.interactionRootBoundary)
      throw std::invalid_argument("SceneGraph retirement: subtree contains a structural wrapper");
    saved.before.push_back(node.retired);
    saved.incarnations.push_back(nodeIncarnation(saved.nodes[next]));
    saved.nodes.insert(saved.nodes.end(), node.children.begin(), node.children.end());
  }
  SceneGraph candidate(*this);
  for (const auto node : saved.nodes) candidate.nodes_[node].retired = true;
  // Also rejects immutable mesh owners referenced outside the retired closure.
  validateAppendForest(candidate.nodes_, candidate.roots_, candidate.renderableNodes_);
  candidate.updateWorldTransforms();
  for (const auto& node : candidate.nodes_)
    if (!matrixFinite(node.localTransform) || !matrixFinite(node.worldTransform))
      throw std::invalid_argument("SceneGraph retirement: nonfinite evaluated transform");
  if (candidate.activeRoots_)
    for (const auto id : *candidate.activeRoots_)
      if (id >= candidate.nodes_.size()) throw std::invalid_argument("SceneGraph retirement: invalid active root");
  PreparedRetirement prepared;
  prepared.state_ = std::make_unique<PreparedRetirement::State>(std::move(candidate));
  auto& state = *prepared.state_;
  state.candidate.refreshActiveMembership();
  state.receipt.emplace(std::move(receipt));
  state.owner = this; state.identity = identity_; state.revision = revision_;
  state.hierarchyRevision = hierarchyRevision_; state.nodeCount = nodes_.size();
  return prepared;
}

SceneGraph::PreparedRetirement SceneGraph::prepareRetirementChange(
    const SubtreeRetirement& receipt, bool retired,
    std::span<const SceneNodeLocalPose> capturedPose,
    const SceneRetirementLimits& limits) const {
  validateRetirementBudget(nodes_, roots_, renderableNodes_, activeRoots_, limits);
  if (!receipt.state_) return {};
  const auto& saved = *receipt.state_;
  if (saved.graphIdentity != identity_ || saved.nodeCount != nodes_.size() ||
      saved.nodes.empty() || saved.nodes.size() != saved.before.size() || saved.incarnations.size()!=saved.nodes.size() ||
      saved.activeRoots != activeRoots_ ||
      saved.topology != topologyFingerprint(nodes_, roots_, renderableNodes_, false)) return {};
  for(size_t i=0;i<saved.nodes.size();++i)
    if(!saved.incarnations[i] || nodeIncarnation(saved.nodes[i])!=saved.incarnations[i]) return {};
  validateAppendForest(nodes_, roots_, renderableNodes_);
  for (size_t index = 0; index < saved.nodes.size(); ++index)
    if (saved.nodes[index] >= nodes_.size() || nodes_[saved.nodes[index]].retired !=
        (retired ? bool(saved.before[index]) : true)) return {};
  if (capturedPose.size() > nodes_.size())
    throw std::invalid_argument("SceneGraph retirement: pose metadata budget exceeded");
  std::vector<uint8_t> posed(nodes_.size(), 0);
  for (const auto& pose : capturedPose) {
    if (pose.node >= nodes_.size() || posed[pose.node] || !matrixFinite(pose.localTransform))
      throw std::invalid_argument("SceneGraph retirement: invalid, duplicate or nonfinite pose");
    posed[pose.node] = 1;
  }
  SceneGraph candidate(*this);
  for (size_t index = 0; index < saved.nodes.size(); ++index)
    candidate.nodes_[saved.nodes[index]].retired = retired || saved.before[index];
  for (const auto& pose : capturedPose) candidate.nodes_[pose.node].localTransform = pose.localTransform;
  validateAppendForest(candidate.nodes_, candidate.roots_, candidate.renderableNodes_);
  candidate.updateWorldTransforms();
  for (const auto& node : candidate.nodes_)
    if (!matrixFinite(node.localTransform) || !matrixFinite(node.worldTransform))
      throw std::invalid_argument("SceneGraph retirement: nonfinite evaluated transform");
  if (candidate.activeRoots_)
    for (const auto id : *candidate.activeRoots_)
      if (id >= candidate.nodes_.size()) throw std::invalid_argument("SceneGraph retirement: invalid active root");
  PreparedRetirement prepared;
  prepared.state_ = std::make_unique<PreparedRetirement::State>(std::move(candidate));
  auto& state = *prepared.state_;
  state.candidate.refreshActiveMembership();
  state.owner = this; state.identity = identity_; state.revision = revision_;
  state.hierarchyRevision = hierarchyRevision_; state.nodeCount = nodes_.size();
  return prepared;
}

bool SceneGraph::publishRetirement(PreparedRetirement& prepared) noexcept {
  if (!prepared.state_) return false;
  auto& state = *prepared.state_;
  if (state.consumed || state.owner != this || state.identity != identity_ ||
      state.revision != revision_ || state.hierarchyRevision != hierarchyRevision_ ||
      state.nodeCount != nodes_.size()) return false;
  auto& candidate = state.candidate;
  nodes_.swap(candidate.nodes_); roots_.swap(candidate.roots_);
  nodeIncarnations_.swap(candidate.nodeIncarnations_);
  renderableNodes_.swap(candidate.renderableNodes_); activeRoots_.swap(candidate.activeRoots_);
  activeMembership_.swap(candidate.activeMembership_); activeRenderables_.swap(candidate.activeRenderables_);
  ++revision_; ++hierarchyRevision_;
  activeMembershipRevision_ = hierarchyRevision_;
  state.consumed = true;
  return true;
}

SceneGraph::PreparedAppend SceneGraph::prepareAppend(SceneGraph candidate) const {
  const size_t firstNode = nodes_.size();
  const auto reject = [](const char* message) {
    throw std::invalid_argument(std::string("SceneGraph append: ") + message);
  };
  if (candidate.nodes_.size() <= firstNode || candidate.nodes_.size() > kInvalidNode)
    reject("candidate must append an addressable nonempty tail");
  if (nodeIncarnations_.size()!=firstNode || candidate.nodeIncarnations_.size()!=candidate.nodes_.size() ||
      !std::equal(nodeIncarnations_.begin(),nodeIncarnations_.end(),candidate.nodeIncarnations_.begin()))
    reject("candidate replaced a retained node lifetime");
  const auto preservesPrefix = [firstNode](const auto& old, const auto& proposed) {
    return proposed.size() >= old.size() && std::equal(old.begin(), old.end(), proposed.begin()) &&
        std::all_of(proposed.begin() + old.size(), proposed.end(),
                    [firstNode](uint32_t id) { return id >= firstNode; });
  };
  if (!preservesPrefix(roots_, candidate.roots_) ||
      !preservesPrefix(renderableNodes_, candidate.renderableNodes_))
    reject("retained root/renderable order changed");
  const auto& previousView = activeRoots_ ? *activeRoots_ : roots_;
  const auto preservesActiveView = [&] {
    if (!candidate.activeRoots_) return !activeRoots_;
    if (preservesPrefix(previousView, *candidate.activeRoots_)) return true;
    // A procedural session may select its editor root. Refine that context-only
    // root into its ORIGINAL children before appending inactive canonical data
    // beneath it. This preserves every old source/renderable membership while
    // keeping the editor root available as context. No other old root may be
    // omitted/reordered, and new children never enter this deterministic view.
    std::vector<uint32_t> refined;
    std::vector<uint8_t> seen(firstNode, 0);
    const auto append = [&](uint32_t id) {
      if (id >= firstNode) reject("retained active view has an invalid root");
      if (!seen[id]) { seen[id] = 1; refined.push_back(id); }
    };
    for (const auto id : previousView) {
      if (id >= firstNode) reject("retained active view has an invalid root");
      const auto& node = nodes_[id];
      if (node.editorSceneRoot && !node.renderable && node.sourceNodeIndex == -1)
        for (const auto child : node.children) append(child);
      else append(id);
    }
    return preservesPrefix(refined, *candidate.activeRoots_);
  };
  if (!preservesActiveView()) {
    reject("retained active view changed outside the editor-root refinement");
  }
  for (size_t index = 0; index < firstNode; ++index) {
    const auto& retained = nodes_[index];
    const auto& proposed = candidate.nodes_[index];
    if (!preservesPrefix(retained.children, proposed.children))
      reject("retained child order changed");
    auto expected = retained;
    expected.children = proposed.children;
    if (expected != proposed)
      reject("retained node payload changed");
  }
  validateAppendForest(candidate.nodes_, candidate.roots_, candidate.renderableNodes_);
  for (size_t index = firstNode; index < candidate.nodes_.size(); ++index)
    validateNewAppendNode(candidate.nodes_[index], index, candidate.nodes_.size());
  if (candidate.activeRoots_)
    for (const auto id : *candidate.activeRoots_)
      if (id >= candidate.nodes_.size()) reject("active root is outside the candidate graph");
  PreparedAppend prepared;
  prepared.state_ = std::make_unique<PreparedAppend::State>(std::move(candidate));
  auto& state = *prepared.state_;
  // SceneGraph moves deliberately discard lazy caches. Rebuild only after
  // the final move into proof-owned storage; subsequent proof moves transfer
  // its unique_ptr and leave these prepared vectors intact. The validated
  // forest makes this O(nodes+edges), including shared context ancestors.
  state.candidate.refreshActiveMembership();
  state.owner = this;
  state.identity = identity_;
  state.revision = revision_;
  state.hierarchyRevision = hierarchyRevision_;
  state.retainedNodeCount = firstNode;
  return prepared;
}

SceneGraph SceneGraph::makeAppendCandidate(std::vector<SceneNode> tail) const {
  const size_t first = nodes_.size();
  if (tail.empty() || first > kInvalidNode || tail.size() > kInvalidNode - first)
    throw std::invalid_argument("SceneGraph append: bulk tail must be addressable and nonempty");
  validateAppendForest(nodes_, roots_, renderableNodes_);
  const auto count = first + tail.size();
  std::vector<size_t> addedChildren(first, 0);
  size_t addedRoots = 0, addedRenderables = 0;
  for (size_t offset = 0; offset < tail.size(); ++offset) {
    const auto& node = tail[offset];
    validateNewAppendNode(node, first + offset, count);
    if (node.parent == kInvalidNode) ++addedRoots;
    else if (node.parent < first) ++addedChildren[node.parent];
    else if (node.parent >= count)
      throw std::invalid_argument("SceneGraph append: bulk parent is outside the graph");
    for (const auto child : node.children)
      if (child < first)
        throw std::invalid_argument("SceneGraph append: bulk tail cannot adopt retained children");
    addedRenderables += node.renderable;
  }
  SceneGraph candidate(*this);
  candidate.nodes_.reserve(count);
  candidate.nodeIncarnations_.reserve(count);
  candidate.roots_.reserve(roots_.size() + addedRoots);
  candidate.renderableNodes_.reserve(renderableNodes_.size() + addedRenderables);
  for (size_t id = 0; id < first; ++id)
    if (addedChildren[id]) candidate.nodes_[id].children.reserve(nodes_[id].children.size() + addedChildren[id]);
  for (size_t offset = 0; offset < tail.size(); ++offset) {
    const auto id = static_cast<uint32_t>(first + offset);
    const auto parent = tail[offset].parent;
    if (parent == kInvalidNode) candidate.roots_.push_back(id);
    else if (parent < first) candidate.nodes_[parent].children.push_back(id);
    if (tail[offset].renderable) candidate.renderableNodes_.push_back(id);
    candidate.nodes_.push_back(std::move(tail[offset]));
    candidate.nodeIncarnations_.push_back(nextSceneNodeIncarnation());
  }
  validateAppendForest(candidate.nodes_, candidate.roots_, candidate.renderableNodes_);
  ++candidate.revision_; ++candidate.hierarchyRevision_;
  candidate.activeMembershipRevision_ = std::numeric_limits<uint64_t>::max();
  return candidate;
}

SceneGraph SceneGraph::makeEmptyNodeCandidate(
    std::optional<uint32_t> parent, std::string name) const {
  ValidateSceneNodeName(name);
  if (nodes_.size() >= kInvalidNode)
    throw std::invalid_argument("SceneGraph empty node: node count exceeds addressable indices");
  if (parent && isNodeRetired(*parent))
    throw std::invalid_argument("SceneGraph empty node: parent is retired");
  if (parent && !isNodeInActiveHierarchy(*parent)) {
    const auto* context = getNode(*parent);
    if (!context || !context->editorSceneRoot || context->renderable ||
        context->sourceNodeIndex != -1 || context->interactionRootBoundary)
      throw std::invalid_argument("SceneGraph empty node: parent is outside the active hierarchy");
  }

  SceneNode empty;
  empty.parent = parent.value_or(kInvalidNode);
  empty.name = std::move(name);
  std::vector<SceneNode> tail;
  tail.push_back(std::move(empty));
  auto candidate = makeAppendCandidate(std::move(tail));
  const auto id = static_cast<uint32_t>(nodes_.size());

  // The bulk assembly above validates the complete forest before following
  // this chain. Evaluate in parent-first order, matching updateWorldTransforms
  // even when a retained cached world predates its current local transform.
  std::vector<uint32_t> ancestors;
  for (auto ancestor = parent.value_or(kInvalidNode); ancestor != kInvalidNode;
       ancestor = nodes_[ancestor].parent)
    ancestors.push_back(ancestor);
  glm::mat4 world(1.0f);
  for (auto ancestor = ancestors.rbegin(); ancestor != ancestors.rend(); ++ancestor)
    world = world * nodes_[*ancestor].localTransform;
  if (!matrixFinite(world))
    throw std::invalid_argument("SceneGraph empty node: inherited world transform is nonfinite");
  candidate.nodes_[id].worldTransform = world;

  if (activeRoots_ && (!parent || !isNodeActive(*parent))) {
    auto view = *activeRoots_;
    view.push_back(id);
    candidate.setActiveRoots(view);
  }
  return candidate;
}

bool SceneGraph::publishAppend(PreparedAppend& prepared) noexcept {
  if (!prepared.state_) return false;
  auto& state = *prepared.state_;
  if (state.consumed || state.owner != this || state.identity != identity_ ||
      state.revision != revision_ || state.hierarchyRevision != hierarchyRevision_ ||
      state.retainedNodeCount != nodes_.size())
    return false;
  auto& candidate = state.candidate;
  static_assert(std::is_nothrow_swappable_v<decltype(activeRoots_)>);
  nodes_.swap(candidate.nodes_);
  nodeIncarnations_.swap(candidate.nodeIncarnations_);
  roots_.swap(candidate.roots_);
  renderableNodes_.swap(candidate.renderableNodes_);
  activeRoots_.swap(candidate.activeRoots_);
  activeMembership_.swap(candidate.activeMembership_);
  activeRenderables_.swap(candidate.activeRenderables_);
  ++revision_;
  ++hierarchyRevision_;
  // The old cache is invalidated by replacement; the prepared view is already
  // complete for this topology, so the first reader need not allocate either.
  activeMembershipRevision_ = hierarchyRevision_;
  state.consumed = true;
  return true;
}

SceneGraph::SceneGraph() : identity_(nextSceneGraphIdentity()) {}

SceneGraph::SceneGraph(const SceneGraph &other)
    : nodes_(other.nodes_), nodeIncarnations_(other.nodeIncarnations_), roots_(other.roots_),
      renderableNodes_(other.renderableNodes_), revision_(other.revision_),
      identity_(nextSceneGraphIdentity()),
      hierarchyRevision_(other.hierarchyRevision_),activeRoots_(other.activeRoots_) {}

SceneGraph &SceneGraph::operator=(const SceneGraph &other) {
  if (this != &other) {
    // Finish every potentially throwing copy before replacing live topology.
    SceneGraph copy(other);
    *this = std::move(copy);
  }
  return *this;
}

SceneGraph::SceneGraph(SceneGraph &&other) noexcept : SceneGraph() {
  *this = std::move(other);
}

SceneGraph &SceneGraph::operator=(SceneGraph &&other) noexcept {
  if (this != &other) {
    nodes_ = std::move(other.nodes_);
    nodeIncarnations_ = std::move(other.nodeIncarnations_);
    roots_ = std::move(other.roots_);
    renderableNodes_ = std::move(other.renderableNodes_);
    revision_ = other.revision_;
    hierarchyRevision_ = other.hierarchyRevision_;
    activeRoots_=std::move(other.activeRoots_);
    activeMembershipRevision_=std::numeric_limits<uint64_t>::max();
    activeMembership_.clear(); activeRenderables_.clear();
    identity_ = nextSceneGraphIdentity();
    // A moved-from graph is a fresh empty graph, including its revision key.
    other.nodes_.clear();
    other.nodeIncarnations_.clear();
    other.roots_.clear();
    other.renderableNodes_.clear();
    other.revision_ = 0u;
    other.hierarchyRevision_ = 0u;
    other.activeRoots_.reset(); other.activeMembership_.clear(); other.activeRenderables_.clear();
    other.activeMembershipRevision_=std::numeric_limits<uint64_t>::max();
    other.identity_ = nextSceneGraphIdentity();
  }
  return *this;
}

uint32_t SceneGraph::createNode(const glm::mat4 &localTransform,
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
  const auto incarnation=nextSceneNodeIncarnation();
  // Reserve this parallel private table before exposing a new public payload.
  // Geometric growth preserves linear import assembly.
  if (nodeIncarnations_.size()==nodeIncarnations_.capacity())
    nodeIncarnations_.reserve(std::max<size_t>(8,nodeIncarnations_.capacity()*2));
  nodes_.push_back(std::move(node));
  nodeIncarnations_.push_back(incarnation);
  roots_.push_back(nodeIndex);
  if (renderable) {
    renderableNodes_.push_back(nodeIndex);
  }
  ++revision_;
  ++hierarchyRevision_;
  return nodeIndex;
}

bool SceneGraph::canSetParent(uint32_t child,
                              std::optional<uint32_t> parentIndex) const {
  if (child >= nodes_.size() || nodes_[child].retired) {
    return false;
  }
  if (!parentIndex.has_value()) {
    return true;
  }
  const uint32_t newParent = parentIndex.value();
  // Public topology is a valid forest. A just-created last root has no
  // descendants, so parent-before-child import assembly cannot form a cycle.
  // All ordinary reparenting retains the full ancestry validation below.
  const bool freshLeaf = child + 1 == nodes_.size() && !roots_.empty() &&
      roots_.back() == child && nodes_[child].parent == kInvalidNode &&
      nodes_[child].children.empty();
  return newParent < nodes_.size() && !nodes_[newParent].retired && newParent != child &&
         (freshLeaf || !isDescendant(child, newParent));
}

bool SceneGraph::setParent(uint32_t child, std::optional<uint32_t> parentIndex) {
  if (!canSetParent(child, parentIndex)) return false;

  SceneNode& childNode = nodes_[child];
  const uint32_t newParent =
      parentIndex.has_value() ? parentIndex.value() : kInvalidNode;
  if (childNode.parent == newParent) {
    return false;
  }

  // Allocate before changing either list so allocation failure cannot detach
  // a live node halfway through an edit/history action.
  auto &destination = newParent == kInvalidNode ? roots_ : nodes_[newParent].children;
  if (destination.size() == destination.capacity()) {
    const auto available = destination.max_size() - destination.size();
    if (!available) throw std::length_error("SceneGraph parent child list exceeds addressable storage");
    const auto growth = std::min(available, std::max<size_t>(destination.size(), 1));
    destination.reserve(destination.size() + growth);
  }

  if (childNode.parent != kInvalidNode) {
    auto& siblings = nodes_[childNode.parent].children;
    std::erase(siblings, child);
  } else {
    // Parent-before-child imports append a fresh last root, even when many
    // canonical roots precede it. Valid topology has unique root IDs.
    if (!roots_.empty() && roots_.back() == child) roots_.pop_back();
    else std::erase(roots_, child);
  }

  if (parentIndex.has_value() && parentIndex.value() < nodes_.size()) {
    childNode.parent = parentIndex.value();
    nodes_[childNode.parent].children.push_back(child);
  } else {
    childNode.parent = kInvalidNode;
    roots_.push_back(child);
  }
  ++revision_;
  ++hierarchyRevision_;
  return true;
}

bool SceneGraph::setParentPreserveWorldTransform(
    uint32_t child, std::optional<uint32_t> parentIndex) {
  if (!canSetParent(child, parentIndex)) return false;

  const uint32_t newParent =
      parentIndex.has_value() ? parentIndex.value() : kInvalidNode;
  if (nodes_[child].parent == newParent) {
    return false;
  }

  updateWorldTransforms();
  const glm::mat4 originalWorld = nodes_[child].worldTransform;
  const glm::mat4 parentWorld =
      parentIndex.has_value() ? nodes_[parentIndex.value()].worldTransform
                              : glm::mat4(1.0f);
  if (!transformBasisInvertible(parentWorld)) {
    return false;
  }
  const glm::mat4 newLocal = glm::inverse(parentWorld) * originalWorld;
  if (!matrixFinite(newLocal)) {
    return false;
  }
  const bool changed = setParent(child, parentIndex);
  if (!changed) {
    return false;
  }
  nodes_[child].localTransform = newLocal;
  updateWorldTransforms();
  return true;
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

std::optional<size_t> SceneGraph::siblingIndex(uint32_t node) const {
  if (node >= nodes_.size()) return std::nullopt;
  const auto parent = nodes_[node].parent;
  const auto &siblings = parent == kInvalidNode ? roots_ : nodes_[parent].children;
  const auto found = std::find(siblings.begin(), siblings.end(), node);
  return found == siblings.end() ? std::nullopt
      : std::optional<size_t>(static_cast<size_t>(found - siblings.begin()));
}

bool SceneGraph::setSiblingIndex(uint32_t node, size_t index) {
  if (isNodeRetired(node)) return false;
  const auto previous = siblingIndex(node);
  if (!previous) return false;
  const auto parent = nodes_[node].parent;
  auto &siblings = parent == kInvalidNode ? roots_ : nodes_[parent].children;
  if (index >= siblings.size()) return false;
  if (*previous == index) return true;
  if (*previous < index)
    std::rotate(siblings.begin() + *previous, siblings.begin() + *previous + 1,
                siblings.begin() + index + 1);
  else
    std::rotate(siblings.begin() + index, siblings.begin() + *previous,
                siblings.begin() + *previous + 1);
  ++revision_;
  ++hierarchyRevision_;
  return true;
}

SceneGraph::PreparedPlacement SceneGraph::preparePlacement(uint32_t node,
    std::optional<uint32_t> parent, const glm::mat4& local, size_t siblingIndex,
    const std::optional<std::vector<uint32_t>>& targetActiveRoots,
    const ScenePlacementLimits& limits) const {
  if (!placementBudgetValid(nodes_, roots_, renderableNodes_, activeRoots_, targetActiveRoots, limits) ||
      node >= nodes_.size() || nodes_[node].retired || !matrixFinite(local) ||
      revision_ == UINT64_MAX || hierarchyRevision_ == UINT64_MAX) return {};
  try {
    // Validate before canSetParent follows ancestry, including disconnected
    // components that need not belong to the selected scene.
    validateAppendForest(nodes_, roots_, renderableNodes_);
    for (const auto* view : {&activeRoots_, &targetActiveRoots})
      if (*view) for (const auto root : **view) if (root >= nodes_.size()) return {};
    if (!canSetParent(node, parent)) return {};
    const auto target = parent.value_or(kInvalidNode);
    const auto targetCount = parent ? nodes_[*parent].children.size() : roots_.size();
    const bool changingParent = nodes_[node].parent != target;
    if (siblingIndex >= targetCount + (changingParent ? 1u : 0u)) return {};
    SceneGraph candidate(*this);
    if (changingParent && !candidate.setParent(node, parent)) return {};
    candidate.setLocalTransform(node, local);
    if (!candidate.setSiblingIndex(node, siblingIndex)) return {};
    candidate.activeRoots_ = targetActiveRoots;
    validateAppendForest(candidate.nodes_, candidate.roots_, candidate.renderableNodes_);
    candidate.updateWorldTransforms();
    for (const auto& item : candidate.nodes_)
      if (!matrixFinite(item.localTransform) || !matrixFinite(item.worldTransform)) return {};
    PreparedPlacement prepared;
    prepared.state_ = std::make_unique<PreparedPlacement::State>(std::move(candidate));
    auto& state = *prepared.state_;
    state.candidate.refreshActiveMembership();
    state.owner = this; state.identity = identity_; state.revision = revision_;
    state.hierarchyRevision = hierarchyRevision_; state.nodeCount = nodes_.size();
    return prepared;
  } catch (const std::invalid_argument&) { return {}; }
}

bool SceneGraph::canPublishPlacement(const PreparedPlacement& prepared) const noexcept {
  if (!prepared.state_) return false;
  const auto& state = *prepared.state_;
  return !state.consumed && state.owner==this && state.identity==identity_ &&
      state.revision==revision_ && state.hierarchyRevision==hierarchyRevision_ &&
      state.nodeCount==nodes_.size();
}

bool SceneGraph::publishPlacement(PreparedPlacement& prepared) noexcept {
  if (!canPublishPlacement(prepared)) return false;
  auto& state = *prepared.state_;
  auto& candidate = state.candidate;
  nodes_.swap(candidate.nodes_); roots_.swap(candidate.roots_);
  nodeIncarnations_.swap(candidate.nodeIncarnations_);
  renderableNodes_.swap(candidate.renderableNodes_); activeRoots_.swap(candidate.activeRoots_);
  activeMembership_.swap(candidate.activeMembership_); activeRenderables_.swap(candidate.activeRenderables_);
  ++revision_; ++hierarchyRevision_; activeMembershipRevision_ = hierarchyRevision_;
  state.consumed = true;
  return true;
}

void SceneGraph::setLocalTransform(uint32_t nodeIndex,
                                   const glm::mat4& localTransform) {
  if (nodeIndex >= nodes_.size() || nodes_[nodeIndex].retired) return;
  if (nodes_[nodeIndex].localTransform == localTransform) return;
  nodes_[nodeIndex].localTransform = localTransform;
  ++revision_;
}

void SceneGraph::setRenderable(uint32_t nodeIndex, bool renderable) {
  if (nodeIndex >= nodes_.size() || nodes_[nodeIndex].retired) return;
  if (nodes_[nodeIndex].renderable == renderable) return;
  nodes_[nodeIndex].renderable = renderable;
  if (renderable) {
    registerRenderable(nodeIndex);
  } else {
    unregisterRenderable(nodeIndex);
  }
  activeMembershipRevision_=std::numeric_limits<uint64_t>::max();
  ++revision_;
}

void SceneGraph::setVisible(uint32_t nodeIndex, bool visible) {
  if (nodeIndex >= nodes_.size() || nodes_[nodeIndex].retired) return;
  if (nodes_[nodeIndex].visible == visible) return;
  nodes_[nodeIndex].visible = visible;
  ++revision_;
}

void SceneGraph::setSelectable(uint32_t nodeIndex, bool selectable) {
  if (nodeIndex >= nodes_.size() || nodes_[nodeIndex].retired || nodes_[nodeIndex].selectable == selectable) return;
  nodes_[nodeIndex].selectable = selectable;
  ++revision_;
}

void SceneGraph::setHoverable(uint32_t nodeIndex, bool hoverable) {
  if (nodeIndex >= nodes_.size() || nodes_[nodeIndex].retired || nodes_[nodeIndex].hoverable == hoverable) return;
  nodes_[nodeIndex].hoverable = hoverable;
  ++revision_;
}

void SceneGraph::setName(uint32_t nodeIndex, std::string name) {
  if (nodeIndex >= nodes_.size() || nodes_[nodeIndex].retired || nodes_[nodeIndex].name == name)
    return;
  nodes_[nodeIndex].name = std::move(name);
  ++revision_;
  ++hierarchyRevision_;
}

SceneGraph::PreparedTailChange SceneGraph::prepareDetachTail(uint32_t firstNode) const {
  auto undoView = activeRoots_;
  if (undoView)
    std::erase_if(*undoView, [firstNode](uint32_t id) { return id >= firstNode; });
  return prepareDetachTail(firstNode, undoView);
}

SceneGraph::PreparedTailChange SceneGraph::prepareDetachTail(uint32_t firstNode,
    const std::optional<std::vector<uint32_t>>& undoActiveRoots,
    std::span<const SceneNodeLocalPose> capturedPose) const {
  if (firstNode >= nodes_.size() || !tailPoseValid(capturedPose, firstNode)) return {};
  try {
    validateAppendForest(nodes_, roots_, renderableNodes_);
    if (activeRoots_)
      for (const auto id : *activeRoots_) if (id >= nodes_.size()) return {};
    if (undoActiveRoots)
      for (const auto id : *undoActiveRoots) if (id >= firstNode) return {};
    // Retained ownership or parenting cannot depend on retired tail nodes.
    for (uint32_t id = 0; id < firstNode; ++id) {
      const auto& node = nodes_[id];
      if ((node.parent != kInvalidNode && node.parent >= firstNode) ||
          (node.meshOwnerGraphNode != kInvalidNode && node.meshOwnerGraphNode >= firstNode))
        return {};
    }
    DetachedTail tail;
    tail.firstNode = firstNode;
    tail.graphIdentity = identity_;
    tail.nodes.assign(nodes_.begin() + firstNode, nodes_.end());
    tail.nodeIncarnations.assign(nodeIncarnations_.begin()+firstNode,nodeIncarnations_.end());
    tail.retainedIncarnations.assign(nodeIncarnations_.begin(),nodeIncarnations_.begin()+firstNode);
    tail.detachedActiveRoots = undoActiveRoots;
    tail.restoredActiveRoots = activeRoots_;
    SceneGraph candidate;
    candidate.nodes_.assign(nodes_.begin(), nodes_.begin() + firstNode);
    candidate.nodeIncarnations_.assign(nodeIncarnations_.begin(),nodeIncarnations_.begin()+firstNode);
    candidate.activeRoots_ = undoActiveRoots;
    for (uint32_t id = 0; id < firstNode; ++id) {
      const auto& children = nodes_[id].children;
      for (size_t position = 0; position < children.size(); ++position)
        if (children[position] >= firstNode)
          tail.externalChildren.push_back({id, children[position], position});
      // Compact each affected list once, including a parent with 20k children.
      std::erase_if(candidate.nodes_[id].children,
          [firstNode](uint32_t child) { return child >= firstNode; });
    }
    for (size_t position = 0; position < roots_.size(); ++position)
      if (roots_[position] >= firstNode) tail.roots.push_back({roots_[position], position});
      else candidate.roots_.push_back(roots_[position]);
    for (size_t position = 0; position < renderableNodes_.size(); ++position)
      if (renderableNodes_[position] >= firstNode)
        tail.renderables.push_back({renderableNodes_[position], position});
      else candidate.renderableNodes_.push_back(renderableNodes_[position]);
    validateAppendForest(candidate.nodes_, candidate.roots_, candidate.renderableNodes_);
    // Retained local edits may precede a world-cache refresh. A self-contained
    // generation publication must already contain their evaluated worlds. A
    // history snapshot also restores non-source/manual locals, rather than
    // inferring the captured pose from the current source animation bindings.
    for (const auto& pose : capturedPose) candidate.nodes_[pose.node].localTransform = pose.localTransform;
    candidate.updateWorldTransforms();
    if (!capturedPose.empty())
      for (const auto& node : candidate.nodes_) if (!matrixFinite(node.worldTransform)) return {};
    // Explicit pre-merge views may restore an editor root that was refined to
    // context. Every other retained node must keep its exact active membership.
    candidate.refreshActiveMembership();
    std::vector<uint8_t> currentMembership(nodes_.size(), activeRoots_ ? 0 : 1);
    if (activeRoots_) {
      std::vector<uint32_t> pending(*activeRoots_);
      while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (currentMembership[id]) continue;
        currentMembership[id] = 1;
        pending.insert(pending.end(), nodes_[id].children.begin(), nodes_[id].children.end());
      }
    }
    for (uint32_t id = 0; id < firstNode; ++id) {
      const bool before = currentMembership[id] != 0;
      const bool after = !candidate.activeRoots_ || candidate.activeMembership_[id] == 1;
      if (before != after && !(nodes_[id].editorSceneRoot &&
          !nodes_[id].renderable && nodes_[id].sourceNodeIndex == -1)) return {};
    }
    tail.retainedTopologyFingerprint = topologyFingerprint(candidate.nodes_,
        candidate.roots_, candidate.renderableNodes_);
    PreparedTailChange prepared;
    prepared.state_ = std::make_unique<PreparedTailChange::State>(std::move(candidate));
    auto& state = *prepared.state_;
    state.candidate.refreshActiveMembership();
    state.detached.emplace(std::move(tail));
    state.owner = this; state.identity = identity_; state.revision = revision_;
    state.hierarchyRevision = hierarchyRevision_; state.nodeCount = nodes_.size();
    return prepared;
  } catch (const std::invalid_argument&) { return {}; }
}

SceneGraph::PreparedTailChange SceneGraph::prepareRestoreTail(const DetachedTail& tail,
    std::span<const SceneNodeLocalPose> capturedPose) const {
  if (tail.graphIdentity != identity_ || tail.firstNode != nodes_.size() ||
      tail.nodeIncarnations.size()!=tail.nodes.size() || tail.retainedIncarnations!=nodeIncarnations_ ||
      tail.nodes.empty() || nodes_.size() > kInvalidNode ||
      tail.nodes.size() > kInvalidNode - nodes_.size() ||
      activeRoots_ != tail.detachedActiveRoots ||
      tail.retainedTopologyFingerprint != topologyFingerprint(nodes_, roots_, renderableNodes_))
    return {};
  if (!tailPoseValid(capturedPose, nodes_.size() + tail.nodes.size())) return {};
  try {
    validateAppendForest(nodes_, roots_, renderableNodes_);
    const size_t firstNode = nodes_.size(), end = firstNode + tail.nodes.size();
    for (const auto& node : nodes_)
      if (node.meshOwnerGraphNode != kInvalidNode && node.meshOwnerGraphNode >= firstNode) return {};
    SceneGraph candidate(*this);
    candidate.nodes_.reserve(end);
    candidate.nodeIncarnations_.reserve(end);
    for (size_t offset = 0; offset < tail.nodes.size(); ++offset) {
      validateNewAppendNode(tail.nodes[offset], firstNode + offset, end);
      candidate.nodes_.push_back(tail.nodes[offset]);
      candidate.nodeIncarnations_.push_back(tail.nodeIncarnations[offset]);
    }
    // Merge saved positions in one pass instead of shifting the retained
    // suffix once per insertion. Full forest validation checks coverage and
    // identity once after all three private index lists have been restored.
    const auto mergePositions = [firstNode, end](auto& destination, const auto& positions,
                                               const auto nodeIndex) {
      if (positions.size() > destination.max_size() - destination.size()) return false;
      std::vector<uint32_t> merged;
      const size_t total = destination.size() + positions.size();
      merged.reserve(total);
      size_t retained = 0, inserted = 0;
      for (size_t position = 0; position < total; ++position) {
        if (inserted < positions.size() && positions[inserted].position == position) {
          const auto id = nodeIndex(positions[inserted]);
          if (id < firstNode || id >= end) return false;
          merged.push_back(id); ++inserted;
        } else {
          if (retained >= destination.size()) return false;
          merged.push_back(destination[retained++]);
        }
      }
      if (inserted != positions.size() || retained != destination.size()) return false;
      destination = std::move(merged);
      return true;
    };
    if (!mergePositions(candidate.roots_, tail.roots, [](const auto& entry) { return entry.node; }) ||
        !mergePositions(candidate.renderableNodes_, tail.renderables,
            [](const auto& entry) { return entry.node; })) return {};
    uint32_t previousParent = kInvalidNode;
    for (size_t begin = 0; begin < tail.externalChildren.size();) {
      const auto parent = tail.externalChildren[begin].parent;
      if (parent >= firstNode || (previousParent != kInvalidNode && parent <= previousParent)) return {};
      size_t finish = begin + 1;
      while (finish < tail.externalChildren.size() && tail.externalChildren[finish].parent == parent) ++finish;
      const auto entries = std::span(tail.externalChildren).subspan(begin, finish - begin);
      for (const auto& entry : entries)
        if (entry.child < firstNode || entry.child >= end ||
            candidate.nodes_[entry.child].parent != parent) return {};
      if (!mergePositions(candidate.nodes_[parent].children, entries,
          [](const auto& entry) { return entry.child; })) return {};
      previousParent = parent; begin = finish;
    }
    validateAppendForest(candidate.nodes_, candidate.roots_, candidate.renderableNodes_);
    candidate.activeRoots_ = tail.restoredActiveRoots;
    if (candidate.activeRoots_)
      for (const auto id : *candidate.activeRoots_) if (id >= end) return {};
    // Recompute from current retained locals while detached. Allocation or
    // invalid restored topology cannot leave a partially published live graph.
    for (const auto& pose : capturedPose) candidate.nodes_[pose.node].localTransform = pose.localTransform;
    candidate.updateWorldTransforms();
    if (!capturedPose.empty())
      for (const auto& node : candidate.nodes_) if (!matrixFinite(node.worldTransform)) return {};
    for (size_t id = firstNode; id < end; ++id)
      validateNewAppendNode(candidate.nodes_[id], id, end);
    PreparedTailChange prepared;
    prepared.state_ = std::make_unique<PreparedTailChange::State>(std::move(candidate));
    auto& state = *prepared.state_;
    state.candidate.refreshActiveMembership();
    state.owner = this; state.identity = identity_; state.revision = revision_;
    state.hierarchyRevision = hierarchyRevision_; state.nodeCount = nodes_.size();
    return prepared;
  } catch (const std::invalid_argument&) { return {}; }
}

bool SceneGraph::publishTailChange(PreparedTailChange& prepared) noexcept {
  if (!prepared.state_) return false;
  auto& state = *prepared.state_;
  if (state.consumed || state.owner != this || state.identity != identity_ ||
      state.revision != revision_ || state.hierarchyRevision != hierarchyRevision_ ||
      state.nodeCount != nodes_.size()) return false;
  auto& candidate = state.candidate;
  nodes_.swap(candidate.nodes_); roots_.swap(candidate.roots_);
  nodeIncarnations_.swap(candidate.nodeIncarnations_);
  renderableNodes_.swap(candidate.renderableNodes_); activeRoots_.swap(candidate.activeRoots_);
  activeMembership_.swap(candidate.activeMembership_); activeRenderables_.swap(candidate.activeRenderables_);
  ++revision_; ++hierarchyRevision_;
  activeMembershipRevision_ = hierarchyRevision_;
  state.consumed = true;
  return true;
}

std::optional<SceneGraph::DetachedTail> SceneGraph::detachTail(uint32_t firstNode) {
  auto prepared = prepareDetachTail(firstNode);
  if (!publishTailChange(prepared)) return std::nullopt;
  return prepared.takeDetachedTail();
}

bool SceneGraph::restoreTail(const DetachedTail& tail) {
  auto prepared = prepareRestoreTail(tail);
  return publishTailChange(prepared);
}

bool SceneGraph::isNodeEffectivelyVisible(uint32_t nodeIndex) const {
  if (!isNodeActive(nodeIndex)) return false;

  uint32_t current = nodeIndex;
  while (current != kInvalidNode && current < nodes_.size()) {
    const SceneNode& node = nodes_[current];
    if (node.retired || !node.visible) return false;
    current = node.parent;
  }
  return true;
}

uint32_t SceneGraph::nearestInteractionAncestor(
    uint32_t nodeIndex, bool SceneNode::*flag) const {
  if (!isNodeActive(nodeIndex)) return kInvalidNode;
  uint32_t highestBlocker = kInvalidNode;
  uint32_t current = nodeIndex;
  size_t steps = 0;
  while (current != kInvalidNode) {
    // Mutable legacy payload access must not turn a malformed parent chain
    // into an unbounded pick loop or a successful partial hierarchy walk.
    if (current >= nodes_.size() || steps++ >= nodes_.size()) return kInvalidNode;
    const auto& node = nodes_[current];
    if (node.retired) return kInvalidNode;
    if (!(node.*flag)) highestBlocker = current;
    current = node.parent;
  }
  if (highestBlocker == kInvalidNode) return nodeIndex;
  const auto target = nodes_[highestBlocker].parent;
  if (!isNodeActive(target)) return kInvalidNode;
  const auto& parent = nodes_[target];
  if (parent.interactionRootBoundary || parent.editorSceneRoot) return kInvalidNode;
  return target;
}

uint32_t SceneGraph::nearestSelectableAncestor(uint32_t nodeIndex) const {
  return nearestInteractionAncestor(nodeIndex, &SceneNode::selectable);
}

uint32_t SceneGraph::nearestHoverableAncestor(uint32_t nodeIndex) const {
  return nearestInteractionAncestor(nodeIndex, &SceneNode::hoverable);
}

bool SceneGraph::isNodeEffectivelySelectable(uint32_t nodeIndex) const {
  return nodeIndex != kInvalidNode && nearestSelectableAncestor(nodeIndex) == nodeIndex;
}

bool SceneGraph::isNodeEffectivelyHoverable(uint32_t nodeIndex) const {
  return nodeIndex != kInvalidNode && nearestHoverableAncestor(nodeIndex) == nodeIndex;
}

void SceneGraph::updateWorldTransforms() {
  for (uint32_t root : roots_) {
    updateWorldRecursive(root, glm::mat4(1.0f));
  }
}

void SceneGraph::setActiveRoots(std::span<const uint32_t> roots) {
  std::vector<uint32_t> selected(roots.begin(),roots.end());
  for(const auto id:selected) if(id>=nodes_.size()) throw std::invalid_argument("Active scene root is outside the graph");
  if(activeRoots_ && *activeRoots_==selected) return;
  activeRoots_=std::move(selected);
  ++revision_; ++hierarchyRevision_;
  activeMembershipRevision_=std::numeric_limits<uint64_t>::max();
}

void SceneGraph::clearActiveRoots() {
  if(!activeRoots_) return;
  activeRoots_.reset(); activeMembership_.clear(); activeRenderables_.clear();
  ++revision_; ++hierarchyRevision_;
  activeMembershipRevision_=std::numeric_limits<uint64_t>::max();
}

void SceneGraph::refreshActiveMembership() const {
  if(activeMembershipRevision_==hierarchyRevision_ && activeMembership_.size()==nodes_.size()) return;
  std::vector<uint8_t> membership(nodes_.size(),0);
  std::vector<uint32_t> stack=activeRoots_ ? *activeRoots_ : roots_;
  while(!stack.empty()) {
    const auto id=stack.back(); stack.pop_back();
    if(id>=nodes_.size() || nodes_[id].retired || membership[id]) continue;
    membership[id]=1;
    stack.insert(stack.end(),nodes_[id].children.begin(),nodes_[id].children.end());
  }
  // Context ancestors are visible in the UI but do not activate siblings.
  if(activeRoots_) for(const auto root:*activeRoots_) {
    if(root>=nodes_.size() || nodes_[root].retired) continue; // History may retain an inactive slot.
    auto parent=nodes_[root].parent;
    size_t steps=0;
    while(parent!=kInvalidNode && parent<nodes_.size() && steps++<nodes_.size()) {
      if(nodes_[parent].retired || membership[parent]) break;
      membership[parent]=2; parent=nodes_[parent].parent;
    }
  }
  std::vector<uint32_t> renderables;
  for(const auto id:renderableNodes_) if(id<nodes_.size() && membership[id]==1) renderables.push_back(id);
  activeMembership_=std::move(membership); activeRenderables_=std::move(renderables);
  activeMembershipRevision_=hierarchyRevision_;
}

bool SceneGraph::isNodeActive(uint32_t nodeIndex) const {
  if(nodeIndex>=nodes_.size() || nodes_[nodeIndex].retired) return false;
  refreshActiveMembership();
  return activeMembership_[nodeIndex]==1;
}

bool SceneGraph::isNodeInActiveHierarchy(uint32_t nodeIndex) const {
  if(nodeIndex>=nodes_.size() || nodes_[nodeIndex].retired) return false;
  refreshActiveMembership();
  return activeMembership_[nodeIndex]!=0;
}

bool SceneGraph::isNodeRetired(uint32_t nodeIndex) const noexcept {
  return nodeIndex < nodes_.size() && nodes_[nodeIndex].retired;
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
  if (node.children.empty()) return;

  // Imported assets may contain very deep inactive trees. Keep the original
  // parent-first depth-first order without consuming one call frame per node.
  // Store the traversal parent explicitly, as the recursive implementation did.
  struct PendingNode {
    uint32_t nodeIndex;
    uint32_t parentIndex;
  };
  std::vector<PendingNode> pending;
  pending.reserve(node.children.size());
  for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
    pending.push_back({*child, nodeIndex});
  while (!pending.empty()) {
    const auto current = pending.back();
    pending.pop_back();
    auto& descendant = nodes_[current.nodeIndex];
    descendant.worldTransform = nodes_[current.parentIndex].worldTransform *
                                descendant.localTransform;
    for (auto child = descendant.children.rbegin();
         child != descendant.children.rend(); ++child)
      pending.push_back({*child, current.nodeIndex});
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
