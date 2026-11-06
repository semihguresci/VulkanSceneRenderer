#pragma once

#include "Container/geometry/OpenVdbFluidProjection.h"

namespace container::geometry::vdb {

struct FluidViscosityOptions {
  double timeStep{1.0 / 24.0};     // Seconds, [1e-6,3600].
  double kinematicViscosity{0.01}; // World distance^2/sec, [0,1e6].
  double tolerance{
      1e-6}; // Stored diffusion/full pressure residual, [1e-12,1e-2].
  uint32_t maxIterations{
      1000}; // Per connected diffusion block and M04 component.
  uint64_t maxFluidCells{
      100'000}; // [1,1000000], additionally bounded by shared budgets.
  // Existing canonical state names, also used in the output. These are literal
  // display names, not descriptor selectors; duplicate matching names fail.
  std::string densityGridName{"density"}, temperatureGridName{"temperature"};
  std::string velocityGridName{"velocity"}, pressureGridName{"pressure"},
      domainGridName{"fluid"};
};

struct FluidViscosityResult {
  uint64_t fluidCellCount{0}, faceCount{0};
  uint32_t diffusionIterations{0}, projectionIterations{0};
  double diffusionResidual{0}, pressureResidual{0};
  double maxDivergenceBefore{0}, maxDivergenceAfter{0},
      divergenceRoundoffBound{0};
  // Discrete unit-density MAC energy = .5*h^5*sum(u_index_face^2). Values below
  // Double's diagnostic range can round to zero; field checks use scaled norms.
  double kineticEnergyBefore{0}, kineticEnergyAfterDiffusion{0},
      kineticEnergyAfterProjection{0};
  bool skipped{false}, roundoffLimited{false}, converged{false};
};

// Apply backward-Euler viscosity to a canonical five-grid M05 state, then M04
// pressure projection. Inputs are Double density/temperature/pressure, Double
// negative-face local MAC and Bool domain with one finite uniform orthogonal
// affine transform. Closed exterior normals must already be zero. Scalar and
// state bounds follow the smoke-step contract; pressure must be finite.
//
// Each component's fluid-fluid faces solve (I-r*L)u'=u, r=nu*dt/h^2. L is the
// nearest-neighbor face-lattice Laplacian, with ZERO Dirichlet values at every
// missing face-lattice node. This is an explicit stair-step no-slip numerical
// approximation, not a complete physical collider/free-slip boundary model.
// Face blocks are normalized independently, avoiding an absolute weak-flow
// cutoff. Nonzero coefficients or fields lost to numeric range are rejected.
// The stored normalized infinity residual and dissipative scaled norm are
// independently checked before publication. M04 retains its full pressure
// residual check and reported local stored-face divergence allowance.
//
// Density, temperature and domain fields/custom metadata are retained. Pressure
// and velocity structural tags are refreshed after projection. nu=0 validates
// and copies the selected five loaded grids exactly, without projection/solve.
// Shared encoded/topology/metadata/solver/writer budgets apply before expanded
// allocations. Owned temporary VDBs bridge the pressure API. Only a successful
// final state is written to a new output path; inputs/existing paths stay
// intact. OFF fails before I/O/options. Generation/triangle ProcessingOptions
// are ignored.
[[nodiscard]] FluidViscosityResult
WriteViscousFluidState(const std::filesystem::path &inputState,
                       const std::filesystem::path &output,
                       const FluidViscosityOptions &viscosity = {},
                       const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
