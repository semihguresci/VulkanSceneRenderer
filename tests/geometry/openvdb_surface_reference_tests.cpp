#include "Container/geometry/OpenVdbSurfaceReference.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/io.h>
#include <openvdb/openvdb.h>
#include <tinyexr.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
using Kind = vdb::SdfRayHitKind;
using Normal = vdb::RayNormalStatus;
using Shade = vdb::SurfaceShading;

TEST(OpenVdbSurfaceReferenceAvailability, RenderOffBeforeCameraOptionsAndPath) {
  EXPECT_EQ(vdb::SurfaceReferenceOptions{}.rays.limits.maxWorkItems,
            2'000'000'000u);
  EXPECT_EQ(vdb::RayQueryLimits{}.maxWorkItems, 200'000'000u);
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::SurfaceReferenceCamera camera;
  camera.target = camera.eye;
  vdb::SurfaceReferenceOptions options;
  options.width = 0;
  options.rays.limits.maxWorkItems = 0;
  vdb::ProcessingOptions processing;
  processing.maxInputBytes = 0;
  try {
    (void)vdb::RenderSurfaceReference("missing.vdb", camera, options, {},
                                      processing);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

TEST(OpenVdbSurfaceReferenceAvailability, WriterOffBeforeCallerImageAndPath) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  try {
    (void)vdb::WriteSurfaceReferenceExr({}, {}, 0);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
template <class G> class ExactReferenceFixture : public G {
public:
  explicit ExactReferenceFixture(const G &grid)
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
        static_cast<const ExactReferenceFixture &>(*this).copyGrid());
  }
};

struct ReferenceExr {
  EXRHeader header;
  EXRImage image;
  const char *error = nullptr;
  ReferenceExr() {
    InitEXRHeader(&header);
    InitEXRImage(&image);
  }
  ~ReferenceExr() {
    FreeEXRImage(&image);
    FreeEXRHeader(&header);
    if (error)
      FreeEXRErrorMessage(error);
  }
  void read(const std::string &encoded) {
    const auto *data = reinterpret_cast<const unsigned char *>(encoded.data());
    EXRVersion version{};
    ASSERT_EQ(ParseEXRVersionFromMemory(&version, data, encoded.size()),
              TINYEXR_SUCCESS);
    ASSERT_EQ(ParseEXRHeaderFromMemory(&header, &version, data, encoded.size(),
                                       &error),
              TINYEXR_SUCCESS)
        << (error ? error : "");
    ASSERT_EQ(
        LoadEXRImageFromMemory(&image, &header, data, encoded.size(), &error),
        TINYEXR_SUCCESS)
        << (error ? error : "");
  }
  const float *channel(const char *name) const {
    for (int i = 0; i < header.num_channels; ++i)
      if (std::string(header.channels[i].name) == name)
        return reinterpret_cast<const float *>(image.images[i]);
    ADD_FAILURE() << "missing EXR channel " << name;
    return nullptr;
  }
};

class OpenVdbSurfaceReference : public ::testing::Test {
protected:
  using V = openvdb::Vec3d;
  using C = openvdb::Coord;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-surface-reference-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.width = options.height = 1;
    options.rays.limits.maxWorkItems = 2'000'000'000;
    camera.maximumDistance = 20;
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
    openvdb::GridPtrVec views;
    for (const auto &grid : grids)
      if (grid->isType<openvdb::FloatGrid>())
        views.push_back(
            std::make_shared<ExactReferenceFixture<openvdb::FloatGrid>>(
                *openvdb::gridPtrCast<openvdb::FloatGrid>(grid)));
      else if (grid->isType<openvdb::DoubleGrid>())
        views.push_back(
            std::make_shared<ExactReferenceFixture<openvdb::DoubleGrid>>(
                *openvdb::gridPtrCast<openvdb::DoubleGrid>(grid)));
      else
        views.push_back(grid);
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(views, metadata);
  }
  static void uniformMap(openvdb::GridBase &grid, double h) {
    if (h >= 1e-4) {
      grid.setTransform(openvdb::math::Transform::createLinearTransform(h));
      return;
    }
    std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
    openvdb::writeString(record, openvdb::math::UniformScaleMap::mapType());
    for (double value : {h, h, 1 / h, 1 / (h * h), .5 / h})
      V(value).write(record);
    record.seekg(0);
    openvdb::io::setCurrentVersion(record);
    grid.transform().read(record);
  }
  template <class G = openvdb::DoubleGrid>
  static typename G::Ptr plane(int axis = 2, double h = 1) {
    using T = typename G::ValueType;
    auto grid = G::create(T(4 * h));
    grid->setName("plane");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    uniformMap(*grid, h);
    for (int a = -8; a <= 8; ++a)
      for (int b = -8; b <= 8; ++b)
        for (int n = -6; n <= 3; ++n) {
          C c;
          c[axis] = n;
          c[(axis + 1) % 3] = a;
          c[(axis + 2) % 3] = b;
          if (n >= -3)
            grid->tree().setValue(c, T((n - .25) * h));
          else
            grid->tree().setValueOff(c, T(-4 * h));
        }
    return grid;
  }
  static openvdb::DoubleGrid::Ptr sphere() {
    auto grid = openvdb::DoubleGrid::create(1.);
    grid->setName("sphere");
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    uniformMap(*grid, .25);
    for (int x = -9; x <= 9; ++x)
      for (int y = -9; y <= 9; ++y)
        for (int z = -9; z <= 9; ++z) {
          const double d = std::hypot(double(x), double(y), double(z)) - 4.5;
          if (std::abs(d) < 4)
            grid->tree().setValue(C(x, y, z), d * .25);
          else if (d < 0)
            grid->tree().setValueOff(C(x, y, z), -1.);
        }
    return grid;
  }
  vdb::SurfaceReferenceImage render(const std::string &input = "input.vdb",
                                    const std::string &selector = {}) {
    return vdb::RenderSurfaceReference(path(input), camera, options, selector,
                                       processing);
  }
  static void hit(const vdb::SurfaceReferenceImage &image, size_t pixel,
                  double distance, V position, double tolerance = 1e-9) {
    ASSERT_LT(pixel, image.geometry.rays.size());
    ASSERT_LT(pixel, image.rgba.size());
    const auto &ray = image.geometry.rays[pixel];
    ASSERT_NE(ray.kind, Kind::Miss);
    EXPECT_NEAR(ray.distance, distance, tolerance);
    for (int a = 0; a < 3; ++a)
      EXPECT_NEAR(ray.position[a], position[a], tolerance);
    EXPECT_EQ(image.rgba[pixel][3], 1);
  }
  void structural(const vdb::SurfaceReferenceImage &image) {
    EXPECT_EQ(image.width, options.width);
    EXPECT_EQ(image.height, options.height);
    EXPECT_EQ(image.rgba.size(), size_t(image.width) * image.height);
    EXPECT_EQ(image.geometry.rays.size(), image.rgba.size());
    EXPECT_GT(image.preparationWork, 0u);
    EXPECT_GT(image.geometry.source.workItems, 0u);
    EXPECT_LE(image.work, options.rays.limits.maxWorkItems);
    EXPECT_GE(image.work, image.preparationWork);
    size_t hits = 0;
    for (size_t i = 0; i < image.rgba.size(); ++i) {
      const bool on = image.geometry.rays[i].kind != Kind::Miss;
      hits += on;
      EXPECT_EQ(image.rgba[i][3], on ? 1 : 0);
      for (double c : image.rgba[i])
        EXPECT_TRUE(std::isfinite(c));
    }
    EXPECT_EQ(image.hitCount, hits);
  }
  void exportFails(const vdb::SurfaceReferenceImage &image) {
    const auto name = "failed-" + std::to_string(++serial) + ".exr";
    EXPECT_THROW((void)vdb::WriteSurfaceReferenceExr(path(name), image),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(name)));
  }
  std::filesystem::path directory;
  vdb::SurfaceReferenceCamera camera;
  vdb::SurfaceReferenceOptions options;
  vdb::ProcessingOptions processing;
  unsigned serial = 0;
};

TEST_F(OpenVdbSurfaceReference,
       CenterPlaneHasDoubleDepthAndExactNativeShaderColors) {
  write("input.vdb", {plane()});
  const auto source = bytes(path("input.vdb"));
  options.color = {.25, .5, 1};
  for (auto shading :
       {Shade::Matte, Shade::WorldNormal, Shade::TwoSidedHeadlight}) {
    options.shading = shading;
    options.rays.computeNormals = shading != Shade::Matte;
    const auto image = render();
    structural(image);
    hit(image, 0, 2.75, V(0, 0, .25));
    const auto expected = shading == Shade::WorldNormal
                              ? std::array<double, 4>{.125, .25, 1, 1}
                              : std::array<double, 4>{.25, .5, 1, 1};
    EXPECT_EQ(image.rgba[0], expected);
    EXPECT_EQ(image.geometry.rays[0].normalStatus, shading == Shade::Matte
                                                       ? Normal::NotRequested
                                                       : Normal::Available);
    EXPECT_EQ(image.roundedColorValues, 0u);
  }
  EXPECT_EQ(bytes(path("input.vdb")), source);
}

TEST_F(OpenVdbSurfaceReference,
       PerspectiveNonSquareCentersProveVerticalFovTopRowAndRadialDepth) {
  write("input.vdb", {plane()});
  camera.eye = {0, 0, 2};
  camera.target = {0, 0, 0};
  camera.verticalFovDegrees = 90;
  options.width = 4;
  options.height = 2;
  options.shading = Shade::Matte;
  options.rays.computeNormals = false;
  const auto image = render();
  structural(image);
  ASSERT_EQ(image.hitCount, 8u);
  for (unsigned y = 0; y < 2; ++y)
    for (unsigned x = 0; x < 4; ++x) {
      const double sx = (2 * (x + .5) / 4 - 1) * 2, sy = 1 - 2 * (y + .5) / 2;
      const double length = std::hypot(sx, sy, 1.);
      hit(image, y * 4 + x, 1.75 * length, V(1.75 * sx, 1.75 * sy, .25), 2e-9);
      EXPECT_NEAR(image.geometry.rays[y * 4 + x].normalizedDirection[0],
                  sx / length, 1e-13);
      EXPECT_NEAR(image.geometry.rays[y * 4 + x].normalizedDirection[1],
                  sy / length, 1e-13);
      EXPECT_NEAR(image.geometry.rays[y * 4 + x].normalizedDirection[2],
                  -1 / length, 1e-13);
    }
  EXPECT_LT(image.geometry.rays[0].position[0],
            image.geometry.rays[3].position[0]);
  EXPECT_GT(image.geometry.rays[0].position[1],
            image.geometry.rays[4].position[1]);
}

TEST_F(OpenVdbSurfaceReference,
       OrthographicChangesOriginsNotDirectionAndIgnoresHiddenFov) {
  write("input.vdb", {plane()});
  camera.projection = vdb::SurfaceProjection::Orthographic;
  camera.eye = {0, 0, 2};
  camera.target = {0, 0, 0};
  camera.orthographicHeight = 2;
  camera.verticalFovDegrees = std::numeric_limits<double>::quiet_NaN();
  options.width = 4;
  options.height = 2;
  const auto image = render();
  structural(image);
  for (unsigned y = 0; y < 2; ++y)
    for (unsigned x = 0; x < 4; ++x) {
      hit(image, y * 4 + x, 1.75,
          V((2 * (x + .5) / 4 - 1) * 2, 1 - 2 * (y + .5) / 2, .25));
      EXPECT_EQ(image.geometry.rays[y * 4 + x].normalizedDirection,
                (std::array<double, 3>{0, 0, -1}));
    }
}

TEST_F(OpenVdbSurfaceReference,
       PerspectiveClippingIsRadialNotNativeViewPlaneDepth) {
  write("input.vdb", {plane()});
  camera.eye = {0, 0, 2};
  camera.target = {0, 0, 0};
  camera.verticalFovDegrees = 90;
  options.width = 4;
  options.height = 2;
  // Every plane depth is1.75, but radial distances range2.14..3.28.
  camera.maximumDistance = 2;
  const auto missed = render();
  structural(missed);
  EXPECT_EQ(missed.hitCount, 0u);
  camera.maximumDistance = 20;
  camera.minimumDistance = 2;
  const auto visible = render();
  structural(visible);
  EXPECT_EQ(visible.hitCount, 8u);
  EXPECT_GT(visible.geometry.rays[0].distance, 3);
}

TEST_F(OpenVdbSurfaceReference,
       SphereSilhouetteAndDepthHaveIndependentAnalyticEnvelope) {
  write("input.vdb", {sphere()});
  camera.projection = vdb::SurfaceProjection::Orthographic;
  camera.orthographicHeight = 5;
  options.width = options.height = 5;
  const auto image = render();
  structural(image);
  const std::set<size_t> expected{7, 11, 12, 13, 17};
  EXPECT_EQ(image.hitCount, expected.size());
  for (size_t i = 0; i < 25; ++i) {
    const bool on = expected.contains(i);
    EXPECT_EQ(image.geometry.rays[i].kind != Kind::Miss, on) << i;
    if (on) {
      EXPECT_GE(image.geometry.rays[i].distance, 1.875 - 1e-8);
      EXPECT_LE(image.geometry.rays[i].distance, 3.);
      const double x = double(i % 5) - 2, y = 2 - double(i / 5);
      const double analytic = 3 - std::sqrt(1.125 * 1.125 - x * x - y * y);
      EXPECT_NEAR(image.geometry.rays[i].distance, analytic, .04);
    }
  }
  hit(image, 12, 1.875, V(0, 0, 1.125), 1e-9);
}

TEST_F(OpenVdbSurfaceReference,
       FloatAndDoubleGeometryRetainsReflectedRotatedNonunitMap) {
  for (bool nativeFloat : {false, true}) {
    auto grid = nativeFloat
                    ? openvdb::GridBase::Ptr(plane<openvdb::FloatGrid>(0, 2))
                    : openvdb::GridBase::Ptr(plane(0, 2));
    grid->transform().postScale(V(-1, 1, 1));
    grid->transform().postRotate(.37, openvdb::math::Z_AXIS);
    grid->transform().postTranslate(V(100.125, -50, 7));
    write("input.vdb", {grid});
    const auto eye = grid->transform().indexToWorld(V(2.25, .25, .25));
    const auto target = grid->transform().indexToWorld(V(.25, .25, .25));
    const auto up = grid->transform().baseMap()->applyJacobian(V(0, 1, 0));
    camera.eye = {eye[0], eye[1], eye[2]};
    camera.target = {target[0], target[1], target[2]};
    camera.up = {up[0], up[1], up[2]};
    options.shading = Shade::WorldNormal;
    const auto image = render();
    structural(image);
    hit(image, 0, 4, target, 2e-9);
    EXPECT_NEAR(image.geometry.rays[0].normal[0], -std::cos(.37), 1e-11);
    EXPECT_NEAR(image.geometry.rays[0].normal[1], -std::sin(.37), 1e-11);
  }
}

TEST_F(OpenVdbSurfaceReference, TinyAndHugeUniformSpacingRetainsWorldGeometry) {
  for (double h : {1e-6, 1e6}) {
    write("input.vdb", {plane(2, h)});
    camera.eye = {0, 0, 3 * h};
    camera.target = {0, 0, 0};
    camera.maximumDistance = 10 * h;
    const auto image = render();
    structural(image);
    hit(image, 0, 2.75 * h, V(0, 0, .25 * h), 1e-9 * h);
  }
}

TEST_F(OpenVdbSurfaceReference,
       EmptyMissBackgroundHasCoverageZeroAndCheckedColorRounding) {
  auto empty = openvdb::DoubleGrid::create(4);
  empty->setGridClass(openvdb::GRID_LEVEL_SET);
  write("input.vdb", {empty});
  options.width = 3;
  options.height = 2;
  options.background = {.1, .25, .5};
  const auto image = render();
  structural(image);
  EXPECT_TRUE(image.geometry.source.empty);
  EXPECT_EQ(image.hitCount, 0u);
  EXPECT_GT(image.roundedColorValues, 0u);
  EXPECT_GT(image.maximumColorAbsoluteError, 0);
  for (size_t i = 0; i < image.rgba.size(); ++i) {
    EXPECT_EQ(image.geometry.rays[i].kind, Kind::Miss);
    EXPECT_EQ(image.rgba[i],
              (std::array<double, 4>{double(float(.1)), .25, .5, 0}));
  }
}

TEST_F(OpenVdbSurfaceReference,
       MissingDegenerateAndUnrequestedNormalsAreVisibleHits) {
  auto grid = plane();
  grid->tree().setValueOff(C(1, 1, 1), 4);
  write("input.vdb", {grid});
  camera.eye = {0, 0, 3};
  camera.target = {0, 0, 0};
  options.shading = Shade::WorldNormal;
  options.invalidNormalColor = {.25, .5, .75};
  auto image = render();
  structural(image);
  hit(image, 0, 2.75, V(0, 0, .25));
  EXPECT_EQ(image.geometry.rays[0].normalStatus,
            Normal::MissingActiveFootprint);
  EXPECT_EQ(image.unavailableNormalCount, 1u);
  EXPECT_EQ(image.rgba[0], (std::array<double, 4>{.25, .5, .75, 1}));
  write("input.vdb", {plane()});
  options.rays.computeNormals = false;
  image = render();
  structural(image);
  EXPECT_EQ(image.geometry.rays[0].normalStatus, Normal::NotRequested);
  EXPECT_EQ(image.unavailableNormalCount, 1u);
  EXPECT_EQ(image.rgba[0], (std::array<double, 4>{.25, .5, .75, 1}));
  grid = plane();
  for (auto value = grid->beginValueOn(); value; ++value) {
    const int z = value.getCoord().z();
    value.setValue(z == 0 || z == 1 ? 0 : double(z));
  }
  write("input.vdb", {grid});
  options.rays.computeNormals = true;
  camera.eye = {0, 0, .25};
  camera.target = {0, 0, -1};
  image = render();
  structural(image);
  hit(image, 0, 0, V(0, 0, .25));
  EXPECT_EQ(image.geometry.rays[0].kind, Kind::SampledZero);
  EXPECT_EQ(image.geometry.rays[0].normalStatus, Normal::DegenerateGradient);
  EXPECT_EQ(image.rgba[0], (std::array<double, 4>{.25, .5, .75, 1}));
}

TEST_F(OpenVdbSurfaceReference,
       SourceMetadataSelectorsUnicodeAndRepeatabilitySurvive) {
  auto grid = plane();
  grid->setName("descriptor:plane");
  grid->insertMeta("artist",
                   openvdb::StringMetadata("source remains unchanged"));
  openvdb::MetaMap metadata;
  metadata.insertMeta("scene", openvdb::StringMetadata("native surface"));
  write("input.vdb", {grid}, metadata);
  const auto before = bytes(path("input.vdb"));
  const auto unicode = directory / std::filesystem::path(u"source_球.vdb");
  std::filesystem::copy_file(path("input.vdb"), unicode);
  const auto first = vdb::RenderSurfaceReference(
      unicode, camera, options, "descriptor:descriptor:plane", processing);
  const auto second = vdb::RenderSurfaceReference(
      unicode, camera, options, "descriptor:descriptor:plane", processing);
  EXPECT_EQ(first.rgba, second.rgba);
  ASSERT_EQ(first.geometry.rays.size(), 1u);
  ASSERT_EQ(second.geometry.rays.size(), 1u);
  EXPECT_EQ(first.geometry.rays[0].distance, second.geometry.rays[0].distance);
  EXPECT_EQ(first.geometry.rays[0].position, second.geometry.rays[0].position);
  bool artist = false, scene = false;
  for (const auto &m : first.geometry.source.metadata)
    if (m.name == "artist") {
      artist = true;
      EXPECT_EQ(m.value, "source remains unchanged");
    }
  for (const auto &m : first.geometry.source.fileMetadata)
    if (m.name == "scene") {
      scene = true;
      EXPECT_EQ(m.value, "native surface");
    }
  EXPECT_TRUE(artist);
  EXPECT_TRUE(scene);
  EXPECT_THROW((void)render("input.vdb", "descriptor:missing"), std::exception);
  EXPECT_EQ(bytes(path("input.vdb")), before);
  EXPECT_EQ(bytes(unicode), before);
}

TEST_F(OpenVdbSurfaceReference,
       CameraImageAndSharedCapsRejectBeforeProducingImages) {
  write("input.vdb", {plane()});
  const auto before = bytes(path("input.vdb"));
  const auto goodCamera = camera;
  const auto goodOptions = options;
  for (int bad = 0; bad < 6; ++bad) {
    camera = goodCamera;
    if (bad == 0)
      camera.target = camera.eye;
    if (bad == 1)
      camera.up = {0, 0, 1};
    if (bad == 2)
      camera.up = {0, 0, 0};
    if (bad == 3)
      camera.verticalFovDegrees = 151;
    if (bad == 4)
      camera.minimumDistance = camera.maximumDistance;
    if (bad == 5)
      camera.eye[0] = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)render(), std::exception);
  }
  camera = goodCamera;
  for (int bad = 0; bad < 8; ++bad) {
    options = goodOptions;
    processing = {};
    if (bad == 0)
      options.width = 0;
    if (bad == 1)
      options.height = 2049;
    if (bad == 2) {
      options.width = 2;
      options.maxPixels = 1;
    }
    if (bad == 3) {
      options.width = 2;
      options.rays.limits.maxRays = 1;
    }
    if (bad == 4)
      options.rays.limits.maxWorkItems = 1;
    if (bad == 5)
      processing.maxInputBytes = 1;
    if (bad == 6)
      processing.maxVoxelCount = 1;
    if (bad == 7)
      options.color[0] = std::numeric_limits<double>::denorm_min();
    EXPECT_THROW((void)render(), std::exception);
  }
  options = goodOptions;
  processing = {};
  processing.voxelSize = 0;
  processing.halfWidth = 0;
  processing.maxTriangles = 0;
  EXPECT_NO_THROW((void)render());
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbSurfaceReference,
       ExrElevenLinearFloatChannelsMatchMemoryGeometryAndMasks) {
  write("input.vdb", {sphere()});
  camera.projection = vdb::SurfaceProjection::Orthographic;
  camera.orthographicHeight = 5;
  options.width = options.height = 5;
  const auto image = render();
  const auto saved = vdb::WriteSurfaceReferenceExr(path("image.exr"), image);
  EXPECT_EQ(saved.encodedBytes, std::filesystem::file_size(path("image.exr")));
  ReferenceExr read;
  read.read(bytes(path("image.exr")));
  ASSERT_EQ(read.header.num_channels, 11);
  ASSERT_EQ(read.image.width, 5);
  ASSERT_EQ(read.image.height, 5);
  EXPECT_EQ(read.header.compression_type, TINYEXR_COMPRESSIONTYPE_NONE);
  const std::set<std::string> expected{"R",       "G",        "B",       "A",
                                       "Z",       "N.X",      "N.Y",     "N.Z",
                                       "N.VALID", "N.STATUS", "HIT.KIND"};
  std::set<std::string> actual;
  for (int c = 0; c < read.header.num_channels; ++c) {
    actual.insert(read.header.channels[c].name);
    EXPECT_EQ(read.header.pixel_types[c], TINYEXR_PIXELTYPE_FLOAT);
  }
  EXPECT_EQ(actual, expected);
  const auto *red = read.channel("R");
  const auto *green = read.channel("G");
  const auto *blue = read.channel("B");
  const auto *alpha = read.channel("A");
  const auto *depth = read.channel("Z");
  const auto *valid = read.channel("N.VALID");
  const auto *status = read.channel("N.STATUS");
  const auto *kind = read.channel("HIT.KIND");
  ASSERT_NE(red, nullptr);
  ASSERT_NE(green, nullptr);
  ASSERT_NE(blue, nullptr);
  ASSERT_NE(alpha, nullptr);
  ASSERT_NE(depth, nullptr);
  ASSERT_NE(valid, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_NE(kind, nullptr);
  const std::array<const float *, 3> normals{
      read.channel("N.X"), read.channel("N.Y"), read.channel("N.Z")};
  for (auto pointer : normals)
    ASSERT_NE(pointer, nullptr);
  for (size_t p = 0; p < 25; ++p) {
    const auto &hit = image.geometry.rays[p];
    const bool on = hit.kind != Kind::Miss;
    EXPECT_EQ(red[p], float(image.rgba[p][0]));
    EXPECT_EQ(green[p], float(image.rgba[p][1]));
    EXPECT_EQ(blue[p], float(image.rgba[p][2]));
    EXPECT_EQ(alpha[p], on ? 1.f : 0.f);
    EXPECT_EQ(depth[p], on ? float(hit.distance) : 0.f);
    EXPECT_EQ(kind[p], float(static_cast<int>(hit.kind)));
    EXPECT_EQ(status[p], float(static_cast<int>(hit.normalStatus)));
    EXPECT_EQ(valid[p], hit.normalStatus == Normal::Available ? 1.f : 0.f);
    for (int c = 0; c < 3; ++c)
      EXPECT_EQ(normals[c][p], hit.normalStatus == Normal::Available
                                   ? float(hit.normal[c])
                                   : 0.f);
  }
}

TEST_F(OpenVdbSurfaceReference,
       ExrUnavailableNormalsArePlaceholdersNotValidityOrMisses) {
  auto grid = plane();
  grid->tree().setValueOff(C(1, 1, 1), 4);
  write("input.vdb", {grid});
  const auto image = render();
  ASSERT_EQ(image.geometry.rays[0].normalStatus,
            Normal::MissingActiveFootprint);
  (void)vdb::WriteSurfaceReferenceExr(path("missing.exr"), image);
  ReferenceExr read;
  read.read(bytes(path("missing.exr")));
  const auto *valid = read.channel("N.VALID");
  const auto *status = read.channel("N.STATUS");
  const auto *alpha = read.channel("A");
  ASSERT_NE(valid, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_NE(alpha, nullptr);
  EXPECT_EQ(valid[0], 0);
  EXPECT_EQ(status[0], float(static_cast<int>(Normal::MissingActiveFootprint)));
  EXPECT_EQ(alpha[0], 1);
  for (const auto *name : {"N.X", "N.Y", "N.Z"}) {
    const auto *value = read.channel(name);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value[0], 0);
  }
}

TEST_F(OpenVdbSurfaceReference,
       ExrUnicodeExclusivePublicationAndSourceProtection) {
  write("input.vdb", {plane()});
  const auto source = bytes(path("input.vdb"));
  const auto image = render();
  const auto unicode = directory / std::filesystem::path(u"surface_球_é.exr");
  const auto saved = vdb::WriteSurfaceReferenceExr(unicode, image);
  EXPECT_EQ(saved.encodedBytes, std::filesystem::file_size(unicode));
  const auto original = bytes(unicode);
  ReferenceExr read;
  read.read(original);
  EXPECT_EQ(read.header.num_channels, 11);
  EXPECT_THROW((void)vdb::WriteSurfaceReferenceExr(unicode, image),
               std::exception);
  EXPECT_EQ(bytes(unicode), original);
  EXPECT_THROW((void)vdb::WriteSurfaceReferenceExr(path("input.vdb"), image),
               std::exception);
  EXPECT_EQ(bytes(path("input.vdb")), source);
}

TEST_F(OpenVdbSurfaceReference,
       ExrRejectsContradictoryNormalStatusesWithConsistentCounters) {
  write("input.vdb", {plane()});
  const auto image = render();
  ASSERT_EQ(image.hitCount, 1u);
  ASSERT_EQ(image.geometry.rays[0].normalStatus, Normal::Available);
  auto bad = image;
  bad.geometry.rays[0].normalStatus = Normal::NoHit;
  bad.unavailableNormalCount = 1;
  exportFails(bad); // Counts/coverage agree; only the status matrix rejects.
  bad = image;
  bad.options.rays.computeNormals = false;
  exportFails(bad); // Available is contradictory even with correct counters.
  bad.geometry.rays[0].normalStatus = Normal::MissingActiveFootprint;
  bad.unavailableNormalCount = 1;
  exportFails(bad);
  bad = image;
  bad.geometry.rays[0].kind = Kind::Miss;
  bad.geometry.rays[0].normalStatus = Normal::NotRequested;
  bad.rgba[0][3] = 0;
  bad.hitCount = bad.unavailableNormalCount = 0;
  exportFails(bad); // Requested+miss must be NoHit, not NotRequested.
}

TEST_F(OpenVdbSurfaceReference,
       ExrRangeMalformedStateAndJointBytesFailWithoutOutput) {
  write("input.vdb", {plane()});
  const auto image = render();
  auto bad = image;
  bad.rgba[0][0] = std::numeric_limits<double>::denorm_min();
  exportFails(bad);
  bad = image;
  bad.geometry.rays[0].distance = std::numeric_limits<double>::max();
  exportFails(bad);
  bad = image;
  bad.geometry.rays[0].normalStatus = static_cast<Normal>(99);
  exportFails(bad);
  bad = image;
  bad.geometry.rays[0].kind = static_cast<Kind>(99);
  exportFails(bad);
  bad = image;
  bad.rgba.pop_back();
  exportFails(bad);
  bad = image;
  bad.maxImageBytes = 1;
  exportFails(bad);
  EXPECT_THROW((void)vdb::WriteSurfaceReferenceExr(path("cap.exr"), image, 1),
               std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("cap.exr")));
}
#endif
} // namespace
