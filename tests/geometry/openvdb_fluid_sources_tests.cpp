#include "Container/geometry/OpenVdbFluidSources.h"

#include <algorithm>
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
#include <type_traits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbFluidSourcesAvailability, DisabledFailsBeforePathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  vdb::FluidSourcesOptions options;
  options.timeStep = 0;
  options.sources.resize(65);
  try {
    (void)vdb::WriteFluidSources("missing.vdb", {}, options);
    FAIL() << "disabled source editing must fail";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFluidSources : public ::testing::Test {
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
    directory = std::filesystem::temp_directory_path() /
        ("container-sources-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path file(const std::string &name) const {
    return directory / name;
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             bool statistics = true) const {
    openvdb::io::File output(file(name).string());
    output.setGridStatsMetadataEnabled(statistics);
    output.write(grids);
    output.close();
  }
  static std::string bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
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
      transform->postTranslate(openvdb::Vec3d(.5, -.25, .125));
    }
    state.domain->setTransform(transform);
    state.domain->setName("fluid");
    state.domain->setGridClass(openvdb::GRID_UNKNOWN);
    state.density = openvdb::DoubleGrid::create(0);
    state.temperature = openvdb::DoubleGrid::create(0);
    state.pressure = openvdb::DoubleGrid::create(0);
    state.velocity = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
    state.density->setName("density");
    state.temperature->setName("temperature");
    state.pressure->setName("pressure");
    state.velocity->setName("velocity");
    state.density->setGridClass(openvdb::GRID_FOG_VOLUME);
    state.temperature->setGridClass(openvdb::GRID_UNKNOWN);
    state.pressure->setGridClass(openvdb::GRID_UNKNOWN);
    state.velocity->setGridClass(openvdb::GRID_STAGGERED);
    state.velocity->setIsInWorldSpace(false);
    state.velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    state.velocity->insertMeta("container:mac_layout",
                              openvdb::StringMetadata("negative_faces"));
    state.velocity->insertMeta("container:velocity_units",
                              openvdb::StringMetadata("voxels_per_second"));
    for (const auto &grid : {state.density, state.temperature, state.pressure}) {
      grid->setTransform(transform->copy());
      grid->setIsInWorldSpace(true);
      grid->setVectorType(openvdb::VEC_INVARIANT);
    }
    state.velocity->setTransform(transform->copy());
    for (const auto &grid : state.grids()) {
      grid->insertMeta("artist", openvdb::StringMetadata(grid->getName() + " artist"));
      grid->insertMeta("test:precise", openvdb::DoubleMetadata(.1234567891234567));
      grid->insertMeta("test:counter", openvdb::Int64Metadata(1234567890123));
    }
    for (int x = 0; x < nx; ++x)
      for (int y = 0; y < ny; ++y)
        for (int z = 0; z < nz; ++z) {
          const openvdb::Coord c(x, y, z);
          state.domain->tree().setValue(c, true);
          state.density->tree().setValue(c, 2);
          state.temperature->tree().setValue(c, 350);
          state.pressure->tree().setValue(c, x - .5 * y + .125 * z);
          state.velocity->tree().setValue(c, openvdb::Vec3d(0));
        }
    return state;
  }
  static void face(State &state, const openvdb::Coord &c, size_t axis,
                   double value) {
    auto velocity = state.velocity->tree().getValue(c);
    velocity[axis] = value;
    state.velocity->tree().setValue(c, velocity);
  }
  static void circulation(State &state) {
    face(state, {1, 0, 0}, 0, .375);
    face(state, {1, 1, 0}, 1, .375);
    face(state, {1, 1, 0}, 0, -.375);
    face(state, {0, 1, 0}, 1, -.375);
  }
  State read(const std::string &name,
             const vdb::FluidSourcesOptions &options = {}) const {
    openvdb::io::File input(file(name).string());
    input.open(false);
    State state{openvdb::gridPtrCast<openvdb::DoubleGrid>(input.readGrid(options.densityGridName)),
                openvdb::gridPtrCast<openvdb::DoubleGrid>(input.readGrid(options.temperatureGridName)),
                openvdb::gridPtrCast<openvdb::DoubleGrid>(input.readGrid(options.pressureGridName)),
                openvdb::gridPtrCast<openvdb::Vec3DGrid>(input.readGrid(options.velocityGridName)),
                openvdb::gridPtrCast<openvdb::BoolGrid>(input.readGrid(options.domainGridName))};
    input.close();
    return state;
  }
  static vdb::FluidSourceSphere sphere(double radius = 10) {
    vdb::FluidSourceSphere source;
    source.radius = radius;
    return source;
  }
  static void sameMetadata(const openvdb::GridBase &a,
                           const openvdb::GridBase &b) {
    EXPECT_EQ(a.getName(), b.getName());
    EXPECT_EQ(a.getGridClass(), b.getGridClass());
    EXPECT_EQ(a.getVectorType(), b.getVectorType());
    EXPECT_EQ(a.isInWorldSpace(), b.isInWorldSpace());
    EXPECT_EQ(a.transform(), b.transform());
    EXPECT_EQ(a.metaValue<std::string>("artist"), b.metaValue<std::string>("artist"));
    EXPECT_EQ(a.metaValue<int64_t>("test:counter"), b.metaValue<int64_t>("test:counter"));
    EXPECT_EQ(std::bit_cast<uint64_t>(a.metaValue<double>("test:precise")),
              std::bit_cast<uint64_t>(b.metaValue<double>("test:precise")));
  }
  template <class Grid> static void sameGrid(const Grid &a, const Grid &b) {
    sameMetadata(a, b);
    EXPECT_TRUE(a.tree().hasSameTopology(b.tree()));
    EXPECT_EQ(a.activeVoxelCount(), b.activeVoxelCount());
    EXPECT_EQ(a.background(), b.background());
    for (auto value = a.cbeginValueAll(); value; ++value) {
      const auto actual = b.tree().getValue(value.getCoord());
      EXPECT_EQ(value.isValueOn(), b.tree().isValueOn(value.getCoord()));
      if constexpr (std::is_same_v<typename Grid::ValueType, double>) {
        EXPECT_EQ(std::bit_cast<uint64_t>(*value), std::bit_cast<uint64_t>(actual));
      } else if constexpr (std::is_same_v<typename Grid::ValueType, openvdb::Vec3d>) {
        for (size_t axis = 0; axis < 3; ++axis)
          EXPECT_EQ(std::bit_cast<uint64_t>((*value)[axis]),
                    std::bit_cast<uint64_t>(actual[axis]));
      } else {
        EXPECT_EQ(*value, actual);
      }
    }
  }
  static void preservedNativeFields(const State &before, const State &after) {
    ASSERT_TRUE(after.velocity); ASSERT_TRUE(after.pressure); ASSERT_TRUE(after.domain);
    sameGrid(*before.velocity, *after.velocity);
    sameGrid(*before.pressure, *after.pressure);
    sameGrid(*before.domain, *after.domain);
    EXPECT_EQ(after.velocity->metaValue<std::string>("container:mac_layout"), "negative_faces");
    EXPECT_EQ(after.velocity->metaValue<std::string>("container:velocity_units"), "voxels_per_second");
  }
  void failure(const std::string &input, const vdb::FluidSourcesOptions &options = {},
               const vdb::ProcessingOptions &processing = {}) {
    const auto output = file("failure-" + std::to_string(failureSerial++) + ".vdb");
    EXPECT_THROW((void)vdb::WriteFluidSources(file(input), output, options, processing),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(output));
  }
  std::filesystem::path directory;
  int failureSerial{0};
};

TEST_F(OpenVdbFluidSources, NoOpRetainsInactiveSignedZeroNativeFields) {
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
  (void)vdb::WriteFluidSources(file("signed-zero.vdb"), file("preserved.vdb"));
  const auto after = read("preserved.vdb");
  sameGrid(*before.density, *after.density);
  sameGrid(*before.temperature, *after.temperature);
  preservedNativeFields(before, after);
  EXPECT_TRUE(std::signbit(after.density->tree().getValue(remote)));
  EXPECT_TRUE(std::signbit(after.temperature->tree().getValue(remote)));
  EXPECT_TRUE(std::signbit(after.pressure->tree().getValue(remote)));
  EXPECT_TRUE(std::signbit(after.velocity->tree().getValue(remote)[0]));
  EXPECT_FALSE(std::signbit(after.velocity->tree().getValue(remote)[1]));
  EXPECT_TRUE(std::signbit(after.velocity->tree().getValue(remote)[2]));
}

TEST_F(OpenVdbFluidSources, OverlappingRatesHaveKnownDensityHalfLifeAndKelvinRelaxation) {
  auto state = make();
  circulation(state);
  write("state.vdb", state.grids());
  const auto original = bytes(file("state.vdb"));
  auto emitter = sphere();
  emitter.densityRate = 2; emitter.densityDecayRate = .5;
  emitter.temperatureRate = 10; emitter.temperatureRelaxationRate = .25;
  vdb::FluidSourcesOptions options;
  options.timeStep = std::log(2.0); options.ambientTemperature = 300;
  options.sources = {emitter, emitter};
  const auto result = vdb::WriteFluidSources(file("state.vdb"), file("edited.vdb"), options);
  const auto edited = read("edited.vdb");
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(result.fluidCellCount, 4u); EXPECT_EQ(result.affectedCellCount, 4u);
  EXPECT_EQ(result.changedCellCount, 4u);
  EXPECT_EQ(result.clearedDensityCells, 0u); EXPECT_EQ(result.resetTemperatureCells, 0u);
  // The combined density equilibrium is 4 and one density half-life elapsed.
  // Temperature equilibrium is 340 and its decay factor is 1/sqrt(2).
  for (auto value = state.domain->cbeginValueOn(); value; ++value) {
    EXPECT_NEAR(edited.density->tree().getValue(value.getCoord()), 3, 2e-12);
    EXPECT_NEAR(edited.temperature->tree().getValue(value.getCoord()),
                340 + 10 / std::sqrt(2.0), 2e-12);
  }
  EXPECT_NEAR(result.densityMassBefore, 8, 1e-12);
  EXPECT_NEAR(result.densityMassAfter, 12, 1e-11);
  EXPECT_NEAR(result.temperatureIntegralAfter, 4 * (340 + 10 / std::sqrt(2.0)), 1e-10);
  sameMetadata(*state.density, *edited.density);
  sameMetadata(*state.temperature, *edited.temperature);
  preservedNativeFields(state, edited);
  EXPECT_EQ(bytes(file("state.vdb")), original);
}

TEST_F(OpenVdbFluidSources, PureInjectionAndAmbientRelaxationUseWorldVolumeDiagnostics) {
  auto state = make(1, 1, 1, .5);
  state.density->tree().setValue({0, 0, 0}, 0);
  state.temperature->tree().setValue({0, 0, 0}, 500);
  write("state.vdb", state.grids());
  auto emitter = sphere(); emitter.densityRate = 8;
  emitter.temperatureRelaxationRate = 2;
  vdb::FluidSourcesOptions options;
  options.timeStep = .25; options.ambientTemperature = 300; options.sources = {emitter};
  const auto result = vdb::WriteFluidSources(file("state.vdb"), file("injected.vdb"), options);
  const auto output = read("injected.vdb");
  EXPECT_DOUBLE_EQ(output.density->tree().getValue({0, 0, 0}), 2);
  EXPECT_NEAR(output.temperature->tree().getValue({0, 0, 0}), 300 + 200 / std::sqrt(std::exp(1.0)), 1e-12);
  EXPECT_DOUBLE_EQ(result.densityMassAfter, .25);
  EXPECT_DOUBLE_EQ(result.temperatureIntegralBefore, 62.5);
  EXPECT_NEAR(result.temperatureIntegralAfter,
              output.temperature->tree().getValue({0, 0, 0}) / 8, 1e-12);
}

TEST_F(OpenVdbFluidSources, ConstantCoefficientExactOdeComposesAcrossRestartTimesteps) {
  auto state = make(1, 1, 1);
  state.density->tree().setValue({0, 0, 0}, 8);
  state.temperature->tree().setValue({0, 0, 0}, 500);
  write("state.vdb", state.grids());
  auto emitter = sphere(); emitter.densityRate = 3; emitter.densityDecayRate = .75;
  emitter.temperatureRate = 4; emitter.temperatureRelaxationRate = .2;
  vdb::FluidSourcesOptions options;
  options.timeStep = .6; options.ambientTemperature = 300; options.sources = {emitter};
  (void)vdb::WriteFluidSources(file("state.vdb"), file("full.vdb"), options);
  options.timeStep = .3;
  (void)vdb::WriteFluidSources(file("state.vdb"), file("half.vdb"), options);
  (void)vdb::WriteFluidSources(file("half.vdb"), file("two-halves.vdb"), options);
  const auto full = read("full.vdb"), halves = read("two-halves.vdb");
  EXPECT_NEAR(full.density->tree().getValue({0, 0, 0}), 4 + 4 * std::exp(-.45), 1e-12);
  EXPECT_NEAR(full.temperature->tree().getValue({0, 0, 0}), 320 + 180 * std::exp(-.12), 1e-12);
  EXPECT_NEAR(full.density->tree().getValue({0, 0, 0}), halves.density->tree().getValue({0, 0, 0}), 1e-12);
  EXPECT_NEAR(full.temperature->tree().getValue({0, 0, 0}), halves.temperature->tree().getValue({0, 0, 0}), 1e-12);
  preservedNativeFields(state, halves);
}

TEST_F(OpenVdbFluidSources, SourceOrderIsBitExactForDisparateOverlappingRates) {
  auto state = make(1, 1, 1); write("state.vdb", state.grids());
  vdb::FluidSourcesOptions options; options.timeStep = .1;
  auto large = sphere(); large.densityRate = 300000; large.temperatureRate = 400000;
  options.sources.push_back(large);
  for (unsigned index = 0; index < 31; ++index) {
    auto small = sphere(); small.densityRate = (index % 3 + 1) * 1e-11;
    small.temperatureRate = (index % 5 + 1) * 1e-11;
    small.densityDecayRate = (index % 2 + 1) * 1e-7;
    small.temperatureRelaxationRate = (index % 4 + 1) * 1e-7;
    options.sources.push_back(small);
  }
  (void)vdb::WriteFluidSources(file("state.vdb"), file("reference.vdb"), options);
  const auto reference = read("reference.vdb");
  for (int permutation = 0; permutation < 4; ++permutation) {
    if (permutation % 2 == 0) std::reverse(options.sources.begin(), options.sources.end());
    else std::rotate(options.sources.begin(), options.sources.begin() + 7, options.sources.end());
    const auto name = "permutation-" + std::to_string(permutation) + ".vdb";
    (void)vdb::WriteFluidSources(file("state.vdb"), file(name), options);
    const auto output = read(name);
    sameGrid(*reference.density, *output.density);
    sameGrid(*reference.temperature, *output.temperature);
  }
}

TEST_F(OpenVdbFluidSources, InclusiveWorldCellCentersExcludeActiveFalseInactiveAndOutsideCells) {
  auto state = make(3, 1, 1);
  state.domain->tree().setValue({3, 0, 0}, false);
  state.domain->tree().setValueOff({4, 0, 0}, true);
  write("state.vdb", state.grids());
  auto emitter = sphere(2); emitter.center = {2, 0, 0}; emitter.densityRate = 2;
  vdb::FluidSourcesOptions options; options.timeStep = .5; options.sources = {emitter};
  const auto result = vdb::WriteFluidSources(file("state.vdb"), file("membership.vdb"), options);
  const auto output = read("membership.vdb");
  EXPECT_EQ(result.fluidCellCount, 3u); EXPECT_EQ(result.affectedCellCount, 3u);
  for (int x = 0; x < 3; ++x) EXPECT_DOUBLE_EQ(output.density->tree().getValue({x, 0, 0}), 3);
  for (int x : {-1, 3, 4, 5}) EXPECT_DOUBLE_EQ(output.density->tree().getValue({x, 0, 0}), 0);
  EXPECT_TRUE(output.domain->tree().isValueOn({3, 0, 0}));
  EXPECT_FALSE(output.domain->tree().getValue({3, 0, 0}));
  EXPECT_FALSE(output.domain->tree().isValueOn({4, 0, 0}));
  EXPECT_TRUE(output.domain->tree().getValue({4, 0, 0}));
  preservedNativeFields(state, output);
}

TEST_F(OpenVdbFluidSources, RotatedReflectedNonunitVoxelsUseSourceWorldSphereDistances) {
  int serial = 0;
  for (double h : {.25, 2.0}) {
    auto state = make(3, 1, 1, h, true);
    write("state.vdb", state.grids());
    auto emitter = sphere(.75 * h);
    const auto world = state.domain->indexToWorld(openvdb::Vec3d(1, 0, 0));
    emitter.center = {world.x(), world.y(), world.z()}; emitter.densityRate = 4;
    vdb::FluidSourcesOptions options; options.timeStep = .25; options.sources = {emitter};
    const auto name = "world-" + std::to_string(serial++) + ".vdb";
    const auto result = vdb::WriteFluidSources(file("state.vdb"), file(name), options);
    const auto output = read(name);
    EXPECT_EQ(result.affectedCellCount, 1u);
    EXPECT_DOUBLE_EQ(output.density->tree().getValue({0, 0, 0}), 2);
    EXPECT_DOUBLE_EQ(output.density->tree().getValue({1, 0, 0}), 3);
    EXPECT_DOUBLE_EQ(output.density->tree().getValue({2, 0, 0}), 2);
    EXPECT_NEAR(result.densityMassAfter - result.densityMassBefore, h * h * h, 1e-12);
    preservedNativeFields(state, output);
  }
}

TEST_F(OpenVdbFluidSources, ClearAndResetPrecedeCombinedRatesIndependentlyOfSourceOrder) {
  auto state = make(2, 1, 1); write("state.vdb", state.grids());
  auto reset = sphere(.25); reset.clearDensity = true; reset.resetTemperature = true;
  auto rates = sphere(.25); rates.densityRate = 4; rates.densityDecayRate = 2;
  rates.temperatureRate = 10; rates.temperatureRelaxationRate = .5;
  vdb::FluidSourcesOptions options;
  options.timeStep = .25; options.ambientTemperature = 300; options.sources = {rates, reset};
  const auto result = vdb::WriteFluidSources(file("state.vdb"), file("cleared.vdb"), options);
  const auto output = read("cleared.vdb");
  EXPECT_EQ(result.affectedCellCount, 1u); EXPECT_EQ(result.changedCellCount, 1u);
  EXPECT_EQ(result.clearedDensityCells, 1u); EXPECT_EQ(result.resetTemperatureCells, 1u);
  EXPECT_NEAR(output.density->tree().getValue({0, 0, 0}), 2 * (1 - std::exp(-.5)), 1e-12);
  EXPECT_NEAR(output.temperature->tree().getValue({0, 0, 0}), 320 - 20 * std::exp(-.125), 1e-12);
  EXPECT_DOUBLE_EQ(output.density->tree().getValue({1, 0, 0}), 2);
  EXPECT_DOUBLE_EQ(output.temperature->tree().getValue({1, 0, 0}), 350);
  std::reverse(options.sources.begin(), options.sources.end());
  (void)vdb::WriteFluidSources(file("state.vdb"), file("reordered-clear.vdb"), options);
  const auto reordered = read("reordered-clear.vdb");
  sameGrid(*output.density, *reordered.density);
  sameGrid(*output.temperature, *reordered.temperature);
}

TEST_F(OpenVdbFluidSources, StiffNonzeroTermsFailWhileExplicitClearResetCanAnnihilateThem) {
  auto state = make(1, 1, 1); write("state.vdb", state.grids());
  auto stiff = sphere(); stiff.densityDecayRate = 1e6;
  vdb::FluidSourcesOptions options;
  options.timeStep = 1; options.ambientTemperature = 300; options.sources = {stiff};
  failure("state.vdb", options);
  options.sources[0].densityDecayRate = 0;
  options.sources[0].temperatureRelaxationRate = 1e6;
  failure("state.vdb", options);
  options.sources[0].densityDecayRate = 1e6;
  options.sources[0].clearDensity = true; options.sources[0].resetTemperature = true;
  const auto result = vdb::WriteFluidSources(file("state.vdb"), file("intentional-reset.vdb"), options);
  const auto output = read("intentional-reset.vdb");
  EXPECT_TRUE(result.converged);
  EXPECT_DOUBLE_EQ(output.density->tree().getValue({0, 0, 0}), 0);
  EXPECT_DOUBLE_EQ(output.temperature->tree().getValue({0, 0, 0}), 300);
  EXPECT_EQ(result.clearedDensityCells, 1u); EXPECT_EQ(result.resetTemperatureCells, 1u);
}

TEST_F(OpenVdbFluidSources, NoSourcesCopiesEveryLoadedFieldTopologyValueAndCustomMetadataExactly) {
  auto state = make(); circulation(state);
  for (const auto &grid : {state.density, state.temperature, state.pressure})
    grid->tree().setValueOff({16, 0, 0}, 0);
  state.velocity->tree().setValueOff({16, 0, 0}, openvdb::Vec3d(0));
  state.domain->tree().setValueOff({16, 0, 0}, false);
  write("state.vdb", state.grids());
  const auto original = bytes(file("state.vdb"));
  vdb::ProcessingOptions ignored;
  ignored.voxelSize = -1; ignored.halfWidth = -1; ignored.maxTriangles = 0;
  ignored.gridName.clear();
  const auto result = vdb::WriteFluidSources(file("state.vdb"), file("copy.vdb"), {}, ignored);
  const auto output = read("copy.vdb");
  EXPECT_TRUE(result.converged); EXPECT_EQ(result.affectedCellCount, 0u);
  EXPECT_EQ(result.changedCellCount, 0u);
  EXPECT_DOUBLE_EQ(result.densityMassBefore, result.densityMassAfter);
  EXPECT_DOUBLE_EQ(result.temperatureIntegralBefore, result.temperatureIntegralAfter);
  sameGrid(*state.density, *output.density);
  sameGrid(*state.temperature, *output.temperature);
  preservedNativeFields(state, output);
  EXPECT_EQ(bytes(file("state.vdb")), original);
}

TEST_F(OpenVdbFluidSources, LiteralCustomNamesAndExclusiveOutputPreserveExistingFiles) {
  auto state = make();
  vdb::FluidSourcesOptions options;
  options.densityGridName = "descriptor:literal density";
  options.temperatureGridName = "kelvin"; options.velocityGridName = "wind";
  options.pressureGridName = "potential"; options.domainGridName = "mask";
  state.density->setName(options.densityGridName);
  state.temperature->setName(options.temperatureGridName);
  state.velocity->setName(options.velocityGridName);
  state.pressure->setName(options.pressureGridName);
  state.domain->setName(options.domainGridName);
  write("named.vdb", state.grids());
  (void)vdb::WriteFluidSources(file("named.vdb"), file("named-output.vdb"), options);
  const auto output = read("named-output.vdb", options);
  sameGrid(*state.density, *output.density); preservedNativeFields(state, output);
  const auto original = bytes(file("named.vdb"));
  { std::ofstream sentinel(file("existing.vdb")); sentinel << "keep existing"; }
  const auto existing = bytes(file("existing.vdb"));
  EXPECT_THROW((void)vdb::WriteFluidSources(file("named.vdb"), file("named.vdb"), options), std::exception);
  EXPECT_THROW((void)vdb::WriteFluidSources(file("named.vdb"), file("existing.vdb"), options), std::exception);
  EXPECT_THROW((void)vdb::WriteFluidSources(file("named.vdb"), file("missing-parent/output.vdb"), options), std::exception);
  EXPECT_EQ(bytes(file("named.vdb")), original);
  EXPECT_EQ(bytes(file("existing.vdb")), existing);
  EXPECT_FALSE(std::filesystem::exists(file("missing-parent")));
}

TEST_F(OpenVdbFluidSources, InvalidOptionsNamesSourceCountsAndRateRangesLeaveNoOutput) {
  write("state.vdb", make().grids());
  for (double dt : {0.0, 1e-7, 3601.0, std::numeric_limits<double>::quiet_NaN()}) {
    vdb::FluidSourcesOptions o; o.timeStep = dt; failure("state.vdb", o);
  }
  for (double ambient : {-1.0, 1e6 + 1, std::numeric_limits<double>::infinity()}) {
    vdb::FluidSourcesOptions o; o.ambientTemperature = ambient; failure("state.vdb", o);
  }
  for (uint64_t cap : {0ull, 1ull, 1'000'001ull}) {
    vdb::FluidSourcesOptions o; o.maxFluidCells = cap; failure("state.vdb", o);
  }
  { vdb::FluidSourcesOptions o; o.sources.resize(65); failure("state.vdb", o); }
  { vdb::FluidSourcesOptions o; o.domainGridName = o.densityGridName; failure("state.vdb", o); }
  { vdb::FluidSourcesOptions o; o.temperatureGridName.clear(); failure("state.vdb", o); }
  { vdb::FluidSourcesOptions o; o.velocityGridName.assign(257, 'x'); failure("state.vdb", o); }
  for (double radius : {0.0, 1e-7, 1e6 + 1, std::numeric_limits<double>::infinity()}) {
    vdb::FluidSourcesOptions o; o.sources = {sphere(radius)}; failure("state.vdb", o);
  }
  for (double center : {1e12 + 1, std::numeric_limits<double>::quiet_NaN()}) {
    vdb::FluidSourcesOptions o; o.sources = {sphere()}; o.sources[0].center[1] = center; failure("state.vdb", o);
  }
  for (auto member : {&vdb::FluidSourceSphere::densityRate, &vdb::FluidSourceSphere::temperatureRate,
                      &vdb::FluidSourceSphere::densityDecayRate, &vdb::FluidSourceSphere::temperatureRelaxationRate})
    for (double rate : {-1.0, 1e6 + 1, std::numeric_limits<double>::infinity()}) {
      vdb::FluidSourcesOptions o; o.sources = {sphere()}; o.sources[0].*member = rate; failure("state.vdb", o);
    }
}

TEST_F(OpenVdbFluidSources, CombinedRateAndFinalScalarCapsRejectRatherThanClamp) {
  auto state = make(2, 1, 1); write("state.vdb", state.grids());
  for (auto member : {&vdb::FluidSourceSphere::densityRate, &vdb::FluidSourceSphere::temperatureRate,
                      &vdb::FluidSourceSphere::densityDecayRate, &vdb::FluidSourceSphere::temperatureRelaxationRate}) {
    auto emitter = sphere(); emitter.*member = 600000;
    vdb::FluidSourcesOptions o; o.sources = {emitter, emitter}; failure("state.vdb", o);
  }
  for (bool heat : {false, true}) {
    auto emitter = sphere();
    if (heat) emitter.temperatureRate = 1e6; else emitter.densityRate = 1e6;
    vdb::FluidSourcesOptions o; o.timeStep = 1; o.sources = {emitter}; failure("state.vdb", o);
  }
  // The rate cap applies to an overlap, not to disjoint sources elsewhere.
  auto left = sphere(.25); left.densityRate = 600000;
  auto right = left; right.center = {1, 0, 0};
  vdb::FluidSourcesOptions o; o.timeStep = .1; o.sources = {left, right};
  (void)vdb::WriteFluidSources(file("state.vdb"), file("disjoint.vdb"), o);
  const auto output = read("disjoint.vdb");
  EXPECT_NEAR(output.density->tree().getValue({0, 0, 0}), 60002, 1e-10);
  EXPECT_NEAR(output.density->tree().getValue({1, 0, 0}), 60002, 1e-10);
}

TEST_F(OpenVdbFluidSources, NoncanonicalTypesClassesTransformsAndStoredPayloadsAreRejected) {
  const auto bad = [&](const State &state) { write("bad.vdb", state.grids()); failure("bad.vdb"); };
  { auto s = make(); s.density->tree().setValue({0, 0, 0}, -1); bad(s); }
  { auto s = make(); s.temperature->tree().setValue({0, 0, 0}, 1e6 + 1); bad(s); }
  { auto s = make(); s.temperature->tree().setValue({0, 0, 0}, std::numeric_limits<double>::quiet_NaN()); bad(s); }
  { auto s = make(); s.pressure->tree().setValue({0, 0, 0}, std::numeric_limits<double>::infinity()); bad(s); }
  { auto s = make(); s.density->tree().root().setBackground(1, false); bad(s); }
  { auto s = make(); s.temperature->tree().setValueOff({0, 0, 0}, 1); bad(s); }
  { auto s = make(); s.density->tree().setValue({20, 0, 0}, 1); bad(s); }
  { auto s = make(); s.density->setGridClass(openvdb::GRID_UNKNOWN); bad(s); }
  { auto s = make(); s.temperature->setGridClass(openvdb::GRID_LEVEL_SET); bad(s); }
  { auto s = make(); s.domain->setGridClass(openvdb::GRID_FOG_VOLUME); bad(s); }
  { auto s = make(); s.velocity->setIsInWorldSpace(true); bad(s); }
  { auto s = make(); face(s, {0, 0, 0}, 0, 1); bad(s); }
  { auto s = make(); face(s, {1, 0, 0}, 0, std::numeric_limits<double>::infinity()); bad(s); }
  { auto s = make(); s.pressure->transform().postTranslate({1, 0, 0}); bad(s); }
  { auto s = make(); for (const auto &g : s.grids()) g->transform().postScale(openvdb::Vec3d(1, 2, 1)); bad(s); }
  auto state = make();
  for (size_t index = 0; index < 5; ++index) {
    auto grids = state.grids();
    const auto name = grids[index]->getName();
    grids[index] = openvdb::FloatGrid::create(0); grids[index]->setName(name);
    write("type.vdb", grids); failure("type.vdb");
  }
}

TEST_F(OpenVdbFluidSources, MissingStatisticsStreamingMissingNamesAndDescriptorCollisionsFail) {
  auto state = make();
  write("no-statistics.vdb", state.grids(), false); failure("no-statistics.vdb");
  { std::ofstream stream(file("stream.vdb"), std::ios::binary);
    openvdb::io::Stream output(stream); output.write(state.grids()); }
  failure("stream.vdb");
  auto grids = state.grids(); grids.pop_back(); write("missing.vdb", grids); failure("missing.vdb");
  grids = state.grids(); grids.push_back(state.density->deepCopy());
  write("duplicate.vdb", grids); failure("duplicate.vdb");
  auto literal = state.density->deepCopy(); literal->setName("density[0]"); grids.push_back(literal);
  write("collision.vdb", grids); failure("collision.vdb");
}

TEST_F(OpenVdbFluidSources, SharedResidentBudgetsAndRadiusOrVoxelRelativeWorldPrecisionAreEnforced) {
  auto state = make(); write("state.vdb", state.grids());
  { vdb::ProcessingOptions p; p.maxInputBytes = 1; failure("state.vdb", {}, p); }
  { vdb::ProcessingOptions p; p.maxVoxelCount = 19; failure("state.vdb", {}, p); }
  { vdb::ProcessingOptions p; p.maxVoxelCount = 1024; failure("state.vdb", {}, p); }
  { uintmax_t sum = 0, largest = 0;
    for (const auto &g : state.grids()) { sum += g->memUsage(); largest = std::max<uintmax_t>(largest, g->memUsage()); }
    vdb::ProcessingOptions p; p.maxInputBytes = std::filesystem::file_size(file("state.vdb")) + sum - 1;
    EXPECT_GT(p.maxInputBytes, largest + std::filesystem::file_size(file("state.vdb")));
    failure("state.vdb", {}, p); }
  int serial = 0;
  for (const auto [h, origin, radius] : {std::array<double, 3>{1, 1e9, 1e-6},
                                        std::array<double, 3>{1.5e-5, 1e8, 1}}) {
    auto remote = make(1, 1, 1, h);
    for (const auto &g : remote.grids()) g->transform().postTranslate({origin, 0, 0});
    const auto name = "precision-" + std::to_string(serial++) + ".vdb"; write(name, remote.grids());
    auto emitter = sphere(radius); emitter.center = {origin, 0, 0}; emitter.densityRate = 1;
    vdb::FluidSourcesOptions o; o.sources = {emitter}; failure(name, o);
  }
}

TEST_F(OpenVdbFluidSources, ScalarLeafExpansionIsPreflightedAgainstCombinedTopologyBudget) {
  auto state = make(1, 1, 1);
  state.density->tree().clear();
  state.temperature->tree().clear();
  ASSERT_EQ(state.density->tree().leafCount(), 0u);
  ASSERT_EQ(state.temperature->tree().leafCount(), 0u);
  ASSERT_EQ(state.velocity->tree().leafCount() + state.pressure->tree().leafCount() +
                state.domain->tree().leafCount(), 3u);
  write("sparse-scalars.vdb", state.grids());
  auto emitter = sphere(); emitter.densityRate = 1; emitter.temperatureRate = 1;
  vdb::FluidSourcesOptions options; options.timeStep = .1; options.sources = {emitter};
  vdb::ProcessingOptions processing; processing.maxVoxelCount = 1536;
  failure("sparse-scalars.vdb", options, processing);
  processing.maxVoxelCount = 2560;
  const auto result = vdb::WriteFluidSources(file("sparse-scalars.vdb"),
      file("five-leaves.vdb"), options, processing);
  const auto output = read("five-leaves.vdb");
  EXPECT_TRUE(result.converged);
  EXPECT_NEAR(output.density->tree().getValue({0, 0, 0}), .1, 1e-16);
  EXPECT_NEAR(output.temperature->tree().getValue({0, 0, 0}), .1, 1e-16);
  uint64_t leaves = 0;
  for (const auto &grid : output.grids()) leaves += grid->baseTree().leafCount();
  EXPECT_EQ(leaves, 5u);
  preservedNativeFields(state, output);
}

TEST_F(OpenVdbFluidSources, EquilibriumAndAmbientEndpointsDoNotCreateFalseUpperCapFailures) {
  auto state = make(1, 1, 1);
  state.density->tree().setValue({0, 0, 0}, 1e6);
  state.temperature->tree().setValue({0, 0, 0}, 0);
  write("endpoints.vdb", state.grids());
  auto emitter = sphere();
  emitter.densityRate = 500; emitter.densityDecayRate = .0005;
  emitter.temperatureRelaxationRate = 75;
  vdb::FluidSourcesOptions options;
  options.timeStep = 1; options.ambientTemperature = 1e6; options.sources = {emitter};
  const auto result = vdb::WriteFluidSources(file("endpoints.vdb"), file("relaxed.vdb"), options);
  const auto output = read("relaxed.vdb");
  const double density = output.density->tree().getValue({0, 0, 0});
  EXPECT_TRUE(result.converged);
  // With the binary value of .0005, J/K is just below 1e6; the exact
  // endpoint is within one stored ULP below 1e6, never above the cap.
  EXPECT_LE(density, 1e6);
  EXPECT_GE(density, std::nextafter(1e6, 0.0));
  EXPECT_EQ(std::bit_cast<uint64_t>(output.temperature->tree().getValue({0, 0, 0})),
            std::bit_cast<uint64_t>(1e6));
  // Increasing injection makes the exact derivative positive at the cap.
  // This must still fail rather than hide a true excess behind endpoint repair.
  options.sources[0].densityRate = 501;
  failure("endpoints.vdb", options);
}

TEST_F(OpenVdbFluidSources, ActiveFalseCoordinateLimitDoesNotExpandTheFluidPrecisionBounds) {
  auto state = make(1, 1, 1);
  const openvdb::Coord remote(std::numeric_limits<int32_t>::max(), 0, 0);
  state.domain->tree().setValue(remote, false);
  write("false-edge.vdb", state.grids());
  (void)vdb::WriteFluidSources(file("false-edge.vdb"), file("edge-copy.vdb"));
  const auto copy = read("edge-copy.vdb");
  sameGrid(*state.domain, *copy.domain);
  auto emitter = sphere(.25); emitter.densityRate = 1;
  vdb::FluidSourcesOptions options; options.timeStep = .1; options.sources = {emitter};
  const auto result = vdb::WriteFluidSources(file("false-edge.vdb"), file("edge-edit.vdb"), options);
  const auto edited = read("edge-edit.vdb");
  EXPECT_EQ(result.fluidCellCount, 1u); EXPECT_EQ(result.affectedCellCount, 1u);
  EXPECT_NEAR(edited.density->tree().getValue({0, 0, 0}), 2.1, 1e-15);
  EXPECT_TRUE(edited.domain->tree().isValueOn(remote));
  EXPECT_FALSE(edited.domain->tree().getValue(remote));
  preservedNativeFields(state, edited);
}

TEST_F(OpenVdbFluidSources, AmbientScalingRecoversRelaxationBelowTheIntermediateProductRange) {
  auto state = make(1, 1, 1);
  state.temperature->tree().setValue({0, 0, 0}, 0);
  write("cold.vdb", state.grids());
  auto emitter = sphere();
  emitter.temperatureRelaxationRate = std::numeric_limits<double>::denorm_min();
  vdb::FluidSourcesOptions options;
  options.timeStep = 1e-6; options.ambientTemperature = 1e6; options.sources = {emitter};
  const auto result = vdb::WriteFluidSources(file("cold.vdb"), file("tiny-heat.vdb"), options);
  const auto output = read("tiny-heat.vdb");
  const double temperature = output.temperature->tree().getValue({0, 0, 0});
  EXPECT_TRUE(result.converged);
  EXPECT_EQ(std::bit_cast<uint64_t>(temperature), uint64_t{1});
  EXPECT_DOUBLE_EQ(result.temperatureIntegralAfter, temperature);
  EXPECT_EQ(result.changedCellCount, 1u);
}

TEST_F(OpenVdbFluidSources, LargeInitialDensityRecoversAnOtherwiseUnderflowingExponentialTail) {
  auto state = make(1, 1, 1);
  state.density->tree().setValue({0, 0, 0}, 1e6);
  write("large-density.vdb", state.grids());
  auto emitter = sphere(); emitter.densityDecayRate = 750;
  vdb::FluidSourcesOptions options; options.timeStep = 1; options.sources = {emitter};
  const auto result = vdb::WriteFluidSources(file("large-density.vdb"), file("decay-tail.vdb"), options);
  const auto output = read("decay-tail.vdb");
  const double density = output.density->tree().getValue({0, 0, 0});
  // Evaluate the known logarithmic result directly; exp(-750) itself is zero
  // in Double, while exp(log(1e6)-750) is a representable subnormal.
  const double expected = std::exp(std::log(1e6) - 750);
  ASSERT_GT(expected, 0);
  EXPECT_TRUE(result.converged);
  EXPECT_GT(density, 0);
  EXPECT_NEAR(density, expected, 8 * std::numeric_limits<double>::denorm_min());
  EXPECT_LT(density, 1e-319);
  EXPECT_DOUBLE_EQ(output.temperature->tree().getValue({0, 0, 0}), 350);
  preservedNativeFields(state, output);
}
TEST_F(OpenVdbFluidSources, SubnormalReciprocalDoubleRoundingCannotEraseForcing) {
  auto state = make(1, 1, 1);
  state.density->tree().setValue({0, 0, 0}, 0);
  state.temperature->tree().setValue({0, 0, 0}, 0);
  write("tiny-forcing.vdb", state.grids());
  auto emitter = sphere();
  emitter.densityRate = 37 * std::numeric_limits<double>::denorm_min();
  emitter.densityDecayRate = 74;
  vdb::FluidSourcesOptions options;
  options.timeStep = 1; options.ambientTemperature = 0; options.sources = {emitter};
  // J/K rounds to zero (an exact half-subnormal tie), while J*(rounded1/K)
  // can round UP to one subnormal. Convex endpoint repair must not erase that
  // positive term and silently accept an unrepresentable forcing response.
  EXPECT_DOUBLE_EQ(emitter.densityRate / emitter.densityDecayRate, 0);
  failure("tiny-forcing.vdb", options);
  options.sources[0].densityRate = 0;
  options.sources[0].temperatureRate = emitter.densityRate;
  options.sources[0].temperatureRelaxationRate = 74;
  failure("tiny-forcing.vdb", options);
}
#endif
} // namespace
