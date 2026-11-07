#include <Container/geometry/GltfCameraPoseAuthoring.h>
#include <Container/geometry/GltfAnimationRuntime.h>
#include <Container/common/CommonMath.h>

#include <gtest/gtest.h>
#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace container::geometry::gltf;
using V=tinygltf::Value;using O=V::Object;using A=V::Array;
void matrixNear(const glm::mat4& a,const glm::mat4& b,double tolerance=3e-5) {
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) EXPECT_NEAR(a[c][r],b[c][r],tolerance);
}
void copyMatrix(tinygltf::Node& node,const glm::mat4& m) {
  node.matrix.resize(16);for(int c=0;c<4;++c) for(int r=0;r<4;++r) node.matrix[size_t(c*4+r)]=m[c][r];
  node.translation.clear();node.rotation.clear();node.scale.clear();
}
void copyPose(tinygltf::Node& to,const tinygltf::Node& from) {
  to.matrix=from.matrix;to.translation=from.translation;to.rotation=from.rotation;to.scale=from.scale;
}
tinygltf::Camera lens(std::string name) {
  tinygltf::Camera c;c.type="perspective";c.name=std::move(name);c.perspective.yfov=.8;c.perspective.znear=.1;
  c.extras=V(O{{"negativeZero",V(-0.)}});c.extensions["VENDOR_camera"]=V(O{{"annotation",V(91)}});
  c.extras_json_string="exact lens raw metadata";return c;
}
struct Fixture {
  tinygltf::Model authored,evaluated;
  GltfCameraPoseGraphState graph;
  glm::mat4 desired=glm::lookAt(glm::vec3(12,-3,7),glm::vec3(11,-2.5,5),glm::normalize(glm::vec3(.4f,1,.2f)));
  Fixture() {
    authored.cameras={lens("Archive lens"),lens("Other archive lens")};
    evaluated.cameras={lens("Other evaluated lens"),lens("Evaluated lens")};
    authored.nodes.resize(3);evaluated.nodes.resize(4);authored.nodes[1].camera=0;evaluated.nodes[2].camera=1;
    authored.nodes[0].camera=0;evaluated.nodes[0].camera=1; // sibling uses the SAME definition
    auto& a=authored.nodes[1];a.name="Selected camera node";a.translation={.5,1,-2};a.rotation={0,0,std::sin(.3),std::cos(.3)};
    a.scale={1.5,.75,2};a.weights={.2,.8};a.children={2};a.lods={2};a.mesh=2;a.skin=1;a.light=0;
    a.extras=V(O{{"negativeZero",V(-0.)},{"opaqueIndex",V(16777217)}});
    a.extensions["VENDOR_node"]=V(O{{"annotation",V(A{V("keep"),V(23)})}});
    a.extras_json_string="archive raw extras";a.extensions_json_string="archive raw extensions";
    auto& e=evaluated.nodes[2];e=a;e.camera=1;e.children={3};e.weights={.3,.7};e.name="Independent evaluated node";
    e.extras_json_string="evaluated raw extras";
    graph.identity=71;graph.revision=4;graph.nodeCount=5;graph.node=3;graph.parent=1;
    graph.nodeIncarnation=83;graph.parentIncarnation=79;graph.siblingIndex=0;graph.viewportAspect=1.5;
    graph.parentWorld=glm::translate(glm::mat4(1),glm::vec3(-3,2,5))*glm::rotate(glm::mat4(1),.4f,glm::vec3(0,1,0))*
        glm::scale(glm::mat4(1),glm::vec3(1.2f,.8f,1.5f));
    graph.local=glm::translate(glm::mat4(1),glm::vec3(.5f,1,-2))*glm::rotate(glm::mat4(1),.6f,glm::vec3(0,0,1))*
        glm::scale(glm::mat4(1),glm::vec3(1.5f,.75f,2));
    graph.world=graph.parentWorld*graph.local;copyMatrix(e,graph.local);
  }
  GltfCameraPosePlan plan(const GltfCameraPoseAuthoringLimits& limits={}) const {
    return PlanGltfCameraViewPose(GltfCameraPoseView(authored),GltfCameraPoseView(evaluated),1,2,graph,desired,limits);
  }
  bool matches(const GltfCameraPosePlan& plan) const noexcept {
    return GltfCameraPoseEditMatches(plan,GltfCameraPoseView(authored),GltfCameraPoseView(evaluated),graph);
  }
  void publish(const GltfCameraPosePlan& plan) {
    copyPose(authored.nodes[1],plan.authoredAfter);copyPose(evaluated.nodes[2],plan.evaluatedAfter);
    graph.local=plan.localAfter;graph.world=plan.worldAfter;++graph.revision;
  }
};
void channel(tinygltf::Model& model,int32_t node,std::string path,bool pointer=false) {
  if(model.animations.empty()) model.animations.emplace_back();
  model.animations[0].samplers.emplace_back();tinygltf::AnimationChannel c;
  c.sampler=int(model.animations[0].samplers.size()-1);c.target_node=pointer?-1:node;c.target_path=pointer?"pointer":path;
  if(pointer) c.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::move(path))}});
  c.target_extras=V(O{{"exactMetadata",V(127)}});c.target_extensions_json_string="raw selected target";
  model.animations[0].channels.push_back(c);
}
} // namespace

TEST(GltfCameraPoseAuthoring, ParentedRolledPoseMatchesDesiredViewAndPreservesWorldMetricAndLensBytes) {
  Fixture f;const auto a=f.authored.nodes[1],e=f.evaluated.nodes[2];const auto aLens=f.authored.cameras[0],eLens=f.evaluated.cameras[1];
  const auto plan=f.plan();ASSERT_TRUE(plan.changed);EXPECT_EQ(plan.authoredNode,1);EXPECT_EQ(plan.evaluatedNode,2);
  EXPECT_EQ(plan.authoredCamera,0);EXPECT_EQ(plan.evaluatedCamera,1);EXPECT_TRUE(f.matches(plan));
  matrixNear(plan.viewAfter,f.desired);matrixNear(plan.worldAfter,f.graph.parentWorld*plan.localAfter);
  const auto oldGram=glm::transpose(glm::dmat3(f.graph.world))*glm::dmat3(f.graph.world);
  const auto newGram=glm::transpose(glm::dmat3(plan.worldAfter))*glm::dmat3(plan.worldAfter);
  for(int c=0;c<3;++c) for(int r=0;r<3;++r) EXPECT_NEAR(oldGram[c][r],newGram[c][r],3e-5);
  const auto expectedEye=glm::inverse(glm::dmat4(f.desired))[3];
  for(int r=0;r<3;++r) EXPECT_NEAR(plan.worldAfter[3][r],expectedEye[r],3e-5);
  EXPECT_EQ(plan.authoredAfter.camera,a.camera);EXPECT_EQ(plan.evaluatedAfter.camera,e.camera);
  EXPECT_EQ(plan.authoredAfter.children,a.children);EXPECT_EQ(plan.evaluatedAfter.children,e.children);
  EXPECT_EQ(plan.authoredAfter.mesh,a.mesh);EXPECT_EQ(plan.authoredAfter.skin,a.skin);EXPECT_EQ(plan.authoredAfter.light,a.light);
  EXPECT_EQ(plan.authoredAfter.weights,a.weights);EXPECT_EQ(plan.evaluatedAfter.weights,e.weights);
  EXPECT_EQ(plan.authoredAfter.extensions,a.extensions);EXPECT_EQ(plan.authoredAfter.extras_json_string,a.extras_json_string);
  EXPECT_EQ(plan.evaluatedAfter.extras_json_string,e.extras_json_string);
  EXPECT_EQ(std::bit_cast<uint64_t>(plan.authoredAfter.extras.Get("negativeZero").Get<double>()),std::bit_cast<uint64_t>(-0.));
  EXPECT_EQ(plan.authoredAfter.matrix.size(),16u);EXPECT_TRUE(plan.authoredAfter.translation.empty());EXPECT_TRUE(plan.authoredAfter.rotation.empty());
  EXPECT_TRUE(GltfCameraRecordsEqual(f.authored.cameras[0],aLens));EXPECT_TRUE(GltfCameraRecordsEqual(f.evaluated.cameras[1],eLens));
  EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(f.authored.nodes[1],a));EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(f.evaluated.nodes[2],e));
}

TEST(GltfCameraPoseAuthoring, ShearedMirroredParentKeepsReflectionScaleAndRollWithoutLossyTrs) {
  Fixture f;f.graph.parentWorld[1][0]+=.35f;f.graph.parentWorld[0]*=-1.f;f.graph.world=f.graph.parentWorld*f.graph.local;
  const auto before=glm::determinant(glm::dmat3(f.graph.world));ASSERT_LT(before,0);
  const auto plan=f.plan();matrixNear(plan.viewAfter,f.desired);
  EXPECT_LT(glm::determinant(glm::dmat3(plan.worldAfter)),0);
  const auto oldGram=glm::transpose(glm::dmat3(f.graph.world))*glm::dmat3(f.graph.world);
  const auto newGram=glm::transpose(glm::dmat3(plan.worldAfter))*glm::dmat3(plan.worldAfter);
  for(int c=0;c<3;++c) for(int r=0;r<3;++r) EXPECT_NEAR(oldGram[c][r],newGram[c][r],4e-5);
  EXPECT_EQ(plan.authoredAfter.matrix.size(),16u);EXPECT_TRUE(f.matches(plan));
}

TEST(GltfCameraPoseAuthoring, RootCameraAndExactCurrentViewAreSupportedWithoutOptionalPoseMaterialization) {
  Fixture f;f.graph.parent=UINT32_MAX;f.graph.parentIncarnation=0;f.graph.parentWorld=glm::mat4(1);f.graph.world=f.graph.local;
  auto root=f.plan();matrixNear(root.localAfter,root.worldAfter);
  const auto state=EvaluateAnimatedCamera(f.evaluated.cameras[1],f.graph.world,f.graph.viewportAspect);ASSERT_TRUE(state);
  f.desired=state->view;auto noop=f.plan();EXPECT_FALSE(noop.changed);
  EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(noop.authoredAfter,f.authored.nodes[1]));
  EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(noop.evaluatedAfter,f.evaluated.nodes[2]));matrixNear(noop.localAfter,f.graph.local,0);
}

TEST(GltfCameraPoseAuthoring, SourceSiblingCameraPosesAndLiveWeightsAreNotOverwrittenOrOwned) {
  Fixture f;const auto plan=f.plan();f.authored.nodes[0].translation={70,80,90};f.evaluated.nodes[0].matrix.assign(16,0);
  f.authored.nodes[1].weights={.4,.6};f.evaluated.nodes[2].weights={.9,.1};
  bool ready{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=f.matches(plan);attempts=deny.allocations();}
  EXPECT_TRUE(ready);EXPECT_EQ(attempts,0u);
  copyPose(f.authored.nodes[1],plan.authoredAfter);copyPose(f.evaluated.nodes[2],plan.evaluatedAfter);
  EXPECT_EQ(f.authored.nodes[1].weights,(std::vector<double>{.4,.6}));EXPECT_EQ(f.evaluated.nodes[2].weights,(std::vector<double>{.9,.1}));
  EXPECT_EQ(f.authored.nodes[0].translation,(std::vector<double>{70,80,90}));
}

TEST(GltfCameraPoseAuthoring, SelectedMatrixAndUnknownPosePointersStillRejectAcrossEveryClip) {
  for(const auto* property:{"matrix","futurePose"}) {
    Fixture f;channel(f.authored,1,property);EXPECT_THROW((void)f.plan(),std::invalid_argument);
    Fixture pointerFixture;channel(pointerFixture.evaluated,-1,std::string("/nodes/2/")+property,true);
    EXPECT_THROW((void)pointerFixture.plan(),std::invalid_argument);
  }
  Fixture f;channel(f.evaluated,0,"translation");channel(f.authored,0,"rotation"); // other nodes/parents may animate
  channel(f.authored,1,"weights");channel(f.evaluated,-1,"/nodes/2/weights/0",true);
  auto plan=f.plan();ASSERT_EQ(plan.authoredAnimationBindings.size(),1u);ASSERT_EQ(plan.evaluatedAnimationBindings.size(),1u);
  EXPECT_EQ(plan.authoredAnimationBindings[0].clip,0u);EXPECT_EQ(plan.evaluatedAnimationBindings[0].channel,1u);EXPECT_TRUE(f.matches(plan));
  f.evaluated.animations.emplace_back();f.evaluated.animations.back().samplers.emplace_back();tinygltf::AnimationChannel target;
  target.target_node=2;target.target_path="translation";target.sampler=0;f.evaluated.animations.back().channels.push_back(target);
  EXPECT_THROW((void)f.plan(),std::invalid_argument);
}

TEST(GltfCameraPoseAuthoring, AnimatedTrsDefaultsRetainTrsAndCurrentTargetComponents) {
  for(const auto* property:{"translation","rotation","scale"}) {
    Fixture f;copyPose(f.evaluated.nodes[2],f.authored.nodes[1]);
    f.graph.parentWorld=glm::mat4(1);f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[2]);f.graph.world=f.graph.local;
    channel(f.authored,1,property);channel(f.evaluated,2,property);
    const auto plan=f.plan();ASSERT_TRUE(plan.changed);EXPECT_TRUE(plan.authoredAfter.matrix.empty());EXPECT_TRUE(plan.evaluatedAfter.matrix.empty());
    matrixNear(EvaluateAnimatedNodeLocalTransform(plan.authoredAfter),plan.localAfter,0);
    ASSERT_EQ(plan.evaluatedAnimationBindings.size(),1u);EXPECT_TRUE(f.matches(plan));
    if(std::string(property)=="translation") EXPECT_EQ(plan.evaluatedAfter.translation,f.evaluated.nodes[2].translation);
    if(std::string(property)=="rotation") EXPECT_EQ(plan.evaluatedAfter.rotation,f.evaluated.nodes[2].rotation);
    if(std::string(property)=="scale") EXPECT_EQ(plan.evaluatedAfter.scale,f.evaluated.nodes[2].scale);
  }
}

TEST(GltfCameraPoseAuthoring, PartialPointerPreservesOnlyItsAnimatedComponent) {
  Fixture f;copyPose(f.evaluated.nodes[2],f.authored.nodes[1]);
  f.graph.parentWorld=glm::mat4(1);f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[2]);f.graph.world=f.graph.local;
  channel(f.authored,-1,"/nodes/1/translation/0",true);channel(f.evaluated,-1,"/nodes/2/translation/0",true);
  const auto plan=f.plan();ASSERT_EQ(plan.evaluatedAnimationBindings[0].element,0u);
  EXPECT_EQ(plan.evaluatedAfter.translation[0],f.evaluated.nodes[2].translation[0]);
  EXPECT_EQ(plan.evaluatedAfter.translation[1],plan.authoredAfter.translation[1]);
  EXPECT_EQ(plan.evaluatedAfter.translation[2],plan.authoredAfter.translation[2]);EXPECT_TRUE(f.matches(plan));
}

TEST(GltfCameraPoseAuthoring, AnimatedNodeRejectsGenuineLocalShearWithoutChangingSource) {
  Fixture f;copyPose(f.evaluated.nodes[2],f.authored.nodes[1]);
  f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[2]);f.graph.world=f.graph.parentWorld*f.graph.local;
  channel(f.authored,1,"translation");channel(f.evaluated,2,"translation");const auto before=f.authored.nodes[1];
  EXPECT_THROW((void)f.plan(),std::invalid_argument);EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(before,f.authored.nodes[1]));
}

TEST(GltfCameraPoseAuthoring, TrueAnimatedRestNoopPreservesOptionalFieldsAndSignedZero) {
  Fixture f;f.authored.nodes[1].translation[0]=-0.;copyPose(f.evaluated.nodes[2],f.authored.nodes[1]);
  f.graph.parentWorld=glm::mat4(1);f.graph.local=EvaluateAnimatedNodeLocalTransform(f.evaluated.nodes[2]);f.graph.world=f.graph.local;
  channel(f.authored,1,"rotation");channel(f.evaluated,2,"rotation");
  const auto view=EvaluateAnimatedCamera(f.evaluated.cameras[1],f.graph.world,f.graph.viewportAspect);ASSERT_TRUE(view);
  const auto plan=PlanGltfCameraViewPose(GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),1,2,f.graph,view->view);
  EXPECT_FALSE(plan.changed);EXPECT_TRUE(GltfCameraNodePoseEqual(plan.authoredAfter,f.authored.nodes[1]));
  EXPECT_TRUE(std::signbit(plan.authoredAfter.translation[0]));EXPECT_TRUE(f.matches(plan));
}

TEST(GltfCameraPoseAuthoring, MalformedPointerAndAmbiguousRawTargetSchemasFailClosed) {
  for(const auto* path:{"/nodes","/nodes/2","/nodes/02/weights","/nodes/999/weights","/nodes/2/weights/00","/nodes/2/weights/0/future","nodes/2/weights"}) {
    Fixture f;channel(f.evaluated,-1,path,true);EXPECT_THROW((void)f.plan(),std::invalid_argument);
  }
  Fixture f;channel(f.authored,1,"weights");
  f.authored.animations[0].channels[0].target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::string("/nodes/1/translation"))}});
  EXPECT_THROW((void)f.plan(),std::invalid_argument);
}

TEST(GltfCameraPoseAuthoring, InvalidGraphIncarnationsStaleWorldAndNonRigidViewsRejectBeforeMutation) {
  const std::array<std::function<void(Fixture&)>,11> mutate{
      [](auto& f){f.graph.nodeIncarnation=0;},[](auto& f){f.graph.parentIncarnation=0;},
      [](auto& f){f.graph.nodeCount=0;},[](auto& f){f.graph.parent=f.graph.node;},
      [](auto& f){f.graph.world[3][0]+=1;},[](auto& f){f.graph.viewportAspect=0;},
      [](auto& f){f.graph.local[0][0]=std::numeric_limits<float>::infinity();},
      [](auto& f){f.desired[0][0]*=2;},[](auto& f){f.desired[0]*=-1;},
      [](auto& f){f.desired[2][3]=-1;},[](auto& f){f.desired[3][0]=std::numeric_limits<float>::quiet_NaN();}};
  for(const auto& change:mutate) {Fixture f;change(f);EXPECT_THROW((void)f.plan(),std::invalid_argument);EXPECT_FALSE(f.authored.nodes[1].translation.empty());}
}

TEST(GltfCameraPoseAuthoring, SingularIllConditionedParentsAndUnrepresentableLocalCoordinatesReject) {
  Fixture f;f.graph.parentWorld[0]=glm::vec4(0);f.graph.world=f.graph.parentWorld*f.graph.local;
  EXPECT_THROW((void)f.plan(),std::invalid_argument);
  Fixture thin;thin.graph.parentWorld=glm::scale(glm::mat4(1),glm::vec3(1e-10f,1,1));thin.graph.world=thin.graph.parentWorld*thin.graph.local;
  EXPECT_THROW((void)thin.plan(),std::invalid_argument);
  Fixture overflow;overflow.graph.parentWorld=glm::scale(glm::mat4(1),glm::vec3(1e-20f));
  overflow.graph.local=glm::scale(glm::mat4(1),glm::vec3(1e20f));overflow.graph.world=overflow.graph.parentWorld*overflow.graph.local;
  overflow.desired=glm::mat4(1);overflow.desired[3][0]=-1e30f;EXPECT_THROW((void)overflow.plan(),std::invalid_argument);
}

TEST(GltfCameraPoseAuthoring, MalformedSelectedNodeCameraAndMetadataRecordsReject) {
  Fixture f;f.authored.nodes[1].camera=-1;EXPECT_THROW((void)f.plan(),std::invalid_argument);
  Fixture shape;shape.evaluated.nodes[2].translation={1,2,3};EXPECT_THROW((void)shape.plan(),std::invalid_argument);
  Fixture quaternion;quaternion.authored.nodes[1].rotation={0,0,0,0};EXPECT_THROW((void)quaternion.plan(),std::invalid_argument);
  Fixture metadata;metadata.authored.nodes[1].extras=V(7);metadata.authored.nodes[1].extras.Get<A>().push_back(V(1));
  EXPECT_THROW((void)metadata.plan(),std::invalid_argument);
  Fixture camera;camera.evaluated.cameras[1].perspective.yfov=0;EXPECT_THROW((void)camera.plan(),std::invalid_argument);
}

TEST(GltfCameraPoseAuthoring, ExactSourceMetadataLensAndRawWeightTargetsGuardNoallocPublication) {
  const std::array<std::function<void(Fixture&)>,8> mutate{
      [](auto& f){f.authored.nodes[1].translation[0]=std::nextafter(.5,1.);},
      [](auto& f){f.authored.nodes[1].extras.Get<O>()["negativeZero"]=V(0.);},
      [](auto& f){f.authored.nodes[1].extras.Get<O>()["opaqueIndex"]=V(16777217.);},
      [](auto& f){f.evaluated.nodes[2].extras_json_string+=" stale";},
      [](auto& f){f.evaluated.nodes[2].camera=0;},
      [](auto& f){f.authored.cameras[0].perspective.zfar=-0.;},
      [](auto& f){f.evaluated.cameras[1].extensions_json_string+=" stale";},
      [](auto& f){f.evaluated.animations[0].channels[0].target_extras.Get<O>()["exactMetadata"]=V(127.);}};
  for(const auto& change:mutate) {
    Fixture f;channel(f.evaluated,2,"weights");auto plan=f.plan();change(f);bool ready{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=f.matches(plan);attempts=deny.allocations();}
    EXPECT_FALSE(ready);EXPECT_EQ(attempts,0u);
  }
}

TEST(GltfCameraPoseAuthoring, EveryPublicPlanFieldAndActualGraphSnapshotRequiresPrivateReceipt) {
  Fixture f;const auto original=f.plan();EXPECT_TRUE(f.matches(GltfCameraPosePlan(original)));EXPECT_FALSE(f.matches({}));
  const std::array<std::function<void(GltfCameraPosePlan&)>,10> change{
      [](auto& p){p.authoredNode=0;},[](auto& p){p.evaluatedCamera=0;},[](auto& p){p.authoredAfter.matrix[0]+=1;},
      [](auto& p){p.evaluatedBefore.extras_json_string+=" stale";},[](auto& p){p.graphBefore.parentIncarnation++;},
      [](auto& p){p.desiredView[3][0]+=1;},[](auto& p){p.worldAfter[3][0]+=1;},
      [](auto& p){p.viewAfter[0][0]+=1;},[](auto& p){p.changed=false;},[](auto& p){p.peakOwnedBytes++;}};
  for(const auto& mutation:change) {auto p=original;mutation(p);bool ready{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=f.matches(p);attempts=deny.allocations();}
    EXPECT_FALSE(ready);EXPECT_EQ(attempts,0u);
  }
  auto graph=f.graph;++graph.revision;EXPECT_FALSE(GltfCameraPoseEditMatches(original,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),graph));
}

TEST(GltfCameraPoseAuthoring, ReplayRestoresExactOptionalTrsAndMatrixWithTodaysParentMetadataWeightsAndLens) {
  Fixture f;const auto original=f.plan();const auto aBefore=f.authored.nodes[1],eBefore=f.evaluated.nodes[2];f.publish(original);
  f.authored.nodes[1].name="Current authored label";f.evaluated.nodes[2].weights={.6,.4};f.evaluated.cameras[1].perspective.yfov=1.1;
  f.graph.parentWorld=glm::translate(glm::mat4(1),glm::vec3(8,2,1))*f.graph.parentWorld;f.graph.world=f.graph.parentWorld*f.graph.local;++f.graph.revision;
  const auto parentNow=f.graph.parentWorld;
  auto undo=RebaseGltfCameraViewPose(original,GltfCameraEditSide::Before,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph);
  EXPECT_TRUE(GltfCameraNodePoseEqual(undo.authoredAfter,aBefore));EXPECT_TRUE(GltfCameraNodePoseEqual(undo.evaluatedAfter,eBefore));
  EXPECT_EQ(undo.authoredAfter.name,"Current authored label");EXPECT_EQ(undo.evaluatedAfter.weights,(std::vector<double>{.6,.4}));
  matrixNear(undo.worldAfter,parentNow*original.graphBefore.local);EXPECT_TRUE(f.matches(undo));f.publish(undo);
  auto redo=RebaseGltfCameraViewPose(undo,GltfCameraEditSide::After,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph);
  EXPECT_TRUE(GltfCameraNodePoseEqual(redo.authoredAfter,original.authoredAfter));EXPECT_EQ(redo.authoredAfter.name,"Current authored label");
  EXPECT_EQ(f.evaluated.cameras[1].perspective.yfov,1.1);matrixNear(redo.worldAfter,parentNow*original.localAfter);EXPECT_TRUE(f.matches(redo));
}

TEST(GltfCameraPoseAuthoring, ReplayRejectsCutReusedNodeParentAndWrongOwnedEndpoint) {
  const std::array<std::function<void(Fixture&)>,6> changes{
      [](auto& f){++f.graph.nodeIncarnation;},[](auto& f){++f.graph.parentIncarnation;},
      [](auto& f){++f.graph.siblingIndex;},[](auto& f){f.graph.parent=0;},
      [](auto& f){f.authored.nodes[1].matrix[12]+=1;},
      [](auto& f){f.graph.local[3][0]+=1;f.graph.world=f.graph.parentWorld*f.graph.local;}};
  for(const auto& mutation:changes) {Fixture f;const auto plan=f.plan();f.publish(plan);mutation(f);
    EXPECT_THROW((void)RebaseGltfCameraViewPose(plan,GltfCameraEditSide::Before,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph),std::invalid_argument);
  }
  Fixture f;auto plan=f.plan();f.publish(plan);plan.authoredBefore.translation[0]+=1;
  EXPECT_THROW((void)RebaseGltfCameraViewPose(plan,GltfCameraEditSide::Before,GltfCameraPoseView(f.authored),GltfCameraPoseView(f.evaluated),f.graph),std::invalid_argument);
}

TEST(GltfCameraPoseAuthoring, FiniteBudgetsCoverSelectedSnapshotsRawTargetsAndMathLimits) {
  Fixture f;channel(f.evaluated,2,"weights");const auto plan=f.plan();GltfCameraPoseAuthoringLimits exactBudget;exactBudget.maxOwnedBytes=plan.peakOwnedBytes;
  EXPECT_TRUE(f.matches(f.plan(exactBudget)));
  const std::array<std::function<void(GltfCameraPoseAuthoringLimits&)>,10> tighten{
      [](auto& l){l.maxOwnedBytes=1;},[](auto& l){l.maxMetadataValues=1;},[](auto& l){l.maxMetadataDepth=0;},
      [](auto& l){l.maxNodes=1;},[](auto& l){l.maxCameraRecords=1;},[](auto& l){l.maxGraphNodes=1;},
      [](auto& l){l.maxAnimationClips=0;},[](auto& l){l.maxAnimationChannels=0;},
      [](auto& l){l.maxConditionNumber=std::numeric_limits<double>::infinity();},[](auto& l){l.maxAngularError=0;}};
  for(const auto& change:tighten) {GltfCameraPoseAuthoringLimits limits;change(limits);EXPECT_THROW((void)f.plan(limits),std::invalid_argument);}
  EXPECT_GT(GltfCameraPoseNodeStorageBytes(f.authored.nodes[1]),sizeof(tinygltf::Node));
  V raw(O{{"signedZero",V(-0.)},{"annotation",V(17)}});auto altered=raw;altered.Get<O>()["annotation"]=V(17.);
  tinygltf::Light light;light.type="spot";light.color={1,.5,.25};light.extras=raw;light.spot.extras_json_string="raw spot metadata";
  auto changed=light;changed.spot.innerConeAngle=-0.;
  EXPECT_GT(GltfAuthoringValueStorageBytes(raw),sizeof(V));EXPECT_GT(GltfCameraPoseLightStorageBytes(light),sizeof(light));
  bool same{},different{},sameLight{},differentLight{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);same=GltfCameraPoseValuesMatch(raw,raw);different=GltfCameraPoseValuesMatch(raw,altered);
    sameLight=GltfCameraPoseLightRecordsMatch(light,light);differentLight=GltfCameraPoseLightRecordsMatch(light,changed);attempts=deny.allocations();}
  EXPECT_TRUE(same);EXPECT_FALSE(different);EXPECT_TRUE(sameLight);EXPECT_FALSE(differentLight);EXPECT_EQ(attempts,0u);
}

TEST(GltfCameraPoseAuthoring, UnrelatedGeometryAndSiblingMetadataNeverEnterSelectedOwnedStorage) {
  Fixture f;const auto small=f.plan();f.authored.buffers.emplace_back();f.authored.buffers[0].data.resize(4*1024*1024,71);
  f.evaluated.images.emplace_back();f.evaluated.images[0].image.resize(4*1024*1024,73);
  f.authored.nodes[0].extras=V(std::string(1024*1024,'x'));f.evaluated.cameras[0].extras=V(std::string(1024*1024,'y'));
  auto plan=f.plan();EXPECT_EQ(plan.peakOwnedBytes,small.peakOwnedBytes);EXPECT_TRUE(f.matches(plan));
}

TEST(GltfCameraPoseAuthoring, EveryDeniedAllocationLeavesSourcesGraphAndPreviousProofUnchanged) {
  Fixture f;channel(f.evaluated,2,"weights");const auto before=f.authored.nodes[1],evaluated=f.evaluated.nodes[2];const auto proof=f.plan();size_t allocations{};
  {material_publication_test::AllocationGuard count(false);auto plan=f.plan();count.disable();allocations=count.allocations();}
  ASSERT_GT(allocations,0u);ASSERT_LT(allocations,3000u);
  for(size_t allowance=0;allowance<allocations;++allowance) {bool denied{};
    {material_publication_test::AllocationGuard guard(false,allowance);try {(void)f.plan();} catch(const std::bad_alloc&) {denied=true;}}
    EXPECT_TRUE(denied);EXPECT_TRUE(f.matches(proof));
  }
  EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(before,f.authored.nodes[1]));EXPECT_TRUE(GltfCameraPoseNodeRecordsEqual(evaluated,f.evaluated.nodes[2]));
}
