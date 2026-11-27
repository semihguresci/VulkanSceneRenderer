#pragma once

#include "Container/robotics/SensorSchedule.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace container::robotics {

struct SensorCameraPose {
  glm::dvec3 position{0.0};
  glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
};

// Brown-Conrady parameters in normalized image coordinates (+X right, +Y down).
struct SensorCameraDistortion {
  double k1{}, k2{}, p1{}, p2{}, k3{};
};

struct SensorCameraSettings {
  uint32_t width{1280}, height{720};
  // Pinhole parameters in pixels from the top-left image edge. Pixel centers
  // are (column + 0.5, row + 0.5).
  double fx{640.0}, fy{640.0}, cx{640.0}, cy{360.0};
  // Positive optical-axis distances in metres, not Euclidean range.
  double nearMeters{0.05}, farMeters{40.0};
  // Camera axes are +X right, +Y up and -Z forward. The default mount maps
  // these to -Y, +Z and +X in the REP-103 robot base, respectively.
  SensorCameraPose baseFromCamera{{0.0, 0.0, 0.0}, {0.5, 0.5, -0.5, -0.5}};
  std::optional<SensorCameraDistortion> distortion;
  double depthNoiseStdDevMeters{};
  uint64_t depthNoiseSeed{};
};

struct SensorCameraFrame {
  uint64_t tick{};
  double seconds{};
  uint64_t sampleIndex{};
  // Rigid, column-major transform from the camera axes to the metric
  // REP-103 world. The columns of the linear part have unit length.
  glm::dmat4 worldFromCamera{1.0};
};

struct SensorCameraPlayback {
  SensorCameraSettings settings;
  SensorSchedule schedule{60, 10'000};
  std::vector<SensorCameraFrame> frames;
};

void validateSensorCameraSettings(const SensorCameraSettings& settings);
void validateSensorCameraFrame(const SensorCameraFrame& frame);

// Unflipped, right-handed pinhole projection for a [0, 1] reverse-Z depth
// buffer. metersPerRendererUnit converts the near/far distances to the units
// of the camera-space input. Lens distortion is applied during acquisition
// or export. A Vulkan Y flip must also flip the principal-point Y offset.
[[nodiscard]] glm::dmat4 sensorCameraProjection(const SensorCameraSettings& settings,
                                                 double metersPerRendererUnit = 1.0);
[[nodiscard]] SensorCameraFrame makeSensorCameraFrame(const SensorCameraSettings& settings,
                                                       const SensorSchedule& schedule, uint64_t tick,
                                                       const SensorCameraPose& worldFromBase);

// A version-1 container.sensor-camera spec and optional version-1
// container.bim-robotics-simulation report. Playback indexes committed base
// poses by tick. A static spec defaults to one capture at the first tick.
// JSON poses use xyz in metres and fixed-axis rpy in radians. Mount rpy
// rotates the default camera basis in the base coordinate system.
[[nodiscard]] SensorCameraPlayback parseSensorCameraPlayback(
    std::string_view specJson, std::optional<std::string_view> trajectoryJson = std::nullopt);
[[nodiscard]] SensorCameraPlayback loadSensorCameraPlayback(
    const std::filesystem::path& specPath,
    const std::optional<std::filesystem::path>& trajectoryPath = std::nullopt);

} // namespace container::robotics
