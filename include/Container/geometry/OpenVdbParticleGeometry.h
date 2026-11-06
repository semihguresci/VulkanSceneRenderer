#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <optional>
#include <span>
#include <vector>

namespace container::geometry::vdb {

struct GeometryParticle {
  std::array<double, 3> position{}, velocity{};
  double radius{1}; // WORLD length; velocity is WORLD length/time.
};
enum class ParticleGeometryOutput { LevelSet, Mask, Both };
struct ParticleGeometryOptions {
  ParticleGeometryOutput output{ParticleGeometryOutput::LevelSet};
  double timeStep{1}, spacing{1}; // Signed time; spacing in [0.25,4].
  double minRadiusVoxels{1.5}, maxRadiusVoxels{100};
  // Empty: the reported midpoint origin defines the sampling lattice. Set an
  // explicit origin to control lattice phase across independently authored
  // files.
  std::optional<std::array<double, 3>> originWorld;
  std::string maskGridName{"particle_mask"};
  uint64_t maxParticles{1'000'000}, maxTrailInstances{1'000'000};
  double maxTrailLengthVoxels{1'000'000};
  uint64_t maxNodes{1'000'000}, maxWork{2'000'000'000};
  uintmax_t maxMetadataBytes{1024 * 1024}, maxOutputBytes{512ull * 1024 * 1024};
};
struct ParticleGeometryResult {
  std::optional<ProcessingResult> levelSet, mask;
  std::array<double, 3> originWorld{};
  uint64_t particleCount{}, sphereParticleCount{}, trailParticleCount{};
  uint64_t sphereInstances{}, instanceUpperBound{}, candidateVoxelUpperBound{};
  uint64_t storedLeafVoxelUpperBound{}, nodeUpperBound{};
  uint64_t workspaceUpperBound{}, workUpperBound{};
  uint64_t roundedDistanceValues{};
  uint64_t repairedNativeCenterValues{};
  uint64_t centerCandidateCount{}, centerRepairPairUpperBound{};
  double maximumDistanceAbsoluteError{}, maximumPositionErrorVoxels{};
  double effectiveExteriorBand{}, maximumTrailLengthWorld{};
  std::vector<std::string> diagnostics;
};

// Native ParticlesToLevelSet discrete tapered trails. Displacement=v*timeStep;
// sphere instances run OPPOSITE displacement, with adaptive step .5*spacing*R.
// The radius decreases from particle.radius toward minRadiusVoxels. The last
// sampled sphere need not reach the requested endpoint: this is neither a
// continuous capsule nor a time-integrated/mass-conserving field. Exactly zero
// displacement uses rasterizeSpheres, avoiding the SDK's division by zero.
// Nonzero displacement length/components lost by native arithmetic reject.
// All head radii must lie in [minRadiusVoxels,maxRadiusVoxels], with
// 1.5<=min<=max<=1024; ignored particles are errors, not silent omissions.
//
// SDF: unit/index-conditioned native Double union, then audited Float WORLD
// distances, GRID_LEVEL_SET and a symmetric [3,64]-voxel band. Float rounding
// is counted over stored native values/background; nonfinite/nonzero-to-zero
// conversion rejects. Mask: native MaskGrid activity at voxel CENTERS strictly
// inside the sampled spheres (distance^2<radius^2), including their interiors;
// this is not cell/sphere intersection, SDF-band occupancy or particle-center
// rebucketing. No source IDs/attributes are generated: the SDK's blind-value
// comparisons/pruning and mask overwrite ownership require a separate contract.
// The SDK saturates zero-squared-distance centers to -band even if radius<band.
// Such samples are repaired against the complete stamp union and counted, with
// bounded stamp staging/deduplication before exact center*stamp repair work is
// checked. Pair and correction counts are reported; repair precedes conversion.
//
// h=processing.voxelSize in [1e-6,1e6]. Exact stored diagonal h and the
// reported Double origin are restored without a simplifying map copy. WORLD
// coordinates, relative subtraction/division and native recurrence must retain
// 1e-4 voxel precision; unsafe/lost weak components reject. The output sampling
// lattice is defined by originWorld, not implicitly by WORLD zero.
//
// Before native allocation: a conservative instance bound precedes exact finite
// recurrence simulation; swept voxel/node bounds, partition arrays,
// Double/Float/ Mask trees, conversion and writer scratch share
// maxInputBytes/maxVoxelCount, maxNodes/maxWork and encoded/metadata limits.
// Max particles<UInt32MAX, instances
// <=16M, work<=1e10, nodes<=1M, byte caps<=16GiB. Logical limits are not an RSS
// sandbox. Processing maxTriangles is ignored; halfWidth/gridName apply to SDF.
// One worker; exclusive seekable Unicode output and owned-partial cleanup.
// Empty input emits empty requested grids. Span data is borrowed only during
// the call. OFF availability precedes all options, spans and paths.
[[nodiscard]] ParticleGeometryResult WriteParticleGeometry(
    std::span<const GeometryParticle>, const std::filesystem::path &output,
    const ParticleGeometryOptions & = {}, const ProcessingOptions & = {});

// Strict version1 JSON: {"version":1,"particles":[{"position":[x,y,z],
// "radius":r,"velocity":[vx,vy,vz]}]}. Velocity is optional, default zero.
// Unknown/duplicate keys, wrong lengths/types, nonfinite values, inexact
// integer to Double conversion and raw tokens>128 bytes reject. A bounded
// immutable native-path snapshot feeds SAX (no DOM);
// encoded/token/decoded-vector capacity remains reserved alongside native
// rasterization. Input bytes are unchanged.
[[nodiscard]] ParticleGeometryResult WriteParticleGeometryFile(
    const std::filesystem::path &input, const std::filesystem::path &output,
    const ParticleGeometryOptions & = {}, const ProcessingOptions & = {});

} // namespace container::geometry::vdb
