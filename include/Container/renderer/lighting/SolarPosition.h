#pragma once

namespace container::renderer {

struct SolarTime {
  bool enabled{false};
  float latitudeDegrees{0.0f};
  float longitudeDegrees{0.0f};
  int year{2026};
  int month{6};
  int day{21};
  int hour{12};
  int minute{0};
  int utcOffsetMinutes{0};
};

struct SolarAngles {
  double azimuthDegrees{};
  double elevationDegrees{};
};

[[nodiscard]] bool validSolarTime(const SolarTime& local);
// Geometric solar elevation and eastward azimuth from geographic north.
[[nodiscard]] SolarAngles solarPosition(const SolarTime& local);

} // namespace container::renderer
