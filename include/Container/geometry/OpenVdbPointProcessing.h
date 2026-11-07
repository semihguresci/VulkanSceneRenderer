#pragma once

#include "Container/geometry/OpenVdbPoints.h"

namespace container::geometry::vdb {

struct ScatterOptions {
  uint64_t pointCount{1000};
  uint64_t seed{0};
  double radius{1.0};
};

enum class PointIntegration { RK2, RK4 };
enum class PointAttribute { Velocity, Radius };
struct PointAdvectionOptions {
  double timeStep{1.0 / 24.0}; // Signed seconds, in [-3600,3600].
  uint32_t steps{1}; // In [1,64].
  PointIntegration integration{PointIntegration::RK2};
  bool updateVelocity{true}; // Sample final v; false retains the existing v.
};

// Scatter exactly pointCount points from finite nonnegative Float/Double fog.
// Active voxels/tiles are weighted by their density and cell count; positions
// are uniform inside the selected cells and transformed into world space.
// Inactive values/background must be zero. Affine axes span [1e-6,1e6]. An
// explicit mt19937_64-to-53-bit mapping makes a seed reproducible across STL
// implementations. Extreme density ranges that lose normalized/CDF mass or fall
// below the generator's 53-bit probability resolution fail.
// Velocity is zero and pscale is radius. A zero requested count writes empty
// points; other requests need positive mass. Fog custom grid metadata is copied.
[[nodiscard]] PointResult ScatterPoints(
    const std::filesystem::path& inputFog, const std::filesystem::path& output,
    const std::string& inputGrid = {}, const ScatterOptions& scatter = {},
    const ProcessingOptions& processing = {}, const PointOptions& points = {});

// Move standard P/v/pscale points through a static world-space collocated
// Vec3S/Vec3D field. Midpoint RK2 or classical RK4 uses trilinear Double sampling
// and the velocity grid's independent affine transform. Signed time is allowed.
// Potential travel is limited to 256 source-point voxels total and 16 per step,
// measured using the smallest source-point voxel axis. Positions/attributes
// must remain finite, representable and within point/sampling coordinate caps.
// World and Jacobian-mapped index ULPs must fit 1e-4 of the smaller point/field
// voxel axis; recenter fields whose remote index origins lose this precision.
// Existing v and pscale are retained unless updateVelocity requests final v.
// Point grid metadata is copied. Unsupported attribute descriptor metadata or
// hidden flags and points in inactive voxels fail rather than being discarded;
// arbitrary attrs/groups remain
// outside the standard-layout API. Zero time/speed retain world records.
[[nodiscard]] PointResult WriteAdvectedPoints(
    const std::filesystem::path& inputPoints, const std::filesystem::path& velocity,
    const std::filesystem::path& output, const std::string& inputGrid = {},
    const std::string& velocityGrid = {}, const PointAdvectionOptions& advection = {},
    const ProcessingOptions& processing = {}, const PointOptions& points = {});

// Trilinearly sample an independent affine field into standard point attributes.
// Velocity accepts the same world-space Vec3S/Vec3D fields as advection; Radius
// accepts finite Float/Double scalar fields and requires positive sampled values.
// Double interpolation avoids Float32 arithmetic overflow. Position, the other
// attribute and custom grid metadata are retained. Descriptor metadata/hidden
// flags/inactive point selections have the same restrictions as advection.
[[nodiscard]] PointResult WriteSampledPoints(
    const std::filesystem::path& inputPoints, const std::filesystem::path& field,
    const std::filesystem::path& output, PointAttribute target = PointAttribute::Velocity,
    const std::string& inputGrid = {}, const std::string& fieldGrid = {},
    const ProcessingOptions& processing = {}, const PointOptions& points = {});

// Both operations repartition world records using WritePoints into the requested
// origin-aligned processing.voxelSize; source grid transforms are not retained.
// Shared budgets cover encoded inputs, retained metadata/fields/records, CDF or
// integration scratch, and generation. They are conservative estimates, not a
// hard process-RSS cap. Source files remain unchanged; outputs must be new.
// Disabled builds report ENABLE_OPENVDB=ON before reading any paths.
}  // namespace container::geometry::vdb
