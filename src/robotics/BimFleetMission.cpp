#include "Container/robotics/BimFleetMission.h"

#include "Container/robotics/DifferentialDriveSimulation.h"
#include "TwinTelemetryLimits.h"
#include "TwinTelemetryValidation.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace container::robotics {
namespace {

constexpr std::size_t maxRobots = 16;
constexpr std::size_t maxWaypoints = 256;
constexpr uint32_t maxTickRate = 10'000;
constexpr uint32_t maxMissionTicks = 8'191;
constexpr uint64_t nanosecondsPerSecond = 1'000'000'000;

bool positiveAtMost(double value, double limit) {
  return value > 0.0 && value <= limit;
}

void validateOptions(const BimFleetMissionOptions& options) {
  if (options.robots.empty() || options.robots.size() > maxRobots)
    throw std::invalid_argument("Fleet mission requires 1 to 16 robots");
  if (!options.ticksPerSecond || options.ticksPerSecond > maxTickRate ||
      !options.maxTicks || options.maxTicks > maxMissionTicks)
    throw std::invalid_argument("Fleet mission tick rate must be in [1, 10000] and maxTicks in [1, 8191]");
  if (!positiveAtMost(options.waypointToleranceMeters, 1.0))
    throw std::invalid_argument("Fleet waypoint tolerance must be positive and at most one metre");
}

DriveTwist trackWaypoint(const BimFleetRobotMission& mission, const BimFleetRobotResult& robot,
                         uint32_t ticksPerSecond) {
  if (robot.completionTick)
    return {};
  const auto delta = mission.waypoints[robot.arrivals.size()] - robot.finalPose.position;
  const double distance = std::hypot(delta.x, delta.y);
  if (distance <= 1e-9)
    return {};
  const double bearing = std::atan2(delta.y, delta.x);
  const double error = std::remainder(bearing - robot.finalPose.yaw, 2 * std::numbers::pi);
  const double speed = std::min(mission.maxLinearMetersPerSecond, distance * ticksPerSecond);
  const double turn = std::clamp(error * ticksPerSecond,
      -mission.maxAngularRadiansPerSecond, mission.maxAngularRadiansPerSecond);
  return {speed * std::max(0.0, std::cos(error)), turn};
}

void sampleRobot(const BimFleetRobotMission& mission, BimFleetRobotResult& robot,
                 const DifferentialDriveState& state, const BimFleetMissionOptions& options,
                 TwinRecording& recording, std::size_t& activeRobots) {
  robot.finalPose = state.base;
  const uint64_t time = state.tick * nanosecondsPerSecond / options.ticksPerSecond;
  TwinRobotPose pose{state.base.position, state.base.yaw};
  if (state.contact)
    pose.contact = TwinRobotContact{state.blockingEntityId, state.contact->position, state.contact->normal};
  recording.events.push_back({time, robot.robotId, "simulation:fleet", std::move(pose)});
  if (robot.completionTick)
    return;

  if (state.obstruction != BimWalkabilityFailure::None) {
    ++robot.blockedTicks;
    robot.lastObstruction = state.obstruction;
    robot.blockingEntityId = state.blockingEntityId;
  }
  const auto previous = robot.arrivals.size();
  while (robot.arrivals.size() < mission.waypoints.size()) {
    const auto index = robot.arrivals.size();
    const auto remaining = mission.waypoints[index] - state.base.position;
    if (std::hypot(remaining.x, remaining.y, remaining.z) > options.waypointToleranceMeters)
      break;
    robot.arrivals.push_back({index, state.tick, state.base.position});
  }
  const bool arrived = robot.arrivals.size() == mission.waypoints.size();
  if (arrived) {
    robot.completionTick = state.tick;
    --activeRobots;
  }
  const bool timedOut = !arrived && state.tick == options.maxTicks;
  if (state.tick == 0 || arrived || timedOut || robot.arrivals.size() != previous) {
    const double progress = double(robot.arrivals.size()) / double(mission.waypoints.size());
    recording.events.push_back({time, robot.robotId, "mission:waypoint-supervisor",
        TwinTaskStatus{robot.taskId, arrived ? "succeeded" : timedOut ? "timed-out" : "running", progress}});
  }
}

} // namespace

BimFleetMissionResult runBimFleetMission(CookedBimCollision geometry, std::string sourceNamespace,
                                        BimSceneSource source, const BimFleetMissionOptions& options) {
  validateOptions(options);
  BimFleetMissionResult result;
  auto& recording = result.recording;
  recording.sourceNamespace = std::move(sourceNamespace);
  recording.sceneSource = std::move(source);
  recording.clockId = "fleet-fixed-step";
  twin_telemetry_validation::validateRecordingMetadata(recording);

  std::vector<const BimFleetRobotMission*> sorted;
  sorted.reserve(options.robots.size());
  for (const auto& robot : options.robots)
    sorted.push_back(&robot);
  std::ranges::sort(sorted, {}, [](const auto* robot) { return std::string_view(robot->robotId); });

  std::vector<FleetRobotSpawn> spawns;
  spawns.reserve(sorted.size());
  result.robots.reserve(sorted.size());
  std::size_t eventBudget = 0;
  for (const auto* robot : sorted) {
    if (!result.robots.empty() && result.robots.back().robotId == robot->robotId)
      throw std::invalid_argument("Duplicate fleet robot identity " + robot->robotId);
    if (robot->waypoints.empty() || robot->waypoints.size() > maxWaypoints)
      throw std::invalid_argument("Fleet robot " + robot->robotId + " requires 1 to 256 waypoints");
    if (!positiveAtMost(robot->maxLinearMetersPerSecond, 2.0) ||
        !positiveAtMost(robot->maxAngularRadiansPerSecond, 10.0))
      throw std::invalid_argument("Fleet robot " + robot->robotId + " has an invalid waypoint speed limit");

    eventBudget += std::size_t(options.maxTicks) + robot->waypoints.size() + 4;
    if (eventBudget > twin_telemetry_limits::maxEvents)
      throw std::invalid_argument("Fleet mission can exceed the twin recording event limit");

    using twin_telemetry_validation::validateEvent;
    validateEvent({0, robot->robotId, "simulation:fleet", TwinRobotPose{robot->spawn.position, robot->spawn.yaw}});
    validateEvent({0, robot->robotId, "mission:waypoint-supervisor", TwinTaskStatus{robot->taskId, "running", 0.0}});
    std::vector<glm::dvec3> plan{robot->spawn.position};
    plan.insert(plan.end(), robot->waypoints.begin(), robot->waypoints.end());
    TwinEvent planEvent{0, robot->robotId, "mission:waypoint-supervisor", TwinPlannedPath{std::move(plan)}};
    validateEvent(planEvent);
    recording.events.push_back(std::move(planEvent));

    DifferentialDriveSimulationOptions drive;
    drive.robotId = robot->robotId;
    drive.ticksPerSecond = options.ticksPerSecond;
    drive.maxHistorySamples = 2;
    spawns.push_back({robot->spawn, std::move(drive)});
    result.robots.push_back({.robotId = robot->robotId, .taskId = robot->taskId});
  }
  recording.events.reserve(eventBudget);

  BimCollisionWorld world(std::move(geometry), options.openDoorIds, 1, static_cast<uint32_t>(sorted.size()));
  DifferentialDriveFleetSimulation fleet(world, std::move(spawns));
  std::size_t activeRobots = sorted.size();
  const auto sample = [&] {
    for (std::size_t index = 0; index < sorted.size(); ++index) {
      auto& robot = result.robots[index];
      sampleRobot(*sorted[index], robot, fleet.state(robot.robotId), options, recording, activeRobots);
    }
  };

  sample();
  while (activeRobots && fleet.clock().tick() < options.maxTicks) {
    const auto tick = fleet.clock().tick();
    for (std::size_t index = 0; index < sorted.size(); ++index) {
      const auto& robot = result.robots[index];
      fleet.queueCommand(robot.robotId, tick, trackWaypoint(*sorted[index], robot, options.ticksPerSecond));
    }
    fleet.step();
    sample();
  }

  result.ticks = fleet.clock().tick();
  result.completed = activeRobots == 0;
  canonicalizeTwinRecording(recording);
  return result;
}

} // namespace container::robotics
