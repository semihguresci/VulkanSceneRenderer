#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

namespace container::geometry::vdb {

struct FluidProjectionOptions {
  double timeStep{1.0 / 24.0};                 // Seconds, [1e-6,3600].
  std::array<double, 3> acceleration{0, 0, 0}; // World units/sec^2, components <=1e12.
  double tolerance{1e-6};          // Relative full-domain divergence/residual, [1e-12,1e-2].
  uint32_t maxIterations{1000};    // [1,10000].
  uint64_t maxFluidCells{100'000}; // [1,1000000], also limited by shared budgets.
  std::string velocityGridName{"velocity"};
  std::string pressureGridName{"pressure"};
  std::string domainGridName{"fluid"};
  bool writeDomain{true};
};

struct FluidProjectionResult {
  uint64_t fluidCellCount{0}, faceCount{0},
      componentCount{0};  // faceCount counts fluid-fluid scalar faces.
  uint32_t iterations{0}; // Maximum iterations used by any connected component.
  double maxDivergenceBefore{0}, maxDivergenceAfter{0}, residual{0};
  double divergenceRoundoffBound{0}; // Maximum local stored-face bound, in 1/sec.
  bool roundoffLimited{false}; // Some cell exceeded the relative-only target.
  bool converged{false};
  std::string velocityGridName, pressureGridName, domainGridName;
};

// Closed-domain, unit-density MAC projection with optional uniform world force.
// Domain cells are active true BoolGrid or active positive Float/Double voxels;
// background must be zero. Inactive values do not define fluid cells, but all
// stored scalar values must be finite/nonnegative. Empty domains fail.
// The cell/topology preflight conservatively counts all active source values,
// including active false/zero entries, before building the positive-cell mask.
// Domain transforms are finite uniform orthogonal affine, including rotation
// and reflection, with voxel size h in [1e-6,1e6] and adequate world precision.
//
// Velocity accepts collocated WORLD Vec3S/Vec3D fields, trilinearly sampled at
// domain faces, or GRID_STAGGERED index-space CONTRAVARIANT_RELATIVE vectors
// with exactly the domain transform. Stored inputs/background must be finite:
// collocated world components have magnitudes <=1e12 and native MAC components
// <=1e24. Projected output obeys the same MAC cap and can be projected again.
// Independently transformed collocated sources must resolve domain face
// positions to 1e-4 domain voxel in both world and source-index precision.
// The output is always Vec3D GRID_STAGGERED,
// isInWorldSpace=false, CONTRAVARIANT_RELATIVE: u_a(i) is the negative face
// i-0.5e_a velocity in VOXELS/SECOND; world velocity is Jacobian*u. This explicit
// frame supports rotated/reflected domains without mislabeling local MAC faces
// as world Cartesian components. All exterior-domain normal faces are zero.
// Acceleration is transformed by the inverse Jacobian and multiplied by dt.
//
// D(u)_i=sum_a[u_a(i+e_a)-u_a(i)], in 1/sec. The pressure solve uses the matching
// six-neighbor graph Laplacian with homogeneous Neumann walls. Each connected
// component pins one cell pressure to zero by symmetric row/column elimination;
// closed-flux compatibility and the FULL unpinned residual/divergence, including
// gauge cells, are checked. Correction u'=u-dt/h^2*(p_i-p_(i-e_a)); pressure is
// physical unit-density pressure. Normalization avoids absolute small-flow cutoffs.
// Stored MAC divergence is checked per cell against tolerance*componentDpeak
// plus 8 times the sum of ULP spacings of that cell's six stored face values.
// This representability allowance matters for order-one circulation whose
// initial divergence is only roundoff; it is local and never replaces weak-flow
// normalization or the unchanged full pressure-residual check. The result
// reports the maximum bound and whether any cell required this allowance.
//
// Outputs are MAC velocity, DoubleGrid pressure and optionally BoolGrid domain
// in one new seekable VDB. Velocity custom metadata comes from velocity input;
// pressure/domain custom metadata comes from domain input. Names use the fields
// above; ProcessingOptions generation/name/triangle settings are ignored. Shared
// voxel/byte budgets include both inputs, expanded faces/trees, metadata and
// serial solver/writer scratch, with summed retained output checks. Estimates
// are not a hard process-RSS sandbox. Nonconvergence/nonfinite/precision failures
// leave no output, existing paths are never overwritten, OFF fails before I/O.
// This performs force addition and projection, not velocity advection or a fluid
// simulation timestep.
[[nodiscard]] FluidProjectionResult
WriteProjectedVelocity(const std::filesystem::path &velocity, const std::filesystem::path &domain,
                       const std::filesystem::path &output,
                       const FluidProjectionOptions &projection = {},
                       const std::string &velocityGrid = {}, const std::string &domainGrid = {},
                       const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
