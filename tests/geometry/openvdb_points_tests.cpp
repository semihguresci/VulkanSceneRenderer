#include "Container/geometry/OpenVdbPoints.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/points/PointGroup.h>
#include <nlohmann/json.hpp>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbPointsAvailability, DisabledApisFailBeforeFileAccess) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  const auto output = std::filesystem::temp_directory_path() / "container-disabled-points-do-not-create.vdb";
  try { (void)vdb::WritePoints({}, output); FAIL(); }
  catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  EXPECT_THROW((void)vdb::WritePointsFile("missing.json", output), std::runtime_error);
  EXPECT_THROW((void)vdb::ReadPoints("missing.vdb"), std::runtime_error);
  EXPECT_THROW((void)vdb::ExportPoints("missing.vdb", output), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(output));
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbPoints : public ::testing::Test {
 protected:
  using Grid = openvdb::points::PointDataGrid;
  void SetUp() override {
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() / ("container-vdb-points-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.voxelSize = .1;
    options.gridName = "points";
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  std::filesystem::path json(const std::string& value) const {
    std::ofstream(file("input.json")) << value;
    return file("input.json");
  }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  Grid::Ptr readGrid(const char* name) const {
    openvdb::io::File input(file(name).string());
    input.open(false);
    const auto grid = openvdb::gridPtrCast<Grid>(input.readGrid(options.gridName));
    input.close();
    return grid;
  }
  void direct(const char* name, const openvdb::GridPtrVec& grids) const {
    openvdb::io::File output(file(name).string());
    output.write(grids); output.close();
  }
  static void same(const std::vector<vdb::PointRecord>& a, const std::vector<vdb::PointRecord>& b,
                   double tolerance = 1e-7) {
    ASSERT_EQ(a.size(), b.size());
    for (size_t index = 0; index < a.size(); ++index) {
      for (size_t axis = 0; axis < 3; ++axis) {
        EXPECT_NEAR(a[index].position[axis], b[index].position[axis], tolerance);
        EXPECT_DOUBLE_EQ(a[index].velocity[axis], b[index].velocity[axis]);
      }
      EXPECT_DOUBLE_EQ(a[index].radius, b[index].radius);
    }
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions options;
};

TEST_F(OpenVdbPoints, RemoteVoxelOriginRetainsSmallLocalizedOffsetsAfterWorldRecentering) {
  options.voxelSize=1;
  const std::vector<vdb::PointRecord> initial{{{1000000000,0,0},{1,2,3},.25}};
  (void)vdb::WritePoints(initial,file("remote.vdb"),options);
  auto grid=readGrid("remote.vdb");
  auto leaf=grid->tree().beginLeaf(); ASSERT_TRUE(leaf);
  auto point=leaf->beginIndexAll(); ASSERT_TRUE(point);
  constexpr float offset=.1234567f;
  openvdb::points::AttributeWriteHandle<openvdb::Vec3f> position(leaf->attributeArray("P"));
  position.set(*point,openvdb::Vec3f(offset,-.2f,.3f));
  grid->transform().postTranslate(openvdb::Vec3d(-1000000000,0,0));
  direct("recentered.vdb",{grid});
  const auto before=bytes(file("recentered.vdb"));
  const auto actual=vdb::ReadPoints(file("recentered.vdb"));
  ASSERT_EQ(actual.size(),1u);
  EXPECT_DOUBLE_EQ(actual[0].position[0],double(offset));
  EXPECT_DOUBLE_EQ(actual[0].position[1],double(-.2f));
  EXPECT_DOUBLE_EQ(actual[0].position[2],double(.3f));
  EXPECT_EQ(actual[0].velocity,initial[0].velocity); EXPECT_EQ(actual[0].radius,.25);
  EXPECT_EQ(bytes(file("recentered.vdb")),before);
}

TEST_F(OpenVdbPoints, WritesRealPointDataGridAndPreservesWorldPositionAttributesAndDuplicates) {
  const std::vector<vdb::PointRecord> points{
      {{1.123456789, -.312345678, .019876543}, {1, 2, 3}, .17},
      {{-.123456789, .512345678, -.419876543}, {-4, 5, -6}, .43},
      {{1.123456789, -.312345678, .019876543}, {7, 8, 9}, .91}};
  const auto result = vdb::WritePoints(points, file("points.vdb"), options);
  EXPECT_EQ(result.gridName, "points"); EXPECT_EQ(result.pointCount, 3u); EXPECT_DOUBLE_EQ(result.voxelSize, .1);
  const auto grid = readGrid("points.vdb");
  ASSERT_TRUE(grid);
  EXPECT_TRUE(grid->isType<Grid>());
  EXPECT_EQ(grid->voxelSize(), openvdb::Vec3d(.1));
  const auto leaf = grid->tree().cbeginLeaf();
  ASSERT_TRUE(leaf);
  EXPECT_TRUE((leaf->constAttributeArray("P").isType<openvdb::points::TypedAttributeArray<openvdb::Vec3f, openvdb::points::NullCodec>>()));
  EXPECT_TRUE((leaf->constAttributeArray("v").isType<openvdb::points::TypedAttributeArray<openvdb::Vec3d, openvdb::points::NullCodec>>()));
  EXPECT_TRUE((leaf->constAttributeArray("pscale").isType<openvdb::points::TypedAttributeArray<double, openvdb::points::NullCodec>>()));
  const auto actual = vdb::ReadPoints(file("points.vdb"), "points", options);
  std::vector<vdb::PointRecord> expected{points[1], points[0], points[2]};
  same(actual, expected);
}

TEST_F(OpenVdbPoints, AffineReadbackTransformsPositionsAndKeepsWorldVelocityAndScale) {
  const std::vector<vdb::PointRecord> points{{{.123, -.234, .345}, {3, -4, 5}, .17}, {{1.1, .4, -.2}, {6, 7, -8}, .38}};
  (void)vdb::WritePoints(points, file("source.vdb"), options);
  auto grid = readGrid("source.vdb");
  const auto originalTransform = grid->transform().copy();
  grid->transform().postScale(openvdb::Vec3d(2, 3, 4));
  grid->transform().postRotate(.3, openvdb::math::Z_AXIS);
  grid->transform().postTranslate(openvdb::Vec3d(4, -2, 1));
  direct("transformed.vdb", {grid});
  auto expected = points;
  for (auto& point : expected) {
    const auto index = originalTransform->worldToIndex(openvdb::Vec3d(point.position[0], point.position[1], point.position[2]));
    const auto world = grid->indexToWorld(index);
    point.position = {world.x(), world.y(), world.z()};
  }
  std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
  same(vdb::ReadPoints(file("transformed.vdb"), {}, options), expected, 2e-7);
}

TEST_F(OpenVdbPoints, JsonDefaultsExportAndReimportRoundTripEverySupportedAttribute) {
  const auto input = json(R"([{"position":[1.123,-2.234,3.345]},{"radius":0.37,"velocity":[4,5,6],"position":[-1.5,0.2,0.1]}])");
  const auto original = bytes(input);
  (void)vdb::WritePointsFile(input, file("points.vdb"), options);
  const auto source = bytes(file("points.vdb"));
  const auto expected = vdb::ReadPoints(file("points.vdb"), {}, options);
  ASSERT_EQ(expected.size(), 2u);
  EXPECT_EQ(expected[1].velocity, (std::array<double, 3>{0, 0, 0}));
  EXPECT_DOUBLE_EQ(expected[1].radius, 1);
  const auto result = vdb::ExportPoints(file("points.vdb"), file("export.json"), "points", options);
  EXPECT_EQ(result.pointCount, 2u); EXPECT_EQ(result.gridName, "points");
  const auto parsed = nlohmann::json::parse(bytes(file("export.json")));
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].size(), 3u);
  (void)vdb::WritePointsFile(file("export.json"), file("restored.vdb"), options);
  same(vdb::ReadPoints(file("restored.vdb"), {}, options), expected);
  EXPECT_EQ(bytes(file("points.vdb")), source); EXPECT_EQ(bytes(input), original);
}

TEST_F(OpenVdbPoints, EmptyGridsAndInactiveVoxelPointsAreRetained) {
  (void)vdb::WritePoints({}, file("empty.vdb"), options);
  EXPECT_TRUE(vdb::ReadPoints(file("empty.vdb"), {}, options).empty());
  EXPECT_EQ(vdb::ExportPoints(file("empty.vdb"), file("empty.json"), {}, options).pointCount, 0u);
  EXPECT_TRUE(nlohmann::json::parse(bytes(file("empty.json"))).empty());
  const std::vector<vdb::PointRecord> points{{{0, 0, 0}, {1, 2, 3}, .4}, {{1, 2, 3}, {4, 5, 6}, .7}};
  (void)vdb::WritePoints(points, file("source.vdb"), options);
  auto grid = readGrid("source.vdb");
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) leaf->setValuesOff();
  direct("inactive.vdb", {grid});
  same(vdb::ReadPoints(file("inactive.vdb"), {}, options), points);
}

TEST_F(OpenVdbPoints, RejectsMissingExtraGroupedAndIncompatibleAttributes) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  for (int which : {0, 1, 2, 3, 4}) {
    auto grid = readGrid("source.vdb");
    if (which == 0) openvdb::points::dropAttribute(grid->tree(), std::string("v"));
    if (which == 1) openvdb::points::appendAttribute<float>(grid->tree(), "temperature");
    if (which == 2) openvdb::points::appendGroup(grid->tree(), "selected");
    if (which == 3) {
      openvdb::points::dropAttribute(grid->tree(), std::string("v"));
      openvdb::points::appendAttribute<openvdb::Vec3f>(grid->tree(), "v");
    }
    if (which == 4) {
      openvdb::points::dropAttribute(grid->tree(), std::string("pscale"));
      openvdb::points::appendAttribute<double>(grid->tree(), "pscale", 1., 2);
    }
    const auto name = "layout-" + std::to_string(which) + ".vdb";
    direct(name.c_str(), {grid});
    EXPECT_THROW((void)vdb::ReadPoints(file(name.c_str()), {}, options), std::invalid_argument);
    EXPECT_THROW((void)vdb::ExportPoints(file(name.c_str()), file("bad.json"), {}, options), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(file("bad.json")));
  }
}

TEST_F(OpenVdbPoints, InvalidRecordsNumericScalesCoordinatesAndWorldPrecisionAreRejected) {
  std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  for (int which : {0, 1, 2, 3, 4}) {
    auto bad = records;
    if (which == 0) bad[0].position[0] = std::numeric_limits<double>::quiet_NaN();
    if (which == 1) bad[0].velocity[0] = std::numeric_limits<double>::infinity();
    if (which == 2) bad[0].radius = -1;
    if (which == 3) bad[0].radius = 0;
    if (which == 4) bad[0].position[0] = 1e30;
    EXPECT_THROW((void)vdb::WritePoints(bad, file("bad.vdb"), options), std::invalid_argument);
  }
  for (double size : {1e-6, 1e7, 0., -1.}) {
    auto bad = options; bad.voxelSize = size;
    EXPECT_THROW((void)vdb::WritePoints(records, file("bad.vdb"), bad), std::invalid_argument);
  }
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  auto grid = readGrid("source.vdb");
  grid->transform().postTranslate(openvdb::Vec3d(1e12, 0, 0));
  direct("imprecise.vdb", {grid});
  try { (void)vdb::ReadPoints(file("imprecise.vdb"), {}, options); FAIL(); }
  catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("precision"), std::string::npos); }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbPoints, CountOccupiedVoxelLeafTopologyAndMemoryBudgetsAreEnforced) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}, {{.2, 0, 0}, {}, 1}};
  EXPECT_THROW((void)vdb::WritePoints(records, file("bad.vdb"), options, {.maxPoints = 1}), std::invalid_argument);
  EXPECT_THROW((void)vdb::WritePoints(records, file("bad.vdb"), options, {.maxPoints = 0}), std::invalid_argument);
  auto bounded = options; bounded.maxVoxelCount = 1;
  EXPECT_THROW((void)vdb::WritePoints(records, file("bad.vdb"), bounded), std::invalid_argument);
  bounded = options; bounded.maxInputBytes = 5 * 1024 * 1024;
  EXPECT_THROW((void)vdb::WritePoints(records, file("bad.vdb"), bounded), std::invalid_argument);
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  EXPECT_THROW((void)vdb::ReadPoints(file("source.vdb"), {}, options, {.maxPoints = 1}), std::invalid_argument);
  bounded = options; bounded.maxInputBytes = 16;
  EXPECT_THROW((void)vdb::ReadPoints(file("source.vdb"), {}, bounded), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbPoints, HalfVoxelRoundingUsesTheSameTransformAsUpstreamPartitioning) {
  options.voxelSize = .07;
  options.maxVoxelCount = 512;
  const std::vector<vdb::PointRecord> records{{{2.17, 0, 0}, {}, 1}, {{2.205, 0, 0}, {}, 1}};
  // x/dx rounds the second point to31, but x*(1/dx), used by the OpenVDB
  // transform, is exactly31.5 and rounds to32 in the next8-voxel leaf.
  EXPECT_THROW((void)vdb::WritePoints(records, file("bad.vdb"), options), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
  options.maxVoxelCount = 1024;
  (void)vdb::WritePoints(records, file("accepted.vdb"), options);
  EXPECT_EQ(readGrid("accepted.vdb")->tree().leafCount(), 2u);
  same(vdb::ReadPoints(file("accepted.vdb"), {}, options), records);
}

TEST_F(OpenVdbPoints, StrictJsonSchemaCountAndTokenLimitsFailWithoutOutput) {
  const std::vector<std::string> invalid{"{}", "null", "[1]", "[{}]",
      R"([{"position":[0,0]}])", R"([{"position":[0,0,0,0]}])",
      R"([{"position":[0,0,"0"]}])", R"([{"position":[0,0,0],"radius":true}])",
      R"([{"position":[0,0,0],"velocity":[1,2]}])", R"([{"position":[0,0,0],"radius":0}])",
      R"([{"position":[0,0,0],"extra":1}])", R"([{"position":[0,0,0],"position":[1,2,3]}])",
      R"([{"position":[0,0,0],"velocity":[1,2,3],"velocity":[4,5,6]}])",
      R"([{"position":[0,0,0],"radius":1,"radius":2}])", R"([{"position":[0,0,1e1000]}])",
      R"([{"position":[0,0,0]}] trailing)"};
  for (const auto& text : invalid) {
    SCOPED_TRACE(text);
    EXPECT_THROW((void)vdb::WritePointsFile(json(text), file("bad.vdb"), options), std::invalid_argument);
  }
  EXPECT_THROW((void)vdb::WritePointsFile(json(R"([{"position":[0,0,0]},{"position":[1,2,3]}])"),
      file("bad.vdb"), options, {.maxPoints = 1}), std::invalid_argument);
  try {
    (void)vdb::WritePointsFile(json("[{\"position\":[0,0,1." + std::string(5000, '0') + "]}]"), file("bad.vdb"), options);
    FAIL();
  } catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("128-byte"), std::string::npos); }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbPoints, MalformedAttributeValuesAndOffsetsFailBeforeJsonPublication) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  for (int which : {0, 1, 2, 3}) {
    auto grid = readGrid("source.vdb");
    auto leaf = grid->tree().beginLeaf();
    if (which == 0) {
      openvdb::points::AttributeWriteHandle<openvdb::Vec3f> handle(leaf->attributeArray("P"));
      handle.set(0, openvdb::Vec3f(1, 0, 0));
    }
    if (which == 1) {
      openvdb::points::AttributeWriteHandle<openvdb::Vec3d> handle(leaf->attributeArray("v"));
      handle.set(0, openvdb::Vec3d(std::numeric_limits<double>::quiet_NaN()));
    }
    if (which == 2) {
      openvdb::points::AttributeWriteHandle<double> handle(leaf->attributeArray("pscale"));
      handle.set(0, -1);
    }
    if (which == 3) leaf->setOffsetOnly(Grid::TreeType::LeafNodeType::NUM_VALUES - 1, 0);
    const auto name = "malformed-" + std::to_string(which) + ".vdb";
    direct(name.c_str(), {grid});
    EXPECT_ANY_THROW((void)vdb::ExportPoints(file(name.c_str()), file("bad.json"), {}, options));
    EXPECT_FALSE(std::filesystem::exists(file("bad.json")));
  }
}

TEST_F(OpenVdbPoints, RejectsStreamingArchivesAndNonPointGrids) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  {
    std::ofstream stream(file("stream.vdb"), std::ios::binary);
    openvdb::io::Stream archive(stream);
    archive.write(openvdb::GridPtrVec{readGrid("source.vdb")});
  }
  try { (void)vdb::ReadPoints(file("stream.vdb"), {}, options); FAIL(); }
  catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("offset"), std::string::npos); }
  direct("float.vdb", {openvdb::FloatGrid::create()});
  EXPECT_THROW((void)vdb::ReadPoints(file("float.vdb"), {}, options), std::invalid_argument);
}

TEST_F(OpenVdbPoints, ExistingOutputsSourceFilesAndPrivateTemporaryFilesRemainProtected) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  const auto original = bytes(file("source.vdb"));
  EXPECT_THROW((void)vdb::WritePoints(records, file("source.vdb"), options), std::invalid_argument);
  EXPECT_THROW((void)vdb::ExportPoints(file("source.vdb"), file("source.vdb"), {}, options), std::invalid_argument);
  std::ofstream(file("protected.json")) << "preserve";
  EXPECT_THROW((void)vdb::ExportPoints(file("source.vdb"), file("protected.json"), {}, options), std::invalid_argument);
  EXPECT_EQ(bytes(file("protected.json")), "preserve"); EXPECT_EQ(bytes(file("source.vdb")), original);
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    EXPECT_FALSE(entry.path().filename().string().starts_with(".container-vdb-"));
}

TEST_F(OpenVdbPoints, DescriptorSelectionIsExactForDuplicateNames) {
  const std::vector<vdb::PointRecord> records{{{-1, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  auto a = readGrid("source.vdb"), b = a->deepCopy();
  b->transform().postTranslate(openvdb::Vec3d(2, 0, 0));
  direct("duplicate.vdb", {a, b});
  EXPECT_THROW((void)vdb::ReadPoints(file("duplicate.vdb"), "points", options), std::invalid_argument);
  openvdb::io::File input(file("duplicate.vdb").string()); input.open(false);
  const auto descriptor = input.beginName().gridName(); input.close();
  EXPECT_EQ(vdb::ReadPoints(file("duplicate.vdb"), "descriptor:" + descriptor, options).size(), 1u);
}

TEST_F(OpenVdbPoints, CollidingDescriptorsAreRejectedBeforeAnyNamedMetadataRead) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  auto a = readGrid("source.vdb"), b = a->deepCopy(), literal = a->deepCopy();
  literal->setName("points[0]");
  direct("collision.vdb", {a, b, literal});
  for (const std::string& selection : {std::string{}, std::string("descriptor:points[0]")}) {
    try { (void)vdb::ReadPoints(file("collision.vdb"), selection, options); FAIL(); }
    catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find("colliding grid descriptors"), std::string::npos);
    }
  }
  EXPECT_THROW((void)vdb::ExportPoints(file("collision.vdb"), file("bad.json"), {}, options), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.json")));
}

TEST_F(OpenVdbPoints, CustomMetadataPayloadCountsTowardReadbackWorkspace) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  auto bounded = options;
  bounded.maxInputBytes = 12ull * 1024 * 1024;
  EXPECT_EQ(vdb::ReadPoints(file("source.vdb"), {}, bounded).size(), 1u);
  auto grid = readGrid("source.vdb");
  grid->insertMeta("payload", openvdb::StringMetadata(std::string(2 * 1024 * 1024, 'x')));
  direct("metadata.vdb", {grid});
  try { (void)vdb::ReadPoints(file("metadata.vdb"), {}, bounded); FAIL(); }
  catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("workspace"), std::string::npos);
  }
}

TEST_F(OpenVdbPoints, MissingOrInvalidGridStatisticsAreRejectedBeforeTreeReadback) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  for (int which : {0, 1, 2, 3}) {
    auto grid = readGrid("source.vdb");
    const char* key = which % 2 == 0 ? openvdb::GridBase::META_FILE_MEM_BYTES : openvdb::GridBase::META_FILE_VOXEL_COUNT;
    if (which < 2) grid->removeMeta(key);
    else grid->insertMeta(key, openvdb::Int64Metadata(-1));
    const auto name = "stats-" + std::to_string(which) + ".vdb";
    openvdb::io::File output(file(name.c_str()).string());
    output.setGridStatsMetadataEnabled(false);
    output.write(openvdb::GridPtrVec{grid}); output.close();
    try { (void)vdb::ReadPoints(file(name.c_str()), {}, options); FAIL(); }
    catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find("grid statistics enabled"), std::string::npos);
    }
  }
}

TEST_F(OpenVdbPoints, DelayedAttributeLengthsArePreflightedWithoutCustomCountMetadata) {
  std::vector<vdb::PointRecord> records;
  for (int index = 0; index < 4096; ++index)
    records.push_back({{.04 * index / 4096, 0, 0}, {double(index), double(-index), .5}, 1 + .001 * index});
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  openvdb::io::File source(file("source.vdb").string());
  source.open(true);
  auto grid = openvdb::gridPtrCast<Grid>(source.readGrid("points"));
  source.close();
  const auto leaf = grid->tree().cbeginLeaf();
  ASSERT_TRUE(leaf);
  const auto& position = leaf->constAttributeArray("P");
  EXPECT_FALSE(position.isDataLoaded());
  EXPECT_GT(position.memUsageIfLoaded(), position.memUsage());
  EXPECT_EQ(position.size(), records.size());
  grid->removeMeta("container:point_count");
  direct("rewritten.vdb", {grid});
  EXPECT_THROW((void)vdb::ReadPoints(file("rewritten.vdb"), {}, options, {.maxPoints = 4095}), std::invalid_argument);
  auto bounded = options;
  bounded.maxInputBytes = 4 * 1024 * 1024 + 500'000;
  EXPECT_THROW((void)vdb::ReadPoints(file("rewritten.vdb"), {}, bounded), std::invalid_argument);
  same(vdb::ReadPoints(file("rewritten.vdb"), {}, options), records);
}

TEST_F(OpenVdbPoints, GloballyDisabledDelayedLoadingHasActionableReadbackDiagnostic) {
  const std::vector<vdb::PointRecord> records{{{0, 0, 0}, {}, 1}};
  (void)vdb::WritePoints(records, file("source.vdb"), options);
  struct EnvironmentGuard {
    bool present{false};
    std::string value;
    EnvironmentGuard() {
      if (const auto* original = std::getenv("OPENVDB_DISABLE_DELAYED_LOAD")) { present = true; value = original; }
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", "1");
#else
      setenv("OPENVDB_DISABLE_DELAYED_LOAD", "1", 1);
#endif
    }
    ~EnvironmentGuard() {
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", present ? value.c_str() : "");
#else
      if (present) setenv("OPENVDB_DISABLE_DELAYED_LOAD", value.c_str(), 1);
      else unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
    }
  } environment;
  try { (void)vdb::ReadPoints(file("source.vdb"), {}, options); FAIL(); }
  catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("unset OPENVDB_DISABLE_DELAYED_LOAD"), std::string::npos);
  }
}
#endif
}  // namespace
