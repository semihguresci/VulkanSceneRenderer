#pragma once
#include "Container/geometry/OpenVdbProcessing.h"
#include <vector>
namespace container::geometry::vdb {
enum class FieldAdvectionScheme { Semi, Midpoint, Rk3, Rk4, MacCormack, Bfecc };
enum class FieldAdvectionLimiter { None, Clamp, Revert };
enum class FieldVelocityLayout { Auto, CollocatedWorld, NativeMacIndex };
struct FieldAdvectionOptions {
  double timeStep{1.0/24.0}; // Signed WORLD seconds, [-3600,3600].
  uint32_t steps{1}; // 1..64 equal substeps of the total timeStep interval.
  FieldAdvectionScheme scheme{FieldAdvectionScheme::Midpoint};
  FieldAdvectionLimiter limiter{FieldAdvectionLimiter::Clamp}; // MAC/BFECC only.
  FieldVelocityLayout velocityLayout{FieldVelocityLayout::Auto};
  double maxCfl{4}; // Positive maximum source-voxel L2 travel/step, <=16.
  std::filesystem::path mask;
  std::string maskGrid;
  bool voxelizeActiveTiles{false};
  uint64_t maxWorkItems{200'000'000};
};
struct FieldAdvectionResult {
  ProcessingResult grid;
  uint64_t inputActiveVoxelCount{0}, updatedVoxelCount{0}, workItems{0};
  uint64_t overshootVoxelCount{0}, clampedVoxelCount{0}, revertedVoxelCount{0};
  uint32_t executedSteps{0};
  double maximumWorldSpeed{0}, maximumStepDisplacement{0};
  uint64_t roundedTraceComponentCount{0};
  double maximumTraceRounding{0}; // Source-index coordinate rounding envelope.
  std::array<double,3> inputMinimum{},inputMaximum{},outputMinimum{},outputMaximum{};
  std::array<double,3> maximumRoundTripCorrection{}; // Heuristic, not an error certificate.
  bool noOp{false}, maskUsed{false}, fogClassCleared{false};
  std::vector<std::string> diagnostics;
};
// Passive Float/Double scalar channels (UNKNOWN or FOG_VOLUME) and Vec3S/Vec3D
// collocated world channels (UNKNOWN, invariant or contravariant-relative).
// Scalar backgrounds/inactive values may be nonzero or signed. Every stored
// value/background must be finite and within native arithmetic headroom.
// Fog inputs require nonnegative values everywhere; unlimited correction that
// creates a negative output clears class to UNKNOWN and reports it explicitly.
// Uniform orthogonal source axes [1e-6,1e6] are required. Source file/grid
// metadata, vector frame and exact affine coefficients survive except generated
// statistics/name/provenance. Source files remain untouched. Active tiles need
// explicit voxelizeActiveTiles for nontrivial transport; exact no-ops retain
// the original tile representation.
//
// Collocated Vec3S/D velocity values must already be WORLD units/second,
// invariant or contravariant-relative, with finite component magnitudes <=1e12.
// They may use an independent nonsingular affine map (axes [1e-6,1e6]), subject
// to a conservative <=1e-4 velocity-voxel joint-map arithmetic uncertainty cap.
// NativeMacIndex requires GRID_STAGGERED, local contravariant-relative,
// container:mac_layout=negative_faces and container:velocity_units=
// voxels_per_second. Uniform orthogonal MAC axes are required. Components are
// reconstructed at index+(0.5,0,0)/(0,0.5,0)/(0,0,0.5), then converted via J*u
// to world velocities. Auto selects by the native class; forced layout/tag
// mismatches reject. Background and inactive velocity values participate in
// both sampling and the conservative speed bound; activity never clips flow.
//
// SDK trilinear field/velocity samplers are combined with bounded Double RK
// tracing in source index coordinates. This adapts VolumeAdvection's six
// schemes while avoiding its approximate-background pruning, Float position
// narrowing, map simplification and lexicographic vector limiters. Velocity
// fetches are promoted to Double before interpolation; detectable Double
// interpolation underflow rejects before world/index conversion. MacCormack
// and BFECC follow the native Euler forward/reverse correction formulation;
// Clamp/Revert work independently per component using the original departure
// cell's eight values. None may overshoot. Round-trip correction/overshoot
// diagnostics do not certify physical accuracy, convergence or conservation.
//
// Activity follows nonzero-weight interpolation footprints and nonzero
// correction contributions. Advected old active cells can become inactive;
// active zero/background values are retained without tolerance pruning. All
// source inactive values/activity outside affected support remain bit-exact.
// Mask accepts activity of nine native non-point types, ignoring payloads,
// signs/class (including active false/zero/NaN). It is a FINAL write selector:
// intermediate transport/correction passes keep their full halo; unselected
// original values/activity survive exactly. Mask coefficients must match the
// source map exactly. Zero time/speed, empty source activity or empty effective
// mask writes an exact value/activity/map copy with no integration.
//
// Total travel <=256 source voxels and per-step travel<=maxCfl. Full multi-pass
// and multi-step interpolation/trajectory halos, coordinate/ULP limits,
// aggregate input/output topology, native arrays, metadata, writer and work are
// reserved before transport. Conservative dense-domain caps can reject sparse
// remote layouts. Nonzero multiplication underflow and wholly unresolved
// constant motion reject; endpoint rounding (including local stagnation points
// in a nonconstant flow) is bounded to1e-4 source voxel and reported.
// One worker; seekable recorded-stat inputs with
// delayed loading enabled; native Unicode snapshots/exclusive full-precision
// ZIP publication through a usable narrow system temp path. Metadata decoding
// precedes caps; these estimates are not a hostile-file heap or hard RSS
// sandbox. Upstream metadata/writer map copies may reject extreme origin/step
// combinations before publication; successful output coefficients remain exact.
// Generation/triangle controls ignored; OFF precedes options/paths.
[[nodiscard]] FieldAdvectionResult WriteAdvectedField(
    const std::filesystem::path &input,const std::filesystem::path &velocity,
    const std::filesystem::path &output,const FieldAdvectionOptions &advection={},
    const std::string &inputGrid={},const std::string &velocityGrid={},
    const ProcessingOptions &processing={});
} // namespace container::geometry::vdb
