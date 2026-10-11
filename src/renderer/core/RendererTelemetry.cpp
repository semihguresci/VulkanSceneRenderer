#include "Container/renderer/core/RendererTelemetry.h"

#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/core/RenderGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace container::renderer {

namespace {

constexpr size_t phaseIndex(RendererTelemetryPhase phase) {
  return static_cast<size_t>(phase);
}

const RenderPassExecutionStatus* findStatus(
    std::span<const RenderPassExecutionStatus> statuses,
    RenderPassId id) {
  const auto it = std::ranges::find_if(statuses, [id](const auto& status) {
    return status.id == id;
  });
  return it == statuses.end() ? nullptr : &*it;
}

std::string blockerText(const RenderPassExecutionStatus& status) {
  switch (status.skipReason) {
    case RenderPassSkipReason::MissingPassDependency:
      return std::string(renderPassName(status.blockingPass));
    case RenderPassSkipReason::MissingResource:
      return std::string(renderResourceName(status.blockingResource));
    case RenderPassSkipReason::None:
    case RenderPassSkipReason::Disabled:
    case RenderPassSkipReason::MissingRecordCallback:
    case RenderPassSkipReason::NotNeeded:
      break;
  }
  return {};
}

float percentile95(std::vector<float> values) {
  if (values.empty()) return 0.0f;
  std::ranges::sort(values);
  const auto index = static_cast<size_t>(
      std::ceil(static_cast<float>(values.size()) * 0.95f)) - 1u;
  return values[std::min(index, values.size() - 1u)];
}

}  // namespace

RendererTelemetry::RendererTelemetry(size_t historyCapacity)
    : historyCapacity_(std::max<size_t>(historyCapacity, 1u)) {
  history_.reserve(historyCapacity_);
  activePassCpuMs_.assign(kRenderPassIdCount, 0.0f);
  activePassGpuMs_.assign(kRenderPassIdCount, 0.0f);
  activePassCpuRecorded_.assign(kRenderPassIdCount, 0u);
  activePassGpuRecorded_.assign(kRenderPassIdCount, 0u);
}

void RendererTelemetry::beginFrame(uint64_t frameIndex,
                                   uint32_t frameSlot,
                                   uint32_t maxFramesInFlight,
                                   bool serializedConcurrency,
                                   std::string_view concurrencyReason) {
  active_ = {};
  active_.valid = true;
  active_.frameIndex = frameIndex;
  active_.sync.frameSlot = frameSlot;
  active_.sync.maxFramesInFlight = maxFramesInFlight;
  active_.sync.serializedConcurrency = serializedConcurrency;
  active_.sync.concurrencyReason = std::string(concurrencyReason);
  active_.sync.swapchainRecreateCount = swapchainRecreateCount_;
  active_.sync.deviceWaitIdleCount = deviceWaitIdleCount_;
  if (activePassCpuMs_.size() != kRenderPassIdCount) {
    activePassCpuMs_.assign(kRenderPassIdCount, 0.0f);
  } else {
    std::ranges::fill(activePassCpuMs_, 0.0f);
  }
  if (activePassGpuMs_.size() != kRenderPassIdCount) {
    activePassGpuMs_.assign(kRenderPassIdCount, 0.0f);
  } else {
    std::ranges::fill(activePassGpuMs_, 0.0f);
  }
  if (activePassCpuRecorded_.size() != kRenderPassIdCount) {
    activePassCpuRecorded_.assign(kRenderPassIdCount, 0u);
  } else {
    std::ranges::fill(activePassCpuRecorded_, 0u);
  }
  if (activePassGpuRecorded_.size() != kRenderPassIdCount) {
    activePassGpuRecorded_.assign(kRenderPassIdCount, 0u);
  } else {
    std::ranges::fill(activePassGpuRecorded_, 0u);
  }
  activePassGpuTimingAvailable_ = false;
  activeRecording_ = true;
}

void RendererTelemetry::setImageIndex(uint32_t imageIndex) {
  if (activeRecording_) active_.imageIndex = imageIndex;
}

void RendererTelemetry::setCpuPhase(RendererTelemetryPhase phase,
                                    float milliseconds) {
  if (!activeRecording_ || phase == RendererTelemetryPhase::Count) return;
  active_.timing.cpuMs[phaseIndex(phase)] = std::max(milliseconds, 0.0f);
}

void RendererTelemetry::addCpuPhase(RendererTelemetryPhase phase,
                                    float milliseconds) {
  if (!activeRecording_ || phase == RendererTelemetryPhase::Count) return;
  active_.timing.cpuMs[phaseIndex(phase)] += std::max(milliseconds, 0.0f);
}

void RendererTelemetry::setCullingStats(const CullStats& stats) {
  if (!activeRecording_) return;
  active_.culling.inputCount = stats.totalInputCount;
  active_.culling.frustumPassedCount = stats.frustumPassedCount;
  active_.culling.occlusionPassedCount = stats.occlusionPassedCount;
}

void RendererTelemetry::setLightCullingStats(
    const container::gpu::LightCullingStats& stats) {
  if (!activeRecording_) return;
  active_.lightCulling = stats;
  active_.workload.submittedLights = stats.submittedLights;
  if (!activePassGpuTimingAvailable_) {
    std::ranges::fill(activePassGpuMs_, 0.0f);
    activePassGpuMs_[static_cast<size_t>(RenderPassId::TileCull)] =
        std::max(stats.clusterCullMs, 0.0f);
    activePassGpuMs_[static_cast<size_t>(RenderPassId::Lighting)] =
        std::max(stats.clusteredLightingMs, 0.0f);
    if (stats.clusterCullMs > 0.0f) {
      activePassGpuRecorded_[static_cast<size_t>(RenderPassId::TileCull)] = 1u;
    }
    if (stats.clusteredLightingMs > 0.0f) {
      activePassGpuRecorded_[static_cast<size_t>(RenderPassId::Lighting)] = 1u;
    }
    active_.timing.gpuKnownMs =
        activePassGpuMs_[static_cast<size_t>(RenderPassId::TileCull)] +
        activePassGpuMs_[static_cast<size_t>(RenderPassId::Lighting)];
    if (active_.timing.gpuKnownMs > 0.0f) {
      active_.timing.gpuSource = RendererGpuTimingSource::LightCullingStats;
    }
  }
}

void RendererTelemetry::recordPassCpuTime(RenderPassId id,
                                          float milliseconds) {
  if (!activeRecording_) return;
  const auto index = static_cast<size_t>(id);
  if (index >= activePassCpuMs_.size()) return;
  activePassCpuMs_[index] += std::max(milliseconds, 0.0f);
  if (index < activePassCpuRecorded_.size()) {
    activePassCpuRecorded_[index] = 1u;
  }
}

void RendererTelemetry::setPassGpuTimings(
    std::span<const RendererPassGpuTiming> timings,
    RendererGpuTimingSource source) {
  if (!activeRecording_) return;
  if (activePassGpuMs_.size() != kRenderPassIdCount) {
    activePassGpuMs_.assign(kRenderPassIdCount, 0.0f);
  } else {
    std::ranges::fill(activePassGpuMs_, 0.0f);
  }
  if (activePassGpuRecorded_.size() != kRenderPassIdCount) {
    activePassGpuRecorded_.assign(kRenderPassIdCount, 0u);
  } else {
    std::ranges::fill(activePassGpuRecorded_, 0u);
  }

  float totalGpuMs = 0.0f;
  bool recordedAnyGpuTiming = false;
  for (const auto& timing : timings) {
    const auto index = static_cast<size_t>(timing.id);
    if (index >= activePassGpuMs_.size()) continue;
    const float milliseconds = std::max(timing.milliseconds, 0.0f);
    activePassGpuMs_[index] += milliseconds;
    activePassGpuRecorded_[index] = 1u;
    recordedAnyGpuTiming = true;
    totalGpuMs += milliseconds;
  }

  activePassGpuTimingAvailable_ = recordedAnyGpuTiming;
  active_.timing.gpuSource =
      activePassGpuTimingAvailable_ ? source : RendererGpuTimingSource::None;
  if (activePassGpuTimingAvailable_) {
    active_.timing.gpuKnownMs = totalGpuMs;
  }
}

void RendererTelemetry::setGpuProfilerStatus(
    RendererGpuProfilerTelemetry status) {
  if (!activeRecording_) return;
  active_.gpuProfiler = std::move(status);
}

void RendererTelemetry::setWorkload(RendererWorkloadTelemetry workload) {
  if (!activeRecording_) return;
  const uint32_t submittedLights = active_.workload.submittedLights;
  active_.workload = workload;
  if (active_.workload.submittedLights == 0u) {
    active_.workload.submittedLights = submittedLights;
  }
}

void RendererTelemetry::setResources(RendererResourceTelemetry resources) {
  if (activeRecording_) active_.resources = resources;
}

void RendererTelemetry::setRenderGraph(const RenderGraph& graph) {
  if (!activeRecording_) return;

  active_.passes.clear();
  active_.passes.reserve(graph.passes().size());
  active_.graph = {};
  active_.graph.totalPasses = graph.passCount();
  active_.graph.enabledPasses = graph.enabledPassCount();

  const auto statuses = graph.lastFrameExecutionStatuses();
  for (const auto& node : graph.passes()) {
    RendererPassTelemetry pass{};
    pass.name = node.name;
    pass.enabled = node.enabled;
    const auto passIndex = static_cast<size_t>(node.id);
    if (passIndex < activePassCpuMs_.size()) {
      pass.cpuRecordMs = activePassCpuMs_[passIndex];
    }
    if (passIndex < activePassGpuMs_.size()) {
      pass.gpuKnownMs = activePassGpuMs_[passIndex];
    }
    pass.cpuTimed = passIndex < activePassCpuRecorded_.size() &&
                    activePassCpuRecorded_[passIndex] != 0u;
    pass.gpuTimed = passIndex < activePassGpuRecorded_.size() &&
                    activePassGpuRecorded_[passIndex] != 0u;

    if (const auto* status = findStatus(statuses, node.id)) {
      pass.active = status->active;
      pass.status = status->active
                        ? "Active"
                        : std::string(renderPassSkipReasonName(
                              status->skipReason));
      pass.blocker = blockerText(*status);
    } else {
      pass.status = node.enabled ? "Unknown" : "Disabled";
    }

    if (pass.active) {
      ++active_.graph.activePasses;
    } else {
      ++active_.graph.skippedPasses;
    }
    if (pass.cpuTimed) {
      ++active_.graph.cpuTimedPasses;
    }
    if (pass.gpuTimed) {
      ++active_.graph.gpuTimedPasses;
    }

    active_.passes.push_back(std::move(pass));
  }
}

void RendererTelemetry::noteSwapchainRecreate() {
  ++swapchainRecreateCount_;
  if (activeRecording_) {
    active_.sync.swapchainRecreateCount = swapchainRecreateCount_;
  }
}

void RendererTelemetry::noteDeviceWaitIdle() {
  ++deviceWaitIdleCount_;
  if (activeRecording_) {
    active_.sync.deviceWaitIdleCount = deviceWaitIdleCount_;
  }
}

void RendererTelemetry::endFrame() {
  if (!activeRecording_) return;

  latest_ = active_;
  if (history_.size() == historyCapacity_) {
    history_.erase(history_.begin());
  }
  history_.push_back(RendererTelemetryHistorySample{
      .frameIndex = latest_.frameIndex,
      .cpuFrameMs =
          latest_.timing.cpuMs[phaseIndex(RendererTelemetryPhase::Frame)],
      .gpuKnownMs = latest_.timing.gpuKnownMs,
      .waitForFrameMs =
          latest_.timing.cpuMs[phaseIndex(RendererTelemetryPhase::WaitForFrame)],
      .presentMs =
          latest_.timing.cpuMs[phaseIndex(RendererTelemetryPhase::Present)],
      .cpuPhasesMs = latest_.timing.cpuMs,
  });
  activeRecording_ = false;
}

RendererTelemetryView RendererTelemetry::view() const {
  return RendererTelemetryView{
      .latest = latest_,
      .history = history_,
      .summary = buildSummary(),
  };
}

RendererTelemetrySummary RendererTelemetry::buildSummary() const {
  RendererTelemetrySummary summary{};
  summary.frameCount = static_cast<uint32_t>(history_.size());
  if (history_.empty()) return summary;

  std::vector<float> cpuFrames;
  cpuFrames.reserve(history_.size());

  float totalCpu = 0.0f;
  float totalGpu = 0.0f;
  for (const auto& sample : history_) {
    cpuFrames.push_back(sample.cpuFrameMs);
    totalCpu += sample.cpuFrameMs;
    totalGpu += sample.gpuKnownMs;
    summary.maxCpuFrameMs = std::max(summary.maxCpuFrameMs, sample.cpuFrameMs);
    summary.maxGpuKnownMs = std::max(summary.maxGpuKnownMs, sample.gpuKnownMs);
  }

  const float frameCount = static_cast<float>(history_.size());
  summary.averageCpuFrameMs = totalCpu / frameCount;
  summary.averageGpuKnownMs = totalGpu / frameCount;
  summary.p95CpuFrameMs = percentile95(std::move(cpuFrames));
  return summary;
}

std::string_view rendererTelemetryPhaseName(RendererTelemetryPhase phase) {
  static constexpr std::array<std::string_view,
                              kRendererTelemetryPhaseCount>
      kNames{{
          "Frame",
          "Frame wait",
          "Readbacks",
          "Acquire image",
          "Image fence",
          "Resource growth",
          "GUI",
          "Scene update",
          "Descriptors",
          "Command record",
          "Queue submit",
          "Screenshot",
          "Present",
      }};

  if (phase == RendererTelemetryPhase::Count) return "Unknown";
  return kNames[phaseIndex(phase)];
}

std::string_view rendererGpuTimingSourceName(RendererGpuTimingSource source) {
  switch (source) {
    case RendererGpuTimingSource::None:
      return "none";
    case RendererGpuTimingSource::TimestampQuery:
      return "timestamp queries";
    case RendererGpuTimingSource::PerformanceQuery:
      return "VK_KHR_performance_query";
    case RendererGpuTimingSource::LightCullingStats:
      return "light culling stats";
  }
  return "unknown";
}

}  // namespace container::renderer
