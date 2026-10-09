#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Container/renderer/core/TechniqueDebugModel.h"

// Forward declaration. No Vulkan header needed here.
struct VkCommandBuffer_T;
using VkCommandBuffer = VkCommandBuffer_T*;

namespace container::renderer {

struct FrameRecordParams;

// Stable IDs for the renderer's frame graph passes. Display names are still
// exposed for the debug UI, but renderer code should use IDs for scheduling,
// dependency checks, and pass lookups.
enum class RenderPassId : uint8_t {
  FrustumCull,
  DepthPrepass,
  BimDepthPrepass,
  HiZGenerate,
  OcclusionCull,
  CullStatsReadback,
  GBuffer,
  BimGBuffer,
  TransparentPick,
  OitClear,
  ShadowCullCascade0,
  ShadowCullCascade1,
  ShadowCullCascade2,
  ShadowCullCascade3,
  ShadowCascade0,
  ShadowCascade1,
  ShadowCascade2,
  ShadowCascade3,
  LocalShadowDepth,
  DepthToReadOnly,
  TileCull,
  GTAO,
  Lighting,
  TransformGizmos,
  ExposureAdaptation,
  OitResolve,
  Bloom,
  PostProcess,
  TemporalVelocity,
  TemporalResolve,
  RaySceneBuild,
  RayShadowTrace,
  RayShadowFilter,
  Count,
  Invalid = Count,
};

inline constexpr auto kRenderPassIdCount =
    static_cast<size_t>(RenderPassId::Count);

enum class RenderResourceId : uint8_t {
  SceneGeometry,
  BimGeometry,
  CameraBuffer,
  ObjectBuffer,
  BimObjectBuffer,
  LightingData,
  ShadowData,
  EnvironmentMaps,
  FrustumCullDraws,
  HiZPyramid,
  OcclusionCullDraws,
  CullStats,
  SceneDepth,
  GBufferAlbedo,
  GBufferNormal,
  GBufferMaterial,
  GBufferEmissive,
  GBufferSpecular,
  GBufferMaterialLayers,
  GBufferMaterialSheen,
  PickId,
  PickDepth,
  OitStorage,
  ShadowCullCascade0,
  ShadowCullCascade1,
  ShadowCullCascade2,
  ShadowCullCascade3,
  ShadowAtlas,
  LocalShadowData,
  LocalShadowAtlas,
  TileLightGrid,
  AmbientOcclusion,
  SceneColor,
  ExposureState,
  BloomTexture,
  SwapchainImage,
  TemporalMotion,
  TemporalColor,
  RayScene,
  RayShadowRaw,
  RayShadowVisibility,
  Count,
  Invalid = Count,
};

inline constexpr auto kRenderResourceIdCount =
    static_cast<size_t>(RenderResourceId::Count);

struct RenderResourceEdge {
  RenderResourceId resource{RenderResourceId::Invalid};
  RenderPassId     writer{RenderPassId::Invalid};
  RenderPassId     reader{RenderPassId::Invalid};
};

enum class RenderResourceState : uint8_t {
  Undefined,
  ColorAttachment,
  DepthStencilAttachment,
  DepthStencilReadOnly,
  ShaderRead,
  ShaderWrite,
  TransferRead,
  Present,
};

struct RenderResourceTransition {
  RenderResourceId    resource{RenderResourceId::Invalid};
  RenderResourceState before{RenderResourceState::Undefined};
  RenderResourceState after{RenderResourceState::Undefined};
};

enum class RenderPassSkipReason : uint8_t {
  None,
  Disabled,
  MissingPassDependency,
  MissingResource,
  MissingRecordCallback,
  NotNeeded,
};

struct RenderPassExecutionStatus {
  RenderPassId           id{RenderPassId::Invalid};
  bool                   active{false};
  RenderPassSkipReason   skipReason{RenderPassSkipReason::None};
  RenderPassId           blockingPass{RenderPassId::Invalid};
  RenderResourceId       blockingResource{RenderResourceId::Invalid};
};

struct RenderPassReadiness {
  bool                   ready{true};
  RenderPassSkipReason   skipReason{RenderPassSkipReason::None};
  RenderPassId           blockingPass{RenderPassId::Invalid};
  RenderResourceId       blockingResource{RenderResourceId::Invalid};
};

[[nodiscard]] std::string_view renderPassName(RenderPassId id);
[[nodiscard]] RenderPassId renderPassIdFromName(std::string_view name);
[[nodiscard]] bool isProtectedRenderPass(RenderPassId id);
// Enable dependencies gate optional passes when their producer pass is
// disabled. Schedule dependencies only constrain graph order.
[[nodiscard]] std::span<const RenderPassId> renderPassDependencies(RenderPassId id);
[[nodiscard]] std::span<const RenderPassId> renderPassScheduleDependencies(RenderPassId id);
[[nodiscard]] std::string_view renderResourceName(RenderResourceId id);
[[nodiscard]] bool isExternalRenderResource(RenderResourceId id);
[[nodiscard]] std::span<const RenderResourceId> renderPassResourceReads(RenderPassId id);
[[nodiscard]] std::span<const RenderResourceId> renderPassOptionalResourceReads(RenderPassId id);
[[nodiscard]] std::span<const RenderResourceId> renderPassResourceWrites(RenderPassId id);
[[nodiscard]] std::span<const RenderResourceTransition> renderPassResourceTransitions(RenderPassId id);
[[nodiscard]] std::string_view renderResourceStateName(RenderResourceState state);
[[nodiscard]] std::string_view renderPassSkipReasonName(RenderPassSkipReason reason);
[[nodiscard]] std::span<const RenderPassId> shadowCullPassIds();
[[nodiscard]] std::span<const RenderPassId> shadowCascadePassIds();

// A single node in the render graph.
// Each node wraps a callable that records commands for one logical pass.
struct RenderPassNode {
  RenderPassId id{RenderPassId::Invalid};
  std::string  name;
  std::vector<RenderPassId> scheduleDependencies;
  std::vector<RenderResourceId> reads;
  std::vector<RenderResourceId> optionalReads;
  std::vector<RenderResourceId> writes;
  std::vector<RenderResourceTransition> transitions;
  bool         enabled{true};

  // The recording callback. Receives the command buffer and the current
  // per-frame params built by RendererFrontend.
  using RecordFn = std::function<void(VkCommandBuffer, const FrameRecordParams&)>;
  using ReadinessFn = std::function<RenderPassReadiness(const FrameRecordParams&)>;
  ReadinessFn readiness;
  RecordFn record;
};

struct RenderPassExecutionHooks {
  std::function<void(RenderPassId, VkCommandBuffer)> beginPass;
  std::function<void(RenderPassId, VkCommandBuffer, float)> endPass;
};

// Ordered sequence of render pass nodes.
// Nodes are scheduled from their dependency edges; disabled nodes are skipped.
//
// The graph performs dependency validation, topological sorting, and owns the
// pass-level resource transition intent. FrameRecorder still emits the concrete
// Vulkan barriers while the transition executor is kept close to pass-specific
// attachment handles.
class RenderGraph {
 public:
  RenderGraph();

  // Append a pass to the graph.
  // Returns a reference to the newly added node. The reference is invalidated
  // by subsequent addPass() calls; use findPass() for deferred inspection and
  // the setPass* methods for deferred mutation.
  const RenderPassNode& addPass(RenderPassId id, RenderPassNode::RecordFn fn);
  const RenderPassNode& addPass(
      RenderPassId id,
      std::initializer_list<RenderPassId> scheduleDependencies,
      RenderPassNode::RecordFn fn);
  const RenderPassNode& addPass(
      RenderPassId id,
      std::span<const RenderPassId> scheduleDependencies,
      RenderPassNode::RecordFn fn);

  // Validate dependencies and rebuild the execution schedule.
  void compile();

  // Evaluate per-frame pass readiness without recording commands. This is for
  // pre-record work that needs to know the same runtime pass plan execute()
  // will use, such as cache population or secondary command-buffer setup.
  void prepareFrame(const FrameRecordParams& params) const;

  // Execute all enabled passes in dependency order.
  void execute(VkCommandBuffer cmd, const FrameRecordParams& params) const;
  void execute(VkCommandBuffer cmd,
               const FrameRecordParams& params,
               const RenderPassExecutionHooks& hooks) const;
  void executePreparedFrame(VkCommandBuffer cmd,
                            const FrameRecordParams& params) const;
  void executePreparedFrame(VkCommandBuffer cmd,
                            const FrameRecordParams& params,
                            const RenderPassExecutionHooks& hooks) const;

  // Update a pass toggle and invalidate the active execution plan when it
  // changes. Returns false when the pass is not registered.
  bool setPassEnabled(RenderPassId id, bool enabled);
  bool setPassRecord(RenderPassId id, RenderPassNode::RecordFn fn);
  bool setPassReadiness(RenderPassId id, RenderPassNode::ReadinessFn fn);
  bool setPassScheduleDependencies(
      RenderPassId id,
      std::initializer_list<RenderPassId> scheduleDependencies);
  bool setPassScheduleDependencies(
      RenderPassId id,
      std::span<const RenderPassId> scheduleDependencies);
  bool setPassResourceAccess(
      RenderPassId id,
      std::initializer_list<RenderResourceId> reads,
      std::initializer_list<RenderResourceId> optionalReads,
      std::initializer_list<RenderResourceId> writes);
  bool setPassResourceAccess(
      RenderPassId id,
      std::span<const RenderResourceId> reads,
      std::span<const RenderResourceId> optionalReads,
      std::span<const RenderResourceId> writes);
  bool setPassResourceTransitions(
      RenderPassId id,
      std::initializer_list<RenderResourceTransition> transitions);
  bool setPassResourceTransitions(
      RenderPassId id,
      std::span<const RenderResourceTransition> transitions);

  // Access a pass by stable ID. Returns nullptr when the graph has not
  // registered that pass.
  const RenderPassNode* findPass(RenderPassId id) const;

  // Number of registered passes.
  [[nodiscard]] uint32_t passCount() const;

  // Number of currently enabled passes.
  [[nodiscard]] uint32_t enabledPassCount() const;

  // Read-only access to the pass list for UI iteration and diagnostics.
  [[nodiscard]] std::span<const RenderPassNode> passes() const {
    return {passes_.data(), passes_.size()};
  }
  [[nodiscard]] std::span<const RenderPassId> executionPassIds() const;
  [[nodiscard]] std::span<const RenderPassId> activeExecutionPassIds() const;
  [[nodiscard]] std::span<const RenderPassExecutionStatus> executionStatuses() const;
  [[nodiscard]] std::span<const RenderPassId> lastFrameActiveExecutionPassIds() const;
  [[nodiscard]] std::span<const RenderPassExecutionStatus> lastFrameExecutionStatuses() const;
  [[nodiscard]] const RenderPassExecutionStatus* executionStatus(RenderPassId id) const;
  [[nodiscard]] bool isPassActive(RenderPassId id) const;
  [[nodiscard]] std::span<const RenderResourceEdge> resourceEdges() const;
  [[nodiscard]] RenderGraphDebugModel debugModel() const;

  // Remove all passes.
  void clear();

 private:
  [[nodiscard]] uint32_t indexFor(RenderPassId id) const;
  [[nodiscard]] RenderPassNode* mutablePass(RenderPassId id);
  [[nodiscard]] std::vector<std::vector<uint32_t>> buildResourceDependencies();
  [[nodiscard]] uint64_t computeActivePlanSignature() const;
  [[nodiscard]] uint64_t
  computePreparedFrameSignature(const FrameRecordParams& params) const;
  void invalidatePreparedFrame();
  void ensureActivePlan() const;
  void rebuildActiveExecutionOrder() const;
  void rebuildFrameExecutionOrder(const FrameRecordParams& params) const;
  void ensureCompiled() const;

  std::vector<RenderPassNode> passes_;
  std::array<uint32_t, kRenderPassIdCount> passIndexById_{};
  mutable std::vector<uint32_t> executionOrder_;
  mutable std::vector<uint32_t> activeExecutionOrder_;
  mutable std::vector<RenderPassId> executionPassIds_;
  mutable std::vector<RenderPassId> activeExecutionPassIds_;
  mutable std::vector<uint32_t> lastFrameExecutionOrder_;
  mutable std::vector<RenderPassId> lastFrameActiveExecutionPassIds_;
  mutable std::vector<uint8_t> activePasses_;
  mutable std::vector<RenderPassExecutionStatus> executionStatuses_;
  mutable std::vector<RenderPassExecutionStatus> lastFrameExecutionStatuses_;
  std::vector<RenderResourceEdge> resourceEdges_;
  mutable bool executionOrderDirty_{false};
  mutable bool activePlanDirty_{true};
  mutable bool preparedFramePlanDirty_{true};
  mutable uint64_t activePlanSignature_{0};
  mutable uint64_t preparedFrameSignature_{0};
  mutable bool executing_{false};
};

class RenderGraphBuilder {
 public:
  explicit RenderGraphBuilder(RenderGraph& graph) : graph_(graph) {}

  void clear() { graph_.clear(); }

  const RenderPassNode& addPass(RenderPassId id,
                                RenderPassNode::RecordFn fn) {
    return graph_.addPass(id, std::move(fn));
  }

  const RenderPassNode& addPass(
      RenderPassId id,
      std::initializer_list<RenderPassId> scheduleDependencies,
      RenderPassNode::RecordFn fn) {
    return graph_.addPass(id, scheduleDependencies, std::move(fn));
  }

  const RenderPassNode& addPass(
      RenderPassId id,
      std::span<const RenderPassId> scheduleDependencies,
      RenderPassNode::RecordFn fn) {
    return graph_.addPass(id, scheduleDependencies, std::move(fn));
  }

  bool setPassReadiness(RenderPassId id,
                        RenderPassNode::ReadinessFn readiness) {
    return graph_.setPassReadiness(id, std::move(readiness));
  }

  bool setPassResourceAccess(
      RenderPassId id,
      std::initializer_list<RenderResourceId> reads,
      std::initializer_list<RenderResourceId> optionalReads,
      std::initializer_list<RenderResourceId> writes) {
    return graph_.setPassResourceAccess(id, reads, optionalReads, writes);
  }

  bool setPassResourceAccess(
      RenderPassId id,
      std::span<const RenderResourceId> reads,
      std::span<const RenderResourceId> optionalReads,
      std::span<const RenderResourceId> writes) {
    return graph_.setPassResourceAccess(id, reads, optionalReads, writes);
  }

  bool setPassResourceTransitions(
      RenderPassId id,
      std::initializer_list<RenderResourceTransition> transitions) {
    return graph_.setPassResourceTransitions(id, transitions);
  }

  bool setPassResourceTransitions(
      RenderPassId id,
      std::span<const RenderResourceTransition> transitions) {
    return graph_.setPassResourceTransitions(id, transitions);
  }

  void compile() { graph_.compile(); }

 private:
  RenderGraph& graph_;
};

}  // namespace container::renderer
