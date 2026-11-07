#include "Container/utility/SceneHierarchyCache.h"

#include "Container/utility/SceneGraph.h"

#include <limits>
#include <algorithm>
#include <utility>

namespace container::ui {

bool SceneHierarchyCache::refresh(const container::scene::SceneGraph &graph) {
  const bool replacement = source_ != &graph || identity_ != graph.identity();
  if (!replacement && hierarchyRevision_ == graph.hierarchyRevision() &&
      expanded_.size() == graph.nodeCount() && !dirty_) {
    return false;
  }
  if (replacement)
    expanded_.clear();
  source_ = &graph;
  identity_ = graph.identity();
  hierarchyRevision_ = graph.hierarchyRevision();
  expanded_.resize(graph.nodeCount(), 0u);
  rows_.clear();
  rows_.reserve(graph.nodeCount());
  rowByNode_.assign(graph.nodeCount(), std::numeric_limits<size_t>::max());

  struct PendingRow {
    uint32_t nodeIndex;
    uint32_t depth;
  };
  std::vector<PendingRow> pending;
  pending.reserve(graph.rootNodes().size());
  for (auto root = graph.rootNodes().rbegin(); root != graph.rootNodes().rend();
       ++root) {
    pending.push_back({*root, 0u});
  }
  // Valid SceneGraph operations reject cycles. Also bound traversal for legacy
  // callers that edit SceneNode::children through the mutable node accessor.
  std::vector<uint8_t> visited(graph.nodeCount(), 0u);
  while (!pending.empty()) {
    const auto [nodeIndex, depth] = pending.back();
    pending.pop_back();
    const auto *node = graph.getNode(nodeIndex);
    if (node == nullptr || node->retired || visited[nodeIndex] || !graph.isNodeInActiveHierarchy(nodeIndex))
      continue;
    visited[nodeIndex] = 1u;
    std::string label =
        node->name.empty() ? "Node " + std::to_string(nodeIndex) : node->name;
    label += " [" + std::to_string(nodeIndex) + "]";
    if (node->primitiveIndex != container::scene::SceneGraph::kInvalidNode)
      label += " / Primitive " + std::to_string(node->primitiveIndex);
    // Clipped rows have one fixed height. Preserve source names on the graph
    // for the full tooltip, while control characters occupy one display line.
    for (char &character : label)
      if (character == '\r' || character == '\n' || character == '\t')
        character = ' ';
    const bool hasChildren = std::ranges::any_of(node->children,[&](uint32_t child) { return graph.isNodeInActiveHierarchy(child); });
    const bool expanded = hasChildren && expanded_[nodeIndex] != 0u;
    rowByNode_[nodeIndex] = rows_.size();
    rows_.push_back(
        {nodeIndex, depth, std::move(label), hasChildren, expanded});
    if (!expanded)
      continue;
    const uint32_t childDepth =
        depth == std::numeric_limits<uint32_t>::max() ? depth : depth + 1u;
    for (auto child = node->children.rbegin(); child != node->children.rend();
         ++child)
      pending.push_back({*child, childDepth});
  }
  dirty_ = false;
  ++rebuildCount_;
  cachedBytes_ = expanded_.capacity() * sizeof(uint8_t) +
                 rows_.capacity() * sizeof(SceneHierarchyRow) +
                 rowByNode_.capacity() * sizeof(size_t);
  for (const auto &row : rows_)
    cachedBytes_ += row.label.capacity() + 1u;
  return true;
}

bool SceneHierarchyCache::setExpanded(uint32_t nodeIndex, bool expanded) {
  if (nodeIndex >= expanded_.size() || expanded_[nodeIndex] == expanded)
    return false;
  expanded_[nodeIndex] = expanded ? 1u : 0u;
  dirty_ = true;
  return true;
}

void SceneHierarchyCache::clear() { *this = {}; }

std::optional<size_t>
SceneHierarchyCache::rowForNode(uint32_t nodeIndex) const {
  if (nodeIndex >= rowByNode_.size() ||
      rowByNode_[nodeIndex] == std::numeric_limits<size_t>::max())
    return std::nullopt;
  return rowByNode_[nodeIndex];
}

} // namespace container::ui
