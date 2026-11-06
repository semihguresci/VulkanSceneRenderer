#include "Container/geometry/OpenVdbVectorPreview.h"
#include <atomic>
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
TEST(OpenVdbVectorPreviewAvailability, DisabledFailsBeforeOptionsAndFilesystem) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::VectorPreviewOptions o;
  o.maxSeeds = 0;
  try {
    (void)vdb::LoadVectorPreviewFromFile("missing.vdb", {}, -1, o);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbVectorPreview : public ::testing::Test {
protected:
  using V = openvdb::Vec3d;
  std::filesystem::path directory;
  bool ownsDirectory{false};
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
                ("container-vector-preview-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++serial));
    ownsDirectory=std::filesystem::create_directory(directory);
    ASSERT_TRUE(ownsDirectory);
  }
  void TearDown() override {
    if (!ownsDirectory) return;
    std::error_code error;
    const auto resolved=std::filesystem::weakly_canonical(directory,error);
    if (error || !resolved.filename().string().starts_with("container-vector-preview-") ||
        !std::filesystem::equivalent(resolved.parent_path(),std::filesystem::temp_directory_path(),error) || error) {
      ADD_FAILURE() << "refusing cleanup outside the owned vector preview temporary directory";
      return;
    }
    std::filesystem::remove_all(resolved,error);
  }
  std::filesystem::path path(const char *name = "input.vdb") const { return directory / name; }
  void write(const openvdb::GridPtrVec &grids, const char *name = "input.vdb", bool stats = true) {
    openvdb::io::File file(path(name).string());
    file.setGridStatsMetadataEnabled(stats);
    file.write(grids);
    file.close();
  }
  static openvdb::Vec3DGrid::Ptr field(V background = V(1, 0, 0)) {
    auto grid = openvdb::Vec3DGrid::create(background);
    grid->setName("wind");
    grid->setIsInWorldSpace(true);
    return grid;
  }
  static vdb::VectorPreviewOptions options() {
    vdb::VectorPreviewOptions o;
    o.hasBounds = true;
    o.boundsMin = {-.1, -.1, -.1};
    o.boundsMax = {.1, .1, .1};
    o.seedCounts = {1, 1, 1};
    o.worldRadius = .005;
    return o;
  }
  static bool warning(const container::geometry::dotbim::Model &model, const char *name) {
    return model.importReport.representationWarnings.contains(name);
  }
  static V position(const container::geometry::Vertex &v) {
    return V(v.position.x, v.position.y, v.position.z);
  }
  static void tip(const container::geometry::dotbim::Model &model, V expected,
                  double tolerance = 1e-5) {
    bool found = false;
    for (const auto &vertex : model.vertices)
      found |= (position(vertex) - expected).length() < tolerance;
    EXPECT_TRUE(found) << expected;
  }
  static void geometry(const container::geometry::dotbim::Model &model) {
    ASSERT_EQ(model.elements.size(), 1u);
    ASSERT_EQ(model.meshRanges.size(), 1u);
    EXPECT_EQ(model.elements[0].geometryKind, container::geometry::dotbim::GeometryKind::Mesh);
    const auto &range = model.meshRanges[0];
    for (const auto &vertex : model.vertices) {
      EXPECT_TRUE(std::isfinite(vertex.position.x));
      EXPECT_NEAR(glm::length(vertex.normal), 1, 1e-5);
      EXPECT_LE(glm::distance(vertex.position, range.boundsCenter), range.boundsRadius + 1e-6f);
    }
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      const auto &a = model.vertices.at(model.indices[i]);
      const auto &b = model.vertices.at(model.indices[i + 1]);
      const auto &c = model.vertices.at(model.indices[i + 2]);
      EXPECT_GT(glm::dot(glm::cross(b.position - a.position, c.position - a.position), a.normal),
                0);
    }
  }
};

TEST_F(OpenVdbVectorPreview,
       ConstantFieldRequiresExplicitBoundsAndGlyphsHaveSignedWorldDisplacement) {
  auto wind = field();
  write({wind});
  vdb::VectorPreviewOptions automatic;
  EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path(), "wind", 1, automatic),
               std::invalid_argument);
  auto o = options();
  const auto model = vdb::LoadVectorPreviewFromFile(path(), "wind", 1, o);
  EXPECT_EQ(model.indices.size() / 3, 36u);
  tip(model, V(.1, 0, 0));
  geometry(model);
  for (const auto &vertex : model.vertices) {
    EXPECT_FLOAT_EQ(vertex.color.r, 1);
    EXPECT_FLOAT_EQ(vertex.color.g, .5);
    EXPECT_FLOAT_EQ(vertex.color.b, .5);
  }
  wind = field(V(-1, 0, 0));
  write({wind}, "negative.vdb");
  const auto negative = vdb::LoadVectorPreviewFromFile(path("negative.vdb"), {}, 2, o);
  tip(negative, V(-.2, 0, 0));
  EXPECT_FLOAT_EQ(negative.vertices[0].color.r, 0);
}

TEST_F(OpenVdbVectorPreview, AffineShearedReflectedFloatAndDoubleFieldsSampleWorldComponents) {
  for (bool floats : {false, true}) {
    auto transform = openvdb::math::Transform::createLinearTransform(.25);
    transform->postShear(.3, openvdb::math::X_AXIS, openvdb::math::Y_AXIS);
    transform->postRotate(.45, openvdb::math::Z_AXIS);
    transform->postScale(V(-1, 1, 1));
    transform->postTranslate(V(.5, -.25, .125));
    const V seed = transform->indexToWorld(V(.25, .5, .75));
    const auto expected = [](V p) { return V(1 + .2 * p.x(), .3 + .1 * p.y(), -.2 + .1 * p.z()); };
    openvdb::GridBase::Ptr generic;
    if (floats) {
      auto g = openvdb::Vec3SGrid::create(openvdb::Vec3f(0));
      g->setTransform(transform);
      g->setIsInWorldSpace(true);
      g->setVectorType(openvdb::VEC_COVARIANT);
      g->setName("wind");
      for (int x = -3; x <= 3; ++x)
        for (int y = -3; y <= 3; ++y)
          for (int z = -3; z <= 3; ++z)
            g->tree().setValue({x, y, z}, openvdb::Vec3f(expected(g->indexToWorld(V(x, y, z)))));
      generic = g;
    } else {
      auto g = field(V(0));
      g->setTransform(transform);
      g->setVectorType(openvdb::VEC_COVARIANT);
      for (int x = -3; x <= 3; ++x)
        for (int y = -3; y <= 3; ++y)
          for (int z = -3; z <= 3; ++z)
            g->tree().setValue({x, y, z}, expected(g->indexToWorld(V(x, y, z))));
      generic = g;
    }
    write({generic}, floats ? "float.vdb" : "double.vdb");
    auto o = options();
    for (size_t a = 0; a < 3; ++a) {
      o.boundsMin[a] = seed[a] - .01;
      o.boundsMax[a] = seed[a] + .01;
    }
    const auto model =
        vdb::LoadVectorPreviewFromFile(path(floats ? "float.vdb" : "double.vdb"), {}, 1, o);
    tip(model, seed + expected(seed) * o.displaySeconds);
    geometry(model);
  }
}

TEST_F(OpenVdbVectorPreview, NativeMacUsesNegativeFacesThenJacobianWorldConversion) {
  auto g = field(V(0));
  auto t = openvdb::math::Transform::createLinearTransform(.5);
  t->postRotate(.4, openvdb::math::Z_AXIS);
  t->postScale(V(-1, 1, 1));
  g->setTransform(t);
  g->setGridClass(openvdb::GRID_STAGGERED);
  g->setIsInWorldSpace(false);
  g->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  g->insertMeta("container:mac_layout", openvdb::StringMetadata("negative_faces"));
  g->insertMeta("container:velocity_units", openvdb::StringMetadata("voxels_per_second"));
  for (int x = -2; x <= 2; ++x)
    for (int y = -2; y <= 2; ++y)
      for (int z = -2; z <= 2; ++z)
        g->tree().setValue({x, y, z}, V(x - .5, y - .5, z - .5));
  write({g});
  auto o = options();
  const V index(.25, .5, .75), seed = t->indexToWorld(index);
  for (size_t a = 0; a < 3; ++a) {
    o.boundsMin[a] = seed[a] - .01;
    o.boundsMax[a] = seed[a] + .01;
  }
  const auto model = vdb::LoadVectorPreviewFromFile(path(), {}, 1, o);
  tip(model, seed + t->baseMap()->applyJacobian(index) * o.displaySeconds);
  geometry(model);
  g->removeMeta("container:mac_layout");
  write({g}, "untagged.vdb");
  EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path("untagged.vdb"), {}, 1, o),
               std::invalid_argument);
}

TEST_F(OpenVdbVectorPreview, ZeroAndLengthCappedGlyphsAreExplicitAndColorMagnitudeIsDeterministic) {
  auto g = field(V(0));
  write({g});
  auto o = options();
  auto model = vdb::LoadVectorPreviewFromFile(path(), {}, 1, o);
  EXPECT_TRUE(model.vertices.empty());
  EXPECT_TRUE(warning(model, "OpenVDB.VectorZero"));
  g = field(V(100, 0, 0));
  write({g}, "fast.vdb");
  o.maxGlyphLength = .2;
  o.colorMode = vdb::PointPreviewColor::VelocityMagnitude;
  o.velocityScale = 10;
  model = vdb::LoadVectorPreviewFromFile(path("fast.vdb"), {}, 1, o);
  tip(model, V(.2, 0, 0));
  EXPECT_TRUE(warning(model, "OpenVDB.VectorGlyphShortened"));
  EXPECT_FLOAT_EQ(model.vertices[0].color.r, 1);
  EXPECT_FLOAT_EQ(model.vertices[0].color.g, .2f);
  EXPECT_FLOAT_EQ(model.vertices[0].color.b, .05f);
  const auto again = vdb::LoadVectorPreviewFromFile(path("fast.vdb"), {}, 1, o);
  ASSERT_EQ(model.vertices.size(), again.vertices.size());
  for (size_t i = 0; i < model.vertices.size(); ++i) {
    EXPECT_EQ(model.vertices[i].position, again.vertices[i].position);
    EXPECT_EQ(model.vertices[i].color, again.vertices[i].color);
  }
}

TEST_F(OpenVdbVectorPreview, StraightStreamlinesClipAtWorldBoundsAndReverseDirection) {
  write({field()});
  auto o = options();
  o.mode = vdb::VectorPreviewMode::Streamlines;
  o.boundsMin = {-1, -.1, -.1};
  o.boundsMax = {1, .1, .1};
  o.worldStep = .1;
  o.maxSteps = 64;
  const auto model = vdb::LoadVectorPreviewFromFile(path(), {}, 1, o);
  geometry(model);
  EXPECT_TRUE(warning(model, "OpenVDB.VectorClipped"));
  tip(model, V(1, 0, 0));
  tip(model, V(-1, 0, 0));
  for (const auto &v : model.vertices) {
    EXPECT_GE(v.position.x, -1.00001);
    EXPECT_LE(v.position.x, 1.00001);
  }
  o.bothDirections = false;
  o.maxSteps = 2;
  const auto limited = vdb::LoadVectorPreviewFromFile(path(), {}, 1, o);
  EXPECT_TRUE(warning(limited, "OpenVDB.VectorStepLimit"));
  EXPECT_EQ(limited.indices.size() / 3, 48u);
}

TEST_F(OpenVdbVectorPreview, RequestedWorldRadiusSurvivesShortSegmentsAndImportScale) {
  write({field()});
  auto o = options();
  o.mode = vdb::VectorPreviewMode::Streamlines;
  o.boundsMin = {-.2, -.2, -.2};
  o.boundsMax = {.2, .2, .2};
  o.worldStep = .05;
  o.worldRadius = .04;
  o.maxSteps = 4;
  o.bothDirections = false;
  const auto tube = vdb::LoadVectorPreviewFromFile(path(), {}, 2, o);
  geometry(tube);
  double maxY = 0;
  for (const auto &vertex : tube.vertices)
    maxY = std::max(maxY, std::abs(double(vertex.position.y)));
  EXPECT_NEAR(maxY, .08, 1e-7);

  o.mode = vdb::VectorPreviewMode::Glyphs;
  o.displaySeconds = .1;
  o.worldRadius = .2;
  const auto glyph = vdb::LoadVectorPreviewFromFile(path(), {}, 2, o);
  geometry(glyph);
  maxY = 0;
  for (const auto &vertex : glyph.vertices)
    maxY = std::max(maxY, std::abs(double(vertex.position.y)));
  EXPECT_NEAR(maxY, .8, 1e-7); // The arrow head spans twice the requested radius.
}

TEST_F(OpenVdbVectorPreview, RotationalFieldStopsReturnToSeedCycles) {
  auto g = field(V(0));
  g->setTransform(openvdb::math::Transform::createLinearTransform(.25));
  for (int x = -12; x <= 12; ++x)
    for (int y = -12; y <= 12; ++y)
      for (int z = -1; z <= 1; ++z)
        g->tree().setValue({x, y, z}, V(-y * .25, x * .25, 0));
  write({g});
  auto o = options();
  o.mode = vdb::VectorPreviewMode::Streamlines;
  o.boundsMin = {-2, -2, -.1};
  o.boundsMax = {2, 2, .1};
  o.seedCounts = {2, 1, 1};
  o.maxSteps = 200;
  o.bothDirections = false;
  const auto model = vdb::LoadVectorPreviewFromFile(path(), {}, 1, o);
  EXPECT_TRUE(warning(model, "OpenVDB.VectorCycle"));
  EXPECT_FALSE(warning(model, "OpenVDB.VectorStepLimit"));
  geometry(model);
  for (const auto &v : model.vertices)
    EXPECT_NEAR(std::hypot(v.position.x, v.position.y), 1, .02);
}

TEST_F(OpenVdbVectorPreview, OptionalFluidMaskClipsLinesAtSolidWallsAndPreservesSourceBytes) {
  auto g = field();
  auto mask = openvdb::BoolGrid::create(false);
  mask->setName("fluid");
  mask->tree().setValue({0, 0, 0}, true);
  mask->tree().setValue({1, 0, 0}, true);
  write({g, mask});
  std::ifstream beforeStream(path(), std::ios::binary);
  const std::string before{std::istreambuf_iterator<char>(beforeStream), {}};
  beforeStream.close();
  auto o = options();
  o.mode = vdb::VectorPreviewMode::Streamlines;
  o.domainGrid = "fluid";
  o.boundsMin = {-.4, -.1, -.1};
  o.boundsMax = {2.4, .1, .1};
  o.bothDirections = false;
  o.worldStep = .1;
  const auto model = vdb::LoadVectorPreviewFromFile(path(), "wind", 1, o);
  geometry(model);
  EXPECT_TRUE(warning(model, "OpenVDB.VectorClipped"));
  tip(model, V(1.5, 0, 0));
  for (const auto &v : model.vertices)
    EXPECT_LE(v.position.x, 1.50001);
  std::ifstream afterStream(path(), std::ios::binary);
  EXPECT_EQ((std::string{std::istreambuf_iterator<char>(afterStream), {}}), before);
}

TEST_F(OpenVdbVectorPreview, RemoteFluidConnectionCannotLeakVectorStencilThroughSolidCorner) {
  auto g = field(V(0));
  g->tree().setValue({1, 1, 0}, V(100, 0, 0));
  auto mask = openvdb::BoolGrid::create(false);
  mask->setName("fluid");
  for (int x = 0; x < 2; ++x)
    for (int y = 0; y < 2; ++y)
      mask->tree().setValue({x, y, 0}, true);
  for (int x = 2; x < 4; ++x)
    for (int y = 2; y < 4; ++y)
      mask->tree().setValue({x, y, 0}, true);
  // Connect the two blocks remotely, so a global component test alone cannot
  // prevent the (1,1) stencil value crossing solid (1,2)/(2,1).
  for (int y = 0; y <= 4; ++y)
    mask->tree().setValue({-1, y, 0}, true);
  for (int x = -1; x <= 3; ++x)
    mask->tree().setValue({x, 4, 0}, true);
  write({g, mask});
  auto o = options();
  o.boundsMin = {1.89, 1.89, -.01};
  o.boundsMax = {1.91, 1.91, .01};
  const auto unconstrained = vdb::LoadVectorPreviewFromFile(path(), "wind", 1, o);
  EXPECT_FALSE(unconstrained.vertices.empty());
  o.domainGrid = "fluid";
  const auto clipped = vdb::LoadVectorPreviewFromFile(path(), "wind", 1, o);
  EXPECT_TRUE(clipped.vertices.empty());
  EXPECT_TRUE(warning(clipped, "OpenVDB.VectorZero"));
}

TEST_F(OpenVdbVectorPreview, FarSourceIndexCannotLoseRepresentableWorldSamplingFractions) {
  auto g = field(V(0));
  auto map = openvdb::math::Transform::createLinearTransform(1e6);
  map->postTranslate(V(1e15, 0, 0));
  g->setTransform(map);
  g->tree().setValue({-1'000'000'001, 0, 0}, V(-1e12, 0, 0));
  g->tree().setValue({-1'000'000'000, 0, 0}, V(0));
  g->tree().setValue({-999'999'999, 0, 0}, V(1e12, 0, 0));
  write({g});
  EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path(), {}, 1, options()),
               std::invalid_argument);
}

TEST_F(OpenVdbVectorPreview, FloatSubnormalInterpolationStillDefinesAVisibleStreamlineDirection) {
  auto g = openvdb::Vec3SGrid::create(openvdb::Vec3f(0));
  g->setName("wind");
  g->setIsInWorldSpace(true);
  g->tree().setValue({0, 0, 0}, openvdb::Vec3f(std::numeric_limits<float>::denorm_min(), 0, 0));
  write({g});
  auto o = options();
  o.mode = vdb::VectorPreviewMode::Streamlines;
  o.boundsMin = {.74, -.01, -.01};
  o.boundsMax = {.76, .01, .01};
  o.worldStep = .005;
  o.maxSteps = 10;
  o.bothDirections = false;
  const auto model = vdb::LoadVectorPreviewFromFile(path(), {}, 1, o);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_TRUE(warning(model, "OpenVDB.VectorClipped"));
  geometry(model);
}

TEST_F(OpenVdbVectorPreview, InvalidLimitsTypesStatisticsAndPrecisionFailWithoutOutput) {
  auto g = field();
  write({g});
  const auto fail = [&](vdb::VectorPreviewOptions o) {
    EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path(), {}, 1, o), std::exception);
  };
  {
    auto o = options();
    o.seedCounts = {256, 256, 256};
    fail(o);
  }
  {
    auto o = options();
    o.maxWork = 1;
    fail(o);
  }
  {
    auto o = options();
    o.maxTriangles = 35;
    fail(o);
  }
  {
    auto o = options();
    o.maxInputBytes = 1;
    fail(o);
  }
  {
    auto o = options();
    o.worldRadius = 0;
    fail(o);
  }
  {
    auto o = options();
    o.maxSteps = 0;
    fail(o);
  }
  {
    auto o = options();
    o.boundsMax = o.boundsMin;
    fail(o);
  }
  {
    auto o = options();
    o.boundsMin = {1e12, -.1, -.1};
    o.boundsMax = {1e12 + 1, .1, .1};
    fail(o);
  }
  g->tree().setValueOff({100, 0, 0}, V(std::numeric_limits<double>::infinity(), 0, 0));
  write({g}, "bad.vdb");
  EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path("bad.vdb"), {}, 1, options()),
               std::invalid_argument);
  write({field()}, "no-stats.vdb", false);
  EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path("no-stats.vdb"), {}, 1, options()),
               std::invalid_argument);
  auto scalar = openvdb::FloatGrid::create();
  scalar->setName("scalar");
  write({scalar}, "scalar.vdb");
  EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path("scalar.vdb"), {}, 1, options()),
               std::invalid_argument);
}

TEST_F(OpenVdbVectorPreview, ExactDescriptorsAndCollidingNamesAreHandledBeforeLookup) {
  auto a = field(), b = field(V(0, 1, 0));
  a->setName("field");
  b->setName("field");
  write({a, b});
  EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path(), "field", 1, options()),
               std::invalid_argument);
  const auto model = vdb::LoadVectorPreviewFromFile(path(), "descriptor:field[1]", 1, options());
  EXPECT_FALSE(model.vertices.empty());
  auto literal = field();
  literal->setName("field[0]");
  write({a, b, literal}, "collision.vdb");
  EXPECT_THROW((void)vdb::LoadVectorPreviewFromFile(path("collision.vdb"), {}, 1, options()),
               std::invalid_argument);
}
#endif
} // namespace
