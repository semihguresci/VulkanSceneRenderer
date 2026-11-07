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

class SceneControllerGenerationGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<SceneControllerGenerationGpu*>(user)->errors;
      std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled=std::getenv("CONTAINER_RUN_GPU_SCENE_CONTROLLER");
    if(!enabled || std::strcmp(enabled,"1")!=0) GTEST_SKIP()<<"Set CONTAINER_RUN_GPU_SCENE_CONTROLLER=1";
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
    v12.shaderStorageBufferArrayNonUniformIndexing=VK_TRUE;
    info.next=&v12;
    device=std::make_shared<VulkanDevice>(*instance,surfaceHandle,info);
    pool=vk::raii::CommandPool(device->raii(),vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,device->queueFamilyIndices().graphicsFamily.value()));
    allocation=std::make_unique<AllocationManager>();
    allocation->initialize(instance->instance(),device->physicalDevice(),device->device(),
        device->graphicsQueue(),*pool,{});
    pipelines=std::make_unique<PipelineManager>(device->device());
    temp=std::filesystem::temp_directory_path()/("container-controller-generation-"+
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
  void load(bool implicit=false,bool variants=false) {
    auto initial=triangle("A",31,implicit);
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
    manager->initialize(path.string(),2.0f,2);
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


static_assert(!std::is_copy_constructible_v<SceneController::PreparedGeneration>);
static_assert(std::is_nothrow_move_constructible_v<SceneController::PreparedGeneration>);
static_assert(std::is_nothrow_move_assignable_v<SceneController::PreparedGeneration>);
static_assert(noexcept(std::declval<SceneController&>().exchangeGeneration(
    std::declval<SceneController::PreparedGeneration&>(),0,0,0)));

TEST_F(SceneControllerGenerationGpu,DetachedCpuAndActualGpuBuffersPreserveLiveWorld) {
  load();auto* stableWorld=&controller->world();auto* stableRegistry=&controller->world().registry();
  const auto oldVertex=controller->vertexSlice().buffer,oldIndex=controller->indexSlice().buffer;
  const auto oldObjects=controller->objectData();const auto oldRevision=controller->geometryRevision();
  auto scene=manager->prepareMergeGeneration(triangle("B",201),temp/"b.glb",{},graph);
  auto prepared=controller->prepareGeneration(scene.graph(),scene.manager(),2,true,graph.revision(),manager->generationEpoch());
  ASSERT_TRUE(prepared.ready());ASSERT_TRUE(controller->canExchangeGeneration(prepared,manager->generationEpoch()));
  EXPECT_EQ(&controller->world(),stableWorld);EXPECT_EQ(&controller->world().registry(),stableRegistry);
  EXPECT_EQ(controller->vertexSlice().buffer,oldVertex);EXPECT_EQ(controller->indexSlice().buffer,oldIndex);
  EXPECT_EQ(controller->geometryRevision(),oldRevision);EXPECT_EQ(controller->objectData().size(),oldObjects.size());
  EXPECT_EQ(prepared.world().registry().ctx().get<std::string>(),"retained context");
  ASSERT_NE(prepared.world().activeCamera(),nullptr);
  EXPECT_EQ(prepared.world().activeCamera()->data.cameraWorldPosition,glm::vec4(3,4,5,1));
  EXPECT_FLOAT_EQ(prepared.world().activeCamera()->nearPlane,.2f);EXPECT_EQ(prepared.world().pointLightCount(),1u);
  EXPECT_EQ(prepared.world().renderableCount(),2u);EXPECT_EQ(controller->world().renderableCount(),1u);
  EXPECT_EQ(prepared.objectData().size(),3u); // Two meshes plus diagnostic object.
  EXPECT_EQ(prepared.opaqueDrawCommands().size(),2u);EXPECT_TRUE(prepared.transparentDrawCommands().empty());
  EXPECT_EQ(prepared.geometryVertices().size(),6u);EXPECT_EQ(prepared.geometryIndices().size(),6u);
  EXPECT_EQ(prepared.geometryRevision(),oldRevision+1);EXPECT_EQ(prepared.objectDataRevision(),controller->objectDataRevision()+1);
  ASSERT_EQ(prepared.objectBuffers().size(),2u);EXPECT_NE(prepared.vertexSlice().buffer,oldVertex);
  const auto* objectAddress=prepared.objectData().data();const auto* bindAddress=prepared.gpuBindVertices().data();
  auto moved=std::move(prepared);EXPECT_FALSE(prepared.ready());ASSERT_TRUE(moved.ready());
  EXPECT_EQ(moved.objectData().data(),objectAddress);EXPECT_EQ(moved.gpuBindVertices().data(),bindAddress);
  checkPrepared(moved);
  moved.world().registry().ctx().get<std::string>()="candidate context";
  EXPECT_EQ(controller->world().registry().ctx().get<std::string>(),"retained context");
}

TEST_F(SceneControllerGenerationGpu,UploadFailureAndStaleProofKeepLiveResourcesAndState) {
  load();auto scene=manager->prepareMergeGeneration(triangle("B",137),temp/"b.glb",{},graph);
  const auto baseline=allocations();const auto revision=controller->geometryRevision(),graphRevision=graph.revision();
  const auto vertex=controller->vertexSlice().buffer,index=controller->indexSlice().buffer;
  auto* stableWorld=&controller->world();
  allocation->setLoadProgressCallback([](bool done){if(done) throw std::runtime_error("controller-upload-failure");});
  const auto prepare=[&] {return controller->prepareGeneration(scene.graph(),scene.manager(),2,false,graph.revision(),manager->generationEpoch());};
  EXPECT_THROW((void)prepare(),std::runtime_error);
  allocation->setLoadProgressCallback({});
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(controller->geometryRevision(),revision);EXPECT_EQ(graph.revision(),graphRevision);
  EXPECT_EQ(controller->vertexSlice().buffer,vertex);EXPECT_EQ(controller->indexSlice().buffer,index);
  EXPECT_EQ(&controller->world(),stableWorld);EXPECT_EQ(controller->world().pointLightCount(),1u);
  auto prepared=prepare();ASSERT_TRUE(controller->canExchangeGeneration(prepared,manager->generationEpoch()));
  graph.setVisible(graph.renderableNodes().front(),false);
  EXPECT_FALSE(controller->canExchangeGeneration(prepared,manager->generationEpoch()));
  EXPECT_EQ(controller->vertexSlice().buffer,vertex);EXPECT_EQ(controller->world().renderableCount(),1u);
}

TEST_F(SceneControllerGenerationGpu,CoordinatedPublishUndoRedoRetainOwnedInitialArenaSnapshot) {
  load();const auto first=uint32_t(graph.nodeCount());const auto undoRoots=graph.activeRoots();
  auto* stableController=controller.get();auto* stableWorld=&controller->world();
  const auto arenaVertex=controller->vertexSlice().buffer;
  const auto oldObjects=controller->objectData();const auto oldGeometry=controller->geometryRevision();
  auto scene=manager->prepareMergeGeneration(triangle("B",191),temp/"b.glb",{},graph);
  auto prepared=controller->prepareGeneration(scene.graph(),scene.manager(),2,false,graph.revision(),manager->generationEpoch());
  const auto prospective=prepared.geometryRevision(),prospectiveObjects=prepared.objectDataRevision();
  const auto vertexB=prepared.vertexSlice().buffer,objectB=prepared.objectBuffers()[0].buffer;
  auto append=graph.prepareAppend(scene.graph());
  ASSERT_TRUE(scene.readyForExchange(*manager,graph));ASSERT_TRUE(controller->canExchangeGeneration(prepared,manager->generationEpoch()));
  manager->exchangeMergeGeneration(scene);ASSERT_TRUE(graph.publishAppend(append));
  controller->exchangeGeneration(prepared,graph.revision(),manager->geometryRevision(),manager->generationEpoch());
  EXPECT_EQ(controller.get(),stableController);EXPECT_EQ(&controller->world(),stableWorld);
  EXPECT_EQ(controller->geometryRevision(),prospective);EXPECT_EQ(controller->objectDataRevision(),prospectiveObjects);
  EXPECT_EQ(controller->vertexSlice().buffer,vertexB);EXPECT_TRUE(controller->ownsObjectBuffer(objectB));
  EXPECT_GT(controller->geometryRevision(),oldGeometry);EXPECT_EQ(controller->world().pointLightCount(),1u);
  EXPECT_EQ(controller->world().registry().ctx().get<std::string>(),"retained context");
  EXPECT_NE(prepared.vertexSlice().buffer,arenaVertex);EXPECT_EQ(prepared.objectData().size(),oldObjects.size());
  checkPrepared(prepared);checkLive();
  const auto revision=controller->geometryRevision(),objects=controller->objectDataRevision();
  const auto update=controller->updateDeformation();
  EXPECT_FALSE(update.topologyChanged);EXPECT_FALSE(update.geometryChanged);
  EXPECT_EQ(controller->geometryRevision(),revision);EXPECT_EQ(controller->objectDataRevision(),objects);
  // Deliberately replace both original arenas: parked A must use independent
  // owned buffers, rather than aliases that happen to remain valid until now.
  allocation->resetGeometryAllocations();
  std::vector<Vertex> overwrite(4096);std::vector<uint32_t> overwriteIndices(8192);
  (void)allocation->uploadVertices(overwrite);(void)allocation->uploadIndices(overwriteIndices);
  checkPrepared(prepared);
  auto detach=graph.prepareDetachTail(first,undoRoots);ASSERT_TRUE(detach);
  scene.prepareResourceDependencies(*manager);
  manager->exchangeMergeGeneration(scene);ASSERT_TRUE(graph.publishTailChange(detach));
  controller->exchangeGeneration(prepared,graph.revision(),manager->geometryRevision(),manager->generationEpoch());
  auto tail=detach.takeDetachedTail();ASSERT_TRUE(tail);
  EXPECT_EQ(graph.nodeCount(),first);EXPECT_EQ(controller->world().renderableCount(),1u);
  EXPECT_EQ(&controller->world(),stableWorld);EXPECT_GT(controller->geometryRevision(),revision);
  EXPECT_EQ(controller->objectData().size(),oldObjects.size());checkLive();
  const auto undoRevision=controller->geometryRevision();
  auto restore=graph.prepareRestoreTail(*tail);ASSERT_TRUE(restore);
  scene.prepareResourceDependencies(*manager);
  manager->exchangeMergeGeneration(scene);ASSERT_TRUE(graph.publishTailChange(restore));
  controller->exchangeGeneration(prepared,graph.revision(),manager->geometryRevision(),manager->generationEpoch());
  EXPECT_EQ(controller->vertexSlice().buffer,vertexB);EXPECT_EQ(controller->objectBuffers()[0].buffer,objectB);
  EXPECT_GT(controller->geometryRevision(),undoRevision);EXPECT_EQ(controller->world().renderableCount(),2u);
  EXPECT_FALSE(controller->updateDeformation().topologyChanged);checkLive();
}

TEST_F(SceneControllerGenerationGpu,ProceduralAppendResizesOwnedObjectRecordWithoutAliasDoubleFree) {
  load();auto scene=manager->prepareMergeGeneration(triangle("B",171),temp/"b.glb",{},graph);
  auto prepared=controller->prepareGeneration(scene.graph(),scene.manager(),2,false,graph.revision(),manager->generationEpoch());
  auto append=graph.prepareAppend(scene.graph());manager->exchangeMergeGeneration(scene);
  ASSERT_TRUE(graph.publishAppend(append));
  controller->exchangeGeneration(prepared,graph.revision(),manager->geometryRevision(),manager->generationEpoch());
  auto alias=controller->objectBuffers()[0];auto capacity=controller->objectBufferCapacities()[0];
  const auto oldCapacity=capacity;
  const auto primitive=controller->addScenePrimitive(ScenePrimitiveKind::Cube,
      glm::translate(glm::mat4(1),glm::vec3(4,0,0)),editor);
  ASSERT_NE(primitive,SceneGraph::kInvalidNode);
  EXPECT_TRUE(controller->updateObjectBuffer(alias,capacity,{}));
  EXPECT_GT(capacity,oldCapacity);EXPECT_EQ(controller->objectBuffers()[0].buffer,alias.buffer);
  EXPECT_EQ(controller->objectBufferCapacities()[0],capacity);EXPECT_TRUE(controller->ownsObjectBuffer(alias.buffer));
  EXPECT_EQ(controller->objectData().size(),3u);EXPECT_EQ(controller->world().renderableCount(),3u);
  checkBytes(BufferSlice{alias.buffer,0,controller->objectData().size()*sizeof(ObjectData)},std::span<const ObjectData>(controller->objectData()));
  checkBytes(controller->vertexSlice(),controller->gpuBindVertices());
  // The second image still owns its prior capacity; update its real record too.
  auto second=controller->objectBuffers()[1];auto secondCapacity=controller->objectBufferCapacities()[1];
  EXPECT_TRUE(controller->updateObjectBuffer(second,secondCapacity,{}));
  EXPECT_EQ(controller->objectBuffers()[1].buffer,second.buffer);checkLive();
  EXPECT_EQ(manager->runtimePrimitiveToSource(),(std::vector<int32_t>{0,1,-1}));
}

static_assert(!std::is_copy_constructible_v<SceneController::PreparedMaterialRouting>);
static_assert(std::is_nothrow_move_constructible_v<SceneController::PreparedMaterialRouting>);
static_assert(noexcept(std::declval<SceneController&>().exchangeMaterialRouting(
    std::declval<SceneController::PreparedMaterialRouting&>(),0,0)));
static_assert(noexcept(std::declval<const SceneController&>().materialRoutingCacheCurrent(false)));

TEST_F(SceneControllerGenerationGpu,MaterialRoutingStagesActualBuffersWithStableGeometryWorldAndObjectOrder) {
  load();const auto oldObjects=controller->objectData();ASSERT_EQ(oldObjects.size(),1u);
  ASSERT_TRUE(controller->materialRoutingCacheCurrent(false));EXPECT_FALSE(controller->materialRoutingCacheCurrent(true));
  const auto oldObjectRevision=controller->objectDataRevision();
  const auto material=oldObjects[0].objectInfo.x;auto facts=materialFacts();
  facts[material].transparent=true;facts[material].doubleSided=true;
  facts[material].unlit=true;facts[material].specularGlossiness=true;facts[material].heightScale=.25f;
  auto* world=&controller->world();auto* registry=&world->registry();
  const auto* vertices=controller->geometryVertices().data();const auto* indices=controller->geometryIndices().data();
  const auto* bind=controller->gpuBindVertices().data();const auto* deformation=controller->deformationScene();
  const auto vertex=controller->vertexSlice(),index=controller->indexSlice();const auto geometry=controller->geometryRevision();
  const auto baseline=allocations();
  {
    auto prepared=controller->prepareMaterialRouting(facts,2,false,manager->generationEpoch());
    ASSERT_TRUE(prepared.ready());ASSERT_TRUE(controller->canExchangeMaterialRouting(prepared,manager->generationEpoch()));
    EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));EXPECT_FALSE(controller->materialRoutingCacheCurrent(true));
    EXPECT_EQ(controller->objectDataRevision(),oldObjectRevision);
    EXPECT_EQ(allocations(),baseline+4); // Candidate plus owned legacy object snapshot, two images each.
    EXPECT_TRUE(prepared.opaqueDrawCommands().empty());ASSERT_EQ(prepared.transparentDrawCommands().size(),1u);
    EXPECT_EQ(prepared.transparentDoubleSidedDrawCommands().size(),1u);
    EXPECT_EQ(prepared.objectData()[0].model,oldObjects[0].model);
    EXPECT_EQ(prepared.objectData()[0].temporalInfo,oldObjects[0].temporalInfo);
    EXPECT_NE(prepared.objectData()[0].objectInfo.y&kObjectFlagDoubleSided,0u);
    EXPECT_GT(prepared.objectData()[0].boundingSphere.w,oldObjects[0].boundingSphere.w);
    EXPECT_EQ(prepared.objectNodeIndices()[0],oldObjects[0].temporalInfo.z);
    ASSERT_EQ(prepared.objectBuffers().size(),2u);checkRouting(prepared);
    const auto* data=prepared.objectData().data();auto moved=std::move(prepared);
    EXPECT_FALSE(prepared.ready());EXPECT_EQ(moved.objectData().data(),data);checkRouting(moved);
  }
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(controller->geometryVertices().data(),vertices);
  EXPECT_EQ(controller->geometryIndices().data(),indices);EXPECT_EQ(controller->gpuBindVertices().data(),bind);
  EXPECT_EQ(controller->deformationScene(),deformation);EXPECT_EQ(controller->geometryRevision(),geometry);
  EXPECT_EQ(controller->vertexSlice().buffer,vertex.buffer);EXPECT_EQ(controller->vertexSlice().offset,vertex.offset);
  EXPECT_EQ(controller->indexSlice().buffer,index.buffer);EXPECT_EQ(controller->indexSlice().offset,index.offset);
  EXPECT_EQ(&controller->world(),world);EXPECT_EQ(&controller->world().registry(),registry);
  EXPECT_EQ(world->registry().ctx().get<std::string>(),"retained context");EXPECT_EQ(world->pointLightCount(),1u);
  EXPECT_EQ(world->activeCamera()->data.cameraWorldPosition,glm::vec4(3,4,5,1));
  EXPECT_EQ(controller->objectData()[0].objectInfo,oldObjects[0].objectInfo);
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));EXPECT_EQ(controller->objectDataRevision(),oldObjectRevision);
}

TEST_F(SceneControllerGenerationGpu,MaterialRoutingPublishesAllFramesAndReplaysWithoutGeometryOrCacheRebuild) {
  load();auto facts=materialFacts();const auto material=controller->objectData()[0].objectInfo.x;
  facts[material].transparent=true;auto prepared=controller->prepareMaterialRouting(facts,2,false,manager->generationEpoch());
  const auto prospective=prepared.objectDataRevision(),geometry=controller->geometryRevision();
  const auto vertex=controller->vertexSlice().buffer;const auto* vertices=controller->geometryVertices().data();
  const auto candidate=prepared.objectBuffers()[0].buffer;
  // Isolated routing fixture: the material component publishes its revision
  // separately. Its validated facts table is this API's authority.
  manager->uploadMaterialResources();
  controller->exchangeMaterialRouting(prepared,manager->materialRevision(),manager->generationEpoch());
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
  EXPECT_EQ(controller->objectDataRevision(),prospective);EXPECT_EQ(controller->geometryRevision(),geometry);
  EXPECT_EQ(controller->objectBuffers()[0].buffer,candidate);EXPECT_TRUE(controller->ownsObjectBuffer(candidate));
  EXPECT_TRUE(controller->opaqueDrawCommands().empty());EXPECT_EQ(controller->transparentDrawCommands().size(),1u);
  EXPECT_EQ(controller->geometryVertices().data(),vertices);EXPECT_EQ(controller->vertexSlice().buffer,vertex);
  ASSERT_EQ(controller->objectBuffers().size(),2u);
  for(size_t i=0;i<2;++i) {
    auto alias=controller->objectBuffers()[i];auto capacity=controller->objectBufferCapacities()[i];
    EXPECT_FALSE(controller->updateObjectBuffer(alias,capacity,{}));EXPECT_EQ(controller->objectDataRevision(),prospective);
  }
  for(const auto& object:controller->objectBuffers())
    checkBytes(BufferSlice{object.buffer,0,controller->objectData().size()*sizeof(ObjectData)},std::span<const ObjectData>(controller->objectData()));
  checkRouting(prepared);EXPECT_TRUE(prepared.transparentDrawCommands().empty());
  ASSERT_TRUE(controller->canExchangeMaterialRouting(prepared,manager->generationEpoch()));
  manager->uploadMaterialResources();controller->exchangeMaterialRouting(prepared,manager->materialRevision(),manager->generationEpoch());
  EXPECT_EQ(controller->opaqueDrawCommands().size(),1u);EXPECT_TRUE(controller->transparentDrawCommands().empty());
  EXPECT_EQ(controller->geometryRevision(),geometry);EXPECT_EQ(controller->vertexSlice().buffer,vertex);
  manager->uploadMaterialResources();controller->exchangeMaterialRouting(prepared,manager->materialRevision(),manager->generationEpoch());
  EXPECT_EQ(controller->objectBuffers()[0].buffer,candidate);EXPECT_EQ(controller->geometryVertices().data(),vertices);
}

TEST_F(SceneControllerGenerationGpu,MaterialRoutingCoversMaskSidedWindingAndDiagnosticTailWithoutTouchingGraph) {
  load();const auto node=graph.renderableNodes().front();
  graph.setLocalTransform(node,glm::scale(glm::mat4(1),glm::vec3(-1,1,1)));
  EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));
  controller->syncObjectDataFromSceneGraph(false);auto facts=materialFacts();
  ASSERT_TRUE(controller->materialRoutingCacheCurrent(false));EXPECT_FALSE(controller->materialRoutingCacheCurrent(true));
  const auto material=controller->objectData()[0].objectInfo.x;facts[material].alphaMasked=true;
  auto mask=controller->prepareMaterialRouting(facts,2,true,manager->generationEpoch());
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));EXPECT_FALSE(controller->materialRoutingCacheCurrent(true));
  EXPECT_EQ(mask.opaqueDrawCommands().size(),1u);EXPECT_TRUE(mask.transparentDrawCommands().empty());
  EXPECT_EQ(mask.opaqueWindingFlippedDrawCommands().size(),1u);EXPECT_EQ(mask.objectData().size(),2u);
  EXPECT_EQ(mask.objectNodeIndices().back(),UINT32_MAX);checkRouting(mask);
  facts[material].doubleSided=true;facts[material].unlit=true;facts[material].specularGlossiness=true;
  auto sided=controller->prepareMaterialRouting(facts,2,false,manager->generationEpoch());
  EXPECT_EQ(sided.opaqueDoubleSidedDrawCommands().size(),1u);EXPECT_TRUE(sided.opaqueWindingFlippedDrawCommands().empty());
  facts[material].transparent=true;facts[material].doubleSided=false;
  auto transparent=controller->prepareMaterialRouting(facts,2,false,manager->generationEpoch());
  EXPECT_EQ(transparent.transparentWindingFlippedDrawCommands().size(),1u);EXPECT_TRUE(transparent.opaqueDrawCommands().empty());
  EXPECT_EQ(graph.getNode(node)->localTransform,glm::scale(glm::mat4(1),glm::vec3(-1,1,1)));
  EXPECT_EQ(controller->objectData().size(),1u);EXPECT_EQ(controller->world().renderableCount(),1u);
  controller->syncObjectDataFromSceneGraph(true);
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(true));EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));
  EXPECT_EQ(controller->objectData().size(),2u);EXPECT_EQ(controller->world().renderableCount(),1u);
}

TEST_F(SceneControllerGenerationGpu,MaterialRoutingRejectsBudgetsMalformedFactsAndStaleProofAtomically) {
  load();auto facts=materialFacts();const auto baseline=allocations();const auto revision=controller->objectDataRevision();
  const auto vertex=controller->vertexSlice().buffer;
  SceneController::MaterialRoutingLimits limits;limits.maxCpuBytes=1;
  EXPECT_THROW((void)controller->prepareMaterialRouting(facts,2,false,manager->generationEpoch(),limits),std::runtime_error);
  limits={};limits.maxGpuBytes=1;
  EXPECT_THROW((void)controller->prepareMaterialRouting(facts,2,false,manager->generationEpoch(),limits),std::runtime_error);
  auto bad=facts;bad[0].gpuMaterialIndex=UINT32_MAX;
  EXPECT_THROW((void)controller->prepareMaterialRouting(bad,2,false,manager->generationEpoch()),std::runtime_error);
  bad=facts;bad[0].heightScale=std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)controller->prepareMaterialRouting(bad,2,false,manager->generationEpoch()),std::runtime_error);
  facts.pop_back();EXPECT_THROW((void)controller->prepareMaterialRouting(facts,2,false,manager->generationEpoch()),std::runtime_error);
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(controller->objectDataRevision(),revision);
  auto prepared=controller->prepareMaterialRouting(materialFacts(),2,false,manager->generationEpoch());
  EXPECT_FALSE(controller->canExchangeMaterialRouting(prepared,manager->generationEpoch()+1));
  controller->exchangeMaterialRouting(prepared,manager->materialRevision()+1,manager->generationEpoch());
  EXPECT_EQ(controller->objectDataRevision(),revision);
  controller->exchangeMaterialRouting(prepared,manager->materialRevision(),manager->generationEpoch()+1);
  EXPECT_EQ(controller->objectDataRevision(),revision);
  graph.setVisible(graph.renderableNodes().front(),false);
  EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));EXPECT_FALSE(controller->materialRoutingCacheCurrent(true));
  EXPECT_EQ(controller->world().renderableCount(),1u); // A query must not reconcile stale live ECS state.
  EXPECT_FALSE(controller->canExchangeMaterialRouting(prepared,manager->generationEpoch()));
  controller->exchangeMaterialRouting(prepared,manager->materialRevision(),manager->generationEpoch());
  EXPECT_EQ(controller->objectDataRevision(),revision);EXPECT_EQ(controller->vertexSlice().buffer,vertex);
  EXPECT_THROW((void)controller->prepareMaterialRouting(materialFacts(),2,false,manager->generationEpoch()),std::runtime_error);
  EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));EXPECT_EQ(controller->world().renderableCount(),1u);
}

TEST_F(SceneControllerGenerationGpu,MaterialRoutingOwnersSurviveFullGenerationExchangeAndProceduralResize) {
  load();auto routing=controller->prepareMaterialRouting(materialFacts(),2,false,manager->generationEpoch());
  controller->exchangeMaterialRouting(routing,manager->materialRevision(),manager->generationEpoch());
  const auto oldObject=controller->objectBuffers()[0].buffer;const auto first=uint32_t(graph.nodeCount());const auto roots=graph.activeRoots();
  auto scene=manager->prepareMergeGeneration(triangle("B",151),temp/"b.glb",{},graph);
  auto full=controller->prepareGeneration(scene.graph(),scene.manager(),2,false,graph.revision(),manager->generationEpoch());
  auto append=graph.prepareAppend(scene.graph());manager->exchangeMergeGeneration(scene);ASSERT_TRUE(graph.publishAppend(append));
  controller->exchangeGeneration(full,graph.revision(),manager->geometryRevision(),manager->generationEpoch());
  EXPECT_EQ(full.objectBuffers()[0].buffer,oldObject);checkPrepared(full);checkLive();
  auto detach=graph.prepareDetachTail(first,roots);scene.prepareResourceDependencies(*manager);
  manager->exchangeMergeGeneration(scene);ASSERT_TRUE(graph.publishTailChange(detach));
  controller->exchangeGeneration(full,graph.revision(),manager->geometryRevision(),manager->generationEpoch());
  EXPECT_EQ(controller->objectBuffers()[0].buffer,oldObject);
  auto alias=controller->objectBuffers()[0];auto capacity=controller->objectBufferCapacities()[0];
  ASSERT_NE(controller->addScenePrimitive(ScenePrimitiveKind::Cube,glm::mat4(1),editor),SceneGraph::kInvalidNode);
  EXPECT_TRUE(controller->updateObjectBuffer(alias,capacity,{}));EXPECT_EQ(controller->objectBuffers()[0].buffer,alias.buffer);
  auto other=controller->objectBuffers()[1];auto otherCapacity=controller->objectBufferCapacities()[1];
  EXPECT_TRUE(controller->updateObjectBuffer(other,otherCapacity,{}));checkLive();
}

TEST_F(SceneControllerGenerationGpu,MaterialRevisionCacheInvalidatesAndSafeAcknowledgementSkipsOnlyUnchangedRouting) {
  load();auto routing=controller->prepareMaterialRouting(materialFacts(),2,false,manager->generationEpoch());
  controller->exchangeMaterialRouting(routing,manager->materialRevision(),manager->generationEpoch());
  auto alias=controller->objectBuffers()[0];auto capacity=controller->objectBufferCapacities()[0];
  auto objects=controller->objectDataRevision();manager->uploadMaterialResources();
  EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));EXPECT_EQ(controller->objectDataRevision(),objects);
  EXPECT_FALSE(controller->updateObjectBuffer(alias,capacity,{}));EXPECT_GT(controller->objectDataRevision(),objects);
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
  objects=controller->objectDataRevision();manager->uploadMaterialResources();
  EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));
  controller->acknowledgeMaterialRevision(manager->materialRevision());
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));EXPECT_EQ(controller->objectDataRevision(),objects);
  EXPECT_FALSE(controller->updateObjectBuffer(alias,capacity,{}));EXPECT_EQ(controller->objectDataRevision(),objects);
  graph.setLocalTransform(graph.renderableNodes().front(),glm::translate(glm::mat4(1),glm::vec3(1,2,3)));
  EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));
  controller->acknowledgeMaterialRevision(manager->materialRevision());
  EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));
  EXPECT_FALSE(controller->updateObjectBuffer(alias,capacity,{}));EXPECT_GT(controller->objectDataRevision(),objects);
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
  controller->createGeometryBuffers();
  EXPECT_FALSE(controller->materialRoutingCacheCurrent(false));
}

TEST_F(SceneControllerGenerationGpu,BindingRoutingPublishesActualIdsAndPreservesExistingWorldEntities) {
  load();container::material::Material replacement;replacement.alphaMode=container::material::AlphaMode::Blend;
  replacement.doubleSided=true;replacement.heightScale=.125f;
  const auto target=manager->createMaterial(replacement);manager->uploadMaterialResources();
  controller->syncObjectDataFromSceneGraph(false);
  const auto node=controller->objectData()[0].temporalInfo.z,original=graph.getNode(node)->materialIndex;
  const auto context=bindingContext();
  auto command=PlanMaterialBindings(graph,context,std::array{MaterialBindingRequest{node,original,target}});
  auto binding=PrepareMaterialBindings(graph,context,command);
  const auto beforeGraph=graph.revision(),hierarchy=graph.hierarchyRevision();
  const auto entity=entityForNode(node);auto& registry=controller->world().registry();
  const auto* component=registry.try_get<container::ecs::MaterialComponent>(entity);
  const auto* nodeRef=registry.try_get<container::ecs::SceneNodeRef>(entity);
  const auto* vertex=controller->geometryVertices().data();const auto geometry=controller->geometryRevision();
  const auto vertexBuffer=controller->vertexSlice().buffer,indexBuffer=controller->indexSlice().buffer;
  auto* world=&controller->world();const auto entities=world->entityCount();
  const auto oldObject=controller->objectData()[0];const auto objectRevision=controller->objectDataRevision();
  auto routing=controller->prepareMaterialRouting(materialFacts(),binding,2,false,manager->generationEpoch());
  ASSERT_TRUE(routing.ready());EXPECT_EQ(routing.expectedGraphRevision(),beforeGraph);
  EXPECT_EQ(routing.candidateGraphRevision(),beforeGraph+1);ASSERT_TRUE(controller->canExchangeMaterialRouting(routing,manager->generationEpoch()));
  EXPECT_EQ(graph.getNode(node)->materialIndex,original);EXPECT_EQ(component->materialIndex,original);
  EXPECT_EQ(controller->objectData()[0].objectInfo.x,original);EXPECT_EQ(controller->objectDataRevision(),objectRevision);
  EXPECT_EQ(routing.objectData()[0].objectInfo.x,target);EXPECT_EQ(routing.transparentDoubleSidedDrawCommands().size(),1u);
  EXPECT_TRUE(routing.opaqueDrawCommands().empty());checkRouting(routing);
  auto moved=std::move(binding);EXPECT_FALSE(bool(binding));
  ASSERT_TRUE(PublishMaterialBindings(graph,moved,context));
  moved=PreparedMaterialBindingChange{};command=MaterialBindingCommand{};
  // The routing receipt owns its authority after the graph token/command die.
  EXPECT_FALSE(controller->canExchangeMaterialRouting(routing,manager->generationEpoch()));
  controller->exchangeMaterialRouting(routing,manager->materialRevision(),manager->generationEpoch());
  EXPECT_FALSE(routing.ready());EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
  EXPECT_EQ(graph.revision(),beforeGraph+1);EXPECT_EQ(graph.hierarchyRevision(),hierarchy);
  EXPECT_EQ(&controller->world(),world);EXPECT_EQ(world->entityCount(),entities);EXPECT_EQ(entityForNode(node),entity);
  EXPECT_EQ(registry.try_get<container::ecs::MaterialComponent>(entity),component);
  EXPECT_EQ(registry.try_get<container::ecs::SceneNodeRef>(entity),nodeRef);EXPECT_EQ(component->materialIndex,target);
  EXPECT_EQ(controller->objectData()[0].objectInfo.x,target);EXPECT_EQ(controller->objectData()[0].model,oldObject.model);
  EXPECT_EQ(controller->objectData()[0].temporalInfo,oldObject.temporalInfo);
  EXPECT_EQ(controller->geometryVertices().data(),vertex);EXPECT_EQ(controller->geometryRevision(),geometry);
  EXPECT_EQ(controller->vertexSlice().buffer,vertexBuffer);EXPECT_EQ(controller->indexSlice().buffer,indexBuffer);
  checkLiveObjects();const auto committedRevision=controller->objectDataRevision();
  controller->exchangeMaterialRouting(routing,manager->materialRevision(),manager->generationEpoch());
  EXPECT_EQ(controller->objectDataRevision(),committedRevision);
  // Fresh reverse preparation reuses the immutable command at the new graph
  // revision and preserves all entity identities in the opposite direction.
  auto inverseCommand=PlanMaterialBindings(graph,context,std::array{MaterialBindingRequest{node,target,original}});
  auto inverse=PrepareMaterialBindings(graph,context,inverseCommand);
  auto restore=controller->prepareMaterialRouting(materialFacts(),inverse,2,false,manager->generationEpoch());
  ASSERT_TRUE(PublishMaterialBindings(graph,inverse,context));
  controller->exchangeMaterialRouting(restore,manager->materialRevision(),manager->generationEpoch());
  EXPECT_EQ(entityForNode(node),entity);EXPECT_EQ(component->materialIndex,original);
  EXPECT_EQ(controller->objectData()[0].objectInfo.x,original);EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
  controller->syncObjectDataFromSceneGraph(false);
  EXPECT_EQ(controller->objectData()[0].objectInfo.x,original);EXPECT_EQ(controller->opaqueDrawCommands().size(),1u);
}

TEST_F(SceneControllerGenerationGpu,BindingRoutingStagesExactProspectiveMaterialTailWithoutPublishingIt) {
  load();const auto node=controller->objectData()[0].temporalInfo.z,original=graph.getNode(node)->materialIndex;
  const auto context=bindingContext();const auto target=context.runtimeMaterialCount;
  const MaterialBindingAppendTarget append{uint32_t(context.sourceMaterialToRuntime.size()),target};
  const auto command=PlanMaterialBindings(graph,context,std::array{MaterialBindingRequest{node,original,target}},append);
  auto binding=PrepareMaterialBindings(graph,context,command);auto facts=materialFacts();
  facts.push_back({target,true,false,true,false,false,.25f});
  const auto baseline=allocations();const auto objects=controller->objectDataRevision();const auto currentCount=manager->materialCount();
  {
    auto routing=controller->prepareMaterialRouting(facts,binding,2,false,manager->generationEpoch());
    ASSERT_TRUE(routing.ready());ASSERT_TRUE(controller->canExchangeMaterialRouting(routing,manager->generationEpoch()));
    EXPECT_EQ(routing.objectData()[0].objectInfo.x,target);EXPECT_EQ(routing.transparentDoubleSidedDrawCommands().size(),1u);
    EXPECT_EQ(routing.candidateGraphRevision(),graph.revision()+1);checkRouting(routing);
    controller->exchangeMaterialRouting(routing,manager->materialRevision(),manager->generationEpoch());
    EXPECT_EQ(controller->objectDataRevision(),objects);EXPECT_EQ(controller->objectData()[0].objectInfo.x,original);
    EXPECT_EQ(manager->materialCount(),currentCount);EXPECT_EQ(graph.getNode(node)->materialIndex,original);
    EXPECT_THROW((void)controller->prepareMaterialRouting(materialFacts(),binding,2,false,manager->generationEpoch()),std::runtime_error);
    auto malformed=facts;malformed.back().gpuMaterialIndex=target+1;
    EXPECT_THROW((void)controller->prepareMaterialRouting(malformed,binding,2,false,manager->generationEpoch()),std::runtime_error);
  }
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(manager->materialCount(),currentCount);
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
}

TEST_F(SceneControllerGenerationGpu,RealMaterialAppendBindingAndRoutingPublishTogetherWithoutAllocating) {
  using namespace container::geometry::gltf;
  load(false,true);
  struct Cameras {
    AllocationManager& owner;std::array<AllocatedBuffer,2> values{};
    ~Cameras(){for(auto& buffer:values) owner.destroyBuffer(buffer);}
  } cameras{*allocation};
  for(auto& buffer:cameras.values) buffer=allocation->createBuffer(sizeof(CameraData),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
  const auto node=controller->objectData()[0].temporalInfo.z,original=graph.getNode(node)->materialIndex;
  const auto beforeContext=bindingContext();const auto sourceCount=manager->animationSource().materials.size();
  const auto runtimeCount=manager->materialCount();const auto graphRevision=graph.revision();
  const auto hierarchy=graph.hierarchyRevision(),geometry=controller->geometryRevision();
  const auto* vertices=controller->geometryVertices().data();const auto* indices=controller->geometryIndices().data();
  const auto vertexBuffer=controller->vertexSlice().buffer,indexBuffer=controller->indexSlice().buffer;
  const auto entity=entityForNode(node);const auto* component=controller->world().registry().try_get<container::ecs::MaterialComponent>(entity);
  auto* world=&controller->world();auto* registry=&world->registry();const auto entityCount=world->entityCount();
  const auto sourceDefault=manager->animationSource().meshes[0].primitives[0].material;
  GltfMaterialPatch patch;patch.name="Independent assigned material";patch.alphaMode=GltfMaterialAlphaMode::Blend;
  patch.doubleSided=true;patch.values={{GltfMaterialProperty::Roughness,.25}};
  const auto plan=manager->planMaterialMakeUnique(0,patch);
  auto material=manager->prepareMaterialGeneration(plan,nullptr);
  ASSERT_EQ(material.sourceMaterial(),int32_t(sourceCount));ASSERT_EQ(material.runtimeMaterial(),runtimeCount);
  const auto target=material.runtimeMaterial();
  const auto command=PlanMaterialBindings(graph,beforeContext,std::array{MaterialBindingRequest{node,original,target}},
      MaterialBindingAppendTarget{uint32_t(material.sourceMaterial()),target});
  auto binding=PrepareMaterialBindings(graph,beforeContext,command);
  auto routing=controller->prepareMaterialRouting(material.materialProperties(),binding,2,false,manager->generationEpoch());
  material.prepareDescriptorFrames(cameras.values,routing.objectBuffers());
  ASSERT_TRUE(manager->canExchangeMaterialGeneration(material,nullptr));
  ASSERT_TRUE(CanPublishMaterialBindings(graph,binding,beforeContext));
  ASSERT_TRUE(controller->canExchangeMaterialRouting(routing,manager->generationEpoch()));
  EXPECT_EQ(graph.getNode(node)->materialIndex,original);EXPECT_EQ(component->materialIndex,original);
  EXPECT_EQ(manager->materialCount(),runtimeCount);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount);
  EXPECT_EQ(controller->objectData()[0].objectInfo.x,original);
  const auto epoch=material.generationEpoch(),revision=material.materialRevision();
  const auto candidateObject=routing.objectBuffers()[0].buffer;
  size_t attempts{};bool published{};
  {
    material_publication_test::AllocationGuard guard(true);
    published=PublishMaterialBindings(graph,binding,beforeContext);
    if(published) {
      manager->exchangeMaterialGeneration(material,nullptr);
      controller->exchangeMaterialRouting(routing,revision,epoch);
    }
    attempts=guard.allocations();
  }
  ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_FALSE(bool(material));EXPECT_FALSE(routing.ready());
  EXPECT_EQ(manager->materialCount(),runtimeCount+1);EXPECT_EQ(manager->animationSource().materials.size(),sourceCount+1);
  EXPECT_EQ(manager->sourceMaterialRuntimeMapping().back(),target);EXPECT_EQ(graph.getNode(node)->materialIndex,target);
  EXPECT_EQ(controller->objectData()[0].objectInfo.x,target);EXPECT_EQ(component->materialIndex,target);
  EXPECT_EQ(controller->objectBuffers()[0].buffer,candidateObject);EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
  EXPECT_EQ(controller->transparentDoubleSidedDrawCommands().size(),1u);EXPECT_TRUE(controller->opaqueDrawCommands().empty());
  EXPECT_EQ(graph.revision(),graphRevision+1);EXPECT_EQ(graph.hierarchyRevision(),hierarchy);
  EXPECT_EQ(&controller->world(),world);EXPECT_EQ(&controller->world().registry(),registry);
  EXPECT_EQ(world->entityCount(),entityCount);EXPECT_EQ(entityForNode(node),entity);
  EXPECT_EQ(registry->try_get<container::ecs::MaterialComponent>(entity),component);
  EXPECT_EQ(controller->geometryRevision(),geometry);EXPECT_EQ(controller->geometryVertices().data(),vertices);
  EXPECT_EQ(controller->geometryIndices().data(),indices);EXPECT_EQ(controller->vertexSlice().buffer,vertexBuffer);
  EXPECT_EQ(controller->indexSlice().buffer,indexBuffer);checkLiveObjects();
  EXPECT_EQ(manager->animationSource().meshes[0].primitives[0].material,sourceDefault);
  const auto saved=manager->buildSceneExport(graph);
  ASSERT_TRUE(saved.extensions.contains("KHR_materials_variants"));
  EXPECT_EQ(saved.extensions.at("KHR_materials_variants").Get("variants").Get(0).Get("name").Get<std::string>(),"Original");
  EXPECT_EQ(saved.extensions.at("KHR_materials_variants").Get("variants").Get(0).Get("extras").Get<int>(),16'777'217);
  EXPECT_NE(std::ranges::find(saved.extensionsRequired,std::string("KHR_materials_variants")),saved.extensionsRequired.end());
  const auto library=ParseMaterialVariants(saved);bool mapped{};
  for(const auto& primitive:library.primitives()) for(const auto& [variant,id]:primitive.mappings) {
    mapped=true;EXPECT_EQ(variant,0u);EXPECT_EQ(id,int32_t(original));
  }
  EXPECT_TRUE(mapped);EXPECT_EQ(saved.materials[target].name,"Independent assigned material");
}

TEST_F(SceneControllerGenerationGpu,BindingRoutingAcknowledgesInactiveChangesAndNoopWithoutRebuildingWorld) {
  load();const auto original=controller->objectData()[0].objectInfo.x,target=manager->defaultMaterialIndex();
  ASSERT_NE(original,target);
  const auto inactive=graph.createNode(glm::mat4(1),original,true,0,"inactive material binding");
  ASSERT_TRUE(graph.setParent(inactive,editor));ASSERT_FALSE(graph.isNodeActive(inactive));
  controller->syncObjectDataFromSceneGraph(false);ASSERT_EQ(controller->objectData().size(),1u);
  const auto context=bindingContext();const auto revision=graph.revision();const auto activeNode=controller->objectData()[0].temporalInfo.z;
  const auto entity=entityForNode(activeNode);const auto entities=controller->world().entityCount();
  const auto command=PlanMaterialBindings(graph,context,std::array{MaterialBindingRequest{inactive,original,target}});
  auto binding=PrepareMaterialBindings(graph,context,command);
  auto routing=controller->prepareMaterialRouting(materialFacts(),binding,2,false,manager->generationEpoch());
  EXPECT_EQ(routing.objectData()[0].objectInfo.x,original);EXPECT_EQ(routing.candidateGraphRevision(),revision+1);
  ASSERT_TRUE(PublishMaterialBindings(graph,binding,context));
  controller->exchangeMaterialRouting(routing,manager->materialRevision(),manager->generationEpoch());
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));EXPECT_EQ(graph.revision(),revision+1);
  EXPECT_EQ(graph.getNode(inactive)->materialIndex,target);EXPECT_EQ(entityForNode(activeNode),entity);
  EXPECT_EQ(controller->world().entityCount(),entities);checkLiveObjects();
  const auto noop=PlanMaterialBindings(graph,context,std::array{MaterialBindingRequest{inactive,target,target}});
  auto unchanged=PrepareMaterialBindings(graph,context,noop);
  auto same=controller->prepareMaterialRouting(materialFacts(),unchanged,2,false,manager->generationEpoch());
  EXPECT_EQ(same.expectedGraphRevision(),same.candidateGraphRevision());
  ASSERT_TRUE(PublishMaterialBindings(graph,unchanged,context));
  controller->exchangeMaterialRouting(same,manager->materialRevision(),manager->generationEpoch());
  EXPECT_EQ(graph.revision(),revision+1);EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
}

TEST_F(SceneControllerGenerationGpu,BindingRoutingRejectsForeignProofRawGraphAndEcsChangesWithoutPublication) {
  load();const auto node=controller->objectData()[0].temporalInfo.z,original=graph.getNode(node)->materialIndex;
  const auto target=manager->defaultMaterialIndex();ASSERT_NE(original,target);const auto context=bindingContext();
  const auto command=PlanMaterialBindings(graph,context,std::array{MaterialBindingRequest{node,original,target}});
  auto binding=PrepareMaterialBindings(graph,context,command);const auto baseline=allocations();
  SceneController::MaterialRoutingLimits limits;limits.maxCpuBytes=1;
  EXPECT_THROW((void)controller->prepareMaterialRouting(materialFacts(),binding,2,false,manager->generationEpoch(),limits),std::runtime_error);
  auto foreign=graph;const auto foreignCommand=PlanMaterialBindings(foreign,context,std::array{MaterialBindingRequest{node,original,target}});
  auto foreignBinding=PrepareMaterialBindings(foreign,context,foreignCommand);
  EXPECT_THROW((void)controller->prepareMaterialRouting(materialFacts(),foreignBinding,2,false,manager->generationEpoch()),std::runtime_error);
  EXPECT_EQ(allocations(),baseline);
  const auto entity=entityForNode(node);
  auto* nodeRef=controller->world().registry().try_get<container::ecs::SceneNodeRef>(entity);
  nodeRef->nodeIndex=UINT32_MAX;
  EXPECT_THROW((void)controller->prepareMaterialRouting(materialFacts(),binding,2,false,manager->generationEpoch()),std::runtime_error);
  EXPECT_EQ(allocations(),baseline);nodeRef->nodeIndex=node;
  auto routing=controller->prepareMaterialRouting(materialFacts(),binding,2,false,manager->generationEpoch());
  const auto objects=controller->objectDataRevision();auto* raw=graph.getNode(node);
  ++raw->sourceInstanceIndex;
  EXPECT_FALSE(controller->canExchangeMaterialRouting(routing,manager->generationEpoch()));--raw->sourceInstanceIndex;
  EXPECT_TRUE(controller->canExchangeMaterialRouting(routing,manager->generationEpoch()));
  auto* component=controller->world().registry().try_get<container::ecs::MaterialComponent>(entity);
  component->materialIndex=target;EXPECT_FALSE(controller->canExchangeMaterialRouting(routing,manager->generationEpoch()));
  component->materialIndex=original;ASSERT_TRUE(controller->canExchangeMaterialRouting(routing,manager->generationEpoch()));
  ASSERT_TRUE(PublishMaterialBindings(graph,binding,context));
  component->materialIndex=target;
  controller->exchangeMaterialRouting(routing,manager->materialRevision(),manager->generationEpoch());
  EXPECT_EQ(controller->objectDataRevision(),objects);EXPECT_EQ(controller->objectData()[0].objectInfo.x,original);
  component->materialIndex=original;
  controller->exchangeMaterialRouting(routing,manager->materialRevision(),manager->generationEpoch());
  EXPECT_GT(controller->objectDataRevision(),objects);EXPECT_EQ(component->materialIndex,target);
  EXPECT_TRUE(controller->materialRoutingCacheCurrent(false));
}
} // namespace
