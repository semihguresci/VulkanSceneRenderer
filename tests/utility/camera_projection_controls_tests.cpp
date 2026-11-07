#include <Container/utility/GltfAnimationControls.h>

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace container::ui;
using Projection=container::geometry::gltf::GltfCameraProjection;
using Property=container::geometry::gltf::GltfCameraProperty;
using Patch=container::geometry::gltf::GltfCameraProjectionPatch;
using Target=container::scene::CameraProjectionTarget;

CameraProjectionSnapshot snapshot() {
  CameraProjectionSnapshot s;
  s.target={11,12,0,13,0,14,2,3,4,5};
  s.type=Projection::Perspective;s.values={std::numbers::pi/3,0,.1,0,0,0,0,0};s.sharedDefinitionUses=2;
  return s;
}
GltfAnimationControls controls() {
  GltfAnimationControls c;c.cameraNode=3;
  if(!SetCameraProjectionSnapshot(c,snapshot())) throw std::runtime_error("Invalid camera control fixture");
  return c;
}
Patch nearPatch() {return {{},{{Property::PerspectiveNear,.25}}};}
const container::geometry::gltf::GltfCameraNumericEdit* edit(const Patch& p,Property property) {
  const auto found=std::find_if(p.values.begin(),p.values.end(),[&](const auto& e) {return e.property==property;});
  return found==p.values.end()?nullptr:&*found;
}
void expectUnchanged(const GltfAnimationControls& c,const CameraProjectionSnapshot& previous,const CameraProjectionDraft& draft) {
  ASSERT_TRUE(c.cameraProjection);EXPECT_EQ(c.cameraProjection->target,previous.target);
  EXPECT_EQ(c.cameraProjection->type,previous.type);EXPECT_EQ(c.cameraProjection->values,previous.values);
  EXPECT_EQ(c.cameraProjectionDraft.target,draft.target);EXPECT_EQ(c.cameraProjectionDraft.values,draft.values);
  EXPECT_EQ(c.cameraProjectionDraft.edited,draft.edited);EXPECT_EQ(c.cameraProjectionDraft.type,draft.type);
}
} // namespace

TEST(CameraProjectionControls, SnapshotPreservesDirtyLensAcrossGraphMotionAndResetsOnActualIdentityChanges) {
  auto c=controls();auto draft=c.cameraProjectionDraft;draft.values[2]=.25;draft.edited[2]=true;
  ASSERT_TRUE(SetCameraProjectionDraft(c,draft));
  auto latest=*c.cameraProjection;++latest.target.graphRevision;latest.values[0]=.9;
  ASSERT_TRUE(SetCameraProjectionSnapshot(c,latest));
  EXPECT_EQ(c.cameraProjectionDraft.target,latest.target);EXPECT_DOUBLE_EQ(c.cameraProjectionDraft.values[2],.25);
  EXPECT_TRUE(c.cameraProjectionDraft.edited[2]);EXPECT_DOUBLE_EQ(c.cameraProjectionDraft.values[0],.9);
  EXPECT_FALSE(SetCameraProjectionDraft(c,draft));EXPECT_DOUBLE_EQ(c.cameraProjectionDraft.values[2],.25);
  EXPECT_FALSE(QueueCameraProjectionEdit(c,draft.target,nearPatch()));
  ASSERT_TRUE(QueueCameraProjectionDraft(c));EXPECT_EQ(c.requestedCameraProjection->target,latest.target);
  ++latest.target.revision;latest.values[2]=.3;
  ASSERT_TRUE(SetCameraProjectionSnapshot(c,latest));EXPECT_FALSE(c.requestedCameraProjection);
  EXPECT_DOUBLE_EQ(c.cameraProjectionDraft.values[2],.3);EXPECT_FALSE(c.cameraProjectionDraft.edited[2]);
  c.cameraProjectionError="Previous camera error";
  ASSERT_TRUE(SetCameraProjectionSnapshot(c,std::nullopt));EXPECT_FALSE(c.cameraProjection);EXPECT_TRUE(c.cameraProjectionError.empty());
  EXPECT_FALSE(QueueCameraProjectionEdit(c,latest.target,nearPatch()));
}

TEST(CameraProjectionControls, SnapshotRejectsInvalidIdentityNumbersAndOversizedReasonsWithoutChangingControls) {
  auto c=controls();const auto before=*c.cameraProjection;const auto draft=c.cameraProjectionDraft;
  const std::array<std::function<void(CameraProjectionSnapshot&)>,11> mutate{
      [](auto& s) {s.target.asset=0;},[](auto& s) {s.target.sourceNamespace=0;},[](auto& s) {s.target.incarnation=0;},
      [](auto& s) {s.target.graphNode=UINT32_MAX;},[](auto& s) {s.target.sourceNode=-1;},
      [](auto& s) {s.type=Projection(99);},[](auto& s) {s.values[0]=std::numeric_limits<double>::quiet_NaN();},
      [](auto& s) {s.values[2]=0;},[](auto& s) {s.sharedDefinitionUses=0;},
      [](auto& s) {s.propertyBlockReasons[0]=std::string(513,'x');},[](auto& s) {s.typeSwitchBlockReason=std::string(513,'y');}};
  for(const auto& change:mutate) {auto invalid=before;change(invalid);EXPECT_FALSE(SetCameraProjectionSnapshot(c,invalid));expectUnchanged(c,before,draft);}
}

TEST(CameraProjectionControls, QueueRequiresExactWeakTargetAndNeverReplacesOnePendingRequest) {
  auto c=controls();const auto target=c.cameraProjection->target;const auto patch=nearPatch();
  const std::array<std::function<void(Target&)>,10> mutate{
      [](auto& t) {++t.asset;},[](auto& t) {++t.sourceNamespace;},[](auto& t) {++t.revision;},
      [](auto& t) {++t.graphIdentity;},[](auto& t) {++t.graphRevision;},[](auto& t) {++t.incarnation;},
      [](auto& t) {++t.graphNode;},[](auto& t) {++t.sourceNode;},[](auto& t) {++t.authoredCamera;},[](auto& t) {++t.evaluatedCamera;}};
  for(const auto& change:mutate) {auto stale=target;change(stale);EXPECT_FALSE(QueueCameraProjectionEdit(c,stale,patch));EXPECT_FALSE(c.requestedCameraProjection);}
  ASSERT_TRUE(QueueCameraProjectionEdit(c,target,patch));
  EXPECT_FALSE(QueueCameraProjectionEdit(c,target,{{},{{Property::PerspectiveNear,.5}}}));
  ASSERT_EQ(c.requestedCameraProjection->patch.values.size(),1u);
  EXPECT_DOUBLE_EQ(std::get<double>(c.requestedCameraProjection->patch.values[0].value),.25);
}

TEST(CameraProjectionControls, QueueRejectsDuplicateIncompatibleNonfiniteOmissionAndIncompleteTypeEdits) {
  auto c=controls();const auto target=c.cameraProjection->target;
  const std::array<Patch,9> invalid{
      Patch{},Patch{{},{{Property::PerspectiveNear,.2},{Property::PerspectiveNear,.3}}},
      Patch{{},{{Property::OrthographicXmag,2.}}},Patch{{},{{Property::Count,1.}}},
      Patch{{},{{Property::PerspectiveNear,std::monostate{}}}},Patch{{},{{Property::PerspectiveAspectRatio,0.}}},
      Patch{{},{{Property::PerspectiveFar,std::numeric_limits<double>::infinity()}}},
      Patch{Projection::Orthographic,{{Property::OrthographicXmag,2.}}},Patch{Projection(99),{}}};
  for(const auto& p:invalid) {EXPECT_FALSE(QueueCameraProjectionEdit(c,target,p));EXPECT_FALSE(c.requestedCameraProjection);}
  Patch tooMany;for(int i=0;i<9;++i) tooMany.values.push_back({Property::PerspectiveNear,.2});
  EXPECT_FALSE(QueueCameraProjectionEdit(c,target,tooMany));
  const Patch orthographic{Projection::Orthographic,{{Property::OrthographicXmag,8.},{Property::OrthographicYmag,3.},
      {Property::OrthographicNear,0.},{Property::OrthographicFar,30.}}};
  ASSERT_TRUE(QueueCameraProjectionEdit(c,target,orthographic));EXPECT_EQ(c.requestedCameraProjection->patch.type,Projection::Orthographic);
}

TEST(CameraProjectionControls, ExplicitOptionalOmissionsRemainOmittedAndDoNotCaptureViewportFallbacks) {
  auto c=controls();auto s=*c.cameraProjection;s.values[1]=1.5;s.values[3]=200.;ASSERT_TRUE(SetCameraProjectionSnapshot(c,s));
  const Patch omitted{{},{{Property::PerspectiveAspectRatio,std::monostate{}},{Property::PerspectiveFar,std::monostate{}}}};
  ASSERT_TRUE(QueueCameraProjectionEdit(c,s.target,omitted));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(edit(c.requestedCameraProjection->patch,Property::PerspectiveAspectRatio)->value));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(edit(c.requestedCameraProjection->patch,Property::PerspectiveFar)->value));
  EXPECT_DOUBLE_EQ(c.cameraProjection->values[1],1.5);EXPECT_DOUBLE_EQ(c.cameraProjection->values[3],200.);
}

TEST(CameraProjectionControls, AnimatedDefaultsAcceptNumbersAndKeepOptionalPointerTargetsPresent) {
  auto c=controls();auto s=*c.cameraProjection;
  s.values[1]=1.5;s.values[3]=200.;
  s.animatedProperties[0]=s.animatedProperties[1]=s.animatedProperties[3]=true;
  ASSERT_TRUE(SetCameraProjectionSnapshot(c,s));
  for(const auto property:{Property::PerspectiveAspectRatio,Property::PerspectiveFar}) {
    EXPECT_FALSE(QueueCameraProjectionEdit(c,s.target,{{},{{property,std::monostate{}}}}));
    auto draft=c.cameraProjectionDraft;draft.values[size_t(property)]=0;draft.edited[size_t(property)]=true;
    EXPECT_FALSE(SetCameraProjectionDraft(c,draft));
  }
  EXPECT_TRUE(QueueCameraProjectionEdit(c,s.target,{{},{{Property::PerspectiveYfov,.8},
      {Property::PerspectiveAspectRatio,1.25},{Property::PerspectiveFar,300.}}}));
}

TEST(CameraProjectionControls, PendingSelectionAndTimelineRequestsCannotShareTheLensApplyQueue) {
  const std::array<std::function<void(GltfAnimationControls&)>,6> conflict{
      [](auto& c) {c.requestedCameraNode=-1;},[](auto& c) {c.requestedClip=0;},[](auto& c) {c.requestedTime=2.;},
      [](auto& c) {c.requestedSpeed=.5;},[](auto& c) {c.requestedPlaying=true;},[](auto& c) {c.requestedLoop=false;}};
  for(const auto& change:conflict) {auto c=controls();change(c);EXPECT_FALSE(QueueCameraProjectionEdit(c,c.cameraProjection->target,nearPatch()));EXPECT_FALSE(c.requestedCameraProjection);}
}

TEST(CameraProjectionControls, PropertyBlockersLeaveUnrelatedLensFieldsUsable) {
  auto c=controls();auto s=*c.cameraProjection;s.propertyBlockReasons[0]="The lens field is locked in this context.";
  s.typeSwitchBlockReason="Projection type is animated.";ASSERT_TRUE(SetCameraProjectionSnapshot(c,s));
  EXPECT_FALSE(QueueCameraProjectionEdit(c,s.target,{{},{{Property::PerspectiveYfov,.8}}}));
  EXPECT_FALSE(QueueCameraProjectionEdit(c,s.target,{Projection::Orthographic,{{Property::OrthographicXmag,8.},
      {Property::OrthographicYmag,3.},{Property::OrthographicNear,0.},{Property::OrthographicFar,30.}}}));
  EXPECT_TRUE(QueueCameraProjectionEdit(c,s.target,nearPatch()));
}

TEST(CameraProjectionControls, AllocationFailurePreservesSnapshotDraftAndEmptyRequest) {
  auto c=controls();const auto before=*c.cameraProjection;const auto draft=c.cameraProjectionDraft;
  auto next=before;next.propertyBlockReasons[0]=std::string(200,'r');const std::optional<CameraProjectionSnapshot> input=next;
  bool denied{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);try {(void)SetCameraProjectionSnapshot(c,input);}catch(const std::bad_alloc&){denied=true;}attempts=guard.allocations();}
  EXPECT_TRUE(denied);EXPECT_GT(attempts,0u);expectUnchanged(c,before,draft);EXPECT_FALSE(c.requestedCameraProjection);
  denied=false;const auto patch=nearPatch();
  {material_publication_test::AllocationGuard guard(true);try {(void)QueueCameraProjectionEdit(c,before.target,patch);}catch(const std::bad_alloc&){denied=true;}attempts=guard.allocations();}
  EXPECT_TRUE(denied);EXPECT_GT(attempts,0u);expectUnchanged(c,before,draft);EXPECT_FALSE(c.requestedCameraProjection);
}

namespace {
class CameraProjectionWidgetTests:public ::testing::Test {
 protected:
  void SetUp() override {
    previous_=ImGui::GetCurrentContext();context_=ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.DisplaySize={800,900};io.DeltaTime=1.f/60;
    unsigned char* pixels{};int width{},height{};io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    c_=controls();
  }
  void TearDown() override {ImGui::DestroyContext(context_);ImGui::SetCurrentContext(previous_);}
  void frame() {
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({720,850});
    ImGui::Begin("Camera lens widget test",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
    DrawCameraProjectionControls(c_);applyMin_=ImGui::GetItemRectMin();applyMax_=ImGui::GetItemRectMax();
    ImGui::End();ImGui::Render();
  }
  void settle() {frame();frame();}
  void click(ImVec2 point) {
    auto& io=ImGui::GetIO();io.AddMousePosEvent(point.x,point.y);frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();
  }
  ImVec2 rowBeforeApply(int rows) const {
    return {applyMin_.x+60,(applyMin_.y+applyMax_.y)*.5f-float(rows)*ImGui::GetFrameHeightWithSpacing()};
  }
  void apply() {click({(applyMin_.x+applyMax_.x)*.5f,(applyMin_.y+applyMax_.y)*.5f});}
  void enter(int rows,std::string_view value) {
    click(rowBeforeApply(rows));auto& io=ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);frame();
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);frame();
    const std::string text(value);io.AddInputCharactersUTF8(text.c_str());frame();
    io.AddKeyEvent(ImGuiKey_Enter,true);frame();io.AddKeyEvent(ImGuiKey_Enter,false);settle();
  }
  void chooseOrthographic() {
    click(rowBeforeApply(7));settle();ImGuiWindow* popup{};
    for(auto* window:context_->Windows) if(window->Active && std::string_view(window->Name).starts_with("##Combo_")) popup=window;
    ASSERT_NE(popup,nullptr);
    click({popup->DC.CursorStartPos.x+35,popup->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()*1.5f});settle();
  }
  GltfAnimationControls c_;ImGuiContext* previous_{};ImGuiContext* context_{};ImVec2 applyMin_{},applyMax_{};
};
} // namespace

TEST_F(CameraProjectionWidgetTests, IdleFramesDoNotQueueAndFovDegreesBecomeOneExplicitRadiansApply) {
  settle();for(int i=0;i<12;++i) frame();EXPECT_FALSE(c_.requestedCameraProjection);
  enter(6,"75");EXPECT_NEAR(c_.cameraProjectionDraft.values[0],75.*std::numbers::pi/180.,1e-10);
  EXPECT_FALSE(c_.requestedCameraProjection);apply();ASSERT_TRUE(c_.requestedCameraProjection);
  const auto* field=edit(c_.requestedCameraProjection->patch,Property::PerspectiveYfov);ASSERT_NE(field,nullptr);
  EXPECT_NEAR(std::get<double>(field->value),75.*std::numbers::pi/180.,1e-10);
  EXPECT_EQ(c_.requestedCameraProjection->patch.values.size(),1u);
  const auto request=c_.requestedCameraProjection->target;apply();for(int i=0;i<8;++i) frame();
  ASSERT_TRUE(c_.requestedCameraProjection);EXPECT_EQ(c_.requestedCameraProjection->target,request);
  EXPECT_EQ(c_.requestedCameraProjection->patch.values.size(),1u);
}

TEST_F(CameraProjectionWidgetTests, TypeSwitchExposesIndependentOrthographicMagnitudesAndExplicitDestinationFields) {
  settle();chooseOrthographic();ASSERT_EQ(c_.cameraProjectionDraft.type,Projection::Orthographic);
  enter(4,"8");enter(3,"3");EXPECT_DOUBLE_EQ(c_.cameraProjectionDraft.values[4],8.);EXPECT_DOUBLE_EQ(c_.cameraProjectionDraft.values[5],3.);
  apply();ASSERT_TRUE(c_.requestedCameraProjection);const auto& patch=c_.requestedCameraProjection->patch;
  EXPECT_EQ(patch.type,Projection::Orthographic);ASSERT_EQ(patch.values.size(),4u);
  ASSERT_NE(edit(patch,Property::OrthographicXmag),nullptr);ASSERT_NE(edit(patch,Property::OrthographicYmag),nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(edit(patch,Property::OrthographicXmag)->value),8.);
  EXPECT_DOUBLE_EQ(std::get<double>(edit(patch,Property::OrthographicYmag)->value),3.);
  EXPECT_DOUBLE_EQ(c_.cameraProjection->values[4],0.);EXPECT_DOUBLE_EQ(c_.cameraProjection->values[5],0.);
}

TEST_F(CameraProjectionWidgetTests, OptionalCheckboxesQueueOmissionAndKeepTheDisplayedSnapshotUnchanged) {
  auto s=*c_.cameraProjection;s.values[1]=1.5;s.values[3]=200.;ASSERT_TRUE(SetCameraProjectionSnapshot(c_,s));settle();
  click(rowBeforeApply(5));click(rowBeforeApply(2));enter(3,"0.25");apply();
  ASSERT_TRUE(c_.requestedCameraProjection);const auto& patch=c_.requestedCameraProjection->patch;
  ASSERT_NE(edit(patch,Property::PerspectiveAspectRatio),nullptr);ASSERT_NE(edit(patch,Property::PerspectiveFar),nullptr);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(edit(patch,Property::PerspectiveAspectRatio)->value));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(edit(patch,Property::PerspectiveFar)->value));
  ASSERT_NE(edit(patch,Property::PerspectiveNear),nullptr);EXPECT_DOUBLE_EQ(std::get<double>(edit(patch,Property::PerspectiveNear)->value),.25);
  EXPECT_DOUBLE_EQ(c_.cameraProjection->values[1],1.5);EXPECT_DOUBLE_EQ(c_.cameraProjection->values[3],200.);
}
