#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <span>
#include <vector>

namespace container::geometry::vdb {
struct SurfaceQueryOptions {
  size_t maxQueries{10000}, maxGrids{256};
  uint64_t maxWork{1'000'000'000};
  uintmax_t maxMetadataBytes{1024 * 1024};
};
struct SurfaceQueryMetadata {
  std::string name, type, value;
  uint64_t nativeBytes{0};
  bool opaque{false};
};
struct SurfaceQueryResult {
  std::array<double, 3> query{}, closestPosition{}, normal{};
  double distance{
      0}; // Nonnegative approximate point-cloud distance, world units.
  bool querySdfValid{false}, geometryValid{false};
  double querySdf{
      0}; // World signed distance interpolation, ONLY if querySdfValid.
  double meanCurvature{0}, gaussianCurvature{0}; // ONLY if geometryValid.
};
struct SurfaceQueryBatch {
  std::string gridName, valueType, diagnostic;
  double voxelSize{0};
  std::vector<SurfaceQueryResult> queries;
  std::vector<SurfaceQueryMetadata> metadata, fileMetadata;
};

// Native OpenVDB ClosestSurfacePoint queries the nearest extracted SURFACE
// POINT CLOUD, not triangles or an exact implicit projection. Every returned
// closestPosition/distance is approximate, including off-band queries. Small
// grids can have poorer native accuracy; no adaptive resampling is performed.
//
// Query SDF uses trilinear interpolation only when all8 corners are active.
// Normal/curvature are evaluated AT THE APPROXIMATE CLOSEST POSITION, using
// trilinear interpolation of native19-point CurvatureStencil data at8 corners.
// geometryValid requires the entire active footprint and nonzero gradients;
// otherwise geometry fields are finite zero placeholders and MUST be ignored.
// Normal points toward increasing SDF. Mean curvature of an outward sphere is
// +1/r in inverse world units; Gaussian is +1/r² in inverse squared world
// units. These local estimates are neither global measures nor convergence
// proofs.
//
// Finite Float/Double LEVEL_SET, uniform orthogonal affine voxels [1e-6,1e6],
// symmetric inactive band halfwidth[3,64], no active tiles, resolved zero
// crossing. Integer recentering/unit-distance Double stencils preserve remote
// source origins. Native Float query/surface coordinates must retain1e-4 source
// voxels; large object extents or very distant queries reject actionably.
// Exact original maps and metadata are not modified. Metadata summaries retain
// native type/size and known values; unknown payloads are explicitly opaque.
//
// maxQueries1..1M; shared
// encoded/full-decoded/metadata/input/output/surface-index and worst-comparison
// work budgets precede native allocation/materialization. Mandatory delayed
// loading, immutable snapshots, one worker. Logical budgets are not a
// hostile-file/RSS sandbox for eager metadata/legacy encodings. Empty query
// spans return validated source metadata without building a point index. Only
// processing.maxVoxelCount/maxInputBytes apply. OFF precedes all I/O/options.
[[nodiscard]] SurfaceQueryBatch
QueryLevelSetSurface(const std::filesystem::path &input,
                     std::span<const std::array<double, 3>> worldPoints,
                     const SurfaceQueryOptions &options = {},
                     const std::string &inputGrid = {},
                     const ProcessingOptions &processing = {});
} // namespace container::geometry::vdb
