#include "Container/geometry/OpenVdbPotentialFlow.h"

#include <algorithm>
#include <atomic>
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
#include <openvdb/io/io.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbPotentialFlowAvailability, DisabledBeforePathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::PotentialFlowOptions options;
  options.maxIterations = 0;
  vdb::ProcessingOptions processing;
  processing.maxInputBytes = 0;
  try {
    (void)vdb::WritePotentialFlow("missing.vdb", {}, options, {}, processing);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
template <class G> class ExactPotentialFixture : public G {
public:
  explicit ExactPotentialFixture(const G &grid)
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
        static_cast<const ExactPotentialFixture &>(*this).copyGrid());
  }
};

class OpenVdbPotentialFlow : public ::testing::Test {
protected:
  using V = openvdb::Vec3d;
  struct Fields {
    openvdb::Vec3DGrid::Ptr flow, neumann;
    openvdb::DoubleGrid::Ptr potential;
    openvdb::BoolGrid::Ptr domain;
  };

  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-potential-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.tolerance = 1e-8;
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             bool statistics = true, const openvdb::MetaMap &metadata = {}) {
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.setGridStatsMetadataEnabled(statistics);
    openvdb::GridPtrVec views;
    for (const auto &grid : grids) {
      if (grid->isType<openvdb::DoubleGrid>())
        views.push_back(
            std::make_shared<ExactPotentialFixture<openvdb::DoubleGrid>>(
                *openvdb::gridPtrCast<openvdb::DoubleGrid>(grid)));
      else if (grid->isType<openvdb::FloatGrid>())
        views.push_back(
            std::make_shared<ExactPotentialFixture<openvdb::FloatGrid>>(
                *openvdb::gridPtrCast<openvdb::FloatGrid>(grid)));
      else
        views.push_back(grid);
    }
    file.write(views, metadata);
  }
  static std::string bytes(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }

  // Analytic distances are authored directly. Active flags describe the full
  // strict three-voxel narrow band; negative clamped values remain inactive.
  template <class G = openvdb::DoubleGrid>
  static typename G::Ptr sphere(double h = 1, double radius = 6.5,
                                const std::vector<V> &centers = {V(0)},
                                bool hollow = false) {
    using T = typename G::ValueType;
    auto grid = G::create(T(3 * h));
    grid->setName("collider");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    if (h < 1e-4) {
      // A persisted native uniform map admits these finite coefficients, while
      // the convenience factory rejects tiny determinants. Author its public
      // record independently of the production exact-map helper.
      std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
      openvdb::writeString(record, openvdb::math::UniformScaleMap::mapType());
      for (const double value : {h, h, 1 / h, 1 / (h * h), .5 / h})
        V(value).write(record);
      record.seekg(0);
      openvdb::io::setCurrentVersion(record);
      grid->transform().read(record);
    } else {
      grid->setTransform(openvdb::math::Transform::createLinearTransform(h));
    }
    grid->insertMeta("artist", openvdb::StringMetadata("analytic source"));
    int minimumX = int(std::floor(centers.front().x() - radius - 4)),
        maximumX = int(std::ceil(centers.front().x() + radius + 4));
    for (const auto &center : centers) {
      minimumX = std::min(minimumX, int(std::floor(center.x() - radius - 4)));
      maximumX = std::max(maximumX, int(std::ceil(center.x() + radius + 4)));
    }
    const int limit = int(std::ceil(radius + 4));
    auto accessor = grid->getAccessor();
    for (int x = minimumX; x <= maximumX; ++x)
      for (int y = -limit; y <= limit; ++y)
        for (int z = -limit; z <= limit; ++z) {
          double distance = std::numeric_limits<double>::infinity();
          for (const auto &center : centers) {
            const double r =
                std::hypot(x - center.x(), y - center.y(), z - center.z());
            distance = std::min(distance, hollow ? std::max(4.5 - r, r - radius)
                                                 : r - radius);
          }
          const openvdb::Coord coordinate(x, y, z);
          if (std::abs(distance) < 3)
            accessor.setValue(coordinate, T(distance * h));
          else if (distance < 0)
            accessor.setValueOff(coordinate, T(-3 * h));
        }
    return grid;
  }
  vdb::PotentialFlowResult run(const std::string &input = "input.vdb",
                               const std::string &output = "output.vdb",
                               const std::string &selector = {}) {
    return vdb::WritePotentialFlow(path(input), path(output), options, selector,
                                   processing);
  }
  Fields read(const std::string &name) {
    openvdb::io::File file(path(name).string());
    file.open(false);
    auto flow = file.readGrid(options.velocityGridName),
         potential = file.readGrid(options.potentialGridName),
         domain = file.readGrid(options.domainGridName),
         neumann = file.readGrid(options.neumannGridName);
    EXPECT_TRUE(flow->isType<openvdb::Vec3DGrid>());
    EXPECT_TRUE(potential->isType<openvdb::DoubleGrid>());
    EXPECT_TRUE(domain->isType<openvdb::BoolGrid>());
    EXPECT_TRUE(neumann->isType<openvdb::Vec3DGrid>());
    Fields fields{openvdb::gridPtrCast<openvdb::Vec3DGrid>(flow),
                  openvdb::gridPtrCast<openvdb::Vec3DGrid>(neumann),
                  openvdb::gridPtrCast<openvdb::DoubleGrid>(potential),
                  openvdb::gridPtrCast<openvdb::BoolGrid>(domain)};
    fields.domain->tree().voxelizeActiveTiles();
    return fields;
  }
  static double norm(const V &v) { return std::hypot(v[0], v[1], v[2]); }
  static std::array<V, 3> axes(const openvdb::GridBase &grid, double &h) {
    std::array<V, 3> basis;
    for (size_t a = 0; a < 3; ++a) {
      V e(0);
      e[a] = 1;
      basis[a] = grid.transform().baseMap()->applyJacobian(e);
      if (a == 0)
        h = norm(basis[a]);
      basis[a] /= h;
    }
    return basis;
  }

  // Reconstruct the physical seven-point equation from PUBLISHED fields. No
  // native Poisson solver or production residual routine is used as an oracle.
  static double measuredResidual(const Fields &fields) {
    double h = 0;
    const auto basis = axes(*fields.potential, h);
    std::set<openvdb::Coord> remaining;
    for (auto it = fields.domain->cbeginValueOn(); it; ++it)
      if (*it)
        remaining.insert(it.getCoord());
    double worst = 0;
    while (!remaining.empty()) {
      std::vector<openvdb::Coord> component{*remaining.begin()};
      remaining.erase(remaining.begin());
      long double rhsPeak = 0, errorPeak = 0;
      for (size_t index = 0; index < component.size(); ++index) {
        const auto c = component[index];
        const long double phi = fields.potential->tree().getValue(c);
        const bool neumann = fields.neumann->tree().isValueOn(c);
        const auto velocity = fields.neumann->tree().getValue(c);
        long double lhs = 0, rhs = 0;
        for (size_t a = 0; a < 3; ++a)
          for (int sign : {-1, 1}) {
            auto q = c;
            q[a] += sign;
            if (fields.domain->tree().isValueOn(q) &&
                fields.domain->tree().getValue(q)) {
              lhs += phi - fields.potential->tree().getValue(q);
              const auto found = remaining.find(q);
              if (found != remaining.end()) {
                component.push_back(q);
                remaining.erase(found);
              }
            } else if (neumann) {
              long double projected = 0;
              for (size_t row = 0; row < 3; ++row)
                projected +=
                    static_cast<long double>(basis[a][row]) * velocity[row];
              rhs += static_cast<long double>(h) * sign * projected;
            } else {
              lhs += phi;
            }
          }
        rhsPeak = std::max(rhsPeak, std::abs(rhs));
        errorPeak = std::max(errorPeak, std::abs(rhs - lhs));
      }
      if (rhsPeak != 0)
        worst = std::max(worst, double(errorPeak / rhsPeak));
      else
        EXPECT_EQ(errorPeak, 0);
    }
    return worst;
  }
  static std::pair<double, double> interiorDiagnostics(const Fields &fields) {
    double h = 0;
    const auto basis = axes(*fields.flow, h);
    double divergence = 0, curl = 0;
    size_t supported = 0;
    for (auto it = fields.domain->cbeginValueOn(); it; ++it) {
      const auto c = it.getCoord();
      bool complete = true;
      for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y)
          for (int z = -2; z <= 2; ++z)
            if (std::abs(x) + std::abs(y) + std::abs(z) <= 2 &&
                !fields.domain->tree().isValueOn(c + openvdb::Coord(x, y, z)))
              complete = false;
      if (!complete)
        continue;
      ++supported;
      double derivative[3][3]{};
      for (size_t direction = 0; direction < 3; ++direction) {
        auto plus = c, minus = c;
        ++plus[direction];
        --minus[direction];
        const auto difference = (fields.flow->tree().getValue(plus) -
                                 fields.flow->tree().getValue(minus)) /
                                (2 * h);
        for (size_t component = 0; component < 3; ++component)
          derivative[direction][component] = basis[component].dot(difference);
      }
      divergence =
          std::max(divergence, std::abs(derivative[0][0] + derivative[1][1] +
                                        derivative[2][2]));
      curl = std::max(curl, norm(V(derivative[1][2] - derivative[2][1],
                                   derivative[2][0] - derivative[0][2],
                                   derivative[0][1] - derivative[1][0])));
    }
    EXPECT_GT(supported, 0u);
    return {divergence, curl};
  }

  std::filesystem::path directory;
  vdb::PotentialFlowOptions options;
  vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbPotentialFlow,
       ZeroFlowFloatAndDoubleRetainBoundaryClassification) {
  auto check = [&]<class G>(const std::string &stem) {
    write(stem + ".vdb", {sphere<G>()});
    options.farFieldVelocity = {0, 0, 0};
    const auto result = run(stem + ".vdb", stem + "-out.vdb");
    ASSERT_TRUE(result.converged);
    EXPECT_GT(result.domainCellCount, 0u);
    EXPECT_GT(result.neumannCellCount, 0u);
    EXPECT_EQ(result.componentCount, 1u);
    EXPECT_EQ(result.residual, 0);
    const auto fields = read(stem + "-out.vdb");
    EXPECT_EQ(fields.domain->activeVoxelCount(), result.domainCellCount);
    EXPECT_EQ(fields.neumann->activeVoxelCount(), result.neumannCellCount);
    double maximum = 0;
    for (auto it = fields.domain->cbeginValueOn(); it; ++it) {
      const auto c = it.getCoord();
      EXPECT_TRUE(*it);
      maximum = std::max({maximum, norm(fields.flow->tree().getValue(c)),
                          std::abs(fields.potential->tree().getValue(c)),
                          norm(fields.neumann->tree().getValue(c))});
    }
    EXPECT_EQ(maximum, 0);
    EXPECT_EQ(measuredResidual(fields), 0);
  };
  check.operator()<openvdb::FloatGrid>("float");
  check.operator()<openvdb::DoubleGrid>("double");
}

TEST_F(OpenVdbPotentialFlow, UniformCoMotionHasZeroPerturbationEverywhere) {
  write("input.vdb", {sphere<>()});
  options.farFieldVelocity = options.colliderVelocity = {.75, -.25, .125};
  const auto result = run();
  ASSERT_TRUE(result.converged);
  EXPECT_GT(result.neumannCellCount, 0u);
  const auto fields = read("output.vdb");
  const V expected(options.farFieldVelocity.data());
  EXPECT_EQ(fields.flow->background(), expected);
  EXPECT_EQ(fields.potential->background(), 0);
  EXPECT_EQ(fields.neumann->background(), V(0));
  EXPECT_EQ(fields.flow->tree().getValue({100, 100, 100}), expected);
  double velocityError = 0, potentialPeak = 0;
  for (auto it = fields.domain->cbeginValueOn(); it; ++it) {
    const auto c = it.getCoord();
    velocityError = std::max(velocityError,
                             norm(fields.flow->tree().getValue(c) - expected));
    potentialPeak =
        std::max(potentialPeak, std::abs(fields.potential->tree().getValue(c)));
  }
  EXPECT_EQ(velocityError, 0);
  EXPECT_EQ(potentialPeak, 0);
  EXPECT_EQ(measuredResidual(fields), 0);
}

TEST_F(OpenVdbPotentialFlow, SphereWindHasSlipBoundaryAndStoredMixedResidual) {
  write("input.vdb", {sphere<openvdb::FloatGrid>()});
  const auto result = run();
  ASSERT_TRUE(result.converged);
  EXPECT_GT(result.iterations, 0u);
  EXPECT_LE(result.iterations, options.maxIterations);
  EXPECT_LE(result.residual, options.tolerance);
  const auto fields = read("output.vdb");
  EXPECT_LE(measuredResidual(fields), options.tolerance * 1.05 + 1e-12);
  ASSERT_TRUE(fields.domain->tree().isValueOn({7, 0, 0}));
  ASSERT_TRUE(fields.neumann->tree().isValueOn({7, 0, 0}));
  EXPECT_NEAR(fields.flow->tree().getValue({7, 0, 0}).x(), 0, 1e-10);
  // The finite outer boundary and voxelized surface preclude an exact 1.5U
  // continuum assertion. Tangential acceleration away from substitution cells
  // still distinguishes obstacle-aware flow from an unchanged wind field.
  ASSERT_TRUE(fields.domain->tree().isValueOn({0, 8, 0}));
  const auto equator = fields.flow->tree().getValue({0, 8, 0});
  EXPECT_GT(equator.x(), 1.03);
  EXPECT_LT(equator.x(), 1.9);
  const auto diagnostics = interiorDiagnostics(fields);
  EXPECT_GT(result.interiorDiagnosticCellCount, 0u);
  EXPECT_TRUE(std::isfinite(result.maxBoundaryNormalError));
  EXPECT_TRUE(std::isfinite(result.maxInteriorDivergence));
  EXPECT_TRUE(std::isfinite(result.maxInteriorCurl));
  EXPECT_LT(diagnostics.first, 1.0);
  EXPECT_LT(diagnostics.second, 1e-8);
  EXPECT_TRUE(fields.flow->isInWorldSpace());
}

TEST_F(OpenVdbPotentialFlow,
       GalileanShiftPreservesPotentialAndAddsUniformFlow) {
  write("input.vdb", {sphere<>()});
  ASSERT_TRUE(run("input.vdb", "base.vdb").converged);
  const auto base = read("base.vdb");
  const V shift(.25, -.4, .1);
  options.farFieldVelocity = {1.25, -.4, .1};
  options.colliderVelocity = {.25, -.4, .1};
  ASSERT_TRUE(run("input.vdb", "shift.vdb").converged);
  const auto shifted = read("shift.vdb");
  EXPECT_EQ(base.domain->activeVoxelCount(),
            shifted.domain->activeVoxelCount());
  double velocityError = 0, potentialError = 0;
  for (auto it = base.domain->cbeginValueOn(); it; ++it) {
    const auto c = it.getCoord();
    velocityError =
        std::max(velocityError, norm(shifted.flow->tree().getValue(c) -
                                     base.flow->tree().getValue(c) - shift));
    potentialError = std::max(potentialError,
                              std::abs(shifted.potential->tree().getValue(c) -
                                       base.potential->tree().getValue(c)));
  }
  EXPECT_LT(velocityError, 2e-6);
  EXPECT_LT(potentialError, 2e-6);
  EXPECT_LE(measuredResidual(shifted), options.tolerance * 1.05 + 1e-12);
}

TEST_F(OpenVdbPotentialFlow,
       RotatedLargeCommonMotionCannotSilentlyEraseRelativeVelocity) {
  auto collider = sphere<>();
  collider->transform().postRotate(.37, openvdb::math::Z_AXIS);
  write("input.vdb", {collider});
  const auto before = bytes(path("input.vdb"));
  options.farFieldVelocity = {300004071070.25, 599996466226.25, 1000000000000.};
  options.colliderVelocity = options.farFieldVelocity;
  ASSERT_TRUE(run("input.vdb", "comoving.vdb").converged);
  const auto comoving = read("comoving.vdb");
  double potentialPeak = 0, neumannPeak = 0;
  for (auto value = comoving.domain->cbeginValueOn(); value; ++value) {
    const auto coordinate = value.getCoord();
    potentialPeak =
        std::max(potentialPeak,
                 std::abs(comoving.potential->tree().getValue(coordinate)));
    neumannPeak = std::max(neumannPeak,
                           norm(comoving.neumann->tree().getValue(coordinate)));
  }
  EXPECT_EQ(potentialPeak, 0);
  EXPECT_EQ(neumannPeak, 0);
  // This is one exactly representable input ULP, not zero relative motion.
  // Independently rotating/normalizing the two large absolute vectors rounds
  // them to the same local vector and can falsely produce phi=N=0.
  options.colliderVelocity[0] = 300004071070.25006;
  ASSERT_DOUBLE_EQ(options.colliderVelocity[0] - options.farFieldVelocity[0],
                   0.00006103515625);
  try {
    (void)run("input.vdb", "weak-disturbance.vdb");
    FAIL() << "unrepresentable stored total flow needs a precision failure; "
              "relative motion must not silently become co-motion";
  } catch (const std::invalid_argument &error) {
    const std::string diagnostic = error.what();
    EXPECT_TRUE(diagnostic.find("precision") != std::string::npos ||
                diagnostic.find("relative motion") != std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("weak-disturbance.vdb")));
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbPotentialFlow, ScaleRotationAndReflectionRestorePhysicalUnits) {
  write("input.vdb", {sphere<>()});
  ASSERT_TRUE(run("input.vdb", "base.vdb").converged);
  const auto base = read("base.vdb");
  constexpr double h = 1.75;
  auto transformed = sphere<>(h);
  transformed->transform().postRotate(.37, openvdb::math::Z_AXIS);
  transformed->transform().postScale(V(-1, 1, 1));
  transformed->transform().postTranslate({17, -5, 9});
  double measuredH = 0;
  const auto basis = axes(*transformed, measuredH);
  const V wind = basis[0];
  options.farFieldVelocity = {wind[0], wind[1], wind[2]};
  write("transformed.vdb", {transformed});
  ASSERT_TRUE(run("transformed.vdb", "transformed-out.vdb").converged);
  const auto fields = read("transformed-out.vdb");
  double velocityError = 0, potentialError = 0;
  for (auto it = base.domain->cbeginValueOn(); it; ++it) {
    const auto c = it.getCoord();
    const V original = base.flow->tree().getValue(c);
    V expected(0);
    for (size_t a = 0; a < 3; ++a)
      expected += basis[a] * original[a];
    velocityError = std::max(velocityError,
                             norm(fields.flow->tree().getValue(c) - expected));
    potentialError = std::max(
        potentialError, std::abs(fields.potential->tree().getValue(c) / h -
                                 base.potential->tree().getValue(c)));
  }
  EXPECT_LT(velocityError, 2e-6);
  EXPECT_LT(potentialError, 2e-6);
  EXPECT_LE(measuredResidual(fields), options.tolerance * 1.05 + 1e-12);
  EXPECT_EQ(fields.flow->transform().baseMap()->applyMap(V(0)),
            transformed->transform().baseMap()->applyMap(V(0)));
  for (size_t a = 0; a < 3; ++a) {
    V e(0);
    e[a] = 1;
    EXPECT_EQ(fields.flow->transform().baseMap()->applyJacobian(e),
              transformed->transform().baseMap()->applyJacobian(e));
  }
}

TEST_F(OpenVdbPotentialFlow,
       ExtremeAllowedVoxelSizesScalePotentialNotVelocity) {
  write("base.vdb", {sphere<>()});
  ASSERT_TRUE(run("base.vdb", "base-out.vdb").converged);
  const auto base = read("base-out.vdb");
  for (const double h : {1e-6, 1e6}) {
    const std::string stem = h < 1 ? "small" : "large";
    write(stem + ".vdb", {sphere<>(h)});
    ASSERT_TRUE(run(stem + ".vdb", stem + "-out.vdb").converged);
    const auto fields = read(stem + "-out.vdb");
    double velocityError = 0, potentialError = 0;
    for (auto it = base.domain->cbeginValueOn(); it; ++it) {
      const auto c = it.getCoord();
      velocityError =
          std::max(velocityError, norm(fields.flow->tree().getValue(c) -
                                       base.flow->tree().getValue(c)));
      potentialError = std::max(
          potentialError, std::abs(fields.potential->tree().getValue(c) / h -
                                   base.potential->tree().getValue(c)));
    }
    EXPECT_LT(velocityError, 2e-6);
    EXPECT_LT(potentialError, 2e-6);
    EXPECT_LE(measuredResidual(fields), options.tolerance * 1.05 + 1e-12);
  }
}

TEST_F(OpenVdbPotentialFlow,
       RemoteIndexRecenteringRestoresAllPublishedCoordinates) {
  const auto original = sphere<>();
  write("input.vdb", {original});
  ASSERT_TRUE(run("input.vdb", "base.vdb").converged);
  const auto base = read("base.vdb");
  const openvdb::Coord shift(1'000'000'003, -800'000'001, 600'000'005);
  auto remote = openvdb::DoubleGrid::create(original->background());
  remote->setName("remote-index");
  remote->setGridClass(openvdb::GRID_LEVEL_SET);
  remote->setTransform(original->transform().copy());
  auto accessor = remote->getAccessor();
  for (auto value = original->cbeginValueAll(); value; ++value) {
    if (!value.isValueOn() && *value == original->background())
      continue;
    if (value.isVoxelValue()) {
      const auto coordinate = value.getCoord() + shift;
      if (value.isValueOn())
        accessor.setValue(coordinate, *value);
      else
        accessor.setValueOff(coordinate, *value);
    } else {
      auto box = value.getBoundingBox();
      box.translate(shift);
      remote->tree().fill(box, *value, value.isValueOn());
    }
  }
  write("remote.vdb", {remote});
  const auto before = bytes(path("remote.vdb"));
  ASSERT_TRUE(run("remote.vdb", "remote-out.vdb").converged);
  const auto shifted = read("remote-out.vdb");
  EXPECT_EQ(shifted.domain->activeVoxelCount(),
            base.domain->activeVoxelCount());
  EXPECT_EQ(shifted.neumann->activeVoxelCount(),
            base.neumann->activeVoxelCount());
  EXPECT_FALSE(shifted.domain->tree().isValueOn({7, 0, 0}));
  size_t missingDomain = 0, missingPotential = 0, missingFlow = 0,
         missingNeumann = 0;
  double velocityError = 0, potentialError = 0, neumannError = 0;
  for (auto value = base.domain->cbeginValueOn(); value; ++value) {
    const auto c = value.getCoord(), q = c + shift;
    missingDomain += !shifted.domain->tree().isValueOn(q);
    missingPotential += !shifted.potential->tree().isValueOn(q);
    missingFlow += !shifted.flow->tree().isValueOn(q);
    velocityError =
        std::max(velocityError, norm(shifted.flow->tree().getValue(q) -
                                     base.flow->tree().getValue(c)));
    potentialError = std::max(potentialError,
                              std::abs(shifted.potential->tree().getValue(q) -
                                       base.potential->tree().getValue(c)));
    if (base.neumann->tree().isValueOn(c)) {
      missingNeumann += !shifted.neumann->tree().isValueOn(q);
      neumannError =
          std::max(neumannError, norm(shifted.neumann->tree().getValue(q) -
                                      base.neumann->tree().getValue(c)));
    }
  }
  EXPECT_EQ(missingDomain + missingPotential + missingFlow + missingNeumann,
            0u);
  EXPECT_LT(velocityError, 2e-6);
  EXPECT_LT(potentialError, 2e-6);
  EXPECT_LT(neumannError, 1e-12);
  EXPECT_LE(measuredResidual(shifted), options.tolerance * 1.05 + 1e-12);
  EXPECT_EQ(shifted.flow->transform().baseMap()->applyMap(V(0)), V(0));
  for (size_t a = 0; a < 3; ++a) {
    V e(0);
    e[a] = 1;
    EXPECT_EQ(shifted.flow->transform().baseMap()->applyJacobian(e), e);
  }
  EXPECT_EQ(bytes(path("remote.vdb")), before);
}

TEST_F(OpenVdbPotentialFlow, DisconnectedExteriorComponentsAllConverge) {
  write("input.vdb", {sphere<>(1, 4.5, {V(-12, 0, 0), V(12, 0, 0)})});
  const auto result = run();
  ASSERT_TRUE(result.converged);
  EXPECT_EQ(result.componentCount, 2u);
  EXPECT_GT(result.neumannCellCount, 0u);
  EXPECT_LE(result.iterations, options.maxIterations);
  EXPECT_LE(measuredResidual(read("output.vdb")),
            options.tolerance * 1.05 + 1e-12);
}

TEST_F(OpenVdbPotentialFlow, ClosedShellRejectsUngaugedPureNeumannCavity) {
  write("input.vdb", {sphere<>(1, 8.5, {V(0)}, true)});
  for (const bool stationary : {false, true}) {
    if (stationary)
      options.farFieldVelocity = {0, 0, 0};
    const std::string output = stationary ? "still.vdb" : "wind.vdb";
    try {
      (void)run("input.vdb", output);
      FAIL() << "a closed cavity needs an explicit compatible gauge";
    } catch (const std::invalid_argument &error) {
      const std::string message = error.what();
      EXPECT_TRUE(message.find("Neumann") != std::string::npos ||
                  message.find("Dirichlet") != std::string::npos);
    }
    EXPECT_FALSE(std::filesystem::exists(path(output)));
  }
}

TEST_F(OpenVdbPotentialFlow, NamesMetadataUnicodeAndOutputProtection) {
  auto collider = sphere<>();
  collider->insertMeta("container:pressure_units",
                       openvdb::StringMetadata("stale units"));
  openvdb::MetaMap metadata;
  metadata.insertMeta("project", openvdb::Int64Metadata(INT64_MAX));
  write("input.vdb", {collider}, true, metadata);
  const auto source = directory / std::filesystem::path(u"collider_球.vdb"),
             output = directory / std::filesystem::path(u"flow_球.vdb");
  std::filesystem::copy_file(path("input.vdb"), source);
  const auto sourceBefore = bytes(source);
  options.velocityGridName = "descriptor:wind";
  options.potentialGridName = "descriptor:phi";
  options.domainGridName = "descriptor:exterior";
  options.neumannGridName = "descriptor:boundary";
  const auto result = vdb::WritePotentialFlow(
      source, output, options, "descriptor:collider", processing);
  ASSERT_EQ(result.grids.size(), 4u);
  EXPECT_EQ(bytes(source), sourceBefore);
  // Native OpenVDB narrow paths cannot open every Unicode Windows filename;
  // publication is checked at that path and decoded through an ASCII copy.
  std::filesystem::copy_file(output, path("named-output.vdb"));
  openvdb::io::File file(path("named-output.vdb").string());
  file.open(false);
  EXPECT_EQ(file.getMetadata()->metaValue<int64_t>("project"), INT64_MAX);
  const auto grids = file.getGrids();
  ASSERT_EQ(grids->size(), 4u);
  for (const auto &grid : *grids) {
    EXPECT_EQ(grid->metaValue<std::string>("artist"), "analytic source");
    EXPECT_FALSE((*grid)["container:pressure_units"]);
  }
  file.close();
  const auto outputBefore = bytes(output);
  EXPECT_THROW(
      (void)vdb::WritePotentialFlow(source, output, options, {}, processing),
      std::invalid_argument);
  EXPECT_THROW(
      (void)vdb::WritePotentialFlow(source, source, options, {}, processing),
      std::invalid_argument);
  EXPECT_EQ(bytes(output), outputBefore);
  EXPECT_EQ(bytes(source), sourceBefore);
}

TEST_F(OpenVdbPotentialFlow, CapsAndUnconvergedSolveLeaveNoPublicOutput) {
  write("input.vdb", {sphere<>()});
  const auto before = bytes(path("input.vdb"));
  options.maxSolverVoxels = 1;
  EXPECT_THROW((void)run(), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("output.vdb")));
  options.maxSolverVoxels = 100000;
  options.maxWorkItems = 1;
  EXPECT_THROW((void)run(), std::invalid_argument);
  options.maxWorkItems = 8'000'000'000ull;
  options.maxMetadataBytes = 1;
  EXPECT_THROW((void)run(), std::invalid_argument);
  options.maxMetadataBytes = 1024 * 1024;
  processing.maxVoxelCount = 512;
  EXPECT_THROW((void)run(), std::invalid_argument);
  processing.maxVoxelCount = 16'000'000;
  processing.maxInputBytes = 4 * 1024 * 1024;
  EXPECT_THROW((void)run(), std::invalid_argument);
  processing.maxInputBytes = 512ull * 1024 * 1024;
  options.maxIterations = 1;
  options.tolerance = 1e-12;
  EXPECT_ANY_THROW((void)run());
  EXPECT_FALSE(std::filesystem::exists(path("output.vdb")));
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbPotentialFlow, InvalidSourcesAndOptionsRejectBeforePublication) {
  auto collider = sphere<>();
  write("input.vdb", {collider});
  options.exteriorWidth = 4;
  EXPECT_THROW((void)run(), std::invalid_argument);
  options.exteriorWidth = 6;
  options.potentialGridName = options.velocityGridName;
  EXPECT_THROW((void)run(), std::invalid_argument);
  options.potentialGridName = "potential";
  options.farFieldVelocity[0] = std::numeric_limits<double>::infinity();
  EXPECT_THROW((void)run(), std::invalid_argument);
  options.farFieldVelocity = {1, 0, 0};
  write("no-stats.vdb", {collider}, false);
  EXPECT_THROW((void)run("no-stats.vdb"), std::invalid_argument);
  auto malformed = collider->deepCopy();
  malformed->tree().setValue({0, 0, 0}, NAN);
  write("nan.vdb", {malformed});
  EXPECT_THROW((void)run("nan.vdb"), std::invalid_argument);
  malformed = collider->deepCopy();
  malformed->setGridClass(openvdb::GRID_FOG_VOLUME);
  write("fog.vdb", {malformed});
  EXPECT_THROW((void)run("fog.vdb"), std::invalid_argument);
  malformed = collider->deepCopy();
  malformed->transform().postScale(V(1, 2, 1));
  write("nonuniform.vdb", {malformed});
  EXPECT_THROW((void)run("nonuniform.vdb"), std::invalid_argument);
  malformed = collider->deepCopy();
  malformed->tree().setValue({INT32_MAX - 1, 0, 0}, 1);
  write("coordinate.vdb", {malformed});
  EXPECT_THROW((void)run("coordinate.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("output.vdb")));
}
#endif
} // namespace
