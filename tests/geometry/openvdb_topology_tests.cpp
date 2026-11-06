#include "Container/geometry/OpenVdbTopology.h"
#include "Container/geometry/OpenVdbMaskedValues.h"
#include "Container/geometry/OpenVdbSdfBackground.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <set>
#include <sstream>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/LevelSetSphere.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
using Operation = vdb::TopologyOperation;
using Boolean = vdb::TopologyBoolean;
using Connectivity = vdb::TopologyConnectivity;
using TilePolicy = vdb::TopologyTilePolicy;

TEST(OpenVdbTopologyAvailability, DisabledPrecedesBothApisOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  for (bool binary : {false, true}) {
    try {
      vdb::ProcessingOptions limits;
      limits.maxInputBytes = 0;
      if (binary)
        (void)vdb::WriteTopologyBoolean("missing-a", "missing-b", {},
                                        static_cast<Boolean>(99), {}, {},
                                        limits, 0);
      else
        (void)vdb::WriteTopologyMask("missing", {}, static_cast<Operation>(99),
                                     {.iterations = 0}, {}, limits);
      FAIL();
    } catch (const std::runtime_error &error) {
      EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
                std::string::npos);
    }
  }
#endif
}

TEST(OpenVdbTopologyAvailability, ValueEditingDisabledPrecedesOptionsAndIo) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  try {
    (void)vdb::WriteValueTopology("missing",{},static_cast<vdb::ValueTopologyOperation>(99),{.maxWorkItems=0});
    FAIL();
  } catch(const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);
  }
#endif
}
TEST(OpenVdbTopologyAvailability, MaskedValuesDisabledPrecedesOptionsAndIo) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  try {
    vdb::ProcessingOptions p; p.maxVoxelCount=0;
    (void)vdb::WriteMaskedValues("missing","missing",{},static_cast<vdb::MaskedValuesOperation>(99),{},{},p,0);
    FAIL();
  } catch(const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);
  }
#endif
}
TEST(OpenVdbTopologyAvailability, SdfBackgroundDisabledPrecedesOptionsAndIo) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  try {
    (void)vdb::WriteSdfBackground("missing",{},static_cast<vdb::SdfBackgroundOperation>(99),{.maxWorkItems=0}); FAIL();
  } catch(const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using Cell = std::array<int32_t, 3>;
using Cells = std::set<Cell>;
openvdb::Coord coord(const Cell &cell) { return {cell[0], cell[1], cell[2]}; }
Cell translated(const Cell &a, const Cell &b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Cells neighbors(Connectivity connectivity) {
  Cells result;
  const int maximum = connectivity == Connectivity::Faces6         ? 1
                      : connectivity == Connectivity::FacesEdges18 ? 2
                                                                   : 3;
  for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) {
        const int distance = std::abs(x) + std::abs(y) + std::abs(z);
        if (distance > 0 && distance <= maximum)
          result.insert({x, y, z});
      }
  return result;
}
Cells dilated(Cells source, Connectivity connectivity,
              unsigned iterations = 1) {
  const auto stencil = neighbors(connectivity);
  for (unsigned n = 0; n < iterations; ++n) {
    auto result = source;
    for (const auto &cell : source)
      for (const auto &offset : stencil)
        result.insert(translated(cell, offset));
    source = std::move(result);
  }
  return source;
}
Cells eroded(Cells source, Connectivity connectivity, unsigned iterations = 1) {
  const auto stencil = neighbors(connectivity);
  for (unsigned n = 0; n < iterations; ++n) {
    Cells result;
    for (const auto &cell : source) {
      bool retained = true;
      for (const auto &offset : stencil)
        if (!source.contains(translated(cell, offset))) {
          retained = false;
          break;
        }
      if (retained)
        result.insert(cell);
    }
    source = std::move(result);
  }
  return source;
}
Cells box(int minimum, int maximum) {
  Cells result;
  for (int z = minimum; z <= maximum; ++z)
    for (int y = minimum; y <= maximum; ++y)
      for (int x = minimum; x <= maximum; ++x)
        result.insert({x, y, z});
  return result;
}

class OpenVdbTopology : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t next{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-topology-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++next));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    processing.gridName = "topology";
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  std::string bytes(const std::string &name) const {
    std::ifstream input(path(name), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             bool statistics = true) {
    openvdb::io::File output(path(name).string());
    output.setCompression(openvdb::io::COMPRESS_ZIP);
    output.setGridStatsMetadataEnabled(statistics);
    output.write(grids);
  }
  auto read(const std::string &name) {
    openvdb::io::File input(path(name).string());
    input.open(false);
    auto result = openvdb::gridPtrCast<openvdb::MaskGrid>(
        input.readGrid(processing.gridName));
    input.close();
    return result;
  }
  static auto field(const Cells &cells = {{0, 0, 0}}) {
    auto result = openvdb::FloatGrid::create(7);
    result->setName("source");
    for (const auto &cell : cells)
      result->tree().setValue(coord(cell), 0.f);
    return result;
  }
  void cleanupCheck() {
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      EXPECT_FALSE(entry.is_directory()) << entry.path();
  }
  void check(const openvdb::MaskGrid &actual, const Cells &expected) {
    EXPECT_FALSE(actual.background());
    EXPECT_EQ(actual.activeVoxelCount(), expected.size());
    for (const auto &cell : expected) {
      EXPECT_TRUE(actual.tree().isValueOn(coord(cell))) << coord(cell);
      EXPECT_TRUE(actual.tree().getValue(coord(cell))) << coord(cell);
      for (const auto &offset : neighbors(Connectivity::Full26)) {
        const auto adjacent = translated(cell, offset);
        if (!expected.contains(adjacent)) {
          EXPECT_FALSE(actual.tree().isValueOn(coord(adjacent)))
              << coord(adjacent);
          EXPECT_FALSE(actual.tree().getValue(coord(adjacent)))
              << coord(adjacent);
        }
      }
    }
    // Mask values must agree with activity even in explicitly stored inactive
    // slots.
    for (auto value = actual.cbeginValueAll(); value; ++value)
      EXPECT_EQ(bool(*value), value.isValueOn());
  }
  vdb::TopologyResult apply(const std::string &input, const std::string &output,
                            Operation operation = Operation::ActiveMask,
                            const vdb::TopologyOptions &options = {},
                            const vdb::ProcessingOptions *limits = nullptr,
                            const std::string &selection = {}) {
    return vdb::WriteTopologyMask(path(input), path(output), operation, options,
                                  selection, limits ? *limits : processing);
  }
  void failure(const std::string &input,
               Operation operation = Operation::ActiveMask,
               const vdb::TopologyOptions &options = {},
               const vdb::ProcessingOptions *limits = nullptr,
               const std::string &selection = {}) {
    const auto output = "failed-" + std::to_string(++serial) + ".vdb";
    EXPECT_THROW(
        (void)apply(input, output, operation, options, limits, selection),
        std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(output)));
    cleanupCheck();
  }
  void binaryFailure(const std::string &a, const std::string &b,
                     Boolean operation = Boolean::Union,
                     const vdb::ProcessingOptions *limits = nullptr,
                     uint64_t work = 200'000'000, const std::string &gridA = {},
                     const std::string &gridB = {}) {
    const auto output = "failed-" + std::to_string(++serial) + ".vdb";
    EXPECT_THROW((void)vdb::WriteTopologyBoolean(
                     path(a), path(b), path(output), operation, gridA, gridB,
                     limits ? *limits : processing, work),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(output)));
    cleanupCheck();
  }
  template <class Grid>
  void nativeFlags(const std::string &tag, typename Grid::ValueType background,
                   typename Grid::ValueType activeValue,
                   typename Grid::ValueType inactiveValue) {
    auto source = Grid::create(background);
    source->setName("source");
    source->tree().setValue({-9, -1, 7}, activeValue);
    source->tree()
        .touchLeaf({32, 0, 0})
        ->setValueOff({32, 0, 0}, inactiveValue);
    source->setGridClass(openvdb::GRID_STAGGERED);
    source->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);
    source->setIsInWorldSpace(false);
    source->insertMeta("artist", openvdb::StringMetadata("source metadata"));
    write(tag + ".vdb", {source});
    const auto original = bytes(tag + ".vdb");
    const auto report = apply(tag + ".vdb", tag + "-mask.vdb");
    const auto actual = read(tag + "-mask.vdb");
    ASSERT_TRUE(actual);
    check(*actual, {{-9, -1, 7}});
    EXPECT_EQ(actual->tree().leafCount(), source->tree().leafCount());
    EXPECT_EQ(report.inputActiveVoxelCountA, 1u);
    EXPECT_EQ(report.inputActiveVoxelCountB, 0u);
    EXPECT_EQ(report.storedLeafCount, 2u);
    EXPECT_EQ(report.grid.gridName, processing.gridName);
    EXPECT_EQ(report.grid.activeVoxelCount, 1u);
    EXPECT_EQ(actual->getGridClass(), openvdb::GRID_UNKNOWN);
    EXPECT_EQ(actual->getVectorType(), openvdb::VEC_INVARIANT);
    EXPECT_TRUE(actual->isInWorldSpace());
    EXPECT_EQ(actual->template metaValue<std::string>("artist"),
              "source metadata");
    EXPECT_EQ(bytes(tag + ".vdb"), original);
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions processing;
  size_t serial{0};
};

TEST_F(OpenVdbTopology,
       AllNineNativeTypesUseActivityRatherThanStoredValuesOrSemantics) {
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  nativeFlags<openvdb::FloatGrid>("float", float(nan), float(nan), -3.f);
  nativeFlags<openvdb::DoubleGrid>("double", nan, 0., -3.);
  nativeFlags<openvdb::BoolGrid>("bool", true, false, true);
  nativeFlags<openvdb::Int32Grid>("int32", INT32_MIN, 0, INT32_MAX);
  nativeFlags<openvdb::Int64Grid>("int64", INT64_MAX, INT64_MIN, INT64_MAX);
  nativeFlags<openvdb::Vec3IGrid>("vec3i", {4, 5, 6}, {0, -1, INT32_MIN},
                                  {INT32_MAX, 2, 3});
  nativeFlags<openvdb::Vec3SGrid>("vec3s", {float(nan), 2, 3}, {0, 0, 0},
                                  {-3, 4, 5});
  nativeFlags<openvdb::Vec3DGrid>("vec3d", {nan, 2, 3}, {nan, 0, -0.},
                                  {-3, 4, 5});
  auto mask = openvdb::MaskGrid::create();
  mask->setName("source");
  mask->tree().setValueOn({-9, -1, 7});
  mask->tree().touchLeaf({32, 0, 0});
  write("mask.vdb", {mask});
  const auto report = apply("mask.vdb", "mask-copy.vdb");
  check(*read("mask-copy.vdb"), {{-9, -1, 7}});
  EXPECT_EQ(report.storedLeafCount, 2u);
}

TEST_F(OpenVdbTopology,
       ActiveMaskRetainsCompactTilesAndRemoteInactiveTopology) {
  auto source = openvdb::FloatGrid::create(-3.f);
  source->setName("source");
  source->setGridClass(openvdb::GRID_LEVEL_SET);
  source->tree().addTile(1, {0, 0, 0}, 0.f, true);
  source->tree().touchLeaf({64, 0, 0})->setValueOff({64, 0, 0}, -20.f);
  write("source.vdb", {source});
  const auto report = apply("source.vdb", "copy.vdb");
  auto actual = read("copy.vdb");
  check(*actual, box(0, 7));
  EXPECT_EQ(report.inputActiveVoxelCountA, 512u);
  EXPECT_EQ(report.activeTileCount, 1u);
  EXPECT_EQ(report.storedLeafCount, 1u);
  EXPECT_EQ(actual->tree().activeTileCount(), 1u);
  EXPECT_FALSE(actual->tree().getValue({64, 0, 0}));
}

TEST_F(OpenVdbTopology,
       DilationMatchesIndependentNeighborSetsAtNegativeLeafBoundaries) {
  const Cells initial{{-9, -1, 7}};
  write("source.vdb", {field(initial)});
  for (auto connectivity :
       {Connectivity::Faces6, Connectivity::FacesEdges18, Connectivity::Full26})
    for (uint32_t iterations : {1u, 2u}) {
      vdb::TopologyOptions options;
      options.connectivity = connectivity;
      options.iterations = iterations;
      const auto output = "dilate-" + std::to_string(++serial) + ".vdb";
      const auto report =
          apply("source.vdb", output, Operation::Dilate, options);
      const auto expected = dilated(initial, connectivity, iterations);
      check(*read(output), expected);
      EXPECT_EQ(report.grid.activeVoxelCount, expected.size());
      EXPECT_GT(report.workItems, 0u);
    }
}

TEST_F(OpenVdbTopology, ErosionMatchesIndependentSetsAndRemovesIsolatedCells) {
  const Cells initial = box(-2, 2);
  write("cube.vdb", {field(initial)});
  for (auto connectivity :
       {Connectivity::Faces6, Connectivity::FacesEdges18, Connectivity::Full26})
    for (uint32_t iterations : {1u, 2u}) {
      vdb::TopologyOptions options;
      options.connectivity = connectivity;
      options.iterations = iterations;
      const auto output = "erode-" + std::to_string(++serial) + ".vdb";
      (void)apply("cube.vdb", output, Operation::Erode, options);
      check(*read(output), eroded(initial, connectivity, iterations));
    }
  write("isolated.vdb", {field()});
  (void)apply("isolated.vdb", "empty.vdb", Operation::Erode);
  check(*read("empty.vdb"), {});
}

TEST_F(OpenVdbTopology, OpenAndCloseFollowIndependentOrderedSetComposition) {
  Cells initial = box(-1, 1);
  initial.erase({0, 0, 0});
  initial.insert({8, -3, 2});
  initial.insert({-2, 0, 0});
  initial.insert({-3, 0, 0});
  write("source.vdb", {field(initial)});
  for (auto connectivity :
       {Connectivity::Faces6, Connectivity::FacesEdges18, Connectivity::Full26})
    for (auto operation : {Operation::Open, Operation::Close}) {
      vdb::TopologyOptions options;
      options.connectivity = connectivity;
      const auto output = "compose-" + std::to_string(++serial) + ".vdb";
      const auto expected =
          operation == Operation::Open
              ? dilated(eroded(initial, connectivity), connectivity)
              : eroded(dilated(initial, connectivity), connectivity);
      (void)apply("source.vdb", output, operation, options);
      check(*read(output), expected);
    }
}

TEST_F(OpenVdbTopology,
       PreserveAndExpandTilesHaveSameOccupancyAndDifferentStorage) {
  auto source = openvdb::FloatGrid::create();
  source->setName("source");
  source->tree().addTile(1, {0, 0, 0}, 5.f, true);
  write("tile.vdb", {source});
  const auto initial = box(0, 7);
  for (auto policy : {TilePolicy::Preserve, TilePolicy::Expand})
    for (auto operation : {Operation::Dilate, Operation::Erode}) {
      vdb::TopologyOptions options;
      options.tilePolicy = policy;
      const auto output = "tile-" + std::to_string(++serial) + ".vdb";
      const auto report = apply("tile.vdb", output, operation, options);
      const auto actual = read(output);
      check(*actual, operation == Operation::Dilate
                         ? dilated(initial, Connectivity::Faces6)
                         : eroded(initial, Connectivity::Faces6));
      if (policy == TilePolicy::Expand) {
        EXPECT_EQ(report.activeTileCount, 0u);
        EXPECT_GT(report.storedLeafCount, 0u);
      } else if (operation == Operation::Dilate) {
        EXPECT_EQ(report.activeTileCount, 1u);
      }
    }
}

TEST_F(OpenVdbTopology,
       IgnoreLeavesTilesUnchangedButUsesThemAsErosionNeighbors) {
  auto source = openvdb::FloatGrid::create();
  source->setName("source");
  source->tree().addTile(1, {0, 0, 0}, 5.f, true);
  write("tile.vdb", {source});
  vdb::TopologyOptions options;
  options.tilePolicy = TilePolicy::Ignore;
  for (auto operation : {Operation::Dilate, Operation::Erode, Operation::Open,
                         Operation::Close}) {
    const auto output = "ignore-" + std::to_string(++serial) + ".vdb";
    (void)apply("tile.vdb", output, operation, options);
    check(*read(output), box(0, 7));
  }
  for (const Cell cell :
       Cells{{8, 3, 3}, {9, 3, 3}, {8, 2, 3}, {8, 4, 3}, {8, 3, 2}, {8, 3, 4}})
    source->tree().setValue(coord(cell), 0.f);
  write("neighbors.vdb", {source});
  (void)apply("neighbors.vdb", "eroded.vdb", Operation::Erode, options);
  auto expected = box(0, 7);
  expected.insert({8, 3, 3});
  check(*read("eroded.vdb"), expected);
}

TEST_F(OpenVdbTopology, WidelySeparatedActiveCellsUseLocalCandidateBounds) {
  const Cells initial{{-1'000'000, 0, 0}, {1'000'000, 0, 0}};
  auto source = field(initial);
  write("remote.vdb", {source});
  auto limits = processing;
  limits.maxVoxelCount = 32'768;
  vdb::TopologyOptions options;
  options.connectivity = Connectivity::Full26;
  const auto report = apply("remote.vdb", "remote-dilated.vdb",
                            Operation::Dilate, options, &limits);
  check(*read("remote-dilated.vdb"), dilated(initial, Connectivity::Full26));
  EXPECT_EQ(report.grid.activeVoxelCount, 54u);
}

TEST_F(OpenVdbTopology,
       NonFaceErosionReservesItsExtraInverseDilationWavefrontBeforeAllocation) {
  write("source.vdb", {field()});
  const auto original = bytes("source.vdb");
  auto limits = processing;
  // Radius eight around the original [0,7]^3 leaf covers 27 leaves. Native
  // 26-neighbor erosion first dilates once to construct its inverse wavefront,
  // then dilates that wavefront by the requested count. The scratch therefore
  // reaches distance nine and cannot be admitted under this leaf-slot cap.
  limits.maxVoxelCount = 27 * 512;
  vdb::TopologyOptions options;
  options.connectivity = Connectivity::Full26;
  options.iterations = 8;
  failure("source.vdb", Operation::Erode, options, &limits);
  // Close adds four dilation steps before that four-plus-one erosion scratch.
  options.iterations = 4;
  failure("source.vdb", Operation::Close, options, &limits);
  EXPECT_EQ(bytes("source.vdb"), original);
}

TEST_F(OpenVdbTopology, BooleansAcceptMixedClassesAndIgnoreAllNativeValues) {
  auto a = openvdb::DoubleGrid::create(-3);
  a->setName("a");
  a->setGridClass(openvdb::GRID_LEVEL_SET);
  for (const Cell cell : Cells{{-1, 0, 0}, {0, 0, 0}, {1, 0, 0}})
    a->tree().setValue(coord(cell), std::numeric_limits<double>::quiet_NaN());
  a->insertMeta("artist", openvdb::StringMetadata("left"));
  a->insertMeta("container:point_count", openvdb::Int64Metadata(42));
  a->insertMeta("container:mac_layout",
                openvdb::StringMetadata("negative_faces"));
  auto b = openvdb::BoolGrid::create(true);
  b->setName("b");
  b->setGridClass(openvdb::GRID_FOG_VOLUME);
  b->tree().setValue({0, 0, 0}, false);
  b->tree().setValue({2, 0, 0}, false);
  b->tree().touchLeaf({32, 0, 0})->setValueOff({32, 0, 0}, true);
  b->insertMeta("right-only", openvdb::StringMetadata("not copied"));
  write("a.vdb", {a});
  write("b.vdb", {b});
  const auto beforeA = bytes("a.vdb"), beforeB = bytes("b.vdb");
  for (auto operation :
       {Boolean::Union, Boolean::Intersection, Boolean::Difference}) {
    const auto output = "boolean-" + std::to_string(++serial) + ".vdb";
    const auto report =
        vdb::WriteTopologyBoolean(path("a.vdb"), path("b.vdb"), path(output),
                                  operation, {}, {}, processing);
    const Cells expected =
        operation == Boolean::Union
            ? Cells{{-1, 0, 0}, {0, 0, 0}, {1, 0, 0}, {2, 0, 0}}
        : operation == Boolean::Intersection ? Cells{{0, 0, 0}}
                                             : Cells{{-1, 0, 0}, {1, 0, 0}};
    const auto actual = read(output);
    check(*actual, expected);
    EXPECT_EQ(report.inputActiveVoxelCountA, 3u);
    EXPECT_EQ(report.inputActiveVoxelCountB, 2u);
    EXPECT_EQ(actual->getGridClass(), openvdb::GRID_UNKNOWN);
    EXPECT_EQ(actual->getVectorType(), openvdb::VEC_INVARIANT);
    EXPECT_TRUE(actual->isInWorldSpace());
    EXPECT_EQ(actual->metaValue<std::string>("artist"), "left");
    EXPECT_FALSE(bool((*actual)["right-only"]));
    EXPECT_FALSE(bool((*actual)["container:point_count"]));
    EXPECT_FALSE(bool((*actual)["container:mac_layout"]));
    bool provenance = false;
    for (auto it = actual->beginMeta(); it != actual->endMeta(); ++it)
      provenance |= it->first.starts_with("container:topology_");
    EXPECT_TRUE(provenance);
  }
  EXPECT_EQ(bytes("a.vdb"), beforeA);
  EXPECT_EQ(bytes("b.vdb"), beforeB);
}

TEST_F(OpenVdbTopology, BooleanTilesRemainCompactWithoutVoxelization) {
  auto a = openvdb::FloatGrid::create();
  a->setName("source");
  auto b = openvdb::Int64Grid::create(77);
  b->setName("source");
  a->tree().addTile(1, {0, 0, 0}, 0.f, true);
  b->tree().addTile(1, {8, 0, 0}, int64_t(0), true);
  write("a.vdb", {a});
  write("b.vdb", {b});
  for (auto operation :
       {Boolean::Union, Boolean::Intersection, Boolean::Difference}) {
    const auto output = "tiles-" + std::to_string(++serial) + ".vdb";
    const auto report =
        vdb::WriteTopologyBoolean(path("a.vdb"), path("b.vdb"), path(output),
                                  operation, {}, {}, processing);
    const auto actual = read(output);
    EXPECT_EQ(report.storedLeafCount, 0u);
    EXPECT_EQ(actual->tree().leafCount(), 0u);
    EXPECT_EQ(report.grid.activeVoxelCount, operation == Boolean::Union ? 1024u
                                            : operation == Boolean::Intersection
                                                ? 0u
                                                : 512u);
    EXPECT_EQ(actual->tree().getValue({3, 3, 3}),
              operation != Boolean::Intersection);
    EXPECT_EQ(actual->tree().getValue({11, 3, 3}), operation == Boolean::Union);
  }
}

TEST_F(OpenVdbTopology, ExactAffineMapsArePreservedAndNearMatchesReject) {
  auto a = field(), b = field();
  a->transform().postScale(openvdb::Vec3d(-2, 3, .5));
  a->transform().postRotate(.3, openvdb::math::Z_AXIS);
  a->transform().postTranslate({12, -34, 56});
  b->setTransform(a->transform().copy());
  write("a.vdb", {a});
  write("b.vdb", {b});
  (void)vdb::WriteTopologyBoolean(path("a.vdb"), path("b.vdb"),
                                  path("matching.vdb"), Boolean::Union, {}, {},
                                  processing);
  const auto actual = read("matching.vdb");
  for (const auto &position :
       {openvdb::Vec3d(0), openvdb::Vec3d(1, 2, 3), openvdb::Vec3d(-7, 4, 2)})
    EXPECT_EQ(actual->indexToWorld(position), a->indexToWorld(position));
  b->transform().postTranslate({1e-9, 0, 0});
  write("near.vdb", {b});
  binaryFailure("a.vdb", "near.vdb");
  auto nonlinear = field();
  nonlinear->setTransform(openvdb::math::Transform::createFrustumTransform(
      openvdb::BBoxd(openvdb::Vec3d(-1), openvdb::Vec3d(1)), .5, 10., 1.));
  write("nonlinear.vdb", {nonlinear});
  failure("nonlinear.vdb");
  binaryFailure("a.vdb", "nonlinear.vdb");
}

TEST_F(OpenVdbTopology,
       LimitsInvalidOptionsAndCoordinateExpansionFailBeforeOutput) {
  auto source = field();
  source->tree().touchLeaf({64, 0, 0})->setValueOff({64, 0, 0}, -8.f);
  write("source.vdb", {source});
  auto limits = processing;
  limits.maxVoxelCount = 512;
  failure("source.vdb", Operation::ActiveMask, {}, &limits);
  limits = processing;
  limits.maxInputBytes = 1;
  failure("source.vdb", Operation::ActiveMask, {}, &limits);
  binaryFailure("source.vdb", "source.vdb", Boolean::Union, &limits);
  vdb::TopologyOptions options;
  options.maxWorkItems = 1;
  failure("source.vdb", Operation::Dilate, options);
  binaryFailure("source.vdb", "source.vdb", Boolean::Union, nullptr, 1);
  options = {};
  options.iterations = 2;
  failure("source.vdb", Operation::ActiveMask, options);
  options = {};
  options.connectivity = Connectivity::Full26;
  failure("source.vdb", Operation::ActiveMask, options);
  options = {};
  options.tilePolicy = TilePolicy::Ignore;
  failure("source.vdb", Operation::ActiveMask, options);
  options = {};
  options.iterations = 0;
  failure("source.vdb", Operation::Dilate, options);
  options.iterations = 33;
  failure("source.vdb", Operation::Dilate, options);
  options = {};
  options.connectivity = static_cast<Connectivity>(99);
  failure("source.vdb", Operation::Dilate, options);
  options = {};
  options.tilePolicy = static_cast<TilePolicy>(99);
  failure("source.vdb", Operation::Dilate, options);
  failure("source.vdb", static_cast<Operation>(99));
  binaryFailure("source.vdb", "source.vdb", static_cast<Boolean>(99));
  auto edge = field({{INT32_MAX, 0, 0}});
  write("edge.vdb", {edge});
  failure("edge.vdb", Operation::Dilate);
  auto metadata = field();
  metadata->insertMeta("large",
                       openvdb::StringMetadata(std::string(1'000'000, 'x')));
  write("metadata.vdb", {metadata});
  limits = processing;
  limits.maxInputBytes = 8 * 1024 * 1024;
  failure("metadata.vdb", Operation::ActiveMask, {}, &limits);
}

TEST_F(OpenVdbTopology,
       DescriptorSelectionAndExistingSourcesAndOutputsAreProtected) {
  auto a = field({{0, 0, 0}}), duplicate = field({{16, 0, 0}});
  write("duplicates.vdb", {a, duplicate});
  write("b.vdb", {a});
  const auto before = bytes("duplicates.vdb");
  failure("duplicates.vdb", Operation::ActiveMask, {}, nullptr, "source");
  (void)apply("duplicates.vdb", "selected.vdb", Operation::ActiveMask, {},
              nullptr, "descriptor:source[1]");
  check(*read("selected.vdb"), {{16, 0, 0}});
  binaryFailure("duplicates.vdb", "b.vdb", Boolean::Union, nullptr, 200'000'000,
                "missing");
  binaryFailure("duplicates.vdb", "b.vdb", Boolean::Union, nullptr, 200'000'000,
                "descriptor:source[1]", "missing");
  (void)vdb::WriteTopologyBoolean(
      path("duplicates.vdb"), path("b.vdb"), path("binary-selected.vdb"),
      Boolean::Union, "descriptor:source[1]", "descriptor:source", processing);
  check(*read("binary-selected.vdb"), {{0, 0, 0}, {16, 0, 0}});
  a->setName("descriptor:source");
  write("literal.vdb", {a});
  (void)apply("literal.vdb", "literal-out.vdb", Operation::ActiveMask, {},
              nullptr, "descriptor:descriptor:source");
  check(*read("literal-out.vdb"), {{0, 0, 0}});
  const auto existing = bytes("literal-out.vdb");
  EXPECT_THROW((void)apply("literal.vdb", "literal-out.vdb"), std::exception);
  EXPECT_THROW((void)apply("literal.vdb", "literal.vdb"), std::exception);
  EXPECT_THROW((void)vdb::WriteTopologyBoolean(path("b.vdb"), path("b.vdb"),
                                               path("b.vdb"), Boolean::Union,
                                               {}, {}, processing),
               std::exception);
  EXPECT_EQ(bytes("literal-out.vdb"), existing);
  EXPECT_EQ(bytes("duplicates.vdb"), before);
  cleanupCheck();
}

TEST_F(OpenVdbTopology,
       UnsupportedPointGridsMissingStatsAndStreamingFilesReject) {
  auto point = openvdb::points::PointDataGrid::create();
  point->setName("points");
  write("points.vdb", {point});
  failure("points.vdb");
  write("source.vdb", {field()});
  binaryFailure("source.vdb", "points.vdb");
  write("no-stats.vdb", {field()}, false);
  failure("no-stats.vdb");
  {
    std::ofstream output(path("stream.vdb"), std::ios::binary);
    openvdb::io::Stream stream(output);
    stream.write(openvdb::GridPtrVec{field()});
  }
  failure("stream.vdb");
  binaryFailure("source.vdb", "stream.vdb");
}
using Edit=vdb::ValueTopologyOperation;
class OpenVdbValueTopology:public OpenVdbTopology {
protected:
  template<class Grid> auto native(const char *name) {
    openvdb::io::File file(path(name).string()); file.open(false);
    return openvdb::gridPtrCast<Grid>(file.readGrid(processing.gridName));
  }
  auto edit(const char *input,const char *output,Edit op,const vdb::ValueTopologyOptions &o={}) {
    return vdb::WriteValueTopology(path(input),path(output),op,o,{},processing);
  }
  template<class Grid> void activity(typename Grid::ValueType value,const std::vector<std::string> &literal,
                                     const char *suffix) {
    auto grid=Grid::create(); grid->setName("source");
    grid->tree().setValueOff(openvdb::Coord(-1,2,3),value);
    grid->tree().setValueOn(openvdb::Coord(0,2,3),value);
    grid->insertMeta("kept",openvdb::StringMetadata("original"));
    const std::string input=std::string(suffix)+"-in.vdb", on=std::string(suffix)+"-on.vdb", off=std::string(suffix)+"-off.vdb";
    write(input,{grid}); const auto original=bytes(input);
    (void)edit(input.c_str(),on.c_str(),Edit::ActivateValue,{.value=literal});
    auto result=native<Grid>(on.c_str());
    EXPECT_TRUE(result->tree().isValueOn(openvdb::Coord(-1,2,3)));
    EXPECT_EQ(result->tree().getValue(openvdb::Coord(-1,2,3)),value);
    EXPECT_EQ(result->template metaValue<std::string>("kept"),"original");
    (void)edit(on.c_str(),off.c_str(),Edit::DeactivateValue,{.value=literal});
    result=native<Grid>(off.c_str());
    EXPECT_FALSE(result->tree().isValueOn(openvdb::Coord(-1,2,3)));
    EXPECT_FALSE(result->tree().isValueOn(openvdb::Coord(0,2,3)));
    EXPECT_EQ(result->tree().getValue(openvdb::Coord(0,2,3)),value);
    EXPECT_EQ(bytes(input),original);
  }
};
TEST_F(OpenVdbValueTopology, EightNativeTypesRetainValuesWhileEditingActivity) {
  activity<openvdb::FloatGrid>(1.25f,{"1.25"},"f");
  activity<openvdb::DoubleGrid>(1.125,{"1.125"},"d");
  activity<openvdb::BoolGrid>(true,{"true"},"b");
  activity<openvdb::Int32Grid>(INT32_MIN,{"-2147483648"},"i");
  activity<openvdb::Int64Grid>(INT64_MAX,{"9223372036854775807"},"l");
  activity<openvdb::Vec3IGrid>({INT32_MIN,3,INT32_MAX},{"-2147483648","3","2147483647"},"vi");
  activity<openvdb::Vec3SGrid>({1.25f,-2.f,3.f},{"1.25","-2","3"},"vf");
  activity<openvdb::Vec3DGrid>({1.125,-2.,3.},{"1.125","-2","3"},"vd");
}
TEST_F(OpenVdbValueTopology, SignedZeroActivationAndPruningUseExactBits) {
  auto grid=openvdb::DoubleGrid::create(0.); grid->setName("source");
  grid->tree().fill(openvdb::CoordBBox({0,0,0},{7,7,7}),-0.,false);
  grid->tree().touchLeaf({0,0,0})->setValueOff({0,0,0},0.);
  write("zero.vdb",{grid});
  (void)edit("zero.vdb","pruned.vdb",Edit::PruneExact);
  auto out=native<openvdb::DoubleGrid>("pruned.vdb");
  EXPECT_EQ(out->tree().leafCount(),1u);
  EXPECT_TRUE(std::signbit(out->tree().getValue({1,0,0})));
  EXPECT_FALSE(std::signbit(out->tree().getValue({0,0,0})));
  (void)edit("zero.vdb","active.vdb",Edit::ActivateValue,{.value={"-0"}});
  out=native<openvdb::DoubleGrid>("active.vdb");
  EXPECT_EQ(out->activeVoxelCount(),511u);
  EXPECT_FALSE(out->tree().isValueOn({0,0,0}));
  EXPECT_TRUE(out->tree().isValueOn({1,0,0}));
  grid->tree().touchLeaf({0,0,0})->setValueOff({0,0,0},-0.); write("constant.vdb",{grid});
  (void)edit("constant.vdb","constant-out.vdb",Edit::PruneExact);
  out=native<openvdb::DoubleGrid>("constant-out.vdb");
  EXPECT_EQ(out->tree().leafCount(),0u);
  EXPECT_TRUE(std::signbit(out->tree().getValue({0,0,0})));
  EXPECT_FALSE(std::signbit(out->background()));
}
TEST_F(OpenVdbValueTopology, PruneRetainsMixedActivityAndCompactsConstantTiles) {
  auto grid=openvdb::Int64Grid::create(); grid->setName("source");
  grid->tree().fill(openvdb::CoordBBox({0,0,0},{7,7,7}),INT64_MAX,true);
  grid->tree().setValueOff({1,0,0},INT64_MAX); write("mixed.vdb",{grid});
  (void)edit("mixed.vdb","mixed-out.vdb",Edit::PruneExact);
  auto result=native<openvdb::Int64Grid>("mixed-out.vdb");
  EXPECT_EQ(result->tree().leafCount(),1u); EXPECT_FALSE(result->tree().isValueOn({1,0,0}));
  grid->tree().setValueOn({1,0,0},INT64_MAX); write("constant.vdb",{grid});
  const auto report=edit("constant.vdb","constant-out.vdb",Edit::PruneExact);
  EXPECT_EQ(report.storedLeafCount,0u); EXPECT_EQ(report.activeTileCount,1u);
  EXPECT_EQ(native<openvdb::Int64Grid>("constant-out.vdb")->tree().getValue({3,2,1}),INT64_MAX);
}
TEST_F(OpenVdbValueTopology, BackgroundChangesOnlyExactInactiveMatchesAndRetainsZeroSign) {
  auto grid=openvdb::DoubleGrid::create(2.); grid->setName("source");
  grid->tree().setValueOff({0,0,0},2.); grid->tree().setValueOff({1,0,0},-2.);
  grid->tree().setValueOff({2,0,0},std::nextafter(2.,3.)); grid->tree().setValueOn({3,0,0},2.);
  write("bg.vdb",{grid});
  (void)edit("bg.vdb","bg-out.vdb",Edit::ReplaceBackground,{.value={"7"}});
  auto out=native<openvdb::DoubleGrid>("bg-out.vdb");
  EXPECT_EQ(out->background(),7.); EXPECT_EQ(out->tree().getValue({0,0,0}),7.);
  EXPECT_EQ(out->tree().getValue({1,0,0}),-2.);
  EXPECT_EQ(out->tree().getValue({2,0,0}),std::nextafter(2.,3.));
  EXPECT_EQ(out->tree().getValue({3,0,0}),2.); EXPECT_TRUE(out->tree().isValueOn({3,0,0}));
  grid=openvdb::DoubleGrid::create(0.); grid->setName("zero");
  grid->tree().touchLeaf({0,0,0})->setValueOff({0,0,0},-0.); write("zero-bg.vdb",{grid});
  (void)edit("zero-bg.vdb","zero-bg-out.vdb",Edit::ReplaceBackground,{.value={"-0"}});
  out=native<openvdb::DoubleGrid>("zero-bg-out.vdb");
  EXPECT_TRUE(std::signbit(out->background())); EXPECT_TRUE(std::signbit(out->tree().getValue({0,0,0})));
}
TEST_F(OpenVdbValueTopology, BoxFillUsesInclusiveIndexCoordinatesAndPreservesRemoteBits) {
  auto grid=openvdb::DoubleGrid::create(-0.); grid->setName("source");
  grid->tree().setValueOff({100000,0,0},-0.); grid->tree().setValueOn({0,0,0},9.);
  grid->transform().postTranslate({2,3,4}); write("fill.vdb",{grid});
  vdb::ValueTopologyOptions options{.value={"1.125"},.minimum={-2,-1,0},.maximum={0,1,0},.fillActive=false};
  (void)edit("fill.vdb","fill-out.vdb",Edit::FillBox,options);
  auto out=native<openvdb::DoubleGrid>("fill-out.vdb");
  for(int x=-2;x<=0;++x) for(int y=-1;y<=1;++y) {
    EXPECT_EQ(out->tree().getValue({x,y,0}),1.125); EXPECT_FALSE(out->tree().isValueOn({x,y,0}));
  }
  EXPECT_TRUE(std::signbit(out->tree().getValue({100000,0,0})));
  EXPECT_EQ(out->transform(),grid->transform()); EXPECT_EQ(out->activeVoxelCount(),0u);
}
TEST_F(OpenVdbValueTopology, ClipPreservesNativeBitsAndActivityInsideAndBackgroundOutside) {
  auto grid=openvdb::DoubleGrid::create(0.); grid->setName("source");
  grid->tree().fill(openvdb::CoordBBox({-8,-8,-8},{7,7,7}),-0.,false);
  grid->tree().setValueOn({-1,0,0},std::nextafter(1.,2.));
  grid->tree().setValueOn({100000,0,0},9.); write("clip.vdb",{grid});
  vdb::ValueTopologyOptions options; options.minimum={-2,-1,-1}; options.maximum={2,1,1};
  (void)edit("clip.vdb","clip-out.vdb",Edit::ClipBox,options);
  auto out=native<openvdb::DoubleGrid>("clip-out.vdb");
  EXPECT_EQ(out->activeVoxelCount(),1u);
  EXPECT_EQ(out->tree().getValue({-1,0,0}),std::nextafter(1.,2.));
  EXPECT_TRUE(std::signbit(out->tree().getValue({1,0,0})));
  EXPECT_FALSE(std::signbit(out->tree().getValue({3,0,0})));
  EXPECT_EQ(out->tree().getValue({100000,0,0}),0.); EXPECT_FALSE(out->tree().isValueOn({100000,0,0}));
}
TEST_F(OpenVdbValueTopology, FileMetadataUnicodeParentsAndExclusiveOutputsSurvive) {
  auto grid=openvdb::FloatGrid::create(); grid->setName("source"); grid->tree().setValueOn({0,0,0},1.f);
  grid->insertMeta("exact",openvdb::Int64Metadata(INT64_MAX));
  openvdb::MetaMap fileMeta; fileMeta.insertMeta("author",openvdb::StringMetadata("retained"));
  { openvdb::io::File writer(path("source.vdb").string()); writer.setCompression(openvdb::io::COMPRESS_ZIP); writer.write(openvdb::GridPtrVec{grid},fileMeta); }
  const auto unicode=path("parent") / std::filesystem::path(u8"体積-é");
  ASSERT_TRUE(std::filesystem::create_directory(path("parent"))); ASSERT_TRUE(std::filesystem::create_directory(unicode));
  const auto input=unicode/std::filesystem::path(u8"入力.vdb"),output=unicode/std::filesystem::path(u8"出力.vdb");
  std::filesystem::copy_file(path("source.vdb"),input);
  (void)vdb::WriteValueTopology(input,output,Edit::PruneExact,{}, {},processing);
  std::filesystem::copy_file(output,path("readable.vdb"));
  auto out=native<openvdb::FloatGrid>("readable.vdb"); EXPECT_EQ(out->metaValue<int64_t>("exact"),INT64_MAX);
  {openvdb::io::File file(path("readable.vdb").string()); file.open(false); EXPECT_EQ(file.getMetadata()->metaValue<std::string>("author"),"retained");}
  EXPECT_THROW((void)vdb::WriteValueTopology(input,output,Edit::PruneExact),std::exception);
  EXPECT_THROW((void)vdb::WriteValueTopology(input,input,Edit::PruneExact),std::exception);
  EXPECT_EQ(std::filesystem::file_size(output),std::filesystem::file_size(path("readable.vdb")));
  std::filesystem::remove(output); std::filesystem::remove(input); std::filesystem::remove(unicode); std::filesystem::remove(path("parent"));
}
TEST_F(OpenVdbValueTopology, InvalidTypesSemanticsNumbersAndIrrelevantOptionsRejectBeforeOutput) {
  auto grid=openvdb::FloatGrid::create(); grid->setName("source"); grid->tree().setValueOn({0,0,0},1.f);
  const auto fail=[&](Edit op,const vdb::ValueTopologyOptions &options) {
    EXPECT_THROW((void)edit("in.vdb","failure.vdb",op,options),std::exception);
    EXPECT_FALSE(std::filesystem::exists(path("failure.vdb")));
  };
  write("in.vdb",{grid});
  for(const std::string invalid:{"nan","inf","1e-9999","1e999","1 ","+1","0x1"}) fail(Edit::ActivateValue,{.value={invalid}});
  fail(Edit::ActivateValue,{.value={"1","2","3"}}); fail(Edit::PruneExact,{.value={"1"}});
  fail(Edit::PruneExact,{.minimum={1,0,0}}); fail(Edit::ClipBox,{.fillActive=false});
  fail(Edit::FillBox,{.minimum={1,0,0},.maximum={0,0,0}});
  fail(Edit::FillBox,{.maximum={INT32_MAX,0,0}});
  fail(Edit::PruneExact,{.maxWorkItems=1});
  grid->setGridClass(openvdb::GRID_LEVEL_SET); write("in.vdb",{grid});
  fail(Edit::ActivateValue,{.value={"1"}});
  (void)edit("in.vdb","sdf-prune.vdb",Edit::PruneExact);
  EXPECT_EQ(native<openvdb::FloatGrid>("sdf-prune.vdb")->getGridClass(),openvdb::GRID_LEVEL_SET);
  auto mask=openvdb::MaskGrid::create(); mask->setName("mask"); write("in.vdb",{mask}); fail(Edit::PruneExact,{});
  auto point=openvdb::points::PointDataGrid::create(); point->setName("points"); write("in.vdb",{point}); fail(Edit::PruneExact,{});
}
TEST_F(OpenVdbValueTopology, TileActivationBoxExpansionAndWriterCapsHaveNoPublishedOutput) {
  auto grid=openvdb::FloatGrid::create(); grid->setName("source"); grid->tree().setValueOn({0,0,0},1.f);
  write("source.vdb",{grid});
  EXPECT_THROW((void)edit("source.vdb","activation-fail.vdb",Edit::ActivateValue),std::exception);
  vdb::ValueTopologyOptions options{.value={"1"},.minimum={-100,-100,-100},.maximum={100,100,100}};
  auto limits=processing; limits.maxVoxelCount=512;
  EXPECT_THROW((void)vdb::WriteValueTopology(path("source.vdb"),path("box-fail.vdb"),Edit::FillBox,options,{},limits),std::exception);
  limits=processing; limits.maxInputBytes=1024;
  EXPECT_THROW((void)vdb::WriteValueTopology(path("source.vdb"),path("byte-fail.vdb"),Edit::PruneExact,{}, {},limits),std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("activation-fail.vdb")));
  EXPECT_FALSE(std::filesystem::exists(path("box-fail.vdb"))); EXPECT_FALSE(std::filesystem::exists(path("byte-fail.vdb")));
  grid->tree().clear(); grid->tree().addTile(1,{0,0,0},1.f,true); write("tile.vdb",{grid});
  options.minimum={8,0,0}; options.maximum={15,7,7}; limits=processing; limits.maxVoxelCount=512;
  EXPECT_THROW((void)vdb::WriteValueTopology(path("tile.vdb"),path("sum-fail.vdb"),Edit::FillBox,options,{},limits),std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("sum-fail.vdb")));
  write("no-stats.vdb",{grid},false);
  EXPECT_THROW((void)edit("no-stats.vdb","stats-fail.vdb",Edit::PruneExact),std::exception);
}
TEST_F(OpenVdbValueTopology, DefaultSelectionSkipsMasksAndExplicitDuplicateDescriptorsWork) {
  auto mask=openvdb::MaskGrid::create(); mask->setName("a-mask");
  auto a=openvdb::Int64Grid::create(); a->setName("same"); a->tree().setValueOn({0,0,0},INT64_MAX);
  auto b=a->deepCopy(); b->tree().setValueOn({0,0,0},INT64_MAX-1);
  write("mixed.vdb",{mask,a,b});
  (void)edit("mixed.vdb","auto.vdb",Edit::PruneExact);
  EXPECT_EQ(native<openvdb::Int64Grid>("auto.vdb")->tree().getValue({0,0,0}),INT64_MAX);
  EXPECT_THROW((void)vdb::WriteValueTopology(path("mixed.vdb"),path("ambiguous.vdb"),Edit::PruneExact,{},"same",processing),std::exception);
  (void)vdb::WriteValueTopology(path("mixed.vdb"),path("selected.vdb"),Edit::PruneExact,{},"descriptor:same[1]",processing);
  EXPECT_EQ(native<openvdb::Int64Grid>("selected.vdb")->tree().getValue({0,0,0}),INT64_MAX-1);
}
using MaskEdit=vdb::MaskedValuesOperation;
class OpenVdbMaskedValues:public OpenVdbValueTopology {
protected:
  auto applyMask(const char *a,const char *b,const char *output,MaskEdit op) {
    return vdb::WriteMaskedValues(path(a),path(b),path(output),op,{},{},processing);
  }
  template<class Grid> void nativeMask(typename Grid::ValueType value,const char *prefix) {
    auto source=Grid::create(); source->setName("a");
    source->tree().setValueOn({0,0,0},value); source->tree().setValueOff({1,0,0},value);
    auto mask=openvdb::MaskGrid::create(); mask->setName("b"); mask->tree().setValueOn({1,0,0});
    const std::string a=std::string(prefix)+"-a.vdb",b=std::string(prefix)+"-b.vdb",out=std::string(prefix)+"-out.vdb";
    write(a,{source}); write(b,{mask});
    (void)applyMask(a.c_str(),b.c_str(),out.c_str(),MaskEdit::Activate);
    auto result=native<Grid>(out.c_str()); EXPECT_EQ(result->activeVoxelCount(),2u);
    EXPECT_EQ(result->tree().getValue({0,0,0}),value); EXPECT_EQ(result->tree().getValue({1,0,0}),value);
  }
};
TEST_F(OpenVdbMaskedValues, EightNativeSourceTypesRetainExactValuesWithMaskActivation) {
  nativeMask<openvdb::FloatGrid>(1.125f,"f"); nativeMask<openvdb::DoubleGrid>(1.125,"d");
  nativeMask<openvdb::BoolGrid>(false,"b"); nativeMask<openvdb::Int32Grid>(INT32_MIN,"i");
  nativeMask<openvdb::Int64Grid>(INT64_MAX,"l");
  nativeMask<openvdb::Vec3IGrid>({INT32_MIN,INT32_MAX,1},"vi");
  nativeMask<openvdb::Vec3SGrid>({1.125f,-2,3},"vf"); nativeMask<openvdb::Vec3DGrid>({1.125,-2,3},"vd");
}
TEST_F(OpenVdbMaskedValues, ActivityEditsPreserveInactiveBitsThatTopologyDifferenceWouldDiscard) {
  auto source=openvdb::DoubleGrid::create(0.); source->setName("a");
  source->tree().setValueOn({0,0,0},2.); source->tree().setValueOn({1,0,0},3.);
  source->tree().touchLeaf({2,0,0})->setValueOff({2,0,0},-0.);
  source->tree().setValueOff({100000,0,0},9.);
  auto mask=openvdb::FloatGrid::create(999.f); mask->setName("mask");
  mask->tree().setValueOn({1,0,0},0.f); mask->tree().setValueOn({2,0,0},std::numeric_limits<float>::quiet_NaN());
  mask->tree().setValueOff({0,0,0},-5.f);
  write("a.vdb",{source}); write("mask.vdb",{mask}); const auto before=bytes("a.vdb");
  for(const auto op:{MaskEdit::Activate,MaskEdit::Deactivate,MaskEdit::IntersectActivity}) {
    const auto name=std::to_string(static_cast<int>(op))+".vdb";
    (void)applyMask("a.vdb","mask.vdb",name.c_str(),op); auto out=native<openvdb::DoubleGrid>(name.c_str());
    EXPECT_EQ(out->tree().getValue({0,0,0}),2.); EXPECT_EQ(out->tree().getValue({1,0,0}),3.);
    EXPECT_TRUE(std::signbit(out->tree().getValue({2,0,0}))); EXPECT_EQ(out->tree().getValue({100000,0,0}),9.);
    EXPECT_EQ(out->tree().isValueOn({0,0,0}),op!=MaskEdit::IntersectActivity);
    EXPECT_EQ(out->tree().isValueOn({1,0,0}),op!=MaskEdit::Deactivate);
    EXPECT_EQ(out->tree().isValueOn({2,0,0}),op==MaskEdit::Activate);
  }
  EXPECT_EQ(bytes("a.vdb"),before);
}
TEST_F(OpenVdbMaskedValues, ClipInsideAndOutsideRetainSelectedNativeValuesAndResetOthers) {
  auto source=openvdb::DoubleGrid::create(0.); source->setName("a");
  source->tree().setValueOn({0,0,0},1.125); source->tree().touchLeaf({1,0,0})->setValueOff({1,0,0},-0.);
  source->tree().setValueOn({2,0,0},3.125);
  auto mask=openvdb::BoolGrid::create(true); mask->setName("mask");
  mask->tree().setValueOn({0,0,0},false); mask->tree().setValueOn({1,0,0},false);
  write("a.vdb",{source}); write("b.vdb",{mask});
  (void)applyMask("a.vdb","b.vdb","inside.vdb",MaskEdit::ClipInside);
  auto out=native<openvdb::DoubleGrid>("inside.vdb");
  EXPECT_EQ(out->activeVoxelCount(),1u); EXPECT_EQ(out->tree().getValue({0,0,0}),1.125);
  EXPECT_TRUE(std::signbit(out->tree().getValue({1,0,0}))); EXPECT_EQ(out->tree().getValue({2,0,0}),0.);
  (void)applyMask("a.vdb","b.vdb","outside.vdb",MaskEdit::ClipOutside);
  out=native<openvdb::DoubleGrid>("outside.vdb"); EXPECT_EQ(out->activeVoxelCount(),1u);
  EXPECT_EQ(out->tree().getValue({0,0,0}),0.); EXPECT_FALSE(std::signbit(out->tree().getValue({1,0,0})));
  EXPECT_EQ(out->tree().getValue({2,0,0}),3.125);
}
TEST_F(OpenVdbMaskedValues, CompactTilesSplitWithoutDiscardingInactiveValues) {
  auto source=openvdb::Int64Grid::create(INT64_MIN); source->setName("a");
  source->tree().addTile(1,{0,0,0},INT64_MAX,true);
  auto mask=openvdb::MaskGrid::create(); mask->setName("mask"); mask->tree().setValueOn({1,2,3});
  write("a.vdb",{source}); write("mask.vdb",{mask});
  (void)applyMask("a.vdb","mask.vdb","deactivated.vdb",MaskEdit::Deactivate);
  auto out=native<openvdb::Int64Grid>("deactivated.vdb");
  EXPECT_EQ(out->activeVoxelCount(),511u); EXPECT_FALSE(out->tree().isValueOn({1,2,3}));
  EXPECT_EQ(out->tree().getValue({1,2,3}),INT64_MAX); EXPECT_EQ(out->tree().getValue({0,0,0}),INT64_MAX);
  (void)applyMask("a.vdb","mask.vdb","clipped.vdb",MaskEdit::ClipInside);
  out=native<openvdb::Int64Grid>("clipped.vdb"); EXPECT_EQ(out->activeVoxelCount(),1u);
  EXPECT_EQ(out->tree().getValue({1,2,3}),INT64_MAX); EXPECT_EQ(out->tree().getValue({0,0,0}),INT64_MIN);
}
TEST_F(OpenVdbMaskedValues, ExactAffineAndNativeSemanticsAreEnforcedBeforeOutput) {
  auto source=openvdb::Vec3DGrid::create(); source->setName("a"); source->tree().setValueOn({-1,0,0},{1,2,3});
  source->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE); source->setIsInWorldSpace(false);
  source->transform().postTranslate({2,3,4});
  auto mask=openvdb::MaskGrid::create(); mask->setName("mask"); mask->tree().setValueOn({-1,0,0});
  mask->setTransform(source->transform().copy()); write("a.vdb",{source}); write("b.vdb",{mask});
  (void)applyMask("a.vdb","b.vdb","matched.vdb",MaskEdit::IntersectActivity);
  auto out=native<openvdb::Vec3DGrid>("matched.vdb"); EXPECT_EQ(out->transform(),source->transform());
  EXPECT_EQ(out->getVectorType(),source->getVectorType()); EXPECT_FALSE(out->isInWorldSpace());
  mask->transform().postTranslate({1e-12,0,0}); write("bad-map.vdb",{mask});
  EXPECT_THROW((void)applyMask("a.vdb","bad-map.vdb","bad.vdb",MaskEdit::Activate),std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  source->setGridClass(openvdb::GRID_STAGGERED); write("sdf.vdb",{source});
  EXPECT_THROW((void)applyMask("sdf.vdb","b.vdb","bad.vdb",MaskEdit::Activate),std::exception);
}
TEST_F(OpenVdbMaskedValues, CombinedCapsAndExclusivePublicationPreserveSources) {
  auto source=openvdb::FloatGrid::create(); source->setName("a"); source->tree().setValueOn({0,0,0},1.f);
  auto mask=openvdb::MaskGrid::create(); mask->setName("mask"); mask->tree().setValueOn({8,0,0});
  write("a.vdb",{source}); write("b.vdb",{mask});
  auto limits=processing; limits.maxVoxelCount=512;
  EXPECT_THROW((void)vdb::WriteMaskedValues(path("a.vdb"),path("b.vdb"),path("limited.vdb"),MaskEdit::Activate,{},{},limits),std::exception);
  EXPECT_THROW((void)vdb::WriteMaskedValues(path("a.vdb"),path("b.vdb"),path("work.vdb"),MaskEdit::Activate,{},{},processing,1),std::exception);
  const auto original=bytes("a.vdb");
  EXPECT_THROW((void)applyMask("a.vdb","b.vdb","a.vdb",MaskEdit::Activate),std::exception);
  EXPECT_EQ(bytes("a.vdb"),original); EXPECT_FALSE(std::filesystem::exists(path("limited.vdb")));
  EXPECT_FALSE(std::filesystem::exists(path("work.vdb")));
}
using SdfOp=vdb::SdfBackgroundOperation;
class OpenVdbSdfBackground:public OpenVdbValueTopology {
protected:
  template<class Grid> auto sphere() {
    auto source=openvdb::tools::createLevelSetSphere<Grid>(1.,openvdb::Vec3f(0),.1,3.);
    source->setName("sdf"); return source;
  }
  auto sdf(const char *input,const char *output,SdfOp op,const vdb::SdfBackgroundOptions &o={}) {
    return vdb::WriteSdfBackground(path(input),path(output),op,o,{},processing);
  }
  template<class Grid> void activeUnchanged(const Grid &a,const Grid &b) {
    EXPECT_EQ(a.activeVoxelCount(),b.activeVoxelCount());
    const auto originalMap=a.transform().baseMap()->getAffineMap()->getMat4();
    const auto updatedMap=b.transform().baseMap()->getAffineMap()->getMat4();
    for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) EXPECT_EQ(originalMap(r,c),updatedMap(r,c));
    for(auto it=a.cbeginValueOn();it;++it) {
      EXPECT_TRUE(b.tree().isValueOn(it.getCoord()));
      const auto original=std::bit_cast<std::array<std::byte,sizeof(typename Grid::ValueType)>>(typename Grid::ValueType(*it));
      const auto updated=std::bit_cast<std::array<std::byte,sizeof(typename Grid::ValueType)>>(b.tree().getValue(it.getCoord()));
      EXPECT_EQ(original,updated);
    }
  }
};
TEST_F(OpenVdbSdfBackground, FloatSymmetricBackgroundRetainsAllActiveBitsAndSigns) {
  auto source=sphere<openvdb::FloatGrid>(); write("source.vdb",{source});
  (void)sdf("source.vdb","out.vdb",SdfOp::BackgroundOnly,{.outsideWidth=.5});
  auto out=native<openvdb::FloatGrid>("out.vdb"); activeUnchanged(*source,*out);
  EXPECT_EQ(out->background(),.5f); EXPECT_EQ(out->getGridClass(),openvdb::GRID_LEVEL_SET);
  EXPECT_EQ(out->tree().getValue({0,0,0}),-.5f); EXPECT_EQ(out->tree().getValue({1000,0,0}),.5f);
  for(auto it=out->cbeginValueOff();it;++it) EXPECT_EQ(std::abs(*it),.5f);
}
TEST_F(OpenVdbSdfBackground, DoubleAsymmetricBackgroundNormalizesRootTilesAndKeepsMapAndMetadata) {
  auto source=sphere<openvdb::DoubleGrid>(); source->tree().addTile(3,{16384,0,0},-.3,false);
  source->transform().postTranslate({1e8,-3,4}); source->insertMeta("kept",openvdb::Int64Metadata(INT64_MAX));
  write("source.vdb",{source});
  (void)sdf("source.vdb","out.vdb",SdfOp::BackgroundOnly,{.outsideWidth=.5,.insideWidth=.7});
  auto out=native<openvdb::DoubleGrid>("out.vdb"); activeUnchanged(*source,*out);
  EXPECT_EQ(out->background(),.5); EXPECT_EQ(out->tree().getValue({16384,0,0}),-.7);
  EXPECT_EQ(out->metaValue<int64_t>("kept"),INT64_MAX);
  EXPECT_EQ(out->metaValue<double>("container:sdf_inside_width"),.7);
  EXPECT_EQ(out->metaValue<double>("container:sdf_outside_width"),.5);
}
TEST_F(OpenVdbSdfBackground, FloodFillPropagatesBandSignsWithoutChangingActiveDistances) {
  auto source=sphere<openvdb::FloatGrid>();
  ASSERT_FALSE(source->tree().isValueOn({3,3,3})); ASSERT_LT(source->tree().getValue({3,3,3}),0.f);
  source->tree().touchLeaf({3,3,3})->setValueOff({3,3,3},source->background()); write("source.vdb",{source});
  const auto report=sdf("source.vdb","out.vdb",SdfOp::FloodFill,{.outsideWidth=.4,.insideWidth=.5});
  auto out=native<openvdb::FloatGrid>("out.vdb"); activeUnchanged(*source,*out);
  EXPECT_EQ(out->tree().getValue({3,3,3}),-.5f); EXPECT_GT(out->tree().getValue({15,0,0}),0.f);
  EXPECT_EQ(out->tree().leafCount(),source->tree().leafCount()); EXPECT_FALSE(report.diagnostics.empty());
}
TEST_F(OpenVdbSdfBackground, DoubleCavityAndDisconnectedSurfacesKeepInteriorConventions) {
  auto source=sphere<openvdb::DoubleGrid>();
  auto hole=openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(.4,openvdb::Vec3f(0),.1,3.);
  openvdb::tools::csgDifference(*source,*hole);
  auto distant=openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(.4,openvdb::Vec3f(3,0,0),.1,3.);
  openvdb::tools::csgUnion(*source,*distant); write("source.vdb",{source});
  (void)sdf("source.vdb","out.vdb",SdfOp::FloodFill,{.outsideWidth=.4,.insideWidth=.5});
  auto out=native<openvdb::DoubleGrid>("out.vdb"); activeUnchanged(*source,*out);
  EXPECT_GT(out->tree().getValue({0,0,0}),0.); EXPECT_LT(out->tree().getValue({7,0,0}),0.);
  EXPECT_GT(out->tree().getValue({15,0,0}),0.); EXPECT_LT(out->tree().getValue({30,0,0}),0.);
}
TEST_F(OpenVdbSdfBackground, WidthMapTypeNonfiniteAndActiveTileErrorsNeverPublish) {
  auto source=sphere<openvdb::FloatGrid>(); write("source.vdb",{source});
  const auto reject=[&](const vdb::SdfBackgroundOptions &o={}) {
    EXPECT_THROW((void)sdf("source.vdb","bad.vdb",SdfOp::FloodFill,o),std::exception);
    EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  };
  reject({.outsideWidth=.01}); reject({.insideWidth=.01}); reject({.outsideWidth=0.});
  reject({.outsideWidth=std::numeric_limits<double>::infinity()}); reject({.outsideWidth=1e-300}); reject({.maxWorkItems=1});
  source->transform().postScale(openvdb::Vec3d(1,2,1)); write("source.vdb",{source}); reject();
  source=sphere<openvdb::FloatGrid>(); source->tree().addTile(1,{100,0,0},.1f,true); write("source.vdb",{source}); reject();
  source=sphere<openvdb::FloatGrid>(); source->tree().touchLeaf({3,3,3})->setValueOff({3,3,3},std::numeric_limits<float>::quiet_NaN());
  write("source.vdb",{source}); reject();
  source=sphere<openvdb::FloatGrid>(); source->setGridClass(openvdb::GRID_UNKNOWN); write("source.vdb",{source}); reject();
}
TEST_F(OpenVdbSdfBackground, RootDifferenceOverflowAndScanlineExpansionRejectBeforeNativeFill) {
  for(const int32_t span:{1'000'000'000,2'000'000'000}) {
    auto source=openvdb::FloatGrid::create(1.f); source->setGridClass(openvdb::GRID_LEVEL_SET);
    source->setTransform(openvdb::math::Transform::createLinearTransform(.1)); source->setName("sdf");
    source->tree().setValueOn({0,0,-span},-.1f); source->tree().setValueOn({0,0,span},.1f);
    write("source.vdb",{source});
    EXPECT_THROW((void)sdf("source.vdb","bad.vdb",SdfOp::FloodFill),std::exception);
    EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  }
}
TEST_F(OpenVdbSdfBackground, DefaultSelectionSkipsOrdinaryGridsAndExclusiveInputsSurvive) {
  auto ordinary=openvdb::FloatGrid::create(); ordinary->setName("a-ordinary");
  auto source=sphere<openvdb::DoubleGrid>(); write("source.vdb",{ordinary,source}); const auto original=bytes("source.vdb");
  (void)sdf("source.vdb","out.vdb",SdfOp::BackgroundOnly);
  EXPECT_EQ(native<openvdb::DoubleGrid>("out.vdb")->getGridClass(),openvdb::GRID_LEVEL_SET);
  EXPECT_THROW((void)vdb::WriteSdfBackground(path("source.vdb"),path("bad.vdb"),SdfOp::BackgroundOnly,{},"a-ordinary",processing),std::exception);
  EXPECT_THROW((void)sdf("source.vdb","source.vdb",SdfOp::FloodFill),std::exception);
  EXPECT_THROW((void)sdf("source.vdb","out.vdb",SdfOp::FloodFill),std::exception);
  EXPECT_EQ(bytes("source.vdb"),original);
}
TEST_F(OpenVdbValueTopology, AllTopologyCopiesRetainDistantScaleAndSubToleranceShearExactly) {
  for(const int mapCase:{0,2}) {
    auto source=openvdb::DoubleGrid::create(-0.); source->setName("source"); source->tree().setValueOn({0,0,0},1.);
    if(mapCase==2) {
      auto matrix=openvdb::math::Mat4d::identity(); matrix[1][0]=std::ldexp(1.,-60); matrix[3][0]=1e8;
      // Independent SDK record bypasses constructor simplification in this
      // external fixture; production transform helpers are not used here.
      std::stringstream encoded(std::ios::in|std::ios::out|std::ios::binary);
      openvdb::writeString(encoded,openvdb::math::AffineMap::mapType()); openvdb::math::AffineMap(matrix).write(encoded);
      encoded.seekg(0); openvdb::io::setCurrentVersion(encoded);
      auto transform=std::make_shared<openvdb::math::Transform>(); transform->read(encoded); source->setTransform(transform);
    } else {
      source->setTransform(openvdb::math::Transform::createLinearTransform(.1)); source->transform().postTranslate({1e8,0,0});
    }
    const auto expected=source->transform().baseMap()->getAffineMap()->getMat4();
    auto selection=openvdb::MaskGrid::create(); selection->setName("selection"); selection->tree().setValueOn({0,0,0});
    selection->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));
    write("source.vdb",{source}); write("mask.vdb",{selection});
    for(int op=0;op<3;++op) {
      const auto name=std::to_string(mapCase)+"-"+std::to_string(op)+".vdb";
      if(op==0) (void)vdb::WriteTopologyMask(path("source.vdb"),path(name),Operation::ActiveMask,{}, {},processing);
      if(op==1) (void)edit("source.vdb",name.c_str(),Edit::PruneExact);
      if(op==2) (void)vdb::WriteMaskedValues(path("source.vdb"),path("mask.vdb"),path(name),MaskEdit::ClipInside,{},{},processing);
      openvdb::io::File file(path(name).string()); file.open(false);
      const auto result=file.readGrid(processing.gridName);
      const auto actual=result->transform().baseMap()->getAffineMap()->getMat4();
      for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) EXPECT_EQ(actual(r,c),expected(r,c));
    }
  }
}
#endif
} // namespace
