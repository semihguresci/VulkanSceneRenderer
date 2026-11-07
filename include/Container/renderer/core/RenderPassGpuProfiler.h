#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/core/RenderGraph.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

class RenderPassGpuProfiler {
 public:
  RenderPassGpuProfiler() = default;
  ~RenderPassGpuProfiler();

  RenderPassGpuProfiler(const RenderPassGpuProfiler&) = delete;
  RenderPassGpuProfiler& operator=(const RenderPassGpuProfiler&) = delete;

  void initialize(VkDevice device,
                  VkInstance instance,
                  VkPhysicalDevice physicalDevice,
                  uint32_t queueFamilyIndex,
                  uint32_t frameSlots);
  void shutdown();
  void recreate(uint32_t frameSlots);

  [[nodiscard]] bool isReady() const {
    return backend_ != Backend::None && queryPool_ != VK_NULL_HANDLE;
  }
  [[nodiscard]] bool usesPerformanceQueries() const {
    return backend_ == Backend::PerformanceQuery;
  }
  [[nodiscard]] RendererGpuTimingSource timingSource() const {
    if (backend_ == Backend::PerformanceQuery) {
      return RendererGpuTimingSource::PerformanceQuery;
    }
    if (backend_ == Backend::Timestamp) {
      return RendererGpuTimingSource::TimestampQuery;
    }
    return RendererGpuTimingSource::None;
  }
  [[nodiscard]] std::string_view backendStatus() const {
    return backendStatus_;
  }
  [[nodiscard]] uint32_t resultLatencyFrames() const {
    return latestFrameHasQueries_ ? 1u : 0u;
  }
  [[nodiscard]] uint64_t latestRecordingSerial() const {
    return latestFrameHasQueries_ ? latestRecordingSerial_ : 0u;
  }

  void beginFrame(VkCommandBuffer cmd, uint32_t frameSlot);
  void beginPass(VkCommandBuffer cmd, RenderPassId id);
  void endPass(VkCommandBuffer cmd, RenderPassId id);

  [[nodiscard]] std::vector<RendererPassGpuTiming> collectLatest() const;

 private:
  static constexpr uint32_t kQueriesPerPass = 2u;
  static constexpr uint32_t kQueriesPerFrame =
      static_cast<uint32_t>(kRenderPassIdCount) * kQueriesPerPass;
  static constexpr uint32_t kPerformanceQueriesPerFrame =
      static_cast<uint32_t>(kRenderPassIdCount);

  enum class Backend {
    None,
    Timestamp,
    PerformanceQuery,
  };

  [[nodiscard]] uint32_t queryBase(uint32_t frameSlot) const;
  [[nodiscard]] uint32_t performanceQuery(uint32_t frameSlot,
                                          RenderPassId id) const;
  [[nodiscard]] uint32_t startQuery(uint32_t frameSlot, RenderPassId id) const;
  [[nodiscard]] uint32_t endQuery(uint32_t frameSlot, RenderPassId id) const;
  [[nodiscard]] bool isValidPassId(RenderPassId id) const;
  [[nodiscard]] bool initializePerformanceQueryBackend();
  [[nodiscard]] bool initializeTimestampBackend();
  [[nodiscard]] std::vector<RendererPassGpuTiming> collectPerformanceTimings()
      const;
  [[nodiscard]] std::vector<RendererPassGpuTiming> collectTimestampTimings()
      const;
  [[nodiscard]] float performanceCounterToMilliseconds(
      const VkPerformanceCounterResultKHR& value) const;

  VkInstance instance_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkQueryPool queryPool_{VK_NULL_HANDLE};
  uint32_t queueFamilyIndex_{0};
  uint32_t frameSlots_{0};
  float timestampPeriodNs_{1.0f};
  Backend backend_{Backend::None};
  std::string backendStatus_{"not initialized"};

  uint32_t activeFrameSlot_{0};
  bool activeFrameRecording_{false};
  uint64_t recordingSerial_{0};
  uint64_t latestRecordingSerial_{0};
  mutable uint32_t latestFrameSlot_{0};
  mutable bool latestFrameHasQueries_{false};
  std::array<uint8_t, kRenderPassIdCount> activeWrittenPasses_{};
  mutable std::array<uint8_t, kRenderPassIdCount> latestWrittenPasses_{};

  PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR
      enumeratePerformanceCounters_{nullptr};
  PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR
      getPerformanceQueryPasses_{nullptr};
  PFN_vkAcquireProfilingLockKHR acquireProfilingLock_{nullptr};
  PFN_vkReleaseProfilingLockKHR releaseProfilingLock_{nullptr};
  bool profilingLockAcquired_{false};
  uint32_t performanceCounterIndex_{0};
  VkPerformanceCounterStorageKHR performanceCounterStorage_{
      VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR};
};

}  // namespace container::renderer
