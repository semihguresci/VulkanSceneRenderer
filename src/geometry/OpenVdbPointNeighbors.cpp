#include "Container/geometry/OpenVdbPointNeighbors.h"
#include "OpenVdbProvenance.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbPointAttributesInternal.h"
#include <map>
#include <openvdb/tools/ParticleAtlas.h>
#include <openvdb/tools/PointIndexGrid.h>
#include <openvdb/tools/PointsToMask.h>
#include <sstream>
#include <tbb/task_arena.h>
#include <variant>
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB point indexing/neighbors are unavailable; "
                           "configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace point_attribute_detail;
using V = openvdb::Vec3d;
using IndexGrid = openvdb::tools::PointIndexGrid;
struct Predicate {
  std::array<std::vector<points::GroupHandle>, 3> handles;
  Predicate(const Leaf &leaf, const PointGroupSelection &s) {
    size_t i = 0;
    for (const auto *names : {&s.anyOf, &s.allOf, &s.noneOf}) {
      handles[i].reserve(names->size());
      for (const auto &n : *names)
        handles[i].push_back(leaf.groupHandle(n));
      ++i;
    }
  }
  bool operator()(openvdb::Index point) const {
    bool any = handles[0].empty();
    for (const auto &h : handles[0])
      any |= h.get(point);
    if (!any)
      return false;
    for (const auto &h : handles[1])
      if (!h.get(point))
        return false;
    for (const auto &h : handles[2])
      if (h.get(point))
        return false;
    return true;
  }
};
long double selectionBytes(const PointGroupSelection &s,
                           const PointAttributeOptions &options,
                           const PointAttributeSchema *schema) {
  long double bytes = 4096;
  size_t count = 0;
  for (const auto *names : {&s.anyOf, &s.allOf, &s.noneOf})
    for (const auto &n : *names) {
      if (++count > 3 * options.maxGroups)
        throw std::invalid_argument(
            "native point indexing group predicate exceeds count limit");
      name(n);
      if (schema && std::find(schema->groups.begin(), schema->groups.end(),
                              n) == schema->groups.end())
        throw std::invalid_argument(
            "native point indexing group was not found: " + n);
      bytes += 8.0L * n.size() + 1024;
    }
  return bytes;
}

struct Affine {
  openvdb::Vec3d origin;
  std::array<openvdb::Vec3d, 3> basis;
  double minimum;
  explicit Affine(const openvdb::math::Transform &transform) {
    if (!transform.isLinear())
      throw std::invalid_argument(
          "native point indexing requires an affine source transform");
    origin = transform.indexToWorld(openvdb::Vec3d(0));
    const auto map = transform.baseMap();
    basis = {map->applyJacobian({1, 0, 0}), map->applyJacobian({0, 1, 0}),
             map->applyJacobian({0, 0, 1})};
    auto normalized = basis;
    minimum = INFINITY;
    for (size_t k = 0; k < 3; ++k) {
      const double length =
          std::hypot(basis[k].x(), basis[k].y(), basis[k].z());
      if (!std::isfinite(origin[k]) || !std::isfinite(length) ||
          length < 1e-6 || length > 1e6)
        throw std::invalid_argument("native point affine axes must be finite "
                                    "in [1e-6,1e6] world units");
      minimum = std::min(minimum, length);
      normalized[k] /= length;
    }
    if (std::abs(normalized[0].dot(normalized[1].cross(normalized[2]))) < 1e-8)
      throw std::invalid_argument(
          "native point affine is singular or insufficiently conditioned");
  }
  openvdb::Vec3d world(const openvdb::Coord &coordinate,
                       const openvdb::Vec3f &local) const {
    for (size_t k = 0; k < 3; ++k)
      if (!std::isfinite(local[k]) || std::abs(local[k]) > .500001f)
        throw std::invalid_argument("selected point P requires finite "
                                    "voxel-local offsets in [-0.5,0.5]");
    openvdb::Vec3d result;
    for (size_t row = 0; row < 3; ++row) {
      double sum = origin[row], correction = 0;
      const auto add = [&](double term) {
        const double next = sum + term;
        correction += std::abs(sum) >= std::abs(term) ? (sum - next) + term
                                                      : (term - next) + sum;
        sum = next;
      };
      for (size_t col = 0; col < 3; ++col)
        for (double x : {double(coordinate[col]), double(local[col])}) {
          const double product = basis[col][row] * x;
          add(product);
          add(std::fma(basis[col][row], x, -product));
        }
      result[row] = sum + correction;
      const double spacing = std::max(
          std::abs(std::nextafter(result[row], INFINITY) - result[row]),
          std::abs(result[row] - std::nextafter(result[row], -INFINITY)));
      if (!std::isfinite(result[row]) || !std::isfinite(spacing) ||
          spacing > minimum * 1e-4)
        throw std::invalid_argument("native point world position loses source "
                                    "voxel precision; recenter source data");
    }
    return result;
  }
};

constexpr std::array<const char *, 3> ArchiveKeys{
    "container:point_empty_manifest_v1", "container:point_empty_descriptor_v1",
    "container:point_empty_leaf_flags_v1"};
struct Record {
  V position;
  double radius = 0;
  NativePointReference reference;
};
struct Source {
  std::unique_ptr<Snapshot> outer;
  Loaded native;
  openvdb::MetaMap::Ptr fileMeta;
  PointAttributeSchema schema;
  std::vector<Record> records;
  bool archived = false;
  long double resident = 0, metadata = 0;
};
template <class O>
void controls(const O &o, const ProcessingOptions &p,
              const PointAttributeOptions &limits) {
  validateOptions(p, limits);
  if (!o.maxWork || !o.maxGrids || o.maxGrids > 4096 || !o.maxMetadataBytes)
    throw std::invalid_argument(
        "invalid native point index work/catalog/metadata budgets");
}
bool numeric(const points::AttributeArray &a) {
  return a.hasValueType<bool>() || a.hasValueType<int32_t>() ||
         a.hasValueType<int64_t>() || a.hasValueType<float>() ||
         a.hasValueType<double>();
}
using ScalarHandles = std::variant<
    std::monostate, points::AttributeHandle<bool>,
    points::AttributeHandle<int32_t>, points::AttributeHandle<int64_t>,
    points::AttributeHandle<float>, points::AttributeHandle<double>>;
struct Radius {
  ScalarHandles h;
  explicit Radius(const points::AttributeArray &a) {
    if (!a.hasConstantStride() || a.stride() != 1 || points::isGroup(a) ||
        !numeric(a))
      throw std::invalid_argument(
          "particle radius requires numeric fixed stride-one values");
    if (a.hasValueType<bool>())
      h.emplace<1>(a, false);
    else if (a.hasValueType<int32_t>())
      h.emplace<2>(a, false);
    else if (a.hasValueType<int64_t>())
      h.emplace<3>(a, false);
    else if (a.hasValueType<float>())
      h.emplace<4>(a, false);
    else
      h.emplace<5>(a, false);
  }
  double get(openvdb::Index index) const {
    return std::visit(
        [&](const auto &value) -> double {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>,
                                       std::monostate>)
            throw std::logic_error("unbound radius");
          else
            return double(value.get(index));
        },
        h);
  }
};
template <class O>
Source source(const std::filesystem::path &input, const std::string &selection,
              const O &o, const ProcessingOptions &p,
              const PointAttributeOptions &limits, long double callerReserve,
              bool particles = false, const std::string &radiusName = {},
              double constantRadius = 0) {
  Source r;
  budget(callerReserve + Fixed + 1, p,
         "point index caller/options buffers exceed memory budget");
  auto outerBudget = p;
  outerBudget.maxInputBytes -= uintmax_t(std::ceil(callerReserve));
  r.outer = std::make_unique<Snapshot>(input, outerBudget);
  detail::RequireSeekableFile(r.outer->file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("native point indexing requires delayed "
                                "loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "native point indexing requires delayed-loading support");
#else
  openvdb::io::File file(r.outer->file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  r.fileMeta = file.getMetadata();
  r.metadata = metadataBytes(*r.fileMeta);
  long double catalog = Fixed + r.outer->encoded + callerReserve;
  std::map<std::string, size_t> names;
  const auto check = [&] {
    if (r.metadata > o.maxMetadataBytes)
      throw std::invalid_argument("joint native point file/grid/descriptor "
                                  "metadata exceeds maxMetadataBytes");
    budget(catalog + 8 * r.metadata, p,
           "native point catalog/metadata exceeds shared memory budget");
  };
  check();
  for (auto i = file.beginName(); i != file.endName(); ++i) {
    if (names.size() >= o.maxGrids)
      throw std::invalid_argument("point index catalog exceeds maxGrids");
    const auto n = i.gridName();
    catalog += 4.0L * n.size() + 256;
    check();
    if (!names.emplace(n, names.size()).second)
      throw std::invalid_argument(
          "point index source has colliding descriptors; rename grids first");
  }
  auto streamMetadata = std::make_shared<openvdb::io::StreamMetadata>();
  streamMetadata->setSeekable(true);
  std::ifstream stream(r.outer->file, std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  detail::ExactMetadataArchive archive;
  archive.begin(stream, streamMetadata);
  openvdb::MetaMap rawFileMetadata;
  rawFileMetadata.readMeta(stream);
  const int32_t gridCount = archive.count(stream);
  if (gridCount < 0 || size_t(gridCount) != names.size())
    throw std::invalid_argument("native point raw/native catalogs disagree");
  std::string descriptor;
  bool found = false;
  size_t selectedRank = SIZE_MAX;
  for (int32_t index = 0; index < gridCount; ++index) {
    openvdb::io::GridDescriptor encodedDescriptor;
    const auto meta = archive.next(stream, encodedDescriptor, r.outer->encoded);
    const auto n = openvdb::io::GridDescriptor::nameAsString(encodedDescriptor.uniqueName());
    const auto rank = names.find(n);
    if (rank == names.end())
      throw std::invalid_argument("native point raw/native descriptor names disagree");
    r.metadata += metadataBytes(*meta);
    check();
    if (!meta->isType<Grid>() ||
        !GridSelectionMatches(selection, n, meta->getName()))
      continue;
    if (selection.empty() && found && rank->second >= selectedRank)
      continue;
    if (found && !selection.empty())
      throw std::invalid_argument(
          "point index selector is ambiguous; use descriptor:<exact name>");
    descriptor = n;
    selectedRank = rank->second;
    found = true;
  }
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error("native point private snapshot changed");
  stream.close();
  file.close();
  if (!found)
    throw std::invalid_argument(
        "no matching native PointDataGrid found for indexing");
  const long double outerReserve = catalog + 8 * r.metadata;
  budget(outerReserve + Fixed + 1, p,
         "native point snapshots exceed memory budget");
  auto nativeBudget = p;
  nativeBudget.maxInputBytes -= uintmax_t(std::ceil(outerReserve));
  r.native =
      load(r.outer->file, "descriptor:" + descriptor, nativeBudget, limits);
  r.metadata += r.native.metadata;
  check();
  r.schema = r.native.schema;
  r.resident = outerReserve + r.native.snapshot->encoded +
               6 * r.native.decoded + 12 * r.native.metadata + Fixed;
  for (const auto *key : ArchiveKeys)
    r.archived |= bool((*r.native.grid)[key]);
  if (r.archived) {
    budget(r.resident + Fixed + 1, p,
           "empty-schema point validation exceeds shared budget");
    auto nested = p;
    nested.maxInputBytes -= uintmax_t(std::ceil(r.resident));
    const auto counted =
        CountSelectedPoints(r.native.snapshot->file, o.groups,
                            "descriptor:" + descriptor, nested, limits);
    if (!counted.archivedSchema)
      throw std::invalid_argument(
          "native empty-schema metadata conflicts with point payload");
    r.schema = counted.schema;
  }
  (void)selectionBytes(o.groups, limits, &r.schema);
  if (r.schema.pointCount > UINT32_MAX)
    throw std::invalid_argument(
        "native point index requires at most UInt32 point references");
  const auto groupTerms =
      o.groups.anyOf.size() + o.groups.allOf.size() + o.groups.noneOf.size();
  const long double decodingWork =
      r.schema.pointCount * (64.0L + 4 * groupTerms) +
      r.native.leaves.size() * 512.0L;
  if (decodingWork > o.maxWork)
    throw std::invalid_argument(
        "native point position/selection validation exceeds maxWork");
  budget(r.resident + 4.0L * r.schema.pointCount * sizeof(Record), p,
         "native point decoded position/reference workspace exceeds memory "
         "budget");
  const Affine map(r.native.grid->transform());
  r.records.reserve(size_t(r.schema.pointCount));
  uint64_t ordinal = 0;
  for (const auto *leaf : r.native.leaves) {
    Predicate selected(*leaf, o.groups);
    points::AttributeHandle<openvdb::Vec3f> positions(
        leaf->constAttributeArray("P"));
    std::optional<Radius> radii;
    if (particles && !radiusName.empty()) {
      if (radiusName == "P" || !leaf->hasAttribute(radiusName))
        throw std::invalid_argument(
            "particle radius attribute is missing or protected P");
      radii.emplace(leaf->constAttributeArray(radiusName));
    }
    for (auto i = leaf->beginIndexAll(); i; ++i) {
      const uint64_t original = ordinal++;
      if (!selected(*i))
        continue;
      const V world = map.world(i.getCoord(), positions.get(*i));
      const double radius =
          particles ? (radii ? radii->get(*i) : constantRadius) : 0;
      if (particles && (!std::isfinite(radius) || radius <= 0 || radius > 1e6))
        throw std::invalid_argument("selected particle radii must be finite "
                                    "positive world units <=1e6");
      const auto c = leaf->origin();
      r.records.push_back(
          {world, radius, {original, {c[0], c[1], c[2]}, uint32_t(*i)}});
    }
  }
  return r;
#endif
}
std::vector<PointNeighborMetadata> summaries(const openvdb::MetaMap &map) {
  std::vector<PointNeighborMetadata> out;
  for (auto i = map.beginMeta(); i != map.endMeta(); ++i) {
    const auto &m = *i->second;
    const bool known = dynamic_cast<const openvdb::StringMetadata *>(&m) ||
                       dynamic_cast<const openvdb::BoolMetadata *>(&m) ||
                       dynamic_cast<const openvdb::Int32Metadata *>(&m) ||
                       dynamic_cast<const openvdb::Int64Metadata *>(&m) ||
                       dynamic_cast<const openvdb::FloatMetadata *>(&m) ||
                       dynamic_cast<const openvdb::DoubleMetadata *>(&m) ||
                       dynamic_cast<const openvdb::Vec3SMetadata *>(&m) ||
                       dynamic_cast<const openvdb::Vec3DMetadata *>(&m);
    out.push_back({i->first, m.typeName(), known ? m.str() : std::string{},
                   m.size(), !known});
  }
  return out;
}
struct Array {
  using PosType = V;
  using ScalarType = double;
  const std::vector<Record> &records;
  V origin;
  double scale = 1;
  size_t size() const { return records.size(); }
  void getPos(size_t n, V &p) const {
    p = (records[n].position - origin) / scale;
    for (size_t k = 0; k < 3; ++k)
      if (!std::isfinite(p[k]))
        throw std::invalid_argument("point index local coordinates overflow; "
                                    "recenter/partition source");
  }
  void getRadius(size_t n, double &r) const { r = records[n].radius / scale; }
};
openvdb::Coord aligned(const openvdb::Coord &c, int64_t dimension) {
  openvdb::Coord result;
  for (size_t k = 0; k < 3; ++k) {
    const int64_t x = c[k],
                  block = x >= 0 ? x / dimension : (x + 1) / dimension - 1;
    result[k] = int32_t(block * dimension);
  }
  return result;
}
openvdb::Coord coordinate(const V &local,
                          const openvdb::math::Transform &transform) {
  const auto q = transform.worldToIndex(local);
  for (size_t k = 0; k < 3; ++k)
    if (!std::isfinite(q[k]) || q[k] < double(INT32_MIN) + 8192 ||
        q[k] > double(INT32_MAX) - 8192)
      throw std::invalid_argument("native point index coordinates need a "
                                  "larger voxel size or recentering");
  return transform.worldToIndexCellCentered(local);
}
struct Plan {
  double voxel = 0, minRadius = INFINITY, maxRadius = 0;
  size_t count = 0;
  std::set<openvdb::Coord> leaves, lower, upper;
  openvdb::CoordBBox active;
};
struct Planning {
  std::vector<Plan> levels;
  std::vector<uint32_t> labels;
  uint32_t sdkLevels = 1;
  long double treeBytes = 0;
};
Planning plan(const Source &s, const PointNeighborOptions &o,
              const ProcessingOptions &p, const Array &array,
              long double outputReserve) {
  Planning result;
  if (s.records.empty())
    return result;
  const size_t n = s.records.size();
  budget(s.resident + n * 2048.0L + outputReserve, p,
         "native point branch planning/sort workspace exceeds memory budget");
  result.labels.assign(n, 0);
  double minimum = INFINITY, maximum = 0;
  for (size_t n = 0; n < s.records.size(); ++n) {
    double radius;
    array.getRadius(n, radius);
    minimum = std::min(minimum, radius);
    maximum = std::max(maximum, radius);
  }
  const bool atlas = o.mode == PointNeighborMode::ParticleIntersections;
  result.sdkLevels = o.maxAtlasLevels <= 2 ? 1 : o.maxAtlasLevels - 1;
  result.levels.push_back({atlas ? std::max(1., minimum) : 1.});
  if (atlas && result.sdkLevels > 1)
    for (uint32_t split = 0; split < result.sdkLevels; ++split) {
      const uint32_t last = uint32_t(result.levels.size() - 1);
      const double threshold = 2 * result.levels.back().voxel;
      double next = INFINITY;
      for (size_t i = 0; i < n; ++i)
        if (result.labels[i] == last) {
          double radius;
          array.getRadius(i, radius);
          if (radius >= threshold)
            next = std::min(next, radius);
        }
      if (!std::isfinite(next))
        break;
      for (size_t i = 0; i < n; ++i)
        if (result.labels[i] == last) {
          double radius;
          array.getRadius(i, radius);
          if (radius >= threshold)
            result.labels[i] = last + 1;
        }
      result.levels.push_back({next});
    }
  std::vector<openvdb::math::Transform::Ptr> transforms;
  for (const auto &level : result.levels)
    transforms.push_back(
        openvdb::math::Transform::createLinearTransform(level.voxel));
  for (size_t i = 0; i < n; ++i) {
    auto &level = result.levels[result.labels[i]];
    V local;
    array.getPos(i, local);
    const auto c = coordinate(local, *transforms[result.labels[i]]);
    level.active.expand(c);
    level.leaves.insert(aligned(c, 8));
    level.lower.insert(aligned(c, 128));
    level.upper.insert(aligned(c, 4096));
    ++level.count;
    level.minRadius = std::min(level.minRadius, s.records[i].radius);
    level.maxRadius = std::max(level.maxRadius, s.records[i].radius);
  }
  long double slots = 0;
  using T = IndexGrid::TreeType;
  using L = T::LeafNodeType;
  using U = T::RootNodeType::ChildNodeType;
  using M = U::ChildNodeType;
  for (const auto &level : result.levels) {
    slots += level.leaves.size() * 512.0L;
    result.treeBytes +=
        level.leaves.size() *
            (sizeof(L) + 512 * sizeof(openvdb::PointIndex32) + 1024.0L) +
        level.lower.size() * (sizeof(M) + 1024.0L) +
        level.upper.size() * (sizeof(U) + 1024.0L) + level.count * 32.0L;
  }
  if (slots > p.maxVoxelCount)
    throw std::invalid_argument(
        "aggregate native index/atlas stored leaf slots exceed maxVoxelCount");
  budget(
      s.resident + 8 * result.treeBytes + n * 2048.0L + outputReserve + Fixed,
      p,
      "native index/atlas/sort/result workspace exceeds shared memory budget");
  return result;
}

openvdb::CoordBBox queryBox(const PointNeighborQuery &query, const Plan &level,
                            const V &origin, double scale, bool particles) {
  const double radius =
      (query.radius + (particles ? level.maxRadius : 0)) / scale;
  openvdb::Coord lo, hi;
  for (size_t k = 0; k < 3; ++k) {
    const double center = (query.center[k] - origin[k]) / scale;
    if (!std::isfinite(center))
      throw std::invalid_argument("point query relative coordinates overflow");
    const double a = std::max(std::floor((center - radius) / level.voxel) - 2,
                              double(level.active.min()[k])),
                 b = std::min(std::ceil((center + radius) / level.voxel) + 2,
                              double(level.active.max()[k]));
    if (a > b)
      return {};
    lo[k] = int32_t(a);
    hi[k] = int32_t(b);
  }
  return {lo, hi};
}
long double boxCells(const openvdb::CoordBBox &box) {
  if (box.empty())
    return 0;
  long double n = 1;
  for (size_t k = 0; k < 3; ++k)
    n *= int64_t(box.max()[k]) - box.min()[k] + 1;
  return n;
}
PointNeighborBatch neighbors(const Source &s,
                             std::span<const PointNeighborQuery> queries,
                             const PointNeighborOptions &o,
                             const ProcessingOptions &p,
                             long double inputReserve) {
  PointNeighborBatch result;
  result.schema = s.schema;
  result.selectedCount = s.records.size();
  result.archivedSchema = s.archived;
  result.gridMetadata = summaries(*s.native.grid);
  result.fileMetadata = summaries(*s.fileMeta);
  if (!s.native.leaves.empty())
    result.descriptorMetadata = summaries(
        s.native.leaves.front()->attributeSet().descriptor().getMetadata());
  result.diagnostic =
      "Native index/atlas candidates with inclusive rounded-Double world hypot "
      "filtering; stable original native references include inactive points. "
      "Atlas coordinates are automatically recentered in the forward world "
      "map. Raw PointPartitioner sort controls, sphere-coverage masks and "
      "mutable atlas updates are separate capabilities.";
  const V origin = s.records.empty() ? V(0) : s.records.front().position;
  result.indexOriginWorld = {origin[0], origin[1], origin[2]};
  Array array{s.records, origin, o.indexVoxelSize};
  const long double hitBound =
      std::min(static_cast<long double>(o.maxNeighbors),
               static_cast<long double>(s.records.size()) * queries.size());
  const long double outputReserve =
      inputReserve + hitBound * (3.0L * sizeof(PointNeighborHit)) +
      queries.size() * 128.0L +
      (o.includeBuckets ? s.records.size() * 512.0L : 0) + 16 * s.metadata;
  budget(s.resident + outputReserve + Fixed, p,
         "native point neighbor hits/buckets exceed shared memory budget");
  const auto planned = plan(s, o, p, array, outputReserve);
  const bool particles = o.mode == PointNeighborMode::ParticleIntersections;
  long double work =
      s.schema.pointCount *
          (128.0L + 4 * (o.groups.anyOf.size() + o.groups.allOf.size() +
                         o.groups.noneOf.size())) +
      s.native.leaves.size() * 512.0L +
      s.records.size() * (256.0L + 128 * planned.levels.size()) +
      queries.size() * 128.0L;
  for (const auto &q : queries)
    for (const auto &level : planned.levels) {
      work +=
          boxCells(queryBox(q, level, origin, array.scale, particles)) * 64 +
          s.records.size() * 16.0L;
      if (work > o.maxWork)
        throw std::invalid_argument(
            "native point neighbor candidate boxes/comparisons exceed maxWork");
    }
  if (work > o.maxWork)
    throw std::invalid_argument(
        "native point index construction exceeds maxWork");
  result.queries.resize(queries.size());
  if (s.records.empty())
    return result;
  std::unique_ptr<openvdb::tools::ParticleIndexAtlas> atlas;
  IndexGrid::Ptr index;
  std::vector<const IndexGrid *> levels;
  if (particles) {
    atlas = std::make_unique<openvdb::tools::ParticleIndexAtlas>();
    atlas->construct(array, 1., planned.sdkLevels);
    for (size_t k = 0; k < atlas->levels(); ++k)
      levels.push_back(&atlas->pointIndexGrid(k));
  } else {
    index = openvdb::tools::createPointIndexGrid<IndexGrid>(array, 1.);
    levels.push_back(index.get());
  }
  if (levels.size() != planned.levels.size() ||
      levels.size() > o.maxAtlasLevels)
    throw std::runtime_error(
        "native particle atlas levels disagree with bounded plan");
  std::vector<uint8_t> seen(s.records.size(), 0);
  long double actual = 0;
  uint64_t totalPoints = 0, totalSlots = 0;
  for (size_t k = 0; k < levels.size(); ++k) {
    const auto &grid = *levels[k];
    const auto &expected = planned.levels[k];
    if (grid.voxelSize()[0] != expected.voxel ||
        grid.tree().leafCount() != expected.leaves.size())
      throw std::runtime_error("native point index topology/transform "
                               "disagrees with coordinate preflight");
    uint64_t points = 0;
    for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
      if (!expected.leaves.contains(leaf->origin()))
        throw std::runtime_error(
            "native index created a leaf outside its bounded plan");
      PointIndexBucket bucket;
      bucket.level = uint32_t(k);
      const auto c = leaf->origin();
      bucket.origin = {c[0], c[1], c[2]};
      if (o.includeBuckets)
        bucket.points.reserve(leaf->indices().size());
      for (const auto value : leaf->indices()) {
        const size_t n = size_t(value);
        if (n >= s.records.size() || seen[n]++ || planned.labels[n] != k)
          throw std::runtime_error(
              "native point index lost/duplicated original references");
        ++points;
        if (o.includeBuckets)
          bucket.points.push_back(s.records[n].reference);
      }
      if (o.includeBuckets) {
        std::sort(
            bucket.points.begin(), bucket.points.end(),
            [](const auto &a, const auto &b) { return a.ordinal < b.ordinal; });
        result.buckets.push_back(std::move(bucket));
      }
    }
    if (points != expected.count)
      throw std::runtime_error("native atlas point count disagrees with plan");
    totalPoints += points;
    totalSlots += grid.tree().leafCount() * 512;
    actual += grid.memUsage();
    result.levels.push_back({expected.voxel * array.scale,
                             particles ? expected.minRadius : 0,
                             particles ? expected.maxRadius : 0, points,
                             grid.activeVoxelCount(), grid.tree().leafCount()});
  }
  if (totalPoints != s.records.size() || totalSlots > p.maxVoxelCount)
    throw std::runtime_error(
        "native index aggregate reference/topology cap violated");
  budget(s.resident + 8 * actual + s.records.size() * 2048.0L + outputReserve +
             Fixed,
         p, "native index actual memory exceeds shared budget");
  std::sort(result.buckets.begin(), result.buckets.end(),
            [](const auto &a, const auto &b) {
              if (a.level != b.level)
                return a.level < b.level;
              return a.origin < b.origin;
            });
  uint64_t hitCount = 0;
  for (size_t n = 0; n < queries.size(); ++n) {
    const V queryPosition(queries[n].center.data());
    auto &hits = result.queries[n];
    for (size_t k = 0; k < levels.size(); ++k) {
      const auto box = queryBox(queries[n], planned.levels[k], origin,
                                array.scale, particles);
      if (box.empty())
        continue;
      auto accessor = levels[k]->getConstAccessor();
      openvdb::tools::PointIndexIterator<> iterator(box, accessor);
      for (; iterator; ++iterator) {
        const auto &record = s.records.at(size_t(*iterator));
        // Recenter only the native index. The public rounded-Double oracle
        // subtracts the original world positions exactly once.
        const V delta = queryPosition - record.position;
        const double distance = std::hypot(delta[0], delta[1], delta[2]);
        if (!std::isfinite(distance))
          throw std::invalid_argument(
              "point neighbor world distance overflows");
        const double limit =
            queries[n].radius + (particles ? record.radius : 0);
        if (distance <= limit) {
          if (hitCount == o.maxNeighbors)
            throw std::invalid_argument(
                "point neighbor output exceeds maxNeighbors");
          ++hitCount;
          hits.push_back(
              {record.reference,
               {record.position[0], record.position[1], record.position[2]},
               distance,
               particles ? record.radius : 0});
        }
      }
    }
    std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) {
      if (a.distanceWorld != b.distanceWorld)
        return a.distanceWorld < b.distanceWorld;
      return a.reference.ordinal < b.reference.ordinal;
    });
  }
  return result;
}
void maskMetadata(openvdb::GridBase &grid, const Source &s,
                  const std::string &gridName, bool counts) {
  grid.insertMeta(*s.native.grid);
  detail::RemoveBoundaryProjectionMetadata(grid);
  constexpr std::array<std::string_view, 4> generated{
      "container:point_index_operation", "container:point_index_source_grid",
      "container:point_index_units", "container:point_count"};
  for (auto i = grid.beginMeta(); i != grid.endMeta();) {
    const auto key = (i++)->first;
    if (key == openvdb::GridBase::META_GRID_CLASS ||
        key == openvdb::GridBase::META_GRID_NAME ||
        key == openvdb::GridBase::META_SAVE_HALF_FLOAT ||
        key == openvdb::GridBase::META_IS_LOCAL_SPACE ||
        key == openvdb::GridBase::META_VECTOR_TYPE ||
        key == openvdb::GridBase::META_FILE_BBOX_MIN ||
        key == openvdb::GridBase::META_FILE_BBOX_MAX ||
        key == openvdb::GridBase::META_FILE_COMPRESSION ||
        key == openvdb::GridBase::META_FILE_MEM_BYTES ||
        key == openvdb::GridBase::META_FILE_VOXEL_COUNT ||
        key == openvdb::GridBase::META_FILE_DELAYED_LOAD ||
        std::find(ArchiveKeys.begin(), ArchiveKeys.end(), key) !=
            ArchiveKeys.end() ||
        std::find(generated.begin(), generated.end(), key) != generated.end())
      grid.removeMeta(key);
  }
  grid.setName(gridName);
  grid.setGridClass(openvdb::GRID_UNKNOWN);
  grid.setVectorType(openvdb::VEC_INVARIANT);
  grid.setIsInWorldSpace(true);
  grid.setSaveFloatAsHalf(false);
  grid.insertMeta(
      "container:point_index_operation",
      openvdb::StringMetadata(counts ? "repartitioned-point-count"
                                     : "repartitioned-point-center-mask"));
  grid.insertMeta("container:point_index_source_grid",
                  openvdb::StringMetadata(s.schema.gridName));
  grid.insertMeta("container:point_index_units",
                  openvdb::StringMetadata(
                      counts
                          ? "selected point records per output world voxel"
                          : "world point centers only; not sphere coverage"));
}
void publish(const std::filesystem::path &temporary,
             const std::filesystem::path &output) {
  std::ifstream input(temporary, std::ios::binary);
  std::ofstream target;
  bool owned = false;
  try {
    if (!input)
      throw std::runtime_error(
          "cannot read native indexed mask private output");
    input.exceptions(std::ios::badbit);
    target.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    owned = target.is_open();
    if (!owned)
      throw std::runtime_error(
          "cannot exclusively create indexed point mask output");
    target.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
      target.write(buffer.data(), input.gcount());
    if (!input.eof())
      throw std::runtime_error("indexed point mask private output read failed");
    target.flush();
    target.close();
  } catch (...) {
    target.exceptions(std::ios::goodbit);
    if (target.is_open())
      target.close();
    if (owned) {
      std::error_code e;
      std::filesystem::remove(output, e);
    }
    throw;
  }
}
// Native scale constructors reject tiny determinants, even though the public
// stock-map record supports well-conditioned h=1e-6. Avoid simplification and
// absolute determinant thresholds without a custom map or private member use.
openvdb::math::Transform::Ptr maskTransform(double h, const V &origin) {
  std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
  record.exceptions(std::ios::badbit | std::ios::failbit);
  openvdb::writeString(record,
                       openvdb::math::UniformScaleTranslateMap::mapType());
  origin.write(record);
  const double inverse = 1 / h;
  for (double x : {h, h, inverse, inverse * inverse, inverse * .5})
    V(x).write(record);
  record.seekg(0);
  openvdb::io::setCurrentVersion(record);
  auto result = std::make_shared<openvdb::math::Transform>();
  result->read(record);
  return result;
}
// Archive::writeGrid calls copyGrid; the ordinary implementation simplifies
// its transform. Share the immutable exact map/tree during that metadata copy.
template <class Grid> class ExactWriteGrid : public Grid {
public:
  explicit ExactWriteGrid(const Grid &grid)
      : Grid(openvdb::ConstPtrCast<typename Grid::TreeType>(
            grid.constTreePtr())) {
    this->insertMeta(grid);
    this->setTransform(detail::CopyTransformExact(grid.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, detail::CopyTransformExact(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactWriteGrid &>(*this).copyGrid());
  }
};
IndexedPointMaskResult mask(const Source &s,
                            const std::filesystem::path &output,
                            const IndexedPointMaskOptions &o,
                            const ProcessingOptions &p) {
  const std::string countName =
      o.includeCounts
          ? (o.countGridName.empty() ? p.gridName + "_count" : o.countGridName)
          : std::string{};
  auto transform = maskTransform(o.voxelSize, V(o.originWorld.data()));
  budget(s.resident + s.records.size() * 1024.0L + Fixed, p,
         "indexed point mask coordinate/count planning exceeds shared memory "
         "budget");
  std::map<openvdb::Coord, int64_t> cells;
  std::set<openvdb::Coord> leaves, lower, upper;
  const long double work =
      s.schema.pointCount *
          (128.0L + 4 * (o.groups.anyOf.size() + o.groups.allOf.size() +
                         o.groups.noneOf.size())) +
      s.native.leaves.size() * 512.0L + s.records.size() * 1024.0L;
  if (work > o.maxWork)
    throw std::invalid_argument("indexed point mask work exceeds maxWork");
  for (const auto &r : s.records) {
    const auto c = coordinate(r.position, *transform);
    ++cells[c];
    leaves.insert(aligned(c, 8));
    lower.insert(aligned(c, 128));
    upper.insert(aligned(c, 4096));
  }
  const uint64_t grids = o.includeCounts ? 2 : 1;
  if (leaves.size() > p.maxVoxelCount / 512 / grids ||
      cells.size() > p.maxVoxelCount / grids)
    throw std::invalid_argument(
        "indexed mask/count aggregate stored topology exceeds maxVoxelCount");
  using M = openvdb::MaskGrid::TreeType;
  using C = openvdb::Int64Grid::TreeType;
  const long double
      leafBytes = sizeof(M::LeafNodeType) + 1024.0L +
                  (o.includeCounts ? sizeof(C::LeafNodeType) + 4096.0L : 0),
      lowerBytes =
          sizeof(M::RootNodeType::ChildNodeType::ChildNodeType) + 1024.0L +
          (o.includeCounts
               ? sizeof(C::RootNodeType::ChildNodeType::ChildNodeType) + 1024.0L
               : 0),
      upperBytes =
          sizeof(M::RootNodeType::ChildNodeType) + 1024.0L +
          (o.includeCounts ? sizeof(C::RootNodeType::ChildNodeType) + 1024.0L
                           : 0);
  const long double projected = leaves.size() * leafBytes +
                                lower.size() * lowerBytes +
                                upper.size() * upperBytes;
  budget(
      s.resident + 8 * projected + s.records.size() * 1024.0L +
          grids * 16 * s.metadata + Fixed,
      p,
      "indexed mask/count trees/metadata/writer exceed shared memory budget");
  auto occupancy = openvdb::MaskGrid::create(false);
  occupancy->setTransform(detail::CopyTransformExact(*transform));
  Array array{s.records, V(0)};
  openvdb::tools::PointsToMask<openvdb::MaskGrid> rasterizer(*occupancy);
  rasterizer.addPoints<Array, V>(array, 0);
  openvdb::Int64Grid::Ptr counts;
  if (o.includeCounts) {
    counts = openvdb::Int64Grid::create(0);
    counts->setTransform(detail::CopyTransformExact(*transform));
    auto a = counts->getAccessor();
    for (const auto &[c, n] : cells)
      a.setValueOn(c, n);
  }
  if (occupancy->tree().leafCount() != leaves.size() ||
      occupancy->activeVoxelCount() != cells.size())
    throw std::runtime_error(
        "native PointsToMask disagrees with exact transform preflight");
  for (const auto &[c, n] : cells)
    if (!occupancy->tree().isValueOn(c))
      throw std::runtime_error("native PointsToMask dropped an occupied cell");
  maskMetadata(*occupancy, s, p.gridName, false);
  if (counts)
    maskMetadata(*counts, s, countName, true);
  long double actual = occupancy->memUsage() + 4 * metadataBytes(*occupancy);
  if (counts)
    actual += counts->memUsage() + 4 * metadataBytes(*counts);
  budget(
      s.resident + 4 * actual + s.records.size() * 1024.0L + Fixed, p,
      "indexed mask final tree/metadata/writer exceeds shared memory budget");
  IndexedPointMaskResult result;
  result.mask = {p.gridName, "unknown", occupancy->activeVoxelCount(),
                 o.voxelSize};
  if (counts)
    result.counts = {countName, "unknown", counts->activeVoxelCount(),
                     o.voxelSize};
  result.inputCount = s.schema.pointCount;
  result.selectedCount = s.records.size();
  openvdb::GridPtrVec outputGrids{
      std::make_shared<ExactWriteGrid<openvdb::MaskGrid>>(*occupancy)};
  if (counts)
    outputGrids.push_back(
        std::make_shared<ExactWriteGrid<openvdb::Int64Grid>>(*counts));
  const auto temporary = s.outer->directory / "indexed-mask.vdb";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove(path, e);
    }
  } cleanup{temporary};
  openvdb::io::File writer(temporary.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(outputGrids, *s.fileMeta);
  writer.close();
  budget(
      s.resident + 4 * actual + std::filesystem::file_size(temporary) + Fixed,
      p, "indexed mask encoded output and writer exceed shared memory budget");
  static_assert(std::is_nothrow_move_constructible_v<IndexedPointMaskResult>);
  publish(temporary, output);
  return result;
}
#endif
} // namespace

PointNeighborBatch
QueryPointNeighbors(const std::filesystem::path &input,
                    std::span<const PointNeighborQuery> queries,
                    const PointNeighborOptions &o, const std::string &selector,
                    const ProcessingOptions &p,
                    const PointAttributeOptions &limits) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;
  (void)queries;
  (void)o;
  (void)selector;
  (void)p;
  (void)limits;
  unavailable();
#else
  controls(o, p, limits);
  if (o.mode != PointNeighborMode::CentersWithinRadius &&
      o.mode != PointNeighborMode::ParticleIntersections)
    throw std::invalid_argument("invalid native point neighbor mode");
  if (!o.maxQueries || o.maxQueries > 1'000'000 ||
      queries.size() > o.maxQueries || !o.maxNeighbors ||
      o.maxNeighbors > 10'000'000 || !o.maxAtlasLevels ||
      o.maxAtlasLevels > 32 || !std::isfinite(o.indexVoxelSize) ||
      o.indexVoxelSize < 1e-6 || o.indexVoxelSize > 1e6)
    throw std::invalid_argument(
        "invalid native point query/atlas/voxel/output limits");
  const bool particles = o.mode == PointNeighborMode::ParticleIntersections;
  if (!particles && !o.radiusAttribute.empty())
    throw std::invalid_argument(
        "radius attribute applies only to particle intersection queries");
  if (particles && o.radiusAttribute.empty() &&
      (!std::isfinite(o.constantRadius) || o.constantRadius <= 0 ||
       o.constantRadius > 1e6))
    throw std::invalid_argument(
        "constant particle radius must be finite positive <=1e6");
  if (!o.radiusAttribute.empty())
    name(o.radiusAttribute);
  for (const auto &q : queries) {
    if (!std::isfinite(q.radius) || q.radius < 0 || q.radius > 1e6)
      throw std::invalid_argument(
          "point query radius must be finite in [0,1e6]");
    for (double x : q.center)
      if (!std::isfinite(x))
        throw std::invalid_argument("point query centers must be finite");
  }
  const auto caller = queries.size() * 128.0L +
                      selectionBytes(o.groups, limits, nullptr) + Fixed;
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto data = source(input, selector, o, p, limits, caller, particles,
                             o.radiusAttribute, o.constantRadius);
    return neighbors(data, queries, o, p, caller);
  });
#endif
}

IndexedPointMaskResult WriteIndexedPointMask(
    const std::filesystem::path &input, const std::filesystem::path &output,
    const IndexedPointMaskOptions &o, const std::string &selector,
    const ProcessingOptions &p, const PointAttributeOptions &limits) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;
  (void)output;
  (void)o;
  (void)selector;
  (void)p;
  (void)limits;
  unavailable();
#else
  controls(o, p, limits);
  validateOptions(p, limits, true);
  if (!std::isfinite(o.voxelSize) || o.voxelSize < 1e-6 || o.voxelSize > 1e6)
    throw std::invalid_argument(
        "indexed point mask voxel size must be in [1e-6,1e6]");
  for (double x : o.originWorld)
    if (!std::isfinite(x))
      throw std::invalid_argument("indexed point mask origin must be finite");
  if (!o.includeCounts && !o.countGridName.empty())
    throw std::invalid_argument("count grid name requires includeCounts");
  if (o.includeCounts) {
    const auto countName =
        o.countGridName.empty() ? p.gridName + "_count" : o.countGridName;
    name(countName);
    if (countName == p.gridName)
      throw std::invalid_argument(
          "indexed mask/count grid names must be distinct");
  }
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "indexed point mask output must be new in an existing directory");
  const auto caller = selectionBytes(o.groups, limits, nullptr) + Fixed;
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto data = source(input, selector, o, p, limits, caller);
    return mask(data, output, o, p);
  });
#endif
}
} // namespace container::geometry::vdb
