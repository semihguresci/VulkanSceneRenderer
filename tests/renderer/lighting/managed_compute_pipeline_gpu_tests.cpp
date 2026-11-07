#include "Container/common/CommonGLFW.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

namespace {
using namespace container::gpu;
class ManagedComputeGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<ManagedComputeGpu*>(user)->errors;std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const char* enabled=std::getenv("CONTAINER_RUN_GPU_MANAGED_COMPUTE");
    if (!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_MANAGED_COMPUTE=1";
    ASSERT_EQ(glfwInit(),GLFW_TRUE);started=true;glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    window=glfwCreateWindow(32,32,"Managed compute lifecycle",nullptr,nullptr);ASSERT_NE(window,nullptr);
    uint32_t count{};const auto extensions=glfwGetRequiredInstanceExtensions(&count);
    InstanceCreateInfo info;info.enableValidationLayers=true;info.validationLayers={"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions,extensions+count);info.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};validation.enabledValidationFeatureCount=1;validation.pEnabledValidationFeatures=&sync;info.next=&validation;
    instance=std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;debug.messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation|vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback=reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(callback);debug.pUserData=this;
    messenger=vk::raii::DebugUtilsMessengerEXT(instance->raii(),debug);
    VkSurfaceKHR raw{};ASSERT_EQ(glfwCreateWindowSurface(instance->instance(),window,nullptr,&raw),VK_SUCCESS);surface=vk::raii::SurfaceKHR(instance->raii(),raw);
    DeviceCreateInfo create;create.requiredExtensions={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};v12.bufferDeviceAddress=VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};v13.synchronization2=VK_TRUE;v13.pNext=&v12;create.next=&v13;
    device=std::make_unique<VulkanDevice>(*instance,raw,create);memory=std::make_unique<VulkanMemoryManager>(instance->instance(),device->physicalDevice(),device->device());
    manager=std::make_unique<PipelineManager>(device->device());
    pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,device->queueFamilyIndices().graphicsFamily.value()));
    auto commands=device->raii().allocateCommandBuffers(vk::CommandBufferAllocateInfo(*pool,vk::CommandBufferLevel::ePrimary,1));command=std::move(commands.front());
    const vk::DescriptorSetLayoutBinding binding(0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute);
    descriptorLayout=vk::raii::DescriptorSetLayout(device->raii(),vk::DescriptorSetLayoutCreateInfo({},binding));
    layout=vk::raii::PipelineLayout(device->raii(),vk::PipelineLayoutCreateInfo({},*descriptorLayout));
    scratch=std::filesystem::path(CONTAINER_MANAGED_COMPUTE_A_SPIRV).parent_path()/
        ("managed-lifecycle-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for(const auto& folder:{"original","replacement"}) std::filesystem::create_directories(scratch/folder/"spv_shaders");
  }
  void TearDown() override {
    if (device) device->raii().waitIdle();
    command.clear();manager.reset();layout.clear();descriptorLayout.clear();pool.clear();memory.reset();device.reset();surface.clear();messenger.clear();instance.reset();
    if (window) glfwDestroyWindow(window);if (started) glfwTerminate();EXPECT_EQ(errors.load(),0u);
  }
  std::filesystem::path source(const char* name) {
    auto path=scratch/"original"/"spv_shaders"/name;
    std::filesystem::copy_file(CONTAINER_MANAGED_COMPUTE_A_SPIRV,path);return path;
  }
  void replacement(const char* name) {
    std::filesystem::copy_file(CONTAINER_MANAGED_COMPUTE_B_SPIRV,scratch/"replacement"/"spv_shaders"/name,
        std::filesystem::copy_options::overwrite_existing);
  }
  uint32_t run(VkPipeline pipeline) {
    auto buffer=memory->createBuffer(sizeof(uint32_t),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    uint32_t zero=0;std::memcpy(buffer.allocation_info.pMappedData,&zero,sizeof(zero));
    EXPECT_EQ(vmaFlushAllocation(memory->allocator(),buffer.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
    const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer,1);
    vk::raii::DescriptorPool descriptors(device->raii(),vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,1,size));
    auto sets=device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors,*descriptorLayout));
    const vk::DescriptorBufferInfo binding(buffer.buffer,0,sizeof(uint32_t));
    device->raii().updateDescriptorSets(vk::WriteDescriptorSet(*sets.front(),0,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&binding),{});
    command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::MemoryBarrier2 before(vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostWrite,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite);
    vk::DependencyInfo dependency;dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&before;command.pipelineBarrier2(dependency);
    command.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline);command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*layout,0,*sets.front(),{});command.dispatch(1,1,1);
    vk::MemoryBarrier2 after(vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead);
    dependency.pMemoryBarriers=&after;command.pipelineBarrier2(dependency);command.end();
    const vk::CommandBuffer handle=*command;vk::SubmitInfo submit;submit.commandBufferCount=1;submit.pCommandBuffers=&handle;
    vk::raii::Queue queue(device->raii(),device->queueFamilyIndices().graphicsFamily.value(),0);queue.submit(submit,nullptr);queue.waitIdle();
    EXPECT_EQ(vmaInvalidateAllocation(memory->allocator(),buffer.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
    uint32_t value{};std::memcpy(&value,buffer.allocation_info.pMappedData,sizeof(value));command.reset();memory->destroyBuffer(buffer);return value;
  }
  std::atomic_uint errors{};bool started{};GLFWwindow* window{};
  std::unique_ptr<VulkanInstance> instance;vk::raii::DebugUtilsMessengerEXT messenger{nullptr};vk::raii::SurfaceKHR surface{nullptr};
  std::unique_ptr<VulkanDevice> device;std::unique_ptr<VulkanMemoryManager> memory;std::unique_ptr<PipelineManager> manager;
  vk::raii::CommandPool pool{nullptr};vk::raii::CommandBuffer command{nullptr};
  vk::raii::DescriptorSetLayout descriptorLayout{nullptr};vk::raii::PipelineLayout layout{nullptr};std::filesystem::path scratch;
};
TEST_F(ManagedComputeGpu, PreparedReloadChangesExecutionOnlyAfterPublicationAndUpdatesSharedSlots) {
  const auto file=source("one.comp.spv");auto owner=manager->createComputePipeline(file,*layout,"one");auto shared=owner.share();
  const auto identity=owner.contentIdentity();ASSERT_TRUE(identity);EXPECT_EQ(shared.contentIdentity(),identity);
  EXPECT_EQ(run(owner),17u);replacement("one.comp.spv");const VkPipeline old=owner;
  auto reload=manager->prepareComputeReload(scratch/"replacement");EXPECT_EQ(reload.size(),1u);EXPECT_TRUE(reload.changesShader("one.comp.spv"));
  EXPECT_EQ(VkPipeline(owner),old);EXPECT_EQ(run(owner),17u);EXPECT_EQ(owner.contentIdentity(),identity);
  manager->publishComputeReload(reload);EXPECT_NE(VkPipeline(owner),old);EXPECT_EQ(VkPipeline(owner),VkPipeline(shared));EXPECT_EQ(run(shared),29u);
  EXPECT_NE(owner.contentIdentity(),identity);EXPECT_EQ(shared.contentIdentity(),owner.contentIdentity());
  auto unchanged=manager->prepareComputeReload(scratch/"replacement");EXPECT_EQ(unchanged.size(),0u);
  auto newlyCreated=manager->createComputePipeline(file,*layout,"new-after-reload");EXPECT_EQ(run(newlyCreated),29u);
}
TEST_F(ManagedComputeGpu, PartialReloadFailureKeepsEveryLivePipelineAndItsExecutionUnchanged) {
  auto first=manager->createComputePipeline(source("first.comp.spv"),*layout,"first");
  auto second=manager->createComputePipeline(source("second.comp.spv"),*layout,"second");
  const VkPipeline oldFirst=first,oldSecond=second;replacement("first.comp.spv");
  EXPECT_THROW((void)manager->prepareComputeReload(scratch/"replacement"),std::runtime_error);
  EXPECT_EQ(VkPipeline(first),oldFirst);EXPECT_EQ(VkPipeline(second),oldSecond);EXPECT_EQ(run(first),17u);EXPECT_EQ(run(second),17u);
  replacement("second.comp.spv");auto complete=manager->prepareComputeReload(scratch/"replacement");EXPECT_EQ(complete.size(),2u);
  manager->publishComputeReload(complete);EXPECT_EQ(run(first),29u);EXPECT_EQ(run(second),29u);
}
TEST_F(ManagedComputeGpu, SharedOwnersCanRetireBeforeOrAfterManagerWithoutDoubleDestruction) {
  auto owner=manager->createComputePipeline(source("shared.comp.spv"),*layout,"shared");auto shared=owner.share();
  owner.reset();EXPECT_FALSE(owner.contentIdentity());EXPECT_EQ(run(shared),17u);manager.reset();EXPECT_EQ(VkPipeline(shared),VK_NULL_HANDLE);EXPECT_FALSE(shared.contentIdentity());shared.reset();
}
TEST_F(ManagedComputeGpu, PublishedContentIdentitiesRemainDistinctAcrossRepeatedAndAbandonedReloads) {
  const auto file=source("identity.comp.spv");auto owner=manager->createComputePipeline(file,*layout,"identity");
  const auto original=owner.contentIdentity();ASSERT_TRUE(original);
  replacement("identity.comp.spv");auto first=manager->prepareComputeReload(scratch/"replacement");
  manager->publishComputeReload(first);const auto middle=owner.contentIdentity();
  EXPECT_NE(middle,original);EXPECT_EQ(run(owner),29u);
  std::filesystem::copy_file(file,scratch/"replacement"/"spv_shaders"/file.filename(),std::filesystem::copy_options::overwrite_existing);
  auto second=manager->prepareComputeReload(scratch/"replacement");manager->publishComputeReload(second);
  const auto final=owner.contentIdentity();EXPECT_NE(final,original);EXPECT_NE(final,middle);EXPECT_EQ(run(owner),17u);
  replacement("identity.comp.spv");{
    auto abandoned=manager->prepareComputeReload(scratch/"replacement");EXPECT_EQ(abandoned.size(),1u);
    EXPECT_EQ(owner.contentIdentity(),final);EXPECT_EQ(run(owner),17u);
  }
  EXPECT_EQ(owner.contentIdentity(),final);EXPECT_TRUE(original);EXPECT_TRUE(middle);
}
} // namespace
