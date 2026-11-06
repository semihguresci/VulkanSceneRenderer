#include "Container/geometry/OpenVdbSurfaceQueries.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/tools/LevelSetSphere.h>
#endif
namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbSurfaceQueriesAvailability, OffBeforePathsAndInvalidOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::SurfaceQueryOptions o;
  o.maxQueries = 0;
  try {
    (void)vdb::QueryLevelSetSurface("missing", {}, o);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbSurfaceQueries : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    dir = std::filesystem::temp_directory_path() /
          ("container-surface-queries-tests-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
    std::filesystem::create_directory(dir);
    sphere = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(
        1, openvdb::Vec3f(0), .1f, 3);
    sphere->setName("sphere");
    sphere->insertMeta("artist", openvdb::StringMetadata("source unchanged"));
    write("sphere.vdb", {sphere});
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &f : std::filesystem::directory_iterator(dir))
      std::filesystem::remove(f.path(), e);
    std::filesystem::remove(dir, e);
  }
  auto path(const std::string &n) const { return dir / n; }
  void write(const std::string &n, const openvdb::GridPtrVec &grids) {
    openvdb::io::File file(path(n).string());
    file.write(grids);
  }
  static std::string bytes(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
  }
  auto query(const std::vector<std::array<double, 3>> &points,
             const std::string &file = "sphere.vdb") {
    return vdb::QueryLevelSetSurface(path(file), points, o, {}, p);
  }
  std::filesystem::path dir;
  openvdb::FloatGrid::Ptr sphere;
  vdb::SurfaceQueryOptions o;
  vdb::ProcessingOptions p;
};
TEST_F(OpenVdbSurfaceQueries,
       SpherePointCloudDistanceLocalCurvatureNormalsAndMetadata) {
  const auto before = bytes(path("sphere.vdb"));
  const auto result = query({{1.2, 0, 0}, {1, 0, 0}, {0, 0, 0}, {2, 0, 0}});
  ASSERT_EQ(result.queries.size(), 4u);
  EXPECT_EQ(result.gridName, "sphere");
  EXPECT_NE(result.diagnostic.find("POINT CLOUD"), std::string::npos);
  bool artist = false;
  for (const auto &m : result.metadata)
    if (m.name == "artist") {
      artist = true;
      EXPECT_EQ(m.value, "source unchanged");
    }
  EXPECT_TRUE(artist);
  for (const auto &item : result.queries) {
    const auto &point = item.closestPosition;
    EXPECT_NEAR(std::hypot(point[0], point[1], point[2]), 1, .035);
    EXPECT_TRUE(item.geometryValid);
    EXPECT_NEAR(item.meanCurvature, 1, .035);
    EXPECT_NEAR(item.gaussianCurvature, 1, .07);
    const double dot = point[0] * item.normal[0] + point[1] * item.normal[1] +
                       point[2] * item.normal[2];
    EXPECT_GT(dot, .97);
    EXPECT_NEAR(std::hypot(item.normal[0], item.normal[1], item.normal[2]), 1,
                1e-12);
  }
  EXPECT_NEAR(result.queries[0].distance, .2, .04);
  EXPECT_TRUE(result.queries[1].querySdfValid);
  EXPECT_NEAR(result.queries[1].querySdf, 0, 1e-7);
  EXPECT_FALSE(result.queries[2].querySdfValid);
  EXPECT_FALSE(result.queries[3].querySdfValid);
  EXPECT_EQ(bytes(path("sphere.vdb")), before);
}
TEST_F(OpenVdbSurfaceQueries, DoublePlaneHasZeroCurvatureAndOutwardNormal) {
  auto plane = openvdb::DoubleGrid::create(.3);
  plane->setName("plane");
  plane->setGridClass(openvdb::GRID_LEVEL_SET);
  plane->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  for (int x = -2; x <= 2; ++x)
    for (int y = -8; y <= 8; ++y)
      for (int z = -8; z <= 8; ++z)
        plane->tree().setValue({x, y, z}, x * .1);
  write("plane.vdb", {plane});
  const auto result = query({{.15, 0, 0}}, "plane.vdb");
  ASSERT_EQ(result.queries.size(), 1u);
  const auto &item = result.queries[0];
  EXPECT_TRUE(item.querySdfValid);
  EXPECT_NEAR(item.querySdf, .15, 1e-15);
  ASSERT_TRUE(item.geometryValid);
  EXPECT_NEAR(item.closestPosition[0], 0, 1e-12);
  EXPECT_NEAR(item.normal[0], 1, 1e-12);
  EXPECT_NEAR(item.normal[1], 0, 1e-12);
  EXPECT_NEAR(item.meanCurvature, 0, 1e-12);
  EXPECT_NEAR(item.gaussianCurvature, 0, 1e-12);
}
TEST_F(OpenVdbSurfaceQueries, TorusOuterAndInnerSignedCurvature) {
  const double dx = .05, R = 1, r = .3, bg = .15;
  auto torus = openvdb::DoubleGrid::create(bg);
  torus->setName("torus");
  torus->setGridClass(openvdb::GRID_LEVEL_SET);
  torus->setTransform(openvdb::math::Transform::createLinearTransform(dx));
  for (int x = -30; x <= 30; ++x)
    for (int y = -30; y <= 30; ++y)
      for (int z = -10; z <= 10; ++z) {
        const double d = std::hypot(std::hypot(x * dx, y * dx) - R, z * dx) - r;
        if (std::abs(d) < bg)
          torus->tree().setValue({x, y, z}, d);
        else if (d < 0)
          torus->tree().setValueOff({x, y, z}, -bg);
      }
  write("torus.vdb", {torus});
  const auto result = query({{1.4, 0, 0}, {.6, 0, 0}}, "torus.vdb");
  ASSERT_EQ(result.queries.size(), 2u);
  const auto &outer = result.queries[0];
  const auto &inner = result.queries[1];
  ASSERT_TRUE(outer.geometryValid);
  ASSERT_TRUE(inner.geometryValid);
  EXPECT_GT(outer.normal[0], .97);
  EXPECT_LT(inner.normal[0], -.97);
  EXPECT_NEAR(outer.meanCurvature, (1 / r + 1 / (R + r)) / 2, .15);
  EXPECT_NEAR(outer.gaussianCurvature, 1 / (r * (R + r)), .35);
  EXPECT_NEAR(inner.meanCurvature, (1 / r - 1 / (R - r)) / 2, .2);
  EXPECT_NEAR(inner.gaussianCurvature, -1 / (r * (R - r)), .6);
}
TEST_F(OpenVdbSurfaceQueries, ScaleRotationAndRemoteOriginRestoreWorldUnits) {
  const auto base = query({{1.2, 0, 0}});
  auto g = sphere->deepCopy();
  g->transform().postScale(2.);
  g->transform().postRotate(.3, openvdb::math::Z_AXIS);
  g->transform().postTranslate({1e6 + .125, -100, 20});
  for (auto v = g->beginValueAll(); v; ++v)
    v.setValue(*v * 2);
  g->tree().root().setBackground(g->background() * 2, false);
  write("transformed.vdb", {g});
  const double c = std::cos(.3), s = std::sin(.3);
  const auto transformed =
      query({{1e6 + .125 + 2.4 * c, -100 + 2.4 * s, 20}}, "transformed.vdb");
  const auto &a = base.queries[0];
  const auto &b = transformed.queries[0];
  ASSERT_TRUE(b.geometryValid);
  EXPECT_NEAR(b.distance, a.distance * 2, 2e-7);
  EXPECT_NEAR(b.meanCurvature, a.meanCurvature / 2, 1e-6);
  EXPECT_NEAR(b.gaussianCurvature, a.gaussianCurvature / 4, 1e-6);
  EXPECT_NEAR(b.normal[0], a.normal[0] * c - a.normal[1] * s, 1e-6);
  EXPECT_NEAR(b.normal[1], a.normal[0] * s + a.normal[1] * c, 1e-6);
}
TEST_F(OpenVdbSurfaceQueries, MissingActiveCurvatureFootprintIsFlagged) {
  auto g = sphere->deepCopy();
  std::vector<openvdb::Coord> removed;
  for (auto v = g->cbeginValueOn(); v; ++v)
    if (v.getCoord().x() > 10)
      removed.push_back(v.getCoord());
  for (const auto &coordinate : removed)
    g->tree().setValueOff(coordinate, g->background());
  write("thin.vdb", {g});
  const auto result = query({{1.2, 0, 0}}, "thin.vdb");
  ASSERT_EQ(result.queries.size(), 1u);
  EXPECT_FALSE(result.queries[0].geometryValid);
  EXPECT_EQ(result.queries[0].normal, (std::array<double, 3>{}));
  EXPECT_EQ(result.queries[0].meanCurvature, 0);
}
TEST_F(OpenVdbSurfaceQueries,
       RemoteIndexForwardCenterAvoidsInverseCoefficientCancellation) {
  constexpr int origin = -1000000000;
  auto plane = openvdb::DoubleGrid::create(3e6);
  plane->setName("remote-plane");
  plane->setGridClass(openvdb::GRID_LEVEL_SET);
  plane->setTransform(openvdb::math::Transform::createLinearTransform(1e6));
  plane->transform().postTranslate({1e15, 0, 0});
  for (int x = -2; x <= 2; ++x)
    for (int y = -4; y <= 4; ++y)
      for (int z = -4; z <= 4; ++z)
        plane->tree().setValue({origin + x, y, z}, double(x) * 1e6);
  write("remote-plane.vdb", {plane});
  const auto result = query({{.5, 0, 0}}, "remote-plane.vdb");
  ASSERT_EQ(result.queries.size(), 1u);
  ASSERT_TRUE(result.queries[0].querySdfValid);
  EXPECT_NEAR(result.queries[0].querySdf, .5, 1e-12);
}
TEST_F(OpenVdbSurfaceQueries, EmptyQueriesAndUnicodeSource) {
  const auto unicode = dir / std::filesystem::path(u"surface_球.vdb");
  std::filesystem::copy_file(path("sphere.vdb"), unicode);
  const auto result = vdb::QueryLevelSetSurface(unicode, {}, o, {}, p);
  EXPECT_TRUE(result.queries.empty());
  EXPECT_EQ(result.gridName, "sphere");
  const auto other = query({{1, 0, 0}});
  ASSERT_EQ(other.queries.size(), 1u);
}
TEST_F(OpenVdbSurfaceQueries, LimitsAndInvalidInputsReject) {
  o.maxQueries = 1;
  EXPECT_THROW((void)query({{1, 0, 0}, {2, 0, 0}}), std::invalid_argument);
  o.maxQueries = 10000;
  o.maxWork = 1;
  EXPECT_THROW((void)query({{1, 0, 0}}), std::invalid_argument);
  o.maxWork = 1'000'000'000;
  o.maxMetadataBytes = 1;
  EXPECT_THROW((void)query({{1, 0, 0}}), std::invalid_argument);
  o.maxMetadataBytes = 1024 * 1024;
  p.maxVoxelCount = 512;
  EXPECT_THROW((void)query({{1, 0, 0}}), std::invalid_argument);
  p.maxVoxelCount = 16'000'000;
  p.maxInputBytes = 4 * 1024 * 1024;
  EXPECT_THROW((void)query({{1, 0, 0}}), std::invalid_argument);
  p.maxInputBytes = 512ull * 1024 * 1024;
  EXPECT_THROW((void)query({{NAN, 0, 0}}), std::invalid_argument);
  EXPECT_THROW((void)query({{1e6, 0, 0}}), std::invalid_argument);
  auto g = sphere->deepCopy();
  g->tree().setValue({0, 0, 0}, NAN);
  write("nan.vdb", {g});
  EXPECT_THROW((void)query({{1, 0, 0}}, "nan.vdb"), std::invalid_argument);
  g = sphere->deepCopy();
  g->transform().postScale({1., 2., 1.});
  write("shear.vdb", {g});
  EXPECT_THROW((void)query({{1, 0, 0}}, "shear.vdb"), std::invalid_argument);
}
#endif
} // namespace
