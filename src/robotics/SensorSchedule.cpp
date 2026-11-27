#include "Container/robotics/SensorSchedule.h"

#include <limits>
#include <stdexcept>

namespace container::robotics {
namespace {

uint64_t denominator(uint32_t ticksPerSecond) noexcept {
  return uint64_t(ticksPerSecond) * 1000;
}

} // namespace

SensorSchedule::SensorSchedule(uint32_t ticksPerSecond, uint32_t samplesPerSecondMilliHz,
                               uint64_t firstTick)
    : ticksPerSecond_(ticksPerSecond), sampleMilliHz_(samplesPerSecondMilliHz), firstTick_(firstTick) {
  if (ticksPerSecond == 0 || ticksPerSecond > 1'000'000 || sampleMilliHz_ == 0 ||
      sampleMilliHz_ > denominator(ticksPerSecond_))
    throw std::invalid_argument("Sensor schedule requires 1..1000000 tick Hz and a positive sample rate at most one per tick");
}

bool SensorSchedule::due(uint64_t tick) const noexcept {
  if (tick < firstTick_) return false;
  const auto elapsed = tick - firstTick_;
  if (elapsed == 0) return true;
  const auto divisor = denominator(ticksPerSecond_);
  // floor(t*n/d) advances at ceil(k*d/n). The modular product avoids
  // multiplying a potentially enormous absolute tick by the sample rate.
  return ((elapsed % divisor) * sampleMilliHz_) % divisor < sampleMilliHz_;
}

uint64_t SensorSchedule::periodsThrough(uint64_t elapsed) const noexcept {
  const auto divisor = denominator(ticksPerSecond_);
  // sampleMilliHz_ <= divisor <= 1e9. The remainder product fits in uint64_t
  // and the resulting count of elapsed periods is at most elapsed.
  return (elapsed / divisor) * sampleMilliHz_ + ((elapsed % divisor) * sampleMilliHz_) / divisor;
}

uint64_t SensorSchedule::samplesThrough(uint64_t tick) const {
  if (tick < firstTick_) return 0;
  const auto count = periodsThrough(tick - firstTick_);
  if (count == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Inclusive sensor sample count exceeds the integer range");
  return count + 1;
}

uint64_t SensorSchedule::sampleIndex(uint64_t tick) const {
  if (!due(tick)) throw std::invalid_argument("Sensor sample requested outside its fixed-tick schedule");
  return periodsThrough(tick - firstTick_);
}

double SensorSchedule::seconds(uint64_t tick) const noexcept {
  return static_cast<double>(tick) / ticksPerSecond_;
}

} // namespace container::robotics
