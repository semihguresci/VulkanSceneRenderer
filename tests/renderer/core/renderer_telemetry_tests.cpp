#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/core/RenderGraph.h"

#include <gtest/gtest.h>

#include <array>

using container::renderer::RendererTelemetry;
using container::renderer::RendererTelemetryPhase;
using container::renderer::RendererGpuTimingSource;
using container::renderer::RenderGraph;
using container::renderer::RenderPassId;

namespace {

void recordFrame(RendererTelemetry& telemetry,
                 uint64_t frameIndex,
                 float cpuFrameMs,
                 float gpuKnownMs = 0.0f) {
  telemetry.beginFrame(frameIndex, 0u, 2u, false, "per-frame resources");
  telemetry.setCpuPhase(RendererTelemetryPhase::Frame, cpuFrameMs);
  telemetry.setCpuPhase(RendererTelemetryPhase::Present, cpuFrameMs * 0.1f);
  container::gpu::LightCullingStats lightStats{};
  lightStats.clusterCullMs = gpuKnownMs * 0.4f;
  lightStats.clusteredLightingMs = gpuKnownMs * 0.6f;
  telemetry.setLightCullingStats(lightStats);
  telemetry.endFrame();
}

}  // namespace

TEST(RendererTelemetryTests, KeepsLatestCompletedFrame) {
  RendererTelemetry telemetry{8};

  recordFrame(telemetry, 42u, 12.5f, 1.25f);

  const auto view = telemetry.view();
  ASSERT_TRUE(view.latest.valid);
  EXPECT_EQ(view.latest.frameIndex, 42u);
  EXPECT_FLOAT_EQ(
      view.latest.timing.cpuMs[static_cast<size_t>(
          RendererTelemetryPhase::Frame)],
      12.5f);
  EXPECT_FLOAT_EQ(view.latest.timing.gpuKnownMs, 1.25f);
  EXPECT_EQ(view.latest.timing.gpuSource,
            RendererGpuTimingSource::LightCullingStats);
  ASSERT_EQ(view.history.size(), 1u);
  EXPECT_EQ(view.history.front().frameIndex, 42u);
}

TEST(RendererTelemetryTests, LimitsHistoryToCapacity) {
  RendererTelemetry telemetry{2};

  recordFrame(telemetry, 1u, 10.0f);
  recordFrame(telemetry, 2u, 20.0f);
  recordFrame(telemetry, 3u, 30.0f);

  const auto view = telemetry.view();
  ASSERT_EQ(view.history.size(), 2u);
  EXPECT_EQ(view.history[0].frameIndex, 2u);
  EXPECT_EQ(view.history[1].frameIndex, 3u);
}

TEST(RendererTelemetryTests, KeepsIndependentPhaseBreakdownsForCompletedFrames) {
  RendererTelemetry telemetry{2};
  telemetry.beginFrame(1u, 0u, 2u, true, "shared resources");
  telemetry.setCpuPhase(RendererTelemetryPhase::Frame, 300.0f);
  telemetry.setCpuPhase(RendererTelemetryPhase::WaitForFrame, 280.0f);
  telemetry.setCpuPhase(RendererTelemetryPhase::Gui, 12.0f);
  telemetry.endFrame();

  telemetry.beginFrame(2u, 1u, 2u, true, "shared resources");
  telemetry.setCpuPhase(RendererTelemetryPhase::Frame, 80.0f);
  telemetry.setCpuPhase(RendererTelemetryPhase::Screenshot, 60.0f);
  telemetry.setCpuPhase(RendererTelemetryPhase::Gui, 1.0f);
  telemetry.endFrame();

  const auto history = telemetry.view().history;
  ASSERT_EQ(history.size(), 2u);
  const auto phase = [](RendererTelemetryPhase value) { return static_cast<size_t>(value); };
  EXPECT_FLOAT_EQ(history[0].cpuPhasesMs[phase(RendererTelemetryPhase::WaitForFrame)], 280.0f);
  EXPECT_FLOAT_EQ(history[0].cpuPhasesMs[phase(RendererTelemetryPhase::Gui)], 12.0f);
  EXPECT_FLOAT_EQ(history[0].cpuPhasesMs[phase(RendererTelemetryPhase::Screenshot)], 0.0f);
  EXPECT_FLOAT_EQ(history[1].cpuPhasesMs[phase(RendererTelemetryPhase::WaitForFrame)], 0.0f);
  EXPECT_FLOAT_EQ(history[1].cpuPhasesMs[phase(RendererTelemetryPhase::Gui)], 1.0f);
  EXPECT_FLOAT_EQ(history[1].cpuPhasesMs[phase(RendererTelemetryPhase::Screenshot)], 60.0f);
}

TEST(RendererTelemetryTests, BuildsSummaryFromHistory) {
  RendererTelemetry telemetry{8};

  recordFrame(telemetry, 1u, 10.0f, 1.0f);
  recordFrame(telemetry, 2u, 20.0f, 2.0f);
  recordFrame(telemetry, 3u, 30.0f, 3.0f);

  const auto summary = telemetry.view().summary;
  EXPECT_EQ(summary.frameCount, 3u);
  EXPECT_FLOAT_EQ(summary.averageCpuFrameMs, 20.0f);
  EXPECT_FLOAT_EQ(summary.maxCpuFrameMs, 30.0f);
  EXPECT_FLOAT_EQ(summary.p95CpuFrameMs, 30.0f);
  EXPECT_FLOAT_EQ(summary.averageGpuKnownMs, 2.0f);
  EXPECT_FLOAT_EQ(summary.maxGpuKnownMs, 3.0f);
}

TEST(RendererTelemetryTests, AttachesPassCpuAndKnownGpuTimings) {
  RendererTelemetry telemetry{8};
  RenderGraph graph;
  graph.addPass(RenderPassId::TileCull, {}, [](VkCommandBuffer,
                                               const auto&) {});
  graph.addPass(RenderPassId::Lighting, {}, [](VkCommandBuffer,
                                               const auto&) {});
  graph.setPassResourceAccess(RenderPassId::TileCull, {}, {}, {});
  graph.setPassResourceAccess(RenderPassId::Lighting, {}, {}, {});

  telemetry.beginFrame(1u, 0u, 2u, false, "per-frame resources");
  telemetry.recordPassCpuTime(RenderPassId::TileCull, 0.25f);
  telemetry.recordPassCpuTime(RenderPassId::Lighting, 0.5f);

  container::gpu::LightCullingStats lightStats{};
  lightStats.clusterCullMs = 0.75f;
  lightStats.clusteredLightingMs = 1.25f;
  telemetry.setLightCullingStats(lightStats);
  telemetry.setRenderGraph(graph);
  telemetry.endFrame();

  const auto view = telemetry.view();
  ASSERT_EQ(view.latest.passes.size(), 2u);
  EXPECT_EQ(view.latest.graph.cpuTimedPasses, 2u);
  EXPECT_EQ(view.latest.graph.gpuTimedPasses, 2u);
  EXPECT_EQ(view.latest.passes[0].name, "TileCull");
  EXPECT_TRUE(view.latest.passes[0].cpuTimed);
  EXPECT_TRUE(view.latest.passes[0].gpuTimed);
  EXPECT_FLOAT_EQ(view.latest.passes[0].cpuRecordMs, 0.25f);
  EXPECT_FLOAT_EQ(view.latest.passes[0].gpuKnownMs, 0.75f);
  EXPECT_EQ(view.latest.passes[1].name, "Lighting");
  EXPECT_TRUE(view.latest.passes[1].cpuTimed);
  EXPECT_TRUE(view.latest.passes[1].gpuTimed);
  EXPECT_FLOAT_EQ(view.latest.passes[1].cpuRecordMs, 0.5f);
  EXPECT_FLOAT_EQ(view.latest.passes[1].gpuKnownMs, 1.25f);
}

TEST(RendererTelemetryTests, PassGpuTimingsOverrideFallbackLightingTimers) {
  RendererTelemetry telemetry{8};
  RenderGraph graph;
  graph.addPass(RenderPassId::TileCull, {}, [](VkCommandBuffer,
                                               const auto&) {});
  graph.addPass(RenderPassId::Lighting, {}, [](VkCommandBuffer,
                                               const auto&) {});
  graph.setPassResourceAccess(RenderPassId::TileCull, {}, {}, {});
  graph.setPassResourceAccess(RenderPassId::Lighting, {}, {}, {});

  telemetry.beginFrame(1u, 0u, 2u, false, "per-frame resources");
  const std::array<container::renderer::RendererPassGpuTiming, 2> gpuTimings{{
      {.id = RenderPassId::TileCull, .milliseconds = 2.0f},
      {.id = RenderPassId::Lighting, .milliseconds = 3.0f},
  }};
  telemetry.setPassGpuTimings(gpuTimings,
                              RendererGpuTimingSource::PerformanceQuery);

  container::gpu::LightCullingStats lightStats{};
  lightStats.clusterCullMs = 0.25f;
  lightStats.clusteredLightingMs = 0.5f;
  telemetry.setLightCullingStats(lightStats);
  telemetry.setRenderGraph(graph);
  telemetry.endFrame();

  const auto view = telemetry.view();
  ASSERT_EQ(view.latest.passes.size(), 2u);
  EXPECT_FLOAT_EQ(view.latest.timing.gpuKnownMs, 5.0f);
  EXPECT_EQ(view.latest.timing.gpuSource,
            RendererGpuTimingSource::PerformanceQuery);
  EXPECT_FLOAT_EQ(view.latest.passes[0].gpuKnownMs, 2.0f);
  EXPECT_FLOAT_EQ(view.latest.passes[1].gpuKnownMs, 3.0f);
}

TEST(RendererTelemetryTests, CountsZeroMillisecondGpuTimingsAsAvailable) {
  RendererTelemetry telemetry{8};
  RenderGraph graph;
  graph.addPass(RenderPassId::TileCull, {}, [](VkCommandBuffer,
                                               const auto&) {});
  graph.setPassResourceAccess(RenderPassId::TileCull, {}, {}, {});

  telemetry.beginFrame(1u, 0u, 2u, false, "per-frame resources");
  const std::array<container::renderer::RendererPassGpuTiming, 1> gpuTimings{{
      {.id = RenderPassId::TileCull, .milliseconds = 0.0f},
  }};
  telemetry.setPassGpuTimings(gpuTimings,
                              RendererGpuTimingSource::TimestampQuery);
  telemetry.setRenderGraph(graph);
  telemetry.endFrame();

  const auto view = telemetry.view();
  ASSERT_EQ(view.latest.passes.size(), 1u);
  EXPECT_EQ(view.latest.graph.gpuTimedPasses, 1u);
  EXPECT_TRUE(view.latest.passes[0].gpuTimed);
  EXPECT_FLOAT_EQ(view.latest.passes[0].gpuKnownMs, 0.0f);
}

TEST(RendererTelemetryTests, StoresGpuProfilerStatus) {
  RendererTelemetry telemetry{8};

  telemetry.beginFrame(7u, 1u, 2u, true, "serialized resources");
  telemetry.setGpuProfilerStatus(container::renderer::RendererGpuProfilerTelemetry{
      .source = RendererGpuTimingSource::TimestampQuery,
      .available = true,
      .resultLatencyFrames = 1u,
      .status =
          "using timestamp queries; performance queries unavailable",
  });
  telemetry.endFrame();

  const auto view = telemetry.view();
  EXPECT_EQ(view.latest.gpuProfiler.source,
            RendererGpuTimingSource::TimestampQuery);
  EXPECT_TRUE(view.latest.gpuProfiler.available);
  EXPECT_EQ(view.latest.gpuProfiler.resultLatencyFrames, 1u);
  EXPECT_EQ(view.latest.gpuProfiler.status,
            "using timestamp queries; performance queries unavailable");
}
