#pragma once

#include <cstdint>

namespace container::robotics {

// The first sample occurs at firstTick. Each following sample uses the first
// fixed tick at or after its ideal time, with at most one sample per tick.
class SensorSchedule {
public:
  SensorSchedule(uint32_t ticksPerSecond, uint32_t samplesPerSecondMilliHz, uint64_t firstTick = 0);
  [[nodiscard]] bool due(uint64_t tick) const noexcept;
  // Inclusive sample count through this tick.
  [[nodiscard]] uint64_t samplesThrough(uint64_t tick) const;
  // Zero-based index of a due sample.
  [[nodiscard]] uint64_t sampleIndex(uint64_t tick) const;
  [[nodiscard]] double seconds(uint64_t tick) const noexcept;
  [[nodiscard]] uint32_t ticksPerSecond() const noexcept { return ticksPerSecond_; }
  [[nodiscard]] uint32_t samplesPerSecondMilliHz() const noexcept { return sampleMilliHz_; }
  [[nodiscard]] uint64_t firstTick() const noexcept { return firstTick_; }

private:
  [[nodiscard]] uint64_t periodsThrough(uint64_t elapsed) const noexcept;
  uint32_t ticksPerSecond_;
  uint32_t sampleMilliHz_;
  uint64_t firstTick_;
};

} // namespace container::robotics
