#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderGraph.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using container::renderer::FrameRecordParams;
using container::renderer::RenderGraph;
using container::renderer::RenderPassExecutionHooks;
using container::renderer::RenderPassId;
using container::renderer::RenderPassReadiness;
using container::renderer::RenderPassSkipReason;
using container::renderer::RenderPassNode;
using container::renderer::RenderResourceId;

RenderPassNode::RecordFn recordPass(
    std::vector<RenderPassId>& recorded,
    RenderPassId id) {
  return [&recorded, id](VkCommandBuffer, const FrameRecordParams&) {
    recorded.push_back(id);
  };
}

RenderPassNode::RecordFn noopRecord() {
  return [](VkCommandBuffer, const FrameRecordParams&) {};
}

RenderPassReadiness notNeeded() {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::NotNeeded;
  return readiness;
}

std::vector<RenderPassId> execute(RenderGraph& graph) {
  std::vector<RenderPassId> recorded;
  for (const auto& pass : graph.passes()) {
    const RenderPassId id = pass.id;
    graph.setPassRecord(id, recordPass(recorded, id));
  }

  FrameRecordParams params{};
  graph.execute(VK_NULL_HANDLE, params);
  return recorded;
}

size_t executionPosition(const RenderGraph& graph, RenderPassId id) {
  const auto order = graph.executionPassIds();
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == id) return i;
  }
  return order.size();
}

bool activeContains(const RenderGraph& graph, RenderPassId id) {
  const auto order = graph.activeExecutionPassIds();
  return std::ranges::find(order, id) != order.end();
}

bool resourceSpanContains(std::span<const RenderResourceId> resources,
                          RenderResourceId id) {
  return std::ranges::find(resources, id) != resources.end();
}

bool passSpanContains(std::span<const RenderPassId> passes, RenderPassId id) {
  return std::ranges::find(passes, id) != passes.end();
}

const container::renderer::RenderPassExecutionStatus* statusFor(
    std::span<const container::renderer::RenderPassExecutionStatus> statuses,
    RenderPassId id) {
  const auto it = std::ranges::find_if(statuses, [&](const auto& status) {
    return status.id == id;
  });
  return it == statuses.end() ? nullptr : &*it;
}

const container::renderer::RenderPassExecutionStatus* statusFor(
    const RenderGraph& graph,
    RenderPassId id) {
  return statusFor(graph.executionStatuses(), id);
}

}  // namespace

TEST(RenderGraphTests, CompileOrdersPassesByScheduleDependency) {
  RenderGraph graph;
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());

  graph.compile();

  const auto order = graph.executionPassIds();
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], RenderPassId::FrustumCull);
  EXPECT_EQ(order[1], RenderPassId::DepthPrepass);
}

TEST(RenderGraphTests, AddPassRejectsInvalidPassId) {
  RenderGraph graph;

  EXPECT_THROW(graph.addPass(RenderPassId::Invalid, {}, noopRecord()),
               std::runtime_error);
  EXPECT_EQ(graph.passCount(), 0u);
}

TEST(RenderGraphTests, AddPassRejectsInvalidScheduleDependency) {
  RenderGraph graph;

  EXPECT_THROW(
      graph.addPass(
          RenderPassId::DepthPrepass,
          {RenderPassId::Invalid},
          noopRecord()),
      std::runtime_error);
  EXPECT_EQ(graph.passCount(), 0u);
}

TEST(RenderGraphTests, AddPassRejectsDuplicateScheduleDependency) {
  RenderGraph graph;

  EXPECT_THROW(
      graph.addPass(
          RenderPassId::DepthPrepass,
          {RenderPassId::FrustumCull, RenderPassId::FrustumCull},
          noopRecord()),
      std::runtime_error);
  EXPECT_EQ(graph.passCount(), 0u);
}

TEST(RenderGraphTests, FindPassUsesStablePassId) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());

  const RenderPassNode* pass = graph.findPass(RenderPassId::FrustumCull);
  ASSERT_NE(pass, nullptr);
  EXPECT_EQ(pass->id, RenderPassId::FrustumCull);
  EXPECT_EQ(graph.findPass(RenderPassId::PostProcess), nullptr);
}

TEST(RenderGraphTests, ExecutionPassIdsAreClearedWithGraph) {
  RenderGraph graph;
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());

  graph.compile();
  ASSERT_EQ(graph.executionPassIds().size(), 2u);

  graph.clear();

  EXPECT_TRUE(graph.executionPassIds().empty());
  EXPECT_TRUE(graph.activeExecutionPassIds().empty());
}

TEST(RenderGraphTests, ActiveExecutionPassIdsRefreshAfterToggleChange) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());
  graph.addPass(RenderPassId::HiZGenerate,
                {RenderPassId::DepthPrepass},
                noopRecord());

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::HiZGenerate, false));
  EXPECT_FALSE(activeContains(graph, RenderPassId::HiZGenerate));

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::HiZGenerate, true));
  EXPECT_TRUE(activeContains(graph, RenderPassId::HiZGenerate));
}

TEST(RenderGraphTests, DebugModelReportsPassStatusWithoutUiCoupling) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  graph.addPass(RenderPassId::HiZGenerate, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::HiZGenerate, {}, {},
                                          {}));
  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::HiZGenerate, false));

  const container::renderer::RenderGraphDebugModel debugModel =
      graph.debugModel();

  ASSERT_EQ(debugModel.passes.size(), 2u);
  EXPECT_EQ(debugModel.passes[0].passName, "FrustumCull");
  EXPECT_TRUE(debugModel.passes[0].enabled);
  EXPECT_TRUE(debugModel.passes[0].active);
  EXPECT_EQ(debugModel.passes[0].skipReason, "active");
  EXPECT_EQ(debugModel.passes[1].passName, "HiZGenerate");
  EXPECT_FALSE(debugModel.passes[1].enabled);
  EXPECT_FALSE(debugModel.passes[1].active);
  EXPECT_EQ(debugModel.passes[1].skipReason, "disabled");
}

TEST(RenderGraphTests, ExecuteSkipsPassWhenRequiredDependencyIsDisabled) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());
  graph.addPass(RenderPassId::HiZGenerate,
                {RenderPassId::DepthPrepass},
                noopRecord());
  graph.addPass(RenderPassId::OcclusionCull,
                {RenderPassId::FrustumCull, RenderPassId::HiZGenerate},
                noopRecord());

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::HiZGenerate, false));

  const std::vector<RenderPassId> recorded = execute(graph);

  const std::vector<RenderPassId> expected = {
      RenderPassId::FrustumCull,
      RenderPassId::DepthPrepass,
  };
  EXPECT_EQ(recorded, expected);
}

TEST(RenderGraphTests, DisabledScheduleOnlyDependencyDoesNotSkipProtectedPass) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::FrustumCull, false));

  const std::vector<RenderPassId> recorded = execute(graph);

  const std::vector<RenderPassId> expected = {
      RenderPassId::DepthPrepass,
  };
  EXPECT_EQ(recorded, expected);
}

TEST(RenderGraphTests, CompileThrowsForMissingScheduleDependency) {
  RenderGraph graph;
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());

  EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(RenderGraphTests, CompileThrowsForDependencyCycle) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull,
                {RenderPassId::DepthPrepass},
                noopRecord());
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());

  EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(RenderGraphTests, CompileBuildsResourceEdgesFromPriorWriters) {
  RenderGraph graph;
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  graph.addPass(RenderPassId::HiZGenerate,
                {RenderPassId::DepthPrepass},
                noopRecord());

  graph.compile();

  const auto edges = graph.resourceEdges();
  const auto depthToHiz =
      std::ranges::find_if(edges, [](const auto& edge) {
        return edge.resource == RenderResourceId::SceneDepth &&
               edge.writer == RenderPassId::DepthPrepass &&
               edge.reader == RenderPassId::HiZGenerate;
      });

  EXPECT_NE(depthToHiz, edges.end());
}

TEST(RenderGraphTests, ActiveExecutionSkipsRequiredResourceConsumerWhenWriterDisabled) {
  RenderGraph graph;
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  graph.addPass(RenderPassId::HiZGenerate,
                {RenderPassId::DepthPrepass},
                noopRecord());

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::DepthPrepass, false));

  EXPECT_FALSE(activeContains(graph, RenderPassId::DepthPrepass));
  EXPECT_FALSE(activeContains(graph, RenderPassId::HiZGenerate));

  const auto* status = statusFor(graph, RenderPassId::HiZGenerate);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(status->skipReason, RenderPassSkipReason::MissingPassDependency);
  EXPECT_EQ(status->blockingPass, RenderPassId::DepthPrepass);
}

TEST(RenderGraphTests, ActiveExecutionKeepsConsumerWithOnlyDisabledOptionalRead) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::FrustumCull, false));

  EXPECT_FALSE(activeContains(graph, RenderPassId::FrustumCull));
  EXPECT_TRUE(activeContains(graph, RenderPassId::DepthPrepass));

  const auto* status = statusFor(graph, RenderPassId::DepthPrepass);
  ASSERT_NE(status, nullptr);
  EXPECT_TRUE(status->active);
  EXPECT_EQ(status->skipReason, RenderPassSkipReason::None);
}

TEST(RenderGraphTests, ExecutionStatusReportsMissingRequiredResource) {
  RenderGraph graph;
  graph.addPass(RenderPassId::Bloom, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::Bloom,
      {},
      {},
      {RenderResourceId::BloomTexture}));

  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::FrustumCull,
      {RenderResourceId::BloomTexture},
      {},
      {}));

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::Bloom, false));

  const auto* status = statusFor(graph, RenderPassId::FrustumCull);
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->active);
  EXPECT_EQ(status->skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(status->blockingResource, RenderResourceId::BloomTexture);
}

TEST(RenderGraphTests, SetPassResourceAccessRejectsInvalidResources) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::FrustumCull,
      {},
      {},
      {RenderResourceId::FrustumCullDraws}));

  EXPECT_THROW(
      graph.setPassResourceAccess(
          RenderPassId::FrustumCull,
          {RenderResourceId::Invalid},
          {},
          {}),
      std::runtime_error);
  EXPECT_THROW(
      graph.setPassResourceAccess(
          RenderPassId::FrustumCull,
          {},
          {RenderResourceId::Invalid},
          {}),
      std::runtime_error);
  EXPECT_THROW(
      graph.setPassResourceAccess(
          RenderPassId::FrustumCull,
          {},
          {},
          {RenderResourceId::Invalid}),
      std::runtime_error);
  EXPECT_THROW(
      graph.setPassResourceAccess(
          RenderPassId::FrustumCull,
          {RenderResourceId::BloomTexture, RenderResourceId::BloomTexture},
          {},
          {}),
      std::runtime_error);
  EXPECT_THROW(
      graph.setPassResourceAccess(
          RenderPassId::FrustumCull,
          {},
          {RenderResourceId::BloomTexture, RenderResourceId::BloomTexture},
          {}),
      std::runtime_error);
  EXPECT_THROW(
      graph.setPassResourceAccess(
          RenderPassId::FrustumCull,
          {},
          {},
          {RenderResourceId::FrustumCullDraws,
           RenderResourceId::FrustumCullDraws}),
      std::runtime_error);
  EXPECT_THROW(
      graph.setPassResourceAccess(
          RenderPassId::FrustumCull,
          {RenderResourceId::BloomTexture},
          {RenderResourceId::BloomTexture},
          {}),
      std::runtime_error);

  const auto* pass = graph.findPass(RenderPassId::FrustumCull);
  ASSERT_NE(pass, nullptr);
  EXPECT_TRUE(pass->reads.empty());
  EXPECT_TRUE(pass->optionalReads.empty());
  ASSERT_EQ(pass->writes.size(), 1u);
  EXPECT_EQ(pass->writes.front(), RenderResourceId::FrustumCullDraws);
}

TEST(RenderGraphTests, AddPassCopiesDefaultTransitionMetadata) {
  RenderGraph graph;
  graph.addPass(RenderPassId::DepthToReadOnly, {}, noopRecord());

  const RenderPassNode* pass = graph.findPass(RenderPassId::DepthToReadOnly);
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(pass->transitions.size(), 1u);
  EXPECT_EQ(pass->transitions.front().resource, RenderResourceId::SceneDepth);
  EXPECT_EQ(pass->transitions.front().before,
            container::renderer::RenderResourceState::DepthStencilAttachment);
  EXPECT_EQ(pass->transitions.front().after,
            container::renderer::RenderResourceState::DepthStencilReadOnly);
}

TEST(RenderGraphTests, SetPassResourceTransitionsRejectsInvalidResource) {
  RenderGraph graph;
  graph.addPass(RenderPassId::Bloom, {}, noopRecord());

  EXPECT_THROW(
      graph.setPassResourceTransitions(
          RenderPassId::Bloom,
          {{RenderResourceId::Invalid,
            container::renderer::RenderResourceState::ColorAttachment,
            container::renderer::RenderResourceState::ShaderRead}}),
      std::runtime_error);

  ASSERT_TRUE(graph.setPassResourceTransitions(
      RenderPassId::Bloom,
      {{RenderResourceId::SceneColor,
        container::renderer::RenderResourceState::ColorAttachment,
        container::renderer::RenderResourceState::ShaderRead}}));

  const RenderPassNode* pass = graph.findPass(RenderPassId::Bloom);
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(pass->transitions.size(), 1u);
  EXPECT_EQ(pass->transitions.front().resource, RenderResourceId::SceneColor);
}

TEST(RenderGraphTests, ActivePredicateFollowsExecutionStatus) {
  RenderGraph graph;
  graph.addPass(RenderPassId::Bloom, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::Bloom,
      {},
      {},
      {RenderResourceId::BloomTexture}));

  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::FrustumCull,
      {RenderResourceId::BloomTexture},
      {},
      {}));

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::Bloom, false));

  const auto* status = graph.executionStatus(RenderPassId::FrustumCull);
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->active);
  EXPECT_FALSE(graph.isPassActive(RenderPassId::FrustumCull));

  ASSERT_TRUE(graph.setPassEnabled(RenderPassId::Bloom, true));

  status = graph.executionStatus(RenderPassId::FrustumCull);
  ASSERT_NE(status, nullptr);
  EXPECT_TRUE(status->active);
  EXPECT_TRUE(graph.isPassActive(RenderPassId::FrustumCull));
  EXPECT_FALSE(graph.setPassEnabled(RenderPassId::PostProcess, false));
  EXPECT_EQ(graph.executionStatus(RenderPassId::PostProcess), nullptr);
}

TEST(RenderGraphTests, SetPassRecordUpdatesActivePlan) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, {});

  const auto* status = graph.executionStatus(RenderPassId::FrustumCull);
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->active);
  EXPECT_EQ(status->skipReason, RenderPassSkipReason::MissingRecordCallback);

  ASSERT_TRUE(graph.setPassRecord(RenderPassId::FrustumCull, noopRecord()));

  status = graph.executionStatus(RenderPassId::FrustumCull);
  ASSERT_NE(status, nullptr);
  EXPECT_TRUE(status->active);
  EXPECT_TRUE(graph.isPassActive(RenderPassId::FrustumCull));
}

TEST(RenderGraphTests, ExecuteSkipsPassWhenReadinessFailsAtRuntime) {
  RenderGraph graph;
  std::vector<RenderPassId> recorded;
  std::vector<RenderPassId> began;

  graph.addPass(RenderPassId::FrustumCull,
                {},
                recordPass(recorded, RenderPassId::FrustumCull));
  ASSERT_TRUE(graph.setPassReadiness(RenderPassId::FrustumCull,
                                     [](const FrameRecordParams&) {
                                       return notNeeded();
                                     }));

  RenderPassExecutionHooks hooks{};
  hooks.beginPass = [&](RenderPassId id, VkCommandBuffer) {
    began.push_back(id);
  };
  FrameRecordParams params{};
  graph.execute(VK_NULL_HANDLE, params, hooks);

  EXPECT_TRUE(recorded.empty());
  EXPECT_TRUE(began.empty());

  const auto* status = statusFor(graph.lastFrameExecutionStatuses(),
                                 RenderPassId::FrustumCull);
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->active);
  EXPECT_EQ(status->skipReason, RenderPassSkipReason::NotNeeded);
  EXPECT_TRUE(graph.isPassActive(RenderPassId::FrustumCull));
}

TEST(RenderGraphTests, PrepareFramePublishesRuntimeStatusBeforeExecute) {
  RenderGraph graph;

  graph.addPass(RenderPassId::TileCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::TileCull, {}, {}, {}));
  ASSERT_TRUE(graph.setPassReadiness(RenderPassId::TileCull,
                                     [](const FrameRecordParams&) {
                                       return notNeeded();
                                     }));

  FrameRecordParams params{};
  graph.prepareFrame(params);

  const auto* status = statusFor(graph.lastFrameExecutionStatuses(),
                                 RenderPassId::TileCull);
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->active);
  EXPECT_EQ(status->skipReason, RenderPassSkipReason::NotNeeded);

  // The static plan remains available for UI/topology queries; prepared-frame
  // status is exposed through lastFrameExecutionStatuses().
  EXPECT_TRUE(graph.isPassActive(RenderPassId::TileCull));
}

TEST(RenderGraphTests, PrepareFrameIsIdempotentForSameFrameSignature) {
  RenderGraph graph;
  int readinessCalls = 0;

  graph.addPass(RenderPassId::TileCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::TileCull, {}, {}, {}));
  ASSERT_TRUE(graph.setPassReadiness(RenderPassId::TileCull,
                                     [&](const FrameRecordParams&) {
                                       ++readinessCalls;
                                       return RenderPassReadiness{};
                                     }));

  FrameRecordParams params{};
  params.runtime.frameSlot = 7u;
  params.runtime.imageIndex = 2u;
  graph.prepareFrame(params);
  graph.prepareFrame(params);

  EXPECT_EQ(readinessCalls, 1);

  params.runtime.imageIndex = 1u;
  graph.prepareFrame(params);

  EXPECT_EQ(readinessCalls, 2);
}

TEST(RenderGraphTests, PrepareFrameRebuildsWhenExposureModeChanges) {
  RenderGraph graph;
  int readinessCalls = 0;

  graph.addPass(RenderPassId::ExposureAdaptation, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::ExposureAdaptation, {},
                                          {}, {}));
  ASSERT_TRUE(graph.setPassReadiness(
      RenderPassId::ExposureAdaptation, [&](const FrameRecordParams& params) {
        ++readinessCalls;
        return params.postProcess.exposureSettings.mode ==
                       container::gpu::kExposureModeAuto
                   ? RenderPassReadiness{}
                   : notNeeded();
      }));

  FrameRecordParams params{};
  params.postProcess.exposureSettings.mode =
      container::gpu::kExposureModeManual;
  graph.prepareFrame(params);

  params.postProcess.exposureSettings.mode =
      container::gpu::kExposureModeAuto;
  graph.prepareFrame(params);

  EXPECT_EQ(readinessCalls, 2);
  const auto* exposureStatus = statusFor(graph.lastFrameExecutionStatuses(),
                                         RenderPassId::ExposureAdaptation);
  ASSERT_NE(exposureStatus, nullptr);
  EXPECT_TRUE(exposureStatus->active);
}

TEST(RenderGraphTests, ExecutePreparedFrameConsumesPreparedPlan) {
  RenderGraph graph;
  std::vector<RenderPassId> recorded;
  int readinessCalls = 0;
  bool ready = false;

  graph.addPass(RenderPassId::TileCull,
                {},
                recordPass(recorded, RenderPassId::TileCull));
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::TileCull, {}, {}, {}));
  ASSERT_TRUE(graph.setPassReadiness(RenderPassId::TileCull,
                                     [&](const FrameRecordParams&) {
                                       ++readinessCalls;
                                       return ready ? RenderPassReadiness{}
                                                    : notNeeded();
                                     }));

  FrameRecordParams params{};
  graph.prepareFrame(params);
  ready = true;
  graph.executePreparedFrame(VK_NULL_HANDLE, params);

  EXPECT_TRUE(recorded.empty());
  EXPECT_EQ(readinessCalls, 1);
}

TEST(RenderGraphTests, ExecutePreparedFrameRebuildsAfterReadinessMutation) {
  RenderGraph graph;
  std::vector<RenderPassId> recorded;
  int readinessCalls = 0;

  graph.addPass(RenderPassId::TileCull,
                {},
                recordPass(recorded, RenderPassId::TileCull));
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::TileCull, {}, {}, {}));
  ASSERT_TRUE(graph.setPassReadiness(RenderPassId::TileCull,
                                     [&](const FrameRecordParams&) {
                                       ++readinessCalls;
                                       return notNeeded();
                                     }));

  FrameRecordParams params{};
  graph.prepareFrame(params);
  ASSERT_TRUE(graph.setPassReadiness(RenderPassId::TileCull,
                                     [&](const FrameRecordParams&) {
                                       ++readinessCalls;
                                       return RenderPassReadiness{};
                                     }));

  graph.executePreparedFrame(VK_NULL_HANDLE, params);

  const std::vector<RenderPassId> expected = {RenderPassId::TileCull};
  EXPECT_EQ(recorded, expected);
  EXPECT_EQ(readinessCalls, 2);
}

TEST(RenderGraphTests, RuntimeSkippedWriterSkipsRequiredResourceReader) {
  RenderGraph graph;
  std::vector<RenderPassId> recorded;

  graph.addPass(RenderPassId::Bloom, {}, recordPass(recorded, RenderPassId::Bloom));
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::Bloom,
      {},
      {},
      {RenderResourceId::BloomTexture}));
  ASSERT_TRUE(graph.setPassReadiness(RenderPassId::Bloom,
                                     [](const FrameRecordParams&) {
                                       return notNeeded();
                                     }));

  graph.addPass(RenderPassId::FrustumCull,
                {},
                recordPass(recorded, RenderPassId::FrustumCull));
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::FrustumCull,
      {RenderResourceId::BloomTexture},
      {},
      {}));

  FrameRecordParams params{};
  graph.execute(VK_NULL_HANDLE, params);

  EXPECT_TRUE(recorded.empty());

  const auto* readerStatus = statusFor(graph.lastFrameExecutionStatuses(),
                                       RenderPassId::FrustumCull);
  ASSERT_NE(readerStatus, nullptr);
  EXPECT_FALSE(readerStatus->active);
  EXPECT_EQ(readerStatus->skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(readerStatus->blockingResource, RenderResourceId::BloomTexture);
}

TEST(RenderGraphTests, IsPassActiveUsesRuntimeStateInsideCallbacks) {
  RenderGraph graph;
  std::vector<RenderPassId> recorded;

  graph.addPass(RenderPassId::TileCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::TileCull, {}, {}, {}));
  ASSERT_TRUE(graph.setPassReadiness(RenderPassId::TileCull,
                                     [](const FrameRecordParams&) {
                                       return notNeeded();
                                     }));

  graph.addPass(RenderPassId::Lighting,
                {RenderPassId::TileCull},
                [&](VkCommandBuffer, const FrameRecordParams&) {
                  EXPECT_FALSE(graph.isPassActive(RenderPassId::TileCull));
                  recorded.push_back(RenderPassId::Lighting);
                });
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::Lighting, {}, {}, {}));

  FrameRecordParams params{};
  graph.execute(VK_NULL_HANDLE, params);

  const std::vector<RenderPassId> expected = {RenderPassId::Lighting};
  EXPECT_EQ(recorded, expected);
  EXPECT_TRUE(graph.isPassActive(RenderPassId::TileCull));
}

TEST(RenderGraphTests, ExecuteAllowsActiveQueriesInsidePassCallbacks) {
  RenderGraph graph;
  std::vector<RenderPassId> recorded;

  graph.addPass(RenderPassId::FrustumCull, {}, [&](VkCommandBuffer,
                                                   const FrameRecordParams&) {
    EXPECT_TRUE(graph.isPassActive(RenderPassId::DepthPrepass));
    recorded.push_back(RenderPassId::FrustumCull);
  });
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                recordPass(recorded, RenderPassId::DepthPrepass));

  FrameRecordParams params{};
  graph.execute(VK_NULL_HANDLE, params);

  const std::vector<RenderPassId> expected = {
      RenderPassId::FrustumCull,
      RenderPassId::DepthPrepass,
  };
  EXPECT_EQ(recorded, expected);
}

TEST(RenderGraphTests, CompileThrowsWhenRequiredInternalReadHasNoWriter) {
  RenderGraph graph;
  graph.addPass(RenderPassId::HiZGenerate, {}, noopRecord());

  EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(RenderGraphTests, OptionalInternalReadDoesNotRequireWriter) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::FrustumCull,
      {},
      {RenderResourceId::BloomTexture},
      {}));

  EXPECT_NO_THROW(graph.compile());
}

TEST(RenderGraphTests, ResourceEdgeCanScheduleWriterBeforeReader) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::FrustumCull,
      {RenderResourceId::BloomTexture},
      {},
      {}));

  graph.addPass(RenderPassId::Bloom, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::Bloom,
      {},
      {},
      {RenderResourceId::BloomTexture}));

  graph.compile();

  EXPECT_LT(executionPosition(graph, RenderPassId::Bloom),
            executionPosition(graph, RenderPassId::FrustumCull));
}

TEST(RenderGraphTests, ResourceAccessMutationInvalidatesCompiledSchedule) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::FrustumCull, {}, {}, {}));

  graph.addPass(RenderPassId::Bloom, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::Bloom,
      {},
      {},
      {RenderResourceId::BloomTexture}));

  graph.compile();
  EXPECT_LT(executionPosition(graph, RenderPassId::FrustumCull),
            executionPosition(graph, RenderPassId::Bloom));

  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::FrustumCull,
      {RenderResourceId::BloomTexture},
      {},
      {}));

  EXPECT_LT(executionPosition(graph, RenderPassId::Bloom),
            executionPosition(graph, RenderPassId::FrustumCull));
}

TEST(RenderGraphTests, ScheduleDependencyMutationInvalidatesCompiledSchedule) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::FrustumCull, {}, {}, {}));

  graph.addPass(RenderPassId::DepthPrepass, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(RenderPassId::DepthPrepass, {}, {}, {}));

  graph.compile();
  EXPECT_LT(executionPosition(graph, RenderPassId::FrustumCull),
            executionPosition(graph, RenderPassId::DepthPrepass));

  ASSERT_TRUE(graph.setPassScheduleDependencies(
      RenderPassId::FrustumCull,
      {RenderPassId::DepthPrepass}));

  EXPECT_LT(executionPosition(graph, RenderPassId::DepthPrepass),
            executionPosition(graph, RenderPassId::FrustumCull));
}

TEST(RenderGraphTests, ScheduleDependencyMutationRejectsInvalidDependency) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  graph.addPass(RenderPassId::DepthPrepass,
                {RenderPassId::FrustumCull},
                noopRecord());

  EXPECT_THROW(
      graph.setPassScheduleDependencies(
          RenderPassId::DepthPrepass,
          {RenderPassId::Invalid}),
      std::runtime_error);
  EXPECT_THROW(
      graph.setPassScheduleDependencies(
          RenderPassId::DepthPrepass,
          {RenderPassId::FrustumCull, RenderPassId::FrustumCull}),
      std::runtime_error);

  const auto* pass = graph.findPass(RenderPassId::DepthPrepass);
  ASSERT_NE(pass, nullptr);
  ASSERT_EQ(pass->scheduleDependencies.size(), 1u);
  EXPECT_EQ(pass->scheduleDependencies.front(), RenderPassId::FrustumCull);
}

TEST(RenderGraphTests, CompileThrowsForResourceDependencyCycle) {
  RenderGraph graph;
  graph.addPass(RenderPassId::FrustumCull, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::FrustumCull,
      {RenderResourceId::BloomTexture},
      {},
      {RenderResourceId::SceneColor}));

  graph.addPass(RenderPassId::Bloom, {}, noopRecord());
  ASSERT_TRUE(graph.setPassResourceAccess(
      RenderPassId::Bloom,
      {RenderResourceId::SceneColor},
      {},
      {RenderResourceId::BloomTexture}));

  EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(RenderGraphTests, DefaultScheduleModelsCurrentFrameFlow) {
  constexpr std::array passes = {
      RenderPassId::FrustumCull,
      RenderPassId::DepthPrepass,
      RenderPassId::HiZGenerate,
      RenderPassId::OcclusionCull,
      RenderPassId::CullStatsReadback,
      RenderPassId::GBuffer,
      RenderPassId::TransparentPick,
      RenderPassId::OitClear,
      RenderPassId::ShadowCullCascade0,
      RenderPassId::ShadowCullCascade1,
      RenderPassId::ShadowCullCascade2,
      RenderPassId::ShadowCullCascade3,
      RenderPassId::ShadowCascade0,
      RenderPassId::ShadowCascade1,
      RenderPassId::ShadowCascade2,
      RenderPassId::ShadowCascade3,
      RenderPassId::LocalShadowDepth,
      RenderPassId::DepthToReadOnly,
      RenderPassId::TileCull,
      RenderPassId::GTAO,
      RenderPassId::Lighting,
      RenderPassId::TransformGizmos,
      RenderPassId::ExposureAdaptation,
      RenderPassId::OitResolve,
      RenderPassId::Bloom,
      RenderPassId::PostProcess,
  };

  RenderGraph graph;
  for (auto it = passes.rbegin(); it != passes.rend(); ++it) {
    graph.addPass(*it, noopRecord());
  }

  graph.compile();

  EXPECT_LT(executionPosition(graph, RenderPassId::FrustumCull),
            executionPosition(graph, RenderPassId::DepthPrepass));
  EXPECT_LT(executionPosition(graph, RenderPassId::DepthPrepass),
            executionPosition(graph, RenderPassId::HiZGenerate));
  EXPECT_LT(executionPosition(graph, RenderPassId::HiZGenerate),
            executionPosition(graph, RenderPassId::OcclusionCull));
  EXPECT_LT(executionPosition(graph, RenderPassId::OcclusionCull),
            executionPosition(graph, RenderPassId::GBuffer));
  EXPECT_LT(executionPosition(graph, RenderPassId::GBuffer),
            executionPosition(graph, RenderPassId::TransparentPick));
  EXPECT_LT(executionPosition(graph, RenderPassId::TransparentPick),
            executionPosition(graph, RenderPassId::DepthToReadOnly));
  EXPECT_LT(executionPosition(graph, RenderPassId::LocalShadowDepth),
            executionPosition(graph, RenderPassId::DepthToReadOnly));
  EXPECT_LT(executionPosition(graph, RenderPassId::DepthToReadOnly),
            executionPosition(graph, RenderPassId::Lighting));
  EXPECT_LT(executionPosition(graph, RenderPassId::Lighting),
            executionPosition(graph, RenderPassId::TransformGizmos));
  EXPECT_LT(executionPosition(graph, RenderPassId::TransformGizmos),
            executionPosition(graph, RenderPassId::ExposureAdaptation));
  EXPECT_LT(executionPosition(graph, RenderPassId::ExposureAdaptation),
            executionPosition(graph, RenderPassId::OitResolve));
  EXPECT_LT(executionPosition(graph, RenderPassId::OitResolve),
            executionPosition(graph, RenderPassId::Bloom));
  EXPECT_LT(executionPosition(graph, RenderPassId::Bloom),
            executionPosition(graph, RenderPassId::PostProcess));
  EXPECT_TRUE(resourceSpanContains(
      container::renderer::renderPassResourceReads(
          RenderPassId::LocalShadowDepth),
      RenderResourceId::LocalShadowData));
  EXPECT_TRUE(resourceSpanContains(
      container::renderer::renderPassResourceWrites(
          RenderPassId::LocalShadowDepth),
      RenderResourceId::LocalShadowAtlas));
  EXPECT_TRUE(resourceSpanContains(
      container::renderer::renderPassOptionalResourceReads(
          RenderPassId::Lighting),
      RenderResourceId::LocalShadowAtlas));
  EXPECT_FALSE(passSpanContains(
      container::renderer::renderPassScheduleDependencies(
          RenderPassId::LocalShadowDepth),
      RenderPassId::ShadowCascade0));
  EXPECT_FALSE(passSpanContains(
      container::renderer::renderPassScheduleDependencies(
          RenderPassId::LocalShadowDepth),
      RenderPassId::ShadowCascade3));
  EXPECT_FALSE(passSpanContains(
      container::renderer::renderPassScheduleDependencies(
          RenderPassId::DepthToReadOnly),
      RenderPassId::LocalShadowDepth));
}

TEST(RenderGraphTests, OpenWorldFrameDoesNotRequireLocalShadowDepthPass) {
  constexpr std::array passes = {
      RenderPassId::FrustumCull,
      RenderPassId::DepthPrepass,
      RenderPassId::HiZGenerate,
      RenderPassId::OcclusionCull,
      RenderPassId::CullStatsReadback,
      RenderPassId::GBuffer,
      RenderPassId::TransparentPick,
      RenderPassId::ShadowCullCascade0,
      RenderPassId::ShadowCullCascade1,
      RenderPassId::ShadowCullCascade2,
      RenderPassId::ShadowCullCascade3,
      RenderPassId::ShadowCascade0,
      RenderPassId::ShadowCascade1,
      RenderPassId::ShadowCascade2,
      RenderPassId::ShadowCascade3,
      RenderPassId::DepthToReadOnly,
  };

  RenderGraph graph;
  for (auto it = passes.rbegin(); it != passes.rend(); ++it) {
    graph.addPass(*it, noopRecord());
  }

  EXPECT_NO_THROW(graph.compile());
  EXPECT_LT(executionPosition(graph, RenderPassId::ShadowCascade3),
            executionPosition(graph, RenderPassId::DepthToReadOnly));
}

TEST(RenderGraphTests, BimPassesSlotIntoFrameOrderWhenRegistered) {
  constexpr std::array passes = {
      RenderPassId::FrustumCull,
      RenderPassId::DepthPrepass,
      RenderPassId::BimDepthPrepass,
      RenderPassId::HiZGenerate,
      RenderPassId::OcclusionCull,
      RenderPassId::CullStatsReadback,
      RenderPassId::GBuffer,
      RenderPassId::BimGBuffer,
      RenderPassId::TransparentPick,
      RenderPassId::OitClear,
      RenderPassId::ShadowCullCascade0,
      RenderPassId::ShadowCullCascade1,
      RenderPassId::ShadowCullCascade2,
      RenderPassId::ShadowCullCascade3,
      RenderPassId::ShadowCascade0,
      RenderPassId::ShadowCascade1,
      RenderPassId::ShadowCascade2,
      RenderPassId::ShadowCascade3,
      RenderPassId::LocalShadowDepth,
      RenderPassId::DepthToReadOnly,
      RenderPassId::TileCull,
      RenderPassId::GTAO,
      RenderPassId::Lighting,
      RenderPassId::TransformGizmos,
  };

  RenderGraph graph;
  for (auto it = passes.rbegin(); it != passes.rend(); ++it) {
    graph.addPass(*it, noopRecord());
  }

  graph.compile();

  EXPECT_LT(executionPosition(graph, RenderPassId::DepthPrepass),
            executionPosition(graph, RenderPassId::BimDepthPrepass));
  EXPECT_LT(executionPosition(graph, RenderPassId::BimDepthPrepass),
            executionPosition(graph, RenderPassId::HiZGenerate));
  EXPECT_LT(executionPosition(graph, RenderPassId::HiZGenerate),
            executionPosition(graph, RenderPassId::OcclusionCull));
  EXPECT_LT(executionPosition(graph, RenderPassId::OcclusionCull),
            executionPosition(graph, RenderPassId::GBuffer));
  EXPECT_LT(executionPosition(graph, RenderPassId::BimDepthPrepass),
            executionPosition(graph, RenderPassId::GBuffer));
  EXPECT_LT(executionPosition(graph, RenderPassId::GBuffer),
            executionPosition(graph, RenderPassId::BimGBuffer));
  EXPECT_LT(executionPosition(graph, RenderPassId::BimGBuffer),
            executionPosition(graph, RenderPassId::TransparentPick));
  EXPECT_LT(executionPosition(graph, RenderPassId::TransparentPick),
            executionPosition(graph, RenderPassId::DepthToReadOnly));
  EXPECT_LT(executionPosition(graph, RenderPassId::DepthToReadOnly),
            executionPosition(graph, RenderPassId::Lighting));
  EXPECT_LT(executionPosition(graph, RenderPassId::Lighting),
            executionPosition(graph, RenderPassId::TransformGizmos));
}
