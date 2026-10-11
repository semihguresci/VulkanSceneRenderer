#pragma once

#include "Container/renderer/bim/BimModelCompare.h"
#include "Container/renderer/bim/BimScheduleExtractor.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace container::renderer {

struct BimElementMetadata;

// Derived inspection data only changes with the loaded model metadata. Keep
// its owning strings and grouped schedules out of the per-frame GUI path.
class BimInspectionCache {
public:
  // Returns true after rebuilding. Metadata/bounds changes must advance the
  // geometry revision; GPU object-color and visibility edits do not affect it.
  bool refresh(const void *source, uint64_t geometryRevision,
               std::span<const BimElementMetadata> metadata);
  void clear();

  [[nodiscard]] std::span<const BimScheduleRow> byClassAndStorey() const {
    return byClassAndStorey_;
  }
  [[nodiscard]] std::span<const BimScheduleRow> byTypeAndStorey() const {
    return byTypeAndStorey_;
  }
  [[nodiscard]] std::span<const BimScheduleRow> byMaterial() const {
    return byMaterial_;
  }
  [[nodiscard]] std::span<const BimModelCompareElement> compareElements() const {
    return compareElements_;
  }

private:
  const void *source_{nullptr};
  uint64_t geometryRevision_{0};
  const BimElementMetadata *metadata_{nullptr};
  size_t metadataCount_{0};
  std::vector<BimScheduleRow> byClassAndStorey_;
  std::vector<BimScheduleRow> byTypeAndStorey_;
  std::vector<BimScheduleRow> byMaterial_;
  std::vector<BimModelCompareElement> compareElements_;
};

} // namespace container::renderer
