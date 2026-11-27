#pragma once

#include "Container/physics/PhysicsWorld.h"
#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/BimWalkability.h"
#include "Container/robotics/LidarQuery.h"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace container::robotics {

class DifferentialDriveSimulation;
class DifferentialDriveFleetSimulation;

// Owns a Z-up, metre-based Jolt world built from a separate collider snapshot.
// Query callbacks borrow this world and must not outlive it.
class BimCollisionWorld {
public:
  static constexpr uint32_t maxRobotCapacity = 64;

  explicit BimCollisionWorld(CookedBimCollision geometry,
                             std::span<const std::string> openDoorIds = {},
                             uint32_t workerThreads = 0,
                             uint32_t robotCapacity = 1);

  [[nodiscard]] const CookedBimCollision& geometry() const { return geometry_; }
  [[nodiscard]] bool isDoorOpen(std::string_view entityId) const;
  // Reject a close that would put a door collider inside any attached robot.
  void setDoorOpen(std::string_view entityId, bool open);
  [[nodiscard]] size_t activeColliderCount() const { return bodyRows_.size(); }
  [[nodiscard]] size_t cachedStaticMeshCount() const { return meshShapes_.size(); }
  [[nodiscard]] uint32_t robotCapacity() const noexcept { return robotCapacity_; }
  [[nodiscard]] size_t robotCount() const noexcept { return robots_.size(); }
  // Returns the simulated capsule's center and orientation in building space.
  [[nodiscard]] std::optional<physics::PhysicsBodyPose> robotPose(std::string_view robotId) const;
  [[nodiscard]] BimWalkabilityQueries walkabilityQueries() const;
  // Reacquire after changing the attachment of the ignored robot.
  [[nodiscard]] BimWalkabilityQueries walkabilityQueries(std::string_view ignoredRobotId) const;
  // Excludes the sole attached robot for the single-drive compatibility API.
  // Fleet callers can identify the scanning robot explicitly.
  [[nodiscard]] LidarCast lidarRaycasts() const;
  [[nodiscard]] LidarCast lidarRaycasts(std::string_view ignoredRobotId) const;

private:
  friend class DifferentialDriveSimulation;
  friend class DifferentialDriveFleetSimulation;

  struct Door {
    std::vector<size_t> rows;
    bool open{false};
    bool initiallyOpen{false};
  };
  struct RobotBody {
    std::string entityId;
    float halfHeight{};
    float radius{};
  };
  struct ColliderIdentity {
    std::string_view entityId;
    bool walkableSupport{};
  };

  void install(size_t row);
  void uninstall(size_t row) noexcept;
  void acquireDriveOwner();
  // Releases all attached robot bodies, including any from a partial setup.
  void releaseDriveOwner() noexcept;
  [[nodiscard]] physics::PhysicsBodyId attachRobot(std::string robotId, float halfHeight, float radius,
                                                   physics::PhysicsBodyPose pose);
  void detachRobot(physics::PhysicsBodyId robot) noexcept;
  void resetAuthoredBodies();
  [[nodiscard]] physics::PhysicsBodyId robotBody(std::string_view robotId) const noexcept;
  [[nodiscard]] ColliderIdentity colliderIdentity(physics::PhysicsBodyId body) const;
  [[nodiscard]] std::optional<LidarHit> castLidar(glm::dvec3 origin, glm::dvec3 displacement,
                                                 physics::PhysicsBodyId ignored) const;
  [[nodiscard]] BimWalkabilityQueries walkabilityQueries(physics::PhysicsBodyId ignored) const;
  // Fleet arbitration queries authored geometry separately from moving peers.
  // Reacquire these queries when the registered handles change.
  [[nodiscard]] BimWalkabilityQueries fleetWalkabilityQueries() const;
  template<class IgnoredBodies>
  [[nodiscard]] BimWalkabilityQueries walkabilityQueriesWithFilter(IgnoredBodies ignoredBodies) const;

  CookedBimCollision geometry_;
  physics::PhysicsWorld world_;
  std::unordered_map<const BimCollisionMesh*, physics::PhysicsStaticMesh> meshShapes_;
  std::vector<physics::PhysicsBodyId> rowBodies_;
  std::unordered_map<physics::PhysicsBodyId, size_t> bodyRows_;
  std::map<std::string, Door, std::less<>> doors_;
  std::map<physics::PhysicsBodyId, RobotBody> robots_;
  uint32_t robotCapacity_;
  bool driveOwned_{};
};

}  // namespace container::robotics
