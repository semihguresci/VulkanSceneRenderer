#pragma once

#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/BimWalkability.h"
#include "Container/robotics/DifferentialDrive.h"
#include "Container/robotics/TwinTelemetry.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace container::robotics {

struct BimFleetRobotMission {
  std::string robotId;
  std::string taskId;
  DrivePose spawn;
  // Ordered feet positions in the building's REP-103 frame in metres.
  std::vector<glm::dvec3> waypoints;
  double maxLinearMetersPerSecond{0.8};
  double maxAngularRadiansPerSecond{3.0};
};

struct BimFleetMissionOptions {
  uint32_t ticksPerSecond{60};
  uint32_t maxTicks{1200};
  double waypointToleranceMeters{0.08};
  std::vector<std::string> openDoorIds;
  std::vector<BimFleetRobotMission> robots;
};

struct BimFleetWaypointArrival {
  std::size_t waypointIndex{};
  uint64_t tick{};
  glm::dvec3 position{0.0};
};

struct BimFleetRobotResult {
  std::string robotId;
  std::string taskId;
  DrivePose finalPose;
  std::optional<uint64_t> completionTick;
  std::vector<BimFleetWaypointArrival> arrivals;
  uint64_t blockedTicks{};
  BimWalkabilityFailure lastObstruction{BimWalkabilityFailure::None};
  std::string blockingEntityId;
};

struct BimFleetMissionResult {
  uint64_t ticks{};
  bool completed{};
  TwinRecording recording;
  std::vector<BimFleetRobotResult> robots;
};

// Run the waypoint supervisor in one simulated Jolt world. The collision
// recipe and recording source evidence must come from the same BIM import.
// A deadline returns a partial run with terminal task states for replay.
[[nodiscard]] BimFleetMissionResult runBimFleetMission(
    CookedBimCollision geometry, std::string sourceNamespace, BimSceneSource source,
    const BimFleetMissionOptions& options);

} // namespace container::robotics
