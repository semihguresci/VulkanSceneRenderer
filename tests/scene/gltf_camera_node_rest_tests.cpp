#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfAnimationRuntime.h>
#include <Container/geometry/GltfCameraPoseAuthoring.h>

#include <gtest/gtest.h>

// This executable owns the global allocation controls exactly once.
#include "../renderer/scene/material_publication_allocations.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace {
using namespace container::geometry::gltf;
using V = tinygltf::Value;
using O = V::Object;

int floats(tinygltf::Model& model, std::initializer_list<float> values, int type, size_t count) {
  tinygltf::Buffer buffer; buffer.data.resize(values.size() * sizeof(float));
  std::memcpy(buffer.data.data(), values.begin(), buffer.data.size());
  tinygltf::BufferView view; view.buffer = int(model.buffers.size()); view.byteLength = buffer.data.size();
  tinygltf::Accessor accessor; accessor.sparse = {}; accessor.bufferView = int(model.bufferViews.size());
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT; accessor.type = type; accessor.count = count;
  if (type == TINYGLTF_TYPE_SCALAR) {
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    accessor.minValues = {*low}; accessor.maxValues = {*high};
  }
  const auto result = int(model.accessors.size());
  model.buffers.push_back(buffer); model.bufferViews.push_back(view); model.accessors.push_back(accessor);
  return result;
}
void channel(tinygltf::Model& model, size_t clip, int input, int output, int node, const std::string& path) {
  if (model.animations.size() <= clip) model.animations.resize(clip + 1);
  auto& animation = model.animations[clip];
  tinygltf::AnimationSampler sampler; sampler.input = input; sampler.output = output;
  sampler.extras = V(O{{"sampler", V(std::string(96, 's'))}});
  tinygltf::AnimationChannel raw; raw.sampler = int(animation.samplers.size());
  raw.target_node = node; raw.target_path = node < 0 ? "pointer" : path;
  if (node < 0) raw.target_extensions["KHR_animation_pointer"] = V(O{{"pointer", V(path)}});
  raw.target_extras = V(O{{"channel", V(27)}});
  raw.target_extras_json_string = "selected raw channel metadata";
  animation.samplers.push_back(sampler); animation.channels.push_back(raw);
}
void copyPose(tinygltf::Node& destination, const tinygltf::Node& source) {
  destination.matrix = source.matrix; destination.translation = source.translation;
  destination.rotation = source.rotation; destination.scale = source.scale;
}
struct Fixture {
  tinygltf::Model authored, evaluated;
  GltfCameraPoseGraphState graph;
  Fixture() {
    authored.nodes.resize(3); authored.cameras.emplace_back(); authored.materials.emplace_back();
    auto& camera = authored.cameras[0]; camera.type = "perspective";
    camera.perspective.yfov = .8; camera.perspective.znear = .1;
    camera.extras = V(O{{"keepLens", V(19)}});
    auto& node = authored.nodes[0]; node.camera = 0; node.mesh = 0; node.name = std::string(96, 'n');
    node.translation = {0, 0, 6}; node.weights = {.25, .75};
    node.extras = V(O{{"keepNode", V(17)}, {"signedZero", V(-0.)}});
    node.extensions["VENDOR_node"] = V(O{{"keep", V(std::string(96, 'm'))}});
    node.extras_json_string = "selected rest raw extras";
    authored.nodes[1].camera = 0; authored.nodes[1].translation = {4, 1, 7};
    authored.nodes[2].translation = {0, 0, 0};
    authored.meshes.emplace_back(); authored.meshes[0].weights = {.1, .9};
    authored.meshes[0].primitives.emplace_back(); authored.meshes[0].primitives[0].targets.resize(2);
    const auto time = floats(authored, {0, 2}, TINYGLTF_TYPE_SCALAR, 2);
    channel(authored, 0, time, floats(authored, {.25f, .75f, .75f, .25f}, TINYGLTF_TYPE_SCALAR, 4), 0, "weights");
    channel(authored, 0, time, floats(authored, {0, 0, 0, 2, 3, 4}, TINYGLTF_TYPE_VEC3, 2), 2, "translation");
    channel(authored, 0, time, floats(authored, {.2f, .8f}, TINYGLTF_TYPE_SCALAR, 2), -1,
        "/materials/0/pbrMetallicRoughness/roughnessFactor");
    channel(authored, 1, time, floats(authored, {.4f, .6f, .6f, .4f}, TINYGLTF_TYPE_SCALAR, 4), 0, "weights");
    authored.extensionsUsed = {"KHR_animation_pointer"};
    evaluated = authored;
    graph.identity = 31; graph.revision = 7; graph.nodeIncarnation = 91;
    graph.nodeCount = 3; graph.node = 0; graph.parent = UINT32_MAX;
    graph.local = glm::translate(glm::mat4(1), glm::vec3(0, 0, 6)); graph.world = graph.local;
    graph.viewportAspect = 1.5f;
  }
  AnimationPlayer running() {
    AnimationPlayer player(authored); player.selectClip(evaluated, 0); player.seek(evaluated, .5);
    player.setSpeed(1.25); player.setLoop(false); player.play(); return player;
  }
  GltfCameraPosePlan plan() const {
    return PlanGltfCameraViewPose(GltfCameraPoseView(authored), GltfCameraPoseView(evaluated), 0, 0, graph,
        glm::lookAt(glm::vec3(2, 1, 8), glm::vec3(0), glm::vec3(0, 1, 0)));
  }
  void publishSource(const GltfCameraPosePlan& plan) {
    copyPose(authored.nodes[0], plan.authoredAfter); copyPose(evaluated.nodes[0], plan.evaluatedAfter);
    graph.local = plan.localAfter; graph.world = plan.worldAfter; ++graph.revision;
  }
};
struct GuardResult { bool ready{}, exchanged{}; size_t allocations{}; };
GuardResult publish(AnimationPlayer& player, PreparedCameraNodeRestEdit& token, const Fixture& fixture) {
  GuardResult result;
  { material_publication_test::AllocationGuard deny(true);
    result.ready = player.cameraNodeRestEditMatches(token, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    result.exchanged = player.exchangeCameraNodeRestEdit(token, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    result.allocations = deny.allocations(); }
  return result;
}
} // namespace

TEST(GltfCameraNodeRest, CommitsOnlySelectedPoseAndPreservesCurrentWeightsMetadataClipsAndClock) {
  Fixture fixture; auto player = fixture.running(); auto plan = fixture.plan(); ASSERT_TRUE(plan.changed);
  const auto before = player; const auto sourceBefore = fixture.evaluated;
  auto prepared = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
  auto moved = std::move(prepared); EXPECT_FALSE(prepared); ASSERT_TRUE(moved);
  fixture.authored.nodes[0].weights = {.6, .4}; fixture.evaluated.nodes[0].weights = {.45, .55};
  // Model/player state can be maintained independently. Rest weights and
  // opaque rest metadata are outside pose ownership and must remain current.
  auto& rest = const_cast<AnimationLibrary&>(player.library()).restNodes[0];
  rest.weights = {.3, .7}; rest.extras_json_string = "new current rest metadata";
  const auto expectedRest = rest;
  const auto result = publish(player, moved, fixture);
  EXPECT_TRUE(result.ready); EXPECT_TRUE(result.exchanged); EXPECT_EQ(result.allocations, 0u); EXPECT_FALSE(moved);
  EXPECT_TRUE(GltfCameraNodePoseEqual(player.library().restNodes[0], plan.authoredAfter));
  auto expected = expectedRest; copyPose(expected, plan.authoredAfter);
  EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(player.library().restNodes[0], expected));
  EXPECT_EQ(player.library().restNodes[1], before.library().restNodes[1]);
  EXPECT_EQ(player.library().restNodes[2], before.library().restNodes[2]);
  EXPECT_EQ(player.library().restMaterials, before.library().restMaterials);
  EXPECT_EQ(player.library().restCameras, before.library().restCameras);
  EXPECT_EQ(player.library().clips, before.library().clips);
  EXPECT_EQ(player.selectedClip(), before.selectedClip()); EXPECT_DOUBLE_EQ(player.time(), before.time());
  EXPECT_DOUBLE_EQ(player.speed(), before.speed()); EXPECT_EQ(player.playing(), before.playing()); EXPECT_EQ(player.loop(), before.loop());
  EXPECT_TRUE(GltfCameraNodePoseEqual(fixture.evaluated.nodes[0], sourceBefore.nodes[0]));
  EXPECT_EQ(fixture.evaluated.nodes[0].weights, (std::vector<double>{.45, .55}));
  EXPECT_EQ(fixture.authored.nodes[0].weights, (std::vector<double>{.6, .4}));
}

TEST(GltfCameraNodeRest, OriginalPoseReplaysAtCurrentClockWithoutRewindingWeightSampling) {
  Fixture fixture; auto player = fixture.running(); const auto original = fixture.plan();
  auto first = player.prepareCameraNodeRestEdit(original, GltfCameraPoseView(fixture.authored), fixture.evaluated);
  ASSERT_TRUE(publish(player, first, fixture).exchanged); fixture.publishSource(original);
  for (unsigned cycle = 0; cycle < 2; ++cycle) {
    for (const auto side : {GltfCameraEditSide::Before, GltfCameraEditSide::After}) {
      player.selectClip(fixture.evaluated, cycle % 2); player.seek(fixture.evaluated, .75 + cycle * .25); player.play();
      const auto weights = fixture.evaluated.nodes[0].weights; const auto time = player.time();
      auto replay = RebaseGltfCameraViewPose(original, side, GltfCameraPoseView(fixture.authored),
          GltfCameraPoseView(fixture.evaluated), fixture.graph);
      auto prepared = player.prepareCameraNodeRestEdit(replay, GltfCameraPoseView(fixture.authored), fixture.evaluated);
      const auto result = publish(player, prepared, fixture);
      EXPECT_TRUE(result.ready); EXPECT_TRUE(result.exchanged); EXPECT_EQ(result.allocations, 0u);
      fixture.publishSource(replay);
      EXPECT_TRUE(GltfCameraNodePoseEqual(player.library().restNodes[0], replay.authoredAfter));
      EXPECT_EQ(fixture.evaluated.nodes[0].weights, weights); EXPECT_DOUBLE_EQ(player.time(), time);
      EXPECT_EQ(fixture.evaluated.nodes[1], fixture.authored.nodes[1]);
    }
  }
}

TEST(GltfCameraNodeRest, IndependentAuthoredAndEvaluatedNodeIdsAddressOnlyTheEvaluatedRestRow) {
  Fixture fixture;
  std::swap(fixture.evaluated.nodes[0], fixture.evaluated.nodes[1]);
  for (auto& clip : fixture.evaluated.animations) for (auto& raw : clip.channels)
    if (raw.target_node == 0) raw.target_node = 1;
  AnimationPlayer player(fixture.evaluated); player.selectClip(fixture.evaluated, 0); player.seek(fixture.evaluated, .5);
  fixture.graph.node = 1; fixture.graph.nodeIncarnation = 92;
  const auto plan = PlanGltfCameraViewPose(GltfCameraPoseView(fixture.authored), GltfCameraPoseView(fixture.evaluated),
      0, 1, fixture.graph, glm::lookAt(glm::vec3(2, 1, 8), glm::vec3(0), glm::vec3(0, 1, 0)));
  auto prepared = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
  const auto before = player.library().restNodes;
  const auto result = publish(player, prepared, fixture);
  EXPECT_TRUE(result.ready); EXPECT_TRUE(result.exchanged); EXPECT_EQ(result.allocations, 0u);
  EXPECT_TRUE(GltfCameraNodePoseEqual(player.library().restNodes[1], plan.authoredAfter));
  EXPECT_EQ(player.library().restNodes[0], before[0]); EXPECT_EQ(player.library().restNodes[2], before[2]);
}

TEST(GltfCameraNodeRest, ZeroMaterialAndZeroClipSourceStillPublishesOnlyCameraNodePose) {
  Fixture fixture; fixture.authored.animations.clear(); fixture.authored.materials.clear();
  fixture.authored.buffers.clear(); fixture.authored.bufferViews.clear(); fixture.authored.accessors.clear();
  fixture.evaluated = fixture.authored; AnimationPlayer player(fixture.evaluated);
  const auto plan = fixture.plan();
  auto prepared = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
  const auto weights = player.library().restNodes[0].weights;
  const auto result = publish(player, prepared, fixture);
  EXPECT_TRUE(result.ready); EXPECT_TRUE(result.exchanged); EXPECT_EQ(result.allocations, 0u);
  EXPECT_TRUE(player.library().clips.empty()); EXPECT_TRUE(player.library().restMaterials.empty());
  EXPECT_FALSE(player.selectedClip()); EXPECT_DOUBLE_EQ(player.time(), 0.);
  EXPECT_TRUE(GltfCameraNodePoseEqual(player.library().restNodes[0], plan.authoredAfter));
  EXPECT_EQ(player.library().restNodes[0].weights, weights);
}

TEST(GltfCameraNodeRest, RejectsUnsupportedSelectedMatrixQuaternionComponentsAndUnknownPointers) {
  for (const auto& path : {std::string("/nodes/0/rotation/0"),
      std::string("/nodes/0/matrix/0"), std::string("/nodes/0/translation/future"), std::string("/nodes/0")}) {
    SCOPED_TRACE(path);
    Fixture fixture;
    const auto time = floats(fixture.authored, {0, 2}, TINYGLTF_TYPE_SCALAR, 2);
    const bool pointer = path.front() == '/';
    const int type = path == "rotation" ? TINYGLTF_TYPE_VEC4 : (pointer ? TINYGLTF_TYPE_SCALAR : TINYGLTF_TYPE_VEC3);
    const auto values = path == "rotation" ? floats(fixture.authored, {0, 0, 0, 1, 0, 0, 0, 1}, type, 2) :
        (pointer ? floats(fixture.authored, {0, 1}, type, 2) : floats(fixture.authored, {1, 1, 1, 2, 2, 2}, type, 2));
    channel(fixture.authored, 2, time, values, pointer ? -1 : 0, path);
    fixture.evaluated = fixture.authored;
    EXPECT_THROW((void)fixture.plan(), std::invalid_argument);
  }
}

TEST(GltfCameraNodeRest, PartialTranslationRestEditSamplesCurrentClockAndReplaysWithoutRewinding) {
  Fixture f;
  channel(f.authored,0,0,floats(f.authored,{3,7},TINYGLTF_TYPE_SCALAR,2),-1,"/nodes/0/translation/0");
  f.evaluated=f.authored;auto player=f.running();
  const auto buffers=f.evaluated.buffers;const auto beforeWeights=f.evaluated.nodes[0].weights;
  f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;
  const auto original=f.plan();auto token=player.prepareCameraNodeRestEdit(original,GltfCameraPoseView(f.authored),f.evaluated);
  auto candidate=token.evaluatedNode();EXPECT_DOUBLE_EQ(candidate.translation[0],4.);
  EXPECT_EQ(candidate.translation[1],original.authoredAfter.translation[1]);EXPECT_EQ(candidate.weights,beforeWeights);
  EXPECT_TRUE(candidate.matrix.empty());const auto committed=publish(player,token,f);EXPECT_TRUE(committed.exchanged);EXPECT_EQ(committed.allocations,0u);
  copyPose(f.authored.nodes[0],original.authoredAfter);copyPose(f.evaluated.nodes[0],candidate);
  player.seek(f.evaluated,1.5);f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;++f.graph.revision;
  const auto undo=RebaseGltfCameraViewPose(original,GltfCameraEditSide::Before,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph,{},player.selectedClip());
  auto undoToken=player.prepareCameraNodeRestEdit(undo,GltfCameraPoseView(f.authored),f.evaluated);
  candidate=undoToken.evaluatedNode();EXPECT_DOUBLE_EQ(candidate.translation[0],6.);EXPECT_DOUBLE_EQ(candidate.translation[1],0.);EXPECT_DOUBLE_EQ(candidate.translation[2],6.);
  EXPECT_TRUE(publish(player,undoToken,f).exchanged);copyPose(f.authored.nodes[0],undo.authoredAfter);copyPose(f.evaluated.nodes[0],candidate);
  EXPECT_DOUBLE_EQ(player.time(),1.5);EXPECT_TRUE(player.playing());EXPECT_DOUBLE_EQ(player.speed(),1.25);
  player.clearClip(f.evaluated);f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;++f.graph.revision;
  const auto redo=RebaseGltfCameraViewPose(undo,GltfCameraEditSide::After,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph);
  auto redoToken=player.prepareCameraNodeRestEdit(redo,GltfCameraPoseView(f.authored),f.evaluated);
  candidate=redoToken.evaluatedNode();EXPECT_EQ(candidate.translation,original.authoredAfter.translation);
  EXPECT_TRUE(publish(player,redoToken,f).exchanged);EXPECT_FALSE(player.selectedClip());
  for(size_t i=0;i<buffers.size();++i) EXPECT_EQ(f.evaluated.buffers[i].data,buffers[i].data);
}

TEST(GltfCameraNodeRest, TrsOnlyCameraWithoutMeshDoesNotRequireMorphTargets) {
  Fixture f;f.authored.meshes.clear();f.authored.nodes[0].mesh=-1;f.authored.nodes[0].weights.clear();f.authored.animations.clear();
  const auto input=floats(f.authored,{0,2},TINYGLTF_TYPE_SCALAR,2);
  channel(f.authored,0,input,floats(f.authored,{0,0,6,4,0,6},TINYGLTF_TYPE_VEC3,2),0,"translation");
  f.evaluated=f.authored;AnimationPlayer player(f.authored);player.selectClip(f.evaluated,0);player.seek(f.evaluated,.5);
  f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;
  const auto plan=f.plan();auto token=player.prepareCameraNodeRestEdit(plan,GltfCameraPoseView(f.authored),f.evaluated);
  EXPECT_EQ(token.evaluatedNode().translation,(std::vector<double>{1,0,6}));EXPECT_EQ(token.evaluatedNode().mesh,-1);
  EXPECT_TRUE(publish(player,token,f).exchanged);EXPECT_EQ(player.library().restNodes[0].translation,plan.authoredAfter.translation);
}

TEST(GltfCameraNodeRest, InactiveTargetsUseAuthoredEndpointAndRejectConflictingEvaluatedChanges) {
  for(const bool clear:{false,true}) {
    Fixture f;channel(f.authored,0,0,floats(f.authored,{3,7},TINYGLTF_TYPE_SCALAR,2),-1,"/nodes/0/translation/0");
    f.evaluated=f.authored;auto player=f.running();f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;
    const auto original=f.plan();auto token=player.prepareCameraNodeRestEdit(original,GltfCameraPoseView(f.authored),f.evaluated);
    const auto candidate=token.evaluatedNode();ASSERT_TRUE(publish(player,token,f).exchanged);
    copyPose(f.authored.nodes[0],original.authoredAfter);copyPose(f.evaluated.nodes[0],candidate);
    if(clear) player.clearClip(f.evaluated);else player.selectClip(f.evaluated,1);
    f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;++f.graph.revision;
    const auto undo=RebaseGltfCameraViewPose(original,GltfCameraEditSide::Before,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph,{},player.selectedClip());
    EXPECT_NO_THROW((void)player.prepareCameraNodeRestEdit(undo,GltfCameraPoseView(f.authored),f.evaluated));
    const auto wrongMask=RebaseGltfCameraViewPose(original,GltfCameraEditSide::Before,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph,{},0);
    EXPECT_THROW((void)player.prepareCameraNodeRestEdit(wrongMask,GltfCameraPoseView(f.authored),f.evaluated),std::invalid_argument);
    f.evaluated.nodes[0].translation[0]+=1.;f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;
    EXPECT_THROW((void)RebaseGltfCameraViewPose(original,GltfCameraEditSide::Before,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph,{},player.selectedClip()),std::invalid_argument);
  }
}

TEST(GltfCameraNodeRest, ReplayPreservesUnownedBehaviorAuthoredEvaluatedScale) {
  Fixture f;f.authored.nodes[0].scale={1,1,1};f.authored.nodes[0].rotation={0,0,0,1};
  channel(f.authored,0,0,floats(f.authored,{3,7},TINYGLTF_TYPE_SCALAR,2),-1,"/nodes/0/translation/0");
  f.evaluated=f.authored;auto player=f.running();f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;
  const auto original=PlanGltfCameraViewPose(GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),0,0,f.graph,
      glm::lookAt(glm::vec3(2,1,8),glm::vec3(2,1,7),glm::vec3(0,1,0)));
  ASSERT_EQ(original.authoredAfter.scale,original.authoredBefore.scale);
  auto token=player.prepareCameraNodeRestEdit(original,GltfCameraPoseView(f.authored),f.evaluated);const auto candidate=token.evaluatedNode();
  ASSERT_TRUE(publish(player,token,f).exchanged);copyPose(f.authored.nodes[0],original.authoredAfter);copyPose(f.evaluated.nodes[0],candidate);
  player.selectClip(f.evaluated,1);f.evaluated.nodes[0].scale={2,3,4};
  f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;++f.graph.revision;
  const auto undo=RebaseGltfCameraViewPose(original,GltfCameraEditSide::Before,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph,{},player.selectedClip());
  auto prepared=player.prepareCameraNodeRestEdit(undo,GltfCameraPoseView(f.authored),f.evaluated);
  EXPECT_EQ(prepared.evaluatedNode().scale,(std::vector<double>{2,3,4}));
}

TEST(GltfCameraNodeRest, QuaternionAndReflectedScaleTracksKeepTheirCurrentSampleAndOriginalKeys) {
  for(const bool rotation:{true,false}) {
    Fixture f;f.authored.meshes.clear();f.authored.nodes[0].mesh=-1;f.authored.nodes[0].weights.clear();f.authored.animations.clear();
    const auto input=floats(f.authored,{0,2},TINYGLTF_TYPE_SCALAR,2);
    const auto output=rotation?floats(f.authored,{0,0,0,1,0,0,.70710677f,.70710677f},TINYGLTF_TYPE_VEC4,2):
        floats(f.authored,{-1,2,3,-2,4,6},TINYGLTF_TYPE_VEC3,2);
    channel(f.authored,0,input,output,0,rotation?"rotation":"scale");f.evaluated=f.authored;
    AnimationPlayer player(f.authored);player.selectClip(f.evaluated,0);player.seek(f.evaluated,.5);
    const auto expected=rotation?f.evaluated.nodes[0].rotation:f.evaluated.nodes[0].scale;const auto bytes=f.evaluated.buffers.back().data;
    f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;
    const auto plan=f.plan();auto token=player.prepareCameraNodeRestEdit(plan,GltfCameraPoseView(f.authored),f.evaluated);
    const auto candidate=token.evaluatedNode();EXPECT_EQ(rotation?candidate.rotation:candidate.scale,expected);
    EXPECT_TRUE(candidate.matrix.empty());EXPECT_TRUE(plan.authoredAfter.matrix.empty());
    EXPECT_TRUE(publish(player,token,f).exchanged);EXPECT_EQ(f.evaluated.buffers.back().data,bytes);
    EXPECT_DOUBLE_EQ(player.time(),.5);
  }
}

TEST(GltfCameraNodeRest, StepLinearAndCubicTranslationsRemainSampledAndRevealEditedRestOnClear) {
  for(const std::string interpolation:{"STEP","LINEAR","CUBICSPLINE"}) {
    Fixture f;f.authored.meshes.clear();f.authored.nodes[0].mesh=-1;f.authored.nodes[0].weights.clear();f.authored.animations.clear();
    const auto input=floats(f.authored,{0,2},TINYGLTF_TYPE_SCALAR,2);
    const auto output=interpolation=="CUBICSPLINE"?
        floats(f.authored,{0,0,0,0,0,6,1,0,0,1,0,0,4,0,6,0,0,0},TINYGLTF_TYPE_VEC3,6):
        floats(f.authored,{0,0,6,4,0,6},TINYGLTF_TYPE_VEC3,2);
    channel(f.authored,0,input,output,0,"translation");f.authored.animations[0].samplers[0].interpolation=interpolation;
    f.evaluated=f.authored;AnimationPlayer player(f.authored);player.selectClip(f.evaluated,0);player.seek(f.evaluated,.5);
    const auto expected=f.evaluated.nodes[0].translation;const auto bytes=f.evaluated.buffers.back().data;
    f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[0]);f.graph.world=f.graph.local;
    const auto plan=f.plan();auto token=player.prepareCameraNodeRestEdit(plan,GltfCameraPoseView(f.authored),f.evaluated);
    const auto candidate=token.evaluatedNode();EXPECT_EQ(candidate.translation,expected);
    EXPECT_TRUE(publish(player,token,f).exchanged);copyPose(f.authored.nodes[0],plan.authoredAfter);copyPose(f.evaluated.nodes[0],candidate);
    player.clearClip(f.evaluated);EXPECT_EQ(f.evaluated.nodes[0].translation,plan.authoredAfter.translation);
    EXPECT_EQ(f.evaluated.animations[0].samplers[0].interpolation,interpolation);EXPECT_EQ(f.evaluated.buffers.back().data,bytes);
  }
}

TEST(GltfCameraNodeRest, StaleSelectedRawEncodedAndDecodedProofsRejectReadinessAndDirectExchangeWithoutAllocation) {
  const std::vector<std::function<void(Fixture&, AnimationPlayer&)>> changes{
    [](auto& f, auto&) { f.evaluated.animations[0].channels[0].target_extras_json_string += " stale"; },
    [](auto& f, auto&) { f.evaluated.animations[1].samplers[0].interpolation = "STEP"; },
    [](auto& f, auto&) { f.evaluated.buffers[1].data[0] ^= 1; },
    [](auto& f, auto&) { f.evaluated.accessors[1].normalized = true; },
    [](auto& f, auto&) { f.evaluated.bufferViews[1].byteStride = 16; },
    [](auto& f, auto&) { f.evaluated.meshes[0].primitives[0].targets.resize(3); },
    [](auto& f, auto&) { f.evaluated.extensionsRequired.push_back("KHR_accessor_float64"); },
    [](auto&, auto& p) { const_cast<AnimationLibrary&>(p.library()).clips[1].tracks[0].values[0] += .1; },
    [](auto&, auto& p) { const_cast<AnimationLibrary&>(p.library()).restNodes[0].translation[0] += 1; },
    [](auto& f, auto&) { f.authored.nodes[0].extras_json_string += " stale"; },
    [](auto& f, auto&) { f.evaluated.cameras[0].perspective.yfov += .1; }
  };
  for (size_t i = 0; i < changes.size(); ++i) {
    SCOPED_TRACE(i);
    Fixture fixture; auto player = fixture.running(); auto plan = fixture.plan();
    auto prepared = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    changes[i](fixture, player); const auto before = player; const auto sourceBefore = fixture.evaluated;
    const auto result = publish(player, prepared, fixture);
    EXPECT_FALSE(result.ready); EXPECT_FALSE(result.exchanged); EXPECT_EQ(result.allocations, 0u); EXPECT_TRUE(prepared);
    EXPECT_EQ(player.library().restNodes, before.library().restNodes); EXPECT_EQ(player.library().clips, before.library().clips);
    EXPECT_EQ(fixture.evaluated.nodes, sourceBefore.nodes); EXPECT_EQ(fixture.evaluated.animations, sourceBefore.animations);
    EXPECT_DOUBLE_EQ(player.time(), before.time());
  }
}

TEST(GltfCameraNodeRest, ForeignPlayerChangedControlsAndCompetingPublicationKeepPreparedTokenAtomic) {
  for (unsigned change = 0; change < 6; ++change) {
    SCOPED_TRACE(change);
    Fixture fixture; auto player = fixture.running(); auto plan = fixture.plan();
    auto prepared = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    if (change == 0) player.pause();
    else if (change == 1) player.setLoop(true);
    else if (change == 2) player.setSpeed(-1.);
    else if (change == 3) player.seek(fixture.evaluated, .6);
    else if (change == 4) {
      auto other = player;
      const auto result = publish(other, prepared, fixture);
      EXPECT_FALSE(result.ready); EXPECT_FALSE(result.exchanged); EXPECT_EQ(result.allocations, 0u); EXPECT_TRUE(prepared);
      continue;
    } else {
      auto competitor = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
      ASSERT_TRUE(publish(player, competitor, fixture).exchanged);
    }
    const auto before = player.library().restNodes;
    const auto result = publish(player, prepared, fixture);
    EXPECT_FALSE(result.ready); EXPECT_FALSE(result.exchanged); EXPECT_EQ(result.allocations, 0u);
    EXPECT_EQ(player.library().restNodes, before); EXPECT_TRUE(prepared);
  }
}

TEST(GltfCameraNodeRest, NoopAndAbandonedPreparationsPreservePreviouslyReadyRestAndRevision) {
  Fixture fixture; auto player = fixture.running(); auto plan = fixture.plan();
  auto earlier = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
  { auto abandoned = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated); }
  const auto current = EvaluateAnimatedCamera(fixture.evaluated.cameras[0], fixture.graph.world, fixture.graph.viewportAspect);
  ASSERT_TRUE(current);
  const auto noop = PlanGltfCameraViewPose(GltfCameraPoseView(fixture.authored), GltfCameraPoseView(fixture.evaluated),
      0, 0, fixture.graph, current->view);
  ASSERT_FALSE(noop.changed);
  auto noChange = player.prepareCameraNodeRestEdit(noop, GltfCameraPoseView(fixture.authored), fixture.evaluated);
  const auto before = player.library().restNodes; const auto result = publish(player, noChange, fixture);
  EXPECT_TRUE(result.ready); EXPECT_TRUE(result.exchanged); EXPECT_EQ(result.allocations, 0u); EXPECT_FALSE(noChange);
  EXPECT_EQ(player.library().restNodes, before);
  { material_publication_test::AllocationGuard deny(true);
    const auto ready = player.cameraNodeRestEditMatches(earlier, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    deny.disable(); EXPECT_TRUE(ready); EXPECT_EQ(deny.allocations(), 0u); }
}

TEST(GltfCameraNodeRest, SelectedOnlyBudgetExcludesUnrelatedRestsGeometryAndStridedGaps) {
  Fixture fixture; auto& data = fixture.authored.buffers[1].data;
  data.assign(2 * 1024 * 1024, 83); fixture.authored.bufferViews[1].byteStride = 64;
  fixture.authored.bufferViews[1].byteLength = data.size(); fixture.authored.accessors[1].byteOffset = 4;
  const std::array<float, 4> values{.25f, .75f, .75f, .25f};
  for (size_t i = 0; i < values.size(); ++i) std::memcpy(data.data() + 4 + i * 64, &values[i], sizeof(float));
  fixture.authored.nodes[2].extras = V(std::string(2 * 1024 * 1024, 'r'));
  fixture.authored.materials[0].extras = V(std::string(2 * 1024 * 1024, 'm'));
  fixture.evaluated = fixture.authored; auto player = fixture.running();
  GltfCameraNodeAnimationEditLimits limits; limits.maxOwnedBytes = 256 * 1024;
  auto prepared = player.prepareCameraNodeRestEdit(fixture.plan(), GltfCameraPoseView(fixture.authored), fixture.evaluated, limits);
  EXPECT_LT(prepared.ownedBytes(), limits.maxOwnedBytes);
  fixture.evaluated.buffers[1].data[20] ^= 1;
  fixture.evaluated.buffers[2].data[0] ^= 1; // An unrelated node's encoded curve.
  const auto result = publish(player, prepared, fixture);
  EXPECT_TRUE(result.ready); EXPECT_TRUE(result.exchanged); EXPECT_EQ(result.allocations, 0u);
}

TEST(GltfCameraNodeRest, SparseWeightIndicesAndValuesRemainPartOfExactSelectedSourceProof) {
  Fixture fixture;
  tinygltf::Buffer indices; indices.data = {0, 1, 2, 3};
  const int buffer = int(fixture.authored.buffers.size()); fixture.authored.buffers.push_back(indices);
  tinygltf::BufferView view; view.buffer = buffer; view.byteLength = 4;
  const int indexView = int(fixture.authored.bufferViews.size()); fixture.authored.bufferViews.push_back(view);
  auto& accessor = fixture.authored.accessors[1]; accessor.bufferView = -1; accessor.sparse = {};
  accessor.sparse.isSparse = true; accessor.sparse.count = 4; accessor.sparse.indices.bufferView = indexView;
  accessor.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE; accessor.sparse.values.bufferView = 1;
  fixture.evaluated = fixture.authored; auto player = fixture.running();
  auto prepared = player.prepareCameraNodeRestEdit(fixture.plan(), GltfCameraPoseView(fixture.authored), fixture.evaluated);
  bool ready{}, stale{}, exchanged{}; size_t allocations{};
  { material_publication_test::AllocationGuard deny(true);
    ready = player.cameraNodeRestEditMatches(prepared, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    fixture.evaluated.buffers[size_t(buffer)].data[1] = 0;
    stale = player.cameraNodeRestEditMatches(prepared, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    exchanged = player.exchangeCameraNodeRestEdit(prepared, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    allocations = deny.allocations(); }
  EXPECT_TRUE(ready); EXPECT_FALSE(stale); EXPECT_FALSE(exchanged); EXPECT_EQ(allocations, 0u); EXPECT_TRUE(prepared);
}

TEST(GltfCameraNodeRest, EveryDeniedPreparationAndFiniteBudgetFailurePreserveEarlierReadyToken) {
  Fixture fixture; auto player = fixture.running(); auto plan = fixture.plan();
  auto earlier = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
  size_t calls{};
  { material_publication_test::AllocationGuard count(false);
    auto measured = player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated);
    calls = count.allocations(); }
  ASSERT_GT(calls, 0u); ASSERT_LT(calls, 4000u);
  for (size_t allowance = 0; allowance < calls; ++allowance) {
    SCOPED_TRACE(allowance);
    bool failed{};
    { material_publication_test::AllocationGuard deny(false, allowance);
      try { (void)player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated); }
      catch (const std::bad_alloc&) { failed = true; } }
    EXPECT_TRUE(failed);
    EXPECT_TRUE(player.cameraNodeRestEditMatches(earlier, GltfCameraPoseView(fixture.authored), fixture.evaluated));
  }
  for (unsigned field = 0; field < 6; ++field) {
    GltfCameraNodeAnimationEditLimits limits;
    if (field == 0) limits.maxOwnedBytes = 1;
    else if (field == 1) limits.maxDecodedBytes = 1;
    else if (field == 2) limits.maxMetadataValues = 1;
    else if (field == 3) limits.maxChannels = 1;
    else if (field == 4) limits.authoring.maxNodes = 1;
    else limits.maxOwnedBytes = UINT64_MAX;
    EXPECT_THROW((void)player.prepareCameraNodeRestEdit(plan, GltfCameraPoseView(fixture.authored), fixture.evaluated, limits), std::invalid_argument);
    EXPECT_TRUE(player.cameraNodeRestEditMatches(earlier, GltfCameraPoseView(fixture.authored), fixture.evaluated));
  }
}
