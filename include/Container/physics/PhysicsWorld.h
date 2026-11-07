#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace container::physics {

enum class PhysicsMotionType : uint8_t { Static, Kinematic, Dynamic };

using PhysicsBodyId = uint64_t;
inline constexpr PhysicsBodyId kInvalidPhysicsBodyId = 0u;

struct PhysicsBodyPose {
  glm::dvec3 position{0.0};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct PhysicsBodySettings {
  PhysicsMotionType motionType{PhysicsMotionType::Dynamic};
  glm::dvec3 position{0.0};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 linearVelocity{0.0f};
  glm::vec3 angularVelocity{0.0f};
  float mass{1.0f};
  float friction{0.5f};
  float restitution{0.0f};
  float gravityFactor{1.0f};
  bool useCCD{false};
};

struct PhysicsWorldSettings {
  glm::vec3 gravity{0.0f, -9.81f, 0.0f};
  uint32_t maxBodies{16'384u};
  // Zero chooses at most four workers, leaving small systems on the caller.
  uint32_t workerThreads{0u};
};

using PhysicsJointId = uint64_t;
inline constexpr PhysicsJointId kInvalidPhysicsJointId = 0u;
enum class PhysicsJointType : uint8_t { Fixed, Hinge, Slider };
enum class PhysicsJointMotorMode : uint8_t { Off, Velocity, Position };

struct PhysicsJointMotorSettings {
  PhysicsJointMotorMode mode{PhysicsJointMotorMode::Off};
  float targetVelocity{0.0f}; // rad/s for hinges; scene units/s for sliders.
  // Radians for hinges; unlimited Position targets normalize to [-pi, pi].
  // Slider targets use scene units.
  float targetPosition{0.0f};
  float maxForceOrTorque{1000.0f};
  float frequencyHz{2.0f};
  float dampingRatio{1.0f};
};

// Anchors are relative to the body's center of mass for canonical local
// frames. The fixed world has identity pose, so its local frame is world space.
struct PhysicsJointFrame {
  glm::dvec3 anchor{0.0};
  glm::vec3 axis{0.0f, 1.0f, 0.0f};
  glm::vec3 normal{1.0f, 0.0f, 0.0f};
};

struct PhysicsJointSettings {
  PhysicsJointType type{PhysicsJointType::Hinge};
  glm::dvec3 anchor{0.0};
  glm::vec3 axis{0.0f, 1.0f, 0.0f};
  glm::vec3 normal{1.0f, 0.0f, 0.0f};
  bool limitsEnabled{false};
  float lowerLimit{-1.0f};
  float upperLimit{1.0f};
  PhysicsJointMotorSettings motor{};
  bool collideConnected{false};
};

struct PhysicsJointState {
  PhysicsJointType type{PhysicsJointType::Hinge};
  PhysicsBodyId bodyA{kInvalidPhysicsBodyId};
  PhysicsBodyId bodyB{kInvalidPhysicsBodyId};
  float coordinate{0.0f}; // Hinge angle / slider displacement from zero pose.
  PhysicsJointSettings settings{};
  PhysicsJointFrame frameA{};
  PhysicsJointFrame frameB{};
};

// CPU rigid-body world. Scene bindings and fixed-step accumulation live above
// this layer. All methods are called by the owning application thread; internal
// collision jobs finish before step() returns. No graphics API is required.
class PhysicsWorld {
public:
  explicit PhysicsWorld(const PhysicsWorldSettings &settings = {});
  ~PhysicsWorld();
  PhysicsWorld(const PhysicsWorld &) = delete;
  PhysicsWorld &operator=(const PhysicsWorld &) = delete;
  PhysicsWorld(PhysicsWorld &&) = delete;
  PhysicsWorld &operator=(PhysicsWorld &&) = delete;

  // Pure preflight helpers for scene transactions. No world or Jolt runtime
  // is required; returned quaternions and joint axes are normalized.
  [[nodiscard]] static PhysicsBodyPose validatePose(PhysicsBodyPose pose);
  [[nodiscard]] static PhysicsJointSettings
  validateJointSettings(PhysicsJointSettings settings);

  [[nodiscard]] PhysicsBodyId
  createBox(glm::vec3 halfExtents, const PhysicsBodySettings &settings = {});
  [[nodiscard]] PhysicsBodyId
  createSphere(float radius, const PhysicsBodySettings &settings = {});
  [[nodiscard]] PhysicsBodyId
  createCapsule(float halfHeight, float radius,
                const PhysicsBodySettings &settings = {});
  [[nodiscard]] PhysicsBodyId
  createStaticMesh(std::span<const glm::vec3> vertices,
                   std::span<const uint32_t> indices,
                   const PhysicsBodySettings &settings = {
                       .motionType = PhysicsMotionType::Static});

  [[nodiscard]] bool destroyBody(PhysicsBodyId id) noexcept;
  [[nodiscard]] bool contains(PhysicsBodyId id) const noexcept;
  [[nodiscard]] std::optional<PhysicsBodyPose> pose(PhysicsBodyId id) const;
  [[nodiscard]] std::optional<glm::vec3> linearVelocity(PhysicsBodyId id) const;
  [[nodiscard]] std::optional<glm::vec3> angularVelocity(PhysicsBodyId id) const;
  [[nodiscard]] bool setPose(PhysicsBodyId id, const PhysicsBodyPose &pose,
                             bool activate = true);
  [[nodiscard]] bool setLinearVelocity(PhysicsBodyId id, glm::vec3 velocity);
  [[nodiscard]] bool setAngularVelocity(PhysicsBodyId id, glm::vec3 velocity);
  [[nodiscard]] bool moveKinematic(PhysicsBodyId id,
                                   const PhysicsBodyPose &target,
                                   float deltaSeconds);

  // Body A may be invalid/zero to attach body B to the fixed world. Body B is
  // required and at least one endpoint must be dynamic. Axis/normal must be
  // nonzero perpendicular vectors; input lengths are normalized. Enabled
  // limits straddle zero with a nonzero interval, and hinge angles are
  // restricted to [-pi, pi].
  [[nodiscard]] PhysicsJointId createJoint(PhysicsBodyId bodyA,
                                            PhysicsBodyId bodyB,
                                            PhysicsJointSettings settings);
  // Recreates an authored zero frame after bodies are rebuilt. Settings'
  // world anchor/axis/normal are ignored; all other settings are retained.
  [[nodiscard]] PhysicsJointId createJointLocal(
      PhysicsBodyId bodyA, PhysicsBodyId bodyB, PhysicsJointSettings settings,
      PhysicsJointFrame frameA, PhysicsJointFrame frameB);
  [[nodiscard]] bool destroyJoint(PhysicsJointId id) noexcept;
  [[nodiscard]] bool containsJoint(PhysicsJointId id) const noexcept;
  [[nodiscard]] std::optional<PhysicsJointState> jointState(PhysicsJointId id) const;
  [[nodiscard]] bool setJointMotor(PhysicsJointId id,
                                   PhysicsJointMotorSettings motor);
  [[nodiscard]] bool setJointLimits(PhysicsJointId id, bool enabled,
                                    float lower, float upper);
  [[nodiscard]] size_t jointCount() const noexcept;

  // Executes exactly one simulation update. The caller supplies a fixed,
  // positive finite duration, typically 1/60 second.
  void step(float deltaSeconds);
  void clear() noexcept;
  [[nodiscard]] size_t bodyCount() const noexcept;
  [[nodiscard]] size_t activeBodyCount() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace container::physics
