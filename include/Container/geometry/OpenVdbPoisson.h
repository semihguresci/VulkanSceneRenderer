#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

namespace container::geometry::vdb {

struct PoissonOptions {
  uint32_t maxIterations{200}; // In [1,10000].
  double tolerance{1e-6}; // Relative infinity-norm residual, in [1e-12,1e-2].
  uint64_t maxSolverVoxels{1'000'000}; // In [1,1'000'000], also limited by processing budgets.
};

struct PoissonResult {
  ProcessingResult grid;
  uint32_t iterations{0};
  double residual{0.0}; // Independently measured relative infinity-norm residual of the stored output.
  bool converged{false};
};

// Solve the discrete world-space equation Laplacian(u) = rhs on the active
// cells of a FloatGrid/DoubleGrid, with u=0 on exterior cells. Uses the nearest-
// neighbor seven-point stencil (upstream's staggered=true branch), not the
// collocated central-divergence/gradient stencil. This is a scalar PDE solve,
// not an incompressible velocity projection or a complete fluid simulation.
// Uniform orthogonal affine voxels in [1e-6,1e6] are required; rotations and
// reflections are supported. All stored values/background must be finite.
// Background/inactive values do not enter rhs. Active tiles are counted and
// preflighted before expansion. Empty/zero rhs produces an exact zero solution.
// The output retains source precision/transform/custom metadata, has unknown
// grid class and zero background/inactive values, and uses processing.gridName.
// Recorded count/memory hints and actual topology, matrix/preconditioner/vector
// workspace and metadata are checked before solving. Maximum unknown count is
// one million. A nonconverged/nonfinite/unrepresentable solve writes no output.
// The stored output must also meet tolerance after rounding to its scalar type.
// RHS ranges that underflow a nonzero value during normalization are rejected;
// reduce its dynamic range or solve disconnected regions separately.
// Outputs are seekable, exclusively created and never overwrite an existing
// path; disabled builds report availability before filesystem access.
[[nodiscard]] PoissonResult WritePoissonSolution(
    const std::filesystem::path& inputRhs, const std::filesystem::path& output,
    const std::string& inputGrid = {}, const PoissonOptions& poisson = {},
    const ProcessingOptions& processing = {});

} // namespace container::geometry::vdb
