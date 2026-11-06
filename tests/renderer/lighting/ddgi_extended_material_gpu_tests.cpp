#include "Container/common/CommonGLFW.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <numbers>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::gpu;
struct alignas(16) Query {
  GpuMaterial material{};
  glm::vec4 smoothNormal{0,0,1,0}, geometricNormal{0,0,1,0};
  glm::vec4 tangent{1,0,0,1}, finalNormal{0,0,1,0};
  glm::vec4 view{0,0,1,0}, light{0,0,1,0}, uv{.25f,.25f,.75f,.75f};
  glm::vec4 frontIrradiance{2,4,6,0}, backIrradiance{11,7,3,0};
  glm::vec4 albedo{.8f,.4f,.2f,0}, f0Metallic{.04f,.04f,.04f,0};
  glm::vec4 radianceRoughness{1,1,1,.5f}, occlusionLayerTransmission{1,1,0,0};
  glm::uvec4 controls{0};
};
static_assert(offsetof(Query,smoothNormal)==sizeof(GpuMaterial));
static_assert(sizeof(Query)==sizeof(GpuMaterial)+14*16);
struct alignas(16) Result {
  glm::vec4 direct{}, feedback{}, anisotropy{}, diffuse{}, visibilityNormal{}, resolvedUv{};
  glm::vec4 sampledLayerFactors{}, sampledSheenIridescence{}, deferredLayerFactors{}, deferredSheen{};
};
static_assert(sizeof(Result)==160);

class DdgiExtendedMaterialGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<DdgiExtendedMaterialGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto *enabled = std::getenv("CONTAINER_RUN_GPU_DDGI");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_DDGI=1 for DDGI transport contracts";
    std::ifstream shader(CONTAINER_DDGI_EXTENDED_MATERIAL_SPIRV,
                         std::ios::binary | std::ios::ate);
    ASSERT_TRUE(shader);
    const auto bytes = static_cast<size_t>(shader.tellg());
    ASSERT_GT(bytes, 20u);
    ASSERT_EQ(bytes % 4, 0u);
    std::vector<uint32_t> code(bytes / 4);
    shader.seekg(0);
    shader.read(reinterpret_cast<char *>(code.data()), bytes);
    ASSERT_TRUE(shader);
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "DDGI extended hit contract", nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t extensionCount{};
    const auto *extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
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
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(callback);
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);
    VkSurfaceKHR surfaceHandle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(), window, nullptr, &surfaceHandle),
              VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), surfaceHandle);
    DeviceCreateInfo create;
    create.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.pNext = &v12;
    create.next = &v13;
    device = std::make_unique<VulkanDevice>(*instance, surfaceHandle, create);
    memory = std::make_unique<VulkanMemoryManager>(instance->instance(),
        device->physicalDevice(), device->device());
    commandPool = vk::raii::CommandPool(device->raii(),
        vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            device->queueFamilyIndices().graphicsFamily.value()));
    auto commands = device->raii().allocateCommandBuffers(
        vk::CommandBufferAllocateInfo(*commandPool, vk::CommandBufferLevel::ePrimary, 1));
    command = std::move(commands.front());
    const std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eSampler,1,vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eSampledImage,1,vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eSampledImage,1,vk::ShaderStageFlagBits::eCompute)};
    layout = vk::raii::DescriptorSetLayout(device->raii(),
        vk::DescriptorSetLayoutCreateInfo({}, bindings));
    pipelineLayout = vk::raii::PipelineLayout(device->raii(),
        vk::PipelineLayoutCreateInfo({}, *layout));
    vk::raii::ShaderModule module(device->raii(), vk::ShaderModuleCreateInfo({}, code));
    pipeline = vk::raii::Pipeline(device->raii(), nullptr,
        vk::ComputePipelineCreateInfo({},
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute,
                                               *module, "computeMain"), *pipelineLayout));
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.magFilter=samplerInfo.minFilter=vk::Filter::eNearest;
    samplerInfo.addressModeU=samplerInfo.addressModeV=samplerInfo.addressModeW=vk::SamplerAddressMode::eClampToEdge;
    sampler=vk::raii::Sampler(device->raii(),samplerInfo);
    createTextures();
  }
  void TearDown() override {
    if (device) device->raii().waitIdle();
    command.clear();
    pipeline.clear(); pipelineLayout.clear(); layout.clear(); commandPool.clear();
    sampler.clear();
    for (auto& view : textureViews) view.clear();
    for (size_t i=0;i<textureImages.size();++i)
      if (textureImages[i]) vmaDestroyImage(memory->allocator(),textureImages[i],textureMemory[i]);
    memory.reset(); device.reset(); surface.clear(); messenger.clear(); instance.reset();
    if (window) glfwDestroyWindow(window);
    if (glfwStarted) glfwTerminate();
    EXPECT_EQ(validationErrors.load(), 0u);
  }
  struct Buffer {
    VulkanMemoryManager &memory;
    AllocatedBuffer buffer;
    Buffer(VulkanMemoryManager &m, size_t bytes) : memory(m),
      buffer(m.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT)) {}
    ~Buffer() { memory.destroyBuffer(buffer); }
  };
  std::vector<Result> run(std::span<const Query> input) {
    if (input.empty()) throw std::invalid_argument("DDGI transport empty queries");
    Buffer queries(*memory, input.size_bytes()), output(*memory, input.size() * sizeof(Result));
    std::memcpy(queries.buffer.allocation_info.pMappedData, input.data(), input.size_bytes());
    if (vmaFlushAllocation(memory->allocator(), queries.buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI transport flush failed");
    const std::array sizes{vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer,2),
      vk::DescriptorPoolSize(vk::DescriptorType::eSampler,1),vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage,2)};
    vk::raii::DescriptorPool descriptors(device->raii(), vk::DescriptorPoolCreateInfo(
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
    auto sets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*descriptors, *layout));
    const std::array buffers{
      vk::DescriptorBufferInfo(queries.buffer.buffer, 0, input.size_bytes()),
      vk::DescriptorBufferInfo(output.buffer.buffer, 0, input.size() * sizeof(Result))};
    for (uint32_t i = 0; i < 2; ++i)
      device->raii().updateDescriptorSets(vk::WriteDescriptorSet(*sets.front(), i, 0, 1,
          vk::DescriptorType::eStorageBuffer, nullptr, &buffers[i]), {});
    const std::array images{vk::DescriptorImageInfo(*sampler,{},vk::ImageLayout::eUndefined),
      vk::DescriptorImageInfo({},*textureViews[0],vk::ImageLayout::eShaderReadOnlyOptimal),
      vk::DescriptorImageInfo({},*textureViews[1],vk::ImageLayout::eShaderReadOnlyOptimal)};
    const std::array writes{vk::WriteDescriptorSet(*sets.front(),2,0,1,vk::DescriptorType::eSampler,&images[0]),
      vk::WriteDescriptorSet(*sets.front(),3,0,1,vk::DescriptorType::eSampledImage,&images[1]),
      vk::WriteDescriptorSet(*sets.front(),4,0,1,vk::DescriptorType::eSampledImage,&images[2])};
    device->raii().updateDescriptorSets(writes,{});
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::MemoryBarrier2 before(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead |
                                                     vk::AccessFlagBits2::eShaderWrite);
    vk::DependencyInfo dependency;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &before;
    command.pipelineBarrier2(dependency);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 0, *sets.front(), {});
    command.dispatch(static_cast<uint32_t>(input.size()), 1, 1);
    vk::MemoryBarrier2 after(vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderWrite, vk::PipelineStageFlagBits2::eHost,
        vk::AccessFlagBits2::eHostRead);
    dependency.pMemoryBarriers = &after;
    command.pipelineBarrier2(dependency);
    command.end();
    const vk::CommandBuffer handle = *command;
    vk::SubmitInfo submit;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &handle;
    vk::raii::Fence fence(device->raii(), vk::FenceCreateInfo());
    vk::raii::Queue queue(device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submit, *fence);
    if (device->raii().waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
      throw std::runtime_error("DDGI transport fence failed");
    if (vmaInvalidateAllocation(memory->allocator(), output.buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI transport invalidate failed");
    std::vector<Result> result(input.size());
    std::memcpy(result.data(), output.buffer.allocation_info.pMappedData, result.size() * sizeof(Result));
    // Drop command references before descriptor/buffer destruction.
    command.reset();
    return result;
  }
  void createTextures() {
    constexpr std::array<unsigned char,16> texels{255,128,64,32, 128,255,192,128,
                                               0,128,255,192, 128,0,128,255};
    StagingBuffer staging(*memory,texels.size());staging.upload(std::as_bytes(std::span(texels)));
    for (size_t i=0;i<textureImages.size();++i) {
      VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      create.imageType=VK_IMAGE_TYPE_2D;create.format=i==0?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_R8G8B8A8_SRGB;
      create.extent={2,2,1};create.mipLevels=create.arrayLayers=1;create.samples=VK_SAMPLE_COUNT_1_BIT;
      create.tiling=VK_IMAGE_TILING_OPTIMAL;create.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      ASSERT_EQ(vmaCreateImage(memory->allocator(),&create,&allocation,&textureImages[i],&textureMemory[i],nullptr),VK_SUCCESS);
      textureViews[i]=vk::raii::ImageView(device->raii(),vk::ImageViewCreateInfo({},textureImages[i],
        vk::ImageViewType::e2D,static_cast<vk::Format>(create.format),{},
        vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)));
      command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.image=textureImages[i];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(*command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
      VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={2,2,1};
      vkCmdCopyBufferToImage(*command,staging.buffer().buffer,textureImages[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
      barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(*command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
      command.end();const vk::CommandBuffer raw=*command;vk::SubmitInfo submit;submit.commandBufferCount=1;submit.pCommandBuffers=&raw;
      vk::raii::Queue queue(device->raii(),device->queueFamilyIndices().graphicsFamily.value(),0);queue.submit(submit,nullptr);queue.waitIdle();
      command.reset();
    }
  }
  std::array<VkImage,2> textureImages{};
  std::array<VmaAllocation,2> textureMemory{};
  std::array<vk::raii::ImageView,2> textureViews{vk::raii::ImageView(nullptr),vk::raii::ImageView(nullptr)};
  vk::raii::Sampler sampler{nullptr};
  GLFWwindow *window{};
  bool glfwStarted{};
  std::atomic_uint validationErrors{0};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};
  std::unique_ptr<VulkanDevice> device;
  std::unique_ptr<VulkanMemoryManager> memory;
  vk::raii::CommandPool commandPool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
  vk::raii::DescriptorSetLayout layout{nullptr};
  vk::raii::PipelineLayout pipelineLayout{nullptr};
  vk::raii::Pipeline pipeline{nullptr};
};


constexpr double pi=std::numbers::pi_v<double>;
glm::dvec3 unit(glm::vec4 value) { return glm::normalize(glm::dvec3(value)); }
glm::dvec3 analyticTangent(const Query& query) {
  const auto normal=unit(query.smoothNormal), finalNormal=unit(query.finalNormal);
  const auto raw=glm::dvec3(query.tangent);
  const auto tangent=glm::normalize(raw-normal*glm::dot(normal,raw));
  const auto bitangent=glm::cross(normal,tangent)*(query.tangent.w<0?-1.0:1.0);
  const double angle=query.material.anisotropyDiffuseFactors.y;
  const auto rotated=tangent*std::cos(angle)+bitangent*std::sin(angle);
  return glm::normalize(rotated-finalNormal*glm::dot(finalNormal,rotated));
}

// Independent binary64 evaluation of the documented renderer approximation:
// slope-density elliptical GGX with its legacy denominator floor and Schlick
// masking, rather than the physical correlated-Smith oracle's visibility term.
glm::dvec3 analyticDirect(const Query& query) {
  const auto normal=unit(query.finalNormal), geometric=unit(query.geometricNormal);
  const auto view=unit(query.view), light=unit(query.light), tangent=analyticTangent(query);
  const double nv=glm::dot(normal,view), nl=glm::dot(normal,light), gl=glm::dot(geometric,light);
  if (nv<=0 || nl==0 || nl*gl<=0) return glm::dvec3(0);
  const double strength=query.material.anisotropyDiffuseFactors.x;
  const double diffuse=query.material.anisotropyDiffuseFactors.z;
  const double residual=1-query.occlusionLayerTransmission.z;
  const auto f0=glm::dvec3(query.f0Metallic), albedo=glm::dvec3(query.albedo);
  const double metallic=query.f0Metallic.w, roughness=query.radianceRoughness.w;
  if (nl<0) {
    if (diffuse==0) return glm::dvec3(0);
    const auto mirrored=light-normal*(2*nl), half=glm::normalize(view+mirrored);
    const auto fresnel=f0+(glm::dvec3(1)-f0)*std::pow(1-std::clamp(glm::dot(half,view),0.0,1.0),5);
    return (glm::dvec3(1)-fresnel)*glm::dvec3(query.material.diffuseTransmissionColorFactor)*
        ((1-metallic)*residual*diffuse*(-nl)/pi)*glm::dvec3(query.radianceRoughness);
  }
  const auto half=glm::normalize(view+light), bitangent=glm::cross(normal,tangent);
  const double alpha=roughness*roughness, at=alpha+(1-alpha)*strength*strength, ab=alpha;
  const double ht=glm::dot(half,tangent), hb=glm::dot(half,bitangent), hn=glm::dot(half,normal);
  const double q=ht*ht/(at*at)+hb*hb/(ab*ab)+hn*hn;
  const double distribution=at*ab/std::max(pi*std::pow(at*ab*q,2),1e-4);
  const double k=(roughness+1)*(roughness+1)/8;
  const double geometry=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));
  const auto fresnel=f0+(glm::dvec3(1)-f0)*std::pow(1-std::clamp(glm::dot(half,view),0.0,1.0),5);
  const auto specular=fresnel*(distribution*geometry/std::max(4*nv*nl,1e-4));
  const auto reflected=(glm::dvec3(1)-fresnel)*albedo*((1-metallic)*residual*(1-diffuse)/pi);
  return (reflected+specular)*glm::dvec3(query.radianceRoughness)*nl;
}

glm::dvec3 analyticFeedback(const Query& query) {
  const double cosine=std::max(glm::dot(unit(query.finalNormal),unit(query.view)),0.0);
  const auto f0=glm::dvec3(query.f0Metallic);
  const auto fresnel=f0+(glm::max(glm::dvec3(1-query.radianceRoughness.w),f0)-f0)*std::pow(1-cosine,5);
  const double diffuse=query.material.anisotropyDiffuseFactors.z, residual=1-query.occlusionLayerTransmission.z;
  const auto front=glm::dvec3(query.frontIrradiance)*glm::dvec3(query.albedo)*
      (residual*(1-diffuse)*query.occlusionLayerTransmission.x);
  const auto back=glm::dvec3(query.backIrradiance)*glm::dvec3(query.material.diffuseTransmissionColorFactor)*(residual*diffuse);
  return (front+back)*(glm::dvec3(1)-fresnel)*((1-query.f0Metallic.w)*query.occlusionLayerTransmission.y/pi);
}

TEST_F(DdgiExtendedMaterialGpu, DirectFrontAndBackMatchIndependentDoublePrecisionApproximation) {
  std::vector<Query> input;
  for (float strength : {0.f,1e-6f,.3f,.8f})
    for (float diffuse : {0.f,.6f,1.f})
      for (float metallic : {0.f,1.f})
        for (float sign : {-1.f,1.f}) {
          Query query;
          query.material.anisotropyDiffuseFactors={strength,.43f,diffuse,0};
          query.material.diffuseTransmissionColorFactor={.15f,.7f,.9f,0};
          query.light={.3f,.4f,sign*std::sqrt(.75f),0};
          query.f0Metallic={.07f,.04f,.02f,metallic};
          query.occlusionLayerTransmission.z=.25f;
          input.push_back(query);
        }
  const auto actual=run(input);
  for (size_t i=0;i<input.size();++i) {
    SCOPED_TRACE(i);
    const auto expected=analyticDirect(input[i]);
    for (int c=0;c<3;++c) EXPECT_NEAR(actual[i].direct[c],expected[c],3e-5);
  }
}

TEST_F(DdgiExtendedMaterialGpu, ZeroAnisotropyKeepsSpecularAndHasNoEnableBoundaryJump) {
  std::array<Query,3> input;
  for (auto& query : input) query.albedo=glm::vec4(0);
  input[1].material.anisotropyDiffuseFactors.x=1e-6f;
  input[2].material.anisotropyDiffuseFactors.z=1;
  input[2].material.diffuseTransmissionColorFactor=glm::vec4(0);
  const auto actual=run(input);
  EXPECT_GT(actual[0].direct.x,.01f); // The previous diffuse-only hit baseline was zero.
  for (int c=0;c<3;++c) {
    EXPECT_NEAR(actual[0].direct[c],analyticDirect(input[0])[c],2e-6);
    EXPECT_NEAR(actual[0].direct[c],actual[1].direct[c],2e-6);
    EXPECT_NEAR(actual[0].direct[c],actual[2].direct[c],2e-6); // Black BTDF never tints specular.
  }
}

TEST_F(DdgiExtendedMaterialGpu, SmoothFrameProjectionHandednessAndBackOrientationAreIndependentOfFaceNormal) {
  std::array<Query,4> input;
  for (auto& query : input) {
    query.smoothNormal={0,.6f,.8f,0};
    query.material.anisotropyDiffuseFactors={.7f,.75f,0,0};
  }
  input[1].tangent.w=-1;
  input[2].smoothNormal=-input[2].smoothNormal; input[2].finalNormal={0,0,-1,0};
  input[3].material.anisotropyDiffuseFactors.y=0; // Absent map is +T, not diagonal white RG.
  const auto actual=run(input);
  for (size_t i=0;i<input.size();++i) {
    const auto expected=analyticTangent(input[i]);
    for (int c=0;c<3;++c) EXPECT_NEAR(actual[i].anisotropy[c],expected[c],2e-6);
    EXPECT_NEAR(glm::dot(glm::vec3(actual[i].anisotropy),glm::vec3(input[i].finalNormal)),0,2e-6);
  }
  EXPECT_GT(actual[0].anisotropy.y,.5f); EXPECT_LT(actual[1].anisotropy.y,-.5f);
  EXPECT_EQ(glm::vec3(actual[3].anisotropy),glm::vec3(1,0,0));
}

TEST_F(DdgiExtendedMaterialGpu, BackLightUsesLightFacingBiasNormalAndRejectsGeometricMismatch) {
  std::array<Query,4> input;
  for (auto& query : input) query.material.anisotropyDiffuseFactors.z=1;
  input[0].light={0,0,-1,0};
  input[1].light={0,0,1,0};
  input[2].geometricNormal={0,0,-1,0}; // Shading front / geometric back.
  input[3].finalNormal={0,.8f,.6f,0};input[3].light=glm::vec4(glm::normalize(glm::vec3(0,1,-.2f)),0);
  const auto actual=run(input);
  EXPECT_EQ(glm::vec3(actual[0].visibilityNormal),glm::vec3(0,0,-1));
  EXPECT_GT(actual[0].direct.x,0);
  EXPECT_EQ(glm::vec3(actual[1].visibilityNormal),glm::vec3(0,0,1));
  for (size_t i : {2u,3u}) { EXPECT_EQ(actual[i].visibilityNormal,glm::vec4(0)); EXPECT_EQ(actual[i].direct,glm::vec4(0)); }
}

TEST_F(DdgiExtendedMaterialGpu, DifferentlyColoredHemisphereFeedbackUsesIndependentColorAndResidualWeights) {
  std::array<Query,5> input;
  for (size_t i=0;i<input.size();++i) {
    input[i].material.anisotropyDiffuseFactors.z=float(i%3)*.5f;
    input[i].material.diffuseTransmissionColorFactor={.1f,.6f,.9f,0};
    input[i].occlusionLayerTransmission={.7f,.8f,.25f,0};
  }
  input[3].material.anisotropyDiffuseFactors.z=1; input[3].material.diffuseTransmissionColorFactor=glm::vec4(0);
  input[4]=input[2];input[4].albedo={0,1,0,0}; // d=1 back feedback cannot inherit albedo.
  const auto actual=run(input);
  for (size_t i=0;i<input.size();++i) {
    const auto expected=analyticFeedback(input[i]);
    for (int c=0;c<3;++c) EXPECT_NEAR(actual[i].feedback[c],expected[c],2e-6);
  }
  EXPECT_EQ(actual[3].feedback,glm::vec4(0));
  EXPECT_EQ(actual[2].feedback,actual[4].feedback);
}

TEST_F(DdgiExtendedMaterialGpu, ActualUv1TransformsKeepLinearStrengthAlphaAndSrgbColorRoles) {
  std::array<Query,3> input;
  auto& material=input[0].material;
  input[0].controls.x=1;
  material.anisotropyTextureIndex=material.diffuseTransmissionTextureIndex=0;
  material.diffuseTransmissionColorTextureIndex=1;
  material.anisotropyDiffuseFactors={.8f,0,.6f,0};
  material.diffuseTransmissionColorFactor={.4f,.6f,.8f,0};
  material.anisotropyTextureTransform.row0={1,0,-.5f,1};
  material.anisotropyTextureTransform.row1={0,1,0,4};
  material.diffuseTransmissionTextureTransform.row0={1,0,.5f,0};
  material.diffuseTransmissionTextureTransform.row1={0,1,0,3};
  material.diffuseTransmissionColorTextureTransform.row0={1,0,0,1};
  material.diffuseTransmissionColorTextureTransform.row1={0,1,-.5f,4};
  input[1]=input[0];input[1].material.anisotropyDiffuseFactors.x=0;
  input[1].material.anisotropyDiffuseFactors.z=0;
  input[1].material.diffuseTransmissionColorFactor=glm::vec4(0);
  input[2]=input[0]; // Independent wrong-UV controls choose different texels in all three roles.
  input[2].material.anisotropyTextureTransform.row0.w=0;
  input[2].material.diffuseTransmissionTextureTransform.row0.w=1;
  input[2].material.diffuseTransmissionColorTextureTransform.row0.w=0;
  const auto actual=run(input);
  EXPECT_NEAR(actual[0].anisotropy.w,.8,2e-6);
  EXPECT_NEAR(actual[0].diffuse.w,.6*128/255,2e-6);
  const auto linear=[](double byte) { const double value=byte/255;return value<=.04045?value/12.92:std::pow((value+.055)/1.055,2.4); };
  EXPECT_NEAR(actual[0].diffuse.x,.4*linear(128),4e-4);
  EXPECT_NEAR(actual[0].diffuse.y,.6,4e-4);
  EXPECT_NEAR(actual[0].diffuse.z,.8*linear(192),4e-4);
  const auto direction=glm::normalize(glm::dvec3(-1,1.0/255,0));
  for (int c=0;c<3;++c) EXPECT_NEAR(actual[0].anisotropy[c],direction[c],2e-6);
  EXPECT_EQ(actual[0].resolvedUv,glm::vec4(.25f,.75f,.75f,.25f));
  EXPECT_FLOAT_EQ(actual[1].anisotropy.w,0);EXPECT_EQ(actual[1].diffuse,glm::vec4(0));
  EXPECT_NEAR(actual[2].anisotropy.w,.8*64/255,2e-6);
  EXPECT_NEAR(actual[2].diffuse.w,.6,2e-6);
  EXPECT_NEAR(actual[2].diffuse.x,.4,4e-4);
}
TEST_F(DdgiExtendedMaterialGpu, TexturesNeverPromoteZeroFactorsOrBlackSheen) {
  std::array<Query,3> input;
  for (size_t i=0; i<input.size(); ++i) {
    auto& m=input[i].material;
    m.transmissionFactor=m.clearcoatFactor=m.clearcoatRoughnessFactor=0;
    m.sheenRoughnessFactor=m.iridescenceFactor=0;
    m.sheenColorFactor=glm::vec4(0);
    if (i) {
      m.transmissionTextureIndex=m.clearcoatTextureIndex=m.clearcoatRoughnessTextureIndex=0;
      m.sheenRoughnessTextureIndex=m.iridescenceTextureIndex=0;
      m.sheenColorTextureIndex=uint32_t(i-1); // Linear and sRGB image roles.
    }
  }
  const auto actual=run(input);
  for (size_t i=0; i<input.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(actual[i].sampledLayerFactors,glm::vec4(0));
    EXPECT_EQ(actual[i].sampledSheenIridescence,glm::vec4(0));
    EXPECT_EQ(actual[i].deferredLayerFactors,glm::vec4(0,0,.045f,.045f));
    EXPECT_EQ(actual[i].deferredSheen,glm::vec4(0));
  }
}

TEST_F(DdgiExtendedMaterialGpu, PositiveFactorsMultiplyActualTextureChannelsAndKeepDeferredValues) {
  std::array<Query,2> input;
  for (auto& q:input) {
    auto& m=q.material;
    m.transmissionFactor=.0005f; m.clearcoatFactor=.25f;
    m.clearcoatRoughnessFactor=.75f; m.sheenRoughnessFactor=.5f;
    m.iridescenceFactor=.6f; m.sheenColorFactor={.2f,.4f,.8f,0};
    m.transmissionTextureIndex=m.clearcoatTextureIndex=m.clearcoatRoughnessTextureIndex=0;
    m.sheenRoughnessTextureIndex=m.iridescenceTextureIndex=m.sheenColorTextureIndex=0;
  }
  // UV0=.25,.25 samples literal UNORM RGBA=(255,128,64,32); UV1=.75,.75
  // samples=(128,0,128,255). Every channel witness differs.
  auto& second=input[1].material;
  for (auto* transform:{&second.transmissionTextureTransform,&second.clearcoatTextureTransform,
      &second.clearcoatRoughnessTextureTransform,&second.sheenRoughnessTextureTransform,
      &second.sheenColorTextureTransform,&second.iridescenceTextureTransform}) transform->row0.w=1;
  const auto actual=run(input);
  const std::array<glm::vec4,2> texels{{{1,128.f/255,64.f/255,32.f/255},{128.f/255,0,128.f/255,1}}};
  for (size_t i=0;i<input.size();++i) {
    SCOPED_TRACE(i);
    const auto& t=texels[i];
    const glm::vec4 factors{.0005f*t.r,.25f*t.r,.75f*t.g,.5f*t.a};
    const glm::vec4 sheen{.2f*t.r,.4f*t.g,.8f*t.b,.6f*t.r};
    for (int c=0;c<4;++c) {
      EXPECT_NEAR(actual[i].sampledLayerFactors[c],factors[c],2e-6);
      EXPECT_NEAR(actual[i].sampledSheenIridescence[c],sheen[c],2e-6);
    }
    EXPECT_EQ(actual[i].deferredLayerFactors,glm::vec4(.0005f,.25f,.75f,.5f));
    EXPECT_EQ(actual[i].deferredSheen,glm::vec4(.2f,.4f,.8f,.6f));
  }
}

} // namespace
