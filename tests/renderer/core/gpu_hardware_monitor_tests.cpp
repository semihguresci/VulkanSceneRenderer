#include "Container/renderer/core/GpuHardwareMonitor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;
using container::renderer::GpuHardwareMonitor;
using container::renderer::GpuHardwareMonitorStatus;
using container::renderer::GpuHardwareSnapshot;

std::optional<GpuHardwareSnapshot> waitForStatus(
    const GpuHardwareMonitor& monitor, GpuHardwareMonitorStatus status) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    auto sample = monitor.trySnapshot();
    if (sample && sample->status == status) return sample;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return std::nullopt;
}

TEST(GpuHardwareMonitor, ABlockedSensorPollDoesNotHoldTheReaderLock) {
  std::promise<std::thread::id> started;
  auto startedFuture = started.get_future();
  std::promise<void> release;
  auto releaseFuture = release.get_future();
  GpuHardwareMonitor monitor([&] {
    started.set_value(std::this_thread::get_id());
    releaseFuture.wait_for(2s);
    return GpuHardwareSnapshot{.status = GpuHardwareMonitorStatus::LibraryUnavailable};
  });

  ASSERT_EQ(startedFuture.wait_for(1s), std::future_status::ready);
  EXPECT_NE(startedFuture.get(), std::this_thread::get_id());
  const auto sample = monitor.trySnapshot();
  release.set_value();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->status, GpuHardwareMonitorStatus::Initializing);
  ASSERT_TRUE(waitForStatus(monitor, GpuHardwareMonitorStatus::LibraryUnavailable));
}

TEST(GpuHardwareMonitor, SuccessfulPollsAreRateLimitedAndKeepUnsupportedValuesEmpty) {
  using Clock = std::chrono::steady_clock;
  std::promise<Clock::time_point> first, second;
  auto firstFuture = first.get_future();
  auto secondFuture = second.get_future();
  GpuHardwareMonitor monitor([&, calls = 0]() mutable {
    ++calls;
    if (calls == 1) first.set_value(Clock::now());
    if (calls == 2) second.set_value(Clock::now());
    return GpuHardwareSnapshot{.status = GpuHardwareMonitorStatus::Available,
                               .utilizationPercent = 0};
  });

  const auto sample = waitForStatus(monitor, GpuHardwareMonitorStatus::Available);
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->utilizationPercent, 0u);
  EXPECT_FALSE(sample->powerMilliwatts);
  EXPECT_GT(sample->timestampUnixMilliseconds, 0);
  EXPECT_GE(sample->pollMicroseconds, 0);
  ASSERT_EQ(firstFuture.wait_for(1s), std::future_status::ready);
  ASSERT_EQ(secondFuture.wait_for(2s), std::future_status::ready);
  EXPECT_GE(secondFuture.get() - firstFuture.get(), GpuHardwareMonitor::pollInterval);
}

} // namespace
