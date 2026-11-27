#pragma once

#include <cstddef>

namespace container::robotics::twin_telemetry_limits {

inline constexpr std::size_t maxEvents = 65'536;
inline constexpr std::size_t maxSceneLayers = 256;
inline constexpr std::size_t maxPlanPoints = 4'096;
inline constexpr std::size_t maxTotalPlanPoints = 262'144;
inline constexpr std::size_t maxObservedPoints = 2'048;
inline constexpr std::size_t maxIdentityBytes = 1'024;
inline constexpr std::size_t maxLabelBytes = 256;
inline constexpr double maxPositionMeters = 1e8;
inline constexpr double maxYawRadians = 1e9;

} // namespace container::robotics::twin_telemetry_limits
