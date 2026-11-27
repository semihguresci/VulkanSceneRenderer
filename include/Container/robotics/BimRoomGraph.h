#pragma once

#include "Container/robotics/BimWalkability.h"
#include "Container/robotics/RoboticsScene.h"

#include <array>
#include <span>

namespace container::robotics {

class BimCollisionWorld;

enum class BimPortalState { Closed, Locked, Open };

struct BimRoom {
  std::string id;
  std::string guid;
  std::string name;
  std::vector<std::string> storeyIds;
  bool hasVolume{};
};

struct BimPortalCrossing {
  glm::dvec3 fromFeet;
  glm::dvec3 toFeet;
  BimWalkabilityFailure result;
};

struct BimRoomPortal {
  std::string doorId;
  std::string doorGuid;
  std::string openingId;
  std::string hostId;
  std::array<std::string, 2> roomIds;
  std::string evidence;
  BimPortalState state;
  std::optional<BimPortalCrossing> forward;
  std::optional<BimPortalCrossing> reverse;
};

struct BimRoomGraph {
  std::vector<BimRoom> rooms;
  std::vector<BimRoomPortal> portals;
  std::vector<RoboticsDiagnostic> diagnostics;
};

// 'roomIds' denotes architectural adjacency. A directed crossing is
// traversable only when a walk witness exists with result None. The physics
// world owns open-door state. Locked IDs must refer to closed scene doors.
[[nodiscard]] BimRoomGraph buildBimRoomGraph(
    const geometry::dotbim::Model& model, const RoboticsScene& scene,
    const BimCollisionWorld& world, std::span<const std::string> lockedDoorIds = {},
    const BimWalkabilitySettings& agent = {});

} // namespace container::robotics
