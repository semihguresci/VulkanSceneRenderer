#include "Container/renderer/bim/BimInspectionCache.h"

#include "Container/renderer/bim/BimManager.h"

#include <utility>

namespace container::renderer {

bool BimInspectionCache::refresh(
    const void *source, uint64_t geometryRevision,
    std::span<const BimElementMetadata> metadata) {
  if (source == nullptr) {
    clear();
    return false;
  }
  if (source_ == source && geometryRevision_ == geometryRevision &&
      metadata_ == metadata.data() && metadataCount_ == metadata.size()) {
    return false;
  }

  // Publish the new spans only after every derived table has been built.
  BimInspectionCache replacement;
  std::vector<BimScheduleElement> scheduleElements;
  scheduleElements.reserve(metadata.size());
  replacement.compareElements_.reserve(metadata.size());
  for (const BimElementMetadata &element : metadata) {
    const std::string &storey =
        !element.storeyName.empty() ? element.storeyName : element.storeyId;
    const std::string &material = !element.materialName.empty()
                                      ? element.materialName
                                      : element.materialCategory;
    scheduleElements.push_back(
        {.guid = element.guid,
         .sourceId = element.sourceId,
         .ifcClass = element.type,
         .type = element.objectType,
         .storey = storey,
         .material = material,
         .bounds = {.valid = element.bounds.valid,
                    .min = element.bounds.min,
                    .max = element.bounds.max}});
    replacement.compareElements_.push_back(
        {.guid = element.guid,
         .sourceId = element.sourceId,
         .ifcClass = element.type,
         .type = element.objectType,
         .storey = storey,
         .material = material,
         .bounds = {.valid = element.bounds.valid,
                    .min = element.bounds.min,
                    .max = element.bounds.max}});
  }
  replacement.byClassAndStorey_ =
      buildBimScheduleByIfcClassAndStorey(scheduleElements);
  replacement.byTypeAndStorey_ =
      buildBimScheduleByTypeAndStorey(scheduleElements);
  replacement.byMaterial_ = buildBimScheduleMaterialTotals(scheduleElements);
  replacement.source_ = source;
  replacement.geometryRevision_ = geometryRevision;
  replacement.metadata_ = metadata.data();
  replacement.metadataCount_ = metadata.size();
  *this = std::move(replacement);
  return true;
}

void BimInspectionCache::clear() { *this = BimInspectionCache{}; }

} // namespace container::renderer
