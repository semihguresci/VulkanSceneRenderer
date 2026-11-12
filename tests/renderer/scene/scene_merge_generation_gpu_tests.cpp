#include <Container/common/CommonGLFW.h>
#include <Container/geometry/GltfModelLoader.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/GltfMaterialConversion.h>
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
#include <utility>

namespace container::scene {
// Inspect the publication result before a fresh preparation can reconcile it.
struct SceneTextureImageOwnershipTestAccess {
  static bool materialDependenciesValid(const SceneManager& manager) noexcept {
    return manager.materialResourceDependenciesValid();
  }
  static TextureResourceLiveOwnerPtr materialOwner(const SceneManager& manager,uint32_t runtime) noexcept {
    return manager.materialResourceLiveOwner(runtime);
  }
};
}

namespace {
using namespace container::scene;
using namespace container::geometry;
using namespace container::gpu;

static_assert(!std::is_copy_constructible_v<SceneManager::PreparedMergeGeneration>);
static_assert(std::is_nothrow_move_constructible_v<SceneManager::PreparedMergeGeneration>);
static_assert(std::is_nothrow_move_assignable_v<SceneManager::PreparedMergeGeneration>);
static_assert(noexcept(std::declval<SceneManager&>().exchangeMergeGeneration(
    std::declval<SceneManager::PreparedMergeGeneration&>())));
static_assert(noexcept(std::declval<SceneManager::PreparedMergeGeneration&>().releasePreparationData()));
static_assert(!std::is_copy_constructible_v<SceneManager::PreparedEditorSceneRootChange>);
static_assert(std::is_nothrow_move_constructible_v<SceneManager::PreparedEditorSceneRootChange>);
static_assert(noexcept(std::declval<SceneManager&>().publishEditorSceneRootChange(
    std::declval<SceneManager::PreparedEditorSceneRootChange&>())));
static_assert(!std::is_copy_constructible_v<SceneManager::PreparedReparentSceneRootChange>);
static_assert(std::is_nothrow_move_constructible_v<SceneManager::PreparedReparentSceneRootChange>);
static_assert(noexcept(std::declval<SceneManager&>().publishReparentSceneRootChange(
    std::declval<SceneManager::PreparedReparentSceneRootChange&>())));
static_assert(!std::is_copy_constructible_v<SceneManager::PreparedSceneBindings>);
static_assert(std::is_nothrow_move_constructible_v<SceneManager::PreparedSceneBindings>);
static_assert(std::is_nothrow_move_assignable_v<SceneManager::PreparedSceneBindings>);
static_assert(noexcept(std::declval<const SceneManager&>().canExchangeSceneBindings(
    std::declval<const SceneManager::PreparedSceneBindings&>())));
static_assert(noexcept(std::declval<SceneManager&>().exchangeSceneBindings(
    std::declval<SceneManager::PreparedSceneBindings&>())));

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

GltfLoadResult roleAliasedTriangle() {
  auto result=triangle("Role aliases",31);auto& source=result.gltfModel;
  const auto texture=source.textures[0];source.textures.push_back(texture);source.textures.push_back(texture);
  source.textures[1].name="Same pixels and sampler, independent source identity";
  source.textures[2].name="Unused role must remain unavailable";
  auto& material=source.materials[0];material.pbrMetallicRoughness.metallicRoughnessTexture.index=0;
  material.emissiveTexture.index=1;material.normalTexture.index=1;
  return result;
}

template<class T,size_t N> int cloneAccessor(tinygltf::Model& source,const std::array<T,N>& values,
    int type,int component,size_t count) {
  tinygltf::Buffer buffer;buffer.data.resize(sizeof(values));std::memcpy(buffer.data.data(),values.data(),sizeof(values));
  tinygltf::BufferView view;view.buffer=int(source.buffers.size());view.byteLength=buffer.data.size();
  tinygltf::Accessor accessor;accessor.bufferView=int(source.bufferViews.size());accessor.sparse={};
  accessor.componentType=component;accessor.type=type;accessor.count=count;
  if constexpr(std::is_same_v<T,float>) if(type==TINYGLTF_TYPE_SCALAR) {
    const auto [low,high]=std::minmax_element(values.begin(),values.end());accessor.minValues={*low};accessor.maxValues={*high};
  }
  const auto id=int(source.accessors.size());source.buffers.push_back(std::move(buffer));
  source.bufferViews.push_back(std::move(view));source.accessors.push_back(std::move(accessor));return id;
}
GltfLoadResult cloneAnimatedAsset() {
  auto result=triangle("Animated shared mesh",127);auto& source=result.gltfModel;
  const auto delta=cloneAccessor(source,std::array<float,9>{0,0,.5f,0,0,.5f,0,0,.5f},
      TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT,3);
  source.meshes[0].primitives[0].targets={{{"POSITION",delta}}};source.meshes[0].weights={.25};
  source.meshes[0].primitives[0].attributes["JOINTS_0"]=cloneAccessor(source,std::array<uint8_t,12>{},
      TINYGLTF_TYPE_VEC4,TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,3);
  source.meshes[0].primitives[0].attributes["WEIGHTS_0"]=cloneAccessor(source,
      std::array<float,12>{1,0,0,0,1,0,0,0,1,0,0,0},TINYGLTF_TYPE_VEC4,TINYGLTF_COMPONENT_TYPE_FLOAT,3);
  source.nodes.resize(3);source.nodes[0].weights={.4};source.nodes[0].skin=0;
  source.nodes[1].name="External joint";source.nodes[1].translation={0,1,0};
  source.nodes[2].name="Instanced shared mesh";source.nodes[2].mesh=0;source.nodes[2].weights={.3};
  source.skins.resize(1);source.skins[0].joints={1};source.skins[0].skeleton=1;
  const auto translations=cloneAccessor(source,std::array<float,6>{0,0,0,2,0,0},
      TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  const auto ids=cloneAccessor(source,std::array<uint32_t,2>{16777217u,16777219u},
      TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,2);
  source.extensionsUsed.push_back("EXT_mesh_gpu_instancing");
  source.nodes[2].extensions["EXT_mesh_gpu_instancing"]=tinygltf::Value(tinygltf::Value::Object{
      {"attributes",tinygltf::Value(tinygltf::Value::Object{{"TRANSLATION",tinygltf::Value(translations)},{"_ID",tinygltf::Value(ids)}})}});
  const auto input=cloneAccessor(source,std::array<float,2>{0,1},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  const auto weights=cloneAccessor(source,std::array<float,2>{.4f,.9f},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  tinygltf::Animation clip;clip.name="Authored morph weights";tinygltf::AnimationSampler sampler;
  sampler.input=input;sampler.output=weights;sampler.interpolation="LINEAR";clip.samplers.push_back(sampler);
  tinygltf::AnimationChannel channel;channel.sampler=0;channel.target_node=0;channel.target_path="weights";
  clip.channels.push_back(channel);source.animations.push_back(clip);
  source.materials.push_back(source.materials[0]);source.materials[1].name="Shared variant";
  source.materials[1].pbrMetallicRoughness.baseColorFactor={.1,.2,.9,1};
  source.extensionsUsed.push_back("KHR_materials_variants");
  source.extensions["KHR_materials_variants"]=tinygltf::Value(tinygltf::Value::Object{
      {"variants",tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(tinygltf::Value::Object{{"name",tinygltf::Value(std::string("Blue"))}})})}});
  source.meshes[0].primitives[0].extensions["KHR_materials_variants"]=tinygltf::Value(tinygltf::Value::Object{
      {"mappings",tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(tinygltf::Value::Object{
          {"material",tinygltf::Value(1)},{"variants",tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(0)})}})})}});
  source.scenes[0].nodes={0,1,2};return result;
}

class SceneMergeGenerationGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<SceneMergeGenerationGpu*>(user)->errors;
      std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_SCENE_MERGE");
    if(!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_SCENE_MERGE=1";
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
    temp=std::filesystem::temp_directory_path()/("container-merge-generation-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();
    manager.reset();pipelines.reset();allocation.reset();pool.clear();device.reset();
    surface.clear();messenger.clear();instance.reset();
    if(window) glfwDestroyWindow(window);if(glfwStarted) glfwTerminate();
    if(!temp.empty()) { std::error_code ignored;std::filesystem::remove_all(temp,ignored); }
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
  void loadSource(GltfLoadResult initial) {
    const auto path=temp/"a.glb";
    tinygltf::TinyGLTF writer;
    ASSERT_TRUE(writer.WriteGltfSceneToFile(&initial.gltfModel,path.string(),true,true,true,true));
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
  void exchangeMerge(SceneManager::PreparedMergeGeneration& prepared) {
    prepared.prepareResourceDependencies(*manager);
    if(!prepared.resourceDependenciesReadyForExchange(*manager))
      throw std::runtime_error("Test merge dependency preparation became stale");
    size_t attempted{};
    {
      material_publication_test::AllocationGuard guard(true);
      manager->exchangeMergeGeneration(prepared);attempted=guard.allocations();
    }
    EXPECT_EQ(attempted,0u);
    prepared.releaseResourceDependencyPreparation();
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

TEST_F(SceneMergeGenerationGpu,PreparationKeepsLiveAssetsPathsAndDescriptorHandlesUnchanged) {
  load();manager->animationSource().defaultScene=-1;
  const auto before=manager->buildSceneExport(graph);
  const auto nodes=graph.nodeCount();const auto revision=graph.revision();
  const auto materialRevision=manager->materialRevision();
  const auto descriptor=manager->descriptorSet(0),auxiliary=manager->auxiliaryDescriptorSet(0);
  auto incoming=triangle("B",211);
  const auto original=incoming.gltfModel;
  auto staged=manager->prepareMergeGeneration(incoming,temp/"other"/"b.gltf",{},graph);
  EXPECT_TRUE(staged.readyForExchange(*manager,graph));
  EXPECT_EQ(graph.nodeCount(),nodes);EXPECT_EQ(graph.revision(),revision);
  EXPECT_EQ(manager->materialRevision(),materialRevision);
  EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->auxiliaryDescriptorSet(0),auxiliary);
  EXPECT_EQ(manager->buildSceneExport(graph),before);EXPECT_EQ(incoming.gltfModel,original);
  EXPECT_NE(staged.manager().descriptorSet(0),descriptor);
  EXPECT_NE(staged.manager().auxiliaryDescriptorSet(0),auxiliary);
  EXPECT_NE(staged.manager().sceneClipBuffer().buffer,manager->sceneClipBuffer().buffer);
  EXPECT_EQ(staged.manager().sourcePrimitiveToRuntime(),(std::vector<uint32_t>{0,1}));
  EXPECT_EQ(staged.manager().animationSource().images.size(),2u);
  EXPECT_EQ(staged.manager().animationSource().defaultScene,-1);
  EXPECT_EQ(staged.sourcePlan().evaluatedSource.defaultScene,-1);
  EXPECT_EQ(staged.sourcePlan().merge.model.defaultScene,0);
}

TEST_F(SceneMergeGenerationGpu,DiscardAndMoveReleaseOnlyDetachedImagesAndBuffers) {
  load();const auto baseline=allocations();const auto before=manager->buildSceneExport(graph);
  {
    auto staged=manager->prepareMergeGeneration(triangle("B",173),temp/"b.glb",{},graph);
    EXPECT_GT(allocations(),baseline);
    auto moved=std::move(staged);
    EXPECT_TRUE(moved.readyForExchange(*manager,graph));
    EXPECT_EQ(manager->buildSceneExport(graph),before);
  }
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->buildSceneExport(graph),before);
}

TEST_F(SceneMergeGenerationGpu,ExchangeUndoRedoKeepBothGenerationsAndMonotonicLiveRevisions) {
  load();const auto baseline=allocations();const auto original=manager->buildSceneExport(graph);
  const auto originalBytes=manager->mergeHistoryResidentBytes();
  auto staged=manager->prepareMergeGeneration(triangle("B",151),temp/"b.glb",{},graph);
  const auto mergedGraph=staged.graph(); // History owns its graph receipt separately.
  const auto merged=staged.manager().buildSceneExport(mergedGraph);
  const auto mergedBytes=staged.manager().mergeHistoryResidentBytes();
  EXPECT_GT(mergedBytes,originalBytes);
  const auto descriptorA=manager->descriptorSet(0),descriptorB=staged.manager().descriptorSet(0);
  auto epoch=manager->generationEpoch(),geometry=manager->geometryRevision(),material=manager->materialRevision();
  exchangeMerge(staged);
  EXPECT_GT(manager->generationEpoch(),epoch);EXPECT_GT(manager->geometryRevision(),geometry);
  EXPECT_GT(manager->materialRevision(),material);EXPECT_EQ(manager->descriptorSet(0),descriptorB);
  EXPECT_EQ(manager->buildSceneExport(mergedGraph),merged);
  const auto ownedAllocations=allocations();
  staged.releasePreparationData();
  staged.releasePreparationData(); // Retiring preparation data is idempotent.
  EXPECT_FALSE(staged.readyForExchange(*manager,graph));
  EXPECT_EQ(allocations(),ownedAllocations);
  EXPECT_EQ(manager->mergeHistoryResidentBytes(),mergedBytes);
  EXPECT_EQ(staged.manager().mergeHistoryResidentBytes(),originalBytes);
  epoch=manager->generationEpoch();geometry=manager->geometryRevision();material=manager->materialRevision();
  exchangeMerge(staged);
  EXPECT_GT(manager->generationEpoch(),epoch);EXPECT_GT(manager->geometryRevision(),geometry);
  EXPECT_GT(manager->materialRevision(),material);EXPECT_EQ(manager->descriptorSet(0),descriptorA);
  EXPECT_EQ(manager->buildSceneExport(graph),original);
  EXPECT_GT(allocations(),baseline); // Redo retains its own live Vulkan resources.
  exchangeMerge(staged);
  EXPECT_EQ(manager->buildSceneExport(mergedGraph),merged);
  EXPECT_EQ(manager->mergeHistoryResidentBytes(),mergedBytes);
}

TEST_F(SceneMergeGenerationGpu,PreflightBudgetsAndMalformedGeometryLeaveLiveAllocationsUnchanged) {
  load();const auto baseline=allocations();const auto nodeCount=graph.nodeCount();
  const auto original=manager->buildSceneExport(graph);auto incoming=triangle("B",191);
  SceneMergeGenerationLimits limits;limits.maxPreparedCpuBytes=1;
  EXPECT_THROW((void)manager->prepareMergeGeneration(incoming,temp/"b.glb",{},graph,limits),std::runtime_error);
  limits={};limits.maxNewTextureBytes=1;
  EXPECT_THROW((void)manager->prepareMergeGeneration(incoming,temp/"b.glb",{},graph,limits),std::runtime_error);
  limits={};limits.graph.hierarchy.maxGraphNodes=1;
  EXPECT_THROW((void)manager->prepareMergeGeneration(incoming,temp/"b.glb",{},graph,limits),std::runtime_error);
  incoming.model=Model::FromMeshes({Mesh(std::vector<Vertex>(3),{0,1,3},0)});
  EXPECT_THROW((void)manager->prepareMergeGeneration(incoming,temp/"b.glb",{},graph),std::runtime_error);
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(graph.nodeCount(),nodeCount);
  EXPECT_EQ(manager->buildSceneExport(graph),original);
}

TEST_F(SceneMergeGenerationGpu,IncomingImplicitDefaultIsIndependentAndSurvivesVariantResetAndExport) {
  load(true);
  const auto* a=sourcePrimitive(graph,0);ASSERT_NE(a,nullptr);const auto oldDefault=a->materialIndex;
  auto staged=manager->prepareMergeGeneration(triangle("B",0,true,false),temp/"b.glb",{},graph);
  const auto* b=sourcePrimitive(staged.graph(),1);ASSERT_NE(b,nullptr);
  EXPECT_NE(b->materialIndex,oldDefault);
  EXPECT_EQ(staged.manager().resolveMaterialColor(b->materialIndex),glm::vec4(1));
  EXPECT_EQ(staged.manager().resolveMaterialMetallicRoughnessFactors(b->materialIndex),glm::vec2(1));
  EXPECT_EQ(staged.manager().animationSource().meshes[1].primitives[0].material,-1);
  const auto reset=staged.manager().planMaterialVariant(staged.graph(),std::nullopt);
  ASSERT_TRUE(staged.manager().applyMaterialVariant(staged.graph(),reset));
  const auto* after=sourcePrimitive(staged.graph(),1);ASSERT_NE(after,nullptr);
  EXPECT_EQ(after->materialIndex,b->materialIndex);
  const auto exported=staged.manager().buildSceneExport(staged.graph());
  EXPECT_TRUE(std::ranges::any_of(exported.materials,[](const auto& value) {
    return value.pbrMetallicRoughness.metallicFactor==1 &&
        value.pbrMetallicRoughness.baseColorFactor==std::vector<double>({1,1,1,1});
  }));
}

TEST_F(SceneMergeGenerationGpu,ProceduralGapAndRootOrderingRemainStableAcrossReplans) {
  load();const auto manual=manager->createSolidMaterial({0.8f,0.1f,0.3f,1});
  EXPECT_EQ(manager->appendRuntimeMesh(Model::MakeCube().meshes().front()),1u);
  const auto procedural=graph.createNode(glm::mat4(1),manual,true,1,"manual");
  ASSERT_TRUE(graph.setParent(procedural,editor));manager->registerActiveSceneNode(graph,procedural);
  graph.updateWorldTransforms();
  auto staged=manager->prepareMergeGeneration(triangle("B",73),temp/"b.glb",{},graph);
  EXPECT_EQ(staged.manager().sourcePrimitiveToRuntime(),(std::vector<uint32_t>{0,2}));
  EXPECT_EQ(staged.manager().runtimePrimitiveToSource(),(std::vector<int32_t>{0,-1,1}));
  EXPECT_EQ(staged.graph().getNode(procedural)->materialIndex,manual);
  EXPECT_EQ(staged.graph().getNode(procedural)->primitiveIndex,1u);
  ASSERT_TRUE(staged.graph().activeRoots());
  EXPECT_EQ(staged.manager().planSceneSelection(staged.graph(),staged.manager().selectedScene()).runtimeRoots,
      *staged.graph().activeRoots());
  graph.setVisible(procedural,false);
  EXPECT_FALSE(staged.readyForExchange(*manager,graph));
}

TEST_F(SceneMergeGenerationGpu,EmptySceneMembershipPublishesAfterGraphAndSurvivesSwitchUndoRedo) {
  load(false,true);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  ASSERT_TRUE(graph.activeRoots());ASSERT_TRUE(graph.activeRoots()->empty());
  ASSERT_FALSE(graph.isNodeInActiveHierarchy(editor));
  const auto beforeView=graph.activeRoots();
  const auto source=*graph.getNode(0);
  const auto first=static_cast<uint32_t>(graph.nodeCount());
  auto candidate=graph.makeEmptyNodeCandidate(editor,"Empty authored node");
  auto membership=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  const auto receipt=membership.membership();
  EXPECT_EQ(receipt.sceneIndex(),1u);EXPECT_EQ(receipt.nodeIndex(),first);
  EXPECT_TRUE(receipt.requiresExplicitRoot());
  EXPECT_EQ(std::vector<uint32_t>(membership.candidateRoots().begin(),membership.candidateRoots().end()),
      (std::vector<uint32_t>{first}));
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(manager->canPublishEditorSceneRootChange(membership));
  ASSERT_TRUE(graph.publishAppend(append));
  // Publication deliberately follows the graph replacement: no revision or
  // active-view guard may invalidate an already prepared metadata operation.
  ASSERT_TRUE(manager->canPublishEditorSceneRootChange(membership));
  ASSERT_TRUE(manager->publishEditorSceneRootChange(membership));
  EXPECT_FALSE(membership);EXPECT_FALSE(manager->publishEditorSceneRootChange(membership));
  EXPECT_EQ(manager->planSceneSelection(graph,1).runtimeRoots,(std::vector<uint32_t>{first}));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
  EXPECT_FALSE(graph.isNodeActive(first));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  EXPECT_TRUE(graph.isNodeActive(first));
  EXPECT_EQ(graph.getNode(first)->name,"Empty authored node");
  EXPECT_EQ(graph.getNode(0)->sourceNodeIndex,source.sourceNodeIndex);
  EXPECT_EQ(graph.getNode(0)->localTransform,source.localTransform);

  auto remove=manager->prepareEditorSceneRootChange(graph,receipt,false);
  auto detach=graph.prepareDetachTail(first,beforeView);ASSERT_TRUE(detach);
  ASSERT_TRUE(manager->canPublishEditorSceneRootChange(remove));
  ASSERT_TRUE(graph.publishTailChange(detach));
  ASSERT_TRUE(manager->canPublishEditorSceneRootChange(remove));
  ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  auto tail=detach.takeDetachedTail();ASSERT_TRUE(tail);
  EXPECT_TRUE(manager->planSceneSelection(graph,1).runtimeRoots.empty());

  auto restore=manager->prepareEditorSceneRootChange(graph,receipt,true);
  auto attach=graph.prepareRestoreTail(*tail);ASSERT_TRUE(attach);
  ASSERT_TRUE(manager->canPublishEditorSceneRootChange(restore));
  ASSERT_TRUE(graph.publishTailChange(attach));
  ASSERT_TRUE(manager->canPublishEditorSceneRootChange(restore));
  ASSERT_TRUE(manager->publishEditorSceneRootChange(restore));
  ASSERT_TRUE(graph.activeRoots());EXPECT_EQ(*graph.activeRoots(),(std::vector<uint32_t>{first}));
  EXPECT_EQ(manager->planSceneSelection(graph,1).runtimeRoots,*graph.activeRoots());
}

TEST_F(SceneMergeGenerationGpu,InheritedMembershipIsConsumableAndSceneChangesInvalidatePreparedTokens) {
  load(false,true);
  uint32_t parent=SceneGraph::kInvalidNode;
  for(uint32_t id=0;id<graph.nodeCount();++id)
    if(graph.getNode(id)->sourceNodeIndex==0) parent=id;
  ASSERT_NE(parent,SceneGraph::kInvalidNode);ASSERT_TRUE(graph.isNodeActive(parent));
  const auto beforeView=graph.activeRoots();const auto first=static_cast<uint32_t>(graph.nodeCount());
  auto candidate=graph.makeEmptyNodeCandidate(parent,"Inherited empty");
  auto registration=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  const auto receipt=registration.membership();EXPECT_FALSE(receipt.requiresExplicitRoot());
  EXPECT_TRUE(registration.candidateRoots().empty());
  auto moved=std::move(registration);EXPECT_FALSE(registration);ASSERT_TRUE(moved);
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(moved));
  EXPECT_TRUE(graph.isNodeActive(first));
  auto stale=manager->prepareEditorSceneRootChange(graph,receipt,false);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  EXPECT_FALSE(graph.isNodeActive(first));EXPECT_FALSE(manager->canPublishEditorSceneRootChange(stale));
  EXPECT_THROW((void)manager->prepareEditorSceneRootChange(graph,receipt,false),std::invalid_argument);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
  EXPECT_TRUE(graph.isNodeActive(first));EXPECT_FALSE(manager->canPublishEditorSceneRootChange(stale));
  auto remove=manager->prepareEditorSceneRootChange(graph,receipt,false);
  auto detach=graph.prepareDetachTail(first,beforeView);ASSERT_TRUE(detach);
  ASSERT_TRUE(graph.publishTailChange(detach));ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  auto tail=detach.takeDetachedTail();ASSERT_TRUE(tail);
  auto restore=manager->prepareEditorSceneRootChange(graph,receipt,true);
  auto attach=graph.prepareRestoreTail(*tail);ASSERT_TRUE(attach);
  ASSERT_TRUE(graph.publishTailChange(attach));ASSERT_TRUE(manager->publishEditorSceneRootChange(restore));
  EXPECT_TRUE(manager->planSceneSelection(graph,0).runtimeRoots==*beforeView);
  EXPECT_TRUE(graph.isNodeActive(first));
}

TEST_F(SceneMergeGenerationGpu,DiscardedEmptyRedoCannotRegisterReusedTailIdInAnotherScene) {
  load(false,true);
  const auto beforeView=graph.activeRoots();const auto first=static_cast<uint32_t>(graph.nodeCount());
  auto candidate=graph.makeEmptyNodeCandidate(editor,"Scene A empty");
  auto registration=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  const auto receiptA=registration.membership();ASSERT_TRUE(receiptA.requiresExplicitRoot());
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(registration));
  auto remove=manager->prepareEditorSceneRootChange(graph,receiptA,false);
  auto detach=graph.prepareDetachTail(first,beforeView);ASSERT_TRUE(detach);
  ASSERT_TRUE(graph.publishTailChange(detach));ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  auto discardedRedo=detach.takeDetachedTail();ASSERT_TRUE(discardedRedo);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  auto replacement=graph.makeEmptyNodeCandidate(editor,"Scene B reused ID");
  auto newRegistration=manager->prepareEditorSceneRootRegistration(graph,replacement,first);
  const auto receiptB=newRegistration.membership();EXPECT_EQ(receiptB.sceneIndex(),1u);
  auto replacementAppend=graph.prepareAppend(std::move(replacement));ASSERT_TRUE(replacementAppend);
  ASSERT_TRUE(graph.publishAppend(replacementAppend));
  ASSERT_TRUE(manager->publishEditorSceneRootChange(newRegistration));
  discardedRedo.reset();
  EXPECT_THROW((void)manager->prepareEditorSceneRootChange(graph,receiptA,true),std::invalid_argument);
  EXPECT_EQ(manager->planSceneSelection(graph,1).runtimeRoots,(std::vector<uint32_t>{first}));
  const auto rootsA=manager->planSceneSelection(graph,0).runtimeRoots;
  EXPECT_EQ(std::ranges::find(rootsA,first),rootsA.end());
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
  EXPECT_FALSE(graph.isNodeActive(first));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  EXPECT_TRUE(graph.isNodeActive(first));EXPECT_EQ(graph.getNode(first)->name,"Scene B reused ID");
}

TEST_F(SceneMergeGenerationGpu,EmptyMembershipReceiptSurvivesMergeThenUndoOfTheSameCatalog) {
  load();const auto first=static_cast<uint32_t>(graph.nodeCount());
  const auto beforeEmptyView=graph.activeRoots();
  auto candidate=graph.makeEmptyNodeCandidate(editor,"Retained empty");
  auto registration=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  const auto receipt=registration.membership();
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(registration));
  const auto preMergeView=graph.activeRoots();const auto mergeFirst=static_cast<uint32_t>(graph.nodeCount());
  auto staleBeforeMerge=manager->prepareEditorSceneRootChange(graph,receipt,false);
  auto generation=manager->prepareMergeGeneration(triangle("B",89),temp/"b.glb",{},graph);
  auto mergeAppend=graph.prepareAppend(generation.graph());ASSERT_TRUE(mergeAppend);
  ASSERT_TRUE(graph.publishAppend(mergeAppend));exchangeMerge(generation);
  EXPECT_FALSE(manager->canPublishEditorSceneRootChange(staleBeforeMerge));
  auto undoMerge=graph.prepareDetachTail(mergeFirst,preMergeView);ASSERT_TRUE(undoMerge);
  ASSERT_TRUE(graph.publishTailChange(undoMerge));exchangeMerge(generation);
  // The command receipt belongs to the restored catalog, not one particular
  // asset-generation epoch; fresh replay remains possible after merge undo.
  auto remove=manager->prepareEditorSceneRootChange(graph,receipt,false);
  auto detach=graph.prepareDetachTail(first,beforeEmptyView);ASSERT_TRUE(detach);
  ASSERT_TRUE(graph.publishTailChange(detach));ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  EXPECT_EQ(graph.nodeCount(),first);
  EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*beforeEmptyView);
}

TEST_F(SceneMergeGenerationGpu,MembershipRejectsCatalogResetAndManagerReconstructionAtTheSameAddress) {
  load();const auto first=static_cast<uint32_t>(graph.nodeCount());
  auto candidate=graph.makeEmptyNodeCandidate(editor,"Unpublished empty");
  auto token=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  const auto receipt=token.membership();
  SceneGraph replacement;
  manager->populateSceneGraph(replacement);
  EXPECT_FALSE(manager->canPublishEditorSceneRootChange(token));
  EXPECT_THROW((void)manager->prepareEditorSceneRootChange(graph,receipt,true),std::invalid_argument);
  // Keep the original graph alive and reconstruct the owner in exactly the
  // same allocation. Raw owner pointers and freshly matching counters alone
  // must not validate a command from the previous manager lifetime.
  auto* address=manager.release();
  std::destroy_at(address);
  std::construct_at(address,*allocation,*pipelines,device,container::app::AppConfig{});
  manager.reset(address);
  manager->initialize((temp/"a.glb").string(),2.0f,2);
  SceneGraph unrelated;
  manager->populateSceneGraph(unrelated);
  EXPECT_FALSE(manager->canPublishEditorSceneRootChange(token));
  EXPECT_THROW((void)manager->prepareEditorSceneRootChange(graph,receipt,true),std::invalid_argument);
  EXPECT_EQ(graph.nodeCount(),first);EXPECT_EQ(graph.getNode(editor)->name,"Scene Root");
}

TEST_F(SceneMergeGenerationGpu,RetiredSourceSlotsLeaveBoundsLightsAndVariantsButKeepDenseOwnershipForUndo) {
  load(false,true,true);ASSERT_TRUE(manager->modelBounds().valid);
  ASSERT_EQ(manager->evaluateAuthoredSceneLights(graph,0,true).points.size(),1u);
  uint32_t owner=SceneGraph::kInvalidNode;
  for(uint32_t id=0;id<graph.nodeCount();++id) if(graph.getNode(id)->sourceNodeIndex==0) owner=id;
  ASSERT_NE(owner,SceneGraph::kInvalidNode);
  const auto source=manager->animationSource();const auto mappings=manager->sourcePrimitiveToRuntime();
  const auto count=graph.nodeCount();const auto oldVariant=manager->planMaterialVariant(graph,std::nullopt);
  ASSERT_FALSE(oldVariant.assignments.empty());
  auto prepared=graph.prepareRetireSubtree(owner);ASSERT_TRUE(prepared);
  ASSERT_TRUE(manager->evaluateAuthoredSceneLights(*prepared.candidateGraph(),0,true).points.empty());
  ASSERT_TRUE(graph.publishRetirement(prepared));auto receipt=prepared.takeRetirement();ASSERT_TRUE(receipt);
  manager->refreshActiveScene(graph);
  EXPECT_FALSE(manager->modelBounds().valid);EXPECT_TRUE(manager->authoredPointLights().empty());
  EXPECT_TRUE(manager->evaluateAuthoredSceneLights(graph,0,true).points.empty());
  EXPECT_TRUE(manager->planMaterialVariant(graph,std::nullopt).assignments.empty());
  EXPECT_FALSE(manager->applyMaterialVariant(graph,oldVariant));
  EXPECT_EQ(graph.nodeCount(),count);EXPECT_EQ(manager->sourcePrimitiveToRuntime(),mappings);
  EXPECT_EQ(manager->animationSource(),source);EXPECT_FALSE(manager->retiredSceneExportNodes(graph).empty());
  const auto raw=manager->buildSceneExport(graph);EXPECT_GE(raw.nodes.size(),graph.nodeCount());
  EXPECT_EQ(raw.nodes[owner].name,graph.getNode(owner)->name);
  auto undo=graph.prepareRetirementChange(*receipt,false);ASSERT_TRUE(undo);
  ASSERT_TRUE(graph.publishRetirement(undo));manager->refreshActiveScene(graph);
  EXPECT_TRUE(manager->modelBounds().valid);EXPECT_EQ(manager->authoredPointLights().size(),1u);
  EXPECT_TRUE(manager->retiredSceneExportNodes(graph).empty());
  EXPECT_EQ(manager->planMaterialVariant(graph,std::nullopt).assignments.size(),oldVariant.assignments.size());
  auto redo=graph.prepareRetirementChange(*receipt,true);ASSERT_TRUE(redo);
  ASSERT_TRUE(graph.publishRetirement(redo));manager->refreshActiveScene(graph);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
  EXPECT_TRUE(manager->planSceneSelection(graph,0).runtimeRoots.empty());
  EXPECT_FALSE(manager->modelBounds().valid);EXPECT_TRUE(manager->authoredPointLights().empty());
  EXPECT_EQ(manager->sceneCatalog().entries[0].sourceRoots,(std::vector<int32_t>{0}));
}

TEST_F(SceneMergeGenerationGpu,DirectSaveProjectsRetiredProceduralNodesWithoutTouchingSourceOrSurvivingGeometry) {
  load();const auto source=manager->animationSource();
  const auto manual=manager->createSolidMaterial({.7f,.1f,.3f,1});
  const auto primitive=manager->appendRuntimeMesh(Model::MakeCube().meshes().front());
  const auto object=graph.createNode(glm::translate(glm::mat4(1),{20,0,0}),manual,true,primitive,"Retired manual cube");
  ASSERT_TRUE(graph.setParent(object,editor));manager->registerActiveSceneNode(graph,object);
  const auto child=graph.createNode(glm::mat4(1),manual,false,SceneGraph::kInvalidNode,"Retired manual label");
  ASSERT_TRUE(graph.setParent(child,object));graph.updateWorldTransforms();
  auto prepared=graph.prepareRetireSubtree(object);ASSERT_TRUE(prepared);ASSERT_TRUE(graph.publishRetirement(prepared));
  manager->refreshActiveScene(graph);
  EXPECT_LT(manager->modelBounds().max.x,3.0f);
  EXPECT_EQ(manager->retiredSceneExportNodes(graph),(std::vector<int32_t>{int32_t(object),int32_t(child)}));
  const auto raw=manager->buildSceneExport(graph);EXPECT_EQ(raw.nodes[object].name,"Retired manual cube");
  for(const auto* suffix:{".glb",".gltf"}) {
    const auto path=temp/(std::string("retired-manual")+suffix);
    manager->saveScene(graph,path);
    const auto loaded=container::geometry::gltf::LoadModelWithSource(path.string()).gltfModel;
    EXPECT_TRUE(std::ranges::none_of(loaded.nodes,[](const auto& node) { return node.name.starts_with("Retired manual"); }));
    EXPECT_EQ(loaded.nodes.size(),raw.nodes.size()-2);
  }
  EXPECT_EQ(manager->animationSource(),source);EXPECT_EQ(manager->runtimePrimitiveToSource(),(std::vector<int32_t>{0,-1}));
}

TEST_F(SceneMergeGenerationGpu,CloneKeepsImportedProceduralEditsAndTextureIdentitiesWithoutNewImages) {
  load();const auto owner=sourceNode(0);ASSERT_NE(owner,SceneGraph::kInvalidNode);
  const auto imported=primitiveNode(owner);ASSERT_NE(imported,SceneGraph::kInvalidNode);
  const auto manual=manager->createSolidMaterial({.2f,.7f,.4f,1});
  const auto geometry=manager->appendRuntimeMesh(Model::MakeCube().meshes().front());
  const auto empty=graph.createNode(glm::translate(glm::mat4(1),{0,3,0}),manual,false,SceneGraph::kInvalidNode,"Edited empty");
  ASSERT_TRUE(graph.setParent(empty,owner));graph.setSelectable(empty,false);
  const auto cube=graph.createNode(glm::scale(glm::mat4(1),{1,2,1}),manual,true,geometry,"Edited procedural cube");
  ASSERT_TRUE(graph.setParent(cube,empty));graph.setHoverable(cube,false);graph.updateWorldTransforms();
  const auto texture=manager->resolveMaterialTextureIndex(graph.getNode(imported)->materialIndex);
  const auto original=manager->buildSceneExport(graph);const auto allocationsBefore=allocations();
  const auto originalDescriptor=manager->descriptorSet(0);const auto first=uint32_t(graph.nodeCount());
  {
    auto clone=manager->prepareSubtreeCloneGeneration(graph,owner,"Imported and manual copy");
    ASSERT_TRUE(clone.readyForExchange(*manager,graph));EXPECT_TRUE(clone.isSubtreeClone());
    EXPECT_EQ(allocations(),allocationsBefore+3); // Material, metadata and clip buffers; no image allocations.
    EXPECT_NE(clone.manager().descriptorSet(0),originalDescriptor);
    EXPECT_EQ(manager->buildSceneExport(graph),original);EXPECT_EQ(graph.nodeCount(),first);
    EXPECT_EQ(clone.manager().runtimePrimitiveToSource(),manager->runtimePrimitiveToSource());
    EXPECT_EQ(clone.manager().model().meshes().size(),manager->model().meshes().size());
    EXPECT_EQ(clone.manager().indices(),manager->indices());EXPECT_EQ(clone.manager().materialCount(),manager->materialCount());
    EXPECT_EQ(clone.manager().animationSource().images,manager->animationSource().images);
    EXPECT_EQ(clone.graph().getNode(clone.cloneRoot())->name,"Imported and manual copy");
    EXPECT_EQ(clone.graph().renderableNodes().size(),4u);
    ASSERT_EQ(clone.cloneOriginalGraphNodes().size(),4u); // owner, primitive, empty, cube
    for(size_t n=0;n<clone.cloneOriginalGraphNodes().size();++n) {
      const auto old=clone.cloneOriginalGraphNodes()[n];const auto* copied=clone.graph().getNode(first+uint32_t(n));
      EXPECT_EQ(copied->worldTransform,graph.getNode(old)->worldTransform);
      EXPECT_EQ(copied->primitiveIndex,graph.getNode(old)->primitiveIndex);
      EXPECT_EQ(copied->materialIndex,graph.getNode(old)->materialIndex);
      EXPECT_EQ(copied->selectable,graph.getNode(old)->selectable);EXPECT_EQ(copied->hoverable,graph.getNode(old)->hoverable);
      if(old==imported) EXPECT_EQ(clone.manager().resolveMaterialTextureIndex(copied->materialIndex),texture);
    }
    EXPECT_NO_THROW(clone.cloneOwnership().validate(manager->animationSource(),clone.projectedClonePlan()));
  }
  EXPECT_EQ(allocations(),allocationsBefore);EXPECT_EQ(manager->descriptorSet(0),originalDescriptor);
}

TEST_F(SceneMergeGenerationGpu,GraphOnlyClonePreservesSourceCountAndCopiesProceduralGeometryByIdentity) {
  load();const auto owner=sourceNode(0);const auto geometry=manager->appendRuntimeMesh(Model::MakeCube().meshes().front());
  const auto empty=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Graph-only root");
  ASSERT_TRUE(graph.setParent(empty,owner));
  const auto cube=graph.createNode(glm::translate(glm::mat4(1),{4,0,0}),0,true,geometry,"Graph-only cube");
  ASSERT_TRUE(graph.setParent(cube,empty));graph.updateWorldTransforms();
  auto clone=manager->prepareSubtreeCloneGeneration(graph,empty,"Graph-only copy");
  EXPECT_EQ(clone.manager().animationSource(),manager->animationSource());
  EXPECT_EQ(clone.projectedClonePlan().clone.clonedNodes,0u);
  EXPECT_EQ(clone.manager().model().meshes().size(),manager->model().meshes().size());
  EXPECT_EQ(clone.graph().getNode(clone.cloneRoot())->sourceNodeIndex,-1);
  EXPECT_EQ(clone.graph().renderableNodes().size(),3u);
  ASSERT_EQ(clone.cloneOriginalGraphNodes().size(),2u);
  const auto copy=uint32_t(graph.nodeCount())+1;
  EXPECT_EQ(clone.graph().getNode(copy)->primitiveIndex,geometry);
  EXPECT_EQ(clone.graph().getNode(copy)->meshOwnerGraphNode,SceneGraph::kInvalidNode);
  EXPECT_EQ(clone.manager().planSceneSelection(clone.graph(),0).runtimeRoots,*graph.activeRoots());
}

TEST_F(SceneMergeGenerationGpu,MovedPrimitiveCloneKeepsExternalOwnerBindingButDoesNotInventDrawsForTheVisualOwner) {
  load(false,false,true);const auto owner=sourceNode(0),primitive=primitiveNode(owner);
  const auto empty=graph.createNode(glm::translate(glm::mat4(1),{3,0,0}),0,false,SceneGraph::kInvalidNode,"Moved primitive owner");
  ASSERT_TRUE(graph.setParent(empty,editor));ASSERT_TRUE(graph.setParent(primitive,empty));
  manager->registerActiveSceneNode(graph,empty);graph.updateWorldTransforms();
  {
    auto clone=manager->prepareSubtreeCloneGeneration(graph,empty,"Moved primitive copy");
    const auto& source=clone.projectedClonePlan();EXPECT_EQ(source.roles[0],container::geometry::gltf::GltfCloneNodeRole::MeshBindingOnly);
    EXPECT_TRUE(source.retainMeshBindings[0]);EXPECT_FALSE(source.clone.contextOnly[0]);
    const auto newSource=source.clone.nodes[0];ASSERT_GE(newSource,0);
    const auto& node=clone.manager().animationSource().nodes[size_t(newSource)];EXPECT_EQ(node.mesh,0);EXPECT_EQ(node.light,-1);
    EXPECT_EQ(clone.graph().renderableNodes().size(),2u);EXPECT_EQ(clone.manager().authoredPointLights().size(),1u);
    EXPECT_EQ(clone.cloneOriginalGraphNodes().size(),3u); // actual empty+primitive and the external owner context
  }
  {
    auto clone=manager->prepareSubtreeCloneGeneration(graph,owner,"Empty source owner copy");
    const auto& source=clone.projectedClonePlan();EXPECT_EQ(source.roles[0],container::geometry::gltf::GltfCloneNodeRole::Visual);
    EXPECT_FALSE(source.retainMeshBindings[0]);EXPECT_FALSE(source.copyWeightTracks[0]);
    const auto newSource=source.clone.nodes[0];ASSERT_GE(newSource,0);
    const auto& node=clone.manager().animationSource().nodes[size_t(newSource)];EXPECT_EQ(node.mesh,-1);EXPECT_EQ(node.skin,-1);
    EXPECT_EQ(node.light,0);EXPECT_EQ(clone.graph().renderableNodes().size(),1u);
    EXPECT_EQ(clone.manager().authoredPointLights().size(),2u);
  }
}

TEST_F(SceneMergeGenerationGpu,CloneRetainsMorphRestSkinFramesVariantAppearanceAndInstancingAccessors) {
  loadSource(cloneAnimatedAsset());const auto owner=sourceNode(0),instances=sourceNode(2);
  const auto group=graph.createNode(glm::translate(glm::mat4(1),{3,0,0}),0,false,SceneGraph::kInvalidNode,"Edited animated group");
  ASSERT_TRUE(graph.setParent(group,editor));ASSERT_TRUE(graph.setParent(owner,group));ASSERT_TRUE(graph.setParent(instances,group));
  manager->registerActiveSceneNode(graph,group);graph.updateWorldTransforms();
  const auto variant=manager->planMaterialVariant(graph,0);ASSERT_TRUE(manager->applyMaterialVariant(graph,variant));
  ASSERT_NE(manager->deformationData(),nullptr);const auto oldDefault=manager->deformationData()->nodes[0].defaultMorphWeights;
  const auto oldBind=manager->deformationData()->primitives[0].bindVertices;
  manager->animationSource().nodes[0].weights={.9};
  auto clone=manager->prepareSubtreeCloneGeneration(graph,group,"Animated edited copy");
  const auto& plan=clone.projectedClonePlan();ASSERT_NE(clone.manager().deformationData(),nullptr);
  const auto& data=*clone.manager().deformationData();EXPECT_EQ(data.primitives.size(),1u);
  ASSERT_EQ(data.primitives[0].bindVertices.size(),oldBind.size());
  EXPECT_EQ(std::memcmp(data.primitives[0].bindVertices.data(),oldBind.data(),oldBind.size()*sizeof(Vertex)),0);
  EXPECT_EQ(data.nodes[size_t(plan.clone.nodes[0])].defaultMorphWeights,oldDefault);
  EXPECT_EQ(data.skins.size(),2u);EXPECT_EQ(data.skins[1].joints,(std::vector<uint32_t>{uint32_t(plan.clone.nodes[1])}));
  EXPECT_EQ(data.skins[1].inverseBindMatrices,manager->deformationData()->skins[0].inverseBindMatrices);
  EXPECT_EQ(plan.clone.evaluatedSource.nodes[size_t(plan.clone.nodes[0])].weights,(std::vector<double>{.9}));
  EXPECT_EQ(plan.clone.authoredSource.nodes[size_t(plan.clone.nodes[0])].weights,(std::vector<double>{.4}));
  EXPECT_EQ(clone.manager().selectedMaterialVariant(),manager->selectedMaterialVariant());
  EXPECT_EQ(clone.manager().instancingData().nodes.size(),2u);
  const auto* original=container::geometry::gltf::FindNodeInstancing(manager->instancingData(),2);
  const auto* copied=container::geometry::gltf::FindNodeInstancing(clone.manager().instancingData(),uint32_t(plan.clone.nodes[2]));
  ASSERT_NE(original,nullptr);ASSERT_NE(copied,nullptr);EXPECT_EQ(copied->localMatrices,original->localMatrices);
  ASSERT_EQ(copied->attributes.size(),original->attributes.size());
  for(size_t i=0;i<copied->attributes.size();++i) EXPECT_EQ(copied->attributes[i].sourceAccessor,original->attributes[i].sourceAccessor);
  EXPECT_EQ(clone.graph().renderableNodes().size(),6u);EXPECT_EQ(clone.manager().model().meshes().size(),1u);
  EXPECT_EQ(clone.manager().animationSource().accessors.size(),manager->animationSource().accessors.size());
  const auto reset=clone.manager().planMaterialVariant(clone.graph(),std::nullopt);
  ASSERT_TRUE(clone.manager().applyMaterialVariant(clone.graph(),reset));
  for(const auto& assignment:reset.assignments) EXPECT_EQ(clone.graph().getNode(assignment.node)->materialIndex,assignment.after);
}

TEST_F(SceneMergeGenerationGpu,SharedSceneCloneMembershipAndResourceAliasesSurviveExchangeUndoRedoAndRelease) {
  auto initial=triangle("Shared source",89);initial.gltfModel.scenes.push_back(initial.gltfModel.scenes[0]);
  initial.gltfModel.scenes.emplace_back();loadSource(std::move(initial));
  const auto owner=sourceNode(0);const auto geometry=manager->appendRuntimeMesh(Model::MakeCube().meshes().front());
  const auto empty=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Shared parent empty");
  ASSERT_TRUE(graph.setParent(empty,owner));const auto cube=graph.createNode(glm::mat4(1),0,true,geometry,"Shared manual cube");
  ASSERT_TRUE(graph.setParent(cube,empty));graph.updateWorldTransforms();
  const auto oldSource=manager->animationSource();const auto oldView=graph.activeRoots();const auto first=uint32_t(graph.nodeCount());
  auto clone=manager->prepareSubtreeCloneGeneration(graph,empty,"Shared copy");const auto root=clone.cloneRoot();
  EXPECT_EQ(clone.projectedClonePlan().clone.affectedScenes,(std::vector<size_t>{0,1}));
  const auto roots0=clone.manager().planSceneSelection(clone.graph(),0).runtimeRoots;
  const auto roots1=clone.manager().planSceneSelection(clone.graph(),1).runtimeRoots;EXPECT_EQ(roots0,roots1);
  EXPECT_TRUE(clone.manager().planSceneSelection(clone.graph(),2).runtimeRoots.empty());
  const auto descriptorA=manager->descriptorSet(0),descriptorB=clone.manager().descriptorSet(0);
  auto append=graph.prepareAppend(clone.graph());ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));
  exchangeMerge(clone);EXPECT_TRUE(graph.isNodeActive(root));
  EXPECT_EQ(manager->descriptorSet(0),descriptorB);EXPECT_EQ(manager->animationSource(),oldSource);
  const auto owned=allocations();clone.releasePreparationData();EXPECT_TRUE(clone.cloneOriginalGraphNodes().empty());
  EXPECT_FALSE(clone.cloneOwnership());EXPECT_FALSE(clone.readyForExchange(*manager,graph));
  auto detach=graph.prepareDetachTail(first,oldView);ASSERT_TRUE(detach);ASSERT_TRUE(graph.publishTailChange(detach));
  auto tail=detach.takeDetachedTail();ASSERT_TRUE(tail);exchangeMerge(clone);
  EXPECT_EQ(manager->descriptorSet(0),descriptorA);EXPECT_EQ(graph.nodeCount(),first);EXPECT_EQ(allocations(),owned);
  auto restore=graph.prepareRestoreTail(*tail);ASSERT_TRUE(restore);ASSERT_TRUE(graph.publishTailChange(restore));
  exchangeMerge(clone);EXPECT_TRUE(graph.isNodeActive(root));EXPECT_EQ(allocations(),owned);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));EXPECT_TRUE(graph.isNodeActive(root));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,2));EXPECT_FALSE(graph.isNodeActive(root));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));EXPECT_TRUE(graph.isNodeActive(root));
}

TEST_F(SceneMergeGenerationGpu,CloneBudgetAndStaleProofFailuresLeaveLiveResourcesUnchanged) {
  auto source=triangle("Shared scene proof",61);source.gltfModel.scenes.push_back(source.gltfModel.scenes[0]);
  loadSource(std::move(source));const auto owner=sourceNode(0);const auto before=manager->buildSceneExport(graph);
  const auto count=graph.nodeCount();const auto revision=graph.revision();const auto allocated=allocations();
  const auto descriptor=manager->descriptorSet(0);
  SceneMergeGenerationLimits limits;limits.maxPreparedCpuBytes=1;
  EXPECT_THROW((void)manager->prepareSubtreeCloneGeneration(graph,owner,{},limits),std::runtime_error);
  limits={};limits.graph.maxActiveRenderableObjects=1;
  EXPECT_THROW((void)manager->prepareSubtreeCloneGeneration(graph,owner,{},limits),std::runtime_error);
  limits={};limits.geometry.maxOutputBytes=1;
  EXPECT_THROW((void)manager->prepareSubtreeCloneGeneration(graph,owner,{},limits),std::runtime_error);
  limits={};limits.maxMaterialBufferBytes=1;
  EXPECT_THROW((void)manager->prepareSubtreeCloneGeneration(graph,owner,{},limits),std::runtime_error);
  EXPECT_THROW((void)manager->prepareSubtreeCloneGeneration(graph,owner,std::string("\xc0\x80",2)),std::invalid_argument);
  EXPECT_EQ(allocations(),allocated);EXPECT_EQ(manager->descriptorSet(0),descriptor);
  EXPECT_EQ(graph.nodeCount(),count);EXPECT_EQ(graph.revision(),revision);EXPECT_EQ(manager->buildSceneExport(graph),before);
  {
    auto shared=manager->prepareSubtreeCloneGeneration(graph,owner,"Stale shared-scene copy");
    const auto graphRevision=graph.revision();manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
    EXPECT_EQ(graph.revision(),graphRevision);EXPECT_FALSE(shared.readyForExchange(*manager,graph));
    manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
    EXPECT_FALSE(shared.readyForExchange(*manager,graph));
  }
  auto clone=manager->prepareSubtreeCloneGeneration(graph,owner,"Pending copy");
  EXPECT_TRUE(clone.readyForExchange(*manager,graph));graph.setVisible(owner,false);
  EXPECT_FALSE(clone.readyForExchange(*manager,graph));
  EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(graph.nodeCount(),count);
}

TEST_F(SceneMergeGenerationGpu,PrimitiveMembershipOwnsInheritedAndContextEntriesWithExactViewUndoRedo) {
  load();const auto owner=sourceNode(0);
  for(const bool inherited:{true,false}) {
    const auto oldView=graph.activeRoots();const auto first=uint32_t(graph.nodeCount());
    const auto originalOrder=manager->planSceneSelection(graph,0).runtimeRoots;
    auto candidate=primitiveCandidate(inherited?owner:editor);
    auto registration=manager->prepareEditorPrimitiveSceneRootRegistration(graph,candidate,first);
    const auto receipt=registration.membership();EXPECT_TRUE(receipt.requiresExplicitRoot());
    EXPECT_EQ(std::vector<uint32_t>(registration.candidateRoots().begin(),registration.candidateRoots().end()),
        (std::vector<uint32_t>{first}));
    if(inherited) EXPECT_EQ(candidate.activeRoots(),oldView);
    else {ASSERT_TRUE(candidate.activeRoots());EXPECT_EQ(candidate.activeRoots()->back(),first);}
    auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
    ASSERT_TRUE(manager->canPublishEditorSceneRootChange(registration));ASSERT_TRUE(graph.publishAppend(append));
    ASSERT_TRUE(manager->publishEditorSceneRootChange(registration));
    EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*graph.activeRoots());
    auto remove=manager->prepareEditorSceneRootChange(graph,receipt,false);
    auto detach=graph.prepareDetachTail(first,oldView);ASSERT_TRUE(detach);
    ASSERT_TRUE(manager->canPublishEditorSceneRootChange(remove));ASSERT_TRUE(graph.publishTailChange(detach));
    ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));auto tail=detach.takeDetachedTail();ASSERT_TRUE(tail);
    EXPECT_EQ(graph.activeRoots(),oldView);EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,originalOrder);
    auto restore=manager->prepareEditorSceneRootChange(graph,receipt,true);
    auto attach=graph.prepareRestoreTail(*tail);ASSERT_TRUE(attach);
    ASSERT_TRUE(graph.publishTailChange(attach));ASSERT_TRUE(manager->publishEditorSceneRootChange(restore));
    EXPECT_TRUE(graph.getNode(first)->renderable);EXPECT_EQ(graph.getNode(first)->primitiveIndex,0u);
    EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*graph.activeRoots());
    remove=manager->prepareEditorSceneRootChange(graph,receipt,false);
    detach=graph.prepareDetachTail(first,oldView);ASSERT_TRUE(detach);
    ASSERT_TRUE(graph.publishTailChange(detach));ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  }
}

TEST_F(SceneMergeGenerationGpu,UndonePrimitiveRegistrationCannotAliasAnEmptyBranchInAnotherScene) {
  load(false,true);const auto first=uint32_t(graph.nodeCount());const auto oldView=graph.activeRoots();
  auto candidate=primitiveCandidate(editor);
  auto registration=manager->prepareEditorPrimitiveSceneRootRegistration(graph,candidate,first);
  const auto receipt=registration.membership();auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(registration));
  auto remove=manager->prepareEditorSceneRootChange(graph,receipt,false);
  auto detach=graph.prepareDetachTail(first,oldView);ASSERT_TRUE(detach);
  ASSERT_TRUE(graph.publishTailChange(detach));ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  auto discarded=detach.takeDetachedTail();ASSERT_TRUE(discarded);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  candidate=graph.makeEmptyNodeCandidate(editor,"Empty branch reuses primitive ID");
  auto empty=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(empty));discarded.reset();
  EXPECT_THROW((void)manager->prepareEditorSceneRootChange(graph,receipt,true),std::invalid_argument);
  EXPECT_EQ(manager->planSceneSelection(graph,1).runtimeRoots,(std::vector<uint32_t>{first}));
  const auto roots=manager->planSceneSelection(graph,0).runtimeRoots;
  EXPECT_EQ(std::ranges::find(roots,first),roots.end());
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));EXPECT_FALSE(graph.isNodeActive(first));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));EXPECT_TRUE(graph.isNodeActive(first));
  EXPECT_FALSE(graph.getNode(first)->renderable);
}

TEST_F(SceneMergeGenerationGpu,PrimitiveBelowSharedParentInheritsBothScenesWithoutRedundantActiveRoots) {
  auto initial=triangle("Shared primitive parent",131);initial.gltfModel.scenes.push_back(initial.gltfModel.scenes[0]);
  loadSource(std::move(initial));const auto parent=sourceNode(0);const auto first=uint32_t(graph.nodeCount());
  const auto oldView=graph.activeRoots();auto candidate=primitiveCandidate(parent);
  auto roots=manager->prepareEditorPrimitiveSceneRootRegistration(graph,candidate,first);
  EXPECT_TRUE(roots.membership().requiresExplicitRoot());EXPECT_EQ(candidate.activeRoots(),oldView);
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(roots));
  EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*oldView);
  EXPECT_EQ(manager->planSceneSelection(graph,1).runtimeRoots,*oldView);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));EXPECT_TRUE(graph.isNodeActive(first));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));EXPECT_TRUE(graph.isNodeActive(first));
}

TEST_F(SceneMergeGenerationGpu,PrimitiveRegistrationRejectsMalformedLeavesBindingsAndViewsBeforePublication) {
  load(false,true);const auto first=uint32_t(graph.nodeCount());const auto owner=sourceNode(0);
  const auto allocated=allocations();const auto revision=graph.revision();const auto before=manager->buildSceneExport(graph);
  const auto expect=[&](const SceneGraph& candidate) {
    EXPECT_THROW((void)manager->prepareEditorPrimitiveSceneRootRegistration(graph,candidate,first),std::invalid_argument);
  };
  expect(graph.makeEmptyNodeCandidate(editor,"Not a primitive"));
  for(const int defect:{0,1,2,3,4,5,6,7,8,9,10}) {
    auto candidate=primitiveCandidate(owner);auto* node=candidate.getNode(first);
    if(defect==0) node->sourceNodeIndex=0;
    if(defect==1) node->sourceMeshIndex=0;
    if(defect==2) node->meshOwnerGraphNode=owner;
    if(defect==3) node->primitiveIndex=uint32_t(manager->model().meshes().size());
    if(defect==4) node->materialIndex=UINT32_MAX;
    if(defect==5) node->editorSceneRoot=true;
    if(defect==6) node->interactionRootBoundary=true;
    if(defect==7) node->name=std::string(4097,'x');
    if(defect==8) node->children={owner};
    if(defect==9) {auto view=*candidate.activeRoots();view.push_back(first);candidate.setActiveRoots(view);}
    if(defect==10) node->worldTransform[0][0]=std::numeric_limits<float>::infinity();
    expect(candidate);
  }
  for(uint32_t id=0;id<graph.nodeCount();++id) if(graph.getNode(id)->interactionRootBoundary) expect(primitiveCandidate(id));
  expect(primitiveCandidate(primitiveNode(owner))); // Imported synthetic selection must resolve to its authored group first.
  EXPECT_EQ(graph.nodeCount(),first);EXPECT_EQ(graph.revision(),revision);
  EXPECT_EQ(allocations(),allocated);EXPECT_EQ(manager->buildSceneExport(graph),before);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));expect(primitiveCandidate(owner));
  EXPECT_EQ(graph.nodeCount(),first);EXPECT_TRUE(graph.activeRoots()->empty());
}

TEST_F(SceneMergeGenerationGpu,PrimitiveRegistrationRejectsStaleForeignAndConsumedPreparedAuthority) {
  load(false,true);const auto first=uint32_t(graph.nodeCount());auto candidate=primitiveCandidate(editor);
  auto token=manager->prepareEditorPrimitiveSceneRootRegistration(graph,candidate,first);
  auto foreign=manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"Foreign manager proof");
  EXPECT_FALSE(foreign.manager().canPublishEditorSceneRootChange(token));
  EXPECT_THROW((void)foreign.manager().prepareEditorSceneRootChange(graph,token.membership(),true),std::invalid_argument);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  EXPECT_FALSE(manager->canPublishEditorSceneRootChange(token));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
  EXPECT_FALSE(manager->canPublishEditorSceneRootChange(token));
  token=manager->prepareEditorPrimitiveSceneRootRegistration(graph,candidate,first);
  const auto receipt=token.membership();SceneGraph unrelated=graph;
  EXPECT_THROW((void)manager->prepareEditorSceneRootChange(unrelated,receipt,true),std::invalid_argument);
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));
  ASSERT_TRUE(manager->canPublishEditorSceneRootChange(token));ASSERT_TRUE(manager->publishEditorSceneRootChange(token));
  EXPECT_FALSE(token);EXPECT_FALSE(manager->publishEditorSceneRootChange(token));
}

TEST_F(SceneMergeGenerationGpu,PrimitiveAndEarlierEmptyReceiptsRemainExactAfterCloneAndUndo) {
  load();const auto emptyId=uint32_t(graph.nodeCount());const auto beforeEmpty=graph.activeRoots();
  auto candidate=graph.makeEmptyNodeCandidate(editor,"Earlier empty");
  auto empty=manager->prepareEditorSceneRootRegistration(graph,candidate,emptyId);const auto emptyReceipt=empty.membership();
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(empty));
  const auto primitiveId=uint32_t(graph.nodeCount());const auto beforePrimitive=graph.activeRoots();
  candidate=primitiveCandidate(emptyId);
  auto primitive=manager->prepareEditorPrimitiveSceneRootRegistration(graph,candidate,primitiveId);
  const auto primitiveReceipt=primitive.membership();append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(primitive));
  const auto cloneFirst=uint32_t(graph.nodeCount());const auto beforeClone=graph.activeRoots();
  auto clone=manager->prepareSubtreeCloneGeneration(graph,emptyId,"Copy with owned primitive");
  append=graph.prepareAppend(clone.graph());ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));
  exchangeMerge(clone);
  auto detachClone=graph.prepareDetachTail(cloneFirst,beforeClone);ASSERT_TRUE(detachClone);
  ASSERT_TRUE(graph.publishTailChange(detachClone));exchangeMerge(clone);
  auto removePrimitive=manager->prepareEditorSceneRootChange(graph,primitiveReceipt,false);
  auto detachPrimitive=graph.prepareDetachTail(primitiveId,beforePrimitive);ASSERT_TRUE(detachPrimitive);
  ASSERT_TRUE(graph.publishTailChange(detachPrimitive));ASSERT_TRUE(manager->publishEditorSceneRootChange(removePrimitive));
  auto primitiveTail=detachPrimitive.takeDetachedTail();ASSERT_TRUE(primitiveTail);
  auto removeEmpty=manager->prepareEditorSceneRootChange(graph,emptyReceipt,false);
  auto detachEmpty=graph.prepareDetachTail(emptyId,beforeEmpty);ASSERT_TRUE(detachEmpty);
  ASSERT_TRUE(graph.publishTailChange(detachEmpty));ASSERT_TRUE(manager->publishEditorSceneRootChange(removeEmpty));
  auto emptyTail=detachEmpty.takeDetachedTail();ASSERT_TRUE(emptyTail);
  EXPECT_EQ(graph.activeRoots(),beforeEmpty);
  auto restoreEmpty=manager->prepareEditorSceneRootChange(graph,emptyReceipt,true);
  auto attachEmpty=graph.prepareRestoreTail(*emptyTail);ASSERT_TRUE(attachEmpty);
  ASSERT_TRUE(graph.publishTailChange(attachEmpty));ASSERT_TRUE(manager->publishEditorSceneRootChange(restoreEmpty));
  auto restorePrimitive=manager->prepareEditorSceneRootChange(graph,primitiveReceipt,true);
  auto attachPrimitive=graph.prepareRestoreTail(*primitiveTail);ASSERT_TRUE(attachPrimitive);
  ASSERT_TRUE(graph.publishTailChange(attachPrimitive));ASSERT_TRUE(manager->publishEditorSceneRootChange(restorePrimitive));
  EXPECT_EQ(graph.activeRoots(),beforeClone);
  EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*beforeClone);
}
TEST_F(SceneMergeGenerationGpu,InheritedEmptyReparentUndoRestoresItsCreationReceiptAndExactViews) {
  load(false,true);const auto owner=sourceNode(0),first=uint32_t(graph.nodeCount());
  const auto beforeCreation=graph.activeRoots();const auto source=manager->animationSource();
  const auto originalAllocations=allocations();const auto descriptor=manager->descriptorSet(0);
  auto candidate=graph.makeEmptyNodeCandidate(owner,"Inherited empty before reparent");
  auto creation=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  const auto creationReceipt=creation.membership();ASSERT_FALSE(creationReceipt.requiresExplicitRoot());
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(creation));
  const auto oldLocal=graph.getNode(first)->localTransform,oldWorld=graph.getNode(first)->worldTransform;
  const auto oldSibling=*graph.siblingIndex(first),newSibling=graph.getNode(editor)->children.size();
  const auto membership=reparentMembership(first,editor,oldWorld,newSibling);
  EXPECT_EQ(membership.activeRoots(false),beforeCreation);
  ASSERT_TRUE(membership.activeRoots(true));EXPECT_EQ(membership.activeRoots(true)->back(),first);
  ASSERT_TRUE(publishReparent(membership,true,first,editor,oldWorld,newSibling));
  EXPECT_EQ(graph.getNode(first)->worldTransform,oldWorld);
  EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*membership.activeRoots(true));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));EXPECT_FALSE(graph.isNodeActive(first));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));EXPECT_TRUE(graph.isNodeActive(first));
  ASSERT_TRUE(publishReparent(membership,false,first,owner,oldLocal,oldSibling));
  EXPECT_EQ(graph.activeRoots(),beforeCreation);
  auto remove=manager->prepareEditorSceneRootChange(graph,creationReceipt,false);
  auto detach=graph.prepareDetachTail(first,beforeCreation);ASSERT_TRUE(detach);
  ASSERT_TRUE(graph.publishTailChange(detach));ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  auto tail=detach.takeDetachedTail();ASSERT_TRUE(tail);
  auto restore=manager->prepareEditorSceneRootChange(graph,creationReceipt,true);
  auto attach=graph.prepareRestoreTail(*tail);ASSERT_TRUE(attach);
  ASSERT_TRUE(graph.publishTailChange(attach));ASSERT_TRUE(manager->publishEditorSceneRootChange(restore));
  ASSERT_TRUE(publishReparent(membership,true,first,editor,oldWorld,newSibling));
  ASSERT_TRUE(publishReparent(membership,false,first,owner,oldLocal,oldSibling));
  EXPECT_EQ(graph.activeRoots(),beforeCreation);EXPECT_EQ(manager->animationSource(),source);
  EXPECT_EQ(allocations(),originalAllocations);EXPECT_EQ(manager->descriptorSet(0),descriptor);
}

TEST_F(SceneMergeGenerationGpu,ReparentPreservesExplicitPrimitiveRegistrationRatherThanOwningIt) {
  load();const auto owner=sourceNode(0),first=uint32_t(graph.nodeCount());const auto oldView=graph.activeRoots();
  auto candidate=primitiveCandidate(owner);
  auto creation=manager->prepareEditorPrimitiveSceneRootRegistration(graph,candidate,first);
  const auto creationReceipt=creation.membership();auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(creation));
  const auto oldLocal=graph.getNode(first)->localTransform,world=graph.getNode(first)->worldTransform;
  const auto oldSibling=*graph.siblingIndex(first),newSibling=graph.getNode(editor)->children.size();
  const auto membership=reparentMembership(first,editor,world,newSibling);
  ASSERT_TRUE(publishReparent(membership,true,first,editor,world,newSibling));
  ASSERT_TRUE(publishReparent(membership,false,first,owner,oldLocal,oldSibling));
  EXPECT_EQ(graph.activeRoots(),oldView);
  // Reparent undo must retain the entry owned by primitive creation. Its
  // creation receipt is still the sole authority that can remove it.
  auto remove=manager->prepareEditorSceneRootChange(graph,creationReceipt,false);
  auto detach=graph.prepareDetachTail(first,oldView);ASSERT_TRUE(detach);
  ASSERT_TRUE(graph.publishTailChange(detach));ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*oldView);
  auto replacement=graph.makeEmptyNodeCandidate(editor,"Reused primitive tail after reparent");
  EXPECT_NO_THROW((void)manager->prepareEditorSceneRootRegistration(graph,replacement,first));
}

TEST_F(SceneMergeGenerationGpu,ReparentMembershipRetainsRetiredRootPositionsWithoutResurrectingContent) {
  auto initial=triangle("Retired root",97);initial.gltfModel.nodes.push_back(initial.gltfModel.nodes[0]);
  initial.gltfModel.nodes[1].name="Surviving root";initial.gltfModel.scenes[0].nodes={0,1};
  loadSource(std::move(initial));const auto retired=sourceNode(0),owner=sourceNode(1),first=uint32_t(graph.nodeCount());
  auto candidate=graph.makeEmptyNodeCandidate(owner,"Live child beside retired root");
  auto creation=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(creation));
  auto retirement=graph.prepareRetireSubtree(retired);ASSERT_TRUE(retirement);ASSERT_TRUE(graph.publishRetirement(retirement));
  const auto exactBefore=graph.activeRoots();ASSERT_TRUE(exactBefore);ASSERT_EQ(exactBefore->front(),retired);
  const auto oldLocal=graph.getNode(first)->localTransform,world=graph.getNode(first)->worldTransform;
  const auto oldSibling=*graph.siblingIndex(first),newSibling=graph.getNode(editor)->children.size();
  const auto membership=reparentMembership(first,editor,world,newSibling);
  EXPECT_EQ(membership.activeRoots(false),exactBefore);ASSERT_TRUE(membership.activeRoots(true));
  EXPECT_EQ(membership.activeRoots(true)->front(),retired);EXPECT_EQ(membership.activeRoots(true)->back(),first);
  ASSERT_TRUE(publishReparent(membership,true,first,editor,world,newSibling));
  EXPECT_TRUE(graph.isNodeRetired(retired));EXPECT_FALSE(graph.isNodeActive(retired));
  EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,(std::vector<uint32_t>{owner,first}));
  ASSERT_TRUE(publishReparent(membership,false,first,owner,oldLocal,oldSibling));
  EXPECT_EQ(graph.activeRoots(),exactBefore);EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,(std::vector<uint32_t>{owner}));
}

TEST_F(SceneMergeGenerationGpu,ReparentReceiptsPreserveAbsentAllActiveViewsAndSharedSceneMembership) {
  auto initial=triangle("Shared source owner",101);initial.gltfModel.scenes.push_back(initial.gltfModel.scenes[0]);
  loadSource(std::move(initial));const auto owner=sourceNode(0),first=uint32_t(graph.nodeCount());
  auto candidate=graph.makeEmptyNodeCandidate(owner,"Shared inherited empty");
  auto creation=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(creation));
  const auto oldLocal=graph.getNode(first)->localTransform,world=graph.getNode(first)->worldTransform;
  const auto oldSibling=*graph.siblingIndex(first),newSibling=graph.getNode(editor)->children.size();
  auto membership=reparentMembership(first,editor,world,newSibling);
  ASSERT_TRUE(publishReparent(membership,true,first,editor,world,newSibling));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));EXPECT_FALSE(graph.isNodeActive(first));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));EXPECT_TRUE(graph.isNodeActive(first));
  ASSERT_TRUE(publishReparent(membership,false,first,owner,oldLocal,oldSibling));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));EXPECT_TRUE(graph.isNodeActive(first));
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
  graph.clearActiveRoots();membership=reparentMembership(first,editor,world,newSibling);
  EXPECT_FALSE(membership.activeRoots(false));EXPECT_FALSE(membership.activeRoots(true));
  ASSERT_TRUE(publishReparent(membership,true,first,editor,world,newSibling));EXPECT_FALSE(graph.activeRoots());
  ASSERT_TRUE(publishReparent(membership,false,first,owner,oldLocal,oldSibling));EXPECT_FALSE(graph.activeRoots());
  EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,(std::vector<uint32_t>{owner}));
}

TEST_F(SceneMergeGenerationGpu,ReparentPreparedProofsRejectStaleForeignConsumedAndChangedCatalogAuthorities) {
  load(false,true);const auto owner=sourceNode(0),first=uint32_t(graph.nodeCount());
  auto candidate=graph.makeEmptyNodeCandidate(owner,"Reparent proof target");
  auto creation=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(creation));
  const auto world=graph.getNode(first)->worldTransform;const auto sibling=graph.getNode(editor)->children.size();
  const auto membership=reparentMembership(first,editor,world,sibling);
  auto prepared=manager->prepareReparentSceneRootChange(graph,membership,true);
  auto foreign=manager->prepareSubtreeCloneGeneration(graph,first,"Foreign owner");
  EXPECT_FALSE(foreign.manager().canPublishReparentSceneRootChange(prepared));
  EXPECT_THROW((void)foreign.manager().prepareReparentSceneRootChange(graph,membership,true),std::invalid_argument);
  SceneGraph unrelated=graph;
  EXPECT_THROW((void)manager->prepareReparentSceneRootChange(unrelated,membership,true),std::invalid_argument);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));
  EXPECT_FALSE(manager->canPublishReparentSceneRootChange(prepared));
  EXPECT_THROW((void)manager->prepareReparentSceneRootChange(graph,membership,true),std::invalid_argument);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
  EXPECT_FALSE(manager->canPublishReparentSceneRootChange(prepared));
  prepared=manager->prepareReparentSceneRootChange(graph,membership,true);
  auto placement=graph.preparePlacement(first,editor,world,sibling,membership.activeRoots(true));ASSERT_TRUE(placement);
  ASSERT_TRUE(graph.publishPlacement(placement));ASSERT_TRUE(manager->canPublishReparentSceneRootChange(prepared));
  ASSERT_TRUE(manager->publishReparentSceneRootChange(prepared));EXPECT_FALSE(prepared);
  EXPECT_FALSE(manager->publishReparentSceneRootChange(prepared));
  EXPECT_THROW((void)manager->prepareReparentSceneRootChange(graph,membership,true),std::invalid_argument);
  // Resetting the catalog on the same live manager invalidates persistent
  // receipts, even when the asset source-node count is unchanged.
  manager->populateSceneGraph(graph);
  EXPECT_THROW((void)manager->prepareReparentSceneRootChange(graph,membership,false),std::invalid_argument);
}

TEST_F(SceneMergeGenerationGpu,ReparentAndEarlierCreationReceiptsSurviveCloneGenerationExchangeAndUndo) {
  load();const auto owner=sourceNode(0),first=uint32_t(graph.nodeCount());const auto beforeCreation=graph.activeRoots();
  auto candidate=graph.makeEmptyNodeCandidate(owner,"Earlier inherited node");
  auto creation=manager->prepareEditorSceneRootRegistration(graph,candidate,first);const auto creationReceipt=creation.membership();
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(creation));
  const auto oldLocal=graph.getNode(first)->localTransform,world=graph.getNode(first)->worldTransform;
  const auto oldSibling=*graph.siblingIndex(first),newSibling=graph.getNode(editor)->children.size();
  const auto membership=reparentMembership(first,editor,world,newSibling);
  auto stale=manager->prepareReparentSceneRootChange(graph,membership,true);
  const auto cloneFirst=uint32_t(graph.nodeCount());const auto beforeClone=graph.activeRoots();
  auto clone=manager->prepareSubtreeCloneGeneration(graph,owner,"Independent source copy");
  append=graph.prepareAppend(clone.graph());ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));
  exchangeMerge(clone);EXPECT_FALSE(manager->canPublishReparentSceneRootChange(stale));
  EXPECT_THROW((void)manager->prepareReparentSceneRootChange(graph,membership,true),std::invalid_argument);
  auto detachClone=graph.prepareDetachTail(cloneFirst,beforeClone);ASSERT_TRUE(detachClone);
  ASSERT_TRUE(graph.publishTailChange(detachClone));exchangeMerge(clone);
  EXPECT_FALSE(manager->canPublishReparentSceneRootChange(stale));
  ASSERT_TRUE(publishReparent(membership,true,first,editor,world,newSibling));
  ASSERT_TRUE(publishReparent(membership,false,first,owner,oldLocal,oldSibling));
  auto remove=manager->prepareEditorSceneRootChange(graph,creationReceipt,false);
  auto detach=graph.prepareDetachTail(first,beforeCreation);ASSERT_TRUE(detach);
  ASSERT_TRUE(graph.publishTailChange(detach));ASSERT_TRUE(manager->publishEditorSceneRootChange(remove));
  EXPECT_EQ(graph.activeRoots(),beforeCreation);EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*beforeCreation);
}

TEST_F(SceneMergeGenerationGpu,ReparentRestoresRootOrderHintsAlreadyOwnedByACloneGeneration) {
  load();const auto owner=sourceNode(0),first=uint32_t(graph.nodeCount());
  auto candidate=graph.makeEmptyNodeCandidate(owner,"Retained empty before clone");
  auto creation=manager->prepareEditorSceneRootRegistration(graph,candidate,first);
  auto append=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(append);
  ASSERT_TRUE(graph.publishAppend(append));ASSERT_TRUE(manager->publishEditorSceneRootChange(creation));
  auto clone=manager->prepareSubtreeCloneGeneration(graph,owner,"Second source root");
  append=graph.prepareAppend(clone.graph());ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));
  exchangeMerge(clone);
  const auto exactRoots=graph.activeRoots();ASSERT_TRUE(exactRoots);ASSERT_EQ(exactRoots->size(),2u);
  const auto oldLocal=graph.getNode(first)->localTransform,world=graph.getNode(first)->worldTransform;
  const auto oldSibling=*graph.siblingIndex(first),newSibling=graph.getNode(editor)->children.size();
  const auto membership=reparentMembership(first,editor,world,newSibling);
  ASSERT_TRUE(publishReparent(membership,true,first,editor,world,newSibling));
  auto expected=*exactRoots;expected.push_back(first);
  EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,expected);
  ASSERT_TRUE(publishReparent(membership,false,first,owner,oldLocal,oldSibling));
  EXPECT_EQ(graph.activeRoots(),exactRoots);EXPECT_EQ(manager->planSceneSelection(graph,0).runtimeRoots,*exactRoots);
}

TEST_F(SceneMergeGenerationGpu,RetainedSourceTextureRolesPreserveAliasesAbsentRolesAndReloadIdentity) {
  loadSource(roleAliasedTriangle());
  const auto original=manager->gltfTextureResources();
  ASSERT_EQ(original.color.size(),3u);ASSERT_EQ(original.data.size(),3u);
  EXPECT_NE(original.color[0],UINT32_MAX);EXPECT_NE(original.data[0],UINT32_MAX);
  EXPECT_NE(original.color[0],original.data[0]);
  EXPECT_EQ(original.color[0],original.color[1]);EXPECT_EQ(original.data[0],original.data[1]);
  EXPECT_EQ(original.color[2],UINT32_MAX);EXPECT_EQ(original.data[2],UINT32_MAX);
  const auto* node=sourcePrimitive(graph,0);ASSERT_NE(node,nullptr);
  EXPECT_EQ(manager->resolveMaterialTextureIndex(node->materialIndex),original.color[0]);
  EXPECT_EQ(manager->resolveMaterialNormalTexture(node->materialIndex),original.data[1]);
  EXPECT_EQ(manager->resolveMaterialEmissiveTexture(node->materialIndex),original.color[1]);
  const auto baseline=allocations();auto malformed=triangle("Invalid reload",99);
  malformed.gltfModel.materials[0].normalTexture.index=99;
  EXPECT_FALSE(manager->reloadModel((temp/"invalid.glb").string(),2,{},AllocatedBuffer{},&malformed));
  EXPECT_EQ(manager->gltfTextureResources().color,original.color);EXPECT_EQ(manager->gltfTextureResources().data,original.data);
  EXPECT_EQ(allocations(),baseline);
  auto replacement=triangle("Replacement without textures",77,false,false);
  ASSERT_TRUE(manager->reloadModel((temp/"replacement.glb").string(),2,{},AllocatedBuffer{},&replacement));
  EXPECT_TRUE(manager->animationSource().textures.empty());
  EXPECT_TRUE(manager->gltfTextureResources().color.empty());EXPECT_TRUE(manager->gltfTextureResources().data.empty());
  auto restored=roleAliasedTriangle();
  ASSERT_TRUE(manager->reloadModel((temp/"restored.glb").string(),2,{},AllocatedBuffer{},&restored));
  ASSERT_EQ(manager->gltfTextureResources().color.size(),3u);ASSERT_EQ(manager->gltfTextureResources().data.size(),3u);
  EXPECT_EQ(manager->gltfTextureResources().color[0],manager->gltfTextureResources().color[1]);
  EXPECT_EQ(manager->gltfTextureResources().data[0],manager->gltfTextureResources().data[1]);
  EXPECT_EQ(manager->gltfTextureResources().color[2],UINT32_MAX);
}

TEST_F(SceneMergeGenerationGpu,MergeRetainsOldRolePrefixAndPairedManualSourcesAcrossRepeatedGenerations) {
  loadSource(roleAliasedTriangle());const auto original=manager->gltfTextureResources();
  auto provider=triangle("External provider",66);provider.gltfModel.images[0].uri="manual-provider.png";
  tinygltf::TinyGLTF writer;
  ASSERT_TRUE(writer.WriteGltfSceneToFile(&provider.gltfModel,(temp/"provider.gltf").string(),false,true,true,false));
  ASSERT_TRUE(std::filesystem::exists(temp/"manual-provider.png"));
  const auto manualSource=manager->loadMaterialTextureSource(temp/"manual-provider.png",true);
  const auto manualTexture=manualSource.runtimeResource();
  container::material::Material manual;manual.baseColorTextureIndex=manualTexture;
  SceneManager::MaterialTextureProvenance provenance;provenance[0]=manualSource;
  const auto manualMaterial=manager->createMaterial(manual,provenance);
  const auto paired=manager->gltfTextureResources();
  ASSERT_EQ(paired.color.size(),4u);ASSERT_EQ(paired.data.size(),4u);
  EXPECT_TRUE(std::equal(original.color.begin(),original.color.end(),paired.color.begin()));
  EXPECT_TRUE(std::equal(original.data.begin(),original.data.end(),paired.data.begin()));
  EXPECT_EQ(manager->animationSource().textures.size(),4u);
  EXPECT_EQ(manualSource.archiveTexture(),3u);EXPECT_EQ(manualSource.evaluatedTexture(),std::optional<uint32_t>(3));
  EXPECT_EQ(paired.color[3],manualTexture);EXPECT_EQ(paired.data[3],UINT32_MAX);
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(manualMaterial));
  EXPECT_EQ(manager->buildSceneExport(graph).textures.size(),4u);
  const auto beforeBytes=manager->mergeHistoryResidentBytes();const auto baseline=allocations();
  // Same source pixels+role+sampler deduplicate against A without a GPU image.
  auto staged=manager->prepareMergeGeneration(triangle("B alias",31),temp/"b.glb",{},graph);
  EXPECT_EQ(allocations(),baseline+3); // Detached material/metadata/clip buffers only.
  const auto merged=staged.manager().gltfTextureResources();
  ASSERT_EQ(merged.color.size(),5u);ASSERT_EQ(merged.data.size(),5u);
  EXPECT_TRUE(std::equal(paired.color.begin(),paired.color.end(),merged.color.begin()));
  EXPECT_TRUE(std::equal(paired.data.begin(),paired.data.end(),merged.data.begin()));
  EXPECT_EQ(merged.color[3],manualTexture);EXPECT_EQ(merged.data[3],UINT32_MAX);
  EXPECT_EQ(merged.color[4],original.color[0]);EXPECT_EQ(merged.data[4],UINT32_MAX);
  EXPECT_EQ(staged.manager().resolveMaterialTextureIndex(manualMaterial),manualTexture);
  EXPECT_EQ(staged.sourcePlan().merge.imported.textures,(std::vector<int32_t>{4}));
  {
    auto again=staged.manager().prepareMergeGeneration(triangle("C alias",31),temp/"c.glb",{},staged.graph());
    const auto& repeated=again.manager().gltfTextureResources();
    ASSERT_EQ(repeated.color.size(),6u);ASSERT_EQ(repeated.data.size(),6u);
    EXPECT_TRUE(std::equal(merged.color.begin(),merged.color.end(),repeated.color.begin()));
    EXPECT_TRUE(std::equal(merged.data.begin(),merged.data.end(),repeated.data.begin()));
    EXPECT_EQ(repeated.color[5],original.color[0]);EXPECT_EQ(repeated.data[5],UINT32_MAX);
    EXPECT_EQ(again.manager().resolveMaterialTextureIndex(manualMaterial),manualTexture);
  }
  const auto owned=allocations();const auto mergedBytes=staged.manager().mergeHistoryResidentBytes();
  exchangeMerge(staged);
  EXPECT_EQ(manager->gltfTextureResources().color,merged.color);EXPECT_EQ(staged.manager().gltfTextureResources().color,paired.color);
  EXPECT_EQ(manager->mergeHistoryResidentBytes(),mergedBytes);EXPECT_EQ(staged.manager().mergeHistoryResidentBytes(),beforeBytes);
  exchangeMerge(staged);
  EXPECT_EQ(manager->gltfTextureResources().color,paired.color);EXPECT_EQ(manager->gltfTextureResources().data,paired.data);
  exchangeMerge(staged);
  EXPECT_EQ(manager->gltfTextureResources().color,merged.color);EXPECT_EQ(manager->resolveMaterialTextureIndex(manualMaterial),manualTexture);
  EXPECT_EQ(allocations(),owned);
}

TEST_F(SceneMergeGenerationGpu,SubtreeCloneCopiesTextureRolesWithoutUploadingAndExchangeRetainsRoleAliases) {
  loadSource(roleAliasedTriangle());const auto original=manager->gltfTextureResources();
  const auto baseline=allocations();const auto bytes=manager->mergeHistoryResidentBytes();
  SceneMergeGenerationLimits limits;limits.maxNewTextureBytes=0;limits.maxPreparedTextureBytes=0;
  {
    auto clone=manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"Role-preserving copy",limits);
    EXPECT_EQ(allocations(),baseline+3);
    EXPECT_EQ(clone.manager().gltfTextureResources().color,original.color);
    EXPECT_EQ(clone.manager().gltfTextureResources().data,original.data);
    EXPECT_EQ(clone.manager().animationSource().textures.size(),manager->animationSource().textures.size());
    const auto cloneBytes=clone.manager().mergeHistoryResidentBytes();
    exchangeMerge(clone);
    EXPECT_EQ(manager->gltfTextureResources().color,original.color);EXPECT_EQ(manager->gltfTextureResources().data,original.data);
    EXPECT_EQ(manager->mergeHistoryResidentBytes(),cloneBytes);
    exchangeMerge(clone);EXPECT_EQ(manager->mergeHistoryResidentBytes(),bytes);
    EXPECT_EQ(manager->gltfTextureResources().color,original.color);EXPECT_EQ(manager->gltfTextureResources().data,original.data);
  }
  EXPECT_EQ(allocations(),baseline);
}

TEST_F(SceneMergeGenerationGpu,MalformedTextureContextsAndIncomingBindingsRejectBeforeAllocationsOrRoleMutation) {
  loadSource(roleAliasedTriangle());const auto original=manager->gltfTextureResources();
  const auto baseline=allocations();const auto revision=manager->materialRevision(),epoch=manager->generationEpoch();
  auto incoming=triangle("Bad incoming",155);incoming.gltfModel.textures[0].source=99;
  EXPECT_THROW((void)manager->prepareMergeGeneration(incoming,temp/"invalid.glb",{},graph),std::runtime_error);
  manager->animationSource().textures.push_back(manager->animationSource().textures[0]);
  EXPECT_THROW((void)manager->prepareMergeGeneration(triangle("B",55),temp/"b.glb",{},graph),std::runtime_error);
  EXPECT_THROW((void)manager->prepareSubtreeCloneGeneration(graph,sourceNode(0)),std::runtime_error);
  manager->animationSource().textures.pop_back();
  EXPECT_EQ(manager->gltfTextureResources().color,original.color);EXPECT_EQ(manager->gltfTextureResources().data,original.data);
  EXPECT_EQ(manager->materialRevision(),revision);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(allocations(),baseline);
}

TEST_F(SceneMergeGenerationGpu,TransmissionRoutingUsesExactPositiveFactorAndKeepsTextureSlotIdentity) {
  loadSource(roleAliasedTriangle());const auto roles=manager->gltfTextureResources();
  ASSERT_NE(roles.data[0],UINT32_MAX);
  for(const float factor:{0.0f,.0005f,.25f,1.0f}) {
    SCOPED_TRACE(factor);container::material::Material material;
    material.transmissionTextureIndex=roles.data[0];material.transmissionFactor=factor;
    const auto id=manager->createMaterial(material);
    EXPECT_EQ(manager->materialRenderProperties(id).transparent,factor>0);
    EXPECT_EQ(manager->isMaterialTransparent(id),factor>0);
    EXPECT_EQ(manager->resolveMaterialTransmissionTexture(id),roles.data[0]);
    EXPECT_FLOAT_EQ(manager->resolveMaterialTransmissionFactor(id),factor);
  }
  container::material::Material blend;blend.alphaMode=container::material::AlphaMode::Blend;
  blend.transmissionTextureIndex=roles.data[0];blend.transmissionFactor=0;
  const auto blendId=manager->createMaterial(blend);
  EXPECT_TRUE(manager->materialRenderProperties(blendId).transparent);EXPECT_TRUE(manager->isMaterialTransparent(blendId));
  blend.alphaMode=container::material::AlphaMode::Mask;
  const auto maskId=manager->createMaterial(blend);
  EXPECT_FALSE(manager->materialRenderProperties(maskId).transparent);EXPECT_TRUE(manager->materialRenderProperties(maskId).alphaMasked);
  EXPECT_EQ(manager->resolveMaterialTransmissionTexture(maskId),roles.data[0]);
  EXPECT_EQ(manager->gltfTextureResources().color,roles.color);EXPECT_EQ(manager->gltfTextureResources().data,roles.data);
}
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

TEST_F(SceneMergeGenerationGpu,SceneBindingsDiscardOwnsOnlyDescriptorsAndPreservesResidentTables) {
  load();MaterialFrames frames(*allocation),auxiliaryFrames(*allocation);
  const auto baseline=allocations();const auto descriptor=manager->descriptorSet(0),auxiliary=manager->auxiliaryDescriptorSet(1);
  const auto epoch=manager->generationEpoch(),material=manager->materialRevision(),authored=manager->authoredMaterialRevision();
  const auto geometry=manager->geometryRevision(),graphRevision=graph.revision();
  const auto* vertices=manager->vertices().data();const auto* bytes=manager->animationSource().buffers[0].data.data();
  const auto* materials=manager->animationSource().materials.data();const auto original=manager->buildSceneExport(graph);
  {
    auto stage=manager->prepareSceneBindings();EXPECT_TRUE(stage);EXPECT_EQ(stage.generationEpoch(),epoch+1);
    EXPECT_FALSE(manager->canExchangeSceneBindings(stage));EXPECT_EQ(stage.descriptorSet(0),VK_NULL_HANDLE);
    stage.prepareDescriptorFrames(frames.cameras,frames.objects,auxiliaryFrames.objects);
    EXPECT_TRUE(manager->canExchangeSceneBindings(stage));EXPECT_EQ(allocations(),baseline);
    for(uint32_t i=0;i<2;++i) {
      EXPECT_NE(stage.descriptorSet(i),VK_NULL_HANDLE);EXPECT_NE(stage.auxiliaryDescriptorSet(i),VK_NULL_HANDLE);
      EXPECT_NE(stage.descriptorSet(i),stage.auxiliaryDescriptorSet(i));
    }
    EXPECT_EQ(stage.descriptorSet(2),VK_NULL_HANDLE);EXPECT_EQ(stage.auxiliaryDescriptorSet(2),VK_NULL_HANDLE);
    auto moved=std::move(stage);EXPECT_FALSE(stage);EXPECT_TRUE(moved);
    EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->auxiliaryDescriptorSet(1),auxiliary);
  }
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialRevision(),material);
  EXPECT_EQ(manager->authoredMaterialRevision(),authored);EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(graph.revision(),graphRevision);
  EXPECT_EQ(manager->vertices().data(),vertices);EXPECT_EQ(manager->animationSource().buffers[0].data.data(),bytes);
  EXPECT_EQ(manager->animationSource().materials.data(),materials);EXPECT_EQ(manager->buildSceneExport(graph),original);
}

TEST_F(SceneMergeGenerationGpu,SceneBindingsPublicationAllocatesNothingAndRetainsOldPoolUntilTokenDestruction) {
  load();MaterialFrames frames(*allocation);manager->updateDescriptorSets(frames.cameras,frames.objects);
  manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const std::array oldPrimary{manager->descriptorSet(0),manager->descriptorSet(1)};
  const std::array oldAuxiliary{manager->auxiliaryDescriptorSet(0),manager->auxiliaryDescriptorSet(1)};
  const auto baseline=allocations();const auto epoch=manager->generationEpoch(),material=manager->materialRevision();
  const auto authored=manager->authoredMaterialRevision(),geometry=manager->geometryRevision();
  const auto sourceBefore=manager->buildSceneExport(graph);const auto* gpuSource=manager->animationSource().materials.data();
  auto stage=manager->prepareSceneBindings();stage.prepareDescriptorFrames(frames.cameras,std::span(frames.objects.data(),1));
  const std::array primary{stage.descriptorSet(0),stage.descriptorSet(1)};
  const std::array auxiliary{stage.auxiliaryDescriptorSet(0),stage.auxiliaryDescriptorSet(1)};
  const auto clipBuffer=manager->sceneClipBuffer().buffer;auto clip=manager->sceneClipState();
  clip.boxClipEnabled=1;clip.boxClipPlaneCount=1;clip.boxClipPlanes[0]={0,1,0,-2};manager->updateSceneClipState(clip);
  bool accepted{};size_t attempted{};
  {
    material_publication_test::AllocationGuard guard(true);
    accepted=manager->canExchangeSceneBindings(stage);manager->exchangeSceneBindings(stage);attempted=guard.allocations();
  }
  EXPECT_TRUE(accepted);EXPECT_EQ(attempted,0u);EXPECT_FALSE(stage);EXPECT_FALSE(manager->canExchangeSceneBindings(stage));
  EXPECT_EQ(manager->generationEpoch(),epoch+1);EXPECT_EQ(manager->materialRevision(),material);
  EXPECT_EQ(manager->authoredMaterialRevision(),authored);EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(allocations(),baseline);
  EXPECT_EQ(manager->sceneClipBuffer().buffer,clipBuffer);EXPECT_EQ(manager->sceneClipState().boxClipEnabled,1u);
  EXPECT_EQ(manager->sceneClipState().boxClipPlanes[0],clip.boxClipPlanes[0]);
  EXPECT_EQ(manager->animationSource().materials.data(),gpuSource);EXPECT_EQ(manager->buildSceneExport(graph),sourceBefore);
  for(uint32_t i=0;i<2;++i) {
    EXPECT_EQ(manager->descriptorSet(i),primary[i]);EXPECT_EQ(manager->auxiliaryDescriptorSet(i),auxiliary[i]);
    EXPECT_EQ(stage.descriptorSet(i),oldPrimary[i]);EXPECT_EQ(stage.auxiliaryDescriptorSet(i),oldAuxiliary[i]);
  }
  // Validation must accept both retired and published sets while the token
  // still owns the original pool. Commands bind them without reading material
  // bytes or depending on a shader-specific test pipeline.
  struct Layout {
    VkDevice device{};VkPipelineLayout value{};
    ~Layout(){if(value) vkDestroyPipelineLayout(device,value,nullptr);}
  } layout{device->device()};
  const auto setLayout=manager->descriptorSetLayout();
  VkPipelineLayoutCreateInfo create{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};create.setLayoutCount=1;create.pSetLayouts=&setLayout;
  ASSERT_EQ(vkCreatePipelineLayout(layout.device,&create,nullptr,&layout.value),VK_SUCCESS);
  allocation->executeStagedCommands([&](VkCommandBuffer command) {
    for(const auto& sets:{oldPrimary,oldAuxiliary,primary,auxiliary}) for(const auto set:sets)
      vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout.value,0,1,&set,0,nullptr);
  });
  manager->exchangeSceneBindings(stage);EXPECT_EQ(manager->generationEpoch(),epoch+1);
  stage={};EXPECT_EQ(allocations(),baseline);
}

TEST_F(SceneMergeGenerationGpu,SceneBindingsRejectInvalidLimitsAndFrameShapesBeforePublication) {
  load();MaterialFrames frames(*allocation);const auto baseline=allocations();const auto epoch=manager->generationEpoch();
  SceneBindingsLimits limits;
  limits.maxPreparedCpuBytes=0;EXPECT_THROW((void)manager->prepareSceneBindings(limits),std::invalid_argument);
  limits.maxPreparedCpuBytes=UINT64_MAX;EXPECT_THROW((void)manager->prepareSceneBindings(limits),std::invalid_argument);
  limits.maxPreparedCpuBytes=1;EXPECT_THROW((void)manager->prepareSceneBindings(limits),std::invalid_argument);
  limits={};limits.maxDescriptorFrames=0;EXPECT_THROW((void)manager->prepareSceneBindings(limits),std::invalid_argument);
  limits.maxDescriptorFrames=1025;EXPECT_THROW((void)manager->prepareSceneBindings(limits),std::invalid_argument);
  limits.maxDescriptorFrames=1;auto limited=manager->prepareSceneBindings(limits);
  EXPECT_THROW(limited.prepareDescriptorFrames(frames.cameras,frames.objects),std::invalid_argument);
  auto stage=manager->prepareSceneBindings();
  EXPECT_THROW(stage.prepareDescriptorFrames({},frames.objects),std::invalid_argument);
  EXPECT_THROW(stage.prepareDescriptorFrames(frames.cameras,{}),std::invalid_argument);
  std::vector<AllocatedBuffer> tooMany(3,frames.objects[0]);
  EXPECT_THROW(stage.prepareDescriptorFrames(frames.cameras,tooMany),std::invalid_argument);
  EXPECT_THROW(stage.prepareDescriptorFrames(frames.cameras,frames.objects,tooMany),std::invalid_argument);
  auto invalidCameras=frames.cameras;invalidCameras[0].buffer=VK_NULL_HANDLE;
  EXPECT_THROW(stage.prepareDescriptorFrames(invalidCameras,frames.objects),std::invalid_argument);
  invalidCameras=frames.cameras;invalidCameras[0].allocation_info.size=1;
  EXPECT_THROW(stage.prepareDescriptorFrames(invalidCameras,frames.objects),std::invalid_argument);
  auto invalidObjects=frames.objects;invalidObjects[0].allocation_info.size=1;
  EXPECT_THROW(stage.prepareDescriptorFrames(frames.cameras,invalidObjects),std::invalid_argument);
  EXPECT_FALSE(manager->canExchangeSceneBindings(stage));EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->generationEpoch(),epoch);
  stage.prepareDescriptorFrames(frames.cameras,frames.objects);EXPECT_TRUE(manager->canExchangeSceneBindings(stage));
  EXPECT_THROW(stage.prepareDescriptorFrames(frames.cameras,frames.objects),std::invalid_argument);
}

TEST_F(SceneMergeGenerationGpu,SceneBindingsCpuBudgetIncludesFrameAndWriteScratchBeforePoolAllocation) {
  load();MaterialFrames frames(*allocation);const auto baseline=allocations();const auto descriptor=manager->descriptorSet(0);
  // Find the minimum admission budget without assuming this ABI's State size
  // or the device's sampler capacity. The admitted token has no remaining
  // storage budget for frame tables and write scratch.
  uint64_t low=1,high=32ull*1024*1024;
  while(low<high) {
    const auto middle=low+(high-low)/2;SceneBindingsLimits limits;limits.maxPreparedCpuBytes=middle;
    try {auto probe=manager->prepareSceneBindings(limits);high=middle;}
    catch(const std::invalid_argument&) {low=middle+1;}
  }
  SceneBindingsLimits limits;limits.maxPreparedCpuBytes=low;auto stage=manager->prepareSceneBindings(limits);
  EXPECT_THROW(stage.prepareDescriptorFrames(frames.cameras,frames.objects),std::invalid_argument);
  EXPECT_EQ(stage.descriptorSet(0),VK_NULL_HANDLE);EXPECT_FALSE(manager->canExchangeSceneBindings(stage));
  EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(allocations(),baseline);
}

TEST_F(SceneMergeGenerationGpu,SceneBindingsRejectMaterialChangesAndCompetingDescriptorPublication) {
  load();MaterialFrames frames(*allocation);auto stale=manager->prepareSceneBindings();stale.prepareDescriptorFrames(frames.cameras,frames.objects);
  ASSERT_TRUE(manager->canExchangeSceneBindings(stale));manager->uploadMaterialResources();
  EXPECT_FALSE(manager->canExchangeSceneBindings(stale));const auto epoch=manager->generationEpoch();const auto descriptor=manager->descriptorSet(0);
  size_t attempted{};
  {
    material_publication_test::AllocationGuard guard(true);manager->exchangeSceneBindings(stale);attempted=guard.allocations();
  }
  EXPECT_EQ(attempted,0u);EXPECT_TRUE(stale);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->descriptorSet(0),descriptor);
  auto next=manager->prepareSceneBindings();next.prepareDescriptorFrames(frames.cameras,frames.objects);
  auto competing=manager->prepareSceneBindings();competing.prepareDescriptorFrames(frames.cameras,frames.objects);
  ASSERT_TRUE(manager->canExchangeSceneBindings(next));ASSERT_TRUE(manager->canExchangeSceneBindings(competing));
  manager->exchangeSceneBindings(next);EXPECT_FALSE(manager->canExchangeSceneBindings(competing));
  const auto published=manager->descriptorSet(0);manager->exchangeSceneBindings(competing);
  EXPECT_EQ(manager->generationEpoch(),epoch+1);EXPECT_EQ(manager->descriptorSet(0),published);
  auto beforeUpload=manager->prepareSceneBindings();manager->uploadMaterialResources();
  EXPECT_THROW(beforeUpload.prepareDescriptorFrames(frames.cameras,frames.objects),std::invalid_argument);
}

TEST_F(SceneMergeGenerationGpu,SceneBindingsRejectForeignManagerAndDestroyedOwnerIdentity) {
  load();MaterialFrames frames(*allocation);auto stage=manager->prepareSceneBindings();stage.prepareDescriptorFrames(frames.cameras,frames.objects);
  auto other=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});
  EXPECT_THROW((void)other->prepareSceneBindings(),std::invalid_argument);
  const auto otherEpoch=other->generationEpoch();const auto otherDescriptor=other->descriptorSet(0);
  EXPECT_FALSE(other->canExchangeSceneBindings(stage));other->exchangeSceneBindings(stage);
  EXPECT_EQ(other->generationEpoch(),otherEpoch);EXPECT_EQ(other->descriptorSet(0),otherDescriptor);
  // New managers can reuse an address, epoch, and resource counts. Their unique
  // identity still prevents a discarded old owner's token becoming authority.
  other.reset();manager.reset();manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});
  manager->initialize((temp/"a.glb").string(),2.f,2);
  EXPECT_FALSE(manager->canExchangeSceneBindings(stage));const auto epoch=manager->generationEpoch();
  manager->exchangeSceneBindings(stage);EXPECT_EQ(manager->generationEpoch(),epoch);
}

TEST_F(SceneMergeGenerationGpu,ResidentNormalCoordinateUsesExplicitSlotProvenanceAndRejectsInvalidMaterial) {
  loadSource(roleAliasedTriangle());const auto mapping=manager->sourceMaterialRuntimeMapping();
  ASSERT_FALSE(mapping.empty());const auto runtime=mapping.front();
  ASSERT_NE(manager->resolveMaterialNormalTexture(runtime),UINT32_MAX);
  EXPECT_EQ(manager->materialNormalTexCoord(runtime),std::optional<uint32_t>(0));
  container::material::Material material;material.normalTextureIndex=manager->resolveMaterialNormalTexture(runtime);
  material.normalTextureTransform.texCoord=1;material.normalTextureTransform.offset={.5f,.25f};
  const auto manual=manager->createMaterial(material);
  // A shared image handle does not imply a shared normal-coordinate choice.
  EXPECT_EQ(manager->resolveMaterialNormalTexture(manual),manager->resolveMaterialNormalTexture(runtime));
  EXPECT_EQ(manager->materialNormalTexCoord(manual),std::optional<uint32_t>(1));
  material.normalTextureIndex=UINT32_MAX;const auto absent=manager->createMaterial(material);
  EXPECT_FALSE(manager->materialNormalTexCoord(absent));
  EXPECT_THROW((void)manager->materialNormalTexCoord(uint32_t(manager->materialCount())),std::invalid_argument);
  EXPECT_THROW((void)manager->materialNormalTexCoord(UINT32_MAX),std::invalid_argument);
}

TEST_F(SceneMergeGenerationGpu,MaterialStageDiscardOwnsOnlyOneNewBufferAndDetachedDescriptors) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);const auto allocationsBefore=allocations();
  const auto descriptor=manager->descriptorSet(0),auxiliary=manager->auxiliaryDescriptorSet(1);
  const auto geometry=manager->geometryRevision(),material=manager->materialRevision(),epoch=manager->generationEpoch();
  const auto* vertices=manager->vertices().data();const auto* indices=manager->indices().data();
  const auto* bytes=manager->animationSource().buffers[0].data.data();
  const auto sourceBefore=manager->buildSceneExport(graph);
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Roughness,.25}};
  const auto command=manager->planMaterialEdit(0,patch);
  {
    auto stage=manager->prepareMaterialGeneration(command,nullptr);
    EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));EXPECT_FALSE(stage.routingChanged());
    EXPECT_EQ(allocations(),allocationsBefore+1);frames.bind(stage);
    EXPECT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
    EXPECT_NE(stage.descriptorSet(0),descriptor);EXPECT_NE(stage.auxiliaryDescriptorSet(1),auxiliary);
    EXPECT_FLOAT_EQ(stage.gpuMaterials()[stage.runtimeMaterial()].metallicRoughness.y,.25f);
    EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->auxiliaryDescriptorSet(1),auxiliary);
    EXPECT_EQ(manager->buildSceneExport(graph),sourceBefore);
    auto moved=std::move(stage);EXPECT_FALSE(stage);EXPECT_TRUE(moved);
  }
  EXPECT_EQ(allocations(),allocationsBefore);EXPECT_EQ(manager->geometryRevision(),geometry);
  EXPECT_EQ(manager->materialRevision(),material);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(manager->vertices().data(),vertices);EXPECT_EQ(manager->indices().data(),indices);
  EXPECT_EQ(manager->animationSource().buffers[0].data.data(),bytes);
}

TEST_F(SceneMergeGenerationGpu,MaterialPublicationAllocatesNothingAndUpdatesAuthoredEvaluatedExportAndGpuTogether) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);const auto base=allocations();
  GltfMaterialPatch patch;patch.name="Edited material";patch.doubleSided=true;patch.alphaMode=GltfMaterialAlphaMode::Mask;
  patch.values={{GltfMaterialProperty::Clearcoat,0.},{GltfMaterialProperty::BaseColor,std::array<double,4>{0.,.2,.8,1.}}};
  const auto command=manager->planMaterialEdit(0,patch);
  auto stage=manager->prepareMaterialGeneration(command,nullptr);const auto runtime=stage.runtimeMaterial();
  ASSERT_EQ(manager->sourceMaterialForRuntime(runtime),0);EXPECT_TRUE(stage.routingChanged());
  frames.bind(stage);ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
  const auto descriptor=stage.descriptorSet(0),auxiliary=stage.auxiliaryDescriptorSet(1);
  const auto epoch=manager->generationEpoch(),material=manager->materialRevision(),authored=manager->authoredMaterialRevision();
  const auto geometry=manager->geometryRevision(),graphRevision=graph.revision();const auto texture=manager->resolveMaterialTextureIndex(runtime);
  size_t publicationAllocations{};
  {
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMaterialGeneration(stage,nullptr);publicationAllocations=guard.allocations();
  }
  EXPECT_EQ(publicationAllocations,0u);EXPECT_FALSE(stage);
  EXPECT_EQ(manager->generationEpoch(),epoch+1);EXPECT_EQ(manager->materialRevision(),material+1);
  EXPECT_EQ(manager->authoredMaterialRevision(),authored+1);EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(graph.revision(),graphRevision);
  EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->auxiliaryDescriptorSet(1),auxiliary);
  EXPECT_EQ(manager->authoredMaterial(0).name,"Edited material");EXPECT_EQ(manager->animationSource().materials[0].name,"Edited material");
  EXPECT_EQ(manager->authoredMaterial(0).alphaMode,"MASK");EXPECT_TRUE(manager->isMaterialDoubleSided(runtime));
  EXPECT_TRUE(manager->isMaterialAlphaMasked(runtime));EXPECT_FLOAT_EQ(manager->resolveMaterialClearcoatFactor(runtime),0.f);
  EXPECT_EQ(manager->resolveMaterialTextureIndex(runtime),texture);EXPECT_EQ(manager->resolveMaterialColor(runtime),glm::vec4(0,.2f,.8f,1));
  const auto saved=manager->buildSceneExport(graph);
  ASSERT_LT(runtime,saved.materials.size());EXPECT_EQ(saved.materials[runtime].name,"Edited material");
  EXPECT_EQ(saved.materials[runtime].alphaMode,"MASK");EXPECT_TRUE(saved.materials[runtime].doubleSided);
  ASSERT_TRUE(saved.materials[runtime].extensions.contains("KHR_materials_clearcoat"));
  EXPECT_DOUBLE_EQ(saved.materials[runtime].extensions.at("KHR_materials_clearcoat").Get("clearcoatFactor").GetNumberAsDouble(),0.);
  const auto published=manager->generationEpoch();manager->exchangeMaterialGeneration(stage,nullptr);EXPECT_EQ(manager->generationEpoch(),published);
  stage={};EXPECT_EQ(allocations(),base);
}

TEST_F(SceneMergeGenerationGpu,MaterialUndoRemovesExtensionAndExportCacheWithoutRemovingGlobalDeclarations) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Anisotropy,.6}};patch.name="Anisotropic";
  const auto command=manager->planMaterialEdit(0,patch);
  auto edit=manager->prepareMaterialGeneration(command,nullptr);EXPECT_TRUE(edit.hasExtendedPbrMaterials());
  EXPECT_TRUE(std::ranges::all_of(edit.gpuMaterials(),[](const auto& m){return m.anisotropyDiffuseFactors.w==1.f;}));
  const auto runtime=edit.runtimeMaterial();frames.bind(edit);ASSERT_TRUE(manager->canExchangeMaterialGeneration(edit,nullptr));
  manager->exchangeMaterialGeneration(edit,nullptr);EXPECT_TRUE(manager->hasExtendedPbrMaterials());
  const auto undoCommand=manager->rebaseMaterialEdit(command,GltfMaterialEditSide::Before);
  auto undo=manager->prepareMaterialGeneration(undoCommand,nullptr);EXPECT_FALSE(undo.hasExtendedPbrMaterials());
  EXPECT_TRUE(std::ranges::all_of(undo.gpuMaterials(),[](const auto& m){return m.anisotropyDiffuseFactors.w==0.f;}));
  frames.bind(undo);ASSERT_TRUE(manager->canExchangeMaterialGeneration(undo,nullptr));
  manager->exchangeMaterialGeneration(undo,nullptr);
  EXPECT_FALSE(manager->authoredMaterial(0).extensions.contains("KHR_materials_anisotropy"));
  EXPECT_FALSE(manager->animationSource().materials[0].extensions.contains("KHR_materials_anisotropy"));
  EXPECT_NE(std::ranges::find(manager->animationSource().extensionsUsed,"KHR_materials_anisotropy"),manager->animationSource().extensionsUsed.end());
  const auto saved=manager->buildSceneExport(graph);
  EXPECT_FALSE(saved.materials[runtime].extensions.contains("KHR_materials_anisotropy"));
  auto redo=manager->prepareMaterialGeneration(manager->rebaseMaterialEdit(command,GltfMaterialEditSide::After),nullptr);
  frames.bind(redo);ASSERT_TRUE(manager->canExchangeMaterialGeneration(redo,nullptr));manager->exchangeMaterialGeneration(redo,nullptr);
  EXPECT_EQ(manager->authoredMaterial(0).name,"Anisotropic");EXPECT_TRUE(manager->hasExtendedPbrMaterials());
}

TEST_F(SceneMergeGenerationGpu,MaterialStaleRecordsPlayersDeclarationsAndGpuTablesRejectBeforePublication) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Metallic,0.}};
  const auto command=manager->planMaterialEdit(0,patch);
  auto stage=manager->prepareMaterialGeneration(command,&player);frames.bind(stage);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));
  player.setSpeed(2);EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,&player));player.setSpeed(1);
  const auto roughness=manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor;
  manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor=std::nextafter(roughness,0.);
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,&player));
  manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor=roughness;
  manager->animationSource().extensionsUsed.push_back("VENDOR_later");EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,&player));
  manager->animationSource().extensionsUsed.pop_back();ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  auto competing=manager->prepareMaterialGeneration(command,&player);frames.bind(competing);
  manager->exchangeMaterialGeneration(competing,&player);EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,&player));
  EXPECT_THROW(manager->prepareMaterialGeneration(command,&player),std::invalid_argument);
}

TEST_F(SceneMergeGenerationGpu,MaterialPreparationBudgetsAndInvalidBindingsLeaveAllLiveStateUnchanged) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);const auto before=manager->buildSceneExport(graph);const auto base=allocations();
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Clearcoat,.3}};const auto command=manager->planMaterialEdit(0,patch);
  MaterialGenerationLimits limits;limits.maxPreparedCpuBytes=1;
  EXPECT_THROW(manager->prepareMaterialGeneration(command,nullptr,limits),std::invalid_argument);
  limits={};limits.maxMaterialBufferBytes=1;EXPECT_THROW(manager->prepareMaterialGeneration(command,nullptr,limits),std::invalid_argument);
  auto forged=command;forged.authoredAfter.name="forged";EXPECT_THROW(manager->prepareMaterialGeneration(forged,nullptr),std::invalid_argument);
  EXPECT_THROW(manager->planMaterialEdit(-1,patch),std::invalid_argument);
  auto stage=manager->prepareMaterialGeneration(command,nullptr);
  EXPECT_THROW(stage.prepareDescriptorFrames({},frames.objects),std::invalid_argument);
  auto invalidCameras=frames.cameras;invalidCameras[1].buffer=VK_NULL_HANDLE;
  EXPECT_THROW(stage.prepareDescriptorFrames(invalidCameras,frames.objects),std::invalid_argument);
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));
  EXPECT_EQ(manager->buildSceneExport(graph),before);stage={};EXPECT_EQ(allocations(),base);
}

TEST_F(SceneMergeGenerationGpu,AnimatedMaterialEditAndUndoResampleCurrentClockWithoutRewindingOtherSourceValues) {
  using namespace container::geometry::gltf;using V=tinygltf::Value;using O=V::Object;
  auto asset=triangle("Material animation",93);auto& source=asset.gltfModel;
  source.materials[0].pbrMetallicRoughness.roughnessFactor=.35;
  source.materials.push_back(source.materials[0]);source.materials[1].name="Untouched";
  const auto times=cloneAccessor(source,std::array<float,2>{0,2},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  const auto values=cloneAccessor(source,std::array<float,2>{.1f,.9f},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  const auto translations=cloneAccessor(source,std::array<float,6>{0,0,0,2,0,0},TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  tinygltf::Animation clip;
  tinygltf::AnimationSampler sampler;sampler.input=times;sampler.output=values;sampler.interpolation="LINEAR";clip.samplers.push_back(sampler);
  tinygltf::AnimationChannel pointer;pointer.sampler=0;pointer.target_node=-1;pointer.target_path="pointer";
  pointer.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::string("/materials/0/pbrMetallicRoughness/roughnessFactor"))}});
  clip.channels.push_back(pointer);sampler.output=translations;clip.samplers.push_back(sampler);
  tinygltf::AnimationChannel node;node.sampler=1;node.target_node=0;node.target_path="translation";clip.channels.push_back(node);
  source.animations.push_back(clip);source.extensionsUsed.push_back("KHR_animation_pointer");
  loadSource(std::move(asset));MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.5));
  manager->publishAnimationChanges(dirty,graph);player.setLoop(false);player.setSpeed(.5);player.play();
  const auto* buffer=manager->animationSource().buffers[0].data.data();const auto clips=manager->animationSource().animations;
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Roughness,.2}};patch.name="New authored rest";
  const auto command=manager->planMaterialEdit(0,patch);auto stage=manager->prepareMaterialGeneration(command,&player);
  const auto runtime=stage.runtimeMaterial();frames.bind(stage);ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  size_t attempts{};{
    material_publication_test::AllocationGuard guard(true);manager->exchangeMaterialGeneration(stage,&player);attempts=guard.allocations();
  }
  EXPECT_EQ(attempts,0u);EXPECT_DOUBLE_EQ(player.time(),.5);EXPECT_TRUE(player.playing());EXPECT_FALSE(player.loop());EXPECT_DOUBLE_EQ(player.speed(),.5);
  EXPECT_DOUBLE_EQ(player.library().restMaterials[0].pbrMetallicRoughness.roughnessFactor,.2);
  EXPECT_NEAR(manager->resolveMaterialMetallicRoughnessFactors(runtime).y,.3,1e-6);
  dirty=player.seek(manager->animationSource(),1.5);manager->publishAnimationChanges(dirty,graph);
  const auto currentNode=manager->animationSource().nodes[0];const auto unrelated=manager->animationSource().materials[1];
  auto undo=manager->prepareMaterialGeneration(manager->rebaseMaterialEdit(command,GltfMaterialEditSide::Before),&player);
  frames.bind(undo);ASSERT_TRUE(manager->canExchangeMaterialGeneration(undo,&player));manager->exchangeMaterialGeneration(undo,&player);
  EXPECT_DOUBLE_EQ(player.time(),1.5);EXPECT_TRUE(player.playing());EXPECT_FALSE(player.loop());EXPECT_DOUBLE_EQ(player.speed(),.5);
  EXPECT_DOUBLE_EQ(player.library().restMaterials[0].pbrMetallicRoughness.roughnessFactor,.35);
  EXPECT_NEAR(manager->resolveMaterialMetallicRoughnessFactors(runtime).y,.7,1e-6);
  EXPECT_EQ(manager->animationSource().nodes[0],currentNode);EXPECT_EQ(manager->animationSource().materials[1],unrelated);
  EXPECT_EQ(manager->animationSource().animations,clips);EXPECT_EQ(manager->animationSource().buffers[0].data.data(),buffer);
}

GltfLoadResult animatedMaterialAppendAsset() {
  using V=tinygltf::Value;using O=V::Object;
  auto result=triangle("Animated material append",93);auto& source=result.gltfModel;
  source.materials[0].pbrMetallicRoughness.roughnessFactor=.35;
  source.materials[0].extras=V(O{{"opaqueInteger",V(16777217)},{"owner",V(std::string("original"))}});
  const auto times=cloneAccessor(source,std::array<float,2>{0,2},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  const auto roughness=cloneAccessor(source,std::array<float,2>{.1f,.9f},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  const auto positions=cloneAccessor(source,std::array<float,6>{0,0,0,2,0,0},TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT,2);
  tinygltf::Animation clip;clip.name="Retained absolute material and node curves";
  tinygltf::AnimationSampler sampler;sampler.input=times;sampler.output=roughness;sampler.interpolation="LINEAR";
  sampler.extras=V(O{{"samplerOwner",V(std::string("unchanged"))}});clip.samplers.push_back(sampler);
  tinygltf::AnimationChannel pointer;pointer.sampler=0;pointer.target_node=-1;pointer.target_path="pointer";
  pointer.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::string("/materials/0/pbrMetallicRoughness/roughnessFactor"))}});
  clip.channels.push_back(pointer);sampler.output=positions;clip.samplers.push_back(sampler);
  tinygltf::AnimationChannel node;node.sampler=1;node.target_node=0;node.target_path="translation";clip.channels.push_back(node);
  source.animations.push_back(clip);source.extensionsUsed.push_back("KHR_animation_pointer");return result;
}

TEST_F(SceneMergeGenerationGpu,MaterialDuplicateDiscardKeepsResidentTablesGeometryImagesAndEveryFrameUntouched) {
  using namespace container::geometry::gltf;
  loadSource(roleAliasedTriangle());MaterialFrames frames(*allocation);
  const auto baseline=allocations();const auto sourceCount=manager->animationSource().materials.size();
  const auto runtimeCount=manager->materialCount(),epoch=manager->generationEpoch();
  const auto geometry=manager->geometryRevision(),graphRevision=graph.revision();
  const auto before=manager->buildSceneExport(graph);const auto roles=manager->gltfTextureResources();
  const auto* vertices=manager->vertices().data();const auto* indices=manager->indices().data();
  const auto* images=manager->animationSource().images.data();const auto* pixels=manager->animationSource().images[0].image.data();
  const std::array<VkDescriptorSet,2> primary{manager->descriptorSet(0),manager->descriptorSet(1)};
  const std::array<VkDescriptorSet,2> auxiliary{manager->auxiliaryDescriptorSet(0),manager->auxiliaryDescriptorSet(1)};
  GltfMaterialPatch patch;patch.name="Detached duplicate";patch.doubleSided=true;
  patch.values={{GltfMaterialProperty::Roughness,.23}};
  const auto command=manager->planMaterialDuplicate(0,patch);
  ASSERT_EQ(command.kind,GltfMaterialPlanKind::Duplicate);ASSERT_EQ(command.destinationMaterial,int32_t(sourceCount));
  {
    auto stage=manager->prepareMaterialGeneration(command,nullptr);
    EXPECT_EQ(stage.sourceMaterial(),int32_t(sourceCount));EXPECT_EQ(stage.runtimeMaterial(),runtimeCount);
    ASSERT_EQ(stage.gpuMaterials().size(),runtimeCount+1);ASSERT_EQ(stage.materialProperties().size(),runtimeCount+1);
    EXPECT_FLOAT_EQ(stage.gpuMaterials().back().metallicRoughness.y,.23f);EXPECT_TRUE(stage.materialProperties().back().doubleSided);
    ASSERT_NE(stage.materialBuffer().allocation_info.pMappedData,nullptr);
    EXPECT_EQ(std::memcmp(stage.materialBuffer().allocation_info.pMappedData,stage.gpuMaterials().data(),
        stage.gpuMaterials().size_bytes()),0); // Actual mapped VMA buffer; not a GPU execution/readback claim.
    EXPECT_EQ(allocations(),baseline+1);EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));
    frames.bind(stage);ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
    for(uint32_t image=0;image<2;++image) {
      EXPECT_NE(stage.descriptorSet(image),VK_NULL_HANDLE);EXPECT_NE(stage.descriptorSet(image),primary[image]);
      EXPECT_NE(stage.auxiliaryDescriptorSet(image),VK_NULL_HANDLE);EXPECT_NE(stage.auxiliaryDescriptorSet(image),auxiliary[image]);
      EXPECT_EQ(manager->descriptorSet(image),primary[image]);EXPECT_EQ(manager->auxiliaryDescriptorSet(image),auxiliary[image]);
    }
    EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);EXPECT_EQ(manager->materialCount(),runtimeCount);
    EXPECT_EQ(manager->buildSceneExport(graph),before);
    auto moved=std::move(stage);EXPECT_FALSE(stage);EXPECT_TRUE(moved);
  }
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->geometryRevision(),geometry);
  EXPECT_EQ(graph.revision(),graphRevision);EXPECT_EQ(manager->vertices().data(),vertices);EXPECT_EQ(manager->indices().data(),indices);
  EXPECT_EQ(manager->animationSource().images.data(),images);EXPECT_EQ(manager->animationSource().images[0].image.data(),pixels);
  EXPECT_EQ(manager->gltfTextureResources().color,roles.color);EXPECT_EQ(manager->gltfTextureResources().data,roles.data);
}

TEST_F(SceneMergeGenerationGpu,MaterialAppendPublishesNoncontiguousSourceRuntimeIdentityWithoutAllocatingOrBinding) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);
  const auto manual=manager->createSolidMaterial({.7f,.1f,.3f,1});manager->uploadMaterialResources();
  manager->updateDescriptorSets(frames.cameras,frames.objects);manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto baseline=allocations();const auto runtimeCount=manager->materialCount();
  const auto sourceCount=manager->animationSource().materials.size();
  const std::vector<uint32_t> oldMap(manager->sourceMaterialRuntimeMapping().begin(),manager->sourceMaterialRuntimeMapping().end());
  ASSERT_EQ(oldMap.size(),1u);ASSERT_GT(manual,oldMap[0]);ASSERT_EQ(manager->sourceMaterialForRuntime(manual),-1);
  ASSERT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),manager->animationSource().materials[0]));
  const auto original=manager->authoredMaterial(0);const auto roles=manager->gltfTextureResources();
  const auto* vertices=manager->vertices().data();const auto* indices=manager->indices().data();
  const auto* images=manager->animationSource().images.data();const auto* pixels=manager->animationSource().images[0].image.data();
  const auto* sourceBytes=manager->animationSource().buffers[0].data.data();
  const auto geometry=manager->geometryRevision(),graphRevision=graph.revision(),epoch=manager->generationEpoch(),revision=manager->materialRevision();
  const auto oldBinding=graph.getNode(primitiveNode(sourceNode(0)))->materialIndex;
  GltfMaterialPatch patch;patch.name="Materialized appended source";patch.doubleSided=true;patch.alphaMode=GltfMaterialAlphaMode::Blend;
  patch.values={{GltfMaterialProperty::BaseColor,std::array<double,4>{.2,.6,.9,.5}}};
  const auto command=PlanGltfMaterialAppend(manager->animationSource(),manager->animationSource(),original,original,patch);
  ASSERT_EQ(command.kind,GltfMaterialPlanKind::Append);EXPECT_FALSE(command.sourceMaterial);
  {
    auto discarded=manager->prepareMaterialGeneration(command,nullptr);frames.bind(discarded);
    ASSERT_TRUE(manager->canExchangeMaterialGeneration(discarded,nullptr));EXPECT_EQ(allocations(),baseline+1);
    EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);EXPECT_EQ(manager->materialCount(),runtimeCount);
  }
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->generationEpoch(),epoch);
  auto stage=manager->prepareMaterialGeneration(command,nullptr);const auto runtime=stage.runtimeMaterial();
  ASSERT_EQ(runtime,runtimeCount);ASSERT_NE(runtime,uint32_t(command.destinationMaterial));
  frames.bind(stage);ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
  size_t attempted{};{
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMaterialGeneration(stage,nullptr);attempted=guard.allocations();
  }
  EXPECT_EQ(attempted,0u);EXPECT_FALSE(stage);EXPECT_EQ(manager->materialCount(),runtimeCount+1);
  ASSERT_EQ(manager->animationSource().materials.size(),sourceCount+1);
  ASSERT_EQ(manager->sourceMaterialRuntimeMapping().size(),sourceCount+1);
  EXPECT_EQ(manager->sourceMaterialRuntimeMapping()[0],oldMap[0]);EXPECT_EQ(manager->sourceMaterialRuntimeMapping().back(),runtime);
  EXPECT_EQ(manager->sourceMaterialForRuntime(manual),-1);EXPECT_EQ(manager->sourceMaterialForRuntime(runtime),int32_t(sourceCount));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),original));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(int32_t(sourceCount)),command.authoredAfter));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),command.evaluatedAfter));
  EXPECT_TRUE(manager->isMaterialTransparent(runtime));EXPECT_TRUE(manager->isMaterialDoubleSided(runtime));
  EXPECT_EQ(manager->resolveMaterialTextureIndex(runtime),roles.color[0]);
  EXPECT_EQ(graph.getNode(primitiveNode(sourceNode(0)))->materialIndex,oldBinding);
  EXPECT_EQ(graph.revision(),graphRevision);EXPECT_EQ(manager->geometryRevision(),geometry);
  EXPECT_EQ(manager->vertices().data(),vertices);EXPECT_EQ(manager->indices().data(),indices);
  EXPECT_EQ(manager->animationSource().images.data(),images);EXPECT_EQ(manager->animationSource().images[0].image.data(),pixels);
  EXPECT_EQ(manager->animationSource().buffers[0].data.data(),sourceBytes);
  EXPECT_EQ(manager->generationEpoch(),epoch+1);EXPECT_EQ(manager->materialRevision(),revision+1);
  EXPECT_EQ(manager->gltfTextureResources().color,roles.color);EXPECT_EQ(manager->gltfTextureResources().data,roles.data);
  const auto saved=manager->buildSceneExport(graph);ASSERT_LT(runtime,saved.materials.size());
  EXPECT_EQ(saved.materials[runtime].name,"Materialized appended source");EXPECT_EQ(saved.materials[runtime].alphaMode,"BLEND");
  stage={};EXPECT_EQ(allocations(),baseline);
}

TEST_F(SceneMergeGenerationGpu,VariantUndoRestoresSharedTangentsWithUnmappedInstance) {
  auto asset=triangle("Shared variant tangents",91);
  auto& source=asset.gltfModel;
  auto& primitive=source.meshes[0].primitives[0];
  primitive.attributes["NORMAL"]=cloneAccessor(source,std::array<float,9>{0,0,1,0,0,1,0,0,1},
      TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT,3);
  primitive.attributes["TEXCOORD_1"]=cloneAccessor(source,std::array<float,6>{0,0,0,1,1,0},
      TINYGLTF_TYPE_VEC2,TINYGLTF_COMPONENT_TYPE_FLOAT,3);
  source.materials.resize(3,source.materials[0]);
  source.materials[0].normalTexture.index=0;
  source.materials[0].normalTexture.texCoord=1;
  source.materials[1].normalTexture.index=0;
  source.nodes.push_back(source.nodes[0]);
  source.scenes[0].nodes.push_back(1);
  using V=tinygltf::Value;using O=V::Object;using A=V::Array;
  source.extensionsUsed.push_back("KHR_materials_variants");
  source.extensions["KHR_materials_variants"]=V(O{{"variants",V(A{V(O{{"name",V(std::string("UV0"))}})})}});
  primitive.extensions["KHR_materials_variants"]=V(O{{"mappings",V(A{V(O{{"material",V(1)},
      {"variants",V(A{V(0)})}})})}});
  loadSource(std::move(asset));

  const auto first=primitiveNode(sourceNode(0));
  const auto second=primitiveNode(sourceNode(1));
  ASSERT_LT(first,second);
  ASSERT_NE(second,SceneGraph::kInvalidNode);
  const auto runtime=graph.getNode(first)->primitiveIndex;
  ASSERT_EQ(runtime,graph.getNode(second)->primitiveIndex);
  const auto materials=manager->sourceMaterialRuntimeMapping();
  ASSERT_EQ(materials.size(),3u);
  graph.setMaterialIndex(second,materials[2]);
  ASSERT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));
  ASSERT_FALSE(manager->vertices().empty());
  const auto originalTangent=manager->vertices().front().tangent;

  const auto edit=manager->planMaterialVariant(graph,0);
  ASSERT_TRUE(manager->applyMaterialVariant(graph,edit));
  EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(0));
  EXPECT_NE(manager->vertices().front().tangent,originalTangent);
  EXPECT_EQ(graph.getNode(first)->materialIndex,materials[1]);
  EXPECT_EQ(graph.getNode(second)->materialIndex,materials[1]);

  ASSERT_TRUE(manager->applyMaterialVariant(graph,edit,false));
  EXPECT_EQ(graph.getNode(first)->materialIndex,materials[0]);
  EXPECT_EQ(graph.getNode(second)->materialIndex,materials[2]);
  EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));
  EXPECT_EQ(manager->vertices().front().tangent,originalTangent);

  auto conflictingEdit=edit;
  const auto secondAssignment=std::ranges::find_if(conflictingEdit.assignments,
      [second](const auto& assignment){return assignment.node==second;});
  ASSERT_NE(secondAssignment,conflictingEdit.assignments.end());
  secondAssignment->after=materials[0];
  const auto graphRevision=graph.revision();
  const auto geometryRevision=manager->geometryRevision();
  const auto materialRevision=manager->materialRevision();
  EXPECT_THROW(manager->applyMaterialVariant(graph,conflictingEdit),std::runtime_error);
  EXPECT_EQ(graph.revision(),graphRevision);
  EXPECT_EQ(manager->geometryRevision(),geometryRevision);
  EXPECT_EQ(manager->materialRevision(),materialRevision);
  EXPECT_EQ(manager->selectedMaterialVariant(),std::nullopt);
  EXPECT_EQ(graph.getNode(first)->materialIndex,materials[0]);
  EXPECT_EQ(graph.getNode(second)->materialIndex,materials[2]);
  EXPECT_EQ(manager->vertices().front().tangent,originalTangent);
}

TEST_F(SceneMergeGenerationGpu,MaterialDuplicateExtendsVariantAuthorityAndExportsBothExistingMappingsAndNewResource) {
  using namespace container::geometry::gltf;
  auto asset=triangle("Variant append",91);auto& source=asset.gltfModel;using V=tinygltf::Value;using O=V::Object;using A=V::Array;
  source.materials.push_back(source.materials[0]);source.materials[1].name="Existing blue variant";
  source.materials[1].pbrMetallicRoughness.baseColorFactor={.1,.2,.8,1};
  source.extensionsUsed.push_back("KHR_materials_variants");
  source.extensions["KHR_materials_variants"]=V(O{{"variants",V(A{V(O{{"name",V(std::string("Blue"))}})})}});
  source.meshes[0].primitives[0].extensions["KHR_materials_variants"]=V(O{{"mappings",V(A{V(O{
      {"material",V(1)},{"variants",V(A{V(0)})},{"extras",V(O{{"mappingTag",V(17)}})}})})}});
  loadSource(std::move(asset));MaterialFrames frames(*allocation);
  ASSERT_TRUE(manager->applyMaterialVariant(graph,manager->planMaterialVariant(graph,0)));
  const auto node=primitiveNode(sourceNode(0));const auto variantRuntime=graph.getNode(node)->materialIndex;
  const auto graphRevision=graph.revision(),geometry=manager->geometryRevision();
  GltfMaterialPatch patch;patch.name="Unused duplicate remains an asset";
  const auto command=manager->planMaterialDuplicate(0,patch);auto stage=manager->prepareMaterialGeneration(command,nullptr);
  const auto duplicateRuntime=stage.runtimeMaterial();frames.bind(stage);ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
  size_t attempted{};{
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMaterialGeneration(stage,nullptr);attempted=guard.allocations();
  }
  EXPECT_EQ(attempted,0u); // A root-bearing variant library must swap without map construction.
  EXPECT_EQ(manager->selectedMaterialVariant(),std::optional<size_t>(0));EXPECT_EQ(manager->materialVariants().materialCount(),3u);
  EXPECT_EQ(graph.getNode(node)->materialIndex,variantRuntime);EXPECT_EQ(graph.revision(),graphRevision);
  EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(manager->materialVariants().primitive(0,0).mappings,
      (std::vector<std::pair<size_t,int32_t>>{{0,1}}));
  const auto selected=manager->buildSceneExport(graph);ASSERT_LT(duplicateRuntime,selected.materials.size());
  EXPECT_EQ(selected.materials[duplicateRuntime].name,"Unused duplicate remains an asset");
  const auto parsed=ParseMaterialVariants(selected);ASSERT_TRUE(parsed.hasVariants());ASSERT_EQ(parsed.names().size(),1u);
  EXPECT_EQ(parsed.names()[0],"Blue");EXPECT_EQ(parsed.materialCount(),selected.materials.size());
  ASSERT_EQ(parsed.primitives().size(),1u);EXPECT_EQ(parsed.primitives()[0].defaultMaterial,int32_t(variantRuntime));
  EXPECT_EQ(parsed.primitives()[0].mappings,(std::vector<std::pair<size_t,int32_t>>{{0,int32_t(variantRuntime)}}));
  const auto& savedMapping=selected.meshes[0].primitives[0].extensions.at("KHR_materials_variants").Get("mappings").Get<A>()[0];
  ASSERT_TRUE(savedMapping.Has("extras"));EXPECT_EQ(savedMapping.Get("extras").Get("mappingTag").Get<int>(),17);
  ASSERT_TRUE(manager->applyMaterialVariant(graph,manager->planMaterialVariant(graph,std::nullopt)));
  EXPECT_EQ(graph.getNode(node)->materialIndex,manager->sourceMaterialRuntimeMapping()[0]);
  ASSERT_TRUE(manager->applyMaterialVariant(graph,manager->planMaterialVariant(graph,0)));
  EXPECT_EQ(graph.getNode(node)->materialIndex,variantRuntime);
  // Binding is an external graph operation: the prepared resource adds no
  // assignment. An explicit later binding must export that exact runtime row.
  graph.setMaterialIndex(node,duplicateRuntime);const auto rebound=manager->buildSceneExport(graph);
  const auto bound=std::ranges::find_if(rebound.meshes,[](const auto& mesh){return !mesh.primitives.empty();});
  ASSERT_NE(bound,rebound.meshes.end());EXPECT_EQ(bound->primitives[0].material,int(duplicateRuntime));
}

TEST_F(SceneMergeGenerationGpu,AnimatedMaterialDuplicateKeepsSeparateRestAndVisibleValuesWithoutCloningTracks) {
  using namespace container::geometry::gltf;
  loadSource(animatedMaterialAppendAsset());MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.5));
  manager->publishAnimationChanges(dirty,graph);player.setLoop(false);player.setSpeed(.5);player.play();
  const auto originalRest=player.library().restMaterials[0];const auto visible=manager->animationSource().materials[0];
  const auto clips=manager->animationSource().animations;const auto node=manager->animationSource().nodes[0];
  const auto* bytes=manager->animationSource().buffers[0].data.data();const auto sourceCount=manager->animationSource().materials.size();
  GltfMaterialPatch patch;patch.name="Independent untracked duplicate";
  const auto command=manager->planMaterialDuplicate(0,patch);
  EXPECT_DOUBLE_EQ(command.authoredAfter.pbrMetallicRoughness.roughnessFactor,.35);
  EXPECT_NEAR(command.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.3,1e-6);
  auto stage=manager->prepareMaterialGeneration(command,&player);const auto runtime=stage.runtimeMaterial();frames.bind(stage);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  size_t attempted{};{
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMaterialGeneration(stage,&player);attempted=guard.allocations();
  }
  EXPECT_EQ(attempted,0u);EXPECT_EQ(player.selectedClip(),std::optional<size_t>(0));EXPECT_DOUBLE_EQ(player.time(),.5);
  EXPECT_DOUBLE_EQ(player.speed(),.5);EXPECT_TRUE(player.playing());EXPECT_FALSE(player.loop());
  ASSERT_EQ(player.library().restMaterials.size(),sourceCount+1);
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[0],originalRest));
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials.back(),command.authoredAfter));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),command.evaluatedAfter));
  EXPECT_EQ(manager->animationSource().animations,clips);EXPECT_EQ(manager->animationSource().nodes[0],node);
  EXPECT_EQ(manager->animationSource().buffers[0].data.data(),bytes);
  EXPECT_EQ(player.library().clips[0].tracks.size(),2u);EXPECT_EQ(manager->animationSource().animations[0].channels.size(),2u);
  dirty=player.seek(manager->animationSource(),1.5);manager->publishAnimationChanges(dirty,graph);
  EXPECT_NEAR(manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor,.7,1e-6);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),command.evaluatedAfter));
  EXPECT_NEAR(manager->resolveMaterialMetallicRoughnessFactors(runtime).y,.3,1e-6);
  dirty=player.clearClip(manager->animationSource());manager->publishAnimationChanges(dirty,graph);
  // Rest restore touches animation targets only; the new untracked duplicate
  // retains its independent visible value while its authored rest stays .35.
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),command.evaluatedAfter));
  EXPECT_DOUBLE_EQ(player.library().restMaterials.back().pbrMetallicRoughness.roughnessFactor,.35);
  EXPECT_EQ(manager->animationSource().animations,clips);EXPECT_TRUE(GltfMaterialRecordsEqual(originalRest,manager->authoredMaterial(0)));
  EXPECT_NE(visible.pbrMetallicRoughness.roughnessFactor,originalRest.pbrMetallicRoughness.roughnessFactor);
}

TEST_F(SceneMergeGenerationGpu,MaterialMakeUniqueFreezesVisibleRestAndStaysUntrackedAcrossSeekAndRestRestore) {
  using namespace container::geometry::gltf;
  loadSource(animatedMaterialAppendAsset());MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.5));
  manager->publishAnimationChanges(dirty,graph);const auto visible=manager->animationSource().materials[0];
  const auto clips=manager->animationSource().animations;const auto oldRest=player.library().restMaterials[0];
  const auto stale=manager->planMaterialMakeUnique(0);
  dirty=player.seek(manager->animationSource(),1.5);manager->publishAnimationChanges(dirty,graph);
  EXPECT_THROW((void)manager->prepareMaterialGeneration(stale,&player),std::invalid_argument);
  dirty=player.seek(manager->animationSource(),.5);manager->publishAnimationChanges(dirty,graph);
  const auto command=manager->planMaterialMakeUnique(0);
  EXPECT_EQ(command.kind,GltfMaterialPlanKind::MakeUnique);EXPECT_EQ(command.sourceMaterial,std::optional<int32_t>(0));
  EXPECT_TRUE(GltfMaterialRecordsEqual(command.authoredAfter,visible));EXPECT_TRUE(GltfMaterialRecordsEqual(command.evaluatedAfter,visible));
  auto stage=manager->prepareMaterialGeneration(command,&player);frames.bind(stage);ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  manager->exchangeMaterialGeneration(stage,&player);EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials.back(),visible));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(command.destinationMaterial),visible));
  dirty=player.seek(manager->animationSource(),1.5);manager->publishAnimationChanges(dirty,graph);
  EXPECT_NEAR(manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor,.7,1e-6);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),visible));
  dirty=player.clearClip(manager->animationSource());manager->publishAnimationChanges(dirty,graph);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),visible));
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[0],oldRest));EXPECT_EQ(manager->animationSource().animations,clips);
}

TEST_F(SceneMergeGenerationGpu,MaterialAppendRejectsForgedPlansAndEveryGrowthBudgetBeforeChangingLiveResources) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);const auto before=manager->buildSceneExport(graph);const auto original=manager->authoredMaterial(0);
  const auto baseline=allocations();const auto count=manager->materialCount(),epoch=manager->generationEpoch();
  const auto command=manager->planMaterialDuplicate(0);
  const auto reject=[&](const GltfMaterialAuthoringPlan& forged) {
    EXPECT_THROW((void)manager->prepareMaterialGeneration(forged,nullptr),std::invalid_argument);
    EXPECT_EQ(allocations(),baseline);
  };
  auto forged=command;forged.destinationMaterial+=1;reject(forged);
  forged=command;forged.sourceMaterial=99;reject(forged);
  forged=command;forged.kind=GltfMaterialPlanKind::Append;reject(forged);
  forged=command;forged.authoredAfter.extras=tinygltf::Value(tinygltf::Value::Object{{"forged",tinygltf::Value(true)}});reject(forged);
  forged=command;forged.evaluatedAfter.pbrMetallicRoughness.baseColorTexture.index=99;reject(forged);
  forged=command;forged.extensionsToDeclare.push_back("KHR_materials_unlit");reject(forged);
  reject(GltfMaterialAuthoringPlan{});
  const auto rejectBudget=[&](const MaterialGenerationLimits& limits) {
    EXPECT_THROW((void)manager->prepareMaterialGeneration(command,nullptr,limits),std::invalid_argument);
    EXPECT_EQ(allocations(),baseline);
  };
  MaterialGenerationLimits limits;limits.maxMaterialCount=count;rejectBudget(limits);
  limits={};limits.maxPreparedCpuBytes=1;rejectBudget(limits);
  limits={};limits.maxMaterialBufferBytes=count*sizeof(GpuMaterial);rejectBudget(limits);
  limits={};limits.variantGrowth.maxMaterials=manager->animationSource().materials.size();
  EXPECT_THROW((void)manager->prepareMaterialGeneration(command,nullptr,limits),std::runtime_error);
  EXPECT_EQ(allocations(),baseline);
  limits={};limits.variantGrowth.maxOwnedBytes=1;
  EXPECT_THROW((void)manager->prepareMaterialGeneration(command,nullptr,limits),std::runtime_error);
  EXPECT_EQ(allocations(),baseline);
  auto stage=manager->prepareMaterialGeneration(command,nullptr);
  auto cameras=frames.cameras;cameras[1].buffer=VK_NULL_HANDLE;
  EXPECT_THROW(stage.prepareDescriptorFrames(cameras,frames.objects),std::invalid_argument);
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));stage={};
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialCount(),count);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),original));EXPECT_EQ(manager->buildSceneExport(graph),before);
}

TEST_F(SceneMergeGenerationGpu,MaterialAppendProofRejectsUnselectedPrefixUlpTypesAndCompetingTailPublication) {
  using namespace container::geometry::gltf;using V=tinygltf::Value;
  auto asset=triangle("Prefix witness",81);asset.gltfModel.materials.push_back(asset.gltfModel.materials[0]);
  asset.gltfModel.materials[1].extras=V(V::Object{{"typed",V(1)}});loadSource(std::move(asset));MaterialFrames frames(*allocation);
  const auto command=manager->planMaterialDuplicate(0);auto stage=manager->prepareMaterialGeneration(command,nullptr);frames.bind(stage);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
  auto& prefix=manager->animationSource().materials[1];const auto before=prefix;
  prefix.pbrMetallicRoughness.roughnessFactor=std::nextafter(prefix.pbrMetallicRoughness.roughnessFactor,0.);
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));prefix=before;
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));prefix.extras.Get<V::Object>()["typed"]=V(1.0);
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));prefix=before;
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
  auto competing=manager->prepareMaterialGeneration(command,nullptr);frames.bind(competing);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(competing,nullptr));const auto sourceCount=manager->animationSource().materials.size();
  manager->exchangeMaterialGeneration(competing,nullptr);EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));
  const auto count=manager->materialCount(),epoch=manager->generationEpoch();manager->exchangeMaterialGeneration(stage,nullptr);
  EXPECT_EQ(manager->materialCount(),count);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(manager->animationSource().materials.size(),sourceCount+1);
  EXPECT_THROW((void)manager->prepareMaterialGeneration(command,nullptr),std::invalid_argument);
}

// Staging/validation may allocate. Only the final exchange is under the actual
// host-allocation denial guard; no rendering or GPU readback is implied here.
bool publishMaterialTestGeneration(SceneManager& manager,MaterialFrames& frames,
    SceneManager::PreparedMaterialGeneration& stage,container::geometry::gltf::AnimationPlayer* player=nullptr) {
  if(!stage) return false;
  if(stage.descriptorSet(0)==VK_NULL_HANDLE) frames.bind(stage);
  if(!manager.canExchangeMaterialGeneration(stage,player)) return false;
  size_t attempted{};
  {
    material_publication_test::AllocationGuard guard(true);
    manager.exchangeMaterialGeneration(stage,player);attempted=guard.allocations();
  }
  return !stage && attempted==0;
}

TEST_F(SceneMergeGenerationGpu,MaterialTailRejectsUnpublishedDiscardedForeignAndMissingAuthority) {
  load();MaterialFrames frames(*allocation);const auto base=allocations();
  const auto before=manager->buildSceneExport(graph);const auto count=manager->materialCount();const auto nodes=graph.nodeCount();
  SceneManager::MaterialAppendCommand abandoned;
  {
    auto staged=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);
    abandoned=staged.appendCommand();ASSERT_TRUE(abandoned);EXPECT_FALSE(abandoned.published());EXPECT_FALSE(abandoned.present());
    EXPECT_GT(abandoned.ownedBytes(),0u);frames.bind(staged);
    EXPECT_THROW((void)manager->prepareMaterialTailChange(abandoned,MaterialTailState::Detached,graph,nullptr),std::invalid_argument);
    EXPECT_THROW((void)manager->prepareMaterialTailChange(abandoned,MaterialTailState::Present,graph,nullptr),std::invalid_argument);
  }
  EXPECT_EQ(allocations(),base);EXPECT_FALSE(abandoned.published());EXPECT_EQ(manager->buildSceneExport(graph),before);
  EXPECT_THROW((void)manager->prepareMaterialTailChange(abandoned,MaterialTailState::Present,graph,nullptr),std::invalid_argument);
  EXPECT_THROW((void)manager->prepareMaterialTailChange({},MaterialTailState::Detached,graph,nullptr),std::invalid_argument);
  auto stage=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto command=stage.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,stage));stage={};
  auto foreign=manager->prepareMergeGeneration(triangle("Foreign candidate",51),temp/"foreign.glb",{},graph);
  EXPECT_THROW((void)foreign.manager().prepareMaterialTailChange(command,MaterialTailState::Detached,foreign.graph(),nullptr),std::invalid_argument);
  EXPECT_TRUE(command.published());EXPECT_TRUE(command.present());EXPECT_EQ(manager->materialCount(),count+1);
  EXPECT_EQ(graph.nodeCount(),nodes);
  EXPECT_EQ(manager->animationSource().meshes[0].primitives[0].material,0);
}

TEST_F(SceneMergeGenerationGpu,MaterialTailSparseIdsDetachRestoreAllFrameDescriptorsWithoutAllocatingOrChangingGeometry) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);
  const auto manual=manager->createSolidMaterial({.3f,.6f,.2f,1});manager->uploadMaterialResources();
  manager->updateDescriptorSets(frames.cameras,frames.objects);manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto base=allocations();const auto sourceCount=manager->animationSource().materials.size(),runtimeCount=manager->materialCount();
  const auto* vertices=manager->vertices().data();const auto* indices=manager->indices().data();
  const auto* pixels=manager->animationSource().images[0].image.data();const auto* bytes=manager->animationSource().buffers[0].data.data();
  const auto geometry=manager->geometryRevision(),graphRevision=graph.revision();const auto graphIdentity=graph.identity();
  const auto plan=manager->planMaterialDuplicate(0);auto initial=manager->prepareMaterialGeneration(plan,nullptr);
  auto command=initial.appendCommand();ASSERT_EQ(command.sourceMaterial(),int32_t(sourceCount));ASSERT_EQ(command.runtimeMaterial(),runtimeCount);
  ASSERT_NE(uint32_t(command.sourceMaterial()),command.runtimeMaterial());ASSERT_GT(manual,manager->sourceMaterialRuntimeMapping()[0]);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,initial));initial={};EXPECT_EQ(allocations(),base);
  auto epoch=manager->generationEpoch(),revision=manager->materialRevision();
  auto detach=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr);frames.bind(detach);
  ASSERT_EQ(detach.gpuMaterials().size(),runtimeCount);ASSERT_EQ(detach.materialProperties().size(),runtimeCount);
  std::array<VkDescriptorSet,2> detachedPrimary{},detachedAuxiliary{};
  for(uint32_t frame=0;frame<2;++frame) {
    detachedPrimary[frame]=detach.descriptorSet(frame);detachedAuxiliary[frame]=detach.auxiliaryDescriptorSet(frame);
    EXPECT_NE(detachedPrimary[frame],VK_NULL_HANDLE);EXPECT_NE(detachedAuxiliary[frame],VK_NULL_HANDLE);
    EXPECT_NE(detachedPrimary[frame],manager->descriptorSet(frame));
  }
  VmaAllocationInfo info{};vmaGetAllocationInfo(allocation->memoryManager()->allocator(),detach.materialBuffer().allocation,&info);
  ASSERT_NE(info.pMappedData,nullptr);
  EXPECT_EQ(std::memcmp(info.pMappedData,detach.gpuMaterials().data(),detach.gpuMaterials().size_bytes()),0); // Mapped upload cache, not GPU readback.
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,detach));detach={};
  EXPECT_TRUE(command.published());EXPECT_FALSE(command.present());EXPECT_EQ(manager->materialCount(),runtimeCount);
  EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);EXPECT_EQ(manager->sourceMaterialForRuntime(manual),-1);
  EXPECT_GT(manager->generationEpoch(),epoch);EXPECT_GT(manager->materialRevision(),revision);EXPECT_EQ(allocations(),base);
  for(uint32_t frame=0;frame<2;++frame) {EXPECT_EQ(manager->descriptorSet(frame),detachedPrimary[frame]);EXPECT_EQ(manager->auxiliaryDescriptorSet(frame),detachedAuxiliary[frame]);}
  epoch=manager->generationEpoch();revision=manager->materialRevision();
  auto restore=manager->prepareMaterialTailChange(command,MaterialTailState::Present,graph,nullptr);frames.bind(restore);
  ASSERT_EQ(restore.sourceMaterial(),int32_t(sourceCount));ASSERT_EQ(restore.runtimeMaterial(),runtimeCount);
  ASSERT_EQ(restore.gpuMaterials().size(),runtimeCount+1);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,restore));restore={};
  EXPECT_TRUE(command.present());EXPECT_EQ(manager->sourceMaterialRuntimeMapping().back(),runtimeCount);
  EXPECT_EQ(manager->sourceMaterialForRuntime(runtimeCount),int32_t(sourceCount));EXPECT_EQ(manager->sourceMaterialForRuntime(manual),-1);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(int32_t(sourceCount)),plan.authoredAfter));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),plan.evaluatedAfter));
  EXPECT_GT(manager->generationEpoch(),epoch);EXPECT_GT(manager->materialRevision(),revision);EXPECT_EQ(allocations(),base);
  EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(graph.revision(),graphRevision);EXPECT_EQ(graph.identity(),graphIdentity);
  EXPECT_EQ(manager->vertices().data(),vertices);EXPECT_EQ(manager->indices().data(),indices);
  EXPECT_EQ(manager->animationSource().images[0].image.data(),pixels);EXPECT_EQ(manager->animationSource().buffers[0].data.data(),bytes);
}

TEST_F(SceneMergeGenerationGpu,MaterialTailReplayUsesCurrentAnimatedPrefixRestAndControlsInsteadOfCreationTimeSnapshots) {
  using namespace container::geometry::gltf;
  loadSource(animatedMaterialAppendAsset());MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.5));
  manager->publishAnimationChanges(dirty,graph);player.setLoop(false);player.setSpeed(.75);player.play();
  const auto plan=manager->planMaterialDuplicate(0);auto initial=manager->prepareMaterialGeneration(plan,&player);auto command=initial.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,initial,&player));initial={};
  dirty=player.seek(manager->animationSource(),1.25);manager->publishAnimationChanges(dirty,graph);
  const auto beforeDetach=manager->animationSource().nodes[0];const auto clips=manager->animationSource().animations;
  auto detach=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,&player);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,detach,&player));detach={};
  EXPECT_DOUBLE_EQ(player.time(),1.25);EXPECT_TRUE(player.playing());EXPECT_FALSE(player.loop());EXPECT_DOUBLE_EQ(player.speed(),.75);
  EXPECT_EQ(manager->animationSource().nodes[0],beforeDetach);EXPECT_EQ(player.library().restMaterials.size(),1u);
  EXPECT_NEAR(manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor,.6,1e-6);
  dirty=player.seek(manager->animationSource(),1.75);manager->publishAnimationChanges(dirty,graph);
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Metallic,.63}};
  auto prefixEdit=manager->prepareMaterialGeneration(manager->planMaterialEdit(0,patch),&player);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,prefixEdit,&player));prefixEdit={};
  const auto current=manager->animationSource().materials[0];const auto currentNode=manager->animationSource().nodes[0];
  const auto currentRest=player.library().restMaterials[0];const auto geometry=manager->geometryRevision();
  auto restore=manager->prepareMaterialTailChange(command,MaterialTailState::Present,graph,&player);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,restore,&player));restore={};
  EXPECT_DOUBLE_EQ(player.time(),1.75);EXPECT_TRUE(player.playing());EXPECT_FALSE(player.loop());EXPECT_DOUBLE_EQ(player.speed(),.75);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[0],current));
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[0],currentRest));
  EXPECT_DOUBLE_EQ(player.library().restMaterials[0].pbrMetallicRoughness.metallicFactor,.63);
  EXPECT_EQ(manager->animationSource().nodes[0],currentNode);EXPECT_EQ(manager->geometryRevision(),geometry);
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials.back(),plan.authoredAfter));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),plan.evaluatedAfter));
  EXPECT_NEAR(manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor,.8,1e-6);
  EXPECT_NEAR(manager->animationSource().materials.back().pbrMetallicRoughness.roughnessFactor,.3,1e-6);
  EXPECT_EQ(manager->animationSource().animations,clips);
}

TEST_F(SceneMergeGenerationGpu,MaterialTailRootedVariantsSurviveAllocationDeniedShrinkAndRestoreWithCurrentSelection) {
  using namespace container::geometry::gltf;using V=tinygltf::Value;using O=V::Object;using A=V::Array;
  auto asset=triangle("Variant tail",91);auto& source=asset.gltfModel;
  source.materials.push_back(source.materials[0]);source.materials[1].pbrMetallicRoughness.baseColorFactor={.1,.2,.8,1};
  source.extensionsUsed.push_back("KHR_materials_variants");
  source.extensions["KHR_materials_variants"]=V(O{{"variants",V(A{V(O{{"name",V(std::string("Blue"))}})})}});
  source.meshes[0].primitives[0].extensions["KHR_materials_variants"]=V(O{{"mappings",V(A{V(O{
      {"material",V(1)},{"variants",V(A{V(0)})},{"extras",V(O{{"mappingIdentity",V(17)}})}})})}});
  loadSource(std::move(asset));MaterialFrames frames(*allocation);
  auto initial=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto command=initial.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,initial));initial={};
  ASSERT_TRUE(manager->applyMaterialVariant(graph,manager->planMaterialVariant(graph,0)));
  const auto node=primitiveNode(sourceNode(0));const auto binding=graph.getNode(node)->materialIndex;
  const auto rawRoot=manager->animationSource().extensions.at("KHR_materials_variants");
  const auto rawPrimitive=manager->animationSource().meshes[0].primitives[0].extensions.at("KHR_materials_variants");
  auto detach=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,detach));detach={};
  EXPECT_EQ(manager->materialVariants().materialCount(),2u);EXPECT_EQ(manager->selectedMaterialVariant(),std::optional<size_t>(0));
  EXPECT_EQ(graph.getNode(node)->materialIndex,binding);
  auto restore=manager->prepareMaterialTailChange(command,MaterialTailState::Present,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,restore));restore={};
  EXPECT_EQ(manager->materialVariants().materialCount(),3u);EXPECT_EQ(manager->selectedMaterialVariant(),std::optional<size_t>(0));
  EXPECT_EQ(graph.getNode(node)->materialIndex,binding);
  EXPECT_EQ(manager->animationSource().extensions.at("KHR_materials_variants"),rawRoot);
  EXPECT_EQ(manager->animationSource().meshes[0].primitives[0].extensions.at("KHR_materials_variants"),rawPrimitive);
  const auto exported=manager->buildSceneExport(graph);const auto parsed=ParseMaterialVariants(exported);
  ASSERT_TRUE(parsed.hasVariants());EXPECT_EQ(parsed.names()[0],"Blue");
  ASSERT_TRUE(manager->applyMaterialVariant(graph,manager->planMaterialVariant(graph,std::nullopt)));
  EXPECT_EQ(graph.getNode(node)->materialIndex,manager->sourceMaterialRuntimeMapping()[0]);
}

TEST_F(SceneMergeGenerationGpu,MaterialTailAuditRejectsInactiveAndRetiredGraphReferencesWithoutLosingOwnership) {
  load(false,true);MaterialFrames frames(*allocation);const auto owner=sourceNode(0),node=primitiveNode(owner);
  auto initial=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto command=initial.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,initial));initial={};
  const auto old=graph.getNode(node)->materialIndex;graph.setMaterialIndex(node,command.runtimeMaterial());
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,1));ASSERT_FALSE(graph.isNodeActive(node));
  const auto epoch=manager->generationEpoch(),count=manager->materialCount();const auto base=allocations();
  EXPECT_THROW((void)manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr),std::invalid_argument);
  EXPECT_EQ(allocations(),base);EXPECT_TRUE(command.present());EXPECT_EQ(manager->generationEpoch(),epoch);
  manager->applySceneSelection(graph,manager->planSceneSelection(graph,0));
  auto retirement=graph.prepareRetireSubtree(owner);ASSERT_TRUE(retirement);ASSERT_TRUE(graph.publishRetirement(retirement));
  auto retired=retirement.takeRetirement();ASSERT_TRUE(retired);ASSERT_TRUE(graph.isNodeRetired(node));
  EXPECT_THROW((void)manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr),std::invalid_argument);
  EXPECT_EQ(manager->materialCount(),count);EXPECT_TRUE(command.present());
  auto revive=graph.prepareRetirementChange(*retired,false);ASSERT_TRUE(revive);ASSERT_TRUE(graph.publishRetirement(revive));
  graph.setMaterialIndex(node,old);
  auto detach=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,detach));EXPECT_FALSE(command.present());
}

TEST_F(SceneMergeGenerationGpu,MaterialTailRawPointerBudgetsAndStaleGraphProofFailBeforeAnyPublication) {
  using namespace container::geometry::gltf;using V=tinygltf::Value;
  loadSource(animatedMaterialAppendAsset());MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto initial=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),&player);auto command=initial.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,initial,&player));initial={};
  const auto base=allocations();const auto epoch=manager->generationEpoch(),count=manager->materialCount();
  auto& channel=manager->animationSource().animations[0].channels[0];const auto old=channel.target_extensions;
  channel.target_extensions["KHR_animation_pointer"].Get<V::Object>()["pointer"]=
      V(std::string("/materials/")+std::to_string(command.sourceMaterial())+"/future~1factor");
  bool rejectedByAudit=false;
  try {(void)manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,&player);}
  catch(const std::invalid_argument& error) {rejectedByAudit=std::string(error.what()).find("Material tail audit:")!=std::string::npos;}
  EXPECT_TRUE(rejectedByAudit);channel.target_extensions=old;
  MaterialGenerationLimits limits;limits.maxPreparedCpuBytes=1;
  EXPECT_THROW((void)manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,&player,nullptr,limits),std::invalid_argument);
  limits={};limits.references.maxGraphRows=1;
  EXPECT_THROW((void)manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,&player,nullptr,limits),std::invalid_argument);
  limits={};limits.variantGrowth.maxOwnedBytes=1;
  EXPECT_THROW((void)manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,&player,nullptr,limits),std::runtime_error);
  EXPECT_EQ(allocations(),base);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialCount(),count);EXPECT_TRUE(command.present());
  auto stale=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,&player);frames.bind(stale);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stale,&player));graph.setName(sourceNode(0),"Changed after reference audit");
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stale,&player));manager->exchangeMaterialGeneration(stale,&player);
  EXPECT_TRUE(stale);EXPECT_TRUE(command.present());EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialCount(),count);
}

TEST_F(SceneMergeGenerationGpu,MaterialTailProspectiveBindingUndoRedoPublishesGraphBeforeTablesWithoutAllocating) {
  load();MaterialFrames frames(*allocation);const auto node=primitiveNode(sourceNode(0));const auto old=graph.getNode(node)->materialIndex;
  auto initial=manager->prepareMaterialGeneration(manager->planMaterialMakeUnique(0),nullptr);auto command=initial.appendCommand();
  const MaterialBindingContext before{manager->materialAssetIdentity(),manager->sourceMaterialRuntimeMapping(),uint32_t(manager->materialCount()),uint32_t(manager->primitiveRanges().size())};
  const std::array requests{MaterialBindingRequest{node,old,command.runtimeMaterial()}};
  const auto bindingCommand=PlanMaterialBindings(graph,before,requests,
      MaterialBindingAppendTarget{uint32_t(command.sourceMaterial()),command.runtimeMaterial()});
  auto bind=PrepareMaterialBindings(graph,before,bindingCommand);frames.bind(initial);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(initial,nullptr));ASSERT_TRUE(CanPublishMaterialBindings(graph,bind,before));
  size_t attempts{};bool bound{};
  {
    material_publication_test::AllocationGuard guard(true);
    bound=PublishMaterialBindings(graph,bind,before);if(bound) manager->exchangeMaterialGeneration(initial,nullptr);attempts=guard.allocations();
  }
  ASSERT_TRUE(bound);EXPECT_EQ(attempts,0u);ASSERT_FALSE(initial);initial={};EXPECT_EQ(graph.getNode(node)->materialIndex,command.runtimeMaterial());
  const MaterialBindingContext present{manager->materialAssetIdentity(),manager->sourceMaterialRuntimeMapping(),uint32_t(manager->materialCount()),uint32_t(manager->primitiveRanges().size())};
  EXPECT_THROW((void)manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr),std::invalid_argument);
  auto undoBinding=RebaseMaterialBindings(graph,present,bindingCommand,MaterialBindingSide::Before);
  auto detach=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr,&undoBinding);frames.bind(detach);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(detach,nullptr));ASSERT_TRUE(CanPublishMaterialBindings(graph,undoBinding,present));
  {
    material_publication_test::AllocationGuard guard(true);
    bound=PublishMaterialBindings(graph,undoBinding,present);if(bound) manager->exchangeMaterialGeneration(detach,nullptr);attempts=guard.allocations();
  }
  ASSERT_TRUE(bound);EXPECT_EQ(attempts,0u);ASSERT_FALSE(detach);detach={};EXPECT_FALSE(command.present());EXPECT_EQ(graph.getNode(node)->materialIndex,old);
  const MaterialBindingContext absent{manager->materialAssetIdentity(),manager->sourceMaterialRuntimeMapping(),uint32_t(manager->materialCount()),uint32_t(manager->primitiveRanges().size())};
  auto redoBinding=RebaseMaterialBindings(graph,absent,bindingCommand,MaterialBindingSide::After);
  auto restore=manager->prepareMaterialTailChange(command,MaterialTailState::Present,graph,nullptr,&redoBinding);frames.bind(restore);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(restore,nullptr));ASSERT_TRUE(CanPublishMaterialBindings(graph,redoBinding,absent));
  {
    material_publication_test::AllocationGuard guard(true);
    bound=PublishMaterialBindings(graph,redoBinding,absent);if(bound) manager->exchangeMaterialGeneration(restore,nullptr);attempts=guard.allocations();
  }
  ASSERT_TRUE(bound);EXPECT_EQ(attempts,0u);ASSERT_FALSE(restore);EXPECT_TRUE(command.present());
  EXPECT_EQ(graph.getNode(node)->materialIndex,command.runtimeMaterial());
}

TEST_F(SceneMergeGenerationGpu,MaterialTailAuthoritySurvivesLaterMergeAndCloneUndoWithDetachedRedoPayloads) {
  load();MaterialFrames frames(*allocation);
  auto initial=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto command=initial.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,initial));initial={};
  for(const bool clone:{false,true}) {
    SCOPED_TRACE(clone);const auto first=uint32_t(graph.nodeCount());const auto oldView=graph.activeRoots();
    auto later=clone?manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"Later copied source"):
        manager->prepareMergeGeneration(triangle("Later merged source",77),temp/"later.glb",{},graph);
    auto append=graph.prepareAppend(later.graph());ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));exchangeMerge(later);
    if(clone) {
      auto candidate=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr);frames.bind(candidate);
      EXPECT_TRUE(manager->canExchangeMaterialGeneration(candidate,nullptr)); // Cookie survived the clone payload.
    } else {
      EXPECT_THROW((void)manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr),std::invalid_argument);
    }
    auto undoLater=graph.prepareDetachTail(first,oldView);ASSERT_TRUE(undoLater);ASSERT_TRUE(graph.publishTailChange(undoLater));
    auto historyTail=undoLater.takeDetachedTail();ASSERT_TRUE(historyTail);exchangeMerge(later);
    const auto source=command.sourceMaterial();const auto runtime=command.runtimeMaterial();
    auto detach=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr);
    ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,detach));detach={};
    EXPECT_FALSE(command.present()); // later/historyTail intentionally remain alive as redo-only history.
    auto restore=manager->prepareMaterialTailChange(command,MaterialTailState::Present,graph,nullptr);
    ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,restore));restore={};
    EXPECT_TRUE(command.present());EXPECT_EQ(command.sourceMaterial(),source);EXPECT_EQ(command.runtimeMaterial(),runtime);
  }
}

TEST_F(SceneMergeGenerationGpu,MaterialTailSequentialLifoRestoresVacanciesAndRejectsSameIdReplacementBranches) {
  load();MaterialFrames frames(*allocation);
  auto first=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto a=first.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,first));first={};
  auto second=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto b=second.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,second));second={};
  EXPECT_EQ(b.sourceMaterial(),a.sourceMaterial()+1);EXPECT_EQ(b.runtimeMaterial(),a.runtimeMaterial()+1);
  EXPECT_THROW((void)manager->prepareMaterialTailChange(a,MaterialTailState::Detached,graph,nullptr),std::invalid_argument);
  auto undoB=manager->prepareMaterialTailChange(b,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,undoB));undoB={};
  auto undoA=manager->prepareMaterialTailChange(a,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,undoA));undoA={};
  auto redoA=manager->prepareMaterialTailChange(a,MaterialTailState::Present,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,redoA));redoA={};
  auto redoB=manager->prepareMaterialTailChange(b,MaterialTailState::Present,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,redoB));redoB={};EXPECT_TRUE(a.present());EXPECT_TRUE(b.present());
  undoB=manager->prepareMaterialTailChange(b,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,undoB));undoB={};
  auto replacement=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);auto c=replacement.appendCommand();
  ASSERT_EQ(c.sourceMaterial(),b.sourceMaterial());ASSERT_EQ(c.runtimeMaterial(),b.runtimeMaterial());
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,replacement));replacement={};
  EXPECT_THROW((void)manager->prepareMaterialTailChange(b,MaterialTailState::Present,graph,nullptr),std::invalid_argument);
  auto undoC=manager->prepareMaterialTailChange(c,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,undoC));undoC={};
  EXPECT_FALSE(c.present());EXPECT_FALSE(b.present());
  EXPECT_THROW((void)manager->prepareMaterialTailChange(b,MaterialTailState::Present,graph,nullptr),std::invalid_argument);
  auto redoC=manager->prepareMaterialTailChange(c,MaterialTailState::Present,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,redoC));EXPECT_TRUE(c.present());EXPECT_TRUE(a.present());
}

TEST_F(SceneMergeGenerationGpu,MaterialTailPreparedPlayerRejectsPostStageRawCurveBitsAndSamplerChanges) {
  using namespace container::geometry::gltf;
  loadSource(animatedMaterialAppendAsset());MaterialFrames frames(*allocation);AnimationPlayer player(manager->animationSource());
  auto dirty=player.selectClip(manager->animationSource(),0);dirty.merge(player.seek(manager->animationSource(),.5));
  manager->publishAnimationChanges(dirty,graph);player.play();
  auto initial=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),&player);auto command=initial.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,initial,&player));initial={};
  auto stage=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,&player);frames.bind(stage);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  const auto epoch=manager->generationEpoch(),count=manager->materialCount();const auto time=player.time();
  auto& source=manager->animationSource();auto& sampler=source.animations[0].samplers[0];
  const auto& accessor=source.accessors[size_t(sampler.output)];const auto& view=source.bufferViews[size_t(accessor.bufferView)];
  auto* encoded=source.buffers[size_t(view.buffer)].data.data()+view.byteOffset+accessor.byteOffset;
  float original{};std::memcpy(&original,encoded,sizeof(original));const auto changed=std::nextafter(original,1.f);
  std::memcpy(encoded,&changed,sizeof(changed));
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,&player));
  EXPECT_TRUE(command.present());EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialCount(),count);EXPECT_DOUBLE_EQ(player.time(),time);
  std::memcpy(encoded,&original,sizeof(original));ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  const auto interpolation=sampler.interpolation;sampler.interpolation="STEP";
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,&player));
  sampler.interpolation=interpolation;ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  const auto name=source.animations[0].name;source.animations[0].name+=" stale clip identity";
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stage,&player));
  source.animations[0].name=name;ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,&player));
  // The raw source was restored before the final external Can+exchange phase.
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,stage,&player));EXPECT_FALSE(command.present());
  EXPECT_DOUBLE_EQ(player.time(),time);EXPECT_TRUE(player.playing());
}
std::vector<std::byte> producerPng(const std::filesystem::path& directory,const std::string& name,unsigned char red=63) {
  auto asset=triangle("Texture producer",red);asset.gltfModel.images[0].uri=name+".png";
  tinygltf::TinyGLTF writer;
  if(!writer.WriteGltfSceneToFile(&asset.gltfModel,(directory/(name+".gltf")).string(),false,true,true,false))
    throw std::runtime_error("Cannot write texture producer fixture");
  std::ifstream stream(directory/(name+".png"),std::ios::binary|std::ios::ate);
  const auto length=stream.tellg();
  if(!stream || length<=0 || uint64_t(length)>1024*1024) throw std::runtime_error("Invalid texture producer PNG fixture");
  std::vector<std::byte> bytes(static_cast<size_t>(length));stream.seekg(0);
  if(!stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))
    throw std::runtime_error("Cannot read texture producer PNG fixture");
  return bytes;
}

TEST_F(SceneMergeGenerationGpu,TextureSourceProducerKeepsPairedSourceIdsAndIndependentColorDataRoles) {
  loadSource(triangle("Untextured prefix",71,false,false));
  const auto bytes=producerPng(temp,"paired-source");const auto oldTextures=manager->animationSource().textures.size();
  const auto* geometry=manager->vertices().data();const auto* sourceBytes=manager->animationSource().buffers[0].data.data();
  const auto first=manager->loadMaterialTextureSourceFromBytes("source-A",bytes,true);
  const auto data=manager->loadMaterialTextureSourceFromBytes("source-A",bytes,false);
  const auto again=manager->loadMaterialTextureSourceFromBytes("source-A",bytes,true);
  ASSERT_TRUE(first);ASSERT_TRUE(data);ASSERT_TRUE(again);ASSERT_TRUE(first.evaluatedTexture());ASSERT_TRUE(data.evaluatedTexture());
  EXPECT_TRUE(first.isSrgb());EXPECT_FALSE(data.isSrgb());
  EXPECT_EQ(first.archiveTexture(),data.archiveTexture());EXPECT_EQ(first.evaluatedTexture(),data.evaluatedTexture());
  EXPECT_EQ(first.runtimeResource(),again.runtimeResource());EXPECT_EQ(first.archiveTexture(),again.archiveTexture());
  EXPECT_EQ(first.evaluatedTexture(),again.evaluatedTexture());EXPECT_EQ(manager->animationSource().textures.size(),oldTextures+1);
  EXPECT_EQ(manager->gltfTextureResources().color[*first.evaluatedTexture()],first.runtimeResource());
  EXPECT_EQ(manager->gltfTextureResources().data[*data.evaluatedTexture()],data.runtimeResource());
  const auto independent=manager->loadMaterialTextureSourceFromBytes("source-B",bytes,true);
  ASSERT_TRUE(independent.evaluatedTexture());EXPECT_NE(independent.archiveTexture(),first.archiveTexture());
  EXPECT_NE(independent.evaluatedTexture(),first.evaluatedTexture());
  // Equal pixels may alias residency, but never erase the distinct source ID.
  EXPECT_EQ(manager->animationSource().textures.size(),oldTextures+2);
  const auto pathColor=manager->loadMaterialTextureSource(temp/"paired-source.png",true);
  const auto pathData=manager->loadMaterialTextureSource(temp/"paired-source.png",false);
  EXPECT_EQ(pathColor.archiveTexture(),pathData.archiveTexture());EXPECT_EQ(pathColor.evaluatedTexture(),pathData.evaluatedTexture());
  EXPECT_NE(pathColor.archiveTexture(),first.archiveTexture());
  EXPECT_EQ(manager->animationSource().textures.size(),oldTextures+3);
  const auto alternatePolicy=container::material::NormalizeMaterialSamplerKey(TINYGLTF_TEXTURE_FILTER_NEAREST,
      TINYGLTF_TEXTURE_FILTER_LINEAR,TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE,TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT);
  ASSERT_NE(alternatePolicy.value,0u);
  const auto alternate=manager->loadMaterialTextureSourceFromBytes("source-A",bytes,true,alternatePolicy.value);
  ASSERT_TRUE(alternate);ASSERT_TRUE(alternate.evaluatedTexture());EXPECT_TRUE(alternate.isSrgb());
  EXPECT_NE(alternate.archiveTexture(),first.archiveTexture());EXPECT_NE(alternate.evaluatedTexture(),first.evaluatedTexture());
  EXPECT_NE(alternate.runtimeResource(),first.runtimeResource());
  ASSERT_LT(*alternate.evaluatedTexture(),manager->gltfTextureResources().color.size());
  EXPECT_EQ(manager->gltfTextureResources().color[*alternate.evaluatedTexture()],alternate.runtimeResource());
  EXPECT_EQ(manager->animationSource().textures.size(),oldTextures+4);
  const auto archived=manager->buildSceneExport(graph);const auto& live=manager->animationSource();
  ASSERT_LT(alternate.archiveTexture(),archived.textures.size());ASSERT_LT(first.archiveTexture(),archived.textures.size());
  ASSERT_LT(*alternate.evaluatedTexture(),live.textures.size());ASSERT_LT(*first.evaluatedTexture(),live.textures.size());
  const auto archivedSampler=archived.textures[alternate.archiveTexture()].sampler;
  const auto liveSampler=live.textures[*alternate.evaluatedTexture()].sampler;
  EXPECT_NE(archivedSampler,archived.textures[first.archiveTexture()].sampler);
  EXPECT_NE(liveSampler,live.textures[*first.evaluatedTexture()].sampler);
  ASSERT_GE(archivedSampler,0);ASSERT_LT(size_t(archivedSampler),archived.samplers.size());
  ASSERT_GE(liveSampler,0);ASSERT_LT(size_t(liveSampler),live.samplers.size());
  for(const auto* sampler:std::array{&archived.samplers[size_t(archivedSampler)],&live.samplers[size_t(liveSampler)]}) {
    EXPECT_EQ(sampler->minFilter,TINYGLTF_TEXTURE_FILTER_NEAREST);
    EXPECT_EQ(sampler->magFilter,TINYGLTF_TEXTURE_FILTER_LINEAR);
    EXPECT_EQ(sampler->wrapS,TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);
    EXPECT_EQ(sampler->wrapT,TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT);
  }
  const auto liveSamplers=live.samplers.size();
  const auto alternateAgain=manager->loadMaterialTextureSourceFromBytes("source-A",bytes,true,alternatePolicy.value);
  const auto originalAgain=manager->loadMaterialTextureSourceFromBytes("source-A",bytes,true);
  EXPECT_EQ(alternateAgain.archiveTexture(),alternate.archiveTexture());EXPECT_EQ(alternateAgain.evaluatedTexture(),alternate.evaluatedTexture());
  EXPECT_EQ(alternateAgain.runtimeResource(),alternate.runtimeResource());
  EXPECT_EQ(originalAgain.archiveTexture(),first.archiveTexture());EXPECT_EQ(originalAgain.evaluatedTexture(),first.evaluatedTexture());
  EXPECT_EQ(originalAgain.runtimeResource(),first.runtimeResource());
  EXPECT_EQ(live.textures.size(),oldTextures+4);EXPECT_EQ(live.samplers.size(),liveSamplers);
  EXPECT_EQ(manager->vertices().data(),geometry);EXPECT_EQ(manager->animationSource().buffers[0].data.data(),sourceBytes);
}

TEST_F(SceneMergeGenerationGpu,TextureSourceProducerPromotesArchivePrefixBeforeAppendingNewImageAndSamplerReferences) {
  loadSource(triangle("Archive prefix promotion",78,false,false));
  const auto prefixImages=manager->animationSource().images.size(),prefixSamplers=manager->animationSource().samplers.size();
  const auto prefixTextures=manager->animationSource().textures.size();
  const auto oldBytes=producerPng(temp,"archive-old",63),newBytes=producerPng(temp,"archive-new",191);
  ASSERT_NE(oldBytes,newBytes);
  const auto old=manager->loadMaterialTextureSourceFromBytes("archive-old",oldBytes,true);
  ASSERT_TRUE(old.evaluatedTexture());const auto archiveBefore=manager->buildSceneExport(graph);
  const auto retainedImage=manager->animationSource().images[prefixImages];
  container::material::Material oldMaterial;oldMaterial.baseColorTextureIndex=old.runtimeResource();
  SceneManager::MaterialTextureProvenance oldProvenance;oldProvenance[0]=old;
  const auto origin=manager->createMaterial(oldMaterial,oldProvenance);ASSERT_TRUE(manager->hasRuntimeMaterialProvenance(origin));
  auto& live=manager->animationSource();const auto* geometryBytes=live.buffers[0].data.data();
  // Public mutable source access simulates a legacy archive-only resource
  // suffix. No live material references that suffix; its archive and explicit
  // runtime source receipt remain resident for the producer to promote.
  live.images.resize(prefixImages);live.samplers.resize(prefixSamplers);live.textures.resize(prefixTextures);
  EXPECT_FALSE(manager->hasRuntimeMaterialProvenance(origin));
  const auto key=container::material::NormalizeMaterialSamplerKey(TINYGLTF_TEXTURE_FILTER_NEAREST,
      TINYGLTF_TEXTURE_FILTER_NEAREST,TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE,TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT);
  ASSERT_NE(key.value,0u);
  const auto next=manager->loadMaterialTextureSourceFromBytes("archive-new",newBytes,false,key.value);
  ASSERT_TRUE(next.evaluatedTexture());ASSERT_LT(*next.evaluatedTexture(),live.textures.size());
  EXPECT_EQ(live.images.size(),prefixImages+2);EXPECT_EQ(live.samplers.size(),prefixSamplers+2);
  EXPECT_EQ(live.textures.size(),prefixTextures+2);EXPECT_EQ(next.archiveTexture(),archiveBefore.textures.size());
  const auto& appended=live.textures[*next.evaluatedTexture()];
  EXPECT_EQ(appended.source,int(live.images.size()-1));EXPECT_EQ(appended.sampler,int(live.samplers.size()-1));
  EXPECT_EQ(appended.source,int(archiveBefore.images.size()));EXPECT_EQ(appended.sampler,int(archiveBefore.samplers.size()));
  ASSERT_LT(*old.evaluatedTexture(),live.textures.size());const auto& promoted=live.textures[*old.evaluatedTexture()];
  EXPECT_NE(appended.source,promoted.source);EXPECT_NE(appended.sampler,promoted.sampler);
  EXPECT_EQ(live.images[prefixImages],retainedImage);
  EXPECT_EQ(live.samplers[prefixSamplers],archiveBefore.samplers[prefixSamplers]);
  EXPECT_EQ(live.textures[prefixTextures],archiveBefore.textures[prefixTextures]);
  const auto& sampler=live.samplers[size_t(appended.sampler)];const auto expected=container::material::DecodeMaterialSamplerKey(key);
  EXPECT_EQ(sampler.minFilter,expected.minFilter);EXPECT_EQ(sampler.magFilter,expected.magFilter);
  EXPECT_EQ(sampler.wrapS,expected.wrapS);EXPECT_EQ(sampler.wrapT,expected.wrapT);
  EXPECT_EQ(manager->gltfTextureResources().color[*old.evaluatedTexture()],old.runtimeResource());
  EXPECT_EQ(manager->gltfTextureResources().data[*next.evaluatedTexture()],next.runtimeResource());
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(origin));
  const auto repeated=manager->loadMaterialTextureSourceFromBytes("archive-old",oldBytes,true);
  EXPECT_EQ(repeated.archiveTexture(),old.archiveTexture());EXPECT_EQ(repeated.evaluatedTexture(),old.evaluatedTexture());
  EXPECT_EQ(repeated.runtimeResource(),old.runtimeResource());EXPECT_EQ(live.textures.size(),prefixTextures+2);
  EXPECT_EQ(live.buffers[0].data.data(),geometryBytes);
}

TEST_F(SceneMergeGenerationGpu,TextureSourceDecodeFailureRetainsPriorDomainsResourcesAndExplicitReferences) {
  loadSource(triangle("Failed texture prefix",79,false,false));const auto bytes=producerPng(temp,"prior-valid");
  const auto prior=manager->loadMaterialTextureSourceFromBytes("prior-valid",bytes,true);
  container::material::Material material;material.baseColorTextureIndex=prior.runtimeResource();
  SceneManager::MaterialTextureProvenance provenance;provenance[0]=prior;const auto origin=manager->createMaterial(material,provenance);
  manager->uploadMaterialResources();MaterialFrames frames(*allocation);manager->updateDescriptorSets(frames.cameras,frames.objects);
  manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto archive=manager->buildSceneExport(graph);const auto images=manager->animationSource().images;
  const auto samplers=manager->animationSource().samplers;const auto textures=manager->animationSource().textures;
  const auto roles=manager->gltfTextureResources();const auto count=manager->materialCount();
  const auto revision=manager->materialRevision(),epoch=manager->generationEpoch();const auto baseline=allocations();
  const auto descriptor=manager->descriptorSet(0);const auto* geometryBytes=manager->animationSource().buffers[0].data.data();
  ASSERT_GT(bytes.size(),16u);auto invalid=bytes;invalid.resize(16);
  const auto key=container::material::NormalizeMaterialSamplerKey(TINYGLTF_TEXTURE_FILTER_NEAREST,
      TINYGLTF_TEXTURE_FILTER_LINEAR,TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE,TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);
  ASSERT_NE(key.value,0u);
  EXPECT_THROW((void)manager->loadMaterialTextureSourceFromBytes("decode-must-fail",invalid,false,key.value),std::runtime_error);
  EXPECT_THROW((void)manager->loadMaterialTextureSourceFromBytes("empty-must-fail",std::span<const std::byte>{},false,key.value),std::length_error);
  EXPECT_EQ(manager->animationSource().images,images);EXPECT_EQ(manager->animationSource().samplers,samplers);
  EXPECT_EQ(manager->animationSource().textures,textures);EXPECT_EQ(manager->buildSceneExport(graph),archive);
  EXPECT_EQ(manager->gltfTextureResources().color,roles.color);EXPECT_EQ(manager->gltfTextureResources().data,roles.data);
  EXPECT_EQ(manager->materialCount(),count);EXPECT_EQ(manager->materialRevision(),revision);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(allocations(),baseline);
  EXPECT_EQ(manager->animationSource().buffers[0].data.data(),geometryBytes);EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(origin));
  const auto reused=manager->loadMaterialTextureSourceFromBytes("prior-valid",bytes,true);
  EXPECT_EQ(reused.runtimeResource(),prior.runtimeResource());EXPECT_EQ(reused.archiveTexture(),prior.archiveTexture());
  EXPECT_EQ(reused.evaluatedTexture(),prior.evaluatedTexture());EXPECT_EQ(manager->animationSource().textures,textures);
  const auto reuseMaterial=manager->createMaterial(material,provenance);EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(reuseMaterial));
}

TEST_F(SceneMergeGenerationGpu,ExplicitMaterialTextureProvenanceRejectsAbsentWrongRoleForeignAndExpiredReferences) {
  loadSource(triangle("Provenance prefix",72,false,false));const auto bytes=producerPng(temp,"provenance-source");
  const auto color=manager->loadMaterialTextureSourceFromBytes("provenance",bytes,true);
  const auto data=manager->loadMaterialTextureSourceFromBytes("provenance",bytes,false);
  container::material::Material material;material.baseColorTextureIndex=color.runtimeResource();
  SceneManager::MaterialTextureProvenance valid;valid[0]=color;
  const auto count=manager->materialCount(),textures=manager->animationSource().textures.size();
  const auto reject=[&](const container::material::Material& value,const SceneManager::MaterialTextureProvenance& provenance) {
    EXPECT_THROW((void)manager->createMaterial(value,provenance),std::invalid_argument);
    EXPECT_EQ(manager->materialCount(),count);EXPECT_EQ(manager->animationSource().textures.size(),textures);
  };
  reject(material,{});
  auto wrong=valid;wrong[0]=data;reject(material,wrong);
  wrong=valid;wrong[0]=SceneManager::MaterialTextureReference{};reject(material,wrong);
  reject(container::material::Material{},valid);
  auto foreignAllocation=std::make_unique<AllocationManager>();
  foreignAllocation->initialize(instance->instance(),device->physicalDevice(),device->device(),device->graphicsQueue(),*pool,{});
  auto other=std::make_unique<SceneManager>(*foreignAllocation,*pipelines,device,container::app::AppConfig{});
  other->initialize((temp/"a.glb").string(),2.f,2);
  const auto foreign=other->loadMaterialTextureSourceFromBytes("provenance",bytes,true);
  auto foreignMaterial=material;foreignMaterial.baseColorTextureIndex=foreign.runtimeResource();
  wrong=valid;wrong[0]=foreign;reject(foreignMaterial,wrong);
  const auto certified=manager->createMaterial(material,valid);EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(certified));
  const auto unknown=manager->createMaterial(material);EXPECT_FALSE(manager->hasRuntimeMaterialProvenance(unknown));
  auto replacement=triangle("Replacement expires receipts",73,false,false);
  ASSERT_TRUE(manager->reloadModel((temp/"replacement.glb").string(),2.f,{},AllocatedBuffer{},&replacement));
  const auto newCount=manager->materialCount();
  EXPECT_THROW((void)manager->createMaterial(material,valid),std::invalid_argument);EXPECT_EQ(manager->materialCount(),newCount);
}

TEST_F(SceneMergeGenerationGpu,RuntimeDefaultSolidAndImplicitMaterialsMaterializeWithoutSourceIdsOrMetallicPromotion) {
  using namespace container::geometry::gltf;
  load(true);MaterialFrames frames(*allocation);
  const auto implicit=graph.getNode(primitiveNode(sourceNode(0)))->materialIndex;
  EXPECT_NE(implicit,manager->defaultMaterialIndex());
  EXPECT_EQ(manager->resolveMaterialMetallicRoughnessFactors(implicit).x,1.f);
  EXPECT_EQ(manager->resolveMaterialMetallicRoughnessFactors(implicit).y,1.f);
  const auto solid=manager->createSolidMaterial({.2f,.4f,.8f,.45f},true,container::material::AlphaMode::Blend);
  manager->uploadMaterialResources();manager->updateDescriptorSets(frames.cameras,frames.objects);
  manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto sources=manager->animationSource().materials.size(),runtimes=manager->materialCount();const auto baseline=allocations();
  for(const auto origin:std::array{manager->defaultMaterialIndex(),manager->diagnosticMaterialIndex(),implicit,solid}) {
    SCOPED_TRACE(origin);ASSERT_EQ(manager->sourceMaterialForRuntime(origin),-1);ASSERT_TRUE(manager->hasRuntimeMaterialProvenance(origin));
    const auto factors=manager->resolveMaterialMetallicRoughnessFactors(origin);const auto color=manager->resolveMaterialColor(origin);
    auto stage=manager->prepareRuntimeMaterialMakeUnique(origin,{},nullptr);ASSERT_EQ(stage.sourceMaterial(),int32_t(sources));
    EXPECT_EQ(stage.runtimeMaterial(),runtimes);EXPECT_TRUE(stage.appendCommand());
    const auto& record=stage.evaluatedMaterial();
    EXPECT_DOUBLE_EQ(record.pbrMetallicRoughness.metallicFactor,double(factors.x));
    EXPECT_DOUBLE_EQ(record.pbrMetallicRoughness.roughnessFactor,double(factors.y));
    ASSERT_EQ(record.pbrMetallicRoughness.baseColorFactor.size(),4u);
    for(size_t i=0;i<4;++i) EXPECT_DOUBLE_EQ(record.pbrMetallicRoughness.baseColorFactor[i],double(color[i]));
    frames.bind(stage);ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
    EXPECT_EQ(manager->materialCount(),runtimes);EXPECT_EQ(manager->animationSource().materials.size(),sources);
    EXPECT_EQ(manager->sourceMaterialForRuntime(origin),-1);
  }
  EXPECT_EQ(allocations(),baseline);
  EXPECT_FLOAT_EQ(manager->resolveMaterialMetallicRoughnessFactors(solid).x,0.f);
}

TEST_F(SceneMergeGenerationGpu,RuntimeMaterializationPreservesAll23ExplicitTextureSourcesAndOwnedTailReplay) {
  using namespace container::geometry::gltf;using Runtime=container::material::Material;
  loadSource(triangle("Twenty-three slot prefix",74,false,false));const auto bytes=producerPng(temp,"all-slots");
  const std::array<uint32_t Runtime::*,23> slots{&Runtime::baseColorTextureIndex,&Runtime::normalTextureIndex,&Runtime::occlusionTextureIndex,
      &Runtime::emissiveTextureIndex,&Runtime::metallicRoughnessTextureIndex,&Runtime::roughnessTextureIndex,&Runtime::metalnessTextureIndex,
      &Runtime::specularTextureIndex,&Runtime::heightTextureIndex,&Runtime::opacityTextureIndex,&Runtime::transmissionTextureIndex,
      &Runtime::specularColorTextureIndex,&Runtime::clearcoatTextureIndex,&Runtime::clearcoatRoughnessTextureIndex,&Runtime::clearcoatNormalTextureIndex,
      &Runtime::thicknessTextureIndex,&Runtime::sheenColorTextureIndex,&Runtime::sheenRoughnessTextureIndex,&Runtime::iridescenceTextureIndex,
      &Runtime::iridescenceThicknessTextureIndex,&Runtime::anisotropyTextureIndex,&Runtime::diffuseTransmissionTextureIndex,&Runtime::diffuseTransmissionColorTextureIndex};
  Runtime material;material.baseColor={.1f,.3f,.7f,.45f};material.metallicFactor=0.f;material.roughnessFactor=.625f;
  material.alphaMode=container::material::AlphaMode::Blend;material.doubleSided=true;material.clearcoatFactor=.3f;
  SceneManager::MaterialTextureProvenance provenance;std::array<int32_t,23> sourceIds{};uint32_t resourceCount{};
  for(size_t i=0;i<slots.size();++i) {
    const bool color=i==0 || i==3 || i==11 || i==16 || i==22;
    const auto source=manager->loadMaterialTextureSourceFromBytes("explicit-slot-"+std::to_string(i),bytes,color);
    ASSERT_TRUE(source.evaluatedTexture());provenance[i]=source;material.*slots[i]=source.runtimeResource();
    sourceIds[i]=int32_t(*source.evaluatedTexture());resourceCount=std::max(resourceCount,source.runtimeResource()+1);
    for(size_t earlier=0;earlier<i;++earlier) EXPECT_NE(sourceIds[i],sourceIds[earlier]);
  }
  const auto origin=manager->createMaterial(material,provenance);ASSERT_TRUE(manager->hasRuntimeMaterialProvenance(origin));
  manager->uploadMaterialResources();MaterialFrames frames(*allocation);manager->updateDescriptorSets(frames.cameras,frames.objects);
  manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto textureCount=manager->animationSource().textures.size(),images=manager->animationSource().images.size();const auto baseline=allocations();
  const auto roles=manager->gltfTextureResources();const auto* pixels=manager->animationSource().images.back().image.data();
  const auto sourceCount=manager->animationSource().materials.size(),runtimeCount=manager->materialCount();
  GltfMaterialPatch patch;patch.name="All slots unique";
  auto stage=manager->prepareRuntimeMaterialMakeUnique(origin,patch,nullptr);auto command=stage.appendCommand();
  const auto frozen=stage.evaluatedMaterial();ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,stage));stage={};
  EXPECT_EQ(manager->sourceMaterialForRuntime(origin),-1);EXPECT_EQ(manager->sourceMaterialRuntimeMapping().back(),runtimeCount);
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(runtimeCount));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(int32_t(sourceCount)),frozen));
  const auto& source=manager->animationSource();
  const auto converted=container::material::ConvertGltfMaterial(source.materials.back(),{source.textures.size(),source.extensionsUsed,
      manager->gltfTextureResources().color,manager->gltfTextureResources().data,resourceCount});
  EXPECT_EQ(converted.sourceTextures,sourceIds);
  EXPECT_EQ(converted.runtimeBindings.runtimeIndices,snapshotMaterialTextureBindings(material).runtimeIndices);
  EXPECT_EQ(manager->animationSource().textures.size(),textureCount);EXPECT_EQ(manager->animationSource().images.size(),images);
  EXPECT_EQ(manager->animationSource().images.back().image.data(),pixels);EXPECT_EQ(allocations(),baseline);
  auto detach=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,detach));detach={};
  EXPECT_EQ(manager->materialCount(),runtimeCount);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(origin));
  auto restore=manager->prepareMaterialTailChange(command,MaterialTailState::Present,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,restore));restore={};
  EXPECT_TRUE(command.present());EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),frozen));
  EXPECT_EQ(manager->gltfTextureResources().color,roles.color);EXPECT_EQ(manager->gltfTextureResources().data,roles.data);
  EXPECT_EQ(manager->animationSource().images.size(),images);EXPECT_EQ(allocations(),baseline);
}

TEST_F(SceneMergeGenerationGpu,RuntimeMaterializationKeepsCurrentAnimatedPrefixAndUntrackedRestAcrossTailReplay) {
  using namespace container::geometry::gltf;
  loadSource(animatedMaterialAppendAsset());const auto origin=manager->createSolidMaterial({.3f,.6f,.2f,1});
  manager->uploadMaterialResources();MaterialFrames frames(*allocation);manager->updateDescriptorSets(frames.cameras,frames.objects);
  manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  AnimationPlayer player(manager->animationSource());auto dirty=player.selectClip(manager->animationSource(),0);
  dirty.merge(player.seek(manager->animationSource(),.5));manager->publishAnimationChanges(dirty,graph);
  player.setLoop(false);player.setSpeed(.25);player.play();const auto clips=manager->animationSource().animations;
  const auto originalRest=player.library().restMaterials[0];const auto* bytes=manager->animationSource().buffers[0].data.data();
  const auto sourceCount=manager->animationSource().materials.size();
  auto initial=manager->prepareRuntimeMaterialMakeUnique(origin,{},&player);auto command=initial.appendCommand();
  const auto frozen=initial.evaluatedMaterial();ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,initial,&player));initial={};
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials.back(),frozen));
  dirty=player.seek(manager->animationSource(),1.5);manager->publishAnimationChanges(dirty,graph);
  const auto prefixAtUndo=manager->animationSource().materials[0];
  auto detach=manager->prepareMaterialTailChange(command,MaterialTailState::Detached,graph,&player);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,detach,&player));detach={};
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[0],prefixAtUndo));
  EXPECT_EQ(player.library().restMaterials.size(),sourceCount);EXPECT_DOUBLE_EQ(player.time(),1.5);
  dirty=player.seek(manager->animationSource(),.25);manager->publishAnimationChanges(dirty,graph);
  const auto currentPrefix=manager->animationSource().materials[0];
  auto restore=manager->prepareMaterialTailChange(command,MaterialTailState::Present,graph,&player);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,restore,&player));restore={};
  EXPECT_DOUBLE_EQ(player.time(),.25);EXPECT_DOUBLE_EQ(player.speed(),.25);EXPECT_TRUE(player.playing());EXPECT_FALSE(player.loop());
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials[0],currentPrefix));
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[0],originalRest));
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials.back(),frozen));
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->animationSource().materials.back(),frozen));
  EXPECT_EQ(manager->animationSource().animations,clips);EXPECT_EQ(manager->animationSource().buffers[0].data.data(),bytes);
}

TEST_F(SceneMergeGenerationGpu,RuntimeMaterializationRejectsUnprovedSourceBackedInvalidAndBudgetedOriginsBeforePublication) {
  using namespace container::geometry::gltf;
  loadSource(roleAliasedTriangle());MaterialFrames frames(*allocation);
  container::material::Material unknown;unknown.baseColorTextureIndex=manager->gltfTextureResources().color[0];
  const auto unknownId=manager->createMaterial(unknown);EXPECT_FALSE(manager->hasRuntimeMaterialProvenance(unknownId));
  const auto origin=manager->createSolidMaterial({.4f,.2f,.7f,1});manager->uploadMaterialResources();
  manager->updateDescriptorSets(frames.cameras,frames.objects);manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto sourceBacked=manager->sourceMaterialRuntimeMapping()[0];const auto count=manager->materialCount(),sources=manager->animationSource().materials.size();
  const auto epoch=manager->generationEpoch();const auto baseline=allocations();
  for(const auto invalid:std::array{UINT32_MAX,uint32_t(count),unknownId,sourceBacked}) {
    EXPECT_THROW((void)manager->prepareRuntimeMaterialMakeUnique(invalid,{},nullptr),std::invalid_argument);
    EXPECT_EQ(manager->materialCount(),count);EXPECT_EQ(manager->animationSource().materials.size(),sources);EXPECT_EQ(allocations(),baseline);
  }
  MaterialGenerationLimits limits;limits.authoring.maxOutputBytes=1;
  EXPECT_THROW((void)manager->prepareRuntimeMaterialMakeUnique(origin,{},nullptr,limits),std::invalid_argument);
  limits={};limits.maxPreparedCpuBytes=1;
  EXPECT_THROW((void)manager->prepareRuntimeMaterialMakeUnique(origin,{},nullptr,limits),std::invalid_argument);
  limits={};limits.maxMaterialBufferBytes=1;
  EXPECT_THROW((void)manager->prepareRuntimeMaterialMakeUnique(origin,{},nullptr,limits),std::invalid_argument);
  limits={};limits.maxMaterialCount=count;
  EXPECT_THROW((void)manager->prepareRuntimeMaterialMakeUnique(origin,{},nullptr,limits),std::invalid_argument);
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialCount(),count);
  auto stale=manager->prepareRuntimeMaterialMakeUnique(origin,{},nullptr);frames.bind(stale);auto receipt=stale.appendCommand();
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stale,nullptr));
  const auto competitor=manager->createSolidMaterial({.1f,.2f,.3f,1});const auto competed=manager->materialCount();
  EXPECT_FALSE(manager->canExchangeMaterialGeneration(stale,nullptr));manager->exchangeMaterialGeneration(stale,nullptr);
  EXPECT_TRUE(stale);EXPECT_FALSE(receipt.published());EXPECT_EQ(manager->materialCount(),competed);
  EXPECT_EQ(manager->sourceMaterialForRuntime(competitor),-1);
}

TEST_F(SceneMergeGenerationGpu,RuntimeTextureReceiptsAndMaterialProvenanceSurviveDetachedMergeAndCloneCopies) {
  using namespace container::geometry::gltf;
  loadSource(triangle("Resource receipt prefix",75,false,false));const auto bytes=producerPng(temp,"receipt-copy");
  const auto reference=manager->loadMaterialTextureSourceFromBytes("retained-request",bytes,true);
  container::material::Material material;material.baseColorTextureIndex=reference.runtimeResource();
  SceneManager::MaterialTextureProvenance provenance;provenance[0]=reference;
  const auto origin=manager->createMaterial(material,provenance);manager->uploadMaterialResources();MaterialFrames frames(*allocation);
  manager->updateDescriptorSets(frames.cameras,frames.objects);manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto check=[&](SceneManager& candidate) {
    EXPECT_TRUE(candidate.hasRuntimeMaterialProvenance(origin));const auto count=candidate.animationSource().textures.size();
    const auto repeated=candidate.loadMaterialTextureSourceFromBytes("retained-request",bytes,true);
    EXPECT_EQ(repeated.archiveTexture(),reference.archiveTexture());EXPECT_EQ(repeated.evaluatedTexture(),reference.evaluatedTexture());
    EXPECT_EQ(repeated.runtimeResource(),reference.runtimeResource());EXPECT_EQ(candidate.animationSource().textures.size(),count);
    auto unique=candidate.prepareRuntimeMaterialMakeUnique(origin,{},nullptr);frames.bind(unique);
    EXPECT_TRUE(candidate.canExchangeMaterialGeneration(unique,nullptr));
  };
  auto merge=manager->prepareMergeGeneration(triangle("Receipt merge",76),temp/"receipt-merge.glb",{},graph);
  check(merge.manager());
  auto clone=manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"Receipt clone");check(clone.manager());
  exchangeMerge(merge);check(*manager);
  exchangeMerge(merge);check(*manager);
}

TEST_F(SceneMergeGenerationGpu,DiscardedRuntimeMaterializationRetainsSourceResourceTablesGraphAndDescriptorPools) {
  loadSource(triangle("Runtime discard prefix",77,false,false));const auto bytes=producerPng(temp,"runtime-discard");
  const auto reference=manager->loadMaterialTextureSourceFromBytes("discard-source",bytes,true);
  container::material::Material material;material.baseColor={.2f,.4f,.8f,1};material.baseColorTextureIndex=reference.runtimeResource();
  SceneManager::MaterialTextureProvenance provenance;provenance[0]=reference;const auto origin=manager->createMaterial(material,provenance);
  manager->uploadMaterialResources();MaterialFrames frames(*allocation);manager->updateDescriptorSets(frames.cameras,frames.objects);
  manager->updateAuxiliaryDescriptorSets(frames.cameras,frames.objects);
  const auto count=manager->materialCount(),sourceCount=manager->animationSource().materials.size();const auto baseline=allocations();
  const auto epoch=manager->generationEpoch(),graphRevision=graph.revision(),geometry=manager->geometryRevision();
  const auto primary=manager->descriptorSet(0),auxiliary=manager->auxiliaryDescriptorSet(1);
  const auto roles=manager->gltfTextureResources();const auto* pixels=manager->animationSource().images.back().image.data();
  const auto* vertices=manager->vertices().data();const auto* sourceBytes=manager->animationSource().buffers[0].data.data();
  SceneManager::MaterialAppendCommand abandoned;
  {
    auto stage=manager->prepareRuntimeMaterialMakeUnique(origin,{},nullptr);abandoned=stage.appendCommand();
    frames.bind(stage);ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));EXPECT_EQ(allocations(),baseline+1);
    EXPECT_FALSE(abandoned.published());EXPECT_EQ(manager->materialCount(),count);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);
  }
  EXPECT_FALSE(abandoned.published());EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(graph.revision(),graphRevision);EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(manager->vertices().data(),vertices);
  EXPECT_EQ(manager->animationSource().buffers[0].data.data(),sourceBytes);EXPECT_EQ(manager->animationSource().images.back().image.data(),pixels);
  EXPECT_EQ(manager->descriptorSet(0),primary);EXPECT_EQ(manager->auxiliaryDescriptorSet(1),auxiliary);
  EXPECT_EQ(manager->gltfTextureResources().color,roles.color);EXPECT_EQ(manager->gltfTextureResources().data,roles.data);
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(origin));
}
TEST_F(SceneMergeGenerationGpu,RuntimeMaterialStageRejectsSuccessfulUnusedTextureImportDuringFinalExchange) {
  loadSource(triangle("Stale runtime resource domain",80,false,false));
  const auto bytes=producerPng(temp,"unused-after-runtime-stage");
  MaterialFrames frames(*allocation);
  auto stage=manager->prepareRuntimeMaterialMakeUnique(manager->defaultMaterialIndex(),{},nullptr);
  const auto receipt=stage.appendCommand();frames.bind(stage);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));
  const auto materialCount=manager->materialCount(),sourceCount=manager->animationSource().materials.size();
  const auto sourceTextures=manager->animationSource().textures.size();
  const auto sourceImages=manager->animationSource().images.size(),sourceSamplers=manager->animationSource().samplers.size();
  const auto allocationsBeforeImport=allocations();
  const auto epoch=manager->generationEpoch(),revision=manager->materialRevision();
  const auto imported=manager->loadMaterialTextureSourceFromBytes("unused-after-runtime-stage",bytes,true);
  ASSERT_TRUE(imported);ASSERT_TRUE(imported.evaluatedTexture());
  EXPECT_EQ(allocations(),allocationsBeforeImport+1);
  EXPECT_EQ(manager->animationSource().textures.size(),sourceTextures+1);
  EXPECT_EQ(manager->animationSource().images.size(),sourceImages+1);EXPECT_EQ(manager->animationSource().samplers.size(),sourceSamplers+1);
  ASSERT_LT(*imported.evaluatedTexture(),manager->gltfTextureResources().color.size());
  EXPECT_EQ(manager->gltfTextureResources().color[*imported.evaluatedTexture()],imported.runtimeResource());
  EXPECT_EQ(manager->materialCount(),materialCount);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);
  // Importing an unused resource changes none of the material/revision guards.
  // Its new source/runtime texture domains must independently reject the token.
  EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialRevision(),revision);
  ASSERT_FALSE(manager->canExchangeMaterialGeneration(stage,nullptr));
  const auto archived=manager->buildSceneExport(graph);
  const auto images=manager->animationSource().images;const auto samplers=manager->animationSource().samplers;
  const auto textures=manager->animationSource().textures;const auto roles=manager->gltfTextureResources();
  const auto descriptor=manager->descriptorSet(0),auxiliary=manager->auxiliaryDescriptorSet(1);
  const auto graphRevision=graph.revision(),geometry=manager->geometryRevision();const auto allocationCount=allocations();
  size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMaterialGeneration(stage,nullptr);attempts=guard.allocations();
  }
  EXPECT_EQ(attempts,0u);EXPECT_TRUE(stage);EXPECT_FALSE(receipt.published());
  EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialRevision(),revision);
  EXPECT_EQ(manager->materialCount(),materialCount);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);
  EXPECT_EQ(allocations(),allocationsBeforeImport+1);EXPECT_EQ(manager->animationSource().textures.size(),sourceTextures+1);
  EXPECT_EQ(manager->animationSource().images,images);EXPECT_EQ(manager->animationSource().samplers,samplers);
  EXPECT_EQ(manager->animationSource().textures,textures);EXPECT_EQ(manager->buildSceneExport(graph),archived);
  EXPECT_EQ(manager->gltfTextureResources().color,roles.color);EXPECT_EQ(manager->gltfTextureResources().data,roles.data);
  EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->auxiliaryDescriptorSet(1),auxiliary);
  EXPECT_EQ(graph.revision(),graphRevision);EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(allocations(),allocationCount);
  const auto retained=manager->loadMaterialTextureSourceFromBytes("unused-after-runtime-stage",bytes,true);
  EXPECT_EQ(retained.archiveTexture(),imported.archiveTexture());EXPECT_EQ(retained.evaluatedTexture(),imported.evaluatedTexture());
  EXPECT_EQ(retained.runtimeResource(),imported.runtimeResource());EXPECT_EQ(allocations(),allocationCount);
}
TEST_F(SceneMergeGenerationGpu,RetiredMaterialPhasesSurviveMergeAndCloneAndOriginalAuthorityReturnsOnHistorySwap) {
  auto source=triangle("Retired prefix",93,false,false);source.gltfModel.materials.resize(3);
  source.gltfModel.materials[1].name="Unused middle";source.gltfModel.materials[2].name="Unused surviving suffix";
  loadSource(std::move(source));MaterialFrames frames(*allocation);
  const auto runtime=manager->sourceMaterialRuntimeMapping()[1];
  const auto count=manager->materialCount();
  auto retire=manager->prepareMaterialRetirement(runtime,graph,nullptr);const auto command=retire.retirementCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,retire));retire={};
  ASSERT_FALSE(manager->materialIsActive(runtime));ASSERT_TRUE(command.retired());
  const auto incarnation=manager->materialIncarnation(runtime);
  for(const bool clone:{false,true}) {
    SCOPED_TRACE(clone);
    const auto first=uint32_t(graph.nodeCount());const auto view=graph.activeRoots();
    auto later=clone?manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"Retired prefix clone"):
        manager->prepareMergeGeneration(triangle("Retired prefix merge",101,false,false),temp/"retired-incoming.glb",{},graph);
    EXPECT_FALSE(later.manager().materialIsActive(runtime));
    EXPECT_EQ(later.manager().retiredMaterialIndices(),(std::vector<int32_t>{int32_t(runtime)}));
    auto append=graph.prepareAppend(later.graph());ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));
    exchangeMerge(later);
    EXPECT_FALSE(manager->materialIsActive(runtime));
    if(!clone) {
      EXPECT_THROW((void)manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Active,graph,nullptr),std::invalid_argument);
    } else {
      auto restore=manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Active,graph,nullptr);
      ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,restore));restore={};
      EXPECT_TRUE(manager->materialIsActive(runtime));
      auto remove=manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Retired,graph,nullptr);
      ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,remove));remove={};
    }
    auto undo=graph.prepareDetachTail(first,view);ASSERT_TRUE(undo);ASSERT_TRUE(graph.publishTailChange(undo));
    auto retainedGraph=undo.takeDetachedTail();ASSERT_TRUE(retainedGraph);exchangeMerge(later);
    EXPECT_EQ(manager->materialCount(),count);EXPECT_EQ(manager->materialIncarnation(runtime),incarnation);
    EXPECT_FALSE(manager->materialIsActive(runtime));
  }
  auto restore=manager->prepareMaterialRetirementChange(command,MaterialRetirementState::Active,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,restore));
  EXPECT_TRUE(manager->materialIsActive(runtime));EXPECT_EQ(manager->authoredMaterial(1).name,"Unused middle");
}

TEST_F(SceneMergeGenerationGpu,CloneHistoryKeepsCatalogHighWaterWhenFreshBranchReusesDetachedTailAddress) {
  load();MaterialFrames frames(*allocation);
  const auto first=uint32_t(graph.nodeCount());const auto view=graph.activeRoots();
  auto clone=manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"High-water clone");
  auto appendGraph=graph.prepareAppend(clone.graph());ASSERT_TRUE(appendGraph);ASSERT_TRUE(graph.publishAppend(appendGraph));
  exchangeMerge(clone);
  auto original=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);const auto old=original.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,original));original={};
  const auto oldIncarnation=manager->materialIncarnation(old.runtimeMaterial());
  auto detach=manager->prepareMaterialTailChange(old,MaterialTailState::Detached,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,detach));detach={};
  auto undo=graph.prepareDetachTail(first,view);ASSERT_TRUE(undo);ASSERT_TRUE(graph.publishTailChange(undo));
  auto retainedGraph=undo.takeDetachedTail();ASSERT_TRUE(retainedGraph);exchangeMerge(clone);
  auto replacement=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);const auto fresh=replacement.appendCommand();
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,replacement));replacement={};
  EXPECT_EQ(fresh.runtimeMaterial(),old.runtimeMaterial());EXPECT_EQ(fresh.sourceMaterial(),old.sourceMaterial());
  EXPECT_NE(manager->materialIncarnation(fresh.runtimeMaterial()),oldIncarnation);
  EXPECT_THROW((void)manager->prepareMaterialTailChange(old,MaterialTailState::Present,graph,nullptr),std::invalid_argument);
  EXPECT_TRUE(fresh.present());EXPECT_FALSE(old.present());
}

TEST_F(SceneMergeGenerationGpu,ManagerSaveCompactsUnusedMiddleMaterialInAsciiAndGlbWithoutChangingLiveSlotsOrImages) {
  auto source=triangle("Portable retired material",117);source.gltfModel.materials.resize(3);
  source.gltfModel.materials[1].name="Delete this middle row";source.gltfModel.materials[2].name="Surviving suffix";
  loadSource(std::move(source));MaterialFrames frames(*allocation);
  const auto count=manager->materialCount(),sourceCount=manager->animationSource().materials.size();
  const auto runtime=manager->sourceMaterialRuntimeMapping()[1];const auto images=manager->animationSource().images;
  auto primitive=primitiveCandidate(editor);const auto newNode=uint32_t(graph.nodeCount());
  primitive.getNode(newNode)->materialIndex=runtime;
  EXPECT_NO_THROW((void)manager->prepareEditorPrimitiveSceneRootRegistration(graph,primitive,newNode));
  auto retire=manager->prepareMaterialRetirement(runtime,graph,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,retire));retire={};
  EXPECT_THROW((void)manager->prepareEditorPrimitiveSceneRootRegistration(graph,primitive,newNode),std::invalid_argument);
  const auto tombstones=manager->animationSource().materials;
  for(const auto extension:{".gltf",".glb"}) {
    const auto path=temp/(std::string("retired-material")+extension);manager->saveScene(graph,path);
    tinygltf::TinyGLTF reader;tinygltf::Model loaded;std::string error,warning;
    const bool ok=std::string_view(extension)==".glb"?reader.LoadBinaryFromFile(&loaded,&error,&warning,path.string()):
        reader.LoadASCIIFromFile(&loaded,&error,&warning,path.string());
    ASSERT_TRUE(ok)<<error<<warning;EXPECT_EQ(loaded.materials.size(),count-1);
    EXPECT_TRUE(std::ranges::none_of(loaded.materials,[](const auto& m){return m.name=="Delete this middle row";}));
    EXPECT_TRUE(std::ranges::any_of(loaded.materials,[](const auto& m){return m.name=="Surviving suffix";}));
    ASSERT_EQ(loaded.images.size(),images.size());
    EXPECT_EQ(loaded.images[0].width,1);EXPECT_EQ(loaded.images[0].height,1);EXPECT_EQ(loaded.images[0].component,4);
    EXPECT_EQ(loaded.images[0].image,(std::vector<unsigned char>{117,47,109,255}));
    tinygltf::TinyGLTF encodedReader;tinygltf::Model encoded;
    encodedReader.SetImageLoader([](tinygltf::Image* image,int,std::string*,std::string*,int,int,
        const unsigned char* bytes,int length,void*) {
      if(!bytes || length<=0 || length>1024*1024) return false;
      image->image.assign(bytes,bytes+length);image->as_is=true;return true;
    },nullptr);
    const bool encodedOk=std::string_view(extension)==".glb"?encodedReader.LoadBinaryFromFile(&encoded,&error,&warning,path.string()):
        encodedReader.LoadASCIIFromFile(&encoded,&error,&warning,path.string());
    ASSERT_TRUE(encodedOk)<<error<<warning;ASSERT_EQ(encoded.images.size(),images.size());
    EXPECT_EQ(encoded.images[0].image,images[0].image);
    for(const auto& mesh:loaded.meshes) for(const auto& primitive:mesh.primitives)
      EXPECT_LT(primitive.material,int32_t(loaded.materials.size()));
  }
  EXPECT_EQ(manager->materialCount(),count);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);
  EXPECT_EQ(manager->animationSource().materials,tombstones);EXPECT_EQ(manager->animationSource().images,images);
  EXPECT_FALSE(manager->materialIsActive(runtime));
}
using LiveResourceCounts=std::vector<std::pair<container::util::ResourceHandle,uint64_t>>;
LiveResourceCounts liveResourceCounts(const TextureResourceLifecycleGenerationPtr& generation) {
  LiveResourceCounts result;const auto inventory=generation->inventory();
  for(const auto& row:inventory.rows())
    result.emplace_back(row.resource,row.pins[size_t(container::util::ResourcePinClass::LiveDependency)]);
  return result;
}

TEST_F(SceneMergeGenerationGpu,SharedCloneHistoryRestagesMaterialControlsAfterFactorUndo) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);
  const auto original=manager->authoredMaterial(0);
  const auto first=uint32_t(graph.nodeCount());const auto oldView=graph.activeRoots();
  auto clone=manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"Dependency clone");
  const auto root=clone.cloneRoot();auto append=graph.prepareAppend(clone.graph());
  ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));exchangeMerge(clone);
  clone.releasePreparationData();
  ASSERT_FALSE(manager->sourceMaterialRuntimeMapping().empty());
  const auto runtime=manager->sourceMaterialRuntimeMapping()[0];
  const auto inactiveOwner=SceneTextureImageOwnershipTestAccess::materialOwner(clone.manager(),runtime);
  ASSERT_TRUE(inactiveOwner);ASSERT_TRUE(inactiveOwner->active());
  const auto counts=liveResourceCounts(manager->resourceLifecycleGeneration());
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Roughness,.23}};
  const auto command=manager->planMaterialEdit(0,patch);
  auto edit=manager->prepareMaterialGeneration(command,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,edit));edit={};
  EXPECT_FALSE(inactiveOwner->active());
  auto undo=manager->prepareMaterialGeneration(manager->rebaseMaterialEdit(command,GltfMaterialEditSide::Before),nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,undo));undo={};
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),original));
  EXPECT_FALSE(inactiveOwner->active());
  const auto currentOwner=SceneTextureImageOwnershipTestAccess::materialOwner(*manager,runtime);
  ASSERT_TRUE(currentOwner);EXPECT_TRUE(currentOwner->active());EXPECT_NE(currentOwner,inactiveOwner);
  EXPECT_TRUE(SceneTextureImageOwnershipTestAccess::materialDependenciesValid(*manager));
  EXPECT_EQ(liveResourceCounts(manager->resourceLifecycleGeneration()),counts);

  // The inactive clone still contains the old, now inactive controls. Replay
  // must stage fresh edges from the actual current owners before graph commit.
  clone.prepareResourceDependencies(*manager);
  ASSERT_TRUE(clone.resourceDependenciesReadyForExchange(*manager));
  auto detach=graph.prepareDetachTail(first,oldView);ASSERT_TRUE(detach);ASSERT_TRUE(graph.publishTailChange(detach));
  auto tail=detach.takeDetachedTail();ASSERT_TRUE(tail);size_t attempted{};
  {
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMergeGeneration(clone);attempted=guard.allocations();
  }
  EXPECT_EQ(attempted,0u);clone.releaseResourceDependencyPreparation();
  EXPECT_EQ(graph.nodeCount(),first);
  EXPECT_TRUE(SceneTextureImageOwnershipTestAccess::materialDependenciesValid(*manager));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->valid());
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->liveDependenciesValid());
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),original));
  EXPECT_EQ(liveResourceCounts(manager->resourceLifecycleGeneration()),counts);
  EXPECT_NE(manager->descriptorSet(0),VK_NULL_HANDLE);

  clone.prepareResourceDependencies(*manager);
  ASSERT_TRUE(clone.resourceDependenciesReadyForExchange(*manager));
  auto restore=graph.prepareRestoreTail(*tail);ASSERT_TRUE(restore);ASSERT_TRUE(graph.publishTailChange(restore));
  {
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMergeGeneration(clone);attempted=guard.allocations();
  }
  EXPECT_EQ(attempted,0u);clone.releaseResourceDependencyPreparation();
  EXPECT_TRUE(graph.isNodeActive(root));
  EXPECT_TRUE(SceneTextureImageOwnershipTestAccess::materialDependenciesValid(*manager));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->liveDependenciesValid());
  EXPECT_EQ(liveResourceCounts(manager->resourceLifecycleGeneration()),counts);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),original));
}

TEST_F(SceneMergeGenerationGpu,SharedCloneDependencyReceiptRejectsStaleAndSurvivesDeniedRefresh) {
  using namespace container::geometry::gltf;
  load();MaterialFrames frames(*allocation);const auto first=uint32_t(graph.nodeCount());const auto oldView=graph.activeRoots();
  auto clone=manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"Stale dependency clone");
  auto append=graph.prepareAppend(clone.graph());ASSERT_TRUE(append);ASSERT_TRUE(graph.publishAppend(append));
  exchangeMerge(clone);clone.releasePreparationData();
  clone.prepareResourceDependencies(*manager);ASSERT_TRUE(clone.resourceDependenciesReadyForExchange(*manager));
  GltfMaterialPatch patch;patch.values={{GltfMaterialProperty::Roughness,.38}};
  const auto command=manager->planMaterialEdit(0,patch);
  auto edit=manager->prepareMaterialGeneration(command,nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,edit));edit={};
  ASSERT_FALSE(clone.resourceDependenciesReadyForExchange(*manager));
  const auto epoch=manager->generationEpoch();const auto generation=manager->resourceLifecycleGeneration();
  const auto descriptor=manager->descriptorSet(0);const auto material=manager->authoredMaterial(0);
  size_t attempted{};
  {
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMergeGeneration(clone);attempted=guard.allocations();
  }
  EXPECT_EQ(attempted,0u);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);EXPECT_EQ(manager->descriptorSet(0),descriptor);
  EXPECT_TRUE(GltfMaterialRecordsEqual(manager->authoredMaterial(0),material));
  EXPECT_TRUE(SceneTextureImageOwnershipTestAccess::materialDependenciesValid(*manager));

  auto undo=manager->prepareMaterialGeneration(manager->rebaseMaterialEdit(command,GltfMaterialEditSide::Before),nullptr);
  ASSERT_TRUE(publishMaterialTestGeneration(*manager,frames,undo));undo={};
  clone.prepareResourceDependencies(*manager);ASSERT_TRUE(clone.resourceDependenciesReadyForExchange(*manager));
  const auto preparedBytes=clone.resourceDependencyPreparationBytes();
  const auto readyEpoch=manager->generationEpoch(),graphRevision=graph.revision();
  const auto readyGeneration=manager->resourceLifecycleGeneration();const auto allocated=allocations();
  const auto counts=liveResourceCounts(readyGeneration);bool denied=false;
  {
    material_publication_test::AllocationGuard guard(true);
    try {clone.prepareResourceDependencies(*manager);} catch(const std::bad_alloc&) {denied=true;}
    attempted=guard.allocations();
  }
  EXPECT_TRUE(denied);EXPECT_GT(attempted,0u);
  EXPECT_EQ(clone.resourceDependencyPreparationBytes(),preparedBytes);
  ASSERT_TRUE(clone.resourceDependenciesReadyForExchange(*manager));
  EXPECT_EQ(manager->generationEpoch(),readyEpoch);EXPECT_EQ(graph.revision(),graphRevision);
  EXPECT_EQ(manager->resourceLifecycleGeneration(),readyGeneration);EXPECT_EQ(allocations(),allocated);
  EXPECT_EQ(liveResourceCounts(readyGeneration),counts);
  auto detach=graph.prepareDetachTail(first,oldView);ASSERT_TRUE(detach);ASSERT_TRUE(graph.publishTailChange(detach));
  auto tail=detach.takeDetachedTail();ASSERT_TRUE(tail);
  {
    material_publication_test::AllocationGuard guard(true);
    manager->exchangeMergeGeneration(clone);attempted=guard.allocations();
  }
  EXPECT_EQ(attempted,0u);clone.releaseResourceDependencyPreparation();
  EXPECT_TRUE(SceneTextureImageOwnershipTestAccess::materialDependenciesValid(*manager));
  EXPECT_TRUE(manager->resourceLifecycleGeneration()->liveDependenciesValid());
  EXPECT_EQ(liveResourceCounts(manager->resourceLifecycleGeneration()),counts);
}

TEST_F(SceneMergeGenerationGpu,MergeDependencyReceiptRejectsDestroyedOrReplacedBorrowedPlayerWithoutAllocation) {
  using namespace container::geometry::gltf;
  load();auto clone=manager->prepareSubtreeCloneGeneration(graph,sourceNode(0),"Borrowed player clone");
  auto borrowed=std::make_unique<AnimationPlayer>(clone.manager().animationSource());
  clone.prepareResourceDependencies(*manager,borrowed.get());
  ASSERT_TRUE(clone.resourceDependenciesReadyForExchange(*manager,borrowed.get()));
  borrowed.reset();
  // A stack replacement cannot reuse the destroyed heap object's address.
  AnimationPlayer replacement(clone.manager().animationSource());
  const auto epoch=manager->generationEpoch(),graphRevision=graph.revision();
  const auto generation=manager->resourceLifecycleGeneration();const auto descriptor=manager->descriptorSet(0);
  const auto counts=liveResourceCounts(generation);bool defaultReady=true,replacementReady=true;size_t attempted{};
  {
    material_publication_test::AllocationGuard guard(true);
    defaultReady=clone.resourceDependenciesReadyForExchange(*manager);
    replacementReady=clone.resourceDependenciesReadyForExchange(*manager,&replacement);
    manager->exchangeMergeGeneration(clone);
    manager->exchangeMergeGeneration(clone,&replacement);attempted=guard.allocations();
  }
  EXPECT_FALSE(defaultReady);EXPECT_FALSE(replacementReady);EXPECT_EQ(attempted,0u);
  EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(graph.revision(),graphRevision);
  EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);EXPECT_EQ(manager->descriptorSet(0),descriptor);
  EXPECT_EQ(liveResourceCounts(generation),counts);
  EXPECT_TRUE(SceneTextureImageOwnershipTestAccess::materialDependenciesValid(*manager));
  clone.prepareResourceDependencies(*manager,&replacement);
  EXPECT_TRUE(clone.resourceDependenciesReadyForExchange(*manager,&replacement));
}
} // namespace
