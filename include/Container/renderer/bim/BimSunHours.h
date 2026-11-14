#pragma once

#include "Container/renderer/lighting/SolarPosition.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace container::renderer {

inline constexpr uint32_t kMaxBimSunHoursGridDimension = 128;
inline constexpr size_t kMaxBimSunHoursCells = 16'384;
inline constexpr size_t kMaxBimSunHoursSamples = 1'440;
inline constexpr size_t kMaxBimSunHoursRays = 3'000'000;
inline constexpr size_t kMaxBimSunHoursTriangles = 1'000'000;

struct BimSunHoursSettings {
  SolarTime solar; // location, date and UTC offset (the window supplies local times)
  double northDegrees{}; // rotation of north from world +Z toward +X
  double minX{-5.0}, minZ{-5.0}, maxX{5.0}, maxZ{5.0};
  double elevation{};
  uint32_t columns{16}, rows{16};
  uint32_t startMinute{480}, endMinute{1080}, stepMinutes{15};
};

struct BimSunTriangle {
  std::array<glm::dvec3, 3> vertices; // world positions, opaque on both sides
};

struct BimSunHoursResult {
  BimSunHoursSettings settings;
  std::vector<double> hours; // row * columns + column, increasing X and Z
  size_t sampleCount{};
  size_t daylightSampleCount{};
  size_t skippedTriangles{};
  double daylightHours{};
  bool cancelled{};
};

void ValidateBimSunHoursSettings(const BimSunHoursSettings&);
[[nodiscard]] glm::dvec3 BimSunHoursCellCenter(const BimSunHoursSettings&, uint32_t column,
                                              uint32_t row);
[[nodiscard]] glm::dvec3 BimSunHoursCellCenter(const BimSunHoursSettings&, size_t cellIndex);
[[nodiscard]] glm::vec3 BimSunHoursColor(double hours, double maxHours);

// A successful result covers the full window. Invalid input or a workload over
// the study limits throws. Cancellation returns settings and no accumulated data.
[[nodiscard]] BimSunHoursResult RunBimSunHours(const BimSunHoursSettings&,
                                             std::span<const BimSunTriangle>,
                                             std::stop_token stop = {});

} // namespace container::renderer
