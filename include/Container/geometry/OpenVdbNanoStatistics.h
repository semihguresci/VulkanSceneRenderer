#pragma once
#include "Container/geometry/OpenVdbNanoInterchange.h"
#include <optional>
#include <variant>

namespace container::geometry::vdb {
enum class NanoVdbStatisticsMode { Bounds, MinMax, All };
using NanoVdbStatisticValue =
    std::variant<bool, int32_t, int64_t, float, double, std::array<float, 3>,
                 std::array<double, 3>>;
struct NanoVdbStatisticsSummary {
  uint32_t sourceIndex{};
  std::string name, valueType;
  uint64_t activeVoxels{};
  std::optional<std::array<int64_t, 3>> indexMinimum, indexMaximum;
  std::optional<NanoVdbStatisticValue> minimum, maximum;
  std::optional<double> average, standardDeviation;
};
struct NanoVdbStatisticsResult {
  NanoVdbFileInfo file;
  std::vector<NanoVdbStatisticsSummary> summaries;
  std::vector<std::string> diagnostics;
  uint64_t work{};
};
// Recompute native active-value caches while retaining every value, state,
// affine map and blind payload. Bounds are inclusive index bounds. Vector
// extrema compare native squared magnitudes; vector moments describe SQUARED
// magnitudes (the upstream cache convention), rather than components or lengths.
// Integer extrema retain their native exact type. Moments use upstream Double
// accumulation and native cache precision, and are explicitly approximate.
// Bool/Mask expose occupancy bounds without numeric caches. Empty grids expose
// no extrema/moments. Unsafe squared magnitudes/moment accumulation reject.
// Active regions ending at INT32_MAX require recentering because the upstream
// statistics pass constructs an exclusive bound in Int32.
//
// Uses the checked immutable I/O API through private raw intermediates instead
// of exposing mutable archive pointers. All passes share the work/workspace
// cap; intermediate encoded bytes are bounded by maxNativeBytes, while user
// input/output obey maxInputBytes. Structural corruption always fails. An old
// checksum mismatch requires explicit read.verifyChecksums=false.
[[nodiscard]] NanoVdbStatisticsResult WriteNanoVdbStatistics(
    const std::filesystem::path &input, const std::filesystem::path &output,
    NanoVdbStatisticsMode = NanoVdbStatisticsMode::All,
    const NanoVdbReadOptions & = {}, const NanoVdbWriteOptions & = {},
    const NanoVdbLimits & = {});
} // namespace container::geometry::vdb
