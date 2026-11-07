#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <vector>

namespace container::geometry::vdb {

enum class SurfaceFieldOperation {
  MeanCurvature,
  GaussianCurvature,
  Normal,
  ClosestPoint
};
struct SurfaceFieldsOptions {
  // Otherwise Float input produces Float/Vec3S; Double produces Double/Vec3D.
  // Promotion is useful for absolute CPT positions at remote world origins.
  bool promoteToDouble{false};
  std::string validityGridName{"surface_valid"};
  uint64_t maxWorkItems{200'000'000}; // [1,10^10], shared across stages.
  uint32_t maxGrids{256};             // [1,4096], scanned descriptors.
  uint64_t maxMetadataBytes{1024 *
                            1024}; // Aggregate file/scanned grid metadata.
};
struct SurfaceFieldsResult {
  ProcessingResult field, validity;
  std::string valueType;
  uint64_t inputActiveVoxelCount{0}, validVoxelCount{0},
      missingFootprintVoxelCount{0}, degenerateGradientVoxelCount{0},
      gradientSampleCount{0}, workItems{0};
  // Available only when gradientSampleCount>0. This is a local SDF-quality
  // diagnostic, not a signed-distance, closure or convergence certificate.
  double maximumUnitGradientError{0};
  std::vector<std::string> diagnostics;
};

// Derives one field plus a true-valued Bool validity grid in one new VDB.
// Float/Double positive-exterior world/invariant LEVEL_SET; present semantic
// tags must have their recognized native types. Finite world-distance values
// inside a symmetric [3,64]-voxel band, signed inactive backgrounds, no active
// tiles. Uniform orthogonal affine h[1e-6,1e6], rotations/reflections included.
// Exact source maps/custom file+grid metadata survive regenerated structural
// tags. Empty positive-background-only input yields diagnosed empty fields;
// hidden negative inactive topology is not an empty surface. Nonempty input
// needs a represented crossing. Input files remain unchanged.
//
// Every output is evaluated AT ITS ORIGINAL SOURCE CELL, not at a projected
// surface point. Full active center+6-face footprints are required for normal
// and CPT, and center+6-face+12-edge footprints for curvature. Unit-gradient
// norm must exceed1e-7; other cells are inactive zero in both outputs and are
// counted separately. Background is zero, including invalid absolute CPT
// positions: only active validity-mask cells carry meaningful field values.
// Source inactivity/background is never silently treated as valid stencil data.
//
// Double unit-distance/index conditioning precedes native GridOperators and
// CurvatureStencil evaluation. H=Hunit/h, K=Kunit/h²; outward sphere values are
// +1/rho and +1/rho² at cell radius rho. Normal is normalized increasing-phi
// world gradient (covariant-normalize/world tag). CPT is the native local
// approximation x-phi*UNNORMALIZED gradient, not a Newton iteration, exact
// projection or nearest point-cloud/triangle query. CPT stores absolute WORLD
// positions (contravariant-absolute/world tag); scalar fields are UNKNOWN.
// Conditioning/restoration rejects nonzero Double range loss; Float storage
// overflow/nonzero underflow rejects. promoteToDouble can avoid
// Float losses. Absolute CPT position spacing must resolve1e-4 source voxel;
// promote/recenter when inadequate. Derivative fields do not depend on world
// translation and need no representable absolute position values.
//
// processing.gridName names the field; validityGridName must be distinct, both
// 1..256 bytes. Only processing name/byte/voxel settings apply. Source encoded
// snapshot, all scanned/file metadata, full typed buffers before paging,
// normalized/native/output/validity trees, conservative full boxes and writer
// storage share caps before expansion; work is charged before traversals.
// One worker, mandatory delayed loading, native Unicode-safe private snapshots
// and exclusive full-precision ZIP output. Metadata/legacy decoding can precede
// checks; these are logical workspace bounds, not a hostile-file/RSS sandbox.
// OFF reports availability before options/path access. Failure leaves no public
// output. Existing SurfaceQueries has a separate point-cloud query contract.
[[nodiscard]] SurfaceFieldsResult WriteSurfaceFields(
    const std::filesystem::path &input, const std::filesystem::path &output,
    SurfaceFieldOperation operation = SurfaceFieldOperation::MeanCurvature,
    const SurfaceFieldsOptions &options = {}, const std::string &inputGrid = {},
    const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
