#pragma once

#include "Container/physics/PhysicsWorld.h"

#include <limits>
#include <optional>

namespace container::robotics {

struct ArticulatedMotorSettings {
  float maxForceOrTorque{1000.0f};
  float frequencyHz{2.0f};
  float dampingRatio{1.0f};
  // Applies to velocity commands. The position motor uses a force-limited spring.
  float maxVelocity{std::numeric_limits<float>::max()};
};

// Hinge targets use radians and sliders use scene units. The world enforces
// the authored joint limits. Missing and fixed joints return false; invalid
// motor settings or targets throw std::invalid_argument.
[[nodiscard]] bool commandArticulatedJointPosition(
    physics::PhysicsWorld& world, physics::PhysicsJointId joint, float target,
    ArticulatedMotorSettings settings = {});
[[nodiscard]] bool commandArticulatedJointVelocity(
    physics::PhysicsWorld& world, physics::PhysicsJointId joint, float target,
    ArticulatedMotorSettings settings = {});

struct GripperFingerTargets {
  float left{};
  float right{};
};

struct ParallelGripperSettings {
  physics::PhysicsBodyId palm{};
  physics::PhysicsJointId leftFingerJoint{};
  physics::PhysicsJointId rightFingerJoint{};
  GripperFingerTargets open{};
  GripperFingerTargets closed{};
  ArticulatedMotorSettings motor{};
};

// The caller owns the contact-enabled world, palm, and two live slider joints
// connecting the palm to distinct dynamic fingers. The gripper only owns the
// grasp constraint. Its world must outlive it.
class ParallelGripper {
public:
  ParallelGripper(physics::PhysicsWorld& world, ParallelGripperSettings settings);
  ~ParallelGripper();
  ParallelGripper(const ParallelGripper&) = delete;
  ParallelGripper& operator=(const ParallelGripper&) = delete;

  // These command both fingers together and return false if either slider was
  // retired or its current limits cannot reach the requested target.
  [[nodiscard]] bool open();
  [[nodiscard]] bool close();

  // Call after stepping with the close command still active on both fingers.
  // A new grasp requires a dynamic payload with active inward contacts on
  // both fingers. It creates a fixed joint at the current relative pose with
  // collision enabled. An existing grasp returns true only for its payload.
  [[nodiscard]] bool tryGrasp(physics::PhysicsBodyId payload);

  // Returns true if a live grasp was detached. The payload pose and velocity
  // and both finger commands stay in effect. Use open() to detach and command
  // both fingers open. Externally retired grasp joints report as empty.
  [[nodiscard]] bool release() noexcept;
  [[nodiscard]] std::optional<physics::PhysicsBodyId> heldPayload() const noexcept;
  [[nodiscard]] std::optional<physics::PhysicsJointId> graspJoint() const noexcept;

private:
  [[nodiscard]] bool setFingerTargets(GripperFingerTargets targets);

  physics::PhysicsWorld& world_;
  ParallelGripperSettings settings_;
  physics::PhysicsBodyId leftFinger_{};
  physics::PhysicsBodyId rightFinger_{};
  glm::vec3 palmSliderAxis_{};
  bool closing_{}; // Latches the last successful close/open command.
  physics::PhysicsJointId grasp_{};
  physics::PhysicsBodyId payload_{};
};

} // namespace container::robotics
