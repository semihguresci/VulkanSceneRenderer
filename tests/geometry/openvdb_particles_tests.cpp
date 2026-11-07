#include "Container/geometry/OpenVdbParticles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbParticlesAvailability, DisabledBuildHasActionableErrorBeforeReadingInput) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  const auto output = std::filesystem::temp_directory_path() /
      "container-disabled-particles-do-not-create.vdb";
  const std::array<vdb::Particle, 1> particles{{{{0, 0, 0}, 1.0}}};
  try {
    (void)vdb::WriteParticlesLevelSet(particles, output);
    FAIL() << "disabled particle rasterization should throw";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  try {
    (void)vdb::WriteParticlesLevelSetFile("missing-particles.json", output);
    FAIL() << "disabled particle file ingestion should throw";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(output));
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbParticles : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-particles-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    processing.voxelSize = 0.2;
    processing.maxVoxelCount = 1'000'000;
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  std::filesystem::path json(const std::string& text) const {
    const auto path = file("particles.json");
    std::ofstream(path) << text;
    return path;
  }
  openvdb::FloatGrid::Ptr read(const char* name) const {
    openvdb::io::File input(file(name).string());
    input.open(false);
    auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(input.readGrid(processing.gridName));
    input.close();
    return grid;
  }
  static float sample(const openvdb::FloatGrid& grid, const openvdb::Vec3d& world) {
    return grid.tree().getValue(grid.transform().worldToIndexCellCentered(world));
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbParticles, WorldPositionsRadiiAndDistancesSurviveRoundTrip) {
  processing.gridName = "particle_surface";
  const std::array<vdb::Particle, 1> particles{{{{3, -2, 1}, 1.0}}};
  const auto result = vdb::WriteParticlesLevelSet(particles, file("surface.vdb"), processing);
  EXPECT_EQ(result.gridName, "particle_surface");
  EXPECT_EQ(result.gridClass, "level set");
  EXPECT_GT(result.activeVoxelCount, 0u);
  EXPECT_DOUBLE_EQ(result.voxelSize, 0.2);
  const auto grid = read("surface.vdb");
  EXPECT_EQ(grid->getGridClass(), openvdb::GRID_LEVEL_SET);
  EXPECT_EQ(grid->indexToWorld(openvdb::Vec3d(0)), openvdb::Vec3d(0));
  EXPECT_EQ(grid->voxelSize(), openvdb::Vec3d(0.2));
  EXPECT_NEAR(grid->background(), 0.6, 1e-6);
  EXPECT_NEAR(sample(*grid, {3.8, -2, 1}), -0.2, 1e-6);
  EXPECT_NEAR(sample(*grid, {4.2, -2, 1}), 0.2, 1e-6);
  EXPECT_LT(sample(*grid, {3, -2, 1}), 0);
  EXPECT_GT(sample(*grid, {0, 0, 0}), 0);
  for (auto value = grid->cbeginValueAll(); value; ++value) {
    EXPECT_TRUE(std::isfinite(*value));
    EXPECT_LE(std::abs(*value), grid->background());
  }
}

TEST_F(OpenVdbParticles, OverlappingAndSeparateSpheresAreUnionedIndependentOfInputOrder) {
  std::array<vdb::Particle, 3> particles{{
      {{0, 0, 0}, 1.0}, {{1.2, 0, 0}, 1.0}, {{-3, 0, 0}, 0.6}}};
  (void)vdb::WriteParticlesLevelSet(particles, file("forward.vdb"), processing);
  std::reverse(particles.begin(), particles.end());
  (void)vdb::WriteParticlesLevelSet(particles, file("reverse.vdb"), processing);
  const auto forward = read("forward.vdb");
  const auto reverse = read("reverse.vdb");
  EXPECT_NEAR(sample(*forward, {0.6, 0, 0}), -0.4, 1e-6);
  EXPECT_LT(sample(*forward, {-3, 0, 0}), 0);
  EXPECT_GT(sample(*forward, {-1.8, 0, 0}), 0);
  EXPECT_NEAR(sample(*forward, {2.4, 0, 0}), 0.2, 1e-6);
  for (int x = -22; x <= 15; ++x)
    for (int y = -7; y <= 7; ++y)
      for (int z = -2; z <= 2; ++z)
        EXPECT_FLOAT_EQ(forward->tree().getValue(openvdb::Coord(x, y, z)),
                        reverse->tree().getValue(openvdb::Coord(x, y, z)));
}

TEST_F(OpenVdbParticles, MinimumUpstreamRadiusIsAcceptedRatherThanSilentlyLost) {
  const std::array<vdb::Particle, 1> particles{{{{0, 0, 0}, 0.3}}};
  (void)vdb::WriteParticlesLevelSet(particles, file("minimum.vdb"), processing);
  const auto grid = read("minimum.vdb");
  EXPECT_LT(sample(*grid, {0, 0, 0}), 0);
  EXPECT_NEAR(sample(*grid, {0.4, 0, 0}), 0.1, 1e-6);
}

TEST_F(OpenVdbParticles, SupportedVoxelScaleEndpointsKeepSignedDistancesInWorldUnits) {
  for (double voxelSize : {1.5e-5, 1e6}) {
    SCOPED_TRACE(voxelSize);
    processing.voxelSize = voxelSize;
    const std::array<vdb::Particle, 1> particles{{
        {{4 * voxelSize, 0, 0}, 5 * voxelSize}}};
    const char* name = voxelSize < 1 ? "small.vdb" : "large.vdb";
    (void)vdb::WriteParticlesLevelSet(particles, file(name), processing);
    const auto grid = read(name);
    EXPECT_NEAR(sample(*grid, {10 * voxelSize, 0, 0}), voxelSize, voxelSize * 1e-5);
    EXPECT_NEAR(sample(*grid, {8 * voxelSize, 0, 0}), -voxelSize, voxelSize * 1e-5);
  }
}

TEST_F(OpenVdbParticles, ReportsParticleIndexAndActualUpstreamRadiusCutoffs) {
  std::array<vdb::Particle, 2> particles{{{{0, 0, 0}, 1.0}, {{2, 0, 0}, 0.299}}};
  for (double radius : {0.299, 20.001}) {
    particles[1].radius = radius;
    try {
      (void)vdb::WriteParticlesLevelSet(particles, file("rejected.vdb"), processing);
      FAIL() << "a particle outside OpenVDB's cutoff should fail";
    } catch (const std::invalid_argument& error) {
      const std::string text(error.what());
      EXPECT_NE(text.find("particle 1"), std::string::npos);
      EXPECT_NE(text.find("[1.5, 100]"), std::string::npos);
      EXPECT_NE(text.find("ignores"), std::string::npos);
    }
  }
  EXPECT_FALSE(std::filesystem::exists(file("rejected.vdb")));
}

TEST_F(OpenVdbParticles, RejectsInvalidParticlesAndNumericOptionsBeforeWriting) {
  std::array<vdb::Particle, 1> particles{{{{0, 0, 0}, 1.0}}};
  const auto output = file("rejected.vdb");
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet({}, output, processing), std::invalid_argument);
  for (double value : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    particles[0].radius = value;
    EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, processing), std::invalid_argument);
  }
  particles[0].radius = 1;
  for (double value : {std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN(), 1e30, -1e30}) {
    particles[0].position[0] = value;
    EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, processing), std::invalid_argument);
  }
  particles[0].position[0] = 0;
  for (double value : {0.0, -1.0, 1e-6, 1e-5, 1e7,
                       std::numeric_limits<double>::quiet_NaN()}) {
    auto invalid = processing;
    invalid.voxelSize = value;
    EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, invalid), std::invalid_argument);
  }
  for (double value : {1.0, 65.0, std::numeric_limits<double>::infinity()}) {
    auto invalid = processing;
    invalid.halfWidth = value;
    EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, invalid), std::invalid_argument);
  }
  auto invalid = processing;
  invalid.gridName.clear();
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, invalid), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(OpenVdbParticles, CountBoundsAndTopologyWorkspaceBudgetsArePreflighted) {
  const std::array<vdb::Particle, 2> particles{{{{0, 0, 0}, 1.0}, {{0.5, 0, 0}, 1.0}}};
  const auto output = file("rejected.vdb");
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, processing,
      {.maxParticles = 1}), std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, processing,
      {.maxParticles = 0}), std::invalid_argument);
  auto invalid = processing;
  invalid.maxVoxelCount = 100;
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, invalid), std::invalid_argument);
  invalid = processing;
  invalid.maxInputBytes = 1'000'000;
  try {
    (void)vdb::WriteParticlesLevelSet(particles, output, invalid);
    FAIL() << "node storage and partitioner workspace must be preflighted";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("workspace"), std::string::npos);
  }
  invalid = processing;
  invalid.maxInputBytes = sizeof(vdb::Particle);
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, invalid), std::invalid_argument);
  const std::array<vdb::Particle, 2> farApart{{{{-1e5, 0, 0}, 1.0}, {{1e5, 0, 0}, 1.0}}};
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet(farApart, output, processing), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(OpenVdbParticles, ValidJsonSupportsEitherFieldOrderAndPreservesSource) {
  const std::string text = R"([{"position":[3,-2,1],"radius":1},{"radius":0.6,"position":[-2,0,0]}])";
  const auto input = json(text);
  (void)vdb::WriteParticlesLevelSetFile(input, file("parsed.vdb"), processing);
  const auto grid = read("parsed.vdb");
  EXPECT_LT(sample(*grid, {3, -2, 1}), 0);
  EXPECT_LT(sample(*grid, {-2, 0, 0}), 0);
  EXPECT_GT(sample(*grid, {0, 0, 0}), 0);
  std::ifstream stream(input);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream), {}), text);
}

TEST_F(OpenVdbParticles, StrictJsonRejectsInvalidSchemaWithoutCreatingOutput) {
  const std::vector<std::string> invalid{
      "{}", "[]", "null", "[1]", "[[0,0,0,1]]", "[{}]",
      R"([{"position":[0,0,0]}])", R"([{"radius":1}])",
      R"([{"position":[0,0],"radius":1}])",
      R"([{"position":[0,0,0,0],"radius":1}])",
      R"([{"position":[0,0,"0"],"radius":1}])",
      R"([{"position":[0,0,0],"radius":true}])",
      R"([{"position":[0,0,0],"radius":null}])",
      R"([{"position":[0,0,0],"radius":1,"extra":3}])",
      R"([{"position":[0,0,0],"position":[1,2,3],"radius":1}])",
      R"([{"position":[0,0,0],"radius":1,"radius":2}])",
      R"([{"position":[0,0,0],"radius":1e1000}])",
      R"([{"position":[0,0,0],"radius":-1}])",
      R"([{"position":[0,0,0],"radius":1}]) trailing)",
      R"([{"position":[0,0,0],"radius":1})"};
  for (const auto& text : invalid) {
    SCOPED_TRACE(text);
    EXPECT_THROW((void)vdb::WriteParticlesLevelSetFile(json(text), file("bad.vdb"), processing),
                 std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
  }
}

TEST_F(OpenVdbParticles, JsonInputAndDecodedCountBudgetsFailBeforeRasterization) {
  const auto output = file("bad.vdb");
  const auto input = json(R"([{"position":[0,0,0],"radius":1},{"position":[1,0,0],"radius":1}])");
  try {
    (void)vdb::WriteParticlesLevelSetFile(input, output, processing, {.maxParticles = 1});
    FAIL() << "SAX decoder must reject count overflow";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("particle count"), std::string::npos);
  }
  auto invalid = processing;
  invalid.maxInputBytes = 20;
  EXPECT_THROW((void)vdb::WriteParticlesLevelSetFile(input, output, invalid), std::invalid_argument);
  invalid.maxInputBytes = 180;
  try {
    (void)vdb::WriteParticlesLevelSetFile(json(R"([{"position":[0,0,0],"radius":1}])"), output, invalid);
    FAIL() << "SAX decoder must preflight decoded particle storage";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("decoded particles"), std::string::npos);
  }
  EXPECT_THROW((void)vdb::WriteParticlesLevelSetFile(file("missing.json"), output, processing),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(OpenVdbParticles, OversizedLexicalTokensAreRejectedBeforeSaxAllocation) {
  const auto input = json("[{\"position\":[0,0,0],\"radius\":1." + std::string(5000, '0') + "}]");
  try {
    (void)vdb::WriteParticlesLevelSetFile(input, file("bad.vdb"), processing);
    FAIL() << "long numeric tokens must fail before SAX stores them";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("128-byte"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbParticles, ExistingOutputsAreNeverOverwritten) {
  const auto output = file("existing.vdb");
  std::ofstream(output) << "keep me";
  const std::array<vdb::Particle, 1> particles{{{{0, 0, 0}, 1.0}}};
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, output, processing), std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteParticlesLevelSetFile(
      json(R"([{"position":[0,0,0],"radius":1}])"), output, processing), std::invalid_argument);
  std::ifstream stream(output);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream), {}), "keep me");
  EXPECT_THROW((void)vdb::WriteParticlesLevelSet(particles, file("missing") / "out.vdb", processing),
               std::invalid_argument);
}
#endif
}  // namespace
