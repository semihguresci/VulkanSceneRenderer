#pragma once
#include "Container/geometry/OpenVdbNanoStatistics.h"

namespace container::geometry::vdb {
enum class NanoVdbPrimitiveShape { Sphere, Box, Torus };
enum class NanoVdbPrimitiveClass { LevelSet, Fog };
enum class NanoVdbPrimitiveStorage { Float, Double };
struct NanoVdbPrimitiveOptions {
  NanoVdbPrimitiveClass gridClass{NanoVdbPrimitiveClass::LevelSet};
  NanoVdbPrimitiveStorage storage{NanoVdbPrimitiveStorage::Float};
  double voxelSize{.125},halfWidth{3}; // Positive h[1e-6,1e6], band[1,64] voxels.
  std::array<double,3> center{},origin{};
  double radius{1};
  std::array<double,3> boxDimensions{2,2,2}; // Full widths in WORLD units.
  double majorRadius{1},minorRadius{.25}; // Torus symmetry axis is WORLD Y.
  std::string gridName{"primitive"};
  NanoVdbStatisticsMode statistics{NanoVdbStatisticsMode::All};
};
struct NanoVdbPrimitiveResult {
  NanoVdbFileInfo file;
  std::array<int32_t,3> candidateMinimum{},candidateMaximum{};
  uint64_t candidateVoxels{},storedLeafVoxelUpperBound{},nodeUpperBound{};
  uint64_t nativeByteUpperBound{},workspaceUpperBound{},workUpperBound{};
  uint64_t roundedFloatValues{};
  double maximumFloatAbsoluteError{},maximumFloatMapCacheErrorVoxels{};
  bool floatMapCachesRounded{};
  std::vector<std::string> diagnostics;
};

// Source-free CPU NanoVDB sphere, axis-aligned box or Y-axis torus. Native
// Double primitive construction/flood/fog tools precede checked Float/Double
// serialization; Float geometry parameters are never prematurely narrowed.
// Unused shape parameters retain defaults. Sphere radius and box half-widths,
// or the torus minor radius, must resolve at least 1.5 voxels; native silently
// empty sub-Nyquist primitives reject. Dimensions/radii are finite [1e-6,1e6],
// torus major>minor, centers/origins finite with adequate WORLD precision.
// h is positive; native map is diag(h)+origin, without rotation/shear.
// SDF is a symmetric narrow band in WORLD units with inactive signed band
// continuation. Fog uses native linear interior ramp min(1,-phi/(halfWidth*h))
// and zero exterior; exact zero surface values may remain active. These are
// native Double analytic approximations and ordinary output rounding, not
// exact-distance or mass-conservation certificates.
//
// For this authoring API, write.allowFloatMapRounding additionally opts into
// rounded native Float map caches. Without it both stored Float forward/inverse
// caches must represent the Double coefficients exactly. With it, the maximum
// stored-coefficient mapping discrepancy across the candidate domain must be
// <=1e-4 voxel and is reported. This excludes subsequent Float-coordinate or
// Float-arithmetic evaluation error; the published Double map stays exact.
// Float value rounding is measured on EVERY published stored value and the
// background, including inactive values/tiles. Nonzero conversion to zero,
// nonfinite output and unresolved scaled quantities reject before publication.
// Native name/class/map/statistics/CRC are authored; arbitrary OpenVDB metadata
// extensions, CUDA, quantized/index/point backends and general transforms are
// outside this API. Existing native raw/codec/checksum and coverage diagnostics
// remain applicable; CRC integrity is not authentication.
//
// Full leaf-aligned candidate cells/node tables, native builder/manager/buffer,
// threads/scratch, validation, encoded intermediates, codec, statistics, CRC and
// writer share preflight caps BEFORE native allocation. TBB uses one worker
// when configured; upstream std-thread fallback may parallelize. Worker stacks
// and provider internals are outside logical caps, not a process RSS sandbox.
// A private completed raw file feeds the existing checked native reader/writer;
// final publication is exclusive and Unicode-safe, with owned-partial cleanup.
// Existing files survive failures. Native I/O availability alone is required;
// ENABLE_OPENVDB is unnecessary. OFF fails before options or path access.
[[nodiscard]] NanoVdbPrimitiveResult WriteNanoVdbPrimitive(
    const std::filesystem::path& output,NanoVdbPrimitiveShape shape,
    const NanoVdbPrimitiveOptions& options={},
    const NanoVdbWriteOptions& write={},const NanoVdbLimits& limits={});
} // namespace container::geometry::vdb
