#pragma once

#include "Container/geometry/OpenVdbDenseInterchange.h"
#include <memory>

namespace container::geometry::vdb {
struct MultiResolutionOptions {
  uint32_t levels{3};              // [1,8], level0 finest. One level is an exact passthrough.
  uint64_t maxWork{1'000'000'000}; // [1,1e10], conservative voxel/filter-pass bound.
};
struct MultiResolutionLevelInfo {
  uint32_t level{};
  std::string gridName, valueType;
  uint64_t activeVoxelCount{}, leafCount{};
  std::array<double, 3> voxelSize{};
};
class MultiResolutionVolume;
[[nodiscard]] std::shared_ptr<const MultiResolutionVolume>
BuildMultiResolution(const std::filesystem::path &input,
                     const MultiResolutionOptions &multiresolution = {},
                     const std::string &inputGrid = {}, const ProcessingOptions &processing = {});

// Immutable bounded full-weighting hierarchy built by installed OpenVDB
// MultiResGrid. Float/Double and Vec3S/Vec3D retain native numeric types.
// Scalars support unknown/fog class (fog must be nonnegative with background0).
// Vectors require unknown class, collocated world-space invariant or
// contravariant-relative components. Level sets/MAC/point/index-space/normal/
// position fields are rejected. Inactive stored values must equal background.
// This is a low-pass LoD hierarchy, not an SDF rebuild or a multigrid solver.
//
// Integer levelL has the source affine transform pre-scaled by2^L in INDEX
// axes: world translation stays fixed and stored world vectors are not scaled.
// Full weighting uses the upstream27-point [1,2,1]^3/64 stencil; activity is the
// dilated/decimated upstream topology, not a mass-conservation guarantee.
// Input axes [1e-6,1e6], nonsingular/conditioned affine including shear/reflection;
// inverse-map determinant and world/index precision limits also apply.
// Filtered numeric input components must be zero or within [128*minNormal,max/128] in
// their native precision, preventing filter's unnormalized intermediates from
// overflowing. The one-level passthrough only requires finite values.
// Filtering/interpolation use native arithmetic including rounding/underflow;
// the hierarchy is a low-pass approximation, not lossless numeric interchange.
// Active-tile voxelization, retained levels, leaf-aligned masks, metadata and
// serial scratch are preflighted together before the upstream constructor.
class MultiResolutionVolume {
public:
  ~MultiResolutionVolume();
  MultiResolutionVolume(const MultiResolutionVolume &) = delete;
  MultiResolutionVolume &operator=(const MultiResolutionVolume &) = delete;
  [[nodiscard]] const std::vector<MultiResolutionLevelInfo> &levels() const noexcept;
  // World-position trilinear sample at integer/fractional LoD. Fractional LoD
  // linearly blends the two adjacent independently sampled integer levels.
  // Centered Double inverse mapping requires1e-8 finest-voxel fractional index
  // precision; unresolved coordinates fail rather than silently alias cells.
  [[nodiscard]] DenseValue sampleWorld(const std::array<double, 3> &position,
                                       double level = 0) const;
  [[nodiscard]] ProcessingResult writeLevel(const std::filesystem::path &output, uint32_t level,
                                            const ProcessingOptions &processing = {}) const;
  void writeAll(const std::filesystem::path &output,
                const ProcessingOptions &processing = {}) const;

private:
  struct Impl;
  explicit MultiResolutionVolume(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
  friend std::shared_ptr<const MultiResolutionVolume>
  BuildMultiResolution(const std::filesystem::path &, const MultiResolutionOptions &,
                       const std::string &, const ProcessingOptions &);
};
// Source custom metadata is preserved; producer/LoD fields are regenerated.
// Every retained level counts against voxel/byte budgets, including writer
// calls while the hierarchy stays alive. Generation/gridName options are
// ignored. One-worker construction, deterministic levels, exclusive seekable
// outputs. OFF fails before file/options access. Source bytes remain untouched.
} // namespace container::geometry::vdb
