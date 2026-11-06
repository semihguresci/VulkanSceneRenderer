#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <vector>

namespace container::geometry::vdb {

struct PotentialFlowOptions {
  std::array<double, 3> farFieldVelocity{
      1, 0, 0}; // WORLD length/second, |component|<=1e12.
  std::array<double, 3> colliderVelocity{0, 0,
                                         0}; // Uniform WORLD collider velocity.
  uint32_t exteriorWidth{6}; // [1,128] native18-neighbor dilation steps.
  uint64_t maxSolverVoxels{100'000}; // [1,1M], also limited by processing caps.
  uint32_t maxIterations{1000}; // [1,10000], all solve attempts share this cap.
  double tolerance{
      1e-6}; // Stored-potential relative infinity residual [1e-12,1e-2].
  uint64_t maxWorkItems{8'000'000'000ull}; // Conservative [1,10^12] work cap.
  uint32_t maxGrids{256};                  // [1,4096], scanned descriptors.
  uint64_t maxMetadataBytes{1024 *
                            1024}; // Aggregate file/scanned grid metadata.
  std::string velocityGridName{"flow"}, potentialGridName{"potential"},
      domainGridName{"flow_domain"}, neumannGridName{"neumann"};
};
struct PotentialFlowResult {
  uint64_t domainCellCount{0}, componentCount{0}, neumannCellCount{0},
      workItems{0}, interiorDiagnosticCellCount{0};
  uint32_t iterations{0};
  // Interior maxima are available only when interiorDiagnosticCellCount>0.
  double residual{0}, maxBoundaryNormalError{0}, maxInteriorDivergence{0},
      maxInteriorCurl{0};
  bool converged{false};
  std::vector<ProcessingResult> grids;
  std::vector<std::string> diagnostics;
};

// Irrotational boundary-driven exterior flow using OpenVDB PotentialFlow tools,
// not pressure projection of an arbitrary velocity field or a fluid timestep.
// Float/Double positive-exterior LEVEL_SET, symmetric inactive band [3,64]
// source voxels, no active tiles, finite distances within the band and a
// resolved zero crossing. Uniform orthogonal affine h[1e-6,1e6],
// rotations/reflections included. Integer recentering and
// unit-distance/velocity Double conditioning precede native operators; exact
// original map coefficients survive publication. Nonzero range loss, unsafe
// coordinates and inadequate world precision fail. Collider-minus-far-field
// velocity is formed in WORLD units before conditioning. Nonzero relative
// motion requires the stored total velocity's ULP spacing to resolve that
// motion at the requested tolerance; otherwise reduce the common velocity.
// Exact co-motion does not need this relative-motion precision check.
//
// The FIXED domain is an18-neighbor dilation of the strict represented negative
// interior minus that interior. Width is a topological shell thickness, not an
// exact Euclidean world-distance width; require width>ceil(band)+1. At native
// Neumann boundary cells nVelocity=(n dot(Vcollider-WfarField))*n. Other
// missing faces use zero perturbation potential (finite outer Dirichlet
// boundary). Every domain component must have an actual Dirichlet face.
// Pure-Neumann closed cavities reject clearly; compatible gauges and spatial
// boundary-velocity grids remain separate follow-ups. Outer faces incorrectly
// classified Neumann and unresolved/zero-gradient solid normals also reject
// rather than changing BCs.
//
// Outputs: Vec3D WORLD flow, Double physical perturbation potential
// (length²/sec), Bool domain and Vec3D WORLD projected Neumann perturbation, in
// one new VDB. Flow outside/inactive domain is W; potential/Neumann background
// is zero. Native boundary flow substitutes the projected normal perturbation
// and drops its tangential potential derivative: this voxel approximation is
// explicit. The independently measured stored potential obeys the seven-point
// mixed-BC residual. Boundary normal error is checked. Central divergence/curl
// are only diagnostics on complete2-ring interior stencils; they do not
// establish MAC incompressibility or convergence to the infinite-domain
// analytic solution.
//
// Names must be distinct1..256-byte strings. Source custom grid/file metadata
// survives with regenerated structural tags/statistics. Only processing byte/
// voxel limits apply. One worker; immutable native-path snapshots, mandatory
// delayed loading, preflighted full domain/nodes/matrix/Krylov/metadata/output/
// work budgets (conservative estimates, not a hostile-file/RSS sandbox).
// Existing output is never overwritten; failure leaves no public output.
// OFF reports availability before options or I/O. Upstream temp paths must be
// usable; Unicode input/output parents are supported through owned snapshots.
[[nodiscard]] PotentialFlowResult
WritePotentialFlow(const std::filesystem::path &collider,
                   const std::filesystem::path &output,
                   const PotentialFlowOptions &options = {},
                   const std::string &colliderGrid = {},
                   const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
