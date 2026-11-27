#pragma once

#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/DifferentialDrive.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace container::robotics {

struct NavigationTrainingLayout {
  CookedBimCollision geometry;
  DrivePose spawn;
  glm::dvec3 goal{0.0};
  std::vector<std::string> openDoorIds;
  double ambientLight{};
  double directionalLight{};
  double lightYaw{};
  double rangeNoiseMeters{};
  double encoderNoiseRadians{};
  nlohmann::json manifest;
};

// Holds immutable BIM geometry and a metric episode recipe. A sample owns its
// collider occurrences so furniture moves cannot modify another episode.
class NavigationTrainingScene {
public:
  explicit NavigationTrainingScene(const std::filesystem::path& recipe);
  [[nodiscard]] const nlohmann::json& identity() const;
  [[nodiscard]] NavigationTrainingLayout sample(uint64_t seed, bool randomize) const;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};

}  // namespace container::robotics
