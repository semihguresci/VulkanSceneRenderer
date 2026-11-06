#include "Container/geometry/OpenVdbPoisson.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbPoissonAvailability, DisabledBuildReportsFeatureBeforeFilesystemAccess) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  try {
    (void)vdb::WritePoissonSolution("missing-poisson.vdb", "unwritten-poisson.vdb", {}, {.maxIterations = 0});
    FAIL() << "disabled Poisson must fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbPoisson : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-poisson-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    processing.gridName = "solution";
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  static void write(const std::filesystem::path& path, const openvdb::GridPtrVec& grids, bool stats = true) {
    openvdb::io::File output(path.string());
    output.setGridStatsMetadataEnabled(stats);
    output.write(grids);
    output.close();
  }
  template<class Grid> typename Grid::Ptr read(const char* name) const {
    openvdb::io::File input(file(name).string());
    input.open(false);
    return openvdb::gridPtrCast<Grid>(input.readGrid("solution"));
  }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  }
  static openvdb::DoubleGrid::Ptr manufactured(int size, double voxel = 1.0) {
    auto grid = openvdb::DoubleGrid::create(99.0);
    grid->setName("rhs");
    grid->setTransform(openvdb::math::Transform::createLinearTransform(voxel));
    grid->insertMeta("artist", openvdb::StringMetadata("manufactured original"));
    for (int x = 0; x < size; ++x)
      for (int y = 0; y < size; ++y)
        for (int z = 0; z < size; ++z) {
          const double center = exact(x,y,z,size);
          const double laplacian = (exact(x-1,y,z,size) + exact(x+1,y,z,size) +
              exact(x,y-1,z,size) + exact(x,y+1,z,size) + exact(x,y,z-1,size) +
              exact(x,y,z+1,size) - 6 * center) / (voxel * voxel);
          grid->tree().setValue(openvdb::Coord(x,y,z), laplacian);
        }
    return grid;
  }
  static double exact(int x, int y, int z, int size) {
    if (x < 0 || y < 0 || z < 0 || x >= size || y >= size || z >= size) return 0;
    return double((x+1)*(size-x)*(y+1)*(size-y)*(z+1)*(size-z));
  }
  template<class Grid> static double relativeResidual(const Grid& output, const Grid& rhs, double voxel) {
    double maximum = 0, norm = 0;
    const auto accessor = output.getConstAccessor();
    for (auto value = rhs.cbeginValueOn(); value; ++value) {
      EXPECT_TRUE(value.isVoxelValue());
      const auto c = value.getCoord();
      const double laplacian = (double(accessor.getValue(c.offsetBy(-1,0,0))) + accessor.getValue(c.offsetBy(1,0,0)) +
          accessor.getValue(c.offsetBy(0,-1,0)) + accessor.getValue(c.offsetBy(0,1,0)) +
          accessor.getValue(c.offsetBy(0,0,-1)) + accessor.getValue(c.offsetBy(0,0,1)) -
          6 * double(accessor.getValue(c))) / (voxel * voxel);
      maximum = std::max(maximum, std::abs(laplacian - double(*value)));
      norm = std::max(norm, std::abs(double(*value)));
    }
    return norm == 0 ? maximum : maximum / norm;
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbPoisson, ManufacturedDiscretePolynomialUsesWorldVoxelSizeAndRetainsMetadata) {
  auto source = manufactured(5, .25);
  source->transform().postRotate(.6, openvdb::math::Z_AXIS);
  source->transform().postScale(openvdb::Vec3d(-1,1,1));
  source->transform().postTranslate(openvdb::Vec3d(7,-4,2));
  write(file("rhs.vdb"), {source});
  const auto before = bytes(file("rhs.vdb"));
  const auto result = vdb::WritePoissonSolution(file("rhs.vdb"), file("solution.vdb"), "rhs",
      {.tolerance = 1e-10}, processing);
  EXPECT_TRUE(result.converged);
  EXPECT_LE(result.iterations, 200u);
  EXPECT_LE(result.residual, 1e-10);
  EXPECT_EQ(result.grid.activeVoxelCount, 125u);
  const auto output = read<openvdb::DoubleGrid>("solution.vdb");
  EXPECT_EQ(output->transform(), source->transform());
  EXPECT_EQ(output->metaValue<std::string>("artist"), "manufactured original");
  EXPECT_EQ(output->getGridClass(), openvdb::GRID_UNKNOWN);
  EXPECT_EQ(output->background(), 0.0);
  for (int x = 0; x < 5; ++x)
    for (int y = 0; y < 5; ++y)
      for (int z = 0; z < 5; ++z)
        EXPECT_NEAR(output->tree().getValue(openvdb::Coord(x,y,z)), exact(x,y,z,5), 1e-6);
  EXPECT_LT(relativeResidual(*output, *source, .25), 1e-9);
  EXPECT_NEAR(result.residual, relativeResidual(*output, *source, .25), 1e-12);
  EXPECT_EQ(bytes(file("rhs.vdb")), before);
}

TEST_F(OpenVdbPoisson, FloatInputUsesNearestNeighborStencilAndIgnoresInactiveRhs) {
  auto source = openvdb::FloatGrid::create(1000.0f);
  source->setName("rhs");
  source->setTransform(openvdb::math::Transform::createLinearTransform(.5));
  source->tree().setValue(openvdb::Coord(0), 6.0f);
  source->tree().setValueOff(openvdb::Coord(1,0,0), -1e6f);
  source->tree().setValue(openvdb::Coord(10,0,0), 12.0f);
  write(file("rhs.vdb"), {source});
  (void)vdb::WritePoissonSolution(file("rhs.vdb"), file("float.vdb"), {}, {}, processing);
  const auto result = read<openvdb::FloatGrid>("float.vdb");
  EXPECT_FLOAT_EQ(result->tree().getValue(openvdb::Coord(0)), -.25f);
  EXPECT_FLOAT_EQ(result->tree().getValue(openvdb::Coord(10,0,0)), -.5f);
  EXPECT_FLOAT_EQ(result->tree().getValue(openvdb::Coord(1,0,0)), 0.0f);
  EXPECT_EQ(result->activeVoxelCount(), 2u);
}

TEST_F(OpenVdbPoisson, IrregularDisconnectedDomainMatchesIndependentDiscreteSolution) {
  auto known = openvdb::DoubleGrid::create(0.0);
  for (const auto c : {openvdb::Coord(-3,0,0), {-2,0,0}, {-2,1,0}, {9,4,-2}, {9,5,-2}})
    known->tree().setValue(c, double(c.x() + c.y() + 20));
  auto rhs = openvdb::DoubleGrid::create(17.0);
  for (auto value = known->cbeginValueOn(); value; ++value) {
    const auto c = value.getCoord();
    double laplacian = -6 * *value;
    for (const auto offset : {openvdb::Coord(-1,0,0), {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}})
      laplacian += known->tree().getValue(c + offset);
    rhs->tree().setValue(c, laplacian);
  }
  write(file("irregular.vdb"), {rhs});
  (void)vdb::WritePoissonSolution(file("irregular.vdb"), file("irregular-out.vdb"), {}, {.tolerance = 1e-11}, processing);
  const auto result = read<openvdb::DoubleGrid>("irregular-out.vdb");
  for (auto value = known->cbeginValueOn(); value; ++value)
    EXPECT_NEAR(result->tree().getValue(value.getCoord()), *value, 1e-8);
  EXPECT_LT(relativeResidual(*result, *rhs, 1.0), 1e-10);
}

TEST_F(OpenVdbPoisson, ActiveTilesExpandWithinBudgetAndZeroOrEmptyRhsNeedsNoIterations) {
  auto tiled = openvdb::DoubleGrid::create(0.0);
  tiled->tree().addTile(1, openvdb::Coord(0), 6.0, true);
  write(file("tile.vdb"), {tiled});
  (void)vdb::WritePoissonSolution(file("tile.vdb"), file("tile-out.vdb"), {}, {.tolerance = 1e-9}, processing);
  const auto output = read<openvdb::DoubleGrid>("tile-out.vdb");
  EXPECT_EQ(output->activeVoxelCount(), 512u);
  EXPECT_LT(output->tree().getValue(openvdb::Coord(3)), 0.0);
  tiled->tree().voxelizeActiveTiles();
  EXPECT_LT(relativeResidual(*output, *tiled, 1.0), 1e-8);
  for (int variant = 0; variant < 2; ++variant) {
    auto zero = openvdb::FloatGrid::create(9.0f);
    if (variant) zero->tree().addTile(1, openvdb::Coord(0), 0.0f, true);
    write(file("zero.vdb"), {zero});
    const auto path = file(variant ? "zero-tile-out.vdb" : "empty-out.vdb");
    const auto result = vdb::WritePoissonSolution(file("zero.vdb"), path, {}, {}, processing);
    EXPECT_TRUE(result.converged);
    EXPECT_EQ(result.iterations, 0u);
    EXPECT_EQ(result.residual, 0.0);
    EXPECT_EQ(result.grid.activeVoxelCount, variant ? 512u : 0u);
  }
}

TEST_F(OpenVdbPoisson, PeakNormalizationAndExponentScalingPreserveTinyAndLargeFiniteSolutions) {
  for (double value : {1e-300, std::numeric_limits<double>::max() / 2}) {
    auto source = openvdb::DoubleGrid::create(0.0);
    source->setTransform(openvdb::math::Transform::createLinearTransform(2.0));
    source->tree().setValue(openvdb::Coord(0), value);
    write(file("scaled.vdb"), {source});
    const char* name = value < 1 ? "tiny.vdb" : "large.vdb";
    (void)vdb::WritePoissonSolution(file("scaled.vdb"), file(name), {}, {}, processing);
    const auto output = read<openvdb::DoubleGrid>(name);
    EXPECT_NEAR(output->tree().getValue(openvdb::Coord(0)) / value, -2.0/3.0, 1e-12);
  }
}

TEST_F(OpenVdbPoisson, IterationCapFailureWritesNothingAndDoesNotModifySource) {
  const auto source = manufactured(9);
  write(file("hard.vdb"), {source});
  const auto before = bytes(file("hard.vdb"));
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("hard.vdb"), file("failed.vdb"), {},
      {.maxIterations = 1, .tolerance = 1e-12}, processing), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(file("failed.vdb")));
  EXPECT_EQ(bytes(file("hard.vdb")), before);
}

TEST_F(OpenVdbPoisson, NormalizationCannotSilentlyEraseRepresentableDisconnectedRhsValues) {
  auto source = openvdb::DoubleGrid::create(0.0);
  source->tree().setValue(openvdb::Coord(0), 1e200);
  source->tree().setValue(openvdb::Coord(32,0,0), 1e-200);
  write(file("dynamic-range.vdb"), {source});
  const auto before = bytes(file("dynamic-range.vdb"));
  try {
    (void)vdb::WritePoissonSolution(file("dynamic-range.vdb"), file("erased.vdb"), {}, {}, processing);
    FAIL() << "a nonzero RHS must not vanish during normalization";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("dynamic range"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("erased.vdb")));
  EXPECT_EQ(bytes(file("dynamic-range.vdb")), before);
}

TEST_F(OpenVdbPoisson, BudgetsOptionsAndCoordinateLimitsFailBeforeOutput) {
  const auto source = manufactured(4);
  write(file("rhs.vdb"), {source});
  for (const auto options : {vdb::PoissonOptions{.maxIterations = 0}, {.maxIterations = 10001},
      {.tolerance = NAN}, {.tolerance = 1e-13}, {.tolerance = .1}, {.maxSolverVoxels = 0},
      {.maxSolverVoxels = 1'000'001}, {.maxSolverVoxels = 63}})
    EXPECT_THROW((void)vdb::WritePoissonSolution(file("rhs.vdb"), file("bad.vdb"), {}, options, processing),
                 std::invalid_argument);
  auto small = processing;
  small.maxInputBytes = source->memUsage() + std::filesystem::file_size(file("rhs.vdb")) + 4096;
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("rhs.vdb"), file("bad.vdb"), {}, {}, small), std::invalid_argument);
  small = processing;
  small.maxVoxelCount = 63;
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("rhs.vdb"), file("bad.vdb"), {}, {}, small), std::invalid_argument);
  auto edge = openvdb::DoubleGrid::create(0.0);
  edge->tree().setValue(openvdb::Coord(std::numeric_limits<int32_t>::max()-1,0,0), 1.0);
  write(file("edge.vdb"), {edge});
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("edge.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  for (const int32_t coordinate : {std::numeric_limits<int32_t>::min() + 2, std::numeric_limits<int32_t>::max() - 2}) {
    edge->tree().clear();
    edge->tree().setValue(openvdb::Coord(0,0,coordinate), 1.0);
    write(file("leaf-edge.vdb"), {edge});
    EXPECT_THROW((void)vdb::WritePoissonSolution(file("leaf-edge.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  }
  auto tileAndLeaf = openvdb::FloatGrid::create(0.0f);
  tileAndLeaf->tree().addTile(1, openvdb::Coord(0), 1.0f, true);
  tileAndLeaf->tree().setValueOff(openvdb::Coord(32,0,0), 5.0f);
  write(file("expansion.vdb"), {tileAndLeaf});
  small = processing;
  small.maxVoxelCount = 512;
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("expansion.vdb"), file("bad.vdb"), {}, {}, small), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbPoisson, RequestedResidualMustAlsoHoldAfterFloatOutputRounding) {
  auto source = openvdb::FloatGrid::create(0.0f);
  source->tree().setValue(openvdb::Coord(0), 1.0f);
  write(file("float-rounding.vdb"), {source});
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("float-rounding.vdb"), file("too-tight.vdb"), {},
      {.tolerance = 1e-12}, processing), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(file("too-tight.vdb")));
  const auto result = vdb::WritePoissonSolution(file("float-rounding.vdb"), file("representable.vdb"), {}, {}, processing);
  EXPECT_GT(result.residual, 0.0);
  EXPECT_LE(result.residual, 1e-6);
}

TEST_F(OpenVdbPoisson, UnsupportedTransformsNonfiniteDataAndUnrepresentableOutputsFail) {
  auto grid = manufactured(2);
  grid->transform().postScale(openvdb::Vec3d(1,2,1));
  write(file("anisotropic.vdb"), {grid});
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("anisotropic.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  for (int variant = 0; variant < 3; ++variant) {
    grid = manufactured(2);
    if (variant == 0) grid = openvdb::DoubleGrid::create(INFINITY);
    if (variant == 1) grid->tree().setValue(openvdb::Coord(0), NAN);
    if (variant == 2) grid->tree().setValueOff(openvdb::Coord(9,0,0), NAN);
    write(file("nonfinite.vdb"), {grid});
    EXPECT_THROW((void)vdb::WritePoissonSolution(file("nonfinite.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  }
  grid = openvdb::DoubleGrid::create(0.0);
  grid->setTransform(openvdb::math::Transform::createLinearTransform(1e6));
  grid->tree().setValue(openvdb::Coord(0), std::numeric_limits<double>::max());
  write(file("overflow.vdb"), {grid});
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("overflow.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbPoisson, SelectionStreamingMetadataGuardsAndExclusiveOutputAreEnforced) {
  auto a = manufactured(2), b = manufactured(3);
  a->setName("duplicate"); b->setName("duplicate");
  write(file("duplicates.vdb"), {a,b});
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("duplicates.vdb"), file("bad.vdb"), "duplicate", {}, processing),
               std::invalid_argument);
  openvdb::io::File input(file("duplicates.vdb").string()); input.open(false);
  const auto descriptor = input.beginName().gridName(); input.close();
  EXPECT_NO_THROW((void)vdb::WritePoissonSolution(file("duplicates.vdb"), file("selected.vdb"),
      "descriptor:" + descriptor, {}, processing));
  b->setName("duplicate[0]");
  write(file("collision.vdb"), {a, a->deepCopy(), b});
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("collision.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  write(file("no-metadata.vdb"), {manufactured(2)}, false);
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("no-metadata.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  {
    std::ofstream stream(file("stream.vdb"), std::ios::binary);
    openvdb::io::Stream output(stream); output.write(openvdb::GridPtrVec{a});
  }
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("stream.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  auto wind = openvdb::Vec3DGrid::create(openvdb::Vec3d(1));
  write(file("vector.vdb"), {wind});
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("vector.vdb"), file("bad.vdb"), {}, {}, processing), std::invalid_argument);
  std::ofstream(file("protected.vdb")) << "keep me";
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("duplicates.vdb"), file("protected.vdb"), {}, {}, processing), std::invalid_argument);
  EXPECT_EQ(bytes(file("protected.vdb")), "keep me");
  EXPECT_THROW((void)vdb::WritePoissonSolution(file("duplicates.vdb"), file("duplicates.vdb"), {}, {}, processing), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}
#endif
} // namespace
