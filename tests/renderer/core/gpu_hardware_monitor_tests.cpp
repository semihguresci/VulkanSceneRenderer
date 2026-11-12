#include "Container/renderer/core/GpuHardwareMonitor.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;
using container::renderer::GpuHardwareMonitor;
using container::renderer::GpuHardwareMonitorStatus;
using container::renderer::GpuHardwareSnapshot;
using container::renderer::gpuHardwareSampleTiming;
using container::renderer::gpuHardwareTelemetry;

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
  EXPECT_TRUE(sample->sampledAt);
  ASSERT_EQ(firstFuture.wait_for(1s), std::future_status::ready);
  ASSERT_EQ(secondFuture.wait_for(2s), std::future_status::ready);
  EXPECT_GE(secondFuture.get() - firstFuture.get(), GpuHardwareMonitor::pollInterval);
}

TEST(GpuHardwareMonitor, LastSampleAgesWhileTheNextPollIsBlocked) {
  std::promise<void> secondPollStarted, releaseSecondPoll;
  auto started = secondPollStarted.get_future();
  auto release = releaseSecondPoll.get_future();
  GpuHardwareMonitor monitor([&, calls = 0]() mutable {
    if (++calls == 1)
      return GpuHardwareSnapshot{.status = GpuHardwareMonitorStatus::Available,
                                 .utilizationPercent = 0};
    if (calls == 2) {
      secondPollStarted.set_value();
      release.wait_for(4s);
    }
    return GpuHardwareSnapshot{.status = GpuHardwareMonitorStatus::SensorsUnavailable};
  });

  const auto first = waitForStatus(monitor, GpuHardwareMonitorStatus::Available);
  ASSERT_TRUE(first);
  ASSERT_TRUE(first->sampledAt);
  ASSERT_EQ(started.wait_for(GpuHardwareMonitor::pollInterval + 1s), std::future_status::ready);
  const auto retained = monitor.trySnapshot();
  releaseSecondPoll.set_value();
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->status, GpuHardwareMonitorStatus::Available);
  EXPECT_EQ(retained->sampledAt, first->sampledAt);
  EXPECT_EQ(retained->utilizationPercent, 0u);

  constexpr auto staleAge = 3 * GpuHardwareMonitor::pollInterval;
  EXPECT_EQ(GpuHardwareMonitor::staleAfter, staleAge);
  const auto deadline = *first->sampledAt + staleAge;
  const auto before = gpuHardwareSampleTiming(*retained, deadline - 1ms);
  ASSERT_TRUE(before);
  EXPECT_EQ(before->age, staleAge - 1ms);
  EXPECT_FALSE(before->stale);
  const auto atDeadline = gpuHardwareSampleTiming(*retained, deadline);
  ASSERT_TRUE(atDeadline);
  EXPECT_EQ(atDeadline->age, staleAge);
  EXPECT_TRUE(atDeadline->stale);

  const auto stale = gpuHardwareTelemetry(retained, deadline);
  EXPECT_EQ(stale.at("status"), "available");
  EXPECT_EQ(stale.at("utilizationPercent"), 0u);
  EXPECT_EQ(stale.at("sampleAgeMilliseconds"), atDeadline->age.count());
  EXPECT_EQ(stale.at("sampleStale"), true);

  const auto resumed = waitForStatus(monitor, GpuHardwareMonitorStatus::SensorsUnavailable);
  ASSERT_TRUE(resumed);
  ASSERT_TRUE(resumed->sampledAt);
  EXPECT_GT(*resumed->sampledAt, *first->sampledAt);
  const auto fresh = gpuHardwareTelemetry(resumed, *resumed->sampledAt);
  EXPECT_EQ(fresh.at("status"), "sensors-unavailable");
  EXPECT_EQ(fresh.at("sampleAgeMilliseconds"), 0);
  EXPECT_EQ(fresh.at("sampleStale"), false);
  EXPECT_TRUE(fresh.at("utilizationPercent").is_null());
  EXPECT_EQ(gpuHardwareTelemetry(resumed, *resumed->sampledAt + GpuHardwareMonitor::staleAfter)
                .at("sampleStale"), true);
}

TEST(GpuHardwareMonitor, CaptureTelemetryUsesMonotonicAgeAndNullUnsupportedReadings) {
  using container::renderer::GpuHardwarePciAddress;
  using Clock = std::chrono::steady_clock;
  const auto now = Clock::time_point{} + 8s;
  const GpuHardwareSnapshot sample{.status = GpuHardwareMonitorStatus::Available,
      .pci = GpuHardwarePciAddress{0, 4, 0, 0}, .utilizationPercent = 0,
      .usedMemoryBytes = 1'048'576, .totalMemoryBytes = 8'388'608,
      .temperatureCelsius = 46, .timestampUnixMilliseconds = 1'794'000'000'000,
      .pollMicroseconds = 123, .sampledAt = now - 125ms};
  const auto json = gpuHardwareTelemetry(sample, now);
  EXPECT_EQ(json.at("backend"), "nvml");
  EXPECT_EQ(json.at("scope"), "device");
  EXPECT_EQ(json.at("status"), "available");
  EXPECT_EQ(json.at("pci").at("bus"), 4u);
  EXPECT_EQ(json.at("utilizationPercent"), 0u);
  EXPECT_EQ(json.at("driverUsedMemoryBytes"), 1'048'576u);
  EXPECT_EQ(json.at("driverTotalMemoryBytes"), 8'388'608u);
  EXPECT_EQ(json.at("sampleUnixMilliseconds"), 1'794'000'000'000);
  EXPECT_EQ(json.at("sampleAgeMilliseconds"), 125);
  EXPECT_EQ(json.at("sampleStale"), false);
  EXPECT_EQ(json.at("pollMicroseconds"), 123);
  EXPECT_TRUE(json.at("graphicsClockMHz").is_null());
  EXPECT_TRUE(json.at("powerMilliwatts").is_null());
  const auto clamped = gpuHardwareTelemetry(sample, now - 1s);
  EXPECT_EQ(clamped.at("sampleAgeMilliseconds"), 0);
  EXPECT_EQ(clamped.at("sampleStale"), false);

  const auto unavailable = gpuHardwareTelemetry(GpuHardwareSnapshot{
      .status = GpuHardwareMonitorStatus::LibraryUnavailable, .sampledAt = now - 8s}, now);
  EXPECT_EQ(unavailable.at("sampleAgeMilliseconds"), 8000);
  EXPECT_EQ(unavailable.at("sampleStale"), false);
  const auto publishing = gpuHardwareTelemetry(std::nullopt, now);
  EXPECT_EQ(publishing.at("status"), "publishing");
  EXPECT_TRUE(publishing.at("sampleStale").is_null());
}

TEST(GpuHardwareMonitor, AnEnabledMonitorWithoutADeviceReportsNoReadings) {
  const GpuHardwareMonitor monitor(VK_NULL_HANDLE, true);
  const auto sample = monitor.trySnapshot();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->status, GpuHardwareMonitorStatus::UnsupportedDevice);
  EXPECT_FALSE(gpuHardwareSampleTiming(*sample));
  const auto unsupported = gpuHardwareTelemetry(sample);
  EXPECT_EQ(unsupported.at("status"), "unsupported-device");
  for (const auto* field : {"pci", "utilizationPercent", "driverUsedMemoryBytes",
      "driverTotalMemoryBytes", "graphicsClockMHz", "memoryClockMHz", "temperatureCelsius",
      "powerMilliwatts", "sampleUnixMilliseconds", "sampleAgeMilliseconds", "sampleStale",
      "pollMicroseconds"}) {
    EXPECT_TRUE(unsupported.at(field).is_null()) << field;
  }
}

} // namespace
