#include "Container/geometry/OpenVdbTransportImageScene.h"
#include "Container/renderer/effects/OpenVdbTransportImageGpu.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <gtest/gtest.h>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
namespace renderer = container::renderer;
class OpenVdbTransportImageGpu : public ::testing::Test {
protected:
  std::unique_ptr<renderer::TransportImageGpuSession> session;
  void SetUp() override {
    session = std::make_unique<renderer::TransportImageGpuSession>(
        CONTAINER_OPENVDB_TRANSPORT_REFERENCE_SPIRV, true);
  }
  vdb::TransportImageOptions options(uint32_t width = 7, uint32_t height = 5, uint32_t spp = 1024) {
    vdb::TransportImageOptions o;
    o.width = width;
    o.height = height;
    o.transport.samplesPerRay = spp;
    o.transport.maxWork = 1'000'000'000;
    o.transport.maxScatteringEvents = 3;
    return o;
  }
  vdb::TransportScene slab(double extinction = .6) {
    vdb::TransportScene scene;
    vdb::TransportMedium m;
    m.extinction = extinction;
    m.albedo = {0, 0, 0};
    m.boundsMin = {-100, -100, -1};
    m.boundsMax = {100, 100, 1};
    scene.media.push_back(m);
    scene.environment = {1, .5, .25};
    return scene;
  }
  void agreement(const vdb::TransportImageResult &a, const vdb::TransportImageResult &b,
                 double slack = 1e-3) {
    ASSERT_EQ(a.pixels.size(), b.pixels.size());
    for (size_t i = 0; i < a.pixels.size(); ++i)
      for (int k = 0; k < 3; ++k)
        EXPECT_NEAR(a.pixels[i].radiance[k], b.pixels[i].radiance[k],
                    7 * std::hypot(a.pixels[i].standardError[k], b.pixels[i].standardError[k]) +
                        slack);
  }
};
TEST_F(OpenVdbTransportImageGpu, VacuumCameraAndTraversalAreExactAndFailurePublishesNothing) {
  vdb::TransportScene scene;
  scene.environment = {.25, .5, 1};
  auto o = options(17, 9, 8);
  auto a = session->render(scene, {}, o);
  o.reverseBlocks = true;
  auto b = session->render(scene, {}, o);
  ASSERT_EQ(a.pixels.size(), 153u);
  for (size_t i = 0; i < a.pixels.size(); ++i) {
    EXPECT_EQ(a.pixels[i].radiance, scene.environment);
    EXPECT_EQ(a.pixels[i].radiance, b.pixels[i].radiance);
    EXPECT_EQ(a.pixels[i].standardError, (vdb::TransportVector{}));
  }
  EXPECT_EQ(a.diagnostics.paths, b.diagnostics.paths);
  EXPECT_EQ(a.diagnostics.work, b.diagnostics.work);
  o.transport.maxWork = 1;
  EXPECT_THROW((void)session->render(scene, {}, o), std::exception);
  o.transport.maxWork = 1'000'000;
  EXPECT_NO_THROW((void)session->render(scene, {}, o));
}
TEST_F(OpenVdbTransportImageGpu, AbsorptionEmissionAndCpuSpatialImagesAgreeAcrossSeeds) {
  auto scene = slab(.5);
  scene.media[0].albedo = {.6, .4, .2};
  scene.media[0].anisotropy = .4;
  scene.media[0].emission = {.1, .2, .05};
  scene.lights.push_back({vdb::TransportLightType::Directional, {0, 0, -1}, {.3, .2, .4}});
  scene.lights.push_back({vdb::TransportLightType::Point, {-2, 1, 0}, {.2, .1, .3}});
  auto o = options();
  vdb::TransportPinholeCamera camera;
  camera.eye = {0, 0, 3};
  camera.verticalFovDegrees = 12;
  for (uint64_t seed : {3, 71, 901}) {
    SCOPED_TRACE(seed);
    o.transport.seed = seed;
    auto gpu = session->render(scene, camera, o);
    auto cpu = vdb::RenderTransportImageCpu(scene, camera, o);
    agreement(gpu, cpu);
    EXPECT_GT(gpu.diagnostics.scatteringEvents, 0u);
    EXPECT_GT(gpu.diagnostics.shadowProposals, 0u);
  }
}
TEST_F(OpenVdbTransportImageGpu, ImageSamplingConvergesAtFixedScatteringOrder) {
  const auto scene = slab();
  vdb::TransportPinholeCamera camera;
  camera.verticalFovDegrees = 10;
  auto low = options(7, 5, 32), high = options(7, 5, 4096);
  low.transport.maxScatteringEvents = high.transport.maxScatteringEvents = 0;
  double lowError = 0, highError = 0;
  for (uint64_t seed = 1; seed <= 8; ++seed) {
    low.transport.seed = high.transport.seed = seed;
    const auto a = session->render(scene, camera, low), b = session->render(scene, camera, high);
    const double tanHalf = std::tan(camera.verticalFovDegrees * std::acos(-1.) / 360);
    for (uint32_t y = 0; y < 5; ++y)
      for (uint32_t x = 0; x < 7; ++x) {
        const double dx = (2 * (x + .5) / 7 - 1) * (7. / 5) * tanHalf,
                     dy = (1 - 2 * (y + .5) / 5) * tanHalf;
        const double expected = std::exp(-.6 * 2 * std::sqrt(1 + dx * dx + dy * dy));
        const auto i = y * 7 + x;
        lowError += std::pow(a.pixels[i].radiance[0] - expected, 2);
        highError += std::pow(b.pixels[i].radiance[0] - expected, 2);
      }
  }
  EXPECT_LT(highError, lowError / 20);
}
TEST_F(OpenVdbTransportImageGpu, SpatialImageOrdersIlluminateBehindBlackWall) {
  auto scene = slab(.7);
  scene.media[0].boundsMin = {-2, -2, -2};
  scene.media[0].boundsMax = {2, 2, 2};
  scene.media[0].albedo = {.9, .9, .9};
  scene.environment = {};
  scene.lights = {{vdb::TransportLightType::Directional, {1, 0, 0}, {1, 1, 1}}};
  scene.occluders = {{{0, -.5, -2}, {0, .5, -2}, {0, .5, 2}},
                     {{0, -.5, -2}, {0, .5, 2}, {0, -.5, 2}}};
  vdb::TransportPinholeCamera camera;
  camera.eye = {-3, 0, 0};
  camera.target = {0, 0, 0};
  camera.verticalFovDegrees = 1;
  auto o = options(3, 3, 8192);
  o.transport.maxScatteringEvents = 1;
  const auto single = session->render(scene, camera, o);
  o.transport.maxScatteringEvents = 3;
  const auto multiple = session->render(scene, camera, o),
             cpu = vdb::RenderTransportImageCpu(scene, camera, o);
  double mean = 0, error = 0;
  for (size_t i = 0; i < 9; ++i) {
    EXPECT_EQ(single.pixels[i].radiance, (vdb::TransportVector{}));
    mean += multiple.pixels[i].radiance[0] / 9;
    error += std::pow(multiple.pixels[i].standardError[0] / 9, 2);
  }
  EXPECT_GT(mean, 6 * std::sqrt(error));
  EXPECT_GT(mean, 1e-4);
  agreement(multiple, cpu, 5e-4);
  EXPECT_GT(multiple.diagnostics.truncatedPaths, 0u);
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB && defined(CONTAINER_HAS_NANOVDB) &&   \
    CONTAINER_HAS_NANOVDB
TEST_F(OpenVdbTransportImageGpu, TrustedNativeAndDenseImageAdaptersMatchAnalyticProfile) {
  openvdb::initialize();
  static std::atomic_uint64_t ordinal{};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("container-image-native-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "-" + std::to_string(++ordinal));
  ASSERT_TRUE(std::filesystem::create_directory(dir));
  struct Cleanup {
    std::filesystem::path dir;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove(dir / "density.vdb", e);
      std::filesystem::remove(dir, e);
    }
  } cleanup{dir};
  auto grid = openvdb::FloatGrid::create(0);
  grid->setName("density");
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  auto map = openvdb::math::Transform::createLinearTransform(.25);
  map->postScale({-1, 1, 1});
  map->postTranslate({1, 0, 0});
  grid->setTransform(map);
  for (int z = 0; z < 8; ++z)
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        grid->tree().setValue({x, y, z}, z == 0 || z == 7 ? 0.f : 1.f);
  {
    openvdb::io::File f((dir / "density.vdb").string());
    f.write({grid});
    f.close();
  }
  vdb::TransportImageSceneDescription desc;
  desc.environment = {1, .5, 2};
  vdb::TransportImageVolumeSource source;
  source.input = dir / "density.vdb";
  source.volume.importScale = 1.5f;
  source.volume.maxResolution = 128;
  source.material.extinction = .4;
  source.material.albedo = {};
  desc.volumes.push_back(source);
  auto dense = vdb::LoadTransportImageScene(desc);
  desc.volumes[0].volume.sparse = true;
  auto native = vdb::LoadTransportImageScene(desc);
  auto center = map->indexToWorld(openvdb::Vec3d(1.5, 1.5, 3.5)) * 1.5;
  vdb::TransportPinholeCamera camera;
  camera.eye = {center.x(), center.y(), -1};
  camera.target = {center.x(), center.y(), 2};
  camera.verticalFovDegrees = 1;
  auto o = options(3, 3, 8192);
  o.transport.maxScatteringEvents = 0;
  const auto a = session->render(native->scene(), camera, o, native->nativeSources());
  const auto b = session->render(dense->scene(), camera, o);
  const auto cpu = vdb::RenderTransportImageCpu(dense->scene(), camera, o);
  agreement(a, b);
  agreement(b, cpu);
  const double expected = std::exp(-.9);
  for (const auto &p : a.pixels)
    for (int k = 0; k < 3; ++k)
      EXPECT_NEAR(p.radiance[k], expected * desc.environment[k], 7 * p.standardError[k] + 5e-4);
}
#endif
} // namespace

int main(int argc, char** argv) {
  const char* enabled=std::getenv("CONTAINER_RUN_OPENVDB_TRANSPORT_IMAGE_GPU");
  if (!enabled || std::string_view(enabled)!="1") return 77;
  ::testing::InitGoogleTest(&argc,argv);
  return RUN_ALL_TESTS();
}
