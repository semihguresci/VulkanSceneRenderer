#include "Container/geometry/OpenVdbFieldComposite.h"

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
using Binary = vdb::FieldCompositeOperation;
using Unary = vdb::FieldValueOperation;

TEST(OpenVdbFieldCompositeAvailability,
     DisabledPrecedesPathsAndInvalidOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FieldCompositeOptions binary;
  binary.maxWorkItems = 0;
  vdb::FieldValueTransformOptions unary;
  unary.operation = static_cast<Unary>(99);
  for (bool composite : {false, true}) {
    try {
      if (composite)
        (void)vdb::WriteCompositeField("missing-a.vdb", "missing-b.vdb", {},
                                       static_cast<Binary>(99), binary);
      else
        (void)vdb::WriteTransformedField("missing.vdb", {}, unary);
      FAIL();
    } catch (const std::runtime_error &e) {
      EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
                std::string::npos);
    }
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
template <class T> void sameBits(const T &actual, const T &expected) {
  if constexpr (openvdb::VecTraits<T>::IsVec) {
    for (size_t axis = 0; axis < 3; ++axis)
      sameBits(actual[axis], expected[axis]);
  } else if constexpr (sizeof(T) == sizeof(uint32_t)) {
    EXPECT_EQ(std::bit_cast<uint32_t>(actual),
              std::bit_cast<uint32_t>(expected));
  } else {
    EXPECT_EQ(std::bit_cast<uint64_t>(actual),
              std::bit_cast<uint64_t>(expected));
  }
}

class OpenVdbFieldComposite : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t next{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-field-composite-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++next));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    processing.gridName = "composite";
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
  auto path(const std::string &file) const { return directory / file; }
  std::string bytes(const std::string &file) const {
    std::ifstream input(path(file), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  void write(const std::string &file, const openvdb::GridPtrVec &grids,
             bool statistics = true) {
    openvdb::io::File output(path(file).string());
    output.setCompression(openvdb::io::COMPRESS_ZIP);
    output.setGridStatsMetadataEnabled(statistics);
    output.write(grids);
  }
  template <class Grid>
  auto read(const std::string &file, const std::string &name = "composite") {
    openvdb::io::File input(path(file).string());
    input.open(false);
    auto result = openvdb::gridPtrCast<Grid>(input.readGrid(name));
    input.close();
    return result;
  }
  template <class Grid>
  static auto field(typename Grid::ValueType background = {}) {
    auto result = Grid::create(background);
    result->setName("field");
    return result;
  }
  template <class Grid>
  static void off(Grid &grid, const openvdb::Coord &coord,
                  typename Grid::ValueType value) {
    // Force storage even when native equality regards -0 as the +0 background.
    grid.tree().touchLeaf(coord)->setValueOff(coord, value);
  }
  auto scalar(double value = 1) {
    auto result = field<openvdb::DoubleGrid>();
    result->tree().setValue({0, 0, 0}, value);
    return result;
  }
  void noTemporaryDirectories() {
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      EXPECT_FALSE(entry.is_directory()) << entry.path();
  }
  void binaryFailure(const std::string &a, const std::string &b,
                     Binary operation = Binary::Sum,
                     const vdb::FieldCompositeOptions &options = {},
                     const vdb::ProcessingOptions *limits = nullptr,
                     const std::string &selectA = {},
                     const std::string &selectB = {}) {
    const auto output = "failure-" + std::to_string(++serial) + ".vdb";
    EXPECT_THROW((void)vdb::WriteCompositeField(
                     path(a), path(b), path(output), operation, options,
                     selectA, selectB, limits ? *limits : processing),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(output)));
    noTemporaryDirectories();
  }
  void unaryFailure(const std::string &input,
                    const vdb::FieldValueTransformOptions &options = {},
                    const vdb::ProcessingOptions *limits = nullptr,
                    const std::string &selector = {}) {
    const auto output = "failure-" + std::to_string(++serial) + ".vdb";
    EXPECT_THROW((void)vdb::WriteTransformedField(
                     path(input), path(output), options, selector,
                     limits ? *limits : processing),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(output)));
    noTemporaryDirectories();
  }
  template <class Grid>
  void nativeArithmetic(const std::string &tag, typename Grid::ValueType av,
                        typename Grid::ValueType bv) {
    using Value = typename Grid::ValueType;
    auto a = field<Grid>(Value(2)), b = field<Grid>(Value(3));
    a->tree().setValue({0, 0, 0}, av);
    b->tree().setValue({0, 0, 0}, bv);
    off(*a, {16, 0, 0}, Value(7));
    off(*b, {16, 0, 0}, Value(11));
    write(tag + "-a.vdb", {a});
    write(tag + "-b.vdb", {b});
    for (const auto operation : {Binary::Sum, Binary::Product}) {
      const auto output =
          tag + (operation == Binary::Sum ? "-sum.vdb" : "-product.vdb");
      const auto result = vdb::WriteCompositeField(
          path(tag + "-a.vdb"), path(tag + "-b.vdb"), path(output), operation,
          {}, {}, {}, processing);
      const auto actual = read<Grid>(output);
      ASSERT_TRUE(actual);
      sameBits(actual->background(), Value(operation == Binary::Sum ? 5 : 6));
      sameBits(actual->tree().getValue({16, 0, 0}),
               Value(operation == Binary::Sum ? 18 : 77));
      if constexpr (openvdb::VecTraits<Value>::IsVec) {
        const auto value = actual->tree().getValue({0, 0, 0});
        for (size_t axis = 0; axis < 3; ++axis)
          EXPECT_EQ(value[axis], operation == Binary::Sum
                                     ? av[axis] + bv[axis]
                                     : av[axis] * bv[axis]);
      } else {
        EXPECT_EQ(actual->tree().getValue({0, 0, 0}),
                  operation == Binary::Sum ? av + bv : av * bv);
      }
      EXPECT_FALSE(actual->tree().isValueOn({16, 0, 0}));
      EXPECT_EQ(result.inputActiveVoxelCountA, 1u);
      EXPECT_EQ(result.inputActiveVoxelCountB, 1u);
      EXPECT_EQ(result.storedLeafCount, 2u);
      EXPECT_EQ(result.grid.gridName, processing.gridName);
      EXPECT_EQ(result.grid.activeVoxelCount, 1u);
      EXPECT_GT(result.workItems, 0u);
    }
  }
  template <class Grid>
  void nativeDivision(const std::string &tag, typename Grid::ValueType av,
                      typename Grid::ValueType bv) {
    using Value = typename Grid::ValueType;
    auto a = field<Grid>(Value(12)), b = field<Grid>(Value(3));
    a->tree().setValue({0, 0, 0}, av);
    b->tree().setValue({0, 0, 0}, bv);
    a->tree().setValue({1, 0, 0}, Value(-0.));
    b->tree().setValue({1, 0, 0}, Value(-2));
    a->tree().setValue({2, 0, 0}, Value(-0.));
    b->tree().setValue({2, 0, 0}, Value(2));
    off(*a, {16, 0, 0}, Value(15));
    off(*b, {32, 0, 0}, Value(6));
    a->insertMeta("artist", openvdb::StringMetadata("division source"));
    write(tag + "-a.vdb", {a});
    write(tag + "-b.vdb", {b});
    const auto originalA = bytes(tag + "-a.vdb");
    const auto originalB = bytes(tag + "-b.vdb");
    const auto result = vdb::WriteCompositeField(
        path(tag + "-a.vdb"), path(tag + "-b.vdb"), path(tag + "-quotient.vdb"),
        Binary::Divide, {}, {}, {}, processing);
    const auto actual = read<Grid>(tag + "-quotient.vdb");
    ASSERT_TRUE(actual);
    sameBits(actual->background(), Value(4));
    sameBits(actual->tree().getValue({16, 0, 0}), Value(5));
    sameBits(actual->tree().getValue({32, 0, 0}), Value(2));
    sameBits(actual->tree().getValue({1, 0, 0}), Value(+0.));
    sameBits(actual->tree().getValue({2, 0, 0}), Value(-0.));
    if constexpr (openvdb::VecTraits<Value>::IsVec) {
      const auto value = actual->tree().getValue({0, 0, 0});
      for (size_t axis = 0; axis < 3; ++axis)
        EXPECT_EQ(value[axis], av[axis] / bv[axis]);
    } else {
      EXPECT_EQ(actual->tree().getValue({0, 0, 0}), av / bv);
    }
    EXPECT_FALSE(actual->tree().isValueOn({16, 0, 0}));
    EXPECT_FALSE(actual->tree().isValueOn({32, 0, 0}));
    EXPECT_EQ(actual->tree().leafCount(), 3u);
    EXPECT_EQ(result.storedLeafCount, 3u);
    EXPECT_EQ(result.inputActiveVoxelCountA, 3u);
    EXPECT_EQ(result.inputActiveVoxelCountB, 3u);
    EXPECT_EQ(result.grid.activeVoxelCount, 3u);
    EXPECT_EQ(actual->template metaValue<std::string>("artist"),
              "division source");
    EXPECT_EQ(bytes(tag + "-a.vdb"), originalA);
    EXPECT_EQ(bytes(tag + "-b.vdb"), originalB);
  }
  template <class Grid>
  void nativeCopy(const std::string &tag, typename Grid::ValueType background,
                  typename Grid::ValueType active,
                  typename Grid::ValueType inactive) {
    auto input = field<Grid>(background);
    input->tree().setValue({0, 0, 0}, active);
    off(*input, {32, 0, 0}, inactive);
    input->insertMeta("artist", openvdb::StringMetadata("native bits"));
    write(tag + ".vdb", {input});
    const auto original = bytes(tag + ".vdb");
    const auto result = vdb::WriteTransformedField(
        path(tag + ".vdb"), path(tag + "-copy.vdb"), {}, {}, processing);
    const auto actual = read<Grid>(tag + "-copy.vdb");
    ASSERT_TRUE(actual);
    sameBits(actual->background(), background);
    sameBits(actual->tree().getValue({0, 0, 0}), active);
    sameBits(actual->tree().getValue({32, 0, 0}), inactive);
    EXPECT_TRUE(actual->tree().hasSameTopology(input->tree()));
    EXPECT_EQ(actual->template metaValue<std::string>("artist"), "native bits");
    EXPECT_EQ(result.storedLeafCount, 2u);
    EXPECT_EQ(result.inputActiveVoxelCountA, 1u);
    EXPECT_EQ(result.inputActiveVoxelCountB, 0u);
    EXPECT_EQ(bytes(tag + ".vdb"), original);
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions processing;
  size_t serial{0};
};

TEST_F(OpenVdbFieldComposite, SumAndProductRetainAllFourNativeTypes) {
  nativeArithmetic<openvdb::FloatGrid>("float", 4.f, 5.f);
  nativeArithmetic<openvdb::DoubleGrid>("double", 4., 5.);
  nativeArithmetic<openvdb::Vec3SGrid>("vec3s", {4, 6, 8}, {5, 7, 9});
  nativeArithmetic<openvdb::Vec3DGrid>("vec3d", {4, 6, 8}, {5, 7, 9});
}

TEST_F(OpenVdbFieldComposite,
       DivisionRetainsNativeComponentwiseValuesAndSparseInactiveLeaves) {
  nativeDivision<openvdb::FloatGrid>("float-divide", 12.f, -3.f);
  nativeDivision<openvdb::DoubleGrid>("double-divide", 12., -3.);
  nativeDivision<openvdb::Vec3SGrid>("vec3s-divide", {12, -18, 28}, {3, 6, -7});
  nativeDivision<openvdb::Vec3DGrid>("vec3d-divide", {12, -18, 28}, {3, 6, -7});
}

TEST_F(OpenVdbFieldComposite,
       DivisionActivityPoliciesDoNotHideInactiveOrBackgroundQuotients) {
  auto a = field<openvdb::DoubleGrid>(12), b = field<openvdb::DoubleGrid>(2);
  a->tree().setValue({0, 0, 0}, 12);
  off(*b, {0, 0, 0}, 3);
  off(*a, {1, 0, 0}, -8);
  b->tree().setValue({1, 0, 0}, 4);
  off(*b, {32, 0, 0}, 6);
  write("a.vdb", {a});
  write("b.vdb", {b});
  const std::array policies{
      vdb::FieldActivity::Union, vdb::FieldActivity::Intersection,
      vdb::FieldActivity::Left, vdb::FieldActivity::Right};
  const std::array<std::array<bool, 2>, 4> flags{
      {{true, true}, {false, false}, {true, false}, {false, true}}};
  for (size_t p = 0; p < policies.size(); ++p) {
    vdb::FieldCompositeOptions options;
    options.activity = policies[p];
    const auto output = "divide-policy-" + std::to_string(p) + ".vdb";
    (void)vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"), path(output),
                                   Binary::Divide, options, {}, {}, processing);
    const auto actual = read<openvdb::DoubleGrid>(output);
    EXPECT_EQ(actual->background(), 6);
    EXPECT_EQ(actual->tree().getValue({0, 0, 0}), 4);
    EXPECT_EQ(actual->tree().getValue({1, 0, 0}), -2);
    EXPECT_EQ(actual->tree().getValue({32, 0, 0}), 2);
    EXPECT_EQ(actual->tree().isValueOn({0, 0, 0}), flags[p][0]);
    EXPECT_EQ(actual->tree().isValueOn({1, 0, 0}), flags[p][1]);
    EXPECT_FALSE(actual->tree().isValueOn({32, 0, 0}));
    EXPECT_EQ(actual->tree().leafCount(), 2u);
  }
}

TEST_F(OpenVdbFieldComposite,
       DivisionRejectsEveryZeroDenominatorIncludingInactiveAndZeroOverZero) {
  auto fail = [&](const openvdb::GridBase::Ptr &a,
                  const openvdb::GridBase::Ptr &b,
                  const vdb::FieldCompositeOptions &options =
                      vdb::FieldCompositeOptions{}) {
    const auto tag = "zero-denominator-" + std::to_string(++serial);
    write(tag + "-a.vdb", {a});
    write(tag + "-b.vdb", {b});
    const auto originalA = bytes(tag + "-a.vdb"),
               originalB = bytes(tag + "-b.vdb");
    binaryFailure(tag + "-a.vdb", tag + "-b.vdb", Binary::Divide, options);
    EXPECT_EQ(bytes(tag + "-a.vdb"), originalA);
    EXPECT_EQ(bytes(tag + "-b.vdb"), originalB);
  };
  auto a = field<openvdb::DoubleGrid>(12);
  a->tree().setValue({0, 0, 0}, 12);
  auto b = field<openvdb::DoubleGrid>();
  b->tree().setValue({0, 0, 0}, 3);
  fail(a,
       b); // The ordinary zero background is still part of the logical field.
  b = field<openvdb::DoubleGrid>(2);
  b->tree().setValue({0, 0, 0}, 0.);
  fail(a, b);
  b = field<openvdb::DoubleGrid>(2);
  off(*b, {32, 0, 0}, -0.);
  vdb::FieldCompositeOptions intersection;
  intersection.activity = vdb::FieldActivity::Intersection;
  fail(a, b,
       intersection); // No active intersection can conceal this remote zero.
  auto zero = field<openvdb::DoubleGrid>();
  zero->tree().setValue({0, 0, 0}, 0.);
  b = field<openvdb::DoubleGrid>(2);
  b->tree().setValue({0, 0, 0}, 0.);
  fail(zero, b); // There is no 0/0 replacement convention.
  auto va = field<openvdb::Vec3DGrid>(openvdb::Vec3d(12));
  va->tree().setValue({0, 0, 0}, {12, 18, 24});
  auto vb = field<openvdb::Vec3DGrid>(openvdb::Vec3d(2));
  vb->tree().setValue({0, 0, 0}, {2, 0, 4});
  fail(va, vb);
  vb = field<openvdb::Vec3DGrid>(openvdb::Vec3d(2));
  off(*vb, {32, 0, 0}, {2, 4, -0.});
  fail(va, vb, intersection);
  vb = field<openvdb::Vec3DGrid>(openvdb::Vec3d(2, 0, 4));
  vb->tree().setValue({0, 0, 0}, {2, 3, 4});
  fail(va, vb);
}

TEST_F(OpenVdbFieldComposite,
       DivisionRejectsNativeRangeLossButPreservesRepresentableExtremeRatios) {
  auto check = [&]<class Grid>(const std::string &tag) {
    using Value = typename Grid::ValueType;
    const Value maximum = std::numeric_limits<Value>::max();
    const Value tiny = std::numeric_limits<Value>::denorm_min();
    auto a = field<Grid>(Value(0)), b = field<Grid>(Value(1));
    a->tree().setValue({0, 0, 0}, maximum);
    b->tree().setValue({0, 0, 0}, Value(.5));
    write(tag + "-overflow-a.vdb", {a});
    write(tag + "-overflow-b.vdb", {b});
    const auto originalA = bytes(tag + "-overflow-a.vdb");
    const auto originalB = bytes(tag + "-overflow-b.vdb");
    binaryFailure(tag + "-overflow-a.vdb", tag + "-overflow-b.vdb",
                  Binary::Divide);
    EXPECT_EQ(bytes(tag + "-overflow-a.vdb"), originalA);
    EXPECT_EQ(bytes(tag + "-overflow-b.vdb"), originalB);
    a->tree().setValue({0, 0, 0}, tiny);
    b->tree().setValue({0, 0, 0}, Value(2));
    write(tag + "-underflow-a.vdb", {a});
    write(tag + "-underflow-b.vdb", {b});
    binaryFailure(tag + "-underflow-a.vdb", tag + "-underflow-b.vdb",
                  Binary::Divide);
    a->tree().setValue({0, 0, 0}, tiny);
    b->tree().setValue({0, 0, 0}, tiny);
    a->tree().setValue({1, 0, 0}, maximum);
    b->tree().setValue({1, 0, 0}, maximum);
    write(tag + "-safe-a.vdb", {a});
    write(tag + "-safe-b.vdb", {b});
    (void)vdb::WriteCompositeField(
        path(tag + "-safe-a.vdb"), path(tag + "-safe-b.vdb"),
        path(tag + "-safe.vdb"), Binary::Divide, {}, {}, {}, processing);
    const auto actual = read<Grid>(tag + "-safe.vdb");
    sameBits(actual->tree().getValue({0, 0, 0}), Value(1));
    sameBits(actual->tree().getValue({1, 0, 0}), Value(1));
  };
  check.template operator()<openvdb::FloatGrid>("float");
  check.template operator()<openvdb::DoubleGrid>("double");
}

TEST_F(OpenVdbFieldComposite, ActivityPoliciesNeverMaskArithmeticValues) {
  auto a = field<openvdb::DoubleGrid>(2), b = field<openvdb::DoubleGrid>(3);
  a->tree().setValue({0, 0, 0}, 10);
  off(*b, {0, 0, 0}, 20);
  a->tree().setValue({1, 0, 0}, 4);
  b->tree().setValue({1, 0, 0}, 5);
  off(*a, {2, 0, 0}, 6);
  b->tree().setValue({2, 0, 0}, 7);
  off(*a, {3, 0, 0}, 8);
  off(*b, {3, 0, 0}, 9);
  off(*b, {32, 0, 0}, 11);
  write("a.vdb", {a});
  write("b.vdb", {b});
  const std::array policies{
      vdb::FieldActivity::Union, vdb::FieldActivity::Intersection,
      vdb::FieldActivity::Left, vdb::FieldActivity::Right};
  const std::array<std::array<bool, 3>, 4> flags{{{true, true, true},
                                                  {false, true, false},
                                                  {true, true, false},
                                                  {false, true, true}}};
  for (size_t p = 0; p < policies.size(); ++p) {
    vdb::FieldCompositeOptions options;
    options.activity = policies[p];
    const auto output = "policy-" + std::to_string(p) + ".vdb";
    const auto result =
        vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"), path(output),
                                 Binary::Sum, options, {}, {}, processing);
    const auto actual = read<openvdb::DoubleGrid>(output);
    const std::array expected{30., 9., 13., 17.};
    for (int x = 0; x < 4; ++x) {
      EXPECT_EQ(actual->tree().getValue({x, 0, 0}), expected[x]);
      EXPECT_EQ(actual->tree().isValueOn({x, 0, 0}),
                x < 3 ? flags[p][x] : false);
    }
    EXPECT_EQ(actual->background(), 5);
    EXPECT_EQ(actual->tree().getValue({32, 0, 0}), 13);
    EXPECT_FALSE(actual->tree().isValueOn({32, 0, 0}));
    EXPECT_EQ(actual->tree().leafCount(), 2u);
    EXPECT_EQ(result.storedLeafCount, 2u);
    EXPECT_EQ(result.inputActiveVoxelCountA, 2u);
    EXPECT_EQ(result.inputActiveVoxelCountB, 2u);
  }
}

TEST_F(OpenVdbFieldComposite,
       CompactActiveTilesAndRemoteInactiveLeavesStaySparse) {
  auto a = field<openvdb::DoubleGrid>(1), b = field<openvdb::DoubleGrid>(3);
  a->tree().addTile(1, {0, 0, 0}, 2., true);
  off(*b, {32, 0, 0}, 5);
  write("a.vdb", {a});
  write("b.vdb", {b});
  const auto result =
      vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"), path("tiles.vdb"),
                               Binary::Product, {}, {}, {}, processing);
  const auto actual = read<openvdb::DoubleGrid>("tiles.vdb");
  EXPECT_EQ(actual->tree().activeTileCount(), 1u);
  EXPECT_EQ(actual->tree().leafCount(), 1u);
  EXPECT_EQ(actual->activeVoxelCount(), 512u);
  EXPECT_EQ(actual->tree().getValue({7, 7, 7}), 6);
  EXPECT_EQ(actual->tree().getValue({32, 0, 0}), 5);
  EXPECT_EQ(actual->background(), 3);
  EXPECT_EQ(result.storedLeafCount, 1u);
}

TEST_F(OpenVdbFieldComposite, ComponentExtremaPreserveLeftBitsOnNumericTies) {
  auto a = field<openvdb::Vec3DGrid>(), b = field<openvdb::Vec3DGrid>();
  a->tree().setValue({0, 0, 0}, {-0., 4., -3.});
  b->tree().setValue({0, 0, 0}, {+0., 2., -1.});
  write("a.vdb", {a});
  write("b.vdb", {b});
  for (const auto operation : {Binary::Minimum, Binary::Maximum}) {
    const bool minimum = operation == Binary::Minimum;
    const std::string output = minimum ? "min.vdb" : "max.vdb";
    (void)vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"), path(output),
                                   operation, {}, {}, {}, processing);
    sameBits(read<openvdb::Vec3DGrid>(output)->tree().getValue({0, 0, 0}),
             minimum ? openvdb::Vec3d(-0., 2., -3.)
                     : openvdb::Vec3d(-0., 4., -1.));
  }
}

TEST_F(OpenVdbFieldComposite,
       ScaledMagnitudeUsesWholeVectorsAndRoundedNormLexRank) {
  const double maximum = std::numeric_limits<double>::max();
  auto a = field<openvdb::Vec3DGrid>(), b = field<openvdb::Vec3DGrid>();
  a->tree().setValue({0, 0, 0}, {maximum, 0, 0});
  b->tree().setValue({0, 0, 0}, {maximum, maximum, 0});
  a->tree().setValue({1, 0, 0}, {3, 4, 0});
  b->tree().setValue({1, 0, 0}, {0, 0, 5});
  a->tree().setValue({2, 0, 0}, {1, -std::ldexp(1., -30), 0});
  b->tree().setValue({2, 0, 0}, {1, 0, 0});
  a->tree().setValue({3, 0, 0}, {-0., 1, 0});
  b->tree().setValue({3, 0, 0}, {+0., 1, 0});
  write("a.vdb", {a});
  write("b.vdb", {b});
  vdb::FieldCompositeOptions options;
  options.vectorExtrema = vdb::FieldVectorExtrema::ScaledMagnitude;
  for (const auto operation : {Binary::Minimum, Binary::Maximum}) {
    const bool minimum = operation == Binary::Minimum;
    const std::string output = minimum ? "min.vdb" : "max.vdb";
    (void)vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"), path(output),
                                   operation, options, {}, {}, processing);
    const auto actual = read<openvdb::Vec3DGrid>(output);
    sameBits(actual->tree().getValue({0, 0, 0}),
             minimum ? openvdb::Vec3d(maximum, 0, 0)
                     : openvdb::Vec3d(maximum, maximum, 0));
    sameBits(actual->tree().getValue({1, 0, 0}),
             minimum ? openvdb::Vec3d(0, 0, 5) : openvdb::Vec3d(3, 4, 0));
    // Both rounded Double norms equal one; lexicographic rank breaks this tie.
    sameBits(actual->tree().getValue({2, 0, 0}),
             minimum ? openvdb::Vec3d(1, -std::ldexp(1., -30), 0)
                     : openvdb::Vec3d(1, 0, 0));
    sameBits(actual->tree().getValue({3, 0, 0}), openvdb::Vec3d(-0., 1, 0));
  }
}

TEST_F(OpenVdbFieldComposite,
       BlendEndpointsAndExtremeMixedTermsAvoidIntermediateLoss) {
  const double maximum = std::numeric_limits<double>::max();
  const double tiny = std::numeric_limits<double>::denorm_min();
  auto a = scalar(), b = scalar();
  const std::array av{maximum, 1e-300, maximum, tiny, -0.};
  const std::array bv{-maximum, 3e-300, maximum, tiny, +0.};
  for (int x = 0; x < 5; ++x) {
    a->tree().setValue({x, 0, 0}, av[x]);
    b->tree().setValue({x, 0, 0}, bv[x]);
  }
  write("a.vdb", {a});
  write("b.vdb", {b});
  for (double weight : {0., .5, 1.}) {
    vdb::FieldCompositeOptions options;
    options.blendWeight = weight;
    const auto output = "blend-" + std::to_string(++serial) + ".vdb";
    (void)vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"), path(output),
                                   Binary::Blend, options, {}, {}, processing);
    const auto actual = read<openvdb::DoubleGrid>(output);
    if (weight == .5) {
      EXPECT_EQ(actual->tree().getValue({0, 0, 0}), 0);
      EXPECT_NEAR(actual->tree().getValue({1, 0, 0}) / 1e-300, 2., 1e-14);
      sameBits(actual->tree().getValue({2, 0, 0}), maximum);
      sameBits(actual->tree().getValue({3, 0, 0}), tiny);
    } else {
      for (int x = 0; x < 5; ++x)
        sameBits(actual->tree().getValue({x, 0, 0}),
                 weight == 0 ? av[x] : bv[x]);
    }
  }
  a = scalar(1);
  b = scalar(maximum);
  write("tiny-a.vdb", {a});
  write("tiny-b.vdb", {b});
  vdb::FieldCompositeOptions options;
  options.blendWeight = tiny;
  (void)vdb::WriteCompositeField(path("tiny-a.vdb"), path("tiny-b.vdb"),
                                 path("tiny-weight.vdb"), Binary::Blend,
                                 options, {}, {}, processing);
  EXPECT_DOUBLE_EQ(
      read<openvdb::DoubleGrid>("tiny-weight.vdb")->tree().getValue({0, 0, 0}),
      1. + maximum * tiny);
}

TEST_F(OpenVdbFieldComposite,
       MergeActiveCopiesPayloadsAndIgnoresInactiveRightValues) {
  const double nanA = std::bit_cast<double>(uint64_t{0x7ff8000000001234});
  const double nanB = std::bit_cast<double>(uint64_t{0x7ff8000000005678});
  const double infinity = std::numeric_limits<double>::infinity();
  auto a = field<openvdb::DoubleGrid>(-0.),
       b = field<openvdb::DoubleGrid>(nanB);
  a->tree().setValue({0, 0, 0}, -infinity);
  off(*b, {0, 0, 0}, 27);
  a->tree().setValue({1, 0, 0}, nanA);
  b->tree().setValue({1, 0, 0}, nanB);
  off(*a, {2, 0, 0}, -0.);
  b->tree().setValue({2, 0, 0}, infinity);
  off(*a, {3, 0, 0}, 7);
  off(*b, {3, 0, 0}, nanB);
  off(*b, {32, 0, 0}, nanB);
  write("a.vdb", {a});
  write("b.vdb", {b});
  for (const auto preference :
       {vdb::FieldMergePreference::Left, vdb::FieldMergePreference::Right}) {
    vdb::FieldCompositeOptions options;
    options.mergePreference = preference;
    const auto output = "merge-" + std::to_string(++serial) + ".vdb";
    const auto result = vdb::WriteCompositeField(
        path("a.vdb"), path("b.vdb"), path(output), Binary::MergeActive,
        options, {}, {}, processing);
    const auto actual = read<openvdb::DoubleGrid>(output);
    sameBits(actual->background(), -0.);
    sameBits(actual->tree().getValue({0, 0, 0}), -infinity);
    sameBits(actual->tree().getValue({1, 0, 0}),
             preference == vdb::FieldMergePreference::Left ? nanA : nanB);
    sameBits(actual->tree().getValue({2, 0, 0}), infinity);
    sameBits(actual->tree().getValue({3, 0, 0}), 7.);
    sameBits(actual->tree().getValue({32, 0, 0}), -0.);
    EXPECT_EQ(actual->activeVoxelCount(), 3u);
    EXPECT_EQ(result.storedLeafCount, 2u);
  }
  vdb::FieldCompositeOptions wrong;
  wrong.activity = vdb::FieldActivity::Intersection;
  binaryFailure("a.vdb", "b.vdb", Binary::MergeActive, wrong);
}

TEST_F(OpenVdbFieldComposite,
       CopyRetainsNonfiniteBitsAndTopologyForEveryNativeType) {
  const double nd = std::bit_cast<double>(uint64_t{0x7ff8000000001234});
  const float nf = std::bit_cast<float>(uint32_t{0x7fc01234});
  const double inf = std::numeric_limits<double>::infinity();
  nativeCopy<openvdb::FloatGrid>("float", -0.f, nf, float(inf));
  nativeCopy<openvdb::DoubleGrid>("double", -0., nd, inf);
  nativeCopy<openvdb::Vec3SGrid>("vec3s", {-0.f, 0.f, -0.f}, {nf, -0.f, 1.f},
                                 {float(inf), nf, -0.f});
  nativeCopy<openvdb::Vec3DGrid>("vec3d", {-0., 0., -0.}, {nd, -0., 1.},
                                 {inf, nd, -0.});
}

TEST_F(OpenVdbFieldComposite,
       UnaryScopeTransformsTilesWithoutChangingInactiveBitsOrTopology) {
  auto input = field<openvdb::DoubleGrid>(-2);
  input->tree().addTile(1, {0, 0, 0}, -3., true);
  input->tree().setValue({16, 0, 0}, -4);
  off(*input, {17, 0, 0}, -5);
  off(*input, {32, 0, 0}, -0.);
  write("input.vdb", {input});
  for (const auto scope :
       {vdb::FieldValueScope::ActiveOnly, vdb::FieldValueScope::AllValues}) {
    const bool all = scope == vdb::FieldValueScope::AllValues;
    vdb::FieldValueTransformOptions options;
    options.operation = Unary::Affine;
    options.scope = scope;
    options.scale[0] = 2;
    options.offset[0] = 1;
    const std::string output = all ? "all.vdb" : "active.vdb";
    const auto result = vdb::WriteTransformedField(
        path("input.vdb"), path(output), options, {}, processing);
    const auto actual = read<openvdb::DoubleGrid>(output);
    EXPECT_TRUE(actual->tree().hasSameTopology(input->tree()));
    EXPECT_EQ(actual->tree().activeTileCount(), 1u);
    EXPECT_EQ(actual->tree().getValue({7, 7, 7}), -5);
    EXPECT_EQ(actual->tree().getValue({16, 0, 0}), -7);
    EXPECT_EQ(actual->tree().getValue({17, 0, 0}), all ? -9 : -5);
    sameBits(actual->tree().getValue({32, 0, 0}), all ? 1. : -0.);
    EXPECT_EQ(actual->background(), all ? -3 : -2);
    EXPECT_EQ(result.grid.activeVoxelCount, 513u);
    EXPECT_EQ(result.storedLeafCount, 2u);
  }
  vdb::FieldValueTransformOptions absolute;
  absolute.operation = Unary::Absolute;
  (void)vdb::WriteTransformedField(path("input.vdb"), path("abs.vdb"), absolute,
                                   {}, processing);
  const auto actual = read<openvdb::DoubleGrid>("abs.vdb");
  EXPECT_EQ(actual->tree().getValue({0, 0, 0}), 3);
  EXPECT_EQ(actual->tree().getValue({16, 0, 0}), 4);
  EXPECT_EQ(actual->tree().getValue({17, 0, 0}), -5);
  sameBits(actual->tree().getValue({32, 0, 0}), -0.);
  EXPECT_EQ(actual->background(), -2);
}

TEST_F(OpenVdbFieldComposite,
       AffineHandlesFusedExtremeTermsAndPreservesIdentityAndOffsetBits) {
  const double maximum = std::numeric_limits<double>::max();
  const double tiny = std::numeric_limits<double>::denorm_min();
  auto input = scalar(maximum);
  write("large.vdb", {input});
  vdb::FieldValueTransformOptions options;
  options.operation = Unary::Affine;
  options.scale[0] = 2;
  options.offset[0] = -maximum;
  (void)vdb::WriteTransformedField(path("large.vdb"), path("fused.vdb"),
                                   options, {}, processing);
  sameBits(read<openvdb::DoubleGrid>("fused.vdb")->tree().getValue({0, 0, 0}),
           maximum);
  input = scalar(tiny);
  write("tiny.vdb", {input});
  options.scale[0] = .5;
  options.offset[0] = tiny;
  (void)vdb::WriteTransformedField(path("tiny.vdb"), path("rounded.vdb"),
                                   options, {}, processing);
  sameBits(read<openvdb::DoubleGrid>("rounded.vdb")->tree().getValue({0, 0, 0}),
           2 * tiny);
  input = scalar(-0.);
  write("zero.vdb", {input});
  options.scale[0] = 1;
  options.offset[0] = 0;
  (void)vdb::WriteTransformedField(path("zero.vdb"), path("identity.vdb"),
                                   options, {}, processing);
  sameBits(
      read<openvdb::DoubleGrid>("identity.vdb")->tree().getValue({0, 0, 0}),
      -0.);
  options.scale[0] = 0;
  options.offset[0] = -0.;
  (void)vdb::WriteTransformedField(path("large.vdb"), path("offset.vdb"),
                                   options, {}, processing);
  sameBits(read<openvdb::DoubleGrid>("offset.vdb")->tree().getValue({0, 0, 0}),
           -0.);
}

TEST_F(OpenVdbFieldComposite,
       ClampRoundsFloatBoundsInwardAndKeepsSignedZeroTies) {
  auto input = field<openvdb::FloatGrid>();
  input->tree().setValue({0, 0, 0}, 0.f);
  input->tree().setValue({1, 0, 0}, 2.f);
  write("float.vdb", {input});
  vdb::FieldValueTransformOptions options;
  options.operation = Unary::Clamp;
  options.minimum[0] = 1 + std::ldexp(1., -26);
  options.maximum[0] = 1 + 11 * std::ldexp(1., -25);
  (void)vdb::WriteTransformedField(path("float.vdb"), path("inward.vdb"),
                                   options, {}, processing);
  const auto actual = read<openvdb::FloatGrid>("inward.vdb");
  sameBits(actual->tree().getValue({0, 0, 0}), std::nextafter(1.f, 2.f));
  sameBits(actual->tree().getValue({1, 0, 0}), 1.f + std::ldexp(2.f, -23));
  options.maximum[0] = 1 + 3 * std::ldexp(1., -25);
  unaryFailure("float.vdb", options);
  options.minimum[0] = std::numeric_limits<double>::denorm_min();
  options.maximum[0] = 1;
  unaryFailure("float.vdb", options);
  options = {};
  options.operation = Unary::Clamp;
  (void)vdb::WriteTransformedField(
      path("float.vdb"), path("default-bounds.vdb"), options, {}, processing);
  auto zero = scalar(-0.);
  write("zero.vdb", {zero});
  options.minimum[0] = -0.;
  options.maximum[0] = +0.;
  (void)vdb::WriteTransformedField(path("zero.vdb"), path("ties.vdb"), options,
                                   {}, processing);
  sameBits(read<openvdb::DoubleGrid>("ties.vdb")->tree().getValue({0, 0, 0}),
           -0.);
}

TEST_F(
    OpenVdbFieldComposite,
    VectorUnaryParametersAreComponentwiseAndActiveOnlySkipsNonfiniteInactive) {
  auto input = field<openvdb::Vec3DGrid>({-2, -3, -4});
  input->tree().setValue({0, 0, 0}, {-2, 3, -4});
  const openvdb::Vec3d inactive{
      -0., std::numeric_limits<double>::infinity(),
      std::bit_cast<double>(uint64_t{0x7ff8000000001234})};
  off(*input, {16, 0, 0}, inactive);
  write("input.vdb", {input});
  vdb::FieldValueTransformOptions options;
  options.operation = Unary::Affine;
  options.scale = {2, -1, .5};
  options.offset = {1, 2, -3};
  (void)vdb::WriteTransformedField(path("input.vdb"), path("affine.vdb"),
                                   options, {}, processing);
  auto actual = read<openvdb::Vec3DGrid>("affine.vdb");
  sameBits(actual->tree().getValue({0, 0, 0}), openvdb::Vec3d(-3, -1, -5));
  sameBits(actual->tree().getValue({16, 0, 0}), inactive);
  options.scope = vdb::FieldValueScope::AllValues;
  unaryFailure("input.vdb", options);
  options = {};
  options.operation = Unary::Absolute;
  (void)vdb::WriteTransformedField(path("input.vdb"), path("abs.vdb"), options,
                                   {}, processing);
  sameBits(read<openvdb::Vec3DGrid>("abs.vdb")->tree().getValue({0, 0, 0}),
           openvdb::Vec3d(2, 3, 4));
  options = {};
  options.operation = Unary::Clamp;
  options.minimum = {-1, -2, -3};
  options.maximum = {0, 2, 3};
  (void)vdb::WriteTransformedField(path("input.vdb"), path("clamp.vdb"),
                                   options, {}, processing);
  sameBits(read<openvdb::Vec3DGrid>("clamp.vdb")->tree().getValue({0, 0, 0}),
           openvdb::Vec3d(-1, 2, -3));
}

TEST_F(OpenVdbFieldComposite, ExactMapsNativeTypesAndSemanticTagsMustMatch) {
  auto a = scalar(2), b = scalar(3);
  write("a.vdb", {a});
  b->transform().postTranslate({1e-9, 0, 0});
  write("translated.vdb", {b});
  binaryFailure("a.vdb", "translated.vdb");
  auto differentType = field<openvdb::FloatGrid>();
  differentType->tree().setValue({0, 0, 0}, 3.f);
  write("float.vdb", {differentType});
  binaryFailure("a.vdb", "float.vdb");
  b->setTransform(a->transform().copy());
  b->setGridClass(openvdb::GRID_FOG_VOLUME);
  write("class.vdb", {b});
  binaryFailure("a.vdb", "class.vdb");
  for (const auto cls : {openvdb::GRID_LEVEL_SET, openvdb::GRID_STAGGERED}) {
    b->setGridClass(cls);
    write("unsupported.vdb", {b});
    unaryFailure("unsupported.vdb");
  }
  b->setGridClass(openvdb::GRID_UNKNOWN);
  for (const auto &key : {"class", "vector_type", "is_local_space"}) {
    auto malformed = b->deepCopy();
    malformed->insertMeta(key,
                          openvdb::StringMetadata("not-a-native-semantic-tag"));
    write("malformed.vdb", {malformed});
    unaryFailure("malformed.vdb");
  }
  b->removeMeta("class");
  b->insertMeta("class", openvdb::Int32Metadata(0));
  write("wrong-type.vdb", {b});
  unaryFailure("wrong-type.vdb");
  auto vector = field<openvdb::Vec3DGrid>();
  vector->tree().setValue({0, 0, 0}, {1, 2, 3});
  vector->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);
  write("normalized.vdb", {vector});
  unaryFailure("normalized.vdb");
  vector->setVectorType(openvdb::VEC_INVARIANT);
  vector->setIsInWorldSpace(false);
  write("index-space.vdb", {vector});
  unaryFailure("index-space.vdb");
}

TEST_F(OpenVdbFieldComposite,
       FogChecksOperatedOutputWithoutClampingOrTouchingUnoperatedValues) {
  auto a = scalar(2), b = scalar(3);
  a->setGridClass(openvdb::GRID_FOG_VOLUME);
  b->setGridClass(openvdb::GRID_FOG_VOLUME);
  write("a.vdb", {a});
  write("b.vdb", {b});
  (void)vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"), path("fog.vdb"),
                                 Binary::Sum, {}, {}, {}, processing);
  const auto actual = read<openvdb::DoubleGrid>("fog.vdb");
  EXPECT_EQ(actual->getGridClass(), openvdb::GRID_FOG_VOLUME);
  EXPECT_EQ(actual->tree().getValue({0, 0, 0}), 5);
  vdb::FieldValueTransformOptions affine;
  affine.operation = Unary::Affine;
  affine.scale[0] = -1;
  unaryFailure("a.vdb", affine);
  off(*a, {16, 0, 0}, -4.);
  write("inactive-negative.vdb", {a});
  affine.scale[0] = 2;
  (void)vdb::WriteTransformedField(path("inactive-negative.vdb"),
                                   path("active-only.vdb"), affine, {},
                                   processing);
  EXPECT_EQ(
      read<openvdb::DoubleGrid>("active-only.vdb")->tree().getValue({16, 0, 0}),
      -4);
  affine.scope = vdb::FieldValueScope::AllValues;
  unaryFailure("inactive-negative.vdb", affine);
  binaryFailure("inactive-negative.vdb", "b.vdb");
}

TEST_F(OpenVdbFieldComposite,
       RangeLossAndInvalidOrUnusedOptionsFailBeforePublication) {
  auto a = scalar(std::numeric_limits<double>::denorm_min()), b = scalar(.5);
  write("tiny.vdb", {a});
  write("half.vdb", {b});
  binaryFailure("tiny.vdb", "half.vdb", Binary::Product);
  a = scalar(std::numeric_limits<double>::max());
  b = scalar(2);
  write("large.vdb", {a});
  write("two.vdb", {b});
  binaryFailure("large.vdb", "two.vdb", Binary::Product);
  vdb::FieldValueTransformOptions unary;
  unary.operation = Unary::Affine;
  unary.scale[0] = 2;
  unaryFailure("large.vdb", unary);
  unary.scale[0] = .5;
  unaryFailure("tiny.vdb", unary);
  unary.scale[0] = 1;
  unary.scale[1] = 2;
  unaryFailure("two.vdb", unary);
  unary = {};
  unary.offset[0] = 1;
  unaryFailure("two.vdb", unary);
  unary = {};
  unary.operation = Unary::Absolute;
  unary.minimum[0] = 0;
  unaryFailure("two.vdb", unary);
  unary = {};
  unary.maxWorkItems = 1;
  unaryFailure("two.vdb", unary);
  vdb::FieldCompositeOptions binary;
  binary.maxWorkItems = 1;
  binaryFailure("two.vdb", "half.vdb", Binary::Sum, binary);
  binary = {};
  binary.blendWeight = std::numeric_limits<double>::quiet_NaN();
  binaryFailure("two.vdb", "half.vdb", Binary::Blend, binary);
  binary = {};
  binary.activity = static_cast<vdb::FieldActivity>(99);
  binaryFailure("two.vdb", "half.vdb", Binary::Sum, binary);
}

TEST_F(OpenVdbFieldComposite,
       SharedBudgetsIncludeRemoteTopologyEncodedBytesAndMetadata) {
  auto a = scalar(1), b = field<openvdb::DoubleGrid>();
  off(*b, {1'000'000, 0, 0}, 3.);
  write("a.vdb", {a});
  write("b.vdb", {b});
  auto limits = processing;
  limits.maxVoxelCount = 512;
  binaryFailure("a.vdb", "b.vdb", Binary::Sum, {}, &limits);
  limits = processing;
  limits.maxInputBytes = 1;
  binaryFailure("a.vdb", "b.vdb", Binary::Sum, {}, &limits);
  unaryFailure("a.vdb", {}, &limits);
  a->insertMeta("large-custom",
                openvdb::StringMetadata(std::string(1'000'000, 'x')));
  write("metadata.vdb", {a});
  limits = processing;
  limits.maxInputBytes = 8 * 1024 * 1024;
  unaryFailure("metadata.vdb", {}, &limits);
  const auto result =
      vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"), path("remote.vdb"),
                               Binary::Sum, {}, {}, {}, processing);
  EXPECT_EQ(result.storedLeafCount, 2u);
  EXPECT_EQ(read<openvdb::DoubleGrid>("remote.vdb")
                ->tree()
                .getValue({1'000'000, 0, 0}),
            3);
}

TEST_F(OpenVdbFieldComposite,
       SelectorsMetadataAndExistingFilesRemainProtected) {
  auto a = scalar(2), duplicate = scalar(7), b = scalar(3);
  a->insertMeta("artist", openvdb::StringMetadata("left only"));
  b->insertMeta("right-only", openvdb::StringMetadata("not merged"));
  write("a.vdb", {a, duplicate});
  write("b.vdb", {b});
  const auto originalA = bytes("a.vdb"), originalB = bytes("b.vdb");
  binaryFailure("a.vdb", "b.vdb", Binary::Sum, {}, nullptr, "field");
  (void)vdb::WriteCompositeField(path("a.vdb"), path("b.vdb"),
                                 path("selected.vdb"), Binary::Sum, {},
                                 "descriptor:field[1]", {}, processing);
  EXPECT_EQ(
      read<openvdb::DoubleGrid>("selected.vdb")->tree().getValue({0, 0, 0}),
      10);
  a->setName("descriptor:field");
  write("literal.vdb", {a});
  (void)vdb::WriteCompositeField(path("literal.vdb"), path("b.vdb"),
                                 path("literal-out.vdb"), Binary::Sum, {},
                                 "descriptor:descriptor:field", {}, processing);
  const auto actual = read<openvdb::DoubleGrid>("literal-out.vdb");
  EXPECT_EQ(actual->metaValue<std::string>("artist"), "left only");
  EXPECT_FALSE(bool((*actual)["right-only"]));
  EXPECT_EQ(actual->getName(), "composite");
  const auto outputBefore = bytes("literal-out.vdb");
  EXPECT_THROW((void)vdb::WriteCompositeField(
                   path("literal.vdb"), path("b.vdb"), path("literal-out.vdb"),
                   Binary::Sum, {}, {}, {}, processing),
               std::exception);
  EXPECT_THROW((void)vdb::WriteTransformedField(path("b.vdb"), path("b.vdb"),
                                                {}, {}, processing),
               std::exception);
  EXPECT_EQ(bytes("literal-out.vdb"), outputBefore);
  EXPECT_EQ(bytes("a.vdb"), originalA);
  EXPECT_EQ(bytes("b.vdb"), originalB);
  noTemporaryDirectories();
}

TEST_F(OpenVdbFieldComposite,
       UnsupportedTypesMissingStatisticsAndStreamingFilesReject) {
  auto scalarGrid = scalar();
  auto integer = openvdb::Int32Grid::create();
  integer->setName("field");
  integer->tree().setValue({0, 0, 0}, 2);
  write("integer.vdb", {integer});
  write("scalar.vdb", {scalarGrid});
  unaryFailure("integer.vdb");
  binaryFailure("integer.vdb", "scalar.vdb");
  write("no-stats.vdb", {scalarGrid}, false);
  unaryFailure("no-stats.vdb");
  binaryFailure("scalar.vdb", "no-stats.vdb");
  {
    std::ofstream output(path("stream.vdb"), std::ios::binary);
    openvdb::io::Stream stream(output);
    stream.write(openvdb::GridPtrVec{scalarGrid});
  }
  unaryFailure("stream.vdb");
  binaryFailure("scalar.vdb", "stream.vdb");
}
#endif
} // namespace
