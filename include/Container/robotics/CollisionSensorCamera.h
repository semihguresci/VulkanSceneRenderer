#pragma once

#include "Container/robotics/LidarQuery.h"
#include "Container/robotics/SensorCamera.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace container::robotics {

using CollisionSensorColorLookup = std::function<std::optional<std::array<uint8_t, 3>>(std::string_view entityId)>;

struct CollisionSensorImage {
  // Top-down, row-major RGB8 pixels. Misses are black and hits without a
  // supplied entity color are neutral gray (180 in each channel).
  std::vector<uint8_t> rgb8;
  // Positive axial metres, with a quiet NaN for each miss.
  std::vector<float> depth32fc1;
};

// The collision camera accepts ideal pinhole settings up to 256 pixels per
// axis and 65,536 total. Distortion, noise, and appearance effects require
// the rendered sensor camera.
void validateCollisionSensorCameraSettings(const SensorCameraSettings& settings);

// The callback supplies the nearest hit of each near-to-far ray segment in
// the metric world. Callers can exclude the sensing body from those queries.
[[nodiscard]] CollisionSensorImage captureCollisionSensorCamera(
    const SensorCameraSettings& settings, const SensorCameraFrame& frame, const LidarCast& cast,
    const CollisionSensorColorLookup& color = {});

} // namespace container::robotics
