#include "Container/renderer/bim/BimStatisticsCache.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace container::renderer {

void BimStatisticsCache::clear() {
  modelStats_ = {};
  triangleBatches_ = container::scene::ImmutableSceneTriangleBatches{};
  baseBounds_ = {};
  budgetStatsValid_ = false;
  budgetFilter_ = {};
  budgetStats_ = {};
}

void BimStatisticsCache::rebuild(
    BimSceneStats modelStats, std::span<const BimElementMetadata> metadata,
    std::span<const container::gpu::ObjectData> objects,
    std::span<const DrawCommand> opaque,
    std::span<const DrawCommand> transparent) {
  clear();
  modelStats.objectCount = metadata.size();
  modelStats.meshObjectCount = 0;
  modelStats.pointObjectCount = 0;
  modelStats.curveObjectCount = 0;
  for (const auto &element : metadata) {
    switch (element.geometryKind) {
    case BimGeometryKind::Points:
      ++modelStats.pointObjectCount;
      break;
    case BimGeometryKind::Curves:
      ++modelStats.curveObjectCount;
      break;
    case BimGeometryKind::Mesh:
    default:
      ++modelStats.meshObjectCount;
      break;
    }
    if (element.bounds.valid) {
      if (!baseBounds_.valid) {
        baseBounds_ = {.min = element.bounds.min,
                       .max = element.bounds.max,
                       .valid = true};
      } else {
        baseBounds_.min.x = std::min(baseBounds_.min.x, element.bounds.min.x);
        baseBounds_.min.y = std::min(baseBounds_.min.y, element.bounds.min.y);
        baseBounds_.min.z = std::min(baseBounds_.min.z, element.bounds.min.z);
        baseBounds_.max.x = std::max(baseBounds_.max.x, element.bounds.max.x);
        baseBounds_.max.y = std::max(baseBounds_.max.y, element.bounds.max.y);
        baseBounds_.max.z = std::max(baseBounds_.max.z, element.bounds.max.z);
      }
    }
  }
  modelStats_ = modelStats;

  std::vector<container::scene::SceneProviderTriangleBatch> batches;
  batches.reserve(opaque.size() + transparent.size());
  const auto append = [&](std::span<const DrawCommand> commands,
                          bool isTransparent) {
    for (const auto &command : commands) {
      if (command.indexCount == 0)
        continue;
      const auto *element = command.objectIndex < metadata.size()
                                ? &metadata[command.objectIndex]
                                : nullptr;
      if (element && element->geometryKind != BimGeometryKind::Mesh)
        continue;
      uint32_t material = std::numeric_limits<uint32_t>::max();
      bool doubleSided = false;
      bool transparentBatch = isTransparent;
      if (element) {
        material = element->materialIndex;
        doubleSided = element->doubleSided;
        transparentBatch = transparentBatch || element->transparent;
      } else if (command.objectIndex < objects.size()) {
        material = objects[command.objectIndex].objectInfo.x;
        doubleSided = (objects[command.objectIndex].objectInfo.y &
                       container::gpu::kObjectFlagDoubleSided) != 0;
      }
      batches.push_back({
          .firstIndex = command.firstIndex,
          .indexCount = command.indexCount,
          .materialIndex =
              material > uint32_t(std::numeric_limits<int32_t>::max())
                  ? -1
                  : static_cast<int32_t>(material),
          .doubleSided = doubleSided,
          .transparent = transparentBatch,
          .instanceCount = std::max(command.instanceCount, 1u),
      });
    }
  };
  append(opaque, false);
  append(transparent, true);
  triangleBatches_ = std::move(batches);
}

container::scene::SceneProviderBounds
BimStatisticsCache::sceneProviderBounds(glm::vec3 rootTranslation) const {
  auto bounds = baseBounds_;
  if (bounds.valid) {
    bounds.min += rootTranslation;
    bounds.max += rootTranslation;
  }
  return bounds;
}

BimSceneStats BimStatisticsCache::sceneStats(
    const BimMeshletResidencyStats &residency,
    std::span<const BimDrawCompactionStats> compaction) const {
  auto stats = modelStats_;
  stats.meshletGpuResidentObjectCount = residency.residentObjectCount;
  stats.meshletGpuResidentClusterCount = residency.residentClusterCount;
  stats.meshletGpuBufferBytes = residency.clusterBufferBytes +
                                residency.objectLodBufferBytes +
                                residency.residencyBufferBytes;
  stats.meshletGpuComputeReady = residency.computeReady;
  stats.meshletGpuDispatchPending = residency.dispatchPending;
  // Reset live fields even if a caller accidentally supplied them at install.
  stats.gpuCompactionInputDrawCount = 0;
  stats.gpuCompactionOutputCapacity = 0;
  stats.gpuCompactionBufferBytes = 0;
  stats.gpuCompactionReady = false;
  stats.gpuCompactionDispatchPending = false;
  for (const auto &slot : compaction) {
    stats.gpuCompactionInputDrawCount += slot.inputDrawCount;
    stats.gpuCompactionOutputCapacity += slot.outputCapacity;
    stats.gpuCompactionBufferBytes +=
        slot.inputBufferBytes + slot.outputBufferBytes + slot.countBufferBytes;
    stats.gpuCompactionReady = stats.gpuCompactionReady || slot.computeReady;
    stats.gpuCompactionDispatchPending =
        stats.gpuCompactionDispatchPending || slot.dispatchPending;
  }
  return stats;
}

BimDrawBudgetLodStats BimStatisticsCache::drawBudgetLodStats(
    const BimDrawFilter &filter, const BimDrawFilterStateInputs &inputs,
    std::span<const BimObjectLodStreamingMetadata> lod, uint64_t lodRevision,
    uint64_t visibilityRevision) const {
  if (budgetStatsValid_ && budgetObjectRevision_ == inputs.revision &&
      budgetLodRevision_ == lodRevision &&
      budgetVisibilityRevision_ == visibilityRevision &&
      bimDrawFiltersEqual(budgetFilter_, filter)) {
    return budgetStats_;
  }
  budgetStatsValid_ = false;
  BimDrawBudgetLodStats stats{};
  stats.enabled = filter.drawBudgetEnabled;
  stats.maxObjects = filter.drawBudgetMaxObjects;
  const BimDrawFilterState state;
  for (uint32_t objectIndex = 0; objectIndex < inputs.metadata.size();
       ++objectIndex) {
    if (!state.objectMatchesFilter(objectIndex, filter, inputs))
      continue;
    ++stats.visibleObjectCount;
    if (inputs.metadata[objectIndex].geometryKind != BimGeometryKind::Mesh)
      continue;
    ++stats.visibleMeshObjectCount;
    if (objectIndex < lod.size()) {
      stats.visibleMeshletClusterReferences += lod[objectIndex].clusterCount;
      stats.visibleMaxLodLevel =
          std::max(stats.visibleMaxLodLevel, lod[objectIndex].maxLodLevel);
    }
  }
  // Publish the key only after its strings have been copied successfully.
  budgetFilter_ = filter;
  budgetObjectRevision_ = inputs.revision;
  budgetLodRevision_ = lodRevision;
  budgetVisibilityRevision_ = visibilityRevision;
  budgetStats_ = stats;
  budgetStatsValid_ = true;
  return stats;
}

} // namespace container::renderer
