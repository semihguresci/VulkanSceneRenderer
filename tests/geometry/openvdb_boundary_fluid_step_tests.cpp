#include "Container/geometry/OpenVdbBoundaryFluidStep.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "../../src/geometry/OpenVdbFileUtils.h"
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace api = container::geometry::vdb;
TEST(OpenVdbBoundaryFluidAvailability, DisabledBeforePathsOptionsAndInputs) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  api::BoundaryFluidStepOptions o;
  o.maxWork = 0;
  o.timeStep = -1;
  o.initialVelocity = static_cast<api::BoundaryFluidInitialVelocity>(99);
  try {
    (void)api::WriteBoundaryFluidStep({}, "missing-parent/out.vdb", o);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using C = openvdb::Coord;
using V = openvdb::Vec3d;
using M = openvdb::Vec3DGrid;
using S = openvdb::DoubleGrid;
using B = openvdb::BoolGrid;
constexpr const char *Id = "container:boundary_projection_state_id";
const std::array<const char *, 5> provenance{
    "container:boundary_projection_schema", "container:boundary_face_channels",
    Id, "container:boundary_clamped_faces", "container:boundary_maximum_clamp"};

template <class G> class Exact final : public G {
public:
  explicit Exact(const G &g)
      : G(openvdb::ConstPtrCast<typename G::TreeType>(g.constTreePtr())) {
    this->insertMeta(g);
    this->setTransform(api::detail::CopyTransformExact(g.transform()));
    this->setSaveFloatAsHalf(false);
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, api::detail::CopyTransformExact(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const Exact &>(*this).copyGrid());
  }
};
openvdb::math::Transform::Ptr uniform(double h, V origin = V(0)) {
  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  openvdb::writeString(stream,
                       openvdb::math::UniformScaleTranslateMap::mapType());
  origin.write(stream);
  for (double x : {h, h, 1 / h, 1 / (h * h), .5 / h})
    V(x).write(stream);
  stream.seekg(0);
  openvdb::io::setCurrentVersion(stream);
  auto map = std::make_shared<openvdb::math::Transform>();
  map->read(stream);
  return map;
}

class OpenVdbBoundaryFluid : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    directory = api::detail::CreateAsciiVdbTemporaryDirectory();
    options.timeStep = .5;
    options.maxInputBytes = 2ull << 30;
    options.maxOutputBytes = 1ull << 30;
    options.maxWork = 10'000'000'000ull;
    options.maxStoredLeafVoxels = 128ull << 20;
    map = uniform(1);
  }
  void TearDown() override {
    // This exact absolute ASCII directory is freshly and exclusively owned.
    std::error_code e;
    for (auto i = std::filesystem::recursive_directory_iterator(directory, e);
         !e && i != std::filesystem::recursive_directory_iterator(); ++i)
      if (i->is_regular_file())
        std::filesystem::remove(i->path(), e);
    for (const auto &child : {directory / std::filesystem::path(u8"入力-ç")})
      std::filesystem::remove(child, e);
    std::filesystem::remove(directory, e);
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  static std::string bytes(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
  }
  void write(const std::filesystem::path &p, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &metadata) {
    openvdb::GridPtrVec views;
    for (const auto &g : grids) {
#define T(G)                                                                   \
  if (g->isType<openvdb::G>()) {                                               \
    views.push_back(std::make_shared<Exact<openvdb::G>>(                       \
        static_cast<const openvdb::G &>(*g)));                                 \
    continue;                                                                  \
  }
      T(DoubleGrid)
      T(FloatGrid)
      T(Vec3DGrid)
      T(Vec3IGrid)
      T(BoolGrid)
#undef T
      throw std::runtime_error("unsupported fixture native type");
    }
    openvdb::io::File file(p.string());
    file.write(views, metadata);
    file.close();
  }
  openvdb::GridBase::Ptr read(const std::filesystem::path &p,
                              const std::string &name) {
    const auto copy = path("readback.vdb");
    std::filesystem::copy_file(
        p, copy, std::filesystem::copy_options::overwrite_existing);
    openvdb::io::File file(copy.string());
    file.open(false);
    auto grid = file.readGrid(name);
    file.close();
    return grid;
  }
  openvdb::MetaMap::Ptr fileMeta(const std::filesystem::path &p) {
    const auto copy = path("metadata-readback.vdb");
    std::filesystem::copy_file(
        p, copy, std::filesystem::copy_options::overwrite_existing);
    openvdb::io::File file(copy.string());
    file.open(false);
    auto result = file.getMetadata();
    file.close();
    return result;
  }
  template <class G> void semantic(G &g, const char *name) {
    g.setName(name);
    g.setTransform(api::detail::CopyTransformExact(*map));
    g.setIsInWorldSpace(true);
    g.setVectorType(openvdb::VEC_INVARIANT);
    g.insertMeta("artist", openvdb::StringMetadata("native-fixture"));
  }
  // Actual geometric M04 produces the table; a native SDK write independently
  // seeds a manufactured solenoidal MAC predictor. Old pressure is irrelevant.
  api::BoundaryFluidInputs
  fixture(int n, double velocity, std::vector<double> density,
          std::vector<double> temperature = {}, double leftPressure = 0,
          double rightPressure = 0, bool allOpen = false,
          bool gapMiddle = false) {
    auto fluid = B::create(false), open = B::create(false);
    auto u = M::create(V(0));
    auto p = S::create(0), d = S::create(0), t = S::create(0);
    semantic(*fluid, "domain");
    semantic(*open, "open");
    semantic(*u, "predictor");
    u->setGridClass(openvdb::GRID_STAGGERED);
    u->setIsInWorldSpace(false);
    u->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    u->insertMeta("container:mac_layout",
                  openvdb::StringMetadata("negative_faces"));
    u->insertMeta("container:velocity_units",
                  openvdb::StringMetadata("voxels_per_second"));
    semantic(*p, "pD");
    semantic(*d, "D");
    semantic(*t, "T");
    d->setGridClass(openvdb::GRID_FOG_VOLUME);
    for (int x = 0; x < n; ++x) {
      if (gapMiddle && x == 1)
        continue;
      fluid->tree().setValueOn(C(x, 0, 0), true);
      d->tree().setValueOn(C(x, 0, 0), density.at(size_t(x)));
      t->tree().setValueOn(
          C(x, 0, 0), temperature.empty() ? 300. : temperature.at(size_t(x)));
    }
    open->tree().setValueOn(C(-1, 0, 0), true);
    open->tree().setValueOn(C(n, 0, 0), true);
    for (int x = -2; x <= n + 1; ++x)
      for (int y = -1; y <= 1; ++y)
        for (int z = -1; z <= 1; ++z)
          p->tree().setValueOn(C(x, y, z),
                               leftPressure + (rightPressure - leftPressure) *
                                                  (x + .5) / n);
    for (const auto *key : provenance)
      d->insertMeta(key, openvdb::StringMetadata("stale scalar provenance"));
    openvdb::MetaMap metadata;
    metadata.insertMeta("scene", openvdb::Int64Metadata(9007199254740993ll));
    const auto raw = path("raw" + std::to_string(serial) + ".vdb");
    const auto canonical = path("canonical" + std::to_string(serial) + ".vdb");
    const auto seeded = path("seeded" + std::to_string(serial++) + ".vdb");
    write(raw, {fluid, open, u, p, d, t}, metadata);
    api::BoundaryProjectionInputs input;
    input.fluid = {raw, "domain"};
    input.velocity = {raw, "predictor"};
    if (leftPressure != 0 || rightPressure != 0)
      input.pressure = {raw, "pD"};
    if (!allOpen)
      input.open = {raw, "open"};
    api::BoundaryProjectionOptions settings;
    settings.projection.timeStep = .125;
    settings.missing = allOpen ? api::MissingFluidBoundary::OpenFacePressure
                               : api::MissingFluidBoundary::ClosedWall;
    settings.maxWorkItems = 10'000'000'000ull;
    api::ProcessingOptions limits;
    limits.maxInputBytes = 2ull << 30;
    limits.maxVoxelCount = 128ull << 20;
    (void)api::WriteBoundaryProjectedVelocity(input, canonical, settings,
                                              limits);
    openvdb::GridPtrVec grids;
    for (const auto *name :
         {"velocity", "pressure", "fluid", "boundary_kind", "boundary_fraction",
          "boundary_pressure", "boundary_wall_velocity"})
      grids.push_back(read(canonical, name));
    auto stored = openvdb::gridPtrCast<M>(grids[0]);
    auto kind = openvdb::gridPtrCast<openvdb::Vec3IGrid>(grids[3]);
    for (auto i = stored->beginValueOn(); i; ++i) {
      V value(0);
      if (kind->tree().getValue(i.getCoord())[0] != 0 &&
          kind->tree().getValue(i.getCoord())[0] != 2)
        value[0] = velocity;
      i.setValue(value);
    }
    write(seeded, grids, *fileMeta(canonical));
    api::BoundaryFluidInputs result;
    result.boundary.state = seeded;
    result.density = {raw, "D"};
    result.temperature = api::BoundaryProjectionField{raw, "T"};
    return result;
  }
  api::BoundaryFluidInputs restart(const std::filesystem::path &p) {
    api::BoundaryFluidInputs result;
    result.boundary.state = p;
    result.density = {p, "density"};
    result.temperature = api::BoundaryProjectionField{p, "temperature"};
    return result;
  }
  api::BoundaryFluidStepResult step(const api::BoundaryFluidInputs &in,
                                    const std::string &name = "out.vdb") {
    return api::WriteBoundaryFluidStep(in, path(name), options);
  }
  std::vector<double> values(const std::filesystem::path &p, const char *name,
                             int n) {
    auto grid = openvdb::gridPtrCast<S>(read(p, name));
    EXPECT_TRUE(grid);
    std::vector<double> result;
    for (int x = 0; x < n; ++x)
      result.push_back(grid->tree().getValue(C(x, 0, 0)));
    return result;
  }
  void mutate(const api::BoundaryFluidInputs &in,
              const std::function<void(openvdb::GridPtrVec &)> &change) {
    openvdb::GridPtrVec grids;
    for (const auto *name :
         {"velocity", "pressure", "fluid", "boundary_kind", "boundary_fraction",
          "boundary_pressure", "boundary_wall_velocity"})
      grids.push_back(read(in.boundary.state, name));
    auto meta = fileMeta(in.boundary.state);
    change(grids);
    std::filesystem::remove(in.boundary.state);
    write(in.boundary.state, grids, *meta);
  }
  template <class F> void rejects(F call, const std::string &name = "out.vdb") {
    EXPECT_ANY_THROW(call());
    EXPECT_FALSE(std::filesystem::exists(path(name)));
  }
  std::filesystem::path directory;
  openvdb::math::Transform::Ptr map;
  api::BoundaryFluidStepOptions options;
  unsigned serial{};
};

TEST_F(OpenVdbBoundaryFluid, ConstantReservoirChannelAndNineGridMetadata) {
  auto in = fixture(3, .25, {2, 2, 2});
  options.reservoirDensity = 2;
  options.reservoirTemperature = 300;
  const auto original = bytes(in.boundary.state),
             scalar = bytes(in.density.file);
  const auto r = step(in);
  EXPECT_TRUE(r.converged);
  EXPECT_EQ(r.actualSubsteps, 1);
  EXPECT_EQ(r.attempts, 1);
  EXPECT_EQ(r.maxConstantResponseDrift, 0);
  EXPECT_EQ(r.maxDivergenceAfter, 0);
  EXPECT_EQ(values(path("out.vdb"), "density", 3),
            (std::vector<double>{2, 2, 2}));
  EXPECT_EQ(values(path("out.vdb"), "temperature", 3),
            (std::vector<double>{300, 300, 300}));
  EXPECT_EQ(r.densityMassBefore, 6);
  EXPECT_EQ(r.densityMassAfter, 6);
  EXPECT_EQ(r.densityOutwardTransfer, 0);
  EXPECT_EQ(r.densityBalanceError, 0);
  EXPECT_EQ(r.reservoirFaceApplications, 1);
  EXPECT_EQ(r.outflowFaceApplications, 1);
  EXPECT_EQ(fileMeta(path("out.vdb"))->metaValue<int64_t>("scene"),
            9007199254740993ll);
  auto d = read(path("out.vdb"), "density");
  EXPECT_EQ(d->getGridClass(), openvdb::GRID_FOG_VOLUME);
  EXPECT_EQ(d->metaValue<std::string>("artist"), "native-fixture");
  for (const auto *key : provenance)
    EXPECT_FALSE(bool((*d)[key]));
  const auto id = read(path("out.vdb"), "fluid")->metaValue<std::string>(Id);
  EXPECT_NE(id, read(in.boundary.state, "fluid")->metaValue<std::string>(Id));
  for (const auto *name :
       {"velocity", "pressure", "fluid", "boundary_kind", "boundary_fraction",
        "boundary_pressure", "boundary_wall_velocity"})
    EXPECT_EQ(read(path("out.vdb"), name)->metaValue<std::string>(Id), id);
  EXPECT_EQ(bytes(in.boundary.state), original);
  EXPECT_EQ(bytes(in.density.file), scalar);
  EXPECT_LE(r.workUpperBound, options.maxWork);
  EXPECT_LE(r.workspaceUpperBound, options.maxInputBytes);
}

TEST_F(OpenVdbBoundaryFluid, ExactSharedFluxPulseAndRestartPartitionOracle) {
  auto in = fixture(4, .5, {1, 0, 0, 0});
  options.reservoirTemperature = 300;
  const auto first = step(in, "first.vdb");
  EXPECT_EQ(values(path("first.vdb"), "density", 4),
            (std::vector<double>{.75, .25, 0, 0}));
  EXPECT_EQ(first.densityMassAfter, 1);
  (void)step(restart(path("first.vdb")), "second.vdb");
  const std::vector<double> expected{.5625, .375, .0625, 0};
  EXPECT_EQ(values(path("second.vdb"), "density", 4), expected);
  options.timeStep = 1;
  options.substeps = 2;
  const auto combined = step(in, "combined.vdb");
  EXPECT_EQ(combined.actualSubsteps, 2);
  EXPECT_EQ(values(path("combined.vdb"), "density", 4), expected);
  auto a = openvdb::gridPtrCast<M>(read(path("second.vdb"), "velocity"));
  auto b = openvdb::gridPtrCast<M>(read(path("combined.vdb"), "velocity"));
  for (int x = 0; x <= 4; ++x)
    EXPECT_EQ(a->tree().getValue(C(x, 0, 0))[0],
              b->tree().getValue(C(x, 0, 0))[0]);
}

TEST_F(OpenVdbBoundaryFluid, ReverseReservoirDonorAndAmbientInitialization) {
  auto in = fixture(3, -.5, {0, 0, 0});
  in.temperature.reset();
  options.reservoirDensity = 1;
  const auto r = step(in);
  EXPECT_EQ(values(path("out.vdb"), "density", 3),
            (std::vector<double>{0, 0, .25}));
  EXPECT_EQ(r.densityOutwardTransfer, -.25);
  EXPECT_EQ(r.densityMassAfter, .25);
  EXPECT_TRUE(r.temperatureInitialized);
  for (double x : values(path("out.vdb"), "temperature", 3))
    EXPECT_EQ(x, options.ambientTemperature);
}

TEST_F(OpenVdbBoundaryFluid,
       ToleratedStoredDivergenceDoesNotNormalizeConstants) {
  auto in = fixture(3, .25, {1, 1, 1});
  const double delta = std::ldexp(1., -26);
  mutate(in, [&](auto &grids) {
    auto u = openvdb::gridPtrCast<M>(grids[0]);
    auto value = u->tree().getValue(C(1, 0, 0));
    value[0] += delta;
    u->tree().setValueOn(C(1, 0, 0), value);
  });
  options.timeStep = .25;
  options.reservoirDensity = 1;
  options.reservoirTemperature = 300;
  const auto r = step(in);
  EXPECT_EQ(values(path("out.vdb"), "density", 3),
            (std::vector<double>{1 - .25 * delta, 1 + .25 * delta, 1}));
  EXPECT_EQ(r.maxDivergenceBefore, delta);
  EXPECT_EQ(r.maxConstantResponseDrift, .25 * delta);
  EXPECT_EQ(r.densityMassAfter, 3);
  EXPECT_EQ(r.densityBalanceError, 0);
}

TEST_F(OpenVdbBoundaryFluid,
       PressureDrivenEndpointRetriesDiscardExtraImpulses) {
  auto in = fixture(3, 0, {1, 1, 1}, {}, 1.5, -1.5);
  options.timeStep = 1;
  options.maxCfl = .3;
  options.reservoirDensity = 1;
  options.reservoirTemperature = 300;
  const auto r = step(in);
  ASSERT_EQ(r.steps.size(), 3);
  EXPECT_EQ(r.steps[0].timeStep, .5);
  EXPECT_EQ(r.steps[1].timeStep, .25);
  EXPECT_EQ(r.steps[2].timeStep, .25);
  EXPECT_EQ(r.rejectedAttempts, 2);
  EXPECT_EQ(r.attempts, 5);
  EXPECT_EQ(r.simulatedTime, 1);
  uint64_t acceptedIterations = 0;
  for (const auto &s : r.steps)
    acceptedIterations += s.projectionIterations;
  EXPECT_GT(r.totalProjectionIterations, acceptedIterations);
  auto u = openvdb::gridPtrCast<M>(read(path("out.vdb"), "velocity"));
  for (int x = 0; x <= 3; ++x)
    EXPECT_NEAR(u->tree().getValue(C(x, 0, 0))[0], 1, 2e-6);
  options.maxStepAttempts = 1;
  rejects([&] { (void)step(in, "attempt.vdb"); }, "attempt.vdb");
  options.maxStepAttempts = 128;
  options.maxSubsteps = 2;
  rejects([&] { (void)step(in, "steps.vdb"); }, "steps.vdb");
}

TEST_F(OpenVdbBoundaryFluid, HydrostaticWorldUnitsAndPressureBoundaryDistance) {
  map = uniform(.5);
  auto in = fixture(3, 0, {0, 0, 0}, {}, 4.5, 0);
  options.timeStep = .125;
  options.gravity = {-3, 0, 0};
  options.reservoirTemperature = 300;
  const auto r = step(in);
  EXPECT_EQ(r.cellVolume, .125);
  const auto pressure = values(path("out.vdb"), "pressure", 3);
  const std::array<double, 3> expected{3.75, 2.25, .75};
  for (int x = 0; x < 3; ++x)
    EXPECT_NEAR(pressure[size_t(x)], expected[size_t(x)], 2e-6);
  auto u = openvdb::gridPtrCast<M>(read(path("out.vdb"), "velocity"));
  for (int x = 0; x <= 3; ++x)
    EXPECT_NEAR(u->tree().getValue(C(x, 0, 0))[0], 0, 2e-6);
  EXPECT_LT(r.maxDivergenceAfter, 2e-6);
}

TEST_F(OpenVdbBoundaryFluid,
       ExactTinyFluxCancellationAndFinalUnderflowRejection) {
  const double tiny = std::numeric_limits<double>::denorm_min();
  auto in = fixture(1, std::ldexp(1., -100), {tiny});
  options.timeStep = 16;
  options.reservoirDensity = tiny;
  options.reservoirTemperature = 300;
  const auto r = step(in);
  EXPECT_EQ(values(path("out.vdb"), "density", 1)[0], tiny);
  EXPECT_EQ(r.densityBalanceError, 0);
  EXPECT_EQ(r.densityOutwardTransfer, 0);
  auto outflow = fixture(1, .5, {tiny});
  options.timeStep = 1;
  options.reservoirDensity = 0;
  rejects([&] { (void)step(outflow, "underflow.vdb"); }, "underflow.vdb");
}

TEST_F(OpenVdbBoundaryFluid,
       ReflectedWorldForceRetainsWeakCancellationComponent) {
  const double a = std::sqrt(.5) * 5e5;
  // Independent Decimal100 of the EXACT stored binary coefficients:
  // (1e11-nextafter(1e11,0))/(2*a)*Double(1e-6).
  constexpr double weak = 0x1.8e10d3a69204ap-56;
  for (bool reflected : {false, true}) {
    auto matrix = openvdb::math::Mat4d::identity();
    matrix(0, 0) = reflected ? -a : a;
    matrix(0, 1) = reflected ? -a : a;
    matrix(1, 0) = -a;
    matrix(1, 1) = a;
    matrix(2, 2) = 5e5;
    map = openvdb::math::Transform::createLinearTransform(matrix);
    auto in = fixture(1, 0, {0}, {}, 0, 0, true);
    options.timeStep = 1e-6;
    options.gravity = {1e11, -std::nextafter(1e11, 0), 0};
    const auto name = reflected ? "weak-reflected.vdb" : "weak-rotated.vdb";
    const auto r = step(in, name);
    EXPECT_EQ(r.maxDivergenceAfter, 0);
    auto u = openvdb::gridPtrCast<M>(read(path(name), "velocity"));
    for (int x : {0, 1}) {
      const double stored = u->tree().getValue(C(x, 0, 0))[0];
      EXPECT_NEAR(stored / (reflected ? -weak : weak), 1, 5e-14);
      EXPECT_NE(stored, 0);
    }
  }
}

TEST_F(OpenVdbBoundaryFluid,
       DisconnectedFluidUsesReservoirWithoutCrossGapSampling) {
  auto in = fixture(3, .25, {1, 0, 7}, {}, 0, 0, true, true);
  const auto r = step(in);
  EXPECT_EQ(r.fluidCells, 2);
  auto d = openvdb::gridPtrCast<S>(read(path("out.vdb"), "density"));
  EXPECT_EQ(d->tree().getValue(C(0, 0, 0)), .875);
  EXPECT_EQ(d->tree().getValue(C(2, 0, 0)), 6.125);
  EXPECT_FALSE(d->tree().isValueOn(C(1, 0, 0)));
  EXPECT_EQ(r.densityMassAfter, 7);
  EXPECT_EQ(r.densityOutwardTransfer, 1);
}

TEST_F(OpenVdbBoundaryFluid, TinyLargeAndReflectedMapsPreserveWorldMetric) {
  for (const double h : {1e-6, 1e6}) {
    map = uniform(h, V(12.125, -3, .25));
    auto in = fixture(2, .25, {1, 1});
    options.reservoirDensity = 1;
    options.reservoirTemperature = 300;
    const auto name = h < 1 ? "tiny.vdb" : "large.vdb";
    const auto r = step(in, name);
    auto g = read(path(name), "density");
    EXPECT_EQ(g->transform().baseMap()->applyJacobian(V(1, 0, 0)), V(h, 0, 0));
    EXPECT_EQ(g->transform().baseMap()->applyMap(V(0)), V(12.125, -3, .25));
    EXPECT_NEAR(r.maxWorldSpeedAfter, .25 * h, std::abs(h) * 1e-12);
  }
  openvdb::math::Mat4d matrix = openvdb::math::Mat4d::identity();
  matrix(0, 0) = 0;
  matrix(0, 1) = -.5;
  matrix(1, 0) = -.5;
  matrix(1, 1) = 0;
  matrix(2, 2) = .5;
  map = openvdb::math::Transform::createLinearTransform(matrix);
  auto in = fixture(2, .25, {1, 1});
  const auto r = step(in, "reflected.vdb");
  EXPECT_EQ(r.cellVolume, .125);
  EXPECT_EQ(r.maxWorldSpeedAfter, .125);
  auto g = read(path("reflected.vdb"), "velocity");
  EXPECT_EQ(g->transform().baseMap()->applyJacobian(V(1, 0, 0)), V(0, -.5, 0));
}

TEST_F(OpenVdbBoundaryFluid,
       InvalidCanonicalAxesWallsAndDivergenceAreRejected) {
  auto hidden = fixture(1, 0, {0});
  mutate(hidden, [](auto &grids) {
    auto kinds = openvdb::gridPtrCast<openvdb::Vec3IGrid>(grids[3]);
    auto value = kinds->tree().getValue(C(1, 0, 0));
    value[1] = 3;
    kinds->tree().setValueOn(C(1, 0, 0), value);
  });
  rejects([&] { (void)step(hidden); });
  auto wall = fixture(1, 0, {0});
  mutate(wall, [](auto &grids) {
    auto walls = openvdb::gridPtrCast<M>(grids[6]);
    auto value = walls->tree().getValue(C(0, 0, 0));
    value[1] = 1;
    walls->tree().setValueOn(C(0, 0, 0), value);
  });
  rejects([&] { (void)step(wall); });
  auto divergent = fixture(2, .25, {0, 0});
  mutate(divergent, [](auto &grids) {
    auto u = openvdb::gridPtrCast<M>(grids[0]);
    auto value = u->tree().getValue(C(1, 0, 0));
    value[0] = 1;
    u->tree().setValueOn(C(1, 0, 0), value);
  });
  options.initialVelocity = api::BoundaryFluidInitialVelocity::RequireSolenoidal;
  rejects([&] { (void)step(divergent); });
}

TEST_F(OpenVdbBoundaryFluid, JointLimitsRejectBeforePublication) {
  auto in = fixture(3, .25, {1, 0, 0});
  const auto base = options;
  const auto check = [&] {
    rejects([&] { (void)step(in); });
    options = base;
  };
  options.maxWork = 1;
  check();
  options.maxFluidCells = 2;
  check();
  options.maxNodes = 1;
  check();
  options.maxStoredLeafVoxels = 1;
  check();
  options.maxMetadataBytes = 1;
  check();
  options.maxGrids = 1;
  check();
  options.maxOutputBytes = 1024;
  check();
  options.maxInputBytes = 8ull << 20;
  check();
  options.timeStep = -1;
  check();
  options.names.temperature = options.names.density;
  check();
}

TEST_F(OpenVdbBoundaryFluid,
       FloatSourcesPromoteAndOnlyCanonicalFileMetadataSurvives) {
  auto in = fixture(2, .25, {0, 0});
  auto density = openvdb::FloatGrid::create(0);
  auto temperature = openvdb::FloatGrid::create(0);
  semantic(*density, "native_float_density");
  semantic(*temperature, "native_float_temperature");
  density->setGridClass(openvdb::GRID_FOG_VOLUME);
  density->tree().setValueOn(C(0), 1.25f);
  density->tree().setValueOn(C(1, 0, 0), 2.5f);
  temperature->tree().setValueOn(C(0), 500.f);
  temperature->tree().setValueOn(C(1, 0, 0), 500.f);
  openvdb::MetaMap separate;
  separate.insertMeta("scene", openvdb::Int64Metadata(-42));
  separate.insertMeta("scalar_file_only",
                      openvdb::StringMetadata("not merged"));
  const auto scalars = path("floats.vdb");
  write(scalars, {density, temperature}, separate);
  in.density = {scalars, "native_float_density"};
  in.temperature =
      api::BoundaryProjectionField{scalars, "native_float_temperature"};
  const auto original = bytes(scalars);
  options.reservoirTemperature = 500;
  (void)step(in);
  EXPECT_EQ(values(path("out.vdb"), "density", 2),
            (std::vector<double>{1.09375, 2.34375}));
  EXPECT_EQ(values(path("out.vdb"), "temperature", 2),
            (std::vector<double>{500, 500}));
  EXPECT_TRUE(read(path("out.vdb"), "density")->isType<S>());
  EXPECT_EQ(fileMeta(path("out.vdb"))->metaValue<int64_t>("scene"),
            9007199254740993ll);
  EXPECT_FALSE(bool((*fileMeta(path("out.vdb")))["scalar_file_only"]));
  EXPECT_EQ(bytes(scalars), original);
}

TEST_F(OpenVdbBoundaryFluid,
       ScalarDomainClassNonfiniteAndAmbiguityRejectWithoutOutput) {
  auto in = fixture(2, .25, {1, 0});
  const auto ambiguous = in.density.grid;
  in.density.grid.clear();
  rejects([&] { (void)step(in); });
  in.density.grid = ambiguous;
  auto density = openvdb::gridPtrCast<S>(read(in.density.file, "D"));
  auto temperature = openvdb::gridPtrCast<S>(read(in.density.file, "T"));
  openvdb::MetaMap metadata;
  const auto invalid = path("invalid-scalars.vdb");
  density->tree().touchLeaf(C(9, 0, 0))->setValueOff(C(9, 0, 0), 1);
  write(invalid, {density, temperature}, metadata);
  in.density = {invalid, "D"};
  in.temperature = api::BoundaryProjectionField{invalid, "T"};
  rejects([&] { (void)step(in); });
  std::filesystem::remove(invalid);
  density->tree().touchLeaf(C(9, 0, 0))->setValueOff(C(9, 0, 0), 0);
  density->setGridClass(openvdb::GRID_UNKNOWN);
  write(invalid, {density, temperature}, metadata);
  rejects([&] { (void)step(in); });
  std::filesystem::remove(invalid);
  density->setGridClass(openvdb::GRID_FOG_VOLUME);
  temperature->tree()
      .touchLeaf(C(9, 0, 0))
      ->setValueOff(C(9, 0, 0), std::numeric_limits<double>::infinity());
  write(invalid, {density, temperature}, metadata);
  rejects([&] { (void)step(in); });
}

TEST_F(OpenVdbBoundaryFluid, DefaultLimitsAndBuoyancyUseNewTransportedScalars) {
  auto in = fixture(1, .25, {1}, {302}, 0, 0, true);
  options = api::BoundaryFluidStepOptions{};
  options.timeStep = .5;
  options.ambientTemperature = 300;
  options.reservoirTemperature = 300;
  options.buoyancyDensity = .25;
  options.buoyancyTemperature = .25;
  const auto r = step(in);
  EXPECT_EQ(values(path("out.vdb"), "density", 1)[0], .875);
  EXPECT_EQ(values(path("out.vdb"), "temperature", 1)[0], 301.75);
  // AFTER transport: .5 * (.25*(301.75-300)-.25*.875) = .109375.
  auto u = openvdb::gridPtrCast<M>(read(path("out.vdb"), "velocity"));
  EXPECT_EQ(u->tree().getValue(C(0))[0], .25);
  EXPECT_EQ(u->tree().getValue(C(0))[1], .109375);
  EXPECT_EQ(u->tree().getValue(C(0, 1, 0))[1], .109375);
  EXPECT_EQ(r.maxDivergenceAfter, 0);
  EXPECT_LE(r.workUpperBound, api::BoundaryFluidStepOptions{}.maxWork);
  EXPECT_LE(r.workspaceUpperBound,
            api::BoundaryFluidStepOptions{}.maxInputBytes);
}

TEST_F(OpenVdbBoundaryFluid, DelayedLoadingMinimumDtAndUnclampedRangeGuards) {
  auto in = fixture(1, .25, {1});
  const char *existing = std::getenv("OPENVDB_DISABLE_DELAYED_LOAD");
  const bool present = existing != nullptr;
  const std::string prior = existing ? existing : "";
  const auto environment = [](const char *value) {
#if defined(_WIN32)
    return _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", value ? value : "");
#else
    return value ? setenv("OPENVDB_DISABLE_DELAYED_LOAD", value, 1)
                 : unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
  };
  ASSERT_EQ(environment("1"), 0);
  try {
    (void)step(in);
    ADD_FAILURE() << "disabled delayed loading must reject";
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("delayed loading"), std::string::npos);
  }
  EXPECT_EQ(environment(present ? prior.c_str() : nullptr), 0);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  auto fast = fixture(1, 1e6, {0});
  rejects([&] { (void)step(fast); });
  auto compressed = fixture(2, .25, {1e6, 1e6});
  mutate(compressed, [](auto &grids) {
    auto u = openvdb::gridPtrCast<M>(grids[0]);
    auto value = u->tree().getValue(C(1, 0, 0));
    value[0] += std::ldexp(1., -26);
    u->tree().setValueOn(C(1, 0, 0), value);
  });
  options.reservoirDensity = 1e6;
  rejects([&] { (void)step(compressed); });
}

TEST_F(OpenVdbBoundaryFluid,
       ClosedCirculationHasIndependentScalarAndMomentumOracle) {
  const std::array<C, 4> cells{C(0, 0, 0), C(1, 0, 0), C(1, 1, 0), C(0, 1, 0)};
  auto fluid = B::create(false);
  auto velocity = M::create(V(0));
  auto density = S::create(0), temperature = S::create(0);
  semantic(*fluid, "loop_fluid");
  semantic(*velocity, "loop_velocity");
  semantic(*density, "loop_density");
  semantic(*temperature, "loop_temperature");
  velocity->setGridClass(openvdb::GRID_STAGGERED);
  velocity->setIsInWorldSpace(false);
  velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  velocity->insertMeta("container:mac_layout",
                       openvdb::StringMetadata("negative_faces"));
  velocity->insertMeta("container:velocity_units",
                       openvdb::StringMetadata("voxels_per_second"));
  density->setGridClass(openvdb::GRID_FOG_VOLUME);
  for (const auto &c : cells) {
    fluid->tree().setValueOn(c, true);
    density->tree().setValueOn(c, c == C(0) ? 1 : 0);
    temperature->tree().setValueOn(c, 300);
  }
  velocity->tree().setValueOn(C(1, 0, 0), V(.25, 0, 0));
  velocity->tree().setValueOn(C(1, 1, 0), V(-.25, .25, 0));
  velocity->tree().setValueOn(C(0, 1, 0), V(0, -.25, 0));
  openvdb::MetaMap metadata;
  const auto raw = path("loop-raw.vdb"), canonical = path("loop-canonical.vdb");
  write(raw, {fluid, velocity, density, temperature}, metadata);
  api::BoundaryProjectionInputs source;
  source.fluid = {raw, "loop_fluid"};
  source.velocity = {raw, "loop_velocity"};
  api::BoundaryProjectionOptions projection;
  projection.maxWorkItems = options.maxWork;
  api::ProcessingOptions limits;
  limits.maxInputBytes = options.maxInputBytes;
  limits.maxVoxelCount = options.maxStoredLeafVoxels;
  (void)api::WriteBoundaryProjectedVelocity(source, canonical, projection,
                                            limits);
  api::BoundaryFluidInputs in;
  in.boundary.state = canonical;
  in.density = {raw, "loop_density"};
  in.temperature = api::BoundaryProjectionField{raw, "loop_temperature"};
  const auto r = step(in);
  auto d = openvdb::gridPtrCast<S>(read(path("out.vdb"), "density"));
  EXPECT_EQ(d->tree().getValue(cells[0]), .875);
  EXPECT_EQ(d->tree().getValue(cells[1]), .125);
  EXPECT_EQ(d->tree().getValue(cells[2]), 0);
  EXPECT_EQ(d->tree().getValue(cells[3]), 0);
  // Transverse advectors cancel. Normal upwind neighbors are physical wall
  // zeros: a' = a-dt*a*a = .25-.5*.25*.25 = .21875 at all four loop faces.
  auto u = openvdb::gridPtrCast<M>(read(path("out.vdb"), "velocity"));
  EXPECT_EQ(u->tree().getValue(C(1, 0, 0))[0], .21875);
  EXPECT_EQ(u->tree().getValue(C(1, 1, 0))[0], -.21875);
  EXPECT_EQ(u->tree().getValue(C(1, 1, 0))[1], .21875);
  EXPECT_EQ(u->tree().getValue(C(0, 1, 0))[1], -.21875);
  EXPECT_EQ(r.densityMassBefore, 1);
  EXPECT_EQ(r.densityMassAfter, 1);
  EXPECT_EQ(r.densityOutwardTransfer, 0);
  EXPECT_EQ(r.reservoirFaceApplications, 0);
  EXPECT_EQ(r.outflowFaceApplications, 0);
  EXPECT_EQ(r.maxDivergenceAfter, 0);
}

TEST_F(OpenVdbBoundaryFluid, DetectableWeakInverseLossRejectsBeforeSolving) {
  auto matrix = openvdb::math::Mat4d::identity();
  matrix(0, 0) = matrix(1, 1) = matrix(2, 2) = 1e6;
  matrix(0, 1) = 1e-314;
  matrix(1, 0) = -1e-314;
  // Raw native affine serialization retains this represented weak rotation;
  // approximate map simplification would erase the fixture's critical term.
  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  openvdb::writeString(stream, openvdb::math::AffineMap::mapType());
  matrix.write(stream);
  stream.seekg(0);
  openvdb::io::setCurrentVersion(stream);
  map = std::make_shared<openvdb::math::Transform>();
  map->read(stream);
  EXPECT_EQ(map->baseMap()->applyJacobian(V(1, 0, 0))[1], 1e-314);
  // M04 now independently rejects this same bad inverse before authoring.
  // Seed an ordinary coherent state, then independently replace every map so
  // this remains a test of M05's own pre-solve Frame validation.
  const auto criticalMap = map;
  map = uniform(1e6);
  auto in = fixture(1, 0, {0}, {}, 0, 0, true);
  mutate(in, [&](auto &grids) {
    for (const auto &grid : grids)
      grid->setTransform(api::detail::CopyTransformExact(*criticalMap));
  });
  auto density = read(in.density.file, "D"), temperature = read(in.temperature->file, "T");
  density->setTransform(api::detail::CopyTransformExact(*criticalMap));
  temperature->setTransform(api::detail::CopyTransformExact(*criticalMap));
  const auto scalarFile = path("weak-inverse-scalars.vdb");
  write(scalarFile, {density, temperature}, *fileMeta(in.density.file));
  in.density.file = scalarFile;in.temperature->file = scalarFile;map = criticalMap;
  EXPECT_EQ(read(in.boundary.state, "fluid")
                ->transform()
                .baseMap()
                ->applyJacobian(V(1, 0, 0))[1],
            1e-314);
  options.timeStep = 1e-6;
  options.gravity = {5e11, 0, 0};
  // Exact represented inverse gives a nonzero local Y term around -5e-321.
  // The Double inverse loses that coefficient before force multiplication, so
  // explicit prepublication rejection is required rather than silent zero.
  try {
    (void)step(in);
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("inverse coefficient"),
              std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}

TEST_F(OpenVdbBoundaryFluid,
       ForceStorageRoundingIsCountedAndOldPressureIsIgnored) {
  auto in = fixture(1, 0, {0}, {}, 0, 0, true);
  mutate(in, [](auto &grids) {
    auto pressure = openvdb::gridPtrCast<S>(grids[1]);
    pressure->tree().touchLeaf(C(0))->setValueOn(
        C(0), std::numeric_limits<double>::quiet_NaN());
    pressure->insertMeta(Id,
                         openvdb::StringMetadata("ignored old pressure id"));
  });
  options.timeStep = .1;
  options.gravity = {.1, 0, 0};
  const auto r = step(in);
  EXPECT_EQ(r.roundedMomentumRows, 2);
  auto u = openvdb::gridPtrCast<M>(read(path("out.vdb"), "velocity"));
  EXPECT_EQ(u->tree().getValue(C(0))[0], .1 * .1);
  EXPECT_EQ(values(path("out.vdb"), "pressure", 1)[0], 0);
}

TEST_F(OpenVdbBoundaryFluid, UnicodeSourcesExclusiveOutputAndRestartSafety) {
  auto in = fixture(2, .25, {1, 0});
  const auto child = directory / std::filesystem::path(u8"入力-ç");
  ASSERT_TRUE(std::filesystem::create_directory(child));
  const auto original = bytes(in.boundary.state);
  const auto source = child / std::filesystem::path(u8"状態.vdb");
  std::filesystem::copy_file(in.boundary.state, source);
  in.boundary.state = source;
  const auto output = child / std::filesystem::path(u8"煙.vdb");
  (void)api::WriteBoundaryFluidStep(in, output, options);
  EXPECT_EQ(bytes(source), original);
  const auto final = bytes(output);
  EXPECT_ANY_THROW((void)api::WriteBoundaryFluidStep(in, output, options));
  EXPECT_EQ(bytes(output), final);
  EXPECT_ANY_THROW((void)api::WriteBoundaryFluidStep(in, source, options));
  EXPECT_EQ(bytes(source), original);
  const auto foreign = child / "foreign.vdb";
  {
    std::ofstream out(foreign, std::ios::binary);
    out << "foreign bytes";
  }
  EXPECT_ANY_THROW((void)api::WriteBoundaryFluidStep(in, foreign, options));
  EXPECT_EQ(bytes(foreign), "foreign bytes");
}

TEST_F(OpenVdbBoundaryFluid,
       ActualArrestOutputRestartsByDefaultWhileStrictChecksEntryAndEndpoint) {
  map = uniform(.5);
  auto in = fixture(3, .25, {0, 0, 0}, {}, 4.5, 0);
  const auto original = bytes(in.boundary.state);
  options.timeStep = .125;
  options.gravity = {-4, 0, 0};
  options.reservoirTemperature = 300;
  const auto arrested = step(in, "arrest.vdb");
  EXPECT_EQ(options.initialVelocity, api::BoundaryFluidInitialVelocity::TransportStored);
  EXPECT_TRUE(arrested.initialDivergenceWithinTolerance);
  EXPECT_EQ(arrested.actualSubsteps, 1u);
  EXPECT_EQ(arrested.attempts, 1u);
  // Actual stored-pressure cancellation leaves a small divergent near-rest
  // state. Do not replace these values with a manufactured zero fixture.
  EXPECT_GT(arrested.maxDivergenceAfter, 0);
  const auto nearRest = restart(path("arrest.vdb"));
  const auto nearRestBytes = bytes(nearRest.boundary.state);
  auto stored = openvdb::gridPtrCast<M>(read(nearRest.boundary.state, "velocity"));
  for (int x = 0; x <= 3; ++x)
    EXPECT_NEAR(stored->tree().getValue(C(x, 0, 0))[0], 0, 2e-6);
  // Strict entry is the SAME stored-only condition that failed in the actual
  // old Tool self-restart, without any inferred pressure/velocity floor.
  options.initialVelocity = api::BoundaryFluidInitialVelocity::RequireSolenoidal;
  rejects([&] { (void)step(nearRest, "strict-entry.vdb"); }, "strict-entry.vdb");
  // A strict call starting from the clean .25 state must also reject this
  // incompatible endpoint, rather than publish a state it cannot restart.
  rejects([&] { (void)step(in, "strict-endpoint.vdb"); }, "strict-endpoint.vdb");
  options.initialVelocity = api::BoundaryFluidInitialVelocity::TransportStored;
  options.gravity = {-3, 0, 0};
  const auto resumed = step(nearRest, "resumed.vdb");
  EXPECT_FALSE(resumed.initialDivergenceWithinTolerance);
  EXPECT_GT(resumed.maxDivergenceBefore, 0);
  EXPECT_EQ(resumed.actualSubsteps, 1u);
  EXPECT_EQ(resumed.attempts, 1u);
  const auto pressure = values(path("resumed.vdb"), "pressure", 3);
  for (int x = 0; x < 3; ++x)
    EXPECT_NEAR(pressure[size_t(x)], 3.75 - 1.5 * x, 2e-6);
  stored = openvdb::gridPtrCast<M>(read(path("resumed.vdb"), "velocity"));
  for (int x = 0; x <= 3; ++x)
    EXPECT_NEAR(stored->tree().getValue(C(x, 0, 0))[0], 0, 2e-6);
  EXPECT_EQ(bytes(nearRest.boundary.state), nearRestBytes);
  // A genuinely strict admissible flow remains strict on a second restart.
  options.initialVelocity = api::BoundaryFluidInitialVelocity::RequireSolenoidal;
  const auto strict = step(in, "strict-clean.vdb");
  EXPECT_TRUE(strict.initialDivergenceWithinTolerance);
  const auto strictRestart = restart(path("strict-clean.vdb"));
  const auto strictBytes = bytes(strictRestart.boundary.state);
  const auto strictAgain = step(strictRestart, "strict-resumed.vdb");
  EXPECT_TRUE(strictAgain.initialDivergenceWithinTolerance);
  stored = openvdb::gridPtrCast<M>(read(path("strict-resumed.vdb"), "velocity"));
  for (int x = 0; x <= 3; ++x)
    EXPECT_NEAR(stored->tree().getValue(C(x, 0, 0))[0], .25, 2e-6);
  EXPECT_EQ(bytes(in.boundary.state), original);
  EXPECT_EQ(bytes(strictRestart.boundary.state), strictBytes);
}

TEST_F(OpenVdbBoundaryFluid,
       ActualM04HydrostaticProducerResidualIsTransportedWithoutReseeding) {
  map = uniform(.5);
  auto in = fixture(3, 0, {0, 0, 0}, {}, 4.5, 0);
  const auto raw = path("raw0.vdb");
  auto domain = read(raw, "domain"), open = read(raw, "open"), pressure = read(raw, "pD");
  auto predictor = M::create(V(-.75, 0, 0));
  semantic(*predictor, "predictor");
  predictor->setGridClass(openvdb::GRID_STAGGERED);
  predictor->setIsInWorldSpace(false);
  predictor->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  predictor->insertMeta("container:mac_layout", openvdb::StringMetadata("negative_faces"));
  predictor->insertMeta("container:velocity_units", openvdb::StringMetadata("voxels_per_second"));
  const auto producerInput = path("actual-producer-input.vdb");
  write(producerInput, {domain, open, pressure, predictor}, *fileMeta(raw));
  const auto original = bytes(producerInput);
  api::BoundaryProjectionInputs bindings;
  bindings.fluid = {producerInput, "domain"};bindings.velocity = {producerInput, "predictor"};
  bindings.open = {producerInput, "open"};bindings.pressure = {producerInput, "pD"};
  api::BoundaryProjectionOptions controls;controls.projection.timeStep = .125;
  controls.projection.tolerance = 1e-10;controls.maxWorkItems = 10'000'000'000ull;
  api::ProcessingOptions limits;limits.maxInputBytes = 2ull << 30;limits.maxVoxelCount = 128ull << 20;
  const auto produced = path("actual-producer-state.vdb");
  const auto projected = api::WriteBoundaryProjectedVelocity(bindings, produced, controls, limits);
  ASSERT_TRUE(projected.projection.converged);
  ASSERT_GT(projected.projection.maxDivergenceAfter, 0);
  in.boundary.state = produced;
  const auto sourceBytes = bytes(produced);
  options.timeStep = .125;options.gravity = {-3, 0, 0};options.reservoirTemperature = 300;
  const auto result = step(in, "actual-producer-hydro.vdb");
  EXPECT_FALSE(result.initialDivergenceWithinTolerance);
  EXPECT_GT(result.maxDivergenceBefore, 0);
  const auto p = values(path("actual-producer-hydro.vdb"), "pressure", 3);
  for (int x = 0; x < 3; ++x)
    EXPECT_NEAR(p[size_t(x)], 3.75 - 1.5 * x, 2e-6);
  EXPECT_EQ(bytes(producerInput), original);
  EXPECT_EQ(bytes(produced), sourceBytes);
}

TEST_F(OpenVdbBoundaryFluid,
       StoredCompressibleFluxHasIndependentConstantResponseWithoutPreprojection) {
  auto in = fixture(2, .25, {1, 1});
  mutate(in, [](auto &grids) {
    auto velocity = openvdb::gridPtrCast<M>(grids[0]);
    auto v = velocity->tree().getValue(C(1, 0, 0));v[0] = .5;
    velocity->tree().setValueOn(C(1, 0, 0), v);
  });
  const auto original = bytes(in.boundary.state);
  options.timeStep = .25;options.reservoirDensity = 1;options.reservoirTemperature = 300;
  const auto result = step(in, "compressible.vdb");
  EXPECT_FALSE(result.initialDivergenceWithinTolerance);
  EXPECT_EQ(result.maxDivergenceBefore, .25);
  EXPECT_FALSE(result.roundoffLimited); // Actual compressibility is not roundoff.
  // Shared donor flux before projection: 1-.25*(.5-.25), 1-.25*(.25-.5).
  // An extra initial projection or normalization would incorrectly give1,1.
  EXPECT_EQ(values(path("compressible.vdb"), "density", 2),
            (std::vector<double>{.9375, 1.0625}));
  EXPECT_EQ(result.maxConstantResponseDrift, .0625);
  EXPECT_EQ(result.densityMassBefore, 2);EXPECT_EQ(result.densityMassAfter, 2);
  EXPECT_EQ(result.densityOutwardTransfer, 0);EXPECT_EQ(result.densityBalanceError, 0);
  EXPECT_EQ(result.attempts, 1u);EXPECT_EQ(result.actualSubsteps, 1u);
  ASSERT_EQ(result.steps.size(), 1u);
  EXPECT_EQ(result.totalProjectionIterations, result.steps[0].projectionIterations);
  options.initialVelocity = api::BoundaryFluidInitialVelocity::RequireSolenoidal;
  rejects([&] { (void)step(in, "compressible-strict.vdb"); }, "compressible-strict.vdb");
  EXPECT_EQ(bytes(in.boundary.state), original);
}

TEST_F(OpenVdbBoundaryFluid,
       WeakStoredInflowRetainsScalarFluxWithoutAbsoluteFloorOrHiddenProjection) {
  auto in = fixture(1, 0, {0});
  const double weak = std::ldexp(1., -500);
  mutate(in, [&](auto &grids) {
    auto velocity = openvdb::gridPtrCast<M>(grids[0]);
    auto v = velocity->tree().getValue(C(0));v[0] = weak;
    velocity->tree().setValueOn(C(0), v);
  });
  const auto original = bytes(in.boundary.state);
  options.timeStep = .25;options.reservoirDensity = 1;options.reservoirTemperature = 300;
  const auto result = step(in, "weak.vdb");
  EXPECT_FALSE(result.initialDivergenceWithinTolerance);
  EXPECT_EQ(result.maxDivergenceBefore, weak);
  const auto density = values(path("weak.vdb"), "density", 1);
  ASSERT_EQ(density.size(), 1u);
  EXPECT_GT(density[0], 0);EXPECT_EQ(density[0], weak * .25);
  EXPECT_EQ(result.densityMassAfter, weak * .25);
  EXPECT_EQ(result.densityOutwardTransfer, -weak * .25);
  EXPECT_EQ(result.densityBalanceError, 0);
  EXPECT_EQ(result.maxConstantResponseDrift, weak * .25);
  auto velocity = openvdb::gridPtrCast<M>(read(path("weak.vdb"), "velocity"));
  // The one ordinary pressure solve equalizes the two open X faces. Retained
  // donor influx above uses the original weak face, not a preprojected half.
  EXPECT_EQ(velocity->tree().getValue(C(0))[0], weak * .5);
  EXPECT_EQ(velocity->tree().getValue(C(1, 0, 0))[0], weak * .5);
  EXPECT_EQ(result.attempts, 1u);EXPECT_EQ(result.actualSubsteps, 1u);
  ASSERT_EQ(result.steps.size(), 1u);
  EXPECT_EQ(result.totalProjectionIterations, result.steps[0].projectionIterations);
  options.initialVelocity = api::BoundaryFluidInitialVelocity::RequireSolenoidal;
  rejects([&] { (void)step(in, "weak-strict.vdb"); }, "weak-strict.vdb");
  EXPECT_EQ(bytes(in.boundary.state), original);
}

TEST_F(OpenVdbBoundaryFluid,
       InitialVelocityPolicyValidationAndCapsPrecedePublication) {
  auto in = fixture(1, .25, {0});
  const auto original = bytes(in.boundary.state);
  const auto saved = options;
  options.initialVelocity = static_cast<api::BoundaryFluidInitialVelocity>(99);
  try {
    (void)api::WriteBoundaryFluidStep({}, path("bad-policy.vdb"), options);
    FAIL();
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find("policy"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("bad-policy.vdb")));
  options = saved;options.maxWork = 1;
  rejects([&] { (void)step(in, "policy-work.vdb"); }, "policy-work.vdb");
  options = saved;options.maxInputBytes = 1;
  rejects([&] { (void)step(in, "policy-bytes.vdb"); }, "policy-bytes.vdb");
  options = saved;
  const auto foreign = path("policy-foreign.vdb");std::ofstream(foreign) << "foreign bytes";
  EXPECT_ANY_THROW((void)step(in, "policy-foreign.vdb"));
  EXPECT_EQ(bytes(foreign), "foreign bytes");
  EXPECT_EQ(bytes(in.boundary.state), original);
}

#endif
} // namespace
