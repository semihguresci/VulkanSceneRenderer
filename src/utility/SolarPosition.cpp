#include "Container/renderer/lighting/SolarPosition.h"

#include <chrono>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace container::renderer {

bool validSolarTime(const SolarTime& local) {
  using namespace std::chrono;
  return local.latitudeDegrees >= -90.0f && local.latitudeDegrees <= 90.0f &&
         local.longitudeDegrees >= -180.0f && local.longitudeDegrees <= 180.0f &&
         local.year >= 1901 && local.year <= 2099 &&
         local.month >= 1 && local.month <= 12 && local.day >= 1 && local.day <= 31 &&
         (year(local.year) / month(unsigned(local.month)) / day(unsigned(local.day))).ok() &&
         local.hour >= 0 && local.hour < 24 && local.minute >= 0 && local.minute < 60 &&
         local.utcOffsetMinutes >= -720 && local.utcOffsetMinutes <= 840;
}

SolarAngles solarPosition(const SolarTime& local) {
  if (!validSolarTime(local))
    throw std::invalid_argument("Invalid solar location, calendar date, local time, or UTC offset");
  using namespace std::chrono;
  constexpr double rad = std::numbers::pi / 180.0;
  const sys_days day = year(local.year) / month(unsigned(local.month)) / unsigned(local.day);
  const double utcMinutes = 60.0 * local.hour + local.minute - local.utcOffsetMinutes;
  // NOAA solar-calculator equations. Use a Gregorian calendar and fractional
  // UTC day, so the calculation also handles offsets across midnight.
  const double julian = 2440587.5 + double(day.time_since_epoch().count()) + utcMinutes / 1440.0;
  const double t = (julian - 2451545.0) / 36525.0;
  const double meanLongitude = std::remainder(280.46646 + t * (36000.76983 + 0.0003032 * t), 360.0) * rad;
  const double anomaly = (357.52911 + t * (35999.05029 - 0.0001537 * t)) * rad;
  const double eccentricity = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
  const double center = (1.914602 - t * (0.004817 + 0.000014 * t)) * std::sin(anomaly) +
      (0.019993 - 0.000101 * t) * std::sin(2 * anomaly) + 0.000289 * std::sin(3 * anomaly);
  const double node = (125.04 - 1934.136 * t) * rad;
  const double longitude = meanLongitude + (center - 0.00569 - 0.00478 * std::sin(node)) * rad;
  const double tilt = (23 + 26.0 / 60 + (21.448 - t * (46.815 + t * (0.00059 - 0.001813 * t))) / 3600 +
                       0.00256 * std::cos(node)) * rad;
  const double declination = std::asin(std::sin(tilt) * std::sin(longitude));
  const double halfTilt = std::tan(0.5 * tilt);
  const double y = halfTilt * halfTilt;
  const double timeCorrection = (4 / rad) * (
      y * std::sin(2 * meanLongitude) - 2 * eccentricity * std::sin(anomaly) +
      4 * eccentricity * y * std::sin(anomaly) * std::cos(2 * meanLongitude) -
      0.5 * y * y * std::sin(4 * meanLongitude) -
      1.25 * eccentricity * eccentricity * std::sin(2 * anomaly));

  const double latitude = local.latitudeDegrees * rad;
  const double hourAngle =
      std::remainder((utcMinutes + timeCorrection) / 4 + local.longitudeDegrees - 180.0, 360.0) * rad;
  const double east = -std::cos(declination) * std::sin(hourAngle);
  const double meridian = std::cos(declination) * std::cos(hourAngle);
  const double north = std::cos(latitude) * std::sin(declination) - std::sin(latitude) * meridian;
  const double up = std::sin(latitude) * std::sin(declination) + std::cos(latitude) * meridian;
  const double horizontal = std::hypot(east, north);
  return {horizontal > 1e-12 ? std::atan2(east, north) / rad : 0.0,
          std::atan2(up, horizontal) / rad};
}

} // namespace container::renderer
