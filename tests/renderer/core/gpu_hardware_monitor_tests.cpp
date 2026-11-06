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
  EXPECT_EQ(json.at("pollMicroseconds"), 123);
  EXPECT_TRUE(json.at("graphicsClockMHz").is_null());
  EXPECT_TRUE(json.at("powerMilliwatts").is_null());
  EXPECT_EQ(gpuHardwareTelemetry(sample, now - 1s).at("sampleAgeMilliseconds"), 0);

  const auto unsupported = gpuHardwareTelemetry(GpuHardwareSnapshot{
      .status = GpuHardwareMonitorStatus::UnsupportedDevice}, now);
  EXPECT_EQ(unsupported.at("status"), "unsupported-device");
  EXPECT_TRUE(unsupported.at("pci").is_null());
  EXPECT_TRUE(unsupported.at("sampleAgeMilliseconds").is_null());
  EXPECT_TRUE(unsupported.at("pollMicroseconds").is_null());
  EXPECT_EQ(gpuHardwareTelemetry(std::nullopt, now).at("status"), "publishing");
}

} // namespace
