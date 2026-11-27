#pragma once

#include "Container/robotics/BimCollisionWorld.h"
#include "Container/robotics/DifferentialDrive.h"
#include "Container/robotics/SimClock.h"

#include <glm/mat4x4.hpp>

#include <deque>
#include <memory>
#include <string>

namespace container::robotics {

struct DifferentialDriveSimulationOptions {
  DifferentialDriveSettings drive;
  BimWalkabilitySettings capsule;
  uint32_t ticksPerSecond{60};
  uint32_t maxTicksPerAdvance{120};
  size_t maxHistorySamples{8192};
  uint64_t seed{};
  // Standard deviation of independent uniform encoder travel error in radians
  // per moving wheel per tick. Zero uses ideal encoders.
  double encoderNoiseRadians{};
  std::string robotId{"robot/base"};
};

struct DifferentialDriveState {
  uint64_t tick{};
  double seconds{};
  DrivePose base;
  DrivePose odometry;
  DriveWheels wheelAngles;
  DriveWheels wheelRates;
  DriveWheels encoderAngles;
  BimWalkabilityFailure obstruction{BimWalkabilityFailure::None};
  std::string blockingEntityId;
};

struct DifferentialDriveFrames {
  glm::dmat4 mapFromOdometry{1.0};
  glm::dmat4 odometryFromBase{1.0};
  glm::dmat4 baseFromLeftWheel{1.0};
  glm::dmat4 baseFromRightWheel{1.0};
};

// Borrows one BIM Jolt world; one drive at a time may attach to it. The
// owning thread supplies commands, ticks and door updates. The world must
// outlive the simulation. Renderer callers can consume snapshots separately.
class DifferentialDriveSimulation {
public:
  DifferentialDriveSimulation(BimCollisionWorld& world, DrivePose spawn,
                              DifferentialDriveSimulationOptions options = {});
  ~DifferentialDriveSimulation();
  DifferentialDriveSimulation(const DifferentialDriveSimulation&) = delete;
  DifferentialDriveSimulation& operator=(const DifferentialDriveSimulation&) = delete;

  void queueCommand(uint64_t tick, DriveTwist command);
  void clearCommands() noexcept;
  uint32_t advance(std::chrono::nanoseconds elapsed);
  void step();
  // Restore spawn, authored world state, clock and encoder seed. Retain the
  // scheduled velocity commands for repeatable playback.
  void reset();
  void setPaused(bool paused) noexcept;

  [[nodiscard]] const SimClock& clock() const noexcept;
  [[nodiscard]] const DifferentialDriveState& state() const noexcept;
  [[nodiscard]] const std::deque<DifferentialDriveState>& history() const noexcept;
  [[nodiscard]] DifferentialDriveFrames frames() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace container::robotics
