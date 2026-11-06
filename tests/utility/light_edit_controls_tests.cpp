#include <Container/utility/LightEditControls.h>

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace container::renderer;
using container::ui::LightEditControls;

LightEditSnapshot snapshot() {
  LightEditSnapshot result;
  result.target={1,2,3,0,4,1,{EditableLightType::Point,EditableLightSource::Imported,0}};
  result.light.id=result.target.id;result.light.type=result.target.id.type;result.light.source=result.target.id.source;
  result.light.label="Imported Point 0";result.light.intensity=1.23456789f;result.light.range=10;
  result.available=true;
  return result;
}
LightEditRequest request(const LightEditSnapshot& s) {
  return {s.target,LightEditPhase::Commit,0,{LightEditFieldBit(LightEditField::Intensity),LightEditValuesFrom(s.light)}};
}

TEST(LightEditControls, WeakSnapshotAndQueueRejectStaleMalformedAndOverBudgetIntent) {
  LightEditControls controls;const auto original=snapshot();controls.setSnapshot(original);
  auto stale=request(original);++stale.target.incarnation;
  EXPECT_THROW(controls.queue(stale),std::invalid_argument);
  auto invalid=request(original);invalid.patch.fields|=1u<<25;
  EXPECT_THROW(controls.queue(invalid),std::invalid_argument);
  invalid=request(original);invalid.patch.values.intensity=std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(controls.queue(invalid),std::invalid_argument);
  invalid=request(original);invalid.phase=LightEditPhase::Cancel;invalid.gesture=7;
  EXPECT_THROW(controls.queue(invalid),std::invalid_argument);
  auto bad=original;bad.light.label=std::string(LightEditControls::maxDisplayBytes+1,'x');
  EXPECT_THROW(controls.setSnapshot(std::move(bad)),std::invalid_argument);
  EXPECT_EQ(controls.snapshot().target,original.target);EXPECT_TRUE(controls.consume().empty());
  for(size_t i=0;i<LightEditControls::maxPendingRequests;++i) controls.queue(request(original));
  EXPECT_THROW(controls.queue(request(original)),std::length_error);
  EXPECT_EQ(controls.consume().size(),LightEditControls::maxPendingRequests);EXPECT_TRUE(controls.consume().empty());
}

TEST(LightEditControls, ReplacingLifetimeDropsWeakIntentAndManualAddsRemainIndependentOfSelection) {
  LightEditControls controls;auto state=snapshot();controls.setSnapshot(state);controls.queue(request(state));
  ++state.target.incarnation;controls.setSnapshot(state);EXPECT_TRUE(controls.consume().empty());
  controls.setSnapshot({});
  for(const auto type : {EditableLightType::Directional,EditableLightType::Point,EditableLightType::Spot,EditableLightType::Area}) {
    controls.queueAdd(type);EXPECT_THROW(controls.queueAdd(type),std::invalid_argument);
    EXPECT_EQ(controls.consumeAdd(),type);EXPECT_FALSE(controls.consumeAdd());
  }
  EXPECT_THROW(controls.queueAdd(EditableLightType(99)),std::invalid_argument);
}

TEST(LightEditControls, DeniedQueueAndConsumptionPreserveExistingPendingIntent) {
  LightEditControls controls;const auto state=snapshot();controls.setSnapshot(state);const auto pending=request(state);
  bool denied{};
  {material_publication_test::AllocationGuard guard(true);try {controls.queue(pending);}catch(const std::bad_alloc&) {denied=true;}}
  EXPECT_TRUE(denied);EXPECT_TRUE(controls.consume().empty());
  controls.queue(pending);denied=false;
  {material_publication_test::AllocationGuard guard(true);try {(void)controls.consume();}catch(const std::bad_alloc&) {denied=true;}}
  EXPECT_TRUE(denied);const auto kept=controls.consume();ASSERT_EQ(kept.size(),1u);
  EXPECT_EQ(kept[0].target,pending.target);EXPECT_EQ(kept[0].patch.fields,pending.patch.fields);
}

class LightEditWidgetTests:public ::testing::Test {
protected:
  void SetUp() override {
    previous=ImGui::GetCurrentContext();context=ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.DisplaySize={900,1200};io.DeltaTime=1.f/60;io.MouseDragThreshold=0;
    unsigned char* pixels{};int width{},height{};io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    state=snapshot();controls.setSnapshot(state);frame();frame();
  }
  void TearDown() override {ImGui::DestroyContext(context);ImGui::SetCurrentContext(previous);}
  void frame() {
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({800,1100});
    ImGui::Begin("Light edit widget",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);
    controls.drawAddButtons();controls.draw();ImGui::End();ImGui::Render();
    const auto queued=controls.consume();events.insert(events.end(),queued.begin(),queued.end());
  }
  ImGuiID id(const char* label,const char* group=nullptr) const {
    auto* window=ImGui::FindWindowByName("Light edit widget");
    auto seed=window->ID;if(group) seed=ImHashStr(group,0,seed);return ImHashStr(label,0,seed);
  }
  ImVec2 locate(ImGuiID wanted,float x=70) {
    for(float y=30;y<1050;y+=2) {
      ImGui::GetIO().AddMousePosEvent(x,y);frame();if(context->HoveredId==wanted) return {x,y};
    }
    ADD_FAILURE()<<"Could not find light widget "<<wanted;return {0,0};
  }
  ImVec2 beginDrag(const char* label,const char* group=nullptr) {
    const auto point=locate(id(label,group));auto& io=ImGui::GetIO();
    io.AddMousePosEvent(point.x,point.y);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();
    return point;
  }
  void move(ImVec2 point,float delta) {ImGui::GetIO().AddMousePosEvent(point.x+delta,point.y);frame();}
  void release() {ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();frame();}
  size_t count(LightEditPhase phase) const {return size_t(std::count_if(events.begin(),events.end(),[&](const auto& e){return e.phase==phase;}));}
  void type(const char* text) {
    auto& io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);frame();
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);frame();
    io.AddInputCharactersUTF8(text);frame();io.AddKeyEvent(ImGuiKey_Enter,true);frame();
    io.AddKeyEvent(ImGuiKey_Enter,false);frame();frame();
  }
  LightEditControls controls;LightEditSnapshot state;std::vector<LightEditRequest> events;
  ImGuiContext* previous{};ImGuiContext* context{};
};

TEST_F(LightEditWidgetTests, OnePhysicalDragUsesOneGestureAcrossOwnRevisionRefreshAndRelease) {
  const auto point=beginDrag("Intensity");EXPECT_TRUE(events.empty());
  move(point,25);move(point,45);ASSERT_EQ(count(LightEditPhase::Begin),1u);
  const auto gesture=events.front().gesture;ASSERT_NE(gesture,0u);
  ++state.target.revision;state.light.intensity=events.back().patch.values.intensity;controls.setSnapshot(state);
  move(point,65);release();
  EXPECT_EQ(count(LightEditPhase::Begin),1u);EXPECT_GE(count(LightEditPhase::Update),1u);
  EXPECT_EQ(count(LightEditPhase::End),1u);EXPECT_EQ(count(LightEditPhase::Cancel),0u);
  for(const auto& event:events) {EXPECT_EQ(event.gesture,gesture);EXPECT_EQ(event.target.incarnation,state.target.incarnation);}
  EXPECT_EQ(events.back().target.revision,state.target.revision);EXPECT_FALSE(controls.gestureActive());
  const auto next=beginDrag("Intensity");move(next,25);release();
  ASSERT_EQ(count(LightEditPhase::Begin),2u);
  const auto second=std::find_if(events.rbegin(),events.rend(),[](const auto& e){return e.phase==LightEditPhase::Begin;});
  EXPECT_NE(second->gesture,gesture);
}

TEST_F(LightEditWidgetTests, EscapeCancelsActualDragWithoutAReleaseCommitOrDuplicateEnd) {
  const auto point=beginDrag("##X","Position");move(point,40);ASSERT_EQ(count(LightEditPhase::Begin),1u);
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape,true);frame();
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape,false);frame();release();
  EXPECT_EQ(count(LightEditPhase::Cancel),1u);EXPECT_EQ(count(LightEditPhase::End),0u);
  EXPECT_FALSE(controls.gestureActive());EXPECT_EQ(events.back().patch.fields,0u);
}

TEST_F(LightEditWidgetTests, WindowFocusLossCancelsActualPreviewAndIdleActivationCreatesNoHistoryIntent) {
  auto point=beginDrag("Range");release();EXPECT_TRUE(events.empty());
  point=beginDrag("Range");move(point,30);ASSERT_EQ(count(LightEditPhase::Begin),1u);
  ImGui::GetIO().AddFocusEvent(false);frame();frame();
  EXPECT_EQ(count(LightEditPhase::Cancel),1u);EXPECT_EQ(count(LightEditPhase::End),0u);EXPECT_FALSE(controls.gestureActive());
}

TEST_F(LightEditWidgetTests, CtrlClickRoundTripsFloatPrecisionAndTypedEnterEndsExactlyOneGesture) {
  const auto point=locate(id("Intensity"));auto& io=ImGui::GetIO();
  io.AddKeyEvent(ImGuiMod_Ctrl,true);frame();
  io.AddMousePosEvent(point.x,point.y);io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();io.AddKeyEvent(ImGuiMod_Ctrl,false);frame();
  auto* input=ImGui::GetInputTextState(id("Intensity"));ASSERT_NE(input,nullptr);ASSERT_NE(input->TextA.Data,nullptr);
  EXPECT_EQ(std::bit_cast<uint32_t>(std::strtof(input->TextA.Data,nullptr)),std::bit_cast<uint32_t>(state.light.intensity));
  EXPECT_TRUE(events.empty());type("7.12345678");
  ASSERT_EQ(count(LightEditPhase::Begin),1u);EXPECT_EQ(count(LightEditPhase::End),1u);
  const auto value=std::find_if(events.rbegin(),events.rend(),[](const auto& e){return e.patch.fields!=0;});
  ASSERT_NE(value,events.rend());EXPECT_FLOAT_EQ(value->patch.values.intensity,7.12345678f);
  EXPECT_EQ(value->patch.fields,LightEditFieldBit(LightEditField::Intensity));EXPECT_FALSE(controls.gestureActive());
}

TEST_F(LightEditWidgetTests, TypedEscapeCancelsAndAddQueuesTheExistingGestureEndBeforeTheAction) {
  const auto point=beginDrag("Intensity");move(point,30);ASSERT_EQ(count(LightEditPhase::Begin),1u);
  controls.queueAdd(EditableLightType::Area);
  const auto ending=controls.consume();ASSERT_EQ(ending.size(),1u);EXPECT_EQ(ending[0].phase,LightEditPhase::End);
  EXPECT_EQ(controls.consumeAdd(),EditableLightType::Area);EXPECT_FALSE(controls.gestureActive());release();
  events.clear();
  const auto numeric=locate(id("Intensity"));auto& io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);frame();
  io.AddMousePosEvent(numeric.x,numeric.y);io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();io.AddKeyEvent(ImGuiMod_Ctrl,false);frame();
  io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);frame();
  io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);frame();io.AddInputCharactersUTF8("12.25");frame();
  io.AddKeyEvent(ImGuiKey_Escape,true);frame();io.AddKeyEvent(ImGuiKey_Escape,false);frame();
  EXPECT_EQ(count(LightEditPhase::Cancel),1u);EXPECT_EQ(count(LightEditPhase::End),0u);
}

TEST_F(LightEditWidgetTests, ColorComponentNumericEntryRetainsFloatPrecisionInTheColorPatch) {
  state.light.color={.123456789f,.25f,.75f};controls.setSnapshot(state);frame();
  const auto point=locate(id("##X","Color RGB"));auto& io=ImGui::GetIO();
  io.AddKeyEvent(ImGuiMod_Ctrl,true);frame();io.AddMousePosEvent(point.x,point.y);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();
  io.AddKeyEvent(ImGuiMod_Ctrl,false);frame();
  auto* input=ImGui::GetInputTextState(id("##X","Color RGB"));ASSERT_NE(input,nullptr);
  EXPECT_EQ(std::bit_cast<uint32_t>(std::strtof(input->TextA.Data,nullptr)),std::bit_cast<uint32_t>(state.light.color.x));
  type("0.876543211");
  const auto changed=std::find_if(events.rbegin(),events.rend(),[](const auto& e){return e.patch.fields!=0;});
  ASSERT_NE(changed,events.rend());EXPECT_EQ(changed->patch.fields,LightEditFieldBit(LightEditField::Color));
  EXPECT_FLOAT_EQ(changed->patch.values.color.x,.876543211f);
  EXPECT_EQ(changed->patch.values.color.y,state.light.color.y);EXPECT_EQ(changed->patch.values.color.z,state.light.color.z);
  EXPECT_EQ(count(LightEditPhase::Begin),1u);EXPECT_EQ(count(LightEditPhase::End),1u);
}

TEST_F(LightEditWidgetTests, ManualAreaAndGeneratedSpotWidgetsKeepCoupledFieldsAndSourceIdentity) {
  state.light.type=EditableLightType::Area;state.light.source=EditableLightSource::Manual;
  state.light.id=state.target.id={EditableLightType::Area,EditableLightSource::Manual,2};
  ++state.target.incarnation;controls.setSnapshot(state);frame();
  auto point=beginDrag("Area Width");move(point,25);release();
  auto changed=std::find_if(events.rbegin(),events.rend(),[](const auto& e){return e.patch.fields!=0;});
  ASSERT_NE(changed,events.rend());EXPECT_EQ(changed->patch.fields,LightEditFieldBit(LightEditField::AreaSize));
  EXPECT_GT(changed->patch.values.areaHalfSize.x,state.light.areaHalfSize.x);
  EXPECT_EQ(changed->patch.values.areaHalfSize.y,state.light.areaHalfSize.y);
  EXPECT_EQ(changed->target.id.source,EditableLightSource::Manual);EXPECT_EQ(count(LightEditPhase::End),1u);
  events.clear();
  state.light.type=EditableLightType::Spot;state.light.source=EditableLightSource::Generated;
  state.light.id=state.target.id={EditableLightType::Spot,EditableLightSource::Generated,1};
  state.light.innerConeDegrees=10;state.light.outerConeDegrees=30;
  ++state.target.incarnation;controls.setSnapshot(state);frame();
  point=beginDrag("Inner cone (degrees)");move(point,25);release();
  changed=std::find_if(events.rbegin(),events.rend(),[](const auto& e){return e.patch.fields!=0;});
  ASSERT_NE(changed,events.rend());EXPECT_EQ(changed->patch.fields,LightEditFieldBit(LightEditField::SpotCone));
  EXPECT_GT(changed->patch.values.innerConeDegrees,state.light.innerConeDegrees);
  EXPECT_EQ(changed->patch.values.outerConeDegrees,state.light.outerConeDegrees);
  EXPECT_EQ(changed->target.id.source,EditableLightSource::Generated);EXPECT_EQ(count(LightEditPhase::End),1u);
}

TEST_F(LightEditWidgetTests, ActualAddButtonQueuesWithoutSelectionAndColorPopupHasOneCompletion) {
  controls.setSnapshot({});frame();
  float areaX=ImGui::FindWindowByName("Light edit widget")->DC.CursorStartPos.x;
  for(const auto* label : {"Add directional","Add point","Add spot"})
    areaX+=ImGui::CalcTextSize(label).x+2*ImGui::GetStyle().FramePadding.x+ImGui::GetStyle().ItemSpacing.x;
  areaX+=ImGui::GetStyle().FramePadding.x+ImGui::CalcTextSize("Add area").x*.5f;
  auto point=locate(id("Add area"),areaX);
  auto& io=ImGui::GetIO();io.AddMousePosEvent(point.x,point.y);frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();
  EXPECT_EQ(controls.consumeAdd(),EditableLightType::Area);EXPECT_TRUE(events.empty());
  // The color button is a drag source. Use the normal mouse threshold so a
  // stationary click reaches the button's release handler and opens the popup.
  io.MouseDragThreshold=6.0f;
  controls.setSnapshot(state);frame();point=locate(id("Color preview"),20);
  io.AddMousePosEvent(point.x,point.y);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();frame();
  ASSERT_FALSE(context->OpenPopupStack.empty());auto* popup=context->OpenPopupStack.back().Window;ASSERT_NE(popup,nullptr);
  const auto start=popup->DC.CursorStartPos;
  io.AddMousePosEvent(start.x+35,start.y+45);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();
  io.AddMousePosEvent(start.x+95,start.y+75);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();
  ASSERT_EQ(count(LightEditPhase::Begin),1u);EXPECT_EQ(count(LightEditPhase::End),0u);
  io.AddMousePosEvent(850,1150);frame();io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);frame();frame();
  EXPECT_EQ(count(LightEditPhase::End),1u);EXPECT_FALSE(controls.gestureActive());
  for(const auto& event:events) if(event.patch.fields) EXPECT_EQ(event.patch.fields,LightEditFieldBit(LightEditField::Color));
}
} // namespace
