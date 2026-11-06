#pragma once

#include "Container/physics/PhysicsWorld.h"

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace container::scene {
class SceneGraph;
}

namespace container::physics {

struct PhysicsSceneStatistics {
  size_t bodyCount{0};
  size_t activeBodyCount{0};
  size_t jointCount{0};
  uint32_t substeps{0};
  uint64_t totalSteps{0};
  uint64_t transformPublications{0};
  uint64_t authoredPoseUpdates{0};
  double accumulatedSeconds{0.0};
  double droppedSeconds{0.0};
};

struct PhysicsSceneJointState {
  PhysicsJointId id{kInvalidPhysicsJointId};
  // A missing first node anchors the second body to the fixed world.
  std::optional<uint32_t> nodeA{};
  uint32_t nodeB{0};
  PhysicsJointState state{};
};

// Fixed-step CPU physics bindings use graph publication identities and stable
// scene-node indices, independently of recreated renderer entity handles.
class PhysicsScene {
public:
  static constexpr double kFixedStepSeconds = 1.0 / 60.0;
  static constexpr uint32_t kMaxSubsteps = 8u;
  static constexpr double kMaxFrameSeconds = 0.25;

  explicit PhysicsScene(const PhysicsWorldSettings &settings = {});
  ~PhysicsScene();
  PhysicsScene(const PhysicsScene &) = delete;
  PhysicsScene &operator=(const PhysicsScene &) = delete;
  PhysicsScene(PhysicsScene &&) noexcept;
  PhysicsScene &operator=(PhysicsScene &&) noexcept;

  // Invalid bounds/transforms and mixed dynamic/non-dynamic ancestor bindings
  // throw invalid_argument. Box center/scale come from the authored node;
  // settings.position/rotation are therefore replaced by the scene pose.
  [[nodiscard]] PhysicsBodyId attachBox(scene::SceneGraph &graph, uint32_t node,
                                       glm::vec3 localMin, glm::vec3 localMax,
                                       PhysicsBodySettings settings = {});
  // Replace an existing body's settings/shape at its current visual pose.
  // Failure retains the original body; Reset retains its authored local pose.
  // A playing kinematic-to-kinematic replacement retains its physical node
  // frame so pending authored motion still sweeps on the next advancing tick.
  [[nodiscard]] PhysicsBodyId replaceBox(scene::SceneGraph &graph, uint32_t node,
                                         glm::vec3 localMin, glm::vec3 localMax,
                                         PhysicsBodySettings settings = {});
  // Validate the remaining dynamic publication and prospective restoration
  // before changing graph transforms or retiring the endpoint and its joints.
  [[nodiscard]] bool detach(scene::SceneGraph &graph, uint32_t node,
                             bool restore = true);
  // A matching graph restores authored locals before retiring its bindings.
  // A replaced graph is never modified through obsolete node indices.
  void clear(scene::SceneGraph *graph = nullptr);
  // While paused, explicit visual transform edits teleport bound dynamics
  // without advancing time or discarding their existing velocities.
  // Authored matrices, poses and derived collider extents are checked across
  // all bindings before synchronization starts changing bodies.
  // Advancing kinematic scale changes rebuild at the current physical node
  // frame; paused scale changes use the authored pose immediately.
  void update(scene::SceneGraph &graph, double frameDeltaSeconds);
  void singleStep(scene::SceneGraph &graph);
  // Validate a prospective local edit in the affected physics subtree before
  // changing graph transforms or playback. Visual-only branches and bindings
  // from a different graph publication impose no transform restrictions.
  // Checks authored/reset transforms and scale-dependent joint frames; backend
  // allocation and shape creation retain their separate failure behavior.
  // An explicit parent validates the exact local under that prospective parent;
  // nullopt retains the current parent, and SceneGraph::kInvalidNode means root.
  void validateTransform(const scene::SceneGraph &graph, uint32_t node,
                         const glm::mat4 &local,
                         std::optional<uint32_t> parentOverride = std::nullopt) const;
  // Validate a world-preserving hierarchy edit before the editor changes the
  // graph. Rejection leaves topology, body/joint state and playback untouched.
  void validateReparent(const scene::SceneGraph &graph, uint32_t node,
                        std::optional<uint32_t> parent) const;
  // Restore authored locals and initial velocities, then pause. Edited unbound
  // parents retain their current transforms when resolving reset world poses.
  // A rejected collider/joint rebuild leaves the live simulation unchanged.
  void reset(scene::SceneGraph &graph);
  void setPlaying(bool playing) noexcept;
  [[nodiscard]] bool playing() const noexcept;
  [[nodiscard]] size_t bodyCount() const noexcept;
  [[nodiscard]] uint64_t graphIdentity() const noexcept;
  [[nodiscard]] bool hasBody(uint32_t node) const noexcept;
  // Inspect attached binding ancestry rather than walking visual descendants.
  // Obsolete graph publications and missing nodes have no matching bodies.
  [[nodiscard]] bool hasDynamicBodyInSubtree(const scene::SceneGraph &graph,
                                            uint32_t node) const;
  [[nodiscard]] std::optional<PhysicsMotionType> motionType(uint32_t node) const noexcept;
  [[nodiscard]] std::optional<PhysicsBodyId> bodyId(uint32_t node) const noexcept;
  [[nodiscard]] std::optional<PhysicsBodySettings> bodySettings(uint32_t node) const noexcept;
  [[nodiscard]] std::optional<PhysicsBodyPose> bodyPose(uint32_t node) const;
  // Body-local joint frames survive reset and collider rebuilds. A detached
  // endpoint or replacement graph retires its joints; IDs are never reused.
  // Invalid settings/motion pairs are rejected before pending edits are applied.
  // Playing kinematics retain their scheduled motion; new joint frames use the
  // current physics poses. Paused creation synchronizes authored poses at once.
  [[nodiscard]] PhysicsJointId createJoint(
      scene::SceneGraph &graph, std::optional<uint32_t> nodeA, uint32_t nodeB,
      PhysicsJointSettings settings);
  [[nodiscard]] bool destroyJoint(PhysicsJointId id) noexcept;
  [[nodiscard]] bool setJointMotor(PhysicsJointId id, PhysicsJointMotorSettings motor);
  [[nodiscard]] bool setJointLimits(PhysicsJointId id, bool enabled, float lower, float upper);
  [[nodiscard]] std::optional<PhysicsSceneJointState> jointState(PhysicsJointId id) const;
  [[nodiscard]] std::vector<PhysicsSceneJointState> joints() const;
  [[nodiscard]] size_t jointCount() const noexcept;
  [[nodiscard]] PhysicsSceneStatistics statistics() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace container::physics
