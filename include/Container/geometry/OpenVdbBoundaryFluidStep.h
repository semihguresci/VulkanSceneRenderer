#pragma once
#include "Container/geometry/OpenVdbBoundaryProjection.h"
#include <optional>

namespace container::geometry::vdb {
// TransportStored preserves the finite stored initial MAC field, including its
// measured divergence, through conservative donor fluxes before the one normal
// pressure solve. RequireSolenoidal checks the stored-only relative/ULP gate at
// entry AND each accepted endpoint; it adds no cleanup projection.
enum class BoundaryFluidInitialVelocity { TransportStored, RequireSolenoidal };
struct BoundaryFluidInputs {
  BoundaryProjectionReplay boundary;
  BoundaryProjectionField density;
  std::optional<BoundaryProjectionField> temperature;
};
struct BoundaryFluidNames {
  std::string density{"density"}, temperature{"temperature"};
  std::string velocity{"velocity"}, pressure{"pressure"}, fluid{"fluid"};
  std::string faceKind{"boundary_kind"}, faceFraction{"boundary_fraction"};
  std::string facePressure{"boundary_pressure"},
      wallVelocity{"boundary_wall_velocity"};
};
struct BoundaryFluidStepOptions {
  BoundaryFluidInitialVelocity initialVelocity{BoundaryFluidInitialVelocity::TransportStored};
  double timeStep{1.0 / 24.0}; // Positive TOTAL seconds, [1e-6,3600].
  uint32_t substeps{1}, maxSubsteps{64}, maxStepAttempts{128};
  double maxCfl{.5}; // (0,.5]: donor outgoing and MAC L1/endpoint bounds.
  std::array<double, 3> gravity{0, 0,
                                0}; // WORLD length/sec^2, components <=1e12.
  double buoyancyDensity{0}, buoyancyTemperature{0}, ambientTemperature{293.15};
  std::array<double, 3> buoyancyDirection{0, 1, 0};
  double reservoirDensity{0};
  std::optional<double> reservoirTemperature; // Empty means ambientTemperature.
  double tolerance{1e-6};
  uint32_t maxIterations{1000};
  uint64_t maxFluidCells{100'000}, maxStoredLeafVoxels{32ull * 1024 * 1024};
  uint64_t maxNodes{1'000'000}, maxWork{2'000'000'000};
  uint32_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024 * 1024};
  uintmax_t maxInputBytes{512ull * 1024 * 1024},
      maxOutputBytes{512ull * 1024 * 1024};
  BoundaryFluidNames names;
};
struct BoundaryFluidAcceptedStep {
  double timeStep{}, transportCfl{}, endpointCfl{}, constantResponseDrift{};
  double maxDivergence{}, pressureResidual{};
  uint64_t projectionIterations{};
};
struct BoundaryFluidStepResult {
  uint64_t fluidCells{}, incidentFaces{}, pressureFaces{}, wallFaces{};
  uint32_t requestedSubsteps{}, actualSubsteps{}, attempts{},
      rejectedAttempts{};
  double simulatedTime{}, timePartitionError{}, cellVolume{};
  double maxTransportCfl{}, maxEndpointCfl{}, maxConstantResponseDrift{};
  double maxDivergenceBefore{}, maxDivergenceAfter{}, divergenceRoundoffBound{};
  // Speeds sample cell-centered averages of opposing MAC faces, not a
  // certified whole-field/face maximum. CFL uses separate face rates.
  double pressureResidual{}, maxWorldSpeedBefore{}, maxWorldSpeedAfter{};
  double densityMassBefore{}, densityMassAfter{}, densityOutwardTransfer{};
  double temperatureVolumeBefore{}, temperatureVolumeAfter{},
      temperatureOutwardTransfer{};
  double densityBalanceError{}, temperatureBalanceError{};
  double densityBalanceRoundoffBound{}, temperatureBalanceRoundoffBound{};
  // Rounded storage updates across scalar/advection/force passes; a momentum
  // face can count once in each pass, rather than counting distinct faces.
  uint64_t reservoirFaceApplications{}, outflowFaceApplications{},
      roundedScalarRows{}, roundedMomentumRows{};
  uint64_t totalProjectionIterations{}, workUpperBound{}, workspaceUpperBound{},
      nodeUpperBound{};
  // Same stored-only per-component relative plus six-face ULP criterion,
  // measured even when TransportStored permits actual initial compressibility.
  bool initialDivergenceWithinTolerance{};
  bool roundoffLimited{}, diagnosticUnderflow{}, temperatureInitialized{},
      converged{};
  std::vector<BoundaryFluidAcceptedStep> steps;
  std::vector<std::string> diagnostics;
};
// A fixed-geometry open-boundary smoke step, not moving/free-surface geometry.
// Required canonical six members retain matching M04 schema/IDs, exact maps and
// packed incident axes. Old pressure is ignored. Stationary walls require zero
// normal velocity. TransportStored (default) measures and transports the actual
// finite initial MAC values without an initial incompressibility gate, value
// erasure, normalization or extra projection. Its endpoint uses the attempted
// predictor/current component scale plus final six-face ULPs. RequireSolenoidal
// instead requires stored-only component scale plus ULPs at entry AND accepted
// endpoints, closing that strict restart gate. initialDivergenceWithinTolerance
// reports the stored-only entry criterion in either policy; arbitrary initial
// compressibility is not mislabeled roundoffLimited. Saved pressure theta is a
// ghost distance; all transport cells have full volume.
// Uniform orthogonal h[1e-6,1e6] supports rotations/reflections. D/T share the
// map, finite[0,1e6], zero inactive/outside/background values; density is
// Float/Double FOG, temperature ordinary scalar. Missing temperature
// initializes ambient.
//
// First-order donor scalar shared flux is conservative up to stored rounding.
// Pressure inflow uses constant reservoir, outflow the adjacent fluid scalar;
// walls have zero flux. Outgoing CFL ensures nonnegative coefficients. Their
// sum is1-dt*div(U), generally NOT1 for measured stored divergence. Constant
// response drift is reported; no hidden normalization, clamp or general maximum
// principle. MAC momentum uses declared upwind advective-form face differences,
// transverse incident-cell averages and constant continuation when no adjacent
// fluid path exists. Existing incident wall-normal neighbors retain prescribed
// zero. Direct local fluid adjacency prevents
// through-wall/disconnected sampling. This is not conservative momentum/energy
// or tangential no-slip transport.
//
// Transport, WORLD gravity/spatial buoyancy, then ONE M04 canonical replay per
// accepted actual dt. An endpoint-CFL failure discards the entire private trial
// and halves dt from its unchanged input; all attempt/solve work stays charged.
// Nonuniform pD adds a new pressure impulse each accepted dt, not an idempotent
// projection. Actual dt>=1e-6; step/attempt/progress/range failures publish
// nothing. Exact bounded dyadic row products retain tiny fluxes and
// cancellation until final Double rounding; nonzero final field
// underflow/overflow rejects. The native Double inverse remains approximate;
// detectable lost/spurious inverse coefficients reject before solving.
// The constant-response/balance diagnostics are not
// a continuum conservation proof.
//
// Output9: fresh canonical7 plus Double D/T, with9distinct names. File metadata
// comes from the boundary state; scalar grid metadata from each selected input.
// Scalar-file metadata is not merged. Scalars lose the five canonical keys;
// matching table membership is deliberately regenerated for the seven BC grids.
// All source bytes stay unchanged. Sources/sinks, viscosity, moving
// walls/domain, dynamic phi, cut cells, velocity inlets and boundary-aware
// sequences are not included; there are no hidden legacy addon settings.
//
// Native typed full paging, root/internal slots, bounded tile expansion,
// complete catalogs, exact scratch, all state/attempt buffers,
// solver/writer/native+encoded scratch and bounded result storage share caps.
// Each attempt/native solve must fit remaining work BEFORE execution; rejected
// trials are included. These are logical reserves, not a hostile-file RSS/CPU
// sandbox; eager metadata decoding precedes its checks. Delayed loading
// required; source/encoded/work caps apply even to failed attempts. Exclusive
// new Unicode output uses ASCII private SDK snapshots and exact-map writers.
// OFF precedes options/paths. No CUDA backend.
[[nodiscard]] BoundaryFluidStepResult
WriteBoundaryFluidStep(const BoundaryFluidInputs &,
                       const std::filesystem::path &output,
                       const BoundaryFluidStepOptions & = {});
} // namespace container::geometry::vdb
