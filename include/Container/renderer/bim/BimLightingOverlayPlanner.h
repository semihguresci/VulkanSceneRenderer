#pragma once
#include "Container/renderer/debug/SelectionStrokeKernel.h"

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <vector>

namespace container::renderer {

struct DrawCommand;

enum class BimLightingOverlayKind : uint32_t {
  PointStyle = 0,
  CurveStyle = 1,
  FloorPlan = 2,
  SceneHover = 3,
  BimHover = 4,
  NativePointHover = 5,
  NativeCurveHover = 6,
  NativePointSelection = 7,
  NativeCurveSelection = 8,
  CoordinationIssueMarker = 9,
  CoordinationClashMarker = 10,
  SectionPlaneVisual = 11,
};

enum class BimLightingOverlayPipeline : uint32_t {
  WireframeDepth = 0,
  WireframeNoDepth = 1,
  WireframeDepthFrontCull = 2,
  WireframeNoDepthFrontCull = 3,
  BimFloorPlanDepth = 4,
  BimFloorPlanNoDepth = 5,
  BimPointCloudDepth = 6,
  BimCurveDepth = 7,
};

struct BimLightingOverlayDrawLists {
  const std::vector<DrawCommand> *opaqueSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueWindingFlippedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentWindingFlippedDrawCommands{
      nullptr};
  const std::vector<DrawCommand> *opaqueDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentDoubleSidedDrawCommands{nullptr};
};

struct BimLightingOverlayPipelineReadiness {
  bool wireframeDepth{false};
  bool wireframeNoDepth{false};
  bool bimFloorPlanDepth{false};
  bool bimFloorPlanNoDepth{false};
  bool bimPointCloudDepth{false};
  bool bimCurveDepth{false};
  bool selectionMask{false};
  bool selectionOutline{false};
};

struct BimLightingOverlayStyleInputs {
  bool enabled{false};
  bool depthTest{true};
  glm::vec3 color{1.0f};
  float opacity{1.0f};
  float lineWidth{1.0f};
  BimLightingOverlayDrawLists draws{};
};

struct BimLightingFloorPlanOverlayInputs {
  bool enabled{false};
  bool depthTest{true};
  glm::vec3 color{0.02f, 0.02f, 0.02f};
  float opacity{1.0f};
  float lineWidth{1.0f};
  const std::vector<DrawCommand> *commands{nullptr};
};

struct BimLightingOverlayInputs {
  bool bimGeometryReady{false};
  bool wireframeLayoutReady{false};
  bool wireframePushConstantsReady{false};
  bool wideLinesSupported{false};
  uint32_t framebufferWidth{1};
  uint32_t framebufferHeight{1};
  BimLightingOverlayPipelineReadiness pipelines{};

  BimLightingOverlayStyleInputs points{};
  BimLightingOverlayStyleInputs curves{};
  BimLightingFloorPlanOverlayInputs floorPlan{};

  const std::vector<DrawCommand> *sceneHoverCommands{nullptr};
  const std::vector<DrawCommand> *bimHoverCommands{nullptr};
  const std::vector<DrawCommand> *sceneSelectionCommands{nullptr};
  const std::vector<DrawCommand> *bimSelectionCommands{nullptr};
  const std::vector<DrawCommand> *nativePointHoverCommands{nullptr};
  const std::vector<DrawCommand> *nativeCurveHoverCommands{nullptr};
  const std::vector<DrawCommand> *nativePointSelectionCommands{nullptr};
  const std::vector<DrawCommand> *nativeCurveSelectionCommands{nullptr};
  const std::vector<DrawCommand> *coordinationIssueMarkerCommands{nullptr};
  const std::vector<DrawCommand> *coordinationClashMarkerCommands{nullptr};
  const std::vector<DrawCommand> *sectionPlaneVisualCommands{nullptr};
  float nativePointSize{1.0f};
  float nativeCurveLineWidth{1.0f};
  bool coordinationMarkerGeometryReady{false};
  bool sectionPlaneVisualGeometryReady{false};
  BimLightingOverlayStyleInputs coordinationIssueMarkers{};
  BimLightingOverlayStyleInputs coordinationClashMarkers{};
  BimLightingOverlayStyleInputs sectionPlaneVisual{};
};

struct BimLightingOverlayDrawRoute {
  BimLightingOverlayPipeline pipeline{BimLightingOverlayPipeline::WireframeDepth};
  const std::vector<DrawCommand> *commands{nullptr};
  float rasterLineWidth{1.0f};
};

struct BimLightingOverlayStylePlan {
  bool active{false};
  BimLightingOverlayKind kind{BimLightingOverlayKind::PointStyle};
  glm::vec3 color{1.0f};
  float opacity{1.0f};
  float drawLineWidth{1.0f};
  std::array<BimLightingOverlayDrawRoute, 6> routes{};
  uint32_t routeCount{0};
};

struct BimLightingOverlayDrawPlan {
  bool active{false};
  BimLightingOverlayKind kind{BimLightingOverlayKind::FloorPlan};
  BimLightingOverlayPipeline pipeline{BimLightingOverlayPipeline::WireframeDepth};
  const std::vector<DrawCommand> *commands{nullptr};
  glm::vec3 color{1.0f};
  float opacity{1.0f};
  float drawLineWidth{1.0f};
  float rasterLineWidth{1.0f};
  bool rasterLineWidthApplies{true};
};

struct BimLightingSelectionOutlinePlan {
  bool active{false};
  const std::vector<DrawCommand> *commands{nullptr};
  glm::vec3 color{1.0f, 0.46f, 0.0f};
  float maskLineWidth{1.0f};
  float outlineLineWidth{float(kSelectionStrokeRadius)};
  uint32_t framebufferWidth{1};
  uint32_t framebufferHeight{1};
};

struct BimLightingOverlayPlan {
  BimLightingOverlayStylePlan pointStyle{};
  BimLightingOverlayStylePlan curveStyle{};
  BimLightingOverlayDrawPlan floorPlan{};
  BimLightingOverlayDrawPlan sceneHover{};
  BimLightingOverlayDrawPlan bimHover{};
  BimLightingOverlayDrawPlan nativePointHover{};
  BimLightingOverlayDrawPlan nativeCurveHover{};
  BimLightingOverlayDrawPlan nativePointSelection{};
  BimLightingOverlayDrawPlan nativeCurveSelection{};
  BimLightingOverlayDrawPlan coordinationIssueMarkers{};
  BimLightingOverlayDrawPlan coordinationClashMarkers{};
  BimLightingOverlayDrawPlan sectionPlaneVisual{};
  BimLightingSelectionOutlinePlan sceneSelectionOutline{};
  BimLightingSelectionOutlinePlan bimSelectionOutline{};
};

class BimLightingOverlayPlanner {
public:
  explicit BimLightingOverlayPlanner(BimLightingOverlayInputs inputs);

  [[nodiscard]] BimLightingOverlayPlan build() const;

private:
  BimLightingOverlayInputs inputs_{};
};

[[nodiscard]] float sanitizeBimLightingOverlayOpacity(float opacity);
[[nodiscard]] float sanitizeBimLightingOverlayLineWidth(float lineWidth);
[[nodiscard]] float rasterBimLightingOverlayLineWidth(float lineWidth,
                                                      bool wideLinesSupported);

[[nodiscard]] BimLightingOverlayPlan
buildBimLightingOverlayPlan(const BimLightingOverlayInputs &inputs);

} // namespace container::renderer
