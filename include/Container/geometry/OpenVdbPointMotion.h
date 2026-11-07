#pragma once

#include "Container/geometry/OpenVdbPointSelection.h"
#include <optional>

namespace container::geometry::vdb {

struct PointMotionOptions {
  // Row-major matrix multiplying column world positions; translation is 3,7,11.
  // Last row must be exactly 0,0,0,1. Singular deformations (point collapse) are
  // allowed. This changes P only: v, pscale and all other payloads are copied.
  std::array<double, 16> worldFromWorld{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  // Same matrix convention; absent retains the exact source map. A supplied
  // target must have uniform orthogonal axes of length [1e-6,1e6]. Native map
  // construction can reject tiny rotated maps; axis-aligned tiny scales work.
  std::optional<std::array<double, 16>> targetIndexToWorld;
  PointGroupSelection groups;
  // Max component position error, in target voxels, including Float P codec
  // encoding and native Double index rounding. Fixed8 usually needs a larger
  // explicit tolerance than the default. Range (0,.25].
  double maxPositionErrorVoxels{1e-4};
  // Voxel activity cannot represent distinct per-point activity after a merge.
  // Default rejects mixed active/inactive destination cells. false uses OR and
  // reports how many originally inactive records consequently become active.
  bool rejectActivityMerges{true};
  uint64_t maxWork{200'000'000};
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024*1024};
};

struct PointMotionResult {
  PointAttributeSchema schema;
  uint64_t inputCount{0}, selectedCount{0}, outputCount{0};
  uint64_t inputActiveCount{0}, outputActiveCount{0};
  uint64_t inactiveRecordsActivated{0}, outputVoxelCount{0};
  uint64_t workUpperBound{0};
  double maxPositionErrorVoxels{0}, maxPositionErrorWorld{0};
  double maxUnselectedPositionErrorVoxels{0};
  bool noOp{false}, archivedSchema{false};
  std::vector<std::string> diagnostics;
};

// Rounded Double world-affine evaluation and native PointMove repartition every
// stored point, including inactive-voxel
// records; groups select deformation, never deletion. Unselected world P is
// retained within the reported target-codec quantization. Source affine axes
// span [1e-6,1e6], have normalized determinant >=1e-8, and must retain 1e-4
// source-voxel world precision. Destination coordinates have native Int32
// margins; ill-conditioned inverse/world arithmetic rejects before movement.
//
// Registered fixed-stride non-P attribute storage is copied bit for bit,
// including arbitrary codecs, signed zeros/NaNs, strings, hidden arrays and
// group bits. Descriptors and persistent array flags/strides must agree across
// leaves when movement is needed. Dynamic layouts reject for movement; an
// exact no-op keeps them. Transient/streaming payloads and malformed offsets
// reject. Existing P codec is retained, trial encoded/decoded and checked
// against maxPositionErrorVoxels before native allocation. No implicit codec,
// vector, velocity, radius or orientation transformation occurs.
//
// Native global order may change. An internal original-record reference checks
// every output record, payload and position before publication; no ID attribute
// is required or added to the result. Empty/native archived-schema no-ops retain
// their metadata. Grid/file/descriptor metadata is copied except requested
// gridName, regenerated container:point_count and writer statistics.
//
// Private Unicode-safe snapshots, mandatory delayed loading, complete decoded
// array/tree, target topology, native move maps, expanded uniform-stride arrays,
// metadata and writer reserves share maxInputBytes/maxVoxelCount/maxWork caps;
// maxMetadataBytes includes catalog source and predicted output descriptors.
// Logical estimates do not bound eager metadata/legacy nonpaged SDK allocation
// or process RSS. One worker. Outputs are new seekable VDBs; failures remove
// only owned partial files. OFF fails before paths/options. Generation settings
// (voxelSize, halfWidth, maxTriangles) are ignored.
[[nodiscard]] PointMotionResult WriteMovedPoints(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const PointMotionOptions& options = {}, const std::string& inputGrid = {},
    const ProcessingOptions& processing = {},
    const PointAttributeOptions& attributeLimits = {});

} // namespace container::geometry::vdb
