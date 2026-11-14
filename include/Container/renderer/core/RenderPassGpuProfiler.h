#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/core/RenderGraph.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

class RenderPassGpuProfiler {
 public:
  struct ReferenceBatch {
    uint64_t generation;
    uint64_t epoch;
    uint32_t samples;
  };

  struct RetiredFrame {
    uint64_t frameIndex;
    uint64_t serial;
    std::optional<ReferenceBatch> referenceBatch;
    std::vector<RendererPassGpuTiming> timings;
    std::optional<float> graphicsMilliseconds;
  };

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
  // A CPU frame slot's submitted queries must retire before its next recording.
  void beginFrame(VkCommandBuffer cmd, uint32_t frameSlot);
  void endFrame(VkCommandBuffer cmd);
  void beginPass(VkCommandBuffer cmd, RenderPassId id);
  void endPass(VkCommandBuffer cmd, RenderPassId id);
  void onSubmitted(VkFence fence, uint64_t frameIndex,
                   std::optional<ReferenceBatch> referenceBatch) noexcept;
  // Loading records no timing queries but can reuse a scene frame's fence.
  // Call this after the slot's fence completes and before that fence is reset.
  void markFenceComplete(uint32_t frameSlot);
  // The returned samples remain valid until the next collection or shutdown.
  [[nodiscard]] std::span<const RetiredFrame> collectRetired();
  [[nodiscard]] uint32_t pendingFrames() const;

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

  struct Submission {
    VkFence fence;
    uint64_t frameIndex;
    uint64_t serial;
    std::optional<ReferenceBatch> referenceBatch;
  };

  struct Slot {
    std::array<uint8_t, kRenderPassIdCount> writtenPasses{};
    std::optional<Submission> submitted;
    bool graphicsTimestampsWritten{false};
  };

  using Timings = std::optional<std::vector<RendererPassGpuTiming>>;

  [[nodiscard]] uint32_t queryBase(uint32_t frameSlot) const;
  [[nodiscard]] uint32_t performanceQuery(uint32_t frameSlot,
                                          RenderPassId id) const;
  [[nodiscard]] uint32_t startQuery(uint32_t frameSlot, RenderPassId id) const;
  [[nodiscard]] uint32_t endQuery(uint32_t frameSlot, RenderPassId id) const;
  [[nodiscard]] bool isValidPassId(RenderPassId id) const;
  [[nodiscard]] bool initializePerformanceQueryBackend();
  [[nodiscard]] bool initializeTimestampBackend();
  [[nodiscard]] bool initializeTimestampProperties();
  [[nodiscard]] std::optional<float> collectGraphicsDuration(uint32_t frameSlot) const;
  [[nodiscard]] float timestampMilliseconds(uint64_t start, uint64_t end) const;
  [[nodiscard]] Timings collectPerformanceTimings(uint32_t frameSlot) const;
  [[nodiscard]] Timings collectTimestampTimings(uint32_t frameSlot) const;
  [[nodiscard]] float performanceCounterToMilliseconds(
      const VkPerformanceCounterResultKHR& value) const;

  VkInstance instance_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkQueryPool queryPool_{VK_NULL_HANDLE};
  VkQueryPool graphicsQueryPool_{VK_NULL_HANDLE};
  uint32_t queueFamilyIndex_{0};
  uint32_t frameSlots_{0};
  float timestampPeriodNs_{1.0f};
  uint64_t timestampMask_{0};
  Backend backend_{Backend::None};
  std::string backendStatus_{"not initialized"};

  std::vector<Slot> slots_;
  std::vector<uint32_t> submittedSlots_;
  std::vector<RetiredFrame> retired_;
  std::optional<uint32_t> activeFrameSlot_;
  uint64_t recordingSerial_{0};

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
