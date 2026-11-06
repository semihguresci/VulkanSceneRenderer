#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <span>
#include <vector>

namespace container::geometry::vdb {

struct WorldRay {
  std::array<double, 3> origin{0, 0, 0}, direction{0, 0, 1};
  double minimumDistance{0}, maximumDistance{1000};
};
struct RayQueryLimits {
  uint32_t maxRays{10000}, maxGrids{256};
  uint64_t maxWorkItems{200'000'000}, maxMetadataBytes{1024 * 1024};
};
struct LevelSetRayOptions {
  RayQueryLimits limits{};
  double isovalue{0};
  uint32_t refinementSteps{
      4}; // [0,8] bounded one-dimensional chord refinements.
  bool computeNormals{true};
};
struct VolumeRayOptions {
  RayQueryLimits limits{};
  uint32_t maxSpans{
      100000}; // [1,1M], aggregate across the batch; never truncate.
};
enum class SdfRayHitKind { Miss, SampledZero, BracketedCrossing };
enum class RayNormalStatus {
  NotRequested,
  NoHit,
  Available,
  MissingActiveFootprint,
  DegenerateGradient
};
struct SdfRayHit {
  SdfRayHitKind kind{SdfRayHitKind::Miss};
  double distance{0}; // WORLD units; meaningful only for a hit.
  std::array<double, 3> position{}, normal{}, normalizedDirection{};
  RayNormalStatus normalStatus{RayNormalStatus::NoHit};
  double sampledResidual{0}; // phi(position)-isovalue, WORLD units; hit only.
  bool startSampleValid{false}, startedInside{false};
};
struct VolumeRaySpan {
  double entryDistance{0}, exitDistance{0};
  std::array<double, 3> entryPosition{}, exitPosition{};
};
struct VolumeRayResult {
  std::array<double, 3> normalizedDirection{};
  std::vector<VolumeRaySpan> spans;
};
struct RayQueryMetadata {
  std::string name, type, value;
  uint64_t nativeBytes{0};
  bool opaque{false};
};
struct RayQuerySource {
  std::string descriptorName, gridName, valueType;
  uint64_t inputActiveVoxelCount{0}, inputActiveTileCount{0},
      inputStoredLeafCount{0}, workItems{0};
  bool empty{false};
  // Homogeneous rows, column-vector INDEX -> WORLD mapping.
  std::array<double, 16> indexToWorld{};
  std::vector<RayQueryMetadata> metadata, fileMetadata;
};
struct LevelSetRayBatch {
  RayQuerySource source;
  std::vector<SdfRayHit> rays;
  std::vector<std::string> diagnostics;
};
struct VolumeRayBatch {
  RayQuerySource source;
  std::vector<VolumeRayResult> rays;
  uint64_t totalSpanCount{0};
  std::vector<std::string> diagnostics;
};

// Read-only native CPU queries. Finite WORLD directions are normalized; all
// distances are Euclidean WORLD lengths in [minimumDistance,maximumDistance].
// Finite nonzero directions and 0<=minimumDistance<maximumDistance are
// required. Uniform orthogonal affine spacing[1e-6,1e6], rotations/reflections
// included; original map coefficients are never rounded to Float. Unsafe Int32
// node extents, arithmetic/coordinate spacing above1e-4 voxel or meaningful
// Double range loss reject. General affine/nonlinear rays are not supported
// here.
//
// Float/Double positive-exterior world/invariant LEVEL_SET, finite stored
// values, symmetric inactive band[3,64] voxels, no active tiles. isovalue has
// WORLD units and must be >=2 voxels inside both band limits. Both native input
// types use root-recentered Double unit-distance work grids. Native
// LevelSetHDDA traversal and BoxStencil interpolation use a checked
// sign/scaled-chord search, not a cubic trilinear root solver. Miss is
// approximate: tangencies/multiple roots between native samples can be missed.
// SampledZero includes signed zero and zero plateaus, and is not a certificate
// of an isolated surface. Refinement is bounded and does not prove exact
// intersection. sampledResidual is diagnostic. Normal requires all8 active
// original corners and nondegenerate unit gradient; explicit status must be
// consulted. It points toward increasing phi in WORLD coordinates.
// startSampleValid similarly requires all8 corners at the requested minimum; a
// deep inactive negative interior is not silently a valid sample. True
// positive-background-only empty storage returns misses; hidden negative
// topology is not empty. Nonempty sources require a represented signed
// crossing.
//
// Encoded snapshots, aggregate scanned/file metadata, full typed buffers before
// paging, every stored internal/root slot, remote inactive topology,
// prospective working/native trees, complete result capacities and finite HDDA
// traversal bounds share processing.maxInputBytes/maxVoxelCount and
// maxWorkItems. Empty ray lists still charge the source census/validation.
// maxRays1..1M, maxGrids 1..4096, maxWorkItems1..10^10. Metadata summaries
// retain known native values; unknown payloads are explicitly opaque. One
// worker; mandatory delayed loading, immutable private Unicode-safe source
// snapshots. Eager metadata/legacy decoding can precede estimates: logical
// bounds are not a hostile-file/RSS sandbox. OFF reports availability before
// options or file access. Source bytes unchanged.
[[nodiscard]] LevelSetRayBatch QueryLevelSetRays(
    const std::filesystem::path &input, std::span<const WorldRay> rays,
    const LevelSetRayOptions &options = {}, const std::string &inputGrid = {},
    const ProcessingOptions &processing = {});

// Standard Float/Double/Bool/Mask/Int32/Int64/Vec3I/Vec3S/Vec3D activity ONLY.
// Active false/zero/NaN occupy space; inactive nonzero/negative values do not.
// Inactive-only leaves are pruned from an owned occupancy mask, then native
// VolumeRayIntersector returns coarse full8^3 active-leaf/active-tile spans.
// A single active cell in leaf0 can therefore yield[-.5,7.5], NOT[-.5,.5],
// along a crossing axis. A+.5 mask-ray adapter declares centered source-cell
// footprints; no dilation or interpolation kernel expansion is performed.
// Adjacent blocks merge. No density integration, exact active-voxel ray union
// or signed-interior claim. Native zero/grazing intervals<=1e-9 index-distance
// units are omitted explicitly; unsafe short query intervals reject. All spans
// are returned or an error is thrown; no partial/truncated success.
// Map/budget/source preservation/availability rules above apply; values are not
// arithmetically interpreted or required finite. PointData/unknown trees are
// unsupported.
[[nodiscard]] VolumeRayBatch QueryActiveVolumeRays(
    const std::filesystem::path &input, std::span<const WorldRay> rays,
    const VolumeRayOptions &options = {}, const std::string &inputGrid = {},
    const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
