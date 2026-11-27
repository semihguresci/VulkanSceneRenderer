#include "Container/robotics/SimClock.h"

#include <limits>
#include <stdexcept>

namespace container::robotics {
namespace {

constexpr uint64_t nanosPerSecond = 1'000'000'000;
constexpr auto maxTick = std::numeric_limits<uint64_t>::max();

} // namespace

SimClock::SimClock(uint32_t ticksPerSecond, uint32_t maxTicksPerAdvance)
    : frequency_(ticksPerSecond), maxTicksPerAdvance_(maxTicksPerAdvance) {
  if (frequency_ == 0 || maxTicksPerAdvance_ == 0)
    throw std::invalid_argument("Simulation clock requires a positive tick rate and batch budget");
}

double SimClock::seconds() const noexcept {
  return static_cast<double>(tick_) / frequency_;
}

double SimClock::tickSeconds() const noexcept {
  return 1.0 / frequency_;
}

uint32_t SimClock::advance(std::chrono::nanoseconds elapsed, const Tick& execute) {
  if (executing_) throw std::logic_error("A clock callback cannot advance the clock recursively");
  if (elapsed.count() < 0) throw std::invalid_argument("Simulation elapsed time cannot be negative");
  if (paused_) return 0;

  // Divide elapsed before multiplying so the subsecond numerator has a fixed
  // bound even for the largest representable duration and clock frequency.
  const auto nanos = static_cast<uint64_t>(elapsed.count());
  const auto wholeSeconds = nanos / nanosPerSecond;
  const auto fractional = (nanos % nanosPerSecond) * frequency_ + fractional_;
  const auto available = maxTick - tick_ - pendingTicks_;
  if (wholeSeconds > available / frequency_)
    throw std::overflow_error("Simulation tick schedule would overflow");
  const auto wholeTicks = wholeSeconds * frequency_;
  const auto fractionTicks = fractional / nanosPerSecond;
  if (fractionTicks > available - wholeTicks)
    throw std::overflow_error("Simulation tick schedule would overflow");
  const auto pending = pendingTicks_ + wholeTicks + fractionTicks;
  if (pending != 0 && !execute) throw std::invalid_argument("Simulation clock requires a tick callback");

  // Accept elapsed time once. If a callback fails, the unexecuted ticks can
  // be retried with advance(0), including the tick whose callback threw.
  pendingTicks_ = pending;
  fractional_ = static_cast<uint32_t>(fractional % nanosPerSecond);
  uint32_t completed = 0;
  while (pendingTicks_ != 0 && completed < maxTicksPerAdvance_ && !paused_) {
    executeTick(execute);
    --pendingTicks_;
    ++completed;
  }
  return completed;
}

void SimClock::step(const Tick& execute) {
  if (executing_) throw std::logic_error("A clock callback cannot step the clock recursively");
  if (maxTick - tick_ == pendingTicks_)
    throw std::overflow_error("Simulation tick schedule would overflow");
  if (!execute) throw std::invalid_argument("Simulation clock requires a tick callback");
  // Manual stepping advances one tick independently of paused state or debt.
  executeTick(execute);
}

void SimClock::executeTick(const Tick& execute) {
  executing_ = true;
  try {
    execute(tick_, tickSeconds());
  } catch (...) {
    executing_ = false;
    throw;
  }
  executing_ = false;
  ++tick_;
}

void SimClock::reset() {
  if (executing_) throw std::logic_error("A clock callback cannot reset the clock");
  tick_ = 0;
  pendingTicks_ = 0;
  fractional_ = 0;
  paused_ = false;
}

} // namespace container::robotics
