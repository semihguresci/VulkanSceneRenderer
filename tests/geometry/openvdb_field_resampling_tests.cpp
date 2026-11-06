#include "Container/geometry/OpenVdbFieldResampling.h"
#include <atomic>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Count.h>
#endif
namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbFieldResamplingAvailability, DisabledBeforeAllOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FieldResamplingOptions o;
  o.maxWorkItems = 0;
  o.worldFromWorld[15] = 0;
  vdb::ProcessingOptions p;
  p.maxInputBytes = 0;
  try {
    (void)vdb::WriteResampledField("missing", "missing/out", o, {}, p);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = openvdb::Vec3d;
using C = openvdb::Coord;
template <class Grid> class ExactFixture : public Grid {
public:
  explicit ExactFixture(const Grid &g)
      : Grid(openvdb::ConstPtrCast<typename Grid::TreeType>(g.constTreePtr())) {
    this->insertMeta(g);
    this->setTransform(
        std::make_shared<openvdb::math::Transform>(g.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, std::make_shared<openvdb::math::Transform>(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactFixture &>(*this).copyGrid());
  }
};
class OpenVdbFieldResampling : public ::testing::Test {
protected:
  std::filesystem::path directory;
  vdb::ProcessingOptions p;
  vdb::FieldResamplingOptions o;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-field-resampling-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    p.gridName = "sampled";
  }
  void TearDown() override {
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      if (entry.is_regular_file())
        std::filesystem::remove(entry.path(), error);
    std::filesystem::remove(directory, error);
  }
  auto path(const std::string &name) const { return directory / name; }
  std::string bytes(const std::filesystem::path &name) {
    std::ifstream file(name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  template <class Grid>
  void write(const std::string &name, const typename Grid::Ptr &grid,
             const openvdb::MetaMap &metadata = {}) {
    if (grid->getName().empty())
      grid->setName("input");
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(openvdb::GridPtrVec{std::make_shared<ExactFixture<Grid>>(*grid)},
               metadata);
    file.close();
  }
  template <class Grid>
  typename Grid::Ptr read(const std::string &name = "out.vdb") {
    openvdb::io::File file(path(name).string());
    file.open(false);
    auto grid = openvdb::gridPtrCast<Grid>(file.readGrid("sampled"));
    file.close();
    if (!grid)
      throw std::runtime_error("wrong test output native grid type");
    return grid;
  }
  auto run(const std::string &source = "in.vdb",
           const std::string &output = "out.vdb") {
    return vdb::WriteResampledField(path(source), path(output), o, {}, p);
  }
  template <class T> void equalBits(const T &a, const T &b) {
    if constexpr (std::is_same_v<T, float>)
      EXPECT_EQ(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b));
    else if constexpr (std::is_same_v<T, double>)
      EXPECT_EQ(std::bit_cast<uint64_t>(a), std::bit_cast<uint64_t>(b));
    else if constexpr (std::is_arithmetic_v<T>)
      EXPECT_EQ(a, b);
    else
      for (int k = 0; k < 3; ++k)
        equalBits(a[k], b[k]);
  }
  template <class Grid>
  void nearestType(const std::string &id, typename Grid::ValueType negative,
                   typename Grid::ValueType inactive) {
    auto grid = Grid::create(typename Grid::ValueType(0));
    grid->tree().setValueOn({-1, 0, 0}, negative);
    grid->tree().setValueOn({0, 0, 0}, typename Grid::ValueType(0));
    grid->tree().setValueOff({1, 0, 0}, inactive);
    write<Grid>(id + ".vdb", grid);
    const auto source = bytes(path(id + ".vdb"));
    o.interpolation = vdb::FieldResamplingInterpolation::Nearest;
    o.worldFromWorld[3] = 2;
    const auto report = run(id + ".vdb", id + "-out.vdb");
    EXPECT_FALSE(report.exactCopy);
    EXPECT_GT(report.sampledVoxelCount, 0);
    auto out = read<Grid>(id + "-out.vdb");
    for (int x = -1; x <= 1; ++x) {
      equalBits(out->tree().getValue({x + 2, 0, 0}),
                grid->tree().getValue({x, 0, 0}));
      EXPECT_EQ(out->tree().isValueOn({x + 2, 0, 0}),
                grid->tree().isValueOn({x, 0, 0}));
    }
    EXPECT_EQ(bytes(path(id + ".vdb")), source);
  }
};
TEST_F(OpenVdbFieldResampling,
       NineNativeNearestTypesKeepExactValuesActivityAndInt64Endpoints) {
  nearestType<openvdb::FloatGrid>("float", -1.f, -0.f);
  nearestType<openvdb::DoubleGrid>("double", -1., -0.);
  nearestType<openvdb::BoolGrid>("bool", true, true);
  nearestType<openvdb::MaskGrid>("mask", true, true);
  nearestType<openvdb::Int32Grid>("int32", INT32_MIN, INT32_MAX);
  nearestType<openvdb::Int64Grid>("int64", INT64_MIN, INT64_MAX);
  nearestType<openvdb::Vec3IGrid>("vec3i", {INT32_MIN, INT32_MAX, -7},
                                  {INT32_MAX, INT32_MIN, 8});
  nearestType<openvdb::Vec3SGrid>("vec3s", {-1, 2, 3}, {-0.f, 0, 1});
  nearestType<openvdb::Vec3DGrid>("vec3d", {-1, 2, 3}, {-0., 0, 1});
}
TEST_F(OpenVdbFieldResampling,
       IdentityCopiesStoredBitsAndBitwiseInactiveSupportSurvivesTranslation) {
  auto grid = openvdb::DoubleGrid::create(0);
  // Tree-level insertion may optimize an inactive value equal to background.
  // The native leaf buffer must explicitly retain the distinct sign bit.
  grid->tree().touchLeaf({1, 0, 0})->setValueOff({1, 0, 0}, -0.);
  grid->tree().setValueOff({2, 0, 0}, 1e-300);
  write<openvdb::DoubleGrid>("in.vdb", grid);
  auto report = run("in.vdb", "copy.vdb");
  EXPECT_TRUE(report.exactCopy);
  EXPECT_EQ(report.sampledVoxelCount, 0);
  EXPECT_EQ(report.inputStoredLeafCount, report.outputStoredLeafCount);
  auto copy = read<openvdb::DoubleGrid>("copy.vdb");
  equalBits(copy->tree().getValue({1, 0, 0}), -0.);
  equalBits(copy->tree().getValue({2, 0, 0}), 1e-300);
  o.interpolation = vdb::FieldResamplingInterpolation::Nearest;
  o.worldFromWorld[3] = 2;
  report = run();
  ASSERT_TRUE(report.sourceSupportBounds);
  EXPECT_EQ((*report.sourceSupportBounds)[0], 1);
  EXPECT_EQ((*report.sourceSupportBounds)[3], 2);
  auto moved = read<openvdb::DoubleGrid>();
  equalBits(moved->tree().getValue({3, 0, 0}), -0.);
  equalBits(moved->tree().getValue({4, 0, 0}), 1e-300);
  EXPECT_FALSE(moved->tree().isValueOn({3, 0, 0}));
}
TEST_F(OpenVdbFieldResampling,
       NearestNegativeHalfTiesUseGreaterIntegerWithoutNumericPromotion) {
  auto grid = openvdb::Int64Grid::create(0);
  grid->tree().setValue({-1, 0, 0}, INT64_MIN);
  grid->tree().setValue({0, 0, 0}, INT64_MAX);
  write<openvdb::Int64Grid>("in.vdb", grid);
  o.interpolation = vdb::FieldResamplingInterpolation::Nearest;
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, -.5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  (void)run();
  auto out = read<openvdb::Int64Grid>();
  EXPECT_EQ(out->tree().getValue({0, 0, 0}), INT64_MAX);
  EXPECT_EQ(out->tree().getValue({-1, 0, 0}), INT64_MIN);
}
TEST_F(OpenVdbFieldResampling,
       NearestValueAndActivityKeepTheRepresentableValueBelowHalfSeparate) {
  auto grid = openvdb::Int64Grid::create(0);
  grid->tree().setValueOff({0, 0, 0}, INT64_MIN);
  grid->tree().setValueOn({1, 0, 0}, INT64_MAX);
  write<openvdb::Int64Grid>("in.vdb", grid);
  o.interpolation = vdb::FieldResamplingInterpolation::Nearest;
  for (const bool half : {false, true}) {
    const double translation = half ? .5 : std::nextafter(.5, 0.);
    o.targetIndexToWorld = std::array<double, 16>{
        1, 0, 0, translation, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto output = half ? "half.vdb" : "below.vdb";
    (void)run("in.vdb", output);
    const auto out = read<openvdb::Int64Grid>(output);
    EXPECT_EQ(out->tree().getValue({0, 0, 0}), half ? INT64_MAX : INT64_MIN);
    EXPECT_EQ(out->tree().isValueOn({0, 0, 0}), half);
  }
}
TEST_F(OpenVdbFieldResampling,
       LostNegativeFractionAndQuadraticWeightsFailWithoutPublishing) {
  auto grid = openvdb::DoubleGrid::create(0);
  grid->tree().setValue({-1, 0, 0}, 1e308);
  write<openvdb::DoubleGrid>("in.vdb", grid);
  // At cell 0 the exact linear result is Float::denorm_min * 1e308,
  // approximately 1.4e263. Rounding q-floor(q) to 1 must not erase it.
  for (const bool quadratic : {false, true}) {
    o.interpolation = quadratic ? vdb::FieldResamplingInterpolation::Quadratic
                                : vdb::FieldResamplingInterpolation::Linear;
    o.targetIndexToWorld = std::array<double, 16>{
        1, 0, 0, -double(std::numeric_limits<float>::denorm_min()),
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1};
    const auto output =
        quadratic ? "fraction-quadratic.vdb" : "fraction-linear.vdb";
    try {
      (void)run("in.vdb", output);
      FAIL() << "unrepresentable interpolation fraction accepted";
    } catch (const std::invalid_argument &error) {
      EXPECT_NE(std::string(error.what()).find("fraction"), std::string::npos);
    }
    EXPECT_FALSE(std::filesystem::exists(path(output)));
  }
  // The left quadratic contribution is approximately -2.47e-16. Forming
  // .5*t in Double first would round its nonzero coefficient to zero.
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, std::numeric_limits<double>::denorm_min(),
                             0, 1, 0, 0,
                             0, 0, 1, 0,
                             0, 0, 0, 1};
  try {
    (void)run("in.vdb", "weights.vdb");
    FAIL() << "unrepresentable quadratic weight accepted";
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find("kernel weights"),
              std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("weights.vdb")));
}
TEST_F(OpenVdbFieldResampling,
       LinearAndQuadraticMatchIndependentWorldPolynomialAndAffineOracles) {
  for (const bool quadratic : {false, true}) {
    auto grid = openvdb::DoubleGrid::create(0);
    for (int z = -2; z <= 2; ++z)
      for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
          grid->tree().setValue(
              {x, y, z}, quadratic
                             ? double(x * x + 2 * y * y + 3 * z * z + x * y)
                             : double(2 * x - 3 * y + z + 5));
    const auto name = quadratic ? "quadratic.vdb" : "linear.vdb",
               output = quadratic ? "quadratic-out.vdb" : "linear-out.vdb";
    write<openvdb::DoubleGrid>(name, grid);
    o.interpolation = quadratic ? vdb::FieldResamplingInterpolation::Quadratic
                                : vdb::FieldResamplingInterpolation::Linear;
    o.targetIndexToWorld = std::array<double, 16>{
        .5, .25, 0, .25, 0, .5, 0, .25, 0, 0, -.5, .25, 0, 0, 0, 1};
    (void)run(name, output);
    auto out = read<openvdb::DoubleGrid>(output);
    for (int z = -1; z <= 1; ++z)
      for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
          const double a = .5 * x + .25 * y + .25, b = .5 * y + .25,
                       c = -.5 * z + .25;
          const double expected = quadratic
                                      ? a * a + 2 * b * b + 3 * c * c + a * b
                                      : 2 * a - 3 * b + c + 5;
          EXPECT_NEAR(out->tree().getValue({x, y, z}), expected, 2e-12);
          EXPECT_TRUE(out->tree().isValueOn({x, y, z}));
        }
    const auto origin = out->transform().indexToWorld(V(0));
    EXPECT_EQ(origin, V(.25, .25, .25));
    EXPECT_EQ(out->transform().baseMap()->applyJacobian({0, 0, 1}),
              V(0, 0, -.5));
  }
}
TEST_F(OpenVdbFieldResampling,
       InactiveValuesParticipateWhileActivityUsesOnlyNonzeroKernelSupport) {
  auto grid = openvdb::DoubleGrid::create(0);
  grid->tree().setValueOff({0, 0, 0}, 2);
  grid->tree().setValueOff({1, 0, 0}, 4);
  grid->tree().setValueOn({1, 1, 0}, 0);
  write<openvdb::DoubleGrid>("in.vdb", grid);
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, .5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  (void)run("in.vdb", "fraction.vdb");
  auto out = read<openvdb::DoubleGrid>("fraction.vdb");
  EXPECT_DOUBLE_EQ(out->tree().getValue({0, 0, 0}), 3);
  EXPECT_FALSE(out->tree().isValueOn({0, 0, 0}));
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, .5, 0, 1, 0, .5, 0, 0, 1, 0, 0, 0, 0, 1};
  (void)run("in.vdb", "any.vdb");
  out = read<openvdb::DoubleGrid>("any.vdb");
  EXPECT_TRUE(out->tree().isValueOn({0, 0, 0}));
  o.activity = vdb::FieldResamplingActivity::NearestSource;
  (void)run("in.vdb", "nearest-activity.vdb");
  out = read<openvdb::DoubleGrid>("nearest-activity.vdb");
  EXPECT_TRUE(out->tree().isValueOn({0, 0, 0}));
  o.targetIndexToWorld = std::array<double, 16>{1, 0, 0, .25, 0, 1, 0, .25,
                                                0, 0, 1, 0,   0, 0, 0, 1};
  (void)run("in.vdb", "nearest-off.vdb");
  out = read<openvdb::DoubleGrid>("nearest-off.vdb");
  EXPECT_FALSE(out->tree().isValueOn({0, 0, 0}));
}
TEST_F(OpenVdbFieldResampling,
       QuadraticIntegerCentersKeepSubnormalsDespiteExtremeIrrelevantNeighbors) {
  auto grid = openvdb::DoubleGrid::create(0);
  const auto tiny = std::numeric_limits<double>::denorm_min();
  grid->tree().setValue({0, 0, 0}, tiny);
  grid->tree().setValue({-1, 0, 0}, DBL_MAX);
  grid->tree().setValue({1, 0, 0}, -DBL_MAX);
  write<openvdb::DoubleGrid>("in.vdb", grid);
  o.interpolation = vdb::FieldResamplingInterpolation::Quadratic;
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  (void)run();
  auto out = read<openvdb::DoubleGrid>();
  equalBits(out->tree().getValue({-1, 0, 0}), tiny);
  EXPECT_EQ(out->tree().getValue({-2, 0, 0}), DBL_MAX);
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, .5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  EXPECT_THROW(run("in.vdb", "range.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("range.vdb")));
}
TEST_F(OpenVdbFieldResampling,
       LinearExtremeDifferencesRemainFiniteAndNativeFloatUnderflowRejects) {
  auto grid = openvdb::DoubleGrid::create(0);
  grid->tree().setValue({0, 0, 0}, -DBL_MAX);
  grid->tree().setValue({1, 0, 0}, DBL_MAX);
  write<openvdb::DoubleGrid>("in.vdb", grid);
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, .75, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  (void)run();
  EXPECT_NEAR(read<openvdb::DoubleGrid>()->tree().getValue({0, 0, 0}) / DBL_MAX,
              .5, 2e-16);
  auto small = openvdb::FloatGrid::create(0);
  small->tree().setValue({1, 0, 0}, std::numeric_limits<float>::denorm_min());
  write<openvdb::FloatGrid>("small.vdb", small);
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, .5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  EXPECT_THROW(run("small.vdb", "underflow.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("underflow.vdb")));
}
TEST_F(OpenVdbFieldResampling,
       AllFiveVectorSemanticsTransformStoredValuesAndInfiniteBackground) {
  const std::array<openvdb::VecType, 5> tags{
      openvdb::VEC_INVARIANT, openvdb::VEC_COVARIANT,
      openvdb::VEC_COVARIANT_NORMALIZE, openvdb::VEC_CONTRAVARIANT_RELATIVE,
      openvdb::VEC_CONTRAVARIANT_ABSOLUTE};
  for (const auto tag : tags) {
    auto grid = openvdb::Vec3DGrid::create({2, 3, 4});
    grid->setVectorType(tag);
    grid->setIsInWorldSpace(true);
    grid->tree().setValue({0, 0, 0}, {2, 3, 4});
    const auto id = std::to_string(int(tag));
    write<openvdb::Vec3DGrid>(id + ".vdb", grid);
    o.worldFromWorld = {2, 0, 0, 5, 0, -1, 0, 6, 0, 0, .5, 7, 0, 0, 0, 1};
    (void)run(id + ".vdb", id + "-out.vdb");
    auto out = read<openvdb::Vec3DGrid>(id + "-out.vdb");
    V expected(2, 3, 4);
    if (tag == openvdb::VEC_CONTRAVARIANT_RELATIVE)
      expected = {4, -3, 2};
    else if (tag == openvdb::VEC_CONTRAVARIANT_ABSOLUTE)
      expected = {9, 3, 9};
    else if (tag == openvdb::VEC_COVARIANT ||
             tag == openvdb::VEC_COVARIANT_NORMALIZE) {
      expected = {1, -3, 8};
      if (tag == openvdb::VEC_COVARIANT_NORMALIZE)
        expected /= std::sqrt(74.);
    }
    for (int k = 0; k < 3; ++k) {
      EXPECT_NEAR(out->background()[k], expected[k], 1e-14);
      EXPECT_NEAR(out->tree().getValue({5, 6, 7})[k], expected[k], 1e-14);
    }
    EXPECT_EQ(out->getVectorType(), tag);
    EXPECT_TRUE(out->isInWorldSpace());
    o.vectorValues = vdb::FieldVectorTransform::PreserveWorldComponents;
    (void)run(id + ".vdb", id + "-raw.vdb");
    EXPECT_EQ(read<openvdb::Vec3DGrid>(id + "-raw.vdb")->background(),
              V(2, 3, 4));
    o.vectorValues = vdb::FieldVectorTransform::Semantic;
  }
}
TEST_F(OpenVdbFieldResampling,
       Vec3IExactDyadicTransformsRejectHiddenFractionsAndNonaxisNormals) {
  auto grid = openvdb::Vec3IGrid::create({2, 4, 6});
  grid->setVectorType(openvdb::VEC_CONTRAVARIANT_ABSOLUTE);
  grid->tree().setValue({0, 0, 0}, {2, 4, 6});
  write<openvdb::Vec3IGrid>("in.vdb", grid);
  o.interpolation = vdb::FieldResamplingInterpolation::Nearest;
  o.worldFromWorld = {2, 0, 0, 1, 0, -.5, 0, 2, 0, 0, 1, 3, 0, 0, 0, 1};
  (void)run();
  EXPECT_EQ(read<openvdb::Vec3IGrid>()->background(), openvdb::Vec3i(5, 0, 9));
  grid = openvdb::Vec3IGrid::create({1000000000, 0, 0});
  grid->setVectorType(openvdb::VEC_CONTRAVARIANT_ABSOLUTE);
  grid->tree().setValue({0, 0, 0}, {1000000000, 0, 0});
  write<openvdb::Vec3IGrid>("fraction.vdb", grid);
  o.worldFromWorld = {1, 0, 0, 1e-12, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  try {
    (void)run("fraction.vdb", "bad.vdb");
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("exact dyadic"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  grid = openvdb::Vec3IGrid::create({INT32_MAX, 1, 0});
  grid->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);
  grid->tree().setValue({0, 0, 0}, {INT32_MAX, 1, 0});
  write<openvdb::Vec3IGrid>("normal.vdb", grid);
  o.worldFromWorld[3] = 1;
  EXPECT_THROW(run("normal.vdb", "axis.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("axis.vdb")));
  grid = openvdb::Vec3IGrid::create({3, 0, 0});
  grid->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);
  grid->tree().setValue({0, 0, 0}, {3, 0, 0});
  write<openvdb::Vec3IGrid>("axis-in.vdb", grid);
  (void)run("axis-in.vdb", "axis-out.vdb");
  EXPECT_EQ(read<openvdb::Vec3IGrid>("axis-out.vdb")->background(),
            openvdb::Vec3i(1, 0, 0));
  o.worldFromWorld = {.8, -.6, 0, 0, .6, .8, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  EXPECT_THROW(run("axis-in.vdb", "general.vdb"), std::invalid_argument);
  o.vectorValues = vdb::FieldVectorTransform::PreserveWorldComponents;
  EXPECT_NO_THROW((void)run("axis-in.vdb", "general-raw.vdb"));
}
TEST_F(OpenVdbFieldResampling,
       NativeFloatVectorRoundingIsReportedAndIdentityDoesNotNormalize) {
  auto grid = openvdb::Vec3SGrid::create({2, 3, 4});
  grid->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);
  grid->tree().setValue({0, 0, 0}, {2, 3, 4});
  write<openvdb::Vec3SGrid>("in.vdb", grid);
  auto copied = run("in.vdb", "copy.vdb");
  EXPECT_TRUE(copied.exactCopy);
  EXPECT_EQ(read<openvdb::Vec3SGrid>("copy.vdb")->background(),
            openvdb::Vec3f(2, 3, 4));
  o.worldFromWorld[3] = 1;
  const auto result = run();
  EXPECT_GT(result.roundedFloatComponents, 0);
  EXPECT_GT(result.maximumFloatAbsoluteError, 0);
  const auto value = read<openvdb::Vec3SGrid>()->background();
  for (int k = 0; k < 3; ++k)
    EXPECT_NEAR(value[k], V(2, 3, 4)[k] / std::sqrt(29.), 1e-7);
}
TEST_F(OpenVdbFieldResampling,
       ClosedFiniteDomainsMapsNodeWorkAndSemanticsFailBeforePublication) {
  auto grid = openvdb::DoubleGrid::create(0);
  grid->tree().setValue({0, 0, 0}, 1);
  write<openvdb::DoubleGrid>("in.vdb", grid);
  o.maxWorkItems = 512;
  try {
    (void)run();
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("node-table"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  o.maxWorkItems = 1'000'000'000;
  p.maxVoxelCount = 512;
  EXPECT_THROW(run(), std::invalid_argument);
  p.maxVoxelCount = 16'000'000;
  o.targetIndexToWorld = std::array<double, 16>{1, 1, 0, 0, 0, 1e-10, 0, 0,
                                                0, 0, 1, 0, 0, 0,     0, 1};
  EXPECT_THROW(run(), std::invalid_argument);
  o.targetIndexToWorld.reset();
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  write<openvdb::DoubleGrid>("sdf.vdb", grid);
  EXPECT_THROW(run("sdf.vdb"), std::invalid_argument);
  grid->setGridClass(openvdb::GRID_UNKNOWN);
  grid->insertMeta(openvdb::GridBase::META_VECTOR_TYPE,
                   openvdb::StringMetadata("unexpected"));
  write<openvdb::DoubleGrid>("tag.vdb", grid);
  EXPECT_THROW(run("tag.vdb"), std::invalid_argument);
  grid->removeMeta(openvdb::GridBase::META_VECTOR_TYPE);
  grid->insertMeta(openvdb::GridBase::META_VECTOR_TYPE,
                   openvdb::Int32Metadata(0));
  write<openvdb::DoubleGrid>("typed-tag.vdb", grid);
  EXPECT_THROW(run("typed-tag.vdb"), std::invalid_argument);
  grid->removeMeta(openvdb::GridBase::META_VECTOR_TYPE);
  grid->tree().setValueOff({1, 0, 0}, INFINITY);
  write<openvdb::DoubleGrid>("nonfinite.vdb", grid);
  EXPECT_THROW(run("nonfinite.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbFieldResampling,
       FullTypedMemoryAndAggregateFileMetadataAreChargedBeforePaging) {
  auto grid = openvdb::DoubleGrid::create(0);
  for (int x = 0; x < 128; ++x)
    grid->tree().setValue({8 * x, 0, 0}, 1);
  grid->setName("input");
  grid->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                   openvdb::Int64Metadata(1));
  grid->insertMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT,
                   openvdb::Int64Metadata(128));
  {
    openvdb::io::File file(path("in.vdb").string());
    file.setGridStatsMetadataEnabled(false);
    file.write({grid});
    file.close();
  }
  const auto full = openvdb::tools::memUsageIfLoaded(grid->tree(), false);
  p.maxInputBytes = std::filesystem::file_size(path("in.vdb")) +
                    2 * 1024 * 1024 + uintmax_t(full * 1.5) + 32768;
  try {
    (void)run();
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("before paging"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  p.maxInputBytes = 512ull * 1024 * 1024;
  openvdb::MetaMap fileMeta;
  fileMeta.insertMeta("large",
                      openvdb::StringMetadata(std::string(10000, 'x')));
  write<openvdb::DoubleGrid>("metadata.vdb", grid, fileMeta);
  o.maxMetadataBytes = 10000;
  EXPECT_THROW(run("metadata.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbFieldResampling,
       RemoteIndicesAndSmallAlignedTargetMapsRetainActualNativeCoefficients) {
  auto grid = openvdb::DoubleGrid::create(0);
  grid->tree().setValue({1000000000, 0, 0}, 7);
  auto map = openvdb::math::Transform::createLinearTransform(1);
  map->postTranslate({1e12, 0, 0});
  grid->setTransform(map);
  write<openvdb::DoubleGrid>("remote.vdb", grid);
  o.interpolation = vdb::FieldResamplingInterpolation::Nearest;
  o.targetIndexToWorld = std::array<double, 16>{1, 0, 0, 1e12 + 1, 0, 1, 0, 0,
                                                0, 0, 1, 0,        0, 0, 0, 1};
  (void)run("remote.vdb", "remote-out.vdb");
  auto out = read<openvdb::DoubleGrid>("remote-out.vdb");
  EXPECT_EQ(out->tree().getValue({999999999, 0, 0}), 7);
  EXPECT_EQ(out->transform().indexToWorld(V(0)), V(1e12 + 1, 0, 0));
  auto one = openvdb::DoubleGrid::create(0);
  one->tree().setValue({0, 0, 0}, 1);
  write<openvdb::DoubleGrid>("tiny.vdb", one);
  o.targetIndexToWorld = std::array<double, 16>{
      1e-6, 0, 0, 0, 0, 1e-6, 0, 0, 0, 0, 1e-6, 0, 0, 0, 0, 1};
  // A matching world compression keeps the bounded target extent small.
  o.worldFromWorld = {1e-6, 0, 0, 0, 0, 1e-6, 0, 0, 0, 0, 1e-6, 0, 0, 0, 0, 1};
  (void)run("tiny.vdb", "tiny-out.vdb");
  out = read<openvdb::DoubleGrid>("tiny-out.vdb");
  EXPECT_EQ(out->tree().getValue({0, 0, 0}), 1);
  EXPECT_EQ(out->transform().baseMap()->applyJacobian({1, 0, 0}),
            V(1e-6, 0, 0));
}
TEST_F(
    OpenVdbFieldResampling,
    FogQuadraticNegativeOvershootRejectsAndEmptyAbsoluteBackgroundTransforms) {
  auto fog = openvdb::DoubleGrid::create(0);
  fog->setGridClass(openvdb::GRID_FOG_VOLUME);
  fog->tree().setValue({1, 0, 0}, 1);
  write<openvdb::DoubleGrid>("fog.vdb", fog);
  o.interpolation = vdb::FieldResamplingInterpolation::Quadratic;
  o.targetIndexToWorld =
      std::array<double, 16>{1, 0, 0, .5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  EXPECT_THROW(run("fog.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  auto empty = openvdb::Vec3DGrid::create({0, 0, 0});
  empty->setVectorType(openvdb::VEC_CONTRAVARIANT_ABSOLUTE);
  write<openvdb::Vec3DGrid>("empty.vdb", empty);
  o = {};
  o.worldFromWorld[3] = 2;
  const auto result = run("empty.vdb", "empty-out.vdb");
  EXPECT_EQ(result.grid.activeVoxelCount, 0);
  EXPECT_FALSE(result.targetBounds);
  EXPECT_EQ(read<openvdb::Vec3DGrid>("empty-out.vdb")->background(),
            V(2, 0, 0));
}
TEST_F(OpenVdbFieldResampling,
       UnicodePathsMetadataAndExclusiveOutputsPreserveSourceBytes) {
  auto grid = openvdb::DoubleGrid::create(0);
  grid->tree().setValue({0, 0, 0}, 3);
  grid->insertMeta("artist", openvdb::StringMetadata("field-é"));
  openvdb::MetaMap fm;
  fm.insertMeta("scene", openvdb::Int64Metadata(INT64_C(9007199254740993)));
  write<openvdb::DoubleGrid>("in.vdb", grid, fm);
  const auto original = bytes(path("in.vdb"));
  const auto source =
                 directory / std::filesystem::path(std::u8string(u8"場-é.vdb")),
             output = directory /
                      std::filesystem::path(std::u8string(u8"再標本化.vdb"));
  std::filesystem::copy_file(path("in.vdb"), source);
  (void)vdb::WriteResampledField(source, output, o, {}, p);
  const auto completed = bytes(output);
  EXPECT_THROW((void)vdb::WriteResampledField(source, output, o, {}, p),
               std::invalid_argument);
  EXPECT_EQ(bytes(source), original);
  EXPECT_EQ(bytes(output), completed);
  std::filesystem::copy_file(output, path("read.vdb"));
  openvdb::io::File file(path("read.vdb").string());
  file.open(false);
  EXPECT_EQ(file.getMetadata()->metaValue<int64_t>("scene"),
            INT64_C(9007199254740993));
  EXPECT_EQ(file.readGrid("sampled")->metaValue<std::string>("artist"),
            "field-é");
  file.close();
}
TEST_F(OpenVdbFieldResampling, DisabledDelayedLoadingRejectsWithoutOutput) {
  auto grid = openvdb::DoubleGrid::create(0);
  write<openvdb::DoubleGrid>("in.vdb", grid);
  const char *prior = std::getenv("OPENVDB_DISABLE_DELAYED_LOAD");
  const std::string old = prior ? prior : "";
  const bool had = prior != nullptr;
  const auto set = [](const char *value) {
#ifdef _WIN32
    _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", value ? value : "");
#else
    if (value)
      setenv("OPENVDB_DISABLE_DELAYED_LOAD", value, 1);
    else
      unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
  };
  struct Restore {
    decltype(set) setter;
    std::string value;
    bool present;
    ~Restore() { setter(present ? value.c_str() : nullptr); }
  } restore{set, old, had};
  set("1");
  EXPECT_THROW(run(), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
#endif
} // namespace
