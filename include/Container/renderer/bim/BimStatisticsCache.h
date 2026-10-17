#pragma once

#include "Container/renderer/bim/BimDrawFilterState.h"

namespace container::renderer {

// Model-derived counts and topology are rebuilt only after a complete install.
// GPU residency and compaction are overlaid when queried, never cached here.
class BimStatisticsCache {
public:
  void clear();
  void rebuild(BimSceneStats modelStats,
               std::span<const BimElementMetadata> metadata,
               std::span<const container::gpu::ObjectData> objects,
               std::span<const DrawCommand> opaque,
               std::span<const DrawCommand> transparent);

  [[nodiscard]] BimSceneStats
  sceneStats(const BimMeshletResidencyStats &residency,
             std::span<const BimDrawCompactionStats> compaction) const;
  [[nodiscard]] const container::scene::ImmutableSceneTriangleBatches &
  triangleBatches() const {
    return triangleBatches_;
  }
  [[nodiscard]] container::scene::SceneProviderBounds
  sceneProviderBounds(glm::vec3 rootTranslation = glm::vec3(0)) const;

  [[nodiscard]] BimDrawBudgetLodStats
  drawBudgetLodStats(const BimDrawFilter &filter,
                     const BimDrawFilterStateInputs &inputs,
                     std::span<const BimObjectLodStreamingMetadata> lod,
                     uint64_t lodRevision, uint64_t visibilityRevision) const;

private:
  BimSceneStats modelStats_{};
  container::scene::ImmutableSceneTriangleBatches triangleBatches_{};
  container::scene::SceneProviderBounds baseBounds_{};
  mutable bool budgetStatsValid_{false};
  mutable BimDrawFilter budgetFilter_{};
  mutable uint64_t budgetObjectRevision_{0};
  mutable uint64_t budgetLodRevision_{0};
  mutable uint64_t budgetVisibilityRevision_{0};
  mutable BimDrawBudgetLodStats budgetStats_{};
};

} // namespace container::renderer
