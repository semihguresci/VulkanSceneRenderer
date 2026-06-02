#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace container::ui {

struct RenderPassToggle {
  std::string name;
  bool enabled{true};
  bool locked{false};
  bool autoDisabled{false};
  std::string dependencyNote{};
};

struct GuiRenderTechniqueDisplayMode {
  std::string id{};
  std::string label{};
  uint32_t value{0};
};

struct GuiRenderTechniqueDebugControl {
  std::string id{};
  std::string label{};
  std::string kind{};
};

struct GuiRenderTechniqueDebugPanel {
  std::string id{};
  std::string title{};
  std::vector<GuiRenderTechniqueDebugControl> controls{};
};

struct GuiRenderTechniqueDebugState {
  std::string techniqueName{};
  std::string displayName{};
  std::vector<GuiRenderTechniqueDisplayMode> displayModes{};
  std::vector<GuiRenderTechniqueDebugPanel> panels{};
};

enum class GuiRendererTelemetryPhase : uint8_t {
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

inline constexpr size_t kGuiRendererTelemetryPhaseCount =
    static_cast<size_t>(GuiRendererTelemetryPhase::Count);

struct GuiRendererFrameTiming {
  std::array<float, kGuiRendererTelemetryPhaseCount> cpuMs{};
  float gpuKnownMs{0.0f};
  std::string gpuSource{};
};

struct GuiRendererCullingTelemetry {
  uint32_t inputCount{0};
  uint32_t frustumPassedCount{0};
  uint32_t occlusionPassedCount{0};
};

struct GuiRendererLightCullingTelemetry {
  uint32_t submittedLights{0};
  uint32_t activeClusters{0};
  uint32_t totalClusters{0};
  uint32_t maxLightsPerCluster{0};
  uint32_t droppedLightReferences{0};
  float clusterCullMs{0.0f};
  float clusteredLightingMs{0.0f};
};

struct GuiRendererWorkloadTelemetry {
  uint32_t objectCount{0};
  uint32_t opaqueDrawCount{0};
  uint32_t transparentDrawCount{0};
  uint32_t totalDrawCount{0};
  uint32_t submittedLights{0};
};

struct GuiRendererResourceTelemetry {
  uint32_t swapchainWidth{0};
  uint32_t swapchainHeight{0};
  uint32_t swapchainImageCount{0};
  uint32_t cameraBufferCount{0};
  uint32_t objectBufferCapacity{0};
  uint32_t oitNodeCapacity{0};
};

struct GuiRendererSyncTelemetry {
  uint32_t frameSlot{0};
  uint32_t maxFramesInFlight{0};
  bool serializedConcurrency{false};
  std::string concurrencyReason{};
  uint64_t swapchainRecreateCount{0};
  uint64_t deviceWaitIdleCount{0};
};

struct GuiRenderGraphTelemetry {
  uint32_t totalPasses{0};
  uint32_t enabledPasses{0};
  uint32_t activePasses{0};
  uint32_t skippedPasses{0};
  uint32_t cpuTimedPasses{0};
  uint32_t gpuTimedPasses{0};
};

struct GuiRendererGpuProfilerTelemetry {
  std::string source{};
  bool available{false};
  uint32_t resultLatencyFrames{0};
  std::string status{};
};

struct GuiRendererPassTelemetry {
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

struct GuiRendererTelemetrySnapshot {
  bool valid{false};
  uint64_t frameIndex{0};
  uint32_t imageIndex{0};
  GuiRendererFrameTiming timing{};
  GuiRendererCullingTelemetry culling{};
  GuiRendererLightCullingTelemetry lightCulling{};
  GuiRendererWorkloadTelemetry workload{};
  GuiRendererResourceTelemetry resources{};
  GuiRendererSyncTelemetry sync{};
  GuiRenderGraphTelemetry graph{};
  GuiRendererGpuProfilerTelemetry gpuProfiler{};
  std::vector<GuiRendererPassTelemetry> passes{};
};

struct GuiRendererTelemetryHistorySample {
  uint64_t frameIndex{0};
  float cpuFrameMs{0.0f};
  float gpuKnownMs{0.0f};
  float waitForFrameMs{0.0f};
  float presentMs{0.0f};
};

struct GuiRendererTelemetrySummary {
  uint32_t frameCount{0};
  float averageCpuFrameMs{0.0f};
  float p95CpuFrameMs{0.0f};
  float maxCpuFrameMs{0.0f};
  float averageGpuKnownMs{0.0f};
  float maxGpuKnownMs{0.0f};
};

struct GuiRendererTelemetryView {
  GuiRendererTelemetrySnapshot latest{};
  std::vector<GuiRendererTelemetryHistorySample> history{};
  GuiRendererTelemetrySummary summary{};
};

} // namespace container::ui
