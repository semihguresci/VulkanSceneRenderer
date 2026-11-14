#include "../scene/camera_generation_test_support.h"

#include <Container/ecs/World.h>
#include <Container/renderer/lighting/ImportedSceneLightEdits.h>
#include <Container/renderer/lighting/LightingManager.h>

#include <bit>
#include <limits>

#ifndef CONTAINER_LIGHTING_SHADER_ROOT
#define CONTAINER_LIGHTING_SHADER_ROOT "."
#endif
#ifndef CONTAINER_LIGHT_EDIT_SHADER_PATH
#define CONTAINER_LIGHT_EDIT_SHADER_PATH "spv_shaders/light_edit_contract.comp.spv"
#endif

namespace {
using namespace container::renderer;
using V=tinygltf::Value;using O=V::Object;using A=V::Array;
using Probe=std::array<glm::uvec4,14>;
static_assert(offsetof(LightingData, environmentOrientation)==5*sizeof(glm::uvec4));
static_assert(sizeof(PointLightData)==6*sizeof(glm::uvec4));
static_assert(sizeof(AreaLightData)==5*sizeof(glm::uvec4));
glm::uvec4 bits(const glm::vec4& value) {glm::uvec4 out;std::memcpy(&out,&value,sizeof(out));return out;}
glm::vec4 floats(const glm::uvec4& value) {glm::vec4 out;std::memcpy(&out,&value,sizeof(out));return out;}
void exactProbe(const Probe& actual,const Probe& expected) {
  for(size_t row=0;row<actual.size();++row) for(int cell=0;cell<4;++cell)
    EXPECT_EQ(actual[row][cell],expected[row][cell])<<"row "<<row<<" cell "<<cell;
}
uint32_t fields(std::initializer_list<LightEditField> selected) {
  uint32_t result{};for(const auto field:selected) result|=LightEditFieldBit(field);return result;
}
tinygltf::Model lightAsset() {
  auto source=cameraAsset();source.nodes.resize(9);source.lights.resize(3);
  for(int row=0;row<3;++row) {source.nodes[size_t(5+row)].light=row;source.scenes[0].nodes.push_back(5+row);}
  source.nodes[5].translation={3,2,1};source.nodes[6].translation={-2,1,3};
  source.lights[0].name="Known point";source.lights[0].type="point";source.lights[0].color={.25,.5,1};source.lights[0].intensity=7;source.lights[0].range=10;
  source.lights[1].name="Known spot";source.lights[1].type="spot";source.lights[1].color={1,.25,.125};source.lights[1].intensity=5;source.lights[1].range=12;
  source.lights[1].spot.innerConeAngle=.2;source.lights[1].spot.outerConeAngle=.6;
  source.lights[2].name="Known sun";source.lights[2].type="directional";source.lights[2].color={.5,.25,1};source.lights[2].intensity=3;
  source.extensionsUsed={"KHR_lights_punctual","EXT_lights_area"};
  source.extensions["EXT_lights_area"]=V(O{{"lights",V(A{V(O{{"type",V(std::string("rect"))},
      {"width",V(2.)},{"height",V(4.)},{"intensity",V(7.)},{"color",V(A{V(.125),V(.5),V(1.)})}})})}});
  source.nodes[8].translation={-3,4,2};source.nodes[8].extensions["EXT_lights_area"]=V(O{{"light",V(0)}});
  source.scenes[0].nodes.push_back(8);return source;
}
int scalarAccessor(tinygltf::Model& source,const std::array<float,2>& values) {
  auto& data=source.buffers[0].data;while(data.size()%4) data.push_back(0);const size_t start=data.size();data.resize(start+sizeof(values));
  std::memcpy(data.data()+start,values.data(),sizeof(values));tinygltf::BufferView view;view.buffer=0;view.byteOffset=start;view.byteLength=sizeof(values);
  const auto viewId=int(source.bufferViews.size());source.bufferViews.push_back(view);tinygltf::Accessor accessor;
  accessor.bufferView=viewId;accessor.count=2;accessor.type=TINYGLTF_TYPE_SCALAR;accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.minValues={std::min(values[0],values[1])};accessor.maxValues={std::max(values[0],values[1])};
  const auto result=int(source.accessors.size());source.accessors.push_back(accessor);return result;
}
void pointerTrack(tinygltf::Model& source,int times,int values,const char* path) {
  auto& clip=source.animations[0];tinygltf::AnimationSampler sampler;sampler.input=times;sampler.output=values;sampler.interpolation="LINEAR";
  const auto slot=int(clip.samplers.size());clip.samplers.push_back(sampler);tinygltf::AnimationChannel channel;
  channel.sampler=slot;channel.target_node=-1;channel.target_path="pointer";
  channel.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::string(path))}});clip.channels.push_back(channel);
}

class LightEditGenerationGpu:public CameraProjectionGenerationGpu {
protected:
  const char* gpuEnvironment() const noexcept override {return "CONTAINER_RUN_GPU_LIGHT_EDITS";}
  void SetUp() override {
    CameraProjectionGenerationGpu::SetUp();if(::testing::Test::IsSkipped() || HasFatalFailure()) return;
    resetLighting(lightAsset());
  }
  void resetLighting(tinygltf::Model source) {
    if(device) device->raii().waitIdle();lighting.reset();world.clear();load(std::move(source));root=SceneGraph::kInvalidNode;
    for(const auto id:graph.rootNodes()) if(graph.getNode(id)->editorSceneRoot) root=id;
    if(root==SceneGraph::kInvalidNode) throw std::runtime_error("Missing lighting fixture root");
    CameraData camera;camera.cameraWorldPosition={4,5,6,1};(void)world.setActiveCamera(camera,.2f,90);
    lighting=std::make_unique<LightingManager>(device,*allocation,*pipelines,manager.get(),graph,world);
    lighting->setRootNode(root);lighting->createDescriptorResources(2);lighting->createTiledResources(CONTAINER_LIGHTING_SHADER_ROOT,{32,32});
    lighting->updateLightingData();lighting->uploadLightingData();
  }
  void TearDown() override {
    if(device) device->raii().waitIdle();lighting.reset();CameraProjectionGenerationGpu::TearDown();
  }
  LightEditSnapshot selected(EditableLightType type,EditableLightSource source=EditableLightSource::Imported) const {
    for(const auto& light:lighting->editableLights()) if(light.type==type && light.source==source) return lighting->lightEditSnapshot(light.id);
    throw std::runtime_error("Missing selected light fixture");
  }
  bool publish(LightingManager::PreparedLightEdit& prepared) {
    bool ready{},done{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=lighting->canExchangeLightEdit(prepared);done=lighting->exchangeLightEdit(prepared);attempts=deny.allocations();}
    EXPECT_EQ(attempts,0u);return ready&&done&&!prepared;
  }
  void rejected(LightingManager::PreparedLightEdit& prepared) {
    bool ready{},done{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=lighting->canExchangeLightEdit(prepared);done=lighting->exchangeLightEdit(prepared);attempts=deny.allocations();}
    EXPECT_FALSE(ready);EXPECT_FALSE(done);EXPECT_EQ(attempts,0u);
  }
  Probe read(uint32_t image=0,uint32_t point=0,uint32_t area=0) {
    constexpr size_t bytes=sizeof(Probe);auto output=allocation->createBuffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
    struct Cleanup {AllocationManager* owner;AllocatedBuffer* value;~Cleanup(){owner->destroyBuffer(*value);}} cleanup{allocation.get(),&output};
    std::fill_n(static_cast<uint32_t*>(output.allocation_info.pMappedData),bytes/sizeof(uint32_t),0xdeadbeefu);
    if(vmaFlushAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot poison light readback");
    const vk::DescriptorSetLayoutBinding binding(0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute);
    vk::raii::DescriptorSetLayout outputLayout(device->raii(),vk::DescriptorSetLayoutCreateInfo({},binding));
    const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer,1);vk::raii::DescriptorPool descriptors(device->raii(),vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,1,size));
    const std::array outputLayouts{*outputLayout};auto sets=device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors,outputLayouts));
    const vk::DescriptorBufferInfo info(output.buffer,0,bytes);const vk::WriteDescriptorSet write(*sets[0],0,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&info);device->raii().updateDescriptorSets(write,{});
    struct Query {uint32_t point,area;} query{point,area};
    const std::array layouts{vk::DescriptorSetLayout(lighting->lightDescriptorSetLayout()),*outputLayout};
    const vk::PushConstantRange push(vk::ShaderStageFlagBits::eCompute,0,sizeof(Query));vk::raii::PipelineLayout layout(device->raii(),vk::PipelineLayoutCreateInfo({},layouts,push));
    const auto code=readFile(CONTAINER_LIGHT_EDIT_SHADER_PATH);vk::raii::ShaderModule shader(device->raii(),vk::ShaderModuleCreateInfo({},code.size(),reinterpret_cast<const uint32_t*>(code.data())));
    vk::raii::Pipeline pipeline(device->raii(),nullptr,vk::ComputePipelineCreateInfo({},vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,*shader,"computeMain"),*layout));
    allocation->executeStagedCommands([&](VkCommandBuffer command) {
      vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,*pipeline);const std::array<VkDescriptorSet,2> raw{lighting->lightDescriptorSet(image),*sets[0]};
      vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,*layout,0,2,raw.data(),0,nullptr);vkCmdPushConstants(command,*layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Query),&query);vkCmdDispatch(command,1,1,1);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    });
    if(vmaInvalidateAllocation(allocation->memoryManager()->allocator(),output.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot invalidate light readback");
    Probe result;std::memcpy(result.data(),output.allocation_info.pMappedData,bytes);return result;
  }
  void everyFrame(const Probe& expected,uint32_t point=0,uint32_t area=0) {for(uint32_t frame=0;frame<2;++frame) exactProbe(read(frame,point,area),expected);}
  LightEditPatch pointPatch(float intensity=11) {
    LightEditPatch patch;patch.fields=fields({LightEditField::Position,LightEditField::Color,LightEditField::Intensity,LightEditField::Range});
    patch.values.position={-4,-3,2};patch.values.color={.125f,.25f,.5f};patch.values.intensity=intensity;patch.values.range=5;return patch;
  }
  container::ecs::World world;uint32_t root{SceneGraph::kInvalidNode};std::unique_ptr<LightingManager> lighting;
};

TEST_F(LightEditGenerationGpu, ImportedPointPublishesActualDescriptorsForEveryImageAndOriginalUndoRedo) {
  const auto before=read();EXPECT_EQ(before[2].x,2u);EXPECT_EQ(before[3].z,1u);
  EXPECT_EQ(floats(before[5]),glm::vec4(3,2,1,10));EXPECT_EQ(floats(before[6]),glm::vec4(.25f,.5f,1,7));
  EXPECT_EQ(floats(before[0]),glm::vec4(0,0,-1,0));EXPECT_EQ(floats(before[1]),glm::vec4(.5f,.25f,1,3));
  auto expected=before;expected[5]=bits({-4,-3,2,5});expected[6]=bits({.125f,.25f,.5f,11});
  const auto oldSet=lighting->lightDescriptorSet(0);const auto oldBuffer=lighting->pointLightBuffer().buffer;
  const auto epoch=manager->generationEpoch(),geometry=manager->geometryRevision(),material=manager->materialRevision();
  const auto worldCount=world.entityCount();const auto* camera=world.activeCamera();
  auto& context=world.registry().ctx().emplace<std::string>(1024*1024,'L');const auto* contextBytes=context.data();
  auto stage=lighting->prepareLightEdit(selected(EditableLightType::Point).target,pointPatch());const auto command=stage.command();
  everyFrame(before);ASSERT_FALSE(command.published());ASSERT_TRUE(publish(stage));ASSERT_TRUE(command.published());ASSERT_TRUE(command.applied());everyFrame(expected);
  EXPECT_NE(lighting->lightDescriptorSet(0),oldSet);EXPECT_NE(lighting->pointLightBuffer().buffer,oldBuffer);
  for(int cycle=0;cycle<2;++cycle) {
    auto undo=lighting->prepareLightEditReplay(command,false);ASSERT_TRUE(publish(undo));EXPECT_FALSE(command.applied());everyFrame(before);
    auto redo=lighting->prepareLightEditReplay(command,true);ASSERT_TRUE(publish(redo));EXPECT_TRUE(command.applied());everyFrame(expected);
  }
  EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(manager->materialRevision(),material);
  EXPECT_EQ(world.entityCount(),worldCount);EXPECT_EQ(world.activeCamera(),camera);EXPECT_EQ(context.data(),contextBytes);
  EXPECT_EQ(world.registry().ctx().find<std::string>(),&context);
}

TEST_F(LightEditGenerationGpu, AcquiredImageUploadLeavesOtherLightDescriptorsOnPriorSnapshot) {
  const auto before = read(0);
  exactProbe(read(1), before);
  EXPECT_NE(lighting->pointLightBuffer(0).buffer, lighting->pointLightBuffer(1).buffer);
  EXPECT_NE(lighting->areaLightBuffer(0).buffer, lighting->areaLightBuffer(1).buffer);
  ASSERT_NE(lighting->tiledDescriptorSet(0), VK_NULL_HANDLE);
  ASSERT_NE(lighting->tiledDescriptorSet(1), VK_NULL_HANDLE);
  EXPECT_NE(lighting->tiledDescriptorSet(0), lighting->tiledDescriptorSet(1));

  auto edits = lighting->snapshotSceneLightEdits();
  ASSERT_FALSE(lighting->pointLightsSsbo().empty());
  ASSERT_FALSE(lighting->areaLightsSsbo().empty());
  auto point = lighting->pointLightsSsbo().front();
  auto area = lighting->areaLightsSsbo().front();
  point.colorIntensity.a = 17.0f;
  area.colorIntensity.a = 13.0f;
  edits.importedPointOverrides.resize(1);
  edits.importedAreaOverrides.resize(1);
  edits.importedPointOverrides.front() = point;
  edits.importedAreaOverrides.front() = area;
  lighting->restoreSceneLightEdits(edits);
  lighting->updateLightingData();
  lighting->uploadLightingData(1);

  exactProbe(read(0), before);
  const auto updated = read(1);
  EXPECT_FLOAT_EQ(floats(updated[6]).w, 17.0f);
  EXPECT_FLOAT_EQ(floats(updated[10]).w, 13.0f);
  lighting->uploadLightingData();
  exactProbe(read(0), updated);
  exactProbe(read(1), updated);
}

TEST_F(LightEditGenerationGpu, ImportedPointHistorySurvivesVisibilityAndRejectsSourceReplacement) {
  const auto pointNode=node(5);
  ASSERT_NE(pointNode,SceneGraph::kInvalidNode);
  const auto initial=selected(EditableLightType::Point);
  const auto spot=selected(EditableLightType::Spot);
  ASSERT_TRUE(initial.available);
  ASSERT_TRUE(spot.available);
  auto edit=lighting->prepareLightEdit(initial.target,pointPatch());
  const auto command=edit.command();
  ASSERT_TRUE(publish(edit));

  ImportedSceneLightEdits overrides;
  const auto refresh=[&](auto change) {
    auto edits=lighting->snapshotSceneLightEdits();
    overrides.capture(manager->publishedAuthoredSceneLights(),edits);
    change();
    manager->refreshActiveScene(graph);
    lighting->restoreSceneLightEdits(overrides.merge(manager->publishedAuthoredSceneLights(),std::move(edits)));
    lighting->updateLightingData();
    lighting->uploadLightingData();
  };
  refresh([&] { graph.setVisible(pointNode,false); });
  const auto packedSpot=selected(EditableLightType::Spot);
  EXPECT_EQ(packedSpot.target.id.index,initial.target.id.index);
  EXPECT_EQ(packedSpot.target.incarnation,spot.target.incarnation);
  EXPECT_THROW((void)lighting->prepareLightEditReplay(command,false),std::invalid_argument);
  EXPECT_TRUE(command.applied());

  refresh([&] { graph.setVisible(pointNode,true); });
  EXPECT_EQ(selected(EditableLightType::Point).target.incarnation,initial.target.incarnation);
  EXPECT_EQ(selected(EditableLightType::Point).light.intensity,11.f);
  auto undo=lighting->prepareLightEditReplay(command,false);
  ASSERT_TRUE(publish(undo));
  EXPECT_EQ(selected(EditableLightType::Point).light.intensity,7.f);
  auto redo=lighting->prepareLightEditReplay(command,true);
  ASSERT_TRUE(publish(redo));
  EXPECT_EQ(selected(EditableLightType::Point).light.intensity,11.f);

  refresh([&] { graph.setVisible(pointNode,false); });
  auto retirement=graph.prepareRetireSubtree(pointNode);
  ASSERT_TRUE(retirement);
  refresh([&] { ASSERT_TRUE(graph.publishRetirement(retirement)); });
  EXPECT_THROW((void)lighting->prepareLightEditReplay(command,false),std::invalid_argument);
  EXPECT_TRUE(command.applied());

  resetLighting(lightAsset());
  const auto replacement=selected(EditableLightType::Point);
  EXPECT_EQ(replacement.target.id.index,initial.target.id.index);
  EXPECT_NE(replacement.target.incarnation,initial.target.incarnation);
  EXPECT_THROW((void)lighting->prepareLightEditReplay(command,false),std::invalid_argument);
  EXPECT_EQ(selected(EditableLightType::Point).light.intensity,7.f);
}

TEST_F(LightEditGenerationGpu, AreaBasisAndSizeAndDirectionalUboAreReadFromPublishedSets) {
  const auto before=read();EXPECT_EQ(floats(before[9]).x,-3);EXPECT_EQ(floats(before[9]).y,4);EXPECT_EQ(floats(before[9]).z,2);
  EXPECT_EQ(floats(before[10]),glm::vec4(.125f,.5f,1,7));EXPECT_EQ(floats(before[12]).w,1);EXPECT_EQ(floats(before[13]).w,2);
  auto areaExpected=before;areaExpected[9]=bits({2,3,4,6});areaExpected[10]=bits({.5f,.25f,.125f,13});
  areaExpected[11]=bits({0,-1,0,kAreaLightTypeRectangle});areaExpected[12]=bits({1,0,0,2});
  // Independent right-handed basis: (-Y) cross (+X) is +Z. Keep the
  // arithmetic's signed zero as the probe compares the actual upload bits.
  areaExpected[13]=bits(glm::vec4(glm::cross(glm::vec3(0,-1,0),glm::vec3(1,0,0)),3));
  LightEditPatch patch;patch.fields=fields({LightEditField::Position,LightEditField::Direction,LightEditField::Color,LightEditField::Intensity,
      LightEditField::Range,LightEditField::AreaSize,LightEditField::AreaBasis});
  patch.values.position={2,3,4};patch.values.direction={0,-1,0};patch.values.color={.5f,.25f,.125f};patch.values.intensity=13;
  patch.values.range=6;patch.values.areaHalfSize={2,3};patch.values.tangent={1,0,0};patch.values.bitangent={0,0,1};
  auto area=lighting->prepareLightEdit(selected(EditableLightType::Area).target,patch);const auto areaCommand=area.command();ASSERT_TRUE(publish(area));everyFrame(areaExpected);
  auto sunExpected=areaExpected;sunExpected[0]=bits({1,0,0,0});sunExpected[1]=bits({.125f,.5f,.25f,9});
  LightEditPatch sun;sun.fields=fields({LightEditField::Direction,LightEditField::Color,LightEditField::Intensity});
  sun.values.direction={1,0,0};sun.values.color={.125f,.5f,.25f};sun.values.intensity=9;
  auto stage=lighting->prepareLightEdit(selected(EditableLightType::Directional).target,sun);const auto sunCommand=stage.command();ASSERT_TRUE(publish(stage));everyFrame(sunExpected);
  auto sunUndo=lighting->prepareLightEditReplay(sunCommand,false);ASSERT_TRUE(publish(sunUndo));everyFrame(areaExpected);
  auto areaUndo=lighting->prepareLightEditReplay(areaCommand,false);ASSERT_TRUE(publish(areaUndo));everyFrame(before);
  auto areaRedo=lighting->prepareLightEditReplay(areaCommand,true);ASSERT_TRUE(publish(areaRedo));everyFrame(areaExpected);
  auto sunRedo=lighting->prepareLightEditReplay(sunCommand,true);ASSERT_TRUE(publish(sunRedo));everyFrame(sunExpected);
}

TEST_F(LightEditGenerationGpu, SpotTypeDirectionAndBothConeCosinesSurviveGpuUndoRedo) {
  const auto before=read(0,1);EXPECT_EQ(floats(before[5]),glm::vec4(-2,1,3,12));EXPECT_EQ(floats(before[6]),glm::vec4(1,.25f,.125f,5));
  EXPECT_EQ(floats(before[8]).y,kLocalShadowTypeSpot);auto expected=before;expected[5]=bits({1,2,3,4});expected[6]=bits({.25f,.5f,1,8});
  expected[7]=bits({0,-1,0,1});expected[8].x=std::bit_cast<uint32_t>(std::cos(glm::radians(45.f)));
  expected[8].y=std::bit_cast<uint32_t>(kLocalShadowTypeSpot); // unchanged assigned shadow layer/count stay exact
  LightEditPatch patch;patch.fields=fields({LightEditField::Position,LightEditField::Direction,LightEditField::Color,LightEditField::Intensity,LightEditField::Range,LightEditField::SpotCone});
  patch.values.position={1,2,3};patch.values.direction={0,-1,0};patch.values.color={.25f,.5f,1};patch.values.intensity=8;patch.values.range=4;
  patch.values.innerConeDegrees=0;patch.values.outerConeDegrees=45;
  auto stage=lighting->prepareLightEdit(selected(EditableLightType::Spot).target,patch);const auto command=stage.command();ASSERT_TRUE(publish(stage));everyFrame(expected,1);
  auto undo=lighting->prepareLightEditReplay(command,false);ASSERT_TRUE(publish(undo));everyFrame(before,1);
  auto redo=lighting->prepareLightEditReplay(command,true);ASSERT_TRUE(publish(redo));everyFrame(expected,1);
}

TEST_F(LightEditGenerationGpu, ManualAndGeneratedOriginalTargetsRetainIndependentPhysicalRows) {
  const auto manual=lighting->addManualEditableLight(EditableLightType::Point);lighting->updateLightingData();lighting->uploadLightingData();
  ASSERT_EQ(manual.source,EditableLightSource::Manual);const auto before=read(0,2);ASSERT_EQ(before[2].x,3u);
  auto expected=before;expected[5]=bits({-4,-3,2,5});expected[6]=bits({.125f,.25f,.5f,11});
  auto stage=lighting->prepareLightEdit(lighting->lightEditSnapshot(manual).target,pointPatch());const auto command=stage.command();ASSERT_TRUE(publish(stage));everyFrame(expected,2);
  const auto untouched=read(0,0);EXPECT_EQ(floats(untouched[5]),glm::vec4(3,2,1,10));
  auto undo=lighting->prepareLightEditReplay(command,false);ASSERT_TRUE(publish(undo));everyFrame(before,2);
  auto redo=lighting->prepareLightEditReplay(command,true);ASSERT_TRUE(publish(redo));everyFrame(expected,2);
  resetLighting(cameraAsset());const auto generated=selected(EditableLightType::Directional,EditableLightSource::Generated);ASSERT_TRUE(generated.available);
  const auto generatedBefore=read();EXPECT_EQ(generatedBefore[2].x,0u);auto generatedExpected=generatedBefore;
  generatedExpected[0]=bits({1,0,0,0});generatedExpected[1]=bits({.125f,.5f,.25f,9});
  LightEditPatch generatedPatch;generatedPatch.fields=fields({LightEditField::Direction,LightEditField::Color,LightEditField::Intensity});
  generatedPatch.values.direction={1,0,0};generatedPatch.values.color={.125f,.5f,.25f};generatedPatch.values.intensity=9;
  auto generatedStage=lighting->prepareLightEdit(generated.target,generatedPatch);const auto generatedCommand=generatedStage.command();ASSERT_TRUE(publish(generatedStage));everyFrame(generatedExpected);
  auto generatedUndo=lighting->prepareLightEditReplay(generatedCommand,false);ASSERT_TRUE(publish(generatedUndo));everyFrame(generatedBefore);
  auto generatedRedo=lighting->prepareLightEditReplay(generatedCommand,true);ASSERT_TRUE(publish(generatedRedo));everyFrame(generatedExpected);
  EXPECT_THROW((void)lighting->prepareLightEditReplay(command,false),std::invalid_argument);
}

TEST_F(LightEditGenerationGpu, AbandonedBudgetDeniedAndStalePreparedEditsNeverAlterPublishedDescriptors) {
  const auto target=selected(EditableLightType::Point).target;const auto before=read();const auto baseline=allocations();const auto oldSet=lighting->lightDescriptorSet(0);
  {auto abandoned=lighting->prepareLightEdit(target,pointPatch());ASSERT_TRUE(abandoned);EXPECT_GT(allocations(),baseline);everyFrame(before);}
  EXPECT_EQ(allocations(),baseline);EXPECT_EQ(lighting->lightDescriptorSet(0),oldSet);
  for(int budget=0;budget<3;++budget) {
    LightEditLimits limits;if(budget==0) limits.maxPreparedCpuBytes=1;if(budget==1) limits.maxPreparedGpuBytes=1;if(budget==2) limits.maxCommandBytes=1;
    EXPECT_THROW((void)lighting->prepareLightEdit(target,pointPatch(),nullptr,limits),std::invalid_argument);EXPECT_EQ(allocations(),baseline);everyFrame(before);
  }
  bool denied{};{material_publication_test::AllocationGuard fail(false,0);try {(void)lighting->prepareLightEdit(target,pointPatch());}catch(const std::bad_alloc&) {denied=true;}}
  EXPECT_TRUE(denied);EXPECT_EQ(allocations(),baseline);everyFrame(before);
  auto stale=lighting->prepareLightEdit(target,pointPatch(20));auto fresh=lighting->prepareLightEdit(target,pointPatch());const auto command=fresh.command();ASSERT_TRUE(publish(fresh));
  auto expected=before;expected[5]=bits({-4,-3,2,5});expected[6]=bits({.125f,.25f,.5f,11});rejected(stale);everyFrame(expected);
  auto undo=lighting->prepareLightEditReplay(command,false);ASSERT_TRUE(publish(undo));everyFrame(before);rejected(stale);everyFrame(before);
}

TEST_F(LightEditGenerationGpu, GestureUpdatesKeepOneOriginalBeforeAndCancelRemovesAbsentOverride) {
  const auto before=read();auto begin=lighting->prepareLightEdit(selected(EditableLightType::Point).target,pointPatch(9));const auto command=begin.command();ASSERT_TRUE(publish(begin));
  auto update=lighting->prepareLightEdit(selected(EditableLightType::Point).target,pointPatch(17),&command);const auto extended=update.command();ASSERT_TRUE(publish(update));
  auto expected=before;expected[5]=bits({-4,-3,2,5});expected[6]=bits({.125f,.25f,.5f,17});everyFrame(expected);
  EXPECT_TRUE(extended.applied());EXPECT_EQ(extended.target().incarnation,command.target().incarnation);
  EXPECT_EQ(extended.target().incarnation,selected(EditableLightType::Point).target.incarnation);
  EXPECT_THROW((void)lighting->prepareLightEditReplay(command,false),std::invalid_argument);
  auto cancel=lighting->prepareLightEditReplay(extended,false);ASSERT_TRUE(publish(cancel));everyFrame(before);
  const auto edits=lighting->snapshotSceneLightEdits();ASSERT_FALSE(edits.importedPointOverrides.empty());EXPECT_FALSE(edits.importedPointOverrides[0]);
  auto redo=lighting->prepareLightEditReplay(extended,true);ASSERT_TRUE(publish(redo));everyFrame(expected);
}

TEST_F(LightEditGenerationGpu, CurrentAnimatedSourceAndMaterialClockAreNotRewoundByOverrideUndo) {
  auto source=lightAsset();source.extensionsUsed.push_back("KHR_animation_pointer");source.animations.emplace_back();
  const auto times=scalarAccessor(source,{0,2}),intensities=scalarAccessor(source,{7,11}),roughness=scalarAccessor(source,{.2f,.8f});
  pointerTrack(source,times,intensities,"/extensions/KHR_lights_punctual/lights/0/intensity");
  pointerTrack(source,times,roughness,"/materials/0/pbrMetallicRoughness/roughnessFactor");resetLighting(std::move(source));
  auto dirty=player->selectClip(manager->animationSource(),0);dirty.merge(player->seek(manager->animationSource(),.5));manager->publishAnimationChanges(dirty,graph);
  lighting->updateLightingData();lighting->uploadLightingData();const auto before=read();EXPECT_EQ(floats(before[6]).w,8);
  player->setSpeed(.625);player->setLoop(false);player->play();LightEditPatch patch;patch.fields=LightEditFieldBit(LightEditField::Intensity);patch.values.intensity=20;
  auto stage=lighting->prepareLightEdit(selected(EditableLightType::Point).target,patch);const auto command=stage.command();ASSERT_TRUE(publish(stage));
  auto expected=before;expected[6].w=std::bit_cast<uint32_t>(20.f);everyFrame(expected);
  dirty=player->seek(manager->animationSource(),1.5);manager->publishAnimationChanges(dirty,graph);lighting->updateLightingData();lighting->uploadLightingData();
  everyFrame(expected);const auto materialNow=manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor;
  const auto restLight=player->library().restLights[0];const auto restMaterial=player->library().restMaterials[0];
  auto undo=lighting->prepareLightEditReplay(command,false);ASSERT_TRUE(publish(undo));auto atCurrentClock=before;atCurrentClock[6].w=std::bit_cast<uint32_t>(10.f);everyFrame(atCurrentClock);
  auto redo=lighting->prepareLightEditReplay(command,true);ASSERT_TRUE(publish(redo));everyFrame(expected);
  EXPECT_DOUBLE_EQ(player->time(),1.5);EXPECT_DOUBLE_EQ(player->speed(),.625);EXPECT_TRUE(player->playing());EXPECT_FALSE(player->loop());
  EXPECT_DOUBLE_EQ(manager->animationSource().lights[0].intensity,10);EXPECT_DOUBLE_EQ(player->library().restLights[0].intensity,restLight.intensity);
  EXPECT_EQ(player->library().restMaterials[0],restMaterial);EXPECT_DOUBLE_EQ(manager->animationSource().materials[0].pbrMetallicRoughness.roughnessFactor,materialNow);
}

TEST_F(LightEditGenerationGpu, ManualAddOriginalBirthReplaysWithoutChangingOpaqueWorldContextOrPriorLights) {
  const auto before=read();const auto count=world.entityCount();auto& context=world.registry().ctx().emplace<std::string>(1024*1024,'A');const auto* data=context.data();
  // This fixture's only mesh is the unit right triangle: independent AABB
  // center (.5,.5,0), radius sqrt(.5), clamped to one by the Add contract.
  auto expected=before;expected[2].x=3;expected[5]=bits({.5f,.75f,0,1.25f});expected[6]=bits({1,.9f,.78f,6});
  expected[7]=bits({0,-1,0,1});expected[8]=bits({0,kLightTypePoint,0,0});
  auto stage=lighting->prepareLightAdd(EditableLightType::Point);const auto command=stage.command();const auto addedId=stage.selectedLight().id;
  ASSERT_EQ(addedId.source,EditableLightSource::Manual);ASSERT_TRUE(publish(stage));const auto added=read(0,2);
  everyFrame(expected,2);
  EXPECT_EQ(added[2].x,before[2].x+1);EXPECT_EQ(world.entityCount(),count+1);const auto incarnation=lighting->lightEditSnapshot(addedId).target.incarnation;
  EXPECT_GT(floats(added[6]).w,0);for(int row=0;row<5;++row) if(row!=2) EXPECT_EQ(added[size_t(row)],before[size_t(row)]);
  const auto prior=read();for(int row=5;row<14;++row) EXPECT_EQ(prior[size_t(row)],before[size_t(row)]);
  for(int cycle=0;cycle<2;++cycle) {
    auto undo=lighting->prepareLightEditReplay(command,false);ASSERT_TRUE(publish(undo));everyFrame(before);EXPECT_EQ(world.entityCount(),count);
    auto redo=lighting->prepareLightEditReplay(command,true);ASSERT_TRUE(publish(redo));everyFrame(added,2);EXPECT_EQ(world.entityCount(),count+1);
    EXPECT_EQ(lighting->lightEditSnapshot(addedId).target.incarnation,incarnation);
  }
  EXPECT_EQ(world.registry().ctx().find<std::string>(),&context);EXPECT_EQ(context.data(),data);
}
} // namespace
