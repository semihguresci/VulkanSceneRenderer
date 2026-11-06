#include <Container/common/CommonGLFW.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/MaterialBindingAuthoring.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/ecs/World.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/renderer/scene/ScenePrimitives.h>
#include <glm/gtc/matrix_transform.hpp>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "material_publication_allocations.h"

namespace {
using namespace container::scene;
using namespace container::geometry;
using namespace container::gpu;
using namespace container::renderer;

static_assert(!std::is_copy_constructible_v<SceneManager::PreparedMergeGeneration>);
static_assert(std::is_nothrow_move_constructible_v<SceneManager::PreparedMergeGeneration>);
static_assert(std::is_nothrow_move_assignable_v<SceneManager::PreparedMergeGeneration>);
static_assert(noexcept(std::declval<SceneManager&>().exchangeMergeGeneration(
    std::declval<SceneManager::PreparedMergeGeneration&>())));

GltfLoadResult triangle(const std::string& name,unsigned char red,bool implicit=false,bool textured=true) {
  GltfLoadResult result;
  auto& source=result.gltfModel;source.asset.version="2.0";
  source.buffers.resize(1);
  const std::array<float,9> positions{0,0,0,1,0,0,0,1,0};
  const std::array<float,6> uv{};
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
    vertices[i].normal={0,0,1};
  }
  result.model=Model::FromMeshes({Mesh(std::move(vertices),{0,1,2},implicit?-1:0)});
  return result;
}

void makeDeformingCameraDescendants(tinygltf::Model& source) {
  const auto attribute=[&](const void* data,size_t size,int component,int type) {
    auto& bytes=source.buffers[0].data;while(bytes.size()%4) bytes.push_back(0);const auto offset=bytes.size();
    bytes.resize(offset+size);std::memcpy(bytes.data()+offset,data,size);
    tinygltf::BufferView view;view.buffer=0;view.byteOffset=offset;view.byteLength=size;view.target=TINYGLTF_TARGET_ARRAY_BUFFER;
    const auto id=int(source.accessors.size());tinygltf::Accessor accessor;accessor.bufferView=int(source.bufferViews.size());
    accessor.componentType=component;accessor.type=type;accessor.count=3;source.bufferViews.push_back(view);source.accessors.push_back(accessor);return id;
  };
  const std::array<uint8_t,12> joints{};const std::array<float,12> weights{1,0,0,0,1,0,0,0,1,0,0,0};
  const std::array<float,9> delta{2,0,0,2,0,0,2,0,0};
  const auto joint=attribute(joints.data(),sizeof(joints),TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,TINYGLTF_TYPE_VEC4);
  const auto weight=attribute(weights.data(),sizeof(weights),TINYGLTF_COMPONENT_TYPE_FLOAT,TINYGLTF_TYPE_VEC4);
  const auto target=attribute(delta.data(),sizeof(delta),TINYGLTF_COMPONENT_TYPE_FLOAT,TINYGLTF_TYPE_VEC3);
  auto staticMesh=source.meshes[0];source.meshes.push_back(staticMesh);
  auto& primitive=source.meshes[0].primitives[0];primitive.attributes["JOINTS_0"]=joint;primitive.attributes["WEIGHTS_0"]=weight;
  primitive.targets.push_back({{"POSITION",target}});source.meshes[0].weights={.5};
  source.nodes.resize(4);source.nodes[0].mesh=-1;source.nodes[0].camera=0;source.nodes[0].children={1,3};source.nodes[0].name="Camera parent";
  source.nodes[1].mesh=0;source.nodes[1].skin=0;source.nodes[1].weights={.25};source.nodes[1].name="Skinned morph descendant";
  source.nodes[2].translation={1,0,0};source.nodes[2].name="Independent joint";
  source.nodes[3].mesh=1;source.nodes[3].translation={0,2,0};source.nodes[3].name="Static descendant";
  source.cameras.resize(1);source.cameras[0].type="perspective";source.cameras[0].perspective.yfov=.8;
  source.cameras[0].perspective.znear=.1;source.cameras[0].perspective.zfar=40;
  source.skins.resize(1);source.skins[0].joints={2};source.skins[0].skeleton=2;
  source.scenes[0].nodes={0,2};
}

class CameraPoseSceneGenerationGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<CameraPoseSceneGenerationGpu*>(user)->errors;
      std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_CAMERA_POSES");
    if(!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_CAMERA_POSES=1";
    ASSERT_EQ(glfwInit(),GLFW_TRUE);glfwStarted=true;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    window=glfwCreateWindow(32,32,"Detached controller generation",nullptr,nullptr);ASSERT_NE(window,nullptr);
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
    temp=std::filesystem::temp_directory_path()/("container-camera-pose-generation-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();
    controller.reset();manager.reset();pipelines.reset();allocation.reset();pool.clear();device.reset();
    surface.clear();messenger.clear();instance.reset();
    if(window) glfwDestroyWindow(window);if(glfwStarted) glfwTerminate();
    if(!temp.empty()) { std::error_code ignored;std::filesystem::remove_all(temp,ignored); }
    EXPECT_EQ(errors.load(),0u);
  }
  void load(bool implicit=false,bool variants=false,bool deformation=false) {
    auto initial=triangle("A",31,implicit,false);
    if(deformation) makeDeformingCameraDescendants(initial.gltfModel);
    if(variants) {
      using V=tinygltf::Value;
      initial.gltfModel.extensionsUsed.push_back("KHR_materials_variants");
      initial.gltfModel.extensionsRequired.push_back("KHR_materials_variants");
      initial.gltfModel.extensions["KHR_materials_variants"]=V(V::Object{
          {"variants",V(V::Array{V(V::Object{{"name",V("Original")},{"extras",V(16'777'217)}})})}});
      initial.gltfModel.meshes[0].primitives[0].extensions["KHR_materials_variants"]=V(V::Object{
          {"mappings",V(V::Array{V(V::Object{{"material",V(0)},{"variants",V(V::Array{V(0)})}})})}});
    }
    const auto path=temp/"a.glb";
    tinygltf::TinyGLTF writer;
    ASSERT_TRUE(writer.WriteGltfSceneToFile(&initial.gltfModel,path.string(),true,true,true,true));
    manager=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});
    manager->initialize(path.string(),1.0f,2);
    manager->populateSceneGraph(graph);
    const auto oldRoots=graph.rootNodes();
    editor=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Scene Root");
    graph.getNode(editor)->editorSceneRoot=true;
    for(const auto root:oldRoots) ASSERT_TRUE(graph.setParent(root,editor));
    graph.updateWorldTransforms();
    controller=std::make_unique<SceneController>(device,*allocation,*pipelines,graph,*manager,nullptr,config);
    controller->createGeometryBuffers();controller->syncObjectDataFromSceneGraph(false);
    CameraData camera;camera.cameraWorldPosition={3,4,5,1};
    (void)controller->world().setActiveCamera(camera,.2f,90);
    PointLightData light;light.colorIntensity={.2f,.3f,.4f,11};
    (void)controller->world().createPointLight(light);
    controller->world().registry().ctx().emplace<std::string>("retained context");
  }
  uint32_t allocations() const {
    VmaTotalStatistics stats{};vmaCalculateStatistics(allocation->memoryManager()->allocator(),&stats);
    return stats.total.statistics.allocationCount;
  }
  std::vector<std::byte> readBytes(BufferSlice source,size_t bytes) {
    if(!bytes) return {};
    if(!source.buffer || bytes>source.size) throw std::invalid_argument("Invalid controller readback range");
    auto& memory=*allocation->memoryManager();
    struct Readback {
      VulkanMemoryManager& memory;AllocatedBuffer buffer;
      ~Readback(){memory.destroyBuffer(buffer);}
    } output{memory,memory.createBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|
        VMA_ALLOCATION_CREATE_MAPPED_BIT)};
    allocation->executeStagedCommands([&](VkCommandBuffer command) {
      VkBufferMemoryBarrier sourceBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      sourceBarrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
      sourceBarrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      sourceBarrier.srcQueueFamilyIndex=sourceBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      sourceBarrier.buffer=source.buffer;sourceBarrier.offset=source.offset;sourceBarrier.size=bytes;
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&sourceBarrier,0,nullptr);
      const VkBufferCopy copy{source.offset,0,bytes};
      vkCmdCopyBuffer(command,source.buffer,output.buffer.buffer,1,&copy);
      VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      host.buffer=output.buffer.buffer;host.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
          0,0,nullptr,1,&host,0,nullptr);
    });
    if(vmaInvalidateAllocation(memory.allocator(),output.buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
      throw std::runtime_error("Controller readback invalidate failed");
    std::vector<std::byte> result(bytes);
    std::memcpy(result.data(),output.buffer.allocation_info.pMappedData,bytes);return result;
  }
  template<class T> void checkBytes(BufferSlice source,std::span<const T> expected) {
    const auto bytes=std::as_bytes(expected);
    EXPECT_EQ(readBytes(source,bytes.size()),std::vector<std::byte>(bytes.begin(),bytes.end()));
  }
  void checkPrepared(const SceneController::PreparedGeneration& prepared) {
    checkBytes(prepared.vertexSlice(),prepared.gpuBindVertices());
    std::vector<uint32_t> expected(prepared.geometryIndices().begin(),prepared.geometryIndices().end());
    const auto cube=makeScenePrimitiveCube();
    const auto base=uint32_t(prepared.gpuBindVertices().size()-cube.vertices().size());
    for(const auto index:cube.indices()) expected.push_back(base+index);
    checkBytes(prepared.indexSlice(),std::span<const uint32_t>(expected));
    EXPECT_EQ(prepared.diagCubeIndexSlice().offset,prepared.geometryIndices().size_bytes());
    EXPECT_EQ(prepared.diagCubeIndexCount(),cube.indices().size());
    for(const auto& object:prepared.objectBuffers())
      checkBytes(BufferSlice{object.buffer,0,prepared.objectData().size_bytes()},prepared.objectData());
  }
  void checkLive() {
    checkBytes(controller->vertexSlice(),controller->gpuBindVertices());
    checkLiveObjects();
  }
  // Object-only publication deliberately retains the legacy vertex arena,
  // whose usage does not include TRANSFER_SRC. Read back its new owned object
  // buffers; geometry identity/pointers are checked separately by these cases.
  void checkLiveObjects() {
    const auto data=std::span<const ObjectData>(controller->objectData());
    for(const auto& object:controller->objectBuffers())
      checkBytes(BufferSlice{object.buffer,0,data.size_bytes()},data);
  }
  std::vector<MaterialRenderProperties> materialFacts() const {
    std::vector<MaterialRenderProperties> result;
    for(uint32_t i=0;i<manager->materialCount();++i) result.push_back(manager->materialRenderProperties(i));
    return result;
  }
  MaterialBindingContext bindingContext() const {
    return {manager->materialAssetIdentity(),manager->sourceMaterialRuntimeMapping(),uint32_t(manager->materialCount()),
        uint32_t(manager->primitiveRanges().size())};
  }
  entt::entity entityForNode(uint32_t node) const {
    const auto& registry=std::as_const(controller->world()).registry();
    const auto view=registry.view<const container::ecs::SceneNodeRef,const container::ecs::MaterialComponent,
        const container::ecs::RenderableTag>();
    for(const auto entity:view) if(view.get<const container::ecs::SceneNodeRef>(entity).nodeIndex==node) return entity;
    throw std::runtime_error("Missing live ECS source-node binding");
  }
  void checkRouting(const SceneController::PreparedMaterialRouting& prepared) {
    for(const auto& object:prepared.objectBuffers())
      checkBytes(BufferSlice{object.buffer,0,prepared.objectData().size_bytes()},prepared.objectData());
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
  container::app::AppConfig config;std::unique_ptr<SceneController> controller;
};

SceneGraph::PreparedPlacement moved(SceneGraph& graph,uint32_t node,float x) {
  const auto* row=graph.getNode(node);if(!row) throw std::invalid_argument("Missing pose fixture node");
  const auto parent=row->parent==SceneGraph::kInvalidNode?std::optional<uint32_t>{}:std::optional<uint32_t>{row->parent};
  return graph.preparePlacement(node,parent,glm::translate(glm::mat4(1),glm::vec3(x,0,0)),*graph.siblingIndex(node),graph.activeRoots());
}

TEST_F(CameraPoseSceneGenerationGpu, StaticObjectsStageAndPublishActualGpuRecordsWithoutCopyingTheLiveManager) {
  load();const auto epoch=manager->generationEpoch(),geometry=manager->geometryRevision(),material=manager->materialRevision();
  const auto* sourceNodes=manager->animationSource().nodes.data();const auto* sourceBytes=manager->animationSource().buffers[0].data.data();
  const auto* ctx=controller->world().registry().ctx().find<std::string>();const auto oldObjects=controller->objectData();
  const auto oldBuffer=controller->vertexSlice().buffer;auto* world=&controller->world();auto placement=moved(graph,editor,3);
  ASSERT_TRUE(placement);auto prepared=controller->prepareGraphPoseGeneration(*placement.candidateGraph(),2,false,graph.revision(),epoch);
  ASSERT_TRUE(prepared.ready());EXPECT_GT(prepared.ownedBytes(),0u);checkPrepared(prepared);
  ASSERT_EQ(prepared.objectData().size(),oldObjects.size());
  for(size_t i=0;i<oldObjects.size();++i) {
    EXPECT_EQ(prepared.objectData()[i].objectInfo.x,oldObjects[i].objectInfo.x);
    EXPECT_EQ(prepared.objectData()[i].temporalInfo.w,oldObjects[i].temporalInfo.w);
    EXPECT_NEAR(prepared.objectData()[i].model[3].x,oldObjects[i].model[3].x+3,1e-6f);
  }
  EXPECT_EQ(controller->vertexSlice().buffer,oldBuffer);ASSERT_EQ(controller->objectData().size(),oldObjects.size());
  EXPECT_EQ(std::memcmp(controller->objectData().data(),oldObjects.data(),oldObjects.size()*sizeof(ObjectData)),0);
  EXPECT_EQ(manager->animationSource().nodes.data(),sourceNodes);EXPECT_EQ(manager->animationSource().buffers[0].data.data(),sourceBytes);
  bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);ready=controller->canExchangeGeneration(prepared,epoch);
    if(ready) {published=graph.publishPlacement(placement);if(published) controller->exchangeGeneration(prepared,graph.revision(),geometry,epoch+1);}
    attempts=guard.allocations();}
  ASSERT_TRUE(ready);ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_FALSE(prepared.ready());
  EXPECT_EQ(&controller->world(),world);EXPECT_EQ(controller->world().registry().ctx().find<std::string>(),ctx);
  ASSERT_NE(ctx,nullptr);EXPECT_EQ(*ctx,"retained context");EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(manager->materialRevision(),material);
  checkLive();EXPECT_NE(controller->vertexSlice().buffer,oldBuffer);
}

TEST_F(CameraPoseSceneGenerationGpu, SkinnedMorphedAndStaticCameraDescendantsUseCandidateWorldsAndActualGpuObjectRecords) {
  load(false,false,true);ASSERT_NE(manager->deformationData(),nullptr);
  uint32_t camera=UINT32_MAX,joint=UINT32_MAX,deformed=UINT32_MAX,staticNode=UINT32_MAX;
  for(uint32_t i=0;i<graph.nodeCount();++i) {
    const auto& node=*graph.getNode(i);
    if(node.sourceNodeIndex==0) camera=i;if(node.sourceNodeIndex==2) joint=i;
    if(node.renderable && node.sourceMeshIndex==0) deformed=i;if(node.renderable && node.sourceMeshIndex==1) staticNode=i;
  }
  ASSERT_NE(camera,UINT32_MAX);ASSERT_NE(joint,UINT32_MAX);ASSERT_NE(deformed,UINT32_MAX);ASSERT_NE(staticNode,UINT32_MAX);
  const auto beforePosition=controller->deformationScene()->currentVertices()[controller->deformationScene()->drawRange(deformed)->vertexOffset].position;
  const auto oldGeometry=manager->geometryRevision(),oldMaterial=manager->materialRevision(),epoch=manager->generationEpoch();
  auto placement=moved(graph,camera,3);ASSERT_TRUE(placement);const auto& candidate=*placement.candidateGraph();
  auto prepared=controller->prepareGraphPoseGeneration(candidate,2,false,graph.revision(),epoch);
  const auto* deformation=prepared.deformationScene();ASSERT_NE(deformation,nullptr);const auto* range=deformation->drawRange(deformed);ASSERT_NE(range,nullptr);
  ASSERT_TRUE(range->deformed());const auto& owner=*candidate.getNode(graph.getNode(deformed)->meshOwnerGraphNode);
  const auto expected=glm::vec3(glm::inverse(owner.worldTransform)*candidate.getNode(joint)->worldTransform*glm::vec4(.5f,0,0,1));
  const auto actual=deformation->currentVertices()[range->vertexOffset].position;
  EXPECT_NEAR(actual.x,expected.x,1e-5f);EXPECT_NEAR(actual.y,expected.y,1e-5f);EXPECT_NEAR(actual.z,expected.z,1e-5f);
  EXPECT_GT(glm::length(actual-beforePosition),2.9f);EXPECT_EQ(manager->animationSource().nodes[1].weights,std::vector<double>{.25});
  for(size_t i=0;i<prepared.objectNodeIndices().size();++i) {
    const auto node=prepared.objectNodeIndices()[i];EXPECT_EQ(prepared.objectData()[i].model,candidate.getNode(node)->worldTransform);
    EXPECT_EQ(prepared.objectData()[i].temporalInfo.w,graph.getNode(node)->primitiveIndex);
  }
  checkPrepared(prepared);bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);ready=controller->canExchangeGeneration(prepared,epoch);
    if(ready) {published=graph.publishPlacement(placement);if(published) controller->exchangeGeneration(prepared,graph.revision(),oldGeometry,epoch+1);}
    attempts=guard.allocations();}
  ASSERT_TRUE(ready);ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);checkLive();
  EXPECT_EQ(manager->geometryRevision(),oldGeometry);EXPECT_EQ(manager->materialRevision(),oldMaterial);
  EXPECT_EQ(controller->deformationScene()->currentVertices()[range->vertexOffset].position,actual);
}

TEST_F(CameraPoseSceneGenerationGpu, NonPoseBindingsFlagsTopologyAndForeignNodeLifetimesRejectBeforeAnyUpload) {
  load();const auto baseline=allocations();const auto graphRevision=graph.revision();
  const std::array<std::function<void(SceneGraph&)>,6> mutate{
      [&](auto& g) {g.getNode(editor)->name="replaced";},[&](auto& g) {g.getNode(editor)->sourceNodeIndex=0;},
      [&](auto& g) {g.getNode(editor)->visible=false;},[&](auto& g) {g.getNode(editor)->children.clear();},
      [&](auto& g) {g.getNode(editor)->materialIndex=99;},[&](auto& g) {g.setActiveRoots(std::array{editor});}};
  for(const auto& change:mutate) {SceneGraph candidate(graph);change(candidate);
    EXPECT_THROW((void)controller->prepareGraphPoseGeneration(candidate,2,false,graphRevision,manager->generationEpoch()),std::invalid_argument);
    EXPECT_EQ(allocations(),baseline);EXPECT_EQ(graph.revision(),graphRevision);}
  SceneGraph fresh;for(uint32_t i=0;i<graph.nodeCount();++i) (void)fresh.createNode(glm::mat4(1),0);
  EXPECT_THROW((void)controller->prepareGraphPoseGeneration(fresh,2,false,graphRevision,manager->generationEpoch()),std::invalid_argument);
  SceneGraph candidate(graph);
  EXPECT_THROW((void)controller->prepareGeneration(candidate,*manager,2,false,graphRevision,manager->generationEpoch()),std::invalid_argument);
}

TEST_F(CameraPoseSceneGenerationGpu, ExactRawGraphWeightsAndWorldWitnessesRejectStalenessWithoutTouchingLiveBuffers) {
  load();const auto epoch=manager->generationEpoch();auto placement=moved(graph,editor,2);ASSERT_TRUE(placement);
  auto prepared=controller->prepareGraphPoseGeneration(*placement.candidateGraph(),2,false,graph.revision(),epoch);
  const auto buffer=controller->vertexSlice().buffer;const auto original=graph.getNode(editor)->localTransform;
  graph.getNode(editor)->localTransform[3].x=.5f;bool staleReady{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);staleReady=controller->canExchangeGeneration(prepared,epoch);
    controller->exchangeGeneration(prepared,graph.revision(),manager->geometryRevision(),epoch+1);attempts=guard.allocations();}
  EXPECT_FALSE(staleReady);EXPECT_EQ(attempts,0u);EXPECT_EQ(controller->vertexSlice().buffer,buffer);
  graph.getNode(editor)->localTransform=original;ASSERT_TRUE(controller->canExchangeGeneration(prepared,epoch));
  manager->animationSource().nodes[0].weights={.5};EXPECT_FALSE(controller->canExchangeGeneration(prepared,epoch));
  manager->animationSource().nodes[0].weights.clear();ASSERT_TRUE(controller->canExchangeGeneration(prepared,epoch));
  const auto entity=entityForNode(prepared.objectNodeIndices().front());auto& material=controller->world().registry().get<container::ecs::MaterialComponent>(entity);
  const auto id=material.materialIndex;++material.materialIndex;EXPECT_FALSE(controller->canExchangeGeneration(prepared,epoch));material.materialIndex=id;
  ASSERT_TRUE(controller->canExchangeGeneration(prepared,epoch));controller->world().registry().ctx().get<std::string>()="Uncopied context change";
  EXPECT_TRUE(controller->canExchangeGeneration(prepared,epoch));EXPECT_FALSE(controller->canExchangeGeneration(prepared,epoch+1));
}

TEST_F(CameraPoseSceneGenerationGpu, BudgetsAllocationDenialUploadFailureAndAbandonmentPreserveLiveState) {
  load();const auto epoch=manager->generationEpoch(),revision=controller->geometryRevision();const auto baseline=allocations();
  const auto buffer=controller->vertexSlice().buffer;const auto* context=controller->world().registry().ctx().find<std::string>();
  auto placement=moved(graph,editor,2);ASSERT_TRUE(placement);const auto prepare=[&](const SceneController::GraphPoseGenerationLimits& limits) {
    return controller->prepareGraphPoseGeneration(*placement.candidateGraph(),2,false,graph.revision(),epoch,limits);
  };
  SceneController::GraphPoseGenerationLimits limits;limits.maxCpuBytes=1;EXPECT_THROW((void)prepare(limits),std::length_error);
  limits={};limits.maxGpuBytes=1;EXPECT_THROW((void)prepare(limits),std::length_error);
  limits={};limits.maxVertices=1;EXPECT_THROW((void)prepare(limits),std::length_error);
  bool denied{};{material_publication_test::AllocationGuard guard(true);try {(void)prepare({});}catch(const std::bad_alloc&) {denied=true;}}
  EXPECT_TRUE(denied);EXPECT_EQ(allocations(),baseline);
  allocation->setLoadProgressCallback([](bool done) {if(done) throw std::runtime_error("graph-pose-upload-failure");});
  EXPECT_THROW((void)prepare({}),std::runtime_error);allocation->setLoadProgressCallback({});EXPECT_EQ(allocations(),baseline);
  {auto prepared=prepare({});EXPECT_TRUE(controller->canExchangeGeneration(prepared,epoch));EXPECT_GT(allocations(),baseline);}
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(controller->vertexSlice().buffer,buffer);EXPECT_EQ(controller->geometryRevision(),revision);
  EXPECT_EQ(controller->world().registry().ctx().find<std::string>(),context);ASSERT_NE(context,nullptr);EXPECT_EQ(*context,"retained context");
}
} // namespace
