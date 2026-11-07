#include "camera_generation_test_support.h"

#include <Container/geometry/GltfSceneExport.h>
#include <glm/gtc/matrix_transform.hpp>

#include <functional>
#include <limits>

namespace {
void poseMatrixNear(const glm::mat4& actual,const glm::mat4& expected,float epsilon=2e-5f) {
  for(int column=0;column<4;++column) for(int row=0;row<4;++row) EXPECT_NEAR(actual[column][row],expected[column][row],epsilon);
}
void poseBoundsEqual(const ModelBounds& actual,const ModelBounds& expected) {
  EXPECT_EQ(actual.valid,expected.valid);EXPECT_FLOAT_EQ(actual.radius,expected.radius);
  for(int i=0;i<3;++i) {
    EXPECT_FLOAT_EQ(actual.min[i],expected.min[i]);EXPECT_FLOAT_EQ(actual.max[i],expected.max[i]);
    EXPECT_FLOAT_EQ(actual.center[i],expected.center[i]);EXPECT_FLOAT_EQ(actual.size[i],expected.size[i]);
  }
}
int poseFloats(tinygltf::Model& source,std::initializer_list<float> values,int type,size_t count) {
  tinygltf::Buffer buffer;buffer.data.resize(values.size()*sizeof(float));std::memcpy(buffer.data.data(),values.begin(),buffer.data.size());
  tinygltf::BufferView view;view.buffer=int(source.buffers.size());view.byteLength=buffer.data.size();
  tinygltf::Accessor accessor;accessor.sparse={};accessor.bufferView=int(source.bufferViews.size());
  accessor.type=type;accessor.count=count;accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  if(type==TINYGLTF_TYPE_SCALAR) {const auto [low,high]=std::minmax_element(values.begin(),values.end());accessor.minValues={*low};accessor.maxValues={*high};}
  const auto result=int(source.accessors.size());source.buffers.push_back(buffer);source.bufferViews.push_back(view);source.accessors.push_back(accessor);return result;
}
void poseTrack(tinygltf::Model& source,int node,const char* property,int type,
    std::initializer_list<float> values,bool pointer=false) {
  if(source.animations.empty()) source.animations.emplace_back();
  tinygltf::AnimationSampler sampler;sampler.input=poseFloats(source,{0,2},TINYGLTF_TYPE_SCALAR,2);
  sampler.output=poseFloats(source,values,type,2);sampler.interpolation="LINEAR";
  auto& clip=source.animations[0];tinygltf::AnimationChannel channel;channel.sampler=int(clip.samplers.size());
  channel.target_node=node;channel.target_path=property;
  if(pointer) {
    channel.target_node=-1;channel.target_path="pointer";
    channel.target_extensions["KHR_animation_pointer"]=tinygltf::Value(tinygltf::Value::Object{
        {"pointer",tinygltf::Value(std::string("/nodes/")+std::to_string(node)+"/"+property)}});
    if(std::ranges::find(source.extensionsUsed,"KHR_animation_pointer")==source.extensionsUsed.end())
      source.extensionsUsed.push_back("KHR_animation_pointer");
  }
  clip.samplers.push_back(sampler);clip.channels.push_back(channel);
}
tinygltf::Model poseLightAsset() {
  auto source=cameraAsset();source.nodes[1].mesh=0;source.nodes[1].light=0;source.nodes[1].children={5};
  source.nodes.emplace_back();source.nodes[5].translation={0,1,0};source.nodes[5].mesh=0;source.nodes[5].light=0;
  source.lights.emplace_back();source.lights[0].type="point";source.lights[0].intensity=3.;source.lights[0].range=5.;
  source.lights[0].extras=tinygltf::Value(tinygltf::Value::Object{{"rawLight",tinygltf::Value(std::string(96,'l'))}});
  source.extensionsUsed={"KHR_lights_punctual"};return source;
}
class CameraViewPoseGenerationGpu:public CameraProjectionGenerationGpu {
 protected:
  const char* gpuEnvironment() const noexcept override {return "CONTAINER_RUN_GPU_CAMERA_POSES";}
  void publishAnimation(const AnimationDirtySet& dirty) {
    ApplyAnimatedNodeTransforms(manager->animationSource(),dirty,graph);
    manager->publishAnimationChanges(dirty,graph);
  }
  CameraViewPoseTarget poseTarget(int source=1) const {
    const auto value=manager->cameraViewPoseTarget(graph,node(source));if(!value) throw std::runtime_error("Missing pose fixture target");return *value;
  }
  glm::mat4 desired(float offset=0) const {
    return glm::lookAt(glm::vec3(5+offset,-1,10),glm::vec3(4+offset,0,6),glm::normalize(glm::vec3(.25f,1,.1f)));
  }
  bool publishPose(SceneManager::PreparedCameraViewPoseEdit& prepared,const glm::mat4* currentView=nullptr,float aspect=2.f) {
    const auto view=prepared.candidateView();bool ready{},published{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);
      ready=manager->canExchangeCameraViewPoseEdit(prepared,graph,player.get(),aspect,currentView);
      published=manager->exchangeCameraViewPoseEdit(prepared,graph,player.get(),aspect,currentView);attempts=deny.allocations();}
    if(!ready || !published || prepared || attempts) return false;
    for(auto& camera:cameras) {
      std::memcpy(camera.allocation_info.pMappedData,&view.data,sizeof(view.data));
      if(vmaFlushAllocation(allocation->memoryManager()->allocator(),camera.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS) throw std::runtime_error("Cannot flush pose probe camera");
    }
    return true;
  }
  // Reorder this unedited fixture once so its selected source-bearing leaf is
  // an actual graph tail. New private births are minted by ordinary createNode;
  // no receipt can mint, overwrite or forge a node lifetime.
  void makeSelectedLeafLast() {
    const auto selected=node(1);std::vector<SceneNode> original;
    for(uint32_t i=0;i<graph.nodeCount();++i) original.push_back(*graph.getNode(i));
    if(!original[selected].children.empty()) throw std::runtime_error("Pose tail fixture requires a leaf");
    const auto roots=graph.rootNodes();const auto active=graph.activeRoots();
    SceneGraph replacement;std::vector<uint32_t> mapping(original.size(),SceneGraph::kInvalidNode);
    const auto create=[&](uint32_t old) {
      const auto& value=original[old];const auto id=replacement.createNode(value.localTransform,value.materialIndex,value.renderable,value.primitiveIndex,value.name);
      mapping[old]=id;auto& next=*replacement.getNode(id);next=value;next.parent=SceneGraph::kInvalidNode;next.children.clear();next.meshOwnerGraphNode=SceneGraph::kInvalidNode;
    };
    for(uint32_t i=0;i<original.size();++i) if(i!=selected) create(i);create(selected);
    for(uint32_t i=0;i<original.size();++i) {
      if(original[i].meshOwnerGraphNode!=SceneGraph::kInvalidNode)
        replacement.getNode(mapping[i])->meshOwnerGraphNode=mapping[original[i].meshOwnerGraphNode];
      if(original[i].parent!=SceneGraph::kInvalidNode && !replacement.setParent(mapping[i],mapping[original[i].parent]))
        throw std::runtime_error("Cannot rebuild pose tail parent");
    }
    for(size_t i=0;i<roots.size();++i) if(!replacement.setSiblingIndex(mapping[roots[i]],i)) throw std::runtime_error("Cannot rebuild pose tail roots");
    for(uint32_t i=0;i<original.size();++i) for(size_t child=0;child<original[i].children.size();++child)
      if(!replacement.setSiblingIndex(mapping[original[i].children[child]],child)) throw std::runtime_error("Cannot rebuild pose tail sibling");
    if(active) {std::vector<uint32_t> mapped;for(const auto root:*active) mapped.push_back(mapping[root]);replacement.setActiveRoots(mapped);}
    replacement.updateWorldTransforms();graph=std::move(replacement);
  }
};

TEST_F(CameraViewPoseGenerationGpu, SelectedPosePublishesSourceRestAndGraphWithActualGpuRollAndNoResourceChurn) {
  const auto target=poseTarget();const auto oldSibling=*graph.getNode(node(2));const auto lens=manager->authoredCamera(0);
  const auto geometry=manager->geometryRevision(),material=manager->materialRevision(),resources=manager->materialResourceRevision();
  const auto epoch=manager->generationEpoch(),revision=manager->cameraRevision();const auto descriptor=manager->descriptorSet(0);
  const auto auxiliary=manager->auxiliaryDescriptorSet(0);const auto buffer=cameras[0].buffer;const auto count=allocations();
  const auto view=desired();auto prepared=manager->prepareCameraViewPoseEdit(target,view,graph,player.get(),2.f);
  const auto candidate=prepared.candidateView();const auto command=prepared.command();ASSERT_TRUE(prepared.changed());
  EXPECT_LT(command.ownedBytes(),prepared.ownedBytes());EXPECT_LT(command.ownedBytes(),1024*1024u);
  poseMatrixNear(candidate.view,view);ASSERT_TRUE(publishPose(prepared,&view));
  EXPECT_TRUE(command.published());EXPECT_TRUE(command.applied());EXPECT_EQ(manager->cameraRevision(),revision+1);
  const auto& a=manager->authoredCameraSourceView().nodes[1];const auto& e=manager->animationSource().nodes[1];
  EXPECT_TRUE(GltfCameraNodePoseEqual(a,e));EXPECT_TRUE(GltfCameraNodePoseEqual(a,player->library().restNodes[1]));
  ASSERT_EQ(a.matrix.size(),16u);EXPECT_TRUE(a.translation.empty());EXPECT_TRUE(a.rotation.empty());EXPECT_TRUE(a.scale.empty());
  EXPECT_EQ(*graph.getNode(node(2)),oldSibling);EXPECT_TRUE(GltfCameraRecordsEqual(manager->authoredCamera(0),lens));
  EXPECT_EQ(manager->geometryRevision(),geometry);EXPECT_EQ(manager->materialRevision(),material);EXPECT_EQ(manager->materialResourceRevision(),resources);
  EXPECT_EQ(manager->generationEpoch(),epoch);EXPECT_EQ(manager->descriptorSet(0),descriptor);EXPECT_EQ(manager->auxiliaryDescriptorSet(0),auxiliary);
  EXPECT_EQ(cameras[0].buffer,buffer);EXPECT_EQ(allocations(),count);
  const glm::vec4 point(1,2,0,1);const auto sampled=probe(point);const auto expected=candidate.data.viewProj*point;
  for(int i=0;i<4;++i) {EXPECT_NEAR(sampled[0][i],expected[i],1e-5f);EXPECT_NEAR(sampled[1][i],point[i],2e-4f);}
}

TEST_F(CameraViewPoseGenerationGpu, OriginalHistoryUsesCurrentAnimatedParentAndLensWithoutRewindingClockOrSiblingPose) {
  auto source=cameraAsset();const auto time=poseFloats(source,{0,2},TINYGLTF_TYPE_SCALAR,2);
  const auto positions=poseFloats(source,{1,2,3,3,4,5},TINYGLTF_TYPE_VEC3,2);
  source.animations.emplace_back();tinygltf::AnimationSampler sampler;sampler.input=time;sampler.output=positions;
  source.animations[0].samplers.push_back(sampler);tinygltf::AnimationChannel raw;raw.sampler=0;raw.target_node=4;raw.target_path="translation";
  source.animations[0].channels.push_back(raw);load(std::move(source));
  auto dirty=player->selectClip(manager->animationSource(),0);dirty.merge(player->seek(manager->animationSource(),.5));publishAnimation(dirty);
  player->setSpeed(.625);player->setLoop(false);player->play();const auto originalLocal=graph.getNode(node(1))->localTransform;
  const auto view=desired();auto initial=manager->prepareCameraViewPoseEdit(poseTarget(),view,graph,player.get(),2.f);
  const auto command=initial.command();ASSERT_TRUE(publishPose(initial,&view));
  GltfCameraProjectionPatch patch;patch.values={{Property::PerspectiveNear,.35}};
  auto lensEdit=manager->prepareCameraProjectionEdit(target(),patch,graph,player.get(),2.f);ASSERT_TRUE(publish(lensEdit));
  dirty=player->seek(manager->animationSource(),1.5);publishAnimation(dirty);
  const auto parentWorld=graph.getNode(node(4))->worldTransform;const auto sibling=*graph.getNode(node(2));const auto timeNow=player->time();
  auto undo=manager->prepareCameraViewPoseReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);
  const auto expected=EvaluateAnimatedCamera(manager->animationSource().cameras[0],parentWorld*originalLocal,2.f);ASSERT_TRUE(expected);
  poseMatrixNear(undo.candidateView().view,expected->view);ASSERT_TRUE(publishPose(undo));
  EXPECT_DOUBLE_EQ(player->time(),timeNow);EXPECT_DOUBLE_EQ(player->speed(),.625);EXPECT_TRUE(player->playing());EXPECT_FALSE(player->loop());
  EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.znear,.35);EXPECT_EQ(*graph.getNode(node(2)),sibling);
  poseMatrixNear(graph.getNode(node(1))->localTransform,originalLocal,0.f);
  dirty=player->seek(manager->animationSource(),.75);publishAnimation(dirty);
  auto redo=manager->prepareCameraViewPoseReplay(command,GltfCameraEditSide::After,graph,player.get(),2.f);ASSERT_TRUE(publishPose(redo));
  EXPECT_DOUBLE_EQ(player->time(),.75);EXPECT_DOUBLE_EQ(manager->authoredCamera(0).perspective.znear,.35);EXPECT_TRUE(command.applied());
}

TEST_F(CameraViewPoseGenerationGpu, FullyAnimatedSampleEqualityStillAuthorsDefaultsAndReplaysAtCurrentClock) {
  auto source=cameraAsset();source.nodes[1].name="Animated pose destination";
  poseTrack(source,1,"translation",TINYGLTF_TYPE_VEC3,{0,0,6,0,0,8});
  poseTrack(source,1,"rotation",TINYGLTF_TYPE_VEC4,{0,0,0,1,0,0,std::sin(.2f),std::cos(.2f)});
  poseTrack(source,1,"scale",TINYGLTF_TYPE_VEC3,{1,1,1,2,2,2});
  tinygltf::Animation other;other.name="Material-only clip";tinygltf::AnimationSampler materialSampler;
  materialSampler.input=poseFloats(source,{0,2},TINYGLTF_TYPE_SCALAR,2);
  materialSampler.output=poseFloats(source,{.2f,.8f},TINYGLTF_TYPE_SCALAR,2);materialSampler.interpolation="LINEAR";
  tinygltf::AnimationChannel materialChannel;materialChannel.sampler=0;materialChannel.target_path="pointer";
  materialChannel.target_extensions["KHR_animation_pointer"]=tinygltf::Value(tinygltf::Value::Object{
      {"pointer",tinygltf::Value(std::string("/materials/0/pbrMetallicRoughness/roughnessFactor"))}});
  other.samplers.push_back(materialSampler);other.channels.push_back(materialChannel);source.animations.push_back(other);
  source.extensionsUsed.push_back("KHR_animation_pointer");const auto originalCurves=ParseAnimations(source).clips;load(std::move(source));
  auto dirty=player->selectClip(manager->animationSource(),0);
  dirty.merge(player->seek(manager->animationSource(),.5));publishAnimation(dirty);
  player->setSpeed(.625);player->setLoop(false);player->play();
  const auto original=manager->authoredCameraSourceView().nodes[1];
  const auto originalRest=player->library().restNodes[1];const auto sampled=manager->animationSource().nodes[1];
  const auto sampledLocal=graph.getNode(node(1))->localTransform;
  const auto current=EvaluateAnimatedCamera(manager->animationSource().cameras[0],graph.getNode(node(1))->worldTransform,2.f);ASSERT_TRUE(current);
  const auto view=current->view;const auto revision=manager->cameraRevision();const auto allocationsBefore=allocations();
  auto edit=manager->prepareCameraViewPoseEdit(poseTarget(),view,graph,player.get(),2.f);
  const auto command=edit.command();ASSERT_TRUE(edit.changed());
  poseMatrixNear(edit.candidateGraph().getNode(node(1))->localTransform,sampledLocal,0.f);
  poseMatrixNear(edit.candidateView().view,current->view,0.f);ASSERT_TRUE(publishPose(edit,&view));
  EXPECT_TRUE(command.applied());EXPECT_EQ(manager->cameraRevision(),revision+1);EXPECT_EQ(allocations(),allocationsBefore);
  const auto edited=manager->authoredCameraSourceView().nodes[1];
  EXPECT_TRUE(edited.matrix.empty());EXPECT_FALSE(GltfCameraNodePoseEqual(edited,original));
  EXPECT_TRUE(GltfCameraNodePoseEqual(edited,player->library().restNodes[1]));
  EXPECT_TRUE(GltfCameraNodePoseEqual(sampled,manager->animationSource().nodes[1]));
  poseMatrixNear(graph.getNode(node(1))->localTransform,sampledLocal,0.f);
  dirty=player->seek(manager->animationSource(),1.5);publishAnimation(dirty);
  const auto later=manager->animationSource().nodes[1];const auto laterLocal=graph.getNode(node(1))->localTransform;
  auto undo=manager->prepareCameraViewPoseReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);
  ASSERT_TRUE(publishPose(undo));EXPECT_TRUE(GltfCameraNodePoseEqual(manager->authoredCameraSourceView().nodes[1],original));
  EXPECT_TRUE(GltfCameraNodePoseEqual(player->library().restNodes[1],originalRest));
  EXPECT_TRUE(GltfCameraNodePoseEqual(manager->animationSource().nodes[1],later));
  poseMatrixNear(graph.getNode(node(1))->localTransform,laterLocal,0.f);EXPECT_DOUBLE_EQ(player->time(),1.5);
  dirty=player->seek(manager->animationSource(),.75);publishAnimation(dirty);
  const auto redoSample=manager->animationSource().nodes[1];
  auto redo=manager->prepareCameraViewPoseReplay(command,GltfCameraEditSide::After,graph,player.get(),2.f);
  ASSERT_TRUE(publishPose(redo));EXPECT_TRUE(GltfCameraNodePoseEqual(manager->authoredCameraSourceView().nodes[1],edited));
  EXPECT_TRUE(GltfCameraNodePoseEqual(manager->animationSource().nodes[1],redoSample));
  EXPECT_DOUBLE_EQ(player->time(),.75);EXPECT_DOUBLE_EQ(player->speed(),.625);EXPECT_TRUE(player->playing());EXPECT_FALSE(player->loop());
  const auto checkSaved=[&](const char* phase,const char* extension,const tinygltf::Node& fileDefault) {
    const auto path=temp/(std::string("animated-pose-")+phase+extension);manager->saveScene(graph,path);
    auto loaded=LoadSourceModel(path.string());const auto found=std::ranges::find_if(loaded.nodes,[](const auto& row){return row.name=="Animated pose destination";});
    ASSERT_NE(found,loaded.nodes.end());const auto selected=size_t(found-loaded.nodes.begin());
    poseMatrixNear(EvaluateAnimatedNodeLocalTransform(loaded.nodes[selected]),EvaluateAnimatedNodeLocalTransform(fileDefault),0.f);
    EXPECT_TRUE(loaded.nodes[selected].matrix.empty());AnimationPlayer roundtrip(loaded);const auto& clips=roundtrip.library().clips;
    ASSERT_TRUE(roundtrip.library().diagnostics.empty());ASSERT_EQ(clips.size(),2u);ASSERT_EQ(clips[0].tracks.size(),4u);ASSERT_EQ(clips[1].tracks.size(),4u);
    for(size_t i=0;i<3;++i) {
      auto expected=originalCurves[0].tracks[i];expected.target.index=selected;EXPECT_EQ(clips[0].tracks[i],expected);
    }
    auto material=originalCurves[1].tracks[0];material.target.index=clips[1].tracks[0].target.index;EXPECT_EQ(clips[1].tracks[0],material);
    const auto& materialRest=clips[0].tracks[3];EXPECT_EQ(materialRest.target,material.target);
    EXPECT_EQ(materialRest.interpolation,AnimationInterpolation::Step);EXPECT_EQ(materialRest.times,(std::vector<double>{0}));EXPECT_EQ(materialRest.values,(std::vector<double>{1}));
    for(size_t i=1;i<4;++i) {
      const auto& restore=clips[1].tracks[i];EXPECT_EQ(restore.target.index,selected);EXPECT_EQ(restore.interpolation,AnimationInterpolation::Step);
      auto expectedTarget=originalCurves[0].tracks[i-1].target;expectedTarget.index=selected;EXPECT_EQ(restore.target,expectedTarget);
      EXPECT_EQ(restore.times,(std::vector<double>{0}));
      const auto& value=restore.target.kind==AnimationTargetKind::NodeTranslation?edited.translation:
          restore.target.kind==AnimationTargetKind::NodeRotation?edited.rotation:edited.scale;
      std::vector<double> binary32;for(const auto component:value) binary32.push_back(double(float(component)));EXPECT_EQ(restore.values,binary32);
    }
    (void)roundtrip.selectClip(loaded,0);(void)roundtrip.seek(loaded,.75);
    ASSERT_EQ(loaded.nodes[selected].translation.size(),3u);EXPECT_DOUBLE_EQ(loaded.nodes[selected].translation[2],6.75);
    ASSERT_EQ(loaded.nodes[selected].scale.size(),3u);EXPECT_DOUBLE_EQ(loaded.nodes[selected].scale[0],1.375);
    (void)roundtrip.selectClip(loaded,1);poseMatrixNear(EvaluateAnimatedNodeLocalTransform(loaded.nodes[selected]),EvaluateAnimatedNodeLocalTransform(edited),0.f);
    (void)roundtrip.clearClip(loaded);poseMatrixNear(EvaluateAnimatedNodeLocalTransform(loaded.nodes[selected]),EvaluateAnimatedNodeLocalTransform(fileDefault),0.f);
  };
  for(const auto* extension:{".gltf",".glb"}) checkSaved("sampled",extension,redoSample);
  dirty=player->clearClip(manager->animationSource());publishAnimation(dirty);
  EXPECT_TRUE(GltfCameraNodePoseEqual(manager->animationSource().nodes[1],edited));
  poseMatrixNear(graph.getNode(node(1))->localTransform,EvaluateAnimatedNodeLocalTransform(edited),0.f);
  for(const auto* extension:{".gltf",".glb"}) checkSaved("rest",extension,edited);
}

TEST_F(CameraViewPoseGenerationGpu, ComponentAnimationFeedsGraphChildLightsAndBoundsFromSampledCandidate) {
  auto source=poseLightAsset();poseTrack(source,1,"translation/0",TINYGLTF_TYPE_SCALAR,{0,2},true);load(std::move(source));
  auto dirty=player->selectClip(manager->animationSource(),0);dirty.merge(player->seek(manager->animationSource(),1.));
  publishAnimation(dirty);const auto parent=graph.getNode(node(4))->worldTransform;
  const auto view=desired();auto edit=manager->prepareCameraViewPoseEdit(poseTarget(),view,graph,player.get(),2.f);
  ASSERT_TRUE(edit.changed());const auto lights=edit.candidateLights();const auto bounds=edit.candidateBounds();
  const auto* selected=edit.candidateGraph().getNode(node(1));ASSERT_NE(selected,nullptr);
  EXPECT_FLOAT_EQ(selected->localTransform[3].x,1.f);
  for(size_t i=0;i<lights.pointBindings.size();++i) {
    const auto owner=lights.pointBindings[i].sourceNode;
    if(owner==1 || owner==5) {
      const auto expected=edit.candidateGraph().getNode(node(owner))->worldTransform[3];
      for(int axis=0;axis<3;++axis) EXPECT_NEAR(lights.points[i].positionRadius[axis],expected[axis],2e-5f);
    }
  }
  glm::vec3 expectedMin(std::numeric_limits<float>::infinity()),expectedMax(-std::numeric_limits<float>::infinity());
  for(const auto renderable:edit.candidateGraph().renderableNodes()) for(const auto position:
      {glm::vec4(0,0,0,1),glm::vec4(1,0,0,1),glm::vec4(0,1,0,1)}) {
    const auto point=glm::vec3(edit.candidateGraph().getNode(renderable)->worldTransform*position);
    expectedMin=glm::min(expectedMin,point);expectedMax=glm::max(expectedMax,point);
  }
  ASSERT_TRUE(bounds.valid);for(int axis=0;axis<3;++axis) {EXPECT_NEAR(bounds.min[axis],expectedMin[axis],2e-5f);EXPECT_NEAR(bounds.max[axis],expectedMax[axis],2e-5f);}
  const auto candidateLocal=selected->localTransform;ASSERT_TRUE(publishPose(edit,&view));
  const auto& authored=manager->authoredCameraSourceView().nodes[1];
  ASSERT_EQ(authored.translation.size(),3u);EXPECT_GT(std::abs(authored.translation[0]-1.),.1);
  auto expected=authored;expected.translation[0]=1.;
  EXPECT_TRUE(GltfCameraNodePoseEqual(manager->animationSource().nodes[1],expected));
  poseMatrixNear(candidateLocal,EvaluateAnimatedNodeLocalTransform(expected),0.f);
  poseMatrixNear(graph.getNode(node(1))->worldTransform,parent*candidateLocal,0.f);
  poseBoundsEqual(manager->modelBounds(),bounds);EXPECT_DOUBLE_EQ(player->time(),1.);
  ASSERT_EQ(manager->authoredPointLights().size(),lights.points.size());
  EXPECT_EQ(std::memcmp(manager->authoredPointLights().data(),lights.points.data(),lights.points.size()*sizeof(PointLightData)),0);
}

TEST_F(CameraViewPoseGenerationGpu, MissingViewportWitnessRejectsAndNoopDoesNotAdvanceSourceGraphOrRestRevisions) {
  const auto selected=poseTarget();const auto current=EvaluateAnimatedCamera(manager->animationSource().cameras[0],
      graph.getNode(node(1))->worldTransform,2.f);ASSERT_TRUE(current);
  const auto changedView=desired();auto earlier=manager->prepareCameraViewPoseEdit(selected,changedView,graph,player.get(),2.f);
  auto noop=manager->prepareCameraViewPoseEdit(selected,current->view,graph,player.get(),2.f);ASSERT_FALSE(noop.changed());
  const auto graphRevision=graph.revision(),cameraRevision=manager->cameraRevision();const auto rest=player->library().restNodes[1];
  bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeCameraViewPoseEdit(noop,graph,player.get(),2.f);
    published=manager->exchangeCameraViewPoseEdit(noop,graph,player.get(),2.f);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(noop);
  ASSERT_TRUE(publishPose(noop,&current->view));EXPECT_EQ(graph.revision(),graphRevision);EXPECT_EQ(manager->cameraRevision(),cameraRevision);
  EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(player->library().restNodes[1],rest));
  EXPECT_TRUE(manager->canExchangeCameraViewPoseEdit(earlier,graph,player.get(),2.f,&changedView));
}

TEST_F(CameraViewPoseGenerationGpu, StaleCurrentViewPlayerParentAndSourceRejectBothFinalGuardsWithoutAllocation) {
  for(unsigned mutation=0;mutation<8;++mutation) {
    SCOPED_TRACE(mutation);load(cameraAsset());const auto view=desired();
    auto prepared=manager->prepareCameraViewPoseEdit(poseTarget(),view,graph,player.get(),2.f);
    auto currentView=view;const AnimationPlayer* currentPlayer=player.get();float aspect=2.f;std::unique_ptr<AnimationPlayer> foreign;
    if(mutation==0) currentView[3][0]+=.1f;
    else if(mutation==1) {foreign=std::make_unique<AnimationPlayer>(*player);currentPlayer=foreign.get();}
    else if(mutation==2) {auto local=graph.getNode(node(4))->localTransform;local[3].x+=1;graph.setLocalTransform(node(4),local);graph.updateWorldTransforms();}
    else if(mutation==3) manager->animationSource().nodes[1].extras_json_string="later selected source metadata";
    else if(mutation==4) aspect=1.5f;
    else if(mutation==5) graph.getNode(node(0))->name+=" untracked unrelated name";
    else if(mutation==6) graph.getNode(node(0))->localTransform[3].y+=.5f;
    else graph.getNode(node(0))->children.push_back(node(2));
    const auto before=manager->animationSource().nodes[1],rest=player->library().restNodes[1];const auto graphRevision=graph.revision(),cameraRevision=manager->cameraRevision();
    const auto unrelated=*graph.getNode(node(0));
    bool ready{},published{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);
      ready=manager->canExchangeCameraViewPoseEdit(prepared,graph,currentPlayer,aspect,&currentView);
      published=manager->exchangeCameraViewPoseEdit(prepared,graph,currentPlayer==player.get()?player.get():foreign.get(),aspect,&currentView);attempts=deny.allocations();}
    EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(prepared);EXPECT_FALSE(prepared.command().published());
    EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(manager->animationSource().nodes[1],before));EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(player->library().restNodes[1],rest));
    EXPECT_EQ(graph.revision(),graphRevision);EXPECT_EQ(manager->cameraRevision(),cameraRevision);
    EXPECT_EQ(*graph.getNode(node(0)),unrelated);
    EXPECT_EQ(std::memcmp(&graph.getNode(node(0))->localTransform,&unrelated.localTransform,sizeof(glm::mat4)),0);
  }
}

TEST_F(CameraViewPoseGenerationGpu, CameraMeshAndChildLightsPublishPreparedBoundsAndLightBindingsTogether) {
  load(poseLightAsset());const auto before=manager->modelBounds();const auto view=desired();
  auto prepared=manager->prepareCameraViewPoseEdit(poseTarget(),view,graph,player.get(),2.f);
  const auto bounds=prepared.candidateBounds();const auto lights=prepared.candidateLights();
  ASSERT_EQ(lights.points.size(),2u);ASSERT_EQ(lights.pointBindings.size(),2u);EXPECT_TRUE(bounds.valid);
  EXPECT_GT(glm::length(bounds.center-before.center),.1f);const auto count=allocations();
  ASSERT_TRUE(publishPose(prepared,&view));EXPECT_EQ(allocations(),count);
  EXPECT_EQ(manager->authoredPointLightBindings().size(),lights.pointBindings.size());
  EXPECT_TRUE(std::equal(manager->authoredPointLightBindings().begin(),manager->authoredPointLightBindings().end(),lights.pointBindings.begin()));
  ASSERT_EQ(manager->authoredPointLights().size(),lights.points.size());
  EXPECT_EQ(std::memcmp(manager->authoredPointLights().data(),lights.points.data(),lights.points.size()*sizeof(PointLightData)),0);
  poseBoundsEqual(manager->modelBounds(),bounds);
  const auto parentWorld=graph.getNode(node(1))->worldTransform;const auto* child=graph.getNode(node(5));ASSERT_NE(child,nullptr);
  poseMatrixNear(child->worldTransform,parentWorld*child->localTransform);
}

TEST_F(CameraViewPoseGenerationGpu, ChangedRawLightDefinitionsAssociationsOrAreaTablesCannotPublishStaleDerivedState) {
  for(unsigned mutation=0;mutation<4;++mutation) {
    SCOPED_TRACE(mutation);load(poseLightAsset());const auto view=desired();
    auto prepared=manager->prepareCameraViewPoseEdit(poseTarget(),view,graph,player.get(),2.f);
    auto& source=manager->animationSource();
    if(mutation==0) source.lights[0].intensity+=1;
    else if(mutation==1) source.lights[0].extras=tinygltf::Value(tinygltf::Value::Object{{"currentOpaque",tinygltf::Value(41)}});
    else if(mutation==2) source.nodes[5].light=-1;
    else source.extensions["KHR_lights_area"]=tinygltf::Value(tinygltf::Value::Object{{"lights",tinygltf::Value(tinygltf::Value::Array{})}});
    const auto before=manager->publishedAuthoredSceneLights();const auto bounds=manager->modelBounds();const auto revision=graph.revision();
    bool ready{},published{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeCameraViewPoseEdit(prepared,graph,player.get(),2.f,&view);
      published=manager->exchangeCameraViewPoseEdit(prepared,graph,player.get(),2.f,&view);attempts=deny.allocations();}
    EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(prepared);
    EXPECT_EQ(graph.revision(),revision);poseBoundsEqual(manager->modelBounds(),bounds);
    ASSERT_EQ(manager->authoredPointLights().size(),before.points.size());
    EXPECT_EQ(std::memcmp(manager->authoredPointLights().data(),before.points.data(),before.points.size()*sizeof(PointLightData)),0);
  }
}

TEST_F(CameraViewPoseGenerationGpu, OriginalTailBirthRestoresHistoryButEqualFreshReplacementCannotReviveIt) {
  makeSelectedLeafLast();const auto selected=node(1);ASSERT_EQ(selected,graph.nodeCount()-1);const auto initialBirth=graph.nodeIncarnation(selected);
  const auto view=desired();auto prepared=manager->prepareCameraViewPoseEdit(poseTarget(),view,graph,player.get(),2.f);const auto command=prepared.command();
  ASSERT_TRUE(publishPose(prepared,&view));auto stale=manager->prepareCameraViewPoseReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);
  auto originalTail=graph.detachTail(selected);ASSERT_TRUE(originalTail);ASSERT_TRUE(graph.restoreTail(*originalTail));graph.updateWorldTransforms();
  EXPECT_EQ(graph.nodeIncarnation(selected),initialBirth);bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=manager->canExchangeCameraViewPoseEdit(stale,graph,player.get(),2.f);
    published=manager->exchangeCameraViewPoseEdit(stale,graph,player.get(),2.f);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);
  auto undo=manager->prepareCameraViewPoseReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f);ASSERT_TRUE(publishPose(undo));
  auto redo=manager->prepareCameraViewPoseReplay(command,GltfCameraEditSide::After,graph,player.get(),2.f);ASSERT_TRUE(publishPose(redo));
  const auto original=*graph.getNode(selected);auto cut=graph.detachTail(selected);ASSERT_TRUE(cut);
  const auto replacement=graph.createNode(original.localTransform,original.materialIndex,false,SceneGraph::kInvalidNode,original.name);
  ASSERT_EQ(replacement,selected);graph.getNode(replacement)->sourceNodeIndex=original.sourceNodeIndex;
  ASSERT_TRUE(graph.setParent(replacement,original.parent));graph.updateWorldTransforms();
  EXPECT_NE(graph.nodeIncarnation(replacement),initialBirth);
  EXPECT_THROW((void)manager->prepareCameraViewPoseReplay(command,GltfCameraEditSide::Before,graph,player.get(),2.f),std::invalid_argument);
}

TEST_F(CameraViewPoseGenerationGpu, DeniedPreparationAndBudgetsKeepEarlierReadyAndBothSavePathsRoundTripZeroMaterialPose) {
  auto source=cameraAsset(false);source.nodes[1].name="Pose destination";source.nodes[2].name="Shared sibling";
  // A tiny hierarchy with many indexed triangles proves the independent
  // derived-work admission accounts for geometry, not merely graph row count.
  constexpr uint32_t denseIndices=3072;auto& indexView=source.bufferViews[1];
  indexView.byteLength=denseIndices*sizeof(uint16_t);source.accessors[1].count=denseIndices;
  source.buffers[0].data.resize(indexView.byteOffset+indexView.byteLength);
  for(uint32_t i=0;i<denseIndices;++i) {const uint16_t index=uint16_t(i%3);std::memcpy(source.buffers[0].data.data()+indexView.byteOffset+i*sizeof(index),&index,sizeof(index));}
  load(std::move(source));ASSERT_LT(graph.nodeCount(),10u);
  ASSERT_FALSE(manager->primitiveRanges().empty());ASSERT_GT(manager->primitiveRanges()[0].indexCount,1024u);
  ASSERT_TRUE(manager->sourceMaterialRuntimeMapping().empty());const auto view=desired();const auto target=poseTarget();
  auto earlier=manager->prepareCameraViewPoseEdit(target,view,graph,player.get(),2.f);
  {auto warm=manager->prepareCameraViewPoseEdit(target,view,graph,player.get(),2.f);}
  size_t calls{};
  {material_publication_test::AllocationGuard count(false);auto measured=manager->prepareCameraViewPoseEdit(target,view,graph,player.get(),2.f);calls=count.allocations();}
  ASSERT_GT(calls,0u);ASSERT_LT(calls,5000u);
  for(size_t allowance=0;allowance<calls;++allowance) {
    SCOPED_TRACE(allowance);bool failed{};
    {material_publication_test::AllocationGuard deny(false,allowance);
      try {(void)manager->prepareCameraViewPoseEdit(target,view,graph,player.get(),2.f);}catch(const std::bad_alloc&) {failed=true;}}
    ASSERT_TRUE(failed);EXPECT_TRUE(manager->canExchangeCameraViewPoseEdit(earlier,graph,player.get(),2.f,&view));
  }
  for(unsigned field=0;field<6;++field) {
    CameraViewPoseLimits limits;if(field==0) limits.maxPreparedCpuBytes=1;else if(field==1) limits.placement.maxPayloadBytes=1;
    else if(field==2) limits.animation.maxOwnedBytes=1;else if(field==3) limits.authoring.maxOwnedBytes=1;
    else if(field==4) limits.maxGraphNodes=1;else limits.maxDerivedWork=1024;
    const auto revision=graph.revision(),cameraRevision=manager->cameraRevision();const auto count=allocations();
    const auto rest=player->library().restNodes[1];
    EXPECT_THROW((void)manager->prepareCameraViewPoseEdit(target,view,graph,player.get(),2.f,limits),std::invalid_argument);
    EXPECT_TRUE(manager->canExchangeCameraViewPoseEdit(earlier,graph,player.get(),2.f,&view));
    EXPECT_EQ(graph.revision(),revision);EXPECT_EQ(manager->cameraRevision(),cameraRevision);EXPECT_EQ(allocations(),count);
    EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(player->library().restNodes[1],rest));
  }
  const auto lens=manager->authoredCamera(0);ASSERT_TRUE(publishPose(earlier,&view));const auto expected=manager->authoredCameraSourceView().nodes[1];
  for(const auto* extension:{".gltf",".glb"}) for(bool frontendSharedPath:{false,true}) {
    const auto path=temp/(std::string(frontendSharedPath?"shared-pose":"manager-pose")+extension);
    if(frontendSharedPath) writeExportModel(manager->buildSceneExport(graph),path);else manager->saveScene(graph,path);
    const auto loaded=LoadSourceModel(path.string());const auto found=std::ranges::find_if(loaded.nodes,[](const auto& node){return node.name=="Pose destination";});
    ASSERT_NE(found,loaded.nodes.end());EXPECT_TRUE(GltfCameraNodePoseEqual(*found,expected));
    EXPECT_TRUE(GltfCameraRecordsEqual(loaded.cameras[0],lens));EXPECT_EQ(std::ranges::count_if(loaded.nodes,[](const auto& node){return node.camera==0;}),2);
  }
}
} // namespace
