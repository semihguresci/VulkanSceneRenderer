#pragma once

#include "Container/geometry/GeometryLod.h"
#include "Container/geometry/GeometryLodSelection.h"
#include "Container/geometry/Model.h"
#include "Container/utility/DerivedAssetCache.h"

namespace container::geometry {

struct GeometryLodPrimitive {
  uint32_t firstIndex{};
  uint32_t indexCount{};
  std::vector<GeometryLodRange> levels;
};

struct GeometryLodPreparationOptions {
  uint32_t maxBufferIndexCount{128u * 1024 * 1024 / sizeof(uint32_t)};
  uint32_t sourceIndexBudget{1'500'000};
  TriangleLodWeldPolicy weldPolicy{TriangleLodWeldPolicy::PreserveAttributes};
};

struct GeometryLodCacheStats {
  util::DerivedAssetCacheStatus status{util::DerivedAssetCacheStatus::Disabled};
  std::string key;
  uint64_t bytesRead{};
  uint64_t bytesWritten{};
  double keyMilliseconds{};
  double readMilliseconds{};
  double generationMilliseconds{};
  double writeMilliseconds{};
};

struct PreparedGeometryLods {
  std::vector<GeometryLodPrimitive> ranges;
  // Empty if no ranges could be reduced. Otherwise includes the canonical
  // draw stream followed by the reduced ranges so one binding serves both.
  std::vector<uint32_t> indices;
  GeometryLodCacheStats cache;
};

// An empty directory disables disk caching. Encoded entries persist only the
// reduced index tail, with their source key covering the packed input streams,
// primitive domains, and derivation settings.
[[nodiscard]] PreparedGeometryLods prepareGeometryLods(
    std::span<const Vertex> vertices, std::span<const uint32_t> source,
    std::span<const PrimitiveRange> primitives,
    const std::filesystem::path& cacheDirectory = {},
    GeometryLodPreparationOptions options = {},
    std::stop_token stop = {});

} // namespace container::geometry
