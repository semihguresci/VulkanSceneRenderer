#include "Container/common/CommonGLFW.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/Camera.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
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
  glm::vec4 diffuseRoughness{.7f,.4f,.2f,.5f};
  glm::vec4 f0Scale{.04f,.04f,.04f,1};
  glm::vec4 view{0,0,1,0},light{0,0,1,0};
  glm::uvec4 controls{0,1234,0,0};
  glm::vec4 previousMean{},previousMoment{},sampleValue{};
  glm::mat4 inverseProjection{1},inverseView{1};
  glm::vec4 footprintA{},footprintB{},footprintC{};
};
static_assert(sizeof(Query)==304);
struct alignas(16) Result {
  glm::vec4 valuePdf{},directionPdf{},weightValid{},auxiliary{};
};
static_assert(sizeof(Result)==64);

class PathTransportGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<PathTransportGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto *enabled = std::getenv("CONTAINER_RUN_GPU_PATH_TRANSPORT");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_PATH_TRANSPORT=1 for Reference path transport contracts";
    std::ifstream shader(CONTAINER_PATH_TRANSPORT_SPIRV,
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
    window = glfwCreateWindow(32, 32, "Reference path transport contract", nullptr, nullptr);
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
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eSampledImage, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eSampler, 1,
                                       vk::ShaderStageFlagBits::eCompute)};
    layout = vk::raii::DescriptorSetLayout(device->raii(),
        vk::DescriptorSetLayoutCreateInfo({}, bindings));
    pipelineLayout = vk::raii::PipelineLayout(device->raii(),
        vk::PipelineLayoutCreateInfo({}, *layout));
    vk::raii::ShaderModule module(device->raii(), vk::ShaderModuleCreateInfo({}, code));
    pipeline = vk::raii::Pipeline(device->raii(), nullptr,
        vk::ComputePipelineCreateInfo({},
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute,
                                               *module, "computeMain"), *pipelineLayout));
    createContractTexture();
  }

  // A rectangular RGBA8 image with nine authored constant-colour mips.
  // Distinct dimensions let transformed gradients expose U/V swaps in LOD.
  static constexpr uint32_t kContractTextureWidth = 256u;
  static constexpr uint32_t kContractTextureHeight = 16u;
  static constexpr uint32_t kContractTextureMips = 9u;
  static glm::u8vec4 contractMipColour(uint32_t mip) {
    return {glm::u8(mip * 28u), glm::u8(255u - mip * 28u), 77u, 255u};
  }
  void createContractTexture() {
    std::vector<std::byte> pixels;
    std::vector<vk::BufferImageCopy> regions;
    for (uint32_t mip = 0; mip < kContractTextureMips; ++mip) {
      const uint32_t width = kContractTextureWidth >> mip;
      const uint32_t height = std::max(kContractTextureHeight >> mip, 1u);
      const size_t texelCount = size_t(width) * height;
      const auto colour = contractMipColour(mip);
      vk::BufferImageCopy region{};
      region.bufferOffset = pixels.size();
      region.imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1};
      region.imageExtent = vk::Extent3D(width, height, 1);
      regions.push_back(region);
      pixels.resize(region.bufferOffset + texelCount * 4);
      auto* target = reinterpret_cast<glm::u8vec4*>(pixels.data() + region.bufferOffset);
      std::fill_n(target, texelCount, colour);
    }
    auto staging = memory->createBuffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT);
    struct StagingGuard {
      VulkanMemoryManager& manager;
      AllocatedBuffer buffer;
      ~StagingGuard() { manager.destroyBuffer(buffer); }
    } stagingGuard{*memory, staging};
    std::memcpy(staging.allocation_info.pMappedData, pixels.data(), pixels.size());
    if (vmaFlushAllocation(memory->allocator(), staging.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Reference path transport texture flush failed");
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {kContractTextureWidth, kContractTextureHeight, 1};
    imageInfo.mipLevels = kContractTextureMips;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(memory->allocator(), &imageInfo, &allocationInfo,
                       &contractImage, &contractImageMemory, nullptr) != VK_SUCCESS)
      throw std::runtime_error("Reference path transport texture creation failed");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = contractImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kContractTextureMips, 0, 1};
    if (vkCreateImageView(device->device(), &viewInfo, nullptr, &contractImageView) != VK_SUCCESS)
      throw std::runtime_error("Reference path transport texture view failed");
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = float(kContractTextureMips - 1u);
    if (vkCreateSampler(device->device(), &samplerInfo, nullptr, &contractSampler) != VK_SUCCESS)
      throw std::runtime_error("Reference path transport sampler failed");
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::ImageMemoryBarrier2 toDestination;
    toDestination.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    toDestination.srcAccessMask = {};
    toDestination.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    toDestination.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    toDestination.oldLayout = vk::ImageLayout::eUndefined;
    toDestination.newLayout = vk::ImageLayout::eTransferDstOptimal;
    toDestination.image = contractImage;
    toDestination.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, kContractTextureMips, 0, 1};
    vk::DependencyInfo toDestinationDependency;
    toDestinationDependency.imageMemoryBarrierCount = 1;
    toDestinationDependency.pImageMemoryBarriers = &toDestination;
    command.pipelineBarrier2(toDestinationDependency);
    command.copyBufferToImage(staging.buffer, contractImage,
                              vk::ImageLayout::eTransferDstOptimal, regions);
    vk::ImageMemoryBarrier2 toSampled;
    toSampled.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    toSampled.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    toSampled.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    toSampled.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    toSampled.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    toSampled.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    toSampled.image = contractImage;
    toSampled.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, kContractTextureMips, 0, 1};
    vk::DependencyInfo toSampledDependency;
    toSampledDependency.imageMemoryBarrierCount = 1;
    toSampledDependency.pImageMemoryBarriers = &toSampled;
    command.pipelineBarrier2(toSampledDependency);
    command.end();
    const vk::CommandBuffer handle = *command;
    vk::SubmitInfo submit;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &handle;
    vk::raii::Fence fence(device->raii(), vk::FenceCreateInfo());
    vk::raii::Queue queue(device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submit, *fence);
    if (device->raii().waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
      throw std::runtime_error("Reference path transport texture fence failed");
    command.reset();
  }
  void TearDown() override {
    if (device) device->raii().waitIdle();
    if (device) {
      if (contractSampler) vkDestroySampler(device->device(), contractSampler, nullptr);
      if (contractImageView) vkDestroyImageView(device->device(), contractImageView, nullptr);
      if (contractImage) vmaDestroyImage(memory->allocator(), contractImage, contractImageMemory);
      contractSampler = VK_NULL_HANDLE;
      contractImageView = VK_NULL_HANDLE;
      contractImage = VK_NULL_HANDLE;
      contractImageMemory = nullptr;
    }
    command.clear();
    pipeline.clear(); pipelineLayout.clear(); layout.clear(); commandPool.clear();
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
    if (input.empty()) throw std::invalid_argument("Reference path transport empty queries");
    Buffer queries(*memory, input.size_bytes()), output(*memory, input.size() * sizeof(Result));
    std::memcpy(queries.buffer.allocation_info.pMappedData, input.data(), input.size_bytes());
    if (vmaFlushAllocation(memory->allocator(), queries.buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Reference path transport flush failed");
    const std::array poolSizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 2),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 1),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampler, 1)};
    vk::raii::DescriptorPool descriptors(device->raii(), vk::DescriptorPoolCreateInfo(
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, poolSizes));
    auto sets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*descriptors, *layout));
    const std::array buffers{
      vk::DescriptorBufferInfo(queries.buffer.buffer, 0, input.size_bytes()),
      vk::DescriptorBufferInfo(output.buffer.buffer, 0, input.size() * sizeof(Result))};
    for (uint32_t i = 0; i < 2; ++i)
      device->raii().updateDescriptorSets(vk::WriteDescriptorSet(*sets.front(), i, 0, 1,
          vk::DescriptorType::eStorageBuffer, nullptr, &buffers[i]), {});
    const vk::DescriptorImageInfo sampledImage({}, contractImageView,
                                               vk::ImageLayout::eShaderReadOnlyOptimal);
    device->raii().updateDescriptorSets(vk::WriteDescriptorSet(*sets.front(), 2, 0, 1,
        vk::DescriptorType::eSampledImage, &sampledImage, {}), {});
    const vk::DescriptorImageInfo samplerImage(contractSampler, {}, vk::ImageLayout::eUndefined);
    device->raii().updateDescriptorSets(vk::WriteDescriptorSet(*sets.front(), 3, 0, 1,
        vk::DescriptorType::eSampler, &samplerImage, {}), {});
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
    command.dispatch(static_cast<uint32_t>((input.size()+63)/64), 1, 1);
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
      throw std::runtime_error("Reference path transport fence failed");
    if (vmaInvalidateAllocation(memory->allocator(), output.buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Reference path transport invalidate failed");
    std::vector<Result> result(input.size());
    std::memcpy(result.data(), output.buffer.allocation_info.pMappedData, result.size() * sizeof(Result));
    // Drop command references before descriptor/buffer destruction.
    command.reset();
    return result;
  }
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
  VkImage contractImage{VK_NULL_HANDLE};
  VmaAllocation contractImageMemory{nullptr};
  VkImageView contractImageView{VK_NULL_HANDLE};
  VkSampler contractSampler{VK_NULL_HANDLE};
};


constexpr double pi=3.14159265358979323846;
std::vector<Query> sampling(Query base,size_t count,uint32_t operation=1) {
  std::vector<Query> values(count,base);
  for(size_t i=0;i<count;++i) values[i].controls={operation,1234,17,uint32_t(i)};
  return values;
}
glm::dvec3 meanWeights(std::span<const Result> values) {
  glm::dvec3 sum{};
  for(const auto& value:values) sum+=glm::dvec3(value.weightValid);
  return sum/double(values.size());
}
// Independent double-precision BRDF reference. Use the original NDF and
// Smith-lambda equations, rather than the shader's rearranged visibility form.
glm::dvec3 reference(const Query& q,glm::dvec3 v,glm::dvec3 l) {
  if(v.z<=0 || l.z<=0) return {};
  const auto h=glm::normalize(v+l);
  const auto f0=glm::dvec3(q.f0Scale);
  const auto F=(f0+(glm::dvec3(1)-f0)*std::pow(1-glm::dot(v,h),5))*double(q.f0Scale.w);
  auto result=(glm::dvec3(1)-F)*glm::dvec3(q.diffuseRoughness)/pi;
  if(q.diffuseRoughness.w>0 && q.f0Scale.w>0) {
    const double alpha=double(q.diffuseRoughness.w)*q.diffuseRoughness.w;
    const double denominator=(h.x*h.x+h.y*h.y)+alpha*alpha*h.z*h.z;
    const double D=alpha*alpha/(pi*denominator*denominator);
    const auto lambda=[&](glm::dvec3 w){return .5*(std::sqrt(1+alpha*alpha*(w.x*w.x+w.y*w.y)/(w.z*w.z))-1);};
    result+=F*(D/(4*v.z*l.z*(1+lambda(v)+lambda(l))));
  }
  return result;
}
double probability(const Query& q,glm::dvec3 view) {
  if(q.f0Scale.w==0) return 0;
  if(glm::dvec3(q.diffuseRoughness)==glm::dvec3(0)) return 1;
  const auto F=(glm::dvec3(q.f0Scale)+(glm::dvec3(1)-glm::dvec3(q.f0Scale))*std::pow(1-view.z,5))*double(q.f0Scale.w);
  const auto luminance=[](glm::dvec3 v){return glm::dot(v,glm::dvec3(.2126,.7152,.0722));};
  const auto spec=luminance(F),diffuse=luminance(glm::dvec3(q.diffuseRoughness))*(1-spec);
  return std::clamp(spec/std::max(spec+diffuse,1e-30),.05,.95);
}
double referencePdf(const Query& q,glm::dvec3 v,glm::dvec3 l) {
  if(v.z<=0 || l.z<=0) return 0;
  const double p=probability(q,v);
  double result=(1-p)*l.z/pi;
  if(q.diffuseRoughness.w>0 && q.f0Scale.w>0) {
    const auto h=glm::normalize(v+l);
    const double alpha=double(q.diffuseRoughness.w)*q.diffuseRoughness.w;
    const double denominator=(h.x*h.x+h.y*h.y)+alpha*alpha*h.z*h.z;
    const double D=alpha*alpha/(pi*denominator*denominator);
    const double lambda=.5*(std::sqrt(1+alpha*alpha*(v.x*v.x+v.y*v.y)/(v.z*v.z))-1);
    result+=p*(D/(4*v.z*(1+lambda)));
  }
  return result;
}
glm::dvec3 integrate(const Query& q) {
  glm::dvec3 sum{};
  constexpr int elevations=192,azimuths=384;
  const auto v=glm::normalize(glm::dvec3(q.view));
  for(int i=0;i<elevations;++i) {
    const double z=(i+.5)/elevations,r=std::sqrt(1-z*z);
    for(int j=0;j<azimuths;++j) {
      const double phi=2*pi*(j+.5)/azimuths;
      sum+=reference(q,v,{r*std::cos(phi),r*std::sin(phi),z})*z;
    }
  }
  return sum*(2*pi/(elevations*azimuths));
}

TEST_F(PathTransportGpu, LambertSamplesCarryExactAlbedoAndCosineDensity) {
  Query q;q.f0Scale.w=0;
  const auto output=run(sampling(q,16384));
  glm::dvec3 mean{},squares{};
  for(const auto& r:output) {
    ASSERT_EQ(r.weightValid.w,1);
    const auto direction=glm::dvec3(r.directionPdf);
    EXPECT_NEAR(glm::dot(direction,direction),1,1e-6);
    EXPECT_GT(direction.z,0);
    EXPECT_NEAR(r.directionPdf.w,direction.z/pi,5e-8);
    for(int c=0;c<3;++c) EXPECT_NEAR(r.weightValid[c],q.diffuseRoughness[c],2e-6);
    mean+=direction;squares+=direction*direction;
  }
  mean/=output.size();squares/=output.size();
  EXPECT_NEAR(mean.x,0,.015);EXPECT_NEAR(mean.y,0,.015);EXPECT_NEAR(mean.z,2./3,.01);
  EXPECT_NEAR(squares.x,.25,.01);EXPECT_NEAR(squares.y,.25,.01);EXPECT_NEAR(squares.z,.5,.01);
}
TEST_F(PathTransportGpu, ReciprocalBrdfMatchesIndependentDoublePrecisionReference) {
  std::vector<Query> input;
  for(float roughness:{0.f,.1f,.35f,.8f,1.f})
    for(float viewCosine:{1.f,.8f,.3f,.02f})
      for(float lightCosine:{1.f,.7f,.1f}) {
        Query q;q.diffuseRoughness.w=roughness;q.f0Scale={.04f,.3f,.8f,.7f};
        q.view={std::sqrt(1-viewCosine*viewCosine),0,viewCosine,0};
        q.light={0,std::sqrt(1-lightCosine*lightCosine),lightCosine,0};
        input.push_back(q);std::swap(q.view,q.light);input.push_back(q);
      }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto v=glm::normalize(glm::dvec3(input[i].view)),l=glm::normalize(glm::dvec3(input[i].light));
    const auto expected=reference(input[i],v,l);
    for(int c=0;c<3;++c) {
      EXPECT_NEAR(output[i].valuePdf[c],expected[c],1e-5+std::abs(expected[c])*8e-5) << i;
      if(i%2) EXPECT_NEAR(output[i].valuePdf[c],output[i-1].valuePdf[c],1e-6+std::abs(expected[c])*1e-5);
    }
    const auto pdf=referencePdf(input[i],v,l);
    EXPECT_NEAR(output[i].valuePdf.w,pdf,1e-5+std::abs(pdf)*8e-5) << i;
  }
}
TEST_F(PathTransportGpu, GgxMixtureSamplesUseCompleteIndependentMixturePdfAndWeight) {
  std::vector<Query> input;
  for(float roughness:{.12f,.4f,.8f,1.f}) for(float viewCosine:{1.f,.7f,.1f}) {
    Query q;q.diffuseRoughness.w=roughness;
    q.view={std::sqrt(1-viewCosine*viewCosine),0,viewCosine,0};
    auto batch=sampling(q,1024);input.insert(input.end(),batch.begin(),batch.end());
  }
  const auto output=run(input);
  size_t nulls=0;
  for(size_t i=0;i<output.size();++i) {
    const auto& r=output[i];
    EXPECT_EQ(r.auxiliary.y,0);
    if(r.weightValid.w==0) {++nulls;continue;}
    const auto v=glm::normalize(glm::dvec3(input[i].view)),l=glm::normalize(glm::dvec3(r.directionPdf));
    const double pdf=referencePdf(input[i],v,l);
    const auto weight=reference(input[i],v,l)*(l.z/pdf);
    ASSERT_GT(pdf,0);EXPECT_EQ(r.auxiliary.x,0);
    EXPECT_NEAR(r.directionPdf.w,pdf,2e-6+pdf*4e-4) << i;
    EXPECT_NEAR(r.valuePdf.w,r.directionPdf.w,1e-7+pdf*1e-6);
    for(int c=0;c<3;++c) EXPECT_NEAR(r.weightValid[c],weight[c],2e-5+std::abs(weight[c])*5e-4) << i;
  }
  EXPECT_GT(nulls,10u);EXPECT_LT(nulls,input.size()/3);
}
TEST_F(PathTransportGpu, RoughWhiteMetalFurnaceMatchesOneMinusLogTwoIncludingNullEvents) {
  Query q;q.diffuseRoughness={0,0,0,1};q.f0Scale={1,1,1,1};
  const auto output=run(sampling(q,131072));
  const auto mean=meanWeights(output);
  std::printf("PATH_FURNACE samples=%zu mean=%.9f expected=%.9f\n",output.size(),mean.x,1-std::log(2.));
  for(int c=0;c<3;++c) EXPECT_NEAR(mean[c],1-std::log(2.),.004);
  const auto nulls=std::count_if(output.begin(),output.end(),[](const auto& r){return r.weightValid.w==0;});
  EXPECT_NEAR(double(nulls)/output.size(),.5,.006);
  for(const auto& r:output) {
    EXPECT_EQ(r.auxiliary.y,0);
    if(r.weightValid.w) EXPECT_NEAR(r.directionPdf.w,1/(4*pi),1e-7);
  }
}
TEST_F(PathTransportGpu, ZeroRoughnessIsAnExactDeltaWithDiscreteProbability) {
  std::vector<Query> input;
  for(float cosine:{1.f,.5f,.001f}) {
    Query q;q.diffuseRoughness={0,0,0,0};q.f0Scale={.2f,.6f,1,.7f};
    q.view={std::sqrt(1-cosine*cosine),0,cosine,0};
    auto batch=sampling(q,256);input.insert(input.end(),batch.begin(),batch.end());
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto& q=input[i];const auto& r=output[i];
    ASSERT_EQ(r.weightValid.w,1);EXPECT_EQ(r.auxiliary.x,1);EXPECT_EQ(r.directionPdf.w,1);
    EXPECT_FLOAT_EQ(r.directionPdf.x,-q.view.x);EXPECT_FLOAT_EQ(r.directionPdf.y,-q.view.y);EXPECT_FLOAT_EQ(r.directionPdf.z,q.view.z);
    EXPECT_EQ(r.valuePdf,glm::vec4(0));
    for(int c=0;c<3;++c) EXPECT_NEAR(r.weightValid[c],q.f0Scale.w*(q.f0Scale[c]+(1-q.f0Scale[c])*std::pow(1-q.view.z,5)),2e-6);
  }
}
TEST_F(PathTransportGpu, PairedEnvironmentMisConvergesToAnalyticAndQuadratureIntegrals) {
  std::array<Query,3> materials;
  materials[0].f0Scale.w=0;
  materials[1].diffuseRoughness={0,0,0,1};materials[1].f0Scale={1,1,1,1};
  materials[2].diffuseRoughness={.65f,.2f,.05f,.6f};materials[2].view={.8f,0,.6f,0};
  for(size_t i=0;i<materials.size();++i) {
    const auto output=run(sampling(materials[i],65536,2));
    const auto mean=meanWeights(output);
    const auto expected=i==0?glm::dvec3(materials[i].diffuseRoughness):
        i==1?glm::dvec3(1-std::log(2.)):integrate(materials[i]);
    glm::dvec3 variance{};
    for(const auto& r:output) {const auto delta=glm::dvec3(r.weightValid)-mean;variance+=delta*delta;}
    variance/=output.size()-1;
    std::printf("PATH_MIS material=%zu samples=%zu mean=%.9f,%.9f,%.9f expected=%.9f,%.9f,%.9f\n",
        i,output.size(),mean.x,mean.y,mean.z,expected.x,expected.y,expected.z);
    for(int c=0;c<3;++c) {
      EXPECT_NEAR(mean[c],expected[c],.007) << i << ',' << c;
      EXPECT_NEAR(mean[c],expected[c],5.5*std::sqrt(variance[c]/output.size())+.0002);
    }
  }
}
TEST_F(PathTransportGpu, PowerHeuristicStaysFiniteAcrossPdfScalesAndInvalidInputs) {
  const float nan=std::numeric_limits<float>::quiet_NaN();
  const std::array<glm::vec3,9> probes{{{1,1,.5f},{1,2,.2f},{1e30f,2e30f,.2f},
      {1e-30f,2e-30f,.2f},{0,1,0},{1,0,1},{-1,1,0},{nan,1,0},{1,nan,0}}};
  std::vector<Query> input;
  for(auto probe:probes) {Query q;q.controls.x=7;q.light={probe.x,probe.y,0,0};input.push_back(q);}
  const auto output=run(input);
  for(size_t i=0;i<probes.size();++i) EXPECT_NEAR(output[i].valuePdf.x,probes[i].z,1e-7);
}
TEST_F(PathTransportGpu, CounterAddressedRandomSequenceSurvivesBatchingReorderingAndSeedChanges) {
  auto input=sampling(Query{},4099,5);
  for(size_t i=0;i<input.size();++i) {input[i].controls.z=uint32_t(i);input[i].controls.w=345;}
  const auto whole=run(input);
  std::vector<Query> subset;
  for(size_t i=input.size();i>0;i-=std::min<size_t>(i,7)) subset.push_back(input[i-1]);
  const auto reordered=run(subset);
  for(size_t i=0;i<subset.size();++i) EXPECT_EQ(reordered[i].valuePdf,whole[subset[i].controls.z].valuePdf);
  const auto first=run(std::span<const Query>(input).first(113));
  const auto rest=run(std::span<const Query>(input).subspan(113));
  for(size_t i=0;i<whole.size();++i) EXPECT_EQ(whole[i].valuePdf,i<113?first[i].valuePdf:rest[i-113].valuePdf);
  for(auto& q:input) ++q.controls.y;
  const auto reseeded=run(input);
  for(size_t i=0;i<whole.size();++i) {
    EXPECT_NE(whole[i].valuePdf,reseeded[i].valuePdf);
    for(int c=0;c<4;++c) {EXPECT_GT(whole[i].valuePdf[c],0);EXPECT_LT(whole[i].valuePdf[c],1);}
  }
}
TEST_F(PathTransportGpu, RussianRoulettePreservesExpectedEnergyAndMinimumDepth) {
  Query q;q.sampleValue={.04f,.2f,.7f,0};q.controls={6,0,4,3};
  std::vector<Query> input(16384,q);
  for(size_t i=0;i<input.size();++i) input[i].previousMean.x=float((i+.5)/input.size());
  const auto output=run(input);
  glm::dvec3 sum{};
  for(const auto& r:output) if(r.weightValid.w) sum+=glm::dvec3(r.weightValid);
  for(int c=0;c<3;++c) EXPECT_NEAR(sum[c]/input.size(),q.sampleValue[c],8e-5);
  q.controls.z=2;q.previousMean.x=.99f;
  auto bypass=run(std::span<const Query>(&q,1));
  EXPECT_EQ(bypass[0].weightValid,glm::vec4(glm::vec3(q.sampleValue),1));
  q.sampleValue={0,0,0,0};
  EXPECT_EQ(run(std::span<const Query>(&q,1))[0].weightValid.w,0);
}
TEST_F(PathTransportGpu, EnvironmentMisNoiseDecreasesAcrossIndependentSeedReplicates) {
  constexpr size_t seeds=16,perSeed=16384;
  Query q;q.f0Scale.w=0;
  std::vector<Query> input;
  for(uint32_t seed=0;seed<seeds;++seed) {
    auto batch=sampling(q,perSeed,2);
    for(auto& value:batch) value.controls.y=17+seed*131;
    input.insert(input.end(),batch.begin(),batch.end());
  }
  const auto output=run(input);
  double firstRms=0,lastRms=0;
  for(size_t samples:{64u,256u,1024u,4096u,16384u}) {
    double squaredError=0;
    for(size_t seed=0;seed<seeds;++seed) {
      const auto mean=meanWeights(std::span<const Result>(output).subspan(seed*perSeed,samples));
      const double error=mean.x-q.diffuseRoughness.x;squaredError+=error*error;
    }
    const double rms=std::sqrt(squaredError/seeds);
    std::printf("PATH_CONVERGENCE seeds=%zu samples=%zu rms=%.9f\n",seeds,samples,rms);
    if(samples==64) firstRms=rms;
    if(samples==16384) lastRms=rms;
  }
  EXPECT_GT(firstRms,.001);EXPECT_LT(lastRms,firstRms/4);EXPECT_LT(lastRms,.004);
}
TEST_F(PathTransportGpu, RandomDimensionsAndBouncesHaveIndependentUniformMoments) {
  auto input=sampling(Query{},16384,5);
  for(size_t i=0;i<input.size();++i) input[i].controls.z=uint32_t(i);
  const auto output=run(input);
  glm::dvec4 mean{},square{};
  double cross=0;
  for(const auto& r:output) {
    const auto value=glm::dvec4(r.valuePdf);mean+=value;square+=value*value;
    cross+=value.x*value.y;
  }
  mean/=output.size();square/=output.size();cross/=output.size();
  for(int c=0;c<4;++c) {EXPECT_NEAR(mean[c],.5,.01);EXPECT_NEAR(square[c]-mean[c]*mean[c],1./12,.004);}
  EXPECT_NEAR(cross-mean.x*mean.y,0,.003);
}
TEST_F(PathTransportGpu, DeltaAndContinuousLobesRetainTheirSeparateProbabilityMeasures) {
  Query q;q.diffuseRoughness.w=0;
  const auto output=run(sampling(q,65536));
  const auto expected=integrate(q)+glm::dvec3(q.f0Scale)*double(q.f0Scale.w);
  const double p=probability(q,{0,0,1});
  size_t deltas=0;
  for(const auto& r:output) {
    ASSERT_EQ(r.weightValid.w,1);
    if(r.auxiliary.x) {++deltas;EXPECT_NEAR(r.directionPdf.w,p,1e-7);}
    else EXPECT_NEAR(r.directionPdf.w,(1-p)*r.directionPdf.z/pi,1e-7);
  }
  const auto mean=meanWeights(output);
  for(int c=0;c<3;++c) EXPECT_NEAR(mean[c],expected[c],.006);
  EXPECT_NEAR(double(deltas)/output.size(),p,.003);
}
TEST_F(PathTransportGpu, WelfordAccumulatesHdrMomentsWithoutRadianceClamping) {
  Query q;q.controls={3,0,0,0};
  const float nan=std::numeric_limits<float>::quiet_NaN();
  q.previousMean=glm::vec4(nan);q.previousMoment=glm::vec4(nan);
  const std::array<double,7> radiance{0,1,16,10000,1000000,4,1234};
  double sum=0,sumSquares=0;
  for(size_t i=0;i<radiance.size();++i) {
    q.sampleValue=glm::vec4(float(radiance[i]),float(radiance[i]*.5),float(radiance[i]*2),0);
    const auto r=run(std::span<const Query>(&q,1))[0];
    ASSERT_EQ(r.directionPdf.w,1);ASSERT_EQ(r.valuePdf.w,i+1);
    sum+=radiance[i];sumSquares+=radiance[i]*radiance[i];
    const double expectedMean=sum/(i+1),expectedMoment=sumSquares-sum*sum/(i+1);
    for(int c=0;c<3;++c) {
      const double scale=c==0?1:c==1?.5:2;
      EXPECT_NEAR(r.valuePdf[c],expectedMean*scale,.00001+expectedMean*scale*3e-6);
      EXPECT_NEAR(r.directionPdf[c],expectedMoment*scale*scale,.001+expectedMoment*scale*scale*4e-6);
    }
    q.previousMean=r.valuePdf;q.previousMoment=r.directionPdf;++q.controls.y;
  }
}
TEST_F(PathTransportGpu, AccumulationRejectsInvalidSamplesOverflowAndSampleLimitWithoutMutation) {
  Query q;q.controls={3,4,0,0};q.previousMean={1,2,3,0};q.previousMoment={4,5,6,0};
  std::vector<Query> input;
  q.sampleValue={std::numeric_limits<float>::quiet_NaN(),1,2,0};input.push_back(q);
  q.sampleValue={-1,1,2,0};input.push_back(q);
  q.sampleValue={1e30f,1,2,0};input.push_back(q);
  q.sampleValue={5,6,7,0};q.controls.y=16777216;input.push_back(q);
  q.controls.y=4;q.previousMean.x=-1;input.push_back(q);
  q.previousMean.x=1;q.previousMoment.z=-1;input.push_back(q);
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    EXPECT_EQ(output[i].directionPdf.w,0);
    EXPECT_EQ(glm::vec3(output[i].valuePdf),glm::vec3(input[i].previousMean));
    EXPECT_EQ(glm::vec3(output[i].directionPdf),glm::vec3(input[i].previousMoment));
    EXPECT_EQ(output[i].valuePdf.w,input[i].controls.y);
  }
}
TEST_F(PathTransportGpu, InvalidBsdfsDirectionsAndRandomInputsDoNotProduceEnergy) {
  Query base;base.controls.x=4;base.previousMean={.3f,.4f,.5f,0};
  std::vector<Query> input;
  auto q=base;q.diffuseRoughness.x=-1;input.push_back(q);
  q=base;q.diffuseRoughness.w=1.1f;input.push_back(q);
  q=base;q.f0Scale.z=std::numeric_limits<float>::infinity();input.push_back(q);
  q=base;q.f0Scale.w=1.1f;input.push_back(q);
  q=base;q.view={0,0,2,0};input.push_back(q);
  q=base;q.view={0,0,-1,0};input.push_back(q);
  q=base;q.previousMean.x=1;input.push_back(q);
  q=base;q.previousMean.z=std::numeric_limits<float>::quiet_NaN();input.push_back(q);
  q=base;q.diffuseRoughness={0,0,0,1e-30f};q.previousMean.x=0;input.push_back(q);
  for(const auto& r:run(input)) {
    EXPECT_EQ(r.weightValid,glm::vec4(0));EXPECT_EQ(r.directionPdf,glm::vec4(0));EXPECT_EQ(r.auxiliary.y,1);
  }
}
TEST_F(PathTransportGpu, SmallPositiveRoughnessRemainsFiniteWithoutAnImplicitRoughnessFloor) {
  Query q;q.diffuseRoughness={0,0,0,.0001f};q.f0Scale={.04f,.3f,.9f,1};
  auto evaluation=run(std::span<const Query>(&q,1))[0];
  const double alpha=double(q.diffuseRoughness.w)*q.diffuseRoughness.w;
  for(int c=0;c<3;++c) EXPECT_NEAR(evaluation.valuePdf[c],q.f0Scale[c]/(4*pi*alpha*alpha),q.f0Scale[c]/(4*pi*alpha*alpha)*2e-5);
  const auto output=run(sampling(q,1024));
  for(const auto& r:output) {
    EXPECT_EQ(r.weightValid.w,1);EXPECT_EQ(r.auxiliary.x,0);
    for(int c=0;c<3;++c) {EXPECT_TRUE(std::isfinite(r.weightValid[c]));EXPECT_NEAR(r.weightValid[c],q.f0Scale[c],2e-5);}
  }
}
TEST_F(PathTransportGpu, PerspectiveRaysMatchReverseZAndNegativeHeightViewport) {
  Query q;q.controls={8,0,200,100};
  q.inverseProjection=glm::inverse(glm::perspectiveRH_ZO(float(pi/2),2.f,100.f,.1f));
  q.inverseView=glm::inverse(glm::lookAtRH(glm::vec3(0,0,3),glm::vec3(0),glm::vec3(0,1,0)));
  std::vector<Query> input;
  for(auto pixel:{glm::vec2(100,50),glm::vec2(0,0),glm::vec2(199.75f,99.75f),glm::vec2(23.25f,17.75f)}) {
    q.light={pixel.x,pixel.y,0,0};input.push_back(q);
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto& r=output[i];const auto& pixel=input[i].light;
    ASSERT_EQ(r.valuePdf.w,1);EXPECT_EQ(glm::vec3(r.valuePdf),glm::vec3(0,0,3));
    const auto expected=glm::normalize(glm::dvec3((2*double(pixel.x)/200-1)*2,1-2*double(pixel.y)/100,-1));
    for(int c=0;c<3;++c) EXPECT_NEAR(r.directionPdf[c],expected[c],5e-7);
    EXPECT_NEAR(r.auxiliary.x,.1/std::abs(expected.z),2e-6);
    EXPECT_NEAR(r.auxiliary.y,100/std::abs(expected.z),.0002);
  }
}
TEST_F(PathTransportGpu, InfiniteFarPerspectiveAndLargeWorldTranslationPreserveDirections) {
  Query q;q.controls={8,0,200,100};q.light={23.25f,17.75f,0,0};
  glm::mat4 projection(0);projection[0][0]=.5f;projection[1][1]=1;
  projection[2][3]=-1;projection[3][2]=.01f;
  q.inverseProjection=glm::inverse(projection);
  std::vector<Query> input;
  for(auto origin:{glm::vec3(0),glm::vec3(1e8f,-2e8f,3e8f)}) {
    q.inverseView=glm::translate(glm::mat4(1),origin);input.push_back(q);
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    ASSERT_EQ(output[i].valuePdf.w,1);
    EXPECT_EQ(glm::vec3(output[i].valuePdf),glm::vec3(input[i].inverseView[3]));
    EXPECT_EQ(output[i].directionPdf,output[0].directionPdf);
    EXPECT_EQ(output[i].auxiliary,output[0].auxiliary);
    EXPECT_EQ(output[i].auxiliary.y,std::numeric_limits<float>::max());
  }
  const auto expected=glm::normalize(glm::dvec3((2*23.25/200-1)*2,1-2*17.75/100,-1));
  for(int c=0;c<3;++c) EXPECT_NEAR(output[0].directionPdf[c],expected[c],5e-7);
}
TEST_F(PathTransportGpu, OrthographicOriginsAndCameraRollUseColumnMajorWorldTransforms) {
  Query q;q.controls={8,1,200,100};
  q.inverseProjection=glm::inverse(glm::orthoRH_ZO(-2.f,2.f,-1.f,1.f,10.f,.1f));
  q.inverseView=glm::inverse(glm::lookAtRH(glm::vec3(1,2,3),glm::vec3(1,2,2),glm::vec3(1,0,0)));
  std::vector<Query> input;
  for(auto pixel:{glm::vec2(100,50),glm::vec2(0,0),glm::vec2(199.75f,99.75f)}) {
    q.light={pixel.x,pixel.y,0,0};input.push_back(q);
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto& r=output[i];const auto& pixel=input[i].light;
    ASSERT_EQ(r.valuePdf.w,1);
    const glm::dvec3 expected(1+(1-2*double(pixel.y)/100),2-(2*double(pixel.x)/200-1)*2,2.9);
    for(int c=0;c<3;++c) EXPECT_NEAR(r.valuePdf[c],expected[c],2e-6);
    EXPECT_EQ(glm::vec3(r.directionPdf),glm::vec3(0,0,-1));
    EXPECT_EQ(r.auxiliary.x,0);EXPECT_NEAR(r.auxiliary.y,9.9,2e-6);
  }
}
TEST_F(PathTransportGpu, OrthographicCameraFootprintSurvivesViewportEdgesAndWorldTranslation) {
  Query origin; origin.controls = {30, 1, 200, 100};
  origin.light = {23.25f, 17.75f, 0, 0};
  origin.inverseProjection = glm::inverse(glm::orthoRH_ZO(-2.f, 2.f, -3.f, 3.f, 10.f, .1f));
  Query translated = origin;
  translated.inverseView = glm::translate(glm::mat4(1), glm::vec3(1e8f, -2e8f, 3e8f));
  Query corner = translated;
  corner.light = {199.75f, 99.75f, 0, 0};
  Query single = translated;
  single.controls.z = single.controls.w = 1;
  single.light = {.5f, .5f, 0, 0};
  const std::array input{origin, translated, corner, single};
  const auto output = run(input);
  for (size_t i = 0; i < input.size(); ++i) {
    ASSERT_EQ(output[i].valuePdf.w, 1.f) << "case " << i;
    const float width = std::max(4.f / input[i].controls.z, 6.f / input[i].controls.w);
    EXPECT_NEAR(output[i].valuePdf.x, width, 1e-6f) << "case " << i;
    EXPECT_EQ(output[i].valuePdf.y, 0.f) << "case " << i;
  }
}
TEST_F(PathTransportGpu, PerspectiveCameraFootprintUsesBothAxesAtViewportEdges) {
  Query edge; edge.controls = {30, 0, 200, 100};
  edge.light = {199.75f, 50.25f, 0, 0};
  edge.inverseProjection = glm::inverse(glm::perspectiveRH_ZO(float(pi / 2), 2.f, 100.f, .1f));
  Query corner = edge;
  corner.light.y = 99.75f;
  const std::array input{edge, corner};
  const auto output = run(input);
  const auto direction = [](double x, double y) {
    return glm::normalize(glm::dvec3((2.0 * x / 200.0 - 1.0) * 2.0,
                                    1.0 - 2.0 * y / 100.0, -1.0));
  };
  for (size_t i = 0; i < input.size(); ++i) {
    ASSERT_EQ(output[i].valuePdf.w, 1.f) << "case " << i;
    const double x = input[i].light.x, y = input[i].light.y;
    const auto center = direction(x, y);
    const double spread = std::max(glm::length(direction(x + 1.0, y) - center),
                                   glm::length(direction(x, y + 1.0) - center));
    EXPECT_EQ(output[i].valuePdf.x, 0.f) << "case " << i;
    EXPECT_NEAR(output[i].valuePdf.y, spread, 1e-6) << "case " << i;
  }
}
TEST_F(PathTransportGpu, InvalidCameraProjectionsAndViewportCoordinatesRejectBeforeDivision) {
  Query base;base.controls={8,0,200,100};base.light={100,50,0,0};
  base.inverseProjection=glm::inverse(glm::perspectiveRH_ZO(float(pi/2),2.f,100.f,.1f));
  std::vector<Query> input;
  auto q=base;q.controls.z=0;input.push_back(q);
  q=base;q.light.x=200;input.push_back(q);
  q=base;q.light.y=-1;input.push_back(q);
  q=base;q.inverseProjection=glm::mat4(0);input.push_back(q);
  q=base;q.inverseView[0][3]=1;input.push_back(q);
  q=base;q.inverseProjection[1][1]=std::numeric_limits<float>::infinity();input.push_back(q);
  q=base;q.light.y=std::numeric_limits<float>::quiet_NaN();input.push_back(q);
  for(const auto& r:run(input)) {EXPECT_EQ(r.valuePdf,glm::vec4(0));EXPECT_EQ(r.directionPdf,glm::vec4(0));}
}
TEST_F(PathTransportGpu, NavigationCameraAngleGridProducesValidCenteredWorldRays) {
  container::scene::PerspectiveCamera camera;
  camera.setPosition({.183f,-.416f,3.714f});
  std::vector<Query> input;
  std::vector<glm::vec3> expected;
  for(int pitch=-84;pitch<=84;pitch+=12) for(int yaw=-177;yaw<=177;yaw+=6) {
    camera.setYawPitch(float(yaw),float(pitch));
    Query q;q.controls={8,0,200,100};q.light={100,50,0,0};
    q.inverseProjection=glm::inverse(camera.projectionMatrix(2));
    q.inverseView=glm::inverse(camera.viewMatrix());
    input.push_back(q);expected.push_back(camera.frontVector());
  }
  const auto output=run(input);
  std::printf("REFERENCE_CAMERA_ANGLE_GRID poses=%zu\n",input.size());
  for(size_t i=0;i<input.size();++i) {
    ASSERT_EQ(output[i].valuePdf.w,1) << "pose " << i << " inverse homogeneous W " << input[i].inverseView[3][3];
    for(int c=0;c<3;++c) {
      EXPECT_NEAR(output[i].directionPdf[c],expected[i][c],1e-6);
      EXPECT_NEAR(output[i].valuePdf[c],camera.position()[c],1e-5);
    }
  }
}
// Independent thin-sheet reference uses the unrearranged NDF and Smith lambdas.
double thinProbability(const Query& q,glm::dvec3 v) {
  const auto tint=glm::dvec3(q.sampleValue);
  if(tint==glm::dvec3(0)) return 0;
  if(q.f0Scale.w==0 && glm::dvec3(q.diffuseRoughness)==glm::dvec3(0)) return 1;
  const auto F=(glm::dvec3(q.f0Scale)+(glm::dvec3(1)-glm::dvec3(q.f0Scale))*std::pow(1-v.z,5))*double(q.f0Scale.w);
  const auto luminance=[](glm::dvec3 x){return glm::dot(x,glm::dvec3(.2126,.7152,.0722));};
  const auto f=luminance(F),r=f+(1-f)*luminance(glm::dvec3(q.diffuseRoughness)),t=(1-f)*luminance(tint);
  return std::clamp(t/std::max(r+t,1e-30),.05,.95);
}
glm::dvec3 thinReference(const Query& q,glm::dvec3 v,glm::dvec3 l) {
  if(v.z<=0 || l.z==0) return {};
  if(l.z>0) return reference(q,v,l);
  if(q.sampleValue.w==0) return {};
  l.z=-l.z;
  const auto h=glm::normalize(v+l);
  const double alpha=double(q.sampleValue.w)*q.sampleValue.w;
  const double denominator=h.x*h.x+h.y*h.y+alpha*alpha*h.z*h.z;
  const double D=alpha*alpha/(pi*denominator*denominator);
  const auto lambda=[&](glm::dvec3 w){return .5*(std::sqrt(1+alpha*alpha*(w.x*w.x+w.y*w.y)/(w.z*w.z))-1);};
  const auto F=(glm::dvec3(q.f0Scale)+(glm::dvec3(1)-glm::dvec3(q.f0Scale))*std::pow(1-glm::dot(v,h),5))*double(q.f0Scale.w);
  return glm::dvec3(q.sampleValue)*(glm::dvec3(1)-F)*(D/(4*v.z*l.z*(1+lambda(v)+lambda(l))));
}
double thinReferencePdf(const Query& q,glm::dvec3 v,glm::dvec3 l) {
  const double p=thinProbability(q,v);
  if(v.z<=0 || l.z==0) return 0;
  if(l.z>0) return (1-p)*referencePdf(q,v,l);
  if(q.sampleValue.w==0) return 0;
  l.z=-l.z;
  const auto h=glm::normalize(v+l);
  const double alpha=double(q.sampleValue.w)*q.sampleValue.w;
  const double denominator=h.x*h.x+h.y*h.y+alpha*alpha*h.z*h.z;
  const double D=alpha*alpha/(pi*denominator*denominator);
  const double lambda=.5*(std::sqrt(1+alpha*alpha*(v.x*v.x+v.y*v.y)/(v.z*v.z))-1);
  return p*D/(4*v.z*(1+lambda));
}
TEST_F(PathTransportGpu, ThinReflectionAndTransmissionMatchIndependentDoublePrecisionAndReciprocity) {
  std::vector<Query> input;
  for(float roughness:{0.f,.25f,.7f,1.f}) for(float transmissionRoughness:{0.f,.12f,.6f,1.f})
    for(float v:{1.f,.7f,.07f}) for(float l:{.9f,.5f,.02f}) for(float side:{-1.f,1.f}) {
      Query q;q.controls.x=9;q.diffuseRoughness={.1f,.05f,.02f,roughness};q.f0Scale={.04f,.1f,.3f,.8f};
      q.sampleValue={.6f,.3f,.1f,transmissionRoughness};
      q.view={std::sqrt(1-v*v),0,v,0};q.light={0,std::sqrt(1-l*l),side*l,0};input.push_back(q);
      if(side<0) {auto reversed=q;reversed.view={q.light.x,q.light.y,-q.light.z,0};reversed.light={q.view.x,q.view.y,-q.view.z,0};input.push_back(reversed);}
    }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto v=glm::dvec3(input[i].view),l=glm::dvec3(input[i].light);
    const auto value=thinReference(input[i],v,l);const auto pdf=thinReferencePdf(input[i],v,l);
    ASSERT_EQ(output[i].auxiliary.z,1);
    for(int c=0;c<3;++c) EXPECT_NEAR(output[i].valuePdf[c],value[c],1e-6+std::abs(value[c])*3e-4) << i;
    EXPECT_NEAR(output[i].valuePdf.w,pdf,1e-6+pdf*3e-4) << i;
    if(l.z<0 && i%3==1) for(int c=0;c<3;++c) EXPECT_NEAR(output[i].valuePdf[c],output[i-1].valuePdf[c],1e-6+std::abs(value[c])*3e-4);
  }
}
TEST_F(PathTransportGpu, ThinSamplesUseCompleteMixtureDensityIncludingStraightThroughDeltas) {
  std::vector<Query> input;
  for(float reflectionRoughness:{0.f,.4f}) for(float transmissionRoughness:{0.f,.1f,.5f,1.f}) for(float v:{1.f,.5f,.05f}) {
    Query q;q.diffuseRoughness={.05f,.025f,.01f,reflectionRoughness};q.sampleValue={.6f,.3f,.1f,transmissionRoughness};
    q.view={std::sqrt(1-v*v),0,v,0};auto batch=sampling(q,1024,10);input.insert(input.end(),batch.begin(),batch.end());
  }
  const auto output=run(input);size_t nulls=0,reflections=0,transmissions=0,deltas=0;
  for(size_t i=0;i<output.size();++i) {
    const auto& r=output[i];const auto& q=input[i];ASSERT_EQ(r.auxiliary.y,0) << i;
    if(r.weightValid.w==0) {++nulls;continue;}
    const auto v=glm::dvec3(q.view),l=glm::dvec3(r.directionPdf);
    EXPECT_NEAR(glm::dot(l,l),1,3e-6);
    l.z<0?++transmissions:++reflections;
    double pdf;glm::dvec3 weight;
    if(r.auxiliary.x) {
      ++deltas;
      const auto F=(glm::dvec3(q.f0Scale)+(glm::dvec3(1)-glm::dvec3(q.f0Scale))*std::pow(1-v.z,5))*double(q.f0Scale.w);
      if(l.z<0) {pdf=thinProbability(q,v);weight=glm::dvec3(q.sampleValue)*(glm::dvec3(1)-F)/pdf;}
      else {pdf=(1-thinProbability(q,v))*probability(q,v);weight=F/pdf;}
    } else {pdf=thinReferencePdf(q,v,l);weight=thinReference(q,v,l)*(std::abs(l.z)/pdf);}
    ASSERT_GT(pdf,0);
    EXPECT_NEAR(r.directionPdf.w,pdf,2e-6+pdf*9e-4) << i;
    for(int c=0;c<3;++c) EXPECT_NEAR(r.weightValid[c],weight[c],3e-5+std::abs(weight[c])*9e-4) << i;
  }
  EXPECT_GT(nulls,100u);EXPECT_GT(reflections,100u);EXPECT_GT(transmissions,100u);EXPECT_GT(deltas,100u);
}
TEST_F(PathTransportGpu, ThinWhiteRoughFurnaceIncludesBothHemispheresAndNullEvents) {
  Query q;q.diffuseRoughness={0,0,0,1};q.sampleValue={1,1,1,1};
  for(uint32_t operation:{10u,12u}) {
    const auto output=run(sampling(q,131072,operation));const auto value=meanWeights(output);
    std::printf("THIN_FURNACE operation=%u mean=%.9f expected=%.9f\n",operation,value.x,1-std::log(2.));
    for(int c=0;c<3;++c) EXPECT_NEAR(value[c],1-std::log(2.),.004);
    for(const auto& r:output) EXPECT_EQ(r.auxiliary.y,0);
  }
}
TEST_F(PathTransportGpu, ThinMixedEnvironmentMisAgreesWithIndependentSphereQuadrature) {
  Query q;q.diffuseRoughness={.12f,.06f,.03f,.4f};q.f0Scale={.04f,.12f,.3f,.8f};q.sampleValue={.5f,.25f,.125f,.7f};q.view={.8f,0,.6f,0};
  glm::dvec3 expected{};constexpr int zCount=256,phiCount=512;
  for(int z=0;z<zCount;++z) for(int p=0;p<phiCount;++p) {
    const double cosine=(z+.5)/zCount,r=std::sqrt(1-cosine*cosine),phi=2*pi*(p+.5)/phiCount;
    const glm::dvec3 l(r*std::cos(phi),r*std::sin(phi),cosine);
    expected+=(thinReference(q,glm::dvec3(q.view),l)+thinReference(q,glm::dvec3(q.view),{l.x,l.y,-l.z}))*cosine;
  }
  expected*=2*pi/(zCount*phiCount);
  const auto output=run(sampling(q,131072,12));const auto mean=meanWeights(output);
  for(int c=0;c<3;++c) EXPECT_NEAR(mean[c],expected[c],.003);
  std::printf("THIN_MIXED_MIS mean=%.9f,%.9f,%.9f expected=%.9f,%.9f,%.9f\n",mean.x,mean.y,mean.z,expected.x,expected.y,expected.z);
}
TEST_F(PathTransportGpu, ThinZeroTransmissionPreservesOriginalOpaqueSamplesExactly) {
  Query q;q.sampleValue={0,0,0,.5f};const auto original=run(sampling(q,2048,1));const auto thin=run(sampling(q,2048,10));
  ASSERT_EQ(original.size(),thin.size());
  for(size_t i=0;i<original.size();++i) {
    EXPECT_EQ(thin[i].directionPdf,original[i].directionPdf);EXPECT_EQ(thin[i].weightValid,original[i].weightValid);EXPECT_EQ(thin[i].valuePdf,original[i].valuePdf);
  }
}
TEST_F(PathTransportGpu, ThinInvalidInputsRejectWithoutInventingDeltaDensities) {
  Query base;base.controls.x=11;base.sampleValue={.8f,.4f,.2f,0};base.previousMean={.2f,.3f,.4f,0};
  auto result=run(std::array{base})[0];ASSERT_EQ(result.weightValid.w,1);EXPECT_EQ(result.auxiliary.x,1);EXPECT_EQ(result.valuePdf,glm::vec4(0));
  EXPECT_EQ(glm::vec3(result.directionPdf),-glm::vec3(base.view));
  std::vector<Query> input;
  for(float value:{-.1f,1.1f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
    auto q=base;q.sampleValue.x=value;input.push_back(q);q=base;q.sampleValue.w=value;input.push_back(q);
  }
  auto q=base;q.previousMean.x=1;input.push_back(q);q=base;q.view.z=-1;input.push_back(q);
  for(const auto& r:run(input)) {EXPECT_EQ(r.weightValid.w,0);EXPECT_EQ(r.auxiliary.y,1);EXPECT_EQ(r.directionPdf,glm::vec4(0));}
}
glm::dvec3 volumeFresnel(const Query& q,double cosine) {
  const double eta=q.previousMoment.w,sine=(1-cosine*cosine)/(eta*eta);
  if(sine>=1) return glm::dvec3(1);
  if(eta==1 && glm::dvec3(q.f0Scale)==glm::dvec3(0)) return {};
  const double angle=eta<1?std::sqrt(std::max(0.,1-sine)):cosine;
  return (glm::dvec3(q.f0Scale)+(glm::dvec3(1)-glm::dvec3(q.f0Scale))*std::pow(1-angle,5))*double(q.f0Scale.w);
}
double volumeMicroProbability(const Query& q,glm::dvec3 v) {
  const auto diffuse=glm::dvec3(q.diffuseRoughness),tint=glm::dvec3(q.sampleValue);
  if(diffuse==glm::dvec3(0)) return 1;
  if(q.f0Scale.w==0 && tint==glm::dvec3(0) && q.previousMoment.w>=1) return 0;
  const auto F=volumeFresnel(q,v.z);
  const auto lum=[](glm::dvec3 a){return glm::dot(a,glm::dvec3(.2126,.7152,.0722));};
  const double micro=lum(F+tint*(glm::dvec3(1)-F)),d=lum(diffuse*(glm::dvec3(1)-F));
  return std::clamp(micro/std::max(micro+d,1e-30),.05,.95);
}
glm::dvec4 volumeReference(const Query& q,glm::dvec3 v,glm::dvec3 l) {
  if(v.z<=0 || l.z==0) return {};
  const double eta=q.previousMoment.w;const bool reflection=l.z>0;
  auto h=v+(reflection?1.:eta)*l;
  if(glm::dot(h,h)==0) return {};
  h=glm::normalize(h);if(h.z<0) h=-h;
  const double vh=glm::dot(v,h),lh=glm::dot(l,h);
  if(vh<=0 || lh*l.z<=0) return {};
  const auto F=volumeFresnel(q,vh);
  const auto lum=[](glm::dvec3 a){return glm::dot(a,glm::dvec3(.2126,.7152,.0722));};
  const double R=lum(F),T=lum(glm::dvec3(q.sampleValue)*(glm::dvec3(1)-F));
  const double pr=R+T>0?R/(R+T):0,pm=volumeMicroProbability(q,v);
  glm::dvec3 value{};double pdf=0;
  if(reflection) {value=glm::dvec3(q.diffuseRoughness)*(glm::dvec3(1)-F)/pi;pdf=(1-pm)*l.z/pi;}
  if(q.diffuseRoughness.w>0) {
    const double alpha=double(q.diffuseRoughness.w)*q.diffuseRoughness.w;
    const double ndfDenom=h.x*h.x+h.y*h.y+alpha*alpha*h.z*h.z,D=alpha*alpha/(pi*ndfDenom*ndfDenom);
    const auto lambda=[&](glm::dvec3 w){return .5*(std::sqrt(1+alpha*alpha*(w.x*w.x+w.y*w.y)/(w.z*w.z))-1);};
    const double G=1/(1+lambda(v)+lambda(l)),G1=1/(1+lambda(v)),halfPdf=D*G1*vh/v.z;
    if(reflection) {value+=F*(D*G/(4*v.z*l.z));pdf+=pm*pr*halfPdf/(4*vh);}
    else {
      const double denominator=vh+eta*lh,square=denominator*denominator;
      if(square==0) return {};
      value=glm::dvec3(q.sampleValue)*(glm::dvec3(1)-F)*(D*G*std::abs(vh*lh)/(v.z*std::abs(l.z)*square));
      pdf=pm*(1-pr)*halfPdf*eta*eta*std::abs(lh)/square;
    }
  }
  return {value,pdf};
}
Query volumeQuery(float eta,float roughness=.5f) {
  Query q;q.previousMoment.w=eta;q.diffuseRoughness={0,0,0,roughness};q.sampleValue={1,1,1,0};
  const float r=(eta-1)/(eta+1);q.f0Scale={r*r,r*r,r*r,1};return q;
}
TEST_F(PathTransportGpu, VolumeFresnelUsesRelativeIorExitAngleAndUnscaledTotalInternalReflection) {
  std::vector<Query> input;
  for(float eta:{1.f,1.33f,1.5f,2.4f,1.f/1.5f,.5f}) for(float cosine:{1.f,.9f,.7f,.3f,.02f}) for(float scale:{0.f,.3f,1.f}) {
    auto q=volumeQuery(eta);q.controls.x=18;q.view={std::sqrt(1-cosine*cosine),0,cosine,0};q.f0Scale.w=scale;input.push_back(q);
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto F=volumeFresnel(input[i],input[i].view.z);
    for(int c=0;c<3;++c) EXPECT_NEAR(output[i].valuePdf[c],F[c],2e-6);
    const double eta=input[i].previousMoment.w,cosine=input[i].view.z;
    EXPECT_NEAR(output[i].valuePdf.w,std::sqrt(std::max(0.,1-(1-cosine*cosine)/(eta*eta))),2e-6);
  }
}
TEST_F(PathTransportGpu, SmoothVolumeSamplesObeySnellAndRadianceMeasureIncludingTir) {
  std::vector<Query> input;
  for(float eta:{1.f,1.33f,1.5f,2.4f,1.f/1.5f,.5f}) for(float cosine:{1.f,.9f,.7f,.3f}) for(float choice:{.001f,.999f}) {
    auto q=volumeQuery(eta,0);q.controls.x=15;q.sampleValue={.8f,.4f,.2f,0};q.previousMean={choice,.2f,.3f,0};
    q.view={std::sqrt(1-cosine*cosine),0,cosine,0};input.push_back(q);
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto& q=input[i];const auto& r=output[i];const auto v=glm::dvec3(q.view),F=volumeFresnel(q,v.z),tint=glm::dvec3(q.sampleValue);
    const auto lum=[](glm::dvec3 a){return glm::dot(a,glm::dvec3(.2126,.7152,.0722));};
    const double R=lum(F),T=lum(tint*(glm::dvec3(1)-F)),probability=R/(R+T),eta=q.previousMoment.w;
    const bool reflection=q.previousMean.x<probability;
    const double pdf=reflection?probability:1-probability;
    const auto weight=reflection?F/pdf:tint*(glm::dvec3(1)-F)/(pdf*eta*eta);
    glm::dvec3 direction(-v.x,-v.y,v.z);
    if(!reflection) direction=-v/eta+glm::dvec3(0,0,v.z/eta-std::sqrt(std::max(0.,1-(1-v.z*v.z)/(eta*eta))));
    ASSERT_EQ(r.weightValid.w,1) << i;EXPECT_EQ(r.auxiliary.x,1);EXPECT_EQ(r.auxiliary.y,0);EXPECT_EQ(r.valuePdf,glm::vec4(0));
    EXPECT_NEAR(r.directionPdf.w,pdf,2e-6);
    for(int c=0;c<3;++c) {EXPECT_NEAR(r.directionPdf[c],direction[c],3e-6);EXPECT_NEAR(r.weightValid[c],weight[c],3e-6);}
  }
}
TEST_F(PathTransportGpu, RoughVolumeEvaluationAndJacobianMatchIndependentDoublePrecision) {
  std::vector<Query> input;
  for(float eta:{1.f/1.5f,.8f,1.2f,1.5f,2.4f}) for(float roughness:{.2f,.5f,1.f})
    for(float v:{1.f,.7f,.1f}) for(float l:{1.f,.7f,.02f}) for(float side:{-1.f,1.f}) {
      auto q=volumeQuery(eta,roughness);q.controls.x=13;q.diffuseRoughness.x=.1f;q.sampleValue={.6f,.3f,.1f,0};q.f0Scale.w=.8f;
      q.view={std::sqrt(1-v*v),0,v,0};q.light={0,std::sqrt(1-l*l),side*l,0};input.push_back(q);
    }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto expected=volumeReference(input[i],glm::dvec3(input[i].view),glm::dvec3(input[i].light));
    ASSERT_EQ(output[i].auxiliary.z,1);
    for(int c=0;c<4;++c) EXPECT_NEAR(output[i].valuePdf[c],expected[c],2e-6+std::abs(expected[c])*.0008) << i << " channel " << c;
  }
}
TEST_F(PathTransportGpu, RoughVolumeSamplesUseCompleteMixtureDensityAndRadianceWeights) {
  std::vector<Query> input;
  for(float eta:{1.f/1.5f,.8f,1.2f,1.5f,2.4f}) for(float roughness:{.2f,.6f,1.f}) for(float v:{1.f,.2f}) {
    auto q=volumeQuery(eta,roughness);q.diffuseRoughness.x=.1f;q.sampleValue={.6f,.3f,.1f,0};q.f0Scale.w=.8f;
    q.view={std::sqrt(1-v*v),0,v,0};auto batch=sampling(q,512,14);input.insert(input.end(),batch.begin(),batch.end());
  }
  const auto output=run(input);size_t nulls=0,reflected=0,transmitted=0;
  for(size_t i=0;i<output.size();++i) {
    const auto& r=output[i];ASSERT_EQ(r.auxiliary.y,0) << i;
    if(r.weightValid.w==0) {++nulls;continue;}
    const auto v=glm::dvec3(input[i].view),l=glm::dvec3(r.directionPdf);const auto expected=volumeReference(input[i],v,l);
    EXPECT_NEAR(glm::dot(l,l),1,3e-6);ASSERT_GT(expected.w,0) << i;
    l.z<0?++transmitted:++reflected;
    EXPECT_NEAR(r.directionPdf.w,expected.w,2e-5+expected.w*.003) << i;
    for(int c=0;c<3;++c) EXPECT_NEAR(r.weightValid[c],expected[c]*std::abs(l.z)/expected.w,3e-5+std::abs(r.weightValid[c])*.003) << i;
  }
  EXPECT_GT(nulls,100u);EXPECT_GT(reflected,100u);EXPECT_GT(transmitted,100u);
}
TEST_F(PathTransportGpu, RefractiveRadianceReciprocityIncludesSquaredRelativeIor) {
  std::vector<Query> input;
  for(float eta:{1.2f,1.5f,2.4f}) for(float roughness:{.3f,.7f,1.f}) for(float cosine:{.95f,.5f,.1f}) {
    auto q=volumeQuery(eta,roughness);q.controls.x=13;q.view={.6f,0,.8f,0};q.light={0,std::sqrt(1-cosine*cosine),-cosine,0};input.push_back(q);
    auto reversed=q;reversed.previousMoment.w=1/eta;reversed.view=-q.light;reversed.light=-q.view;input.push_back(reversed);
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();i+=2) for(int c=0;c<3;++c) {
    const double expected=output[i].valuePdf[c]*input[i].previousMoment.w*input[i].previousMoment.w;
    EXPECT_NEAR(output[i+1].valuePdf[c],expected,3e-6+std::abs(expected)*.001);
  }
}
TEST_F(PathTransportGpu, RoughVolumeEnvironmentMisMatchesIndependentSphereQuadrature) {
  for(float eta:{1.5f,1.f/1.5f}) {
    auto q=volumeQuery(eta,.8f);q.diffuseRoughness.x=.1f;q.sampleValue={.6f,.4f,.2f,0};q.view={.8f,0,.6f,0};
    glm::dvec3 expected{};constexpr int zCount=384,phiCount=768;
    for(int z=0;z<zCount;++z) for(int p=0;p<phiCount;++p) for(double side:{-1.,1.}) {
      const double cosine=(z+.5)/zCount,r=std::sqrt(1-cosine*cosine),phi=2*pi*(p+.5)/phiCount;
      const auto value=volumeReference(q,glm::dvec3(q.view),{r*std::cos(phi),r*std::sin(phi),side*cosine});
      expected+=glm::dvec3(value)*cosine;
    }
    expected*=2*pi/(zCount*phiCount);
    for(uint32_t operation:{14u,17u}) {
      const auto output=run(sampling(q,131072,operation));const auto mean=meanWeights(output);
      for(int c=0;c<3;++c) EXPECT_NEAR(mean[c],expected[c],.006);
      std::printf("VOLUME_ENV eta=%.7f op=%u mean=%.9f,%.9f,%.9f expected=%.9f,%.9f,%.9f\n",eta,operation,mean.x,mean.y,mean.z,expected.x,expected.y,expected.z);
      for(const auto& r:output) EXPECT_EQ(r.auxiliary.y,0);
    }
  }
}
TEST_F(PathTransportGpu, IndexMatchedRoughVolumeIsStraightThroughAndRadianceFactorsCancel) {
  auto q=volumeQuery(1,1);q.controls.x=15;q.previousMean={.2f,.3f,.4f,0};q.view={.8f,0,.6f,0};
  auto r=run(std::array{q})[0];EXPECT_EQ(glm::vec3(r.directionPdf),-glm::vec3(q.view));EXPECT_EQ(r.directionPdf.w,1);EXPECT_EQ(r.weightValid,glm::vec4(1));EXPECT_EQ(r.auxiliary.x,1);
  q=volumeQuery(1.5f,0);q.f0Scale.w=0;q.controls.x=15;q.previousMean={.5f,.3f,.4f,0};q.view={.6f,0,.8f,0};
  const auto enter=run(std::array{q})[0];q.previousMoment.w=1/1.5f;q.view=glm::vec4(-glm::vec3(enter.directionPdf),0);
  const auto leave=run(std::array{q})[0];
  for(int c=0;c<3;++c) EXPECT_NEAR(enter.weightValid[c]*leave.weightValid[c],1,5e-7);
}
TEST_F(PathTransportGpu, BeerAbsorptionPreservesZeroBlackInfiniteAndWorldDistanceLimits) {
  std::vector<Query> input;
  for(float distance:{0.f,.125f,1.f,2.f,10.f}) for(float densityDistance:{.25f,2.f,std::numeric_limits<float>::infinity()}) {
    Query q;q.controls.x=16;q.sampleValue={0,.5f,1,densityDistance};q.previousMean.x=distance;input.push_back(q);
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    EXPECT_EQ(output[i].valuePdf.w,1);
    for(int c=0;c<3;++c) {
      const double expected=input[i].previousMean.x==0 || std::isinf(input[i].sampleValue.w)?1:std::pow(input[i].sampleValue[c],double(input[i].previousMean.x)/input[i].sampleValue.w);
      EXPECT_NEAR(output[i].valuePdf[c],expected,5e-7);
    }
  }
  input.clear();Query q;q.controls.x=16;q.sampleValue={1,1,1,0};input.push_back(q);
  q.sampleValue.w=-1;input.push_back(q);q.sampleValue.w=std::numeric_limits<float>::quiet_NaN();input.push_back(q);
  q.sampleValue={-1,1,1,1};input.push_back(q);q.sampleValue={1.1f,1,1,1};input.push_back(q);
  for(const auto& value:run(input)) EXPECT_EQ(value.valuePdf,glm::vec4(0));
}
TEST_F(PathTransportGpu, InvalidVolumeInputsRejectWithoutFabricatedTransport) {
  auto base=volumeQuery(1.5f);base.controls.x=15;base.previousMean={.5f,.2f,.3f,0};std::vector<Query> input;
  for(float value:{0.f,-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
    auto q=base;q.previousMoment.w=value;input.push_back(q);
  }
  auto q=base;q.sampleValue.x=-.1f;input.push_back(q);q=base;q.diffuseRoughness.w=1.1f;input.push_back(q);
  q=base;q.previousMean.x=1;input.push_back(q);q=base;q.view.z=-1;input.push_back(q);
  for(const auto& result:run(input)) {EXPECT_EQ(result.weightValid.w,0);EXPECT_EQ(result.auxiliary.y,1);}
}
glm::dvec4 volumeMixtureReference(const Query& q,glm::dvec3 v,glm::dvec3 l) {
  Query metal=q;metal.diffuseRoughness=glm::vec4(0,0,0,q.diffuseRoughness.w);metal.f0Scale=glm::vec4(glm::vec3(q.previousMean),1);
  const double weight=q.previousMoment.x;
  return volumeReference(q,v,l)*(1-weight)+glm::dvec4(reference(metal,v,l),referencePdf(metal,v,l))*weight;
}
TEST_F(PathTransportGpu, VolumeConductorMixtureMatchesIndependentWeightedBsdfAndCompletePdf) {
  std::vector<Query> input;
  for(float eta:{1.5f,1.f/1.5f}) for(float m:{.1f,.5f,.9f}) for(float rough:{.2f,.6f,1.f}) for(float side:{-1.f,1.f}) {
    auto q=volumeQuery(eta,rough);q.controls.x=19;q.diffuseRoughness.x=.1f;q.sampleValue={.6f,.4f,.2f,0};
    q.previousMean={.2f,.4f,.8f,0};q.previousMoment.x=m;q.view={.8f,0,.6f,0};q.light={0,.6f,side*.8f,0};input.push_back(q);
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto expected=volumeMixtureReference(input[i],glm::dvec3(input[i].view),glm::dvec3(input[i].light));
    ASSERT_EQ(output[i].auxiliary.z,1);for(int c=0;c<4;++c) EXPECT_NEAR(output[i].valuePdf[c],expected[c],2e-6+std::abs(expected[c])*.001);
  }
}
TEST_F(PathTransportGpu, VolumeConductorSamplesCarryFullWeightedDensityAndThroughput) {
  std::vector<Query> input;
  for(float eta:{1.5f,1.f/1.5f}) for(float m:{.1f,.5f,.9f}) for(float rough:{.3f,.8f}) {
    auto q=volumeQuery(eta,rough);q.diffuseRoughness.x=.1f;q.sampleValue={.6f,.4f,.2f,0};q.previousMean={.2f,.4f,.8f,0};q.previousMoment.x=m;q.view={.8f,0,.6f,0};
    auto batch=sampling(q,512,20);input.insert(input.end(),batch.begin(),batch.end());
  }
  const auto output=run(input);
  for(size_t i=0;i<input.size();++i) {
    const auto& r=output[i];ASSERT_EQ(r.auxiliary.y,0);if(r.weightValid.w==0) continue;
    const auto l=glm::dvec3(r.directionPdf);const auto expected=volumeMixtureReference(input[i],glm::dvec3(input[i].view),l);
    ASSERT_GT(expected.w,0);EXPECT_NEAR(r.directionPdf.w,expected.w,3e-5+expected.w*.003);
    for(int c=0;c<3;++c) EXPECT_NEAR(r.weightValid[c],expected[c]*std::abs(l.z)/expected.w,3e-5+std::abs(r.weightValid[c])*.003);
  }
}
TEST_F(PathTransportGpu, MixedMetalTirMergesCoincidentDeltaMassesWithoutWhiteningConductor) {
  auto base=volumeQuery(1.f/1.5f,0);base.f0Scale.w=0;base.previousMean={.2f,.4f,.8f,0};base.previousMoment.x=.25f;base.view={.8f,0,.6f,0};
  const auto output=run(sampling(base,4096,20));const auto metal=glm::dvec3(base.previousMean);
  const auto expected=glm::dvec3(.75)+.25*(metal+(glm::dvec3(1)-metal)*std::pow(.4,5));
  for(const auto& r:output) {
    ASSERT_EQ(r.weightValid.w,1);EXPECT_EQ(r.auxiliary.x,1);EXPECT_EQ(r.directionPdf.w,1);
    for(int c=0;c<3;++c) EXPECT_NEAR(r.weightValid[c],expected[c],2e-7);
  }
}
TEST_F(PathTransportGpu, VolumeConductorEnvironmentMisMatchesIndependentMixtureQuadrature) {
  auto q=volumeQuery(1.5f,.8f);q.diffuseRoughness.x=.1f;q.sampleValue={.6f,.4f,.2f,0};q.previousMean={.2f,.4f,.8f,0};q.previousMoment.x=.35f;q.view={.8f,0,.6f,0};
  glm::dvec3 expected{};constexpr int zCount=256,phiCount=512;
  for(int z=0;z<zCount;++z) for(int p=0;p<phiCount;++p) for(double side:{-1.,1.}) {
    const double cosine=(z+.5)/zCount,r=std::sqrt(1-cosine*cosine),phi=2*pi*(p+.5)/phiCount;
    expected+=glm::dvec3(volumeMixtureReference(q,glm::dvec3(q.view),{r*std::cos(phi),r*std::sin(phi),side*cosine}))*cosine;
  }
  expected*=2*pi/(zCount*phiCount);const auto mean=meanWeights(run(sampling(q,131072,22)));
  for(int c=0;c<3;++c) EXPECT_NEAR(mean[c],expected[c],.005);
}
TEST_F(PathTransportGpu, ConductorMixtureEndpointsPreservePureSamplersAndRejectInvalidWeights) {
  auto q=volumeQuery(1.5f,.6f);q.previousMean={.2f,.4f,.8f,0};q.previousMoment.x=0;
  const auto pure=run(sampling(q,512,14)),mixed=run(sampling(q,512,20));
  for(size_t i=0;i<pure.size();++i) {EXPECT_EQ(pure[i].directionPdf,mixed[i].directionPdf);EXPECT_EQ(pure[i].weightValid,mixed[i].weightValid);}
  q.previousMoment.x=1;auto metal=q;metal.diffuseRoughness={0,0,0,q.diffuseRoughness.w};metal.f0Scale=glm::vec4(glm::vec3(q.previousMean),1);
  const auto endpoint=run(sampling(q,512,20)),conductor=run(sampling(metal,512,1));
  for(size_t i=0;i<endpoint.size();++i) {EXPECT_EQ(endpoint[i].directionPdf,conductor[i].directionPdf);EXPECT_EQ(endpoint[i].weightValid,conductor[i].weightValid);}
  std::vector<Query> invalid;
  for(float weight:{-.1f,1.1f,std::numeric_limits<float>::quiet_NaN()}) {auto item=q;item.controls.x=21;item.previousMoment.x=weight;item.light={.2f,.3f,.4f,0};invalid.push_back(item);}
  for(const auto& r:run(invalid)) {EXPECT_EQ(r.weightValid.w,0);EXPECT_EQ(r.auxiliary.y,1);}
}
TEST_F(PathTransportGpu, MaterialTextureZeroFactorsStayInactive) {
  std::vector<Query> input;
  for (uint32_t texture : {UINT32_MAX, 0u, 4095u}) {
    Query q; q.controls = {23, texture, 0, 0}; q.diffuseRoughness = glm::vec4(0);
    input.push_back(q);
  }
  for (const auto& value : run(input)) {
    EXPECT_EQ(value.valuePdf, glm::vec4(0));
    EXPECT_FLOAT_EQ(value.auxiliary.x, .045f);
  }
}
TEST_F(PathTransportGpu, MaterialTextureFactorsPreserveAuthoredValuesAndHdrColor) {
  std::vector<Query> input(3);
  for (auto& q : input) q.controls = {23, 7, 0, 0};
  input[0].diffuseRoughness = {.25f, 1.5f, -1, 0};
  input[1].diffuseRoughness = {2, -2, 0, 0};
  input[2].diffuseRoughness = {.02f, 0, .5f, 0};
  const auto output = run(input);
  const std::array expected{glm::vec4(.25f, 1.5f, 0, .25f),
      glm::vec4(2, 0, 0, 1), glm::vec4(.02f, 0, .5f, .02f)};
  const std::array roughness{.25f, 1.f, .045f};
  for (size_t i = 0; i < output.size(); ++i) {
    EXPECT_EQ(output[i].valuePdf, expected[i]);
    EXPECT_FLOAT_EQ(output[i].auxiliary.x, roughness[i]);
  }
}
TEST_F(PathTransportGpu, MaterialTextureFactorsSanitizeNonfiniteInputsPerChannel) {
  Query q; q.controls = {23, 1, 0, 0};
  q.diffuseRoughness = {std::numeric_limits<float>::quiet_NaN(), .75f,
      std::numeric_limits<float>::infinity(), 0};
  const auto output = run(std::array{q});
  ASSERT_EQ(output.size(), 1u);
  EXPECT_EQ(output[0].valuePdf, glm::vec4(0, .75f, 0, 0));
  EXPECT_FLOAT_EQ(output[0].auxiliary.x, .045f);
}
TEST_F(PathTransportGpu, RayConeFootprintMatchesReferenceSlantGeometry) {
  Query q; q.controls = {24, 0, 0, 0};
  q.light = {0, 0, 1, 0}; // surface normal
  // Normal incidence accumulates width and spread exactly.
  q.view = {0, 0, 1, 0};
  q.footprintA = {.5f, .01f, 10.f, 0};
  const auto normal = run(std::array{q});
  EXPECT_FLOAT_EQ(float(normal[0].valuePdf.x), .6f);
  // A 45 degree slant stretches the footprint by 1/cos.
  q.view = glm::vec4(glm::normalize(glm::vec3(1, 0, 1)), 0);
  const auto slant = run(std::array{q});
  EXPECT_NEAR(float(slant[0].valuePdf.x), .6f * std::sqrt(2.f), 1e-6f);
  // Grazing view directions clamp at the reference 1e-3 slant floor.
  q.view = {1, 0, 0, 0};
  const auto grazing = run(std::array{q});
  EXPECT_NEAR(float(grazing[0].valuePdf.x), 600.f, 1e-3f);
  // Orthographic-style cones start with a width at the origin and no spread.
  q.view = {0, 0, 1, 0};
  q.footprintA = {.25f, 0.f, 4.f, 0};
  const auto carried = run(std::array{q});
  EXPECT_FLOAT_EQ(float(carried[0].valuePdf.x), .25f);
}
TEST_F(PathTransportGpu, ReconstructedSurfaceCarriesPerSetTexelDensity) {
  // A unit world triangle: UV0 is stretched 2x per world unit, UV1 is
  // degenerate (constant), and barycentric interpolation stays exact.
  Query q; q.controls = {25, 1, 0, 0};
  q.view = {0, 0, 0, 1};        // corner position 0
  q.light = {1, 0, 0, 1};       // corner position 1
  q.previousMean = {0, 1, 0, 1}; // corner position 2
  q.footprintA = {0, 0, .5f, .5f};
  q.footprintB = {2, 0, .5f, .5f};
  q.footprintC = {0, 2, .5f, .5f};
  q.previousMoment = {.25f, .25f, 0, 0}; // barycentrics
  const auto identity = run(std::array{q});
  EXPECT_FLOAT_EQ(float(identity[0].valuePdf.w), 1.f);
  EXPECT_NEAR(float(identity[0].valuePdf.x), 2.f, 1e-6f);
  EXPECT_NEAR(float(identity[0].valuePdf.y), 0.f, 1e-7f);
  EXPECT_NEAR(float(identity[0].directionPdf.x), .5f, 1e-6f);
  EXPECT_NEAR(float(identity[0].directionPdf.y), .5f, 1e-6f);
  EXPECT_NEAR(float(identity[0].auxiliary.x), .25f, 1e-6f);
  EXPECT_NEAR(float(identity[0].auxiliary.y), .25f, 1e-6f);
  // A 2x instance scale quadruples world area and halves the density,
  // exercising the model-transformed world edges.
  q.inverseView = glm::scale(glm::mat4(1), glm::vec3(2));
  const auto scaled = run(std::array{q});
  EXPECT_FLOAT_EQ(float(scaled[0].valuePdf.w), 1.f);
  EXPECT_NEAR(float(scaled[0].valuePdf.x), 1.f, 1e-6f);
  EXPECT_NEAR(float(scaled[0].directionPdf.x), .5f, 1e-6f);
}
TEST_F(PathTransportGpu, EqualSolidAngleProbeSpreadMatchesEquivalentCone) {
  std::vector<Query> input;
  for (uint32_t rays : {1u, 2u, 3u, 4u, 5u, 27u, 128u, 1024u, 4096u}) {
    Query q; q.controls = {26, 0, rays, 0};
    input.push_back(q);
  }
  const auto output = run(input);
  for (size_t i = 0; i < input.size(); ++i) {
    const uint32_t rays = input[i].controls.z;
    const float spread = output[i].valuePdf.x;
    if (rays <= 2u) {
      // A hemisphere or larger needs a finite cap on the cone's diameter.
      EXPECT_NEAR(spread, 2000.f, 1e-3f) << rays << " rays";
      continue;
    }
    // Invert the spherical cap area 2*pi*(1 - cos(halfAngle)) = 4*pi/rays.
    const double halfAngle = std::acos(1.0 - 2.0 / rays);
    EXPECT_NEAR(spread, 2.0 * std::tan(halfAngle), 1e-6) << rays << " rays";
    EXPECT_LT(spread, output[i - 1].valuePdf.x) << rays << " rays";
  }
}
TEST_F(PathTransportGpu, ExplicitGradientSamplingSelectsAuthoredMip) {
  std::vector<Query> input;
  for (uint32_t mip = 0; mip < kContractTextureMips; ++mip) {
    Query q; q.controls = {27, 0, 0, 0};
    q.light = {.5f, .5f, 0, 0}; // constant mips: any UV works
    q.footprintA = {1.f, 0.f, 0.f, 1.f}; // identity transform rows
    // A gradient of 2^mip / texture width addresses exactly mip level mip.
    q.previousMean = {float(1u << mip) / float(kContractTextureWidth), float(mip), 0, 0};
    input.push_back(q);
  }
  const auto output = run(input);
  for (uint32_t mip = 0; mip < kContractTextureMips; ++mip) {
    const auto colour = contractMipColour(mip);
    const glm::vec4 expected(colour.x / 255.f, colour.y / 255.f, colour.z / 255.f, 1.f);
    for (uint32_t channel = 0; channel < 4; ++channel)
      EXPECT_NEAR(output[mip].valuePdf[channel], expected[channel], 1.f / 255.f)
          << "mip " << mip << " channel " << channel;
    // The explicit gradient and an equivalent explicit LOD agree exactly.
    for (uint32_t channel = 0; channel < 4; ++channel)
      EXPECT_NEAR(output[mip].valuePdf[channel], output[mip].directionPdf[channel], 1e-5f)
          << "mip " << mip << " channel " << channel;
  }
  // A zero gradient keeps the base mip, matching the production fallback.
  Query base; base.controls = {27, 0, 0, 0};
  base.light = {.5f, .5f, 0, 0};
  base.footprintA = {1.f, 0.f, 0.f, 1.f};
  base.previousMean = {0, 0, 0, 0};
  const auto baseOutput = run(std::array{base});
  const auto baseColour = contractMipColour(0);
  EXPECT_NEAR(float(baseOutput[0].valuePdf.x), baseColour.x / 255.f, 1.f / 255.f);
  EXPECT_NEAR(float(baseOutput[0].valuePdf.y), baseColour.y / 255.f, 1.f / 255.f);
}
TEST_F(PathTransportGpu, RotatedTextureTransformSelectsMipOnRectangularImage) {
  Query q; q.controls = {27, 0, 0, 0};
  q.light = {.5f, .5f, 0, 0};
  // Rotate a 4x1 scale by 90 degrees. At a UV gradient of 1/16, the two
  // transformed gradients cover 4 and 16 texels, selecting mip 4.
  // Treating the transform rows as gradients would select mip 6.
  q.footprintA = {0.f, -1.f, 4.f, 0.f};
  q.previousMean = {1.f / 16.f, 4.f, 0.f, 0.f};
  const auto output = run(std::array{q});
  const auto colour = contractMipColour(4);
  EXPECT_NEAR(output[0].valuePdf.x, colour.x / 255.f, 1.f / 255.f);
  EXPECT_NEAR(output[0].valuePdf.x, output[0].directionPdf.x, 1.f / 255.f);
}
TEST_F(PathTransportGpu, DispersionChannelIorsMatchTheSharedRasterMapping) {
  std::vector<Query> input;
  struct Case { float ior, dispersion; uint32_t channel; };
  for (const auto& item : std::array<Case, 6>{{
      {1.5f, 30.f, 0u}, {1.5f, 30.f, 1u}, {1.5f, 30.f, 2u},
      {1.2f, 0.f, 0u}, {2.4f, 65.f, 0u}, {1.5f, 30.f, 2u}}}) {
    Query q; q.controls = {28, 0, item.channel, 0};
    q.previousMoment.w = item.ior;
    q.sampleValue.w = item.dispersion;
    input.push_back(q);
  }
  const auto output = run(input);
  for (size_t i = 0; i < input.size(); ++i) {
    const float ior = input[i].previousMoment.w, dispersion = input[i].sampleValue.w;
    const uint32_t channel = input[i].controls.z;
    const float safeIor = std::max(ior, 1.f);
    const float halfSpread = (safeIor - 1.f) * .025f * std::max(dispersion, 0.f);
    const float expected = channel == 0u ? std::max(safeIor - halfSpread, 1.f)
        : channel == 2u ? safeIor + halfSpread : safeIor;
    EXPECT_NEAR(output[i].valuePdf[channel], expected, 1e-6f) << "case " << i;
    EXPECT_NEAR(float(output[i].valuePdf.w), expected, 1e-6f) << "case " << i;
  }
  // Red bends least, blue most, and zero dispersion keeps the authored IOR.
  EXPECT_FLOAT_EQ(float(output[0].valuePdf.x), 1.5f - .5f * .025f * 30.f);
  EXPECT_FLOAT_EQ(float(output[1].valuePdf.y), 1.5f);
  EXPECT_FLOAT_EQ(float(output[2].valuePdf.z), 1.5f + .5f * .025f * 30.f);
  EXPECT_FLOAT_EQ(float(output[3].valuePdf.x), 1.2f);
}
TEST_F(PathTransportGpu, SpectralDispersionEstimatorMatchesPerChannelQuadrature) {
  Query q; q.controls = {29, 4242, 17, 0};
  q.view = {0, 0, 1, 0};
  q.diffuseRoughness = {0, 0, 0, 0};   // no diffuse lobe, zero roughness
  q.f0Scale = {.04f, .04f, .04f, 1};
  q.sampleValue = {1, 1, 1, 0};        // white transmission
  q.previousMoment.w = 1.5f;           // authored IOR
  q.footprintA.x = 30.f;               // authored dispersion
  const auto output = run(std::array{q});
  ASSERT_EQ(output.size(), 1u);
  // All three channels sampled valid transmitted delta events.
  EXPECT_FLOAT_EQ(float(output[0].auxiliary.w), 1.f);
  EXPECT_GT(float(output[0].auxiliary.x), float(output[0].auxiliary.y));
  EXPECT_GT(float(output[0].auxiliary.y), float(output[0].auxiliary.z));
  // The 3x-weighted uniform channel estimate converges to each channel's
  // own transmitted weight within Monte-Carlo noise (8192 samples).
  for (uint32_t channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(output[0].directionPdf[channel], output[0].valuePdf[channel], .05f)
        << "channel " << channel;
}
} // namespace
