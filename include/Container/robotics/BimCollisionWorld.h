#pragma once

#include "Container/physics/PhysicsWorld.h"
#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/BimWalkability.h"

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace container::robotics {

class DifferentialDriveSimulation;

// Owns a Z-up, metre-based Jolt world built from a separate collider snapshot.
// Query callbacks borrow this world and must not outlive it.
class BimCollisionWorld {
public:
  explicit BimCollisionWorld(CookedBimCollision geometry,
                             std::span<const std::string> openDoorIds = {},
                             uint32_t workerThreads = 0);

  [[nodiscard]] const CookedBimCollision& geometry() const { return geometry_; }
  [[nodiscard]] bool isDoorOpen(std::string_view entityId) const;
  // Reject a close that would put a door collider inside an attached robot.
  void setDoorOpen(std::string_view entityId, bool open);
  [[nodiscard]] size_t activeColliderCount() const { return bodyRows_.size(); }
  [[nodiscard]] size_t cachedStaticMeshCount() const { return meshShapes_.size(); }
  [[nodiscard]] BimWalkabilityQueries walkabilityQueries() const;

private:
  friend class DifferentialDriveSimulation;

  struct Door {
    std::vector<size_t> rows;
    bool open{false};
    bool initiallyOpen{false};
  };

  void install(size_t row);
  void uninstall(size_t row) noexcept;
  void resetAuthoredBodies();
  [[nodiscard]] BimWalkabilityQueries walkabilityQueries(physics::PhysicsBodyId ignored) const;

  CookedBimCollision geometry_;
  physics::PhysicsWorld world_;
  std::unordered_map<const BimCollisionMesh*, physics::PhysicsStaticMesh> meshShapes_;
  std::vector<physics::PhysicsBodyId> rowBodies_;
  std::unordered_map<physics::PhysicsBodyId, size_t> bodyRows_;
  std::map<std::string, Door, std::less<>> doors_;
  physics::PhysicsBodyId robotBody_{physics::kInvalidPhysicsBodyId};
  float robotHalfHeight_{};
  float robotRadius_{};
  std::string robotEntityId_;
};

}  // namespace container::robotics
