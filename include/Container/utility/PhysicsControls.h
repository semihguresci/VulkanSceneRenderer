#pragma once

#include "Container/physics/PhysicsWorld.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace container::ui {

struct PhysicsJointPanelRow {
  physics::PhysicsJointId id{physics::kInvalidPhysicsJointId};
  std::string label{};
  physics::PhysicsJointType type{physics::PhysicsJointType::Fixed};
  double coordinate{0.0};
};

struct PhysicsPanelState {
  bool available{false};
  bool playing{false};
  bool selectionAvailable{false};
  bool selectedHasBody{false};
  std::optional<physics::PhysicsMotionType> selectedMotion{};
  physics::PhysicsBodyId selectedBodyId{physics::kInvalidPhysicsBodyId};
  std::optional<physics::PhysicsBodySettings> selectedBodySettings{};
  size_t bodyCount{0};
  size_t activeBodyCount{0};
  uint64_t totalSteps{0};
  std::string error{};
  std::string selectionLabel{};
  std::function<void(const physics::PhysicsBodySettings &)> attachSelected{};
  std::function<void()> detachSelected{};
  std::function<void(bool)> setPlaying{};
  std::function<void()> singleStep{};
  std::function<void()> reset{};
  std::function<void()> clear{};
  std::function<void()> createDemo{};
  bool jointFirstChosen{false};
  std::string jointFirstBodyLabel{};
  bool canConnectJoint{false};
  std::string jointConnectionHint{};
  glm::dvec3 suggestedJointAnchor{0.0};
  std::vector<PhysicsJointPanelRow> joints{};
  std::function<void()> chooseFirstJointBody{};
  std::function<void()> chooseWorldJointBody{};
  std::function<void()> clearFirstJointBody{};
  std::function<void(const physics::PhysicsJointSettings &)> connectJoint{};
  std::function<void(physics::PhysicsJointId)> removeJoint{};
  std::function<void(physics::PhysicsJointId, const physics::PhysicsJointMotorSettings &)> setJointMotor{};
};

struct PhysicsPanelOptions {
  physics::PhysicsBodyId lastEditedBodyId{physics::kInvalidPhysicsBodyId};
  physics::PhysicsMotionType motionType{physics::PhysicsMotionType::Dynamic};
  float mass{1.0f};
  float friction{0.5f};
  float restitution{0.1f};
  bool useCCD{false};
  float gravityFactor{1.0f};
  physics::PhysicsJointType jointType{physics::PhysicsJointType::Hinge};
  bool autoJointAnchor{true};
  glm::dvec3 jointAnchor{0.0};
  glm::vec3 jointAxis{0.0f, 1.0f, 0.0f};
  bool jointLimitsEnabled{false};
  double hingeLowerDegrees{-90.0}, hingeUpperDegrees{90.0};
  double sliderLowerMeters{-1.0}, sliderUpperMeters{1.0};
  physics::PhysicsJointMotorMode jointMotorMode{physics::PhysicsJointMotorMode::Off};
  double hingeTargetVelocityDegrees{0.0}, hingeTargetPositionDegrees{0.0};
  double sliderTargetVelocity{0.0}, sliderTargetPosition{0.0};
  float jointMotorMaxForceOrTorque{1000.0f};
  float jointMotorFrequencyHz{2.0f};
  float jointMotorDampingRatio{1.0f};
  bool jointCollideConnected{false};
};

// Called inside the existing scene-controls window. State is a snapshot;
// callbacks run only for explicit user actions and options persist in the UI.
void drawPhysicsPanel(const PhysicsPanelState &state, PhysicsPanelOptions &options);
// Kept separate so focused widget tests can exercise real joint controls.
void drawPhysicsJointPanel(const PhysicsPanelState &state, PhysicsPanelOptions &options);

} // namespace container::ui
