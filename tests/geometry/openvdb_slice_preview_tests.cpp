#include "Container/geometry/OpenVdbSlicePreview.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <type_traits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Count.h>
#endif
namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbSlicePreviewAvailability, DisabledPrecedesFilesAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::SlicePreviewOptions o;
  o.maxInputBytes = 0;
  o.sampleCounts = {0, 0};
  try {
    (void)vdb::LoadSlicePreviewFromFile("missing", {}, 0, o);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbSlicePreview : public ::testing::Test {
protected:
  std::filesystem::path dir;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{};
    dir = std::filesystem::temp_directory_path() /
          ("container-slice-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(dir));
  }
  void TearDown() override {
    std::error_code e;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
      if (entry.is_regular_file())
        std::filesystem::remove(entry.path(), e);
    std::filesystem::remove(dir, e);
  }
  auto path(const std::string &name) const { return dir / name; }
  void write(const std::string &name, const openvdb::GridPtrVec &g) {
    openvdb::io::File f(path(name).string());
    f.write(g);
    f.close();
  }
  auto bytes(const std::string &name) {
    std::ifstream f(path(name), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), {});
  }
  auto options() {
    vdb::SlicePreviewOptions o;
    o.sampleCounts = {3, 3};
    o.worldExtent = {2, 2};
    o.colorMinimum = -1;
    o.colorMaximum = 1;
    return o;
  }
  std::string property(const container::geometry::dotbim::Model &m,
                       const std::string &name) {
    for (const auto &p : m.elements.at(0).properties)
      if (p.set == "OpenVDB" && p.name == name)
        return p.value;
    return {};
  }
  template <class Grid> void checkRawWorldVectorTags() {
    constexpr std::array<openvdb::VecType, 5> tags{
        openvdb::VEC_INVARIANT, openvdb::VEC_COVARIANT,
        openvdb::VEC_COVARIANT_NORMALIZE, openvdb::VEC_CONTRAVARIANT_RELATIVE,
        openvdb::VEC_CONTRAVARIANT_ABSOLUTE};
    // Nonuniform, sheared, reflected and translated map. Applying J*u,
    // translating a position or normalizing a normal changes every oracle.
    const openvdb::math::Mat4d matrix(.5, .25, 0., 0., 0., -.75, 0., 0., 0., 0.,
                                      1.5, 0., 10., -4., 7., 1.);
    for (const auto tag : tags) {
      SCOPED_TRACE(openvdb::GridBase::vecTypeToString(tag));
      auto grid = Grid::create(typename Grid::ValueType(2, -3, 4));
      grid->setName("world");
      grid->setIsInWorldSpace(true);
      grid->setVectorType(tag);
      grid->setTransform(
          openvdb::math::Transform::createLinearTransform(matrix));
      grid->insertMeta("proof", openvdb::StringMetadata("unchanged vector"));
      for (int z = -2; z <= 2; ++z)
        for (int y = -2; y <= 2; ++y)
          for (int x = -2; x <= 2; ++x)
            grid->tree().setValueOff(
                {x, y, z}, typename Grid::ValueType(2 + x, -3 + 2 * y, 4 + z));
      grid->tree().setActiveState({0, 0, 0}, true);
      const auto filename = "raw-" + std::to_string(int(tag)) + ".vdb";
      write(filename, {grid});
      const auto original = bytes(filename);
      auto o = options();
      o.origin = {10.125, -4.125, 7}; // Absolute index (.25,.25,0).
      o.axisU = {.5, .25, 0};
      o.axisV = {0, -.75, 0};
      o.worldExtent = {std::sqrt(1.25), 1.5};
      o.colorMap = vdb::SliceColorMap::Gray;
      o.colorMinimum = -10;
      o.colorMaximum = 10;
      for (const auto mode :
           {vdb::SliceValue::VectorX, vdb::SliceValue::VectorY,
            vdb::SliceValue::VectorZ, vdb::SliceValue::VectorMagnitude}) {
        SCOPED_TRACE(int(mode));
        o.valueMode = mode;
        const auto model =
            vdb::LoadSlicePreviewFromFile(path(filename), "world", 1, o);
        ASSERT_EQ(model.vertices.size(), 9u);
        EXPECT_EQ(property(model, "Vector semantics"),
                  openvdb::GridBase::vecTypeToString(tag));
        EXPECT_NE(property(model, "World vector interpretation")
                      .find("no Jacobian multiplication, point translation or "
                            "normal normalization"),
                  std::string::npos);
        EXPECT_EQ(
            property(model, "Sampling"),
            (std::is_same_v<Grid, openvdb::Vec3IGrid>
                 ? "Nearest integer center; half ties toward greater index"
                 : "Trilinear native field values"));
        bool copiedMetadata = false;
        for (const auto &p : model.elements[0].properties)
          if (p.set == "OpenVDB metadata" && p.name == "proof")
            copiedMetadata = p.value == "unchanged vector";
        EXPECT_TRUE(copiedMetadata);
        for (int y = 0; y < 3; ++y)
          for (int x = 0; x < 3; ++x) {
            const double qx = x - .75, qy = y - .75;
            const double vx = 2 + (std::is_same_v<Grid, openvdb::Vec3IGrid>
                                       ? std::floor(qx + .5)
                                       : qx);
            const double vy = -3 + 2 * (std::is_same_v<Grid, openvdb::Vec3IGrid>
                                            ? std::floor(qy + .5)
                                            : qy);
            const std::array<double, 3> value{vx, vy, 4};
            const double expected =
                mode == vdb::SliceValue::VectorMagnitude
                    ? std::hypot(vx, vy, 4)
                    : value[int(mode) - int(vdb::SliceValue::VectorX)];
            EXPECT_NEAR(model.vertices[3 * y + x].color.x, (expected + 10) / 20,
                        2e-7);
          }
      }
      EXPECT_EQ(bytes(filename), original);
    }
  }
};
TEST_F(OpenVdbSlicePreview, AllFiveWorldFloatVectorTagsDisplayRawComponents) {
  checkRawWorldVectorTags<openvdb::Vec3SGrid>();
}
TEST_F(OpenVdbSlicePreview, AllFiveWorldDoubleVectorTagsDisplayRawComponents) {
  checkRawWorldVectorTags<openvdb::Vec3DGrid>();
}
TEST_F(OpenVdbSlicePreview, AllFiveWorldIntegerVectorTagsDisplayRawComponents) {
  checkRawWorldVectorTags<openvdb::Vec3IGrid>();
}
TEST_F(OpenVdbSlicePreview,
       EveryLocalSemanticAndMismatchedMacRejectWithoutSourceEdits) {
  auto o = options();
  o.valueMode = vdb::SliceValue::VectorMagnitude;
  constexpr std::array<openvdb::VecType, 5> tags{
      openvdb::VEC_INVARIANT, openvdb::VEC_COVARIANT,
      openvdb::VEC_COVARIANT_NORMALIZE, openvdb::VEC_CONTRAVARIANT_RELATIVE,
      openvdb::VEC_CONTRAVARIANT_ABSOLUTE};
  const auto reject = [&](const std::string &name,
                          const openvdb::GridBase::Ptr &grid) {
    write(name, {grid});
    const auto original = bytes(name);
    EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path(name), {}, 1, o),
                 std::exception);
    EXPECT_EQ(bytes(name), original);
  };
  for (const auto tag : tags) {
    auto f = openvdb::Vec3SGrid::create({2, -3, 4});
    f->setIsInWorldSpace(false);
    f->setVectorType(tag);
    reject("local-f-" + std::to_string(int(tag)) + ".vdb", f);
    auto d = openvdb::Vec3DGrid::create({2, -3, 4});
    d->setIsInWorldSpace(false);
    d->setVectorType(tag);
    reject("local-d-" + std::to_string(int(tag)) + ".vdb", d);
    auto i = openvdb::Vec3IGrid::create({2, -3, 4});
    i->setIsInWorldSpace(false);
    i->setVectorType(tag);
    reject("local-i-" + std::to_string(int(tag)) + ".vdb", i);
  }
  auto mac = openvdb::Vec3DGrid::create({2, -3, 4});
  mac->setGridClass(openvdb::GRID_STAGGERED);
  mac->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  mac->insertMeta("container:mac_layout",
                  openvdb::StringMetadata("negative_faces"));
  mac->insertMeta("container:velocity_units",
                  openvdb::StringMetadata("voxels_per_second"));
  mac->setIsInWorldSpace(true);
  reject("mac-world.vdb", mac);
  mac->setIsInWorldSpace(false);
  mac->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);
  reject("mac-normal.vdb", mac);
  mac->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  mac->removeMeta("container:velocity_units");
  reject("mac-units.vdb", mac);
}
TEST_F(OpenVdbSlicePreview,
       RawWorldSemanticTextSourceAndGeometryShareExistingCaps) {
  auto g = openvdb::Vec3DGrid::create({2, -3, 4});
  g->setName("normal");
  g->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);
  g->setIsInWorldSpace(true);
  g->insertMeta("proof", openvdb::StringMetadata(std::string(4096, 'x')));
  g->tree().setValue({0, 0, 0}, {6, 8, 0});
  write("raw-caps.vdb", {g});
  const auto original = bytes("raw-caps.vdb");
  auto o = options();
  o.valueMode = vdb::SliceValue::VectorMagnitude;
  o.maxMetadataBytes = 1000;
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("raw-caps.vdb"), {}, 1, o),
      std::exception);
  o.maxMetadataBytes = 1'048'576;
  o.maxWork = 1;
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("raw-caps.vdb"), {}, 1, o),
      std::exception);
  o.maxWork = 4'000'000;
  o.maxInputBytes = std::filesystem::file_size(path("raw-caps.vdb"));
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("raw-caps.vdb"), {}, 1, o),
      std::exception);
  EXPECT_EQ(bytes("raw-caps.vdb"), original);
}
TEST_F(OpenVdbSlicePreview,
       ScalarSignedValuesSharedTopologyAndWindingRemainInWorldUnits) {
  auto g = openvdb::FloatGrid::create(3);
  g->setName("phi");
  g->setGridClass(openvdb::GRID_LEVEL_SET);
  for (int z = -2; z <= 2; ++z)
    for (int y = -2; y <= 2; ++y)
      for (int x = -2; x <= 2; ++x)
        g->tree().setValue({x, y, z}, float(x));
  write("sdf.vdb", {g});
  const auto before = bytes("sdf.vdb");
  auto o = options();
  o.colorMap = vdb::SliceColorMap::Diverging;
  const auto m = vdb::LoadSlicePreviewFromFile(path("sdf.vdb"), {}, 2, o);
  ASSERT_EQ(m.vertices.size(), 9u);
  ASSERT_EQ(m.indices.size(), 24u);
  ASSERT_EQ(m.elements.size(), 1u);
  EXPECT_EQ(m.vertices[0].position, glm::vec3(-2, -2, 0));
  EXPECT_EQ(m.vertices[8].position, glm::vec3(2, 2, 0));
  EXPECT_EQ(m.vertices[0].color, glm::vec3(0, 0, 1));
  EXPECT_EQ(m.vertices[1].color, glm::vec3(1));
  EXPECT_EQ(m.vertices[2].color, glm::vec3(1, 0, 0));
  EXPECT_EQ(property(m, "Sample minimum"), "-1");
  EXPECT_EQ(property(m, "Sample maximum"), "1");
  EXPECT_EQ(property(m, "Clipped below"), "0");
  EXPECT_EQ(property(m, "Clipped above"), "0");
  for (size_t i = 0; i < m.indices.size(); i += 3) {
    const auto &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]],
               &c = m.vertices[m.indices[i + 2]];
    EXPECT_GT(
        glm::dot(glm::cross(b.position - a.position, c.position - a.position),
                 a.normal),
        0);
    EXPECT_EQ(a.normal, glm::vec3(0, 0, 1));
  }
  EXPECT_TRUE(m.materials[0].pbr.unlit);
  EXPECT_TRUE(m.materials[0].pbr.doubleSided);
  EXPECT_TRUE(m.elements[0].doubleSided);
  EXPECT_EQ(m.materials[0].pbr.alphaMode,
            container::material::AlphaMode::Opaque);
  EXPECT_EQ(before, bytes("sdf.vdb"));
}
TEST_F(OpenVdbSlicePreview,
       ConstantInactiveBackgroundUsesOnlyExplicitPlaneAndOpacityOnce) {
  auto g = openvdb::DoubleGrid::create(7);
  g->setName("constant");
  write("constant.vdb", {g});
  auto o = options();
  o.origin = {20, 30, 40};
  o.axisU = {2, 0, 0};
  o.axisV = {1, 1, 0};
  o.worldExtent = {4, 2};
  o.colorMinimum = 0;
  o.colorMaximum = 14;
  o.colorMap = vdb::SliceColorMap::Gray;
  o.opacity = .25f;
  const auto m = vdb::LoadSlicePreviewFromFile(path("constant.vdb"), {}, 1, o);
  ASSERT_EQ(m.vertices.size(), 9u);
  for (const auto &v : m.vertices) {
    EXPECT_EQ(v.color, glm::vec3(.5f));
    EXPECT_FLOAT_EQ(v.colorAlpha, 1);
  }
  EXPECT_EQ(m.vertices[4].position, glm::vec3(20, 30, 40));
  EXPECT_EQ(property(m, "Sample minimum"), "7");
  EXPECT_EQ(m.materials[0].pbr.alphaMode,
            container::material::AlphaMode::Blend);
  EXPECT_FLOAT_EQ(m.materials[0].pbr.baseColor.a, .25f);
  EXPECT_FLOAT_EQ(m.materials[0].pbr.opacityFactor, 1);
  EXPECT_FLOAT_EQ(m.elements[0].color.a, 1);
}
TEST_F(OpenVdbSlicePreview,
       AffineScalarSamplingHandlesRotationShearReflectionAndInactiveValues) {
  auto g = openvdb::DoubleGrid::create(0);
  g->setName("linear");
  auto matrix = openvdb::math::Mat4d::identity();
  matrix(0, 0) = 0;
  matrix(0, 1) = -2;
  matrix(1, 0) = 3;
  matrix(1, 1) = .5;
  matrix(2, 2) = .25;
  matrix(3, 0) = 4;
  matrix(3, 1) = -2;
  matrix(3, 2) = 7;
  g->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
  for (int z = -3; z <= 3; ++z)
    for (int y = -3; y <= 3; ++y)
      for (int x = -3; x <= 3; ++x) {
        const auto w = g->indexToWorld(openvdb::Vec3d(x, y, z));
        g->tree().setValueOff({x, y, z}, 2 * w.x() - w.y() + .5 * w.z());
      }
  write("affine.vdb", {g});
  auto o = options();
  o.origin = {4, -2, 7};
  o.colorMap = vdb::SliceColorMap::Gray;
  o.colorMinimum = -30;
  o.colorMaximum = 30;
  const auto m = vdb::LoadSlicePreviewFromFile(path("affine.vdb"), {}, 1, o);
  for (const auto &v : m.vertices) {
    const double value = 2 * v.position.x - v.position.y + .5 * v.position.z;
    EXPECT_NEAR(v.color.x, (value + 30) / 60, 1e-7);
  }
}
TEST_F(OpenVdbSlicePreview,
       WorldVectorsAndNativeNegativeFacesUseDistinctCorrectFrames) {
  auto world = openvdb::Vec3SGrid::create({1, 2, 3});
  world->setName("world");
  world->setIsInWorldSpace(true);
  world->setVectorType(openvdb::VEC_COVARIANT);
  auto map = openvdb::math::Transform::createLinearTransform(.5);
  map->postScale({-1, 1, 1});
  map->postRotate(std::acos(-1.) / 2, openvdb::math::Z_AXIS);
  world->setTransform(map);
  write("world.vdb", {world});
  auto o = options();
  o.valueMode = vdb::SliceValue::VectorX;
  o.colorMinimum = 0;
  o.colorMaximum = 4;
  o.colorMap = vdb::SliceColorMap::Gray;
  auto m = vdb::LoadSlicePreviewFromFile(path("world.vdb"), {}, 1, o);
  for (const auto &v : m.vertices)
    EXPECT_EQ(v.color, glm::vec3(.25f));
  auto mac = openvdb::Vec3DGrid::create({0, 0, 0});
  mac->setName("velocity");
  mac->setTransform(map->copy());
  mac->setGridClass(openvdb::GRID_STAGGERED);
  mac->setIsInWorldSpace(false);
  mac->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  mac->insertMeta("container:mac_layout",
                  openvdb::StringMetadata("negative_faces"));
  mac->insertMeta("container:velocity_units",
                  openvdb::StringMetadata("voxels_per_second"));
  for (int z = -8; z <= 8; ++z)
    for (int y = -8; y <= 8; ++y)
      for (int x = -8; x <= 8; ++x)
        mac->tree().setValue({x, y, z}, openvdb::Vec3d(x - .5, y - .5, z - .5));
  write("mac.vdb", {mac});
  o.colorMinimum = -1;
  o.colorMaximum = 1;
  m = vdb::LoadSlicePreviewFromFile(path("mac.vdb"), {}, 1, o);
  for (const auto &v : m.vertices)
    EXPECT_NEAR(v.color.x, (double(v.position.x) + 1) / 2, 1e-7);
  o.valueMode = vdb::SliceValue::VectorMagnitude;
  o.colorMinimum = 0;
  o.colorMaximum = 2;
  m = vdb::LoadSlicePreviewFromFile(path("mac.vdb"), {}, 1, o);
  for (const auto &v : m.vertices)
    EXPECT_NEAR(v.color.x,
                std::hypot(double(v.position.x), double(v.position.y),
                           double(v.position.z)) /
                    2,
                1e-7);
  mac->removeMeta("container:mac_layout");
  write("untagged.vdb", {mac});
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("untagged.vdb"), {}, 1, o),
      std::exception);
}
TEST_F(OpenVdbSlicePreview,
       RampsClippingEndpointsAndRepeatedGeometryAreDeterministic) {
  auto g = openvdb::FloatGrid::create(0);
  g->setName("values");
  for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x)
        g->tree().setValue({x, y, z}, float(2 * x));
  write("values.vdb", {g});
  auto o = options();
  const auto a = vdb::LoadSlicePreviewFromFile(path("values.vdb"), {}, 1, o),
             b = vdb::LoadSlicePreviewFromFile(path("values.vdb"), {}, 1, o);
  EXPECT_EQ(a.indices, b.indices);
  for (size_t i = 0; i < a.vertices.size(); ++i) {
    EXPECT_EQ(a.vertices[i].position, b.vertices[i].position);
    EXPECT_EQ(a.vertices[i].color, b.vertices[i].color);
  }
  EXPECT_EQ(property(a, "Clipped below"), "3");
  EXPECT_EQ(property(a, "Clipped above"), "3");
  EXPECT_EQ(a.vertices[0].color, glm::vec3(0, 0, 1));
  EXPECT_EQ(a.vertices[1].color, glm::vec3(.5f, 0, .5f));
  EXPECT_EQ(a.vertices[2].color, glm::vec3(1, 0, 0));
  EXPECT_EQ(
      a.importReport.representationWarnings.at("OpenVDB.SliceColorClipped"),
      6u);
}
TEST_F(OpenVdbSlicePreview,
       MacWorldConversionRetainsProductCancellationResiduals) {
  auto grid = openvdb::Vec3DGrid::create(openvdb::Vec3d(8e23, 6e23, 0));
  grid->setName("velocity");
  auto matrix = openvdb::math::Mat4d::identity();
  matrix(0, 0) = .6;
  matrix(0, 1) = .8;
  matrix(1, 0) = -.8;
  matrix(1, 1) = .6;
  grid->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
  grid->setGridClass(openvdb::GRID_STAGGERED);
  grid->setIsInWorldSpace(false);
  grid->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  grid->insertMeta("container:mac_layout",
                   openvdb::StringMetadata("negative_faces"));
  grid->insertMeta("container:velocity_units",
                   openvdb::StringMetadata("voxels_per_second"));
  write("cancel.vdb", {grid});
  auto o = options();
  o.valueMode = vdb::SliceValue::VectorX;
  o.colorMap = vdb::SliceColorMap::Gray;
  o.colorMinimum = -2e8;
  o.colorMaximum = 0;
  const auto m = vdb::LoadSlicePreviewFromFile(path("cancel.vdb"), {}, 1, o);
  // Independently evaluated with Decimal100 from the represented binary64
  // coefficients and components, not their rounded individual products.
  constexpr double expected = -98096012.18500626087;
  EXPECT_NEAR(std::stod(property(m, "Sample minimum")), expected, 1e-7);
  EXPECT_NEAR(std::stod(property(m, "Sample maximum")), expected, 1e-7);
  for (const auto &v : m.vertices)
    EXPECT_NEAR(v.color.x, (expected + 2e8) / 2e8, 1e-7);
}
TEST_F(OpenVdbSlicePreview,
       LimitsInvalidTypesAndPrecisionRejectBeforePublishingGeometry) {
  auto g = openvdb::FloatGrid::create(0);
  g->setName("field");
  g->tree().setValue({0, 0, 0}, 1);
  write("field.vdb", {g});
  auto o = options();
  o.maxSamples = 8;
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("field.vdb"), {}, 1, o),
               std::exception);
  o = options();
  o.maxTriangles = 7;
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("field.vdb"), {}, 1, o),
               std::exception);
  o = options();
  o.maxWork = 1;
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("field.vdb"), {}, 1, o),
               std::exception);
  o = options();
  o.maxInputBytes = 1;
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("field.vdb"), {}, 1, o),
               std::exception);
  o = options();
  o.axisV = o.axisU;
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("field.vdb"), {}, 1, o),
               std::exception);
  o = options();
  o.origin = {1e9, 0, 0};
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("field.vdb"), {}, 1, o),
               std::exception);
  auto farMap = openvdb::math::Transform::createLinearTransform(1e6);
  farMap->postTranslate({1e15, 0, 0});
  g->setTransform(farMap);
  write("source-index.vdb", {g});
  o = options();
  try {
    (void)vdb::LoadSlicePreviewFromFile(path("source-index.vdb"), {}, 1, o);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("source-index/world precision"),
              std::string::npos);
  }
  o = options();
  o.colorMaximum = o.colorMinimum;
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("field.vdb"), {}, 1, o),
               std::exception);
  o = options();
  o.opacity = NAN;
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("field.vdb"), {}, 1, o),
               std::exception);
  auto bad = openvdb::Int32Grid::create(0);
  bad->setName("integer");
  write("integer.vdb", {bad});
  EXPECT_NO_THROW((void)vdb::LoadSlicePreviewFromFile(path("integer.vdb")));
  g->tree().setValueOff({1, 0, 0}, INFINITY);
  write("nonfinite.vdb", {g});
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("nonfinite.vdb")),
               std::exception);
}
TEST_F(OpenVdbSlicePreview, DescriptorStatsMetadataAndSeekabilityAreBounded) {
  auto a = openvdb::DoubleGrid::create(1);
  a->setName("same");
  auto b = openvdb::DoubleGrid::create(2);
  b->setName("same");
  write("duplicates.vdb", {a, b});
  auto o = options();
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("duplicates.vdb"), "same", 1, o),
      std::exception);
  EXPECT_NO_THROW((void)vdb::LoadSlicePreviewFromFile(
      path("duplicates.vdb"), "descriptor:same[0]", 1, o));
  auto c = openvdb::DoubleGrid::create(3);
  c->setName("same[0]");
  write("collision.vdb", {a, b, c});
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("collision.vdb"), {}, 1, o),
      std::exception);
  a->insertMeta("large", openvdb::StringMetadata(std::string(5000, 'x')));
  write("metadata.vdb", {a});
  o.maxMetadataBytes = 1000;
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("metadata.vdb"), {}, 1, o),
      std::exception);
  {
    std::ofstream f(path("stream.vdb"), std::ios::binary);
    openvdb::io::Stream stream(f);
    stream.write({a});
  }
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("stream.vdb")),
               std::exception);
  {
    openvdb::io::File f(path("stats.vdb").string());
    f.setGridStatsMetadataEnabled(false);
    f.write({a});
    f.close();
  }
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("stats.vdb")),
               std::exception);
}
TEST_F(OpenVdbSlicePreview, ExactAdjacentInt64ExtremaRangesTextAndColors) {
  auto g = openvdb::Int64Grid::create(0);
  g->setName("ids");
  const std::array<int64_t, 3> values{INT64_MAX - 2, INT64_MAX - 1, INT64_MAX};
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
      g->tree().setValue({x, y, 0}, values[x + 1]);
  write("ids.vdb", {g});
  const auto before = bytes("ids.vdb");
  auto o = options();
  o.colorMinimum = 0;
  o.colorMaximum = 1;
  o.colorMap = vdb::SliceColorMap::Gray;
  auto m = vdb::LoadSlicePreviewFromFile(path("ids.vdb"), {}, 1, o);
  EXPECT_EQ(property(m, "Sample minimum"), std::to_string(INT64_MAX - 2));
  EXPECT_EQ(property(m, "Sample maximum"), std::to_string(INT64_MAX));
  EXPECT_EQ(property(m, "Exact integer color range"),
            std::to_string(INT64_MAX - 2) + ", " + std::to_string(INT64_MAX));
  for (size_t row = 0; row < 3; ++row) {
    EXPECT_EQ(m.vertices[row * 3].color, glm::vec3(0));
    EXPECT_EQ(m.vertices[row * 3 + 1].color, glm::vec3(.5f));
    EXPECT_EQ(m.vertices[row * 3 + 2].color, glm::vec3(1));
  }
  EXPECT_EQ(property(m, "Sampling"),
            "Nearest integer center; half ties toward greater index");
  o.integerRange = std::array<int64_t, 2>{INT64_MAX - 1, INT64_MAX};
  m = vdb::LoadSlicePreviewFromFile(path("ids.vdb"), {}, 1, o);
  EXPECT_EQ(property(m, "Clipped below"), "3");
  EXPECT_EQ(property(m, "Clipped above"), "0");
  EXPECT_EQ(m.vertices[1].color, glm::vec3(0));
  EXPECT_EQ(m.vertices[2].color, glm::vec3(1));
  EXPECT_EQ(before, bytes("ids.vdb"));
  const std::array<int64_t, 3> extremes{INT64_MIN, 0, INT64_MAX};
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
      g->tree().setValue({x, y, 0}, extremes[x + 1]);
  write("extremes.vdb", {g});
  o.integerRange = std::array<int64_t, 2>{INT64_MIN, INT64_MAX};
  m = vdb::LoadSlicePreviewFromFile(path("extremes.vdb"), {}, 1, o);
  EXPECT_EQ(property(m, "Sample minimum"), "-9223372036854775808");
  EXPECT_EQ(property(m, "Sample maximum"), "9223372036854775807");
  EXPECT_EQ(m.vertices[0].color, glm::vec3(0));
  EXPECT_EQ(m.vertices[1].color, glm::vec3(.5f));
  EXPECT_EQ(m.vertices[2].color, glm::vec3(1));
  auto constant = openvdb::Int64Grid::create(INT64_MIN + 1);
  constant->setName("constant");
  write("constant-id.vdb", {constant});
  o.integerRange.reset();
  m = vdb::LoadSlicePreviewFromFile(path("constant-id.vdb"), {}, 1, o);
  EXPECT_EQ(property(m, "Sample minimum"), "-9223372036854775807");
  for (const auto &v : m.vertices)
    EXPECT_EQ(v.color, glm::vec3(.5f));
}

TEST_F(OpenVdbSlicePreview,
       BoolStoredInactiveValuesAndMaskActivityHaveDistinctSemantics) {
  auto boolean = openvdb::BoolGrid::create(false);
  boolean->setName("bool");
  auto mask = openvdb::MaskGrid::create(false);
  mask->setName("mask");
  for (int y = -1; y <= 1; ++y) {
    boolean->tree().setValueOn({-1, y, 0}, false);
    boolean->tree().setValueOff({0, y, 0}, true);
    boolean->tree().setValueOn({1, y, 0}, true);
    mask->tree().setValueOn({-1, y, 0});
    mask->tree().setValueOff({0, y, 0});
    mask->tree().setValueOn({1, y, 0});
  }
  write("bool.vdb", {boolean});
  write("mask.vdb", {mask});
  auto o = options();
  o.colorMinimum = 0;
  o.colorMaximum = 1;
  o.colorMap = vdb::SliceColorMap::Gray;
  const auto b = vdb::LoadSlicePreviewFromFile(path("bool.vdb"), {}, 1, o),
             m = vdb::LoadSlicePreviewFromFile(path("mask.vdb"), {}, 1, o);
  EXPECT_EQ(property(b, "Exact integer color range"), "0, 1");
  EXPECT_EQ(property(m, "Exact integer color range"), "0, 1");
  for (size_t row = 0; row < 3; ++row) {
    EXPECT_EQ(b.vertices[row * 3].color, glm::vec3(0));
    EXPECT_EQ(b.vertices[row * 3 + 1].color, glm::vec3(1));
    EXPECT_EQ(b.vertices[row * 3 + 2].color, glm::vec3(1));
    EXPECT_EQ(m.vertices[row * 3].color, glm::vec3(1));
    EXPECT_EQ(m.vertices[row * 3 + 1].color, glm::vec3(0));
    EXPECT_EQ(m.vertices[row * 3 + 2].color, glm::vec3(1));
  }
}

TEST_F(OpenVdbSlicePreview, AdjacentDoubleMidpointsRetainExactNearestCell) {
  constexpr int64_t base = 9007199254740992LL;
  auto ids = openvdb::Int64Grid::create(base);
  auto vectors = openvdb::Vec3IGrid::create(openvdb::Vec3i(0));
  auto boolean = openvdb::BoolGrid::create(true);
  auto mask = openvdb::MaskGrid::create(false);
  ids->setName("ids"); vectors->setName("vectors");
  boolean->setName("boolean"); mask->setName("mask");
  for (int z = 0; z <= 1; ++z)
    for (int y = 0; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) {
        const openvdb::Coord c(x, y, z);
        ids->tree().setValue(c, base + x);
        vectors->tree().setValue(c, openvdb::Vec3i(x + 1, 0, 0));
        if (x == 0) {
          boolean->tree().setValueOn(c, false);
          mask->tree().setValueOn(c);
        } else {
          boolean->tree().setValueOff(c, true);
          mask->tree().setValueOff(c);
        }
      }
  write("midpoints.vdb", {ids, vectors, boolean, mask});
  const auto original = bytes("midpoints.vdb");
  const std::array<std::pair<double, int>, 6> cases{{
      {std::nextafter(-.5, -1.), -1}, {-.5, 0},
      {std::nextafter(-.5, 0.), 0},
      {std::nextafter(.5, 0.), 0}, {.5, 1},
      {std::nextafter(.5, 1.), 1}}};
  for (const auto &[coordinate, index] : cases) {
    SCOPED_TRACE(coordinate);
    auto o = options();
    o.origin = {coordinate, .5, .5};
    o.axisU = {0, 1, 0}; o.axisV = {0, 0, 1};
    o.worldExtent = {1, 1}; o.sampleCounts = {2, 2};
    o.colorMap = vdb::SliceColorMap::Gray;
    o.colorMinimum = 0; o.colorMaximum = 1;
    o.integerRange = std::array<int64_t, 2>{base - 1, base + 1};
    const auto integer = vdb::LoadSlicePreviewFromFile(path("midpoints.vdb"), "ids", 1, o);
    EXPECT_EQ(property(integer, "Sample minimum"), std::to_string(base + index));
    for (const auto &vertex : integer.vertices)
      EXPECT_EQ(vertex.color, glm::vec3(float(index + 1) / 2));
    o.integerRange.reset();
    const auto b = vdb::LoadSlicePreviewFromFile(path("midpoints.vdb"), "boolean", 1, o);
    const auto m = vdb::LoadSlicePreviewFromFile(path("midpoints.vdb"), "mask", 1, o);
    for (const auto &vertex : b.vertices)
      EXPECT_EQ(vertex.color, glm::vec3(index == 0 ? 0 : 1));
    for (const auto &vertex : m.vertices)
      EXPECT_EQ(vertex.color, glm::vec3(index == 0 ? 1 : 0));
    o.valueMode = vdb::SliceValue::VectorX; o.colorMaximum = 2;
    const auto v = vdb::LoadSlicePreviewFromFile(path("midpoints.vdb"), "vectors", 1, o);
    for (const auto &vertex : v.vertices)
      EXPECT_EQ(vertex.color, glm::vec3(float(index + 1) / 2));
  }
  EXPECT_EQ(bytes("midpoints.vdb"), original);
}

TEST_F(OpenVdbSlicePreview, NegativeNearestTiesAndFullInt32VectorComponents) {
  auto g = openvdb::Int32Grid::create(0);
  g->setName("signed");
  for (int y = 0; y <= 1; ++y)
    for (int x = -2; x <= 2; ++x)
      g->tree().setValue({x, y, 0}, x);
  write("ties.vdb", {g});
  auto o = options();
  o.colorMinimum = 0;
  o.colorMaximum = 1;
  o.colorMap = vdb::SliceColorMap::Gray;
  // Binary-exact plane positions avoid an unrelated thirds-rounding offset
  // from placing an intended half tie slightly below the boundary.
  o.sampleCounts = {3, 2};
  o.worldExtent = {2, 1};
  o.origin = {-.5, 0, 0};
  o.integerRange = std::array<int64_t, 2>{-1, 2};
  const auto m = vdb::LoadSlicePreviewFromFile(path("ties.vdb"), {}, 1, o);
  for (size_t row = 0; row < 2; ++row)
    for (size_t x = 0; x < 3; ++x) {
      EXPECT_FLOAT_EQ(m.vertices[row * 3 + x].position.x, float(x) - 1.5f);
      EXPECT_FLOAT_EQ(m.vertices[row * 3 + x].color.x, float(x) / 3);
    }
  auto vectors =
      openvdb::Vec3IGrid::create(openvdb::Vec3i(INT32_MIN, INT32_MAX, -7));
  vectors->setName("vector");
  write("vector-int.vdb", {vectors});
  o = options();
  o.colorMap = vdb::SliceColorMap::Gray;
  o.valueMode = vdb::SliceValue::VectorX;
  o.colorMinimum = INT32_MIN;
  o.colorMaximum = INT32_MAX;
  auto v = vdb::LoadSlicePreviewFromFile(path("vector-int.vdb"), {}, 1, o);
  EXPECT_EQ(property(v, "Sample minimum"), "-2147483648");
  for (const auto &vertex : v.vertices)
    EXPECT_EQ(vertex.color, glm::vec3(0));
  o.valueMode = vdb::SliceValue::VectorY;
  v = vdb::LoadSlicePreviewFromFile(path("vector-int.vdb"), {}, 1, o);
  EXPECT_EQ(property(v, "Sample maximum"), "2147483647");
  for (const auto &vertex : v.vertices)
    EXPECT_EQ(vertex.color, glm::vec3(1));
  o.valueMode = vdb::SliceValue::VectorMagnitude;
  o.colorMinimum = 0;
  o.colorMaximum = 4e9;
  v = vdb::LoadSlicePreviewFromFile(path("vector-int.vdb"), {}, 1, o);
  const double expected = std::hypot(double(INT32_MIN), double(INT32_MAX), -7.);
  EXPECT_NEAR(std::stod(property(v, "Sample minimum")), expected, 1e-6);
  EXPECT_NEAR(v.vertices[0].color.x, expected / 4e9, 1e-7);
}

TEST_F(OpenVdbSlicePreview,
       DiscreteRangesModesAndClassesRejectIrrelevantControls) {
  auto integer = openvdb::Int64Grid::create(7);
  integer->setName("ids");
  auto floating = openvdb::FloatGrid::create(1);
  floating->setName("float");
  auto vector = openvdb::Vec3IGrid::create({1, 2, 3});
  vector->setName("vector");
  write("integer-options.vdb", {integer});
  write("float-options.vdb", {floating});
  write("vector-options.vdb", {vector});
  auto o = options();
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("integer-options.vdb"),
                                                   {}, 1, o),
               std::exception);
  o.colorMinimum = 0;
  o.colorMaximum = 1;
  o.integerRange = std::array<int64_t, 2>{7, 7};
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("integer-options.vdb"),
                                                   {}, 1, o),
               std::exception);
  o.integerRange = std::array<int64_t, 2>{8, 7};
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("integer-options.vdb"),
                                                   {}, 1, o),
               std::exception);
  o.integerRange = std::array<int64_t, 2>{0, 10};
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("float-options.vdb"), {}, 1, o),
      std::exception);
  o.valueMode = vdb::SliceValue::VectorX;
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("vector-options.vdb"), {}, 1, o),
      std::exception);
  o.integerRange.reset();
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("integer-options.vdb"),
                                                   {}, 1, o),
               std::exception);
  integer->setGridClass(openvdb::GRID_LEVEL_SET);
  write("integer-class.vdb", {integer});
  o.valueMode = vdb::SliceValue::Scalar;
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("integer-class.vdb"), {}, 1, o),
      std::exception);
}

TEST_F(OpenVdbSlicePreview,
       UnicodeSnapshotAggregateFileScannedMetadataAndFullPayloadBudgets) {
  auto g = openvdb::Int64Grid::create(INT64_MAX);
  g->setName("ids");
  write("unicode-source.vdb", {g});
  const auto unicode = dir / std::filesystem::path(L"\u5207\u7247-\u03b4.vdb");
  std::filesystem::copy_file(path("unicode-source.vdb"), unicode);
  const auto before = bytes("unicode-source.vdb");
  auto o = options();
  o.colorMinimum = 0;
  o.colorMaximum = 1;
  EXPECT_NO_THROW((void)vdb::LoadSlicePreviewFromFile(unicode, {}, 1, o));
  EXPECT_EQ(before, bytes("unicode-source.vdb"));
  openvdb::MetaMap fileMetadata;
  fileMetadata.insertMeta("file-payload",
                          openvdb::StringMetadata(std::string(6000, 'x')));
  {
    openvdb::io::File f(path("file-meta.vdb").string());
    f.write({g}, fileMetadata);
    f.close();
  }
  o.maxMetadataBytes = 10000;
  EXPECT_THROW(
      (void)vdb::LoadSlicePreviewFromFile(path("file-meta.vdb"), {}, 1, o),
      std::exception);
  auto other = openvdb::Int32Grid::create(1);
  other->setName("unused");
  other->insertMeta("unselected-payload",
                    openvdb::StringMetadata(std::string(6000, 'x')));
  write("scanned-meta.vdb", {g, other});
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("scanned-meta.vdb"),
                                                   "ids", 1, o),
               std::exception);
  o = options();
  o.colorMinimum = 0;
  o.colorMaximum = 1;
  auto dense = openvdb::Int64Grid::create(0);
  dense->setName("dense");
  for (int x = 0; x < 96; ++x)
    for (int y = 0; y < 96; ++y)
      for (int z = 0; z < 96; ++z)
        dense->tree().setValue({x, y, z}, x + y + z);
  dense->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                    openvdb::Int64Metadata(1));
  dense->insertMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT,
                    openvdb::Int64Metadata(1));
  {
    openvdb::io::File f(path("full-buffer.vdb").string());
    f.setGridStatsMetadataEnabled(false);
    f.write({dense});
    f.close();
  }
  const auto full = openvdb::tools::memUsageIfLoaded(dense->tree(), false);
  o.maxInputBytes = std::filesystem::file_size(path("full-buffer.vdb")) +
                    1024 * 1024 + full / 2 + 128 * 1024;
  try {
    (void)vdb::LoadSlicePreviewFromFile(path("full-buffer.vdb"), {}, 1, o);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("before leaf materialization"),
              std::string::npos);
  }
  o = options();
  o.colorMinimum = 0;
  o.colorMaximum = 1;
  o.maxActiveVoxels = 10;
  try {
    (void)vdb::LoadSlicePreviewFromFile(path("full-buffer.vdb"), {}, 1, o);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("loaded topology"), std::string::npos);
  }
  auto inactive = openvdb::Int64Grid::create(0);
  inactive->setName("inactive");
  inactive->tree().setValueOff({0, 0, 0}, 1);
  inactive->tree().setValueOff({32, 0, 0}, 2);
  write("inactive-leaves.vdb", {inactive});
  o.maxActiveVoxels = 512;
  EXPECT_THROW((void)vdb::LoadSlicePreviewFromFile(path("inactive-leaves.vdb"),
                                                   {}, 1, o),
               std::exception);
  struct Environment {
    bool present = false;
    std::string saved;
    Environment() {
      if (const auto *old = std::getenv("OPENVDB_DISABLE_DELAYED_LOAD")) {
        present = true;
        saved = old;
      }
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", "1");
#else
      setenv("OPENVDB_DISABLE_DELAYED_LOAD", "1", 1);
#endif
    }
    ~Environment() {
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD", saved.c_str());
#else
      if (present)
        setenv("OPENVDB_DISABLE_DELAYED_LOAD", saved.c_str(), 1);
      else
        unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
    }
  } environment;
  try {
    (void)vdb::LoadSlicePreviewFromFile(path("unicode-source.vdb"), {}, 1, o);
    FAIL();
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("delayed loading"), std::string::npos);
  }
}
#endif
} // namespace
