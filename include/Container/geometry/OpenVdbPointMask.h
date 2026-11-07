#pragma once

#include "Container/geometry/OpenVdbPointSelection.h"

namespace container::geometry::vdb {

struct PointMaskOptions {
  bool writeCounts{false};
  std::string countGridName{"point_count"};
};
struct PointMaskResult {
  uint64_t inputCount{0}, selectedCount{0}, selectedActive{0}, selectedInactive{0};
  uint64_t occupiedVoxelCount{0}, leafCount{0};
  std::string maskGridName, countGridName;
  bool sourceArchivedSchema{false};
};

// Marks the ORIGINAL stored voxel of every selected native point, including
// points in inactive voxels. No P decode, world-space repartitioning, radius
// coverage, dilation or scalar/vector attribute reinterpretation takes place.
// The source transform is copied exactly, including nonlinear transforms.
// Group semantics/order/limits match CountSelectedPoints. P03's registered
// schema, descriptor, offset and group-backing validation still applies.
//
// One seekable .vdb contains a native MaskGrid named processing.gridName and,
// optionally, an Int64Grid with exact selected-point counts in occupied voxels
// and zero background. Both outputs have identical active voxel topology;
// empty selections and validated empty-schema archives yield empty grids.
// Mask values are true exactly where active. Output class is GRID_UNKNOWN.
// Custom grid metadata survives except regenerated native structural/file
// statistics, container:point_count and the three reserved v1 empty archives
// (container:point_empty_manifest_v1/descriptor_v1/leaf_flags_v1). Other custom
// metadata using the same prefix survives unchanged.
// Distinct container:point_voxel_* provenance describes source/selection/units.
// Point descriptor attributes/groups are intentionally not copied to scalars.
//
// Source snapshots, loaded tree/attributes/metadata, predicate storage and
// conservative prospective leaf/internal/root/writer workspace share the
// byte budget BEFORE output tree allocation. Aggregate leaf slots and occupied
// voxels ACROSS output grids obey maxVoxelCount; point counts obey maxPoints
// and fit Int64 exactly. Result leaf/voxel counts describe the common topology.
// Upstream metadata/nonpaged reads may allocate before estimates are known:
// logical budgets are not a hostile-file RSS sandbox. One worker, ZIP without
// active-mask compression, exclusive new output and owned temporary cleanup.
// Source bytes remain unchanged. Generation/triangle fields are ignored.
// OFF reports ENABLE_OPENVDB=ON before options or filesystem access.
[[nodiscard]] PointMaskResult WritePointVoxelMask(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const PointGroupSelection& selection = {}, const PointMaskOptions& mask = {},
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointAttributeOptions& options = {});

} // namespace container::geometry::vdb
