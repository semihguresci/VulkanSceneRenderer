#include "Container/geometry/OpenVdbTransportImage.h"
#include "Container/geometry/OpenVdbTransportImageScene.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <string_view>
#include <tinyexr.h>
#include <tuple>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
using V = vdb::TransportVector;

vdb::TransportImageOptions imageOptions(uint32_t width = 1, uint32_t height = 1,
                                        uint32_t samples = 8192, uint32_t order = 0) {
  vdb::TransportImageOptions o;
  o.width = width;
  o.height = height;
  o.transport.samplesPerRay = samples;
  o.transport.maxScatteringEvents = order;
  o.transport.rouletteStart = 65;
  o.transport.seed = 73;
  return o;
}
vdb::TransportScene slab(double extinction) {
  vdb::TransportScene scene;
  vdb::TransportMedium medium;
  medium.extinction = extinction;
  medium.albedo = {0, 0, 0};
  scene.media.push_back(medium);
  return scene;
}
void nearEstimate(const vdb::TransportRayEstimate &actual, V expected) {
  for (size_t c = 0; c < 3; ++c) {
    EXPECT_GE(actual.radiance[c], 0);
    EXPECT_GE(actual.standardError[c], 0);
    EXPECT_NEAR(actual.radiance[c], expected[c], 7 * actual.standardError[c] + 1e-4);
  }
}
auto counters(const vdb::TransportImageDiagnostics &d) {
  return std::tuple{d.preparationWork, d.work, d.paths, d.scatteringEvents,
                    d.nullCollisions, d.shadowProposals, d.phaseSamples,
                    d.truncatedPaths, d.rouletteTerminations,
                    d.shadowRouletteTerminations};
}
void samePixelBits(const vdb::TransportImageResult &a,
                   const vdb::TransportImageResult &b) {
  ASSERT_EQ(a.pixels.size(), b.pixels.size());
  for (size_t i = 0; i < a.pixels.size(); ++i)
    for (size_t c = 0; c < 3; ++c) {
      EXPECT_EQ(std::bit_cast<uint64_t>(a.pixels[i].radiance[c]),
                std::bit_cast<uint64_t>(b.pixels[i].radiance[c])) << i << ',' << c;
      EXPECT_EQ(std::bit_cast<uint64_t>(a.pixels[i].standardError[c]),
                std::bit_cast<uint64_t>(b.pixels[i].standardError[c])) << i << ',' << c;
    }
}
// A square at z=-1. The 2x2, 90-degree camera's top-left center ray
// intersects (-.5,+.5,-1), away from every edge of these triangles.
void square(vdb::TransportScene &scene, double x, double y, double half = .2) {
  scene.occluders.push_back({{x-half, y-half, -1}, {x+half, y-half, -1},
                             {x+half, y+half, -1}});
  scene.occluders.push_back({{x-half, y-half, -1}, {x+half, y+half, -1},
                             {x-half, y+half, -1}});
}

TEST(OpenVdbTransportImageCpu, VacuumIsExactAndIndependentOfOpenVdbAvailability) {
  vdb::TransportScene scene;
  scene.environment = {.25, .5, 2};
  const auto options = imageOptions(7, 3, 8);
  const auto image = vdb::RenderTransportImageCpu(scene, {}, options);
  ASSERT_EQ(image.pixels.size(), 21u);
  EXPECT_FALSE(image.gpu);
  EXPECT_EQ(image.width, 7u);
  EXPECT_EQ(image.height, 3u);
  EXPECT_EQ(image.samplesPerPixel, 8u);
  EXPECT_EQ(image.seed, options.transport.seed);
  EXPECT_EQ(image.diagnostics.paths, 21u * 8u);
  EXPECT_GT(image.diagnostics.preparationWork, 0u);
  EXPECT_LE(image.diagnostics.preparationWork + image.diagnostics.work,
            options.transport.maxWork);
  EXPECT_EQ(image.diagnostics.scatteringEvents, 0u);
  for (const auto &pixel : image.pixels) {
    EXPECT_EQ(pixel.radiance, scene.environment);
    EXPECT_EQ(pixel.standardError, (V{}));
  }
}

TEST(OpenVdbTransportImageCpu, PixelCentersUseTopRowsXFastestAndVerticalFovAspect) {
  vdb::TransportPinholeCamera camera;
  camera.eye = {0, 0, 0};
  camera.target = {0, 0, -1};
  camera.verticalFovDegrees = 90;
  vdb::TransportScene scene;
  scene.environment = {1, 2, 3};
  square(scene, -.5, .5);
  auto image = vdb::RenderTransportImageCpu(scene, camera, imageOptions(2, 2, 4));
  ASSERT_EQ(image.pixels.size(), 4u);
  for (size_t i = 0; i < 4; ++i)
    EXPECT_EQ(image.pixels[i].radiance, i == 0 ? V{} : scene.environment) << i;
  for (auto &t : scene.occluders) std::swap(t.a, t.c);
  samePixelBits(image, vdb::RenderTransportImageCpu(scene, camera, imageOptions(2, 2, 4)));
  scene.occluders.clear();
  square(scene, 1.5, .5);
  image = vdb::RenderTransportImageCpu(scene, camera, imageOptions(4, 2, 4));
  for (size_t i = 0; i < 8; ++i)
    EXPECT_EQ(image.pixels[i].radiance, i == 3 ? V{} : scene.environment) << i;
}

TEST(OpenVdbTransportImageCpu, CenterRayFollowsTargetAndRotatedCameraUp) {
  vdb::TransportScene scene;
  scene.environment = {1, .5, .25};
  // Central ray starts at (4,2,3) and travels along negative X.
  scene.occluders.push_back({{0, 1, 2}, {0, 3, 2}, {0, 2, 4}});
  vdb::TransportPinholeCamera camera;
  camera.eye = {4, 2, 3}; camera.target = {0, 2, 3}; camera.up = {0, 0, 1};
  for (double fov : {.1, 45., 150.}) {
    camera.verticalFovDegrees = fov;
    EXPECT_EQ(vdb::RenderTransportImageCpu(scene, camera, imageOptions(1, 1, 4))
                  .pixels[0].radiance, (V{}));
  }
  scene.occluders.clear();
  camera.eye = {0, 0, 0}; camera.target = {0, 0, -1}; camera.up = {1, 0, 0};
  camera.verticalFovDegrees = 90;
  // Camera up=+X and right=-Y: top-left hits (+.5,+.5,-1).
  square(scene, .5, .5);
  const auto image = vdb::RenderTransportImageCpu(scene, camera, imageOptions(2, 2, 4));
  for (size_t i = 0; i < 4; ++i)
    EXPECT_EQ(image.pixels[i].radiance, i == 0 ? V{} : scene.environment) << i;
}

TEST(OpenVdbTransportImageCpu, BeerAbsorptionAndIndependentEmissionMatchAnalyticSlab) {
  auto scene = slab(.7);
  scene.environment = {1, .5, 2};
  const double transmission = std::exp(-1.4);
  auto image = vdb::RenderTransportImageCpu(scene, {}, imageOptions());
  nearEstimate(image.pixels[0], {transmission, .5 * transmission, 2 * transmission});
  EXPECT_EQ(image.diagnostics.scatteringEvents, 0u);
  EXPECT_EQ(image.diagnostics.truncatedPaths, 0u);
  scene.media[0].extinction = 0;
  scene.media[0].emission = {2, 1, .25};
  scene.environment = {.5, .25, 1};
  image = vdb::RenderTransportImageCpu(scene, {}, imageOptions());
  nearEstimate(image.pixels[0], {4.5, 2.25, 1.5});
  EXPECT_GT(image.diagnostics.nullCollisions, 0u);
  scene.media[0].extinction = .7;
  image = vdb::RenderTransportImageCpu(scene, {}, imageOptions());
  const double integral = -std::expm1(-1.4) / .7;
  nearEstimate(image.pixels[0], {.5 * transmission + 2 * integral,
                               .25 * transmission + integral,
                               transmission + .25 * integral});
}

TEST(OpenVdbTransportImageCpu, CenterPixelAgreesWithIndependentReferenceAndReportsHgStatistics) {
  auto scene = slab(.5);
  scene.media[0].albedo = {.8, .6, .4};
  scene.media[0].anisotropy = .7;
  scene.environment = {.5, .25, 1};
  scene.lights.push_back({vdb::TransportLightType::Directional, {0, 0, 1}, {2, 1, .5}});
  auto options = imageOptions(1, 1, 8192, 3);
  const auto image = vdb::RenderTransportImageCpu(scene, {}, options);
  auto referenceOptions = options.transport;
  referenceOptions.samplesPerRay = 40'000;
  referenceOptions.seed = 901; // Independent noise; the camera's center ray is known analytically.
  const std::array<vdb::TransportRay, 1> rays{{{{0, 0, 3}, {0, 0, -1}}}};
  const auto reference = vdb::TraceTransportReference(scene, rays, referenceOptions);
  for (size_t c = 0; c < 3; ++c)
    EXPECT_NEAR(image.pixels[0].radiance[c], reference.rays[0].radiance[c],
                7 * std::hypot(image.pixels[0].standardError[c],
                               reference.rays[0].standardError[c]) + 1e-4);
  EXPECT_GT(image.diagnostics.shadowProposals, 0u);
  EXPECT_GT(image.diagnostics.phaseSamples, 3000u);
  EXPECT_NEAR(image.diagnostics.meanScatteringCosine, .7, .025);
  scene.media[0].anisotropy = -.7;
  const auto backwards = vdb::RenderTransportImageCpu(scene, {}, options);
  EXPECT_GT(backwards.diagnostics.phaseSamples, 3000u);
  EXPECT_NEAR(backwards.diagnostics.meanScatteringCosine, -.7, .025);
}

TEST(OpenVdbTransportImageCpu, RepeatingAndReversingCanonicalBlocksPreservesEveryPixel) {
  auto scene = slab(.6);
  scene.environment = {1, .5, .25};
  scene.media[0].albedo = {.8, .7, .6};
  scene.media[0].anisotropy = .35;
  scene.media[0].emission = {.1, .05, .025};
  auto options = imageOptions(17, 9, 32, 3); // Three blocks, including a partial final block.
  const auto original = vdb::RenderTransportImageCpu(scene, {}, options);
  const auto repeated = vdb::RenderTransportImageCpu(scene, {}, options);
  samePixelBits(original, repeated);
  EXPECT_EQ(counters(original.diagnostics), counters(repeated.diagnostics));
  EXPECT_DOUBLE_EQ(original.diagnostics.meanScatteringCosine,
                   repeated.diagnostics.meanScatteringCosine);
  options.reverseBlocks = true;
  const auto reversed = vdb::RenderTransportImageCpu(scene, {}, options);
  samePixelBits(original, reversed);
  EXPECT_EQ(counters(original.diagnostics), counters(reversed.diagnostics));
  EXPECT_NEAR(original.diagnostics.meanScatteringCosine,
              reversed.diagnostics.meanScatteringCosine, 1e-14);
  EXPECT_EQ(original.diagnostics.paths, 17u * 9u * 32u);
  EXPECT_GT(original.diagnostics.truncatedPaths, 0u);
}

TEST(OpenVdbTransportImageCpu, CameraSceneAndAggregateLimitsFailExplicitly) {
  const vdb::TransportScene scene;
  const auto valid = imageOptions(1, 1, 1);
  for (unsigned mode = 0; mode < 7; ++mode) {
    vdb::TransportPinholeCamera camera;
    if (mode == 0) camera.target = camera.eye;
    if (mode == 1) camera.up = {};
    if (mode == 2) camera.up = {0, 0, -1};
    if (mode == 3) camera.eye[0] = std::numeric_limits<double>::quiet_NaN();
    if (mode == 4) camera.verticalFovDegrees = 0;
    if (mode == 5) camera.verticalFovDegrees = 151;
    if (mode == 6) camera.verticalFovDegrees = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)vdb::RenderTransportImageCpu(scene, camera, valid), std::exception) << mode;
  }
  for (unsigned mode = 0; mode < 10; ++mode) {
    auto o = valid;
    if (mode == 0) o.width = 0;
    if (mode == 1) o.height = 2049;
    if (mode == 2) {o.width = o.height = 1024;}
    if (mode == 3) {o.width = 2; o.maxPixels = 1;}
    if (mode == 4) {o.width = 2; o.maxPaths = 1;}
    if (mode == 5) o.transport.samplesPerRay = 8193;
    if (mode == 6) o.transport.maxWork = 1;
    if (mode == 7) o.maxImageBytes = 1024;
    if (mode == 8) o.transport.maxInputBytes = 1;
    if (mode == 9) o.transport.maxScatteringEvents = 65;
    EXPECT_THROW((void)vdb::RenderTransportImageCpu(scene, {}, o), std::exception) << mode;
  }
  auto invalid = slab(1);
  invalid.media[0].density[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)vdb::RenderTransportImageCpu(invalid, {}, valid), std::exception);
}

class OpenVdbTransportImageFiles : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-transport-image-tests-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
         std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
  }
  void TearDown() override {
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), error);
    std::filesystem::remove(directory, error);
  }
  auto path(const std::string &name) const {return directory / name;}
  static std::string bytes(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  std::filesystem::path directory;
};
struct ExrRead {
  EXRHeader header;
  EXRImage image;
  const char *error = nullptr;
  ExrRead() {InitEXRHeader(&header); InitEXRImage(&image);}
  ~ExrRead() {FreeEXRImage(&image); FreeEXRHeader(&header); if (error) FreeEXRErrorMessage(error);}
  std::string metadata() const {
    for (int n = 0; n < header.num_custom_attributes; ++n) {
      const auto &a = header.custom_attributes[n];
      if (std::string_view(a.name) == "container.transport")
        return {reinterpret_cast<const char *>(a.value), size_t(a.size)};
    }
    return {};
  }
};

TEST_F(OpenVdbTransportImageFiles, ExrContainsSixLosslessFloatPlanesAndConversionDiagnostics) {
  auto image = vdb::RenderTransportImageCpu({}, {}, imageOptions(2, 2, 4));
  image.pixels = {{{1. / 3, 2, 3}, {.125, .25, .5}},
                  {{4, 5, 6}, {.5, 1, 2}}, {{7, 8, 9}, {1, 2, 4}},
                  {{10, 11, 12}, {2, 4, 8}}};
  const auto result = vdb::WriteTransportImageExr(path("image.exr"), image);
  EXPECT_EQ(result.encodedBytes, std::filesystem::file_size(path("image.exr")));
  EXPECT_EQ(result.roundedValues, 1u);
  const double error = std::abs(double(float(1. / 3)) - 1. / 3);
  EXPECT_DOUBLE_EQ(result.maxAbsoluteError, error);
  EXPECT_DOUBLE_EQ(result.maxRelativeError, error / (1. / 3));
  EXRVersion version{};
  ASSERT_EQ(ParseEXRVersionFromFile(&version, path("image.exr").string().c_str()), TINYEXR_SUCCESS);
  ExrRead read;
  ASSERT_EQ(ParseEXRHeaderFromFile(&read.header, &version, path("image.exr").string().c_str(), &read.error), TINYEXR_SUCCESS);
  ASSERT_EQ(read.header.num_channels, 6);
  EXPECT_EQ(read.header.compression_type, TINYEXR_COMPRESSIONTYPE_NONE);
  const std::array<const char *, 6> names{"B", "G", "R", "SE.B", "SE.G", "SE.R"};
  for (size_t c = 0; c < 6; ++c) {
    EXPECT_STREQ(read.header.channels[c].name, names[c]);
    EXPECT_EQ(read.header.pixel_types[c], TINYEXR_PIXELTYPE_FLOAT);
  }
  ASSERT_EQ(LoadEXRImageFromFile(&read.image, &read.header, path("image.exr").string().c_str(), &read.error), TINYEXR_SUCCESS);
  ASSERT_EQ(read.image.width, 2); ASSERT_EQ(read.image.height, 2);
  for (size_t c = 0; c < 6; ++c) {
    const auto *data = reinterpret_cast<const float *>(read.image.images[c]);
    for (size_t i = 0; i < 4; ++i) {
      const auto &p = image.pixels[i];
      EXPECT_EQ(data[i], float(c < 3 ? p.radiance[2 - c] : p.standardError[5 - c]));
    }
  }
  const auto metadata = read.metadata();
  for (const auto text : {"schema=1", "backend=CPU Double", "seed=73", "spp=4",
                          "width=2", "height=2", "center_pixel_rays=1",
                          "uncertainty_available=1", "rounded_float_values=1"})
    EXPECT_NE(metadata.find(text), std::string::npos) << text;
}

TEST_F(OpenVdbTransportImageFiles, OneSampleTagsUncertaintyUnavailableAndExportProtectsFiles) {
  vdb::TransportScene scene; scene.environment = {.25, .5, 1};
  auto image = vdb::RenderTransportImageCpu(scene, {}, imageOptions(1, 1, 1));
  EXPECT_EQ(image.pixels[0].standardError, (V{}));
  (void)vdb::WriteTransportImageExr(path("one.exr"), image);
  const auto original = bytes(path("one.exr"));
  EXPECT_THROW((void)vdb::WriteTransportImageExr(path("one.exr"), image), std::exception);
  EXPECT_EQ(bytes(path("one.exr")), original);
  EXRVersion version{}; ExrRead read;
  ASSERT_EQ(ParseEXRVersionFromFile(&version, path("one.exr").string().c_str()), TINYEXR_SUCCESS);
  ASSERT_EQ(ParseEXRHeaderFromFile(&read.header, &version, path("one.exr").string().c_str(), &read.error), TINYEXR_SUCCESS);
  EXPECT_NE(read.metadata().find("uncertainty_available=0"), std::string::npos);
  EXPECT_THROW((void)vdb::WriteTransportImageExr(path("budget.exr"), image, 1024), std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("budget.exr")));
  for (double value : {-1., std::numeric_limits<double>::quiet_NaN(),
                        2 * double(std::numeric_limits<float>::max()),
                        std::numeric_limits<double>::denorm_min()}) {
    image.pixels[0].radiance[0] = value;
    EXPECT_THROW((void)vdb::WriteTransportImageExr(path("invalid.exr"), image), std::exception);
    EXPECT_FALSE(std::filesystem::exists(path("invalid.exr")));
  }
  EXPECT_EQ(bytes(path("one.exr")), original);
}

TEST(OpenVdbTransportImageSceneAvailability, DisabledFailsBeforePathsAndDescriptionOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::TransportImageSceneDescription description;
  description.maxInputBytes = 0;
  description.volumes.resize(5);
  description.volumes[0].input = "missing.vdb";
  try {(void)vdb::LoadTransportImageScene(description); FAIL();}
  catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
TEST_F(OpenVdbTransportImageFiles, DenseSceneRetainsImmutableSourcesMaterialsAndLoaderDiagnostics) {
  openvdb::initialize();
  auto grid = openvdb::FloatGrid::create(0);
  grid->setName("density"); grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->fill(openvdb::CoordBBox({0, 0, 0}, {3, 3, 3}), .5f, true);
  {openvdb::io::File file(path("density.vdb").string()); file.write({grid});}
  const auto original = bytes(path("density.vdb"));
  vdb::TransportImageSceneDescription description;
  vdb::TransportImageVolumeSource source;
  source.input = path("density.vdb"); source.volume.maxResolution = 16;
  source.material = {.7, {.1, .2, .3}, .4, {.2, .4, .6}};
  description.volumes.push_back(source);
  description.environment = {.25, .5, 1};
  description.lights.push_back({vdb::TransportLightType::Point, {0, 0, 10}, {1, 2, 3}});
  const auto loaded = vdb::LoadTransportImageScene(description);
  ASSERT_TRUE(loaded); ASSERT_EQ(loaded->sources().size(), 1u);
  ASSERT_EQ(loaded->scene().media.size(), 1u);
  EXPECT_FALSE(loaded->hasNativeSources());
  for (const auto &native : loaded->nativeSources()) EXPECT_FALSE(native);
  const auto &info = loaded->sources()[0];
  const auto &medium = loaded->scene().media[0];
  EXPECT_EQ(info.input, source.input); EXPECT_EQ(info.gridName, "density");
  EXPECT_FALSE(info.nativeSparse); EXPECT_EQ(info.sourceActiveVoxels, 64u);
  EXPECT_GT(info.retainedBytes, 0u); EXPECT_EQ(info.dimensions, medium.dimensions);
  EXPECT_EQ(info.boundsMin, medium.boundsMin); EXPECT_EQ(info.boundsMax, medium.boundsMax);
  EXPECT_EQ(medium.albedo, source.material.albedo); EXPECT_DOUBLE_EQ(medium.extinction, .7);
  EXPECT_DOUBLE_EQ(medium.anisotropy, .4); EXPECT_EQ(medium.emission, source.material.emission);
  EXPECT_EQ(loaded->scene().environment, description.environment);
  ASSERT_EQ(loaded->scene().lights.size(), 1u);
  EXPECT_EQ(loaded->scene().lights[0].vector, (V{0, 0, 10}));
  EXPECT_EQ(bytes(path("density.vdb")), original);
  const auto retained = medium.density;
  ASSERT_FALSE(retained.empty()); EXPECT_GT(*std::max_element(retained.begin(), retained.end()), 0);
  description.environment = {}; description.volumes[0].material.extinction = 9;
  std::filesystem::remove(path("density.vdb"));
  EXPECT_EQ(medium.density, retained); EXPECT_DOUBLE_EQ(medium.extinction, .7);
  EXPECT_EQ(loaded->scene().environment, (V{.25, .5, 1}));
  EXPECT_NO_THROW((void)vdb::RenderTransportImageCpu(loaded->scene(), {}, imageOptions(1, 1, 8)));
}

TEST_F(OpenVdbTransportImageFiles, SceneSourceCountTemperatureAndCombinedDensityLimitsReject) {
  openvdb::initialize();
  auto grid = openvdb::FloatGrid::create(0);
  grid->setName("density"); grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->fill(openvdb::CoordBBox({0, 0, 0}, {1, 1, 1}), 1.f, true);
  {openvdb::io::File file(path("source.vdb").string()); file.write({grid});}
  vdb::TransportImageSceneDescription description;
  description.volumes.push_back({path("source.vdb")});
  description.volumes[0].volume.maxResolution = 16;
  const auto one = vdb::LoadTransportImageScene(description);
  const auto count = one->scene().media[0].density.size();
  description.volumes.push_back(description.volumes.front());
  description.maxDensityValues = count;
  EXPECT_THROW((void)vdb::LoadTransportImageScene(description), std::exception);
  description.maxDensityValues = count * 2;
  EXPECT_EQ(vdb::LoadTransportImageScene(description)->scene().media.size(), 2u);
  description.volumes.resize(5);
  EXPECT_THROW((void)vdb::LoadTransportImageScene(description), std::exception);
  description.volumes.resize(1);
  description.volumes[0].input = path("missing.vdb");
  description.volumes[0].volume.temperatureGridName = "temperature";
  try {(void)vdb::LoadTransportImageScene(description); FAIL();}
  catch (const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("temperature"), std::string::npos);
  }
}
#endif
} // namespace
