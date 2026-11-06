#include <Container/common/CommonGLFW.h>
#include <Container/renderer/scene/GpuHierarchyPass.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneData.h>
#include <Container/utility/VulkanDevice.h>
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace {
using namespace container::gpu;
using namespace container::renderer;

glm::mat4 fixturePreviousModel() {
  glm::mat4 result(1);result[0]={1,.2f,.3f,0};result[1]={0,2,.4f,0};
  result[2]={0,0,-3,0};result[3]={13,7,-4,1};return result;
}
glm::mat4 fixtureDiagnosticModel() {
  glm::mat4 result(1);result[0]={1,.1f,0,0};result[1]={0,2,.3f,0};
  result[2]={.2f,0,3,0};result[3]={17,-3,5,1};return result;
}
std::vector<ObjectData> fixtureMetadata(size_t count) {
  std::vector<ObjectData> initial(count);
  for(size_t i=0;i<count;++i) {
    auto& object=initial[i];object.model=fixtureDiagnosticModel();object.previousModel=fixturePreviousModel();
    object.normalMatrix0=glm::vec4(10);object.normalMatrix1=glm::vec4(11);object.normalMatrix2=glm::vec4(12);
    object.objectInfo=glm::uvec4(uint32_t(i),42,43,44);object.temporalInfo=glm::uvec4(3,1,4,5);
    object.boundingSphere=glm::vec4(1,2,3,4);
  }
  return initial;
}

class GpuHierarchyGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<GpuHierarchyGpu*>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const char* enabled = std::getenv("CONTAINER_RUN_GPU_HIERARCHY");
    if (!enabled || std::strcmp(enabled, "1") != 0) {
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_HIERARCHY=1 for production Vulkan hierarchy checks";
    }
    const char* shaderOverride = std::getenv("CONTAINER_HIERARCHY_SHADER_DIR");
    shaderDir = shaderOverride ? shaderOverride : CONTAINER_HIERARCHY_SHADER_DIR;
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderDir / "spv_shaders" / "hierarchy_transform.comp.spv"));
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
    pass = std::make_unique<GpuHierarchyPass>(device, *memory, *pipelines);
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
    if(memory) for(auto& destination:destinations) memory->destroyBuffer(destination);
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




  struct Result { std::vector<glm::mat4> worlds; std::vector<ObjectData> objects; double gpuMs{}; };
  Result run(uint32_t image, std::span<const HierarchyLocalTransform> nodes,
             std::span<const uint32_t> mapping,const std::vector<glm::mat4>* cpuWorlds=nullptr,
             const std::vector<ObjectData>* metadataOverride=nullptr) {
    const auto objectBytes=mapping.size()*sizeof(ObjectData);
    auto& destination=destinations.at(image);
    memory->destroyBuffer(destination);
    destination=memory->createBuffer(std::max<size_t>(objectBytes,16),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    auto initial=metadataOverride?*metadataOverride:fixtureMetadata(mapping.size());
    if(cpuWorlds) for(size_t i=0;i<mapping.size();++i) if(mapping[i]!=UINT32_MAX)
      initial[i].boundingSphere=glm::vec4(glm::vec3(cpuWorlds->at(mapping[i])[3]),4);
    // Every output byte starts poisoned; compute must publish full records.
    std::memset(destination.allocation_info.pMappedData,0xcd,objectBytes);
    ASSERT_VK(vmaFlushAllocation(memory->allocator(),destination.allocation,0,VK_WHOLE_SIZE));
    pass->prepareFrame(image,nodes,mapping,{destination.buffer,0,objectBytes});
    EXPECT_THROW(pass->record(*command,image),std::invalid_argument);
    pass->uploadObjectMetadata(image,initial);
    const auto output=pass->worldMatrices(image);
    auto readback=memory->createBuffer(output.size+objectBytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    VkQueryPool query{};
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;
    ASSERT_VK(vkCreateQueryPool(device->device(),&qi,nullptr,&query));
    command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vkCmdResetQueryPool(*command,query,0,2);
    vkCmdWriteTimestamp(*command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,query,0);
    pass->record(*command,image);
    vkCmdWriteTimestamp(*command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,1);
    const VkBufferCopy worldsCopy{0,0,output.size}, objectsCopy{0,output.size,objectBytes};
    vkCmdCopyBuffer(*command,output.buffer,readback.buffer,1,&worldsCopy);
    vkCmdCopyBuffer(*command,destination.buffer,readback.buffer,1,&objectsCopy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(*command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    command.end();
    const vk::CommandBuffer handle=*command;vk::SubmitInfo submit;submit.commandBufferCount=1;submit.pCommandBuffers=&handle;
    vk::raii::Queue queue(device->raii(),device->queueFamilyIndices().graphicsFamily.value(),0);
    queue.submit(submit,nullptr);pass->commitFrame(image);queue.waitIdle();
    ASSERT_VK(vmaInvalidateAllocation(memory->allocator(),readback.allocation,0,VK_WHOLE_SIZE));
    std::array<uint64_t,2> ticks{};
    ASSERT_VK(vkGetQueryPoolResults(device->device(),query,0,2,sizeof(ticks),ticks.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT));
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(device->physicalDevice(),&properties);
    Result result{std::vector<glm::mat4>(nodes.size()),std::vector<ObjectData>(mapping.size()),
                  double(ticks[1]-ticks[0])*properties.limits.timestampPeriod/1e6};
    std::memcpy(result.worlds.data(),readback.allocation_info.pMappedData,size_t(output.size));
    std::memcpy(result.objects.data(),static_cast<const std::byte*>(readback.allocation_info.pMappedData)+output.size,objectBytes);
    vkDestroyQueryPool(device->device(),query,nullptr);memory->destroyBuffer(readback);
    return result;
  }
  static void ASSERT_VK(VkResult result) { if(result!=VK_SUCCESS) throw std::runtime_error("Vulkan fixture operation failed"); }
  void matrixNear(const glm::mat4& actual,const glm::mat4& expected) {
    for(int c=0;c<4;++c) for(int r=0;r<4;++r)
      EXPECT_NEAR(actual[c][r],expected[c][r],3e-4f*std::max(1.f,std::abs(expected[c][r])));
  }
  std::vector<glm::mat4> reference(std::span<const HierarchyLocalTransform> nodes) {
    std::vector<glm::mat4> result(nodes.size());std::vector<bool> done(nodes.size());
    size_t count=0;
    while(count<nodes.size()) for(size_t i=0;i<nodes.size();++i)
      if(!done[i] && (nodes[i].parent==UINT32_MAX || done[nodes[i].parent])) {
        result[i]=(nodes[i].parent==UINT32_MAX?glm::mat4(1):result[nodes[i].parent])*nodes[i].local;done[i]=true;++count;
      }
    return result;
  }
  Result verify(uint32_t image,const std::vector<HierarchyLocalTransform>& nodes,const std::vector<uint32_t>& mapping) {
    const auto expected=reference(nodes);const auto result=run(image,nodes,mapping);
    for(size_t i=0;i<nodes.size();++i) {SCOPED_TRACE(i);matrixNear(result.worlds[i],expected[i]);}
    for(size_t i=0;i<mapping.size();++i) {
      SCOPED_TRACE(i);const auto& object=result.objects[i];
      if(mapping[i]!=UINT32_MAX) {
        matrixNear(object.model,expected[mapping[i]]);
        const auto normal=glm::transpose(glm::inverse(glm::mat3(expected[mapping[i]])));
        const std::array columns{object.normalMatrix0,object.normalMatrix1,object.normalMatrix2};
        for(int c=0;c<3;++c) for(int r=0;r<3;++r) EXPECT_NEAR(columns[c][r],normal[c][r],3e-4f*std::max(1.f,std::abs(normal[c][r])));
      } else {
        matrixNear(object.model,fixtureDiagnosticModel());
        EXPECT_EQ(object.normalMatrix0,glm::vec4(10));EXPECT_EQ(object.normalMatrix1,glm::vec4(11));
        EXPECT_EQ(object.normalMatrix2,glm::vec4(12));
      }
      EXPECT_EQ(object.objectInfo,glm::uvec4(uint32_t(i),42,43,44));
      EXPECT_EQ(object.temporalInfo,glm::uvec4(3,1,4,5));EXPECT_EQ(object.boundingSphere,glm::vec4(1,2,3,4));
      matrixNear(object.previousModel,fixturePreviousModel());
    }
    return result;
  }
  std::filesystem::path shaderDir;
  bool glfwStarted{};GLFWwindow* window{};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device;
  std::unique_ptr<VulkanMemoryManager> memory;
  std::unique_ptr<PipelineManager> pipelines;
  std::unique_ptr<GpuHierarchyPass> pass;
  std::array<AllocatedBuffer,2> destinations{};
  vk::raii::CommandPool pool{nullptr};vk::raii::CommandBuffer command{nullptr};
  std::atomic<uint32_t> validationErrors{};
};

TEST_F(GpuHierarchyGpu, UnorderedBroadMirroredNestedForestPreservesMetadata) {
  std::vector<HierarchyLocalTransform> nodes(301);std::vector<uint32_t> mapping;
  for(uint32_t i=0;i<300;++i) {
    nodes[i].parent=300;nodes[i].local=glm::translate(glm::mat4(1),glm::vec3(float(i)*.01f,.3f,-.2f))*
        glm::rotate(glm::mat4(1),.01f*float(i),glm::normalize(glm::vec3(1,2,3)))*
        glm::scale(glm::mat4(1),glm::vec3(i%2?-.5f:1.f,2.f,.3f));mapping.push_back(i);
  }
  nodes[2].parent=1;nodes[3].parent=2;nodes[4].parent=UINT32_MAX;
  nodes[300].local=glm::translate(glm::mat4(1),glm::vec3(1,2,3));
  mapping.push_back(300);mapping.push_back(UINT32_MAX);mapping.push_back(2);
  verify(0,nodes,mapping);EXPECT_EQ(pass->stats(0).levels,4);
  verify(0,nodes,mapping);EXPECT_EQ(pass->stats(0).uploadedBytes,0);
  EXPECT_EQ(pass->stats(0).dispatchedLevels,0);
  nodes[300].local[3].x+=.2f;verify(0,nodes,mapping);
  EXPECT_EQ(pass->stats(0).dirtyLocals,1);EXPECT_EQ(pass->stats(0).uploadedBytes,80);
  EXPECT_EQ(pass->stats(0).dispatchedLevels,4);
  verify(1,nodes,mapping);EXPECT_EQ(pass->stats(1).dirtyLocals,nodes.size());
}
TEST_F(GpuHierarchyGpu, DeepChainReparentAndReplacementAgreeWithCpu) {
  std::vector<HierarchyLocalTransform> nodes(257);std::vector<uint32_t> mapping;
  for(uint32_t i=0;i<nodes.size();++i) {
    nodes[i].parent=i?i-1:UINT32_MAX;nodes[i].local=glm::translate(glm::mat4(1),glm::vec3(.001f,.002f,.003f));mapping.push_back(i);
  }
  verify(0,nodes,mapping);EXPECT_EQ(pass->stats(0).levels,257);
  nodes[128].parent=4;verify(0,nodes,mapping);EXPECT_EQ(pass->stats(0).levels,134);
  nodes.resize(3);mapping={2,1,0};verify(0,nodes,mapping);
  pass->createResources(shaderDir,2);verify(1,nodes,mapping);
}
TEST_F(GpuHierarchyGpu, RejectedInputRetainsPreparedFrameAndCancelledRecordingCannotStaleWorlds) {
  std::vector<HierarchyLocalTransform> nodes{{glm::mat4(1),UINT32_MAX},{glm::mat4(1),0}};
  const std::vector<uint32_t> mapping{1};verify(0,nodes,mapping);
  const BufferSlice target{destinations[0].buffer,0,sizeof(ObjectData)};
  auto bad=nodes;bad[0].parent=1;EXPECT_THROW(pass->prepareFrame(0,bad,mapping,target),std::invalid_argument);
  bad=nodes;bad[0].local[0][3]=1;EXPECT_THROW(pass->prepareFrame(0,bad,mapping,target),std::invalid_argument);
  bad=nodes;bad[0].parent=10;EXPECT_THROW(pass->prepareFrame(0,bad,mapping,target),std::invalid_argument);
  bad=nodes;bad[0].local[0][0]=0;EXPECT_THROW(pass->prepareFrame(0,bad,mapping,target),std::invalid_argument);
  bad=nodes;bad[0].local[0][0]=std::numeric_limits<float>::quiet_NaN();EXPECT_THROW(pass->prepareFrame(0,bad,mapping,target),std::invalid_argument);
  EXPECT_THROW(pass->prepareFrame(0,nodes,std::array<uint32_t,1>{8},target),std::invalid_argument);
  EXPECT_THROW(pass->prepareFrame(3,nodes,mapping,target),std::invalid_argument);
  EXPECT_THROW(pass->prepareFrame(0,nodes,mapping,{target.buffer,0,16}),std::invalid_argument);
  EXPECT_THROW(pass->createResources(shaderDir,0),std::invalid_argument);
  EXPECT_THROW(pass->createResources(shaderDir,65),std::invalid_argument);
  const std::vector<HierarchyLocalTransform> oversized(262145);
  EXPECT_THROW(pass->prepareFrame(0,oversized,mapping,target),std::invalid_argument);
  const std::vector<uint32_t> tooManyObjects(1000001,0);
  EXPECT_THROW(pass->prepareFrame(0,nodes,tooManyObjects,target),std::invalid_argument);
  std::vector<HierarchyLocalTransform> tooDeep(4097);
  for(uint32_t i=1;i<tooDeep.size();++i) tooDeep[i].parent=i-1;
  EXPECT_THROW(pass->prepareFrame(0,tooDeep,mapping,target),std::invalid_argument);
  nodes[0].local[3].x=2;pass->prepareFrame(0,nodes,mapping,target); // Deliberately never record or submit.
  nodes[1].local[3].y=3;verify(0,nodes,mapping);EXPECT_EQ(pass->stats(0).dirtyLocals,1);
}
TEST_F(GpuHierarchyGpu, LargeChangingHierarchyReportsActualCpuUploadAndGpuCosts) {
  std::vector<HierarchyLocalTransform> nodes(50000);std::vector<uint32_t> mapping;
  for(uint32_t i=0;i<nodes.size();++i) {
    nodes[i].parent=i?(i-1)/8:UINT32_MAX;nodes[i].local=glm::translate(glm::mat4(1),glm::vec3(.1f,.2f,.3f));mapping.push_back(i);
  }
  verify(0,nodes,mapping);
  double cpuMs=0,prepareMs=0,gpuMs=0,metadataMs=0;uint64_t uploaded=0,metadataBytes=0;
  for(uint32_t iteration=0;iteration<6;++iteration) {
    for(uint32_t i=0;i<nodes.size();i+=97) nodes[i].local[3].x+=.01f;
    const auto start=std::chrono::steady_clock::now();const auto expected=reference(nodes);
    cpuMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    const auto result=run(0,nodes,mapping,&expected);matrixNear(result.worlds.back(),expected.back());
    prepareMs+=pass->stats(0).prepareMs;uploaded+=pass->stats(0).uploadedBytes;gpuMs+=result.gpuMs;
    metadataMs+=pass->stats(0).objectMetadataMs;metadataBytes+=pass->stats(0).objectMetadataBytes;
  }
  std::printf("HIERARCHY_BENCHMARK nodes=50000 levels=%u CPU_reference_ms=%.4f prepare_ms=%.4f hierarchy_input_bytes=%llu metadata_bytes=%llu metadata_ms=%.4f GPU_ms=%.4f (six iterations; CPU mirror retained; complete GPU object publication)\n",
      pass->stats(0).levels,cpuMs/6,prepareMs/6,static_cast<unsigned long long>(uploaded/6),
      static_cast<unsigned long long>(metadataBytes/6),metadataMs/6,gpuMs/6);
}
TEST_F(GpuHierarchyGpu, SparseMetadataPreservesHistoryAndDiagnosticPrefixWithoutCpuObjectWrites) {
  const std::vector<HierarchyLocalTransform> nodes{{glm::mat4(1),UINT32_MAX},{glm::mat4(1),0}};
  const std::vector<uint32_t> mapping{0,1,UINT32_MAX};
  verify(0,nodes,mapping);EXPECT_EQ(pass->stats(0).objectMetadataBytes,112*4);
  verify(0,nodes,mapping);EXPECT_EQ(pass->stats(0).objectMetadataBytes,0);
  auto metadata=fixtureMetadata(mapping.size());
  metadata[0].objectInfo.x=0xdeadbeefu;metadata[0].previousModel[3].x=23;
  metadata[2].model[3].y=-37;metadata[2].normalMatrix1=glm::vec4(-7,-8,-9,-10);
  const auto result=run(0,nodes,mapping,nullptr,&metadata);
  EXPECT_EQ(pass->stats(0).objectMetadataBytes,224);
  EXPECT_EQ(result.objects[0].objectInfo.x,0xdeadbeefu);
  matrixNear(result.objects[0].previousModel,metadata[0].previousModel);
  matrixNear(result.objects[2].model,metadata[2].model);
  EXPECT_EQ(result.objects[2].normalMatrix1,metadata[2].normalMatrix1);
  EXPECT_THROW(pass->uploadObjectMetadata(0,{}),std::invalid_argument);
}
}  // namespace
