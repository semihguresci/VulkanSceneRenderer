#include "Container/geometry/OpenVdbSegmentation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/LevelSetUtil.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbSegmentationAvailability, DisabledBuildFailsBeforeInputAccess) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  const auto output = std::filesystem::temp_directory_path() / "container-disabled-segments-do-not-create.vdb";
  try {
    (void)vdb::WriteSegmentedLevelSet("missing-input.vdb", output);
    FAIL() << "disabled segmentation should throw";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(output));
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbSegmentation : public ::testing::Test {
 protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-segmentation-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.maxVoxelCount = 1'000'000;
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  openvdb::FloatGrid::Ptr sphere(float radius = 1, openvdb::Vec3f center = {}) const {
    auto grid = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(radius, center, .2f, 3.f);
    grid->setName("input");
    return grid;
  }
  openvdb::FloatGrid::Ptr pair(float leftRadius = 1, float rightRadius = 1,
                              float distance = 6) const {
    auto left = sphere(leftRadius, {-distance / 2, 0, 0});
    auto right = sphere(rightRadius, {distance / 2, 0, 0});
    openvdb::tools::csgUnion(*left, *right);
    return left;
  }
  void write(const char* name, const openvdb::GridPtrVec& grids) const {
    openvdb::io::File output(file(name).string());
    output.write(grids);
    output.close();
  }
  std::vector<openvdb::FloatGrid::Ptr> read(const char* name) const {
    openvdb::io::File input(file(name).string());
    input.open(false);
    const auto grids = input.getGrids();
    input.close();
    std::vector<openvdb::FloatGrid::Ptr> result;
    for (const auto& grid : *grids) result.push_back(openvdb::gridPtrCast<openvdb::FloatGrid>(grid));
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a->getName() < b->getName(); });
    return result;
  }
  static float sample(const openvdb::FloatGrid& grid, openvdb::Vec3d point) {
    return grid.tree().getValue(grid.transform().worldToIndexCellCentered(point));
  }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions options;
};

TEST_F(OpenVdbSegmentation, DisjointUnionBecomesIndependentSignedComponentsOrderedBySize) {
  write("source.vdb", {pair(.6f, 1.f)});
  options.gridName = "island";
  const auto results = vdb::WriteSegmentedLevelSet(file("source.vdb"), file("parts.vdb"), "input", options);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].gridName, "island_000");
  EXPECT_EQ(results[1].gridName, "island_001");
  EXPECT_GT(results[0].activeVoxelCount, results[1].activeVoxelCount);
  EXPECT_EQ(results[0].gridClass, "level set");
  const auto grids = read("parts.vdb");
  ASSERT_EQ(grids.size(), 2u);
  EXPECT_LT(sample(*grids[0], {3, 0, 0}), 0);
  EXPECT_GT(sample(*grids[0], {-3, 0, 0}), 0);
  EXPECT_LT(sample(*grids[1], {-3, 0, 0}), 0);
  EXPECT_GT(sample(*grids[1], {3, 0, 0}), 0);
  EXPECT_NEAR(sample(*grids[0], {4.2, 0, 0}), .2, 1e-6);
  EXPECT_NEAR(sample(*grids[1], {-2.2, 0, 0}), .2, 1e-6);
}

TEST_F(OpenVdbSegmentation, TouchingOverlappingAndNestedUnionSurfacesStayConnected) {
  for (float distance : {1.8f, 2.f}) {
    SCOPED_TRACE(distance);
    const char* input = distance < 2 ? "overlap.vdb" : "touch.vdb";
    const char* output = distance < 2 ? "overlap-parts.vdb" : "touch-parts.vdb";
    write(input, {pair(1, 1, distance)});
    const auto results = vdb::WriteSegmentedLevelSet(file(input), file(output), {}, options);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_LT(sample(*read(output)[0], {-distance / 2, 0, 0}), 0);
    EXPECT_LT(sample(*read(output)[0], {distance / 2, 0, 0}), 0);
  }
  auto outer = sphere(1.5f), inner = sphere(.5f);
  openvdb::tools::csgUnion(*outer, *inner);
  write("nested-union.vdb", {outer});
  EXPECT_EQ(vdb::WriteSegmentedLevelSet(file("nested-union.vdb"), file("nested-parts.vdb"), {}, options).size(), 1u);
}

TEST_F(OpenVdbSegmentation, NestedCavityNegativeExteriorIsRejectedWithoutChangingSigns) {
  auto outer = sphere(1.6f), cavity = sphere(.6f);
  openvdb::tools::csgDifference(*outer, *cavity);
  write("shell.vdb", {outer});
  const auto original = bytes(file("shell.vdb"));
  std::vector<openvdb::FloatGrid::Ptr> rawSegments;
  openvdb::tools::segmentSDF(*outer, rawSegments);
  ASSERT_EQ(rawSegments.size(), 2u);
  bool foundNegativeExterior = false;
  for (const auto& segment : rawSegments) {
    const auto active = segment->evalActiveVoxelBoundingBox();
    openvdb::CoordBBox negative;
    for (auto value = segment->cbeginValueOff(); value; ++value)
      if (*value < 0) negative.expand(value.getBoundingBox());
    bool outside = false;
    for (size_t axis = 0; axis < 3; ++axis)
      outside |= negative.min()[axis] < active.min()[axis] || negative.max()[axis] > active.max()[axis];
    if (outside) {
      foundNegativeExterior = true;
      std::ostringstream activeText, negativeText;
      activeText << active;
      negativeText << negative;
      RecordProperty("cavityActiveBounds", activeText.str());
      RecordProperty("cavityNegativeBounds", negativeText.str());
    }
  }
  ASSERT_TRUE(foundNegativeExterior);
  try {
    (void)vdb::WriteSegmentedLevelSet(file("shell.vdb"), file("shell-parts.vdb"), {}, options);
    FAIL() << "an unbounded negative exterior cannot be preserved as an isolated component";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("negative-exterior"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("positive-exterior"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("shell-parts.vdb")));
  EXPECT_EQ(bytes(file("shell.vdb")), original);
  EXPECT_GT(sample(*outer, {0, 0, 0}), 0);
  EXPECT_LT(sample(*outer, {1, 0, 0}), 0);
}

TEST_F(OpenVdbSegmentation, TiesUseIndexBoundsAndPreserveTransformMetadataAndSource) {
  auto source = pair();
  source->transform().postTranslate(openvdb::Vec3d(4, -2, 1));
  source->insertMeta("artist", openvdb::StringMetadata("source metadata"));
  source->insertMeta("tag", openvdb::Int32Metadata(17));
  write("source.vdb", {source});
  const auto original = bytes(file("source.vdb"));
  options.gridName = "equal";
  options.voxelSize = 17; // Generation settings must not resample inputs.
  options.halfWidth = 9;
  const auto first = vdb::WriteSegmentedLevelSet(file("source.vdb"), file("first.vdb"), {}, options);
  const auto second = vdb::WriteSegmentedLevelSet(file("source.vdb"), file("second.vdb"), {}, options);
  ASSERT_EQ(first.size(), 2u);
  ASSERT_EQ(second.size(), first.size());
  EXPECT_EQ(first[0].activeVoxelCount, first[1].activeVoxelCount);
  const auto a = read("first.vdb"), b = read("second.vdb");
  ASSERT_EQ(a.size(), 2u);
  EXPECT_LT(a[0]->evalActiveVoxelBoundingBox().min().x(), a[1]->evalActiveVoxelBoundingBox().min().x());
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i]->transform(), source->transform());
    EXPECT_FLOAT_EQ(a[i]->background(), source->background());
    EXPECT_EQ(a[i]->metaValue<std::string>("artist"), "source metadata");
    EXPECT_EQ(a[i]->metaValue<int32_t>("tag"), 17);
    EXPECT_EQ(a[i]->getGridClass(), openvdb::GRID_LEVEL_SET);
    EXPECT_EQ(a[i]->getName(), b[i]->getName());
    EXPECT_EQ(a[i]->evalActiveVoxelBoundingBox(), b[i]->evalActiveVoxelBoundingBox());
    EXPECT_TRUE(a[i]->tree().hasSameTopology(b[i]->tree()));
    for (auto value = a[i]->cbeginValueAll(); value; ++value)
      EXPECT_FLOAT_EQ(*value, b[i]->tree().getValue(value.getCoord()));
  }
  EXPECT_LT(sample(*a[0], {1, -2, 1}), 0);
  EXPECT_GT(sample(*a[0], {7, -2, 1}), 0);
  EXPECT_EQ(bytes(file("source.vdb")), original);
}

TEST_F(OpenVdbSegmentation, ComponentLimitIsCheckedByZeroMaskBeforeUpstreamGraphAllocation) {
  auto grid = pair(.6f, .6f);
  auto third = sphere(.6f, {0, 3, 0});
  openvdb::tools::csgUnion(*grid, *third);
  write("three.vdb", {grid});
  try {
    (void)vdb::WriteSegmentedLevelSet(file("three.vdb"), file("bad.vdb"), {}, options, {.maxComponents = 2});
    FAIL() << "a third zero surface must fail the prepass";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("max-components"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbSegmentation, FileTopologyAndWorkspaceBudgetsRejectBeforeWriting) {
  write("source.vdb", {pair()});
  for (auto limit : {uintmax_t(20), uintmax_t(8 * 1024 * 1024)}) {
    SCOPED_TRACE(limit);
    auto bounded = options;
    bounded.maxInputBytes = limit;
    EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("bad.vdb"), {}, bounded), std::invalid_argument);
  }
  auto bounded = options;
  bounded.maxVoxelCount = 100;
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("bad.vdb"), {}, bounded), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbSegmentation, SharedLeafStorageIsCountedAcrossAllOutputComponents) {
  const auto source = pair(1, 1, 2.8f);
  write("source.vdb", {source});
  (void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("parts.vdb"), {}, options);
  const auto parts = read("parts.vdb");
  ASSERT_EQ(parts.size(), 2u);
  const uint64_t sourceLeaves = source->tree().leafCount();
  const uint64_t outputLeaves = uint64_t(parts[0]->tree().leafCount()) + parts[1]->tree().leafCount();
  ASSERT_GT(outputLeaves, sourceLeaves);
  auto bounded = options;
  bounded.maxVoxelCount = sourceLeaves * openvdb::FloatTree::LeafNodeType::NUM_VALUES;
  try {
    (void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("bad.vdb"), {}, bounded);
    FAIL() << "sum of output leaves must fit the shared budget";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("summed"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbSegmentation, ComponentMetadataCopiesAreIncludedBeforeUpstreamSegmentation) {
  auto source = pair();
  write("plain.vdb", {source});
  auto bounded = options;
  bounded.maxInputBytes = 48ull * 1024 * 1024;
  ASSERT_EQ(vdb::WriteSegmentedLevelSet(file("plain.vdb"), file("plain-parts.vdb"), {}, bounded).size(), 2u);
  source->insertMeta("payload", openvdb::StringMetadata(std::string(2 * 1024 * 1024, 'x')));
  write("metadata.vdb", {source});
  try {
    (void)vdb::WriteSegmentedLevelSet(file("metadata.vdb"), file("bad.vdb"), {}, bounded);
    FAIL() << "component metadata deep copies must fit the workspace budget";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("component/graph workspace"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbSegmentation, InvalidLimitsAndProtectedOutputPathsFailWithoutChangingFiles) {
  write("source.vdb", {sphere()});
  const auto original = bytes(file("source.vdb"));
  for (uint32_t count : {0u, 1025u})
    EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("bad.vdb"), {}, options,
        {.maxComponents = count}), std::invalid_argument);
  for (int which : {0, 1, 2, 3}) {
    auto invalid = options;
    if (which == 0) invalid.maxVoxelCount = 0;
    if (which == 1) invalid.maxInputBytes = 0;
    if (which == 2) invalid.gridName.clear();
    if (which == 3) invalid.gridName = std::string(257, 'a');
    EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("bad.vdb"), {}, invalid), std::invalid_argument);
  }
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("source.vdb"), {}, options), std::invalid_argument);
  std::ofstream(file("protected.vdb")) << "preserve";
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("protected.vdb"), {}, options), std::invalid_argument);
  EXPECT_EQ(bytes(file("protected.vdb")), "preserve");
  EXPECT_EQ(bytes(file("source.vdb")), original);
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("missing.vdb"), file("bad.vdb"), {}, options), std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("source.vdb"), file("missing") / "out.vdb", {}, options), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbSegmentation, EmptyNoCrossingAndUnsupportedGridsAreExplicitlyRejected) {
  auto empty = openvdb::FloatGrid::create(.6f);
  empty->setTransform(openvdb::math::Transform::createLinearTransform(.2));
  empty->setGridClass(openvdb::GRID_LEVEL_SET);
  empty->setName("input");
  write("empty.vdb", {empty});
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("empty.vdb"), file("bad.vdb"), {}, options), std::invalid_argument);
  empty->getAccessor().setValue(openvdb::Coord(0), .1f);
  write("no-crossing.vdb", {empty});
  try {
    (void)vdb::WriteSegmentedLevelSet(file("no-crossing.vdb"), file("bad.vdb"), {}, options);
    FAIL() << "a nonempty positive-only grid has no zero surface";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("zero-crossing"), std::string::npos);
  }
  auto fog = sphere();
  fog->setGridClass(openvdb::GRID_FOG_VOLUME);
  write("fog.vdb", {fog});
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("fog.vdb"), file("bad.vdb"), {}, options), std::invalid_argument);
  auto precise = openvdb::DoubleGrid::create(.6);
  precise->setGridClass(openvdb::GRID_LEVEL_SET);
  precise->setName("input");
  write("double.vdb", {precise});
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("double.vdb"), file("bad.vdb"), {}, options), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbSegmentation, NonfiniteOutOfBandActiveTilesAndNonuniformTransformsFail) {
  for (int which : {0, 1, 2, 3}) {
    auto grid = sphere();
    if (which == 0) grid->getAccessor().setValueOff(openvdb::Coord(30), std::numeric_limits<float>::quiet_NaN());
    if (which == 1) grid->getAccessor().setValue(openvdb::Coord(0), 1.f);
    if (which == 2) grid->tree().addTile(1, openvdb::Coord(0), .1f, true);
    if (which == 3) grid->setTransform(openvdb::math::Transform::Ptr(
        new openvdb::math::Transform(openvdb::math::MapBase::Ptr(
            new openvdb::math::ScaleMap(openvdb::Vec3d(.2, .4, .2))))));
    const auto name = "invalid-" + std::to_string(which) + ".vdb";
    write(name.c_str(), {grid});
    EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file(name.c_str()), file("bad.vdb"), {}, options), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
  }
}

TEST_F(OpenVdbSegmentation, ExactDescriptorSelectorResolvesDuplicateDisplayNames) {
  auto a = sphere(.6f, {-3, 0, 0}), b = sphere(.6f, {3, 0, 0});
  write("duplicate.vdb", {a, b});
  EXPECT_THROW((void)vdb::WriteSegmentedLevelSet(file("duplicate.vdb"), file("bad.vdb"), "input", options), std::invalid_argument);
  openvdb::io::File input(file("duplicate.vdb").string());
  input.open(false);
  const auto descriptor = input.beginName().gridName();
  const auto selected = openvdb::gridPtrCast<openvdb::FloatGrid>(input.readGrid(descriptor));
  input.close();
  const auto results = vdb::WriteSegmentedLevelSet(file("duplicate.vdb"), file("selected.vdb"),
      "descriptor:" + descriptor, options);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(read("selected.vdb")[0]->evalActiveVoxelBoundingBox(), selected->evalActiveVoxelBoundingBox());
}
#endif
}  // namespace
