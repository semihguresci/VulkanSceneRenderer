#pragma once

#include "Container/geometry/OpenVdbPointMotion.h"

namespace container::geometry::vdb {

enum class NativePointIntegration { RK2, RK4 };
enum class NativePointVelocityLayout { Auto, World, NativeMacIndex };

struct NativePointAdvectionOptions {
  double timeStep{1.0 / 24.0}; // Signed total WORLD seconds, [-3600,3600].
  uint32_t steps{1};          // 1..64 equal substeps of that total interval.
  NativePointIntegration integration{NativePointIntegration::RK2};
  NativePointVelocityLayout velocityLayout{NativePointVelocityLayout::Auto};
  PointGroupSelection groups;
  std::optional<std::array<double, 16>> targetIndexToWorld;
  double maxPositionErrorVoxels{1e-4}; // Target P codec/index error, (0,.25].
  bool rejectActivityMerges{true};
  // L2 travel in the VELOCITY grid's index coordinate frame, not point-grid
  // voxels. Both bounds include the background/inactive velocity envelope.
  double maxStepTravelVelocityVoxels{16}; // Positive, <=16.
  double maxTravelVelocityVoxels{256};   // Positive, <=256 over all substeps.
  uint64_t maxWork{200'000'000};
  size_t maxGrids{256}; // Aggregate point/velocity catalog entries, <=4096.
  uintmax_t maxMetadataBytes{1024 * 1024}; // Aggregate input/output metadata.
};

struct NativePointAdvectionResult {
  PointMotionResult motion;
  std::string velocityGridName, velocityValueType, velocitySemantics;
  uint64_t velocitySampleCount{0};
  uint32_t executedSteps{0};
  bool integrationSkipped{true}; // Independent of motion.noOp/target changes.
  double maximumWorldSpeedBound{0};
  double maximumStepTravelVelocityVoxels{0};
  double maximumTravelVelocityVoxels{0};
  double maximumWorldTravel{0};
  double maximumEndpointArithmeticEnvelopeWorld{0}; // Double endpoint guard.
  std::vector<std::string> diagnostics;
};

// Integrates selected native points through a STATIC Vec3S/Vec3D velocity grid,
// using midpoint RK2 or classical RK4 and Double trilinear arithmetic. Samples
// use decoded P plus its native voxel coordinate, in world space. Background
// and inactive field values participate; velocity activity never clips flow.
//
// UNKNOWN, already-WORLD vectors accept all five recognized vector tags. Their
// stored XYZ components are physical world distance/second; covariant-normalize
// is NOT renormalized and absolute vectors are NOT translated. NativeMacIndex
// instead requires GRID_STAGGERED, local contravariant-relative,
// container:mac_layout=negative_faces and container:velocity_units=
// voxels_per_second. Each component is sampled at index+0.5 on its own axis and
// the uniform orthogonal map Jacobian converts it to world velocity. Auto uses
// class to choose the layout; explicit layout/tag mismatches reject.
//
// Source points and collocated velocity may have independent nonsingular affine
// maps with axis lengths [1e-6,1e6]. The optional target uses PointMotion's COLUMN
// matrix convention and uniform orthogonal axes. Conditioned coordinate,
// interpolation, displacement and intermediate RK arithmetic reject nonfinite,
// detectable nonzero-to-zero underflow and unresolved world/index precision.
// Reported codec error bounds FINAL native P encoding, not integration error.
// RK convergence, incompressibility and conservation are not certified.
//
// All stored points, including inactive-voxel records, survive. Groups select
// motion rather than deletion. Trajectories are integrated before ONE final
// native PointMove repartition. Every original reference and all registered
// fixed-stride non-P storage bytes/codecs/groups/hidden flags are verified;
// v, pscale, IDs, radii and orientations are never transformed implicitly.
// Native output order may change. Dynamic layouts require an exact no-op.
// Mixed destination activity rejects unless the explicit OR policy is chosen.
// Empty archives, zero time, zero flow and empty selections obey PointMotion's
// target-map/codec/no-op rules; zero time does not bypass input validation.
//
// Point file/grid/descriptor metadata survives except output name and generated
// counts/writer statistics; velocity metadata is budgeted, not merged. Both
// Unicode-safe immutable snapshots, complete decoded arrays/trees, stored-node
// traversal, trajectories, native topology/payload moves and writer share the
// byte/voxel/work/metadata caps. Delayed loading is mandatory. Logical estimates
// are not a hostile-file allocator or RSS bound. One worker, exclusive seekable
// full-precision ZIP output, source files unchanged, no owned partial outputs.
// Generation voxel/band/triangle controls are ignored. OFF precedes all option
// and path access. Time-varying flow, constraints and replication are separate.
[[nodiscard]] NativePointAdvectionResult WriteAdvectedNativePoints(
    const std::filesystem::path& points, const std::filesystem::path& velocity,
    const std::filesystem::path& output,
    const NativePointAdvectionOptions& options = {},
    const std::string& pointGrid = {}, const std::string& velocityGrid = {},
    const ProcessingOptions& processing = {},
    const PointAttributeOptions& attributeLimits = {});

} // namespace container::geometry::vdb
