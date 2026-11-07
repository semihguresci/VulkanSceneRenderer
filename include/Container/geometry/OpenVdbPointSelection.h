#pragma once

#include "Container/geometry/OpenVdbPointAttributes.h"

namespace container::geometry::vdb {

struct PointGroupSelection {
  // Empty anyOf is true; empty allOf is true; empty noneOf excludes nothing.
  // (ANY(anyOf) || anyOf.empty()) && ALL(allOf) && !ANY(noneOf).
  // All named groups must exist. Duplicate names have no extra effect.
  std::vector<std::string> anyOf, allOf, noneOf;
};
enum class PointSelectionMode { KeepSelected, DeleteSelected };
struct PointSelectionCounts {
  uint64_t total{0}, selected{0}, selectedActive{0}, selectedInactive{0};
  PointAttributeSchema schema;
  bool archivedSchema{false};
};
struct PointSelectionResult {
  uint64_t inputCount{0}, selectedCount{0}, outputCount{0};
  PointAttributeSchema schema;
  bool archivedSchema{false};
};
struct PointAttributeStatistics {
  PointAttributeDescription attribute;
  uint64_t selectedCount{0};
  bool hasValues{false};
  uint32_t componentCount{1};
  // Exact native extrema: one typed value, or an empty typed vector if no
  // selected points. Vector extrema are componentwise, not magnitude extrema.
  PointAttributeValues minimum, maximum;
  std::array<double,3> mean{0,0,0}, populationStdDev{0,0,0};
};

// Counts include inactive-voxel points. Native ordering is identical to P03:
// lexicographic leaf origin, then local stored index. No world-position sort.
// A zero-point archive is explicitly validated using bounded lengths/counts
// and byte checksums; its opaque native descriptor is never deserialized here.
[[nodiscard]] PointSelectionCounts CountSelectedPoints(
    const std::filesystem::path& input, const PointGroupSelection& selection = {},
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointAttributeOptions& options = {});

// Stable native compaction retains registered fixed-stride attributes/codecs,
// descriptor/grid metadata, hidden flags, group bits, transform and per-voxel
// active/inactive state in surviving leaves. Exact IDs and P offsets are copied
// as native storage, without position repartitioning or codec conversion.
// Empty leaves are removed explicitly; inactive leaves with points survive.
// Dynamic-stride attributes reject if any point would be removed.
//
// Keeping none/deleting all produces a valid empty native tree. Since native
// descriptors live in leaves, three reserved metadata keys retain a version1
// opaque OpenVDB12 descriptor, original per-leaf flags/strides and a bounded
// public schema manifest. Existing reserved-key conflicts reject. The result
// and CountSelectedPoints report archivedSchema=true and the zero-point schema.
// Ordinary native tools see zero points; repopulation/restoration is separate.
// Original metadata is preserved except regenerated container:point_count and
// the requested processing.gridName. Logical memory/encoded/topology/string
// budgets are shared; one worker, private snapshots and exclusive seekable
// output follow P03. Generation/triangle settings are ignored. OFF fails first.
[[nodiscard]] PointSelectionResult WriteSelectedPoints(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const PointGroupSelection& selection = {},
    PointSelectionMode mode = PointSelectionMode::KeepSelected,
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointAttributeOptions& options = {});

// Seven numeric P03 types (bool/int32/int64/float/double/Vec3f/Vec3d), native
// NullCodec and stride1. P is intentionally rejected: its values are voxel-local
// offsets, not world-position statistics. Strings and opaque codecs reject.
// Fixed4608-bit integer/binary accumulators avoid overflow, cancellation and
// squared underflow, including ±DBL_MAX and adjacent large int64 IDs. Per point
// work uses fixed144-limb integer arrays/component; final square roots use128
// guard bits and mean/stddev are rounded to Double (not exact real numbers).
// Final Double
// means/stddevs that underflow to zero fail explicitly. Empty selection yields
// selectedCount0, hasValues=false, empty typed extrema and zero diagnostics.
[[nodiscard]] PointAttributeStatistics MeasurePointAttribute(
    const std::filesystem::path& input, const std::string& attribute,
    const PointGroupSelection& selection = {}, const std::string& inputGrid = {},
    const ProcessingOptions& processing = {}, const PointAttributeOptions& options = {});

} // namespace container::geometry::vdb
