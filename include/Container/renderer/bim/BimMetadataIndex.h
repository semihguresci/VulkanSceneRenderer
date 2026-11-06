#pragma once

#include "Container/renderer/bim/BimManager.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace container::renderer {

class BimMetadataIndex {
public:
  void clear();
  void reserve(size_t objectCount);
  void index(const BimElementMetadataView &metadata);

  [[nodiscard]] std::span<const uint32_t>
  objectIndicesForGuid(std::string_view guid) const;
  [[nodiscard]] std::span<const uint32_t>
  objectIndicesForSourceId(std::string_view sourceId) const;

private:
  BimObjectIndexLookup objectIndicesByGuid_{};
  BimObjectIndexLookup objectIndicesBySourceId_{};
};

} // namespace container::renderer
