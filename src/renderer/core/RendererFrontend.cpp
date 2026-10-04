#include "Container/renderer/core/RendererFrontend.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include <fstream>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "Container/app/AppConfig.h"
#include "Container/ecs/World.h"
#include "Container/renderer/bim/BimDrawingExport.h"
#include "Container/renderer/bim/BimFrameDrawRoutingPlanner.h"
#include "Container/renderer/bim/BimGeoreferenceTransform.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimModelCompare.h"
#include "Container/renderer/bim/BimScheduleExtractor.h"
#include "Container/renderer/core/FrameConcurrencyPolicy.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderExtraction.h"
#include "Container/renderer/core/RenderPassGpuProfiler.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/RendererMsaa.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/debug/DebugUiPresenter.h"
#include "Container/renderer/deferred/DeferredLightGizmoPlanner.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/effects/BloomManager.h"
#include "Container/renderer/effects/ExposureManager.h"
#include "Container/renderer/effects/OitManager.h"
#include "Container/renderer/lighting/EnvironmentManager.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/picking/RenderSurfaceInteractionController.h"
#include "Container/renderer/pipeline/GraphicsPipelineBuilder.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/resources/CommandBufferManager.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/CameraController.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/ScenePrimitives.h"
#include "Container/renderer/scene/SceneProviderSynchronizer.h"
#include "Container/renderer/shadow/ShadowCullManager.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/shadow/ShadowPipelineBridge.h"
#include "Container/renderer/shadow/ShadowResourceBridge.h"
#include "Container/scene/MeshSceneProviderAssetAdapter.h"
#include "Container/scene/SceneProvider.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/DebugMessengerExt.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/Logger.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#include "stb_image_write.h"

namespace container::renderer {

using container::gpu::CameraData;
using container::gpu::LightingData;

namespace {

using TelemetryClock = std::chrono::steady_clock;

class GuiFrameExceptionGuard {
public:
  explicit GuiFrameExceptionGuard(container::ui::GuiManager &gui)
      : gui_(gui), uncaughtOnEntry_(std::uncaught_exceptions()) {}

  ~GuiFrameExceptionGuard() {
    if (std::uncaught_exceptions() > uncaughtOnEntry_) {
      gui_.endFrame();
    }
  }

  GuiFrameExceptionGuard(const GuiFrameExceptionGuard &) = delete;
  GuiFrameExceptionGuard &operator=(const GuiFrameExceptionGuard &) = delete;

private:
  container::ui::GuiManager &gui_;
  int uncaughtOnEntry_{0};
};

[[nodiscard]] bool submitReadbackCommandBufferAndWait(
    VkDevice device, VkQueue queue, VkCommandBuffer commandBuffer) {
  if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
      commandBuffer == VK_NULL_HANDLE) {
    return false;
  }

  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence{VK_NULL_HANDLE};
  if (createOwnedFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
    return false;
  }

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  const VkResult submitResult = vkQueueSubmit(queue, 1, &submitInfo, fence);
  const VkResult waitResult =
      submitResult == VK_SUCCESS
          ? vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX)
          : submitResult;
  destroyOwnedFence(device, fence, nullptr);
  return submitResult == VK_SUCCESS && waitResult == VK_SUCCESS;
}

[[nodiscard]] std::string bimFloorPlanDrawingViewName(bool sourceElevation) {
  return sourceElevation ? "Floor plan (source elevation)"
                         : "Floor plan (projected ground)";
}

[[nodiscard]] std::string bimDrawingTitle(const BimManager *bimManager) {
  if (bimManager == nullptr || bimManager->modelPath().empty()) {
    return "BIM drawing";
  }
  const std::filesystem::path modelPath =
      container::util::pathFromUtf8(bimManager->modelPath());
  const std::string filename = container::util::pathToUtf8(modelPath.filename());
  return filename.empty() ? "BIM drawing" : filename;
}

[[nodiscard]] BimScheduleBounds
bimScheduleBounds(const BimElementBounds &bounds) {
  return {.valid = bounds.valid, .min = bounds.min, .max = bounds.max};
}

[[nodiscard]] BimModelCompareBounds
bimModelCompareBounds(const BimElementBounds &bounds) {
  return {.valid = bounds.valid, .min = bounds.min, .max = bounds.max};
}

[[nodiscard]] std::vector<BimScheduleElement> buildBimScheduleElements(
    std::span<const BimElementMetadata> metadata) {
  std::vector<BimScheduleElement> elements;
  elements.reserve(metadata.size());
  for (const BimElementMetadata &element : metadata) {
    elements.push_back({.guid = element.guid,
                        .sourceId = element.sourceId,
                        .ifcClass = element.type,
                        .type = element.objectType,
                        .storey = !element.storeyName.empty()
                                      ? element.storeyName
                                      : element.storeyId,
                        .material = !element.materialName.empty()
                                        ? element.materialName
                                        : element.materialCategory,
                        .bounds = bimScheduleBounds(element.bounds)});
  }
  return elements;
}

[[nodiscard]] std::vector<BimModelCompareElement> buildBimModelCompareElements(
    std::span<const BimElementMetadata> metadata) {
  std::vector<BimModelCompareElement> elements;
  elements.reserve(metadata.size());
  for (const BimElementMetadata &element : metadata) {
    elements.push_back({.guid = element.guid,
                        .sourceId = element.sourceId,
                        .ifcClass = element.type,
                        .type = element.objectType,
                        .storey = !element.storeyName.empty()
                                      ? element.storeyName
                                      : element.storeyId,
                        .material = !element.materialName.empty()
                                        ? element.materialName
                                        : element.materialCategory,
                        .bounds = bimModelCompareBounds(element.bounds)});
  }
  return elements;
}

[[nodiscard]] BimGeoreferenceMetadata buildBimGeoreferenceMetadata(
    const BimModelUnitMetadata &unitMetadata,
    const BimModelGeoreferenceMetadata &georeferenceMetadata) {
  BimGeoreferenceMetadata metadata{};
  metadata.hasMetersPerUnit = unitMetadata.hasMetersPerUnit;
  metadata.metersPerUnit = unitMetadata.metersPerUnit;
  metadata.hasEffectiveImportScale = unitMetadata.hasEffectiveImportScale;
  metadata.effectiveImportScale = unitMetadata.effectiveImportScale;
  metadata.hasSourceUpAxis = georeferenceMetadata.hasSourceUpAxis;
  metadata.sourceUpAxis = georeferenceMetadata.sourceUpAxis;
  metadata.hasRebaseOffset = georeferenceMetadata.hasCoordinateOffset;
  metadata.rebaseOffset = georeferenceMetadata.coordinateOffset;
  metadata.crsAuthority = georeferenceMetadata.crsAuthority;
  metadata.crsCode = georeferenceMetadata.crsCode;
  metadata.crsName = georeferenceMetadata.crsName;
  metadata.mapConversionLabel = georeferenceMetadata.mapConversionName;
  return metadata;
}

[[nodiscard]] bool hasBimGeoreferenceReadoutMetadata(
    const BimGeoreferenceMetadata &metadata) {
  return metadata.hasMetersPerUnit || metadata.hasEffectiveImportScale ||
         metadata.hasProjectOrigin || metadata.hasSourceUpAxis ||
         metadata.hasRebaseOffset || metadata.hasSurveyOffset ||
         !metadata.crsAuthority.empty() ||
         !metadata.crsCode.empty() || !metadata.crsName.empty() ||
         !metadata.mapConversionLabel.empty();
}

float elapsedMilliseconds(
    TelemetryClock::time_point start,
    TelemetryClock::time_point end = TelemetryClock::now()) {
  return std::chrono::duration<float, std::milli>(end - start).count();
}

uint32_t saturatingU32(size_t value) {
  return static_cast<uint32_t>(
      std::min<size_t>(value, std::numeric_limits<uint32_t>::max()));
}

constexpr uint32_t kEditableLightTransformNode =
    std::numeric_limits<uint32_t>::max() - 1u;
constexpr uint32_t kSectionPlaneTransformNode =
    std::numeric_limits<uint32_t>::max() - 2u;

const FrameResourceBinding *
deferredRasterRuntimeBinding(const FrameResourceManager *manager,
                             uint32_t imageIndex, std::string_view name) {
  if (manager == nullptr || imageIndex >= manager->frameCount()) {
    return nullptr;
  }
  return manager->resourceRegistry().findBinding(
      TechniqueResourceKey{.technique = RenderTechniqueId::DeferredRaster,
                           .name = std::string(name)},
      imageIndex);
}

const FrameImageBinding *
deferredRasterRuntimeImageBinding(const FrameResourceManager *manager,
                                  uint32_t imageIndex, std::string_view name) {
  const FrameResourceBinding *binding =
      deferredRasterRuntimeBinding(manager, imageIndex, name);
  return binding != nullptr && binding->kind == FrameResourceKind::Image
             ? &binding->image
             : nullptr;
}

VkImage deferredRasterRuntimeImage(const FrameResourceManager *manager,
                                   uint32_t imageIndex, std::string_view name) {
  const FrameImageBinding *binding =
      deferredRasterRuntimeImageBinding(manager, imageIndex, name);
  return binding != nullptr ? binding->image : VK_NULL_HANDLE;
}

const FrameBufferBinding *
deferredRasterRuntimeBufferBinding(const FrameResourceManager *manager,
                                   uint32_t imageIndex, std::string_view name) {
  const FrameResourceBinding *binding =
      deferredRasterRuntimeBinding(manager, imageIndex, name);
  return binding != nullptr && binding->kind == FrameResourceKind::Buffer
             ? &binding->buffer
             : nullptr;
}

uint32_t
deferredRasterRuntimeOitNodeCapacity(const FrameResourceManager *manager,
                                     uint32_t imageIndex) {
  const FrameBufferBinding *binding = deferredRasterRuntimeBufferBinding(
      manager, imageIndex, "oit-node-buffer");
  if (binding == nullptr || binding->size == 0) {
    return 0;
  }
  const VkDeviceSize capacity = binding->size / sizeof(OitNode);
  return static_cast<uint32_t>(
      std::min<VkDeviceSize>(capacity, std::numeric_limits<uint32_t>::max()));
}

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

bool isAuxiliaryRenderModelPath(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos) {
    return false;
  }

  const std::string extension = lowerAscii(path.substr(dot));
  return extension == ".bim" || extension == ".ifc" || extension == ".ifcx" ||
         extension == ".usd" || extension == ".usda" || extension == ".usdc" ||
         extension == ".usdz";
}

glm::vec3 arrayToVec3(const std::array<float, 3> &value) {
  return {value[0], value[1], value[2]};
}

container::gpu::ExposureSettings
exposureSettingsFromConfig(const container::app::AppConfig &config) {
  container::gpu::ExposureSettings settings{};
  if (config.hasManualExposureOverride) {
    settings.mode = container::gpu::kExposureModeManual;
    settings.manualExposure = std::max(config.manualExposure, 0.0f);
  }
  return settings;
}

std::optional<container::ui::GBufferViewMode>
displayModeFromName(std::string_view value) {
  const std::string mode = lowerAscii(value);
  if (mode == "taa-velocity")
    return container::ui::GBufferViewMode::TemporalVelocity;
  if (mode == "taa-age")
    return container::ui::GBufferViewMode::TemporalHistoryAge;
  if (mode == "taa-rejection")
    return container::ui::GBufferViewMode::TemporalRejection;
  if (mode == "taa-blend")
    return container::ui::GBufferViewMode::TemporalBlend;
  if (mode == "taa-reactive")
    return container::ui::GBufferViewMode::TemporalReactive;
  if (mode.empty()) {
    return std::nullopt;
  }
  if (mode == "lit" || mode == "final-lit" || mode == "final_lit") {
    return container::ui::GBufferViewMode::Lit;
  }
  if (mode == "albedo" || mode == "base-color" || mode == "base_color") {
    return container::ui::GBufferViewMode::Albedo;
  }
  if (mode == "normals" || mode == "normal") {
    return container::ui::GBufferViewMode::Normals;
  }
  if (mode == "material" || mode == "materials") {
    return container::ui::GBufferViewMode::Material;
  }
  if (mode == "depth") {
    return container::ui::GBufferViewMode::Depth;
  }
  if (mode == "emissive") {
    return container::ui::GBufferViewMode::Emissive;
  }
  if (mode == "transparency") {
    return container::ui::GBufferViewMode::Transparency;
  }
  if (mode == "revealage") {
    return container::ui::GBufferViewMode::Revealage;
  }
  if (mode == "overview") {
    return container::ui::GBufferViewMode::Overview;
  }
  if (mode == "surface-normals" || mode == "surface_normals") {
    return container::ui::GBufferViewMode::SurfaceNormals;
  }
  if (mode == "object-space-normals" || mode == "object_space_normals") {
    return container::ui::GBufferViewMode::ObjectSpaceNormals;
  }
  if (mode == "shadow-cascades" || mode == "shadow_cascades") {
    return container::ui::GBufferViewMode::ShadowCascades;
  }
  if (mode == "tile-light-heat-map" || mode == "tile_light_heat_map" ||
      mode == "tile-light-heatmap") {
    return container::ui::GBufferViewMode::TileLightHeatMap;
  }
  if (mode == "shadow-texel-density" || mode == "shadow_texel_density") {
    return container::ui::GBufferViewMode::ShadowTexelDensity;
  }
  return std::nullopt;
}

container::ui::GBufferViewMode
configuredDisplayMode(const container::app::AppConfig &config) {
  if (config.displayModeOverride.empty()) {
    return container::ui::GBufferViewMode::Overview;
  }
  if (auto mode = displayModeFromName(config.displayModeOverride)) {
    return *mode;
  }
  throw std::runtime_error("unknown display mode override: " +
                           config.displayModeOverride);
}

void applyCameraOverride(container::scene::BaseCamera *camera,
                         const container::app::AppConfig &config) {
  if (!camera || !config.hasCameraOverride) {
    return;
  }

  const glm::vec3 position = arrayToVec3(config.cameraPosition);
  const glm::vec3 target = arrayToVec3(config.cameraTarget);
  glm::vec3 forward = target - position;
  const float length = glm::length(forward);
  if (length <= 0.0001f) {
    return;
  }
  forward /= length;

  const float pitchDegrees =
      glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
  const float yawDegrees = glm::degrees(std::atan2(-forward.z, forward.x));
  camera->setPosition(position);
  camera->setYawPitch(yawDegrees, pitchDegrees);
  if (auto *perspective =
          dynamic_cast<container::scene::PerspectiveCamera *>(camera)) {
    perspective->setFieldOfView(
        std::clamp(config.cameraVerticalFovDegrees, 1.0f, 179.0f));
  }
}

bool isSupportedScreenshotFormat(VkFormat format) {
  switch (format) {
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_R8G8B8A8_UNORM:
    return true;
  default:
    return false;
  }
}

std::vector<unsigned char> convertSwapchainBytesToRgba(const unsigned char *src,
                                                       size_t pixelCount,
                                                       VkFormat format) {
  std::vector<unsigned char> rgba(pixelCount * 4u);
  const bool bgra =
      format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
  for (size_t i = 0; i < pixelCount; ++i) {
    const unsigned char c0 = src[i * 4u + 0u];
    const unsigned char c1 = src[i * 4u + 1u];
    const unsigned char c2 = src[i * 4u + 2u];
    const unsigned char c3 = src[i * 4u + 3u];
    rgba[i * 4u + 0u] = bgra ? c2 : c0;
    rgba[i * 4u + 1u] = c1;
    rgba[i * 4u + 2u] = bgra ? c0 : c2;
    rgba[i * 4u + 3u] = c3;
  }
  return rgba;
}

bool decodeDepthReadbackValue(VkFormat format, const void *data,
                              float &outDepth) {
  if (data == nullptr) {
    return false;
  }

  if (format == VK_FORMAT_D32_SFLOAT ||
      format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
    float depth = 0.0f;
    std::memcpy(&depth, data, sizeof(depth));
    if (!std::isfinite(depth)) {
      return false;
    }
    outDepth = std::clamp(depth, 0.0f, 1.0f);
    return true;
  }

  if (format == VK_FORMAT_D24_UNORM_S8_UINT) {
    uint32_t packed = 0u;
    std::memcpy(&packed, data, sizeof(packed));
    outDepth = static_cast<float>(packed & 0x00ffffffu) /
               static_cast<float>(0x00ffffffu);
    return true;
  }

  return false;
}

bool depthHitVisible(float hitDepth, float sampledDepth) {
  constexpr float kDepthVisibilityTolerance = 0.0005f;
  // Reverse-Z stores nearer surfaces as larger values.
  return sampledDepth <= hitDepth + kDepthVisibilityTolerance;
}

bool hasTransparentCommands(const std::vector<DrawCommand> &draws,
                            const std::vector<DrawCommand> &singleSided,
                            const std::vector<DrawCommand> &windingFlipped,
                            const std::vector<DrawCommand> &doubleSided) {
  return !draws.empty() || !singleSided.empty() || !windingFlipped.empty() ||
         !doubleSided.empty();
}

bool hasTransparentGeometry(const BimGeometryDrawLists &draws) {
  return hasTransparentCommands(draws.transparentDrawCommands,
                                draws.transparentSingleSidedDrawCommands,
                                draws.transparentWindingFlippedDrawCommands,
                                draws.transparentDoubleSidedDrawCommands);
}

bool hasAnyGeometry(const BimGeometryDrawLists &draws) {
  return !draws.opaqueDrawCommands.empty() ||
         !draws.opaqueSingleSidedDrawCommands.empty() ||
         !draws.opaqueWindingFlippedDrawCommands.empty() ||
         !draws.opaqueDoubleSidedDrawCommands.empty() ||
         hasTransparentGeometry(draws);
}

bool hasTransparentGeometry(const BimDrawLists &draws, bool includePoints,
                            bool includeCurves) {
  if (hasTransparentCommands(draws.transparentDrawCommands,
                             draws.transparentSingleSidedDrawCommands,
                             draws.transparentWindingFlippedDrawCommands,
                             draws.transparentDoubleSidedDrawCommands)) {
    return true;
  }
  return (includePoints && (hasTransparentGeometry(draws.points) ||
                            hasTransparentGeometry(draws.nativePoints))) ||
         (includeCurves && (hasTransparentGeometry(draws.curves) ||
                            hasTransparentGeometry(draws.nativeCurves)));
}

bool hasTransparentBimSurfaceGeometry(const BimDrawLists &draws,
                                      bool includePoints, bool includeCurves) {
  if (hasTransparentCommands(draws.transparentDrawCommands,
                             draws.transparentSingleSidedDrawCommands,
                             draws.transparentWindingFlippedDrawCommands,
                             draws.transparentDoubleSidedDrawCommands)) {
    return true;
  }
  return (includePoints && hasTransparentGeometry(draws.points)) ||
         (includeCurves && hasTransparentGeometry(draws.curves));
}

bool hasTransparentBimGeometry(const BimManager &bimManager, bool includePoints,
                               bool includeCurves) {
  if (hasTransparentCommands(bimManager.transparentDrawCommands(),
                             bimManager.transparentSingleSidedDrawCommands(),
                             bimManager.transparentWindingFlippedDrawCommands(),
                             bimManager.transparentDoubleSidedDrawCommands())) {
    return true;
  }
  return (includePoints &&
          (hasTransparentGeometry(bimManager.pointDrawLists()) ||
           hasTransparentGeometry(bimManager.nativePointDrawLists()))) ||
         (includeCurves &&
          (hasTransparentGeometry(bimManager.curveDrawLists()) ||
           hasTransparentGeometry(bimManager.nativeCurveDrawLists())));
}

bool hasTransparentBimSurfaceGeometry(const BimManager& bimManager,
                                      bool includePoints, bool includeCurves) {
  if (hasTransparentCommands(bimManager.transparentDrawCommands(),
                             bimManager.transparentSingleSidedDrawCommands(),
                             bimManager.transparentWindingFlippedDrawCommands(),
                             bimManager.transparentDoubleSidedDrawCommands())) {
    return true;
  }
  return (includePoints &&
          hasTransparentGeometry(bimManager.pointDrawLists())) ||
         (includeCurves && hasTransparentGeometry(bimManager.curveDrawLists()));
}

BimFrameGpuVisibilityInputs bimFrameGpuVisibilityInputs(
    const BimManager &bimManager, const BimDrawFilter &filter) {
  const BimVisibilityFilterStats &stats = bimManager.visibilityFilterStats();
  const bool gpuCompatibleFilter = !filter.requiresCpuFiltering();
  return {.filterActive = filter.active(),
          .gpuResident = stats.gpuResident && gpuCompatibleFilter,
          .computeReady = stats.computeReady && gpuCompatibleFilter,
          .objectCount = stats.objectCount,
          .visibilityMaskReady =
              bimManager.visibilityMaskBuffer().buffer != VK_NULL_HANDLE};
}

BimFrameMeshDrawLists bimFrameMeshDrawLists(const BimManager &bimManager) {
  return {.opaqueDrawCommands = &bimManager.opaqueDrawCommands(),
          .opaqueSingleSidedDrawCommands =
              &bimManager.opaqueSingleSidedDrawCommands(),
          .opaqueWindingFlippedDrawCommands =
              &bimManager.opaqueWindingFlippedDrawCommands(),
          .opaqueDoubleSidedDrawCommands =
              &bimManager.opaqueDoubleSidedDrawCommands(),
          .transparentDrawCommands = &bimManager.transparentDrawCommands(),
          .transparentSingleSidedDrawCommands =
              &bimManager.transparentSingleSidedDrawCommands(),
          .transparentWindingFlippedDrawCommands =
              &bimManager.transparentWindingFlippedDrawCommands(),
          .transparentDoubleSidedDrawCommands =
              &bimManager.transparentDoubleSidedDrawCommands()};
}

BimFrameDrawRoutingInputs
bimFrameDrawRoutingInputs(const BimManager &bimManager,
                          const BimDrawFilter &filter,
                          const container::ui::BimLayerVisibilityState &layers,
                          const BimDrawLists *cpuFilteredDraws) {
  return {.gpuVisibility = bimFrameGpuVisibilityInputs(bimManager, filter),
          .pointCloudVisible = layers.pointCloudVisible,
          .curvesVisible = layers.curvesVisible,
          .unfilteredMeshDraws = bimFrameMeshDrawLists(bimManager),
          .unfilteredPointDraws = &bimManager.pointDrawLists(),
          .unfilteredCurveDraws = &bimManager.curveDrawLists(),
          .unfilteredNativePointDraws = &bimManager.nativePointDrawLists(),
          .unfilteredNativeCurveDraws = &bimManager.nativeCurveDrawLists(),
          .cpuFilteredDraws = cpuFilteredDraws};
}

void assignFrameDrawLists(FrameDrawLists &target,
                          const BimFrameMeshDrawLists &source) {
  target.opaqueDrawCommands = source.opaqueDrawCommands;
  target.opaqueSingleSidedDrawCommands = source.opaqueSingleSidedDrawCommands;
  target.opaqueWindingFlippedDrawCommands =
      source.opaqueWindingFlippedDrawCommands;
  target.opaqueDoubleSidedDrawCommands = source.opaqueDoubleSidedDrawCommands;
  target.transparentDrawCommands = source.transparentDrawCommands;
  target.transparentSingleSidedDrawCommands =
      source.transparentSingleSidedDrawCommands;
  target.transparentWindingFlippedDrawCommands =
      source.transparentWindingFlippedDrawCommands;
  target.transparentDoubleSidedDrawCommands =
      source.transparentDoubleSidedDrawCommands;
}

void assignFrameDrawLists(FrameDrawLists &target,
                          const BimGeometryDrawLists &source) {
  target.opaqueDrawCommands = &source.opaqueDrawCommands;
  target.opaqueSingleSidedDrawCommands = &source.opaqueSingleSidedDrawCommands;
  target.opaqueWindingFlippedDrawCommands =
      &source.opaqueWindingFlippedDrawCommands;
  target.opaqueDoubleSidedDrawCommands = &source.opaqueDoubleSidedDrawCommands;
  target.transparentDrawCommands = &source.transparentDrawCommands;
  target.transparentSingleSidedDrawCommands =
      &source.transparentSingleSidedDrawCommands;
  target.transparentWindingFlippedDrawCommands =
      &source.transparentWindingFlippedDrawCommands;
  target.transparentDoubleSidedDrawCommands =
      &source.transparentDoubleSidedDrawCommands;
}

bool isFiniteVec3(const glm::vec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

glm::vec3 normalizedOr(const glm::vec3 &value, const glm::vec3 &fallback) {
  const float length = glm::length(value);
  if (!std::isfinite(length) || length <= 0.0001f) {
    return fallback;
  }
  return value / length;
}

void applyConfiguredDirectionalLightOverrides(
    container::gpu::LightingData &lightingData,
    const container::app::AppConfig &config) {
  if (config.hasDirectionalDirectionOverride) {
    const glm::vec3 direction = arrayToVec3(config.directionalDirection);
    if (isFiniteVec3(direction)) {
      lightingData.directionalDirection =
          glm::vec4(normalizedOr(direction,
                                 glm::vec3(lightingData.directionalDirection)),
                    0.0f);
    }
  }
  if (config.hasDirectionalColorOverride) {
    const glm::vec3 color = arrayToVec3(config.directionalColor);
    if (isFiniteVec3(color)) {
      lightingData.directionalColorIntensity =
          glm::vec4(glm::max(color, glm::vec3(0.0f)),
                    lightingData.directionalColorIntensity.a);
    }
  }
  if (config.hasDirectionalIntensityOverride &&
      std::isfinite(config.directionalIntensity)) {
    lightingData.directionalColorIntensity.a =
        std::max(config.directionalIntensity, 0.0f);
  }
}

void applyConfiguredLightingOverrides(
    LightingManager *lightingManager, const container::app::AppConfig &config) {
  if (lightingManager == nullptr) {
    return;
  }
  applyConfiguredDirectionalLightOverrides(lightingManager->lightingData(),
                                           config);
}

glm::vec4
sectionPlaneEquation(const container::ui::SectionPlaneState &sectionPlane) {
  const glm::vec3 normal =
      normalizedOr(sectionPlane.normal, {0.0f, 1.0f, 0.0f});
  return {normal, -sectionPlane.offset};
}

glm::vec3 sectionPlaneOrigin(
    const container::ui::SectionPlaneState &sectionPlane) {
  const glm::vec3 normal =
      normalizedOr(sectionPlane.normal, {0.0f, 1.0f, 0.0f});
  return normal * sectionPlane.offset;
}

glm::vec3 rotateVectorAroundAxis(const glm::vec3 &value,
                                 const glm::vec3 &axis, float degrees) {
  const glm::vec3 unitAxis = normalizedOr(axis, {0.0f, 1.0f, 0.0f});
  const float radians = degrees * 0.017453292519943295f;
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  return value * c + glm::cross(unitAxis, value) * s +
         unitAxis * glm::dot(unitAxis, value) * (1.0f - c);
}

void sectionPlaneBasis(const glm::vec3 &normal, glm::vec3 &axisX,
                       glm::vec3 &axisY, glm::vec3 &axisZ) {
  axisY = normalizedOr(normal, {0.0f, 1.0f, 0.0f});
  const glm::vec3 helper =
      std::abs(axisY.y) < 0.9f ? glm::vec3{0.0f, 1.0f, 0.0f}
                               : glm::vec3{1.0f, 0.0f, 0.0f};
  axisX = normalizedOr(glm::cross(helper, axisY), {1.0f, 0.0f, 0.0f});
  axisZ = normalizedOr(glm::cross(axisX, axisY), {0.0f, 0.0f, 1.0f});
}

std::array<glm::vec4, 6>
makeBoxClipPlanes(const container::ui::BimBoxClipUiState &boxClip) {
  const glm::vec3 minBounds = glm::min(boxClip.min, boxClip.max);
  const glm::vec3 maxBounds = glm::max(boxClip.min, boxClip.max);
  return {{
      {1.0f, 0.0f, 0.0f, -minBounds.x},
      {-1.0f, 0.0f, 0.0f, maxBounds.x},
      {0.0f, 1.0f, 0.0f, -minBounds.y},
      {0.0f, -1.0f, 0.0f, maxBounds.y},
      {0.0f, 0.0f, 1.0f, -minBounds.z},
      {0.0f, 0.0f, -1.0f, maxBounds.z},
  }};
}

bool currentSectionPlaneEquation(const container::ui::GuiManager *guiManager,
                                 glm::vec4 &outPlane) {
  if (guiManager == nullptr) {
    return false;
  }
  const auto &sectionPlane = guiManager->sectionPlaneState();
  if (!sectionPlane.enabled) {
    return false;
  }
  outPlane = sectionPlaneEquation(sectionPlane);
  return true;
}

void includeBoundingSphere(const glm::vec4 &sphere, glm::vec3 &boundsMin,
                           glm::vec3 &boundsMax, bool &hasBounds) {
  const glm::vec3 center{sphere.x, sphere.y, sphere.z};
  const float radius = std::max(sphere.w, 0.0f);
  if (!isFiniteVec3(center) || !std::isfinite(radius)) {
    return;
  }

  const glm::vec3 extent{radius};
  if (!hasBounds) {
    boundsMin = center - extent;
    boundsMax = center + extent;
    hasBounds = true;
    return;
  }

  boundsMin = glm::min(boundsMin, center - extent);
  boundsMax = glm::max(boundsMax, center + extent);
}

bool isValidShadowCasterBoundingSphere(const glm::vec4 &sphere) {
  return isFiniteVec3(glm::vec3{sphere.x, sphere.y, sphere.z}) &&
         std::isfinite(sphere.w) && sphere.w > 0.0f;
}

bool accumulateDrawCommandBounds(
    const std::vector<DrawCommand> &commands,
    const std::vector<container::gpu::ObjectData> &objectData,
    glm::vec3 &boundsMin, glm::vec3 &boundsMax) {
  bool hasBounds = false;
  for (const DrawCommand &command : commands) {
    const uint32_t instanceCount = std::max(command.instanceCount, 1u);
    for (uint32_t instance = 0; instance < instanceCount; ++instance) {
      const uint32_t objectIndex = command.objectIndex + instance;
      if (objectIndex >= objectData.size()) {
        continue;
      }
      includeBoundingSphere(objectData[objectIndex].boundingSphere, boundsMin,
                            boundsMax, hasBounds);
    }
  }
  return hasBounds;
}

bool accumulateShadowCasterDrawCommandBounds(
    const std::vector<DrawCommand> &commands,
    const std::vector<container::gpu::ObjectData> &objectData,
    glm::vec3 &boundsMin, glm::vec3 &boundsMax) {
  bool hasBounds = false;
  for (const DrawCommand &command : commands) {
    const uint32_t instanceCount = std::max(command.instanceCount, 1u);
    for (uint32_t instance = 0; instance < instanceCount; ++instance) {
      const uint32_t objectIndex = command.objectIndex + instance;
      if (objectIndex >= objectData.size()) {
        continue;
      }
      const glm::vec4 &sphere = objectData[objectIndex].boundingSphere;
      if (!isValidShadowCasterBoundingSphere(sphere)) {
        continue;
      }
      includeBoundingSphere(sphere, boundsMin, boundsMax, hasBounds);
    }
  }
  return hasBounds;
}

void includeBounds(const glm::vec3 &sourceMin, const glm::vec3 &sourceMax,
                   glm::vec3 &boundsMin, glm::vec3 &boundsMax,
                   bool &hasBounds) {
  if (!isFiniteVec3(sourceMin) || !isFiniteVec3(sourceMax)) {
    return;
  }
  const glm::vec3 orderedMin = glm::min(sourceMin, sourceMax);
  const glm::vec3 orderedMax = glm::max(sourceMin, sourceMax);
  if (!hasBounds) {
    boundsMin = orderedMin;
    boundsMax = orderedMax;
    hasBounds = true;
    return;
  }
  boundsMin = glm::min(boundsMin, orderedMin);
  boundsMax = glm::max(boundsMax, orderedMax);
}

std::optional<ShadowCasterSceneBounds> accumulateShadowCasterSceneBounds(
    const SceneController *sceneController, const BimManager *bimManager) {
  glm::vec3 boundsMin{0.0f};
  glm::vec3 boundsMax{0.0f};
  bool hasBounds = false;

  if (sceneController != nullptr) {
    glm::vec3 sceneMin{0.0f};
    glm::vec3 sceneMax{0.0f};
    if (accumulateShadowCasterDrawCommandBounds(
            sceneController->opaqueDrawCommands(),
            sceneController->objectData(), sceneMin, sceneMax)) {
      includeBounds(sceneMin, sceneMax, boundsMin, boundsMax, hasBounds);
    }
  }

  if (bimManager != nullptr) {
    glm::vec3 bimMin{0.0f};
    glm::vec3 bimMax{0.0f};
    if (accumulateShadowCasterDrawCommandBounds(
            bimManager->opaqueDrawCommands(), bimManager->objectData(), bimMin,
            bimMax)) {
      includeBounds(bimMin, bimMax, boundsMin, boundsMax, hasBounds);
    }
  }

  if (!hasBounds) {
    return std::nullopt;
  }
  return ShadowCasterSceneBounds{.minBounds = boundsMin,
                                 .maxBounds = boundsMax};
}

float transformGizmoScale(const glm::vec3 &origin, float boundsRadius,
                          const CameraData &cameraData) {
  const glm::vec3 cameraPosition{cameraData.cameraWorldPosition};
  const float distance = glm::length(cameraPosition - origin);
  const float screenScale = std::isfinite(distance) ? distance * 0.075f : 1.0f;
  const float radiusScale = std::max(boundsRadius * 0.45f, 0.18f);
  const float maxScale = std::max(boundsRadius * 1.75f, 0.35f);
  return std::clamp(std::max(screenScale, radiusScale), 0.18f, maxScale);
}

std::optional<glm::vec2> projectToFramebuffer(const CameraData &cameraData,
                                              VkExtent2D viewportExtent,
                                              const glm::vec3 &worldPosition) {
  if (viewportExtent.width == 0u || viewportExtent.height == 0u) {
    return std::nullopt;
  }

  const glm::vec4 clip = cameraData.viewProj * glm::vec4(worldPosition, 1.0f);
  if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
      !std::isfinite(clip.w) || clip.w <= 0.0001f) {
    return std::nullopt;
  }

  const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
  return glm::vec2{
      (ndc.x * 0.5f + 0.5f) * static_cast<float>(viewportExtent.width),
      (1.0f - (ndc.y * 0.5f + 0.5f)) *
          static_cast<float>(viewportExtent.height),
  };
}

float distanceSquaredToSegment(const glm::vec2 &point, const glm::vec2 &start,
                               const glm::vec2 &end) {
  const glm::vec2 segment = end - start;
  const float lengthSquared = glm::dot(segment, segment);
  if (lengthSquared <= 0.0001f) {
    const glm::vec2 delta = point - start;
    return glm::dot(delta, delta);
  }

  const float t =
      std::clamp(glm::dot(point - start, segment) / lengthSquared, 0.0f, 1.0f);
  const glm::vec2 closest = start + segment * t;
  const glm::vec2 delta = point - closest;
  return glm::dot(delta, delta);
}

float snapRelativeFloat(float base, float value, float step) {
  if (step <= 0.0f) {
    return value;
  }
  return base + std::round((value - base) / step) * step;
}

glm::vec3 snapRelativeVec3(const glm::vec3 &base, const glm::vec3 &value,
                           float step) {
  return {
      snapRelativeFloat(base.x, value.x, step),
      snapRelativeFloat(base.y, value.y, step),
      snapRelativeFloat(base.z, value.z, step),
  };
}

enum class GpuPickTargetKind {
  None,
  Scene,
  Bim,
  Light,
};

struct GpuPickTarget {
  GpuPickTargetKind kind{GpuPickTargetKind::None};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  EditableLightId editableLightId{};
};

GpuPickTarget decodeGpuPickId(uint32_t pickId) {
  if (pickId == container::gpu::kPickIdNone) {
    return {};
  }

  if ((pickId & container::gpu::kPickIdLightMask) != 0u) {
    if (auto lightId = decodeEditableLightPickId(pickId)) {
      return GpuPickTarget{.kind = GpuPickTargetKind::Light,
                           .editableLightId = *lightId};
    }
    return {};
  }

  const bool isBim = (pickId & container::gpu::kPickIdBimMask) != 0u;
  const uint32_t encodedObject = pickId & container::gpu::kPickIdObjectMask;
  if (encodedObject == 0u) {
    return {};
  }

  return GpuPickTarget{
      .kind = isBim ? GpuPickTargetKind::Bim : GpuPickTargetKind::Scene,
      .objectIndex = encodedObject - 1u,
  };
}

std::optional<glm::vec3>
unprojectDepthAtCursor(const container::gpu::CameraData &cameraData,
                       VkExtent2D viewportExtent, double cursorX,
                       double cursorY, float depth) {
  if (viewportExtent.width == 0u || viewportExtent.height == 0u ||
      cursorX < 0.0 || cursorY < 0.0 ||
      cursorX >= static_cast<double>(viewportExtent.width) ||
      cursorY >= static_cast<double>(viewportExtent.height) ||
      !std::isfinite(depth)) {
    return std::nullopt;
  }

  const float ndcX = static_cast<float>(
      (cursorX / static_cast<double>(viewportExtent.width)) * 2.0 - 1.0);
  const float ndcY = static_cast<float>(
      1.0 - (cursorY / static_cast<double>(viewportExtent.height)) * 2.0);
  glm::vec4 world = cameraData.inverseViewProj *
                    glm::vec4(ndcX, ndcY, std::clamp(depth, 0.0f, 1.0f), 1.0f);
  if (world.w == 0.0f) {
    return std::nullopt;
  }
  world /= world.w;
  const glm::vec3 point{world};
  return isFiniteVec3(point) ? std::optional<glm::vec3>{point} : std::nullopt;
}

container::ui::GBufferViewMode
frontendDisplayMode(const container::ui::GuiManager *guiManager,
                    container::ui::GBufferViewMode fallbackDisplayMode) {
  return guiManager ? guiManager->gBufferViewMode() : fallbackDisplayMode;
}

bool displayModeRecordsShadowAtlas(container::ui::GBufferViewMode mode) {
  return static_cast<uint32_t>(mode) >= 100u ||
         mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::Overview;
}

bool displayModeRecordsTileCull(container::ui::GBufferViewMode mode) {
  return static_cast<uint32_t>(mode) >= 100u ||
         mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::Overview ||
         mode == container::ui::GBufferViewMode::TileLightHeatMap;
}

bool displayModeRecordsGtao(container::ui::GBufferViewMode mode) {
  return static_cast<uint32_t>(mode) >= 100u ||
         mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::Overview;
}

std::string bimSelectionLabel(const BimElementMetadata &metadata) {
  std::string label =
      "Selected BIM object " + std::to_string(metadata.objectIndex);
  if (!metadata.type.empty() && metadata.type != "Unknown") {
    label += " (" + metadata.type + ")";
  }
  if (!metadata.guid.empty()) {
    label += " [" + metadata.guid + "]";
  }
  return label;
}

std::string bimGeometryKindLabel(BimGeometryKind kind) {
  switch (kind) {
  case BimGeometryKind::Points:
    return "points";
  case BimGeometryKind::Curves:
    return "curves";
  case BimGeometryKind::Mesh:
  default:
    return "mesh";
  }
}

struct FrameFeatureReadiness {
  bool shadowAtlas{false};
  bool localShadowAtlas{false};
  bool gtao{false};
  bool tileCull{false};
};

bool hasShadowAtlasResources(const ShadowManager *shadowManager) {
  if (shadowManager == nullptr ||
      shadowManager->shadowAtlasArrayView() == VK_NULL_HANDLE ||
      shadowManager->shadowSampler() == VK_NULL_HANDLE) {
    return false;
  }

  return std::ranges::any_of(shadowManager->shadowUbos(), [](const auto &ubo) {
    return ubo.buffer != VK_NULL_HANDLE;
  });
}

bool hasLocalShadowAtlasResources(const ShadowManager *shadowManager,
                                  uint32_t imageIndex) {
  if (shadowManager == nullptr ||
      shadowManager->localShadowAtlasArrayView() == VK_NULL_HANDLE ||
      shadowManager->shadowSampler() == VK_NULL_HANDLE ||
      shadowManager->localShadowDescriptorSet(imageIndex) == VK_NULL_HANDLE ||
      shadowManager->localShadowLayerCount() == 0u) {
    return false;
  }

  const auto &localShadowUbo = shadowManager->localShadowUbo(imageIndex);
  if (localShadowUbo.buffer == VK_NULL_HANDLE) {
    return false;
  }

  const uint32_t layerCount = shadowManager->localShadowLayerCount();
  for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
    if (shadowManager->localShadowFramebuffer(layerIndex) == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

bool hasGtaoResources(const EnvironmentManager *environmentManager) {
  return environmentManager != nullptr && environmentManager->isGtaoReady() &&
         environmentManager->isAoEnabled() &&
         environmentManager->aoTextureView() != VK_NULL_HANDLE &&
         environmentManager->aoSampler() != VK_NULL_HANDLE;
}

bool hasTileCullResources(const LightingManager *lightingManager) {
  return lightingManager != nullptr &&
         lightingManager->isTiledLightingReady() &&
         lightingManager->tileGridBuffer() != VK_NULL_HANDLE &&
         lightingManager->tileGridBufferSize() > 0;
}

bool graphPassScheduled(const FrameRecorder *frameRecorder, RenderPassId id,
                        bool preferPreparedFrame = false) {
  if (frameRecorder == nullptr) {
    return false;
  }
  if (preferPreparedFrame) {
    for (const auto &status :
         frameRecorder->graph().lastFrameExecutionStatuses()) {
      if (status.id == id) {
        return status.active;
      }
    }
  }
  const auto *status = frameRecorder->graph().executionStatus(id);
  return status != nullptr && status->active;
}

bool anyShadowCascadeScheduled(const FrameRecorder *frameRecorder,
                               bool preferPreparedFrame = false) {
  return std::ranges::any_of(
      shadowCascadePassIds(), [frameRecorder, preferPreparedFrame](auto id) {
        return graphPassScheduled(frameRecorder, id, preferPreparedFrame);
      });
}

bool deferredRasterLocalShadowSceneInputsReady(const FrameRecordParams &p) {
  return shadowDescriptorSet(p, ShadowDescriptorSetId::Scene) !=
             VK_NULL_HANDLE &&
         p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         hasOpaqueDrawCommands(p.draws);
}

bool deferredRasterLocalShadowBimInputsReady(const FrameRecordParams &p) {
  return shadowDescriptorSet(p, ShadowDescriptorSetId::BimScene) !=
             VK_NULL_HANDLE &&
         p.bim.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.bim.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         hasBimOpaqueDrawCommands(p.bim);
}

bool deferredRasterLocalShadowFrameInputsReady(const FrameRecordParams &p,
                                               bool shadowAtlasVisible) {
  if (!shadowAtlasVisible || p.shadows.renderPass == VK_NULL_HANDLE ||
      p.shadows.localShadowFramebuffers == nullptr ||
      p.shadows.localShadowData == nullptr ||
      p.shadows.localShadowData->counts.w == 0u ||
      p.shadows.localShadowLayerCount == 0u ||
      shadowDescriptorSet(p, ShadowDescriptorSetId::LocalShadow) ==
          VK_NULL_HANDLE ||
      !shadowPipelineReady(p, ShadowPipelineId::LocalDepth) ||
      !shadowPipelineLayoutReady(p, ShadowPipelineLayoutId::Shadow)) {
    return false;
  }

  const uint32_t layerCount =
      std::min(p.shadows.localShadowLayerCount,
               container::gpu::kMaxShadowedLocalLightLayers);
  for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
    if (p.shadows.localShadowFramebuffers[layerIndex] == VK_NULL_HANDLE) {
      return false;
    }
  }

  return deferredRasterLocalShadowSceneInputsReady(p) ||
         deferredRasterLocalShadowBimInputsReady(p);
}

FrameFeatureReadiness evaluateFrameFeatureReadiness(
    RenderTechniqueId activeTechnique,
    container::ui::GBufferViewMode displayMode,
    const FrameRecorder *frameRecorder, const ShadowManager *shadowManager,
    const EnvironmentManager *environmentManager,
    const LightingManager *lightingManager, uint32_t imageIndex,
    const FrameRecordParams *preparedParams) {
  const bool usePreparedFrame =
      frameRecorder != nullptr && preparedParams != nullptr;
  if (usePreparedFrame) {
    frameRecorder->graph().prepareFrame(*preparedParams);
  }

  const bool shadowAtlasVisible =
      activeTechnique == RenderTechniqueId::ForwardRaster ||
      displayModeRecordsShadowAtlas(displayMode);

  FrameFeatureReadiness readiness{};
  readiness.shadowAtlas =
      shadowAtlasVisible &&
      anyShadowCascadeScheduled(frameRecorder, usePreparedFrame) &&
      hasShadowAtlasResources(shadowManager);
  readiness.localShadowAtlas =
      shadowAtlasVisible &&
      graphPassScheduled(frameRecorder, RenderPassId::LocalShadowDepth,
                         usePreparedFrame) &&
      (preparedParams == nullptr ||
       preparedParams->shadows.localShadowLayerCount > 0u) &&
      hasLocalShadowAtlasResources(shadowManager, imageIndex);
  readiness.gtao =
      displayModeRecordsGtao(displayMode) &&
      graphPassScheduled(frameRecorder, RenderPassId::GTAO, usePreparedFrame) &&
      hasGtaoResources(environmentManager);
  readiness.tileCull = displayModeRecordsTileCull(displayMode) &&
                       graphPassScheduled(frameRecorder, RenderPassId::TileCull,
                                          usePreparedFrame) &&
                       hasTileCullResources(lightingManager);
  return readiness;
}

bool sameVec4(const glm::vec4 &lhs, const glm::vec4 &rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

bool sameMat4(const glm::mat4 &lhs, const glm::mat4 &rhs) {
  for (int column = 0; column < 4; ++column) {
    if (!sameVec4(lhs[column], rhs[column])) {
      return false;
    }
  }
  return true;
}

bool sameCameraData(const container::gpu::CameraData &lhs,
                    const container::gpu::CameraData &rhs) {
  return sameMat4(lhs.viewProj, rhs.viewProj) &&
         sameMat4(lhs.inverseViewProj, rhs.inverseViewProj) &&
         sameVec4(lhs.cameraWorldPosition, rhs.cameraWorldPosition) &&
         sameVec4(lhs.cameraForward, rhs.cameraForward);
}

container::scene::SceneProviderBounds sceneProviderBoundsFromModelBounds(
    const container::scene::ModelBounds &bounds) {
  return {
      .min = bounds.min,
      .max = bounds.max,
      .valid = bounds.valid,
  };
}

container::scene::SceneProviderBounds
sceneProviderBoundsFromBim(const BimManager &bimManager) {
  container::scene::SceneProviderBounds bounds{};
  for (const BimElementMetadata &metadata : bimManager.elementMetadata()) {
    if (!metadata.bounds.valid) {
      continue;
    }
    if (!bounds.valid) {
      bounds.min = metadata.bounds.min;
      bounds.max = metadata.bounds.max;
      bounds.valid = true;
      continue;
    }
    bounds.min.x = std::min(bounds.min.x, metadata.bounds.min.x);
    bounds.min.y = std::min(bounds.min.y, metadata.bounds.min.y);
    bounds.min.z = std::min(bounds.min.z, metadata.bounds.min.z);
    bounds.max.x = std::max(bounds.max.x, metadata.bounds.max.x);
    bounds.max.y = std::max(bounds.max.y, metadata.bounds.max.y);
    bounds.max.z = std::max(bounds.max.z, metadata.bounds.max.z);
  }
  return bounds;
}

std::string sceneProviderDisplayName(std::string_view modelPath,
                                     std::string_view fallback) {
  if (modelPath.empty()) {
    return std::string(fallback);
  }
  const size_t slash = modelPath.find_last_of("/\\");
  const std::string_view fileName =
      slash == std::string_view::npos ? modelPath : modelPath.substr(slash + 1);
  return fileName.empty() ? std::string(fallback) : std::string(fileName);
}

bool finiteVec3(const glm::vec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

void includeSceneViewBounds(CameraController::SceneViewBounds &target,
                            const container::scene::SceneProviderBounds &source) {
  if (!source.valid || !finiteVec3(source.min) || !finiteVec3(source.max)) {
    return;
  }

  const glm::vec3 sourceMin = glm::min(source.min, source.max);
  const glm::vec3 sourceMax = glm::max(source.min, source.max);
  if (!target.valid) {
    target.min = sourceMin;
    target.max = sourceMax;
    target.valid = true;
    return;
  }

  target.min = glm::min(target.min, sourceMin);
  target.max = glm::max(target.max, sourceMax);
}

CameraController::SceneViewBounds cameraSceneBoundsFromActiveContent(
    const container::scene::SceneManager *sceneManager,
    const BimManager *bimManager) {
  CameraController::SceneViewBounds bounds{};
  if (sceneManager != nullptr) {
    includeSceneViewBounds(
        bounds, sceneProviderBoundsFromModelBounds(sceneManager->modelBounds()));
  }
  if (bimManager != nullptr && bimManager->hasScene()) {
    includeSceneViewBounds(bounds, sceneProviderBoundsFromBim(*bimManager));
  }
  return bounds;
}

} // namespace

RendererFrontend::RendererFrontend(RendererFrontendCreateInfo info)
    : svc_{*info.ctx,
           *info.pipelineManager,
           *info.allocationManager,
           *info.swapChainManager,
           *info.commandBufferManager,
           *info.config,
           info.nativeWindow,
           *info.inputManager} {}

RendererFrontend::~RendererFrontend() { shutdown(); }

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void RendererFrontend::initialize() {
  // Initialization order mirrors runtime dependencies: render passes define
  // attachment compatibility, scene/lighting managers create descriptor
  // layouts, and only then can graphics pipelines be built.
  subs_.renderPassManager =
      std::make_unique<RenderPassManager>(svc_.ctx.deviceWrapper);
  subs_.oitManager = std::make_unique<OitManager>(svc_.ctx.deviceWrapper);
  {
    const RendererMsaaDeviceSupport msaaSupport =
        queryRendererMsaaDeviceSupport(svc_.ctx.deviceWrapper->physicalDevice());
    const auto supportedSampleCounts =
        supportedMsaaSampleCounts(msaaSupport.color, msaaSupport.depth);
    supportedMsaaSamples_.clear();
    supportedMsaaSamples_.reserve(supportedSampleCounts.size());
    for (VkSampleCountFlagBits sampleCount : supportedSampleCounts) {
      supportedMsaaSamples_.push_back(sampleCountToSamples(sampleCount));
    }
    msaaSampleCount_ = clampMsaaSampleCount(
        svc_.config.msaaSamples, msaaSupport.color, msaaSupport.depth);
  }
  createRenderPasses();

  subs_.sceneManager = std::make_unique<container::scene::SceneManager>(
      svc_.allocationManager, svc_.pipelineManager, svc_.ctx.deviceWrapper,
      svc_.config);
  subs_.sceneManager->initialize(
      svc_.config.modelPath, svc_.config.importScale,
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  sceneState_.indexType = subs_.sceneManager->indexType();
  activePrimaryModelPath_ = svc_.config.modelPath;
  activePrimaryImportScale_ = svc_.config.importScale;

  if (!svc_.config.bimModelPath.empty()) {
    subs_.bimManager = std::make_unique<BimManager>(
        svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
    subs_.bimManager->createMeshletResidencyResources(
        container::util::executableDirectory());
    subs_.bimManager->loadModel(svc_.config.bimModelPath,
                                svc_.config.bimImportScale,
                                *subs_.sceneManager);
    activeAuxiliaryModelPath_ = svc_.config.bimModelPath;
    activeAuxiliaryImportScale_ = svc_.config.bimImportScale;
  }

  subs_.sceneController = std::make_unique<SceneController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      sceneGraph_, *subs_.sceneManager, nullptr, svc_.config);
  subs_.lightingManager = std::make_unique<LightingManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      subs_.sceneManager.get(), sceneGraph_, subs_.sceneController->world());
  subs_.shadowManager = std::make_unique<ShadowManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.shadowManager->createResources(
      resources_.gBufferFormats.depthStencil,
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.shadowManager->createFramebuffers(resources_.renderPasses.shadow);
  subs_.shadowCullManager = std::make_unique<ShadowCullManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.shadowCullManager->createResources(
      container::util::executableDirectory(),
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.environmentManager = std::make_unique<EnvironmentManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.commandBufferManager.pool());
  subs_.environmentManager->createResources(
      container::util::executableDirectory());
  {
    const auto exeDir = container::util::executableDirectory();
    const std::filesystem::path hdrPath =
        exeDir / container::app::kDefaultEnvironmentHdrRelativePath;
    if (std::filesystem::exists(hdrPath)) {
      subs_.environmentManager->loadHdrEnvironment(exeDir, hdrPath);
    }
  }
  subs_.gpuCullManager = std::make_unique<GpuCullManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.gpuCullManager->createResources(
      container::util::executableDirectory(),
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.bloomManager = std::make_unique<BloomManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.commandBufferManager.pool());
  subs_.bloomManager->createResources(container::util::executableDirectory());
  applySceneLightingDefaults();
  subs_.exposureManager = std::make_unique<ExposureManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.exposureManager->createResources(
      container::util::executableDirectory(),
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.temporalManager = std::make_unique<TemporalManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.temporalManager->settings() = svc_.config.taa;
  subs_.frameResourceManager = std::make_unique<FrameResourceManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.swapChainManager, svc_.commandBufferManager.pool());
  subs_.frameResourceManager->createDescriptorSetLayouts();
  subs_.frameResourceManager->createGBufferSampler();
  subs_.lightingManager->createDescriptorResources(
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.lightingManager->createTiledResources(
      container::util::executableDirectory(), svc_.swapChainManager.extent());
  createGraphicsPipelines();
  svc_.swapChainManager.createFramebuffers(resources_.renderPasses.postProcess);

  createCamera();

  if (svc_.config.enableGui) {
    subs_.guiManager = std::make_unique<container::ui::GuiManager>();
    subs_.guiManager->initialize(
        svc_.ctx.instance, svc_.ctx.deviceWrapper->device(),
        svc_.ctx.deviceWrapper->physicalDevice(),
        svc_.ctx.deviceWrapper->graphicsQueue(),
        svc_.ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
        resources_.renderPasses.postProcess,
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()),
        svc_.nativeWindow, svc_.config.modelPath, svc_.config.importScale);
    subs_.guiManager->setWireframeCapabilities(
        svc_.ctx.wireframeSupported, svc_.ctx.wireframeRasterModeSupported,
        svc_.ctx.wireframeWideLinesSupported);
    if (subs_.environmentManager) {
      subs_.guiManager->setEnvironmentStatus(
          subs_.environmentManager->environmentStatus());
    }
    if (subs_.sceneController)
      subs_.sceneController->setGuiManager(subs_.guiManager.get());
  }

  subs_.frameRecorder = std::make_unique<FrameRecorder>();
  subs_.deferredRasterFrameGraphContext =
      std::make_unique<DeferredRasterFrameGraphContext>(
          DeferredRasterFrameGraphServices{
              .graph = &subs_.frameRecorder->graph(),
              .swapChainManager = &svc_.swapChainManager,
              .oitManager = subs_.oitManager.get(),
              .lightingManager = subs_.lightingManager.get(),
              .environmentManager = subs_.environmentManager.get(),
              .sceneController = subs_.sceneController.get(),
              .gpuCullManager = subs_.gpuCullManager.get(),
              .bloomManager = subs_.bloomManager.get(),
              .exposureManager = subs_.exposureManager.get(),
              .camera = subs_.cameraController
                            ? subs_.cameraController->camera()
                            : nullptr,
              .guiManager = subs_.guiManager.get(),
              .fallbackDisplayMode = configuredDisplayMode(svc_.config)});
  subs_.frameResourceRegistry = std::make_unique<FrameResourceRegistry>();
  subs_.frameRuntimeResourceRegistry =
      std::make_unique<FrameResourceRegistry>();
  subs_.pipelineRegistry = std::make_unique<PipelineRegistry>();
  subs_.sceneProviderSynchronizer =
      std::make_unique<SceneProviderSynchronizer>();
  subs_.sceneProviderRegistry =
      std::make_unique<container::scene::SceneProviderRegistry>();
  syncSceneProviders();
  auto techniqueRegistry = createDefaultRenderTechniqueRegistry();
  subs_.techniqueRegistry =
      std::make_unique<RenderTechniqueRegistry>(std::move(techniqueRegistry));
  const RenderTechniqueId requestedTechnique =
      renderTechniqueIdFromName(svc_.config.renderTechnique)
          .value_or(RenderTechniqueId::DeferredRaster);
  initializeRenderTechnique(requestedTechnique, svc_.config.renderTechnique);

  svc_.commandBufferManager.allocate(svc_.swapChainManager.imageCount());
  svc_.commandBufferManager.configureSecondaryBuffers(
      static_cast<uint32_t>(shadowCascadePassIds().size()), 1);
  subs_.frameSyncManager = std::make_unique<container::gpu::FrameSyncManager>(
      svc_.ctx.deviceWrapper->device(), svc_.config.maxFramesInFlight);
  subs_.frameSyncManager->initialize(
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  frame_.imagesInFlight.assign(svc_.swapChainManager.imageCount(),
                               VK_NULL_HANDLE);
  subs_.renderPassGpuProfiler = std::make_unique<RenderPassGpuProfiler>();
  subs_.renderPassGpuProfiler->initialize(
      svc_.ctx.deviceWrapper->device(), svc_.ctx.instance,
      svc_.ctx.deviceWrapper->physicalDevice(),
      svc_.ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  container::log::ContainerLogger::instance().renderer()->info(
      "GPU pass profiler: {}", subs_.renderPassGpuProfiler->backendStatus());
  subs_.rendererTelemetry = std::make_unique<RendererTelemetry>();

  initializeScene();
}

RenderSystemContext RendererFrontend::renderSystemContext() {
  return RenderSystemContext{
      .frameRecorder = subs_.frameRecorder.get(),
      .deferredRaster = subs_.deferredRasterFrameGraphContext.get(),
      .deviceCapabilities = &subs_.deviceCapabilities,
      .sceneProviders = subs_.sceneProviderRegistry.get(),
      .frameResources = subs_.frameResourceRegistry.get(),
      .pipelines = subs_.pipelineRegistry.get(),
  };
}

void RendererFrontend::initializeRenderTechnique(
    RenderTechniqueId requested, std::string_view requestLabel) {
  if (!subs_.techniqueRegistry) {
    throw std::runtime_error("render technique registry is not initialized");
  }

  RenderSystemContext context = renderSystemContext();
  const RenderTechniqueSelection selection =
      subs_.techniqueRegistry->select(requested, context);
  subs_.activeTechnique = selection.technique;
  if (subs_.activeTechnique == nullptr) {
    throw std::runtime_error(
        "No available render technique; deferred raster fallback unavailable");
  }

  subs_.activeTechnique->registerTechniqueContracts(context);
  subs_.activeTechnique->buildFrameGraph(context);

  if (subs_.guiManager && subs_.frameRecorder) {
    DebugUiPresenter::publishTechniqueDebugModel(
        *subs_.guiManager, subs_.activeTechnique->debugModel());
    DebugUiPresenter::publishRenderPasses(*subs_.guiManager,
                                          subs_.frameRecorder->graph());
  }

  if (selection.usedFallback) {
    container::log::ContainerLogger::instance().renderer()->warn(
        "Requested render technique '{}' unavailable ({}); using '{}'.",
        requestLabel, selection.unavailableReason,
        subs_.activeTechnique->name());
  } else {
    container::log::ContainerLogger::instance().renderer()->info(
        "Active render technique: {}", subs_.activeTechnique->name());
  }
}

void RendererFrontend::syncGuiRenderEngineOptions() {
  if (!subs_.guiManager || !subs_.techniqueRegistry) {
    return;
  }

  std::vector<container::ui::RenderEngineOption> options;
  RenderSystemContext context = renderSystemContext();
  for (const RenderTechniqueDescriptor &descriptor :
       knownRenderTechniqueDescriptors()) {
    RenderTechnique *technique = subs_.techniqueRegistry->find(descriptor.id);
    if (technique == nullptr && !descriptor.implemented) {
      continue;
    }

    container::ui::RenderEngineOption option{};
    option.id = descriptor.id;
    option.label = std::string(descriptor.displayName);
    if (technique == nullptr) {
      option.available = false;
      option.unavailableReason = "render technique is not registered";
    } else {
      const RenderTechniqueAvailability availability =
          technique->availability(context);
      option.available = availability.available;
      option.unavailableReason = availability.reason;
    }
    options.push_back(std::move(option));
  }

  subs_.guiManager->setRenderEngineOptions(
      std::move(options),
      subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                       : RenderTechniqueId::DeferredRaster);
}

void RendererFrontend::requestRenderTechnique(RenderTechniqueId requested) {
  if (subs_.activeTechnique != nullptr &&
      subs_.activeTechnique->id() == requested) {
    return;
  }
  pendingRenderTechniqueChange_ = requested;
}

void RendererFrontend::applyPendingRenderTechniqueChange() {
  if (!pendingRenderTechniqueChange_) {
    return;
  }

  const RenderTechniqueId requested = *pendingRenderTechniqueChange_;
  pendingRenderTechniqueChange_.reset();
  if (subs_.temporalManager)
    subs_.temporalManager->reset("render technique changed");
  initializeRenderTechnique(requested, renderTechniqueName(requested));
  syncGuiRenderEngineOptions();

  if (subs_.guiManager) {
    const RenderTechniqueId active =
        subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                         : RenderTechniqueId::DeferredRaster;
    subs_.guiManager->setStatusMessage(
        "Render engine active: " +
        std::string(renderTechniqueDisplayName(active)));
  }
}

bool RendererFrontend::drawFrame(bool &framebufferResized) {
  const auto frameStart = TelemetryClock::now();
  const auto concurrencyPolicy = FrameConcurrencyPolicy::serializedGpuResources(
      "shared object buffer plus GPU cull, tile cull, GTAO, bloom, "
      "exposure, and readback resources are not yet per-frame");
  auto *telemetry = subs_.rendererTelemetry.get();
  if (telemetry) {
    telemetry->beginFrame(frame_.submittedFrameCount, frame_.currentFrame,
                          svc_.config.maxFramesInFlight,
                          concurrencyPolicy.mode() ==
                              FrameConcurrencyMode::SerializedGpuResources,
                          concurrencyPolicy.reason());
  }

  auto phaseStart = TelemetryClock::now();
  concurrencyPolicy.waitBeforeAcquire(*subs_.frameSyncManager,
                                      frame_.currentFrame);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::WaitForFrame,
                           elapsedMilliseconds(phaseStart));
    if (subs_.renderPassGpuProfiler) {
      const auto gpuTimings = subs_.renderPassGpuProfiler->collectLatest();
      telemetry->setPassGpuTimings(gpuTimings,
                                   subs_.renderPassGpuProfiler->timingSource());
      telemetry->setGpuProfilerStatus(RendererGpuProfilerTelemetry{
          .source = subs_.renderPassGpuProfiler->timingSource(),
          .available = subs_.renderPassGpuProfiler->isReady(),
          .resultLatencyFrames =
              subs_.renderPassGpuProfiler->resultLatencyFrames(),
          .status = std::string(subs_.renderPassGpuProfiler->backendStatus()),
      });
    }
  }

  applyPendingRenderTechniqueChange();

  if (pendingMsaaSampleCount_) {
    phaseStart = TelemetryClock::now();
    const VkSampleCountFlagBits requestedSampleCount = *pendingMsaaSampleCount_;
    pendingMsaaSampleCount_.reset();
    recreateMsaaResources(requestedSampleCount);
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  }

  // Collect culling statistics from the previous frame (now safe after fence).
  phaseStart = TelemetryClock::now();
  if (subs_.gpuCullManager)
    subs_.gpuCullManager->collectStats();
  if (subs_.lightingManager)
    subs_.lightingManager->collectStats();
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Readbacks,
                           elapsedMilliseconds(phaseStart));
    if (subs_.gpuCullManager) {
      telemetry->setCullingStats(subs_.gpuCullManager->cullStats());
    }
    if (subs_.lightingManager) {
      telemetry->setLightCullingStats(
          subs_.lightingManager->lightCullingStats());
    }
  }

  uint32_t imageIndex = 0;
  phaseStart = TelemetryClock::now();
  VkResult result =
      std::exchange(captureAcquireOutOfDate_, false)
          ? VK_ERROR_OUT_OF_DATE_KHR
          : vkAcquireNextImageKHR(
      svc_.ctx.deviceWrapper->device(), svc_.swapChainManager.swapChain(),
      UINT64_MAX, subs_.frameSyncManager->imageAvailable(frame_.currentFrame),
      VK_NULL_HANDLE, &imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::AcquireImage,
                           elapsedMilliseconds(phaseStart));
  }

  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    gfxJournal_.acquireFailed(static_cast<int32_t>(result));
    phaseStart = TelemetryClock::now();
    handleResize();
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
      telemetry->setCpuPhase(RendererTelemetryPhase::Frame,
                             elapsedMilliseconds(frameStart));
      telemetry->endFrame();
    }
    framebufferResized = false;
    return false;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    gfxJournal_.acquireFailed(static_cast<int32_t>(result));
    throw std::runtime_error("failed to acquire swap chain image!");
  }
  if (telemetry) {
    telemetry->setImageIndex(imageIndex);
  }

  phaseStart = TelemetryClock::now();
  if (frame_.imagesInFlight[imageIndex]) {
    vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                    &frame_.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::ImageFenceWait,
                           elapsedMilliseconds(phaseStart));
  }

  if (subs_.exposureManager) {
    subs_.exposureManager->collectReadback(
        imageIndex, subs_.guiManager ? subs_.guiManager->exposureSettings()
                                     : container::gpu::ExposureSettings{});
  }

  phaseStart = TelemetryClock::now();
  growExactOitNodePoolIfNeeded(imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  presentSceneControls();
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Gui,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  applyBimSemanticColorMode();
  auto &temporal = *subs_.temporalManager;
  // Section coverage changes invalidate the whole clipped surface domain;
  // material/topology revisions below remain local to their provider/object.
  uint64_t clipRevision = 1469598103934665603ull;
  auto hashClip = [&](const auto &value) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) {
      clipRevision ^= bytes[i];
      clipRevision *= 1099511628211ull;
    }
  };
  glm::vec4 plane{};
  bool planeEnabled =
      currentSectionPlaneEquation(subs_.guiManager.get(), plane);
  if (captureSectionPlane_) {
    planeEnabled = true;
    plane = *captureSectionPlane_;
  }
  hashClip(planeEnabled);
  if (planeEnabled)
    hashClip(plane);
  if (subs_.guiManager) {
    const auto &box = subs_.guiManager->bimBoxClipState();
    hashClip(box.enabled);
    if (box.enabled) {
      hashClip(box.invert);
      const auto planes = makeBoxClipPlanes(box);
      for (const auto &equation : planes)
        hashClip(equation);
    }
  }
  if (temporalClipRevision_ && *temporalClipRevision_ != clipRevision)
    temporal.reset("section coverage changed");
  temporalClipRevision_ = clipRevision;
  container::temporal::validateSettings(
      temporal.settings(), static_cast<uint32_t>(msaaSampleCount_));
  if (temporal.settings().enabled) {
    temporal.createPipelines(container::util::executableDirectory(),
                             subs_.sceneManager->descriptorSetLayout(),
                             resources_.gBufferFormats.depthStencil);
    temporal.prepare(*subs_.frameResourceManager,
                     svc_.swapChainManager.extent(),
                     static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  if (svc_.config.taaResetFrame != 0 &&
      frame_.submittedFrameCount + 1 == svc_.config.taaResetFrame)
    temporal.reset("configured capture reset");
  updateCameraBuffer(imageIndex);
  const auto temporalExtent = svc_.swapChainManager.extent();
  const auto *temporalCamera = subs_.cameraController->camera();
  auto frameTemporalSettings = temporal.settings();
  const auto mode = static_cast<uint32_t>(frontendDisplayMode(
      subs_.guiManager.get(), configuredDisplayMode(svc_.config)));
  frameTemporalSettings.enabled =
      frameTemporalSettings.enabled &&
      (mode == 0u || mode == 8u || (mode >= 100u && mode <= 104u));
  temporal.state().prepareCamera(
      buffers_.cameraData, {temporalExtent.width, temporalExtent.height},
      frameTemporalSettings,
      temporalCamera->projectionMatrix(float(temporalExtent.width) /
                                       float(temporalExtent.height)));
  SceneController::writeToBuffer(
      svc_.allocationManager, buffers_.cameras[imageIndex],
      &buffers_.cameraData, sizeof(buffers_.cameraData));
  updateObjectBuffer(imageIndex);
  if (frameTemporalSettings.enabled) {
    auto objects = subs_.sceneController->objectData();
    for (auto &object : objects) {
      const uint64_t key = uint64_t(object.temporalInfo.z) |
                           (uint64_t(object.temporalInfo.w) << 32u);
      temporal.state().prepareObject(
          object, {1, key},
          (subs_.sceneManager->temporalMaterialRevision(object.objectInfo.x) ^
           (uint64_t(object.objectInfo.y) << 32u)));
    }
    if (!objects.empty())
      SceneController::writeToBuffer(
          svc_.allocationManager, buffers_.objects[imageIndex], objects.data(),
          objects.size() * sizeof(objects[0]));
    if (subs_.bimManager && subs_.bimManager->objectAllocatedBuffer().buffer) {
      uint64_t providerRevision = 0;
      // Residency policy changes invalidate only the sidecar provider;
      // root/instance motion keeps correspondence and retains its history.
      if (subs_.guiManager) {
        const auto &lod = subs_.guiManager->bimLodStreamingUiState();
        providerRevision =
            uint64_t(lod.lodBias + 8) ^ (uint64_t(lod.autoLod) << 8u);
      }
      if (captureBimLodBias_)
        providerRevision = uint64_t(*captureBimLodBias_ + 8);
      auto bimObjects = subs_.bimManager->objectData();
      for (size_t index = 0; index < bimObjects.size(); ++index) {
        auto &object = bimObjects[index];
        temporal.state().prepareObject(
            object,
            {2, uint64_t(object.temporalInfo.z) |
                    (uint64_t(object.temporalInfo.w) << 32u)},
            (subs_.sceneManager->temporalMaterialRevision(object.objectInfo.x) ^
             (uint64_t(object.objectInfo.y) << 32u) ^
             (providerRevision << 16u)));
      }
      if (!bimObjects.empty())
        SceneController::writeToBuffer(
            svc_.allocationManager, subs_.bimManager->objectAllocatedBuffer(),
            bimObjects.data(), bimObjects.size() * sizeof(bimObjects[0]));
    }
  }

  if (subs_.lightingManager && subs_.cameraController) {
    subs_.lightingManager->updateLightingDataForActiveCamera();
    applyConfiguredLightingOverrides(subs_.lightingManager.get(),
                                      svc_.config);
  }

  if (subs_.shadowManager && subs_.lightingManager && subs_.cameraController) {
    const auto &ld = subs_.lightingManager->lightingData();
    const auto &currentLightingSettings =
        subs_.lightingManager->lightingSettings();
    const float aspect =
        static_cast<float>(svc_.swapChainManager.extent().width) /
        static_cast<float>(svc_.swapChainManager.extent().height);
    const container::gpu::ShadowSettings shadowSettings =
        subs_.guiManager ? subs_.guiManager->shadowSettings()
                         : container::gpu::ShadowSettings{};
    const std::optional<ShadowCasterSceneBounds> shadowCasterBounds =
        accumulateShadowCasterSceneBounds(subs_.sceneController.get(),
                                          subs_.bimManager.get());
    subs_.shadowManager->update(subs_.cameraController->camera(), aspect,
                                glm::vec3(ld.directionalDirection),
                                shadowSettings,
                                shadowCasterBounds ? &*shadowCasterBounds
                                                   : nullptr,
                                imageIndex);
    subs_.shadowManager->updateLocalShadows(
        subs_.lightingManager->pointLightsSsbo(),
        subs_.lightingManager->areaLightsSsbo(), shadowSettings,
        currentLightingSettings.localShadowLayerBudget, imageIndex);
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::SceneUpdate,
                           elapsedMilliseconds(phaseStart));
  }

  const bool screenshotThisFrame = screenshot_.pending;
  if (screenshotThisFrame) {
    phaseStart = TelemetryClock::now();
    ensureScreenshotReadbackBuffer(frame_.currentFrame,
                                   svc_.swapChainManager.extent(),
                                   svc_.swapChainManager.imageFormat());
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  }

  FrameRecordParams frameRecordParams = buildFrameRecordParams(imageIndex);
  attachActiveTechniqueLifecycle(frameRecordParams);

  phaseStart = TelemetryClock::now();
  updateFrameDescriptorSets(imageIndex, &frameRecordParams);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::DescriptorUpdate,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  vkResetCommandBuffer(svc_.commandBufferManager.buffer(imageIndex), 0);
  recordCommandBuffer(svc_.commandBufferManager.buffer(imageIndex), imageIndex,
                      &frameRecordParams);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::CommandRecord,
                           elapsedMilliseconds(phaseStart));
    if (subs_.frameRecorder) {
      telemetry->setRenderGraph(subs_.frameRecorder->graph());
    }
  }

  VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  VkPerformanceQuerySubmitInfoKHR performanceSubmitInfo{};
  if (subs_.renderPassGpuProfiler &&
      subs_.renderPassGpuProfiler->usesPerformanceQueries()) {
    performanceSubmitInfo.sType =
        VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR;
    performanceSubmitInfo.counterPassIndex = 0u;
    submitInfo.pNext = &performanceSubmitInfo;
  }

  VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  waitInfo.semaphore = subs_.frameSyncManager->imageAvailable(frame_.currentFrame);
  waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  submitInfo.waitSemaphoreInfoCount = 1;
  submitInfo.pWaitSemaphoreInfos = &waitInfo;

  VkCommandBuffer cmdHandle = svc_.commandBufferManager.buffer(imageIndex);
  VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  commandInfo.commandBuffer = cmdHandle;
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &commandInfo;

  VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signalInfo.semaphore = subs_.frameSyncManager->renderFinishedForImage(imageIndex);
  signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  submitInfo.signalSemaphoreInfoCount = 1;
  submitInfo.pSignalSemaphoreInfos = &signalInfo;

  phaseStart = TelemetryClock::now();
  subs_.frameSyncManager->resetFence(frame_.currentFrame);
  const VkFence submittedFrameFence =
      subs_.frameSyncManager->fence(frame_.currentFrame);
  if (vkQueueSubmit2(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    submittedFrameFence) != VK_SUCCESS) {
    throw std::runtime_error("failed to submit draw command buffer!");
  }
  subs_.temporalManager->commit();
  gfxJournal_.submitted();
  frame_.imagesInFlight[imageIndex] = submittedFrameFence;
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::QueueSubmit,
                           elapsedMilliseconds(phaseStart));
  }

  if (screenshotThisFrame) {
    phaseStart = TelemetryClock::now();
    const VkFence fence = subs_.frameSyncManager->fence(frame_.currentFrame);
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1, &fence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      throw std::runtime_error("failed to wait for screenshot frame");
    }
    writePendingScreenshotPng(frame_.currentFrame);
    if (telemetry) {
      telemetry->setCpuPhase(RendererTelemetryPhase::Screenshot,
                             elapsedMilliseconds(phaseStart));
    }
  }

  phaseStart = TelemetryClock::now();
  result = svc_.swapChainManager.present(
      svc_.ctx.deviceWrapper->presentQueue(), imageIndex,
      subs_.frameSyncManager->renderFinishedForImage(imageIndex));
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Present,
                           elapsedMilliseconds(phaseStart));
  }

  const VkResult actualPresentResult = result;
  if (std::exchange(capturePresentSuboptimal_, false) && result == VK_SUCCESS)
    result = VK_SUBOPTIMAL_KHR;
  if (gfxJournal_.enabled())
    gfxJournal_.presented(static_cast<int32_t>(actualPresentResult),
                          static_cast<int32_t>(result), imageIndex,
                          frame_.currentFrame, captureTelemetry());
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
      framebufferResized) {
    framebufferResized = false;
    phaseStart = TelemetryClock::now();
    handleResize();
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  } else if (result != VK_SUCCESS) {
    throw std::runtime_error("failed to present swap chain image!");
  } else {
    markDepthVisibilityFrameComplete(imageIndex, frame_.currentFrame);
  }

  if (telemetry) {
    RendererWorkloadTelemetry workload{};
    if (subs_.sceneController) {
      workload.objectCount =
          saturatingU32(subs_.sceneController->objectData().size());
      workload.opaqueDrawCount =
          saturatingU32(subs_.sceneController->opaqueDrawCommands().size());
      workload.transparentDrawCount = saturatingU32(
          subs_.sceneController->transparentDrawCommands().size());
      workload.totalDrawCount =
          saturatingU32(static_cast<size_t>(workload.opaqueDrawCount) +
                        static_cast<size_t>(workload.transparentDrawCount));
    }
    if (subs_.lightingManager) {
      workload.submittedLights =
          subs_.lightingManager->lightCullingStats().submittedLights;
    }
    telemetry->setWorkload(workload);

    RendererResourceTelemetry resources{};
    const VkExtent2D extent = svc_.swapChainManager.extent();
    resources.swapchainWidth = extent.width;
    resources.swapchainHeight = extent.height;
    resources.swapchainImageCount =
        saturatingU32(svc_.swapChainManager.imageCount());
    resources.cameraBufferCount = saturatingU32(buffers_.cameras.size());
    resources.objectBufferCapacity = saturatingU32(maxSceneObjectCapacity());
    resources.oitNodeCapacity = deferredRasterRuntimeOitNodeCapacity(
        subs_.frameResourceManager.get(), imageIndex);
    telemetry->setResources(resources);
    telemetry->setCpuPhase(RendererTelemetryPhase::Frame,
                           elapsedMilliseconds(frameStart));
    telemetry->endFrame();
  }

  frame_.currentFrame =
      (frame_.currentFrame + 1) % svc_.config.maxFramesInFlight;
  ++frame_.submittedFrameCount;
  return true;
}

void RendererFrontend::applyTemporalCapture(
    const container::temporal::CaptureSample &sample) {
  // Capture events can replace shared scene/provider buffers. Retire submitted
  // readers with the existing frame-fence policy before changing those buffers.
  FrameConcurrencyPolicy::serializedGpuResources("capture scene mutation")
      .waitBeforeAcquire(*subs_.frameSyncManager, frame_.currentFrame);
  const auto &event = sample.event;
  if (!event.reload.empty()) {
    captureObjectBase_.reset();
    reloadSceneModel(event.reload);
  }
  if (event.taa)
    subs_.temporalManager->settings().enabled = *event.taa;
  if (event.samples)
    recreateMsaaResources(sampleCountFromSamples(*event.samples));
  if (event.reset)
    subs_.temporalManager->reset("capture sequence reset");
  captureAcquireOutOfDate_ = event.acquireOutOfDate;
  capturePresentSuboptimal_ = event.presentSuboptimal;
  if (event.sectionPlane)
    captureSectionPlane_ = *event.sectionPlane;
  if (event.bimHiddenObject)
    captureBimHiddenObject_ = *event.bimHiddenObject;
  if (event.bimLodBias)
    captureBimLodBias_ = *event.bimLodBias;
  if (!event.technique.empty()) {
    const auto technique = renderTechniqueIdFromName(event.technique);
    if (!technique)
      throw std::invalid_argument("Invalid capture technique");
    pendingRenderTechniqueChange_ = *technique;
  }
  if (event.orthographic)
    subs_.cameraController->setOrthographic(sceneState_.selectedMeshNode,
                                            *event.orthographic);
  if (sample.camera) {
    container::app::AppConfig poseConfig = svc_.config;
    poseConfig.hasCameraOverride = true;
    for (uint32_t component = 0; component < 3; ++component) {
      poseConfig.cameraPosition[component] = sample.camera->position[component];
      poseConfig.cameraTarget[component] = sample.camera->target[component];
    }
    applyCameraOverride(subs_.cameraController->camera(), poseConfig);
  }
  uint32_t objectNode = sample.objectNode;
  if (!sample.objectName.empty()) {
    bool found = false;
    for (uint32_t node = 0; node < sceneGraph_.nodeCount(); ++node)
      if (sceneGraph_.getNode(node)->name == sample.objectName) {
        objectNode = node;
        found = true;
        break;
      }
    if (!found)
      throw std::invalid_argument("Capture objectName does not exist");
  }
  if (sample.objectTranslation) {
    const uint32_t nodeIndex = objectNode;
    const auto *node = sceneGraph_.getNode(nodeIndex);
    if (!node)
      throw std::invalid_argument("Capture objectNode does not exist");
    if (!captureObjectBase_ || captureObjectNode_ != nodeIndex) {
      captureObjectBase_ = node->localTransform;
      captureObjectNode_ = nodeIndex;
    }
    glm::mat4 transform = *captureObjectBase_;
    transform[3] += glm::vec4(*sample.objectTranslation, 0);
    sceneGraph_.setLocalTransform(nodeIndex, transform);
    sceneGraph_.updateWorldTransforms();
    refreshSceneObjectData();
  }
  if (event.objectVisible) {
    sceneGraph_.setVisible(objectNode, *event.objectVisible);
    refreshSceneObjectData();
  }
  if (sample.bimTranslation && subs_.bimManager)
    subs_.bimManager->setRootTranslation(*sample.bimTranslation);
  if (event.exposure)
    captureExposure_ = *event.exposure;
}

nlohmann::json RendererFrontend::captureTelemetry() const {
  const auto &temporal = *subs_.temporalManager;
  const auto &settings = temporal.settings();
  const auto extent = svc_.swapChainManager.extent();
  VkPhysicalDeviceProperties gpu{};
  vkGetPhysicalDeviceProperties(svc_.ctx.deviceWrapper->physicalDevice(), &gpu);
  nlohmann::json json{
      {"schemaVersion", 1},
      {"gpu", gpu.deviceName},
      {"driverVersion", gpu.driverVersion},
      {"vulkanApiVersion", gpu.apiVersion},
      {"technique", std::string(subs_.activeTechnique->name())},
      {"resolution", {extent.width, extent.height}},
      {"msaaSamples", sampleCountToSamples(msaaSampleCount_)},
      {"taa",
       {{"enabled", settings.enabled},
        {"historyWeight", settings.historyWeight},
        {"varianceGamma", settings.varianceGamma},
        {"depthAbsoluteTolerance", settings.depthAbsoluteTolerance},
        {"depthRelativeTolerance", settings.depthRelativeTolerance},
        {"jitterSeed", settings.jitterSeed},
        {"submittedFrame", temporal.state().frameId()},
        {"epoch", temporal.state().epoch()},
        {"resetReason", temporal.state().resetReason()},
        {"imagePayloadBytes", temporal.memoryBytes()},
        {"allocatedImageBytes", temporal.allocatedBytes()}}}};
  if (subs_.lightingManager) {
    const auto &lighting = subs_.lightingManager->lightingData();
    json["lighting"] = {
        {"directionalColor", {lighting.directionalColorIntensity.x, lighting.directionalColorIntensity.y, lighting.directionalColorIntensity.z}},
        {"directionalDirection", {lighting.directionalDirection.x, lighting.directionalDirection.y, lighting.directionalDirection.z}},
        {"directionalIntensity", lighting.directionalColorIntensity.w},
        {"environmentIntensity", lighting.environmentIntensity},
        {"bounceIntensity", lighting.bounceIntensity},
        {"pointLightCount", lighting.pointLightCount},
        {"areaLightCount", lighting.areaLightCount},
        {"localShadowLayerBudget",
         subs_.lightingManager->lightingSettings().localShadowLayerBudget}};
    if (subs_.shadowManager)
      json["lighting"]["activeLocalShadowLayers"] =
          subs_.shadowManager->localShadowData().counts.x;
    if (svc_.config.gfxrecon.enabled()) {
      auto vector = [](const glm::vec4& value) {
        return nlohmann::json::array({value.x, value.y, value.z, value.w});
      };
      json["lighting"]["pointLights"] = nlohmann::json::array();
      for (const auto& light : subs_.lightingManager->pointLightsSsbo())
        json["lighting"]["pointLights"].push_back({
            {"positionRadius", vector(light.positionRadius)},
            {"colorIntensity", vector(light.colorIntensity)},
            {"directionInnerCos", vector(light.directionInnerCos)},
            {"coneOuterCosType", vector(light.coneOuterCosType)}});
      json["lighting"]["areaLights"] = nlohmann::json::array();
      for (const auto& light : subs_.lightingManager->areaLightsSsbo())
        json["lighting"]["areaLights"].push_back({
            {"positionRange", vector(light.positionRange)},
            {"colorIntensity", vector(light.colorIntensity)},
            {"directionType", vector(light.directionType)},
            {"tangentHalfSize", vector(light.tangentHalfSize)},
            {"bitangentHalfSize", vector(light.bitangentHalfSize)}});
      const auto shadow = subs_.guiManager ? subs_.guiManager->shadowSettings()
                                           : container::gpu::ShadowSettings{};
      json["shadows"] = {
          {"directionalEnabled", lighting.shadowEnabled != 0},
          {"localEnabled", lighting.localShadowEnabled != 0},
          {"normalBiasMinTexels", shadow.normalBiasMinTexels},
          {"normalBiasMaxTexels", shadow.normalBiasMaxTexels},
          {"slopeBiasScale", shadow.slopeBiasScale},
          {"receiverPlaneBiasScale", shadow.receiverPlaneBiasScale},
          {"filterRadiusTexels", shadow.filterRadiusTexels},
          {"cascadeBlendFraction", shadow.cascadeBlendFraction},
          {"constantDepthBias", shadow.constantDepthBias},
          {"maxDepthBias", shadow.maxDepthBias},
          {"rasterConstantBias", shadow.rasterConstantBias},
          {"rasterSlopeBias", shadow.rasterSlopeBias},
          {"directionalPcssEnabled", shadow.directionalPcssEnabled},
          {"directionalPcssLightRadiusDegrees", shadow.directionalPcssLightRadiusDegrees},
          {"directionalPcssBlockerSearchRadiusTexels", shadow.directionalPcssBlockerSearchRadiusTexels},
          {"directionalPcssMaxFilterRadiusTexels", shadow.directionalPcssMaxFilterRadiusTexels},
          {"directionalContactVisibility", shadow.directionalContactVisibility},
          {"directionalContactMaxDistance", shadow.directionalContactMaxDistance},
          {"directionalContactThickness", shadow.directionalContactThickness},
          {"directionalContactFadeDistance", shadow.directionalContactFadeDistance},
          {"localContactVisibility", shadow.localContactVisibility}};
      if (subs_.environmentManager)
        json["environment"] = {
            {"hdrPath", container::app::kDefaultEnvironmentHdrRelativePath},
            {"status", subs_.environmentManager->environmentStatus()}};
    }
  }
  if (subs_.rendererTelemetry) {
    const auto &snapshot = subs_.rendererTelemetry->latest();
    json["gpuKnownMs"] = snapshot.timing.gpuKnownMs;
    json["cpuPhasesMs"] = nlohmann::json::object();
    for (size_t phase = 0; phase < snapshot.timing.cpuMs.size(); ++phase)
      json["cpuPhasesMs"][std::string(rendererTelemetryPhaseName(
          static_cast<RendererTelemetryPhase>(phase)))] =
          snapshot.timing.cpuMs[phase];
    json["passes"] = nlohmann::json::array();
    for (const auto &pass : snapshot.passes)
      json["passes"].push_back({{"name", pass.name},
                                {"active", pass.active},
                                {"gpuTimed", pass.gpuTimed},
                                {"gpuMs", pass.gpuKnownMs},
                                {"cpuMs", pass.cpuRecordMs},
                                {"status", pass.status}});
    json["swapchainRecreates"] = snapshot.sync.swapchainRecreateCount;
    json["deviceIdleWaits"] = snapshot.sync.deviceWaitIdleCount;
    json["framesInFlight"] = snapshot.sync.maxFramesInFlight;
    json["serializedConcurrency"] = snapshot.sync.serializedConcurrency;
    json["swapchainImages"] = svc_.swapChainManager.imageCount();
    json["taa"]["imageAllocationCount"] = temporal.memoryBytes() == 0 ? 0 : 6 + 5 * svc_.swapChainManager.imageCount();
    bool temporalWrites = false;
    for (const auto& pass : snapshot.passes) temporalWrites |= pass.name == "TemporalResolve" && pass.active;
    json["taa"]["logicalImageWriteBytesPerFrame"] = temporalWrites ? uint64_t(extent.width) * extent.height * 57 : 0;
  }
  json["scene"] = {{"primary", activePrimaryModelPath_}, {"auxiliary", activeAuxiliaryModelPath_},
                    {"primaryImportScale", activePrimaryImportScale_}, {"auxiliaryImportScale", activeAuxiliaryImportScale_}};
  const auto& camera = buffers_.cameraData;
  json["camera"] = {{"position", {camera.cameraWorldPosition.x, camera.cameraWorldPosition.y, camera.cameraWorldPosition.z}},
                     {"jitterUv", {camera.jitterUv.x, camera.jitterUv.y, camera.jitterUv.z, camera.jitterUv.w}}};
  auto matrix = [](const glm::mat4& value) {
    nlohmann::json columns = nlohmann::json::array();
    for (int c = 0; c < 4; ++c) columns.push_back({value[c][0], value[c][1], value[c][2], value[c][3]});
    return columns;
  };
  json["camera"]["viewProjColumns"] = matrix(camera.viewProj);
  json["camera"]["unjitteredViewProjColumns"] = matrix(camera.unjitteredViewProj);
  return json;
}

void RendererFrontend::writeCaptureTelemetry(
    const std::filesystem::path &path) const {
  const auto json = captureTelemetry();
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path);
  if (!stream)
    throw std::runtime_error("Cannot write capture telemetry");
  stream << json.dump(2);
  if (!stream)
    throw std::runtime_error("Capture telemetry write failed");
}

void RendererFrontend::startGfxCapture() {
  const auto& capture = svc_.config.gfxrecon;
  if (!capture.enabled()) return;
  auto runtime = captureTelemetry();
  runtime["build"] = {{"revision", CONTAINER_GIT_REVISION}, {"configuration", CONTAINER_BUILD_CONFIG},
                      {"compiler", CONTAINER_BUILD_COMPILER}};
  runtime["validationEnabled"] = svc_.config.enableValidationLayers;
  runtime["fixedTimestepSeconds"] = (svc_.config.screenshotCapturePath.empty() && svc_.config.temporalCaptureSequencePath.empty())
      ? nlohmann::json(nullptr) : nlohmann::json(svc_.config.screenshotFixedTimestepSeconds);
  gfxJournal_.open(capture, std::move(runtime));
  if (subs_.guiManager)
    subs_.guiManager->setGfxCaptureStatus(
        "GFXReconstruct " + capture.toolVersion + " armed (" + capture.mode + "). " +
        (capture.mode == "all" ? "All presentation boundaries" :
         capture.trigger.empty() ? "Selected frames: " + capture.frames : "Start/stop hotkey: " + capture.trigger) +
        "\nOutput: " + capture.outputDirectory.string() +
        "\nRecording feedback: layer.log. Recording state is not exposed by this layer API.");
  container::log::ContainerLogger::instance().renderer()->info(
      "GFXReconstruct {} armed: mode={}, frames={}, hotkey={}, output={}. Recording state is reported by the layer log.",
      capture.toolVersion, capture.mode, capture.frames, capture.trigger, capture.outputDirectory.string());
}
void RendererFrontend::gfxCaptureTick(uint64_t tick, bool skipped) {
  if (gfxJournal_.enabled()) gfxJournal_.tick(tick, skipped);
}
bool RendererFrontend::gfxCaptureComplete() const {
  const auto end = svc_.config.gfxrecon.stopAfterPresent;
  return end != 0 && gfxJournal_.presentCalls() >= end;
}

void RendererFrontend::handleResize() {
  if (subs_.rendererTelemetry) {
    subs_.rendererTelemetry->noteDeviceWaitIdle();
    subs_.rendererTelemetry->noteSwapchainRecreate();
  }
  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());

  destroyGBufferResources();
  invalidateDepthVisibilityFrames();
  svc_.swapChainManager.recreate(resources_.renderPasses.postProcess);
  ensureCameraBuffers();
  if (subs_.lightingManager) {
    subs_.lightingManager->createDescriptorResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  if (subs_.shadowManager) {
    subs_.shadowManager->recreatePerFrameResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  if (subs_.shadowCullManager) {
    subs_.shadowCullManager->recreatePerFrameResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
    std::fill(buffers_.shadowObjectDescriptorReady.begin(),
              buffers_.shadowObjectDescriptorReady.end(), false);
  }
  if (subs_.gpuCullManager) {
    subs_.gpuCullManager->recreatePerFrameResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  createFrameResources();
  if (subs_.environmentManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.environmentManager->recreateGtaoTextures(ext.width, ext.height);
  }
  if (subs_.bloomManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.bloomManager->createTextures(ext.width, ext.height);
  }
  svc_.commandBufferManager.reallocate(svc_.swapChainManager.imageCount());

  const uint32_t imageCount =
      static_cast<uint32_t>(svc_.swapChainManager.imageCount());
  if (subs_.guiManager)
    subs_.guiManager->updateSwapchainImageCount(imageCount);

  subs_.frameSyncManager->recreateRenderFinishedSemaphores(
      svc_.swapChainManager.imageCount());
  frame_.imagesInFlight.assign(svc_.swapChainManager.imageCount(),
                               VK_NULL_HANDLE);
  if (subs_.renderPassGpuProfiler) {
    subs_.renderPassGpuProfiler->recreate(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }

  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }
  updateAllObjectBuffers();
  if (subs_.sceneManager) {
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.objects);
    if (subs_.bimManager && subs_.bimManager->hasScene()) {
      subs_.sceneManager->updateAuxiliaryDescriptorSets(
          buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
    }
  }
  updateFrameDescriptorSets();
}

void RendererFrontend::processInput(float deltaTime) {
  if (!subs_.cameraController) {
    svc_.inputManager.endFrame();
    return;
  }

  // Input updates the CPU camera object only. GPU camera buffers are uploaded
  // in drawFrame() after all in-flight work has completed, so culling, depth,
  // and G-buffer passes cannot race a previous frame still reading them.
  syncCameraSelectionPivotOverride();
  subs_.cameraController->updateViewAnimation(deltaTime);
  const bool hasSelectedEditableLight =
      subs_.lightingManager &&
      subs_.lightingManager->selectedEditableLight().has_value();
  const bool hasEditableSectionPlane =
      subs_.guiManager &&
      subs_.guiManager->sectionPlaneState().enabled &&
      subs_.guiManager->sectionPlaneState().visualPlaneEditable;
  interactionController_.process(RenderSurfaceInteractionController::Context{
      .inputManager = svc_.inputManager,
      .cameraController = *subs_.cameraController,
      .debugState = debugState_,
      .deltaTime = deltaTime,
      .guiCapturingMouse =
          subs_.guiManager ? subs_.guiManager->isCapturingMouse() : false,
      .guiCapturingKeyboard =
          subs_.guiManager ? subs_.guiManager->isCapturingKeyboard() : false,
      .selectedMeshNode = sceneState_.selectedMeshNode,
      .hasSelection =
          sceneState_.selectedMeshNode !=
              container::scene::SceneGraph::kInvalidNode ||
          selectedBimObjectIndex_ != std::numeric_limits<uint32_t>::max() ||
          hasSelectedEditableLight || hasEditableSectionPlane,
      .selectAtCursor =
          [this](double cursorX, double cursorY) {
            selectMeshNodeAtCursor(cursorX, cursorY);
          },
      .hoverAtCursor =
          [this](double cursorX, double cursorY) {
            hoverMeshNodeAtCursor(cursorX, cursorY);
          },
      .clearHover = [this]() { clearHoveredMeshNode(); },
      .clearSelection = [this]() { clearSelectedMeshNode(); },
      .pickTransformGizmoAxisAtCursor =
          [this](double cursorX, double cursorY) {
            return pickTransformGizmoAxisAtCursor(cursorX, cursorY);
          },
      .transformSelectedByDrag =
          [this](container::ui::ViewportTool tool,
                 container::ui::TransformSpace space,
                 container::ui::TransformAxis axis, bool snapEnabled,
                 double deltaX, double deltaY) {
            transformSelectedNodeByDrag(tool, space, axis, snapEnabled, deltaX,
                                        deltaY);
          },
      .setStatusMessage =
          [this](std::string message) {
            if (subs_.guiManager) {
              subs_.guiManager->setStatusMessage(std::move(message));
            }
          },
      .onCullingUnfrozen =
          [this]() {
            if (subs_.gpuCullManager) {
              subs_.gpuCullManager->unfreezeCulling();
            }
          },
  });
  if (interactionController_.state().gesture !=
      container::ui::ViewportGesture::TransformDrag) {
    transformDragSession_.active = false;
  }
}

void RendererFrontend::selectMeshNodeAtCursor(double cursorX, double cursorY) {
  if (!subs_.sceneController) {
    return;
  }

  auto selectEditableLight = [&](EditableLightId id) {
    if (!subs_.lightingManager) {
      return;
    }
    subs_.lightingManager->selectEditableLight(id);
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
    selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
    selectionNavigationAnchor_ = {};
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (subs_.guiManager) {
      subs_.guiManager->setSectionPlaneVisualEditable(false);
      subs_.guiManager->setStatusMessage("Selected editable light");
    }
  };

  auto selectBimObject = [&](uint32_t objectIndex,
                             std::optional<glm::vec3> anchorPoint =
                                 std::nullopt) {
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
    selectedBimObjectIndex_ = objectIndex;
    if (subs_.lightingManager) {
      subs_.lightingManager->selectEditableLight({});
    }
    selectionNavigationAnchor_ = {};
    if (anchorPoint && isFiniteVec3(*anchorPoint) && subs_.bimManager) {
      const BimElementBounds bounds =
          subs_.bimManager->elementBoundsForObject(objectIndex);
      selectionNavigationAnchor_ = SelectionNavigationAnchor{
          .valid = true,
          .point = *anchorPoint,
          .radius = bounds.valid ? std::max(bounds.radius, 0.25f) : 1.0f,
          .sceneNode = container::scene::SceneGraph::kInvalidNode,
          .bimObject = objectIndex,
      };
    }
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (subs_.guiManager) {
      subs_.guiManager->setSectionPlaneVisualEditable(false);
      if (subs_.bimManager) {
        if (const auto *metadata =
                subs_.bimManager->metadataForObject(objectIndex)) {
          subs_.guiManager->setStatusMessage(bimSelectionLabel(*metadata));
          return;
        }
      }
      subs_.guiManager->setStatusMessage("Selected BIM object " +
                                         std::to_string(objectIndex));
    }
  };

  auto selectSceneNode = [&](uint32_t nodeIndex,
                             std::optional<glm::vec3> anchorPoint =
                                 std::nullopt) {
    sceneState_.selectedMeshNode = nodeIndex;
    selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
    if (subs_.lightingManager) {
      subs_.lightingManager->selectEditableLight({});
    }
    selectionNavigationAnchor_ = {};
    if (anchorPoint && isFiniteVec3(*anchorPoint)) {
      const SceneNodeWorldBounds bounds =
          subs_.sceneController->nodeWorldBounds(nodeIndex);
      selectionNavigationAnchor_ = SelectionNavigationAnchor{
          .valid = true,
          .point = *anchorPoint,
          .radius = bounds.valid ? std::max(bounds.radius, 0.25f) : 1.0f,
          .sceneNode = nodeIndex,
          .bimObject = std::numeric_limits<uint32_t>::max(),
      };
    }
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (subs_.guiManager) {
      subs_.guiManager->setSectionPlaneVisualEditable(false);
      subs_.guiManager->setStatusMessage("Selected node " +
                                         std::to_string(nodeIndex));
    }
  };

  const BimDrawFilter bimFilter = currentBimDrawFilter();
  glm::vec4 activeSectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  const bool sectionPlaneEnabled =
      currentSectionPlaneEquation(subs_.guiManager.get(), activeSectionPlane);
  uint32_t gpuPickId = container::gpu::kPickIdNone;
  const bool hasGpuPick = samplePickIdAtCursor(cursorX, cursorY, gpuPickId);
  if (hasGpuPick) {
    const GpuPickTarget target = decodeGpuPickId(gpuPickId);
    if (target.kind == GpuPickTargetKind::Light) {
      selectEditableLight(target.editableLightId);
      return;
    }

    std::optional<glm::vec3> gpuPickWorldPoint;
    float gpuPickDepth = 0.0f;
    if (samplePickDepthAtCursor(cursorX, cursorY, gpuPickDepth) ||
        sampleDepthAtCursor(cursorX, cursorY, gpuPickDepth)) {
      if (const auto* depthFrame =
              depthVisibilityFrameSlot(depthVisibility_.latestFrameSlot)) {
        gpuPickWorldPoint = unprojectDepthAtCursor(
            depthFrame->cameraData, depthFrame->extent, cursorX, cursorY,
            gpuPickDepth);
      }
    }

    if (target.kind == GpuPickTargetKind::Bim && subs_.bimManager &&
        target.objectIndex < subs_.bimManager->objectData().size() &&
        subs_.bimManager->objectMatchesFilter(target.objectIndex, bimFilter) &&
        bimObjectVisibleByLayer(target.objectIndex)) {
      selectBimObject(target.objectIndex, gpuPickWorldPoint);
      return;
    }
    if (target.kind == GpuPickTargetKind::Scene) {
      const uint32_t nodeIndex =
          subs_.sceneController->nodeIndexForObject(target.objectIndex);
      if (nodeIndex != container::scene::SceneGraph::kInvalidNode) {
        selectSceneNode(nodeIndex, gpuPickWorldPoint);
        return;
      }
    }

    clearSelectedMeshNode();
    return;
  }

  SceneNodePickHit sceneHit = subs_.sceneController->pickRenderableNodeHit(
      buffers_.cameraData, svc_.swapChainManager.extent(), cursorX, cursorY,
      sectionPlaneEnabled, activeSectionPlane);
  BimPickHit bimHit =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->pickRenderableObject(
                buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
                cursorY, sectionPlaneEnabled, activeSectionPlane)
          : BimPickHit{};

  float sampledDepth = 0.0f;
  if (sampleDepthAtCursor(cursorX, cursorY, sampledDepth)) {
    if (sceneHit.hit && !depthHitVisible(sceneHit.depth, sampledDepth)) {
      sceneHit.hit = false;
    }
    if (bimHit.hit && !depthHitVisible(bimHit.depth, sampledDepth)) {
      bimHit.hit = false;
    }
  }
  if (bimHit.hit && subs_.bimManager &&
      (!subs_.bimManager->objectMatchesFilter(bimHit.objectIndex, bimFilter) ||
       !bimObjectVisibleByLayer(bimHit.objectIndex))) {
    bimHit.hit = false;
  }

  if (!sceneHit.hit && !bimHit.hit) {
    clearSelectedMeshNode();
    return;
  }

  if (bimHit.hit && (!sceneHit.hit || bimHit.distance < sceneHit.distance)) {
    selectBimObject(bimHit.objectIndex,
                    bimHit.hasWorldPosition
                        ? std::optional<glm::vec3>{bimHit.worldPosition}
                        : std::nullopt);
    return;
  }

  selectSceneNode(sceneHit.nodeIndex,
                  sceneHit.hasWorldPosition
                      ? std::optional<glm::vec3>{sceneHit.worldPosition}
                      : std::nullopt);
}

void RendererFrontend::hoverMeshNodeAtCursor(double cursorX, double cursorY) {
  if (!subs_.sceneController) {
    clearHoveredMeshNode();
    return;
  }

  const uint64_t objectRevision = subs_.sceneController->objectDataRevision();
  const uint64_t bimObjectRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  const BimDrawFilter bimFilter = currentBimDrawFilter();
  const auto currentBimLayers =
      subs_.guiManager ? subs_.guiManager->bimLayerVisibilityState()
                       : container::ui::BimLayerVisibilityState{};
  glm::vec4 activeSectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  const bool sectionPlaneEnabled =
      currentSectionPlaneEquation(subs_.guiManager.get(), activeSectionPlane);
  if (hoverPickCache_.valid && hoverPickCache_.cursorX == cursorX &&
      hoverPickCache_.cursorY == cursorY &&
      hoverPickCache_.selectedMeshNode == sceneState_.selectedMeshNode &&
      hoverPickCache_.selectedBimObjectIndex == selectedBimObjectIndex_ &&
      hoverPickCache_.objectDataRevision == objectRevision &&
      hoverPickCache_.bimObjectDataRevision == bimObjectRevision &&
      hoverPickCache_.bimTypeFilterEnabled == bimFilter.typeFilterEnabled &&
      hoverPickCache_.bimFilterType == bimFilter.type &&
      hoverPickCache_.bimStoreyFilterEnabled == bimFilter.storeyFilterEnabled &&
      hoverPickCache_.bimFilterStorey == bimFilter.storey &&
      hoverPickCache_.bimMaterialFilterEnabled ==
          bimFilter.materialFilterEnabled &&
      hoverPickCache_.bimFilterMaterial == bimFilter.material &&
      hoverPickCache_.bimDisciplineFilterEnabled ==
          bimFilter.disciplineFilterEnabled &&
      hoverPickCache_.bimFilterDiscipline == bimFilter.discipline &&
      hoverPickCache_.bimDisciplinePreset == bimFilter.disciplinePreset &&
      hoverPickCache_.bimPhaseFilterEnabled == bimFilter.phaseFilterEnabled &&
      hoverPickCache_.bimFilterPhase == bimFilter.phase &&
      hoverPickCache_.bimPhaseTimelineEnabled ==
          bimFilter.phaseTimelineEnabled &&
      hoverPickCache_.bimPhaseTimelineActiveIndex ==
          bimFilter.phaseTimelineActiveIndex &&
      hoverPickCache_.bimPhaseTimelineShowExisting ==
          bimFilter.phaseTimelineShowExisting &&
      hoverPickCache_.bimPhaseTimelineShowNew ==
          bimFilter.phaseTimelineShowNew &&
      hoverPickCache_.bimPhaseTimelineShowDemolished ==
          bimFilter.phaseTimelineShowDemolished &&
      hoverPickCache_.bimPhaseTimelineGhostFuture ==
          bimFilter.phaseTimelineGhostFuture &&
      hoverPickCache_.bimFireRatingFilterEnabled ==
          bimFilter.fireRatingFilterEnabled &&
      hoverPickCache_.bimFilterFireRating == bimFilter.fireRating &&
      hoverPickCache_.bimLoadBearingFilterEnabled ==
          bimFilter.loadBearingFilterEnabled &&
      hoverPickCache_.bimFilterLoadBearing == bimFilter.loadBearing &&
      hoverPickCache_.bimStatusFilterEnabled == bimFilter.statusFilterEnabled &&
      hoverPickCache_.bimFilterStatus == bimFilter.status &&
      hoverPickCache_.bimDrawBudgetEnabled == bimFilter.drawBudgetEnabled &&
      hoverPickCache_.bimDrawBudgetMaxObjects ==
          bimFilter.drawBudgetMaxObjects &&
      hoverPickCache_.bimIsolateSelection == bimFilter.isolateSelection &&
      hoverPickCache_.bimHideSelection == bimFilter.hideSelection &&
      hoverPickCache_.bimPointCloudVisible ==
          currentBimLayers.pointCloudVisible &&
      hoverPickCache_.bimCurvesVisible == currentBimLayers.curvesVisible &&
      hoverPickCache_.sectionPlaneEnabled == sectionPlaneEnabled &&
      (!sectionPlaneEnabled ||
       sameVec4(hoverPickCache_.sectionPlane, activeSectionPlane)) &&
      sameCameraData(hoverPickCache_.cameraData, buffers_.cameraData)) {
    return;
  }

  hoverPickCache_ = HoverPickCache{
      .valid = true,
      .cursorX = cursorX,
      .cursorY = cursorY,
      .selectedMeshNode = sceneState_.selectedMeshNode,
      .selectedBimObjectIndex = selectedBimObjectIndex_,
      .objectDataRevision = objectRevision,
      .bimObjectDataRevision = bimObjectRevision,
      .bimTypeFilterEnabled = bimFilter.typeFilterEnabled,
      .bimFilterType = bimFilter.type,
      .bimStoreyFilterEnabled = bimFilter.storeyFilterEnabled,
      .bimFilterStorey = bimFilter.storey,
      .bimMaterialFilterEnabled = bimFilter.materialFilterEnabled,
      .bimFilterMaterial = bimFilter.material,
      .bimDisciplineFilterEnabled = bimFilter.disciplineFilterEnabled,
      .bimFilterDiscipline = bimFilter.discipline,
      .bimDisciplinePreset = bimFilter.disciplinePreset,
      .bimPhaseFilterEnabled = bimFilter.phaseFilterEnabled,
      .bimFilterPhase = bimFilter.phase,
      .bimPhaseTimelineEnabled = bimFilter.phaseTimelineEnabled,
      .bimPhaseTimelineActiveIndex = bimFilter.phaseTimelineActiveIndex,
      .bimPhaseTimelineShowExisting = bimFilter.phaseTimelineShowExisting,
      .bimPhaseTimelineShowNew = bimFilter.phaseTimelineShowNew,
      .bimPhaseTimelineShowDemolished =
          bimFilter.phaseTimelineShowDemolished,
      .bimPhaseTimelineGhostFuture = bimFilter.phaseTimelineGhostFuture,
      .bimFireRatingFilterEnabled = bimFilter.fireRatingFilterEnabled,
      .bimFilterFireRating = bimFilter.fireRating,
      .bimLoadBearingFilterEnabled = bimFilter.loadBearingFilterEnabled,
      .bimFilterLoadBearing = bimFilter.loadBearing,
      .bimStatusFilterEnabled = bimFilter.statusFilterEnabled,
      .bimFilterStatus = bimFilter.status,
      .bimDrawBudgetEnabled = bimFilter.drawBudgetEnabled,
      .bimDrawBudgetMaxObjects = bimFilter.drawBudgetMaxObjects,
      .bimIsolateSelection = bimFilter.isolateSelection,
      .bimHideSelection = bimFilter.hideSelection,
      .bimPointCloudVisible = currentBimLayers.pointCloudVisible,
      .bimCurvesVisible = currentBimLayers.curvesVisible,
      .sectionPlaneEnabled = sectionPlaneEnabled,
      .sectionPlane = activeSectionPlane,
      .cameraData = buffers_.cameraData,
  };

  uint32_t hoveredNode = container::scene::SceneGraph::kInvalidNode;
  uint32_t hoveredBimObject = std::numeric_limits<uint32_t>::max();

  uint32_t gpuPickId = container::gpu::kPickIdNone;
  const bool hasGpuPick = samplePickIdAtCursor(cursorX, cursorY, gpuPickId);
  if (hasGpuPick) {
    const GpuPickTarget target = decodeGpuPickId(gpuPickId);
    if (target.kind == GpuPickTargetKind::Bim && subs_.bimManager &&
        target.objectIndex < subs_.bimManager->objectData().size() &&
        subs_.bimManager->objectMatchesFilter(target.objectIndex, bimFilter) &&
        bimObjectVisibleByLayer(target.objectIndex)) {
      hoveredBimObject = target.objectIndex;
    } else if (target.kind == GpuPickTargetKind::Scene) {
      hoveredNode =
          subs_.sceneController->nodeIndexForObject(target.objectIndex);
    }
  } else {
    const SceneNodePickHit sceneHit =
        subs_.sceneController->pickRenderableNodeHit(
            buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
            cursorY, sectionPlaneEnabled, activeSectionPlane);
    BimPickHit bimHit =
        (subs_.bimManager && subs_.bimManager->hasScene())
            ? subs_.bimManager->pickRenderableObject(
                  buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
                  cursorY, sectionPlaneEnabled, activeSectionPlane)
            : BimPickHit{};
    if (bimHit.hit && subs_.bimManager &&
        (!subs_.bimManager->objectMatchesFilter(bimHit.objectIndex,
                                                bimFilter) ||
         !bimObjectVisibleByLayer(bimHit.objectIndex))) {
      bimHit.hit = false;
    }

    if (bimHit.hit && (!sceneHit.hit || bimHit.distance < sceneHit.distance)) {
      hoveredBimObject = bimHit.objectIndex;
    } else if (sceneHit.hit) {
      hoveredNode = sceneHit.nodeIndex;
    }
  }

  if (hoveredNode == sceneState_.selectedMeshNode) {
    hoveredNode = container::scene::SceneGraph::kInvalidNode;
  }
  if (hoveredBimObject != std::numeric_limits<uint32_t>::max() &&
      selectedBimObjectIndex_ != std::numeric_limits<uint32_t>::max() &&
      subs_.bimManager) {
    const auto *selectedMetadata =
        subs_.bimManager->metadataForObject(selectedBimObjectIndex_);
    const auto *hoveredMetadata =
        subs_.bimManager->metadataForObject(hoveredBimObject);
    if (selectedMetadata != nullptr && hoveredMetadata != nullptr &&
        sameBimProductIdentity(*selectedMetadata, *hoveredMetadata)) {
      hoveredBimObject = std::numeric_limits<uint32_t>::max();
    }
  }
  if (hoveredMeshNode_ == hoveredNode &&
      hoveredBimObjectIndex_ == hoveredBimObject) {
    return;
  }

  hoveredMeshNode_ = hoveredNode;
  hoveredBimObjectIndex_ = hoveredBimObject;
  hoveredDrawCommands_.clear();
  hoveredBimDrawCommands_.clear();
  hoveredBimNativePointDrawCommands_.clear();
  hoveredBimNativeCurveDrawCommands_.clear();
}

void RendererFrontend::clearHoveredMeshNode() {
  hoverPickCache_.valid = false;
  if (hoveredMeshNode_ == container::scene::SceneGraph::kInvalidNode &&
      hoveredBimObjectIndex_ == std::numeric_limits<uint32_t>::max()) {
    return;
  }

  hoveredMeshNode_ = container::scene::SceneGraph::kInvalidNode;
  hoveredBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
  hoveredDrawCommands_.clear();
  hoveredBimDrawCommands_.clear();
  hoveredBimNativePointDrawCommands_.clear();
  hoveredBimNativeCurveDrawCommands_.clear();
}

void RendererFrontend::clearSelectedMeshNode() {
  selectionNavigationAnchor_ = {};
  const bool hasSelectedEditableLight =
      subs_.lightingManager &&
      subs_.lightingManager->selectedEditableLight().has_value();
  const bool hasEditableSectionPlane =
      subs_.guiManager &&
      subs_.guiManager->sectionPlaneState().visualPlaneEditable;
  if (sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode &&
      selectedBimObjectIndex_ == std::numeric_limits<uint32_t>::max() &&
      !hasSelectedEditableLight && !hasEditableSectionPlane) {
    return;
  }

  sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
  selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
  if (subs_.lightingManager) {
    subs_.lightingManager->selectEditableLight({});
  }
  if (subs_.guiManager) {
    subs_.guiManager->setSectionPlaneVisualEditable(false);
  }
  clearHoveredMeshNode();
  selectedDrawCommands_.clear();
  selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear();
  selectedBimNativeCurveDrawCommands_.clear();
  if (subs_.guiManager) {
    subs_.guiManager->setStatusMessage("Selection cleared");
  }
}

void RendererFrontend::transformSelectedNodeByDrag(
    container::ui::ViewportTool tool, container::ui::TransformSpace space,
    container::ui::TransformAxis axis, bool snapEnabled, double deltaX,
    double deltaY) {
  if (!subs_.cameraController) {
    return;
  }

  const std::optional<EditableLightEntity> selectedEditableLight =
      subs_.lightingManager ? subs_.lightingManager->selectedEditableLight()
                            : std::nullopt;
  const bool transformingEditableLight =
      sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode &&
      selectedEditableLight.has_value();
  const bool transformingSectionPlane =
      subs_.guiManager && subs_.guiManager->sectionPlaneState().enabled &&
      subs_.guiManager->sectionPlaneState().visualPlaneEditable &&
      sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode &&
      !selectedEditableLight.has_value();
  if (!transformingSectionPlane && !transformingEditableLight &&
      sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode) {
    return;
  }

  const uint32_t selectedNode =
      transformingSectionPlane
          ? kSectionPlaneTransformNode
          : (transformingEditableLight ? kEditableLightTransformNode
                                       : sceneState_.selectedMeshNode);
  auto currentControls =
      transformingSectionPlane
          ? container::ui::TransformControls{
                .position = sectionPlaneOrigin(
                    subs_.guiManager->sectionPlaneState()),
                .rotationDegrees = glm::vec3(0.0f),
                .scale = glm::vec3(std::max(
                    subs_.guiManager->sectionPlaneState().visualPlaneSize,
                    0.1f))}
          : (transformingEditableLight
                 ? container::ui::TransformControls{
                       .position = selectedEditableLight->position,
                       .rotationDegrees = glm::vec3(0.0f),
                       .scale = glm::vec3(
                           std::max(selectedEditableLight->range, 1.0f))}
                 : subs_.cameraController->nodeTransformControls(
                       sceneState_.selectedMeshNode));
  if (!transformDragSession_.active ||
      transformDragSession_.nodeIndex != selectedNode ||
      transformDragSession_.tool != tool ||
      transformDragSession_.space != space ||
      transformDragSession_.axis != axis ||
      transformDragSession_.snapEnabled != snapEnabled) {
    const FrameTransformGizmoState gizmo = buildTransformGizmoState();
    transformDragSession_ = TransformDragSession{
        .active = true,
        .nodeIndex = selectedNode,
        .tool = tool,
        .space = space,
        .axis = axis,
        .snapEnabled = snapEnabled,
        .startControls = currentControls,
        .startSectionPlane =
            transformingSectionPlane ? subs_.guiManager->sectionPlaneState()
                                     : container::ui::SectionPlaneState{},
        .origin = gizmo.visible ? gizmo.origin : currentControls.position,
        .gizmoScale = std::max(gizmo.scale, 0.0001f),
        .axisX = normalizedOr(gizmo.axisX, {1.0f, 0.0f, 0.0f}),
        .axisY = normalizedOr(gizmo.axisY, {0.0f, 1.0f, 0.0f}),
        .axisZ = normalizedOr(gizmo.axisZ, {0.0f, 0.0f, 1.0f}),
    };
  }

  transformDragSession_.accumulatedDeltaX += deltaX;
  transformDragSession_.accumulatedDeltaY += deltaY;

  auto controls = transformDragSession_.startControls;
  const double dragDeltaX = transformDragSession_.accumulatedDeltaX;
  const double dragDeltaY = transformDragSession_.accumulatedDeltaY;

  auto transformAxisVector = [&]() {
    if (transformingSectionPlane &&
        tool == container::ui::ViewportTool::Translate) {
      return normalizedOr(transformDragSession_.startSectionPlane.normal,
                          {0.0f, 1.0f, 0.0f});
    }
    switch (axis) {
    case container::ui::TransformAxis::X:
      return transformDragSession_.axisX;
    case container::ui::TransformAxis::Y:
      return transformDragSession_.axisY;
    case container::ui::TransformAxis::Z:
      return transformDragSession_.axisZ;
    case container::ui::TransformAxis::Free:
      break;
    }
    return glm::vec3{1.0f, 0.0f, 0.0f};
  };

  auto projectedAxisDragAmount = [&](const glm::vec3 &axisVector,
                                     float fallbackScale) {
    const VkExtent2D extent = svc_.swapChainManager.extent();
    const auto startScreen = projectToFramebuffer(buffers_.cameraData, extent,
                                                  transformDragSession_.origin);
    const auto endScreen =
        projectToFramebuffer(buffers_.cameraData, extent,
                             transformDragSession_.origin +
                                 axisVector * transformDragSession_.gizmoScale);
    if (!startScreen || !endScreen) {
      return static_cast<float>(dragDeltaX - dragDeltaY) * fallbackScale;
    }

    const glm::vec2 screenAxis = *endScreen - *startScreen;
    const float screenAxisLength = glm::length(screenAxis);
    if (!std::isfinite(screenAxisLength) || screenAxisLength <= 0.0001f) {
      return static_cast<float>(dragDeltaX - dragDeltaY) * fallbackScale;
    }

    const glm::vec2 mouseScreenDelta{static_cast<float>(dragDeltaX),
                                     static_cast<float>(-dragDeltaY)};
    const float screenPixels =
        glm::dot(mouseScreenDelta, screenAxis / screenAxisLength);
    return screenPixels * (transformDragSession_.gizmoScale / screenAxisLength);
  };

  switch (tool) {
  case container::ui::ViewportTool::Select:
    return;
  case container::ui::ViewportTool::Translate: {
    const float scaleHint =
        std::max(0.05f, glm::length(transformDragSession_.startControls.scale) *
                            0.33333334f);
    const float dragScale = scaleHint * 0.01f;
    if (axis != container::ui::TransformAxis::Free) {
      const glm::vec3 axisVector = transformAxisVector();
      controls.position +=
          axisVector * projectedAxisDragAmount(axisVector, dragScale);
      break;
    }

    if (transformingSectionPlane) {
      const glm::vec3 normal =
          normalizedOr(transformDragSession_.startSectionPlane.normal,
                       {0.0f, 1.0f, 0.0f});
      controls.position +=
          normal * static_cast<float>(dragDeltaX - dragDeltaY) * dragScale;
      break;
    }

    glm::vec3 horizontal{1.0f, 0.0f, 0.0f};
    glm::vec3 vertical{0.0f, 1.0f, 0.0f};

    if (auto *camera = subs_.cameraController->camera()) {
      const glm::vec3 front = camera->frontVector();
      const glm::vec3 up = camera->upVector(front);
      horizontal = camera->rightVector(front, up);
      vertical = up;
    }

    controls.position +=
        horizontal * static_cast<float>(dragDeltaX) * dragScale;
    controls.position += vertical * static_cast<float>(dragDeltaY) * dragScale;
    break;
  }
  case container::ui::ViewportTool::Rotate: {
    const float amount = static_cast<float>(dragDeltaX - dragDeltaY) * 0.25f;
    switch (axis) {
    case container::ui::TransformAxis::X:
      controls.rotationDegrees.x += amount;
      break;
    case container::ui::TransformAxis::Y:
      controls.rotationDegrees.y += amount;
      break;
    case container::ui::TransformAxis::Z:
      controls.rotationDegrees.z += amount;
      break;
    case container::ui::TransformAxis::Free:
      controls.rotationDegrees.y += static_cast<float>(dragDeltaX) * 0.25f;
      controls.rotationDegrees.x += static_cast<float>(dragDeltaY) * 0.25f;
      break;
    }
    break;
  }
  case container::ui::ViewportTool::Scale: {
    const float factor = std::clamp(
        std::exp(static_cast<float>(dragDeltaX - dragDeltaY) * 0.005f), 0.25f,
        4.0f);
    if (axis == container::ui::TransformAxis::Free) {
      controls.scale = glm::clamp(controls.scale * factor, glm::vec3(0.001f),
                                  glm::vec3(1000.0f));
    } else {
      auto applyScaleAxis = [&](float &component) {
        component = std::clamp(component * factor, 0.001f, 1000.0f);
      };
      switch (axis) {
      case container::ui::TransformAxis::X:
        applyScaleAxis(controls.scale.x);
        break;
      case container::ui::TransformAxis::Y:
        applyScaleAxis(controls.scale.y);
        break;
      case container::ui::TransformAxis::Z:
        applyScaleAxis(controls.scale.z);
        break;
      case container::ui::TransformAxis::Free:
        break;
      }
    }
    break;
  }
  }

  if (snapEnabled) {
    switch (tool) {
    case container::ui::ViewportTool::Translate:
      controls.position =
          snapRelativeVec3(transformDragSession_.startControls.position,
                           controls.position, 0.25f);
      break;
    case container::ui::ViewportTool::Rotate:
      controls.rotationDegrees =
          snapRelativeVec3(transformDragSession_.startControls.rotationDegrees,
                           controls.rotationDegrees, 15.0f);
      break;
    case container::ui::ViewportTool::Scale:
      controls.scale =
          glm::clamp(snapRelativeVec3(transformDragSession_.startControls.scale,
                                      controls.scale, 0.1f),
                     glm::vec3(0.001f), glm::vec3(1000.0f));
      break;
    case container::ui::ViewportTool::Select:
      break;
    }
  }

  if (transformingEditableLight && subs_.lightingManager) {
    bool lightChanged = false;
    switch (tool) {
    case container::ui::ViewportTool::Select:
      break;
    case container::ui::ViewportTool::Translate: {
      const glm::vec3 delta =
          controls.position - transformDragSession_.startControls.position;
      if (glm::dot(delta, delta) > 1.0e-10f) {
        lightChanged =
            subs_.lightingManager->translateSelectedEditableLight(delta);
      }
      break;
    }
    case container::ui::ViewportTool::Rotate: {
      const glm::vec3 rotationDelta =
          controls.rotationDegrees -
          transformDragSession_.startControls.rotationDegrees;
      auto rotateAxis = [&](const glm::vec3 &axisVector, float degrees) {
        if (std::abs(degrees) > 1.0e-4f) {
          lightChanged |= subs_.lightingManager->rotateSelectedEditableLight(
              axisVector, degrees);
        }
      };
      switch (axis) {
      case container::ui::TransformAxis::X:
        rotateAxis(transformDragSession_.axisX, rotationDelta.x);
        break;
      case container::ui::TransformAxis::Y:
        rotateAxis(transformDragSession_.axisY, rotationDelta.y);
        break;
      case container::ui::TransformAxis::Z:
        rotateAxis(transformDragSession_.axisZ, rotationDelta.z);
        break;
      case container::ui::TransformAxis::Free:
        rotateAxis(transformDragSession_.axisY, rotationDelta.y);
        rotateAxis(transformDragSession_.axisX, rotationDelta.x);
        break;
      }
      break;
    }
    case container::ui::ViewportTool::Scale: {
      const float startScale =
          std::max((transformDragSession_.startControls.scale.x +
                    transformDragSession_.startControls.scale.y +
                    transformDragSession_.startControls.scale.z) *
                       0.33333334f,
                   0.001f);
      const float newScale =
          std::max((controls.scale.x + controls.scale.y + controls.scale.z) *
                       0.33333334f,
                   0.001f);
      lightChanged = subs_.lightingManager->scaleSelectedEditableLight(
          newScale / startScale);
      break;
    }
    }
    if (lightChanged) {
      subs_.lightingManager->updateLightingData();
      updateFrameDescriptorSets();
      transformDragSession_.active = false;
    }
    return;
  }

  if (transformingSectionPlane && subs_.guiManager) {
    auto nextSectionPlane = transformDragSession_.startSectionPlane;
    const glm::vec3 startNormal =
        normalizedOr(nextSectionPlane.normal, {0.0f, 1.0f, 0.0f});
    const glm::vec3 startOrigin = sectionPlaneOrigin(nextSectionPlane);
    switch (tool) {
    case container::ui::ViewportTool::Select:
      break;
    case container::ui::ViewportTool::Translate:
      nextSectionPlane.offset = glm::dot(startNormal, controls.position);
      break;
    case container::ui::ViewportTool::Rotate: {
      glm::vec3 rotatedNormal = startNormal;
      const glm::vec3 rotationDelta =
          controls.rotationDegrees -
          transformDragSession_.startControls.rotationDegrees;
      auto applyRotation = [&](const glm::vec3 &axisVector, float degrees) {
        if (std::abs(degrees) > 1.0e-4f) {
          rotatedNormal =
              rotateVectorAroundAxis(rotatedNormal, axisVector, degrees);
        }
      };
      switch (axis) {
      case container::ui::TransformAxis::X:
        applyRotation(transformDragSession_.axisX, rotationDelta.x);
        break;
      case container::ui::TransformAxis::Y:
        applyRotation(transformDragSession_.axisY, rotationDelta.y);
        break;
      case container::ui::TransformAxis::Z:
        applyRotation(transformDragSession_.axisZ, rotationDelta.z);
        break;
      case container::ui::TransformAxis::Free:
        applyRotation(transformDragSession_.axisY, rotationDelta.y);
        applyRotation(transformDragSession_.axisX, rotationDelta.x);
        break;
      }
      nextSectionPlane.normal =
          normalizedOr(rotatedNormal, {0.0f, 1.0f, 0.0f});
      nextSectionPlane.offset = glm::dot(nextSectionPlane.normal, startOrigin);
      break;
    }
    case container::ui::ViewportTool::Scale: {
      const float startSize =
          std::max(transformDragSession_.startSectionPlane.visualPlaneSize,
                   0.1f);
      const float startScale =
          std::max((transformDragSession_.startControls.scale.x +
                    transformDragSession_.startControls.scale.y +
                    transformDragSession_.startControls.scale.z) *
                       0.33333334f,
                   0.001f);
      const float nextScale =
          std::max((controls.scale.x + controls.scale.y + controls.scale.z) *
                       0.33333334f,
                   0.001f);
      nextSectionPlane.visualPlaneSize =
          std::clamp(startSize * (nextScale / startScale), 0.1f, 100000.0f);
      break;
    }
    }
    nextSectionPlane.enabled = true;
    nextSectionPlane.visualPlaneVisible = true;
    nextSectionPlane.visualPlaneEditable = true;
    subs_.guiManager->setSectionPlaneState(nextSectionPlane);
    return;
  }

  subs_.cameraController->applyNodeTransform(sceneState_.selectedMeshNode,
                                             sceneState_.rootNode, controls);
  if (sceneState_.selectedMeshNode == sceneState_.rootNode &&
      subs_.lightingManager) {
    subs_.lightingManager->updateLightingData();
  }
  refreshSceneObjectData();
}

std::optional<container::ui::TransformAxis>
RendererFrontend::pickTransformGizmoAxisAtCursor(double cursorX,
                                                 double cursorY) const {
  if (subs_.guiManager != nullptr &&
      !subs_.guiManager->editorOverlaysEnabled()) {
    return std::nullopt;
  }

  const bool hasSelectedEditableLight =
      subs_.lightingManager &&
      subs_.lightingManager->selectedEditableLight().has_value();
  const bool hasEditableSectionPlane =
      subs_.guiManager &&
      subs_.guiManager->sectionPlaneState().enabled &&
      subs_.guiManager->sectionPlaneState().visualPlaneEditable;
  if (sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode &&
      !hasSelectedEditableLight && !hasEditableSectionPlane) {
    return std::nullopt;
  }

  const FrameTransformGizmoState gizmo = buildTransformGizmoState();
  if (!gizmo.visible || gizmo.tool == container::ui::ViewportTool::Select) {
    return std::nullopt;
  }

  const VkExtent2D extent = svc_.swapChainManager.extent();
  const glm::vec2 cursor{static_cast<float>(cursorX),
                         static_cast<float>(cursorY)};
  const auto originScreen =
      projectToFramebuffer(buffers_.cameraData, extent, gizmo.origin);
  if (!originScreen) {
    return std::nullopt;
  }

  constexpr float kCenterHitRadiusPx = 12.0f;
  constexpr float kAxisHitRadiusPx = 9.0f;
  constexpr float kEndpointHitRadiusPx = 15.0f;
  constexpr float kScaleEndpointHitRadiusPx = 18.0f;
  constexpr float kRotateRingHitRadiusPx = 8.5f;

  const glm::vec2 centerDelta = cursor - *originScreen;
  if (glm::dot(centerDelta, centerDelta) <=
      kCenterHitRadiusPx * kCenterHitRadiusPx) {
    return container::ui::TransformAxis::Free;
  }

  std::optional<container::ui::TransformAxis> bestAxis;
  float bestDistance2 = std::numeric_limits<float>::max();
  auto consider = [&](container::ui::TransformAxis axis, float distance2,
                      float thresholdPx) {
    const float threshold2 = thresholdPx * thresholdPx;
    if (distance2 <= threshold2 && distance2 < bestDistance2) {
      bestDistance2 = distance2;
      bestAxis = axis;
    }
  };

  auto axisDirection = [&](container::ui::TransformAxis axis) {
    switch (axis) {
    case container::ui::TransformAxis::X:
      return normalizedOr(gizmo.axisX, {1.0f, 0.0f, 0.0f});
    case container::ui::TransformAxis::Y:
      return normalizedOr(gizmo.axisY, {0.0f, 1.0f, 0.0f});
    case container::ui::TransformAxis::Z:
      return normalizedOr(gizmo.axisZ, {0.0f, 0.0f, 1.0f});
    case container::ui::TransformAxis::Free:
      break;
    }
    return glm::vec3{1.0f, 0.0f, 0.0f};
  };

  auto axisSideBasis = [&](container::ui::TransformAxis axis, glm::vec3 &sideA,
                           glm::vec3 &sideB) {
    if (axis == container::ui::TransformAxis::X) {
      sideA = axisDirection(container::ui::TransformAxis::Y);
      sideB = axisDirection(container::ui::TransformAxis::Z);
    } else if (axis == container::ui::TransformAxis::Y) {
      sideA = axisDirection(container::ui::TransformAxis::X);
      sideB = axisDirection(container::ui::TransformAxis::Z);
    } else {
      sideA = axisDirection(container::ui::TransformAxis::X);
      sideB = axisDirection(container::ui::TransformAxis::Y);
    }
  };

  auto considerSegment = [&](container::ui::TransformAxis axis,
                             const glm::vec3 &start, const glm::vec3 &end,
                             float thresholdPx) {
    const auto startScreen =
        projectToFramebuffer(buffers_.cameraData, extent, start);
    const auto endScreen =
        projectToFramebuffer(buffers_.cameraData, extent, end);
    if (!startScreen || !endScreen) {
      return;
    }
    consider(axis, distanceSquaredToSegment(cursor, *startScreen, *endScreen),
             thresholdPx);
  };

  auto considerPoint = [&](container::ui::TransformAxis axis,
                           const glm::vec3 &point, float thresholdPx) {
    const auto pointScreen =
        projectToFramebuffer(buffers_.cameraData, extent, point);
    if (!pointScreen) {
      return;
    }
    const glm::vec2 delta = cursor - *pointScreen;
    consider(axis, glm::dot(delta, delta), thresholdPx);
  };

  static constexpr std::array kAxes{
      container::ui::TransformAxis::X,
      container::ui::TransformAxis::Y,
      container::ui::TransformAxis::Z,
  };

  if (gizmo.tool == container::ui::ViewportTool::Rotate) {
    constexpr uint32_t kRingSegments = 64u;
    constexpr float kPi = 3.14159265358979323846f;
    for (const container::ui::TransformAxis axis : kAxes) {
      glm::vec3 sideA{0.0f};
      glm::vec3 sideB{0.0f};
      axisSideBasis(axis, sideA, sideB);
      for (uint32_t segment = 0u; segment < kRingSegments; ++segment) {
        const float angle0 = static_cast<float>(segment) *
                             (2.0f * kPi / static_cast<float>(kRingSegments));
        const float angle1 = static_cast<float>(segment + 1u) *
                             (2.0f * kPi / static_cast<float>(kRingSegments));
        const glm::vec3 start =
            gizmo.origin +
            (std::cos(angle0) * sideA + std::sin(angle0) * sideB) * gizmo.scale;
        const glm::vec3 end =
            gizmo.origin +
            (std::cos(angle1) * sideA + std::sin(angle1) * sideB) * gizmo.scale;
        considerSegment(axis, start, end, kRotateRingHitRadiusPx);
      }
    }
    return bestAxis;
  }

  for (const container::ui::TransformAxis axis : kAxes) {
    const glm::vec3 direction = axisDirection(axis);
    if (gizmo.tool == container::ui::ViewportTool::Scale) {
      const glm::vec3 handleCenter =
          gizmo.origin + direction * gizmo.scale * 0.92f;
      considerSegment(axis, gizmo.origin, handleCenter, kAxisHitRadiusPx);
      considerPoint(axis, handleCenter, kScaleEndpointHitRadiusPx);
    } else {
      const glm::vec3 handleTip = gizmo.origin + direction * gizmo.scale;
      considerSegment(axis, gizmo.origin, handleTip, kAxisHitRadiusPx);
      considerPoint(axis, handleTip, kEndpointHitRadiusPx);
    }
  }

  return bestAxis;
}

void RendererFrontend::requestScreenshot(std::filesystem::path outputPath) {
  if (outputPath.empty()) {
    throw std::runtime_error("screenshot output path is empty");
  }
  if (!svc_.swapChainManager.supportsTransferSrc()) {
    throw std::runtime_error(
        "swapchain does not support VK_IMAGE_USAGE_TRANSFER_SRC_BIT");
  }
  if (!isSupportedScreenshotFormat(svc_.swapChainManager.imageFormat())) {
    throw std::runtime_error("unsupported swapchain screenshot format");
  }

  screenshot_.outputPath = std::move(outputPath);
  screenshot_.pending = true;
}

bool RendererFrontend::reloadSceneModel(const std::string &path,
                                        float importScale) {
  if (!subs_.sceneController || !subs_.sceneManager)
    return false;

  ensureObjectBuffers();
  if (buffers_.objects.empty() || buffers_.objectCapacities.empty())
    return false;

  const auto cameraBuffer = buffers_.cameras.empty()
                                ? container::gpu::AllocatedBuffer{}
                                : buffers_.cameras.front();
  auto reloadPrimary = [&](const std::string &modelPath, float scale) {
    return subs_.sceneController->reloadSceneModel(
        modelPath, scale, buffers_.objects.front(),
        buffers_.objectCapacities.front(),
        cameraBuffer, sceneState_.indexType, sceneState_.rootNode,
        sceneState_.selectedMeshNode, sceneState_.cubeNode);
  };
  auto refreshSceneState = [&](bool resetCamera) {
    // reloadPrimary can replace the first object buffer before
    // updateObjectBuffer sees it. Rebind shadow-cull sets even when the new
    // scene fits the already-created buffer capacity.
    std::fill(buffers_.shadowObjectDescriptorReady.begin(),
              buffers_.shadowObjectDescriptorReady.end(), false);
    syncSceneStateFromController();
    applySceneLightingDefaults();
    if (subs_.lightingManager) {
      subs_.lightingManager->setRootNode(sceneState_.rootNode);
      subs_.lightingManager->updateLightingData();
      subs_.lightingManager->createLightVolumeGeometry();
    }
    if (resetCamera) {
      resetCameraForActiveScene();
      for (uint32_t imageIndex = 0;
           imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
           ++imageIndex) {
        updateCameraBuffer(imageIndex);
      }
    }
    updateAllObjectBuffers();
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.objects);
    if (subs_.bimManager && subs_.bimManager->hasScene()) {
      subs_.sceneManager->updateAuxiliaryDescriptorSets(
          buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
    }
    syncSceneProviders();
  };

  if (isAuxiliaryRenderModelPath(path)) {
    const std::string previousPrimaryPath = activePrimaryModelPath_;
    const float previousPrimaryScale = activePrimaryImportScale_;
    const std::string previousAuxiliaryPath = activeAuxiliaryModelPath_;
    const float previousAuxiliaryScale = activeAuxiliaryImportScale_;

    (void)reloadPrimary("", 1.0f);
    if (!subs_.bimManager) {
      subs_.bimManager = std::make_unique<BimManager>(
          svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
      subs_.bimManager->createMeshletResidencyResources(
          container::util::executableDirectory());
    }

    try {
      subs_.bimManager->loadModel(path, importScale, *subs_.sceneManager);
    } catch (const std::exception &) {
      subs_.bimManager->clear();
      (void)reloadPrimary(previousPrimaryPath, previousPrimaryScale);
      if (!previousAuxiliaryPath.empty()) {
        try {
          subs_.bimManager->loadModel(previousAuxiliaryPath,
                                      previousAuxiliaryScale,
                                      *subs_.sceneManager);
        } catch (const std::exception &) {
          subs_.bimManager->clear();
          activeAuxiliaryModelPath_.clear();
        }
      }
      refreshSceneState(true);
      if (subs_.guiManager) {
        subs_.guiManager->setStatusMessage("Failed to load model: " + path);
      }
      return false;
    }

    activePrimaryModelPath_.clear();
    activePrimaryImportScale_ = 1.0f;
    activeAuxiliaryModelPath_ = path;
    activeAuxiliaryImportScale_ = importScale;
    refreshSceneState(true);
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage("Loaded model: " + path);
    }
    return subs_.bimManager->hasScene();
  }

  const bool result = reloadPrimary(path, importScale);
  if (result) {
    activePrimaryModelPath_ = path;
    activePrimaryImportScale_ = importScale;
    activeAuxiliaryModelPath_.clear();
    activeAuxiliaryImportScale_ = 1.0f;

    if (subs_.bimManager) {
      subs_.bimManager->clear();
    }
    if (!svc_.config.bimModelPath.empty()) {
      if (!subs_.bimManager) {
        subs_.bimManager = std::make_unique<BimManager>(svc_.ctx.deviceWrapper,
                                                        svc_.allocationManager,
                                                        svc_.pipelineManager);
        subs_.bimManager->createMeshletResidencyResources(
            container::util::executableDirectory());
      }
      subs_.bimManager->loadModel(svc_.config.bimModelPath,
                                  svc_.config.bimImportScale,
                                  *subs_.sceneManager);
      activeAuxiliaryModelPath_ = svc_.config.bimModelPath;
      activeAuxiliaryImportScale_ = svc_.config.bimImportScale;
    }
  }

  refreshSceneState(result);
  return result;
}

void RendererFrontend::processPendingGuiModelLoadRequest() {
  if (!subs_.guiManager) {
    return;
  }

  std::optional<container::ui::ModelLoadRequest> request =
      subs_.guiManager->consumeModelLoadRequest();
  if (!request) {
    return;
  }

  const std::string statusLabel =
      request->label.empty() ? request->path : request->label;
  try {
    const bool success = reloadSceneModel(request->path, request->importScale);
    if (!success && subs_.guiManager) {
      subs_.guiManager->setStatusMessage("Failed to load model: " +
                                         statusLabel);
    }
  } catch (const std::exception &e) {
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage("Failed to load model: " +
                                         statusLabel + " (" + e.what() + ")");
    }
  } catch (...) {
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage("Failed to load model: " +
                                         statusLabel + " (unknown error)");
    }
  }
}

void RendererFrontend::shutdown() {
  if (!svc_.ctx.deviceWrapper)
    return;

  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());

  subs_.frameSyncManager.reset();
  subs_.deferredRasterFrameGraphContext.reset();
  subs_.frameRecorder.reset();
  subs_.renderPassGpuProfiler.reset();
  subs_.activeTechnique = nullptr;
  subs_.techniqueRegistry.reset();
  subs_.pipelineRegistry.reset();
  subs_.frameRuntimeResourceRegistry.reset();
  subs_.frameResourceRegistry.reset();

  subs_.temporalManager.reset();
  destroyGBufferResources();
  subs_.frameResourceManager.reset();

  if (subs_.guiManager) {
    subs_.guiManager->shutdown(svc_.ctx.deviceWrapper->device());
    subs_.guiManager.reset();
  }
  subs_.rendererTelemetry.reset();

  subs_.renderPassManager.reset();
  resources_.renderPasses = {};

  subs_.bimManager.reset();
  subs_.sceneController.reset();
  subs_.sceneManager.reset();
  subs_.lightingManager.reset();
  subs_.shadowCullManager.reset();
  subs_.shadowManager.reset();
  subs_.environmentManager.reset();
  subs_.bloomManager.reset();
  subs_.exposureManager.reset();
  subs_.oitManager.reset();
  subs_.cameraController.reset();
  subs_.pipelineBuilder.reset();

  for (auto &cameraBuffer : buffers_.cameras) {
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(cameraBuffer);
    }
  }
  buffers_.cameras.clear();
  for (auto &objectBuffer : buffers_.objects) {
    if (objectBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(objectBuffer);
    }
  }
  buffers_.objects.clear();
  buffers_.objectCapacities.clear();
  buffers_.shadowObjectDescriptorReady.clear();
  destroyReadbackSlots(screenshot_.readbacks);
  destroyDepthVisibilityFrameSlots();
}

// ---------------------------------------------------------------------------
// Init helpers
// ---------------------------------------------------------------------------

void RendererFrontend::createRenderPasses() {
  resources_.gBufferFormats.depthStencil =
      subs_.renderPassManager->findDepthStencilFormat();
  subs_.renderPassManager->create(
      svc_.swapChainManager.imageFormat(),
      resources_.gBufferFormats.depthStencil,
      resources_.gBufferFormats.sceneColor, resources_.gBufferFormats.albedo,
      resources_.gBufferFormats.normal, resources_.gBufferFormats.material,
      resources_.gBufferFormats.emissive, resources_.gBufferFormats.specular,
      resources_.gBufferFormats.pickId, msaaSampleCount_);
  resources_.renderPasses = subs_.renderPassManager->passes();
}

void RendererFrontend::createGraphicsPipelines() {
  if (!subs_.pipelineBuilder) {
    subs_.pipelineBuilder = std::make_unique<GraphicsPipelineBuilder>(
        svc_.ctx.deviceWrapper, svc_.pipelineManager);
  }
  const PipelineDescriptorLayouts descLayouts{
      subs_.sceneManager->descriptorSetLayout(),
      subs_.frameResourceManager->lightingLayout(),
      subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.lightingManager->lightGizmoIconDescriptorSetLayout(),
      // Some tests or fallback configurations may not create tiled lighting
      // resources. Reuse the regular light layout so pipeline layout creation
      // stays well-formed even when the tiled path is unavailable.
      subs_.lightingManager->isTiledLightingReady()
          ? subs_.lightingManager->tiledDescriptorSetLayout()
          : subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.shadowManager->descriptorSetLayout(),
      subs_.frameResourceManager->postProcessLayout(),
      subs_.frameResourceManager->oitLayout()};
  const PipelineRenderPasses rp{resources_.renderPasses.depthPrepass,
                                resources_.renderPasses.bimDepthPrepass,
                                resources_.renderPasses.gBuffer,
                                resources_.renderPasses.bimGBuffer,
                                resources_.renderPasses.transparentPick,
                                resources_.renderPasses.shadow,
                                resources_.renderPasses.lighting,
                                resources_.renderPasses.transformGizmos,
                                resources_.renderPasses.postProcess,
                                resources_.renderPasses.forwardLighting};
  resources_.builtPipelines = subs_.pipelineBuilder->build(
      container::util::executableDirectory(), descLayouts, rp,
      msaaSampleCount_);
  if (!resources_.builtPipelines.pipelines.layoutRegistry) {
    resources_.builtPipelines.pipelines.layoutRegistry =
        resources_.builtPipelines.layouts.layoutRegistry
            ? resources_.builtPipelines.layouts.layoutRegistry
            : buildGraphicsPipelineLayoutRegistry(
                  resources_.builtPipelines.layouts);
  }
  resources_.builtPipelines.layouts.layoutRegistry =
      resources_.builtPipelines.pipelines.layoutRegistry;
}

void RendererFrontend::destroyGraphicsPipelines() {
  auto &pipelines = resources_.builtPipelines.pipelines;
  auto destroyPipeline = [this](VkPipeline &pipeline) {
    svc_.pipelineManager.destroyPipeline(pipeline);
  };
  destroyPipeline(pipelines.depthPrepass);
  destroyPipeline(pipelines.depthPrepassFrontCull);
  destroyPipeline(pipelines.depthPrepassNoCull);
  destroyPipeline(pipelines.bimDepthPrepass);
  destroyPipeline(pipelines.bimDepthPrepassFrontCull);
  destroyPipeline(pipelines.bimDepthPrepassNoCull);
  destroyPipeline(pipelines.gBuffer);
  destroyPipeline(pipelines.gBufferFrontCull);
  destroyPipeline(pipelines.gBufferNoCull);
  destroyPipeline(pipelines.bimGBuffer);
  destroyPipeline(pipelines.bimGBufferFrontCull);
  destroyPipeline(pipelines.bimGBufferNoCull);
  destroyPipeline(pipelines.shadowDepth);
  destroyPipeline(pipelines.shadowDepthFrontCull);
  destroyPipeline(pipelines.shadowDepthNoCull);
  destroyPipeline(pipelines.localShadowDepth);
  destroyPipeline(pipelines.localShadowDepthFrontCull);
  destroyPipeline(pipelines.localShadowDepthNoCull);
  destroyPipeline(pipelines.directionalLight);
  destroyPipeline(pipelines.stencilVolume);
  destroyPipeline(pipelines.pointLight);
  destroyPipeline(pipelines.pointLightStencilDebug);
  destroyPipeline(pipelines.tiledPointLight);
  destroyPipeline(pipelines.transparent);
  destroyPipeline(pipelines.transparentFrontCull);
  destroyPipeline(pipelines.transparentNoCull);
  destroyPipeline(pipelines.transparentPick);
  destroyPipeline(pipelines.transparentPickFrontCull);
  destroyPipeline(pipelines.transparentPickNoCull);
  destroyPipeline(pipelines.postProcess);
  destroyPipeline(pipelines.geometryDebug);
  destroyPipeline(pipelines.normalValidation);
  destroyPipeline(pipelines.normalValidationFrontCull);
  destroyPipeline(pipelines.normalValidationNoCull);
  destroyPipeline(pipelines.wireframeDepth);
  destroyPipeline(pipelines.wireframeDepthFrontCull);
  destroyPipeline(pipelines.wireframeNoDepth);
  destroyPipeline(pipelines.wireframeNoDepthFrontCull);
  destroyPipeline(pipelines.selectionMask);
  destroyPipeline(pipelines.selectionOutline);
  destroyPipeline(pipelines.bimFloorPlanDepth);
  destroyPipeline(pipelines.bimFloorPlanNoDepth);
  destroyPipeline(pipelines.bimPointCloudDepth);
  destroyPipeline(pipelines.bimPointCloudNoDepth);
  destroyPipeline(pipelines.bimCurveDepth);
  destroyPipeline(pipelines.bimCurveNoDepth);
  destroyPipeline(pipelines.bimSectionClipCapFill);
  destroyPipeline(pipelines.bimSectionClipCapHatch);
  destroyPipeline(pipelines.surfaceNormalLine);
  destroyPipeline(pipelines.objectNormalDebug);
  destroyPipeline(pipelines.objectNormalDebugFrontCull);
  destroyPipeline(pipelines.objectNormalDebugNoCull);
  destroyPipeline(pipelines.lightGizmo);
  destroyPipeline(pipelines.lightGizmoCoverage);
  destroyPipeline(pipelines.lightGizmoPick);
  destroyPipeline(pipelines.transformGizmo);
  destroyPipeline(pipelines.transformGizmoSolid);
  destroyPipeline(pipelines.transformGizmoOverlay);
  destroyPipeline(pipelines.transformGizmoSolidOverlay);
  for (RegisteredPipelineHandle &handle : pipelines.extraHandles) {
    destroyPipeline(handle.pipeline);
  }
  pipelines.extraHandles.clear();

  auto &layouts = resources_.builtPipelines.layouts;
  auto destroyLayout = [this](VkPipelineLayout &layout) {
    svc_.pipelineManager.destroyPipelineLayout(layout);
  };
  destroyLayout(layouts.scene);
  destroyLayout(layouts.transparent);
  destroyLayout(layouts.lighting);
  destroyLayout(layouts.lightGizmo);
  destroyLayout(layouts.tiledLighting);
  destroyLayout(layouts.shadow);
  destroyLayout(layouts.postProcess);
  destroyLayout(layouts.wireframe);
  destroyLayout(layouts.normalValidation);
  destroyLayout(layouts.surfaceNormal);
  destroyLayout(layouts.transformGizmo);
  for (RegisteredPipelineLayout &layout : layouts.extraLayouts) {
    destroyLayout(layout.layout);
  }
  layouts.extraLayouts.clear();

  resources_.builtPipelines = {};
}

void RendererFrontend::recreateMsaaResources(
    VkSampleCountFlagBits sampleCount) {
  if (subs_.temporalManager && subs_.temporalManager->settings().enabled &&
      sampleCount != VK_SAMPLE_COUNT_1_BIT) {
    if (subs_.guiManager)
      subs_.guiManager->setStatusMessage("Disable TAA before enabling MSAA");
    return;
  }
  if (sampleCount == msaaSampleCount_) {
    return;
  }

  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());
  destroyGBufferResources();
  svc_.swapChainManager.destroyFramebuffers();
  destroyGraphicsPipelines();
  if (subs_.renderPassManager) {
    subs_.renderPassManager->destroy();
  }

  const bool guiWasInitialized = subs_.guiManager != nullptr;
  if (guiWasInitialized) {
    subs_.guiManager->shutdown(svc_.ctx.deviceWrapper->device());
  }

  msaaSampleCount_ = sampleCount;
  createRenderPasses();
  svc_.swapChainManager.createFramebuffers(resources_.renderPasses.postProcess);
  if (guiWasInitialized) {
    subs_.guiManager->initialize(
        svc_.ctx.instance, svc_.ctx.deviceWrapper->device(),
        svc_.ctx.deviceWrapper->physicalDevice(),
        svc_.ctx.deviceWrapper->graphicsQueue(),
        svc_.ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
        resources_.renderPasses.postProcess,
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()),
        svc_.nativeWindow, svc_.config.modelPath, svc_.config.importScale);
    subs_.guiManager->setWireframeCapabilities(
        svc_.ctx.wireframeSupported, svc_.ctx.wireframeRasterModeSupported,
        svc_.ctx.wireframeWideLinesSupported);
    if (subs_.environmentManager) {
      subs_.guiManager->setEnvironmentStatus(
          subs_.environmentManager->environmentStatus());
    }
  }
  createGraphicsPipelines();
  createFrameResources();
  updateFrameDescriptorSets();
  invalidateDepthVisibilityFrames();
  if (subs_.guiManager) {
    subs_.guiManager->setStatusMessage(
        "MSAA set to " + std::to_string(sampleCountToSamples(sampleCount)) +
        "x");
  }
}

void RendererFrontend::createCamera() {
  subs_.cameraController = std::make_unique<CameraController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.swapChainManager,
      sceneGraph_, subs_.sceneManager.get(), subs_.sceneController.get(),
      subs_.sceneController->world(), svc_.inputManager);
  subs_.cameraController->createCamera();
  resetCameraForActiveScene();
  applyCameraOverride(subs_.cameraController->camera(), svc_.config);
}

void RendererFrontend::applySceneLightingDefaults() {
  if (!subs_.lightingManager || !subs_.bloomManager)
    return;

  auto settings = subs_.lightingManager->lightingSettings();
  const container::gpu::LightingSettings viewerSettings{};
  container::app::SceneLightingValues defaults{
      viewerSettings.directionalIntensity, viewerSettings.environmentIntensity,
      viewerSettings.bounceIntensity, true, viewerSettings.localShadowLayerBudget};
  const bool isolatedAuthoredLocalScene =
      container::app::IsDefaultAuthoredLocalLightScene(activePrimaryModelPath_) &&
      (!subs_.bimManager || !subs_.bimManager->hasScene()) &&
      subs_.sceneManager &&
      (!subs_.sceneManager->authoredPointLights().empty() ||
       !subs_.sceneManager->authoredAreaLights().empty());
  if (isolatedAuthoredLocalScene) {
    defaults = {container::app::kDefaultAuthoredLocalLightDirectionalIntensity,
                container::app::kDefaultAuthoredLocalLightEnvironmentIntensity,
                container::app::kDefaultAuthoredLocalLightBounceIntensity,
                container::app::kDefaultAuthoredLocalLightBloomEnabled,
                container::app::kDefaultAuthoredLocalLightShadowLayerBudget};
  }

  container::app::SceneLightingValues current{
      settings.directionalIntensity, settings.environmentIntensity,
      settings.bounceIntensity, subs_.bloomManager->enabled(),
      settings.localShadowLayerBudget};
  if (svc_.config.hasDirectionalIntensityOverride)
    current.directionalIntensity = svc_.config.directionalIntensity;
  if (svc_.config.hasEnvironmentIntensityOverride)
    current.environmentIntensity = svc_.config.environmentIntensity;
  if (svc_.config.hasBloomEnabledOverride)
    current.bloomEnabled = svc_.config.bloomEnabled;
  current = sceneLightingDefaults_.apply(
      current, defaults,
      {svc_.config.hasDirectionalIntensityOverride,
       svc_.config.hasEnvironmentIntensityOverride,
       svc_.config.hasBloomEnabledOverride});
  settings.directionalIntensity = current.directionalIntensity;
  settings.environmentIntensity = current.environmentIntensity;
  settings.bounceIntensity = current.bounceIntensity;
  settings.localShadowLayerBudget = current.localShadowLayerBudget;
  subs_.lightingManager->setLightingSettings(settings);
  subs_.bloomManager->enabled() = current.bloomEnabled;
}

void RendererFrontend::resetCameraForActiveScene() {
  if (subs_.temporalManager)
    subs_.temporalManager->reset("scene/camera reset");
  if (!subs_.cameraController) {
    return;
  }

  subs_.cameraController->resetCameraForBounds(
      cameraSceneBoundsFromActiveContent(subs_.sceneManager.get(),
                                         subs_.bimManager.get()));
}

void RendererFrontend::syncCameraSelectionPivotOverride() {
  if (!subs_.cameraController) {
    return;
  }

  constexpr uint32_t invalidObjectIndex = std::numeric_limits<uint32_t>::max();
  const bool anchorMatchesScene =
      selectionNavigationAnchor_.valid &&
      sceneState_.selectedMeshNode !=
          container::scene::SceneGraph::kInvalidNode &&
      selectionNavigationAnchor_.sceneNode == sceneState_.selectedMeshNode;
  const bool anchorMatchesBim =
      selectionNavigationAnchor_.valid &&
      selectedBimObjectIndex_ != invalidObjectIndex &&
      selectionNavigationAnchor_.bimObject == selectedBimObjectIndex_;
  if (anchorMatchesScene) {
    subs_.cameraController->setSelectionPivotOverride(
        CameraController::NavigationPivot{
            .center = selectionNavigationAnchor_.point,
            .radius = selectionNavigationAnchor_.radius,
            .valid = true});
    return;
  }

  if (selectedBimObjectIndex_ == invalidObjectIndex || !subs_.bimManager ||
      !subs_.bimManager->hasScene()) {
    subs_.cameraController->clearSelectionPivotOverride();
    return;
  }
  if (selectionNavigationAnchor_.valid && !anchorMatchesBim) {
    subs_.cameraController->clearSelectionPivotOverride();
    return;
  }

  const BimElementBounds bounds =
      subs_.bimManager->elementBoundsForObject(selectedBimObjectIndex_);
  if (!bounds.valid) {
    subs_.cameraController->clearSelectionPivotOverride();
    return;
  }

  subs_.cameraController->setSelectionPivotOverride(
      CameraController::NavigationPivot{
          .center = bounds.center, .radius = bounds.radius, .valid = true});
}

void RendererFrontend::initializeScene() {
  buildSceneGraph();
  createSceneBuffers();
  subs_.lightingManager->updateLightingData();
  applyConfiguredLightingOverrides(subs_.lightingManager.get(), svc_.config);
  subs_.lightingManager->createLightVolumeGeometry();
  createFrameResources();
  if (subs_.environmentManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.environmentManager->createGtaoResources(
        container::util::executableDirectory(), ext.width, ext.height);
  }
  if (subs_.bloomManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.bloomManager->createTextures(ext.width, ext.height);
  }
  createGeometryBuffers();
  subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.objects);
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    subs_.sceneManager->updateAuxiliaryDescriptorSets(
        buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
  }
  updateFrameDescriptorSets();
  syncSceneProviders();

  container::log::ContainerLogger::instance().renderer()->info(
      "Initializing Vulkan renderer");
  container::log::ContainerLogger::instance().vulkan()->debug(
      "Debugging Vulkan initialization");
}

void RendererFrontend::buildSceneGraph() {
  if (!subs_.sceneController)
    return;
  subs_.sceneController->buildSceneGraph(
      sceneState_.rootNode, sceneState_.selectedMeshNode, sceneState_.cubeNode);
  selectionNavigationAnchor_ = {};
  if (subs_.lightingManager)
    subs_.lightingManager->setRootNode(sceneState_.rootNode);
}

void RendererFrontend::ensureCameraBuffers() {
  const size_t imageCount = svc_.swapChainManager.imageCount();
  if (buffers_.cameras.size() == imageCount)
    return;

  for (auto &cameraBuffer : buffers_.cameras) {
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(cameraBuffer);
    }
  }

  buffers_.cameras.assign(imageCount, {});
  for (auto &cameraBuffer : buffers_.cameras) {
    cameraBuffer = svc_.allocationManager.createBuffer(
        sizeof(container::gpu::CameraData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
}

void RendererFrontend::ensureObjectBuffers() {
  const size_t imageCount = svc_.swapChainManager.imageCount();
  if (buffers_.objects.size() == imageCount &&
      buffers_.objectCapacities.size() == imageCount &&
      buffers_.shadowObjectDescriptorReady.size() == imageCount) {
    return;
  }

  for (auto &objectBuffer : buffers_.objects) {
    if (objectBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(objectBuffer);
    }
  }

  buffers_.objects.assign(imageCount, {});
  buffers_.objectCapacities.assign(imageCount, 0);
  buffers_.shadowObjectDescriptorReady.assign(imageCount, false);
}

void RendererFrontend::createSceneBuffers() {
  ensureCameraBuffers();
  ensureObjectBuffers();
  if (subs_.sceneController) {
    for (uint32_t imageIndex = 0;
         imageIndex < static_cast<uint32_t>(buffers_.objects.size());
         ++imageIndex) {
      const auto cameraBuffer =
          imageIndex < buffers_.cameras.size()
              ? buffers_.cameras[imageIndex]
              : container::gpu::AllocatedBuffer{};
      subs_.sceneController->createSceneBuffers(
          cameraBuffer, buffers_.objects[imageIndex],
          buffers_.objectCapacities[imageIndex]);
    }
  }
  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }
  updateAllObjectBuffers();
  updateFrameDescriptorSets();
}

void RendererFrontend::createGeometryBuffers() {
  if (!subs_.sceneController)
    return;
  subs_.sceneController->createGeometryBuffers();
  syncSceneStateFromController();
}

void RendererFrontend::createFrameResources() {
  if (subs_.temporalManager)
    subs_.temporalManager->destroyImages();
  ensureObjectBuffers();
  if (subs_.lightingManager) {
    subs_.lightingManager->resizeTiledResources(svc_.swapChainManager.extent());
  }
  const auto objectBuffer = sceneObjectBuffer(0);
  subs_.frameResourceManager->create(
      resources_.gBufferFormats, resources_.renderPasses.depthPrepass,
      resources_.renderPasses.bimDepthPrepass, resources_.renderPasses.gBuffer,
      resources_.renderPasses.bimGBuffer,
      resources_.renderPasses.transparentPick, resources_.renderPasses.lighting,
      resources_.renderPasses.forwardLighting,
      resources_.renderPasses.forwardTransparent,
      resources_.renderPasses.transformGizmos, msaaSampleCount_, buffers_.cameras,
      objectBuffer);
}

// ---------------------------------------------------------------------------
// Per-frame helpers
// ---------------------------------------------------------------------------

void RendererFrontend::updateCameraBuffer(uint32_t imageIndex) {
  if (!subs_.cameraController || imageIndex >= buffers_.cameras.size())
    return;
  subs_.cameraController->updateCameraBuffer(buffers_.cameraData,
                                             buffers_.cameras[imageIndex]);
}

void RendererFrontend::refreshSceneObjectData() {
  if (!subs_.sceneController)
    return;

  const bool showDiagCube =
      subs_.guiManager && subs_.guiManager->showNormalDiagCube();
  subs_.sceneController->syncObjectDataFromSceneGraph(showDiagCube);
  sceneState_.diagCubeObjectIndex =
      subs_.sceneController->diagCubeObjectIndex();
}

void RendererFrontend::updateObjectBuffer(uint32_t imageIndex) {
  if (!subs_.sceneController)
    return;
  ensureObjectBuffers();
  if (imageIndex >= buffers_.objects.size() ||
      imageIndex >= buffers_.objectCapacities.size()) {
    return;
  }

  const auto cameraBuffer =
      imageIndex < buffers_.cameras.size()
          ? buffers_.cameras[imageIndex]
          : container::gpu::AllocatedBuffer{};
  const bool recreated = subs_.sceneController->updateObjectBuffer(
      buffers_.objects[imageIndex], buffers_.objectCapacities[imageIndex],
      cameraBuffer);
  if (subs_.shadowCullManager &&
      imageIndex < buffers_.shadowObjectDescriptorReady.size() &&
      (recreated || !buffers_.shadowObjectDescriptorReady[imageIndex])) {
    subs_.shadowCullManager->updateObjectSsboDescriptor(
        imageIndex, buffers_.objects[imageIndex].buffer,
        sizeof(container::gpu::ObjectData) *
            buffers_.objectCapacities[imageIndex]);
    buffers_.shadowObjectDescriptorReady[imageIndex] = true;
  }
  if (recreated && subs_.sceneManager) {
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.objects);
  }
  sceneState_.diagCubeObjectIndex =
      subs_.sceneController->diagCubeObjectIndex();
}

void RendererFrontend::updateAllObjectBuffers() {
  ensureObjectBuffers();
  const uint32_t imageCount =
      static_cast<uint32_t>(std::min(buffers_.objects.size(),
                                     buffers_.objectCapacities.size()));
  for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
    updateObjectBuffer(imageIndex);
  }
}

container::gpu::AllocatedBuffer RendererFrontend::sceneObjectBuffer(
    uint32_t imageIndex) const {
  if (imageIndex < buffers_.objects.size()) {
    return buffers_.objects[imageIndex];
  }
  if (!buffers_.objects.empty()) {
    return buffers_.objects.front();
  }
  return {};
}

size_t RendererFrontend::sceneObjectCapacity(uint32_t imageIndex) const {
  if (imageIndex < buffers_.objectCapacities.size()) {
    return buffers_.objectCapacities[imageIndex];
  }
  if (!buffers_.objectCapacities.empty()) {
    return buffers_.objectCapacities.front();
  }
  return 0;
}

size_t RendererFrontend::maxSceneObjectCapacity() const {
  return buffers_.objectCapacities.empty()
             ? 0
             : *std::max_element(buffers_.objectCapacities.begin(),
                                 buffers_.objectCapacities.end());
}

void RendererFrontend::applyBimSemanticColorMode() {
  if (!subs_.bimManager || !subs_.bimManager->hasScene() || !subs_.guiManager) {
    return;
  }
  subs_.bimManager->setSemanticColorMode(
      subs_.guiManager->bimSemanticColorMode());
}

void RendererFrontend::updateFrameDescriptorSets(
    uint32_t imageIndex, const FrameRecordParams *preparedParams) {
  if (subs_.frameResourceManager) {
    const auto displayMode = frontendDisplayMode(
        subs_.guiManager.get(), configuredDisplayMode(svc_.config));
    const FrameFeatureReadiness featureReadiness =
        evaluateFrameFeatureReadiness(
            subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                             : RenderTechniqueId::DeferredRaster,
            displayMode, subs_.frameRecorder.get(), subs_.shadowManager.get(),
            subs_.environmentManager.get(), subs_.lightingManager.get(),
            imageIndex, preparedParams);

    if (subs_.lightingManager) {
      applyConfiguredLightingOverrides(subs_.lightingManager.get(),
                                        svc_.config);
      auto &lightingData = subs_.lightingManager->lightingData();
      const container::gpu::ShadowSettings shadowSettings =
          subs_.guiManager ? subs_.guiManager->shadowSettings()
                           : container::gpu::ShadowSettings{};
      lightingData.shadowEnabled = featureReadiness.shadowAtlas ? 1u : 0u;
      lightingData.localShadowEnabled =
          featureReadiness.localShadowAtlas ? 1u : 0u;
      lightingData.gtaoEnabled = featureReadiness.gtao ? 1u : 0u;
      lightingData.tileCullEnabled = featureReadiness.tileCull ? 1u : 0u;
      lightingData.localContactVisibility =
          shadowSettings.localContactVisibility ? 1u : 0u;
      lightingData.prefilteredMipCount =
          subs_.environmentManager
              ? subs_.environmentManager->prefilteredMipCount()
              : 1u;
      if (imageIndex == UINT32_MAX) {
        subs_.lightingManager->uploadLightingData();
      } else {
        subs_.lightingManager->uploadLightingData(imageIndex);
      }
    }

    VkImageView shadowView = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    std::span<const container::gpu::AllocatedBuffer> shadowUbos{};
    VkImageView localShadowView = VK_NULL_HANDLE;
    VkSampler localShadowSampler = VK_NULL_HANDLE;
    std::span<const container::gpu::AllocatedBuffer> localShadowUbos{};
    if (subs_.shadowManager) {
      shadowView = subs_.shadowManager->shadowAtlasArrayView();
      shadowSampler = subs_.shadowManager->shadowSampler();
      shadowUbos = subs_.shadowManager->shadowUbos();
      if (featureReadiness.localShadowAtlas) {
        localShadowView = subs_.shadowManager->localShadowAtlasArrayView();
        localShadowSampler = subs_.shadowManager->shadowSampler();
        localShadowUbos = subs_.shadowManager->localShadowUbos();
      }
    }
    VkImageView irradianceView = VK_NULL_HANDLE;
    VkImageView prefilteredView = VK_NULL_HANDLE;
    VkImageView brdfLutView = VK_NULL_HANDLE;
    VkSampler envSampler = VK_NULL_HANDLE;
    VkSampler brdfLutSampler = VK_NULL_HANDLE;
    VkImageView aoTextureView = VK_NULL_HANDLE;
    VkSampler aoSampler = VK_NULL_HANDLE;
    if (subs_.environmentManager && subs_.environmentManager->isReady()) {
      irradianceView = subs_.environmentManager->irradianceView();
      prefilteredView = subs_.environmentManager->prefilteredView();
      brdfLutView = subs_.environmentManager->brdfLutView();
      envSampler = subs_.environmentManager->envSampler();
      brdfLutSampler = subs_.environmentManager->brdfLutSampler();
    }
    if (featureReadiness.gtao && subs_.environmentManager) {
      aoTextureView = subs_.environmentManager->aoTextureView();
      aoSampler = subs_.environmentManager->aoSampler();
    }
    VkImageView bloomTextureView = VK_NULL_HANDLE;
    VkSampler bloomSampler = VK_NULL_HANDLE;
    if (subs_.bloomManager && subs_.bloomManager->isReady()) {
      bloomTextureView = subs_.bloomManager->bloomResultView();
      bloomSampler = subs_.bloomManager->bloomSampler();
    }
    VkBuffer tileGridBuffer = VK_NULL_HANDLE;
    VkDeviceSize tileGridBufferSize = 0;
    if (featureReadiness.tileCull && subs_.lightingManager) {
      tileGridBuffer = subs_.lightingManager->tileGridBuffer();
      tileGridBufferSize = subs_.lightingManager->tileGridBufferSize();
    }
    std::span<const container::gpu::AllocatedBuffer> exposureStateBuffers{};
    VkDeviceSize exposureStateBufferSize = 0;
    if (subs_.exposureManager && subs_.exposureManager->isReady()) {
      exposureStateBuffers =
          subs_.exposureManager->exposureStateBuffers();
      exposureStateBufferSize =
          subs_.exposureManager->exposureStateBufferSize();
    }
    const auto objectBuffer = sceneObjectBuffer(0);
    subs_.frameResourceManager->updateDescriptorSets(
        buffers_.cameras, objectBuffer, shadowView, shadowSampler,
        shadowUbos, localShadowView, localShadowSampler, localShadowUbos,
        irradianceView, prefilteredView, brdfLutView, envSampler,
        brdfLutSampler, aoTextureView, aoSampler, bloomTextureView,
        bloomSampler, tileGridBuffer, tileGridBufferSize, exposureStateBuffers,
        exposureStateBufferSize);
  }
  if (subs_.temporalManager && preparedParams &&
      imageIndex < buffers_.cameras.size()) {
    if (const auto *frame = subs_.frameResourceManager->frame(imageIndex))
      subs_.temporalManager->updateDescriptors(
          imageIndex, *frame, buffers_.cameras[imageIndex],
          preparedParams->debug.displayMode == 0 ||
              preparedParams->debug.displayMode == 8 ||
              preparedParams->debug.displayMode >= 100);
  }
}

void RendererFrontend::destroyGBufferResources() {
  if (subs_.temporalManager)
    subs_.temporalManager->destroyImages();
  if (subs_.frameResourceManager)
    subs_.frameResourceManager->destroy();
}

bool RendererFrontend::growExactOitNodePoolIfNeeded(uint32_t imageIndex) {
  if (!subs_.frameResourceManager)
    return false;
  const bool grew = subs_.frameResourceManager->growOitPoolIfNeeded(imageIndex);
  if (grew) {
    vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());
    createFrameResources();
    updateFrameDescriptorSets();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(
          "Expanded exact OIT node pool to " +
          std::to_string(subs_.frameResourceManager->oitNodeCapacityFloor()));
    }
  }
  return grew;
}

RendererFrontend::HostReadbackSlot& RendererFrontend::ensureReadbackSlot(
    std::vector<HostReadbackSlot>& readbacks, uint32_t frameSlot) {
  if (readbacks.size() <= frameSlot) {
    readbacks.resize(static_cast<size_t>(frameSlot) + 1u);
  }
  return readbacks[frameSlot];
}

void RendererFrontend::destroyReadbackSlots(
    std::vector<HostReadbackSlot>& readbacks) {
  for (HostReadbackSlot& slot : readbacks) {
    if (slot.readbackBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(slot.readbackBuffer);
    }
    slot = {};
  }
  readbacks.clear();
}

RendererFrontend::DepthVisibilityFrameSlot&
RendererFrontend::ensureDepthVisibilityFrameSlot(uint32_t frameSlot) {
  if (depthVisibility_.slots.size() <= frameSlot) {
    depthVisibility_.slots.resize(static_cast<size_t>(frameSlot) + 1u);
  }
  DepthVisibilityFrameSlot& frame =
      depthVisibility_.slots[static_cast<size_t>(frameSlot)];
  frame.frameSlot = frameSlot;
  return frame;
}

RendererFrontend::DepthVisibilityFrameSlot*
RendererFrontend::depthVisibilityFrameSlot(uint32_t frameSlot) {
  if (frameSlot >= depthVisibility_.slots.size()) {
    return nullptr;
  }
  return &depthVisibility_.slots[static_cast<size_t>(frameSlot)];
}

const RendererFrontend::DepthVisibilityFrameSlot*
RendererFrontend::depthVisibilityFrameSlot(uint32_t frameSlot) const {
  if (frameSlot >= depthVisibility_.slots.size()) {
    return nullptr;
  }
  return &depthVisibility_.slots[static_cast<size_t>(frameSlot)];
}

void RendererFrontend::invalidateDepthVisibilityFrames() {
  for (DepthVisibilityFrameSlot& frame : depthVisibility_.slots) {
    frame.valid = false;
    frame.renderFence = VK_NULL_HANDLE;
  }
}

void RendererFrontend::destroyDepthVisibilityFrameSlots() {
  for (DepthVisibilityFrameSlot& frame : depthVisibility_.slots) {
    if (frame.readback.readbackBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(frame.readback.readbackBuffer);
    }
    frame = {};
  }
  depthVisibility_.slots.clear();
  depthVisibility_.latestFrameSlot = 0;
}

void RendererFrontend::ensureScreenshotReadbackBuffer(uint32_t frameSlot,
                                                      VkExtent2D extent,
                                                      VkFormat format) {
  if (extent.width == 0 || extent.height == 0) {
    throw std::runtime_error("cannot capture a zero-sized swapchain image");
  }
  if (!isSupportedScreenshotFormat(format)) {
    throw std::runtime_error("unsupported swapchain screenshot format");
  }

  const VkDeviceSize requiredSize = static_cast<VkDeviceSize>(extent.width) *
                                    static_cast<VkDeviceSize>(extent.height) *
                                    4u;
  HostReadbackSlot& readback =
      ensureReadbackSlot(screenshot_.readbacks, frameSlot);
  if (readback.readbackBuffer.buffer != VK_NULL_HANDLE &&
      readback.readbackSize == requiredSize &&
      readback.extent.width == extent.width &&
      readback.extent.height == extent.height && readback.format == format) {
    return;
  }

  if (readback.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(readback.readbackBuffer);
  }
  readback.readbackBuffer = svc_.allocationManager.createBuffer(
      requiredSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  readback.readbackSize = requiredSize;
  readback.extent = extent;
  readback.format = format;
}

void RendererFrontend::writePendingScreenshotPng(uint32_t frameSlot) {
  if (!screenshot_.pending) {
    return;
  }
  if (frameSlot >= screenshot_.readbacks.size()) {
    throw std::runtime_error("screenshot readback buffer is not initialized");
  }
  HostReadbackSlot& readback = screenshot_.readbacks[frameSlot];
  if (readback.readbackBuffer.buffer == VK_NULL_HANDLE ||
      readback.readbackBuffer.allocation == nullptr ||
      readback.readbackSize == 0) {
    throw std::runtime_error("screenshot readback buffer is not initialized");
  }

  void *mapped = readback.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     readback.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map screenshot readback buffer");
    }
    mappedHere = true;
  }

  if (vmaInvalidateAllocation(
          svc_.allocationManager.memoryManager()->allocator(),
          readback.readbackBuffer.allocation, 0, readback.readbackSize) !=
      VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     readback.readbackBuffer.allocation);
    }
    throw std::runtime_error("failed to invalidate screenshot readback buffer");
  }

  const auto *src = static_cast<const unsigned char *>(mapped);
  const size_t pixelCount = static_cast<size_t>(readback.extent.width) *
                            static_cast<size_t>(readback.extent.height);
  std::vector<unsigned char> rgba =
      convertSwapchainBytesToRgba(src, pixelCount, readback.format);

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   readback.readbackBuffer.allocation);
  }

  if (!screenshot_.outputPath.parent_path().empty()) {
    std::filesystem::create_directories(screenshot_.outputPath.parent_path());
  }
  const std::string outputPath =
      container::util::pathToUtf8(screenshot_.outputPath);
  const int ok = stbi_write_png(
      outputPath.c_str(), static_cast<int>(readback.extent.width),
      static_cast<int>(readback.extent.height), 4, rgba.data(),
      static_cast<int>(readback.extent.width * 4));
  screenshot_.pending = false;
  if (ok == 0) {
    throw std::runtime_error("failed to write screenshot PNG: " + outputPath);
  }
}

void RendererFrontend::ensureDepthVisibilityReadbackBuffer(uint32_t frameSlot) {
  constexpr VkDeviceSize kDepthReadbackSize = sizeof(uint32_t);
  DepthVisibilityFrameSlot& frame = ensureDepthVisibilityFrameSlot(frameSlot);
  HostReadbackSlot& readback = frame.readback;
  if (readback.readbackBuffer.buffer != VK_NULL_HANDLE &&
      readback.readbackSize == kDepthReadbackSize) {
    return;
  }

  if (readback.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(readback.readbackBuffer);
  }

  readback.readbackBuffer = svc_.allocationManager.createBuffer(
      kDepthReadbackSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  readback.readbackSize = kDepthReadbackSize;
}

void RendererFrontend::markDepthVisibilityFrameComplete(uint32_t imageIndex,
                                                        uint32_t frameSlot) {
  DepthVisibilityFrameSlot& frame =
      ensureDepthVisibilityFrameSlot(frameSlot);
  depthVisibility_.latestFrameSlot = frameSlot;
  frame.valid = false;
  frame.renderFence = VK_NULL_HANDLE;
  if (!subs_.frameResourceManager ||
      imageIndex >= subs_.frameResourceManager->frameCount()) {
    return;
  }

  const VkImage depthStencilImage = deferredRasterRuntimeImage(
      subs_.frameResourceManager.get(), imageIndex, "depth-stencil");
  if (depthStencilImage == VK_NULL_HANDLE ||
      frame_.imagesInFlight.size() <= imageIndex) {
    return;
  }

  frame.frameSlot = frameSlot;
  frame.imageIndex = imageIndex;
  frame.extent = svc_.swapChainManager.extent();
  frame.format = resources_.gBufferFormats.depthStencil;
  frame.cameraData = buffers_.cameraData;
  frame.objectDataRevision =
      subs_.sceneController ? subs_.sceneController->objectDataRevision() : 0u;
  frame.bimObjectDataRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  frame.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  frame.sectionPlaneEnabled = currentSectionPlaneEquation(
      subs_.guiManager.get(), frame.sectionPlane);
  const BimDrawFilter bimFilter = currentBimDrawFilter();
  frame.bimTypeFilterEnabled = bimFilter.typeFilterEnabled;
  frame.bimFilterType = bimFilter.type;
  frame.bimStoreyFilterEnabled = bimFilter.storeyFilterEnabled;
  frame.bimFilterStorey = bimFilter.storey;
  frame.bimMaterialFilterEnabled = bimFilter.materialFilterEnabled;
  frame.bimFilterMaterial = bimFilter.material;
  frame.bimDisciplineFilterEnabled =
      bimFilter.disciplineFilterEnabled;
  frame.bimFilterDiscipline = bimFilter.discipline;
  frame.bimDisciplinePreset = bimFilter.disciplinePreset;
  frame.bimPhaseFilterEnabled = bimFilter.phaseFilterEnabled;
  frame.bimFilterPhase = bimFilter.phase;
  frame.bimPhaseTimelineEnabled = bimFilter.phaseTimelineEnabled;
  frame.bimPhaseTimelineActiveIndex =
      bimFilter.phaseTimelineActiveIndex;
  frame.bimPhaseTimelineShowExisting =
      bimFilter.phaseTimelineShowExisting;
  frame.bimPhaseTimelineShowNew = bimFilter.phaseTimelineShowNew;
  frame.bimPhaseTimelineShowDemolished =
      bimFilter.phaseTimelineShowDemolished;
  frame.bimPhaseTimelineGhostFuture =
      bimFilter.phaseTimelineGhostFuture;
  frame.bimFireRatingFilterEnabled =
      bimFilter.fireRatingFilterEnabled;
  frame.bimFilterFireRating = bimFilter.fireRating;
  frame.bimLoadBearingFilterEnabled =
      bimFilter.loadBearingFilterEnabled;
  frame.bimFilterLoadBearing = bimFilter.loadBearing;
  frame.bimStatusFilterEnabled = bimFilter.statusFilterEnabled;
  frame.bimFilterStatus = bimFilter.status;
  frame.bimDrawBudgetEnabled = bimFilter.drawBudgetEnabled;
  frame.bimDrawBudgetMaxObjects = bimFilter.drawBudgetMaxObjects;
  frame.bimIsolateSelection = bimFilter.isolateSelection;
  frame.bimHideSelection = bimFilter.hideSelection;
  if (subs_.guiManager) {
    const auto &layers = subs_.guiManager->bimLayerVisibilityState();
    frame.bimPointCloudVisible = layers.pointCloudVisible;
    frame.bimCurvesVisible = layers.curvesVisible;
  } else {
    frame.bimPointCloudVisible = true;
    frame.bimCurvesVisible = true;
  }
  frame.transparentPickDepthValid = false;
  if (subs_.sceneController &&
      hasTransparentCommands(
          subs_.sceneController->transparentDrawCommands(),
          subs_.sceneController->transparentSingleSidedDrawCommands(),
          subs_.sceneController->transparentWindingFlippedDrawCommands(),
          subs_.sceneController->transparentDoubleSidedDrawCommands())) {
    frame.transparentPickDepthValid = true;
  }
  if (!frame.transparentPickDepthValid && subs_.bimManager &&
      subs_.bimManager->hasScene()) {
    // Transparent-pick depth only records mesh and placeholder point/curve
    // surface paths. Native point/curve primitive passes render later in the
    // lighting pass, so they should not force a filteredDrawLists() lookup
    // here.
    const bool transparentPickSurfaceGeometry =
        hasTransparentBimSurfaceGeometry(*subs_.bimManager,
                                         frame.bimPointCloudVisible,
                                         frame.bimCurvesVisible);
    if (bimFilter.active()) {
      const bool gpuOpaqueFiltering = bimFrameGpuVisibilityAvailable(
          bimFrameGpuVisibilityInputs(*subs_.bimManager, bimFilter));
      if (gpuOpaqueFiltering &&
          hasTransparentBimSurfaceGeometry(*subs_.bimManager, false, false)) {
        // GPU-compacted transparent mesh draws are filtered by the visibility
        // mask in the transparent-pick pass. Treat the clear/no-draw case as a
        // valid transparent depth surface when the active filter removes all
        // transparent objects.
        frame.transparentPickDepthValid = true;
      }
      const bool cpuFilteredSurfaceGeometryRequired =
          transparentPickSurfaceGeometry &&
          (!gpuOpaqueFiltering ||
           (frame.bimPointCloudVisible &&
            hasAnyGeometry(subs_.bimManager->pointDrawLists())) ||
           (frame.bimCurvesVisible &&
            hasAnyGeometry(subs_.bimManager->curveDrawLists())));
      if (!frame.transparentPickDepthValid &&
          cpuFilteredSurfaceGeometryRequired) {
        const BimDrawLists &filteredDraws =
            subs_.bimManager->filteredDrawLists(bimFilter);
        frame.transparentPickDepthValid =
            hasTransparentBimSurfaceGeometry(
                filteredDraws, frame.bimPointCloudVisible,
                frame.bimCurvesVisible);
      }
    } else {
      frame.transparentPickDepthValid =
          transparentPickSurfaceGeometry;
    }
  }
  frame.selectedBimObjectIndex = bimFilter.selectedObjectIndex;
  frame.renderFence = frame_.imagesInFlight[imageIndex];
  frame.valid = true;
}

bool RendererFrontend::depthVisibilityFrameMatchesCurrentState() const {
  const DepthVisibilityFrameSlot* frame =
      depthVisibilityFrameSlot(depthVisibility_.latestFrameSlot);
  if (frame == nullptr || !frame->valid || !subs_.sceneController) {
    return false;
  }

  const VkExtent2D extent = svc_.swapChainManager.extent();
  if (frame->extent.width != extent.width ||
      frame->extent.height != extent.height || frame->extent.width == 0 ||
      frame->extent.height == 0) {
    return false;
  }
  if (!sameCameraData(frame->cameraData, buffers_.cameraData)) {
    return false;
  }
  if (frame->objectDataRevision != subs_.sceneController->objectDataRevision()) {
    return false;
  }
  glm::vec4 currentSectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  const bool currentSectionPlaneEnabled =
      currentSectionPlaneEquation(subs_.guiManager.get(), currentSectionPlane);
  if (frame->sectionPlaneEnabled != currentSectionPlaneEnabled) {
    return false;
  }
  if (currentSectionPlaneEnabled &&
      !sameVec4(frame->sectionPlane, currentSectionPlane)) {
    return false;
  }

  const BimDrawFilter currentBimFilter = currentBimDrawFilter();
  if (frame->bimTypeFilterEnabled !=
          currentBimFilter.typeFilterEnabled ||
      frame->bimFilterType != currentBimFilter.type ||
      frame->bimStoreyFilterEnabled !=
          currentBimFilter.storeyFilterEnabled ||
      frame->bimFilterStorey != currentBimFilter.storey ||
      frame->bimMaterialFilterEnabled !=
          currentBimFilter.materialFilterEnabled ||
      frame->bimFilterMaterial != currentBimFilter.material ||
      frame->bimDisciplineFilterEnabled !=
          currentBimFilter.disciplineFilterEnabled ||
      frame->bimFilterDiscipline != currentBimFilter.discipline ||
      frame->bimDisciplinePreset !=
          currentBimFilter.disciplinePreset ||
      frame->bimPhaseFilterEnabled !=
          currentBimFilter.phaseFilterEnabled ||
      frame->bimFilterPhase != currentBimFilter.phase ||
      frame->bimPhaseTimelineEnabled !=
          currentBimFilter.phaseTimelineEnabled ||
      frame->bimPhaseTimelineActiveIndex !=
          currentBimFilter.phaseTimelineActiveIndex ||
      frame->bimPhaseTimelineShowExisting !=
          currentBimFilter.phaseTimelineShowExisting ||
      frame->bimPhaseTimelineShowNew !=
          currentBimFilter.phaseTimelineShowNew ||
      frame->bimPhaseTimelineShowDemolished !=
          currentBimFilter.phaseTimelineShowDemolished ||
      frame->bimPhaseTimelineGhostFuture !=
          currentBimFilter.phaseTimelineGhostFuture ||
      frame->bimFireRatingFilterEnabled !=
          currentBimFilter.fireRatingFilterEnabled ||
      frame->bimFilterFireRating != currentBimFilter.fireRating ||
      frame->bimLoadBearingFilterEnabled !=
          currentBimFilter.loadBearingFilterEnabled ||
      frame->bimFilterLoadBearing != currentBimFilter.loadBearing ||
      frame->bimStatusFilterEnabled !=
          currentBimFilter.statusFilterEnabled ||
      frame->bimFilterStatus != currentBimFilter.status ||
      frame->bimDrawBudgetEnabled !=
          currentBimFilter.drawBudgetEnabled ||
      frame->bimDrawBudgetMaxObjects !=
          currentBimFilter.drawBudgetMaxObjects ||
      frame->bimIsolateSelection !=
          currentBimFilter.isolateSelection ||
      frame->bimHideSelection != currentBimFilter.hideSelection ||
      frame->selectedBimObjectIndex !=
          currentBimFilter.selectedObjectIndex) {
    return false;
  }
  if (subs_.guiManager) {
    const auto &layers = subs_.guiManager->bimLayerVisibilityState();
    if (frame->bimPointCloudVisible != layers.pointCloudVisible ||
        frame->bimCurvesVisible != layers.curvesVisible) {
      return false;
    }
  } else if (!frame->bimPointCloudVisible || !frame->bimCurvesVisible) {
    return false;
  }

  const uint64_t bimRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  return frame->bimObjectDataRevision == bimRevision;
}

BimDrawFilter RendererFrontend::currentBimDrawFilter() const {
  BimDrawFilter filter{};
  if (captureBimHiddenObject_) {
    filter.hideSelection = true;
    filter.selectedObjectIndex = *captureBimHiddenObject_;
  }
  if (!subs_.guiManager) {
    return filter;
  }
  const auto &guiFilter = subs_.guiManager->bimFilterState();
  filter.typeFilterEnabled = guiFilter.typeFilterEnabled;
  filter.type = guiFilter.type;
  filter.storeyFilterEnabled = guiFilter.storeyFilterEnabled;
  filter.storey = guiFilter.storey;
  filter.materialFilterEnabled = guiFilter.materialFilterEnabled;
  filter.material = guiFilter.material;
  filter.disciplineFilterEnabled = guiFilter.disciplineFilterEnabled;
  filter.discipline = guiFilter.discipline;
  filter.disciplinePreset = guiFilter.disciplinePreset;
  filter.phaseFilterEnabled = guiFilter.phaseFilterEnabled;
  filter.phase = guiFilter.phase;
  const auto &phaseTimeline = subs_.guiManager->bimPhaseTimelineUiState();
  filter.phaseTimelineEnabled = phaseTimeline.enabled;
  filter.phaseTimelineActiveIndex = phaseTimeline.activePhaseIndex;
  filter.phaseTimelineShowExisting = phaseTimeline.showExisting;
  filter.phaseTimelineShowNew = phaseTimeline.showNew;
  filter.phaseTimelineShowDemolished = phaseTimeline.showDemolished;
  filter.phaseTimelineGhostFuture = phaseTimeline.ghostFuture;
  filter.fireRatingFilterEnabled = guiFilter.fireRatingFilterEnabled;
  filter.fireRating = guiFilter.fireRating;
  filter.loadBearingFilterEnabled = guiFilter.loadBearingFilterEnabled;
  filter.loadBearing = guiFilter.loadBearing;
  filter.statusFilterEnabled = guiFilter.statusFilterEnabled;
  filter.status = guiFilter.status;
  filter.drawBudgetEnabled = guiFilter.drawBudgetEnabled;
  filter.drawBudgetMaxObjects = guiFilter.drawBudgetMaxObjects;
  filter.isolateSelection = guiFilter.isolateSelection;
  filter.hideSelection = guiFilter.hideSelection;
  filter.selectedObjectIndex = selectedBimObjectIndex_;
  if (captureBimHiddenObject_) {
    filter.hideSelection = true;
    filter.selectedObjectIndex = *captureBimHiddenObject_;
  }
  return filter;
}

bool RendererFrontend::bimObjectVisibleByLayer(uint32_t objectIndex) const {
  if (!subs_.bimManager) {
    return false;
  }
  const BimElementMetadata *metadata =
      subs_.bimManager->metadataForObject(objectIndex);
  if (metadata == nullptr) {
    return false;
  }
  if (!subs_.guiManager) {
    return true;
  }
  const auto &layerState = subs_.guiManager->bimLayerVisibilityState();
  switch (metadata->geometryKind) {
  case BimGeometryKind::Points:
    return layerState.pointCloudVisible;
  case BimGeometryKind::Curves:
    return layerState.curvesVisible;
  case BimGeometryKind::Mesh:
  default:
    return true;
  }
}

container::ui::ViewpointSnapshotState
RendererFrontend::currentViewpointSnapshot() const {
  container::ui::ViewpointSnapshotState snapshot{};
  if (subs_.cameraController) {
    snapshot.camera = subs_.cameraController->cameraTransformControls();
  }
  snapshot.selectedMeshNode = sceneState_.selectedMeshNode;
  snapshot.selectedBimObjectIndex = selectedBimObjectIndex_;
  if (subs_.guiManager) {
    snapshot.bimFilter = subs_.guiManager->bimFilterState();
    snapshot.phaseTimeline = subs_.guiManager->bimPhaseTimelineUiState();
  }
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    snapshot.bimModelPath = subs_.bimManager->modelPath();
    if (const auto *metadata =
            subs_.bimManager->metadataForObject(selectedBimObjectIndex_)) {
      snapshot.selectedBimObjectIndex = metadata->objectIndex;
      snapshot.selectedBimGuid = metadata->guid;
      snapshot.selectedBimType = metadata->type;
      snapshot.selectedBimSourceId = metadata->sourceId;
    }
  }
  return snapshot;
}

bool RendererFrontend::restoreViewpointSnapshot(
    const container::ui::ViewpointSnapshotState &snapshot) {
  if (!subs_.cameraController) {
    return false;
  }

  subs_.cameraController->applyCameraTransform(
      snapshot.camera, buffers_.cameraData,
      buffers_.cameras.empty() ? container::gpu::AllocatedBuffer{}
                               : buffers_.cameras.front());
  for (uint32_t imageIndex = 1;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }

  auto clearSelectionState = [this]() {
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
    selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
    selectionNavigationAnchor_ = {};
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (subs_.lightingManager) {
      subs_.lightingManager->selectEditableLight({});
    }
    if (subs_.guiManager) {
      subs_.guiManager->setSectionPlaneVisualEditable(false);
    }
  };

  const uint32_t invalidObjectIndex = std::numeric_limits<uint32_t>::max();
  uint32_t restoredBimObjectIndex = invalidObjectIndex;
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const bool sameBimScene =
        snapshot.bimModelPath.empty() ||
        snapshot.bimModelPath == subs_.bimManager->modelPath();
    auto chooseIndexedObject =
        [this, &snapshot, invalidObjectIndex](
            std::span<const uint32_t> objectIndices) -> uint32_t {
      if (objectIndices.empty()) {
        return invalidObjectIndex;
      }
      if (std::ranges::find(objectIndices, snapshot.selectedBimObjectIndex) !=
          objectIndices.end()) {
        return snapshot.selectedBimObjectIndex;
      }
      for (uint32_t objectIndex : objectIndices) {
        const BimElementMetadata *metadata =
            subs_.bimManager->metadataForObject(objectIndex);
        if (snapshot.selectedBimType.empty() ||
            (metadata != nullptr &&
             metadata->type == snapshot.selectedBimType)) {
          return objectIndex;
        }
      }
      return objectIndices.front();
    };

    if (sameBimScene && !snapshot.selectedBimGuid.empty()) {
      restoredBimObjectIndex = chooseIndexedObject(
          subs_.bimManager->objectIndicesForGuid(snapshot.selectedBimGuid));
    }

    if (sameBimScene && restoredBimObjectIndex == invalidObjectIndex &&
        !snapshot.selectedBimSourceId.empty()) {
      restoredBimObjectIndex =
          chooseIndexedObject(subs_.bimManager->objectIndicesForSourceId(
              snapshot.selectedBimSourceId));
    }

    if (sameBimScene && restoredBimObjectIndex == invalidObjectIndex &&
        snapshot.selectedBimObjectIndex <
            subs_.bimManager->objectData().size()) {
      const auto *metadata =
          subs_.bimManager->metadataForObject(snapshot.selectedBimObjectIndex);
      const bool guidCompatible =
          snapshot.selectedBimGuid.empty() ||
          (metadata != nullptr && metadata->guid == snapshot.selectedBimGuid);
      const bool sourceIdCompatible =
          snapshot.selectedBimSourceId.empty() ||
          (metadata != nullptr &&
           metadata->sourceId == snapshot.selectedBimSourceId);
      const bool typeCompatible =
          snapshot.selectedBimType.empty() ||
          (metadata != nullptr && metadata->type == snapshot.selectedBimType);
      if (guidCompatible && sourceIdCompatible && typeCompatible) {
        restoredBimObjectIndex = snapshot.selectedBimObjectIndex;
      }
    }
  }

  if (restoredBimObjectIndex != invalidObjectIndex) {
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
    selectedBimObjectIndex_ = restoredBimObjectIndex;
    selectionNavigationAnchor_ = {};
    if (subs_.lightingManager) {
      subs_.lightingManager->selectEditableLight({});
    }
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (subs_.guiManager) {
      subs_.guiManager->setSectionPlaneVisualEditable(false);
    }
    return true;
  }

  if (snapshot.selectedMeshNode != container::scene::SceneGraph::kInvalidNode &&
      sceneGraph_.getNode(snapshot.selectedMeshNode) != nullptr) {
    sceneState_.selectedMeshNode = snapshot.selectedMeshNode;
    selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
    selectionNavigationAnchor_ = {};
    if (subs_.lightingManager) {
      subs_.lightingManager->selectEditableLight({});
    }
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (subs_.guiManager) {
      subs_.guiManager->setSectionPlaneVisualEditable(false);
    }
    return true;
  }

  clearSelectionState();
  return true;
}

bool RendererFrontend::sampleDepthAtCursor(double cursorX, double cursorY,
                                           float &outDepth) {
  return sampleDepthAtCursor(cursorX, cursorY, outDepth, false);
}

bool RendererFrontend::samplePickDepthAtCursor(double cursorX, double cursorY,
                                               float &outDepth) {
  return sampleDepthAtCursor(cursorX, cursorY, outDepth, true);
}

bool RendererFrontend::sampleDepthAtCursor(double cursorX, double cursorY,
                                           float &outDepth, bool pickDepth) {
  if (!depthVisibilityFrameMatchesCurrentState() ||
      !subs_.frameResourceManager) {
    return false;
  }
  DepthVisibilityFrameSlot* frame =
      depthVisibilityFrameSlot(depthVisibility_.latestFrameSlot);
  if (frame == nullptr ||
      frame->imageIndex >= subs_.frameResourceManager->frameCount()) {
    return false;
  }
  if (pickDepth && !frame->transparentPickDepthValid) {
    return false;
  }

  const VkImage depthImage = deferredRasterRuntimeImage(
      subs_.frameResourceManager.get(), frame->imageIndex,
      pickDepth ? "pick-depth" : "depth-stencil");
  if (depthImage == VK_NULL_HANDLE || cursorX < 0.0 || cursorY < 0.0 ||
      cursorX >= static_cast<double>(frame->extent.width) ||
      cursorY >= static_cast<double>(frame->extent.height)) {
    return false;
  }

  if (frame->renderFence != VK_NULL_HANDLE) {
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                        &frame->renderFence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
  }

  ensureDepthVisibilityReadbackBuffer(frame->frameSlot);
  frame = depthVisibilityFrameSlot(depthVisibility_.latestFrameSlot);
  if (frame == nullptr) {
    return false;
  }
  HostReadbackSlot& readback = frame->readback;
  if (readback.readbackBuffer.buffer == VK_NULL_HANDLE ||
      readback.readbackBuffer.allocation == nullptr) {
    return false;
  }

  const uint32_t x =
      std::min<uint32_t>(static_cast<uint32_t>(std::floor(cursorX)),
                         frame->extent.width - 1u);
  const uint32_t y =
      std::min<uint32_t>(static_cast<uint32_t>(std::floor(cursorY)),
                         frame->extent.height - 1u);

  VkCommandBufferAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = svc_.commandBufferManager.pool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  if (vkAllocateCommandBuffers(svc_.ctx.deviceWrapper->device(), &allocInfo,
                               &commandBuffer) != VK_SUCCESS) {
    return false;
  }

  auto freeCommandBuffer = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(svc_.ctx.deviceWrapper->device(),
                           svc_.commandBufferManager.pool(), 1, &commandBuffer);
      commandBuffer = VK_NULL_HANDLE;
    }
  };

  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = pickDepth
                             ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                             : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = depthImage;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion{};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y),
                            0};
  copyRegion.imageExtent = {1u, 1u, 1u};
  vkCmdCopyImageToBuffer(
      commandBuffer, depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      readback.readbackBuffer.buffer, 1, &copyRegion);

  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = readback.readbackBuffer.buffer;
  hostRead.offset = 0;
  hostRead.size = readback.readbackSize;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead,
                       0, nullptr);

  VkImageMemoryBarrier toReadOnly = toTransfer;
  toReadOnly.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toReadOnly.dstAccessMask =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  toReadOnly.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toReadOnly.newLayout = pickDepth
                             ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                             : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toReadOnly);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  if (!submitReadbackCommandBufferAndWait(
          svc_.ctx.deviceWrapper->device(),
          svc_.ctx.deviceWrapper->graphicsQueue(), commandBuffer)) {
    freeCommandBuffer();
    return false;
  }
  freeCommandBuffer();

  void *mapped = readback.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     readback.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      return false;
    }
    mappedHere = true;
  }

  const VkResult invalidateResult = vmaInvalidateAllocation(
      svc_.allocationManager.memoryManager()->allocator(),
      readback.readbackBuffer.allocation, 0, readback.readbackSize);
  const bool decoded =
      invalidateResult == VK_SUCCESS &&
      decodeDepthReadbackValue(frame->format, mapped, outDepth);

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   readback.readbackBuffer.allocation);
  }
  return decoded;
}

bool RendererFrontend::samplePickIdAtCursor(double cursorX, double cursorY,
                                            uint32_t &outPickId) {
  if (!depthVisibilityFrameMatchesCurrentState() ||
      !subs_.frameResourceManager) {
    return false;
  }
  DepthVisibilityFrameSlot* frame =
      depthVisibilityFrameSlot(depthVisibility_.latestFrameSlot);
  if (frame == nullptr ||
      frame->imageIndex >= subs_.frameResourceManager->frameCount()) {
    return false;
  }

  const VkImage pickIdImage = deferredRasterRuntimeImage(
      subs_.frameResourceManager.get(), frame->imageIndex, "pick-id");
  if (pickIdImage == VK_NULL_HANDLE || cursorX < 0.0 || cursorY < 0.0 ||
      cursorX >= static_cast<double>(frame->extent.width) ||
      cursorY >= static_cast<double>(frame->extent.height)) {
    return false;
  }

  if (frame->renderFence != VK_NULL_HANDLE) {
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                        &frame->renderFence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
  }

  ensureDepthVisibilityReadbackBuffer(frame->frameSlot);
  frame = depthVisibilityFrameSlot(depthVisibility_.latestFrameSlot);
  if (frame == nullptr) {
    return false;
  }
  HostReadbackSlot& readback = frame->readback;
  if (readback.readbackBuffer.buffer == VK_NULL_HANDLE ||
      readback.readbackBuffer.allocation == nullptr) {
    return false;
  }

  const uint32_t x =
      std::min<uint32_t>(static_cast<uint32_t>(std::floor(cursorX)),
                         frame->extent.width - 1u);
  const uint32_t y =
      std::min<uint32_t>(static_cast<uint32_t>(std::floor(cursorY)),
                         frame->extent.height - 1u);

  VkCommandBufferAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = svc_.commandBufferManager.pool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  if (vkAllocateCommandBuffers(svc_.ctx.deviceWrapper->device(), &allocInfo,
                               &commandBuffer) != VK_SUCCESS) {
    return false;
  }

  auto freeCommandBuffer = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(svc_.ctx.deviceWrapper->device(),
                           svc_.commandBufferManager.pool(), 1, &commandBuffer);
      commandBuffer = VK_NULL_HANDLE;
    }
  };

  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = pickIdImage;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion{};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y),
                            0};
  copyRegion.imageExtent = {1u, 1u, 1u};
  vkCmdCopyImageToBuffer(
      commandBuffer, pickIdImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      readback.readbackBuffer.buffer, 1, &copyRegion);

  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = readback.readbackBuffer.buffer;
  hostRead.offset = 0;
  hostRead.size = readback.readbackSize;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead,
                       0, nullptr);

  VkImageMemoryBarrier toReadOnly = toTransfer;
  toReadOnly.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toReadOnly.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toReadOnly.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toReadOnly.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toReadOnly);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  if (!submitReadbackCommandBufferAndWait(
          svc_.ctx.deviceWrapper->device(),
          svc_.ctx.deviceWrapper->graphicsQueue(), commandBuffer)) {
    freeCommandBuffer();
    return false;
  }
  freeCommandBuffer();

  void *mapped = readback.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     readback.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      return false;
    }
    mappedHere = true;
  }

  const VkResult invalidateResult = vmaInvalidateAllocation(
      svc_.allocationManager.memoryManager()->allocator(),
      readback.readbackBuffer.allocation, 0, readback.readbackSize);
  if (invalidateResult == VK_SUCCESS) {
    std::memcpy(&outPickId, mapped, sizeof(outPickId));
  }

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   readback.readbackBuffer.allocation);
  }
  return invalidateResult == VK_SUCCESS;
}

void RendererFrontend::presentSceneControls() {
  if (!subs_.guiManager)
    return;

  if (subs_.gpuCullManager) {
    const auto stats = subs_.gpuCullManager->cullStats();
    subs_.guiManager->setCullStats(stats.totalInputCount,
                                   stats.frustumPassedCount,
                                   stats.occlusionPassedCount);
  }
  if (subs_.lightingManager) {
    subs_.guiManager->setLightCullingStats(
        subs_.lightingManager->lightCullingStats());
    subs_.guiManager->setLightingSettings(
        subs_.lightingManager->lightingSettings());
  }
  if (subs_.rendererTelemetry) {
    subs_.guiManager->setRendererTelemetry(subs_.rendererTelemetry->view());
  }

  // Sync freeze-culling: push debug state into GUI before rendering.
  subs_.guiManager->setFreezeCulling(debugState_.freezeCulling);
  subs_.guiManager->setMsaaSampleState(
      std::span<const uint32_t>(supportedMsaaSamples_.data(),
                                supportedMsaaSamples_.size()),
      sampleCountToSamples(msaaSampleCount_));

  if (subs_.temporalManager)
    subs_.guiManager->setTemporalSettings(
        subs_.temporalManager->settings(), subs_.temporalManager->memoryBytes(),
        subs_.temporalManager->state().frameId(),
        subs_.temporalManager->state().epoch(),
        subs_.temporalManager->state().resetReason(),
        {svc_.swapChainManager.extent().width,
         svc_.swapChainManager.extent().height},
        subs_.temporalManager->allocatedBytes(), [&] {
          if (subs_.rendererTelemetry)
            for (const auto &pass : subs_.rendererTelemetry->latest().passes)
              if (pass.name == "TemporalResolve" && pass.gpuTimed)
                return pass.gpuKnownMs;
          return 0.0f;
        }());
  // Sync bloom: push BloomManager settings into GUI before rendering.
  if (subs_.bloomManager) {
    subs_.guiManager->setBloomSettings(
        subs_.bloomManager->enabled(), subs_.bloomManager->threshold(),
        subs_.bloomManager->knee(), subs_.bloomManager->intensity(),
        subs_.bloomManager->filterRadius());
  }

  // Sync render pass toggles: push graph pass list into GUI before rendering.
  if (subs_.frameRecorder) {
    DebugUiPresenter::publishRenderPasses(*subs_.guiManager,
                                          subs_.frameRecorder->graph());
  }
  syncGuiRenderEngineOptions();

  processPendingGuiModelLoadRequest();

  subs_.guiManager->startFrame();
  const GuiFrameExceptionGuard guiFrameExceptionGuard(*subs_.guiManager);
  const std::vector<container::gpu::PointLightData> emptyPointLights;
  const auto &pointLights = subs_.lightingManager
                                ? subs_.lightingManager->pointLightsSsbo()
                                : emptyPointLights;
  const std::vector<container::renderer::EditableLightEntity>
      emptyEditableLights;
  const auto &editableLights = subs_.lightingManager
                                   ? subs_.lightingManager->editableLights()
                                   : emptyEditableLights;
  const auto selectedEditableLight =
      subs_.lightingManager ? subs_.lightingManager->selectedEditableLightId()
                            : container::renderer::EditableLightId{};
  std::vector<BimScheduleElement> bimScheduleElements;
  std::vector<BimScheduleRow> bimScheduleByClassAndStoreyRows;
  std::vector<BimScheduleRow> bimScheduleByTypeAndStoreyRows;
  std::vector<BimScheduleRow> bimScheduleByMaterialRows;
  std::vector<BimModelCompareElement> bimModelCompareElements;
  container::ui::BimInspectionState bimInspection{};
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const BimSceneStats stats = subs_.bimManager->sceneStats();
    const BimOptimizedModelMetadata &optimizedMetadata =
        subs_.bimManager->optimizedModelMetadata();
    const BimDrawBudgetLodStats drawBudgetLodStats =
        subs_.bimManager->drawBudgetLodStats(currentBimDrawFilter());
    const auto &elementTypes = subs_.bimManager->elementTypes();
    const auto &elementStoreys = subs_.bimManager->elementStoreys();
    const auto &elementMaterials = subs_.bimManager->elementMaterials();
    const auto &elementDisciplines = subs_.bimManager->elementDisciplines();
    const auto &elementPhases = subs_.bimManager->elementPhases();
    const auto &elementFireRatings = subs_.bimManager->elementFireRatings();
    const auto &elementLoadBearingValues =
        subs_.bimManager->elementLoadBearingValues();
    const auto &elementStatuses = subs_.bimManager->elementStatuses();
    const auto &elementStoreyRanges = subs_.bimManager->elementStoreyRanges();
    const auto &elementMetadata = subs_.bimManager->elementMetadata();
    bimScheduleElements = buildBimScheduleElements(elementMetadata);
    bimScheduleByClassAndStoreyRows =
        buildBimScheduleByIfcClassAndStorey(bimScheduleElements);
    bimScheduleByTypeAndStoreyRows =
        buildBimScheduleByTypeAndStorey(bimScheduleElements);
    bimScheduleByMaterialRows =
        buildBimScheduleMaterialTotals(bimScheduleElements);
    bimModelCompareElements = buildBimModelCompareElements(elementMetadata);
    bimInspection.hasScene = true;
    bimInspection.modelPath = subs_.bimManager->modelPath();
    bimInspection.objectCount = stats.objectCount;
    bimInspection.meshObjectCount = stats.meshObjectCount;
    bimInspection.pointObjectCount = stats.pointObjectCount;
    bimInspection.curveObjectCount = stats.curveObjectCount;
    bimInspection.opaqueDrawCount = stats.opaqueDrawCount;
    bimInspection.transparentDrawCount = stats.transparentDrawCount;
    bimInspection.pointOpaqueDrawCount = stats.pointOpaqueDrawCount;
    bimInspection.pointTransparentDrawCount = stats.pointTransparentDrawCount;
    bimInspection.curveOpaqueDrawCount = stats.curveOpaqueDrawCount;
    bimInspection.curveTransparentDrawCount = stats.curveTransparentDrawCount;
    bimInspection.nativePointOpaqueDrawCount = stats.nativePointOpaqueDrawCount;
    bimInspection.nativePointTransparentDrawCount =
        stats.nativePointTransparentDrawCount;
    bimInspection.nativeCurveOpaqueDrawCount = stats.nativeCurveOpaqueDrawCount;
    bimInspection.nativeCurveTransparentDrawCount =
        stats.nativeCurveTransparentDrawCount;
    bimInspection.meshletClusterCount = stats.meshletClusterCount;
    bimInspection.meshletSourceClusterCount = stats.meshletSourceClusterCount;
    bimInspection.meshletEstimatedClusterCount =
        stats.meshletEstimatedClusterCount;
    bimInspection.meshletObjectReferenceCount =
        stats.meshletObjectReferenceCount;
    bimInspection.meshletGpuResidentObjectCount =
        stats.meshletGpuResidentObjectCount;
    bimInspection.meshletGpuResidentClusterCount =
        stats.meshletGpuResidentClusterCount;
    bimInspection.meshletGpuBufferBytes = stats.meshletGpuBufferBytes;
    bimInspection.meshletGpuComputeReady = stats.meshletGpuComputeReady;
    bimInspection.meshletGpuDispatchPending = stats.meshletGpuDispatchPending;
    bimInspection.meshletMaxLodLevel = stats.meshletMaxLodLevel;
    bimInspection.optimizedModelMetadataCacheable =
        stats.optimizedModelMetadataCacheable;
    bimInspection.optimizedModelMetadataCacheHit =
        stats.optimizedModelMetadataCacheHit;
    bimInspection.optimizedModelMetadataCacheStale =
        stats.optimizedModelMetadataCacheStale;
    bimInspection.optimizedModelMetadataCacheWritten =
        stats.optimizedModelMetadataCacheWriteSucceeded;
    bimInspection.optimizedModelMetadataCacheKey = optimizedMetadata.cacheKey;
    bimInspection.optimizedModelMetadataCachePath = optimizedMetadata.cachePath;
    bimInspection.optimizedModelMetadataCacheStatus =
        optimizedMetadata.cacheStatus;
    bimInspection.drawBudgetVisibleObjectCount =
        drawBudgetLodStats.visibleObjectCount;
    bimInspection.drawBudgetVisibleMeshObjectCount =
        drawBudgetLodStats.visibleMeshObjectCount;
    bimInspection.drawBudgetVisibleMeshletClusterCount =
        drawBudgetLodStats.visibleMeshletClusterReferences;
    bimInspection.drawBudgetVisibleMaxLodLevel =
        drawBudgetLodStats.visibleMaxLodLevel;
    bimInspection.floorPlanDrawCount = stats.floorPlanDrawCount;
    bimInspection.uniqueTypeCount = stats.uniqueTypeCount;
    bimInspection.uniqueStoreyCount = stats.uniqueStoreyCount;
    bimInspection.uniqueMaterialCount = stats.uniqueMaterialCount;
    bimInspection.uniqueDisciplineCount = stats.uniqueDisciplineCount;
    bimInspection.uniquePhaseCount = stats.uniquePhaseCount;
    bimInspection.uniqueFireRatingCount = stats.uniqueFireRatingCount;
    bimInspection.uniqueLoadBearingCount = stats.uniqueLoadBearingCount;
    bimInspection.uniqueStatusCount = stats.uniqueStatusCount;
    const BimModelUnitMetadata &unitMetadata =
        subs_.bimManager->modelUnitMetadata();
    bimInspection.hasSourceUnits = unitMetadata.hasSourceUnits;
    bimInspection.sourceUnits = unitMetadata.sourceUnits;
    bimInspection.hasMetersPerUnit = unitMetadata.hasMetersPerUnit;
    bimInspection.metersPerUnit = unitMetadata.metersPerUnit;
    bimInspection.hasImportScale = unitMetadata.hasImportScale;
    bimInspection.importScale = unitMetadata.importScale;
    bimInspection.hasEffectiveImportScale =
        unitMetadata.hasEffectiveImportScale;
    bimInspection.effectiveImportScale = unitMetadata.effectiveImportScale;
    const BimModelGeoreferenceMetadata &georeferenceMetadata =
        subs_.bimManager->modelGeoreferenceMetadata();
    bimInspection.hasSourceUpAxis = georeferenceMetadata.hasSourceUpAxis;
    bimInspection.sourceUpAxis = georeferenceMetadata.sourceUpAxis;
    bimInspection.hasCoordinateOffset =
        georeferenceMetadata.hasCoordinateOffset;
    bimInspection.coordinateOffset = georeferenceMetadata.coordinateOffset;
    bimInspection.coordinateOffsetSource =
        georeferenceMetadata.coordinateOffsetSource;
    bimInspection.crsName = georeferenceMetadata.crsName;
    bimInspection.crsAuthority = georeferenceMetadata.crsAuthority;
    bimInspection.crsCode = georeferenceMetadata.crsCode;
    bimInspection.mapConversionName = georeferenceMetadata.mapConversionName;
    const BimGeoreferenceMetadata coordinateReadoutMetadata =
        buildBimGeoreferenceMetadata(unitMetadata, georeferenceMetadata);
    bimInspection.scheduleByClassAndStoreyRows =
        std::span<const BimScheduleRow>(bimScheduleByClassAndStoreyRows.data(),
                                        bimScheduleByClassAndStoreyRows.size());
    bimInspection.scheduleByTypeAndStoreyRows =
        std::span<const BimScheduleRow>(bimScheduleByTypeAndStoreyRows.data(),
                                        bimScheduleByTypeAndStoreyRows.size());
    bimInspection.scheduleByMaterialRows =
        std::span<const BimScheduleRow>(bimScheduleByMaterialRows.data(),
                                        bimScheduleByMaterialRows.size());
    bimInspection.modelCompareElements =
        std::span<const BimModelCompareElement>(bimModelCompareElements.data(),
                                                bimModelCompareElements.size());
    bimInspection.hasOriginRebaseRecommendation =
        hasBimGeoreferenceReadoutMetadata(coordinateReadoutMetadata);
    bimInspection.originRebaseRecommendation =
        recommendBimOriginRebase(glm::dvec3{0.0}, coordinateReadoutMetadata);
    bimInspection.elementTypes =
        std::span<const std::string>(elementTypes.data(), elementTypes.size());
    bimInspection.elementStoreys = std::span<const std::string>(
        elementStoreys.data(), elementStoreys.size());
    bimInspection.elementMaterials = std::span<const std::string>(
        elementMaterials.data(), elementMaterials.size());
    bimInspection.elementDisciplines = std::span<const std::string>(
        elementDisciplines.data(), elementDisciplines.size());
    bimInspection.elementPhases = std::span<const std::string>(
        elementPhases.data(), elementPhases.size());
    bimInspection.elementFireRatings = std::span<const std::string>(
        elementFireRatings.data(), elementFireRatings.size());
    bimInspection.elementLoadBearingValues = std::span<const std::string>(
        elementLoadBearingValues.data(), elementLoadBearingValues.size());
    bimInspection.elementStatuses = std::span<const std::string>(
        elementStatuses.data(), elementStatuses.size());
    bimInspection.elementStoreyRanges = std::span<const BimStoreyRange>(
        elementStoreyRanges.data(), elementStoreyRanges.size());
    bimInspection.relationshipGraph = &subs_.bimManager->relationshipGraph();
    if (const auto *metadata =
            subs_.bimManager->metadataForObject(selectedBimObjectIndex_)) {
      bimInspection.hasSelection = true;
      bimInspection.selectedObjectIndex = metadata->objectIndex;
      bimInspection.sourceElementIndex = metadata->sourceElementIndex;
      bimInspection.meshId = metadata->meshId;
      bimInspection.sourceMaterialIndex = metadata->sourceMaterialIndex;
      bimInspection.materialIndex = metadata->materialIndex;
      bimInspection.semanticTypeId = metadata->semanticTypeId;
      bimInspection.sourceColor = metadata->sourceColor;
      bimInspection.guid = metadata->guid;
      bimInspection.type = metadata->type;
      bimInspection.displayName = metadata->displayName;
      bimInspection.objectType = metadata->objectType;
      bimInspection.storeyName = metadata->storeyName;
      bimInspection.storeyId = metadata->storeyId;
      bimInspection.materialName = metadata->materialName;
      bimInspection.materialCategory = metadata->materialCategory;
      bimInspection.discipline = metadata->discipline;
      bimInspection.phase = metadata->phase;
      bimInspection.fireRating = metadata->fireRating;
      bimInspection.loadBearing = metadata->loadBearing;
      bimInspection.status = metadata->status;
      bimInspection.sourceId = metadata->sourceId;
      bimInspection.geometryKind = bimGeometryKindLabel(metadata->geometryKind);
      bimInspection.properties = std::span<const BimElementProperty>(
          metadata->properties.data(), metadata->properties.size());
      bimInspection.transparent = metadata->transparent;
      bimInspection.doubleSided = metadata->doubleSided;
      const BimElementBounds elementBounds =
          subs_.bimManager->elementBoundsForObject(selectedBimObjectIndex_);
      if (elementBounds.valid) {
        bimInspection.hasSelectionBounds = true;
        bimInspection.selectionBoundsMin = elementBounds.min;
        bimInspection.selectionBoundsMax = elementBounds.max;
        bimInspection.selectionBoundsCenter = elementBounds.center;
        bimInspection.selectionBoundsSize = elementBounds.size;
        bimInspection.selectionBoundsRadius = elementBounds.radius;
        bimInspection.selectionFloorElevation = elementBounds.floorElevation;
        bimInspection.hasSelectedCoordinateReadout = true;
        bimInspection.selectedCoordinateReadout = buildBimCoordinateReadout(
            glm::dvec3(elementBounds.center), coordinateReadoutMetadata);
        bimInspection.hasOriginRebaseRecommendation = true;
        bimInspection.originRebaseRecommendation = recommendBimOriginRebase(
            glm::dvec3(elementBounds.center), coordinateReadoutMetadata);
      }
    }
  }
  const container::ui::ViewpointSnapshotState currentViewpoint =
      currentViewpointSnapshot();
  const auto uploadCameraBuffers = [this]() {
    for (uint32_t imageIndex = 0;
         imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
         ++imageIndex) {
      updateCameraBuffer(imageIndex);
    }
  };
  const std::function<void(container::renderer::ScenePrimitiveKind)>
      addScenePrimitive = [this](container::renderer::ScenePrimitiveKind kind) {
        if (!subs_.sceneController) {
          return;
        }
        const uint32_t node = subs_.sceneController->addScenePrimitive(
            kind, glm::mat4(1.0f), sceneState_.rootNode);
        if (node == container::scene::SceneGraph::kInvalidNode) {
          return;
        }
        if (subs_.cameraController) {
          subs_.cameraController->selectMeshNode(node,
                                                 sceneState_.selectedMeshNode);
        } else {
          sceneState_.selectedMeshNode = node;
        }
        if (subs_.lightingManager) {
          subs_.lightingManager->selectEditableLight({});
        }
        if (subs_.guiManager) {
          subs_.guiManager->setSectionPlaneVisualEditable(false);
        }
        syncSceneStateFromController();
        selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
        selectionNavigationAnchor_ = {};
        clearHoveredMeshNode();
        selectedDrawCommands_.clear();
        selectedBimDrawCommands_.clear();
        refreshSceneObjectData();
        syncSceneProviders();
      };
  subs_.guiManager->drawSceneControls(
      sceneGraph_,
      addScenePrimitive,
      subs_.cameraController ? subs_.cameraController->cameraTransformControls()
                             : container::ui::TransformControls{},
      [this,
       uploadCameraBuffers](const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyCameraTransform(
              controls, buffers_.cameraData,
              buffers_.cameras.empty() ? container::gpu::AllocatedBuffer{}
                                       : buffers_.cameras.front());
        uploadCameraBuffers();
      },
      subs_.cameraController
          ? subs_.cameraController->nodeTransformControls(sceneState_.rootNode)
          : container::ui::TransformControls{},
      [this](const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              sceneState_.rootNode, sceneState_.rootNode, controls);
        if (subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        refreshSceneObjectData();
      },
      subs_.lightingManager ? subs_.lightingManager->directionalLightPosition()
                            : glm::vec3{0.0f},
      subs_.lightingManager ? subs_.lightingManager->lightingData()
                            : container::gpu::LightingData{},
      pointLights, editableLights, selectedEditableLight,
      [this](container::renderer::EditableLightId id) {
        if (!subs_.lightingManager) {
          return;
        }
        subs_.lightingManager->selectEditableLight(id);
        sceneState_.selectedMeshNode =
            container::scene::SceneGraph::kInvalidNode;
        selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
        selectionNavigationAnchor_ = {};
        clearHoveredMeshNode();
        selectedDrawCommands_.clear();
        selectedBimDrawCommands_.clear();
        if (subs_.guiManager) {
          subs_.guiManager->setSectionPlaneVisualEditable(false);
          if (container::renderer::isValidEditableLightId(id)) {
            subs_.guiManager->setStatusMessage("Selected editable light");
          }
        }
      },
      [this](const container::renderer::EditableLightEntity &light) {
        if (!subs_.lightingManager ||
            !subs_.lightingManager->updateEditableLight(light)) {
          return;
        }
        sceneState_.selectedMeshNode =
            container::scene::SceneGraph::kInvalidNode;
        selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
        selectionNavigationAnchor_ = {};
        if (subs_.guiManager) {
          subs_.guiManager->setSectionPlaneVisualEditable(false);
        }
        subs_.lightingManager->updateLightingData();
        updateFrameDescriptorSets();
      },
      [this](container::renderer::EditableLightType type) {
        if (!subs_.lightingManager) {
          return;
        }
        const auto id = subs_.lightingManager->addManualEditableLight(type);
        subs_.lightingManager->updateLightingData();
        subs_.lightingManager->selectEditableLight(id);
        sceneState_.selectedMeshNode =
            container::scene::SceneGraph::kInvalidNode;
        selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
        selectionNavigationAnchor_ = {};
        selectedDrawCommands_.clear();
        selectedBimDrawCommands_.clear();
        if (subs_.guiManager) {
          subs_.guiManager->setSectionPlaneVisualEditable(false);
        }
        updateFrameDescriptorSets();
        if (subs_.guiManager) {
          subs_.guiManager->setStatusMessage("Selected editable light");
        }
      },
      sceneState_.selectedMeshNode, bimInspection, currentViewpoint,
      [this](const container::ui::ViewpointSnapshotState &snapshot) {
        return restoreViewpointSnapshot(snapshot);
      },
      sceneState_.rootNode,
      [this](uint32_t nodeIndex) {
        if (nodeIndex == container::scene::SceneGraph::kInvalidNode) {
          clearSelectedMeshNode();
          return;
        }
        if (subs_.cameraController) {
          subs_.cameraController->selectMeshNode(nodeIndex,
                                                 sceneState_.selectedMeshNode);
          selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
          if (subs_.lightingManager) {
            subs_.lightingManager->selectEditableLight({});
          }
          selectionNavigationAnchor_ = {};
          clearHoveredMeshNode();
          selectedDrawCommands_.clear();
          selectedBimDrawCommands_.clear();
          selectedBimNativePointDrawCommands_.clear();
          selectedBimNativeCurveDrawCommands_.clear();
          if (subs_.guiManager) {
            subs_.guiManager->setSectionPlaneVisualEditable(false);
            subs_.guiManager->setStatusMessage(
                "Selected node " +
                std::to_string(sceneState_.selectedMeshNode));
          }
        }
      },
      [this, uploadCameraBuffers](uint32_t nodeIndex) {
        if (nodeIndex == container::scene::SceneGraph::kInvalidNode ||
            sceneGraph_.getNode(nodeIndex) == nullptr) {
          return;
        }
        if (subs_.cameraController) {
          subs_.cameraController->selectMeshNode(nodeIndex,
                                                 sceneState_.selectedMeshNode);
          subs_.cameraController->frameNodeOrScene(nodeIndex);
          uploadCameraBuffers();
        } else {
          sceneState_.selectedMeshNode = nodeIndex;
        }
        selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
        if (subs_.lightingManager) {
          subs_.lightingManager->selectEditableLight({});
        }
        selectionNavigationAnchor_ = {};
        clearHoveredMeshNode();
        selectedDrawCommands_.clear();
        selectedBimDrawCommands_.clear();
        selectedBimNativePointDrawCommands_.clear();
        selectedBimNativeCurveDrawCommands_.clear();
        if (subs_.guiManager) {
          subs_.guiManager->setSectionPlaneVisualEditable(false);
          subs_.guiManager->setStatusMessage("Focused scene node " +
                                             std::to_string(nodeIndex));
        }
      },
      [this](uint32_t nodeIndex, bool visible) {
        if (sceneGraph_.getNode(nodeIndex) == nullptr) {
          return;
        }
        sceneGraph_.setVisible(nodeIndex, visible);
        clearHoveredMeshNode();
        selectedDrawCommands_.clear();
        selectedBimDrawCommands_.clear();
        refreshSceneObjectData();
        syncSceneProviders();
        if (subs_.guiManager) {
          subs_.guiManager->setStatusMessage(
              std::string(visible ? "Showed" : "Hid") + " scene node " +
              std::to_string(nodeIndex));
        }
      },
      [this](uint32_t nodeIndex, std::optional<uint32_t> parentNode) {
        if (nodeIndex == sceneState_.rootNode ||
            sceneGraph_.getNode(nodeIndex) == nullptr) {
          return;
        }
        if (!sceneGraph_.setParentPreserveWorldTransform(nodeIndex,
                                                          parentNode)) {
          if (subs_.guiManager) {
            subs_.guiManager->setStatusMessage(
                "Failed to reparent scene node " + std::to_string(nodeIndex));
          }
          return;
        }
        selectionNavigationAnchor_ = {};
        clearHoveredMeshNode();
        selectedDrawCommands_.clear();
        selectedBimDrawCommands_.clear();
        if (subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        refreshSceneObjectData();
        syncSceneProviders();
        if (subs_.guiManager) {
          subs_.guiManager->setStatusMessage("Reparented scene node " +
                                             std::to_string(nodeIndex));
        }
      },
      subs_.cameraController ? subs_.cameraController->nodeTransformControls(
                                   sceneState_.selectedMeshNode)
                             : container::ui::TransformControls{},
      [this](uint32_t nodeIndex,
             const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              nodeIndex, sceneState_.rootNode, controls);
        if (nodeIndex == sceneState_.rootNode && subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        refreshSceneObjectData();
      });

  if (auto requested = subs_.guiManager->consumeRenderEngineChange()) {
    requestRenderTechnique(*requested);
  }

  if (auto msaaRequest = subs_.guiManager->consumeMsaaSampleChange()) {
    const RendererMsaaDeviceSupport msaaSupport =
        queryRendererMsaaDeviceSupport(svc_.ctx.deviceWrapper->physicalDevice());
    const VkSampleCountFlagBits requestedSampleCount = clampMsaaSampleCount(
        *msaaRequest, msaaSupport.color, msaaSupport.depth);
    if (requestedSampleCount != msaaSampleCount_) {
      pendingMsaaSampleCount_ = requestedSampleCount;
    }
  }

  if (auto elevationRequest =
          subs_.guiManager->consumeBimElevationViewRequest()) {
    if (subs_.cameraController) {
      subs_.cameraController->setBimElevationView(sceneState_.selectedMeshNode,
                                                  *elevationRequest);
      uploadCameraBuffers();
    }
  }

  if (auto drawingExportRequest =
          subs_.guiManager->consumeBimDrawingExportRequest()) {
    const std::filesystem::path outputPath =
        container::util::pathFromUtf8(drawingExportRequest->path);
    const std::string outputLabel = container::util::pathToUtf8(outputPath);

    BimDrawingExportRequest svgRequest{};
    svgRequest.title = bimDrawingTitle(subs_.bimManager.get());
    const auto &floorPlan = subs_.guiManager->bimFloorPlanOverlayState();
    svgRequest.paperWidthMm = drawingExportRequest->paperWidthMm;
    svgRequest.paperHeightMm = drawingExportRequest->paperHeightMm;
    svgRequest.modelUnitsPerPaperMm =
        drawingExportRequest->modelUnitsPerPaperMm;
    bool sourceElevation =
        floorPlan.elevationMode ==
        container::ui::BimFloorPlanElevationMode::SourceElevation;
    if (subs_.bimManager) {
      svgRequest.lines = subs_.bimManager->floorPlanDrawingExportLines(
          sourceElevation, floorPlan.color, 0.18f);
      if (svgRequest.lines.empty() && sourceElevation) {
        sourceElevation = false;
        svgRequest.lines = subs_.bimManager->floorPlanDrawingExportLines(
            false, floorPlan.color, 0.18f);
      }
    }
    svgRequest.viewName = bimFloorPlanDrawingViewName(sourceElevation);

    const std::string svg = ExportBimDrawingSvg(svgRequest);
    if (outputPath.empty()) {
      subs_.guiManager->setStatusMessage(
          "Failed to export BIM drawing SVG: empty path");
    } else if (svgRequest.lines.empty()) {
      subs_.guiManager->setStatusMessage(
          "Failed to export BIM drawing SVG: no floor plan linework available");
    } else if (svg.empty()) {
      subs_.guiManager->setStatusMessage(
          "Failed to export BIM drawing SVG: invalid paper size or scale");
    } else {
      bool readyToWrite = true;
      if (outputPath.has_parent_path()) {
        std::error_code directoryError;
        std::filesystem::create_directories(outputPath.parent_path(),
                                            directoryError);
        if (directoryError) {
          readyToWrite = false;
          subs_.guiManager->setStatusMessage(
              "Failed to create BIM drawing SVG directory: " +
              container::util::pathToUtf8(outputPath.parent_path()));
        }
      }

      if (readyToWrite) {
        std::ofstream output(outputPath, std::ios::binary);
        if (!output) {
          subs_.guiManager->setStatusMessage(
              "Failed to open BIM drawing SVG: " + outputLabel);
        } else {
          output << svg;
          subs_.guiManager->setStatusMessage(
              output ? "Exported BIM drawing SVG: " + outputLabel
                     : "Failed to write BIM drawing SVG: " + outputLabel);
        }
      }
    }
  }

  subs_.guiManager->drawViewportInteractionControls(
      interactionController_.state(),
      [this](container::ui::ViewportTool tool) {
        interactionController_.setTool(tool);
      },
      [this](container::ui::TransformSpace transformSpace) {
        interactionController_.setTransformSpace(transformSpace);
      },
      [this](container::ui::TransformAxis transformAxis) {
        interactionController_.setTransformAxis(transformAxis);
      },
      [this](container::ui::ViewportNavigationStyle navigationStyle) {
        interactionController_.setNavigationStyle(navigationStyle);
      },
      [this](bool snapEnabled) {
        interactionController_.setTransformSnapEnabled(snapEnabled);
      });

  syncCameraSelectionPivotOverride();
  container::ui::ViewportNavigationState navigationState{};
  if (subs_.cameraController) {
    navigationState.projectionMode =
        subs_.cameraController->isOrthographic()
            ? container::ui::CameraProjectionMode::Orthographic
            : container::ui::CameraProjectionMode::Perspective;
    if (const auto *camera = subs_.cameraController->camera()) {
      navigationState.cameraForward = camera->frontVector();
      navigationState.cameraUp =
          camera->upVector(navigationState.cameraForward);
      navigationState.cameraRight = camera->rightVector(
          navigationState.cameraForward, navigationState.cameraUp);
    }
  }
  subs_.guiManager->drawViewportNavigationOverlay(
      navigationState,
      [this, uploadCameraBuffers](container::ui::CameraViewPreset preset) {
        if (!subs_.cameraController) {
          return;
        }
        subs_.cameraController->setViewPreset(sceneState_.selectedMeshNode,
                                              preset);
        uploadCameraBuffers();
      },
      [this, uploadCameraBuffers](float deltaX, float deltaY) {
        if (!subs_.cameraController) {
          return;
        }
        subs_.cameraController->orbit(sceneState_.selectedMeshNode, deltaX,
                                      deltaY, 1.0f);
        uploadCameraBuffers();
      },
      [this, uploadCameraBuffers](float deltaX, float deltaY) {
        if (!subs_.cameraController) {
          return;
        }
        subs_.cameraController->pan(sceneState_.selectedMeshNode, deltaX,
                                    -deltaY, 2.0f);
        uploadCameraBuffers();
      },
      [this, uploadCameraBuffers]() {
        if (!subs_.cameraController) {
          return;
        }
        subs_.cameraController->toggleProjectionMode(
            sceneState_.selectedMeshNode);
        uploadCameraBuffers();
      });

  // Sync freeze-culling: pull GUI state back into debug state (checkbox may
  // have toggled it).
  const bool guiFreeze = subs_.guiManager->freezeCullingRequested();
  if (guiFreeze != debugState_.freezeCulling) {
    debugState_.freezeCulling = guiFreeze;
    if (!guiFreeze && subs_.gpuCullManager)
      subs_.gpuCullManager->unfreezeCulling();
  }

  if (subs_.temporalManager) {
    subs_.temporalManager->settings() = subs_.guiManager->temporalSettings();
    if (subs_.guiManager->consumeTemporalReset())
      subs_.temporalManager->reset("user requested reset");
  }
  // Sync bloom: pull GUI state back into BloomManager.
  if (subs_.bloomManager) {
    subs_.bloomManager->enabled() = subs_.guiManager->bloomEnabled();
    subs_.bloomManager->threshold() = subs_.guiManager->bloomThreshold();
    subs_.bloomManager->knee() = subs_.guiManager->bloomKnee();
    subs_.bloomManager->intensity() = subs_.guiManager->bloomIntensity();
    subs_.bloomManager->filterRadius() = subs_.guiManager->bloomRadius();
  }

  if (subs_.lightingManager) {
    const auto &guiLightingSettings = subs_.guiManager->lightingSettings();
    const auto &currentLightingSettings =
        subs_.lightingManager->lightingSettings();
    const bool lightingSettingsChanged =
        guiLightingSettings.preset != currentLightingSettings.preset ||
        guiLightingSettings.density != currentLightingSettings.density ||
        guiLightingSettings.radiusScale !=
            currentLightingSettings.radiusScale ||
        guiLightingSettings.intensityScale !=
            currentLightingSettings.intensityScale ||
        guiLightingSettings.directionalIntensity !=
            currentLightingSettings.directionalIntensity ||
        guiLightingSettings.environmentIntensity !=
            currentLightingSettings.environmentIntensity ||
        guiLightingSettings.bounceIntensity !=
            currentLightingSettings.bounceIntensity ||
        guiLightingSettings.localShadowPointBudget !=
            currentLightingSettings.localShadowPointBudget ||
        guiLightingSettings.localShadowLayerBudget !=
            currentLightingSettings.localShadowLayerBudget;
    if (lightingSettingsChanged) {
      subs_.lightingManager->setLightingSettings(guiLightingSettings);
      subs_.lightingManager->updateLightingData();
      updateFrameDescriptorSets();
    }
  }

  if (subs_.guiManager && subs_.environmentManager) {
    subs_.guiManager->setEnvironmentStatus(
        subs_.environmentManager->environmentStatus());
  }

  // Sync render pass toggles: pull GUI toggle states back into the render
  // graph.
  if (subs_.frameRecorder) {
    auto &graph = subs_.frameRecorder->graph();
    if (DebugUiPresenter::applyRenderPassToggles(*subs_.guiManager, graph)) {
      subs_.guiManager->setStatusMessage(
          "Protected passes stay enabled; dependent optional passes are "
          "disabled automatically.");
    }
  }
}

void RendererFrontend::recordCommandBuffer(
    VkCommandBuffer commandBuffer, uint32_t imageIndex,
    const FrameRecordParams *preparedParams) {
  if (!subs_.frameRecorder || !subs_.frameResourceManager) {
    throw std::runtime_error(
        "frameRecorder not initialized or image index out of range");
  }
  if (imageIndex >= subs_.frameResourceManager->frameCount()) {
    throw std::runtime_error(
        "frameRecorder not initialized or image index out of range");
  }
  if (preparedParams != nullptr) {
    subs_.frameRecorder->record(commandBuffer, *preparedParams);
    return;
  }
  auto p = buildFrameRecordParams(imageIndex);
  attachActiveTechniqueLifecycle(p);
  subs_.frameRecorder->record(commandBuffer, p);
}

FrameTransformGizmoState RendererFrontend::buildTransformGizmoState() const {
  FrameTransformGizmoState gizmo{};
  if (subs_.guiManager != nullptr &&
      !subs_.guiManager->editorOverlaysEnabled()) {
    return gizmo;
  }

  const auto &interaction = interactionController_.state();
  gizmo.tool = interaction.tool;
  gizmo.transformSpace = interaction.transformSpace;
  gizmo.activeAxis =
      interaction.transformAxis != container::ui::TransformAxis::Free ||
              interaction.gesture ==
                  container::ui::ViewportGesture::TransformDrag
          ? interaction.transformAxis
          : interaction.hoverTransformAxis;

  if (interaction.tool == container::ui::ViewportTool::Select) {
    return gizmo;
  }

  glm::vec3 boundsMin{0.0f};
  glm::vec3 boundsMax{0.0f};
  bool hasBounds = false;
  bool useLocalAxes = false;
  bool useSectionPlaneAxes = false;
  glm::vec3 sectionPlaneNormal{0.0f, 1.0f, 0.0f};
  const container::scene::SceneNode *selectedNode = nullptr;
  std::optional<EditableLightEntity> selectedEditableLightForGizmo{};
  const bool hasSelectedEditableLightForGizmo =
      subs_.lightingManager &&
      subs_.lightingManager->selectedEditableLight().has_value();

  if (subs_.guiManager != nullptr &&
      subs_.guiManager->sectionPlaneState().enabled &&
      subs_.guiManager->sectionPlaneState().visualPlaneEditable &&
      sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode &&
      !hasSelectedEditableLightForGizmo) {
    const auto &sectionPlane = subs_.guiManager->sectionPlaneState();
    const glm::vec3 origin = sectionPlaneOrigin(sectionPlane);
    const float visualSize = std::max(sectionPlane.visualPlaneSize, 0.1f);
    boundsMin = origin - glm::vec3{visualSize * 0.5f};
    boundsMax = origin + glm::vec3{visualSize * 0.5f};
    sectionPlaneNormal =
        normalizedOr(sectionPlane.normal, {0.0f, 1.0f, 0.0f});
    hasBounds = true;
    useSectionPlaneAxes = true;
    gizmo.transformSpace = container::ui::TransformSpace::Local;
  } else if (sceneState_.selectedMeshNode !=
          container::scene::SceneGraph::kInvalidNode &&
      subs_.sceneController) {
    const auto &objectData = subs_.sceneController->objectData();
    hasBounds = accumulateDrawCommandBounds(selectedDrawCommands_, objectData,
                                            boundsMin, boundsMax);
    selectedNode = sceneGraph_.getNode(sceneState_.selectedMeshNode);
    if (!hasBounds && selectedNode != nullptr) {
      const glm::vec3 origin{selectedNode->worldTransform[3]};
      boundsMin = origin - glm::vec3{0.5f};
      boundsMax = origin + glm::vec3{0.5f};
      hasBounds = true;
    }
    useLocalAxes =
        selectedNode != nullptr &&
        interaction.transformSpace == container::ui::TransformSpace::Local;
  } else if (selectedBimObjectIndex_ != std::numeric_limits<uint32_t>::max() &&
             subs_.bimManager && subs_.bimManager->hasScene()) {
    const auto &objectData = subs_.bimManager->objectData();
    std::vector<DrawCommand> selectedProductDraws;
    const uint32_t selectedObjectIndex = selectedBimObjectIndex_;
    subs_.bimManager->collectDrawCommandsForObject(selectedObjectIndex,
                                                   selectedProductDraws);
    if (!selectedProductDraws.empty()) {
      hasBounds = accumulateDrawCommandBounds(selectedProductDraws, objectData,
                                              boundsMin, boundsMax);
    } else if (selectedBimObjectIndex_ < objectData.size()) {
      includeBoundingSphere(objectData[selectedBimObjectIndex_].boundingSphere,
                            boundsMin, boundsMax, hasBounds);
    }
    // BIM meshes are selectable overlays today; transform drag still operates
    // on scene graph nodes, so world-space axes avoid implying hidden local
    // transform state.
    gizmo.transformSpace = container::ui::TransformSpace::World;
  } else if (subs_.lightingManager) {
    selectedEditableLightForGizmo =
        subs_.lightingManager->selectedEditableLight();
    if (selectedEditableLightForGizmo) {
      const EditableLightEntity &light = *selectedEditableLightForGizmo;
      const float lightRadius =
          light.type == EditableLightType::Area
              ? std::max({light.areaHalfSize.x, light.areaHalfSize.y, 0.25f})
              : std::max(light.range * 0.08f, 0.25f);
      boundsMin = light.position - glm::vec3(lightRadius);
      boundsMax = light.position + glm::vec3(lightRadius);
      hasBounds = true;
      useLocalAxes = light.type == EditableLightType::Area ||
                     light.type == EditableLightType::Spot ||
                     light.type == EditableLightType::Directional;
    }
  }

  if (!hasBounds) {
    return gizmo;
  }

  gizmo.origin = (boundsMin + boundsMax) * 0.5f;
  gizmo.radius = std::max(glm::length(boundsMax - boundsMin) * 0.5f, 0.1f);
  gizmo.scale =
      transformGizmoScale(gizmo.origin, gizmo.radius, buffers_.cameraData);
  if (useSectionPlaneAxes) {
    if (gizmo.tool == container::ui::ViewportTool::Translate) {
      gizmo.axisX = sectionPlaneNormal;
      gizmo.axisY = sectionPlaneNormal;
      gizmo.axisZ = sectionPlaneNormal;
    } else {
      sectionPlaneBasis(sectionPlaneNormal, gizmo.axisX, gizmo.axisY,
                        gizmo.axisZ);
    }
  } else if (useLocalAxes && selectedNode != nullptr) {
    gizmo.axisX = normalizedOr(glm::vec3{selectedNode->worldTransform[0]},
                               {1.0f, 0.0f, 0.0f});
    gizmo.axisY = normalizedOr(glm::vec3{selectedNode->worldTransform[1]},
                               {0.0f, 1.0f, 0.0f});
    gizmo.axisZ = normalizedOr(glm::vec3{selectedNode->worldTransform[2]},
                               {0.0f, 0.0f, 1.0f});
  } else if (selectedEditableLightForGizmo && useLocalAxes) {
    const EditableLightEntity &light = *selectedEditableLightForGizmo;
    gizmo.axisY = normalizedOr(-light.direction, {0.0f, 1.0f, 0.0f});
    gizmo.axisX = normalizedOr(light.tangent, {1.0f, 0.0f, 0.0f});
    gizmo.axisZ = normalizedOr(
        light.bitangent,
        normalizedOr(glm::cross(gizmo.axisX, gizmo.axisY), {0.0f, 0.0f, 1.0f}));
  }
  gizmo.visible = true;
  return gizmo;
}

void RendererFrontend::publishFrameRuntimeResourceBindings(
    uint32_t imageIndex) {
  FrameResourceRegistry *runtime = subs_.frameRuntimeResourceRegistry.get();
  if (runtime == nullptr) {
    return;
  }

  runtime->clearBindings();

  const RenderTechniqueId activeTechnique =
      subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                       : RenderTechniqueId::DeferredRaster;

  auto copyBinding = [runtime](const FrameResourceBinding &binding) {
    switch (binding.kind) {
    case FrameResourceKind::Image:
      runtime->bindImage(binding.key.technique, binding.key.name,
                         binding.frameIndex, binding.image);
      break;
    case FrameResourceKind::Buffer:
      runtime->bindBuffer(binding.key.technique, binding.key.name,
                          binding.frameIndex, binding.buffer);
      break;
    case FrameResourceKind::Framebuffer:
      runtime->bindFramebuffer(binding.key.technique, binding.key.name,
                               binding.frameIndex, binding.framebuffer);
      break;
    case FrameResourceKind::DescriptorSet:
      runtime->bindDescriptorSet(binding.key.technique, binding.key.name,
                                 binding.frameIndex, binding.descriptor);
      break;
    case FrameResourceKind::Sampler:
      runtime->bindSampler(binding.key.technique, binding.key.name,
                           binding.frameIndex, binding.sampler);
      break;
    case FrameResourceKind::External:
      break;
    }
  };

  auto copyManagerBinding = [this, imageIndex,
                             &copyBinding](RenderTechniqueId technique) {
    if (subs_.frameResourceManager == nullptr) {
      return;
    }
    subs_.frameResourceManager->resourceRegistry().forEachBindingForFrame(
        technique, imageIndex,
        [&copyBinding](const FrameResourceBinding &binding) {
          copyBinding(binding);
        });
  };
  copyManagerBinding(RenderTechniqueId::DeferredRaster);
  if (activeTechnique != RenderTechniqueId::DeferredRaster) {
    copyManagerBinding(activeTechnique);
  }
  if (activeTechnique != RenderTechniqueId::ForwardRaster) {
    copyManagerBinding(RenderTechniqueId::ForwardRaster);
  }

  auto bindDescriptorSetForTechnique =
      [runtime, imageIndex](RenderTechniqueId technique, std::string_view name,
                            VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) {
      return;
    }
    runtime->bindDescriptorSet(technique, std::string(name), imageIndex,
                               FrameDescriptorBinding{.descriptorSet = set});
  };
  auto bindSharedDescriptorSet =
      [activeTechnique,
       &bindDescriptorSetForTechnique](std::string_view name,
                                       VkDescriptorSet set) {
        bindDescriptorSetForTechnique(RenderTechniqueId::DeferredRaster, name,
                                      set);
        if (activeTechnique != RenderTechniqueId::DeferredRaster) {
          bindDescriptorSetForTechnique(activeTechnique, name, set);
        }
        if (activeTechnique != RenderTechniqueId::ForwardRaster) {
          bindDescriptorSetForTechnique(RenderTechniqueId::ForwardRaster, name,
                                        set);
        }
      };
  auto bindDeferredDescriptorSet =
      [&bindDescriptorSetForTechnique](std::string_view name,
                                       VkDescriptorSet set) {
        bindDescriptorSetForTechnique(RenderTechniqueId::DeferredRaster, name,
                                      set);
      };

  bindSharedDescriptorSet("scene-descriptor-set",
                    subs_.sceneManager
                        ? subs_.sceneManager->descriptorSet(imageIndex)
                        : VK_NULL_HANDLE);
  bindSharedDescriptorSet(
      "bim-scene-descriptor-set",
      (subs_.bimManager && subs_.bimManager->hasScene() && subs_.sceneManager)
          ? subs_.sceneManager->auxiliaryDescriptorSet(imageIndex)
          : VK_NULL_HANDLE);
  bindSharedDescriptorSet("light-descriptor-set",
                    subs_.lightingManager
                        ? subs_.lightingManager->lightDescriptorSet(imageIndex)
                        : VK_NULL_HANDLE);
  bindDeferredDescriptorSet(
      "tiled-lighting-descriptor-set",
      (subs_.lightingManager && subs_.lightingManager->isTiledLightingReady())
          ? subs_.lightingManager->tiledDescriptorSet()
          : VK_NULL_HANDLE);
  bindSharedDescriptorSet("shadow-descriptor-set",
                    subs_.shadowManager
                        ? subs_.shadowManager->descriptorSet(imageIndex)
                        : VK_NULL_HANDLE);
  bindSharedDescriptorSet(
      "local-shadow-descriptor-set",
      subs_.shadowManager
          ? subs_.shadowManager->localShadowDescriptorSet(imageIndex)
          : VK_NULL_HANDLE);

  if (subs_.frameResourceManager != nullptr &&
      subs_.frameResourceManager->gBufferSampler() != VK_NULL_HANDLE) {
    // Both techniques use point depth sampling for their Hi-Z culling passes.
    runtime->bindSampler(
        RenderTechniqueId::ForwardRaster, "depth-cull-sampler", imageIndex,
        FrameSamplerBinding{.sampler = subs_.frameResourceManager->gBufferSampler()});
    runtime->bindSampler(
        RenderTechniqueId::DeferredRaster, "g-buffer-sampler", imageIndex,
        FrameSamplerBinding{.sampler =
                                subs_.frameResourceManager->gBufferSampler()});
  }

  const VkBuffer cameraBuffer = imageIndex < buffers_.cameras.size()
                                    ? buffers_.cameras[imageIndex].buffer
                                    : VK_NULL_HANDLE;
  auto bindBufferForTechnique =
      [runtime, imageIndex](RenderTechniqueId technique, std::string_view name,
                            const FrameBufferBinding &binding) {
        runtime->bindBuffer(technique, std::string(name), imageIndex, binding);
      };
  auto bindSharedBuffer =
      [activeTechnique,
       &bindBufferForTechnique](std::string_view name,
                                const FrameBufferBinding &binding) {
        bindBufferForTechnique(RenderTechniqueId::DeferredRaster, name,
                               binding);
        if (activeTechnique != RenderTechniqueId::DeferredRaster) {
          bindBufferForTechnique(activeTechnique, name, binding);
        }
        if (activeTechnique != RenderTechniqueId::ForwardRaster) {
          bindBufferForTechnique(RenderTechniqueId::ForwardRaster, name,
                                 binding);
        }
      };
  if (cameraBuffer != VK_NULL_HANDLE) {
    bindSharedBuffer("camera-buffer",
                     FrameBufferBinding{
                         .buffer = cameraBuffer,
                         .size = sizeof(container::gpu::CameraData),
                         .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT});
  }
  const auto objectBuffer = sceneObjectBuffer(imageIndex);
  const size_t objectCapacity = sceneObjectCapacity(imageIndex);
  if (objectBuffer.buffer != VK_NULL_HANDLE && objectCapacity > 0) {
    bindSharedBuffer(
        "scene-object-buffer",
        FrameBufferBinding{.buffer = objectBuffer.buffer,
                           .size = objectCapacity *
                                   sizeof(container::gpu::ObjectData),
                           .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
  }
}

FrameRecordParams
RendererFrontend::buildFrameRecordParams(uint32_t imageIndex) {
  publishFrameRuntimeResourceBindings(imageIndex);
  syncSceneProviders();

  FrameRecordParams p{};
  p.runtime.frameSlot = frame_.currentFrame;
  p.runtime.imageIndex = imageIndex;
  p.runtime.activeTechnique =
      subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                       : RenderTechniqueId::DeferredRaster;
  p.registries.resourceContracts = subs_.frameResourceRegistry.get();
  p.registries.pipelineRecipes = subs_.pipelineRegistry.get();
  p.registries.resourceBindings = subs_.frameRuntimeResourceRegistry.get();
  p.scene.vertexSlice = sceneState_.vertexSlice;
  p.scene.indexSlice = sceneState_.indexSlice;
  p.scene.indexType = sceneState_.indexType;
  p.draws.opaqueDrawCommands = &subs_.sceneController->opaqueDrawCommands();
  p.draws.transparentDrawCommands =
      &subs_.sceneController->transparentDrawCommands();
  p.draws.opaqueSingleSidedDrawCommands =
      &subs_.sceneController->opaqueSingleSidedDrawCommands();
  p.draws.opaqueWindingFlippedDrawCommands =
      &subs_.sceneController->opaqueWindingFlippedDrawCommands();
  p.draws.opaqueDoubleSidedDrawCommands =
      &subs_.sceneController->opaqueDoubleSidedDrawCommands();
  p.draws.transparentSingleSidedDrawCommands =
      &subs_.sceneController->transparentSingleSidedDrawCommands();
  p.draws.transparentWindingFlippedDrawCommands =
      &subs_.sceneController->transparentWindingFlippedDrawCommands();
  p.draws.transparentDoubleSidedDrawCommands =
      &subs_.sceneController->transparentDoubleSidedDrawCommands();
  subs_.sceneController->collectDrawCommandsForNode(hoveredMeshNode_,
                                                    hoveredDrawCommands_);
  p.draws.hoveredDrawCommands = &hoveredDrawCommands_;
  subs_.sceneController->collectDrawCommandsForNode(
      sceneState_.selectedMeshNode, selectedDrawCommands_);
  p.draws.selectedDrawCommands = &selectedDrawCommands_;
  p.scene.objectData = &subs_.sceneController->objectData();
  p.scene.objectDataRevision = subs_.sceneController->objectDataRevision();
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const BimDrawFilter bimFilter = currentBimDrawFilter();
    BimMeshletResidencySettings residencySettings{};
    residencySettings.drawBudgetEnabled = bimFilter.drawBudgetEnabled;
    residencySettings.drawBudgetMaxObjects = bimFilter.drawBudgetMaxObjects;
    residencySettings.selectedObjectIndex = selectedBimObjectIndex_;
    residencySettings.viewportHeightPixels =
        static_cast<float>(std::max(svc_.swapChainManager.extent().height, 1u));
    if (subs_.guiManager != nullptr) {
      const auto &lodUi = subs_.guiManager->bimLodStreamingUiState();
      residencySettings.autoLod = lodUi.autoLod;
      residencySettings.pauseStreaming = lodUi.pauseStreamingRequest;
      residencySettings.keepSelectedResident = true;
      residencySettings.lodBias = lodUi.lodBias;
      residencySettings.screenErrorPixels = lodUi.screenErrorPixels;
      residencySettings.forceResident = false;
    }
    if (captureBimLodBias_)
      residencySettings.lodBias = *captureBimLodBias_;
    subs_.bimManager->updateMeshletResidencySettings(residencySettings);
    // Exact type/storey/material/discipline/phase/status filters still route
    // through the existing GPU metadata IDs. Timeline and discipline-preset
    // filters use phase ranges and class tokens, so they intentionally fall
    // back to the CPU draw filter until the shader mask grows that model.
    const BimDrawFilter gpuVisibilityFilter =
        bimFilter.requiresCpuFiltering() ? BimDrawFilter{} : bimFilter;
    subs_.bimManager->updateVisibilityFilterSettings(gpuVisibilityFilter);
    const auto bimLayers = subs_.guiManager
                               ? subs_.guiManager->bimLayerVisibilityState()
                               : container::ui::BimLayerVisibilityState{};
    BimFrameDrawRoutingInputs bimRoutingInputs = bimFrameDrawRoutingInputs(
        *subs_.bimManager, bimFilter, bimLayers, nullptr);
    BimFrameDrawRoutingPlan bimRouting =
        buildBimFrameDrawRoutingPlan(bimRoutingInputs);
    const BimDrawLists *filteredDraws = nullptr;
    if (bimRouting.cpuFilteredDrawsRequired) {
      filteredDraws = &subs_.bimManager->filteredDrawLists(bimFilter);
      bimRoutingInputs.cpuFilteredDraws = filteredDraws;
      bimRouting = buildBimFrameDrawRoutingPlan(bimRoutingInputs);
    }
    p.bim.scene.vertexSlice = subs_.bimManager->vertexSlice();
    p.bim.scene.indexSlice = subs_.bimManager->indexSlice();
    p.bim.scene.indexType = subs_.bimManager->indexType();
    p.bim.scene.objectData = &subs_.bimManager->objectData();
    p.bim.scene.objectDataRevision = subs_.bimManager->objectDataRevision();
    p.bim.scene.objectBuffer = subs_.bimManager->objectBuffer();
    p.bim.scene.objectBufferSize = subs_.bimManager->objectBufferSize();
    p.bim.semanticColorMode =
        static_cast<uint32_t>(subs_.bimManager->semanticColorMode());
    p.bim.opaqueMeshDrawsUseGpuVisibility =
        bimRouting.meshDrawsUseGpuVisibility;
    p.bim.transparentMeshDrawsUseGpuVisibility =
        bimRouting.transparentMeshDrawsUseGpuVisibility;
    p.bim.nativePrimitiveDrawsUseGpuVisibility =
        bimRouting.nativePrimitiveDrawsUseGpuVisibility;
    p.bim.nativePointDrawsUseGpuVisibility =
        bimRouting.nativePointDrawsUseGpuVisibility;
    p.bim.nativeCurveDrawsUseGpuVisibility =
        bimRouting.nativeCurveDrawsUseGpuVisibility;
    assignFrameDrawLists(p.bim.draws, bimRouting.meshDraws);
    if (bimLayers.pointCloudVisible) {
      if (bimRouting.pointPlaceholderDraws != nullptr) {
        assignFrameDrawLists(p.bim.pointDraws,
                             *bimRouting.pointPlaceholderDraws);
      }
      if (bimRouting.nativePointDraws != nullptr) {
        assignFrameDrawLists(p.bim.nativePointDraws,
                             *bimRouting.nativePointDraws);
      }
      p.bim.primitivePasses.pointCloud.enabled =
          bimRouting.pointPrimitivePassEnabled;
      p.bim.primitivePasses.pointCloud.depthTest = true;
    }
    if (bimLayers.curvesVisible) {
      if (bimRouting.curvePlaceholderDraws != nullptr) {
        assignFrameDrawLists(p.bim.curveDraws,
                             *bimRouting.curvePlaceholderDraws);
      }
      if (bimRouting.nativeCurveDraws != nullptr) {
        assignFrameDrawLists(p.bim.nativeCurveDraws,
                             *bimRouting.nativeCurveDraws);
      }
      p.bim.primitivePasses.curves.enabled =
          bimRouting.curvePrimitivePassEnabled;
      p.bim.primitivePasses.curves.depthTest = true;
    }
    hoveredBimDrawCommands_.clear();
    hoveredBimNativePointDrawCommands_.clear();
    hoveredBimNativeCurveDrawCommands_.clear();
    if (bimObjectVisibleByLayer(hoveredBimObjectIndex_)) {
      subs_.bimManager->collectDrawCommandsForObject(hoveredBimObjectIndex_,
                                                     hoveredBimDrawCommands_);
      if (bimLayers.pointCloudVisible) {
        subs_.bimManager->collectNativePointDrawCommandsForObject(
            hoveredBimObjectIndex_, hoveredBimNativePointDrawCommands_);
      }
      if (bimLayers.curvesVisible) {
        subs_.bimManager->collectNativeCurveDrawCommandsForObject(
            hoveredBimObjectIndex_, hoveredBimNativeCurveDrawCommands_);
      }
    }
    p.bim.draws.hoveredDrawCommands = &hoveredBimDrawCommands_;
    p.bim.nativePointDraws.hoveredDrawCommands =
        &hoveredBimNativePointDrawCommands_;
    p.bim.nativeCurveDraws.hoveredDrawCommands =
        &hoveredBimNativeCurveDrawCommands_;
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (!bimFilter.hideSelection &&
        bimObjectVisibleByLayer(selectedBimObjectIndex_)) {
      subs_.bimManager->collectDrawCommandsForObject(selectedBimObjectIndex_,
                                                     selectedBimDrawCommands_);
      if (bimLayers.pointCloudVisible) {
        subs_.bimManager->collectNativePointDrawCommandsForObject(
            selectedBimObjectIndex_, selectedBimNativePointDrawCommands_);
      }
      if (bimLayers.curvesVisible) {
        subs_.bimManager->collectNativeCurveDrawCommandsForObject(
            selectedBimObjectIndex_, selectedBimNativeCurveDrawCommands_);
      }
    }
    p.bim.draws.selectedDrawCommands = &selectedBimDrawCommands_;
    p.bim.nativePointDraws.selectedDrawCommands =
        &selectedBimNativePointDrawCommands_;
    p.bim.nativeCurveDraws.selectedDrawCommands =
        &selectedBimNativeCurveDrawCommands_;
    if (subs_.guiManager != nullptr) {
      const auto &floorPlan = subs_.guiManager->bimFloorPlanOverlayState();
      p.bim.floorPlanDrawCommands =
          floorPlan.elevationMode ==
                  container::ui::BimFloorPlanElevationMode::SourceElevation
              ? &subs_.bimManager->floorPlanSourceElevationDrawCommands()
              : &subs_.bimManager->floorPlanGroundDrawCommands();
      p.bim.floorPlan.enabled = floorPlan.enabled &&
                                p.bim.floorPlanDrawCommands != nullptr &&
                                !p.bim.floorPlanDrawCommands->empty();
      p.bim.floorPlan.depthTest = floorPlan.depthTest;
      p.bim.floorPlan.color = floorPlan.color;
      p.bim.floorPlan.opacity = floorPlan.opacity;
      p.bim.floorPlan.lineWidth = floorPlan.lineWidth;

      const bool coordinationLayerRequested =
          bimLayers.clashLayerVisible || bimLayers.markupLayerVisible;
      if (coordinationLayerRequested) {
        const auto issuePins = subs_.guiManager->bimIssueOverlayPins();
        const BimCoordinationOverlayResult coordinationOverlay =
            subs_.bimManager->buildCoordinationOverlay(
                {.spacesEnabled = false,
                 .mepXrayEnabled = false,
                 .clashesEnabled = bimLayers.clashLayerVisible,
                 .issuePinsEnabled = bimLayers.markupLayerVisible},
                std::span<const BimCoordinationOverlayClashPair>{}, issuePins);
        if (coordinationOverlay.markers.empty()) {
          subs_.bimManager->clearCoordinationMarkerGeometry();
        } else if (subs_.bimManager->rebuildCoordinationMarkerGeometry(
                coordinationOverlay)) {
          const BimCoordinationMarkerDrawData &markerDrawData =
              subs_.bimManager->coordinationMarkerDrawData();
          p.bim.coordinationMarkerScene.vertexSlice =
              markerDrawData.vertexSlice;
          p.bim.coordinationMarkerScene.indexSlice = markerDrawData.indexSlice;
          p.bim.coordinationMarkerScene.indexType = markerDrawData.indexType;
          p.bim.coordinationMarkerScene.objectData =
              &subs_.bimManager->objectData();
          p.bim.coordinationMarkerScene.objectDataRevision =
              subs_.bimManager->objectDataRevision();
          p.bim.coordinationMarkerScene.objectBuffer =
              subs_.bimManager->objectBuffer();
          p.bim.coordinationMarkerScene.objectBufferSize =
              subs_.bimManager->objectBufferSize();
          p.bim.coordinationIssueMarkerDrawCommands =
              &markerDrawData.issuePinDrawCommands;
          p.bim.coordinationClashMarkerDrawCommands =
              &markerDrawData.clashDrawCommands;
          p.bim.coordinationMarkers.issueMarkersEnabled =
              bimLayers.markupLayerVisible &&
              !markerDrawData.issuePinDrawCommands.empty();
          p.bim.coordinationMarkers.clashMarkersEnabled =
              bimLayers.clashLayerVisible &&
              !markerDrawData.clashDrawCommands.empty();
        }
      } else {
        subs_.bimManager->clearCoordinationMarkerGeometry();
      }

      const auto &elevation = subs_.guiManager->bimElevationViewState();
      const bool hiddenLineElevation =
          elevation.style ==
          container::ui::BimElevationTechnicalStyle::HiddenLine;
      // Hidden-line elevation is a renderer composition mode: camera presets
      // stay in the camera controller, while the frame asks lighting to draw a
      // depth-tested line overlay and lets clipped views reuse section caps.
      p.bim.technicalElevation.enabled = hiddenLineElevation;
      p.bim.technicalElevation.hiddenLineOverlay = hiddenLineElevation;
      p.bim.technicalElevation.depthTestLines =
          hiddenLineElevation && elevation.useDepthTestedLines;
    }
  }
  p.transformGizmo = buildTransformGizmoState();
  p.registries.pipelineHandles =
      resources_.builtPipelines.pipelines.handleRegistry.get();
  p.registries.pipelineLayouts =
      resources_.builtPipelines.layouts.layoutRegistry.get();
  p.debug.debugDirectionalOnly = debugState_.directionalOnly;
  p.debug.debugVisualizePointLightStencil =
      debugState_.visualizePointLightStencil;
  p.debug.debugFreezeCulling = debugState_.freezeCulling;
  p.debug.wireframeRasterModeSupported = svc_.ctx.wireframeRasterModeSupported;
  p.debug.wireframeWideLinesSupported = svc_.ctx.wireframeWideLinesSupported;
  pushConstants_.bindless.sectionPlaneEnabled = 0u;
  pushConstants_.bindless.semanticColorMode = 0u;
  pushConstants_.bindless.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  bool sectionPlaneActive = false;
  glm::vec4 activeSectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  container::gpu::SceneClipState sceneClipState{};
  bool boxClipActive = false;
  std::array<glm::vec4, 6> activeBoxClipPlanes{};
  if (subs_.guiManager != nullptr) {
    const auto &sectionPlane = subs_.guiManager->sectionPlaneState();
    if (sectionPlane.enabled) {
      sectionPlaneActive = true;
      activeSectionPlane = sectionPlaneEquation(sectionPlane);
      pushConstants_.bindless.sectionPlaneEnabled = 1u;
      pushConstants_.bindless.sectionPlane = activeSectionPlane;
    }
    const auto &boxClip = subs_.guiManager->bimBoxClipState();
    if (boxClip.enabled) {
      boxClipActive = true;
      activeBoxClipPlanes = makeBoxClipPlanes(boxClip);
      sceneClipState.boxClipEnabled = 1u;
      sceneClipState.boxClipInvert = boxClip.invert ? 1u : 0u;
      sceneClipState.boxClipPlaneCount =
          static_cast<uint32_t>(activeBoxClipPlanes.size());
      for (size_t i = 0; i < activeBoxClipPlanes.size(); ++i) {
        sceneClipState.boxClipPlanes[i] = activeBoxClipPlanes[i];
      }
    }
  }
  if (captureSectionPlane_) {
    sectionPlaneActive = true;
    activeSectionPlane = *captureSectionPlane_;
    pushConstants_.bindless.sectionPlaneEnabled = 1u;
    pushConstants_.bindless.sectionPlane = activeSectionPlane;
  }
  if (subs_.sceneManager != nullptr) {
    subs_.sceneManager->updateSceneClipState(sceneClipState);
  }
  if (subs_.bimManager != nullptr && subs_.guiManager != nullptr) {
    const auto &sectionPlane = subs_.guiManager->sectionPlaneState();
    const bool sectionPlaneVisualEnabled =
        sectionPlaneActive && sectionPlane.visualPlaneVisible;
    p.bim.sectionPlaneVisual.enabled = sectionPlaneVisualEnabled;
    p.bim.sectionPlaneVisual.depthTest = true;
    p.bim.sectionPlaneVisual.color = sectionPlane.visualPlaneColor;
    p.bim.sectionPlaneVisual.opacity =
        std::clamp(sectionPlane.visualPlaneOpacity, 0.05f, 1.0f);
    p.bim.sectionPlaneVisual.lineWidth =
        std::clamp(sectionPlane.visualPlaneLineWidth, 0.5f, 8.0f);
    if (sectionPlaneVisualEnabled &&
        subs_.bimManager->rebuildSectionPlaneVisualGeometry(
            activeSectionPlane, sectionPlane.visualPlaneSize,
            sectionPlane.visualPlaneColor)) {
      const auto &visualData =
          subs_.bimManager->sectionPlaneVisualDrawData();
      p.bim.sectionPlaneVisualScene.vertexSlice = visualData.vertexSlice;
      p.bim.sectionPlaneVisualScene.indexSlice = visualData.indexSlice;
      p.bim.sectionPlaneVisualScene.indexType = visualData.indexType;
      p.bim.sectionPlaneVisualScene.objectData =
          &subs_.bimManager->objectData();
      p.bim.sectionPlaneVisualScene.objectDataRevision =
          subs_.bimManager->objectDataRevision();
      p.bim.sectionPlaneVisualScene.objectBuffer =
          subs_.bimManager->objectBuffer();
      p.bim.sectionPlaneVisualScene.objectBufferSize =
          subs_.bimManager->objectBufferSize();
      p.bim.sectionPlaneVisualDrawCommands = &visualData.drawCommands;
    } else {
      subs_.bimManager->clearSectionPlaneVisualGeometry();
    }

    const auto &capUi = subs_.guiManager->bimClipCapHatchingUiState();
    const bool technicalCapsEnabled =
        p.bim.technicalElevation.enabled &&
        p.bim.technicalElevation.sectionCapsEnabled;
    const bool capStyleEnabled =
        (sectionPlaneActive || boxClipActive) &&
        (capUi.capPreview || capUi.hatchingPreview ||
         capUi.sectionMarkersPreview || technicalCapsEnabled);
    p.bim.sectionClipCaps.enabled = capStyleEnabled;
    p.bim.sectionClipCaps.fillEnabled =
        capUi.capPreview ||
        (technicalCapsEnabled && p.bim.technicalElevation.capFillEnabled);
    p.bim.sectionClipCaps.hatchEnabled =
        capUi.hatchingPreview ||
        (technicalCapsEnabled && p.bim.technicalElevation.capHatchingEnabled);
    p.bim.sectionClipCaps.hatchMode =
        p.bim.sectionClipCaps.hatchEnabled
            ? FrameSectionClipCapHatchMode::Diagonal
            : FrameSectionClipCapHatchMode::None;
    p.bim.sectionClipCaps.fillColor =
        glm::vec4(capUi.capColor, capUi.capOpacity);
    p.bim.sectionClipCaps.hatchColor = glm::vec4(capUi.hatchColor, 0.95f);
    p.bim.sectionClipCaps.hatchSpacing =
        std::clamp(capUi.hatchSpacing, 0.05f, 5.0f);
    p.bim.sectionClipCaps.hatchLineWidth =
        std::clamp(capUi.hatchLineWidth, 1.0f, 8.0f);
    p.bim.sectionClipCaps.hatchAngleRadians =
        std::clamp(capUi.hatchAngleDegrees, 0.0f, 180.0f) *
        0.017453292519943295f;
    p.bim.sectionClipCaps.boxClip.enabled = boxClipActive;
    p.bim.sectionClipCaps.boxClip.invert = sceneClipState.boxClipInvert != 0u;
    p.bim.sectionClipCaps.boxClip.planeCount = sceneClipState.boxClipPlaneCount;
    p.bim.sectionClipCaps.boxClip.planes = activeBoxClipPlanes;
    const bool invertedBoxClipCapsUnsupported =
        boxClipActive && p.bim.sectionClipCaps.boxClip.invert;
    if (capStyleEnabled && !invertedBoxClipCapsUnsupported) {
      BimSectionCapBuildOptions capOptions{};
      if (sectionPlaneActive) {
        appendBimSectionCapClipPlane(capOptions, activeSectionPlane);
      } else {
        capOptions.sectionPlane = activeSectionPlane;
      }
      if (boxClipActive) {
        for (const glm::vec4& boxClipPlane : activeBoxClipPlanes) {
          appendBimSectionCapClipPlane(capOptions, boxClipPlane);
        }
      }
      capOptions.hatchSpacing = p.bim.sectionClipCaps.hatchSpacing;
      capOptions.hatchAngleRadians = p.bim.sectionClipCaps.hatchAngleRadians;
      capOptions.capOffset = p.bim.sectionClipCaps.capOffset;
      capOptions.fillColor = glm::vec3(capUi.capColor);
      capOptions.fillOpacity = capUi.capOpacity;
      capOptions.hatchColor = glm::vec3(capUi.hatchColor);
      capOptions.sectionMarkersEnabled = capUi.sectionMarkersPreview;
      capOptions.sectionMarkerColor = capUi.sectionMarkerColor;
      capOptions.sectionMarkerLineWidth = capUi.sectionMarkerLineWidth;
      if (capUi.perMaterialCutStyles) {
        capOptions.materialStyles.push_back(BimSectionCapMaterialStyle{
            .materialIndex = capUi.concreteMaterialIndex,
            .fillColor = capUi.capColor,
            .fillOpacity = capUi.capOpacity,
            .hatchSpacing = capUi.concreteHatchSpacing,
            .hatchAngleRadians = p.bim.sectionClipCaps.hatchAngleRadians,
            .hatchColor = capUi.hatchColor,
        });
        if (capUi.glassMaterialIndex != capUi.concreteMaterialIndex) {
          capOptions.materialStyles.push_back(BimSectionCapMaterialStyle{
              .materialIndex = capUi.glassMaterialIndex,
              .fillColor = capUi.capColor,
              .fillOpacity = std::min(capUi.capOpacity, 0.45f),
              .hatchSpacing = capUi.glassHatchSpacing,
              .hatchAngleRadians = p.bim.sectionClipCaps.hatchAngleRadians,
              .hatchColor = capUi.hatchColor,
          });
        }
      }
      if (subs_.bimManager->rebuildSectionClipCapGeometry(capOptions)) {
        const auto &capData = subs_.bimManager->sectionClipCapDrawData();
        p.bim.sectionClipCapGeometry.scene.vertexSlice = capData.vertexSlice;
        p.bim.sectionClipCapGeometry.scene.indexSlice = capData.indexSlice;
        p.bim.sectionClipCapGeometry.scene.indexType = capData.indexType;
        p.bim.sectionClipCapGeometry.fillDrawCommands =
            &capData.fillDrawCommands;
        p.bim.sectionClipCapGeometry.hatchDrawCommands =
            &capData.hatchDrawCommands;
        p.bim.sectionClipCapGeometry.fillDrawStyles = &capData.fillDrawStyles;
        p.bim.sectionClipCapGeometry.hatchDrawStyles =
            &capData.hatchDrawStyles;
        p.bim.sectionClipCapGeometry.sectionMarkerLines =
          &capData.sectionMarkerLines;
      }
    } else {
      subs_.bimManager->clearSectionClipCapGeometry();
    }
  }
  p.pushConstants = pushConstants_.state();
  p.swapchain.swapChainFramebuffers = &svc_.swapChainManager.framebuffers();
  p.scene.diagCubeObjectIndex = sceneState_.diagCubeObjectIndex;
  const auto *activeCamera = subs_.sceneController
                                 ? subs_.sceneController->world().activeCamera()
                                 : nullptr;
  if (activeCamera) {
    p.camera.nearPlane = activeCamera->nearPlane;
    p.camera.farPlane = activeCamera->farPlane;
  } else if (subs_.cameraController) {
    const auto *perspCam =
        dynamic_cast<const container::scene::PerspectiveCamera *>(
            subs_.cameraController->camera());
    if (perspCam) {
      p.camera.nearPlane = perspCam->nearPlane();
      p.camera.farPlane = perspCam->farPlane();
    }
  }
  p.camera.orthographic =
      subs_.cameraController != nullptr &&
      subs_.cameraController->isOrthographic();
  if (subs_.shadowManager) {
    p.shadows.renderPass = resources_.renderPasses.shadow;
    p.shadows.shadowFramebuffers = subs_.shadowManager->framebuffers().data();
    p.shadows.localShadowFramebuffers =
        subs_.shadowManager->localShadowFramebuffers().data();
    p.shadows.shadowData = &subs_.shadowManager->shadowData();
    p.shadows.localShadowData = &subs_.shadowManager->localShadowData();
    p.shadows.shadowSettings = subs_.guiManager
                                   ? subs_.guiManager->shadowSettings()
                                   : container::gpu::ShadowSettings{};
    p.shadows.localShadowLayerCount =
        subs_.shadowManager->localShadowLayerCount();
    p.shadows.shadowManager = subs_.shadowManager.get();
  }
  const auto displayMode = frontendDisplayMode(
      subs_.guiManager.get(), configuredDisplayMode(svc_.config));
  p.debug.temporalForceReactive =
      subs_.guiManager && (subs_.guiManager->showGeometryOverlay() ||
                           subs_.guiManager->showNormalValidation());
  p.debug.displayMode = static_cast<uint32_t>(displayMode);
  const bool shadowAtlasVisible =
      p.runtime.activeTechnique == RenderTechniqueId::ForwardRaster ||
      container::renderer::displayModeRecordsShadowAtlas(displayMode);
  if (!graphPassScheduled(subs_.frameRecorder.get(),
                          RenderPassId::LocalShadowDepth) ||
      !deferredRasterLocalShadowFrameInputsReady(p, shadowAtlasVisible)) {
    p.shadows.localShadowLayerCount = 0u;
  }
  if (subs_.shadowCullManager) {
    if (subs_.shadowManager) {
      subs_.shadowCullManager->updateShadowCullDescriptor(
          imageIndex, subs_.shadowManager->shadowCullUbo(imageIndex).buffer,
          sizeof(container::gpu::ShadowCullData));
    }
    p.shadows.shadowCullManager = subs_.shadowCullManager.get();
    p.shadows.useGpuShadowCull = subs_.shadowCullManager->isReady();
  }
  p.shadows.useShadowSecondaryCommandBuffers =
      svc_.commandBufferManager.secondaryWorkerCount() >=
      container::gpu::kShadowCascadeCount;
  for (uint32_t cascadeIndex = 0;
       cascadeIndex < container::gpu::kShadowCascadeCount; ++cascadeIndex) {
    p.shadows.shadowSecondaryCommandBuffers[cascadeIndex] =
        svc_.commandBufferManager.secondaryBuffer(imageIndex, cascadeIndex, 0);
  }
  p.services.temporalManager = subs_.temporalManager.get();
  p.services.gpuCullManager = subs_.gpuCullManager.get();
  p.services.bimManager = subs_.bimManager.get();
  p.services.bloomManager = subs_.bloomManager.get();
  p.services.telemetry = subs_.rendererTelemetry.get();
  p.services.gpuProfiler = subs_.renderPassGpuProfiler.get();
  p.postProcess.exposureSettings =
      subs_.guiManager ? subs_.guiManager->exposureSettings()
                       : exposureSettingsFromConfig(svc_.config);
  if (captureExposure_) {
    p.postProcess.exposureSettings.mode = container::gpu::kExposureModeManual;
    p.postProcess.exposureSettings.manualExposure = *captureExposure_;
  }
  p.postProcess.renderPass = resources_.renderPasses.postProcess;
  if (subs_.sceneProviderRegistry) {
    p.sceneExtraction = extractProviderSceneFrameInputs(
        *subs_.sceneProviderRegistry);
  }
  const auto objectBuffer = sceneObjectBuffer(imageIndex);
  p.scene.objectBuffer = objectBuffer.buffer;
  p.scene.objectBufferSize =
      sizeof(container::gpu::ObjectData) * sceneObjectCapacity(imageIndex);
  if (screenshot_.pending) {
    p.screenshot.enabled = true;
    p.screenshot.swapChainImage = svc_.swapChainManager.image(imageIndex);
    if (frame_.currentFrame < screenshot_.readbacks.size()) {
      p.screenshot.readbackBuffer =
          screenshot_.readbacks[frame_.currentFrame].readbackBuffer.buffer;
    }
    p.screenshot.extent = svc_.swapChainManager.extent();
  }
  return p;
}

void RendererFrontend::attachActiveTechniqueLifecycle(
    FrameRecordParams &params) {
  if (subs_.deferredRasterFrameGraphContext == nullptr) {
    return;
  }

  switch (params.runtime.activeTechnique) {
  case RenderTechniqueId::DeferredRaster:
  case RenderTechniqueId::ForwardRaster:
    params.lifecycle =
        subs_.deferredRasterFrameGraphContext->lifecycleHooks();
    break;
  default:
    params.lifecycle = {};
    break;
  }
}

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------

void RendererFrontend::syncSceneProviders() {
  if (!subs_.sceneProviderRegistry || !subs_.sceneProviderSynchronizer) {
    return;
  }

  SceneProviderSyncInput syncInput{};

  if (subs_.sceneManager) {
    const auto &sceneManager = *subs_.sceneManager;
    const auto meshBounds =
        sceneProviderBoundsFromModelBounds(sceneManager.modelBounds());
    const std::size_t meshInstanceCount =
        subs_.sceneController ? subs_.sceneController->objectData().size() : 0u;
    const container::scene::MeshSceneAsset meshAsset =
        container::scene::buildMeshSceneProviderAsset({
            .primitiveRanges = sceneManager.primitiveRanges(),
            .materialCount = sceneManager.materialCount(),
            .instanceCount = meshInstanceCount,
            .bounds = meshBounds,
            .materialFactsAt =
                [&sceneManager](uint32_t materialIndex) {
                  const auto properties =
                      sceneManager.materialRenderProperties(materialIndex);
                  return container::scene::MeshSceneProviderMaterialFacts{
                      .transparent = properties.transparent,
                      .doubleSided = properties.doubleSided,
                  };
                },
        });
    const uint64_t objectRevision =
        subs_.sceneController ? subs_.sceneController->objectDataRevision()
                              : 0u;
    syncInput.mesh = MeshSceneProviderSyncInput{
        .available = meshAsset.primitiveCount > 0 || meshAsset.bounds.valid,
        .primitiveCount = meshAsset.primitiveCount,
        .materialCount = meshAsset.materialCount,
        .instanceCount = meshAsset.instanceCount,
        .triangleBatches = meshAsset.triangleBatches,
        .bounds = meshAsset.bounds,
        .geometryRevision = objectRevision,
        .instanceRevision = objectRevision,
        .displayName = sceneProviderDisplayName(activePrimaryModelPath_,
                                                "Primary mesh scene"),
    };
  }

  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const BimSceneStats stats = subs_.bimManager->sceneStats();
    const uint64_t objectRevision = subs_.bimManager->objectDataRevision();
    syncInput.bim = BimSceneProviderSyncInput{
        .available = true,
        .elementCount = stats.objectCount,
        .meshPrimitiveCount =
            stats.opaqueDrawCount + stats.transparentDrawCount,
        .meshOpaqueBatchCount = stats.opaqueDrawCount,
        .meshTransparentBatchCount = stats.transparentDrawCount,
        .nativePointRangeCount = stats.nativePointOpaqueDrawCount +
                                 stats.nativePointTransparentDrawCount,
        .nativeCurveRangeCount = stats.nativeCurveOpaqueDrawCount +
                                 stats.nativeCurveTransparentDrawCount,
        .nativePointOpaqueRangeCount = stats.nativePointOpaqueDrawCount,
        .nativePointTransparentRangeCount =
            stats.nativePointTransparentDrawCount,
        .nativeCurveOpaqueRangeCount = stats.nativeCurveOpaqueDrawCount,
        .nativeCurveTransparentRangeCount =
            stats.nativeCurveTransparentDrawCount,
        .triangleBatches = subs_.bimManager->sceneProviderTriangleBatches(),
        .bounds = sceneProviderBoundsFromBim(*subs_.bimManager),
        .geometryRevision = objectRevision,
        .instanceRevision = objectRevision,
        .displayName =
            sceneProviderDisplayName(activeAuxiliaryModelPath_, "BIM scene"),
    };
  }

  subs_.sceneProviderSynchronizer->sync(*subs_.sceneProviderRegistry,
                                        syncInput);
}

void RendererFrontend::syncSceneStateFromController() {
  if (!subs_.sceneController)
    return;
  sceneState_.vertexSlice = subs_.sceneController->vertexSlice();
  sceneState_.indexSlice = subs_.sceneController->indexSlice();
}

} // namespace container::renderer
