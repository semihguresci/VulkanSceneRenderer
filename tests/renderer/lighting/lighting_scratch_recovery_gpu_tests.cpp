#include "../scene/camera_generation_test_support.h"

#include <Container/ecs/World.h>
#include <Container/renderer/lighting/LightingManager.h>

#ifndef CONTAINER_LIGHTING_SHADER_ROOT
#define CONTAINER_LIGHTING_SHADER_ROOT "."
#endif
#ifndef CONTAINER_LIGHTING_SCRATCH_SHADER_PATH
#define CONTAINER_LIGHTING_SCRATCH_SHADER_PATH "spv_shaders/lighting_scratch_contract.comp.spv"
#endif

namespace container::renderer {
struct LightingScratchRecoveryTestAccess {
  static void failNext(LightingManager& manager,uint8_t allocation) {manager.scratchAllocationFailureForTest_=allocation;}
  static void limitStorageRange(LightingManager& manager, VkDeviceSize range) {
    manager.clusteredStorageRangeLimit_ = range;
  }
  static uint64_t epoch(const LightingManager& manager) {return manager.descriptorBindingEpoch_;}
  static bool ready(const LightingManager& manager) {return manager.tiledScratchResourcesReady();}
  static const auto& grid(const LightingManager& manager) {return manager.tileGridSsbo_;}
  static const auto& indices(const LightingManager& manager) {return manager.lightIndexListSsbo_;}
  static const auto& stats(const LightingManager& manager) {return manager.lightStatsBuffer_;}
  static VkDescriptorSet published(const LightingManager& manager) {return manager.tiledSets_.front()[2];}
  static VkDescriptorSetLayout layout(const LightingManager& manager) {return manager.tileCullSet2Layout_;}
};
}

namespace {
using namespace container::renderer;
using Access=LightingScratchRecoveryTestAccess;

class LightingScratchRecoveryGpu:public CameraProjectionGenerationGpu {
protected:
  const char* gpuEnvironment() const noexcept override {return "CONTAINER_RUN_GPU_LIGHT_EDITS";}
  void SetUp() override {
    CameraProjectionGenerationGpu::SetUp();if(::testing::Test::IsSkipped() || HasFatalFailure()) return;
    auto source=cameraAsset();source.lights.resize(1);source.lights[0].type="point";source.lights[0].intensity=7;
    source.lights[0].range=10;source.nodes[1].light=0;source.extensionsUsed={"KHR_lights_punctual"};load(std::move(source));
    for(const auto id:graph.rootNodes()) if(graph.getNode(id)->editorSceneRoot) root=id;
    ASSERT_NE(root,SceneGraph::kInvalidNode);
    lighting=std::make_unique<LightingManager>(device,*allocation,*pipelines,manager.get(),graph,world);
    lighting->setRootNode(root);lighting->createDescriptorResources(2);
    lighting->createTiledResources(CONTAINER_LIGHTING_SHADER_ROOT,{32,32});lighting->updateLightingData();lighting->uploadLightingData();
    ASSERT_TRUE(Access::ready(*lighting));
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();lighting.reset();CameraProjectionGenerationGpu::TearDown();
  }
  LightEditSnapshot selected() {
    for(const auto& row:lighting->editableLights()) if(row.type==EditableLightType::Point&&row.source==EditableLightSource::Imported)
      return lighting->lightEditSnapshot(row.id);
    throw std::runtime_error("Missing scratch fixture point light");
  }
  LightEditPatch patch() const {
    LightEditPatch result;result.fields=LightEditFieldBit(LightEditField::Intensity);result.values.intensity=11;return result;
  }
  glm::uvec4 probe() {
    auto output=allocation->createBuffer(sizeof(glm::uvec4),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    struct Cleanup {AllocationManager* owner;AllocatedBuffer* buffer;~Cleanup(){owner->destroyBuffer(*buffer);}} cleanup{allocation.get(),&output};
    std::array<vk::DescriptorSetLayoutBinding,4> bindings;
    for(uint32_t i=0;i<bindings.size();++i) bindings[i]=vk::DescriptorSetLayoutBinding(i,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute);
    vk::raii::DescriptorSetLayout directLayout(device->raii(),vk::DescriptorSetLayoutCreateInfo({},bindings));
    const vk::DescriptorPoolSize poolSize(vk::DescriptorType::eStorageBuffer,4);
    vk::raii::DescriptorPool descriptors(device->raii(),vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,1,poolSize));
    const std::array directLayouts{*directLayout};auto sets=device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors,directLayouts));
    const std::array<vk::DescriptorBufferInfo,4> infos{{{output.buffer,0,sizeof(glm::uvec4)},
        {Access::grid(*lighting).buffer,0,sizeof(TileLightGrid)},
        {Access::indices(*lighting).buffer,0,sizeof(uint32_t)},
        {Access::stats(*lighting).buffer,0,sizeof(uint32_t)}}};
    std::array<vk::WriteDescriptorSet,4> writes;
    for(uint32_t i=0;i<writes.size();++i) writes[i]=vk::WriteDescriptorSet(*sets[0],i,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&infos[i]);
    device->raii().updateDescriptorSets(writes,{});
    const std::array layouts{vk::DescriptorSetLayout(Access::layout(*lighting)),*directLayout};
    const vk::PushConstantRange push(vk::ShaderStageFlagBits::eCompute,0,sizeof(uint32_t));
    vk::raii::PipelineLayout layout(device->raii(),vk::PipelineLayoutCreateInfo({},layouts,push));
    const auto code=readFile(CONTAINER_LIGHTING_SCRATCH_SHADER_PATH);
    vk::raii::ShaderModule shader(device->raii(),vk::ShaderModuleCreateInfo({},code.size(),reinterpret_cast<const uint32_t*>(code.data())));
    vk::raii::Pipeline pipeline(device->raii(),nullptr,vk::ComputePipelineCreateInfo({},
        vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,*shader,"computeMain"),*layout));
    allocation->executeStagedCommands([&](VkCommandBuffer command) {
      vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,*pipeline);
      const std::array<VkDescriptorSet,2> raw{Access::published(*lighting),*sets[0]};
      vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,*layout,0,uint32_t(raw.size()),raw.data(),0,nullptr);
      const auto barrier=[&](VkPipelineStageFlags target,VkAccessFlags access) {
        VkMemoryBarrier value{VK_STRUCTURE_TYPE_MEMORY_BARRIER};value.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        value.dstAccessMask=access;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,target,0,1,&value,0,nullptr,0,nullptr);
      };
      // Poison today's exact resources first; then production descriptors must
      // overwrite those same resources, verified by the independent reader.
      for(const uint32_t mode:{2u,0u,1u}) {
        vkCmdPushConstants(command,*layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(mode),&mode);vkCmdDispatch(command,1,1,1);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_HOST_READ_BIT);
      }
    });
    if(vmaInvalidateAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
      throw std::runtime_error("Cannot invalidate scratch descriptor readback");
    glm::uvec4 result;std::memcpy(&result,output.allocation_info.pMappedData,sizeof(result));return result;
  }
  void exerciseFailure(uint8_t fault) {
    // Legacy generations intentionally remain usable after a successful resize.
    auto detachedScene=std::make_unique<SceneManager>(*allocation,*pipelines,device,container::app::AppConfig{});
    detachedScene->initialize((temp/"camera.glb").string(),1,2);auto detachedGraph=graph;
    auto detachedWorld=world.cloneStateWithoutContext();
    auto legacy=lighting->prepareGeneration(*detachedScene,detachedGraph,*detachedWorld,root,lighting->snapshotSceneLightEdits(),2);
    ASSERT_TRUE(lighting->canExchangeGeneration(legacy));
    auto light=lighting->prepareLightEdit(selected().target,patch());ASSERT_TRUE(lighting->canExchangeLightEdit(light));
    const auto target=manager->cameraViewPoseTarget(graph,node(1));ASSERT_TRUE(target);
    auto pose=manager->prepareCameraViewPoseEdit(*target,glm::lookAt(glm::vec3(6,3,9),glm::vec3(1,1,0),glm::vec3(0,1,0)),graph,player.get(),1.f);
    auto poseWorld=world.cloneStateWithoutContext();
    auto poseLighting=lighting->prepareGraphPoseGeneration(*manager,pose.candidateGraph(),*poseWorld,root,
        pose.candidateLights(),pose.candidateBounds(),lighting->snapshotSceneLightEdits(),2);
    ASSERT_TRUE(lighting->canExchangeGeneration(poseLighting));
    const auto oldEpoch=Access::epoch(*lighting),sourceEpoch=manager->generationEpoch(),cameraRevision=manager->cameraRevision();
    const auto graphRevision=graph.revision(),editRevision=lighting->lightEditRevision();
    const auto oldSet=lighting->lightDescriptorSet(0);const auto oldPoint=lighting->pointLightBuffer().buffer;
    const auto before=lighting->pointLightsSsbo();const auto worldCount=world.entityCount();
    Access::failNext(*lighting,fault);EXPECT_THROW(lighting->resizeTiledResources({96,64}),std::bad_alloc);
    EXPECT_GT(Access::epoch(*lighting),oldEpoch);EXPECT_FALSE(Access::ready(*lighting));
    EXPECT_EQ(Access::grid(*lighting).buffer==VK_NULL_HANDLE,fault==1);
    EXPECT_EQ(Access::indices(*lighting).buffer,VK_NULL_HANDLE);
    bool legacyReady{},legacyDone{},lightReady{},lightDone{},poseReady{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);
      legacyReady=lighting->canExchangeGeneration(legacy);legacyDone=lighting->exchangeGeneration(legacy);
      lightReady=lighting->canExchangeLightEdit(light);lightDone=lighting->exchangeLightEdit(light);
      poseReady=lighting->canExchangeGeneration(poseLighting);attempts=deny.allocations();}
    EXPECT_FALSE(legacyReady);EXPECT_FALSE(legacyDone);EXPECT_FALSE(lightReady);EXPECT_FALSE(lightDone);EXPECT_FALSE(poseReady);EXPECT_EQ(attempts,0u);
    EXPECT_EQ(lighting->lightDescriptorSet(0),oldSet);EXPECT_EQ(lighting->pointLightBuffer().buffer,oldPoint);
    ASSERT_EQ(lighting->pointLightsSsbo().size(),before.size());
    EXPECT_EQ(std::memcmp(lighting->pointLightsSsbo().data(),before.data(),before.size()*sizeof(PointLightData)),0);
    EXPECT_EQ(lighting->lightEditRevision(),editRevision);EXPECT_EQ(world.entityCount(),worldCount);
    EXPECT_EQ(manager->generationEpoch(),sourceEpoch);EXPECT_EQ(manager->cameraRevision(),cameraRevision);EXPECT_EQ(graph.revision(),graphRevision);
    EXPECT_THROW((void)lighting->prepareLightEdit(selected().target,patch()),std::invalid_argument);
    EXPECT_THROW((void)lighting->prepareGeneration(*detachedScene,detachedGraph,*detachedWorld,root,lighting->snapshotSceneLightEdits(),2),std::invalid_argument);
    // The failed attempt already recorded this exact cluster count. A retry
    // must repair live descriptors even though the extent/count are equal.
    lighting->resizeTiledResources({96,64});ASSERT_TRUE(Access::ready(*lighting));
    EXPECT_EQ(probe(),glm::uvec4(0x12345678,0x23456789,0x3456789a,0x456789ab));
    EXPECT_FALSE(lighting->canExchangeLightEdit(light));EXPECT_FALSE(lighting->canExchangeGeneration(poseLighting));
    for(int cycle=0;cycle<2;++cycle) {
      bool ready{},done{};size_t allocations{};
      {material_publication_test::AllocationGuard deny(true);ready=lighting->canExchangeGeneration(legacy);
        done=lighting->exchangeGeneration(legacy);allocations=deny.allocations();}
      EXPECT_TRUE(ready);EXPECT_TRUE(done);EXPECT_EQ(allocations,0u);
      EXPECT_EQ(probe(),glm::uvec4(0x12345678,0x23456789,0x3456789a,0x456789ab));
    }
    auto fresh=lighting->prepareLightEdit(selected().target,patch());ASSERT_TRUE(lighting->canExchangeLightEdit(fresh));
  }
  container::ecs::World world;uint32_t root{SceneGraph::kInvalidNode};std::unique_ptr<LightingManager> lighting;
};

TEST_F(LightingScratchRecoveryGpu, FailureBeforeGridRejectsAllPublicationAndSameExtentRetryRepairsRealDescriptors) {exerciseFailure(1);}
TEST_F(LightingScratchRecoveryGpu, FailureBeforeIndexRejectsAllPublicationAndSameExtentRetryRepairsRealDescriptors) {exerciseFailure(2);}
TEST_F(LightingScratchRecoveryGpu, GenuineSameGridNoopAndRejectedOversizedExtentPreserveLightStage) {
  auto prepared=lighting->prepareLightEdit(selected().target,patch());const auto epoch=Access::epoch(*lighting);
  const auto grid=Access::grid(*lighting).buffer,index=Access::indices(*lighting).buffer;
  lighting->resizeTiledResources({32,32});EXPECT_EQ(Access::epoch(*lighting),epoch);EXPECT_TRUE(lighting->canExchangeLightEdit(prepared));
  EXPECT_THROW(lighting->resizeTiledResources({UINT32_MAX,UINT32_MAX}),std::length_error);
  EXPECT_EQ(Access::epoch(*lighting),epoch);EXPECT_EQ(Access::grid(*lighting).buffer,grid);EXPECT_EQ(Access::indices(*lighting).buffer,index);
  bool ready{},done{};size_t attempts{};{material_publication_test::AllocationGuard deny(true);ready=lighting->canExchangeLightEdit(prepared);
    done=lighting->exchangeLightEdit(prepared);attempts=deny.allocations();}
  EXPECT_TRUE(ready);EXPECT_TRUE(done);EXPECT_EQ(attempts,0u);
  EXPECT_EQ(probe(),glm::uvec4(0x12345678,0x23456789,0x3456789a,0x456789ab));
}
TEST_F(LightingScratchRecoveryGpu, RangeLimitedResizePreservesLightEditsAndRecoversDescriptors) {
  const auto oldEpoch = Access::epoch(*lighting);
  const auto tiledLayout = lighting->tiledDescriptorSetLayout();
  auto staleEdit = lighting->prepareLightEdit(selected().target, patch());
  ASSERT_TRUE(lighting->canExchangeLightEdit(staleEdit));

  Access::limitStorageRange(*lighting, 128ull * 1024 * 1024);
  lighting->resizeTiledResources({3840, 2160});
  EXPECT_EQ(lighting->clusteredLightCapacity().status,
            ClusteredLightCapacityStatus::StorageRangeExceeded);
  EXPECT_EQ(lighting->clusteredLightCapacity().lightIndexListBytes, 265420800u);
  EXPECT_GT(Access::epoch(*lighting), oldEpoch);
  EXPECT_FALSE(lighting->isTiledLightingReady());
  EXPECT_EQ(lighting->tileGridBufferSize(), 0u);
  EXPECT_EQ(Access::grid(*lighting).buffer, VK_NULL_HANDLE);
  EXPECT_EQ(Access::indices(*lighting).buffer, VK_NULL_HANDLE);
  EXPECT_EQ(lighting->tiledDescriptorSetLayout(), tiledLayout);
  EXPECT_FALSE(lighting->canExchangeLightEdit(staleEdit));

  auto fallbackEdit = lighting->prepareLightEdit(selected().target, patch());
  ASSERT_TRUE(lighting->canExchangeLightEdit(fallbackEdit));
  const auto fallbackEpoch = Access::epoch(*lighting);
  lighting->resizeTiledResources({3840, 2160});
  EXPECT_EQ(Access::epoch(*lighting), fallbackEpoch);
  ASSERT_TRUE(lighting->exchangeLightEdit(fallbackEdit));
  ASSERT_FALSE(lighting->pointLightsSsbo().empty());
  EXPECT_FLOAT_EQ(lighting->pointLightsSsbo().front().colorIntensity.a, 11.0f);

  lighting->resizeTiledResources({48, 32});
  EXPECT_GT(Access::epoch(*lighting), fallbackEpoch);
  ASSERT_TRUE(lighting->isTiledLightingReady());
  EXPECT_EQ(lighting->tileGridBufferSize(), 768u);
  EXPECT_EQ(lighting->tiledDescriptorSetLayout(), tiledLayout);
  EXPECT_EQ(probe(), glm::uvec4(0x12345678, 0x23456789, 0x3456789a, 0x456789ab));
}
TEST_F(LightingScratchRecoveryGpu, OriginalManualSunUndoRevealsTodaysGeneratedBirthAndRedoKeepsOriginalManualBirth) {
  const auto directional=[&](EditableLightSource source) {
    for(const auto& row:lighting->editableLights()) if(row.type==EditableLightType::Directional&&row.source==source)
      return lighting->lightEditSnapshot(row.id);
    throw std::runtime_error("Missing directional lifetime fixture");
  };
  const auto generated=directional(EditableLightSource::Generated);
  auto added=lighting->prepareLightAdd(EditableLightType::Directional);const auto command=added.command();
  const auto publish=[&](LightingManager::PreparedLightEdit& value) {
    bool ready{},done{};size_t attempts{};{material_publication_test::AllocationGuard deny(true);
      ready=lighting->canExchangeLightEdit(value);done=lighting->exchangeLightEdit(value);attempts=deny.allocations();}
    EXPECT_TRUE(ready);EXPECT_TRUE(done);EXPECT_EQ(attempts,0u);
  };
  publish(added);const auto manual=directional(EditableLightSource::Manual);EXPECT_EQ(manual.target.incarnation,command.target().incarnation);
  auto settings=lighting->lightingSettings();settings.directionalIntensity+=2;lighting->setLightingSettings(settings);
  lighting->updateLightingData();lighting->uploadLightingData();
  auto undo=lighting->prepareLightEditReplay(command,false);publish(undo);
  const auto today=directional(EditableLightSource::Generated);
  EXPECT_NE(today.target.incarnation,generated.target.incarnation);EXPECT_NE(today.target.incarnation,manual.target.incarnation);
  EXPECT_THROW((void)lighting->prepareLightEdit(generated.target,patch()),std::invalid_argument);
  auto redo=lighting->prepareLightEditReplay(command,true);publish(redo);
  EXPECT_EQ(directional(EditableLightSource::Manual).target.incarnation,manual.target.incarnation);
}
TEST_F(LightingScratchRecoveryGpu, OversizedSelectedLegacyDirectionalLabelRejectsBeforeSnapshotCopyOrPublication) {
  auto edits=lighting->snapshotSceneLightEdits();
  auto manual=editableDirectionalLight(EditableLightSource::Manual,glm::vec3(0),lighting->lightingData(),false);
  manual.label.assign(5000,'L');edits.directionalOverride=manual;
  lighting->restoreSceneLightEdits(edits);lighting->updateLightingData();lighting->uploadLightingData();
  // Warm only immutable identity rows through another selected light. The
  // canonical visible sun label is short; the oversized retained Before is
  // nevertheless rejected before any selected record/label is copied.
  (void)selected();
  const auto set=lighting->lightDescriptorSet(0);const auto buffer=lighting->pointLightBuffer().buffer;
  const auto revision=lighting->lightEditRevision(),source=manager->generationEpoch();
  LightEditSnapshot snapshot;size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);snapshot=lighting->lightEditSnapshot(manual.id);attempts=deny.allocations();}
  EXPECT_FALSE(snapshot.available);EXPECT_EQ(snapshot.reason,"Label too long");EXPECT_EQ(attempts,0u);
  EXPECT_EQ(snapshot.target.incarnation,0u);EXPECT_TRUE(snapshot.light.label.empty());
  EXPECT_THROW((void)lighting->prepareLightEdit(snapshot.target,patch()),std::invalid_argument);
  EXPECT_EQ(lighting->lightDescriptorSet(0),set);EXPECT_EQ(lighting->pointLightBuffer().buffer,buffer);
  EXPECT_EQ(lighting->lightEditRevision(),revision);EXPECT_EQ(manager->generationEpoch(),source);
  EXPECT_EQ(lighting->snapshotSceneLightEdits().directionalOverride->label.size(),5000u);
}
} // namespace
