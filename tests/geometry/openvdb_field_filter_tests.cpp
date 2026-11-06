#include "Container/geometry/OpenVdbFieldFilter.h"

#include <atomic>
#include <bit>
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
using Operation = vdb::FieldFilterOperation;
TEST(OpenVdbFieldFilterAvailability, DisabledPrecedesPathsOptionsAndOperation) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FieldFilterOptions filter;
  filter.width = 0;
  filter.mask = "missing-mask.vdb";
  try {
    (void)vdb::WriteFilteredField("missing.vdb", {}, static_cast<Operation>(99),
                                  filter, {}, {.maxInputBytes = 0});
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFieldFilter : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-field-filter-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    processing.gridName = "filtered";
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
  auto path(const std::string &name) const { return directory / name; }
  std::string bytes(const std::string &name) const {
    std::ifstream stream(path(name), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  void write(const std::string &file, const openvdb::GridPtrVec &grids,
             bool statistics = true) {
    openvdb::io::File output(path(file).string());
    output.setCompression(openvdb::io::COMPRESS_ZIP);
    output.setGridStatsMetadataEnabled(statistics);
    output.write(grids);
  }
  template <class Grid>
  auto read(const std::string &file, const std::string &name = "filtered") {
    openvdb::io::File input(path(file).string());
    input.open(false);
    auto grid = openvdb::gridPtrCast<Grid>(input.readGrid(name));
    input.close();
    return grid;
  }
  template <class Grid> static auto field(typename Grid::ValueType value) {
    auto grid = Grid::create();
    grid->setName("field");
    grid->tree().setValue({0, 0, 0}, value);
    return grid;
  }
  auto scalar(double value = 27) { return field<openvdb::DoubleGrid>(value); }
  void failure(const std::string &input, Operation operation = Operation::Mean,
               const vdb::FieldFilterOptions &filter = {},
               const vdb::ProcessingOptions *limits = nullptr,
               const std::string &selection = {}) {
    const auto file = "failure-" + std::to_string(++serial) + ".vdb";
    EXPECT_THROW((void)vdb::WriteFilteredField(path(input), path(file),
                                               operation, filter, selection,
                                               limits ? *limits : processing),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(file)));
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      EXPECT_FALSE(entry.is_directory()) << entry.path();
  }
  template <class Grid>
  void nativeMean(const std::string &tag, typename Grid::ValueType impulse) {
    auto grid = field<Grid>(impulse);
    write(tag + ".vdb", {grid});
    const auto result =
        vdb::WriteFilteredField(path(tag + ".vdb"), path(tag + "-mean.vdb"),
                                Operation::Mean, {}, {}, processing);
    const auto actual = read<Grid>(tag + "-mean.vdb");
    EXPECT_TRUE(actual->tree().hasSameTopology(grid->tree()));
    EXPECT_EQ(actual->activeVoxelCount(), 1u);
    EXPECT_EQ(result.inputActiveVoxelCount, 1u);
    EXPECT_EQ(result.grid.gridName, "filtered");
    EXPECT_EQ(result.grid.activeVoxelCount, 1u);
    EXPECT_EQ(result.kernelPasses, 3u);
    EXPECT_FALSE(result.maskUsed);
    const double w = double(1.f / 3.f);
    const auto center = actual->tree().getValue({0, 0, 0});
    using Value = typename Grid::ValueType;
    if constexpr (openvdb::VecTraits<Value>::IsVec) {
      for (size_t c = 0; c < 3; ++c)
        EXPECT_NEAR(double(center[c]), double(impulse[c]) * w * w * w,
                    sizeof(typename Value::ValueType) == 4 ? 1e-5 : 1e-12);
    } else {
      EXPECT_NEAR(double(center), double(impulse) * w * w * w,
                  sizeof(Value) == 4 ? 1e-5 : 1e-12);
    }
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions processing;
  size_t serial{0};
};

TEST_F(OpenVdbFieldFilter,
       MeanRetainsAllFourNativeTypesAndFloatReciprocalWeights) {
  nativeMean<openvdb::FloatGrid>("float", 27.f);
  nativeMean<openvdb::DoubleGrid>("double", 27.);
  nativeMean<openvdb::Vec3SGrid>("vec3s", {27, 54, 81});
  nativeMean<openvdb::Vec3DGrid>("vec3d", {27, 54, 81});
  const auto value =
      read<openvdb::DoubleGrid>("double-mean.vdb")->tree().getValue({0, 0, 0});
  EXPECT_GT(
      value,
      1. + 1e-8); // Double storage deliberately retains upstream Float weights.
}

TEST_F(OpenVdbFieldFilter,
       GaussianIsFourBoxIterationsWithIndependentConvolutionCoefficient) {
  auto grid = openvdb::DoubleGrid::create();
  grid->setName("field");
  grid->fill(openvdb::CoordBBox({-4, -4, -4}, {4, 4, 4}), 0., true);
  grid->tree().setValue({0, 0, 0}, 1.);
  write("impulse.vdb", {grid});
  const auto result =
      vdb::WriteFilteredField(path("impulse.vdb"), path("gaussian.vdb"),
                              Operation::Gaussian, {}, {}, processing);
  const auto actual = read<openvdb::DoubleGrid>("gaussian.vdb");
  // The coefficient of x^0 in (x^-1+1+x)^4 is19, independently in each axis.
  const double expected = std::pow(19. / 81., 3);
  EXPECT_NEAR(actual->tree().getValue({0, 0, 0}), expected, 2e-8);
  EXPECT_GT(actual->tree().getValue({1, 0, 0}), 0);
  EXPECT_TRUE(actual->tree().hasSameTopology(grid->tree()));
  EXPECT_EQ(result.kernelPasses, 12u);
  EXPECT_EQ(result.grid.activeVoxelCount, 729u);
}

TEST_F(OpenVdbFieldFilter,
       MedianUsesInactiveNeighborsAndLexicographicVectorRank) {
  auto scalarGrid = scalar(100);
  auto vectorGrid = field<openvdb::Vec3DGrid>({0, 100, -100});
  int neighbor = 0, rank = 0;
  for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x, ++rank) {
        const openvdb::Coord c{x, y, z};
        if (c == openvdb::Coord(0, 0, 0))
          continue;
        scalarGrid->tree().setValueOff(c, double(neighbor++));
        vectorGrid->tree().setValueOff(c, {double(rank - 13), 0, 0});
      }
  write("scalar.vdb", {scalarGrid});
  write("vector.vdb", {vectorGrid});
  const auto result =
      vdb::WriteFilteredField(path("scalar.vdb"), path("scalar-median.vdb"),
                              Operation::Median, {}, {}, processing);
  EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("scalar-median.vdb")
                       ->tree()
                       .getValue({0, 0, 0}),
                   13.);
  (void)vdb::WriteFilteredField(path("vector.vdb"), path("vector-median.vdb"),
                                Operation::Median, {}, {}, processing);
  const auto actual = read<openvdb::Vec3DGrid>("vector-median.vdb");
  EXPECT_EQ(actual->tree().getValue({0, 0, 0}), openvdb::Vec3d(0, 100, -100));
  EXPECT_EQ(actual->activeVoxelCount(), 1u);
  EXPECT_TRUE(actual->tree().hasSameTopology(vectorGrid->tree()));
  EXPECT_EQ(result.kernelPasses, 1u);
}

TEST_F(OpenVdbFieldFilter,
       ScalarAndVectorOffsetsUseStoredUnitsAndRequestedIterations) {
  auto scalarGrid = scalar(1);
  auto vectorGrid = field<openvdb::Vec3DGrid>({1, 2, 3});
  vectorGrid->setVectorType(openvdb::VEC_CONTRAVARIANT_ABSOLUTE);
  vectorGrid->transform().postScale({-2, 3, .5});
  vectorGrid->transform().postTranslate({10, 20, 30});
  vectorGrid->insertMeta("artist",
                         openvdb::StringMetadata(std::string("a\0b", 3)));
  write("scalar.vdb", {scalarGrid});
  write("vector.vdb", {vectorGrid});
  vdb::FieldFilterOptions f;
  f.iterations = 3;
  f.offset = {.25, 0, 0};
  const auto result =
      vdb::WriteFilteredField(path("scalar.vdb"), path("scalar-offset.vdb"),
                              Operation::Offset, f, {}, processing);
  EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("scalar-offset.vdb")
                       ->tree()
                       .getValue({0, 0, 0}),
                   1.75);
  EXPECT_EQ(result.kernelPasses, 3u);
  f.offset = {.25, -.5, 1};
  (void)vdb::WriteFilteredField(path("vector.vdb"), path("vector-offset.vdb"),
                                Operation::Offset, f, {}, processing);
  const auto actual = read<openvdb::Vec3DGrid>("vector-offset.vdb");
  EXPECT_EQ(actual->tree().getValue({0, 0, 0}), openvdb::Vec3d(1.75, .5, 6));
  EXPECT_EQ(actual->transform(), vectorGrid->transform());
  EXPECT_EQ(actual->getVectorType(), openvdb::VEC_CONTRAVARIANT_ABSOLUTE);
  EXPECT_TRUE(actual->isInWorldSpace());
  EXPECT_EQ(actual->metaValue<std::string>("artist"), std::string("a\0b", 3));
}

TEST_F(OpenVdbFieldFilter,
       MaskSmoothStepUsesBackgroundInactiveValuesAndInvert) {
  auto grid = scalar(10);
  grid->tree().setValue({3, 0, 0}, 20);
  grid->tree().setValue({6, 0, 0}, 30);
  auto mask = openvdb::DoubleGrid::create(-1);
  mask->setName("mask");
  mask->tree().setValueOff({3, 0, 0}, .25);
  mask->tree().setValue({6, 0, 0}, 2);
  write("field.vdb", {grid});
  write("mask.vdb", {mask});
  const auto original = bytes("mask.vdb");
  vdb::FieldFilterOptions f;
  f.mask = path("mask.vdb");
  f.offset = {4, 0, 0};
  const auto result =
      vdb::WriteFilteredField(path("field.vdb"), path("mask-out.vdb"),
                              Operation::Offset, f, {}, processing);
  const auto actual = read<openvdb::DoubleGrid>("mask-out.vdb");
  EXPECT_DOUBLE_EQ(actual->tree().getValue({0, 0, 0}), 10);
  EXPECT_DOUBLE_EQ(actual->tree().getValue({3, 0, 0}), 20.625);
  EXPECT_DOUBLE_EQ(actual->tree().getValue({6, 0, 0}), 34);
  EXPECT_TRUE(result.maskUsed);
  f.invertMask = true;
  (void)vdb::WriteFilteredField(path("field.vdb"), path("inverse.vdb"),
                                Operation::Offset, f, {}, processing);
  const auto inverse = read<openvdb::DoubleGrid>("inverse.vdb");
  EXPECT_DOUBLE_EQ(inverse->tree().getValue({0, 0, 0}), 14);
  EXPECT_DOUBLE_EQ(inverse->tree().getValue({3, 0, 0}), 23.375);
  EXPECT_DOUBLE_EQ(inverse->tree().getValue({6, 0, 0}), 30);
  EXPECT_EQ(bytes("mask.vdb"), original);
}

TEST_F(OpenVdbFieldFilter,
       SeparableMeanBlendsAtEachAxisRatherThanOnlyAfterConvolution) {
  auto grid = scalar(27);
  auto mask = openvdb::FloatGrid::create(.5f);
  mask->setName("mask");
  write("field.vdb", {grid});
  write("mask.vdb", {mask});
  vdb::FieldFilterOptions f;
  f.mask = path("mask.vdb");
  (void)vdb::WriteFilteredField(path("field.vdb"), path("blend.vdb"),
                                Operation::Mean, f, {}, processing);
  const double axis = .5 + .5 * double(1.f / 3.f);
  const double actual =
      read<openvdb::DoubleGrid>("blend.vdb")->tree().getValue({0, 0, 0});
  EXPECT_NEAR(actual, 27 * axis * axis * axis, 1e-12);
  EXPECT_NEAR(actual, 8., 1e-6);
}

TEST_F(OpenVdbFieldFilter,
       MaskNormalizationIsSafeAndRequiresExactAffineAndResolvableWeights) {
  auto grid = scalar(1);
  write("field.vdb", {grid});
  auto mask = openvdb::DoubleGrid::create(0);
  mask->setName("mask");
  write("mask.vdb", {mask});
  vdb::FieldFilterOptions f;
  f.mask = path("mask.vdb");
  f.offset = {2, 0, 0};
  f.maskMinimum = -std::numeric_limits<double>::max();
  f.maskMaximum = std::numeric_limits<double>::max();
  (void)vdb::WriteFilteredField(path("field.vdb"), path("wide-range.vdb"),
                                Operation::Offset, f, {}, processing);
  EXPECT_DOUBLE_EQ(
      read<openvdb::DoubleGrid>("wide-range.vdb")->tree().getValue({0, 0, 0}),
      2);
  f.maskMinimum = 0;
  f.maskMaximum = 1;
  for (double value : {1e-300, 1. - 1e-12}) {
    mask = openvdb::DoubleGrid::create(value);
    mask->setName("mask");
    write("mask.vdb", {mask});
    failure("field.vdb", Operation::Offset, f);
  }
  mask = openvdb::DoubleGrid::create(.5);
  mask->setName("mask");
  mask->transform().postTranslate({1e-9, 0, 0});
  write("mask.vdb", {mask});
  failure("field.vdb", Operation::Offset, f);
}

TEST_F(OpenVdbFieldFilter,
       ActiveTilesRemainFixedByDefaultAndOnlyExplicitVoxelizationFiltersThem) {
  auto grid = openvdb::DoubleGrid::create();
  grid->setName("field");
  grid->tree().addTile(1, {0, 0, 0}, 27., true);
  grid->tree().setValue({8, 0, 0}, 0.);
  write("tiles.vdb", {grid});
  const auto result =
      vdb::WriteFilteredField(path("tiles.vdb"), path("tile-mean.vdb"),
                              Operation::Mean, {}, {}, processing);
  const auto mean = read<openvdb::DoubleGrid>("tile-mean.vdb");
  EXPECT_TRUE(mean->tree().hasSameTopology(grid->tree()));
  EXPECT_DOUBLE_EQ(mean->tree().getValue({0, 0, 0}), 27);
  EXPECT_NEAR(mean->tree().getValue({8, 0, 0}), 1., 1e-6);
  EXPECT_EQ(result.activeTileCount, 1u);
  EXPECT_EQ(result.voxelizedTileCount, 0u);
  EXPECT_EQ(result.inputActiveVoxelCount, 513u);
  vdb::FieldFilterOptions f;
  f.offset = {2, 0, 0};
  f.voxelizeActiveTiles = true;
  const auto expanded =
      vdb::WriteFilteredField(path("tiles.vdb"), path("expanded.vdb"),
                              Operation::Offset, f, {}, processing);
  const auto actual = read<openvdb::DoubleGrid>("expanded.vdb");
  EXPECT_FALSE(actual->tree().hasActiveTiles());
  EXPECT_DOUBLE_EQ(actual->tree().getValue({0, 0, 0}), 29);
  EXPECT_DOUBLE_EQ(actual->tree().getValue({8, 0, 0}), 2);
  EXPECT_EQ(actual->activeVoxelCount(), 513u);
  EXPECT_EQ(expanded.voxelizedTileCount, 1u);
  EXPECT_EQ(expanded.storedLeafCount, 2u);
}

TEST_F(OpenVdbFieldFilter,
       InactiveBitsBackgroundMetadataAndInputsSurviveFilteringAndFailure) {
  auto grid = openvdb::DoubleGrid::create(+0.);
  grid->setName("field");
  grid->tree().setValue({0, 0, 0}, 0.);
  auto *leaf = grid->tree().touchLeaf({16, 0, 0});
  leaf->setValueOff({16, 0, 0}, -0.);
  leaf->setValueOff({17, 0, 0}, +0.);
  grid->tree().setValueOff({1, 0, 0}, 27.);
  grid->insertMeta("artist",
                   openvdb::StringMetadata(std::string("raw\0bytes", 9)));
  write("source.vdb", {grid});
  const auto original = bytes("source.vdb");
  (void)vdb::WriteFilteredField(path("source.vdb"), path("preserved.vdb"),
                                Operation::Mean, {}, {}, processing);
  const auto actual = read<openvdb::DoubleGrid>("preserved.vdb");
  EXPECT_TRUE(actual->tree().hasSameTopology(grid->tree()));
  EXPECT_NEAR(actual->tree().getValue({0, 0, 0}), 1., 1e-6);
  EXPECT_DOUBLE_EQ(actual->tree().getValue({1, 0, 0}), 27.);
  EXPECT_EQ(std::bit_cast<uint64_t>(actual->tree().getValue({16, 0, 0})),
            std::bit_cast<uint64_t>(-0.));
  EXPECT_EQ(std::bit_cast<uint64_t>(actual->tree().getValue({17, 0, 0})),
            std::bit_cast<uint64_t>(+0.));
  EXPECT_EQ(std::bit_cast<uint64_t>(actual->background()),
            std::bit_cast<uint64_t>(+0.));
  EXPECT_EQ(actual->metaValue<std::string>("artist"),
            std::string("raw\0bytes", 9));
  EXPECT_EQ(bytes("source.vdb"), original);
  const auto output = bytes("preserved.vdb");
  EXPECT_THROW((void)vdb::WriteFilteredField(
                   path("source.vdb"), path("preserved.vdb"), Operation::Mean),
               std::exception);
  EXPECT_THROW((void)vdb::WriteFilteredField(
                   path("source.vdb"), path("source.vdb"), Operation::Mean),
               std::exception);
  EXPECT_EQ(bytes("source.vdb"), original);
  EXPECT_EQ(bytes("preserved.vdb"), output);
}

TEST_F(OpenVdbFieldFilter,
       ExactDescriptorSelectorsResolveDuplicatesAndLiteralPrefixes) {
  auto first = scalar(1), second = scalar(2);
  write("duplicate.vdb", {first, second});
  failure("duplicate.vdb", Operation::Offset, {}, nullptr, "field");
  vdb::FieldFilterOptions f;
  f.offset = {1, 0, 0};
  (void)vdb::WriteFilteredField(path("duplicate.vdb"), path("chosen.vdb"),
                                Operation::Offset, f, "descriptor:field[1]",
                                processing);
  EXPECT_DOUBLE_EQ(
      read<openvdb::DoubleGrid>("chosen.vdb")->tree().getValue({0, 0, 0}), 3.);
  auto literal = scalar(7);
  literal->setName("descriptor:field");
  write("literal.vdb", {literal});
  (void)vdb::WriteFilteredField(path("literal.vdb"), path("literal-out.vdb"),
                                Operation::Offset, f,
                                "descriptor:descriptor:field", processing);
  EXPECT_DOUBLE_EQ(
      read<openvdb::DoubleGrid>("literal-out.vdb")->tree().getValue({0, 0, 0}),
      8.);
  auto collision = scalar();
  collision->setName("field[0]");
  write("collision.vdb", {first, second, collision});
  failure("collision.vdb");
}

TEST_F(OpenVdbFieldFilter,
       FogAndUnclearClassOrVectorSemanticsRejectWithoutClamping) {
  auto grid = scalar(1);
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  write("fog.vdb", {grid});
  vdb::FieldFilterOptions f;
  f.offset = {-2, 0, 0};
  failure("fog.vdb", Operation::Offset, f);
  for (const auto cls : {openvdb::GRID_LEVEL_SET, openvdb::GRID_STAGGERED}) {
    grid->setGridClass(cls);
    write("class.vdb", {grid});
    failure("class.vdb");
  }
  grid->setGridClass(openvdb::GRID_UNKNOWN);
  grid->setVectorType(openvdb::VEC_COVARIANT);
  write("scalar-vector.vdb", {grid});
  failure("scalar-vector.vdb");
  auto vector = field<openvdb::Vec3SGrid>({1, 2, 3});
  vector->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);
  write("normalize.vdb", {vector});
  failure("normalize.vdb");
  vector->setVectorType(openvdb::VEC_INVARIANT);
  vector->setIsInWorldSpace(false);
  write("index-space.vdb", {vector});
  failure("index-space.vdb");
}

TEST_F(OpenVdbFieldFilter,
       MalformedStructuralTagsCannotFallBackToUnknownOrInvariant) {
  for (unsigned mode = 0; mode < 6; ++mode) {
    auto grid = scalar(1);
    const char *key = mode < 2   ? openvdb::GridBase::META_GRID_CLASS
                      : mode < 4 ? openvdb::GridBase::META_VECTOR_TYPE
                                 : openvdb::GridBase::META_IS_LOCAL_SPACE;
    grid->removeMeta(key);
    if (mode == 0)
      grid->insertMeta(key, openvdb::StringMetadata("not-a-grid-class"));
    if (mode == 1)
      grid->insertMeta(key, openvdb::Int32Metadata(3));
    if (mode == 2)
      grid->insertMeta(key, openvdb::StringMetadata("not-a-vector-semantic"));
    if (mode == 3)
      grid->insertMeta(key, openvdb::BoolMetadata(false));
    if (mode == 4)
      grid->insertMeta(key, openvdb::StringMetadata("false"));
    if (mode == 5)
      grid->insertMeta(key, openvdb::Int32Metadata(0));
    write("structural.vdb", {grid});
    failure("structural.vdb");
  }
  auto recognized = scalar(1);
  recognized->insertMeta(openvdb::GridBase::META_GRID_CLASS,
                         openvdb::StringMetadata(" \tUNKNOWN \n"));
  recognized->insertMeta(openvdb::GridBase::META_VECTOR_TYPE,
                         openvdb::StringMetadata(" INVARIANT "));
  recognized->insertMeta(openvdb::GridBase::META_IS_LOCAL_SPACE,
                         openvdb::BoolMetadata(false));
  write("recognized.vdb", {recognized});
  vdb::FieldFilterOptions f;
  f.offset = {1, 0, 0};
  EXPECT_NO_THROW((void)vdb::WriteFilteredField(
      path("recognized.vdb"), path("recognized-out.vdb"), Operation::Offset, f,
      {}, processing));
  EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("recognized-out.vdb")
                       ->tree()
                       .getValue({0, 0, 0}),
                   2.);
}

TEST_F(OpenVdbFieldFilter,
       NumericCoordinateAndWorkMemoryExpansionLimitsArePreflighted) {
  auto grid = scalar();
  write("field.vdb", {grid});
  for (unsigned mode = 0; mode < 6; ++mode) {
    vdb::FieldFilterOptions f;
    if (mode == 0)
      f.width = 5;
    if (mode == 1)
      f.iterations = 33;
    if (mode == 2)
      f.offset[0] = std::numeric_limits<double>::quiet_NaN();
    if (mode == 3)
      f.offset[1] = 1;
    if (mode == 4)
      f.maxWorkItems = 0;
    if (mode == 5)
      f.maxWorkItems = 1;
    failure("field.vdb",
            (mode == 2 || mode == 3) ? Operation::Offset : Operation::Mean, f);
  }
  auto limits = processing;
  limits.maxInputBytes = 1;
  failure("field.vdb", Operation::Mean, {}, &limits);
  auto wide = openvdb::DoubleGrid::create();
  wide->setName("field");
  wide->tree().addTile(1, {0, 0, 0}, 1., true);
  wide->tree().touchLeaf({1'000'000, 0, 0});
  write("wide.vdb", {wide});
  limits = processing;
  limits.maxVoxelCount = 512;
  vdb::FieldFilterOptions f;
  f.voxelizeActiveTiles = true;
  f.offset = {1, 0, 0};
  failure("wide.vdb", Operation::Offset, f, &limits);
  limits.maxVoxelCount = 1024;
  EXPECT_NO_THROW((void)vdb::WriteFilteredField(
      path("wide.vdb"), path("wide-ok.vdb"), Operation::Offset, f, {}, limits));
  auto edge = openvdb::DoubleGrid::create();
  edge->setName("field");
  edge->tree().setValue({INT32_MAX, 0, 0}, 1.);
  write("edge.vdb", {edge});
  failure("edge.vdb");
  auto bad = scalar(1);
  bad->tree().setValueOff({16, 0, 0}, std::numeric_limits<double>::infinity());
  write("nonfinite.vdb", {bad});
  failure("nonfinite.vdb");
  auto huge = scalar(1e308);
  write("huge.vdb", {huge});
  f = {};
  f.width = 4;
  failure("huge.vdb", Operation::Mean, f);
  auto small = field<openvdb::FloatGrid>(1);
  write("float.vdb", {small});
  f = {};
  f.offset[0] = std::numeric_limits<double>::denorm_min();
  failure("float.vdb", Operation::Offset, f);
}

TEST_F(OpenVdbFieldFilter,
       UnsupportedTypesAndMissingStatisticsOrStreamingFilesReject) {
  auto boolean = openvdb::BoolGrid::create();
  boolean->setName("field");
  boolean->tree().setValueOn({0, 0, 0});
  write("bool.vdb", {boolean});
  failure("bool.vdb");
  auto grid = scalar();
  write("no-stats.vdb", {grid}, false);
  failure("no-stats.vdb");
  {
    std::ofstream file(path("stream.vdb"), std::ios::binary);
    openvdb::io::Stream stream(file);
    stream.write(openvdb::GridPtrVec{grid});
  }
  failure("stream.vdb");
  write("field.vdb", {grid});
  failure("field.vdb", static_cast<Operation>(99));
}
#endif
} // namespace
