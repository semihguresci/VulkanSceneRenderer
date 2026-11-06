#pragma once

#include "Container/geometry/OpenVdbRayQueries.h"

namespace container::geometry::vdb {

enum class FogTransmissionPolicy { RejectUnderflow, AllowOpaqueUnderflow };
enum class FogTransmissionStatus { Represented, OpaqueUnderflow };

struct FogRayIntegralOptions {
  RayQueryLimits limits{};
  uint32_t maxCellSegmentsPerRay{100000}; // [1,1M], prospective cell visits.
  uint64_t maxCellSegments{1000000};      // [1,1M], aggregate; never truncate.
  double extinction{1}; // Finite >=0, inverse WORLD length / density unit.
  FogTransmissionPolicy transmissionPolicy{
      FogTransmissionPolicy::RejectUnderflow};
};

struct FogRayIntegral {
  std::array<double, 3> normalizedDirection{};
  double densityIntegral{}; // Integral rho(x(s)) ds, density * WORLD length.
  double opticalDepth{};
  double transmittance{1}, absorbedFraction{}; // exp(-tau), -expm1(-tau).
  uint64_t cellSegments{}; // Actual positive interior lattice-cell intervals.
  FogTransmissionStatus transmissionStatus{FogTransmissionStatus::Represented};
};

struct FogRayIntegralBatch {
  RayQuerySource source; // source.empty describes ACTIVITY, never vacuum.
  double backgroundDensity{};
  bool numericallyConstant{false};
  std::vector<FogRayIntegral> rays;
  uint64_t cellSegments{}, cellSegmentUpperBound{}, workItems{};
  std::vector<std::string> diagnostics;
};

// Read-only Float/Double FOG_VOLUME or UNKNOWN, recognized WORLD/invariant
// scalar metadata, finite nonnegative ALL stored values and background. Values
// are nodal: inactive values/background participate in trilinear interpolation,
// independent of activity. No normalization, clamp or density ceiling. Float
// values promote exactly before arithmetic. Positive BG-only/no-activity trees
// are constant fields, not vacuum. Source bits and native map/custom metadata
// remain unchanged.
//
// Uniform orthogonal affine h[1e-6,1e6], rotations/reflections included.
// Directions normalize; finite 0<=minimumDistance<maximumDistance are Euclidean
// WORLD lengths. Centered inverse mapping requires 1e-4 voxel coordinate
// precision; transformed ray endpoints must be within1e12. Nonzero Double
// normalization/map products lost to zero, unsafe Int32 interpolation extents
// and unresolved coordinates reject. No general affine/nonlinear map
// interpretation.
//
// Census every stored non-BG knot/tile and expand support by one index unit.
// Constant BG over the outer intervals is added directly, without subtracting
// large nearly equal density terms. The interior uses checked rational event
// ordering and fixed8-corner cell Simpson integration, polynomial-exact in real
// arithmetic. Checked fixed65536-bit integer arithmetic retains tiny
// independent complements and common product exponents. The returned Double
// result is approximate, not a correctly-rounded/error certificate. No epsilon
// holes, near-tie merging, activity pruning or positive sliver omission.
//
// Nonzero final I/tau outside Double range rejects, even if extinction could
// recover an unrepresentable I. Extinction0 does not skip I/source validation.
// Thin positive tau can round transmittance to1; absorbedFraction retains it.
// Finite tau whose exp underflows rejects unless AllowOpaqueUnderflow
// explicitly requests zero T plus OpaqueUnderflow status; overflowing tau still
// rejects. No scattering, emission, density transfer function or
// solid-occupancy claim.
//
// All scanned/file metadata, immutable encoded snapshots, actual full typed
// buffers before paging, full root/internal/leaf tables, plan/result
// capacities, fixed arithmetic/accessor workspace and prospective
// cell/primitive work share the supplied limits. Active weighted count and
// stored leaf slots independently obey processing.maxVoxelCount; inactive tiles
// are not expanded. A sparse huge gap can conservatively reject before
// traversal. Optional saved stats may be absent; present hints are validated.
// Empty ray batches still validate/census the source. One worker, mandatory
// delayed loading; upstream metadata/topology decoding can precede estimates,
// so these logical caps are not an RSS sandbox. Other generation fields in
// ProcessingOptions are ignored. All results or an error; no partial success.
// OFF availability precedes options/file access.
[[nodiscard]] FogRayIntegralBatch
IntegrateFogRays(const std::filesystem::path &input,
                 std::span<const WorldRay> rays,
                 const FogRayIntegralOptions &options = {},
                 const std::string &inputGrid = {},
                 const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
