#pragma once

#include "Container/geometry/OpenVdbPointSelection.h"
#include <span>

namespace container::geometry::vdb {
struct NativePointReference {
  uint64_t ordinal{
      0}; // P03 lexicographic leaf origin, then native stored index.
  std::array<int32_t, 3> leafOrigin{};
  uint32_t localIndex{
      0}; // Includes inactive-voxel records; never renumbered by selection.
};
enum class PointNeighborMode { CentersWithinRadius, ParticleIntersections };
struct PointNeighborQuery {
  std::array<double, 3> center{};
  double radius{0};
};
struct PointNeighborOptions {
  PointNeighborMode mode{PointNeighborMode::CentersWithinRadius};
  PointGroupSelection groups;
  double indexVoxelSize{.1}; // World units, [1e-6,1e6].
  std::string
      radiusAttribute; // Particle mode: positive scalar, excluding P/groups.
  double constantRadius{1}; // Used when radiusAttribute is empty.
  uint32_t maxAtlasLevels{
      16}; // Actual level cap, [1,32]; subdivision is adaptive.
  bool includeBuckets{false};
  size_t maxQueries{10000};
  uint64_t maxNeighbors{1'000'000}, maxWork{200'000'000};
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024 * 1024};
};
struct PointNeighborHit {
  NativePointReference reference;
  std::array<double, 3> positionWorld{};
  double distanceWorld{0}, radiusWorld{0};
};
struct PointIndexLevel {
  double voxelSize{0}, minRadius{0}, maxRadius{0};
  uint64_t pointCount{0}, occupiedVoxelCount{0}, leafCount{0};
};
struct PointIndexBucket {
  uint32_t level{0};
  std::array<int32_t, 3> origin{}; // 8³ index-cell bucket.
  std::vector<NativePointReference> points;
};
struct PointNeighborMetadata {
  std::string name, type, value;
  uint64_t nativeBytes{0};
  bool opaque{false};
};
struct PointNeighborBatch {
  PointAttributeSchema schema;
  uint64_t selectedCount{0};
  bool archivedSchema{false};
  std::array<double, 3> indexOriginWorld{};
  std::vector<PointIndexLevel> levels;
  std::vector<PointIndexBucket> buckets;
  std::vector<std::vector<PointNeighborHit>> queries;
  std::vector<PointNeighborMetadata> gridMetadata, fileMetadata,
      descriptorMetadata;
  std::string diagnostic;
};
// Centers: rounded-Double Euclidean distance <= query.radius. Particles:
// distance <= query.radius + positive particle radius. Boundaries are
// inclusive; query radius0 includes exact coincident centers/point-in-particle
// queries. Hits sort by distance, then original ordinal; no self/duplicate
// suppression. Real PointIndexGrid/ParticleAtlas construction retains original
// references, including selected inactive points. Native atlas radial filters
// are avoided: per-level conservative native bucket queries plus Double hypot
// filtering keep multi-radius/near-equal radii and inclusive boundaries
// correct. Atlas iteration order and approximate-uniform-radius optimizations
// do not define the result. Registered fixed P codecs decode without
// conversion/repartitioning. Chosen radius supports
// bool/int32/int64/Float/Double, finite positive <=1e6; unchosen
// attributes/groups/metadata remain untouched. P, dynamic and group radius
// storage reject. Radius controls apply only to particle mode. Atlas level caps
// bound subdivision rather than promising an exact number of levels.
//
// Index coordinates are centered around the first selected world position;
// indexOriginWorld plus each level's voxel size interprets optional buckets.
// Internal native positions/radii divide by indexVoxelSize before construction,
// making the base native scale1 even at the supported world-scale endpoints.
// Final filtering always uses original world values, including tiny radii;
// normalized radii only decide atlas partitions. Level voxel sizes are restored
// to world units. Native floating bucket boundary rounding follows that order.
// Bucket members retain ascending native ordinal. These are useful8³ native
// index partitions, not exposure of every raw PointPartitioner sort control.
// Source affine maps are conditioned during compensated P reconstruction;
// query distances/radii stay in world units. Native Int32 margins, occupied
// branch/leaf slots, full source/attribute metadata, atlas scratch, query
// boxes, output hits and buckets share caps before upstream allocation. One
// worker, mandatory delayed loading/private snapshots, Unicode source paths.
// Logical budgets are not an RSS sandbox for eager metadata/legacy encodings.
// Empty selections yield empty hits; bounded native empty-schema archives are
// checked. Only processing.maxVoxelCount/maxInputBytes apply; OFF precedes all
// I/O/options.
[[nodiscard]] PointNeighborBatch
QueryPointNeighbors(const std::filesystem::path &input,
                    std::span<const PointNeighborQuery> queries,
                    const PointNeighborOptions &options = {},
                    const std::string &inputGrid = {},
                    const ProcessingOptions &processing = {},
                    const PointAttributeOptions &attributeLimits = {});

struct IndexedPointMaskOptions {
  PointGroupSelection groups;
  double voxelSize{.1};
  std::array<double, 3> originWorld{};
  bool includeCounts{false};
  std::string countGridName; // Defaults gridName+"_count".
  uint64_t maxWork{200'000'000};
  size_t maxGrids{256};
  uintmax_t maxMetadataBytes{1024 * 1024};
};
struct IndexedPointMaskResult {
  ProcessingResult mask, counts;
  uint64_t inputCount{0}, selectedCount{0};
};
// Native PointsToMask serial center occupancy on the EXPLICIT axis-aligned
// output world map. It re-buckets positions, unlike original stored-voxel
// masks. Radius attributes are irrelevant: this is not sphere coverage.
// Optional Int64 per-cell counts include inactive-voxel points. Original custom
// grid/file metadata survives; PointData structural/archive keys are removed
// from masks. All output stored leaves/counts share topology/memory/ZIP writer
// budgets. Exclusive seekable full-precision VDB, Unicode output, owned-partial
// cleanup.
[[nodiscard]] IndexedPointMaskResult WriteIndexedPointMask(
    const std::filesystem::path &input, const std::filesystem::path &output,
    const IndexedPointMaskOptions &options = {},
    const std::string &inputGrid = {}, const ProcessingOptions &processing = {},
    const PointAttributeOptions &attributeLimits = {});
} // namespace container::geometry::vdb
