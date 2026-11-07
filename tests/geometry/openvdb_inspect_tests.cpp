#include "Container/geometry/OpenVdbInspect.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/io/io.h>
#include <openvdb/io/Stream.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbInspectionAvailability, DisabledBuildReportsFeatureBeforeInputOrOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  try {
    (void)vdb::InspectFile("missing-inspection-input.vdb", {.maxGrids = 0});
    FAIL() << "disabled inspection must fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbInspection : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-inspect-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  static void write(const std::filesystem::path& path, const openvdb::GridPtrVec& grids,
                    bool savedStatistics = true, const openvdb::MetaMap& metadata = {}) {
    openvdb::io::File output(path.string());
    output.setGridStatsMetadataEnabled(savedStatistics);
    output.write(grids, metadata);
    output.close();
  }
  static openvdb::FloatGrid::Ptr scalar(const std::string& name = "density") {
    auto grid = openvdb::FloatGrid::create(7.0f);
    grid->setName(name);
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    grid->tree().setValue(openvdb::Coord(-2,3,4), 2.0f);
    return grid;
  }
  std::filesystem::path directory;
};

TEST_F(OpenVdbInspection, MetadataOnlyReportsDescriptorsTypesAndRecordedHints) {
  const auto density = scalar();
  density->insertMeta("artist", openvdb::StringMetadata("A \"quoted\" name\nand a new line"));
  auto distance = openvdb::DoubleGrid::create(0.125);
  distance->setName("distance");
  distance->setGridClass(openvdb::GRID_LEVEL_SET);
  auto wind = openvdb::Vec3DGrid::create(openvdb::Vec3d(1,2,3));
  wind->setName("wind");
  wind->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  wind->setIsInWorldSpace(true);
  write(file("metadata.vdb"), {density, distance, wind});
  // Even a much smaller statistics limit must not turn default inspection into
  // a topology load or a statistics-budget rejection.
  const auto report = vdb::InspectFile(file("metadata.vdb"), {.maxActiveVoxels = 1});
  EXPECT_EQ(report.fileBytes, std::filesystem::file_size(file("metadata.vdb")));
  ASSERT_EQ(report.grids.size(), 3u);
  const auto found = std::find_if(report.grids.begin(), report.grids.end(),
      [](const auto& grid) { return grid.name == "density"; });
  ASSERT_NE(found, report.grids.end());
  EXPECT_EQ(found->valueType, "float");
  EXPECT_EQ(found->gridClass, openvdb::GridBase::gridClassToString(openvdb::GRID_FOG_VOLUME));
  ASSERT_TRUE(found->activeVoxelCount);
  EXPECT_EQ(*found->activeVoxelCount, 1u);
  ASSERT_TRUE(found->memoryBytes);
  EXPECT_GT(*found->memoryBytes, 0u);
  ASSERT_TRUE(found->indexBounds);
  EXPECT_EQ(found->indexBounds->minimum, (std::array<int32_t,3>{-2,3,4}));
  EXPECT_EQ(found->indexBounds->maximum, found->indexBounds->minimum);
  EXPECT_FALSE(found->statistics);
  const auto artist = std::find_if(found->metadata.begin(), found->metadata.end(),
      [](const auto& metadata) { return metadata.name == "artist"; });
  ASSERT_NE(artist, found->metadata.end());
  EXPECT_EQ(artist->value, "A \"quoted\" name\nand a new line");
  const auto vector = std::find_if(report.grids.begin(), report.grids.end(),
      [](const auto& grid) { return grid.name == "wind"; });
  ASSERT_NE(vector, report.grids.end());
  EXPECT_EQ(vector->valueType, "vec3d");
  EXPECT_TRUE(vector->inWorldSpace);
  EXPECT_EQ(vector->vectorType, openvdb::GridBase::vecTypeToString(openvdb::VEC_CONTRAVARIANT_RELATIVE));
}

TEST_F(OpenVdbInspection, MissingAndMalformedSavedStatisticsRemainExplicitlyUnavailable) {
  auto grid = scalar();
  write(file("no-hints.vdb"), {grid}, false);
  auto report = vdb::InspectFile(file("no-hints.vdb"));
  ASSERT_EQ(report.grids.size(), 1u);
  EXPECT_FALSE(report.grids[0].activeVoxelCount);
  EXPECT_FALSE(report.grids[0].memoryBytes);
  EXPECT_FALSE(report.grids[0].indexBounds);
  try {
    (void)vdb::InspectFile(file("no-hints.vdb"), {.loadStatistics = true});
    FAIL() << "statistics need recorded allocation guards";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("rewrite"), std::string::npos);
  }
  grid->insertMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT, openvdb::StringMetadata("untrusted count"));
  grid->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES, openvdb::Int64Metadata(-1));
  grid->insertMeta(openvdb::GridBase::META_FILE_BBOX_MIN, openvdb::Vec3IMetadata(openvdb::Vec3i(9)));
  grid->insertMeta(openvdb::GridBase::META_FILE_BBOX_MAX, openvdb::Vec3IMetadata(openvdb::Vec3i(-9)));
  write(file("bad-hints.vdb"), {grid}, false);
  report = vdb::InspectFile(file("bad-hints.vdb"));
  EXPECT_FALSE(report.grids[0].activeVoxelCount);
  EXPECT_FALSE(report.grids[0].memoryBytes);
  EXPECT_FALSE(report.grids[0].indexBounds);
}

TEST_F(OpenVdbInspection, ActiveTilesAreWeightedWithoutExpansionAndInactiveValuesAreExcluded) {
  auto grid = openvdb::FloatGrid::create(11.0f);
  grid->setName("density");
  grid->tree().addTile(1, openvdb::Coord(0), 2.0f, true);
  grid->tree().setValue(openvdb::Coord(10,0,0), 3.0f);
  grid->tree().setValue(openvdb::Coord(11,0,0), 0.0f);
  grid->tree().setValueOff(openvdb::Coord(12,0,0), -100.0f);
  grid->tree().addTile(1, openvdb::Coord(24,0,0), 1000.0f, false);
  write(file("weighted.vdb"), {grid});
  const auto report = vdb::InspectFile(file("weighted.vdb"), {.loadStatistics = true});
  ASSERT_TRUE(report.grids[0].statistics);
  const auto& stats = *report.grids[0].statistics;
  EXPECT_EQ(stats.background, 11.0);
  EXPECT_EQ(stats.count, 514u);
  EXPECT_EQ(stats.minimum, std::optional<double>(0.0));
  EXPECT_EQ(stats.maximum, std::optional<double>(3.0));
  EXPECT_EQ(stats.sum, 1027.0);
  EXPECT_EQ(stats.actualMemoryBytes, grid->memUsage());
  EXPECT_EQ(grid->tree().leafCount(), 1u);
}

TEST_F(OpenVdbInspection, DoubleAndEmptyStatisticsKeepBackgroundSeparateFromActiveValues) {
  auto grid = openvdb::DoubleGrid::create(5.25);
  grid->setName("double");
  grid->tree().setValue(openvdb::Coord(-1,0,0), -2.125);
  grid->tree().setValue(openvdb::Coord(2,0,0), 8.25);
  auto empty = openvdb::FloatGrid::create(-7.0f);
  empty->setName("empty");
  write(file("double.vdb"), {grid, empty});
  const auto report = vdb::InspectFile(file("double.vdb"), {.loadStatistics = true});
  ASSERT_EQ(report.grids.size(), 2u);
  for (const auto& information : report.grids) {
    ASSERT_TRUE(information.statistics);
    const auto& stats = *information.statistics;
    if (information.name == "double") {
      EXPECT_EQ(stats.background, 5.25);
      EXPECT_EQ(stats.count, 2u);
      EXPECT_EQ(stats.minimum, std::optional<double>(-2.125));
      EXPECT_EQ(stats.maximum, std::optional<double>(8.25));
      EXPECT_EQ(stats.sum, 6.125);
    } else {
      EXPECT_EQ(stats.background, -7.0);
      EXPECT_EQ(stats.count, 0u);
      EXPECT_FALSE(stats.minimum);
      EXPECT_FALSE(stats.maximum);
      EXPECT_EQ(stats.sum, 0.0);
      EXPECT_FALSE(stats.indexBounds);
      EXPECT_FALSE(stats.worldBounds);
      EXPECT_FALSE(information.indexBounds);
    }
  }
}

TEST_F(OpenVdbInspection, AffineMatrixAndWorldBoundsUseTransformedCellCorners) {
  auto grid = scalar();
  grid->tree().setValue(openvdb::Coord(3,5,-1), 4.0f);
  grid->transform().postScale(openvdb::Vec3d(-2,3,.5));
  grid->transform().postRotate(.6, openvdb::math::Z_AXIS);
  grid->transform().postTranslate(openvdb::Vec3d(7,-4,2));
  write(file("transform.vdb"), {grid});
  const auto report = vdb::InspectFile(file("transform.vdb"), {.loadStatistics = true});
  const auto& information = report.grids[0];
  EXPECT_TRUE(information.affine);
  ASSERT_TRUE(information.indexToWorld);
  const auto& matrix = *information.indexToWorld;
  const openvdb::Vec3d point(1.25,-.25,4.5);
  const auto expectedPoint = grid->indexToWorld(point);
  for (size_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(matrix[4*axis] * point.x() + matrix[4*axis+1] * point.y() +
                matrix[4*axis+2] * point.z() + matrix[4*axis+3], expectedPoint[axis], 1e-12);
  openvdb::Vec3d low(std::numeric_limits<double>::infinity());
  openvdb::Vec3d high(-std::numeric_limits<double>::infinity());
  const auto bounds = grid->evalActiveVoxelBoundingBox();
  for (unsigned corner = 0; corner < 8; ++corner) {
    openvdb::Vec3d index;
    for (size_t axis = 0; axis < 3; ++axis)
      index[axis] = (corner & (1u << axis)) ? double(bounds.max()[axis]) + .5 : double(bounds.min()[axis]) - .5;
    const auto world = grid->indexToWorld(index);
    for (size_t axis = 0; axis < 3; ++axis) {
      low[axis] = std::min(low[axis], world[axis]);
      high[axis] = std::max(high[axis], world[axis]);
    }
  }
  ASSERT_TRUE(information.worldBounds);
  ASSERT_TRUE(information.statistics->worldBounds);
  for (size_t axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(information.worldBounds->minimum[axis], low[axis], 1e-12);
    EXPECT_NEAR(information.worldBounds->maximum[axis], high[axis], 1e-12);
    EXPECT_NEAR(information.statistics->worldBounds->minimum[axis], low[axis], 1e-12);
    EXPECT_NEAR(information.statistics->worldBounds->maximum[axis], high[axis], 1e-12);
  }
}

TEST_F(OpenVdbInspection, NonlinearTransformHasNoFabricatedMatrixOrWorldEnvelope) {
  auto grid = scalar();
  grid->setTransform(openvdb::math::Transform::createFrustumTransform(
      openvdb::BBoxd(openvdb::Vec3d(-1), openvdb::Vec3d(1)), .5, 10.0, 1.0));
  write(file("nonlinear.vdb"), {grid});
  const auto report = vdb::InspectFile(file("nonlinear.vdb"), {.loadStatistics = true});
  const auto& information = report.grids[0];
  EXPECT_FALSE(information.affine);
  EXPECT_FALSE(information.indexToWorld);
  EXPECT_FALSE(information.worldBounds);
  EXPECT_FALSE(information.statistics->worldBounds);
  EXPECT_FALSE(information.transformDiagnostic.empty());
  EXPECT_EQ(information.statistics->count, 1u);
}

TEST_F(OpenVdbInspection, DuplicateDisplayNamesRequireExactDescriptorSelection) {
  auto first = scalar("duplicate");
  auto second = scalar("duplicate");
  second->tree().setValue(openvdb::Coord(-2,3,4), 9.0f);
  write(file("duplicates.vdb"), {first, second});
  const auto report = vdb::InspectFile(file("duplicates.vdb"));
  ASSERT_EQ(report.grids.size(), 2u);
  EXPECT_THROW((void)vdb::InspectFile(file("duplicates.vdb"), {.gridName = "duplicate"}), std::invalid_argument);
  for (const auto& information : report.grids) {
    const auto selected = vdb::InspectFile(file("duplicates.vdb"),
        {.gridName = "descriptor:" + information.descriptorName, .loadStatistics = true});
    ASSERT_EQ(selected.grids.size(), 1u);
    EXPECT_EQ(selected.grids[0].descriptorName, information.descriptorName);
    openvdb::io::File source(file("duplicates.vdb").string());
    source.open(false);
    const auto actual = openvdb::gridPtrCast<openvdb::FloatGrid>(source.readGrid(information.descriptorName));
    EXPECT_EQ(selected.grids[0].statistics->sum, actual->tree().getValue(openvdb::Coord(-2,3,4)));
  }
  EXPECT_THROW((void)vdb::InspectFile(file("duplicates.vdb"), {.gridName = "missing"}), std::invalid_argument);
}

TEST_F(OpenVdbInspection, UnsupportedStatisticsHavePerGridReasonsAndExplicitRequestsFail) {
  auto wind = openvdb::Vec3SGrid::create(openvdb::Vec3f(1,0,0));
  wind->setName("wind");
  write(file("mixed.vdb"), {scalar(), wind});
  const auto report = vdb::InspectFile(file("mixed.vdb"), {.loadStatistics = true});
  for (const auto& information : report.grids) {
    if (information.name == "wind") {
      EXPECT_FALSE(information.statistics);
      EXPECT_FALSE(information.statisticsUnavailableReason.empty());
    } else EXPECT_TRUE(information.statistics);
  }
  EXPECT_THROW((void)vdb::InspectFile(file("mixed.vdb"), {.gridName = "wind", .loadStatistics = true}),
               std::invalid_argument);
}

TEST_F(OpenVdbInspection, BracketSuffixedNamesCannotSilentlyAliasDuplicateDescriptors) {
  write(file("colliding.vdb"), {scalar("density"), scalar("density"), scalar("density[0]")});
  try {
    (void)vdb::InspectFile(file("colliding.vdb"));
    FAIL() << "colliding descriptor strings must not select another grid";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("collide"), std::string::npos);
  }
}

TEST_F(OpenVdbInspection, NonfiniteScalarsAndOverflowingSumsFailOnlyWhenStatisticsAreLoaded) {
  for (int invalid = 0; invalid < 3; ++invalid) {
    auto grid = scalar();
    if (invalid == 0) grid = openvdb::FloatGrid::create(std::numeric_limits<float>::infinity());
    if (invalid == 1) grid->tree().setValue(openvdb::Coord(0), std::numeric_limits<float>::quiet_NaN());
    if (invalid == 2) grid->tree().setValueOff(openvdb::Coord(0), std::numeric_limits<float>::infinity());
    write(file("nonfinite.vdb"), {grid});
    EXPECT_NO_THROW((void)vdb::InspectFile(file("nonfinite.vdb")));
    EXPECT_THROW((void)vdb::InspectFile(file("nonfinite.vdb"), {.loadStatistics = true}), std::invalid_argument);
  }
  auto overflowing = openvdb::DoubleGrid::create(0.0);
  overflowing->tree().addTile(1, openvdb::Coord(0), std::numeric_limits<double>::max() / 2, true);
  write(file("overflow.vdb"), {overflowing});
  EXPECT_THROW((void)vdb::InspectFile(file("overflow.vdb"), {.loadStatistics = true}), std::invalid_argument);
}

TEST_F(OpenVdbInspection, DescriptorMetadataEncodedInputAndTopologyBudgetsAreEnforced) {
  const auto grid = scalar("a");
  grid->tree().addTile(1, openvdb::Coord(0), 2.0f, true);
  write(file("budget.vdb"), {grid, scalar("b")});
  EXPECT_THROW((void)vdb::InspectFile(file("budget.vdb"), {.gridName = "a", .maxGrids = 1}), std::invalid_argument);
  EXPECT_THROW((void)vdb::InspectFile(file("budget.vdb"), {.maxMetadataBytes = 16}), std::invalid_argument);
  EXPECT_THROW((void)vdb::InspectFile(file("budget.vdb"),
      {.maxInputBytes = std::filesystem::file_size(file("budget.vdb")) - 1}), std::invalid_argument);
  EXPECT_NO_THROW((void)vdb::InspectFile(file("budget.vdb"), {.maxActiveVoxels = 1}));
  EXPECT_THROW((void)vdb::InspectFile(file("budget.vdb"), {.loadStatistics = true, .maxActiveVoxels = 1}),
               std::invalid_argument);
  // Active count alone cannot hide storage in inactive leaves.
  auto inactive = openvdb::FloatGrid::create(0.0f);
  inactive->tree().setValueOff(openvdb::Coord(0), 1.0f);
  inactive->tree().setValueOff(openvdb::Coord(32,0,0), 1.0f);
  write(file("inactive.vdb"), {inactive});
  EXPECT_THROW((void)vdb::InspectFile(file("inactive.vdb"), {.loadStatistics = true, .maxActiveVoxels = 512}),
               std::invalid_argument);
  auto longMetadata = scalar();
  longMetadata->insertMeta("large", openvdb::StringMetadata(std::string(4096, 'x')));
  write(file("long-metadata.vdb"), {longMetadata});
  EXPECT_THROW((void)vdb::InspectFile(file("long-metadata.vdb"), {.maxMetadataBytes = 1024}), std::invalid_argument);
  openvdb::MetaMap fileMetadata;
  fileMetadata.insertMeta("large", openvdb::StringMetadata(std::string(4096, 'x')));
  write(file("file-metadata.vdb"), {scalar()}, true, fileMetadata);
  EXPECT_THROW((void)vdb::InspectFile(file("file-metadata.vdb"), {.maxMetadataBytes = 1024}), std::invalid_argument);
}

TEST_F(OpenVdbInspection, InvalidOptionsAndStreamingLegacyOrMalformedFilesFailClearly) {
  write(file("valid.vdb"), {scalar()});
  for (const auto options : {vdb::InspectionOptions{.maxGrids = 0}, {.maxGrids = 4097},
      {.maxMetadataBytes = 0}, {.maxInputBytes = 0}, {.maxActiveVoxels = 0}})
    EXPECT_THROW((void)vdb::InspectFile(file("valid.vdb"), options), std::invalid_argument);
  EXPECT_THROW((void)vdb::InspectFile(file("missing.vdb")), std::invalid_argument);
  {
    std::ofstream output(file("streaming.vdb"), std::ios::binary);
    openvdb::io::Stream stream(output);
    stream.write(openvdb::GridPtrVec{scalar()});
  }
  try {
    (void)vdb::InspectFile(file("streaming.vdb"));
    FAIL() << "streaming archives must not eagerly load trees during inspection";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("offsets"), std::string::npos);
  }
  std::filesystem::copy_file(file("valid.vdb"), file("legacy.vdb"));
  {
    std::fstream output(file("legacy.vdb"), std::ios::binary | std::ios::in | std::ios::out);
    output.seekp(8);
    const uint32_t version = 215;
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
  }
  EXPECT_THROW((void)vdb::InspectFile(file("legacy.vdb")), std::invalid_argument);
  std::ofstream(file("malformed.vdb"), std::ios::binary) << "not VDB";
  EXPECT_THROW((void)vdb::InspectFile(file("malformed.vdb")), std::exception);
}

TEST_F(OpenVdbInspection, MetadataAndStatisticsRetainExactDistantScalesAndTinyShears) {
  for (bool shear : {false, true}) {
    auto grid = scalar();
    if (shear) {
      auto matrix = openvdb::math::Mat4d::identity();
      matrix[0][1] = std::ldexp(1., -60);
      matrix[3][0] = 1e8;
      openvdb::math::AffineMap map(matrix);
      // Both public matrix factories simplify tiny shear. Serialize the SDK's
      // native map record to construct an independent exact fixture.
      std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
      openvdb::writeString(record, map.type());
      map.write(record);
      record.seekg(0);
      openvdb::io::setCurrentVersion(record);
      grid->transform().read(record);
    } else {
      grid->setTransform(openvdb::math::Transform::createLinearTransform(.1));
      grid->transform().postTranslate(openvdb::Vec3d(1e8, -1e8, 1e8));
    }
    const auto expected = grid->transform().baseMap()->getAffineMap()->getMat4();
    write(file("exact.vdb"), {grid});
    for (bool statistics : {false, true}) {
      const auto report = vdb::InspectFile(file("exact.vdb"), {.loadStatistics = statistics});
      ASSERT_EQ(report.grids.size(), 1u);
      const auto& information = report.grids.front();
      ASSERT_TRUE(information.indexToWorld);
      for (size_t r = 0; r < 4; ++r)
        for (size_t c = 0; c < 4; ++c)
          EXPECT_EQ((*information.indexToWorld)[4*r+c], expected(c,r));
      for (size_t axis = 0; axis < 3; ++axis)
        EXPECT_EQ(information.voxelSize[axis],
            std::hypot(expected(axis,0), expected(axis,1), expected(axis,2)));
      EXPECT_EQ(information.statistics.has_value(), statistics);
    }
  }
}

TEST_F(OpenVdbInspection, PersistedWellConditionedSmallScaleDoesNotRequireAffineMapConstruction) {
  constexpr double h=1e-6;
  auto grid=scalar();
  std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
  openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());
  for(const double value:{h,h,1/h,(1/h)*(1/h),.5/h})openvdb::Vec3d(value).write(record);
  record.seekg(0);openvdb::io::setCurrentVersion(record);grid->transform().read(record);
  write(file("small-scale.vdb"),{grid});
  for(bool loaded:{false,true}) {
    const auto report=vdb::InspectFile(file("small-scale.vdb"),{.loadStatistics=loaded});
    const auto& result=report.grids[0];ASSERT_TRUE(result.indexToWorld)<<result.transformDiagnostic;
    EXPECT_TRUE(result.transformDiagnostic.empty());
    for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c)
      EXPECT_EQ((*result.indexToWorld)[4*r+c],r==c?(r==3?1.:h):0.);
    EXPECT_EQ(result.voxelSize,(std::array{h,h,h}));ASSERT_TRUE(result.worldBounds);
    EXPECT_DOUBLE_EQ(result.worldBounds->minimum[0],-2.5*h);
    EXPECT_DOUBLE_EQ(result.worldBounds->maximum[0],-1.5*h);
    EXPECT_EQ(result.statistics.has_value(),loaded);
  }
}

TEST_F(OpenVdbInspection, CatalogOrderAndSharedTreeInstanceMetadataMatchSdkDescriptors) {
  const auto shared = scalar();
  openvdb::GridPtrVec grids{scalar("z"), scalar("a")};
  for (int index = 0; index < 12; ++index) {
    auto instance = openvdb::FloatGrid::create();
    instance->setName("duplicate");
    instance->setTree(shared->treePtr());
    instance->insertMeta("instance", openvdb::Int32Metadata(index));
    instance->transform().postTranslate(openvdb::Vec3d(index,0,0));
    // The SDK instance writer does not synthesize these allocation hints.
    instance->addStatsMetadata();
    grids.push_back(instance);
  }
  write(file("instances.vdb"), grids);
  openvdb::io::File source(file("instances.vdb").string());
  source.open(false);
  std::vector<std::string> expected;
  for (auto name = source.beginName(); name != source.endName(); ++name)
    expected.push_back(name.gridName());
  const auto report = vdb::InspectFile(file("instances.vdb"), {.loadStatistics = true});
  ASSERT_EQ(report.grids.size(), expected.size());
  size_t instance = 0;
  for (size_t index = 0; index < expected.size(); ++index) {
    const auto& information = report.grids[index];
    EXPECT_EQ(information.descriptorName, expected[index]);
    ASSERT_TRUE(information.statistics);
    EXPECT_EQ(information.statistics->sum, 2.);
    if (information.name != "duplicate") continue;
    ASSERT_TRUE(information.indexToWorld);
    EXPECT_EQ((*information.indexToWorld)[3], double(instance));
    const auto metadata = std::find_if(information.metadata.begin(), information.metadata.end(),
        [](const auto& field) { return field.name == "instance"; });
    ASSERT_NE(metadata, information.metadata.end());
    EXPECT_EQ(metadata->value, std::to_string(instance));
    ++instance;
  }
  EXPECT_EQ(instance, 12u);
}

class InspectionLayoutProbe : public openvdb::io::Archive {
public:
  struct Layout {
    int64_t countPosition, offsetsPosition, metadataEnd, gridPosition, blocksPosition, endPosition;
  };
  Layout layout(const std::filesystem::path& path) {
    auto metadata = std::make_shared<openvdb::io::StreamMetadata>();
    metadata->setSeekable(true);
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::badbit | std::ios::failbit);
    readHeader(input);
    openvdb::io::setStreamMetadataPtr(input, metadata, false);
    setFormatVersion(input); setLibraryVersion(input); setDataCompression(input);
    openvdb::MetaMap fileMetadata; fileMetadata.readMeta(input);
    const auto countPosition = static_cast<int64_t>(input.tellg());
    if (readGridCount(input) != 1) throw std::runtime_error("one-grid test fixture required");
    openvdb::io::GridDescriptor descriptor;
    const auto grid = descriptor.read(input);
    const auto offsetsPosition = static_cast<int64_t>(input.tellg()) - 3*sizeof(int64_t);
    descriptor.seekToGrid(input); readGridCompression(input);
    grid->readMeta(input); grid->readTransform(input);
    return {countPosition, static_cast<int64_t>(offsetsPosition),
        static_cast<int64_t>(input.tellg()), descriptor.getGridPos(),
        descriptor.getBlockPos(), descriptor.getEndPos()};
  }
};

TEST_F(OpenVdbInspection, InvalidEncodedCountsAndOffsetsRejectBeforeTraversal) {
  write(file("source.vdb"), {scalar()});
  InspectionLayoutProbe probe;
  const auto layout = probe.layout(file("source.vdb"));
  for (const auto [field, value] : std::vector<std::pair<int64_t,int64_t>>{
      {0, -1}, {0, layout.gridPosition-1}, {1, layout.gridPosition-1},
      {2, layout.gridPosition}, {2, layout.endPosition+1}, {2, -1}}) {
    std::filesystem::copy_file(file("source.vdb"), file("bad-offset.vdb"),
        std::filesystem::copy_options::overwrite_existing);
    {
      std::fstream output(file("bad-offset.vdb"), std::ios::binary | std::ios::in | std::ios::out);
      output.seekp(layout.offsetsPosition + field*sizeof(int64_t));
      output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    EXPECT_THROW((void)vdb::InspectFile(file("bad-offset.vdb")), std::invalid_argument);
  }
  for (int32_t count : {-1, 4097}) {
    std::filesystem::copy_file(file("source.vdb"), file("bad-count.vdb"),
        std::filesystem::copy_options::overwrite_existing);
    {
      std::fstream output(file("bad-count.vdb"), std::ios::binary | std::ios::in | std::ios::out);
      output.seekp(layout.countPosition);
      output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    }
    EXPECT_THROW((void)vdb::InspectFile(file("bad-count.vdb")), std::invalid_argument);
  }
}

TEST_F(OpenVdbInspection, MetadataOnlyDoesNotRequireTreePayloadAndReadsNativeUnicodeParents) {
  const auto parent = directory / std::filesystem::path(std::u8string(u8"inspect-\u6e2c\u8a66-\u00e9"));
  std::filesystem::create_directory(parent);
  const auto path = parent / "metadata-only.vdb";
  write(file("source.vdb"), {scalar()});
  std::filesystem::copy_file(file("source.vdb"), path);
  InspectionLayoutProbe probe;
  const auto layout = probe.layout(file("source.vdb"));
  {
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    output.seekp(layout.offsetsPosition + sizeof(int64_t));
    output.write(reinterpret_cast<const char*>(&layout.metadataEnd), sizeof(int64_t));
    output.write(reinterpret_cast<const char*>(&layout.metadataEnd), sizeof(int64_t));
  }
  std::filesystem::resize_file(path, static_cast<uintmax_t>(layout.metadataEnd));
  const auto report = vdb::InspectFile(path);
  ASSERT_EQ(report.grids.size(), 1u);
  EXPECT_EQ(report.grids[0].name, "density");
  EXPECT_EQ(report.grids[0].activeVoxelCount, 1u);
  EXPECT_FALSE(report.grids[0].statistics);
  write(file("empty.vdb"), {});
  EXPECT_TRUE(vdb::InspectFile(file("empty.vdb")).grids.empty());
}

TEST_F(OpenVdbInspection, MomentsWeightActiveTilesAndExcludeInactiveAndBackgroundValues) {
  auto grid=openvdb::DoubleGrid::create(100.);
  grid->tree().addTile(1,openvdb::Coord(0),2.,true);
  grid->tree().setValue({10,0,0},3.);
  grid->tree().setValue({11,0,0},0.);
  grid->tree().setValueOff({12,0,0},-100.);
  write(file("moments.vdb"),{grid});
  const auto report=vdb::InspectFile(file("moments.vdb"),{.loadStatistics=true});
  const auto& s=*report.grids[0].statistics;
  const double mean=1027./514.;
  const double squareMean=2057./514.;
  EXPECT_EQ(s.count,514u);
  ASSERT_TRUE(s.mean); ASSERT_TRUE(s.populationStdDev); ASSERT_TRUE(s.rootMeanSquare);
  EXPECT_EQ(*s.mean,mean);
  EXPECT_NEAR(*s.populationStdDev,std::sqrt(squareMean-mean*mean),1e-14);
  EXPECT_EQ(*s.rootMeanSquare,std::sqrt(squareMean));
  EXPECT_FALSE(s.histogram);
}

TEST_F(OpenVdbInspection, ExactMomentsAvoidCancelledOverflowAndSquaredOverflow) {
  const auto maximum=std::numeric_limits<double>::max();
  auto grid=openvdb::DoubleGrid::create();
  for (int n=0;n<4;++n) grid->tree().setValue({n,0,0},n<2?maximum:-maximum);
  write(file("extreme-moments.vdb"),{grid});
  const auto report=vdb::InspectFile(file("extreme-moments.vdb"),{.loadStatistics=true});
  const auto& s=*report.grids[0].statistics;
  EXPECT_EQ(s.sum,0.);
  EXPECT_EQ(s.mean,0.);
  EXPECT_EQ(s.populationStdDev,maximum);
  EXPECT_EQ(s.rootMeanSquare,maximum);
}

TEST_F(OpenVdbInspection, AdjacentLargeValuesHaveNonzeroDeviationAndCorrectRoundedMean) {
  const double high=1e308,low=std::nextafter(high,0.);
  auto grid=openvdb::DoubleGrid::create();
  // Halve the adjacent pair so its required sum remains representable.
  grid->tree().setValue({0,0,0},low/2);
  grid->tree().setValue({1,0,0},high/2);
  write(file("adjacent.vdb"),{grid});
  const auto report=vdb::InspectFile(file("adjacent.vdb"),{.loadStatistics=true});
  const auto& s=*report.grids[0].statistics;
  const double lower=low/2,upper=high/2;
  const double mean=(std::bit_cast<uint64_t>(lower)&1)?upper:lower;
  EXPECT_EQ(s.mean,mean);
  EXPECT_EQ(s.populationStdDev,(upper-lower)/2);
  EXPECT_EQ(s.rootMeanSquare,upper);
}

TEST_F(OpenVdbInspection, SubnormalMomentsStayNonzeroAndUnrepresentableNonzeroMeanRejects) {
  const double tiny=std::numeric_limits<double>::denorm_min();
  auto grid=openvdb::DoubleGrid::create();
  grid->tree().setValue({0,0,0},tiny);
  grid->tree().setValue({1,0,0},-tiny);
  write(file("tiny-moments.vdb"),{grid});
  const auto report=vdb::InspectFile(file("tiny-moments.vdb"),{.loadStatistics=true});
  const auto& s=*report.grids[0].statistics;
  EXPECT_EQ(s.mean,0.);
  EXPECT_EQ(s.populationStdDev,tiny);
  EXPECT_EQ(s.rootMeanSquare,tiny);
  grid->tree().setValue({1,0,0},0.);
  write(file("underflow-mean.vdb"),{grid});
  EXPECT_THROW((void)vdb::InspectFile(file("underflow-mean.vdb"),{.loadStatistics=true}),std::invalid_argument);
}

TEST_F(OpenVdbInspection, HistogramsUseWeightedHalfOpenBinsAndInclusiveMaximum) {
  auto grid=openvdb::DoubleGrid::create();
  grid->tree().addTile(1,openvdb::Coord(0),0.,true);
  const std::array values{-2.,-1.,1.,2.,std::nextafter(1.,0.)};
  for (size_t n=0;n<values.size();++n) grid->tree().setValue({int32_t(10+n),0,0},values[n]);
  grid->tree().setValueOff({20,0,0},99.);
  write(file("histogram.vdb"),{grid});
  vdb::InspectionOptions options; options.loadStatistics=true; options.histogramBins=4;
  auto report=vdb::InspectFile(file("histogram.vdb"),options);
  auto h=*report.grids[0].statistics->histogram;
  EXPECT_EQ(h.minimum,-2.); EXPECT_EQ(h.maximum,2.);
  EXPECT_EQ(h.bins,(std::vector<uint64_t>{1,1,513,2}));
  EXPECT_EQ(h.belowRange,0u); EXPECT_EQ(h.aboveRange,0u);
  options.histogramRange=std::array{-1.,1.};
  report=vdb::InspectFile(file("histogram.vdb"),options);
  h=*report.grids[0].statistics->histogram;
  EXPECT_EQ(h.bins,(std::vector<uint64_t>{1,0,512,2}));
  EXPECT_EQ(h.belowRange,1u); EXPECT_EQ(h.aboveRange,1u);
}

TEST_F(OpenVdbInspection, HistogramBoundariesRemainExactAcrossExtremeAndAdjacentDoubles) {
  const auto maximum=std::numeric_limits<double>::max(),tiny=std::numeric_limits<double>::denorm_min();
  auto grid=openvdb::DoubleGrid::create();
  const std::array values{-maximum,-tiny,-0.,0.,tiny,maximum};
  for (size_t n=0;n<values.size();++n) grid->tree().setValue({int32_t(n),0,0},values[n]);
  write(file("extreme-hist.vdb"),{grid});
  vdb::InspectionOptions options; options.loadStatistics=true; options.histogramBins=2;
  options.histogramRange=std::array{-maximum,maximum};
  auto report=vdb::InspectFile(file("extreme-hist.vdb"),options);
  EXPECT_EQ(report.grids[0].statistics->histogram->bins,(std::vector<uint64_t>{2,4}));
  const double high=1e308,low=std::nextafter(high,0.);
  grid=openvdb::DoubleGrid::create();
  grid->tree().setValue({0,0,0},low/2);grid->tree().setValue({1,0,0},high/2);
  write(file("adjacent-hist.vdb"),{grid});
  options.histogramRange=std::array{low/2,high/2};options.histogramBins=4096;
  report=vdb::InspectFile(file("adjacent-hist.vdb"),options);
  const auto& bins=report.grids[0].statistics->histogram->bins;
  EXPECT_EQ(bins.front(),1u); EXPECT_EQ(bins.back(),1u);
  EXPECT_EQ(std::count(bins.begin(),bins.end(),0),4094);
}

TEST_F(OpenVdbInspection, ConstantEmptyHistogramsAndInvalidOrExcessiveReportOptionsAreExplicit) {
  auto constant=scalar(); constant->tree().setValue({-2,3,4},-7.);
  auto empty=openvdb::DoubleGrid::create();empty->setName("empty");
  write(file("constant-empty.vdb"),{constant,empty});
  vdb::InspectionOptions options;options.loadStatistics=true;options.histogramBins=3;
  const auto report=vdb::InspectFile(file("constant-empty.vdb"),options);
  for (const auto& g:report.grids) {
    const auto& s=*g.statistics; const auto& h=*s.histogram;
    if (!s.count) {
      EXPECT_FALSE(s.mean);EXPECT_FALSE(s.populationStdDev);EXPECT_FALSE(s.rootMeanSquare);
      EXPECT_FALSE(h.minimum);EXPECT_FALSE(h.maximum);
      EXPECT_EQ(h.bins,(std::vector<uint64_t>{0,0,0}));
    } else {
      EXPECT_EQ(s.mean,-7.);EXPECT_EQ(s.populationStdDev,0.);EXPECT_EQ(s.rootMeanSquare,7.);
      EXPECT_EQ(h.minimum,-7.);EXPECT_EQ(h.maximum,-7.);
      EXPECT_EQ(h.bins,(std::vector<uint64_t>{1,0,0}));
    }
  }
  for (int invalid=0;invalid<6;++invalid) {
    options={};options.loadStatistics=true;options.histogramBins=2;
    if(invalid==0) options.histogramBins=4097;
    if(invalid==1) options.loadStatistics=false;
    if(invalid==2) options.histogramRange=std::array{1.,1.};
    if(invalid==3) options.histogramRange=std::array{std::numeric_limits<double>::quiet_NaN(),1.};
    if(invalid==4) {options.histogramBins=0;options.histogramRange=std::array{0.,1.};}
    if(invalid==5) {options.histogramBins=4096;options.maxMetadataBytes=32768;}
    EXPECT_THROW((void)vdb::InspectFile(file("constant-empty.vdb"),options),std::invalid_argument);
  }
}
#endif
} // namespace
