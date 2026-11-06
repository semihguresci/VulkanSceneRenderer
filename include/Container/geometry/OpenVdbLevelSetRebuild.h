#pragma once
#include "Container/geometry/OpenVdbProcessing.h"
#include <optional>
#include <vector>

namespace container::geometry::vdb {
struct LevelSetRebuildOptions {
  double isovalue{0};
  double exteriorWidth{3}, interiorWidth{3}; // TARGET voxel units, [1,64].
  // Public row-major COLUMN-vector matrix; translation at indices3,7,11.
  // Uniform orthogonal affine axes[1e-6,1e6], rotation/reflection permitted.
  // Omission preserves the exact source transform.
  std::optional<std::array<double, 16>> targetIndexToWorld;
  uint64_t maxWork{1'000'000'000};
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024 * 1024};
};
struct LevelSetRebuildResult {
  ProcessingResult grid;
  std::string sourceValueType;
  uint64_t inputActiveVoxelCount{0}, meshTriangleUpperBound{0},
      workUpperBound{0};
  double isovalue{0}, effectiveExteriorWidth{0}, effectiveInteriorWidth{0};
  std::array<int32_t, 3> sourceIndexOrigin{}, targetIndexOrigin{};
  std::vector<std::string> diagnostics;
};
// Native Float/Double closed-contour rebuild via VolumeToMesh -> MeshToVolume.
// Source scalar UNKNOWN/LEVEL_SET/FOG values need not already be distances.
// Background!=isovalue; interior is the opposite side of the contour from the
// background. Finite stored values, closed positive bounding faces and no
// opposite-side inactive regions outside the active box are required. These
// finite guards do not certify manifold topology. Open planes reject.
//
// Isovalue subtraction/value normalization, integer recentering and relative
// unit transforms avoid the SDK's Float isovalue/world-origin narrowing.
// Native Float mesh points still impose a checked1e-4 TARGET-voxel coordinate
// quantization bound; ordinary geometric/distance approximation remains.
// Float/Double output storage matches source native type, WORLD distance units,
// exact target coefficients and explicitly Float-rounded band widths. No claim
// of exact surface projection or distance accuracy beyond native rebuilding.
// Internal bubbles/overlapping interior edges may close/merge: upstream's
// original-grid bubble oracle is disabled. This is not topology preservation.
//
// Conservative dense source/output domains, meshes, multiple scratch trees,
// work, metadata and writer reserves precede native allocation. Small aligned
// maps support h=1e-6 through stock-map serialization; SDK may reject tiny
// rotated affine maps or conditioned target maps changed by native copying.
// Native Unicode immutable snapshots, mandatory delayed loading/full-buffer
// preflight, one worker, exact-map seekable ZIP/exclusive output and cleanup.
// Source/file custom metadata survives except regenerated native/provenance
// fields. Legacy eager metadata/encodings precede limits; no hard RSS sandbox.
// processing.maxVoxelCount/maxInputBytes/maxTriangles/gridName apply;
// generation voxelSize/halfWidth are ignored. OFF precedes every option/path
// access.
[[nodiscard]] LevelSetRebuildResult
WriteRebuiltLevelSet(const std::filesystem::path &input,
                     const std::filesystem::path &output,
                     const LevelSetRebuildOptions &rebuild = {},
                     const std::string &inputGrid = {},
                     const ProcessingOptions &processing = {});
} // namespace container::geometry::vdb
