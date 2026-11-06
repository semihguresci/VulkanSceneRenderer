#include "Container/geometry/OpenVdbFileAuthoring.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/points/PointGroup.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbFileAuthoringAvailability, DisabledBeforePathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FileAuthoringOptions options;
  options.maxGrids = 0;
  try {
    (void)vdb::WriteAuthoredVdb({}, options, {.maxInputBytes = 0});
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points = openvdb::points;
class OpenVdbFileAuthoring : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-file-author-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
  }
  void TearDown() override {
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      if (entry.is_regular_file())
        std::filesystem::remove(entry.path(), error);
    std::filesystem::remove(directory, error);
  }
  auto path(const std::string &name) const { return directory / name; }
  static std::string bytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &metadata = {}, bool statistics = true) {
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.setGridStatsMetadataEnabled(statistics);
    file.write(grids, metadata);
  }
  static openvdb::FloatGrid::Ptr field(const std::string &name,
                                       float value = 1) {
    auto grid = openvdb::FloatGrid::create(-0.0f);
    grid->setName(name);
    grid->tree().setValue({1, 2, 3}, value);
    grid->insertMeta("artist", openvdb::StringMetadata("unchanged"));
    return grid;
  }
  vdb::FileAuthoringOptions options(const std::string &input) const {
    vdb::FileAuthoringOptions result;
    result.inputs.push_back({path(input)});
    return result;
  }
  template <class Grid>
  auto read(const std::string &file, const std::string &name) {
    openvdb::io::File input(path(file).string());
    input.open(false);
    auto grid = openvdb::gridPtrCast<Grid>(input.readGrid(name));
    input.close();
    return grid;
  }
  template <class T> static void valueBits(const T &a, const T &b) {
    if constexpr (std::is_same_v<T, float>)
      EXPECT_EQ(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b));
    else if constexpr (std::is_same_v<T, double>)
      EXPECT_EQ(std::bit_cast<uint64_t>(a), std::bit_cast<uint64_t>(b));
    else if constexpr (openvdb::VecTraits<T>::IsVec) {
      for (size_t i = 0; i < 3; ++i)
        valueBits(a[i], b[i]);
    } else
      EXPECT_EQ(a, b);
  }
  template <class Grid> static void same(const Grid &a, const Grid &b) {
    EXPECT_EQ(a.transform(), b.transform());
    EXPECT_EQ(a.getGridClass(), b.getGridClass());
    EXPECT_EQ(a.getVectorType(), b.getVectorType());
    EXPECT_EQ(a.isInWorldSpace(), b.isInWorldSpace());
    EXPECT_TRUE(a.tree().hasSameTopology(b.tree()));
    valueBits(a.background(), b.background());
    for (auto value = a.cbeginValueAll(); value; ++value) {
      valueBits(*value, b.tree().getValue(value.getCoord()));
      EXPECT_EQ(value.isValueOn(), b.tree().isValueOn(value.getCoord()));
    }
  }
  template <class Grid>
  void scalarType(const std::string &name,
                  const typename Grid::ValueType &value) {
    auto grid = Grid::create();
    grid->setName(name);
    grid->tree().setValue({1, 2, 3}, value);
    grid->tree().setValueOff({16, 0, 0}, value);
    write(name + ".vdb", {grid});
    (void)vdb::WriteAuthoredVdb(path(name + "-out.vdb"),
                                options(name + ".vdb"));
    same(*grid, *read<Grid>(name + "-out.vdb", name));
  }
  void cleanFailure(const vdb::FileAuthoringOptions &options,
                    const vdb::ProcessingOptions &processing = {}) {
    const auto output = path("failure.vdb");
    EXPECT_THROW((void)vdb::WriteAuthoredVdb(output, options, processing),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(output));
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      EXPECT_FALSE(entry.is_directory()) << entry.path();
  }
  std::filesystem::path directory;
};

TEST_F(OpenVdbFileAuthoring,
       DefaultCopyRetainsNonfinitePayloadBitsInactiveSignsAndAffineSemantics) {
  auto f = field("float", std::bit_cast<float>(uint32_t(0x7fc12345)));
  f->tree().touchLeaf({16, 0, 0})->setValueOff({16, 0, 0}, -0.0f);
  f->tree().setValue({2, 2, 3}, std::numeric_limits<float>::infinity());
  f->setGridClass(openvdb::GRID_FOG_VOLUME);
  f->transform().postRotate(.47, openvdb::math::Z_AXIS);
  f->transform().postScale({-2, 3, .5});
  f->transform().postTranslate({1e15, -2e15, 4});
  openvdb::UnknownMetadata opaque("opaque:bytes");
  opaque.setValue({0, 255, 1, 0, 128});
  f->insertMeta("opaque", opaque);
  auto d = openvdb::DoubleGrid::create(-0.0);
  d->setName("double");
  d->tree().setValue({0, 0, 0},
                     std::bit_cast<double>(uint64_t(0xfff8000000012345)));
  d->tree().touchLeaf({32, 0, 0})->setValueOff({32, 0, 0}, -0.0);
  d->tree().setValue({1, 0, 0}, std::numeric_limits<double>::denorm_min());
  write("raw.vdb", {f, d});
  const auto original = bytes(path("raw.vdb"));
  const auto result =
      vdb::WriteAuthoredVdb(path("copy.vdb"), options("raw.vdb"));
  EXPECT_EQ(result.outputGridCount, 2u);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_EQ(result.inputBytes, original.size());
  EXPECT_EQ(result.outputBytes, std::filesystem::file_size(path("copy.vdb")));
  same(*f, *read<openvdb::FloatGrid>("copy.vdb", "float"));
  same(*d, *read<openvdb::DoubleGrid>("copy.vdb", "double"));
  auto copied = read<openvdb::FloatGrid>("copy.vdb", "float");
  auto copiedOpaque = std::dynamic_pointer_cast<const openvdb::UnknownMetadata>(
      (*copied)["opaque"]);
  ASSERT_TRUE(copiedOpaque);
  EXPECT_EQ(copiedOpaque->value(), opaque.value());
  EXPECT_EQ(bytes(path("raw.vdb")), original);
}

TEST_F(OpenVdbFileAuthoring,
       BoolMaskIntegerAndVectorNativeTypesRetainStoredTopology) {
  scalarType<openvdb::BoolGrid>("bool", true);
  scalarType<openvdb::Int32Grid>("i32", INT32_MIN);
  scalarType<openvdb::Int64Grid>("i64", INT64_MAX);
  scalarType<openvdb::Vec3IGrid>("vi", {INT32_MIN, 17, INT32_MAX});
  scalarType<openvdb::Vec3SGrid>(
      "vs", {-0.0f, std::numeric_limits<float>::denorm_min(), -7});
  scalarType<openvdb::Vec3DGrid>(
      "vd", {-0.0, std::numeric_limits<double>::infinity(), -1e300});
  auto mask = openvdb::MaskGrid::create();
  mask->setName("mask");
  mask->tree().setValueOn({1, 2, 3});
  mask->tree().touchLeaf({16, 0, 0});
  write("mask.vdb", {mask});
  (void)vdb::WriteAuthoredVdb(path("mask-out.vdb"), options("mask.vdb"));
  const auto actual = read<openvdb::MaskGrid>("mask-out.vdb", "mask");
  EXPECT_TRUE(mask->tree().hasSameTopology(actual->tree()));
  EXPECT_EQ(actual->tree().leafCount(), mask->tree().leafCount());
}

TEST_F(OpenVdbFileAuthoring, SelectionRemovalRenameAndMergeRetainFileMetadata) {
  openvdb::MetaMap a, b;
  a.insertMeta("shared", openvdb::Int64Metadata(7));
  a.insertMeta("first", openvdb::StringMetadata("a"));
  b.insertMeta("shared", openvdb::Int64Metadata(7));
  b.insertMeta("second", openvdb::StringMetadata("b"));
  write("a.vdb", {field("keep"), field("remove"), field("unselected")}, a);
  write("b.vdb", {field("keep", 2)}, b);
  auto o = options("a.vdb");
  o.inputs[0].select = {"keep", "remove"};
  o.inputs[0].remove = {"remove"};
  o.inputs.push_back({path("b.vdb"), {}, {}, {{"keep", "renamed"}}});
  const auto result = vdb::WriteAuthoredVdb(path("merged.vdb"), o);
  EXPECT_EQ(result.inputGridCount, 4u);
  EXPECT_EQ(result.outputGridCount, 2u);
  EXPECT_EQ(result.removedGridCount, 1u);
  EXPECT_EQ(result.renamedGridCount, 1u);
  EXPECT_FLOAT_EQ(read<openvdb::FloatGrid>("merged.vdb", "renamed")
                      ->tree()
                      .getValue({1, 2, 3}),
                  2);
  openvdb::io::File file(path("merged.vdb").string());
  file.open(false);
  const auto meta = file.getMetadata();
  EXPECT_EQ(meta->metaValue<int64_t>("shared"), 7);
  EXPECT_EQ(meta->metaValue<std::string>("first"), "a");
  EXPECT_EQ(meta->metaValue<std::string>("second"), "b");
}

TEST_F(OpenVdbFileAuthoring,
       ConflictingFileMetadataRequiresExplicitPolicyAndTypedEdits) {
  openvdb::MetaMap a, b;
  a.insertMeta("conflict", openvdb::Int64Metadata(1));
  b.insertMeta("conflict", openvdb::StringMetadata("last"));
  write("a.vdb", {field("a")}, a);
  write("b.vdb", {field("b")}, b);
  auto o = options("a.vdb");
  o.inputs.push_back({path("b.vdb")});
  cleanFailure(o);
  for (const auto policy : {vdb::FileMetadataConflict::PreferFirst,
                            vdb::FileMetadataConflict::PreferLast}) {
    o.metadataConflict = policy;
    const auto name = policy == vdb::FileMetadataConflict::PreferFirst
                          ? "first.vdb"
                          : "last.vdb";
    (void)vdb::WriteAuthoredVdb(path(name), o);
    openvdb::io::File file(path(name).string());
    file.open(false);
    const auto meta = file.getMetadata();
    if (policy == vdb::FileMetadataConflict::PreferFirst)
      EXPECT_EQ(meta->metaValue<int64_t>("conflict"), 1);
    else
      EXPECT_EQ(meta->metaValue<std::string>("conflict"), "last");
  }
  o.metadata = {{"conflict", vdb::FileMetadataOperation::Set, 2.5}};
  (void)vdb::WriteAuthoredVdb(path("edited.vdb"), o);
  openvdb::io::File file(path("edited.vdb").string());
  file.open(false);
  EXPECT_DOUBLE_EQ(file.getMetadata()->metaValue<double>("conflict"), 2.5);
}

TEST_F(OpenVdbFileAuthoring,
       TypedCustomGridEditsCanReplaceTypesButCannotSpoofStructure) {
  write("input.vdb", {field("field")});
  auto o = options("input.vdb");
  o.inputs[0].edits = {
      {"field",
       "renamed",
       {{"artist", vdb::FileMetadataOperation::Remove},
        {"bool", vdb::FileMetadataOperation::Set, true},
        {"i32", vdb::FileMetadataOperation::Set, int32_t(-7)},
        {"i64", vdb::FileMetadataOperation::Set, int64_t(INT64_MAX)},
        {"float", vdb::FileMetadataOperation::Set, .125f},
        {"double", vdb::FileMetadataOperation::Set, .1234567891234567},
        {"text", vdb::FileMetadataOperation::Set, std::string("a\0b", 3)},
        {"v32", vdb::FileMetadataOperation::Set, std::array<float, 3>{1, 2, 3}},
        {"v64", vdb::FileMetadataOperation::Set,
         std::array<double, 3>{4, 5, 6}}}}};
  const auto result = vdb::WriteAuthoredVdb(path("edited.vdb"), o);
  EXPECT_EQ(result.metadataEditCount, 9u);
  auto grid = read<openvdb::FloatGrid>("edited.vdb", "renamed");
  EXPECT_FALSE((*grid)["artist"]);
  EXPECT_TRUE(grid->metaValue<bool>("bool"));
  EXPECT_EQ(grid->metaValue<int64_t>("i64"), INT64_MAX);
  EXPECT_EQ(grid->metaValue<std::string>("text"), std::string("a\0b", 3));
  EXPECT_EQ(grid->metaValue<openvdb::Vec3d>("v64"), openvdb::Vec3d(4, 5, 6));
  for (const auto key :
       {openvdb::GridBase::META_GRID_NAME, openvdb::GridBase::META_GRID_CLASS,
        openvdb::GridBase::META_SAVE_HALF_FLOAT,
        openvdb::GridBase::META_FILE_VOXEL_COUNT}) {
    o.inputs[0].edits[0].metadata = {
        {key, vdb::FileMetadataOperation::Set, std::string("spoof")}};
    cleanFailure(o);
  }
}

TEST_F(OpenVdbFileAuthoring,
       ExactDescriptorsAndLiteralPrefixNamesWorkWhileCollisionsFail) {
  write("duplicates.vdb", {field("duplicate", 1), field("duplicate", 2),
                           field("descriptor:wind", 3)});
  auto o = options("duplicates.vdb");
  cleanFailure(o); // Final duplicate names require explicit rename.
  o.inputs[0].select = {"duplicate"};
  cleanFailure(o);
  std::vector<std::string> duplicateDescriptors;
  {
    openvdb::io::File input(path("duplicates.vdb").string());
    input.open(false);
    for (auto it = input.beginName(); it != input.endName(); ++it)
      if (input.readGridMetadata(it.gridName())->getName() == "duplicate")
        duplicateDescriptors.push_back("descriptor:" + it.gridName());
  }
  ASSERT_EQ(duplicateDescriptors.size(), 2u);
  o.inputs[0].select = {duplicateDescriptors[0], duplicateDescriptors[1],
                        "descriptor:descriptor:wind"};
  o.inputs[0].edits = {{duplicateDescriptors[0], "first"},
                       {duplicateDescriptors[1], "second"}};
  (void)vdb::WriteAuthoredVdb(path("disambiguated.vdb"), o);
  EXPECT_NE(read<openvdb::FloatGrid>("disambiguated.vdb", "first")
                ->tree()
                .getValue({1, 2, 3}),
            read<openvdb::FloatGrid>("disambiguated.vdb", "second")
                ->tree()
                .getValue({1, 2, 3}));
  EXPECT_FLOAT_EQ(
      read<openvdb::FloatGrid>("disambiguated.vdb", "descriptor:wind")
          ->tree()
          .getValue({1, 2, 3}),
      3);
  write("colliding.vdb", {field("field"), field("field"), field("field[0]")});
  cleanFailure(options("colliding.vdb"));
  write("unnamed.vdb", {field("")});
  o = options("unnamed.vdb");
  {
    openvdb::io::File input(path("unnamed.vdb").string());
    input.open(false);
    ASSERT_TRUE(input.beginName() != input.endName());
    o.inputs[0].edits = {
        {"descriptor:" + input.beginName().gridName(), "formerly_unnamed"}};
  }
  EXPECT_NO_THROW((void)vdb::WriteAuthoredVdb(path("named.vdb"), o));
}

TEST_F(OpenVdbFileAuthoring,
       RemovingAllGridsWritesValidEmptySeekableFileWithMetadata) {
  openvdb::MetaMap meta;
  meta.insertMeta("note", openvdb::StringMetadata("still here"));
  write("input.vdb", {field("field")}, meta);
  auto o = options("input.vdb");
  o.inputs[0].remove = {"field"};
  const auto result = vdb::WriteAuthoredVdb(path("empty.vdb"), o);
  EXPECT_EQ(result.outputGridCount, 0u);
  openvdb::io::File file(path("empty.vdb").string());
  file.open(false);
  EXPECT_TRUE(file.beginName() == file.endName());
  EXPECT_EQ(file.getMetadata()->metaValue<std::string>("note"), "still here");
}

TEST_F(OpenVdbFileAuthoring,
       CompressionDefaultsPreserveBitsAndLossOptInsAreReported) {
  auto grid = field("field", 1.234567f);
  grid->tree().touchLeaf({16, 0, 0})->setValueOff({16, 0, 0}, -0.0f);
  write("input.vdb", {grid});
  for (const auto compression :
       {vdb::FileCompression::Zip, vdb::FileCompression::None,
        vdb::FileCompression::Blosc}) {
    auto o = options("input.vdb");
    o.compression = compression;
    const auto file = "compressed-" + std::to_string(int(compression)) + ".vdb";
    const auto result = vdb::WriteAuthoredVdb(path(file), o);
    EXPECT_TRUE(result.diagnostics.empty());
    same(*grid, *read<openvdb::FloatGrid>(file, "field"));
  }
  auto o = options("input.vdb");
  o.activeMaskCompression = true;
  o.saveFloatAsHalf = true;
  const auto result = vdb::WriteAuthoredVdb(path("loss-opt-in.vdb"), o);
  EXPECT_EQ(result.diagnostics.size(), 2u);
  auto changed = read<openvdb::FloatGrid>("loss-opt-in.vdb", "field");
  EXPECT_NE(changed->tree().getValue({1, 2, 3}),
            grid->tree().getValue({1, 2, 3}));
  EXPECT_NEAR(changed->tree().getValue({1, 2, 3}), 1.234567f, .001f);
}

TEST_F(OpenVdbFileAuthoring,
       NativePointCodecsHiddenFlagsGroupsAndInactiveRecordsSurviveCopy) {
  using Packed = points::TypedAttributeArray<float, points::TruncateCodec>;
  if (!Packed::isRegistered())
    Packed::registerType();
  auto transform = openvdb::math::Transform::createLinearTransform(1);
  auto grid =
      points::createPointDataGrid<points::NullCodec, points::PointDataGrid>(
          std::vector<openvdb::Vec3f>{{.1f, 0, 0}, {.2f, 0, 0}, {16.1f, 0, 0}},
          *transform);
  grid->setName("points");
  points::appendAttribute<float, points::TruncateCodec>(grid->tree(), "packed",
                                                        .75f);
  points::appendAttribute<float>(grid->tree(), "uniform_stride", -0.0f, 2);
  points::appendGroup(grid->tree(), "keep");
  points::setGroup(grid->tree(), "keep", true);
  auto descriptor = points::makeDescriptorUnique(grid->tree());
  descriptor->getMetadata().insertMeta("custom",
                                       openvdb::Int64Metadata(INT64_MAX));
  grid->tree().setActiveState({0, 0, 0}, false);
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) {
    leaf->attributeArray("packed").setHidden(true);
    // appendAttribute treats -0 as its default zero; explicitly author the
    // storage sign before testing a raw passthrough.
    static_cast<points::TypedAttributeArray<float, points::NullCodec> &>(
        leaf->attributeArray("uniform_stride"))
        .collapse(-0.0f);
  }
  write("points.vdb", {grid});
  {
    const auto original = read<points::PointDataGrid>("points.vdb", "points");
    for (auto leaf = original->tree().cbeginLeaf(); leaf; ++leaf) {
      points::AttributeHandle<float> value(
          leaf->constAttributeArray("uniform_stride"));
      ASSERT_TRUE(std::signbit(value.get(0)));
    }
  }
  auto o = options("points.vdb");
  o.inputs[0].edits = {{"points", "copied"}};
  const auto result = vdb::WriteAuthoredVdb(path("copied.vdb"), o);
  EXPECT_EQ(result.pointCount, 3u);
  auto copied = read<points::PointDataGrid>("copied.vdb", "copied");
  EXPECT_TRUE(grid->tree().hasSameTopology(copied->tree()));
  for (auto leaf = grid->tree().cbeginLeaf(); leaf; ++leaf) {
    const auto *actual = copied->tree().probeConstLeaf(leaf->origin());
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->attributeSet().descriptor(),
              leaf->attributeSet().descriptor());
    EXPECT_TRUE(actual->constAttributeArray("packed").isHidden());
    EXPECT_TRUE(actual->constAttributeArray("packed").isType<Packed>());
    EXPECT_TRUE(actual->constAttributeArray("uniform_stride").isUniform());
    EXPECT_EQ(actual->constAttributeArray("uniform_stride").stride(), 2u);
    points::AttributeHandle<float> uniform(
        actual->constAttributeArray("uniform_stride"));
    EXPECT_TRUE(std::signbit(uniform.get(0)));
    points::AttributeHandle<float, points::TruncateCodec> value(
        actual->constAttributeArray("packed"));
    const auto index = actual->attributeSet().groupIndex("keep");
    points::GroupHandle group(static_cast<const points::GroupAttributeArray &>(
                                  actual->constAttributeArray(index.first)),
                              index.second);
    for (openvdb::Index n = 0; n < actual->pointCount(); ++n) {
      EXPECT_EQ(value.get(n), .75f);
      EXPECT_TRUE(group.get(n));
    }
    for (openvdb::Index n = 0;
         n < points::PointDataGrid::TreeType::LeafNodeType::NUM_VALUES; ++n) {
      EXPECT_EQ(leaf->getValue(n), actual->getValue(n));
      EXPECT_EQ(leaf->isValueOn(n), actual->isValueOn(n));
    }
  }
  o.maxPoints = 2;
  cleanFailure(o);
}

TEST_F(OpenVdbFileAuthoring,
       RetainedDecodedLeavesShareOneBudgetAcrossSelectedGrids) {
  auto a = openvdb::DoubleGrid::create(0);
  a->setName("first");
  // Many leaf buffers, within a compact internal-node extent: the encoded
  // constant payload is much smaller than its materialized Double storage.
  for (int x = 0; x < 8; ++x)
    for (int y = 0; y < 8; ++y)
      for (int z = 0; z < 8; ++z)
        a->tree().setValue({8 * x, 8 * y, 8 * z}, 1.0);
  auto b = a->deepCopy();
  b->setName("second");
  write("two.vdb", {a, b});
  vdb::ProcessingOptions limits;
  limits.maxInputBytes = 4 * 1024 * 1024 + 6 * a->memUsage() + 512 * 1024;
  auto o = options("two.vdb");
  o.inputs[0].select = {"first"};
  EXPECT_NO_THROW((void)vdb::WriteAuthoredVdb(path("one.vdb"), o, limits));
  o.inputs[0].select.clear();
  cleanFailure(o, limits);
}

TEST_F(OpenVdbFileAuthoring,
       LimitsMalformedSelectionsStreamingAndExistingOutputLeaveNoArtifacts) {
  write("input.vdb", {field("field")});
  const auto original = bytes(path("input.vdb"));
  auto o = options("input.vdb");
  o.inputs[0].select = {"field", "descriptor:field"};
  cleanFailure(o);
  o.inputs[0].select = {"missing"};
  cleanFailure(o);
  o = options("input.vdb");
  o.inputs[0].remove = {"field", "field"};
  cleanFailure(o);
  o = options("input.vdb");
  o.inputs[0].edits = {{"field", "one"}, {"descriptor:field", "two"}};
  cleanFailure(o);
  o = options("input.vdb");
  o.inputs[0].edits = {{"field",
                        {},
                        {{"x", vdb::FileMetadataOperation::Set, 1.0},
                         {"x", vdb::FileMetadataOperation::Set, 2.0}}}};
  cleanFailure(o);
  o = options("input.vdb");
  o.metadata = {{"not-finite", vdb::FileMetadataOperation::Set,
                 std::numeric_limits<double>::infinity()}};
  cleanFailure(o);
  o = options("input.vdb");
  cleanFailure(o, {.maxVoxelCount = 511});
  cleanFailure(o, {.maxInputBytes = 1024});
  o.maxMetadataBytes = 64;
  cleanFailure(o);
  o = options("input.vdb");
  o.maxGrids = 0;
  cleanFailure(o);
  write("no-stats.vdb", {field("field")}, {}, false);
  cleanFailure(options("no-stats.vdb"));
  {
    std::ofstream stream(path("stream.vdb"), std::ios::binary);
    openvdb::io::Stream archive(stream);
    archive.write(openvdb::GridPtrVec{field("field")});
  }
  cleanFailure(options("stream.vdb"));
  EXPECT_THROW(
      (void)vdb::WriteAuthoredVdb(path("input.vdb"), options("input.vdb")),
      std::exception);
  EXPECT_EQ(bytes(path("input.vdb")), original);
}
#endif
} // namespace
