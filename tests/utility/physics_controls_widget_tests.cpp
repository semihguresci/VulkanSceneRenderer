#include "Container/utility/PhysicsControls.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

using container::physics::PhysicsBodySettings;
using container::physics::PhysicsMotionType;
using container::ui::PhysicsPanelOptions;
using container::ui::PhysicsPanelState;

class PhysicsControlsWidgetTests : public ::testing::Test {
protected:
  void SetUp() override {
    previousContext_ = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2{800.0f, 800.0f};
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    state_.available = true;
    state_.selectionAvailable = true;
    state_.bodyCount = 2u;
    state_.activeBodyCount = 1u;
    state_.attachSelected = [&](const PhysicsBodySettings &settings) {
      ++attachCalls_;
      attachedSettings_ = settings;
    };
    state_.detachSelected = [&] { ++detachCalls_; };
    state_.setPlaying = [&](bool playing) {
      ++playCalls_;
      requestedPlaying_ = playing;
    };
    state_.singleStep = [&] { ++stepCalls_; };
    state_.reset = [&] { ++resetCalls_; };
    state_.clear = [&] { ++clearCalls_; };
    state_.createDemo = [&] { ++demoCalls_; };
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previousContext_);
  }

  void frame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
    ImGui::SetNextWindowSize(ImVec2{760.0f, 740.0f});
    ImGui::Begin("Physics controls widget test", nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    origin_ = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    container::ui::drawPhysicsPanel(state_, options_);
    ImGui::End();
    ImGui::Render();
  }

  void settle() { frame(); frame(); }

  void click(ImVec2 position) {
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(position.x, position.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
  }

  float controlsTop() const {
    const float line = ImGui::GetTextLineHeightWithSpacing();
    float top = origin_.y + line; // Expanded tree heading.
    if (!state_.available)
      top += line;
    top += 2.0f * line; // Body and step statistics.
    if ((state_.selectedHasBody && state_.selectedMotion) || !state_.selectionAvailable)
      top += line;
    return top;
  }

  ImVec2 controlPosition(int row) const {
    return {origin_.x + ImGui::GetStyle().IndentSpacing + 40.0f,
            controlsTop() + row * ImGui::GetFrameHeightWithSpacing() +
                ImGui::GetFrameHeight() * 0.5f};
  }

  float buttonWidth(std::string_view label) const {
    return ImGui::CalcTextSize(label.data(), label.data() + label.size()).x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
  }

  void clickButton(std::string_view label) {
    const std::string_view attach = state_.selectedHasBody ? "Replace body" : "Attach body";
    const std::string_view play = state_.playing ? "Pause" : "Play";
    float x = origin_.x + ImGui::GetStyle().IndentSpacing;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    int row = 6;
    if (label == "Detach body")
      x += buttonWidth(attach) + spacing;
    else if (label == "Play" || label == "Pause")
      row = 7;
    else if (label == "Single step") {
      row = 7;
      x += buttonWidth(play) + spacing;
    } else if (label == "Reset") {
      row = 7;
      x += buttonWidth(play) + spacing + buttonWidth("Single step") + spacing;
    } else if (label == "Clear bodies") {
      row = 8;
    } else if (label == "Create demo") {
      row = 8;
      x += buttonWidth("Clear bodies") + spacing;
    }
    const float y = controlsTop() + row * ImGui::GetFrameHeightWithSpacing() +
                    ImGui::GetFrameHeight() * 0.5f;
    click({x + buttonWidth(label) * 0.5f, y});
  }

  void chooseMotion(PhysicsMotionType motion) {
    click(controlPosition(0));
    // Combo popups spend their opening frame measuring content before their
    // final placement. Resolve row coordinates after that placement settles.
    settle();
    ImGuiWindow *popup{};
    for (auto *window : context_->Windows)
      if (window->Active && std::string_view{window->Name}.starts_with("##Combo_"))
        popup = window;
    ASSERT_NE(popup, nullptr);
    const float y = popup->DC.CursorStartPos.y +
        static_cast<int>(motion) * ImGui::GetTextLineHeightWithSpacing() +
        ImGui::GetTextLineHeight() * 0.5f;
    click({popup->DC.CursorStartPos.x + 25.0f, y});
    settle();
    EXPECT_EQ(options_.motionType, motion);
  }

  void enterMass(std::string_view value) {
    enterParameter(1, value);
  }

  void enterParameter(int row, std::string_view value) {
    click(controlPosition(row));
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    const std::string text{value};
    io.AddInputCharactersUTF8(text.c_str());
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    settle();
  }

  int totalCalls() const {
    return attachCalls_ + detachCalls_ + playCalls_ + stepCalls_ + resetCalls_ +
        clearCalls_ + demoCalls_;
  }

  PhysicsPanelState state_;
  PhysicsPanelOptions options_;
  ImGuiContext *previousContext_{};
  ImGuiContext *context_{};
  ImVec2 origin_{};
  std::optional<PhysicsBodySettings> attachedSettings_;
  bool requestedPlaying_{false};
  int attachCalls_{0}, detachCalls_{0}, playCalls_{0}, stepCalls_{0};
  int resetCalls_{0}, clearCalls_{0}, demoCalls_{0};
};

TEST_F(PhysicsControlsWidgetTests, FramesDoNotTriggerActionsAndClicksFireOnce) {
  state_.selectedHasBody = true;
  state_.selectedMotion = PhysicsMotionType::Dynamic;
  state_.totalSteps = 120u;
  state_.error = "A previous collider could not be attached.";
  settle();
  for (int index = 0; index < 20; ++index)
    frame();
  EXPECT_EQ(totalCalls(), 0);
  clickButton("Replace body");
  EXPECT_EQ(attachCalls_, 1);
  ASSERT_TRUE(attachedSettings_);
  EXPECT_EQ(attachedSettings_->motionType, PhysicsMotionType::Dynamic);
  clickButton("Detach body");
  EXPECT_EQ(detachCalls_, 1);
  clickButton("Play");
  EXPECT_EQ(playCalls_, 1);
  EXPECT_TRUE(requestedPlaying_);
  state_.playing = true;
  settle();
  clickButton("Pause");
  EXPECT_EQ(playCalls_, 2);
  EXPECT_FALSE(requestedPlaying_);
  clickButton("Single step");
  EXPECT_EQ(stepCalls_, 1);
  clickButton("Reset");
  EXPECT_EQ(resetCalls_, 1);
  clickButton("Clear bodies");
  EXPECT_EQ(clearCalls_, 1);
  clickButton("Create demo");
  EXPECT_EQ(demoCalls_, 1);
  const int beforeIdle = totalCalls();
  for (int index = 0; index < 20; ++index)
    frame();
  EXPECT_EQ(totalCalls(), beforeIdle);
}

TEST_F(PhysicsControlsWidgetTests, MotionComboAndKeyboardMassReachAttachCallback) {
  settle();
  chooseMotion(PhysicsMotionType::Static);
  chooseMotion(PhysicsMotionType::Kinematic);
  chooseMotion(PhysicsMotionType::Dynamic);
  enterMass("42.5");
  EXPECT_NEAR(options_.mass, 42.5f, 0.001f);
  options_.friction = 0.25f;
  options_.restitution = 0.75f;
  settle();
  clickButton("Attach body");
  ASSERT_TRUE(attachedSettings_);
  EXPECT_EQ(attachCalls_, 1);
  EXPECT_EQ(attachedSettings_->motionType, PhysicsMotionType::Dynamic);
  EXPECT_NEAR(attachedSettings_->mass, 42.5f, 0.001f);
  EXPECT_FLOAT_EQ(attachedSettings_->friction, 0.25f);
  EXPECT_FLOAT_EQ(attachedSettings_->restitution, 0.75f);
  EXPECT_EQ(stepCalls_, 0);
}

TEST_F(PhysicsControlsWidgetTests, DynamicCcdClickAndGravityInputReachBodySettings) {
  settle();
  click(controlPosition(4));
  EXPECT_TRUE(options_.useCCD);
  enterParameter(5, "-2.5");
  EXPECT_NEAR(options_.gravityFactor, -2.5f, .001f);
  clickButton("Attach body");
  ASSERT_TRUE(attachedSettings_);
  EXPECT_TRUE(attachedSettings_->useCCD);
  EXPECT_NEAR(attachedSettings_->gravityFactor, -2.5f, .001f);
  chooseMotion(PhysicsMotionType::Static);
  click(controlPosition(4));
  EXPECT_TRUE(options_.useCCD); // Disabled control retains the dynamic draft.
  clickButton("Attach body");
  EXPECT_FALSE(attachedSettings_->useCCD);
  EXPECT_FLOAT_EQ(attachedSettings_->gravityFactor, 1.0f);
}

TEST_F(PhysicsControlsWidgetTests, SelectionLoadsEachBodyBeforeEditingOneProperty) {
  const PhysicsBodySettings floor{.motionType = PhysicsMotionType::Static,
      .mass = 50, .friction = .8f, .restitution = .2f};
  const PhysicsBodySettings moving{.motionType = PhysicsMotionType::Dynamic,
      .mass = 12.5f, .friction = .25f, .restitution = .7f,
      .gravityFactor = -2.5f, .useCCD = true};
  state_.selectedHasBody = true;
  state_.selectedBodyId = 101;
  state_.selectedBodySettings = floor;
  state_.selectedMotion = floor.motionType;
  settle();
  EXPECT_EQ(options_.motionType, PhysicsMotionType::Static);
  EXPECT_FLOAT_EQ(options_.mass, 50);
  EXPECT_FALSE(options_.useCCD);

  state_.selectedBodyId = 202;
  state_.selectedBodySettings = moving;
  state_.selectedMotion = moving.motionType;
  settle();
  EXPECT_EQ(options_.motionType, PhysicsMotionType::Dynamic);
  EXPECT_FLOAT_EQ(options_.mass, moving.mass);
  EXPECT_FLOAT_EQ(options_.gravityFactor, moving.gravityFactor);
  EXPECT_TRUE(options_.useCCD);
  click(controlPosition(2)); // Change only the selected body's friction slider.
  const float friction = options_.friction;
  EXPECT_NE(friction, moving.friction);
  clickButton("Replace body");
  ASSERT_TRUE(attachedSettings_);
  EXPECT_EQ(attachedSettings_->motionType, moving.motionType);
  EXPECT_FLOAT_EQ(attachedSettings_->mass, moving.mass);
  EXPECT_FLOAT_EQ(attachedSettings_->friction, friction);
  EXPECT_FLOAT_EQ(attachedSettings_->restitution, moving.restitution);
  EXPECT_EQ(attachedSettings_->useCCD, moving.useCCD);
  EXPECT_FLOAT_EQ(attachedSettings_->gravityFactor, moving.gravityFactor);

  state_.selectedBodyId = 101;
  state_.selectedBodySettings = floor;
  state_.selectedMotion = floor.motionType;
  settle();
  click(controlPosition(2));
  const float floorFriction = options_.friction;
  clickButton("Replace body");
  EXPECT_EQ(attachedSettings_->motionType, PhysicsMotionType::Static);
  EXPECT_FLOAT_EQ(attachedSettings_->mass, floor.mass);
  EXPECT_FLOAT_EQ(attachedSettings_->friction, floorFriction);
  EXPECT_FLOAT_EQ(attachedSettings_->restitution, floor.restitution);
  EXPECT_FALSE(attachedSettings_->useCCD);
  EXPECT_FLOAT_EQ(attachedSettings_->gravityFactor, 1.0f);
}

TEST_F(PhysicsControlsWidgetTests, UnchangedBodyIdentityPreservesUnappliedEdits) {
  state_.selectedHasBody = true;
  state_.selectedBodyId = 303;
  state_.selectedBodySettings = PhysicsBodySettings{.mass = 5, .friction = .2f};
  state_.selectedMotion = PhysicsMotionType::Dynamic;
  settle();
  enterMass("19.5");
  click(controlPosition(4));
  enterParameter(5, "0.25");
  for (int index = 0; index < 30; ++index) frame();
  EXPECT_FLOAT_EQ(options_.mass, 19.5f);
  EXPECT_TRUE(options_.useCCD);
  EXPECT_FLOAT_EQ(options_.gravityFactor, .25f);
  clickButton("Replace body");
  ASSERT_TRUE(attachedSettings_);
  EXPECT_FLOAT_EQ(attachedSettings_->mass, 19.5f);
  EXPECT_TRUE(attachedSettings_->useCCD);
  EXPECT_FLOAT_EQ(attachedSettings_->gravityFactor, .25f);
  // A collider replacement gets a new process-unique body identity even if
  // the selected scene node and label remain the same.
  state_.selectedBodyId = 304;
  state_.selectedBodySettings = PhysicsBodySettings{.mass = 9};
  settle();
  EXPECT_FLOAT_EQ(options_.mass, 9);
  EXPECT_FALSE(options_.useCCD);
  EXPECT_FLOAT_EQ(options_.gravityFactor, 1);
}

TEST_F(PhysicsControlsWidgetTests, UnboundSelectionRetainsDraftAndReturningBodyReloads) {
  state_.selectedHasBody = true;
  state_.selectedBodyId = 404;
  state_.selectedBodySettings = PhysicsBodySettings{.mass = 3};
  state_.selectedMotion = PhysicsMotionType::Dynamic;
  settle();
  enterMass("7");
  state_.selectedHasBody = false;
  state_.selectedBodyId = container::physics::kInvalidPhysicsBodyId;
  state_.selectedBodySettings.reset();
  state_.selectedMotion.reset();
  settle();
  EXPECT_FLOAT_EQ(options_.mass, 7);
  EXPECT_EQ(options_.lastEditedBodyId, container::physics::kInvalidPhysicsBodyId);
  clickButton("Attach body");
  ASSERT_TRUE(attachedSettings_);
  EXPECT_FLOAT_EQ(attachedSettings_->mass, 7);
  state_.selectedHasBody = true;
  state_.selectedBodyId = 404;
  state_.selectedBodySettings = PhysicsBodySettings{.mass = 3};
  state_.selectedMotion = PhysicsMotionType::Dynamic;
  settle();
  EXPECT_FLOAT_EQ(options_.mass, 3);
}

TEST_F(PhysicsControlsWidgetTests, ExistingApiValuesAndInitialVelocitiesSurviveRestitutionEdit) {
  PhysicsBodySettings original;
  original.mass = 2'000'000.0f;
  original.friction = 2.0f;
  original.restitution = .25f;
  original.gravityFactor = 20.0f;
  original.useCCD = true;
  original.linearVelocity = {10, 20, 30};
  original.angularVelocity = {1, 2, 3};
  state_.selectedHasBody = true;
  state_.selectedBodyId = 505;
  state_.selectedMotion = original.motionType;
  state_.selectedBodySettings = original;
  settle();
  EXPECT_FLOAT_EQ(options_.mass, original.mass);
  EXPECT_FLOAT_EQ(options_.friction, original.friction);
  EXPECT_FLOAT_EQ(options_.gravityFactor, original.gravityFactor);
  click(controlPosition(3)); // Change restitution using the actual slider.
  const float restitution = options_.restitution;
  EXPECT_NE(restitution, original.restitution);
  clickButton("Replace body");
  ASSERT_TRUE(attachedSettings_);
  EXPECT_FLOAT_EQ(attachedSettings_->mass, original.mass);
  EXPECT_FLOAT_EQ(attachedSettings_->friction, original.friction);
  EXPECT_FLOAT_EQ(attachedSettings_->gravityFactor, original.gravityFactor);
  EXPECT_FLOAT_EQ(attachedSettings_->restitution, restitution);
  EXPECT_EQ(attachedSettings_->useCCD, original.useCCD);
  EXPECT_EQ(attachedSettings_->linearVelocity, original.linearVelocity);
  EXPECT_EQ(attachedSettings_->angularVelocity, original.angularVelocity);
}

TEST_F(PhysicsControlsWidgetTests, ExistingSmallMassIsNotRaisedWhenEditingAnotherProperty) {
  const PhysicsBodySettings original{.mass = 0.0001f, .friction = .75f, .restitution = .25f};
  state_.selectedHasBody = true;
  state_.selectedBodyId = 606;
  state_.selectedMotion = original.motionType;
  state_.selectedBodySettings = original;
  settle();
  EXPECT_FLOAT_EQ(options_.mass, original.mass);
  click(controlPosition(3));
  clickButton("Replace body");
  ASSERT_TRUE(attachedSettings_);
  EXPECT_FLOAT_EQ(attachedSettings_->mass, original.mass);
  EXPECT_FLOAT_EQ(attachedSettings_->friction, original.friction);
}

TEST_F(PhysicsControlsWidgetTests, DisabledBuildSuppressesEveryAction) {
  state_.available = false;
  state_.selectedHasBody = true;
  state_.selectedMotion = PhysicsMotionType::Static;
  settle();
  for (std::string_view label : {"Replace body", "Detach body", "Play", "Single step",
                                 "Reset", "Clear bodies", "Create demo"})
    clickButton(label);
  EXPECT_EQ(totalCalls(), 0);
}

TEST_F(PhysicsControlsWidgetTests, NoSelectionOrBodiesStillAllowsDemoCreation) {
  state_.selectionAvailable = false;
  state_.bodyCount = 0u;
  state_.activeBodyCount = 0u;
  settle();
  for (std::string_view label : {"Attach body", "Detach body", "Play", "Single step",
                                 "Reset", "Clear bodies"})
    clickButton(label);
  EXPECT_EQ(totalCalls(), 0);
  clickButton("Create demo");
  EXPECT_EQ(demoCalls_, 1);
  EXPECT_EQ(totalCalls(), 1);
}

TEST_F(PhysicsControlsWidgetTests, MissingCallbacksDisableOtherwiseAvailableActions) {
  state_.attachSelected = {};
  state_.detachSelected = {};
  state_.setPlaying = {};
  state_.singleStep = {};
  state_.reset = {};
  state_.clear = {};
  state_.createDemo = {};
  state_.selectedHasBody = true;
  state_.selectedMotion = PhysicsMotionType::Dynamic;
  settle();
  for (std::string_view label : {"Replace body", "Detach body", "Play", "Single step",
                                 "Reset", "Clear bodies", "Create demo"})
    clickButton(label);
  EXPECT_EQ(totalCalls(), 0);
}

TEST_F(PhysicsControlsWidgetTests, InvalidParameterValuesUseFiniteClampedDefaults) {
  options_.motionType = static_cast<PhysicsMotionType>(99u);
  options_.mass = std::numeric_limits<float>::quiet_NaN();
  options_.friction = std::numeric_limits<float>::infinity();
  options_.restitution = std::numeric_limits<float>::quiet_NaN();
  options_.gravityFactor = std::numeric_limits<float>::quiet_NaN();
  settle();
  EXPECT_EQ(options_.motionType, PhysicsMotionType::Dynamic);
  EXPECT_FLOAT_EQ(options_.mass, 1.0f);
  EXPECT_FLOAT_EQ(options_.friction, 0.5f);
  EXPECT_FLOAT_EQ(options_.restitution, 0.1f);
  EXPECT_FLOAT_EQ(options_.gravityFactor, 1.0f);
  options_.mass = -10.0f;
  options_.friction = -1.0f;
  options_.restitution = 5.0f;
  options_.gravityFactor = 50.0f;
  settle();
  clickButton("Attach body");
  ASSERT_TRUE(attachedSettings_);
  EXPECT_FLOAT_EQ(attachedSettings_->mass, 0.001f);
  EXPECT_FLOAT_EQ(attachedSettings_->friction, 0.0f);
  EXPECT_FLOAT_EQ(attachedSettings_->restitution, 1.0f);
  EXPECT_FLOAT_EQ(attachedSettings_->gravityFactor, 10.0f);
}

class PhysicsJointControlsWidgetTests : public ::testing::Test {
protected:
  void SetUp() override {
    previous_ = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 1100};
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels{}; int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    state_.available = true;
    state_.selectedHasBody = true;
    state_.selectionLabel = "Second body";
    state_.jointFirstChosen = true;
    state_.jointFirstBodyLabel = "First body";
    state_.canConnectJoint = true;
    state_.suggestedJointAnchor = {1, 2, 3};
    state_.chooseFirstJointBody = [&] { ++firstCalls_; };
    state_.chooseWorldJointBody = [&] { ++worldCalls_; };
    state_.clearFirstJointBody = [&] { ++clearCalls_; };
    state_.connectJoint = [&](const auto &settings) { ++connectCalls_; connected_ = settings; };
    state_.removeJoint = [&](auto id) { removed_.push_back(id); };
    state_.setJointMotor = [&](auto id, const auto &motor) {
      ++motorCalls_; motorJoint_ = id; appliedMotor_ = motor;
    };
  }
  void TearDown() override {
    ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previous_);
  }
  void frame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({960, 1050});
    ImGui::Begin("Joint widget test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    origin_ = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    container::ui::drawPhysicsJointPanel(state_, options_);
    if (!state_.joints.empty())
      lastJointRowCenter_ = context_->LastItemData.Rect.GetCenter();
    ImGui::End();
    ImGui::Render();
  }
  void settle() { frame(); frame(); }
  void click(ImVec2 position) {
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(position.x, position.y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
  }
  float controlsTop() const {
    // Tree header, instruction, first label, choose/clear buttons, second label.
    return origin_.y + 4 * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing();
  }
  ImVec2 control(int row) const {
    return {origin_.x + ImGui::GetStyle().IndentSpacing + 40,
            controlsTop() + row * ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeight() * .5f};
  }
  int motorRow() const {
    return 4 + (!options_.autoJointAnchor ? 1 : 0) + (options_.jointLimitsEnabled ? 2 : 0);
  }
  int connectRow() const {
    int rows = 2 + (!options_.autoJointAnchor ? 1 : 0); // Type and auto anchor.
    if (options_.jointType != container::physics::PhysicsJointType::Fixed) {
      rows += 3 + (options_.jointLimitsEnabled ? 2 : 0); // Axis, limits, motor.
      if (options_.jointMotorMode != container::physics::PhysicsJointMotorMode::Off)
        rows += options_.jointMotorMode == container::physics::PhysicsJointMotorMode::Position ? 4 : 2;
    }
    return rows + 1; // Collision checkbox before Connect.
  }
  void connect() { click(control(connectRow())); }
  void chooseCombo(int row, int value) {
    click(control(row)); settle();
    ImGuiWindow *popup{};
    for (auto *window : context_->Windows)
      if (window->Active && std::string_view{window->Name}.starts_with("##Combo_")) popup = window;
    ASSERT_NE(popup, nullptr);
    click({popup->DC.CursorStartPos.x + 30,
           popup->DC.CursorStartPos.y + value * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetTextLineHeight() * .5f});
    settle();
  }
  void enterNumber(int row, const char *text) {
    click(control(row));
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true); frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
    io.AddInputCharactersUTF8(text); frame();
    io.AddKeyEvent(ImGuiKey_Enter, true); frame();
    io.AddKeyEvent(ImGuiKey_Enter, false); settle();
  }
  void chooseFirst() {
    click({origin_.x + ImGui::GetStyle().IndentSpacing + 50,
           origin_.y + 3 * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * .5f});
  }
  void clearFirst() {
    const float firstWidth = ImGui::CalcTextSize("Use selected as first").x + ImGui::GetStyle().FramePadding.x * 2;
    const float worldWidth = ImGui::CalcTextSize("Use world").x + ImGui::GetStyle().FramePadding.x * 2;
    click({origin_.x + ImGui::GetStyle().IndentSpacing + firstWidth + worldWidth + ImGui::GetStyle().ItemSpacing.x * 2 + 30,
           origin_.y + 3 * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * .5f});
  }
  void chooseWorld() {
    const float firstWidth = ImGui::CalcTextSize("Use selected as first").x + ImGui::GetStyle().FramePadding.x * 2;
    click({origin_.x + ImGui::GetStyle().IndentSpacing + firstWidth + ImGui::GetStyle().ItemSpacing.x + 25,
           origin_.y + 3 * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * .5f});
  }
  void removeRow(size_t row) {
    const float firstY = controlsTop() + (connectRow() + 1) * ImGui::GetFrameHeightWithSpacing() +
                        ImGui::GetTextLineHeightWithSpacing(); // "Joints: N".
    const float smallHeight = ImGui::GetTextLineHeightWithSpacing();
    click({origin_.x + ImGui::GetStyle().IndentSpacing + 25,
           firstY + row * smallHeight + ImGui::GetTextLineHeight() * .5f});
  }
  void applyMotorRow(size_t row) {
    const float removeWidth = ImGui::CalcTextSize("Remove joint").x + ImGui::GetStyle().FramePadding.x * 2;
    click({origin_.x + ImGui::GetStyle().IndentSpacing + removeWidth + ImGui::GetStyle().ItemSpacing.x + 25,
           lastJointRowCenter_.y - (state_.joints.size() - 1 - row) * ImGui::GetTextLineHeightWithSpacing()});
  }
  ImGuiContext *previous_{}, *context_{};
  ImVec2 origin_{};
  ImVec2 lastJointRowCenter_{};
  PhysicsPanelState state_;
  PhysicsPanelOptions options_;
  std::optional<container::physics::PhysicsJointSettings> connected_;
  int firstCalls_{0}, worldCalls_{0}, clearCalls_{0}, connectCalls_{0}, motorCalls_{0};
  container::physics::PhysicsJointId motorJoint_{};
  std::optional<container::physics::PhysicsJointMotorSettings> appliedMotor_;
  std::vector<container::physics::PhysicsJointId> removed_;
};

TEST_F(PhysicsJointControlsWidgetTests, ExplicitFirstAndClearClicksDoNotCreateAJoint) {
  settle();
  for (int frameIndex = 0; frameIndex < 20; ++frameIndex) frame();
  EXPECT_EQ(firstCalls_ + clearCalls_ + connectCalls_, 0);
  chooseFirst(); clearFirst();
  EXPECT_EQ(firstCalls_, 1);
  EXPECT_EQ(clearCalls_, 1);
  EXPECT_EQ(connectCalls_, 0);
}

TEST_F(PhysicsJointControlsWidgetTests, WorldAnchorCanBeChosenBeforeAnyBodyIsSelected) {
  state_.selectedHasBody = false;
  state_.jointFirstChosen = false;
  settle(); chooseFirst(); chooseWorld();
  EXPECT_EQ(firstCalls_, 0);
  EXPECT_EQ(worldCalls_, 1);
  state_.jointFirstChosen = true;
  state_.jointFirstBodyLabel = "World";
  state_.selectedHasBody = true;
  settle(); connect();
  EXPECT_EQ(connectCalls_, 1);
}

TEST_F(PhysicsJointControlsWidgetTests, TypeComboCreatesFixedHingeAndSliderWithAutoAnchor) {
  using container::physics::PhysicsJointType;
  settle();
  for (const auto type : {PhysicsJointType::Fixed, PhysicsJointType::Hinge, PhysicsJointType::Slider}) {
    chooseCombo(0, static_cast<int>(type));
    EXPECT_EQ(options_.jointType, type);
    connect();
    ASSERT_TRUE(connected_);
    EXPECT_EQ(connected_->type, type);
    EXPECT_EQ(connected_->anchor, glm::dvec3(1, 2, 3));
  }
  EXPECT_EQ(connectCalls_, 3);
}

TEST_F(PhysicsJointControlsWidgetTests, HingeLimitsAndMotorConvertDegreesToRadians) {
  using container::physics::PhysicsJointMotorMode;
  options_.autoJointAnchor = false;
  options_.jointAnchor = {4, 5, 6};
  options_.jointAxis = {0, 4, 0};
  options_.jointLimitsEnabled = true;
  options_.hingeLowerDegrees = -45;
  options_.hingeUpperDegrees = 90;
  options_.jointMotorMaxForceOrTorque = 12;
  options_.jointCollideConnected = true;
  settle();
  chooseCombo(motorRow(), static_cast<int>(PhysicsJointMotorMode::Velocity));
  enterNumber(motorRow() + 1, "180");
  EXPECT_NEAR(options_.hingeTargetVelocityDegrees, 180.0, 1e-6);
  connect();
  ASSERT_TRUE(connected_);
  EXPECT_EQ(connectCalls_, 1);
  EXPECT_EQ(connected_->anchor, glm::dvec3(4, 5, 6));
  EXPECT_EQ(connected_->axis, glm::vec3(0, 1, 0));
  EXPECT_NEAR(glm::dot(connected_->axis, connected_->normal), 0, 1e-6f);
  EXPECT_NEAR(glm::length(connected_->normal), 1, 1e-6f);
  EXPECT_TRUE(connected_->limitsEnabled);
  EXPECT_NEAR(connected_->lowerLimit, glm::radians(-45.0f), 1e-6f);
  EXPECT_NEAR(connected_->upperLimit, glm::radians(90.0f), 1e-6f);
  EXPECT_EQ(connected_->motor.mode, PhysicsJointMotorMode::Velocity);
  EXPECT_NEAR(connected_->motor.targetVelocity, glm::radians(180.0f), 1e-6f);
  EXPECT_EQ(connected_->motor.maxForceOrTorque, 12);
  EXPECT_TRUE(connected_->collideConnected);
}

TEST_F(PhysicsJointControlsWidgetTests, SliderPositionMotorKeepsMeterUnits) {
  using container::physics::PhysicsJointType;
  using container::physics::PhysicsJointMotorMode;
  options_.jointType = PhysicsJointType::Slider;
  options_.jointAxis = {2, 0, 0};
  options_.jointLimitsEnabled = true;
  options_.sliderLowerMeters = -.25;
  options_.sliderUpperMeters = 2.5;
  options_.sliderTargetPosition = 1.25;
  options_.jointMotorFrequencyHz = 4;
  options_.jointMotorDampingRatio = .75f;
  settle();
  chooseCombo(motorRow(), static_cast<int>(PhysicsJointMotorMode::Position));
  connect();
  ASSERT_TRUE(connected_);
  EXPECT_EQ(connected_->type, PhysicsJointType::Slider);
  EXPECT_FLOAT_EQ(connected_->lowerLimit, -.25f);
  EXPECT_FLOAT_EQ(connected_->upperLimit, 2.5f);
  EXPECT_EQ(connected_->motor.mode, PhysicsJointMotorMode::Position);
  EXPECT_FLOAT_EQ(connected_->motor.targetPosition, 1.25f);
  EXPECT_FLOAT_EQ(connected_->motor.frequencyHz, 4);
  EXPECT_FLOAT_EQ(connected_->motor.dampingRatio, .75f);
}

TEST_F(PhysicsJointControlsWidgetTests, IncompatiblePairAndMissingCallbacksDisableActions) {
  state_.canConnectJoint = false;
  state_.jointConnectionHint = "Choose a different body";
  state_.selectedHasBody = false;
  state_.jointFirstChosen = false;
  settle(); chooseFirst(); clearFirst(); connect();
  EXPECT_EQ(firstCalls_ + clearCalls_ + connectCalls_, 0);
  state_.canConnectJoint = true;
  state_.selectedHasBody = true;
  state_.jointFirstChosen = true;
  state_.chooseFirstJointBody = {};
  state_.clearFirstJointBody = {};
  state_.connectJoint = {};
  settle(); chooseFirst(); clearFirst(); connect();
  EXPECT_EQ(firstCalls_ + clearCalls_ + connectCalls_, 0);
}

TEST_F(PhysicsJointControlsWidgetTests, InvalidAxisLimitsAndMotorPreventCreation) {
  options_.jointAxis = glm::vec3{0};
  settle(); connect(); EXPECT_EQ(connectCalls_, 0);
  options_.jointAxis = {0, 1, 0};
  options_.jointLimitsEnabled = true;
  options_.hingeLowerDegrees = 90;
  options_.hingeUpperDegrees = -90;
  settle(); connect(); EXPECT_EQ(connectCalls_, 0);
  options_.hingeLowerDegrees = -90;
  options_.hingeUpperDegrees = 90;
  options_.jointMotorMode = container::physics::PhysicsJointMotorMode::Velocity;
  options_.jointMotorMaxForceOrTorque = std::numeric_limits<float>::quiet_NaN();
  settle(); connect(); EXPECT_EQ(connectCalls_, 0);
  options_.jointMotorMaxForceOrTorque = 10;
  settle(); connect(); EXPECT_EQ(connectCalls_, 1);
}

TEST_F(PhysicsJointControlsWidgetTests, LimitsExcludingZeroCannotCreateAJoint) {
  options_.jointLimitsEnabled = true;
  options_.hingeLowerDegrees = 10;
  options_.hingeUpperDegrees = 30;
  settle(); connect(); EXPECT_EQ(connectCalls_, 0);
  options_.jointType = container::physics::PhysicsJointType::Slider;
  options_.sliderLowerMeters = -3;
  options_.sliderUpperMeters = -1;
  settle(); connect(); EXPECT_EQ(connectCalls_, 0);
  options_.sliderUpperMeters = 0;
  settle(); connect(); EXPECT_EQ(connectCalls_, 1);
  ASSERT_TRUE(connected_);
  EXPECT_FLOAT_EQ(connected_->upperLimit, 0);
  options_.sliderLowerMeters = 0;
  settle(); connect(); EXPECT_EQ(connectCalls_, 1); // Use Fixed for locked motion.
  options_.jointType = container::physics::PhysicsJointType::Hinge;
  options_.hingeLowerDegrees = options_.hingeUpperDegrees = 0;
  settle(); connect(); EXPECT_EQ(connectCalls_, 1);
}

TEST_F(PhysicsJointControlsWidgetTests, JointRemovalPreservesFull64BitIdentity) {
  options_.jointType = container::physics::PhysicsJointType::Fixed;
  state_.joints = {{.id = 1, .label = "Low identity"},
                   {.id = (uint64_t{1} << 32) + 1, .label = "High identity"}};
  settle(); removeRow(1);
  ASSERT_EQ(removed_.size(), 1);
  EXPECT_EQ(removed_.front(), (uint64_t{1} << 32) + 1);
}

TEST_F(PhysicsJointControlsWidgetTests, ApplyMotorUpdatesExistingHingeOnceWithRadians) {
  const uint64_t joint = (uint64_t{1} << 32) + 7;
  state_.joints = {{.id = joint, .label = "Hinge", .type = container::physics::PhysicsJointType::Hinge}};
  options_.jointMotorMode = container::physics::PhysicsJointMotorMode::Velocity;
  options_.hingeTargetVelocityDegrees = 90;
  options_.jointMotorMaxForceOrTorque = 42;
  settle(); applyMotorRow(0);
  ASSERT_TRUE(appliedMotor_);
  EXPECT_EQ(motorCalls_, 1);
  EXPECT_EQ(motorJoint_, joint);
  EXPECT_NEAR(appliedMotor_->targetVelocity, glm::radians(90.0f), 1e-6f);
  EXPECT_FLOAT_EQ(appliedMotor_->maxForceOrTorque, 42);
  EXPECT_EQ(connectCalls_, 0);
  for (int frameIndex = 0; frameIndex < 20; ++frameIndex) frame();
  EXPECT_EQ(motorCalls_, 1);
  state_.joints[0].type = container::physics::PhysicsJointType::Slider;
  settle(); applyMotorRow(0);
  EXPECT_EQ(motorCalls_, 1);
}

TEST_F(PhysicsJointControlsWidgetTests, StopMotorIgnoresInvalidJointCreationDrafts) {
  using container::physics::PhysicsJointMotorMode;
  state_.joints = {{.id = 7, .label = "Hinge", .type = container::physics::PhysicsJointType::Hinge}};
  options_.jointMotorMode = PhysicsJointMotorMode::Off;
  for (int invalidDraft = 0; invalidDraft < 3; ++invalidDraft) {
    SCOPED_TRACE(invalidDraft);
    options_.autoJointAnchor = invalidDraft != 0;
    options_.jointAnchor = {std::numeric_limits<double>::quiet_NaN(), 0, 0};
    options_.jointAxis = invalidDraft == 1 ? glm::vec3{0} : glm::vec3{0, 1, 0};
    options_.jointLimitsEnabled = invalidDraft == 2;
    options_.hingeLowerDegrees = 30;
    options_.hingeUpperDegrees = -30;
    settle();
    connect();
    EXPECT_EQ(connectCalls_, 0);
    applyMotorRow(0);
    ASSERT_TRUE(appliedMotor_);
    EXPECT_EQ(appliedMotor_->mode, PhysicsJointMotorMode::Off);
    EXPECT_EQ(motorCalls_, invalidDraft + 1);
  }
}

TEST_F(PhysicsJointControlsWidgetTests, VelocityMotorIgnoresHiddenPositionAndSpringDrafts) {
  using container::physics::PhysicsJointMotorMode;
  state_.joints = {{.id = 7, .label = "Hinge", .type = container::physics::PhysicsJointType::Hinge}};
  options_.jointMotorMode = PhysicsJointMotorMode::Velocity;
  options_.hingeTargetVelocityDegrees = 90;
  options_.hingeTargetPositionDegrees = std::numeric_limits<double>::quiet_NaN();
  options_.jointMotorFrequencyHz = 0;
  options_.jointMotorDampingRatio = std::numeric_limits<float>::quiet_NaN();
  settle();
  connect();
  ASSERT_TRUE(connected_);
  EXPECT_EQ(connectCalls_, 1);
  EXPECT_EQ(connected_->motor.mode, PhysicsJointMotorMode::Velocity);
  EXPECT_NEAR(connected_->motor.targetVelocity, glm::radians(90.f), 1e-6f);
  EXPECT_TRUE(std::isfinite(connected_->motor.targetPosition));
  EXPECT_TRUE(std::isfinite(connected_->motor.frequencyHz));
  EXPECT_TRUE(std::isfinite(connected_->motor.dampingRatio));
  settle();
  applyMotorRow(0);
  ASSERT_TRUE(appliedMotor_);
  EXPECT_EQ(motorCalls_, 1);
  EXPECT_NEAR(appliedMotor_->targetVelocity, glm::radians(90.f), 1e-6f);
  EXPECT_TRUE(std::isnan(options_.hingeTargetPositionDegrees));
  EXPECT_FLOAT_EQ(options_.jointMotorFrequencyHz, 0);
  EXPECT_TRUE(std::isnan(options_.jointMotorDampingRatio));
}

TEST_F(PhysicsJointControlsWidgetTests, PositionMotorIgnoresHiddenVelocityDraft) {
  using container::physics::PhysicsJointType;
  using container::physics::PhysicsJointMotorMode;
  options_.jointType = PhysicsJointType::Slider;
  state_.joints = {{.id = 7, .label = "Slider", .type = PhysicsJointType::Slider}};
  options_.jointMotorMode = PhysicsJointMotorMode::Position;
  options_.sliderTargetPosition = 1.25;
  options_.sliderTargetVelocity = std::numeric_limits<double>::infinity();
  options_.jointMotorFrequencyHz = 3;
  options_.jointMotorDampingRatio = .75f;
  settle();
  connect();
  ASSERT_TRUE(connected_);
  EXPECT_EQ(connectCalls_, 1);
  EXPECT_FLOAT_EQ(connected_->motor.targetPosition, 1.25f);
  EXPECT_FLOAT_EQ(connected_->motor.targetVelocity, 0);
  settle();
  applyMotorRow(0);
  ASSERT_TRUE(appliedMotor_);
  EXPECT_EQ(motorCalls_, 1);
  EXPECT_FLOAT_EQ(appliedMotor_->targetPosition, 1.25f);
  EXPECT_FLOAT_EQ(appliedMotor_->frequencyHz, 3);
  EXPECT_FLOAT_EQ(appliedMotor_->dampingRatio, .75f);
  EXPECT_TRUE(std::isinf(options_.sliderTargetVelocity));
}

TEST_F(PhysicsJointControlsWidgetTests, InvalidActiveMotorFieldsDisableCreationAndApply) {
  using container::physics::PhysicsJointMotorMode;
  state_.joints = {{.id = 7, .label = "Hinge", .type = container::physics::PhysicsJointType::Hinge}};
  for (int invalidField = 0; invalidField < 5; ++invalidField) {
    SCOPED_TRACE(invalidField);
    options_ = {};
    options_.jointMotorMode = invalidField < 2 ? PhysicsJointMotorMode::Velocity
                                             : PhysicsJointMotorMode::Position;
    if (invalidField == 0) options_.hingeTargetVelocityDegrees = std::numeric_limits<double>::infinity();
    if (invalidField == 1) options_.jointMotorMaxForceOrTorque = std::numeric_limits<float>::quiet_NaN();
    if (invalidField == 2) options_.hingeTargetPositionDegrees = std::numeric_limits<double>::quiet_NaN();
    if (invalidField == 3) options_.jointMotorFrequencyHz = 0;
    if (invalidField == 4) options_.jointMotorDampingRatio = -1;
    settle();
    connect();
    applyMotorRow(0);
    EXPECT_EQ(connectCalls_, 0);
    EXPECT_EQ(motorCalls_, 0);
  }
}

TEST_F(PhysicsJointControlsWidgetTests, DisabledBuildSuppressesJointCreationAndRemoval) {
  state_.available = false;
  options_.jointType = container::physics::PhysicsJointType::Fixed;
  state_.joints = {{.id = 7, .label = "A joint"}};
  settle(); chooseFirst(); chooseWorld(); clearFirst(); connect(); removeRow(0);
  EXPECT_EQ(firstCalls_ + worldCalls_ + clearCalls_ + connectCalls_, 0);
  EXPECT_TRUE(removed_.empty());
}

} // namespace
