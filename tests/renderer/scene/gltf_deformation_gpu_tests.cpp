#include <Container/common/CommonGLFW.h>
#include <Container/renderer/scene/GltfDeformationPass.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/VulkanDevice.h>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#ifndef CONTAINER_GLTF_DEFORMATION_SHADER_DIR
#define CONTAINER_GLTF_DEFORMATION_SHADER_DIR "."
#endif

namespace {
using namespace container::gpu;
using namespace container::renderer;
using container::geometry::Vertex;
namespace gltf = container::geometry::gltf;

Vertex bindVertex(size_t index = 0) {
  Vertex vertex;
  vertex.position = glm::vec3(1.0f + static_cast<float>(index) * 0.01f,
                               static_cast<float>(index % 3) * 0.2f, -0.5f);
  vertex.normal = glm::normalize(glm::vec3(1, 2, 3));
  const glm::vec3 tangent(2, -1, 0);
  vertex.tangent = glm::vec4(glm::normalize(tangent), index % 2 == 0 ? 1 : -1);
  vertex.color = glm::vec3(0.25f, 0.5f, 0.75f);
  vertex.colorAlpha = static_cast<float>(index % 11) / 10.0f;
  vertex.texCoord = glm::vec2(0.2f, 0.8f);
  vertex.texCoord1 = glm::vec2(0.7f, 0.3f);
  return vertex;
}

void compareVertices(std::span<const Vertex> actual, std::span<const Vertex> expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    SCOPED_TRACE(i);
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(actual[i].position[c], expected[i].position[c], 2e-4f);
      EXPECT_NEAR(actual[i].normal[c], expected[i].normal[c], 2e-4f);
      EXPECT_NEAR(actual[i].tangent[c], expected[i].tangent[c], 2e-4f);
    }
    EXPECT_FLOAT_EQ(actual[i].tangent.w, expected[i].tangent.w);
    EXPECT_EQ(actual[i].color, expected[i].color);
    EXPECT_FLOAT_EQ(actual[i].colorAlpha, expected[i].colorAlpha);
    EXPECT_EQ(actual[i].texCoord, expected[i].texCoord);
    EXPECT_EQ(actual[i].texCoord1, expected[i].texCoord1);
  }
}

class GltfDeformationGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<GltfDeformationGpu*>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const char* enabled = std::getenv("CONTAINER_RUN_GPU_GLTF_DEFORMATION");
    if (!enabled || std::strcmp(enabled, "1") != 0) {
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_GLTF_DEFORMATION=1 for production Vulkan deformation checks";
    }
    const char* shaderOverride = std::getenv("CONTAINER_GLTF_DEFORMATION_SHADER_DIR");
    shaderDir = shaderOverride ? shaderOverride : CONTAINER_GLTF_DEFORMATION_SHADER_DIR;
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderDir / "spv_shaders" / "gltf_deformation.comp.spv"));
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "glTF deformation numerical contract", nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo create;
    create.enableValidationLayers = true;
    create.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions, extensions + count);
    create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    create.next = &validation;
    instance = std::make_unique<VulkanInstance>(create);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(validationCallback);
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);
    VkSurfaceKHR surfaceHandle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(), window, nullptr, &surfaceHandle), VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), surfaceHandle);
    DeviceCreateInfo deviceInfo;
    deviceInfo.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    deviceInfo.enableRayQueries = true;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    deviceInfo.next = &v12;
    device = std::make_shared<VulkanDevice>(*instance, surfaceHandle, deviceInfo);
    memory = std::make_unique<VulkanMemoryManager>(instance->instance(), device->physicalDevice(), device->device());
    pipelines = std::make_unique<PipelineManager>(device->device());
    pass = std::make_unique<GltfDeformationPass>(device, *memory, *pipelines);
    pass->createResources(shaderDir, 2);
    pool = vk::raii::CommandPool(device->raii(), vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        device->queueFamilyIndices().graphicsFamily.value()));
    auto commands = device->raii().allocateCommandBuffers(vk::CommandBufferAllocateInfo(
        *pool, vk::CommandBufferLevel::ePrimary, 1));
    command = std::move(commands.front());
  }

  void TearDown() override {
    if (device) device->raii().waitIdle();
    pass.reset();
    pipelines.reset();
    command.clear();
    pool.clear();
    memory.reset();
    device.reset();
    surface.clear();
    messenger.clear();
    instance.reset();
    if (window) glfwDestroyWindow(window);
    if (glfwStarted) glfwTerminate();
    EXPECT_EQ(validationErrors.load(), 0u);
  }

  std::pair<std::vector<Vertex>, std::vector<Vertex>> run(uint32_t frameIndex,
      std::span<const GltfDeformationPose> poses) {
    pass->uploadFrame(frameIndex, poses);
    const BufferSlice current = pass->currentVertices(frameIndex);
    const BufferSlice previous = pass->previousVertices(frameIndex);
    auto readback = memory->createBuffer(current.size * 2,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    pass->record(*command, frameIndex);
    const VkBufferCopy copyCurrent{current.offset, 0, current.size};
    const VkBufferCopy copyPrevious{previous.offset, current.size, previous.size};
    vkCmdCopyBuffer(*command, current.buffer, readback.buffer, 1, &copyCurrent);
    vkCmdCopyBuffer(*command, previous.buffer, readback.buffer, 1, &copyPrevious);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(*command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &host, 0, nullptr, 0, nullptr);
    command.end();
    const vk::CommandBuffer handle = *command;
    vk::SubmitInfo submit;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &handle;
    vk::raii::Queue queue(device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submit, nullptr);
    queue.waitIdle();
    EXPECT_EQ(vmaInvalidateAllocation(memory->allocator(), readback.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);
    std::pair<std::vector<Vertex>, std::vector<Vertex>> output{
        std::vector<Vertex>(pass->vertexCount()), std::vector<Vertex>(pass->vertexCount())};
    const auto* data = static_cast<const std::byte*>(readback.allocation_info.pMappedData);
    std::memcpy(output.first.data(), data, static_cast<size_t>(current.size));
    std::memcpy(output.second.data(), data + current.size, static_cast<size_t>(previous.size));
    memory->destroyBuffer(readback);
    return output;
  }

  std::filesystem::path shaderDir;
  bool glfwStarted{false};
  GLFWwindow* window{nullptr};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device;
  std::unique_ptr<VulkanMemoryManager> memory;
  std::unique_ptr<PipelineManager> pipelines;
  std::unique_ptr<GltfDeformationPass> pass;
  vk::raii::CommandPool pool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
  std::atomic<uint32_t> validationErrors{0};
};

TEST_F(GltfDeformationGpu, AllInfluencesAndMorphTargetsMatchCpuWithPreviousPose) {
  constexpr size_t count = 257;  // Crosses workgroup boundaries and has a tail.
  gltf::DeformationPrimitive primitive;
  primitive.meshDefaultWeights = {0.2f, -0.4f, 1.2f};
  primitive.morphTargets.resize(3);
  for (size_t i = 0; i < count; ++i) {
    primitive.bindVertices.push_back(bindVertex(i));
    gltf::VertexSkinInfluences influences;
    for (uint32_t joint = 0; joint < 12; ++joint) influences.values.push_back({joint, 1.0f / 12.0f});
    primitive.influences.push_back(std::move(influences));
    for (size_t target = 0; target < primitive.morphTargets.size(); ++target) {
      const float factor = static_cast<float>(target + 1) * 0.1f;
      primitive.morphTargets[target].positions.emplace_back(factor, 0, -factor);
      primitive.morphTargets[target].normals.emplace_back(0, factor, 0);
      primitive.morphTargets[target].tangents.emplace_back(factor, -factor, factor);
    }
  }
  std::vector<Vertex> initial(count + 2, bindVertex());
  const std::array<GltfDeformationInstance, 1> instances{{{&primitive, 1}}};
  pass->setScene(initial, instances);
  std::vector<glm::mat4> palette;
  std::vector<glm::mat4> previous;
  for (uint32_t joint = 0; joint < 12; ++joint) {
    palette.push_back(glm::translate(glm::mat4(1), glm::vec3(0, static_cast<float>(joint) * 0.1f, 0)) *
        glm::rotate(glm::mat4(1), glm::radians(static_cast<float>(joint) * 5), glm::vec3(0, 1, 0)) *
        glm::scale(glm::mat4(1), glm::vec3(-1.0f - joint * 0.1f, 2, 0.5f)));
    previous.push_back(glm::translate(glm::mat4(1), glm::vec3(0.1f, 0, 0)));
  }
  const std::array<float, 3> previousWeights{1, 0, 0};
  const std::array<GltfDeformationPose, 1> poses{{{{}, palette, previousWeights, previous, false}}};
  const auto [current, old] = run(0, poses);
  compareVertices(std::span<const Vertex>(current).subspan(1, count),
      gltf::EvaluatePrimitive(primitive, {}, palette));
  compareVertices(std::span<const Vertex>(old).subspan(1, count),
      gltf::EvaluatePrimitive(primitive, previousWeights, previous));
  EXPECT_EQ(std::memcmp(&current.front(), &initial.front(), sizeof(Vertex)), 0);
  EXPECT_EQ(std::memcmp(&current.back(), &initial.back(), sizeof(Vertex)), 0);
}

TEST_F(GltfDeformationGpu, SharedMeshInstancesUseDifferentMorphAndSkinPoses) {
  gltf::DeformationPrimitive primitive;
  primitive.bindVertices = {bindVertex()};
  primitive.influences = {{{{0, 1}}}};
  primitive.morphTargets = {{{glm::vec3(2, 0, 0)}, {}, {}}};
  primitive.meshDefaultWeights = {0};
  const std::array<Vertex, 2> initial{bindVertex(), bindVertex()};
  const std::array<GltfDeformationInstance, 2> instances{{{&primitive, 0}, {&primitive, 1}}};
  pass->setScene(initial, instances);
  const std::array<float, 1> weights{1};
  const std::array<glm::mat4, 1> first{glm::translate(glm::mat4(1), glm::vec3(0, 2, 0))};
  const std::array<glm::mat4, 1> second{glm::translate(glm::mat4(1), glm::vec3(0, -3, 0))};
  const std::array<GltfDeformationPose, 2> poses{{{{}, first}, {weights, second}}};
  const auto [current, previous] = run(1, poses);
  compareVertices(std::span<const Vertex>(current).subspan(0, 1), gltf::EvaluatePrimitive(primitive, {}, first));
  compareVertices(std::span<const Vertex>(current).subspan(1, 1), gltf::EvaluatePrimitive(primitive, weights, second));
  compareVertices(previous, current);
}

TEST_F(GltfDeformationGpu, ResetHistoryAndFrameReuseProduceCurrentPreviousEquality) {
  gltf::DeformationPrimitive primitive;
  primitive.bindVertices = {bindVertex()};
  primitive.influences = {{{{0, 1}}}};
  const std::array<GltfDeformationInstance, 1> instances{{{&primitive, 0}}};
  pass->setScene(primitive.bindVertices, instances);
  const std::array<glm::mat4, 1> currentPalette{glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4) * 1e-12f)};
  const std::array<glm::mat4, 1> previousPalette{glm::mat4(1)};
  std::array<GltfDeformationPose, 1> poses{{{{}, currentPalette, {}, previousPalette, false}}};
  auto output = run(0, poses);
  compareVertices(output.first, gltf::EvaluatePrimitive(primitive, {}, currentPalette));
  compareVertices(output.second, gltf::EvaluatePrimitive(primitive, {}, previousPalette));
  poses[0].resetHistory = true;
  output = run(0, poses);
  compareVertices(output.first, output.second);
  pass->createResources(shaderDir, 1);
  output = run(0, poses);
  compareVertices(output.first, output.second);
}

TEST_F(GltfDeformationGpu, MorphOnlyInstanceAndCollapsedSkinMatchFiniteCpuFrames) {
  gltf::DeformationPrimitive primitive;
  primitive.bindVertices = {bindVertex()};
  primitive.influences = {{{{0, 0.5f}, {1, 0.5f}}}};
  primitive.morphTargets = {{{glm::vec3(1, 0, 0)}, {glm::vec3(0, 1, 0)}, {}}};
  primitive.meshDefaultWeights = {0.5f};
  const std::array<Vertex, 2> initial{bindVertex(), bindVertex()};
  const std::array<GltfDeformationInstance, 2> instances{{{&primitive, 0}, {&primitive, 1}}};
  pass->setScene(initial, instances);
  const std::array<glm::mat4, 2> collapsed{glm::mat4(1), glm::scale(glm::mat4(1), glm::vec3(-1))};
  const std::array<GltfDeformationPose, 2> poses{{{}, {{}, collapsed}}};
  const auto [current, previous] = run(0, poses);
  compareVertices(std::span<const Vertex>(current).subspan(0, 1), gltf::EvaluatePrimitive(primitive));
  compareVertices(std::span<const Vertex>(current).subspan(1, 1), gltf::EvaluatePrimitive(primitive, {}, collapsed));
  compareVertices(previous, current);
}

TEST_F(GltfDeformationGpu, RejectsOverlappingRangesAndInvalidPoseUpload) {
  gltf::DeformationPrimitive primitive;
  primitive.bindVertices = {bindVertex()};
  primitive.influences = {{{{0, 1}}}};
  const std::array<GltfDeformationInstance, 2> overlapping{{{&primitive, 0}, {&primitive, 0}}};
  EXPECT_THROW(pass->setScene(primitive.bindVertices, overlapping), std::runtime_error);
  const std::array<GltfDeformationInstance, 1> instances{{{&primitive, 0}}};
  pass->setScene(primitive.bindVertices, instances);
  EXPECT_THROW(pass->uploadFrame(0, {}), std::runtime_error);
  glm::mat4 nonfinite(1);
  nonfinite[0][0] = std::numeric_limits<float>::quiet_NaN();
  const std::array<glm::mat4, 1> palette{nonfinite};
  const std::array<GltfDeformationPose, 1> poses{{{{}, palette}}};
  EXPECT_THROW(pass->uploadFrame(0, poses), std::runtime_error);
}

TEST_F(GltfDeformationGpu, EvaluatedFallbackCopiesEveryVertexByteWithHistoryAndReset) {
  gltf::DeformationPrimitive primitive;
  primitive.bindVertices = {bindVertex(), bindVertex(1), bindVertex(2)};
  primitive.morphTargets = {{{glm::vec3(100), glm::vec3(100), glm::vec3(100)}, {}, {}}};
  primitive.meshDefaultWeights = {1};
  primitive.authoredBaseNormal = false;
  primitive.authoredBaseTangent = false;
  const std::array<GltfDeformationInstance, 1> instances{{{&primitive, 1}}};
  std::vector<Vertex> initial(5, bindVertex());
  pass->setScene(initial, instances);
  const std::array<GltfDeformationPose, 1> absent{};
  EXPECT_THROW(pass->uploadFrame(0, absent), std::runtime_error);
  auto current = primitive.bindVertices;
  auto previous = current;
  for (auto& vertex : current) {
    vertex.position += glm::vec3(3, 4, 5);
    vertex.normal = glm::vec3(0, 1, 0);
    vertex.tangent = glm::vec4(0, 0, 1, -1);
    vertex.color = glm::vec3(.9f, .3f, .1f);
    vertex.colorAlpha = .375f;
    vertex.texCoord = glm::vec2(.6f, .2f);
    vertex.texCoord1 = glm::vec2(.4f, .8f);
  }
  std::array<GltfDeformationPose, 1> poses{};
  poses[0].evaluatedVertices = current;
  poses[0].previousEvaluatedVertices = previous;
  auto output = run(0, poses);
  EXPECT_EQ(std::memcmp(output.first.data() + 1, current.data(), current.size() * sizeof(Vertex)), 0);
  EXPECT_EQ(std::memcmp(output.second.data() + 1, previous.data(), previous.size() * sizeof(Vertex)), 0);
  EXPECT_EQ(std::memcmp(&output.first.front(), &initial.front(), sizeof(Vertex)), 0);
  EXPECT_EQ(std::memcmp(&output.first.back(), &initial.back(), sizeof(Vertex)), 0);
  poses[0].resetHistory = true;
  output = run(1, poses);
  EXPECT_EQ(std::memcmp(output.first.data(), output.second.data(), output.first.size() * sizeof(Vertex)), 0);
  poses[0].resetHistory = false;
  poses[0].previousEvaluatedVertices = {};
  output = run(0, poses);
  EXPECT_EQ(std::memcmp(output.first.data(), output.second.data(), output.first.size() * sizeof(Vertex)), 0);
  poses[0].evaluatedVertices = std::span<const Vertex>(current).first(2);
  EXPECT_THROW(pass->uploadFrame(0, poses), std::runtime_error);
  current[0].normal.x = std::numeric_limits<float>::quiet_NaN();
  poses[0].evaluatedVertices = current;
  EXPECT_THROW(pass->uploadFrame(0, poses), std::runtime_error);
}

}  // namespace
