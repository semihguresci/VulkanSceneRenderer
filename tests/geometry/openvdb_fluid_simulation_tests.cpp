#include "Container/geometry/OpenVdbFluidSimulation.h"
#include "Container/geometry/OpenVdbFluidViscosity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbFluidSimulationAvailability,
     DisabledBuildFailsBeforePathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  vdb::FluidSimulationOptions options;
  options.timeStep = 0;
  options.substeps = 0;
  try {
    (void)vdb::WriteFluidStep({}, {}, options);
    FAIL() << "disabled smoke step must fail";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFluidSimulation : public ::testing::Test {
protected:
  using Cells = std::vector<openvdb::Coord>;
  struct State {
    openvdb::DoubleGrid::Ptr density, temperature, pressure;
    openvdb::Vec3DGrid::Ptr velocity;
    openvdb::BoolGrid::Ptr domain;
  };
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-vdb-smoke-" +
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
  static std::string bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }
  static Cells box(int x, int y, int z) {
    Cells cells;
    for (int i = 0; i < x; ++i)
      for (int j = 0; j < y; ++j)
        for (int k = 0; k < z; ++k)
          cells.emplace_back(i, j, k);
    return cells;
  }
  static openvdb::Coord axis(size_t direction, int amount = 1) {
    openvdb::Coord offset(0);
    offset[direction] = amount;
    return offset;
  }
  static openvdb::BoolGrid::Ptr mask(const Cells &cells, double h = 1.0,
                                     bool reflect = false) {
    auto result = openvdb::BoolGrid::create(false);
    result->setName("mask");
    auto transform = openvdb::math::Transform::createLinearTransform(h);
    if (reflect) {
      transform->postRotate(.37, openvdb::math::Z_AXIS);
      transform->postScale(openvdb::Vec3d(-1, 1, 1));
    }
    transform->postTranslate(openvdb::Vec3d(.5, -.25, .125));
    result->setTransform(transform);
    result->insertMeta("artist", openvdb::StringMetadata("domain metadata"));
    for (const auto &c : cells)
      result->tree().setValue(c, true);
    return result;
  }
  template <class Grid = openvdb::DoubleGrid>
  static typename Grid::Ptr scalar(const openvdb::BoolGrid &domain,
                                   const Cells &cells, double value,
                                   const char *name, bool fog) {
    auto result = Grid::create(typename Grid::ValueType(0));
    result->setName(name);
    result->setTransform(domain.transform().copy());
    result->setGridClass(fog ? openvdb::GRID_FOG_VOLUME
                             : openvdb::GRID_UNKNOWN);
    result->insertMeta(
        "artist", openvdb::StringMetadata(std::string(name) + " metadata"));
    for (const auto &c : cells)
      result->tree().setValue(c, typename Grid::ValueType(value));
    return result;
  }
  static openvdb::Vec3DGrid::Ptr mac(const openvdb::BoolGrid &domain) {
    auto result = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
    result->setName("wind");
    result->setTransform(domain.transform().copy());
    result->setGridClass(openvdb::GRID_STAGGERED);
    result->setIsInWorldSpace(false);
    result->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    result->insertMeta("artist", openvdb::StringMetadata("velocity metadata"));
    return result;
  }
  static void face(openvdb::Vec3DGrid &velocity, const openvdb::Coord &c,
                   size_t component, double value) {
    auto vector = velocity.tree().getValue(c);
    vector[component] = value;
    velocity.tree().setValue(c, vector);
  }
  static void circulation(openvdb::Vec3DGrid &velocity, const Cells &loop,
                          double speed) {
    for (size_t i = 0; i < loop.size(); ++i) {
      const auto from = loop[i], to = loop[(i + 1) % loop.size()];
      for (size_t component = 0; component < 3; ++component) {
        if (to == from + axis(component))
          face(velocity, to, component, speed);
        if (to == from - axis(component))
          face(velocity, from, component, -speed);
      }
    }
  }
  vdb::FluidSimulationInputs
  inputs(const openvdb::BoolGrid::Ptr &domain,
         const openvdb::GridBase::Ptr &density,
         const openvdb::Vec3DGrid::Ptr &velocity,
         const openvdb::GridBase::Ptr &temperature = {}) {
    write("domain.vdb", {domain});
    write("density.vdb", {density});
    write("velocity.vdb", {velocity});
    vdb::FluidSimulationInputs result;
    result.domain = file("domain.vdb");
    result.density = file("density.vdb");
    result.velocity = file("velocity.vdb");
    result.domainGrid = domain->getName();
    result.densityGrid = density->getName();
    result.velocityGrid = velocity->getName();
    if (temperature) {
      write("temperature.vdb", {temperature});
      result.temperature = file("temperature.vdb");
      result.temperatureGrid = temperature->getName();
    }
    return result;
  }
  static vdb::FluidSimulationInputs restart(const std::filesystem::path &path) {
    return {path,      path,       path,    path,
            "density", "velocity", "fluid", "temperature"};
  }
  State read(const char *name,
             const vdb::FluidSimulationOptions &options = {}) const {
    openvdb::io::File input(file(name).string());
    input.open(false);
    State result;
    result.density = openvdb::gridPtrCast<openvdb::DoubleGrid>(
        input.readGrid(options.densityGridName));
    result.temperature = openvdb::gridPtrCast<openvdb::DoubleGrid>(
        input.readGrid(options.temperatureGridName));
    result.velocity = openvdb::gridPtrCast<openvdb::Vec3DGrid>(
        input.readGrid(options.velocityGridName));
    result.pressure = openvdb::gridPtrCast<openvdb::DoubleGrid>(
        input.readGrid(options.pressureGridName));
    result.domain = openvdb::gridPtrCast<openvdb::BoolGrid>(
        input.readGrid(options.domainGridName));
    input.close();
    if (!result.density || !result.temperature || !result.velocity ||
        !result.pressure || !result.domain)
      throw std::runtime_error(
          "smoke output must contain the five documented grid types");
    return result;
  }
  static double maxDivergence(const openvdb::Vec3DGrid &velocity,
                              const Cells &cells) {
    double maximum = 0;
    for (const auto &c : cells) {
      double divergence = 0;
      for (size_t a = 0; a < 3; ++a)
        divergence += velocity.tree().getValue(c + axis(a))[a] -
                      velocity.tree().getValue(c)[a];
      maximum = std::max(maximum, std::abs(divergence));
    }
    return maximum;
  }
  static void closedWalls(const State &state, const Cells &cells) {
    for (const auto &c : cells)
      for (size_t a = 0; a < 3; ++a) {
        if (!state.domain->tree().getValue(c - axis(a)))
          EXPECT_DOUBLE_EQ(state.velocity->tree().getValue(c)[a], 0);
        if (!state.domain->tree().getValue(c + axis(a)))
          EXPECT_DOUBLE_EQ(state.velocity->tree().getValue(c + axis(a))[a], 0);
      }
  }
  static void nearFields(const State &a, const State &b, const Cells &cells,
                         double tolerance) {
    for (const auto &c : cells) {
      EXPECT_NEAR(a.density->tree().getValue(c), b.density->tree().getValue(c),
                  tolerance);
      EXPECT_NEAR(a.temperature->tree().getValue(c),
                  b.temperature->tree().getValue(c), tolerance);
      EXPECT_NEAR(a.pressure->tree().getValue(c),
                  b.pressure->tree().getValue(c), tolerance);
      EXPECT_EQ(a.domain->tree().getValue(c), b.domain->tree().getValue(c));
      for (size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(a.velocity->tree().getValue(c)[k],
                    b.velocity->tree().getValue(c)[k], tolerance);
        EXPECT_NEAR(a.velocity->tree().getValue(c + axis(k))[k],
                    b.velocity->tree().getValue(c + axis(k))[k], tolerance);
      }
    }
  }
  static void exactLoadedFields(const State &a, const State &b) {
    const std::array<std::pair<openvdb::DoubleGrid::Ptr, openvdb::DoubleGrid::Ptr>, 3>
        scalars{{{a.density, b.density}, {a.temperature, b.temperature}, {a.pressure, b.pressure}}};
    for (const auto &[x, y] : scalars) {
      ASSERT_TRUE(x); ASSERT_TRUE(y);
      EXPECT_TRUE(x->tree().hasSameTopology(y->tree()));
      EXPECT_EQ(x->transform(), y->transform());
      for (auto value = x->cbeginValueAll(); value; ++value) {
        EXPECT_EQ(value.isValueOn(), y->tree().isValueOn(value.getCoord()));
        EXPECT_EQ(std::bit_cast<uint64_t>(*value),
                  std::bit_cast<uint64_t>(y->tree().getValue(value.getCoord())));
      }
    }
    EXPECT_TRUE(a.velocity->tree().hasSameTopology(b.velocity->tree()));
    EXPECT_TRUE(a.domain->tree().hasSameTopology(b.domain->tree()));
    for (auto value = a.velocity->cbeginValueAll(); value; ++value)
      for (size_t axis = 0; axis < 3; ++axis)
        EXPECT_EQ(std::bit_cast<uint64_t>((*value)[axis]),
                  std::bit_cast<uint64_t>(b.velocity->tree().getValue(value.getCoord())[axis]));
    for (auto value = a.domain->cbeginValueAll(); value; ++value) {
      EXPECT_EQ(value.isValueOn(), b.domain->tree().isValueOn(value.getCoord()));
      EXPECT_EQ(*value, b.domain->tree().getValue(value.getCoord()));
    }
  }
  void noTemporaryState() const {
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      EXPECT_FALSE(entry.path().filename().string().starts_with(".container-")) << entry.path();
  }
  std::filesystem::path directory;
};

TEST_F(OpenVdbFluidSimulation,
       StaticConstantsPreserveWorldIntegralsAndReflectedTransform) {
  const auto cells = box(3, 3, 2);
  const auto domain = mask(cells, .5, true);
  const auto input = inputs(
      domain, scalar<openvdb::FloatGrid>(*domain, cells, 2.5, "fog", true),
      mac(*domain),
      scalar<openvdb::FloatGrid>(*domain, cells, 400, "heat", false));
  vdb::FluidSimulationOptions options;
  options.timeStep = .125;
  options.substeps = 3;
  const auto result = vdb::WriteFluidStep(input, file("static.vdb"), options);
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(result.fluidCellCount, cells.size());
  EXPECT_EQ(result.requestedSubsteps, 3u);
  EXPECT_EQ(result.actualSubsteps, 3u);
  EXPECT_NEAR(result.simulatedTime, .125, 1e-14);
  EXPECT_DOUBLE_EQ(result.maxWorldSpeedBefore, 0);
  EXPECT_DOUBLE_EQ(result.maxWorldSpeedAfter, 0);
  EXPECT_NEAR(result.densityMassBefore, 2.5 * cells.size() * .125, 1e-12);
  EXPECT_NEAR(result.densityMassAfter, result.densityMassBefore, 1e-12);
  EXPECT_NEAR(result.temperatureIntegralBefore, 400 * cells.size() * .125,
              1e-10);
  EXPECT_NEAR(result.temperatureIntegralAfter, result.temperatureIntegralBefore,
              1e-10);
  const auto state = read("static.vdb");
  EXPECT_EQ(state.density->transform(), domain->transform());
  EXPECT_EQ(state.velocity->transform(), domain->transform());
  EXPECT_EQ(state.density->getGridClass(), openvdb::GRID_FOG_VOLUME);
  EXPECT_EQ(state.velocity->getGridClass(), openvdb::GRID_STAGGERED);
  EXPECT_FALSE(state.velocity->isInWorldSpace());
  EXPECT_EQ(state.velocity->getVectorType(),
            openvdb::VEC_CONTRAVARIANT_RELATIVE);
  for (const auto &c : cells) {
    EXPECT_DOUBLE_EQ(state.density->tree().getValue(c), 2.5);
    EXPECT_DOUBLE_EQ(state.temperature->tree().getValue(c), 400);
    EXPECT_DOUBLE_EQ(state.pressure->tree().getValue(c), 0);
  }
  closedWalls(state, cells);
}

TEST_F(OpenVdbFluidSimulation,
       OmittedTemperatureInitializesAmbientAndVacuumStaysVacuum) {
  const auto cells = box(2, 2, 2);
  const auto domain = mask(cells);
  const auto input =
      inputs(domain, scalar(*domain, cells, 0, "fog", true), mac(*domain));
  vdb::FluidSimulationOptions options;
  options.timeStep = .1;
  options.ambientTemperature = 310;
  const auto result = vdb::WriteFluidStep(input, file("vacuum.vdb"), options);
  const auto state = read("vacuum.vdb");
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(result.actualSubsteps, 1u);
  EXPECT_DOUBLE_EQ(result.densityMassBefore, 0);
  EXPECT_DOUBLE_EQ(result.densityMassAfter, 0);
  EXPECT_NEAR(result.temperatureIntegralAfter, 310 * cells.size(), 1e-10);
  for (const auto &c : cells) {
    EXPECT_DOUBLE_EQ(state.density->tree().getValue(c), 0);
    EXPECT_DOUBLE_EQ(state.temperature->tree().getValue(c), 310);
    for (size_t a = 0; a < 3; ++a)
      EXPECT_DOUBLE_EQ(state.velocity->tree().getValue(c)[a], 0);
  }
  EXPECT_DOUBLE_EQ(state.temperature->background(), 0);
  EXPECT_DOUBLE_EQ(state.temperature->tree().getValue({20, 20, 20}), 0);
}

TEST_F(OpenVdbFluidSimulation,
       LocalSolenoidalFlowTransportsBlobAndCorrelatedTemperature) {
  const auto cells = box(6, 6, 1);
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  circulation(*velocity, {{2, 2, 0}, {3, 2, 0}, {3, 3, 0}, {2, 3, 0}}, 1);
  auto density = scalar(*domain, cells, 0, "fog", true),
       temperature = scalar(*domain, cells, 300, "heat", false);
  density->tree().setValue({2, 2, 0}, 1);
  temperature->tree().setValue({2, 2, 0}, 400);
  const auto input = inputs(domain, density, velocity, temperature);
  vdb::FluidSimulationOptions options;
  options.timeStep = .2;
  options.ambientTemperature = 300;
  options.tolerance = 1e-9;
  const auto result = vdb::WriteFluidStep(input, file("blob.vdb"), options);
  const auto state = read("blob.vdb");
  EXPECT_TRUE(result.converged);
  EXPECT_GT(result.maxWorldSpeedBefore, 0);
  EXPECT_LT(state.density->tree().getValue({2, 2, 0}), 1);
  EXPECT_GT(state.density->tree().getValue({3, 2, 0}), 0);
  for (const auto &c : cells) {
    const double value = state.density->tree().getValue(c);
    EXPECT_GE(value, 0);
    EXPECT_LE(value, 1);
    EXPECT_NEAR(state.temperature->tree().getValue(c) - 300, 100 * value, 1e-9);
  }
  EXPECT_LE(maxDivergence(*state.velocity, cells), 1e-8);
  closedWalls(state, cells);
}

TEST_F(OpenVdbFluidSimulation,
       ConstantsSurviveNonuniformSelfAdvectionAtClosedWalls) {
  const auto cells = box(4, 4, 1);
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  circulation(*velocity, {{1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}}, 2);
  const auto input =
      inputs(domain, scalar(*domain, cells, 1e6, "fog", true), velocity,
             scalar(*domain, cells, 1e6, "heat", false));
  vdb::FluidSimulationOptions options;
  options.timeStep = .2;
  options.substeps = 2;
  const auto result =
      vdb::WriteFluidStep(input, file("constant-flow.vdb"), options);
  const auto state = read("constant-flow.vdb");
  EXPECT_TRUE(result.converged);
  for (const auto &c : cells) {
    EXPECT_DOUBLE_EQ(state.density->tree().getValue(c), 1e6);
    EXPECT_DOUBLE_EQ(state.temperature->tree().getValue(c), 1e6);
  }
  closedWalls(state, cells);
  options.timeStep = .05;
  options.substeps = 1;
  EXPECT_NO_THROW((void)vdb::WriteFluidStep(restart(file("constant-flow.vdb")),
                                            file("constant-restart.vdb"),
                                            options));
  const auto resumed = read("constant-restart.vdb");
  for (const auto &c : cells) {
    EXPECT_DOUBLE_EQ(resumed.density->tree().getValue(c), 1e6);
    EXPECT_DOUBLE_EQ(resumed.temperature->tree().getValue(c), 1e6);
  }
}

TEST_F(OpenVdbFluidSimulation,
       SpatialDensityAndHeatProduceOppositeBuoyancyAndProjectedFlow) {
  const auto cells = box(4, 4, 1);
  const auto domain = mask(cells);
  auto heavy = scalar(*domain, cells, 0, "fog", true);
  heavy->tree().setValue({1, 2, 0}, 1);
  const auto denseInput = inputs(domain, heavy, mac(*domain),
                                 scalar(*domain, cells, 300, "heat", false));
  vdb::FluidSimulationOptions options;
  options.timeStep = .1;
  options.ambientTemperature = 300;
  options.buoyancyDensity = 1;
  options.tolerance = 1e-9;
  const auto denseResult =
      vdb::WriteFluidStep(denseInput, file("heavy.vdb"), options);
  const auto dense = read("heavy.vdb");
  auto hot = scalar(*domain, cells, 300, "heat", false);
  hot->tree().setValue({1, 2, 0}, 400);
  const auto hotInput =
      inputs(domain, scalar(*domain, cells, 0, "fog", true), mac(*domain), hot);
  options.buoyancyDensity = 0;
  options.buoyancyTemperature = .01;
  const auto hotResult =
      vdb::WriteFluidStep(hotInput, file("hot.vdb"), options);
  const auto heated = read("hot.vdb");
  for (const auto &result : {denseResult, hotResult}) {
    EXPECT_TRUE(result.converged);
    EXPECT_GT(result.maxDivergenceBefore, 0);
    EXPECT_GT(result.maxWorldSpeedAfter, 1e-6);
    EXPECT_LE(result.maxDivergenceAfter,
              result.maxDivergenceBefore * options.tolerance);
  }
  EXPECT_LT(dense.velocity->tree().getValue({1, 2, 0})[1] +
                dense.velocity->tree().getValue({1, 3, 0})[1],
            0);
  for (const auto &c : cells)
    for (size_t a = 0; a < 3; ++a)
      EXPECT_NEAR(dense.velocity->tree().getValue(c)[a],
                  -heated.velocity->tree().getValue(c)[a], 1e-9);
  closedWalls(dense, cells);
  closedWalls(heated, cells);
}

TEST_F(OpenVdbFluidSimulation,
       UniformGravityCreatesHydrostaticPressureInReflectedWorldFrame) {
  const auto cells = box(2, 2, 2);
  const auto domain = mask(cells, .5, true);
  const auto input =
      inputs(domain, scalar(*domain, cells, 0, "fog", true), mac(*domain));
  vdb::FluidSimulationOptions options;
  options.timeStep = .2;
  options.gravity = {.75, -.5, 1.25};
  options.tolerance = 1e-9;
  const auto result = vdb::WriteFluidStep(input, file("gravity.vdb"), options);
  const auto state = read("gravity.vdb");
  EXPECT_TRUE(result.converged);
  EXPECT_GT(result.maxDivergenceBefore, 0);
  const openvdb::Vec3d gravity(.75, -.5, 1.25),
      origin = domain->indexToWorld(cells.front().asVec3d());
  for (const auto &c : cells) {
    EXPECT_NEAR(state.pressure->tree().getValue(c) -
                    state.pressure->tree().getValue(cells.front()),
                gravity.dot(domain->indexToWorld(c.asVec3d()) - origin), 1e-8);
  }
  EXPECT_LT(result.maxWorldSpeedAfter, 1e-8);
  closedWalls(state, cells);
}

TEST_F(OpenVdbFluidSimulation,
       AdaptiveCflSubdividesAndActualStepBudgetFailsWithoutOutput) {
  const auto cells = box(4, 4, 1);
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  circulation(*velocity, {{1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}}, 8);
  const auto input =
      inputs(domain, scalar(*domain, cells, 1, "fog", true), velocity);
  vdb::FluidSimulationOptions options;
  options.timeStep = .2;
  const auto result = vdb::WriteFluidStep(input, file("adaptive.vdb"), options);
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(result.requestedSubsteps, 1u);
  EXPECT_GT(result.actualSubsteps, 1u);
  EXPECT_LE(result.actualSubsteps, options.maxSubsteps);
  EXPECT_NEAR(result.simulatedTime, .2, 1e-13);
  options.maxSubsteps = 1;
  EXPECT_THROW(
      (void)vdb::WriteFluidStep(input, file("over-budget.vdb"), options),
      std::exception);
  EXPECT_FALSE(std::filesystem::exists(file("over-budget.vdb")));
}

TEST_F(OpenVdbFluidSimulation,
       CflBoundaryRoundoffRedistributesWithoutAnUnsupportedFinalSliver) {
  const auto cells = box(4, 4, 1);
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  circulation(*velocity, {{1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}}, 1);
  const auto input =
      inputs(domain, scalar(*domain, cells, 0, "fog", true), velocity);
  vdb::FluidSimulationOptions options;
  options.timeStep = std::nextafter(.5, std::numeric_limits<double>::infinity());
  options.maxCfl = .5;
  const auto result = vdb::WriteFluidStep(input, file("cfl-boundary.vdb"), options);
  EXPECT_TRUE(result.converged);
  EXPECT_GE(result.actualSubsteps, 2u);
  EXPECT_LE(result.actualSubsteps, options.maxSubsteps);
  EXPECT_NEAR(result.simulatedTime, options.timeStep, 1e-15);
  closedWalls(read("cfl-boundary.vdb"), cells);
}

TEST_F(OpenVdbFluidSimulation,
       TinyCflForceBoundCannotUnderflowIntoAnUnrestrictedStep) {
  const auto cells = box(2, 1, 1);
  const auto domain = mask(cells);
  const auto input =
      inputs(domain, scalar(*domain, cells, 0, "fog", true), mac(*domain));
  vdb::FluidSimulationOptions options;
  options.timeStep = .1;
  options.maxCfl = 1e-300;
  options.gravity = {1e-200, 0, 0};
  // sqrt(CFL/acceleration) is about 1e-50 seconds, so no supported actual
  // timestep can meet this CFL, even though CFL*acceleration underflows.
  EXPECT_THROW(
      (void)vdb::WriteFluidStep(input, file("tiny-cfl.vdb"), options),
      std::exception);
  EXPECT_FALSE(std::filesystem::exists(file("tiny-cfl.vdb")));
}

TEST_F(OpenVdbFluidSimulation,
       TwoRequestedStepsMatchTwoRestartCallsWithIdenticalActualDt) {
  const auto cells = box(4, 4, 1);
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  circulation(*velocity, {{1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}}, 1);
  auto density = scalar<openvdb::FloatGrid>(*domain, cells, 0, "fog", true);
  density->tree().setValue({1, 1, 0}, 1);
  const auto input = inputs(domain, density, velocity,
                            scalar(*domain, cells, 315, "heat", false));
  vdb::FluidSimulationOptions combined;
  combined.timeStep = .1;
  combined.substeps = 2;
  combined.tolerance = 1e-9;
  const auto whole = vdb::WriteFluidStep(input, file("whole.vdb"), combined);
  auto single = combined;
  single.timeStep = .05;
  single.substeps = 1;
  const auto first = vdb::WriteFluidStep(input, file("half.vdb"), single);
  const auto second = vdb::WriteFluidStep(restart(file("half.vdb")),
                                          file("restarted.vdb"), single);
  EXPECT_EQ(whole.actualSubsteps, 2u);
  EXPECT_EQ(first.actualSubsteps, 1u);
  EXPECT_EQ(second.actualSubsteps, 1u);
  nearFields(read("whole.vdb"), read("restarted.vdb"), cells, 1e-11);
}

TEST_F(OpenVdbFluidSimulation,
       ObstacleRingAndDiagonalCornersRemainClosedWithoutSolidPopulation) {
  const Cells cells{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {2, 1, 0},
                    {2, 2, 0}, {1, 2, 0}, {0, 2, 0}, {0, 1, 0}};
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  circulation(*velocity, cells, 1);
  auto density = scalar(*domain, cells, 0, "fog", true);
  density->tree().setValue({0, 1, 0}, 1);
  const auto input = inputs(domain, density, velocity,
                            scalar(*domain, cells, 300, "heat", false));
  vdb::FluidSimulationOptions options;
  options.timeStep = .25;
  const auto result = vdb::WriteFluidStep(input, file("obstacle.vdb"), options);
  const auto state = read("obstacle.vdb");
  EXPECT_TRUE(result.converged);
  closedWalls(state, cells);
  EXPECT_FALSE(state.domain->tree().getValue({1, 1, 0}));
  EXPECT_DOUBLE_EQ(state.density->tree().getValue({1, 1, 0}), 0);
  EXPECT_DOUBLE_EQ(state.temperature->tree().getValue({1, 1, 0}), 0);
  EXPECT_DOUBLE_EQ(state.density->tree().getValue({2, 1, 0}), 0);
  for (const auto &c : cells) {
    EXPECT_GE(state.density->tree().getValue(c), 0);
    EXPECT_LE(state.density->tree().getValue(c), 1);
    EXPECT_NEAR(state.temperature->tree().getValue(c), 300, 1e-10);
  }
}

TEST_F(OpenVdbFluidSimulation,
       DiagonallyAdjacentDisconnectedRegionsDoNotExchangeScalarStencilWeights) {
  const Cells first{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
  const Cells second{{2, 2, 0}, {3, 2, 0}, {2, 3, 0}, {3, 3, 0}};
  auto cells = first;
  cells.insert(cells.end(), second.begin(), second.end());
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  // Both interior faces leave the lower-left cell of the second component.
  // Its scalar backtrace stays inside that cell, but the trilinear stencil
  // reaches the diagonally adjacent first component across two solid cells.
  face(*velocity, {3, 2, 0}, 0, 1);
  face(*velocity, {2, 3, 0}, 1, 1);
  auto density = scalar(*domain, cells, 0, "fog", true),
       temperature = scalar(*domain, cells, 300, "heat", false);
  for (const auto &c : first) {
    density->tree().setValue(c, 1);
    temperature->tree().setValue(c, 400);
  }
  const auto input = inputs(domain, density, velocity, temperature);
  vdb::FluidSimulationOptions options;
  options.timeStep = .2;
  const auto result = vdb::WriteFluidStep(input, file("diagonal.vdb"), options);
  const auto state = read("diagonal.vdb");
  EXPECT_TRUE(result.converged);
  closedWalls(state, cells);
  for (const auto &c : first) {
    EXPECT_NEAR(state.density->tree().getValue(c), 1, 1e-12);
    EXPECT_NEAR(state.temperature->tree().getValue(c), 400, 1e-10);
  }
  for (const auto &c : second) {
    EXPECT_DOUBLE_EQ(state.density->tree().getValue(c), 0);
    EXPECT_NEAR(state.temperature->tree().getValue(c), 300, 1e-10);
  }
}

TEST_F(OpenVdbFluidSimulation,
       RemoteCorridorDoesNotAllowScalarStencilLeakageAcrossLocalSolidCorner) {
  const Cells first{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
  const Cells second{{2, 2, 0}, {3, 2, 0}, {2, 3, 0}, {3, 3, 0}};
  auto cells = first;
  cells.insert(cells.end(), second.begin(), second.end());
  const Cells corridor{{-1, 0, 0},  {-2, 0, 0}, {-2, -1, 0}, {-2, -2, 0},
                       {-1, -2, 0}, {0, -2, 0}, {1, -2, 0},  {2, -2, 0},
                       {3, -2, 0},  {4, -2, 0}, {5, -2, 0},  {5, -1, 0},
                       {5, 0, 0},   {5, 1, 0},  {5, 2, 0},   {4, 2, 0}};
  cells.insert(cells.end(), corridor.begin(), corridor.end());
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  face(*velocity, {3, 2, 0}, 0, 1);
  face(*velocity, {2, 3, 0}, 1, 1);
  auto density = scalar(*domain, cells, 0, "fog", true),
       temperature = scalar(*domain, cells, 300, "heat", false);
  for (const auto &c : first) {
    density->tree().setValue(c, 1);
    temperature->tree().setValue(c, 400);
  }
  const auto input = inputs(domain, density, velocity, temperature);
  vdb::FluidSimulationOptions options;
  options.timeStep = .2;
  const auto result = vdb::WriteFluidStep(input, file("corridor.vdb"), options);
  const auto state = read("corridor.vdb");
  EXPECT_TRUE(result.converged);
  closedWalls(state, cells);
  // One connected component reaches the hot lobe through the remote corridor.
  // A component-label filter alone would still sample diagonally through these
  // two local solid cells; straight-line stencil visibility must reject it.
  EXPECT_FALSE(state.domain->tree().getValue({1, 2, 0}));
  EXPECT_FALSE(state.domain->tree().getValue({2, 1, 0}));
  for (const auto &c : second) {
    EXPECT_DOUBLE_EQ(state.density->tree().getValue(c), 0);
    EXPECT_NEAR(state.temperature->tree().getValue(c), 300, 1e-10);
  }
}

TEST_F(OpenVdbFluidSimulation,
       TripleCornerVisibilityChecksPairwiseSolidEdgeCells) {
  const Cells corridor{{0, 1, 1},  {0, 0, 1},  {0, 0, 0},  {0, 0, -1},
                       {1, 0, -1}, {2, 0, -1}, {3, 0, -1}, {4, 0, -1},
                       {4, 1, -1}, {4, 2, -1}, {4, 2, 0},  {4, 2, 1},
                       {4, 2, 2},  {3, 2, 2}};
  Cells cells{{1, 1, 1}, {2, 2, 2}, {1, 2, 2}, {2, 1, 2}, {2, 2, 1}};
  cells.insert(cells.end(), corridor.begin(), corridor.end());
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  for (size_t a = 0; a < 3; ++a)
    face(*velocity, {2, 2, 2}, a, 2);
  auto density = scalar(*domain, cells, 0, "fog", true),
       temperature = scalar(*domain, cells, 300, "heat", false);
  density->tree().setValue({1, 1, 1}, 1);
  temperature->tree().setValue({1, 1, 1}, 400);
  const auto input = inputs(domain, density, velocity, temperature);
  vdb::FluidSimulationOptions options;
  options.timeStep = .1;
  const auto result =
      vdb::WriteFluidStep(input, file("triple-corner.vdb"), options);
  const auto state = read("triple-corner.vdb");
  EXPECT_TRUE(result.converged);
  closedWalls(state, cells);
  // All three face-neighbor cells and the final body diagonal are fluid.
  // Only the pairwise edge cells block the simultaneous corner crossing.
  for (const openvdb::Coord c : Cells{{1, 1, 2}, {1, 2, 1}, {2, 1, 1}})
    EXPECT_FALSE(state.domain->tree().getValue(c));
  EXPECT_DOUBLE_EQ(state.density->tree().getValue({2, 2, 2}), 0);
  EXPECT_NEAR(state.temperature->tree().getValue({2, 2, 2}), 300, 1e-10);
}

TEST_F(OpenVdbFluidSimulation,
       BoolAndFogDomainStructuralMetadataIsCanonicalized) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  for (bool fogSource : {false, true}) {
    auto input =
        inputs(domain, scalar(*domain, cells, 1, "fog", true), mac(*domain));
    openvdb::GridBase::Ptr source =
        fogSource ? openvdb::GridBase::Ptr(scalar<openvdb::FloatGrid>(
                        *domain, cells, 1, "mask", true))
                  : openvdb::GridBase::Ptr(domain->deepCopy());
    source->setGridClass(openvdb::GRID_FOG_VOLUME);
    source->setVectorType(openvdb::VEC_COVARIANT);
    source->setIsInWorldSpace(false);
    source->insertMeta("container:mac_layout",
                       openvdb::StringMetadata("stale"));
    source->insertMeta("container:velocity_units",
                       openvdb::StringMetadata("stale"));
    source->insertMeta("container:projection_dt", openvdb::DoubleMetadata(42));
    source->insertMeta("container:pressure_gauge",
                       openvdb::StringMetadata("stale"));
    source->insertMeta("container:pressure_units",
                       openvdb::StringMetadata("stale"));
    write("domain.vdb", {source});
    const char *name = fogSource ? "fog-domain.vdb" : "bool-domain.vdb";
    const auto result = vdb::WriteFluidStep(input, file(name));
    const auto state = read(name);
    EXPECT_TRUE(result.converged);
    EXPECT_EQ(state.domain->getGridClass(), openvdb::GRID_UNKNOWN);
    EXPECT_EQ(state.domain->getVectorType(), openvdb::VEC_INVARIANT);
    EXPECT_TRUE(state.domain->isInWorldSpace());
    EXPECT_EQ(state.domain->metaValue<std::string>("artist"),
              source->metaValue<std::string>("artist"));
    for (const char *key :
         {"container:mac_layout", "container:velocity_units",
          "container:projection_dt", "container:pressure_gauge",
          "container:pressure_units"})
      EXPECT_FALSE((*state.domain)[key]);
  }
}

TEST_F(OpenVdbFluidSimulation,
       CustomNamesAndFieldMetadataSurviveWithoutMutatingSourceFiles) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  const auto input =
      inputs(domain, scalar(*domain, cells, 1, "fog", true), mac(*domain),
             scalar(*domain, cells, 300, "heat", false));
  const auto density = bytes(input.density), velocity = bytes(input.velocity),
             domainBytes = bytes(input.domain),
             temperature = bytes(input.temperature);
  vdb::FluidSimulationOptions options;
  options.densityGridName = "smoke";
  options.temperatureGridName = "kelvin";
  options.velocityGridName = "mac";
  options.pressureGridName = "potential";
  options.domainGridName = "container";
  (void)vdb::WriteFluidStep(input, file("named.vdb"), options);
  const auto state = read("named.vdb", options);
  EXPECT_EQ(state.density->metaValue<std::string>("artist"), "fog metadata");
  EXPECT_EQ(state.temperature->metaValue<std::string>("artist"),
            "heat metadata");
  EXPECT_EQ(state.velocity->metaValue<std::string>("artist"),
            "velocity metadata");
  EXPECT_EQ(state.domain->metaValue<std::string>("artist"), "domain metadata");
  EXPECT_EQ(state.pressure->metaValue<std::string>("artist"),
            "domain metadata");
  EXPECT_EQ(bytes(input.density), density);
  EXPECT_EQ(bytes(input.velocity), velocity);
  EXPECT_EQ(bytes(input.domain), domainBytes);
  EXPECT_EQ(bytes(input.temperature), temperature);
  openvdb::io::File output(file("named.vdb").string());
  output.open(false);
  size_t count = 0;
  for (auto name = output.beginName(); name != output.endName(); ++name)
    ++count;
  EXPECT_EQ(count, 5u);
  output.close();
}

TEST_F(OpenVdbFluidSimulation,
       ExistingOutputsAndEveryInputAliasRemainUntouched) {
  const auto cells = box(2, 1, 1);
  const auto domain = mask(cells);
  const auto input =
      inputs(domain, scalar(*domain, cells, 1, "fog", true), mac(*domain),
             scalar(*domain, cells, 300, "heat", false));
  {
    std::ofstream output(file("existing.vdb"));
    output << "keep existing output";
  }
  for (const auto &output : {file("existing.vdb"), input.density,
                             input.velocity, input.domain, input.temperature}) {
    const auto before = bytes(output);
    EXPECT_THROW((void)vdb::WriteFluidStep(input, output), std::exception);
    EXPECT_EQ(bytes(output), before);
  }
  EXPECT_THROW((void)vdb::WriteFluidStep(input, file("absent/child.vdb")),
               std::exception);
}

TEST_F(OpenVdbFluidSimulation,
       InvalidOptionsAndTooSmallActualDtDoNotPublishOutput) {
  const auto cells = box(2, 1, 1);
  const auto domain = mask(cells);
  const auto input =
      inputs(domain, scalar(*domain, cells, 1, "fog", true), mac(*domain));
  const auto fails = [&](const vdb::FluidSimulationOptions &options) {
    EXPECT_THROW((void)vdb::WriteFluidStep(input, file("invalid.vdb"), options),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(file("invalid.vdb")));
  };
  for (double dt :
       {0.0, 1e-7, 3601.0, std::numeric_limits<double>::quiet_NaN()}) {
    vdb::FluidSimulationOptions o;
    o.timeStep = dt;
    fails(o);
  }
  for (double cfl : {0.0, .6, std::numeric_limits<double>::quiet_NaN()}) {
    vdb::FluidSimulationOptions o;
    o.maxCfl = cfl;
    fails(o);
  }
  for (uint32_t steps : {0u, 65u}) {
    vdb::FluidSimulationOptions o;
    o.substeps = steps;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.substeps = 2;
    o.maxSubsteps = 1;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.maxSubsteps = 65;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.timeStep = 1e-6;
    o.substeps = 2;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.buoyancyDensity = -1;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.buoyancyTemperature = 1e7;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.ambientTemperature = -1;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.buoyancyDirection = {0, 0, 0};
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.buoyancyDirection = {0, 2, 0};
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.gravity = {1e13, 0, 0};
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.maxIterations = 0;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.tolerance = 1e-13;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.maxFluidCells = 1;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.pressureGridName = o.densityGridName;
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.temperatureGridName.clear();
    fails(o);
  }
  {
    vdb::FluidSimulationOptions o;
    o.velocityGridName.assign(257, 'x');
    fails(o);
  }
}

TEST_F(OpenVdbFluidSimulation,
       EncodedCellAndWorkspaceBudgetsFailBeforeFinalOutput) {
  const auto cells = box(3, 3, 2);
  const auto domain = mask(cells);
  const auto input =
      inputs(domain, scalar(*domain, cells, 1, "fog", true), mac(*domain));
  for (int variant = 0; variant < 4; ++variant) {
    vdb::ProcessingOptions options;
    if (variant == 0)
      options.maxInputBytes = 1;
    if (variant == 1)
      options.maxInputBytes = 4 * 1024 * 1024;
    if (variant == 2)
      options.maxVoxelCount = 1;
    if (variant == 3)
      options.maxVoxelCount = 0;
    EXPECT_THROW(
        (void)vdb::WriteFluidStep(input, file("bounded.vdb"), {}, options),
        std::exception);
    EXPECT_FALSE(std::filesystem::exists(file("bounded.vdb")));
  }
}

TEST_F(OpenVdbFluidSimulation,
       InvalidScalarRangesBackgroundInactiveAndSolidValuesAreRejected) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  for (int variant = 0; variant < 7; ++variant) {
    auto density = scalar(*domain, cells, 1, "fog", true);
    if (variant == 0)
      density->setGridClass(openvdb::GRID_UNKNOWN);
    if (variant == 1)
      density->tree().setValue({0, 0, 0}, -1);
    if (variant == 2)
      density->tree().setValue({0, 0, 0}, 1e7);
    if (variant == 3)
      density->tree().setValue({0, 0, 0},
                               std::numeric_limits<double>::quiet_NaN());
    if (variant == 4)
      density->tree().root().setBackground(1, false);
    if (variant == 5)
      density->tree().setValueOff({8, 0, 0}, 1);
    if (variant == 6)
      density->tree().setValue({8, 0, 0}, 1);
    const auto input = inputs(domain, density, mac(*domain));
    EXPECT_THROW((void)vdb::WriteFluidStep(input, file("bad-scalar.vdb")),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(file("bad-scalar.vdb")));
  }
  auto temperature = scalar(*domain, cells, 300, "heat", false);
  temperature->tree().setValue({0, 0, 0}, -1);
  auto input = inputs(domain, scalar(*domain, cells, 1, "fog", true),
                      mac(*domain), temperature);
  EXPECT_THROW((void)vdb::WriteFluidStep(input, file("bad-temperature.vdb")),
               std::exception);
  EXPECT_FALSE(std::filesystem::exists(file("bad-temperature.vdb")));
}

TEST_F(OpenVdbFluidSimulation,
       MismatchedTransformsAndNonNativeVelocityAreRejected) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  for (int variant = 0; variant < 5; ++variant) {
    auto density = scalar(*domain, cells, 1, "fog", true);
    auto velocity = mac(*domain);
    if (variant == 0)
      density->transform().postTranslate({1, 0, 0});
    if (variant == 1)
      velocity->transform().postTranslate({1, 0, 0});
    if (variant == 2)
      velocity->setGridClass(openvdb::GRID_UNKNOWN);
    if (variant == 3)
      velocity->setIsInWorldSpace(true);
    if (variant == 4)
      velocity->tree().setValue(
          {0, 0, 0},
          openvdb::Vec3d(std::numeric_limits<double>::infinity(), 0, 0));
    const auto input = inputs(domain, density, velocity);
    EXPECT_THROW((void)vdb::WriteFluidStep(input, file("bad-frame.vdb")),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(file("bad-frame.vdb")));
  }
  {
    const auto input = inputs(domain, domain, mac(*domain));
    EXPECT_THROW((void)vdb::WriteFluidStep(input, file("bool-density.vdb")),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(file("bool-density.vdb")));
  }
  {
    auto input =
        inputs(domain, scalar(*domain, cells, 1, "fog", true), mac(*domain));
    write("velocity.vdb", {scalar(*domain, cells, 0, "wind", false)});
    EXPECT_THROW((void)vdb::WriteFluidStep(input, file("scalar-velocity.vdb")),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(file("scalar-velocity.vdb")));
  }
  {
    const auto input = inputs(domain, scalar(*domain, cells, 1, "fog", true),
                              mac(*domain), domain);
    EXPECT_THROW((void)vdb::WriteFluidStep(input, file("bool-temperature.vdb")),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(file("bool-temperature.vdb")));
  }
}

TEST_F(OpenVdbFluidSimulation,
       InternalPressureNonconvergenceDoesNotPublishOrModifyInputs) {
  const auto cells = box(6, 5, 3);
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  const auto potential = [](const openvdb::Coord &c) {
    return std::sin(.31 * c.x()) + std::cos(.73 * c.y()) + c.z() * c.z() +
           .17 * c.x() * c.y() * c.y();
  };
  for (const auto &c : cells)
    for (size_t a = 0; a < 3; ++a)
      if (domain->tree().getValue(c - axis(a)))
        face(*velocity, c, a, potential(c) - potential(c - axis(a)));
  const auto input =
      inputs(domain, scalar(*domain, cells, 1, "fog", true), velocity);
  const auto beforeDensity = bytes(input.density),
             beforeVelocity = bytes(input.velocity),
             beforeDomain = bytes(input.domain);
  vdb::FluidSimulationOptions options;
  options.timeStep = .01;
  options.substeps = 2;
  options.maxIterations = 1;
  options.tolerance = 1e-12;
  EXPECT_THROW(
      (void)vdb::WriteFluidStep(input, file("nonconverged.vdb"), options),
      std::exception);
  EXPECT_FALSE(std::filesystem::exists(file("nonconverged.vdb")));
  EXPECT_EQ(bytes(input.density), beforeDensity);
  EXPECT_EQ(bytes(input.velocity), beforeVelocity);
  EXPECT_EQ(bytes(input.domain), beforeDomain);
}

TEST_F(OpenVdbFluidSimulation,
       GridSelectionAndSeekableRecordedMetadataAreRequired) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  const auto density = scalar(*domain, cells, 1, "fog", true);
  const auto input = inputs(domain, density, mac(*domain));
  auto missing = input;
  missing.densityGrid = "missing";
  EXPECT_THROW((void)vdb::WriteFluidStep(missing, file("missing.vdb")),
               std::exception);
  write("density.vdb", {density, density->deepCopy()});
  EXPECT_THROW((void)vdb::WriteFluidStep(input, file("ambiguous.vdb")),
               std::exception);
  openvdb::io::File catalog(file("density.vdb").string());
  catalog.open(false);
  const auto descriptor = catalog.beginName().gridName();
  catalog.close();
  auto selected = input;
  selected.densityGrid = "descriptor:" + descriptor;
  EXPECT_NO_THROW((void)vdb::WriteFluidStep(selected, file("selected.vdb")));
  write("density.vdb", {density}, false);
  EXPECT_THROW((void)vdb::WriteFluidStep(input, file("no-stats.vdb")),
               std::exception);
  {
    std::ofstream stream(file("density.vdb"), std::ios::binary);
    openvdb::io::Stream archive(stream);
    archive.write(openvdb::GridPtrVec{density});
  }
  EXPECT_THROW((void)vdb::WriteFluidStep(input, file("stream.vdb")),
               std::exception);
  for (const auto *name :
       {"missing.vdb", "ambiguous.vdb", "no-stats.vdb", "stream.vdb"})
    EXPECT_FALSE(std::filesystem::exists(file(name)));
}

TEST_F(OpenVdbFluidSimulation,
       DisabledCompositionFeaturesRetainExactStaticTransportForceProjectionFields) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  const auto density = scalar(*domain, cells, 1.25, "density", true);
  const auto temperature = scalar(*domain, cells, 300, "temperature", false);
  const auto input = inputs(domain, density, mac(*domain), temperature);
  vdb::FluidSimulationOptions options;
  options.timeStep = .125; options.gravity = {.5, -.25, 0}; options.tolerance = 1e-9;
  const auto result = vdb::WriteFluidStep(input, file("default-composition.vdb"), options);
  vdb::FluidProjectionOptions projection;
  projection.timeStep = options.timeStep; projection.acceleration = options.gravity;
  projection.tolerance = options.tolerance;
  (void)vdb::WriteProjectedVelocity(input.velocity, input.domain, file("legacy-projection.vdb"),
      projection, input.velocityGrid, input.domainGrid);
  openvdb::io::File legacyFile(file("legacy-projection.vdb").string());
  legacyFile.open(false);
  State legacy{density, temperature,
      openvdb::gridPtrCast<openvdb::DoubleGrid>(legacyFile.readGrid("pressure")),
      openvdb::gridPtrCast<openvdb::Vec3DGrid>(legacyFile.readGrid("velocity")),
      openvdb::gridPtrCast<openvdb::BoolGrid>(legacyFile.readGrid("fluid"))};
  legacyFile.close();
  exactLoadedFields(legacy, read("default-composition.vdb"));
  EXPECT_EQ(result.viscositySubsteps, 0u); EXPECT_EQ(result.sourceSubsteps, 0u);
  EXPECT_EQ(result.maxDiffusionIterations, 0u);
  EXPECT_DOUBLE_EQ(result.viscosityEnergyDissipated, 0);
  EXPECT_DOUBLE_EQ(result.sourceDensityMassChange, 0);
  EXPECT_DOUBLE_EQ(result.sourceTemperatureIntegralChange, 0);
  noTemporaryState();
}

TEST_F(OpenVdbFluidSimulation,
       CoupledViscosityDampsTransportedCirculationByTheKnownFaceEigenvalue) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells, .5);
  auto velocity = mac(*domain);
  circulation(*velocity, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, 1);
  const auto input = inputs(domain, scalar(*domain, cells, 2, "fog", true), velocity,
                            scalar(*domain, cells, 300, "heat", false));
  vdb::FluidSimulationOptions inviscid;
  inviscid.timeStep = .125; inviscid.tolerance = 1e-9;
  const auto old = vdb::WriteFluidStep(input, file("transported.vdb"), inviscid);
  auto options = inviscid; options.kinematicViscosity = .5;
  const auto result = vdb::WriteFluidStep(input, file("damped.vdb"), options);
  const auto before = read("transported.vdb"), after = read("damped.vdb");
  // Every face of this four-cell circulation is the antisymmetric two-node
  // face mode: six-point -L has eigenvalue7, with r=nu*dt/h^2=.25.
  const double attenuation = 1 / 2.75;
  EXPECT_EQ(result.actualSubsteps, 1u); EXPECT_EQ(result.viscositySubsteps, 1u);
  EXPECT_EQ(result.sourceSubsteps, 0u); EXPECT_GT(result.maxDiffusionIterations, 0u);
  EXPECT_LE(result.maxDiffusionResidual, options.tolerance);
  EXPECT_GT(result.viscosityEnergyDissipated, 0);
  EXPECT_LT(result.maxWorldSpeedAfter, old.maxWorldSpeedAfter);
  double energy = 0;
  for (const auto &c : cells) {
    EXPECT_DOUBLE_EQ(after.density->tree().getValue(c), 2);
    EXPECT_DOUBLE_EQ(after.temperature->tree().getValue(c), 300);
    for (size_t a = 0; a < 3; ++a) {
      const double value = before.velocity->tree().getValue(c)[a];
      EXPECT_NEAR(after.velocity->tree().getValue(c)[a], value * attenuation, 2e-9);
      if (domain->tree().getValue(c - axis(a))) energy += .5 * std::pow(.5, 5) * value * value;
    }
  }
  EXPECT_NEAR(result.viscosityEnergyDissipated, energy * (1 - attenuation * attenuation), 1e-9);
  EXPECT_LE(maxDivergence(*after.velocity, cells), 1e-8);
  closedWalls(after, cells); noTemporaryState();
}

TEST_F(OpenVdbFluidSimulation,
       AdaptiveViscosityMatchesManualPrimitiveCompositionAtEachActualTimeStep) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  circulation(*velocity, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, 8);
  const auto input = inputs(domain, scalar(*domain, cells, 1, "fog", true), velocity,
                            scalar(*domain, cells, 300, "heat", false));
  vdb::FluidSimulationOptions options;
  options.timeStep = .2; options.kinematicViscosity = 100; options.tolerance = 1e-9;
  const auto coupled = vdb::WriteFluidStep(input, file("adaptive-viscosity.vdb"), options);
  EXPECT_EQ(coupled.actualSubsteps, 2u); EXPECT_EQ(coupled.viscositySubsteps, 2u);
  // First CFL partition: .2 / ceil(.2/(.5/8)) = .05. Large viscosity then
  // damps speed below .25, so all remaining .15 seconds fit the next step.
  auto transport = options; transport.kinematicViscosity = 0; transport.timeStep = .05;
  const auto first = vdb::WriteFluidStep(input, file("manual-transport1.vdb"), transport);
  EXPECT_EQ(first.actualSubsteps, 1u);
  vdb::FluidViscosityOptions diffusion;
  diffusion.timeStep = .05; diffusion.kinematicViscosity = 100; diffusion.tolerance = options.tolerance;
  const auto firstDiffusion = vdb::WriteViscousFluidState(file("manual-transport1.vdb"),
      file("manual-diffusion1.vdb"), diffusion);
  transport.timeStep = .15;
  const auto second = vdb::WriteFluidStep(restart(file("manual-diffusion1.vdb")),
      file("manual-transport2.vdb"), transport);
  EXPECT_EQ(second.actualSubsteps, 1u);
  diffusion.timeStep = .15;
  const auto secondDiffusion = vdb::WriteViscousFluidState(file("manual-transport2.vdb"),
      file("manual-diffusion2.vdb"), diffusion);
  // Projection and diffusion commute for this solenoidal symmetric mode.
  nearFields(read("adaptive-viscosity.vdb"), read("manual-diffusion2.vdb"), cells, 1e-8);
  EXPECT_NEAR(coupled.viscosityEnergyDissipated,
      firstDiffusion.kineticEnergyBefore - firstDiffusion.kineticEnergyAfterDiffusion +
      secondDiffusion.kineticEnergyBefore - secondDiffusion.kineticEnergyAfterDiffusion, 1e-8);
  EXPECT_NEAR(read("adaptive-viscosity.vdb").velocity->metaValue<double>("container:projection_dt"), .15, 1e-15);
  closedWalls(read("adaptive-viscosity.vdb"), cells); noTemporaryState();
}

TEST_F(OpenVdbFluidSimulation,
       SourcesLastUseExactScalarOdeAndComposeTwoHalfStepsAndRestart) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  const auto input = inputs(domain, scalar(*domain, cells, 1, "fog", true), mac(*domain),
                            scalar(*domain, cells, 350, "heat", false));
  vdb::FluidSourceSphere source; source.radius = 10;
  source.densityRate = 4; source.densityDecayRate = 2;
  source.temperatureRate = 10; source.temperatureRelaxationRate = .5;
  vdb::FluidSimulationOptions options;
  options.timeStep = .2; options.substeps = 2; options.ambientTemperature = 300; options.sources = {source};
  const auto result = vdb::WriteFluidStep(input, file("source-two-halves.vdb"), options);
  auto single = options; single.timeStep = .1; single.substeps = 1;
  const auto first = vdb::WriteFluidStep(input, file("source-half.vdb"), single);
  const auto second = vdb::WriteFluidStep(restart(file("source-half.vdb")), file("source-restart.vdb"), single);
  const auto output = read("source-two-halves.vdb");
  const double density = 2 - std::exp(-.4), temperature = 320 + 30 * std::exp(-.1);
  EXPECT_EQ(result.sourceSubsteps, 2u); EXPECT_EQ(result.viscositySubsteps, 0u);
  EXPECT_EQ(result.sourceAffectedCellApplications, 8u);
  EXPECT_EQ(result.sourceChangedCellApplications, 8u);
  EXPECT_EQ(first.sourceSubsteps, 1u); EXPECT_EQ(second.sourceSubsteps, 1u);
  for (const auto &c : cells) {
    EXPECT_NEAR(output.density->tree().getValue(c), density, 1e-12);
    EXPECT_NEAR(output.temperature->tree().getValue(c), temperature, 1e-12);
    EXPECT_EQ(output.velocity->tree().getValue(c), openvdb::Vec3d(0));
  }
  EXPECT_NEAR(result.sourceDensityMassChange, 4 * (density - 1), 1e-11);
  EXPECT_NEAR(result.sourceTemperatureIntegralChange, 4 * (temperature - 350), 1e-10);
  exactLoadedFields(output, read("source-restart.vdb")); noTemporaryState();
}

TEST_F(OpenVdbFluidSimulation,
       SourcesAffectOnlyTheNextBuoyancyStepWhoseCflIsRecomputed) {
  const auto cells = box(4, 4, 1);
  const auto domain = mask(cells);
  const auto input = inputs(domain, scalar(*domain, cells, 0, "fog", true), mac(*domain),
                            scalar(*domain, cells, 300, "heat", false));
  vdb::FluidSourceSphere source; source.radius = .1; source.densityRate = 100;
  const auto center = domain->indexToWorld(openvdb::Vec3d(1, 2, 0));
  source.center = {center.x(), center.y(), center.z()};
  vdb::FluidSimulationOptions options;
  options.timeStep = .1; options.ambientTemperature = 300;
  options.buoyancyDensity = 100; options.sources = {source}; options.tolerance = 1e-9;
  const auto first = vdb::WriteFluidStep(input, file("emitted-first.vdb"), options);
  const auto emitted = read("emitted-first.vdb");
  EXPECT_EQ(first.actualSubsteps, 1u); EXPECT_DOUBLE_EQ(first.maxWorldSpeedAfter, 0);
  EXPECT_DOUBLE_EQ(emitted.density->tree().getValue({1, 2, 0}), 10);
  const auto next = vdb::WriteFluidStep(restart(file("emitted-first.vdb")), file("buoyant-next.vdb"), options);
  EXPECT_GT(next.actualSubsteps, 1u); EXPECT_GT(next.maxWorldSpeedAfter, 1e-4);
  auto combined = options; combined.timeStep = .2; combined.substeps = 2;
  combined.maxSubsteps = 2;
  EXPECT_THROW((void)vdb::WriteFluidStep(input, file("stale-force-cfl.vdb"), combined), std::exception);
  EXPECT_FALSE(std::filesystem::exists(file("stale-force-cfl.vdb")));
  combined.maxSubsteps = 64;
  const auto both = vdb::WriteFluidStep(input, file("buoyant-combined.vdb"), combined);
  EXPECT_EQ(both.actualSubsteps, first.actualSubsteps + next.actualSubsteps);
  EXPECT_EQ(both.sourceSubsteps, both.actualSubsteps);
  nearFields(read("buoyant-combined.vdb"), read("buoyant-next.vdb"), cells, 1e-8);
  EXPECT_LE(maxDivergence(*read("buoyant-combined.vdb").velocity, cells),
      both.maxDivergenceBefore * combined.tolerance + both.divergenceRoundoffBound + 1e-12);
  closedWalls(read("buoyant-combined.vdb"), cells); noTemporaryState();
}

TEST_F(OpenVdbFluidSimulation,
       BothFeaturesRetainLiteralNamesMetadataActualDtAndSmallSourceIntegralChanges) {
  const auto cells = box(3, 3, 1);
  const auto domain = mask(cells, .3);
  auto velocity = mac(*domain);
  circulation(*velocity, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, .25);
  const auto input = inputs(domain, scalar(*domain, cells, 1e6, "fog", true), velocity,
                            scalar(*domain, cells, 500000, "heat", false));
  const auto densityBytes = bytes(input.density), velocityBytes = bytes(input.velocity);
  vdb::FluidSourceSphere source; source.radius = 10; source.densityDecayRate = 1e-13;
  source.temperatureRate = 1e-8;
  vdb::FluidSimulationOptions options;
  options.timeStep = .1; options.substeps = 2; options.kinematicViscosity = .2;
  options.sources = {source}; options.tolerance = 1e-9;
  options.densityGridName = "descriptor:smoke"; options.temperatureGridName = "kelvin";
  options.velocityGridName = "descriptor:wind"; options.pressureGridName = "potential";
  options.domainGridName = "walls";
  const auto result = vdb::WriteFluidStep(input, file("combined-named.vdb"), options);
  const auto output = read("combined-named.vdb", options);
  EXPECT_EQ(result.actualSubsteps, 2u); EXPECT_EQ(result.viscositySubsteps, 2u);
  EXPECT_EQ(result.sourceSubsteps, 2u); EXPECT_EQ(result.sourceAffectedCellApplications, 18u);
  EXPECT_GT(result.viscosityEnergyDissipated, 0);
  EXPECT_EQ(output.density->metaValue<std::string>("artist"), "fog metadata");
  EXPECT_EQ(output.temperature->metaValue<std::string>("artist"), "heat metadata");
  EXPECT_EQ(output.velocity->metaValue<std::string>("artist"), "velocity metadata");
  EXPECT_EQ(output.pressure->metaValue<std::string>("artist"), "domain metadata");
  EXPECT_EQ(output.domain->metaValue<std::string>("artist"), "domain metadata");
  EXPECT_DOUBLE_EQ(output.velocity->metaValue<double>("container:projection_dt"), .05);
  const double volume = .3 * .3 * .3 * cells.size();
  const double expectedMass = (output.density->tree().getValue(cells.front()) - 1e6) * volume;
  const double expectedHeat = (output.temperature->tree().getValue(cells.front()) - 500000) * volume;
  ASSERT_LT(expectedMass, 0); ASSERT_GT(expectedHeat, 0);
  EXPECT_NEAR(result.sourceDensityMassChange, expectedMass, std::abs(expectedMass) * 1e-12);
  EXPECT_NEAR(result.sourceTemperatureIntegralChange, expectedHeat, std::abs(expectedHeat) * 1e-12);
  EXPECT_EQ(bytes(input.density), densityBytes); EXPECT_EQ(bytes(input.velocity), velocityBytes);
  closedWalls(output, cells); noTemporaryState();
}

TEST_F(OpenVdbFluidSimulation,
       CompositionValidationStiffFailureAndWorkspaceFailureLeaveNoOutputOrTemporaryState) {
  const auto cells = box(2, 2, 1);
  const auto domain = mask(cells);
  auto velocity = mac(*domain);
  circulation(*velocity, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, .25);
  const auto input = inputs(domain, scalar(*domain, cells, 2, "fog", true), velocity,
                            scalar(*domain, cells, 350, "heat", false));
  const auto originalDensity = bytes(input.density), originalVelocity = bytes(input.velocity);
  int serial = 0;
  const auto fail = [&](const vdb::FluidSimulationOptions &options, const vdb::ProcessingOptions &p = {}) {
    const auto name = "composition-failure-" + std::to_string(serial++) + ".vdb";
    EXPECT_THROW((void)vdb::WriteFluidStep(input, file(name.c_str()), options, p), std::exception);
    EXPECT_FALSE(std::filesystem::exists(file(name.c_str()))); noTemporaryState();
  };
  for (double nu : {-1.0, 1e6 + 1, std::numeric_limits<double>::quiet_NaN()}) {
    vdb::FluidSimulationOptions o; o.kinematicViscosity = nu; fail(o);
  }
  { vdb::FluidSimulationOptions o; o.sources.resize(65); fail(o); }
  { vdb::FluidSimulationOptions o; o.sources.resize(1); o.sources[0].temperatureRate = -1; fail(o); }
  { vdb::FluidSimulationOptions o; o.sources.resize(1); o.sources[0].center[0] = std::numeric_limits<double>::infinity(); fail(o); }
  { vdb::FluidSimulationOptions o; o.timeStep = .1; o.kinematicViscosity = .1;
    vdb::FluidSourceSphere source; source.radius = 10; source.densityDecayRate = 1e6; o.sources = {source}; fail(o); }
  { vdb::FluidSimulationOptions o; o.timeStep = .1; o.kinematicViscosity = .1;
    vdb::ProcessingOptions p; p.maxInputBytes = 32 * 1024 * 1024; fail(o, p); }
  EXPECT_EQ(bytes(input.density), originalDensity); EXPECT_EQ(bytes(input.velocity), originalVelocity);
  // The stored density source is representable, but its physical mass delta
  // is not. The coupled diagnostic contract rejects that lost nonzero work.
  const Cells tinyCells{{0, 0, 0}};
  const auto tinyDomain = mask(tinyCells, 1.5e-5);
  const auto tinyInput = inputs(tinyDomain, scalar(*tinyDomain, tinyCells, 0, "fog", true), mac(*tinyDomain));
  vdb::FluidSimulationOptions tinyOptions; tinyOptions.timeStep = 1;
  vdb::FluidSourceSphere tinySource; tinySource.radius = 1e-6;
  const auto tinyCenter = tinyDomain->indexToWorld(openvdb::Vec3d(0));
  tinySource.center = {tinyCenter.x(), tinyCenter.y(), tinyCenter.z()};
  tinySource.densityRate = std::numeric_limits<double>::denorm_min();
  tinyOptions.sources = {tinySource};
  EXPECT_THROW((void)vdb::WriteFluidStep(tinyInput, file("tiny-source-integral.vdb"), tinyOptions), std::exception);
  EXPECT_FALSE(std::filesystem::exists(file("tiny-source-integral.vdb"))); noTemporaryState();
}
#endif
} // namespace
