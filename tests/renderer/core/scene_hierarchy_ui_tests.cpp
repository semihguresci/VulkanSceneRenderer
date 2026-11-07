#include "Container/renderer/scene/SceneReloadPublication.h"
#include "Container/renderer/scene/PhysicsDemoNodes.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneEditHistory.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneHierarchyCache.h"
#if CONTAINER_HAS_JOLT
#include "Container/physics/PhysicsElementBindings.h"
#include "Container/physics/PhysicsScene.h"
#endif

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <glm/gtc/matrix_transform.hpp>

#include <functional>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace {

using container::scene::SceneGraph;
using container::ui::SceneHierarchyCache;

uint32_t addNode(SceneGraph &graph, std::string name) {
  return graph.createNode(glm::mat4(1.0f), 0, false, SceneGraph::kInvalidNode,
                          std::move(name));
}

class HierarchyWidgetsTests : public ::testing::Test {
protected:
  void SetUp() override {
    previousContext_ = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previousContext_);
  }

  void frame(const std::function<void()> &draw) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(windowWidth_, 470.0f));
    ImGui::Begin("Hierarchy tests", nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    draw();
    ImGui::End();
    ImGui::Render();
  }

  void click(const std::function<void()> &draw, ImVec2 position) {
    ImGui::GetIO().AddMousePosEvent(position.x, position.y);
    frame(draw);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(draw);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(draw);
  }

  void key(const std::function<void()>& draw, ImGuiKey value) {
    ImGui::GetIO().AddKeyEvent(value, true); frame(draw);
    ImGui::GetIO().AddKeyEvent(value, false); frame(draw);
  }

  void beginInline(const std::function<void()>& draw) {
    key(draw, ImGuiKey_F2); frame(draw); frame(draw);
  }

  void replaceText(const std::function<void()>& draw, const std::string& text) {
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame(draw);
    io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame(draw);
    io.AddInputCharactersUTF8(text.c_str()); frame(draw);
  }

  void contextMenu(const std::function<void()>& draw, ImVec2 position) {
    auto& io = ImGui::GetIO(); io.AddMousePosEvent(position.x, position.y); frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true); frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false); frame(draw); frame(draw);
  }

  void drag(const std::function<void()> &draw, ImVec2 position) {
    ImGui::GetIO().AddMousePosEvent(position.x, position.y);
    frame(draw);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(draw);
    ImGui::GetIO().AddMousePosEvent(position.x + 40, position.y);
    frame(draw);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(draw);
  }

  void dragInSteps(const std::function<void()> &draw, ImVec2 position) {
    ImGui::GetIO().AddMousePosEvent(position.x, position.y);
    frame(draw);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(draw);
    for (float offset : {20.0f, 40.0f, 60.0f}) {
      ImGui::GetIO().AddMousePosEvent(position.x + offset, position.y);
      frame(draw);
    }
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(draw);
  }

  void openTransformEditors() {
    auto *window = ImGui::GetCurrentWindow();
    window->StateStorage.SetBool(window->GetID("Mesh Transform"), true);
    window->StateStorage.SetBool(window->GetID("Node Transform"), true);
  }

  ImVec2 positionXControl(ImVec2 start, bool meshPicker) {
    return {start.x + ImGui::GetStyle().IndentSpacing + 30,
        start.y + ImGui::GetFrameHeightWithSpacing() * (meshPicker ? 2 : 1) +
            ImGui::GetFrameHeight() * .5f};
  }

  ImGuiTable *hierarchyTable() {
    for (int index = 0; index < context_->Tables.GetMapSize(); ++index) {
      auto *table = context_->Tables.TryGetMapData(index);
      if (table && table->ColumnsCount == 6)
        return table;
    }
    return nullptr;
  }

  ImGuiContext *previousContext_{};
  ImGuiContext *context_{};
  float windowWidth_{630.0f};
};

} // namespace

namespace {

container::renderer::PhysicsDemoNodes makeDemoNodes(SceneGraph &graph) {
  container::renderer::PhysicsDemoNodes demo;
  demo.graphIdentity = graph.identity();
  demo.root = addNode(graph, "Physics demo");
  for (uint32_t index = 0; index < demo.bodies.size(); ++index) {
    demo.bodies[index] = addNode(graph, index == 0u ? "Floor" : "Box");
    EXPECT_TRUE(graph.setParent(demo.bodies[index], demo.root));
  }
  return demo;
}

} // namespace

TEST(PhysicsDemoNodesTests, ReorderedOwnedChildrenKeepAuthoredFloorAndBoxOrder) {
  SceneGraph graph;
  const auto demo = makeDemoNodes(graph);
  const auto floor = demo.bodies[0];
  ASSERT_TRUE(demo.reusable(graph));
  ASSERT_TRUE(graph.setParent(floor, std::nullopt));
  ASSERT_TRUE(graph.setParent(floor, demo.root));
  ASSERT_NE(graph.getNode(demo.root)->children.front(), floor);
  EXPECT_TRUE(demo.reusable(graph));
  EXPECT_EQ(demo.bodies[0], floor);
  EXPECT_EQ(graph.getNode(demo.bodies[0])->name, "Floor");
}

TEST(PhysicsDemoNodesTests, ForeignChildWithSameCountCannotBeResetAsDemoGeometry) {
  SceneGraph graph;
  const auto demo = makeDemoNodes(graph);
  ASSERT_TRUE(graph.setParent(demo.bodies[1], std::nullopt));
  const auto foreign = addNode(graph, "Unrelated imported mesh");
  graph.setLocalTransform(foreign, glm::translate(glm::mat4{1}, glm::vec3{20, 30, 40}));
  ASSERT_TRUE(graph.setParent(foreign, demo.root));
  const auto local = graph.getNode(foreign)->localTransform;
  ASSERT_EQ(graph.getNode(demo.root)->children.size(), demo.bodies.size());
  EXPECT_FALSE(demo.reusable(graph));
  EXPECT_EQ(graph.getNode(foreign)->name, "Unrelated imported mesh");
  EXPECT_EQ(graph.getNode(foreign)->localTransform, local);
}

TEST(PhysicsDemoNodesTests, ReplacementGraphCannotReuseOldDemoNodeIndices) {
  SceneGraph graph;
  const auto previous = makeDemoNodes(graph);
  graph = SceneGraph{};
  const auto current = makeDemoNodes(graph);
  ASSERT_EQ(current.root, previous.root);
  ASSERT_EQ(current.bodies, previous.bodies);
  EXPECT_FALSE(previous.reusable(graph));
  EXPECT_TRUE(current.reusable(graph));
}

#if CONTAINER_HAS_JOLT
TEST(SceneReloadPublicationTests, FailedPreparationRetainsBodiesAndJoints) {
  SceneGraph graph;
  const auto node = addNode(graph, "Live body");
  container::physics::PhysicsWorldSettings worldSettings;
  worldSettings.workerThreads = 0;
  container::physics::PhysicsScene physics{worldSettings};
  const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto joint = physics.createJoint(graph, std::nullopt, node, {});
  physics.setPlaying(true);
  const auto previousIdentity = graph.identity();
  const auto previousTransform = graph.getNode(node)->localTransform;

  // A rejected CPU payload does not publish a graph or enter GPU commit.
  bool retired = false;
  EXPECT_FALSE(container::renderer::retireBindingsAfterSceneReload(
      graph, previousIdentity, [&] { retired = true; physics.clear(); }));
  EXPECT_FALSE(retired);
  EXPECT_EQ(physics.bodyId(node), body);
  ASSERT_TRUE(physics.jointState(joint));
  EXPECT_EQ(physics.jointCount(), 1u);
  EXPECT_TRUE(physics.playing());
  EXPECT_EQ(graph.getNode(node)->localTransform, previousTransform);
}

TEST(SceneReloadPublicationTests, FailedCommitRetiresRuntimeJointsBeforeUiRead) {
  SceneGraph graph;
  addNode(graph, "Imported root");
  const auto runtimeNode = addNode(graph, "Runtime demo body");
  container::physics::PhysicsWorldSettings worldSettings;
  worldSettings.workerThreads = 0;
  container::physics::PhysicsScene physics{worldSettings};
  (void)physics.attachBox(graph, runtimeNode, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto joint = physics.createJoint(graph, std::nullopt, runtimeNode, {});
  const auto previousIdentity = graph.identity();

  // GPU commit failure restores only the imported graph, removing runtime
  // primitives. The UI reads joints before the next physics update/bindGraph.
  graph = SceneGraph{};
  const auto restoredNode = addNode(graph, "Restored imported root");
  const auto restoredTransform = graph.getNode(restoredNode)->localTransform;
  ASSERT_EQ(graph.getNode(runtimeNode), nullptr);
  ASSERT_TRUE(physics.jointState(joint));
  ASSERT_EQ(physics.joints().front().nodeB, runtimeNode);

  EXPECT_TRUE(container::renderer::retireBindingsAfterSceneReload(
      graph, previousIdentity, [&] { physics.clear(); }));
  EXPECT_EQ(physics.bodyCount(), 0u);
  EXPECT_TRUE(physics.joints().empty());
  EXPECT_FALSE(physics.jointState(joint));
  EXPECT_EQ(graph.getNode(restoredNode)->localTransform, restoredTransform);
}

TEST(SceneReloadPublicationTests, ReusedNodeIndicesStillRetireOldBindings) {
  SceneGraph graph;
  const auto node = addNode(graph, "Old imported body");
  container::physics::PhysicsWorldSettings worldSettings;
  worldSettings.workerThreads = 0;
  container::physics::PhysicsScene physics{worldSettings};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  (void)physics.createJoint(graph, std::nullopt, node, {});
  const auto previousIdentity = graph.identity();
  const auto previousRevision = graph.revision();

  graph = SceneGraph{};
  ASSERT_EQ(addNode(graph, "Restored imported body"), node);
  ASSERT_EQ(graph.revision(), previousRevision);
  EXPECT_TRUE(container::renderer::retireBindingsAfterSceneReload(
      graph, previousIdentity, [&] { physics.clear(); }));
  EXPECT_FALSE(physics.hasBody(node));
  EXPECT_TRUE(physics.joints().empty());
  EXPECT_EQ(graph.getNode(node)->name, "Restored imported body");
}
#endif

#if CONTAINER_HAS_JOLT
TEST_F(HierarchyWidgetsTests, NonrenderableProductHasNumericTransformControls) {
  using namespace container::physics;
  SceneGraph graph;
  (void)graph.createNode(glm::mat4{1}, 0, true);
  PhysicsScene physics{{.gravity = {0, 0, 0}, .maxBodies = 4, .workerThreads = 0}};
  PhysicsElementBindings bindings;
  const glm::vec3 center{4, 5, 6};
  const std::array fragments{PhysicsElementFragment{.objectIndex = 0,
      .model = glm::translate(glm::mat4{1}, center),
      .worldMin = center - glm::vec3{.5f}, .worldMax = center + glm::vec3{.5f}}};
  const auto body = bindings.attachOrReplace(graph, physics, 1, 1, fragments,
      {.motionType = PhysicsMotionType::Static}, "BIM product");
  const auto selected = *bindings.bodyNodeForObject(0);
  ASSERT_FALSE(graph.getNode(selected)->renderable);
  container::ui::GuiManager gui;
  container::ui::TransformControls controls;
  controls.position = center;
  uint32_t applied = SceneGraph::kInvalidNode;
  int applications = 0, selections = 0;
  ImVec2 start;
  const auto draw = [&] {
    openTransformEditors();
    start = ImGui::GetCursorScreenPos();
    gui.drawSceneNodeControls(graph, selected, controls,
        [&](uint32_t) { ++selections; },
        [&](uint32_t node, const auto &edited) {
          applied = node;
          controls = edited;
          ++applications;
        });
  };
  frame(draw);
  frame(draw);
  drag(draw, positionXControl(start, true));
  EXPECT_EQ(applications, 1);
  EXPECT_EQ(selections, 0);
  EXPECT_EQ(applied, selected);
  EXPECT_GT(controls.position.x, center.x);
  EXPECT_FLOAT_EQ(controls.position.y, center.y);
  EXPECT_FLOAT_EQ(controls.position.z, center.z);
  EXPECT_EQ(physics.bodyId(selected), body);
}
#endif

TEST_F(HierarchyWidgetsTests, GroupOnlySceneHasNumericTransformControls) {
  SceneGraph graph;
  const auto selected = addNode(graph, "Only a nonrenderable group");
  ASSERT_TRUE(graph.renderableNodes().empty());
  container::ui::GuiManager gui;
  container::ui::TransformControls controls;
  controls.position = {1, 2, 3};
  uint32_t applied = SceneGraph::kInvalidNode;
  int applications = 0;
  ImVec2 start;
  const auto draw = [&] {
    openTransformEditors();
    start = ImGui::GetCursorScreenPos();
    gui.drawSceneNodeControls(graph, selected, controls, [](uint32_t) {},
        [&](uint32_t node, const auto &edited) {
          applied = node;
          controls = edited;
          ++applications;
        });
  };
  frame(draw);
  frame(draw);
  drag(draw, positionXControl(start, false));
  EXPECT_EQ(applications, 1);
  EXPECT_EQ(applied, selected);
  EXPECT_GT(controls.position.x, 1);
  EXPECT_FLOAT_EQ(controls.position.y, 2);
  EXPECT_FLOAT_EQ(controls.position.z, 3);
}

TEST_F(HierarchyWidgetsTests,
       InspectorDragCreatesOneUndoStepAcrossChangedFrames) {
  SceneGraph graph;
  const auto selected = addNode(graph, "Editable group");
  container::ui::GuiManager gui;
  container::ui::TransformControls controls;
  controls.position = {1, 2, 3};
  const auto original = glm::translate(glm::mat4{1}, controls.position);
  graph.setLocalTransform(selected, original);
  container::util::SceneEditHistory history(graph);
  std::vector<uint64_t> gestures;
  ImVec2 start;
  const auto draw = [&] {
    openTransformEditors();
    start = ImGui::GetCursorScreenPos();
    gui.drawSceneNodeControls(
        graph, selected, controls, [](uint32_t) {},
        [&](uint32_t node, const auto &edited) {
          ASSERT_EQ(node, selected);
          ASSERT_TRUE(gui.sceneEditGesture());
          EXPECT_TRUE(gui.sceneEditGestureActive());
          EXPECT_NE(context_->ActiveId, 0u);
          gestures.push_back(*gui.sceneEditGesture());
          controls = edited;
          EXPECT_TRUE(history.setTransform(
              node, glm::translate(glm::mat4{1}, edited.position),
              gui.sceneEditGesture()));
        });
  };
  frame(draw);
  frame(draw);

  dragInSteps(draw, positionXControl(start, false));
  ASSERT_GE(gestures.size(), 2u)
      << "Several changed frames must exercise coalescing within one drag";
  const auto firstGesture = gestures.front();
  for (auto gesture : gestures)
    EXPECT_EQ(gesture, firstGesture);
  EXPECT_EQ(context_->ActiveId, 0u);
  ASSERT_EQ(history.undoCount(), 1u);
  const auto afterFirstDrag = graph.getNode(selected)->localTransform;
  EXPECT_NE(afterFirstDrag, original);

  // This fixture draws the inspector without GuiManager::startFrame. End its
  // history gesture explicitly after the synthetic mouse release.
  history.finishGesture();
  // Re-pressing the same field inside ImGui's double-click interval opens its
  // text editor. Advance idle frames so the next interaction is another drag.
  for (float elapsed = 0; elapsed <= ImGui::GetIO().MouseDoubleClickTime;
       elapsed += ImGui::GetIO().DeltaTime)
    frame(draw);
  gestures.clear();
  dragInSteps(draw, positionXControl(start, false));
  ASSERT_GE(gestures.size(), 2u);
  const auto secondGesture = gestures.front();
  EXPECT_NE(secondGesture, firstGesture);
  for (auto gesture : gestures)
    EXPECT_EQ(gesture, secondGesture);
  ASSERT_EQ(history.undoCount(), 2u);
  const auto afterSecondDrag = graph.getNode(selected)->localTransform;
  EXPECT_NE(afterSecondDrag, afterFirstDrag);
  history.finishGesture();

  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(selected)->localTransform, afterFirstDrag);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(selected)->localTransform, original);
  ASSERT_TRUE(history.redo());
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(graph.getNode(selected)->localTransform, afterSecondDrag);
}

TEST_F(HierarchyWidgetsTests,
       InspectorGesturesAreSeparateForFieldsAndSelections) {
  SceneGraph graph;
  const auto first = addNode(graph, "First group");
  const auto second = addNode(graph, "Second group");
  uint32_t selected = first;
  container::ui::GuiManager gui;
  std::array<container::ui::TransformControls, 2> controls;
  std::vector<uint64_t> gestures;
  std::vector<uint32_t> appliedNodes;
  ImVec2 start;
  const auto draw = [&] {
    openTransformEditors();
    start = ImGui::GetCursorScreenPos();
    gui.drawSceneNodeControls(
        graph, selected, controls[selected], [](uint32_t) {},
        [&](uint32_t node, const auto &edited) {
          ASSERT_TRUE(gui.sceneEditGesture());
          EXPECT_EQ(node, selected);
          EXPECT_NE(context_->ActiveId, 0u);
          gestures.push_back(*gui.sceneEditGesture());
          appliedNodes.push_back(node);
          controls[node] = edited;
        });
  };
  frame(draw);
  frame(draw);
  std::vector<uint64_t> completedGestures;
  const auto dragField = [&](int row) {
    gestures.clear();
    appliedNodes.clear();
    auto position = positionXControl(start, false);
    position.y += ImGui::GetFrameHeightWithSpacing() * row;
    dragInSteps(draw, position);
    ASSERT_GE(gestures.size(), 2u);
    for (auto gesture : gestures)
      EXPECT_EQ(gesture, gestures.front());
    for (auto node : appliedNodes)
      EXPECT_EQ(node, selected);
    for (auto previous : completedGestures)
      EXPECT_NE(gestures.front(), previous);
    completedGestures.push_back(gestures.front());
    EXPECT_EQ(context_->ActiveId, 0u);
  };

  dragField(0); // Position.
  EXPECT_GT(controls[first].position.x, 0);
  EXPECT_EQ(controls[first].rotationDegrees, glm::vec3(0));
  EXPECT_EQ(controls[first].scale, glm::vec3(1));
  const auto position = controls[first].position;
  dragField(1); // Rotation.
  EXPECT_EQ(controls[first].position, position);
  EXPECT_GT(controls[first].rotationDegrees.x, 0);
  EXPECT_EQ(controls[first].scale, glm::vec3(1));
  const auto rotation = controls[first].rotationDegrees;
  dragField(2); // Scale.
  EXPECT_EQ(controls[first].position, position);
  EXPECT_EQ(controls[first].rotationDegrees, rotation);
  EXPECT_GT(controls[first].scale.x, 1);
  const auto scale = controls[first].scale;

  selected = second;
  frame(draw);
  dragField(0);
  EXPECT_GT(controls[second].position.x, 0);
  EXPECT_EQ(controls[second].rotationDegrees, glm::vec3(0));
  EXPECT_EQ(controls[second].scale, glm::vec3(1));
  EXPECT_EQ(controls[first].position, position);
  EXPECT_EQ(controls[first].rotationDegrees, rotation);
  EXPECT_EQ(controls[first].scale, scale);
  EXPECT_EQ(completedGestures.size(), 4u);
}

TEST_F(HierarchyWidgetsTests, InvalidNumericTransformSelectionCannotApplyControls) {
  SceneGraph graph;
  (void)graph.createNode(glm::mat4{1}, 0, true);
  container::ui::GuiManager gui;
  int applications = 0;
  for (const auto selected : {SceneGraph::kInvalidNode, 1000u}) {
    SCOPED_TRACE(selected);
    ImVec2 start;
    float height = 0;
    const auto draw = [&] {
      openTransformEditors();
      start = ImGui::GetCursorScreenPos();
      gui.drawSceneNodeControls(graph, selected, {}, [](uint32_t) {},
          [&](uint32_t, const auto &) { ++applications; });
      height = ImGui::GetCursorScreenPos().y - start.y;
    };
    frame(draw);
    frame(draw);
    drag(draw, positionXControl(start, true));
    EXPECT_EQ(applications, 0);
    EXPECT_LT(height, ImGui::GetFrameHeightWithSpacing() * 2);
  }
}

TEST_F(HierarchyWidgetsTests, MeshSelectionDefersNumericEditorUntilItsSnapshotIsCurrent) {
  SceneGraph graph;
  const auto group = addNode(graph, "Group with distinct transform");
  const auto mesh = graph.createNode(glm::mat4{1}, 0, true);
  container::ui::GuiManager gui;
  uint32_t selected = group;
  container::ui::TransformControls groupTransform, meshTransform, appliedTransform;
  groupTransform.position = {50, 60, 70};
  meshTransform.position = {3, 4, 5};
  uint32_t applied = SceneGraph::kInvalidNode;
  int applications = 0;
  float selectionFrameHeight = -1;
  ImVec2 start;
  const auto draw = [&] {
    openTransformEditors();
    start = ImGui::GetCursorScreenPos();
    const auto snapshot = selected == group ? groupTransform : meshTransform;
    bool selectionChanged = false;
    gui.drawSceneNodeControls(graph, selected, snapshot,
        [&](uint32_t node) { selected = node; selectionChanged = true; },
        [&](uint32_t node, const auto &edited) {
          applied = node;
          appliedTransform = edited;
          if (node == mesh) meshTransform = edited;
          ++applications;
        });
    if (selectionChanged)
      selectionFrameHeight = ImGui::GetCursorScreenPos().y - start.y;
  };
  frame(draw);
  frame(draw);
  click(draw, {start.x + 40, start.y + ImGui::GetFrameHeight() * .5f});
  ASSERT_FALSE(context_->OpenPopupStack.empty());
  const auto *popup = context_->OpenPopupStack.back().Window;
  ASSERT_NE(popup, nullptr);
  // Only None and the renderable mesh belong in this picker; the group does not.
  const auto itemHeight = ImGui::GetTextLineHeightWithSpacing();
  click(draw, {popup->InnerRect.Min.x + 35, popup->DC.CursorStartPos.y + itemHeight * 1.5f});
  ASSERT_EQ(selected, mesh);
  EXPECT_GE(selectionFrameHeight, 0);
  EXPECT_LT(selectionFrameHeight, ImGui::GetFrameHeightWithSpacing() * 2)
      << "The new selection must not receive the previous group's transform snapshot";
  EXPECT_EQ(applications, 0);
  frame(draw);
  frame(draw);
  drag(draw, positionXControl(start, true));
  EXPECT_EQ(applications, 1);
  EXPECT_EQ(applied, mesh);
  EXPECT_GT(appliedTransform.position.x, 3);
  EXPECT_FLOAT_EQ(appliedTransform.position.y, 4);
  EXPECT_FLOAT_EQ(appliedTransform.position.z, 5);
  EXPECT_EQ(groupTransform.position, glm::vec3(50, 60, 70));
}

TEST_F(HierarchyWidgetsTests, LargeMeshPickerFocusesFinalSelectionAndKeepsNoneReachable) {
  SceneGraph graph;
  uint32_t selected = SceneGraph::kInvalidNode;
  constexpr uint32_t count = 10001;
  for (uint32_t index = 0; index < count; ++index) {
    if (index % 11 == 0) (void)addNode(graph, "Nonrenderable group");
    selected = graph.createNode(glm::mat4{1}, 0, true, index % 7);
  }
  ASSERT_GT(selected, count);
  const auto revision = graph.revision();
  container::ui::GuiManager gui;
  ImVec2 start;
  int selections = 0, applications = 0;
  const auto draw = [&] {
    start = ImGui::GetCursorScreenPos();
    gui.drawSceneNodeControls(graph, selected, {},
        [&](uint32_t node) { selected = node; ++selections; },
        [&](uint32_t, const auto &) { ++applications; });
  };
  frame(draw);
  frame(draw);
  click(draw, {start.x + 40, start.y + ImGui::GetFrameHeight() * .5f});
  frame(draw);
  frame(draw);
  ASSERT_FALSE(context_->OpenPopupStack.empty());
  auto *popup = context_->OpenPopupStack.back().Window;
  ASSERT_NE(popup, nullptr);
  EXPECT_GT(popup->ScrollMax.y, 100000);
  EXPECT_GT(popup->Scroll.y, popup->ScrollMax.y * .95f);
  EXPECT_LT(ImGui::GetDrawData()->TotalVtxCount, 5000);
  EXPECT_EQ(selections, 0);
  EXPECT_EQ(applications, 0);
  ImGui::SetScrollY(popup, 0);
  frame(draw);
  frame(draw);
  click(draw, {popup->InnerRect.Min.x + 35,
      popup->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * .5f});
  EXPECT_EQ(selected, SceneGraph::kInvalidNode);
  EXPECT_EQ(selections, 1);
  EXPECT_EQ(applications, 0);
  EXPECT_TRUE(context_->OpenPopupStack.empty());
  EXPECT_EQ(graph.revision(), revision);
}

TEST_F(HierarchyWidgetsTests, ClippedMeshRowsKeepGraphIdsAfterReorderingAndReload) {
  SceneGraph graph;
  const auto group = addNode(graph, "Selected group");
  const auto firstMesh = graph.createNode(glm::mat4{1}, 0, true);
  for (uint32_t index = 0; index < 10001; ++index) {
    if (index % 11 == 0) (void)addNode(graph, "Interspersed group");
    (void)graph.createNode(glm::mat4{1}, 0, true, index % 7);
  }
  graph.setRenderable(firstMesh, false);
  graph.setRenderable(firstMesh, true);
  ASSERT_EQ(graph.renderableNodes().back(), firstMesh);
  const auto revision = graph.revision();
  uint32_t selected = group;
  container::ui::GuiManager gui;
  ImVec2 start;
  int selections = 0, applications = 0;
  const auto draw = [&] {
    start = ImGui::GetCursorScreenPos();
    gui.drawSceneNodeControls(graph, selected, {},
        [&](uint32_t node) { selected = node; ++selections; },
        [&](uint32_t, const auto &) { ++applications; });
  };
  frame(draw);
  frame(draw);
  click(draw, {start.x + 40, start.y + ImGui::GetFrameHeight() * .5f});
  frame(draw);
  frame(draw);
  ASSERT_FALSE(context_->OpenPopupStack.empty());
  auto *popup = context_->OpenPopupStack.back().Window;
  ASSERT_NE(popup, nullptr);
  ImGui::SetScrollY(popup, popup->ScrollMax.y);
  frame(draw);
  frame(draw);
  const float finalY = popup->DC.CursorMaxPos.y - ImGui::GetTextLineHeight() * .5f;
  ASSERT_GT(finalY, popup->InnerRect.Min.y);
  ASSERT_LT(finalY, popup->InnerRect.Max.y);
  click(draw, {popup->InnerRect.Min.x + 35, finalY});
  EXPECT_EQ(selected, firstMesh);
  EXPECT_EQ(selections, 1);
  EXPECT_EQ(applications, 0);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_TRUE(context_->OpenPopupStack.empty());

  graph = SceneGraph{};
  const auto replacementMesh = graph.createNode(glm::mat4{1}, 0, true);
  ASSERT_EQ(graph.getNode(selected), nullptr);
  frame(draw);
  frame(draw);
  click(draw, {start.x + 40, start.y + ImGui::GetFrameHeight() * .5f});
  frame(draw);
  frame(draw);
  ASSERT_FALSE(context_->OpenPopupStack.empty());
  popup = context_->OpenPopupStack.back().Window;
  ASSERT_NE(popup, nullptr);
  click(draw, {popup->InnerRect.Min.x + 35,
      popup->DC.CursorStartPos.y + ImGui::GetTextLineHeightWithSpacing() * 1.5f});
  EXPECT_EQ(selected, replacementMesh);
  EXPECT_EQ(selections, 2);
  EXPECT_EQ(applications, 0);
}

TEST(SceneHierarchyCacheTests, PreorderAndDepthFollowExpandedStableNodeIds) {
  SceneGraph graph;
  const auto root = addNode(graph, "Root");
  const auto child = addNode(graph, "Child");
  const auto leaf = addNode(graph, "Leaf");
  const auto second = addNode(graph, "Second root");
  ASSERT_TRUE(graph.setParent(child, root));
  ASSERT_TRUE(graph.setParent(leaf, child));
  SceneHierarchyCache cache;
  EXPECT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(), 2u);
  EXPECT_EQ(cache.rows()[0].nodeIndex, root);
  EXPECT_EQ(cache.rows()[1].nodeIndex, second);
  EXPECT_TRUE(cache.setExpanded(root, true));
  EXPECT_TRUE(cache.setExpanded(child, true));
  EXPECT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(), 4u);
  EXPECT_EQ(cache.rows()[1].nodeIndex, child);
  EXPECT_EQ(cache.rows()[1].depth, 1u);
  EXPECT_EQ(cache.rows()[2].nodeIndex, leaf);
  EXPECT_EQ(cache.rows()[2].depth, 2u);
  EXPECT_EQ(cache.rows()[3].nodeIndex, second);
  EXPECT_EQ(cache.rows()[3].depth, 0u);
  EXPECT_FALSE(cache.rows()[2].hasChildren);
  EXPECT_TRUE(cache.rows()[0].expanded);
  EXPECT_TRUE(cache.setExpanded(root, false));
  cache.refresh(graph);
  ASSERT_EQ(cache.rows().size(), 2u);
  cache.setExpanded(root, true);
  cache.refresh(graph);
  EXPECT_EQ(cache.rows().size(), 4u); // Child expansion survives collapse.
}

TEST(SceneHierarchyCacheTests, UnchangedTransformsAndVisibilityKeepCachedRows) {
  SceneGraph graph;
  const auto root = addNode(graph, "Root");
  const auto child = addNode(graph, "Child");
  ASSERT_TRUE(graph.setParent(child, root));
  SceneHierarchyCache cache;
  cache.refresh(graph);
  cache.setExpanded(root, true);
  cache.refresh(graph);
  const auto rebuilds = cache.rebuildCount();
  const auto *rows = cache.rows().data();
  const auto *label = cache.rows()[0].label.data();
  const auto hierarchyRevision = graph.hierarchyRevision();
  graph.setLocalTransform(root,
                          glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3)));
  graph.setVisible(root, false);
  graph.setRenderable(child, true);
  graph.updateWorldTransforms();
  EXPECT_EQ(graph.hierarchyRevision(), hierarchyRevision);
  for (size_t frame = 0; frame < 100; ++frame) {
    EXPECT_FALSE(cache.refresh(graph));
    EXPECT_EQ(cache.rows().data(), rows);
    EXPECT_EQ(cache.rows()[0].label.data(), label);
  }
  EXPECT_EQ(cache.rebuildCount(), rebuilds);
}

TEST(SceneHierarchyCacheTests, ReparentAndRenamePreserveValidExpansion) {
  SceneGraph graph;
  const auto first = addNode(graph, "First");
  const auto second = addNode(graph, "Second");
  const auto branch = addNode(graph, "Branch");
  const auto leaf = addNode(graph, "Leaf");
  ASSERT_TRUE(graph.setParent(branch, first));
  ASSERT_TRUE(graph.setParent(leaf, branch));
  SceneHierarchyCache cache;
  cache.refresh(graph);
  for (auto node : {first, second, branch})
    cache.setExpanded(node, true);
  cache.refresh(graph);
  ASSERT_TRUE(graph.setParent(branch, second));
  graph.setName(branch, "Renamed branch");
  EXPECT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(), 4u);
  EXPECT_EQ(cache.rows()[2].nodeIndex, branch);
  EXPECT_TRUE(cache.rows()[2].expanded);
  EXPECT_EQ(cache.rows()[2].label, "Renamed branch [2]");
  EXPECT_EQ(cache.rows()[3].nodeIndex, leaf);
  EXPECT_EQ(cache.rows()[3].depth, 2u);
  const auto revision = graph.hierarchyRevision();
  graph.setName(branch, "Renamed branch");
  EXPECT_EQ(graph.hierarchyRevision(), revision);
  EXPECT_FALSE(cache.refresh(graph));
}

TEST(SceneHierarchyCacheTests,
     SameAddressAndRevisionReplacementResetsOldExpansion) {
  SceneGraph graph;
  addNode(graph, "Old root");
  addNode(graph, "Old child");
  ASSERT_TRUE(graph.setParent(1, 0));
  SceneHierarchyCache cache;
  cache.refresh(graph);
  cache.setExpanded(0, true);
  cache.refresh(graph);
  const auto oldIdentity = graph.identity();
  const auto oldRevision = graph.hierarchyRevision();
  graph = SceneGraph{};
  addNode(graph, "New root");
  addNode(graph, "New child");
  ASSERT_TRUE(graph.setParent(1, 0));
  ASSERT_EQ(graph.hierarchyRevision(), oldRevision);
  ASSERT_NE(graph.identity(), oldIdentity);
  EXPECT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(), 1u);
  EXPECT_EQ(cache.rows()[0].label, "New root [0]");
  EXPECT_FALSE(cache.rows()[0].expanded);
  graph = SceneGraph{};
  EXPECT_TRUE(cache.refresh(graph));
  EXPECT_TRUE(cache.rows().empty());
  addNode(graph, "Reloaded root");
  EXPECT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(), 1u);
  EXPECT_EQ(cache.rows()[0].label, "Reloaded root [0]");
}

TEST(SceneHierarchyCacheTests, TenThousandLevelsUseBoundedIterativeTraversal) {
  SceneGraph graph;
  constexpr uint32_t count = 10001;
  auto root = addNode(graph, "Leaf");
  for (uint32_t index = 1; index < count; ++index) {
    const auto parent = addNode(graph, "Parent");
    ASSERT_TRUE(graph.setParent(root, parent));
    root = parent;
  }
  SceneHierarchyCache cache;
  cache.refresh(graph);
  for (uint32_t index = 0; index < count; ++index)
    cache.setExpanded(index, true);
  ASSERT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(), count);
  EXPECT_EQ(cache.rows().front().nodeIndex, count - 1);
  EXPECT_EQ(cache.rows().back().nodeIndex, 0u);
  EXPECT_EQ(cache.rows().back().depth, count - 1);
  EXPECT_FALSE(cache.refresh(graph));
}

TEST(SceneHierarchyCacheTests,
     DivergentCopiesCannotAliasReplacementRevisionKeys) {
  SceneGraph original;
  addNode(original, "Original root");
  addNode(original, "Original child");
  ASSERT_TRUE(original.setParent(1, 0));
  SceneGraph first(original);
  SceneGraph second(original);
  ASSERT_NE(first.identity(), original.identity());
  ASSERT_NE(second.identity(), first.identity());
  first.setName(0, "First copy");
  second.setName(0, "Second copy");
  ASSERT_EQ(first.hierarchyRevision(), second.hierarchyRevision());
  SceneGraph graph;
  graph = first;
  SceneHierarchyCache cache;
  cache.refresh(graph);
  cache.setExpanded(0, true);
  cache.refresh(graph);
  ASSERT_EQ(cache.rows().size(), 2u);
  const auto previousIdentity = graph.identity();
  graph = second;
  ASSERT_NE(graph.identity(), previousIdentity);
  ASSERT_EQ(graph.hierarchyRevision(), first.hierarchyRevision());
  ASSERT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(), 1u);
  EXPECT_EQ(cache.rows()[0].label, "Second copy [0]");
  EXPECT_FALSE(cache.rows()[0].expanded);
  const auto identity = graph.identity();
  graph = graph;
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_FALSE(cache.refresh(graph));
}

TEST(SceneHierarchyCacheTests, MoveAndMovedFromReuseKeepIndependentIdentities) {
  SceneGraph source;
  addNode(source, "Moved root");
  const auto sourceIdentity = source.identity();
  SceneHierarchyCache cache;
  cache.refresh(source);
  SceneGraph destination(std::move(source));
  EXPECT_NE(source.identity(), sourceIdentity);
  EXPECT_NE(destination.identity(), sourceIdentity);
  EXPECT_NE(source.identity(), destination.identity());
  EXPECT_EQ(source.nodeCount(), 0u);
  EXPECT_EQ(source.hierarchyRevision(), 0u);
  ASSERT_TRUE(cache.refresh(source));
  EXPECT_TRUE(cache.rows().empty());
  addNode(source, "Reused source");
  ASSERT_TRUE(cache.refresh(source));
  ASSERT_EQ(cache.rows().size(), 1u);
  EXPECT_EQ(cache.rows()[0].label, "Reused source [0]");
  const auto reusedIdentity = source.identity();
  source = std::move(destination);
  EXPECT_NE(source.identity(), reusedIdentity);
  EXPECT_EQ(destination.nodeCount(), 0u);
  ASSERT_TRUE(cache.refresh(source));
  ASSERT_EQ(cache.rows().size(), 1u);
  EXPECT_EQ(cache.rows()[0].label, "Moved root [0]");
}

TEST(SceneHierarchyCacheTests,
     MultilineNamesKeepOneDisplayLineAndOriginalText) {
  SceneGraph graph;
  const std::string original = "First line\r\nSecond line\tTabbed";
  const auto node = addNode(graph, original);
  SceneHierarchyCache cache;
  ASSERT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(), 1u);
  EXPECT_EQ(cache.rows()[0].label, "First line  Second line Tabbed [0]");
  EXPECT_EQ(graph.getNode(node)->name, original);
  EXPECT_FALSE(cache.refresh(graph));
}

TEST_F(HierarchyWidgetsTests,
       ExpandedTenThousandRowsClipAndFinalNodeSelectsAndFocuses) {
  SceneGraph graph;
  const auto root = addNode(graph, "Root\nwith a second line");
  constexpr uint32_t childCount = 10001;
  for (uint32_t index = 0; index < childCount; ++index) {
    std::string name = "Child " + std::to_string(index);
    if (index % 1000u == 0u)
      name += "\r\nwith a second line\tand a tab";
    const auto child = addNode(graph, std::move(name));
    ASSERT_TRUE(graph.setParent(child, root));
  }
  container::ui::GuiManager gui;
  gui.setSceneHierarchyNodeExpanded(graph, root, true);
  uint32_t selected = SceneGraph::kInvalidNode;
  uint32_t focused = SceneGraph::kInvalidNode;
  const auto draw = [&] {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    gui.drawSceneHierarchy(
        graph, root, selected, [&](uint32_t node) { selected = node; },
        [&](uint32_t node) { focused = node; },
        [&](uint32_t node, bool visible) { graph.setVisible(node, visible); },
        [&](uint32_t node, std::optional<uint32_t> parent) {
          graph.setParent(node, parent);
        });
  };
  frame(draw);
  frame(draw);
  auto *table = hierarchyTable();
  ASSERT_NE(table, nullptr);
  EXPECT_GT(table->InnerWindow->ScrollMax.y, 200000.0f);
  EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows, childCount + 1u);
  EXPECT_LT(gui.sceneHierarchyStatistics().submittedRows, 40u);
  const auto rebuilds = gui.sceneHierarchyStatistics().cacheRebuilds;
  graph.setLocalTransform(root,
                          glm::translate(glm::mat4(1.0f), glm::vec3(0, 1, 0)));
  frame(draw);
  EXPECT_EQ(gui.sceneHierarchyStatistics().cacheRebuilds, rebuilds);
  ImGui::SetScrollY(table->InnerWindow, table->InnerWindow->ScrollMax.y);
  frame(draw);
  frame(draw);
  const float y = (table->RowPosY1 + table->RowPosY2) * 0.5f;
  ASSERT_LT(y, table->InnerWindow->InnerRect.Max.y);
  ASSERT_GT(y, table->InnerWindow->InnerRect.Min.y);
  click(draw, ImVec2(table->Columns[0].WorkMinX + 65.0f, y));
  EXPECT_EQ(selected, childCount);
  click(draw, ImVec2(table->Columns[2].WorkMinX + 20.0f, y));
  EXPECT_EQ(focused, childCount);
  EXPECT_EQ(gui.sceneHierarchyStatistics().cacheRebuilds, rebuilds);
  EXPECT_LT(gui.sceneHierarchyStatistics().submittedRows, 40u);
}

TEST_F(HierarchyWidgetsTests, InteractionCheckboxesRecordIndependentUndoCommands) {
  windowWidth_ = 1100;
  ImGui::GetIO().DisplaySize = ImVec2(1200, 480);
  SceneGraph graph;
  const auto node = addNode(graph, "Authored flags");
  container::util::SceneEditHistory history(graph);
  container::ui::GuiManager gui;
  const auto draw = [&] {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    gui.drawSceneHierarchy(graph, node, node, [](uint32_t) {}, [](uint32_t) {},
        [&](uint32_t id, bool value) { history.setVisible(id, value); },
        [](uint32_t, std::optional<uint32_t>) {},
        [&](uint32_t id, bool value) { history.setSelectable(id, value); },
        [&](uint32_t id, bool value) { history.setHoverable(id, value); });
  };
  frame(draw); frame(draw);
  auto* table = hierarchyTable();
  ASSERT_NE(table, nullptr);
  const auto checkbox = [&](int column) {
    return ImVec2(table->Columns[column].WorkMinX + ImGui::GetFrameHeight() * .5f,
                  (table->RowPosY1 + table->RowPosY2) * .5f);
  };
  click(draw, checkbox(4));
  EXPECT_FALSE(graph.getNode(node)->selectable);
  EXPECT_TRUE(graph.getNode(node)->hoverable);
  EXPECT_EQ(history.size(), 1u);
  click(draw, checkbox(5));
  EXPECT_FALSE(graph.getNode(node)->hoverable);
  EXPECT_TRUE(graph.getNode(node)->visible);
  EXPECT_EQ(history.size(), 2u);
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.getNode(node)->hoverable);
  EXPECT_FALSE(graph.getNode(node)->selectable);
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.getNode(node)->selectable);
}

TEST_F(HierarchyWidgetsTests, ArrowExpansionAndReplacementKeepRowsConsistent) {
  SceneGraph graph;
  const auto root = addNode(graph, "Root");
  const auto child = addNode(graph, "Child");
  ASSERT_TRUE(graph.setParent(child, root));
  container::ui::GuiManager gui;
  const auto draw = [&] {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    gui.drawSceneHierarchy(
        graph, root, child, [](uint32_t) {}, [](uint32_t) {},
        [](uint32_t, bool) {}, [](uint32_t, std::optional<uint32_t>) {});
  };
  frame(draw);
  frame(draw);
  auto *table = hierarchyTable();
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(gui.sceneHierarchyStatistics().visibleRows, 1u);
  const ImVec2 arrow(table->Columns[0].WorkMinX + ImGui::GetFontSize() * 0.5f,
                     (table->RowPosY1 + table->RowPosY2) * 0.5f);
  click(draw, arrow);
  frame(draw);
  EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows, 2u);
  graph.setName(root, "Renamed root");
  frame(draw);
  EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows, 2u);
  click(draw, arrow);
  frame(draw);
  EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows, 1u);
  gui.setSceneHierarchyNodeExpanded(graph, root, true);
  frame(draw);
  EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows, 2u);
  graph = SceneGraph{};
  addNode(graph, "Replacement root");
  addNode(graph, "Replacement child");
  ASSERT_TRUE(graph.setParent(1, 0));
  frame(draw);
  EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows, 1u);
  gui.setSceneHierarchyNodeExpanded(graph, root, true);
  frame(draw);
  gui.resetModelInspectionState();
  frame(draw);
  EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows, 1u);
}

TEST_F(HierarchyWidgetsTests, ReparentComboAppliesAfterRowsAndKeepsSelection) {
  SceneGraph graph;
  const auto root = addNode(graph, "Protected root");
  const auto parent = addNode(graph, "New parent");
  const auto child = addNode(graph, "Reparent me");
  container::ui::GuiManager gui;
  gui.setSceneHierarchyNodeExpanded(graph, parent, true);
  uint32_t selected = child;
  uint32_t reparented = SceneGraph::kInvalidNode;
  bool callbackDuringTable = true;
  const auto draw = [&] {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    gui.drawSceneHierarchy(
        graph, root, selected, [&](uint32_t node) { selected = node; },
        [](uint32_t) {}, [](uint32_t, bool) {},
        [&](uint32_t node, std::optional<uint32_t> target) {
          callbackDuringTable = ImGui::GetCurrentTable() != nullptr;
          reparented = node;
          graph.setParent(node, target);
        });
  };
  frame(draw);
  frame(draw);
  auto *table = hierarchyTable();
  ASSERT_NE(table, nullptr);
  const float finalY = (table->RowPosY1 + table->RowPosY2) * 0.5f;
  // Scroll horizontally so the full parent cell (and combo) is reachable.
  ImGui::SetScrollX(table->InnerWindow, table->InnerWindow->ScrollMax.x);
  frame(draw);
  frame(draw);
  click(draw, ImVec2(table->Columns[3].WorkMinX + 30.0f, finalY));
  ASSERT_FALSE(context_->OpenPopupStack.empty());
  auto *popup = context_->OpenPopupStack.back().Window;
  ASSERT_NE(popup, nullptr);
  // The candidates are None, Protected root, New parent; self is excluded.
  const float itemHeight = ImGui::GetTextLineHeightWithSpacing();
  click(draw, ImVec2(popup->InnerRect.Min.x + 30.0f,
                     popup->DC.CursorStartPos.y + itemHeight * 2.5f));
  frame(draw);
  EXPECT_EQ(reparented, child);
  EXPECT_EQ(graph.getNode(child)->parent, parent);
  EXPECT_FALSE(callbackDuringTable);
  EXPECT_EQ(selected, child);
  EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows, 3u);
}

TEST_F(HierarchyWidgetsTests,
       ParentNearEndOfTenThousandCandidatesGetsPopupFocus) {
  SceneGraph graph;
  constexpr uint32_t parentIndex = 10001;
  for (uint32_t index = 0; index <= parentIndex; ++index) {
    addNode(graph, "Candidate\n" + std::to_string(index));
  }
  ASSERT_TRUE(graph.setParent(0, parentIndex));
  container::ui::GuiManager gui;
  gui.setSceneHierarchyNodeExpanded(graph, parentIndex, true);
  const auto draw = [&] {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    gui.drawSceneHierarchy(
        graph, SceneGraph::kInvalidNode, 0, [](uint32_t) {}, [](uint32_t) {},
        [](uint32_t, bool) {}, [](uint32_t, std::optional<uint32_t>) {});
  };
  frame(draw);
  frame(draw);
  auto *table = hierarchyTable();
  ASSERT_NE(table, nullptr);
  ImGui::SetScrollY(table->InnerWindow, table->InnerWindow->ScrollMax.y);
  ImGui::SetScrollX(table->InnerWindow, table->InnerWindow->ScrollMax.x);
  frame(draw);
  frame(draw);
  const float finalY = (table->RowPosY1 + table->RowPosY2) * 0.5f;
  click(draw, ImVec2(table->Columns[3].WorkMinX + 30.0f, finalY));
  frame(draw);
  frame(draw);
  ASSERT_FALSE(context_->OpenPopupStack.empty());
  const auto *popup = context_->OpenPopupStack.back().Window;
  ASSERT_NE(popup, nullptr);
  EXPECT_GT(popup->ScrollMax.y, 100000.0f);
  EXPECT_GT(popup->Scroll.y, popup->ScrollMax.y * 0.95f);
  EXPECT_LT(gui.sceneHierarchyStatistics().submittedRows, 40u);
}

TEST(SceneHierarchyActiveScene, FiltersInactiveBranchesAndPreservesExpansionAcrossViews) {
  SceneGraph graph;
  const auto root=addNode(graph,"Container"),a=addNode(graph,"Scene A"),b=addNode(graph,"Scene B");
  graph.setParent(a,root);graph.setParent(b,root);
  SceneHierarchyCache cache;
  graph.setActiveRoots(std::array<uint32_t,1>{a});
  ASSERT_TRUE(cache.refresh(graph));cache.setExpanded(root,true);ASSERT_TRUE(cache.refresh(graph));
  ASSERT_EQ(cache.rows().size(),2u);
  EXPECT_TRUE(cache.rowForNode(a));EXPECT_FALSE(cache.rowForNode(b));
  graph.setName(a,"Edited inactive label");
  graph.setActiveRoots(std::array<uint32_t,1>{b});
  ASSERT_TRUE(cache.refresh(graph));ASSERT_EQ(cache.rows().size(),2u);
  EXPECT_FALSE(cache.rowForNode(a));EXPECT_TRUE(cache.rowForNode(b));
  graph.setActiveRoots(std::array<uint32_t,1>{a});ASSERT_TRUE(cache.refresh(graph));
  ASSERT_TRUE(cache.rowForNode(a));
  EXPECT_NE(cache.rows()[*cache.rowForNode(a)].label.find("Edited inactive label"),std::string::npos);
}

TEST(SceneHierarchyActiveScene, RetirementHidesStableSlotsAndUndoRestoresCachedRowOrder) {
  SceneGraph graph;
  const auto root = addNode(graph, "Container");
  const auto first = addNode(graph, "First");
  const auto second = addNode(graph, "Second");
  ASSERT_TRUE(graph.setParent(first, root));
  ASSERT_TRUE(graph.setParent(second, root));
  SceneHierarchyCache cache;
  cache.refresh(graph);
  cache.setExpanded(root, true);
  cache.refresh(graph);
  ASSERT_EQ(cache.rows().size(), 3u);
  const auto firstRow = cache.rowForNode(first);
  const auto secondRow = cache.rowForNode(second);
  auto prepared = graph.prepareRetireSubtree(first);
  ASSERT_TRUE(graph.publishRetirement(prepared));
  auto receipt = prepared.takeRetirement();
  ASSERT_TRUE(receipt);
  cache.refresh(graph);
  EXPECT_FALSE(cache.rowForNode(first));
  ASSERT_EQ(cache.rows().size(), 2u);
  EXPECT_TRUE(cache.rowForNode(second));
  ASSERT_NE(graph.getNode(first), nullptr);
  auto undo = graph.prepareRetirementChange(*receipt, false);
  ASSERT_TRUE(graph.publishRetirement(undo));
  cache.refresh(graph);
  EXPECT_EQ(cache.rowForNode(first), firstRow);
  EXPECT_EQ(cache.rowForNode(second), secondRow);
}

TEST_F(HierarchyWidgetsTests, InlineRenameUsesRealTextInputAndQueuesOneExactUtf8HistoryEdit) {
  SceneGraph graph;const auto node=addNode(graph,"Imported original");
  graph.getNode(node)->sourceNodeIndex=7;graph.getNode(node)->materialIndex=13;
  const auto local=glm::translate(glm::mat4(1),glm::vec3(2,3,4));graph.setLocalTransform(node,local);
  container::ui::GuiManager gui;container::util::SceneEditHistory history(graph);
  const auto draw=[&] {
    ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    gui.drawSceneHierarchy(graph,SceneGraph::kInvalidNode,node,[](uint32_t){},[](uint32_t){},
        [](uint32_t,bool){},[](uint32_t,std::optional<uint32_t>){});
  };
  frame(draw);frame(draw);auto* table=hierarchyTable();ASSERT_NE(table,nullptr);
  const float rowHeight=table->RowPosY2-table->RowPosY1;beginInline(draw);
  ASSERT_NE(context_->ActiveId,0u);ASSERT_EQ(context_->ActiveId,context_->InputTextState.ID);
  EXPECT_FLOAT_EQ(table->RowPosY2-table->RowPosY1,rowHeight);
  EXPECT_TRUE(context_->OpenPopupStack.empty());
  replaceText(draw,"Fa\xc3\xa7" "ade \xe6\x9d\xb1");
  EXPECT_EQ(graph.getNode(node)->name,"Imported original");EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
  key(draw,ImGuiKey_Enter);const auto request=gui.consumeSceneNodeAuthoringRequest();ASSERT_TRUE(request);
  EXPECT_EQ(request->action,container::ui::SceneNodeAuthoringAction::Rename);
  EXPECT_EQ(request->graphIdentity,graph.identity());EXPECT_EQ(request->node,node);
  EXPECT_EQ(request->name,"Fa\xc3\xa7" "ade \xe6\x9d\xb1");EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
  ASSERT_TRUE(history.setName(request->node,request->name));EXPECT_EQ(history.undoCount(),1u);
  ASSERT_TRUE(history.undo());EXPECT_EQ(graph.getNode(node)->name,"Imported original");
  ASSERT_TRUE(history.redo());EXPECT_EQ(graph.getNode(node)->name,request->name);
  EXPECT_EQ(graph.getNode(node)->localTransform,local);EXPECT_EQ(graph.getNode(node)->sourceNodeIndex,7);
  EXPECT_EQ(graph.getNode(node)->materialIndex,13u);
}

TEST_F(HierarchyWidgetsTests, InlineRenameInvalidInputStaysEditableAndEscapeOrUnchangedEnterQueuesNothing) {
  SceneGraph graph;const auto node=addNode(graph,"Keep exact name");container::ui::GuiManager gui;
  const auto draw=[&] {
    ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    gui.drawSceneHierarchy(graph,SceneGraph::kInvalidNode,node,[](uint32_t){},[](uint32_t){},
        [](uint32_t,bool){},[](uint32_t,std::optional<uint32_t>){});
  };
  frame(draw);frame(draw);beginInline(draw);replaceText(draw,"   ");key(draw,ImGuiKey_Enter);
  frame(draw);frame(draw);EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
  ASSERT_NE(context_->ActiveId,0u);EXPECT_EQ(context_->ActiveId,context_->InputTextState.ID);
  replaceText(draw,"Cancelled draft");key(draw,ImGuiKey_Escape);
  EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());EXPECT_EQ(graph.getNode(node)->name,"Keep exact name");
  beginInline(draw);key(draw,ImGuiKey_Enter);
  EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());EXPECT_EQ(context_->ActiveId,0u);
}

TEST_F(HierarchyWidgetsTests, InlineRenameCancelsStaleDraftsAndNeverReusesAnOldInputSession) {
  for(int change=0;change<6;++change) {
    SceneGraph graph;const auto node=addNode(graph,"Original target");container::ui::GuiManager gui;
    const auto draw=[&] {
      ImGui::SetNextItemOpen(true,ImGuiCond_Always);
      gui.drawSceneHierarchy(graph,SceneGraph::kInvalidNode,node,[](uint32_t){},[](uint32_t){},
          [](uint32_t,bool){},[](uint32_t,std::optional<uint32_t>){});
    };
    frame(draw);frame(draw);beginInline(draw);replaceText(draw,"Stale draft");
    ASSERT_NE(context_->ActiveId,0u);const auto oldInput=context_->ActiveId;
    if(change==0) {graph=SceneGraph{};addNode(graph,"Replacement at same slot");}
    if(change==1) {auto retirement=graph.prepareRetireSubtree(node);ASSERT_TRUE(retirement);ASSERT_TRUE(graph.publishRetirement(retirement));}
    if(change==2) addNode(graph,"Unrelated topology addition");
    if(change==3) graph.setActiveRoots(std::array<uint32_t,0>{});
    if(change==4) graph.setName(node,"Conflicting external rename");
    if(change==5) gui.resetModelInspectionState();
    frame(draw);key(draw,ImGuiKey_Enter);EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
    EXPECT_NE(graph.getNode(node)->name,"Stale draft");
    if(change!=1 && change!=3) {
      beginInline(draw);ASSERT_NE(context_->ActiveId,0u);EXPECT_NE(context_->ActiveId,oldInput);
      key(draw,ImGuiKey_Enter);EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
    }
  }
}

TEST_F(HierarchyWidgetsTests, InlineRenameSurvivesAnimatedTransformsWithoutMutatingThem) {
  SceneGraph graph;const auto node=addNode(graph,"Animated label");container::ui::GuiManager gui;
  const auto draw=[&] {
    ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    gui.drawSceneHierarchy(graph,SceneGraph::kInvalidNode,node,[](uint32_t){},[](uint32_t){},
        [](uint32_t,bool){},[](uint32_t,std::optional<uint32_t>){});
  };
  frame(draw);frame(draw);beginInline(draw);replaceText(draw,"Authored name during playback");
  const auto transform=glm::translate(glm::mat4(1),glm::vec3(9,8,7));graph.setLocalTransform(node,transform);
  frame(draw);key(draw,ImGuiKey_Enter);const auto request=gui.consumeSceneNodeAuthoringRequest();ASSERT_TRUE(request);
  EXPECT_EQ(request->name,"Authored name during playback");EXPECT_EQ(graph.getNode(node)->localTransform,transform);
  EXPECT_EQ(graph.getNode(node)->name,"Animated label");
}

TEST_F(HierarchyWidgetsTests, InlineRenameResolvesSyntheticOwnerAndRevealsItsCollapsedAncestors) {
  SceneGraph graph;const auto root=addNode(graph,"Structural editor root");graph.getNode(root)->editorSceneRoot=true;
  const auto parent=addNode(graph,"Collapsed parent"),owner=addNode(graph,"Imported owner");
  graph.getNode(owner)->sourceNodeIndex=0;ASSERT_TRUE(graph.setParent(parent,root));ASSERT_TRUE(graph.setParent(owner,parent));
  const auto primitive=graph.createNode(glm::mat4(1),0,true,0,"Synthetic primitive label");
  graph.getNode(primitive)->meshOwnerGraphNode=owner;ASSERT_TRUE(graph.setParent(primitive,owner));
  container::ui::GuiManager gui;
  const auto draw=[&] {
    ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    gui.drawSceneHierarchy(graph,root,primitive,[](uint32_t){},[](uint32_t){},
        [](uint32_t,bool){},[](uint32_t,std::optional<uint32_t>){});
  };
  frame(draw);frame(draw);ASSERT_EQ(gui.sceneHierarchyStatistics().visibleRows,1u);
  beginInline(draw);ASSERT_NE(context_->ActiveId,0u);EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows,3u);
  replaceText(draw,"Renamed actual owner");key(draw,ImGuiKey_Enter);
  const auto request=gui.consumeSceneNodeAuthoringRequest();ASSERT_TRUE(request);
  EXPECT_EQ(request->node,owner);EXPECT_EQ(request->name,"Renamed actual owner");
  EXPECT_EQ(graph.getNode(primitive)->name,"Synthetic primitive label");
}

TEST_F(HierarchyWidgetsTests, InlineRenameKeepsTenThousandRowsClippedWhileItsEditorIsOffscreen) {
  SceneGraph graph;constexpr uint32_t last=10001;
  for(uint32_t id=0;id<=last;++id) addNode(graph,"Visible row "+std::to_string(id));
  container::ui::GuiManager gui;
  const auto draw=[&] {
    ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    gui.drawSceneHierarchy(graph,SceneGraph::kInvalidNode,last,[](uint32_t){},[](uint32_t){},
        [](uint32_t,bool){},[](uint32_t,std::optional<uint32_t>){});
  };
  frame(draw);frame(draw);beginInline(draw);ASSERT_NE(context_->ActiveId,0u);
  auto* table=hierarchyTable();ASSERT_NE(table,nullptr);
  EXPECT_GT(table->InnerWindow->Scroll.y,table->InnerWindow->ScrollMax.y*.95f);
  EXPECT_LT(gui.sceneHierarchyStatistics().submittedRows,40u);
  ImGui::SetScrollY(table->InnerWindow,0);frame(draw);frame(draw);
  EXPECT_LT(gui.sceneHierarchyStatistics().submittedRows,40u);EXPECT_EQ(gui.sceneHierarchyStatistics().visibleRows,size_t(last)+1);
  replaceText(draw,"Clipped target renamed");key(draw,ImGuiKey_Enter);
  const auto request=gui.consumeSceneNodeAuthoringRequest();ASSERT_TRUE(request);
  EXPECT_EQ(request->node,last);EXPECT_EQ(request->name,"Clipped target renamed");
}

TEST_F(HierarchyWidgetsTests, ContextInlineRenameAndModalFallbackOwnOnlyOneEditor) {
  SceneGraph graph;const auto node=addNode(graph,"Context target");container::ui::GuiManager gui;
  const auto draw=[&] {
    ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    gui.drawSceneHierarchy(graph,SceneGraph::kInvalidNode,node,[](uint32_t){},[](uint32_t){},
        [](uint32_t,bool){},[](uint32_t,std::optional<uint32_t>){});
  };
  frame(draw);frame(draw);auto* table=hierarchyTable();ASSERT_NE(table,nullptr);
  const auto rowPoint=[&] {return ImVec2(table->Columns[0].WorkMinX+ImGui::GetFontSize()*.5f,(table->RowPosY1+table->RowPosY2)*.5f);};
  contextMenu(draw,rowPoint());ASSERT_FALSE(context_->OpenPopupStack.empty());
  auto* popup=context_->OpenPopupStack.back().Window;ASSERT_NE(popup,nullptr);
  click(draw,{popup->DC.CursorStartPos.x+35,popup->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()*.5f});
  frame(draw);frame(draw);ASSERT_NE(context_->ActiveId,0u);EXPECT_TRUE(context_->OpenPopupStack.empty());
  replaceText(draw,"Uncommitted inline text");contextMenu(draw,rowPoint());ASSERT_FALSE(context_->OpenPopupStack.empty());
  popup=context_->OpenPopupStack.back().Window;ASSERT_NE(popup,nullptr);
  click(draw,{popup->DC.CursorStartPos.x+35,popup->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()*1.5f});
  frame(draw);frame(draw);ASSERT_FALSE(context_->OpenPopupStack.empty());
  EXPECT_TRUE(context_->OpenPopupStack.back().Window->Flags & ImGuiWindowFlags_Modal);
  EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());EXPECT_EQ(graph.getNode(node)->name,"Context target");
}
