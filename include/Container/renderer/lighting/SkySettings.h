#pragma once

#include "Container/renderer/lighting/SolarPosition.h"
#include "Container/renderer/lighting/AtmosphereLutData.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace container::renderer {

enum class SkyMode : uint32_t { Hdr = 0, Plain = 1, Gradient = 2, Atmosphere = 3 };

struct AtmosphereSettings {
  float sunAzimuthDegrees{0.0f};
  float sunElevationDegrees{35.0f};
  float solarIrradiance{20.0f};
  float planetRadiusKm{6360.0f};
  float atmosphereHeightKm{100.0f};
  float observerHeightKm{0.002f};
  float rayleighScale{1.0f};
  float mieScale{1.0f};
  float ozoneScale{1.0f};
  float aerialMetersPerUnit{0.0f};
  bool multipleScattering{false};
  bool lightScene{true};
  SolarTime localSun{};
};

struct AtmosphereSun {
  std::array<float, 3> direction{};
  std::array<float, 3> irradiance{};
  bool lightScene{};
};

struct SkySettings {
  SkyMode mode{SkyMode::Hdr};
  std::array<float, 3> horizon{0.65f, 0.76f, 0.9f};
  std::array<float, 3> zenith{0.12f, 0.3f, 0.65f};
  std::array<float, 3> ground{0.12f, 0.10f, 0.08f};
  AtmosphereSettings atmosphere{};
  float zenithCurve{1.0f};
  float groundCurve{1.0f};
  std::array<float, 3> gradientUp{0.0f, 1.0f, 0.0f};
};

[[nodiscard]] AtmosphereLut buildAtmosphereLut(const SkySettings& sky);

[[nodiscard]] inline bool validGradient(const SkySettings& sky) {
  const auto& up = sky.gradientUp;
  const float lengthSquared = up[0]*up[0] + up[1]*up[1] + up[2]*up[2];
  return sky.zenithCurve >= 0.001f && sky.zenithCurve <= 1.0f &&
         sky.groundCurve >= 0.001f && sky.groundCurve <= 1.0f &&
         lengthSquared >= 0.999f && lengthSquared <= 1.001f;
}

[[nodiscard]] inline bool validAtmosphere(const AtmosphereSettings& a) {
  const auto in = [](float x, float min, float max) { return x >= min && x <= max; };
  return in(a.sunAzimuthDegrees, -180.0f, 180.0f) &&
         in(a.sunElevationDegrees, -90.0f, 90.0f) &&
         in(a.solarIrradiance, 0.0f, 64.0f) &&
         in(a.planetRadiusKm, 1000.0f, 10000.0f) &&
         in(a.atmosphereHeightKm, 40.0f, 200.0f) &&
         in(a.observerHeightKm, 0.002f, a.atmosphereHeightKm) &&
         in(a.rayleighScale, 0.0f, 4.0f) &&
         in(a.mieScale, 0.0f, 4.0f) && in(a.ozoneScale, 0.0f, 4.0f) &&
         in(a.aerialMetersPerUnit, 0.0f, 1000.0f) &&
         (!a.localSun.enabled || validSolarTime(a.localSun));
}

} // namespace container::renderer
