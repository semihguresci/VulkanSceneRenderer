#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace container::renderer {

struct DrawCommand;
struct FrameDrawLists;

inline constexpr uint32_t kDeferredDebugOverlayNormalValidationInvertFaceClassification =
    1u << 0u;
inline constexpr uint32_t kDeferredDebugOverlayNormalValidationBothSidesValid =
    1u << 1u;
inline constexpr uint32_t kDeferredDebugOverlaySourceCapacity = 4u;
inline constexpr uint32_t kDeferredDebugOverlayRouteCapacity = 6u;

enum class DeferredDebugOverlaySource : uint32_t {
  Scene = 0,
  BimMesh = 1,
  BimPointPlaceholders = 2,
  BimCurvePlaceholders = 3,
};

enum class DeferredDebugOverlayPipeline : uint32_t {
  WireframeDepth = 0,
  WireframeNoDepth = 1,
  WireframeDepthFrontCull = 2,
  WireframeNoDepthFrontCull = 3,
  ObjectNormalDebug = 4,
  ObjectNormalDebugFrontCull = 5,
  ObjectNormalDebugNoCull = 6,
  GeometryDebug = 7,
  NormalValidation = 8,
  NormalValidationFrontCull = 9,
  NormalValidationNoCull = 10,
  SurfaceNormalLine = 11,
};

struct DeferredDebugOverlayFrameState {
  bool wireframeFullMode{false};
  bool wireframeOverlayMode{false};
  bool objectSpaceNormalsEnabled{false};
  bool geometryOverlayEnabled{false};
  bool normalValidationEnabled{false};
  bool surfaceNormalLinesEnabled{false};
  bool wireframeDepthTest{true};
  float wireframeLineWidth{1.0f};
  float surfaceNormalLineWidth{1.0f};
};

struct DeferredDebugOverlayPipelineReadiness {
  bool wireframeDepth{false};
  bool wireframeNoDepth{false};
  bool wireframeDepthFrontCull{false};
  bool wireframeNoDepthFrontCull{false};
  bool objectNormalDebug{false};
  bool objectNormalDebugFrontCull{false};
  bool objectNormalDebugNoCull{false};
  bool geometryDebug{false};
  bool normalValidation{false};
  bool normalValidationFrontCull{false};
  bool normalValidationNoCull{false};
  bool surfaceNormalLine{false};
};

struct DeferredDebugOverlayDrawLists {
  const std::vector<DrawCommand> *opaqueDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueWindingFlippedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentWindingFlippedDrawCommands{
      nullptr};
  const std::vector<DrawCommand> *opaqueDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentDoubleSidedDrawCommands{nullptr};
};

[[nodiscard]] DeferredDebugOverlayDrawLists
debugOverlayDrawLists(const FrameDrawLists &draws);

struct DeferredDebugOverlaySourceInput {
  DeferredDebugOverlaySource source{DeferredDebugOverlaySource::Scene};
  bool geometryReady{false};
  bool drawDiagnosticCube{false};
  uint32_t diagnosticCubeObjectIndex{std::numeric_limits<uint32_t>::max()};
  DeferredDebugOverlayDrawLists draws{};
};

struct DeferredDebugOverlayInputs {
  DeferredDebugOverlayFrameState frameState{};
  DeferredDebugOverlayPipelineReadiness pipelines{};
  bool bindlessPushConstantsReady{false};
  bool wireframePushConstantsReady{false};
  bool normalValidationPushConstantsReady{false};
  bool surfaceNormalPushConstantsReady{false};
  std::array<DeferredDebugOverlaySourceInput,
             kDeferredDebugOverlaySourceCapacity>
      sources{};
  uint32_t sourceCount{0};
};

struct DeferredDebugOverlayRoute {
  DeferredDebugOverlayPipeline pipeline{DeferredDebugOverlayPipeline::WireframeDepth};
  const std::vector<DrawCommand> *commands{nullptr};
  const std::vector<DrawCommand> *opaqueCommands{nullptr};
  const std::vector<DrawCommand> *transparentCommands{nullptr};
  uint32_t normalValidationFaceFlags{0};
  float drawLineWidth{1.0f};
  float rasterLineWidth{1.0f};
};

struct DeferredDebugOverlaySourcePlan {
  DeferredDebugOverlaySource source{DeferredDebugOverlaySource::Scene};
  std::array<DeferredDebugOverlayRoute, kDeferredDebugOverlayRouteCapacity>
      routes{};
  uint32_t routeCount{0};
  bool drawDiagnosticCube{false};
  DeferredDebugOverlayPipeline diagnosticCubePipeline{
      DeferredDebugOverlayPipeline::WireframeDepth};
  uint32_t diagnosticCubeObjectIndex{std::numeric_limits<uint32_t>::max()};
};

struct DeferredDebugOverlayPlan {
  std::array<DeferredDebugOverlaySourcePlan,
             kDeferredDebugOverlaySourceCapacity>
      wireframeFullSources{};
  uint32_t wireframeFullSourceCount{0};

  std::array<DeferredDebugOverlaySourcePlan,
             kDeferredDebugOverlaySourceCapacity>
      objectNormalSources{};
  uint32_t objectNormalSourceCount{0};

  std::array<DeferredDebugOverlaySourcePlan,
             kDeferredDebugOverlaySourceCapacity>
      geometryOverlaySources{};
  uint32_t geometryOverlaySourceCount{0};

  std::array<DeferredDebugOverlaySourcePlan,
             kDeferredDebugOverlaySourceCapacity>
      normalValidationSources{};
  uint32_t normalValidationSourceCount{0};

  std::array<DeferredDebugOverlaySourcePlan,
             kDeferredDebugOverlaySourceCapacity>
      surfaceNormalSources{};
  uint32_t surfaceNormalSourceCount{0};

  std::array<DeferredDebugOverlaySourcePlan,
             kDeferredDebugOverlaySourceCapacity>
      wireframeOverlaySources{};
  uint32_t wireframeOverlaySourceCount{0};
};

class DeferredRasterDebugOverlayPlanner {
public:
  explicit DeferredRasterDebugOverlayPlanner(DeferredDebugOverlayInputs inputs);

  [[nodiscard]] DeferredDebugOverlayPlan build() const;

private:
  DeferredDebugOverlayInputs inputs_{};
};

[[nodiscard]] DeferredDebugOverlayPlan
buildDeferredDebugOverlayPlan(const DeferredDebugOverlayInputs &inputs);

} // namespace container::renderer
