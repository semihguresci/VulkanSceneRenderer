#include "Container/geometry/OpenVdbRayQueries.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/io.h>
#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
using Kind = vdb::SdfRayHitKind;
using Normal = vdb::RayNormalStatus;

TEST(OpenVdbRayQueriesAvailability, LevelSetDisabledBeforeOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::LevelSetRayOptions options;
  options.limits.maxRays = 0;
  options.refinementSteps = 99;
  vdb::ProcessingOptions processing;
  processing.maxInputBytes = 0;
  const std::vector<vdb::WorldRay> rays{{{}, {}, -1, 0}};
  try {
    (void)vdb::QueryLevelSetRays("missing.vdb", rays, options, {}, processing);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

TEST(OpenVdbRayQueriesAvailability, VolumeDisabledBeforeOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::VolumeRayOptions options;
  options.limits.maxWorkItems = 0;
  options.maxSpans = 0;
  vdb::ProcessingOptions processing;
  processing.maxVoxelCount = 0;
  const std::vector<vdb::WorldRay> rays{{{}, {}, -1, 0}};
  try {
    (void)vdb::QueryActiveVolumeRays("missing.vdb", rays, options, {},
                                     processing);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
// Archive's ordinary copyGrid may simplify maps. This independent fixture view
// preserves the public native transform record exactly, including tiny scales.
template <class G> class ExactRayFixture : public G {
public:
  explicit ExactRayFixture(const G &grid)
      : G(openvdb::ConstPtrCast<typename G::TreeType>(grid.constTreePtr())) {
    this->insertMeta(grid);
    this->setTransform(
        std::make_shared<openvdb::math::Transform>(grid.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, std::make_shared<openvdb::math::Transform>(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactRayFixture &>(*this).copyGrid());
  }
};

class OpenVdbRayQueries : public ::testing::Test {
protected:
  using V = openvdb::Vec3d;
  using C = openvdb::Coord;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-ray-queries-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  static std::string bytes(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &metadata = {}, bool statistics = true) {
    openvdb::GridPtrVec exact;
    for (const auto &grid : grids) {
      if (grid->isType<openvdb::FloatGrid>())
        exact.push_back(std::make_shared<ExactRayFixture<openvdb::FloatGrid>>(
            *openvdb::gridPtrCast<openvdb::FloatGrid>(grid)));
      else if (grid->isType<openvdb::DoubleGrid>())
        exact.push_back(std::make_shared<ExactRayFixture<openvdb::DoubleGrid>>(
            *openvdb::gridPtrCast<openvdb::DoubleGrid>(grid)));
      else
        exact.push_back(grid);
    }
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.setGridStatsMetadataEnabled(statistics);
    file.write(exact, metadata);
  }
  static void uniformMap(openvdb::GridBase &grid, double h) {
    if (h >= 1e-4) {
      grid.setTransform(openvdb::math::Transform::createLinearTransform(h));
      return;
    }
    std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
    openvdb::writeString(record, openvdb::math::UniformScaleMap::mapType());
    for (const double value : {h, h, 1 / h, 1 / (h * h), .5 / h})
      V(value).write(record);
    record.seekg(0);
    openvdb::io::setCurrentVersion(record);
    grid.transform().read(record);
  }
  template <class G = openvdb::DoubleGrid>
  static typename G::Ptr plane(double h = 1, C offset = {}) {
    using T = typename G::ValueType;
    auto grid = G::create(T(4 * h));
    grid->setName("plane");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    grid->setVectorType(openvdb::VEC_INVARIANT);
    grid->setIsInWorldSpace(true);
    uniformMap(*grid, h);
    for (int x = -3; x <= 3; ++x)
      for (int y = -3; y <= 3; ++y)
        for (int z = -3; z <= 3; ++z)
          grid->tree().setValue(offset + C(x, y, z), T((x - .25) * h));
    for (int x = -6; x <= -4; ++x)
      for (int y = -3; y <= 3; ++y)
        for (int z = -3; z <= 3; ++z)
          grid->tree().setValueOff(offset + C(x, y, z), T(-4 * h));
    return grid;
  }
  template <class G = openvdb::DoubleGrid>
  static typename G::Ptr sphere(double h = .25) {
    using T = typename G::ValueType;
    auto grid = G::create(T(4 * h));
    grid->setName("sphere");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    uniformMap(*grid, h);
    for (int x = -9; x <= 9; ++x)
      for (int y = -9; y <= 9; ++y)
        for (int z = -9; z <= 9; ++z) {
          const double d = std::hypot(double(x), double(y), double(z)) - 4.5;
          if (std::abs(d) < 4)
            grid->tree().setValue(C(x, y, z), T(d * h));
          else if (d < 0)
            grid->tree().setValueOff(C(x, y, z), T(-4 * h));
        }
    return grid;
  }
  static vdb::WorldRay ray(V origin = V(2.25, .25, .25),
                           V direction = V(-7, 0, 0), double maximum = 3,
                           double minimum = 0) {
    return {{origin.x(), origin.y(), origin.z()},
            {direction.x(), direction.y(), direction.z()},
            minimum,
            maximum};
  }
  static vdb::WorldRay volumeRay(double maximum = 40, double minimum = 0) {
    return ray(V(-2, .25, .25), V(3, 0, 0), maximum, minimum);
  }
  vdb::LevelSetRayBatch sdf(const std::vector<vdb::WorldRay> &rays,
                            const std::string &name = "input.vdb",
                            const std::string &selector = {}) {
    return vdb::QueryLevelSetRays(path(name), rays, level, selector,
                                  processing);
  }
  vdb::VolumeRayBatch volume(const std::vector<vdb::WorldRay> &rays,
                             const std::string &name = "input.vdb",
                             const std::string &selector = {}) {
    return vdb::QueryActiveVolumeRays(path(name), rays, occupancy, selector,
                                      processing);
  }
  static void hit(const vdb::SdfRayHit &value, double distance, V position,
                  double tolerance = 1e-10) {
    ASSERT_NE(value.kind, Kind::Miss);
    EXPECT_NEAR(value.distance, distance, tolerance);
    for (int axis = 0; axis < 3; ++axis)
      EXPECT_NEAR(value.position[axis], position[axis], tolerance);
    EXPECT_NEAR(value.sampledResidual, 0, tolerance);
  }
  static void span(const vdb::VolumeRaySpan &value, double entry, double exit,
                   double tolerance = 1e-10) {
    EXPECT_NEAR(value.entryDistance, entry, tolerance);
    EXPECT_NEAR(value.exitDistance, exit, tolerance);
    EXPECT_LT(value.entryDistance, value.exitDistance);
  }
  std::filesystem::path directory;
  vdb::LevelSetRayOptions level;
  vdb::VolumeRayOptions occupancy;
  vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbRayQueries,
       FloatAndDoublePlaneReturnWorldDistanceAndOutwardNormal) {
  for (const bool nativeFloat : {false, true}) {
    SCOPED_TRACE(nativeFloat);
    auto grid = nativeFloat
                    ? openvdb::GridBase::Ptr(plane<openvdb::FloatGrid>())
                    : openvdb::GridBase::Ptr(plane());
    write("input.vdb", {grid});
    const auto before = bytes(path("input.vdb"));
    const auto batch = sdf({ray()});
    ASSERT_EQ(batch.rays.size(), 1u);
    hit(batch.rays[0], 2, V(.25, .25, .25));
    EXPECT_EQ(batch.rays[0].normalizedDirection,
              (std::array<double, 3>{-1, 0, 0}));
    EXPECT_TRUE(batch.rays[0].startSampleValid);
    EXPECT_FALSE(batch.rays[0].startedInside);
    ASSERT_EQ(batch.rays[0].normalStatus, Normal::Available);
    EXPECT_NEAR(batch.rays[0].normal[0], 1, 1e-12);
    EXPECT_NEAR(batch.rays[0].normal[1], 0, 1e-12);
    EXPECT_NEAR(batch.rays[0].normal[2], 0, 1e-12);
    EXPECT_EQ(batch.source.gridName, "plane");
    EXPECT_GT(batch.source.workItems, 0u);
    EXPECT_EQ(bytes(path("input.vdb")), before);
  }
}

TEST_F(OpenVdbRayQueries, DistanceWindowInsideStartAndNormalOptOutAreExplicit) {
  write("input.vdb", {plane()});
  const auto result = sdf({ray(V(2.25, .25, .25), V(-1, 0, 0), 1.5),
                           ray(V(2.25, .25, .25), V(-1, 0, 0), 3, 2.1),
                           ray(V(-1.25, .25, .25), V(1, 0, 0), 2),
                           ray(V(2.25, .25, .25), V(-1, 0, 0), 3, .5)});
  ASSERT_EQ(result.rays.size(), 4u);
  EXPECT_EQ(result.rays[0].kind, Kind::Miss);
  EXPECT_EQ(result.rays[0].normalStatus, Normal::NoHit);
  EXPECT_EQ(result.rays[1].kind, Kind::Miss);
  hit(result.rays[2], 1.5, V(.25, .25, .25));
  EXPECT_TRUE(result.rays[2].startSampleValid);
  EXPECT_TRUE(result.rays[2].startedInside);
  hit(result.rays[3], 2, V(.25, .25, .25));
  level.computeNormals = false;
  const auto none = sdf({ray()});
  ASSERT_EQ(none.rays.size(), 1u);
  hit(none.rays[0], 2, V(.25, .25, .25));
  EXPECT_EQ(none.rays[0].normalStatus, Normal::NotRequested);
}

TEST_F(OpenVdbRayQueries,
       SignedZeroAndZeroPlateauAreSampledZeroNotIsolatedRoots) {
  for (const bool plateau : {false, true}) {
    auto grid = plane();
    for (auto value = grid->beginValueOn(); value; ++value) {
      const int x = value.getCoord().x();
      value.setValue(x == 0 ? -0.0 : plateau && x == 1 ? 0.0 : double(x));
    }
    write("input.vdb", {grid});
    const auto result =
        sdf({ray(V(plateau ? .25 : 0, .25, .25), V(1, 0, 0), 1)});
    ASSERT_EQ(result.rays.size(), 1u);
    EXPECT_EQ(result.rays[0].kind, Kind::SampledZero);
    EXPECT_DOUBLE_EQ(result.rays[0].distance, 0);
    EXPECT_DOUBLE_EQ(result.rays[0].sampledResidual, 0);
    if (plateau)
      EXPECT_EQ(result.rays[0].normalStatus, Normal::DegenerateGradient);
  }
}

TEST_F(OpenVdbRayQueries, TinySameSignSamplesNeverInventAZeroCrossing) {
  auto grid = plane();
  for (auto value = grid->beginValueOn(); value; ++value) {
    const int x = value.getCoord().x();
    value.setValue(x < 0 ? -1e-300 : (x + 1) * 1e-300);
  }
  write("input.vdb", {grid});
  const auto result = sdf({ray(V(0, .25, .25), V(1, 0, 0), 1)});
  ASSERT_EQ(result.rays.size(), 1u);
  EXPECT_EQ(result.rays[0].kind, Kind::Miss);
  EXPECT_EQ(result.rays[0].normalStatus, Normal::NoHit);
}

TEST_F(OpenVdbRayQueries,
       TinyOppositeSignsUseScaledChordWithoutProductUnderflow) {
  auto grid = plane();
  for (auto value = grid->beginValueOn(); value; ++value)
    value.setValue((value.getCoord().x() <= 0 ? -1 : 3) * 1e-300);
  write("input.vdb", {grid});
  level.computeNormals = false;
  const auto result = sdf({ray(V(0, .25, .25), V(1, 0, 0), 1)});
  ASSERT_EQ(result.rays.size(), 1u);
  hit(result.rays[0], .25, V(.25, .25, .25), 1e-12);
  EXPECT_EQ(result.rays[0].kind, Kind::BracketedCrossing);
}

TEST_F(OpenVdbRayQueries, MissingOriginalNormalCornerIsNotAnAvailableNormal) {
  auto grid = plane();
  // This zero-weight corner leaves the ray's axis root unchanged. It still
  // invalidates the explicitly required eight-active-corner normal footprint.
  grid->tree().setValueOff(C(1, 1, 1), grid->background());
  write("input.vdb", {grid});
  const auto result = sdf({ray(V(2.25, 0, 0), V(-1, 0, 0), 3)});
  ASSERT_EQ(result.rays.size(), 1u);
  hit(result.rays[0], 2, V(.25, 0, 0));
  EXPECT_EQ(result.rays[0].normalStatus, Normal::MissingActiveFootprint);
}

TEST_F(OpenVdbRayQueries, SphereEntryExitAndDeepInactiveStartHaveHonestStatus) {
  for (const bool nativeFloat : {false, true}) {
    auto grid = nativeFloat
                    ? openvdb::GridBase::Ptr(sphere<openvdb::FloatGrid>())
                    : openvdb::GridBase::Ptr(sphere());
    write("input.vdb", {grid});
    const auto result =
        sdf({ray(V(-2, 0, 0), V(1, 0, 0), 4), ray(V(0, 0, 0), V(1, 0, 0), 2),
             ray(V(-2, 2, 0), V(1, 0, 0), 4)});
    ASSERT_EQ(result.rays.size(), 3u);
    hit(result.rays[0], .875, V(-1.125, 0, 0), 1e-8);
    hit(result.rays[1], 1.125, V(1.125, 0, 0), 1e-8);
    EXPECT_FALSE(result.rays[1].startSampleValid);
    EXPECT_EQ(result.rays[0].normalStatus, Normal::Available);
    EXPECT_LT(result.rays[0].normal[0], -.97);
    EXPECT_EQ(result.rays[2].kind, Kind::Miss);
  }
}

TEST_F(OpenVdbRayQueries,
       ReflectedRotatedNonunitPlaneRetainsWorldDistanceAndMap) {
  constexpr double angle = .37;
  auto grid = plane(2);
  grid->transform().postScale(V(-1, 1, 1));
  grid->transform().postRotate(angle, openvdb::math::Z_AXIS);
  grid->transform().postTranslate(V(100.125, -50, 7));
  write("input.vdb", {grid});
  const V origin = grid->transform().indexToWorld(V(2.25, .25, .25));
  const V target = grid->transform().indexToWorld(V(.25, .25, .25));
  const auto result = sdf({ray(origin, target - origin, 6)});
  ASSERT_EQ(result.rays.size(), 1u);
  hit(result.rays[0], 4, target, 2e-11);
  ASSERT_EQ(result.rays[0].normalStatus, Normal::Available);
  EXPECT_NEAR(result.rays[0].normal[0], -std::cos(angle), 1e-11);
  EXPECT_NEAR(result.rays[0].normal[1], -std::sin(angle), 1e-11);
  const auto &map = result.source.indexToWorld;
  for (int axis = 0; axis < 3; ++axis) {
    const double mapped = map[4 * axis] * .25 + map[4 * axis + 1] * .25 +
                          map[4 * axis + 2] * .25 + map[4 * axis + 3];
    EXPECT_NEAR(mapped, target[axis], 1e-12);
  }
}

TEST_F(OpenVdbRayQueries, TinyHugeSpacingAndRemoteIndicesRetainPhysicalRoots) {
  for (const double h : {1e-6, 1e6}) {
    SCOPED_TRACE(h);
    write("input.vdb", {plane(h)});
    const auto result =
        sdf({ray(V(2.25 * h, .25 * h, .25 * h), V(-1, 0, 0), 3 * h)});
    ASSERT_EQ(result.rays.size(), 1u);
    hit(result.rays[0], 2 * h, V(.25 * h, .25 * h, .25 * h), 1e-10 * h);
  }
  auto remote = plane(1e6, C(-1000000000, 0, 0));
  remote->transform().postTranslate(V(1e15, 0, 0));
  write("input.vdb", {remote});
  const auto result = sdf({ray(V(2.25e6, .25e6, .25e6), V(-1, 0, 0), 3e6)});
  ASSERT_EQ(result.rays.size(), 1u);
  hit(result.rays[0], 2e6, V(.25e6, .25e6, .25e6), 1e-5);
}

TEST_F(OpenVdbRayQueries, SingleActiveCellMeansWholeLeafForAllNineNativeTypes) {
  const auto check = [&]<class G>(const typename G::ValueType &active) {
    auto grid = G::create();
    grid->setName("occupancy");
    grid->tree().setValueOn(C(0), active);
    write("input.vdb", {grid});
    const auto original = bytes(path("input.vdb"));
    const auto result = volume({volumeRay()});
    ASSERT_EQ(result.rays.size(), 1u);
    ASSERT_EQ(result.totalSpanCount, 1u);
    ASSERT_EQ(result.rays[0].spans.size(), 1u);
    span(result.rays[0].spans[0], 1.5, 9.5);
    EXPECT_NEAR(result.rays[0].spans[0].entryPosition[0], -.5, 1e-12);
    EXPECT_NEAR(result.rays[0].spans[0].exitPosition[0], 7.5, 1e-12);
    EXPECT_EQ(result.source.inputActiveVoxelCount, 1u);
    EXPECT_EQ(result.source.inputStoredLeafCount, 1u);
    EXPECT_EQ(bytes(path("input.vdb")), original);
  };
  check.template operator()<openvdb::FloatGrid>(
      std::numeric_limits<float>::quiet_NaN());
  check.template operator()<openvdb::DoubleGrid>(
      std::numeric_limits<double>::infinity());
  check.template operator()<openvdb::BoolGrid>(false);
  check.template operator()<openvdb::MaskGrid>(true);
  check.template operator()<openvdb::Int32Grid>(0);
  check.template operator()<openvdb::Int64Grid>(INT64_MAX);
  check.template operator()<openvdb::Vec3IGrid>(
      openvdb::Vec3i(INT32_MIN, 0, INT32_MAX));
  check.template operator()<openvdb::Vec3SGrid>(openvdb::Vec3f(0));
  check.template operator()<openvdb::Vec3DGrid>(V(0));
}

TEST_F(OpenVdbRayQueries,
       AdjacentCoarseLeavesMergeAndSeparatedLeavesRemainSeparate) {
  auto grid = openvdb::FloatGrid::create(0);
  grid->setName("blocks");
  for (const int x : {0, 8, 24})
    grid->tree().setValueOn(C(x, 0, 0), 0);
  // Inactive nonzero data and an allocated empty leaf cannot add a span.
  grid->tree().touchLeaf(C(40, 0, 0));
  grid->tree().setValueOff(C(40, 0, 0), -17);
  write("input.vdb", {grid});
  const auto result = volume({volumeRay(48), volumeRay(12, 4)});
  ASSERT_EQ(result.rays.size(), 2u);
  ASSERT_EQ(result.rays[0].spans.size(), 2u);
  span(result.rays[0].spans[0], 1.5, 17.5);
  span(result.rays[0].spans[1], 25.5, 33.5);
  ASSERT_EQ(result.rays[1].spans.size(), 1u);
  span(result.rays[1].spans[0], 4, 12);
  EXPECT_EQ(result.totalSpanCount, 3u);
  occupancy.maxSpans = 1;
  EXPECT_THROW((void)volume({volumeRay(48)}), std::exception);
}

TEST_F(OpenVdbRayQueries,
       ActiveTilesStayCompactAndInactiveOnlyTopologyHasNoSpan) {
  auto grid = openvdb::BoolGrid::create(false);
  grid->setName("tile");
  grid->tree().addTile(2, C(0), false, true);
  write("input.vdb", {grid});
  const auto result = volume({volumeRay(140)});
  ASSERT_EQ(result.rays.size(), 1u);
  ASSERT_EQ(result.rays[0].spans.size(), 1u);
  span(result.rays[0].spans[0], 1.5, 129.5);
  EXPECT_EQ(result.source.inputActiveTileCount, 1u);
  EXPECT_EQ(result.source.inputStoredLeafCount, 0u);
  EXPECT_EQ(result.source.inputActiveVoxelCount, uint64_t(128) * 128 * 128);
  auto inactive = openvdb::DoubleGrid::create(9);
  inactive->tree().touchLeaf(C(0));
  inactive->tree().setValueOff(C(0), -9);
  inactive->tree().addTile(1, C(32, 0, 0), -9, false);
  write("input.vdb", {inactive});
  const auto empty = volume({volumeRay(48)});
  ASSERT_EQ(empty.rays.size(), 1u);
  EXPECT_TRUE(empty.rays[0].spans.empty());
  EXPECT_EQ(empty.totalSpanCount, 0u);
  EXPECT_TRUE(empty.source.empty);
}

TEST_F(OpenVdbRayQueries,
       CoarseVolumeSpansUsePhysicalScaleRotationAndReflection) {
  auto grid = openvdb::Int64Grid::create(0);
  grid->tree().setValueOn(C(0), -1);
  grid->setTransform(openvdb::math::Transform::createLinearTransform(2));
  grid->transform().postScale(V(-1, 1, 1));
  grid->transform().postRotate(.31, openvdb::math::Z_AXIS);
  grid->transform().postTranslate(V(20, -7, 2));
  write("input.vdb", {grid});
  const V origin = grid->transform().indexToWorld(V(-2, .25, .25));
  const V finish = grid->transform().indexToWorld(V(10, .25, .25));
  const auto result = volume({ray(origin, finish - origin, 24)});
  ASSERT_EQ(result.rays.size(), 1u);
  ASSERT_EQ(result.rays[0].spans.size(), 1u);
  span(result.rays[0].spans[0], 3, 19, 1e-10);
  const V entry = grid->transform().indexToWorld(V(-.5, .25, .25));
  const V exit = grid->transform().indexToWorld(V(7.5, .25, .25));
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(result.rays[0].spans[0].entryPosition[axis], entry[axis],
                1e-10);
    EXPECT_NEAR(result.rays[0].spans[0].exitPosition[axis], exit[axis], 1e-10);
  }
}

TEST_F(OpenVdbRayQueries,
       MetadataDescriptorsUnicodeAndEmptyBatchesRemainReadOnly) {
  auto first = plane();
  first->insertMeta("artist", openvdb::StringMetadata("retained source"));
  first->insertMeta("exact_integer",
                    openvdb::Int64Metadata(INT64_C(9007199254740993)));
  auto second = plane();
  openvdb::MetaMap metadata;
  metadata.insertMeta("scene", openvdb::StringMetadata("file metadata"));
  write("input.vdb", {first, second}, metadata, false);
  auto invalidHint = plane();
  invalidHint->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                          openvdb::Int64Metadata(-1));
  write("negative-statistics.vdb", {invalidHint}, {}, false);
  EXPECT_THROW((void)sdf({}, "negative-statistics.vdb"), std::exception);
  EXPECT_THROW((void)volume({}, "negative-statistics.vdb"), std::exception);
  invalidHint->removeMeta(openvdb::GridBase::META_FILE_MEM_BYTES);
  invalidHint->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                          openvdb::StringMetadata("not a native byte count"));
  write("malformed-statistics.vdb", {invalidHint}, {}, false);
  EXPECT_THROW((void)sdf({}, "malformed-statistics.vdb"), std::exception);
  EXPECT_THROW((void)volume({}, "malformed-statistics.vdb"), std::exception);
  const auto before = bytes(path("input.vdb"));
  // Archive numbers every duplicate display name, including the first one.
  EXPECT_THROW((void)sdf({}, "input.vdb", "plane"), std::exception);
  const auto result = sdf({}, "input.vdb", "descriptor:plane[0]");
  EXPECT_TRUE(result.rays.empty());
  EXPECT_GT(result.source.workItems, 0u);
  EXPECT_EQ(result.source.gridName, "plane");
  EXPECT_EQ(result.source.descriptorName, "plane[0]");
  bool artist = false, integer = false, scene = false;
  for (const auto &m : result.source.metadata) {
    if (m.name == "artist") {
      artist = true;
      EXPECT_EQ(m.value, "retained source");
    }
    if (m.name == "exact_integer") {
      integer = true;
      EXPECT_EQ(m.value, "9007199254740993");
    }
  }
  for (const auto &m : result.source.fileMetadata)
    if (m.name == "scene") {
      scene = true;
      EXPECT_EQ(m.value, "file metadata");
    }
  EXPECT_TRUE(artist);
  EXPECT_TRUE(integer);
  EXPECT_TRUE(scene);
  const auto selected = sdf({ray()}, "input.vdb", "descriptor:plane[1]");
  EXPECT_EQ(selected.source.gridName, "plane");
  EXPECT_EQ(selected.source.descriptorName, "plane[1]");
  first->setName("descriptor:literal");
  write("literal.vdb", {first});
  const auto literal =
      sdf({ray()}, "literal.vdb", "descriptor:descriptor:literal");
  EXPECT_EQ(literal.source.gridName, "descriptor:literal");
  EXPECT_THROW((void)sdf({ray()}, "input.vdb", "descriptor:missing"),
               std::exception);
  const auto unicode = directory / std::filesystem::path(u"rays_球.vdb");
  std::filesystem::copy_file(path("input.vdb"), unicode);
  const std::vector<vdb::WorldRay> rays{ray()};
  const auto native = vdb::QueryLevelSetRays(unicode, rays, level,
                                             "descriptor:plane[0]", processing);
  ASSERT_EQ(native.rays.size(), 1u);
  hit(native.rays[0], 2, V(.25, .25, .25));
  EXPECT_EQ(bytes(path("input.vdb")), before);
  EXPECT_EQ(bytes(unicode), before);
}

TEST_F(OpenVdbRayQueries,
       TrueEmptySdfMissesButHiddenNegativeAndUnsignedSourcesReject) {
  auto empty = openvdb::DoubleGrid::create(4);
  empty->setGridClass(openvdb::GRID_LEVEL_SET);
  write("input.vdb", {empty});
  const auto result = sdf({ray()});
  ASSERT_EQ(result.rays.size(), 1u);
  EXPECT_TRUE(result.source.empty);
  EXPECT_EQ(result.rays[0].kind, Kind::Miss);
  EXPECT_EQ(result.rays[0].normalStatus, Normal::NoHit);
  empty->tree().setValueOff(C(0), -4);
  write("input.vdb", {empty});
  EXPECT_THROW((void)sdf({ray()}), std::exception);
  auto positive = openvdb::DoubleGrid::create(4);
  positive->setGridClass(openvdb::GRID_LEVEL_SET);
  positive->tree().setValueOn(C(0), 1);
  write("input.vdb", {positive});
  EXPECT_THROW((void)sdf({ray()}), std::exception);
}

TEST_F(OpenVdbRayQueries, LevelSetClassBandTilesAndFiniteValuesAreValidated) {
  auto grid = plane();
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  write("input.vdb", {grid});
  EXPECT_THROW((void)sdf({ray()}), std::exception);
  grid = plane();
  grid->tree().setValueOn(C(0), std::numeric_limits<double>::quiet_NaN());
  write("input.vdb", {grid});
  EXPECT_THROW((void)sdf({ray()}), std::exception);
  grid = plane();
  grid->tree().addTile(1, C(16, 0, 0), 1, true);
  write("input.vdb", {grid});
  EXPECT_THROW((void)sdf({ray()}), std::exception);
  write("input.vdb", {plane()});
  level.isovalue = 3;
  EXPECT_THROW((void)sdf({ray()}), std::exception);
  auto points = openvdb::points::PointDataGrid::create();
  write("points.vdb", {points});
  EXPECT_THROW((void)volume({volumeRay()}, "points.vdb"), std::exception);
}

TEST_F(OpenVdbRayQueries, InvalidRaysMapsPrecisionAndNodeExtentsReject) {
  write("input.vdb", {plane()});
  for (auto bad :
       {ray(V(0), V(0)), ray(V(0), V(1, 0, 0), 1, -1),
        ray(V(0), V(1, 0, 0), 1, 1),
        ray(V(0), V(std::numeric_limits<double>::infinity(), 0, 0))}) {
    EXPECT_THROW((void)sdf({bad}), std::exception);
    EXPECT_THROW((void)volume({bad}), std::exception);
  }
  EXPECT_THROW((void)volume({volumeRay(1e-12)}), std::exception);
  auto grid = plane();
  grid->transform().postScale(V(1, 2, 1));
  write("input.vdb", {grid});
  EXPECT_THROW((void)sdf({ray()}), std::exception);
  EXPECT_THROW((void)volume({volumeRay()}), std::exception);
  grid = plane();
  grid->transform().postTranslate(V(1e16, 0, 0));
  write("input.vdb", {grid});
  EXPECT_THROW((void)sdf({ray(V(1e16, .25, .25))}), std::exception);
  EXPECT_THROW((void)volume({ray(V(1e16, .25, .25))}), std::exception);
  grid = plane();
  grid->tree().touchLeaf(C(INT32_MAX, 0, 0));
  grid->tree().setValueOff(C(INT32_MAX, 0, 0), 4);
  write("input.vdb", {grid});
  EXPECT_THROW((void)sdf({ray()}), std::exception);
  EXPECT_THROW((void)volume({volumeRay()}), std::exception);
}

TEST_F(OpenVdbRayQueries,
       SharedCapsApplyToCensusEmptyBatchesAndCompleteResults) {
  write("input.vdb", {plane()});
  const auto before = bytes(path("input.vdb"));
  level.limits.maxRays = 1;
  EXPECT_THROW((void)sdf({ray(), ray()}), std::exception);
  level = {};
  level.refinementSteps = 9;
  EXPECT_THROW((void)sdf({ray()}), std::exception);
  level = {};
  level.limits.maxWorkItems = 1;
  EXPECT_THROW((void)sdf({}), std::exception);
  occupancy.limits.maxWorkItems = 1;
  EXPECT_THROW((void)volume({}), std::exception);
  level = {};
  occupancy = {};
  level.limits.maxMetadataBytes = 1;
  occupancy.limits.maxMetadataBytes = 1;
  EXPECT_THROW((void)sdf({}), std::exception);
  EXPECT_THROW((void)volume({}), std::exception);
  level = {};
  occupancy = {};
  processing.maxVoxelCount = 1;
  EXPECT_THROW((void)sdf({}), std::exception);
  EXPECT_THROW((void)volume({}), std::exception);
  processing = {};
  processing.maxInputBytes = 1;
  EXPECT_THROW((void)sdf({}), std::exception);
  EXPECT_THROW((void)volume({}), std::exception);
  processing = {};
  auto other = plane();
  other->setName("other");
  write("two.vdb", {plane(), other});
  level.limits.maxGrids = 1;
  occupancy.limits.maxGrids = 1;
  EXPECT_THROW((void)sdf({}, "two.vdb", "descriptor:plane"), std::exception);
  EXPECT_THROW((void)volume({}, "two.vdb", "descriptor:plane"), std::exception);
  EXPECT_EQ(bytes(path("input.vdb")), before);
  std::ofstream corrupt(path("corrupt.vdb"), std::ios::binary);
  corrupt << "not a VDB archive";
  corrupt.close();
  EXPECT_THROW((void)sdf({}, "corrupt.vdb"), std::exception);
  EXPECT_THROW((void)volume({}, "corrupt.vdb"), std::exception);
}
#endif
} // namespace
