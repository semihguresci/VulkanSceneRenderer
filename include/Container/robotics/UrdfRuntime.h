#pragma once

#include "Container/robotics/SimClock.h"
#include "Container/robotics/UrdfModel.h"

#include <span>

namespace container::robotics {

struct UrdfJointValue {
  std::string_view joint;
  double position{};
};

struct UrdfJointState {
  double position{};
  double velocity{};
  double target{};
};

// Independent CPU joint state and metric REP-103 forward kinematics. World
// poses are derived from authored frames; readers never mutate the model.
class UrdfRuntime {
public:
  explicit UrdfRuntime(UrdfModel model, UrdfPose worldFromRoot = {},
                       uint32_t ticksPerSecond = 60, uint32_t maxTicksPerAdvance = 120);
  // Keep model identity stable for attached consumers of its link indices.
  UrdfRuntime(const UrdfRuntime&) = delete;
  UrdfRuntime& operator=(const UrdfRuntime&) = delete;
  UrdfRuntime(UrdfRuntime&&) = delete;
  UrdfRuntime& operator=(UrdfRuntime&&) = delete;

  [[nodiscard]] const UrdfModel& model() const noexcept { return model_; }
  [[nodiscard]] const SimClock& clock() const noexcept { return clock_; }
  [[nodiscard]] std::span<const UrdfJointState> joints() const noexcept { return joints_; }
  [[nodiscard]] std::span<const glm::dmat4> worldFromLinks() const noexcept { return worldFromLinks_; }

  // Batch validation is atomic. Fixed joints are not commandable. Direct
  // position samples also zero joint velocity and replace their target.
  void setPositions(std::span<const UrdfJointValue> values);
  void setTargets(std::span<const UrdfJointValue> values);
  void setWorldFromRoot(const UrdfPose& pose);
  uint32_t advance(std::chrono::nanoseconds elapsed);
  void step();
  void setPaused(bool paused) noexcept { clock_.setPaused(paused); }
  // Restore the initial root/positions/targets, clear the clock and pause.
  void reset();

  [[nodiscard]] glm::dmat4 worldFromVisual(size_t link, size_t visual) const;
  [[nodiscard]] glm::dmat4 worldFromCollision(size_t link, size_t collision) const;
  [[nodiscard]] std::optional<glm::dmat4> worldFromInertial(size_t link) const;

private:
  std::vector<UrdfJointState> validated(std::span<const UrdfJointValue> values, bool direct) const;
  std::vector<glm::dmat4> placeLinks(const glm::dmat4& root, std::span<const UrdfJointState> joints) const;
  void executeTick(double dt);

  UrdfModel model_;
  SimClock clock_;
  glm::dmat4 initialRoot_;
  glm::dmat4 root_;
  std::vector<UrdfJointState> initialJoints_;
  std::vector<UrdfJointState> joints_;
  std::vector<glm::dmat4> worldFromLinks_;
};

} // namespace container::robotics
