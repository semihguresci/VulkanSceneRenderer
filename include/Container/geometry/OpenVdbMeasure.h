#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

namespace container::geometry::vdb {

struct SurfaceMeasurements {
  std::string gridName;
  uint64_t activeVoxelCount{0};
  double voxelSize{0};
  double area{0};                      // World length squared.
  double volume{0};                    // Enclosed world length cubed.
  double averageMeanCurvature{0};       // Inverse world length; outward sphere = 1/r.
  double totalGaussianCurvature{0};     // Dimensionless surface integral.
  double eulerEstimate{0};              // Gaussian integral / (2*pi), before rounding.
  int64_t estimatedEulerCharacteristic{0};
  double eulerRoundingError{0};
  std::string topologyDiagnostic;
};

// Numerical surface integrals of a Float/Double narrow-band level set using
// OpenVDB LevelSetMeasure. Requires finite symmetric inactive band values,
// uniform orthogonal affine voxels in [1e-6,1e6], half width [3,64], active
// voxels (no active tiles), and a resolved zero crossing. Source distances must
// be world-space signed distances. The file and its metadata remain unchanged.
// An empty selector measures the first supported level set in descriptor order;
// explicit ambiguous names fail and descriptor:<exact name> disambiguates.
//
// Integer recentering and Double voxel-unit integration condition remote index
// origins. Returned area/volume/curvature restore world units. These are
// resolution-dependent estimates, including Euler: rounding a curvature
// integral is not an exact topological test. A rounding error above 0.1 is
// reported in topologyDiagnostic; no genus is inferred for disconnected input.
//
// Only processing.maxVoxelCount and maxInputBytes apply. They bound encoded
// input, recorded/loaded trees and metadata, the full padded measurement box,
// and conservative reconstruction/integration workspace before allocation.
// This is a logical allocation estimate, not a hard process-RSS guarantee or
// a sandbox for hostile compressed files. All stages run in a one-worker arena.
// Disabled builds report ENABLE_OPENVDB=ON before accessing the input path.
[[nodiscard]] SurfaceMeasurements MeasureLevelSet(
    const std::filesystem::path& input, const std::string& inputGrid = {},
    const ProcessingOptions& processing = {});

} // namespace container::geometry::vdb
