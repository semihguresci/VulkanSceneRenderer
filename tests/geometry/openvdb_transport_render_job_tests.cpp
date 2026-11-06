#include "Container/geometry/OpenVdbTransportRenderJob.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#endif
namespace {
namespace vdb = container::geometry::vdb;
class OpenVdbTransportRenderJob : public ::testing::Test {
protected:
  std::filesystem::path dir;
  void SetUp() override {
    static std::atomic_uint64_t serial{};
    dir = std::filesystem::temp_directory_path() /
          ("container-render-job-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(dir));
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
      if (entry.is_regular_file())
        std::filesystem::remove(entry.path(), e);
    std::filesystem::remove(dir, e);
  }
  void write(const std::filesystem::path &p, const std::string &text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
    ASSERT_TRUE(f.good());
  }
  auto options() {
    vdb::TransportRenderJobOptions o;
    o.sceneSpec = dir / "scene.json";
    o.output = dir / "image.exr";
    o.image.transport.samplesPerRay = 8;
    return o;
  }
  std::string vacuum =
      R"({"schemaVersion":1,"camera":{"width":2,"height":1},"environment":[0.25,0.5,0.75]})";
  auto bytes(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), {});
  }
};
TEST_F(OpenVdbTransportRenderJob, VacuumIsAvailableWithoutVdbAndOutputIsExclusive) {
  auto o = options();
  write(o.sceneSpec, vacuum);
  const auto original = bytes(o.sceneSpec);
  auto result = vdb::RunTransportRenderJob(o);
  ASSERT_EQ(result.image.pixels.size(), 2u);
  EXPECT_EQ(result.output, o.output);
  EXPECT_TRUE(result.sources.empty());
  for (const auto &p : result.image.pixels)
    EXPECT_EQ(p.radiance, (vdb::TransportVector{.25, .5, .75}));
  EXPECT_EQ(result.exported.encodedBytes, std::filesystem::file_size(o.output));
  EXPECT_EQ(original, bytes(o.sceneSpec));
  const auto before = bytes(o.output);
  EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
  EXPECT_EQ(before, bytes(o.output));
}
TEST_F(OpenVdbTransportRenderJob, ExternalCapsAreCheckedBeforeFilesAndCannotBeRaisedBySpec) {
  auto o = options();
  o.image.transport.maxWork = 0;
  try {
    (void)vdb::RunTransportRenderJob(o);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("caps"), std::string::npos);
  }
  o = options();
  o.maxSpecBytes = 0;
  EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
  o = options();
  write(o.sceneSpec, R"({"schemaVersion":1,"camera":{"width":8,"height":8}})");
  o.image.maxPixels = 4;
  EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
  EXPECT_FALSE(std::filesystem::exists(o.output));
  o = options();
  write(o.sceneSpec, R"({"schemaVersion":1,"maxWork":1000000000})");
  EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
  write(o.sceneSpec, vacuum);
  o.maxSpecBytes = 8;
  EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
  o = options();
  o.image.maxPaths = 8;
  EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
  o = options();
  o.maxSceneBytes = 1;
  EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
  EXPECT_FALSE(std::filesystem::exists(o.output));
}
TEST_F(OpenVdbTransportRenderJob, RelativeSpecAndOutputAreCapturedAsAbsoluteSnapshots) {
  const auto local = std::filesystem::current_path() /
                     ("container-relative-render-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directory(local));
  struct Cleanup {
    std::filesystem::path p;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove(p / "scene.json", e);
      std::filesystem::remove(p / "image.exr", e);
      std::filesystem::remove(p, e);
    }
  } cleanup{local};
  auto o = options();
  o.sceneSpec = local.filename() / "scene.json";
  o.output = local.filename() / "image.exr";
  write(local / "scene.json", vacuum);
  const auto result = vdb::RunTransportRenderJob(o);
  EXPECT_TRUE(result.output.is_absolute());
  EXPECT_EQ(result.output, local / "image.exr");
  EXPECT_TRUE(std::filesystem::exists(result.output));
}
TEST_F(OpenVdbTransportRenderJob, StrictNumbersSchemaAndDuplicateKeysFailWithoutPublication) {
  auto o = options();
  for (const std::string input :
       {R"({"schemaVersion":1,"environment":[1e-999,0,0]})",
        R"({"schemaVersion":1,"schemaVersion":1})",
        R"({"schemaVersion":1,"camera":{"target":[0,"bad",0]}})",
        R"({"schemaVersion":1,"environment":[1e999,0,0]})", R"({"schemaVersion":1} {})"}) {
    SCOPED_TRACE(input);
    write(o.sceneSpec, input);
    EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
    EXPECT_FALSE(std::filesystem::exists(o.output));
  }
}
TEST_F(OpenVdbTransportRenderJob, UnicodeSpecAndOutputPathsRemainNative) {
  auto o = options();
  o.sceneSpec = dir / std::filesystem::path(u8"场景-ışık.json");
  o.output = dir / std::filesystem::path(u8"体积-ışık.exr");
  write(o.sceneSpec, vacuum);
  auto result = vdb::RunTransportRenderJob(o);
  EXPECT_EQ(result.output, o.output);
  EXPECT_TRUE(std::filesystem::exists(o.output));
}
TEST_F(OpenVdbTransportRenderJob, SourceAvailabilityIsRequiredOnlyForVdbMedia) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  auto o = options();
  write(o.sceneSpec, R"({"schemaVersion":1,"media":[{"input":"missing.vdb"}]})");
  try {
    (void)vdb::RunTransportRenderJob(o);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(o.output));
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
TEST_F(OpenVdbTransportRenderJob, RelativeSourcesUseOriginalSpecParentAndDensityCapIsExternal) {
  openvdb::initialize();
  auto grid = openvdb::FloatGrid::create(0);
  grid->setName("density");
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->tree().setValue({0, 0, 0}, 1);
  {
    openvdb::io::File f((dir / "density.vdb").string());
    f.write({grid});
    f.close();
  }
  auto o = options();
  write(
      o.sceneSpec,
      R"({"schemaVersion":1,"camera":{"width":1,"height":1},"environment":[0.25,0.5,0.75],"media":[{"input":"density.vdb","maxResolution":16,"extinction":0}]})");
  const auto before = bytes(dir / "density.vdb");
  auto result = vdb::RunTransportRenderJob(o);
  ASSERT_EQ(result.sources.size(), 1u);
  EXPECT_EQ(result.sources[0].input, dir / "density.vdb");
  EXPECT_EQ(result.sources[0].gridName, "density");
  EXPECT_EQ(before, bytes(dir / "density.vdb"));
  o.output = dir / "capped.exr";
  o.maxDensityValues = 1;
  EXPECT_THROW((void)vdb::RunTransportRenderJob(o), std::exception);
  EXPECT_FALSE(std::filesystem::exists(o.output));
}
#endif
} // namespace
