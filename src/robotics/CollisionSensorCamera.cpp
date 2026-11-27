#include "Container/robotics/CollisionSensorCamera.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace container::robotics {
namespace {

constexpr uint32_t maxImageAxis = 256;
constexpr uint64_t maxImagePixels = 65'536;
constexpr std::array<uint8_t, 3> defaultHitColor{180, 180, 180};

bool finite(glm::dvec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

void validateCollisionSensorCameraSettings(const SensorCameraSettings& settings) {
  if (settings.width == 0 || settings.height == 0 || settings.width > maxImageAxis ||
      settings.height > maxImageAxis || uint64_t(settings.width) * settings.height > maxImagePixels)
    throw std::invalid_argument("Collision camera requires 1..256 pixels per axis and at most 65536 pixels");
  if (settings.distortion)
    throw std::invalid_argument("Collision camera does not support lens distortion");
  if (settings.depthNoiseStdDevMeters != 0 || settings.depthNoiseSeed != 0 ||
      settings.depthDropoutProbability != 0 || settings.rgbNoiseStdDevLinear != 0 || settings.rgbNoiseSeed != 0)
    throw std::invalid_argument("Collision camera does not support depth or RGB noise or dropout");
  if (settings.appearance)
    throw std::invalid_argument("Collision camera does not support appearance effects");
  validateSensorCameraSettings(settings);
}

CollisionSensorImage captureCollisionSensorCamera(
    const SensorCameraSettings& settings, const SensorCameraFrame& frame, const LidarCast& cast,
    const CollisionSensorColorLookup& color) {
  validateCollisionSensorCameraSettings(settings);
  validateSensorCameraFrame(frame);
  if (!cast)
    throw std::invalid_argument("Collision camera requires a raycast callback");

  const auto pixelCount = std::size_t(settings.width) * settings.height;
  CollisionSensorImage image{std::vector<uint8_t>(pixelCount * 3),
                             std::vector<float>(pixelCount, std::numeric_limits<float>::quiet_NaN())};
  const auto& worldFromCamera = frame.worldFromCamera;
  const auto cameraPosition = glm::dvec3(worldFromCamera[3]);
  const double depthSpan = settings.farMeters - settings.nearMeters;
  for (uint32_t row = 0; row < settings.height; ++row) {
    const double up = (settings.cy - (double(row) + 0.5)) / settings.fy;
    for (uint32_t column = 0; column < settings.width; ++column) {
      const double right = (double(column) + 0.5 - settings.cx) / settings.fx;
      // Keep the forward component at one axial metre per unit of ray scale.
      // Normalizing here would turn the clipping planes and depth into range.
      const auto direction = glm::dvec3(worldFromCamera * glm::dvec4(right, up, -1, 0));
      const auto origin = cameraPosition + direction * settings.nearMeters;
      const auto displacement = direction * depthSpan;
      if (!finite(origin) || !finite(displacement) || !finite(origin + displacement))
        throw std::invalid_argument("Collision camera ray endpoints must be finite");
      const auto hit = cast(origin, displacement);
      if (!hit) continue;
      if (!std::isfinite(hit->fraction) || hit->fraction < 0 || hit->fraction > 1)
        throw std::invalid_argument("Collision camera raycast callback returned a fraction outside [0,1]");
      const auto index = std::size_t(row) * settings.width + column;
      image.depth32fc1[index] = static_cast<float>(std::lerp(settings.nearMeters, settings.farMeters, hit->fraction));
      const auto rgb = color ? color(hit->entityId).value_or(defaultHitColor) : defaultHitColor;
      std::ranges::copy(rgb, image.rgb8.data() + index * 3);
    }
  }
  return image;
}

} // namespace container::robotics
