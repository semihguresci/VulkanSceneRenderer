#include "Container/geometry/OpenVdbPointFieldSampling.h"
#include "Container/geometry/OpenVdbPoints.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointGroup.h>
#endif
namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbPointFieldSamplingAvailability, DisabledBeforeOptionsAndIo) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ProcessingOptions p;
  p.maxInputBytes = 0;
  vdb::PointFieldSamplingOptions o;
  o.attributeName = "P";
  try {
    (void)vdb::WriteSampledPointAttribute("missing", "missing", "missing", o,
                                          {}, {}, p);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points = openvdb::points;
using Grid = points::PointDataGrid;
using Op = vdb::PointEditOperation;
using Interp = vdb::PointFieldInterpolation;
class OpenVdbPointFieldSampling : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic_uint64_t serial{0};
    dir = std::filesystem::temp_directory_path() /
          ("container-point-field-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
    std::filesystem::create_directory(dir);
    p.voxelSize = 1;
    p.gridName = "points";
    makePoints({{.25, 0, 0}, {.75, 0, 0}, {1.25, 0, 0}});
    auto scalar = openvdb::DoubleGrid::create(5);
    scalar->setName("field");
    for (int x = -2; x < 5; ++x)
      scalar->tree().setValue({x, 0, 0}, double(x));
    write("field.vdb", {scalar});
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &f : std::filesystem::directory_iterator(dir))
      std::filesystem::remove(f.path(), e);
    std::filesystem::remove(dir, e);
  }
  auto path(const std::string &n) const { return dir / n; }
  void write(const std::string &n, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &fileMetadata = {}) {
    // Fixtures sometimes replace the file they were read from. Point arrays
    // can still reference paged source bytes even with File::open(false), so
    // materialize every payload before truncating that file.
    for (const auto &grid : grids) {
      grid->readNonresidentBuffers();
      if (const auto pointsGrid = openvdb::gridPtrCast<Grid>(grid))
        for (auto leaf = pointsGrid->tree().cbeginLeaf(); leaf; ++leaf)
          for (size_t index = 0; index < leaf->attributeSet().size(); ++index)
            leaf->constAttributeArray(index).loadData();
    }
    openvdb::io::File f(path(n).string());
    f.write(grids, fileMetadata);
  }
  Grid::Ptr read(const std::string &n = "source.vdb") {
    openvdb::io::File f(path(n).string());
    f.open(false);
    auto g = openvdb::gridPtrCast<Grid>(f.readGrid("points"));
    f.close();
    return g;
  }
  static std::string bytes(const std::filesystem::path &file) {
    std::ifstream f(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
  }
  void makePoints(const std::vector<std::array<double, 3>> &positions) {
    std::vector<vdb::PointRecord> records;
    for (const auto &x : positions) {
      vdb::PointRecord r;
      r.position = x;
      records.push_back(r);
    }
    (void)vdb::WritePoints(records, path("raw.vdb"), p);
    const std::array<vdb::PointAttributeEdit, 2> edits{
        {{Op::Add, "id", {}, std::vector<int64_t>{INT64_MAX}},
         {Op::Add, "chosen", {}, std::vector<double>{-0.0}}}};
    const std::array<vdb::PointGroupEdit, 2> groups{
        {{Op::Add, "keep", {}, {1}}, {Op::Add, "none", {}, {0}}}};
    (void)vdb::WritePointAttributeEdits(path("raw.vdb"), path("source.vdb"),
                                        edits, groups, {}, p);
    ASSERT_EQ(std::get<std::vector<int64_t>>(
                  vdb::ReadPointAttribute(path("source.vdb"), "id").values),
              (std::vector<int64_t>{INT64_MAX, INT64_MAX, INT64_MAX}));
    auto g = read();
    g->insertMeta("artist", openvdb::StringMetadata("retained"));
    points::makeDescriptorUnique(g->tree())->getMetadata().insertMeta(
        "note", openvdb::StringMetadata("descriptor"));
    for (auto l = g->tree().beginLeaf(); l; ++l)
      l->attributeArray("chosen").setHidden(true);
    write("source.vdb", {g});
    ASSERT_EQ(std::get<std::vector<int64_t>>(
                  vdb::ReadPointAttribute(path("source.vdb"), "id").values),
              (std::vector<int64_t>{INT64_MAX, INT64_MAX, INT64_MAX}));
  }
  auto sample(vdb::PointFieldSamplingOptions o = {},
              const std::string &out = "out.vdb",
              const std::string &field = "field.vdb",
              const std::string &source = "source.vdb") {
    return vdb::WriteSampledPointAttribute(path(source), path(field), path(out),
                                           o, {}, {}, p);
  }
  template <class T>
  auto column(const std::string &n = "sampled",
              const std::string &file = "out.vdb") {
    return std::get<std::vector<T>>(
        vdb::ReadPointAttribute(path(file), n, {}, p).values);
  }
  std::filesystem::path dir;
  vdb::ProcessingOptions p;
};

TEST_F(OpenVdbPointFieldSampling,
       LinearInactivePointMetadataGroupsAndExactIds) {
  auto source = read();
  source->tree().setActiveState({0, 0, 0}, false);
  write("source.vdb", {source});
  const auto before = bytes(path("source.vdb"));
  const auto result = sample();
  EXPECT_EQ(result.selectedCount, 3u);
  EXPECT_EQ(result.inputCount, 3u);
  EXPECT_EQ(column<double>(), (std::vector<double>{.25, .75, 1.25}));
  const auto target = read("out.vdb");
  EXPECT_EQ(target->transform(), source->transform());
  EXPECT_TRUE(target->tree().hasSameTopology(source->tree()));
  EXPECT_EQ(target->metaValue<std::string>("artist"), "retained");
  EXPECT_EQ(target->tree()
                .cbeginLeaf()
                ->attributeSet()
                .descriptor()
                .getMetadata()
                .metaValue<std::string>("note"),
            "descriptor");
  EXPECT_EQ(column<int64_t>("id"),
            (std::vector<int64_t>{INT64_MAX, INT64_MAX, INT64_MAX}));
  EXPECT_EQ(vdb::ReadPointGroup(path("out.vdb"), "keep"),
            (std::vector<uint8_t>{1, 1, 1}));
  EXPECT_EQ(bytes(path("source.vdb")), before);
}

TEST_F(OpenVdbPointFieldSampling,
       EmptySelectionAddsZeroAndSetPreservesAllNativeBits) {
  vdb::PointFieldSamplingOptions o;
  o.groups.allOf = {"none"};
  auto result = sample(o);
  EXPECT_EQ(result.selectedCount, 0u);
  EXPECT_EQ(column<double>(), (std::vector<double>{0, 0, 0}));
  o.operation = Op::Set;
  o.attributeName = "chosen";
  result = sample(o, "set.vdb");
  for (const auto value : column<double>("chosen", "set.vdb"))
    EXPECT_TRUE(std::signbit(value));
  EXPECT_TRUE(read("set.vdb")
                  ->tree()
                  .cbeginLeaf()
                  ->constAttributeArray("chosen")
                  .isHidden());
}

TEST_F(OpenVdbPointFieldSampling,
       PartialSetPreservesNonfiniteUnselectedAndSignedZero) {
  auto source = read();
  for (auto l = source->tree().beginLeaf(); l; ++l) {
    auto keep = l->groupWriteHandle("keep");
    points::AttributeWriteHandle<double> values(l->attributeArray("chosen"));
    for (openvdb::Index n = 0; n < l->pointCount(); ++n) {
      keep.set(n, n == 0);
      values.set(n, n == 1 ? NAN : -0.0);
    }
  }
  write("source.vdb", {source});
  vdb::PointFieldSamplingOptions o;
  o.attributeName = "chosen";
  o.operation = Op::Set;
  o.groups.anyOf = {"keep"};
  EXPECT_EQ(sample(o).selectedCount, 1u);
  const auto target = read("out.vdb");
  const auto l = target->tree().cbeginLeaf();
  points::AttributeHandle<double> values(l->constAttributeArray("chosen"));
  EXPECT_EQ(values.get(0), .25);
  EXPECT_TRUE(std::isnan(values.get(1)));
  EXPECT_TRUE(std::signbit(values.get(2)));
}

TEST_F(OpenVdbPointFieldSampling, NearestIntegerIdsAndBooleanStayExact) {
  auto ids = openvdb::Int64Grid::create(INT64_MIN);
  ids->setName("ids");
  ids->tree().setValue({0, 0, 0}, INT64_MAX - 1);
  ids->tree().setValue({1, 0, 0}, INT64_MAX);
  write("ids.vdb", {ids});
  vdb::PointFieldSamplingOptions o;
  o.interpolation = Interp::Nearest;
  sample(o, "out.vdb", "ids.vdb");
  EXPECT_EQ(column<int64_t>(),
            (std::vector<int64_t>{INT64_MAX - 1, INT64_MAX, INT64_MAX}));
  o.interpolation = Interp::Linear;
  EXPECT_THROW(sample(o, "bad.vdb", "ids.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  auto flags = openvdb::BoolGrid::create(false);
  flags->setName("flag");
  flags->tree().setValue({1, 0, 0}, true);
  write("flags.vdb", {flags});
  o.interpolation = Interp::Nearest;
  sample(o, "flags-out.vdb", "flags.vdb");
  EXPECT_EQ(column<uint8_t>("sampled", "flags-out.vdb"),
            (std::vector<uint8_t>{0, 1, 1}));
}

TEST_F(OpenVdbPointFieldSampling, QuadraticIndependentPolynomialAndBackground) {
  auto g = openvdb::DoubleGrid::create(0);
  g->setName("polynomial");
  for (int x = -2; x < 5; ++x)
    for (int y = -2; y <= 2; ++y)
      for (int z = -2; z <= 2; ++z)
        g->tree().setValue({x, y, z}, double(x * x + 2 * y * y + 3 * z * z));
  write("quadratic.vdb", {g});
  vdb::PointFieldSamplingOptions o;
  o.interpolation = Interp::Quadratic;
  sample(o, "out.vdb", "quadratic.vdb");
  auto result = column<double>();
  ASSERT_EQ(result.size(), 3u);
  EXPECT_NEAR(result[0], .0625, 1e-14);
  EXPECT_NEAR(result[1], .5625, 1e-14);
  EXPECT_NEAR(result[2], 1.5625, 1e-14);
  g = openvdb::DoubleGrid::create(7);
  g->setName("background");
  write("background.vdb", {g});
  p.maxVoxelCount = 512; // A constant field needs no additional stored voxels.
  sample(o, "background-out.vdb", "background.vdb");
  EXPECT_EQ(column<double>("sampled", "background-out.vdb"),
            (std::vector<double>{7, 7, 7}));
}

TEST_F(OpenVdbPointFieldSampling,
       DoubleExtremeLinearAndExplicitFloatPrecision) {
  auto g = openvdb::DoubleGrid::create(0);
  g->setName("extreme");
  g->tree().setValue({0, 0, 0}, -DBL_MAX);
  g->tree().setValue({1, 0, 0}, DBL_MAX);
  g->tree().setValue({2, 0, 0}, DBL_MAX);
  write("extreme.vdb", {g});
  sample({}, "out.vdb", "extreme.vdb");
  const auto result = column<double>();
  EXPECT_NEAR(result[0] / DBL_MAX, -.5, 1e-15);
  EXPECT_NEAR(result[1] / DBL_MAX, .5, 1e-15);
  auto f = openvdb::FloatGrid::create(0);
  f->setName("float");
  f->tree().setValue({1, 0, 0}, 1.f);
  write("float.vdb", {f});
  vdb::PointFieldSamplingOptions o;
  o.precision = vdb::PointFieldPrecision::PreserveSource;
  sample(o, "float-out.vdb", "float.vdb");
  EXPECT_EQ(column<float>("sampled", "float-out.vdb"),
            (std::vector<float>{.25f, .75f, .75f}));
  o.attributeName = "v";
  o.operation = Op::Set;
  EXPECT_THROW(sample(o, "wrong.vdb", "float.vdb"), std::invalid_argument);
}

TEST_F(OpenVdbPointFieldSampling,
       QuadraticExactIntegerCenterAndExplicitDynamicRangeFailure) {
  std::vector<vdb::PointRecord> records(1);
  (void)vdb::WritePoints(records, path("center.vdb"), p);
  auto g = openvdb::DoubleGrid::create(0);
  g->setName("tiny");
  g->tree().setValue({-1, 0, 0}, DBL_MAX);
  g->tree().setValue({0, 0, 0}, std::numeric_limits<double>::denorm_min());
  write("tiny.vdb", {g});
  vdb::PointFieldSamplingOptions o;
  o.interpolation = Interp::Quadratic;
  sample(o, "out.vdb", "tiny.vdb", "center.vdb");
  EXPECT_EQ(column<double>()[0], std::numeric_limits<double>::denorm_min());
  EXPECT_THROW(sample(o, "range.vdb", "tiny.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("range.vdb")));
}

TEST_F(OpenVdbPointFieldSampling, WorldVectorTagsAndNativeMacOffsetJacobian) {
  auto g = openvdb::Vec3DGrid::create({1, -2, 3});
  g->setName("gradient");
  g->setIsInWorldSpace(true);
  g->setVectorType(openvdb::VEC_COVARIANT);
  write("vector.vdb", {g});
  sample({}, "out.vdb", "vector.vdb");
  EXPECT_EQ(
      (column<std::array<double, 3>>()),
      (std::vector<std::array<double, 3>>{{1, -2, 3}, {1, -2, 3}, {1, -2, 3}}));
  auto m = openvdb::Vec3DGrid::create({0, 0, 0});
  m->setName("MAC");
  m->setGridClass(openvdb::GRID_STAGGERED);
  m->setIsInWorldSpace(false);
  m->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  m->insertMeta("container:mac_layout",
                openvdb::StringMetadata("negative_faces"));
  m->insertMeta("container:velocity_units",
                openvdb::StringMetadata("voxels_per_second"));
  m->setTransform(openvdb::math::Transform::createLinearTransform(2));
  for (int x = -2; x < 4; ++x)
    for (int y = -2; y < 3; ++y)
      for (int z = -2; z < 3; ++z)
        m->tree().setValue({x, y, z}, {double(x), double(y), double(z)});
  write("mac.vdb", {m});
  sample({}, "mac-out.vdb", "mac.vdb");
  const auto values = column<std::array<double, 3>>("sampled", "mac-out.vdb");
  EXPECT_EQ(values[0], (std::array<double, 3>{1.25, 1, 1}));
  vdb::PointFieldSamplingOptions o;
  o.interpolation = Interp::Quadratic;
  EXPECT_THROW(sample(o, "bad.vdb", "mac.vdb"), std::invalid_argument);
}

TEST_F(OpenVdbPointFieldSampling,
       IndependentTransformsAndCompensatedRemotePointPosition) {
  auto source = read();
  source->transform().postTranslate({2, 0, 0});
  write("source.vdb", {source});
  auto field = openvdb::DoubleGrid::create(0);
  field->setName("scaled");
  field->setTransform(openvdb::math::Transform::createLinearTransform(.5));
  for (int x = 0; x < 10; ++x)
    field->tree().setValue({x, 0, 0}, double(x) * .5);
  write("scaled.vdb", {field});
  sample({}, "out.vdb", "scaled.vdb");
  EXPECT_EQ(column<double>(), (std::vector<double>{2.25, 2.75, 3.25}));
  auto remote = source->deepCopy();
  remote->tree().clear(); // Make native remote partition through the public
                          // writer instead.
  std::vector<vdb::PointRecord> records(1);
  records[0].position = {1e9, 0, 0};
  (void)vdb::WritePoints(records, path("remote.vdb"), p);
  auto r = read("remote.vdb");
  auto leaf = r->tree().beginLeaf();
  points::AttributeWriteHandle<openvdb::Vec3f> local(leaf->attributeArray("P"));
  local.set(0, {.1234567f, 0, 0});
  r->transform().postTranslate({-1e9, 0, 0});
  write("remote.vdb", {r});
  sample({}, "remote-out.vdb", "field.vdb", "remote.vdb");
  EXPECT_DOUBLE_EQ(column<double>("sampled", "remote-out.vdb")[0],
                   double(.1234567f));
}

TEST_F(OpenVdbPointFieldSampling,
       WorldPrecisionUsesTheSmallerPointOrFieldAxis) {
  std::vector<vdb::PointRecord> records(1);
  (void)vdb::WritePoints(records, path("single.vdb"), p);
  auto source = read("single.vdb");
  auto leaf = source->tree().beginLeaf();
  points::AttributeWriteHandle<openvdb::Vec3f> positions(
      leaf->attributeArray("P"));
  positions.set(0, {1e-10f, 0, 0});
  source->setTransform(openvdb::math::Transform::createLinearTransform(1e6));
  source->transform().postTranslate({1e12, 0, 0});
  write("large-point.vox.vdb", {source});
  auto field = openvdb::DoubleGrid::create(1);
  field->setName("small");
  field->setTransform(openvdb::math::Transform::createLinearTransform(1e-4));
  field->transform().postTranslate({1e12, 0, 0});
  write("small-field.vdb", {field});
  const auto before = bytes(path("large-point.vox.vdb"));
  // A ~1e-4 point offset rounds to one ~1.22e-4 world ULP. This fits the
  // coarse point voxel tolerance but loses a substantial fine field voxel.
  EXPECT_THROW(
      sample({}, "precision.vdb", "small-field.vdb", "large-point.vox.vdb"),
      std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("precision.vdb")));
  EXPECT_EQ(bytes(path("large-point.vox.vdb")), before);
}

TEST_F(OpenVdbPointFieldSampling, MacWorldProductsRejectLostNonzeroTerms) {
  const double tiny = std::numeric_limits<double>::denorm_min();
  auto field = openvdb::Vec3DGrid::create({tiny, 0, 0});
  field->setName("MAC");
  field->setGridClass(openvdb::GRID_STAGGERED);
  field->setIsInWorldSpace(false);
  field->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  field->insertMeta("container:mac_layout",
                    openvdb::StringMetadata("negative_faces"));
  field->insertMeta("container:velocity_units",
                    openvdb::StringMetadata("voxels_per_second"));
  write("unit-mac.vdb", {field});
  sample({}, "tiny-world.vdb", "unit-mac.vdb");
  for (const auto &value :
       column<std::array<double, 3>>("sampled", "tiny-world.vdb"))
    EXPECT_EQ(value, (std::array<double, 3>{tiny, 0, 0}));
  field->setTransform(openvdb::math::Transform::createLinearTransform(1e-4));
  write("small-mac.vdb", {field});
  const auto before = bytes(path("small-mac.vdb"));
  EXPECT_THROW(sample({}, "underflow.vdb", "small-mac.vdb"),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("underflow.vdb")));
  EXPECT_EQ(bytes(path("small-mac.vdb")), before);
}

TEST_F(OpenVdbPointFieldSampling,
       FileMetadataIsJointlyBoundedAndPointMetadataSurvives) {
  auto source = read();
  auto field = openvdb::DoubleGrid::create(7);
  field->setName("constant");
  openvdb::MetaMap large;
  large.insertMeta("large", openvdb::StringMetadata(std::string(600'000, 'x')));
  write("point-large.vdb", {source}, large);
  EXPECT_THROW(sample({}, "point-limit.vdb", "field.vdb", "point-large.vdb"),
               std::invalid_argument);
  write("field-large.vdb", {field}, large);
  EXPECT_THROW(sample({}, "field-limit.vdb", "field-large.vdb"),
               std::invalid_argument);
  openvdb::MetaMap pointMeta, fieldMeta;
  pointMeta.insertMeta("large",
                       openvdb::StringMetadata(std::string(300'000, 'p')));
  pointMeta.insertMeta("owner", openvdb::StringMetadata("point file"));
  pointMeta.insertMeta("exact_id", openvdb::Int64Metadata(INT64_MAX));
  fieldMeta.insertMeta("large",
                       openvdb::StringMetadata(std::string(300'000, 'f')));
  fieldMeta.insertMeta("owner", openvdb::StringMetadata("field file"));
  write("point-meta.vdb", {source}, pointMeta);
  write("field-meta.vdb", {field}, fieldMeta);
  EXPECT_THROW(
      sample({}, "joint-limit.vdb", "field-meta.vdb", "point-meta.vdb"),
      std::invalid_argument);
  vdb::PointFieldSamplingOptions options;
  options.maxMetadataBytes = 2 * 1024 * 1024;
  const auto before = bytes(path("point-meta.vdb"));
  sample(options, "metadata-out.vdb", "field-meta.vdb", "point-meta.vdb");
  EXPECT_EQ(column<double>("sampled", "metadata-out.vdb"),
            (std::vector<double>{7, 7, 7}));
  openvdb::io::File output(path("metadata-out.vdb").string());
  output.open(false);
  const auto retained = output.getMetadata();
  EXPECT_EQ(retained->metaValue<std::string>("owner"), "point file");
  EXPECT_EQ(retained->metaValue<int64_t>("exact_id"), INT64_MAX);
  EXPECT_EQ(retained->metaValue<std::string>("large"),
            std::string(300'000, 'p'));
  output.close();
  for (const auto *name :
       {"point-limit.vdb", "field-limit.vdb", "joint-limit.vdb"})
    EXPECT_FALSE(std::filesystem::exists(path(name)));
  EXPECT_EQ(bytes(path("point-meta.vdb")), before);
}

TEST_F(OpenVdbPointFieldSampling,
       FullFieldPayloadReservePrecedesDelayedBufferMaterialization) {
  auto field = openvdb::DoubleGrid::create(0);
  field->setName("large");
  for (int x = 0; x < 64; ++x)
    for (int y = 0; y < 64; ++y)
      field->tree().setValue({x * 8, y * 8, 0}, 1.);
  // Persist a seekable file whose stale recorded statistics deliberately
  // underestimate its decoded leaf buffers. The typed topology estimate must
  // reject it before requesting all delayed value pages.
  field->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                    openvdb::Int64Metadata(0));
  field->insertMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT,
                    openvdb::Int64Metadata(4096));
  openvdb::io::File writer(path("stale-stats.vdb").string());
  writer.setGridStatsMetadataEnabled(false);
  writer.write(openvdb::GridPtrVec{field});
  writer.close();
  const auto before = bytes(path("stale-stats.vdb"));
  p.maxInputBytes = 32 * 1024 * 1024;
  try {
    (void)sample({}, "bounded.vdb", "stale-stats.vdb");
    FAIL() << "fully decoded field workspace should exceed the joint budget";
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find("fully decoded sampling field"),
              std::string::npos)
        << error.what();
  }
  EXPECT_FALSE(std::filesystem::exists(path("bounded.vdb")));
  EXPECT_EQ(bytes(path("stale-stats.vdb")), before);
}

TEST_F(OpenVdbPointFieldSampling,
       InvalidFieldValuesSelectionPrecisionAndBudgetsLeaveNoOutput) {
  vdb::PointFieldSamplingOptions o;
  o.groups.anyOf = {"missing"};
  EXPECT_THROW(sample(o), std::invalid_argument);
  o = {};
  o.attributeName = "P";
  EXPECT_THROW(sample(o), std::invalid_argument);
  o = {};
  o.maxWork = 1;
  EXPECT_THROW(sample(o), std::invalid_argument);
  o = {};
  o.maxMetadataBytes = 1;
  EXPECT_THROW(sample(o), std::invalid_argument);
  o = {};
  auto old = p.maxInputBytes;
  p.maxInputBytes = 8 * 1024 * 1024;
  EXPECT_THROW(sample(o), std::invalid_argument);
  p.maxInputBytes = old;
  const auto oldVoxels = p.maxVoxelCount;
  p.maxVoxelCount = 1024; // One point leaf plus two field leaves exceed this.
  EXPECT_THROW(sample(o), std::invalid_argument);
  p.maxVoxelCount = oldVoxels;
  auto g = openvdb::DoubleGrid::create(0);
  g->setName("bad");
  g->tree().setValue({100, 0, 0}, NAN);
  write("bad.vdb", {g});
  EXPECT_THROW(sample(o, "out.vdb", "bad.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  g = openvdb::DoubleGrid::create(1);
  g->setName("far");
  g->setTransform(openvdb::math::Transform::createLinearTransform(1e6));
  g->transform().postTranslate({1e15, 0, 0});
  g->tree().setValue({-1000000000, 0, 0}, 1);
  write("far.vdb", {g});
  EXPECT_THROW(sample(o, "out.vdb", "far.vdb"), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}

TEST_F(OpenVdbPointFieldSampling,
       ExclusiveOutputAndUnsupportedTargetPreserveSource) {
  const auto before = bytes(path("source.vdb"));
  EXPECT_THROW(sample({}, "source.vdb"), std::invalid_argument);
  vdb::PointFieldSamplingOptions o;
  o.attributeName = "chosen";
  EXPECT_THROW(sample(o), std::invalid_argument);
  o.operation = Op::Rename;
  EXPECT_THROW(sample(o), std::invalid_argument);
  o = {};
  sample(o);
  const auto output = bytes(path("out.vdb"));
  EXPECT_THROW(sample(o), std::invalid_argument);
  EXPECT_EQ(bytes(path("out.vdb")), output);
  EXPECT_EQ(bytes(path("source.vdb")), before);
}
#endif
} // namespace
