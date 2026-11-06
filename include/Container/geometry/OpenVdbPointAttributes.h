#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <span>
#include <variant>
#include <vector>

namespace container::geometry::vdb {

// Bool payloads use bytes restricted to 0/1 (avoids vector<bool> proxy storage).
using PointAttributeValues = std::variant<
    std::vector<uint8_t>, std::vector<int32_t>, std::vector<int64_t>,
    std::vector<float>, std::vector<double>, std::vector<std::array<float,3>>,
    std::vector<std::array<double,3>>, std::vector<std::string>>;
enum class PointEditOperation { Add, Set, Rename, Drop };
struct PointAttributeEdit {
  PointEditOperation operation{PointEditOperation::Add};
  std::string name, newName;
  PointAttributeValues values;
};
struct PointGroupEdit {
  PointEditOperation operation{PointEditOperation::Add};
  std::string name, newName;
  std::vector<uint8_t> membership;
};
struct PointAttributeOptions {
  size_t maxPoints{1'000'000}, maxAttributes{64}, maxGroups{256}, maxEdits{64};
  // Aggregate source dictionary bytes + input edit string bytes, or decoded
  // string bytes returned by ReadPointAttribute. Strings are byte-preserving.
  uintmax_t maxStringBytes{1024*1024};
};
struct PointAttributeDescription {
  std::string name, valueType, codec;
  uint32_t stride{0}; // Zero if variable or not identical across leaves.
  bool groupStorage{false}, anyHidden{false}, readable{false};
};
struct PointAttributeSchema {
  std::string gridName;
  uint64_t pointCount{0}, leafCount{0};
  std::vector<PointAttributeDescription> attributes;
  std::vector<std::string> groups;
};
struct PointAttributeColumn {
  PointAttributeDescription attribute;
  PointAttributeValues values;
};

// Native traversal order: ascending lexicographic (x,y,z) leaf origin, then
// stored point index within each leaf (the voxel-offset order). Inactive-voxel
// points participate. Read columns/memberships and per-point edits use exactly
// this order, which is independent of world-position sorting in ReadPoints.
[[nodiscard]] PointAttributeSchema InspectPointAttributes(
    const std::filesystem::path& input, const std::string& inputGrid = {},
    const ProcessingOptions& processing = {}, const PointAttributeOptions& options = {});
[[nodiscard]] PointAttributeColumn ReadPointAttribute(
    const std::filesystem::path& input, const std::string& attribute,
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointAttributeOptions& options = {});
[[nodiscard]] std::vector<uint8_t> ReadPointGroup(
    const std::filesystem::path& input, const std::string& group,
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointAttributeOptions& options = {});

// Edits run sequentially, attributes first and then groups, in one transaction.
// Add requires a missing name, Set an existing matching type/codec and stride1,
// Rename a missing destination, Drop an existing name. New attributes use
// NullCodec (native StringCodec for strings); payload size1 broadcasts, otherwise
// it must equal pointCount. Rename/Drop must have no payload/newName as applicable.
// ALL mutations of position attribute P, including Set/rename-to-P, are rejected:
// coordinate changes require point repartitioning. Group backing arrays cannot
// be edited directly; group operations preserve every other group's bits.
// Drop group retains unused backing storage, without point deletion/filtering.
//
// Existing registered attributes/codecs, hidden flags, grid+descriptor metadata,
// transform, offsets and active/inactive masks are preserved. Transient/streaming
// payloads, active point tiles, heterogeneous descriptors and invalid array
// lengths are rejected before materialization; no unsupported data is discarded.
// Empty trees have no persisted descriptor: edits require at least one leaf.
// Empty edit batches can copy an empty grid. processing.gridName names output;
// voxelSize/halfWidth/maxTriangles are ignored, and no repartitioning occurs.
//
// Limits: points<=INT64_MAX, attributes1..1024, groups1..4096, edits1..1024. Names1..256 bytes without
// NUL; __group names are reserved for native backing arrays. Numeric new/read
// values must be finite. Private bounded snapshots, mandatory delayed loading,
// decoded attribute/dictionary/clone/edit/output reserves and one worker bound
// allocations before native operations. Logical budgets are not an RSS sandbox;
// upstream legacy nonpaged encodings can allocate before per-array checks.
// Output is seekable/exclusive; failures leave no partial file. OFF reports
// ENABLE_OPENVDB=ON before input/options/path access. Generic rasterization and
// group-based point deletion remain separate capabilities.
[[nodiscard]] PointAttributeSchema WritePointAttributeEdits(
    const std::filesystem::path& input, const std::filesystem::path& output,
    std::span<const PointAttributeEdit> attributes, std::span<const PointGroupEdit> groups = {},
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointAttributeOptions& options = {});

} // namespace container::geometry::vdb
