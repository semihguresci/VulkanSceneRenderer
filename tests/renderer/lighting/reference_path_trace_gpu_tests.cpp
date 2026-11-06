#include "Container/common/CommonGLFW.h"
#include "Container/renderer/raytracing/RaySceneAcceleration.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include <gtest/gtest.h>
#include <Container/renderer/pathtracing/ReferencePathTracePass.h>
#include <fstream>
#include <algorithm>
#include <chrono>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <tuple>

namespace {
using namespace container::renderer;
using namespace container::gpu;

enum class SurfaceFault { Normal, Tangent, Color, InverseTransform };

class ReferencePathTraceGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<ReferencePathTraceGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const char *enabled = std::getenv("CONTAINER_RUN_GPU_REFERENCE_PATH_TRACE");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP()
          << "Set CONTAINER_RUN_GPU_REFERENCE_PATH_TRACE=1 for Vulkan manager tests";
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "Optional ray surface fallback", nullptr,
                              nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t count{};
    const auto *extensions = glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions, extensions + count);
    info.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    info.next = &validation;
    instance = std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback =
        reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(
            validationCallback);
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);
    VkSurfaceKHR handle{};
    ASSERT_EQ(
        glfwCreateWindowSurface(instance->instance(), window, nullptr, &handle),
        VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), handle);
    DeviceCreateInfo deviceInfo;
    deviceInfo.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    deviceInfo.enableRayQueries = true;
    deviceInfo.enabledFeatures.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.separateDepthStencilLayouts = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.pNext = &v12;
    deviceInfo.next = &v13;
    device = std::make_shared<VulkanDevice>(*instance, handle, deviceInfo);
    if (!device->rayQueriesEnabled())
      GTEST_SKIP() << device->rayQuerySupport().unavailableReason();
    memory = std::make_unique<VulkanMemoryManager>(
        instance->instance(), device->physicalDevice(), device->device());
    pool = vk::raii::CommandPool(
        device->raii(),
        vk::CommandPoolCreateInfo(
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            device->queueFamilyIndices().graphicsFamily.value()));
    auto commands =
        device->raii().allocateCommandBuffers(vk::CommandBufferAllocateInfo(
            *pool, vk::CommandBufferLevel::ePrimary, 1));
    command = std::move(commands.front());

    // The production shadow pipeline requires the complete material and light
    // layouts even though this fixture records only AS builds, not dispatches.
    const std::array sceneBindings{
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eSampler, 9,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(5, vk::DescriptorType::eSampledImage,
                                       4096,
                                       vk::ShaderStageFlagBits::eCompute)};
    sceneLayout = vk::raii::DescriptorSetLayout(
        device->raii(), vk::DescriptorSetLayoutCreateInfo({}, sceneBindings));
    const std::array lightBindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute)};
    lightLayout = vk::raii::DescriptorSetLayout(
        device->raii(), vk::DescriptorSetLayoutCreateInfo({}, lightBindings));
    const std::array descriptorSizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eSampler, 9),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 4096),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 4),
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 1)};
    descriptorPool = vk::raii::DescriptorPool(
        device->raii(),
        vk::DescriptorPoolCreateInfo(
            vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 2,
            descriptorSizes));
    const std::array layouts{*sceneLayout, *lightLayout};
    sets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*descriptorPool, layouts));
    createDepth();
    manager = std::make_unique<RayShadowManager>(device, *memory);
    ASSERT_NO_THROW(manager->createPipelines(CONTAINER_REFERENCE_PATH_SHADER_ROOT,
                                             *sceneLayout, *lightLayout));
    manager->prepare({32, 32}, 2);
    manager->requestSurfaceScene(true);
    vertices.resize(3);
    vertices[0].position = {-1, -1, 0};
    vertices[1].position = {1, -1, 0};
    vertices[2].position = {0, 1, 0};
    for (auto &vertex : vertices) {
      vertex.normal = {0, 0, 1};
      vertex.tangent = {1, 0, 0, 1};
      vertex.color = {1, 1, 1};
    }
    indices = {0, 1, 2};
    objects.resize(1);
    draws = {{0, 0, 3}};
    initializeReference();
  }

  void createDepth() {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT;
    info.extent = {32, 32, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ASSERT_EQ(vmaCreateImage(memory->allocator(), &info, &alloc, &depth,
                             &depthAllocation, nullptr),
              VK_SUCCESS);
    vk::ImageViewCreateInfo view;
    view.image = depth;
    view.viewType = vk::ImageViewType::e2D;
    view.format = vk::Format::eD32Sfloat;
    view.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    depthView = vk::raii::ImageView(device->raii(), view);
    frame.depthStencil = {depth, depthAllocation, *depthView,
                          VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT};
    frame.depthSamplingView = *depthView;
  }

  void TearDown() override {
    if (device)
      device->raii().waitIdle();
    // Command references and descriptor sets retire before their resources.
    command.clear();
    tracer.reset();
    pipelines.reset();
    timingPool.clear();
    manager.reset();
    sets.clear();
    descriptorPool.clear();
    uploads.clear();
    whiteSampler.clear();
    whiteCubeView.clear();
    whiteTextureView.clear();
    mipTextureView.clear();
    if (mipImage) vmaDestroyImage(memory->allocator(), mipImage, mipAllocation);
    if(whiteImage) vmaDestroyImage(memory->allocator(),whiteImage,whiteAllocation);
    sceneLayout.clear();
    lightLayout.clear();
    depthView.clear();
    if (depth)
      vmaDestroyImage(memory->allocator(), depth, depthAllocation);
    pool.clear();
    memory.reset();
    device.reset();
    surface.clear();
    messenger.clear();
    instance.reset();
    if (window)
      glfwDestroyWindow(window);
    if (glfwStarted)
      glfwTerminate();
    EXPECT_EQ(validationErrors.load(), 0u);
  }

  void update(uint32_t image, bool emptyProviders = false) {
    const std::array providers{RaySceneProviderSource{
        {"mesh"}, revision, vertices, indices, objects, draws}};
    manager->update(image, camera, frame, *sets[0], *sets[1],
                    emptyProviders ? std::span<const RaySceneProviderSource>{}
                                   : std::span<const RaySceneProviderSource>(providers),
                    lighting, pointValues, areaValues, 0, {}, {},
                    [](uint32_t) { return uint64_t(1); });
  }

  void build(uint32_t image, bool emptyProviders = false) {
    update(image, emptyProviders);
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    manager->recordBuild(*command, image);
    command.end();
    const vk::CommandBuffer handle = *command;
    vk::SubmitInfo submit;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &handle;
    vk::raii::Queue queue(
        device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submit, nullptr);
    queue.waitIdle();
  }

  void verifyUnloadAndReload(bool surfaces) {
    manager->settings().mode = RayShadowMode::Hard;
    manager->requestSurfaceScene(surfaces);
    ASSERT_NO_THROW(build(0));
    ASSERT_NO_THROW(build(1));
    ASSERT_TRUE(manager->active());
    ASSERT_NE(manager->buildStats(), nullptr);
    ASSERT_EQ(manager->buildStats()->instanceCount, 1u);
    EXPECT_EQ(bool(manager->surfaceBindings(1)), surfaces);
    const auto loadedGeneration = manager->sceneGeneration();
    const auto loadedBytes = manager->allocatedBytes();
    const auto savedDraws = draws;
    draws.clear();

    // Both slots retain the submitted generation. Retiring the first image
    // disables all new consumers immediately, retaining the other image's AS.
    ASSERT_NO_THROW(build(0));
    EXPECT_FALSE(manager->active());
    EXPECT_EQ(manager->sceneRevision(), 0u);
    EXPECT_EQ(manager->buildStats(), nullptr);
    EXPECT_FALSE(manager->surfaceBindings(0));
    EXPECT_FALSE(manager->surfaceBindings(1));
    EXPECT_TRUE(manager->surfaceUnsupportedReason().empty());
    EXPECT_EQ(manager->settings().mode, RayShadowMode::Hard);
    EXPECT_EQ(manager->sceneGeneration(), loadedGeneration);
    EXPECT_EQ(manager->allocatedBytes(), loadedBytes);
    ASSERT_NO_THROW(build(0, true));
    EXPECT_EQ(manager->allocatedBytes(), loadedBytes);

    // Only the last retired image can release the final submitted reference.
    ASSERT_NO_THROW(build(1, true));
    const auto emptyBytes = manager->allocatedBytes();
    EXPECT_LT(emptyBytes, loadedBytes);
    EXPECT_EQ(manager->buildStats(), nullptr);
    EXPECT_EQ(manager->sceneGeneration(), loadedGeneration);
    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
      draws = savedDraws;
      ASSERT_NO_THROW(build(0));
      ASSERT_NO_THROW(build(1));
      EXPECT_TRUE(manager->active());
      EXPECT_NE(manager->sceneRevision(), 0u);
      ASSERT_NE(manager->buildStats(), nullptr);
      EXPECT_EQ(manager->buildStats()->instanceCount, 1u);
      EXPECT_EQ(bool(manager->surfaceBindings(1)), surfaces);
      EXPECT_EQ(manager->sceneGeneration(), loadedGeneration + cycle + 1);
      EXPECT_LE(manager->allocatedBytes(), loadedBytes);
      draws.clear();
      ASSERT_NO_THROW(build(0));
      ASSERT_NO_THROW(build(1, true));
      EXPECT_FALSE(manager->active());
      EXPECT_EQ(manager->sceneRevision(), 0u);
      EXPECT_EQ(manager->buildStats(), nullptr);
      EXPECT_EQ(manager->allocatedBytes(), emptyBytes);
    }
  }

  struct Buffer {
    VulkanMemoryManager& memory;AllocatedBuffer value{};size_t bytes{};
    Buffer(VulkanMemoryManager& m,size_t n,VkBufferUsageFlags usage):memory(m),bytes(n) {
      value=m.createBuffer(n,usage,VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
          VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    }
    ~Buffer() {memory.destroyBuffer(value);}
    void write(const void* data,size_t size) {
      if(size>bytes) throw std::runtime_error("fixture upload exceeds buffer");
      std::memcpy(value.allocation_info.pMappedData,data,size);
      if(vmaFlushAllocation(memory.allocator(),value.allocation,0,size)!=VK_SUCCESS) throw std::runtime_error("fixture flush failed");
    }
  };
  Buffer& upload(size_t bytes,VkBufferUsageFlags usage) {
    uploads.push_back(std::make_unique<Buffer>(*memory,bytes,usage));return *uploads.back();
  }
  void bindBuffer(VkDescriptorSet set,uint32_t binding,const Buffer& buffer,VkDescriptorType type) {
    VkDescriptorBufferInfo info{buffer.value.buffer,0,buffer.bytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=binding;
    w.descriptorCount=1;w.descriptorType=type;w.pBufferInfo=&info;vkUpdateDescriptorSets(device->device(),1,&w,0,nullptr);
  }
  void submit() {
    command.end();const vk::CommandBuffer cb=*command;
    vk::SubmitInfo info;info.commandBufferCount=1;info.pCommandBuffers=&cb;
    vk::raii::Queue q(device->raii(),device->queueFamilyIndices().graphicsFamily.value(),0);
    q.submit(info,nullptr);q.waitIdle();
  }
  void initializeReference() {
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(device->physicalDevice(),&properties);
    timestampPeriod=properties.limits.timestampPeriod;
    uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(),&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(),&count,families.data());
    timestampBits=families[device->queueFamilyIndices().graphicsFamily.value()].timestampValidBits;
    if(timestampBits) timingPool=vk::raii::QueryPool(device->raii(),vk::QueryPoolCreateInfo({},vk::QueryType::eTimestamp,2));
    materialValues.resize(1);materialValues[0].color={.5f,.5f,.5f,1};materialValues[0].specularFactor=0;
    materialsUpload=&upload(sizeof(GpuMaterial)*8,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    metadataUpload=&upload(sizeof(GpuTextureMetadata)*4096,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    lightUpload=&upload(sizeof(LightingData),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    pointUpload=&upload(sizeof(PointLightData)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    areaUpload=&upload(sizeof(AreaLightData)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::vector<GpuTextureMetadata> metadata(4096);for(auto& m:metadata) {m.samplerIndex=0;m.samplerCount=9;}
    metadataUpload->write(metadata.data(),metadata.size()*sizeof(metadata[0]));
    std::memset(pointUpload->value.allocation_info.pMappedData,0,pointUpload->bytes);
    std::memset(areaUpload->value.allocation_info.pMappedData,0,areaUpload->bytes);
    lighting.directionalColorIntensity={};lighting.environmentIntensity=1;
    syncInputs();
    bindBuffer(*sets[0],3,*materialsUpload,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindBuffer(*sets[0],4,*metadataUpload,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindBuffer(*sets[1],0,*lightUpload,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    bindBuffer(*sets[1],1,*pointUpload,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindBuffer(*sets[1],2,*areaUpload,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R32G32B32A32_SFLOAT;info.extent={1,1,1};
    info.mipLevels=1;info.arrayLayers=6;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};alloc.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ASSERT_EQ(vmaCreateImage(memory->allocator(),&info,&alloc,&whiteImage,&whiteAllocation,nullptr),VK_SUCCESS);
    vk::ImageViewCreateInfo view;view.image=whiteImage;view.viewType=vk::ImageViewType::eCube;
    view.format=vk::Format::eR32G32B32A32Sfloat;view.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,6};
    whiteCubeView=vk::raii::ImageView(device->raii(),view);
    view.viewType=vk::ImageViewType::e2D;view.subresourceRange.layerCount=1;whiteTextureView=vk::raii::ImageView(device->raii(),view);
    vk::SamplerCreateInfo sampler;sampler.magFilter=vk::Filter::eNearest;sampler.minFilter=vk::Filter::eNearest;
    sampler.mipmapMode=vk::SamplerMipmapMode::eNearest;
    sampler.maxLod=1.0f;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=vk::SamplerAddressMode::eClampToEdge;
    whiteSampler=vk::raii::Sampler(device->raii(),sampler);
    std::vector<VkDescriptorImageInfo> images(4096,{VK_NULL_HANDLE,*whiteTextureView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    std::vector<VkDescriptorImageInfo> samplers(9,{*whiteSampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED});
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=*sets[0];write.dstBinding=5;
    write.descriptorCount=4096;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=images.data();
    vkUpdateDescriptorSets(device->device(),1,&write,0,nullptr);
    write.dstBinding=2;write.descriptorCount=9;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;write.pImageInfo=samplers.data();
    vkUpdateDescriptorSets(device->device(),1,&write,0,nullptr);
    command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;b.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=whiteImage;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(*command,&dep);
    const VkClearColorValue color{{.5f,.25f,.75f,1}};
    vkCmdClearColorImage(*command,whiteImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&b.subresourceRange);
    b.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;b.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT;
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(*command,&dep);submit();
    pipelines=std::make_unique<PipelineManager>(device->device());
    tracer=std::make_unique<ReferencePathTracePass>(device,*memory,*pipelines);
    tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,{32,32},*sceneLayout,*lightLayout);
    input.data.inverseProjection=glm::inverse(glm::orthoRH_ZO(-1.f,1.f,-1.f,1.f,10.f,.1f));
    input.data.inverseView=glm::translate(glm::mat4(1),glm::vec3(0,0,2));
    input.data.limits={0,0,1,2};input.data.environmentRadiance=glm::vec4(1);
    input.environment=*whiteCubeView;input.environmentSampler=*whiteSampler;
    input.materials=*sets[0];input.lights=*sets[1];input.contentRevision=1;
    input.settings.samplesPerFrame=64;input.settings.sampleLimit=512;input.settings.maxBounces=4;
    build(0);build(1);
  }
  static constexpr uint32_t kMipTextureIndex = 24;

  void bindTransportMipTexture() {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    info.extent = {2, 2, 1};
    info.mipLevels = 2;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ASSERT_EQ(vmaCreateImage(memory->allocator(), &info, &alloc, &mipImage,
                             &mipAllocation, nullptr), VK_SUCCESS);
    vk::ImageViewCreateInfo view;
    view.image = mipImage;
    view.viewType = vk::ImageViewType::e2D;
    view.format = vk::Format::eR32G32B32A32Sfloat;
    view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 2, 0, 1};
    mipTextureView = vk::raii::ImageView(device->raii(), view);

    command.reset();
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = mipImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(*command, &dependency);
    // Packed metalness uses blue, specular transmission uses red, and diffuse
    // transmission uses alpha. The two mips deliberately disagree.
    const std::array colors{VkClearColorValue{{0.f, 0.f, 1.f, .25f}},
                            VkClearColorValue{{1.f, 0.f, 0.f, .75f}}};
    for (uint32_t level = 0; level < 2; ++level) {
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
      vkCmdClearColorImage(*command, mipImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &colors[level], 1, &range);
    }
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(*command, &dependency);
    submit();

    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, *mipTextureView,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = *sets[0];
    write.dstBinding = 5;
    write.dstArrayElement = kMipTextureIndex;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device->device(), 1, &write, 0, nullptr);
  }

  static GpuTextureTransform minifiedTextureTransform() {
    // These fixtures span two world units. At 32 pixels across, tiling the
    // unit UV square 64 times selects the coarse mip even at normal incidence.
    GpuTextureTransform transform;
    transform.row0.x = transform.row1.y = 64.f;
    return transform;
  }

  void syncInputs() {
    materialsUpload->write(materialValues.data(),materialValues.size()*sizeof(GpuMaterial));
    lightUpload->write(&lighting,sizeof(lighting));
    std::array<PointLightData,4> points{};std::copy(pointValues.begin(),pointValues.end(),points.begin());pointUpload->write(points.data(),sizeof(points));
    std::array<AreaLightData,4> areas{};std::copy(areaValues.begin(),areaValues.end(),areas.begin());areaUpload->write(areas.data(),sizeof(areas));
    if(input.environment) ++input.contentRevision;
    input.data.limits.z=uint32_t(materialValues.size());
    input.data.limits.w &= ~16u;
    for (const auto& material:materialValues)
      if (material.thicknessFactor>0 && material.transmissionFactor>0) input.data.limits.w |= 16u;
  }
  struct Pixels {
    uint32_t width{32},height{32};
    std::array<std::vector<glm::vec4>,3> images;
    glm::dvec3 roi(size_t image=0) const {
      glm::dvec3 value{};
      for(uint32_t y=height/2-2;y<height/2+2;++y) for(uint32_t x=width/2-2;x<width/2+2;++x)
        value+=glm::dvec3(images[image][x+width*y]);return value/16.;
    }
  };
  Pixels render(uint32_t image=0,bool capture=true) {
    input.scene=input.emptyScene ? RaySurfaceGpuBindings{} : manager->surfaceBindings(image);
    if (meshLightProposal)
      tracer->prepareFrame(image, input, materialValues, manager->surfaceInstanceMetadata(image));
    else
      tracer->prepareFrame(image, input);
    command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    if(timestampBits) {
      command.resetQueryPool(*timingPool,0,2);
      command.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe,*timingPool,0);
    }
    tracer->record(*command,image);
    if(timestampBits) command.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands,*timingPool,1);
    const size_t pixels=size_t(captureExtent.width)*captureExtent.height;
    std::unique_ptr<Buffer> readback;
    if(capture) {
      readback=std::make_unique<Buffer>(*memory,3*pixels*sizeof(glm::vec4),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      const std::array images{tracer->meanImage(),tracer->momentImage(),tracer->displayImage()};
      std::array<VkImageMemoryBarrier2,3> barriers{};
      for(size_t i=0;i<3;++i) {
        auto& b=barriers[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;b.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=images[i];b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      }
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=3;dep.pImageMemoryBarriers=barriers.data();vkCmdPipelineBarrier2(*command,&dep);
      for(size_t i=0;i<3;++i) {
        VkBufferImageCopy copy{};copy.bufferOffset=i*pixels*sizeof(glm::vec4);copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={captureExtent.width,captureExtent.height,1};
        vkCmdCopyImageToBuffer(*command,images[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback->value.buffer,1,&copy);
      }
      for(auto& b:barriers) {
        b.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;b.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT;
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      }
      VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
      host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
      dep.memoryBarrierCount=1;dep.pMemoryBarriers=&host;vkCmdPipelineBarrier2(*command,&dep);
    }
    submit();tracer->commitFrame(image);
    if(timestampBits) {
      std::array<uint64_t,2> values{};
      if(vkGetQueryPoolResults(device->device(),*timingPool,0,2,sizeof(values),values.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS)
        throw std::runtime_error("reference timestamp readback failed");
      const uint64_t mask=timestampBits==64?UINT64_MAX:(uint64_t(1)<<timestampBits)-1;
      gpuTimes.push_back(double((values[1]-values[0])&mask)*timestampPeriod/1e6);
    }
    Pixels result;result.width=captureExtent.width;result.height=captureExtent.height;
    if(capture) {
      if(vmaInvalidateAllocation(memory->allocator(),readback->value.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("reference readback invalidate failed");
      const auto* data=static_cast<const glm::vec4*>(readback->value.allocation_info.pMappedData);
      for(size_t i=0;i<3;++i) result.images[i].assign(data+i*pixels,data+(i+1)*pixels);
    }
    command.reset();return result;
  }
  Pixels converge() {
    auto result=render();
    while(tracer->attemptedSamples()<input.settings.sampleLimit) result=render();
    return result;
  }
  void addQuad(glm::vec3 center,float halfWidth,float halfHeight,bool front,uint32_t material) {
    const uint32_t firstVertex=uint32_t(vertices.size()),firstIndex=uint32_t(indices.size()),object=uint32_t(objects.size());
    for(auto xy:{glm::vec2(-halfWidth,-halfHeight),glm::vec2(halfWidth,-halfHeight),
                glm::vec2(halfWidth,halfHeight),glm::vec2(-halfWidth,halfHeight)}) {
      container::geometry::Vertex vertex{};vertex.position=center+glm::vec3(xy,0);
      vertex.texCoord = (xy / glm::vec2(halfWidth, halfHeight) + 1.f) * .5f;
      vertex.normal={0,0,front?1.f:-1.f};vertex.tangent={1,0,0,1};vertex.color={1,1,1};vertices.push_back(vertex);
    }
    for(uint32_t corner:front?std::array<uint32_t,6>{0,1,2,0,2,3}:std::array<uint32_t,6>{0,2,1,0,3,2}) indices.push_back(firstVertex+corner);
    objects.emplace_back();objects.back().objectInfo.x=material;draws.push_back({object,firstIndex,6});
    ++revision;
  }
  void save(const Pixels& pixels,const std::string& name) {
    const char* directory=std::getenv("CONTAINER_REFERENCE_PATH_ARTIFACT_DIR");if(!directory) return;
    const std::filesystem::path path(directory);std::filesystem::create_directories(path);
    std::ofstream file(path/(name+".rgba32f"),std::ios::binary);
    const std::array<uint32_t,2> size{pixels.width,pixels.height};file.write(reinterpret_cast<const char*>(size.data()),sizeof(size));
    for(const auto& image:pixels.images) file.write(reinterpret_cast<const char*>(image.data()),image.size()*sizeof(glm::vec4));
    if(!file) throw std::runtime_error("reference artifact write failed");
  }
  void addVolumeBox(glm::vec3 center,glm::vec3 half,uint32_t material) {
    const uint32_t firstIndex=uint32_t(indices.size()),object=uint32_t(objects.size());
    for(int axis=0;axis<3;++axis) for(float sign:{-1.f,1.f}) {
      glm::vec3 normal{},tangent{};normal[axis]=sign;tangent[(axis+1)%3]=1;
      const auto bitangent=glm::cross(normal,tangent);const auto first=uint32_t(vertices.size());
      for(auto xy:{glm::vec2(-1,-1),glm::vec2(1,-1),glm::vec2(1,1),glm::vec2(-1,1)}) {
        container::geometry::Vertex vertex{};
        vertex.position=center+normal*half[axis]+tangent*xy.x*half[(axis+1)%3]+bitangent*xy.y*half[(axis+2)%3];
        vertex.texCoord = (xy + 1.f) * .5f;
        vertex.normal=normal;vertex.tangent=glm::vec4(tangent,1);vertex.color={1,1,1};vertices.push_back(vertex);
      }
      for(uint32_t corner:{0u,1u,2u,0u,2u,3u}) indices.push_back(first+corner);
    }
    objects.emplace_back();objects.back().objectInfo.x=material;draws.push_back({object,firstIndex,36});++revision;
  }
  std::vector<std::unique_ptr<Buffer>> uploads;
  Buffer *materialsUpload{},*metadataUpload{},*lightUpload{},*pointUpload{},*areaUpload{};
  VkImage whiteImage{};VmaAllocation whiteAllocation{};
  vk::raii::ImageView whiteCubeView{nullptr},whiteTextureView{nullptr};vk::raii::Sampler whiteSampler{nullptr};
  VkImage mipImage{};
  VmaAllocation mipAllocation{};
  vk::raii::ImageView mipTextureView{nullptr};
  std::unique_ptr<PipelineManager> pipelines;
  std::unique_ptr<ReferencePathTracePass> tracer;
  ReferencePathTraceInput input;
  bool meshLightProposal{false};
  std::vector<GpuMaterial> materialValues;
  std::vector<PointLightData> pointValues;
  std::vector<AreaLightData> areaValues;
  VkExtent2D captureExtent{32,32};
  vk::raii::QueryPool timingPool{nullptr};uint32_t timestampBits{};double timestampPeriod{};
  std::vector<double> gpuTimes;
  GLFWwindow *window{};
  bool glfwStarted{};
  std::atomic_uint validationErrors{};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device;
  std::unique_ptr<VulkanMemoryManager> memory;
  vk::raii::CommandPool pool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
  vk::raii::DescriptorSetLayout sceneLayout{nullptr}, lightLayout{nullptr};
  vk::raii::DescriptorPool descriptorPool{nullptr};
  std::vector<vk::raii::DescriptorSet> sets;
  VkImage depth{};
  VmaAllocation depthAllocation{};
  vk::raii::ImageView depthView{nullptr};
  FrameResources frame;
  CameraData camera;
  LightingData lighting;
  std::unique_ptr<RayShadowManager> manager;
  std::vector<container::geometry::Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<ObjectData> objects;
  std::vector<DrawCommand> draws;
  uint64_t revision{1};
};


TEST_F(ReferencePathTraceGpu, ActualRayQueriesMissOffscreenGeometryAndAccumulateHdrEnvironment) {
  objects[0].model=glm::translate(glm::mat4(1),glm::vec3(100,0,0));build(0);build(1);
  input.data.limits.w|=4;input.settings.sampleLimit=64;
  const auto pixels=render();
  EXPECT_EQ(tracer->attemptedSamples(),64);EXPECT_EQ(tracer->historyResets(),1);
  for(size_t i=0;i<32*32;++i) {
    EXPECT_EQ(pixels.images[0][i],glm::vec4(.5f,.25f,.75f,64));
    EXPECT_EQ(pixels.images[1][i],glm::vec4(0));
    EXPECT_EQ(pixels.images[2][i],glm::vec4(.5f,.25f,.75f,1));
  }
  const auto diagnostics=tracer->diagnostics(0);
  EXPECT_EQ(diagnostics.acceptedSamples,64*32*32);EXPECT_EQ(diagnostics.unsupportedSamples,0);EXPECT_EQ(diagnostics.rejectedSamples,0);
  save(pixels,"environment");
}
TEST_F(ReferencePathTraceGpu, SharedTriangleMaterialAndEnvironmentMisConvergeToLambertEnergy) {
  const auto pixels=converge();const auto value=pixels.roi();
  for(int c=0;c<3;++c) EXPECT_NEAR(value[c],.5,.012);
  for(int y=14;y<18;++y) for(int x=14;x<18;++x) {
    EXPECT_EQ(pixels.images[0][x+y*32].w,512);EXPECT_EQ(pixels.images[1][x+y*32].w,0);
  }
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);
  std::printf("REFERENCE_LAMBERT samples=512 mean=%.7f,%.7f,%.7f expected=0.5\n",value.x,value.y,value.z);
  save(pixels,"lambert-environment");
}
TEST_F(ReferencePathTraceGpu, TextureDescriptorsAndAuthoredFactorsReachActualClosestHits) {
  materialValues[0].baseColorTextureIndex=23;materialValues[0].color={.8f,.4f,.2f,1};syncInputs();
  const auto pixels=converge();const auto mean=pixels.roi();
  const glm::dvec3 expected(.4,.1,.15);
  for(int c=0;c<3;++c) EXPECT_NEAR(mean[c],expected[c],.012);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);save(pixels,"textured-lambert");
}
TEST_F(ReferencePathTraceGpu, UnlitUsesTexturedBaseAndEmissionWithoutScatteringOrVolume) {
  auto& material = materialValues[0];
  material.flags = kObjectFlagUnlit;
  material.baseColorTextureIndex = 23;
  material.color = {.8f, .4f, .2f, 1};
  material.emissiveColor = {.125f, .25f, .5f};
  material.transmissionFactor = 1.f;
  material.thicknessFactor = 1.f;
  material.clearcoatFactor = 1.f;
  lighting.environmentIntensity = 12.f;
  syncInputs();
  input.settings.sampleLimit = 64;
  input.settings.maxBounces = 4;
  const auto pixels = converge();
  const glm::dvec3 expected(.525, .35, .65);
  for (int channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(pixels.roi()[channel], expected[channel], 1e-6);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);
}
TEST_F(ReferencePathTraceGpu, UnsupportedSolidVolumeIsVisibleWithoutContaminatingRawMoments) {
  materialValues[0].transmissionFactor=1;materialValues[0].thicknessFactor=1;syncInputs();input.settings.sampleLimit=64;
  auto pixels=render();
  for(int y=14;y<18;++y) for(int x=14;x<18;++x) {
    const auto i=x+y*32;
    EXPECT_EQ(pixels.images[0][i],glm::vec4(0));EXPECT_EQ(pixels.images[1][i],glm::vec4(0,0,0,1));
    EXPECT_EQ(pixels.images[2][i],glm::vec4(1,0,1,1));
  }
  EXPECT_GT(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"unsupported-solid-volume");
  materialValues[0].transmissionFactor=0;syncInputs();pixels=render();
  EXPECT_EQ(tracer->historyResets(),2);
  EXPECT_GT(pixels.roi().x,.4);EXPECT_EQ(pixels.images[1][16+16*32].w,0);
}
TEST_F(ReferencePathTraceGpu, InvalidRoughnessProducesNumericDiagnosticRatherThanBlackSamples) {
  materialValues[0].metallicRoughness.y=std::numeric_limits<float>::quiet_NaN();syncInputs();input.settings.sampleLimit=64;
  const auto pixels=render();
  EXPECT_EQ(pixels.images[0][16+16*32],glm::vec4(0));
  EXPECT_EQ(pixels.images[1][16+16*32],glm::vec4(0,0,0,2));
  EXPECT_EQ(pixels.images[2][16+16*32],glm::vec4(1,1,0,1));
  EXPECT_GT(tracer->diagnostics(0).rejectedSamples,0);EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);
}
TEST_F(ReferencePathTraceGpu, DirectionalAndPointLightUnitsMatchIndependentDiffuseFalloff) {
  lighting.environmentIntensity=0;lighting.directionalDirection={0,0,-1,0};lighting.directionalColorIntensity={.2f,.5f,1,3};
  syncInputs();input.settings.sampleLimit=64;input.settings.maxBounces=1;
  auto pixels=render();auto mean=pixels.roi();
  constexpr double pi=3.14159265358979323846;
  for(int c=0;c<3;++c) EXPECT_NEAR(mean[c],.5*lighting.directionalColorIntensity[c]*3/pi,2e-6);
  lighting.directionalColorIntensity={};pointValues.resize(1);lighting.pointLightCount=1;
  pointValues[0].positionRadius={0,0,2,0};pointValues[0].colorIntensity={1,1,1,8};syncInputs();
  pixels=render();mean=pixels.roi();
  // Independent midpoint quadrature across the actual orthographic ROI.
  const auto expected=[](double height) {
    double sum=0;constexpr int subdivisions=32;
    for(int y=0;y<subdivisions;++y) for(int x=0;x<subdivisions;++x) {
      const double px=-.125+.25*(x+.5)/subdivisions,py=-.125+.25*(y+.5)/subdivisions;
      const double square=px*px+py*py+height*height;
      sum+=.5*8*height/(3.14159265358979323846*square*std::sqrt(square));
    }
    return sum/(subdivisions*subdivisions);
  };
  EXPECT_NEAR(mean.x,expected(2),.0003);
  pointValues[0].positionRadius.z=4;syncInputs();const auto far=render().roi();
  EXPECT_NEAR(far.x,expected(4),.0001);EXPECT_NEAR(mean.x/far.x,expected(2)/expected(4),.004);
  EXPECT_EQ(tracer->historyResets(),3);
  std::printf("REFERENCE_POINT near=%.7f expected=%.7f far=%.7f expected=%.7f\n",mean.x,expected(2),far.x,expected(4));
}
TEST_F(ReferencePathTraceGpu, AnisotropyUsesAuthoredTangentRotationAndFilteredStrength) {
  ASSERT_NO_FATAL_FAILURE(bindTransportMipTexture());
  vertices[0].texCoord = {0, 0};
  vertices[1].texCoord = {1, 0};
  vertices[2].texCoord = {.5f, 1};
  ++revision;
  build(0);
  build(1);
  auto& material = materialValues[0];
  material.color = {.8f, .6f, .3f, 1.f};
  material.metallicRoughness = {1.f, .45f};
  constexpr double strength = .85, pi = 3.14159265358979323846;
  material.anisotropyDiffuseFactors.x = float(strength);
  lighting.environmentIntensity = 0;
  lighting.directionalDirection = {-.6f, 0, -.8f, 0};
  lighting.directionalColorIntensity = {1, 1, 1, 1};
  input.settings.sampleLimit = 64;
  input.settings.maxBounces = 1;
  syncInputs();
  const auto tangent = converge().roi();
  material.anisotropyDiffuseFactors.y = float(pi / 2);
  syncInputs();
  const auto bitangent = converge().roi();

  constexpr double alpha = .45 * .45;
  const double ax = alpha + (1 - alpha) * strength * strength;
  const auto expected = [&](double x, double y, int channel) {
    // Normal view with a light at (.6, 0, .8) in the unrotated frame.
    const double halfZ = std::sqrt(.9);
    const double cosine = .8;
    const double q = x * x / (3.6 * ax * ax) + y * y / (3.6 * alpha * alpha) + .9;
    const double D = 1 / (pi * ax * alpha * q * q);
    const double lambdaLight =
        .5 * (std::sqrt(1 + (ax * ax * x * x + alpha * alpha * y * y) / (cosine * cosine)) - 1);
    const double f0 = material.color[channel];
    const double fresnel = f0 + (1 - f0) * std::pow(1 - halfZ, 5);
    return fresnel * D / (4 * (1 + lambdaLight));
  };
  EXPECT_GT(tangent.x, 5 * bitangent.x);
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(tangent[c], expected(.6, 0, c), 4e-6);
    EXPECT_NEAR(bitangent[c], expected(0, .6, c), 4e-6);
  }
  material.anisotropyTextureIndex = kMipTextureIndex;
  material.anisotropyTextureTransform = minifiedTextureTransform();
  syncInputs();
  const auto masked = converge();
  material.anisotropyDiffuseFactors.x = 0;
  syncInputs();
  EXPECT_EQ(converge().images[0], masked.images[0]);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);
}
TEST_F(ReferencePathTraceGpu, DiffuseTransmissionUsesAlphaAndIndependentTintInDirectAndMisPaths) {
  ASSERT_NO_FATAL_FAILURE(bindTransportMipTexture());
  auto& material = materialValues[0];
  material.color = {.65f, .4f, .1f, 1.f};
  material.specularFactor = 0;
  material.anisotropyDiffuseFactors.z = .8f;
  material.diffuseTransmissionColorFactor = {.25f, .8f, .4f, 1.f};
  material.diffuseTransmissionColorTextureIndex = 23;
  material.diffuseTransmissionTextureIndex = kMipTextureIndex;
  lighting.environmentIntensity = 0;
  lighting.directionalDirection = {0, 0, 1, 0};
  lighting.directionalColorIntensity = {1, 1, 1, 1};
  input.settings.sampleLimit = 64;
  input.settings.maxBounces = 1;
  syncInputs();
  const auto back = converge().roi();
  lighting.directionalDirection.z = -1;
  syncInputs();
  const auto front = converge().roi();

  constexpr double pi = 3.14159265358979323846;
  constexpr double factor = .8 * .25;
  const glm::dvec3 environment(.5, .25, .75);
  const auto expectedBack = glm::dvec3(material.diffuseTransmissionColorFactor) * environment * factor;
  const auto expectedFront = glm::dvec3(material.color) * (1 - factor);
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(back[c], expectedBack[c] / pi, 1e-6);
    EXPECT_NEAR(front[c], expectedFront[c] / pi, 1e-6);
  }
  lighting.directionalColorIntensity = {};
  lighting.environmentIntensity = 1;
  input.data.limits.w |= 4u;
  input.settings.sampleLimit = 1024;
  input.settings.maxBounces = 2;
  syncInputs();
  const auto mis = converge().roi();
  for (int c = 0; c < 3; ++c)
    EXPECT_NEAR(mis[c], environment[c] * (expectedBack[c] + expectedFront[c]), .006);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);

  material.thicknessFactor = 1.f;
  syncInputs();
  EXPECT_EQ(render().roi(2), glm::dvec3(1, 0, 1));
  EXPECT_GT(tracer->diagnostics(0).unsupportedSamples, 0);
}
TEST_F(ReferencePathTraceGpu, RealOffAxisBlockerCastsAndRestoresRayTracedShadow) {
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={2,0,2,0};pointValues[0].colorIntensity={1,1,1,32};syncInputs();
  input.settings.sampleLimit=64;input.settings.maxBounces=1;
  const auto lit=render();EXPECT_GT(lit.roi().x,.4);save(lit,"point-unblocked");
  addQuad({1,0,1},.3f,.4f,false,0);build(0);build(1);
  const auto blocked=render();EXPECT_EQ(blocked.roi(),glm::dvec3(0));save(blocked,"point-blocked");
  draws.pop_back();++revision;build(0);build(1);const auto restored=render();
  EXPECT_EQ(restored.images[0],lit.images[0]);EXPECT_EQ(tracer->historyResets(),3);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, RectangleAndDiskEmitterMisMatchAnalyticParallelPlaneEnergy) {
  constexpr double pi=3.14159265358979323846;
  lighting.environmentIntensity=0;lighting.areaLightCount=1;areaValues.resize(1);
  auto& light=areaValues[0];light.positionRange={0,0,2,0};light.directionType={0,0,-1,0};
  light.tangentHalfSize={1,0,0,1};light.bitangentHalfSize={0,1,0,1};light.colorIntensity={1,1,1,2};syncInputs();
  input.settings.sampleLimit=512;
  const auto rectangle=converge();const auto rect=rectangle.roi();
  const double a=.5,denominator=std::sqrt(1+a*a);
  const double expected=4/pi*(a/denominator)*std::atan(a/denominator);
  EXPECT_NEAR(rect.x,expected,.012);save(rectangle,"rectangle-mis");
  light.directionType.w=1;syncInputs();const auto disk=converge();
  EXPECT_NEAR(disk.roi().x,.2,.012);save(disk,"disk-mis");
  input.settings.maxBounces=1;++input.contentRevision;const auto directOnly=converge();
  EXPECT_NEAR(directOnly.roi().x,.2,.012);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  std::printf("REFERENCE_AREA rectangle=%.7f expected=%.7f disk=%.7f expected=0.2 directOnly=%.7f\n",rect.x,expected,disk.roi().x,directOnly.roi().x);
}
TEST_F(ReferencePathTraceGpu, BsdfPathsReachEmissiveGeometryAndRespectBounceLimit) {
  lighting.environmentIntensity=0;materialValues.resize(2);auto& emitter=materialValues[1];
  emitter.color={0,0,0,1};emitter.specularFactor=0;emitter.emissiveColor={1,.25f,.1f};emitter.emissiveStrength=2;
  addQuad({0,0,2},1,1,false,1);syncInputs();build(0);build(1);
  input.settings.sampleLimit=1024;input.settings.maxBounces=2;
  const auto indirect=converge();const auto value=indirect.roi();
  constexpr double pi=3.14159265358979323846;
  const double ratio=.5,scale=std::sqrt(1+ratio*ratio);
  const double factor=4/pi*(ratio/scale)*std::atan(ratio/scale);
  EXPECT_NEAR(value.x,factor,.015);EXPECT_NEAR(value.y,factor*.25,.005);EXPECT_NEAR(value.z,factor*.1,.002);
  save(indirect,"emissive-bounce");
  input.settings.maxBounces=1;const auto direct=converge();EXPECT_EQ(direct.roi(),glm::dvec3(0));
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  std::printf("REFERENCE_EMISSIVE samples=1024 mean=%.7f expected=%.7f\n",value.x,factor);
}
TEST_F(ReferencePathTraceGpu, DirectMeshLightAndBsdfMisMatchIndependentParallelPlaneEnergy) {
  meshLightProposal = true;
  lighting.environmentIntensity = 0.f;
  materialValues.emplace_back();
  auto& emitter = materialValues[1];
  emitter.color = {0, 0, 0, 1};
  emitter.specularFactor = 0.f;
  emitter.emissiveColor = {1, .25f, .1f};
  emitter.emissiveStrength = 2.f;
  addQuad({0, 0, 2}, 1, 1, false, 1);
  syncInputs(); build(0); build(1);
  const double ratio = .5, scale = std::sqrt(1 + ratio * ratio);
  const double factor = 4 / 3.14159265358979323846 * (ratio / scale) * std::atan(ratio / scale);
  const glm::dvec3 expected = factor * glm::dvec3(1, .25, .1);
  input.settings.sampleLimit = 1024;
  input.settings.maxBounces = 1;
  auto direct = converge().roi();
  for (int channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(direct[channel], expected[channel], .004);
  input.settings.maxBounces = 2;
  auto combined = converge().roi();
  for (int channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(combined[channel], expected[channel], .006);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);

  emitter.flags = kObjectFlagAlphaBlend;
  emitter.color.a = .25f;
  syncInputs();
  auto blended = converge().roi();
  for (int channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(blended[channel], expected[channel] * .25, .003);

  emitter.flags = 0u;
  emitter.color.a = 1.f;
  objects[1].model = glm::translate(glm::mat4(1), glm::vec3(2.2f, 0, -1.2f));
  ++revision;
  syncInputs(); build(0); build(1);
  input.settings.maxBounces = 1;
  // Independent midpoint integration for a distant parallel mesh. Grazing
  // rays must reach their own sampled triangle through the normal ray offset.
  constexpr int steps = 96;
  constexpr double height = .8;
  double grazingEnergy = 0.;
  for (int y = 0; y < steps; ++y)
    for (int x = 0; x < steps; ++x) {
      const double horizontal = 1.2 + 2. * (x + .5) / steps;
      const double vertical = -1. + 2. * (y + .5) / steps;
      const double distanceSquared = horizontal * horizontal + vertical * vertical + height * height;
      grazingEnergy += height * height / (distanceSquared * distanceSquared);
    }
  grazingEnergy *= 4. / (3.14159265358979323846 * steps * steps);
  EXPECT_NEAR(converge().roi().x, grazingEnergy, .0015);

  emitter.emissiveStrength = 0.f;
  syncInputs();
  EXPECT_EQ(converge().roi(), glm::dvec3(0));
}
TEST_F(ReferencePathTraceGpu, AlphaMaskRejectsCoverageWhileOpaqueIgnoresVertexAndMaterialAlpha) {
  for(auto& vertex:vertices) vertex.colorAlpha=.1f;
  ++revision;build(0);build(1);
  materialValues[0].color.a=1;materialValues[0].flags=1;materialValues[0].alphaCutoff=.5f;syncInputs();
  input.settings.sampleLimit=64;auto masked=render();
  EXPECT_EQ(masked.roi(),glm::dvec3(1));
  materialValues[0].color.a=.1f;materialValues[0].flags=0;syncInputs();auto opaque=converge();
  EXPECT_NEAR(opaque.roi().x,.5,.025);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  for(auto& vertex:vertices) vertex.colorAlpha=1;
  ++revision;build(0);build(1);materialValues[0].flags=1;syncInputs();
  EXPECT_EQ(render().roi(),glm::dvec3(1));
}
TEST_F(ReferencePathTraceGpu, SampleBatchSizeAndImageSlotChangesPreserveExactSequenceAndStop) {
  input.settings.sampleLimit=37;input.settings.samplesPerFrame=16;
  render(0,false);EXPECT_EQ(tracer->attemptedSamples(),16);
  input.settings.samplesPerFrame=32;render(1,false);EXPECT_EQ(tracer->attemptedSamples(),37);
  const auto batched=render(0);EXPECT_EQ(tracer->attemptedSamples(),37);EXPECT_EQ(tracer->historyResets(),1);
  tracer.reset();tracer=std::make_unique<ReferencePathTracePass>(device,*memory,*pipelines);
  tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,{32,32},*sceneLayout,*lightLayout);
  input.settings.samplesPerFrame=1;
  const auto singles=converge();EXPECT_EQ(tracer->attemptedSamples(),37);
  EXPECT_EQ(singles.images[0],batched.images[0]);EXPECT_EQ(singles.images[1],batched.images[1]);
  input.settings.sampleLimit=40;render(1);EXPECT_EQ(tracer->attemptedSamples(),38);EXPECT_EQ(tracer->historyResets(),1);
  input.settings.sampleLimit=8;const auto smaller=converge();EXPECT_EQ(tracer->attemptedSamples(),8);EXPECT_EQ(tracer->historyResets(),2);
  EXPECT_EQ(smaller.images[0][16+16*32].w,8);
}
TEST_F(ReferencePathTraceGpu, AlphaBlendEndpointsPreserveOpaqueAndEmptyRadiance) {
  input.settings.sampleLimit=128;
  const auto opaque=converge();
  materialValues[0].flags=2;materialValues[0].color.a=1;syncInputs();
  const auto full=converge();EXPECT_EQ(full.images[0],opaque.images[0]);
  materialValues[0].color.a=0;syncInputs();const auto empty=converge();
  for(const auto& pixel:empty.images[0]) EXPECT_EQ(pixel,glm::vec4(1,1,1,128));
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, LayeredAlphaCoverageMatchesIndependentEmissionCompositing) {
  materialValues.resize(2);
  for(auto& material:materialValues) {material=GpuMaterial{};material.color={0,0,0,.5f};material.specularFactor=0;material.flags=2;}
  materialValues[0].color.a=.25f;materialValues[0].emissiveColor={4,0,0};
  materialValues[1].emissiveColor={0,2,0};
  addQuad({0,0,-.5f},1,1,true,1);syncInputs();build(0);build(1);
  input.data.environmentRadiance={0,0,1,1};input.settings.sampleLimit=4096;input.settings.maxBounces=2;
  const auto pixels=converge();const auto mean=pixels.roi();
  EXPECT_NEAR(mean.x,1,.025);EXPECT_NEAR(mean.y,.75,.025);EXPECT_NEAR(mean.z,.375,.015);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  for(const auto& pixel:pixels.images[0]) EXPECT_EQ(pixel.w,4096);
  save(pixels,"alpha-layered-emission");
  std::printf("REFERENCE_ALPHA_LAYERED mean=%.6f,%.6f,%.6f expected=1,.75,.375\n",mean.x,mean.y,mean.z);
}
TEST_F(ReferencePathTraceGpu, AlphaCoverageAttenuatesPointShadowVisibilityAndRestoresExactly) {
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={2,0,2,0};pointValues[0].colorIntensity={1,1,1,8};syncInputs();
  input.settings.sampleLimit=2048;input.settings.maxBounces=1;const auto clear=converge();
  materialValues.push_back(GpuMaterial{});auto& blocker=materialValues[1];
  blocker.color={0,0,0,.5f};blocker.specularFactor=0;blocker.flags=2|4;
  addQuad({1,0,1},.4f,.4f,true,1);syncInputs();build(0);build(1);
  const auto half=converge();EXPECT_NEAR(half.roi().x,clear.roi().x*.5,clear.roi().x*.025);
  blocker.color.a=1;syncInputs();EXPECT_EQ(converge().roi(),glm::dvec3(0));
  blocker.color.a=0;syncInputs();const auto restored=converge();EXPECT_EQ(restored.images[0],clear.images[0]);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, DoubleSidedAlphaMaskUsesCoverageAndOrientedBackfaceShading) {
  objects[0].model=glm::rotate(glm::mat4(1),3.14159265359f,glm::vec3(0,1,0));
  const auto normal=glm::transpose(glm::inverse(glm::mat3(objects[0].model)));
  objects[0].normalMatrix0=glm::vec4(normal[0],0);objects[0].normalMatrix1=glm::vec4(normal[1],0);objects[0].normalMatrix2=glm::vec4(normal[2],0);
  for(auto& vertex:vertices) vertex.colorAlpha=.75f;
  materialValues[0].flags=1|4;materialValues[0].alphaCutoff=.5f;syncInputs();++revision;build(0);build(1);
  input.settings.sampleLimit=512;EXPECT_NEAR(converge().roi().x,.5,.015);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);
  materialValues[0].alphaCutoff=.8f;syncInputs();EXPECT_EQ(converge().roi(),glm::dvec3(1));
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, AlphaCoverageUsesVertexFactorAndAuthoredOpacityTextureChannel) {
  lighting.environmentIntensity=0;
  auto& material=materialValues[0];material.color={0,0,0,.8f};material.specularFactor=0;
  material.flags=2;material.opacityFactor=.5f;material.opacityTextureIndex=0;
  material.emissiveColor={4,2,1};for(auto& vertex:vertices) vertex.colorAlpha=.5f;
  syncInputs();++revision;build(0);build(1);input.settings.sampleLimit=2048;input.settings.maxBounces=1;
  const auto red=converge().roi();
  EXPECT_NEAR(red.x,.4,.025);EXPECT_NEAR(red.y,.2,.015);EXPECT_NEAR(red.z,.1,.01);
  material.opacityTextureTransform.row1.w=2;syncInputs();const auto blue=converge().roi();
  EXPECT_NEAR(blue.x,.6,.03);EXPECT_NEAR(blue.y,.3,.02);EXPECT_NEAR(blue.z,.15,.01);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, AlphaSampleDecisionsAreIndependentOfBatchingAndRetiredImageSlot) {
  materialValues[0].flags=2;materialValues[0].color.a=.37f;syncInputs();
  input.settings.sampleLimit=97;input.settings.samplesPerFrame=16;const auto batched=converge();
  tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,{32,32},*sceneLayout,*lightLayout);
  input.settings.samplesPerFrame=1;auto single=render(1);uint32_t image=0;
  while(tracer->attemptedSamples()<97) {single=render(image);image^=1;}
  EXPECT_EQ(single.images[0],batched.images[0]);EXPECT_EQ(single.images[1],batched.images[1]);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, AbandonedRecordingCannotCommitSamplesOrImageLayouts) {
  input.scene=manager->surfaceBindings(0);tracer->prepareFrame(0,input);
  command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  tracer->record(*command,0);command.end();command.reset();
  EXPECT_EQ(tracer->attemptedSamples(),0);EXPECT_EQ(tracer->historyResets(),0);
  tracer->commitFrame(1);EXPECT_EQ(tracer->attemptedSamples(),0);
  auto pixels=render(1);EXPECT_EQ(tracer->attemptedSamples(),64);EXPECT_EQ(tracer->historyResets(),1);
  for(const auto& pixel:pixels.images[0]) EXPECT_EQ(pixel.w,64);
}
TEST_F(ReferencePathTraceGpu, SmoothTintedSheetsTransmitExactlyWithoutConsumingScatteringBounces) {
  auto& first=materialValues[0];first.color={.8f,.4f,.2f,1};first.transmissionFactor=1;first.metallicRoughness.y=0;
  materialValues.push_back(first);materialValues[1].color={.5f,.75f,1,1};
  addQuad({0,0,-.5f},1,1,true,1);syncInputs();build(0);build(1);
  input.settings.maxBounces=1;input.settings.sampleLimit=64;input.settings.rouletteDepth=0;
  const auto pixels=converge();const glm::vec3 expected(.4f,.3f,.2f);
  for(int y=14;y<18;++y) for(int x=14;x<18;++x) {
    const auto i=x+y*32;for(int c=0;c<3;++c) EXPECT_NEAR(pixels.images[0][i][c],expected[c],2e-7);
    EXPECT_EQ(pixels.images[0][i].w,64);EXPECT_EQ(pixels.images[1][i],glm::vec4(0));
  }
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"thin-two-tinted-sheets");
}
TEST_F(ReferencePathTraceGpu, MultipleSmoothSheetsTintPointShadowsAndAlphaZeroRestoresExactly) {
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={2,0,2,0};pointValues[0].colorIntensity={1,1,1,8};syncInputs();
  input.settings.sampleLimit=128;input.settings.maxBounces=1;const auto clear=converge();
  materialValues.push_back(GpuMaterial{});materialValues.push_back(GpuMaterial{});
  materialValues[1].color={.5f,.25f,1,1};materialValues[2].color={.8f,.5f,.25f,1};
  for(size_t i=1;i<3;++i) {auto& m=materialValues[i];m.specularFactor=0;m.transmissionFactor=1;m.metallicRoughness.y=0;m.flags=4;}
  addQuad({1,0,1},.3f,.3f,true,1);addQuad({.5f,0,.5f},.2f,.2f,true,2);syncInputs();build(0);build(1);
  const auto tinted=converge();const glm::dvec3 tint(.4,.125,.25);
  for(int c=0;c<3;++c) EXPECT_NEAR(tinted.roi()[c],clear.roi()[c]*tint[c],2e-7);
  for(size_t i=1;i<3;++i) {materialValues[i].flags=2|4;materialValues[i].color.a=0;}
  syncInputs();EXPECT_EQ(converge().images[0],clear.images[0]);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(tinted,"thin-point-shadow-tint");
}
TEST_F(ReferencePathTraceGpu, RoughThinSurfaceReceivesPunctualLightFromOppositeHemisphere) {
  auto& material=materialValues[0];material.color={1,1,1,1};material.transmissionFactor=1;material.metallicRoughness.y=1;
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={0,0,-2,0};pointValues[0].colorIntensity={1,1,1,4};syncInputs();
  input.settings.sampleLimit=512;input.settings.maxBounces=1;const auto pixels=converge();
  double expected=0;constexpr int steps=128;
  for(int y=0;y<steps;++y) for(int x=0;x<steps;++x) {
    const double px=(x+.5)/steps*.25-.125,py=(y+.5)/steps*.25-.125,square=4+px*px+py*py,cosine=2/std::sqrt(square);
    expected+=(4/square)*cosine/(2*3.14159265358979323846*(1+cosine));
  }
  expected/=steps*steps;
  for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected,.00002);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  std::printf("REFERENCE_THIN_POINT mean=%.9f expected=%.9f\n",pixels.roi().x,expected);
  save(pixels,"thin-rough-point");
}
TEST_F(ReferencePathTraceGpu, RoughThinEnvironmentMisMatchesIndependentWhiteFurnace) {
  auto& material=materialValues[0];material.color={1,1,1,1};material.specularFactor=1;
  material.transmissionFactor=1;material.metallicRoughness.y=1;syncInputs();
  input.settings.sampleLimit=8192;input.settings.maxBounces=2;
  const auto pixels=converge();const auto mean=pixels.roi();
  for(int c=0;c<3;++c) EXPECT_NEAR(mean[c],1-std::log(2.),.004);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  std::printf("REFERENCE_THIN_FURNACE mean=%.9f expected=%.9f\n",mean.x,1-std::log(2.));
  save(pixels,"thin-rough-furnace");
}
TEST_F(ReferencePathTraceGpu, ThinTransmissionTextureChannelAndBackfaceReachEmissiveGeometry) {
  auto& sheet=materialValues[0];sheet.color={.8f,.4f,.2f,1};sheet.transmissionFactor=.5f;
  sheet.transmissionTextureIndex=23;sheet.metallicRoughness.y=0;sheet.flags=4;
  // Author the sheet facing away from the camera, with matching vertex normals.
  std::swap(indices[1],indices[2]);for(auto& vertex:vertices) vertex.normal={0,0,-1};++revision;
  materialValues.push_back(GpuMaterial{});auto& emitter=materialValues[1];emitter.color={0,0,0,1};emitter.specularFactor=0;emitter.emissiveColor={4,2,1};
  addQuad({0,0,-.5f},1,1,true,1);lighting.environmentIntensity=0;syncInputs();build(0);build(1);
  input.settings.sampleLimit=4096;input.settings.maxBounces=1;
  const auto red=converge();const glm::dvec3 expectedRed(.8,.2,.05);
  for(int c=0;c<3;++c) EXPECT_NEAR(red.roi()[c],expectedRed[c],.015);
  materialValues[0].transmissionTextureTransform.row1.w=2;syncInputs();const auto blue=converge();
  const glm::dvec3 expectedBlue(1.2,.3,.075);
  for(int c=0;c<3;++c) EXPECT_NEAR(blue.roi()[c],expectedBlue[c],.02);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(blue,"thin-texture-backface");
}
TEST_F(ReferencePathTraceGpu, AlphaCoverageAndOpticalTransmissionRemainDistinctAtSameSurface) {
  auto& sheet=materialValues[0];sheet.color={.5f,.25f,.125f,.5f};sheet.transmissionFactor=1;sheet.metallicRoughness.y=0;sheet.flags=2;
  materialValues.push_back(GpuMaterial{});auto& emitter=materialValues[1];emitter.color={0,0,0,1};emitter.specularFactor=0;emitter.emissiveColor={4,2,1};
  addQuad({0,0,-.5f},1,1,true,1);lighting.environmentIntensity=0;syncInputs();build(0);build(1);
  input.settings.sampleLimit=4096;input.settings.maxBounces=1;const auto pixels=converge();
  const glm::dvec3 expected(3,1.25,.5625);
  for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],.025);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, MetallicTransmissionIsInactiveAndPreservesOriginalOpaqueResult) {
  auto& material=materialValues[0];material.color={.8f,.4f,.2f,1};material.specularFactor=1;material.metallicRoughness={1,.6f};syncInputs();
  input.settings.sampleLimit=128;const auto opaque=converge();
  material.transmissionFactor=1;material.thicknessFactor=10;material.dispersion=1;syncInputs();const auto metal=converge();
  EXPECT_EQ(metal.images[0],opaque.images[0]);EXPECT_EQ(metal.images[1],opaque.images[1]);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, ThinPaneChoicesAndMomentsAreExactAcrossBatchingAndInputSlots) {
  auto& sheet=materialValues[0];sheet.color={.8f,.4f,.2f,1};sheet.transmissionFactor=1;sheet.metallicRoughness.y=0;sheet.specularFactor=1;sheet.flags=4;
  addQuad({0,0,-.5f},1,1,true,0);syncInputs();build(0);build(1);
  input.settings.sampleLimit=97;input.settings.samplesPerFrame=16;const auto batched=converge();
  tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,{32,32},*sceneLayout,*lightLayout);
  input.settings.samplesPerFrame=1;auto single=render(1);uint32_t image=0;
  while(tracer->attemptedSamples()<97) {single=render(image);image^=1;}
  EXPECT_EQ(single.images[0],batched.images[0]);EXPECT_EQ(single.images[1],batched.images[1]);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, NullSheetLimitReportsUnsupportedInsteadOfDroppingRadiance) {
  auto& sheet=materialValues[0];sheet.color={1,1,1,1};sheet.transmissionFactor=1;sheet.metallicRoughness.y=0;
  for(int i=0;i<64;++i) addQuad({0,0,-.01f*(i+1)},1,1,true,0);
  syncInputs();build(0);build(1);input.settings.sampleLimit=16;input.settings.maxBounces=1;
  auto pixels=converge();EXPECT_EQ(pixels.images[0][16+16*32],glm::vec4(0));
  EXPECT_EQ(pixels.images[1][16+16*32].w,1);EXPECT_GT(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  draws.pop_back();++revision;build(0);build(1);pixels=converge();
  EXPECT_EQ(pixels.images[0][16+16*32],glm::vec4(1,1,1,16));EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);
}
TEST_F(ReferencePathTraceGpu, AreaLightMisThroughTwoReflectivePanesMatchesIndependentQuadrature) {
  input.data.inverseView=glm::translate(glm::mat4(1),glm::vec3(0,0,.2f));
  lighting.environmentIntensity=0;lighting.areaLightCount=1;areaValues.resize(1);
  auto& light=areaValues[0];light.positionRange={0,0,2,0};light.directionType={0,0,-1,0};
  light.tangentHalfSize={1,0,0,1};light.bitangentHalfSize={0,1,0,1};light.colorIntensity={1,1,1,2};
  materialValues.push_back(GpuMaterial{});auto& sheet=materialValues[1];sheet.color={1,1,1,1};
  sheet.specularFactor=1;sheet.transmissionFactor=1;sheet.metallicRoughness.y=0;sheet.ior=3;sheet.flags=4;
  addQuad({0,0,.5f},1,1,true,1);addQuad({0,0,1},1,1,true,1);syncInputs();build(0);build(1);
  input.settings.sampleLimit=8192;input.settings.maxBounces=2;const auto mis=converge();
  input.settings.maxBounces=1;const auto nee=converge();
  double expected=0;constexpr int steps=128;constexpr double pi=3.14159265358979323846;
  for(int py=14;py<18;++py) for(int px=14;px<18;++px) {
    const double x=(px+.5)/16-1,y=1-(py+.5)/16;
    for(int ly=0;ly<steps;++ly) for(int lx=0;lx<steps;++lx) {
      const double dx=2*(lx+.5)/steps-1-x,dy=2*(ly+.5)/steps-1-y;
      const double square=dx*dx+dy*dy+4,cosine=2/std::sqrt(square);
      const double F=.25+.75*std::pow(1-cosine,5);
      expected+=(.5/pi)*2*cosine*cosine/square*(1-F)*(1-F)*4;
    }
  }
  expected/=16*steps*steps;
  EXPECT_NEAR(mis.roi().x,expected,.0008);EXPECT_NEAR(nee.roi().x,expected,.0008);
  EXPECT_NEAR(mis.roi().x,nee.roi().x,.0008);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  std::printf("REFERENCE_THIN_PANE_MIS mean=%.9f nee=%.9f expected=%.9f\n",mis.roi().x,nee.roi().x,expected);
  save(mis,"thin-two-panes-area-mis");
}
TEST_F(ReferencePathTraceGpu, AreaLightMisAndVisibilityUseTheSameMinifiedSheetTransmission) {
  ASSERT_NO_FATAL_FAILURE(bindTransportMipTexture());
  input.data.inverseView = glm::translate(glm::mat4(1), glm::vec3(0, 0, .2f));
  lighting.environmentIntensity = 0;
  lighting.areaLightCount = 1;
  auto& light = areaValues.emplace_back();
  light.positionRange = {0, 0, 2, 0};
  light.directionType = {0, 0, -1, 0};
  light.tangentHalfSize = {1, 0, 0, 1};
  light.bitangentHalfSize = {0, 1, 0, 1};
  light.colorIntensity = {1, 1, 1, 2};
  auto& sheet = materialValues.emplace_back();
  sheet.specularFactor = 0;
  sheet.transmissionFactor = 1;
  sheet.ior = 1;
  sheet.metallicRoughness.y = 0;
  sheet.flags = 4;
  addQuad({0, 0, .5f}, 1, 1, true, 1);
  syncInputs();
  build(0);
  build(1);

  input.settings.sampleLimit = 4096;
  input.settings.maxBounces = 1;
  const auto untextured = converge().roi();
  ASSERT_GT(untextured.x, .1);

  // Only mip 1 transmits. A mip-zero connection would block the area light.
  sheet.transmissionTextureIndex = kMipTextureIndex;
  sheet.transmissionTextureTransform = minifiedTextureTransform();
  syncInputs();
  const auto direct = converge().roi();
  input.settings.maxBounces = 2;
  const auto mis = converge().roi();
  for (int channel = 0; channel < 3; ++channel) {
    EXPECT_NEAR(direct[channel], untextured[channel], 1e-6);
    EXPECT_NEAR(mis[channel], untextured[channel], .002);
  }
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);
}
TEST_F(ReferencePathTraceGpu, ClosedIndexMatchedVolumeUsesTracedWorldThicknessAndBothFaces) {
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=123;glass.thicknessTextureIndex=0;glass.ior=1;
  glass.metallicRoughness.y=1;glass.attenuationColor={.5f,.25f,1,1};glass.attenuationDistance=1;
  addVolumeBox({0,0,1},{1,1,.5f},1);syncInputs();build(0);build(1);
  input.settings.sampleLimit=64;input.settings.maxBounces=1;const auto pixels=converge();
  const glm::dvec3 expected(2,.5,1);
  for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],2e-6);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"volume-world-absorption");
}
TEST_F(ReferencePathTraceGpu, ExperimentalMaterialProfilesDiagnoseRefractiveVolumes) {
  draws.clear();
  auto& glass = materialValues[0];
  glass.color = {1, 1, 1, 1};
  glass.metallicRoughness = {0, 0};
  glass.transmissionFactor = 1;
  glass.thicknessFactor = 1;
  glass.ior = 1;
  glass.specularFactor = 0;
  addVolumeBox({0, 0, 1}, {1, 1, .5f}, 0);
  input.settings.sampleLimit = 64;
  syncInputs();
  build(0);
  build(1);
  EXPECT_GT(converge().roi().x, 0.0);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0);

  glass.experimentalFactors.y = .75f;
  syncInputs();
  auto pixels = render();
  EXPECT_EQ(pixels.roi(), glm::dvec3(0));
  EXPECT_EQ(pixels.roi(2), glm::dvec3(1, 0, 1));
  EXPECT_GT(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);

  glass.experimentalFactors = {.75f, 0, 0, 0};
  syncInputs();
  pixels = render();
  EXPECT_EQ(pixels.roi(2), glm::dvec3(1, 0, 1));
  EXPECT_GT(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);
}
TEST_F(ReferencePathTraceGpu, MinifiedMetallicVolumeDoesNotCreateABoundaryAtAnOpaqueTexel) {
  ASSERT_NO_FATAL_FAILURE(bindTransportMipTexture());
  draws.clear();
  auto& glass = materialValues.emplace_back();
  glass.color = {.25f, .5f, .75f, 1};
  glass.metallicRoughness = {1, 0};
  glass.metallicRoughnessTextureIndex = kMipTextureIndex;
  glass.metallicRoughnessTextureTransform = minifiedTextureTransform();
  glass.transmissionFactor = 1;
  glass.thicknessFactor = 1;
  glass.ior = 1;
  addVolumeBox({0, 0, 1}, {1, 1, .5f}, 1);
  syncInputs();
  build(0);
  build(1);

  // Only mip 0 is fully metallic. Traversal culls the single-sided back face
  // as opaque, so shading must also reflect instead of entering the medium.
  input.settings.sampleLimit = 32;
  input.settings.maxBounces = 2;
  const auto pixels = converge();
  const auto reflected = pixels.roi();
  for (int channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(reflected[channel], glass.color[channel], 1e-6);
  EXPECT_EQ(pixels.images[0][16 + 16 * 32].w, 32.f);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);
}
TEST_F(ReferencePathTraceGpu, CameraInsideVolumeAttenuatesOpaqueHitsBeforeItsExit) {
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1.5f;
  glass.attenuationColor={.5f,.25f,1,1};glass.attenuationDistance=1;
  addVolumeBox({0,0,2},{1,1,3},1);syncInputs();build(0);build(1);
  input.settings.sampleLimit=64;input.settings.maxBounces=1;const auto pixels=converge();
  const glm::dvec3 expected(4*std::pow(.5,1.9),2*std::pow(.25,1.9),1);
  for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],2e-6);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"volume-camera-inside");
  glass.experimentalFactors.y=.5f;
  syncInputs();
  EXPECT_EQ(render().roi(2),glm::dvec3(1,0,1));
  EXPECT_GT(tracer->diagnostics(0).unsupportedSamples,0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, NestedVolumesRestoreOuterAbsorptionAcrossDifferentMaterials) {
  for(auto& vertex:vertices) vertex.position.z=-1;++revision;
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});materialValues.push_back(GpuMaterial{});
  for(size_t i=1;i<3;++i) {auto& m=materialValues[i];m.color={1,1,1,1};m.transmissionFactor=1;m.thicknessFactor=1;m.ior=1;m.attenuationDistance=1;}
  materialValues[1].attenuationColor={.5f,1,1,1};materialValues[2].attenuationColor={1,.5f,1,1};
  addVolumeBox({0,0,.5f},{2,2,1},1);addVolumeBox({0,0,.5f},{1,1,.5f},2);syncInputs();build(0);build(1);
  input.settings.sampleLimit=64;input.settings.maxBounces=1;const auto pixels=converge();
  const glm::dvec3 expected(2,1,1);for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],3e-6);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"volume-nested-absorption");
}
TEST_F(ReferencePathTraceGpu, NestedRefractiveIorFactorsCancelAfterEntryAndExit) {
  for(auto& vertex:vertices) vertex.position.z=-1;++revision;
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});materialValues.push_back(GpuMaterial{});
  for(size_t i=1;i<3;++i) {auto& m=materialValues[i];m.color={1,1,1,1};m.transmissionFactor=1;m.thicknessFactor=1;m.specularFactor=0;m.metallicRoughness.y=0;}
  materialValues[1].ior=1.5f;materialValues[2].ior=2.4f;
  addVolumeBox({0,0,.5f},{2,2,1},1);addVolumeBox({0,0,.5f},{1,1,.5f},2);syncInputs();build(0);build(1);
  input.settings.sampleLimit=64;input.settings.maxBounces=8;input.settings.rouletteDepth=32;const auto pixels=converge();
  const glm::dvec3 expected(4,2,1);for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],3e-6);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"volume-nested-ior");
}
TEST_F(ReferencePathTraceGpu, SolidRefractionReachesIndependentSnellPredictedInteriorEmitter) {
  vertices.clear();indices.clear();objects.clear();draws.clear();++revision;
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1.5f;glass.specularFactor=0;glass.metallicRoughness.y=0;
  addVolumeBox({0,0,0},{5,5,1},1);
  const float refractedX=-.25f+.4f/std::sqrt(1-.4f*.4f);
  addQuad({refractedX,0,0},.18f,1,true,0);
  input.data.inverseView=glm::translate(glm::mat4(1),glm::vec3(-1,0,2))*glm::rotate(glm::mat4(1),-std::asin(.6f),glm::vec3(0,1,0));
  lighting.environmentIntensity=0;syncInputs();build(0);build(1);
  input.settings.sampleLimit=64;input.settings.maxBounces=3;input.settings.rouletteDepth=32;const auto pixels=converge();
  const glm::dvec3 expected=glm::dvec3(4,2,1)/2.25;for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],.00001);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  materialValues[1].ior=1;syncInputs();EXPECT_LT(converge().roi().x,1);
  save(pixels,"volume-snell-interior-emitter");
}
TEST_F(ReferencePathTraceGpu, DispersiveGlassReachesSeparateSnellPredictedEmitters) {
  vertices.clear(); indices.clear(); objects.clear(); draws.clear();
  materialValues[0].color = {0, 0, 0, 1};
  materialValues[0].emissiveColor = {1, 1, 1};
  materialValues.emplace_back();
  auto& glass = materialValues[1];
  glass.color = {1, 1, 1, 1};
  glass.transmissionFactor = 1.f;
  glass.thicknessFactor = 1.f;
  glass.ior = 1.5f;
  glass.dispersion = 24.f;
  glass.specularFactor = 0.f;
  glass.metallicRoughness.y = 0.f;
  addVolumeBox({0, 0, 0}, {5, 5, 1}, 1);
  const std::array indicesOfRefraction{1.2, 1.5, 1.8};
  for (double ior : indicesOfRefraction) {
    const double refractedSine = .6 / ior;
    const auto emitterX = float(-.25 + refractedSine / std::sqrt(1. - refractedSine * refractedSine));
    addQuad({emitterX, 0, 0}, .029f, 1, true, 0);
  }
  input.data.inverseView = glm::translate(glm::mat4(1), glm::vec3(-1, 0, 2)) *
      glm::rotate(glm::mat4(1), -std::asin(.6f), glm::vec3(0, 1, 0));
  input.data.inverseProjection = glm::inverse(glm::orthoRH_ZO(-.125f, .125f, -.125f, .125f, 10.f, .1f));
  lighting.environmentIntensity = 0.f;
  syncInputs(); build(0); build(1);
  input.settings.sampleLimit = 256;
  input.settings.maxBounces = 3;
  input.settings.rouletteDepth = 32;
  const auto dispersed = converge().roi();
  for (int channel = 0; channel < 3; ++channel) {
    const double ior = indicesOfRefraction[channel];
    EXPECT_NEAR(dispersed[channel], 1. / (ior * ior), .035);
  }
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0);

  glass.dispersion = 0.f;
  syncInputs();
  const auto neutral = converge().roi();
  for (int channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(neutral[channel], 1. / 2.25, 1e-5);
}
TEST_F(ReferencePathTraceGpu, InsideTotalInternalReflectionIgnoresDisabledSpecularScale) {
  vertices.clear();indices.clear();objects.clear();draws.clear();++revision;
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1.5f;glass.specularFactor=0;glass.metallicRoughness.y=0;
  addVolumeBox({0,0,0},{10,10,1},1);addQuad({10.f/3,0,0},.23f,1,false,0);
  input.data.inverseView=glm::translate(glm::mat4(1),glm::vec3(0,0,.5f))*glm::rotate(glm::mat4(1),-std::asin(.8f),glm::vec3(0,1,0));
  lighting.environmentIntensity=0;syncInputs();build(0);build(1);
  input.settings.sampleLimit=64;input.settings.maxBounces=2;input.settings.rouletteDepth=32;const auto pixels=converge();
  const glm::dvec3 expected(4,2,1);for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],.00001);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  materialValues[1].ior=1;syncInputs();EXPECT_EQ(converge().roi(),glm::dvec3(0));
  save(pixels,"volume-tir-interior-emitter");
}
TEST_F(ReferencePathTraceGpu, AbsorbingMediumAttenuatesBothCameraAndFiniteLightSegments) {
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1.5f;glass.attenuationDistance=1;
  glass.attenuationColor={.5f,.8f,1,1};addVolumeBox({0,0,0},{5,5,5},1);
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={0,0,1,0};pointValues[0].colorIntensity={1,1,1,4};syncInputs();build(0);build(1);
  input.settings.sampleLimit=512;input.settings.maxBounces=1;const auto pixels=converge();
  glm::dvec3 expected{};constexpr int steps=128;constexpr double pi=3.14159265358979323846;
  for(int y=0;y<steps;++y) for(int x=0;x<steps;++x) {
    const double px=(x+.5)/steps*.25-.125,py=(y+.5)/steps*.25-.125,r=std::sqrt(1+px*px+py*py);
    for(int c=0;c<3;++c) expected[c]+=(2/pi)/(r*r*r)*std::pow(double(glass.attenuationColor[c]),1.9+r);
  }
  expected/=steps*steps;
  for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],.0002);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  std::printf("REFERENCE_VOLUME_CAMERA_SHADOW mean=%.9f,%.9f,%.9f expected=%.9f,%.9f,%.9f\n",pixels.roi().x,pixels.roi().y,pixels.roi().z,expected.x,expected.y,expected.z);
  save(pixels,"volume-camera-shadow-absorption");
}
TEST_F(ReferencePathTraceGpu, IndexMatchedVolumeOnShadowConnectionUsesItsActualRayInterval) {
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=99;glass.ior=1;glass.attenuationDistance=1;
  glass.attenuationColor={.5f,.8f,1,1};addVolumeBox({1,0,1},{.3f,.4f,.3f},1);
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={2,0,2,0};pointValues[0].colorIntensity={1,1,1,8};syncInputs();build(0);build(1);
  input.settings.sampleLimit=512;input.settings.maxBounces=1;const auto pixels=converge();
  glm::dvec3 expected{};constexpr int steps=128;constexpr double pi=3.14159265358979323846;
  const glm::dvec3 minimum(.7,-.4,.7),maximum(1.3,.4,1.3);
  for(int y=0;y<steps;++y) for(int x=0;x<steps;++x) {
    const glm::dvec3 origin((x+.5)/steps*.25-.125,(y+.5)/steps*.25-.125,0),offset=glm::dvec3(2,0,2)-origin;
    const double distance=glm::length(offset);const auto direction=offset/distance;
    double begin=0,end=distance;
    for(int axis=0;axis<3;++axis) {
      const double a=(minimum[axis]-origin[axis])/direction[axis],b=(maximum[axis]-origin[axis])/direction[axis];
      begin=std::max(begin,std::min(a,b));end=std::min(end,std::max(a,b));
    }
    for(int c=0;c<3;++c) expected[c]+=(4/pi)*direction.z/(distance*distance)*std::pow(double(glass.attenuationColor[c]),std::max(0.,end-begin));
  }
  expected/=steps*steps;
  for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],.0002);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  std::printf("REFERENCE_VOLUME_SHADOW_INTERVAL mean=%.9f,%.9f,%.9f expected=%.9f,%.9f,%.9f\n",pixels.roi().x,pixels.roi().y,pixels.roi().z,expected.x,expected.y,expected.z);
  save(pixels,"volume-shadow-interval");
}
TEST_F(ReferencePathTraceGpu, BlackAbsorptionAndInfiniteDistanceKeepTheirExactPhysicalLimits) {
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1;glass.attenuationColor={0,0,0,1};glass.attenuationDistance=1;
  addVolumeBox({0,0,1},{1,1,.5f},1);syncInputs();build(0);build(1);
  input.settings.sampleLimit=64;input.settings.maxBounces=1;EXPECT_EQ(converge().roi(),glm::dvec3(0));
  glass.attenuationDistance=std::numeric_limits<float>::infinity();syncInputs();EXPECT_EQ(converge().roi(),glm::dvec3(4,2,1));
  glass.attenuationDistance=0;syncInputs();auto pixels=converge();EXPECT_EQ(pixels.images[1][16+16*32].w,2);
  EXPECT_GT(tracer->diagnostics(0).rejectedSamples,0);EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);
}
TEST_F(ReferencePathTraceGpu, MediumStackBoundRejectsNinthNestedVolumeAndRestoresEightExactly) {
  for(auto& vertex:vertices) vertex.position.z=-2;++revision;
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1;
  for(int i=0;i<9;++i) addVolumeBox({0,0,0},glm::vec3(1.5f-.1f*i),1);
  syncInputs();build(0);build(1);input.settings.sampleLimit=16;input.settings.maxBounces=1;
  auto pixels=converge();EXPECT_EQ(pixels.images[1][16+16*32].w,1);EXPECT_GT(tracer->diagnostics(0).unsupportedSamples,0);
  draws.pop_back();++revision;build(0);build(1);pixels=converge();EXPECT_EQ(pixels.roi(),glm::dvec3(4,2,1));
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, OverlappingIncompatibleMediaAreDiagnosedInsteadOfAssumingAir) {
  materialValues.push_back(GpuMaterial{});materialValues.push_back(GpuMaterial{});
  for(size_t i=1;i<3;++i) {auto& m=materialValues[i];m.color={1,1,1,1};m.transmissionFactor=1;m.thicknessFactor=1;m.ior=1;}
  materialValues[1].attenuationColor={.5f,1,1,1};materialValues[2].attenuationColor={1,.5f,1,1};
  addVolumeBox({0,0,1},{1,1,.5f},1);addVolumeBox({0,0,.5f},{1,1,.5f},2);syncInputs();build(0);build(1);
  input.settings.sampleLimit=16;auto pixels=converge();EXPECT_EQ(pixels.images[1][16+16*32].w,1);
  EXPECT_GT(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, BendingDeltaLightCausticIsExplicitlyUnsupported) {
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={1,1,1,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1.5f;glass.metallicRoughness.y=0;
  addVolumeBox({1,0,1},{.3f,.4f,.3f},1);lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={2,0,2,0};pointValues[0].colorIntensity={1,1,1,8};syncInputs();build(0);build(1);
  input.settings.sampleLimit=16;input.settings.maxBounces=1;const auto pixels=converge();
  EXPECT_EQ(pixels.images[1][16+16*32].w,1);EXPECT_GT(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, RoughMixedVolumeKeepsFiniteMomentsAndExactBatchSequence) {
  vertices.clear();indices.clear();objects.clear();draws.clear();++revision;
  auto& glass=materialValues[0];glass.color={.8f,.4f,.2f,1};glass.specularFactor=.8f;glass.metallicRoughness={.25f,.6f};
  glass.transmissionFactor=.7f;glass.transmissionTextureIndex=0;glass.thicknessFactor=1;glass.ior=1.5f;
  glass.attenuationColor={.8f,.9f,1,1};glass.attenuationDistance=2;addVolumeBox({0,0,0},{1,1,1},0);
  syncInputs();build(0);build(1);input.settings.sampleLimit=97;input.settings.maxBounces=16;input.settings.samplesPerFrame=16;
  const auto batched=converge();
  for(const auto& pixel:batched.images[0]) {EXPECT_EQ(pixel.w,97);for(int c=0;c<3;++c) {EXPECT_TRUE(std::isfinite(pixel[c]));EXPECT_GE(pixel[c],0);}}
  EXPECT_GT(batched.roi().x,.01);EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,{32,32},*sceneLayout,*lightLayout);
  input.settings.samplesPerFrame=1;auto single=render(1);uint32_t image=0;
  while(tracer->attemptedSamples()<97) {single=render(image);image^=1;}
  EXPECT_EQ(single.images[0],batched.images[0]);EXPECT_EQ(single.images[1],batched.images[1]);
  save(batched,"volume-rough-mixed");
}
TEST_F(ReferencePathTraceGpu, SmoothUnabsorbingGlassFurnacePreservesWhiteEnvironment) {
  vertices.clear();indices.clear();objects.clear();draws.clear();++revision;
  auto& glass=materialValues[0];glass.color={1,1,1,1};glass.specularFactor=1;glass.metallicRoughness.y=0;
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1.5f;addVolumeBox({0,0,0},{1,1,1},0);
  syncInputs();build(0);build(1);input.settings.sampleLimit=512;input.settings.maxBounces=32;input.settings.rouletteDepth=32;
  const auto pixels=converge();
  for(const auto& pixel:pixels.images[0]) {EXPECT_EQ(pixel.w,512);for(int c=0;c<3;++c) EXPECT_NEAR(pixel[c],1,.000003);}
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"volume-white-furnace");
}
TEST_F(ReferencePathTraceGpu, MetallicFractionKeepsItsColorUnderDielectricTotalInternalReflection) {
  vertices.clear();indices.clear();objects.clear();draws.clear();++revision;
  materialValues[0].color={0,0,0,1};materialValues[0].emissiveColor={4,2,1};
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={.2f,.4f,.8f,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1.5f;glass.specularFactor=0;glass.metallicRoughness={.25f,0};
  addVolumeBox({0,0,0},{10,10,1},1);addQuad({10.f/3,0,0},.23f,1,false,0);
  input.data.inverseView=glm::translate(glm::mat4(1),glm::vec3(0,0,.5f))*glm::rotate(glm::mat4(1),-std::asin(.8f),glm::vec3(0,1,0));
  lighting.environmentIntensity=0;syncInputs();build(0);build(1);
  input.settings.sampleLimit=64;input.settings.maxBounces=2;input.settings.rouletteDepth=32;const auto pixels=converge();
  const auto color=glm::dvec3(.2,.4,.8);const auto expected=glm::dvec3(4,2,1)*(glm::dvec3(.75)+.25*(color+(glm::dvec3(1)-color)*std::pow(.4,5)));
  for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],expected[c],.00001);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"volume-metallic-tir");
}
TEST_F(ReferencePathTraceGpu, IndexMatchedMetallicVolumeDoesNotAttenuateTransmissionTwice) {
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={2,0,2,0};pointValues[0].colorIntensity={1,1,1,8};syncInputs();
  input.settings.sampleLimit=128;input.settings.maxBounces=1;const auto clear=converge();
  materialValues.push_back(GpuMaterial{});auto& glass=materialValues[1];glass.color={.5f,.5f,.5f,1};
  glass.transmissionFactor=1;glass.thicknessFactor=1;glass.ior=1;glass.specularFactor=1;glass.metallicRoughness={.25f,0};
  addVolumeBox({1,0,1},{.3f,.4f,.3f},1);syncInputs();build(0);build(1);const auto pixels=converge();
  for(int c=0;c<3;++c) EXPECT_NEAR(pixels.roi()[c],clear.roi()[c]*.375*.375,.000001);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  save(pixels,"volume-metallic-shadow");
}
TEST_F(ReferencePathTraceGpu, AutofocusUsesStableCenterCoverageAtTheExactSampleStop) {
  materialValues.emplace_back();
  auto& foreground = materialValues[1];
  foreground.flags = kObjectFlagAlphaBlend;
  foreground.color.a = .25f;
  addQuad({0, 0, 1}, .5f, .5f, true, 1);
  syncInputs(); build(0); build(1);
  input.data.inverseProjection = glm::inverse(glm::perspectiveRH_ZO(.7f, 1.f, 10.f, .1f));
  input.data.limits.w &= ~2u;
  input.settings.sampleLimit = 1;
  render(0);
  EXPECT_FALSE(tracer->takeAutofocusResult());

  input.autofocusKey = 11;
  render(1);
  const auto background = tracer->takeAutofocusResult();
  ASSERT_TRUE(background);
  EXPECT_EQ(background->key, 11u);
  EXPECT_NEAR(background->distance, 2.f, 1e-5f);
  EXPECT_EQ(tracer->attemptedSamples(), 1u);
  EXPECT_EQ(tracer->lastSubmittedSamples(), 0u);
  EXPECT_EQ(tracer->historyResets(), 1u);
  EXPECT_FALSE(tracer->takeAutofocusResult());

  foreground.color.a = .5f;
  syncInputs();
  input.autofocusKey = 12;
  render(0);
  const auto front = tracer->takeAutofocusResult();
  ASSERT_TRUE(front);
  EXPECT_EQ(front->key, 12u);
  EXPECT_NEAR(front->distance, 1.f, 1e-5f);
}
TEST_F(ReferencePathTraceGpu, FireflyClampScalesTheEntireSampleAndResetsHistory) {
  auto& material = materialValues[0];
  material.color = {0, 0, 0, 1};
  material.emissiveColor = {6, 3, .75f};
  lighting.environmentIntensity = 0.f;
  syncInputs();
  input.settings.sampleLimit = 4;
  EXPECT_EQ(converge().roi(), glm::dvec3(6, 3, .75));
  EXPECT_EQ(tracer->effectiveFireflyClamp(), 0.f);

  input.settings.fireflyClamp = 1.5f;
  EXPECT_EQ(converge().roi(), glm::dvec3(1.5, .75, .1875));
  EXPECT_EQ(tracer->effectiveFireflyClamp(), 1.5f);
  EXPECT_EQ(tracer->historyResets(), 2u);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0u);

  input.settings.fireflyClamp = 0.f;
  EXPECT_EQ(converge().roi(), glm::dvec3(6, 3, .75));
  EXPECT_EQ(tracer->historyResets(), 3u);
}
TEST_F(ReferencePathTraceGpu, AnalyticGroundUsesTheEmptyScenePipelineAndEffectiveHistoryKeys) {
  input.emptyScene = true;
  input.data.inverseView = glm::translate(glm::mat4(1), glm::vec3(0, 4, 0)) *
      glm::rotate(glm::mat4(1), -glm::half_pi<float>(), glm::vec3(1, 0, 0));
  lighting.environmentIntensity = 0.f;
  lighting.directionalDirection = {0, -1, 0, 0};
  lighting.directionalColorIntensity = {1, 1, 1, 1};
  syncInputs();
  input.settings.sampleLimit = 4;
  EXPECT_EQ(converge().roi(), glm::dvec3(0));
  EXPECT_EQ(tracer->historyResets(), 1u);

  auto& ground = input.settings.ground;
  ground.height = .25f;
  ground.color = {.8f, .2f, .1f};
  EXPECT_EQ(converge().roi(), glm::dvec3(0));
  EXPECT_EQ(tracer->historyResets(), 1u);
  ground.mode = ReferenceGroundMode::Surface;
  const auto colored = converge().roi();
  EXPECT_GT(colored.r, colored.g * 2);
  EXPECT_GT(colored.g, colored.b);
  EXPECT_EQ(tracer->effectiveGround().height, .25f);
  EXPECT_EQ(tracer->historyResets(), 2u);

  ground.mode = ReferenceGroundMode::ShadowMatte;
  const auto unblocked = converge();
  EXPECT_EQ(unblocked.roi().r, 0.0);
  EXPECT_GT(unblocked.roi().g, 0.0);
  EXPECT_EQ(unblocked.roi().b, 1.0);
  EXPECT_EQ(unblocked.roi(2), glm::dvec3(0));
  EXPECT_EQ(tracer->historyResets(), 3u);
  ground.color = {0, 1, 0};
  converge();
  EXPECT_EQ(tracer->historyResets(), 3u);

  ground.height = 5.f;
  EXPECT_EQ(converge().roi(), glm::dvec3(0));
  EXPECT_EQ(tracer->historyResets(), 4u);
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples, 0u);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples, 0u);
}
TEST_F(ReferencePathTraceGpu, CameraClippingAndContentChangesResetOnlySubmittedHistory) {
  input.settings.sampleLimit=64;render();
  input.data.environmentRadiance={.3f,.6f,1,1};const auto changed=render(1);
  EXPECT_EQ(tracer->historyResets(),2);EXPECT_EQ(changed.images[0][0],glm::vec4(.3f,.6f,1,64));
  input.data.inverseView=glm::translate(glm::mat4(1),glm::vec3(100,0,2));const auto moved=render();
  EXPECT_EQ(tracer->historyResets(),3);EXPECT_EQ(moved.images[0][16+16*32],glm::vec4(.3f,.6f,1,64));
  ++input.contentRevision;render();EXPECT_EQ(tracer->historyResets(),4);
  input.data.inverseView=glm::translate(glm::mat4(1),glm::vec3(0,0,2));
  objects[0].model=glm::translate(glm::mat4(1),glm::vec3(0,0,1.95f));build(0);build(1);
  const auto nearClipped=render();EXPECT_EQ(nearClipped.images[0][16+16*32],glm::vec4(.3f,.6f,1,64));
}
TEST_F(ReferencePathTraceGpu, RepeatedShaderPublicationsResetHistoryByRetainedContentIdentity) {
  const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
  const auto root=parent/("container-reference-reload-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_EQ(root.parent_path(),parent);ASSERT_TRUE(std::filesystem::create_directory(root));
  struct Cleanup {
    std::filesystem::path root,parent;
    ~Cleanup() {
      std::error_code error;const auto actual=std::filesystem::weakly_canonical(root,error);
      if(!error && actual==root && actual.parent_path()==parent) std::filesystem::remove_all(actual,error);
    }
  } cleanup{root,parent};
  std::filesystem::create_directory(root/"spv_shaders");
  const auto stage=[&](uint32_t version) {
    for(const char* name:{"reference_pathtrace.comp.spv","reference_background.comp.spv"}) {
      std::ifstream source(std::filesystem::path(CONTAINER_REFERENCE_PATH_SHADER_ROOT)/"spv_shaders"/name,std::ios::binary|std::ios::ate);
      const auto bytes=static_cast<size_t>(source.tellg());
      if(!source || bytes<20 || bytes%4) throw std::runtime_error("Invalid reference shader fixture");
      std::vector<uint32_t> code(bytes/4);source.seekg(0);source.read(reinterpret_cast<char*>(code.data()),bytes);
      // Change only generator-version metadata; executable instructions stay identical.
      code[2]=(code[2]&0xffff0000u)|((code[2]+version)&0xffffu);
      std::ofstream output(root/"spv_shaders"/name,std::ios::binary);
      output.write(reinterpret_cast<const char*>(code.data()),bytes);
      if(!source || !output) throw std::runtime_error("Reference reload fixture I/O failed");
    }
  };
  input.emptyScene=true;input.data.limits.w=2;input.data.environmentRadiance={.3f,.6f,1,1};
  input.settings.samplesPerFrame=4;input.settings.sampleLimit=64;
  render();render(1);ASSERT_EQ(tracer->attemptedSamples(),8);ASSERT_EQ(tracer->historyResets(),1);
  for(uint32_t version:{1u,2u}) {
    stage(version);auto reload=pipelines->prepareComputeReload(root);ASSERT_EQ(reload.size(),2u);
    pipelines->publishComputeReload(reload);
  }
  EXPECT_EQ(tracer->attemptedSamples(),8);EXPECT_EQ(tracer->historyResets(),1);
  const auto reset=render();EXPECT_EQ(tracer->attemptedSamples(),4);EXPECT_EQ(tracer->historyResets(),2);
  EXPECT_EQ(reset.images[0][0],glm::vec4(.3f,.6f,1,4));
  auto unchanged=pipelines->prepareComputeReload(root);EXPECT_EQ(unchanged.size(),0u);
  pipelines->publishComputeReload(unchanged);render(1);
  EXPECT_EQ(tracer->attemptedSamples(),8);EXPECT_EQ(tracer->historyResets(),2);
  stage(3);{auto abandoned=pipelines->prepareComputeReload(root);EXPECT_EQ(abandoned.size(),2u);}
  render();EXPECT_EQ(tracer->attemptedSamples(),12);EXPECT_EQ(tracer->historyResets(),2);
}
TEST_F(ReferencePathTraceGpu, InvalidBindingsAndBudgetsRejectWithoutLosingValidResources) {
  const auto view=tracer->meanView();EXPECT_THROW(tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,{32,32},*sceneLayout,*lightLayout,1),std::invalid_argument);
  EXPECT_EQ(tracer->meanView(),view);
  input.scene=manager->surfaceBindings(0);auto invalid=input;invalid.scene.surfaceTrianglesSize=16;
  EXPECT_THROW(tracer->prepareFrame(0,invalid),std::invalid_argument);
  invalid=input;invalid.settings.samplesPerFrame=65;EXPECT_THROW(tracer->prepareFrame(0,invalid),std::invalid_argument);
  invalid=input;invalid.settings.rayBias=-1;EXPECT_THROW(tracer->prepareFrame(0,invalid),std::invalid_argument);
  EXPECT_EQ(tracer->attemptedSamples(),0);const auto pixels=render();EXPECT_GT(pixels.roi().x,.4);
}
TEST_F(ReferencePathTraceGpu, ResourceRecreationRetiresAllOwnedImageAndInputAllocations) {
  device->raii().waitIdle();command.reset();tracer.reset();
  VmaTotalStatistics before{},after{};vmaCalculateStatistics(memory->allocator(),&before);
  for(int i=0;i<3;++i) {
    tracer=std::make_unique<ReferencePathTracePass>(device,*memory,*pipelines);
    tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,{32,32},*sceneLayout,*lightLayout);
    render();EXPECT_GT(tracer->allocatedBytes(),32*32*48);
    device->raii().waitIdle();command.reset();tracer.reset();
    vmaCalculateStatistics(memory->allocator(),&after);
    EXPECT_EQ(after.total.statistics.allocationCount,before.total.statistics.allocationCount);
    EXPECT_EQ(after.total.statistics.allocationBytes,before.total.statistics.allocationBytes);
  }
}
TEST_F(ReferencePathTraceGpu, CornellSceneProducesFiniteMultibounceHdrWithMeasuredGpuCost) {
  vertices.clear();indices.clear();objects.clear();draws.clear();++revision;
  materialValues.resize(4);
  for(auto& material:materialValues) {material=GpuMaterial{};material.specularFactor=0;}
  materialValues[0].color={.72f,.72f,.72f,1};materialValues[1].color={.65f,.05f,.04f,1};
  materialValues[2].color={.05f,.5f,.12f,1};materialValues[3].color={.82f,.65f,.2f,1};
  materialValues[3].metallicRoughness={1,.18f};materialValues[3].specularFactor=1;
  const auto surface=[&](glm::vec3 center,glm::vec3 u,glm::vec3 v,uint32_t material) {
    const uint32_t first=uint32_t(vertices.size()),index=uint32_t(indices.size()),object=uint32_t(objects.size());
    for(auto signs:{glm::vec2(-1,-1),glm::vec2(1,-1),glm::vec2(1,1),glm::vec2(-1,1)}) {
      container::geometry::Vertex vertex{};vertex.position=center+u*signs.x+v*signs.y;
      vertex.normal=glm::normalize(glm::cross(u,v));vertex.tangent=glm::vec4(glm::normalize(u),1);vertex.color={1,1,1};vertices.push_back(vertex);
    }
    for(uint32_t corner:{0u,1u,2u,0u,2u,3u}) indices.push_back(first+corner);
    objects.emplace_back();objects.back().objectInfo.x=material;draws.push_back({object,index,6});
  };
  surface({0,0,-1},{1,0,0},{0,1,0},0);
  surface({-1,0,0},{0,0,-1},{0,1,0},1);
  surface({1,0,0},{0,0,1},{0,1,0},2);
  surface({0,-1,0},{1,0,0},{0,0,-1},0);
  surface({0,1,0},{1,0,0},{0,0,1},0);
  const auto box=[&](glm::vec3 center,glm::vec3 half,float angle,uint32_t material) {
    const auto rotation=glm::mat3(glm::rotate(glm::mat4(1),angle,glm::vec3(0,1,0)));
    const auto face=[&](glm::vec3 offset,glm::vec3 u,glm::vec3 v){surface(center+rotation*offset,rotation*u,rotation*v,material);};
    face({half.x,0,0},{0,0,-half.z},{0,half.y,0});face({-half.x,0,0},{0,0,half.z},{0,half.y,0});
    face({0,half.y,0},{half.x,0,0},{0,0,-half.z});face({0,-half.y,0},{half.x,0,0},{0,0,half.z});
    face({0,0,half.z},{half.x,0,0},{0,half.y,0});face({0,0,-half.z},{-half.x,0,0},{0,half.y,0});
  };
  box({-.4f,-.4f,-.2f},{.28f,.6f,.28f},.3f,0);
  box({.4f,-.65f,.3f},{.26f,.35f,.26f},-.25f,3);
  lighting.environmentIntensity=0;lighting.areaLightCount=1;areaValues.resize(1);
  areaValues[0].positionRange={0,.98f,0,0};areaValues[0].directionType={0,-1,0,0};
  areaValues[0].tangentHalfSize={1,0,0,.32f};areaValues[0].bitangentHalfSize={0,0,1,.3f};
  areaValues[0].colorIntensity={.95f,.9f,.8f,18};syncInputs();build(0);build(1);
  captureExtent={128,128};command.reset();tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,captureExtent,*sceneLayout,*lightLayout);
  input.data.inverseProjection=glm::inverse(glm::perspectiveRH_ZO(.7853981634f,1.f,100.f,.1f));
  input.data.inverseView=glm::inverse(glm::lookAtRH(glm::vec3(0,0,3),glm::vec3(0,0,-.1f),glm::vec3(0,1,0)));
  input.data.limits.w=0;input.settings.samplesPerFrame=16;input.settings.sampleLimit=512;input.settings.maxBounces=8;
  gpuTimes.clear();const auto pixels=converge();save(pixels,"cornell-128-spp512");
  size_t lit=0;
  for(size_t i=0;i<pixels.images[0].size();++i) {
    ASSERT_EQ(pixels.images[0][i].w,512);ASSERT_EQ(pixels.images[1][i].w,0);
    for(int c=0;c<3;++c) {
      ASSERT_TRUE(std::isfinite(pixels.images[0][i][c]));ASSERT_GE(pixels.images[0][i][c],0);
      ASSERT_TRUE(std::isfinite(pixels.images[1][i][c]));ASSERT_GE(pixels.images[1][i][c],0);
    }
    if(glm::dot(glm::vec3(pixels.images[0][i]),glm::vec3(1))>.03f) ++lit;
  }
  EXPECT_GT(lit,128*128/3);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);
  double median=-1;
  if(timestampBits) {
    ASSERT_EQ(gpuTimes.size(),32);auto warmed=std::vector<double>(gpuTimes.begin()+1,gpuTimes.end());
    std::sort(warmed.begin(),warmed.end());median=warmed[warmed.size()/2];EXPECT_GT(median,0);
  }
  std::printf("REFERENCE_CORNELL extent=128x128 samples=512 bounces=8 batch=16 warmed_gpu_ms=%.5f owned_allocation_bytes=%llu lit_pixels=%zu\n",
      median,static_cast<unsigned long long>(tracer->allocatedBytes()),lit);
}
TEST_F(ReferencePathTraceGpu, SpotConeAndFiniteRangeAgreeWithAuthoredPointLightRules) {
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  auto& light=pointValues[0];light.positionRadius={0,0,2,0};light.colorIntensity={1,1,1,8};
  light.directionInnerCos={0,0,-1,.95f};light.coneOuterCosType={.8f,1,0,0};syncInputs();
  input.settings.sampleLimit=64;input.settings.maxBounces=1;
  const auto aligned=render();EXPECT_NEAR(aligned.roi().x,.3170733,.0003);
  light.directionInnerCos={1,0,0,.95f};syncInputs();EXPECT_EQ(render().roi(),glm::dvec3(0));
  light.directionInnerCos={0,0,-1,.95f};light.positionRadius.w=1;syncInputs();EXPECT_EQ(render().roi(),glm::dvec3(0));
  light.positionRadius.w=4;syncInputs();const auto finiteRange=render();EXPECT_NEAR(finiteRange.roi().x,.178,.002);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, AnalyticAreaEmitterIsVisibleToCameraWithExactHdrRadiance) {
  objects[0].model=glm::translate(glm::mat4(1),glm::vec3(100,0,0));build(0);build(1);
  lighting.environmentIntensity=0;lighting.areaLightCount=1;areaValues.resize(1);
  auto& light=areaValues[0];light.positionRange={0,0,0,0};light.directionType={0,0,1,0};
  light.colorIntensity={.25f,.5f,1,4};syncInputs();input.settings.sampleLimit=64;
  const auto pixels=render();
  EXPECT_EQ(pixels.images[0][16+16*32],glm::vec4(1,2,4,64));
  EXPECT_EQ(pixels.images[1][16+16*32],glm::vec4(0));
  EXPECT_EQ(pixels.images[0][0],glm::vec4(0,0,0,64));save(pixels,"visible-emitter-hdr");
}
TEST_F(ReferencePathTraceGpu, SharedFacingFlagsCullSingleSidedBacksAndOrientDoubleSidedNormals) {
  objects[0].model=glm::rotate(glm::mat4(1),3.14159265359f,glm::vec3(0,1,0));
  const auto normal=glm::transpose(glm::inverse(glm::mat3(objects[0].model)));
  objects[0].normalMatrix0=glm::vec4(normal[0],0);objects[0].normalMatrix1=glm::vec4(normal[1],0);objects[0].normalMatrix2=glm::vec4(normal[2],0);
  build(0);build(1);input.settings.sampleLimit=512;
  const auto single=converge();EXPECT_EQ(single.roi(),glm::dvec3(1));
  materialValues[0].flags=4;syncInputs();const auto twoSided=converge();
  EXPECT_NEAR(twoSided.roi().x,.5,.012);EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, BufferCountMismatchesAreDiagnosedBeforeGpuIndexing) {
  input.settings.sampleLimit=64;input.data.limits.z=9;
  auto pixels=render();EXPECT_EQ(pixels.images[0][16+16*32],glm::vec4(0));
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,64*32*32);
  input.data.limits.z=1;lighting.pointLightCount=5;syncInputs();pixels=render();
  EXPECT_EQ(pixels.images[0][16+16*32],glm::vec4(0));EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,64*32*32);
}
TEST_F(ReferencePathTraceGpu, ExplicitEmptySceneUsesNoAccelerationDescriptorsAndAccumulatesEnvironment) {
  input.emptyScene=true;input.data.limits.w=2;input.settings.sampleLimit=128;
  input.data.environmentRadiance={2,.5f,.25f,1};
  const auto pixels=converge();
  for(const auto& pixel:pixels.images[0]) EXPECT_EQ(pixel,glm::vec4(2,.5f,.25f,128));
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,0);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
  EXPECT_EQ(tracer->historyResets(),1);
  EXPECT_GT(tracer->diagnosticSubmission(0),0);
  EXPECT_EQ(tracer->diagnosticEpoch(0),tracer->historyResets());
  input.emptyScene=false;render();EXPECT_EQ(tracer->historyResets(),2);
}
TEST_F(ReferencePathTraceGpu, UnavailableOidnPreservesTheRawMeanAndDisplay) {
  ASSERT_FALSE(device->referenceOidnEnabled());
  input.emptyScene = true;
  input.settings.sampleLimit = 4;
  const auto raw = render();
  command.reset();
  tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT, 2, {32, 32}, *sceneLayout, *lightLayout,
      512ull * 1024 * 1024, {.quality = ReferenceOidnQuality::High, .sampleInterval = 1});
  const auto fallback = render();
  const auto status = tracer->oidnStatus();
  EXPECT_EQ(status.displayedSamples, 0);
  EXPECT_EQ(status.allocatedBytes, 0);
  EXPECT_NE(status.message.find("vk_oidn"), std::string_view::npos);
  EXPECT_EQ(fallback.images, raw.images);
}
TEST_F(ReferencePathTraceGpu, PerspectivePointLightKeepsAllNearlyConstantRadianceSamples) {
  vertices.resize(4);
  for(size_t i=0;i<4;++i) { vertices[i]=vertices[0];vertices[i].position=glm::vec3(i==0||i==3?-1.f:1.f,i<2?-.8f:.8f,0); }
  indices={0,1,2,0,2,3};draws[0].indexCount=6;++revision;
  materialValues[0].color={1,1,1,1};materialValues[0].specularFactor=0;
  lighting.environmentIntensity=0;lighting.pointLightCount=1;pointValues.resize(1);
  pointValues[0].positionRadius={0,0,2,0};pointValues[0].colorIntensity={1,1,1,4};syncInputs();build(0);build(1);
  captureExtent={1280,800};tracer->createResources(CONTAINER_REFERENCE_PATH_SHADER_ROOT,2,captureExtent,*sceneLayout,*lightLayout);
  input.data.inverseProjection=glm::inverse(glm::perspectiveRH_ZO(.7853981634f,1280.f/800.f,100.f,.1f));
  input.data.inverseView=glm::translate(glm::mat4(1),glm::vec3(0,0,4));input.data.limits.w=0;
  input.settings.sampleLimit=13;input.settings.samplesPerFrame=4;input.settings.maxBounces=4;
  const auto pixels=converge();
  const auto diagnostics=tracer->diagnostics(0);
  size_t rejectedPixels=0;for(size_t i=0;i<pixels.images[0].size();++i) if(pixels.images[0][i].w!=13) ++rejectedPixels;
  EXPECT_EQ(rejectedPixels,0) << "failure stage bits " << diagnostics.failureStages;
  EXPECT_EQ(diagnostics.rejectedSamples,0) << "failure stage bits " << diagnostics.failureStages;
  EXPECT_NEAR(pixels.images[0][400*1280+640].x,1/3.141592653589793,.001);
  for(auto& vertex:vertices) vertex.colorAlpha=.5f;
  materialValues[0].flags=1;materialValues[0].alphaCutoff=.5f;syncInputs();++revision;build(0);build(1);
  const auto masked=converge();
  EXPECT_EQ(masked.images[0],pixels.images[0]);
  EXPECT_EQ(tracer->diagnostics(0).rejectedSamples,0);
}
TEST_F(ReferencePathTraceGpu, UnavailableSceneReportsDisplayDiagnosticWithoutInventingRawRadiance) {
  input.emptyScene=true;input.data.limits.w=2|8;input.settings.sampleLimit=64;
  const auto pixels=render();
  for(size_t i=0;i<pixels.images[0].size();++i) {
    EXPECT_EQ(pixels.images[0][i],glm::vec4(0));
    EXPECT_EQ(pixels.images[1][i],glm::vec4(0,0,0,1));
    EXPECT_EQ(pixels.images[2][i],glm::vec4(1,0,1,1));
  }
  EXPECT_EQ(tracer->diagnostics(0).unsupportedSamples,64*32*32);
  EXPECT_EQ(tracer->diagnostics(0).acceptedSamples,0);
}
TEST_F(ReferencePathTraceGpu, ExplicitInvalidationChangesHistoryOnlyAfterSubmissionAndRetainsSlotEpochs) {
  input.settings.samplesPerFrame=16;render(0);
  const auto epoch=tracer->diagnosticEpoch(0),serial=tracer->diagnosticSubmission(0);
  render(1);EXPECT_EQ(tracer->diagnosticEpoch(1),epoch);
  EXPECT_GT(tracer->diagnosticSubmission(1),serial);
  tracer->invalidateHistory();
  EXPECT_EQ(tracer->historyResets(),epoch);EXPECT_EQ(tracer->attemptedSamples(),32);
  render(1);EXPECT_EQ(tracer->attemptedSamples(),16);
  EXPECT_EQ(tracer->historyResets(),epoch+1);
  EXPECT_EQ(tracer->diagnosticEpoch(0),epoch);
  EXPECT_EQ(tracer->diagnosticEpoch(1),epoch+1);
}
} // namespace
