#include "Container/geometry/OpenVdbLevelSetRebuild.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/LevelSetSphere.h>
#endif
namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbLevelSetRebuildAvailability, DisabledPrecedesOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::LevelSetRebuildOptions o;
  o.exteriorWidth = 0;
  try {
    (void)vdb::WriteRebuiltLevelSet("missing", "missing/out", o, {},
                                    {.maxInputBytes = 0});
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = openvdb::Vec3d;
template <class Grid> struct ExactFixture : Grid {
  explicit ExactFixture(const Grid &g)
      : Grid(openvdb::ConstPtrCast<typename Grid::TreeType>(g.constTreePtr())) {
    this->insertMeta(g);
    this->setTransform(
        std::make_shared<openvdb::math::Transform>(g.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, std::make_shared<openvdb::math::Transform>(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactFixture &>(*this).copyGrid());
  }
};
class OpenVdbLevelSetRebuild : public ::testing::Test {
protected:
  std::filesystem::path directory;
  vdb::ProcessingOptions p;
  vdb::LevelSetRebuildOptions o;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-rebuild-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    p.gridName = "rebuilt";
    p.maxInputBytes = 1024ull * 1024 * 1024;
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), e);
    std::filesystem::remove(directory, e);
  }
  auto path(const std::string &s) const { return directory / s; }
  static std::string bytes(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
  }
  template <class Grid> auto sphere(double radius = .6, double h = .1) {
    auto g = openvdb::tools::createLevelSetSphere<Grid>(
        float(radius), openvdb::Vec3f(0), float(h), 3);
    g->setName("source");
    g->insertMeta("artist",
                  openvdb::StringMetadata(std::string("raw\0payload", 11)));
    return g;
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &meta = {}, bool stats = true) {
    openvdb::io::File f(path(name).string());
    f.setGridStatsMetadataEnabled(stats);
    f.write(grids, meta);
    f.close();
  }
  template <class Grid> auto read(const std::string &name) {
    openvdb::io::File f(path(name).string());
    f.open(false);
    auto g = openvdb::gridPtrCast<Grid>(f.readGrid("rebuilt"));
    f.close();
    return g;
  }
  auto run(const std::string &input = "source.vdb",
           const std::string &output = "out.vdb") {
    return vdb::WriteRebuiltLevelSet(path(input), path(output), o, {}, p);
  }
  void failure(const std::string &input = "source.vdb") {
    static int n = 0;
    const auto output = "failure-" + std::to_string(++n) + ".vdb";
    EXPECT_THROW((void)run(input, output), std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(output)));
  }
  static std::array<double, 16> matrix(double h = 1) {
    return {h, 0, 0, 0, 0, h, 0, 0, 0, 0, h, 0, 0, 0, 0, 1};
  }
};
TEST_F(OpenVdbLevelSetRebuild,
       NativeFloatSphereWorldDistancesAsymmetricBandsAndImmutableMetadata) {
  auto source = sphere<openvdb::FloatGrid>();
  openvdb::MetaMap meta;
  meta.insertMeta("scene", openvdb::Int64Metadata(9007199254740993ll));
  write("source.vdb", {source}, meta);
  const auto before = bytes(path("source.vdb"));
  o.exteriorWidth = 5;
  o.interiorWidth = 2;
  const auto r = run();
  EXPECT_EQ(r.sourceValueType, "float");
  EXPECT_EQ(r.effectiveExteriorWidth, 5);
  EXPECT_EQ(r.effectiveInteriorWidth, 2);
  EXPECT_FALSE(r.diagnostics.empty());
  const auto out = read<openvdb::FloatGrid>("out.vdb");
  ASSERT_TRUE(out);
  EXPECT_EQ(out->getGridClass(), openvdb::GRID_LEVEL_SET);
  EXPECT_FALSE(out->saveFloatAsHalf());
  EXPECT_NEAR(out->background(), .5, 1e-6);
  EXPECT_NEAR(out->tree().getValue({0, 0, 0}), -.2, 1e-6);
  EXPECT_NEAR(out->tree().getValue({7, 0, 0}), .1, .03);
  EXPECT_EQ(out->transform().baseMap()->applyJacobian({1, 0, 0}),
            source->transform().baseMap()->applyJacobian({1, 0, 0}));
  EXPECT_EQ(out->metaValue<std::string>("artist"),
            std::string("raw\0payload", 11));
  openvdb::io::File f(path("out.vdb").string());
  f.open(false);
  EXPECT_EQ(f.getMetadata()->metaValue<int64_t>("scene"), 9007199254740993ll);
  f.close();
  EXPECT_EQ(bytes(path("source.vdb")), before);
  EXPECT_THROW((void)run(), std::exception);
  EXPECT_EQ(bytes(path("source.vdb")), before);
}
TEST_F(OpenVdbLevelSetRebuild,
       DoubleExplicitIsovalueAvoidsFloatThresholdNarrowing) {
  auto source = sphere<openvdb::DoubleGrid>();
  constexpr double iso = 9007199254740992.;
  // The represented contour is a box with exact integer-value offsets around
  // an isovalue above Float precision; the API must not narrow that threshold.
  source = openvdb::DoubleGrid::create(iso + 4);
  source->setName("source");
  for (int x = -4; x <= 4; ++x)
    for (int y = -4; y <= 4; ++y)
      for (int z = -4; z <= 4; ++z)
        source->tree().setValue(
            {x, y, z},
            iso + 2 * std::max({std::abs(x), std::abs(y), std::abs(z)}) - 4);
  write("source.vdb", {source});
  o.isovalue = iso;
  const auto r = run();
  EXPECT_EQ(r.sourceValueType, "double");
  auto out = read<openvdb::DoubleGrid>("out.vdb");
  ASSERT_TRUE(out);
  EXPECT_LT(out->tree().getValue({0, 0, 0}), 0);
  EXPECT_NEAR(out->tree().getValue({3, 0, 0}), 1, .3);
}
TEST_F(OpenVdbLevelSetRebuild,
       RotatedReflectedTranslatedTargetUsesPublicColumnVectorMatrix) {
  write("source.vdb", {sphere<openvdb::DoubleGrid>()});
  const double angle = .3, c = std::cos(angle) * .1, s = std::sin(angle) * .1;
  auto target = matrix(.1);
  target[0] = c;
  target[1] = -s;
  target[4] = s;
  target[5] = c;
  target[10] = -.1;
  target[3] = 1e6 + .125;
  target[7] = -2;
  target[11] = 3;
  o.targetIndexToWorld = target;
  const auto r = run();
  const auto out = read<openvdb::DoubleGrid>("out.vdb");
  ASSERT_TRUE(out);
  EXPECT_NEAR(r.grid.voxelSize, .1, 1e-12);
  const auto map = out->transform().baseMap();
  EXPECT_EQ(map->applyMap(V(0)), V(target[3], target[7], target[11]));
  EXPECT_EQ(map->applyJacobian({1, 0, 0}), V(target[0], target[4], target[8]));
  EXPECT_EQ(map->applyJacobian({0, 0, 1}), V(target[2], target[6], target[10]));
  const openvdb::Coord middle(r.targetIndexOrigin[0], r.targetIndexOrigin[1],
                              r.targetIndexOrigin[2]);
  EXPECT_LT(out->tree().getValue(middle), 0);
  for (auto it = out->cbeginValueOn(); it; ++it) {
    const V world = out->transform().indexToWorld(it.getCoord());
    const double expected = std::hypot(world.x(), world.y(), world.z()) - .6;
    if (std::abs(expected) < .15)
      EXPECT_NEAR(*it, expected, .05);
  }
}
TEST_F(OpenVdbLevelSetRebuild,
       SmallNativeVoxelMapsAndTinyDoubleScalarGradientsRemainUsable) {
  auto g = sphere<openvdb::DoubleGrid>(6, 1);
  for (auto it = g->beginValueAll(); it; ++it)
    it.setValue(double(*it) * 1e-6);
  std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
  openvdb::writeString(record,
                       openvdb::math::UniformScaleTranslateMap::mapType());
  V(0).write(record);
  for (double v : {1e-6, 1e-6, 1e6, 1e12, 5e5})
    V(v).write(record);
  record.seekg(0);
  openvdb::io::setCurrentVersion(record);
  g->transform().read(record);
  write("source.vdb",
        {std::make_shared<ExactFixture<openvdb::DoubleGrid>>(*g)});
  const auto r = run();
  auto out = read<openvdb::DoubleGrid>("out.vdb");
  EXPECT_EQ(r.grid.voxelSize, 1e-6);
  EXPECT_EQ(out->transform().baseMap()->applyJacobian({1, 0, 0}),
            V(1e-6, 0, 0));
  EXPECT_NEAR(out->tree().getValue({7, 0, 0}), 1e-6, 3e-7);
  auto tiny = sphere<openvdb::DoubleGrid>();
  for (auto it = tiny->beginValueAll(); it; ++it)
    it.setValue(double(*it) * 1e-200);
  tiny->tree().root().setBackground(3e-201, false);
  write("tiny.vdb", {tiny});
  EXPECT_NO_THROW((void)run("tiny.vdb", "tiny-out.vdb"));
}
TEST_F(OpenVdbLevelSetRebuild,
       OpenPlanesEmptyFieldsAndLostDynamicRangesRejectWithoutOutput) {
  auto plane = openvdb::DoubleGrid::create(4);
  plane->setName("source");
  for (int x = -3; x <= 3; ++x)
    for (int y = -3; y <= 3; ++y)
      for (int z = -3; z <= 3; ++z)
        plane->tree().setValue({x, y, z}, double(x));
  write("source.vdb", {plane});
  failure();
  auto empty = openvdb::DoubleGrid::create(1);
  empty->setName("source");
  write("empty.vdb", {empty});
  failure("empty.vdb");
  auto lost = openvdb::DoubleGrid::create(DBL_MAX);
  lost->setName("source");
  for (int x = -2; x <= 2; ++x)
    for (int y = -2; y <= 2; ++y)
      for (int z = -2; z <= 2; ++z)
        lost->tree().setValue({x, y, z},
                              x == 0 && y == 0 && z == 0
                                  ? -std::numeric_limits<double>::denorm_min()
                                  : DBL_MAX);
  write("lost.vdb", {lost});
  failure("lost.vdb");
  o.isovalue = -DBL_MAX;
  failure("lost.vdb");
  o.isovalue = 0;
  lost->tree().setValueOff({4, 0, 0}, NAN);
  write("nonfinite.vdb", {lost});
  failure("nonfinite.vdb");
}
TEST_F(OpenVdbLevelSetRebuild,
       OptionsTransformsNativeIndicesAndSharedBudgetsReject) {
  write("source.vdb", {sphere<openvdb::DoubleGrid>()});
  o.exteriorWidth = 0;
  failure();
  o = {};
  o.interiorWidth = 65;
  failure();
  o = {};
  o.maxWork = 1;
  failure();
  o = {};
  o.maxMetadataBytes = 1;
  failure();
  o = {};
  p.maxTriangles = 1;
  failure();
  p.maxTriangles = 1'000'000;
  p.maxVoxelCount = 512;
  failure();
  p.maxVoxelCount = 16'000'000;
  p.maxInputBytes = 1024;
  failure();
  p.maxInputBytes = 1024ull * 1024 * 1024;
  auto m = matrix(.1);
  m[5] = .2;
  o.targetIndexToWorld = m;
  failure();
  m = matrix(.1);
  m[1] = .02;
  o.targetIndexToWorld = m;
  failure();
  m = matrix(.1);
  m[12] = 1;
  o.targetIndexToWorld = m;
  failure();
  m = matrix(.1);
  m[3] = 1e300;
  o.targetIndexToWorld = m;
  failure();
}
TEST_F(OpenVdbLevelSetRebuild,
       UnicodePathsMetadataCapsSelectionAndImmutablePublication) {
  auto source = sphere<openvdb::DoubleGrid>();
  write("source.vdb", {source});
  const auto original = bytes(path("source.vdb"));
  const auto unicode =
                 directory / std::filesystem::path(L"\u91cd\u5efa-\u00e9.vdb"),
             output =
                 directory / std::filesystem::path(L"\u66f2\u9762-\u03b4.vdb");
  std::filesystem::copy_file(path("source.vdb"), unicode);
  EXPECT_NO_THROW((void)vdb::WriteRebuiltLevelSet(unicode, output, o, {}, p));
  EXPECT_EQ(bytes(unicode), original);
  EXPECT_THROW((void)vdb::WriteRebuiltLevelSet(unicode, output, o, {}, p),
               std::exception);
  openvdb::MetaMap meta;
  meta.insertMeta("large", openvdb::StringMetadata(std::string(5000, 'x')));
  write("large-meta.vdb", {source}, meta);
  o.maxMetadataBytes = 1000;
  failure("large-meta.vdb");
  o = {};
  write("duplicates.vdb", {source, source->deepCopy()});
  EXPECT_THROW((void)vdb::WriteRebuiltLevelSet(path("duplicates.vdb"),
                                               path("dup.vdb"), o, "source", p),
               std::exception);
  EXPECT_NO_THROW((void)vdb::WriteRebuiltLevelSet(path("duplicates.vdb"),
                                                  path("descriptor.vdb"), o,
                                                  "descriptor:source[1]", p));
}
TEST_F(OpenVdbLevelSetRebuild, FogContourAndFinerTargetRestoreDistanceUnits) {
  auto fog = openvdb::FloatGrid::create(0);
  fog->setName("density");
  fog->setGridClass(openvdb::GRID_FOG_VOLUME);
  fog->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  for (int x = -8; x <= 8; ++x)
    for (int y = -8; y <= 8; ++y)
      for (int z = -8; z <= 8; ++z) {
        const double density =
            std::max(0., 1 - std::hypot(double(x), double(y), double(z)) / 8);
        if (density > 0)
          fog->tree().setValue({x, y, z}, float(density));
      }
  write("source.vdb", {fog});
  o.isovalue = .5;
  o.targetIndexToWorld = matrix(.05);
  o.exteriorWidth = 4.25;
  o.interiorWidth = 3.5;
  const auto r = run();
  const auto out = read<openvdb::FloatGrid>("out.vdb");
  EXPECT_NEAR(out->background(), .2125, 1e-6);
  EXPECT_LT(out->tree().getValue({0, 0, 0}), 0);
  EXPECT_NEAR(out->tree().getValue({10, 0, 0}), .1, .03);
  EXPECT_EQ(r.effectiveExteriorWidth, 4.25);
  EXPECT_EQ(r.effectiveInteriorWidth, 3.5);
}
TEST_F(OpenVdbLevelSetRebuild, RemoteNativeIndicesAreConditionedBeforeMeshing) {
  const auto original = sphere<openvdb::DoubleGrid>();
  auto shifted = openvdb::DoubleGrid::create(original->background());
  shifted->setName("source");
  constexpr int32_t offset = 1'000'000'000;
  shifted->setTransform(
      std::make_shared<openvdb::math::Transform>(original->transform()));
  const auto displacement =
      original->transform().baseMap()->applyJacobian(V(offset, 0, 0));
  shifted->transform().postTranslate(-displacement);
  const auto box = original->evalActiveVoxelBoundingBox();
  for (int x = box.min().x(); x <= box.max().x(); ++x)
    for (int y = box.min().y(); y <= box.max().y(); ++y)
      for (int z = box.min().z(); z <= box.max().z(); ++z) {
        double value;
        const bool active = original->tree().probeValue({x, y, z}, value);
        if (active)
          shifted->tree().setValue({offset + x, y, z}, value);
        else if (value != original->background())
          shifted->tree().setValueOff({offset + x, y, z}, value);
      }
  write("source.vdb",
        {std::make_shared<ExactFixture<openvdb::DoubleGrid>>(*shifted)});
  const auto r = run();
  const auto out = read<openvdb::DoubleGrid>("out.vdb");
  EXPECT_EQ(r.sourceIndexOrigin[0], offset);
  EXPECT_EQ(r.targetIndexOrigin[0], offset);
  EXPECT_NEAR(out->tree().getValue({offset + 7, 0, 0}), .1, .03);
  EXPECT_EQ(out->transform().baseMap()->applyMap(V(0)),
            shifted->transform().baseMap()->applyMap(V(0)));
}
TEST_F(OpenVdbLevelSetRebuild,
       NativeFullBuffersAndAllScannedMetadataShareCaps) {
  auto g = sphere<openvdb::DoubleGrid>();
  g->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                openvdb::Int64Metadata(1));
  g->insertMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT,
                openvdb::Int64Metadata(g->activeVoxelCount()));
  write("underreported.vdb", {g}, {}, false);
  const auto full = openvdb::tools::memUsageIfLoaded(g->tree(), false);
  p.maxInputBytes = std::filesystem::file_size(path("underreported.vdb")) +
                    4 * 1024 * 1024 + full / 2 + 128 * 1024;
  try {
    (void)run("underreported.vdb");
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("before leaf materialization"),
              std::string::npos);
  }
  p.maxInputBytes = 1024ull * 1024 * 1024;
  auto unused = openvdb::Int32Grid::create(1);
  unused->setName("unused");
  unused->insertMeta("large", openvdb::StringMetadata(std::string(6000, 'x')));
  write("scanned.vdb", {g, unused});
  o.maxMetadataBytes = 10000;
  failure("scanned.vdb");
  o = {};
  write("missing-stats.vdb", {sphere<openvdb::DoubleGrid>()}, {}, false);
  failure("missing-stats.vdb");
}
TEST_F(OpenVdbLevelSetRebuild, MandatoryDelayedLoadingHasActionableFailure) {
  write("source.vdb", {sphere<openvdb::DoubleGrid>()});
  struct Environment {
    std::string saved;
    bool present = false;
    Environment() {
      if (const auto *s = std::getenv("OPENVDB_DISABLE_DELAYED_LOAD")) {
        present = true;
        saved = s;
      }
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", "1");
#else
      setenv("OPENVDB_DISABLE_DELAYED_LOAD", "1", 1);
#endif
    }
    ~Environment() {
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", saved.c_str());
#else
      if (present)
        setenv("OPENVDB_DISABLE_DELAYED_LOAD", saved.c_str(), 1);
      else
        unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
    }
  } environment;
  try {
    (void)run();
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("delayed loading"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbLevelSetRebuild,
       InternalStoredTileTraversalIsPreflightedBeforeValidation) {
  auto grid = openvdb::DoubleGrid::create(1);
  grid->setName("source");
  for (int x = 1; x <= 3; ++x)
    for (int y = 1; y <= 3; ++y)
      for (int z = 1; z <= 3; ++z)
        grid->tree().setValue({x, y, z}, x == 2 && y == 2 && z == 2 ? -1. : 1.);
  ASSERT_EQ(grid->tree().leafCount(), 1u);
  write("source.vdb", {grid});
  o.maxWork = 512;
  try {
    (void)run();
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("source validation"),
              std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
#endif
} // namespace
