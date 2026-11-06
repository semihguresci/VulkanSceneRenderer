#include <Container/utility/GltfAnimationControls.h>

#include <gtest/gtest.h>
#include <imgui.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <array>
#include <functional>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using namespace container::ui;
using Target=container::scene::CameraViewPoseTarget;
using Projection=container::geometry::gltf::GltfCameraProjection;
using Property=container::geometry::gltf::GltfCameraProperty;
static_assert(std::is_trivially_copyable_v<CameraViewPoseRequest>);

CameraViewPoseSnapshot snapshot() {
  CameraViewPoseSnapshot s;s.target.camera={11,12,7,13,9,14,2,3,4,5};s.target.nodeIncarnation=19;
  s.viewRevision=23;s.viewportActive=true;s.destinationName="Main camera node";return s;
}
GltfAnimationControls controls() {
  GltfAnimationControls c;c.cameraNode=-1;
  if(!SetCameraViewPoseSnapshot(c,snapshot())) throw std::runtime_error("Invalid camera pose control fixture");
  return c;
}
CameraProjectionSnapshot lensSnapshot(const Target& target) {
  CameraProjectionSnapshot s;s.target=target.camera;s.type=Projection::Perspective;
  s.values={std::numbers::pi/3,0,.1,0,0,0,0,0};return s;
}
void expectSnapshot(const CameraViewPoseSnapshot& actual,const CameraViewPoseSnapshot& expected) {
  EXPECT_EQ(actual.target,expected.target);EXPECT_EQ(actual.viewRevision,expected.viewRevision);
  EXPECT_EQ(actual.viewportActive,expected.viewportActive);EXPECT_EQ(actual.destinationName,expected.destinationName);
  EXPECT_EQ(actual.blockReason,expected.blockReason);
}
} // namespace

TEST(CameraViewPoseControls, ViewportSelectionKeepsTheIndependentWeakDestinationAndSeparateError) {
  auto c=controls();const auto pose=*c.cameraViewPose;
  ASSERT_TRUE(SetCameraProjectionSnapshot(c,lensSnapshot(pose.target)));
  c.cameraNode=pose.target.camera.sourceNode;c.cameraViewPoseError="Previous pose error";
  EXPECT_FALSE(QueueCameraViewPose(c,pose.target,pose.viewRevision));
  c.cameraNode=-1;ASSERT_TRUE(SetCameraProjectionSnapshot(c,std::nullopt));
  ASSERT_TRUE(c.cameraViewPose);expectSnapshot(*c.cameraViewPose,pose);
  EXPECT_EQ(c.cameraViewPoseError,"Previous pose error");EXPECT_FALSE(c.cameraProjection);
  ASSERT_TRUE(QueueCameraViewPose(c,pose.target,pose.viewRevision));
  EXPECT_EQ(c.cameraNode,-1);EXPECT_EQ(c.requestedCameraViewPose->target,pose.target);
  EXPECT_EQ(c.requestedCameraViewPose->viewRevision,pose.viewRevision);
  EXPECT_FALSE(c.requestedCameraProjection);
}

TEST(CameraViewPoseControls, InvalidOrOversizedSnapshotsPreserveTheDisplayedAndPendingIntent) {
  auto c=controls();const auto before=*c.cameraViewPose;ASSERT_TRUE(QueueCameraViewPose(c,before.target,before.viewRevision));
  c.cameraViewPoseError="Keep the error";const auto request=*c.requestedCameraViewPose;
  const std::array<std::function<void(CameraViewPoseSnapshot&)>,9> mutate{
      [](auto& s) {s.target.camera.asset=0;},[](auto& s) {s.target.camera.sourceNamespace=0;},
      [](auto& s) {s.target.camera.incarnation=0;},[](auto& s) {s.target.camera.sourceNode=-1;},
      [](auto& s) {s.target.camera.graphNode=UINT32_MAX;},[](auto& s) {s.target.nodeIncarnation=0;},
      [](auto& s) {s.viewRevision=0;},[](auto& s) {s.destinationName.assign(4097,'n');},
      [](auto& s) {s.blockReason.assign(513,'r');}};
  for(const auto& change:mutate) {
    auto invalid=before;change(invalid);EXPECT_FALSE(SetCameraViewPoseSnapshot(c,invalid));
    ASSERT_TRUE(c.cameraViewPose);expectSnapshot(*c.cameraViewPose,before);
    ASSERT_TRUE(c.requestedCameraViewPose);EXPECT_EQ(c.requestedCameraViewPose->target,request.target);
    EXPECT_EQ(c.requestedCameraViewPose->viewRevision,request.viewRevision);EXPECT_EQ(c.cameraViewPoseError,"Keep the error");
  }
  auto boundary=before;boundary.destinationName.assign(4096,'n');boundary.blockReason.assign(512,'r');
  EXPECT_TRUE(SetCameraViewPoseSnapshot(c,boundary));
}

TEST(CameraViewPoseControls, QueueRejectsEachStaleTargetAndViewWitnessWithoutReplacingTheRequest) {
  auto c=controls();const auto s=*c.cameraViewPose;
  const std::array<std::function<void(Target&)>,11> mutate{
      [](auto& t) {++t.camera.asset;},[](auto& t) {++t.camera.sourceNamespace;},[](auto& t) {++t.camera.revision;},
      [](auto& t) {++t.camera.graphIdentity;},[](auto& t) {++t.camera.graphRevision;},[](auto& t) {++t.camera.incarnation;},
      [](auto& t) {++t.camera.graphNode;},[](auto& t) {++t.camera.sourceNode;},
      [](auto& t) {++t.camera.authoredCamera;},[](auto& t) {++t.camera.evaluatedCamera;},[](auto& t) {++t.nodeIncarnation;}};
  for(const auto& change:mutate) {auto stale=s.target;change(stale);EXPECT_FALSE(QueueCameraViewPose(c,stale,s.viewRevision));}
  EXPECT_FALSE(QueueCameraViewPose(c,s.target,0));EXPECT_FALSE(QueueCameraViewPose(c,s.target,s.viewRevision+1));
  EXPECT_FALSE(c.requestedCameraViewPose);ASSERT_TRUE(QueueCameraViewPose(c,s.target,s.viewRevision));
  EXPECT_FALSE(QueueCameraViewPose(c,s.target,s.viewRevision));
  EXPECT_EQ(c.requestedCameraViewPose->target,s.target);EXPECT_EQ(c.requestedCameraViewPose->viewRevision,s.viewRevision);
}

TEST(CameraViewPoseControls, RefreshNeverRetargetsAPendingWriteAndLosingDestinationClearsOnlyPoseState) {
  auto c=controls();const auto before=*c.cameraViewPose;ASSERT_TRUE(QueueCameraViewPose(c,before.target,before.viewRevision));
  auto next=before;++next.viewRevision;++next.target.camera.graphRevision;++next.target.nodeIncarnation;
  ASSERT_TRUE(SetCameraViewPoseSnapshot(c,next));expectSnapshot(*c.cameraViewPose,next);
  ASSERT_TRUE(c.requestedCameraViewPose);EXPECT_EQ(c.requestedCameraViewPose->target,before.target);
  EXPECT_EQ(c.requestedCameraViewPose->viewRevision,before.viewRevision);
  EXPECT_FALSE(QueueCameraViewPose(c,next.target,next.viewRevision));
  c.cameraViewPoseError="Pose failed";c.cameraProjectionError="Lens error";c.requestedTime=0.;
  ASSERT_TRUE(SetCameraViewPoseSnapshot(c,std::nullopt));
  EXPECT_FALSE(c.cameraViewPose);EXPECT_FALSE(c.requestedCameraViewPose);EXPECT_TRUE(c.cameraViewPoseError.empty());
  EXPECT_EQ(c.cameraProjectionError,"Lens error");ASSERT_TRUE(c.requestedTime);EXPECT_DOUBLE_EQ(*c.requestedTime,0.);
}

TEST(CameraViewPoseControls, OnlyFreeViewportModeAndAnUnblockedDestinationCanQueue) {
  auto c=controls();auto s=*c.cameraViewPose;
  s.viewportActive=false;ASSERT_TRUE(SetCameraViewPoseSnapshot(c,s));EXPECT_FALSE(QueueCameraViewPose(c,s.target,s.viewRevision));
  s.viewportActive=true;ASSERT_TRUE(SetCameraViewPoseSnapshot(c,s));c.cameraNode=s.target.camera.sourceNode;
  EXPECT_FALSE(QueueCameraViewPose(c,s.target,s.viewRevision));c.cameraNode=-1;
  s.blockReason="The selected node has an animated translation.";ASSERT_TRUE(SetCameraViewPoseSnapshot(c,s));
  EXPECT_FALSE(QueueCameraViewPose(c,s.target,s.viewRevision));
  ASSERT_TRUE(SetCameraViewPoseSnapshot(c,std::nullopt));EXPECT_FALSE(QueueCameraViewPose(c,s.target,s.viewRevision));
}

TEST(CameraViewPoseControls, LensTimelineAndSelectionQueuesExcludePoseWritesInBothDirections) {
  const std::array<std::function<void(GltfAnimationControls&)>,6> conflict{
      [](auto& c) {c.requestedCameraNode=-1;},[](auto& c) {c.requestedClip=0;},[](auto& c) {c.requestedTime=0.;},
      [](auto& c) {c.requestedSpeed=.5;},[](auto& c) {c.requestedPlaying=false;},[](auto& c) {c.requestedLoop=false;}};
  for(const auto& change:conflict) {
    auto c=controls();const auto s=*c.cameraViewPose;change(c);
    EXPECT_FALSE(QueueCameraViewPose(c,s.target,s.viewRevision));EXPECT_FALSE(c.requestedCameraViewPose);
  }
  auto c=controls();const auto s=*c.cameraViewPose;ASSERT_TRUE(SetCameraProjectionSnapshot(c,lensSnapshot(s.target)));
  const container::geometry::gltf::GltfCameraProjectionPatch patch{{},{{Property::PerspectiveNear,.25}}};
  ASSERT_TRUE(QueueCameraViewPose(c,s.target,s.viewRevision));
  EXPECT_FALSE(QueueCameraProjectionEdit(c,s.target.camera,patch));EXPECT_FALSE(QueueCameraProjectionDraft(c));
  EXPECT_FALSE(SetCameraProjectionDraft(c,c.cameraProjectionDraft));EXPECT_FALSE(c.requestedCameraProjection);
  c.requestedCameraViewPose.reset();ASSERT_TRUE(QueueCameraProjectionEdit(c,s.target.camera,patch));
  EXPECT_FALSE(QueueCameraViewPose(c,s.target,s.viewRevision));ASSERT_TRUE(c.requestedCameraProjection);
  EXPECT_EQ(c.requestedCameraProjection->target,s.target.camera);
}

TEST(CameraViewPoseControls, NumericQueueAndItsStaleAndDuplicateFailuresAllocateNothing) {
  auto c=controls();const auto s=*c.cameraViewPose;auto stale=s.target;++stale.nodeIncarnation;
  bool accepted{},duplicate{},wrongTarget{},wrongView{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);
    wrongTarget=QueueCameraViewPose(c,stale,s.viewRevision);wrongView=QueueCameraViewPose(c,s.target,s.viewRevision+1);
    accepted=QueueCameraViewPose(c,s.target,s.viewRevision);duplicate=QueueCameraViewPose(c,s.target,s.viewRevision);
    attempts=guard.allocations();}
  EXPECT_TRUE(accepted);EXPECT_FALSE(duplicate);EXPECT_FALSE(wrongTarget);EXPECT_FALSE(wrongView);EXPECT_EQ(attempts,0u);
}

TEST(CameraViewPoseControls, EverySnapshotCopyAllocationFailurePreservesBothStringsAndPendingIntent) {
  const auto before=snapshot();auto next=before;next.destinationName.assign(250,'n');next.blockReason.assign(200,'r');
  const std::optional<CameraViewPoseSnapshot> input=next;size_t allocationCount{};
  {auto c=controls();material_publication_test::AllocationGuard guard(false);const bool accepted=SetCameraViewPoseSnapshot(c,input);
    allocationCount=guard.allocations();guard.disable();ASSERT_TRUE(accepted);}
  ASSERT_GE(allocationCount,2u);
  for(size_t cut=0;cut<allocationCount;++cut) {
    auto c=controls();ASSERT_TRUE(QueueCameraViewPose(c,before.target,before.viewRevision));c.cameraViewPoseError="Keep old error";
    bool denied{};size_t attempts{};
    {material_publication_test::AllocationGuard guard(false,cut);
      try {(void)SetCameraViewPoseSnapshot(c,input);}catch(const std::bad_alloc&) {denied=true;}attempts=guard.allocations();}
    EXPECT_TRUE(denied);EXPECT_EQ(attempts,cut+1);ASSERT_TRUE(c.cameraViewPose);expectSnapshot(*c.cameraViewPose,before);
    ASSERT_TRUE(c.requestedCameraViewPose);EXPECT_EQ(c.requestedCameraViewPose->target,before.target);
    EXPECT_EQ(c.requestedCameraViewPose->viewRevision,before.viewRevision);EXPECT_EQ(c.cameraViewPoseError,"Keep old error");
  }
}

namespace {
class CameraViewPoseWidgetTests:public ::testing::Test {
 protected:
  void SetUp() override {
    previous_=ImGui::GetCurrentContext();context_=ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.DisplaySize={800,600};io.DeltaTime=1.f/60;
    unsigned char* pixels{};int width{},height{};io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);c_=controls();
  }
  void TearDown() override {ImGui::DestroyContext(context_);ImGui::SetCurrentContext(previous_);}
  void frame() {
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({720,550});
    ImGui::Begin("Camera pose widget test",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
    DrawCameraViewPoseControls(c_);buttonMin_=ImGui::GetItemRectMin();buttonMax_=ImGui::GetItemRectMax();
    ImGui::End();ImGui::Render();
  }
  void settle() {frame();frame();}
  void click() {
    auto& io=ImGui::GetIO();io.AddMousePosEvent((buttonMin_.x+buttonMax_.x)*.5f,(buttonMin_.y+buttonMax_.y)*.5f);frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();
  }
  GltfAnimationControls c_;ImGuiContext* previous_{};ImGuiContext* context_{};ImVec2 buttonMin_{},buttonMax_{};
};
} // namespace

TEST_F(CameraViewPoseWidgetTests, ExistingPanelButtonQueuesOneWeakWriteWhileTheLensEditorIsAbsent) {
  EXPECT_FALSE(c_.cameraProjection);settle();for(int i=0;i<10;++i) frame();EXPECT_FALSE(c_.requestedCameraViewPose);
  const auto s=*c_.cameraViewPose;click();ASSERT_TRUE(c_.requestedCameraViewPose);
  EXPECT_EQ(c_.requestedCameraViewPose->target,s.target);EXPECT_EQ(c_.requestedCameraViewPose->viewRevision,s.viewRevision);
  EXPECT_EQ(c_.cameraNode,-1);EXPECT_FALSE(c_.requestedCameraProjection);
  auto latest=s;++latest.viewRevision;ASSERT_TRUE(SetCameraViewPoseSnapshot(c_,latest));settle();click();
  EXPECT_EQ(c_.requestedCameraViewPose->viewRevision,s.viewRevision);
}

TEST_F(CameraViewPoseWidgetTests, UnavailableViewportAndAnimatedDestinationReasonsKeepTheButtonInert) {
  auto s=*c_.cameraViewPose;s.blockReason="This camera node has an animated rotation.";
  ASSERT_TRUE(SetCameraViewPoseSnapshot(c_,s));settle();click();EXPECT_FALSE(c_.requestedCameraViewPose);
  s.blockReason.clear();s.viewportActive=false;ASSERT_TRUE(SetCameraViewPoseSnapshot(c_,s));settle();click();EXPECT_FALSE(c_.requestedCameraViewPose);
  s.viewportActive=true;ASSERT_TRUE(SetCameraViewPoseSnapshot(c_,s));c_.cameraNode=s.target.camera.sourceNode;settle();click();EXPECT_FALSE(c_.requestedCameraViewPose);
}

TEST_F(CameraViewPoseWidgetTests, MissingDestinationAndPendingLensRequestKeepTheButtonInert) {
  ASSERT_TRUE(SetCameraViewPoseSnapshot(c_,std::nullopt));settle();click();EXPECT_FALSE(c_.requestedCameraViewPose);
  const auto s=snapshot();ASSERT_TRUE(SetCameraViewPoseSnapshot(c_,s));ASSERT_TRUE(SetCameraProjectionSnapshot(c_,lensSnapshot(s.target)));
  ASSERT_TRUE(QueueCameraProjectionEdit(c_,s.target.camera,{{},{{Property::PerspectiveNear,.25}}}));
  settle();click();EXPECT_FALSE(c_.requestedCameraViewPose);ASSERT_TRUE(c_.requestedCameraProjection);
  EXPECT_EQ(c_.requestedCameraProjection->patch.values.size(),1u);
}
