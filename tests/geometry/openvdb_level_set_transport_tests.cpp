#include "Container/geometry/OpenVdbLevelSetTransport.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/LevelSetSphere.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbLevelSetTransportAvailability, DisabledFailsBeforeOptionsOrFileAccess) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "availability failure is tested in OFF builds";
#else
  vdb::LevelSetTransportOptions t;
  t.timeStep = std::numeric_limits<double>::quiet_NaN();
  try {
    (void)vdb::WriteAdvectedLevelSet("missing", "missing", "", t);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  try {
    (void)vdb::WriteMorphedLevelSet("missing", "missing", "", t);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbLevelSetTransport : public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{};
    for (int i = 0; i < 100; ++i) {
      directory = std::filesystem::temp_directory_path() /
                  ("container-ls-transport-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                   "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(directory))
        return;
    }
    FAIL() << "fixture directory unavailable";
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &f : std::filesystem::directory_iterator(directory))
      if (f.is_regular_file())
        std::filesystem::remove(f.path(), e);
    std::filesystem::remove(directory, e);
  }
  auto path(const std::string &name) const { return directory / name; }
  auto sphere(double radius = .7, double dx = .1, openvdb::Vec3f center = openvdb::Vec3f(0)) {
    auto g = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(float(radius), center,
                                                                      float(dx), 3.0f);
    g->setName("surface");
    g->insertMeta("artist", openvdb::StringMetadata("preserved"));
    return g;
  }
  void write(const std::string &file, const openvdb::GridPtrVec &grids) {
    openvdb::io::File out(path(file).string());
    out.write(grids);
    out.close();
  }
  auto velocity(openvdb::Vec3d value = openvdb::Vec3d(1, 0, 0)) {
    auto g = openvdb::Vec3DGrid::create(value);
    g->setName("velocity");
    g->setIsInWorldSpace(true);
    g->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    return g;
  }
  auto load(const std::string &name) {
    openvdb::io::File file(path(name).string());
    file.open(false);
    auto g = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("surface"));
    file.close();
    return g;
  }
  double sample(const openvdb::FloatGrid &grid, const openvdb::Vec3d &world) {
    return openvdb::tools::BoxSampler::sample(grid.tree(), grid.worldToIndex(world));
  }
  void quality(const openvdb::FloatGrid &grid, double dx) {
    EXPECT_NEAR(grid.background() / dx, 3, 1e-5);
    double error = 0;
    size_t count = 0;
    for (auto value = grid.cbeginValueOn(); value; ++value) {
      ASSERT_TRUE(std::isfinite(*value));
      EXPECT_LE(std::abs(*value), grid.background() + float(dx * 1e-4));
      if (std::abs(*value) > dx)
        continue;
      openvdb::Vec3d gradient;
      for (int k = 0; k < 3; ++k) {
        auto a = value.getCoord(), b = a;
        ++a[k];
        --b[k];
        gradient[k] = (grid.tree().getValue(a) - grid.tree().getValue(b)) / (2 * dx);
      }
      error += std::abs(gradient.length() - 1);
      ++count;
    }
    ASSERT_GT(count, 20u);
    EXPECT_LT(error / count, .15);
  }
  void unchanged(const openvdb::FloatGrid &a, const openvdb::FloatGrid &b) {
    EXPECT_EQ(a.transform(), b.transform());
    EXPECT_EQ(a.background(), b.background());
    auto x = a.cbeginValueAll(), y = b.cbeginValueAll();
    for (; x && y; ++x, ++y) {
      ASSERT_EQ(x.getCoord(), y.getCoord());
      ASSERT_EQ(x.getLevel(), y.getLevel());
      EXPECT_EQ(x.isValueOn(), y.isValueOn());
      EXPECT_EQ(*x, *y);
    }
    EXPECT_FALSE(x);
    EXPECT_FALSE(y);
  }
};
TEST_F(OpenVdbLevelSetTransport, ConstantVelocityTranslatesBothTimeDirectionsAndPreservesQuality) {
  auto source = sphere();
  source->insertMeta("container:level_set_transport_time", openvdb::StringMetadata("stale"));
  write("source.vdb", {source});
  write("velocity.vdb", {velocity({1, .5, 0})});
  for (int sign : {-1, 1}) {
    vdb::LevelSetTransportOptions t;
    t.timeStep = sign * .2;
    const std::string out = sign < 0 ? "back.vdb" : "forward.vdb";
    const auto result = vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"),
                                                   path(out), t, "surface", "velocity");
    EXPECT_GT(result.cflSteps, 0u);
    EXPECT_GT(result.work, 0u);
    EXPECT_EQ(result.levelSet.gridName, "surface");
    auto grid = load(out);
    const openvdb::Vec3d center(sign * .2, sign * .1, 0);
    EXPECT_NEAR(sample(*grid, center + openvdb::Vec3d(.7, 0, 0)), 0, .035);
    EXPECT_NEAR(sample(*grid, center - openvdb::Vec3d(.7, 0, 0)), 0, .035);
    EXPECT_EQ(grid->metaValue<std::string>("artist"), "preserved");
    EXPECT_DOUBLE_EQ(grid->metaValue<double>("container:level_set_transport_time"), t.timeStep);
    quality(*grid, .1);
  }
  unchanged(*source, *load("source.vdb"));
}
TEST_F(OpenVdbLevelSetTransport, FloatVelocityAndReflectedRotatedSourceUseWorldComponents) {
  auto source = sphere();
  auto transform = source->transform().copy();
  transform->postScale(openvdb::Vec3d(-1, 1, 1));
  transform->postRotate(.4, openvdb::math::Z_AXIS);
  transform->postTranslate({2, -1, .5});
  source->setTransform(transform);
  auto wind = openvdb::Vec3SGrid::create(openvdb::Vec3f(.5, 0, 0));
  wind->setName("velocity");
  wind->setIsInWorldSpace(true);
  wind->setVectorType(openvdb::VEC_INVARIANT);
  auto wmap = openvdb::math::Transform::createLinearTransform(.7);
  wmap->postTranslate({-3, 2, 1});
  wind->setTransform(wmap);
  write("source.vdb", {source});
  write("velocity.vdb", {wind});
  vdb::LevelSetTransportOptions t;
  t.timeStep = .2;
  (void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"), path("out.vdb"), t);
  auto grid = load("out.vdb");
  EXPECT_EQ(grid->transform(), *transform);
  EXPECT_NEAR(sample(*grid, {2.8, -1, .5}), 0, .04);
  quality(*grid, .1);
}
TEST_F(OpenVdbLevelSetTransport, SpatialRotationMovesAnOffCenterSphere) {
  auto source = sphere(.3, .075, {.5, 0, 0});
  auto wind = velocity({0, 0, 0});
  wind->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  for (int z = -12; z <= 12; ++z)
    for (int y = -12; y <= 12; ++y)
      for (int x = -12; x <= 12; ++x)
        wind->tree().setValue({x, y, z}, {-.1 * y, .1 * x, 0});
  write("source.vdb", {source});
  write("velocity.vdb", {wind});
  vdb::LevelSetTransportOptions t;
  t.timeStep = .4;
  (void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"), path("out.vdb"), t);
  auto grid = load("out.vdb");
  const openvdb::Vec3d center(.5 * std::cos(.4), .5 * std::sin(.4), 0);
  EXPECT_NEAR(sample(*grid, center + openvdb::Vec3d(.3, 0, 0)), 0, .035);
  EXPECT_NEAR(sample(*grid, center - openvdb::Vec3d(0, .3, 0)), 0, .035);
  quality(*grid, .075);
}
TEST_F(OpenVdbLevelSetTransport, LargeIndexOriginIsConditionedWithoutChangingWorldShape) {
  auto source = openvdb::FloatGrid::create(3);
  source->setName("surface");
  source->setGridClass(openvdb::GRID_LEVEL_SET);
  auto map = openvdb::math::Transform::createLinearTransform(1);
  map->postTranslate({-1e9, 0, 0});
  source->setTransform(map);
  for (int z = -5; z <= 5; ++z)
    for (int y = -5; y <= 5; ++y)
      for (int x = -5; x <= 5; ++x) {
        const float phi = float(std::sqrt(double(x * x + y * y + z * z)) - 2);
        if (std::abs(phi) < 3)
          source->tree().setValue({1000000000 + x, y, z}, phi);
      }
  write("source.vdb", {source});
  write("velocity.vdb", {velocity()});
  vdb::LevelSetTransportOptions t;
  t.timeStep = .25;
  (void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"), path("out.vdb"), t);
  auto grid = load("out.vdb");
  EXPECT_EQ(grid->transform(), *map);
  EXPECT_NEAR(sample(*grid, {2.25, 0, 0}), 0, .17);
}
TEST_F(OpenVdbLevelSetTransport, MorphApproachesTargetWithPreservedBandAndDistanceQuality) {
  auto source = sphere(.6), target = sphere(.8);
  write("source.vdb", {source});
  write("target.vdb", {target});
  double previousError = 1;
  for (double time : {.5, 1.5}) {
    const auto out = time < 1 ? "half.vdb" : "later.vdb";
    vdb::LevelSetTransportOptions t;
    t.timeStep = time;
    const auto result =
        vdb::WriteMorphedLevelSet(path("source.vdb"), path("target.vdb"), path(out), t);
    EXPECT_GT(result.cflSteps, 0u);
    auto grid = load(out);
    const double radius = .8 - .2 * std::exp(-time);
    EXPECT_NEAR(sample(*grid, {radius, 0, 0}), 0, .035);
    const double targetError = std::abs(sample(*grid, {.8, 0, 0}));
    EXPECT_LT(targetError, previousError);
    previousError = targetError;
    EXPECT_EQ(grid->transform(), source->transform());
    quality(*grid, .1);
  }
  unchanged(*source, *load("source.vdb"));
  unchanged(*target, *load("target.vdb"));
}
TEST_F(OpenVdbLevelSetTransport, ZeroTimeAndZeroSpeedAreExactAndIgnoreGenerationOptions) {
  auto source = sphere();
  write("source.vdb", {source});
  write("velocity.vdb", {velocity()});
  write("target.vdb", {sphere(.8)});
  vdb::ProcessingOptions p;
  p.voxelSize = std::numeric_limits<double>::quiet_NaN();
  p.halfWidth = -1;
  p.maxTriangles = 0;
  vdb::LevelSetTransportOptions t;
  t.timeStep = 0;
  EXPECT_EQ(vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"), path("zero.vdb"),
                                       t, {}, {}, p)
                .cflSteps,
            0u);
  EXPECT_EQ(vdb::WriteMorphedLevelSet(path("source.vdb"), path("target.vdb"), path("morph.vdb"), t,
                                      {}, {}, p)
                .cflSteps,
            0u);
  unchanged(*source, *load("zero.vdb"));
  unchanged(*source, *load("morph.vdb"));
  write("still.vdb", {velocity({0, 0, 0})});
  t.timeStep = 1;
  EXPECT_EQ(
      vdb::WriteAdvectedLevelSet(path("source.vdb"), path("still.vdb"), path("still-out.vdb"), t)
          .cflSteps,
      0u);
  unchanged(*source, *load("still-out.vdb"));
  EXPECT_EQ(vdb::WriteMorphedLevelSet(path("source.vdb"), path("source.vdb"), path("same.vdb"), t)
                .cflSteps,
            0u);
  unchanged(*source, *load("same.vdb"));
}
TEST_F(OpenVdbLevelSetTransport, LimitsRejectWithoutPublishingOrOverwritingInputs) {
  write("source.vdb", {sphere()});
  write("velocity.vdb", {velocity()});
  write("target.vdb", {sphere(.8)});
  vdb::LevelSetTransportOptions t;
  t.timeStep = 1;
  t.maxCflSteps = 1;
  EXPECT_THROW((void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"),
                                                path("cfl.vdb"), t),
               std::exception);
  EXPECT_THROW((void)vdb::WriteMorphedLevelSet(path("source.vdb"), path("target.vdb"),
                                               path("morph-cfl.vdb"), t),
               std::exception);
  t = {};
  t.maxWork = 1;
  EXPECT_THROW((void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"),
                                                path("work.vdb"), t),
               std::exception);
  vdb::ProcessingOptions p;
  p.maxVoxelCount = 512;
  t = {};
  EXPECT_THROW((void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"),
                                                path("vox.vdb"), t, {}, {}, p),
               std::exception);
  p = {};
  p.maxInputBytes = 1024;
  EXPECT_THROW((void)vdb::WriteMorphedLevelSet(path("source.vdb"), path("target.vdb"),
                                               path("bytes.vdb"), t, {}, {}, p),
               std::exception);
  t.timeStep = -.1;
  EXPECT_THROW((void)vdb::WriteMorphedLevelSet(path("source.vdb"), path("target.vdb"),
                                               path("reverse.vdb"), t),
               std::exception);
  t = {};
  t.timeStep = 3601;
  EXPECT_THROW((void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"),
                                                path("time.vdb"), t),
               std::exception);
  t = {};
  EXPECT_THROW((void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"),
                                                path("source.vdb"), t),
               std::exception);
  for (const char *name :
       {"cfl.vdb", "morph-cfl.vdb", "work.vdb", "vox.vdb", "bytes.vdb", "reverse.vdb", "time.vdb"})
    EXPECT_FALSE(std::filesystem::exists(path(name)));
  EXPECT_EQ(load("source.vdb")->metaValue<std::string>("artist"), "preserved");
}
TEST_F(OpenVdbLevelSetTransport, InvalidTrackerOptionsAreRejectedBeforePublication) {
  write("source.vdb", {sphere()});
  write("velocity.vdb", {velocity()});
  std::array<vdb::LevelSetTransportOptions, 6> bad;
  bad[0].normalizationIterations = 0;
  bad[1].normalizationIterations = 9;
  bad[2].maxCflSteps = 0;
  bad[3].maxCflSteps = 4097;
  bad[4].maxWork = 0;
  bad[5].timeStep = std::numeric_limits<double>::quiet_NaN();
  for (const auto &options : bad)
    EXPECT_THROW((void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"),
                                                  path("out.vdb"), options),
                 std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbLevelSetTransport, RejectsInvalidFramesTransformsStoredSignsAndNonfiniteValues) {
  auto source = sphere(), target = sphere(.8);
  write("source.vdb", {source});
  write("target.vdb", {target});
  auto wind = velocity();
  wind->setGridClass(openvdb::GRID_STAGGERED);
  write("mac.vdb", {wind});
  EXPECT_THROW(
      (void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("mac.vdb"), path("out.vdb")),
      std::exception);
  wind = velocity();
  wind->tree().setValue({0, 0, 0}, {NAN, 0, 0});
  write("nan.vdb", {wind});
  EXPECT_THROW(
      (void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("nan.vdb"), path("out.vdb")),
      std::exception);
  wind = velocity();
  write("velocity.vdb", {wind});
  source->tree().setValueOff({20, 0, 0}, -source->background());
  write("negative.vdb", {source});
  EXPECT_THROW(
      (void)vdb::WriteAdvectedLevelSet(path("negative.vdb"), path("velocity.vdb"), path("out.vdb")),
      std::exception);
  source = sphere();
  auto map = source->transform().copy();
  map->postScale({1, 2, 1});
  source->setTransform(map);
  write("nonuniform.vdb", {source});
  EXPECT_THROW((void)vdb::WriteAdvectedLevelSet(path("nonuniform.vdb"), path("velocity.vdb"),
                                                path("out.vdb")),
               std::exception);
  map = target->transform().copy();
  map->postTranslate({.1, 0, 0});
  target->setTransform(map);
  write("different.vdb", {target});
  EXPECT_THROW(
      (void)vdb::WriteMorphedLevelSet(path("source.vdb"), path("different.vdb"), path("out.vdb")),
      std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbLevelSetTransport, RejectsUnsupportedTypesCollidingDescriptorsAndStreamingFiles) {
  auto doubles = openvdb::DoubleGrid::create(.3);
  doubles->setName("double");
  doubles->setGridClass(openvdb::GRID_LEVEL_SET);
  write("double.vdb", {doubles});
  write("velocity.vdb", {velocity()});
  EXPECT_THROW(
      (void)vdb::WriteAdvectedLevelSet(path("double.vdb"), path("velocity.vdb"), path("out.vdb")),
      std::exception);
  auto a = sphere(), b = sphere(.8), c = sphere(.9);
  c->setName("surface[0]");
  write("collision.vdb", {a, b, c});
  EXPECT_THROW((void)vdb::WriteAdvectedLevelSet(path("collision.vdb"), path("velocity.vdb"),
                                                path("out.vdb")),
               std::exception);
  {
    std::ofstream out(path("stream.vdb"), std::ios::binary);
    openvdb::io::Stream stream(out);
    stream.write(openvdb::GridPtrVec{a});
  }
  EXPECT_THROW(
      (void)vdb::WriteAdvectedLevelSet(path("stream.vdb"), path("velocity.vdb"), path("out.vdb")),
      std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbLevelSetTransport, PrecisionAndUnresolvedNonzeroMotionFailExplicitly) {
  auto source = sphere();
  auto map = source->transform().copy();
  map->postTranslate({1e15, 0, 0});
  source->setTransform(map);
  write("huge.vdb", {source});
  write("velocity.vdb", {velocity()});
  EXPECT_THROW(
      (void)vdb::WriteAdvectedLevelSet(path("huge.vdb"), path("velocity.vdb"), path("out.vdb")),
      std::exception);
  write("source.vdb", {sphere()});
  write("tiny.vdb", {velocity({1e-300, 0, 0})});
  EXPECT_THROW(
      (void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("tiny.vdb"), path("out.vdb")),
      std::exception);
  auto wind = velocity();
  auto transform = openvdb::math::Transform::createLinearTransform(1e6);
  transform->postTranslate({1e15, 0, 0});
  wind->setTransform(transform);
  wind->tree().setValue({-1000000000, 0, 0}, {0, 0, 0});
  write("index.vdb", {wind});
  EXPECT_THROW(
      (void)vdb::WriteAdvectedLevelSet(path("source.vdb"), path("index.vdb"), path("out.vdb")),
      std::exception);
  write("target.vdb", {sphere(.8)});
  vdb::LevelSetTransportOptions tiny;
  tiny.timeStep = 1e-30;
  EXPECT_THROW((void)vdb::WriteMorphedLevelSet(path("source.vdb"), path("target.vdb"),
                                               path("out.vdb"), tiny),
               std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbLevelSetTransport, WorldVoxelScalingDoesNotChangeNormalizedTransport) {
  for (double dx : {1e-4, 1e6}) {
    SCOPED_TRACE(dx);
    auto source = sphere(2, 1);
    for (auto value = source->beginValueAll(); value; ++value) {
      const bool on = value.isValueOn();
      value.setValue(float(double(*value) * dx));
      value.setActiveState(on);
    }
    source->tree().root().setBackground(float(3 * dx), false);
    source->setTransform(openvdb::math::Transform::createLinearTransform(dx));
    write("source.vdb", {source});
    write("velocity.vdb", {velocity({dx, 0, 0})});
    vdb::LevelSetTransportOptions t;
    t.timeStep = .25;
    const auto name = dx < 1 ? "small.vdb" : "large.vdb";
    const auto result =
        vdb::WriteAdvectedLevelSet(path("source.vdb"), path("velocity.vdb"), path(name), t);
    auto grid = load(name);
    EXPECT_NEAR(result.levelSet.voxelSize, dx, dx * 1e-12);
    EXPECT_NEAR(sample(*grid, {2.25 * dx, 0, 0}) / dx, 0, .17);
    quality(*grid, dx);
  }
}
#endif
} // namespace
