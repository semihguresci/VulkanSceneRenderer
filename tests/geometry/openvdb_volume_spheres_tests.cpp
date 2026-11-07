#include "Container/geometry/OpenVdbVolumeSpheres.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <nlohmann/json.hpp>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/LevelSetUtil.h>
#endif
namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbVolumeSpheresAvailability, DisabledBeforeOptionsPathsAndOutput) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ProcessingOptions p;
  p.maxInputBytes = 0;
  vdb::VolumeSpheresOptions o;
  o.maxCount = 0;
  try {
    (void)vdb::FitVolumeSpheres("missing", o, {}, p);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
  try {
    (void)vdb::ExportVolumeSpheres("missing", "missing", o, {}, p);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbVolumeSpheres : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-volume-spheres-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    source = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(
        1, openvdb::Vec3f(0), .1f, 3.f);
    source->setName("shape");
    source->insertMeta("artist", openvdb::StringMetadata("kept"));
    write("source.vdb", {source});
    o.candidateCount = 256;
    o.maxCount = 12;
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &f : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(f.path(), e);
    std::filesystem::remove(directory, e);
  }
  auto path(const std::string &n) const { return directory / n; }
  void write(const std::string &n, const openvdb::GridPtrVec &g) {
    openvdb::io::File f(path(n).string());
    f.write(g);
  }
  static std::string bytes(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
  }
  auto fit(const std::string &file = "source.vdb") {
    return vdb::FitVolumeSpheres(path(file), o, {}, p);
  }
  std::filesystem::path directory;
  openvdb::FloatGrid::Ptr source;
  vdb::VolumeSpheresOptions o;
  vdb::ProcessingOptions p;
};
TEST_F(OpenVdbVolumeSpheres,
       AnalyticSphereDeterministicWorldRadiiMetadataAndSourceBytes) {
  const auto before = bytes(path("source.vdb"));
  const auto a = fit(), b = fit();
  ASSERT_FALSE(a.spheres.empty());
  ASSERT_EQ(a.spheres.size(), b.spheres.size());
  EXPECT_LE(a.spheres.size(), o.maxCount);
  EXPECT_EQ(a.gridName, "shape");
  EXPECT_EQ(a.valueType, "float");
  EXPECT_NEAR(a.voxelSize, .1, 1e-8);
  EXPECT_NE(a.diagnostic.find("Approximate"), std::string::npos);
  bool artist = false;
  for (const auto &m : a.metadata)
    if (m.name == "artist") {
      artist = true;
      EXPECT_EQ(m.value, "kept");
      EXPECT_FALSE(m.opaque);
    }
  EXPECT_TRUE(artist);
  double previous = INFINITY;
  for (size_t n = 0; n < a.spheres.size(); ++n) {
    const auto &s = a.spheres[n];
    EXPECT_GT(s.radius, 0);
    EXPECT_LE(s.radius, previous);
    previous = s.radius;
    EXPECT_LE(std::hypot(s.position[0], s.position[1], s.position[2]) +
                  s.radius,
              1.15);
    EXPECT_EQ(s.position, b.spheres[n].position);
    EXPECT_EQ(s.radius, b.spheres[n].radius);
  }
  EXPECT_GT(a.spheres.front().radius, .65);
  EXPECT_EQ(before, bytes(path("source.vdb")));
}
TEST_F(OpenVdbVolumeSpheres, DefaultControlsAndUniformFogInteriorTiles) {
  const auto standard = vdb::FitVolumeSpheres(path("source.vdb"));
  ASSERT_FALSE(standard.spheres.empty());
  auto fog = openvdb::DoubleGrid::create(0);
  fog->setName("tiled-fog");
  fog->setGridClass(openvdb::GRID_FOG_VOLUME);
  fog->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  fog->tree().fill(openvdb::CoordBBox({-8, -8, -8}, {7, 7, 7}), 1, true);
  ASSERT_TRUE(fog->tree().hasActiveTiles());
  write("tiled-fog.vdb", {fog});
  // Surface crossing and interior tiles are both meaningful fog, not malformed
  // active SDF tiles. Bounded rebuild handles the native tiled representation.
  const auto result = fit("tiled-fog.vdb");
  ASSERT_FALSE(result.spheres.empty());
  for (const auto &sphere : result.spheres)
    for (size_t axis = 0; axis < 3; ++axis)
      EXPECT_LE(std::abs(sphere.position[axis] + .05) + sphere.radius, .9);
}
TEST_F(OpenVdbVolumeSpheres, RadiusPostLimitsAndExplicitUnmetMinimum) {
  o.minRadiusWorld = .15;
  o.maxRadiusWorld = .2;
  const auto a = fit();
  ASSERT_FALSE(a.spheres.empty());
  for (const auto &s : a.spheres) {
    EXPECT_GE(s.radius, .15);
    EXPECT_LE(s.radius, .2);
  }
  o.minRadiusWorld = 2;
  o.maxRadiusWorld = 3;
  o.minCount = 0;
  EXPECT_TRUE(fit().spheres.empty());
  o.minCount = 1;
  EXPECT_THROW((void)fit(), std::invalid_argument);
}
TEST_F(OpenVdbVolumeSpheres, SmallGridAdaptivePathAndDoubleFogInterior) {
  auto small = openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(
      .4, openvdb::Vec3d(0), .1, 3);
  small->setName("small");
  ASSERT_LT(small->activeVoxelCount(), 10000u);
  write("small.vdb", {small});
  const auto a = fit("small.vdb");
  ASSERT_FALSE(a.spheres.empty());
  EXPECT_GT(a.spheres.front().radius, .2);
  for (const auto &s : a.spheres)
    EXPECT_LE(std::hypot(s.position[0], s.position[1], s.position[2]) +
                  s.radius,
              .55);
  auto fog = small->deepCopy();
  openvdb::tools::sdfToFogVolume(*fog);
  fog->setName("fog");
  write("fog.vdb", {fog});
  o.isovalue = .5;
  const auto b = fit("fog.vdb");
  ASSERT_FALSE(b.spheres.empty());
  EXPECT_EQ(b.valueType, "double");
  EXPECT_EQ(b.isovalue, .5);
  for (const auto &s : b.spheres)
    EXPECT_LE(
        std::hypot(s.position[0], s.position[1], s.position[2]) + s.radius, .6);
}
TEST_F(OpenVdbVolumeSpheres, LargeWorldTranslationRotationAndPhysicalScale) {
  const auto a = fit();
  auto shifted = source->deepCopy();
  shifted->transform().postTranslate({1e6 + .125, -100, 20});
  write("shifted.vdb", {shifted});
  const auto b = fit("shifted.vdb");
  ASSERT_EQ(a.spheres.size(), b.spheres.size());
  for (size_t n = 0; n < a.spheres.size(); ++n) {
    EXPECT_NEAR(b.spheres[n].position[0] - (1e6 + .125),
                a.spheres[n].position[0], 1e-9);
    EXPECT_NEAR(b.spheres[n].position[1] + 100, a.spheres[n].position[1],
                1e-10);
    EXPECT_EQ(b.spheres[n].radius, a.spheres[n].radius);
  }
  shifted = source->deepCopy();
  shifted->transform().postScale(2.);
  for (auto v = shifted->beginValueAll(); v; ++v)
    v.setValue(*v * 2);
  shifted->tree().root().setBackground(shifted->background() * 2, false);
  write("scaled.vdb", {shifted});
  const auto c = fit("scaled.vdb");
  ASSERT_EQ(a.spheres.size(), c.spheres.size());
  for (size_t n = 0; n < a.spheres.size(); ++n) {
    EXPECT_NEAR(c.spheres[n].radius, a.spheres[n].radius * 2, 1e-6);
    for (size_t k = 0; k < 3; ++k)
      EXPECT_NEAR(c.spheres[n].position[k], a.spheres[n].position[k] * 2, 1e-6);
  }
}
TEST_F(OpenVdbVolumeSpheres,
       NonoverlapAndWorldRadiusCapsRemainPhysicalAfterAdaptiveFit) {
  auto small = openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(
      .4, openvdb::Vec3d(0), .1, 3);
  small->setName("small");
  write("small.vdb", {small});
  o.maxRadiusWorld = .12;
  const auto a = fit("small.vdb");
  ASSERT_FALSE(a.spheres.empty());
  for (const auto &s : a.spheres)
    EXPECT_LE(s.radius, .12);
  for (size_t i = 0; i < a.spheres.size(); ++i)
    for (size_t j = 0; j < i; ++j) {
      const auto &a0 = a.spheres[i];
      const auto &b = a.spheres[j];
      const double distance = std::hypot(a0.position[0] - b.position[0],
                                         a0.position[1] - b.position[1],
                                         a0.position[2] - b.position[2]);
      EXPECT_GE(distance + 2e-5, a0.radius + b.radius);
    }
}
TEST_F(OpenVdbVolumeSpheres,
       UnicodeJsonOutputNoOverwriteAndInvalidUtf8Cleanup) {
  const auto unicode = directory / std::filesystem::path(u"données_球.vdb");
  std::filesystem::copy_file(path("source.vdb"), unicode);
  const auto output = directory / std::filesystem::path(u"sphères_球.json");
  const auto before = bytes(unicode);
  const auto result = vdb::ExportVolumeSpheres(unicode, output, o, {}, p);
  std::ifstream file(output, std::ios::binary);
  const auto json = nlohmann::json::parse(file);
  EXPECT_EQ(json.at("schemaVersion"), 1);
  EXPECT_EQ(json.at("spheres").size(), result.spheres.size());
  EXPECT_EQ(json.at("source").at("gridName"), "shape");
  EXPECT_EQ(bytes(unicode), before);
  EXPECT_THROW((void)vdb::ExportVolumeSpheres(unicode, output, o, {}, p),
               std::invalid_argument);
  source->insertMeta("bad",
                     openvdb::StringMetadata(std::string("\xc0\xaf", 2)));
  write("bad-utf8.vdb", {source});
  EXPECT_THROW((void)vdb::ExportVolumeSpheres(path("bad-utf8.vdb"),
                                              path("bad.json"), o, {}, p),
               nlohmann::json::type_error);
  EXPECT_FALSE(std::filesystem::exists(path("bad.json")));
}
TEST_F(OpenVdbVolumeSpheres, InvalidOptionsBudgetsAndTypesReject) {
  o.maxCount = 0;
  EXPECT_THROW((void)fit(), std::invalid_argument);
  o.maxCount = 12;
  o.maxWork = 1;
  EXPECT_THROW((void)fit(), std::invalid_argument);
  o.maxWork = 1'000'000'000;
  o.maxMetadataBytes = 1;
  EXPECT_THROW((void)fit(), std::invalid_argument);
  o.maxMetadataBytes = 1024 * 1024;
  p.maxVoxelCount = 512;
  EXPECT_THROW((void)fit(), std::invalid_argument);
  p.maxVoxelCount = 16'000'000;
  p.maxInputBytes = 4 * 1024 * 1024;
  EXPECT_THROW((void)fit(), std::invalid_argument);
  p.maxInputBytes = 512ull * 1024 * 1024;
  auto g = openvdb::Int32Grid::create();
  g->setName("integer");
  g->tree().setValue({0, 0, 0}, 1);
  write("integer.vdb", {g});
  EXPECT_THROW((void)fit("integer.vdb"), std::invalid_argument);
}
TEST_F(OpenVdbVolumeSpheres, FileMetadataSharesCapsAndIsReported) {
  openvdb::MetaMap fileMetadata;
  fileMetadata.insertMeta("project",
                          openvdb::StringMetadata("file-level source"));
  {
    openvdb::io::File file(path("file-meta.vdb").string());
    file.write(openvdb::GridPtrVec{source}, fileMetadata);
  }
  const auto result = fit("file-meta.vdb");
  bool found = false;
  for (const auto &m : result.fileMetadata)
    if (m.name == "project") {
      found = true;
      EXPECT_EQ(m.value, "file-level source");
    }
  EXPECT_TRUE(found);
  fileMetadata.insertMeta("large",
                          openvdb::StringMetadata(std::string(16384, 'x')));
  {
    openvdb::io::File file(path("large-file-meta.vdb").string());
    file.write(openvdb::GridPtrVec{source}, fileMetadata);
  }
  o.maxMetadataBytes = 4096;
  try {
    (void)fit("large-file-meta.vdb");
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("file metadata"), std::string::npos);
  }
}
TEST_F(OpenVdbVolumeSpheres, OversizedPublicMetadataCapCannotWrapJsonReserve) {
  o.maxMetadataBytes = uintmax_t(1) << 60;
  EXPECT_THROW((void)vdb::ExportVolumeSpheres(path("source.vdb"),
                                              path("wrapped.json"), o, {}, p),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("wrapped.json")));
}
TEST_F(OpenVdbVolumeSpheres, WorkLimitIsSharedAcrossExecutedStages) {
  // Independent bounds for this origin-centered analytic sphere: initial
  // normalization covers its active box plus16 cells of halo; the native
  // adaptive stage covers allocated leaves rescaled to its small-grid map.
  const auto initial = source->evalActiveVoxelBoundingBox();
  auto native = initial;
  for (auto leaf = source->tree().cbeginLeaf(); leaf; ++leaf)
    native.expand(leaf->getNodeBoundingBox());
  const double scale = source->activeVoxelCount() < 10000
                           ? 1 / std::cbrt(20000. / source->activeVoxelCount())
                           : 1;
  uint64_t firstCells = 1, secondCells = 1;
  for (size_t axis = 0; axis < 3; ++axis) {
    firstCells *=
        uint64_t(int64_t(initial.max()[axis]) - initial.min()[axis] + 33);
    secondCells *= uint64_t(std::ceil(native.max()[axis] / scale) -
                            std::floor(native.min()[axis] / scale) + 17);
  }
  const uint64_t greedy = uint64_t(o.maxCount) * o.candidateCount * 64;
  const uint64_t firstWork = firstCells * 128 + greedy;
  const uint64_t secondWork =
      secondCells * (8 * o.candidateCount + 128) + greedy;
  o.maxWork = std::max(firstWork, secondWork) + 1;
  ASSERT_LT(o.maxWork, firstWork + secondWork);
  EXPECT_THROW((void)fit(), std::invalid_argument);
}
TEST_F(OpenVdbVolumeSpheres,
       FullNativeLeafPayloadIsCheckedBeforeMaterialization) {
  auto grid = source->deepCopy();
  grid->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                   openvdb::Int64Metadata(1));
  grid->insertMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT,
                   openvdb::Int64Metadata(grid->activeVoxelCount()));
  {
    openvdb::io::File file(path("underreported.vdb").string());
    file.setGridStatsMetadataEnabled(false);
    file.write(openvdb::GridPtrVec{grid});
  }
  const auto full = openvdb::tools::memUsageIfLoaded(grid->tree(), false);
  p.maxInputBytes = std::filesystem::file_size(path("underreported.vdb")) +
                    4 * 1024 * 1024 + full / 2 + 128 * 1024;
  try {
    (void)fit("underreported.vdb");
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("before leaf materialization"),
              std::string::npos);
  }
}
TEST_F(OpenVdbVolumeSpheres, DistanceAndNativeIsovalueUnderflowAreExplicit) {
  auto tiny = openvdb::DoubleGrid::create(3e6);
  tiny->setName("tiny-distance");
  tiny->setGridClass(openvdb::GRID_LEVEL_SET);
  tiny->setTransform(openvdb::math::Transform::createLinearTransform(1e6));
  tiny->tree().setValue({0, 0, 0}, std::numeric_limits<double>::denorm_min());
  tiny->tree().setValue({1, 0, 0}, -1e6);
  write("tiny-distance.vdb", {tiny});
  try {
    (void)fit("tiny-distance.vdb");
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("normalization"), std::string::npos);
  }
  o.isovalue = std::numeric_limits<double>::denorm_min();
  try {
    (void)fit();
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("isovalue"), std::string::npos);
  }
}
TEST_F(OpenVdbVolumeSpheres, DisabledDelayedLoadingHasActionableDiagnostic) {
  struct Environment {
    bool present = false;
    std::string previous;
    Environment() {
      if (const auto *value = std::getenv("OPENVDB_DISABLE_DELAYED_LOAD")) {
        present = true;
        previous = value;
      }
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", "1");
#else
      setenv("OPENVDB_DISABLE_DELAYED_LOAD", "1", 1);
#endif
    }
    ~Environment() {
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD",
                present ? previous.c_str() : "");
#else
      if (present)
        setenv("OPENVDB_DISABLE_DELAYED_LOAD", previous.c_str(), 1);
      else
        unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
    }
  } environment;
  try {
    (void)fit();
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("unset OPENVDB_DISABLE_DELAYED_LOAD"),
              std::string::npos);
  }
}
TEST_F(OpenVdbVolumeSpheres, NonfiniteNoCrossingMapAndWorldPrecisionReject) {
  auto g = source->deepCopy();
  g->tree().setValue({0, 0, 0}, NAN);
  write("nan.vdb", {g});
  EXPECT_THROW((void)fit("nan.vdb"), std::invalid_argument);
  g = source->deepCopy();
  g->transform().postScale({1., 2., 1.});
  write("anisotropic.vdb", {g});
  EXPECT_THROW((void)fit("anisotropic.vdb"), std::invalid_argument);
  g = source->deepCopy();
  g->transform().postTranslate({1e15, 0, 0});
  write("precision.vdb", {g});
  EXPECT_THROW((void)fit("precision.vdb"), std::invalid_argument);
  g = openvdb::FloatGrid::create(.3f);
  g->setName("no-crossing");
  g->setGridClass(openvdb::GRID_LEVEL_SET);
  g->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  g->tree().setValue({0, 0, 0}, .1f);
  write("no-crossing.vdb", {g});
  EXPECT_THROW((void)fit("no-crossing.vdb"), std::invalid_argument);
}
TEST_F(OpenVdbVolumeSpheres, SelectorCollisionAndDefaultFirstSupportedGrid) {
  auto other = source->deepCopy();
  other->setName("other");
  write("multi.vdb", {source, other});
  const auto selected = vdb::FitVolumeSpheres(path("multi.vdb"), o, "other", p);
  EXPECT_EQ(selected.gridName, "other");
  o.maxGrids = 1;
  EXPECT_THROW((void)fit("multi.vdb"), std::invalid_argument);
  o.maxGrids = 256;
  auto duplicate = source->deepCopy();
  auto literal = source->deepCopy();
  literal->setName("shape[0]");
  write("collision.vdb", {source, duplicate, literal});
  EXPECT_THROW((void)fit("collision.vdb"), std::invalid_argument);
}
#endif
} // namespace
