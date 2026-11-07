#pragma once

#include "Container/geometry/OpenVdbPointSelection.h"

namespace container::geometry::vdb {

enum class PointTransferMode { WeightedSum, WeightedMean };
enum class PointTransferStorage { Double, Float };
enum class PointTransferCompanion { WeightSum, WorldDensity };
enum class PointTransferIntegers { RejectInexact, AllowRounded };
struct PointTransferOptions {
  // Row-major COLUMN-vector map; translation at3,7,11. Uniform orthogonal
  // target axes [1e-6,1e6], with rotation/reflection. Tiny rotated AffineMaps
  // may exceed SDK determinant limits; tiny aligned/reflected maps work.
  std::array<double,16> targetIndexToWorld{.1,0,0,0, 0,.1,0,0, 0,0,.1,0, 0,0,0,1};
  PointGroupSelection groups;
  PointTransferMode mode{PointTransferMode::WeightedMean};
  PointTransferStorage storage{PointTransferStorage::Double};
  PointTransferCompanion companion{PointTransferCompanion::WeightSum};
  PointTransferIntegers integers{PointTransferIntegers::RejectInexact};
  // A chosen weight attribute must be scalar and strictly positive on selected
  // records. Otherwise constantMass is used. It must remain1 when an attribute
  // is supplied, so an irrelevant nondefault constant never goes unnoticed.
  std::string weightAttribute;
  double constantMass{1};
  std::string companionGridName; // Empty => gridName+"_weight" or +"_density".
  uint64_t maxWork{2'000'000'000};
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024*1024};
};
struct PointTransferResult {
  ProcessingResult field, companion;
  uint64_t inputCount{0}, selectedCount{0}, selectedInactiveCount{0};
  uint64_t contributionCount{0}, roundedIntegerComponents{0};
  uint64_t roundedFloatComponents{0}, workUpperBound{0};
  uint32_t componentCount{1};
  double voxelVolume{0}, totalMass{0}, depositedMass{0};
  // Upper bound on absolute mass error caused by native companion rounding.
  // A positive residual below Double range reports denorm_min conservatively.
  double massErrorUpperBound{0}, maxFloatAbsoluteError{0};
  std::array<double,3> minimum{}, maximum{};
  bool hasValues{false}, archivedSchema{false};
  std::vector<std::string> diagnostics;
};

// Generic collocated eight-center CIC/hat transfer through native PointTransfer.
// Bool/Int32/Int64/Float/Double/Vec3I/Vec3S/Vec3D runtime-registered numeric
// codecs are accepted; chosen P, group backing, strings, dynamic layouts and
// strides other than1 reject. Only selected records are decoded for finite
// field/positive mass/P checks, including inactive-voxel point records. All
// named groups must exist. Unrelated attributes remain untouched in the source.
// Scalars and vector components are caller-supplied stored values: v, normals,
// colors and local-looking vectors receive NO implicit Jacobian, translation,
// normalization or unit conversion. Only P determines deposition coordinates.
// Int64 conversion that loses native integer bits rejects by default; explicit
// AllowRounded counts affected field/weight component conversions.
//
// Sum=sum(value*mass*kernel); Mean=Sum/sum(mass*kernel). The always-Double
// companion is either this weight sum or weight/worldVoxelVolume. voxelVolume
// is the correctly rounded absolute determinant of the actual target axes;
// density divides by that reported Double value. Both grids have zero inactive
// backgrounds, UNKNOWN class, identical positive-weight support, and world/
// invariant vector semantics. Zero-valued/cancelled field cells stay active.
//
// Cached conditioned Double target coordinates avoid native Float P/kernel
// narrowing. Exact dyadic fractions and complements partition unity; fixed
// 8192-bit accumulators retain five-factor products and cancellation before
// rounded rational output. Native PointTransfer performs leaf traversal through
// an integer-bucket ordinal proxy. This is an adaptation of the public transfer
// framework, not the stock floating-only normalized rasterizeTrilinear helper.
// Point counts<=UInt32 plus native Int32 per-leaf offsets prove the accumulator
// exponent/count bounds. Exact canonical deposited mass is verified before
// output rounding. Nonzero field/weight/density outputs that round to zero or
// overflow reject. Global reported total/integrated masses must also fit Double.
// Float field opt-in checks range/underflow and reports rounding.
//
// Source affine axes [1e-6,1e6], normalized determinant>=1e-8 and bounded
// world/inverse arithmetic uncertainty; native coordinate margins apply.
// Custom source grid/file metadata is copied except regenerated point/native
// structural fields and known MAC/solver/surface-field provenance. Source bytes,
// unrelated columns and descriptor metadata
// are preserved in the original input. Native empty-schema archives validate;
// an empty selection emits empty grids if the chosen schema is numeric.
// Mandatory delayed loading/full source memory preflight, private Unicode-safe
// snapshots, source+joint native trees/arrays/nodes, exact per-leaf heap scratch,
// traversal/limb work, metadata and ZIP writer budgets precede allocation.
// These logical caps are not an RSS sandbox for eager metadata/legacy encodings.
// One worker; exclusive seekable output and owned-partial cleanup. Generation
// voxelSize/halfWidth/maxTriangles are ignored. OFF fails before options/paths.
[[nodiscard]] PointTransferResult WriteTransferredPointAttribute(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const std::string& attribute, const PointTransferOptions& options = {},
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointAttributeOptions& attributeLimits = {});

} // namespace container::geometry::vdb
