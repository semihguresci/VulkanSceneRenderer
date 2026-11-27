#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace container::robotics {

// Fixed time is tick / ticksPerSecond. The wall-time adapter retains the
// integer fractional tick and any whole tick debt. The caller owns the clock.
class SimClock {
public:
  using Tick = std::function<void(uint64_t tickIndex, double seconds)>;

  explicit SimClock(uint32_t ticksPerSecond = 60, uint32_t maxTicksPerAdvance = 120);

  [[nodiscard]] uint64_t tick() const noexcept { return tick_; }
  [[nodiscard]] double seconds() const noexcept;
  [[nodiscard]] double tickSeconds() const noexcept;
  [[nodiscard]] uint64_t pendingTicks() const noexcept { return pendingTicks_; }
  [[nodiscard]] bool paused() const noexcept { return paused_; }
  void setPaused(bool paused) noexcept { paused_ = paused; }

  // execute receives the next command tick index and the fixed step duration.
  // afterTick receives the committed tick index and the same duration. If it
  // throws, the committed tick stays consumed and advance(0) can drain the debt.
  uint32_t advance(std::chrono::nanoseconds elapsed, const Tick& execute, const Tick& afterTick = {});
  void step(const Tick& execute, const Tick& afterTick = {});
  [[nodiscard]] bool inTickCallback() const noexcept { return executing_; }
  void reset();

private:
  void invoke(const Tick& callback);
  uint32_t frequency_;
  uint32_t maxTicksPerAdvance_;
  uint64_t tick_{};
  uint64_t pendingTicks_{};
  uint32_t fractional_{};
  bool paused_{};
  bool executing_{};
};

} // namespace container::robotics
