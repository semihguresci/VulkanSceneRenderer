#pragma once

#include "Container/renderer/bim/BimManager.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace container::renderer {

struct BimDrawFilterStateInputs {
  uint64_t revision{0};
  size_t objectCount{0};
  BimMetadataTableView metadata{};
  std::span<const std::string> phaseOrder{};
  const std::vector<DrawCommand> *opaqueDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueWindingFlippedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentWindingFlippedDrawCommands{
      nullptr};
  const std::vector<DrawCommand> *transparentDoubleSidedDrawCommands{nullptr};
  const BimGeometryDrawLists *pointDrawLists{nullptr};
  const BimGeometryDrawLists *curveDrawLists{nullptr};
  const BimGeometryDrawLists *nativePointDrawLists{nullptr};
  const BimGeometryDrawLists *nativeCurveDrawLists{nullptr};
};

class BimDrawFilterState {
public:
  void clear();

  [[nodiscard]] bool
  objectMatchesFilter(uint32_t objectIndex, const BimDrawFilter &filter,
                      const BimDrawFilterStateInputs &inputs) const;
  [[nodiscard]] const BimDrawLists &
  filteredDrawLists(const BimDrawFilter &filter,
                    const BimDrawFilterStateInputs &inputs);

private:
  BimDrawFilter cachedFilter_{};
  uint64_t cachedRevision_{std::numeric_limits<uint64_t>::max()};
  BimDrawLists filteredDrawLists_{};
};

[[nodiscard]] bool bimDrawFiltersEqual(const BimDrawFilter &lhs,
                                       const BimDrawFilter &rhs);

} // namespace container::renderer
