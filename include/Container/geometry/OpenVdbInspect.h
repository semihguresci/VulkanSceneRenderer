#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace container::geometry::vdb {

struct InspectionOptions {
  // Empty selects all grids. Plain names must be unambiguous; descriptor:
  // selects an exact file descriptor, including duplicate display names.
  std::string gridName{};
  bool loadStatistics{false};
  size_t maxGrids{256}; // All scanned descriptors, in [1,4096].
  size_t maxMetadataBytes{1024ull * 1024ull};
  uintmax_t maxInputBytes{512ull * 1024ull * 1024ull};
  uint64_t maxActiveVoxels{8'000'000};
  // Zero disables the histogram. Otherwise [1,4096], with loadStatistics=true.
  uint32_t histogramBins{0};
  // Optional finite increasing range; default is the active extrema. Values
  // outside an explicit range count separately. The maximum is inclusive.
  std::optional<std::array<double,2>> histogramRange;
};

struct InspectionIndexBounds {
  // Inclusive active voxel-center coordinates, as stored by OpenVDB.
  std::array<int32_t, 3> minimum{};
  std::array<int32_t, 3> maximum{};
};

struct InspectionWorldBounds {
  // Conservative occupied-cell envelope: transform all eight corners of
  // [index minimum - .5, index maximum + .5]. Only available for safe affine maps.
  std::array<double, 3> minimum{};
  std::array<double, 3> maximum{};
};

struct InspectionMetadata {
  std::string name;
  std::string type;
  std::string value;
};

struct ScalarHistogram {
  // No active values and no explicit range leave the bounds null.
  std::optional<double> minimum, maximum;
  std::vector<uint64_t> bins;
  uint64_t belowRange{0}, aboveRange{0};
};

struct ScalarStatistics {
  double background{0.0};
  uint64_t count{0}; // Active voxel count; tiles contribute their full voxel count.
  std::optional<double> minimum{};
  std::optional<double> maximum{};
  double sum{0.0}; // Active values only, weighted by tile voxel counts.
  // Empty active grids have null moments. Fixed exact binary accumulators
  // avoid cancellation and squared overflow/underflow. Population convention.
  std::optional<double> mean, populationStdDev, rootMeanSquare;
  std::optional<ScalarHistogram> histogram;
  uint64_t actualMemoryBytes{0};
  std::optional<InspectionIndexBounds> indexBounds{};
  std::optional<InspectionWorldBounds> worldBounds{};
};

struct GridInspection {
  std::string descriptorName;
  std::string name;
  std::string valueType;
  std::string gridClass;
  std::string vectorType;
  bool inWorldSpace{false};
  std::array<double, 3> voxelSize{};
  bool affine{false};
  // Row-major homogeneous matrix for column vectors: world = M * [index,1].
  // Nonlinear, singular or non-finite maps have no matrix/world envelope;
  // transformDiagnostic explains why. The affine flag describes the map type.
  std::optional<std::array<double, 16>> indexToWorld{};
  std::string transformDiagnostic;
  // Recorded hints, not recomputed facts. Absent, malformed or negative fields
  // remain null. Empty active bounds are null, including a recorded zero count.
  std::optional<uint64_t> activeVoxelCount{};
  std::optional<uint64_t> memoryBytes{};
  std::optional<InspectionIndexBounds> indexBounds{};
  std::optional<InspectionWorldBounds> worldBounds{};
  std::vector<InspectionMetadata> metadata;
  std::optional<ScalarStatistics> statistics{};
  std::string statisticsUnavailableReason;
};

struct FileInspection {
  uintmax_t fileBytes{0};
  std::vector<GridInspection> grids;
};

// Default inspection reads descriptors, metadata and transforms only; tree
// background is unavailable until explicit scalar statistics are requested.
// Native transform records are read without SDK copy/simplification, retaining
// distant scales and sub-tolerance shears exactly. Affine voxel sizes derive
// from the linear coefficients, without subtracting translated world points.
// Streaming files without random-access grid offsets are rejected before
// File::open(), which would otherwise eagerly load their trees.
// Legacy formats before 216 and newer unsupported formats also fail: older
// transforms cannot be read independently from the topology.
// Encoded descriptor/grid/block/end ranges and nonnegative grid counts are
// checked before seeking past payloads; duplicate descriptor order matches SDK
// File iteration. Statistics verify the archive UUID before loading trees.
// Human-readable descriptor collisions (duplicate names plus a literal
// bracket-suffixed name) fail instead of reporting a different tree's metadata.
// Non-finite voxel sizes fail; nonlinear or invalid affine matrices remain
// unavailable with an explicit diagnostic.
// Statistics load one selected FloatGrid/DoubleGrid at a time. Recorded voxel
// and memory hints are required and checked before loading, then actual active
// voxels, stored leaf topology and tree memory are checked again. Every stored
// scalar (including inactive values and background) must be finite; active
// sums must remain finite double values. Empty active grids have null extrema.
// Moments use exact weighted sums/squares before final Double rounding. A
// nonzero moment which rounds to zero, or an overflowing sum/moment, fails.
// Histograms weight active tiles without expansion. All bins are half-open,
// except the inclusive maximum in the last bin; a constant automatic range
// uses the first bin. Binary-rational boundaries are exact, including extreme
// or adjacent Double bounds. Bins/report storage shares maxMetadataBytes.
// Unsupported grids get a statisticsUnavailableReason, or fail if explicitly
// selected with loadStatistics. Limits cover encoded input, scanned grids and
// aggregate metadata/report strings. Upstream metadata decoding precedes the
// metadata-budget check, so these are not a strict hostile-file heap sandbox.
// Disabled builds report availability before reading the filesystem.
[[nodiscard]] FileInspection InspectFile(
    const std::filesystem::path& input, const InspectionOptions& options = {});

} // namespace container::geometry::vdb
