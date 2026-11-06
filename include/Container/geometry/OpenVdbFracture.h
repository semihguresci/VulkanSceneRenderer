#pragma once
#include "Container/geometry/OpenVdbProcessing.h"
#include <optional>
#include <vector>
namespace container::geometry::vdb {
struct FractureInstance {
  // World displacement vector, applied after world X then Y then Z rotations.
  std::array<double,3> translation{};
  std::array<double,3> rotation{}; // Radians in [-2*pi,2*pi].
  // Default is the exact source affine origin; world points are not rounded
  // through the SDK's Float instance-point/quaternion interface.
  std::optional<std::array<double,3>> pivot;
};
struct FractureOptions {
  std::vector<FractureInstance> instances; // Empty means one identity instance.
  bool cutterOverlap{true}; // Later instances also cut earlier fragments.
  bool segment{false}; // Split zero-surface components, subject to caveat below.
  uint32_t maxPieces{64}; // 1..256, summed residuals+fragments.
  uint64_t maxWorkItems{200'000'000};
};
struct FracturePiece {
  ProcessingResult grid;
  bool residual{false};
  uint32_t instance{0}; // Instance creating this fragment; residual uses zero.
  uint64_t negativeVoxelCount{0}; // Sampled interior volume, not exact geometry.
};
struct FractureResult {
  std::vector<FracturePiece> pieces;
  uint64_t inputNegativeVoxelCount{0}, outputNegativeVoxelCount{0}, workItems{0};
  uint64_t cutBoundaryVoxelCount{0}; // New cutter-zero samples, owned by neither open interior.
  std::vector<std::string> diagnostics;
};
// Bounded deterministic native CSG fracture orchestration for matching native
// Float/Double LEVEL_SET source/cutter, exact affine coefficients/backgrounds,
// uniform orthogonal voxels [1e-6,1e6], world/invariant scalar semantics,
// finite stored values inside a [3,64]-voxel background width, no active tiles.
// Inactive distances must equal signed native backgrounds; inactive sign jumps
// require an intervening active narrow band and otherwise reject.
// Positive bounding faces and no negative inactive cells outside active bounds
// are checked as finite-grid closure guards; these do not certify a manifold.
// Unlike LevelSetFracture's private <27-active-voxel heuristic, no represented
// negative fragment is silently discarded. Fully contained/disjoint cutters
// produce correct whole fragments/unchanged residuals. Partition of negative
// voxel samples plus new cutter-zero boundary samples is verified exactly
// after every cut and across final outputs (zero surfaces have open interiors).
// CSG and trilinear rigid resampling provide approximate distances; no distance
// reinitialization or exact continuous-volume conservation is claimed.
// Integer index translations use exact native nearest sampling; general rigid
// instances use native trilinear samples with per-point Double inverse mapping,
// avoiding Float instance parameters and affine step-size cancellation. Unit
// maps isolate numerical work; native world distances and exact source maps
// survive. Source file/grid metadata retained, cutter file metadata not merged.
// segment=false retains nested cavities. segment=true uses native surface
// segmentation; negative-exterior cavity surfaces reject before publication,
// because surface labels do not identify enclosed solid components.
// Segmentation clamps inactive component values to their signed backgrounds.
// Instance/piece/growth/tree/workspace caps are reserved before the first cut;
// conservative dense-domain/worst-piece estimates can reject sparse layouts.
// New exclusive full-precision ZIP output contains deterministic residual and
// fragment grids. Names sort by role, descending activity, index bounds and
// creation order. Inputs remain untouched. Requires seekable recorded-stat
// files and SDK delayed loading; native Unicode snapshots/publication through
// a usable narrow system temp path. Metadata decode precedes limits; these
// estimates are not a hostile-file heap sandbox or hard process-RSS contract.
// Upstream metadata/writer map copies can reject extremely distant-origin /
// small-step combinations before publication; exact output-map retention is
// guaranteed when the operation succeeds, not universal SDK IO acceptance.
// OFF precedes options/paths. Processing generation/triangle options ignored.
[[nodiscard]] FractureResult WriteFracturedLevelSet(
    const std::filesystem::path &input,const std::filesystem::path &cutter,
    const std::filesystem::path &output,const FractureOptions &fracture={},
    const std::string &inputGrid={},const std::string &cutterGrid={},
    const ProcessingOptions &processing={});
} // namespace container::geometry::vdb
