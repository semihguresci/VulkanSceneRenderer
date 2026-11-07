#include "camera_generation_test_support.h"

namespace {
TEST(CameraProjectionRuntimeContract, ExtremeFiniteLensHasANonzeroUsableFloatUploadInverse) {
  tinygltf::Camera camera;camera.type="orthographic";camera.orthographic.xmag=camera.orthographic.ymag=1e-13;
  camera.orthographic.znear=0;camera.orthographic.zfar=1e-13;
  const auto view=EvaluateAnimatedCamera(camera,glm::mat4(1),1.f);ASSERT_TRUE(view);
  EXPECT_NE(view->data.inverseViewProj[0][0],0.f);EXPECT_NE(view->data.inverseViewProj[1][1],0.f);
  const auto product=glm::dmat4(view->data.viewProj)*glm::dmat4(view->data.inverseViewProj);
  for(int column=0;column<4;++column) for(int row=0;row<4;++row)
    EXPECT_NEAR(product[column][row],column==row?1.:0.,1e-5);
}

TEST_F(CameraProjectionGenerationGpu, PerspectivePublishesThreeSelectedRecordsAndGpuReverseZWithoutResourceChurn) {
  const auto selected=target();const auto other=manager->authoredCamera(1);const auto geometry=manager->geometryRevision(),material=manager->materialRevision();
  const auto resource=manager->materialResourceRevision(),epoch=manager->generationEpoch(),revision=manager->cameraRevision();
  const auto descriptor=manager->descriptorSet(0),auxiliary=manager->auxiliaryDescriptorSet(0);const auto cameraBuffer=cameras[0].buffer;
  const auto actualImages=manager->textureImageOwnership();const auto logical=manager->resourceLifecycleGeneration();const auto count=allocations();
  const auto* bytes=manager->animationSource().buffers[0].data.data();
  auto prepared=manager->prepareCameraProjectionEdit(selected,perspective(),graph,player.get(),2.f);const auto command=prepared.command();const auto view=prepared.candidateView();
  EXPECT_EQ(allocations(),count);ASSERT_TRUE(prepared.changed());EXPECT_FALSE(command.published());ASSERT_TRUE(publish(prepared));
  EXPECT_TRUE(command.published());EXPECT_TRUE(command.applied());EXPECT_EQ(manager->cameraRevision(),revision+1);
  for(const auto* camera:{&manager->authoredCamera(0),&std::as_const(manager->animationSource()).cameras[0],&player->library().restCameras[0]}) {
    EXPECT_DOUBLE_EQ(camera->perspective.yfov,.95);EXPECT_DOUBLE_EQ(camera->perspective.znear,.25);
    EXPECT_DOUBLE_EQ(camera->perspective.aspectRatio,0);EXPECT_DOUBLE_EQ(camera->perspective.zfar,0);
  }
  EXPECT_TRUE(GltfCameraRecordsEqual(manager->authoredCamera(1),other));EXPECT_GT(std::abs(view.view[0][1]),.1f);
  const auto world=glm::inverse(view.view);const auto nearPoint=world*glm::vec4(0,0,-.25f,1);const auto near=probe(nearPoint);
  EXPECT_NEAR(near[0].z/near[0].w,1.f,1e-4f);for(int axis=0;axis<4;++axis) EXPECT_NEAR(near[1][axis]/near[1].w,nearPoint[axis]/nearPoint.w,1e-4f);
  const float distance=3.f,tangent=std::tan(.95f/2);const auto edge=probe(world*glm::vec4(distance*tangent*2,distance*tangent,-distance,1));
  EXPECT_NEAR(edge[0].x/edge[0].w,1.f,1e-4f);EXPECT_NEAR(edge[0].y/edge[0].w,1.f,1e-4f);
  const auto far=probe(world*glm::vec4(0,0,-10000.f,1));EXPECT_NEAR(far[0].z/far[0].w,.25f/10000.f,1e-7f);
  EXPECT_EQ(allocations(),count);EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->auxiliaryDescriptorSet(0),auxiliary);EXPECT_EQ(cameras[0].buffer,cameraBuffer);
  EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(manager->materialRevision(),material);EXPECT_EQ(manager->materialResourceRevision(),resource);EXPECT_EQ(manager->generationEpoch(),epoch);
  EXPECT_EQ(manager->textureImageOwnership(),actualImages);EXPECT_EQ(manager->resourceLifecycleGeneration(),logical);EXPECT_EQ(manager->animationSource().buffers[0].data.data(),bytes);
}

TEST_F(CameraProjectionGenerationGpu, OrthographicUndoRedoAndUnrelatedCameraStateUseTheOriginalCommand) {
  const auto before=manager->authoredCamera(0);GltfCameraProjectionPatch patch;patch.type=GltfCameraProjection::Orthographic;
  patch.values={{Property::OrthographicXmag,4.},{Property::OrthographicYmag,2.},{Property::OrthographicNear,0.},{Property::OrthographicFar,40.}};
  auto initial=manager->prepareCameraProjectionEdit(target(),patch,graph,player.get(),2.f);const auto command=initial.command();const auto view=initial.candidateView();
  ASSERT_TRUE(publish(initial));const auto world=glm::inverse(view.view);const auto near=probe(world*glm::vec4(4,2,0,1));
  EXPECT_NEAR(near[0].x,1,1e-5);EXPECT_NEAR(near[0].y,1,1e-5);EXPECT_NEAR(near[0].z,1,1e-5);
  const auto far=probe(world*glm::vec4(0,0,-40,1));EXPECT_NEAR(far[0].z,0,1e-5);
  const auto unchanged=manager->animationSource().cameras[1];const auto otherRest=player->library().restCameras[1];
  for(int cycle=0;cycle<2;++cycle) {
    SCOPED_TRACE(cycle);auto undo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);
    ASSERT_TRUE(publish(undo));EXPECT_FALSE(command.applied());EXPECT_TRUE(GltfCameraRecordsEqual(manager->authoredCamera(0),before));
    auto redo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::After,graph,player.get(),2.f);
    ASSERT_TRUE(publish(redo));EXPECT_TRUE(command.applied());EXPECT_EQ(manager->authoredCamera(0).type,"orthographic");
    EXPECT_TRUE(GltfCameraRecordsEqual(manager->animationSource().cameras[1],unchanged));EXPECT_TRUE(GltfCameraRecordsEqual(player->library().restCameras[1],otherRest));
  }
  EXPECT_EQ(target(2).incarnation,command.target().incarnation);EXPECT_EQ(target(2).sourceNamespace,command.target().sourceNamespace);
}

TEST_F(CameraProjectionGenerationGpu, OriginalCommandPreservesUnownedLensLeavesAndLaterOtherDefinitionEdits) {
  GltfCameraProjectionPatch fov;fov.values={{Property::PerspectiveYfov,1.1}};
  auto first=manager->prepareCameraProjectionEdit(target(),fov,graph,player.get(),2.f);const auto command=first.command();ASSERT_TRUE(publish(first));
  GltfCameraProjectionPatch near;near.values={{Property::PerspectiveNear,.4}};
  auto later=manager->prepareCameraProjectionEdit(target(),near,graph,player.get(),2.f);ASSERT_TRUE(publish(later));
  graph.clearActiveRoots();graph.updateWorldTransforms();
  auto other=manager->prepareCameraProjectionEdit(target(3),perspective(.7,.2),graph,player.get(),2.f);ASSERT_TRUE(publish(other));
  const auto otherA=manager->authoredCamera(1),otherE=manager->animationSource().cameras[1],otherRest=player->library().restCameras[1];
  auto undo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);ASSERT_TRUE(publish(undo));
  EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.yfov,.8);EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.znear,.4);
  auto redo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::After,graph,player.get(),2.f);ASSERT_TRUE(publish(redo));
  EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.yfov,1.1);EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.znear,.4);
  EXPECT_TRUE(GltfCameraRecordsEqual(manager->authoredCamera(1),otherA));EXPECT_TRUE(GltfCameraRecordsEqual(manager->animationSource().cameras[1],otherE));
  EXPECT_TRUE(GltfCameraRecordsEqual(player->library().restCameras[1],otherRest));
}

TEST_F(CameraProjectionGenerationGpu, AnimatedOtherLensLeavesKeepCurrentClockAndEncodedProofWithoutChangingAuthoredRevisionOnSeek) {
  auto source=cameraAsset();source.extensionsUsed.push_back("KHR_animation_pointer");
  const std::array<float,4> values{0,2,.8f,1.2f};source.buffers.emplace_back();source.buffers.back().data.resize(sizeof(values));
  std::memcpy(source.buffers.back().data.data(),values.data(),sizeof(values));
  const auto baseView=int(source.bufferViews.size()),baseAccessor=int(source.accessors.size());
  for(int row=0;row<2;++row) {
    tinygltf::BufferView view;view.buffer=1;view.byteOffset=size_t(row)*2*sizeof(float);view.byteLength=2*sizeof(float);source.bufferViews.push_back(view);
    tinygltf::Accessor accessor;accessor.bufferView=baseView+row;accessor.count=2;accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;accessor.type=TINYGLTF_TYPE_SCALAR;
    accessor.minValues={values[size_t(row)*2]};accessor.maxValues={values[size_t(row)*2+1]};source.accessors.push_back(accessor);
  }
  source.animations.emplace_back();tinygltf::AnimationSampler sampler;sampler.input=baseAccessor;sampler.output=baseAccessor+1;
  source.animations[0].samplers.push_back(sampler);tinygltf::AnimationChannel channel;channel.target_path="pointer";channel.target_node=-1;channel.sampler=0;
  channel.target_extensions["KHR_animation_pointer"]=tinygltf::Value(tinygltf::Value::Object{{"pointer",tinygltf::Value(std::string("/cameras/0/perspective/yfov"))}});
  source.animations[0].channels.push_back(channel);load(std::move(source));
  const auto authoredRevision=manager->cameraRevision();auto dirty=player->selectClip(manager->animationSource(),0);
  dirty.merge(player->seek(manager->animationSource(),.5));manager->publishAnimationChanges(dirty,graph);
  EXPECT_EQ(manager->cameraRevision(),authoredRevision);player->setSpeed(.625);player->setLoop(false);player->play();
  GltfCameraProjectionPatch near;near.values={{Property::PerspectiveNear,.3}};
  auto prepared=manager->prepareCameraProjectionEdit(target(),near,graph,player.get(),2.f);const auto command=prepared.command();
  const auto& accessor=manager->animationSource().accessors[size_t(baseAccessor+1)];const auto& view=manager->animationSource().bufferViews[size_t(accessor.bufferView)];
  auto* encoded=manager->animationSource().buffers[size_t(view.buffer)].data.data()+view.byteOffset+accessor.byteOffset;
  float original{};std::memcpy(&original,encoded,sizeof(original));const auto changed=std::nextafter(original,1.f);std::memcpy(encoded,&changed,sizeof(changed));
  bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeCameraProjectionEdit(prepared,graph,player.get(),2.f);
    published=manager->exchangeCameraProjectionEdit(prepared,graph,player.get(),2.f);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.znear,.1);
  std::memcpy(encoded,&original,sizeof(original));ASSERT_TRUE(publish(prepared));const auto editedRevision=manager->cameraRevision();
  auto staleClone=manager->prepareSubtreeCloneGeneration(graph,node(1),"Stale camera animation payload");
  dirty=player->seek(manager->animationSource(),1.5);manager->publishAnimationChanges(dirty,graph);EXPECT_EQ(manager->cameraRevision(),editedRevision);
  {material_publication_test::AllocationGuard deny(true);ready=staleClone.resourceDependenciesReadyForExchange(*manager);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_EQ(attempts,0u);
  const auto currentFov=manager->animationSource().cameras[0].perspective.yfov;
  auto undo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);ASSERT_TRUE(publish(undo));
  EXPECT_DOUBLE_EQ(manager->animationSource().cameras[0].perspective.yfov,currentFov);EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.yfov,.8);
  EXPECT_DOUBLE_EQ(player->library().restCameras[0].perspective.znear,.1);EXPECT_DOUBLE_EQ(player->time(),1.5);
  auto redo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::After,graph,player.get(),2.f);ASSERT_TRUE(publish(redo));
  EXPECT_DOUBLE_EQ(manager->animationSource().cameras[0].perspective.yfov,currentFov);EXPECT_DOUBLE_EQ(player->library().restCameras[0].perspective.znear,.3);
  EXPECT_DOUBLE_EQ(player->speed(),.625);EXPECT_TRUE(player->playing());EXPECT_FALSE(player->loop());
  const auto curveBytes=manager->animationSource().buffers[1].data;
  GltfCameraProjectionPatch fov;fov.values={{Property::PerspectiveYfov,.7}};
  auto animated=manager->prepareCameraProjectionEdit(target(),fov,graph,player.get(),2.f);
  const auto animatedCommand=animated.command();const auto animatedView=animated.candidateView();
  ASSERT_TRUE(publish(animated));
  EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.yfov,.7);
  EXPECT_DOUBLE_EQ(player->library().restCameras[0].perspective.yfov,.7);
  EXPECT_DOUBLE_EQ(manager->animationSource().cameras[0].perspective.yfov,currentFov);
  const auto world=glm::inverse(animatedView.view);
  const auto edge=probe(world*glm::vec4(0,3.f*std::tan(float(currentFov)/2.f),-3,1));
  EXPECT_NEAR(edge[0].y/edge[0].w,1.f,1e-4f);
  auto animatedUndo=manager->prepareCameraProjectionReplay(animatedCommand,GltfCameraEditSide::Before,graph,player.get(),2.f);
  ASSERT_TRUE(publish(animatedUndo));
  EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.yfov,.8);
  EXPECT_DOUBLE_EQ(manager->animationSource().cameras[0].perspective.yfov,currentFov);
  auto animatedRedo=manager->prepareCameraProjectionReplay(animatedCommand,GltfCameraEditSide::After,graph,player.get(),2.f);
  ASSERT_TRUE(publish(animatedRedo));
  dirty=player->clearClip(manager->animationSource());manager->publishAnimationChanges(dirty,graph);
  EXPECT_DOUBLE_EQ(manager->animationSource().cameras[0].perspective.yfov,.7);
  EXPECT_EQ(manager->animationSource().buffers[1].data,curveBytes);
}

TEST_F(CameraProjectionGenerationGpu, StaleRawSelectedGraphPlayerViewportAndSourceRejectBothFinalGuardsWithoutAllocation) {
  auto prepared=manager->prepareCameraProjectionEdit(target(),perspective(),graph,player.get(),2.f);const auto command=prepared.command();
  const auto before=manager->authoredCamera(0),evaluated=manager->animationSource().cameras[0],rest=player->library().restCameras[0];
  const auto revision=manager->cameraRevision(),epoch=manager->generationEpoch();const auto descriptor=manager->descriptorSet(0);
  const auto reject=[&](const AnimationPlayer* actual,float aspect=2.f) {
    bool ready{},published{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeCameraProjectionEdit(prepared,graph,actual,aspect);
      published=manager->exchangeCameraProjectionEdit(prepared,graph,const_cast<AnimationPlayer*>(actual),aspect);attempts=deny.allocations();}
    EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(prepared);EXPECT_FALSE(command.published());
    EXPECT_EQ(manager->cameraRevision(),revision);EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->descriptorSet(0),descriptor);
    EXPECT_TRUE(GltfCameraRecordsEqual(manager->authoredCamera(0),before));EXPECT_TRUE(GltfCameraRecordsEqual(player->library().restCameras[0],rest));
  };
  reject(nullptr);reject(player.get(),1.5f);AnimationPlayer replacement(manager->animationSource());reject(&replacement);
  auto* binding=graph.getNode(target().graphNode);const auto original=binding->worldTransform;binding->worldTransform[3][0]+=1;
  reject(player.get());binding->worldTransform=original;
  manager->animationSource().cameras[0].name=std::string(512,'S');reject(player.get());manager->animationSource().cameras[0]=evaluated;
  ASSERT_TRUE(publish(prepared));
}

TEST_F(CameraProjectionGenerationGpu, DeniedPreparationAndFiniteBudgetsPreserveAnEarlierReadyEditAndHistory) {
  auto ready=manager->prepareCameraProjectionEdit(target(),perspective(),graph,player.get(),2.f);const auto revision=manager->cameraRevision();
  {auto warm=manager->prepareCameraProjectionEdit(target(),perspective(1.1),graph,player.get(),2.f);}
  size_t count{};
  {material_publication_test::AllocationGuard measure(false);auto candidate=manager->prepareCameraProjectionEdit(target(),perspective(1.1),graph,player.get(),2.f);
    measure.disable();count=measure.allocations();}
  ASSERT_GT(count,0u);
  for(const size_t allowed:std::array<size_t,3>{0,count/2,count-1}) {
    SCOPED_TRACE(allowed);bool failed{};
    {material_publication_test::AllocationGuard deny(false,allowed);
      try {(void)manager->prepareCameraProjectionEdit(target(),perspective(1.1),graph,player.get(),2.f);} catch(const std::bad_alloc&) {failed=true;}}
    EXPECT_TRUE(failed);EXPECT_EQ(manager->cameraRevision(),revision);EXPECT_TRUE(manager->canExchangeCameraProjectionEdit(ready,graph,player.get(),2.f));
  }
  CameraProjectionLimits limits;limits.maxPreparedCpuBytes=1;
  EXPECT_THROW((void)manager->prepareCameraProjectionEdit(target(),perspective(),graph,player.get(),2.f,limits),std::invalid_argument);
  ASSERT_TRUE(publish(ready));
}

TEST_F(CameraProjectionGenerationGpu, EmptyAuthoredMaterialsNoopAndBothPortableSavePathsKeepTheEditedLens) {
  load(cameraAsset(false));ASSERT_TRUE(manager->sourceMaterialRuntimeMapping().empty());
  auto initial=manager->prepareCameraProjectionEdit(target(),perspective(),graph,player.get(),2.f);ASSERT_TRUE(publish(initial));
  const auto revision=manager->cameraRevision();auto noop=manager->prepareCameraProjectionEdit(target(),{},graph,player.get(),1.25f);
  EXPECT_FALSE(noop.changed());ASSERT_TRUE(publish(noop,1.25f));EXPECT_EQ(manager->cameraRevision(),revision);
  for(const auto* extension:{".glb",".gltf"}) {
    const auto path=temp/(std::string("saved")+extension);manager->saveScene(graph,path);const auto reloaded=LoadSourceModel(path.string());
    ASSERT_EQ(reloaded.cameras.size(),2u);EXPECT_DOUBLE_EQ(reloaded.cameras[0].perspective.yfov,.95);
    EXPECT_DOUBLE_EQ(reloaded.cameras[0].perspective.aspectRatio,0);EXPECT_DOUBLE_EQ(reloaded.cameras[0].perspective.zfar,0);
    EXPECT_DOUBLE_EQ(reloaded.cameras[0].perspective.znear,.25);EXPECT_TRUE(GltfCameraRecordsEqual(reloaded.cameras[1],manager->authoredCamera(1)));
    EXPECT_EQ(std::ranges::count_if(reloaded.nodes,[](const auto& node){return node.camera==0;}),2);
  }
}

TEST_F(CameraProjectionGenerationGpu, CloneNamespaceAndFreshMergeForkGuardOldHistoryAndPreparedViews) {
  auto initial=manager->prepareCameraProjectionEdit(target(),perspective(),graph,player.get(),2.f);const auto command=initial.command();ASSERT_TRUE(publish(initial));
  const auto original=target();auto stale=manager->prepareCameraProjectionEdit(target(),perspective(1.1),graph,player.get(),2.f);
  const auto exchange=[&](SceneManager::PreparedMergeGeneration& receipt) {
    receipt.prepareResourceDependencies(*manager);bool ready{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=receipt.resourceDependenciesReadyForExchange(*manager);if(ready) manager->exchangeMergeGeneration(receipt);attempts=deny.allocations();}
    receipt.releaseResourceDependencyPreparation();return ready && attempts==0;
  };
  auto clone=manager->prepareSubtreeCloneGeneration(graph,node(1),"Camera instance");ASSERT_TRUE(exchange(clone));
  EXPECT_EQ(target().sourceNamespace,original.sourceNamespace);EXPECT_EQ(target().incarnation,original.incarnation);
  ASSERT_TRUE(exchange(clone));EXPECT_FALSE(manager->canExchangeCameraProjectionEdit(stale,graph,player.get(),2.f));
  auto undo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);ASSERT_TRUE(publish(undo));
  auto redo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::After,graph,player.get(),2.f);ASSERT_TRUE(publish(redo));
  auto incoming=cameraAsset();const auto path=temp/"incoming.glb";tinygltf::TinyGLTF writer;
  ASSERT_TRUE(writer.WriteGltfSceneToFile(&incoming,path.string(),true,true,true,true));
  const auto loaded=LoadModelWithSource(path.string());auto merge=manager->prepareMergeGeneration(loaded,path,{},graph);ASSERT_TRUE(exchange(merge));
  EXPECT_NE(target().sourceNamespace,original.sourceNamespace);EXPECT_NE(target().incarnation,original.incarnation);
  AnimationPlayer current(manager->animationSource());
  EXPECT_THROW((void)manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::Before,graph,&current,2.f),std::invalid_argument);
  ASSERT_TRUE(exchange(merge));EXPECT_EQ(target().sourceNamespace,original.sourceNamespace);
  auto originalUndo=manager->prepareCameraProjectionReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);ASSERT_TRUE(publish(originalUndo));
}
} // namespace
