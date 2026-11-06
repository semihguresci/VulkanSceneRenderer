#include "Container/geometry/OpenVdbFluidViscosity.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbFluidViscosityAvailability,
     DisabledBuildFailsBeforePathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  vdb::FluidViscosityOptions options;
  options.timeStep = 0;
  options.maxIterations = 0;
  try {
    (void)vdb::WriteViscousFluidState("missing.vdb", {}, options);
    FAIL() << "disabled viscosity must fail";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFluidViscosity : public ::testing::Test {
protected:
  struct State {
    openvdb::DoubleGrid::Ptr density, temperature, pressure;
    openvdb::Vec3DGrid::Ptr velocity;
    openvdb::BoolGrid::Ptr domain;
    openvdb::GridPtrVec grids() const {
      return {density, temperature, velocity, pressure, domain};
    }
  };
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-viscosity-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path file(const char *name) const {
    return directory / name;
  }
  void write(const char *name, const openvdb::GridPtrVec &grids,
             bool stats = true) const {
    openvdb::io::File output(file(name).string());
    output.setGridStatsMetadataEnabled(stats);
    output.write(grids);
    output.close();
  }
  static std::string bytes(const std::filesystem::path &file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  static State make(int nx = 2, int ny = 2, int nz = 1, double h = 1,
                    bool reflected = false) {
    State state;
    state.domain = openvdb::BoolGrid::create(false);
    auto transform = openvdb::math::Transform::createLinearTransform(h);
    if (reflected) {
      transform->postRotate(.37, openvdb::math::Z_AXIS);
      transform->postScale(openvdb::Vec3d(-1, 1, 1));
    }
    state.domain->setTransform(transform);
    state.domain->setName("fluid");
    state.domain->setGridClass(openvdb::GRID_UNKNOWN);
    state.domain->insertMeta("artist",
                             openvdb::StringMetadata("domain artist"));
    state.domain->insertMeta("shared-key",
                             openvdb::StringMetadata("domain value"));
    state.density = openvdb::DoubleGrid::create(0);
    state.temperature = openvdb::DoubleGrid::create(0);
    state.pressure = openvdb::DoubleGrid::create(0);
    state.density->setName("density");
    state.density->setGridClass(openvdb::GRID_FOG_VOLUME);
    state.temperature->setName("temperature");
    state.pressure->setName("pressure");
    state.temperature->setGridClass(openvdb::GRID_UNKNOWN);
    state.pressure->setGridClass(openvdb::GRID_UNKNOWN);
    state.pressure->insertMeta("shared-key", openvdb::Int32Metadata(17));
    state.velocity = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
    state.velocity->setName("velocity");
    state.velocity->setGridClass(openvdb::GRID_STAGGERED);
    state.velocity->setIsInWorldSpace(false);
    state.velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    for (const auto &grid : {state.density, state.temperature, state.pressure})
      grid->setTransform(transform->copy());
    state.velocity->setTransform(transform->copy());
    for (int x = 0; x < nx; ++x)
      for (int y = 0; y < ny; ++y)
        for (int z = 0; z < nz; ++z) {
          const openvdb::Coord c(x, y, z);
          state.domain->tree().setValue(c, true);
          state.density->tree().setValue(c, .25 + .01 * x);
          state.temperature->tree().setValue(c, 300 + .5 * y);
          state.pressure->tree().setValue(c, 0);
          state.velocity->tree().setValue(c, openvdb::Vec3d(0));
        }
    return state;
  }
  static void face(openvdb::Vec3DGrid &grid, const openvdb::Coord &c,
                   size_t axis, double value) {
    auto vector = grid.tree().getValue(c);
    vector[axis] = value;
    grid.tree().setValue(c, vector);
  }
  static void loop(State &state, double speed = 1, int x = 0) {
    face(*state.velocity, {x + 1, 0, 0}, 0, speed);
    face(*state.velocity, {x + 1, 1, 0}, 1, speed);
    face(*state.velocity, {x + 1, 1, 0}, 0, -speed);
    face(*state.velocity, {x, 1, 0}, 1, -speed);
  }
  State read(const char *name,
             const vdb::FluidViscosityOptions &options = {}) const {
    openvdb::io::File input(file(name).string());
    input.open(false);
    State state{openvdb::gridPtrCast<openvdb::DoubleGrid>(
                    input.readGrid(options.densityGridName)),
                openvdb::gridPtrCast<openvdb::DoubleGrid>(
                    input.readGrid(options.temperatureGridName)),
                openvdb::gridPtrCast<openvdb::DoubleGrid>(
                    input.readGrid(options.pressureGridName)),
                openvdb::gridPtrCast<openvdb::Vec3DGrid>(
                    input.readGrid(options.velocityGridName)),
                openvdb::gridPtrCast<openvdb::BoolGrid>(
                    input.readGrid(options.domainGridName))};
    input.close();
    return state;
  }
  static void sameScalars(const State &a, const State &b) {
    ASSERT_TRUE(b.density);
    ASSERT_TRUE(b.temperature);
    ASSERT_TRUE(b.domain);
    for (auto value = a.domain->cbeginValueOn(); value; ++value)
      if (*value) {
        const auto c = value.getCoord();
        EXPECT_DOUBLE_EQ(a.density->tree().getValue(c),
                         b.density->tree().getValue(c));
        EXPECT_DOUBLE_EQ(a.temperature->tree().getValue(c),
                         b.temperature->tree().getValue(c));
        EXPECT_EQ(*value, b.domain->tree().getValue(c));
      }
    EXPECT_EQ(a.domain->transform(), b.domain->transform());
  }
  static void closed(const State &state) {
    for (auto value = state.domain->cbeginValueOn(); value; ++value)
      if (*value)
        for (size_t a = 0; a < 3; ++a) {
          auto before = value.getCoord(), after = before;
          --before[a];
          ++after[a];
          if (!state.domain->tree().getValue(before))
            EXPECT_DOUBLE_EQ(
                state.velocity->tree().getValue(value.getCoord())[a], 0);
          if (!state.domain->tree().getValue(after))
            EXPECT_DOUBLE_EQ(state.velocity->tree().getValue(after)[a], 0);
        }
  }
  std::filesystem::path directory;
};

TEST_F(OpenVdbFluidViscosity, ZeroViscosityRetainsInactiveSignedZeroNativeFields) {
  auto state = make(1, 1, 1);
  const openvdb::Coord remote(16, 0, 0);
  for (const auto &grid : {state.density, state.temperature, state.pressure})
    grid->tree().touchLeaf(remote)->setValueOff(remote, -0.0);
  state.velocity->tree().touchLeaf(remote)->setValueOff(
      remote, openvdb::Vec3d(-0.0, 0.0, -0.0));
  {
    openvdb::io::File input(file("signed-zero.vdb").string());
    input.setCompression(openvdb::io::COMPRESS_ZIP);
    input.write(state.grids());
  }
  const auto before = read("signed-zero.vdb");
  ASSERT_TRUE(std::signbit(before.pressure->tree().getValue(remote)));
  ASSERT_TRUE(std::signbit(before.velocity->tree().getValue(remote)[0]));
  vdb::FluidViscosityOptions options;
  options.kinematicViscosity = 0;
  const auto result = vdb::WriteViscousFluidState(
      file("signed-zero.vdb"), file("preserved.vdb"), options);
  EXPECT_TRUE(result.skipped);
  const auto after = read("preserved.vdb");
  for (const auto &grid : {after.density, after.temperature, after.pressure}) {
    EXPECT_TRUE(std::signbit(grid->tree().getValue(remote)));
    EXPECT_FALSE(grid->tree().isValueOn(remote));
  }
  EXPECT_TRUE(std::signbit(after.velocity->tree().getValue(remote)[0]));
  EXPECT_FALSE(std::signbit(after.velocity->tree().getValue(remote)[1]));
  EXPECT_TRUE(std::signbit(after.velocity->tree().getValue(remote)[2]));
  EXPECT_TRUE(before.velocity->tree().hasSameTopology(after.velocity->tree()));
  EXPECT_TRUE(before.pressure->tree().hasSameTopology(after.pressure->tree()));
}

TEST_F(OpenVdbFluidViscosity,
       BackwardEulerFaceEigenmodeHasKnownDecayAndPreservesSolenoidalFlow) {
  auto source = make();
  loop(source, 2);
  write("state.vdb", source.grids());
  const auto original = bytes(file("state.vdb"));
  vdb::FluidViscosityOptions options;
  options.timeStep = .25;
  options.kinematicViscosity = .4;
  options.tolerance = 1e-10;
  const auto result = vdb::WriteViscousFluidState(file("state.vdb"),
                                                  file("viscous.vdb"), options);
  const auto output = read("viscous.vdb");
  ASSERT_TRUE(output.velocity);
  ASSERT_TRUE(output.pressure);
  // The two-node antisymmetric face mode has eigenvalue3 along its pair and
  // four zero exterior neighbors: -L has eigenvalue7 for every circulation
  // face.
  const double decay = 1 / (1 + 7 * .4 * .25);
  EXPECT_TRUE(result.converged);
  EXPECT_FALSE(result.skipped);
  EXPECT_EQ(result.faceCount, 4u);
  EXPECT_EQ(result.fluidCellCount, 4u);
  EXPECT_GT(result.diffusionIterations, 0u);
  EXPECT_LE(result.diffusionResidual, options.tolerance);
  EXPECT_NEAR(output.velocity->tree().getValue({1, 0, 0})[0], 2 * decay, 1e-10);
  EXPECT_NEAR(output.velocity->tree().getValue({1, 1, 0})[1], 2 * decay, 1e-10);
  EXPECT_NEAR(output.velocity->tree().getValue({1, 1, 0})[0], -2 * decay,
              1e-10);
  EXPECT_NEAR(output.velocity->tree().getValue({0, 1, 0})[1], -2 * decay,
              1e-10);
  EXPECT_DOUBLE_EQ(result.kineticEnergyBefore, 8);
  EXPECT_NEAR(result.kineticEnergyAfterDiffusion / result.kineticEnergyBefore,
              decay * decay, 1e-10);
  EXPECT_LE(result.kineticEnergyAfterProjection,
            result.kineticEnergyAfterDiffusion * (1 + options.tolerance));
  sameScalars(source, output);
  closed(output);
  EXPECT_EQ(output.pressure->metaValue<int32_t>("shared-key"), 17);
  EXPECT_EQ(output.domain->metaValue<std::string>("shared-key"),
            "domain value");
  EXPECT_EQ(output.velocity->getVectorType(),
            openvdb::VEC_CONTRAVARIANT_RELATIVE);
  EXPECT_FALSE(output.velocity->isInWorldSpace());
  EXPECT_EQ(bytes(file("state.vdb")), original);
}

TEST_F(OpenVdbFluidViscosity,
       WorldVoxelScaleDtAndReflectionUseNuDtOverHSquared) {
  int serial = 0;
  for (double h : {.25, 2.0})
    for (bool reflected : {false, true}) {
      auto source = make(2, 2, 1, h, reflected);
      loop(source);
      write("source.vdb", source.grids());
      vdb::FluidViscosityOptions options;
      options.timeStep = .2;
      options.kinematicViscosity = .3;
      options.tolerance = 1e-10;
      const auto name = "scale-" + std::to_string(serial++) + ".vdb";
      const auto result = vdb::WriteViscousFluidState(
          file("source.vdb"), file(name.c_str()), options);
      const auto output = read(name.c_str());
      const double expected = 1 / (1 + 7 * .3 * .2 / (h * h));
      EXPECT_NEAR(output.velocity->tree().getValue({1, 0, 0})[0], expected,
                  1e-10);
      EXPECT_NEAR(result.kineticEnergyBefore / (2 * std::pow(h, 5)), 1, 1e-12);
      EXPECT_EQ(source.domain->transform(), output.velocity->transform());
      closed(output);
      sameScalars(source, output);
    }
}

TEST_F(OpenVdbFluidViscosity,
       ZeroViscosityCopiesAllFiveLoadedFieldsAndDoesNotProject) {
  auto source = make();
  loop(source, .375);
  source.pressure->tree().setValue({1, 1, 0}, 7);
  write("state.vdb", source.grids());
  vdb::FluidViscosityOptions options;
  options.kinematicViscosity = 0;
  const auto result =
      vdb::WriteViscousFluidState(file("state.vdb"), file("copy.vdb"), options);
  const auto output = read("copy.vdb");
  EXPECT_TRUE(result.skipped);
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(result.diffusionIterations, 0u);
  EXPECT_EQ(result.projectionIterations, 0u);
  sameScalars(source, output);
  EXPECT_TRUE(source.velocity->tree().hasSameTopology(output.velocity->tree()));
  for (auto value = source.domain->cbeginValueOn(); value; ++value) {
    EXPECT_EQ(source.velocity->tree().getValue(value.getCoord()),
              output.velocity->tree().getValue(value.getCoord()));
    EXPECT_EQ(source.pressure->tree().getValue(value.getCoord()),
              output.pressure->tree().getValue(value.getCoord()));
  }
  EXPECT_EQ(output.pressure->metaValue<int32_t>("shared-key"), 17);
}

TEST_F(OpenVdbFluidViscosity,
       DisconnectedLargeAndWeakFaceBlocksRetainTheirOwnDecay) {
  auto source = make();
  for (int x = 16; x < 18; ++x)
    for (int y = 0; y < 2; ++y) {
      const openvdb::Coord c(x, y, 0);
      source.domain->tree().setValue(c, true);
      source.density->tree().setValue(c, .5);
      source.temperature->tree().setValue(c, 300);
      source.pressure->tree().setValue(c, 0);
      source.velocity->tree().setValue(c, openvdb::Vec3d(0));
    }
  loop(source, 1e12);
  loop(source, 1e-200, 16);
  write("ranges.vdb", source.grids());
  vdb::FluidViscosityOptions options;
  options.timeStep = .1;
  options.kinematicViscosity = 1;
  const auto result = vdb::WriteViscousFluidState(
      file("ranges.vdb"), file("ranges-out.vdb"), options);
  const auto output = read("ranges-out.vdb");
  const double decay = 1 / 1.7;
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(result.faceCount, 8u);
  EXPECT_NEAR(output.velocity->tree().getValue({1, 0, 0})[0] / 1e12, decay,
              1e-9);
  EXPECT_GT(output.velocity->tree().getValue({17, 0, 0})[0], 0);
  EXPECT_NEAR(output.velocity->tree().getValue({17, 0, 0})[0] / 1e-200, decay,
              1e-9);
  closed(output);
}

TEST_F(OpenVdbFluidViscosity,
       LargeFiniteCoefficientRemainsDissipativeAndTinyLostCoefficientFails) {
  // OpenVDB's public uniform-scale constructor rejects determinants below its
  // 3e-15 threshold, so use the smallest supported authoring scale here.
  constexpr double h = 1.5e-5;
  auto source = make(2, 2, 1, h);
  loop(source);
  write("tiny-voxels.vdb", source.grids());
  vdb::FluidViscosityOptions options;
  options.timeStep = 3600;
  options.kinematicViscosity = 1e6;
  const auto result = vdb::WriteViscousFluidState(file("tiny-voxels.vdb"),
                                                  file("large-r.vdb"), options);
  const auto output = read("large-r.vdb");
  const double expected = 1 / (1 + 7 * (1e6 * 3600 / (h * h)));
  EXPECT_GT(output.velocity->tree().getValue({1, 0, 0})[0], 0);
  EXPECT_NEAR(output.velocity->tree().getValue({1, 0, 0})[0] / expected, 1,
              1e-6);
  EXPECT_LT(result.kineticEnergyAfterDiffusion, result.kineticEnergyBefore);
  auto large = make(2, 2, 1, 1e6);
  loop(large);
  write("large-voxels.vdb", large.grids());
  options.timeStep = 1e-6;
  options.kinematicViscosity = std::numeric_limits<double>::denorm_min();
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("large-voxels.vdb"),
                                                 file("lost.vdb"), options),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("lost.vdb")));
}

TEST_F(OpenVdbFluidViscosity,
       DiffusionThenProjectionReducesEnergyAndClosesDivergentPredictor) {
  auto source = make(4, 4, 3);
  face(*source.velocity, {2, 2, 1}, 0, 2);
  face(*source.velocity, {1, 2, 1}, 1, -1);
  write("divergent.vdb", source.grids());
  const auto result =
      vdb::WriteViscousFluidState(file("divergent.vdb"), file("projected.vdb"));
  const auto output = read("projected.vdb");
  EXPECT_TRUE(result.converged);
  EXPECT_GT(result.projectionIterations, 0u);
  EXPECT_LT(result.kineticEnergyAfterDiffusion, result.kineticEnergyBefore);
  EXPECT_LE(result.kineticEnergyAfterProjection,
            result.kineticEnergyAfterDiffusion * (1 + 1e-6));
  EXPECT_LT(result.maxDivergenceAfter, result.maxDivergenceBefore * 1e-4);
  closed(output);
  sameScalars(source, output);
}

TEST_F(OpenVdbFluidViscosity,
       IterationFailurePreservesInputAndDoesNotPublishPartialState) {
  auto source = make(7, 6, 5);
  for (auto cell = source.domain->cbeginValueOn(); cell; ++cell)
    for (size_t a = 0; a < 3; ++a) {
      auto previous = cell.getCoord();
      --previous[a];
      if (!source.domain->tree().getValue(previous))
        continue;
      const auto c = cell.getCoord();
      face(*source.velocity, c, a,
           std::sin(.31 * c.x() + .73 * c.y() + .17 * c.z() + a));
    }
  write("hard.vdb", source.grids());
  const auto original = bytes(file("hard.vdb"));
  vdb::FluidViscosityOptions options;
  options.kinematicViscosity = 10;
  options.timeStep = 1;
  options.maxIterations = 1;
  options.tolerance = 1e-12;
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("hard.vdb"),
                                                 file("failed.vdb"), options),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(file("failed.vdb")));
  EXPECT_EQ(bytes(file("hard.vdb")), original);
}

TEST_F(OpenVdbFluidViscosity,
       LiteralCustomStateNamesAndExclusiveOutputAreEnforced) {
  auto source = make();
  loop(source);
  vdb::FluidViscosityOptions options;
  options.densityGridName = "descriptor:literal density";
  options.temperatureGridName = "heat";
  options.velocityGridName = "descriptor:wind";
  options.pressureGridName = "potential";
  options.domainGridName = "descriptor:mask";
  source.density->setName(options.densityGridName);
  source.temperature->setName(options.temperatureGridName);
  source.velocity->setName(options.velocityGridName);
  source.pressure->setName(options.pressureGridName);
  source.domain->setName(options.domainGridName);
  write("named.vdb", source.grids());
  (void)vdb::WriteViscousFluidState(file("named.vdb"), file("named-out.vdb"),
                                    options);
  sameScalars(source, read("named-out.vdb", options));
  const auto original = bytes(file("named.vdb"));
  {
    std::ofstream output(file("existing.vdb"));
    output << "preserve existing bytes";
  }
  const auto existing = bytes(file("existing.vdb"));
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("named.vdb"),
                                                 file("named.vdb"), options),
               std::exception);
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("named.vdb"),
                                                 file("existing.vdb"), options),
               std::exception);
  EXPECT_EQ(bytes(file("named.vdb")), original);
  EXPECT_EQ(bytes(file("existing.vdb")), existing);
}

TEST_F(OpenVdbFluidViscosity,
       InvalidOptionsAndSharedStateBudgetsFailBeforeOutput) {
  auto source = make();
  loop(source);
  write("state.vdb", source.grids());
  int serial = 0;
  const auto fail = [&](const vdb::FluidViscosityOptions &options,
                        const vdb::ProcessingOptions &p = {}) {
    const auto output =
        file(("invalid-" + std::to_string(serial++) + ".vdb").c_str());
    EXPECT_THROW((void)vdb::WriteViscousFluidState(file("state.vdb"), output,
                                                   options, p),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(output));
  };
  for (double time :
       {0.0, 1e-7, 3601.0, std::numeric_limits<double>::quiet_NaN()}) {
    vdb::FluidViscosityOptions o;
    o.timeStep = time;
    fail(o);
  }
  for (double nu : {-1.0, 1e7, std::numeric_limits<double>::infinity()}) {
    vdb::FluidViscosityOptions o;
    o.kinematicViscosity = nu;
    fail(o);
  }
  for (double tolerance : {0.0, 1e-13, .1}) {
    vdb::FluidViscosityOptions o;
    o.tolerance = tolerance;
    fail(o);
  }
  for (uint32_t iterations : {0u, 10001u}) {
    vdb::FluidViscosityOptions o;
    o.maxIterations = iterations;
    fail(o);
  }
  for (uint64_t count : {0ull, 1ull, 1'000'001ull}) {
    vdb::FluidViscosityOptions o;
    o.maxFluidCells = count;
    fail(o);
  }
  {
    vdb::FluidViscosityOptions o;
    o.temperatureGridName = o.densityGridName;
    fail(o);
  }
  {
    vdb::FluidViscosityOptions o;
    o.velocityGridName.assign(257, 'x');
    fail(o);
  }
  {
    vdb::ProcessingOptions p;
    p.maxVoxelCount = 4;
    fail({}, p);
  }
  {
    vdb::ProcessingOptions p;
    p.maxInputBytes = 64;
    fail({}, p);
  }
  {
    vdb::ProcessingOptions p;
    p.maxInputBytes = 16 * 1024 * 1024;
    fail({}, p);
  }
}

TEST_F(OpenVdbFluidViscosity,
       NoncanonicalValuesTransformsClassesAndOpenWallsAreRejected) {
  int serial = 0;
  const auto fail = [&](const State &source) {
    write("bad.vdb", source.grids());
    const auto out = file(("bad-" + std::to_string(serial++) + ".vdb").c_str());
    EXPECT_THROW((void)vdb::WriteViscousFluidState(file("bad.vdb"), out),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(out));
  };
  {
    auto s = make();
    s.density->tree().setValue({0, 0, 0}, -1);
    fail(s);
  }
  {
    auto s = make();
    s.temperature->tree().setValue({0, 0, 0}, 1e7);
    fail(s);
  }
  {
    auto s = make();
    s.pressure->tree().setValue({0, 0, 0},
                                std::numeric_limits<double>::quiet_NaN());
    fail(s);
  }
  {
    auto s = make();
    s.density->tree().setValueOff({0, 0, 0}, .1);
    fail(s);
  }
  {
    auto s = make();
    s.density->tree().setValue({20, 0, 0}, 1);
    fail(s);
  }
  {
    auto s = make();
    s.temperature->setGridClass(openvdb::GRID_LEVEL_SET);
    fail(s);
  }
  {
    auto s = make();
    s.velocity->setIsInWorldSpace(true);
    fail(s);
  }
  {
    auto s = make();
    face(*s.velocity, {0, 0, 0}, 0, 1);
    fail(s);
  }
  {
    auto s = make();
    face(*s.velocity, {1, 1, 0}, 0, std::numeric_limits<double>::infinity());
    fail(s);
  }
  {
    auto s = make();
    s.pressure->transform().postTranslate({1, 0, 0});
    fail(s);
  }
  {
    auto s = make();
    for (const auto &grid : s.grids())
      grid->transform().postScale(openvdb::Vec3d(1, 2, 1));
    fail(s);
  }
}

TEST_F(OpenVdbFluidViscosity,
       MissingStatisticsStreamingTypesAndDescriptorCollisionsFailClearly) {
  auto source = make();
  loop(source);
  write("no-stats.vdb", source.grids(), false);
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("no-stats.vdb"),
                                                 file("no-stats-out.vdb")),
               std::exception);
  {
    std::ofstream stream(file("stream.vdb"), std::ios::binary);
    openvdb::io::Stream output(stream);
    output.write(source.grids());
  }
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("stream.vdb"),
                                                 file("stream-out.vdb")),
               std::exception);
  auto grids = source.grids();
  grids[0] = openvdb::FloatGrid::create(0);
  grids[0]->setName("density");
  write("wrong-type.vdb", grids);
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("wrong-type.vdb"),
                                                 file("type-out.vdb")),
               std::exception);
  auto duplicate = source.density->deepCopy();
  grids = source.grids();
  grids.push_back(duplicate);
  write("duplicate.vdb", grids);
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("duplicate.vdb"),
                                                 file("duplicate-out.vdb")),
               std::exception);
  auto literal = source.density->deepCopy();
  literal->setName("density[0]");
  grids.push_back(literal);
  write("collision.vdb", grids);
  EXPECT_THROW((void)vdb::WriteViscousFluidState(file("collision.vdb"),
                                                 file("collision-out.vdb")),
               std::exception);
  for (const char *path : {"no-stats-out.vdb", "stream-out.vdb", "type-out.vdb",
                           "duplicate-out.vdb", "collision-out.vdb"})
    EXPECT_FALSE(std::filesystem::exists(file(path)));
}
#endif
} // namespace
