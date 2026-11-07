#include "Container/geometry/OpenVdbPointNeighbors.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <set>
#include <sstream>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/points/PointGroup.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbPointNeighborsAvailability, DisabledBeforeOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ProcessingOptions p;
  p.maxInputBytes = 0;
  vdb::PointNeighborOptions o;
  o.indexVoxelSize = -1;
  try {
    (void)vdb::QueryPointNeighbors("missing", {}, o, {}, p);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
  try {
    (void)vdb::WriteIndexedPointMask("missing", "missing/out", {}, {}, p);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points = openvdb::points;
using Grid = points::PointDataGrid;
using V = openvdb::Vec3d;
class ExactNeighborFixture final: public Grid {
public:
  explicit ExactNeighborFixture(const Grid& source):Grid(openvdb::ConstPtrCast<Grid::TreeType>(source.constTreePtr())) {
    insertMeta(source);setTransform(std::make_shared<openvdb::math::Transform>(source.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid()const override {
    return copyReplacingMetadataAndTransform(*this,std::make_shared<openvdb::math::Transform>(transform()));
  }
  openvdb::GridBase::Ptr copyGrid()override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactNeighborFixture&>(*this).copyGrid());
  }
};
struct OraclePoint {
  V world;
  double radius;
  vdb::NativePointReference ref;
  bool a, b, c;
};
class OpenVdbPointNeighbors : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    using D = points::TypedAttributeArray<double>;
    using I = points::TypedAttributeArray<int64_t>;
    if (!D::isRegistered())
      D::registerType();
    if (!I::isRegistered())
      I::registerType();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-point-neighbors-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    p.gridName = "occupied";
    auto grid = create({{-16.2, 0, 0},
                        {-16.1, 0, 0},
                        {.1, 0, 0},
                        {.2, 0, 0},
                        {4, 0, 0},
                        {32, 0, 0}});
    grid->tree().setActiveState({-16, 0, 0}, false);
    write("source.vdb", {grid});
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), e);
    std::filesystem::remove(directory, e);
  }
  auto path(const std::string &n) const { return directory / n; }
  static std::string bytes(const std::filesystem::path &p) {
    std::ifstream file(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  Grid::Ptr create(const std::vector<V> &positions, double voxel = 1) {
    auto map = openvdb::math::Transform::createLinearTransform(voxel);
    auto grid =
        points::createPointDataGrid<points::NullCodec, Grid>(positions, *map);
    grid->setName("points");
    points::appendAttribute<double>(grid->tree(), "radius", .1);
    points::appendAttribute<int64_t>(grid->tree(), "id", INT64_MAX);
    for (const auto *name : {"A", "B", "C", "empty"})
      points::appendGroup(grid->tree(), name);
    std::vector<Grid::TreeType::LeafNodeType *> leaves;
    for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf)
      leaves.push_back(&*leaf);
    std::sort(leaves.begin(), leaves.end(),
              [](auto *a, auto *b) { return a->origin() < b->origin(); });
    uint64_t ordinal = 0;
    for (auto *leaf : leaves) {
      auto a = leaf->groupWriteHandle("A"), b = leaf->groupWriteHandle("B"),
           c = leaf->groupWriteHandle("C");
      points::AttributeWriteHandle<int64_t> ids(leaf->attributeArray("id"));
      for (auto i = leaf->beginIndexAll(); i; ++i, ++ordinal) {
        a.set(*i, ordinal % 2 == 0);
        b.set(*i, ordinal < 4);
        c.set(*i, ordinal == 2);
        ids.set(*i, INT64_MAX - int64_t(ordinal));
      }
      leaf->attributeArray("id").setHidden(true);
    }
    grid->insertMeta("artist",
                     openvdb::StringMetadata(std::string("raw\0metadata", 12)));
    grid->insertMeta("container:point_empty_note",
                     openvdb::StringMetadata("ordinary custom payload"));
    if (!leaves.empty())
      points::makeDescriptorUnique(grid->tree())
          ->getMetadata()
          .insertMeta("custom:descriptor",
                      openvdb::StringMetadata("native payload"));
    return grid;
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &meta = {}) {
    openvdb::io::File file(path(name).string());
    file.write(grids, meta);
    file.close();
  }
  template <class T>
  typename T::Ptr read(const std::string &name,
                       const std::string &gridName = "points") {
    openvdb::io::File file(path(name).string());
    file.open(false);
    auto grid = openvdb::gridPtrCast<T>(file.readGrid(gridName));
    file.close();
    return grid;
  }
  std::vector<OraclePoint> oracle(const std::string &name = "source.vdb") {
    auto grid = read<Grid>(name);
    using Leaf = Grid::TreeType::LeafNodeType;
    std::vector<const Leaf *> leaves;
    for (auto leaf = grid->tree().cbeginLeaf(); leaf; ++leaf)
      leaves.push_back(&*leaf);
    std::sort(leaves.begin(), leaves.end(),
              [](auto *a, auto *b) { return a->origin() < b->origin(); });
    std::vector<OraclePoint> result;
    uint64_t ordinal = 0;
    for (const auto *leaf : leaves) {
      points::AttributeHandle<openvdb::Vec3f> positions(
          leaf->constAttributeArray("P"));
      points::AttributeHandle<double> radii(
          leaf->constAttributeArray("radius"));
      auto a = leaf->groupHandle("A"), b = leaf->groupHandle("B"),
           c = leaf->groupHandle("C");
      const auto origin = leaf->origin();
      for (auto i = leaf->beginIndexAll(); i; ++i, ++ordinal) {
        const auto world = grid->transform().indexToWorld(
            i.getCoord().asVec3d() + V(positions.get(*i)));
        result.push_back(
            {world,
             radii.get(*i),
             {ordinal, {origin[0], origin[1], origin[2]}, uint32_t(*i)},
             a.get(*i),
             b.get(*i),
             c.get(*i)});
      }
    }
    return result;
  }
  static bool selected(const OraclePoint &r,
                       const vdb::PointGroupSelection &s) {
    auto group = [&](const std::string &n) {
      return n == "A" ? r.a : n == "B" ? r.b : n == "C" ? r.c : false;
    };
    bool any = s.anyOf.empty();
    for (const auto &n : s.anyOf)
      any |= group(n);
    if (!any)
      return false;
    for (const auto &n : s.allOf)
      if (!group(n))
        return false;
    for (const auto &n : s.noneOf)
      if (group(n))
        return false;
    return true;
  }
  void checkOracle(const std::vector<vdb::PointNeighborQuery> &queries,
                   vdb::PointNeighborOptions o = {},
                   const std::string &file = "source.vdb") {
    const auto expected = oracle(file);
    const auto result = vdb::QueryPointNeighbors(path(file), queries, o, {}, p);
    ASSERT_EQ(result.queries.size(), queries.size());
    for (size_t q = 0; q < queries.size(); ++q) {
      std::vector<std::pair<double, uint64_t>> hits;
      for (const auto &r : expected)
        if (selected(r, o.groups)) {
          const auto d = V(queries[q].center.data()) - r.world;
          const double distance = std::hypot(d.x(), d.y(), d.z());
          const double radius =
              o.mode == vdb::PointNeighborMode::ParticleIntersections
                  ? (o.radiusAttribute.empty() ? o.constantRadius : r.radius)
                  : 0;
          if (distance <= queries[q].radius + radius)
            hits.emplace_back(distance, r.ref.ordinal);
        }
      std::sort(hits.begin(), hits.end());
      ASSERT_EQ(result.queries[q].size(), hits.size()) << q;
      for (size_t n = 0; n < hits.size(); ++n) {
        const auto &h = result.queries[q][n];
        const auto &r = expected[hits[n].second];
        EXPECT_DOUBLE_EQ(h.distanceWorld, hits[n].first);
        EXPECT_EQ(h.reference.ordinal, r.ref.ordinal);
        EXPECT_EQ(h.reference.leafOrigin, r.ref.leafOrigin);
        EXPECT_EQ(h.reference.localIndex, r.ref.localIndex);
        EXPECT_EQ(h.positionWorld, (std::array<double, 3>{
                                       r.world.x(), r.world.y(), r.world.z()}));
      }
    }
  }
  auto query(const std::vector<vdb::PointNeighborQuery> &queries,
             vdb::PointNeighborOptions o = {},
             const std::string &file = "source.vdb") {
    return vdb::QueryPointNeighbors(path(file), queries, o, {}, p);
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions p;
};

TEST_F(OpenVdbPointNeighbors,
       InclusiveCenterQueriesNegativeBucketsReferencesAndInactivePoints) {
  const auto before = bytes(path("source.vdb"));
  const auto points = oracle();
  std::vector<vdb::PointNeighborQuery> queries{
      {{0, 0, 0}, 0}, {{0, 0, 0}, 4}, {{-16, 0, 0}, .5}, {{1e100, 0, 0}, 1}};
  for (const auto &r : points)
    queries.push_back({{r.world.x(), r.world.y(), r.world.z()}, 0});
  vdb::PointNeighborOptions o;
  o.includeBuckets = true;
  o.indexVoxelSize = .5;
  checkOracle(queries, o);
  const auto result = query(queries, o);
  EXPECT_EQ(result.selectedCount, 6u);
  ASSERT_EQ(result.levels.size(), 1u);
  std::set<uint64_t> seen;
  for (const auto &bucket : result.buckets) {
    uint64_t previous = 0;
    bool first = true;
    for (const auto &ref : bucket.points) {
      EXPECT_TRUE(seen.insert(ref.ordinal).second);
      if (!first)
        EXPECT_LT(previous, ref.ordinal);
      first = false;
      previous = ref.ordinal;
    }
  }
  EXPECT_EQ(seen.size(), 6u);
  const auto again = query(queries, o);
  EXPECT_EQ(again.buckets.size(), result.buckets.size());
  for (size_t n = 0; n < result.buckets.size(); ++n)
    EXPECT_EQ(again.buckets[n].origin, result.buckets[n].origin);
  EXPECT_EQ(bytes(path("source.vdb")), before);
}

TEST_F(OpenVdbPointNeighbors,
       GroupUnionIntersectionExclusionAndEmptySelection) {
  std::vector<vdb::PointNeighborQuery> queries{{{0, 0, 0}, 100}};
  vdb::PointNeighborOptions o;
  o.groups = {{"A", "B"}, {}, {"C"}};
  checkOracle(queries, o);
  o.groups = {{}, {"A", "B"}, {"C"}};
  checkOracle(queries, o);
  o.groups = {{"A", "A"}};
  checkOracle(queries, o);
  o.groups = {{"empty"}};
  const auto empty = query(queries, o);
  EXPECT_EQ(empty.selectedCount, 0u);
  EXPECT_TRUE(empty.levels.empty());
  EXPECT_TRUE(empty.queries[0].empty());
  o.groups = {{"missing"}};
  EXPECT_THROW((void)query(queries, o), std::invalid_argument);
}

TEST_F(OpenVdbPointNeighbors,
       NativeMultiradiusAtlasMatchesIndependentInclusiveOracle) {
  auto grid = create(
      {{0, 0, 0}, {4, 0, 0}, {8, 0, 0}, {16, 0, 0}, {-.5, 0, 0}, {-8, 0, 0}});
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) {
    points::AttributeWriteHandle<double> radius(leaf->attributeArray("radius"));
    for (auto i = leaf->beginIndexAll(); i; ++i) {
      const double x = i.getCoord().x();
      radius.set(*i, x == 0 ? .1 : std::max(.2, std::abs(x)));
    }
  }
  write("radii.vdb", {grid});
  vdb::PointNeighborOptions o;
  o.mode = vdb::PointNeighborMode::ParticleIntersections;
  o.radiusAttribute = "radius";
  o.includeBuckets = true;
  const std::vector<vdb::PointNeighborQuery> queries{{{0, 0, 0}, 0},
                                                     {{0, 0, 0}, .1},
                                                     {{4, 1, 0}, 1},
                                                     {{-.5, 0, 0}, 0},
                                                     {{20, 0, 0}, 4}};
  checkOracle(queries, o, "radii.vdb");
  const auto result = query(queries, o, "radii.vdb");
  EXPECT_GT(result.levels.size(), 1u);
  EXPECT_LE(result.levels.size(), o.maxAtlasLevels);
  for (uint32_t cap : {1u, 2u, 3u, 4u}) {
    o.maxAtlasLevels = cap;
    checkOracle(queries, o, "radii.vdb");
    EXPECT_LE(query(queries, o, "radii.vdb").levels.size(), cap);
  }
  o.radiusAttribute.clear();
  o.constantRadius = .5;
  checkOracle(queries, o, "radii.vdb");
}

TEST_F(OpenVdbPointNeighbors,
       NativeRoundingPreflightAndAutomaticForwardRecentering) {
  auto grid = create({{2.17, 0, 0}, {2.205, 0, 0}}, .001);
  grid->transform().postTranslate({1e6 + .125, 0, 0});
  write("translated.vdb", {grid});
  vdb::PointNeighborOptions o;
  o.indexVoxelSize = .07;
  o.includeBuckets = true;
  checkOracle({{{1e6 + 2.3, 0, 0}, 1}}, o, "translated.vdb");
  auto result = query({{{1e6 + 2.3, 0, 0}, 1}}, o, "translated.vdb");
  EXPECT_GT(result.indexOriginWorld[0], 1e6);
  EXPECT_EQ(result.selectedCount, 2u);
  // A remote native source coordinate with cancellation in its forward map.
  grid = create({{1e9, 0, 0}});
  auto leaf = grid->tree().beginLeaf();
  points::AttributeWriteHandle<openvdb::Vec3f> offsets(
      leaf->attributeArray("P"));
  offsets.set(0, {.1234567f, -.2f, .3f});
  grid->transform().postTranslate({-1e9, 0, 0});
  write("remote.vdb", {grid});
  const std::array<double, 3> expected{double(.1234567f), double(-.2f),
                                       double(.3f)};
  result = query({{expected, 0}}, o, "remote.vdb");
  ASSERT_EQ(result.queries[0].size(), 1u);
  EXPECT_EQ(result.queries[0][0].positionWorld, expected);
  EXPECT_DOUBLE_EQ(result.queries[0][0].distanceWorld, 0);
}

TEST_F(OpenVdbPointNeighbors,
       FullMetadataNativeAttributesAndFileMetadataStayAvailable) {
  auto grid = read<Grid>("source.vdb");
  points::appendAttribute<double>(grid->tree(), "pair", .25, 2, true);
  openvdb::MetaMap meta;
  meta.insertMeta("file:artist", openvdb::StringMetadata("file payload"));
  write("metadata.vdb", {grid}, meta);
  const auto result = query({}, {}, "metadata.vdb");
  auto value = [](const auto &metadata, const std::string &name) {
    for (const auto &m : metadata)
      if (m.name == name)
        return m.value;
    return std::string{};
  };
  EXPECT_EQ(value(result.gridMetadata, "artist"),
            std::string("raw\0metadata", 12));
  EXPECT_EQ(value(result.fileMetadata, "file:artist"), "file payload");
  EXPECT_EQ(value(result.descriptorMetadata, "custom:descriptor"),
            "native payload");
  bool id = false, pair = false;
  for (const auto &a : result.schema.attributes) {
    if (a.name == "id")
      id = a.anyHidden;
    if (a.name == "pair")
      pair = a.stride == 2;
  }
  EXPECT_TRUE(id);
  EXPECT_TRUE(pair);
  EXPECT_EQ(result.schema.groups.size(), 4u);
}

TEST_F(OpenVdbPointNeighbors, ExplicitWorldOccupancyMaskAndExactInt64Counts) {
  auto grid =
      create({{-.5, 0, 0}, {.5, 0, 0}, {.51, 0, 0}, {8, 0, 0}, {8.1, 0, 0}});
  grid->tree().setActiveState({8, 0, 0}, false);
  openvdb::MetaMap meta;
  meta.insertMeta("file:artist", openvdb::StringMetadata("file payload"));
  write("mask-source.vdb", {grid}, meta);
  const auto before = bytes(path("mask-source.vdb"));
  vdb::IndexedPointMaskOptions o;
  o.voxelSize = 1;
  o.includeCounts = true;
  o.countGridName = "count";
  const auto result = vdb::WriteIndexedPointMask(path("mask-source.vdb"),
                                                 path("mask.vdb"), o, {}, p);
  EXPECT_EQ(result.inputCount, 5u);
  EXPECT_EQ(result.selectedCount, 5u);
  const auto mask = read<openvdb::MaskGrid>("mask.vdb", "occupied");
  const auto count = read<openvdb::Int64Grid>("mask.vdb", "count");
  EXPECT_EQ(mask->activeVoxelCount(), 3u);
  EXPECT_EQ(count->tree().getValue({0, 0, 0}), 1);
  EXPECT_EQ(count->tree().getValue({1, 0, 0}), 2);
  EXPECT_EQ(count->tree().getValue({8, 0, 0}), 2);
  EXPECT_TRUE(mask->tree().isValueOn({0, 0, 0}));
  EXPECT_TRUE(mask->tree().isValueOn({1, 0, 0}));
  EXPECT_TRUE(mask->tree().isValueOn({8, 0, 0}));
  EXPECT_EQ(mask->metaValue<std::string>("artist"),
            std::string("raw\0metadata", 12));
  EXPECT_EQ(mask->metaValue<std::string>("container:point_empty_note"),
            "ordinary custom payload");
  EXPECT_FALSE((*mask)["container:point_count"]);
  EXPECT_EQ(mask->getGridClass(), openvdb::GRID_UNKNOWN);
  openvdb::io::File input(path("mask.vdb").string());
  input.open(false);
  EXPECT_EQ(input.getMetadata()->metaValue<std::string>("file:artist"),
            "file payload");
  input.close();
  EXPECT_EQ(bytes(path("mask-source.vdb")), before);
  o.originWorld = {10, 0, 0};
  o.voxelSize = .5;
  o.includeCounts = false;
  o.countGridName.clear();
  o.groups = {{"A"}};
  const auto selectedResult = vdb::WriteIndexedPointMask(
      path("mask-source.vdb"), path("selected.vdb"), o, {}, p);
  EXPECT_EQ(selectedResult.selectedCount, 3u);
  const auto selectedMask = read<openvdb::MaskGrid>("selected.vdb", "occupied");
  EXPECT_EQ(selectedMask->transform().indexToWorld(V(0)), V(10, 0, 0));
  for (const auto &r : oracle("mask-source.vdb"))
    if (r.a)
      EXPECT_TRUE(selectedMask->tree().isValueOn(
          selectedMask->transform().worldToIndexCellCentered(r.world)));
}

TEST_F(OpenVdbPointNeighbors, EmptyNativeAndValidatedArchiveAreUseful) {
  auto grid = Grid::create();
  grid->setName("points");
  write("empty.vdb", {grid});
  auto result = query({{{0, 0, 0}, 1}}, {}, "empty.vdb");
  EXPECT_EQ(result.selectedCount, 0u);
  EXPECT_TRUE(result.queries[0].empty());
  auto generation = p;
  generation.gridName = "points";
  const auto filtered = vdb::WriteSelectedPoints(
      path("source.vdb"), path("archive.vdb"), {{"empty"}},
      vdb::PointSelectionMode::KeepSelected, {}, generation);
  ASSERT_TRUE(filtered.archivedSchema);
  vdb::PointNeighborOptions n;
  n.groups = {{"A"}};
  result = query({{{0, 0, 0}, 1}}, n, "archive.vdb");
  EXPECT_TRUE(result.archivedSchema);
  EXPECT_EQ(result.schema.groups.size(), 4u);
  vdb::IndexedPointMaskOptions o;
  o.groups = n.groups;
  o.includeCounts = true;
  (void)vdb::WriteIndexedPointMask(path("archive.vdb"), path("empty-mask.vdb"),
                                   o, {}, p);
  const auto mask = read<openvdb::MaskGrid>("empty-mask.vdb", "occupied");
  EXPECT_EQ(mask->tree().leafCount(), 0u);
  EXPECT_FALSE((*mask)["container:point_empty_manifest_v1"]);
  o.groups = {{"missing"}};
  EXPECT_THROW((void)vdb::WriteIndexedPointMask(path("archive.vdb"),
                                                path("bad.vdb"), o, {}, p),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}

TEST_F(OpenVdbPointNeighbors, SelectedFiniteValuesCodecAndMalformedLayouts) {
  auto grid = read<Grid>("source.vdb");
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) {
    points::AttributeWriteHandle<double> r(leaf->attributeArray("radius"));
    const auto a = leaf->groupHandle("A");
    for (auto i = leaf->beginIndexAll(); i; ++i)
      if (!a.get(*i))
        r.set(*i, NAN);
  }
  write("nan-radius.vdb", {grid});
  vdb::PointNeighborOptions o;
  o.mode = vdb::PointNeighborMode::ParticleIntersections;
  o.radiusAttribute = "radius";
  o.groups = {{"A"}};
  EXPECT_NO_THROW((void)query({{{0, 0, 0}, 1}}, o, "nan-radius.vdb"));
  o.groups = {};
  EXPECT_THROW((void)query({}, o, "nan-radius.vdb"), std::invalid_argument);
  o.radiusAttribute = "P";
  EXPECT_THROW((void)query({}, o), std::invalid_argument);
  o.radiusAttribute = "missing";
  EXPECT_THROW((void)query({}, o), std::invalid_argument);
  grid = read<Grid>("source.vdb");
  points::appendAttribute<double>(grid->tree(), "pair", .25, 2, true);
  write("strided-radius.vdb", {grid});
  o.radiusAttribute = "pair";
  EXPECT_THROW((void)query({}, o, "strided-radius.vdb"), std::invalid_argument);
  using Packed = points::TypedAttributeArray<float, points::TruncateCodec>;
  if (!Packed::isRegistered())
    Packed::registerType();
  points::appendAttribute<float, points::TruncateCodec>(grid->tree(), "packed",
                                                        .25f);
  write("packed.vdb", {grid});
  o.radiusAttribute = "packed";
  EXPECT_NO_THROW((void)query({{{0, 0, 0}, 1}}, o, "packed.vdb"));
  grid = read<Grid>("source.vdb");
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) {
    const auto index = leaf->attributeSet().groupIndex("A").first;
    auto attrs = std::make_unique<points::AttributeSet>(leaf->attributeSet());
    attrs->replace(index, std::make_shared<points::GroupAttributeArray>(
                              openvdb::Index(leaf->pointCount()), 2, true));
    leaf->replaceAttributeSet(attrs.release());
  }
  write("bad-group.vdb", {grid});
  o = {};
  o.groups = {{"A"}};
  EXPECT_THROW((void)query({}, o, "bad-group.vdb"), std::invalid_argument);
}

TEST_F(OpenVdbPointNeighbors, PackedPositionCodecAndNearEqualRadiiRemainExact) {
  using PackedP =
      points::TypedAttributeArray<openvdb::Vec3f, points::TruncateCodec>;
  if (!PackedP::isRegistered())
    PackedP::registerType();
  auto grid = create({{0, 0, 0}, {4, 0, 0}, {8, 0, 0}, {16, 0, 0}, {32, 0, 0}});
  auto descriptor = std::make_shared<points::AttributeSet::Descriptor>(
      grid->tree().cbeginLeaf()->attributeSet().descriptor());
  descriptor = descriptor->duplicateDrop({descriptor->find("P")});
  descriptor = descriptor->duplicateAppend("P", PackedP::attributeType());
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) {
    auto attributes = std::make_unique<points::AttributeSet>(
        descriptor, openvdb::Index(leaf->pointCount()));
    for (const auto &[name, index] : leaf->attributeSet().descriptor().map())
      if (name != "P")
        attributes->replace(name, leaf->constAttributeArray(index).copy());
    points::AttributeWriteHandle<openvdb::Vec3f, points::TruncateCodec> p(
        *attributes->get("P"));
    p.collapse({.125f, -.25f, .375f});
    leaf->replaceAttributeSet(attributes.release(), true);
    points::AttributeWriteHandle<double> radii(leaf->attributeArray("radius"));
    for (auto i = leaf->beginIndexAll(); i; ++i) {
      const double x = i.getCoord().x();
      radii.set(*i, x == 0 ? .1 : std::nextafter(x, x == 8 ? 0 : INFINITY));
    }
  }
  write("packed-position.vdb", {grid});
  vdb::PointNeighborOptions o;
  o.mode = vdb::PointNeighborMode::ParticleIntersections;
  o.radiusAttribute = "radius";
  o.includeBuckets = true;
  const auto records = oracle("packed-position.vdb");
  std::vector<vdb::PointNeighborQuery> queries{{{.125, -.25, .375}, 0},
                                               {{0, 0, 0}, .25}};
  for (const auto &r : records) {
    queries.push_back({{r.world.x() - r.radius, r.world.y(), r.world.z()}, 0});
    queries.push_back({{std::nextafter(r.world.x() - r.radius, -INFINITY),
                        r.world.y(), r.world.z()},
                       0});
  }
  checkOracle(queries, o, "packed-position.vdb");
  const auto result = query(queries, o, "packed-position.vdb");
  bool packed = false;
  for (const auto &attribute : result.schema.attributes)
    if (attribute.name == "P")
      packed = attribute.codec == "trnc";
  EXPECT_TRUE(packed);
}

TEST_F(OpenVdbPointNeighbors,
       SharedInputMetadataSchemaTopologyOutputAndWorkCaps) {
  const std::vector<vdb::PointNeighborQuery> queries{{{0, 0, 0}, 100}};
  vdb::PointNeighborOptions o;
  o.maxNeighbors = 2;
  EXPECT_THROW((void)query(queries, o), std::invalid_argument);
  o = {};
  o.maxWork = 1;
  EXPECT_THROW((void)query(queries, o), std::invalid_argument);
  o = {};
  o.maxMetadataBytes = 1;
  EXPECT_THROW((void)query({}, o), std::invalid_argument);
  o = {};
  o.maxQueries = 1;
  EXPECT_THROW((void)query({{{0, 0, 0}, 1}, {{1, 0, 0}, 1}}, o),
               std::invalid_argument);
  auto small = p;
  small.maxInputBytes = 1024;
  EXPECT_THROW((void)vdb::QueryPointNeighbors(path("source.vdb"), queries, {},
                                              {}, small),
               std::invalid_argument);
  small = p;
  small.maxVoxelCount = 512;
  EXPECT_THROW(
      (void)vdb::QueryPointNeighbors(path("source.vdb"), {}, {}, {}, small),
      std::invalid_argument);
  vdb::PointAttributeOptions limits;
  limits.maxPoints = 2;
  EXPECT_THROW(
      (void)vdb::QueryPointNeighbors(path("source.vdb"), {}, {}, {}, p, limits),
      std::invalid_argument);
  auto grid = read<Grid>("source.vdb");
  openvdb::MetaMap fileMetadata;
  fileMetadata.insertMeta("large-file",
                          openvdb::StringMetadata(std::string(4096, 'x')));
  write("file-metadata.vdb", {grid}, fileMetadata);
  o = {};
  o.maxMetadataBytes = 4096;
  EXPECT_THROW((void)query({}, o, "file-metadata.vdb"), std::invalid_argument);
  write("two.vdb", {grid, grid});
  o = {};
  o.maxGrids = 1;
  EXPECT_THROW((void)query({}, o, "two.vdb"), std::invalid_argument);
  EXPECT_THROW(
      (void)vdb::QueryPointNeighbors(path("two.vdb"), {}, {}, "points", p),
      std::invalid_argument);
  EXPECT_EQ(vdb::QueryPointNeighbors(path("two.vdb"), {}, {},
                                     "descriptor:points[1]", p)
                .selectedCount,
            6u);
  vdb::IndexedPointMaskOptions m;
  m.includeCounts = true;
  small = p;
  small.maxVoxelCount = 512;
  EXPECT_THROW((void)vdb::WriteIndexedPointMask(path("source.vdb"),
                                                path("cap.vdb"), m, {}, small),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("cap.vdb")));
}

TEST_F(OpenVdbPointNeighbors,
       InvalidControlsProtectionUnicodeAndImmutableSource) {
  auto before = bytes(path("source.vdb"));
  vdb::PointNeighborOptions o;
  o.indexVoxelSize = 0;
  EXPECT_THROW((void)query({}, o), std::invalid_argument);
  o = {};
  o.radiusAttribute = "radius";
  EXPECT_THROW((void)query({}, o), std::invalid_argument);
  EXPECT_THROW((void)query({{{NAN, 0, 0}, 1}}), std::invalid_argument);
  EXPECT_THROW((void)query({{{0, 0, 0}, -1}}), std::invalid_argument);
  vdb::IndexedPointMaskOptions m;
  EXPECT_THROW((void)vdb::WriteIndexedPointMask(path("source.vdb"),
                                                path("source.vdb"), m, {}, p),
               std::invalid_argument);
  EXPECT_EQ(bytes(path("source.vdb")), before);
  m.includeCounts = true;
  m.countGridName = p.gridName;
  EXPECT_THROW((void)vdb::WriteIndexedPointMask(path("source.vdb"),
                                                path("bad.vdb"), m, {}, p),
               std::invalid_argument);
  const auto source =
                 directory / std::filesystem::path(L"\u70b9\u7fa4-\u00e9.vdb"),
             output =
                 directory / std::filesystem::path(L"\u7d22\u5f15-\u03b4.vdb");
  std::filesystem::copy_file(path("source.vdb"), source);
  EXPECT_EQ(vdb::QueryPointNeighbors(source, {}, {}, {}, p).selectedCount, 6u);
  m = {};
  EXPECT_NO_THROW((void)vdb::WriteIndexedPointMask(source, output, m, {}, p));
  EXPECT_TRUE(std::filesystem::exists(output));
  EXPECT_THROW((void)vdb::WriteIndexedPointMask(source, output, m, {}, p),
               std::invalid_argument);
  EXPECT_EQ(bytes(source), before);
}

TEST_F(OpenVdbPointNeighbors,
       FullVoxelEndpointsNativeAtlasAndTinyTranslatedMask) {
  auto tiny = create({{0, 0, 0}, {2e-6, 0, 0}, {8e-6, 0, 0}});
  for (auto leaf = tiny->tree().beginLeaf(); leaf; ++leaf) {
    points::AttributeHandle<openvdb::Vec3f> positions(
        leaf->constAttributeArray("P"));
    points::AttributeWriteHandle<double> radii(leaf->attributeArray("radius"));
    for (auto i = leaf->beginIndexAll(); i; ++i) {
      const auto x = double(i.getCoord().x()) + positions.get(*i).x();
      radii.set(*i, x < 1e-6 ? 1e-6 : x < 4e-6 ? 2e-6 : 8e-6);
    }
  }
  write("tiny-index.vdb", {tiny});
  vdb::PointNeighborOptions o;
  o.indexVoxelSize = 1e-6;
  o.includeBuckets = true;
  const std::vector<vdb::PointNeighborQuery> queries{
      {{0, 0, 0}, 0}, {{-1e-6, 0, 0}, 0}, {{0, 0, 0}, 4e-6}};
  checkOracle(queries, o, "tiny-index.vdb");
  EXPECT_DOUBLE_EQ(query(queries, o, "tiny-index.vdb").levels[0].voxelSize,
                   1e-6);
  o.mode = vdb::PointNeighborMode::ParticleIntersections;
  o.radiusAttribute = "radius";
  checkOracle(queries, o, "tiny-index.vdb");
  const auto atlas = query(queries, o, "tiny-index.vdb");
  EXPECT_GT(atlas.levels.size(), 1u);
  for (const auto &level : atlas.levels)
    EXPECT_GE(level.voxelSize, 1e-6);
  const V origin(1e6 + .125, -2, 3);
  tiny->transform().postTranslate(origin);
  write("tiny-translated.vdb", {tiny});
  vdb::IndexedPointMaskOptions m;
  m.voxelSize = 1e-6;
  m.originWorld = {origin.x(), origin.y(), origin.z()};
  m.includeCounts = true;
  (void)vdb::WriteIndexedPointMask(path("tiny-translated.vdb"),
                                   path("tiny-mask.vdb"), m, {}, p);
  const auto mask = read<openvdb::MaskGrid>("tiny-mask.vdb", "occupied");
  const auto counts =
      read<openvdb::Int64Grid>("tiny-mask.vdb", "occupied_count");
  EXPECT_EQ(mask->transform().baseMap()->applyMap(V(0)), origin);
  EXPECT_EQ(counts->transform().baseMap()->applyMap(V(0)), origin);
  EXPECT_EQ(mask->transform().baseMap()->applyJacobian({1, 0, 0}),
            V(1e-6, 0, 0));
  EXPECT_EQ(counts->transform().baseMap()->applyJacobian({0, 1, 0}),
            V(0, 1e-6, 0));
  EXPECT_EQ(mask->activeVoxelCount(), 3u);
  for (int x : {0, 2, 8}) {
    EXPECT_TRUE(mask->tree().isValueOn({x, 0, 0}));
    EXPECT_EQ(counts->tree().getValue({x, 0, 0}), 1);
  }
  auto huge = create({{0, 0, 0}, {2e6, 0, 0}, {8e6, 0, 0}});
  write("large-index.vdb", {huge});
  o = {};
  o.indexVoxelSize = 1e6;
  checkOracle({{{0, 0, 0}, 1e6}}, o, "large-index.vdb");
  o.mode = vdb::PointNeighborMode::ParticleIntersections;
  o.constantRadius = 1e6;
  checkOracle({{{0, 0, 0}, 1e6}}, o, "large-index.vdb");
  // Radius normalization is only an atlas partitioning choice. A positive
  // world radius smaller than normalized Double range retains its hit meaning.
  o.constantRadius = std::numeric_limits<double>::denorm_min();
  checkOracle({{{0, 0, 0}, 0}}, o, "large-index.vdb");
}

TEST_F(OpenVdbPointNeighbors, PersistedTinySourceMapPreservesWorldReferencesAndInactivePoints) {
  auto grid=read<Grid>("source.vdb");
  std::stringstream record(std::ios::binary|std::ios::in|std::ios::out);
  openvdb::writeString(record,openvdb::math::UniformScaleTranslateMap::mapType());
  const V origin(.125,-.25,.5);origin.write(record);
  for(double n:{1e-6,1e-6,1e6,1e12,5e5})V(n).write(record);
  record.seekg(0);openvdb::io::setCurrentVersion(record);
  auto transform=std::make_shared<openvdb::math::Transform>();transform->read(record);grid->setTransform(transform);
  for(auto leaf=grid->tree().beginLeaf();leaf;++leaf) {
    points::AttributeWriteHandle<double> radii(leaf->attributeArray("radius"));
    for(auto i=leaf->beginIndexAll();i;++i)radii.set(*i,1e-6*(1+int(*i)%3));
  }
  write("persisted-tiny.vdb",{std::make_shared<ExactNeighborFixture>(*grid)});
  const auto original=bytes(path("persisted-tiny.vdb"));
  const std::vector<vdb::PointNeighborQuery> queries{
      {{origin[0],origin[1],origin[2]},4e-5},
      {{origin[0]+4e-6,origin[1],origin[2]},0},
      {{origin[0]-16e-6,origin[1],origin[2]},1e-6}};
  vdb::PointNeighborOptions o;o.indexVoxelSize=8;
  checkOracle(queries,o,"persisted-tiny.vdb");
  const auto all=query(queries,o,"persisted-tiny.vdb");
  EXPECT_EQ(all.selectedCount,6u);EXPECT_EQ(all.queries[0].size(),6u);
  o.mode=vdb::PointNeighborMode::ParticleIntersections;o.radiusAttribute="radius";o.indexVoxelSize=1e-6;
  checkOracle(queries,o,"persisted-tiny.vdb");
  o.groups.anyOf={"A"};checkOracle(queries,o,"persisted-tiny.vdb");
  EXPECT_EQ(bytes(path("persisted-tiny.vdb")),original);
}
#endif
} // namespace
