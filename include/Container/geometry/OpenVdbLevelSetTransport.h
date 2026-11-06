#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

namespace container::geometry::vdb {

struct LevelSetTransportOptions {
  // Advection: signed seconds in [-3600,3600]. Morph: nonnegative relaxation
  // pseudo-time in [0,8]; it is not a linear interpolation fraction.
  double timeStep{1.0 / 24.0};
  uint32_t normalizationIterations{3}; // [1,8] narrow-band tracker iterations.
  uint32_t maxCflSteps{512};           // [1,4096], abort rather than publish a partial time.
  uint64_t maxWork{500'000'000};       // [1,1e10], charged leaf-slot passes and samples.
};

struct LevelSetTransportResult {
  ProcessingResult levelSet;
  uint32_t cflSteps{};
  uint64_t work{};
};

// Upstream OpenVDB LevelSetAdvection with HJWENO5 biased derivatives + TVD_RK2,
// and the same fixed schemes in LevelSetTracker. Other schemes, time-varying
// velocity, staggered/MAC velocity, masks and multi-frame output are unsupported.
// Source must be a positive-exterior FloatGrid level set with stored distances
// inside its preserved symmetric band (half-width [3,64] voxels), no active
// tiles, and uniform orthogonal affine voxels in [1e-6,1e6] world units.
// Rotation/reflection/translation are retained; shear/nonuniform voxels fail.
// Velocity selects collocated world-space Vec3S/Vec3D, invariant or
// contravariant-relative, finite components <=1e12 world units/second. Its own
// nonsingular affine sampling transform may differ. Inactive/background vectors
// participate. Total maximum displacement is limited to 256 source voxels.
//
// Math runs on a recentered unit-voxel transform with distances divided by the
// source voxel size, then restores the original transform/world distances.
// Static velocity is sampled in Double and transformed to source index axes.
// World/index precision and Float tracker progress are checked explicitly;
// unresolved nonzero transport rejects instead of silently returning a copy.
// Modeled total movement must be at least eight Float32 ULPs of the band in
// source-voxel units; this prevents time rounding away before reinitialization.
// PDE time and progress use upstream Float32 arithmetic. The requested Double
// duration can round, and an unresolved final remainder can be omitted even
// after positive CFL progress; cflSteps>0 does not prove the exact Double time
// interval was integrated. CFL/work-cap failures still publish no result.
[[nodiscard]] LevelSetTransportResult
WriteAdvectedLevelSet(const std::filesystem::path &input, const std::filesystem::path &velocity,
                      const std::filesystem::path &output,
                      const LevelSetTransportOptions &transport = {},
                      const std::string &inputGrid = {}, const std::string &velocityGrid = {},
                      const ProcessingOptions &processing = {});

// OpenVDB LevelSetMorphing + tracker with the same fixed HJWENO5/TVD_RK2
// schemes. Target has the same Float level-set contract, transform and band.
// Its clamped signed-distance difference drives normal motion toward its zero
// surface: pseudo-time is a relaxation duration, not exact shape interpolation.
// Only nonnegative time is accepted: the installed upstream implementation
// does not perform reverse morphing despite its general documentation wording.
// Conservative maximum normal travel is 2*bandHalfWidth*timeStep <=256 voxels.
// Nonzero morphing requires nonempty interfaces. This front evolution cannot
// seed disconnected target components where no source front supplies motion.
[[nodiscard]] LevelSetTransportResult
WriteMorphedLevelSet(const std::filesystem::path &input, const std::filesystem::path &target,
                     const std::filesystem::path &output,
                     const LevelSetTransportOptions &transport = {},
                     const std::string &inputGrid = {}, const std::string &targetGrid = {},
                     const ProcessingOptions &processing = {});

// Both operations: zero time writes an unchanged field (with chosen output
// name); source custom metadata is retained and producer fields regenerated.
// Generation voxelSize/halfWidth/maxTriangles are ignored. Positive voxel/byte
// budgets and a 1..256-byte output name are required. Encoded inputs, stored
// statistics, metadata, both trees, leaf-aligned prospective topology and
// serial tracker/auxiliary workspace are bounded before growth. maxWork counts
// conservative leaf-slot passes/samples, not machine instructions or wall time.
// One-worker execution, deterministic ordering, seekable exclusive new output;
// failures publish no partial field and never overwrite either input.
// OFF reports ENABLE_OPENVDB=ON before reading files or validating options.
} // namespace container::geometry::vdb
