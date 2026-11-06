#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <limits>
#include <vector>

namespace container::geometry::vdb {

enum class FieldCompositeOperation {
  Sum,
  Product,
  Divide,
  Minimum,
  Maximum,
  Blend,
  MergeActive
};
enum class FieldActivity { Union, Intersection, Left, Right };
enum class FieldMergePreference { Left, Right };
enum class FieldVectorExtrema { Componentwise, ScaledMagnitude };
struct FieldCompositeOptions {
  double blendWeight{0.5}; // Blend=(1-weight)*A+weight*B, finite [0,1].
  FieldActivity activity{FieldActivity::Union};
  FieldMergePreference mergePreference{FieldMergePreference::Left};
  FieldVectorExtrema vectorExtrema{FieldVectorExtrema::Componentwise};
  uint64_t maxWorkItems{200'000'000}; // [1,10^10], sparse stored-entry work.
};
enum class FieldValueOperation { Copy, Affine, Absolute, Clamp };
enum class FieldValueScope { ActiveOnly, AllValues };
struct FieldValueTransformOptions {
  FieldValueOperation operation{FieldValueOperation::Copy};
  FieldValueScope scope{FieldValueScope::ActiveOnly};
  std::array<double, 3> scale{1, 1, 1}, offset{};
  std::array<double, 3> minimum{std::numeric_limits<double>::lowest(),
                                std::numeric_limits<double>::lowest(),
                                std::numeric_limits<double>::lowest()};
  std::array<double, 3> maximum{std::numeric_limits<double>::max(),
                                std::numeric_limits<double>::max(),
                                std::numeric_limits<double>::max()};
  uint64_t maxWorkItems{200'000'000};
};
struct FieldCompositeResult {
  ProcessingResult grid;
  uint64_t inputActiveVoxelCountA{0}, inputActiveVoxelCountB{0},
      storedLeafCount{0}, workItems{0};
  std::vector<std::string> diagnostics;
};

// Pointwise ordinary Float/Double/Vec3S/Vec3D compositing. Exact native types,
// class/world/vector semantics and affine origin/Jacobian coefficients must
// match; Transform's approximate equality is insufficient. Source A metadata
// is retained, B metadata is not merged, and recorded output statistics/name
// are regenerated. UNKNOWN/nonnegative FOG scalar fields and UNKNOWN world
// vectors except COVARIANT_NORMALIZE are supported. SDFs, staggered/index-space
// fields and malformed semantic tags fail; SDF Boolean CSG remains separate.
//
// Numerical operations apply to the ENTIRE logical field, including inactive
// values and infinite background: out(x)=f(A(x),B(x)), bg=f(bgA,bgB). Activity
// is an independent Union/Intersection/Left/Right selection. Sparse structural
// nodes are unioned without pruning or tile voxelization; remote inactive
// leaves remain represented. No sampling or implicit conversion occurs.
// Sum/Product/Divide and componentwise extrema act on each stored component.
// Divide rejects ANY zero denominator component, including inactive values
// and the infinite background (0/0 also fails); no output is published. Use a
// denominator field with a finite nonzero background for whole-field division.
// Extrema
// ties select A's component bits. Optional ScaledMagnitude chooses one WHOLE
// vector by overflow-safe rounded Double norm rank, with lexicographic XYZ
// ties and A's bits when numerically identical; it is not an exact real norm.
// Blend endpoints select native A/B value bits exactly, while activity remains
// the requested policy. Binary arithmetic requires finite stored/background
// values; finite output and nonnegative FOG checks never clamp.
//
// MergeActive has FIXED Union activity (other overrides fail). Prefer the
// selected operand where active, otherwise the other active operand, otherwise
// retain A's inactive/background value bits. B's inactive VALUES are ignored,
// while B's structural nodes are retained. It is a value-copy operation and
// can preserve nonfinite payloads. No field arithmetic is performed.
//
// Product/blend use scaled products and compensated sums to avoid avoidable
// intermediate range loss. Meaningful nonzero results lost to zero and any
// nonfinite output fail before publication; ordinary native rounding remains.
// Encoded inputs, full decoded/union topology, metadata, scratch and writer
// share explicit byte/voxel/work caps before expansion. One worker; exclusive
// ZIP-only seekable output preserves unmodified inactive signed-zero bits.
// Upstream metadata decoding precedes checks; this is not a hostile-file RSS
// sandbox. Inputs/existing outputs stay intact; OFF precedes I/O/options.
[[nodiscard]] FieldCompositeResult WriteCompositeField(
    const std::filesystem::path &inputA, const std::filesystem::path &inputB,
    const std::filesystem::path &output, FieldCompositeOperation operation,
    const FieldCompositeOptions &composite = {}, const std::string &gridA = {},
    const std::string &gridB = {}, const ProcessingOptions &processing = {});

// Copy is an exact native clone, including finite/nonfinite value bits and all
// activity/topology/background. The ordinary-type/semantic/map restrictions
// above still apply; F03 file authoring provides passthrough for other classes.
// Other operations transform finite OPERATED values componentwise: scale*x+
// offset, abs(x), or closed-interval clamp. ActiveOnly transforms active voxels
// AND active tiles, preserving every inactive/background bit. AllValues also
// transforms stored inactive values and the background, without changing
// activity/topology. No pruning, densification or interpolation occurs.
// Scalars use only X parameters; Y/Z must retain their defaults. Unused option
// arrays must be defaults. Affine identity retains original value bits; zero
// scale returns offset bits. Clamp ties retain original bits; used bounds are
// rounded inward to representable native values, or fail if no native value
// fits the interval/nonzero bound is lost to zero. Scaled FMA-based
// work uses Double intermediates before conversion to the original native
// type; overflow/nonzero-to-zero loss fails. Fog's operated output is checked
// nonnegative; unoperated values are untouched. Shared budgets/output rules
// are identical to the binary API, and output name comes from
// ProcessingOptions.
[[nodiscard]] FieldCompositeResult
WriteTransformedField(const std::filesystem::path &input,
                      const std::filesystem::path &output,
                      const FieldValueTransformOptions &transform = {},
                      const std::string &inputGrid = {},
                      const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
