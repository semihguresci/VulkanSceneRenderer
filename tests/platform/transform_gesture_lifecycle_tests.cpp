#include <Container/renderer/picking/RenderSurfaceInteractionController.h>
#include <Container/utility/InputManager.h>
#include <Container/common/CommonGLFW.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace {
using Controller=container::renderer::RenderSurfaceInteractionController;
using Reason=Controller::TransformGestureCancelReason;
using namespace container::ui;
using container::window::InputManager;

TEST(TransformGestureLifecycle, PhysicalPressAndReleaseHaveOneBoundaryEvenWithoutDeltas) {
  Controller controller;InputManager input;
  std::vector<int> events;
  Controller::TransformGestureCallbacks callbacks{
      [&](auto tool,auto space,auto axis,bool snap) {
        EXPECT_EQ(controller.state().gesture,ViewportGesture::None);
        EXPECT_EQ(tool,ViewportTool::Translate);EXPECT_EQ(space,TransformSpace::Local);
        EXPECT_EQ(axis,TransformAxis::Y);EXPECT_TRUE(snap);events.push_back(1);
      },[&] {EXPECT_EQ(controller.state().gesture,ViewportGesture::TransformDrag);events.push_back(2);},
      [&](auto) {events.push_back(3);}};
  input.handleMouseButton(GLFW_MOUSE_BUTTON_LEFT,GLFW_PRESS);
  controller.beginTransformGesture(ViewportTool::Translate,TransformSpace::Local,TransformAxis::Y,true,callbacks);
  controller.beginTransformGesture(ViewportTool::Translate,TransformSpace::Local,TransformAxis::Y,true,callbacks);
  for(int i=0;i<4;++i) {controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks);input.endFrame();}
  EXPECT_EQ(events,(std::vector<int>{1}));
  input.handleMouseButton(GLFW_MOUSE_BUTTON_LEFT,GLFW_RELEASE);
  controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks);
  controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks);
  EXPECT_EQ(events,(std::vector<int>{1,2}));EXPECT_EQ(controller.state().gesture,ViewportGesture::None);
}

TEST(TransformGestureLifecycle, EscapeFocusLossAndSelectionLossCancelBeforeDiscardExactlyOnce) {
  for(const auto reason : {Reason::Escape,Reason::FocusLost,Reason::SelectionLost}) {
    SCOPED_TRACE(int(reason));Controller controller;InputManager input;int cancelled{},ended{};
    Controller::TransformGestureCallbacks callbacks{{},[&] {++ended;},[&](auto actual) {
      EXPECT_EQ(actual,reason);EXPECT_EQ(controller.state().gesture,ViewportGesture::TransformDrag);++cancelled;
    }};
    input.handleMouseButton(GLFW_MOUSE_BUTTON_LEFT,GLFW_PRESS);
    controller.beginTransformGesture(ViewportTool::Rotate,TransformSpace::World,TransformAxis::Free,false,callbacks);
    input.endFrame();
    if(reason==Reason::Escape) input.handleKey(GLFW_KEY_ESCAPE,GLFW_PRESS);
    if(reason==Reason::FocusLost) {input.handleKey(GLFW_KEY_ESCAPE,GLFW_PRESS);input.handleWindowFocus(false);}
    controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks,reason!=Reason::SelectionLost);
    controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks,false);
    EXPECT_EQ(cancelled,1);EXPECT_EQ(ended,0);EXPECT_EQ(controller.state().gesture,ViewportGesture::None);
  }
}

TEST(TransformGestureLifecycle, ToolSpaceAxisAndSnapChangesFinishTheOldGestureWithoutCancellation) {
  for(int change=0;change<4;++change) {
    SCOPED_TRACE(change);Controller controller;InputManager input;int ended{},cancelled{};
    Controller::TransformGestureCallbacks callbacks{{},[&] {++ended;},[&](auto) {++cancelled;}};
    input.handleMouseButton(GLFW_MOUSE_BUTTON_LEFT,GLFW_PRESS);
    controller.beginTransformGesture(ViewportTool::Translate,TransformSpace::World,TransformAxis::X,false,callbacks);
    if(change==0) controller.setTool(ViewportTool::Scale);
    if(change==1) controller.setTransformSpace(TransformSpace::Local);
    if(change==2) controller.setTransformAxis(TransformAxis::Z);
    if(change==3) controller.setTransformSnapEnabled(true);
    controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks);
    controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks);
    EXPECT_EQ(ended,1);EXPECT_EQ(cancelled,0);EXPECT_EQ(controller.state().gesture,ViewportGesture::None);
  }
}

TEST(TransformGestureLifecycle, FailedCallbacksKeepOwnershipForPublicationRetry) {
  Controller controller;InputManager input;
  Controller::TransformGestureCallbacks callbacks;
  callbacks.begin=[](auto,auto,auto,bool) {throw std::runtime_error("Denied light Begin");};
  EXPECT_THROW(controller.beginTransformGesture(ViewportTool::Scale,TransformSpace::World,TransformAxis::X,false,callbacks),std::runtime_error);
  EXPECT_EQ(controller.state().gesture,ViewportGesture::None);
  callbacks.begin={};input.handleMouseButton(GLFW_MOUSE_BUTTON_LEFT,GLFW_PRESS);
  controller.beginTransformGesture(ViewportTool::Scale,TransformSpace::World,TransformAxis::X,false,callbacks);
  input.handleMouseButton(GLFW_MOUSE_BUTTON_LEFT,GLFW_RELEASE);
  callbacks.end=[] {throw std::runtime_error("Denied light End");};
  EXPECT_THROW(controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks),std::runtime_error);
  EXPECT_EQ(controller.state().gesture,ViewportGesture::TransformDrag);
  input.handleKey(GLFW_KEY_ESCAPE,GLFW_PRESS);
  callbacks.cancel=[](auto) {throw std::runtime_error("Denied light Cancel");};
  EXPECT_THROW(controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks),std::runtime_error);
  EXPECT_EQ(controller.state().gesture,ViewportGesture::TransformDrag);
  int cancelled{};callbacks.cancel=[&](auto reason) {EXPECT_EQ(reason,Reason::Escape);++cancelled;};
  controller.processTransformGestureLifecycle(input.frameSnapshot(),callbacks);
  EXPECT_EQ(cancelled,1);EXPECT_EQ(controller.state().gesture,ViewportGesture::None);
}

TEST(TransformGestureLifecycle, DefaultCallbacksPreserveOrdinaryObjectGestureBehavior) {
  Controller controller;InputManager input;
  input.handleMouseButton(GLFW_MOUSE_BUTTON_LEFT,GLFW_PRESS);
  controller.beginTransformGesture(ViewportTool::Translate,TransformSpace::World,TransformAxis::X,false,{});
  controller.processTransformGestureLifecycle(input.frameSnapshot(),{});
  EXPECT_EQ(controller.state().gesture,ViewportGesture::TransformDrag);
  input.handleMouseButton(GLFW_MOUSE_BUTTON_LEFT,GLFW_RELEASE);
  controller.processTransformGestureLifecycle(input.frameSnapshot(),{});
  EXPECT_EQ(controller.state().gesture,ViewportGesture::None);
}
} // namespace
