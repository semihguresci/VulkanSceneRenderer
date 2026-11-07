#include <Container/common/CommonGLFW.h>
#include <Container/geometry/GltfCompression.h>
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
#include <stdexcept>
#include <string_view>
#include <vector>

#ifndef CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH
#define CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH "spv_shaders/scene_texture_sampling_contract.comp.spv"
#endif

namespace container::scene {
struct SceneSamplerPublicationTestAccess {
  static auto& table(SceneManager& manager) {return manager.textureManager_;}
  static const auto& fallback(const SceneManager& manager) {return manager.textureSamplingFallback_->resource;}
  static auto& samplerCapacity(SceneManager& manager) {return manager.materialSamplerDescriptorCapacity_;}
  static auto& imageCapacity(SceneManager& manager) {return manager.textureDescriptorCapacity_;}
  static void install(SceneManager& manager,container::material::TextureManager table,
      std::shared_ptr<const container::gpu::AllocationManager::TextureAllocationLease> lease={}) {
    const std::array inherited{manager.textureImageOwnership_};
    auto images=container::material::PrepareTextureImageOwnership(table,*manager.allocationManager_,inherited);
    using std::swap;swap(manager.textureManager_,table);manager.textureImageOwnership_.swap(images);
    lease.reset();manager.uploadTextureMetadataBuffer();
  }
  static bool retire(SceneManager& manager,uint32_t slot) {
    if(!manager.textureManager_.retireTexture(slot)) return false;
    manager.uploadTextureMetadataBuffer();return true;
  }
  static void refresh(SceneManager& manager) {manager.uploadTextureMetadataBuffer();}
};
} // namespace container::scene
namespace {
using namespace container::scene;
using namespace container::geometry;
using namespace container::geometry::gltf;
using namespace container::gpu;
namespace material=container::material;
using Access=SceneSamplerPublicationTestAccess;
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
    struct ReadbackOwner {
      AllocationManager& allocation;
      AllocatedBuffer buffer;
      ~ReadbackOwner() {allocation.destroyBuffer(buffer);}
    } outputOwner{*allocation,allocation->createBuffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT)};
    auto& output=outputOwner.buffer;
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
    return result;
  }
  void expectThree(SceneManager& scene,VkDescriptorSet set,bool middleDead) {
    const auto values=sample(scene,set,3);ASSERT_EQ(values.size(),6u);
    EXPECT_EQ(values[0],glm::vec4(1,0,0,1));EXPECT_EQ(values[2],middleDead?glm::vec4(1):glm::vec4(0,1,0,1));EXPECT_EQ(values[4],glm::vec4(0,0,1,1));
    EXPECT_EQ(values[1],glm::vec4(0,2,0,0));EXPECT_EQ(values[3],glm::vec4(0,2,0,0));EXPECT_EQ(values[5],glm::vec4(1,2,0,0));
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
  EXPECT_EQ(Access::table(*manager).textureCount(),3u);EXPECT_EQ(Access::table(*manager).activeTextureCount(),2u);
  expectThree(*manager,manager->descriptorSet(0),true);
  const auto clamped=sample(*manager,manager->descriptorSet(0),3,{1.25f,.5f});ASSERT_EQ(clamped.size(),6u);
  // The last image is [yellow, blue]. Its nearest/clamp sampler must select
  // blue at the center boundary and beyond the right edge. Linear or repeat
  // descriptors produce distinct values, so metadata agreement alone cannot pass.
  EXPECT_EQ(clamped[4],glm::vec4(0,0,1,1));

}

TEST_F(SceneTextureSamplingGpu,ZeroAndAllDeadCatalogsHaveExplicitPrivateFallbackDescriptors) {
  ASSERT_EQ(Access::table(*manager).textureCount(),0u);const auto empty=sample(*manager,manager->descriptorSet(0),1);
  ASSERT_EQ(empty.size(),2u);EXPECT_EQ(empty[0],glm::vec4(1));EXPECT_EQ(empty[1],glm::vec4(0,1,0,0));
  installThree();for(uint32_t i=0;i<3;++i) ASSERT_TRUE(Access::retire(*manager,i));bind(*manager);
  const auto dead=sample(*manager,manager->descriptorSet(0),3);ASSERT_EQ(dead.size(),6u);
  for(size_t i=0;i<3;++i) {EXPECT_EQ(dead[i*2],glm::vec4(1));EXPECT_EQ(dead[i*2+1],glm::vec4(0,1,0,0));}
  EXPECT_EQ(Access::table(*manager).textureCount(),3u);EXPECT_EQ(Access::table(*manager).activeTextureCount(),0u);
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


TEST_F(SceneTextureSamplingGpu,ChangedSamplerPolicyRequiresFreshMetadataBeforeDescriptors) {
  installThree();const auto before=manager->descriptorSet(0);
  auto changed=*Access::table(*manager).getTexture(2);
  changed.name="Appended linear alias";changed.samplerIndex=material::NormalizeMaterialSamplerKey(9729,9729,33071,33071).value;
  ASSERT_EQ(Access::table(*manager).registerTexture(changed),3u);
  EXPECT_THROW(bind(*manager),std::runtime_error);
  EXPECT_EQ(manager->descriptorSet(0),before);
  Access::refresh(*manager);bind(*manager);const auto values=sample(*manager,before,4);
  EXPECT_EQ(values[4],glm::vec4(0,0,1,1));
  // Normalized texture filtering has finite implementation precision; retain
  // the independent sampler oracle's tolerance while rejecting nearest/repeat.
  for(int channel=0;channel<3;++channel) EXPECT_NEAR(values[6][channel],.5f,2e-4f);
  EXPECT_EQ(values[6].a,1);
  EXPECT_EQ(values[5],glm::vec4(1,3,0,0));EXPECT_EQ(values[7],glm::vec4(2,3,0,0));
}

TEST_F(SceneTextureSamplingGpu,OverBudgetReplacementLeavesThePublishedSceneIntact) {
  installThree();const auto descriptor=manager->descriptorSet(0);
  auto incoming=triangle("Over budget replacement");auto& source=incoming.gltfModel;
  source.images.resize(1);auto& image=source.images[0];image.width=image.height=1;
  image.bits=8;image.component=4;image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.image={255,255,255,255};image.uri="resident.png";
  source.samplers.resize(2);source.textures.resize(2);
  for(int i=0;i<2;++i) {
    source.samplers[i].minFilter=i==0?9728:9729;source.samplers[i].magFilter=9729;
    source.textures[i].source=0;source.textures[i].sampler=i;
  }
  const auto capacity=Access::samplerCapacity(*manager);Access::samplerCapacity(*manager)=2;
  EXPECT_FALSE(manager->reloadModel("over-budget.gltf",1,cameras,objects[0],&incoming));
  Access::samplerCapacity(*manager)=capacity;
  EXPECT_EQ(manager->descriptorSet(0),descriptor);
  EXPECT_EQ(Access::table(*manager).textureCount(),3u);expectThree(*manager,descriptor,false);
}

TEST_F(SceneTextureSamplingGpu,MalformedRetainedTextureRejectsBeforeTheLiveSceneRetires) {
  installThree();const auto descriptor=manager->descriptorSet(0);
  auto incoming=triangle("Bad retained image");auto& source=incoming.gltfModel;
  source.images.resize(1);source.images[0].as_is=true;source.images[0].image={0,1,2};
  source.images[0].uri="corrupt.png";source.images[0].mimeType="image/png";
  source.textures.resize(1);source.textures[0].source=0;
  source.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
  EXPECT_FALSE(manager->reloadModel("bad-retained-image.gltf",1,cameras,objects[0],&incoming));
  EXPECT_EQ(manager->descriptorSet(0),descriptor);
  EXPECT_EQ(Access::table(*manager).textureCount(),3u);expectThree(*manager,descriptor,false);
}

TEST_F(SceneTextureSamplingGpu,MalformedCompressedReloadKeepsLiveGeometryAndDescriptors) {
  installThree();
  ASSERT_EQ(manager->vertices().size(),3u);
  const std::array positions{manager->vertices()[0].position, manager->vertices()[1].position,
                             manager->vertices()[2].position};
  const auto indices = manager->indices();
  ASSERT_EQ(indices.size(),3u);
  const std::array descriptors{manager->descriptorSet(0), manager->descriptorSet(1)};

  struct InvalidAsset {
    const char* extension;
    const char* payload;
    const char* decoderDiagnostic;
    bool decoderEnabled;
  };
  constexpr auto badMeshopt = R"(
    "buffers":[{"uri":"data:application/octet-stream;base64,oA==","byteLength":1},{"byteLength":36}],
    "bufferViews":[{"buffer":1,"byteLength":36,"byteStride":12,"extensions":{
      "EXT_meshopt_compression":{"buffer":0,"byteLength":1,"byteStride":12,"count":3,"mode":"ATTRIBUTES"}
    }}],
    "accessors":[{"bufferView":0,"componentType":5126,"type":"VEC3","count":3}],
    "meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}])";
  constexpr auto badDraco = R"(
    "accessors":[{"componentType":5126,"type":"VEC3","count":3}],
    "meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4,"extensions":{
      "KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":0}}
    }}]}])";
  const std::array assets{
    InvalidAsset{"EXT_meshopt_compression",badMeshopt,"malformed meshopt bitstream",HasMeshoptDecoder()},
    InvalidAsset{"KHR_draco_mesh_compression",badDraco,"Draco buffer view index is out of range",HasDracoDecoder()},
  };

  for (const auto& asset : assets) {
    SCOPED_TRACE(asset.extension);
    const auto path = temp / (std::string(asset.extension) + ".gltf");
    {
      std::ofstream file(path);
      file << R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0,)"
           << R"("extensionsUsed":[")" << asset.extension << R"("],"extensionsRequired":[")"
           << asset.extension << R"("],)" << asset.payload << '}';
      ASSERT_TRUE(file.good());
    }
    try {
      (void)manager->prepareGltfModel(path.string());
      FAIL() << "Malformed compressed source unexpectedly passed preparation";
    } catch (const std::runtime_error& error) {
      const std::string_view message(error.what());
      EXPECT_NE(message.find(path.filename().string()),std::string_view::npos);
      EXPECT_NE(message.find(asset.decoderEnabled ? asset.decoderDiagnostic : asset.extension),
                std::string_view::npos) << message;
    }
    EXPECT_FALSE(manager->reloadModel(path.string(),1,cameras,objects[0]));
    ASSERT_EQ(manager->vertices().size(),positions.size());
    for (size_t i = 0; i < positions.size(); ++i)
      EXPECT_EQ(manager->vertices()[i].position,positions[i]);
    EXPECT_EQ(manager->indices(),indices);
    ASSERT_EQ(Access::table(*manager).textureCount(),3u);
    for (uint32_t frame = 0; frame < descriptors.size(); ++frame) {
      ASSERT_EQ(manager->descriptorSet(frame),descriptors[frame]);
      expectThree(*manager,descriptors[frame],false);
    }
  }
}

TEST_F(SceneTextureSamplingGpu,ImageDescriptorBudgetRejectsBeforeTheLiveSceneRetires) {
  installThree();const auto descriptor=manager->descriptorSet(0);
  auto incoming=triangle("Image descriptor budget");auto& source=incoming.gltfModel;
  source.images.resize(3);source.textures.resize(3);source.materials.resize(3,source.materials[0]);
  for(int i=0;i<3;++i) {
    auto& image=source.images[i];image.width=image.height=1;image.bits=8;image.component=4;
    image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;image.image={0,0,0,255};
    image.image[i]=255;image.uri="resident-"+std::to_string(i)+".png";
    source.textures[i].source=i;source.materials[i].pbrMetallicRoughness.baseColorTexture.index=i;
  }
  const auto capacity=Access::imageCapacity(*manager);Access::imageCapacity(*manager)=2;
  EXPECT_FALSE(manager->reloadModel("over-image-budget.gltf",1,cameras,objects[0],&incoming));
  Access::imageCapacity(*manager)=capacity;
  EXPECT_EQ(manager->descriptorSet(0),descriptor);
  EXPECT_EQ(Access::table(*manager).textureCount(),3u);expectThree(*manager,descriptor,false);
}
} // namespace
