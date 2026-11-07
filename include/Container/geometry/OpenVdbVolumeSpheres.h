#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <optional>
#include <vector>

namespace container::geometry::vdb {

struct VolumeSphere {
  std::array<double, 3> position{};
  double radius{0};
};
struct VolumeSpheresOptions {
  uint32_t minCount{0}, maxCount{50}, candidateCount{2048};
  bool overlapping{false};
  double minRadiusWorld{0}, maxRadiusWorld{1e6};
  // Defaults: zero for world-distance level sets, 0.5 for [0,1] fog.
  std::optional<double> isovalue;
  uint64_t maxWork{8'000'000'000};
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024 * 1024};
};
struct VolumeSphereMetadata {
  std::string name, type, value;
  uint64_t nativeBytes{0};
  bool opaque{false};
};
struct VolumeSpheresResult {
  std::string gridName, valueType, diagnostic;
  double voxelSize{0}, isovalue{0};
  uint64_t activeVoxelCount{0};
  std::vector<VolumeSphere> spheres;
  std::vector<VolumeSphereMetadata> metadata;
  std::vector<VolumeSphereMetadata> fileMetadata;
};

// Approximate native OpenVDB12 fillWithSpheres, with its fixed seed0, one
// worker and deterministic radius-descending/xyz ordering. Float/Double
// narrow-band level sets (world distances, symmetric band [3,64]) and [0,1]
// zero-background fog are supported. Fog is first rebuilt to a zero-isosurface
// SDF: native fog scattering uses all active voxels, not the requested isovalue
// interior. Maps must be uniform orthogonal affine, voxel size [1e-6,1e6].
//
// Integer recentering and unit-voxel fitting avoid Float world-origin loss.
// Native Float center/radius quantization is checked against 1e-4 source
// voxels; restored centers/radii are Double world units. Radius limits are
// applied AFTER fitting: shrink to maxRadiusWorld, discard below
// minRadiusWorld. This avoids native small-grid adaptive resampling changing
// radius-limit units. minCount is checked afterward; no optimal packing,
// coverage, or exact-inscribed-sphere guarantee is made. Ordinary upstream
// geometric approximation still applies.
//
// Candidate count1..100000, maxCount1..4096, minCount<=maxCount. Work estimates
// bound full padded/resampled domains and worst candidate/surface comparisons;
// topology/memory/metadata are checked before native allocations at each stage.
// These are conservative logical budgets, not hard peak-RSS/hostile-file
// limits. File plus scanned grid metadata share maxMetadataBytes. Mandatory
// delayed loading permits full leaf-buffer preflight before materialization;
// legacy eager metadata/encodings remain outside hard peak-RSS guarantees.
// Source bytes/metadata remain unchanged. Empty/no-crossing sources reject.
// Only processing.maxVoxelCount/maxInputBytes apply. OFF fails before
// any I/O.
[[nodiscard]] VolumeSpheresResult
FitVolumeSpheres(const std::filesystem::path &input,
                 const VolumeSpheresOptions &options = {},
                 const std::string &inputGrid = {},
                 const ProcessingOptions &processing = {});

// Exclusive UTF-8 JSON schemaVersion1: source {gridName,valueType,voxelSize,
// isovalue,activeVoxelCount,metadata[],fileMetadata[]}, diagnostic,
// spheres[{position:[x,y,z],
// radius}]. Known scalar/vector/String metadata is text with native type/size;
// unknown metadata is explicitly opaque (its native payload is not exported).
// Invalid UTF-8 metadata fails rather than silently replacing bytes. Bounded
// output, Unicode paths, and cleanup of this call's partial output are
// provided.
[[nodiscard]] VolumeSpheresResult ExportVolumeSpheres(
    const std::filesystem::path &input, const std::filesystem::path &output,
    const VolumeSpheresOptions &options = {}, const std::string &inputGrid = {},
    const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
