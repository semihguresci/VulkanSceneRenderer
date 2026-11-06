#include "Container/renderer/core/RendererMcpServer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace container::renderer {

RendererMcpMeasurement::RendererMcpMeasurement(uint64_t previousSerial, uint32_t warmup,
    uint32_t samples, Clock::time_point deadline)
    : lastSerial_(previousSerial), warmup_(warmup), target_(samples), deadline_(deadline) {
  if (!samples || samples > 240u || warmup > 240u)
    throw std::invalid_argument("Measurement frame counts are out of range");
  samples_.reserve(samples);
}

void RendererMcpMeasurement::observe(uint64_t serial, std::optional<double> milliseconds,
                                     Clock::time_point now) {
  if (serial <= lastSerial_ || status(now) != Status::Running) return;
  lastSerial_ = serial;
  if (warmup_) {
    --warmup_;
    return;
  }
  if (!milliseconds || !std::isfinite(*milliseconds) || *milliseconds < 0.0) return;
  if (samples_.empty()) firstSampleSerial_ = serial;
  lastSampleSerial_ = serial;
  samples_.push_back(*milliseconds);
}

void RendererMcpMeasurement::cancel(Clock::time_point now) {
  if (status(now) == Status::Running) cancelled_ = true;
}

RendererMcpMeasurement::Status RendererMcpMeasurement::status(Clock::time_point now) const {
  if (cancelled_) return Status::Cancelled;
  if (samples_.size() == target_) return Status::Complete;
  return now >= deadline_ ? Status::TimedOut : Status::Running;
}

nlohmann::json RendererMcpMeasurement::report(Clock::time_point now) const {
  using Json = nlohmann::json;
  const auto state = status(now);
  const auto label = state == Status::Complete ? "complete" : state == Status::Cancelled ? "cancelled" :
      state == Status::TimedOut ? "timedOut" : "running";
  Json result{{"status", label}, {"samples", samples_.size()}, {"requestedSamples", target_},
              {"warmupRemaining", warmup_}};
  if (state != Status::Complete) return result;

  auto sorted = samples_;
  std::ranges::sort(sorted);
  const auto count = sorted.size();
  const double average = std::accumulate(sorted.begin(), sorted.end(), 0.0) / double(count);
  const double squared = std::accumulate(sorted.begin(), sorted.end(), 0.0,
      [average](double sum, double value) { return sum + (value - average) * (value - average); });
  const double median = count % 2 ? sorted[count / 2] : (sorted[count / 2 - 1] + sorted[count / 2]) * .5;
  result["milliseconds"] = {{"mean", average}, {"median", median}, {"min", sorted.front()},
                            {"max", sorted.back()}, {"stddev", std::sqrt(squared / double(count))}};
  result["firstRecordingSerial"] = firstSampleSerial_;
  result["lastRecordingSerial"] = lastSampleSerial_;
  return result;
}

} // namespace container::renderer
