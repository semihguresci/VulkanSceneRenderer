#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <optional>
#include <vector>

namespace container::geometry::vdb {
enum class FieldResamplingInterpolation { Nearest, Linear, Quadratic };
enum class FieldResamplingActivity { NearestSource, AnyContributingStencil };
enum class FieldVectorTransform { Semantic, PreserveWorldComponents };
struct FieldResamplingOptions {
  // Public row-major COLUMN matrices, translation at3,7,11, affine final row.
  // Omission retains the exact source map. Shear/reflection/nonuniform allowed.
  std::optional<std::array<double, 16>> targetIndexToWorld;
  // Invertible WORLD deformation; sampling uses source^-1 * deformation^-1 *
  // target. Changing target alone does not transform vector values.
  std::array<double, 16> worldFromWorld{1, 0, 0, 0, 0, 1, 0, 0,
                                        0, 0, 1, 0, 0, 0, 0, 1};
  FieldResamplingInterpolation interpolation{
      FieldResamplingInterpolation::Linear};
  FieldResamplingActivity activity{
      FieldResamplingActivity::AnyContributingStencil};
  FieldVectorTransform vectorValues{FieldVectorTransform::Semantic};
  uint64_t maxWorkItems{1'000'000'000};
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024 * 1024};
};
struct FieldResamplingResult {
  ProcessingResult grid;
  std::string valueType;
  uint64_t inputActiveVoxelCount{0}, inputStoredLeafCount{0},
      outputStoredLeafCount{0};
  uint64_t sampledVoxelCount{0}, inactiveSampleCount{0}, workUpperBound{0};
  uint64_t roundedFloatComponents{0};
  double maximumFloatAbsoluteError{0};
  bool exactCopy{false};
  // Inclusive XYZ minimum followed by XYZ maximum. Empty support is null.
  std::optional<std::array<int32_t, 6>> sourceSupportBounds, targetBounds;
  std::vector<std::string> diagnostics;
};

// Ordinary native Float/Double/Bool/Mask/Int32/Int64/Vec3I/Vec3S/Vec3D UNKNOWN
// grids; scalar nonnegative FOG is also accepted. LEVEL_SET/staggered/LOCAL
// vectors/nonlinear maps reject. Scalar semantics must be WORLD/invariant.
// Present semantic metadata must have recognized native types/values.
// Nearest is exact native value+activity, half ties toward greater integer.
// Bool inactive true and active false remain distinct; Mask uses native
// activity. Integer/Bool/Mask/Vec3I require Nearest. Floating real grids
// additionally support linear/quadratic at integer cell centers.
// Inactive/background values participate; activity is independent of value and
// uses nearest source or ANY nonzero kernel contributor (including negative
// quadratic lobes).
//
// For NONIDENTITY world deformation A,t with Semantic vector policy:
// Invariant stays raw, Relative=A*v, Absolute=A*p+t, Covariant=A^-T*v,
// CovariantNormalize=normalized(A^-T*v), with zero remaining zero. Infinite
// background is transformed too. Identity deformation preserves raw values,
// including nonunit normalized-tag fields. PreserveWorldComponents explicitly
// leaves all components unchanged. Vector semantic tags remain unchanged.
// Vec3I noninvariant semantic deformation is explicitly limited to signed
// axis permutations with power-of-two axis scales. Exact dyadic products and
// error-free translation sums must yield integral Int32 components; no rounded
// near-integer passes. Normalized Vec3I rejects every exact non-axis result,
// even if it would round to an axis. No implicit truncation/promotion occurs.
//
// Exact identity map/deformation copies native topology and value/activity
// bits. Other routes preserve finite native types, with Float rounding
// diagnostics and explicit overflow/nonzero-underflow/detectable
// interpolation-loss errors. Quadratic has exact center shortcuts but may
// reject extreme dynamic range. Fog negative overshoot rejects. This is
// single-pass point sampling: no antialias, distance rebuild or
// mass-conservation guarantee.
//
// Source support includes every active stored item and every
// bitwise-nonbackground inactive value, including signed zero/subnormals. A
// conservative kernel-padded transformed target box covers all finite
// deviations from the transformed infinite background. Every cell in that box
// is sampled/stored without approximate pruning. Sparse widely separated
// domains can exceed caps. Maps/deformation must be
// affine/invertible/conditioned; source/target voxel axes [1e-6,1e6], Int32
// margins and independently evaluated compensated pull coordinates
// resolving1e-4 source voxel are required. Tiny aligned maps work; tiny rotated
// targets may exceed native AffineMap determinant limits.
//
// Native Unicode snapshots, mandatory delayed/full typed-buffer preflight,
// aggregate file/catalog metadata, source/root/node traversal, joint output
// boxes/leaf slots/native trees/work and ZIP writer reserves precede
// allocation. One worker. Source/file custom metadata and exact native target
// coefficients survive regenerated structural/provenance fields. Exclusive
// seekable outputs, owned-partial cleanup; OFF before options/paths. Logical
// estimates are not an RSS sandbox for eager metadata/legacy decodes. Only
// processing name/byte/voxel settings apply; generation
// voxelSize/halfWidth/maxTriangles are ignored.
[[nodiscard]] FieldResamplingResult
WriteResampledField(const std::filesystem::path &input,
                    const std::filesystem::path &output,
                    const FieldResamplingOptions &options = {},
                    const std::string &inputGrid = {},
                    const ProcessingOptions &processing = {});
} // namespace container::geometry::vdb
