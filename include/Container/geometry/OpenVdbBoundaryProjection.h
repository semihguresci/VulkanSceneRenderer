#pragma once
#include "Container/geometry/OpenVdbFluidProjection.h"
#include <vector>

namespace container::geometry::vdb {

struct BoundaryProjectionField {
  std::filesystem::path file;
  std::string grid;
};
enum class MissingFluidBoundary { ClosedWall, OpenFacePressure, AirCellPressure, FreeSurfacePressure };
enum class AirPressureLocation { NeighborCenter, LevelSetInterface };
struct BoundaryProjectionInputs {
  BoundaryProjectionField velocity, fluid;
  BoundaryProjectionField solid, open, air, liquidPhi, wallVelocity, pressure;
};
struct BoundaryProjectionOptions {
  FluidProjectionOptions projection;
  MissingFluidBoundary missing{MissingFluidBoundary::ClosedWall};
  AirPressureLocation airPressure{AirPressureLocation::NeighborCenter};
  double minimumInterfaceFraction{.01}; // [1e-6,1], explicit effective-interface clamp.
  std::array<double,3> wallVelocityWorld{0,0,0};
  double ambientPressure{0};
  bool writeBoundaryState{true}; // Requires projection.writeDomain.
  std::string faceKindGridName{"boundary_kind"}, faceFractionGridName{"boundary_fraction"};
  std::string facePressureGridName{"boundary_pressure"}, wallVelocityGridName{"boundary_wall_velocity"};
  uint64_t maxWorkItems{2'000'000'000}; // [1,10000000000].
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024*1024};
};
struct BoundaryProjectionReplay {
  std::filesystem::path state;
  std::string velocityGrid{"velocity"}, fluidGrid{"fluid"};
  std::string faceKindGrid{"boundary_kind"}, faceFractionGrid{"boundary_fraction"};
  std::string facePressureGrid{"boundary_pressure"}, wallVelocityGrid{"boundary_wall_velocity"};
};
struct BoundaryProjectionResult {
  FluidProjectionResult projection;
  uint64_t fluidFluidFaces{0}, wallFaces{0}, openFacePressureFaces{0};
  uint64_t airCellPressureFaces{0}, freeSurfacePressureFaces{0}, clampedInterfaceFaces{0};
  uint64_t dirichletComponents{0}, neumannComponents{0}, totalIterations{0};
  uint64_t workUpperBound{0}, roundoffLimitedCells{0};
  double minimumEffectiveFraction{1}, maximumFractionClamp{0};
  double maxClosedNormalFlux{0}, maxWallNormalError{0}; // Local voxel-volume/sec and voxels/sec.
  bool replayed{false}, boundaryStateWritten{false};
  std::vector<std::string> diagnostics;
};

// Fixed-domain unit-density MAC projection with explicit pressure boundaries
// and prescribed stair-step wall normal velocities. Required fluid and optional
// solid/open/air masks are active true Bool/Mask or active positive Float/Double;
// masks are disjoint, background zero, all stored scalar mask values finite and
// nonnegative. Inactive values do not select regions. Discrete maps and phi have
// the exact same finite uniform orthogonal affine transform, h[1e-6,1e6], with
// rotation/reflection. h is a positive norm; dt is positive seconds. Exact
// represented-Jacobian cofactors detect lost/spurious zero inverse components;
// such frames reject before force/sampling. Other Double inverse rounding remains
// approximate; no arbitrary real-inverse equivalence is certified.
//
// Explicit open cells impose pD at the outer FACE (theta=.5, weight2). Explicit
// air cells use neighbor CENTER (theta=1) or phi-zero interface as selected.
// Missing neighbors follow the explicit fallback. Free-surface faces require
// phiFluid<0, phiAir>=0 and theta=max(-phiF/(phiAir-phiF),minimumFraction).
// Clamping moves the effective interface AND pressure sampling location; it is
// reported. Cells/faces retain full h^3/h^2 volume/area: this is not cut-cell flow.
// Phi is a linearly located zero crossing, not a signed-distance certificate.
//
// Velocity/wall sources are WORLD Vec3S/Vec3D collocated fields, or same-map
// GRID_STAGGERED local CONTRAVARIANT_RELATIVE negative-face MAC. WORLD fields
// may use independently conditioned affine maps. Float values are promoted
// before interpolation; background/inactive values participate. Stored WORLD
// vectors/acceleration <=1e12, local MAC <=1e24; all values must be finite.
// All five recognized WORLD vector tags use raw XYZ physical velocity
// components. This sampling policy applies no tag-driven deformation,
// translation or normalization; the caller supplies velocity components.
// A wall field excludes a nonzero constant wall velocity; a pressure field
// excludes nonzero ambientPressure. Pressure scalar fields are WORLD invariant
// Float/Double, sampled at the declared effective pressure location.
//
// D(U)=sum(Uplus-Uminus), k=dt/h^2. FF correction is -k*(pR-pL); pressure faces
// use the SAME oriented difference/theta. Walls are set to J^-1 wallVelocity
// without body force/pressure correction. Positive matrix diagonal is FFdegree
// +sumDir(1/theta), off-diagonal -1, RHS=-D(U*)/k+sumDir(pD/theta).
// Pure-Neumann components require prescribed wall-flux compatibility and a
// symmetric lexicographic zero gauge, never hidden mean subtraction. Pressure
// components normalize separately, including shifted nonuniform pD forcing
// when initial divergence is zero. All native PCG attempts share the per-
// component maxIterations allowance and aggregate work cap; success plus full
// stored-pressure residual and stored-face divergence
// (including gauges) is required. Only divergence permits the reported local
// 8*sum(face ULPs) allowance. Corrections use stored physical Double pressure;
// an unrepresentable small gradient beneath huge ambient pressure rejects.
//
// Default output is seven grids: Double local MAC, Double physical pressure,
// Bool fluid, Vec3I face-kind, Vec3D theta/pD, Double local MAC wall velocity.
// The kind/fraction/pD grids are native INVARIANT raw packed XYZ face channels,
// with dedicated schema metadata, not geometric vectors. All incident face
// records are active, absent components zero. Kind0 absent,1FF,2wall,3open-face,
// 4air-center,5free-surface. Relevant custom metadata and exact maps survive;
// file metadata comes explicitly from the fluid source, other files unchanged.
// Generic derived-field operations must strip canonical-boundary provenance.
//
// Mandatory delayed/full typed-memory preflight, source root/node traversal,
// joint seven-grid/array/matrix/preconditioner/Krylov/writer/metadata/work caps,
// immutable Unicode snapshots, exclusive seekable publication and owned-partial
// cleanup apply. One worker; logical caps are not an RSS sandbox. All inputs
// and existing output bytes survive failures. OFF fails before paths/options.
// Existing closed M04/M05/viscosity APIs are unchanged. No surface/solid motion,
// tangential no-slip, variable density, surface tension, one-way outflow,
// periodic/Robin conditions or complete open-liquid timestep is implied.
[[nodiscard]] BoundaryProjectionResult WriteBoundaryProjectedVelocity(
    const BoundaryProjectionInputs& inputs,const std::filesystem::path& output,
    const BoundaryProjectionOptions& options={},const ProcessingOptions& processing={});

// Replays the validated canonical face table with the SAME loaded predictor as
// the corresponding direct-input call; old pressure is ignored/recomputed.
// Geometry/constant BC controls must retain defaults. dt/force/solve/output
// controls remain applicable. Nonuniform pD is a fresh pressure-driven impulse
// on EVERY call even when D=0; replay is not generally idempotent. Velocity
// idempotence only covers zero extra force and homogeneous/constant component
// pD with already compatible walls; prior pressure need not be retained.
[[nodiscard]] BoundaryProjectionResult WriteReprojectedBoundaryState(
    const BoundaryProjectionReplay& replay,const std::filesystem::path& output,
    const BoundaryProjectionOptions& options={},const ProcessingOptions& processing={});

} // namespace container::geometry::vdb
