#pragma once

#include <Container/geometry/GltfMaterialVariants.h>
#include <Container/utility/SceneGraph.h>

#include <optional>
#include <map>
#include <stdexcept>
#include <vector>

namespace container::scene {
struct MaterialVariantGraphEdit {
  struct Assignment { uint32_t node{},before{},after{}; };
  uint64_t graphIdentity{};
  std::optional<size_t> beforeSelection,afterSelection;
  std::vector<Assignment> assignments;
  [[nodiscard]] bool changes() const {
    if (beforeSelection!=afterSelection) return true;
    for (const auto& assignment:assignments) if (assignment.before!=assignment.after) return true;
    return false;
  }
};

inline MaterialVariantGraphEdit PlanMaterialVariantGraphEdit(
    const SceneGraph& graph,const geometry::gltf::MaterialVariantSelectionPlan& plan,
    std::optional<size_t> previousSelection) {
  MaterialVariantGraphEdit edit{graph.identity(),previousSelection,plan.selectedVariant,{}};
  std::map<std::pair<size_t,size_t>,int32_t> materials;
  for (const auto& assignment:plan.assignments)
    if (assignment.runtimeMaterial<0 || !materials.emplace(std::pair{assignment.sourceMesh,assignment.sourcePrimitive},assignment.runtimeMaterial).second)
      throw std::runtime_error("Material variant plan has invalid or duplicate primitive assignments");
  for (uint32_t index=0;index<graph.nodeCount();++index) {
    const auto* node=graph.getNode(index);
    if (!node || node->sourceMeshIndex<0 || node->sourcePrimitiveIndex<0) continue;
    const auto found=materials.find({size_t(node->sourceMeshIndex),size_t(node->sourcePrimitiveIndex)});
    if (found==materials.end())
      throw std::runtime_error("Material variant plan does not cover an imported primitive");
    edit.assignments.push_back({index,node->materialIndex,uint32_t(found->second)});
  }
  return edit;
}

// Validate every node before mutation. Graph replacement invalidates playback;
// parenting changes do not. Exact snapshots retain intervening per-node edits.
inline bool ApplyMaterialVariantGraphEdit(SceneGraph& graph,const MaterialVariantGraphEdit& edit,
    bool forward,std::optional<size_t>& selected) {
  if (edit.graphIdentity!=graph.identity()) return false;
  for (const auto& assignment:edit.assignments) if (!graph.getNode(assignment.node)) return false;
  for (const auto& assignment:edit.assignments)
    graph.setMaterialIndex(assignment.node,forward?assignment.after:assignment.before);
  selected=forward?edit.afterSelection:edit.beforeSelection;
  return true;
}
} // namespace container::scene
