#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>

namespace container::robotics::sensor_noise {

inline uint64_t mix(uint64_t value) noexcept {
  // SplitMix64's unsigned operations are independent of STL distributions.
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

inline double uniform(uint64_t key, uint64_t channel) noexcept {
  return static_cast<double>(mix(key + channel * 0x9e3779b97f4a7c15ULL) >> 11) * 0x1.0p-53;
}

inline double normal(uint64_t key) noexcept {
  // Box-Muller with 1-U in (0,1], including when U is exactly zero.
  return std::sqrt(-2 * std::log(1 - uniform(key, 1))) *
         std::cos(2 * std::numbers::pi * uniform(key, 2));
}

} // namespace container::robotics::sensor_noise
