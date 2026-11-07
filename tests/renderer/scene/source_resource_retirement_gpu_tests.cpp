#include <Container/common/CommonGLFW.h>
#include <Container/geometry/GltfModelLoader.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/MaterialSamplerKey.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/VulkanDevice.h>

#include <gtest/gtest.h>
#include "material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#ifndef CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH
#define CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH "spv_shaders/scene_texture_sampling_contract.comp.spv"
#endif

namespace container::scene {
struct SceneTextureSamplingTestAccess {
  static bool dependenciesValid(const SceneManager& manager) {return manager.materialResourceDependenciesValid();}
  static auto texture(const SceneManager& manager,uint32_t row) {return *manager.textureManager_.getTexture(row);}
  static int32_t exportTexture(const SceneManager& manager,uint32_t row) {return manager.exportTextureIndices_.at(row);}
  static size_t requestCount(const SceneManager& manager) {return manager.materialTextureSourceRequests_.size();}
  static bool requestActive(const SceneManager& manager,size_t row) {return manager.materialTextureSourceRequests_.at(row).active;}
  static bool requestEmpty(const SceneManager& manager,size_t row) {const auto& request=manager.materialTextureSourceRequests_.at(row);
    return request.key.empty() && !request.encoded && !request.sourceIdentity && !request.inputSourceIdentity;}
  static auto fallback(const SceneManager& manager) {return manager.textureSamplingFallback_->resource;}
  static const tinygltf::Material& exportedMaterial(const SceneManager& manager,uint32_t runtime) {
    return manager.exportMaterialSources_.at(runtime).value();
  }
};
}

namespace {
using namespace container::scene;
using namespace container::geometry;
using namespace container::geometry::gltf;
using namespace container::gpu;
using namespace container::util;
using Access=SceneTextureSamplingTestAccess;

tinygltf::Model triangle() {
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
  source.materials.resize(1);source.materials[0].name="Untextured source material";
  source.meshes.resize(1);tinygltf::Primitive primitive;primitive.attributes["POSITION"]=0;primitive.indices=1;primitive.material=0;
  primitive.mode=TINYGLTF_MODE_TRIANGLES;source.meshes[0].primitives.push_back(primitive);
  source.nodes.resize(1);source.nodes[0].mesh=0;source.scenes.resize(1);source.scenes[0].nodes={0};source.defaultScene=0;
  return source;
}

class SourceResourceRetirementGpu:public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {++static_cast<SourceResourceRetirementGpu*>(user)->errors;std::fprintf(stderr,"%s\n",data->pMessage);}return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_SOURCE_RESOURCE_RETIREMENT");
    if(!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_SOURCE_RESOURCE_RETIREMENT=1";
    ASSERT_EQ(glfwInit(),GLFW_TRUE);glfwStarted=true;glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    window=glfwCreateWindow(32,32,"Source resource retirement",nullptr,nullptr);ASSERT_NE(window,nullptr);
    uint32_t count{};const auto* extensions=glfwGetRequiredInstanceExtensions(&count);ASSERT_NE(extensions,nullptr);
    InstanceCreateInfo create;create.enableValidationLayers=true;create.validationLayers={"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions,extensions+count);create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};validation.enabledValidationFeatureCount=1;validation.pEnabledValidationFeatures=&sync;create.next=&validation;
    instance=std::make_unique<VulkanInstance>(create);vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation|vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback=reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(debugCallback);debug.pUserData=this;
    messenger=vk::raii::DebugUtilsMessengerEXT(instance->raii(),debug);VkSurfaceKHR raw{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(),window,nullptr,&raw),VK_SUCCESS);surface=vk::raii::SurfaceKHR(instance->raii(),raw);
    DeviceCreateInfo info;info.requiredExtensions={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress=VK_TRUE;v12.runtimeDescriptorArray=VK_TRUE;v12.descriptorBindingPartiallyBound=VK_TRUE;v12.shaderSampledImageArrayNonUniformIndexing=VK_TRUE;
    v12.shaderStorageBufferArrayNonUniformIndexing=VK_TRUE;info.next=&v12;
    device=std::make_shared<VulkanDevice>(*instance,raw,info);
    pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,device->queueFamilyIndices().graphicsFamily.value()));
    allocation=std::make_unique<AllocationManager>();allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
    pipelines=std::make_unique<PipelineManager>(device->device());
    temp=std::filesystem::temp_directory_path()/("container-source-retirement-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);auto source=triangle();tinygltf::TinyGLTF writer;
    ASSERT_TRUE(writer.WriteGltfSceneToFile(&source,(temp/"source.glb").string(),true,true,true,true));
    manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});manager->initialize((temp/"source.glb").string(),1,2);
    manager->populateSceneGraph(graph);
    for(int i=0;i<2;++i) {
      cameras.push_back(allocation->createBuffer(sizeof(CameraData),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT));
      objects.push_back(allocation->createBuffer(sizeof(ObjectData),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT));
    }
    manager->updateDescriptorSets(cameras,objects[0]);
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();manager.reset();
    if(allocation) {for(auto& b:cameras) allocation->destroyBuffer(b);for(auto& b:objects) allocation->destroyBuffer(b);}
    cameras.clear();objects.clear();pipelines.reset();allocation.reset();pool.clear();device.reset();surface.clear();messenger.clear();instance.reset();
    if(window) glfwDestroyWindow(window);if(glfwStarted) glfwTerminate();
    if(!temp.empty()) {std::error_code ignored;const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());const auto target=std::filesystem::weakly_canonical(temp);
      if(target.parent_path()==parent && target.filename().string().starts_with("container-source-retirement-")) std::filesystem::remove_all(target,ignored);}
    EXPECT_EQ(errors.load(),0u);
  }
  std::vector<std::byte> png(unsigned char red=255) {
    auto source=triangle();source.images.resize(1);auto& image=source.images[0];image.width=image.height=1;
    image.component=4;image.bits=8;image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;image.image={red,127,63,255};image.uri="texture.png";
    source.textures.resize(1);source.textures[0].source=0;source.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
    tinygltf::TinyGLTF writer;if(!writer.WriteGltfSceneToFile(&source,(temp/"texture.gltf").string(),false,true,true,false)) throw std::runtime_error("Cannot write source PNG");
    std::ifstream stream(temp/"texture.png",std::ios::binary|std::ios::ate);const auto length=stream.tellg();
    if(!stream || length<=0 || length>1024*1024) throw std::runtime_error("Invalid PNG fixture");
    std::vector<std::byte> result(static_cast<size_t>(length));stream.seekg(0);if(!stream.read(reinterpret_cast<char*>(result.data()),std::streamsize(result.size()))) throw std::runtime_error("Cannot read PNG fixture");return result;
  }
  SceneManager::MaterialTextureReference add(const std::string& name,unsigned char red=255,uint32_t sampler=0) {
    auto resources=manager->prepareMaterialTextureBytes(0,name,png(red),MaterialTextureRoles::Color,sampler);
    const auto reference=resources.reference(true);resources.sealLiveDependencies();
    if(!manager->canExchangeMaterialResources(resources)) throw std::runtime_error("Cannot publish source fixture");
    manager->exchangeMaterialResources(resources);if(resources) throw std::runtime_error("Source fixture was not published");
    manager->updateDescriptorSets(cameras,objects[0]);return reference;
  }
  static auto textures(const SceneManager::MaterialTextureReference& reference) {
    return std::array{TextureResourceSelection{ResourceDomainKind::Archive,ResourceKind::Texture,reference.archiveTexture()},
        TextureResourceSelection{ResourceDomainKind::Evaluated,ResourceKind::Texture,reference.evaluatedTexture().value()}};
  }
  bool publish(SceneManager::PreparedSourceResourceRetirement& prepared,const AnimationPlayer* player=nullptr) {
    if(!manager->canExchangeSourceResourceRetirement(prepared,player)) return false;
    bool ready{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeSourceResourceRetirement(prepared,player);
      manager->exchangeSourceResourceRetirement(prepared,player);attempts=deny.allocations();}
    return ready && !prepared && attempts==0;
  }
  bool publishRuntime(SceneManager::PreparedSourceResourceRetirement& prepared,SceneManager::PreparedSceneBindings& bindings,
      const AnimationPlayer* player=nullptr) {
    if(!manager->canExchangeSourceResourceRetirement(prepared,player) || !manager->canExchangeSceneBindings(bindings,player)) return false;
    bool ready{},after{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);
      ready=manager->canExchangeSourceResourceRetirement(prepared,player) && manager->canExchangeSceneBindings(bindings,player);
      manager->exchangeSourceResourceRetirement(prepared,player);after=manager->canExchangeSceneBindings(bindings,player);
      manager->exchangeSceneBindings(bindings,player);attempts=deny.allocations();}
    return ready && after && !prepared && !bindings && attempts==0;
  }
  bool publishMaterial(SceneManager::PreparedMaterialGeneration& prepared,AnimationPlayer* player=nullptr) {
    prepared.prepareDescriptorFrames(cameras,objects);if(!manager->canExchangeMaterialGeneration(prepared,player)) return false;
    size_t attempts{};{material_publication_test::AllocationGuard deny(true);manager->exchangeMaterialGeneration(prepared,player);attempts=deny.allocations();}
    return !prepared && attempts==0;
  }
  uint64_t allocatedBytes() const {VmaTotalStatistics stats{};vmaCalculateStatistics(allocation->memoryManager()->allocator(),&stats);return stats.total.statistics.allocationBytes;}
  std::vector<glm::vec4> sample(VkDescriptorSet sceneSet,uint32_t count) {
    const auto bytes=sizeof(glm::vec4)*size_t(count)*2;
    auto output=allocation->createBuffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if(!output.allocation_info.pMappedData) throw std::runtime_error("Texture readback is not mapped");
    std::fill_n(static_cast<uint32_t*>(output.allocation_info.pMappedData),bytes/sizeof(uint32_t),0x7fc00000u);
    if(vmaFlushAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot poison readback");
    std::vector<glm::vec4> result;
    {
      const vk::DescriptorSetLayoutBinding binding(0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute);
      vk::raii::DescriptorSetLayout outputLayout(device->raii(),vk::DescriptorSetLayoutCreateInfo({},binding));
      const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer,1);
      vk::raii::DescriptorPool descriptors(device->raii(),vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,1,size));
      const std::array outputLayouts{*outputLayout};auto sets=device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors,outputLayouts));
      const vk::DescriptorBufferInfo info(output.buffer,0,bytes);const vk::WriteDescriptorSet write(*sets[0],0,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&info);
      device->raii().updateDescriptorSets(write,{});
      const std::array layouts{vk::DescriptorSetLayout(manager->descriptorSetLayout()),*outputLayout};const vk::PushConstantRange push(vk::ShaderStageFlagBits::eCompute,0,16);
      vk::raii::PipelineLayout layout(device->raii(),vk::PipelineLayoutCreateInfo({},layouts,push));
      const auto code=container::util::readFile(CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH);
      vk::raii::ShaderModule shader(device->raii(),vk::ShaderModuleCreateInfo({},code.size(),reinterpret_cast<const uint32_t*>(code.data())));
      vk::raii::Pipeline pipeline(device->raii(),nullptr,vk::ComputePipelineCreateInfo({},vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,*shader,"computeMain"),*layout));
      struct Query {uint32_t count,padding;float u,v;};const Query query{count,0,.5f,.5f};static_assert(sizeof(Query)==16);
      allocation->executeStagedCommands([&](VkCommandBuffer command) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,*pipeline);const std::array<VkDescriptorSet,2> raw{sceneSet,*sets[0]};
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,*layout,0,2,raw.data(),0,nullptr);
        vkCmdPushConstants(command,*layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(query),&query);vkCmdDispatch(command,(count+31)/32,1,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
      });
      if(vmaInvalidateAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot read sampled textures");
      result.resize(count*2);std::memcpy(result.data(),output.allocation_info.pMappedData,bytes);
    }
    allocation->destroyBuffer(output);return result;
  }
  uint32_t allocations() const {VmaTotalStatistics stats{};vmaCalculateStatistics(allocation->memoryManager()->allocator(),&stats);return stats.total.statistics.allocationCount;}
  std::atomic_uint errors{};bool glfwStarted{};GLFWwindow* window{};
  std::unique_ptr<VulkanInstance> instance;vk::raii::DebugUtilsMessengerEXT messenger{nullptr};vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device;vk::raii::CommandPool pool{nullptr};std::unique_ptr<AllocationManager> allocation;
  std::unique_ptr<PipelineManager> pipelines;std::unique_ptr<SceneManager> manager;std::filesystem::path temp;
  SceneGraph graph;
  std::vector<AllocatedBuffer> cameras,objects;
};

TEST_F(SourceResourceRetirementGpu,SelectedTextureUndoRedoPreservesGpuImagesAndUnrelatedAppends) {
  const auto key=container::material::NormalizeMaterialSamplerKey(9728,9728,33071,33071).value;
  const auto reference=add("Selected source",255,key);const auto selected=textures(reference);
  const auto sourceTexture=manager->authoredMaterialResources().textures[reference.archiveTexture()];
  const auto aImage=uint32_t(sourceTexture.source),aSampler=uint32_t(sourceTexture.sampler);
  const auto eTexture=manager->animationSource().textures[reference.evaluatedTexture().value()];
  ASSERT_GE(sourceTexture.source,0);ASSERT_GE(sourceTexture.sampler,0);ASSERT_GE(eTexture.source,0);ASSERT_GE(eTexture.sampler,0);
  const auto image=Access::texture(*manager,reference.runtimeResource());
  const auto generation=manager->resourceLifecycleGeneration();const auto aHandle=generation->resource(selected[0]),eHandle=generation->resource(selected[1]);
  auto retained=RetainTextureResourceReferences(generation,selected,ResourcePinClass::History);
  auto staged=manager->prepareSourceResourceRetirement(selected);auto command=staged.command();
  EXPECT_FALSE(command.published());const auto descriptor=manager->descriptorSet(0);const auto count=allocations();const auto revision=manager->materialResourceRevision();
  ASSERT_TRUE(publish(staged));staged={};EXPECT_TRUE(command.published());EXPECT_TRUE(command.retired());
  EXPECT_EQ(manager->materialResourceRevision(),revision+1);EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(allocations(),count);
  EXPECT_TRUE(allocation->textureImageMatches(image));EXPECT_TRUE(Access::dependenciesValid(*manager));
  EXPECT_TRUE(retained->valid());EXPECT_FALSE(retained->resourceDependenciesActive());
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(selected[0]));EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(selected[1]));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,reference.runtimeResource()}));
  EXPECT_EQ(Access::exportTexture(*manager,reference.runtimeResource()),-1);EXPECT_FALSE(Access::requestActive(*manager,0));EXPECT_TRUE(Access::requestEmpty(*manager,0));
  const auto other=add("Unrelated source",31);const auto otherMap=Access::exportTexture(*manager,other.runtimeResource());
  ASSERT_NE(other.runtimeResource(),reference.runtimeResource());ASSERT_EQ(Access::requestCount(*manager),2u);
  auto undo=manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Active);ASSERT_TRUE(publish(undo));undo={};
  EXPECT_FALSE(command.retired());EXPECT_TRUE(Access::requestActive(*manager,0));EXPECT_TRUE(Access::requestActive(*manager,1));
  EXPECT_EQ(Access::exportTexture(*manager,other.runtimeResource()),otherMap);
  EXPECT_EQ(manager->resourceLifecycleGeneration()->resource(selected[0]),aHandle);EXPECT_EQ(manager->resourceLifecycleGeneration()->resource(selected[1]),eHandle);
  EXPECT_TRUE(retained->resourceDependenciesActive());
  auto redo=manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Retired);ASSERT_TRUE(publish(redo));redo={};
  EXPECT_TRUE(command.retired());EXPECT_TRUE(Access::requestActive(*manager,1));EXPECT_TRUE(allocation->textureImageMatches(image));
  // Texture outgoing edges were released first. Its exact orphan image and
  // sampler identities can now retire separately, while GPU pixels stay live.
  const std::array orphans{TextureResourceSelection{ResourceDomainKind::Archive,ResourceKind::Image,aImage},
      TextureResourceSelection{ResourceDomainKind::Archive,ResourceKind::Sampler,aSampler},
      TextureResourceSelection{ResourceDomainKind::Evaluated,ResourceKind::Image,uint32_t(eTexture.source)},
      TextureResourceSelection{ResourceDomainKind::Evaluated,ResourceKind::Sampler,uint32_t(eTexture.sampler)}};
  auto orphanStage=manager->prepareSourceResourceRetirement(orphans);auto orphanCommand=orphanStage.command();
  ASSERT_TRUE(publish(orphanStage));orphanStage={};EXPECT_TRUE(allocation->textureImageMatches(image));
  EXPECT_TRUE(command.references()->valid());EXPECT_FALSE(command.references()->resourceDependenciesActive());
  // Both save coordinators use this shared projection before their overlays
  // and resource packing. The manager writer is additionally reopened here.
  const auto projected=manager->buildSceneExport(graph);
  EXPECT_EQ(projected.textures.size(),1u);EXPECT_EQ(projected.images.size(),1u);EXPECT_EQ(projected.samplers.size(),1u);
  for(const auto* extension:{".gltf",".glb"}) {
    const auto path=temp/(std::string("retired-source")+extension);manager->saveScene(graph,path);
    tinygltf::TinyGLTF reader;tinygltf::Model reopened;std::string error,warning;
    const bool opened=std::string_view(extension)==".glb"?reader.LoadBinaryFromFile(&reopened,&error,&warning,path.string()):
        reader.LoadASCIIFromFile(&reopened,&error,&warning,path.string());
    ASSERT_TRUE(opened)<<error<<warning;EXPECT_EQ(reopened.textures.size(),1u);EXPECT_EQ(reopened.images.size(),1u);EXPECT_EQ(reopened.samplers.size(),1u);
  }
  auto restoreOrphans=manager->prepareSourceResourceRetirement(orphanCommand,ResourceLifecyclePhase::Active);
  ASSERT_TRUE(publish(restoreOrphans));restoreOrphans={};
  auto restoreTexture=manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Active);
  ASSERT_TRUE(publish(restoreTexture));EXPECT_TRUE(retained->resourceDependenciesActive());EXPECT_TRUE(Access::dependenciesValid(*manager));
}

TEST_F(SourceResourceRetirementGpu,ActiveSourceAndMaterialEdgesCannotBeClearedToManufactureUnusedResources) {
  const auto reference=add("Referenced source");const auto selected=textures(reference);const auto source=manager->authoredMaterialResources().textures[reference.archiveTexture()];
  const std::array image{TextureResourceSelection{ResourceDomainKind::Archive,ResourceKind::Image,uint32_t(source.source)}};
  const std::array sampler{TextureResourceSelection{ResourceDomainKind::Archive,ResourceKind::Sampler,uint32_t(source.sampler)}};
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(image),std::invalid_argument);
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(sampler),std::invalid_argument);
  container::material::Material manual;manual.baseColorTextureIndex=reference.runtimeResource();
  manager->createMaterial(manual);const auto generation=manager->resourceLifecycleGeneration();
  const auto mapping=Access::exportTexture(*manager,reference.runtimeResource());const auto revision=manager->materialResourceRevision();
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(selected),std::invalid_argument);
  EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);EXPECT_EQ(manager->materialResourceRevision(),revision);
  EXPECT_EQ(Access::exportTexture(*manager,reference.runtimeResource()),mapping);EXPECT_TRUE(Access::requestActive(*manager,0));
}

TEST_F(SourceResourceRetirementGpu,RawDocumentAndBorrowedPlayerStalenessRejectWithoutAllocation) {
  const auto reference=add("Raw witness source");const auto selected=textures(reference);
  auto stage=manager->prepareSourceResourceRetirement(selected);const auto generation=manager->resourceLifecycleGeneration();
  manager->animationSource().extras=tinygltf::Value(tinygltf::Value::Object{{"changed",tinygltf::Value(7)}});
  bool ready=true;size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeSourceResourceRetirement(stage);
    manager->exchangeSourceResourceRetirement(stage);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_EQ(attempts,0u);EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);stage={};
  auto player=std::make_unique<AnimationPlayer>(manager->animationSource());auto borrowed=manager->prepareSourceResourceRetirement(selected,player.get());
  ASSERT_TRUE(manager->canExchangeSourceResourceRetirement(borrowed,player.get()));player.reset();
  {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeSourceResourceRetirement(borrowed);
    manager->exchangeSourceResourceRetirement(borrowed);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_EQ(attempts,0u);EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);borrowed={};
  manager->animationSource().extensions["VENDOR_unknown_resource_schema"]=tinygltf::Value(tinygltf::Value::Object{{"image",tinygltf::Value(0)}});
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(selected),std::invalid_argument);
}

TEST_F(SourceResourceRetirementGpu,AllocationDenialAndBudgetFailurePreserveReadyReceiptAndCurrentEndpoints) {
  const auto reference=add("Atomic source");const auto selected=textures(reference);
  auto ready=manager->prepareSourceResourceRetirement(selected);const auto command=ready.command();
  // Catalog registries retain capacity after an abandoned sibling. Warm that
  // capacity while the earlier receipt stays alive, then deny actual stable
  // preparation allocations rather than a one-time registry growth attempt.
  {auto warm=manager->prepareSourceResourceRetirement(selected);ASSERT_TRUE(manager->canExchangeSourceResourceRetirement(warm));}
  ASSERT_TRUE(manager->canExchangeSourceResourceRetirement(ready));
  const auto measure=[&] {
    size_t attempts{};
    {material_publication_test::AllocationGuard count(false);auto measured=manager->prepareSourceResourceRetirement(selected);attempts=count.allocations();}
    return attempts;
  };
  const auto calls=measure(),confirmed=measure();ASSERT_EQ(calls,confirmed);
  ASSERT_GT(calls,3u);ASSERT_LT(calls,20'000u);
  const auto generation=manager->resourceLifecycleGeneration();const auto revision=manager->materialResourceRevision();
  const auto descriptor=manager->descriptorSet(0);const auto count=allocations();
  for(const auto allowance:std::array<size_t,3>{0,calls/2,calls-1}) {
    SCOPED_TRACE(allowance);
    bool failed=false;{material_publication_test::AllocationGuard deny(false,allowance);
      try {(void)manager->prepareSourceResourceRetirement(selected);} catch(const std::bad_alloc&) {failed=true;}}
    EXPECT_TRUE(failed);EXPECT_TRUE(manager->canExchangeSourceResourceRetirement(ready));
    EXPECT_FALSE(command.published());EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);EXPECT_EQ(manager->materialResourceRevision(),revision);
    EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(allocations(),count);EXPECT_TRUE(Access::requestActive(*manager,0));
  }
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(selected,nullptr,1),std::length_error);
  EXPECT_TRUE(manager->canExchangeSourceResourceRetirement(ready));ASSERT_TRUE(publish(ready));EXPECT_TRUE(command.retired());
}

TEST_F(SourceResourceRetirementGpu,CoexistingRetirementCandidatesInvalidateOnlyAfterSuccessfulPublication) {
  const auto reference=add("Coexisting source candidates");const auto selected=textures(reference);
  const auto before=manager->resourceLifecycleGeneration();const auto revision=manager->materialResourceRevision();
  const auto image=Access::texture(*manager,reference.runtimeResource());
  auto first=manager->prepareSourceResourceRetirement(selected);const auto firstCommand=first.command();
  auto second=manager->prepareSourceResourceRetirement(selected);const auto secondCommand=second.command();
  ASSERT_TRUE(manager->canExchangeSourceResourceRetirement(first));ASSERT_TRUE(manager->canExchangeSourceResourceRetirement(second));
  EXPECT_EQ(manager->resourceLifecycleGeneration(),before);EXPECT_EQ(manager->materialResourceRevision(),revision);
  EXPECT_TRUE(before->active(selected[0]));EXPECT_TRUE(Access::requestActive(*manager,0));
  {auto abandoned=manager->prepareSourceResourceRetirement(selected);EXPECT_TRUE(manager->canExchangeSourceResourceRetirement(abandoned));}
  ASSERT_TRUE(manager->canExchangeSourceResourceRetirement(first));ASSERT_TRUE(manager->canExchangeSourceResourceRetirement(second));
  ASSERT_TRUE(publish(first));EXPECT_TRUE(firstCommand.retired());EXPECT_FALSE(secondCommand.published());
  const auto after=manager->resourceLifecycleGeneration();const auto publishedRevision=manager->materialResourceRevision();
  bool staleReady=true;size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);staleReady=manager->canExchangeSourceResourceRetirement(second);
    manager->exchangeSourceResourceRetirement(second);attempts=deny.allocations();}
  EXPECT_FALSE(staleReady);EXPECT_EQ(attempts,0u);EXPECT_EQ(manager->resourceLifecycleGeneration(),after);
  EXPECT_EQ(manager->materialResourceRevision(),publishedRevision);EXPECT_EQ(publishedRevision,revision+1);
  EXPECT_FALSE(after->active(selected[0]));EXPECT_FALSE(Access::requestActive(*manager,0));
  EXPECT_TRUE(allocation->textureImageMatches(image));EXPECT_TRUE(Access::dependenciesValid(*manager));
}

TEST_F(SourceResourceRetirementGpu,ForeignUnpublishedWrongPhaseAndConsumedSiblingReceiptsReject) {
  const auto reference=add("Receipt authority");const auto selected=textures(reference);
  SceneManager::SourceResourceRetirementCommand abandoned;
  {auto stage=manager->prepareSourceResourceRetirement(selected);abandoned=stage.command();}
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(abandoned,ResourceLifecyclePhase::Active),std::invalid_argument);
  auto stage=manager->prepareSourceResourceRetirement(selected);auto command=stage.command();
  SceneManager foreign(*allocation,*pipelines,device,container::app::AppConfig{});bool foreignReady=true;size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);foreignReady=foreign.canExchangeSourceResourceRetirement(stage);
    foreign.exchangeSourceResourceRetirement(stage);attempts=deny.allocations();}
  EXPECT_FALSE(foreignReady);EXPECT_EQ(attempts,0u);ASSERT_TRUE(manager->canExchangeSourceResourceRetirement(stage));
  ASSERT_TRUE(publish(stage));stage={};
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Retired),std::invalid_argument);
  auto first=manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Active);
  auto sibling=manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Active);
  ASSERT_TRUE(publish(first));first={};const auto generation=manager->resourceLifecycleGeneration();const auto revision=manager->materialResourceRevision();
  bool siblingReady=true;
  {material_publication_test::AllocationGuard deny(true);siblingReady=manager->canExchangeSourceResourceRetirement(sibling);
    manager->exchangeSourceResourceRetirement(sibling);attempts=deny.allocations();}
  EXPECT_FALSE(siblingReady);EXPECT_EQ(attempts,0u);EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);
  EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_FALSE(command.retired());EXPECT_TRUE(Access::dependenciesValid(*manager));
}
TEST_F(SourceResourceRetirementGpu,ImmediateProducerAndMergeClonePreserveSourcePhaseHoles) {
  const auto roots=graph.rootNodes();
  const auto editor=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Scene Root");
  graph.getNode(editor)->editorSceneRoot=true;
  for(const auto root:roots) ASSERT_TRUE(graph.setParent(root,editor));
  graph.updateWorldTransforms();
  const auto original=add("Retired prefix");const auto selected=textures(original);
  const auto firstHandle=manager->resourceLifecycleGeneration()->resource(selected[0]);
  auto retire=manager->prepareSourceResourceRetirement(selected);const auto command=retire.command();
  ASSERT_TRUE(publish(retire));retire={};
  const auto current=manager->loadMaterialTextureSourceFromBytes("Immediate after retirement",png(17),true);
  manager->uploadMaterialResources();
  EXPECT_NE(current.archiveTexture(),original.archiveTexture());
  EXPECT_EQ(manager->authoredMaterialResources().textureActive.size(),2u);
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(selected[0]));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Archive,ResourceKind::Texture,current.archiveTexture()}));
  EXPECT_TRUE(Access::dependenciesValid(*manager));
  uint32_t root=SceneGraph::kInvalidNode;
  for(uint32_t row=0;row<graph.nodeCount();++row) if(graph.getNode(row)->sourceNodeIndex==0) {root=row;break;}
  ASSERT_NE(root,SceneGraph::kInvalidNode);
  auto clone=manager->prepareSubtreeCloneGeneration(graph,root,"Phase clone");
  ASSERT_TRUE(clone.readyForExchange(*manager,graph));
  manager->exchangeMergeGeneration(clone);
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(selected[0]));
  EXPECT_EQ(manager->resourceLifecycleGeneration()->resource(selected[0]),firstHandle);
  clone.prepareResourceDependencies(*manager);manager->exchangeMergeGeneration(clone);
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(selected[0]));
  tinygltf::TinyGLTF writer;auto source=triangle();const auto path=temp/"incoming.glb";
  ASSERT_TRUE(writer.WriteGltfSceneToFile(&source,path.string(),true,true,true,true));
  auto incoming=LoadModelWithSource(path.string());
  auto merge=manager->prepareMergeGeneration(incoming,path,{},graph);
  ASSERT_TRUE(merge.readyForExchange(*manager,graph));manager->exchangeMergeGeneration(merge);
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(selected[0]));
  EXPECT_NE(manager->resourceLifecycleGeneration()->resource(selected[0]),firstHandle);
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Active),std::invalid_argument);
  merge.prepareResourceDependencies(*manager);manager->exchangeMergeGeneration(merge);
  EXPECT_EQ(manager->resourceLifecycleGeneration()->resource(selected[0]),firstHandle);
  auto restore=manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Active);
  ASSERT_TRUE(publish(restore));EXPECT_TRUE(Access::dependenciesValid(*manager));
}

TEST_F(SourceResourceRetirementGpu,TrustedUnusedSourceAppendExtendsExplicitPhasesBeforeFreshPreparation) {
  const auto original=add("Retired source image");const auto selected=textures(original);
  const auto image=uint32_t(manager->authoredMaterialResources().textures[original.archiveTexture()].source);
  const auto evaluated=uint32_t(manager->animationSource().textures[original.evaluatedTexture().value()].source);
  auto textureRetire=manager->prepareSourceResourceRetirement(selected);ASSERT_TRUE(publish(textureRetire));textureRetire={};
  const std::array images{TextureResourceSelection{ResourceDomainKind::Archive,ResourceKind::Image,image},
      TextureResourceSelection{ResourceDomainKind::Evaluated,ResourceKind::Image,evaluated}};
  auto imageRetire=manager->prepareSourceResourceRetirement(images);const auto command=imageRetire.command();
  ASSERT_TRUE(publish(imageRetire));imageRetire={};
  auto added=manager->animationSource().images[evaluated];added.name="Independent evaluated append";
  const auto row=uint32_t(manager->animationSource().images.size());manager->animationSource().images.push_back(std::move(added));
  // The public resource preparation must normalize before capturing its own
  // before phases; a separate retention/refresh call must not hide that order.
  auto fresh=manager->prepareMaterialTextureBytes(0,"Direct fresh preparation",png(23),MaterialTextureRoles::Color);
  ASSERT_NO_THROW(fresh.sealLiveDependencies());EXPECT_TRUE(manager->canExchangeMaterialResources(fresh));fresh={};
  auto retained=manager->retainSceneResourceReferences();ASSERT_TRUE(retained);EXPECT_TRUE(retained->valid());
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Evaluated,ResourceKind::Image,row}));
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(images[1]));
  auto restore=manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Active);ASSERT_TRUE(publish(restore));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active(images[1]));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Evaluated,ResourceKind::Image,row}));
}

TEST_F(SourceResourceRetirementGpu,RuntimeRetirementSamplesFallbackAndReleasesOnlyAfterSelectedHistoryAndReadersDrop) {
  const auto first=add("Unique selected runtime",255),survivor=add("Protected runtime",17);
  const auto selectedImage=Access::texture(*manager,first.runtimeResource());const auto survivorImage=Access::texture(*manager,survivor.runtimeResource());
  const auto fallback=Access::fallback(*manager);const auto before=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()));
  const auto count=allocations();const auto bytes=allocatedBytes();const auto epoch=manager->generationEpoch();
  const std::array selected{TextureResourceSelection{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,first.runtimeResource()}};
  auto source=manager->prepareSourceResourceRetirement(selected);auto command=source.command();
  ASSERT_TRUE(source.runtimeChanged());ASSERT_TRUE(command.textureImageOwnership());
  EXPECT_EQ(command.textureImageOwnership()->imageCount(),1u);
  EXPECT_FALSE(container::material::TextureImageOwnershipContains(command.textureImageOwnership(),survivorImage,*allocation));
  EXPECT_FALSE(manager->canExchangeSourceResourceRetirement(source));
  auto bindings=manager->prepareSceneBindings(source);bindings.prepareDescriptorFrames(cameras,objects);
  const auto future=sample(bindings.descriptorSet(0),uint32_t(source.textureResourceCount()));
  EXPECT_EQ(future[first.runtimeResource()*2],glm::vec4(1));EXPECT_EQ(future[survivor.runtimeResource()*2],before[survivor.runtimeResource()*2]);
  ASSERT_TRUE(publishRuntime(source,bindings));EXPECT_EQ(manager->generationEpoch(),epoch+1);
  const auto live=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()));
  EXPECT_EQ(live[first.runtimeResource()*2],glm::vec4(1));EXPECT_EQ(live[survivor.runtimeResource()*2],before[survivor.runtimeResource()*2]);
  EXPECT_TRUE(allocation->textureImageMatches(selectedImage));source={};bindings={};
  EXPECT_TRUE(allocation->textureImageMatches(selectedImage)); // The selected command alone still owns the image.
  EXPECT_EQ(allocations(),count);command={};
  EXPECT_FALSE(allocation->textureImageMatches(selectedImage));EXPECT_EQ(allocations()+1,count);EXPECT_LT(allocatedBytes(),bytes);
  EXPECT_TRUE(allocation->textureImageMatches(survivorImage));EXPECT_TRUE(allocation->textureImageMatches(fallback));
}

TEST_F(SourceResourceRetirementGpu,RuntimeImageAliasKeepsSharedAllocationAndRoleOnlyRetirementPreservesSource) {
  const auto first=add("Shared image");const auto image=Access::texture(*manager,first.runtimeResource());
  const auto nearest=container::material::NormalizeMaterialSamplerKey(9728,9728,33071,33071).value;
  auto resources=manager->prepareMaterialTextureSource(0,first.archiveTexture(),MaterialTextureRoles::Color,nearest);
  const auto alias=resources.reference(true);resources.sealLiveDependencies();ASSERT_TRUE(manager->canExchangeMaterialResources(resources));
  manager->exchangeMaterialResources(resources);resources={};manager->updateDescriptorSets(cameras,objects[0]);
  ASSERT_NE(alias.runtimeResource(),first.runtimeResource());const auto aliasImage=Access::texture(*manager,alias.runtimeResource());
  ASSERT_EQ(aliasImage.allocationIdentity,image.allocationIdentity);
  const auto count=allocations();const std::array selected{TextureResourceSelection{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,first.runtimeResource()}};
  auto source=manager->prepareSourceResourceRetirement(selected);auto command=source.command();
  auto bindings=manager->prepareSceneBindings(source);bindings.prepareDescriptorFrames(cameras,objects);ASSERT_TRUE(publishRuntime(source,bindings));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Archive,ResourceKind::Texture,first.archiveTexture()}));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,alias.runtimeResource()}));
  source={};bindings={};command={};EXPECT_TRUE(allocation->textureImageMatches(image));EXPECT_EQ(allocations(),count);
  const auto pixels=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()));
  EXPECT_EQ(pixels[first.runtimeResource()*2],glm::vec4(1));EXPECT_NE(pixels[alias.runtimeResource()*2],glm::vec4(1));
}

TEST_F(SourceResourceRetirementGpu,ProvedSourceOrphanPlanIsCpuOnlyAndPreservesUnrelatedResources) {
  const auto first=add("Orphan source",255),other=add("Other source",17);const auto firstImage=Access::texture(*manager,first.runtimeResource());
  const auto otherImage=Access::texture(*manager,other.runtimeResource());const auto row=manager->authoredMaterialResources().textures[first.archiveTexture()];
  const auto count=allocations();const auto revision=manager->materialResourceRevision();const auto generation=manager->resourceLifecycleGeneration();
  const auto plan=manager->planResourceRetirement({ResourceDomainKind::Archive,ResourceKind::Texture,first.archiveTexture()});
  EXPECT_EQ(allocations(),count);EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);
  EXPECT_NE(std::ranges::find(plan,TextureResourceSelection{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,first.runtimeResource()}),plan.end());
  EXPECT_NE(std::ranges::find(plan,TextureResourceSelection{ResourceDomainKind::Archive,ResourceKind::Image,uint32_t(row.source)}),plan.end());
  EXPECT_EQ(std::ranges::find(plan,TextureResourceSelection{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,other.runtimeResource()}),plan.end());
  auto source=manager->prepareSourceResourceRetirement(plan);auto command=source.command();
  auto bindings=manager->prepareSceneBindings(source);bindings.prepareDescriptorFrames(cameras,objects);ASSERT_TRUE(publishRuntime(source,bindings));
  source={};bindings={};command={};EXPECT_FALSE(allocation->textureImageMatches(firstImage));EXPECT_TRUE(allocation->textureImageMatches(otherImage));
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Archive,ResourceKind::Image,uint32_t(row.source)}));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Archive,ResourceKind::Texture,other.archiveTexture()}));
  const auto exported=manager->buildSceneExport(graph);EXPECT_EQ(exported.textures.size(),1u);EXPECT_EQ(exported.images.size(),1u);EXPECT_EQ(exported.samplers.size(),1u);
}

TEST_F(SourceResourceRetirementGpu,UnusedColorRuntimeCanRetireWhileTheSameSourceTextureStillFeedsAnActiveDataRole) {
  auto resources=manager->prepareMaterialTextureBytes(0,"Independent color and data",png(),MaterialTextureRoles::ColorAndData);
  const auto color=resources.reference(true),data=resources.reference(false);resources.sealLiveDependencies();
  ASSERT_TRUE(manager->canExchangeMaterialResources(resources));manager->exchangeMaterialResources(resources);resources={};
  GltfMaterialTextureEdit texture;texture.slot=GltfMaterialTextureSlot::Normal;texture.operation=GltfMaterialTextureOperation::Bind;
  texture.binding=GltfMaterialTextureSourcePair{int32_t(data.archiveTexture()),int32_t(data.evaluatedTexture().value())};
  GltfMaterialPatch patch;patch.textures.push_back(texture);auto material=manager->prepareMaterialGeneration(manager->planMaterialTextureEdit(0,patch),nullptr);
  material.prepareDescriptorFrames(cameras,objects);ASSERT_TRUE(manager->canExchangeMaterialGeneration(material,nullptr));size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);manager->exchangeMaterialGeneration(material,nullptr);attempts=deny.allocations();}
  ASSERT_FALSE(material);EXPECT_EQ(attempts,0u);material={};
  const auto before=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()));
  const std::array used{TextureResourceSelection{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,data.runtimeResource()}};
  EXPECT_THROW((void)manager->prepareSourceResourceRetirement(used),std::invalid_argument);
  const std::array selected{TextureResourceSelection{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,color.runtimeResource()}};
  auto source=manager->prepareSourceResourceRetirement(selected);auto command=source.command();
  auto bindings=manager->prepareSceneBindings(source);bindings.prepareDescriptorFrames(cameras,objects);ASSERT_TRUE(publishRuntime(source,bindings));
  const auto roles=manager->authoredMaterialResources();EXPECT_EQ(roles.colorResources[color.archiveTexture()],UINT32_MAX);
  EXPECT_EQ(roles.dataResources[data.archiveTexture()],data.runtimeResource());EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[0]));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active({ResourceDomainKind::Archive,ResourceKind::Texture,color.archiveTexture()}));
  const auto after=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()));
  EXPECT_EQ(after[color.runtimeResource()*2],glm::vec4(1));EXPECT_EQ(after[data.runtimeResource()*2],before[data.runtimeResource()*2]);
  EXPECT_TRUE(Access::dependenciesValid(*manager));
}

TEST_F(SourceResourceRetirementGpu,RuntimeDescriptorWitnessesSurviveNewerAbandonmentAndRejectAllAbandonedFrames) {
  const auto reference=add("Binding witness");const std::array selected{TextureResourceSelection{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,reference.runtimeResource()}};
  auto source=manager->prepareSourceResourceRetirement(selected);
  auto first=manager->prepareSceneBindings(source);first.prepareDescriptorFrames(cameras,objects);
  {auto newer=manager->prepareSceneBindings(source);newer.prepareDescriptorFrames(cameras,objects);EXPECT_TRUE(manager->canExchangeSceneBindings(newer));}
  EXPECT_TRUE(manager->canExchangeSourceResourceRetirement(source));EXPECT_TRUE(manager->canExchangeSceneBindings(first));first={};
  EXPECT_FALSE(manager->canExchangeSourceResourceRetirement(source));EXPECT_TRUE(source.publicationMatches(*manager));
  auto replacement=manager->prepareSceneBindings(source);replacement.prepareDescriptorFrames(cameras,objects);
  ASSERT_TRUE(publishRuntime(source,replacement));EXPECT_FALSE(source.textureActive(reference.runtimeResource()));EXPECT_EQ(source.textureResourceCount(),0u);
}

TEST_F(SourceResourceRetirementGpu,RuntimeOnlySceneWithNoAuthoredMaterialZeroPreparesRestoresAndSamples) {
  auto zero=triangle();zero.materials.clear();zero.meshes[0].primitives[0].material=-1;tinygltf::TinyGLTF writer;
  const auto path=temp/"no-authored-material.glb";ASSERT_TRUE(writer.WriteGltfSceneToFile(&zero,path.string(),true,true,true,true));
  manager.reset();manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});manager->initialize(path.string(),1,2);
  ASSERT_TRUE(manager->sourceMaterialRuntimeMapping().empty());
  const auto reference=manager->loadMaterialTextureSourceFromBytes("No source material zero",png(),true);manager->uploadMaterialResources();manager->updateDescriptorSets(cameras,objects[0]);
  const auto before=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()));
  const auto plan=manager->planResourceRetirement({ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,reference.runtimeResource()});
  auto source=manager->prepareSourceResourceRetirement(plan);auto command=source.command();auto bindings=manager->prepareSceneBindings(source);
  bindings.prepareDescriptorFrames(cameras,objects);ASSERT_TRUE(publishRuntime(source,bindings));source={};bindings={};
  EXPECT_EQ(sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()))[reference.runtimeResource()*2],glm::vec4(1));
  auto restore=manager->prepareSourceResourceRetirement(command,ResourceLifecyclePhase::Active);auto restoredBindings=manager->prepareSceneBindings(restore);
  restoredBindings.prepareDescriptorFrames(cameras,objects);ASSERT_TRUE(publishRuntime(restore,restoredBindings));
  EXPECT_EQ(sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()))[reference.runtimeResource()*2],before[reference.runtimeResource()*2]);
  EXPECT_TRUE(manager->sourceMaterialRuntimeMapping().empty());EXPECT_TRUE(Access::dependenciesValid(*manager));
}

TEST_F(SourceResourceRetirementGpu,ImportedTailHistoryPreservesTheOriginalDeleteReceiptThroughCutAndRestore) {
  const auto prefix=add("Cohort A0",255);
  const auto binding=[](const SceneManager::MaterialTextureReference& reference) {
    GltfMaterialTextureEdit edit;edit.slot=GltfMaterialTextureSlot::BaseColor;edit.operation=GltfMaterialTextureOperation::Bind;
    edit.binding=GltfMaterialTextureSourcePair{int32_t(reference.archiveTexture()),int32_t(reference.evaluatedTexture().value())};
    GltfMaterialPatch patch;patch.textures.push_back(edit);return patch;
  };
  auto initial=manager->prepareMaterialGeneration(manager->planMaterialTextureEdit(0,binding(prefix)),nullptr);
  ASSERT_TRUE(publishMaterial(initial));initial={};
  auto resources=manager->prepareMaterialTextureBytes(0,"Imported cohort A1",png(17),MaterialTextureRoles::Color);
  const auto imported=resources.reference(true);const auto importCommand=resources.command();
  const auto importPlan=manager->planMaterialTextureEdit(0,binding(imported),&resources);
  auto importedMaterial=manager->prepareMaterialGeneration(importPlan,nullptr,std::move(resources));
  ASSERT_TRUE(publishMaterial(importedMaterial));importedMaterial={};
  const TextureResourceSelection runtime{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,imported.runtimeResource()};
  const auto originalHandle=manager->resourceLifecycleGeneration()->resource(runtime);
  GltfMaterialTextureEdit clear;clear.slot=GltfMaterialTextureSlot::BaseColor;clear.operation=GltfMaterialTextureOperation::Clear;
  GltfMaterialPatch clearPatch;clearPatch.textures.push_back(clear);const auto clearPlan=manager->planMaterialTextureEdit(0,clearPatch);
  auto cleared=manager->prepareMaterialGeneration(clearPlan,nullptr);ASSERT_TRUE(publishMaterial(cleared));cleared={};
  const auto selection=manager->planResourceRetirement({ResourceDomainKind::Archive,ResourceKind::Texture,imported.archiveTexture()});
  auto removed=manager->prepareSourceResourceRetirement(selection);const auto deleteCommand=removed.command();
  auto removedBindings=manager->prepareSceneBindings(removed);removedBindings.prepareDescriptorFrames(cameras,objects);
  ASSERT_TRUE(publishRuntime(removed,removedBindings));removed={};removedBindings={};
  auto undoDelete=manager->prepareSourceResourceRetirement(deleteCommand,ResourceLifecyclePhase::Active);
  auto undoDeleteBindings=manager->prepareSceneBindings(undoDelete);undoDeleteBindings.prepareDescriptorFrames(cameras,objects);
  ASSERT_TRUE(publishRuntime(undoDelete,undoDeleteBindings));undoDelete={};undoDeleteBindings={};
  auto undoClear=manager->prepareMaterialGeneration(manager->rebaseMaterialTextureEdit(clearPlan,GltfMaterialEditSide::Before),nullptr);
  ASSERT_TRUE(publishMaterial(undoClear));undoClear={};
  auto cut=manager->prepareMaterialResourcesChange(importCommand,false);
  const auto beforeImport=manager->rebaseMaterialTextureEdit(importPlan,GltfMaterialEditSide::Before,&cut);
  auto undoImport=manager->prepareMaterialGeneration(beforeImport,nullptr,std::move(cut));ASSERT_TRUE(publishMaterial(undoImport));undoImport={};
  EXPECT_EQ(manager->materialTextureResourceCount(),imported.runtimeResource());
  auto restore=manager->prepareMaterialResourcesChange(importCommand,true);
  const auto afterImport=manager->rebaseMaterialTextureEdit(importPlan,GltfMaterialEditSide::After,&restore);
  auto redoImport=manager->prepareMaterialGeneration(afterImport,nullptr,std::move(restore));ASSERT_TRUE(publishMaterial(redoImport));redoImport={};
  EXPECT_EQ(manager->resourceLifecycleGeneration()->resource(runtime),originalHandle);
  auto redoClear=manager->prepareMaterialGeneration(manager->rebaseMaterialTextureEdit(clearPlan,GltfMaterialEditSide::After),nullptr);
  ASSERT_TRUE(publishMaterial(redoClear));redoClear={};
  auto redoDelete=manager->prepareSourceResourceRetirement(deleteCommand,ResourceLifecyclePhase::Retired);
  auto redoDeleteBindings=manager->prepareSceneBindings(redoDelete);redoDeleteBindings.prepareDescriptorFrames(cameras,objects);
  ASSERT_TRUE(publishRuntime(redoDelete,redoDeleteBindings));EXPECT_TRUE(deleteCommand.retired());
  EXPECT_EQ(manager->resourceLifecycleGeneration()->resource(runtime),originalHandle);EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(runtime));
  EXPECT_EQ(sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()))[imported.runtimeResource()*2],glm::vec4(1));
  EXPECT_TRUE(Access::dependenciesValid(*manager));
}

TEST_F(SourceResourceRetirementGpu,LoadedCoreLegacyAliasesReplaceRetireAndUndoThroughPublishedMaterialRecords) {
  auto loaded=triangle();loaded.images.resize(1);auto& image=loaded.images[0];
  image.width=image.height=1;image.component=4;image.bits=8;image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.image={255,127,63,255};image.uri="loaded-core.png";loaded.samplers.resize(1);
  loaded.textures.resize(1);loaded.textures[0].source=0;loaded.textures[0].sampler=0;
  loaded.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
  tinygltf::TinyGLTF writer;const auto path=temp/"loaded-core.glb";
  ASSERT_TRUE(writer.WriteGltfSceneToFile(&loaded,path.string(),true,true,true,true));
  manager.reset();manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});
  manager->initialize(path.string(),1,2);manager->updateDescriptorSets(cameras,objects[0]);
  AnimationPlayer player(manager->animationSource());ASSERT_EQ(manager->sourceMaterialRuntimeMapping().size(),1u);
  const auto runtimeMaterial=manager->sourceMaterialRuntimeMapping()[0];
  const auto originalAuthored=manager->authoredMaterial(0),originalEvaluated=manager->animationSource().materials.at(0);
  const auto originalExported=Access::exportedMaterial(*manager,runtimeMaterial),originalRest=player.library().restMaterials.at(0);
  // These mirrors must come from the real loader, rather than being supplied
  // by a test-only write to the live resource catalog.
  ASSERT_TRUE(originalAuthored.values.contains("baseColorTexture"));
  ASSERT_TRUE(originalEvaluated.values.contains("baseColorTexture"));
  ASSERT_TRUE(originalExported.values.contains("baseColorTexture"));
  ASSERT_EQ(manager->authoredMaterialResources().colorResources.size(),1u);
  const auto originalRuntime=manager->authoredMaterialResources().colorResources[0];ASSERT_NE(originalRuntime,UINT32_MAX);
  ASSERT_LT(originalRuntime,manager->materialTextureResourceCount());
  const auto originalImage=Access::texture(*manager,uint32_t(originalRuntime));
  const auto originalSample=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()))[size_t(originalRuntime)*2];
  EXPECT_FLOAT_EQ(originalSample.r,1.f);EXPECT_LT(originalSample.g,.5f);EXPECT_LT(originalSample.b,.1f);EXPECT_FLOAT_EQ(originalSample.a,1.f);
  auto resources=manager->prepareMaterialTextureBytes(0,"Loaded replacement A1",png(17),MaterialTextureRoles::Color);
  const auto replacement=resources.reference(true);const auto resourceCommand=resources.command();
  GltfMaterialTextureEdit edit;edit.slot=GltfMaterialTextureSlot::BaseColor;edit.operation=GltfMaterialTextureOperation::Bind;
  edit.binding=GltfMaterialTextureSourcePair{int32_t(replacement.archiveTexture()),int32_t(replacement.evaluatedTexture().value())};
  GltfMaterialPatch patch;patch.textures.push_back(edit);const auto replacePlan=manager->planMaterialTextureEdit(0,patch,&resources);
  auto material=manager->prepareMaterialGeneration(replacePlan,&player,std::move(resources));
  ASSERT_TRUE(publishMaterial(material,&player));material={};
  for(const auto* published:std::array<const tinygltf::Material*,4>{&manager->authoredMaterial(0),&manager->animationSource().materials.at(0),
      &Access::exportedMaterial(*manager,runtimeMaterial),&player.library().restMaterials.at(0)}) {
    EXPECT_FALSE(published->values.contains("baseColorTexture"));EXPECT_FALSE(published->additionalValues.contains("baseColorTexture"));
    EXPECT_GE(published->pbrMetallicRoughness.baseColorTexture.index,1);
  }
  EXPECT_EQ(manager->authoredMaterial(0).pbrMetallicRoughness.baseColorTexture.index,int(replacement.archiveTexture()));
  EXPECT_EQ(manager->animationSource().materials[0].pbrMetallicRoughness.baseColorTexture.index,int(replacement.evaluatedTexture().value()));
  EXPECT_TRUE(Access::dependenciesValid(*manager));
  const auto replacementSample=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()))[replacement.runtimeResource()*2];
  EXPECT_LT(replacementSample.r,.1f);EXPECT_NE(replacementSample,originalSample);
  const TextureResourceSelection originalTexture{ResourceDomainKind::Archive,ResourceKind::Texture,0};
  const auto selected=manager->planResourceRetirement(originalTexture,&player);
  const TextureResourceSelection originalRuntimeSelection{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,uint32_t(originalRuntime)};
  ASSERT_NE(std::ranges::find(selected,originalRuntimeSelection),selected.end());
  auto retirement=manager->prepareSourceResourceRetirement(selected,&player);const auto deleteCommand=retirement.command();
  auto retiredBindings=manager->prepareSceneBindings(retirement,{},&player);retiredBindings.prepareDescriptorFrames(cameras,objects,{},&player);
  ASSERT_TRUE(publishRuntime(retirement,retiredBindings,&player));retirement={};retiredBindings={};
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(originalTexture));
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->active(originalRuntimeSelection));
  const auto retiredSamples=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()));
  EXPECT_EQ(retiredSamples[size_t(originalRuntime)*2],glm::vec4(1));EXPECT_EQ(retiredSamples[replacement.runtimeResource()*2],replacementSample);
  EXPECT_TRUE(allocation->textureImageMatches(originalImage));EXPECT_TRUE(Access::dependenciesValid(*manager));
  auto undoDelete=manager->prepareSourceResourceRetirement(deleteCommand,ResourceLifecyclePhase::Active,&player);
  auto restoredBindings=manager->prepareSceneBindings(undoDelete,{},&player);restoredBindings.prepareDescriptorFrames(cameras,objects,{},&player);
  ASSERT_TRUE(publishRuntime(undoDelete,restoredBindings,&player));undoDelete={};restoredBindings={};
  EXPECT_EQ(sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()))[size_t(originalRuntime)*2],originalSample);
  auto removeReplacement=manager->prepareMaterialResourcesChange(resourceCommand,false);
  const auto beforeReplace=manager->rebaseMaterialTextureEdit(replacePlan,GltfMaterialEditSide::Before,&removeReplacement);
  auto undoReplace=manager->prepareMaterialGeneration(beforeReplace,&player,std::move(removeReplacement));
  ASSERT_TRUE(publishMaterial(undoReplace,&player));undoReplace={};
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),originalAuthored));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[0],originalEvaluated));
  EXPECT_TRUE(GltfMaterialRecordsEqual(Access::exportedMaterial(*manager,runtimeMaterial),originalExported));
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[0],originalRest));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->active(originalTexture));EXPECT_TRUE(manager->resourceLifecycleGeneration()->active(originalRuntimeSelection));
  EXPECT_EQ(sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()))[size_t(originalRuntime)*2],originalSample);
  EXPECT_TRUE(Access::dependenciesValid(*manager));
}
} // namespace
