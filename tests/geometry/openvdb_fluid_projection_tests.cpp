#include "Container/geometry/OpenVdbFluidProjection.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/LevelSetUtil.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbFluidProjectionAvailability, DisabledBuildReportsFeatureBeforeFilesystemOrOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  vdb::FluidProjectionOptions projection;
  projection.timeStep = 0;
  projection.maxIterations = 0;
  try {
    (void)vdb::WriteProjectedVelocity("missing-velocity.vdb", "missing-domain.vdb", {}, projection);
    FAIL() << "disabled projection must fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFluidProjection : public ::testing::Test {
protected:
  using Cells = std::vector<openvdb::Coord>;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-projection-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  void write(const char* name, const openvdb::GridPtrVec& grids, bool stats = true) const {
    openvdb::io::File output(file(name).string());
    output.setGridStatsMetadataEnabled(stats);
    output.write(grids);
    output.close();
  }
  template<class Grid> typename Grid::Ptr read(const char* name, const std::string& gridName) const {
    openvdb::io::File input(file(name).string());
    input.open(false);
    auto output = openvdb::gridPtrCast<Grid>(input.readGrid(gridName));
    input.close();
    return output;
  }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  }
  static Cells box(int nx, int ny, int nz) {
    Cells result;
    for (int x = 0; x < nx; ++x)
      for (int y = 0; y < ny; ++y)
        for (int z = 0; z < nz; ++z) result.emplace_back(x,y,z);
    return result;
  }
  static openvdb::Coord offset(size_t axis, int amount = 1) {
    openvdb::Coord result(0);
    result[axis] = amount;
    return result;
  }
  static openvdb::math::Transform::Ptr transform(double h, bool reflected = false) {
    auto result = openvdb::math::Transform::createLinearTransform(h);
    result->postRotate(.37, openvdb::math::Z_AXIS);
    if (reflected) result->postScale(openvdb::Vec3d(-1,1,1));
    result->postTranslate(openvdb::Vec3d(.5,-.5,.25));
    return result;
  }
  static openvdb::BoolGrid::Ptr domain(const Cells& cells, double h = 1.0, bool reflected = false) {
    auto grid = openvdb::BoolGrid::create(false);
    grid->setName("mask");
    grid->setTransform(transform(h, reflected));
    grid->insertMeta("artist", openvdb::StringMetadata("original domain"));
    for (const auto& c : cells) grid->tree().setValue(c, true);
    return grid;
  }
  static openvdb::Vec3DGrid::Ptr mac(const openvdb::GridBase& mask) {
    auto grid = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
    grid->setName("wind");
    grid->setTransform(mask.transform().copy());
    grid->setGridClass(openvdb::GRID_STAGGERED);
    grid->setIsInWorldSpace(false);
    grid->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    grid->insertMeta("artist", openvdb::StringMetadata("original velocity"));
    return grid;
  }
  static void face(openvdb::Vec3DGrid& grid, const openvdb::Coord& c, size_t axis, double value) {
    auto vector = grid.tree().getValue(c);
    vector[axis] = value;
    grid.tree().setValue(c, vector);
  }
  static double divergence(const openvdb::Vec3DGrid& grid, const openvdb::Coord& c) {
    double result = 0;
    for (size_t axis = 0; axis < 3; ++axis)
      result += grid.tree().getValue(c + offset(axis))[axis] - grid.tree().getValue(c)[axis];
    return result;
  }
  static double maximumDivergence(const openvdb::Vec3DGrid& grid, const Cells& cells) {
    double result = 0;
    for (const auto& c : cells) result = std::max(result, std::abs(divergence(grid, c)));
    return result;
  }
  template<class Domain> static void expectWallsClosed(const openvdb::Vec3DGrid& grid, const Domain& mask, const Cells& cells) {
    for (const auto& c : cells) for (size_t axis = 0; axis < 3; ++axis) {
      if (!mask.tree().getValue(c - offset(axis)))
        EXPECT_DOUBLE_EQ(grid.tree().getValue(c)[axis], 0.0);
      if (!mask.tree().getValue(c + offset(axis)))
        EXPECT_DOUBLE_EQ(grid.tree().getValue(c + offset(axis))[axis], 0.0);
    }
  }
  static void expectZeroFaces(const openvdb::Vec3DGrid& grid, const Cells& cells, double tolerance) {
    for (const auto& c : cells) for (size_t axis = 0; axis < 3; ++axis) {
      EXPECT_NEAR(grid.tree().getValue(c)[axis], 0, tolerance);
      EXPECT_NEAR(grid.tree().getValue(c + offset(axis))[axis], 0, tolerance);
    }
  }
  template<class Function>
  static openvdb::Vec3DGrid::Ptr gradientVelocity(const openvdb::BoolGrid& mask, const Cells& cells,
      double dt, double h, Function physicalPressure) {
    auto result = mac(mask);
    for (const auto& c : cells) for (size_t axis = 0; axis < 3; ++axis) {
      const auto previous = c - offset(axis);
      if (mask.tree().getValue(previous))
        face(*result, c, axis, dt / (h*h) * (physicalPressure(c) - physicalPressure(previous)));
    }
    return result;
  }
  std::filesystem::path directory;
};

TEST_F(OpenVdbFluidProjection, ManufacturedClosedMacGradientDropsDivergenceAndRecoversPhysicalPressure) {
  const auto cells = box(5,4,3);
  constexpr double h = .25, dt = .125;
  const auto mask = domain(cells, h, true);
  const auto potential = [](const openvdb::Coord& c) {
    return .05 * (c.x()*c.x() + 2*c.y()*c.y() + 3*c.z()*c.z() + .25*c.x()*c.y());
  };
  const auto velocity = gradientVelocity(*mask, cells, dt, h, potential);
  velocity->insertMeta("container:mac_layout", openvdb::StringMetadata("stale layout"));
  velocity->insertMeta("container:pressure_units", openvdb::StringMetadata("stale scalar units"));
  velocity->insertMeta("container:projection_dt", openvdb::StringMetadata("stale type"));
  mask->insertMeta("container:velocity_units", openvdb::StringMetadata("stale vector units"));
  mask->insertMeta("container:pressure_units", openvdb::StringMetadata("stale scalar units"));
  write("velocity.vdb", {velocity});
  write("domain.vdb", {mask});
  const auto originalVelocity = bytes(file("velocity.vdb")), originalDomain = bytes(file("domain.vdb"));
  vdb::FluidProjectionOptions options;
  options.timeStep = dt;
  options.tolerance = 1e-10;
  const auto result = vdb::WriteProjectedVelocity(file("velocity.vdb"), file("domain.vdb"), file("projected.vdb"), options);
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(result.fluidCellCount, cells.size());
  EXPECT_EQ(result.faceCount, 133u);
  EXPECT_EQ(result.componentCount, 1u);
  EXPECT_GT(result.iterations, 0u);
  EXPECT_NEAR(result.maxDivergenceBefore, maximumDivergence(*velocity, cells), 1e-12);
  EXPECT_LE(result.residual, options.tolerance);
  const auto output = read<openvdb::Vec3DGrid>("projected.vdb", "velocity");
  const auto pressure = read<openvdb::DoubleGrid>("projected.vdb", "pressure");
  const auto savedMask = read<openvdb::BoolGrid>("projected.vdb", "fluid");
  ASSERT_TRUE(output); ASSERT_TRUE(pressure); ASSERT_TRUE(savedMask);
  EXPECT_EQ(output->transform(), mask->transform());
  EXPECT_EQ(pressure->transform(), mask->transform());
  EXPECT_EQ(output->getGridClass(), openvdb::GRID_STAGGERED);
  EXPECT_FALSE(output->isInWorldSpace());
  EXPECT_EQ(output->getVectorType(), openvdb::VEC_CONTRAVARIANT_RELATIVE);
  EXPECT_EQ(output->metaValue<std::string>("artist"), "original velocity");
  EXPECT_EQ(pressure->metaValue<std::string>("artist"), "original domain");
  EXPECT_EQ(savedMask->metaValue<std::string>("artist"), "original domain");
  EXPECT_EQ(output->metaValue<std::string>("container:mac_layout"), "negative_faces");
  EXPECT_EQ(output->metaValue<std::string>("container:velocity_units"), "voxels_per_second");
  EXPECT_EQ(pressure->metaValue<std::string>("container:pressure_gauge"), "one_zero_cell_per_component");
  EXPECT_EQ(pressure->metaValue<std::string>("container:pressure_units"), "world_distance_squared_per_second_squared");
  EXPECT_DOUBLE_EQ(output->metaValue<double>("container:projection_dt"), dt);
  EXPECT_FALSE(output->getMetadata<openvdb::StringMetadata>("container:pressure_units"));
  EXPECT_FALSE(pressure->getMetadata<openvdb::StringMetadata>("container:velocity_units"));
  EXPECT_FALSE(savedMask->getMetadata<openvdb::StringMetadata>("container:velocity_units"));
  EXPECT_FALSE(savedMask->getMetadata<openvdb::StringMetadata>("container:pressure_units"));
  EXPECT_EQ(pressure->activeVoxelCount(), cells.size());
  EXPECT_EQ(savedMask->activeVoxelCount(), cells.size());
  const double gauge = pressure->tree().getValue(cells.front()) - potential(cells.front());
  for (const auto& c : cells) EXPECT_NEAR(pressure->tree().getValue(c) - gauge, potential(c), 1e-8);
  expectZeroFaces(*output, cells, 1e-8);
  expectWallsClosed(*output, *mask, cells);
  EXPECT_NEAR(result.maxDivergenceAfter, maximumDivergence(*output, cells), 1e-13);
  EXPECT_LE(maximumDivergence(*output, cells), options.tolerance * result.maxDivergenceBefore);
  EXPECT_TRUE(output->getMetadata<openvdb::Int64Metadata>("file_mem_bytes"));
  EXPECT_TRUE(pressure->getMetadata<openvdb::Int64Metadata>("file_voxel_count"));
  EXPECT_EQ(bytes(file("velocity.vdb")), originalVelocity);
  EXPECT_EQ(bytes(file("domain.vdb")), originalDomain);
}

TEST_F(OpenVdbFluidProjection, SphereFogDomainConvergesForConstantWorldVelocityAndCanBeReprojected) {
  auto fog = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(
      1.0f, openvdb::Vec3f(0), .25f, 3.0f);
  openvdb::tools::sdfToFogVolume(*fog);
  fog->setName("density");
  auto wind = openvdb::Vec3DGrid::create(openvdb::Vec3d(1, .5, -.25));
  wind->setName("velocity");
  wind->setIsInWorldSpace(true);
  wind->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  write("sphere-fog.vdb", {fog});
  write("world-wind.vdb", {wind});
  vdb::FluidProjectionOptions options;
  options.timeStep = .1;
  const auto result = vdb::WriteProjectedVelocity(
      file("world-wind.vdb"), file("sphere-fog.vdb"), file("projected.vdb"),
      options, "velocity", "density");
  EXPECT_TRUE(result.converged);
  EXPECT_GT(result.fluidCellCount, 100u);
  EXPECT_GT(result.maxDivergenceBefore, 0);
  EXPECT_LE(result.maxDivergenceAfter, options.tolerance * result.maxDivergenceBefore);
  EXPECT_LE(result.residual, .5 * options.tolerance);
  const auto again = vdb::WriteProjectedVelocity(
      file("projected.vdb"), file("projected.vdb"), file("again.vdb"),
      options, "velocity", "fluid");
  EXPECT_TRUE(again.converged);
  EXPECT_LE(again.maxDivergenceAfter, options.tolerance * again.maxDivergenceBefore);
}

TEST_F(OpenVdbFluidProjection, IrregularSphericalDomainRecoversAnalyticLinearPressureAtDefaultSolverLimits) {
  Cells cells;
  for (int x=-4;x<=4;++x) for(int y=-4;y<=4;++y) for(int z=-4;z<=4;++z)
    if(x*x+y*y+z*z<16) cells.emplace_back(x,y,z);
  auto mask = openvdb::BoolGrid::create(false);
  mask->setName("spherical domain");
  mask->setTransform(openvdb::math::Transform::createLinearTransform(.25));
  for(const auto& c:cells) mask->tree().setValue(c,true);
  const openvdb::Vec3d wind(1,.5,-.25);
  auto velocity = openvdb::Vec3DGrid::create(wind);
  velocity->setName("uniform world"); velocity->setIsInWorldSpace(true);
  velocity->setVectorType(openvdb::VEC_INVARIANT);
  velocity->tree().setValue({0,0,0},wind);
  write("uniform.vdb",{velocity});write("sphere-domain.vdb",{mask});
  const vdb::FluidProjectionOptions options;
  const auto result=vdb::WriteProjectedVelocity(file("uniform.vdb"),file("sphere-domain.vdb"),file("sphere-out.vdb"),options);
  EXPECT_TRUE(result.converged);EXPECT_LE(result.iterations,options.maxIterations);
  EXPECT_EQ(result.fluidCellCount,cells.size());EXPECT_EQ(result.componentCount,1u);
  EXPECT_LE(result.residual,options.tolerance);
  const auto output=read<openvdb::Vec3DGrid>("sphere-out.vdb","velocity");
  const auto pressure=read<openvdb::DoubleGrid>("sphere-out.vdb","pressure");
  ASSERT_TRUE(output);ASSERT_TRUE(pressure);
  expectWallsClosed(*output,*mask,cells);
  expectZeroFaces(*output,cells,1e-5);
  EXPECT_LE(maximumDivergence(*output,cells),options.tolerance*result.maxDivergenceBefore);
  const auto anchor=cells.front();
  for(const auto& c:cells) {
    const auto displacement=mask->indexToWorld(c.asVec3d())-mask->indexToWorld(anchor.asVec3d());
    EXPECT_NEAR(pressure->tree().getValue(c)-pressure->tree().getValue(anchor),wind.dot(displacement)/options.timeStep,1e-4);
  }
}

TEST_F(OpenVdbFluidProjection, SolenoidalCirculationSurvivesAndExteriorFaceNoiseIsClosed) {
  const auto cells = box(2,2,1);
  const auto mask = domain(cells);
  auto velocity = mac(*mask);
  constexpr double circulation = .375;
  face(*velocity, {1,0,0}, 0, circulation);
  face(*velocity, {1,1,0}, 1, circulation);
  face(*velocity, {1,1,0}, 0, -circulation);
  face(*velocity, {0,1,0}, 1, -circulation);
  face(*velocity, {0,0,0}, 0, 17);
  face(*velocity, {2,1,0}, 0, -23);
  face(*velocity, {0,2,0}, 1, 41);
  face(*velocity, {1,0,1}, 2, 59);
  write("velocity.vdb", {velocity}); write("domain.vdb", {mask});
  const auto result = vdb::WriteProjectedVelocity(file("velocity.vdb"), file("domain.vdb"), file("cycle.vdb"));
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(result.iterations, 0u);
  EXPECT_DOUBLE_EQ(result.maxDivergenceBefore, 0);
  EXPECT_DOUBLE_EQ(result.maxDivergenceAfter, 0);
  const auto output = read<openvdb::Vec3DGrid>("cycle.vdb", "velocity");
  ASSERT_TRUE(output);
  EXPECT_DOUBLE_EQ(output->tree().getValue({1,0,0})[0], circulation);
  EXPECT_DOUBLE_EQ(output->tree().getValue({1,1,0})[1], circulation);
  EXPECT_DOUBLE_EQ(output->tree().getValue({1,1,0})[0], -circulation);
  EXPECT_DOUBLE_EQ(output->tree().getValue({0,1,0})[1], -circulation);
  EXPECT_DOUBLE_EQ(maximumDivergence(*output, cells), 0);
  expectWallsClosed(*output, *mask, cells);
}

TEST_F(OpenVdbFluidProjection, RoundoffDivergenceOfOrderOneCirculationUsesExplicitFaceUlpBound) {
  const auto cells = box(3,3,1);
  const auto mask = domain(cells);
  auto velocity = mac(*mask);
  const auto stream = [](int x,int y) {
    if(x==1&&y==1)return .1;
    if(x==1&&y==2)return std::sqrt(2.0);
    if(x==2&&y==1)return .3;
    if(x==2&&y==2)return std::acos(-1.0);
    return 0.0;
  };
  for(const auto& c:cells) {
    if(c.x()>0) face(*velocity,c,0,stream(c.x(),c.y()+1)-stream(c.x(),c.y()));
    if(c.y()>0) face(*velocity,c,1,stream(c.x(),c.y())-stream(c.x()+1,c.y()));
  }
  write("roundoff-wind.vdb",{velocity}); write("roundoff-mask.vdb",{mask});
  const auto result=vdb::WriteProjectedVelocity(file("roundoff-wind.vdb"),file("roundoff-mask.vdb"),file("roundoff.vdb"));
  EXPECT_TRUE(result.converged);
  EXPECT_GT(result.maxDivergenceBefore,0);
  EXPECT_LT(result.maxDivergenceBefore,1e-14);
  EXPECT_GT(result.divergenceRoundoffBound,0);
  EXPECT_LE(result.maxDivergenceAfter,1e-6*result.maxDivergenceBefore+result.divergenceRoundoffBound);
  EXPECT_TRUE(result.roundoffLimited);
  const auto projected=read<openvdb::Vec3DGrid>("roundoff.vdb","velocity");
  for(const auto& c:cells)for(size_t a=0;a<3;++a)
    EXPECT_NEAR(projected->tree().getValue(c)[a],velocity->tree().getValue(c)[a],1e-14);
}

TEST_F(OpenVdbFluidProjection, LargeNativeMacCirculationCanBeReadBackAndProjectedAgain) {
  const auto cells = box(2,2,1);
  const auto mask = domain(cells);
  auto velocity = mac(*mask);
  constexpr double circulation = 1e14;
  face(*velocity, {1,0,0}, 0, circulation);
  face(*velocity, {1,1,0}, 1, circulation);
  face(*velocity, {1,1,0}, 0, -circulation);
  face(*velocity, {0,1,0}, 1, -circulation);
  write("large-mac.vdb", {velocity}); write("domain.vdb", {mask});
  const auto first = vdb::WriteProjectedVelocity(file("large-mac.vdb"), file("domain.vdb"), file("first-mac.vdb"));
  const auto second = vdb::WriteProjectedVelocity(file("first-mac.vdb"), file("first-mac.vdb"), file("second-mac.vdb"), {}, "velocity", "fluid");
  EXPECT_TRUE(first.converged); EXPECT_TRUE(second.converged);
  EXPECT_EQ(first.iterations, 0u); EXPECT_EQ(second.iterations, 0u);
  for (const auto* name : {"first-mac.vdb", "second-mac.vdb"}) {
    const auto output = read<openvdb::Vec3DGrid>(name, "velocity");
    ASSERT_TRUE(output);
    EXPECT_DOUBLE_EQ(output->tree().getValue({1,0,0})[0], circulation);
    EXPECT_DOUBLE_EQ(output->tree().getValue({1,1,0})[1], circulation);
    EXPECT_DOUBLE_EQ(output->tree().getValue({1,1,0})[0], -circulation);
    EXPECT_DOUBLE_EQ(output->tree().getValue({0,1,0})[1], -circulation);
    EXPECT_DOUBLE_EQ(maximumDivergence(*output, cells), 0);
    expectWallsClosed(*output, *mask, cells);
  }
}

TEST_F(OpenVdbFluidProjection, DisconnectedComponentsHaveIndependentGaugesAndInactiveScalarCellsAreIgnored) {
  const Cells cells{{0,0,0}, {1,0,0}, {10,0,0}, {10,1,0}, {20,4,0}};
  auto mask = openvdb::FloatGrid::create(0.0f);
  mask->setName("mask");
  for (const auto& c : cells) mask->tree().setValue(c, 1.0f);
  mask->tree().setValue({30,0,0}, 0.0f);
  mask->tree().setValueOff({40,0,0}, 9.0f);
  auto velocity = mac(*mask);
  face(*velocity, {1,0,0}, 0, 3.0);
  face(*velocity, {10,1,0}, 1, -2.0);
  write("velocity.vdb", {velocity}); write("domain.vdb", {mask});
  vdb::FluidProjectionOptions options; options.timeStep = .5; options.tolerance = 1e-11;
  const auto result = vdb::WriteProjectedVelocity(file("velocity.vdb"), file("domain.vdb"), file("separate.vdb"), options);
  EXPECT_EQ(result.fluidCellCount, 5u);
  EXPECT_EQ(result.faceCount, 2u);
  EXPECT_EQ(result.componentCount, 3u);
  const auto output = read<openvdb::Vec3DGrid>("separate.vdb", "velocity");
  const auto pressure = read<openvdb::DoubleGrid>("separate.vdb", "pressure");
  const auto savedMask = read<openvdb::BoolGrid>("separate.vdb", "fluid");
  ASSERT_TRUE(output); ASSERT_TRUE(pressure); ASSERT_TRUE(savedMask);
  expectZeroFaces(*output, cells, 1e-12);
  EXPECT_NEAR(pressure->tree().getValue({1,0,0}) - pressure->tree().getValue({0,0,0}), 6, 1e-10);
  EXPECT_NEAR(pressure->tree().getValue({10,1,0}) - pressure->tree().getValue({10,0,0}), -4, 1e-10);
  EXPECT_EQ(std::min(std::abs(pressure->tree().getValue({0,0,0})), std::abs(pressure->tree().getValue({1,0,0}))), 0);
  EXPECT_EQ(std::min(std::abs(pressure->tree().getValue({10,0,0})), std::abs(pressure->tree().getValue({10,1,0}))), 0);
  EXPECT_DOUBLE_EQ(pressure->tree().getValue({20,4,0}), 0);
  EXPECT_FALSE(savedMask->tree().isValueOn({30,0,0}));
  EXPECT_FALSE(savedMask->tree().isValueOn({40,0,0}));
}

TEST_F(OpenVdbFluidProjection, FiniteDoubleDomainMagnitudesOnlyDeterminePositiveCellMembership) {
  auto mask = openvdb::DoubleGrid::create(0);
  mask->setName("large positive mask");
  mask->tree().setValue({0,0,0}, 1e100);
  mask->tree().setValue({1,0,0}, std::numeric_limits<double>::max());
  mask->tree().setValueOff({10,0,0}, std::numeric_limits<double>::max());
  auto velocity = mac(*mask);
  face(*velocity, {1,0,0}, 0, .5);
  write("velocity.vdb", {velocity}); write("large-mask.vdb", {mask});
  vdb::FluidProjectionOptions options; options.timeStep = 1;
  const auto result = vdb::WriteProjectedVelocity(file("velocity.vdb"), file("large-mask.vdb"), file("large-mask-out.vdb"), options);
  EXPECT_TRUE(result.converged); EXPECT_EQ(result.fluidCellCount, 2u);
  EXPECT_EQ(result.faceCount, 1u); EXPECT_EQ(result.componentCount, 1u);
  const auto output = read<openvdb::Vec3DGrid>("large-mask-out.vdb", "velocity");
  const auto pressure = read<openvdb::DoubleGrid>("large-mask-out.vdb", "pressure");
  const auto savedMask = read<openvdb::BoolGrid>("large-mask-out.vdb", "fluid");
  ASSERT_TRUE(output); ASSERT_TRUE(pressure); ASSERT_TRUE(savedMask);
  EXPECT_DOUBLE_EQ(output->tree().getValue({1,0,0})[0], 0);
  EXPECT_NEAR(pressure->tree().getValue({1,0,0}) - pressure->tree().getValue({0,0,0}), .5, 1e-12);
  EXPECT_EQ(savedMask->activeVoxelCount(), 2u);
  EXPECT_FALSE(savedMask->tree().isValueOn({10,0,0}));
}

TEST_F(OpenVdbFluidProjection, CollocatedWorldGradientUsesIndependentShearedSamplingMapAndPhysicalUnits) {
  const auto cells = box(3,3,2);
  constexpr double dt = .2;
  const auto mask = domain(cells, .5, true);
  auto velocity = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
  velocity->setName("world velocity"); velocity->setIsInWorldSpace(true);
  velocity->setVectorType(openvdb::VEC_INVARIANT);
  openvdb::math::Mat4d matrix; matrix.setIdentity();
  matrix[0][0] = .75; matrix[0][1] = .15; matrix[1][1] = .8;
  matrix[1][2] = .1; matrix[2][0] = .1;
  matrix[3][0] = -.5; matrix[3][1] = -.25; matrix[3][2] = .2;
  velocity->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
  const auto field = [](const openvdb::Vec3d& p) { return openvdb::Vec3d(.5*p.x()+.25, .75*p.y()-.5, p.z()+.125); };
  const auto potential = [](const openvdb::Vec3d& p) {
    return .25*p.x()*p.x() + .375*p.y()*p.y() + .5*p.z()*p.z() + .25*p.x() - .5*p.y() + .125*p.z();
  };
  for (int x = -5; x <= 5; ++x) for (int y = -5; y <= 5; ++y) for (int z = -5; z <= 5; ++z) {
    const openvdb::Coord c(x,y,z);
    velocity->tree().setValue(c, field(velocity->indexToWorld(c.asVec3d())));
  }
  write("world.vdb", {velocity}); write("domain.vdb", {mask});
  vdb::FluidProjectionOptions options; options.timeStep = dt; options.tolerance = 1e-10;
  const auto result = vdb::WriteProjectedVelocity(file("world.vdb"), file("domain.vdb"), file("world-out.vdb"), options);
  EXPECT_TRUE(result.converged);
  const auto output = read<openvdb::Vec3DGrid>("world-out.vdb", "velocity");
  const auto pressure = read<openvdb::DoubleGrid>("world-out.vdb", "pressure");
  ASSERT_TRUE(output); ASSERT_TRUE(pressure);
  expectZeroFaces(*output, cells, 1e-8);
  expectWallsClosed(*output, *mask, cells);
  const auto anchor = cells.front();
  for (const auto& c : cells) {
    const double expected = (potential(mask->indexToWorld(c.asVec3d())) - potential(mask->indexToWorld(anchor.asVec3d()))) / dt;
    EXPECT_NEAR(pressure->tree().getValue(c) - pressure->tree().getValue(anchor), expected, 1e-8);
  }
}

TEST_F(OpenVdbFluidProjection, UniformWorldForceHasHydrostaticPressureAcrossDtScaleRotationAndReflection) {
  const auto cells = box(2,2,2);
  const openvdb::Vec3d acceleration(.75,-.5,1.25);
  int serial = 0;
  for (const double h : {.25, 2.0}) for (const double dt : {.125, .5}) {
    const auto mask = domain(cells, h, true);
    auto velocity = openvdb::Vec3SGrid::create(openvdb::Vec3f(0));
    velocity->setName("float world"); velocity->setIsInWorldSpace(true);
    velocity->setVectorType(openvdb::VEC_INVARIANT);
    velocity->tree().setValue({0,0,0}, openvdb::Vec3f(0));
    write("force-input.vdb", {velocity}); write("force-domain.vdb", {mask});
    vdb::FluidProjectionOptions options; options.timeStep = dt; options.tolerance = 1e-10;
    options.acceleration = {acceleration.x(), acceleration.y(), acceleration.z()};
    const std::string outputName = "force-" + std::to_string(serial++) + ".vdb";
    const auto result = vdb::WriteProjectedVelocity(file("force-input.vdb"), file("force-domain.vdb"), file(outputName.c_str()), options);
    EXPECT_TRUE(result.converged); EXPECT_GT(result.maxDivergenceBefore, 0);
    const auto output = read<openvdb::Vec3DGrid>(outputName.c_str(), "velocity");
    const auto pressure = read<openvdb::DoubleGrid>(outputName.c_str(), "pressure");
    ASSERT_TRUE(output); ASSERT_TRUE(pressure);
    expectZeroFaces(*output, cells, 1e-10);
    expectWallsClosed(*output, *mask, cells);
    const auto origin = mask->indexToWorld(cells.front().asVec3d());
    for (const auto& c : cells) {
      const auto displacement = mask->indexToWorld(c.asVec3d()) - origin;
      EXPECT_NEAR(pressure->tree().getValue(c) - pressure->tree().getValue(cells.front()), acceleration.dot(displacement), 1e-9);
    }
  }
}

TEST_F(OpenVdbFluidProjection, FarSourceGridIndicesCannotSilentlyLoseSubvoxelWorldFractions) {
  auto mask = openvdb::BoolGrid::create(false);
  mask->setName("mask");
  mask->tree().setValue({0,0,0}, true);
  mask->tree().setValue({1,0,0}, true);
  auto velocity = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
  velocity->setName("far-origin world field");
  velocity->setIsInWorldSpace(true);
  velocity->setVectorType(openvdb::VEC_INVARIANT);
  velocity->setTransform(openvdb::math::Transform::createLinearTransform(1e6));
  velocity->transform().postTranslate(openvdb::Vec3d(1e15,0,0));
  velocity->tree().setValue({-1'000'000'001,0,0}, {-1e12,0,0});
  velocity->tree().setValue({-1'000'000'000,0,0}, {0,0,0});
  velocity->tree().setValue({-999'999'999,0,0}, {1e12,0,0});
  write("far-field.vdb", {velocity}); write("domain.vdb", {mask});
  const auto original = bytes(file("far-field.vdb"));
  // The only interior face is world x=.5. Its exact linear velocity is 500000,
  // but a Double index near -1e9 can reconstruct roughly 476837 instead.
  // A world-query ULP check alone does not detect this loss of source fraction.
  try {
    (void)vdb::WriteProjectedVelocity(file("far-field.vdb"), file("domain.vdb"), file("imprecise.vdb"));
    FAIL() << "unrepresentable source interpolation fractions must be rejected";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("precision"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("imprecise.vdb")));
  EXPECT_EQ(bytes(file("far-field.vdb")), original);
}

TEST_F(OpenVdbFluidProjection, ForceScalingPreservesRecoverableSubnormalLocalIncrement) {
  auto mask = openvdb::BoolGrid::create(false);
  mask->setName("mask");
  constexpr double h = 1e6, dt = 3600, acceleration = 1e-320;
  mask->setTransform(openvdb::math::Transform::createLinearTransform(h));
  mask->tree().setValue({0,0,0}, true); mask->tree().setValue({1,0,0}, true);
  const auto velocity = mac(*mask);
  write("tiny-force.vdb", {velocity}); write("domain.vdb", {mask});
  vdb::FluidProjectionOptions options;
  options.timeStep = dt; options.acceleration = {acceleration,0,0};
  const auto result = vdb::WriteProjectedVelocity(file("tiny-force.vdb"), file("domain.vdb"), file("tiny-force-out.vdb"), options);
  EXPECT_TRUE(result.converged); EXPECT_GT(result.iterations, 0u);
  EXPECT_GT(result.maxDivergenceBefore, 0);
  EXPECT_DOUBLE_EQ(result.maxDivergenceAfter, 0);
  const auto output = read<openvdb::Vec3DGrid>("tiny-force-out.vdb", "velocity");
  const auto pressure = read<openvdb::DoubleGrid>("tiny-force-out.vdb", "pressure");
  ASSERT_TRUE(output); ASSERT_TRUE(pressure);
  EXPECT_DOUBLE_EQ(output->tree().getValue({1,0,0})[0], 0);
  const double difference = pressure->tree().getValue({1,0,0}) - pressure->tree().getValue({0,0,0});
  EXPECT_GT(difference, 0);
  // The final MAC increment has only about seven subnormal quanta. Its rounded
  // value gives a pressure about 4% from ideal hydrostatic a*h; an intermediate
  // inverse-Jacobian underflow used to erase the force and return exactly zero.
  EXPECT_NEAR(difference / (acceleration * h), 1, .1);
}

TEST_F(OpenVdbFluidProjection, IrrelevantLargeWallForceCannotEraseSmallOpenAxisForce) {
  auto mask = openvdb::BoolGrid::create(false);
  mask->setName("mask");
  constexpr double h = 1e6, dt = 3600, smallAcceleration = 1e-308;
  mask->setTransform(openvdb::math::Transform::createLinearTransform(h));
  mask->tree().setValue({0,0,0}, true); mask->tree().setValue({0,1,0}, true);
  const auto velocity = mac(*mask);
  write("mixed-force.vdb", {velocity}); write("domain.vdb", {mask});
  vdb::FluidProjectionOptions options;
  options.timeStep = dt; options.acceleration = {1e12,smallAcceleration,0};
  const auto result = vdb::WriteProjectedVelocity(file("mixed-force.vdb"), file("domain.vdb"), file("mixed-force-out.vdb"), options);
  EXPECT_TRUE(result.converged); EXPECT_GT(result.iterations, 0u);
  EXPECT_GT(result.maxDivergenceBefore, 0);
  EXPECT_DOUBLE_EQ(result.maxDivergenceAfter, 0);
  const auto output = read<openvdb::Vec3DGrid>("mixed-force-out.vdb", "velocity");
  const auto pressure = read<openvdb::DoubleGrid>("mixed-force-out.vdb", "pressure");
  ASSERT_TRUE(output); ASSERT_TRUE(pressure);
  expectZeroFaces(*output, {{0,0,0}, {0,1,0}}, 0);
  const double difference = pressure->tree().getValue({0,1,0}) - pressure->tree().getValue({0,0,0});
  EXPECT_GT(difference, 0);
  EXPECT_NEAR(difference / (smallAcceleration * h), 1, 1e-9);
}

TEST_F(OpenVdbFluidProjection, DisconnectedHugeAndSubnormalFlowsAreProjectedWithoutAbsoluteCutoffs) {
  const Cells cells{{0,0,0}, {1,0,0}, {16,0,0}, {17,0,0}};
  const auto mask = domain(cells);
  auto velocity = mac(*mask);
  face(*velocity, {1,0,0}, 0, 1e12);
  face(*velocity, {17,0,0}, 0, 1e-312);
  write("ranges.vdb", {velocity}); write("domain.vdb", {mask});
  vdb::FluidProjectionOptions options; options.timeStep = 1;
  const auto result = vdb::WriteProjectedVelocity(file("ranges.vdb"), file("domain.vdb"), file("ranges-out.vdb"), options);
  EXPECT_TRUE(result.converged); EXPECT_EQ(result.componentCount, 2u);
  EXPECT_GT(result.iterations, 0u);
  const auto output = read<openvdb::Vec3DGrid>("ranges-out.vdb", "velocity");
  const auto pressure = read<openvdb::DoubleGrid>("ranges-out.vdb", "pressure");
  ASSERT_TRUE(output); ASSERT_TRUE(pressure);
  EXPECT_LE(std::abs(output->tree().getValue({1,0,0})[0]) / 1e12, options.tolerance);
  EXPECT_LE(std::abs(output->tree().getValue({17,0,0})[0]) / 1e-312, options.tolerance);
  EXPECT_NEAR((pressure->tree().getValue({1,0,0}) - pressure->tree().getValue({0,0,0})) / 1e12, 1, 1e-10);
  EXPECT_NEAR((pressure->tree().getValue({17,0,0}) - pressure->tree().getValue({16,0,0})) / 1e-312, 1, 1e-8);
}

TEST_F(OpenVdbFluidProjection, ActiveDomainTilesAndOptionalDomainOutputRespectTopologyAndNames) {
  auto mask = openvdb::DoubleGrid::create(0.0);
  mask->setName("tiled mask");
  mask->tree().addTile(1, {0,0,0}, 1.0, true);
  auto velocity = mac(*mask);
  write("zero.vdb", {velocity}); write("tile.vdb", {mask});
  vdb::FluidProjectionOptions options;
  options.velocityGridName = "projected"; options.pressureGridName = "potential";
  options.domainGridName = "domain copy"; options.writeDomain = false;
  vdb::ProcessingOptions processing;
  processing.voxelSize = -1; processing.halfWidth = std::numeric_limits<double>::quiet_NaN();
  processing.gridName.clear(); processing.maxTriangles = 0;
  const auto result = vdb::WriteProjectedVelocity(file("zero.vdb"), file("tile.vdb"), file("tile-out.vdb"), options, {}, {}, processing);
  EXPECT_TRUE(result.converged); EXPECT_EQ(result.iterations, 0u);
  EXPECT_EQ(result.fluidCellCount, 512u); EXPECT_EQ(result.faceCount, 1344u);
  EXPECT_EQ(result.componentCount, 1u);
  EXPECT_EQ(result.velocityGridName, "projected"); EXPECT_EQ(result.pressureGridName, "potential");
  const auto output = read<openvdb::Vec3DGrid>("tile-out.vdb", "projected");
  const auto pressure = read<openvdb::DoubleGrid>("tile-out.vdb", "potential");
  ASSERT_TRUE(output); ASSERT_TRUE(pressure);
  EXPECT_EQ(pressure->activeVoxelCount(), 512u);
  openvdb::io::File input(file("tile-out.vdb").string()); input.open(false);
  size_t count = 0; for (auto it = input.beginName(); it != input.endName(); ++it) {
    EXPECT_NE(it.gridName(), "domain copy"); ++count;
  }
  EXPECT_EQ(count, 2u); input.close();
  processing.maxVoxelCount = 511;
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("zero.vdb"), file("tile.vdb"), file("tile-budget.vdb"), options, {}, {}, processing), std::exception);
  EXPECT_FALSE(std::filesystem::exists(file("tile-budget.vdb")));
}

TEST_F(OpenVdbFluidProjection, InvalidOptionsAndMemoryCellBudgetsProduceNoPartialOutput) {
  const auto mask = domain(box(2,1,1)); auto velocity = mac(*mask);
  face(*velocity, {1,0,0}, 0, 1);
  write("velocity.vdb", {velocity}); write("domain.vdb", {mask});
  int serial = 0;
  const auto fails = [&](const vdb::FluidProjectionOptions& options, const vdb::ProcessingOptions& processing = {}) {
    const auto output = file(("invalid-" + std::to_string(serial++) + ".vdb").c_str());
    EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("velocity.vdb"), file("domain.vdb"), output, options, {}, {}, processing), std::exception);
    EXPECT_FALSE(std::filesystem::exists(output));
  };
  for (double dt : {0.0, -1.0, 1e-7, 3601.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    vdb::FluidProjectionOptions options; options.timeStep = dt; fails(options);
  }
  for (double tolerance : {0.0, 1e-13, .1, std::numeric_limits<double>::quiet_NaN()}) {
    vdb::FluidProjectionOptions options; options.tolerance = tolerance; fails(options);
  }
  for (uint32_t iterations : {0u, 10001u}) { vdb::FluidProjectionOptions options; options.maxIterations = iterations; fails(options); }
  for (uint64_t count : {0ull, 1ull, 1'000'001ull}) { vdb::FluidProjectionOptions options; options.maxFluidCells = count; fails(options); }
  for (double acceleration : {1e13, std::numeric_limits<double>::quiet_NaN()}) {
    vdb::FluidProjectionOptions options; options.acceleration[1] = acceleration; fails(options);
  }
  { vdb::FluidProjectionOptions options; options.pressureGridName = options.velocityGridName; fails(options); }
  { vdb::FluidProjectionOptions options; options.domainGridName.clear(); options.writeDomain = false; fails(options); }
  { vdb::FluidProjectionOptions options; options.velocityGridName.assign(257, 'x'); fails(options); }
  for (uint64_t voxels : {0ull, 1ull}) { vdb::ProcessingOptions processing; processing.maxVoxelCount = voxels; fails({}, processing); }
  { vdb::ProcessingOptions processing; processing.maxInputBytes = 64; fails({}, processing); }
}

TEST_F(OpenVdbFluidProjection, InvalidDomainsTransformsVelocityFramesAndNonfiniteValuesFailBeforeWriting) {
  const auto cells = box(2,1,1);
  const auto validMask = domain(cells); const auto validVelocity = mac(*validMask);
  write("valid-velocity.vdb", {validVelocity}); write("valid-domain.vdb", {validMask});
  int serial = 0;
  const auto fails = [&](const openvdb::GridBase::Ptr& velocity, const openvdb::GridBase::Ptr& mask) {
    write("bad-velocity.vdb", {velocity}); write("bad-domain.vdb", {mask});
    const auto output = file(("bad-" + std::to_string(serial++) + ".vdb").c_str());
    EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("bad-velocity.vdb"), file("bad-domain.vdb"), output), std::exception);
    EXPECT_FALSE(std::filesystem::exists(output));
  };
  fails(openvdb::FloatGrid::create(0), validMask);
  fails(validVelocity, openvdb::Vec3DGrid::create(openvdb::Vec3d(0)));
  fails(validVelocity, domain({}));
  { auto grid = openvdb::BoolGrid::create(false); grid->tree().setValue({0,0,0}, false); fails(validVelocity, grid); }
  { auto grid = openvdb::FloatGrid::create(0); grid->tree().setValue({0,0,0}, -1); fails(validVelocity, grid); }
  { auto grid = openvdb::DoubleGrid::create(1); grid->tree().setValue({0,0,0}, 1); fails(validVelocity, grid); }
  { auto grid = openvdb::DoubleGrid::create(0); grid->tree().setValue({0,0,0}, std::numeric_limits<double>::infinity()); fails(validVelocity, grid); }
  { auto grid = domain(cells); grid->transform().postScale(openvdb::Vec3d(1,2,1)); fails(validVelocity, grid); }
  { auto grid = domain(cells); openvdb::math::Mat4d m; m.setIdentity(); m[0][1] = .2; grid->setTransform(openvdb::math::Transform::createLinearTransform(m)); fails(validVelocity, grid); }
  { auto grid = mac(*validMask); grid->transform().postTranslate({1,0,0}); fails(grid, validMask); }
  { auto grid = mac(*validMask); grid->setIsInWorldSpace(true); fails(grid, validMask); }
  { auto grid = mac(*validMask); grid->setVectorType(openvdb::VEC_INVARIANT); fails(grid, validMask); }
  { auto grid = mac(*validMask); grid->setGridClass(openvdb::GRID_UNKNOWN); fails(grid, validMask); }
  { auto grid = mac(*validMask); face(*grid, {50,0,0}, 1, std::numeric_limits<double>::quiet_NaN()); fails(grid, validMask); }
  { auto grid = mac(*validMask); face(*grid, {1,0,0}, 0, 1e25); fails(grid, validMask); }
  for (int boundary : {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
    const auto grid = domain({openvdb::Coord(boundary,0,0)}); fails(mac(*grid), grid);
  }
}

TEST_F(OpenVdbFluidProjection, InsufficientIterationsAndWithinComponentNormalizationLossLeaveInputsAndOutputUntouched) {
  const auto cells = box(7,6,5); const auto mask = domain(cells);
  const auto velocity = gradientVelocity(*mask, cells, 1, 1, [](const openvdb::Coord& c) {
    return std::sin(.31*c.x()) + std::cos(.73*c.y()) + c.z()*c.z() + .17*c.x()*c.y()*c.y();
  });
  write("hard.vdb", {velocity}); write("domain.vdb", {mask});
  const auto originalVelocity = bytes(file("hard.vdb")), originalDomain = bytes(file("domain.vdb"));
  vdb::FluidProjectionOptions options; options.maxIterations = 1; options.tolerance = 1e-12;
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("hard.vdb"), file("domain.vdb"), file("failed.vdb"), options), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(file("failed.vdb")));
  EXPECT_EQ(bytes(file("hard.vdb")), originalVelocity); EXPECT_EQ(bytes(file("domain.vdb")), originalDomain);
  const auto chain = domain(box(3,1,1)); auto extremes = mac(*chain);
  face(*extremes, {1,0,0}, 0, 1e12); face(*extremes, {2,0,0}, 0, 1e-312);
  write("extremes.vdb", {extremes}); write("chain.vdb", {chain});
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("extremes.vdb"), file("chain.vdb"), file("erased.vdb")), std::exception);
  EXPECT_FALSE(std::filesystem::exists(file("erased.vdb")));
}

TEST_F(OpenVdbFluidProjection, ExactDescriptorsRecordedMetadataSeekableInputAndExclusiveOutputAreEnforced) {
  const auto mask = domain(box(2,1,1)); auto first = mac(*mask), second = mac(*mask);
  face(*first, {1,0,0}, 0, 1); face(*second, {1,0,0}, 0, 3);
  write("duplicates.vdb", {first,second}); write("domain.vdb", {mask});
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("duplicates.vdb"), file("domain.vdb"), file("ambiguous.vdb"), {}, "wind"), std::exception);
  openvdb::io::File catalog(file("duplicates.vdb").string()); catalog.open(false);
  std::vector<std::string> descriptors;
  for (auto it = catalog.beginName(); it != catalog.endName(); ++it) descriptors.push_back(it.gridName());
  ASSERT_EQ(descriptors.size(), 2u);
  for (size_t i = 0; i < descriptors.size(); ++i) {
    const auto chosen = openvdb::gridPtrCast<openvdb::Vec3DGrid>(catalog.readGrid(descriptors[i]));
    ASSERT_TRUE(chosen);
    const std::string outputName = "descriptor-" + std::to_string(i) + ".vdb";
    vdb::FluidProjectionOptions options; options.timeStep = 1;
    (void)vdb::WriteProjectedVelocity(file("duplicates.vdb"), file("domain.vdb"), file(outputName.c_str()), options, "descriptor:" + descriptors[i]);
    const auto pressure = read<openvdb::DoubleGrid>(outputName.c_str(), "pressure");
    EXPECT_NEAR(pressure->tree().getValue({1,0,0}) - pressure->tree().getValue({0,0,0}), chosen->tree().getValue({1,0,0})[0], 1e-10);
  }
  catalog.close();
  auto collision = mac(*mask); collision->setName("wind[0]");
  write("collision.vdb", {first,second,collision});
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("collision.vdb"), file("domain.vdb"), file("collision-out.vdb")), std::exception);
  write("no-stats.vdb", {first}, false);
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("no-stats.vdb"), file("domain.vdb"), file("no-stats-out.vdb")), std::exception);
  write("domain-no-stats.vdb", {mask}, false);
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("duplicates.vdb"), file("domain-no-stats.vdb"), file("no-domain-stats.vdb"), {}, "descriptor:" + descriptors.front()), std::exception);
  { std::ofstream stream(file("stream.vdb"), std::ios::binary); openvdb::io::Stream output(stream); output.write({first}); }
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("stream.vdb"), file("domain.vdb"), file("stream-out.vdb")), std::exception);
  write("single.vdb", {first});
  { std::ofstream output(file("existing.vdb"), std::ios::binary); output << "keep existing bytes"; }
  const auto existing = bytes(file("existing.vdb")), source = bytes(file("single.vdb")), domainBytes = bytes(file("domain.vdb"));
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("single.vdb"), file("domain.vdb"), file("existing.vdb")), std::exception);
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("single.vdb"), file("domain.vdb"), file("single.vdb")), std::exception);
  EXPECT_THROW((void)vdb::WriteProjectedVelocity(file("single.vdb"), file("domain.vdb"), file("domain.vdb")), std::exception);
  EXPECT_EQ(bytes(file("existing.vdb")), existing); EXPECT_EQ(bytes(file("single.vdb")), source);
  EXPECT_EQ(bytes(file("domain.vdb")), domainBytes);
  for (const auto* name : {"ambiguous.vdb", "collision-out.vdb", "no-stats-out.vdb", "no-domain-stats.vdb", "stream-out.vdb"})
    EXPECT_FALSE(std::filesystem::exists(file(name)));
}
#endif
} // namespace
