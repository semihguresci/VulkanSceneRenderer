#include "Container/robotics/Articulation.h"

#include <gtest/gtest.h>
#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace container::physics;
using namespace container::robotics;

constexpr float stepSeconds = 1.0f / 120;
constexpr double sourceTop = .50;
constexpr double targetTop = .65;
constexpr double targetX = .80;
constexpr double cubeHalfExtent = .075;
constexpr double penetrationTolerance = .001; // Solver contact tolerance, metres.
constexpr double poseReplayTolerance = 1e-4; // Same build, independent worlds, one worker.
constexpr float closedFingerPosition = .061f;
enum Role : size_t { Source, Destination, Payload, Carriage, Palm, LeftPad, RightPad, RoleCount };
enum Joint : size_t { Travel, Lift, LeftFinger, RightFinger, JointCount };
constexpr std::array<const char*, RoleCount> roleNames{
    "source", "destination", "payload", "carriage", "palm", "left-pad", "right-pad"};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

uint64_t pairBit(size_t first, size_t second) {
  if (first > second) std::swap(first, second);
  return uint64_t{1} << (first * RoleCount + second);
}

struct ContactRow {
  size_t first, second;
  glm::vec3 normal;
  float depth;
};

struct Snapshot {
  uint32_t tick{};
  std::string phase;
  std::array<PhysicsBodyPose, RoleCount> poses;
  std::array<float, JointCount> coordinates;
  std::vector<ContactRow> contacts;
  uint64_t began{}, persisted{}, ended{};
};

struct Trace {
  std::vector<Snapshot> samples;
  uint64_t began{}, persisted{}, ended{};
  double maximumPenetration{}, settledPenetration{}, maximumLimitError{};
  double sourceClearance{}, destinationClearance{}, releaseClearance{};
  glm::vec3 releaseLinearVelocity{}, releaseAngularVelocity{};
};

class ArmFixture {
public:
  PhysicsWorld world{{.gravity = {0, 0, -9.81f}, .maxBodies = 32, .workerThreads = 1,
                      .trackContacts = true}};
  std::array<PhysicsBodyId, RoleCount> bodies{};
  std::array<PhysicsJointId, JointCount> joints{};
  ArticulatedMotorSettings stageMotor{.maxForceOrTorque = 1000, .frequencyHz = 8,
                                       .dampingRatio = 1, .maxVelocity = .25f};
  ArticulatedMotorSettings fingerMotor{.maxForceOrTorque = 30, .frequencyHz = 4,
                                        .dampingRatio = 1, .maxVelocity = .10f};
  std::unique_ptr<ParallelGripper> gripper;
  Trace trace;

  ArmFixture() {
    bodies[Source] = world.createBox({.25f, .28f, .10f},
        {.motionType = PhysicsMotionType::Static, .position = {0, 0, sourceTop - .1}});
    bodies[Destination] = world.createBox({.25f, .28f, .10f},
        {.motionType = PhysicsMotionType::Static, .position = {targetX, 0, targetTop - .1}});
    bodies[Payload] = box({.075f, .075f, .075f}, {0, 0, sourceTop + cubeHalfExtent}, .1f);
    bodies[Carriage] = box({.08f, .08f, .035f}, {0, 0, 1.10}, 1);
    bodies[Palm] = box({.10f, .18f, .035f}, {0, 0, .78}, .5f);
    bodies[LeftPad] = box({.09f, .025f, .065f}, {0, .16, .59}, .15f);
    bodies[RightPad] = box({.09f, .025f, .065f}, {0, -.16, .59}, .15f);
    joints[Travel] = slider(0, bodies[Carriage], {1, 0, 0}, {0, 1, 0}, .8f);
    joints[Lift] = slider(bodies[Carriage], bodies[Palm], {0, 0, 1}, {1, 0, 0}, .5f);
    joints[LeftFinger] = slider(bodies[Palm], bodies[LeftPad], {0, -1, 0}, {1, 0, 0}, .07f);
    joints[RightFinger] = slider(bodies[Palm], bodies[RightPad], {0, 1, 0}, {1, 0, 0}, .07f);
    position(Travel, 0);
    position(Lift, 0);
    gripper = std::make_unique<ParallelGripper>(world, ParallelGripperSettings{
        .palm = bodies[Palm], .leftFingerJoint = joints[LeftFinger],
        .rightFingerJoint = joints[RightFinger], .open = {},
        .closed = {closedFingerPosition, closedFingerPosition}, .motor = fingerMotor});
    require(gripper->open(), "Could not open the primitive articulated gripper");
    trace.samples.reserve(2000);
    record("initial");
  }

  PhysicsBodyId box(glm::vec3 half, glm::dvec3 position, float mass) {
    return world.createBox(half, {.position = position, .mass = mass, .friction = .8f, .useCCD = true});
  }

  PhysicsJointId slider(PhysicsBodyId parent, PhysicsBodyId child, glm::vec3 axis,
                         glm::vec3 normal, float upper) {
    return world.createJoint(parent, child, {.type = PhysicsJointType::Slider,
        .anchor = world.pose(child)->position, .axis = axis, .normal = normal,
        .limitsEnabled = true, .lowerLimit = 0, .upperLimit = upper});
  }

  void position(Joint joint, float target) {
    require(commandArticulatedJointPosition(world, joints[joint], target, stageMotor),
            "Could not command articulated stage position");
  }

  PhysicsBodyPose pose(Role role) const { return world.pose(bodies[role]).value(); }

  size_t role(PhysicsBodyId body) const {
    const auto found = std::ranges::find(bodies, body);
    require(found != bodies.end(), "Contact referenced a body outside the fixture");
    return static_cast<size_t>(found - bodies.begin());
  }

  bool touching(Role first, Role second) const {
    return std::ranges::any_of(world.contacts(), [&](const auto& contact) {
      return (contact.bodyA == bodies[first] && contact.bodyB == bodies[second]) ||
             (contact.bodyA == bodies[second] && contact.bodyB == bodies[first]);
    });
  }

  std::optional<glm::vec3> padNormal(Role pad) const {
    for (const auto& contact : world.contacts()) {
      if (contact.bodyA == bodies[pad] && contact.bodyB == bodies[Payload]) return contact.normalFromAToB;
      if (contact.bodyB == bodies[pad] && contact.bodyA == bodies[Payload]) return -contact.normalFromAToB;
    }
    return std::nullopt;
  }

  bool bilateral() const {
    const auto left = padNormal(LeftPad), right = padNormal(RightPad);
    if (!left || !right || glm::dot(*left, *right) > -.8f) return false;
    const auto toward = [&](Role pad) {
      return glm::normalize(glm::vec3(pose(Payload).position - pose(pad).position));
    };
    return glm::dot(*left, toward(LeftPad)) > .8f && glm::dot(*right, toward(RightPad)) > .8f;
  }

  glm::dvec3 cubeExtent() const {
    const auto rotation = glm::dquat(pose(Payload).rotation);
    glm::dvec3 extent(0);
    for (const auto axis : {glm::dvec3(1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, 0, 1)})
      extent += glm::abs(rotation * axis) * cubeHalfExtent;
    return extent;
  }

  double cubeBottom() const { return pose(Payload).position.z - cubeExtent().z; }

  void record(const char* phase) {
    Snapshot row{.tick = static_cast<uint32_t>(trace.samples.size()), .phase = phase};
    for (size_t index = 0; index < RoleCount; ++index)
      row.poses[index] = world.pose(bodies[index]).value();
    for (size_t index = 0; index < JointCount; ++index) {
      const auto joint = world.jointState(joints[index]).value();
      row.coordinates[index] = joint.coordinate;
      trace.maximumLimitError = std::max({trace.maximumLimitError,
          double(joint.settings.lowerLimit - joint.coordinate), double(joint.coordinate - joint.settings.upperLimit)});
    }
    for (const auto& contact : world.contacts()) {
      auto first = role(contact.bodyA), second = role(contact.bodyB);
      if (first != Payload && second != Payload && first != LeftPad && second != LeftPad &&
          first != RightPad && second != RightPad) continue;
      auto normal = contact.normalFromAToB;
      if (first > second) { std::swap(first, second); normal = -normal; }
      require(std::isfinite(contact.penetrationDepth) &&
              std::isfinite(normal.x) && std::isfinite(normal.y) && std::isfinite(normal.z),
              "Contact report contained a nonfinite normal or penetration");
      row.contacts.push_back({first, second, normal, contact.penetrationDepth});
      trace.maximumPenetration = std::max(trace.maximumPenetration, double(contact.penetrationDepth));
      if (std::string_view(phase) == "released-settle")
        trace.settledPenetration = std::max(trace.settledPenetration, double(contact.penetrationDepth));
    }
    std::ranges::sort(row.contacts, [](const auto& first, const auto& second) {
      return std::pair(first.first, first.second) < std::pair(second.first, second.second);
    });
    for (const auto& event : world.contactEvents()) {
      const auto mask = pairBit(role(event.contact.bodyA), role(event.contact.bodyB));
      switch (event.type) {
      case PhysicsContactEventType::Began: row.began |= mask; break;
      case PhysicsContactEventType::Persisted: row.persisted |= mask; break;
      case PhysicsContactEventType::Ended: row.ended |= mask; break;
      }
    }
    trace.began |= row.began;
    trace.persisted |= row.persisted;
    trace.ended |= row.ended;
    trace.samples.push_back(std::move(row));
  }

  void steps(uint32_t count, const char* phase) {
    for (uint32_t index = 0; index < count; ++index) { world.step(stepSeconds); record(phase); }
  }
};

void unchanged(const PhysicsBodyPose& before, const PhysicsBodyPose& after) {
  EXPECT_EQ(after.position, before.position);
  EXPECT_EQ(after.rotation, before.rotation);
}

Trace pickAndPlace() {
  ArmFixture arm;
  arm.steps(120, "source-rest");
  require(arm.trace.began & pairBit(Source, Payload),
          "Cube did not establish real source support contact before resting");
  EXPECT_NEAR(arm.cubeBottom(), sourceTop, penetrationTolerance);
  EXPECT_FALSE(arm.gripper->tryGrasp(arm.bodies[Payload]));
  require(arm.gripper->close(), "Could not close gripper");
  arm.steps(240, "close");
  require(arm.bilateral(), "Closing failed to establish two inward opposing pad contacts");
  const auto beforeGrasp = arm.pose(Payload);
  const auto graspVelocity = arm.world.linearVelocity(arm.bodies[Payload]).value();
  require(arm.gripper->tryGrasp(arm.bodies[Payload]), "Bilateral contact grasp was rejected");
  unchanged(beforeGrasp, arm.pose(Payload));
  EXPECT_EQ(arm.world.linearVelocity(arm.bodies[Payload]).value(), graspVelocity);
  EXPECT_EQ(arm.gripper->heldPayload(), arm.bodies[Payload]);
  const auto grasp = arm.world.jointState(*arm.gripper->graspJoint()).value();
  EXPECT_EQ(grasp.type, PhysicsJointType::Fixed);
  EXPECT_TRUE(grasp.settings.collideConnected);
  EXPECT_EQ(arm.world.jointCount(), JointCount + 1);

  arm.position(Lift, .35f);
  arm.steps(240, "lift-clear");
  EXPECT_EQ(arm.world.jointState(arm.joints[Lift])->settings.motor.mode, PhysicsJointMotorMode::Position);
  EXPECT_NEAR(arm.world.jointState(arm.joints[Lift])->coordinate, .35, .025);
  arm.trace.sourceClearance = arm.cubeBottom() - sourceTop;
  require(arm.trace.sourceClearance > .20, "Transformed cube corners never cleared the source support");
  EXPECT_FALSE(arm.touching(Source, Payload));
  EXPECT_TRUE(arm.gripper->heldPayload());

  require(commandArticulatedJointVelocity(arm.world, arm.joints[Travel], .40f, arm.stageMotor),
          "Could not command horizontal velocity motor");
  const auto velocityMotor = arm.world.jointState(arm.joints[Travel])->settings.motor;
  EXPECT_EQ(velocityMotor.mode, PhysicsJointMotorMode::Velocity);
  EXPECT_FLOAT_EQ(velocityMotor.targetVelocity, .25f);
  arm.steps(600, "transport-to-limit");
  EXPECT_NEAR(arm.world.jointState(arm.joints[Travel])->coordinate, targetX, .005);
  EXPECT_NEAR(arm.pose(Payload).position.x, targetX, .01);
  arm.trace.destinationClearance = arm.cubeBottom() - targetTop;
  require(arm.trace.destinationClearance > .10, "Cube geometry did not pass clear above the destination");
  EXPECT_FALSE(arm.touching(Destination, Payload));
  EXPECT_TRUE(arm.gripper->heldPayload());

  arm.position(Travel, 2); // Position target is clamped to the actual hard stop.
  EXPECT_FLOAT_EQ(arm.world.jointState(arm.joints[Travel])->settings.motor.targetPosition, .8f);
  arm.steps(120, "hold-target");
  arm.position(Lift, .20f);
  arm.steps(240, "lower-above-target");
  arm.trace.releaseClearance = arm.cubeBottom() - targetTop;
  require(arm.trace.releaseClearance > .015 && arm.trace.releaseClearance < .08,
          "Release must start above the destination, with a real fall still required");
  EXPECT_FALSE(arm.touching(Destination, Payload));
  const auto beforeRelease = arm.pose(Payload);
  arm.trace.releaseLinearVelocity = arm.world.linearVelocity(arm.bodies[Payload]).value();
  arm.trace.releaseAngularVelocity = arm.world.angularVelocity(arm.bodies[Payload]).value();
  require(arm.gripper->release(), "Held payload was not released");
  unchanged(beforeRelease, arm.pose(Payload));
  EXPECT_EQ(arm.world.linearVelocity(arm.bodies[Payload]).value(), arm.trace.releaseLinearVelocity);
  EXPECT_EQ(arm.world.angularVelocity(arm.bodies[Payload]).value(), arm.trace.releaseAngularVelocity);
  EXPECT_FALSE(arm.gripper->release());
  EXPECT_FALSE(arm.gripper->heldPayload());
  EXPECT_FALSE(arm.gripper->graspJoint());
  EXPECT_EQ(arm.world.jointCount(), JointCount);
  require(arm.gripper->open(), "Could not open released gripper");
  arm.steps(360, "released-settle");

  require(arm.trace.began & pairBit(Destination, Payload),
          "Released cube did not establish real destination support contact before resting");
  EXPECT_FALSE(arm.touching(Source, Payload));
  EXPECT_FALSE(arm.touching(LeftPad, Payload));
  EXPECT_FALSE(arm.touching(RightPad, Payload));
  EXPECT_NEAR(arm.pose(Payload).position.x, targetX, .01);
  EXPECT_NEAR(arm.pose(Payload).position.y, 0, .01);
  EXPECT_NEAR(arm.pose(Payload).position.z, targetTop + cubeHalfExtent, penetrationTolerance);
  EXPECT_NEAR(arm.cubeBottom(), targetTop, penetrationTolerance);
  const auto extent = arm.cubeExtent();
  EXPECT_LE(std::abs(arm.pose(Payload).position.x - targetX) + extent.x, .25);
  EXPECT_LE(std::abs(arm.pose(Payload).position.y) + extent.y, .28);
  EXPECT_LT(glm::length(arm.world.linearVelocity(arm.bodies[Payload]).value()), .01f);
  EXPECT_LT(glm::length(arm.world.angularVelocity(arm.bodies[Payload]).value()), .02f);
  auto minimumZ = arm.pose(Payload).position.z, maximumZ = minimumZ;
  for (auto it = arm.trace.samples.end() - 120; it != arm.trace.samples.end(); ++it) {
    minimumZ = std::min(minimumZ, it->poses[Payload].position.z);
    maximumZ = std::max(maximumZ, it->poses[Payload].position.z);
  }
  EXPECT_LT(maximumZ - minimumZ, .001);
  EXPECT_LE(arm.trace.maximumPenetration, penetrationTolerance);
  EXPECT_LE(arm.trace.settledPenetration, penetrationTolerance);
  EXPECT_LE(arm.trace.maximumLimitError, .005);
  for (const auto pad : {LeftPad, RightPad}) {
    EXPECT_NE(arm.trace.began & pairBit(pad, Payload), 0u);
    EXPECT_NE(arm.trace.persisted & pairBit(pad, Payload), 0u);
    EXPECT_NE(arm.trace.ended & pairBit(pad, Payload), 0u);
  }
  EXPECT_NE(arm.trace.ended & pairBit(Source, Payload), 0u);
  EXPECT_NE(arm.trace.began & pairBit(Destination, Payload), 0u);
  return std::move(arm.trace);
}

TEST(ArticulatedGrasp, MotorDrivenBilateralPickTransportReleaseAndSettleReplayInIndependentWorlds) {
  Trace first, second;
  ASSERT_NO_THROW(first = pickAndPlace());
  ASSERT_NO_THROW(second = pickAndPlace());
  ASSERT_EQ(first.samples.size(), second.samples.size());
  double maximumReplayPositionError = 0;
  float maximumReplayQuaternionError = 0;
  for (size_t tick = 0; tick < first.samples.size(); ++tick) {
    SCOPED_TRACE("tick=" + std::to_string(tick) + " phase=" + first.samples[tick].phase);
    const auto& expected = first.samples[tick];
    const auto& actual = second.samples[tick];
    EXPECT_EQ(actual.tick, expected.tick);
    EXPECT_EQ(actual.phase, expected.phase);
    EXPECT_EQ(actual.began, expected.began);
    EXPECT_EQ(actual.persisted, expected.persisted);
    EXPECT_EQ(actual.ended, expected.ended);
    for (size_t body = 0; body < RoleCount; ++body) {
      SCOPED_TRACE(roleNames[body]);
      const auto error = glm::length(actual.poses[body].position - expected.poses[body].position);
      maximumReplayPositionError = std::max(maximumReplayPositionError, error);
      EXPECT_LE(error, poseReplayTolerance);
      const auto rotationError = std::min(glm::length(actual.poses[body].rotation - expected.poses[body].rotation),
                                          glm::length(actual.poses[body].rotation + expected.poses[body].rotation));
      maximumReplayQuaternionError = std::max(maximumReplayQuaternionError, rotationError);
      EXPECT_LE(rotationError, 5e-5f); // About 1e-4 rad, without acos rounding at identity.
    }
    for (size_t joint = 0; joint < JointCount; ++joint)
      EXPECT_NEAR(actual.coordinates[joint], expected.coordinates[joint], poseReplayTolerance);
    ASSERT_EQ(actual.contacts.size(), expected.contacts.size());
    for (size_t pair = 0; pair < expected.contacts.size(); ++pair) {
      const auto& left = expected.contacts[pair];
      const auto& right = actual.contacts[pair];
      EXPECT_EQ(right.first, left.first);
      EXPECT_EQ(right.second, left.second);
      EXPECT_NEAR(right.depth, left.depth, poseReplayTolerance);
      EXPECT_LE(glm::length(right.normal - left.normal), .001f);
    }
  }
  EXPECT_EQ(second.began, first.began);
  EXPECT_EQ(second.persisted, first.persisted);
  EXPECT_EQ(second.ended, first.ended);
  RecordProperty("step_seconds", std::to_string(stepSeconds));
  RecordProperty("steps_per_run", std::to_string(first.samples.size() - 1));
  RecordProperty("contact_penetration_limit_m", std::to_string(penetrationTolerance));
  RecordProperty("maximum_contact_penetration_m", std::to_string(std::max(first.maximumPenetration, second.maximumPenetration)));
  RecordProperty("maximum_release_settle_penetration_m", std::to_string(std::max(first.settledPenetration, second.settledPenetration)));
  RecordProperty("source_cube_clearance_m", std::to_string(first.sourceClearance));
  RecordProperty("destination_cube_clearance_m", std::to_string(first.destinationClearance));
  RecordProperty("release_cube_clearance_m", std::to_string(first.releaseClearance));
  RecordProperty("maximum_replay_position_error_m", std::to_string(maximumReplayPositionError));
  RecordProperty("maximum_replay_quaternion_norm_error", std::to_string(maximumReplayQuaternionError));
  RecordProperty("release_linear_speed_m_s", std::to_string(glm::length(first.releaseLinearVelocity)));
  RecordProperty("release_angular_speed_rad_s", std::to_string(glm::length(first.releaseAngularVelocity)));
}

TEST(ArticulatedGrasp, RejectsSinglePadContactWhileClosing) {
  ArmFixture arm;
  auto offset = arm.pose(Payload);
  offset.position.y += .04;
  ASSERT_TRUE(arm.world.setPose(arm.bodies[Payload], offset));
  arm.steps(120, "negative-source-rest");
  ASSERT_TRUE(arm.gripper->close());
  for (uint32_t tick = 0; tick < 120 && !arm.touching(LeftPad, Payload); ++tick)
    arm.steps(1, "single-pad");
  ASSERT_TRUE(arm.touching(LeftPad, Payload));
  ASSERT_FALSE(arm.touching(RightPad, Payload));
  EXPECT_FALSE(arm.gripper->tryGrasp(arm.bodies[Payload]));
  EXPECT_FALSE(arm.gripper->heldPayload());
}

TEST(ArticulatedGrasp, RejectsRelocatedContactEvidenceAndSurvivesWorldClear) {
  ArmFixture arm;
  arm.steps(120, "negative-source-rest");
  ASSERT_TRUE(arm.gripper->close());
  arm.steps(240, "negative-bilateral");
  ASSERT_TRUE(arm.bilateral());
  ASSERT_TRUE(commandArticulatedJointPosition(arm.world, arm.joints[RightFinger], 0, arm.fingerMotor));
  EXPECT_FALSE(arm.gripper->tryGrasp(arm.bodies[Payload])); // The close command was superseded.
  ASSERT_TRUE(arm.gripper->close());
  const auto original = arm.pose(Payload);
  auto relocated = original;
  relocated.position.z += .5;
  ASSERT_TRUE(arm.world.setPose(arm.bodies[Payload], relocated, false));
  EXPECT_FALSE(arm.gripper->tryGrasp(arm.bodies[Payload])); // No restep may refresh stale evidence.
  EXPECT_FALSE(arm.gripper->heldPayload());

  ASSERT_TRUE(arm.world.setPose(arm.bodies[Payload], original));
  arm.steps(240, "restore-real-contact");
  ASSERT_TRUE(arm.bilateral());
  ASSERT_TRUE(arm.gripper->tryGrasp(arm.bodies[Payload]));
  arm.world.clear(); // Logical teardown while the borrowed world object still lives.
  EXPECT_EQ(arm.world.bodyCount(), 0u);
  EXPECT_EQ(arm.world.jointCount(), 0u);
  EXPECT_TRUE(arm.world.contacts().empty());
  EXPECT_FALSE(arm.gripper->heldPayload());
  EXPECT_FALSE(arm.gripper->graspJoint());
  EXPECT_FALSE(arm.gripper->release());
  EXPECT_FALSE(arm.gripper->release());
  EXPECT_FALSE(arm.gripper->open());
  EXPECT_FALSE(arm.gripper->close());
  EXPECT_NO_THROW(arm.gripper.reset());
}

TEST(ArticulatedMotor, HingeClampsPositionAndVelocityCommandsAndObeysBothStops) {
  PhysicsWorld world({.gravity = {0, 0, 0}, .maxBodies = 8, .workerThreads = 1});
  const auto link = world.createBox({.1f, .05f, .05f});
  const auto hinge = world.createJoint(0, link, {
      .type = PhysicsJointType::Hinge, .axis = {0, 0, 1}, .normal = {1, 0, 0},
      .limitsEnabled = true, .lowerLimit = -.15f, .upperLimit = .23f,
  });
  const ArticulatedMotorSettings motor{
      .maxForceOrTorque = 10, .frequencyHz = 4, .dampingRatio = 1, .maxVelocity = .20f,
  };
  ASSERT_TRUE(commandArticulatedJointPosition(world, hinge, .8f, motor));
  EXPECT_FLOAT_EQ(world.jointState(hinge)->settings.motor.targetPosition, .23f);
  for (int tick = 0; tick < 120; ++tick)
    world.step(stepSeconds);
  EXPECT_NEAR(world.jointState(hinge)->coordinate, .23, .002);

  ASSERT_TRUE(commandArticulatedJointVelocity(world, hinge, -.7f, motor));
  EXPECT_FLOAT_EQ(world.jointState(hinge)->settings.motor.targetVelocity, -.20f);
  for (int tick = 0; tick < 360; ++tick)
    world.step(stepSeconds);
  EXPECT_NEAR(world.jointState(hinge)->coordinate, -.15, .002);
  EXPECT_FALSE(commandArticulatedJointPosition(world, kInvalidPhysicsJointId, 0, motor));
}

} // namespace
