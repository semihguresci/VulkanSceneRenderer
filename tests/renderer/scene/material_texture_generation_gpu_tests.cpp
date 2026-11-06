#include <Container/common/CommonGLFW.h>
#include <Container/geometry/GltfModelLoader.h>
#include <Container/geometry/GltfSceneExport.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/GltfMaterialConversion.h>
#include <Container/utility/GltfRuntimeMaterialAuthoring.h>
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

namespace {
using namespace container::scene;
using namespace container::geometry;
using namespace container::gpu;

static_assert(!std::is_copy_constructible_v<SceneManager::PreparedMaterialResources>);
static_assert(std::is_nothrow_move_constructible_v<SceneManager::PreparedMaterialResources>);
static_assert(std::is_nothrow_move_assignable_v<SceneManager::PreparedMaterialResources>);
static_assert(noexcept(std::declval<const SceneManager&>().canExchangeMaterialResources(
    std::declval<const SceneManager::PreparedMaterialResources&>())));
static_assert(noexcept(std::declval<SceneManager&>().exchangeMaterialResources(
    std::declval<SceneManager::PreparedMaterialResources&>())));

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

class MaterialTextureGenerationGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<MaterialTextureGenerationGpu*>(user)->errors;
      std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_MATERIAL_TEXTURES");
    if(!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_MATERIAL_TEXTURES=1";
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
    info.next=&v12;
    device=std::make_shared<VulkanDevice>(*instance,surfaceHandle,info);
    pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,device->queueFamilyIndices().graphicsFamily.value()));
    allocation=std::make_unique<AllocationManager>();
    allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),
        device->graphicsQueue(),*pool,{});
    pipelines=std::make_unique<PipelineManager>(device->device());
    temp=std::filesystem::temp_directory_path()/("container-material-textures-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();
    manager.reset();pipelines.reset();allocation.reset();pool.clear();device.reset();
    surface.clear();messenger.clear();instance.reset();
    if(window) glfwDestroyWindow(window);if(glfwStarted) glfwTerminate();
    if(!temp.empty()) { std::error_code ignored; const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()); const auto target=std::filesystem::weakly_canonical(temp); if(target.parent_path()==parent && target.filename().string().starts_with("container-material-textures-")) std::filesystem::remove_all(target,ignored); }
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

// These tests inspect actual Vulkan allocations and host metadata, not shader
// sampling or pixel readback. Only final publication denies host allocation.
bool publishResources(SceneManager& manager,SceneManager::PreparedMaterialResources& stage) {
  EXPECT_FALSE(manager.canExchangeMaterialResources(stage));
  stage.sealLiveDependencies();
  if(!manager.canExchangeMaterialResources(stage)) return false;
  size_t attempted{};
  {material_publication_test::AllocationGuard guard(true);manager.exchangeMaterialResources(stage);attempted=guard.allocations();}
  return !stage && attempted==0;
}
bool publishMaterial(SceneManager& manager,MaterialFrames& frames,SceneManager::PreparedMaterialGeneration& stage) {
  frames.bind(stage);
  if(!manager.canExchangeMaterialGeneration(stage,nullptr)) return false;
  size_t attempted{};
  {material_publication_test::AllocationGuard guard(true);manager.exchangeMaterialGeneration(stage,nullptr);attempted=guard.allocations();}
  return !stage && attempted==0;
}
container::geometry::gltf::GltfMaterialPatch texturePatch(
    const SceneManager::PreparedMaterialResources& resources,bool all=false) {
  using namespace container::geometry::gltf;
  GltfMaterialPatch patch;
  for(const auto& slot:GltfMaterialTextureSlotRegistry()) {
    if(!all && slot.slot!=GltfMaterialTextureSlot::BaseColor) continue;
    GltfMaterialTextureEdit edit;edit.slot=slot.slot;edit.operation=GltfMaterialTextureOperation::Bind;
    edit.binding=GltfMaterialTextureSourcePair{int32_t(resources.sourceTexture()),int32_t(resources.evaluatedTexture())};
    patch.textures.push_back(edit);
  }
  return patch;
}

GltfLoadResult staticUv1NormalTriangle() {
  auto source=triangle("Static UV1 normal",31);auto& model=source.gltfModel;
  model.meshes[0].primitives[0].attributes["TEXCOORD_1"]=cloneAccessor(model,std::array<float,6>{0,0,0,1,1,0},
      TINYGLTF_TYPE_VEC2,TINYGLTF_COMPONENT_TYPE_FLOAT,3);
  model.materials[0].pbrMetallicRoughness.baseColorTexture.index=-1;
  model.materials[0].normalTexture.index=0;model.materials[0].normalTexture.texCoord=0;
  model.materials[0].normalTexture.extensions["KHR_texture_transform"]=tinygltf::Value(tinygltf::Value::Object{{"texCoord",tinygltf::Value(1)}});
  model.extensionsUsed.push_back("KHR_texture_transform");return source;
}

TEST_F(MaterialTextureGenerationGpu,StaticPrimitiveBasisLookupNeedsSourceIdentityAndActualTexCoordWithoutAllocating) {
  load();ASSERT_EQ(manager->deformationData(),nullptr);const auto runtime=manager->sourcePrimitiveToRuntime()[0];
  size_t attempts{};std::optional<uint32_t> uv;
  {material_publication_test::AllocationGuard guard(true);uv=manager->retainedPrimitiveNormalTexCoord(runtime);attempts=guard.allocations();}
  EXPECT_EQ(uv,std::optional<uint32_t>(0));EXPECT_EQ(attempts,0u);EXPECT_FALSE(manager->retainedPrimitiveNormalTexCoord(UINT32_MAX));
  auto& attributes=manager->animationSource().meshes[0].primitives[0].attributes;const auto accessor=attributes.at("TEXCOORD_0");attributes.erase("TEXCOORD_0");
  {material_publication_test::AllocationGuard guard(true);uv=manager->retainedPrimitiveNormalTexCoord(runtime);attempts=guard.allocations();}
  EXPECT_FALSE(uv);EXPECT_EQ(attempts,0u);attributes["TEXCOORD_0"]=accessor;
  auto& mapping=const_cast<std::vector<uint32_t>&>(manager->sourcePrimitiveToRuntime());mapping[0]=UINT32_MAX;
  {material_publication_test::AllocationGuard guard(true);uv=manager->retainedPrimitiveNormalTexCoord(runtime);attempts=guard.allocations();}
  EXPECT_FALSE(uv);EXPECT_EQ(attempts,0u);mapping[0]=runtime;
  const auto appended=manager->appendRuntimeMesh(manager->model().meshes()[runtime]);
  EXPECT_FALSE(manager->retainedPrimitiveNormalTexCoord(appended));EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(0));
}

TEST_F(MaterialTextureGenerationGpu,StaticUv1NormalClearAndUndoRetainTheImportedBasis) {
  using namespace container::geometry::gltf;loadSource(staticUv1NormalTriangle());MaterialFrames frames(*allocation);
  ASSERT_EQ(manager->deformationData(),nullptr);const auto runtime=manager->sourcePrimitiveToRuntime()[0],material=manager->sourceMaterialRuntimeMapping()[0];
  EXPECT_EQ(manager->materialNormalTexCoord(material),std::optional<uint32_t>(1));EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));
  GltfMaterialPatch patch;GltfMaterialTextureEdit clear;clear.slot=GltfMaterialTextureSlot::Normal;clear.operation=GltfMaterialTextureOperation::Clear;patch.textures.push_back(clear);
  const auto command=manager->planMaterialTextureEdit(0,patch);auto stage=manager->prepareMaterialGeneration(command,nullptr);
  ASSERT_TRUE(publishMaterial(*manager,frames,stage));stage={};
  EXPECT_FALSE(manager->materialNormalTexCoord(material));EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));
  const auto before=manager->rebaseMaterialTextureEdit(command,GltfMaterialEditSide::Before);auto undo=manager->prepareMaterialGeneration(before,nullptr);
  ASSERT_TRUE(publishMaterial(*manager,frames,undo));undo={};
  EXPECT_EQ(manager->materialNormalTexCoord(material),std::optional<uint32_t>(1));EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));
  const auto after=manager->rebaseMaterialTextureEdit(command,GltfMaterialEditSide::After);auto redo=manager->prepareMaterialGeneration(after,nullptr);
  ASSERT_TRUE(publishMaterial(*manager,frames,redo));redo={};
  EXPECT_FALSE(manager->materialNormalTexCoord(material));EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));
}

TEST_F(MaterialTextureGenerationGpu,StaticUv1BasisNeverFallsBackToUv0OrFilledVertexCoordinates) {
  auto source=staticUv1NormalTriangle();source.gltfModel.meshes[0].primitives[0].attributes.erase("TEXCOORD_0");loadSource(std::move(source));
  const auto runtime=manager->sourcePrimitiveToRuntime()[0];ASSERT_EQ(manager->deformationData(),nullptr);
  EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));
  auto& attributes=manager->animationSource().meshes[0].primitives[0].attributes;const auto accessor=attributes.at("TEXCOORD_1");
  attributes.erase("TEXCOORD_1");attributes["TEXCOORD_0"]=accessor;
  EXPECT_FALSE(manager->retainedPrimitiveNormalTexCoord(runtime));
}

TEST_F(MaterialTextureGenerationGpu,StaticClearedUv1BasisSurvivesDetachedCloneAndMerge) {
  using namespace container::geometry::gltf;loadSource(staticUv1NormalTriangle());MaterialFrames frames(*allocation);
  const auto runtime=manager->sourcePrimitiveToRuntime()[0];
  GltfMaterialPatch patch;GltfMaterialTextureEdit clear;clear.slot=GltfMaterialTextureSlot::Normal;clear.operation=GltfMaterialTextureOperation::Clear;patch.textures.push_back(clear);
  auto stage=manager->prepareMaterialGeneration(manager->planMaterialTextureEdit(0,patch),nullptr);ASSERT_TRUE(publishMaterial(*manager,frames,stage));stage={};
  EXPECT_FALSE(manager->materialNormalTexCoord(manager->sourceMaterialRuntimeMapping()[0]));
  {
    auto clone=manager->prepareSubtreeCloneGeneration(graph,sourceNode(0));
    EXPECT_EQ(clone.manager().retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));EXPECT_EQ(clone.manager().deformationData(),nullptr);
  }
  {
    auto merge=manager->prepareMergeGeneration(triangle("UV0 incoming",71),temp/"incoming.gltf",{},graph);
    const auto& candidate=merge.manager();EXPECT_EQ(candidate.retainedPrimitiveNormalTexCoord(candidate.sourcePrimitiveToRuntime()[0]),std::optional<uint32_t>(1));
    EXPECT_EQ(candidate.retainedPrimitiveNormalTexCoord(candidate.sourcePrimitiveToRuntime()[1]),std::optional<uint32_t>(0));
  }
  EXPECT_EQ(manager->retainedPrimitiveNormalTexCoord(runtime),std::optional<uint32_t>(1));
}

TEST_F(MaterialTextureGenerationGpu,ImportPreparationIsDetachedAndDiscardReleasesItsAllocations) {
  loadSource(triangle("A",31,false,false));const auto bytes=producerPng(temp,"detached");
  const auto archive=manager->buildSceneExport(graph),evaluated=manager->animationSource();
  const auto revision=manager->materialResourceRevision(),epoch=manager->generationEpoch(),material=manager->materialRevision();
  const auto descriptor=manager->descriptorSet(0),auxiliary=manager->auxiliaryDescriptorSet(1);const auto baseline=allocations();
  const auto* geometry=manager->animationSource().buffers[0].data.data();
  SceneManager::MaterialResourceCommand abandoned;
  {
    auto stage=manager->prepareMaterialTextureBytes(0,"detached",bytes,MaterialTextureRoles::ColorAndData);
    abandoned=stage.command();EXPECT_TRUE(abandoned);EXPECT_FALSE(abandoned.published());EXPECT_GT(abandoned.ownedBytes(),0u);
    const auto color=stage.reference(true),data=stage.reference(false);
    EXPECT_NE(color.runtimeResource(),data.runtimeResource());EXPECT_EQ(color.archiveTexture(),data.archiveTexture());
    EXPECT_EQ(color.evaluatedTexture(),data.evaluatedTexture());EXPECT_TRUE(color.isSrgb());EXPECT_FALSE(data.isSrgb());
    EXPECT_TRUE(stage.matchesReference(color,true,color.runtimeResource()));EXPECT_FALSE(stage.matchesReference(color,false,color.runtimeResource()));
    EXPECT_NE(stage.metadataBuffer().buffer,VK_NULL_HANDLE);EXPECT_FALSE(stage.imageInfos().empty());EXPECT_GT(stage.ownedBytes(),abandoned.ownedBytes());
    EXPECT_FALSE(manager->canExchangeMaterialResources(stage));stage.sealLiveDependencies();
    EXPECT_TRUE(manager->canExchangeMaterialResources(stage));EXPECT_GT(allocations(),baseline);
    EXPECT_EQ(manager->buildSceneExport(graph),archive);EXPECT_EQ(manager->animationSource(),evaluated);
    EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->auxiliaryDescriptorSet(1),auxiliary);
    EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_EQ(manager->materialRevision(),material);EXPECT_EQ(manager->generationEpoch(),epoch);
    EXPECT_EQ(manager->animationSource().buffers[0].data.data(),geometry);
    auto moved=std::move(stage);EXPECT_TRUE(moved);EXPECT_FALSE(stage);
  }
  EXPECT_EQ(allocations(),baseline+2);EXPECT_FALSE(abandoned.published()); // The retained command still owns both image leases.
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(abandoned,false),std::invalid_argument);
  abandoned={};EXPECT_EQ(allocations(),baseline);
}

TEST_F(MaterialTextureGenerationGpu,ImportAndExactByteReuseKeepExplicitSourceRoles) {
  loadSource(triangle("A",31,false,false));const auto bytes=producerPng(temp,"reuse");
  const auto revision=manager->materialResourceRevision(),epoch=manager->generationEpoch(),material=manager->materialRevision();
  auto stage=manager->prepareMaterialTextureBytes(0,"reuse",bytes,MaterialTextureRoles::ColorAndData);
  const auto color=stage.reference(true),data=stage.reference(false);const auto command=stage.command();
  ASSERT_TRUE(publishResources(*manager,stage));stage={};
  EXPECT_TRUE(command.present());EXPECT_EQ(manager->materialResourceRevision(),revision+1);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->materialRevision(),material);
  const auto sourceCount=manager->animationSource().textures.size(),runtimeCount=manager->materialTextureResourceCount();
  auto again=manager->prepareMaterialTextureBytes(0,"reuse",bytes,MaterialTextureRoles::ColorAndData);
  EXPECT_EQ(again.reference(true),color);EXPECT_EQ(again.reference(false),data);
  ASSERT_TRUE(again.textureImageOwnership());EXPECT_EQ(again.textureImageOwnership()->imageCount(),2u);
  EXPECT_EQ(again.evaluatedSourceView().textureCount,sourceCount);EXPECT_EQ(again.textures().textureCount(),runtimeCount);
  EXPECT_EQ(manager->authoredMaterialResources().archiveToEvaluated[color.archiveTexture()],int32_t(*color.evaluatedTexture()));
  EXPECT_EQ(manager->authoredMaterialResources().colorResources[color.archiveTexture()],color.runtimeResource());
  EXPECT_EQ(manager->authoredMaterialResources().dataResources[data.archiveTexture()],data.runtimeResource());
}

TEST_F(MaterialTextureGenerationGpu,SamplerAliasReusesImageLeaseAndStagesDenseMetadata) {
  loadSource(triangle("A",31,false,false));const auto bytes=producerPng(temp,"sampler");
  auto initial=manager->prepareMaterialTextureBytes(0,"sampler",bytes,MaterialTextureRoles::Color);
  const auto source=initial.sourceTexture(),runtime=initial.reference(true).runtimeResource();const auto image=initial.textures().getTexture(runtime)->imageView;
  ASSERT_TRUE(publishResources(*manager,initial));initial={};
  const auto images=manager->animationSource().images.size(),samplers=manager->animationSource().samplers.size();const auto baseline=allocations();
  const auto policy=container::material::NormalizeMaterialSamplerKey(TINYGLTF_TEXTURE_FILTER_NEAREST,TINYGLTF_TEXTURE_FILTER_LINEAR,TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE,TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT);
  auto alias=manager->prepareMaterialTextureSource(0,source,MaterialTextureRoles::Color,policy.value);
  const auto reference=alias.reference(true);ASSERT_NE(reference.archiveTexture(),source);ASSERT_NE(reference.runtimeResource(),runtime);
  EXPECT_EQ(alias.textures().getTexture(reference.runtimeResource())->imageView,image);EXPECT_EQ(allocations(),baseline+1);
  EXPECT_EQ(manager->animationSource().images.size(),images);EXPECT_EQ(alias.evaluatedSourceView().textureCount,manager->animationSource().textures.size()+1);
  const auto* metadata=static_cast<const GpuTextureMetadata*>(alias.metadataBuffer().allocation_info.pMappedData);ASSERT_NE(metadata,nullptr);
  EXPECT_NE(metadata[runtime].samplerIndex,metadata[reference.runtimeResource()].samplerIndex);
  EXPECT_LT(metadata[reference.runtimeResource()].samplerIndex,alias.descriptorSamplers().size());
  ASSERT_TRUE(publishResources(*manager,alias));alias={};
  EXPECT_EQ(manager->animationSource().images.size(),images);EXPECT_EQ(manager->animationSource().samplers.size(),samplers+1);
  const auto& record=manager->animationSource().samplers.back();EXPECT_EQ(record.minFilter,TINYGLTF_TEXTURE_FILTER_NEAREST);
  EXPECT_EQ(record.magFilter,TINYGLTF_TEXTURE_FILTER_LINEAR);EXPECT_EQ(record.wrapS,TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);EXPECT_EQ(record.wrapT,TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT);
}

TEST_F(MaterialTextureGenerationGpu,ImmediateImportsRemainAvailableAfterIndependentEvaluatedImageDomains) {
  load();const auto bytes=producerPng(temp,"independent");
  auto extra=manager->animationSource().images[0];extra.name="Evaluated-only retained image";manager->animationSource().images.push_back(extra);
  auto detached=manager->prepareMaterialTextureBytes(0,"independent",bytes,MaterialTextureRoles::Color);
  const auto authored=detached.sourceTexture(),evaluated=detached.evaluatedTexture();ASSERT_TRUE(publishResources(*manager,detached));detached={};
  EXPECT_NE(manager->authoredMaterialResources().textures[authored].source,manager->animationSource().textures[evaluated].source);
  const auto next=manager->loadMaterialTextureSourceFromBytes("later-bim",bytes,true);
  ASSERT_TRUE(next);ASSERT_TRUE(next.evaluatedTexture());EXPECT_EQ(manager->authoredMaterialResources().colorResources[next.archiveTexture()],next.runtimeResource());
  EXPECT_EQ(manager->gltfTextureResources().color[*next.evaluatedTexture()],next.runtimeResource());
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[0]));
}

TEST_F(MaterialTextureGenerationGpu,ArchiveOnlyPromotionForwardsExplicitUnmappedProvenance) {
  load();const auto bytes=producerPng(temp,"archive-only");
  const auto images=manager->animationSource().images.size(),samplers=manager->animationSource().samplers.size(),textures=manager->animationSource().textures.size();
  const auto archived=manager->loadMaterialTextureSourceFromBytes("archive-only",bytes,true);
  auto& evaluated=manager->animationSource();evaluated.images.resize(images);evaluated.samplers.resize(samplers);evaluated.textures.resize(textures);
  auto& roles=const_cast<container::material::GltfTextureResources&>(manager->gltfTextureResources());roles.color.resize(textures);roles.data.resize(textures);
  auto mapping=manager->authoredMaterialResources().archiveToEvaluated;const_cast<int32_t&>(mapping[archived.archiveTexture()])=-1;
  const auto unmapped=manager->loadMaterialTextureSourceFromBytes("archive-only",bytes,true);ASSERT_FALSE(unmapped.evaluatedTexture());
  container::material::Material material;material.baseColorTextureIndex=unmapped.runtimeResource();SceneManager::MaterialTextureProvenance provenance;provenance[0]=unmapped;
  const auto runtime=manager->createMaterial(material,provenance);ASSERT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));
  auto stage=manager->prepareMaterialTextureSource(0,unmapped.archiveTexture(),MaterialTextureRoles::Color);
  const auto promoted=stage.reference(true);ASSERT_TRUE(promoted.evaluatedTexture());EXPECT_EQ(promoted.runtimeResource(),unmapped.runtimeResource());
  EXPECT_EQ(stage.textures().textureCount(),manager->materialTextureResourceCount());
  ASSERT_TRUE(publishResources(*manager,stage));stage={};EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));
  EXPECT_EQ(manager->animationSource().textures.size(),textures+1);EXPECT_EQ(manager->animationSource().buffers.size(),1u);
}

TEST_F(MaterialTextureGenerationGpu,IndependentArchiveAndEvaluatedTextureIdsPublishAndReplayTogether) {
  using namespace container::geometry::gltf;load();MaterialFrames frames(*allocation);
  const auto images=manager->animationSource().images.size(),samplers=manager->animationSource().samplers.size(),textures=manager->animationSource().textures.size();
  const auto first=manager->loadMaterialTextureSourceFromBytes("unmapped-first",producerPng(temp,"unmapped-first",91),true);
  const auto second=manager->loadMaterialTextureSourceFromBytes("unmapped-second",producerPng(temp,"unmapped-second",173),true);
  auto& evaluated=manager->animationSource();evaluated.images.resize(images);evaluated.samplers.resize(samplers);evaluated.textures.resize(textures);
  auto& roles=const_cast<container::material::GltfTextureResources&>(manager->gltfTextureResources());roles.color.resize(textures);roles.data.resize(textures);
  auto mapping=manager->authoredMaterialResources().archiveToEvaluated;
  const_cast<int32_t&>(mapping[first.archiveTexture()])=-1;const_cast<int32_t&>(mapping[second.archiveTexture()])=-1;
  auto resources=manager->prepareMaterialTextureSource(0,second.archiveTexture(),MaterialTextureRoles::Color);
  ASSERT_NE(resources.sourceTexture(),resources.evaluatedTexture());const auto command=resources.command();
  const auto original=manager->planMaterialTextureEdit(0,texturePatch(resources),&resources);
  auto stage=manager->prepareMaterialGeneration(original,nullptr,std::move(resources));ASSERT_TRUE(publishMaterial(*manager,frames,stage));stage={};
  EXPECT_EQ(manager->authoredMaterial(0).pbrMetallicRoughness.baseColorTexture.index,int(second.archiveTexture()));
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.baseColorTexture.index,int(textures));
  auto remove=manager->prepareMaterialResourcesChange(command,false);auto before=manager->rebaseMaterialTextureEdit(original,GltfMaterialEditSide::Before,&remove);
  auto undo=manager->prepareMaterialGeneration(before,nullptr,std::move(remove));ASSERT_TRUE(publishMaterial(*manager,frames,undo));undo={};
  EXPECT_EQ(evaluated.textures.size(),textures);EXPECT_EQ(manager->authoredMaterialResources().textures.size(),textures+2);
  auto restore=manager->prepareMaterialResourcesChange(command,true);auto after=manager->rebaseMaterialTextureEdit(original,GltfMaterialEditSide::After,&restore);
  auto redo=manager->prepareMaterialGeneration(after,nullptr,std::move(restore));ASSERT_TRUE(publishMaterial(*manager,frames,redo));redo={};
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[0]));
}

TEST_F(MaterialTextureGenerationGpu,FiniteBudgetsAndInvalidInputsLeaveLiveResourcesUntouched) {
  loadSource(triangle("A",31,false,false));const auto bytes=producerPng(temp,"limits");
  const auto archive=manager->buildSceneExport(graph);const auto baseline=allocations();const auto count=manager->materialTextureResourceCount();
  const auto revision=manager->materialResourceRevision();const auto unchanged=[&] {
    EXPECT_EQ(manager->buildSceneExport(graph),archive);EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->materialTextureResourceCount(),count);EXPECT_EQ(manager->materialResourceRevision(),revision);
  };
  EXPECT_THROW((void)manager->prepareMaterialTextureBytes(0,"limits",bytes,MaterialTextureRoles(0)),std::invalid_argument);unchanged();
  EXPECT_THROW((void)manager->prepareMaterialTextureBytes(0,"limits",bytes,MaterialTextureRoles::Color,108),std::invalid_argument);unchanged();
  EXPECT_THROW((void)manager->prepareMaterialTextureFile(0,temp/"missing.png",MaterialTextureRoles::Color),std::exception);unchanged();
  const std::array<std::byte,4> corrupt{};EXPECT_THROW((void)manager->prepareMaterialTextureBytes(0,"corrupt",corrupt,MaterialTextureRoles::Color),std::exception);unchanged();
  MaterialResourcesLimits limits;limits.maxPreparedCpuBytes=1;EXPECT_THROW((void)manager->prepareMaterialTextureBytes(0,"limits",bytes,MaterialTextureRoles::Color,0,limits),std::length_error);unchanged();
  limits={};limits.maxEncodedBytes=1;EXPECT_THROW((void)manager->prepareMaterialTextureBytes(0,"limits",bytes,MaterialTextureRoles::Color,0,limits),std::length_error);unchanged();
  limits={};limits.maxNewTextureBytes=1;EXPECT_THROW((void)manager->prepareMaterialTextureBytes(0,"limits",bytes,MaterialTextureRoles::Color,0,limits),std::exception);unchanged();
  limits={};limits.maxMetadataBufferBytes=1;EXPECT_THROW((void)manager->prepareMaterialTextureBytes(0,"limits",bytes,MaterialTextureRoles::ColorAndData,0,limits),std::length_error);unchanged();
}

TEST_F(MaterialTextureGenerationGpu,OwnedTailUndoRedoUsesRetainedPayloadAfterFileDisappears) {
  loadSource(triangle("A",31,false,false));(void)producerPng(temp,"history");
  const auto textures=manager->animationSource().textures.size(),images=manager->animationSource().images.size(),samplers=manager->animationSource().samplers.size(),runtime=manager->materialTextureResourceCount();
  auto stage=manager->prepareMaterialTextureFile(0,temp/"history.png",MaterialTextureRoles::ColorAndData);
  const auto command=stage.command();const auto color=stage.reference(true),data=stage.reference(false);const auto view=stage.textures().getTexture(color.runtimeResource())->imageView;
  ASSERT_TRUE(publishResources(*manager,stage));stage={};const auto retained=allocations();
  ASSERT_TRUE(std::filesystem::remove(temp/"history.png"));
  auto undo=manager->prepareMaterialResourcesChange(command,false);ASSERT_TRUE(publishResources(*manager,undo));undo={};
  EXPECT_FALSE(command.present());EXPECT_EQ(manager->animationSource().textures.size(),textures);EXPECT_EQ(manager->animationSource().images.size(),images);
  EXPECT_EQ(manager->animationSource().samplers.size(),samplers);EXPECT_EQ(manager->materialTextureResourceCount(),runtime);
  for(int iteration=0;iteration<3;++iteration) {
    auto redo=manager->prepareMaterialResourcesChange(command,true);EXPECT_EQ(redo.reference(true),color);EXPECT_EQ(redo.reference(false),data);
    EXPECT_EQ(redo.textures().getTexture(color.runtimeResource())->imageView,view);ASSERT_TRUE(publishResources(*manager,redo));redo={};
    EXPECT_EQ(allocations(),retained);EXPECT_TRUE(command.present());
    auto detach=manager->prepareMaterialResourcesChange(command,false);ASSERT_TRUE(publishResources(*manager,detach));detach={};
    EXPECT_EQ(manager->materialTextureResourceCount(),runtime);EXPECT_EQ(allocations(),retained);
  }
}

TEST_F(MaterialTextureGenerationGpu,NestedHistoryAndBranchVacanciesPreventOldCommandRevival) {
  loadSource(triangle("A",31,false,false));const auto bytes=producerPng(temp,"nested");
  auto a=manager->prepareMaterialTextureBytes(0,"nested",bytes,MaterialTextureRoles::Color);const auto ca=a.command();const auto source=a.sourceTexture();ASSERT_TRUE(publishResources(*manager,a));a={};
  auto b=manager->prepareMaterialTextureSource(0,source,MaterialTextureRoles::Color,container::material::NormalizeMaterialSamplerKey(9728,9728).value);
  const auto cb=b.command();const auto bref=b.reference(true);ASSERT_TRUE(publishResources(*manager,b));b={};
  auto ub=manager->prepareMaterialResourcesChange(cb,false);ASSERT_TRUE(publishResources(*manager,ub));ub={};
  auto ua=manager->prepareMaterialResourcesChange(ca,false);ASSERT_TRUE(publishResources(*manager,ua));ua={};
  auto ra=manager->prepareMaterialResourcesChange(ca,true);ASSERT_TRUE(publishResources(*manager,ra));ra={};
  auto rb=manager->prepareMaterialResourcesChange(cb,true);EXPECT_EQ(rb.reference(true),bref);ASSERT_TRUE(publishResources(*manager,rb));rb={};
  auto ub2=manager->prepareMaterialResourcesChange(cb,false);ASSERT_TRUE(publishResources(*manager,ub2));ub2={};
  auto branch=manager->prepareMaterialTextureSource(0,source,MaterialTextureRoles::Color,container::material::NormalizeMaterialSamplerKey(9729,9728).value);
  const auto alternative=branch.command();ASSERT_TRUE(publishResources(*manager,branch));branch={};
  auto undoBranch=manager->prepareMaterialResourcesChange(alternative,false);ASSERT_TRUE(publishResources(*manager,undoBranch));undoBranch={};
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(cb,true),std::invalid_argument);
}

TEST_F(MaterialTextureGenerationGpu,HistoryPreservesUnrelatedCurrentPrefixMetadataAndMaterialFactors) {
  using namespace container::geometry::gltf;load();MaterialFrames frames(*allocation);const auto bytes=producerPng(temp,"prefix");
  auto stage=manager->prepareMaterialTextureBytes(0,"prefix",bytes,MaterialTextureRoles::Color);const auto command=stage.command();ASSERT_TRUE(publishResources(*manager,stage));stage={};
  manager->animationSource().images[0].extras=tinygltf::Value(tinygltf::Value::Object{{"asciiLabel",tinygltf::Value(std::string("中文 label"))}});
  const auto extras=manager->animationSource().images[0].extras;
  GltfMaterialPatch factor;factor.values={{GltfMaterialProperty::Roughness,.27}};auto material=manager->prepareMaterialGeneration(manager->planMaterialEdit(0,factor),nullptr);
  ASSERT_TRUE(publishMaterial(*manager,frames,material));material={};
  auto undo=manager->prepareMaterialResourcesChange(command,false);ASSERT_TRUE(publishResources(*manager,undo));undo={};
  EXPECT_EQ(manager->animationSource().images[0].extras,extras);EXPECT_DOUBLE_EQ(manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor,.27);
  auto redo=manager->prepareMaterialResourcesChange(command,true);ASSERT_TRUE(publishResources(*manager,redo));redo={};
  EXPECT_EQ(manager->animationSource().images[0].extras,extras);EXPECT_DOUBLE_EQ(manager->authoredMaterial(0).pbrMetallicRoughness.roughnessFactor,.27);
}

TEST_F(MaterialTextureGenerationGpu,UndoRejectsOtherRuntimeAndRetainedImageSamplerReferences) {
  load();const auto bytes=producerPng(temp,"references");auto stage=manager->prepareMaterialTextureBytes(0,"references",bytes,MaterialTextureRoles::Color);
  const auto command=stage.command();const auto reference=stage.reference(true);ASSERT_TRUE(publishResources(*manager,stage));stage={};
  auto& source=manager->animationSource();const auto before=source.textures[0];
  source.textures[0].source=source.textures.back().source;EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);source.textures[0]=before;
  source.textures[0].sampler=source.textures.back().sampler;EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);source.textures[0]=before;
  source.textures[0].extensions["EXT_texture_webp"]=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(source.textures.back().source)}});
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);source.textures[0]=before;
  container::material::Material material;material.baseColorTextureIndex=reference.runtimeResource();SceneManager::MaterialTextureProvenance provenance;provenance[0]=reference;
  const auto runtime=manager->createMaterial(material,provenance);EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);EXPECT_TRUE(command.present());
}

TEST_F(MaterialTextureGenerationGpu,UndoRejectsShadowedFallbackTextureReferencesInOtherMaterials) {
  using namespace container::geometry::gltf;load();MaterialFrames frames(*allocation);
  auto duplicate=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);ASSERT_TRUE(publishMaterial(*manager,frames,duplicate));duplicate={};
  const auto bytes=producerPng(temp,"shadowed");auto stage=manager->prepareMaterialTextureBytes(0,"shadowed",bytes,MaterialTextureRoles::Color);
  const auto command=stage.command();const auto texture=stage.evaluatedTexture();ASSERT_TRUE(publishResources(*manager,stage));stage={};
  tinygltf::Parameter prefix,fallback;prefix.json_double_value["index"]=0.;fallback.json_double_value["index"]=double(texture);
  manager->animationSource().materials.back().values["roughnessTexture"]=prefix;
  manager->animationSource().materials.back().additionalValues["roughnessTexture"]=fallback;
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);EXPECT_TRUE(command.present());
}

TEST_F(MaterialTextureGenerationGpu,UnchangedVendorExtensionsAllowUndoButChangedUnknownReferencesReject) {
  auto source=triangle("A",31,false,false);source.gltfModel.nodes[0].extensions["VENDOR_link"]=tinygltf::Value(tinygltf::Value::Object{{"index",tinygltf::Value(700)}});
  loadSource(std::move(source));const auto bytes=producerPng(temp,"vendor");
  auto stage=manager->prepareMaterialTextureBytes(0,"vendor",bytes,MaterialTextureRoles::Color);const auto command=stage.command();const auto texture=stage.evaluatedTexture();ASSERT_TRUE(publishResources(*manager,stage));stage={};
  auto undo=manager->prepareMaterialResourcesChange(command,false);ASSERT_TRUE(publishResources(*manager,undo));undo={};
  auto redo=manager->prepareMaterialResourcesChange(command,true);ASSERT_TRUE(publishResources(*manager,redo));redo={};
  manager->animationSource().nodes[0].extensions["VENDOR_link"]=tinygltf::Value(tinygltf::Value::Object{{"index",tinygltf::Value(int(texture))}});
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);EXPECT_TRUE(command.present());
}

TEST_F(MaterialTextureGenerationGpu,NestedVendorWitnessAllowsTheEditedKnownSiblingToChange) {
  using namespace container::geometry::gltf;auto source=triangle("A",31);
  const auto vendor=tinygltf::Value(tinygltf::Value::Object{{"channelLabel",tinygltf::Value(std::string("Coating"))}});
  source.gltfModel.materials[0].extensions["KHR_materials_clearcoat"]=tinygltf::Value(tinygltf::Value::Object{
      {"clearcoatTexture",tinygltf::Value(tinygltf::Value::Object{{"index",tinygltf::Value(0)},
          {"extensions",tinygltf::Value(tinygltf::Value::Object{{"VENDOR_channel",vendor}})}})}});
  source.gltfModel.extensionsUsed.push_back("KHR_materials_clearcoat");source.gltfModel.extensionsUsed.push_back("VENDOR_channel");
  loadSource(std::move(source));MaterialFrames frames(*allocation);const auto bytes=producerPng(temp,"nested-vendor");
  auto resources=manager->prepareMaterialTextureBytes(0,"nested-vendor",bytes,MaterialTextureRoles::Data);const auto command=resources.command();
  GltfMaterialPatch patch;GltfMaterialTextureEdit edit;edit.slot=GltfMaterialTextureSlot::Clearcoat;edit.operation=GltfMaterialTextureOperation::Bind;
  edit.binding=GltfMaterialTextureSourcePair{int32_t(resources.sourceTexture()),int32_t(resources.evaluatedTexture())};patch.textures.push_back(edit);
  const auto original=manager->planMaterialTextureEdit(0,patch,&resources);
  auto stage=manager->prepareMaterialGeneration(original,nullptr,std::move(resources));ASSERT_TRUE(publishMaterial(*manager,frames,stage));stage={};
  auto remove=manager->prepareMaterialResourcesChange(command,false);const auto before=manager->rebaseMaterialTextureEdit(original,GltfMaterialEditSide::Before,&remove);
  auto undo=manager->prepareMaterialGeneration(before,nullptr,std::move(remove));ASSERT_TRUE(publishMaterial(*manager,frames,undo));undo={};
  EXPECT_EQ(ReadGltfMaterialTextureBinding(manager->authoredMaterial(0),GltfMaterialTextureSlot::Clearcoat).index,0);
  EXPECT_EQ(manager->authoredMaterial(0).extensions.at("KHR_materials_clearcoat").Get("clearcoatTexture").Get("extensions").Get("VENDOR_channel"),vendor);
}

TEST_F(MaterialTextureGenerationGpu,SamplerAliasUndoRemovesItsCopiedVendorMetadataWithTheOwnedRow) {
  auto source=triangle("A",31);const auto vendor=tinygltf::Value(tinygltf::Value::Object{{"image",tinygltf::Value(0)},{"label",tinygltf::Value(std::string("Original source"))}});
  source.gltfModel.textures[0].extensions["VENDOR_texture"]=vendor;source.gltfModel.extensionsUsed.push_back("VENDOR_texture");loadSource(std::move(source));
  const auto textures=manager->animationSource().textures.size(),samplers=manager->animationSource().samplers.size(),runtime=manager->materialTextureResourceCount();
  auto alias=manager->prepareMaterialTextureSource(0,0,MaterialTextureRoles::Color,container::material::NormalizeMaterialSamplerKey(9728,9728).value);
  const auto command=alias.command();ASSERT_TRUE(publishResources(*manager,alias));alias={};
  EXPECT_EQ(manager->animationSource().textures.back().extensions.at("VENDOR_texture"),vendor);
  auto undo=manager->prepareMaterialResourcesChange(command,false);ASSERT_TRUE(publishResources(*manager,undo));undo={};
  EXPECT_EQ(manager->animationSource().textures.size(),textures);EXPECT_EQ(manager->animationSource().samplers.size(),samplers);EXPECT_EQ(manager->materialTextureResourceCount(),runtime);
  EXPECT_EQ(manager->animationSource().textures[0].extensions.at("VENDOR_texture"),vendor);
  auto redo=manager->prepareMaterialResourcesChange(command,true);ASSERT_TRUE(publishResources(*manager,redo));redo={};
  EXPECT_EQ(manager->animationSource().textures.back().extensions.at("VENDOR_texture"),vendor);
}

TEST_F(MaterialTextureGenerationGpu,ResourcesCannotBePairedWithAnotherSelectedMaterialProof) {
  loadSource(triangle("A",31,false,false));MaterialFrames frames(*allocation);
  auto duplicate=manager->prepareMaterialGeneration(manager->planMaterialDuplicate(0),nullptr);ASSERT_TRUE(publishMaterial(*manager,frames,duplicate));duplicate={};
  const auto bytes=producerPng(temp,"source-guards");auto first=manager->prepareMaterialTextureBytes(0,"source-guards",bytes,MaterialTextureRoles::Color);
  const auto patch=texturePatch(first);const auto plan=manager->planMaterialTextureEdit(0,patch,&first);
  EXPECT_THROW((void)manager->planMaterialTextureEdit(1,patch,&first),std::invalid_argument);
  auto other=manager->prepareMaterialTextureBytes(1,"other-source",bytes,MaterialTextureRoles::Color);
  EXPECT_THROW((void)manager->rebaseMaterialTextureEdit(plan,container::geometry::gltf::GltfMaterialEditSide::After,&other),std::invalid_argument);
  EXPECT_THROW((void)manager->prepareMaterialGeneration(plan,nullptr,std::move(other)),std::invalid_argument);
  EXPECT_TRUE(other);EXPECT_FALSE(first.command().published());EXPECT_FALSE(other.command().published());
  EXPECT_TRUE(manager->animationSource().textures.empty());
}

TEST_F(MaterialTextureGenerationGpu,SelectedRawMutationAfterOuterCanRejectsBothResourceAndMaterialPublication) {
  loadSource(triangle("A",31,false,false));MaterialFrames frames(*allocation);const auto bytes=producerPng(temp,"last-guard");
  auto resources=manager->prepareMaterialTextureBytes(0,"last-guard",bytes,MaterialTextureRoles::Color);const auto command=resources.command();
  const auto texture=resources.evaluatedTexture();const auto plan=manager->planMaterialTextureEdit(0,texturePatch(resources),&resources);
  auto stage=manager->prepareMaterialGeneration(plan,nullptr,std::move(resources));frames.bind(stage);
  const auto revision=manager->materialResourceRevision(),material=manager->materialRevision(),epoch=manager->generationEpoch();
  const auto count=manager->materialTextureResourceCount();const auto descriptor=manager->descriptorSet(0);
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));manager->animationSource().materials[0].pbrMetallicRoughness.baseColorTexture.index=int(texture);
  size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);manager->exchangeMaterialGeneration(stage,nullptr);attempts=guard.allocations();}
  EXPECT_EQ(attempts,0u);EXPECT_TRUE(stage);EXPECT_FALSE(command.published());EXPECT_EQ(manager->materialTextureResourceCount(),count);
  EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_EQ(manager->materialRevision(),material);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->descriptorSet(0),descriptor);
  manager->animationSource().materials[0].pbrMetallicRoughness.baseColorTexture.index=-1;
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(stage,nullptr));const_cast<tinygltf::Material&>(manager->authoredMaterial(0)).pbrMetallicRoughness.roughnessFactor=.31;
  {material_publication_test::AllocationGuard guard(true);manager->exchangeMaterialGeneration(stage,nullptr);attempts=guard.allocations();}
  EXPECT_EQ(attempts,0u);EXPECT_TRUE(stage);EXPECT_FALSE(command.published());EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_EQ(manager->descriptorSet(0),descriptor);
}

TEST_F(MaterialTextureGenerationGpu,AnimationPointersAndNewUnknownExtensionsCannotRetainRemovedTails) {
  loadSource(triangle("A",31,false,false));const auto bytes=producerPng(temp,"pointer");auto stage=manager->prepareMaterialTextureBytes(0,"pointer",bytes,MaterialTextureRoles::Color);
  const auto command=stage.command();const auto texture=stage.evaluatedTexture();ASSERT_TRUE(publishResources(*manager,stage));stage={};
  auto& source=manager->animationSource();source.animations.emplace_back();source.animations.back().channels.emplace_back();
  source.animations.back().channels.back().target_extensions["KHR_animation_pointer"]=tinygltf::Value(tinygltf::Value::Object{{"pointer",tinygltf::Value("/textures/"+std::to_string(texture)+"/sampler")}});
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);source.animations.clear();
  source.nodes[0].extensions["VENDOR_new_reference"]=tinygltf::Value(tinygltf::Value::Object{{"texture",tinygltf::Value(int(texture))}});
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);source.nodes[0].extensions.clear();
  source.extras=tinygltf::Value(tinygltf::Value::Object{{"texture",tinygltf::Value(int(texture))},{"asciiLabel",tinygltf::Value(std::string("中文"))}});
  auto undo=manager->prepareMaterialResourcesChange(command,false);ASSERT_TRUE(publishResources(*manager,undo));
}

TEST_F(MaterialTextureGenerationGpu,RetainedTexturePointersCannotAnimateOwnedImageOrSamplerIndices) {
  load();const auto bytes=producerPng(temp,"dynamic-index");auto stage=manager->prepareMaterialTextureBytes(0,"dynamic-index",bytes,MaterialTextureRoles::Color);
  const auto command=stage.command();ASSERT_TRUE(publishResources(*manager,stage));stage={};
  const auto revision=manager->materialResourceRevision(),count=manager->materialTextureResourceCount();
  auto& source=manager->animationSource();source.animations.emplace_back();source.animations.back().channels.emplace_back();
  auto& channel=source.animations.back().channels.back();channel.target_path="pointer";
  for(const auto field:{"source","sampler"}) {
    channel.target_extensions["KHR_animation_pointer"]=tinygltf::Value(tinygltf::Value::Object{{"pointer",tinygltf::Value(std::string("/textures/0/")+field)}});
    EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);
    EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_EQ(manager->materialTextureResourceCount(),count);EXPECT_TRUE(command.present());
  }
  source.animations.clear();auto undo=manager->prepareMaterialResourcesChange(command,false);ASSERT_TRUE(publishResources(*manager,undo));
}

TEST_F(MaterialTextureGenerationGpu,ResourceGuardsCompareOpaqueMetadataAndImageFormatFieldsWithoutAllocating) {
  load();const auto bytes=producerPng(temp,"exact-guard");auto& source=manager->animationSource();
  source.images[0].extensions["VENDOR_image"]=tinygltf::Value(tinygltf::Value::Object{{"opaque",tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(7)})}});
  source.samplers[0].extras=tinygltf::Value(tinygltf::Value::Object{{"label",tinygltf::Value(std::string("Sampler"))}});
  source.textures[0].extensions_json_string="{ cached texture metadata }";
  const auto originalBits=source.images[0].bits;
  auto stage=manager->prepareMaterialTextureBytes(0,"exact-guard",bytes,MaterialTextureRoles::Color);
  EXPECT_FALSE(manager->canExchangeMaterialResources(stage));stage.sealLiveDependencies();
  bool can{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);can=manager->canExchangeMaterialResources(stage);attempts=guard.allocations();}
  EXPECT_TRUE(can);EXPECT_EQ(attempts,0u);
  source.images[0].bits=16;
  {material_publication_test::AllocationGuard guard(true);can=manager->canExchangeMaterialResources(stage);manager->exchangeMaterialResources(stage);attempts=guard.allocations();}
  EXPECT_FALSE(can);EXPECT_EQ(attempts,0u);EXPECT_TRUE(stage);source.images[0].bits=originalBits;
  source.images[0].as_is=!source.images[0].as_is;
  {material_publication_test::AllocationGuard guard(true);can=manager->canExchangeMaterialResources(stage);attempts=guard.allocations();}
  EXPECT_FALSE(can);EXPECT_EQ(attempts,0u);
}

TEST_F(MaterialTextureGenerationGpu,StaleRawReferencesAreRejectedWithoutPublicationAllocations) {
  loadSource(triangle("A",31,false,false));const auto bytes=producerPng(temp,"stale");auto stage=manager->prepareMaterialTextureBytes(0,"stale",bytes,MaterialTextureRoles::Color);
  const auto command=stage.command();const auto texture=stage.evaluatedTexture();ASSERT_TRUE(publishResources(*manager,stage));stage={};
  auto undo=manager->prepareMaterialResourcesChange(command,false);undo.sealLiveDependencies();const auto revision=manager->materialResourceRevision(),count=manager->materialTextureResourceCount();
  manager->animationSource().materials[0].pbrMetallicRoughness.baseColorTexture.index=int(texture);
  size_t attempts{};bool can{};
  {material_publication_test::AllocationGuard guard(true);can=manager->canExchangeMaterialResources(undo);manager->exchangeMaterialResources(undo);attempts=guard.allocations();}
  EXPECT_FALSE(can);EXPECT_EQ(attempts,0u);EXPECT_TRUE(undo);EXPECT_TRUE(command.present());EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_EQ(manager->materialTextureResourceCount(),count);
  manager->animationSource().materials[0].pbrMetallicRoughness.baseColorTexture.index=-1;undo={};
  auto fresh=manager->prepareMaterialResourcesChange(command,false);fresh.sealLiveDependencies();manager->animationSource().nodes[0].extensions["VENDOR_stale"]=tinygltf::Value(1);
  {material_publication_test::AllocationGuard guard(true);can=manager->canExchangeMaterialResources(fresh);manager->exchangeMaterialResources(fresh);attempts=guard.allocations();}
  EXPECT_FALSE(can);EXPECT_EQ(attempts,0u);EXPECT_TRUE(fresh);EXPECT_TRUE(command.present());
}

TEST_F(MaterialTextureGenerationGpu,CombinedMaterialAndResourcePublicationCoversAllTwentyThreeSlots) {
  using namespace container::geometry::gltf;loadSource(triangle("A",31,false,false));MaterialFrames frames(*allocation);const auto bytes=producerPng(temp,"all-slots");
  auto resources=manager->prepareMaterialTextureBytes(0,"all-slots",bytes,MaterialTextureRoles::ColorAndData);
  const auto color=resources.reference(true),data=resources.reference(false);const auto source=resources.sourceTexture(),evaluated=resources.evaluatedTexture();
  const auto plan=manager->planMaterialTextureEdit(0,texturePatch(resources,true),&resources);
  const auto converted=container::material::ConvertGltfMaterial(plan.evaluatedAfter,resources.conversionContext());
  const auto provenance=resources.makeProvenance(converted.value,plan.authoredAfter,plan.evaluatedAfter);
  ASSERT_EQ(GltfMaterialTextureSlotRegistry().size(),23u);
  for(const auto& slot:GltfMaterialTextureSlotRegistry()) {
    const auto index=size_t(slot.slot);EXPECT_EQ(converted.sourceTextures[index],int32_t(evaluated));
    ASSERT_TRUE(provenance[index]);EXPECT_EQ(*provenance[index],slot.srgb?color:data);
    EXPECT_EQ(converted.runtimeBindings.runtimeIndices[index],slot.srgb?color.runtimeResource():data.runtimeResource());
  }
  auto material=manager->prepareMaterialGeneration(plan,nullptr,std::move(resources));const auto epoch=manager->generationEpoch(),revision=manager->materialRevision(),resourceRevision=manager->materialResourceRevision();
  ASSERT_TRUE(publishMaterial(*manager,frames,material));material={};
  EXPECT_EQ(manager->generationEpoch(),epoch+1);EXPECT_EQ(manager->materialRevision(),revision+1);EXPECT_EQ(manager->materialResourceRevision(),resourceRevision+1);
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[0]));
  for(const auto& slot:GltfMaterialTextureSlotRegistry()) {
    EXPECT_EQ(ReadGltfMaterialTextureBinding(manager->authoredMaterial(0),slot.slot).index,int32_t(source));
    EXPECT_EQ(ReadGltfMaterialTextureBinding(manager->animationSource().materials[0],slot.slot).index,int32_t(evaluated));
  }
}

TEST_F(MaterialTextureGenerationGpu,CombinedHistoryShrinksAndGrowsResourceDomainsWithoutRereading) {
  using namespace container::geometry::gltf;loadSource(triangle("A",31,false,false));MaterialFrames frames(*allocation);const auto bytes=producerPng(temp,"combined-history");
  const auto count=manager->materialTextureResourceCount(),textures=manager->animationSource().textures.size();
  auto resources=manager->prepareMaterialTextureBytes(0,"combined-history",bytes,MaterialTextureRoles::Color);
  const auto command=resources.command();const auto reference=resources.reference(true);const auto original=manager->planMaterialTextureEdit(0,texturePatch(resources),&resources);
  auto initial=manager->prepareMaterialGeneration(original,nullptr,std::move(resources));ASSERT_TRUE(publishMaterial(*manager,frames,initial));initial={};const auto baseline=allocations();
  for(int iteration=0;iteration<3;++iteration) {
    auto remove=manager->prepareMaterialResourcesChange(command,false);const auto before=manager->rebaseMaterialTextureEdit(original,GltfMaterialEditSide::Before,&remove);
    auto undo=manager->prepareMaterialGeneration(before,nullptr,std::move(remove));ASSERT_TRUE(publishMaterial(*manager,frames,undo));undo={};
    EXPECT_EQ(manager->materialTextureResourceCount(),count);EXPECT_EQ(manager->animationSource().textures.size(),textures);
    EXPECT_EQ(manager->animationSource().materials[0].pbrMetallicRoughness.baseColorTexture.index,-1);
    auto add=manager->prepareMaterialResourcesChange(command,true);EXPECT_EQ(add.reference(true),reference);const auto after=manager->rebaseMaterialTextureEdit(original,GltfMaterialEditSide::After,&add);
    auto redo=manager->prepareMaterialGeneration(after,nullptr,std::move(add));ASSERT_TRUE(publishMaterial(*manager,frames,redo));redo={};
    EXPECT_EQ(manager->materialTextureResourceCount(),count+1);EXPECT_EQ(manager->animationSource().textures.size(),textures+1);EXPECT_EQ(allocations(),baseline);
    EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[0]));
  }
}

TEST_F(MaterialTextureGenerationGpu,BoundTailRejectsStandaloneSealAndTheSameCandidatePublishesWithItsPairedUndo) {
  using namespace container::geometry::gltf;using D=container::util::ResourceDomainKind;using K=container::util::ResourceKind;
  loadSource(triangle("A",31,false,false));MaterialFrames frames(*allocation);const auto bytes=producerPng(temp,"bound-composite");
  const auto oldCount=manager->materialTextureResourceCount();
  auto resources=manager->prepareMaterialTextureBytes(0,"bound-composite",bytes,MaterialTextureRoles::Color);
  EXPECT_FALSE(manager->canExchangeMaterialResources(resources));
  const auto command=resources.command();const auto reference=resources.reference(true);
  const auto plan=manager->planMaterialTextureEdit(0,texturePatch(resources),&resources);
  auto bind=manager->prepareMaterialGeneration(plan,nullptr,std::move(resources));ASSERT_TRUE(publishMaterial(*manager,frames,bind));bind={};
  const auto generation=manager->resourceLifecycleGeneration();const auto target=generation->resource({D::Runtime,K::RuntimeTexture,reference.runtimeResource()});
  ASSERT_TRUE(target);const auto inventory=generation->inventory();
  const auto row=std::ranges::find_if(inventory.rows(),[&](const auto& value){return value.resource==target;});
  ASSERT_NE(row,inventory.rows().end());EXPECT_GT(row->pins[size_t(container::util::ResourcePinClass::LiveDependency)],0u);
  auto remove=manager->prepareMaterialResourcesChange(command,false);EXPECT_FALSE(manager->canExchangeMaterialResources(remove));
  const auto revision=manager->materialResourceRevision(),materialRevision=manager->materialRevision(),epoch=manager->generationEpoch();
  EXPECT_THROW(remove.sealLiveDependencies(),std::invalid_argument);EXPECT_TRUE(remove);EXPECT_TRUE(command.present());
  bool can{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);can=manager->canExchangeMaterialResources(remove);manager->exchangeMaterialResources(remove);attempts=deny.allocations();}
  EXPECT_FALSE(can);EXPECT_EQ(attempts,0u);EXPECT_EQ(manager->resourceLifecycleGeneration(),generation);
  EXPECT_EQ(manager->materialResourceRevision(),revision);EXPECT_EQ(manager->materialRevision(),materialRevision);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(manager->materialTextureResourceCount(),oldCount+1);
  // Standalone preflight preserved this structural candidate for the exact
  // material-after proof; no premature seal may consume its source mutation.
  const auto before=manager->rebaseMaterialTextureEdit(plan,GltfMaterialEditSide::Before,&remove);
  auto undo=manager->prepareMaterialGeneration(before,nullptr,std::move(remove));ASSERT_TRUE(publishMaterial(*manager,frames,undo));undo={};
  EXPECT_EQ(manager->materialTextureResourceCount(),oldCount);EXPECT_FALSE(command.present());
  EXPECT_EQ(manager->animationSource().materials[0].pbrMetallicRoughness.baseColorTexture.index,-1);
  const auto retired=manager->resourceLifecycleGeneration()->inventory();
  const auto retained=std::ranges::find_if(retired.rows(),[&](const auto& value){return value.resource==target;});
  ASSERT_NE(retained,retired.rows().end());EXPECT_TRUE(retained->retired);
  EXPECT_EQ(retained->pins[size_t(container::util::ResourcePinClass::LiveDependency)],0u);
}

TEST_F(MaterialTextureGenerationGpu,ManualTexturedProducerPublishesItsOwnLogicalMaterialAndSourceDependencies) {
  using D=container::util::ResourceDomainKind;using K=container::util::ResourceKind;using P=container::util::ResourcePinClass;
  loadSource(triangle("A",31,false,false));const auto bytes=producerPng(temp,"manual-live");
  auto resources=manager->prepareMaterialTextureBytes(0,"manual-live",bytes,MaterialTextureRoles::Color);
  const auto command=resources.command();const auto reference=resources.reference(true);ASSERT_TRUE(publishResources(*manager,resources));resources={};
  const auto publishedCount=manager->materialTextureResourceCount();
  const auto generation=manager->resourceLifecycleGeneration();const auto runtimeTarget=generation->resource({D::Runtime,K::RuntimeTexture,reference.runtimeResource()});
  const auto archiveTarget=generation->resource({D::Archive,K::Texture,reference.archiveTexture()});
  const auto before=generation->inventory();const auto beforeRuntime=std::ranges::find_if(before.rows(),[&](const auto& row){return row.resource==runtimeTarget;});
  ASSERT_NE(beforeRuntime,before.rows().end());const auto beforeCount=beforeRuntime->pins[size_t(P::LiveDependency)];
  container::material::Material value;value.baseColorTextureIndex=reference.runtimeResource();SceneManager::MaterialTextureProvenance provenance;
  provenance[size_t(container::material::GltfMaterialTextureSlot::BaseColor)]=reference;
  const auto material=manager->createMaterial(value,provenance);EXPECT_TRUE(manager->materialIsActive(material));EXPECT_GT(manager->materialIncarnation(material),0u);
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(material));manager->uploadMaterialResources();
  const auto current=manager->resourceLifecycleGeneration();EXPECT_EQ(current->resource({D::Runtime,K::RuntimeTexture,reference.runtimeResource()}),runtimeTarget);
  const auto after=current->inventory();const auto runtime=std::ranges::find_if(after.rows(),[&](const auto& row){return row.resource==runtimeTarget;});
  const auto archive=std::ranges::find_if(after.rows(),[&](const auto& row){return row.resource==archiveTarget;});
  ASSERT_NE(runtime,after.rows().end());ASSERT_NE(archive,after.rows().end());
  EXPECT_GT(runtime->pins[size_t(P::LiveDependency)],beforeCount);EXPECT_GT(archive->pins[size_t(P::LiveDependency)],0u);
  EXPECT_THROW((void)manager->prepareMaterialResourcesChange(command,false),std::invalid_argument);
  EXPECT_TRUE(command.present());EXPECT_EQ(manager->materialTextureResourceCount(),publishedCount);
}

GltfLoadResult packedSpecularGlossinessTriangle(bool alsoUsedAsData=false) {
  auto source=triangle("Packed specular glossiness",83);auto& model=source.gltfModel;
  model.materials[0].pbrMetallicRoughness.baseColorTexture.index=-1;
  model.materials[0].extensions["KHR_materials_pbrSpecularGlossiness"]=tinygltf::Value(tinygltf::Value::Object{
      {"specularGlossinessTexture",tinygltf::Value(tinygltf::Value::Object{{"index",tinygltf::Value(0)}})},
      {"glossinessFactor",tinygltf::Value(.63)}});
  model.extensionsUsed.push_back("KHR_materials_pbrSpecularGlossiness");
  if(alsoUsedAsData) {
    model.materials.emplace_back();model.materials.back().name="Independent metallic roughness data use";
    model.materials.back().pbrMetallicRoughness.metallicRoughnessTexture.index=0;
  }
  return source;
}
container::material::GltfMaterialConversionContext currentTextureContext(const SceneManager& manager) {
  const auto& model=manager.animationSource();const auto& roles=manager.gltfTextureResources();
  return {model.textures.size(),model.extensionsUsed,roles.color,roles.data,manager.materialTextureResourceCount()};
}

TEST_F(MaterialTextureGenerationGpu,PackedSpecularGlossinessUsesOneColorResourceForBothRuntimeAliasesAndProvenance) {
  using namespace container::geometry::gltf;loadSource(packedSpecularGlossinessTriangle());
  constexpr auto roughness=size_t(GltfMaterialTextureSlot::Roughness),specular=size_t(GltfMaterialTextureSlot::SpecularColor);
  const auto& roles=manager->gltfTextureResources();ASSERT_EQ(roles.color.size(),1u);ASSERT_NE(roles.color[0],UINT32_MAX);
  EXPECT_EQ(roles.data[0],UINT32_MAX);EXPECT_EQ(manager->materialTextureResourceCount(),1u);
  const auto converted=container::material::ConvertGltfMaterial(manager->animationSource().materials[0],currentTextureContext(*manager));
  const auto authored=container::material::ReadGltfMaterialSourceTextures(manager->authoredMaterial(0),manager->authoredMaterialResources().textures.size());
  const auto evaluated=container::material::ReadGltfMaterialSourceTextures(manager->animationSource().materials[0],manager->animationSource().textures.size());
  EXPECT_TRUE(converted.value.specularGlossinessWorkflow);EXPECT_EQ(converted.value.roughnessTextureIndex,roles.color[0]);
  EXPECT_EQ(converted.value.specularColorTextureIndex,roles.color[0]);
  for(const auto slot:{roughness,specular}) {
    EXPECT_EQ(authored[slot],0);EXPECT_EQ(evaluated[slot],0);EXPECT_EQ(converted.sourceTextures[slot],0);
    EXPECT_EQ(converted.runtimeBindings.runtimeIndices[slot],roles.color[0]);
  }
  const auto runtime=manager->sourceMaterialRuntimeMapping()[0];ASSERT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));
  const auto baseline=allocations();
  {
    auto resources=manager->prepareMaterialTextureSource(0,0,MaterialTextureRoles::Color);
    EXPECT_EQ(resources.textures().textureCount(),1u);EXPECT_EQ(allocations(),baseline+1); // Detached metadata only, no image upload.
    EXPECT_THROW((void)resources.reference(false),std::invalid_argument);
    const auto color=resources.reference(true);const auto proof=resources.makeProvenance(converted.value,manager->authoredMaterial(0),manager->animationSource().materials[0]);
    for(const auto slot:{roughness,specular}) {
      ASSERT_TRUE(proof[slot]);EXPECT_EQ(*proof[slot],color);EXPECT_TRUE(proof[slot]->isSrgb());
      EXPECT_EQ(proof[slot]->archiveTexture(),0u);EXPECT_EQ(proof[slot]->evaluatedTexture(),std::optional<uint32_t>(0));
    }
    EXPECT_TRUE(resources.provenanceMatches(proof,converted.value));EXPECT_EQ(resources.forwardProvenance(proof,converted.value),proof);
  }
  EXPECT_EQ(allocations(),baseline);EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));
  const auto extra=manager->loadMaterialTextureSourceFromBytes("SG provenance forwarding",producerPng(temp,"sg-forwarding"),true);
  EXPECT_TRUE(extra);EXPECT_EQ(manager->materialTextureResourceCount(),2u);EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));
  EXPECT_TRUE(std::ranges::all_of(manager->authoredMaterialResources().dataResources,[](uint32_t resource){return resource==UINT32_MAX;}));
}

TEST_F(MaterialTextureGenerationGpu,PackedSpecularGlossinessColorOnlyImportAndSourceBindingReplayWithIndependentDataUse) {
  using namespace container::geometry::gltf;loadSource(packedSpecularGlossinessTriangle(true));MaterialFrames frames(*allocation);
  constexpr auto roughness=size_t(GltfMaterialTextureSlot::Roughness),specular=size_t(GltfMaterialTextureSlot::SpecularColor);
  const auto originalColor=manager->gltfTextureResources().color[0],independentData=manager->gltfTextureResources().data[0];
  ASSERT_NE(originalColor,UINT32_MAX);ASSERT_NE(independentData,UINT32_MAX);ASSERT_NE(originalColor,independentData);
  ASSERT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[0]));
  ASSERT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[1]));
  const auto originalCount=manager->materialTextureResourceCount(),originalTextures=manager->animationSource().textures.size();
  (void)producerPng(temp,"sg-color-import");
  auto imported=manager->prepareMaterialTextureFile(0,temp/"sg-color-import.png",MaterialTextureRoles::Color);
  const auto importedCommand=imported.command();const auto importedReference=imported.reference(true);
  EXPECT_EQ(imported.textures().textureCount(),originalCount+1);EXPECT_EQ(imported.conversionContext().dataResources[imported.evaluatedTexture()],UINT32_MAX);
  EXPECT_THROW((void)imported.reference(false),std::invalid_argument);
  GltfMaterialPatch importPatch;GltfMaterialTextureEdit edit;edit.slot=GltfMaterialTextureSlot::Roughness;edit.operation=GltfMaterialTextureOperation::Bind;
  edit.binding=GltfMaterialTextureSourcePair{int32_t(importedReference.archiveTexture()),int32_t(*importedReference.evaluatedTexture())};importPatch.textures.push_back(edit);
  const auto importPlan=manager->planMaterialTextureEdit(0,importPatch,&imported);
  const auto candidate=container::material::ConvertGltfMaterial(importPlan.evaluatedAfter,imported.conversionContext());
  const auto candidateProof=imported.makeProvenance(candidate.value,importPlan.authoredAfter,importPlan.evaluatedAfter);
  for(const auto slot:{roughness,specular}) {ASSERT_TRUE(candidateProof[slot]);EXPECT_EQ(*candidateProof[slot],importedReference);}
  auto initial=manager->prepareMaterialGeneration(importPlan,nullptr,std::move(imported));ASSERT_TRUE(publishMaterial(*manager,frames,initial));initial={};
  const auto retainedAllocations=allocations();ASSERT_TRUE(std::filesystem::remove(temp/"sg-color-import.png"));
  const auto check=[&](uint32_t expectedSource,uint32_t expectedEvaluated,uint32_t expectedResource,size_t expectedCount) {
    const auto context=currentTextureContext(*manager);const auto converted=container::material::ConvertGltfMaterial(manager->animationSource().materials[0],context);
    const auto a=container::material::ReadGltfMaterialSourceTextures(manager->authoredMaterial(0),manager->authoredMaterialResources().textures.size());
    const auto e=container::material::ReadGltfMaterialSourceTextures(manager->animationSource().materials[0],context.textureCount);
    for(const auto slot:{roughness,specular}) {EXPECT_EQ(a[slot],int32_t(expectedSource));EXPECT_EQ(e[slot],int32_t(expectedEvaluated));EXPECT_EQ(converted.runtimeBindings.runtimeIndices[slot],expectedResource);}
    EXPECT_EQ(converted.value.roughnessTextureIndex,expectedResource);EXPECT_EQ(converted.value.specularColorTextureIndex,expectedResource);
    EXPECT_EQ(manager->materialTextureResourceCount(),expectedCount);EXPECT_EQ(manager->gltfTextureResources().data[0],independentData);
    if(manager->gltfTextureResources().data.size()>originalTextures) EXPECT_EQ(manager->gltfTextureResources().data.back(),UINT32_MAX);
    EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[0]));
    EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(manager->sourceMaterialRuntimeMapping()[1]));
  };
  check(importedReference.archiveTexture(),*importedReference.evaluatedTexture(),importedReference.runtimeResource(),originalCount+1);
  const auto replay=[&](const SceneManager::MaterialResourceCommand& resources,const GltfMaterialAuthoringPlan& plan,bool forward) {
    auto prospective=manager->prepareMaterialResourcesChange(resources,forward);
    const auto rebased=manager->rebaseMaterialTextureEdit(plan,forward?GltfMaterialEditSide::After:GltfMaterialEditSide::Before,&prospective);
    auto material=manager->prepareMaterialGeneration(rebased,nullptr,std::move(prospective));return publishMaterial(*manager,frames,material);
  };
  ASSERT_TRUE(replay(importedCommand,importPlan,false));check(0,0,originalColor,originalCount);
  ASSERT_TRUE(replay(importedCommand,importPlan,true));check(importedReference.archiveTexture(),*importedReference.evaluatedTexture(),importedReference.runtimeResource(),originalCount+1);
  auto existing=manager->prepareMaterialTextureSource(0,0,MaterialTextureRoles::Color);const auto existingCommand=existing.command();const auto existingColor=existing.reference(true),existingData=existing.reference(false);
  EXPECT_EQ(existingColor.runtimeResource(),originalColor);EXPECT_EQ(existingData.runtimeResource(),independentData);EXPECT_NE(existingColor,existingData);
  EXPECT_EQ(existing.textures().textureCount(),originalCount+1);
  GltfMaterialPatch bindPatch;edit.slot=GltfMaterialTextureSlot::SpecularColor;edit.binding=GltfMaterialTextureSourcePair{0,0};bindPatch.textures.push_back(edit);
  const auto bindPlan=manager->planMaterialTextureEdit(0,bindPatch,&existing);
  const auto rebound=container::material::ConvertGltfMaterial(bindPlan.evaluatedAfter,existing.conversionContext());
  const auto reboundProof=existing.makeProvenance(rebound.value,bindPlan.authoredAfter,bindPlan.evaluatedAfter);
  for(const auto slot:{roughness,specular}) {ASSERT_TRUE(reboundProof[slot]);EXPECT_EQ(*reboundProof[slot],existingColor);EXPECT_NE(*reboundProof[slot],existingData);}
  auto binding=manager->prepareMaterialGeneration(bindPlan,nullptr,std::move(existing));ASSERT_TRUE(publishMaterial(*manager,frames,binding));binding={};check(0,0,originalColor,originalCount+1);
  for(int cycle=0;cycle<2;++cycle) {
    ASSERT_TRUE(replay(existingCommand,bindPlan,false));check(importedReference.archiveTexture(),*importedReference.evaluatedTexture(),importedReference.runtimeResource(),originalCount+1);
    ASSERT_TRUE(replay(existingCommand,bindPlan,true));check(0,0,originalColor,originalCount+1);EXPECT_EQ(allocations(),retainedAllocations);
  }
  ASSERT_TRUE(replay(existingCommand,bindPlan,false));ASSERT_TRUE(replay(importedCommand,importPlan,false));check(0,0,originalColor,originalCount);
  ASSERT_TRUE(replay(importedCommand,importPlan,true));ASSERT_TRUE(replay(existingCommand,bindPlan,true));check(0,0,originalColor,originalCount+1);
  EXPECT_EQ(allocations(),retainedAllocations);
}

TEST_F(MaterialTextureGenerationGpu,SpecularGlossinessWithoutPackedTexturePreservesLegacyRoughnessDataProvenance) {
  using namespace container::geometry::gltf;auto source=packedSpecularGlossinessTriangle();
  source.gltfModel.materials[0].extensions["KHR_materials_pbrSpecularGlossiness"]=tinygltf::Value(tinygltf::Value::Object{});
  tinygltf::Parameter fallback;fallback.json_double_value={{"index",0.},{"channel",2.}};
  source.gltfModel.materials[0].values["roughnessTexture"]=fallback;
  // The project writer accepts a packed export model. Keep the real legacy
  // serialization path while packing this fixture's PNG after its geometry.
  const auto png=producerPng(temp,"sg-legacy-packed",83);auto& model=source.gltfModel;auto& bytes=model.buffers[0].data;
  while(bytes.size()%4) bytes.push_back(0);
  tinygltf::BufferView imageView;imageView.buffer=0;imageView.byteOffset=bytes.size();imageView.byteLength=png.size();
  bytes.insert(bytes.end(),reinterpret_cast<const unsigned char*>(png.data()),reinterpret_cast<const unsigned char*>(png.data())+png.size());
  auto& image=model.images[0];image.bufferView=int(model.bufferViews.size());image.uri.clear();image.mimeType="image/png";image.image.clear();
  model.bufferViews.push_back(std::move(imageView));
  loadSource(std::move(source),true);MaterialFrames frames(*allocation);
  constexpr auto roughness=size_t(GltfMaterialTextureSlot::Roughness),specular=size_t(GltfMaterialTextureSlot::SpecularColor);
  ASSERT_TRUE(manager->authoredMaterial(0).values.contains("roughnessTexture"));
  const auto& roles=manager->gltfTextureResources();ASSERT_EQ(roles.color[0],UINT32_MAX);ASSERT_NE(roles.data[0],UINT32_MAX);
  const auto data=roles.data[0],runtime=manager->sourceMaterialRuntimeMapping()[0];
  const auto converted=container::material::ConvertGltfMaterial(manager->animationSource().materials[0],currentTextureContext(*manager));
  const auto ids=container::material::ReadGltfMaterialSourceTextures(manager->authoredMaterial(0),manager->authoredMaterialResources().textures.size());
  EXPECT_TRUE(converted.value.specularGlossinessWorkflow);EXPECT_EQ(converted.value.roughnessTextureIndex,data);
  EXPECT_EQ(converted.value.specularColorTextureIndex,UINT32_MAX);EXPECT_EQ(ids[roughness],0);EXPECT_EQ(ids[specular],-1);
  EXPECT_EQ(converted.sourceTextures[roughness],0);EXPECT_EQ(converted.sourceTextures[specular],-1);
  EXPECT_EQ(converted.value.roughnessTextureTransform.channel,2u);ASSERT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));
  const auto baseline=allocations();
  {
    auto resources=manager->prepareMaterialTextureSource(0,0,MaterialTextureRoles::Data);
    EXPECT_EQ(resources.textures().textureCount(),1u);EXPECT_EQ(allocations(),baseline+1);
    EXPECT_THROW((void)resources.reference(true),std::invalid_argument);
    const auto proof=resources.makeProvenance(converted.value,manager->authoredMaterial(0),manager->animationSource().materials[0]);
    ASSERT_TRUE(proof[roughness]);EXPECT_EQ(*proof[roughness],resources.reference(false));EXPECT_FALSE(proof[roughness]->isSrgb());EXPECT_FALSE(proof[specular]);
    EXPECT_EQ(resources.forwardProvenance(proof,converted.value),proof);
    bool valid{};size_t attempts{};
    {material_publication_test::AllocationGuard guard(true);valid=resources.provenanceMatches(proof,converted.value) && manager->hasRuntimeMaterialProvenance(runtime);attempts=guard.allocations();}
    EXPECT_TRUE(valid);EXPECT_EQ(attempts,0u);
  }
  EXPECT_EQ(allocations(),baseline);
  GltfMaterialPatch factor;factor.values={{GltfMaterialProperty::Glossiness,.29}};
  auto stage=manager->prepareMaterialGeneration(manager->planMaterialEdit(0,factor),nullptr);ASSERT_TRUE(publishMaterial(*manager,frames,stage));stage={};
  EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));EXPECT_EQ(manager->materialTextureResourceCount(),1u);
  const auto extra=manager->loadMaterialTextureSourceFromBytes("Legacy roughness forwarding",producerPng(temp,"sg-legacy-forwarding"),true);
  EXPECT_TRUE(extra);EXPECT_TRUE(manager->hasRuntimeMaterialProvenance(runtime));EXPECT_EQ(manager->gltfTextureResources().data[0],data);
}

} // namespace


TEST_F(MaterialTextureGenerationGpu, SceneSaveRepacksRuntimeScalarsWithoutChangingSharedData) {
  load();
  const auto png = producerPng(temp, "scalar-input", 63);
  const auto data = manager->loadMaterialTextureSourceFromBytes("scalar-input", png, false);
  ASSERT_TRUE(data);
  container::material::Material material;
  material.occlusionTextureIndex = data.runtimeResource();
  using Material = container::material::Material;
  struct Slot {
    const char* extension;
    const char* field;
    uint32_t Material::* texture;
    container::material::TextureTransform Material::* transform;
    size_t targetChannel;
  };
  constexpr std::array slots{
      Slot{"KHR_materials_specular", "specularTexture", &Material::specularTextureIndex, &Material::specularTextureTransform, 3},
      Slot{"KHR_materials_transmission", "transmissionTexture", &Material::transmissionTextureIndex, &Material::transmissionTextureTransform, 0},
      Slot{"KHR_materials_clearcoat", "clearcoatTexture", &Material::clearcoatTextureIndex, &Material::clearcoatTextureTransform, 0},
      Slot{"KHR_materials_clearcoat", "clearcoatRoughnessTexture", &Material::clearcoatRoughnessTextureIndex, &Material::clearcoatRoughnessTextureTransform, 1},
      Slot{"KHR_materials_volume", "thicknessTexture", &Material::thicknessTextureIndex, &Material::thicknessTextureTransform, 1},
      Slot{"KHR_materials_sheen", "sheenRoughnessTexture", &Material::sheenRoughnessTextureIndex, &Material::sheenRoughnessTextureTransform, 3},
      Slot{"KHR_materials_iridescence", "iridescenceTexture", &Material::iridescenceTextureIndex, &Material::iridescenceTextureTransform, 0},
      Slot{"KHR_materials_iridescence", "iridescenceThicknessTexture", &Material::iridescenceThicknessTextureIndex, &Material::iridescenceThicknessTextureTransform, 1},
      Slot{"KHR_materials_diffuse_transmission", "diffuseTransmissionTexture", &Material::diffuseTransmissionTextureIndex, &Material::diffuseTransmissionTextureTransform, 3},
  };
  for (const auto& slot : slots) {
    material.*slot.texture = data.runtimeResource();
    (material.*slot.transform).channel = 2;
  }
  material.transmissionTextureTransform.offset = {.25f, .5f};
  material.transmissionFactor = .75f;
  material.doubleSided = true;
  const auto runtime = manager->createMaterial(material);
  const auto node = primitiveNode(sourceNode(0));
  ASSERT_NE(node, SceneGraph::kInvalidNode);
  graph.setMaterialIndex(node, runtime);
  const auto original = manager->animationSource();
  const auto* fixtureDirectory = std::getenv("CONTAINER_SCALAR_EXPORT_FIXTURE_DIR");
  const auto portable = fixtureDirectory && *fixtureDirectory
      ? std::filesystem::path(fixtureDirectory) : temp / "portable";
  std::filesystem::create_directories(portable);
  for (const auto* suffix : {"gltf", "glb"}) {
    SCOPED_TRACE(suffix);
    const auto path = portable / (std::string("transmission.") + suffix);
    ASSERT_NO_THROW(manager->saveScene(graph, path));
    tinygltf::Model reloaded;
    tinygltf::TinyGLTF reader;
    std::string error, warning;
    const bool loaded = std::string_view(suffix) == "glb"
        ? reader.LoadBinaryFromFile(&reloaded, &error, &warning, path.string())
        : reader.LoadASCIIFromFile(&reloaded, &error, &warning, path.string());
    ASSERT_TRUE(loaded) << error;
    const auto it = std::ranges::find_if(reloaded.materials, [](const auto& value) {
      return value.extensions.contains("KHR_materials_transmission");
    });
    ASSERT_NE(it, reloaded.materials.end());
    const auto& extension = it->extensions.at("KHR_materials_transmission");
    EXPECT_DOUBLE_EQ(extension.Get("transmissionFactor").GetNumberAsDouble(), .75);
    const auto& info = extension.Get("transmissionTexture");
    const auto originalIndex = it->occlusionTexture.index;
    const auto& originalTexture = reloaded.textures.at(originalIndex);
    const std::vector<unsigned char> originalPixels{63, 47, 109, 255};
    EXPECT_EQ(reloaded.images.at(originalTexture.source).image, originalPixels);
    std::array<int, 4> packedByChannel{-1, -1, -1, -1};
    for (const auto& slot : slots) {
      SCOPED_TRACE(slot.field);
      const auto repackedIndex = it->extensions.at(slot.extension).Get(slot.field).Get("index").GetNumberAsInt();
      ASSERT_NE(originalIndex, repackedIndex);
      const auto& repackedTexture = reloaded.textures.at(repackedIndex);
      EXPECT_EQ(originalTexture.sampler, repackedTexture.sampler);
      auto expected = originalPixels;
      expected[slot.targetChannel] = 109;
      EXPECT_EQ(reloaded.images.at(repackedTexture.source).image, expected);
      auto& first = packedByChannel[slot.targetChannel];
      if (first < 0) first = repackedIndex;
      EXPECT_EQ(first, repackedIndex);
    }
    const auto& offset = info.Get("extensions").Get("KHR_texture_transform").Get("offset");
    EXPECT_DOUBLE_EQ(offset.Get(0).GetNumberAsDouble(), .25);
    EXPECT_DOUBLE_EQ(offset.Get(1).GetNumberAsDouble(), .5);
    EXPECT_EQ(manager->animationSource(), original);
  }
}
