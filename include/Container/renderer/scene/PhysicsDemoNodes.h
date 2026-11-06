#pragma once

#include "Container/utility/SceneGraph.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace container::renderer {

// Authored order matters: the floor is first, followed by the three boxes.
// Hierarchy child order and child count alone cannot establish demo ownership.
struct PhysicsDemoNodes {
  static constexpr uint32_t kBodyCount = 4u;
  uint64_t graphIdentity{0u};
  uint32_t root{scene::SceneGraph::kInvalidNode};
  std::array<uint32_t, kBodyCount> bodies{scene::SceneGraph::kInvalidNode,
      scene::SceneGraph::kInvalidNode, scene::SceneGraph::kInvalidNode,
      scene::SceneGraph::kInvalidNode};

  [[nodiscard]] bool reusable(const scene::SceneGraph &graph) const {
    if (graphIdentity != graph.identity())
      return false;
    const auto *group = graph.getNode(root);
    if (!group || group->retired || group->children.size() != bodies.size())
      return false;
    for (std::size_t index = 0; index < bodies.size(); ++index) {
      const auto node = bodies[index];
      const auto *entry = graph.getNode(node);
      if (!entry || entry->retired || entry->parent != root ||
          std::find(group->children.begin(), group->children.end(), node) == group->children.end() ||
          std::find(bodies.begin(), bodies.begin() + index, node) != bodies.begin() + index)
        return false;
    }
    return true;
  }
};

} // namespace container::renderer
