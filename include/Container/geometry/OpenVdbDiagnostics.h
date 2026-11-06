#pragma once

#include "Container/geometry/OpenVdbPointSelection.h"
#include <optional>

namespace container::geometry::vdb {

enum class DiagnosticProfile {
  StoredValues,
  SymmetricLevelSet,
  NormalizedFog,
  PointData
};
enum class DiagnosticScope { Active, Stored };
enum class DiagnosticGradient { None, FirstOrder, WenoInterface };
enum class DiagnosticFootprint { RequireActive, NativeStoredContinuation };
enum class DiagnosticStatus { Pass, Fail, Unavailable, NotApplicable };

struct DiagnosticsOptions {
  DiagnosticProfile profile{DiagnosticProfile::StoredValues};
  std::optional<std::array<double, 2>> componentRange;
  DiagnosticScope rangeScope{DiagnosticScope::Active};
  DiagnosticGradient gradient{DiagnosticGradient::None};
  std::array<double, 2> gradientNormRange{.5, 1.5};
  DiagnosticFootprint footprint{DiagnosticFootprint::RequireActive};
  // Absolute world-value tolerance for SDF inactive magnitude / fog zero.
  double magnitudeTolerance{0};
  // Point profile checks P/layout always, plus these selected numeric columns.
  // Empty means P/layout only; unrelated opaque attributes are not corruption.
  std::vector<std::string> pointAttributes;
  PointGroupSelection pointGroups;
  std::string maskGridName{"diagnostic_failures"};
  uint64_t maxWork{1'000'000'000}; // [1,10^10].
  size_t maxGrids{256}, maxMetadataBytes{1024 * 1024};
  size_t maxExamples{8}; // [0,128] per check.
};

struct DiagnosticExample {
  std::optional<std::array<int32_t, 3>> index;
  std::optional<uint64_t> pointOrdinal;
  std::string detail;
};
struct DiagnosticCheck {
  std::string id, description;
  DiagnosticStatus status{DiagnosticStatus::Pass};
  bool coverageComplete{true};
  uint64_t examinedLeafVoxels{0}, examinedTiles{0}, examinedPoints{0};
  uint64_t failedLeafVoxels{0}, failedTiles{0}, failedTileVoxelVolume{0},
      failedPoints{0};
  bool backgroundInspected{false}, backgroundFailed{false};
  uint64_t candidateCount{0}, evaluatedCount{0}, missingFootprintCount{0},
      nonfiniteFootprintCount{0}, inactiveContinuationCount{0};
  std::vector<DiagnosticExample> examples;
};
struct DiagnosticPointCounts {
  uint64_t total{0}, selected{0}, selectedActive{0}, selectedInactive{0};
  bool archivedSchema{false};
  bool countsValid{true}; // Counts include only safely validated leaves.
  std::vector<std::string> checkedAttributes;
};
struct DiagnosticReport {
  std::string descriptorName, gridName, valueType, gridClass, vectorType;
  bool inWorldSpace{true}, affine{false};
  // Homogeneous row-major COLUMN-vector affine matrix; absent for nonlinear
  // maps.
  std::optional<std::array<double, 16>> indexToWorld;
  uint64_t leafCount{0}, storedLeafSlots{0}, activeVoxelCount{0},
      activeTileCount{0}, inactiveTileCount{0}, decodedBytes{0},
      workUpperBound{0};
  bool allRequestedPassed{false}, complete{false}, unlocatedFailures{false};
  std::vector<DiagnosticCheck> checks;
  std::optional<DiagnosticPointCounts> points;
  std::optional<ProcessingResult> mask;
  // Unique finite failure locations, including report-only calls; background
  // and global failures have no finite mask location.
  uint64_t maskVoxelCount{0};
  std::vector<std::string> diagnostics;
};

// Read-only bounded checks of one selected registered grid. Supports nine
// native field types (Float/Double/Bool/Mask/Int32/Int64/Vec3I/S/D) and
// PointDataGrid. Diagnostics are returned independently: invalid values are
// Fail, unsafe or unavailable dependent checks are Unavailable, and no implicit
// repair occurs. allRequestedPassed is false for Fail/Unavailable; complete
// excludes requested Unavailable checks. NotApplicable describes unrequested
// checks only.
//
// Finite checks cover stored active/inactive values and background. Component
// ranges are inclusive/raw/componentwise; exact integer comparisons never round
// Int64 through Double. NormalizedFog means native density[0,1], not all
// physical fog. SymmetricLevelSet checks class/world-invariant tags, uniform
// orthogonal affine map, positive >=3-voxel background, no active tiles, active
// band range and signed inactive magnitude with the explicit absolute
// tolerance.
//
// Optional gradients use native FIRST_BIAS/Godunov (7 axial samples) or native
// WENO/Godunov at native six-face zero-crossing centers (19 axial +/-1/2/3
// samples). Public norm bounds are UNSQUARED. Double unit-distance conditioning
// with axes[1e-6,1e6] preserves rotation/reflection; unsupported maps/range or
// missing/nonfinite footprints are Unavailable, never silently Pass. Explicit
// stored continuation may use finite inactive band/background samples and is
// counted. No gradient choice certifies signed distance or surface closure.
// RequireActive also governs the six-face WENO crossing classification; finite
// inactive/background values cannot establish complete active coverage.
//
// Point layout checks precede counts/handles; selected records include inactive
// voxels. P is a local registered Vec3f offset, not a world-position
// certificate. Chosen numeric runtime codecs/stride1 are checked; unrelated
// hidden/opaque columns are retained in the input. Unsafe point layouts disable
// dependent decoding. SDK unreadable/corrupt file errors may throw before a
// report exists.
// Registered native str/str_trnc dictionary indices are checked over their full
// stored flat arrays (including hidden/strided values); no StringHandle is
// allowed to dereference an unchecked dictionary entry. Other opaque columns
// are not decoded merely because they are present.
//
// Processing byte cap covers immutable native-path snapshot, all scanned/file
// metadata, complete typed buffers before paging, work, report/examples,
// optional conditioned tree/mask, metadata/writer. Work includes decoded bytes
// before paging, stored node slots, checks/stencils and selected point
// components. maxVoxelCount bounds stored source/mask leaf slots; compact tile
// volume is counted without expansion, with a separate mask semantic coverage
// cap. Counts are checked for overflow. Delayed loading is mandatory. Logical
// caps cannot bound upstream eager metadata/legacy allocations or process RSS.
// maxMetadataBytes counts all scanned/file metadata and, for mask writes, the
// generated grid/file metadata together. Snapshot and metadata bytes also count
// toward maxWork; no dense tile-volume expansion is charged or performed.
// Generation settings gridName/voxelSize/halfWidth/maxTriangles are ignored.
// OFF reports ENABLE_OPENVDB=ON before options/path access.
[[nodiscard]] DiagnosticReport DiagnoseVdb(
    const std::filesystem::path &input, const DiagnosticsOptions &options = {},
    const std::string &inputGrid = {}, const ProcessingOptions &processing = {},
    const PointAttributeOptions &pointLimits = {});

// Same report plus exclusive full-precision ZIP Bool UNION failure mask, with
// exact source map/custom metadata and regenerated structural tags. Failed
// tiles stay compact. Point locations are ORIGINAL stored voxels, not world
// buckets. Background/schema/global failures have no mask location; an empty
// mask does NOT mean the report passed. Missing derivative footprints are
// counted, not marked failed. Existing output/source bytes remain intact; owned
// partials are removed on failure. maskGridName is used only by this writer.
[[nodiscard]] DiagnosticReport WriteVdbDiagnosticMask(
    const std::filesystem::path &input, const std::filesystem::path &output,
    const DiagnosticsOptions &options = {}, const std::string &inputGrid = {},
    const ProcessingOptions &processing = {},
    const PointAttributeOptions &pointLimits = {});

} // namespace container::geometry::vdb
