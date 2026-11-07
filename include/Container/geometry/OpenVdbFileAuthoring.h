#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <optional>
#include <variant>
#include <vector>

namespace container::geometry::vdb {

using FileMetadataValue =
    std::variant<bool, int32_t, int64_t, float, double, std::string,
                 std::array<float, 3>, std::array<double, 3>>;
enum class FileMetadataOperation { Set, Remove };
struct FileMetadataEdit {
  std::string name;
  FileMetadataOperation operation{FileMetadataOperation::Set};
  FileMetadataValue value{std::string{}}; // Ignored by Remove.
};
struct FileGridEdit {
  std::string selector; // Required; shared descriptor:<exact name> syntax.
  std::optional<std::string> newName;
  std::vector<FileMetadataEdit> metadata;
};
struct FileAuthoringInput {
  std::filesystem::path path;
  // Empty selects all grids. Every explicit selector must resolve exactly one
  // grid. Repeated/overlapping selections, removals or edits fail.
  std::vector<std::string> select;
  std::vector<std::string> remove;
  // Only selected, nonremoved grids can be edited. A grid has at most one edit.
  std::vector<FileGridEdit> edits;
};
enum class FileMetadataConflict { Reject, PreferFirst, PreferLast };
enum class FileCompression { Zip, Blosc, None };
struct FileAuthoringOptions {
  std::vector<FileAuthoringInput>
      inputs; // 1..16; caller order defines output order.
  FileMetadataConflict metadataConflict{FileMetadataConflict::Reject};
  std::vector<FileMetadataEdit>
      metadata; // File-level edits, applied after merge.
  FileCompression compression{FileCompression::Zip};
  // Both are explicit opt-ins. Active-mask compression can lose inactive zero
  // signs; half precision quantizes Float/Vec3S tree values (not point attrs).
  bool activeMaskCompression{false};
  bool saveFloatAsHalf{false};
  size_t maxGrids{256}; // All scanned descriptors across inputs; [1,4096].
  size_t maxMetadataBytes{1024ull *
                          1024ull}; // Aggregate file/grid/point metadata.
  size_t maxEdits{
      1024}; // Selectors, grid edits and metadata edits in aggregate.
  uint64_t maxPoints{
      1'000'000}; // Aggregate PointData records, including inactive.
  size_t maxPointAttributes{1024}, maxPointGroups{4096};
};
struct FileAuthoringResult {
  size_t inputGridCount{0}, outputGridCount{0}, removedGridCount{0},
      renamedGridCount{0}, metadataEditCount{0};
  uintmax_t inputBytes{0}, outputBytes{0};
  uint64_t activeVoxelCount{0}, pointCount{0};
  std::vector<std::string> gridNames;
  std::vector<std::string> diagnostics;
};

// Bounded select/copy/rename/remove/merge of Float, Double, Bool, Mask, Int32,
// Int64, Vec3I, Vec3S, Vec3D and PointData grids. Other registered tree types
// fail explicitly. Default output preserves native value bits (including
// nonfinite values and inactive signed zero), activity, topology, transforms,
// class/vector semantics and custom metadata. No numerical field conversion,
// transform validation/resampling or point repartitioning occurs. Point arrays
// retain registered types/codecs, stride, hidden flags, groups and
// dictionaries; transient/streaming arrays are rejected rather than dropped.
// Empty native point trees and their application metadata archives remain
// unchanged.
//
// Final names must contain 1..256 bytes without NUL and be unique. Plain input
// names must be unambiguous; descriptor: selectors disambiguate duplicates.
// Human-readable descriptor collisions fail before any readGridMetadata call.
// Removing every grid writes a valid zero-grid VDB with file metadata.
// Metadata Set replaces an existing custom key, including its type. Remove
// requires an existing key. Duplicate keys within one edit batch fail. Existing
// opaque registered metadata is copied without conversion; only new numeric
// metadata must be finite. Structural grid/file-stat keys cannot be edited;
// use newName and the explicit output precision settings instead.
//
// Seekable supported-version inputs are privately snapshotted. Delayed loading
// is required so recorded statistics and PointData memUsageIfLoaded can be
// checked before payload access. Aggregate encoded input, loaded topology,
// metadata, point arrays and conservative writer workspace share maxInputBytes;
// active voxels and stored leaf slots share maxVoxelCount. Metadata decoding
// and legacy nonpaged point attributes may allocate within upstream reads
// before estimates are available; this is not a hostile-file RSS sandbox.
// ZIP/Blosc/none retain native bits when both loss opt-ins are disabled. A
// requested unavailable compressor fails. Potential precision losses are
// explicit result diagnostics; no post-hoc claim of exactness is made for
// either opt-in. Full precision overrides any source save-as-half flag.
// One worker; exclusive seekable output, bounded copy, owned temporary cleanup.
// Existing files and inputs remain intact on failure. Generation/triangle
// ProcessingOptions fields are ignored. OFF fails before paths or options.
[[nodiscard]] FileAuthoringResult
WriteAuthoredVdb(const std::filesystem::path &output,
                 const FileAuthoringOptions &authoring,
                 const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
