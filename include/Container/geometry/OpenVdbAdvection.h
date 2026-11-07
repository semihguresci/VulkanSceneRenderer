#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

namespace container::geometry::vdb {

struct AdvectionOptions {
  double timeStep{1.0 / 24.0}; // Signed world-time seconds, in [-3600,3600].
  uint32_t steps{1}; // RK2/linear semi-Lagrangian passes, in [1,64].
};

// Write a sparse constant-background Vec3D velocity field. Values are world
// displacement per second, collocated and contravariant-relative. Each finite
// component must have magnitude <=1e12. The transform uses options.voxelSize
// in [1.5e-5,1e6], matching the upstream uniform-map construction limit.
[[nodiscard]] ProcessingResult WriteConstantVelocity(
    const std::filesystem::path& output, const std::array<double, 3>& velocity,
    const ProcessingOptions& options = {});

// Passive FloatGrid fog advection in a static Vec3S/Vec3D velocity field, using
// OpenVDB's midpoint backtracking and trilinear density/velocity interpolation.
// Grid voxel-axis sizes must be in [1e-6,1e6] world units. Fog needs uniform
// affine voxels, a zero background, inactive zero values and
// finite active density in [0,1e6]. Source transform and metadata are retained.
// Velocity may have an independent nonsingular affine sampling transform, but
// values must already be world-space vectors (invariant or contravariant-
// relative). Staggered, local/index-space, normal and position fields fail.
// Vec3S fields are promoted to Vec3D for world-position integration precision.
// Maximum displacement is 256 source voxels total and 16 voxels per pass;
// increase steps for long timesteps. Both zero time and zero speed are exact
// copies. Nontrivial transport normalizes density by its peak and restores its
// units afterward; upstream prunes magnitudes below about 1e-5 of that peak.
// Maps which the upstream algorithm cannot copy exactly require recentering
// before nontrivial transport; zero-time/zero-speed copies remain available.
// This is passive density transport, not a mass-conserving fluid solver.
// Shared budgets bound both inputs, conversion/copy workspace and prospective
// topology before each expansion. Output paths must not already exist.
[[nodiscard]] ProcessingResult WriteAdvectedFog(
    const std::filesystem::path& input, const std::filesystem::path& velocity,
    const std::filesystem::path& output, const AdvectionOptions& advection = {},
    const std::string& inputGrid = {}, const std::string& velocityGrid = {},
    const ProcessingOptions& options = {});

} // namespace container::geometry::vdb
