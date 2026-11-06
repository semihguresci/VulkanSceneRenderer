#include "Container/geometry/OpenVdbDiagnostics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/io.h>
#include <openvdb/points/AttributeArrayString.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/points/PointGroup.h>
#include <openvdb/tools/Diagnostics.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
using Status = vdb::DiagnosticStatus;

TEST(OpenVdbDiagnosticsAvailability, DisabledBeforeBothPathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::DiagnosticsOptions options;
  options.maxWork = 0;
  options.maskGridName.clear();
  vdb::ProcessingOptions processing;
  processing.maxInputBytes = 0;
  for (bool write : {false, true}) {
    try {
      if (write)
        (void)vdb::WriteVdbDiagnosticMask("missing", "missing/output", options,
                                          {}, processing);
      else
        (void)vdb::DiagnoseVdb("missing", options, {}, processing);
      FAIL();
    } catch (const std::runtime_error &error) {
      EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
                std::string::npos);
    }
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points = openvdb::points;
using C = openvdb::Coord;
using V = openvdb::Vec3d;
using PointGrid = points::PointDataGrid;

template <class G> class ExactDiagnosticsFixture : public G {
public:
  explicit ExactDiagnosticsFixture(const G &grid)
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
        static_cast<const ExactDiagnosticsFixture &>(*this).copyGrid());
  }
};

class OpenVdbDiagnostics : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    registerArray<double>();
    registerArray<int64_t>();
    registerArray<openvdb::Vec3f>();
    using Encoded = points::TypedAttributeArray<float, points::TruncateCodec>;
    if (!Encoded::isRegistered())
      Encoded::registerType();
    static std::atomic_uint64_t serial{};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-diagnostics-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    processing.gridName = "ignored_generation_name";
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  template <class T> static void registerArray() {
    using A = points::TypedAttributeArray<T>;
    if (!A::isRegistered())
      A::registerType();
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  static std::string bytes(const std::filesystem::path &name) {
    std::ifstream stream(name, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &metadata = {}) const {
    openvdb::GridPtrVec exact;
    for (const auto &grid : grids) {
      if (grid->isType<openvdb::DoubleGrid>())
        exact.push_back(
            std::make_shared<ExactDiagnosticsFixture<openvdb::DoubleGrid>>(
                *openvdb::gridPtrCast<openvdb::DoubleGrid>(grid)));
      else if (grid->isType<openvdb::FloatGrid>())
        exact.push_back(
            std::make_shared<ExactDiagnosticsFixture<openvdb::FloatGrid>>(
                *openvdb::gridPtrCast<openvdb::FloatGrid>(grid)));
      else
        exact.push_back(grid);
    }
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(exact, metadata);
    file.close();
  }
  vdb::DiagnosticReport run(const std::string &input = "input.vdb",
                            const std::string &selector = {}) const {
    return vdb::DiagnoseVdb(path(input), options, selector, processing,
                            pointLimits);
  }
  vdb::DiagnosticReport mask(const std::string &input = "input.vdb",
                             const std::string &output = "mask.vdb",
                             const std::string &selector = {}) const {
    return vdb::WriteVdbDiagnosticMask(path(input), path(output), options,
                                       selector, processing, pointLimits);
  }
  openvdb::BoolGrid::Ptr readMask(const std::string &name = "mask.vdb") const {
    openvdb::io::File file(path(name).string());
    file.open(false);
    auto grid = openvdb::gridPtrCast<openvdb::BoolGrid>(
        file.readGrid(options.maskGridName));
    file.close();
    return grid;
  }
  static const vdb::DiagnosticCheck &check(const vdb::DiagnosticReport &report,
                                           const std::string &id) {
    const auto found =
        std::find_if(report.checks.begin(), report.checks.end(),
                     [&](const auto &item) { return item.id == id; });
    if (found == report.checks.end())
      throw std::runtime_error("missing diagnostic check: " + id);
    return *found;
  }
  static size_t statuses(const vdb::DiagnosticReport &report, Status status) {
    return std::count_if(
        report.checks.begin(), report.checks.end(),
        [&](const auto &item) { return item.status == status; });
  }
  static void uniformMap(openvdb::GridBase &grid, double h) {
    if (h >= 1e-4) {
      grid.setTransform(openvdb::math::Transform::createLinearTransform(h));
      return;
    }
    std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
    openvdb::writeString(record, openvdb::math::UniformScaleMap::mapType());
    for (double x : {h, h, 1 / h, 1 / (h * h), .5 / h})
      V(x).write(record);
    record.seekg(0);
    openvdb::io::setCurrentVersion(record);
    grid.transform().read(record);
  }
  static openvdb::DoubleGrid::Ptr plane(int radius = 2, double slope = 1,
                                        double h = 1) {
    auto grid = openvdb::DoubleGrid::create(16 * h);
    grid->setName("field");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    uniformMap(*grid, h);
    for (int x = -radius; x <= radius; ++x)
      for (int y = -radius; y <= radius; ++y)
        for (int z = -radius; z <= radius; ++z)
          grid->tree().setValueOn({x, y, z}, (slope * x + .25) * h);
    return grid;
  }
  static PointGrid::Ptr pointFixture() {
    const std::vector<V> positions{
        {.125, 0, 0}, {.25, 0, 0}, {1.125, 0, 0}, {8.125, 0, 0}};
    points::PointAttributeVector<V> wrapper(positions);
    auto map = openvdb::math::Transform::createLinearTransform(1);
    auto indices =
        openvdb::tools::createPointIndexGrid<openvdb::tools::PointIndexGrid>(
            wrapper, *map);
    auto grid = points::createPointDataGrid<points::NullCodec, PointGrid>(
        *indices, wrapper, *map);
    grid->setName("points");
    points::appendAttribute<double>(grid->tree(), "value", 2.);
    points::appendAttribute<int64_t>(grid->tree(), "id",
                                     INT64_C(9007199254740993));
    points::appendAttribute<float, points::TruncateCodec>(grid->tree(),
                                                          "packed", .75f);
    points::appendAttribute<double>(grid->tree(), "unrelated_pair", 7., 2);
    points::appendGroup(grid->tree(), "A");
    points::appendGroup(grid->tree(), "empty");
    for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) {
      auto group = leaf->groupWriteHandle("A");
      for (auto point = leaf->beginIndexAll(); point; ++point)
        group.set(*point, point.getCoord().x() == 0);
      leaf->attributeArray("value").setHidden(true);
    }
    grid->tree().setActiveState({0, 0, 0}, false);
    return grid;
  }

  std::filesystem::path directory;
  vdb::DiagnosticsOptions options;
  vdb::ProcessingOptions processing;
  vdb::PointAttributeOptions pointLimits;
};

TEST_F(OpenVdbDiagnostics, FiniteChecksSupportAllNineNativeFieldFamilies) {
  const auto finite = [this]<class G>(const typename G::ValueType &value) {
    auto grid = G::create(value);
    grid->setName("native");
    if constexpr (std::is_same_v<G, openvdb::MaskGrid>)
      grid->tree().setValueOn({-1, 2, 3}, true);
    else
      grid->tree().setValueOn({-1, 2, 3}, value);
    grid->tree().touchLeaf({32, 0, 0});
    grid->tree().setValueOff({32, 0, 0}, value);
    write("input.vdb", {grid});
    const auto report = run();
    EXPECT_TRUE(report.complete);
    EXPECT_TRUE(report.allRequestedPassed);
    EXPECT_EQ(check(report, "finite").status, Status::Pass);
    EXPECT_EQ(report.activeVoxelCount, 1u);
    EXPECT_GE(report.storedLeafSlots, 1024u);
  };
  finite.template operator()<openvdb::FloatGrid>(-0.f);
  finite.template operator()<openvdb::DoubleGrid>(-0.);
  finite.template operator()<openvdb::BoolGrid>(false);
  finite.template operator()<openvdb::MaskGrid>(false);
  finite.template operator()<openvdb::Int32Grid>(INT32_MIN);
  finite.template operator()<openvdb::Int64Grid>(INT64_MAX);
  finite.template operator()<openvdb::Vec3IGrid>({INT32_MIN, 0, INT32_MAX});
  finite.template operator()<openvdb::Vec3SGrid>({-0.f, 1e20f, -1e-20f});
  finite.template operator()<openvdb::Vec3DGrid>({-0., 1e200, -1e-200});
}

TEST_F(OpenVdbDiagnostics,
       NaNInfStoredVoxelsTilesAndBackgroundHaveDistinctCounts) {
  auto grid = openvdb::DoubleGrid::create(0.);
  grid->tree().fill(openvdb::CoordBBox({0, 0, 0}, {7, 7, 7}), 0., false);
  grid->tree().setValueOn({0, 0, 0}, std::numeric_limits<double>::quiet_NaN());
  grid->tree().setValueOff({1, 0, 0}, std::numeric_limits<double>::infinity());
  grid->tree().addTile(1, {32, 0, 0}, -std::numeric_limits<double>::infinity(),
                       true);
  write("input.vdb", {grid});
  const auto before = bytes(path("input.vdb"));
  const auto report = mask();
  const auto &finite = check(report, "finite");
  EXPECT_EQ(finite.status, Status::Fail);
  EXPECT_EQ(finite.failedLeafVoxels, 2u);
  EXPECT_EQ(finite.failedTiles, 1u);
  EXPECT_EQ(finite.failedTileVoxelVolume, 512u);
  EXPECT_FALSE(finite.backgroundFailed);
  EXPECT_FALSE(report.unlocatedFailures);
  EXPECT_TRUE(report.complete);
  EXPECT_FALSE(report.allRequestedPassed);
  EXPECT_EQ(report.maskVoxelCount, 514u);
  auto result = readMask();
  EXPECT_TRUE(result->tree().isValueOn({0, 0, 0}));
  EXPECT_TRUE(result->tree().isValueOn({1, 0, 0}));
  EXPECT_TRUE(result->tree().isValueOn({39, 7, 7}));
  EXPECT_FALSE(result->tree().isValueOn({2, 0, 0}));
  EXPECT_FALSE(result->background());
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbDiagnostics,
       InclusiveInt64RangesNeverRoundNativeIntegersThroughDouble) {
  constexpr int64_t base = INT64_C(9007199254740992);
  auto grid = openvdb::Int64Grid::create(base);
  grid->tree().fill(openvdb::CoordBBox({0, 0, 0}, {7, 7, 7}), base, false);
  grid->tree().setValueOn({0, 0, 0}, base);
  grid->tree().setValueOn({1, 0, 0}, base + 1);
  grid->tree().setValueOff({2, 0, 0}, base - 1);
  write("input.vdb", {grid});
  options.componentRange = std::array<double, 2>{double(base), double(base)};
  const auto active = run();
  EXPECT_EQ(check(active, "range").failedLeafVoxels, 1u);
  options.rangeScope = vdb::DiagnosticScope::Stored;
  const auto stored = mask();
  EXPECT_EQ(check(stored, "range").failedLeafVoxels, 2u);
  EXPECT_EQ(stored.maskVoxelCount, 2u);
  auto boundary = openvdb::Int64Grid::create(0);
  boundary->tree().setValueOn({0, 0, 0}, INT64_MAX);
  write("boundary.vdb", {boundary});
  options.rangeScope = vdb::DiagnosticScope::Active;
  options.componentRange =
      std::array<double, 2>{std::ldexp(1., 63), std::ldexp(1., 63)};
  EXPECT_EQ(check(run("boundary.vdb"), "range").failedLeafVoxels, 1u);
}

TEST_F(OpenVdbDiagnostics,
       ComponentwiseVectorChecksUnionFailuresWithoutDoubleCountingMask) {
  auto grid = openvdb::Vec3DGrid::create(V(0));
  grid->tree().setValueOn({0, 0, 0}, {2, -3, 0});
  grid->tree().setValueOn({1, 0, 0},
                          {std::numeric_limits<double>::infinity(), 0, 0});
  grid->tree().setValueOn({2, 0, 0}, {.75, .75, .75});
  write("input.vdb", {grid});
  options.componentRange = std::array<double, 2>{-1, 1};
  const auto report = mask();
  EXPECT_EQ(check(report, "finite").failedLeafVoxels, 1u);
  EXPECT_EQ(check(report, "range").failedLeafVoxels, 2u);
  EXPECT_EQ(report.maskVoxelCount, 2u);
  EXPECT_FALSE(readMask()->tree().isValueOn({2, 0, 0}));
}

TEST_F(OpenVdbDiagnostics,
       AllocatedEmptyLeavesAndBackgroundOnlyFailuresAreNotConflated) {
  auto empty = openvdb::DoubleGrid::create(0);
  empty->tree().touchLeaf({1024, 0, 0});
  write("input.vdb", {empty});
  const auto report = run();
  EXPECT_EQ(report.leafCount, 1u);
  EXPECT_EQ(report.storedLeafSlots, 512u);
  EXPECT_EQ(report.activeVoxelCount, 0u);
  EXPECT_TRUE(report.complete);
  EXPECT_TRUE(report.allRequestedPassed);
  auto bad =
      openvdb::DoubleGrid::create(std::numeric_limits<double>::infinity());
  write("background.vdb", {bad});
  const auto background = mask("background.vdb");
  EXPECT_TRUE(check(background, "finite").backgroundFailed);
  EXPECT_EQ(background.maskVoxelCount, 0u);
  EXPECT_TRUE(background.unlocatedFailures);
  EXPECT_FALSE(background.allRequestedPassed);
  EXPECT_EQ(readMask()->activeVoxelCount(), 0u);
}

TEST_F(OpenVdbDiagnostics,
       AllIndependentLevelSetChecksRunAfterEarlierFailures) {
  auto grid = openvdb::DoubleGrid::create(1.);
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->tree().setValueOn({0, 0, 0}, std::numeric_limits<double>::quiet_NaN());
  grid->tree().setValueOn({1, 0, 0}, 2.);
  grid->tree().setValueOff({2, 0, 0}, .25);
  grid->tree().addTile(1, {32, 0, 0}, .5, true);
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  const auto report = run();
  EXPECT_EQ(check(report, "finite").status, Status::Fail);
  EXPECT_GE(statuses(report, Status::Fail), 4u);
  EXPECT_FALSE(report.allRequestedPassed);
  EXPECT_TRUE(report.unlocatedFailures);
  EXPECT_GE(report.checks.size(), 6u);
}

TEST_F(OpenVdbDiagnostics,
       NormalizedFogSeparatesComponentRangeFromZeroTolerance) {
  auto grid = openvdb::DoubleGrid::create(0.);
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->tree().setValueOn({0, 0, 0}, 1.125);
  grid->tree().setValueOn({1, 0, 0}, -.125);
  grid->tree().setValueOff({2, 0, 0}, .125);
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::NormalizedFog;
  const auto bad = mask();
  EXPECT_FALSE(bad.allRequestedPassed);
  EXPECT_GE(bad.maskVoxelCount, 3u);
  grid->tree().setValueOn({0, 0, 0}, 1.);
  grid->tree().setValueOn({1, 0, 0}, 0.);
  write("bounded.vdb", {grid});
  EXPECT_FALSE(run("bounded.vdb").allRequestedPassed);
  options.magnitudeTolerance = .125;
  const auto tolerated = run("bounded.vdb");
  EXPECT_TRUE(tolerated.complete);
  EXPECT_TRUE(tolerated.allRequestedPassed);
}

TEST_F(OpenVdbDiagnostics,
       FirstOrderGradientMatchesNativeGodunovAndFullActiveSevenPointFootprint) {
  auto grid = plane(2, 2.);
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  options.gradient = vdb::DiagnosticGradient::FirstOrder;
  options.gradientNormRange = {.5, 1.5};
  const auto report = mask();
  const auto &gradient = check(report, "gradient");
  EXPECT_EQ(gradient.candidateCount, 125u);
  EXPECT_EQ(gradient.evaluatedCount, 27u);
  EXPECT_EQ(gradient.missingFootprintCount, 98u);
  EXPECT_EQ(gradient.failedLeafVoxels, 27u);
  EXPECT_EQ(gradient.status, Status::Fail);
  EXPECT_FALSE(gradient.coverageComplete);
  EXPECT_FALSE(report.complete);
  EXPECT_EQ(report.maskVoxelCount, 27u);
  openvdb::tools::CheckNormGrad<openvdb::DoubleGrid> native(*grid, .5, 1.5);
  auto result = readMask();
  for (auto leaf = grid->tree().cbeginLeaf(); leaf; ++leaf)
    for (auto iter = leaf->cbeginValueOn(); iter; ++iter) {
      const C c = iter.getCoord();
      const bool interior =
          std::abs(c.x()) < 2 && std::abs(c.y()) < 2 && std::abs(c.z()) < 2;
      EXPECT_EQ(result->tree().isValueOn(c), interior && native(iter));
    }
}

TEST_F(OpenVdbDiagnostics,
       WenoBoundsArePublicNormsAndFootprintIsNineteenAxialSamples) {
  auto grid = plane(4, 1.25);
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  options.gradient = vdb::DiagnosticGradient::WenoInterface;
  options.gradientNormRange = {1.2, 1.3};
  auto report = run();
  const auto &gradient = check(report, "gradient");
  EXPECT_EQ(gradient.candidateCount, 729u);
  EXPECT_EQ(gradient.evaluatedCount, 18u);
  EXPECT_EQ(gradient.failedLeafVoxels, 0u);
  EXPECT_GT(gradient.missingFootprintCount, 0u);
  openvdb::tools::CheckEikonal<openvdb::DoubleGrid> native(*grid, 1.2 * 1.2,
                                                           1.3 * 1.3);
  for (auto leaf = grid->tree().cbeginLeaf(); leaf; ++leaf)
    for (auto iter = leaf->cbeginValueOn(); iter; ++iter)
      if (iter.getCoord() == C(0))
        EXPECT_FALSE(native(iter));
  options.gradientNormRange = {.5, 1.};
  grid->tree().setValueOff({1, 1, 1}, grid->background());
  write("corner.vdb", {grid});
  auto corner = mask("corner.vdb", "corner-mask.vdb");
  EXPECT_TRUE(readMask("corner-mask.vdb")->tree().isValueOn(C(0)));
  EXPECT_GT(check(corner, "gradient").failedLeafVoxels, 0u);
  grid->tree().setValueOff({3, 0, 0}, grid->background());
  write("axial.vdb", {grid});
  (void)mask("axial.vdb", "axial-mask.vdb");
  EXPECT_FALSE(readMask("axial-mask.vdb")->tree().isValueOn(C(0)));
}

TEST_F(OpenVdbDiagnostics,
       ExplicitFiniteStoredContinuationDoesNotPretendToBeActiveCoverage) {
  auto grid = openvdb::DoubleGrid::create(8.);
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
      for (int z = -1; z <= 1; ++z)
        grid->tree().setValueOff({x, y, z}, x + .25);
  grid->tree().setActiveState(C(0), true);
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  options.gradient = vdb::DiagnosticGradient::FirstOrder;
  options.gradientNormRange = {.99, 1.01};
  const auto active = run();
  EXPECT_EQ(check(active, "gradient").status, Status::Unavailable);
  EXPECT_EQ(check(active, "gradient").evaluatedCount, 0u);
  EXPECT_EQ(check(active, "gradient").missingFootprintCount, 1u);
  EXPECT_FALSE(active.complete);
  options.footprint = vdb::DiagnosticFootprint::NativeStoredContinuation;
  const auto stored = run();
  EXPECT_EQ(check(stored, "gradient").status, Status::Pass);
  EXPECT_EQ(check(stored, "gradient").evaluatedCount, 1u);
  EXPECT_EQ(check(stored, "gradient").inactiveContinuationCount, 1u);
  EXPECT_TRUE(stored.complete);
  EXPECT_FALSE(stored.allRequestedPassed);
}

TEST_F(OpenVdbDiagnostics,
       NonfiniteFootprintsAndFiniteArithmeticOverflowAreUnavailable) {
  auto grid = plane(2);
  grid->tree().setValueOn({1, 0, 0}, std::numeric_limits<double>::infinity());
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  options.gradient = vdb::DiagnosticGradient::FirstOrder;
  const auto report = mask();
  EXPECT_EQ(check(report, "finite").failedLeafVoxels, 1u);
  EXPECT_GT(check(report, "gradient").nonfiniteFootprintCount, 0u);
  EXPECT_FALSE(report.complete);
  EXPECT_FALSE(readMask()->tree().isValueOn(C(0)));
  auto extreme = openvdb::DoubleGrid::create(0);
  extreme->tree().setValueOn(C(0), 1.);
  for (const C c : {C(-1, 0, 0), C(1, 0, 0), C(0, -1, 0), C(0, 1, 0),
                    C(0, 0, -1), C(0, 0, 1)})
    extreme->tree().setValueOn(c, c.x() < 0
                                      ? -std::numeric_limits<double>::max()
                                      : std::numeric_limits<double>::max());
  write("overflow.vdb", {extreme});
  const auto overflow = run("overflow.vdb");
  EXPECT_EQ(check(overflow, "finite").status, Status::Pass);
  EXPECT_EQ(check(overflow, "gradient").status, Status::Unavailable);
  EXPECT_FALSE(overflow.complete);
}

TEST_F(OpenVdbDiagnostics,
       GradientConditioningRetainsWorldUnitsAndExactRotatedReflectedMaps) {
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  options.gradient = vdb::DiagnosticGradient::FirstOrder;
  options.gradientNormRange = {.999999, 1.000001};
  for (double h : {1e-6, .25, 2., 1e6}) {
    auto grid = plane(2, 1, h);
    if (h == .25 || h == 2.) {
      openvdb::math::Mat4d matrix(0., h, 0., 0., -h, 0., 0., 0., 0., 0., -h, 0.,
                                  100., -2., 3., 1.);
      grid->setTransform(
          openvdb::math::Transform::createLinearTransform(matrix));
    }
    const auto input = "scale-" + std::to_string(h) + ".vdb";
    const auto output = "scale-mask-" + std::to_string(h) + ".vdb";
    write(input, {grid});
    const auto report = mask(input, output);
    EXPECT_EQ(check(report, "gradient").failedLeafVoxels, 0u);
    EXPECT_EQ(check(report, "gradient").evaluatedCount, 27u);
    auto result = readMask(output);
    for (V position : {V(0), V(1, 2, 3)})
      EXPECT_EQ(result->indexToWorld(position), grid->indexToWorld(position));
  }
}

TEST_F(OpenVdbDiagnostics,
       UnsupportedDerivativeMapsDoNotBlockIndependentStoredChecks) {
  auto grid = plane();
  grid->transform().preScale(V(1, 2, 1));
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  options.gradient = vdb::DiagnosticGradient::FirstOrder;
  auto report = run();
  EXPECT_EQ(check(report, "finite").status, Status::Pass);
  EXPECT_EQ(check(report, "gradient").status, Status::Unavailable);
  EXPECT_FALSE(report.complete);
  auto nonlinear = plane();
  nonlinear->setTransform(openvdb::math::Transform::createFrustumTransform(
      openvdb::BBoxd(V(-1), V(1)), .5, 10., 1.));
  write("nonlinear.vdb", {nonlinear});
  report = run("nonlinear.vdb");
  EXPECT_FALSE(report.affine);
  EXPECT_FALSE(report.indexToWorld);
  EXPECT_EQ(check(report, "finite").status, Status::Pass);
  EXPECT_EQ(check(report, "gradient").status, Status::Unavailable);
}

TEST_F(OpenVdbDiagnostics,
       SelectedPointRecordsIncludeInactiveVoxelsAndRegisteredNumericCodecs) {
  auto grid = pointFixture();
  auto leaf = grid->tree().probeLeaf(C(0));
  points::AttributeWriteHandle<double> values(leaf->attributeArray("value"));
  values.set(0, std::numeric_limits<double>::quiet_NaN());
  values.set(1, std::numeric_limits<double>::infinity());
  write("input.vdb", {grid});
  const auto before = bytes(path("input.vdb"));
  options.profile = vdb::DiagnosticProfile::PointData;
  options.pointGroups.anyOf = {"A"};
  options.pointAttributes = {"value", "packed"};
  const auto report = mask();
  ASSERT_TRUE(report.points);
  EXPECT_TRUE(report.points->countsValid);
  EXPECT_EQ(report.points->total, 4u);
  EXPECT_EQ(report.points->selected, 2u);
  EXPECT_EQ(report.points->selectedActive, 0u);
  EXPECT_EQ(report.points->selectedInactive, 2u);
  EXPECT_EQ(check(report, "point-layout").status, Status::Pass);
  EXPECT_EQ(check(report, "point-position").status, Status::Pass);
  EXPECT_EQ(check(report, "point-attribute:value").failedPoints, 2u);
  EXPECT_EQ(check(report, "point-attribute:packed").status, Status::Pass);
  EXPECT_EQ(report.maskVoxelCount, 1u);
  EXPECT_TRUE(readMask()->tree().isValueOn(C(0)));
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbDiagnostics,
       PointExactIntegerRangesAndMissingAttributesAreIndependent) {
  auto grid = pointFixture();
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::PointData;
  options.pointAttributes = {"id", "missing"};
  options.componentRange =
      std::array<double, 2>{9007199254740992., 9007199254740992.};
  const auto report = run();
  EXPECT_EQ(check(report, "point-attribute:id").failedPoints, 4u);
  EXPECT_EQ(check(report, "point-attribute:missing").status,
            Status::Unavailable);
  EXPECT_FALSE(report.complete);
  EXPECT_FALSE(report.allRequestedPassed);
}

TEST_F(OpenVdbDiagnostics,
       PointPositionFailuresMarkOriginalStoredVoxelWithoutMovingRecords) {
  auto grid = pointFixture();
  auto leaf = grid->tree().probeLeaf(C(0));
  points::AttributeWriteHandle<openvdb::Vec3f> positions(
      leaf->attributeArray("P"));
  positions.set(0, {1.f, 0, 0});
  positions.set(1, {std::numeric_limits<float>::quiet_NaN(), 0, 0});
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::PointData;
  const auto report = mask();
  EXPECT_EQ(check(report, "point-layout").status, Status::Pass);
  EXPECT_EQ(check(report, "point-position").failedPoints, 2u);
  EXPECT_EQ(report.maskVoxelCount, 1u);
  EXPECT_TRUE(readMask()->tree().isValueOn(C(0)));
  EXPECT_FALSE(readMask()->tree().isValueOn({1, 0, 0}));
}

TEST_F(OpenVdbDiagnostics,
       UnsafePointOffsetsLengthsAndGroupStridesDisableEveryDecoder) {
  options.profile = vdb::DiagnosticProfile::PointData;
  options.pointAttributes = {"value"};
  for (int defect = 0; defect < 3; ++defect) {
    auto grid = pointFixture();
    auto leaf = grid->tree().probeLeaf(C(0));
    if (defect == 0)
      leaf->setOffsetOnly(0, openvdb::Index32(leaf->pointCount() + 1));
    else {
      auto arrays =
          std::make_unique<points::AttributeSet>(leaf->attributeSet());
      if (defect == 1) {
        const auto index = leaf->attributeSet().find("value");
        arrays->replace(index,
                        std::make_shared<points::TypedAttributeArray<double>>(
                            openvdb::Index(leaf->pointCount() + 1)));
      } else {
        const auto index = leaf->attributeSet().groupIndex("A").first;
        arrays->replace(index,
                        std::make_shared<points::GroupAttributeArray>(
                            openvdb::Index(leaf->pointCount()), 2, true));
      }
      leaf->replaceAttributeSet(arrays.release());
    }
    const auto input = "bad-layout-" + std::to_string(defect) + ".vdb";
    write(input, {grid});
    const auto report = run(input);
    EXPECT_EQ(check(report, "point-layout").status, Status::Fail);
    EXPECT_EQ(check(report, "point-position").status, Status::Unavailable);
    EXPECT_EQ(check(report, "point-attribute:value").status,
              Status::Unavailable);
    EXPECT_FALSE(report.complete);
    if (report.points)
      EXPECT_FALSE(report.points->countsValid);
  }
}

TEST_F(OpenVdbDiagnostics,
       MissingPointGroupsDoNotDecodeAndValidEmptyArchivesRemainRecognizable) {
  auto grid = pointFixture();
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::PointData;
  options.pointGroups.anyOf = {"absent"};
  options.pointAttributes = {"value"};
  const auto missing = run();
  EXPECT_EQ(check(missing, "point-groups").status, Status::Fail);
  EXPECT_EQ(check(missing, "point-position").status, Status::Unavailable);
  EXPECT_EQ(check(missing, "point-attribute:value").status,
            Status::Unavailable);
  vdb::ProcessingOptions authoring;
  authoring.gridName = "archived";
  const auto archive = vdb::WriteSelectedPoints(
      path("input.vdb"), path("archive.vdb"), {{"empty"}, {}, {}},
      vdb::PointSelectionMode::KeepSelected, {}, authoring);
  ASSERT_TRUE(archive.archivedSchema);
  options.pointGroups = {};
  const auto empty = run("archive.vdb");
  ASSERT_TRUE(empty.points);
  EXPECT_TRUE(empty.points->archivedSchema);
  EXPECT_TRUE(empty.points->countsValid);
  EXPECT_EQ(empty.points->total, 0u);
  EXPECT_EQ(check(empty, "point-layout").status, Status::Pass);
  auto ordinary = PointGrid::create();
  ordinary->setName("empty");
  write("ordinary-empty.vdb", {ordinary});
  options.pointAttributes.clear();
  const auto ordinaryReport = run("ordinary-empty.vdb");
  EXPECT_TRUE(ordinaryReport.allRequestedPassed);
}

TEST_F(OpenVdbDiagnostics,
       CompactFailureTilesHaveSemanticMaskCapsWithoutDenseExpansion) {
  auto grid = openvdb::DoubleGrid::create(0);
  grid->tree().addTile(2, C(0), std::numeric_limits<double>::infinity(), true);
  write("input.vdb", {grid});
  processing.maxVoxelCount = 1;
  const auto report = run();
  EXPECT_EQ(report.leafCount, 0u);
  EXPECT_EQ(check(report, "finite").failedTiles, 1u);
  EXPECT_EQ(check(report, "finite").failedTileVoxelVolume, 128u * 128u * 128u);
  EXPECT_THROW((void)mask(), std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("mask.vdb")));
  processing.maxVoxelCount = 128u * 128u * 128u;
  const auto written = mask();
  EXPECT_EQ(written.maskVoxelCount, processing.maxVoxelCount);
  const auto output = readMask();
  EXPECT_EQ(output->tree().leafCount(), 0u);
  EXPECT_TRUE(output->tree().isValueOn({127, 127, 127}));
  EXPECT_FALSE(output->tree().isValueOn({128, 0, 0}));
}

TEST_F(OpenVdbDiagnostics,
       SelectorsUnicodeMetadataAndExclusiveOutputPreserveNativeInput) {
  auto a = openvdb::DoubleGrid::create(0);
  a->setName("descriptor:field");
  a->tree().setValueOn({-1, 2, 3}, std::numeric_limits<double>::infinity());
  a->insertMeta("artist",
                openvdb::StringMetadata(std::string("raw\0metadata", 12)));
  a->transform().postTranslate(V(10, 20, 30));
  auto b = openvdb::DoubleGrid::create(0);
  b->setName("other");
  openvdb::MetaMap fileMetadata;
  fileMetadata.insertMeta("scene",
                          openvdb::Int64Metadata(INT64_C(9007199254740993)));
  write("input.vdb", {a, b}, fileMetadata);
  const auto sourceBytes = bytes(path("input.vdb"));
  options.maskGridName = "descriptor:failures";
  auto report = mask("input.vdb", "mask.vdb", "descriptor:descriptor:field");
  EXPECT_EQ(report.gridName, "descriptor:field");
  EXPECT_EQ(report.maskVoxelCount, 1u);
  EXPECT_TRUE(report.mask);
  auto output = readMask();
  EXPECT_EQ(output->getGridClass(), openvdb::GRID_UNKNOWN);
  EXPECT_TRUE(output->isInWorldSpace());
  EXPECT_EQ(output->metaValue<std::string>("artist"),
            std::string("raw\0metadata", 12));
  EXPECT_EQ(output->indexToWorld(V(-1, 2, 3)), a->indexToWorld(V(-1, 2, 3)));
  openvdb::io::File file(path("mask.vdb").string());
  file.open(false);
  EXPECT_EQ(file.getMetadata()->metaValue<int64_t>("scene"),
            INT64_C(9007199254740993));
  file.close();
  const auto completed = bytes(path("mask.vdb"));
  EXPECT_THROW(
      (void)mask("input.vdb", "mask.vdb", "descriptor:descriptor:field"),
      std::exception);
  EXPECT_EQ(bytes(path("mask.vdb")), completed);
  EXPECT_THROW(
      (void)mask("input.vdb", "input.vdb", "descriptor:descriptor:field"),
      std::exception);
  EXPECT_EQ(bytes(path("input.vdb")), sourceBytes);
  const auto unicodeInput =
      directory / std::filesystem::path(std::u8string(u8"入力-é.vdb"));
  const auto unicodeOutput =
      directory / std::filesystem::path(std::u8string(u8"検査-é.vdb"));
  std::filesystem::copy_file(path("input.vdb"), unicodeInput);
  report =
      vdb::WriteVdbDiagnosticMask(unicodeInput, unicodeOutput, options,
                                  "descriptor:descriptor:field", processing);
  EXPECT_EQ(report.maskVoxelCount, 1u);
  EXPECT_EQ(bytes(unicodeInput), sourceBytes);
  EXPECT_TRUE(std::filesystem::is_regular_file(unicodeOutput));
}

TEST_F(OpenVdbDiagnostics,
       ReportExamplesAndAllSharedCapsAreEnforcedBeforePublication) {
  auto grid = openvdb::DoubleGrid::create(0);
  grid->setName("bad");
  for (int x = 0; x < 20; ++x)
    grid->tree().setValueOn({x, 0, 0},
                            std::numeric_limits<double>::quiet_NaN());
  auto other = openvdb::DoubleGrid::create(0);
  other->setName("other");
  write("input.vdb", {grid, other});
  const auto before = bytes(path("input.vdb"));
  options.maxExamples = 2;
  auto report = run("input.vdb", "bad");
  EXPECT_EQ(check(report, "finite").failedLeafVoxels, 20u);
  EXPECT_EQ(check(report, "finite").examples.size(), 2u);
  options.maxExamples = 0;
  EXPECT_TRUE(check(run("input.vdb", "bad"), "finite").examples.empty());
  const auto baseOptions = options;
  const auto baseProcessing = processing;
  for (int cap = 0; cap < 7; ++cap) {
    options = baseOptions;
    processing = baseProcessing;
    if (cap == 0)
      options.maxWork = 1;
    if (cap == 1)
      options.maxMetadataBytes = 1;
    if (cap == 2)
      options.maxGrids = 1;
    if (cap == 3)
      processing.maxInputBytes = 1;
    if (cap == 4)
      processing.maxVoxelCount = 511;
    if (cap == 5)
      options.maxExamples = 129;
    if (cap == 6)
      options.componentRange = std::array<double, 2>{1, -1};
    const auto output = "cap-" + std::to_string(cap) + ".vdb";
    EXPECT_THROW((void)mask("input.vdb", output, "bad"), std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(output)));
    EXPECT_EQ(bytes(path("input.vdb")), before);
  }
  options = baseOptions;
  processing = baseProcessing;
  options.maskGridName.clear();
  EXPECT_NO_THROW((void)run("input.vdb", "bad"));
  EXPECT_THROW((void)mask("input.vdb", "empty-name.vdb", "bad"),
               std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("empty-name.vdb")));
  for (const auto &entry : std::filesystem::directory_iterator(directory))
    EXPECT_FALSE(entry.path().filename().string().starts_with(".container-"));
}

TEST_F(OpenVdbDiagnostics,
       InvalidSdfPrerequisitesNeverInventFiniteLocationFailures) {
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  for (const double background :
       {-3., std::numeric_limits<double>::quiet_NaN()}) {
    auto grid = openvdb::DoubleGrid::create(background);
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    grid->tree().setValueOn(C(0), 0.);
    const auto name = std::isfinite(background) ? "negative.vdb" : "nan.vdb";
    write(name, {grid});
    const auto report = run(name);
    EXPECT_EQ(check(report, "sdf-background").status, Status::Fail);
    for (const auto *id : {"sdf-active-range", "sdf-inactive-magnitude"}) {
      EXPECT_EQ(check(report, id).status, Status::Unavailable);
      EXPECT_EQ(check(report, id).failedLeafVoxels, 0u);
      EXPECT_EQ(check(report, id).failedTiles, 0u);
    }
    EXPECT_FALSE(report.complete);
    if (std::isfinite(background)) {
      EXPECT_EQ(check(report, "finite").status, Status::Pass);
      EXPECT_EQ(report.maskVoxelCount, 0u);
      (void)mask(name);
      EXPECT_FALSE(readMask()->tree().isValueOn(C(0)));
    } else {
      EXPECT_TRUE(check(report, "finite").backgroundFailed);
    }
  }
  auto grid = plane();
  grid->transform().preScale(V(1, 2, 1));
  write("bad-map.vdb", {grid});
  const auto report = run("bad-map.vdb");
  EXPECT_EQ(check(report, "sdf-map").status, Status::Fail);
  EXPECT_EQ(check(report, "sdf-background").status, Status::Unavailable);
  EXPECT_EQ(check(report, "sdf-active-range").status, Status::Pass);
  EXPECT_EQ(check(report, "sdf-inactive-magnitude").status, Status::Pass);
  EXPECT_EQ(report.maskVoxelCount, 0u);
}

TEST_F(OpenVdbDiagnostics,
       WenoCrossingClassificationRequiresItsActiveFaceFootprint) {
  auto grid = openvdb::DoubleGrid::create(3.);
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  grid->tree().setValueOn(C(0), .25);
  write("input.vdb", {grid});
  options.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
  options.gradient = vdb::DiagnosticGradient::WenoInterface;
  const auto report = run();
  const auto &gradient = check(report, "gradient");
  EXPECT_EQ(gradient.candidateCount, 1u);
  EXPECT_EQ(gradient.evaluatedCount, 0u);
  EXPECT_EQ(gradient.missingFootprintCount, 1u);
  EXPECT_EQ(gradient.status, Status::Unavailable);
  EXPECT_FALSE(gradient.coverageComplete);
  EXPECT_FALSE(report.complete);
  EXPECT_FALSE(report.allRequestedPassed);
  EXPECT_EQ(report.maskVoxelCount, 0u);
}

TEST_F(OpenVdbDiagnostics,
       OutputMetadataSharesTheSourceCatalogCapBeforeMaskPublication) {
  auto grid = openvdb::DoubleGrid::create(0.);
  grid->setName("field");
  grid->tree().setValueOn(C(0), std::numeric_limits<double>::infinity());
  grid->insertMeta("large_custom_note",
                   openvdb::StringMetadata(std::string(150000, 'x')));
  write("input.vdb", {grid});
  const auto before = bytes(path("input.vdb"));
  options.maxMetadataBytes = 1024 * 1024;
  const auto report = run();
  EXPECT_EQ(check(report, "finite").failedLeafVoxels, 1u);
  EXPECT_TRUE(report.complete);
  try {
    (void)mask();
    FAIL() << "source plus copied output metadata must share the metadata cap";
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find("metadata"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("mask.vdb")));
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbDiagnostics,
       NativeHiddenStridedStringsValidateEveryIdAndDictionaryEntry) {
  if (!points::StringAttributeArray::isRegistered())
    points::StringAttributeArray::registerType();
  const auto strings = [&] {
    auto grid = pointFixture();
    points::appendAttribute<openvdb::Index, points::StringCodec<false>>(
        grid->tree(), "labels", openvdb::Index(0), 2);
    auto descriptor = points::makeDescriptorUnique(grid->tree());
    points::StringMetaInserter dictionary(descriptor->getMetadata());
    dictionary.insert(std::string("alpha\0raw", 9));
    dictionary.insert("beta-world");
    for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) {
      auto &array = leaf->attributeArray("labels");
      array.setHidden(true);
      points::StringAttributeWriteHandle handle(
          array, leaf->attributeSet().descriptor().getMetadata());
      for (openvdb::Index n = 0; n < leaf->pointCount(); ++n) {
        handle.set(n, 0, std::string("alpha\0raw", 9));
        handle.set(n, 1, "beta-world");
      }
    }
    return grid;
  };
  options.profile = vdb::DiagnosticProfile::PointData;
  options.pointAttributes = {"value"};
  auto valid = strings();
  write("strings.vdb", {valid});
  const auto original = bytes(path("strings.vdb"));
  const auto good = run("strings.vdb");
  EXPECT_EQ(check(good, "point-layout").status, Status::Pass);
  EXPECT_TRUE(good.complete);
  EXPECT_TRUE(good.allRequestedPassed);
  for (int defect = 0; defect < 2; ++defect) {
    auto grid = strings();
    if (defect == 0) {
      auto leaf = grid->tree().probeLeaf(C(0));
      points::AttributeWriteHandle<openvdb::Index, points::StringCodec<false>>
          ids(leaf->attributeArray("labels"));
      // A broken SECOND stride element catches validators that only inspect
      // one string index per point. No StringAttributeHandle decodes it.
      ids.set(0, 1, std::numeric_limits<openvdb::Index>::max());
    } else {
      auto descriptor = points::makeDescriptorUnique(grid->tree());
      auto &metadata = descriptor->getMetadata();
      std::string key;
      for (auto entry = metadata.beginMeta(); entry != metadata.endMeta();
           ++entry)
        if (entry->first.starts_with("string:")) {
          key = entry->first;
          break;
        }
      ASSERT_FALSE(key.empty());
      metadata.removeMeta(key);
      metadata.insertMeta(key, openvdb::Int32Metadata(7));
    }
    const auto input = "string-layout-" + std::to_string(defect) + ".vdb";
    write(input, {grid});
    const auto before = bytes(path(input));
    const auto report = run(input);
    EXPECT_EQ(check(report, "point-layout").status, Status::Fail);
    EXPECT_EQ(check(report, "point-position").status, Status::Unavailable);
    EXPECT_EQ(check(report, "point-attribute:value").status,
              Status::Unavailable);
    EXPECT_FALSE(report.complete);
    EXPECT_EQ(bytes(path(input)), before);
  }
  pointLimits.maxStringBytes = 1;
  EXPECT_THROW((void)mask("strings.vdb", "string-cap.vdb"), std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("string-cap.vdb")));
  EXPECT_EQ(bytes(path("strings.vdb")), original);
}

TEST_F(OpenVdbDiagnostics,
       WrongNativeProfileDisablesDependentChecksWithoutInventingLocations) {
  options.profile = vdb::DiagnosticProfile::PointData;
  options.pointAttributes = {"id"};
  options.pointGroups.anyOf = {"A"};
  options.componentRange = std::array<double, 2>{0, 1};

  const auto unlocatedOnly = [](const vdb::DiagnosticReport &report) {
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.allRequestedPassed);
    EXPECT_TRUE(report.unlocatedFailures);
    EXPECT_FALSE(report.points.has_value());
    EXPECT_EQ(report.maskVoxelCount, 0u);
    for (const auto &item : report.checks) {
      EXPECT_EQ(item.failedLeafVoxels, 0u) << item.id;
      EXPECT_EQ(item.failedTiles, 0u) << item.id;
      EXPECT_EQ(item.failedPoints, 0u) << item.id;
      EXPECT_FALSE(item.backgroundFailed) << item.id;
    }
  };
  const auto scalarReport = [&](const vdb::DiagnosticReport &report) {
    unlocatedOnly(report);
    EXPECT_EQ(check(report, "point-layout").status, Status::Fail);
    for (const char *id :
         {"point-position", "point-groups", "point-attribute:id"}) {
      const auto &dependent = check(report, id);
      EXPECT_EQ(dependent.status, Status::Unavailable) << id;
      EXPECT_FALSE(dependent.coverageComplete) << id;
      EXPECT_EQ(dependent.examinedPoints, 0u) << id;
    }
    // The requested point-column interval must not be applied to scalar data.
    for (const auto &item : report.checks)
      if (item.id == "finite" || item.id == "range")
        EXPECT_NE(item.status, Status::Fail) << item.id;
  };
  const auto emptyMask = [&](const std::string &output,
                             const vdb::DiagnosticReport &report) {
    ASSERT_TRUE(report.mask.has_value());
    EXPECT_EQ(report.mask->activeVoxelCount, 0u);
    const auto grid = readMask(output);
    ASSERT_TRUE(grid);
    EXPECT_FALSE(grid->background());
    EXPECT_EQ(grid->activeVoxelCount(), 0u);
  };
  const auto exerciseScalar = [&]<class Grid>() {
    auto grid = Grid::create(typename Grid::ValueType(0));
    grid->setName("ordinary");
    grid->tree().setValueOn(C(0), typename Grid::ValueType(2));
    grid->tree().setValueOff(C(8, 0, 0), typename Grid::ValueType(.5));
    const std::string stem = grid->valueType();
    const std::string input = stem + "-not-points.vdb";
    const std::string output = stem + "-wrong-profile-mask.vdb";
    write(input, {grid});
    const auto original = bytes(path(input));

    const auto report = run(input, "ordinary");
    scalarReport(report);
    EXPECT_FALSE(report.mask.has_value());
    EXPECT_FALSE(std::filesystem::exists(path(output)));
    EXPECT_EQ(bytes(path(input)), original);

    const auto written = mask(input, output, "ordinary");
    scalarReport(written);
    emptyMask(output, written);
    EXPECT_EQ(bytes(path(input)), original);
  };
  exerciseScalar.template operator()<openvdb::FloatGrid>();
  exerciseScalar.template operator()<openvdb::DoubleGrid>();

  auto points = pointFixture();
  write("points-not-field.vdb", {points});
  const auto original = bytes(path("points-not-field.vdb"));
  for (auto profile : {vdb::DiagnosticProfile::StoredValues,
                       vdb::DiagnosticProfile::SymmetricLevelSet,
                       vdb::DiagnosticProfile::NormalizedFog}) {
    options = {};
    options.profile = profile;
    if (profile == vdb::DiagnosticProfile::SymmetricLevelSet)
      options.gradient = vdb::DiagnosticGradient::FirstOrder;
    const auto verify = [&](const vdb::DiagnosticReport &report) {
      unlocatedOnly(report);
      EXPECT_EQ(check(report, "profile-type").status, Status::Fail);
      EXPECT_EQ(check(report, "finite").status, Status::Unavailable);
      EXPECT_FALSE(check(report, "finite").coverageComplete);
      if (profile == vdb::DiagnosticProfile::SymmetricLevelSet) {
        EXPECT_EQ(check(report, "gradient").status, Status::Unavailable);
        EXPECT_FALSE(check(report, "gradient").coverageComplete);
      }
    };
    const auto report = run("points-not-field.vdb", "points");
    verify(report);
    EXPECT_FALSE(report.mask.has_value());
    const auto output = "point-field-profile-" +
                        std::to_string(static_cast<int>(profile)) + ".vdb";
    const auto written = mask("points-not-field.vdb", output, "points");
    verify(written);
    emptyMask(output, written);
    EXPECT_EQ(bytes(path("points-not-field.vdb")), original);
  }
}

#endif
} // namespace
