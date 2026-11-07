#pragma once
#include "Container/geometry/OpenVdbRegionQueries.h"

namespace container::geometry::vdb {
struct ActiveValueComponentsOptions {
  std::string gridNamePrefix{"component"};
  uint32_t maxComponents{64},maxGrids{256};
  uint64_t maxActiveVoxels{1'000'000};
  uint64_t maxStoredLeafVoxels{32ull*1024*1024}; // Each source/expanded tree.
  uint64_t maxAggregateOutputLeafVoxels{32ull*1024*1024};
  uint64_t maxNodes{1'000'000}; // Joint source/working/labels/output upper bound.
  uint64_t maxWork{250'000'000};
  uintmax_t maxInputBytes{512ull*1024*1024}; // Joint encoded/native/scratch/IO.
  uintmax_t maxOutputBytes{512ull*1024*1024};
  uintmax_t maxMetadataBytes{1024*1024};
};
struct ActiveValueComponent {
  std::string gridName;
  uint64_t activeVoxelCount{},storedLeafVoxels{};
  std::optional<IndexRegion> activeBounds;
};
struct ActiveValueComponentsResult {
  std::string valueType;
  uint64_t activeComponentCount{},sourceActiveVoxels{},sourceActiveTiles{};
  uint64_t sourceStoredLeafVoxels{},expandedStoredLeafVoxels{};
  uint64_t aggregateOutputLeafVoxels{},nodeUpperBound{};
  uint64_t nativeSourceBytes{},workspaceUpperBound{},workUpperBound{};
  bool emptySourceCopy{false};
  std::vector<ActiveValueComponent> grids;
  std::vector<std::string> diagnostics;
};
// Writes SOURCE FIELD values with one six-connected active component per grid.
// Bool/Mask/Int32/Int64/Float/Double/Vec3I/Vec3S/Vec3D activity is independent
// of value: active false/zero/NaN/Inf participate; inactive values do not.
// Face adjacency is in INDEX space, with checked Int32 boundary neighbors.
// Component minima sort lexicographically X/Y/Z and determine deterministic
// prefix_000001,... names. A tile is expanded only after joint growth bounds.
//
// Every non-Mask output retains the source VALUE at EVERY represented/infinite
// coordinate, including inactive continuations, signed zero and native NaN bits.
// Other components become INACTIVE while keeping their stored values. Native
// Mask has activity only, so its values necessarily follow the chosen component.
// No pruning/reset/interpolation, value partition, distance repair, solid/cavity
// inference or physical density/mass partition is performed. Source class and
// vector/world tags remain labels of the unchanged field, not certificates of
// an isolated SDF/fog component. Raw vectors are not transformed.
//
// Empty activity writes ONE inactive source-field copy, explicitly reported as
// activeComponentCount=0/emptySourceCopy=true, with suffix_000000. Stored inactive
// data is retained.
// Exact native maps (including nonlinear/tiny/remote maps) and custom grid/file
// metadata survive; name/statistics and five canonical boundary-projection
// provenance keys are regenerated/removed. ZIP without active-mask compression
// or half storage preserves native values. Input bytes are never changed.
//
// A bounded native label tree and explicit six-neighbor BFS adapt SDK topology
// facilities; unbounded stock segmentation/masked-copy allocation is not used.
// Source full typed paging/nodes/catalog, active-tile expansion, labels/queues/
// sorting, all output clones/metadata and writer/native+encoded scratch share
// preflight caps. All components are counted before the first output clone.
// maxComponents is1..64; catalog is1..4096; work is1..1e10; byte caps <=16GiB.
// Delayed loading is mandatory. Native eager metadata/topology decoding precedes
// post-decode checks, so these logical limits are not a hostile-file RSS/CPU
// sandbox. Exclusive new Unicode output; failure removes only owned partials.
// Upstream narrow scratch paths must be ASCII usable; OFF precedes paths/options.
[[nodiscard]] ActiveValueComponentsResult WriteActiveValueComponents(
    const std::filesystem::path& input,const std::filesystem::path& output,
    const ActiveValueComponentsOptions& options={},const std::string& inputGrid={});
} // namespace container::geometry::vdb
