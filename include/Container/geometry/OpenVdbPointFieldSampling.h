#pragma once

#include "Container/geometry/OpenVdbPointSelection.h"

namespace container::geometry::vdb {

enum class PointFieldInterpolation { Nearest, Linear, Quadratic };
enum class PointFieldPrecision { PromoteFloatingToDouble, PreserveSource };
struct PointFieldSamplingOptions {
  std::string attributeName{"sampled"};
  PointEditOperation operation{PointEditOperation::Add}; // Add or Set only.
  PointFieldInterpolation interpolation{PointFieldInterpolation::Linear};
  PointFieldPrecision precision{PointFieldPrecision::PromoteFloatingToDouble};
  PointGroupSelection groups;
  uint64_t maxWork{
      16'000'000}; // Catalog, validation, traversal and stencil fetches.
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{
      1024 * 1024}; // Aggregate point/field/descriptor metadata.
};
struct PointFieldSamplingResult {
  uint64_t inputCount{0}, selectedCount{0}, writtenCount{0};
  PointAttributeSchema schema;
  std::string fieldGridName, fieldValueType, vectorSemantics;
};

// Samples at each selected native point's world position (voxel coordinate plus
// decoded P), including points in inactive voxels. Field values live at integer
// voxel centers. Background and inactive values participate without clipping.
// Selection/order match P03. Both inputs are privately snapshotted; P,
// topology, transforms, groups, metadata, codecs and unselected attribute bits
// survive.
// Point-file metadata is retained; field-file metadata is budgeted, not merged.
//
// Nearest bool/int32/int64 is exact; their interpolated modes reject. Float,
// Double and class-UNKNOWN collocated world Vec3S/D support nearest/linear/
// quadratic. Every valid already-world vector tag is accepted, including
// covariant-normalize: stored components are sampled without renormalization.
// Floating
// outputs default to Double/Vec3d; PreserveSource permits Float/Vec3f rounding
// but rejects overflow and nonzero-to-zero narrowing. Quadratic uses the native
// floor(index)+[-1,0,1] stencil, can overshoot and is never clamped. Integer
// centers are exact; general quadratic stencils reject exponent ranges whose
// normalized nonzero terms become subnormal/zero rather than losing them.
// Explicit
// negative-face, voxels_per_second native MAC grids support nearest/linear
// only; their +0.5 component samples are converted through J to world Vec3d.
// Nonzero MAC transform terms lost to zero in Double are rejected explicitly.
// Result vectorSemantics records interpretation; a point attribute has no grid
// tags.
//
// Add requires a missing name and initializes unselected points to zero. Set
// requires the exact resulting native type, NullCodec and stride1; it preserves
// unselected payload bits, including nonfinite values and signed zeros. P and
// group backing arrays cannot be targets. Explicit matching Set of v/pscale is
// allowed without imposing velocity/radius meaning on generic field values.
// Empty selection Add still adds a schema/defaults; Set is a value no-op.
// Empty trees/archived empty schemas need separate native repopulation first.
//
// Affine axes span [1e-6,1e6]; precision is guarded to 1e-4 of the smaller
// point/ field axis. Stored field values must be finite. Budgets jointly
// reserve both encoded/decoded sources, metadata, clone, attribute and writer
// workspace. Point/field active voxels and stored leaf slots share
// maxVoxelCount. These logical bounds are not a hard RSS sandbox for legacy
// eager encodings. One worker, exclusive seekable ZIP output, no partial
// output. Generation and triangle controls are ignored; processing.gridName
// names the output. OFF reports ENABLE_OPENVDB=ON before any option or
// filesystem access.
[[nodiscard]] PointFieldSamplingResult WriteSampledPointAttribute(
    const std::filesystem::path &points, const std::filesystem::path &field,
    const std::filesystem::path &output,
    const PointFieldSamplingOptions &options = {},
    const std::string &pointGrid = {}, const std::string &fieldGrid = {},
    const ProcessingOptions &processing = {},
    const PointAttributeOptions &attributeLimits = {});

} // namespace container::geometry::vdb
