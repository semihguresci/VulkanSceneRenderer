#pragma once
#include "Container/geometry/OpenVdbProcessing.h"
#include <vector>

namespace container::geometry::vdb {
enum class FastSweepingOperation {
  FogToSdf, ReinitializeSdf, DilateSdf, MaskSdf, TopologyToSdf
};
enum class FastSweepingSide { All, GreaterThanIsovalue, LessThanIsovalue };
enum class FastSweepingNeighbors { Face, FaceEdge, FaceEdgeVertex };
struct FastSweepingOptions {
  double isoValue{0};
  uint32_t iterations{1}; // 1..32; each performs eight directional sweeps.
  uint32_t dilation{1}; // 1..64, active-band growth, not interface offset.
  FastSweepingNeighbors neighbors{FastSweepingNeighbors::Face};
  FastSweepingSide side{FastSweepingSide::All};
  // MaskSdf extends into the UNION of source activity and mask activity.
  // Other distance conversions and field extension INTERSECT source activity
  // with mask activity before solving. Mask values/sign/class are ignored.
  std::filesystem::path mask;
  std::string maskGrid;
  bool voxelizeActiveTiles{false};
  uint32_t topologyHalfWidth{3}; // 3..16 source voxels; Float output.
  uint32_t topologyClosing{0}, topologyDilation{0}, topologySmoothing{0}; // 0..8
  uint64_t maxWorkItems{200'000'000};
};
struct FastSweepingResult {
  ProcessingResult grid;
  ProcessingResult extensionGrid; // Empty unless WriteFastExtendedField.
  uint64_t inputActiveVoxelCount{0}, boundaryVoxelCount{0}, sweepingVoxelCount{0};
  uint64_t workItems{0};
  std::vector<std::string> diagnostics;
};
// Finite native Float/Double fog or LEVEL_SET scalar input. TopologyToSdf
// additionally accepts Bool/Mask/Int32/Int64/Vec3I/Vec3S/Vec3D and ignores all
// values. Masks accept these nine types and use activity only (including active
// false/zero/NaN values).
// Uniform orthogonal affine maps, voxel size [1e-6,1e6], are required. Source
// affine coefficients and file/grid metadata are retained exactly; work uses a
// unit index map and distances return in world units. TopologyToSdf outputs
// Float; other operations retain scalar precision. Conversion solves only the
// specified active domain and does not claim a uniform-width closed band.
// Conversion backgrounds enclose every active distance plus one source voxel;
// inactive signs are retained even when the active domain has only one sign.
// DilateSdf/MaskSdf preserve existing active distances as Dirichlet seeds and
// require |active value| < positive background; other conversions reinitialize
// from interpolated iso crossings. SDF inputs reject active tiles. Fog and
// topology tiles require explicit voxelizeActiveTiles. Side applies only to
// DilateSdf; it selects positive/negative distance band growth.
// Empty domains, missing crossings and unreachable disconnected active
// components fail without an output. Iterations are fixed, not convergence
// certified; finite approximate upwind distances are reported.
// Scalar values are centered/normalized before native iso interpolation;
// distinctions below eight native tolerances/epsilons of that range reject
// rather than silently becoming exact-zero voxels. Extreme scalar/extension
// magnitudes outside safe native interpolation arithmetic also reject.
// Safe Int32 stencil/slice sums, dense bounding-domain/tree/work limits are reserved
// before SDK growth. Conservative limits can reject sparse remote components.
// One worker; shared encoded/tree/workspace/voxel/work limits; seekable inputs
// with recorded stats; new exclusive full-precision ZIP output and owned
// cleanup. SDK delayed loading must be enabled, allowing full resident-buffer
// preflight before eager materialization (OPENVDB_DISABLE_DELAYED_LOAD rejects).
// Unicode parents use native snapshots through the system temp path,
// whose narrow representation must be usable by OpenVDB. Metadata decoding
// precedes caps (not a hostile-file heap sandbox). OFF precedes options/paths.
[[nodiscard]] FastSweepingResult WriteFastSwept(
    const std::filesystem::path &input, const std::filesystem::path &output,
    FastSweepingOperation operation, const FastSweepingOptions &sweeping = {},
    const std::string &inputGrid = {}, const ProcessingOptions &processing = {});
// Extends an independently loaded scalar or collocated world vector field off
// the input iso surface, solving grad(field).grad(distance)=0. Float input
// accepts Float/Vec3S, Double accepts Double/Vec3D; native precision is retained.
// Field and optional mask affine coefficients must match source exactly.
// Boundary values use native trilinear interpolation in index coordinates;
// inactive values and the infinite field background participate. Finite values
// in ALL stored field cells/background are required; integer, MAC, local-space,
// SDF fields and normalized-vector semantics are rejected. All side solves the
// entire active domain. Greater/Less selects by original scalar >/< isoValue;
// the opposite side (including equality) retains original field values there,
// including boundary voxels. Output contains distance named processing.gridName
// and extension named processing.gridName + "_extension", with field metadata.
// Activity of both outputs is the selected scalar domain, not field activity;
// outside that domain the extension has its original field background.
[[nodiscard]] FastSweepingResult WriteFastExtendedField(
    const std::filesystem::path &input, const std::filesystem::path &field,
    const std::filesystem::path &output, bool inputIsFog = false,
    const FastSweepingOptions &sweeping = {}, const std::string &inputGrid = {},
    const std::string &fieldGrid = {}, const ProcessingOptions &processing = {});
} // namespace container::geometry::vdb
