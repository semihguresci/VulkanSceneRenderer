#include "Container/robotics/Articulation.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace container::robotics {
namespace {

using namespace physics;

PhysicsJointMotorSettings prepareMotor(const PhysicsJointState& joint,
                                       PhysicsJointMotorMode mode, float target,
                                       const ArticulatedMotorSettings& settings) {
  if (!std::isfinite(settings.maxVelocity) || settings.maxVelocity < 0.0f ||
      !std::isfinite(target))
    throw std::invalid_argument(
        "Articulated motor target and velocity limit must be finite, with a nonnegative limit");

  auto configuration = joint.settings;
  auto& motor = configuration.motor;
  motor.mode = mode;
  motor.maxForceOrTorque = settings.maxForceOrTorque;
  motor.frequencyHz = settings.frequencyHz;
  motor.dampingRatio = settings.dampingRatio;
  if (mode == PhysicsJointMotorMode::Position)
    motor.targetPosition = target;
  else
    motor.targetVelocity = std::clamp(target, -settings.maxVelocity, settings.maxVelocity);
  return PhysicsWorld::validateJointSettings(configuration).motor;
}

bool commandMotor(PhysicsWorld& world, PhysicsJointId id, PhysicsJointMotorMode mode,
                  float target, const ArticulatedMotorSettings& settings) {
  const auto joint = world.jointState(id);
  if (!joint || joint->type == PhysicsJointType::Fixed)
    return false;
  return world.setJointMotor(id, prepareMotor(*joint, mode, target, settings));
}

struct FingerBinding {
  PhysicsBodyId body;
  glm::vec3 axisInPalm;
};

FingerBinding bindFinger(const PhysicsWorld& world, PhysicsBodyId palm,
                         const PhysicsJointState& joint) {
  if (joint.type != PhysicsJointType::Slider ||
      (joint.bodyA != palm && joint.bodyB != palm))
    throw std::invalid_argument("Gripper finger joint must be a slider connected to the palm");
  const bool palmIsFirst = joint.bodyA == palm;
  const auto finger = palmIsFirst ? joint.bodyB : joint.bodyA;
  if (world.motionType(finger) != PhysicsMotionType::Dynamic)
    throw std::invalid_argument("Gripper finger body must be dynamic");
  return {finger, palmIsFirst ? joint.frameA.axis : joint.frameB.axis};
}

bool withinLimits(const PhysicsJointState& joint, float target) {
  return !joint.settings.limitsEnabled ||
         (target >= joint.settings.lowerLimit && target <= joint.settings.upperLimit);
}

bool hasPositionTarget(const PhysicsWorld& world, PhysicsJointId id, float target) {
  const auto joint = world.jointState(id);
  return joint && joint->settings.motor.mode == PhysicsJointMotorMode::Position &&
         joint->settings.motor.targetPosition == target;
}

std::optional<glm::vec3> normalFromFinger(std::span<const PhysicsContact> contacts,
                                         PhysicsBodyId finger, PhysicsBodyId payload) {
  const std::pair<PhysicsBodyId, PhysicsBodyId> key = std::minmax(finger, payload);
  const auto contact = std::ranges::lower_bound(contacts, key, {}, [](const PhysicsContact& pair) {
    return std::pair{pair.bodyA, pair.bodyB};
  });
  if (contact == contacts.end() || std::pair{contact->bodyA, contact->bodyB} != key)
    return std::nullopt;
  return contact->bodyA == finger ? contact->normalFromAToB : -contact->normalFromAToB;
}

} // namespace

bool commandArticulatedJointPosition(PhysicsWorld& world, PhysicsJointId joint, float target,
                                    ArticulatedMotorSettings settings) {
  return commandMotor(world, joint, PhysicsJointMotorMode::Position, target, settings);
}

bool commandArticulatedJointVelocity(PhysicsWorld& world, PhysicsJointId joint, float target,
                                    ArticulatedMotorSettings settings) {
  return commandMotor(world, joint, PhysicsJointMotorMode::Velocity, target, settings);
}

ParallelGripper::ParallelGripper(PhysicsWorld& world, ParallelGripperSettings settings)
    : world_(world), settings_(settings) {
  if (!world_.tracksContacts())
    throw std::invalid_argument("Parallel gripper requires a physics world with trackContacts enabled");
  const auto left = world_.jointState(settings_.leftFingerJoint);
  const auto right = world_.jointState(settings_.rightFingerJoint);
  if (!world_.contains(settings_.palm) || !left || !right)
    throw std::invalid_argument("Gripper palm and finger joints must belong to the live physics world");

  const auto leftBinding = bindFinger(world_, settings_.palm, *left);
  const auto rightBinding = bindFinger(world_, settings_.palm, *right);
  if (leftBinding.body == rightBinding.body)
    throw std::invalid_argument("Gripper fingers must be distinct bodies");
  if (std::abs(glm::dot(leftBinding.axisInPalm, rightBinding.axisInPalm)) < 0.9999f)
    throw std::invalid_argument("Gripper finger slider axes must be parallel");
  if (settings_.open.left == settings_.closed.left &&
      settings_.open.right == settings_.closed.right)
    throw std::invalid_argument("Gripper open and closed targets must differ");

  for (const auto targets : {settings_.open, settings_.closed}) {
    (void)prepareMotor(*left, PhysicsJointMotorMode::Position, targets.left, settings_.motor);
    (void)prepareMotor(*right, PhysicsJointMotorMode::Position, targets.right, settings_.motor);
    if (!withinLimits(*left, targets.left) || !withinLimits(*right, targets.right))
      throw std::invalid_argument("Gripper finger targets must fit their slider limits");
  }
  leftFinger_ = leftBinding.body;
  rightFinger_ = rightBinding.body;
  palmSliderAxis_ = leftBinding.axisInPalm;
}

ParallelGripper::~ParallelGripper() { (void)release(); }

bool ParallelGripper::setFingerTargets(GripperFingerTargets targets) {
  const auto left = world_.jointState(settings_.leftFingerJoint);
  const auto right = world_.jointState(settings_.rightFingerJoint);
  if (!left || !right || !withinLimits(*left, targets.left) ||
      !withinLimits(*right, targets.right))
    return false;

  // Validate both before changing either. World operations run on this thread
  // and neither call can retire a handle or step the simulation.
  const auto leftMotor =
      prepareMotor(*left, PhysicsJointMotorMode::Position, targets.left, settings_.motor);
  const auto rightMotor =
      prepareMotor(*right, PhysicsJointMotorMode::Position, targets.right, settings_.motor);
  if (!world_.setJointMotor(settings_.leftFingerJoint, leftMotor))
    return false;
  if (world_.setJointMotor(settings_.rightFingerJoint, rightMotor))
    return true;
  (void)world_.setJointMotor(settings_.leftFingerJoint, left->settings.motor);
  return false;
}

bool ParallelGripper::open() {
  if (!setFingerTargets(settings_.open))
    return false;
  closing_ = false;
  (void)release();
  return true;
}

bool ParallelGripper::close() {
  if (!setFingerTargets(settings_.closed))
    return false;
  closing_ = true;
  return true;
}

bool ParallelGripper::tryGrasp(PhysicsBodyId payload) {
  if (const auto held = heldPayload())
    return *held == payload;
  if (!closing_ ||
      !hasPositionTarget(world_, settings_.leftFingerJoint, settings_.closed.left) ||
      !hasPositionTarget(world_, settings_.rightFingerJoint, settings_.closed.right) ||
      payload == settings_.palm || payload == leftFinger_ || payload == rightFinger_ ||
      world_.motionType(payload) != PhysicsMotionType::Dynamic)
    return false;

  const auto contacts = world_.contacts();
  const auto leftNormal = normalFromFinger(contacts, leftFinger_, payload);
  const auto rightNormal = normalFromFinger(contacts, rightFinger_, payload);
  if (!leftNormal || !rightNormal)
    return false;
  const auto palm = world_.pose(settings_.palm);
  const auto left = world_.pose(leftFinger_);
  const auto right = world_.pose(rightFinger_);
  const auto object = world_.pose(payload);
  if (!palm || !left || !right || !object)
    return false;

  glm::vec3 spanAxis = palm->rotation * palmSliderAxis_;
  const double separation = glm::dot(glm::dvec3(spanAxis), right->position - left->position);
  if (separation == 0.0)
    return false;
  if (separation < 0.0)
    spanAxis = -spanAxis;
  constexpr float minimumInwardAlignment = 0.5f;
  if (!(glm::dot(*leftNormal, spanAxis) >= minimumInwardAlignment &&
        glm::dot(*rightNormal, spanAxis) <= -minimumInwardAlignment &&
        glm::dot(*leftNormal, *rightNormal) <= -minimumInwardAlignment))
    return false;

  // The world-space frame is converted to both endpoints' current local
  // frames, preserving their relative position and orientation at attachment.
  grasp_ = world_.createJoint(settings_.palm, payload, {
      .type = PhysicsJointType::Fixed,
      .anchor = object->position,
      .collideConnected = true,
  });
  payload_ = payload;
  return true;
}

bool ParallelGripper::release() noexcept {
  const bool detached = world_.destroyJoint(grasp_);
  grasp_ = kInvalidPhysicsJointId;
  payload_ = kInvalidPhysicsBodyId;
  return detached;
}

std::optional<PhysicsBodyId> ParallelGripper::heldPayload() const noexcept {
  return graspJoint() ? std::optional{payload_} : std::nullopt;
}

std::optional<PhysicsJointId> ParallelGripper::graspJoint() const noexcept {
  return world_.containsJoint(grasp_) ? std::optional{grasp_} : std::nullopt;
}

} // namespace container::robotics
