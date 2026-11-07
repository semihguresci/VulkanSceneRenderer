#include <Container/common/CommonGLFW.h>
#include <Container/geometry/GltfModelLoader.h>
#include <Container/geometry/GltfSceneExport.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/GltfMaterialConversion.h>
#include <Container/utility/GltfRuntimeMaterialAuthoring.h>
#include <Container/utility/MaterialSamplerKey.h>
#include <Container/utility/MaterialBindingAuthoring.h>
#include <Container/utility/PipelineManager.h>
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
#include <fstream>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

#ifndef CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH
#define CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH "spv_shaders/scene_texture_sampling_contract.comp.spv"
#endif

namespace {
using namespace container::scene;
using namespace container::geometry;
using namespace container::gpu;

GltfLoadResult triangle(const std::string& name,unsigned char red,bool implicit=false,bool textured=true) {
  GltfLoadResult result;
  auto& source=result.gltfModel;source.asset.version="2.0";
  source.buffers.resize(1);
  const std::array<float,9> positions{0,0,0,1,0,0,0,1,0};
  const std::array<float,6> uv{0,0,1,0,0,1};
  const std::array<uint16_t,3> indices{0,1,2};
  source.buffers[0].data.resize(sizeof(positions)+sizeof(uv)+sizeof(indices));
  std::memcpy(source.buffers[0].data.data(),positions.data(),sizeof(positions));
  std::memcpy(source.buffers[0].data.data()+sizeof(positions),uv.data(),sizeof(uv));
  std::memcpy(source.buffers[0].data.data()+sizeof(positions)+sizeof(uv),indices.data(),sizeof(indices));
  source.bufferViews.resize(3);
  source.bufferViews[0].buffer=0;source.bufferViews[0].byteLength=sizeof(positions);
  source.bufferViews[0].target=TINYGLTF_TARGET_ARRAY_BUFFER;
  source.bufferViews[1].buffer=0;source.bufferViews[1].byteOffset=sizeof(positions)+sizeof(uv);
  source.bufferViews[1].byteLength=sizeof(indices);source.bufferViews[1].target=TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
  source.bufferViews[2].buffer=0;source.bufferViews[2].byteOffset=sizeof(positions);
  source.bufferViews[2].byteLength=sizeof(uv);source.bufferViews[2].target=TINYGLTF_TARGET_ARRAY_BUFFER;
  source.accessors.resize(3);
  source.accessors[0].bufferView=0;source.accessors[0].componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  source.accessors[0].type=TINYGLTF_TYPE_VEC3;source.accessors[0].count=3;
  source.accessors[0].minValues={0,0,0};source.accessors[0].maxValues={1,1,0};
  source.accessors[1].bufferView=1;source.accessors[1].componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
  source.accessors[1].type=TINYGLTF_TYPE_SCALAR;source.accessors[1].count=3;
  source.accessors[2].bufferView=2;source.accessors[2].componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  source.accessors[2].type=TINYGLTF_TYPE_VEC2;source.accessors[2].count=3;
  if(!implicit) {
    source.materials.resize(1);
    source.materials[0].name=name+" material";
    source.materials[0].pbrMetallicRoughness.baseColorFactor={red/255.0,0.25,0.5,1};
    if(textured) {
      source.images.resize(1);auto& image=source.images[0];
      image.width=image.height=1;image.component=4;image.bits=8;
      image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
      image.image={red,47,109,255};image.uri="same.png";
      image.extras=tinygltf::Value(tinygltf::Value::Object{{"imageOwner",tinygltf::Value(name)}});
      source.samplers.resize(1);source.samplers[0].wrapS=TINYGLTF_TEXTURE_WRAP_REPEAT;
      source.samplers[0].wrapT=TINYGLTF_TEXTURE_WRAP_REPEAT;
      source.textures.resize(1);source.textures[0].source=0;source.textures[0].sampler=0;
      source.textures[0].extras=tinygltf::Value(tinygltf::Value::Object{{"textureOwner",tinygltf::Value(name)}});
      source.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
    }
  }
  source.meshes.resize(1);tinygltf::Primitive primitive;
  primitive.attributes["POSITION"]=0;primitive.attributes["TEXCOORD_0"]=2;primitive.indices=1;
  primitive.material=implicit?-1:0;primitive.mode=TINYGLTF_MODE_TRIANGLES;
  source.meshes[0].primitives.push_back(primitive);
  source.nodes.resize(1);source.nodes[0].mesh=0;source.nodes[0].name=name;
  source.scenes.resize(1);source.scenes[0].nodes={0};source.defaultScene=0;
  std::vector<Vertex> vertices(3);
  for(size_t i=0;i<3;++i) {
    vertices[i].position={positions[i*3],positions[i*3+1],positions[i*3+2]};
    vertices[i].texCoord={uv[i*2],uv[i*2+1]};
    vertices[i].texCoord1={0,0}; // No TEXCOORD_1 accessor is authored.
    vertices[i].normal={0,0,1};
  }
  result.model=Model::FromMeshes({Mesh(std::move(vertices),{0,1,2},implicit?-1:0)});
  return result;
}

class MaterialRetirementGenerationGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<MaterialRetirementGenerationGpu*>(user)->errors;
      std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_MATERIAL_RETIREMENTS");
    if(!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_MATERIAL_RETIREMENTS=1";
    ASSERT_EQ(glfwInit(),GLFW_TRUE);glfwStarted=true;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    window=glfwCreateWindow(32,32,"Detached scene generation",nullptr,nullptr);ASSERT_NE(window,nullptr);
    uint32_t count{};const auto* extensions=glfwGetRequiredInstanceExtensions(&count);ASSERT_NE(extensions,nullptr);
    InstanceCreateInfo create;create.enableValidationLayers=true;create.validationLayers={"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions,extensions+count);
    create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount=1;validation.pEnabledValidationFeatures=&sync;create.next=&validation;
    instance=std::make_unique<VulkanInstance>(create);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation|vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback=reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(debugCallback);debug.pUserData=this;
    messenger=vk::raii::DebugUtilsMessengerEXT(instance->raii(),debug);
    VkSurfaceKHR surfaceHandle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(),window,nullptr,&surfaceHandle),VK_SUCCESS);
    surface=vk::raii::SurfaceKHR(instance->raii(),surfaceHandle);
    DeviceCreateInfo info;info.requiredExtensions={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress=VK_TRUE;v12.runtimeDescriptorArray=VK_TRUE;
    v12.descriptorBindingPartiallyBound=VK_TRUE;v12.shaderSampledImageArrayNonUniformIndexing=VK_TRUE;
    v12.shaderStorageBufferArrayNonUniformIndexing=VK_TRUE;
    info.next=&v12;
    device=std::make_shared<VulkanDevice>(*instance,surfaceHandle,info);
    pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,device->queueFamilyIndices().graphicsFamily.value()));
    allocation=std::make_unique<AllocationManager>();
    allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),
        device->graphicsQueue(),*pool,{});
    pipelines=std::make_unique<PipelineManager>(device->device());
    temp=std::filesystem::temp_directory_path()/("container-material-retirements-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();
    manager.reset();pipelines.reset();allocation.reset();pool.clear();device.reset();
    surface.clear();messenger.clear();instance.reset();
    if(window) glfwDestroyWindow(window);if(glfwStarted) glfwTerminate();
    if(!temp.empty()) { std::error_code ignored; const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()); const auto target=std::filesystem::weakly_canonical(temp); if(target.parent_path()==parent && target.filename().string().starts_with("container-material-retirements-")) std::filesystem::remove_all(target,ignored); }
    EXPECT_EQ(errors.load(),0u);
  }
  void load(bool implicit=false,bool withEmptyScene=false,bool withLight=false) {
    auto initial=triangle("A",31,implicit);
    if(withLight) {
      initial.gltfModel.lights.resize(1);initial.gltfModel.lights[0].type="point";
      initial.gltfModel.lights[0].intensity=7;initial.gltfModel.lights[0].range=3;
      initial.gltfModel.nodes[0].light=0;initial.gltfModel.extensionsUsed.push_back("KHR_lights_punctual");
    }
    if(withEmptyScene) {
      initial.gltfModel.scenes.emplace_back();
      initial.gltfModel.scenes.back().name="Empty scene";
    }
    loadSource(std::move(initial));
  }
  void loadSource(GltfLoadResult initial,bool projectWriter=false) {
    const auto path=temp/"a.glb";
    if(projectWriter) container::geometry::gltf::writeExportModel(initial.gltfModel,path);
    else {
      tinygltf::TinyGLTF writer;
      ASSERT_TRUE(writer.WriteGltfSceneToFile(&initial.gltfModel,path.string(),true,true,true,true));
    }
    manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});
    manager->initialize(path.string(),2.0f,2);
    manager->populateSceneGraph(graph);
    const auto oldRoots=graph.rootNodes();
    editor=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Scene Root");
    graph.getNode(editor)->editorSceneRoot=true;
    for(const auto root:oldRoots) ASSERT_TRUE(graph.setParent(root,editor));
    graph.updateWorldTransforms();
  }
  uint32_t sourceNode(int source) const {
    for(uint32_t id=0;id<graph.nodeCount();++id) if(graph.getNode(id)->sourceNodeIndex==source) return id;
    return SceneGraph::kInvalidNode;
  }
  uint32_t primitiveNode(uint32_t owner) const {
    for(uint32_t id=0;id<graph.nodeCount();++id)
      if(graph.getNode(id)->renderable && graph.getNode(id)->meshOwnerGraphNode==owner) return id;
    return SceneGraph::kInvalidNode;
  }
  SceneGraph primitiveCandidate(std::optional<uint32_t> parent) const {
    SceneNode node;node.parent=parent.value_or(SceneGraph::kInvalidNode);node.renderable=true;
    node.primitiveIndex=0;node.materialIndex=manager->defaultMaterialIndex();node.name="Editor primitive";
    if(parent) node.worldTransform=graph.getNode(*parent)->worldTransform;
    auto candidate=graph.makeAppendCandidate({node});
    if(graph.activeRoots() && (!parent || !graph.isNodeActive(*parent))) {
      auto roots=*graph.activeRoots();roots.push_back(uint32_t(graph.nodeCount()));candidate.setActiveRoots(roots);
    }
    return candidate;
  }
  SceneManager::ReparentSceneRootMembership reparentMembership(uint32_t node,
      std::optional<uint32_t> parent,const glm::mat4& local,size_t sibling) const {
    auto prototype=graph.preparePlacement(node,parent,local,sibling,graph.activeRoots());
    if(!prototype) throw std::runtime_error("Test reparent prototype is invalid");
    return manager->planReparentSceneRoots(graph,*prototype.candidateGraph(),node);
  }
  bool publishReparent(const SceneManager::ReparentSceneRootMembership& receipt,bool redo,
      uint32_t node,std::optional<uint32_t> parent,const glm::mat4& local,size_t sibling) {
    auto metadata=manager->prepareReparentSceneRootChange(graph,receipt,redo);
    auto placement=graph.preparePlacement(node,parent,local,sibling,receipt.activeRoots(redo));
    if(!placement || !manager->canPublishReparentSceneRootChange(metadata)) return false;
    // The actual frontend protocol: graph view/topology changes first, then
    // manager metadata swaps without consulting the old graph revision/view.
    return graph.publishPlacement(placement) && manager->publishReparentSceneRootChange(metadata);
  }
  uint32_t allocations() const {
    VmaTotalStatistics stats{};vmaCalculateStatistics(allocation->memoryManager()->allocator(),&stats);
    return stats.total.statistics.allocationCount;
  }
  std::vector<glm::vec4> sample(VkDescriptorSet sceneSet,uint32_t count) {
    if(!count || count>4096) throw std::invalid_argument("Material history sampling exceeds its row budget");
    const auto bytes=sizeof(glm::vec4)*size_t(count)*2;
    auto output=allocation->createBuffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if(!output.allocation_info.pMappedData) throw std::runtime_error("Material history readback is not mapped");
    std::fill_n(static_cast<uint32_t*>(output.allocation_info.pMappedData),bytes/sizeof(uint32_t),0x7fc00000u);
    if(vmaFlushAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
      throw std::runtime_error("Cannot poison material history readback");
    std::vector<glm::vec4> result;
    {
      const vk::DescriptorSetLayoutBinding binding(0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute);
      vk::raii::DescriptorSetLayout outputLayout(device->raii(),vk::DescriptorSetLayoutCreateInfo({},binding));
      const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer,1);
      vk::raii::DescriptorPool descriptors(device->raii(),vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,1,size));
      const std::array outputLayouts{*outputLayout};auto sets=device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors,outputLayouts));
      const vk::DescriptorBufferInfo info(output.buffer,0,bytes);const vk::WriteDescriptorSet write(*sets[0],0,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&info);
      device->raii().updateDescriptorSets(write,{});
      const std::array layouts{vk::DescriptorSetLayout(manager->descriptorSetLayout()),*outputLayout};
      const vk::PushConstantRange push(vk::ShaderStageFlagBits::eCompute,0,16);
      vk::raii::PipelineLayout layout(device->raii(),vk::PipelineLayoutCreateInfo({},layouts,push));
      const auto code=container::util::readFile(CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH);
      vk::raii::ShaderModule shader(device->raii(),vk::ShaderModuleCreateInfo({},code.size(),reinterpret_cast<const uint32_t*>(code.data())));
      vk::raii::Pipeline pipeline(device->raii(),nullptr,vk::ComputePipelineCreateInfo({},
          vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,*shader,"computeMain"),*layout));
      struct Query {uint32_t count,padding;float u,v;};const Query query{count,0,.5f,.5f};static_assert(sizeof(Query)==16);
      allocation->executeStagedCommands([&](VkCommandBuffer command) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,*pipeline);const std::array<VkDescriptorSet,2> raw{sceneSet,*sets[0]};
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,*layout,0,2,raw.data(),0,nullptr);
        vkCmdPushConstants(command,*layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(query),&query);vkCmdDispatch(command,(count+31)/32,1,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
      });
      if(vmaInvalidateAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
        throw std::runtime_error("Cannot read sampled material history textures");
      result.resize(count*2);std::memcpy(result.data(),output.allocation_info.pMappedData,bytes);
    }
    allocation->destroyBuffer(output);return result;
  }
  const SceneNode* sourcePrimitive(const SceneGraph& candidate,int sourceMesh) const {
    for(uint32_t id=0;id<candidate.nodeCount();++id)
      if(const auto* node=candidate.getNode(id);node->renderable && node->sourceMeshIndex==sourceMesh) return node;
    return nullptr;
  }
  std::atomic_uint errors{};bool glfwStarted{};GLFWwindow* window{};
  std::unique_ptr<VulkanInstance> instance;vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};std::shared_ptr<VulkanDevice> device;
  vk::raii::CommandPool pool{nullptr};std::unique_ptr<AllocationManager> allocation;
  std::unique_ptr<PipelineManager> pipelines;std::unique_ptr<SceneManager> manager;
  SceneGraph graph;uint32_t editor{};std::filesystem::path temp;
};

struct MaterialFrames {
  AllocationManager& allocation;
  std::vector<AllocatedBuffer> cameras,objects;
  explicit MaterialFrames(AllocationManager& allocator):allocation(allocator) {
    try {for(int i=0;i<2;++i) {
      cameras.push_back(allocation.createBuffer(sizeof(CameraData),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT));
      objects.push_back(allocation.createBuffer(sizeof(ObjectData),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT));
    }} catch(...) {clear();throw;}
  }
  void clear() {for(auto& b:cameras) allocation.destroyBuffer(b);for(auto& b:objects) allocation.destroyBuffer(b);cameras.clear();objects.clear();}
  ~MaterialFrames(){clear();}
  void bind(SceneManager::PreparedMaterialGeneration& stage) {stage.prepareDescriptorFrames(cameras,objects);}
};

int animationFloats(tinygltf::Model& model,std::initializer_list<float> values) {
  tinygltf::Buffer buffer;buffer.data.resize(values.size()*sizeof(float));std::memcpy(buffer.data.data(),values.begin(),buffer.data.size());
  tinygltf::BufferView view;view.buffer=int(model.buffers.size());view.byteLength=buffer.data.size();
  tinygltf::Accessor accessor;accessor.sparse={};accessor.bufferView=int(model.bufferViews.size());accessor.count=values.size();
  accessor.type=TINYGLTF_TYPE_SCALAR;accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  const auto mm=std::minmax_element(values.begin(),values.end());accessor.minValues={*mm.first};accessor.maxValues={*mm.second};
  const auto result=int(model.accessors.size());model.buffers.push_back(std::move(buffer));model.bufferViews.push_back(view);model.accessors.push_back(accessor);return result;
}
GltfLoadResult retirementAsset(bool animated=false) {
  auto result=triangle("Retirement",87);auto& source=result.gltfModel;
  source.materials.resize(4,source.materials[0]);
  for(size_t i=0;i<4;++i) {source.materials[i].name="Source "+std::to_string(i);source.materials[i].extras=tinygltf::Value(tinygltf::Value::Object{{"row",tinygltf::Value(int(i))}});}
  source.meshes[0].primitives.push_back(source.meshes[0].primitives[0]);source.meshes[0].primitives[1].material=3;
  auto second=Mesh(result.model.meshes()[0].vertices(),result.model.meshes()[0].indices(),3);
  result.model=Model::FromMeshes({result.model.meshes()[0],std::move(second)});
  if(animated) {
    source.extensionsUsed.push_back("KHR_animation_pointer");tinygltf::Animation clip;tinygltf::AnimationSampler sampler;
    sampler.input=animationFloats(source,{0,2});sampler.output=animationFloats(source,{.2f,.8f});clip.samplers.push_back(sampler);
    tinygltf::AnimationChannel channel;channel.sampler=0;channel.target_node=-1;channel.target_path="pointer";
    channel.target_extensions["KHR_animation_pointer"]=tinygltf::Value(tinygltf::Value::Object{
        {"pointer",tinygltf::Value(std::string("/materials/0/pbrMetallicRoughness/roughnessFactor"))}});
    clip.channels.push_back(channel);source.animations.push_back(clip);
  }
  return result;
}
bool publishRetirement(SceneManager& manager,MaterialFrames& frames,SceneManager::PreparedMaterialGeneration& stage,
    container::geometry::gltf::AnimationPlayer* player=nullptr) {
  frames.bind(stage);if(!manager.canExchangeMaterialGeneration(stage,player)) return false;
  size_t allocations{};bool ready{};
  {material_publication_test::AllocationGuard guard(true);ready=manager.canExchangeMaterialGeneration(stage,player);
    if(ready) manager.exchangeMaterialGeneration(stage,player);allocations=guard.allocations();}
  return ready && !stage && allocations==0;
}
MaterialBindingContext bindingContext(const SceneManager& manager) {
  return {manager.materialAssetIdentity(),manager.sourceMaterialRuntimeMapping(),uint32_t(manager.materialCount()),uint32_t(manager.primitiveRanges().size())};
}
bool publishBoundMaterial(SceneManager& manager,MaterialFrames& frames,SceneGraph& graph,
    SceneManager::PreparedMaterialGeneration& stage,PreparedMaterialBindingChange& bindings,
    container::geometry::gltf::AnimationPlayer* player=nullptr) {
  frames.bind(stage);const auto context=bindingContext(manager);
  if(!manager.canExchangeMaterialGeneration(stage,player) || !CanPublishMaterialBindings(graph,bindings,context)) return false;
  size_t attempts{};bool ready{},bound{};
  {material_publication_test::AllocationGuard deny(true);
    ready=manager.canExchangeMaterialGeneration(stage,player) && CanPublishMaterialBindings(graph,bindings,context);
    if(ready) {bound=PublishMaterialBindings(graph,bindings,context);if(bound) manager.exchangeMaterialGeneration(stage,player);}
    attempts=deny.allocations();}
  return ready && bound && !stage && !bindings && attempts==0;
}
bool publishBinding(SceneManager& manager,MaterialFrames& frames,SceneGraph& graph,PreparedMaterialBindingChange& bindings) {
  auto scene=manager.prepareSceneBindings();scene.prepareDescriptorFrames(frames.cameras,frames.objects);
  const auto context=bindingContext(manager);
  if(!manager.canExchangeSceneBindings(scene) || !CanPublishMaterialBindings(graph,bindings,context)) return false;
  size_t attempts{};bool ready{},bound{};
  {material_publication_test::AllocationGuard deny(true);
    ready=manager.canExchangeSceneBindings(scene) && CanPublishMaterialBindings(graph,bindings,context);
    if(ready) {bound=PublishMaterialBindings(graph,bindings,context);if(bound) manager.exchangeSceneBindings(scene);}
    attempts=deny.allocations();}
  return ready && bound && !scene && !bindings && attempts==0;
}
} // namespace

TEST_F(MaterialRetirementGenerationGpu, MiddleMaterialRetirementPreservesIdsRecordsResourcesAndUndoAuthority) {
  using namespace container::geometry::gltf;
  loadSource(retirementAsset());MaterialFrames frames(*allocation);
  const auto runtime=manager->sourceMaterialRuntimeMapping()[1];const auto count=manager->materialCount();
  const auto sourceCount=manager->animationSource().materials.size(),incarnation=manager->materialIncarnation(runtime);
  const auto authored=manager->authoredMaterial(1),evaluated=manager->animationSource().materials[1];
  const auto source0=manager->authoredMaterial(0),source3=manager->authoredMaterial(3);
  const auto mapping=std::vector<uint32_t>(manager->sourceMaterialRuntimeMapping().begin(),manager->sourceMaterialRuntimeMapping().end());
  const auto* image=manager->animationSource().images[0].image.data();const auto buffer=manager->descriptorSet(0);
  auto stage=manager->prepareMaterialRetirement(runtime,graph,nullptr);auto command=stage.retirementCommand();
  ASSERT_TRUE(command);EXPECT_EQ(command.sourceMaterial(),1);EXPECT_EQ(command.runtimeMaterial(),runtime);
  EXPECT_FALSE(command.published());EXPECT_EQ(stage.retiringMaterial(),std::optional<uint32_t>(runtime));
  ASSERT_TRUE(publishRetirement(*manager,frames,stage));stage={};
  EXPECT_TRUE(command.published());EXPECT_TRUE(command.retired());EXPECT_FALSE(manager->materialIsActive(runtime));
  EXPECT_EQ(manager->materialCount(),count);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);
  EXPECT_EQ(manager->materialIncarnation(runtime),incarnation);EXPECT_EQ(manager->animationSource().images[0].image.data(),image);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(1),tinygltf::Material{}));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),source0));EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(3),source3));
  EXPECT_EQ(std::vector<uint32_t>(manager->sourceMaterialRuntimeMapping().begin(),manager->sourceMaterialRuntimeMapping().end()),mapping);
  EXPECT_NE(manager->descriptorSet(0),buffer);EXPECT_EQ(manager->retiredMaterialIndices(),(std::vector<int32_t>{int32_t(runtime)}));
  GltfMaterialPatch patch;patch.name="Forbidden retired edit";
  EXPECT_THROW((void)manager->planMaterialEdit(1,patch),std::invalid_argument);
  EXPECT_THROW((void)manager->planMaterialDuplicate(1),std::invalid_argument);
  EXPECT_THROW((void)manager->prepareMaterialTextureSource(1,0,MaterialTextureRoles::Color),std::invalid_argument);
  auto restore=manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Active,graph,nullptr);
  ASSERT_TRUE(publishRetirement(*manager,frames,restore));restore={};EXPECT_FALSE(command.retired());
  EXPECT_TRUE(manager->materialIsActive(runtime));EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(1),authored));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[1],evaluated));
  auto redo=manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Retired,graph,nullptr);
  ASSERT_TRUE(publishRetirement(*manager,frames,redo));EXPECT_TRUE(command.retired());EXPECT_EQ(manager->materialIncarnation(runtime),incarnation);
}

TEST_F(MaterialRetirementGenerationGpu, RuntimeOnlyMaterialGetsTrackedIncarnationAndRetiresWithoutSourceRestChanges) {
  using namespace container::geometry::gltf;
  loadSource(retirementAsset(true));MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.75));manager->publishAnimationChanges(dirty,graph);
  container::material::Material value;value.baseColor={.2f,.4f,.6f,1};
  const auto runtime=manager->createMaterial(value);ASSERT_TRUE(manager->materialIsActive(runtime));ASSERT_GT(manager->materialIncarnation(runtime),0u);
  manager->uploadMaterialResources();manager->updateDescriptorSets(frames.cameras,frames.objects);manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto source=manager->animationSource().materials;const auto rest=player.library().restMaterials;const auto time=player.time();
  auto stage=manager->prepareMaterialRetirement(runtime,graph,&player);const auto command=stage.retirementCommand();
  EXPECT_EQ(command.sourceMaterial(),-1);ASSERT_TRUE(publishRetirement(*manager,frames,stage,&player));stage={};
  EXPECT_FALSE(manager->materialIsActive(runtime));EXPECT_EQ(manager->animationSource().materials,source);EXPECT_EQ(player.library().restMaterials,rest);EXPECT_EQ(player.time(),time);
  auto restore=manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Active,graph,&player);
  ASSERT_TRUE(publishRetirement(*manager,frames,restore,&player));EXPECT_TRUE(manager->materialIsActive(runtime));EXPECT_EQ(player.time(),time);
  EXPECT_EQ(manager->animationSource().materials,source);EXPECT_EQ(player.library().restMaterials,rest);
}

TEST_F(MaterialRetirementGenerationGpu, ReferenceDefaultsHiddenBindingsRawPointersAndNormativeMetadataBlock) {
  loadSource(retirementAsset());const auto mapping=manager->sourceMaterialRuntimeMapping();
  EXPECT_THROW((void)manager->prepareMaterialRetirement(manager->defaultMaterialIndex(),graph,nullptr),std::invalid_argument);
  EXPECT_THROW((void)manager->prepareMaterialRetirement(manager->diagnosticMaterialIndex(),graph,nullptr),std::invalid_argument);
  EXPECT_THROW((void)manager->prepareMaterialRetirement(mapping[0],graph,nullptr),std::invalid_argument);
  const auto runtime=mapping[1];const auto leaf=graph.createNode(glm::mat4(1),runtime,true,0,"Hidden material binding");graph.setVisible(leaf,false);
  EXPECT_FALSE(manager->materialRetirementBlockReason(runtime,graph).empty());
  EXPECT_THROW((void)manager->prepareMaterialRetirement(runtime,graph,nullptr),std::invalid_argument);
  graph.getNode(leaf)->renderable=false;auto& source=manager->animationSource();
  tinygltf::Animation animation;tinygltf::AnimationChannel channel;channel.target_node=-1;channel.target_path="pointer";
  channel.target_extensions["KHR_animation_pointer"]=tinygltf::Value(tinygltf::Value::Object{
      {"pointer",tinygltf::Value(std::string("/materials/1/extensions/FUTURE_property/value"))}});
  animation.channels.push_back(channel);source.extensionsUsed.push_back("KHR_animation_pointer");source.animations.push_back(animation);
  EXPECT_THROW((void)manager->prepareMaterialRetirement(runtime,graph,nullptr),std::invalid_argument);
  source.animations.clear();source.extensionsUsed.pop_back();source.extensionsUsed.push_back("VENDOR_material_reference");
  source.materials[0].extensions["VENDOR_material_reference"]=tinygltf::Value(tinygltf::Value::Object{{"material",tinygltf::Value(1)}});
  EXPECT_THROW((void)manager->prepareMaterialRetirement(runtime,graph,nullptr),std::invalid_argument);
  EXPECT_TRUE(manager->materialIsActive(runtime));
}

TEST_F(MaterialRetirementGenerationGpu, CurrentClockRestAndUnrelatedPreparationsAreRevalidatedWithoutRewinding) {
  using namespace container::geometry::gltf;
  loadSource(retirementAsset(true));MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.5));manager->publishAnimationChanges(dirty,graph);
  const auto runtime=manager->sourceMaterialRuntimeMapping()[1];
  auto stale=manager->prepareMaterialRetirement(runtime,graph,&player);frames.bind(stale);
  dirty=player.seek(manager->animationSource(),1.5);manager->publishAnimationChanges(dirty,graph);
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stale,&player));stale={};
  GltfMaterialPatch edit;edit.name="Other unused source edit";
  auto other=manager->prepareMaterialGeneration(manager->planMaterialEdit(2,edit),&player);frames.bind(other);
  auto stage=manager->prepareMaterialRetirement(runtime,graph,&player);auto command=stage.retirementCommand();
  ASSERT_TRUE(publishRetirement(*manager,frames,stage,&player));stage={};
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(other,&player));other={};EXPECT_DOUBLE_EQ(player.time(),1.5);
  dirty=player.seek(manager->animationSource(),.25);manager->publishAnimationChanges(dirty,graph);
  const auto sampled=manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor;
  auto restore=manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Active,graph,&player);
  ASSERT_TRUE(publishRetirement(*manager,frames,restore,&player));EXPECT_DOUBLE_EQ(player.time(),.25);
  EXPECT_DOUBLE_EQ(manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor,sampled);
  EXPECT_EQ(player.library().restMaterials.size(),4u);EXPECT_EQ(manager->animationSource().animations.size(),1u);
}

TEST_F(MaterialRetirementGenerationGpu, FailureAndChangedTombstoneRetainCurrentPhaseAndHistoryAuthority) {
  loadSource(retirementAsset());MaterialFrames frames(*allocation);const auto runtime=manager->sourceMaterialRuntimeMapping()[1];
  const auto epoch=manager->generationEpoch(),count=manager->materialCount();
  for(const int kind:{0,1,2,3}) {
    MaterialGenerationLimits limits;if(kind==0) limits.maxPreparedCpuBytes=1;if(kind==1) limits.maxMaterialBufferBytes=1;
    if(kind==2) limits.references.maxOwnedBytes=1;if(kind==3) limits.references.maxTraversalSteps=1;
    EXPECT_THROW((void)manager->prepareMaterialRetirement(runtime,graph,nullptr,limits),std::invalid_argument);
    EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialCount(),count);EXPECT_TRUE(manager->materialIsActive(runtime));
  }
  auto stage=manager->prepareMaterialRetirement(runtime,graph,nullptr);const auto command=stage.retirementCommand();
  ASSERT_TRUE(publishRetirement(*manager,frames,stage));stage={};
  auto& record=manager->animationSource().materials[1];record.name="Unexpected tombstone mutation";
  EXPECT_THROW((void)manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Active,graph,nullptr),std::invalid_argument);
  EXPECT_TRUE(command.retired());EXPECT_FALSE(manager->materialIsActive(runtime));record.name.clear();
  auto restore=manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Active,graph,nullptr);
  ASSERT_TRUE(publishRetirement(*manager,frames,restore));EXPECT_FALSE(command.retired());
}

TEST_F(MaterialRetirementGenerationGpu, OriginalDeleteSurvivesMakeUniqueAssignmentUndoRedoWithoutReissuingItsMaterial) {
  using namespace container::geometry::gltf;
  loadSource(retirementAsset(true));MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.5));
  manager->publishAnimationChanges(dirty,graph);player.setLoop(false);player.setSpeed(.375);player.play();
  const auto node=primitiveNode(sourceNode(0));ASSERT_NE(node,SceneGraph::kInvalidNode);
  const auto original=graph.getNode(node)->materialIndex;
  const auto sourceCount=manager->animationSource().materials.size(),runtimeCount=manager->materialCount();
  const auto prefixMapping=std::vector<uint32_t>(manager->sourceMaterialRuntimeMapping().begin(),manager->sourceMaterialRuntimeMapping().end());
  const auto geometry=manager->geometryRevision(),resources=manager->materialResourceRevision();
  const auto sourceRest=player.library().restMaterials[0];const auto* geometryBytes=manager->animationSource().buffers[0].data.data();
  const auto imageCount=manager->animationSource().images.size(),textureCount=manager->animationSource().textures.size();
  GltfMaterialPatch patch;patch.name="Owned Make Unique endpoint";
  patch.values.push_back({GltfMaterialProperty::BaseColor,std::array{.125,.75,.25,1.}});
  auto unique=manager->prepareMaterialGeneration(manager->planMaterialMakeUnique(0,patch),&player);const auto append=unique.appendCommand();
  const auto frozen=unique.evaluatedMaterial();const auto source=append.sourceMaterial();const auto runtime=append.runtimeMaterial();
  ASSERT_EQ(size_t(source),sourceCount);ASSERT_EQ(runtime,runtimeCount);
  const std::array uniqueRequest{MaterialBindingRequest{node,original,runtime}};
  const auto uniqueBinding=PlanMaterialBindings(graph,bindingContext(*manager),uniqueRequest,
      MaterialBindingAppendTarget{uint32_t(source),runtime});
  auto bindUnique=PrepareMaterialBindings(graph,bindingContext(*manager),uniqueBinding);
  ASSERT_TRUE(publishBoundMaterial(*manager,frames,graph,unique,bindUnique,&player));unique={};
  const auto originalHandle=manager->materialResourceHandle(runtime);const auto incarnation=manager->materialIncarnation(runtime);
  const auto appendedAuthored=manager->authoredMaterial(source),appendedRest=player.library().restMaterials[size_t(source)];
  ASSERT_TRUE(originalHandle);ASSERT_GT(incarnation,0u);EXPECT_EQ(graph.getNode(node)->materialIndex,runtime);
  EXPECT_THROW((void)manager->prepareMaterialRetirement(runtime,graph,&player),std::invalid_argument);
  const auto samples=sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount()));
  ASSERT_EQ(samples.size(),manager->materialTextureResourceCount()*2);EXPECT_LT(samples[0].r,.5f);EXPECT_GT(samples[0].a,.99f);
  const std::array restoreRequest{MaterialBindingRequest{node,runtime,original}};
  const auto assignment=PlanMaterialBindings(graph,bindingContext(*manager),restoreRequest);
  auto assigned=PrepareMaterialBindings(graph,bindingContext(*manager),assignment);ASSERT_TRUE(publishBinding(*manager,frames,graph,assigned));
  auto removed=manager->prepareMaterialRetirement(runtime,graph,&player);const auto deleteCommand=removed.retirementCommand();
  ASSERT_TRUE(publishRetirement(*manager,frames,removed,&player));removed={};
  EXPECT_TRUE(deleteCommand.retired());EXPECT_FALSE(manager->materialIsActive(runtime));
  EXPECT_EQ(sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount())),samples);
  dirty=player.seek(manager->animationSource(),1.25);manager->publishAnimationChanges(dirty,graph);
  const auto currentPrefix=manager->animationSource().materials[0];
  for(int cycle=0;cycle<2;++cycle) {
    SCOPED_TRACE(cycle);const auto epoch=manager->generationEpoch();const auto lifecycleRevision=manager->materialLifecycleInventory().revision();
    auto restoreDelete=manager->prepareMaterialRetirementChange(deleteCommand,MaterialRetirementState::Active,graph,&player);
    ASSERT_TRUE(publishRetirement(*manager,frames,restoreDelete,&player));restoreDelete={};
    EXPECT_EQ(manager->materialResourceHandle(runtime),originalHandle);EXPECT_EQ(manager->materialIncarnation(runtime),incarnation);
    // This exact old preparation must not become valid again when the original
    // append resumes its earlier active endpoint stamp.
    auto stale=manager->prepareMaterialRetirementChange(deleteCommand,MaterialRetirementState::Retired,graph,&player);frames.bind(stale);
    auto undoAssignment=RebaseMaterialBindings(graph,bindingContext(*manager),assignment,MaterialBindingSide::Before);
    ASSERT_TRUE(publishBinding(*manager,frames,graph,undoAssignment));EXPECT_EQ(graph.getNode(node)->materialIndex,runtime);
    auto undoUnique=RebaseMaterialBindings(graph,bindingContext(*manager),uniqueBinding,MaterialBindingSide::Before);
    auto detached=manager->prepareMaterialTailChange(append,MaterialTailState::Detached,graph,&player,&undoUnique);
    ASSERT_TRUE(publishBoundMaterial(*manager,frames,graph,detached,undoUnique,&player));detached={};
    EXPECT_FALSE(append.present());EXPECT_EQ(manager->materialCount(),runtimeCount);
    EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);EXPECT_EQ(graph.getNode(node)->materialIndex,original);
    EXPECT_EQ(std::vector<uint32_t>(manager->sourceMaterialRuntimeMapping().begin(),manager->sourceMaterialRuntimeMapping().end()),prefixMapping);
    EXPECT_FALSE(manager->canExchangeMaterialGeneration(stale,&player));
    auto redoUnique=RebaseMaterialBindings(graph,bindingContext(*manager),uniqueBinding,MaterialBindingSide::After);
    auto restored=manager->prepareMaterialTailChange(append,MaterialTailState::Present,graph,&player,&redoUnique);
    const auto uploaded=restored.gpuMaterials();ASSERT_NE(restored.materialBuffer().allocation_info.pMappedData,nullptr);
    EXPECT_EQ(std::memcmp(restored.materialBuffer().allocation_info.pMappedData,uploaded.data(),uploaded.size_bytes()),0);
    ASSERT_TRUE(publishBoundMaterial(*manager,frames,graph,restored,redoUnique,&player));restored={};
    EXPECT_TRUE(append.present());EXPECT_EQ(manager->materialResourceHandle(runtime),originalHandle);
    EXPECT_EQ(manager->materialIncarnation(runtime),incarnation);EXPECT_EQ(manager->sourceMaterialRuntimeMapping().back(),runtime);
    EXPECT_EQ(manager->materialCount(),runtimeCount+1);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount+1);
    EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(source),appendedAuthored));
    EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[size_t(source)],frozen));
    EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[size_t(source)],appendedRest));
    auto redoAssignment=RebaseMaterialBindings(graph,bindingContext(*manager),assignment,MaterialBindingSide::After);
    ASSERT_TRUE(publishBinding(*manager,frames,graph,redoAssignment));EXPECT_EQ(graph.getNode(node)->materialIndex,original);
    const auto beforeRejected=manager->generationEpoch();const auto descriptor=manager->descriptorSet(0);
    size_t attempts{};bool accepted{};
    {material_publication_test::AllocationGuard deny(true);accepted=manager->canExchangeMaterialGeneration(stale,&player);
      manager->exchangeMaterialGeneration(stale,&player);attempts=deny.allocations();}
    EXPECT_FALSE(accepted);EXPECT_TRUE(stale);EXPECT_EQ(attempts,0u);EXPECT_EQ(manager->generationEpoch(),beforeRejected);
    EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_TRUE(manager->materialIsActive(runtime));stale={};
    auto redoDelete=manager->prepareMaterialRetirementChange(deleteCommand,MaterialRetirementState::Retired,graph,&player);
    ASSERT_TRUE(publishRetirement(*manager,frames,redoDelete,&player));redoDelete={};
    EXPECT_TRUE(deleteCommand.retired());EXPECT_FALSE(manager->materialIsActive(runtime));EXPECT_GT(manager->generationEpoch(),epoch);
    const auto inventory=manager->materialLifecycleInventory();EXPECT_GT(inventory.revision(),lifecycleRevision);
    const auto selected=std::ranges::find_if(inventory.rows(),[&](const auto& row){return row.resource==originalHandle;});
    ASSERT_NE(selected,inventory.rows().end());EXPECT_TRUE(selected->retired);EXPECT_FALSE(selected->active);
    EXPECT_EQ(manager->materialIncarnation(runtime),incarnation);EXPECT_EQ(graph.getNode(node)->materialIndex,original);
    EXPECT_EQ(sample(manager->descriptorSet(0),uint32_t(manager->materialTextureResourceCount())),samples);
    EXPECT_DOUBLE_EQ(player.time(),1.25);EXPECT_DOUBLE_EQ(player.speed(),.375);EXPECT_TRUE(player.playing());EXPECT_FALSE(player.loop());
    EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[0],currentPrefix));
    EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[0],sourceRest));
    EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(manager->materialResourceRevision(),resources);
    EXPECT_EQ(manager->animationSource().images.size(),imageCount);EXPECT_EQ(manager->animationSource().textures.size(),textureCount);
    EXPECT_EQ(manager->animationSource().buffers[0].data.data(),geometryBytes);
  }
}

TEST_F(MaterialRetirementGenerationGpu, RuntimeOnlyBirthPreservesLatestDeleteEndpointButCannotReviveCompetingOrdinaryReceipt) {
  using namespace container::geometry::gltf;
  load(true);MaterialFrames frames(*allocation);ASSERT_TRUE(manager->animationSource().materials.empty());
  container::material::Material value;value.baseColor={.2f,.4f,.6f,1};const auto origin=manager->createMaterial(value);
  manager->uploadMaterialResources();manager->updateDescriptorSets(frames.cameras,frames.objects);
  manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);AnimationPlayer player(manager->animationSource());
  ASSERT_EQ(manager->sourceMaterialForRuntime(origin),-1);
  auto initial=manager->prepareRuntimeMaterialMakeUnique(origin,{},&player);const auto append=initial.appendCommand();
  const auto frozen=initial.evaluatedMaterial();ASSERT_TRUE(publishRetirement(*manager,frames,initial,&player));initial={};
  const auto runtime=append.runtimeMaterial();ASSERT_EQ(append.sourceMaterial(),0);
  const auto handle=manager->materialResourceHandle(runtime);ASSERT_TRUE(handle);
  const auto samples=sample(manager->descriptorSet(0),1);ASSERT_EQ(samples.size(),2u);EXPECT_EQ(samples[0],glm::vec4(1));
  auto first=manager->prepareMaterialRetirement(runtime,graph,&player);const auto firstDelete=first.retirementCommand();
  ASSERT_TRUE(publishRetirement(*manager,frames,first,&player));first={};
  auto undoFirst=manager->prepareMaterialRetirementChange(firstDelete,MaterialRetirementState::Active,graph,&player);
  ASSERT_TRUE(publishRetirement(*manager,frames,undoFirst,&player));undoFirst={};
  auto second=manager->prepareMaterialRetirement(runtime,graph,&player);const auto secondDelete=second.retirementCommand();
  ASSERT_TRUE(publishRetirement(*manager,frames,second,&player));second={};
  auto undoSecond=manager->prepareMaterialRetirementChange(secondDelete,MaterialRetirementState::Active,graph,&player);
  ASSERT_TRUE(publishRetirement(*manager,frames,undoSecond,&player));undoSecond={};
  for(int cycle=0;cycle<2;++cycle) {
    SCOPED_TRACE(cycle);
    auto detached=manager->prepareMaterialTailChange(append,MaterialTailState::Detached,graph,&player);
    ASSERT_TRUE(publishRetirement(*manager,frames,detached,&player));detached={};
    ASSERT_TRUE(manager->animationSource().materials.empty());EXPECT_EQ(manager->sourceMaterialForRuntime(origin),-1);
    auto restored=manager->prepareMaterialTailChange(append,MaterialTailState::Present,graph,&player);
    ASSERT_TRUE(publishRetirement(*manager,frames,restored,&player));restored={};
    EXPECT_EQ(manager->materialResourceHandle(runtime),handle);EXPECT_EQ(manager->materialIncarnation(runtime),handle.incarnation());
    EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[0],frozen));
    const auto epoch=manager->generationEpoch(),revision=manager->materialRevision(),authored=manager->authoredMaterialRevision();
    const auto catalogRevision=manager->materialLifecycleInventory().revision();const auto descriptor=manager->descriptorSet(0);
    EXPECT_THROW((void)manager->prepareMaterialRetirementChange(firstDelete,MaterialRetirementState::Retired,graph,&player),std::invalid_argument);
    EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialRevision(),revision);EXPECT_EQ(manager->authoredMaterialRevision(),authored);
    EXPECT_EQ(manager->materialLifecycleInventory().revision(),catalogRevision);EXPECT_EQ(manager->descriptorSet(0),descriptor);
    EXPECT_TRUE(manager->materialIsActive(runtime));EXPECT_FALSE(firstDelete.retired());EXPECT_EQ(manager->materialResourceHandle(runtime),handle);
    auto redoSecond=manager->prepareMaterialRetirementChange(secondDelete,MaterialRetirementState::Retired,graph,&player);
    ASSERT_TRUE(publishRetirement(*manager,frames,redoSecond,&player));redoSecond={};EXPECT_TRUE(secondDelete.retired());
    auto restoreSecond=manager->prepareMaterialRetirementChange(secondDelete,MaterialRetirementState::Active,graph,&player);
    ASSERT_TRUE(publishRetirement(*manager,frames,restoreSecond,&player));restoreSecond={};
    EXPECT_EQ(sample(manager->descriptorSet(0),1),samples);EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(origin));
  }
}

TEST_F(MaterialRetirementGenerationGpu, PreparedCurveWitnessRejectsRawBytesChannelsAndReplacedPlayerWithoutAllocationOrPublication) {
  using namespace container::geometry::gltf;
  loadSource(retirementAsset(true));MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.5));
  manager->publishAnimationChanges(dirty,graph);player.setLoop(false);player.setSpeed(.375);player.play();
  const auto runtime=manager->sourceMaterialRuntimeMapping()[1];const auto handle=manager->materialResourceHandle(runtime);
  auto stage=manager->prepareMaterialRetirement(runtime,graph,&player);const auto command=stage.retirementCommand();frames.bind(stage);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  const auto epoch=manager->generationEpoch(),materialRevision=manager->materialRevision(),authoredRevision=manager->authoredMaterialRevision();
  const auto catalogRevision=manager->materialLifecycleInventory().revision(),graphRevision=graph.revision();
  const auto logical=manager->resourceLifecycleGeneration();const auto primary=manager->descriptorSet(0),auxiliary=manager->auxiliaryDescriptorSet(0);
  const auto authored=manager->authoredMaterial(1),evaluated=manager->animationSource().materials[1],rest=player.library().restMaterials[1];
  const auto rejectWithoutPublication=[&] {
    bool ready{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeMaterialGeneration(stage,&player);
      manager->exchangeMaterialGeneration(stage,&player);attempts=deny.allocations();}
    EXPECT_FALSE(ready);EXPECT_EQ(attempts,0u);EXPECT_TRUE(stage);EXPECT_FALSE(command.published());EXPECT_FALSE(command.retired());
    EXPECT_TRUE(manager->materialIsActive(runtime));EXPECT_EQ(manager->materialResourceHandle(runtime),handle);
    EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialRevision(),materialRevision);
    EXPECT_EQ(manager->authoredMaterialRevision(),authoredRevision);EXPECT_EQ(manager->materialLifecycleInventory().revision(),catalogRevision);
    EXPECT_EQ(graph.revision(),graphRevision);EXPECT_EQ(manager->resourceLifecycleGeneration(),logical);
    EXPECT_EQ(manager->descriptorSet(0),primary);EXPECT_EQ(manager->auxiliaryDescriptorSet(0),auxiliary);
    EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(1),authored));
    EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[1],evaluated));
    EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[1],rest));
    EXPECT_DOUBLE_EQ(player.time(),.5);EXPECT_DOUBLE_EQ(player.speed(),.375);EXPECT_TRUE(player.playing());EXPECT_FALSE(player.loop());
  };
  auto& source=manager->animationSource();auto& animation=source.animations[0];auto& sampler=animation.samplers[0];
  const auto interpolation=sampler.interpolation;sampler.interpolation="STEP";rejectWithoutPublication();sampler.interpolation=interpolation;
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  auto& pointer=animation.channels[0].target_extensions.at("KHR_animation_pointer").Get<tinygltf::Value::Object>().at("pointer");
  const auto originalPointer=pointer;pointer=tinygltf::Value(std::string("/materials/0/pbrMetallicRoughness/metallicFactor"));
  rejectWithoutPublication();pointer=originalPointer;ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  const auto originalName=animation.name;animation.name=std::string(1024,'L');rejectWithoutPublication();animation.name=originalName;
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  const auto& accessor=source.accessors[size_t(sampler.output)];const auto& view=source.bufferViews[size_t(accessor.bufferView)];
  auto* bytes=source.buffers[size_t(view.buffer)].data.data()+view.byteOffset+accessor.byteOffset;
  float original{};std::memcpy(&original,bytes,sizeof(original));const float changed=std::nextafter(original,1.f);
  std::memcpy(bytes,&changed,sizeof(changed));rejectWithoutPublication();std::memcpy(bytes,&original,sizeof(original));
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  // The same borrowed player address and playback clock cannot substitute a
  // different decoded curve library while the raw source stays unchanged.
  auto alternateSource=source;
  alternateSource.nodes=player.library().restNodes;alternateSource.materials=player.library().restMaterials;
  alternateSource.lights=player.library().restLights;alternateSource.cameras=player.library().restCameras;
  auto* alternateBytes=alternateSource.buffers[size_t(view.buffer)].data.data()+view.byteOffset+accessor.byteOffset;
  std::memcpy(alternateBytes,&changed,sizeof(changed));AnimationPlayer replacement(alternateSource);
  (void)replacement.selectClip(alternateSource,0);(void)replacement.seek(alternateSource,.5);
  replacement.setLoop(false);replacement.setSpeed(.375);replacement.play();
  ASSERT_EQ(replacement.library().restMaterials.size(),player.library().restMaterials.size());
  for(size_t row=0;row<player.library().restMaterials.size();++row)
    EXPECT_TRUE(GltfMaterialRecordsEqual(replacement.library().restMaterials[row],player.library().restMaterials[row]));
  using std::swap;swap(player,replacement);rejectWithoutPublication();swap(player,replacement);
  bool ready{};size_t attempts{};
  // This same transaction already owns descriptor frames. Commit it directly,
  // preserving the readiness-plus-exchange allocation-denial assertion.
  {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeMaterialGeneration(stage,&player);
    if(ready) manager->exchangeMaterialGeneration(stage,&player);attempts=deny.allocations();}
  EXPECT_TRUE(ready);EXPECT_EQ(attempts,0u);ASSERT_FALSE(stage);
  EXPECT_TRUE(command.published());EXPECT_TRUE(command.retired());EXPECT_FALSE(manager->materialIsActive(runtime));
}

