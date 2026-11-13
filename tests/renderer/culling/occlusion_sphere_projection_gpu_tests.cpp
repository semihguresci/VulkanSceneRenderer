#include <Container/common/CommonGLFW.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/VulkanDevice.h>

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>

namespace {
using namespace container::gpu;

struct alignas(16) SphereQuery {
  glm::mat4 viewProj;
  glm::vec4 sphere;
};
static_assert(sizeof(SphereQuery) == 80);

struct alignas(16) SphereResult {
  glm::vec4 bounds;
  glm::vec4 depthAndValid;
};
static_assert(sizeof(SphereResult) == 32);

struct Lens {
  double scaleX;
  double scaleY;
  double nearPlane;
  double farPlane; // Zero denotes infinite perspective.
  bool orthographic = false;
};

struct ProjectionCase {
  const char* name;
  Lens lens;
  glm::dvec3 viewCenter;
  double radius;
  glm::dvec2 lensShift{};
  glm::dvec2 jitter{};
  glm::dmat4 view{1.0};
  bool expectUncertain = false;
};

glm::dmat4 projection(const ProjectionCase& test) {
  const auto& lens = test.lens;
  glm::dmat4 result(0.0);
  result[0][0] = lens.scaleX;
  result[1][1] = lens.scaleY;
  if (lens.orthographic) {
    result[2][2] = 1.0 / (lens.farPlane - lens.nearPlane);
    result[3] = glm::dvec4(test.lensShift, lens.farPlane * result[2][2], 1.0);
  } else {
    const double inverseRange = lens.farPlane == 0.0 ? 0.0 : 1.0 / (lens.farPlane - lens.nearPlane);
    result[2] = {-test.lensShift.x, -test.lensShift.y, lens.nearPlane * inverseRange, -1.0};
    result[3][2] = lens.nearPlane * (1.0 + lens.nearPlane * inverseRange);
  }
  glm::dmat4 clipTranslation(1.0);
  clipTranslation[3] = glm::dvec4(test.jitter, 0.0, 1.0);
  return clipTranslation * result;
}

SphereQuery query(const ProjectionCase& test) {
  const glm::dvec4 worldCenter = glm::inverse(test.view) * glm::dvec4(test.viewCenter, 1.0);
  return {glm::mat4(projection(test) * test.view),
          glm::vec4(glm::vec3(worldCenter), static_cast<float>(test.radius))};
}

struct ExpectedProjection {
  glm::dvec4 bounds;
  double closestDepth;
};

std::optional<ExpectedProjection> expectedProjection(const ProjectionCase& test, const SphereQuery& uploaded) {
  // Work in camera space with the actual uploaded sphere. The projected
  // slopes are the tangents to the circle in each (axis, view-depth) plane.
  const glm::dvec4 viewCenter = test.view * glm::dvec4(glm::dvec3(uploaded.sphere), 1.0);
  const double distance = -viewCenter.z;
  const double radius = uploaded.sphere.w;
  const double nearest = distance - radius;
  const auto& lens = test.lens;
  if (nearest <= lens.nearPlane) return std::nullopt;
  const auto axisBounds = [&](double axis, double scale, double shift) {
    if (lens.orthographic)
      return glm::dvec2(axis - radius, axis + radius) * scale + shift;
    const double denominator = distance * distance - radius * radius;
    const double spread = radius * std::sqrt(denominator + axis * axis);
    return glm::dvec2(axis * distance - spread, axis * distance + spread)
        * (scale / denominator) + shift;
  };
  const auto x = axisBounds(viewCenter.x, lens.scaleX, test.lensShift.x + test.jitter.x);
  const auto y = axisBounds(viewCenter.y, lens.scaleY, test.lensShift.y + test.jitter.y);
  const glm::dvec4 bounds = glm::clamp(
      glm::dvec4(x.x, -y.y, x.y, -y.x) * 0.5 + 0.5, 0.0, 1.0);
  double depth = lens.nearPlane / nearest;
  if (lens.orthographic) {
    depth = (lens.farPlane - nearest) / (lens.farPlane - lens.nearPlane);
  } else if (lens.farPlane > 0.0) {
    depth *= (lens.farPlane - nearest) / (lens.farPlane - lens.nearPlane);
  }
  return ExpectedProjection{bounds, depth};
}

class OcclusionSphereProjectionGpu : public testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<OcclusionSphereProjectionGpu*>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const auto* enabled = std::getenv("CONTAINER_RUN_GPU_CULL_GENERATIONS");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_CULL_GENERATIONS=1";
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "Occlusion sphere projection", nullptr, nullptr);
    ASSERT_NE(window, nullptr);

    uint32_t extensionCount = 0;
    const auto* extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions, extensions + extensionCount);
    info.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    info.next = &validation;
    instance = std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(validationCallback);
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);

    VkSurfaceKHR handle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(), window, nullptr, &handle), VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), handle);
    DeviceCreateInfo deviceInfo;
    deviceInfo.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    deviceInfo.next = &v12;
    device = std::make_unique<VulkanDevice>(*instance, handle, deviceInfo);
    commandPool = vk::raii::CommandPool(device->raii(), vk::CommandPoolCreateInfo(
        {}, device->queueFamilyIndices().graphicsFamily.value()));
    allocation = std::make_unique<AllocationManager>();
    allocation->initialize(instance->instance(), device->physicalDevice(), device->device(),
                           device->graphicsQueue(), *commandPool, {});
  }

  void TearDown() override {
    if (device) device->raii().waitIdle();
    allocation.reset();
    commandPool.clear();
    device.reset();
    surface.clear();
    messenger.clear();
    instance.reset();
    if (window) glfwDestroyWindow(window);
    if (glfwStarted) glfwTerminate();
    EXPECT_EQ(validationErrors.load(), 0u);
  }

  template <size_t Count>
  void dispatch(const std::array<SphereQuery, Count>& input,
                std::array<SphereResult, Count>& output) {
    const std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
    };
    const vk::raii::DescriptorSetLayout setLayout(
        device->raii(), vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const vk::raii::PipelineLayout layout(device->raii(), vk::PipelineLayoutCreateInfo({}, *setLayout));
    PipelineManager pipelines(device->device());
    const auto pipeline = pipelines.createComputePipeline(
        CONTAINER_OCCLUSION_SPHERE_SPIRV, *layout, "occlusion-sphere-contract");
    ASSERT_NE(static_cast<VkPipeline>(pipeline), VK_NULL_HANDLE);

    auto& memory = *allocation->memoryManager();
    struct Buffer {
      VulkanMemoryManager& memory;
      AllocatedBuffer allocation;
      ~Buffer() { memory.destroyBuffer(allocation); }
    };
    const Buffer queries{memory, memory.createBuffer(sizeof(input), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT)};
    const Buffer results{memory, memory.createBuffer(sizeof(output), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT)};
    ASSERT_NE(queries.allocation.allocation_info.pMappedData, nullptr);
    ASSERT_NE(results.allocation.allocation_info.pMappedData, nullptr);
    std::memcpy(queries.allocation.allocation_info.pMappedData, input.data(), sizeof(input));
    ASSERT_EQ(vmaFlushAllocation(memory.allocator(), queries.allocation.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);

    const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer, 2);
    const vk::raii::DescriptorPool pool(device->raii(), vk::DescriptorPoolCreateInfo(
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, size));
    const vk::raii::DescriptorSets sets(device->raii(), vk::DescriptorSetAllocateInfo(*pool, *setLayout));
    const std::array infos{
        vk::DescriptorBufferInfo(queries.allocation.buffer, 0, sizeof(input)),
        vk::DescriptorBufferInfo(results.allocation.buffer, 0, sizeof(output)),
    };
    const std::array writes{
        vk::WriteDescriptorSet(*sets.front(), 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &infos[0]),
        vk::WriteDescriptorSet(*sets.front(), 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &infos[1]),
    };
    device->raii().updateDescriptorSets(writes, {});
    const VkDescriptorSet set = *sets.front();
    allocation->executeStagedCommands([&](VkCommandBuffer command) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, 1, &set, 0, nullptr);
      vkCmdDispatch(command, static_cast<uint32_t>(Count), 1, 1);
      const VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 1, &hostRead, 0, nullptr, 0, nullptr);
    });
    ASSERT_EQ(vmaInvalidateAllocation(memory.allocator(), results.allocation.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);
    std::memcpy(output.data(), results.allocation.allocation_info.pMappedData, sizeof(output));
  }

 private:
  bool glfwStarted = false;
  GLFWwindow* window = nullptr;
  std::atomic_uint validationErrors{};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};
  std::unique_ptr<VulkanDevice> device;
  vk::raii::CommandPool commandPool{nullptr};
  std::unique_ptr<AllocationManager> allocation;
};

TEST_F(OcclusionSphereProjectionGpu, EnclosesIndependentTangentsAndNearestDepth) {
  const double scaleY = 1.0 / std::tan(std::numbers::pi / 6.0);
  const Lens perspective{scaleY * 9.0 / 16.0, scaleY, 0.1, 100.0};
  const Lens infinite{perspective.scaleX, perspective.scaleY, perspective.nearPlane, 0.0};
  const Lens ortho{0.25, 0.5, 0.1, 12.0, true};
  const glm::dvec3 eye{13.0, -6.0, 9.0};
  const glm::dvec3 direction{-0.3, 0.15, -1.0};
  const glm::dvec3 up{0.0, 1.0, 0.0};
  const glm::dmat4 view = glm::lookAt(eye, eye + direction, up);
  const glm::dvec3 distantEye{4194304.0, -2097152.0, 4194304.0};
  const glm::dmat4 distantView = glm::lookAt(distantEye, distantEye + direction, up);
  const std::array cases{
      ProjectionCase{"centered", perspective, {0.0, 0.0, -2.0}, 0.1},
      ProjectionCase{"off-axis infinite far", infinite, {0.12, 0.0, -0.12}, 0.005},
      ProjectionCase{"translated and rotated", perspective, {-0.75, 0.45, -2.5}, 0.2, {}, {}, view},
      ProjectionCase{"asymmetric and jittered", perspective, {0.64, -0.25, -1.4}, 0.09,
                     {0.14, -0.21}, {-0.003, 0.004}},
      ProjectionCase{"orthographic and jittered", ortho, {-1.3, 0.65, -2.0}, 0.17,
                     {0.09, -0.17}, {0.004, -0.002}},
      ProjectionCase{"perspective crosses near plane", perspective, {0.02, 0.0, -0.105}, 0.012},
      ProjectionCase{"perspective before near plane", infinite, {0.0, 0.0, -0.05}, 0.004},
      ProjectionCase{"orthographic crosses near plane", ortho, {0.2, -0.1, -0.105}, 0.012},
      ProjectionCase{"perspective crosses eye plane", perspective, {0.0, 0.0, -0.01}, 0.025},
      ProjectionCase{"large translation cancels in clip W", perspective, {0.25, 0.05, -2.0}, 0.125,
                     {}, {}, distantView, true},
  };
  constexpr std::array invalidReasons{"non-finite sphere center", "non-finite view projection"};
  std::array<SphereQuery, cases.size() + invalidReasons.size()> input{};
  for (size_t i = 0; i < cases.size(); ++i) input[i] = query(cases[i]);
  input[cases.size()] = input[0];
  input[cases.size()].sphere.x = std::numeric_limits<float>::quiet_NaN();
  input[cases.size() + 1] = input[0];
  input[cases.size() + 1].viewProj[3][3] = std::numeric_limits<float>::infinity();
  std::array<SphereResult, input.size()> output{};
  ASSERT_NO_FATAL_FAILURE(dispatch(input, output));

  constexpr double roundingSlack = 2e-6;
  constexpr double maximumExpansion = 2e-4;
  for (size_t i = 0; i < cases.size(); ++i) {
    const auto& test = cases[i];
    const auto& result = output[i];
    SCOPED_TRACE(test.name);
    const auto expected = expectedProjection(test, input[i]);
    if (test.expectUncertain) {
      // The double oracle still resolves a small sphere in front of the
      // camera. The float clip-W dot cancels terms of order millions.
      ASSERT_TRUE(expected.has_value());
      EXPECT_GT(expected->closestDepth, 0.02);
      EXPECT_LT(expected->closestDepth, 0.08);
      EXPECT_LT(expected->bounds[2] - expected->bounds[0], 0.2);
      EXPECT_EQ(result.depthAndValid.y, 0.0f);
      continue;
    }
    EXPECT_EQ(result.depthAndValid.y, expected ? 1.0f : 0.0f);
    if (!expected) continue;
    for (int axis = 0; axis < 2; ++axis) {
      EXPECT_GE(result.bounds[axis], 0.0f);
      EXPECT_LE(result.bounds[axis + 2], 1.0f);
      EXPECT_LE(result.bounds[axis], expected->bounds[axis] + roundingSlack);
      EXPECT_GE(result.bounds[axis + 2], expected->bounds[axis + 2] - roundingSlack);
      EXPECT_NEAR(result.bounds[axis], expected->bounds[axis], maximumExpansion);
      EXPECT_NEAR(result.bounds[axis + 2], expected->bounds[axis + 2], maximumExpansion);
    }
    EXPECT_GE(result.depthAndValid.x, expected->closestDepth - roundingSlack);
    EXPECT_NEAR(result.depthAndValid.x, expected->closestDepth, maximumExpansion);
  }
  for (size_t i = 0; i < invalidReasons.size(); ++i) {
    SCOPED_TRACE(invalidReasons[i]);
    EXPECT_EQ(output[cases.size() + i].depthAndValid.y, 0.0f);
  }
}
}  // namespace
