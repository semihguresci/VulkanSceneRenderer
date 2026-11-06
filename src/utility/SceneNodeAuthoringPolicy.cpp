#include <Container/utility/SceneNodeAuthoringPolicy.h>

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace container::scene {
namespace {
constexpr auto invalid = SceneGraph::kInvalidNode;
constexpr uint8_t protectedBit = 1, productBit = 2, bodyBit = 4, jointBit = 8, skinBit = 16;

[[noreturn]] void reject(const char* message) {
  throw std::invalid_argument(std::string("Scene node authoring: ") + message);
}


void validateGraphBudget(const SceneGraph& graph,
                         const SceneNodeAuthoringLimits& limits) {
  if (!limits.maxVisitedNodes || !limits.maxVisitedEdges)
    reject("traversal budgets must be positive");
  if (graph.nodeCount() >= invalid || graph.nodeCount() > limits.maxVisitedNodes)
    reject("graph node budget exceeded");
  uint64_t edges = 0;
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    const auto count = graph.getNode(id)->children.size();
    if (count > limits.maxVisitedEdges - edges)
      reject("graph edge budget exceeded");
    edges += count;
  }
  // Active-view caching copies this list. Bound it before asking the graph to
  // resolve membership, including legacy duplicate view entries.
  if (graph.activeRoots() && graph.activeRoots()->size() > limits.maxVisitedEdges - edges)
    reject("active view metadata budget exceeded");
}

void validateRestrictions(const SceneGraph& graph,
                          const SceneNodeAuthoringRestrictions& restrictions,
                          const SceneNodeAuthoringLimits& limits) {
  for (const auto entries : {restrictions.protectedNodes,
                            restrictions.physicsProductNodes,
                            restrictions.physicsBodyNodes,
                            restrictions.jointEndpointNodes,
                            restrictions.skinDependencyNodes}) {
    if (entries.size() > limits.maxVisitedNodes)
      reject("restriction metadata budget exceeded");
    for (const auto node : entries)
      if (!graph.getNode(node)) reject("restriction metadata references a missing node");
  }
}

bool contains(std::span<const uint32_t> entries, uint32_t node) {
  return std::find(entries.begin(), entries.end(), node) != entries.end();
}

void checkTarget(const SceneGraph& graph, uint32_t target,
                 SceneNodeAuthoringPurpose purpose,
                 const SceneNodeAuthoringRestrictions& restrictions) {
  const auto* node = graph.getNode(target);
  if (!node) reject("selected target is missing");
  if (node->retired) reject("selected target is retired");
  if (node->sourceNodeIndex < -1) reject("selected target has an invalid source identity");
  if (contains(restrictions.protectedNodes, target))
    reject("selected target is protected");
  if (purpose == SceneNodeAuthoringPurpose::Rename || purpose == SceneNodeAuthoringPurpose::DeleteSubtree ||
      purpose == SceneNodeAuthoringPurpose::DuplicateSubtree) {
    if (node->editorSceneRoot || node->interactionRootBoundary)
      reject("structural editor/import roots cannot be renamed, duplicated or deleted");
  } else if (purpose == SceneNodeAuthoringPurpose::CreateEmptyChild) {
    if (node->interactionRootBoundary)
      reject("choose an authored node or the editor root instead of an import wrapper");
    if (contains(restrictions.physicsProductNodes, target))
      reject("physics product internals cannot receive editor children");
    if (node->editorSceneRoot && (node->renderable || node->sourceNodeIndex != -1))
      reject("editor attachment context has invalid source/renderable metadata");
  } else {
    reject("unknown authoring purpose");
  }
  if (!graph.isNodeInActiveHierarchy(target) &&
      !(purpose == SceneNodeAuthoringPurpose::CreateEmptyChild && node->editorSceneRoot))
    reject("selected target is outside the active scene hierarchy");
}
} // namespace


uint32_t ResolveSceneNodeAuthoringTarget(
    const SceneGraph& graph, uint32_t selected,
    SceneNodeAuthoringPurpose purpose,
    const SceneNodeAuthoringRestrictions& restrictions,
    const SceneNodeAuthoringLimits& limits) {
  validateGraphBudget(graph, limits);
  validateRestrictions(graph, restrictions, limits);
  checkTarget(graph, selected, purpose, restrictions);
  const auto& node = *graph.getNode(selected);
  if (node.renderable && node.sourceNodeIndex == -1 &&
      node.meshOwnerGraphNode != invalid) {
    const auto* owner = graph.getNode(node.meshOwnerGraphNode);
    if (!owner || owner->sourceNodeIndex < -1)
      reject("selected primitive has an invalid mesh owner");
    // Aggregate/default geometry can have an import wrapper as a source-less
    // mesh owner. It has no authored group to redirect to; keep the primitive.
    if (owner->sourceNodeIndex >= 0) {
      checkTarget(graph, node.meshOwnerGraphNode, purpose, restrictions);
      return node.meshOwnerGraphNode;
    }
  }
  return selected;
}

SceneNodeAuthoringSubtree CollectSceneNodeAuthoringSubtree(
    const SceneGraph& graph, uint32_t root,
    const SceneNodeAuthoringRestrictions& restrictions,
    const SceneNodeAuthoringLimits& limits) {
  validateGraphBudget(graph, limits);
  validateRestrictions(graph, restrictions, limits);
  const auto* first = graph.getNode(root);
  if (!first) reject("subtree root is missing");
  uint64_t edges = 0;
  if (first->parent != invalid) {
    const auto* parent = graph.getNode(first->parent);
    if (!parent || first->parent == root)
      reject("subtree root has an invalid parent");
    if (parent->children.size() > limits.maxVisitedEdges)
      reject("subtree edge budget exceeded");
    edges = parent->children.size();
    if (std::count(parent->children.begin(), parent->children.end(), root) != 1)
      reject("subtree root parent link is inconsistent");
  }
  std::vector<uint8_t> metadata(graph.nodeCount(), 0), visited(graph.nodeCount(), 0);
  const auto mark = [&](std::span<const uint32_t> entries, uint8_t bit) {
    for (const auto node : entries) metadata[node] |= bit;
  };
  mark(restrictions.protectedNodes, protectedBit);
  mark(restrictions.physicsProductNodes, productBit);
  mark(restrictions.physicsBodyNodes, bodyBit);
  mark(restrictions.jointEndpointNodes, jointBit);
  mark(restrictions.skinDependencyNodes, skinBit);
  SceneNodeAuthoringSubtree result;
  result.nodes.reserve(graph.nodeCount());
  std::vector<uint32_t> pending;
  pending.reserve(graph.nodeCount());
  pending.push_back(root); visited[root] = 1;
  while (!pending.empty()) {
    const auto id = pending.back(); pending.pop_back();
    const auto& node = *graph.getNode(id);
    result.nodes.push_back(id);
    result.containsProtectedNodes |= node.editorSceneRoot || node.interactionRootBoundary ||
        (metadata[id] & protectedBit) != 0;
    result.containsPhysicsProductNodes |= (metadata[id] & productBit) != 0;
    result.containsPhysicsBodies |= (metadata[id] & bodyBit) != 0;
    result.containsJointEndpoints |= (metadata[id] & jointBit) != 0;
    result.containsSkinDependencies |= (metadata[id] & skinBit) != 0;
    if (node.children.size() > limits.maxVisitedEdges - edges)
      reject("subtree edge budget exceeded");
    edges += node.children.size();
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
      const auto* payload = graph.getNode(*child);
      if (!payload || payload->parent != id)
        reject("subtree child reference is invalid or inconsistent");
      if (visited[*child]) reject("subtree has a cycle or repeated child");
      visited[*child] = 1;
      pending.push_back(*child);
    }
  }
  return result;
}

SceneNodeAuthoringSubtree ValidateSceneNodeRetirement(
    const SceneGraph& graph, uint32_t selected,
    const SceneNodeAuthoringRestrictions& restrictions,
    const SceneNodeAuthoringLimits& limits) {
  const auto root = ResolveSceneNodeAuthoringTarget(graph, selected,
      SceneNodeAuthoringPurpose::DeleteSubtree, restrictions, limits);
  auto subtree = CollectSceneNodeAuthoringSubtree(graph, root, restrictions, limits);
  if (subtree.containsProtectedNodes) reject("subtree contains a protected structural node");
  if (subtree.containsPhysicsProductNodes) reject("subtree contains physics product internals");
  if (subtree.containsPhysicsBodies) reject("detach physics bodies before deleting their subtree");
  if (subtree.containsJointEndpoints) reject("detach joint endpoints before deleting their subtree");
  if (subtree.containsSkinDependencies) reject("subtree contains a retained skin joint or skeleton dependency");
  return subtree;
}

} // namespace container::scene
