#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace container::geometry::vdb {
struct RegionFloat32Bits { uint32_t bits{}; bool operator==(const RegionFloat32Bits&)const=default; };
struct RegionFloat64Bits { uint64_t bits{}; bool operator==(const RegionFloat64Bits&)const=default; };
using RegionNativeValue=std::variant<bool,int32_t,int64_t,RegionFloat32Bits,RegionFloat64Bits,
    std::array<int32_t,3>,std::array<RegionFloat32Bits,3>,std::array<RegionFloat64Bits,3>>;
enum class RegionValueType { Boolean,Mask,Int32,Int64,Float,Double,Vec3I,Vec3S,Vec3D };
struct IndexRegion {
  std::array<int32_t,3> minimum{},maximum{}; // Inclusive voxel CENTER indices.
  bool operator==(const IndexRegion&)const=default;
};
struct ActiveRegionRecord {
  IndexRegion source,clipped;
  uint32_t sourceLevel{}; // 0 voxel; 1/2/3 native 8/128/4096-cubed tile.
  bool tile{false};
  uint64_t voxelCount{}; // Exact multiplicity of clipped, never expanded.
  RegionNativeValue value;
};
struct RegionQueryMetadata {
  std::string name,type; // Native byte strings; UTF-8 is not assumed.
  std::vector<uint8_t> serializedValue; // Metadata::write, UInt32 size prefix included.
};
struct ActiveRegionOptions {
  uintmax_t maxInputBytes{512ull*1024*1024}; // Joint encoded/native/metadata/report reserve.
  uint64_t maxStoredLeafVoxels{32ull*1024*1024},maxNodes{1'000'000};
  uint64_t maxSourceActiveVoxels{std::numeric_limits<uint64_t>::max()};
  uint64_t maxWork{250'000'000};
  size_t maxGrids{256},maxQueries{256};
  uintmax_t maxMetadataBytes{1024*1024};
  uint64_t maxRecordsPerQuery{4096},maxTotalRecords{65536}; // Zero: counts only.
};
struct ActiveRegionResult {
  IndexRegion region;
  uint64_t matchedRecordCount{},matchedVoxelCount{},returnedVoxelCount{};
  uint64_t omittedRecordCount{},omittedVoxelCount{};
  bool countsComplete{true},recordsTruncated{false};
  std::vector<ActiveRegionRecord> records;
};
struct ActiveRegionBatch {
  std::string descriptorName,gridName,valueType,gridClass,vectorType,transformType;
  RegionValueType nativeType{RegionValueType::Float};
  bool inWorldSpace{true},affine{false};
  std::optional<std::array<double,16>> indexToWorld; // Finite affine coefficients, COLUMN convention.
  std::vector<uint8_t> serializedTransform; // Exact encoded source transform record.
  RegionNativeValue background;
  std::vector<RegionQueryMetadata> gridMetadata,fileMetadata;
  uint64_t sourceActiveVoxels{},sourceActiveRecords{},sourceActiveTiles{},sourceActiveTileVoxels{};
  uint64_t sourceLeafNodes{},sourceLowerNodes{},sourceUpperNodes{},sourceRootEntries{};
  uint64_t storedLeafVoxels{},storedInternalSlots{},nodeAndRootEntryCount{};
  uint64_t nativeBytes{},workUpperBound{},workspaceUpperBound{};
  bool countsComplete{true};
  std::string traversal{"native_depth_first_child_table_order"};
  std::vector<ActiveRegionResult> queries;
  std::vector<std::string> diagnostics;
};
// Read-only nine-type ACTIVE value/region intersection. Activity is independent
// of value: Bool false, integer/Float zero and nonfinite Float payloads remain
// active. Mask values equal activity. Inactive continuations are never hits.
// Float wrappers carry exact native bits (including NaN payload and signedzero)
// rather than lossy JSON numbers. Vec3 components stay raw; tags do not transform
// positions, normals, velocities or arbitrary stored components.
//
// Records are native voxels/tiles intersected with inclusive INDEX boxes. A
// root tile contributes up to2^36 voxels without expansion. No world ROI or
// geometry/inverse-map validity is implied. Finite affine matrices are returned
// directly from origin/Jacobian coefficients; other maps retain exact serialized
// source records and have no matrix. Native metadata bytes are authoritative.
//
// All source active values are censused with checked UInt64 arithmetic before
// returning, including an EMPTY query batch. Every query has complete matched
// counts even when its record prefix is truncated. The pinned native depth-first
// child-table order defines each prefix; earlier input queries consume the
// aggregate record quota first. Overlapping queries count independently.
// maxNodes counts the root, root entries and all allocated leaf/internal nodes;
// full internal/leaf slot traversal separately enters the shared work preflight.
// Huge active-tile volume is separate from stored-leaf allocation limits.
//
// A Unicode-safe immutable private snapshot, strict unambiguous selector,
// aggregate catalog/file/grid metadata, full typed native memory BEFORE paging,
// node/slot census, every query and bounded output share caps. Delayed loading
// is mandatory. Native metadata and root/internal topology decode precede their
// post-decode checks; logical reserves are not a hostile-file allocator/RSS or
// CPU sandbox. No source edit, provenance invalidation, writer or tile expansion.
// Unsupported point/custom trees reject explicitly when selected. Empty selector
// requires exactly one supported grid. OFF precedes options/path access.
[[nodiscard]] ActiveRegionBatch QueryActiveRegions(const std::filesystem::path& input,
    std::span<const IndexRegion> regions,const ActiveRegionOptions& options={},
    const std::string& inputGrid={});
} // namespace container::geometry::vdb
