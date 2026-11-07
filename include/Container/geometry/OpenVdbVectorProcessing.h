#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

namespace container::geometry::vdb {

enum class DifferentialOperation { Gradient, Divergence, Curl, Laplacian, Magnitude, Normalize };

// Gradient/Laplacian accept Float/Double scalar grids; the other operations
// accept collocated, world-space Vec3S/Vec3D grids. Vector metadata describes
// behavior under later transform edits: all world-space vector types (including
// covariant gradients) are accepted, and stored components are used directly.
// Index-space and staggered vectors are rejected rather than reinterpreted.
// OpenVDB's centered second-order operators produce world-space derivatives
// under bounded nonsingular affine transforms, including rotation/shear.
// Output precision and transform/custom metadata are retained; class is unknown.
// Gradient/curl use OpenVDB's covariant vector tag. Scalar outputs are invariant.
//
// Operations compute on the source active topology. Active tiles are densified
// for derivatives only, after leaf-rounded topology/workspace preflight. Inactive
// source values participate in stencils; inactive output values become the
// operation's constant-background result. Magnitude/normalize keep active tiles.
// Normalize maps exactly zero to zero and preserves tiny nonzero directions.
// Source axes must span [1e-6,1e6] world units with finite affine coordinates,
// sufficient conditioning and stored component magnitudes <=1e12. Generation
// voxelSize/halfWidth do not resample input. Shared budgets bound encoded input,
// stored/expanded topology, metadata copies and conservative serial workspace;
// they are not a hard process-RSS cap. Output must be a new path and failures do
// not leave a partial output. Disabled builds fail before inspecting paths.
[[nodiscard]] ProcessingResult WriteDifferential(
    const std::filesystem::path& input, const std::filesystem::path& output,
    DifferentialOperation operation, const std::string& inputGrid = {},
    const ProcessingOptions& options = {});

// Trilinear world-space sampling, including inactive values and background.
// Uses the selected vector grid's own transform; no automatic vector metadata
// conversion or renormalization is applied. The same input restrictions/budgets
// apply; sampling coordinates must fit OpenVDB's integer stencil range.
// Source index coordinates must resolve increments of 1e-8 voxels in Double;
// remote index origins that lose finer fractions require recentering the grid.
// Generation/output options (including gridName) are ignored by this query.
[[nodiscard]] std::array<double, 3> SampleVector(
    const std::filesystem::path& input, const std::array<double, 3>& position,
    const std::string& inputGrid = {}, const ProcessingOptions& options = {});

} // namespace container::geometry::vdb
