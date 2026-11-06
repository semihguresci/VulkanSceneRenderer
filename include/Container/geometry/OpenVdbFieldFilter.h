#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <vector>

namespace container::geometry::vdb {

enum class FieldFilterOperation { Mean, Gaussian, Median, Offset };
struct FieldFilterOptions {
  uint32_t width{1};      // Radius in INDEX voxels, [1,4].
  uint32_t iterations{1}; // [1,32]; offset is applied once per iteration too.
  // Offset uses stored scalar/vector component units. Scalars require y=z=0.
  std::array<double, 3> offset{};
  bool voxelizeActiveTiles{false};
  std::filesystem::path mask{}; // Empty disables masking.
  std::string maskGridName{};
  double maskMinimum{0}, maskMaximum{1};
  bool invertMask{false};
  uint64_t maxWorkItems{200'000'000}; // Conservative kernel work, [1,10^10].
};
struct FieldFilterResult {
  ProcessingResult grid;
  uint64_t inputActiveVoxelCount{0}, activeTileCount{0}, voxelizedTileCount{0},
      storedLeafCount{0};
  uint32_t kernelPasses{0};
  bool maskUsed{false};
  std::vector<std::string> diagnostics;
};

// Ordinary Float/Double/Vec3S/Vec3D value filtering using tools::Filter.
// Scalar classes are UNKNOWN or nonnegative FOG; vectors must be UNKNOWN,
// world-space and not COVARIANT_NORMALIZE. SDFs, staggered/index-local vectors
// and unclear class/semantic combinations fail: this is separate from tracked
// SDF smoothing and MAC fluid operators. Finite nonsingular affine maps are
// retained. Kernels are in index voxels, not isotropic world distances.
//
// Mean/Gaussian are componentwise. Gaussian is the upstream approximation of
// FOUR box iterations per requested iteration. Vector median is the upstream
// LEXICOGRAPHIC (X,Y,Z) rank, not a componentwise or Euclidean vector median.
// Upstream means use Float reciprocal weights even for Double values, and
// masking uses Float alpha weights. These native precision choices can round
// active results; finite postchecks and nonnegative fog checks never clamp.
//
// The default keeps topology, active tiles, background and all inactive value
// bits unchanged. Active tiles contribute to neighbors but are not modified.
// Optional upfront full active-tile voxelization filters their cells too;
// leaf-aligned prospective active bounds and retained remote inactive topology
// are budgeted before expansion. No inactive cell is activated. Neighbors use
// their stored values (including inactive/background), not an activity mask.
//
// Optional Float/Double masks require exactly matching transforms. Mask value
// activity is ignored: inactive/background values also define the alpha.
// Values <=min map0, >=max map1; interior t=(value-min)/(max-min) maps through
// SmoothStep t*t*(3-2*t), optionally inverted. A normalized interior value or
// either nonzero blend weight lost in Float precision is rejected. The mask
// is normalized safely before upstream processing; it is never modified.
// With separable kernels, the alpha blend occurs at EACH axis pass, as in
// upstream Filter, rather than once after a complete 3D convolution.
//
// Finite stored values and conservative native sum/offset range are checked;
// nonzero offsets unrepresentable in the source type fail. Native rounding/
// underflow of filtered active values is not claimed to be exact arithmetic.
// Input bytes, recorded statistics, fully decoded trees, masks, clones,
// topology expansion, buffers, stencil work and writer share explicit limits.
// Upstream metadata/catalog decoding precedes caller checks; estimates are not
// a hostile-file RSS sandbox.
// Source metadata/transforms remain; output name comes from ProcessingOptions.
// ZIP-only exclusive seekable output preserves inactive signed zero. One
// worker; no output or owned temporary survives failure. OFF precedes I/O.
[[nodiscard]] FieldFilterResult WriteFilteredField(
    const std::filesystem::path &input, const std::filesystem::path &output,
    FieldFilterOperation operation, const FieldFilterOptions &filter = {},
    const std::string &inputGrid = {},
    const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
