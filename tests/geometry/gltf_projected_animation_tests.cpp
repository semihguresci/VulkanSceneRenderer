#include <Container/geometry/GltfAnimation.h>
#include <Container/utility/GraphCloneProjection.h>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace {
using namespace container::geometry::gltf;
using namespace container::scene;
using Value=tinygltf::Value;
using Object=Value::Object;
constexpr auto invalid=SceneGraph::kInvalidNode;

int bytes(tinygltf::Model& model,std::vector<unsigned char> data,int component,int type,size_t count) {
  tinygltf::Buffer buffer;buffer.data=std::move(data);buffer.extras=Value(Object{{"identity",Value(1.0)}});
  tinygltf::BufferView view;view.buffer=int(model.buffers.size());view.byteLength=buffer.data.size();
  tinygltf::Accessor accessor;accessor.sparse={};accessor.bufferView=int(model.bufferViews.size());
  accessor.componentType=component;accessor.type=type;accessor.count=count;
  const auto result=int(model.accessors.size());
  model.buffers.push_back(std::move(buffer));model.bufferViews.push_back(view);model.accessors.push_back(accessor);
  return result;
}
int floats(tinygltf::Model& model,std::initializer_list<float> values,int type,size_t count) {
  std::vector<unsigned char> data(values.size()*sizeof(float));std::memcpy(data.data(),values.begin(),data.size());
  const auto result=bytes(model,std::move(data),TINYGLTF_COMPONENT_TYPE_FLOAT,type,count);
  if(type==TINYGLTF_TYPE_SCALAR && values.size()) {
    const auto [low,high]=std::minmax_element(values.begin(),values.end());
    model.accessors[size_t(result)].minValues={*low};model.accessors[size_t(result)].maxValues={*high};
  }
  return result;
}
void channel(tinygltf::Animation& clip,int input,int output,int node,std::string path) {
  tinygltf::AnimationSampler sampler;sampler.input=input;sampler.output=output;sampler.interpolation="LINEAR";
  sampler.extras=Value(Object{{"identity",Value(1.0)}});sampler.extras_json_string="sampler metadata";
  tinygltf::AnimationChannel target;target.sampler=int(clip.samplers.size());target.target_node=node;target.target_path=std::move(path);
  target.extras=Value(Object{{"identity",Value(1.0)}});target.target_extras=Value(Object{{"identity",Value(1.0)}});
  target.target_extras_json_string="target metadata";
  clip.samplers.push_back(sampler);clip.channels.push_back(target);
}
void pointer(tinygltf::Animation& clip,int input,int output,std::string path) {
  channel(clip,input,output,-1,"pointer");
  clip.channels.back().target_extensions["KHR_animation_pointer"]=Value(Object{
      {"pointer",Value(std::move(path))},{"extras",Value(Object{{"identity",Value(1.0)}})}});
  clip.channels.back().target_extensions_json_string="pointer metadata";
}
struct Fixture {
  tinygltf::Model authored;
  SceneGraph graph;
  std::vector<std::vector<uint32_t>> scenes{{2,4,6,8}};
  std::array<uint32_t,1> primitives{0};
  Fixture() {
    authored.asset.version="2.0";authored.nodes.resize(4);authored.materials.emplace_back();
    const auto positions=floats(authored,{0,0,0,1,0,0,0,1,0},TINYGLTF_TYPE_VEC3,3);
    const auto delta=floats(authored,{0,0,1,0,0,1,0,0,1},TINYGLTF_TYPE_VEC3,3);
    tinygltf::Primitive primitive;primitive.mode=TINYGLTF_MODE_TRIANGLES;primitive.material=0;
    primitive.attributes["POSITION"]=positions;primitive.targets={{{"POSITION",delta}}};
    primitive.attributes["JOINTS_0"]=bytes(authored,std::vector<unsigned char>(12),TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,TINYGLTF_TYPE_VEC4,3);
    primitive.attributes["WEIGHTS_0"]=floats(authored,{1,0,0,0,1,0,0,0,1,0,0,0},TINYGLTF_TYPE_VEC4,3);
    primitive.extras=Value(Object{{"identity",Value(1.0)}});
    tinygltf::Mesh mesh;mesh.primitives.push_back(primitive);mesh.weights={.25};authored.meshes.push_back(mesh);
    for(int n=0;n<3;++n) {authored.nodes[size_t(n)].mesh=0;authored.nodes[size_t(n)].name="Owner "+std::to_string(n);}
    authored.nodes[0].skin=0;authored.nodes[0].translation={1,0,0};authored.nodes[0].weights={.3};
    authored.nodes[1].translation={0,2,0};authored.nodes[1].weights={.4};
    authored.nodes[2].matrix={1,0,0,0,.25,1,0,0,0,0,1,0,-1,0,0,1};
    tinygltf::Skin skin;skin.joints={1};skin.skeleton=1;skin.extras=Value(Object{{"identity",Value(1.0)}});
    skin.inverseBindMatrices=floats(authored,{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},TINYGLTF_TYPE_MAT4,1);authored.skins.push_back(skin);
    tinygltf::Camera camera;camera.type="perspective";camera.perspective.yfov=.7;camera.perspective.znear=.1;authored.cameras.push_back(camera);
    tinygltf::Light light;light.type="point";light.color={1,1,1};light.intensity=3;authored.lights.push_back(light);
    authored.nodes[3].camera=0;authored.nodes[3].light=0;authored.nodes[3].name="Camera and lamp";
    authored.nodes[3].extensions["KHR_lights_punctual"]=Value(Object{{"light",Value(0)}});
    authored.extensionsUsed={"KHR_animation_pointer","KHR_lights_punctual"};
    authored.extensions["KHR_lights_punctual"]=Value(Object{{"lights",Value(Value::Array{
        Value(Object{{"type",Value(std::string("point"))}})})}});
    tinygltf::Scene scene;scene.nodes={0,1,2,3};scene.extras=Value(Object{{"identity",Value(1.0)}});authored.scenes.push_back(scene);authored.defaultScene=0;
    tinygltf::Image image;image.name="image identity";image.as_is=true;image.mimeType="image/png";image.image={1,2,3,4};authored.images.push_back(image);
    authored.samplers.emplace_back();tinygltf::Texture texture;texture.source=0;texture.sampler=0;authored.textures.push_back(texture);
    authored.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
    const auto input=floats(authored,{0,1},TINYGLTF_TYPE_SCALAR,2);tinygltf::Animation clip;clip.name="Shared animation";
    clip.extras=Value(Object{{"identity",Value(1.0)}});
    channel(clip,input,floats(authored,{1,0,0,4,0,0},TINYGLTF_TYPE_VEC3,2),0,"translation");
    pointer(clip,input,floats(authored,{.2f,.8f},TINYGLTF_TYPE_SCALAR,2),"/nodes/0/weights/0");
    channel(clip,input,floats(authored,{0,2,0,0,4,0},TINYGLTF_TYPE_VEC3,2),1,"translation");
    pointer(clip,input,floats(authored,{.1f,.9f},TINYGLTF_TYPE_SCALAR,2),"/nodes/1/weights/0");
    pointer(clip,input,floats(authored,{.2f,.9f},TINYGLTF_TYPE_SCALAR,2),"/materials/0/pbrMetallicRoughness/roughnessFactor");
    pointer(clip,input,floats(authored,{3,9},TINYGLTF_TYPE_SCALAR,2),"/extensions/KHR_lights_punctual/lights/0/intensity");
    pointer(clip,input,floats(authored,{.7f,.9f},TINYGLTF_TYPE_SCALAR,2),"/cameras/0/perspective/yfov");
    pointer(clip,input,floats(authored,{1,3},TINYGLTF_TYPE_SCALAR,2),"/nodes/0/scale/1");
    authored.animations.push_back(clip);
    graph.createNode(glm::mat4(1),0,false,invalid,"Editor");graph.getNode(0)->editorSceneRoot=true;
    graph.createNode(glm::mat4(1),0,false,invalid,"Import");graph.getNode(1)->interactionRootBoundary=true;graph.setParent(1,0);
    for(int source=0;source<3;++source) {
      const auto group=graph.createNode(glm::mat4(1),0,false,invalid,authored.nodes[size_t(source)].name);
      graph.getNode(group)->sourceNodeIndex=source;graph.setParent(group,1);
      const auto draw=graph.createNode(glm::mat4(1),0,true,0,"Primitive");auto& node=*graph.getNode(draw);
      node.sourceMeshIndex=0;node.sourcePrimitiveIndex=0;node.meshOwnerGraphNode=group;graph.setParent(draw,group);
    }
    graph.createNode(glm::mat4(1),0,false,invalid,"Camera and lamp");graph.getNode(8)->sourceNodeIndex=3;graph.setParent(8,1);
    graph.createNode(glm::mat4(1),0,false,invalid,"Independent empty");graph.setParent(9,2);
    graph.setActiveRoots(scenes[0]);graph.updateWorldTransforms();
  }
  GraphCloneProjectionPlan projection(const tinygltf::Model& current,uint32_t root) const {
    return BuildGraphCloneProjection({graph,current,root,scenes,0,1,1,primitives});
  }
};

void controls(const AnimationPlayer& actual,const AnimationPlayer& expected) {
  EXPECT_EQ(actual.selectedClip(),expected.selectedClip());EXPECT_EQ(std::bit_cast<uint64_t>(actual.time()),std::bit_cast<uint64_t>(expected.time()));
  EXPECT_EQ(std::bit_cast<uint64_t>(actual.speed()),std::bit_cast<uint64_t>(expected.speed()));
  EXPECT_EQ(actual.playing(),expected.playing());EXPECT_EQ(actual.loop(),expected.loop());
}
void unchangedPlayer(const AnimationPlayer& player,const AnimationPlayer& before) {
  controls(player,before);EXPECT_EQ(player.library().clips,before.library().clips);
  EXPECT_EQ(player.library().restNodes,before.library().restNodes);
}
void expectRejected(const Fixture& f,const AnimationPlayer& player,const tinygltf::Model& live,
    const GltfProjectedSubtreeClonePlan& plan,const GltfProjectedCloneOwnershipProof& proof) {
  const auto oldPlayer=player;const auto oldLive=live;
  EXPECT_ANY_THROW((void)player.withClonedSource(live,plan,proof));
  unchangedPlayer(player,oldPlayer);EXPECT_EQ(live,oldLive);(void)f;
}

TEST(GltfProjectedAnimation,PreservesExactControlsAuthoredRestsAndSharedResourceTracks) {
  Fixture f;auto live=f.authored;AnimationPlayer player(live);player.selectClip(live,0);player.seek(live,.375);
  player.setSpeed(-2.5);player.setLoop(false);player.play();const auto before=live;
  const auto graph=f.projection(live,2);const auto plan=PlanGltfProjectedSubtreeClone(live,f.authored,graph.sourceProjection);
  auto copy=player.withClonedSource(live,plan,graph.ownership);controls(copy,player);EXPECT_EQ(live,before);
  ASSERT_EQ(copy.library().clips[0].tracks.size(),12u); // Owner TRS/weights plus joint TRS; no joint weights.
  const auto owner=size_t(plan.clone.nodes[0]),joint=size_t(plan.clone.nodes[1]);
  EXPECT_EQ(plan.roles[1],GltfCloneNodeRole::JointTransformOnly);EXPECT_FALSE(plan.copyWeightTracks[1]);
  EXPECT_EQ(copy.library().restNodes[owner].translation,f.authored.nodes[0].translation);
  EXPECT_EQ(copy.library().restNodes[owner].weights,f.authored.nodes[0].weights);
  EXPECT_TRUE(copy.library().restNodes[joint].weights.empty());EXPECT_EQ(copy.library().restNodes[joint].mesh,-1);
  EXPECT_EQ(copy.library().restMaterials,player.library().restMaterials);EXPECT_EQ(copy.library().restLights,player.library().restLights);
  EXPECT_EQ(copy.library().restCameras,player.library().restCameras);
  auto restored=plan.clone.evaluatedSource;copy.clearClip(restored);
  EXPECT_EQ(restored.nodes[owner].translation,f.authored.nodes[0].translation);EXPECT_EQ(restored.nodes[owner].weights,f.authored.nodes[0].weights);
  EXPECT_EQ(restored.materials[0].pbrMetallicRoughness.roughnessFactor,f.authored.materials[0].pbrMetallicRoughness.roughnessFactor);
  EXPECT_EQ(restored.lights[0].intensity,3);EXPECT_EQ(restored.cameras[0].perspective.yfov,.7);
}

TEST(GltfProjectedAnimation,VisualOwnerWithoutCopiedPrimitivesOmitsWeightTracksAndRest) {
  Fixture f;const auto outside=f.graph.createNode(glm::mat4(1),0,false,invalid,"Outside");f.graph.setParent(outside,1);
  f.graph.setParent(3,outside);f.scenes[0].push_back(outside);f.graph.setActiveRoots(f.scenes[0]);f.graph.updateWorldTransforms();
  AnimationPlayer player(f.authored);const auto graph=f.projection(f.authored,2);
  const auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);
  EXPECT_EQ(plan.roles[0],GltfCloneNodeRole::Visual);EXPECT_FALSE(plan.clone.contextOnly[0]);EXPECT_FALSE(plan.copyWeightTracks[0]);
  const auto copy=player.withClonedSource(f.authored,plan,graph.ownership);
  ASSERT_EQ(copy.library().clips[0].tracks.size(),10u);const auto owner=size_t(plan.clone.nodes[0]);
  EXPECT_EQ(copy.library().restNodes[owner].mesh,-1);EXPECT_TRUE(copy.library().restNodes[owner].weights.empty());
  EXPECT_EQ(plan.clone.nodes[1],-1);
}

TEST(GltfProjectedAnimation,NonvisualBindingOwnerRetainsWeightsWhileJointOnlyContextDoesNot) {
  Fixture f;const auto outside=f.graph.createNode(glm::mat4(1),0,false,invalid,"Selected container");f.graph.setParent(outside,1);
  f.graph.setParent(3,outside);f.scenes[0].push_back(outside);f.graph.setActiveRoots(f.scenes[0]);f.graph.updateWorldTransforms();
  auto live=f.authored;AnimationPlayer player(live);player.selectClip(live,0);player.seek(live,.625);
  const auto graph=f.projection(live,outside);const auto plan=PlanGltfProjectedSubtreeClone(live,f.authored,graph.sourceProjection);
  ASSERT_EQ(plan.roles[0],GltfCloneNodeRole::MeshBindingOnly);EXPECT_FALSE(plan.clone.contextOnly[0]);EXPECT_TRUE(plan.copyWeightTracks[0]);
  auto copy=player.withClonedSource(live,plan,graph.ownership);ASSERT_EQ(copy.library().clips[0].tracks.size(),12u);
  const auto owner=size_t(plan.clone.nodes[0]),joint=size_t(plan.clone.nodes[1]);
  EXPECT_EQ(copy.library().restNodes[owner].skin,plan.clone.skins[0]);EXPECT_EQ(copy.library().restNodes[owner].weights,f.authored.nodes[0].weights);
  EXPECT_TRUE(copy.library().restNodes[joint].weights.empty());
  auto next=plan.clone.evaluatedSource;copy.seek(next,1);EXPECT_EQ(next.nodes[owner].weights,next.nodes[0].weights);
  EXPECT_EQ(next.nodes[joint].translation,next.nodes[1].translation);EXPECT_TRUE(next.nodes[joint].weights.empty());
}

TEST(GltfProjectedAnimation,RepeatedNonrestCloneKeepsOriginalAuthoredRestsAndControls) {
  Fixture f;auto live=f.authored;AnimationPlayer player(live);player.selectClip(live,0);player.seek(live,.25);player.play();
  auto firstGraph=f.projection(live,2);const auto first=PlanGltfProjectedSubtreeClone(live,f.authored,firstGraph.sourceProjection);
  auto firstPlayer=player.withClonedSource(live,first,firstGraph.ownership);live=first.clone.evaluatedSource;firstPlayer.seek(live,.75);
  firstPlayer.setSpeed(-.0);firstPlayer.setLoop(false);firstPlayer.pause();
  auto secondGraph=BuildGraphCloneProjection({firstGraph.graph,live,firstGraph.root,firstGraph.sceneRoots,0,1,1,f.primitives});
  const auto second=PlanGltfProjectedSubtreeClone(live,first.clone.authoredSource,secondGraph.sourceProjection);
  auto secondPlayer=firstPlayer.withClonedSource(live,second,secondGraph.ownership);controls(secondPlayer,firstPlayer);
  const auto originalCopy=size_t(first.clone.nodes[0]),secondCopy=size_t(second.clone.nodes[originalCopy]);
  EXPECT_EQ(secondPlayer.library().restNodes[secondCopy].translation,f.authored.nodes[0].translation);
  EXPECT_EQ(secondPlayer.library().restNodes[secondCopy].weights,f.authored.nodes[0].weights);
  auto reset=second.clone.evaluatedSource;secondPlayer.clearClip(reset);
  EXPECT_EQ(reset.nodes[0].translation,reset.nodes[originalCopy].translation);EXPECT_EQ(reset.nodes[0].translation,reset.nodes[secondCopy].translation);
}

TEST(GltfProjectedAnimation,GraphOnlyCopyAcceptsEqualSourceCountsAndKeepsUnselectedControls) {
  Fixture f;AnimationPlayer player(f.authored);player.setSpeed(-.0);player.setLoop(false);
  const auto graph=f.projection(f.authored,9);ASSERT_TRUE(graph.sourceProjection.nodes.empty());
  const auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);
  const auto copy=player.withClonedSource(f.authored,plan,graph.ownership);controls(copy,player);
  EXPECT_EQ(copy.library().clips,player.library().clips);EXPECT_EQ(copy.library().restNodes,player.library().restNodes);
  EXPECT_EQ(plan.clone.evaluatedSource.nodes.size(),f.authored.nodes.size());
}

TEST(GltfProjectedAnimation,EmptySourceGraphOnlyCopyAcceptsZeroNodeProjection) {
  tinygltf::Model source;source.asset.version="2.0";SceneGraph graph;const auto node=graph.createNode(glm::mat4(1),0,false,invalid,"Empty");
  graph.updateWorldTransforms();const std::vector<std::vector<uint32_t>> scenes{{node}};
  const auto projection=BuildGraphCloneProjection({graph,source,node,scenes,0,0,1,{}});
  const auto plan=PlanGltfProjectedSubtreeClone(source,source,projection.sourceProjection);AnimationPlayer player(source);
  const auto copy=player.withClonedSource(source,plan,projection.ownership);EXPECT_TRUE(copy.library().restNodes.empty());
  EXPECT_TRUE(copy.library().clips.empty());EXPECT_FALSE(copy.selectedClip());
}

TEST(GltfProjectedAnimation,PreservesAuthoredMatrixForUnanimatedClone) {
  Fixture f;AnimationPlayer player(f.authored);const auto graph=f.projection(f.authored,6);
  const auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);
  const auto copy=player.withClonedSource(f.authored,plan,graph.ownership);
  EXPECT_EQ(copy.library().restNodes[size_t(plan.clone.nodes[2])].matrix,f.authored.nodes[2].matrix);
}

TEST(GltfProjectedAnimation,RejectsMissingForeignAndForgedOwnershipWithoutMutation) {
  Fixture f;AnimationPlayer player(f.authored);const auto graph=f.projection(f.authored,2);
  const auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);
  expectRejected(f,player,f.authored,plan,{});const auto foreign=f.projection(f.authored,6);
  expectRejected(f,player,f.authored,plan,foreign.ownership);
  using Mutate=std::function<void(GltfProjectedSubtreeClonePlan&)>;
  for(const auto& mutate:std::vector<Mutate>{
      [](auto& p){p.copyWeightTracks[0]=false;},[](auto& p){p.retainMeshBindings[0]=false;},
      [](auto& p){p.roles[0]=GltfCloneNodeRole::JointTransformOnly;},[](auto& p){p.clone.contextOnly[0]=true;},
      [](auto& p){p.clone.nodes[0]=p.clone.nodes[1];},
      [](auto& p){p.clone.evaluatedSource.skins.back().joints[0]=0;},
      [](auto& p){p.clone.authoredSource.skins.back().extras=Value(std::nextafter(1.0,2.0));},
      [](auto& p){p.clone.evaluatedSource.scenes[0].nodes.pop_back();}}) {
    auto bad=plan;mutate(bad);expectRejected(f,player,f.authored,bad,graph.ownership);
  }
}

TEST(GltfProjectedAnimation,RejectsResourcePrefixAndEvaluatedPayloadChangesIncludingOneUlpMetadata) {
  Fixture f;AnimationPlayer player(f.authored);const auto graph=f.projection(f.authored,2);
  const auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);
  using Mutate=std::function<void(tinygltf::Model&)>;
  for(const auto& mutate:std::vector<Mutate>{
      [](auto& m){m.buffers[0].data[0]^=1;},[](auto& m){m.accessors[0].byteOffset=4;},
      [](auto& m){m.accessors[0].extras=Value(std::nextafter(1.0,2.0));},
      [](auto& m){m.bufferViews[0].extensions_json_string="changed";},
      [](auto& m){m.meshes[0].primitives[0].extras.Get<Object>()["identity"]=Value(std::nextafter(1.0,2.0));},
      [](auto& m){m.meshes[0].weights[0]=std::nextafter(.25,1.0);},
      [](auto& m){m.samplers[0].wrapS=TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;},
      [](auto& m){m.textures[0].source=-1;},[](auto& m){m.images[0].image[0]^=1;},
      [](auto& m){m.images[0].bufferView=0;},
      [](auto& m){m.nodes[0].translation[0]=std::nextafter(1.0,2.0);},
      [](auto& m){m.materials[0].pbrMetallicRoughness.roughnessFactor=.5;},
      [](auto& m){m.lights[0].intensity=4;},[](auto& m){m.cameras[0].perspective.yfov=.8;}}) {
    auto bad=plan;mutate(bad.clone.evaluatedSource);expectRejected(f,player,f.authored,bad,graph.ownership);
  }
}

TEST(GltfProjectedAnimation,RejectsForgedAppendedNumericPoseAndOpaqueMetadata) {
  Fixture f;AnimationPlayer player(f.authored);const auto graph=f.projection(f.authored,2);
  const auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);
  for(int mutation=0;mutation<4;++mutation) {
    auto bad=plan;auto& node=bad.clone.evaluatedSource.nodes[size_t(bad.clone.nodes[0])];
    if(mutation==0) node.translation[0]=std::nextafter(1.0,2.0);
    if(mutation==1) node.extras=Value(Object{{"invented",Value(1.0)}});
    if(mutation==2) node.extras_json_string="invented raw metadata";
    if(mutation==3) node.extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(true)},{"extras",Value(1.0)}});
    expectRejected(f,player,f.authored,bad,graph.ownership);
  }
}

TEST(GltfProjectedAnimation,RejectsRawPrefixAndUndeclaredSuffixChanges) {
  Fixture f;AnimationPlayer player(f.authored);const auto graph=f.projection(f.authored,2);
  const auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);
  using Mutate=std::function<void(tinygltf::Animation&)>;
  for(const auto& mutate:std::vector<Mutate>{
      [](auto& a){a.channels[0].target_extras.Get<Object>()["identity"]=Value(std::nextafter(1.0,2.0));},
      [](auto& a){a.channels[1].target_extensions_json_string+="!";},
      [](auto& a){a.samplers[0].extras.Get<Object>()["identity"]=Value(std::nextafter(1.0,2.0));},
      [](auto& a){a.channels.pop_back();},[](auto& a){std::swap(a.channels[8],a.channels[9]);},
      [](auto& a){a.channels.back().target_extensions["KHR_animation_pointer"].Get<Object>()["pointer"]=Value(std::string("/nodes/0/scale/1"));},
      [](auto& a){a.channels.push_back(a.channels[4]);}}) {
    auto bad=plan;mutate(bad.clone.evaluatedSource.animations[0]);expectRejected(f,player,f.authored,bad,graph.ownership);
    // Keep both public outputs internally equal: the original raw/typed source
    // and declared remap must still reject an independently forged suffix.
    bad.clone.authoredSource.animations=bad.clone.evaluatedSource.animations;
    expectRejected(f,player,f.authored,bad,graph.ownership);
  }
}

TEST(GltfProjectedAnimation,RejectsDecodedDataChangesEvenWhenCurrentAndBothOutputsAgree) {
  Fixture f;AnimationPlayer player(f.authored);const auto graph=f.projection(f.authored,2);
  auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);auto changed=f.authored;
  const auto output=changed.animations[0].samplers[0].output;const auto view=changed.accessors[size_t(output)].bufferView;
  const auto buffer=changed.bufferViews[size_t(view)].buffer;float value=2;std::memcpy(changed.buffers[size_t(buffer)].data.data(),&value,sizeof(value));
  plan.clone.authoredSource.buffers=changed.buffers;plan.clone.evaluatedSource.buffers=changed.buffers;
  expectRejected(f,player,changed,plan,graph.ownership);
}

TEST(GltfProjectedAnimation,AllowsArchiveResourceSuffixAndCompressedImageRepresentation) {
  Fixture f;auto live=f.authored;live.images[0].as_is=false;live.images[0].width=live.images[0].height=1;
  live.images[0].component=4;live.images[0].bits=8;live.images[0].pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;live.images[0].image={255,0,0,255};
  auto archive=f.authored;archive.buffers.push_back(archive.buffers[0]);archive.bufferViews.push_back(archive.bufferViews[0]);
  archive.accessors.push_back(archive.accessors[0]);archive.samplers.emplace_back();archive.textures.push_back(archive.textures[0]);archive.images.push_back(archive.images[0]);
  AnimationPlayer player(live);const auto graph=f.projection(live,2);const auto plan=PlanGltfProjectedSubtreeClone(live,archive,graph.sourceProjection);
  EXPECT_NO_THROW((void)player.withClonedSource(live,plan,graph.ownership));
}

TEST(GltfProjectedAnimation,RejectsExactSparseReferencesAndMetadataWithoutDependingOnDecodedUse) {
  Fixture f;
  const auto indices=bytes(f.authored,{0},TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,TINYGLTF_TYPE_SCALAR,1);
  const auto values=floats(f.authored,{7},TINYGLTF_TYPE_SCALAR,1);
  tinygltf::Accessor sparse;sparse.sparse={};sparse.type=TINYGLTF_TYPE_SCALAR;
  sparse.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;sparse.count=3;sparse.sparse.isSparse=true;sparse.sparse.count=1;
  sparse.sparse.indices.bufferView=f.authored.accessors[size_t(indices)].bufferView;
  sparse.sparse.indices.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  sparse.sparse.values.bufferView=f.authored.accessors[size_t(values)].bufferView;
  sparse.sparse.indices.extras=Value(Object{{"identity",Value(1.0)}});f.authored.accessors.push_back(sparse);
  AnimationPlayer player(f.authored);const auto graph=f.projection(f.authored,2);
  const auto plan=PlanGltfProjectedSubtreeClone(f.authored,f.authored,graph.sourceProjection);
  EXPECT_NO_THROW((void)player.withClonedSource(f.authored,plan,graph.ownership));
  for(int mutation=0;mutation<4;++mutation) {
    auto bad=plan;
    for(auto* output:{&bad.clone.evaluatedSource,&bad.clone.authoredSource}) {
      auto& value=output->accessors.back().sparse;
      if(mutation==0) ++value.indices.byteOffset;
      if(mutation==1) value.values.bufferView=value.indices.bufferView;
      if(mutation==2) ++value.count;
      if(mutation==3) value.indices.extras.Get<Object>()["identity"]=Value(std::nextafter(1.0,2.0));
    }
    expectRejected(f,player,f.authored,bad,graph.ownership);
  }
}
} // namespace
