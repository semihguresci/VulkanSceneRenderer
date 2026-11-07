#include "Container/geometry/OpenVdbRegions.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "../../src/geometry/OpenVdbTransformUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/tools/LevelSetSphere.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbRegionsAvailability, DisabledBeforeOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::RegionsOptions o;
  o.maxComponents = 0;
  vdb::ProcessingOptions p;
  p.maxInputBytes = 0;
  try {
    (void)vdb::WriteRegions("missing", "missing",
                            vdb::RegionsOperation::Cavities, o, {}, p);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
template <class G> class ExactNativeFixture : public G {
public:
  explicit ExactNativeFixture(const G &g)
      : G(openvdb::ConstPtrCast<typename G::TreeType>(g.constTreePtr())) {
    this->insertMeta(g);
    this->setTransform(vdb::detail::CopyTransformExact(g.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, vdb::detail::CopyTransformExact(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactNativeFixture &>(*this).copyGrid());
  }
};
class OpenVdbRegions : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-regions-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    p.gridName = "region";
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &f : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(f.path(), e);
    std::filesystem::remove(directory, e);
  }
  auto path(const std::string &n) const { return directory / n; }
  void write(const std::string &n, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &meta = {}) {
    openvdb::io::File f(path(n).string());
    f.setCompression(openvdb::io::COMPRESS_ZIP);
    f.write(grids, meta);
  }
  std::vector<openvdb::BoolGrid::Ptr> masks(const std::string &n) {
    openvdb::io::File f(path(n).string());
    f.open(false);
    auto all = f.getGrids();
    std::vector<openvdb::BoolGrid::Ptr> out;
    for (auto &g : *all) {
      EXPECT_TRUE(g->isType<openvdb::BoolGrid>());
      out.push_back(openvdb::gridPtrCast<openvdb::BoolGrid>(g));
    }
    return out;
  }
  static std::string fileBytes(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
  }
  auto run(vdb::RegionsOperation op, const std::string &source = "input.vdb",
           const std::string &out = "output.vdb",
           const std::string &selector = {}) {
    return vdb::WriteRegions(path(source), path(out), op, o, selector, p);
  }
  // Exact analytic signed shell, dense enough to cross every surface but with
  // inactive clamped exterior/interior. No CSG/rebuild topology assumptions.
  template <class G>
  typename G::Ptr shell(bool opening = false, bool torus = false) {
    using T = typename G::ValueType;
    auto g = G::create(T(3));
    g->setName("shape");
    g->setGridClass(openvdb::GRID_LEVEL_SET);
    auto a = g->getAccessor();
    for (int x = -12; x <= 12; ++x)
      for (int y = -12; y <= 12; ++y)
        for (int z = -12; z <= 12; ++z) {
          const double r = std::hypot(double(x), double(y), double(z));
          double d = torus ? std::hypot(std::hypot(double(x), double(y)) - 6.,
                                        double(z)) -
                                 2.
                           : std::max(4. - r, r - 8.);
          if (opening)
            d = std::max(d, 1.5 - std::hypot(double(y), double(z)));
          const double value = std::clamp(d, -3., 3.);
          const auto c = openvdb::Coord(x, y, z);
          if (std::abs(d) < 3)
            a.setValue(c, T(value));
          else
            a.setValueOff(c, T(value));
        }
    return g;
  }
  std::filesystem::path directory;
  vdb::RegionsOptions o;
  vdb::ProcessingOptions p;
};
TEST_F(OpenVdbRegions,
       ComponentsUseActivityFaceConnectivityAndDeterministicOrdering) {
  auto g = openvdb::DoubleGrid::create(17);
  g->setName("values");
  auto a = g->getAccessor();
  a.setValue({0, 0, 0}, 0);
  a.setValue({1, 0, 0}, std::numeric_limits<double>::quiet_NaN());
  a.setValue({4, 4, 4}, 0);
  a.setValue({5, 5, 5}, 0);
  a.setValueOff({3, 3, 3}, 99);
  write("input.vdb", {g});
  const auto before = fileBytes(path("input.vdb"));
  auto result = run(vdb::RegionsOperation::ActiveComponents);
  ASSERT_EQ(result.grids.size(), 3u);
  EXPECT_EQ(result.totalActiveVoxelCount, 4u);
  EXPECT_EQ(result.grids[0].gridName, "region_000");
  EXPECT_EQ(result.grids[0].activeVoxelCount, 2u);
  auto m = masks("output.vdb");
  ASSERT_EQ(m.size(), 3u);
  EXPECT_TRUE(m[0]->tree().getValue({1, 0, 0}));
  EXPECT_FALSE(m[0]->tree().getValue({3, 3, 3}));
  EXPECT_TRUE(m[1]->tree().getValue({4, 4, 4}));
  EXPECT_TRUE(m[2]->tree().getValue({5, 5, 5}));
  EXPECT_EQ(fileBytes(path("input.vdb")), before);
}
TEST_F(OpenVdbRegions, AllNineNativeTypesAndActiveFalseMaskValues) {
  auto check = [&]<class G>(const std::string &name) {
    auto g = G::create();
    g->setName(name);
    g->tree().setValueOn({0, 0, 0});
    g->tree().setValueOn({0, 0, 1});
    write(name + ".vdb", {g});
    auto r = run(vdb::RegionsOperation::ActiveComponents, name + ".vdb",
                 name + "-out.vdb");
    EXPECT_EQ(r.totalActiveVoxelCount, 2u);
    auto m = masks(name + "-out.vdb");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_TRUE(m[0]->tree().getValue({0, 0, 0}));
    EXPECT_FALSE(m[0]->tree().getValue({1, 1, 1}));
  };
  check.operator()<openvdb::FloatGrid>("f");
  check.operator()<openvdb::DoubleGrid>("d");
  check.operator()<openvdb::BoolGrid>("b");
  check.operator()<openvdb::MaskGrid>("m");
  check.operator()<openvdb::Int32Grid>("i");
  check.operator()<openvdb::Int64Grid>("l");
  check.operator()<openvdb::Vec3IGrid>("vi");
  check.operator()<openvdb::Vec3SGrid>("vf");
  check.operator()<openvdb::Vec3DGrid>("vd");
}
TEST_F(OpenVdbRegions,
       ActiveTilesConnectAcrossLeafBoundariesAndPreflightExpansion) {
  auto g = openvdb::FloatGrid::create();
  g->setName("tiled");
  g->tree().fill({{0, 0, 0}, {7, 7, 7}}, 0, true);
  g->tree().setValue({8, 0, 0}, 0);
  ASSERT_TRUE(g->tree().hasActiveTiles());
  write("input.vdb", {g});
  auto r = run(vdb::RegionsOperation::ActiveComponents);
  ASSERT_EQ(r.grids.size(), 1u);
  EXPECT_EQ(r.totalActiveVoxelCount, 513u);
  auto m = masks("output.vdb");
  EXPECT_FALSE(m[0]->tree().hasActiveTiles());
  p.maxVoxelCount = 512;
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents, "input.vdb",
                         "limited.vdb"),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("limited.vdb")));
}
TEST_F(OpenVdbRegions,
       HollowSphereInteriorEnclosedAndCavityAreDifferentAnalyticSets) {
  write("input.vdb", {shell<openvdb::FloatGrid>()});
  auto a =
      run(vdb::RegionsOperation::SignedInterior, "input.vdb", "inside.vdb");
  auto b =
      run(vdb::RegionsOperation::EnclosedRegion, "input.vdb", "enclosed.vdb");
  auto c = run(vdb::RegionsOperation::Cavities, "input.vdb", "cavity.vdb");
  auto in = masks("inside.vdb")[0], en = masks("enclosed.vdb")[0],
       ca = masks("cavity.vdb")[0];
  EXPECT_FALSE(in->tree().getValue({0, 0, 0}));
  EXPECT_TRUE(in->tree().getValue({6, 0, 0}));
  EXPECT_TRUE(en->tree().getValue({0, 0, 0}));
  EXPECT_TRUE(en->tree().getValue({6, 0, 0}));
  EXPECT_FALSE(en->tree().getValue({10, 0, 0}));
  EXPECT_TRUE(ca->tree().getValue({0, 0, 0}));
  EXPECT_FALSE(ca->tree().getValue({6, 0, 0}));
  EXPECT_FALSE(ca->tree().getValue({10, 0, 0}));
  EXPECT_EQ(b.totalActiveVoxelCount,
            a.totalActiveVoxelCount + c.totalActiveVoxelCount);
  EXPECT_GT(c.totalActiveVoxelCount, 100u);
  for (int x = -10; x <= 10; ++x)
    for (int y = -10; y <= 10; ++y)
      for (int z = -10; z <= 10; ++z) {
        const double radius = std::hypot(double(x), double(y), double(z));
        const openvdb::Coord index(x, y, z);
        EXPECT_EQ(in->tree().isValueOn(index), radius > 4 && radius < 8);
        EXPECT_EQ(en->tree().isValueOn(index), radius < 8);
        EXPECT_EQ(ca->tree().isValueOn(index), radius <= 4);
        EXPECT_EQ(ca->tree().getValue(index), ca->tree().isValueOn(index));
      }
}
TEST_F(OpenVdbRegions, OpenShellAndTorusHoleRemainExteriorRatherThanCavities) {
  write("open.vdb", {shell<openvdb::DoubleGrid>(true)});
  auto a = run(vdb::RegionsOperation::Cavities, "open.vdb", "open-out.vdb");
  EXPECT_EQ(a.totalActiveVoxelCount, 0u);
  EXPECT_FALSE(a.diagnostics.empty());
  write("torus.vdb", {shell<openvdb::DoubleGrid>(false, true)});
  auto b = run(vdb::RegionsOperation::Cavities, "torus.vdb", "torus-out.vdb");
  EXPECT_EQ(b.totalActiveVoxelCount, 0u);
  EXPECT_FALSE(masks("torus-out.vdb")[0]->tree().getValue({0, 0, 0}));
}
TEST_F(OpenVdbRegions,
       InactiveNegativeTilesBelongToInteriorButNotActiveComponents) {
  auto g = openvdb::DoubleGrid::create(3);
  g->setName("tiled-sdf");
  g->setGridClass(openvdb::GRID_LEVEL_SET);
  g->tree().fill({{0, 0, 0}, {7, 7, 7}}, -3, false);
  write("input.vdb", {g});
  auto a = run(vdb::RegionsOperation::SignedInterior);
  EXPECT_EQ(a.totalActiveVoxelCount, 512u);
  EXPECT_TRUE(masks("output.vdb")[0]->tree().getValue({4, 4, 4}));
  auto b =
      run(vdb::RegionsOperation::ActiveComponents, "input.vdb", "empty.vdb");
  EXPECT_EQ(b.totalActiveVoxelCount, 0u);
  EXPECT_EQ(b.grids.size(), 1u);
}
TEST_F(OpenVdbRegions, FullyInactiveStoredLeafCannotCrashNativeEnclosedHelper) {
  auto g = shell<openvdb::DoubleGrid>();
  g->tree().setValueOff({24, 0, 0}, 3);
  write("input.vdb", {g});
  auto r = run(vdb::RegionsOperation::Cavities);
  EXPECT_GT(r.totalActiveVoxelCount, 100u);
  EXPECT_TRUE(masks("output.vdb")[0]->tree().getValue({0, 0, 0}));
}
TEST_F(OpenVdbRegions,
       ExactAffineAndGridFileMetadataAreRetainedWhileStructureIsRegenerated) {
  auto g = openvdb::FloatGrid::create();
  g->setName("original");
  g->tree().setValue({0, 0, 0}, 1);
  g->insertMeta("artist", openvdb::StringMetadata("unchanged"));
  g->insertMeta("container:mac_layout", openvdb::StringMetadata("stale"));
  openvdb::math::Mat4d matrix = openvdb::math::Mat4d::identity();
  matrix[0][0] = .1;
  matrix[1][1] = -.2;
  matrix[2][2] = .3;
  matrix[0][1] = 1e-12;
  matrix[3][0] = 1e12 + .125;
  g->setTransform(vdb::detail::MakeAffineTransformExact(matrix));
  openvdb::MetaMap meta;
  meta.insertMeta("project", openvdb::Int64Metadata(INT64_MAX));
  write("input.vdb", {g}, meta);
  run(vdb::RegionsOperation::ActiveComponents);
  auto m = masks("output.vdb")[0];
  EXPECT_EQ(m->getGridClass(), openvdb::GRID_UNKNOWN);
  EXPECT_EQ(m->getVectorType(), openvdb::VEC_INVARIANT);
  EXPECT_TRUE(m->isInWorldSpace());
  EXPECT_EQ(m->metaValue<std::string>("artist"), "unchanged");
  EXPECT_FALSE(m->getMetadata<openvdb::StringMetadata>("container:mac_layout"));
  auto map = m->transform().baseMap();
  for (int axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d e(0);
    e[axis] = 1;
    EXPECT_EQ(map->applyJacobian(e),
              g->transform().baseMap()->applyJacobian(e));
  }
  EXPECT_EQ(map->applyMap(openvdb::Vec3d(0)),
            g->transform().baseMap()->applyMap(openvdb::Vec3d(0)));
  openvdb::io::File output(path("output.vdb").string());
  output.open(false);
  EXPECT_EQ(output.getMetadata()->metaValue<int64_t>("project"), INT64_MAX);
}
TEST_F(OpenVdbRegions,
       ComponentCapAndSharedWorkspaceWorkMetadataLimitsLeaveNoOutput) {
  auto g = openvdb::BoolGrid::create();
  g->setName("specks");
  for (int x = 0; x < 8; x += 2)
    for (int y = 0; y < 8; y += 2)
      for (int z = 0; z < 8; z += 2)
        g->tree().setValueOn({x, y, z});
  write("input.vdb", {g});
  o.maxComponents = 3;
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("output.vdb")));
  o.maxComponents = 64;
  o.maxWorkItems = 1;
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents),
               std::invalid_argument);
  o.maxWorkItems = 200'000'000;
  p.maxInputBytes = 4 * 1024 * 1024;
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents),
               std::invalid_argument);
  p.maxInputBytes = 512ull * 1024 * 1024;
  openvdb::MetaMap meta;
  meta.insertMeta("large", openvdb::StringMetadata(std::string(4096, 'x')));
  write("metadata.vdb", {g}, meta);
  o.maxMetadataBytes = 1024;
  EXPECT_THROW(
      (void)run(vdb::RegionsOperation::ActiveComponents, "metadata.vdb"),
      std::invalid_argument);
}
TEST_F(OpenVdbRegions,
       TinyNativeDistanceBandsAndHugeOriginDoNotChangeIndexThresholds) {
  auto g = openvdb::DoubleGrid::create(1e-320);
  g->setName("tiny");
  g->setGridClass(openvdb::GRID_LEVEL_SET);
  g->tree().setValue({0, 0, 0}, -1e-320);
  g->tree().setValue({1, 0, 0}, 1e-320);
  // Native constructors reject a determinant below an absolute tolerance even
  // though persisted maps admit this finite invertible scale. Author its public
  // record directly, independently from the module's exact-copy path.
  std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
  openvdb::writeString(record,
                       openvdb::math::UniformScaleTranslateMap::mapType());
  openvdb::Vec3d(1e12, 0, 0).write(record);
  for (const double value : {1e-6, 1e-6, 1e6, 1e12, 5e5})
    openvdb::Vec3d(value).write(record);
  record.seekg(0);
  openvdb::io::setCurrentVersion(record);
  g->transform().read(record);
  write("input.vdb",
        {std::make_shared<ExactNativeFixture<openvdb::DoubleGrid>>(*g)});
  const auto r = run(vdb::RegionsOperation::SignedInterior);
  EXPECT_EQ(r.totalActiveVoxelCount, 1u);
  ASSERT_EQ(r.grids.size(), 1u);
  EXPECT_EQ(r.grids[0].voxelSize, 1e-6);
  const auto m = masks("output.vdb")[0];
  EXPECT_TRUE(m->tree().getValue({0, 0, 0}));
  EXPECT_FALSE(m->tree().getValue({1, 0, 0}));
  openvdb::Vec3d x(1, 0, 0);
  EXPECT_EQ(m->transform().baseMap()->applyJacobian(x),
            g->transform().baseMap()->applyJacobian(x));
  EXPECT_EQ(m->transform().baseMap()->applyMap(openvdb::Vec3d(0)),
            g->transform().baseMap()->applyMap(openvdb::Vec3d(0)));
  EXPECT_EQ(run(vdb::RegionsOperation::Cavities, "input.vdb", "cavity.vdb")
                .totalActiveVoxelCount,
            0u);
}
TEST_F(OpenVdbRegions,
       WrongTypedReservedMetadataAndExactPointArchiveAreRegenerated) {
  auto g = openvdb::BoolGrid::create();
  g->setName("shape");
  g->tree().setValueOn({0, 0, 0});
  for (auto key :
       {openvdb::GridBase::META_GRID_CLASS, openvdb::GridBase::META_VECTOR_TYPE,
        openvdb::GridBase::META_IS_LOCAL_SPACE, "container:regions_operation",
        "container:pressure_units", "container:point_empty_manifest_v1",
        "container:point_empty_descriptor_v1",
        "container:point_empty_leaf_flags_v1"})
    g->insertMeta(key, openvdb::Int32Metadata(4));
  g->insertMeta("container:point_empty_note",
                openvdb::StringMetadata("ordinary custom metadata"));
  write("input.vdb", {g});
  run(vdb::RegionsOperation::ActiveComponents);
  const auto m = masks("output.vdb")[0];
  EXPECT_EQ(m->getGridClass(), openvdb::GRID_UNKNOWN);
  EXPECT_EQ(m->getVectorType(), openvdb::VEC_INVARIANT);
  EXPECT_TRUE(m->isInWorldSpace());
  EXPECT_FALSE((*m)["container:pressure_units"]);
  EXPECT_FALSE((*m)["container:point_empty_manifest_v1"]);
  EXPECT_FALSE((*m)["container:point_empty_descriptor_v1"]);
  EXPECT_FALSE((*m)["container:point_empty_leaf_flags_v1"]);
  EXPECT_EQ(m->metaValue<std::string>("container:regions_operation"),
            "active_components");
  EXPECT_EQ(m->metaValue<std::string>("container:point_empty_note"),
            "ordinary custom metadata");
}
TEST_F(OpenVdbRegions,
       CoordinateMarginsAndConservativeRemoteDomainFailBeforeExpansion) {
  auto g = openvdb::FloatGrid::create();
  g->setName("far");
  g->tree().setValue({0, 0, 0}, 1);
  g->tree().setValue({1'000'000, 0, 0}, 1);
  write("input.vdb", {g});
  const auto before = fileBytes(path("input.vdb"));
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("output.vdb")));
  EXPECT_EQ(fileBytes(path("input.vdb")), before);
  auto edgeGrid = openvdb::FloatGrid::create();
  edgeGrid->setName("edge");
  edgeGrid->tree().setValue({INT32_MAX - 1, 0, 0}, 1);
  write("edge.vdb", {edgeGrid});
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents, "edge.vdb",
                         "edge-out.vdb"),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("edge-out.vdb")));
}
TEST_F(OpenVdbRegions,
       AggregateOutputLeafSlotsAreBoundedBeforeNativeSegmentation) {
  auto g = openvdb::BoolGrid::create();
  g->setName("one-leaf");
  for (int x : {1, 3, 5})
    for (int y : {1, 3, 5})
      for (int z : {1, 3, 5})
        g->tree().setValueOn({x, y, z});
  ASSERT_EQ(g->tree().leafCount(), 1u);
  write("input.vdb", {g});
  p.maxVoxelCount = 512;
  try {
    (void)run(vdb::RegionsOperation::ActiveComponents);
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_NE(
        std::string(e.what()).find("combined prospective component leaf slots"),
        std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("output.vdb")));
  p.maxVoxelCount = 27 * 512;
  const auto r = run(vdb::RegionsOperation::ActiveComponents);
  EXPECT_EQ(r.grids.size(), 27u);
  EXPECT_EQ(r.totalActiveVoxelCount, 27u);
}
TEST_F(OpenVdbRegions,
       SelectorsDescriptorCollisionsAndUnsupportedClassesReject) {
  auto a = openvdb::FloatGrid::create();
  a->setName("duplicate");
  a->tree().setValue({0, 0, 0}, 0);
  auto b = a->deepCopy();
  b->tree().setValue({4, 0, 0}, 0);
  write("input.vdb", {a, b});
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents),
               std::invalid_argument);
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents, "input.vdb",
                         "named.vdb", "duplicate"),
               std::invalid_argument);
  openvdb::io::File catalog(path("input.vdb").string());
  catalog.open(false);
  std::string descriptor;
  for (auto it = catalog.beginName(); it != catalog.endName(); ++it)
    if (catalog.readGridMetadata(it.gridName())
            ->getMetadata<openvdb::Int64Metadata>(
                openvdb::GridBase::META_FILE_VOXEL_COUNT)
            ->value() == 1) {
      descriptor = it.gridName();
      break;
    }
  ASSERT_FALSE(descriptor.empty());
  auto r = run(vdb::RegionsOperation::ActiveComponents, "input.vdb",
               "chosen.vdb", "descriptor:" + descriptor);
  EXPECT_EQ(r.totalActiveVoxelCount, 1u);
  auto c = a->deepCopy();
  c->setName("duplicate[0]");
  write("collision.vdb", {a, b, c});
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents,
                         "collision.vdb", "collision-out.vdb"),
               std::invalid_argument);
  write("fog.vdb", {a});
  EXPECT_THROW(
      (void)run(vdb::RegionsOperation::Cavities, "fog.vdb", "wrong.vdb"),
      std::invalid_argument);
}
TEST_F(OpenVdbRegions,
       EmptyFieldsInvalidSignedDataAndOptionsHaveExplicitBehavior) {
  auto empty = openvdb::DoubleGrid::create(3);
  empty->setName("empty");
  empty->setGridClass(openvdb::GRID_LEVEL_SET);
  write("input.vdb", {empty});
  EXPECT_EQ(run(vdb::RegionsOperation::ActiveComponents).totalActiveVoxelCount,
            0u);
  EXPECT_EQ(run(vdb::RegionsOperation::Cavities, "input.vdb", "cavity.vdb")
                .totalActiveVoxelCount,
            0u);
  empty->tree().setValue({0, 0, 0}, std::numeric_limits<double>::quiet_NaN());
  write("nan.vdb", {empty});
  EXPECT_THROW(
      (void)run(vdb::RegionsOperation::SignedInterior, "nan.vdb", "bad.vdb"),
      std::invalid_argument);
  o.isovalue = 1;
  EXPECT_THROW((void)run(vdb::RegionsOperation::ActiveComponents, "input.vdb",
                         "ignored.vdb"),
               std::invalid_argument);
  o.isovalue = 0;
  o.maxComponents = 1;
  EXPECT_THROW(
      (void)run(vdb::RegionsOperation::Cavities, "input.vdb", "ignored.vdb"),
      std::invalid_argument);
  o.maxComponents = 64;
  auto f = shell<openvdb::FloatGrid>();
  write("float.vdb", {f});
  o.isovalue = .1;
  EXPECT_THROW((void)run(vdb::RegionsOperation::SignedInterior, "float.vdb",
                         "rounded.vdb"),
               std::invalid_argument);
}
TEST_F(OpenVdbRegions,
       UnicodeNativePathsProtectedOutputsAndSourcePreservation) {
  auto g = openvdb::FloatGrid::create();
  g->setName("native");
  g->tree().setValue({0, 0, 0}, 1);
  write("input.vdb", {g});
  const auto input = directory / std::filesystem::path(u"entrée_球.vdb"),
             output = directory / std::filesystem::path(u"régions_球.vdb");
  std::filesystem::copy_file(path("input.vdb"), input);
  const auto before = fileBytes(input);
  auto r = vdb::WriteRegions(input, output,
                             vdb::RegionsOperation::ActiveComponents, o, {}, p);
  EXPECT_EQ(r.totalActiveVoxelCount, 1u);
  EXPECT_EQ(fileBytes(input), before);
  const auto outputBefore = fileBytes(output);
  EXPECT_THROW((void)vdb::WriteRegions(input, output,
                                       vdb::RegionsOperation::ActiveComponents,
                                       o, {}, p),
               std::invalid_argument);
  EXPECT_EQ(fileBytes(output), outputBefore);
  EXPECT_THROW((void)vdb::WriteRegions(input, input,
                                       vdb::RegionsOperation::ActiveComponents,
                                       o, {}, p),
               std::invalid_argument);
  EXPECT_EQ(fileBytes(input), before);
}
#endif
} // namespace
