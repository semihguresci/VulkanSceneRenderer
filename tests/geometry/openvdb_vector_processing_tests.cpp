#include "Container/geometry/OpenVdbVectorProcessing.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
using Operation = vdb::DifferentialOperation;

TEST(OpenVdbVectorProcessingAvailability, DisabledOperationsFailBeforePathOrParameterAccess) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  for (int operation = 0; operation < 2; ++operation) {
    try {
      if (operation == 0) (void)vdb::WriteDifferential("missing.vdb", {}, Operation::Gradient);
      else (void)vdb::SampleVector("missing.vdb", {std::numeric_limits<double>::quiet_NaN(), 0, 0});
      FAIL() << "disabled operation must throw first";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
    }
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbVectorProcessing : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-vector-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  void write(const char* name, const openvdb::GridPtrVec& grids) {
    openvdb::io::File stream(file(name).string()); stream.write(grids); stream.close();
  }
  template<class Grid> typename Grid::Ptr read(const char* name) {
    openvdb::io::File stream(file(name).string()); stream.open(false);
    const auto result = openvdb::gridPtrCast<Grid>(stream.readGrid(stream.beginName().gridName()));
    stream.close(); return result;
  }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary); return {std::istreambuf_iterator<char>(stream), {}};
  }
  static openvdb::math::Transform::Ptr affine(bool reflect = false) {
    openvdb::math::Mat4d matrix; matrix.setIdentity();
    matrix[0][0] = reflect ? -.5 : .5; matrix[0][1] = .2;
    matrix[1][1] = .75; matrix[1][2] = .1;
    matrix[2][0] = .1; matrix[2][2] = 1.2;
    matrix[3][0] = 3; matrix[3][1] = -2; matrix[3][2] = 1;
    auto result = openvdb::math::Transform::createLinearTransform(matrix);
    result->postRotate(.4, openvdb::math::Z_AXIS);
    return result;
  }
  template<class Grid, class Function>
  typename Grid::Ptr field(Function function, bool reflect = false) {
    auto grid = Grid::create(typename Grid::ValueType(0));
    grid->setName("field"); grid->setTransform(affine(reflect));
    grid->setIsInWorldSpace(true); grid->setVectorType(openvdb::VEC_INVARIANT);
    grid->insertMeta("artist", openvdb::StringMetadata("retain custom metadata"));
    auto access = grid->getAccessor();
    for (int x = -4; x <= 4; ++x) for (int y = -4; y <= 4; ++y) for (int z = -4; z <= 4; ++z) {
      const openvdb::Coord index(x,y,z);
      access.setValue(index, typename Grid::ValueType(function(grid->indexToWorld(index.asVec3d()))));
    }
    return grid;
  }
  template<class Vector> static void nearVector(const Vector& value, const openvdb::Vec3d& expected, double tolerance) {
    for (size_t axis = 0; axis < 3; ++axis) EXPECT_NEAR(double(value[axis]), expected[axis], tolerance);
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions options;
};

TEST_F(OpenVdbVectorProcessing, DoubleGradientUsesWorldUnitsUnderRotatedShearedReflectedAffine) {
  for (bool reflect : {false, true}) {
    auto grid = field<openvdb::DoubleGrid>([](const auto& p) { return 2*p.x()-3*p.y()+4*p.z()+7; }, reflect);
    write("input.vdb", {grid}); const auto original = bytes(file("input.vdb"));
    const char* output = reflect ? "reflected.vdb" : "gradient.vdb";
    options.gridName = "gradient";
    const auto result = vdb::WriteDifferential(file("input.vdb"), file(output), Operation::Gradient, {}, options);
    const auto gradient = read<openvdb::Vec3DGrid>(output); ASSERT_TRUE(gradient);
    nearVector(gradient->tree().getValue(openvdb::Coord(0)), {2,-3,4}, 1e-11);
    EXPECT_EQ(gradient->transform(), grid->transform());
    EXPECT_EQ(gradient->metaValue<std::string>("artist"), "retain custom metadata");
    EXPECT_TRUE(gradient->isInWorldSpace()); EXPECT_EQ(gradient->getVectorType(), openvdb::VEC_COVARIANT);
    EXPECT_EQ(gradient->getGridClass(), openvdb::GRID_UNKNOWN);
    EXPECT_EQ(result.gridName, "gradient"); EXPECT_EQ(result.gridClass, "unknown");
    EXPECT_EQ(bytes(file("input.vdb")), original);
  }
}

TEST_F(OpenVdbVectorProcessing, QuadraticLaplacianAndComposedGradientCalculusAgree) {
  auto grid = field<openvdb::DoubleGrid>([](const auto& p) { return p.x()*p.x()+2*p.y()*p.y()+3*p.z()*p.z()+p.x()*p.y(); });
  write("input.vdb", {grid});
  (void)vdb::WriteDifferential(file("input.vdb"), file("laplacian.vdb"), Operation::Laplacian);
  EXPECT_NEAR(read<openvdb::DoubleGrid>("laplacian.vdb")->tree().getValue(openvdb::Coord(0)), 12, 1e-10);
  (void)vdb::WriteDifferential(file("input.vdb"), file("gradient.vdb"), Operation::Gradient);
  (void)vdb::WriteDifferential(file("gradient.vdb"), file("divgrad.vdb"), Operation::Divergence);
  (void)vdb::WriteDifferential(file("gradient.vdb"), file("curlgrad.vdb"), Operation::Curl);
  EXPECT_NEAR(read<openvdb::DoubleGrid>("divgrad.vdb")->tree().getValue(openvdb::Coord(0)), 12, 1e-10);
  nearVector(read<openvdb::Vec3DGrid>("curlgrad.vdb")->tree().getValue(openvdb::Coord(0)), {0,0,0}, 1e-11);
  const auto origin = grid->indexToWorld(openvdb::Vec3d(0));
  const auto sampled = vdb::SampleVector(file("gradient.vdb"), {origin.x(),origin.y(),origin.z()});
  nearVector(sampled, {2*origin.x()+origin.y(),4*origin.y()+origin.x(),6*origin.z()}, 1e-10);
}

TEST_F(OpenVdbVectorProcessing, FloatPrecisionAndWorldVectorDivergenceCurlArePreserved) {
  auto vector = field<openvdb::Vec3SGrid>([](const auto& p) { return openvdb::Vec3d(2*p.x()+3*p.y(),-p.y()+4*p.z(),5*p.x()+6*p.z()); });
  write("vector.vdb", {vector});
  (void)vdb::WriteDifferential(file("vector.vdb"), file("div.vdb"), Operation::Divergence);
  (void)vdb::WriteDifferential(file("vector.vdb"), file("curl.vdb"), Operation::Curl);
  const auto divergence = read<openvdb::FloatGrid>("div.vdb"); ASSERT_TRUE(divergence);
  const auto curl = read<openvdb::Vec3SGrid>("curl.vdb"); ASSERT_TRUE(curl);
  EXPECT_NEAR(divergence->tree().getValue(openvdb::Coord(0)), 7, 2e-5);
  nearVector(curl->tree().getValue(openvdb::Coord(0)), {-4,-5,-3}, 2e-5);
  const auto index = openvdb::Vec3d(.25,-.5,.75), point = vector->indexToWorld(index);
  nearVector(vdb::SampleVector(file("vector.vdb"), {point.x(),point.y(),point.z()}),
             {2*point.x()+3*point.y(),-point.y()+4*point.z(),5*point.x()+6*point.z()}, 2e-5);
  auto scalar = field<openvdb::FloatGrid>([](const auto& p) { return 2*p.x()-3*p.y()+4*p.z(); });
  write("scalar.vdb", {scalar});
  (void)vdb::WriteDifferential(file("scalar.vdb"), file("gradient.vdb"), Operation::Gradient);
  ASSERT_TRUE(read<openvdb::Vec3SGrid>("gradient.vdb"));
}

TEST_F(OpenVdbVectorProcessing, PointwiseOperationsRetainActiveTilesAndDeriveInactiveBackground) {
  auto grid = openvdb::Vec3DGrid::create({0,0,2}); grid->setName("tiles"); grid->setIsInWorldSpace(true);
  grid->tree().addTile(1, openvdb::Coord(0), {3,4,0}, true);
  grid->tree().addTile(1, openvdb::Coord(8,0,0), {9,0,0}, false);
  write("tiles.vdb", {grid});
  nearVector(vdb::SampleVector(file("tiles.vdb"), {9,1,1}), {9,0,0}, 0);
  (void)vdb::WriteDifferential(file("tiles.vdb"), file("magnitude.vdb"), Operation::Magnitude);
  const auto magnitude = read<openvdb::DoubleGrid>("magnitude.vdb");
  EXPECT_TRUE(magnitude->tree().hasActiveTiles()); EXPECT_EQ(magnitude->tree().leafCount(), 0u);
  EXPECT_DOUBLE_EQ(magnitude->tree().getValue(openvdb::Coord(1)), 5);
  EXPECT_DOUBLE_EQ(magnitude->tree().getValue(openvdb::Coord(9,1,1)), 2);
  EXPECT_DOUBLE_EQ(magnitude->background(), 2);
  (void)vdb::WriteDifferential(file("tiles.vdb"), file("normalized.vdb"), Operation::Normalize);
  const auto normalized = read<openvdb::Vec3DGrid>("normalized.vdb");
  EXPECT_TRUE(normalized->tree().hasActiveTiles());
  nearVector(normalized->tree().getValue(openvdb::Coord(1)), {.6,.8,0}, 1e-14);
  nearVector(normalized->tree().getValue(openvdb::Coord(9,1,1)), {0,0,1}, 1e-14);
}

TEST_F(OpenVdbVectorProcessing, TinyNonzeroAndZeroVectorsNormalizeWithoutAbsoluteCutoff) {
  for (bool single : {true, false}) {
    const char* input = single ? "float.vdb" : "double.vdb";
    const char* output = single ? "float-normal.vdb" : "double-normal.vdb";
    const char* magnitude = single ? "float-length.vdb" : "double-length.vdb";
    if (single) {
      auto grid = openvdb::Vec3SGrid::create({0,0,0}); grid->setName("field"); grid->setIsInWorldSpace(true);
      grid->getAccessor().setValue(openvdb::Coord(0), {std::numeric_limits<float>::denorm_min(),0,0});
      grid->getAccessor().setValue(openvdb::Coord(1,0,0), {0,0,0}); write(input, {grid});
    } else {
      auto grid = openvdb::Vec3DGrid::create({0,0,0}); grid->setName("field"); grid->setIsInWorldSpace(true);
      grid->getAccessor().setValue(openvdb::Coord(0), {1e-300,0,0});
      grid->getAccessor().setValue(openvdb::Coord(1,0,0), {0,0,0}); write(input, {grid});
    }
    (void)vdb::WriteDifferential(file(input), file(output), Operation::Normalize);
    (void)vdb::WriteDifferential(file(input), file(magnitude), Operation::Magnitude);
    if (single) {
      const auto grid = read<openvdb::Vec3SGrid>(output);
      nearVector(grid->tree().getValue(openvdb::Coord(0)), {1,0,0}, 0);
      nearVector(grid->tree().getValue(openvdb::Coord(1,0,0)), {0,0,0}, 0);
      EXPECT_GT(read<openvdb::FloatGrid>(magnitude)->tree().getValue(openvdb::Coord(0)), 0);
    } else {
      const auto grid = read<openvdb::Vec3DGrid>(output);
      nearVector(grid->tree().getValue(openvdb::Coord(0)), {1,0,0}, 0);
      nearVector(grid->tree().getValue(openvdb::Coord(1,0,0)), {0,0,0}, 0);
      EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>(magnitude)->tree().getValue(openvdb::Coord(0)), 1e-300);
    }
  }
}

TEST_F(OpenVdbVectorProcessing, DerivativeDensifiesBoundedTilesAndUsesInactiveStencilValues) {
  auto grid = openvdb::DoubleGrid::create(0); grid->setName("field");
  grid->tree().addTile(1, openvdb::Coord(0), 2, true);
  grid->tree().addTile(1, openvdb::Coord(8,0,0), 4, false);
  write("tile.vdb", {grid});
  (void)vdb::WriteDifferential(file("tile.vdb"), file("gradient.vdb"), Operation::Gradient);
  const auto gradient = read<openvdb::Vec3DGrid>("gradient.vdb");
  EXPECT_EQ(gradient->activeVoxelCount(), 512u);
  nearVector(gradient->tree().getValue(openvdb::Coord(7,3,3)), {1,0,0}, 0);
  nearVector(gradient->tree().getValue(openvdb::Coord(3)), {0,0,0}, 0);
  nearVector(gradient->tree().getValue(openvdb::Coord(9,3,3)), {0,0,0}, 0);
}

TEST_F(OpenVdbVectorProcessing, EmptyConstantBackgroundFieldsHaveMeaningfulResults) {
  auto scalar = openvdb::DoubleGrid::create(7); scalar->setName("constant"); write("scalar.vdb", {scalar});
  (void)vdb::WriteDifferential(file("scalar.vdb"), file("gradient.vdb"), Operation::Gradient);
  const auto gradient = read<openvdb::Vec3DGrid>("gradient.vdb");
  EXPECT_TRUE(gradient->empty()); nearVector(gradient->background(), {0,0,0}, 0);
  auto vector = openvdb::Vec3DGrid::create({3,4,0}); vector->setName("constant"); vector->setIsInWorldSpace(true);
  write("vector.vdb", {vector});
  (void)vdb::WriteDifferential(file("vector.vdb"), file("length.vdb"), Operation::Magnitude);
  EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("length.vdb")->background(), 5);
  nearVector(vdb::SampleVector(file("vector.vdb"), {100,100,100}), {3,4,0}, 0);
  auto queryOnly = options;
  queryOnly.voxelSize = std::numeric_limits<double>::quiet_NaN();
  queryOnly.halfWidth = 0; queryOnly.gridName.clear(); queryOnly.maxTriangles = 0;
  nearVector(vdb::SampleVector(file("vector.vdb"), {100,100,100}, {}, queryOnly), {3,4,0}, 0);
}

TEST_F(OpenVdbVectorProcessing, SelectionTypeSemanticsAndUnsafeCoordinatesAreRejected) {
  auto vector = field<openvdb::Vec3DGrid>([](const auto&) { return openvdb::Vec3d(1); });
  write("input.vdb", {vector});
  EXPECT_THROW((void)vdb::WriteDifferential(file("input.vdb"), file("bad.vdb"), Operation::Gradient), std::invalid_argument);
  EXPECT_THROW((void)vdb::SampleVector(file("input.vdb"), {1e100,0,0}), std::invalid_argument);
  EXPECT_THROW((void)vdb::SampleVector(file("input.vdb"), {std::numeric_limits<double>::quiet_NaN(),0,0}), std::invalid_argument);
  EXPECT_THROW((void)vdb::SampleVector(file("input.vdb"), {0,0,0}, "missing"), std::invalid_argument);
  for (int kind : {0,1,2,3}) {
    auto invalid = vector->deepCopy();
    if (kind == 0) invalid->setIsInWorldSpace(false);
    if (kind == 1) invalid->setGridClass(openvdb::GRID_STAGGERED);
    if (kind == 2) invalid->getAccessor().setValueOff(openvdb::Coord(30), {std::numeric_limits<double>::infinity(),0,0});
    if (kind == 3) invalid->getAccessor().setValue(openvdb::Coord(0), {1e13,0,0});
    const auto name = "invalid-" + std::to_string(kind) + ".vdb"; write(name.c_str(), {invalid});
    EXPECT_THROW((void)vdb::WriteDifferential(file(name.c_str()), file("bad.vdb"), Operation::Curl), std::invalid_argument);
  }
  auto other = vector->deepCopy(); write("duplicate.vdb", {vector,other});
  EXPECT_THROW((void)vdb::SampleVector(file("duplicate.vdb"), {0,0,0}, "field"), std::invalid_argument);
  openvdb::io::File duplicate(file("duplicate.vdb").string()); duplicate.open(false);
  const auto descriptor = duplicate.beginName().gridName(); duplicate.close();
  EXPECT_NO_THROW((void)vdb::SampleVector(file("duplicate.vdb"), {0,0,0}, "descriptor:" + descriptor));
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbVectorProcessing, BudgetsOutputProtectionAndInvalidOptionsLeaveNoOutput) {
  auto scalar = field<openvdb::DoubleGrid>([](const auto& p) { return p.x(); }); write("input.vdb", {scalar});
  const auto source = bytes(file("input.vdb"));
  EXPECT_THROW((void)vdb::WriteDifferential(file("input.vdb"), file("input.vdb"), Operation::Gradient), std::invalid_argument);
  std::ofstream(file("existing.vdb")) << "keep existing";
  EXPECT_THROW((void)vdb::WriteDifferential(file("input.vdb"), file("existing.vdb"), Operation::Gradient), std::invalid_argument);
  EXPECT_EQ(bytes(file("existing.vdb")), "keep existing");
  for (int kind : {0,1,2,3,4,5}) {
    auto bounded = options;
    if (kind == 0) bounded.maxVoxelCount = 1;
    if (kind == 1) bounded.maxInputBytes = 1;
    if (kind == 2) bounded.maxInputBytes = 4*1024*1024;
    if (kind == 3) bounded.maxVoxelCount = 0;
    if (kind == 4) bounded.gridName.clear();
    if (kind == 5) bounded.gridName = std::string(257, 'x');
    EXPECT_THROW((void)vdb::WriteDifferential(file("input.vdb"), file("bad.vdb"), Operation::Gradient, {}, bounded), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
  }
  EXPECT_THROW((void)vdb::WriteDifferential(file("input.vdb"), file("bad.vdb"), static_cast<Operation>(999)), std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteDifferential(file("input.vdb"), file("missing") / "bad.vdb", Operation::Gradient), std::invalid_argument);
  EXPECT_EQ(bytes(file("input.vdb")), source);
}

TEST_F(OpenVdbVectorProcessing, MetadataCopiesAreBudgetedAndStreamingInputsAreRejectedBeforeLoading) {
  auto scalar = openvdb::DoubleGrid::create(0); scalar->setName("field");
  scalar->getAccessor().setValue(openvdb::Coord(0), 1);
  write("plain.vdb", {scalar});
  auto bounded = options; bounded.maxInputBytes = 16*1024*1024;
  EXPECT_NO_THROW((void)vdb::WriteDifferential(file("plain.vdb"), file("plain-result.vdb"), Operation::Gradient, {}, bounded));
  scalar->insertMeta("large-custom-field", openvdb::StringMetadata(std::string(2*1024*1024, 'x')));
  write("metadata.vdb", {scalar});
  try {
    (void)vdb::WriteDifferential(file("metadata.vdb"), file("bad.vdb"), Operation::Gradient, {}, bounded);
    FAIL() << "metadata source/output/writer copies must fit the shared workspace budget";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("workspace"), std::string::npos);
  }
  std::ofstream stream(file("stream.vdb"), std::ios::binary);
  openvdb::io::Stream archive(static_cast<std::ostream&>(stream));
  archive.write(openvdb::GridPtrVec{scalar}); stream.close();
  try {
    (void)vdb::WriteDifferential(file("stream.vdb"), file("bad.vdb"), Operation::Gradient);
    FAIL() << "metadata-only budgets cannot allow eager loading offset-free archives";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("random-access"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbVectorProcessing, CollidingSynthesizedDescriptorsNeverSilentlySelectAnotherGrid) {
  auto first = openvdb::DoubleGrid::create(1); first->setName("field");
  auto second = openvdb::DoubleGrid::create(2); second->setName("field");
  auto literal = openvdb::DoubleGrid::create(3); literal->setName("field[0]");
  write("colliding.vdb", {first,second,literal});
  for (const std::string& selection : {std::string(), std::string("field"), std::string("descriptor:field[0]")}) {
    try {
      (void)vdb::WriteDifferential(file("colliding.vdb"), file("bad.vdb"), Operation::Gradient, selection);
      FAIL() << "colliding archive must be rejected before descriptor lookup";
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find("colliding"), std::string::npos);
    }
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbVectorProcessing, TrilinearSamplingCentersAffineInverseAtLargeWorldTranslations) {
  auto vector = openvdb::Vec3DGrid::create({0,0,0}); vector->setName("field"); vector->setIsInWorldSpace(true);
  openvdb::math::Mat4d matrix; matrix.setIdentity();
  matrix[0][0] = .1; matrix[0][1] = .1; matrix[1][1] = .1;
  matrix[3][0] = 1e15; matrix[3][1] = -2e15;
  vector->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
  auto access = vector->getAccessor();
  for (int x = 0; x <= 5; ++x) for (int y = 0; y <= 5; ++y)
    access.setValue(openvdb::Coord(x,y,0), {double(x),double(y),double(x+2*y)});
  write("translated.vdb", {vector});
  // These offsets are representable in world double precision, while absolute
  // inverse translation terms are much larger than their resulting indices.
  nearVector(vdb::SampleVector(file("translated.vdb"), {1e15+.25,-2e15+.5,0}), {2.5,2.5,7.5}, 1e-12);
}

TEST_F(OpenVdbVectorProcessing, SamplingRejectsUnresolvableRemoteIndexFractionsAndAcceptsRecenteredGrid) {
  auto vector = openvdb::Vec3DGrid::create({0,0,0});
  vector->setName("field"); vector->setIsInWorldSpace(true);
  auto transform = openvdb::math::Transform::createLinearTransform(1e6);
  transform->postTranslate({1e15,0,0}); vector->setTransform(transform);
  vector->getAccessor().setValue(openvdb::Coord(-1'000'000'000,0,0), {0,0,0});
  vector->getAccessor().setValue(openvdb::Coord(-999'999'999,0,0), {1e12,0,0});
  write("remote.vdb", {vector});
  try {
    (void)vdb::SampleVector(file("remote.vdb"), {.5,0,0});
    FAIL() << "remote source indices cannot resolve the requested subcell fraction";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("recenter"), std::string::npos);
  }
  auto centered = openvdb::Vec3DGrid::create({0,0,0});
  centered->setName("field"); centered->setIsInWorldSpace(true);
  centered->setTransform(openvdb::math::Transform::createLinearTransform(1e6));
  centered->getAccessor().setValue(openvdb::Coord(0), {0,0,0});
  centered->getAccessor().setValue(openvdb::Coord(1,0,0), {1e12,0,0});
  write("centered.vdb", {centered});
  nearVector(vdb::SampleVector(file("centered.vdb"), {.5,0,0}), {500000,0,0}, 1e-8);
}
#endif
} // namespace
