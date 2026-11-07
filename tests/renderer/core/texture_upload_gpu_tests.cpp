#include <Container/common/CommonGLFW.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/utility/TextureImageData.h>
#include <Container/utility/TextureManager.h>
#include <Container/utility/FileLoader.h>

#include <gtest/gtest.h>
#include "../scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <thread>
#include <vector>

namespace {
using namespace container::gpu;
namespace material=container::material;

template<class T> std::vector<std::byte> bytes(std::span<const T> values) {
  const auto raw=std::as_bytes(values); return {raw.begin(),raw.end()};
}
template<class T,size_t N> std::vector<std::byte> bytes(const std::array<T,N>& values) {
  return bytes(std::span<const T>(values));
}

material::TextureImageData singleTexel(unsigned char value) {
  material::TextureImageData image; image.format=VK_FORMAT_R8G8B8A8_UNORM;
  image.authoredMipChain=true;
  image.mips.push_back({1,1,bytes(std::array<unsigned char,4>{value,17,81,255})});
  return image;
}

static_assert(!std::is_copy_constructible_v<AllocationManager::TextureAllocationBatch>);
static_assert(std::is_nothrow_move_constructible_v<AllocationManager::TextureAllocationBatch>);
static_assert(std::is_nothrow_move_assignable_v<AllocationManager::TextureAllocationBatch>);
static_assert(std::is_nothrow_move_constructible_v<
    std::shared_ptr<const AllocationManager::TextureAllocationLease>>);

class TextureUploadGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if (severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<TextureUploadGpu*>(user)->validationErrors;
      std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_TEXTURE_UPLOADS");
    if (!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_TEXTURE_UPLOADS=1";
    ASSERT_EQ(glfwInit(),GLFW_TRUE); glfwStarted=true;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    window=glfwCreateWindow(32,32,"Typed texture upload contract",nullptr,nullptr); ASSERT_NE(window,nullptr);
    uint32_t count{}; const auto* extensions=glfwGetRequiredInstanceExtensions(&count); ASSERT_NE(extensions,nullptr);
    InstanceCreateInfo create; create.enableValidationLayers=true; create.validationLayers={"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions,extensions+count); create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount=1; validation.pEnabledValidationFeatures=&sync; create.next=&validation;
    instance=std::make_unique<VulkanInstance>(create);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation|vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback=reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(callback); debug.pUserData=this;
    messenger=vk::raii::DebugUtilsMessengerEXT(instance->raii(),debug);
    VkSurfaceKHR handle{}; ASSERT_EQ(glfwCreateWindowSurface(instance->instance(),window,nullptr,&handle),VK_SUCCESS);
    surface=vk::raii::SurfaceKHR(instance->raii(),handle);
    DeviceCreateInfo info; info.requiredExtensions={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    info.optionalFeatures.textureCompressionBC=VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress=VK_TRUE;v12.timelineSemaphore=VK_TRUE;info.next=&v12;
    device=std::make_shared<VulkanDevice>(*instance,handle,info);
    pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        device->queueFamilyIndices().graphicsFamily.value()));
    auto commands=device->raii().allocateCommandBuffers(vk::CommandBufferAllocateInfo(*pool,vk::CommandBufferLevel::ePrimary,1));
    command=std::move(commands.front());
    allocation=std::make_unique<AllocationManager>();
    allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
    allocation->setTextureCompressionFeatures(device->enabledFeatures());
  }
  void TearDown() override {
    if (device) device->raii().waitIdle();
    command.clear(); allocation.reset(); pool.clear(); device.reset(); surface.clear(); messenger.clear(); instance.reset();
    if (window) glfwDestroyWindow(window);
    if (glfwStarted) glfwTerminate();
    EXPECT_EQ(validationErrors.load(),0u);
  }
  bool supports(VkFormat format) const {
    const auto options=allocation->textureDecodeOptions(false);
    return std::ranges::find(options.allowedFormats,format)!=options.allowedFormats.end();
  }
  uint32_t liveAllocations() const {
    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(allocation->memoryManager()->allocator(),&statistics);
    return statistics.total.statistics.allocationCount;
  }
  void adoptImage(const std::shared_ptr<const AllocationManager::TextureImageAllocationLease>& pin) {
    ASSERT_TRUE(allocation->adoptTextureImages(std::span(&pin,1)));
  }
  vk::raii::Semaphore timelineGate() const {
    vk::SemaphoreTypeCreateInfo type(vk::SemaphoreType::eTimeline,0);
    vk::SemaphoreCreateInfo create;create.pNext=&type;
    return vk::raii::Semaphore(device->raii(),create);
  }
  VkResult openGate(VkSemaphore gate) const {
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=1;
    return vkSignalSemaphore(device->device(),&signal);
  }
  uint64_t gateValue(VkSemaphore gate) const {
    uint64_t value{};
    if(vkGetSemaphoreCounterValue(device->device(),gate,&value)!=VK_SUCCESS)
      throw std::runtime_error("Could not read upload test timeline gate");
    return value;
  }
  std::vector<material::TextureMipLevel> readback(const material::TextureResource& resource,
      std::function<void()> afterSubmit={},VkSemaphore gate=VK_NULL_HANDLE) {
    std::vector<material::TextureMipLevel> result;
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize total{}; uint32_t width=resource.width,height=resource.height;
    for (uint32_t level=0;level<resource.mipLevels;++level) {
      total=(total+15)&~VkDeviceSize(15);
      const auto size=material::TextureMipByteSize(resource.format,width,height);
      VkBufferImageCopy copy{}; copy.bufferOffset=total;
      copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1}; copy.imageExtent={width,height,1}; copies.push_back(copy);
      result.push_back({width,height,std::vector<std::byte>(size)}); total+=size;
      width=std::max(width/2,1u); height=std::max(height/2,1u);
    }
    auto* memory=allocation->memoryManager();
    auto output=memory->createBuffer(total,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    command.reset(); command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    image.image=resource.image; image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,resource.mipLevels,0,1};
    image.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image.srcAccessMask=VK_ACCESS_SHADER_READ_BIT; image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(*command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
    vkCmdCopyImageToBuffer(*command,resource.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,uint32_t(copies.size()),copies.data());
    image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(*command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&image);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    host.buffer=output.buffer; host.offset=0; host.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(*command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    command.end();const VkCommandBuffer raw=*command;
    const uint64_t waitValue=1;const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&waitValue;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&raw;
    if(gate) {submit.pNext=&timeline;submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&gate;submit.pWaitDstStageMask=&waitStage;}
    vk::raii::Queue queue(device->raii(),device->queueFamilyIndices().graphicsFamily.value(),0);
    if(vkQueueSubmit(device->graphicsQueue(),1,&submit,VK_NULL_HANDLE)!=VK_SUCCESS)
      throw std::runtime_error("Could not submit typed texture readback");
    if(afterSubmit) afterSubmit();
    queue.waitIdle();
    if (vmaInvalidateAllocation(memory->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
      throw std::runtime_error("Could not invalidate typed texture readback");
    for (size_t i=0;i<result.size();++i)
      std::memcpy(result[i].bytes.data(),static_cast<const std::byte*>(output.allocation_info.pMappedData)+copies[i].bufferOffset,result[i].bytes.size());
    memory->destroyBuffer(output);
    return result;
  }
  void exact(const material::TextureImageData& image) {
    const auto uploaded=allocation->createTextureFromImageData("typed-exact",image);
    EXPECT_NE(uploaded.imageView,VK_NULL_HANDLE); EXPECT_EQ(uploaded.format,image.format);
    EXPECT_EQ(uploaded.width,image.mips.front().width); EXPECT_EQ(uploaded.height,image.mips.front().height);
    EXPECT_EQ(uploaded.mipLevels,image.mips.size());
    const auto result=readback(uploaded); ASSERT_EQ(result.size(),image.mips.size());
    for (size_t level=0;level<result.size();++level) EXPECT_EQ(result[level].bytes,image.mips[level].bytes)<<level;
  }
  std::vector<std::array<float,4>> sampleImageView(const material::TextureResource& image) {
    auto* memory=allocation->memoryManager();
    struct Output {
      VulkanMemoryManager* memory;
      AllocatedBuffer buffer;
      ~Output() {memory->destroyBuffer(buffer);}
    } output{memory,memory->createBuffer(sizeof(float)*4*image.mipLevels,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT)};
    const std::array bindings{
        vk::DescriptorSetLayoutBinding(0,vk::DescriptorType::eSampledImage,1,vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1,vk::DescriptorType::eSampler,1,vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute)};
    vk::raii::DescriptorSetLayout layout(device->raii(),vk::DescriptorSetLayoutCreateInfo({},bindings));
    const std::array sizes{vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage,1),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampler,1),vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer,1)};
    vk::raii::DescriptorPool descriptors(device->raii(),vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,1,sizes));
    const std::array rawLayouts{*layout};
    auto sets=device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors,rawLayouts));
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.magFilter=samplerInfo.minFilter=vk::Filter::eNearest;
    samplerInfo.mipmapMode=vk::SamplerMipmapMode::eNearest;
    samplerInfo.borderColor=vk::BorderColor::eFloatTransparentBlack;
    samplerInfo.maxLod=float(image.mipLevels-1);
    vk::raii::Sampler sampler(device->raii(),samplerInfo);
    const vk::DescriptorImageInfo textureInfo({},image.imageView,vk::ImageLayout::eShaderReadOnlyOptimal);
    const vk::DescriptorImageInfo samplerDescriptor(*sampler,{},vk::ImageLayout::eUndefined);
    const vk::DescriptorBufferInfo bufferInfo(output.buffer.buffer,0,VK_WHOLE_SIZE);
    const std::array writes{
        vk::WriteDescriptorSet(*sets[0],0,0,1,vk::DescriptorType::eSampledImage,&textureInfo),
        vk::WriteDescriptorSet(*sets[0],1,0,1,vk::DescriptorType::eSampler,&samplerDescriptor),
        vk::WriteDescriptorSet(*sets[0],2,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&bufferInfo)};
    device->raii().updateDescriptorSets(writes,{});
    vk::raii::PipelineLayout pipelineLayout(device->raii(),vk::PipelineLayoutCreateInfo({},rawLayouts));
    const auto code=container::util::readFile(CONTAINER_TEXTURE_UPLOAD_SAMPLE_SPV);
    vk::raii::ShaderModule module(device->raii(),vk::ShaderModuleCreateInfo({},code.size(),reinterpret_cast<const uint32_t*>(code.data())));
    vk::raii::Pipeline pipeline(device->raii(),nullptr,vk::ComputePipelineCreateInfo({},
        vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,*module,"computeMain"),*pipelineLayout));
    command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    command.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
    const std::array rawSets{*sets[0]};
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*pipelineLayout,0,rawSets,{});
    command.dispatch(image.mipLevels,1,1);
    const vk::MemoryBarrier host(vk::AccessFlagBits::eShaderWrite,vk::AccessFlagBits::eHostRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,{},host,{},{});
    command.end();
    const std::array commands{*command};device->raii().getQueue(device->queueFamilyIndices().graphicsFamily.value(),0).submit(vk::SubmitInfo({}, {},commands));
    device->raii().getQueue(device->queueFamilyIndices().graphicsFamily.value(),0).waitIdle();
    if(vmaInvalidateAllocation(memory->allocator(),output.buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
      throw std::runtime_error("Could not invalidate sampled texture values");
    std::vector<std::array<float,4>> result(image.mipLevels);
    std::memcpy(result.data(),output.buffer.allocation_info.pMappedData,result.size()*sizeof(result[0]));
    return result;
  }
  bool glfwStarted{}; GLFWwindow* window{};
  std::unique_ptr<VulkanInstance> instance; vk::raii::DebugUtilsMessengerEXT messenger{nullptr}; vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device; vk::raii::CommandPool pool{nullptr}; vk::raii::CommandBuffer command{nullptr};
  std::unique_ptr<AllocationManager> allocation;
  std::atomic<uint32_t> validationErrors{};
};

TEST_F(TextureUploadGpu, R16PrecisionAndAuthoredSinglePartialMipChainsAreBitExact) {
  ASSERT_TRUE(supports(VK_FORMAT_R16_UNORM));
  for (const size_t levels:{size_t(1),size_t(2)}) {
    material::TextureImageData image; image.format=VK_FORMAT_R16_UNORM; image.authoredMipChain=true;
    // A 7x5 image has three possible levels: two supplied levels are a
    // deliberately partial authored pyramid and must stay partial.
    uint32_t width=7,height=5;
    for (size_t level=0;level<levels;++level) {
      std::vector<uint16_t> samples(width*height);
      for (size_t i=0;i<samples.size();++i) samples[i]=uint16_t(257+i*1237+level*8191);
      image.mips.push_back({width,height,bytes(std::span<const uint16_t>(samples))});
      width=std::max(width/2,1u); height=std::max(height/2,1u);
    }
    exact(image);
  }
}

TEST_F(TextureUploadGpu, FloatHdrAuthoredMipChainRetainsValuesAboveOneAndNegativeChannels) {
  ASSERT_TRUE(supports(VK_FORMAT_R32G32B32A32_SFLOAT));
  material::TextureImageData image; image.format=VK_FORMAT_R32G32B32A32_SFLOAT; image.authoredMipChain=true;
  image.mips.push_back({2,1,bytes(std::array<float,8>{8,.1234567f,-2,1,2,4,6,.25f})});
  image.mips.push_back({1,1,bytes(std::array<float,4>{99,-.03125f,17,.75f})});
  exact(image);
}

TEST_F(TextureUploadGpu, ShaderSamplingAppliesR16GraySwizzlesAtEveryAuthoredMip) {
  ASSERT_TRUE(supports(VK_FORMAT_R16_UNORM));
  material::TextureImageData image;image.format=VK_FORMAT_R16_UNORM;image.authoredMipChain=true;
  image.componentMapping={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
  image.mips.push_back({2,2,bytes(std::array<uint16_t,4>{0x1234,0x1234,0x1234,0x1234})});
  image.mips.push_back({1,1,bytes(std::array<uint16_t,1>{0xabcd})});
  const auto sampled=sampleImageView(allocation->createTextureFromImageData("sampled-gray16",image));
  ASSERT_EQ(sampled.size(),2u);
  for(size_t mip=0;mip<sampled.size();++mip) {
    const float expected=float(mip?0xabcd:0x1234)/65535.0f;
    for(size_t channel=0;channel<3;++channel) EXPECT_NEAR(sampled[mip][channel],expected,0.000001f);
    EXPECT_FLOAT_EQ(sampled[mip][3],1.0f);
  }
}

TEST_F(TextureUploadGpu, ShaderSamplingPreservesSignedHdrAndViewChannelOrderAtEveryMip) {
  ASSERT_TRUE(supports(VK_FORMAT_R32G32B32A32_SFLOAT));
  material::TextureImageData image;image.format=VK_FORMAT_R32G32B32A32_SFLOAT;image.authoredMipChain=true;
  image.componentMapping={VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_A};
  image.mips.push_back({2,2,bytes(std::array<float,16>{-.25f,4,2,.125f,-.25f,4,2,.125f,-.25f,4,2,.125f,-.25f,4,2,.125f})});
  image.mips.push_back({1,1,bytes(std::array<float,4>{16,.25f,-2,.75f})});
  const auto sampled=sampleImageView(allocation->createTextureFromImageData("sampled-signed-hdr",image));
  const std::array expected{std::array<float,4>{2,-.25f,4,.125f},std::array<float,4>{-2,16,.25f,.75f}};
  ASSERT_EQ(sampled.size(),expected.size());
  for(size_t mip=0;mip<sampled.size();++mip)
    for(size_t channel=0;channel<4;++channel) EXPECT_FLOAT_EQ(sampled[mip][channel],expected[mip][channel]);
}

TEST_F(TextureUploadGpu, ShaderSamplingDecodesSrgbColorOnceWhileKeepingAlphaLinear) {
  const auto decode=[](float value) {return value<=0.04045f?value/12.92f:std::pow((value+0.055f)/1.055f,2.4f);};
  for(const auto format:{VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB}) {
    ASSERT_TRUE(supports(format));
    material::TextureImageData image;image.format=format;image.authoredMipChain=true;
    image.mips.push_back({1,1,bytes(std::array<uint8_t,4>{128,64,32,17})});
    const auto sampled=sampleImageView(allocation->createTextureFromImageData("sampled-color-space",image));
    ASSERT_EQ(sampled.size(),1u);
    for(size_t channel=0;channel<4;++channel) {
      float expected=float(std::array<uint8_t,4>{128,64,32,17}[channel])/255.0f;
      if(format==VK_FORMAT_R8G8B8A8_SRGB && channel<3) {
        // Bound hardware EOTF precision in the authored 8-bit domain. Half
        // one source code is an independent format-fidelity interval; the
        // sampled result need not equal a CPU float32 pow calculation.
        const float halfCode=0.5f/255.0f;
        EXPECT_GE(sampled[0][channel],decode(expected-halfCode));
        EXPECT_LE(sampled[0][channel],decode(expected+halfCode));
      } else {
        EXPECT_NEAR(sampled[0][channel],expected,0.00001f);
      }
    }
  }
}

TEST_F(TextureUploadGpu, ShaderSamplingPreservesBc3CoverageAlpha) {
  if(!supports(VK_FORMAT_BC3_UNORM_BLOCK)) GTEST_SKIP()<<"Logical device cannot enable BC3";
  material::TextureImageData image;image.format=VK_FORMAT_BC3_UNORM_BLOCK;image.authoredMipChain=true;
  // Independently authored BC3 block: constant red endpoint and alpha 64/255.
  // All color/alpha selector bits are zero, selecting endpoint zero directly.
  image.mips.push_back({4,4,bytes(std::array<uint8_t,16>{64,64,0,0,0,0,0,0,0,248,224,7,0,0,0,0})});
  const auto sampled=sampleImageView(allocation->createTextureFromImageData("sampled-bc3-alpha",image));
  ASSERT_EQ(sampled.size(),1u);
  EXPECT_FLOAT_EQ(sampled[0][0],1.0f);EXPECT_FLOAT_EQ(sampled[0][1],0.0f);EXPECT_FLOAT_EQ(sampled[0][2],0.0f);
  EXPECT_NEAR(sampled[0][3],64.0f/255.0f,0.000001f);
}

TEST_F(TextureUploadGpu, EncodedRadianceUsesRealTypedPathAndGeneratesFloatMipWithoutClipping) {
  const std::string header="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
  auto encoded=bytes(std::span<const char>(header));
  for (const unsigned char value:{128,64,32,132,64,128,192,131}) encoded.push_back(std::byte(value));
  const auto uploaded=allocation->createTextureFromEncodedBytes("encoded-hdr",encoded,VK_FORMAT_R8G8B8A8_UNORM);
  EXPECT_EQ(uploaded.format,VK_FORMAT_R32G32B32A32_SFLOAT); ASSERT_EQ(uploaded.mipLevels,2u);
  const auto result=readback(uploaded);
  EXPECT_EQ(result[0].bytes,bytes(std::array<float,8>{8,4,2,1,2,4,6,1}));
  EXPECT_EQ(result[1].bytes,bytes(std::array<float,4>{5,4,4,1}));
}

TEST_F(TextureUploadGpu, RegeneratedSrgbAndLinearMipsUseDifferentColorSpaceAverages) {
  const std::array<unsigned char,16> texels{0,0,0,0,255,255,255,64,255,255,255,128,0,0,0,255};
  for (const auto format:{VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB}) {
    material::TextureImageData image; image.format=format; image.mips.push_back({2,2,bytes(texels)});
    const auto uploaded=allocation->createTextureFromImageData("generated-color",image); ASSERT_EQ(uploaded.mipLevels,2u);
    const auto result=readback(uploaded); EXPECT_EQ(result[0].bytes,bytes(texels)); ASSERT_EQ(result[1].bytes.size(),4u);
    for (size_t channel=0;channel<3;++channel)
      EXPECT_NEAR(std::to_integer<unsigned char>(result[1].bytes[channel]),format==VK_FORMAT_R8G8B8A8_SRGB?188:128,1);
    EXPECT_NEAR(std::to_integer<unsigned char>(result[1].bytes[3]),112,1);
  }
}

TEST_F(TextureUploadGpu, CompressedBlocksRequireLogicalFeatureAndPreserveEveryAuthoredMipByte) {
  material::TextureImageData image; image.format=VK_FORMAT_BC1_RGBA_UNORM_BLOCK; image.authoredMipChain=true;
  uint32_t width=5,height=7;
  for (unsigned level=0;level<3;++level) {
    std::vector<std::byte> payload(material::TextureMipByteSize(image.format,width,height));
    for (size_t i=0;i<payload.size();++i) payload[i]=std::byte((i*37+level*13)&255);
    image.mips.push_back({width,height,std::move(payload)}); width=std::max(width/2,1u); height=std::max(height/2,1u);
  }
  allocation->setTextureCompressionFeatures({});
  EXPECT_FALSE(supports(image.format));
  EXPECT_THROW((void)allocation->createTextureFromImageData("disabled-bc",image),std::runtime_error);
  allocation->setTextureCompressionFeatures(device->enabledFeatures());
  if (!supports(image.format)) GTEST_SKIP()<<"Logical device cannot enable BC1";
  exact(image);
}

TEST_F(TextureUploadGpu, FailedBatchRollsBackTypedAndArrayUploadsWithoutTouchingOldTexture) {
  const auto image=singleTexel(39);
  const auto old=allocation->createTextureFromImageData("old-owned",image);
  const auto baseline=liveAllocations();ASSERT_EQ(baseline,1u);
  try {
    auto batch=allocation->beginTextureAllocationBatch();
    (void)allocation->createTextureFromImageData("pending-typed",singleTexel(57));
    const std::array<std::byte,8> pixels{};
    (void)allocation->createTexture2DArrayFromRgbaPixels("pending-array",pixels,1,1,2);
    EXPECT_EQ(liveAllocations(),baseline+2);
    const std::array<std::byte,3> invalid{};
    (void)allocation->createTextureFromEncodedBytes("invalid-staged-input",invalid);
    FAIL()<<"Invalid encoded image unexpectedly loaded";
  } catch(const std::runtime_error&) {}
  EXPECT_EQ(liveAllocations(),baseline);
  EXPECT_EQ(readback(old)[0].bytes,image.mips[0].bytes);
}

TEST_F(TextureUploadGpu, CommittedLeaseRetainsHistoryResourcesUntilLastOwnerReleases) {
  const auto oldImage=singleTexel(31),newImage=singleTexel(79);
  const auto old=allocation->createTextureFromImageData("old-scene",oldImage);
  const auto baseline=liveAllocations();
  auto batch=allocation->beginTextureAllocationBatch();const auto id=batch.groupId();
  const auto staged=allocation->createTextureFromImageData("new-scene",newImage);
  uint32_t callbacks{};allocation->setLoadProgressCallback([&](bool){++callbacks;});
  const auto beforeCommit=liveAllocations();auto lease=batch.commit();
  EXPECT_EQ(lease->groupId(),id);EXPECT_FALSE(batch.active());EXPECT_EQ(batch.groupId(),0u);
  EXPECT_EQ(liveAllocations(),beforeCommit);EXPECT_EQ(callbacks,0u);
  auto activeGeneration=lease,historyGeneration=lease;lease.reset();activeGeneration.reset();
  allocation->setLoadProgressCallback({});
  EXPECT_EQ(liveAllocations(),baseline+1);EXPECT_EQ(readback(staged)[0].bytes,newImage.mips[0].bytes);
  callbacks=0;allocation->setLoadProgressCallback([&](bool){++callbacks;});
  // A raw TextureResource alias is deliberately not another ownership lease.
  const auto alias=staged;(void)alias;historyGeneration.reset();
  EXPECT_EQ(callbacks,0u);EXPECT_EQ(liveAllocations(),baseline);
  allocation->setLoadProgressCallback({});EXPECT_EQ(readback(old)[0].bytes,oldImage.mips[0].bytes);
}

TEST_F(TextureUploadGpu, MovedBatchRejectsNestedResetAndCleanupBeforeAnyMutation) {
  const auto image=singleTexel(67);const auto old=allocation->createTextureFromImageData("old-move",image);
  const auto baseline=liveAllocations();
  uint32_t callbacks{};
  {
    auto original=allocation->beginTextureAllocationBatch();const auto id=original.groupId();
    (void)allocation->createTextureFromImageData("move-pending",singleTexel(91));
    auto moved=std::move(original);EXPECT_FALSE(original.active());EXPECT_TRUE(moved.active());EXPECT_EQ(moved.groupId(),id);
    EXPECT_THROW((void)original.commit(),std::logic_error);
    allocation->setLoadProgressCallback([&](bool){++callbacks;});
    EXPECT_THROW((void)allocation->beginTextureAllocationBatch(),std::logic_error);
    EXPECT_THROW(allocation->resetTextureAllocations(),std::logic_error);
    EXPECT_THROW(allocation->resetTextureAllocations(TextureAllocationResetScope::All),std::logic_error);
    EXPECT_THROW(allocation->cleanup(),std::logic_error);
    EXPECT_EQ(callbacks,0u);EXPECT_EQ(liveAllocations(),baseline+1);
  }
  EXPECT_EQ(callbacks,0u);allocation->setLoadProgressCallback({});
  EXPECT_EQ(liveAllocations(),baseline);EXPECT_EQ(readback(old)[0].bytes,image.mips[0].bytes);
  auto next=allocation->beginTextureAllocationBatch();EXPECT_TRUE(next.active());
}

TEST_F(TextureUploadGpu, PersistentAllocationRejectsInsideBatchAndResetScopesRemainDistinct) {
  const std::array<std::byte,4> pixel{};
  (void)allocation->createTexture2DArrayFromRgbaPixels("persistent",pixel,1,1,1,
      VK_FORMAT_R8G8B8A8_UNORM,TextureAllocationLifetime::Persistent);
  const auto baseline=liveAllocations();ASSERT_EQ(baseline,1u);
  {
    auto batch=allocation->beginTextureAllocationBatch();
    EXPECT_THROW((void)allocation->createTexture2DArrayFromRgbaPixels("bad-persistent",pixel,1,1,1,
        VK_FORMAT_R8G8B8A8_UNORM,TextureAllocationLifetime::Persistent),std::logic_error);
    EXPECT_EQ(liveAllocations(),baseline);
    (void)allocation->createTextureFromImageData("pending-scene",singleTexel(1));
  }
  EXPECT_EQ(liveAllocations(),baseline);
  (void)allocation->createTextureFromImageData("ordinary-scene",singleTexel(2));
  allocation->resetTextureAllocations(TextureAllocationResetScope::SceneOnly);EXPECT_EQ(liveAllocations(),baseline);
  allocation->resetTextureAllocations(TextureAllocationResetScope::All);EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, ExplicitResetNeverLetsAStaleLeaseRetireANewerGroup) {
  auto firstBatch=allocation->beginTextureAllocationBatch();
  (void)allocation->createTextureFromImageData("first-generation",singleTexel(41));
  auto first=firstBatch.commit();const auto firstId=first->groupId();
  allocation->resetTextureAllocations();EXPECT_EQ(liveAllocations(),0u);
  auto secondBatch=allocation->beginTextureAllocationBatch();EXPECT_GT(secondBatch.groupId(),firstId);
  const auto image=singleTexel(103);const auto current=allocation->createTextureFromImageData("second-generation",image);
  auto second=secondBatch.commit();first.reset();
  EXPECT_EQ(liveAllocations(),1u);EXPECT_EQ(readback(current)[0].bytes,image.mips[0].bytes);
  second.reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, CancellingFromUploadCallbackCannotRegisterAnOrphanTexture) {
  const auto image=singleTexel(33);const auto old=allocation->createTextureFromImageData("old-before-cancel",image);
  const auto baseline=liveAllocations();
  std::optional<AllocationManager::TextureAllocationBatch> batch;
  batch.emplace(allocation->beginTextureAllocationBatch());
  allocation->setLoadProgressCallback([&](bool render){if(render) batch.reset();});
  EXPECT_THROW((void)allocation->createTextureFromImageData("cancel-during-upload",singleTexel(109)),std::logic_error);
  allocation->setLoadProgressCallback({});EXPECT_FALSE(batch.has_value());
  EXPECT_EQ(liveAllocations(),baseline);EXPECT_EQ(readback(old)[0].bytes,image.mips[0].bytes);
}

TEST_F(TextureUploadGpu, CancelledBatchCannotDestroyAllocatorDuringTypedOrArrayUpload) {
  const auto image=singleTexel(133);const auto old=allocation->createTextureFromImageData("before-callback-reset",image);
  const auto baseline=liveAllocations();auto* const memory=allocation->memoryManager();
  for(const bool array:{false,true}) {
    std::optional<AllocationManager::TextureAllocationBatch> batch;
    batch.emplace(allocation->beginTextureAllocationBatch());uint32_t completedCallbacks{};
    allocation->setLoadProgressCallback([&](bool completed) {
      if(!completed) return;
      ++completedCallbacks;batch.reset(); // Supported, nonthrowing cancellation.
      EXPECT_THROW(allocation->cleanup(),std::logic_error);
      EXPECT_THROW(allocation->resetTextureAllocations(),std::logic_error);
      EXPECT_THROW(allocation->resetTextureAllocations(TextureAllocationResetScope::All),std::logic_error);
      EXPECT_THROW(allocation->resetGeometryAllocations(),std::logic_error);
      EXPECT_THROW(allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),
          device->graphicsQueue(),*pool,{}),std::logic_error);
      EXPECT_EQ(allocation->memoryManager(),memory);
    });
    if(array) {
      const std::array<std::byte,8> pixels{};
      EXPECT_THROW((void)allocation->createTexture2DArrayFromRgbaPixels("cancel-array-reset",pixels,1,1,2),std::logic_error);
    } else {
      EXPECT_THROW((void)allocation->createTextureFromImageData("cancel-typed-reset",singleTexel(141)),std::logic_error);
    }
    allocation->setLoadProgressCallback({});EXPECT_GT(completedCallbacks,0u);EXPECT_FALSE(batch.has_value());
    EXPECT_EQ(liveAllocations(),baseline);EXPECT_EQ(readback(old)[0].bytes,image.mips[0].bytes);
  }
  // The operation guard must have unwound, rather than permanently blocking reset.
  allocation->resetTextureAllocations();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, PendingProgressExceptionRetiresSubmissionBeforeRollingBackImages) {
  const auto image=singleTexel(151);const auto old=allocation->createTextureFromImageData("before-pending-failure",image);
  const auto baseline=liveAllocations();
  std::optional<AllocationManager::TextureAllocationBatch> batch;
  batch.emplace(allocation->beginTextureAllocationBatch());
  (void)allocation->createTextureFromImageData("earlier-staged-image",singleTexel(157));
  auto* const memory=allocation->memoryManager();
  auto witness=memory->createBuffer(sizeof(uint32_t),VK_BUFFER_USAGE_TRANSFER_DST_BIT,VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
  auto semaphore=timelineGate();
  vk::raii::Fence fence(device->raii(),vk::FenceCreateInfo{});
  const auto gate=static_cast<VkSemaphore>(*semaphore);const auto blockerFence=static_cast<VkFence>(*fence);
  command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  constexpr uint32_t expected=0x4f2179b3u;
  vkCmdFillBuffer(*command,witness.buffer,0,sizeof(uint32_t),expected);
  VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  host.buffer=witness.buffer;host.offset=0;host.size=VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(*command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
  command.end();
  std::atomic<bool> callbackEntered{};std::atomic<VkResult> signalled{VK_NOT_READY};
  std::jthread signaler([&](std::stop_token stop) {
    std::stop_callback cancel(stop,[&]{callbackEntered.store(true);callbackEntered.notify_one();});
    callbackEntered.wait(false);
    if(!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    // Even an early test failure must open the gate before jthread joins.
    signalled.store(openGate(gate));
  });
  const VkCommandBuffer raw=*command;const uint64_t waitValue=1;
  const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&waitValue;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&timeline;
  submit.commandBufferCount=1;submit.pCommandBuffers=&raw;
  submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&gate;submit.pWaitDstStageMask=&waitStage;
  vk::raii::Queue queue(device->raii(),device->queueFamilyIndices().graphicsFamily.value(),0);
  const auto submitted=vkQueueSubmit(device->graphicsQueue(),1,&submit,blockerFence);
  if(submitted!=VK_SUCCESS) {
    signaler.request_stop();signaler.join();memory->destroyBuffer(witness);
    FAIL()<<"Could not submit pending upload witness: "<<submitted;
  }
  uint32_t pendingCallbacks{},completedCallbacks{};bool threwExpected{};
  allocation->setLoadProgressCallback([&](bool completed) {
    if(completed) {++completedCallbacks;return;}
    ++pendingCallbacks;EXPECT_EQ(vkGetFenceStatus(device->device(),blockerFence),VK_NOT_READY);
    callbackEntered.store(true);callbackEntered.notify_one();
    throw std::runtime_error("pending-progress-failure");
  });
  try {
    (void)allocation->createTextureFromImageData("pending-callback-throws",singleTexel(163));
  } catch(const std::runtime_error& error) {
    threwExpected=std::string(error.what())=="pending-progress-failure";
  }
  // Check before join/waitIdle: callback failure must retire the upload itself,
  // not defer safety to this test's cleanup or to batch rollback.
  if(pendingCallbacks) {
    EXPECT_EQ(vkGetFenceStatus(device->device(),blockerFence),VK_SUCCESS);
    EXPECT_EQ(gateValue(gate),1u);
  }
  signaler.request_stop();signaler.join();queue.waitIdle();allocation->setLoadProgressCallback({});
  EXPECT_TRUE(threwExpected);EXPECT_EQ(pendingCallbacks,1u);EXPECT_EQ(completedCallbacks,0u);
  EXPECT_EQ(signalled.load(),VK_SUCCESS);
  batch.reset();EXPECT_EQ(liveAllocations(),baseline+1u); // Only old image + witness buffer.
  EXPECT_EQ(vmaInvalidateAllocation(memory->allocator(),witness.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
  uint32_t actual{};std::memcpy(&actual,witness.allocation_info.pMappedData,sizeof(actual));EXPECT_EQ(actual,expected);
  memory->destroyBuffer(witness);EXPECT_EQ(liveAllocations(),baseline);
  EXPECT_EQ(readback(old)[0].bytes,image.mips[0].bytes);
}

TEST_F(TextureUploadGpu, LeaseAndUncommittedBatchMayOutliveTheirAllocatorSafely) {
  std::shared_ptr<const AllocationManager::TextureAllocationLease> lease;
  std::optional<AllocationManager::TextureAllocationBatch> pending;
  for(bool commit:{true,false}) {
    auto owner=std::make_unique<AllocationManager>();
    owner->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
    auto batch=owner->beginTextureAllocationBatch();
    (void)owner->createTextureFromImageData("short-lived-owner",singleTexel(53));
    if(commit) lease=batch.commit();else pending.emplace(std::move(batch));
    owner.reset();
  }
  EXPECT_FALSE(pending->active());EXPECT_THROW((void)pending->commit(),std::logic_error);
  pending.reset();lease.reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, DetachedCommandRecordingFailureNeverExecutesAndRemainsReusable) {
  auto& memory=*allocation->memoryManager();
  auto witness=memory.createBuffer(sizeof(uint32_t),VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|
      VMA_ALLOCATION_CREATE_MAPPED_BIT);
  constexpr uint32_t seed=0x13ba7c29u,expected=0x452c913bu;
  std::memcpy(witness.allocation_info.pMappedData,&seed,sizeof(seed));
  ASSERT_EQ(vmaFlushAllocation(memory.allocator(),witness.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
  const auto fill=[&](VkCommandBuffer command) {
    vkCmdFillBuffer(command,witness.buffer,0,sizeof(uint32_t),expected);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    host.buffer=witness.buffer;host.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,nullptr,1,&host,0,nullptr);
  };
  const auto failedRecorder=[&](VkCommandBuffer command) {
    fill(command);
    EXPECT_THROW(allocation->cleanup(),std::logic_error);
    EXPECT_THROW(allocation->resetGeometryAllocations(),std::logic_error);
    EXPECT_THROW(allocation->resetTextureAllocations(),std::logic_error);
    throw std::runtime_error("staged-record-failure");
  };
  EXPECT_THROW(allocation->executeStagedCommands(failedRecorder),std::runtime_error);
  ASSERT_EQ(vmaInvalidateAllocation(memory.allocator(),witness.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
  uint32_t actual{};std::memcpy(&actual,witness.allocation_info.pMappedData,sizeof(actual));
  EXPECT_EQ(actual,seed);
  ASSERT_NO_THROW(allocation->executeStagedCommands(fill));
  ASSERT_EQ(vmaInvalidateAllocation(memory.allocator(),witness.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
  std::memcpy(&actual,witness.allocation_info.pMappedData,sizeof(actual));EXPECT_EQ(actual,expected);
  memory.destroyBuffer(witness);
}

TEST_F(TextureUploadGpu, DetachedCommandCompletedCallbackFailureStillRetiresItsWrites) {
  auto& memory=*allocation->memoryManager();
  auto witness=memory.createBuffer(sizeof(uint32_t),VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|
      VMA_ALLOCATION_CREATE_MAPPED_BIT);
  constexpr uint32_t expected=0x358c4a67u;
  uint32_t completed{};
  allocation->setLoadProgressCallback([&](bool done) {
    if(done) {++completed;throw std::runtime_error("staged-completed-failure");}
  });
  EXPECT_THROW(allocation->executeStagedCommands([&](VkCommandBuffer command) {
    vkCmdFillBuffer(command,witness.buffer,0,sizeof(uint32_t),expected);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,nullptr,0,nullptr);
  }),std::runtime_error);
  allocation->setLoadProgressCallback({});
  EXPECT_EQ(completed,1u);
  ASSERT_EQ(vmaInvalidateAllocation(memory.allocator(),witness.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
  uint32_t actual{};std::memcpy(&actual,witness.allocation_info.pMappedData,sizeof(actual));EXPECT_EQ(actual,expected);
  memory.destroyBuffer(witness);
}

TEST_F(TextureUploadGpu, LastLeaseReleaseWaitsForAnAlreadySubmittedImageReader) {
  auto batch=allocation->beginTextureAllocationBatch();const auto image=singleTexel(117);
  const auto uploaded=allocation->createTextureFromImageData("in-flight-reader",image);auto lease=batch.commit();
  auto semaphore=timelineGate();
  const auto gate=static_cast<VkSemaphore>(*semaphore);std::atomic<bool> retirementStarted{};std::atomic<VkResult> signalled{VK_NOT_READY};
  std::jthread signaler([&](std::stop_token stop) {
    std::stop_callback cancel(stop,[&]{retirementStarted.store(true);retirementStarted.notify_one();});
    retirementStarted.wait(false);
    if(!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    signalled.store(openGate(gate));
  });
  const auto result=readback(uploaded,[&] {
    // The submitted command references the image but cannot execute until the
    // host-signalled timeline opens. Final lease release must wait before freeing it.
    retirementStarted.store(true);retirementStarted.notify_one();lease.reset();
    EXPECT_EQ(gateValue(gate),1u);
    signaler.join();EXPECT_EQ(signalled.load(),VK_SUCCESS);
  },gate);
  EXPECT_EQ(result[0].bytes,image.mips[0].bytes);EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, SelectedImageLeasesOutliveAggregateWhileUnpinnedSiblingsAreActuallyReclaimed) {
  auto batch=allocation->beginTextureAllocationBatch();
  const auto first=allocation->createTextureFromImageData("selected-first",singleTexel(31));
  const auto middle=allocation->createTextureFromImageData("unselected-middle",singleTexel(79));
  const auto last=allocation->createTextureFromImageData("selected-last",singleTexel(117));
  auto aggregate=batch.commit(),history=aggregate;
  auto firstPin=allocation->retainTextureImage(first),lastPin=allocation->retainTextureImage(last);
  EXPECT_EQ(firstPin->identity(),first.allocationIdentity);EXPECT_NE(first.allocationIdentity,last.allocationIdentity);
  aggregate.reset();EXPECT_EQ(liveAllocations(),3u);firstPin.reset();EXPECT_EQ(liveAllocations(),3u);
  firstPin=allocation->retainTextureImage(first);history.reset();EXPECT_EQ(liveAllocations(),2u);
  EXPECT_THROW((void)allocation->retainTextureImage(middle),std::invalid_argument);
  EXPECT_EQ(readback(first)[0].bytes,singleTexel(31).mips[0].bytes);EXPECT_EQ(readback(last)[0].bytes,singleTexel(117).mips[0].bytes);
  firstPin.reset();EXPECT_EQ(liveAllocations(),1u);EXPECT_EQ(readback(last)[0].bytes,singleTexel(117).mips[0].bytes);
  lastPin.reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, ColorDataImagesAndSamplerAliasesHaveIndependentImageOwnership) {
  auto dataImage=singleTexel(67),colorImage=dataImage;colorImage.format=VK_FORMAT_R8G8B8A8_SRGB;
  auto batch=allocation->beginTextureAllocationBatch();const auto color=allocation->createTextureFromImageData("color",colorImage);
  const auto data=allocation->createTextureFromImageData("data",dataImage);auto aggregate=batch.commit();
  auto alias=color;alias.name="Color with another sampler";alias.samplerIndex=7;alias.contentKey="sampler-alias";
  auto colorPin=allocation->retainTextureImage(color),dataPin=allocation->retainTextureImage(data),aliasPin=allocation->retainTextureImage(alias);
  EXPECT_EQ(alias.allocationIdentity,color.allocationIdentity);EXPECT_NE(color.allocationIdentity,data.allocationIdentity);
  EXPECT_EQ(aliasPin->identity(),colorPin->identity());aggregate.reset();EXPECT_EQ(liveAllocations(),2u);
  colorPin.reset();EXPECT_EQ(liveAllocations(),2u);dataPin.reset();EXPECT_EQ(liveAllocations(),1u);
  EXPECT_EQ(readback(alias)[0].bytes,colorImage.mips[0].bytes);aliasPin.reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, GroupZeroAdoptionRequiresEveryPublishedAliasToRetainItsOwnPinAndRejectsAba) {
  const auto uploaded=allocation->createTextureFromImageData("initial-group-zero",singleTexel(41));
  auto alias=uploaded;alias.name="initial alias";auto first=allocation->retainTextureImage(uploaded),second=allocation->retainTextureImage(alias);
  adoptImage(first);
  first.reset();EXPECT_EQ(liveAllocations(),1u);EXPECT_EQ(readback(alias)[0].bytes,singleTexel(41).mips[0].bytes);
  second.reset();EXPECT_EQ(liveAllocations(),0u);EXPECT_THROW((void)allocation->retainTextureImage(uploaded),std::invalid_argument);
  const auto replacement=allocation->createTextureFromImageData("new-incarnation",singleTexel(99));
  EXPECT_NE(uploaded.allocationIdentity,replacement.allocationIdentity);EXPECT_GT(replacement.allocationIdentity.incarnation(),uploaded.allocationIdentity.incarnation());
  auto stale=uploaded;stale.image=replacement.image;stale.imageView=replacement.imageView; // Simulated handle ABA keeps the stale capability.
  EXPECT_THROW((void)allocation->retainTextureImage(stale),std::invalid_argument);
  auto replacementPin=allocation->retainTextureImage(replacement);adoptImage(replacementPin);EXPECT_EQ(readback(replacement)[0].bytes,singleTexel(99).mips[0].bytes);
  replacementPin.reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, ImagePinRejectsActiveUncommittedMockForeignAndMismatchedCapabilities) {
  material::TextureResource uploaded;
  {
    auto batch=allocation->beginTextureAllocationBatch();uploaded=allocation->createTextureFromImageData("uncommitted",singleTexel(53));
    EXPECT_THROW((void)allocation->retainTextureImage(uploaded),std::logic_error);EXPECT_EQ(liveAllocations(),1u);
  }
  EXPECT_EQ(liveAllocations(),0u);EXPECT_THROW((void)allocation->retainTextureImage(uploaded),std::invalid_argument);
  const auto live=allocation->createTextureFromImageData("live",singleTexel(61));auto pin=allocation->retainTextureImage(live);adoptImage(pin);
  material::TextureResource mock;mock.image=live.image;mock.imageView=live.imageView;
  EXPECT_THROW((void)allocation->retainTextureImage(mock),std::invalid_argument);
  auto other=std::make_unique<AllocationManager>();other->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
  const auto foreign=other->createTextureFromImageData("foreign",singleTexel(83));
  EXPECT_THROW((void)allocation->retainTextureImage(foreign),std::invalid_argument);
  EXPECT_THROW((void)other->retainTextureImage(live),std::invalid_argument);
  auto mismatched=live;mismatched.imageView=foreign.imageView;EXPECT_THROW((void)allocation->retainTextureImage(mismatched),std::invalid_argument);
  mismatched=live;mismatched.image=foreign.image;EXPECT_THROW((void)allocation->retainTextureImage(mismatched),std::invalid_argument);
  {auto active=allocation->beginTextureAllocationBatch();EXPECT_THROW((void)allocation->retainTextureImage(live),std::logic_error);}
  other.reset();EXPECT_EQ(readback(live)[0].bytes,singleTexel(61).mips[0].bytes);pin.reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, DescriptorImageProofIsNonOwningAllocationFreeAndRejectsStaleForeignOrActiveUploads) {
  const auto live=allocation->createTextureFromImageData("descriptor-proof",singleTexel(61));
  ASSERT_TRUE(allocation->textureImageMatches(live));
  auto alias=live;alias.name="descriptor alias";alias.samplerIndex=9;
  bool accepted{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);accepted=allocation->textureImageMatches(alias);attempts=guard.allocations();}
  EXPECT_TRUE(accepted);EXPECT_EQ(attempts,0u);EXPECT_EQ(liveAllocations(),1u);
  material::TextureResource forged;forged.image=live.image;forged.imageView=live.imageView;
  EXPECT_FALSE(allocation->textureImageMatches(forged));
  auto other=std::make_unique<AllocationManager>();
  other->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
  const auto foreign=other->createTextureFromImageData("foreign-descriptor",singleTexel(83));
  EXPECT_FALSE(allocation->textureImageMatches(foreign));EXPECT_FALSE(other->textureImageMatches(live));
  auto mismatched=live;mismatched.imageView=foreign.imageView;EXPECT_FALSE(allocation->textureImageMatches(mismatched));
  mismatched=live;mismatched.image=foreign.image;EXPECT_FALSE(allocation->textureImageMatches(mismatched));
  material::TextureResource abandoned;
  {auto batch=allocation->beginTextureAllocationBatch();
    abandoned=allocation->createTextureFromImageData("unpublished-descriptor",singleTexel(47));
    EXPECT_FALSE(allocation->textureImageMatches(live));EXPECT_FALSE(allocation->textureImageMatches(abandoned));}
  EXPECT_FALSE(allocation->textureImageMatches(abandoned));EXPECT_TRUE(allocation->textureImageMatches(live));
  allocation->resetTextureAllocations();
  EXPECT_FALSE(allocation->textureImageMatches(live));EXPECT_FALSE(allocation->textureImageMatches(alias));
  const auto replacement=allocation->createTextureFromImageData("new-descriptor-incarnation",singleTexel(99));
  auto stale=live;stale.image=replacement.image;stale.imageView=replacement.imageView;
  EXPECT_FALSE(allocation->textureImageMatches(stale));EXPECT_TRUE(allocation->textureImageMatches(replacement));
  EXPECT_EQ(readback(replacement)[0].bytes,singleTexel(99).mips[0].bytes);
}

TEST_F(TextureUploadGpu, PersistentDefaultsCannotBeAccidentallyAdoptedAndForceResetInvalidatesImagePins) {
  const std::array<std::byte,4> pixel{};const auto persistent=allocation->createTexture2DArrayFromRgbaPixels("persistent-default",pixel,1,1,1,
      VK_FORMAT_R8G8B8A8_UNORM,TextureAllocationLifetime::Persistent);
  const auto initial=allocation->createTextureFromImageData("reset-initial",singleTexel(71));auto initialPin=allocation->retainTextureImage(initial);adoptImage(initialPin);
  auto batch=allocation->beginTextureAllocationBatch();const auto staged=allocation->createTextureFromImageData("reset-group",singleTexel(79));
  auto aggregate=batch.commit();auto stagedPin=allocation->retainTextureImage(staged);
  material::TextureResource defaultAlias;defaultAlias.image=persistent.image;defaultAlias.imageView=persistent.imageView;
  EXPECT_THROW((void)allocation->retainTextureImage(defaultAlias),std::invalid_argument);
  defaultAlias.allocationIdentity=initial.allocationIdentity;EXPECT_THROW((void)allocation->retainTextureImage(defaultAlias),std::invalid_argument);
  allocation->resetTextureAllocations();EXPECT_EQ(liveAllocations(),1u);
  EXPECT_THROW((void)allocation->retainTextureImage(initial),std::invalid_argument);
  EXPECT_THROW((void)allocation->retainTextureImage(staged),std::invalid_argument);
  const auto next=allocation->createTextureFromImageData("after-reset",singleTexel(101));auto nextPin=allocation->retainTextureImage(next);adoptImage(nextPin);
  initialPin.reset();stagedPin.reset();aggregate.reset();EXPECT_EQ(liveAllocations(),2u);EXPECT_EQ(readback(next)[0].bytes,singleTexel(101).mips[0].bytes);
  nextPin.reset();EXPECT_EQ(liveAllocations(),1u);allocation->resetTextureAllocations(TextureAllocationResetScope::All);EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, ImagePinsCanOutliveAllocatorAndPreparedPinExchangeDoesNotAllocate) {
  std::shared_ptr<const AllocationManager::TextureImageAllocationLease> escaped;
  {
    auto owner=std::make_unique<AllocationManager>();owner->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
    const auto image=owner->createTextureFromImageData("short-image-owner",singleTexel(43));escaped=owner->retainTextureImage(image);
    owner.reset();EXPECT_FALSE(escaped->identity());
  }
  escaped.reset();
  const auto image=allocation->createTextureFromImageData("pin-publication",singleTexel(59));auto prepared=allocation->retainTextureImage(image);
  std::shared_ptr<const AllocationManager::TextureImageAllocationLease> published;
  size_t attempts{};bool adopted{};{material_publication_test::AllocationGuard guard(true);adopted=allocation->adoptTextureImages(std::span(&prepared,1));published.swap(prepared);attempts=guard.allocations();}
  EXPECT_TRUE(adopted);EXPECT_EQ(attempts,0u);EXPECT_TRUE(published);EXPECT_FALSE(prepared);EXPECT_EQ(readback(image)[0].bytes,singleTexel(59).mips[0].bytes);
  published.reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, TextureManagerRetirementLeavesFallbackSlotsAndErasesCacheAliasesWithoutRetargeting) {
  const auto image=allocation->createTextureFromImageData("cache-image",singleTexel(47));auto pin=allocation->retainTextureImage(image);adoptImage(pin);
  auto first=image;first.name="first-name";first.contentKey="same-content";auto alias=first;alias.name="sampler-name";alias.samplerIndex=9;
  material::TextureManager manager;const auto a=manager.registerTexture(first),b=manager.registerTexture(alias);
  ASSERT_NE(a,b);EXPECT_EQ(manager.findTextureImage("same-content"),manager.getTexture(a));
  const material::TextureResource fallback{};size_t attempts{};bool retired{};
  {material_publication_test::AllocationGuard guard(true);retired=manager.retireTexture(a);attempts=guard.allocations();}
  EXPECT_TRUE(retired);EXPECT_EQ(attempts,0u);EXPECT_EQ(manager.textureCount(),2u);EXPECT_EQ(manager.activeTextureCount(),1u);
  EXPECT_FALSE(manager.isTextureActive(a));EXPECT_EQ(manager.getTexture(a),nullptr);EXPECT_FALSE(manager.findTextureIndex("first-name"));
  EXPECT_EQ(manager.findTextureImage("same-content"),nullptr);EXPECT_EQ(manager.findTextureIndex("sampler-name"),std::optional<uint32_t>(b));
  EXPECT_EQ(&manager.getTextureOrFallback(a,fallback),&fallback);EXPECT_EQ(&manager.getTextureOrFallback(b,fallback),manager.getTexture(b));
  auto next=alias;next.name="new-explicit-alias";next.contentKey="new-content";const auto c=manager.registerTexture(next);EXPECT_GT(c,b);
  auto snapshot=manager;EXPECT_FALSE(snapshot.isTextureActive(a));EXPECT_EQ(snapshot.activeTextureCount(),2u);
  EXPECT_TRUE(manager.retireTexture(b));EXPECT_FALSE(manager.retireTexture(b));EXPECT_TRUE(manager.retireTexture(c));EXPECT_EQ(manager.activeTextureCount(),0u);
  EXPECT_EQ(manager.findTextureImage("same-content"),nullptr);EXPECT_EQ(manager.findTextureImage("new-content"),nullptr);
  EXPECT_EQ(liveAllocations(),1u); // Non-owning cache rows never free a reader's image pin.
  pin.reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, LastIndependentImagePinWaitsForAnAlreadySubmittedReader) {
  const auto payload=singleTexel(123);const auto image=allocation->createTextureFromImageData("independent-reader",payload);
  auto pin=allocation->retainTextureImage(image);adoptImage(pin);auto semaphore=timelineGate();const auto gate=static_cast<VkSemaphore>(*semaphore);
  std::atomic<bool> retirementStarted{};std::atomic<VkResult> signalled{VK_NOT_READY};
  std::jthread signaler([&](std::stop_token stop) {
    std::stop_callback cancel(stop,[&]{retirementStarted.store(true);retirementStarted.notify_one();});retirementStarted.wait(false);
    if(!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(25));signalled.store(openGate(gate));
  });
  const auto result=readback(image,[&] {retirementStarted.store(true);retirementStarted.notify_one();pin.reset();EXPECT_EQ(gateValue(gate),1u);signaler.join();EXPECT_EQ(signalled.load(),VK_SUCCESS);},gate);
  EXPECT_EQ(result[0].bytes,payload.mips[0].bytes);EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, AbandonedOrAllocationFailingGroupZeroPinPreparationPreservesThePublishedImages) {
  const auto first=allocation->createTextureFromImageData("bootstrap-first",singleTexel(43));
  const auto second=allocation->createTextureFromImageData("bootstrap-second",singleTexel(83));
  auto firstPin=allocation->retainTextureImage(first);bool failed{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);try {(void)allocation->retainTextureImage(second);}catch(const std::bad_alloc&){failed=true;}attempts=guard.allocations();}
  EXPECT_TRUE(failed);EXPECT_GT(attempts,0u);firstPin.reset();EXPECT_EQ(liveAllocations(),2u);
  EXPECT_EQ(readback(first)[0].bytes,singleTexel(43).mips[0].bytes);EXPECT_EQ(readback(second)[0].bytes,singleTexel(83).mips[0].bytes);
  {
    auto abandonedFirst=allocation->retainTextureImage(first),abandonedSecond=allocation->retainTextureImage(second);
    const std::array pins{abandonedFirst,abandonedSecond};EXPECT_TRUE(allocation->canAdoptTextureImages(pins));
  }
  EXPECT_EQ(liveAllocations(),2u);EXPECT_EQ(readback(first)[0].bytes,singleTexel(43).mips[0].bytes);
  std::array pins{allocation->retainTextureImage(first),allocation->retainTextureImage(second)};bool can{},adopted{};
  {material_publication_test::AllocationGuard guard(true);can=allocation->canAdoptTextureImages(pins);adopted=allocation->adoptTextureImages(pins);attempts=guard.allocations();}
  EXPECT_TRUE(can);EXPECT_TRUE(adopted);EXPECT_EQ(attempts,0u);EXPECT_FALSE(allocation->canAdoptTextureImages(pins));
  pins[0].reset();EXPECT_EQ(liveAllocations(),1u);EXPECT_EQ(readback(second)[0].bytes,singleTexel(83).mips[0].bytes);
  pins[1].reset();EXPECT_EQ(liveAllocations(),0u);
}

TEST_F(TextureUploadGpu, GroupZeroAdoptionRejectsMixedDuplicateUnsortedForeignAndStaleListsAtomically) {
  const auto first=allocation->createTextureFromImageData("atomic-adopt-first",singleTexel(47));
  const auto second=allocation->createTextureFromImageData("atomic-adopt-second",singleTexel(79));
  auto a=allocation->retainTextureImage(first),b=allocation->retainTextureImage(second);
  {const std::array duplicate{a,a},unsorted{b,a};EXPECT_FALSE(allocation->adoptTextureImages(duplicate));EXPECT_FALSE(allocation->adoptTextureImages(unsorted));}
  EXPECT_FALSE(allocation->adoptTextureImages({}));
  auto batch=allocation->beginTextureAllocationBatch();const auto grouped=allocation->createTextureFromImageData("not-group-zero",singleTexel(97));auto aggregate=batch.commit();
  auto groupedPin=allocation->retainTextureImage(grouped);{const std::array mixed{a,groupedPin};EXPECT_FALSE(allocation->adoptTextureImages(mixed));}
  auto foreign=std::make_unique<AllocationManager>();foreign->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
  const auto foreignImage=foreign->createTextureFromImageData("foreign-adoption",singleTexel(103));auto foreignPin=foreign->retainTextureImage(foreignImage);
  {const std::array wrongOwner{a,foreignPin};EXPECT_FALSE(allocation->adoptTextureImages(wrongOwner));}
  a.reset();b.reset();EXPECT_EQ(liveAllocations(),3u); // Failed lists never released the two reset-only owners.
  auto validA=allocation->retainTextureImage(first),validB=allocation->retainTextureImage(second);const std::array valid{validA,validB};
  EXPECT_TRUE(allocation->adoptTextureImages(valid));allocation->resetTextureAllocations();EXPECT_FALSE(allocation->adoptTextureImages(valid));
  EXPECT_EQ(liveAllocations(),0u);foreign.reset();foreignPin.reset();groupedPin.reset();aggregate.reset();
}

TEST_F(TextureUploadGpu, FailedPartialBatchPinMigrationKeepsTheOldAggregateOwnerAndEverySiblingAlive) {
  auto batch=allocation->beginTextureAllocationBatch();const auto first=allocation->createTextureFromImageData("migration-first",singleTexel(37));
  const auto second=allocation->createTextureFromImageData("migration-second",singleTexel(73));auto aggregate=batch.commit();
  auto candidate=allocation->retainTextureImage(first);bool failed{};
  {material_publication_test::AllocationGuard guard(true);try {(void)allocation->retainTextureImage(second);}catch(const std::bad_alloc&){failed=true;}}
  EXPECT_TRUE(failed);candidate.reset();EXPECT_EQ(liveAllocations(),2u);
  EXPECT_EQ(readback(first)[0].bytes,singleTexel(37).mips[0].bytes);EXPECT_EQ(readback(second)[0].bytes,singleTexel(73).mips[0].bytes);
  aggregate.reset();EXPECT_EQ(liveAllocations(),0u);
}
} // namespace
