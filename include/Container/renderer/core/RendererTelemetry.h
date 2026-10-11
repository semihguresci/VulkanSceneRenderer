#pragma once

#include "Container/utility/SceneData.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

class RenderGraph;
enum class RenderPassId : uint8_t;
struct CullStats;

enum class RendererTelemetryPhase : uint8_t {
  Frame,
  WaitForFrame,
  Readbacks,
  AcquireImage,
  ImageFenceWait,
  ResourceGrowth,
  Gui,
  SceneUpdate,
  DescriptorUpdate,
  CommandRecord,
  QueueSubmit,
  Screenshot,
  Present,
  Count,
};

inline constexpr size_t kRendererTelemetryPhaseCount =
    static_cast<size_t>(RendererTelemetryPhase::Count);

enum class RendererGpuTimingSource : uint8_t {
  None,
  TimestampQuery,
  PerformanceQuery,
  LightCullingStats,
};

struct RendererFrameTiming {
  std::array<float, kRendererTelemetryPhaseCount> cpuMs{};
  float gpuKnownMs{0.0f};
  RendererGpuTimingSource gpuSource{RendererGpuTimingSource::None};
};

struct RendererCullingTelemetry {
  uint32_t inputCount{0};
  uint32_t frustumPassedCount{0};
  uint32_t occlusionPassedCount{0};
};

struct RendererWorkloadTelemetry {
  uint32_t objectCount{0};
  uint32_t opaqueDrawCount{0};
  uint32_t transparentDrawCount{0};
  uint32_t totalDrawCount{0};
  uint32_t submittedLights{0};
};

struct RendererResourceTelemetry {
  uint32_t swapchainWidth{0};
  uint32_t swapchainHeight{0};
  uint32_t swapchainImageCount{0};
  uint32_t cameraBufferCount{0};
  uint32_t objectBufferCapacity{0};
  uint32_t oitNodeCapacity{0};
};

struct RendererSyncTelemetry {
  uint32_t frameSlot{0};
  uint32_t maxFramesInFlight{0};
  bool serializedConcurrency{false};
  std::string concurrencyReason{};
  uint64_t swapchainRecreateCount{0};
  uint64_t deviceWaitIdleCount{0};
};

struct RendererGraphTelemetry {
  uint32_t totalPasses{0};
  uint32_t enabledPasses{0};
  uint32_t activePasses{0};
  uint32_t skippedPasses{0};
  uint32_t cpuTimedPasses{0};
  uint32_t gpuTimedPasses{0};
};

struct RendererGpuProfilerTelemetry {
  RendererGpuTimingSource source{RendererGpuTimingSource::None};
  bool available{false};
  uint32_t resultLatencyFrames{0};
  std::string status{};
};

struct RendererPassTelemetry {
  std::string name{};
  bool enabled{false};
  bool active{false};
  bool cpuTimed{false};
  bool gpuTimed{false};
  float cpuRecordMs{0.0f};
  float gpuKnownMs{0.0f};
  std::string status{};
  std::string blocker{};
};

struct RendererPassGpuTiming {
  RenderPassId id{};
  float milliseconds{0.0f};
};

struct RendererTelemetrySnapshot {
  bool valid{false};
  uint64_t frameIndex{0};
  uint32_t imageIndex{0};
  RendererFrameTiming timing{};
  RendererCullingTelemetry culling{};
  container::gpu::LightCullingStats lightCulling{};
  RendererWorkloadTelemetry workload{};
  RendererResourceTelemetry resources{};
  RendererSyncTelemetry sync{};
  RendererGraphTelemetry graph{};
  RendererGpuProfilerTelemetry gpuProfiler{};
  std::vector<RendererPassTelemetry> passes{};
};

struct RendererTelemetryHistorySample {
  uint64_t frameIndex{0};
  float cpuFrameMs{0.0f};
  float gpuKnownMs{0.0f};
  float waitForFrameMs{0.0f};
  float presentMs{0.0f};
  // Retain the phase breakdown so diagnostics can distinguish CPU work,
  // synchronization and capture overhead across completed frames.
  std::array<float, kRendererTelemetryPhaseCount> cpuPhasesMs{};
};

struct RendererTelemetrySummary {
  uint32_t frameCount{0};
  float averageCpuFrameMs{0.0f};
  float p95CpuFrameMs{0.0f};
  float maxCpuFrameMs{0.0f};
  float averageGpuKnownMs{0.0f};
  float maxGpuKnownMs{0.0f};
};

struct RendererTelemetryView {
  RendererTelemetrySnapshot latest{};
  std::vector<RendererTelemetryHistorySample> history{};
  RendererTelemetrySummary summary{};
};

class RendererTelemetry {
 public:
  explicit RendererTelemetry(size_t historyCapacity = 240);

  void beginFrame(uint64_t frameIndex,
                  uint32_t frameSlot,
                  uint32_t maxFramesInFlight,
                  bool serializedConcurrency,
                  std::string_view concurrencyReason);
  void setImageIndex(uint32_t imageIndex);
  void setCpuPhase(RendererTelemetryPhase phase, float milliseconds);
  void addCpuPhase(RendererTelemetryPhase phase, float milliseconds);
  void setCullingStats(const CullStats& stats);
  void setLightCullingStats(
      const container::gpu::LightCullingStats& stats);
  void recordPassCpuTime(RenderPassId id, float milliseconds);
  void setPassGpuTimings(std::span<const RendererPassGpuTiming> timings,
                         RendererGpuTimingSource source);
  void setGpuProfilerStatus(RendererGpuProfilerTelemetry status);
  void setWorkload(RendererWorkloadTelemetry workload);
  void setResources(RendererResourceTelemetry resources);
  void setRenderGraph(const RenderGraph& graph);
  void noteSwapchainRecreate();
  void noteDeviceWaitIdle();
  void endFrame();

  [[nodiscard]] RendererTelemetryView view() const;
  [[nodiscard]] const RendererTelemetrySnapshot& latest() const {
    return latest_;
  }

 private:
  [[nodiscard]] RendererTelemetrySummary buildSummary() const;

  size_t historyCapacity_{240};
  uint64_t swapchainRecreateCount_{0};
  uint64_t deviceWaitIdleCount_{0};
  bool activeRecording_{false};
  RendererTelemetrySnapshot active_{};
  RendererTelemetrySnapshot latest_{};
  std::vector<RendererTelemetryHistorySample> history_;
  std::vector<float> activePassCpuMs_;
  std::vector<float> activePassGpuMs_;
  std::vector<uint8_t> activePassCpuRecorded_;
  std::vector<uint8_t> activePassGpuRecorded_;
  bool activePassGpuTimingAvailable_{false};
};

[[nodiscard]] std::string_view rendererTelemetryPhaseName(
    RendererTelemetryPhase phase);
[[nodiscard]] std::string_view rendererGpuTimingSourceName(
    RendererGpuTimingSource source);

}  // namespace container::renderer
