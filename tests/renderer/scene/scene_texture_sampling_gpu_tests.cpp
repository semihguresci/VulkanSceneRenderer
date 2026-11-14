#include <Container/common/CommonGLFW.h>
#include <Container/geometry/GltfModelLoader.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneGraph.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/TextureImageOwnership.h>
#include <Container/utility/TextureSamplingPlan.h>
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
#include <span>
#include <vector>

#ifndef CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH
#define CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH "spv_shaders/scene_texture_sampling_contract.comp.spv"
#endif

namespace container::scene {
// Fixture-only access to an otherwise unreachable intermediate lifecycle state.
// It does not add a production Delete API or authorize referenced image removal.
struct SceneTextureSamplingTestAccess {
  static auto& table(SceneManager& manager) { return manager.textureManager_; }
  static const auto& fallback(const SceneManager& manager) { return manager.textureSamplingFallback_->resource; }
  static const auto& samplingPlan(const SceneManager& manager) { return manager.textureSamplingPlan_; }
  static const auto& metadataBuffer(const SceneManager& manager) { return manager.textureMetadataBuffer_; }
  static auto fallbackOwner(const SceneManager& manager) { return manager.textureSamplingFallback_; }
  static void dropFallbackOwner(SceneManager& manager) { manager.textureSamplingFallback_.reset(); }
  static const auto& imageOwner(const SceneManager& manager) { return manager.textureImageOwnership_; }
  static void dropImageOwner(SceneManager& manager) { manager.textureImageOwnership_.reset(); }
  static void refreshImageOwner(SceneManager& manager) {
    const std::array inherited{manager.textureImageOwnership_};
    manager.textureImageOwnership_=container::material::PrepareTextureImageOwnership(
        manager.textureManager_,*manager.allocationManager_,inherited);
  }
  static void install(SceneManager& manager,container::material::TextureManager table,
      std::shared_ptr<const container::gpu::AllocationManager::TextureAllocationLease> lease={}) {
    const std::array inherited{manager.textureImageOwnership_};
    auto images=container::material::PrepareTextureImageOwnership(table,*manager.allocationManager_,inherited);
    using std::swap;swap(manager.textureManager_,table);
    manager.textureImageOwnership_.swap(images);
    lease.reset(); // Every committed batch member now has its independent pin.
    manager.exportTextureIndices_.resize(manager.textureManager_.textureCount(),-1);
    ++manager.materialResourceRevision_;
    manager.uploadTextureMetadataBuffer();
  }
  static bool retire(SceneManager& manager,uint32_t slot,bool refresh=true,bool bumpResourceRevision=true) {
    if(!manager.textureManager_.retireTexture(slot)) return false;
    if(bumpResourceRevision) ++manager.materialResourceRevision_;
    if(refresh) {
      refreshImageOwner(manager);
      // Explicit fixture-only invalidation models a lower-level phase change;
      // production unused-resource retirement keeps its default blocking policy.
      manager.refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations);
      manager.uploadTextureMetadataBuffer();
    }
    return true;
  }
  static void refresh(SceneManager& manager) { manager.uploadTextureMetadataBuffer(); }
};
} // namespace container::scene

namespace {
using namespace container::scene;
using namespace container::geometry;
using namespace container::geometry::gltf;
using namespace container::gpu;
namespace material=container::material;
using Access=SceneTextureSamplingTestAccess;

TEST(SceneTextureSamplingAllocations,TinyGltfModelSwapNeedsPreconstructedScratchOnMsvc) {
  tinygltf::Model left,right,scratch;left.asset.version=right.asset.version="2.0";
  left.asset.generator="Left allocator witness";right.asset.generator="Right allocator witness";
  left.asset.extras=tinygltf::Value(tinygltf::Value::Object{{"exactInteger",tinygltf::Value(16777217)}});
  right.asset.extensions["VENDOR_asset"]=tinygltf::Value(tinygltf::Value::Object{{"opaque",tinygltf::Value(true)}});
  left.extras=tinygltf::Value(tinygltf::Value::Object{{"keep",tinygltf::Value(std::string("left extras"))}});
  right.extensions["VENDOR_root"]=tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(19)});
  left.buffers.resize(1);left.buffers[0].data={1,2,3,4};right.images.resize(1);right.images[0].image={9,8,7,6};
  left.audioEmitters.resize(1);left.audioEmitters[0].source=0;
  right.audioSources.resize(1);right.audioSources[0].name="Opaque audio source";right.audioSources[0].uri="retained-audio.mp3";
  left.extras_json_string="left raw extras";right.extensions_json_string="right raw extensions";
  const auto beforeLeft=left,beforeRight=right;
  size_t genericAttempts{};
  {material_publication_test::AllocationGuard count(false);std::swap(left,right);genericAttempts=count.allocations();}
  EXPECT_EQ(left,beforeRight);EXPECT_EQ(right,beforeLeft);
  EXPECT_EQ(left.audioSources,beforeRight.audioSources);EXPECT_EQ(right.audioEmitters,beforeLeft.audioEmitters);
  EXPECT_EQ(left.extensions_json_string,beforeRight.extensions_json_string);EXPECT_EQ(right.extras_json_string,beforeLeft.extras_json_string);
#ifdef _MSC_VER
  EXPECT_GT(genericAttempts,0u);
#endif
  size_t assignmentAttempts{};
  {material_publication_test::AllocationGuard deny(true);scratch=std::move(left);left=std::move(right);right=std::move(scratch);assignmentAttempts=deny.allocations();}
  EXPECT_EQ(assignmentAttempts,0u);EXPECT_EQ(left,beforeLeft);EXPECT_EQ(right,beforeRight);
  EXPECT_EQ(left.audioEmitters,beforeLeft.audioEmitters);EXPECT_EQ(right.audioSources,beforeRight.audioSources);
  EXPECT_EQ(left.extras_json_string,beforeLeft.extras_json_string);EXPECT_EQ(right.extensions_json_string,beforeRight.extensions_json_string);
  std::fprintf(stderr,"TinyGLTF Model allocation witness: generic swap %zu, preconstructed move assignments %zu\n",genericAttempts,assignmentAttempts);
}

GltfLoadResult triangle(const std::string& name) {
  GltfLoadResult result;auto& source=result.gltfModel;source.asset.version="2.0";
  const std::array<float,9> positions{0,0,0,1,0,0,0,1,0};
  const std::array<uint16_t,3> indices{0,1,2};
  source.buffers.resize(1);source.buffers[0].data.resize(sizeof(positions)+sizeof(indices));
  std::memcpy(source.buffers[0].data.data(),positions.data(),sizeof(positions));
  std::memcpy(source.buffers[0].data.data()+sizeof(positions),indices.data(),sizeof(indices));
  source.bufferViews.resize(2);source.bufferViews[0].buffer=source.bufferViews[1].buffer=0;
  source.bufferViews[0].byteLength=sizeof(positions);source.bufferViews[1].byteOffset=sizeof(positions);
  source.bufferViews[1].byteLength=sizeof(indices);
  source.accessors.resize(2);source.accessors[0].bufferView=0;source.accessors[0].componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  source.accessors[0].type=TINYGLTF_TYPE_VEC3;source.accessors[0].count=3;source.accessors[0].minValues={0,0,0};source.accessors[0].maxValues={1,1,0};
  source.accessors[1].bufferView=1;source.accessors[1].componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
  source.accessors[1].type=TINYGLTF_TYPE_SCALAR;source.accessors[1].count=3;
  source.materials.resize(1);source.materials[0].name=name;source.materials[0].pbrMetallicRoughness.roughnessFactor=.7;
  source.meshes.resize(1);tinygltf::Primitive primitive;primitive.attributes["POSITION"]=0;primitive.indices=1;primitive.material=0;
  primitive.mode=TINYGLTF_MODE_TRIANGLES;source.meshes[0].primitives.push_back(primitive);
  source.nodes.resize(1);source.nodes[0].mesh=0;source.nodes[0].name=name;
  source.scenes.resize(1);source.scenes[0].nodes={0};source.defaultScene=0;
  std::vector<Vertex> vertices(3);for(size_t i=0;i<3;++i) {
    vertices[i].position={positions[i*3],positions[i*3+1],positions[i*3+2]};vertices[i].normal={0,0,1};
  }
  result.model=Model::FromMeshes({Mesh(std::move(vertices),{0,1,2},0)});return result;
}

class SceneTextureSamplingGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<SceneTextureSamplingGpu*>(user)->errors;std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_SCENE_TEXTURE_SAMPLING");
    if(!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_SCENE_TEXTURE_SAMPLING=1";
    ASSERT_TRUE(std::filesystem::is_regular_file(CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH));
    ASSERT_EQ(glfwInit(),GLFW_TRUE);glfwStarted=true;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    window=glfwCreateWindow(32,32,"Scene texture tombstone sampling",nullptr,nullptr);ASSERT_NE(window,nullptr);
    uint32_t count{};const auto* extensions=glfwGetRequiredInstanceExtensions(&count);ASSERT_NE(extensions,nullptr);
    InstanceCreateInfo create;create.enableValidationLayers=true;create.validationLayers={"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions,extensions+count);create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};validation.enabledValidationFeatureCount=1;validation.pEnabledValidationFeatures=&sync;create.next=&validation;
    instance=std::make_unique<VulkanInstance>(create);
    vk::DebugUtilsMessengerCreateInfoEXT debug;debug.messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation|vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback=reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(debugCallback);debug.pUserData=this;
    messenger=vk::raii::DebugUtilsMessengerEXT(instance->raii(),debug);
    VkSurfaceKHR rawSurface{};ASSERT_EQ(glfwCreateWindowSurface(instance->instance(),window,nullptr,&rawSurface),VK_SUCCESS);
    surface=vk::raii::SurfaceKHR(instance->raii(),rawSurface);
    DeviceCreateInfo info;info.requiredExtensions={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress=VK_TRUE;v12.runtimeDescriptorArray=VK_TRUE;v12.descriptorBindingPartiallyBound=VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing=VK_TRUE;
    v12.shaderStorageBufferArrayNonUniformIndexing=VK_TRUE;info.next=&v12;
    device=std::make_shared<VulkanDevice>(*instance,rawSurface,info);
    pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,device->queueFamilyIndices().graphicsFamily.value()));
    allocation=std::make_unique<AllocationManager>();allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
    pipelines=std::make_unique<PipelineManager>(device->device());
    temp=std::filesystem::temp_directory_path()/("container-scene-texture-sampling-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);
    auto source=triangle("Sampling owner");tinygltf::TinyGLTF writer;
    ASSERT_TRUE(writer.WriteGltfSceneToFile(&source.gltfModel,(temp/"source.glb").string(),true,true,true,true));
    manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});manager->initialize((temp/"source.glb").string(),1,2);
    manager->populateSceneGraph(graph);const auto roots=graph.rootNodes();
    const auto editor=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Scene Root");graph.getNode(editor)->editorSceneRoot=true;
    for(const auto root:roots) ASSERT_TRUE(graph.setParent(root,editor));graph.updateWorldTransforms();
    for(int i=0;i<2;++i) {
      cameras.push_back(allocation->createBuffer(sizeof(CameraData),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT));
      objects.push_back(allocation->createBuffer(sizeof(ObjectData),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT));
    }
    bind(*manager);
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();manager.reset();
    if(allocation) {for(auto& b:cameras) allocation->destroyBuffer(b);for(auto& b:objects) allocation->destroyBuffer(b);}
    cameras.clear();objects.clear();pipelines.reset();allocation.reset();pool.clear();device.reset();surface.clear();messenger.clear();instance.reset();
    if(window) glfwDestroyWindow(window);if(glfwStarted) glfwTerminate();
    if(!temp.empty()) {std::error_code ignored;const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());const auto target=std::filesystem::weakly_canonical(temp);
      if(target.parent_path()==parent && target.filename().string().starts_with("container-scene-texture-sampling-")) std::filesystem::remove_all(target,ignored);}
    EXPECT_EQ(errors.load(),0u);
  }
  void bind(SceneManager& scene) {scene.updateDescriptorSets(cameras,objects[0]);}
  void sealStandalone(SceneManager::PreparedMaterialResources& resources) {
    EXPECT_FALSE(manager->canExchangeMaterialResources(resources));
    resources.sealLiveDependencies();
  }
  void installThree() {
    auto batch=allocation->beginTextureAllocationBatch();material::TextureManager table;
    const std::array colors{std::array<std::byte,4>{std::byte{255},std::byte{0},std::byte{0},std::byte{255}},
        std::array<std::byte,4>{std::byte{0},std::byte{255},std::byte{0},std::byte{255}},
        std::array<std::byte,4>{std::byte{0},std::byte{0},std::byte{255},std::byte{255}}};
    constexpr std::array lastPixels{std::byte{255},std::byte{255},std::byte{0},std::byte{255},std::byte{0},std::byte{0},std::byte{255},std::byte{255}};
    for(size_t i=0;i<colors.size();++i) {const std::span<const std::byte> pixels=i==2?std::span<const std::byte>(lastPixels):std::span<const std::byte>(colors[i]);
      auto resource=allocation->createTextureFromRgbaPixels("slot "+std::to_string(i),pixels,i==2?2:1,1,VK_FORMAT_R8G8B8A8_UNORM);
      resource.samplerIndex=i==2?material::NormalizeMaterialSamplerKey(9728,9728,33071,33071).value:0;
      EXPECT_EQ(table.registerTexture(resource),i);}
    Access::install(*manager,std::move(table),batch.commit());bind(*manager);
  }
  std::vector<glm::vec4> sample(SceneManager& scene,VkDescriptorSet sceneSet,uint32_t count,glm::vec2 uv={.5f,.5f}) {
    const auto bytes=sizeof(glm::vec4)*size_t(count)*2;
    auto output=allocation->createBuffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if(!output.allocation_info.pMappedData) throw std::runtime_error("Sampled descriptor output is not mapped");
    // Unwritten shader output must fail deterministically, including metadata.
    std::fill_n(static_cast<uint32_t*>(output.allocation_info.pMappedData),bytes/sizeof(uint32_t),0x7fc00000u);
    if(vmaFlushAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
      throw std::runtime_error("Cannot poison sampled descriptor output");
    std::vector<glm::vec4> result;
    {
      const vk::DescriptorSetLayoutBinding binding(0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute);
      vk::raii::DescriptorSetLayout outputLayout(device->raii(),vk::DescriptorSetLayoutCreateInfo({},binding));
      const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer,1);
      vk::raii::DescriptorPool descriptors(device->raii(),vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,1,size));
      const std::array outputLayouts{*outputLayout};auto sets=device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors,outputLayouts));
      const vk::DescriptorBufferInfo info(output.buffer,0,bytes);
      const vk::WriteDescriptorSet write(*sets[0],0,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&info);device->raii().updateDescriptorSets(write,{});
      const std::array layouts{vk::DescriptorSetLayout(scene.descriptorSetLayout()),*outputLayout};
      const vk::PushConstantRange push(vk::ShaderStageFlagBits::eCompute,0,16);
      vk::raii::PipelineLayout layout(device->raii(),vk::PipelineLayoutCreateInfo({},layouts,push));
      const auto code=container::util::readFile(CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH);
      vk::raii::ShaderModule shader(device->raii(),vk::ShaderModuleCreateInfo({},code.size(),reinterpret_cast<const uint32_t*>(code.data())));
      vk::raii::Pipeline pipeline(device->raii(),nullptr,vk::ComputePipelineCreateInfo({},vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,*shader,"computeMain"),*layout));
      struct SamplingQuery {uint32_t count,padding;float u,v;};
      static_assert(sizeof(SamplingQuery)==16);const SamplingQuery query{count,0,uv.x,uv.y};
      allocation->executeStagedCommands([&](VkCommandBuffer command) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,*pipeline);
        const std::array<VkDescriptorSet,2> rawSets{sceneSet,*sets[0]};vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,*layout,0,2,rawSets.data(),0,nullptr);
        vkCmdPushConstants(command,*layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(query),&query);vkCmdDispatch(command,(count+31)/32,1,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
      });
      if(vmaInvalidateAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot read sampled descriptor output");
      result.resize(count*2);std::memcpy(result.data(),output.allocation_info.pMappedData,bytes);
    }
    allocation->destroyBuffer(output);return result;
  }
  void expectThree(SceneManager& scene,VkDescriptorSet set,bool middleDead) {
    const auto values=sample(scene,set,3);ASSERT_EQ(values.size(),6u);
    EXPECT_EQ(values[0],glm::vec4(1,0,0,1));EXPECT_EQ(values[2],middleDead?glm::vec4(1):glm::vec4(0,1,0,1));EXPECT_EQ(values[4],glm::vec4(0,0,1,1));
    EXPECT_EQ(values[1],glm::vec4(0,2,0,0));EXPECT_EQ(values[3],glm::vec4(0,2,0,0));EXPECT_EQ(values[5],glm::vec4(1,2,0,0));
  }
  bool publish(SceneManager::PreparedMaterialGeneration& stage) {
    stage.prepareDescriptorFrames(cameras,objects);if(!manager->canExchangeMaterialGeneration(stage,nullptr)) return false;
    size_t attempts{};{material_publication_test::AllocationGuard guard(true);manager->exchangeMaterialGeneration(stage,nullptr);attempts=guard.allocations();}
    return !stage && attempts==0;
  }
  std::vector<std::byte> png() {
    auto source=triangle("PNG producer");auto& model=source.gltfModel;model.images.resize(1);auto& image=model.images[0];
    image.width=image.height=1;image.component=4;image.bits=8;image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;image.image={255,255,0,255};image.uri="new.png";
    model.textures.resize(1);model.textures[0].source=0;model.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
    tinygltf::TinyGLTF writer;if(!writer.WriteGltfSceneToFile(&model,(temp/"new.gltf").string(),false,true,true,false)) throw std::runtime_error("Cannot create PNG fixture");
    std::ifstream stream(temp/"new.png",std::ios::binary|std::ios::ate);const auto length=stream.tellg();if(!stream || length<=0 || length>1024*1024) throw std::runtime_error("Invalid PNG fixture");
    std::vector<std::byte> bytes(static_cast<size_t>(length));stream.seekg(0);if(!stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()))) throw std::runtime_error("Cannot read PNG fixture");return bytes;
  }
  std::atomic_uint errors{};bool glfwStarted{};GLFWwindow* window{};
  std::unique_ptr<VulkanInstance> instance;vk::raii::DebugUtilsMessengerEXT messenger{nullptr};vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device;vk::raii::CommandPool pool{nullptr};std::unique_ptr<AllocationManager> allocation;
  std::unique_ptr<PipelineManager> pipelines;std::unique_ptr<SceneManager> manager;SceneGraph graph;
  std::vector<AllocatedBuffer> cameras,objects;std::filesystem::path temp;
};

TEST_F(SceneTextureSamplingGpu,MiddleTombstoneKeepsExactSurvivorImageAndSamplerSlots) {
  installThree();expectThree(*manager,manager->descriptorSet(0),false);
  ASSERT_TRUE(Access::retire(*manager,1));bind(*manager);
  EXPECT_EQ(manager->materialTextureResourceCount(),3u);EXPECT_EQ(Access::table(*manager).activeTextureCount(),2u);
  expectThree(*manager,manager->descriptorSet(0),true);
  const auto clamped=sample(*manager,manager->descriptorSet(0),3,{1.25f,.5f});ASSERT_EQ(clamped.size(),6u);
  // The last image is [yellow, blue]. Its nearest/clamp sampler must select
  // blue at the center boundary and beyond the right edge. Linear or repeat
  // descriptors produce distinct values, so metadata agreement alone cannot pass.
  EXPECT_EQ(clamped[4],glm::vec4(0,0,1,1));
  auto bindings=manager->prepareSceneBindings();bindings.prepareDescriptorFrames(cameras,objects);
  ASSERT_TRUE(manager->canExchangeSceneBindings(bindings));expectThree(*manager,bindings.descriptorSet(0),true);
  size_t attempts{};{material_publication_test::AllocationGuard guard(true);manager->exchangeSceneBindings(bindings);attempts=guard.allocations();}
  EXPECT_EQ(attempts,0u);EXPECT_FALSE(bindings);expectThree(*manager,manager->descriptorSet(0),true);
}

TEST_F(SceneTextureSamplingGpu,ZeroAndAllDeadCatalogsHaveExplicitPrivateFallbackDescriptors) {
  ASSERT_EQ(manager->materialTextureResourceCount(),0u);const auto empty=sample(*manager,manager->descriptorSet(0),1);
  ASSERT_EQ(empty.size(),2u);EXPECT_EQ(empty[0],glm::vec4(1));EXPECT_EQ(empty[1],glm::vec4(0,1,0,0));
  installThree();for(uint32_t i=0;i<3;++i) ASSERT_TRUE(Access::retire(*manager,i));bind(*manager);
  const auto dead=sample(*manager,manager->descriptorSet(0),3);ASSERT_EQ(dead.size(),6u);
  for(size_t i=0;i<3;++i) {EXPECT_EQ(dead[i*2],glm::vec4(1));EXPECT_EQ(dead[i*2+1],glm::vec4(0,1,0,0));}
  EXPECT_EQ(manager->materialTextureResourceCount(),3u);EXPECT_EQ(Access::table(*manager).activeTextureCount(),0u);
}

TEST_F(SceneTextureSamplingGpu, PrimaryAndStagedSceneSetsSampleTheirImageClipState) {
  manager->updateAuxiliaryDescriptorSets(cameras, objects);
  container::gpu::SceneClipState clip{};
  clip.boxClipEnabled = 1;
  clip.boxClipPlaneCount = 1;
  clip.boxClipPlanes[0] = {1, 0, 0, -1};
  auto invertedClip = clip;
  invertedClip.boxClipInvert = 1;
  manager->updateSceneClipState(0, clip);
  manager->updateSceneClipState(1, invertedClip);

  const auto expectClip = [&](VkDescriptorSet set, float expected) {
    const auto values = sample(*manager, set, 1);
    ASSERT_EQ(values.size(), 2u);
    EXPECT_FLOAT_EQ(values[1].z, expected);
  };
  expectClip(manager->descriptorSet(0), 1.0f);
  expectClip(manager->descriptorSet(1), 0.0f);
  expectClip(manager->auxiliaryDescriptorSet(0), 1.0f);
  expectClip(manager->auxiliaryDescriptorSet(1), 0.0f);

  auto bindings = manager->prepareSceneBindings();
  bindings.prepareDescriptorFrames(cameras, objects);
  expectClip(bindings.descriptorSet(0), 1.0f);
  expectClip(bindings.auxiliaryDescriptorSet(1), 0.0f);

  GltfMaterialPatch patch;
  patch.values = {{GltfMaterialProperty::Roughness, .25}};
  auto material = manager->prepareMaterialGeneration(manager->planMaterialEdit(0, patch), nullptr);
  material.prepareDescriptorFrames(cameras, objects);
  expectClip(material.auxiliaryDescriptorSet(0), 1.0f);
  expectClip(material.descriptorSet(1), 0.0f);
}

TEST_F(SceneTextureSamplingGpu,ActiveInvalidCapabilityRejectsInsteadOfResolvingFallback) {
  installThree();const auto descriptor=manager->descriptorSet(0);auto bad=*Access::table(*manager).getTexture(0);bad.imageView=VK_NULL_HANDLE;
  material::TextureManager table;table.registerTexture(bad);
  EXPECT_THROW((void)material::MakeTextureSamplingPlan(table,Access::fallback(*manager),*allocation,3,3),std::invalid_argument);
  auto stale=Access::fallback(*manager);stale.image=VK_NULL_HANDLE;
  EXPECT_THROW((void)material::MakeTextureSamplingPlan(Access::table(*manager),stale,*allocation,3,3),std::invalid_argument);
  EXPECT_THROW((void)material::MakeTextureSamplingPlan(Access::table(*manager),Access::fallback(*manager),*allocation,2,3),std::length_error);
  EXPECT_THROW((void)material::MakeTextureSamplingPlan(Access::table(*manager),Access::fallback(*manager),*allocation,3,3,1),std::length_error);
  EXPECT_EQ(manager->descriptorSet(0),descriptor);expectThree(*manager,descriptor,false);
}

TEST_F(SceneTextureSamplingGpu,EqualResolvedFallbackViewsDoNotHideChangedActivePhase) {
  auto alias=Access::fallback(*manager);alias.name="Explicit active alias of private fallback";material::TextureManager table;table.registerTexture(alias);
  Access::install(*manager,std::move(table));bind(*manager);
  auto bindings=manager->prepareSceneBindings();bindings.prepareDescriptorFrames(cameras,objects);
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Roughness,.27}};
  auto material=manager->prepareMaterialGeneration(manager->planMaterialEdit(0,patch),nullptr);material.prepareDescriptorFrames(cameras,objects);
  const auto before=sample(*manager,manager->descriptorSet(0),1);ASSERT_TRUE(Access::retire(*manager,0,true,false));bind(*manager);
  EXPECT_EQ(sample(*manager,manager->descriptorSet(0),1),before);
  EXPECT_FALSE(manager->canExchangeSceneBindings(bindings));EXPECT_FALSE(manager->canExchangeMaterialGeneration(material,nullptr));
  const auto current=manager->descriptorSet(0);manager->exchangeSceneBindings(bindings);manager->exchangeMaterialGeneration(material,nullptr);
  EXPECT_EQ(manager->descriptorSet(0),current);EXPECT_DOUBLE_EQ(manager->authoredMaterial(0).pbrMetallicRoughness.roughnessFactor,.7);
}

TEST_F(SceneTextureSamplingGpu,ImmediateResourceAppendNeedsExplicitOrCoordinatedMetadataPublication) {
  const auto descriptor=manager->descriptorSet(0);const auto before=manager->authoredMaterial(0);
  const auto reference=manager->loadMaterialTextureSourceFromBytes("Immediate pending upload",png(),true);
  ASSERT_TRUE(reference);ASSERT_GT(manager->materialTextureResourceCount(),0u);
  EXPECT_THROW((void)manager->prepareSceneBindings(),std::invalid_argument);
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Roughness,.27}};
  const auto edit=manager->planMaterialEdit(0,patch);
  EXPECT_THROW((void)manager->prepareMaterialGeneration(edit,nullptr),std::invalid_argument);
  EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->authoredMaterial(0),before);
  // A retirement in the unuploaded prefix is not an append-only repair. This
  // intermediate fixture never publishes or samples the rejected table.
  auto activePrefix=Access::table(*manager);ASSERT_TRUE(Access::retire(*manager,reference.runtimeResource(),false,false));
  EXPECT_THROW((void)manager->prepareMaterialTextureBytes(0,"Rejected stale prefix",png(),MaterialTextureRoles::Color),std::invalid_argument);
  using std::swap;swap(Access::table(*manager),activePrefix);
  auto resources=manager->prepareMaterialTextureBytes(0,"Coordinated pending upload",png(),MaterialTextureRoles::Color);
  GltfMaterialPatch texture;GltfMaterialTextureEdit binding;binding.slot=GltfMaterialTextureSlot::BaseColor;binding.operation=GltfMaterialTextureOperation::Bind;
  binding.binding=GltfMaterialTextureSourcePair{int32_t(resources.sourceTexture()),int32_t(resources.evaluatedTexture())};texture.textures.push_back(binding);
  const auto coordinated=manager->planMaterialTextureEdit(0,texture,&resources);
  auto publication=manager->prepareMaterialGeneration(coordinated,nullptr,std::move(resources));ASSERT_TRUE(publish(publication));publication={};
  auto bindings=manager->prepareSceneBindings();bindings.prepareDescriptorFrames(cameras,objects);
  EXPECT_TRUE(manager->canExchangeSceneBindings(bindings));
  auto material=manager->prepareMaterialGeneration(manager->planMaterialEdit(0,patch),nullptr);EXPECT_TRUE(publish(material));
}

TEST_F(SceneTextureSamplingGpu,ResourcePrefixProofAcceptsOnlyExactSuccessfulActiveAppends) {
  installThree();const auto original=Access::table(*manager);
  const auto plan=material::MakeTextureSamplingPlan(original,Access::fallback(*manager),*allocation,16,16);
  auto alias=*original.getTexture(0);alias.name="New alias after published prefix";
  auto appended=original;ASSERT_EQ(appended.registerTexture(alias),3u);
  bool accepted{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);accepted=material::TextureSamplingPlanPrefixMatches(plan,appended,Access::fallback(*manager),*allocation);attempts=guard.allocations();}
  EXPECT_TRUE(accepted);EXPECT_EQ(attempts,0u);
  EXPECT_FALSE(material::TextureSamplingPlanMatches(plan,appended,Access::fallback(*manager),*allocation));
  auto retiredOld=appended;ASSERT_TRUE(retiredOld.retireTexture(0));
  EXPECT_FALSE(material::TextureSamplingPlanPrefixMatches(plan,retiredOld,Access::fallback(*manager),*allocation));
  auto retiredTail=appended;ASSERT_TRUE(retiredTail.retireTexture(3));
  EXPECT_FALSE(material::TextureSamplingPlanPrefixMatches(plan,retiredTail,Access::fallback(*manager),*allocation));
  auto extraRevision=appended;ASSERT_TRUE(extraRevision.retireTexture(3));alias.name="Another successful append after retirement";
  ASSERT_EQ(extraRevision.registerTexture(alias),4u);
  EXPECT_FALSE(material::TextureSamplingPlanPrefixMatches(plan,extraRevision,Access::fallback(*manager),*allocation));
  auto invalid=original;alias.name="Invalid active appended capability";alias.imageView=VK_NULL_HANDLE;ASSERT_EQ(invalid.registerTexture(alias),3u);
  EXPECT_FALSE(material::TextureSamplingPlanPrefixMatches(plan,invalid,Access::fallback(*manager),*allocation));
  auto corrupt=plan;corrupt.metadata[0].samplerCount=99;
  EXPECT_FALSE(material::TextureSamplingPlanPrefixMatches(corrupt,appended,Access::fallback(*manager),*allocation));
  auto staleRevision=plan;++staleRevision.textureRevision;
  EXPECT_FALSE(material::TextureSamplingPlanPrefixMatches(staleRevision,appended,Access::fallback(*manager),*allocation));
  expectThree(*manager,manager->descriptorSet(0),false);
}

TEST_F(SceneTextureSamplingGpu,PendingAppendResourceTokenRejectsInterveningGpuMetadataRefresh) {
  ASSERT_EQ(Access::samplingPlan(*manager).rows.size(),0u);ASSERT_EQ(manager->materialTextureMetadata().size(),1u);
  const auto reference=manager->loadMaterialTextureSourceFromBytes("Pending single resource",png(),true);ASSERT_TRUE(reference);
  ASSERT_EQ(manager->materialTextureResourceCount(),1u);ASSERT_EQ(Access::samplingPlan(*manager).rows.size(),0u);
  auto resources=manager->prepareMaterialTextureBytes(0,"Prepared after pending append",png(),MaterialTextureRoles::Color);
  sealStandalone(resources);ASSERT_TRUE(manager->canExchangeMaterialResources(resources));
  const auto buffer=Access::metadataBuffer(*manager).buffer;const auto memory=Access::metadataBuffer(*manager).allocation;
  const auto revision=manager->materialRevision(),resourceRevision=manager->materialResourceRevision(),epoch=manager->generationEpoch();
  const auto tableRevision=Access::table(*manager).revision();
  // Exercise only the metadata upload first: every older guard (including
  // metadata row count, buffer/allocation, CPU table and outer revisions) stays
  // identical. The newly captured published-plan phase must reject this token.
  Access::refresh(*manager);
  EXPECT_EQ(manager->materialRevision(),revision);EXPECT_EQ(manager->materialResourceRevision(),resourceRevision);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(Access::table(*manager).revision(),tableRevision);EXPECT_EQ(manager->materialTextureResourceCount(),1u);
  EXPECT_EQ(manager->materialTextureMetadata().size(),1u);EXPECT_EQ(Access::metadataBuffer(*manager).buffer,buffer);EXPECT_EQ(Access::metadataBuffer(*manager).allocation,memory);
  EXPECT_EQ(Access::samplingPlan(*manager).rows.size(),1u);EXPECT_FALSE(manager->canExchangeMaterialResources(resources));
  auto fresh=manager->prepareMaterialTextureBytes(0,"Fresh after metadata publication",png(),MaterialTextureRoles::Color);
  sealStandalone(fresh);ASSERT_TRUE(manager->canExchangeMaterialResources(fresh));manager->uploadMaterialResources();
  EXPECT_FALSE(manager->canExchangeMaterialResources(fresh));
}

TEST_F(SceneTextureSamplingGpu,MergeAndCloneRepairOnlyTheCapturedCommittedImagePrefix) {
  const auto reference=manager->loadMaterialTextureSourceFromBytes("Pending merge and clone image",png(),true);ASSERT_TRUE(reference);
  ASSERT_EQ(Access::samplingPlan(*manager).rows.size(),0u);ASSERT_EQ(manager->materialTextureResourceCount(),1u);
  const auto currentImages=Access::imageOwner(*manager);ASSERT_TRUE(material::TextureImageOwnershipMatches(currentImages,Access::table(*manager),*allocation));
  auto merge=manager->prepareMergeGeneration(triangle("Prefix merge"),temp/"prefix-merge.glb",{},graph);
  ASSERT_TRUE(merge.readyForExchange(*manager,graph));bind(merge.manager());
  const auto merged=sample(merge.manager(),merge.manager().descriptorSet(0),1);ASSERT_EQ(merged[0],glm::vec4(1,1,0,1));
  uint32_t owner=SceneGraph::kInvalidNode;for(uint32_t i=0;i<graph.nodeCount();++i) if(graph.getNode(i)->sourceNodeIndex==0) {owner=i;break;}
  ASSERT_NE(owner,SceneGraph::kInvalidNode);auto clone=manager->prepareSubtreeCloneGeneration(graph,owner,"Prefix clone");
  ASSERT_TRUE(clone.readyForExchange(*manager,graph));bind(clone.manager());
  const auto cloned=sample(clone.manager(),clone.manager().descriptorSet(0),1);ASSERT_EQ(cloned[0],glm::vec4(1,1,0,1));
  const auto revision=manager->materialRevision(),resourceRevision=manager->materialResourceRevision(),epoch=manager->generationEpoch();
  const auto buffer=Access::metadataBuffer(*manager).buffer;const auto memory=Access::metadataBuffer(*manager).allocation;
  Access::refresh(*manager);bind(*manager);
  EXPECT_EQ(manager->materialRevision(),revision);EXPECT_EQ(manager->materialResourceRevision(),resourceRevision);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(Access::imageOwner(*manager),currentImages);EXPECT_EQ(Access::metadataBuffer(*manager).buffer,buffer);EXPECT_EQ(Access::metadataBuffer(*manager).allocation,memory);
  EXPECT_EQ(Access::samplingPlan(*manager).rows.size(),1u);EXPECT_FALSE(merge.readyForExchange(*manager,graph));EXPECT_FALSE(clone.readyForExchange(*manager,graph));
  auto fresh=manager->prepareMergeGeneration(triangle("Fresh prefix merge"),temp/"fresh-prefix.glb",{},graph);
  EXPECT_TRUE(fresh.readyForExchange(*manager,graph));
}

TEST_F(SceneTextureSamplingGpu,FactorAndOwnedTextureTailHistoryKeepRetiredPrefixAndSurvivorSamples) {
  installThree();ASSERT_TRUE(Access::retire(*manager,1));bind(*manager);
  GltfMaterialPatch factor;factor.values={{GltfMaterialProperty::Roughness,.27}};
  auto changed=manager->prepareMaterialGeneration(manager->planMaterialEdit(0,factor),nullptr);ASSERT_TRUE(publish(changed));changed={};expectThree(*manager,manager->descriptorSet(0),true);
  auto resources=manager->prepareMaterialTextureBytes(0,"New texture after middle tombstone",png(),MaterialTextureRoles::Color);
  const auto command=resources.command();EXPECT_EQ(resources.reference(true).runtimeResource(),3u);
  GltfMaterialPatch patch;GltfMaterialTextureEdit binding;binding.slot=GltfMaterialTextureSlot::BaseColor;binding.operation=GltfMaterialTextureOperation::Bind;
  binding.binding=GltfMaterialTextureSourcePair{int32_t(resources.sourceTexture()),int32_t(resources.evaluatedTexture())};patch.textures.push_back(binding);
  const auto edit=manager->planMaterialTextureEdit(0,patch,&resources);
  auto appended=manager->prepareMaterialGeneration(edit,nullptr,std::move(resources));ASSERT_TRUE(publish(appended));appended={};
  EXPECT_EQ(manager->materialTextureResourceCount(),4u);expectThree(*manager,manager->descriptorSet(0),true);
  auto remove=manager->prepareMaterialResourcesChange(command,false);auto before=manager->rebaseMaterialTextureEdit(edit,GltfMaterialEditSide::Before,&remove);
  auto undo=manager->prepareMaterialGeneration(before,nullptr,std::move(remove));ASSERT_TRUE(publish(undo));undo={};
  EXPECT_EQ(manager->materialTextureResourceCount(),3u);EXPECT_FALSE(Access::table(*manager).isTextureActive(1));expectThree(*manager,manager->descriptorSet(0),true);
  auto restore=manager->prepareMaterialResourcesChange(command,true);auto after=manager->rebaseMaterialTextureEdit(edit,GltfMaterialEditSide::After,&restore);
  auto redo=manager->prepareMaterialGeneration(after,nullptr,std::move(restore));ASSERT_TRUE(publish(redo));redo={};
  EXPECT_EQ(manager->materialTextureResourceCount(),4u);EXPECT_FALSE(Access::table(*manager).isTextureActive(1));expectThree(*manager,manager->descriptorSet(0),true);
  EXPECT_DOUBLE_EQ(manager->authoredMaterial(0).pbrMetallicRoughness.roughnessFactor,.27);
}

TEST_F(SceneTextureSamplingGpu,TexturedMergeExchangeUndoRedoAndCloneRetainFallbackAndStablePrefix) {
  installThree();ASSERT_TRUE(Access::retire(*manager,1));bind(*manager);const auto identity=Access::fallback(*manager).allocationIdentity;
  material::TextureResource incomingResource;
  {
    auto incoming=triangle("Incoming textured");auto& source=incoming.gltfModel;source.images.resize(1);
    auto& image=source.images[0];image.width=image.height=1;image.component=4;image.bits=8;image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image={255,255,0,255};image.uri="merge-yellow.png";
    source.textures.resize(1);source.textures[0].source=0;source.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
    auto merge=manager->prepareMergeGeneration(incoming,temp/"incoming.glb",{},graph);ASSERT_TRUE(merge.readyForExchange(*manager,graph));
    const auto expectMerged=[&](SceneManager& scene) {
      EXPECT_EQ(Access::fallback(scene).allocationIdentity,identity);EXPECT_EQ(scene.materialTextureResourceCount(),4u);
      EXPECT_FALSE(Access::table(scene).isTextureActive(1));expectThree(scene,scene.descriptorSet(0),true);
      const auto values=sample(scene,scene.descriptorSet(0),4);ASSERT_EQ(values.size(),8u);
      EXPECT_EQ(values[6],glm::vec4(1,1,0,1));EXPECT_EQ(values[7],glm::vec4(0,2,0,0));
    };
    auto& candidate=merge.manager();incomingResource=*Access::table(candidate).getTexture(3);
    EXPECT_TRUE(material::TextureImageOwnershipContains(Access::imageOwner(candidate),incomingResource,*allocation));
    bind(candidate);expectMerged(candidate);
    bool firstExchange=true;
    const auto exchange=[&] {
      if(!firstExchange) merge.prepareResourceDependencies(*manager);
      firstExchange=false;
      size_t attempts{};{material_publication_test::AllocationGuard guard(true);manager->exchangeMergeGeneration(merge);attempts=guard.allocations();}
      EXPECT_EQ(attempts,0u);bind(*manager);
    };
    // Asset-generation exchange is exercised here. Graph publication/history is
    // covered by the separate scene merge suites and is not inferred from this.
    exchange();expectMerged(*manager);
    exchange();EXPECT_EQ(manager->materialTextureResourceCount(),3u);expectThree(*manager,manager->descriptorSet(0),true);
    exchange();expectMerged(*manager);
    exchange();EXPECT_EQ(manager->materialTextureResourceCount(),3u);expectThree(*manager,manager->descriptorSet(0),true);
  }
  EXPECT_FALSE(allocation->textureImageMatches(incomingResource));
  uint32_t owner=SceneGraph::kInvalidNode;for(uint32_t i=0;i<graph.nodeCount();++i) if(graph.getNode(i)->sourceNodeIndex==0) {owner=i;break;}
  ASSERT_NE(owner,SceneGraph::kInvalidNode);
  {
    auto clone=manager->prepareSubtreeCloneGeneration(graph,owner,"Sampling clone");ASSERT_TRUE(clone.readyForExchange(*manager,graph));
    auto& candidate=clone.manager();EXPECT_EQ(Access::fallback(candidate).allocationIdentity,identity);bind(candidate);
    EXPECT_EQ(Access::imageOwner(candidate),Access::imageOwner(*manager));
    EXPECT_EQ(candidate.materialTextureResourceCount(),3u);EXPECT_FALSE(Access::table(candidate).isTextureActive(1));expectThree(candidate,candidate.descriptorSet(0),true);
  }
  expectThree(*manager,manager->descriptorSet(0),true);
}

TEST_F(SceneTextureSamplingGpu,SceneBindingPreparationDenialLeavesTheLivePublicationUnchanged) {
  installThree();ASSERT_TRUE(Access::retire(*manager,1));bind(*manager);const auto descriptor=manager->descriptorSet(0);const auto epoch=manager->generationEpoch();
  // Prepared registration/pin registries may reserve invisible capacity once.
  // Warm that allowed capacity before counting reproducible failure points.
  {auto warm=manager->prepareSceneBindings();}
  size_t calls{};{material_publication_test::AllocationGuard count(false);auto token=manager->prepareSceneBindings();calls=count.allocations();}
  ASSERT_GT(calls,0u);ASSERT_LT(calls,100u);
  for(size_t allowed=0;allowed<calls;++allowed) {
    bool denied{};{material_publication_test::AllocationGuard guard(false,allowed);try {(void)manager->prepareSceneBindings();} catch(const std::bad_alloc&) {denied=true;}}
    EXPECT_TRUE(denied);EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_FALSE(Access::table(*manager).isTextureActive(1));
  }
  expectThree(*manager,descriptor,true);
}

TEST_F(SceneTextureSamplingGpu,PreparedBindingsRetainFallbackOwnerUntilTheLastActualHolderReleases) {
  const auto resource=Access::fallback(*manager);std::weak_ptr<const void> weak=Access::fallbackOwner(*manager);
  auto token=manager->prepareSceneBindings();token.prepareDescriptorFrames(cameras,objects);Access::dropFallbackOwner(*manager);
  EXPECT_FALSE(weak.expired());EXPECT_TRUE(allocation->textureImageMatches(resource));
  EXPECT_FALSE(manager->canExchangeSceneBindings(token));
  // The live manager cannot publish now, but this actual prepared GPU reader
  // still owns its fallback lease independently of that missing live holder.
  const auto values=sample(*manager,token.descriptorSet(0),1);ASSERT_EQ(values.size(),2u);EXPECT_EQ(values[0],glm::vec4(1));EXPECT_EQ(values[1],glm::vec4(0,1,0,0));
  token={};EXPECT_TRUE(weak.expired());EXPECT_FALSE(allocation->textureImageMatches(resource));
}

TEST_F(SceneTextureSamplingGpu,GroupZeroAdoptionIsDeferredAndSelectedAliasesOwnOnlyTheirImage) {
  constexpr std::array pixels{std::byte{255},std::byte{0},std::byte{0},std::byte{255}};
  const auto first=allocation->createTextureFromRgbaPixels("Adopted red",pixels,1,1,VK_FORMAT_R8G8B8A8_UNORM);
  const auto other=allocation->createTextureFromRgbaPixels("Adopted other",pixels,1,1,VK_FORMAT_R8G8B8A8_UNORM);
  material::TextureManager table;table.registerTexture(first);table.registerTexture(other);
  auto alias=first;alias.name="Red sampler alias";alias.samplerIndex=material::NormalizeMaterialSamplerKey(9728,9728).value;
  ASSERT_EQ(table.registerTexture(alias),2u);
  {
    auto abandoned=material::PrepareTextureImageOwnership(table,*allocation);
    auto adoption=material::PrepareTextureImageOwnershipAdoption(abandoned,table,*allocation);
    ASSERT_EQ(adoption.pendingCount(),2u);
    EXPECT_TRUE(material::CanAdoptTextureImageOwnership(adoption,table,*allocation));
  }
  // A cancelled preparation has not transferred the existing reset owner.
  ASSERT_TRUE(allocation->textureImageMatches(first));ASSERT_TRUE(allocation->textureImageMatches(other));
  auto full=material::PrepareTextureImageOwnership(table,*allocation);ASSERT_EQ(full->imageCount(),2u);
  ASSERT_EQ(full->rows().size(),3u);EXPECT_EQ(full->rows()[0].image,full->rows()[2].image);
  const std::array selectedSlots{2u};auto selected=material::SelectTextureImageOwnership(full,selectedSlots,*allocation);
  ASSERT_EQ(selected->imageCount(),1u);EXPECT_FALSE(selected->complete());
  auto adoption=material::PrepareTextureImageOwnershipAdoption(full,table,*allocation);
  bool accepted{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);accepted=material::AdoptTextureImageOwnership(adoption,table,*allocation);attempts=deny.allocations();}
  ASSERT_TRUE(accepted);EXPECT_EQ(attempts,0u);
  EXPECT_FALSE(material::AdoptTextureImageOwnership(adoption,table,*allocation));
  adoption={};full.reset();
  EXPECT_TRUE(allocation->textureImageMatches(first));EXPECT_FALSE(allocation->textureImageMatches(other));
  EXPECT_TRUE(material::TextureImageOwnershipContains(selected,alias,*allocation));
  selected.reset();EXPECT_FALSE(allocation->textureImageMatches(first));
}

TEST_F(SceneTextureSamplingGpu,CommittedBatchDropsUnselectedMembersWithoutHoldingTheFullParentSet) {
  constexpr std::array pixels{std::byte{0},std::byte{255},std::byte{0},std::byte{255}};
  auto batch=allocation->beginTextureAllocationBatch();material::TextureManager table;
  const auto first=allocation->createTextureFromRgbaPixels("Selected green",pixels,1,1,VK_FORMAT_R8G8B8A8_UNORM);
  const auto other=allocation->createTextureFromRgbaPixels("Unselected green",pixels,1,1,VK_FORMAT_R8G8B8A8_UNORM);
  table.registerTexture(first);table.registerTexture(other);auto aggregate=batch.commit();
  auto full=material::PrepareTextureImageOwnership(table,*allocation);const std::array slots{0u};
  auto selected=material::SelectTextureImageOwnership(full,slots,*allocation);
  std::weak_ptr<const material::TextureImageOwnership> oldGeneration=full;
  aggregate.reset();ASSERT_TRUE(allocation->textureImageMatches(other));full.reset();
  EXPECT_TRUE(oldGeneration.expired());EXPECT_TRUE(allocation->textureImageMatches(first));
  EXPECT_FALSE(allocation->textureImageMatches(other));EXPECT_FALSE(material::TextureImageOwnershipMatches(selected,table,*allocation));
  selected.reset();EXPECT_FALSE(allocation->textureImageMatches(first));
}

TEST_F(SceneTextureSamplingGpu,ImageOwnershipChecksExactCoverageOwnerRevisionAndAllocationFailures) {
  installThree();const auto& table=Access::table(*manager);const auto full=Access::imageOwner(*manager);
  ASSERT_TRUE(material::TextureImageOwnershipMatches(full,table,*allocation));
  const std::array inherited{full};auto rebuilt=material::PrepareTextureImageOwnership(table,*allocation,inherited);
  ASSERT_EQ(rebuilt->pins().size(),full->pins().size());
  for(size_t i=0;i<full->pins().size();++i) EXPECT_EQ(rebuilt->pins()[i],full->pins()[i]);
  auto changed=table;ASSERT_TRUE(changed.retireTexture(1));
  EXPECT_FALSE(material::TextureImageOwnershipMatches(full,changed,*allocation));
  AllocationManager foreign;EXPECT_FALSE(material::TextureImageOwnershipValid(full,foreign));
  EXPECT_THROW((void)material::PrepareTextureImageOwnership(table,foreign),std::invalid_argument);
  EXPECT_THROW((void)material::PrepareTextureImageOwnership(table,*allocation,{},1),std::length_error);
  const std::array badSlot{99u};EXPECT_THROW((void)material::SelectTextureImageOwnership(full,badSlot,*allocation),std::invalid_argument);
  size_t calls{};{material_publication_test::AllocationGuard count(false);auto owner=material::PrepareTextureImageOwnership(table,*allocation);calls=count.allocations();}
  ASSERT_GT(calls,0u);ASSERT_LT(calls,100u);
  for(size_t allowed=0;allowed<calls;++allowed) {
    bool failed{};{material_publication_test::AllocationGuard deny(false,allowed);try {(void)material::PrepareTextureImageOwnership(table,*allocation);} catch(const std::bad_alloc&) {failed=true;}}
    EXPECT_TRUE(failed);EXPECT_TRUE(material::TextureImageOwnershipMatches(full,table,*allocation));
  }
  bool valid{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);valid=material::TextureImageOwnershipMatches(full,table,*allocation);attempts=deny.allocations();}
  EXPECT_TRUE(valid);EXPECT_EQ(attempts,0u);expectThree(*manager,manager->descriptorSet(0),false);
}

TEST_F(SceneTextureSamplingGpu,PreparedSceneDescriptorsRetainActualMainImagesAfterTheLiveOwnerDetaches) {
  installThree();const auto first=*Access::table(*manager).getTexture(0),other=*Access::table(*manager).getTexture(1);
  auto token=manager->prepareSceneBindings();token.prepareDescriptorFrames(cameras,objects);
  Access::dropImageOwner(*manager);EXPECT_FALSE(manager->canExchangeSceneBindings(token));
  ASSERT_TRUE(allocation->textureImageMatches(first));ASSERT_TRUE(allocation->textureImageMatches(other));
  expectThree(*manager,token.descriptorSet(0),false);
  token={};EXPECT_FALSE(allocation->textureImageMatches(first));EXPECT_FALSE(allocation->textureImageMatches(other));
  EXPECT_TRUE(allocation->textureImageMatches(Access::fallback(*manager)));
}

TEST_F(SceneTextureSamplingGpu,PreparedMaterialDescriptorsRetainActualMainImagesAndRejectMissingLiveOwnership) {
  installThree();const auto first=*Access::table(*manager).getTexture(0);
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Roughness,.19}};
  auto stage=manager->prepareMaterialGeneration(manager->planMaterialEdit(0,patch),nullptr);stage.prepareDescriptorFrames(cameras,objects);
  Access::dropImageOwner(*manager);EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));
  ASSERT_TRUE(allocation->textureImageMatches(first));expectThree(*manager,stage.descriptorSet(0),false);
  stage={};EXPECT_FALSE(allocation->textureImageMatches(first));
}

TEST_F(SceneTextureSamplingGpu,UntexturedMaterialAppendHistoryDoesNotPinUnrelatedCatalogImages) {
  installThree();const auto first=*Access::table(*manager).getTexture(0);
  auto stage=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto command=stage.appendCommand();
  ASSERT_TRUE(command);ASSERT_TRUE(command.textureImageOwnership());EXPECT_EQ(command.textureImageOwnership()->imageCount(),0u);
  ASSERT_TRUE(publish(stage));stage={};Access::dropImageOwner(*manager);
  EXPECT_FALSE(allocation->textureImageMatches(first));EXPECT_TRUE(command);
}

TEST_F(SceneTextureSamplingGpu,InitialGltfPublicationAdoptsItsImagesBeforeExposingDescriptors) {
  auto source=triangle("Initial textured owner");auto& model=source.gltfModel;
  model.images.resize(1);auto& image=model.images[0];image.width=image.height=1;image.component=4;
  image.bits=8;image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;image.image={255,255,0,255};image.uri="initial-yellow.png";
  model.textures.resize(1);model.textures[0].source=0;model.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
  tinygltf::TinyGLTF writer;const auto path=temp/"initial-textured.glb";
  ASSERT_TRUE(writer.WriteGltfSceneToFile(&model,path.string(),true,true,true,true));
  manager.reset();manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});
  manager->initialize(path.string(),1,2);bind(*manager);
  ASSERT_TRUE(material::TextureImageOwnershipMatches(Access::imageOwner(*manager),Access::table(*manager),*allocation));
  const auto logical=manager->resourceLifecycleGeneration();ASSERT_TRUE(logical);ASSERT_TRUE(logical->valid());
  const auto archive=logical->resource({container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,0});
  const auto evaluated=logical->resource({container::util::ResourceDomainKind::Evaluated,container::util::ResourceKind::Texture,0});
  const auto runtime=logical->resource({container::util::ResourceDomainKind::Runtime,container::util::ResourceKind::RuntimeTexture,0});
  ASSERT_TRUE(archive);ASSERT_TRUE(evaluated);ASSERT_TRUE(runtime);EXPECT_NE(archive,evaluated);EXPECT_NE(archive.domain(),evaluated.domain());
  EXPECT_FALSE(logical->dependencyAuthorityComplete());
  ASSERT_GT(Access::table(*manager).textureCount(),0u);const auto resource=*Access::table(*manager).getTexture(0);
  EXPECT_FALSE(allocation->canAdoptTextureImages(Access::imageOwner(*manager)->pins()));
  const auto values=sample(*manager,manager->descriptorSet(0),1);ASSERT_EQ(values[0],glm::vec4(1,1,0,1));
  Access::dropImageOwner(*manager);EXPECT_FALSE(allocation->textureImageMatches(resource));
}

TEST_F(SceneTextureSamplingGpu,TexturedAppendReceiptPinsOnlyItsSelectedMaterialImage) {
  installThree();auto resources=manager->prepareMaterialTextureBytes(0,"Receipt yellow",png(),MaterialTextureRoles::Color);
  const auto selected=resources.reference(true).runtimeResource();
  GltfMaterialPatch patch;GltfMaterialTextureEdit binding;binding.slot=GltfMaterialTextureSlot::BaseColor;binding.operation=GltfMaterialTextureOperation::Bind;
  binding.binding=GltfMaterialTextureSourcePair{int32_t(resources.sourceTexture()),int32_t(resources.evaluatedTexture())};patch.textures.push_back(binding);
  auto edit=manager->planMaterialTextureEdit(0,patch,&resources);
  auto stage=manager->prepareMaterialGeneration(edit,nullptr,std::move(resources));ASSERT_TRUE(publish(stage));stage={};
  const auto image=*Access::table(*manager).getTexture(selected),unrelated=*Access::table(*manager).getTexture(0);
  auto duplicate=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto command=duplicate.appendCommand();
  ASSERT_TRUE(command.textureImageOwnership());EXPECT_EQ(command.textureImageOwnership()->imageCount(),1u);
  ASSERT_TRUE(publish(duplicate));duplicate={};Access::dropImageOwner(*manager);
  EXPECT_FALSE(allocation->textureImageMatches(unrelated));EXPECT_TRUE(allocation->textureImageMatches(image));
  command={};EXPECT_FALSE(allocation->textureImageMatches(image));
}

TEST_F(SceneTextureSamplingGpu,FactorClearAndRetirementHistoryRetainSelectedEndpointsWithoutTheCatalog) {
  installThree();auto resources=manager->prepareMaterialTextureBytes(0,"History endpoints yellow",png(),MaterialTextureRoles::Color);
  const auto selected=resources.reference(true).runtimeResource();
  GltfMaterialPatch patch;GltfMaterialTextureEdit binding;binding.slot=GltfMaterialTextureSlot::BaseColor;binding.operation=GltfMaterialTextureOperation::Bind;
  binding.binding=GltfMaterialTextureSourcePair{int32_t(resources.sourceTexture()),int32_t(resources.evaluatedTexture())};patch.textures.push_back(binding);
  auto edit=manager->planMaterialTextureEdit(0,patch,&resources);
  auto stage=manager->prepareMaterialGeneration(edit,nullptr,std::move(resources));ASSERT_TRUE(publish(stage));stage={};
  const auto image=*Access::table(*manager).getTexture(selected),unrelated=*Access::table(*manager).getTexture(0);
  GltfMaterialPatch factor;factor.values={{GltfMaterialProperty::Roughness,.23}};
  auto factorPins=manager->retainMaterialPlanImages(manager->planMaterialEdit(0,factor));ASSERT_EQ(factorPins->imageCount(),1u);
  GltfMaterialPatch clear;GltfMaterialTextureEdit remove;remove.slot=GltfMaterialTextureSlot::BaseColor;remove.operation=GltfMaterialTextureOperation::Clear;clear.textures.push_back(remove);
  auto clearPins=manager->retainMaterialPlanImages(manager->planMaterialTextureEdit(0,clear));ASSERT_EQ(clearPins->imageCount(),1u);
  const std::array materialIds{manager->sourceMaterialRuntimeMapping()[0]};
  auto bindingPins=manager->retainMaterialImages(materialIds);ASSERT_EQ(bindingPins->imageCount(),1u);
  auto duplicate=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);const auto runtime=duplicate.runtimeMaterial();
  ASSERT_TRUE(publish(duplicate));duplicate={};
  auto retirement=manager->prepareMaterialRetirement(runtime,graph,nullptr);auto command=retirement.retirementCommand();
  ASSERT_TRUE(command.textureImageOwnership());EXPECT_EQ(command.textureImageOwnership()->imageCount(),1u);
  ASSERT_TRUE(publish(retirement));retirement={};Access::dropImageOwner(*manager);
  EXPECT_FALSE(allocation->textureImageMatches(unrelated));EXPECT_TRUE(allocation->textureImageMatches(image));
  command={};factorPins.reset();clearPins.reset();EXPECT_TRUE(allocation->textureImageMatches(image));
  bindingPins.reset();EXPECT_FALSE(allocation->textureImageMatches(image));
}

TEST_F(SceneTextureSamplingGpu,SamplerAliasUndoKeepsTheSharedPrefixImageIndependentlyOwned) {
  installThree();auto resources=manager->prepareMaterialTextureBytes(0,"Selected yellow prefix",png(),MaterialTextureRoles::Color);
  const auto source=resources.sourceTexture(),prefix=resources.reference(true).runtimeResource();
  sealStandalone(resources);ASSERT_TRUE(manager->canExchangeMaterialResources(resources));manager->exchangeMaterialResources(resources);resources={};
  bind(*manager);const auto image=*Access::table(*manager).getTexture(prefix);
  auto alias=manager->prepareMaterialTextureSource(0,source,MaterialTextureRoles::Color,material::NormalizeMaterialSamplerKey(9728,9728).value);
  auto command=alias.command();const auto tail=alias.reference(true).runtimeResource();ASSERT_GT(tail,prefix);
  ASSERT_EQ(alias.textures().getTexture(tail)->allocationIdentity,image.allocationIdentity);
  sealStandalone(alias);ASSERT_TRUE(manager->canExchangeMaterialResources(alias));manager->exchangeMaterialResources(alias);alias={};bind(*manager);
  ASSERT_TRUE(command.textureImageOwnership());EXPECT_EQ(command.textureImageOwnership()->imageCount(),1u);
  auto undo=manager->prepareMaterialResourcesChange(command,false);sealStandalone(undo);ASSERT_TRUE(manager->canExchangeMaterialResources(undo));
  size_t attempts{};{material_publication_test::AllocationGuard deny(true);manager->exchangeMaterialResources(undo);attempts=deny.allocations();}
  EXPECT_EQ(attempts,0u);undo={};bind(*manager);
  ASSERT_EQ(manager->materialTextureResourceCount(),tail);ASSERT_TRUE(allocation->textureImageMatches(image));
  EXPECT_TRUE(material::TextureImageOwnershipContains(Access::imageOwner(*manager),image,*allocation));
  const auto values=sample(*manager,manager->descriptorSet(0),prefix+1);ASSERT_EQ(values[prefix*2],glm::vec4(1,1,0,1));
  auto redo=manager->prepareMaterialResourcesChange(command,true);sealStandalone(redo);ASSERT_TRUE(manager->canExchangeMaterialResources(redo));
  manager->exchangeMaterialResources(redo);redo={};bind(*manager);
  EXPECT_EQ(Access::table(*manager).getTexture(tail)->allocationIdentity,image.allocationIdentity);
  const auto unrelated=*Access::table(*manager).getTexture(0);Access::dropImageOwner(*manager);
  EXPECT_FALSE(allocation->textureImageMatches(unrelated));EXPECT_TRUE(allocation->textureImageMatches(image));
  command={};EXPECT_FALSE(allocation->textureImageMatches(image));
}

TEST_F(SceneTextureSamplingGpu,AbandonedResourceRegistrationCancelsEscapedLogicalHistoryWithoutPublishingRows) {
  const auto generation=manager->resourceLifecycleGeneration();ASSERT_TRUE(generation);
  const auto before=generation->inventory();SceneManager::MaterialResourceCommand command;
  TextureResourceReferenceOwnerPtr selected;
  {
    auto resources=manager->prepareMaterialTextureBytes(0,"Abandoned logical source",png(),MaterialTextureRoles::Color);
    command=resources.command();selected=command.resourceReferences();ASSERT_TRUE(selected);EXPECT_TRUE(selected->valid());EXPECT_FALSE(selected->active());
    EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);EXPECT_EQ(generation->inventory().rows().size(),before.rows().size());
    EXPECT_EQ(generation->inventory().revision(),before.revision());
  }
  EXPECT_FALSE(selected->valid());EXPECT_FALSE(command.published());
  EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);EXPECT_EQ(generation->inventory().rows().size(),before.rows().size());
  EXPECT_EQ(generation->inventory().revision(),before.revision());
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,true),std::invalid_argument);
}

TEST_F(SceneTextureSamplingGpu,ResourceHistoryRestoresOriginalLogicalIncarnationsAndRejectsNumericBranchReuse) {
  using D=container::util::ResourceDomainKind;using K=container::util::ResourceKind;
  auto resources=manager->prepareMaterialTextureBytes(0,"Original logical texture",png(),MaterialTextureRoles::Color);
  auto command=resources.command();const auto slot=resources.sourceTexture();
  const auto original=resources.resourceLifecycleGeneration()->resource({D::Archive,K::Texture,slot});ASSERT_TRUE(original);
  sealStandalone(resources);ASSERT_TRUE(manager->canExchangeMaterialResources(resources));manager->exchangeMaterialResources(resources);resources={};bind(*manager);
  auto retained=command.resourceReferences();ASSERT_TRUE(retained);EXPECT_TRUE(retained->resourceDependenciesMatch(manager->resourceLifecycleGeneration()));
  auto undo=manager->prepareMaterialResourcesChange(command,false);sealStandalone(undo);ASSERT_TRUE(manager->canExchangeMaterialResources(undo));
  size_t attempts{};{material_publication_test::AllocationGuard deny(true);manager->exchangeMaterialResources(undo);attempts=deny.allocations();}
  EXPECT_EQ(attempts,0u);undo={};bind(*manager);
  EXPECT_FALSE(manager->resourceLifecycleGeneration()->resource({D::Archive,K::Texture,slot}));EXPECT_TRUE(retained->valid());
  EXPECT_FALSE(retained->resourceDependenciesActive());EXPECT_FALSE(retained->resourceDependenciesMatch(manager->resourceLifecycleGeneration()));
  auto redo=manager->prepareMaterialResourcesChange(command,true);sealStandalone(redo);ASSERT_TRUE(manager->canExchangeMaterialResources(redo));
  {material_publication_test::AllocationGuard deny(true);manager->exchangeMaterialResources(redo);attempts=deny.allocations();}
  EXPECT_EQ(attempts,0u);redo={};bind(*manager);
  EXPECT_EQ(manager->resourceLifecycleGeneration()->resource({D::Archive,K::Texture,slot}),original);
  EXPECT_TRUE(retained->resourceDependenciesActive());EXPECT_TRUE(retained->resourceDependenciesMatch(manager->resourceLifecycleGeneration()));
  const auto values=sample(*manager,manager->descriptorSet(0),1);ASSERT_EQ(values[0],glm::vec4(1,1,0,1));
  auto detachAgain=manager->prepareMaterialResourcesChange(command,false);sealStandalone(detachAgain);ASSERT_TRUE(manager->canExchangeMaterialResources(detachAgain));
  manager->exchangeMaterialResources(detachAgain);detachAgain={};bind(*manager);
  auto branch=manager->prepareMaterialTextureBytes(0,"Different branch at same table index",png(),MaterialTextureRoles::Color);
  auto branchCommand=branch.command();const auto replacement=branch.resourceLifecycleGeneration()->resource({D::Archive,K::Texture,slot});
  ASSERT_TRUE(replacement);EXPECT_NE(replacement,original);EXPECT_EQ(replacement.domain(),original.domain());EXPECT_GT(replacement.slot(),original.slot());
  sealStandalone(branch);ASSERT_TRUE(manager->canExchangeMaterialResources(branch));manager->exchangeMaterialResources(branch);branch={};bind(*manager);
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,true),std::invalid_argument);
  auto removeBranch=manager->prepareMaterialResourcesChange(branchCommand,false);sealStandalone(removeBranch);ASSERT_TRUE(manager->canExchangeMaterialResources(removeBranch));
  manager->exchangeMaterialResources(removeBranch);removeBranch={};bind(*manager);
  // Creating the replacement cut the old redo branch. Removing that new tail
  // does not manufacture authority to revive an abandoned journal branch.
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,true),std::invalid_argument);
  EXPECT_TRUE(retained->valid());EXPECT_FALSE(retained->resourceDependenciesMatch(manager->resourceLifecycleGeneration()));
}

TEST_F(SceneTextureSamplingGpu,SelectedLogicalHistoryDoesNotRebindAcrossFreshMergeDomains) {
  auto resources=manager->prepareMaterialTextureBytes(0,"Selected merge domain",png(),MaterialTextureRoles::Color);
  sealStandalone(resources);ASSERT_TRUE(manager->canExchangeMaterialResources(resources));manager->exchangeMaterialResources(resources);resources={};bind(*manager);
  const auto generation=manager->resourceLifecycleGeneration();
  const std::array selected{TextureResourceSelection{container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,0}};
  auto owner=RetainTextureResourceReferences(generation,selected,container::util::ResourcePinClass::History);
  ASSERT_TRUE(owner->resourceDependenciesMatch(generation));
  auto merge=manager->prepareMergeGeneration(triangle("Fresh independent logical domain"),temp/"fresh-domain.glb",{},graph);
  ASSERT_TRUE(merge.readyForExchange(*manager,graph));const auto merged=merge.manager().resourceLifecycleGeneration();ASSERT_TRUE(merged);
  EXPECT_NE(merged,generation);EXPECT_TRUE(owner->valid());EXPECT_FALSE(owner->resourceDependenciesMatch(merged));
  manager->exchangeMergeGeneration(merge);bind(*manager);EXPECT_FALSE(owner->resourceDependenciesMatch(manager->resourceLifecycleGeneration()));
  merge.prepareResourceDependencies(*manager);
  manager->exchangeMergeGeneration(merge);bind(*manager);EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);
  EXPECT_TRUE(owner->resourceDependenciesMatch(manager->resourceLifecycleGeneration()));
}

TEST_F(SceneTextureSamplingGpu,ResourceRedoRejectsAReusedSourceMaterialIndexWithAnotherIncarnation) {
  auto material=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);
  const auto originalMaterial=material.appendCommand();ASSERT_EQ(originalMaterial.sourceMaterial(),1);ASSERT_TRUE(publish(material));material={};
  auto resources=manager->prepareMaterialTextureBytes(1,"Retained material source",png(),MaterialTextureRoles::Color);
  const auto command=resources.command();sealStandalone(resources);ASSERT_TRUE(manager->canExchangeMaterialResources(resources));
  manager->exchangeMaterialResources(resources);resources={};bind(*manager);
  auto removeResources=manager->prepareMaterialResourcesChange(command,false);sealStandalone(removeResources);ASSERT_TRUE(manager->canExchangeMaterialResources(removeResources));
  manager->exchangeMaterialResources(removeResources);removeResources={};bind(*manager);
  auto restoreResources=manager->prepareMaterialResourcesChange(command,true);sealStandalone(restoreResources);ASSERT_TRUE(manager->canExchangeMaterialResources(restoreResources));
  manager->exchangeMaterialResources(restoreResources);restoreResources={};bind(*manager);
  auto detachResourcesAgain=manager->prepareMaterialResourcesChange(command,false);sealStandalone(detachResourcesAgain);ASSERT_TRUE(manager->canExchangeMaterialResources(detachResourcesAgain));
  manager->exchangeMaterialResources(detachResourcesAgain);detachResourcesAgain={};bind(*manager);
  auto removeMaterial=manager->prepareMaterialTailChange(originalMaterial,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publish(removeMaterial));removeMaterial={};
  auto replacement=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);
  const auto replacementMaterial=replacement.appendCommand();ASSERT_EQ(replacementMaterial.sourceMaterial(),1);
  ASSERT_TRUE(publish(replacement));replacement={};
  const auto revision=manager->materialResourceRevision(),epoch=manager->generationEpoch();
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,true),std::invalid_argument);
  EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_EQ(manager->generationEpoch(),epoch);
  auto detachReplacement=manager->prepareMaterialTailChange(replacementMaterial,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publish(detachReplacement));detachReplacement={};
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,true),std::invalid_argument);
  EXPECT_EQ(manager->materialTextureResourceCount(),0u);
}
} // namespace
