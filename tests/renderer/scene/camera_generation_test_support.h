#pragma once

#include <Container/common/CommonGLFW.h>
#include <Container/geometry/GltfModelLoader.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneGraph.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/VulkanDevice.h>

#include <gtest/gtest.h>
#include "material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#ifndef CONTAINER_CAMERA_PROJECTION_SHADER_PATH
#define CONTAINER_CAMERA_PROJECTION_SHADER_PATH "spv_shaders/camera_projection_contract.comp.spv"
#endif

namespace {
using namespace container::scene;
using namespace container::geometry::gltf;
using namespace container::gpu;
using namespace container::util;
using Property=GltfCameraProperty;
tinygltf::Model cameraAsset(bool material=true) {
  tinygltf::Model source;source.asset.version="2.0";
  const std::array<float,9> positions{0,0,0,1,0,0,0,1,0};const std::array<uint16_t,3> indices{0,1,2};
  source.buffers.resize(1);source.buffers[0].data.resize(sizeof(positions)+sizeof(indices));
  std::memcpy(source.buffers[0].data.data(),positions.data(),sizeof(positions));
  std::memcpy(source.buffers[0].data.data()+sizeof(positions),indices.data(),sizeof(indices));
  source.bufferViews.resize(2);source.bufferViews[0].buffer=source.bufferViews[1].buffer=0;source.bufferViews[0].byteLength=sizeof(positions);
  source.bufferViews[1].byteOffset=sizeof(positions);source.bufferViews[1].byteLength=sizeof(indices);
  source.accessors.resize(2);source.accessors[0].bufferView=0;source.accessors[0].componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  source.accessors[0].type=TINYGLTF_TYPE_VEC3;source.accessors[0].count=3;source.accessors[0].minValues={0,0,0};source.accessors[0].maxValues={1,1,0};
  source.accessors[1].bufferView=1;source.accessors[1].componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
  source.accessors[1].type=TINYGLTF_TYPE_SCALAR;source.accessors[1].count=3;
  if(material) source.materials.emplace_back();source.meshes.resize(1);tinygltf::Primitive primitive;
  primitive.attributes["POSITION"]=0;primitive.indices=1;primitive.material=material?0:-1;primitive.mode=TINYGLTF_MODE_TRIANGLES;
  source.meshes[0].primitives.push_back(primitive);source.cameras.resize(2);
  for(size_t row=0;row<source.cameras.size();++row) {
    auto& camera=source.cameras[row];camera.name="Camera "+std::to_string(row);camera.type="perspective";
    camera.perspective.yfov=.8;camera.perspective.znear=.1;camera.perspective.zfar=100;
    camera.orthographic.xmag=3;camera.orthographic.ymag=2;camera.orthographic.znear=0;camera.orthographic.zfar=50;
    camera.extras=tinygltf::Value(tinygltf::Value::Object{{"opaqueIndex",tinygltf::Value(int(701+row))}});
  }
  source.nodes.resize(5);source.nodes[0].mesh=0;source.nodes[1].camera=source.nodes[2].camera=0;source.nodes[3].camera=1;
  source.nodes[1].translation={0,0,4};source.nodes[2].translation={2,0,4};source.nodes[3].translation={0,3,5};
  source.nodes[4].translation={1,2,3};source.nodes[4].rotation={0,0,std::sin(.25),std::cos(.25)};source.nodes[4].children={1};
  source.scenes.resize(2);source.scenes[0].nodes={0,4,2};source.scenes[1].nodes={3};source.defaultScene=0;return source;
}
GltfCameraProjectionPatch perspective(double fov=.95,double near=.25) {
  GltfCameraProjectionPatch patch;patch.values={{Property::PerspectiveYfov,fov},{Property::PerspectiveNear,near},
      {Property::PerspectiveAspectRatio,std::monostate{}},{Property::PerspectiveFar,std::monostate{}}};return patch;
}

class CameraProjectionGenerationGpu:public ::testing::Test {
 protected:
  virtual const char* gpuEnvironment() const noexcept { return "CONTAINER_RUN_GPU_CAMERA_PROJECTIONS"; }
  virtual void configureIndirectDraws(DeviceCreateInfo&, VkPhysicalDeviceVulkan12Features&) const {}
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {++static_cast<CameraProjectionGenerationGpu*>(user)->errors;std::fprintf(stderr,"%s\n",data->pMessage);}return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv(gpuEnvironment());if(!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set "<<gpuEnvironment()<<"=1";
    ASSERT_EQ(glfwInit(),GLFW_TRUE);glfwStarted=true;glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    window=glfwCreateWindow(32,32,"Camera projection generation",nullptr,nullptr);ASSERT_NE(window,nullptr);
    uint32_t count{};const auto* extensions=glfwGetRequiredInstanceExtensions(&count);ASSERT_NE(extensions,nullptr);
    InstanceCreateInfo create;create.enableValidationLayers=true;create.validationLayers={"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions,extensions+count);create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};validation.enabledValidationFeatureCount=1;validation.pEnabledValidationFeatures=&sync;create.next=&validation;
    instance=std::make_unique<VulkanInstance>(create);vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;debug.messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation|vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback=reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(debugCallback);debug.pUserData=this;
    messenger=vk::raii::DebugUtilsMessengerEXT(instance->raii(),debug);VkSurfaceKHR raw{};ASSERT_EQ(glfwCreateWindowSurface(instance->instance(),window,nullptr,&raw),VK_SUCCESS);
    surface=vk::raii::SurfaceKHR(instance->raii(),raw);DeviceCreateInfo info;info.requiredExtensions={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};v12.bufferDeviceAddress=VK_TRUE;v12.runtimeDescriptorArray=VK_TRUE;
    v12.descriptorBindingPartiallyBound=VK_TRUE;v12.shaderSampledImageArrayNonUniformIndexing=VK_TRUE;
    v12.shaderStorageBufferArrayNonUniformIndexing=VK_TRUE;
    configureIndirectDraws(info, v12);
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2=VK_TRUE;v13.pNext=&v12;info.next=&v13;
    device=std::make_shared<VulkanDevice>(*instance,raw,info);pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,device->queueFamilyIndices().graphicsFamily.value()));
    allocation=std::make_unique<AllocationManager>();allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
    pipelines=std::make_unique<PipelineManager>(device->device());temp=std::filesystem::temp_directory_path()/("container-camera-projection-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);
    for(int row=0;row<2;++row) {
      cameras.push_back(allocation->createBuffer(sizeof(CameraData),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT));
      objects.push_back(allocation->createBuffer(sizeof(ObjectData),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT));
    }
    load(cameraAsset());
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();player.reset();manager.reset();
    if(allocation) {for(auto& value:cameras) allocation->destroyBuffer(value);for(auto& value:objects) allocation->destroyBuffer(value);}
    cameras.clear();objects.clear();pipelines.reset();allocation.reset();pool.clear();device.reset();surface.clear();messenger.clear();instance.reset();
    if(window) glfwDestroyWindow(window);if(glfwStarted) glfwTerminate();
    if(!temp.empty()) {std::error_code ignored;const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());const auto target=std::filesystem::weakly_canonical(temp);
      if(target.parent_path()==parent && target.filename().string().starts_with("container-camera-projection-")) std::filesystem::remove_all(target,ignored);}
    EXPECT_EQ(errors.load(),0u);
  }
  void load(tinygltf::Model source) {
    const auto path=temp/"camera.glb";tinygltf::TinyGLTF writer;if(!writer.WriteGltfSceneToFile(&source,path.string(),true,true,true,true)) throw std::runtime_error("Cannot write camera fixture");
    player.reset();manager.reset();graph=SceneGraph{};manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});
    manager->initialize(path.string(),1,2);manager->populateSceneGraph(graph);
    const auto importedRoots=graph.rootNodes();
    const auto editor=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Scene Root");
    graph.getNode(editor)->editorSceneRoot=true;
    for(const auto root:importedRoots) if(!graph.setParent(root,editor)) throw std::runtime_error("Cannot attach imported camera fixture root");
    graph.updateWorldTransforms();manager->updateDescriptorSets(cameras,objects);manager->updateAuxiliaryDescriptorSets(cameras,objects);
    player=std::make_unique<AnimationPlayer>(manager->animationSource());
  }
  uint32_t node(int source) const {for(uint32_t row=0;row<graph.nodeCount();++row) if(graph.getNode(row)->sourceNodeIndex==source) return row;return SceneGraph::kInvalidNode;}
  CameraProjectionTarget target(int source=1) const {
    const auto value=manager->cameraProjectionTarget(graph,node(source));if(!value) throw std::runtime_error("Missing active camera fixture target");return *value;
  }
  uint32_t allocations() const {VmaTotalStatistics stats{};vmaCalculateStatistics(allocation->memoryManager()->allocator(),&stats);return stats.total.statistics.allocationCount;}
  bool publish(SceneManager::PreparedCameraProjectionEdit& prepared,float aspect=2.f) {
    const auto view=prepared.candidateView();bool ready{},changed{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeCameraProjectionEdit(prepared,graph,player.get(),aspect);
      changed=manager->exchangeCameraProjectionEdit(prepared,graph,player.get(),aspect);attempts=deny.allocations();}
    if(!ready || !changed || prepared || attempts) return false;
    for(auto& camera:cameras) {
      std::memcpy(camera.allocation_info.pMappedData,&view.data,sizeof(view.data));
      if(vmaFlushAllocation(allocation->memoryManager()->allocator(),camera.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot flush camera fixture");
    }
    return true;
  }
  std::array<glm::vec4,2> probe(const glm::vec4& point) {
    const auto bytes=2*sizeof(glm::vec4);auto output=allocation->createBuffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::fill_n(static_cast<uint32_t*>(output.allocation_info.pMappedData),bytes/sizeof(uint32_t),0x7fc00000u);
    if(vmaFlushAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot poison camera readback");
    std::array<glm::vec4,2> result;
    {
      const vk::DescriptorSetLayoutBinding binding(0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute);
      vk::raii::DescriptorSetLayout outputLayout(device->raii(),vk::DescriptorSetLayoutCreateInfo({},binding));
      const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer,1);vk::raii::DescriptorPool descriptors(device->raii(),vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,1,size));
      const std::array outputLayouts{*outputLayout};auto sets=device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors,outputLayouts));
      const vk::DescriptorBufferInfo info(output.buffer,0,bytes);const vk::WriteDescriptorSet write(*sets[0],0,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&info);device->raii().updateDescriptorSets(write,{});
      const std::array layouts{vk::DescriptorSetLayout(manager->descriptorSetLayout()),*outputLayout};const vk::PushConstantRange push(vk::ShaderStageFlagBits::eCompute,0,sizeof(point));
      vk::raii::PipelineLayout layout(device->raii(),vk::PipelineLayoutCreateInfo({},layouts,push));const auto code=readFile(CONTAINER_CAMERA_PROJECTION_SHADER_PATH);
      vk::raii::ShaderModule shader(device->raii(),vk::ShaderModuleCreateInfo({},code.size(),reinterpret_cast<const uint32_t*>(code.data())));
      vk::raii::Pipeline pipeline(device->raii(),nullptr,vk::ComputePipelineCreateInfo({},vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,*shader,"computeMain"),*layout));
      allocation->executeStagedCommands([&](VkCommandBuffer command) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,*pipeline);const std::array<VkDescriptorSet,2> raw{manager->descriptorSet(0),*sets[0]};
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,*layout,0,2,raw.data(),0,nullptr);vkCmdPushConstants(command,*layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(point),&point);vkCmdDispatch(command,1,1,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
      });
      if(vmaInvalidateAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot read camera projection probe");
      std::memcpy(result.data(),output.allocation_info.pMappedData,bytes);
    }
    allocation->destroyBuffer(output);return result;
  }
  std::atomic_uint errors{};bool glfwStarted{};GLFWwindow* window{};std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};vk::raii::SurfaceKHR surface{nullptr};std::shared_ptr<VulkanDevice> device;
  vk::raii::CommandPool pool{nullptr};std::unique_ptr<AllocationManager> allocation;std::unique_ptr<PipelineManager> pipelines;
  std::unique_ptr<SceneManager> manager;std::unique_ptr<AnimationPlayer> player;SceneGraph graph;std::filesystem::path temp;
  std::vector<AllocatedBuffer> cameras,objects;
};

} // namespace
