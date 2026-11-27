#pragma once

#include "Container/physics/PhysicsWorld.h"
#include "Container/robotics/UrdfRuntime.h"

namespace container::robotics {

struct UrdfColliderBinding {
  size_t link{};
  size_t collision{};
  physics::PhysicsBodyId body{};
};

// Places exact URDF primitive collision shapes in a caller-owned Jolt world.
// The world and runtime must outlive this binding. The owner synchronizes
// after updating joint state and before running queries or physics.
class UrdfCollisionBindings {
public:
  UrdfCollisionBindings(physics::PhysicsWorld& world, const UrdfRuntime& robot);
  ~UrdfCollisionBindings();
  UrdfCollisionBindings(const UrdfCollisionBindings&) = delete;
  UrdfCollisionBindings& operator=(const UrdfCollisionBindings&) = delete;

  [[nodiscard]] std::span<const UrdfColliderBinding> bodies() const noexcept { return bodies_; }
  void synchronize();

private:
  physics::PhysicsWorld& world_;
  const UrdfRuntime& robot_;
  std::vector<UrdfColliderBinding> bodies_;
};

} // namespace container::robotics
