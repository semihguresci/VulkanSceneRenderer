#include "../scene/camera_generation_test_support.h"

#include <Container/ecs/World.h>
#include <Container/renderer/lighting/LightingManager.h>

namespace {
using namespace container::renderer;
using V=tinygltf::Value;using O=V::Object;using A=V::Array;
#ifndef CONTAINER_LIGHTING_SHADER_ROOT
#define CONTAINER_LIGHTING_SHADER_ROOT "."
#endif
tinygltf::Model lightingPoseAsset() {
  auto source=cameraAsset();source.nodes[1].mesh=0;source.nodes[1].light=0;source.nodes[1].children={5,6,7};
  source.nodes.resize(8);source.nodes[5].light=1;source.nodes[5].translation={0,2,0};
  source.nodes[6].light=2;source.nodes[6].translation={2,0,1};source.nodes[7].translation={-2,1,0};
  source.lights.resize(3);source.lights[0].type="point";source.lights[0].color={1,.2,.1};source.lights[0].intensity=3;source.lights[0].range=8;
  source.lights[1].type="directional";source.lights[1].color={.2,.4,1};source.lights[1].intensity=2;
  source.lights[2].type="spot";source.lights[2].color={.5,1,.2};source.lights[2].intensity=5;source.lights[2].range=12;
  source.lights[2].spot.innerConeAngle=.2;source.lights[2].spot.outerConeAngle=.6;
  source.extensionsUsed={"KHR_lights_punctual","EXT_lights_area"};
  source.extensions["EXT_lights_area"]=V(O{{"lights",V(A{V(O{{"type",V(std::string("rect"))},
      {"width",V(2.)},{"height",V(4.)},{"intensity",V(7.)},{"color",V(A{V(.1),V(.4),V(1.)})}})})}});
  source.nodes[7].extensions["EXT_lights_area"]=V(O{{"light",V(0)}});return source;
}
void near3(const glm::vec3& actual,const glm::vec3& expected,float epsilon=3e-5f) {
  for(int i=0;i<3;++i) EXPECT_NEAR(actual[i],expected[i],epsilon);
}
class CameraPoseLightingGpu:public CameraProjectionGenerationGpu {
protected:
  const char* gpuEnvironment() const noexcept override {return "CONTAINER_RUN_GPU_CAMERA_POSES";}
  void SetUp() override {
    CameraProjectionGenerationGpu::SetUp();if(::testing::Test::IsSkipped() || HasFatalFailure()) return;
    load(lightingPoseAsset());
    for(const auto id:graph.rootNodes()) if(graph.getNode(id)->editorSceneRoot) root=id;
    ASSERT_NE(root,SceneGraph::kInvalidNode);
    CameraData camera;camera.cameraWorldPosition={4,5,6,1};(void)world.setActiveCamera(camera,.2f,90);
    world.registry().ctx().emplace<std::string>("Live context is not copied by lighting");
    lighting=std::make_unique<LightingManager>(device,*allocation,*pipelines,manager.get(),graph,world);
    lighting->setRootNode(root);lighting->createDescriptorResources(2);
    lighting->createTiledResources(CONTAINER_LIGHTING_SHADER_ROOT,{32,32});
    lighting->updateLightingData();lighting->uploadLightingData();
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();lighting.reset();CameraProjectionGenerationGpu::TearDown();
  }
  glm::mat4 desired() const {
    return glm::lookAt(glm::vec3(6,-1,9),glm::vec3(5,0,5),glm::normalize(glm::vec3(.25f,1,.1f)));
  }
  SceneManager::PreparedCameraViewPoseEdit pose(const glm::mat4& view) {
    const auto weak=manager->cameraViewPoseTarget(graph,node(1));if(!weak) throw std::runtime_error("Missing light-bearing camera target");
    return manager->prepareCameraViewPoseEdit(*weak,view,graph,player.get(),2.f);
  }
  LightingManager::PreparedGeneration stage(const SceneManager::PreparedCameraViewPoseEdit& prepared,container::ecs::World& detached,
      const LightingGraphPoseGenerationLimits& limits={}) {
    return lighting->prepareGraphPoseGeneration(*manager,prepared.candidateGraph(),detached,root,
        prepared.candidateLights(),prepared.candidateBounds(),lighting->snapshotSceneLightEdits(),2,limits);
  }
  template<class T> T read(const AllocatedBuffer& buffer,size_t element=0) const {
    T value{};if(!buffer.allocation || !buffer.allocation_info.pMappedData) throw std::runtime_error("Missing mapped pose lighting allocation");
    const auto offset=element*sizeof(T);
    if(vmaInvalidateAllocation(allocation->memoryManager()->allocator(),buffer.allocation,offset,sizeof(T))!=VK_SUCCESS)
      throw std::runtime_error("Cannot invalidate mapped pose lighting readback");
    std::memcpy(&value,static_cast<const std::byte*>(buffer.allocation_info.pMappedData)+offset,sizeof(T));return value;
  }
  container::ecs::World world;uint32_t root{SceneGraph::kInvalidNode};std::unique_ptr<LightingManager> lighting;
};

TEST_F(CameraPoseLightingGpu, CandidateLightsBoundsAndMappedFramesAreIsolatedThenCommitInExactProspectivePhase) {
  ASSERT_EQ(manager->authoredPointLights().size(),2u);ASSERT_EQ(manager->authoredDirectionalLights().size(),1u);ASSERT_EQ(manager->authoredAreaLights().size(),1u);
  const auto view=desired();const glm::mat4 worldView=glm::inverse(view);
  const glm::vec3 eye(worldView[3]),right(worldView[0]),up(worldView[1]),back(worldView[2]);
  const auto oldPoint=lighting->pointLightBuffer().buffer,oldArea=lighting->areaLightBuffer().buffer;
  const auto oldSet=lighting->lightDescriptorSet(0),oldTile=lighting->tiledDescriptorSet();const auto oldGrid=lighting->tileGridBuffer();
  const auto beforeUbo=read<LightingData>(lighting->lightingBuffer(0));const auto beforeLight=read<PointLightData>(lighting->pointLightBuffer());
  const auto epoch=manager->generationEpoch(),revision=manager->cameraRevision();const auto graphRevision=graph.revision();
  const auto baseline=allocations();const auto* context=world.registry().ctx().find<std::string>();ASSERT_NE(context,nullptr);
  {
    auto source=pose(view);ASSERT_TRUE(source);ASSERT_TRUE(source.changed());auto detached=world.cloneStateWithoutContext();
    auto prepared=stage(source,*detached);ASSERT_TRUE(prepared);EXPECT_GT(prepared.ownedBytes(),0u);EXPECT_GT(prepared.ownedGpuBytes(),0u);
    ASSERT_TRUE(lighting->canExchangeGeneration(prepared));EXPECT_EQ(allocations(),baseline+4);
    ASSERT_EQ(prepared.pointLightsSsbo().size(),2u);ASSERT_EQ(prepared.areaLightsSsbo().size(),1u);
    near3(glm::vec3(prepared.pointLightsSsbo()[0].positionRadius),eye);
    near3(glm::vec3(prepared.pointLightsSsbo()[1].positionRadius),eye+2.f*right+back);
    near3(glm::vec3(prepared.pointLightsSsbo()[1].directionInnerCos),-back);
    near3(glm::vec3(prepared.lightingData().directionalDirection),-back);
    near3(glm::vec3(prepared.areaLightsSsbo()[0].positionRange),eye-2.f*right+up);
    near3(glm::vec3(prepared.areaLightsSsbo()[0].directionType),-back);
    near3(glm::vec3(read<PointLightData>(prepared.pointLightBuffer()).positionRadius),eye);
    near3(glm::vec3(read<AreaLightData>(prepared.areaLightBuffer()).positionRange),eye-2.f*right+up);
    for(const auto& frame:prepared.lightingBuffers()) {
      const auto data=read<LightingData>(frame);near3(glm::vec3(data.directionalDirection),-back);EXPECT_EQ(data.pointLightCount,2u);EXPECT_EQ(data.areaLightCount,1u);
    }
    const auto stagedPoint=prepared.pointLightBuffer().buffer;const auto stagedSet=prepared.lightDescriptorSet(0);
    EXPECT_EQ(lighting->pointLightBuffer().buffer,oldPoint);EXPECT_EQ(lighting->lightDescriptorSet(0),oldSet);
    EXPECT_EQ(read<PointLightData>(lighting->pointLightBuffer()).positionRadius,beforeLight.positionRadius);
    EXPECT_EQ(read<LightingData>(lighting->lightingBuffer(0)).directionalDirection,beforeUbo.directionalDirection);
    EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->cameraRevision(),revision);EXPECT_EQ(graph.revision(),graphRevision);
    bool initialReady{},earlyExchange{},sourceReady{},sourcePublished{},published{},repeated{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);
      initialReady=lighting->canExchangeGeneration(prepared);earlyExchange=lighting->exchangeGeneration(prepared);
      sourceReady=manager->canExchangeCameraViewPoseEdit(source,graph,player.get(),2.f,&view);
      sourcePublished=manager->exchangeCameraViewPoseEdit(source,graph,player.get(),2.f,&view);
      world.swapStatePreservingContext(*detached);
      published=lighting->exchangeGeneration(prepared);repeated=lighting->exchangeGeneration(prepared);attempts=deny.allocations();}
    EXPECT_TRUE(initialReady);EXPECT_FALSE(earlyExchange);EXPECT_TRUE(sourceReady);EXPECT_TRUE(sourcePublished);EXPECT_TRUE(published);EXPECT_FALSE(repeated);
    EXPECT_FALSE(prepared);EXPECT_EQ(attempts,0u);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->cameraRevision(),revision+1);
    EXPECT_EQ(graph.revision(),graphRevision+1);EXPECT_EQ(lighting->pointLightBuffer().buffer,stagedPoint);EXPECT_EQ(lighting->lightDescriptorSet(0),stagedSet);
    EXPECT_EQ(lighting->tileGridBuffer(),oldGrid);EXPECT_NE(lighting->tiledDescriptorSet(),oldTile);
    EXPECT_NE(lighting->pointLightBuffer().buffer,oldPoint);EXPECT_NE(lighting->areaLightBuffer().buffer,oldArea);
    EXPECT_EQ(world.registry().ctx().find<std::string>(),context);EXPECT_EQ(world.pointLightCount(),2u);
    near3(glm::vec3(read<PointLightData>(lighting->pointLightBuffer()).positionRadius),eye);
  }
  EXPECT_EQ(allocations(),baseline); // old lighting payload retired with consumed stage
  // Both original override owners have died; fresh live evaluation must use the
  // now-published source, not an escaped pointer to the old swapped tables.
  lighting->updateLightingData();lighting->uploadLightingData();
  near3(glm::vec3(read<PointLightData>(lighting->pointLightBuffer()).positionRadius),eye);
  near3(glm::vec3(read<AreaLightData>(lighting->areaLightBuffer()).positionRange),eye-2.f*right+up);
}

TEST_F(CameraPoseLightingGpu, CurrentSettingsSourceRevisionAndFrameBindingMutationsRejectWithoutAllocating) {
  const auto view=desired();auto source=pose(view);auto detached=world.cloneStateWithoutContext();auto prepared=stage(source,*detached);
  const auto oldPoint=lighting->pointLightBuffer().buffer;const auto oldSettings=lighting->lightingSettings();
  auto settings=oldSettings;settings.environmentIntensity=std::nextafter(settings.environmentIntensity,2.f);lighting->setLightingSettings(settings);
  bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=lighting->canExchangeGeneration(prepared);published=lighting->exchangeGeneration(prepared);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(lighting->pointLightBuffer().buffer,oldPoint);
  lighting->setLightingSettings(oldSettings);ASSERT_TRUE(lighting->canExchangeGeneration(prepared));
  GltfCameraProjectionPatch patch;patch.values={{GltfCameraProperty::PerspectiveNear,.2}};
  auto projection=manager->prepareCameraProjectionEdit(target(),patch,graph,player.get(),2.f);
  ASSERT_TRUE(manager->exchangeCameraProjectionEdit(projection,graph,player.get(),2.f));
  {material_publication_test::AllocationGuard deny(true);ready=lighting->canExchangeGeneration(prepared);published=lighting->exchangeGeneration(prepared);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(lighting->pointLightBuffer().buffer,oldPoint);
  auto freshSource=pose(view);auto freshWorld=world.cloneStateWithoutContext();auto frames=stage(freshSource,*freshWorld);
  ASSERT_TRUE(lighting->canExchangeGeneration(frames));
  const auto grid=lighting->tileGridBuffer();const auto gridBytes=lighting->tileGridBufferSize();
  lighting->resizeTiledResources({31,31}); // same 2x2 tile grid as the 32x32 fixture
  ASSERT_EQ(lighting->tileGridBuffer(),grid);ASSERT_EQ(lighting->tileGridBufferSize(),gridBytes);
  {material_publication_test::AllocationGuard deny(true);ready=lighting->canExchangeGeneration(frames);attempts=deny.allocations();}
  EXPECT_TRUE(ready);EXPECT_EQ(attempts,0u);
  lighting->resizeTiledResources({64,64});
  // Size proves actual replacement even if a driver recycles a VkBuffer handle.
  ASSERT_GT(lighting->tileGridBufferSize(),gridBytes);
  bool sourcePublished{};
  {material_publication_test::AllocationGuard deny(true);
    ready=lighting->canExchangeGeneration(frames);
    sourcePublished=manager->exchangeCameraViewPoseEdit(freshSource,graph,player.get(),2.f,&view);
    world.swapStatePreservingContext(*freshWorld);
    published=lighting->exchangeGeneration(frames);attempts=deny.allocations();}
  EXPECT_TRUE(sourcePublished); // also exercise rejection in the prospective phase
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(lighting->pointLightBuffer().buffer,oldPoint);
}

TEST_F(CameraPoseLightingGpu, BoundsAndCurrentImportedManualOverridesUseCandidateDataWithoutRewindingEdits) {
  PointLightData manual;manual.positionRadius={20,21,22,4};manual.colorIntensity={.1f,.2f,.3f,17};
  auto edits=lighting->snapshotSceneLightEdits();edits.manualPointLights={manual};lighting->restoreSceneLightEdits(edits);
  lighting->updateLightingData();lighting->uploadLightingData();
  const auto view=desired();auto source=pose(view);auto detached=world.cloneStateWithoutContext();auto prepared=stage(source,*detached);
  ASSERT_EQ(prepared.pointLightsSsbo().size(),3u);EXPECT_EQ(prepared.pointLightsSsbo().back().positionRadius,manual.positionRadius);
  const auto anchor=source.candidateBounds();const auto direction=glm::vec3(prepared.lightingData().directionalDirection);
  const auto expectedPosition=anchor.center-direction*(std::max(anchor.radius,1.f)*1.15f);
  // Editable directional position is derived from candidate bounds (the node
  // owns moving mesh geometry), not merely from the old live source center.
  EXPECT_NE(anchor.center,manager->modelBounds().center);
  ASSERT_TRUE(manager->exchangeCameraViewPoseEdit(source,graph,player.get(),2.f,&view));
  world.swapStatePreservingContext(*detached);ASSERT_TRUE(lighting->exchangeGeneration(prepared));
  const auto sun=std::find_if(lighting->editableLights().begin(),lighting->editableLights().end(),
      [](const auto& light){return light.type==EditableLightType::Directional;});ASSERT_NE(sun,lighting->editableLights().end());
  near3(sun->position,expectedPosition);EXPECT_EQ(lighting->snapshotSceneLightEdits().manualPointLights[0].positionRadius,manual.positionRadius);
  EXPECT_EQ(read<PointLightData>(lighting->pointLightBuffer(),2).positionRadius,manual.positionRadius);
}

TEST_F(CameraPoseLightingGpu, BoundsLimitsLiveAliasesDeniedPreparationAndAbandonmentPreserveLiveGpuPayload) {
  const auto view=desired();auto source=pose(view);auto detached=world.cloneStateWithoutContext();const auto baseline=allocations();
  const auto oldPoint=lighting->pointLightBuffer().buffer;const auto oldSet=lighting->lightDescriptorSet(0);const auto count=world.pointLightCount();
  LightingGraphPoseGenerationLimits limits;limits.maxPreparedCpuBytes=1;EXPECT_THROW((void)stage(source,*detached,limits),std::invalid_argument);
  limits={};limits.maxPreparedGpuBytes=1;EXPECT_THROW((void)stage(source,*detached,limits),std::invalid_argument);
  limits={};limits.maxGraphNodes=1;EXPECT_THROW((void)stage(source,*detached,limits),std::invalid_argument);
  EXPECT_THROW((void)lighting->prepareGraphPoseGeneration(*manager,graph,*detached,root,source.candidateLights(),source.candidateBounds(),
      lighting->snapshotSceneLightEdits(),2),std::invalid_argument);
  EXPECT_THROW((void)stage(source,world),std::invalid_argument);
  const auto edits=lighting->snapshotSceneLightEdits();bool denied{};
  {material_publication_test::AllocationGuard deny(true);try {(void)lighting->prepareGraphPoseGeneration(*manager,source.candidateGraph(),*detached,root,
      source.candidateLights(),source.candidateBounds(),edits,2);}catch(const std::bad_alloc&){denied=true;}}
  EXPECT_TRUE(denied);EXPECT_EQ(allocations(),baseline);
  {auto prepared=stage(source,*detached);ASSERT_TRUE(prepared);EXPECT_EQ(allocations(),baseline+4);}
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(lighting->pointLightBuffer().buffer,oldPoint);EXPECT_EQ(lighting->lightDescriptorSet(0),oldSet);
  EXPECT_EQ(world.pointLightCount(),count);EXPECT_TRUE(manager->canExchangeCameraViewPoseEdit(source,graph,player.get(),2.f,&view));
}
} // namespace
