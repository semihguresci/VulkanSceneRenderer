#include "Container/geometry/OpenVdbSurfaceFields.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/io.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
using Operation = vdb::SurfaceFieldOperation;

TEST(OpenVdbSurfaceFieldsAvailability, DisabledBeforeOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::SurfaceFieldsOptions options;
  options.maxWorkItems = 0;
  options.validityGridName.clear();
  vdb::ProcessingOptions processing;
  processing.maxInputBytes = 0;
  try {
    (void)vdb::WriteSurfaceFields("missing.vdb", {}, static_cast<Operation>(99),
                                  options, {}, processing);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
// Keep the independent fixture's persisted map exact even when the SDK's
// ordinary Archive metadata copy would simplify a tiny uniform map.
template <class G> class ExactSurfaceFixture : public G {
public:
  explicit ExactSurfaceFixture(const G &grid)
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
        static_cast<const ExactSurfaceFixture &>(*this).copyGrid());
  }
};

class OpenVdbSurfaceFields : public ::testing::Test {
protected:
  using V = openvdb::Vec3d;
  struct Fields {
    openvdb::GridBase::Ptr value;
    openvdb::BoolGrid::Ptr valid;
    openvdb::MetaMap::Ptr fileMetadata;
  };
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-surface-fields-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    processing.gridName = "derived";
    options.validityGridName = "valid";
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  static std::string bytes(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &metadata = {}) {
    openvdb::GridPtrVec exact;
    for (const auto &grid : grids) {
      if (grid->isType<openvdb::FloatGrid>())
        exact.push_back(
            std::make_shared<ExactSurfaceFixture<openvdb::FloatGrid>>(
                *openvdb::gridPtrCast<openvdb::FloatGrid>(grid)));
      else if (grid->isType<openvdb::DoubleGrid>())
        exact.push_back(
            std::make_shared<ExactSurfaceFixture<openvdb::DoubleGrid>>(
                *openvdb::gridPtrCast<openvdb::DoubleGrid>(grid)));
      else
        exact.push_back(grid);
    }
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(exact, metadata);
  }
  Fields read(const std::string &name) const {
    openvdb::io::File file(path(name).string());
    file.open(false);
    auto value = file.readGrid(processing.gridName);
    auto valid = file.readGrid(options.validityGridName);
    EXPECT_TRUE(valid->isType<openvdb::BoolGrid>());
    return {value, openvdb::gridPtrCast<openvdb::BoolGrid>(valid),
            file.getMetadata()};
  }
  vdb::SurfaceFieldsResult run(Operation operation,
                               const std::string &input = "input.vdb",
                               const std::string &output = "output.vdb",
                               const std::string &selector = {}) {
    return vdb::WriteSurfaceFields(path(input), path(output), operation,
                                   options, selector, processing);
  }
  void fails(Operation operation, const std::string &input = "input.vdb",
             const std::string &selector = {}) {
    const auto before = bytes(path(input));
    const auto output = "failed-" + std::to_string(++failureSerial) + ".vdb";
    EXPECT_THROW((void)run(operation, input, output, selector), std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(output)));
    EXPECT_EQ(bytes(path(input)), before);
  }
  static void uniformMap(openvdb::GridBase &grid, double h) {
    if (h >= 1e-4) {
      grid.setTransform(openvdb::math::Transform::createLinearTransform(h));
      return;
    }
    std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
    openvdb::writeString(record, openvdb::math::UniformScaleMap::mapType());
    for (const double value : {h, h, 1 / h, 1 / (h * h), .5 / h})
      V(value).write(record);
    record.seekg(0);
    openvdb::io::setCurrentVersion(record);
    grid.transform().read(record);
  }
  template <class G = openvdb::DoubleGrid>
  static typename G::Ptr plane(double h = 1, double slope = 1,
                               double offset = .25,
                               openvdb::Coord origin = {}) {
    using T = typename G::ValueType;
    auto grid = G::create(T(8 * h));
    grid->setName("plane");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    uniformMap(*grid, h);
    for (int x = -2; x <= 2; ++x)
      for (int y = -2; y <= 2; ++y)
        for (int z = -2; z <= 2; ++z)
          grid->tree().setValue(origin + openvdb::Coord(x, y, z),
                                T((slope * x + offset) * h));
    return grid;
  }
  template <class G = openvdb::DoubleGrid>
  static typename G::Ptr sphere(double h = 1, openvdb::Coord origin = {}) {
    using T = typename G::ValueType;
    auto grid = G::create(T(4 * h));
    grid->setName("sphere");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    uniformMap(*grid, h);
    for (int x = -11; x <= 11; ++x)
      for (int y = -11; y <= 11; ++y)
        for (int z = -11; z <= 11; ++z) {
          const double d = std::hypot(double(x), double(y), double(z)) - 6;
          const auto coordinate = origin + openvdb::Coord(x, y, z);
          if (std::abs(d) < 4)
            grid->tree().setValue(coordinate, T(d * h));
          else if (d < 0)
            grid->tree().setValueOff(coordinate, T(-4 * h));
        }
    return grid;
  }
  static double scalar(const Fields &fields, const openvdb::Coord &coordinate) {
    if (fields.value->isType<openvdb::FloatGrid>())
      return openvdb::gridPtrCast<openvdb::FloatGrid>(fields.value)
          ->tree()
          .getValue(coordinate);
    return openvdb::gridPtrCast<openvdb::DoubleGrid>(fields.value)
        ->tree()
        .getValue(coordinate);
  }
  static V vector(const Fields &fields, const openvdb::Coord &coordinate) {
    if (fields.value->isType<openvdb::Vec3SGrid>())
      return V(openvdb::gridPtrCast<openvdb::Vec3SGrid>(fields.value)
                   ->tree()
                   .getValue(coordinate));
    return openvdb::gridPtrCast<openvdb::Vec3DGrid>(fields.value)
        ->tree()
        .getValue(coordinate);
  }
  static bool valueActive(const Fields &fields,
                          const openvdb::Coord &coordinate) {
    if (fields.value->isType<openvdb::FloatGrid>())
      return openvdb::gridPtrCast<openvdb::FloatGrid>(fields.value)
          ->tree()
          .isValueOn(coordinate);
    if (fields.value->isType<openvdb::DoubleGrid>())
      return openvdb::gridPtrCast<openvdb::DoubleGrid>(fields.value)
          ->tree()
          .isValueOn(coordinate);
    if (fields.value->isType<openvdb::Vec3SGrid>())
      return openvdb::gridPtrCast<openvdb::Vec3SGrid>(fields.value)
          ->tree()
          .isValueOn(coordinate);
    return openvdb::gridPtrCast<openvdb::Vec3DGrid>(fields.value)
        ->tree()
        .isValueOn(coordinate);
  }
  static void validity(const Fields &fields, const openvdb::Coord &coordinate,
                       bool expected) {
    EXPECT_EQ(fields.valid->tree().isValueOn(coordinate), expected);
    EXPECT_EQ(fields.valid->tree().getValue(coordinate), expected);
    EXPECT_EQ(valueActive(fields, coordinate), expected);
    if (!expected) {
      if (fields.value->isType<openvdb::FloatGrid>() ||
          fields.value->isType<openvdb::DoubleGrid>())
        EXPECT_EQ(scalar(fields, coordinate), 0);
      else
        EXPECT_EQ(vector(fields, coordinate), V(0));
    }
  }
  static void sameMap(const openvdb::GridBase &a, const openvdb::GridBase &b) {
    EXPECT_EQ(a.transform().baseMap()->applyMap(V(0)),
              b.transform().baseMap()->applyMap(V(0)));
    for (int axis = 0; axis < 3; ++axis) {
      V e(0);
      e[axis] = 1;
      EXPECT_EQ(a.transform().baseMap()->applyJacobian(e),
                b.transform().baseMap()->applyJacobian(e));
    }
  }
  static constexpr Operation operations[] = {
      Operation::MeanCurvature, Operation::GaussianCurvature, Operation::Normal,
      Operation::ClosestPoint};
  std::filesystem::path directory;
  vdb::SurfaceFieldsOptions options;
  vdb::ProcessingOptions processing;
  unsigned failureSerial{0};
};

TEST_F(OpenVdbSurfaceFields, PlaneNativeTypesValuesActivityAndSemanticTags) {
  for (bool nativeFloat : {true, false}) {
    openvdb::GridBase::Ptr input =
        nativeFloat ? openvdb::GridBase::Ptr(plane<openvdb::FloatGrid>())
                    : openvdb::GridBase::Ptr(plane<openvdb::DoubleGrid>());
    write("input.vdb", {input});
    const auto original = bytes(path("input.vdb"));
    for (auto operation : operations) {
      const auto output = std::to_string(nativeFloat) + "-" +
                          std::to_string(int(operation)) + ".vdb";
      const auto result = run(operation, "input.vdb", output);
      const auto fields = read(output);
      EXPECT_EQ(result.valueType, fields.value->valueType());
      EXPECT_EQ(result.inputActiveVoxelCount, 125u);
      EXPECT_GT(result.validVoxelCount, 0u);
      EXPECT_EQ(result.validVoxelCount, fields.valid->activeVoxelCount());
      EXPECT_EQ(result.field.activeVoxelCount, result.validVoxelCount);
      EXPECT_EQ(result.validity.activeVoxelCount, result.validVoxelCount);
      EXPECT_GT(result.gradientSampleCount, 0u);
      EXPECT_NEAR(result.maximumUnitGradientError, 0, 1e-12);
      EXPECT_GT(result.workItems, 0u);
      EXPECT_EQ(fields.value->getGridClass(), openvdb::GRID_UNKNOWN);
      EXPECT_EQ(fields.valid->getGridClass(), openvdb::GRID_UNKNOWN);
      EXPECT_FALSE(fields.valid->background());
      validity(fields, {}, true);
      validity(fields, {2, 0, 0}, false);
      validity(fields, {100, 0, 0}, false);
      if (operation == Operation::MeanCurvature ||
          operation == Operation::GaussianCurvature) {
        EXPECT_EQ(nativeFloat, fields.value->isType<openvdb::FloatGrid>());
        EXPECT_EQ(scalar(fields, {}), 0);
        EXPECT_EQ(fields.value->getVectorType(), openvdb::VEC_INVARIANT);
      } else {
        EXPECT_EQ(nativeFloat, fields.value->isType<openvdb::Vec3SGrid>());
        EXPECT_TRUE(fields.value->isInWorldSpace());
        EXPECT_EQ(vector(fields, {}),
                  operation == Operation::Normal ? V(1, 0, 0) : V(-.25, 0, 0));
        EXPECT_EQ(fields.value->getVectorType(),
                  operation == Operation::Normal
                      ? openvdb::VEC_COVARIANT_NORMALIZE
                      : openvdb::VEC_CONTRAVARIANT_ABSOLUTE);
      }
      sameMap(*input, *fields.value);
      sameMap(*input, *fields.valid);
    }
    EXPECT_EQ(bytes(path("input.vdb")), original);
  }
}

TEST_F(OpenVdbSurfaceFields,
       SphereCurvatureBelongsToCellRadiusNotSourceZeroSet) {
  write("input.vdb", {sphere()});
  for (auto operation : operations) {
    const auto name = "sphere-" + std::to_string(int(operation)) + ".vdb";
    (void)run(operation, "input.vdb", name);
    const auto fields = read(name);
    for (int rho : {5, 7}) {
      const openvdb::Coord c(rho, 0, 0);
      validity(fields, c, true);
      if (operation == Operation::MeanCurvature)
        EXPECT_NEAR(scalar(fields, c), 1. / rho, .0021);
      else if (operation == Operation::GaussianCurvature)
        EXPECT_NEAR(scalar(fields, c), 1. / (rho * rho), .00085);
      else if (operation == Operation::Normal)
        EXPECT_EQ(vector(fields, c), V(1, 0, 0));
      else
        EXPECT_EQ(vector(fields, c), V(6, 0, 0));
    }
  }
}

TEST_F(OpenVdbSurfaceFields,
       TorusOuterInnerAndTopHaveIndependentSignedGeometry) {
  auto grid = openvdb::DoubleGrid::create(3);
  grid->setName("torus");
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  for (int x = -20; x <= 20; ++x)
    for (int y = -20; y <= 20; ++y)
      for (int z = -8; z <= 8; ++z) {
        const double d =
            std::hypot(std::hypot(double(x), double(y)) - 12, double(z)) - 4;
        if (std::abs(d) < 3)
          grid->tree().setValue({x, y, z}, d);
        else if (d < 0)
          grid->tree().setValueOff({x, y, z}, -3);
      }
  write("input.vdb", {grid});
  const std::array<openvdb::Coord, 3> points = {openvdb::Coord(16, 0, 0),
                                                openvdb::Coord(8, 0, 0),
                                                openvdb::Coord(12, 0, 4)};
  for (auto operation : {Operation::MeanCurvature, Operation::GaussianCurvature,
                         Operation::Normal}) {
    const auto name = "torus-" + std::to_string(int(operation)) + ".vdb";
    (void)run(operation, "input.vdb", name);
    const auto fields = read(name);
    for (const auto &c : points)
      validity(fields, c, true);
    if (operation == Operation::MeanCurvature) {
      EXPECT_NEAR(scalar(fields, points[0]), (1. / 4 + 1. / 16) / 2, .003);
      EXPECT_NEAR(scalar(fields, points[1]), (1. / 4 - 1. / 8) / 2, .003);
      EXPECT_NEAR(scalar(fields, points[2]), 1. / 8, .003);
    } else if (operation == Operation::GaussianCurvature) {
      EXPECT_NEAR(scalar(fields, points[0]), 1. / 64, .002);
      EXPECT_NEAR(scalar(fields, points[1]), -1. / 32, .002);
      EXPECT_NEAR(scalar(fields, points[2]), 0, .0003);
    } else {
      EXPECT_EQ(vector(fields, points[0]), V(1, 0, 0));
      EXPECT_EQ(vector(fields, points[1]), V(-1, 0, 0));
      EXPECT_EQ(vector(fields, points[2]), V(0, 0, 1));
    }
  }
}

TEST_F(OpenVdbSurfaceFields,
       FaceEdgeAndCornerRemovalDistinguishExactFootprints) {
  const std::array<openvdb::Coord, 3> removed = {openvdb::Coord(1, 1, 1),
                                                 openvdb::Coord(1, 1, 0),
                                                 openvdb::Coord(1, 0, 0)};
  for (size_t sample = 0; sample < removed.size(); ++sample) {
    auto grid = plane();
    grid->tree().setValueOff(removed[sample], grid->background());
    write("input.vdb", {grid});
    for (auto operation : operations) {
      const auto name = "footprint-" + std::to_string(sample) + "-" +
                        std::to_string(int(operation)) + ".vdb";
      const auto result = run(operation, "input.vdb", name);
      const auto fields = read(name);
      const bool curvature = operation == Operation::MeanCurvature ||
                             operation == Operation::GaussianCurvature;
      validity(fields, {}, sample == 0 || (sample == 1 && !curvature));
      EXPECT_GT(result.missingFootprintVoxelCount, 0u);
      EXPECT_EQ(result.inputActiveVoxelCount,
                result.validVoxelCount + result.missingFootprintVoxelCount +
                    result.degenerateGradientVoxelCount);
    }
  }
}

TEST_F(OpenVdbSurfaceFields,
       NativeCptUsesUnnormalizedGradientAndReportsSdfQuality) {
  write("input.vdb", {plane(1, 2, .25)});
  const auto result = run(Operation::ClosestPoint);
  const auto fields = read("output.vdb");
  validity(fields, {}, true);
  EXPECT_EQ(vector(fields, {}), V(-.5, 0, 0));
  EXPECT_NE(vector(fields, {}), V(-.25, 0, 0));
  EXPECT_NEAR(result.maximumUnitGradientError, 1, 1e-12);
  (void)run(Operation::Normal, "input.vdb", "normal.vdb");
  EXPECT_EQ(vector(read("normal.vdb"), {}), V(1, 0, 0));
}

TEST_F(OpenVdbSurfaceFields,
       ZeroGradientHasInactiveZeroValuesAndSeparateCount) {
  auto grid = plane();
  for (const auto &c :
       {openvdb::Coord(0), openvdb::Coord(-1, 0, 0), openvdb::Coord(1, 0, 0),
        openvdb::Coord(0, -1, 0), openvdb::Coord(0, 1, 0),
        openvdb::Coord(0, 0, -1), openvdb::Coord(0, 0, 1)})
    grid->tree().setValue(c, .25);
  write("input.vdb", {grid});
  for (auto operation : operations) {
    const auto name = "degenerate-" + std::to_string(int(operation)) + ".vdb";
    const auto result = run(operation, "input.vdb", name);
    validity(read(name), {}, false);
    EXPECT_GT(result.degenerateGradientVoxelCount, 0u);
    EXPECT_EQ(result.inputActiveVoxelCount,
              result.validVoxelCount + result.missingFootprintVoxelCount +
                  result.degenerateGradientVoxelCount);
  }
}

TEST_F(OpenVdbSurfaceFields,
       ReflectionRotationAndTranslationPreserveWorldGeometry) {
  auto grid = plane();
  grid->transform().postScale(V(-2, 2, 2));
  grid->transform().postRotate(.37, openvdb::math::Z_AXIS);
  grid->transform().postTranslate(V(1e6 + .125, -100, 20));
  for (auto value = grid->beginValueAll(); value; ++value)
    value.setValue(*value * 2);
  grid->tree().root().setBackground(16, false);
  write("input.vdb", {grid});
  const V expectedNormal(-std::cos(.37), -std::sin(.37), 0);
  const V expectedPoint = V(1e6 + .125, -100, 20) - .5 * expectedNormal;
  for (auto operation : operations) {
    const auto name = "world-" + std::to_string(int(operation)) + ".vdb";
    (void)run(operation, "input.vdb", name);
    const auto fields = read(name);
    validity(fields, {}, true);
    sameMap(*grid, *fields.value);
    sameMap(*grid, *fields.valid);
    if (operation == Operation::Normal ||
        operation == Operation::ClosestPoint) {
      const auto expected =
          operation == Operation::Normal ? expectedNormal : expectedPoint;
      for (int axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(vector(fields, {})[axis], expected[axis], 2e-10);
    } else
      EXPECT_NEAR(scalar(fields, {}), 0, 1e-12);
  }
}

TEST_F(OpenVdbSurfaceFields,
       ExtremeVoxelSizesKeepFloatAndDoubleCurvatureUnits) {
  for (bool nativeFloat : {true, false})
    for (double h : {1e-6, 1e6}) {
      openvdb::GridBase::Ptr grid =
          nativeFloat ? openvdb::GridBase::Ptr(sphere<openvdb::FloatGrid>(h))
                      : openvdb::GridBase::Ptr(sphere<openvdb::DoubleGrid>(h));
      write("input.vdb", {grid});
      for (auto operation :
           {Operation::MeanCurvature, Operation::GaussianCurvature,
            Operation::ClosestPoint}) {
        const auto name = "scale-" + std::to_string(nativeFloat) + "-" +
                          std::to_string(h) + "-" +
                          std::to_string(int(operation)) + ".vdb";
        (void)run(operation, "input.vdb", name);
        const auto fields = read(name);
        const openvdb::Coord c(7, 0, 0);
        validity(fields, c, true);
        sameMap(*grid, *fields.value);
        if (operation == Operation::MeanCurvature)
          EXPECT_NEAR(scalar(fields, c) * h, 1. / 7, .001);
        else if (operation == Operation::GaussianCurvature)
          EXPECT_NEAR(scalar(fields, c) * h * h, 1. / 49, .00025);
        else {
          EXPECT_NEAR(vector(fields, c).x() / h, 6, 2e-5);
          EXPECT_EQ(vector(fields, c).y(), 0);
          EXPECT_EQ(vector(fields, c).z(), 0);
        }
      }
    }
}

TEST_F(OpenVdbSurfaceFields, RemoteIndexCellsAreRestoredWithExactOriginalMap) {
  const openvdb::Coord offset(-1'000'000'000, 100'000'000, -100'000'000);
  auto grid = plane(1, 1, .25, offset);
  grid->transform().postTranslate(V(1e9, -1e8, 1e8));
  write("input.vdb", {grid});
  for (auto operation : operations) {
    const auto name = "index-" + std::to_string(int(operation)) + ".vdb";
    (void)run(operation, "input.vdb", name);
    const auto fields = read(name);
    validity(fields, offset, true);
    validity(fields, {}, false);
    sameMap(*grid, *fields.value);
    if (operation == Operation::Normal)
      EXPECT_EQ(vector(fields, offset), V(1, 0, 0));
    else if (operation == Operation::ClosestPoint)
      EXPECT_EQ(vector(fields, offset), V(-.25, 0, 0));
    else
      EXPECT_EQ(scalar(fields, offset), 0);
  }
}

TEST_F(OpenVdbSurfaceFields,
       FloatAbsoluteCptPrecisionRequiresExplicitPromotion) {
  auto grid = plane<openvdb::FloatGrid>();
  grid->transform().postTranslate(V(1e8, 0, 0));
  write("input.vdb", {grid});
  (void)run(Operation::Normal, "input.vdb", "normal.vdb");
  EXPECT_TRUE(read("normal.vdb").value->isType<openvdb::Vec3SGrid>());
  fails(Operation::ClosestPoint);
  options.promoteToDouble = true;
  const auto result = run(Operation::ClosestPoint);
  const auto fields = read("output.vdb");
  EXPECT_TRUE(fields.value->isType<openvdb::Vec3DGrid>());
  EXPECT_EQ(result.valueType, fields.value->valueType());
  validity(fields, {}, true);
  EXPECT_EQ(vector(fields, {}), V(1e8 - .25, 0, 0));
}

TEST_F(OpenVdbSurfaceFields,
       DoubleAbsoluteCptPrecisionDoesNotRestrictDerivatives) {
  auto grid = plane();
  grid->transform().postTranslate(V(1e15, 0, 0));
  write("input.vdb", {grid});
  for (auto operation : {Operation::MeanCurvature, Operation::GaussianCurvature,
                         Operation::Normal}) {
    const auto name = "derivative-" + std::to_string(int(operation)) + ".vdb";
    (void)run(operation, "input.vdb", name);
    validity(read(name), {}, true);
  }
  fails(Operation::ClosestPoint);
}

TEST_F(OpenVdbSurfaceFields,
       NativeFloatNonzeroCurvatureUnderflowRejectsUnlessPromoted) {
  constexpr float epsilon = 1e-30f;
  auto grid = plane<openvdb::FloatGrid>(1, 1, 0);
  for (auto value = grid->beginValueOn(); value; ++value) {
    const auto c = value.getCoord();
    value.setValue(float(c.x()) +
                   epsilon * float(c.y() * c.y() + c.z() * c.z()));
  }
  write("input.vdb", {grid});
  fails(Operation::GaussianCurvature);
  options.promoteToDouble = true;
  (void)run(Operation::GaussianCurvature);
  const auto fields = read("output.vdb");
  EXPECT_TRUE(fields.value->isType<openvdb::DoubleGrid>());
  validity(fields, {}, true);
  const double expected = 4 * double(epsilon) * double(epsilon);
  EXPECT_GT(scalar(fields, {}), 0);
  EXPECT_NEAR(scalar(fields, {}), expected, expected * 1e-6);
}

TEST_F(OpenVdbSurfaceFields,
       EmptyPositiveStorageIsDiagnosedButHiddenNegativeRejects) {
  auto grid = openvdb::DoubleGrid::create(4);
  grid->setName("empty");
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  grid->tree().setValueOff({0, 0, 0}, 4);
  write("input.vdb", {grid});
  for (auto operation : operations) {
    const auto name = "empty-" + std::to_string(int(operation)) + ".vdb";
    const auto result = run(operation, "input.vdb", name);
    const auto fields = read(name);
    EXPECT_EQ(result.inputActiveVoxelCount, 0u);
    EXPECT_EQ(result.validVoxelCount, 0u);
    EXPECT_EQ(result.gradientSampleCount, 0u);
    EXPECT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(fields.value->activeVoxelCount(), 0u);
    EXPECT_EQ(fields.valid->activeVoxelCount(), 0u);
    validity(fields, {}, false);
  }
  grid->tree().setValueOff({0, 0, 0}, -4);
  write("hidden.vdb", {grid});
  fails(Operation::Normal, "hidden.vdb");
}

TEST_F(
    OpenVdbSurfaceFields,
    AllocatedPositiveBackgroundIsEmptyButHiddenNegativeLeavesAndTilesReject) {
  const auto check = [&]<class G>(const std::string &kind) {
    auto grid = G::create(typename G::ValueType(4));
    grid->setName("allocated_empty");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    grid->tree().touchLeaf({0, 0, 0});
    grid->tree().touchLeaf({32, 0, 0});
    ASSERT_EQ(grid->activeVoxelCount(), 0u);
    ASSERT_EQ(grid->tree().leafCount(), 2u);
    ASSERT_FALSE(grid->empty()) << "Native empty() tests retained structure";
    const auto source = kind + "-allocated.vdb";
    write(source, {grid});
    const auto original = bytes(path(source));
    for (auto operation : operations) {
      const auto output =
          kind + "-empty-" + std::to_string(int(operation)) + ".vdb";
      const auto result = run(operation, source, output);
      const auto fields = read(output);
      EXPECT_EQ(result.inputActiveVoxelCount, 0u);
      EXPECT_EQ(result.validVoxelCount, 0u);
      EXPECT_EQ(result.gradientSampleCount, 0u);
      EXPECT_EQ(result.missingFootprintVoxelCount, 0u);
      EXPECT_EQ(result.degenerateGradientVoxelCount, 0u);
      EXPECT_EQ(fields.value->activeVoxelCount(), 0u);
      EXPECT_EQ(fields.valid->activeVoxelCount(), 0u);
      EXPECT_TRUE(std::any_of(result.diagnostics.begin(),
                              result.diagnostics.end(), [](const auto &text) {
                                return text.find("Empty positive-background") !=
                                       std::string::npos;
                              }));
    }
    EXPECT_EQ(bytes(path(source)), original);
    grid->tree().setValueOff({0, 0, 0}, typename G::ValueType(-4));
    write(kind + "-hidden-leaf.vdb", {grid});
    fails(Operation::Normal, kind + "-hidden-leaf.vdb");
    grid->tree().setValueOff({0, 0, 0}, typename G::ValueType(4));
    grid->tree().addTile(3, {4096, 0, 0}, typename G::ValueType(-4), false);
    ASSERT_EQ(grid->activeVoxelCount(), 0u);
    write(kind + "-hidden-root-tile.vdb", {grid});
    fails(Operation::Normal, kind + "-hidden-root-tile.vdb");
  };
  check.template operator()<openvdb::FloatGrid>("float");
  check.template operator()<openvdb::DoubleGrid>("double");
}

TEST_F(OpenVdbSurfaceFields,
       ExactDescriptorsResolveDuplicatesAndLiteralPrefixes) {
  auto first = plane(), second = plane(1, 1, .5);
  write("input.vdb", {first, second});
  fails(Operation::Normal, "input.vdb", "plane");
  fails(Operation::Normal, "input.vdb", "descriptor:missing");
  (void)run(Operation::ClosestPoint, "input.vdb", "selected.vdb",
            "descriptor:plane[1]");
  EXPECT_EQ(vector(read("selected.vdb"), {}), V(-.5, 0, 0));
  first->setName("descriptor:plane");
  write("literal.vdb", {first});
  (void)run(Operation::ClosestPoint, "literal.vdb", "literal-out.vdb",
            "descriptor:descriptor:plane");
  EXPECT_EQ(vector(read("literal-out.vdb"), {}), V(-.25, 0, 0));
}

TEST_F(OpenVdbSurfaceFields, UnicodeMetadataNamesAndExistingFilesArePreserved) {
  auto grid = plane();
  grid->insertMeta("artist", openvdb::StringMetadata("original scalar"));
  grid->insertMeta("exact_id", openvdb::Int64Metadata(9'007'199'254'740'993));
  openvdb::MetaMap fileMetadata;
  fileMetadata.insertMeta("project",
                          openvdb::StringMetadata("retained file metadata"));
  write("input.vdb", {grid}, fileMetadata);
  const auto source = directory / std::filesystem::path(u"field_球.vdb");
  const auto output = directory / std::filesystem::path(u"derived_球.vdb");
  std::filesystem::copy_file(path("input.vdb"), source);
  const auto original = bytes(source);
  processing.gridName = "descriptor:derived value";
  options.validityGridName = "descriptor:valid value";
  (void)vdb::WriteSurfaceFields(source, output, Operation::Normal, options, {},
                                processing);
  std::filesystem::copy_file(output, path("read.vdb"));
  const auto fields = read("read.vdb");
  EXPECT_EQ(fields.value->metaValue<std::string>("artist"), "original scalar");
  EXPECT_EQ(fields.valid->metaValue<std::string>("artist"), "original scalar");
  EXPECT_EQ(fields.value->metaValue<int64_t>("exact_id"),
            9'007'199'254'740'993);
  EXPECT_EQ(fields.fileMetadata->metaValue<std::string>("project"),
            "retained file metadata");
  const auto published = bytes(output);
  EXPECT_THROW((void)vdb::WriteSurfaceFields(source, output, Operation::Normal,
                                             options, {}, processing),
               std::exception);
  EXPECT_THROW((void)vdb::WriteSurfaceFields(source, source, Operation::Normal,
                                             options, {}, processing),
               std::exception);
  EXPECT_EQ(bytes(source), original);
  EXPECT_EQ(bytes(output), published);
}

TEST_F(OpenVdbSurfaceFields, InvalidTypesClassesValuesBandsTilesAndMapsReject) {
  auto check = [&](const openvdb::GridBase::Ptr &grid) {
    write("invalid.vdb", {grid});
    fails(Operation::MeanCurvature, "invalid.vdb");
  };
  auto grid = plane();
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  check(grid);
  check(openvdb::Int32Grid::create(4));
  for (double value : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(), 9.}) {
    grid = plane();
    grid->tree().setValue({0, 0, 0}, value);
    check(grid);
  }
  grid = plane();
  grid->tree().setValueOff({32, 0, 0},
                           std::numeric_limits<double>::quiet_NaN());
  check(grid);
  grid = plane();
  grid->tree().setValueOff({32, 0, 0}, 4);
  check(grid);
  grid = plane();
  grid->tree().addTile(1, {32, 0, 0}, 2, true);
  check(grid);
  grid = plane();
  grid->transform().postScale(V(1, 2, 1));
  check(grid);
  grid = plane();
  for (auto value = grid->beginValueOn(); value; ++value)
    value.setValue(std::abs(*value) + .25);
  check(grid);
  grid = plane();
  grid->tree().root().setBackground(0, false);
  check(grid);
}

TEST_F(OpenVdbSurfaceFields,
       WorkInputTopologyMetadataCatalogAndNameLimitsReject) {
  auto grid = plane();
  auto other = openvdb::Int32Grid::create(0);
  other->setName("unselected");
  other->insertMeta("note", openvdb::StringMetadata(std::string(1024, 'm')));
  write("input.vdb", {grid, other});
  const auto ordinaryOptions = options;
  const auto ordinaryProcessing = processing;
  for (uint64_t cap : {0ull, 1ull, 10'000'000'001ull}) {
    options.maxWorkItems = cap;
    fails(Operation::Normal, "input.vdb", "plane");
  }
  options = ordinaryOptions;
  options.maxGrids = 1;
  fails(Operation::Normal, "input.vdb", "plane");
  options = ordinaryOptions;
  options.maxMetadataBytes = 1024;
  fails(Operation::Normal, "input.vdb", "plane");
  options = ordinaryOptions;
  processing.maxInputBytes = 1;
  fails(Operation::Normal, "input.vdb", "plane");
  processing = ordinaryProcessing;
  processing.maxVoxelCount = 1;
  fails(Operation::Normal, "input.vdb", "plane");
  processing = ordinaryProcessing;
  for (const std::string &name : {std::string{}, std::string(257, 'n')}) {
    processing.gridName = name;
    fails(Operation::Normal, "input.vdb", "plane");
  }
  processing = ordinaryProcessing;
  options.validityGridName = processing.gridName;
  fails(Operation::Normal, "input.vdb", "plane");
  options = ordinaryOptions;
  fails(static_cast<Operation>(99), "input.vdb", "plane");
}

TEST_F(OpenVdbSurfaceFields,
       InactiveRemoteLeavesAndBothOutputsShareTopologyBudget) {
  auto grid = plane();
  grid->tree().setValueOff({1024, 0, 0}, grid->background());
  write("input.vdb", {grid});
  processing.maxVoxelCount = grid->tree().leafCount() * 512;
  fails(Operation::Normal);
  processing.maxVoxelCount = 1'000'000;
  const auto result = run(Operation::Normal);
  const auto fields = read("output.vdb");
  EXPECT_EQ(result.inputActiveVoxelCount, 125u);
  validity(fields, {}, true);
  validity(fields, {1024, 0, 0}, false);
}

TEST_F(OpenVdbSurfaceFields,
       ExactSubnormalAffineCoefficientCannotSilentlyVanishDuringConditioning) {
  constexpr double h = 1e6, offAxis = 1e-320;
  auto grid = plane(h);
  auto matrix = openvdb::math::Mat4d::identity();
  for (int axis = 0; axis < 3; ++axis)
    matrix(axis, axis) = h;
  matrix(0, 1) = offAxis;
  // Author the public map record directly: Transform's convenience factories
  // may simplify a numerically near-diagonal affine map before it is written.
  std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
  openvdb::writeString(record, openvdb::math::AffineMap::mapType());
  matrix.write(record);
  record.seekg(0);
  openvdb::io::setCurrentVersion(record);
  grid->transform().read(record);
  write("input.vdb", {grid});
  {
    openvdb::io::File source(path("input.vdb").string());
    source.open(false);
    const auto persisted = source.readGrid("plane");
    const auto axis =
        persisted->transform().baseMap()->applyJacobian(V(1, 0, 0));
    ASSERT_EQ(axis[0], h);
    ASSERT_EQ(axis[1], offAxis);
    ASSERT_NE(axis[1], 0);
    EXPECT_EQ(axis[1] / h, 0);
  }
  const auto original = bytes(path("input.vdb"));
  try {
    (void)run(Operation::Normal);
    FAIL() << "An unrepresentable nonzero world-normal component must reject";
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find("range"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("output.vdb")));
  EXPECT_EQ(bytes(path("input.vdb")), original);
}

TEST_F(OpenVdbSurfaceFields,
       MalformedVectorAndWrongTypedWorldMetadataCannotBecomeDefaultSemantics) {
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto grid = plane();
    const char *key = mode < 2 ? openvdb::GridBase::META_VECTOR_TYPE
                               : openvdb::GridBase::META_IS_LOCAL_SPACE;
    grid->removeMeta(key);
    if (mode == 0)
      grid->insertMeta(key, openvdb::StringMetadata("not-a-vector-semantic"));
    else if (mode == 1)
      grid->insertMeta(key, openvdb::Int32Metadata(0));
    else if (mode == 2)
      grid->insertMeta(key, openvdb::StringMetadata(""));
    else
      grid->insertMeta(key, openvdb::Int32Metadata(0));
    write("input.vdb", {grid});
    const auto original = bytes(path("input.vdb"));
    const auto output = "semantic-" + std::to_string(mode) + ".vdb";
    try {
      (void)run(Operation::Normal, "input.vdb", output);
      FAIL() << "Malformed structural metadata must not default to "
                "world/invariant";
    } catch (const std::invalid_argument &error) {
      const std::string diagnostic = error.what();
      EXPECT_TRUE(diagnostic.find("metadata") != std::string::npos ||
                  diagnostic.find("semantic") != std::string::npos);
    }
    EXPECT_FALSE(std::filesystem::exists(path(output)));
    EXPECT_EQ(bytes(path("input.vdb")), original);
  }
  auto recognized = plane();
  recognized->insertMeta(openvdb::GridBase::META_VECTOR_TYPE,
                         openvdb::StringMetadata(" \tINVARIANT \n"));
  recognized->insertMeta(openvdb::GridBase::META_IS_LOCAL_SPACE,
                         openvdb::BoolMetadata(false));
  write("recognized.vdb", {recognized});
  (void)run(Operation::Normal, "recognized.vdb", "recognized-out.vdb");
  validity(read("recognized-out.vdb"), {}, true);
}
#endif
} // namespace
