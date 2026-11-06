#include "Container/geometry/OpenVdbMultiResolution.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbMultiResolutionAvailability, DisabledPrecedesFilesAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::MultiResolutionOptions m;
  m.levels = 0;
  vdb::ProcessingOptions p;
  p.maxInputBytes = 0;
  try {
    (void)vdb::BuildMultiResolution("missing", m, {}, p);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbMultiResolution : public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{};
    for (int i = 0; i < 100; ++i) {
      directory = std::filesystem::temp_directory_path() /
                  ("container-multires-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                   "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(directory))
        return;
    }
    FAIL();
  }
  void TearDown() override {
    std::error_code error;
    for (const auto &e : std::filesystem::directory_iterator(directory))
      if (e.is_regular_file())
        std::filesystem::remove(e.path(), error);
    std::filesystem::remove(directory, error);
  }
  auto path(const std::string &s) const { return directory / s; }
  void write(const std::string &name, const openvdb::GridPtrVec &grids) {
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(grids);
    file.close();
  }
  auto bytes(const std::string &name) {
    std::ifstream input(path(name), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(input), {});
  }
  auto impulse(const std::string &name = "field") {
    auto grid = openvdb::FloatGrid::create(0);
    grid->setName(name);
    grid->tree().setValue({0, 0, 0}, 64);
    return grid;
  }
};
TEST_F(OpenVdbMultiResolution, FullWeightingImpulseFractionalLodAndNativeTypes) {
  for (int type = 0; type < 4; ++type) {
    SCOPED_TRACE(type);
    openvdb::GridBase::Ptr base;
    if (type == 0)
      base = impulse();
    if (type == 1) {
      auto g = openvdb::DoubleGrid::create(0);
      g->tree().setValue({0, 0, 0}, 64);
      base = g;
    }
    if (type == 2) {
      auto g = openvdb::Vec3SGrid::create(openvdb::Vec3s(0));
      g->tree().setValue({0, 0, 0}, {64, -128, 32});
      base = g;
    }
    if (type == 3) {
      auto g = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
      g->tree().setValue({0, 0, 0}, {64, -128, 32});
      base = g;
    }
    base->setName("field");
    base->setIsInWorldSpace(true);
    write("input" + std::to_string(type) + ".vdb", {base});
    auto h = vdb::BuildMultiResolution(path("input" + std::to_string(type) + ".vdb"));
    ASSERT_EQ(h->levels().size(), 3u);
    EXPECT_EQ(h->levels()[0].valueType, base->valueType());
    EXPECT_EQ(h->levels()[1].voxelSize, (std::array<double, 3>{2, 2, 2}));
    const auto a = h->sampleWorld({0, 0, 0}, 0), b = h->sampleWorld({0, 0, 0}, 1),
               c = h->sampleWorld({0, 0, 0}, .5);
    if (type == 0) {
      EXPECT_FLOAT_EQ(std::get<float>(a), 64);
      EXPECT_FLOAT_EQ(std::get<float>(b), 8);
      EXPECT_FLOAT_EQ(std::get<float>(c), 36);
    }
    if (type == 1) {
      EXPECT_DOUBLE_EQ(std::get<double>(a), 64);
      EXPECT_DOUBLE_EQ(std::get<double>(b), 8);
      EXPECT_DOUBLE_EQ(std::get<double>(c), 36);
    }
    if (type == 2)
      EXPECT_EQ((std::get<std::array<float, 3>>(b)), (std::array<float, 3>{8, -16, 4}));
    if (type == 3)
      EXPECT_EQ((std::get<std::array<double, 3>>(b)), (std::array<double, 3>{8, -16, 4}));
    EXPECT_EQ(h->sampleWorld({100, 100, 100}, 1), type == 0   ? vdb::DenseValue(0.f)
                                                  : type == 1 ? vdb::DenseValue(0.)
                                                  : type == 2
                                                      ? vdb::DenseValue(std::array<float, 3>{})
                                                      : vdb::DenseValue(std::array<double, 3>{}));
  }
}
TEST_F(OpenVdbMultiResolution, AffineLinearWorldFieldSurvivesShearReflectionAndMetadata) {
  auto g = openvdb::DoubleGrid::create(0);
  g->setName("linear");
  openvdb::math::Mat4d matrix = openvdb::math::Mat4d::identity();
  matrix(0, 0) = 0;
  matrix(0, 1) = -2;
  matrix(1, 0) = 3;
  matrix(1, 1) = .25;
  matrix(2, 2) = .5;
  matrix(3, 0) = 4;
  matrix(3, 1) = -3;
  matrix(3, 2) = 7;
  g->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
  for (int z = -8; z <= 8; ++z)
    for (int y = -8; y <= 8; ++y)
      for (int x = -8; x <= 8; ++x) {
        auto w = g->indexToWorld(openvdb::Vec3d(x, y, z));
        g->tree().setValue({x, y, z}, 2 * w.x() - w.y() + .5 * w.z());
      }
  g->insertMeta("artist", openvdb::StringMetadata("preserved"));
  g->insertMeta("MultiResGrid_Level", openvdb::StringMetadata("stale"));
  write("linear.vdb", {g});
  const auto before = bytes("linear.vdb");
  auto h = vdb::BuildMultiResolution(path("linear.vdb"));
  const auto w = g->indexToWorld(openvdb::Vec3d(.25, -.5, .5));
  for (double level : {0., .5, 1., 1.5, 2.})
    EXPECT_NEAR(std::get<double>(h->sampleWorld({w.x(), w.y(), w.z()}, level)),
                2 * w.x() - w.y() + .5 * w.z(), 1e-11);
  h->writeAll(path("all.vdb"));
  openvdb::io::File f(path("all.vdb").string());
  f.open(false);
  auto all = f.getGrids();
  f.close();
  ASSERT_EQ(all->size(), 3u);
  for (size_t level = 0; level < 3; ++level) {
    auto out = openvdb::gridPtrCast<openvdb::DoubleGrid>((*all)[level]);
    ASSERT_TRUE(out);
    EXPECT_EQ(out->metaValue<std::string>("artist"), "preserved");
    EXPECT_EQ(out->getName(), "linear_level_" + std::to_string(level));
    EXPECT_EQ(out->indexToWorld(openvdb::Vec3d(0)), g->indexToWorld(openvdb::Vec3d(0)));
    EXPECT_EQ(out->indexToWorld(openvdb::Vec3d(1, 0, 0)),
              g->indexToWorld(openvdb::Vec3d(double(1u << level), 0, 0)));
  }
  EXPECT_EQ(before, bytes("linear.vdb"));
}
TEST_F(OpenVdbMultiResolution, PassthroughPreservesRemoteInactiveLeavesAndExtremeFiniteBits) {
  auto g = openvdb::DoubleGrid::create(7);
  g->setName("constant");
  g->tree().touchLeaf({1024, -1024, 8});
  g->tree().setValueOff({1024, -1024, 8}, 7);
  write("inactive.vdb", {g});
  vdb::MultiResolutionOptions m;
  m.levels = 1;
  auto h = vdb::BuildMultiResolution(path("inactive.vdb"), m);
  EXPECT_EQ(h->levels()[0].leafCount, 1u);
  EXPECT_EQ(std::get<double>(h->sampleWorld({0, 0, 0})), 7);
  h->writeLevel(path("copy.vdb"), 0);
  openvdb::io::File f(path("copy.vdb").string());
  f.open(false);
  auto out = f.readGrid("constant");
  f.close();
  EXPECT_EQ(out->baseTree().leafCount(), 1u);
  EXPECT_EQ(out->activeVoxelCount(), 0u);
  auto active = impulse();
  active->tree().touchLeaf({1024, 1024, 1024});
  write("remote.vdb", {active});
  EXPECT_NO_THROW((void)vdb::BuildMultiResolution(path("remote.vdb")));
  auto tiny = openvdb::DoubleGrid::create(0);
  tiny->setName("tiny");
  tiny->tree().setValue({0, 0, 0}, std::numeric_limits<double>::denorm_min());
  write("tiny.vdb", {tiny});
  auto exact = vdb::BuildMultiResolution(path("tiny.vdb"), m);
  exact->writeLevel(path("tiny-copy.vdb"), 0);
  openvdb::io::File tf(path("tiny-copy.vdb").string());
  tf.open(false);
  auto tg = openvdb::gridPtrCast<openvdb::DoubleGrid>(tf.readGrid("tiny"));
  tf.close();
  EXPECT_EQ(std::bit_cast<uint64_t>(tg->tree().getValue({0, 0, 0})), uint64_t(1));
}
TEST_F(OpenVdbMultiResolution, FiniteFilteredValuesMayFallBelowInputSafetyBand) {
  auto g = openvdb::FloatGrid::create(0);
  g->setName("tiny");
  g->setGridClass(openvdb::GRID_FOG_VOLUME);
  g->tree().setValue({0, 0, 0}, 2e-36f);
  write("tiny.vdb", {g});
  vdb::MultiResolutionOptions m;
  m.levels = 2;
  auto h = vdb::BuildMultiResolution(path("tiny.vdb"), m);
  EXPECT_FLOAT_EQ(std::get<float>(h->sampleWorld({0, 0, 0}, 1)), 2.5e-37f);
}
TEST_F(OpenVdbMultiResolution, ActiveTilesAreVoxelizedOnlyWithinAggregateBudgets) {
  auto g = openvdb::FloatGrid::create(0);
  g->setName("tile");
  g->tree().addTile(1, {0, 0, 0}, 2, true);
  write("tile.vdb", {g});
  auto h = vdb::BuildMultiResolution(path("tile.vdb"));
  EXPECT_EQ(std::get<float>(h->sampleWorld({2, 2, 2}, 0)), 2);
  vdb::ProcessingOptions p;
  p.maxVoxelCount = 512;
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("tile.vdb"), {}, {}, p), std::exception);
  vdb::MultiResolutionOptions m;
  m.maxWork = 10;
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("tile.vdb"), m), std::exception);
  p = {};
  p.maxInputBytes = 8 * 1024 * 1024;
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("tile.vdb"), {}, {}, p), std::exception);
  p = {};
  p.maxVoxelCount = 1;
  EXPECT_THROW(h->writeAll(path("budget.vdb"), p), std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("budget.vdb")));
  p = {};
  p.maxVoxelCount = std::numeric_limits<uint64_t>::max();
  EXPECT_NO_THROW(h->writeLevel(path("unbounded.vdb"), 1, p));
}
TEST_F(OpenVdbMultiResolution, RejectsUnsupportedFieldSemanticsAndInvalidValues) {
  auto g = impulse();
  const auto reject = [&](const std::string &name, openvdb::GridBase::Ptr grid) {
    write(name, {grid});
    EXPECT_THROW((void)vdb::BuildMultiResolution(path(name)), std::exception);
  };
  g->setGridClass(openvdb::GRID_LEVEL_SET);
  reject("sdf.vdb", g);
  g->setGridClass(openvdb::GRID_STAGGERED);
  reject("mac.vdb", g);
  g->setGridClass(openvdb::GRID_FOG_VOLUME);
  g->tree().setValue({0, 0, 0}, -1);
  reject("negative.vdb", g);
  g->setGridClass(openvdb::GRID_UNKNOWN);
  g->tree().setValueOff({1, 0, 0}, 1);
  reject("inactive.vdb", g);
  g->tree().setValueOff({1, 0, 0}, 0);
  g->tree().setValue({0, 0, 0}, INFINITY);
  reject("nonfinite.vdb", g);
  g->tree().setValue({0, 0, 0}, std::numeric_limits<float>::max());
  reject("overflow.vdb", g);
  auto v = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
  v->setName("vector");
  v->tree().setValue({0, 0, 0}, {1, 2, 3});
  v->setVectorType(openvdb::VEC_COVARIANT);
  reject("normal.vdb", v);
  v->setVectorType(openvdb::VEC_INVARIANT);
  v->setIsInWorldSpace(false);
  reject("local.vdb", v);
  auto integer = openvdb::Int32Grid::create(0);
  integer->setName("integer");
  reject("integer.vdb", integer);
}
TEST_F(OpenVdbMultiResolution, SelectorCatalogStreamingAndRecordedStatisticsAreValidated) {
  auto a = impulse("same"), b = impulse("same");
  b->tree().setValue({0, 0, 0}, 128);
  write("duplicates.vdb", {a, b});
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("duplicates.vdb"), {}, "same"), std::exception);
  EXPECT_NO_THROW(
      (void)vdb::BuildMultiResolution(path("duplicates.vdb"), {}, "descriptor:same[0]"));
  auto c = impulse("same[0]");
  write("collision.vdb", {a, b, c});
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("collision.vdb")), std::exception);
  std::ofstream stream(path("stream.vdb"), std::ios::binary);
  openvdb::io::Stream archive(stream);
  archive.write({a});
  stream.close();
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("stream.vdb")), std::exception);
  openvdb::io::File noStats(path("stats.vdb").string());
  noStats.setGridStatsMetadataEnabled(false);
  noStats.write({a});
  noStats.close();
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("stats.vdb")), std::exception);
}
TEST_F(OpenVdbMultiResolution, PrecisionLimitsOptionsAndExclusivePublication) {
  auto g = impulse();
  write("source.vdb", {g});
  const auto source = bytes("source.vdb");
  auto h = vdb::BuildMultiResolution(path("source.vdb"));
  EXPECT_THROW(h->writeAll(path("source.vdb")), std::exception);
  EXPECT_EQ(source, bytes("source.vdb"));
  EXPECT_THROW(h->writeLevel(path("bad.vdb"), 3), std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  EXPECT_THROW((void)h->sampleWorld({0, 0, 0}, 3), std::exception);
  EXPECT_THROW((void)h->sampleWorld({NAN, 0, 0}), std::exception);
  EXPECT_THROW((void)h->sampleWorld({1e9 + .5, 0, 0}), std::exception);
  vdb::MultiResolutionOptions m;
  m.levels = 0;
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("source.vdb"), m), std::exception);
  m.levels = 9;
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("source.vdb"), m), std::exception);
  g->setTransform(openvdb::math::Transform::createLinearTransform(1));
  g->transform().postTranslate({1e15, 0, 0});
  write("precision.vdb", {g});
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("precision.vdb")), std::exception);
}
TEST_F(OpenVdbMultiResolution, RetainedMetadataAndWorldVectorComponentsCountAtEveryLevel) {
  auto g = openvdb::Vec3DGrid::create(openvdb::Vec3d(1, -2, 3));
  g->setName("velocity");
  g->setIsInWorldSpace(true);
  g->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  g->setTransform(openvdb::math::Transform::createLinearTransform(.25));
  g->transform().postScale(openvdb::Vec3d(-1, 2, 1));
  g->tree().setValue({0, 0, 0}, openvdb::Vec3d(1, -2, 3));
  write("plain.vdb", {g});
  vdb::MultiResolutionOptions m;
  m.levels = 8;
  vdb::ProcessingOptions p;
  p.maxInputBytes = 64 * 1024 * 1024;
  auto h = vdb::BuildMultiResolution(path("plain.vdb"), m);
  EXPECT_EQ((std::get<std::array<double, 3>>(h->sampleWorld({1, -3, .5}, 7))),
            (std::array<double, 3>{1, -2, 3}));
  auto metadata = impulse();
  write("plain-scalar.vdb", {metadata});
  EXPECT_NO_THROW((void)vdb::BuildMultiResolution(path("plain-scalar.vdb"), m, {}, p));
  metadata->insertMeta("payload", openvdb::StringMetadata(std::string(2 * 1024 * 1024, 'x')));
  write("metadata.vdb", {metadata});
  m.levels = 1;
  EXPECT_NO_THROW((void)vdb::BuildMultiResolution(path("metadata.vdb"), m, {}, p));
  m.levels = 8;
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("metadata.vdb"), m, {}, p), std::exception);
}
TEST_F(OpenVdbMultiResolution, ProductTranslationCancellationCannotHideWorldPrecisionLoss) {
  auto g = impulse();
  g->tree().clear();
  g->tree().setValue({1000000000, 0, 0}, 1);
  auto matrix = openvdb::math::Mat4d::identity();
  matrix(0, 0) = std::nextafter(1e6, 0.);
  matrix(1, 1) = 1e-6;
  matrix(3, 0) = -1e15;
  g->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
  write("cancellation.vdb", {g});
  EXPECT_THROW((void)vdb::BuildMultiResolution(path("cancellation.vdb")), std::exception);
}
#endif
} // namespace
