#include <Container/common/CommonGLFW.h>
#include <Container/renderer/culling/GpuCullManager.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/renderer/shadow/ShadowCullManager.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/VulkanDevice.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
using namespace container::gpu;
using namespace container::renderer;

static_assert(!std::is_copy_constructible_v<GpuCullManager::PreparedGeneration>);
static_assert(std::is_nothrow_move_constructible_v<GpuCullManager::PreparedGeneration>);
static_assert(std::is_nothrow_move_assignable_v<GpuCullManager::PreparedGeneration>);
static_assert(noexcept(std::declval<GpuCullManager&>().exchangeGeneration(
    std::declval<GpuCullManager::PreparedGeneration&>())));
static_assert(!std::is_copy_constructible_v<ShadowCullManager::PreparedGeneration>);
static_assert(std::is_nothrow_move_constructible_v<ShadowCullManager::PreparedGeneration>);
static_assert(std::is_nothrow_move_assignable_v<ShadowCullManager::PreparedGeneration>);
static_assert(noexcept(std::declval<ShadowCullManager&>().exchangeGeneration(
    std::declval<ShadowCullManager::PreparedGeneration&>())));

class CullGenerationGpu : public ::testing::Test {
 protected:
  struct DepthMip {
    VkExtent2D extent;
    std::vector<float> depths;
  };

  static std::vector<DepthMip> expectedHiZMips(VkExtent2D extent) {
    DepthMip base{extent, std::vector<float>(size_t(extent.width) * extent.height)};
    for (uint32_t y = 0; y < extent.height; ++y) {
      for (uint32_t x = 0; x < extent.width; ++x) {
        const bool trailingColumn = extent.width > 1 && x == extent.width - 1;
        const bool trailingRow = extent.height > 1 && y == extent.height - 1;
        const float depth = float(16 + (19 * x + 37 * y) % 97) / 128.f;
        base.depths[size_t(y) * extent.width + x] =
            trailingColumn || trailingRow ? 0.f : depth;
      }
    }
    // At an odd extent, texel (2, 2) overlaps the first reduced texel's
    // normalized footprint. A disjoint 2x2 reduction misses this minimum.
    if (extent.width == 17 && extent.height == 9)
      base.depths[2 * extent.width + 2] = 1.f / 16.f;

    std::vector<DepthMip> levels;
    levels.push_back(std::move(base));
    while (extent.width > 1 || extent.height > 1) {
      const auto& source = levels.back();
      extent = {std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)};
      DepthMip next{extent, std::vector<float>(size_t(extent.width) * extent.height, 1.f)};
      for (uint32_t y = 0; y < extent.height; ++y) {
        for (uint32_t x = 0; x < extent.width; ++x) {
          // Compare source texel intervals against the normalized interval
          // sampled by this destination texel. Bounds are inclusive/exclusive.
          const uint32_t xBegin = x * source.extent.width / extent.width;
          const uint32_t xEnd = ((x + 1) * source.extent.width + extent.width - 1) / extent.width;
          const uint32_t yBegin = y * source.extent.height / extent.height;
          const uint32_t yEnd = ((y + 1) * source.extent.height + extent.height - 1) / extent.height;
          float& minimum = next.depths[size_t(y) * extent.width + x];
          for (uint32_t sy = yBegin; sy < yEnd; ++sy) {
            for (uint32_t sx = xBegin; sx < xEnd; ++sx)
              minimum = std::min(minimum, source.depths[size_t(sy) * source.extent.width + sx]);
          }
        }
      }
      levels.push_back(std::move(next));
    }
    return levels;
  }

  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<CullGenerationGpu*>(user)->errors;
      std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_CULL_GENERATIONS");
    if(!enabled || std::strcmp(enabled,"1")!=0)
      GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_CULL_GENERATIONS=1";
    ASSERT_EQ(glfwInit(),GLFW_TRUE);glfwStarted=true;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    window=glfwCreateWindow(32,32,"Detached cull generations",nullptr,nullptr);
    ASSERT_NE(window,nullptr);
    uint32_t count{};const auto* extensions=glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions,nullptr);
    InstanceCreateInfo create;create.enableValidationLayers=true;
    create.validationLayers={"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions,extensions+count);
    create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount=1;validation.pEnabledValidationFeatures=&sync;
    create.next=&validation;instance=std::make_unique<VulkanInstance>(create);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation|
        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback=reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(debugCallback);
    debug.pUserData=this;messenger=vk::raii::DebugUtilsMessengerEXT(instance->raii(),debug);
    VkSurfaceKHR handle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(),window,nullptr,&handle),VK_SUCCESS);
    surface=vk::raii::SurfaceKHR(instance->raii(),handle);
    DeviceCreateInfo info;info.requiredExtensions={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    info.enabledFeatures.drawIndirectFirstInstance=VK_TRUE;
    info.enabledFeatures.multiDrawIndirect=VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    // VulkanMemoryManager enables VMA device-address allocations, including
    // ordinary mapped cull buffers. Enable the matching logical-device feature.
    v12.bufferDeviceAddress=VK_TRUE;
    v12.drawIndirectCount=VK_TRUE;info.next=&v12;
    device=std::make_shared<VulkanDevice>(*instance,handle,info);
    pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        device->queueFamilyIndices().graphicsFamily.value()));
    allocation=std::make_unique<AllocationManager>();
    allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),
        device->graphicsQueue(),*pool,{});
    pipelines=std::make_unique<PipelineManager>(device->device());
    CameraData camera;
    for(uint32_t image=0;image<2;++image) {
      cameras.push_back(buffer(sizeof(camera),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&camera));
      std::array<ObjectData,2> original{};
      original[0].boundingSphere={0,0,.5f,.05f};
      original[1].boundingSphere={5,0,.5f,.05f};
      liveObjects.push_back(buffer(sizeof(original),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,original.data()));
      // Swap object identities in image 1 to prove per-image descriptor writes.
      if(image) std::swap(original[0],original[1]);
      stagedObjects.push_back(buffer(sizeof(original),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,original.data()));
      ShadowCullData shadow;
      for(uint32_t cascade=0;cascade<kShadowCascadeCount;++cascade) {
        const float x=cascade==0?0.0f:cascade==1?5.0f:cascade==2?2.5f:20.0f;
        const float half=cascade==2?4.0f:1.0f;
        shadow.cascades[cascade].casterMinBounds={x-half,-1,-1,0};
        shadow.cascades[cascade].casterMaxBounds={x+half,1,1,0};
      }
      shadowBuffers.push_back(buffer(sizeof(shadow),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&shadow));
      shadowHandles.push_back(shadowBuffers.back().buffer);
    }
    primary=std::make_unique<GpuCullManager>(device,*allocation,*pipelines);
    primary->createResources(shaderRoot(),2);ASSERT_TRUE(primary->isReady());
    shadow=std::make_unique<ShadowCullManager>(device,*allocation,*pipelines);
    shadow->createResources(shaderRoot(),2);ASSERT_TRUE(shadow->isReady());
    ASSERT_TRUE(primary->ensureBufferCapacity(1));
    ASSERT_TRUE(shadow->ensureBufferCapacity(1));
    for(uint32_t image=0;image<2;++image) {
      primary->updateObjectSsboDescriptor(image,liveObjects[image].buffer,2*sizeof(ObjectData));
      primary->uploadDrawCommands(image,oldCommands,1);
      shadow->updateShadowCullDescriptor(image,shadowHandles[image],sizeof(ShadowCullData));
      shadow->updateObjectSsboDescriptor(image,liveObjects[image].buffer,2*sizeof(ObjectData));
      shadow->uploadDrawCommands(image,oldCommands,1);
    }
    for(uint32_t i=0;i<200;++i) commands.push_back({i%2,i*3,(i%7+1)*3,1});
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();
    primary.reset();shadow.reset();
    if(allocation) {
      for(auto* buffers:{&liveObjects,&stagedObjects,&cameras,&shadowBuffers})
        for(auto& item:*buffers) allocation->destroyBuffer(item);
    }
    pipelines.reset();allocation.reset();pool.clear();device.reset();
    surface.clear();messenger.clear();instance.reset();
    if(window) glfwDestroyWindow(window);
    if(glfwStarted) glfwTerminate();
    EXPECT_EQ(errors.load(),0u);
  }
  static std::filesystem::path shaderRoot() { return CONTAINER_CULL_SHADER_ROOT; }
  AllocatedBuffer buffer(size_t bytes,VkBufferUsageFlags usage,const void* data) {
    auto result=allocation->createBuffer(bytes,usage,VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    SceneController::writeToBuffer(*allocation,result,data,bytes);return result;
  }
  uint32_t allocations() const {
    VmaTotalStatistics stats{};vmaCalculateStatistics(allocation->memoryManager()->allocator(),&stats);
    return stats.total.statistics.allocationCount;
  }
  template<class T> std::vector<T> read(VkBuffer source,size_t count) {
    const auto bytes=count*sizeof(T);auto& memory=*allocation->memoryManager();
    struct Readback {
      VulkanMemoryManager& memory;AllocatedBuffer buffer;
      ~Readback(){memory.destroyBuffer(buffer);}
    } output{memory,memory.createBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|
        VMA_ALLOCATION_CREATE_MAPPED_BIT)};
    allocation->executeStagedCommands([&](VkCommandBuffer command) {
      VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
      barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer=source;barrier.size=bytes;
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
      const VkBufferCopy copy{0,0,bytes};
      vkCmdCopyBuffer(command,source,output.buffer.buffer,1,&copy);
      barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      barrier.buffer=output.buffer.buffer;
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
          0,0,nullptr,1,&barrier,0,nullptr);
    });
    if(vmaInvalidateAllocation(memory.allocator(),output.buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
      throw std::runtime_error("Cull readback invalidate failed");
    std::vector<T> result(count);
    std::memcpy(result.data(),output.buffer.allocation_info.pMappedData,bytes);return result;
  }
  void checkHiZMips(VkExtent2D extent) {
    const auto expected = expectedHiZMips(extent);
    SCOPED_TRACE(::testing::Message() << extent.width << 'x' << extent.height);
    auto& memory = *allocation->memoryManager();
    const auto sourceBytes = std::as_bytes(std::span(expected.front().depths));
    StagingBuffer upload(memory, sourceBytes.size());
    upload.upload(sourceBytes);

    struct DepthImage {
      VmaAllocator allocator;
      VkImage image{};
      VmaAllocation allocation{};
      ~DepthImage() {
        if (image) vmaDestroyImage(allocator, image, allocation);
      }
    } depth{memory.allocator()};
    VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depthInfo.imageType = VK_IMAGE_TYPE_2D;
    depthInfo.format = VK_FORMAT_D32_SFLOAT;
    depthInfo.extent = {extent.width, extent.height, 1};
    depthInfo.mipLevels = depthInfo.arrayLayers = 1;
    depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VmaAllocationCreateInfo depthAllocation{};
    depthAllocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ASSERT_EQ(vmaCreateImage(depth.allocator, &depthInfo, &depthAllocation, &depth.image,
                             &depth.allocation, nullptr), VK_SUCCESS);
    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = depth.image;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = vk::Format::eD32Sfloat;
    viewInfo.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    const vk::raii::ImageView depthView(device->raii(), viewInfo);
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
        vk::SamplerAddressMode::eClampToEdge;
    const vk::raii::Sampler depthSampler(device->raii(), samplerInfo);

    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize readbackBytes = 0;
    for (uint32_t level = 0; level < expected.size(); ++level) {
      const auto& mip = expected[level];
      VkBufferImageCopy copy{};
      copy.bufferOffset = readbackBytes;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
      copy.imageExtent = {mip.extent.width, mip.extent.height, 1};
      copies.push_back(copy);
      readbackBytes += mip.depths.size() * sizeof(float);
    }
    struct Readback {
      VulkanMemoryManager& memory;
      AllocatedBuffer buffer;
      ~Readback() { memory.destroyBuffer(buffer); }
    } readback{memory, memory.createBuffer(readbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT)};

    constexpr uint32_t imageIndex = 0;
    primary->ensureHiZImage(imageIndex, extent.width, extent.height);
    primary->beginFrameCulling(imageIndex);
    const VkImage pyramid = primary->hiZImage(imageIndex);
    ASSERT_NE(pyramid, VK_NULL_HANDLE);
    allocation->executeStagedCommands([&](VkCommandBuffer command) {
      VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      imageBarrier.srcQueueFamilyIndex = imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      imageBarrier.image = depth.image;
      imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
      VkBufferImageCopy sourceCopy{};
      sourceCopy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      sourceCopy.imageExtent = depthInfo.extent;
      vkCmdCopyBufferToImage(command, upload.buffer().buffer, depth.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &sourceCopy);

      imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      imageBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
      primary->dispatchHiZGenerate(command, imageIndex, *depthView, *depthSampler,
                                   extent.width, extent.height);
      if (!primary->hizGeneratedThisFrame(imageIndex)) return;

      imageBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      imageBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      imageBarrier.image = pyramid;
      imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(copies.size()), 0, 1};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
      vkCmdCopyImageToBuffer(command, pyramid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer.buffer, uint32_t(copies.size()), copies.data());

      imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      host.buffer = readback.buffer.buffer;
      host.size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                           0, 0, nullptr, 1, &host, 1, &imageBarrier);
    });
    ASSERT_TRUE(primary->hizGeneratedThisFrame(imageIndex));
    ASSERT_EQ(vmaInvalidateAllocation(memory.allocator(), readback.buffer.allocation, 0, VK_WHOLE_SIZE),
              VK_SUCCESS);

    ASSERT_NE(readback.buffer.allocation_info.pMappedData, nullptr);
    std::vector<float> depths(size_t(readbackBytes / sizeof(float)));
    std::memcpy(depths.data(), readback.buffer.allocation_info.pMappedData, size_t(readbackBytes));
    auto actual = depths.cbegin();
    size_t mismatchedLevels = 0;
    for (size_t level = 0; level < expected.size(); ++level) {
      const auto& mip = expected[level];
      const auto mismatch = std::mismatch(mip.depths.begin(), mip.depths.end(), actual);
      if (mismatch.first != mip.depths.end()) {
        const auto offset = size_t(mismatch.first - mip.depths.begin());
        ++mismatchedLevels;
        ADD_FAILURE() << "Hi-Z mip " << level << " texel (" << offset % mip.extent.width << ','
                      << offset / mip.extent.width << ") expected " << *mismatch.first
                      << ", read " << *mismatch.second;
      }
      actual += mip.depths.size();
    }
    std::printf("HIZ_MIP_READBACK extent=%ux%u levels=%zu texels=%llu mismatchedLevels=%zu\n",
                extent.width, extent.height, expected.size(),
                static_cast<unsigned long long>(readbackBytes / sizeof(float)), mismatchedLevels);
  }
  void checkPrimary(GpuCullManager& manager,std::span<const DrawCommand> draws,bool staged) {
    for(uint32_t image=0;image<2;++image) {
      allocation->executeStagedCommands([&](VkCommandBuffer command) {
        manager.dispatchFrustumCull(command,image,cameras[image].buffer,sizeof(CameraData),uint32_t(draws.size()));
      });
      ASSERT_TRUE(manager.frustumDrawsValid(image));
      const auto count=read<uint32_t>(manager.drawCountBuffer(image),1).front();
      std::vector<uint32_t> expected;
      for(const auto& draw:draws)
        if(draw.objectIndex==(staged?image:0u)) expected.push_back(draw.firstIndex);
      ASSERT_EQ(count,expected.size());
      const auto output=read<GpuDrawIndexedIndirectCommand>(manager.indirectDrawBuffer(image),count);
      std::vector<uint32_t> actual;
      for(const auto& draw:output) {
        EXPECT_EQ(draw.firstInstance,staged?image:0u);EXPECT_EQ(draw.instanceCount,1u);
        const auto input=std::find_if(draws.begin(),draws.end(),[&](const auto& item){return item.firstIndex==draw.firstIndex;});
        ASSERT_NE(input,draws.end());EXPECT_EQ(draw.indexCount,input->indexCount);
        EXPECT_EQ(draw.vertexOffset,0);actual.push_back(draw.firstIndex);
      }
      std::sort(actual.begin(),actual.end());std::sort(expected.begin(),expected.end());
      EXPECT_EQ(actual,expected);
    }
  }
  void checkShadow(ShadowCullManager& manager,std::span<const DrawCommand> draws,bool staged) {
    for(uint32_t image=0;image<2;++image) {
      for(uint32_t cascade=0;cascade<kShadowCascadeCount;++cascade) {
        bool dispatched=false;
        allocation->executeStagedCommands([&](VkCommandBuffer command) {
          dispatched=manager.dispatchCascadeCull(command,image,cascade,uint32_t(draws.size()));
        });
        ASSERT_TRUE(dispatched);
        EXPECT_EQ(read<uint32_t>(manager.drawCountBuffer(image,cascade),1).front(),draws.size());
        const auto output=read<GpuDrawIndexedIndirectCommand>(manager.indirectDrawBuffer(image,cascade),draws.size());
        for(size_t i=0;i<draws.size();++i) {
          const auto& input=draws[i];const auto& actual=output[i];
          const uint32_t atOrigin=staged?image:0u;
          const bool visible=cascade==2 || (cascade==0 && input.objectIndex==atOrigin) ||
              (cascade==1 && input.objectIndex!=atOrigin);
          EXPECT_EQ(actual.firstIndex,input.firstIndex);EXPECT_EQ(actual.indexCount,input.indexCount);
          EXPECT_EQ(actual.firstInstance,input.objectIndex);EXPECT_EQ(actual.instanceCount,visible?1u:0u);
          EXPECT_EQ(actual.vertexOffset,0);
        }
      }
    }
  }
  std::atomic_uint errors{};bool glfwStarted{};GLFWwindow* window{};
  std::unique_ptr<VulkanInstance> instance;vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};std::shared_ptr<VulkanDevice> device;
  vk::raii::CommandPool pool{nullptr};std::unique_ptr<AllocationManager> allocation;
  std::unique_ptr<PipelineManager> pipelines;
  std::unique_ptr<GpuCullManager> primary;std::unique_ptr<ShadowCullManager> shadow;
  std::vector<AllocatedBuffer> liveObjects,stagedObjects,cameras,shadowBuffers;
  std::vector<VkBuffer> shadowHandles;const std::array<size_t,2> capacities{2,2};
  const std::vector<DrawCommand> oldCommands{{0,3000,3,1}};
  std::vector<DrawCommand> commands;
};

TEST_F(CullGenerationGpu, HiZMipsMatchNormalizedFootprintMinimaAtActualExtents) {
  VkFormatProperties format{};
  vkGetPhysicalDeviceFormatProperties(device->physicalDevice(), VK_FORMAT_D32_SFLOAT, &format);
  constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((format.optimalTilingFeatures & required) != required)
    GTEST_SKIP() << "D32 depth upload and sampling are unavailable";

  constexpr std::array extents{VkExtent2D{17, 9}, VkExtent2D{1, 17},
                               VkExtent2D{17, 1}, VkExtent2D{1920, 1080}};
  for (const auto extent : extents)
    checkHiZMips(extent);
}

TEST_F(CullGenerationGpu,PrimaryStagesRealOutputsAndPublishesWithoutCapacityGrowth) {
  auto* identity=primary.get();const auto oldBuffer=primary->indirectDrawBuffer(0);
  primary->ensureHiZImage(0,16,16);primary->ensureHiZImage(1,16,16);
  ASSERT_TRUE(primary->occlusionCullResourcesReady(0));
  const auto baseline=allocations();
  {
    auto prepared=primary->prepareGeneration(stagedObjects,capacities,cameras,sizeof(CameraData),commands,17);
    ASSERT_TRUE(prepared.ready());ASSERT_TRUE(primary->canExchangeGeneration(prepared));
    EXPECT_EQ(primary->indirectDrawBuffer(0),oldBuffer);EXPECT_EQ(primary->maxDrawCount(),64u);
    EXPECT_NE(prepared.indirectDrawBuffer(0),oldBuffer);EXPECT_GE(prepared.maxDrawCount(),commands.size());
    checkPrimary(prepared.manager(),commands,true);checkPrimary(*primary,oldCommands,false);
    const auto stagedBuffer=prepared.indirectDrawBuffer(0);const auto stagedAllocations=allocations();
    ASSERT_TRUE(primary->exchangeGeneration(prepared));EXPECT_EQ(primary.get(),identity);
    EXPECT_EQ(primary->indirectDrawBuffer(0),stagedBuffer);
    EXPECT_TRUE(primary->occlusionCullResourcesReady(0));
    EXPECT_FALSE(primary->frustumDrawsValid(0));EXPECT_FALSE(primary->hizGeneratedThisFrame(0));
    EXPECT_FALSE(primary->ensureBufferCapacity(uint32_t(commands.size())));
    for(uint32_t image=0;image<2;++image) primary->uploadDrawCommands(image,commands,17);
    EXPECT_EQ(allocations(),stagedAllocations);checkPrimary(*primary,commands,true);
    ASSERT_TRUE(primary->exchangeGeneration(prepared));EXPECT_EQ(primary->indirectDrawBuffer(0),oldBuffer);
    checkPrimary(*primary,oldCommands,false);
    ASSERT_TRUE(primary->exchangeGeneration(prepared));checkPrimary(*primary,commands,true);
  }
  // The new payload remains live; the prior payload was released independently.
  EXPECT_GE(allocations(),baseline);
}

TEST_F(CullGenerationGpu,ShadowStagesEveryImageAndCascadeWithIndependentOldPayload) {
  auto* identity=shadow.get();const auto oldBuffer=shadow->indirectDrawBuffer(0,0);
  auto prepared=shadow->prepareGeneration(stagedObjects,capacities,shadowHandles,sizeof(ShadowCullData),commands,23);
  ASSERT_TRUE(prepared.ready());ASSERT_TRUE(shadow->canExchangeGeneration(prepared));
  EXPECT_EQ(shadow->indirectDrawBuffer(0,0),oldBuffer);EXPECT_EQ(shadow->maxDrawCount(0),64u);
  checkShadow(prepared.manager(),commands,true);checkShadow(*shadow,oldCommands,false);
  const auto stagedBuffer=prepared.indirectDrawBuffer(0,0);const auto stagedAllocations=allocations();
  ASSERT_TRUE(shadow->exchangeGeneration(prepared));EXPECT_EQ(shadow.get(),identity);
  EXPECT_EQ(shadow->indirectDrawBuffer(0,0),stagedBuffer);
  for(uint32_t image=0;image<2;++image) {
    EXPECT_FALSE(shadow->ensureBufferCapacity(image,uint32_t(commands.size())));
    shadow->uploadDrawCommands(image,commands,23);
  }
  EXPECT_EQ(allocations(),stagedAllocations);checkShadow(*shadow,commands,true);
  ASSERT_TRUE(shadow->exchangeGeneration(prepared));EXPECT_EQ(shadow->indirectDrawBuffer(0,0),oldBuffer);
  checkShadow(*shadow,oldCommands,false);
  ASSERT_TRUE(shadow->exchangeGeneration(prepared));checkShadow(*shadow,commands,true);
}

TEST_F(CullGenerationGpu,InvalidForeignStaleAndDiscardedPreparationsLeaveLiveResourcesIntact) {
  const auto baseline=allocations();const auto oldPrimary=primary->indirectDrawBuffer(0);
  const auto oldShadow=shadow->indirectDrawBuffer(0,0);
  auto invalid=commands;invalid.back().objectIndex=2;
  EXPECT_THROW(primary->prepareGeneration(stagedObjects,capacities,cameras,sizeof(CameraData),invalid,2),std::invalid_argument);
  EXPECT_THROW(shadow->prepareGeneration(stagedObjects,capacities,shadowHandles,sizeof(ShadowCullData),invalid,2),std::invalid_argument);
  const std::array<size_t,1> wrong{2};
  EXPECT_THROW(primary->prepareGeneration(stagedObjects,wrong,cameras,sizeof(CameraData),commands,2),std::invalid_argument);
  EXPECT_THROW(shadow->prepareGeneration(stagedObjects,wrong,shadowHandles,sizeof(ShadowCullData),commands,2),std::invalid_argument);
  EXPECT_EQ(allocations(),baseline);
  {
    auto p=primary->prepareGeneration(stagedObjects,capacities,cameras,sizeof(CameraData),commands,2);
    auto s=shadow->prepareGeneration(stagedObjects,capacities,shadowHandles,sizeof(ShadowCullData),commands,2);
    GpuCullManager foreignPrimary(device,*allocation,*pipelines);
    ShadowCullManager foreignShadow(device,*allocation,*pipelines);
    EXPECT_FALSE(foreignPrimary.exchangeGeneration(p));EXPECT_FALSE(foreignShadow.exchangeGeneration(s));
    EXPECT_EQ(primary->indirectDrawBuffer(0),oldPrimary);EXPECT_EQ(shadow->indirectDrawBuffer(0,0),oldShadow);
  }
  EXPECT_EQ(allocations(),baseline);checkPrimary(*primary,oldCommands,false);checkShadow(*shadow,oldCommands,false);
  auto p=primary->prepareGeneration(stagedObjects,capacities,cameras,sizeof(CameraData),commands,3);
  auto s=shadow->prepareGeneration(stagedObjects,capacities,shadowHandles,sizeof(ShadowCullData),commands,3);
  // Current bindings changed after prepare: no stale candidate may publish.
  primary->updateObjectSsboDescriptor(0,stagedObjects[0].buffer,2*sizeof(ObjectData));
  shadow->updateObjectSsboDescriptor(0,stagedObjects[0].buffer,2*sizeof(ObjectData));
  EXPECT_FALSE(primary->canExchangeGeneration(p));EXPECT_FALSE(primary->exchangeGeneration(p));
  EXPECT_FALSE(shadow->canExchangeGeneration(s));EXPECT_FALSE(shadow->exchangeGeneration(s));
  EXPECT_EQ(primary->indirectDrawBuffer(0),oldPrimary);EXPECT_EQ(shadow->indirectDrawBuffer(0,0),oldShadow);
}

TEST_F(CullGenerationGpu,UnavailablePipelinesRetainCpuFallbackWithoutActiveManagerChanges) {
  GpuCullManager fallbackPrimary(device,*allocation,*pipelines);
  fallbackPrimary.recreatePerFrameResources(2);
  ShadowCullManager fallbackShadow(device,*allocation,*pipelines);
  fallbackShadow.createResources(shaderRoot()/"no-such-shader-generation",2);
  ASSERT_FALSE(fallbackPrimary.isReady());ASSERT_FALSE(fallbackShadow.isReady());
  const auto before=allocations();
  auto p=fallbackPrimary.prepareGeneration(stagedObjects,capacities,cameras,sizeof(CameraData),commands,9);
  auto s=fallbackShadow.prepareGeneration(stagedObjects,capacities,shadowHandles,sizeof(ShadowCullData),commands,9);
  ASSERT_TRUE(p.ready());ASSERT_TRUE(s.ready());EXPECT_EQ(allocations(),before);
  ASSERT_TRUE(fallbackPrimary.exchangeGeneration(p));ASSERT_TRUE(fallbackShadow.exchangeGeneration(s));
  EXPECT_FALSE(fallbackPrimary.isReady());EXPECT_FALSE(fallbackShadow.isReady());
  EXPECT_EQ(fallbackPrimary.indirectDrawBuffer(0),VK_NULL_HANDLE);
  EXPECT_EQ(fallbackShadow.indirectDrawBuffer(0,0),VK_NULL_HANDLE);
  EXPECT_FALSE(fallbackShadow.canDispatchCascadeCull(0,0));
  EXPECT_TRUE(primary->isReady());EXPECT_TRUE(shadow->isReady());
}
} // namespace
