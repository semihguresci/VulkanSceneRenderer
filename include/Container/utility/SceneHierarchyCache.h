#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace container::scene {
class SceneGraph;
}

namespace container::ui {

struct SceneHierarchyRow {
  uint32_t nodeIndex{0};
  uint32_t depth{0};
  std::string label{};
  bool hasChildren{false};
  bool expanded{false};
};

// The visible preorder only changes with hierarchy topology or expansion.
// Rows own their labels and retain node IDs rather than pointers into a graph.
class SceneHierarchyCache {
public:
  bool refresh(const container::scene::SceneGraph &graph);
  bool setExpanded(uint32_t nodeIndex, bool expanded);
  void clear();

  [[nodiscard]] std::span<const SceneHierarchyRow> rows() const {
    return rows_;
  }
  [[nodiscard]] uint64_t rebuildCount() const { return rebuildCount_; }
  [[nodiscard]] size_t cachedBytes() const { return cachedBytes_; }
  [[nodiscard]] std::optional<size_t> rowForNode(uint32_t nodeIndex) const;

private:
  const container::scene::SceneGraph *source_{nullptr};
  uint64_t identity_{0};
  uint64_t hierarchyRevision_{0};
  std::vector<uint8_t> expanded_{};
  std::vector<SceneHierarchyRow> rows_{};
  std::vector<size_t> rowByNode_{};
  uint64_t rebuildCount_{0};
  size_t cachedBytes_{0};
  bool dirty_{true};
};

} // namespace container::ui
