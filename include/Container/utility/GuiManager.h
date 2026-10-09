#pragma once
#include "Container/renderer/raytracing/RayShadowSettings.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/geometry/ImportReport.h"
#include "Container/renderer/bim/BimCoordinationOverlay.h"
#include "Container/renderer/bim/BimGeoreferenceTransform.h"
#include "Container/renderer/bim/BimModelCompare.h"
#include "Container/renderer/bim/BimScheduleExtractor.h"
#include "Container/renderer/lighting/EditableLight.h"
#include "Container/renderer/temporal/TemporalState.h"
#include "Container/utility/GuiDebugState.h"
#include "Container/utility/SceneData.h"

struct GLFWwindow;

namespace container::scene {
class SceneGraph;
} // namespace container::scene

namespace container::renderer {
enum class BimDisciplinePreset : uint32_t;
enum class BimSemanticColorMode : uint32_t;
enum class BimSnapKind : uint32_t;
enum class RenderTechniqueId;
enum class ScenePrimitiveKind : uint32_t;
struct BimElementProperty;
class BimRelationshipGraph;
struct BimStoreyRange;
struct CullStats;
struct RendererTelemetryView;
} // namespace container::renderer

namespace container::ui {

enum class GBufferViewMode : uint32_t {
  Lit = 0,
  Albedo = 1,
  Normals = 2,
  Material = 3,
  Depth = 4,
  Emissive = 5,
  Transparency = 6,
  Revealage = 7,
  Overview = 8,
  SurfaceNormals = 9,
  ObjectSpaceNormals = 10,
  ShadowCascades = 11,
  TileLightHeatMap = 12,
  ShadowTexelDensity = 13,
  TemporalVelocity = 100,
  TemporalHistoryAge = 101,
  TemporalRejection = 102,
  TemporalBlend = 103,
  TemporalReactive = 104,
};

enum class SceneViewportMode : uint32_t {
  Editor = 0,
  RenderPreview = 1,
};

enum class WireframeMode : uint32_t {
  Overlay = 0,
  Full = 1,
};

enum class ViewportTool : uint32_t {
  Select = 0,
  Translate = 1,
  Rotate = 2,
  Scale = 3,
};

enum class TransformSpace : uint32_t {
  Local = 0,
  World = 1,
};

enum class TransformAxis : uint32_t {
  Free = 0,
  X = 1,
  Y = 2,
  Z = 3,
};

enum class ViewportGesture : uint32_t {
  None = 0,
  FlyLook = 1,
  Orbit = 2,
  Pan = 3,
  TransformDrag = 4,
};

enum class ViewportNavigationStyle : uint32_t {
  Revit = 0,
  Blender = 1,
};

enum class CameraProjectionMode : uint32_t {
  Perspective = 0,
  Orthographic = 1,
};

enum class CameraViewPreset : uint32_t {
  Front = 0,
  Back = 1,
  Right = 2,
  Left = 3,
  Top = 4,
  Bottom = 5,
};

struct RenderEngineOption {
  container::renderer::RenderTechniqueId id{};
  std::string label{};
  bool available{true};
  std::string unavailableReason{};
};

struct ModelLoadRequest {
  std::string path{};
  float importScale{1.0f};
  std::string label{};
};

struct ViewportInteractionState {
  ViewportTool tool{ViewportTool::Select};
  TransformSpace transformSpace{TransformSpace::Local};
  TransformAxis transformAxis{TransformAxis::Free};
  TransformAxis hoverTransformAxis{TransformAxis::Free};
  ViewportNavigationStyle navigationStyle{ViewportNavigationStyle::Revit};
  ViewportGesture gesture{ViewportGesture::None};
  bool transformSnapEnabled{false};
};

struct ViewportNavigationState {
  CameraProjectionMode projectionMode{CameraProjectionMode::Perspective};
  glm::vec3 cameraRight{1.0f, 0.0f, 0.0f};
  glm::vec3 cameraUp{0.0f, 1.0f, 0.0f};
  glm::vec3 cameraForward{0.0f, 0.0f, -1.0f};
};

struct WireframeSettings {
  bool enabled{false};
  WireframeMode mode{WireframeMode::Overlay};
  bool depthTest{true};
  glm::vec3 color{0.0f, 1.0f, 0.0f};
  float lineWidth{1.0f};
  float overlayIntensity{0.85f};
};

struct TransformControls {
  glm::vec3 position{0.0f, 0.0f, 0.0f};
  glm::vec3 rotationDegrees{0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f, 1.0f, 1.0f};
};

struct BimFilterState {
  bool typeFilterEnabled{false};
  std::string type{};
  bool storeyFilterEnabled{false};
  std::string storey{};
  bool materialFilterEnabled{false};
  std::string material{};
  bool disciplineFilterEnabled{false};
  std::string discipline{};
  bool phaseFilterEnabled{false};
  std::string phase{};
  bool fireRatingFilterEnabled{false};
  std::string fireRating{};
  bool loadBearingFilterEnabled{false};
  std::string loadBearing{};
  bool statusFilterEnabled{false};
  std::string status{};
  bool drawBudgetEnabled{false};
  uint32_t drawBudgetMaxObjects{0};
  bool isolateSelection{false};
  bool hideSelection{false};
  container::renderer::BimDisciplinePreset disciplinePreset{
      static_cast<container::renderer::BimDisciplinePreset>(0u)};
};

struct BimPhaseTimelineUiState {
  bool enabled{false};
  uint32_t activePhaseIndex{0};
  bool showExisting{true};
  bool showNew{true};
  bool showDemolished{false};
  bool ghostFuture{false};
};

enum class BimFloorPlanElevationMode : uint32_t {
  ProjectedGround = 0,
  SourceElevation = 1,
};

struct BimFloorPlanOverlayState {
  bool enabled{false};
  bool depthTest{false};
  BimFloorPlanElevationMode elevationMode{
      BimFloorPlanElevationMode::ProjectedGround};
  glm::vec3 color{0.02f, 0.02f, 0.02f};
  float opacity{0.85f};
  float lineWidth{1.0f};
};

enum class BimElevationTechnicalStyle : uint32_t {
  Shaded = 0,
  ShadedWithLines = 1,
  HiddenLine = 2,
};

struct BimElevationViewState {
  CameraViewPreset preset{CameraViewPreset::Front};
  // Display intent is mapped onto the existing G-buffer and wireframe controls.
  BimElevationTechnicalStyle style{BimElevationTechnicalStyle::ShadedWithLines};
  bool forceOrthographic{true};
  bool useDepthTestedLines{true};
  bool syncSectionPlaneToView{false};
};

struct BimElevationViewRequest {
  CameraViewPreset preset{CameraViewPreset::Front};
  BimElevationTechnicalStyle style{BimElevationTechnicalStyle::ShadedWithLines};
  bool forceOrthographic{true};
  bool syncSectionPlaneToView{false};
};

struct BimDrawingExportUiState {
  std::string path{"bim-drawing.svg"};
  float paperWidthMm{297.0f};
  float paperHeightMm{210.0f};
  float modelUnitsPerPaperMm{50.0f};
  bool exportRequested{false};
};

enum class BimMeasurementSnapMode : uint32_t {
  Off = 0,
  Vertex = 1,
  Edge = 2,
  Face = 3,
  Bounds = 4,
  Floor = 5,
};

struct BimMeasurementSnapUiState {
  BimMeasurementSnapMode mode{BimMeasurementSnapMode::Off};
  float maxScreenDistancePixels{12.0f};
};

struct BimMeasurementCapturedPoint {
  glm::vec3 center{0.0f};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  std::string label{};
  std::string modelPath{};
  container::renderer::BimSnapKind snapKind{};
};

struct BimInspectionState;

[[nodiscard]] std::optional<BimMeasurementCapturedPoint>
CaptureBimMeasurementPointFromSelection(
    const BimInspectionState &inspection,
    const BimMeasurementSnapUiState &snapState);

struct BimLayerVisibilityState {
  bool pointCloudVisible{true};
  bool curvesVisible{true};
  bool spaceLayerVisible{false};
  bool xrayLayerVisible{false};
  bool clashLayerVisible{false};
  bool markupLayerVisible{false};
};

struct BimLodStreamingUiState {
  bool autoLod{true};
  bool drawBudgetEnabled{false};
  int drawBudgetMaxObjects{10000};
  int lodBias{0};
  float screenErrorPixels{2.0f};
  bool pauseStreamingRequest{false};
  bool keepVisibleStoreysResident{false};
};

struct SectionPlaneState {
  bool enabled{false};
  glm::vec3 normal{0.0f, 1.0f, 0.0f};
  float offset{0.0f};
  bool visualPlaneVisible{true};
  bool visualPlaneEditable{false};
  float visualPlaneSize{10.0f};
  glm::vec3 visualPlaneColor{0.12f, 0.62f, 1.0f};
  float visualPlaneOpacity{0.55f};
  float visualPlaneLineWidth{2.0f};
};

struct BimBoxClipUiState {
  bool enabled{false};
  bool invert{false};
  glm::vec3 min{-10.0f, -10.0f, -10.0f};
  glm::vec3 max{10.0f, 10.0f, 10.0f};
};

struct BimClipCapHatchingUiState {
  bool capPreview{false};
  bool hatchingPreview{false};
  glm::vec3 capColor{0.06f, 0.08f, 0.10f};
  float capOpacity{0.82f};
  float hatchSpacing{0.25f};
  float hatchAngleDegrees{45.0f};
  float hatchLineWidth{1.0f};
  glm::vec3 hatchColor{0.08f, 0.08f, 0.08f};
  bool perMaterialCutStyles{false};
  uint32_t concreteMaterialIndex{0};
  float concreteHatchSpacing{0.12f};
  uint32_t glassMaterialIndex{1};
  float glassHatchSpacing{0.75f};
  bool sectionMarkersPreview{true};
  glm::vec3 sectionMarkerColor{0.95f, 0.62f, 0.12f};
  float sectionMarkerLineWidth{2.0f};
};

struct BimInspectionState {
  bool hasScene{false};
  std::string modelPath{};
  const container::geometry::ImportReport *importReport{nullptr};
  size_t objectCount{0};
  size_t meshObjectCount{0};
  size_t pointObjectCount{0};
  size_t curveObjectCount{0};
  size_t opaqueDrawCount{0};
  size_t transparentDrawCount{0};
  size_t pointOpaqueDrawCount{0};
  size_t pointTransparentDrawCount{0};
  size_t curveOpaqueDrawCount{0};
  size_t curveTransparentDrawCount{0};
  size_t nativePointOpaqueDrawCount{0};
  size_t nativePointTransparentDrawCount{0};
  size_t nativeCurveOpaqueDrawCount{0};
  size_t nativeCurveTransparentDrawCount{0};
  size_t meshletClusterCount{0};
  size_t meshletSourceClusterCount{0};
  size_t meshletEstimatedClusterCount{0};
  size_t meshletObjectReferenceCount{0};
  size_t meshletGpuResidentObjectCount{0};
  size_t meshletGpuResidentClusterCount{0};
  size_t meshletGpuBufferBytes{0};
  bool meshletGpuComputeReady{false};
  bool meshletGpuDispatchPending{false};
  uint32_t meshletMaxLodLevel{0};
  bool optimizedModelMetadataCacheable{false};
  bool optimizedModelMetadataCacheHit{false};
  bool optimizedModelMetadataCacheStale{false};
  bool optimizedModelMetadataCacheWritten{false};
  std::string optimizedModelMetadataCacheKey{};
  std::string optimizedModelMetadataCachePath{};
  std::string optimizedModelMetadataCacheStatus{};
  size_t drawBudgetVisibleObjectCount{0};
  size_t drawBudgetVisibleMeshObjectCount{0};
  size_t drawBudgetVisibleMeshletClusterCount{0};
  uint32_t drawBudgetVisibleMaxLodLevel{0};
  size_t floorPlanDrawCount{0};
  size_t uniqueTypeCount{0};
  size_t uniqueStoreyCount{0};
  size_t uniqueMaterialCount{0};
  size_t uniqueDisciplineCount{0};
  size_t uniquePhaseCount{0};
  size_t uniqueFireRatingCount{0};
  size_t uniqueLoadBearingCount{0};
  size_t uniqueStatusCount{0};
  bool hasSourceUnits{false};
  std::string sourceUnits{};
  bool hasMetersPerUnit{false};
  float metersPerUnit{1.0f};
  bool hasImportScale{false};
  float importScale{1.0f};
  bool hasEffectiveImportScale{false};
  float effectiveImportScale{1.0f};
  bool hasSourceUpAxis{false};
  std::string sourceUpAxis{};
  bool hasCoordinateOffset{false};
  glm::dvec3 coordinateOffset{0.0};
  std::string coordinateOffsetSource{};
  std::string crsName{};
  std::string crsAuthority{};
  std::string crsCode{};
  std::string mapConversionName{};
  std::span<const container::renderer::BimScheduleRow>
      scheduleByClassAndStoreyRows{};
  std::span<const container::renderer::BimScheduleRow>
      scheduleByTypeAndStoreyRows{};
  std::span<const container::renderer::BimScheduleRow>
      scheduleByMaterialRows{};
  std::span<const container::renderer::BimModelCompareElement>
      modelCompareElements{};
  bool hasSelectedCoordinateReadout{false};
  container::renderer::BimCoordinateReadout selectedCoordinateReadout{};
  bool hasOriginRebaseRecommendation{false};
  container::renderer::BimOriginRebaseRecommendation
      originRebaseRecommendation{};
  std::span<const std::string> elementTypes{};
  std::span<const std::string> elementStoreys{};
  std::span<const std::string> elementMaterials{};
  std::span<const std::string> elementDisciplines{};
  std::span<const std::string> elementPhases{};
  std::span<const std::string> elementFireRatings{};
  std::span<const std::string> elementLoadBearingValues{};
  std::span<const std::string> elementStatuses{};
  std::span<const container::renderer::BimStoreyRange> elementStoreyRanges{};
  const container::renderer::BimRelationshipGraph *relationshipGraph{nullptr};

  bool hasSelection{false};
  uint32_t selectedObjectIndex{0};
  uint32_t sourceElementIndex{0};
  uint32_t meshId{0};
  uint32_t sourceMaterialIndex{0};
  uint32_t materialIndex{0};
  uint32_t semanticTypeId{0};
  glm::vec4 sourceColor{0.8f, 0.82f, 0.86f, 1.0f};
  std::string guid{};
  std::string type{};
  std::string displayName{};
  std::string objectType{};
  std::string storeyName{};
  std::string storeyId{};
  std::string materialName{};
  std::string materialCategory{};
  std::string discipline{};
  std::string phase{};
  std::string fireRating{};
  std::string loadBearing{};
  std::string status{};
  std::string sourceId{};
  std::string geometryKind{};
  std::span<const container::renderer::BimElementProperty> properties{};
  bool transparent{false};
  bool doubleSided{false};
  bool hasSelectionBounds{false};
  glm::vec3 selectionBoundsMin{0.0f};
  glm::vec3 selectionBoundsMax{0.0f};
  glm::vec3 selectionBoundsCenter{0.0f};
  glm::vec3 selectionBoundsSize{0.0f};
  float selectionBoundsRadius{0.0f};
  float selectionFloorElevation{0.0f};
};

struct ViewpointSnapshotState {
  std::string label{};
  TransformControls camera{};
  uint32_t selectedMeshNode{std::numeric_limits<uint32_t>::max()};
  uint32_t selectedBimObjectIndex{std::numeric_limits<uint32_t>::max()};
  std::string selectedBimGuid{};
  std::string selectedBimType{};
  std::string selectedBimSourceId{};
  std::string bimModelPath{};
  BimFilterState bimFilter{};
  BimPhaseTimelineUiState phaseTimeline{};
};

struct SceneHierarchyParentCandidateRow {
  std::optional<uint32_t> parent{};
  std::string label{};
  bool selected{false};
};

struct SceneHierarchyParentCandidateCacheState {
  uint64_t revision{std::numeric_limits<uint64_t>::max()};
  size_t nodeCount{0};
  size_t rootCount{0};
  uint32_t nodeIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t currentParent{std::numeric_limits<uint32_t>::max()};
  std::vector<SceneHierarchyParentCandidateRow> rows{};
};

class GuiManager {
public:
  GuiManager() = default;
  ~GuiManager();

  void initialize(VkInstance instance, VkDevice device,
                  VkPhysicalDevice physicalDevice, VkQueue graphicsQueue,
                  uint32_t graphicsQueueFamily, RenderingPassHandle renderPass,
                  uint32_t imageCount, GLFWwindow *window,
                  const std::string &defaultModelPath,
                  float defaultImportScale);

  void shutdown(VkDevice device);
  void updateSwapchainImageCount(uint32_t imageCount);
  void setMsaaSampleState(std::span<const uint32_t> options,
                          uint32_t activeSamples);
  [[nodiscard]] std::optional<uint32_t> consumeMsaaSampleChange();
  void setRenderEngineOptions(
      std::vector<RenderEngineOption> options,
      container::renderer::RenderTechniqueId active);
  [[nodiscard]] std::optional<container::renderer::RenderTechniqueId>
  consumeRenderEngineChange();
  [[nodiscard]] container::renderer::RenderTechniqueId activeRenderEngine()
      const;
  void setRenderTechniqueDebugState(GuiRenderTechniqueDebugState state);
  [[nodiscard]] const GuiRenderTechniqueDebugState &
  renderTechniqueDebugState() const;
  [[nodiscard]] std::optional<ModelLoadRequest> consumeModelLoadRequest();
  // Shared by widgets and deterministic captures; consumed before NewFrame.
  void queueModelLoadRequest(std::string path, float importScale,
                             std::string label);
  [[nodiscard]] bool queueSampleModelLoadRequest(std::string_view label);

  void startFrame();
  void endFrame();
  void render(VkCommandBuffer commandBuffer);

  void drawViewportInteractionControls(
      const ViewportInteractionState &state,
      const std::function<void(ViewportTool)> &setTool,
      const std::function<void(TransformSpace)> &setTransformSpace,
      const std::function<void(TransformAxis)> &setTransformAxis,
      const std::function<void(ViewportNavigationStyle)> &setNavigationStyle,
      const std::function<void(bool)> &setTransformSnapEnabled);

  void drawViewportNavigationOverlay(
      const ViewportNavigationState &state,
      const std::function<void(CameraViewPreset)> &setViewPreset,
      const std::function<void(float, float)> &freeRotate,
      const std::function<void(float, float)> &panView,
      const std::function<void()> &toggleProjectionMode);

  void drawSceneControls(
      const container::scene::SceneGraph &sceneGraph,
      const std::function<void(container::renderer::ScenePrimitiveKind)>
          &addScenePrimitive,
      const TransformControls &cameraTransform,
      const std::function<void(const TransformControls &)>
          &applyCameraTransform,
      const TransformControls &sceneTransform,
      const std::function<void(const TransformControls &)> &applySceneTransform,
      const glm::vec3 &directionalLightPosition,
      const container::gpu::LightingData &lightingData,
      const std::vector<container::gpu::PointLightData> &pointLights,
      const std::vector<container::renderer::EditableLightEntity>
          &editableLights,
      container::renderer::EditableLightId selectedEditableLight,
      const std::function<void(container::renderer::EditableLightId)>
          &selectEditableLight,
      const std::function<void(const container::renderer::EditableLightEntity
                                   &)> &updateEditableLight,
      const std::function<void(container::renderer::EditableLightType)>
          &addManualEditableLight,
      uint32_t selectedMeshNode, const BimInspectionState &bimInspection,
      const ViewpointSnapshotState &currentViewpoint,
      const std::function<bool(const ViewpointSnapshotState &)>
          &restoreViewpoint,
      uint32_t rootSceneNode,
      const std::function<void(uint32_t)> &selectMeshNode,
      const std::function<void(uint32_t)> &focusSceneNode,
      const std::function<void(uint32_t, bool)> &setSceneNodeVisible,
      const std::function<void(uint32_t, std::optional<uint32_t>)>
          &reparentSceneNode,
      const TransformControls &meshTransform,
      const std::function<void(uint32_t, const TransformControls &)>
          &applyMeshTransform);

  [[nodiscard]] bool isCapturingInput() const;
  [[nodiscard]] bool isCapturingMouse() const;
  [[nodiscard]] bool isCapturingKeyboard() const;
  [[nodiscard]] SceneViewportMode sceneViewportMode() const {
    return sceneViewportMode_;
  }
  void setSceneViewportMode(SceneViewportMode mode) {
    sceneViewportMode_ = mode;
  }
  [[nodiscard]] bool renderPreviewMode() const {
    return sceneViewportMode_ == SceneViewportMode::RenderPreview;
  }
  [[nodiscard]] bool editorOverlaysEnabled() const {
    return sceneViewportMode_ == SceneViewportMode::Editor &&
           !renderPreviewMode();
  }
  [[nodiscard]] bool showGeometryOverlay() const {
    return editorOverlaysEnabled() && showGeometryOverlay_;
  }
  [[nodiscard]] bool showLightGizmos() const {
    return editorOverlaysEnabled() && showLightGizmos_;
  }
  [[nodiscard]] bool showNormalDiagCube() const {
    return editorOverlaysEnabled() && showNormalDiagCube_;
  }
  [[nodiscard]] bool showNormalValidation() const {
    return editorOverlaysEnabled() && normalValidationSettings_.enabled;
  }
  [[nodiscard]] const container::gpu::NormalValidationSettings &
  normalValidationSettings() const {
    return normalValidationSettings_;
  }
  [[nodiscard]] GBufferViewMode gBufferViewMode() const {
    return gBufferViewMode_;
  }
  void setGBufferViewMode(GBufferViewMode mode) { gBufferViewMode_ = mode; }
  [[nodiscard]] const WireframeSettings &wireframeSettings() const {
    return wireframeSettings_;
  }
  [[nodiscard]] const BimFilterState &bimFilterState() const {
    return bimFilterState_;
  }
  [[nodiscard]] const BimPhaseTimelineUiState &
  bimPhaseTimelineUiState() const {
    return bimPhaseTimelineUiState_;
  }
  [[nodiscard]] const BimFloorPlanOverlayState &
  bimFloorPlanOverlayState() const {
    return bimFloorPlanOverlayState_;
  }
  [[nodiscard]] const BimElevationViewState &bimElevationViewState() const {
    return bimElevationViewState_;
  }
  [[nodiscard]] std::optional<BimElevationViewRequest>
  consumeBimElevationViewRequest() {
    auto request = bimElevationViewRequest_;
    bimElevationViewRequest_.reset();
    return request;
  }
  [[nodiscard]] std::optional<BimDrawingExportUiState>
  consumeBimDrawingExportRequest() {
    if (!bimDrawingExportUiState_.exportRequested) {
      return std::nullopt;
    }
    BimDrawingExportUiState request = bimDrawingExportUiState_;
    bimDrawingExportUiState_.exportRequested = false;
    return request;
  }
  [[nodiscard]] const BimLayerVisibilityState &bimLayerVisibilityState() const {
    return bimLayerVisibilityState_;
  }
  [[nodiscard]] std::span<
      const container::renderer::BimCoordinationOverlayIssuePin>
  bimIssueOverlayPins() const;
  [[nodiscard]] container::renderer::BimSemanticColorMode
  bimSemanticColorMode() const {
    return bimSemanticColorMode_;
  }
  [[nodiscard]] const SectionPlaneState &sectionPlaneState() const {
    return sectionPlaneState_;
  }
  void setSectionPlaneState(const SectionPlaneState &state);
  void setSectionPlaneVisualEditable(bool editable);
  [[nodiscard]] const BimBoxClipUiState &bimBoxClipState() const {
    return bimBoxClipState_;
  }
  [[nodiscard]] const BimLodStreamingUiState &bimLodStreamingUiState() const {
    return bimLodStreamingUiState_;
  }
  [[nodiscard]] const BimClipCapHatchingUiState &
  bimClipCapHatchingUiState() const {
    return bimClipCapHatchingUiState_;
  }
  [[nodiscard]] bool wireframeSupported() const { return wireframeSupported_; }
  [[nodiscard]] const std::string &statusMessage() const {
    return statusMessage_;
  }
  [[nodiscard]] const std::string &environmentStatus() const {
    return environmentStatus_;
  }
  void setStatusMessage(std::string status) {
    statusMessage_ = std::move(status);
  }
  void setEnvironmentStatus(std::string status) {
    environmentStatus_ = std::move(status);
  }
  void setWireframeCapabilities(bool supported, bool rasterModeSupported,
                                bool wideLineSupported);
  void setCullStats(uint32_t total, uint32_t frustumPassed,
                    uint32_t occlusionPassed);
  void setLightCullingStats(const container::gpu::LightCullingStats &stats);
  void setRendererTelemetry(
      const container::renderer::RendererTelemetryView &telemetry);
  void setLightingSettings(const container::gpu::LightingSettings &settings);
  [[nodiscard]] const container::gpu::LightingSettings &
  lightingSettings() const {
    return lightingSettings_;
  }
  [[nodiscard]] const container::gpu::ShadowSettings& shadowSettings() const {
    return shadowSettings_;
  }
  void setAreaShadowQuality(uint32_t quality) {
    shadowSettings_.areaShadowQuality = std::min(quality, 3u);
  }
  void setLtcStatus(std::string status) { ltcStatus_ = std::move(status); }
  void setFreezeCulling(bool frozen);
  [[nodiscard]] container::renderer::RayShadowSettings &rayShadowSettings() {
    return rayShadowSettings_;
  }
  void setRayShadowSupport(bool supported, std::string status) {
    rayShadowSupported_ = supported;
    rayShadowStatus_ = std::move(status);
  }
  void setGfxCaptureStatus(std::string status) { gfxCaptureStatus_ = std::move(status); }
  [[nodiscard]] bool freezeCullingRequested() const { return freezeCulling_; }

  void setTemporalSettings(const container::temporal::Settings &settings,
                           uint64_t bytes, uint64_t frame, uint32_t epoch,
                           const std::string &reason, glm::uvec2 extent,
                           uint64_t allocatedBytes, float resolveGpuMs) {
    temporalSettings_ = settings;
    temporalBytes_ = bytes;
    temporalFrame_ = frame;
    temporalEpoch_ = epoch;
    temporalResetReason_ = reason;
    temporalExtent_ = extent;
    temporalAllocatedBytes_ = allocatedBytes;
    temporalResolveGpuMs_ = resolveGpuMs;
  }
  const container::temporal::Settings &temporalSettings() const {
    return temporalSettings_;
  }
  bool consumeTemporalReset() {
    return std::exchange(temporalResetRequested_, false);
  }
  // Bloom settings (bidirectional sync with BloomManager).
  void setBloomSettings(bool enabled, float threshold, float knee,
                        float intensity, float radius);
  [[nodiscard]] bool bloomEnabled() const { return bloomEnabled_; }
  [[nodiscard]] float bloomThreshold() const { return bloomThreshold_; }
  [[nodiscard]] float bloomKnee() const { return bloomKnee_; }
  [[nodiscard]] float bloomIntensity() const { return bloomIntensity_; }
  [[nodiscard]] float bloomRadius() const { return bloomRadius_; }
  [[nodiscard]] const container::gpu::ExposureSettings& exposureSettings() const {
    return exposureSettings_;
  }
  [[nodiscard]] float postProcessExposure() const {
    return exposureSettings_.manualExposure;
  }

  // Render pass toggles (bidirectional sync with RenderGraph).
  void setRenderPassList(const std::vector<RenderPassToggle> &passes);
  [[nodiscard]] std::vector<RenderPassToggle> &renderPassToggles() {
    return renderPassToggles_;
  }
  [[nodiscard]] const std::vector<RenderPassToggle> &renderPassToggles() const {
    return renderPassToggles_;
  }

private:
  container::temporal::Settings temporalSettings_{};
  std::string gfxCaptureStatus_{"Disabled. Launch with tools/gfxreconstruct.ps1 capture to collect Vulkan captures and runtime metadata."};
  uint64_t temporalBytes_{0}, temporalFrame_{0};
  glm::uvec2 temporalExtent_{};
  uint64_t temporalAllocatedBytes_{0};
  float temporalResolveGpuMs_{0};
  uint32_t temporalEpoch_{0};
  std::string temporalResetReason_{};
  bool temporalResetRequested_{false};
  void ensureInitialized() const;
  void discoverSampleModels();
  void drawRendererTelemetryWindow();
  [[nodiscard]] int sampleModelIndexForPath(const std::string &path) const;

  struct SampleModelOption {
    std::string label;
    std::string path;
  };

  struct BimMeasurementPointState {
    bool captured{false};
    glm::vec3 center{0.0f};
    uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
    std::string label{};
    std::string modelPath{};
    container::renderer::BimSnapKind snapKind{};
  };

  struct BimMeasurementAnnotationState {
    uint32_t id{0};
    std::string label{};
    BimMeasurementPointState pointA{};
    BimMeasurementPointState pointB{};
    BimMeasurementPointState pointC{};
    bool hasPointC{false};
    float distance{0.0f};
    float horizontalDistance{0.0f};
    float elevationDelta{0.0f};
    float slopeAngleDegrees{0.0f};
    float elevationAxisAngleDegrees{0.0f};
    float angleDegrees{0.0f};
    float polygonArea{0.0f};
  };

  struct BimSelectionSetMemberState {
    std::string label{};
    std::string type{};
    std::string storey{};
    std::string material{};
    ViewpointSnapshotState snapshot{};
  };

  struct BimSelectionSetState {
    uint32_t id{0};
    std::string label{};
    std::string modelPath{};
    std::vector<BimSelectionSetMemberState> members{};
  };

  struct BcfTopicArchiveEntryState {
    uint32_t id{0};
    std::string label{};
    std::string status{};
    std::string priority{};
    std::string path{};
    std::vector<container::renderer::BimCoordinationOverlayIssuePin>
        issuePins{};
    bool hasSnapshot{false};
    ViewpointSnapshotState snapshot{};
  };

  void applyBimElevationDisplayIntent();

  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  bool initialized_{false};
  bool imguiFrameOpen_{false};
  bool showGeometryOverlay_{false};
  bool showLightGizmos_{true};
  bool showNormalDiagCube_{false};
  bool wireframeSupported_{false};
  bool wireframeRasterModeSupported_{false};
  bool wireframeWideLineSupported_{false};
  GBufferViewMode gBufferViewMode_{GBufferViewMode::Overview};
  SceneViewportMode sceneViewportMode_{SceneViewportMode::Editor};
  WireframeSettings wireframeSettings_{};
  container::gpu::NormalValidationSettings normalValidationSettings_{};
  std::string modelPathInput_{};
  std::string defaultModelPath_{};
  std::vector<SampleModelOption> sampleModelOptions_;
  int selectedSampleModelIndex_{-1};
  int importScaleIndex_{0};
  float importScale_{1.0f};
  std::string statusMessage_{};
  std::string environmentStatus_{};
  std::vector<uint32_t> msaaSampleOptions_{1u};
  uint32_t msaaSamples_{1u};
  std::optional<uint32_t> pendingMsaaSamples_{};
  std::vector<RenderEngineOption> renderEngineOptions_{};
  container::renderer::RenderTechniqueId activeRenderEngine_{};
  std::optional<container::renderer::RenderTechniqueId>
      pendingRenderEngineChange_{};
  std::optional<ModelLoadRequest> pendingModelLoadRequest_{};
  GuiRenderTechniqueDebugState renderTechniqueDebugState_{};
  uint32_t cullStatsTotal_{0};
  uint32_t cullStatsFrustum_{0};
  uint32_t cullStatsOcclusion_{0};
  container::gpu::LightCullingStats lightCullingStats_{};
  GuiRendererTelemetryView rendererTelemetry_{};
  container::gpu::LightingSettings lightingSettings_{};
  std::string ltcStatus_{};
  container::gpu::ShadowSettings shadowSettings_{};
  container::renderer::RayShadowSettings rayShadowSettings_{};
  bool rayShadowSupported_{false};
  std::string rayShadowStatus_{};
  bool freezeCulling_{false};
  bool bloomEnabled_{true};
  float bloomThreshold_{1.0f};
  float bloomKnee_{0.1f};
  float bloomIntensity_{0.3f};
  float bloomRadius_{1.0f};
  container::gpu::ExposureSettings exposureSettings_{};
  std::vector<RenderPassToggle> renderPassToggles_;
  BimFilterState bimFilterState_{};
  BimPhaseTimelineUiState bimPhaseTimelineUiState_{};
  BimFloorPlanOverlayState bimFloorPlanOverlayState_{};
  BimElevationViewState bimElevationViewState_{};
  std::optional<BimElevationViewRequest> bimElevationViewRequest_{};
  BimDrawingExportUiState bimDrawingExportUiState_{};
  BimLayerVisibilityState bimLayerVisibilityState_{};
  std::string bimQuickFilterSearch_{};
  std::string bimPropertySearch_{};
  std::string bimRelationshipSearch_{};
  container::renderer::BimSemanticColorMode bimSemanticColorMode_{};
  SectionPlaneState sectionPlaneState_{};
  BimBoxClipUiState bimBoxClipState_{};
  int sectionPlaneAxis_{1};
  int selectedBimStoreyRangeIndex_{-1};
  std::string bimSelectionSetNameInput_{"Selection Set"};
  std::vector<BimSelectionSetState> bimSelectionSets_{};
  int selectedBimSelectionSetIndex_{-1};
  uint32_t nextBimSelectionSetId_{1};
  std::vector<ViewpointSnapshotState> viewpointSnapshots_{};
  int selectedViewpointSnapshotIndex_{-1};
  uint32_t nextViewpointSnapshotId_{1};
  std::string bcfViewpointPath_{"container-viewpoint.bcfv"};
  std::string bcfTopicFolderPath_{"container-bcf-topic"};
  std::string bcfTopicArchivePath_{"container-topic.bcfzip"};
  std::string bcfTopicTitleInput_{"BIM issue"};
  std::string bcfTopicStatusInput_{"Open"};
  std::string bcfTopicPriorityInput_{};
  std::string bcfTopicLabelsInput_{};
  std::string bcfTopicCommentInput_{};
  std::vector<BcfTopicArchiveEntryState> bcfTopicArchiveEntries_{};
  std::vector<container::renderer::BimCoordinationOverlayIssuePin>
      bcfIssueOverlayPins_{};
  int selectedBcfTopicArchiveIndex_{-1};
  uint32_t nextBcfTopicArchiveId_{1};
  std::vector<container::renderer::BimModelCompareElement>
      bimCompareBaseline_{};
  std::string bimCompareBaselineModelPath_{};
  std::vector<container::renderer::BimModelCompareChange>
      bimCompareChanges_{};
  SceneHierarchyParentCandidateCacheState sceneHierarchyParentCandidateCache_{};
  BimLodStreamingUiState bimLodStreamingUiState_{};
  BimClipCapHatchingUiState bimClipCapHatchingUiState_{};
  std::string bimMeasurementModelPath_{};
  float bimMeasurementEffectiveImportScale_{1.0f};
  size_t bimMeasurementObjectCount_{0};
  BimMeasurementSnapUiState bimMeasurementSnapState_{};
  BimMeasurementPointState bimMeasurementPointA_{};
  BimMeasurementPointState bimMeasurementPointB_{};
  BimMeasurementPointState bimMeasurementPointC_{};
  std::vector<BimMeasurementAnnotationState> bimMeasurementAnnotations_{};
  uint32_t nextBimMeasurementAnnotationId_{1};
  int selectedBimMeasurementAnnotationIndex_{-1};
  bool viewportNavFreeRotateActive_{false};
  bool viewportNavPanActive_{false};
};

} // namespace container::ui
