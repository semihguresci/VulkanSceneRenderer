#pragma once

#include "Container/geometry/OpenVdbPoints.h"

namespace container::geometry::vdb {

enum class PointSplatField { Density, Velocity, Radius };
struct PointSplatOptions {
  double massPerPoint{1.0}; // Constant positive mass, independent of pscale.
  std::string densityGridName{"point_density"}; // Companion for attribute modes.
};
struct PointSplatResult {
  ProcessingResult field;
  std::string densityGridName; // Empty for Density (field itself is density).
  uint64_t pointCount{0};
  double totalMass{0};
  double depositedMass{0}; // Independently integrated density * world voxel volume.
};

// Standard P/v/pscale PointDataGrid -> union-of-spheres Float SDF. pscale is a
// world-space sphere radius and must span [1.5,100] target voxels, matching the
// upstream particle cutoffs (no silently ignored points). The existing bounded
// particle rasterizer provides distances, topology and workspace checks.
[[nodiscard]] ProcessingResult WritePointsLevelSet(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointOptions& points = {});

// Collocated trilinear hat/CIC deposition onto eight target voxel centers.
// The kernel is fixed in target voxel coordinates and INDEPENDENT OF RADIUS;
// pscale controls the SDF above and is merely a sampled attribute here.
// Density = sum(massPerPoint * weight) / voxelSize^3, with mass/world-length^3
// units and a zero inactive background. Velocity/Radius are normalized weighted
// means in world velocity/world length, plus a companion density DoubleGrid.
// Density and radius use DoubleGrid; velocity uses a world-space Vec3DGrid.
// All positive kernel contributions are accounted, using compensated mass sums
// and stable convex means; positive products/volume scaling that underflow to
// zero, nonfinite results and mass-conservation failures reject publication.
// Empty input produces empty splat grids with zero mass; SDF requires points.
//
// Output uses origin-aligned processing.voxelSize in [1.5e-5,1e6]. halfWidth
// applies only to SDF; maxTriangles is ignored. Output names are bounded
// to 256 bytes; field and companion names must differ. All output grids share
// topology/memory budgets; unique contributing cells and nodes are preflighted
// before tree allocation. These logical estimates are not an RSS guarantee.
[[nodiscard]] PointSplatResult WritePointsSplat(
    const std::filesystem::path& input, const std::filesystem::path& output,
    PointSplatField field = PointSplatField::Density,
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointSplatOptions& splat = {}, const PointOptions& points = {});

// Both APIs read a private bounded copy of the input, so selected metadata and
// records come from the same immutable snapshot. Strict ReadPoints layout,
// finite/coordinate/precision and delayed-loading requirements apply, including
// explicit rejection of unsupported attributes/groups. Custom grid metadata is
// preserved; output structural fields and producer metadata are regenerated.
// Records are processed in ReadPoints deterministic order, including inactive
// point voxels. Source bytes remain untouched. Seekable output must be new;
// failures leave no output or owned temporary files. A one-worker arena covers
// processing. Generic attributes and frustum rasterization remain unsupported.
// OFF reports ENABLE_OPENVDB=ON before file/options access.
} // namespace container::geometry::vdb
