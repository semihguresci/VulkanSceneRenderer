#pragma once

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace container::renderer {

enum class GpuHardwareMonitorStatus {
  Disabled,
  Initializing,
  UnsupportedDevice,
  PciIdentityUnavailable,
  LibraryUnavailable,
  InitializationFailed,
  DeviceUnavailable,
  SensorsUnavailable,
  Available,
  WorkerUnavailable,
};

struct GpuHardwarePciAddress {
  uint32_t domain{}, bus{}, device{}, function{};
};

struct GpuHardwareSnapshot {
  GpuHardwareMonitorStatus status{GpuHardwareMonitorStatus::Disabled};
  std::optional<GpuHardwarePciAddress> pci;
  std::optional<uint32_t> utilizationPercent;
  std::optional<uint64_t> usedMemoryBytes, totalMemoryBytes;
  std::optional<uint32_t> graphicsClockMHz, memoryClockMHz;
  std::optional<uint32_t> temperatureCelsius, powerMilliwatts;
  int64_t timestampUnixMilliseconds{};
  int64_t pollMicroseconds{};
};

// All NVML operations run on one worker. Frame readers never wait for it.
class GpuHardwareMonitor {
 public:
  using PollFunction = std::move_only_function<GpuHardwareSnapshot()>;
  static constexpr std::chrono::seconds pollInterval{1};

  explicit GpuHardwareMonitor(VkPhysicalDevice device);
  explicit GpuHardwareMonitor(PollFunction poll);

  [[nodiscard]] std::optional<GpuHardwareSnapshot> trySnapshot() const;

 private:
  void start(PollFunction poll);

  mutable std::mutex snapshotMutex_;
  GpuHardwareSnapshot snapshot_;
  std::jthread worker_;
};

[[nodiscard]] std::string_view gpuHardwareMonitorStatusText(GpuHardwareMonitorStatus status);

} // namespace container::renderer
