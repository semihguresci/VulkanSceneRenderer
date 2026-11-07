#pragma once

#include "Container/geometry/OpenVdbFluidProjection.h"
#include "Container/geometry/OpenVdbFluidSources.h"

namespace container::geometry::vdb {

struct FluidSimulationInputs {
  std::filesystem::path density, velocity, domain, temperature;
  std::string densityGrid, velocityGrid, domainGrid, temperatureGrid;
};

struct FluidSimulationOptions {
  double timeStep{1.0 / 24.0}; // Seconds, [1e-6,3600].
  uint32_t substeps{1};        // Requested equal intervals, [1,64].
  uint32_t maxSubsteps{64};    // Total actual adaptive steps, [substeps,64].
  double maxCfl{0.5}; // Maximum component travel in source voxels, (0,.5].
  std::array<double, 3> gravity{0, 0,
                                0}; // World units/sec^2, components <=1e12.
  double buoyancyDensity{0}, buoyancyTemperature{0}; // Nonnegative, <=1e6.
  double ambientTemperature{293.15};                 // Kelvin, [0,1e6].
  std::array<double, 3> buoyancyDirection{0, 1, 0};  // Unit world direction.
  double tolerance{1e-6};
  uint32_t maxIterations{1000};
  uint64_t maxFluidCells{100'000};
  std::string densityGridName{"density"}, temperatureGridName{"temperature"};
  std::string velocityGridName{"velocity"}, pressureGridName{"pressure"},
      domainGridName{"fluid"};
  double kinematicViscosity{0}; // World distance^2/sec, [0,1e6]; zero disables.
  std::vector<FluidSourceSphere> sources{}; // Sources-last split, <=64.
};

struct FluidSimulationResult {
  uint64_t fluidCellCount{0}, clippedTraceCount{0};
  uint32_t requestedSubsteps{0}, actualSubsteps{0}, maxProjectionIterations{0};
  double simulatedTime{0}, residual{0};
  double maxDivergenceBefore{0}, maxDivergenceAfter{0};
  double divergenceRoundoffBound{0};
  bool roundoffLimited{false}; // M04 local stored-face allowance was needed.
  // Reconstructed world speeds sampled at fluid cell centers.
  double maxWorldSpeedBefore{0}, maxWorldSpeedAfter{0};
  double densityMassBefore{0}, densityMassAfter{0};
  double temperatureIntegralBefore{0}, temperatureIntegralAfter{0};
  bool converged{false};
  uint32_t viscositySubsteps{0}, sourceSubsteps{0}, maxDiffusionIterations{0};
  double maxDiffusionResidual{0};
  // Sum of per-substep diffusion energy loss, excluding pressure projection.
  // Primitive energy diagnostics below Double range may round to zero.
  double viscosityEnergyDissipated{0};
  // Applications count cells at each substep, not unique cells across a step.
  uint64_t sourceAffectedCellApplications{0}, sourceChangedCellApplications{0};
  double sourceDensityMassChange{0}, sourceTemperatureIntegralChange{0};
};

// A bounded closed-domain smoke step: midpoint semi-Lagrangian MAC
// self-advection and scalar transport, world gravity/spatial buoyancy, then M04
// pressure projection at EVERY actual substep. Nonzero kinematicViscosity adds
// backward-Euler face-lattice viscosity immediately before that projection,
// using the actual dt and the standalone viscosity boundary/numeric contract.
// Scalar sources/sinks run LAST at each actual substep, after projection: their
// heat/density affects buoyancy on the NEXT actual substep. They do not change
// velocity or the just-completed CFL bound; the next bound is recomputed from
// updated fields. dt, ambient, names and shared caps come from this options
// struct, overriding no hidden nested defaults. Zero viscosity/empty sources
// retain the original transport/force/projection path and exact stored fields.
// Velocity is the negative-face
// local voxels/second MAC representation produced by WriteProjectedVelocity,
// with exactly the domain transform. Domain Bool/Float/Double cells follow M04
// rules. Density Float/Double fog and optional Kelvin temperature share that
// transform, contain finite [0,1e6] values, and have zero
// background/inactive/outside values. Omitted temperature initializes every
// fluid cell to ambientTemperature.
//
// Scalar interpolation renormalizes fluid-cell weights; MAC component sampling
// uses the upstream negative-face +.5 offset and renormalizes valid face
// weights. Exterior normal faces are zero. Voxel DDA clips backtraces at solid
// walls, including edge/corner crossings; stencil weights also require local
// fluid visibility. If an intersection cannot be represented just inside fluid,
// the trace retains its origin. This is a closed-wall numerical treatment,
// not a free-surface/collider solver. Scalar transport preserves
// constants and nonnegativity but is NOT mass conservative. The mass and
// temperature-volume integrals report drift; the latter is not physical thermal
// energy.
//
// Buoyancy acceleration is direction*(beta*(T-ambient)-alpha*density), averaged
// onto fluid-fluid faces and added to gravity. CFL bounds can subdivide the
// requested equal intervals; each actual dt must remain >=1e-6 and total steps
// cannot exceed maxSubsteps. Projection uses that actual dt, the solver fields
// above, and zero additional force, including M04's reported local stored-face
// roundoff allowance. All five output names must be distinct.
// Output is Double density/temperature/pressure, Double MAC and Bool domain in
// one new seekable VDB, so the same file can supply every restart input path.
// Equivalent restart tests must use identical actual dt sequences and compare
// loaded fields, not VDB file identifiers. Source custom metadata is retained
// by corresponding fields with structural/solver tags refreshed.
//
// Shared budgets include encoded sources, expanded state, transport scratch and
// the serial M04 solve. Owned temporary seekable VDBs bridge the pressure API;
// only a fully successful final state is published. Existing outputs and inputs
// remain unchanged. OFF fails before path/options access. Vorticity confinement
// and free surfaces are not included; WriteFluidSequence composes these steps.
// ProcessingOptions generation/name/triangle fields are ignored; the source
// transform and five explicit simulation output names define the state.
[[nodiscard]] FluidSimulationResult
WriteFluidStep(const FluidSimulationInputs &input,
               const std::filesystem::path &output,
               const FluidSimulationOptions &simulation = {},
               const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
