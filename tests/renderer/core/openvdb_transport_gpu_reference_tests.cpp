#include "Container/renderer/effects/OpenVdbTransportGpuReference.h"
#include "Container/utility/VulkanInstance.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numbers>
#include <string>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
namespace renderer = container::renderer;
using V = vdb::TransportVector;
const std::array<vdb::TransportRay, 1> view{{{{0, 0, -2}, {0, 0, 1}}}};

renderer::TransportGpuReferenceOptions options(uint32_t samples = 65'536,
                                               uint32_t order = 0) {
  renderer::TransportGpuReferenceOptions result;
  result.transport.samplesPerRay = samples;
  result.transport.maxScatteringEvents = order;
  result.transport.rouletteStart = 65;
  result.maxWorkPerPath = 16'384;
  return result;
}
vdb::TransportScene slab(double extinction = 1) {
  vdb::TransportScene scene;
  vdb::TransportMedium medium;
  medium.extinction = extinction;
  medium.albedo = {0, 0, 0};
  scene.media.push_back(medium);
  return scene;
}
void finite(const vdb::TransportReferenceResult &result) {
  for (const auto &ray : result.rays)
    for (size_t channel = 0; channel < 3; ++channel) {
      EXPECT_TRUE(std::isfinite(ray.radiance[channel]));
      EXPECT_TRUE(std::isfinite(ray.standardError[channel]));
      EXPECT_GE(ray.radiance[channel], 0);
      EXPECT_GE(ray.standardError[channel], 0);
    }
}
void analytic(const vdb::TransportRayEstimate &actual, V expected,
              double numericalSlack = 5e-4) {
  for (size_t channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(actual.radiance[channel], expected[channel],
                6 * actual.standardError[channel] + numericalSlack);
}
void agreement(const vdb::TransportRayEstimate &gpu,
               const vdb::TransportRayEstimate &cpu,
               double numericalSlack = 5e-4) {
  for (size_t channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(
        gpu.radiance[channel], cpu.radiance[channel],
        6 * std::hypot(gpu.standardError[channel], cpu.standardError[channel]) +
            numericalSlack);
}
double hgForward(double g) {
  return (1 - g * g) /
         (4 * std::numbers::pi * std::pow(1 + g * g - 2 * g, 1.5));
}

// Surface-free production API test. Device resources are deliberately kept
// alive until the reference's owned pipeline/submission resources retire.
class OpenVdbTransportGpuReference : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<OpenVdbTransportGpuReference *>(user)->validationErrors_;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    container::gpu::InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const auto sync =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    info.next = &validation;
    instance_ = std::make_unique<container::gpu::VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback =
        reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(
            validationCallback);
    debug.pUserData = this;
    messenger_ = vk::raii::DebugUtilsMessengerEXT(instance_->raii(), debug);
    for (auto &candidate : instance_->raii().enumeratePhysicalDevices()) {
      if (candidate.getProperties().apiVersion < VK_API_VERSION_1_4)
        continue;
      const auto features =
          candidate.getFeatures2<vk::PhysicalDeviceFeatures2,
                                 vk::PhysicalDeviceVulkan12Features>();
      if (!features.get<vk::PhysicalDeviceVulkan12Features>()
               .bufferDeviceAddress)
        continue;
      const auto families = candidate.getQueueFamilyProperties();
      for (uint32_t index = 0; index < families.size(); ++index)
        if (families[index].queueCount &&
            (families[index].queueFlags & vk::QueueFlagBits::eCompute)) {
          family_ = index;
          physical_ = std::move(candidate);
          break;
        }
      if (*physical_)
        break;
    }
    ASSERT_TRUE(*physical_)
        << "Vulkan 1.4 compute with VMA buffer-device-address required";
    const float priority = 1;
    vk::DeviceQueueCreateInfo queue({}, family_, 1, &priority);
    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    vk::DeviceCreateInfo create({}, queue);
    create.pNext = &v12;
    device_ = vk::raii::Device(physical_, create);
    queue_ = device_.getQueue(family_, 0);
    memory_ = std::make_unique<container::gpu::VulkanMemoryManager>(
        instance_->instance(), *physical_, *device_);
    reference_ = std::make_unique<renderer::OpenVdbTransportGpuReference>(
        renderer::TransportGpuDevice{*physical_, *device_, *queue_, family_,
                                     memory_.get()},
        CONTAINER_OPENVDB_TRANSPORT_REFERENCE_SPIRV);
  }
  void TearDown() override {
    reference_.reset();
    if (*device_)
      device_.waitIdle();
    memory_.reset();
    queue_.clear();
    device_.clear();
    physical_.clear();
    messenger_.clear();
    instance_.reset();
    EXPECT_EQ(validationErrors_.load(), 0u);
  }
  renderer::TransportGpuReferenceResult
  trace(const vdb::TransportScene &scene,
        renderer::TransportGpuReferenceOptions o = options()) {
    auto result = reference_->trace(scene, view, o);
    EXPECT_EQ(result.rays.size(), view.size());
    EXPECT_EQ(result.paths, o.transport.samplesPerRay);
    EXPECT_GE(result.work, result.paths);
    EXPECT_LE(result.work, o.transport.maxWork);
    finite(result);
    return result;
  }
  std::atomic_uint validationErrors_{};
  std::unique_ptr<container::gpu::VulkanInstance> instance_;
  vk::raii::DebugUtilsMessengerEXT messenger_{nullptr};
  vk::raii::PhysicalDevice physical_{nullptr};
  vk::raii::Device device_{nullptr};
  vk::raii::Queue queue_{nullptr};
  uint32_t family_{};
  std::unique_ptr<container::gpu::VulkanMemoryManager> memory_;
  std::unique_ptr<renderer::OpenVdbTransportGpuReference> reference_;
};

TEST_F(OpenVdbTransportGpuReference,
       VacuumAndTwoSidedBlackCameraGeometryAreExact) {
  vdb::TransportScene scene;
  scene.environment = {.25, .5, 2};
  auto result = trace(scene, options(32));
  EXPECT_EQ(result.rays[0].radiance, scene.environment);
  EXPECT_EQ(result.rays[0].standardError, (V{}));
  EXPECT_EQ(result.scatteringEvents, 0u);
  scene.occluders.push_back({{-2, -2, 0}, {2, -2, 0}, {0, 2, 0}});
  EXPECT_EQ(trace(scene, options(32)).rays[0].radiance, (V{}));
  std::swap(scene.occluders[0].a, scene.occluders[0].c);
  EXPECT_EQ(trace(scene, options(32)).rays[0].radiance, (V{}));
}

TEST_F(OpenVdbTransportGpuReference,
       AbsorptionMatchesBeerAndCpuAcrossIndependentSeeds) {
  auto scene = slab(.7);
  scene.environment = {1, .5, 2};
  const double t = std::exp(-1.4);
  for (uint64_t seed : {1ull, 17ull, 1234ull}) {
    SCOPED_TRACE(seed);
    auto o = options();
    o.transport.seed = seed;
    const auto actual = trace(scene, o);
    analytic(actual.rays[0], {t, .5 * t, 2 * t});
    agreement(actual.rays[0],
              vdb::TraceTransportReference(scene, view, o.transport).rays[0]);
    EXPECT_EQ(actual.scatteringEvents, 0u);
    EXPECT_EQ(actual.truncatedPaths, 0u);
  }
}

TEST_F(OpenVdbTransportGpuReference,
       MoreSamplesReduceEnsembleErrorAtTheSameScatteringOrder) {
  auto scene = slab(.5);
  scene.environment = {1, 1, 1};
  double lowError = 0, highError = 0, lowVariance = 0, highVariance = 0;
  for (uint64_t seed = 1; seed <= 8; ++seed) {
    auto low = options(64), high = options(8192);
    low.transport.seed = high.transport.seed = seed;
    const auto a = trace(scene, low), b = trace(scene, high);
    lowError += std::pow(a.rays[0].radiance[0] - std::exp(-1.), 2);
    highError += std::pow(b.rays[0].radiance[0] - std::exp(-1.), 2);
    lowVariance += std::pow(a.rays[0].standardError[0], 2);
    highVariance += std::pow(b.rays[0].standardError[0], 2);
    EXPECT_EQ(a.truncatedPaths, 0u);
    EXPECT_EQ(b.truncatedPaths, 0u);
  }
  EXPECT_LT(highError, lowError * .15);
  EXPECT_LT(highVariance, lowVariance * .025);
  RecordProperty("spp64_ensemble_squared_error", lowError);
  RecordProperty("spp8192_ensemble_squared_error", highError);
  RecordProperty("spp64_ensemble_estimated_variance", lowVariance);
  RecordProperty("spp8192_ensemble_estimated_variance", highVariance);
}

TEST_F(OpenVdbTransportGpuReference,
       IndependentEmissionMatchesVacuumAndAbsorbingSlabs) {
  auto scene = slab(0);
  scene.media[0].emission = {2, 1, .25};
  scene.environment = {.5, .25, 1};
  auto actual = trace(scene);
  analytic(actual.rays[0], {4.5, 2.25, 1.5});
  EXPECT_GT(actual.nullCollisions, 0u);
  EXPECT_EQ(actual.truncatedPaths, 0u);
  scene.media[0].extinction = .7;
  const double t = std::exp(-1.4), integral = (1 - t) / .7;
  for (double rate : {1., 4.}) {
    auto o = options();
    o.transport.emissionSamplingRate = rate;
    actual = trace(scene, o);
    analytic(actual.rays[0],
             {.5 * t + 2 * integral, .25 * t + integral, t + .25 * integral});
    agreement(actual.rays[0],
              vdb::TraceTransportReference(scene, view, o.transport).rays[0]);
  }
}

TEST_F(OpenVdbTransportGpuReference,
       HeterogeneousDensityAndOverlapUseTheCombinedEquation) {
  auto scene = slab();
  scene.environment = {1, 1, 1};
  scene.media[0].dimensions = {1, 1, 2};
  scene.media[0].density = {0, 2};
  renderer::TransportGpuReferenceResult actual;
  ASSERT_NO_THROW(actual = trace(scene));
  analytic(actual.rays[0], {std::exp(-2.), std::exp(-2.), std::exp(-2.)});
  scene = slab(.25);
  scene.media[0].albedo = {1, 0, 0};
  scene.media[0].anisotropy = .6;
  scene.media.push_back(scene.media[0]);
  scene.media[1].albedo = {0, 1, 0};
  scene.media[1].anisotropy = -.4;
  scene.lights.push_back(
      {vdb::TransportLightType::Directional, {0, 0, 1}, {1, 1, 1}});
  auto o = options(131'072, 1);
  ASSERT_NO_THROW(actual = trace(scene, o));
  const V expected{.5 * std::exp(-1) * hgForward(.6),
                   .5 * std::exp(-1) * hgForward(-.4), 0};
  analytic(actual.rays[0], expected);
  agreement(actual.rays[0],
            vdb::TraceTransportReference(scene, view, o.transport).rays[0]);
  EXPECT_GT(actual.shadowProposals, 0u);
  EXPECT_GT(actual.shadowRouletteTerminations, 0u);
  std::swap(scene.media[0], scene.media[1]);
  o.transport.seed = 73;
  analytic(trace(scene, o).rays[0], expected);
}

TEST_F(OpenVdbTransportGpuReference,
       PhaseSamplesHaveThePhysicalHenyeyGreensteinMean) {
  auto scene = slab(.5);
  scene.media[0].albedo = {1, 1, 1};
  for (double g : {-.95, -.7, 0., 1e-10, .7, .95}) {
    SCOPED_TRACE(g);
    scene.media[0].anisotropy = g;
    const auto actual = trace(scene, options(65'536, 1));
    EXPECT_GT(actual.phaseSamples, 30'000u);
    EXPECT_NEAR(actual.meanScatteringCosine, g, .012);
  }
}

TEST_F(OpenVdbTransportGpuReference,
       ConservativeEnvironmentAndRoulettePreserveExpectation) {
  auto scene = slab(.25);
  scene.media[0].albedo = {1, 1, 1};
  scene.environment = {1, .5, .25};
  auto o = options(32'768, 16);
  const auto full = trace(scene, o);
  EXPECT_EQ(full.truncatedPaths, 0u);
  analytic(full.rays[0], scene.environment, 2e-6);
  o.transport.rouletteStart = 1;
  const auto roulette = trace(scene, o);
  analytic(roulette.rays[0], scene.environment);
  EXPECT_GT(roulette.rouletteTerminations, 0u);
}

TEST_F(OpenVdbTransportGpuReference,
       HigherSpatialOrdersRouteLightAroundABlackWall) {
  auto scene = slab(.7);
  scene.media[0].boundsMin = {-2, -2, -2};
  scene.media[0].boundsMax = {2, 2, 2};
  scene.media[0].albedo = {.9, .9, .9};
  scene.lights.push_back(
      {vdb::TransportLightType::Directional, {1, 0, 0}, {1, 1, 1}});
  scene.occluders = {{{0, -.5, -2}, {0, .5, -2}, {0, .5, 2}},
                     {{0, -.5, -2}, {0, .5, 2}, {0, -.5, 2}}};
  const std::array<vdb::TransportRay, 1> wallView{{{{-3, 0, 0}, {1, 0, 0}}}};
  auto singleOptions = options(65'536, 1),
       multipleOptions = options(131'072, 3);
  renderer::TransportGpuReferenceResult single, multiple;
  ASSERT_NO_THROW(single = reference_->trace(scene, wallView, singleOptions));
  ASSERT_NO_THROW(multiple =
                      reference_->trace(scene, wallView, multipleOptions));
  finite(single);
  finite(multiple);
  EXPECT_EQ(single.rays[0].radiance, (V{}));
  EXPECT_GT(multiple.rays[0].radiance[0],
            8 * multiple.rays[0].standardError[0]);
  EXPECT_GT(multiple.rays[0].radiance[0], 1e-4);
  EXPECT_GT(multiple.truncatedPaths, 0u);
  EXPECT_GT(multiple.scatteringEvents, single.scatteringEvents);
  EXPECT_GT(multiple.shadowRouletteTerminations, 0u);
  agreement(multiple.rays[0], vdb::TraceTransportReference(
                                  scene, wallView, multipleOptions.transport)
                                  .rays[0]);
}

TEST_F(OpenVdbTransportGpuReference,
       PointIntensityIsLinearAndHasInverseSquareUnits) {
  auto scene = slab(.15);
  scene.media[0].albedo = {.8, .6, .4};
  scene.lights.push_back(
      {vdb::TransportLightType::Point, {0, 0, 3}, {2, 2, 2}});
  auto o = options(65'536, 1);
  const auto original = trace(scene, o);
  scene.lights[0].value = {4, 4, 4};
  const auto doubled = trace(scene, o);
  for (size_t channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(doubled.rays[0].radiance[channel],
                2 * original.rays[0].radiance[channel], 1e-6);
  scene.lights[0].value = {2, 2, 2};
  scene.lights[0].vector = {0, 0, 100};
  const auto near = trace(scene, o);
  scene.lights[0].vector = {0, 0, 200};
  const auto far = trace(scene, o);
  EXPECT_GT(far.rays[0].radiance[0], 0);
  EXPECT_NEAR(near.rays[0].radiance[0] / far.rays[0].radiance[0], 4, .06);
}

TEST_F(OpenVdbTransportGpuReference,
       RepeatableResultsAndBoundedFailuresLeaveTheApiUsable) {
  auto scene = slab(.7);
  scene.environment = {1, .5, 2};
  scene.media[0].albedo = {.4, .6, .8};
  auto o = options(4096, 4);
  o.transport.seed = 987654;
  const auto a = trace(scene, o), b = trace(scene, o);
  EXPECT_EQ(a.rays[0].radiance, b.rays[0].radiance);
  EXPECT_EQ(a.rays[0].standardError, b.rays[0].standardError);
  EXPECT_EQ(a.work, b.work);
  EXPECT_EQ(a.scatteringEvents, b.scatteringEvents);
  EXPECT_EQ(a.phaseSamples, b.phaseSamples);
  EXPECT_EQ(a.meanScatteringCosine, b.meanScatteringCosine);
  auto limited = o;
  limited.maxWorkPerPath = 1;
  EXPECT_THROW((void)reference_->trace(scene, view, limited), std::exception);
  limited = o;
  limited.transport.maxInputBytes = 1;
  EXPECT_THROW((void)reference_->trace(scene, view, limited), std::exception);
  limited = o;
  limited.transport.maxWork = 1;
  EXPECT_THROW((void)reference_->trace(scene, view, limited), std::exception);
  limited = o;
  limited.transport.maxScatteringEvents = 17;
  EXPECT_THROW((void)reference_->trace(scene, view, limited), std::exception);
  scene.media[0].density[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)reference_->trace(scene, view, o), std::exception);
  scene.media[0].density[0] = 1;
  scene.media[0].extinction = std::numeric_limits<double>::denorm_min();
  EXPECT_THROW((void)reference_->trace(scene, view, o), std::exception);
  scene.media[0].extinction = .7;
  EXPECT_EQ(trace(scene, o).rays[0].radiance, a.rays[0].radiance);
  scene.media[0].boundsMin = {1e12, 1e12, 1e12};
  scene.media[0].boundsMax = {1e12 + 2, 1e12 + 2, 1e12 + 2};
  EXPECT_THROW((void)reference_->trace(scene, view, o), std::exception);
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB &&                 \
    defined(CONTAINER_HAS_NANOVDB) && CONTAINER_HAS_NANOVDB
struct NativeFixture {
  std::filesystem::path directory;
  NativeFixture() {
    static std::atomic_uint64_t ordinal{};
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      directory = std::filesystem::temp_directory_path() /
                  ("container-transport-gpu-" + std::to_string(ticks) + "-" +
                   std::to_string(ordinal++));
      if (std::filesystem::create_directory(directory))
        return;
    }
    throw std::runtime_error("Cannot create GPU native fixture directory");
  }
  ~NativeFixture() {
    std::error_code ignored;
    std::filesystem::remove(directory / "density.vdb", ignored);
    std::filesystem::remove(directory, ignored);
  }
};
TEST_F(OpenVdbTransportGpuReference,
       NativeFactoryMatchesDenseAndAnalyticBoundaryProfile) {
  openvdb::initialize();
  NativeFixture fixture;
  const auto path = fixture.directory / "density.vdb";
  // Zero edge centers make the native zero-background interpolation continuous.
  // The profile integrates to six voxels. Reflection + quarter turn + import
  // scale exercise the native map independently of the world-aligned lattice.
  for (unsigned variant = 0; variant < 3; ++variant) {
    SCOPED_TRACE(variant);
    const bool useDouble = variant != 0, clamped = variant == 2;
    auto grid = openvdb::DoubleGrid::create(0);
    grid->setName("density");
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    auto transform = openvdb::math::Transform::createLinearTransform(.25);
    transform->postScale(openvdb::Vec3d(-1, 1, 1));
    transform->postRotate(std::numbers::pi / 2, openvdb::math::Z_AXIS);
    transform->postTranslate(openvdb::Vec3d(2, -1, .5));
    grid->setTransform(transform);
    const double value = clamped ? 1e9 : 1;
    for (int z = 0; z < 8; ++z)
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
          grid->tree().setValue(openvdb::Coord(x, y, z),
                                z == 0 || z == 7 ? 0 : value);
    openvdb::GridBase::Ptr authored = grid;
    if (!useDouble) {
      auto floats = openvdb::FloatGrid::create(0);
      floats->setName("density");
      floats->setGridClass(openvdb::GRID_FOG_VOLUME);
      floats->setTransform(transform->copy());
      for (auto it = grid->cbeginValueOn(); it; ++it)
        floats->tree().setValue(it.getCoord(), static_cast<float>(*it));
      authored = floats;
    }
    {
      openvdb::io::File output(path.string());
      output.write(openvdb::GridPtrVec{authored});
    }
    container::geometry::OpenVdbVolumeOptions loader;
    loader.importScale = 1.5f;
    loader.maxResolution = 128;
    const auto dense = container::geometry::loadOpenVdbVolume(path, loader);
    loader.sparse = true;
    auto source = renderer::LoadTransportGpuNativeSource(path, loader);
    ASSERT_FALSE(source->volume().sparseDensity.empty());
    ASSERT_TRUE(source->volume().density.empty());
    vdb::TransportScene denseScene;
    denseScene.media.push_back(vdb::MakeTransportMedium(dense));
    denseScene.media[0].extinction = clamped ? 4e-7 : .4;
    denseScene.media[0].albedo = {0, 0, 0};
    denseScene.environment = {1, .5, 2};
    auto nativeScene = denseScene;
    auto &medium = nativeScene.media[0];
    const auto &volume = source->volume();
    medium.dimensions = volume.dimensions;
    medium.density
        .clear(); // Native factory replaces this payload, not the material.
    for (size_t axis = 0; axis < 3; ++axis) {
      medium.boundsMin[axis] = volume.boundsMin[axis];
      medium.boundsMax[axis] = volume.boundsMax[axis];
    }
    const auto center =
        transform->indexToWorld(openvdb::Vec3d(1.5, 1.5, 3.5)) * 1.5;
    const std::array<vdb::TransportRay, 1> nativeView{
        {{{center.x(), center.y(), volume.boundsMin[2] - 1}, {0, 0, 1}}}};
    const auto o = options();
    const std::array<std::shared_ptr<const renderer::TransportGpuNativeSource>,
                     4>
        sources{source};
    const auto native = reference_->trace(nativeScene, nativeView, o, sources);
    finite(native);
    const double t = std::exp(-.9); // six * .25 * 1.5 * density * extinction.
    analytic(native.rays[0], {t, .5 * t, 2 * t});
    if (!clamped) {
      const auto preview = reference_->trace(denseScene, nativeView, o);
      const auto cpu =
          vdb::TraceTransportReference(denseScene, nativeView, o.transport);
      finite(preview);
      agreement(native.rays[0], preview.rays[0], 1e-3);
      agreement(native.rays[0], cpu.rays[0], 1e-3);
    }
    // Sparse loading clamps stored Double values before interpolation. Dense
    // loading clamps the interpolated source value; an out-of-range ramp is
    // therefore deliberately not a parity fixture. The native clamped profile
    // still has the independently known integral checked above.
    if (variant == 0) {
      auto mixture = nativeScene;
      auto nativeMedium = mixture.media.front();
      nativeMedium.extinction /= 3;
      vdb::TransportMedium densePrefix;
      densePrefix.density = {0};
      densePrefix.extinction = 0;
      mixture.media = {densePrefix, nativeMedium, nativeMedium, nativeMedium};
      const std::array<
          std::shared_ptr<const renderer::TransportGpuNativeSource>, 4>
          mixedSources{nullptr, source, source, source};
      // One dense uint word requires seven padding words before the first
      // 32-byte native grid, followed by two independent native offsets.
      const auto packed =
          reference_->trace(mixture, nativeView, o, mixedSources);
      finite(packed);
      analytic(packed.rays[0], {t, .5 * t, 2 * t});
      agreement(packed.rays[0], native.rays[0]);
    }
    nativeScene.media[0].boundsMax[0] += 1;
    EXPECT_THROW((void)reference_->trace(nativeScene, nativeView, o, sources),
                 std::exception);
  }
}
#endif
} // namespace

int main(int argc, char **argv) {
  const char *enabled =
      std::getenv("CONTAINER_RUN_OPENVDB_TRANSPORT_REFERENCE_GPU");
  if (!enabled || std::strcmp(enabled, "1") != 0) {
    std::puts("SKIP: set CONTAINER_RUN_OPENVDB_TRANSPORT_REFERENCE_GPU=1 for "
              "transport GPU tests");
    return 77;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
