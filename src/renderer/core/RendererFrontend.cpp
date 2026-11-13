#include "Container/renderer/core/RendererFrontend.h"
#include "Container/renderer/core/ComfyUiFrontendState.h"
#if CONTAINER_HAS_JOLT
#include "Container/physics/PhysicsScene.h"
#include "Container/physics/PhysicsElementBindings.h"
#endif
#include "Container/asl/PoseReplayScene.h"
#include "Container/renderer/raytracing/RaySceneAcceleration.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include "Container/renderer/pathtracing/ReferencePathTracePass.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/lighting/DdgiPreset.h"
#include "Container/renderer/lighting/DdgiSceneBounds.h"
#include "Container/renderer/lighting/DdgiVolumeFit.h"
#include "Container/renderer/lighting/DdgiVolumePlacement.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/geometry/OpenVdbVolume.h"
#include "Container/geometry/OpenVdbVolumeSequence.h"
#include "Container/geometry/GltfMerge.h"
#include "Container/geometry/GltfSky.h"
#include "Container/renderer/effects/OpenVdbVolumeRenderer.h"
#include <fstream>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "Container/app/AppConfig.h"
#include "Container/ecs/World.h"
#include "Container/renderer/bim/BimDrawingExport.h"
#include "Container/renderer/bim/BimFrameDrawRoutingPlanner.h"
#include "Container/renderer/bim/BimGeoreferenceTransform.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/core/FrameConcurrencyPolicy.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderExtraction.h"
#include "Container/renderer/core/RenderPassGpuProfiler.h"
#ifdef CONTAINER_ENABLE_RENDERER_MCP
#include "Container/renderer/core/RendererMcpServer.h"
#endif
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/RendererMsaa.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/core/GpuHardwareMonitor.h"
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
#include "Container/renderer/picking/DepthReadback.h"
#include "Container/renderer/pipeline/GraphicsPipelineBuilder.h"
#include "Container/renderer/pipeline/RuntimeShaderCompiler.h"
#include "Container/renderer/pipeline/RayEffectPipelines.h"
#include "Container/renderer/pipeline/PreparedGraphicsPipelines.h"
#include "Container/renderer/debug/DdgiDebugRenderer.h"
#include "Container/common/VulkanObjectCounts.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/resources/CommandBufferManager.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/CameraController.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/ScenePrimitives.h"
#include "Container/renderer/scene/SceneProviderSynchronizer.h"
#include "Container/renderer/scene/MeshSceneProviderBindings.h"
#include "Container/renderer/scene/SceneReloadPublication.h"
#include "Container/renderer/shadow/ShadowCullManager.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/scene/GltfDeformationPass.h"
#include "Container/renderer/scene/GpuHierarchyPass.h"
#include "Container/renderer/raytracing/RayInstanceTransformPass.h"
#include "Container/renderer/scene/PbrAuxiliaryPass.h"
#include "Container/renderer/effects/ScreenReflectionPass.h"
#include "Container/renderer/effects/VolumetricFogPass.h"
#include "Container/renderer/shadow/ShadowPipelineBridge.h"
#include "Container/renderer/shadow/ShadowResourceBridge.h"
#include "Container/scene/MeshSceneProviderAssetAdapter.h"
#include "Container/scene/SceneProvider.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/AsyncModelPreparation.h"
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
#include "Container/utility/TransformScale.h"
#include "stb_image_write.h"
#include "Container/utility/AtomicFile.h"

#include <tinyexr.h>
#include <glm/matrix.hpp>

namespace container::renderer {

using container::gpu::CameraData;
using container::gpu::LightingData;

struct RendererFrontend::ModelLoadState {
  struct Prepared {
    std::optional<container::geometry::GltfLoadResult> gltf;
    std::optional<BimManager::PreparedScene> bim;
  };
  struct MergeDestination {
    uint64_t graphIdentity;
    size_t scene;
  };
  using Worker = container::util::AsyncModelPreparation<std::unique_ptr<Prepared>>;
  Worker worker;
  std::optional<container::ui::ModelLoadRequest> request;
  std::optional<MergeDestination> mergeDestination;
  std::optional<container::ui::ModelLoadRequest> queued;
  bool queuedRemote{false};
  bool remote{false};
  std::unique_ptr<Prepared> ready;
  bool committing{false};
  std::exception_ptr presentationFailure;
  uint64_t requests{0}, completed{0}, cancelled{0}, preparationFrames{0}, progressFrames{0}, eventPumps{0};
  std::string stage;
  std::chrono::steady_clock::time_point lastProgressFrame{};
};

namespace {

nlohmann::json referenceGroundMetadata(const ReferenceGroundSettings& ground) {
  nlohmann::json metadata{{"mode", referenceGroundModeName(ground.mode)}};
  if (ground.mode != ReferenceGroundMode::Off)
    metadata["height"] = ground.height;
  if (ground.mode == ReferenceGroundMode::Surface) {
    metadata["color"] = {ground.color.r, ground.color.g, ground.color.b};
    metadata["metallic"] = ground.metallic;
    metadata["roughness"] = ground.roughness;
  }
  return metadata;
}

nlohmann::json referenceExperimentalMaterialFallbackMetadata(uint32_t fallbacks) {
  return {{"closedVolumeRetroreflection",
           (fallbacks & ReferencePathTraceDiagnostics::kClosedVolumeRetroreflectionFallback) != 0},
          {"closedVolumeScatter", (fallbacks & ReferencePathTraceDiagnostics::kClosedVolumeScatterFallback) != 0}};
}

using TelemetryClock = std::chrono::steady_clock;

std::shared_ptr<const container::geometry::vdb::ImportOptions> vdbOptionsForReload(
    const std::string& path,
    const std::shared_ptr<const container::geometry::vdb::ImportOptions>& explicitOptions,
    const std::string& currentPath,
    const std::shared_ptr<const container::geometry::vdb::ImportOptions>& currentOptions,
    const std::shared_ptr<const container::geometry::vdb::ImportOptions>& configuredOptions) {
  const auto dot=path.find_last_of('.');
  auto extension=dot==std::string::npos ? std::string{} : path.substr(dot);
  std::ranges::transform(extension,extension.begin(),[](unsigned char c){return char(std::tolower(c));});
  if (extension!=".vdb") return {};
  if (explicitOptions) return explicitOptions;
  if (currentOptions && !currentPath.empty()) {
    if (path==currentPath) return currentOptions;
    try {
      const auto resolve=[](std::filesystem::path value) {
        std::error_code error;
        if (!std::filesystem::exists(value,error)) {
          const auto relative=container::util::executableDirectory()/value;
          if (std::filesystem::exists(relative,error)) return relative;
        }
        return value;
      };
      std::error_code error;
      const bool same=std::filesystem::equivalent(resolve(container::util::pathFromUtf8(path)),
          resolve(container::util::pathFromUtf8(currentPath)),error);
      if (!error && same) return currentOptions;
    } catch (const std::filesystem::filesystem_error&) {
      // Keep malformed/inaccessible path reporting inside model preparation.
    }
  }
  return configuredOptions;
}

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
         extension == ".usdz" || extension == ".vdb";
}

glm::vec3 arrayToVec3(const std::array<float, 3> &value) {
  return {value[0], value[1], value[2]};
}

container::gpu::ExposureSettings
exposureSettingsFromConfig(const container::app::AppConfig &config) {
  container::gpu::ExposureSettings settings{};
  if (config.autoExposure) {
    settings.mode = container::gpu::kExposureModeAuto;
  } else if (config.hasManualExposureOverride) {
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
    return config.enableGui || config.upscaling.mode != UpscaleMode::Native ? container::ui::GBufferViewMode::Lit
                            : container::ui::GBufferViewMode::Overview;
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

void requireSwapchainPngSupport(const container::gpu::SwapChainManager& swapchain) {
  if (!swapchain.supportsTransferSrc())
    throw std::runtime_error("swapchain does not support VK_IMAGE_USAGE_TRANSFER_SRC_BIT");
  const auto surface = swapchain.surfaceFormat();
  if (!isSupportedScreenshotFormat(surface.format) ||
      surface.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    throw std::runtime_error("unsupported swapchain screenshot format or color space");
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
    const container::app::AppConfig &config,
    std::optional<float> directionalIntensity) {
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
  if (directionalIntensity)
    lightingData.directionalColorIntensity.a = *directionalIntensity;
}

void applyLightingPresentation(
    container::gpu::LightingData &lighting, const container::app::AppConfig &config,
    std::optional<float> directionalIntensity,
    const EnvironmentManager* environment) {
  applyConfiguredDirectionalLightOverrides(lighting, config, directionalIntensity);
  lighting.environmentSunDirection = glm::vec4(0.0f);
  lighting.environmentSunRadiance = glm::vec4(0.0f);
  lighting.aerialGeometry = glm::vec4(0.0f);
  lighting.aerialDensity = glm::vec4(0.0f);
  lighting.atmosphereLut = {};
  if (!environment || environment->activeSkyMode() != SkyMode::Atmosphere)
    return;

  const auto& sun = environment->activeSun();
  const auto localDirection = arrayToVec3(sun.direction);
  constexpr float radius = 0.0046542113f; // 0.26667 degrees
  constexpr float projectedSolidAngle = 0.0000680528f;
  const auto irradiance = arrayToVec3(sun.irradiance);
  lighting.environmentSunDirection = {localDirection, std::cos(radius)};
  lighting.environmentSunRadiance = {irradiance / projectedSolidAngle, 0.0f};
  const auto& air = environment->activeAtmosphere();
  if (air.multipleScattering && air.aerialMetersPerUnit > 0.0f) {
    lighting.environmentSunRadiance.w = 1.0f;
    lighting.atmosphereLut = environment->activeAtmosphereLut();
  }
  lighting.aerialGeometry = {air.planetRadiusKm, air.atmosphereHeightKm,
                             air.observerHeightKm, air.aerialMetersPerUnit * 0.001f};
  lighting.aerialDensity = {air.rayleighScale, air.mieScale, air.ozoneScale,
                            air.solarIrradiance * lighting.environmentIntensity};
  if (!sun.lightScene)
    return;

  const auto yaw = lighting.environmentOrientation;
  const glm::vec3 worldDirection(yaw.x * localDirection.x + yaw.y * localDirection.z,
                                 localDirection.y,
                                 -yaw.y * localDirection.x + yaw.x * localDirection.z);
  lighting.directionalDirection = {-worldDirection, 0.0f};
  const float peak = std::max({irradiance.r, irradiance.g, irradiance.b});
  const float directionalScale = directionalIntensity.value_or(1.0f);
  lighting.directionalColorIntensity = peak > 0.0f
      ? glm::vec4(irradiance / peak, peak * lighting.environmentIntensity * directionalScale)
      : glm::vec4(0.0f);
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

std::string bimSelectionLabel(const BimElementMetadataView &metadata) {
  std::string label =
      "Selected BIM object " + std::to_string(metadata.objectIndex);
  if (!metadata.type.empty() && metadata.type != "Unknown") {
    label += " (";
    label += metadata.type;
    label += ")";
  }
  if (!metadata.guid.empty()) {
    label += " [";
    label += metadata.guid;
    label += "]";
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
  return bimManager.sceneProviderBounds();
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
  // Start capture object tracks active. kInvalidNode later suspends them after
  // an explicit unload until a capture model-load event reactivates them.
  captureObjectNode_ = 0;
  if (svc_.config.enableGui || svc_.config.gpuHardwareTelemetry) {
    const bool interactive = svc_.config.enableGui && svc_.config.screenshotCapturePath.empty() &&
        svc_.config.temporalCaptureSequencePath.empty() && svc_.config.pathSampleSeries.empty();
    const auto* requested = interactive ? std::getenv("CONTAINER_MONITOR_GPU") : nullptr;
    const bool enabled = svc_.config.gpuHardwareTelemetry || (requested && std::string_view(requested) == "1");
    subs_.gpuHardwareMonitor = std::make_unique<GpuHardwareMonitor>(svc_.ctx.deviceWrapper->physicalDevice(), enabled);
  }
  // Initialization order mirrors runtime dependencies: render passes define
  // attachment compatibility, scene/lighting managers create descriptor
  // layouts, and only then can graphics pipelines be built.
  subs_.renderPassManager =
      std::make_unique<RenderPassManager>(svc_.ctx.deviceWrapper);
  subs_.oitManager = std::make_unique<OitManager>(svc_.ctx.deviceWrapper);
  resources_.gBufferFormats.depthStencil =
      subs_.renderPassManager->findDepthStencilFormat();
  {
    const auto physicalDevice = svc_.ctx.deviceWrapper->physicalDevice();
    const RendererMsaaDeviceSupport msaaSupport =
        queryRendererMsaaDeviceSupport(physicalDevice);
    msaaDepthStencilResolve_ = chooseRendererDepthStencilResolve(
        msaaSupport.depthResolveModes, msaaSupport.stencilResolveModes,
        msaaSupport.independentResolveNone);
    msaaSampleMask_ = queryRendererMsaaSampleMask(
        physicalDevice, resources_.gBufferFormats, msaaSupport);
    const auto supportedSampleCounts =
        supportedMsaaSampleCounts(msaaSampleMask_);
    supportedMsaaSamples_.clear();
    supportedMsaaSamples_.reserve(supportedSampleCounts.size());
    for (VkSampleCountFlagBits sampleCount : supportedSampleCounts) {
      supportedMsaaSamples_.push_back(sampleCountToSamples(sampleCount));
    }
    msaaSampleCount_ =
        clampMsaaSampleCount(svc_.config.msaaSamples, msaaSampleMask_);
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
                                *subs_.sceneManager, svc_.config.vdbModelOptions);
    activeAuxiliaryModelPath_ = svc_.config.bimModelPath;
    activeAuxiliaryImportScale_ = svc_.config.bimImportScale;
  }

  subs_.sceneController = std::make_unique<SceneController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      sceneGraph_, *subs_.sceneManager, nullptr, svc_.config);
  gpuHierarchyEnabled_ = svc_.config.gpuHierarchyTransforms;
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
  std::optional<SkySettings> authoredSky;
  std::string authoredSkyMessage;
  {
    const auto exeDir = container::util::executableDirectory();
    if (svc_.config.useGltfSky) {
      try {
        authoredSky = container::geometry::gltf::readAuthoredSky(
            subs_.sceneManager->animationSource(), subs_.sceneManager->selectedScene());
        if (!authoredSky)
          authoredSkyMessage = "The selected glTF scene has no authored sky.";
        else if (!subs_.environmentManager->loadSkyEnvironment(*authoredSky)) {
          authoredSkyMessage = subs_.environmentManager->environmentStatus();
          authoredSky.reset();
        }
      } catch (const std::exception& error) {
        authoredSkyMessage = error.what();
      }
      if (!authoredSkyMessage.empty())
        std::println(stderr, "[Sky] {}", authoredSkyMessage);
    }
    if (!authoredSky) {
      if (svc_.config.sky.mode != SkyMode::Hdr) {
        if (!subs_.environmentManager->loadSkyEnvironment(svc_.config.sky))
          throw std::runtime_error(subs_.environmentManager->environmentStatus());
      } else if (!svc_.config.environmentPath.empty()) {
        const auto path = container::util::pathFromUtf8(svc_.config.environmentPath);
        if (!subs_.environmentManager->loadHdrEnvironment(path))
          throw std::runtime_error(subs_.environmentManager->environmentStatus());
      } else {
        const auto path = exeDir / container::app::kDefaultEnvironmentHdrRelativePath;
        if (std::filesystem::exists(path))
          subs_.environmentManager->loadHdrEnvironment(path);
      }
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
  if (!svc_.config.vdbVolumePath.empty() || !svc_.config.vdbSequenceDirectory.empty()) {
    const container::geometry::OpenVdbVolumeOptions volumeOptions{
        .gridName = svc_.config.vdbGridName,
        .maxResolution = svc_.config.vdbVolumeResolution,
        .importScale = svc_.config.vdbVolumeImportScale,
        .temperatureGridName = svc_.config.vdbTemperatureGridName,
        .sparse = svc_.config.vdbSparse};
    std::vector<container::geometry::OpenVdbVolume> volumes;
    if (!svc_.config.vdbSequenceDirectory.empty()) {
      auto manifest = container::geometry::discoverOpenVdbVolumeSequence(
          container::util::pathFromUtf8(svc_.config.vdbSequenceDirectory),
          svc_.config.vdbSequenceFps);
      if (svc_.config.vdbSequenceStartFrame >= manifest.frames.size())
        throw std::invalid_argument("--vdb-start-frame exceeds the sequence frame count");
      vdbPlayback_ = std::make_unique<container::geometry::OpenVdbSequencePlayback>(
          manifest, container::geometry::OpenVdbSequencePlaybackOptions{
              .loop = svc_.config.vdbSequenceLoop,
              .paused = svc_.config.vdbSequencePaused,
              .speed = svc_.config.vdbSequenceSpeed});
      vdbPlayback_->seekSeconds(double(svc_.config.vdbSequenceStartFrame) / manifest.framesPerSecond);
      vdbSequence_ = std::make_unique<container::geometry::OpenVdbVolumeSequence>(
          std::move(manifest), volumeOptions,
          container::geometry::OpenVdbSequenceCacheOptions{
              .maxCachedFrames = 2,
              .maxCachedBytes = uint64_t(svc_.config.vdbSequenceCacheMiB) * 1024 * 1024});
      vdbLoadedFrame_ = vdbPlayback_->frameIndex();
      volumes.push_back(vdbSequence_->frame(*vdbLoadedFrame_));
    }
    std::vector<std::string> staticPaths;
    if (!svc_.config.vdbVolumePath.empty()) staticPaths.push_back(svc_.config.vdbVolumePath);
    staticPaths.insert(staticPaths.end(), svc_.config.vdbAdditionalVolumePaths.begin(),
                       svc_.config.vdbAdditionalVolumePaths.end());
    for (const auto& path : staticPaths)
      volumes.push_back(container::geometry::loadOpenVdbVolume(
          container::util::pathFromUtf8(path), volumeOptions));
    subs_.openVdbVolumeRenderer = std::make_unique<OpenVdbVolumeRenderer>(
        svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
        svc_.commandBufferManager.pool());
    subs_.openVdbVolumeRenderer->initialize(
        container::util::executableDirectory(), volumes, svc_.config.vdbVolumeSettings);
    container::log::ContainerLogger::instance().renderer()->info(
        "OpenVDB loaded {} media, {} storage, sequence {} frames",
        volumes.size(), svc_.config.vdbSparse ? "native sparse NanoVDB" : "dense preview",
        vdbSequence_ ? vdbSequence_->manifest().frames.size() : 0);
    if (svc_.config.vdbSequenceAsync && vdbSequence_) {
      // Release startup copies before the worker starts reserving CPU payloads.
      volumes.clear();
      vdbAsyncSequence_ = std::make_unique<container::geometry::OpenVdbAsyncVolumeSequence>(
          std::move(vdbSequence_));
    }
  }
  // Seed the same lighting settings that live controls edit.
  sceneLightingOverrides_ = {
      .directionalIntensity = svc_.config.hasDirectionalIntensityOverride,
      .environmentIntensity = svc_.config.hasEnvironmentIntensityOverride,
      .bloomEnabled = svc_.config.hasBloomEnabledOverride,
      .bounceIntensity = svc_.config.hasBounceIntensityOverride};
  auto initialLighting = subs_.lightingManager->lightingSettings();
  if (sceneLightingOverrides_.directionalIntensity)
    initialLighting.directionalIntensity = svc_.config.directionalIntensity;
  if (sceneLightingOverrides_.environmentIntensity)
    initialLighting.environmentIntensity = svc_.config.environmentIntensity;
  if (sceneLightingOverrides_.bounceIntensity)
    initialLighting.bounceIntensity = svc_.config.bounceIntensity;
  initialLighting.areaLightingMode = svc_.config.areaLightingMode;
  initialLighting.areaLightSampleCount = svc_.config.areaLightSampleCount;
  initialLighting.areaEmitterDebug = svc_.config.areaEmitterDebug;
  initialLighting.environment = svc_.config.environmentPresentation;
  subs_.lightingManager->setLightingSettings(initialLighting);
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
  subs_.rayShadowManager = std::make_unique<RayShadowManager>(
      svc_.ctx.deviceWrapper, *svc_.allocationManager.memoryManager());
  subs_.rayShadowManager->settings() = svc_.config.rayShadows;
  subs_.rayShadowManager->createPipelines(
      container::util::executableDirectory(),
      subs_.sceneManager->descriptorSetLayout(),
      subs_.lightingManager->lightDescriptorSetLayout());
  subs_.ddgiManager = std::make_unique<DdgiManager>(
      svc_.ctx.deviceWrapper, *svc_.allocationManager.memoryManager(),
      *subs_.rayShadowManager, svc_.config.ddgi);
  subs_.ddgiManager->createPipelines(
      container::util::executableDirectory(),
      subs_.sceneManager->descriptorSetLayout(),
      subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.frameResourceManager->postProcessLayout(),
      svc_.swapChainManager.imageFormat());
  subs_.lightingManager->createTiledResources(
      container::util::executableDirectory(), svc_.swapChainManager.extent());
  createGraphicsPipelines();
  svc_.swapChainManager.createFramebuffers(resources_.renderPasses.postProcess);

  createCamera();

  if (svc_.config.enableGui) {
    subs_.guiManager = std::make_unique<container::ui::GuiManager>();
    subs_.guiManager->setGpuHardwareMonitor(subs_.gpuHardwareMonitor.get());
    subs_.guiManager->setLinkedNodeAuthoringQueries(
        [this](uint32_t node) {
          return subs_.sceneManager && subs_.sceneManager->isLinkedNodeReadOnly(sceneGraph_, node);
        },
        [this](uint32_t node) {
          return subs_.sceneManager && subs_.sceneManager->canMakeLinkedAssetEditable(sceneGraph_, node);
        });
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
        svc_.ctx.wireframeWideLinesSupported,
        svc_.ctx.deviceWrapper->enabledFeatures().wideLines == VK_TRUE);
    subs_.guiManager->setAreaShadowQuality(svc_.config.areaShadowQuality);
    subs_.guiManager->setLtcStatus(subs_.lightingManager->ltcStatus());
    subs_.guiManager->rayShadowSettings() = svc_.config.rayShadows;
    subs_.guiManager->setRayShadowSupport(
        subs_.rayShadowManager->supported(),
        subs_.rayShadowManager->supported()
            ? "Supported"
            : (svc_.ctx.deviceWrapper->rayQueriesEnabled()
                   ? std::string(svc_.ctx.deviceWrapper->rayQuerySupport()
                                     .unavailableReason())
                   : "Disabled or unavailable; raster fallback"));
    subs_.guiManager->setGBufferViewMode(configuredDisplayMode(svc_.config));
    subs_.guiManager->setUpscaling(svc_.config.upscaling);
    subs_.guiManager->setDisplayGrade(svc_.config.displayGrade);
    subs_.guiManager->setGpuHierarchyTransforms(gpuHierarchyEnabled_);
    subs_.guiManager->setReferencePathLens(svc_.config.pathLens);
    subs_.guiManager->setReferenceFireflyClamp(svc_.config.pathFireflyClamp);
    subs_.guiManager->setReferencePathDisplay(svc_.config.pathDisplay);
    subs_.guiManager->setReferenceGround(svc_.config.pathGround);
    subs_.guiManager->setReferenceOidn(svc_.config.pathOidn);
    if (svc_.config.autoExposure || svc_.config.hasManualExposureOverride)
      subs_.guiManager->setExposureSettings(exposureSettingsFromConfig(svc_.config));
    if (authoredSky)
      subs_.guiManager->setSkyDraft(*authoredSky);
    else if (svc_.config.sky.mode != SkyMode::Hdr)
      subs_.guiManager->setSkyDraft(svc_.config.sky);
    if (!authoredSkyMessage.empty())
      subs_.guiManager->setStatusMessage(authoredSkyMessage);
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
  const bool queries = svc_.ctx.deviceWrapper->rayQueriesEnabled();
  subs_.deviceCapabilities.bufferDeviceAddress = queries;
  subs_.deviceCapabilities.accelerationStructure = queries;
  subs_.deviceCapabilities.rayQuery = queries;
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
  subs_.frameSyncManager->initialize();
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
  initializeComfyUi();
#ifdef CONTAINER_ENABLE_RENDERER_MCP
  if (svc_.config.screenshotCapturePath.empty() &&
      svc_.config.temporalCaptureSequencePath.empty() && svc_.config.pathSampleSeries.empty()) {
    try {
      subs_.mcpServer = RendererMcpServer::startFromEnvironment();
      if (subs_.mcpServer)
        spdlog::info("Renderer MCP endpoint listening on http://127.0.0.1:{}/mcp", subs_.mcpServer->port());
    } catch (const std::exception& error) {
      spdlog::warn("Renderer MCP endpoint disabled: {}", error.what());
    }
  }
#endif
  if (svc_.config.ddgiFitScene)
    fitDdgiToActiveScene(svc_.config.ddgiFit.maxProbes);
  if (!svc_.config.ddgiDetailNode.empty())
    fitDdgiDetailToSelection(svc_.config.ddgiDetailProbeBudget, svc_.config.ddgiDetailNode);
  if (!svc_.config.ddgiSavePresetPath.empty())
    saveDdgiPreset(container::util::pathFromUtf8(svc_.config.ddgiSavePresetPath),
                   {ddgiActiveSceneIdentity(), subs_.ddgiManager->settings()});
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
  if (subs_.referencePathTracePass)
    subs_.referencePathTracePass->invalidateHistory();
  referenceAdaptiveBatch_.reset();
  referenceTimedLightingMs_.reset();
  referenceStatus_.clear();
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

void RendererFrontend::processEnvironmentLoad(bool wait) {
  auto* environment = subs_.environmentManager.get();
  if (!environment)
    return;
  if (environment->pollEnvironment(wait, [this] {
        // Frames submitted after the environment bake can still sample the
        // previous maps. Retire those readers before replacing their views.
        if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS)
          throw std::runtime_error("Failed to retire environment readers");
        if (subs_.ddgiManager)
          subs_.ddgiManager->resetSceneHistory();
        if (subs_.referencePathTracePass)
          subs_.referencePathTracePass->invalidateHistory();
      })) {
    if (subs_.ddgiManager)
      subs_.ddgiManager->replaceEnvironment(environment->prefilteredView(), environment->envSampler());
    if (subs_.lightingManager)
      subs_.lightingManager->updateLightingDataForActiveCamera();
    updateFrameDescriptorSets();
    if (subs_.temporalManager)
      subs_.temporalManager->reset("environment changed");
  }
  if (subs_.guiManager) {
    subs_.guiManager->setEnvironmentStatus(environment->environmentStatus());
    subs_.guiManager->setEnvironmentLoading(environment->environmentLoading());
  }
}

#ifdef CONTAINER_ENABLE_RENDERER_MCP
void RendererFrontend::processMcpRequests() {
  if (!subs_.mcpServer) return;
  subs_.mcpServer->pump([this](std::string_view tool, const nlohmann::json& args) -> nlohmann::json {
    using Json = nlohmann::json;
    const auto lighting = [this] {
      const auto& current = subs_.lightingManager->lightingSettings();
      return Json{{"environmentIntensity", current.environmentIntensity},
                  {"directionalIntensity", current.directionalIntensity},
                  {"bounceIntensity", current.bounceIntensity},
                  {"environmentYaw", current.environment.yawDegrees}};
    };
    if (tool == "renderer_get_settings") {
      return {{"lighting", lighting()},
              {"technique", subs_.activeTechnique ? renderTechniqueName(subs_.activeTechnique->id()) : ""}};
    }
    if (tool == "renderer_get_status") {
      return {{"activeModel", activePrimaryModelPath_.empty() ? activeAuxiliaryModelPath_ : activePrimaryModelPath_},
              {"modelLoading", modelLoad_ && (modelLoad_->request || modelLoad_->queued)},
              {"modelStage", modelLoad_ ? modelLoad_->stage : ""},
              {"modelStatus", mcpModelStatus_},
              {"environmentLoading", subs_.environmentManager->environmentLoading()},
              {"environmentStatus", subs_.environmentManager->environmentStatus()},
              {"shaderReloadBusy", shaderReloadJob_.valid()}, {"shaderReloadStatus", shaderReloadStatus_},
              {"shaderReloadGeneration", shaderReloadGeneration_},
              {"capturePending", screenshot_.pending}, {"captureStatus", mcpCaptureStatus_}};
    }
    if (tool == "renderer_set_lighting") {
      if (args.empty()) throw std::invalid_argument("Supply at least one lighting control");
      const auto& current = subs_.lightingManager->lightingSettings();
      auto settings = current;
      settings.environmentIntensity = args.value("environmentIntensity", current.environmentIntensity);
      settings.directionalIntensity = args.value("directionalIntensity", current.directionalIntensity);
      settings.bounceIntensity = args.value("bounceIntensity", current.bounceIntensity);
      settings.environment.yawDegrees = args.value("environmentYaw", current.environment.yawDegrees);
      const bool directionalBecameExplicit = args.contains("directionalIntensity") &&
          !sceneLightingOverrides_.directionalIntensity;
      sceneLightingOverrides_.directionalIntensity |= args.contains("directionalIntensity");
      sceneLightingOverrides_.environmentIntensity |= args.contains("environmentIntensity");
      sceneLightingOverrides_.bounceIntensity |= args.contains("bounceIntensity");
      const bool yawChanged = settings.environment.yawDegrees != current.environment.yawDegrees;
      if (directionalBecameExplicit || yawChanged ||
          settings.environmentIntensity != current.environmentIntensity ||
          settings.directionalIntensity != current.directionalIntensity ||
          settings.bounceIntensity != current.bounceIntensity) {
        if (yawChanged && subs_.ddgiManager) subs_.ddgiManager->resetSceneHistory();
        if (subs_.temporalManager) subs_.temporalManager->reset("lighting settings changed");
        subs_.lightingManager->setLightingSettings(settings);
        if (subs_.guiManager) subs_.guiManager->setLightingSettings(settings);
        subs_.lightingManager->updateLightingData();
        updateFrameDescriptorSets();
      }
      return {{"lighting", lighting()}};
    }
    if (tool == "renderer_load_model") {
      if (!modelLoad_) modelLoad_ = std::make_unique<ModelLoadState>();
      if (modelLoad_->request || modelLoad_->queued)
        throw std::runtime_error("A model import is already in progress");
      modelLoad_->queued = container::ui::ModelLoadRequest{
          .path = args.at("path").get<std::string>(), .importScale = args.value("importScale", 1.0f)};
      modelLoad_->queuedRemote = true;
      mcpModelStatus_ = "Queued";
      return {{"queued", true}};
    }
    if (tool == "renderer_load_environment") {
      if (subs_.environmentManager->environmentLoading())
        throw std::runtime_error("An environment import is already in progress");
      if (!subs_.environmentManager->requestHdrEnvironment(
              container::util::pathFromUtf8(args.at("path").get_ref<const std::string&>())))
        throw std::runtime_error(subs_.environmentManager->environmentStatus());
      return {{"queued", true}};
    }
    if (tool == "renderer_reload_shaders") {
      if (shaderReloadJob_.valid()) throw std::runtime_error("Shader reload is already in progress");
      processShaderReload(true);
      if (shaderReloadStatus_.starts_with("Shader reload failed")) throw std::runtime_error(shaderReloadStatus_);
      return {{"queued", true}};
    }
    if (tool == "renderer_capture") {
      if (screenshot_.pending) throw std::runtime_error("An image capture is already pending");
      requestScreenshot(container::util::pathFromUtf8(args.at("path").get_ref<const std::string&>()),
                        args.value("viewportOnly", false));
      screenshot_.remote = true;
      mcpCaptureStatus_ = "Queued";
      return {{"queued", true}};
    }
    throw std::invalid_argument("Unknown renderer MCP tool");
  });
}
#endif

bool RendererFrontend::drawFrame(bool &framebufferResized) {
  // Commit outside the ImGui frame and before acquiring a swapchain image:
  // cooperative loading frames must own their acquire/submit/present cycle.
  processPendingGuiModelLoadRequest();
  processShaderReload(subs_.guiManager && subs_.guiManager->consumeShaderReloadRequest());
  if (subs_.guiManager && subs_.environmentManager) {
    const auto path = subs_.guiManager->consumeEnvironmentRequest();
    if (!path.empty())
      subs_.environmentManager->requestHdrEnvironment(container::util::pathFromUtf8(path));
  }
  if (modelLoad_ && modelLoad_->request && !modelLoad_->committing)
    ++modelLoad_->preparationFrames;
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
  processEnvironmentLoad();
  if (subs_.guiManager && subs_.environmentManager) {
    auto sky = subs_.guiManager->consumeSkyRequest();
    if (subs_.guiManager->consumeAuthoredSkyRequest() && subs_.sceneManager) {
      try {
        sky = container::geometry::gltf::readAuthoredSky(
            subs_.sceneManager->animationSource(), subs_.sceneManager->selectedScene());
        if (sky)
          subs_.guiManager->setSkyDraft(*sky);
        else
          subs_.guiManager->setStatusMessage("The selected glTF scene has no authored sky.");
      } catch (const std::exception& error) {
        sky.reset();
        subs_.guiManager->setStatusMessage(error.what());
      }
    }
    if (sky) {
      subs_.environmentManager->requestSkyEnvironment(*sky);
      subs_.guiManager->setEnvironmentStatus(subs_.environmentManager->environmentStatus());
      subs_.guiManager->setEnvironmentLoading(subs_.environmentManager->environmentLoading());
    }
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::WaitForFrame,
                           elapsedMilliseconds(phaseStart));
    if (subs_.renderPassGpuProfiler) {
      const auto gpuTimings = subs_.renderPassGpuProfiler->collectLatest();
#ifdef CONTAINER_ENABLE_RENDERER_MCP
      if (subs_.mcpServer)
        subs_.mcpServer->observeGpuTimings(subs_.renderPassGpuProfiler->latestRecordingSerial(),
            rendererGpuTimingSourceName(subs_.renderPassGpuProfiler->timingSource()), gpuTimings);
#endif
      if (svc_.config.pathTargetMs > 0.0f) {
        const auto lighting = std::ranges::find(gpuTimings, RenderPassId::Lighting, &RendererPassGpuTiming::id);
        updateReferenceSampleBudget(lighting == gpuTimings.end() ? 0.0f : lighting->milliseconds);
      }
      telemetry->setPassGpuTimings(gpuTimings,
                                   subs_.renderPassGpuProfiler->timingSource());
      telemetry->setGpuProfilerStatus(RendererGpuProfilerTelemetry{
          .source = subs_.renderPassGpuProfiler->timingSource(),
          .available = subs_.renderPassGpuProfiler->isReady(),
          .resultLatencyFrames =
              subs_.renderPassGpuProfiler->resultLatencyFrames(),
          .recordingSerial = subs_.renderPassGpuProfiler->latestRecordingSerial(),
          .status = std::string(subs_.renderPassGpuProfiler->backendStatus()),
      });
    }
  }

#ifdef CONTAINER_ENABLE_RENDERER_MCP
  processMcpRequests();
#endif
  processComfyUi();
  applyPendingRenderTechniqueChange();
  updateOpenVdbSequence();

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
          : svc_.swapChainManager.acquire(
                UINT64_MAX, frame_.currentFrame,
                subs_.frameSyncManager->imageAvailable(frame_.currentFrame), imageIndex);
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
  if (const auto imageFence = frame_.imagesInFlight[imageIndex];
      imageFence != VK_NULL_HANDLE) {
    const VkResult waitResult = vkWaitForFences(
        svc_.ctx.deviceWrapper->device(), 1, &imageFence, VK_TRUE, UINT64_MAX);
    if (waitResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to wait for the swap chain image fence: " +
                               std::to_string(waitResult));
    }
  }
  if (subs_.ddgiManager)
    subs_.ddgiManager->retireImage(imageIndex);
  collectReferenceDiagnostics(imageIndex);
  ensureReferencePathResources();
  if (!svc_.config.pathSampleSeries.empty() && !referencePathActive())
    throw std::runtime_error("Reference sample series requires an active path tracer. " + referenceStatus_);
  if (subs_.guiManager) {
    std::string status = referenceStatus_;
    if (subs_.referencePathTracePass) {
      if (!status.empty()) status += "; ";
      status += std::to_string(subs_.referencePathTracePass->attemptedSamples()) + " samples";
      if (referenceUnsupported_) status += "; unsupported paths (magenta)";
      if (referenceRejected_) status += "; rejected paths (yellow)";
      if (referenceExperimentalMaterialFallbacks_) status += ". Closed-volume draft layers use ordinary volume transport";
      const bool optix = subs_.referencePathTracePass->optixSelected();
      const bool beauty = referencePathDisplay() == ReferencePathDisplayMode::Beauty;
      if (beauty && (optix || subs_.referencePathTracePass->oidnQuality() != ReferenceOidnQuality::Off)) {
        const auto denoiser = subs_.referencePathTracePass->oidnStatus();
        status += optix ? "; OptiX " : "; OIDN ";
        status += denoiser.displayedSamples ? "at " + std::to_string(denoiser.displayedSamples) + " samples"
                                            : std::string(denoiser.message);
      }
      if (beauty && subs_.referencePathTracePass->dlssQuality() != UpscaleMode::Native) {
        status += " | DLSS Ray Reconstruction ";
        status += subs_.referencePathTracePass->dlssStatus().message;
      }
    }
    subs_.guiManager->setReferencePathStatus(std::move(status));
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::ImageFenceWait,
                           elapsedMilliseconds(phaseStart));
  }

  if (subs_.exposureManager) {
    subs_.exposureManager->collectReadback(imageIndex, effectiveExposureSettings());
  }

  phaseStart = TelemetryClock::now();
  growExactOitNodePoolIfNeeded(imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  updateGltfSceneState();
  updateMaterialVariantState();
  presentSceneControls();
  updateGltfAnimation();
  processCaptureLightEvent();
  advancePhysics();
  updateGltfDeformation();
  processPendingDdgiSceneFit();
  processPendingDdgiDetailFit();
  processPendingDdgiPreset();
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Gui,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  applyBimSemanticColorMode();
  auto &temporal = *subs_.temporalManager;
  // Section coverage and shadow routing changes invalidate the history domain;
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
  if (subs_.rayShadowManager) {
    const auto &ray = subs_.guiManager ? subs_.guiManager->rayShadowSettings()
                                      : subs_.rayShadowManager->settings();
    hashClip(ray.mode);
    hashClip(ray.areaSamples);
    hashClip(ray.localLightBudget);
    hashClip(ray.denoise);
    hashClip(ray.debugLayer);
  }
  if (temporalClipRevision_ && *temporalClipRevision_ != clipRevision)
    temporal.reset("section coverage or ray shadow controls changed");
  temporalClipRevision_ = clipRevision;
  container::temporal::validateSettings(
      temporal.settings(), static_cast<uint32_t>(msaaSampleCount_));
  updateCameraBuffer(imageIndex);
  reconcileUpscaleResources();
  const auto temporalExtent = sceneRenderExtent();
  if ((temporal.settings().enabled || temporal.upscalingActive()) && !referencePathActive()) {
    temporal.createPipelines(activeShaderReload_ ? activeShaderReload_->directory
                                                 : container::util::executableDirectory(),
                             subs_.sceneManager->descriptorSetLayout(),
                             resources_.gBufferFormats.depthStencil);
    temporal.prepare(*subs_.frameResourceManager, temporalExtent,
                     static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  if (svc_.config.taaResetFrame != 0 &&
      frame_.submittedFrameCount + 1 == svc_.config.taaResetFrame)
    temporal.reset("configured capture reset");
  const auto displayExtent = svc_.swapChainManager.extent();
  const auto *temporalCamera = subs_.cameraController->camera();
  auto frameTemporalSettings = temporal.settings();
  const auto mode = static_cast<uint32_t>(frontendDisplayMode(
      subs_.guiManager.get(), configuredDisplayMode(svc_.config)));
  frameTemporalSettings.enabled =
      (frameTemporalSettings.enabled || temporal.upscalingActive()) && !referencePathActive() &&
      (mode == 0u || mode == 8u || (mode >= 100u && mode <= 104u));
  temporal.prepareCamera(
      buffers_.cameraData, {temporalExtent.width, temporalExtent.height},
      frameTemporalSettings,
      gltfAnimationCamera_ ? gltfAnimationCamera_->projection :
      temporalCamera->projectionMatrix(float(displayExtent.width) /
                                       float(displayExtent.height)), temporalFrameMilliseconds_);
  SceneController::writeToBuffer(
      svc_.allocationManager, buffers_.cameras[imageIndex],
      &buffers_.cameraData, sizeof(buffers_.cameraData));
  prepareGltfDeformationFrame(imageIndex);
  cpuObjectUploadBytes_=0;
  if(subs_.guiManager) gpuHierarchyEnabled_=subs_.guiManager->gpuHierarchyTransforms();
  gpuObjectUploadDeferred_=gpuHierarchyEnabled_;
  if (updateObjectBuffer(imageIndex, gpuObjectUploadDeferred_))
    publishSceneObjectDescriptors();
  prepareGpuHierarchyFrame(imageIndex);
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
    uploadPreparedObjectData(imageIndex,objects);
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

  if(!frameTemporalSettings.enabled && gpuHierarchyPrepared_)
    uploadPreparedObjectData(imageIndex,subs_.sceneController->objectData());
  if (subs_.lightingManager && subs_.cameraController) {
    subs_.lightingManager->updateLightingDataForActiveCamera();
    applyLightingOverrides();
  }

  if (subs_.shadowManager && subs_.lightingManager && subs_.cameraController) {
    const auto &ld = subs_.lightingManager->lightingData();
    const auto &currentLightingSettings =
        subs_.lightingManager->lightingSettings();
    const float aspect =
        static_cast<float>(svc_.swapChainManager.extent().width) /
        static_cast<float>(svc_.swapChainManager.extent().height);
    container::gpu::ShadowSettings shadowSettings =
        subs_.guiManager ? subs_.guiManager->shadowSettings()
                         : container::gpu::ShadowSettings{};
    if (!subs_.guiManager)
      shadowSettings.areaShadowQuality = svc_.config.areaShadowQuality;
    const std::optional<ShadowCasterSceneBounds> shadowCasterBounds =
        accumulateShadowCasterSceneBounds(subs_.sceneController.get(),
                                          subs_.bimManager.get());
    if (gltfAnimationCamera_) {
      const auto& camera = *gltfAnimationCamera_;
      subs_.shadowManager->update(
          camera.view, camera.shadowProjection, camera.nearPlane, camera.farPlane,
          glm::vec3(ld.directionalDirection), shadowSettings,
          shadowCasterBounds ? &*shadowCasterBounds : nullptr, imageIndex);
    } else {
      subs_.shadowManager->update(subs_.cameraController->camera(), aspect,
                                  glm::vec3(ld.directionalDirection),
                                  shadowSettings,
                                  shadowCasterBounds ? &*shadowCasterBounds
                                                     : nullptr,
                                  imageIndex);
    }
    subs_.shadowManager->updateLocalShadows(
        subs_.lightingManager->pointLightsSsbo(),
        subs_.lightingManager->areaLightsSsbo(), shadowSettings,
        currentLightingSettings.localShadowLayerBudget, imageIndex);
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::SceneUpdate,
                           elapsedMilliseconds(phaseStart));
  }

  if (!screenshot_.pending && svc_.config.pathSampleSeries.empty() && subs_.guiManager) {
    const auto request = subs_.guiManager->consumeImageExportRequest();
    if (!request.path.empty()) {
      try {
        requestScreenshot(container::util::pathFromUtf8(request.path), request.viewportOnly);
        screenshot_.interactive = true;
      } catch (const std::exception& error) {
        subs_.guiManager->setStatusMessage(error.what());
      }
    }
  }
  bool screenshotThisFrame = screenshot_.pending;
  if (screenshotThisFrame) {
    phaseStart = TelemetryClock::now();
    try {
      if (!screenshot_.sceneLinear)
        requireSwapchainPngSupport(svc_.swapChainManager);
      ensureScreenshotReadbackBuffer(frame_.currentFrame, svc_.swapChainManager.extent(),
          screenshot_.sceneLinear ? VK_FORMAT_R32G32B32A32_SFLOAT
                                  : svc_.swapChainManager.imageFormat());
    } catch (const std::exception& error) {
      const bool comfyCapture = finishComfyUiCapture(error.what());
      if (!screenshot_.interactive && !screenshot_.remote && !comfyCapture)
        throw;
      screenshot_.pending = screenshotThisFrame = false;
      if (screenshot_.remote) mcpCaptureStatus_ = std::string("Capture failed: ") + error.what();
      if (screenshot_.interactive) subs_.guiManager->setStatusMessage(error.what());
    }
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  }

  bool pickIdWritten = false;
  FrameRecordParams frameRecordParams = buildFrameRecordParams(imageIndex);
  frameRecordParams.pickIdWritten = &pickIdWritten;
  attachActiveTechniqueLifecycle(frameRecordParams);

  phaseStart = TelemetryClock::now();
  updateFrameDescriptorSets(imageIndex, &frameRecordParams);
  if (frameRecordParams.screenshot.sceneLinear)
    subs_.frameResourceManager->setLinearCaptureTarget(
        imageIndex, frameRecordParams.screenshot.readbackBuffer);
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
  std::array<VkSemaphoreSubmitInfo, 2> frameWaits{waitInfo};
  submitInfo.waitSemaphoreInfoCount = 1;
  if (subs_.ddgiManager)
    if (const auto giWait = subs_.ddgiManager->graphicsWait())
      frameWaits[submitInfo.waitSemaphoreInfoCount++] = *giWait;
  submitInfo.pWaitSemaphoreInfos = frameWaits.data();

  VkCommandBuffer cmdHandle = svc_.commandBufferManager.buffer(imageIndex);
  VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  commandInfo.commandBuffer = cmdHandle;
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &commandInfo;

  VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signalInfo.semaphore = svc_.swapChainManager.renderFinishedForImage(imageIndex);
  signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  std::array<VkSemaphoreSubmitInfo, 2> frameSignals{signalInfo};
  submitInfo.signalSemaphoreInfoCount = 1;
  if (subs_.ddgiManager)
    if (const auto giSignal = subs_.ddgiManager->graphicsSignal())
      frameSignals[submitInfo.signalSemaphoreInfoCount++] = *giSignal;
  submitInfo.pSignalSemaphoreInfos = frameSignals.data();

  phaseStart = TelemetryClock::now();
  subs_.frameSyncManager->resetFence(frame_.currentFrame);
  const VkFence submittedFrameFence =
      subs_.frameSyncManager->fence(frame_.currentFrame);
  if (vkQueueSubmit2(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    submittedFrameFence) != VK_SUCCESS) {
    throw std::runtime_error("failed to submit draw command buffer!");
  }
  subs_.temporalManager->commit();
  if (subs_.screenReflectionPass)
    subs_.screenReflectionPass->onSubmitted(
        subs_.deferredRasterFrameGraphContext->isPassActive(RenderPassId::ScreenReflections));
  if (subs_.volumetricFogPass)
    subs_.volumetricFogPass->onSubmitted(
        subs_.deferredRasterFrameGraphContext->isPassActive(RenderPassId::VolumetricFog));
  if (gpuHierarchyPrepared_ && subs_.gpuHierarchyPass)
    subs_.gpuHierarchyPass->commitFrame(imageIndex);
  commitGltfDeformationFrame();
  subs_.rayShadowManager->commit();
  if (referenceRecorded_ && subs_.referencePathTracePass) {
    auto& path = *subs_.referencePathTracePass;
    const uint64_t epoch = path.historyResets();
    path.commitFrame(imageIndex);
    if (path.historyResets() != epoch) {
      referenceAdaptiveBatch_.reset();
      referenceTimedLightingMs_.reset();
    }
  }
  if (subs_.ddgiManager) {
    subs_.ddgiManager->commit();
    subs_.ddgiManager->submitAsync(imageIndex);
  }
  gfxJournal_.submitted();
  frame_.imagesInFlight[imageIndex] = submittedFrameFence;
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::QueueSubmit,
                           elapsedMilliseconds(phaseStart));
  }

  if (!svc_.config.pathSampleSeries.empty()) {
    const uint32_t limit = screenshot_.pending
        ? screenshot_.referenceSampleTarget.value_or(svc_.config.pathSampleLimit)
        : svc_.config.pathSampleLimit;
    if (limit <= referenceSeriesSamples_)
      throw std::runtime_error("Reference sample series did not advance");
    const uint32_t batch = std::min(svc_.config.pathSamplesPerFrame, limit - referenceSeriesSamples_);
    if (!referenceRecorded_ || !subs_.referencePathTracePass ||
        subs_.referencePathTracePass->attemptedSamples() != referenceSeriesSamples_ + batch)
      throw std::runtime_error("Reference accumulation changed during the sample series");
    referenceSeriesSamples_ += batch;
  }

  if (screenshotThisFrame) {
    phaseStart = TelemetryClock::now();
    const VkFence fence = subs_.frameSyncManager->fence(frame_.currentFrame);
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1, &fence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      throw std::runtime_error("failed to wait for screenshot frame");
    }
    if (frameRecordParams.screenshot.sceneLinear)
      subs_.frameResourceManager->setLinearCaptureTarget(imageIndex);
    // This graphics fence also retires every earlier reference submission.
    for (uint32_t i = 0; i < referenceDiagnosticSeen_.size(); ++i)
      collectReferenceDiagnostics(i);
    try {
      writePendingScreenshot(frame_.currentFrame, frameRecordParams);
      finishComfyUiCapture();
      if (screenshot_.remote && !screenshot_.pending) mcpCaptureStatus_ = "Saved";
      if (screenshot_.interactive)
        subs_.guiManager->setStatusMessage("Saved image: " +
            container::util::pathToUtf8(screenshot_.outputPath));
    } catch (const std::exception& error) {
      screenshot_.pending = false;
      const bool comfyCapture = finishComfyUiCapture(error.what());
      if (!screenshot_.interactive && !screenshot_.remote && !comfyCapture)
        throw;
      if (screenshot_.remote) mcpCaptureStatus_ = std::string("Capture failed: ") + error.what();
      if (screenshot_.interactive) subs_.guiManager->setStatusMessage(error.what());
    }
    if (telemetry) {
      telemetry->setCpuPhase(RendererTelemetryPhase::Screenshot,
                             elapsedMilliseconds(phaseStart));
    }
  }

  phaseStart = TelemetryClock::now();
  result = svc_.swapChainManager.present(
      svc_.ctx.deviceWrapper->presentQueue(), imageIndex);
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
  if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR &&
      result != VK_SUBOPTIMAL_KHR)
    throw std::runtime_error("failed to present swap chain image!");
  if (result != VK_SUCCESS || framebufferResized) {
    framebufferResized = false;
    phaseStart = TelemetryClock::now();
    handleResize();
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  } else {
    markDepthVisibilityFrameComplete(imageIndex, frame_.currentFrame,
                                     pickIdWritten);
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
    resources.oitStorageRangeLimited =
        subs_.frameResourceManager->oitStorageRangeLimited();
    resources.oitPoolOverflowed = subs_.frameResourceManager->oitPoolOverflowed();
    resources.clusteredLightingFallback =
        subs_.activeTechnique->id() == RenderTechniqueId::DeferredRaster &&
        !subs_.lightingManager->clusteredLightCapacity().available();
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
  if (event.gpuTransforms) {
    gpuHierarchyEnabled_ = *event.gpuTransforms;
    if (subs_.guiManager) subs_.guiManager->setGpuHierarchyTransforms(gpuHierarchyEnabled_);
  }
  if (event.reloadShaders)
    processShaderReload(true, true);
  if (event.referenceAutofocus)
    referenceAutofocusRequested_ = true;
  if (event.referenceDisplay) {
    if (subs_.guiManager) subs_.guiManager->setReferencePathDisplay(*event.referenceDisplay);
    else referencePathDisplayOverride_ = event.referenceDisplay;
  }
  if (event.referenceOidn || event.referenceOptix || event.referenceDlss) {
    if (subs_.guiManager) {
      auto settings = subs_.guiManager->referenceOidn();
      if (event.referenceOidn) settings.quality = *event.referenceOidn;
      if (event.referenceDlss) settings.quality = ReferenceOidnQuality::Off;
      settings.optix = event.referenceOptix.value_or(false);
      settings.dlss = event.referenceDlss.value_or(UpscaleMode::Native);
      subs_.guiManager->setReferenceOidn(settings);
    } else {
      if (event.referenceOidn) referenceOidnQualityOverride_ = *event.referenceOidn;
      if (event.referenceDlss) referenceOidnQualityOverride_ = ReferenceOidnQuality::Off;
      referenceOptixOverride_ = event.referenceOptix.value_or(false);
      referenceDlssOverride_ = event.referenceDlss.value_or(UpscaleMode::Native);
    }
  }
  if (event.comfyUi)
    applyComfyUiCaptureAction(*event.comfyUi);
  if (event.proceduralSky && subs_.environmentManager) {
    if (subs_.environmentManager->requestSkyEnvironment({.mode = *event.proceduralSky}))
      processEnvironmentLoad(true);
  }
  if (!event.environment.empty() && subs_.environmentManager) {
    if (subs_.environmentManager->requestHdrEnvironment(container::util::pathFromUtf8(event.environment)) &&
        !event.environmentAsync)
      processEnvironmentLoad(true);
  }
  if (event.environmentYaw && subs_.lightingManager) {
    auto settings = subs_.lightingManager->lightingSettings();
    if (settings.environment.yawDegrees != *event.environmentYaw) {
      settings.environment.yawDegrees = *event.environmentYaw;
      if (subs_.ddgiManager)
        subs_.ddgiManager->resetSceneHistory();
      subs_.lightingManager->setLightingSettings(settings);
      if (subs_.guiManager)
        subs_.guiManager->setLightingSettings(settings);
      if (subs_.temporalManager)
        subs_.temporalManager->reset("environment rotation changed");
    }
  }
  queueCaptureLightEvent(event);
  // Terminals complete the existing edit before any unrelated event can
  // finish it. In particular, a patch-free End must keep its gesture ID.
  const bool materialTerminal=event.materialEdit &&
      (event.materialEdit->phase==container::material::MaterialEditGesturePhase::End ||
       event.materialEdit->phase==container::material::MaterialEditGesturePhase::Cancel);
  if(materialTerminal && !authorMaterial(*event.materialEdit))
    throw std::runtime_error("Capture material gesture could not be completed");
  if(!event.reload.empty() || !event.guiReload.empty() || !event.guiSampleModel.empty() || event.guiUnload || event.merge ||
      event.materialVariant || !event.sceneHistory.empty() || event.nodeFlags || event.nodeAuthoring ||
      event.nodeReparent || event.nodeTransform || event.addPrimitive || event.animationPlaying || event.animationClip ||
      event.animationTime || !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
      !event.physicsJointAction.empty() || event.physicsJointMotorVelocity || event.samples || event.resize ||
      event.swapchainFormat || !event.technique.empty())
    finishEditGestures();
  if (event.ddgiFitProbes)
    pendingDdgiSceneFit_ = *event.ddgiFitProbes;
  if (subs_.ddgiManager &&
      (event.ddgiPaused || event.ddgiEnabled || event.ddgiIntensity)) {
    const auto &previous = subs_.ddgiManager->settings();
    auto settings = previous;
    if (event.ddgiPaused)
      settings.paused = *event.ddgiPaused;
    if (event.ddgiEnabled)
      settings.enabled = *event.ddgiEnabled;
    if (event.ddgiIntensity)
      settings.intensity = *event.ddgiIntensity;
    const bool enabledChanged = settings.enabled != previous.enabled;
    const bool intensityChanged = settings.intensity != previous.intensity;
    subs_.ddgiManager->applySettings(settings);
    if (subs_.guiManager)
      subs_.guiManager->trySetDdgiSettings(settings);
    if ((enabledChanged || intensityChanged) && subs_.temporalManager)
      subs_.temporalManager->reset(enabledChanged ? "capture DDGI enable changed"
                                                  : "capture DDGI intensity changed");
  }
  if (subs_.lightingManager &&
      (!event.areaLighting.empty() || event.areaLightSamples)) {
    const auto &currentSettings = subs_.lightingManager->lightingSettings();
    auto settings = currentSettings;
    if (!event.areaLighting.empty())
      settings.areaLightingMode = event.areaLighting == "ltc" ? 1u : 0u;
    if (event.areaLightSamples)
      settings.areaLightSampleCount = *event.areaLightSamples;
    const bool areaLightingChanged =
        settings.areaLightingMode != currentSettings.areaLightingMode ||
        settings.areaLightSampleCount != currentSettings.areaLightSampleCount;
    subs_.lightingManager->setLightingSettings(settings);
    if (subs_.guiManager)
      subs_.guiManager->setLightingSettings(settings);
    if (areaLightingChanged && subs_.temporalManager)
      subs_.temporalManager->reset("area light integration changed");
  }
  if (subs_.rayShadowManager &&
      (!event.rayShadows.empty() || event.rayShadowSamples ||
       event.rayShadowDenoise)) {
    auto &settings = subs_.rayShadowManager->settings();
    if (!event.rayShadows.empty())
      settings.mode = event.rayShadows == "soft"   ? RayShadowMode::Soft
                      : event.rayShadows == "hard" ? RayShadowMode::Hard
                                                   : RayShadowMode::Raster;
    if (event.rayShadowSamples)
      settings.areaSamples = *event.rayShadowSamples;
    if (event.rayShadowDenoise)
      settings.denoise = *event.rayShadowDenoise;
    if (subs_.guiManager)
      subs_.guiManager->rayShadowSettings() = settings;
  }
  if (!event.reload.empty()) {
    settlePendingModelLoads();
    captureObjectBase_.reset();
    captureObjectNode_ = 0;
    reloadSceneModel(event.reload);
  }
  if (!event.guiReload.empty()) {
    if (!subs_.guiManager)
      throw std::runtime_error(
          "GUI reload requires an initialized GUI manager");
    captureObjectBase_.reset();
    captureObjectNode_ = 0;
    subs_.guiManager->queueModelLoadRequest(event.guiReload, 1.0f,
                                            event.guiReload);
  }
  if (!event.guiSampleModel.empty()) {
    if (!subs_.guiManager ||
        !subs_.guiManager->queueSampleModelLoadRequest(event.guiSampleModel))
      throw std::runtime_error("GUI sample model not found: " +
                               event.guiSampleModel);
    captureObjectBase_.reset();
    captureObjectNode_ = 0;
  }
  if (event.guiUnload) {
    if (!subs_.guiManager)
      throw std::runtime_error("GUI unload requires an initialized GUI manager");
    captureObjectBase_.reset();
    captureObjectNode_ = container::scene::SceneGraph::kInvalidNode;
    subs_.guiManager->queueModelUnloadRequest();
  }
  if (event.merge) {
    glm::mat4 placement(1.0f);
    if (event.merge->placementMatrix)
      for (size_t column=0; column<4; ++column)
        for (size_t row=0; row<4; ++row)
          placement[column][row]=static_cast<float>((*event.merge->placementMatrix)[column*4+row]);
    if(subs_.guiManager) subs_.guiManager->queueModelMergeRequest(event.merge->path,
        event.merge->parentName,event.merge->incomingScene,placement,event.merge->placementMatrix);
    else {
      if(!modelLoad_) modelLoad_=std::make_unique<ModelLoadState>();
      container::ui::ModelLoadRequest request;
      request.path=event.merge->path; request.label=event.merge->path;
      request.merge=container::ui::ModelMergeRequest{
          event.merge->parentName,event.merge->incomingScene,placement,event.merge->placementMatrix};
      modelLoad_->queued=std::move(request);
    }
  }
  if (event.guiWaitForLoad &&
      (!event.guiReload.empty() || !event.guiSampleModel.empty() || event.guiUnload || event.merge))
    settlePendingModelLoads();
  if(event.materialEdit && !materialTerminal && !authorMaterial(*event.materialEdit))
    throw std::runtime_error("Capture material edit could not be applied");
  if(event.materialBinding && !authorMaterialBinding(*event.materialBinding))
    throw std::runtime_error("Capture material binding could not be applied");
  const auto cameraAction=[&](auto&& apply,bool expectedFailure,const char* label) {
    if(!expectedFailure) {
      if(!apply()) throw std::runtime_error(std::string("Capture ")+label+" could not be applied");
      return;
    }
    std::string failure;
    try {if(!apply()) failure="The camera action could not be published";}
    catch(const std::exception& error) {failure=error.what();}
    if(failure.empty()) throw std::runtime_error(std::string("Expected ")+label+" failure did not occur");
    size_t end=std::min<size_t>(failure.size(),4096);
    while(end<failure.size() && end && (uint8_t(failure[end])&0xc0)==0x80) --end;
    failure.resize(end);spdlog::warn("Expected capture {} failure: {}",label,failure);
  };
  if(event.cameraProjection) {
    const auto& request=*event.cameraProjection;
    cameraAction([&] {return authorCameraProjectionSource(request.sourceNode,request.patch,request.expectedCameraRevision);},
        event.expectCameraProjectionFailure,"camera projection");
  }
  if(event.cameraSelection) {
    const auto& request=*event.cameraSelection;
    cameraAction([&] {return setGltfCameraNode(request.viewport?std::nullopt:std::optional<size_t>(
        request.sourceNode?size_t(*request.sourceNode):cameraNodeByName(*request.nodeName)));},
        event.expectCameraSelectionFailure,"camera selection");
  }
  if(event.resourceRetirement) {
    const auto& request=*event.resourceRetirement;
    const auto apply=[&] {
      if(!subs_.sceneManager) throw std::runtime_error("The resource catalog is unavailable");
      auto& manager=*subs_.sceneManager;const auto generation=manager.resourceLifecycleGeneration();
      using D=container::ui::ResourceCatalogDomain;using K=container::ui::ResourceCatalogKind;
      using Domain=container::util::ResourceDomainKind;using Kind=container::util::ResourceKind;
      const auto domain=[&] {
        switch(request.domain) {case D::Archive:return Domain::Archive;case D::Evaluated:return Domain::Evaluated;case D::Runtime:return Domain::Runtime;}
        throw std::invalid_argument("The resource domain is invalid");
      }();
      const auto kind=[&] {
        switch(request.kind) {case K::Image:return Kind::Image;case K::Texture:return Kind::Texture;
          case K::Sampler:return Kind::Sampler;case K::RuntimeTexture:return Kind::RuntimeTexture;}
        throw std::invalid_argument("The resource kind is invalid");
      }();
      const container::scene::TextureResourceSelection selection{domain,kind,request.row};
      if(!generation || !generation->valid() || !generation->active(selection))
        throw std::invalid_argument("The requested resource is no longer active");
      // Capture is also used with --no-ui. Resolve weak intent from the scene
      // owner's current catalog, then use the same authoritative author path.
      const auto handle=generation->resource(selection);
      if(!handle) throw std::invalid_argument("The requested resource has no current identity");
      const container::ui::ResourceCatalogTarget target{manager.materialAssetIdentity(),manager.materialResourceRevision(),
          handle.domain().identity(),handle.incarnation(),request.row,request.domain,request.kind};
      if((request.expectedResourceRevision && *request.expectedResourceRevision!=target.resourceRevision) ||
          (request.expectedIncarnation && *request.expectedIncarnation!=target.incarnation))
        throw std::invalid_argument("The requested resource catalog revision or incarnation has changed");
      return authorResourceRetirement({target});
    };
    if(!event.expectResourceRetirementFailure) {
      if(!apply()) throw std::runtime_error("Capture resource retirement could not be applied");
    } else {
      std::string failure;
      try {if(!apply()) failure="The resource retirement could not be applied";}
      catch(const std::exception& error) {failure=error.what();}
      if(failure.empty()) throw std::runtime_error("Expected resource retirement failure did not occur");
      size_t end=std::min<size_t>(failure.size(),4096);
      while(end<failure.size() && end && (uint8_t(failure[end])&0xc0)==0x80) --end;
      failure.resize(end);resourceCatalogError_="Cannot delete resource: "+failure;
      spdlog::warn("Expected capture resource retirement failure: {}",failure);
    }
  }
  if(event.materialRetirement) {
    const auto& request=*event.materialRetirement;
    const auto apply=[&] {return authorMaterialRetirement(request.runtimeMaterial,request.sourceMaterial,request.expectedAuthoredRevision);};
    if(!event.expectMaterialRetirementFailure) {
      if(!apply()) throw std::runtime_error("Capture material retirement could not be applied");
    } else {
      std::string failure;
      try {if(!apply()) failure="The material retirement could not be applied";}
      catch(const std::exception& error) {failure=error.what();}
      if(failure.empty()) throw std::runtime_error("Expected material retirement failure did not occur");
      size_t end=std::min<size_t>(failure.size(),4096);
      while(end<failure.size() && end && (uint8_t(failure[end])&0xc0)==0x80) --end;
      failure.resize(end);
      spdlog::warn("Expected capture material retirement failure: {}",failure);
    }
  }
  if(event.materialTextureEdit) {
    if(!event.expectMaterialTextureFailure) {
      if(!authorMaterialTexture(*event.materialTextureEdit))
        throw std::runtime_error("Capture material texture edit could not be applied");
    } else {
      std::string failure;
      try {
        if(!authorMaterialTexture(*event.materialTextureEdit)) failure="The texture edit could not be applied";
      } catch(const std::exception& error) {failure=error.what();}
      if(failure.empty()) throw std::runtime_error("Expected material texture failure did not occur");
      materialTextureError_="Cannot edit texture: "+failure;
      size_t end=std::min<size_t>(materialTextureError_.size(),4096);
      while(end<materialTextureError_.size() && end && (uint8_t(materialTextureError_[end])&0xc0)==0x80) --end;
      materialTextureError_.resize(end);materialTextureErrorSource_=event.materialTextureEdit->sourceMaterial;
      materialTextureErrorAsset_=subs_.sceneManager?subs_.sceneManager->materialAssetIdentity():0;
    }
  }
  if (event.bimSelectedObject) {
    if (!subs_.bimManager ||
        !subs_.bimManager->metadataForObject(*event.bimSelectedObject))
      throw std::runtime_error("Capture BIM selection index is not present in the active model");
    selectBimObject(*event.bimSelectedObject);
  }
  if (event.materialVariant) captureMaterialVariant_=event.materialVariant;
  if (event.gltfScene)
    captureGltfScene_ = CaptureGltfSceneRequest{size_t(*event.gltfScene), event.expectGltfSceneRejection};
  if (!event.sceneHistory.empty()) captureSceneHistory_=event.sceneHistory;
  if (event.nodeFlags) {
    uint32_t target = container::scene::SceneGraph::kInvalidNode;
    for (uint32_t id = 0; id < sceneGraph_.nodeCount(); ++id)
      if (sceneGraph_.getNode(id)->sourceNodeIndex == event.nodeFlags->sourceNode) {
        target = id; break;
      }
    const auto* node = sceneGraph_.getNode(target);
    if (!node) throw std::invalid_argument("Capture nodeFlags sourceNode does not exist");
    ensureSceneEditHistory();
    sceneEdits_->setNodeFlags(target, event.nodeFlags->visible.value_or(node->visible),
        event.nodeFlags->selectable.value_or(node->selectable),
        event.nodeFlags->hoverable.value_or(node->hoverable));
  }
  if (event.nodeAuthoring) {
    const auto& edit = *event.nodeAuthoring;
    auto target = edit.graphNode.value_or(container::scene::SceneGraph::kInvalidNode);
    if (edit.sourceNode) {
      for (uint32_t node = 0; node < sceneGraph_.nodeCount(); ++node) {
        if (sceneGraph_.getNode(node)->sourceNodeIndex != *edit.sourceNode) continue;
        if (target != container::scene::SceneGraph::kInvalidNode)
          throw std::invalid_argument("Capture nodeAuthoring source identity is ambiguous");
        target = node;
      }
    }
    authorSceneNode({edit.action == "rename"
        ? container::ui::SceneNodeAuthoringAction::Rename
        : edit.action == "create-empty" ? container::ui::SceneNodeAuthoringAction::CreateEmpty
        : edit.action == "duplicate-subtree" ? container::ui::SceneNodeAuthoringAction::DuplicateSubtree
        : edit.action == "make-editable" ? container::ui::SceneNodeAuthoringAction::MakeEditable
                                         : container::ui::SceneNodeAuthoringAction::DeleteSubtree,
        sceneGraph_.identity(), target, edit.name, sceneGraph_.nodeIncarnation(target)});
  }
  const auto resolveNode=[this](const container::temporal::CaptureNodeReference& reference) {
    auto target=reference.graphNode.value_or(container::scene::SceneGraph::kInvalidNode);
    if(reference.sourceNode) {
      for(uint32_t node=0;node<sceneGraph_.nodeCount();++node) {
        if(sceneGraph_.getNode(node)->sourceNodeIndex!=*reference.sourceNode) continue;
        if(target!=container::scene::SceneGraph::kInvalidNode)
          throw std::invalid_argument("Capture node source identity is ambiguous");
        target=node;
      }
    }
    const auto* node=sceneGraph_.getNode(target);
    if(!node || (!sceneGraph_.isNodeInActiveHierarchy(target) && !node->editorSceneRoot) || sceneGraph_.isNodeRetired(target))
      throw std::invalid_argument("Capture node reference does not identify an active hierarchy node");
    return target;
  };
  if(event.nodeReparent) {
    const auto node=resolveNode(event.nodeReparent->node),parent=resolveNode(event.nodeReparent->parent);
    if(sceneGraph_.getNode(node)->parent!=parent && !reparentSceneNode(node,parent))
      throw std::invalid_argument("Capture node reparent was rejected");
  }
  if(event.nodeTransform) {
    const auto node=resolveNode(event.nodeTransform->node);
    ensureSceneEditHistory(); sceneEdits_->finishGesture();
    if(sceneGraph_.getNode(node)->localTransform!=event.nodeTransform->matrix &&
        !sceneEdits_->setTransform(node,event.nodeTransform->matrix))
      throw std::invalid_argument("Capture node transform was rejected");
  }
  if (event.nodeGizmo) {
    using Action = container::temporal::CaptureNodeGizmo::Operation;
    using Gizmo = container::temporal::CaptureLightGizmo;
    using container::ui::TransformAxis;
    using container::ui::TransformSpace;
    using container::ui::ViewportTool;
    const auto& request = *event.nodeGizmo;
    const auto node = resolveNode(request.node);
    if (request.operation == Action::Select || request.operation == Action::Begin) {
      if (!sceneGraph_.isNodeEffectivelySelectable(node))
        throw std::invalid_argument("Capture node gizmo target cannot be selected");
      if (request.operation == Action::Begin && transformDragSession_.active)
        throw std::invalid_argument("Capture node gizmo is already active");
      finishEditGestures();
      if (sceneState_.selectedMeshNode != node) {
        clearSelectedMeshNode();
        sceneState_.selectedMeshNode = node;
      }
    } else if (!transformDragSession_.active || transformDragSession_.nodeIndex != node ||
               sceneState_.selectedMeshNode != node) {
      throw std::invalid_argument("Capture node gizmo is not active for the requested node");
    }
    switch (request.operation) {
    case Action::Select:
      break;
    case Action::Begin: {
      const auto tool = request.gizmo.tool == Gizmo::Tool::Translate ? ViewportTool::Translate :
          request.gizmo.tool == Gizmo::Tool::Rotate ? ViewportTool::Rotate : ViewportTool::Scale;
      const auto space = request.gizmo.space == Gizmo::Space::Local ? TransformSpace::Local : TransformSpace::World;
      const auto axis = request.gizmo.axis == Gizmo::Axis::X ? TransformAxis::X :
          request.gizmo.axis == Gizmo::Axis::Y ? TransformAxis::Y :
          request.gizmo.axis == Gizmo::Axis::Z ? TransformAxis::Z : TransformAxis::Free;
      const auto previous = interactionController_.state();
      interactionController_.setTool(tool);
      interactionController_.setTransformSpace(space);
      interactionController_.setTransformAxis(axis);
      transformSelectedNodeByDrag(tool, space, axis, request.gizmo.snap, request.delta.x, request.delta.y);
      interactionController_.setTool(previous.tool);
      interactionController_.setTransformSpace(previous.transformSpace);
      interactionController_.setTransformAxis(previous.transformAxis);
      if (!transformDragSession_.active)
        throw std::runtime_error("Capture node gizmo could not begin");
      break;
    }
    case Action::Update:
      transformSelectedNodeByDrag(transformDragSession_.tool, transformDragSession_.space,
                                  transformDragSession_.axis, transformDragSession_.snapEnabled,
                                  request.delta.x, request.delta.y);
      break;
    case Action::End:
    case Action::Cancel:
      finishSceneTransformGesture(request.operation == Action::Cancel);
      break;
    }
    captureNodeGizmoTarget_ = std::pair{sceneGraph_.identity(), node};
  }
  if(event.addPrimitive) {
    const auto& edit=*event.addPrimitive;
    const std::pair<std::string_view,ScenePrimitiveKind> kinds[]={
        {"quad",ScenePrimitiveKind::Quad},{"plane",ScenePrimitiveKind::Plane},
        {"cube",ScenePrimitiveKind::Cube},{"sphere",ScenePrimitiveKind::Sphere},
        {"triangular-prism",ScenePrimitiveKind::TriangularPrism},
        {"pyramid",ScenePrimitiveKind::Pyramid},{"torus",ScenePrimitiveKind::Torus}};
    const auto found=std::ranges::find_if(kinds,[&](const auto& item) { return item.first==edit.kind; });
    if(found==std::end(kinds)) throw std::invalid_argument("Capture primitive kind is unsupported");
    const auto parent=edit.parent ? std::optional<uint32_t>(resolveNode(*edit.parent)) : std::nullopt;
    if(addScenePrimitive(found->second,edit.name,parent)==container::scene::SceneGraph::kInvalidNode)
      throw std::invalid_argument("Capture primitive creation failed");
  }
  if (event.interactivityPlaying || event.interactivityReset || event.interactivityEvent)
    gltfInteractivityRequest_ = {event.interactivityPlaying, event.interactivityReset, event.interactivityEvent};
  if (event.animationPlaying || event.animationClip || event.animationTime) {
    if (!gltfAnimation_) resetGltfAnimation();
    if(!gltfAnimation_ || !subs_.sceneManager)
      throw std::invalid_argument("Capture animation controls require a primary glTF animation player");
    const auto selected=event.animationClip ? (*event.animationClip<0 ? std::optional<size_t>{}
        : std::optional<size_t>(size_t(*event.animationClip))) : gltfAnimation_->selectedClip();
    if(selected && *selected>=gltfAnimation_->library().clips.size())
      throw std::invalid_argument("Capture animation clip is out of range");
    if((event.animationTime || event.animationPlaying.value_or(false)) && !selected)
      throw std::invalid_argument("Capture animation time/play requires a selected glTF animation clip");
    container::geometry::gltf::AnimationDirtySet dirty;
    if(event.animationClip || event.animationTime) {
      if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
      auto& source=subs_.sceneManager->animationSource();
      if(event.animationClip) dirty.merge(selected ? gltfAnimation_->selectClip(source,*selected)
          : gltfAnimation_->clearClip(source));
      if(event.animationTime) dirty.merge(gltfAnimation_->seek(source,*event.animationTime));
      pendingGltfAnimationDeltaSeconds_=0;
      if (!dirty.empty()) publishGltfAnimationChanges(dirty);
      if (subs_.temporalManager && dirty.nodeFlags.empty())
        subs_.temporalManager->reset("capture glTF animation timeline");
    }
    if(event.animationPlaying) {
      if (*event.animationPlaying) gltfAnimation_->play();
      else gltfAnimation_->pause();
    }
    if (subs_.guiManager) {
      auto& controls = subs_.guiManager->gltfAnimationControls();
      controls.requestedPlaying.reset(); controls.requestedClip.reset(); controls.requestedTime.reset();
      controls.selectedClip=gltfAnimation_->selectedClip()?int(*gltfAnimation_->selectedClip()):-1;
      controls.time=gltfAnimation_->time();
      controls.playing = gltfAnimation_->playing();
    }
  }
  if (!event.physicsAttach.empty() || !event.physicsAction.empty() ||
      event.physicsJointCreate || !event.physicsJointAction.empty() || event.physicsJointMotorVelocity)
    pendingPhysicsActions_.push_back({event.physicsNode, selectedBimObjectIndex_,
        event.physicsAttach, event.physicsAction,
        event.physicsJointCreate ?
            std::make_shared<container::temporal::CapturePhysicsJointCreate>(*event.physicsJointCreate) : nullptr,
        event.physicsJointAction, event.physicsJointMotorVelocity,
        event.physicsSettings ?
            std::make_shared<container::physics::PhysicsBodySettings>(*event.physicsSettings) : nullptr});
  if (event.taa)
    subs_.temporalManager->settings().enabled = *event.taa;
  if (event.upscaling)
    captureUpscaling_ = event.upscaling;
  if (event.samples)
    recreateMsaaResources(sampleCountFromSamples(*event.samples));
  if (event.reset)
    subs_.temporalManager->reset("capture sequence reset");
  captureAcquireOutOfDate_ = event.acquireOutOfDate;
  capturePresentSuboptimal_ = event.presentSuboptimal;
  if (event.sectionPlane)
    captureSectionPlane_ = *event.sectionPlane;
  if (event.bimAnnotations)
    captureBimAnnotations_ =
        std::make_shared<const container::temporal::CaptureBimAnnotations>(*event.bimAnnotations);
  if (const auto& wireframe = event.wireframe) {
    if (!subs_.guiManager)
      throw std::runtime_error("Capture wireframe settings require the editor GUI");
    subs_.guiManager->setWireframeSettings({
        .enabled = wireframe->enabled,
        .mode = wireframe->mode == container::temporal::CaptureWireframe::Mode::Full
            ? container::ui::WireframeMode::Full : container::ui::WireframeMode::Overlay,
        .depthTest = wireframe->depthTest,
        .color = wireframe->color,
        .lineWidth = wireframe->lineWidth,
        .overlayIntensity = wireframe->intensity});
  }
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
  // A camera pose write samples this frame's navigation track. Dispatch after
  // applying it, rather than accidentally authoring the previous frame's view.
  if(event.cameraPose) {
    const auto& request=*event.cameraPose;
    cameraAction([&] {return authorCameraViewPoseSource(request.sourceNode,request.expectedCameraRevision);},
        event.expectCameraPoseFailure,"camera pose");
  }
  uint32_t objectNode = sample.objectNode;
  // Capture tracks are sampled past their last key. An explicit unload retires
  // their target; only a later capture load event permits target lookup again.
  const bool objectTracksActive =
      captureObjectNode_ != container::scene::SceneGraph::kInvalidNode;
  if (objectTracksActive && !sample.objectName.empty()) {
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
  if (objectTracksActive && (sample.objectTranslation || sample.objectScale)) {
    const uint32_t nodeIndex = objectNode;
    const auto *node = sceneGraph_.getNode(nodeIndex);
    if (!node)
      throw std::invalid_argument("Capture objectNode does not exist");
    requireEditableSceneNode(nodeIndex);
    if (!captureObjectBase_ || captureObjectNode_ != nodeIndex) {
      captureObjectBase_ = node->localTransform;
      captureObjectNode_ = nodeIndex;
    }
    glm::mat4 transform = *captureObjectBase_;
    if (sample.objectScale)
      for (int column = 0; column < 3; ++column)
        transform[column] *= (*sample.objectScale)[column];
    if (sample.objectTranslation)
      transform[3] += glm::vec4(*sample.objectTranslation, 0);
    if(node->localTransform!=transform) finishEditGestures();
    sceneGraph_.setLocalTransform(nodeIndex, transform);
    sceneGraph_.updateWorldTransforms();
    refreshSceneObjectData();
  }
  if (objectTracksActive && event.objectVisible) {
    if (!sceneGraph_.getNode(objectNode))
      throw std::invalid_argument("Capture objectNode does not exist");
    requireEditableSceneNode(objectNode);
    finishEditGestures();
    sceneGraph_.setVisible(objectNode, *event.objectVisible);
    publishSceneEdit();
  }
  if (sample.bimTranslation && subs_.bimManager) {
    if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
    subs_.bimManager->setRootTranslation(*sample.bimTranslation);
  }
  if (event.exposure)
    captureExposure_ = *event.exposure;
  if (event.areaLightPosition) {
    const auto &lights = subs_.lightingManager->editableLights();
    const auto it = std::ranges::find_if(lights, [](const auto &light) {
      return light.type == EditableLightType::Area;
    });
    if (it == lights.end())
      throw std::invalid_argument(
          "Capture areaLightPosition needs an area light");
    if (subs_.sceneManager && it->source == EditableLightSource::Imported) {
      const auto bindings = subs_.sceneManager->authoredAreaLightBindings();
      if (it->id.index < bindings.size() &&
          subs_.sceneManager->isLinkedSourceNodeReadOnly(bindings[it->id.index].sourceNode))
        throw std::invalid_argument("This linked asset is read-only. Use Make Editable first.");
    }
    auto light = *it;
    light.position = *event.areaLightPosition;
    if (!subs_.lightingManager->updateEditableLight(light))
      throw std::runtime_error("Capture area light update failed");
  }
}

container::gpu::ExposureSettings RendererFrontend::effectiveExposureSettings() const {
  auto settings = subs_.guiManager ? subs_.guiManager->exposureSettings()
                                   : exposureSettingsFromConfig(svc_.config);
  if (captureExposure_) {
    settings.mode = container::gpu::kExposureModeManual;
    settings.manualExposure = *captureExposure_;
  }
  return settings;
}

nlohmann::json RendererFrontend::captureTelemetry() const {
  const auto &temporal = *subs_.temporalManager;
  const auto &settings = temporal.settings();
  const auto extent = svc_.swapChainManager.extent();
  const auto surfaceFormat = svc_.swapChainManager.surfaceFormat();
  VkPhysicalDeviceProperties gpu{};
  vkGetPhysicalDeviceProperties(svc_.ctx.deviceWrapper->physicalDevice(), &gpu);
  const auto owned = container::gpu::ownedVulkanObjectCounts(svc_.ctx.deviceWrapper->device());
  VmaTotalStatistics allocationStats{};
  vmaCalculateStatistics(svc_.allocationManager.memoryManager()->allocator(), &allocationStats);
  nlohmann::json json{
      {"schemaVersion", 1},
      {"swapchainFormat", static_cast<int32_t>(surfaceFormat.format)},
      {"swapchainColorSpace", static_cast<int32_t>(surfaceFormat.colorSpace)},
      {"swapchainSupportsTransferSrc", svc_.swapChainManager.supportsTransferSrc()},
      {"presentationPipelineGeneration", presentationPipelineGeneration_},
      {"guiPipelineGeneration", subs_.guiManager
           ? subs_.guiManager->swapchainRenderingGeneration() : 0u},
      {"gpu", gpu.deviceName},
      {"vendorId", gpu.vendorID},
      {"deviceId", gpu.deviceID},
      {"driverVersion", gpu.driverVersion},
      {"vulkanApiVersion", gpu.apiVersion},
      {"storageBufferRangeLimit", gpu.limits.maxStorageBufferRange},
      {"build", {{"revision", CONTAINER_GIT_REVISION},
                 {"configuration", CONTAINER_BUILD_CONFIG},
                 {"compiler", CONTAINER_BUILD_COMPILER}}},
      {"validationEnabled", svc_.config.enableValidationLayers},
      {"technique", std::string(subs_.activeTechnique->name())},
      {"hierarchyTransforms", {{"enabled", gpuHierarchyEnabled_}, {"active", gpuHierarchyPrepared_},
                                {"cpuObjectUploadBytes",cpuObjectUploadBytes_},
                                {"status", gpuHierarchyStatus_}}},
      {"shaderReload", {{"generation", shaderReloadGeneration_}, {"status", shaderReloadStatus_},
          {"ownedObjects", {{"pipelines", owned.pipelines}, {"pipelineLayouts", owned.pipelineLayouts},
                            {"pipelineCaches", owned.pipelineCaches}, {"descriptorPools", owned.descriptorPools},
                            {"descriptorSetLayouts", owned.descriptorSetLayouts}, {"imageViews", owned.imageViews},
                            {"samplers", owned.samplers}, {"shaderModules", owned.shaderModules}}},
          {"vmaAllocations", allocationStats.total.statistics.allocationCount},
          {"vmaBytes", allocationStats.total.statistics.allocationBytes}}},
      {"resolution", {extent.width, extent.height}},
      {"msaaSamples", sampleCountToSamples(msaaSampleCount_)},
      {"supportedMsaaSamples", supportedMsaaSamples_},
      {"msaaDepthResolveMode", static_cast<uint32_t>(
           resources_.renderPasses.depthPrepass->depthResolveMode)},
      {"msaaStencilResolveMode", static_cast<uint32_t>(
           resources_.renderPasses.depthPrepass->stencilResolveMode)},
      {"wideLinesSupported", svc_.ctx.deviceWrapper->enabledFeatures().wideLines == VK_TRUE},
      {"wireframeWideLinesSupported", svc_.ctx.wireframeWideLinesSupported},
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
  const auto& frameResources = *subs_.frameResourceManager;
  const auto oitNodes = deferredRasterRuntimeOitNodeCapacity(&frameResources, 0);
  json["oit"] = {{"nodeCapacity", oitNodes},
                 {"nodeBufferBytes", sizeof(OitNode) * static_cast<uint64_t>(oitNodes)},
                 {"requestedNodeCapacity", frameResources.requestedOitNodeCapacity()},
                 {"maxNodeCapacity", frameResources.maxOitNodeCapacity()},
                 {"storageRangeLimited", frameResources.oitStorageRangeLimited()},
                 {"overflowed", frameResources.oitPoolOverflowed()},
                 {"growCount", frameResources.oitPoolGrowCount()}};
  const auto& clustered = subs_.lightingManager->clusteredLightCapacity();
  const bool clusteredActive =
      subs_.activeTechnique->id() == RenderTechniqueId::DeferredRaster &&
      subs_.lightingManager->isTiledLightingReady() &&
      subs_.lightingManager->lightingData().tileCullEnabled != 0u;
  json["clusteredLighting"] = {{"status", clusteredLightCapacityStatusName(clustered.status)},
                               {"active", clusteredActive},
                               {"tileGridBytes", clustered.tileGridBytes},
                               {"lightIndexListBytes", clustered.lightIndexListBytes}};
  if (subs_.exposureManager) {
    const auto exposure = sanitizeExposureSettings(effectiveExposureSettings());
    const bool automatic = exposure.mode == container::gpu::kExposureModeAuto;
    json["exposure"] = {
        {"mode", automatic ? "auto" : "manual"},
        {"resolvedExposure", subs_.exposureManager->resolvedExposure(exposure)},
        {"averageLuminance", automatic && subs_.exposureManager->hasExposureDebugState()
                                 ? nlohmann::json(subs_.exposureManager->averageLuminance())
                                 : nlohmann::json(nullptr)}};
  }
  const auto sceneExtent = sceneRenderExtent();
  const auto upscaleTelemetry = [&](UpscaleBackend backend, bool active) {
    const auto& selection = upscaleResourceRequest_.selection;
    const auto requested = selection.backend == backend ? selection.mode : UpscaleMode::Native;
    return nlohmann::json{{"requested", upscaleModeName(requested)},
        {"mode", upscaleModeName(active ? requested : UpscaleMode::Native)},
        {"active", active},
        {"status", requested == UpscaleMode::Native ? std::string_view{} : std::string_view{upscaleViewportStatus_}},
        {"renderExtent", {sceneExtent.width, sceneExtent.height}},
        {"displayExtent", {extent.width, extent.height}}};
  };
  json["fsr"] = upscaleTelemetry(UpscaleBackend::Fsr, temporal.fsrActive());
  json["dlss"] = upscaleTelemetry(UpscaleBackend::Dlss, temporal.dlssActive());
  if (const auto bytes = temporal.dlssAllocatedBytes())
    json["dlss"]["sdkAllocatedBytes"] = *bytes;
  if (referencePathActive()) {
    const auto* path = subs_.referencePathTracePass.get();
    const auto lens = path ? path->effectiveLens() : std::nullopt;
    const bool optixSelected = path ? path->optixSelected() : svc_.config.pathOidn.optix;
    const auto denoiser = path ? path->oidnStatus() : ReferenceOidnStatus{};
    const auto oidn = optixSelected ? ReferenceOidnStatus{} : denoiser;
    const auto optix = optixSelected ? denoiser : ReferenceOidnStatus{};
    const auto rr = path ? path->dlssStatus() : ReferenceDlssStatus{};
    json["referencePath"] = {{"active", path != nullptr}, {"status", referenceStatus_},
        {"display", referencePathDisplayName(referencePathDisplay())},
        {"attemptedSamples", path ? path->attemptedSamples() : 0},
        {"historyResets", path ? path->historyResets() : 0},
        {"allocatedBytes", path ? path->allocatedBytes() : 0},
        {"diagnosticEpoch", referenceDiagnosticEpoch_},
        {"unsupportedSamples", referenceUnsupported_}, {"rejectedSamples", referenceRejected_},
        {"experimentalMaterialFallbacks", referenceExperimentalMaterialFallbackMetadata(referenceExperimentalMaterialFallbacks_)},
        {"samplesPerFrame", svc_.config.pathSamplesPerFrame}, {"sampleLimit", svc_.config.pathSampleLimit},
        {"scheduledSamplesPerFrame", referenceAdaptiveBatch_.value_or(svc_.config.pathSamplesPerFrame)},
        {"lastSubmittedSamples", path ? path->lastSubmittedSamples() : 0},
        {"adaptiveTargetMs", svc_.config.pathTargetMs},
        {"lastTimedLightingMs", referenceTimedLightingMs_ ? nlohmann::json(*referenceTimedLightingMs_) : nlohmann::json(nullptr)},
        {"fireflyClamp", path ? path->effectiveFireflyClamp() : 0.0f},
        {"oidn", {{"quality", referenceOidnQualityName(path ? path->oidnQuality() :
                      optixSelected ? ReferenceOidnQuality::Off : svc_.config.pathOidn.quality)},
                  {"status", oidn.message}, {"displayedSamples", oidn.displayedSamples},
                  {"allocatedBytes", oidn.allocatedBytes},
                  {"lastGpuMs", oidn.lastGpuMs ? nlohmann::json(*oidn.lastGpuMs) : nlohmann::json(nullptr)}}},
        {"optix", {{"requested", optixSelected}, {"status", optix.message},
                   {"displayedSamples", optix.displayedSamples}, {"allocatedBytes", optix.allocatedBytes},
                   {"lastGpuMs", optix.lastGpuMs ? nlohmann::json(*optix.lastGpuMs) : nlohmann::json(nullptr)}}},
        {"dlss", {{"quality", referenceDlssQualityName(path ? path->dlssQuality() : svc_.config.pathOidn.dlss)},
                  {"status", rr.message}, {"active", rr.active},
                  {"renderExtent", {rr.renderExtent.width, rr.renderExtent.height}},
                  {"outputExtent", {rr.outputExtent.width, rr.outputExtent.height}},
                  {"historyResets", rr.historyResets}, {"historyFrames", rr.historyFrames},
                  {"allocatedBytes", rr.allocatedBytes},
                  {"vendorAllocatedBytes", rr.vendorAllocatedBytes ? nlohmann::json(*rr.vendorAllocatedBytes) : nlohmann::json(nullptr)},
                  {"lastGpuMs", rr.lastGpuMs ? nlohmann::json(*rr.lastGpuMs) : nlohmann::json(nullptr)}}},
        {"ground", referenceGroundMetadata(path ? path->effectiveGround() : ReferenceGroundSettings{})},
        {"autofocusStatus", referenceAutofocusStatus_},
        {"maxBounces", svc_.config.pathMaxBounces}, {"seed", svc_.config.pathSeed},
        {"apertureRadius", lens ? lens->apertureRadius : 0.0f},
        {"focusDistance", lens ? nlohmann::json(lens->focusDistance) : nlohmann::json(nullptr)}};
  }
  if (subs_.gpuHierarchyPass && gpuHierarchyPrepared_) {
    const auto stats = subs_.gpuHierarchyPass->stats(gpuHierarchyImage_);
    json["hierarchyTransforms"]["nodes"] = stats.nodes;
    json["hierarchyTransforms"]["objects"] = stats.objects;
    json["hierarchyTransforms"]["levels"] = stats.levels;
    json["hierarchyTransforms"]["dispatchedLevels"] = stats.dispatchedLevels;
    json["hierarchyTransforms"]["dirtyLocals"] = stats.dirtyLocals;
    json["hierarchyTransforms"]["uploadedBytes"] = stats.uploadedBytes;
    json["hierarchyTransforms"]["prepareMs"] = stats.prepareMs;
    json["hierarchyTransforms"]["objectMetadataBytes"] = stats.objectMetadataBytes;
    json["hierarchyTransforms"]["objectMetadataMs"] = stats.objectMetadataMs;
    if (stats.previousGpuMs >= 0) json["hierarchyTransforms"]["previousGpuMs"] = stats.previousGpuMs;
  }
  if (vdbPlayback_) {
    const auto target = vdbPlayback_->frameIndex();
    const auto bytes = vdbAsyncSequence_ ? vdbAsyncSequence_->status().residentBytes : vdbSequence_->cachedBytes();
    json["openvdbSequence"] = {
        {"requestedFrame", target},
        {"displayedFrame", vdbLoadedFrame_.value_or(0)},
        {"asynchronous", bool(vdbAsyncSequence_)},
        {"pending", !vdbLoadedFrame_ || *vdbLoadedFrame_ != target},
        {"gpuUploadPending", vdbUploadingFrame_.has_value()},
        {"gpuUploadSubmitted", vdbUploadSubmitted_},
        {"gpuUploadPublished", vdbUploadPublished_},
        {"gpuUploadDiscarded", vdbUploadDiscarded_},
        {"residentBytes", bytes}};
  }
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
        {"areaLightingMode", lighting.areaLightingMode},
        {"areaLightingRequestedMode",
         subs_.lightingManager->lightingSettings().areaLightingMode},
        {"areaLightSampleCount", lighting.areaLightSampleCount},
        {"ltcReady", subs_.lightingManager->ltcReady()},
        {"ltcStatus", subs_.lightingManager->ltcStatus()},
        {"ltcAllocatedImageBytes", subs_.lightingManager->ltcMemoryBytes()},
        {"areaEmitterDebug",
         subs_.lightingManager->lightingSettings().areaEmitterDebug},
        {"localShadowLayerBudget",
         subs_.lightingManager->lightingSettings().localShadowLayerBudget}};
    json["lighting"]["areaLightPositions"] = nlohmann::json::array();
    for (const auto &light : subs_.lightingManager->areaLightsSsbo())
      json["lighting"]["areaLightPositions"].push_back({light.positionRange.x,
                                                        light.positionRange.y,
                                                        light.positionRange.z});
    if (subs_.shadowManager) {
      json["lighting"]["activeLocalShadowLayers"] =
          subs_.shadowManager->localShadowData().counts.x;
      const auto &local = subs_.shadowManager->localShadowData();
      uint32_t areaOrigins = 0;
      uint32_t seedTaps = 0;
      uint32_t quality =
          subs_.guiManager
              ? subs_.guiManager->shadowSettings().areaShadowQuality
              : svc_.config.areaShadowQuality;
      for (uint32_t i = 0; i < local.counts.x; ++i) {
        const auto &layer = local.layers[i];
        if (layer.directionType.w == container::gpu::kLocalShadowTypeArea &&
            layer.meta.y == 0u) {
          ++areaOrigins;
          quality = uint32_t(layer.params.w);
          seedTaps = std::max(seedTaps,
                              layer.meta.z /
                                  container::gpu::kLocalShadowPointFaceCount);
        }
      }
      json["lighting"]["areaShadowQuality"] = quality;
      json["lighting"]["areaShadowOrigins"] = areaOrigins;
      json["lighting"]["areaShadowSeedTapsPerOrigin"] =
          quality == 0 ? 0 : seedTaps;
      json["lighting"]["areaShadowBlockerTapsPerOrigin"] =
          quality == 0 ? 0 : 16;
      json["lighting"]["areaShadowFilterTapsPerOrigin"] =
          quality == 0 ? 12 : (8u << quality);
      json["lighting"]["localShadowAtlasPayloadBytes"] =
          uint64_t(container::gpu::kLocalShadowMapResolution) *
          container::gpu::kLocalShadowMapResolution *
          container::gpu::kMaxShadowedLocalLightLayers *
          (subs_.shadowManager->depthFormat() == VK_FORMAT_D32_SFLOAT_S8_UINT
               ? 8u
               : 4u);
      json["lighting"]["localShadowAtlasAllocatedBytes"] =
          subs_.shadowManager->localShadowAtlasAllocatedBytes();
      json["lighting"]["areaShadowHistoryBytes"] = 0;
    }
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
    }
  }
  if (subs_.environmentManager)
    json["environment"] = {
        {"hdrPath", container::util::pathToUtf8(subs_.environmentManager->activeHdrPath())},
        {"loading", subs_.environmentManager->environmentLoading()},
        {"generating", subs_.environmentManager->environmentGenerating()},
        {"skyMode", static_cast<uint32_t>(subs_.environmentManager->activeSkyMode())},
        {"status", subs_.environmentManager->environmentStatus()}};
  if (subs_.environmentManager && subs_.lightingManager) {
    const auto& environment = subs_.lightingManager->lightingSettings().environment;
    json["environment"]["yawDegrees"] = environment.yawDegrees;
    json["environment"]["backgroundBlur"] = environment.backgroundBlur;
    json["environment"]["solidBackground"] = environment.solidBackground;
  }
  if (subs_.rendererTelemetry) {
    const auto &snapshot = subs_.rendererTelemetry->latest();
    json["gpuKnownMs"] = snapshot.timing.gpuKnownMs;
    json["gpuProfiler"] = {
        {"available", snapshot.gpuProfiler.available},
        {"source", std::string(rendererGpuTimingSourceName(
                       snapshot.gpuProfiler.source))},
        {"resultLatencyFrames", snapshot.gpuProfiler.resultLatencyFrames},
        {"recordingSerial", snapshot.gpuProfiler.recordingSerial},
        {"status", snapshot.gpuProfiler.status}};
    json["cpuPhasesMs"] = nlohmann::json::object();
    for (size_t phase = 0; phase < snapshot.timing.cpuMs.size(); ++phase)
      json["cpuPhasesMs"][std::string(rendererTelemetryPhaseName(
          static_cast<RendererTelemetryPhase>(phase)))] =
          snapshot.timing.cpuMs[phase];
    json["frameHistory"] = nlohmann::json::array();
    for (const auto &sample : subs_.rendererTelemetry->view().history) {
      nlohmann::json phases = nlohmann::json::object();
      for (size_t phase = 0; phase < sample.cpuPhasesMs.size(); ++phase)
        phases[std::string(rendererTelemetryPhaseName(
            static_cast<RendererTelemetryPhase>(phase)))] = sample.cpuPhasesMs[phase];
      json["frameHistory"].push_back({{"frameIndex", sample.frameIndex},
                                       {"cpuPhasesMs", std::move(phases)},
                                       {"gpuKnownMs", sample.gpuKnownMs}});
    }
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
  // Count imported content rather than the diagnostic geometry appended to
  // renderer buffers, so an unloaded scene has an unambiguous zero count.
  json["scene"]["primaryObjectCount"] = sceneGraph_.renderableNodes().size();
  json["scene"]["primaryNodeCount"] = sceneGraph_.nodeCount();
  appendPhysicsTelemetry(json);
  appendGltfAnimationTelemetry(json);
  appendGltfInteractivityTelemetry(json);
  if (sceneEdits_)
    json["sceneEditHistory"] = {{"undo", sceneEdits_->undoCount()}, {"redo", sceneEdits_->redoCount()}};
  if (captureNodeGizmoTarget_ && captureNodeGizmoTarget_->first == sceneGraph_.identity()) {
    const auto node = captureNodeGizmoTarget_->second;
    if (const auto sibling = sceneGraph_.siblingIndex(node)) {
      json["nodeGizmo"] = {{"graphNode", node}, {"siblingIndex", *sibling},
          {"active", transformDragSession_.active && transformDragSession_.nodeIndex == node}};
    }
  }
  if (subs_.sceneManager) {
    auto& variants=json["gltfMaterialVariants"];
    const auto selected=subs_.sceneManager->selectedMaterialVariant();
    variants={{"selected",selected?int(*selected):-1},{"count",subs_.sceneManager->materialVariants().names().size()},
      {"assignments",nlohmann::json::array()}};
    if (sceneEdits_) { variants["undoCount"]=sceneEdits_->undoCount(); variants["redoCount"]=sceneEdits_->redoCount(); }
    for (const auto nodeIndex:sceneGraph_.renderableNodes()) {
      const auto& node=*sceneGraph_.getNode(nodeIndex);
      if (node.sourceMeshIndex<0 || node.sourcePrimitiveIndex<0) continue;
      const auto properties=subs_.sceneManager->materialRenderProperties(node.materialIndex);
      variants["assignments"].push_back({{"node",nodeIndex},{"mesh",node.sourceMeshIndex},{"primitive",node.sourcePrimitiveIndex},
        {"material",node.materialIndex},{"transparent",properties.transparent},{"alphaMasked",properties.alphaMasked},
        {"doubleSided",properties.doubleSided},{"rayCompatible",subs_.sceneManager->rayGeometryCompatible(node.materialIndex)}});
    }
  }
  appendGltfDeformationTelemetry(json);
  appendGltfMergeTelemetry(json);
  appendMaterialAuthoringTelemetry(json);
  json["scene"]["primaryVertexCount"] =
      subs_.sceneManager ? subs_.sceneManager->vertices().size() : 0u;
  json["scene"]["primaryIndexCount"] =
      subs_.sceneManager ? subs_.sceneManager->indices().size() : 0u;
  json["scene"]["auxiliaryObjectCount"] =
      subs_.bimManager ? subs_.bimManager->objectData().size() : 0u;
  json["scene"]["geometryArenaCapacityBytes"] =
      svc_.allocationManager.geometryArenaCapacityBytes();
  if (subs_.rayShadowManager) {
    auto &ray = *subs_.rayShadowManager;
    json["rayShadows"] = {{"supported", ray.supported()},
                          {"active", ray.active()},
                          {"mode", uint32_t(ray.settings().mode)},
                          {"areaSamples", ray.settings().areaSamples},
                          {"localLightBudget", ray.settings().localLightBudget},
                          {"denoise", ray.settings().denoise},
                          {"totalAllocatedBytes", ray.allocatedBytes()},
                          {"generation", ray.sceneGeneration()},
                          {"historyResets", ray.historyResets()}};
    json["rayShadows"]["opacityMicromaps"] = {
        {"requested", svc_.config.experimentalOpacityMicromaps},
        {"deviceEnabled", svc_.ctx.deviceWrapper->opacityMicromapsEnabled()}};
    if (const auto *stats = ray.buildStats())
      json["rayShadows"]["build"] = {
          {"blasBuilt", stats->blasBuilt},
          {"blasReused", stats->blasReused},
          {"gpuInstances", stats->gpuInstanceCount},
          {"gpuTransformUploadBytes", stats->gpuTransformUploadBytes},
          {"instances", stats->instanceCount},
          {"opacityMicromaps", stats->opacityMicromapCount},
          {"opacityMicromapsBuilt", stats->opacityMicromapBuilt},
          {"opacityMicromapBlas", stats->opacityMicromapAttachedBlas},
          {"opacityMicromapFallbackBlas", stats->opacityMicromapFallbackBlas},
          {"opacityMicromapBytes", stats->opacityMicromapBytes},
          {"asAllocatedBytes", stats->accelerationAllocatedBytes},
          {"inputAllocatedBytes", stats->inputAllocatedBytes},
          {"scratchAllocatedBytes", stats->scratchAllocatedBytes}};
  }
  if (subs_.ddgiManager) {
    const auto &gi = *subs_.ddgiManager;
    const auto batch = gi.lastBatch();
    const auto &settings = gi.settings();
    json["ddgi"] = {
        {"supported", gi.supported()},
        {"enabled", settings.enabled},
        {"paused", settings.paused},
        {"active", gi.active()},
        {"status", gi.status()},
        {"totalAllocatedBytes", gi.allocatedBytes()},
        {"historyResets", gi.historyResets()},
        {"updateFrame", gi.frameNumber()},
        {"contentEpoch", gi.contentEpoch()},
        {"refreshRemainingProbes", gi.refreshRemainingProbes()},
        {"updatedProbes", batch.probeCount},
        {"tracedRays", batch.rayCount},
        {"firstProbe", batch.firstProbe},
        {"nextProbe", batch.nextProbe},
        {"schedulingCursor", gi.schedulingCursor()},
        {"debugView", static_cast<uint32_t>(settings.debugView)},
        {"debugProbeRadius", settings.debugProbeRadius},
        {"settings",
         {{"origin",
           {settings.volume.origin.x, settings.volume.origin.y,
            settings.volume.origin.z}},
          {"spacing",
           {settings.volume.spacing.x, settings.volume.spacing.y,
            settings.volume.spacing.z}},
          {"counts",
           {settings.volume.counts.x, settings.volume.counts.y,
            settings.volume.counts.z}},
          {"raysPerProbe", settings.resources.updates.raysPerProbe},
          {"maxProbesPerFrame", settings.resources.updates.maxProbesPerFrame},
          {"maxRaysPerFrame", settings.resources.updates.maxRaysPerFrame},
          {"maxDistance", settings.maxDistance},
          {"hysteresis", settings.hysteresis},
          {"normalBias", settings.normalBias},
          {"viewBias", settings.viewBias},
          {"visibilityBias", settings.visibilityBias},
          {"intensity", settings.intensity}}}};
    json["ddgi"]["sceneFit"] = {
        {"applied", fittedDdgiVolume_ && *fittedDdgiVolume_ == settings.volume},
        {"boundsMin",
         {ddgiFitBoundsMin_.x, ddgiFitBoundsMin_.y, ddgiFitBoundsMin_.z}},
        {"boundsMax",
         {ddgiFitBoundsMax_.x, ddgiFitBoundsMax_.y, ddgiFitBoundsMax_.z}},
        {"probeBudget", ddgiFitProbeBudget_},
        {"probeCount", ddgiFitProbeCount_},
        {"payloadBytes", ddgiFitPayloadBytes_},
        {"status", ddgiFitStatus_}};
    json["ddgi"]["selectedProbes"] = nlohmann::json::array();
    for (const auto &update : gi.lastUpdates())
      json["ddgi"]["selectedProbes"].push_back(update.probeIndex);
    json["ddgi"]["settings"]["volumeBlendCells"] = settings.volumeBlendCells;
    json["ddgi"]["settings"]["thinGlassTransmission"] = settings.thinGlassTransmission;
    json["ddgi"]["settings"]["dielectricTransmission"] = settings.dielectricTransmission;
    json["ddgi"]["settings"]["cameraFollow"] = settings.cameraFollow;
    json["ddgi"]["settings"]["asyncCompute"] = settings.asyncCompute;
    json["ddgi"]["asyncActive"] = gi.asyncActive();
    json["ddgi"]["asyncSubmittedUpdates"] = gi.asyncSubmittedUpdates();
    json["ddgi"]["asyncPublishedUpdates"] = gi.asyncPublishedUpdates();
    const auto runtimeVolume = gi.currentPrimaryVolume();
    json["ddgi"]["scroll"] = {
        {"count", gi.scrollCount()}, {"preservedProbes", gi.lastScrollPreservedProbes()},
        {"origin", {runtimeVolume.origin.x, runtimeVolume.origin.y, runtimeVolume.origin.z}}};
    json["ddgi"]["settings"]["additionalVolumes"] = nlohmann::json::array();
    for (const auto &volume : settings.additionalVolumes)
      json["ddgi"]["settings"]["additionalVolumes"].push_back({
          {"origin", {volume.origin.x, volume.origin.y, volume.origin.z}},
          {"spacing", {volume.spacing.x, volume.spacing.y, volume.spacing.z}},
          {"counts", {volume.counts.x, volume.counts.y, volume.counts.z}}});
  }
  if (subs_.guiManager)
    json["gui"] = {{"status", subs_.guiManager->statusMessage()},
                   {"displayMode", static_cast<uint32_t>(
                                       subs_.guiManager->gBufferViewMode())},
                   {"selectedMeshNodeId", sceneState_.selectedMeshNode == container::scene::SceneGraph::kInvalidNode ? -1 : int64_t(sceneState_.selectedMeshNode)},
                   {"selectedBimObjectIndex", selectedBimObjectIndex_ == std::numeric_limits<uint32_t>::max() ? -1 : int64_t(selectedBimObjectIndex_)}};
  if (subs_.bimManager) {
    json["scene"]["auxiliaryObjectCount"] =
        subs_.bimManager->objectData().size();
    const auto bounds = subs_.bimManager->sceneProviderBounds();
    if (bounds.valid)
      json["scene"]["auxiliaryBounds"] = {
          {"min", {bounds.min.x, bounds.min.y, bounds.min.z}},
          {"max", {bounds.max.x, bounds.max.y, bounds.max.z}}};
    const auto &report = subs_.bimManager->importReport();
    json["scene"]["import"] = {
        {"status",
         container::geometry::importCompletenessName(report.completeness)},
        {"sourceProducts", report.sourceProductCount},
        {"importedProducts", report.importedProductCount},
        {"skippedProducts", report.skippedProductCount},
        {"partialProducts", report.partialProductCount},
        {"representationWarnings", report.representationWarnings},
        {"fallbackSource", report.fallbackSource}};
  }
  json["gui"]["selectedMeshNodeId"] = sceneState_.selectedMeshNode;
  const auto cancellation = modelLoad_ ? modelLoad_->worker.cancellationStats()
      : ModelLoadState::Worker::CancellationStats{};
  json["gui"]["modelLoading"] = {
        {"pending", modelLoad_ && modelLoad_->request.has_value()},
        {"requests", modelLoad_ ? modelLoad_->requests : 0u},
        {"completed", modelLoad_ ? modelLoad_->completed : 0u},
        {"cancelled", modelLoad_ ? modelLoad_->cancelled : 0u},
        {"retiredActiveRequests", cancellation.retiredActiveRequests},
        {"cooperativeExits", cancellation.cooperativeExits},
        {"lastCancellationRetirementMs", cancellation.lastRetirementMs},
        {"preparationFrames", modelLoad_ ? modelLoad_->preparationFrames : 0u},
        {"progressFrames", modelLoad_ ? modelLoad_->progressFrames : 0u},
        {"eventPumps", modelLoad_ ? modelLoad_->eventPumps : 0u}};
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
  if (svc_.config.gpuHardwareTelemetry)
    json["gpuHardware"] = gpuHardwareTelemetry(subs_.gpuHardwareMonitor->trySnapshot());
  if (svc_.config.screenReflections != ScreenReflectionMode::Off) {
    json["screenReflections"] = {
        {"mode", static_cast<uint32_t>(svc_.config.screenReflections)},
        {"allocationBytes", subs_.screenReflectionPass ? subs_.screenReflectionPass->allocationBytes() : 0},
        {"rayFallbackRequested", svc_.config.screenReflectionRayFallback},
        {"rayFallbackAvailable", subs_.screenReflectionPass && subs_.screenReflectionPass->rayFallbackAvailable()},
        {"historyAvailable", subs_.screenReflectionPass && subs_.screenReflectionPass->historyAvailable()},
        {"historyReprojected", subs_.screenReflectionPass && subs_.screenReflectionPass->historyReprojected()},
        {"rayFallbackActive", subs_.screenReflectionPass && subs_.deferredRasterFrameGraphContext &&
            subs_.deferredRasterFrameGraphContext->isPassActive(RenderPassId::ScreenReflections) &&
            subs_.screenReflectionPass->rayFallbackActive()}};
  }
  if (svc_.config.volumetricFog.mode != VolumetricFogMode::Off) {
    json["volumetricFog"] = {{"mode", static_cast<uint32_t>(svc_.config.volumetricFog.mode)},
        {"density", svc_.config.volumetricFog.density},
        {"active", subs_.volumetricFogPass && subs_.deferredRasterFrameGraphContext &&
            subs_.deferredRasterFrameGraphContext->isPassActive(RenderPassId::VolumetricFog)},
        {"historyReprojected", subs_.volumetricFogPass && subs_.volumetricFogPass->historyReprojected()},
        {"allocationBytes", subs_.volumetricFogPass ? subs_.volumetricFogPass->allocationBytes() : 0}};
  }
  return json;
}

void RendererFrontend::writeCaptureTelemetry(
    const std::filesystem::path &path, std::optional<glm::uvec2> depthProbe,
    std::optional<uint32_t> referenceSampleTarget) {
  if (referenceSampleTarget &&
      (!referencePathActive() || !subs_.referencePathTracePass ||
       subs_.referencePathTracePass->attemptedSamples() != *referenceSampleTarget))
    throw std::runtime_error("Reference accumulation changed before checkpoint telemetry");
  auto json = captureTelemetry();
  if (comfyUi_) json["comfyUi"] = comfyUiCaptureTelemetry();
  if (depthProbe) {
    auto &probe = json["depthProbe"];
    probe["pixel"] = {depthProbe->x, depthProbe->y};
    float depth = 0.0f;
    probe["opaque"] = sampleDepthAtCursor(depthProbe->x, depthProbe->y, depth)
                          ? nlohmann::json(depth) : nlohmann::json(nullptr);
    probe["pick"] = samplePickDepthAtCursor(depthProbe->x, depthProbe->y, depth)
                        ? nlohmann::json(depth) : nlohmann::json(nullptr);
    if (subs_.sceneController) {
      const auto describe = [this](uint32_t id) -> nlohmann::json {
        const auto* hit = sceneGraph_.getNode(id);
        if (!hit) return nullptr;
        const auto* source = hit;
        while (source && source->sourceNodeIndex < 0) source = sceneGraph_.getNode(source->parent);
        return {{"graphNode", id}, {"sourceNode", source ? source->sourceNodeIndex : -1},
                {"name", source ? source->name : hit->name}, {"sourceMesh", hit->sourceMeshIndex},
                {"sourcePrimitive", hit->sourcePrimitiveIndex}, {"sourceInstance", hit->sourceInstanceIndex}};
      };
      const auto routes = [&](uint32_t id) -> nlohmann::json {
        const bool visible = sceneGraph_.isNodeEffectivelyVisible(id);
        return {{"hit", describe(id)},
                {"selection", visible ? describe(sceneGraph_.nearestSelectableAncestor(id)) : nullptr},
                {"hover", visible ? describe(sceneGraph_.nearestHoverableAncestor(id)) : nullptr}};
      };
      glm::vec4 sectionPlane{0, 1, 0, 0};
      const bool sectionEnabled = currentSectionPlaneEquation(subs_.guiManager.get(), sectionPlane);
      const auto cpuHit = subs_.sceneController->pickRenderableNodeHit(buffers_.cameraData,
          svc_.swapChainManager.extent(), depthProbe->x, depthProbe->y, sectionEnabled, sectionPlane);
      auto& interaction = probe["sceneInteraction"];
      interaction["cpu"] = routes(cpuHit.hit ? cpuHit.nodeIndex : container::scene::SceneGraph::kInvalidNode);
      interaction["gpu"] = nullptr;
      interaction["rawGpuId"] = nullptr;
      uint32_t pickId = container::gpu::kPickIdNone;
      if (samplePickIdAtCursor(depthProbe->x, depthProbe->y, pickId)) {
        const auto target = decodeGpuPickId(pickId);
        const auto node = target.kind == GpuPickTargetKind::Scene
            ? subs_.sceneController->nodeIndexForObject(target.objectIndex)
            : container::scene::SceneGraph::kInvalidNode;
        interaction["gpu"] = routes(node);
        interaction["rawGpuId"] = pickId;
      }
    }
  }
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path);
  if (!stream)
    throw std::runtime_error("Cannot write capture telemetry");
  stream << json.dump(2);
  stream.close();
  if (!stream)
    throw std::runtime_error("Capture telemetry write failed");
}

void RendererFrontend::startGfxCapture() {
  const auto& capture = svc_.config.gfxrecon;
  if (!capture.enabled()) return;
  auto runtime = captureTelemetry();
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

void RendererFrontend::handleResize(std::optional<VkFormat> requestedFormat) {
  if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS)
    throw std::runtime_error("Failed to retire frames before recreating the swapchain");
  if (subs_.rendererTelemetry)
    subs_.rendererTelemetry->noteDeviceWaitIdle();

  auto& swapchain = svc_.swapChainManager;
  const VkFormat previousFormat = swapchain.imageFormat();
  const VkExtent2D previousExtent = swapchain.extent();
  const size_t previousImageCount = swapchain.imageCount();
  if (!swapchain.recreate(requestedFormat))
    return;
  if (subs_.rendererTelemetry)
    subs_.rendererTelemetry->noteSwapchainRecreate();

  updateSwapchainRendering(previousFormat);
  swapchain.createFramebuffers(resources_.renderPasses.postProcess);
  frame_.imagesInFlight.assign(swapchain.imageCount(), VK_NULL_HANDLE);
  invalidateDepthVisibilityFrames();

  const VkExtent2D extent = swapchain.extent();
  const uint32_t imageCount = static_cast<uint32_t>(swapchain.imageCount());
  // Offscreen resources, including temporal and GI histories, still match a
  // format-only replacement with the same extent and image count.
  if (previousFormat != swapchain.imageFormat() &&
      previousExtent.width == extent.width && previousExtent.height == extent.height &&
      previousImageCount == imageCount)
    return;

  destroyGBufferResources();
  subs_.gpuHierarchyPass.reset();
  gpuHierarchyPrepared_ = false;
  gpuHierarchyObjectBuffers_.clear();
  ensureCameraBuffers();
  if (subs_.lightingManager) {
    subs_.lightingManager->createDescriptorResources(imageCount);
  }
  if (subs_.shadowManager) {
    subs_.shadowManager->recreatePerFrameResources(imageCount);
  }
  if (subs_.shadowCullManager) {
    subs_.shadowCullManager->recreatePerFrameResources(imageCount,
                                                       previousImageCount == imageCount);
    std::fill(buffers_.shadowObjectDescriptorReady.begin(),
              buffers_.shadowObjectDescriptorReady.end(), false);
  }
  if (subs_.gpuCullManager) {
    subs_.gpuCullManager->recreatePerFrameResources(imageCount,
                                                    previousImageCount == imageCount);
  }
  if (subs_.exposureManager && previousImageCount != imageCount)
    subs_.exposureManager->resizeFrameResources(imageCount);
  createFrameResources();
  if (subs_.bloomManager)
    subs_.bloomManager->createTextures(extent.width, extent.height);
  svc_.commandBufferManager.reallocate(imageCount);
  if (subs_.renderPassGpuProfiler)
    subs_.renderPassGpuProfiler->recreate(imageCount);

  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }
  updateAllObjectBuffers();
  if (subs_.sceneManager && subs_.bimManager && subs_.bimManager->hasScene())
    subs_.sceneManager->updateAuxiliaryDescriptorSets(
        buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
  updateFrameDescriptorSets();
}

void RendererFrontend::updateOpenVdbSequence() {
  if (!vdbPlayback_ || !subs_.openVdbVolumeRenderer) return;
  const auto index = vdbPlayback_->frameIndex();
  if (vdbAsyncSequence_) {
    // drawFrame retires all graphics readers before this call. Capture settling
    // performs the same retirement before polling or publishing an upload.
    if (vdbUploadingFrame_) {
      if (!subs_.openVdbVolumeRenderer->volumeReplacementReady()) return;
      if (*vdbUploadingFrame_ == index) {
        if (!subs_.openVdbVolumeRenderer->publishVolumeReplacement())
          throw std::logic_error("OpenVDB upload became unavailable before publication");
        vdbLoadedFrame_ = index;
        ++vdbUploadPublished_;
      } else {
        if (!subs_.openVdbVolumeRenderer->discardVolumeReplacement())
          throw std::logic_error("OpenVDB upload became unavailable before discard");
        ++vdbUploadDiscarded_;
      }
      vdbUploadingFrame_.reset();
    }
    const auto prefetchNext = [&] {
      if (vdbPlayback_->options().paused || vdbPlayback_->options().speed <= 0.0) return;
      const auto count = vdbAsyncSequence_->manifest().frames.size();
      if (index + 1 < count) vdbAsyncSequence_->prefetch(index + 1);
      else if (vdbPlayback_->options().loop && count > 1) vdbAsyncSequence_->prefetch(0);
    };
    if (!vdbLoadedFrame_ || *vdbLoadedFrame_ != index) {
      (void)vdbAsyncSequence_->request(index);
      auto completed = vdbAsyncSequence_->tryTakeFrame();
      if (!completed) return;
      if (!completed->error.empty()) throw std::runtime_error(completed->error);
      if (completed->frameIndex != index || !completed->volume)
        throw std::logic_error("OpenVDB background loader returned an unexpected frame");
      if (!subs_.openVdbVolumeRenderer->beginReplaceVolume(0, *completed->volume))
        throw std::logic_error("OpenVDB upload is busy without a tracked pending frame");
      vdbUploadingFrame_ = index;
      ++vdbUploadSubmitted_;
      // Staging owns the copied bytes. Release this CPU lease before prefetch.
      completed.reset();
      prefetchNext();
      return;
    }
    prefetchNext();
    return;
  }
  if (!vdbSequence_) return;
  if (vdbLoadedFrame_ && *vdbLoadedFrame_ == index) return;
  // Called after in-flight frame retirement and before swapchain acquisition.
  // The renderer also retires graphics readers before replacing one medium.
  const auto& volume = vdbSequence_->frame(index);
  subs_.openVdbVolumeRenderer->replaceVolume(0, volume);
  vdbLoadedFrame_ = index;
}

void RendererFrontend::processInput(float deltaTime, bool useViewportInput) {
  if (std::isfinite(deltaTime) && deltaTime > 0.0f)
    temporalFrameMilliseconds_ = std::min(deltaTime * 1000.0f, 250.0f);
  if (std::isfinite(deltaTime) && deltaTime > 0.0f)
    pendingGltfAnimationDeltaSeconds_ += deltaTime;
  if (vdbPlayback_ && std::isfinite(deltaTime) && deltaTime > 0.0f &&
      (!vdbAsyncSequence_ || (vdbLoadedFrame_ && *vdbLoadedFrame_ == vdbPlayback_->frameIndex())))
    vdbPlayback_->advance(deltaTime);
  if (std::isfinite(deltaTime) && deltaTime > 0.0f)
    pendingPhysicsDeltaSeconds_ = deltaTime;
  if (aslPoseReplay_)
    aslPoseReplay_->tick(deltaTime, svc_.nativeWindow);
  if (!subs_.cameraController) {
    svc_.inputManager.endFrame();
    return;
  }

  // Input updates the CPU camera object only. GPU camera buffers are uploaded
  // in drawFrame() after all in-flight work has completed, so culling, depth,
  // and G-buffer passes cannot race a previous frame still reading them.
  syncCameraSelectionPivotOverride();
  subs_.cameraController->updateViewAnimation(deltaTime);
  if (!useViewportInput) {
    svc_.inputManager.endFrame();
    return;
  }
  const bool hasSelectedEditableLight =
      subs_.lightingManager &&
      subs_.lightingManager->selectedEditableLight().has_value();
  const bool hasEditableSectionPlane =
      subs_.guiManager &&
      subs_.guiManager->sectionPlaneState().enabled &&
      subs_.guiManager->sectionPlaneState().visualPlaneEditable;
  try {
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
      .clearHover = [this]() {
        updateGltfInteractivityHover(container::scene::SceneGraph::kInvalidNode);
        clearHoveredMeshNode();
      },
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
      .transformGestureCallbacks = {
          .begin = [this](container::ui::ViewportTool tool,container::ui::TransformSpace space,
                           container::ui::TransformAxis axis,bool snap) {
            finishSceneTransformGesture();
            beginLightGizmoGesture(tool,space,axis,snap);
          },
          .end = [this]() {
            endLightGizmoGesture();
            if (transformDragSession_.active) finishSceneTransformGesture();
          },
          .cancel = [this](RenderSurfaceInteractionController::TransformGestureCancelReason reason) {
            cancelLightGizmoGesture(reason);
            if (transformDragSession_.active) finishSceneTransformGesture(true);
          }},
  });
  } catch (const std::exception& error) {
    if(!hasSelectedEditableLight && !lightEditGestureActive()) throw;
    reportLightEditError(error.what());
  }
  if (transformDragSession_.active && interactionController_.state().gesture !=
      container::ui::ViewportGesture::TransformDrag)
    finishSceneTransformGesture();
}

void RendererFrontend::selectBimObject(uint32_t objectIndex,
                                      std::optional<glm::vec3> anchorPoint) {
  if (!subs_.bimManager || !subs_.bimManager->metadataForObject(objectIndex))
    return;
  finishEditGestures();
  sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
  selectedBimObjectIndex_ = objectIndex;
  if (subs_.lightingManager)
    subs_.lightingManager->selectEditableLight({});
  selectionNavigationAnchor_ = {};
  if (anchorPoint && isFiniteVec3(*anchorPoint)) {
    const BimElementBounds bounds =
        subs_.bimManager->elementBoundsForObject(objectIndex);
    selectionNavigationAnchor_ = SelectionNavigationAnchor{
        .valid = true, .point = *anchorPoint,
        .radius = bounds.valid ? std::max(bounds.radius, 0.25f) : 1.0f,
        .sceneNode = container::scene::SceneGraph::kInvalidNode,
        .bimObject = objectIndex};
  }
  clearHoveredMeshNode();
  selectedDrawCommands_.clear();
  selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear();
  selectedBimNativeCurveDrawCommands_.clear();
  if (subs_.guiManager) {
    subs_.guiManager->setSectionPlaneVisualEditable(false);
    subs_.guiManager->setStatusMessage(
        bimSelectionLabel(*subs_.bimManager->metadataForObject(objectIndex)));
  }
}

void RendererFrontend::selectMeshNodeAtCursor(double cursorX, double cursorY) {
  if (!subs_.sceneController) {
    return;
  }
  finishEditGestures();

  auto selectEditableLight = [&](EditableLightId id) {selectLightForEditing(id);};

  auto selectSceneNode = [&](uint32_t nodeIndex,
                             std::optional<glm::vec3> anchorPoint =
                                 std::nullopt) {
    // Both GPU IDs and CPU rays resolve the nearest geometric hit first.
    // Reference parity routes an excluded subtree to its eligible ancestor.
    nodeIndex = sceneGraph_.nearestSelectableAncestor(nodeIndex);
    if (nodeIndex == container::scene::SceneGraph::kInvalidNode) {
      clearSelectedMeshNode();
      return;
    }
    sceneState_.selectedMeshNode = nodeIndex;
    queueGltfInteractivitySelection(nodeIndex, anchorPoint);
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
    updateGltfInteractivityHover(container::scene::SceneGraph::kInvalidNode);
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
      hoverPickCache_.sceneGraphRevision == sceneGraph_.revision() &&
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
      .sceneGraphRevision = sceneGraph_.revision(),
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

  hoveredNode = sceneGraph_.nearestHoverableAncestor(hoveredNode);
  updateGltfInteractivityHover(hoveredNode);
  if (hoveredNode == sceneState_.selectedMeshNode) {
    hoveredNode = container::scene::SceneGraph::kInvalidNode;
  }
  if (hoveredBimObject != std::numeric_limits<uint32_t>::max() &&
      selectedBimObjectIndex_ != std::numeric_limits<uint32_t>::max() &&
      subs_.bimManager) {
    const auto selectedMetadata =
        subs_.bimManager->metadataForObject(selectedBimObjectIndex_);
    const auto hoveredMetadata =
        subs_.bimManager->metadataForObject(hoveredBimObject);
    if (selectedMetadata && hoveredMetadata &&
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

  finishEditGestures();
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
        .historyGesture = ++nextViewportEditGesture_,
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
  glm::vec3 translationDelta{0.0f};
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
      translationDelta = axisVector * projectedAxisDragAmount(axisVector, dragScale);
      break;
    }

    if (transformingSectionPlane) {
      const glm::vec3 normal =
          normalizedOr(transformDragSession_.startSectionPlane.normal,
                       {0.0f, 1.0f, 0.0f});
      translationDelta = normal * static_cast<float>(dragDeltaX - dragDeltaY) * dragScale;
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

    translationDelta = (horizontal * static_cast<float>(dragDeltaX) +
                        vertical * static_cast<float>(dragDeltaY)) * dragScale;
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
      if (factor != 1.0f)
        controls.scale = container::ui::clampSignedScale(controls.scale * factor);
    } else {
      auto applyScaleAxis = [&](float &component) {
        if (factor != 1.0f)
          component = container::ui::clampSignedScale(component * factor);
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
      if (axis == container::ui::TransformAxis::Free) {
        translationDelta = snapRelativeVec3(glm::vec3(0.0f), translationDelta, 0.25f);
      } else {
        const auto worldAxis = transformAxisVector();
        translationDelta = worldAxis * (std::round(glm::dot(translationDelta, worldAxis) / 0.25f) * 0.25f);
      }
      break;
    case container::ui::ViewportTool::Rotate:
      controls.rotationDegrees =
          snapRelativeVec3(transformDragSession_.startControls.rotationDegrees,
                           controls.rotationDegrees, 15.0f);
      break;
    case container::ui::ViewportTool::Scale:
      controls.scale =
          container::ui::snapSignedScale(transformDragSession_.startControls.scale,
                                         controls.scale, 0.1f);
      break;
    case container::ui::ViewportTool::Select:
      break;
    }
  }

  if (tool == container::ui::ViewportTool::Translate) {
    if (const auto* node = sceneGraph_.getNode(selectedNode)) {
      if (const auto* parent = sceneGraph_.getNode(node->parent)) {
        // The gizmo axes and camera vectors are in world space. Convert the
        // displacement to the coordinates used by the node's local transform.
        const glm::mat3 parentBasis(parent->worldTransform);
        const float determinant = glm::determinant(parentBasis);
        if (!std::isfinite(determinant) || determinant == 0.0f) return;
        translationDelta = glm::inverse(parentBasis) * translationDelta;
      }
    }
    controls.position += translationDelta;
    if (!isFiniteVec3(controls.position)) return;
  }

  if (transformingEditableLight && subs_.lightingManager) {
    try {
      if (!lightEditGestureActive()) beginLightGizmoGesture(tool, space, axis, snapEnabled);
      (void)applyLightGizmoControls(tool, axis, controls);
    } catch (const std::exception& error) {
      reportLightEditError(error.what());
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

  if (!applySceneNodeTransform(sceneState_.selectedMeshNode, controls))
    return;
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

bool RendererFrontend::referencePathActive() const {
  return subs_.activeTechnique && subs_.activeTechnique->id() == RenderTechniqueId::PathTracing;
}

void RendererFrontend::requestScreenshot(std::filesystem::path outputPath, bool viewportOnly,
                                         std::optional<uint32_t> referenceSampleTarget) {
  if (outputPath.empty()) {
    throw std::runtime_error("screenshot output path is empty");
  }
  auto extension = container::util::pathToUtf8(outputPath.extension());
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const bool sceneLinear = extension == ".exr";
  if (!sceneLinear && extension != ".png")
    throw std::runtime_error("screenshot path must end in .png or .exr");
  if (referenceSampleTarget &&
      (!sceneLinear || !referencePathActive() || *referenceSampleTarget == 0 ||
       *referenceSampleTarget > svc_.config.pathSampleLimit))
    throw std::runtime_error("Reference sample checkpoints require path tracing, EXR and a valid sample count");
  if (sceneLinear &&
      frontendDisplayMode(subs_.guiManager.get(), configuredDisplayMode(svc_.config)) !=
          container::ui::GBufferViewMode::Lit)
    throw std::runtime_error("select Lit view before exporting a scene-linear EXR");
  if (!sceneLinear)
    requireSwapchainPngSupport(svc_.swapChainManager);

  screenshot_.outputPath = std::move(outputPath);
  screenshot_.referenceSampleTarget = referenceSampleTarget;
  screenshot_.sceneLinear = sceneLinear;
  screenshot_.viewportOnly = viewportOnly && !sceneLinear;
  screenshot_.interactive = false;
  screenshot_.remote = false;
  screenshot_.pending = true;
}

bool RendererFrontend::reloadSceneModel(const std::string &path,
                                        float importScale) {
  if (!subs_.sceneController || !subs_.sceneManager)
    return false;
  finishEditGestures();

  ensureObjectBuffers();
  if (buffers_.objects.empty() || buffers_.objectCapacities.empty())
    return false;

  const auto cameraBuffer = buffers_.cameras.empty()
                                ? container::gpu::AllocatedBuffer{}
                                : buffers_.cameras.front();
  const uint64_t previousGraphIdentity = sceneGraph_.identity();
  auto reloadPrimary = [&](const std::string &modelPath, float scale) {
    auto *prepared = modelLoad_ && modelLoad_->committing && modelLoad_->ready &&
                             modelLoad_->request && modelLoad_->request->path == modelPath &&
                             modelLoad_->ready->gltf
                         ? &*modelLoad_->ready->gltf : nullptr;
    return subs_.sceneController->reloadSceneModel(
        modelPath, scale, buffers_.objects.front(),
        buffers_.objectCapacities.front(),
        cameraBuffer, sceneState_.indexType, sceneState_.rootNode,
        sceneState_.selectedMeshNode, sceneState_.cubeNode, prepared);
  };
  auto refreshSceneState = [&](bool replacedModel) {
    // reloadPrimary can replace the first object buffer before
    // updateObjectBuffer sees it. Rebind shadow-cull sets even when the new
    // scene fits the already-created buffer capacity.
    std::fill(buffers_.shadowObjectDescriptorReady.begin(),
              buffers_.shadowObjectDescriptorReady.end(), false);
    syncSceneStateFromController();
    retireBindingsAfterSceneReload(sceneGraph_, previousGraphIdentity,
                                   [this] { clearPhysicsBindings(); });
    if (replacedModel) {
      materialGesture_.reset();lastEditedSourceMaterial_=-1;materialInspectorBinding_=UINT32_MAX;
      clearSelectedMeshNode();
      clearHoveredMeshNode();
      captureObjectBase_.reset();
      transformDragSession_.active = false;
      if (subs_.guiManager)
        subs_.guiManager->resetModelInspectionState();
      if (subs_.ddgiManager)
        subs_.ddgiManager->resetSceneHistory();
      applySceneLightingDefaults();
    }
    if (subs_.lightingManager) {
      subs_.lightingManager->setRootNode(sceneState_.rootNode);
      if (replacedModel)
        subs_.lightingManager->clearSceneLightOverrides();
      subs_.lightingManager->updateLightingData();
      subs_.lightingManager->createLightVolumeGeometry();
    }
    if (replacedModel) {
      resetCameraForActiveScene();
      for (uint32_t imageIndex = 0;
           imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
           ++imageIndex) {
        updateCameraBuffer(imageIndex);
      }
    }
    updateAllObjectBuffers();
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
    const auto previousOpenVdbOptions = subs_.bimManager ? subs_.bimManager->openVdbOptions() : nullptr;

    if (!subs_.bimManager) {
      subs_.bimManager = std::make_unique<BimManager>(
          svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
      subs_.bimManager->createMeshletResidencyResources(
          container::util::executableDirectory());
      if (modelLoad_ && modelLoad_->committing)
        subs_.bimManager->setLoadProgressCallback([this](std::string_view stage) {
          pumpModelLoadProgress(stage);
        });
    }

    bool retiredPreviousScene = false;
    try {
      // Parse and validate CPU geometry before replacing either live provider.
      // A bad BIM path must preserve glTF edits and existing material indices.
      std::optional<BimManager::PreparedScene> localPrepared;
      BimManager::PreparedScene *prepared = nullptr;
      if (modelLoad_ && modelLoad_->committing && modelLoad_->ready && modelLoad_->ready->bim)
        prepared = &*modelLoad_->ready->bim;
      else {
        const auto requestedOptions=modelLoad_ && modelLoad_->request
            ? modelLoad_->request->openVdbOptions : nullptr;
        localPrepared = BimManager::prepareScene(path, importScale,
            vdbOptionsForReload(path,requestedOptions,previousAuxiliaryPath,
                                previousOpenVdbOptions,svc_.config.vdbModelOptions));
        prepared = &*localPrepared;
      }
      retiredPreviousScene = true;
      if (!reloadPrimary("", 1.0f))
        throw std::runtime_error("Failed to clear the primary scene");
      subs_.bimManager->loadPreparedScene(std::move(*prepared), *subs_.sceneManager);
    } catch (const std::exception &error) {
      const std::string loadError = error.what();
      container::log::ContainerLogger::instance().renderer()->error(
          "Failed to load model '{}': {}", path, loadError);
      if (retiredPreviousScene) {
        subs_.bimManager->clear();
        if (!reloadPrimary(previousPrimaryPath, previousPrimaryScale)) {
          activePrimaryModelPath_.clear();
          activePrimaryImportScale_ = 1.0f;
        }
        if (!previousAuxiliaryPath.empty()) {
          try {
            subs_.bimManager->loadModel(previousAuxiliaryPath,
                                        previousAuxiliaryScale,
                                      *subs_.sceneManager, previousOpenVdbOptions);
          } catch (const std::exception &restoreError) {
            container::log::ContainerLogger::instance().renderer()->error(
                "Failed to restore auxiliary model '{}': {}",
                previousAuxiliaryPath, restoreError.what());
            subs_.bimManager->clear();
            activeAuxiliaryModelPath_.clear();
            activeAuxiliaryImportScale_ = 1.0f;
            clearSelectedMeshNode();
          }
        }
      }
      if (retiredPreviousScene)
        refreshSceneState(false);
      if (subs_.guiManager) {
        subs_.guiManager->setStatusMessage("Failed to load model: " + path +
                                           "\n" + loadError);
      }
      return false;
    }

    activePrimaryModelPath_.clear();
    activePrimaryImportScale_ = 1.0f;
    activeAuxiliaryModelPath_ = path;
    activeAuxiliaryImportScale_ = importScale;
    refreshSceneState(true);
    if (subs_.guiManager) {
      const auto summary = subs_.bimManager->importReport().summary();
      subs_.guiManager->setStatusMessage(
          "Loaded model: " + path + (summary.empty() ? "" : "\n" + summary));
    }
    return subs_.bimManager->hasScene();
  }

  const uint64_t previousGeometryRevision =
      subs_.sceneManager->geometryRevision();
  const bool result = reloadPrimary(path, importScale);
  if (result) {
    activePrimaryModelPath_ = path;
    activePrimaryImportScale_ = importScale;
    activeAuxiliaryModelPath_.clear();
    activeAuxiliaryImportScale_ = 1.0f;

    if (subs_.bimManager) {
      subs_.bimManager->clear();
    }
  } else if (subs_.sceneManager->geometryRevision() != previousGeometryRevision &&
             subs_.bimManager && !activeAuxiliaryModelPath_.empty()) {
    // SceneManager rebuilds its shared material/texture table on rollback.
    // Recreate the retained auxiliary scene against that restored table before
    // exposing its draw commands again.
    const auto previousOpenVdbOptions = subs_.bimManager->openVdbOptions();
    subs_.bimManager->clear();
    try {
      subs_.bimManager->loadModel(activeAuxiliaryModelPath_,
                                  activeAuxiliaryImportScale_,
                                  *subs_.sceneManager, previousOpenVdbOptions);
    } catch (const std::exception &error) {
      container::log::ContainerLogger::instance().renderer()->error(
          "Failed to restore auxiliary model '{}': {}",
          activeAuxiliaryModelPath_, error.what());
      subs_.bimManager->clear();
      activeAuxiliaryModelPath_.clear();
      activeAuxiliaryImportScale_ = 1.0f;
      clearSelectedMeshNode();
      clearHoveredMeshNode();
    }
  }

  refreshSceneState(result);
  if (subs_.guiManager)
    subs_.guiManager->setStatusMessage(result ? "Loaded model: " + path
                                              : "Failed to load model: " + path);
  return result;
}

void RendererFrontend::processPendingGuiModelLoadRequest() {
  auto queuedRequest=subs_.guiManager?subs_.guiManager->consumeModelLoadRequest()
      : std::optional<container::ui::ModelLoadRequest>{};
  bool remote = false;
  if (!queuedRequest && modelLoad_ && modelLoad_->queued) {
    queuedRequest = std::exchange(modelLoad_->queued, {});
    remote = std::exchange(modelLoad_->queuedRemote, false);
  }
  if (queuedRequest) {
    finishEditGestures();
    queuedRequest->openVdbOptions=vdbOptionsForReload(queuedRequest->path,queuedRequest->openVdbOptions,
        activeAuxiliaryModelPath_,subs_.bimManager ? subs_.bimManager->openVdbOptions() : nullptr,
        svc_.config.vdbModelOptions);
    if (!modelLoad_) modelLoad_ = std::make_unique<ModelLoadState>();
    if (modelLoad_->request) {
      if (modelLoad_->remote) mcpModelStatus_ = "Superseded by a newer model request";
      if(modelLoad_->request->merge) {
        mergeRequest_.outcome=MergeRequestOutcome::Failed;
        mergeRequest_.error="Cancelled by a newer model request";
      }
      ++modelLoad_->cancelled;
      ++modelLoad_->completed;
    }
    modelLoad_->worker.cancel();
    if (modelLoad_->ready) modelLoad_->worker.discard(std::move(modelLoad_->ready));
    modelLoad_->request = std::move(queuedRequest);
    modelLoad_->mergeDestination.reset();
    modelLoad_->remote = remote;
    ++modelLoad_->requests;
    const auto &action = *modelLoad_->request;
    if(action.merge) {
      if (subs_.sceneManager)
        modelLoad_->mergeDestination = ModelLoadState::MergeDestination{
            sceneGraph_.identity(), subs_.sceneManager->selectedScene()};
      ++mergeRequest_.requestId;
      mergeRequest_.outcome=MergeRequestOutcome::Pending;
      mergeRequest_.error.clear();
      mergeRequest_.parentName=action.merge->parentName;
      ++mergeRequest_.sourceReadCount;
    }
    modelLoad_->stage = action.unload ? "Unloading models" : "Reading and preparing model";
    if(subs_.guiManager) subs_.guiManager->setStatusMessage(modelLoad_->stage + ": " +
        (action.label.empty() ? action.path : action.label));
    if (action.unload || action.path.empty() ||
        action.path == container::app::kDefaultSceneModelToken) {
      modelLoad_->ready = std::make_unique<ModelLoadState::Prepared>();
    } else {
      (void)modelLoad_->worker.start([path = action.path, scale = action.importScale,
                                    openVdbOptions = action.openVdbOptions,
                                    merge = action.merge.has_value(),
                                    linked = svc_.config.experimentalGltfLinkedAssets && !remote,
                                    micromaps = svc_.ctx.deviceWrapper->opacityMicromapsEnabled() && !remote,
                                    experimental = svc_.config.experimentalGltfMaterials, remote](std::stop_token stop) {
        auto prepared = std::make_unique<ModelLoadState::Prepared>();
        if (isAuxiliaryRenderModelPath(path))
          prepared->bim = BimManager::prepareScene(path, scale, openVdbOptions, stop);
        else
          prepared->gltf = container::scene::SceneManager::prepareGltfModel(path,
              merge || remote ? 512ull * 1024 * 1024 : std::numeric_limits<uintmax_t>::max(),
              linked, remote, experimental, micromaps && !merge, merge, stop);
        return prepared;
      });
      // Keep at least the request frame interactive, even for tiny models.
      return;
    }
  }
  if (!modelLoad_ || !modelLoad_->request) return;
  auto &state = *modelLoad_;
  const auto request = *state.request;
  const std::string statusLabel = request.label.empty() ? request.path : request.label;
  try {
    // The active scene can change while the CPU worker is preparing the source.
    if (request.merge && (!state.mergeDestination || !subs_.sceneManager ||
        sceneGraph_.identity() != state.mergeDestination->graphIdentity ||
        subs_.sceneManager->selectedScene() != state.mergeDestination->scene)) {
      state.worker.cancel();
      ++state.cancelled;
      throw std::runtime_error("The import destination is no longer active. "
                               "Select the destination scene and import again.");
    }
    if (!state.ready) {
      auto completion = state.worker.tryTake();
      if (!completion) return;
      if (const auto *error = std::get_if<std::exception_ptr>(&completion->outcome))
        std::rethrow_exception(*error);
      state.ready = std::move(std::get<std::unique_ptr<ModelLoadState::Prepared>>(completion->outcome));
    }
    state.committing = true;
    state.lastProgressFrame = {};
    pumpModelLoadProgress(request.unload ? "Releasing scene resources" : "Installing prepared model");
    svc_.allocationManager.setLoadProgressCallback([this](bool render) {
      pumpModelLoadProgress(modelLoad_->stage, render);
    });
    if (subs_.bimManager)
      subs_.bimManager->setLoadProgressCallback([this](std::string_view stage) {
        pumpModelLoadProgress(stage);
      });
    bool success{};
    if (request.merge) {
      container::geometry::gltf::GltfMergeOptions options;
      options.destinationScene=state.mergeDestination->scene;
      options.parentName=request.merge->parentName;
      if (request.merge->incomingScene)
        options.incomingScene=static_cast<size_t>(*request.merge->incomingScene);
      std::array<double,16> placement;
      for (size_t column=0; column<4; ++column)
        for (size_t row=0; row<4; ++row)
          placement[column*4+row]=request.merge->placementMatrix[column][row];
      options.placementMatrix=request.merge->authoredPlacementMatrix
          ? request.merge->authoredPlacementMatrix : std::optional{placement};
      success=mergeSceneModel(request.path,options,
          state.ready->gltf ? &*state.ready->gltf : nullptr);
      mergeRequest_.outcome=success?MergeRequestOutcome::Committed:MergeRequestOutcome::Failed;
      if (success && subs_.guiManager) subs_.guiManager->setStatusMessage("Merged model: "+statusLabel);
    } else success=reloadSceneModel(request.unload ? "" : request.path,
        request.unload ? 1.0f : request.importScale);
    if (state.remote) mcpModelStatus_ = success ? "Loaded" : "Model import failed";
    if (success && request.unload && subs_.guiManager)
      subs_.guiManager->setStatusMessage("Unloaded models.");
    if (!success && subs_.guiManager && subs_.guiManager->statusMessage().empty())
      subs_.guiManager->setStatusMessage("Failed to load model: " + statusLabel);
  } catch (const std::exception &e) {
    if (state.remote) mcpModelStatus_ = std::string("Model import failed: ") + e.what();
    if(request.merge) { mergeRequest_.outcome=MergeRequestOutcome::Failed; mergeRequest_.error=e.what(); }
    container::log::ContainerLogger::instance().renderer()->error(
        "Failed to load model '{}': {}", request.path, e.what());
    if(subs_.guiManager) subs_.guiManager->setStatusMessage("Failed to load model: " + statusLabel + "\n" + e.what());
  } catch (...) {
    if (state.remote) mcpModelStatus_ = "Model import failed: unknown error";
    if(request.merge) { mergeRequest_.outcome=MergeRequestOutcome::Failed; mergeRequest_.error="Unknown scene merge error"; }
    if(subs_.guiManager) subs_.guiManager->setStatusMessage("Failed to load model: " + statusLabel + " (unknown error)");
  }
  svc_.allocationManager.setLoadProgressCallback({});
  if (subs_.bimManager) subs_.bimManager->setLoadProgressCallback({});
  state.committing = false;
  if (state.ready) state.worker.discard(std::move(state.ready));
  state.request.reset();
  state.mergeDestination.reset();
  state.remote = false;
  ++state.completed;
  // Acquired images/semaphores and reset submission fences cannot be recovered
  // as a bad model request. Propagate presentation errors after callback cleanup.
  if (state.presentationFailure)
    std::rethrow_exception(state.presentationFailure);
}

void RendererFrontend::settlePendingModelLoads() {
  processPendingGuiModelLoadRequest();
  while (modelLoad_ && modelLoad_->request) {
    pumpModelLoadProgress("Reading and preparing model");
    processPendingGuiModelLoadRequest();
    if (modelLoad_->request) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!vdbAsyncSequence_ || !vdbPlayback_ ||
      (svc_.config.screenshotCapturePath.empty() && svc_.config.temporalCaptureSequencePath.empty()))
    return;
  // Capture polling runs outside drawFrame's normal before-acquire retirement.
  // No frame submissions occur during this loop, so one retirement covers every
  // descriptor publication until the requested sequence frame is settled.
  subs_.frameSyncManager->waitForAllFrames();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!vdbLoadedFrame_ || *vdbLoadedFrame_ != vdbPlayback_->frameIndex()) {
    // All graphics readers were retired before entering this polling loop.
    updateOpenVdbSequence();
    if (vdbLoadedFrame_ && *vdbLoadedFrame_ == vdbPlayback_->frameIndex()) break;
    if (svc_.nativeWindow) {
      glfwPollEvents();
      if (glfwWindowShouldClose(svc_.nativeWindow))
        throw std::runtime_error("OpenVDB capture cancelled while waiting for a sequence frame");
    }
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("OpenVDB capture timed out waiting for background frame " +
                               std::to_string(vdbPlayback_->frameIndex()));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void RendererFrontend::pumpModelLoadProgress(std::string_view stage, bool render) {
  if (!modelLoad_) return;
  if (modelLoad_->presentationFailure)
    std::rethrow_exception(modelLoad_->presentationFailure);
  modelLoad_->stage = std::string(stage);
  // GLFW event processing stays on the application thread, including during
  // uploads and CPU destruction. Never inspect partially replaced scene data.
  if (svc_.nativeWindow) {
    glfwPollEvents();
    ++modelLoad_->eventPumps;
  }
  const auto now = TelemetryClock::now();
  if (!render || now - modelLoad_->lastProgressFrame < std::chrono::milliseconds(33))
    return;
  modelLoad_->lastProgressFrame = now;
  try {
    drawModelLoadProgressFrame();
  } catch (...) {
    modelLoad_->presentationFailure = std::current_exception();
    throw;
  }
}

void RendererFrontend::drawModelLoadProgressFrame() {
  if (!subs_.guiManager || !modelLoad_ || !modelLoad_->request ||
      (svc_.nativeWindow && glfwWindowShouldClose(svc_.nativeWindow))) return;
  int width = 0, height = 0;
  if (svc_.nativeWindow) glfwGetFramebufferSize(svc_.nativeWindow, &width, &height);
  if (width <= 0 || height <= 0) return;
  subs_.frameSyncManager->waitForAllFrames();
  uint32_t imageIndex = 0;
  // A resized/minimized surface is recovered by the regular frame loop after
  // commit. Bounded acquire keeps event processing alive in the meantime.
  const auto acquire = svc_.swapChainManager.acquire(
      1000000, frame_.currentFrame,
      subs_.frameSyncManager->imageAvailable(frame_.currentFrame), imageIndex);
  if (acquire == VK_TIMEOUT || acquire == VK_NOT_READY || acquire == VK_ERROR_OUT_OF_DATE_KHR)
    return;
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
    throw std::runtime_error("Failed to acquire loading view");
  if (subs_.ddgiManager) subs_.ddgiManager->retireImage(imageIndex);
  const auto command = svc_.commandBufferManager.buffer(imageIndex);
  if (vkResetCommandBuffer(command, 0) != VK_SUCCESS)
    throw std::runtime_error("Failed to reset loading view commands");
  VkCommandBufferBeginInfo commandBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  if (vkBeginCommandBuffer(command, &commandBegin) != VK_SUCCESS)
    throw std::runtime_error("Failed to begin loading view commands");
  const auto extent = svc_.swapChainManager.extent();
  VkClearValue clear{};
  clear.color = {{0.045f, 0.055f, 0.075f, 1.0f}};
  RenderingBeginInfo begin{};
  begin.renderPass = resources_.renderPasses.postProcess;
  begin.framebuffer = svc_.swapChainManager.framebuffers()[imageIndex];
  begin.renderArea = {{0, 0}, extent};
  begin.clearValueCount = 1;
  begin.pClearValues = &clear;
  beginDynamicRendering(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
  subs_.guiManager->startFrame();
  const auto &request = *modelLoad_->request;
  subs_.guiManager->drawModelLoadingProgress(
      request.label.empty() ? request.path : request.label,
      request.unload, modelLoad_->stage);
  subs_.guiManager->render(command);
  endDynamicRendering(command);
  if (vkEndCommandBuffer(command) != VK_SUCCESS)
    throw std::runtime_error("Failed to end loading view commands");
  VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = subs_.frameSyncManager->imageAvailable(frame_.currentFrame);
  wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = svc_.swapChainManager.renderFinishedForImage(imageIndex);
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  commandInfo.commandBuffer = command;
  VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = 1;
  submit.pWaitSemaphoreInfos = &wait;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &commandInfo;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signal;
  subs_.frameSyncManager->resetFence(frame_.currentFrame);
  const auto fence = subs_.frameSyncManager->fence(frame_.currentFrame);
  if (vkQueueSubmit2(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submit, fence) != VK_SUCCESS)
    throw std::runtime_error("Failed to submit loading view");
  frame_.imagesInFlight[imageIndex] = fence;
  const auto present = svc_.swapChainManager.present(svc_.ctx.deviceWrapper->presentQueue(),
      imageIndex);
  if (present != VK_SUCCESS && present != VK_SUBOPTIMAL_KHR && present != VK_ERROR_OUT_OF_DATE_KHR)
    throw std::runtime_error("Failed to present loading view");
  // Keep loading-view submissions out of scene temporal/history counters.
  frame_.currentFrame = (frame_.currentFrame + 1) % svc_.config.maxFramesInFlight;
  ++modelLoad_->progressFrames;
  if (!modelLoad_->committing) ++modelLoad_->preparationFrames;
}

void RendererFrontend::shutdown() {
#ifdef CONTAINER_ENABLE_RENDERER_MCP
  subs_.mcpServer.reset();
#endif
  if (shaderReloadJob_.valid()) {
    shaderReloadStop_.request_stop();
    shaderReloadJob_.wait();
    shaderReloadJob_ = {};
  }
  // Drop the unfinished gesture before histories and their asset services.
  materialGesture_.reset();
  clearPhysicsBindings();
#if CONTAINER_HAS_JOLT
  physicsScene_.reset();
#endif
  // Import jobs own only CPU payloads; join before releasing scene services.
  modelLoad_.reset();
  meshProviderAssetCache_.clear();
  if (!svc_.ctx.deviceWrapper)
    return;

  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());
  svc_.swapChainManager.drainAcquires();

  // History may own inactive scene generations and descriptor pools. Release
  // them while their allocator/pipeline services and live managers still exist.
  sceneEdits_.reset();
  for (auto& objectBuffer : buffers_.objects)
    if (subs_.sceneController && subs_.sceneController->ownsObjectBuffer(objectBuffer.buffer))
      objectBuffer = {};

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
  subs_.screenReflectionPass.reset();
  subs_.volumetricFogPass.reset();
  subs_.gltfDeformationPass.reset();
  subs_.gpuHierarchyPass.reset();
  subs_.openVdbVolumeRenderer.reset();
  vdbUploadingFrame_.reset();
  vdbAsyncSequence_.reset();
  vdbPlayback_.reset();
  vdbSequence_.reset();
  subs_.ddgiManager.reset();
  subs_.referencePathTracePass.reset();
  subs_.rayShadowManager.reset();
  subs_.gpuRayInstancePass.reset();
  destroyGBufferResources();
  subs_.frameResourceManager.reset();

  shutdownComfyUi();
  if (subs_.guiManager) {
    subs_.guiManager->shutdown(svc_.ctx.deviceWrapper->device());
    subs_.guiManager.reset();
  }
  subs_.rendererTelemetry.reset();
  subs_.gpuHardwareMonitor.reset();

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
  subs_.renderPassManager->create(
      svc_.swapChainManager.imageFormat(),
      resources_.gBufferFormats.depthStencil,
      resources_.gBufferFormats.sceneColor, resources_.gBufferFormats.albedo,
      resources_.gBufferFormats.normal, resources_.gBufferFormats.material,
      resources_.gBufferFormats.emissive, resources_.gBufferFormats.specular,
      resources_.gBufferFormats.pickId, resources_.gBufferFormats.materialLayers,
      resources_.gBufferFormats.materialSheen, msaaSampleCount_,
      msaaDepthStencilResolve_);
  resources_.renderPasses = subs_.renderPassManager->passes();
}

void RendererFrontend::createGraphicsPipelines() {
  resources_.builtPipelines = buildGraphicsPipelines(
      activeShaderReload_ ? activeShaderReload_->directory : container::util::executableDirectory(),
      resources_.renderPasses.postProcess);
  ++presentationPipelineGeneration_;
}

PipelineBuildResult RendererFrontend::buildGraphicsPipelines(
    const std::filesystem::path& shaderDirectory,
    RenderingPassHandle postProcessPass) {
  if (!subs_.pipelineBuilder) {
    subs_.pipelineBuilder = std::make_unique<GraphicsPipelineBuilder>(
        svc_.ctx.deviceWrapper, svc_.pipelineManager);
  }
  const PipelineDescriptorLayouts descLayouts{
      subs_.sceneManager->descriptorSetLayout(),
      subs_.frameResourceManager->lightingLayout(),
      subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.lightingManager->lightGizmoIconDescriptorSetLayout(),
      // Keep the tiled pipeline layout compatible across viewport resizes.
      subs_.lightingManager->tiledDescriptorSetLayout() != VK_NULL_HANDLE
          ? subs_.lightingManager->tiledDescriptorSetLayout()
          : subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.shadowManager->descriptorSetLayout(),
      subs_.frameResourceManager->postProcessLayout(),
      subs_.frameResourceManager->oitLayout(),
      subs_.rayShadowManager ? subs_.rayShadowManager->traceDescriptorLayout()
                             : VK_NULL_HANDLE,
      subs_.ddgiManager ? subs_.ddgiManager->descriptorLayout() : VK_NULL_HANDLE};
  const PipelineRenderPasses rp{resources_.renderPasses.depthPrepass,
                                resources_.renderPasses.bimDepthPrepass,
                                resources_.renderPasses.gBuffer,
                                resources_.renderPasses.bimGBuffer,
                                resources_.renderPasses.transparentPick,
                                resources_.renderPasses.shadow,
                                resources_.renderPasses.lighting,
                                resources_.renderPasses.transformGizmos,
                                postProcessPass,
                                resources_.renderPasses.forwardLighting};
  auto result = subs_.pipelineBuilder->build(shaderDirectory, descLayouts, rp, msaaSampleCount_);
  result.pipelines.layoutRegistry = result.layouts.layoutRegistry;
  return result;
}

void RendererFrontend::updateSwapchainRendering(VkFormat previousFormat) {
  const auto format = svc_.swapChainManager.imageFormat();
  const auto imageCount = static_cast<uint32_t>(svc_.swapChainManager.imageCount());
  const auto updateGui = [&](RenderingPassHandle pass) {
    if (subs_.guiManager) {
      if (subs_.guiManager->swapchainImageCount() != imageCount)
        retireComfyUiPreview();
      subs_.guiManager->updateSwapchainRendering(pass, imageCount);
    }
  };
  if (previousFormat == format) {
    updateGui(resources_.renderPasses.postProcess);
    return;
  }

  const auto shaderDirectory = activeShaderReload_ ? activeShaderReload_->directory
                                                  : container::util::executableDirectory();
  auto pass = subs_.renderPassManager->preparePostProcess(format);
  auto ddgiDebug = subs_.ddgiManager
      ? subs_.ddgiManager->prepareDebugReload(shaderDirectory, format) : nullptr;
  // Rebuild the graphics bundle as one unit. Post-process and its overlays
  // share pipeline layouts and the per-technique handle registry.
  auto candidate = buildGraphicsPipelines(shaderDirectory, pass.get());
  try {
    updateGui(pass.get());
  } catch (...) {
    subs_.pipelineBuilder->destroy(candidate);
    throw;
  }
  if (subs_.ddgiManager)
    subs_.ddgiManager->publishDebugReload(std::move(ddgiDebug));
  subs_.renderPassManager->publishPostProcess(std::move(pass));
  resources_.renderPasses = subs_.renderPassManager->passes();
  std::swap(resources_.builtPipelines, candidate);
  subs_.pipelineBuilder->destroy(candidate);
  ++presentationPipelineGeneration_;
}

void RendererFrontend::destroyGraphicsPipelines() {
  if (subs_.pipelineBuilder)
    subs_.pipelineBuilder->destroy(resources_.builtPipelines);
}

void RendererFrontend::processShaderReload(bool request, bool wait) {
  if (!request && !shaderReloadJob_.valid())
    return;
  const auto report = [this](std::string status) {
    shaderReloadStatus_ = std::move(status);
    if (subs_.guiManager)
      subs_.guiManager->setStatusMessage(shaderReloadStatus_);
  };
  try {
    if (request && !shaderReloadJob_.valid()) {
      shaderReloadStop_ = std::stop_source{};
      shaderReloadJob_ = std::async(std::launch::async,
          [directory = container::util::executableDirectory(),
           stop = shaderReloadStop_.get_token()] {
            return compileShaderCandidate(directory, stop);
          });
      report("Compiling shaders");
    }
    if (!wait && shaderReloadJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;

    auto compiled = shaderReloadJob_.get();
    auto computeCandidate = svc_.pipelineManager.prepareComputeReload(compiled->directory);
    const bool rebakeBrdf = computeCandidate.changesShader("brdf_lut.comp.spv");
    const bool rebakeEnvironment = computeCandidate.changesShader("equirect_to_cubemap.comp.spv") ||
        computeCandidate.changesShader("irradiance_convolution.comp.spv") ||
        computeCandidate.changesShader("prefilter_specular.comp.spv");
    if (rebakeBrdf || rebakeEnvironment)
      processEnvironmentLoad(true);
    auto bakeCandidate = subs_.environmentManager
        ? subs_.environmentManager->prepareShaderBake(compiled->directory, rebakeBrdf, rebakeEnvironment)
        : std::unique_ptr<EnvironmentManager::PreparedShaderBake>{};
    auto velocityCandidate = subs_.temporalManager
        ? subs_.temporalManager->prepareVelocityReload(compiled->directory)
        : PreparedGraphicsPipelines(svc_.pipelineManager);
    auto pbrCandidate = subs_.pbrAuxiliaryPass
        ? subs_.pbrAuxiliaryPass->prepareShaderReload(compiled->directory)
        : PreparedGraphicsPipelines(svc_.pipelineManager);
    auto debugCandidate = subs_.ddgiManager
        ? subs_.ddgiManager->prepareDebugReload(compiled->directory)
        : std::unique_ptr<DdgiDebugRenderer>{};
    auto rayCandidate = subs_.rayShadowManager
        ? subs_.rayShadowManager->prepareShaderReload(compiled->directory) : RayEffectPipelines{};
    auto ddgiCandidate = subs_.ddgiManager
        ? subs_.ddgiManager->prepareShaderReload(compiled->directory) : RayEffectPipelines{};
    auto ddgiReset = subs_.ddgiManager
        ? subs_.ddgiManager->prepareSceneHistoryReset() : DdgiManager::PreparedSceneHistoryReset{};
    if (subs_.ddgiManager)
      subs_.ddgiManager->synchronizeAsync();
    auto candidate = buildGraphicsPipelines(compiled->directory,
                                            resources_.renderPasses.postProcess);
    if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS) {
      subs_.pipelineBuilder->destroy(candidate);
      throw std::runtime_error("Failed to retire in-flight graphics pipelines");
    }
    if (bakeCandidate && !subs_.environmentManager->canPublishShaderBake(*bakeCandidate)) {
      subs_.pipelineBuilder->destroy(candidate);
      throw std::runtime_error("The active environment changed during shader baking");
    }
    if (subs_.ddgiManager && !subs_.ddgiManager->publishSceneHistoryReset(ddgiReset)) {
      subs_.pipelineBuilder->destroy(candidate);
      throw std::runtime_error("DDGI history reset became stale during shader reload");
    }
    std::swap(resources_.builtPipelines, candidate);
    subs_.pipelineBuilder->destroy(candidate);
    ++presentationPipelineGeneration_;
    svc_.pipelineManager.publishComputeReload(computeCandidate);
    if (subs_.temporalManager)
      subs_.temporalManager->publishVelocityReload(velocityCandidate);
    if (subs_.pbrAuxiliaryPass)
      subs_.pbrAuxiliaryPass->publishShaderReload(pbrCandidate);
    if (subs_.ddgiManager)
      subs_.ddgiManager->publishDebugReload(std::move(debugCandidate));
    if (subs_.rayShadowManager)
      subs_.rayShadowManager->publishShaderReload(std::move(rayCandidate));
    if (subs_.ddgiManager)
      subs_.ddgiManager->publishShaderReload(std::move(ddgiCandidate));
    if (bakeCandidate) {
      subs_.environmentManager->publishShaderBake(*bakeCandidate);
      if (subs_.ddgiManager)
        subs_.ddgiManager->replaceEnvironment(subs_.environmentManager->prefilteredView(),
                                               subs_.environmentManager->envSampler());
      updateFrameDescriptorSets();
    }
    activeShaderReload_ = std::move(compiled);
    ++shaderReloadGeneration_;
    if (subs_.temporalManager)
      subs_.temporalManager->reset("shaders reloaded");
    invalidateDepthVisibilityFrames();
    report("Shaders reloaded (" + std::to_string(activeShaderReload_->compiledStages) +
           " stages compiled, " + std::to_string(computeCandidate.size()) +
           " managed compute " + (computeCandidate.size() == 1 ? "pipeline" : "pipelines") +
           " replaced)");
  } catch (const std::exception& error) {
    report(std::string("Shader reload failed: ") + error.what());
    spdlog::warn("{}", shaderReloadStatus_);
  }
}

void RendererFrontend::recreateMsaaResources(
    VkSampleCountFlagBits sampleCount) {
  sampleCount = clampMsaaSampleCount(sampleCountToSamples(sampleCount),
                                    msaaSampleMask_);
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
    retireComfyUiPreview();
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
        svc_.ctx.wireframeWideLinesSupported,
        svc_.ctx.deviceWrapper->enabledFeatures().wideLines == VK_TRUE);
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
  if (sceneLightingOverrides_.bloomEnabled)
    current.bloomEnabled = svc_.config.bloomEnabled;
  current = sceneLightingDefaults_.apply(current, defaults, sceneLightingOverrides_);
  settings.directionalIntensity = current.directionalIntensity;
  settings.environmentIntensity = current.environmentIntensity;
  settings.bounceIntensity = current.bounceIntensity;
  settings.localShadowLayerBudget = current.localShadowLayerBudget;
  subs_.lightingManager->setLightingSettings(settings);
  subs_.bloomManager->enabled() = current.bloomEnabled;
}

void RendererFrontend::applyLightingOverrides() {
  auto& lighting = *subs_.lightingManager;
  std::optional<float> directionalIntensity;
  if (sceneLightingOverrides_.directionalIntensity)
    directionalIntensity = lighting.lightingSettings().directionalIntensity;
  applyLightingPresentation(lighting.lightingData(), svc_.config,
                            directionalIntensity, subs_.environmentManager.get());
}

void RendererFrontend::resetCameraForActiveScene() {
  if (subs_.temporalManager)
    subs_.temporalManager->reset("scene/camera reset");
  if (!subs_.cameraController) {
    return;
  }

  auto bounds = cameraSceneBoundsFromActiveContent(subs_.sceneManager.get(),
                                                   subs_.bimManager.get());
  if (subs_.openVdbVolumeRenderer)
    includeSceneViewBounds(
        bounds, {.min = arrayToVec3(subs_.openVdbVolumeRenderer->minimum()),
                 .max = arrayToVec3(subs_.openVdbVolumeRenderer->maximum()),
                 .valid = true});
  subs_.cameraController->resetCameraForBounds(
      bounds, !subs_.openVdbVolumeRenderer &&
                  (!subs_.bimManager || !subs_.bimManager->hasScene()));
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
  if (!svc_.config.aslPosePath.empty()) {
    aslPoseReplay_ = std::make_unique<container::asl::PoseReplayScene>(
        container::asl::PoseReplay::load(
            container::util::pathFromUtf8(svc_.config.aslPosePath)),
        sceneGraph_, *subs_.sceneManager, svc_.config.aslPoseStartMs,
        svc_.config.aslPoseSpeed, svc_.config.aslPoseLoop, svc_.config.aslHandKinematics,
        svc_.config.aslHandAspectRatio, svc_.config.aslHandAngularVelocity,
        container::util::pathFromUtf8(svc_.config.aslHandReportPath));
    subs_.sceneManager->refreshActiveScene(sceneGraph_);
    sceneState_.indexType = subs_.sceneManager->indexType();
  }
  createSceneBuffers();
  subs_.lightingManager->updateLightingData();
  applyLightingOverrides();
  subs_.lightingManager->createLightVolumeGeometry();
  createFrameResources();
  if (subs_.environmentManager) {
    const VkExtent2D ext = sceneRenderExtent();
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

  if (svc_.config.physicsDemo)
    createPhysicsDemo();

  // Resolve startup selectors against the initial model before capture
  // actions or editor requests can replace it.
  if (svc_.config.gltfCameraNode >= 0 || svc_.config.gltfAnimationClip >= 0)
    resetGltfAnimation(true);

  container::log::ContainerLogger::instance().renderer()->info(
      "Initializing Vulkan renderer");
  container::log::ContainerLogger::instance().vulkan()->debug(
      "Debugging Vulkan initialization");
}

void RendererFrontend::buildSceneGraph() {
  if (!subs_.sceneController)
    return;
  finishEditGestures();
  if (sceneEdits_) sceneEdits_->clear();
  pendingLightSelection_.reset();
  transformDragSession_.active = false;
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
    if (objectBuffer.buffer != VK_NULL_HANDLE &&
        (!subs_.sceneController || !subs_.sceneController->ownsObjectBuffer(objectBuffer.buffer))) {
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

VkExtent2D RendererFrontend::sceneRenderExtent() const {
  if (subs_.frameResourceManager) {
    const auto extent = subs_.frameResourceManager->sceneExtent();
    if (extent.width && extent.height) return extent;
  }
  return svc_.swapChainManager.extent();
}

RendererFrontend::UpscaleResourceRequest RendererFrontend::currentUpscaleResourceRequest() const {
  const auto selection = captureUpscaling_.value_or(subs_.guiManager ? subs_.guiManager->upscaling()
                                                                     : svc_.config.upscaling);
  if (selection.mode == UpscaleMode::Native)
    return {};
  UpscaleResourceRequest request{.selection = selection};
  if (msaaSampleCount_ != VK_SAMPLE_COUNT_1_BIT ||
      referencePathActive() || svc_.config.maxFramesInFlight > 4 || !subs_.cameraController)
    return request;
  const auto display = frontendDisplayMode(subs_.guiManager.get(), configuredDisplayMode(svc_.config));
  if (display != container::ui::GBufferViewMode::Lit &&
      display != container::ui::GBufferViewMode::TemporalVelocity &&
      display != container::ui::GBufferViewMode::TemporalReactive)
    return request;
  const auto *camera = dynamic_cast<const container::scene::PerspectiveCamera*>(
      subs_.cameraController->camera());
  if (gltfAnimationCamera_) {
    request.eligible = !gltfAnimationCamera_->orthographic;
    request.infiniteFar = request.eligible && gltfAnimationCamera_->projection[2][2] == 0.0f;
  } else if (camera) {
    request.eligible = true;
    request.infiniteFar = !std::isfinite(camera->farPlane());
  }
  return request;
}

void RendererFrontend::reconcileUpscaleResources() {
  const auto request = currentUpscaleResourceRequest();
  if (request == upscaleResourceRequest_) return;
  if (!request.eligible && !subs_.temporalManager->upscalingActive()) {
    upscaleResourceRequest_ = request;
    upscaleViewportStatus_ = request.selection.mode == UpscaleMode::Native ? "" :
        "Upscaling requires a perspective raster view at MSAA 1x and a supported display mode";
    return;
  }
  if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS)
    throw std::runtime_error("Failed to retire frames before changing the upscaling mode");
  if (subs_.rendererTelemetry) subs_.rendererTelemetry->noteDeviceWaitIdle();
  invalidateDepthVisibilityFrames();
  createFrameResources();
}

void RendererFrontend::createFrameResources() {
  // Auxiliary framebuffers borrow the old depth views. Retire them first.
  subs_.pbrAuxiliaryPass.reset();
  subs_.screenReflectionPass.reset();
  subs_.volumetricFogPass.reset();
  if (subs_.temporalManager)
    subs_.temporalManager->destroyImages();
  const auto previousSceneExtent = sceneRenderExtent();
  const auto displayExtent = svc_.swapChainManager.extent();
  auto sceneExtent = displayExtent;
  upscaleResourceRequest_ = currentUpscaleResourceRequest();
  upscaleViewportStatus_.clear();
  const auto selection = upscaleResourceRequest_.selection;
  if (upscaleResourceRequest_.eligible && subs_.temporalManager) {
    auto& temporal = *subs_.temporalManager;
    const auto root = container::util::executableDirectory();
    if (selection.backend == UpscaleBackend::Dlss) {
      if (const auto optimal = temporal.prepareDlss(root, selection.mode, displayExtent))
        sceneExtent = *optimal;
    } else {
      const auto requested = fsrRenderExtent(selection.mode, displayExtent);
      if (temporal.prepareFsr(root, {requested, displayExtent, upscaleResourceRequest_.infiniteFar}))
        sceneExtent = requested;
    }
    upscaleViewportStatus_ = temporal.upscalingStatus();
  } else if (selection.mode != UpscaleMode::Native) {
    upscaleViewportStatus_ = "Upscaling requires a perspective raster view at MSAA 1x and a supported display mode";
  }
  ensureObjectBuffers();
  const auto objectBuffer = sceneObjectBuffer(0);
  const auto createSceneGrid = [&] {
    if (subs_.lightingManager)
      subs_.lightingManager->resizeTiledResources(sceneExtent);
    subs_.frameResourceManager->create(
        resources_.gBufferFormats, resources_.renderPasses.depthPrepass,
        resources_.renderPasses.bimDepthPrepass, resources_.renderPasses.gBuffer,
        resources_.renderPasses.bimGBuffer,
        resources_.renderPasses.transparentPick, resources_.renderPasses.lighting,
        resources_.renderPasses.forwardLighting, resources_.renderPasses.forwardTransparent,
        resources_.renderPasses.transformGizmos, msaaSampleCount_, buffers_.cameras,
        objectBuffer, sceneExtent);
  };
  if (subs_.temporalManager && subs_.temporalManager->dlssActive()) {
    auto& temporal = *subs_.temporalManager;
    try {
      createSceneGrid();
      temporal.createPipelines(activeShaderReload_ ? activeShaderReload_->directory :
          container::util::executableDirectory(), subs_.sceneManager->descriptorSetLayout(),
          resources_.gBufferFormats.depthStencil);
      temporal.prepare(*subs_.frameResourceManager, sceneExtent,
          static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
    } catch (const std::exception& error) {
      temporal.destroyImages();
      sceneExtent = displayExtent;
      upscaleViewportStatus_ = std::string("DLSS resource setup failed: ") + error.what();
      createSceneGrid();
    }
  } else {
    createSceneGrid();
  }
  subs_.oitManager->prepareNativeComposeResources(
      svc_.pipelineManager, container::util::executableDirectory(),
      subs_.frameResourceManager->frameCount());
  if (subs_.environmentManager && subs_.environmentManager->isGtaoReady() &&
      (sceneExtent.width != previousSceneExtent.width || sceneExtent.height != previousSceneExtent.height))
    subs_.environmentManager->recreateGtaoTextures(sceneExtent.width, sceneExtent.height);
  if (svc_.config.screenReflections != ScreenReflectionMode::Off) {
    try {
      auto reflections = std::make_unique<ScreenReflectionPass>(
          svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
      reflections->createResources(container::util::executableDirectory(),
          *subs_.frameResourceManager, sceneExtent,
          subs_.lightingManager->lightDescriptorSetLayout(), subs_.sceneManager->descriptorSetLayout(),
          svc_.config.screenReflectionRayFallback,
          screenReflectionModeUsesHistory(svc_.config.screenReflections));
      subs_.screenReflectionPass = std::move(reflections);
    } catch (const std::exception& error) {
      container::log::ContainerLogger::instance().renderer()->warn(
          "Screen reflections unavailable: {}", error.what());
    }
  }
  if (svc_.config.volumetricFog.enabled() && subs_.shadowManager) {
    try {
      auto fog = std::make_unique<VolumetricFogPass>(svc_.ctx.deviceWrapper,
          svc_.allocationManager, svc_.pipelineManager);
      fog->createResources(container::util::executableDirectory(), *subs_.frameResourceManager,
          sceneExtent, *subs_.shadowManager, subs_.lightingManager->lightDescriptorSetLayout());
      subs_.volumetricFogPass = std::move(fog);
    } catch (const std::exception& error) {
      container::log::ContainerLogger::instance().renderer()->warn("Volumetric fog unavailable: {}", error.what());
    }
  }
  if (subs_.openVdbVolumeRenderer)
    subs_.openVdbVolumeRenderer->prepare(
        *subs_.frameResourceManager, buffers_.cameras,
        subs_.lightingManager->lightingBuffers(),
        subs_.sceneManager->sceneClipBuffer(),
        subs_.lightingManager->pointLightBuffer(),
        subs_.lightingManager->areaLightBuffer(), *subs_.shadowManager);
  if (subs_.rayShadowManager)
    subs_.rayShadowManager->prepare(
        sceneExtent,
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  if (subs_.ddgiManager)
    subs_.ddgiManager->prepare(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()),
        subs_.environmentManager->prefilteredView(),
        subs_.environmentManager->envSampler());
}

// ---------------------------------------------------------------------------
// Per-frame helpers
// ---------------------------------------------------------------------------

void RendererFrontend::updateCameraBuffer(uint32_t imageIndex) {
  if (!subs_.cameraController || imageIndex >= buffers_.cameras.size())
    return;
  subs_.cameraController->updateCameraBuffer(buffers_.cameraData,
                                             buffers_.cameras[imageIndex]);
  (void)applyGltfAnimationCamera(imageIndex);
  // Keep weak viewport intent initialized in headless captures as well as the
  // editor. Failed writes then do not create a view revision by lazy setup.
  (void)refreshCameraViewPoseViewRevision();
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

void RendererFrontend::prepareGpuHierarchyFrame(uint32_t imageIndex) {
  gpuHierarchyPrepared_ = false;
  if (subs_.guiManager) gpuHierarchyEnabled_ = subs_.guiManager->gpuHierarchyTransforms();
  // A GPU-written SSBO may outlive a path switch without its CPU revision
  // changing. Restore that image's mirror before TAA adds submitted history.
  const auto restoreCpu = [&] {
    if (!subs_.sceneController || imageIndex >= buffers_.objects.size()) return;
    const bool previousGpu=imageIndex<gpuHierarchyObjectBuffers_.size() &&
        gpuHierarchyObjectBuffers_[imageIndex]==buffers_.objects[imageIndex].buffer;
    if (gpuObjectUploadDeferred_ || (previousGpu && subs_.sceneController->lastObjectUploadBytes()==0)) {
      const auto& objects = subs_.sceneController->objectData();
      if (!objects.empty()) {
        SceneController::writeToBuffer(svc_.allocationManager, buffers_.objects[imageIndex],
            objects.data(), objects.size()*sizeof(container::gpu::ObjectData));
        cpuObjectUploadBytes_+=objects.size()*sizeof(container::gpu::ObjectData);
      }
    }
    if(imageIndex<gpuHierarchyObjectBuffers_.size()) gpuHierarchyObjectBuffers_[imageIndex] = VK_NULL_HANDLE;
    gpuObjectUploadDeferred_=false;
  };
  if (!gpuHierarchyEnabled_ || !subs_.sceneController) {
    restoreCpu();
    gpuHierarchyStatus_ = "CPU";
    return;
  }
  try {
    if (!subs_.gpuHierarchyPass) {
      auto pass = std::make_unique<GpuHierarchyPass>(svc_.ctx.deviceWrapper,
          *svc_.allocationManager.memoryManager(), svc_.pipelineManager);
      pass->createResources(activeShaderReload_ ? activeShaderReload_->directory
                                               : container::util::executableDirectory(),
                            uint32_t(svc_.swapChainManager.imageCount()));
      subs_.gpuHierarchyPass = std::move(pass);
    }
    std::vector<HierarchyLocalTransform> locals;
    if (!subs_.gpuRayInstancePass && svc_.ctx.deviceWrapper->rayQueriesEnabled()) {
      auto pass=std::make_unique<RayInstanceTransformPass>(svc_.ctx.deviceWrapper,
          *svc_.allocationManager.memoryManager(),svc_.pipelineManager);
      pass->createResources(activeShaderReload_?activeShaderReload_->directory:container::util::executableDirectory());
      subs_.gpuRayInstancePass=std::move(pass);
    }
    locals.reserve(sceneGraph_.nodeCount());
    for (uint32_t i=0;i<sceneGraph_.nodeCount();++i) {
      const auto& node = *sceneGraph_.getNode(i);
      locals.push_back({node.localTransform,node.parent});
    }
    const auto& objects = subs_.sceneController->objectData();
    subs_.gpuHierarchyPass->prepareFrame(imageIndex, locals, subs_.sceneController->objectNodeIndices(),
        {buffers_.objects.at(imageIndex).buffer, 0, uint64_t(objects.size())*sizeof(container::gpu::ObjectData)});
    gpuHierarchyImage_ = imageIndex;
    gpuHierarchyObjectBuffers_.resize(svc_.swapChainManager.imageCount(), VK_NULL_HANDLE);
    gpuHierarchyObjectBuffers_[imageIndex] = buffers_.objects.at(imageIndex).buffer;
    gpuHierarchyPrepared_ = true;
    gpuObjectUploadDeferred_=false;
    gpuHierarchyStatus_ = "GPU propagation; CPU mirror retained";
  } catch (const std::exception& error) {
    restoreCpu();
    const std::string status = std::string("CPU fallback: ") + error.what();
    if (gpuHierarchyStatus_ != status) {
      spdlog::warn("{}", status);
      if (subs_.guiManager) subs_.guiManager->setStatusMessage(status);
    }
    gpuHierarchyStatus_ = status;
  }
}

void RendererFrontend::uploadPreparedObjectData(uint32_t imageIndex,std::span<const container::gpu::ObjectData> objects) {
  if(gpuHierarchyPrepared_ && subs_.gpuHierarchyPass) {
    try {
      subs_.gpuHierarchyPass->uploadObjectMetadata(imageIndex,objects);
      return;
    } catch(const std::exception& error) {
      gpuHierarchyPrepared_=false;
      if(imageIndex<gpuHierarchyObjectBuffers_.size()) gpuHierarchyObjectBuffers_[imageIndex]=VK_NULL_HANDLE;
      const std::string status=std::string("CPU fallback: ")+error.what();
      if(gpuHierarchyStatus_!=status) {
        spdlog::warn("{}",status);
        if(subs_.guiManager) subs_.guiManager->setStatusMessage(status);
      }
      gpuHierarchyStatus_=status;
    }
  }
  if(!objects.empty()) {
    SceneController::writeToBuffer(svc_.allocationManager,buffers_.objects.at(imageIndex),objects.data(),objects.size_bytes());
    cpuObjectUploadBytes_+=objects.size_bytes();
  }
}

bool RendererFrontend::updateObjectBuffer(uint32_t imageIndex, bool deferUploadToGpu) {
  if (!subs_.sceneController)
    return false;
  ensureObjectBuffers();
  if (imageIndex >= buffers_.objects.size() ||
      imageIndex >= buffers_.objectCapacities.size()) {
    return false;
  }

  const auto cameraBuffer =
      imageIndex < buffers_.cameras.size()
          ? buffers_.cameras[imageIndex]
          : container::gpu::AllocatedBuffer{};
  const bool recreated = subs_.sceneController->updateObjectBuffer(
      buffers_.objects[imageIndex], buffers_.objectCapacities[imageIndex],
      cameraBuffer,deferUploadToGpu);
  cpuObjectUploadBytes_+=subs_.sceneController->lastObjectUploadBytes();
  if (subs_.shadowCullManager &&
      imageIndex < buffers_.shadowObjectDescriptorReady.size() &&
      (recreated || !buffers_.shadowObjectDescriptorReady[imageIndex])) {
    subs_.shadowCullManager->updateObjectSsboDescriptor(
        imageIndex, buffers_.objects[imageIndex].buffer,
        sizeof(container::gpu::ObjectData) *
            buffers_.objectCapacities[imageIndex]);
    buffers_.shadowObjectDescriptorReady[imageIndex] = true;
  }
  sceneState_.diagCubeObjectIndex = subs_.sceneController->diagCubeObjectIndex();
  return recreated;
}

void RendererFrontend::publishSceneObjectDescriptors() {
  if (subs_.sceneManager) {
    // All scene sets share material/texture bindings. An image graphics fence
    // alone does not retire compute work borrowing another image's set.
    if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.objects);
  }
}

void RendererFrontend::updateAllObjectBuffers() {
  ensureObjectBuffers();
  const uint32_t imageCount =
      static_cast<uint32_t>(std::min(buffers_.objects.size(),
                                     buffers_.objectCapacities.size()));
  for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
    updateObjectBuffer(imageIndex);
  }
  // A change in image count replaces all slots. Publish the scene descriptor
  // sets only after every object buffer has a valid binding.
  publishSceneObjectDescriptors();
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
  // createSceneBuffers also calls this before the first createFrameResources.
  if (subs_.rayShadowManager && subs_.lightingManager &&
      subs_.rayShadowManager->visibilityView()) {
    const uint32_t begin = imageIndex == UINT32_MAX ? 0u : imageIndex;
    const uint32_t end = imageIndex == UINT32_MAX
                             ? uint32_t(svc_.swapChainManager.imageCount())
                             : imageIndex + 1;
    for (uint32_t i = begin; i < end; ++i)
      subs_.lightingManager->updateRayShadowDescriptors(
          i, subs_.rayShadowManager->readSettings(i),
          subs_.rayShadowManager->readSettingsSize(),
          subs_.rayShadowManager->visibilityView());
  }
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
      applyLightingOverrides();
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
    std::vector<VkImageView> anisotropyViews, diffuseTransmissionViews;
    if (subs_.pbrAuxiliaryPass && subs_.pbrAuxiliaryPass->ready()) {
      for (uint32_t i = 0; i < subs_.pbrAuxiliaryPass->frameCount(); ++i) {
        anisotropyViews.push_back(subs_.pbrAuxiliaryPass->anisotropyView(i));
        diffuseTransmissionViews.push_back(
            subs_.pbrAuxiliaryPass->diffuseTransmissionView(i));
      }
    }
    const auto objectBuffer = sceneObjectBuffer(0);
    subs_.frameResourceManager->updateDescriptorSets(
        buffers_.cameras, objectBuffer, shadowView, shadowSampler,
        shadowUbos, localShadowView, localShadowSampler, localShadowUbos,
        irradianceView, prefilteredView, brdfLutView, envSampler,
        brdfLutSampler, aoTextureView, aoSampler, bloomTextureView,
        bloomSampler, tileGridBuffer, tileGridBufferSize, exposureStateBuffers,
        exposureStateBufferSize, anisotropyViews, diffuseTransmissionViews);
  }
  if (subs_.temporalManager && preparedParams &&
      imageIndex < buffers_.cameras.size()) {
    if (!referencePathActive()) updateReferenceColorSource(imageIndex, false);
    if (const auto *frame = subs_.frameResourceManager->frame(imageIndex))
      subs_.temporalManager->updateDescriptors(
          imageIndex, *frame, buffers_.cameras[imageIndex],
          preparedParams->debug.displayMode == 0 ||
              preparedParams->debug.displayMode == 8 ||
              preparedParams->debug.displayMode >= 100);
  }
  if (referencePathActive() && preparedParams)
    updateReferenceColorSource(imageIndex, preparedParams->screenshot.sceneLinear);
}

void RendererFrontend::destroyGBufferResources() {
  subs_.referencePathTracePass.reset();
  referenceColorBound_.clear();
  referenceDiagnosticSeen_.clear();
  referenceStatus_.clear();
  referenceRecorded_ = false;
  subs_.pbrAuxiliaryPass.reset();
  subs_.screenReflectionPass.reset();
  subs_.volumetricFogPass.reset();
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
    if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS)
      throw std::runtime_error("Failed to retire frames before growing the OIT node pool");
    if (subs_.rendererTelemetry) subs_.rendererTelemetry->noteDeviceWaitIdle();
    createFrameResources();
    updateFrameDescriptorSets();
    if (subs_.guiManager) {
      const auto capacity = deferredRasterRuntimeOitNodeCapacity(
          subs_.frameResourceManager.get(), imageIndex);
      subs_.guiManager->setStatusMessage(
          "Expanded exact OIT node pool to " + std::to_string(capacity));
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
  if (screenshot_.sceneLinear ? format != VK_FORMAT_R32G32B32A32_SFLOAT
                              : !isSupportedScreenshotFormat(format)) {
    throw std::runtime_error("unsupported screenshot source format");
  }

  const VkDeviceSize requiredSize = static_cast<VkDeviceSize>(extent.width) *
                                    static_cast<VkDeviceSize>(extent.height) *
                                    (screenshot_.sceneLinear ? 16u : 4u);
  if (screenshot_.sceneLinear) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(svc_.ctx.deviceWrapper->physicalDevice(), &properties);
    if (requiredSize > properties.limits.maxStorageBufferRange)
      throw std::runtime_error("scene-linear capture exceeds the Vulkan storage buffer size limit");
  }
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
      requiredSize, screenshot_.sceneLinear ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                            : VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  readback.readbackSize = requiredSize;
  readback.extent = extent;
  readback.format = format;
}

void RendererFrontend::writePendingScreenshot(uint32_t frameSlot, const FrameRecordParams& params) {
  if (!screenshot_.pending) {
    return;
  }
  const auto* referenceCapture = referencePathActive() ? subs_.referencePathTracePass.get() : nullptr;
  const auto ground = referenceCapture ? referenceCapture->effectiveGround() : ReferenceGroundSettings{};
  const bool shadowMatte = ground.mode == ReferenceGroundMode::ShadowMatte;
  if (const auto target = screenshot_.referenceSampleTarget) {
    if (!referenceCapture || referenceCapture->attemptedSamples() != *target)
      throw std::runtime_error("Reference sample checkpoint did not reach " + std::to_string(*target) + " samples");
    if (referenceUnsupported_ != 0 || referenceRejected_ != 0)
      throw std::runtime_error("Reference sample checkpoint contains " +
          std::to_string(referenceUnsupported_) + " unsupported and " +
          std::to_string(referenceRejected_) + " rejected samples");
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
  std::vector<unsigned char> rgba;
  std::vector<float> radiance;
  if (screenshot_.sceneLinear) {
    radiance.resize(pixelCount * 3u);
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
      auto* rgb = radiance.data() + pixel * 3u;
      std::memcpy(rgb, src + pixel * 4u * sizeof(float), 3u * sizeof(float));
      if (shadowMatte) {
        const float opacity = rgb[1] > 0.0f
            ? std::clamp(rgb[0] / rgb[1], 0.0f, 1.0f) * std::clamp(rgb[2], 0.0f, 1.0f)
            : 0.0f;
        std::fill_n(rgb, 3, opacity);
      }
    }
  } else {
    rgba = convertSwapchainBytesToRgba(src, pixelCount, readback.format);
  }

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   readback.readbackBuffer.allocation);
  }

  if (!screenshot_.outputPath.parent_path().empty()) {
    std::filesystem::create_directories(screenshot_.outputPath.parent_path());
  }
  const std::string outputPath =
      container::util::pathToUtf8(screenshot_.outputPath);
  screenshot_.pending = false;
  auto metadataPath = screenshot_.outputPath;
  metadataPath += ".capture.json";
  nlohmann::json metadata{{"schemaVersion", 1},
                          {"width", readback.extent.width}, {"height", readback.extent.height},
                          {"colorSpace", screenshot_.sceneLinear ? "linear-sRGB" : "sRGB"},
                          {"includesUi", !screenshot_.sceneLinear && !screenshot_.viewportOnly && subs_.guiManager != nullptr}};
  if (referenceCapture) {
    const auto lens = referenceCapture->effectiveLens();
    const auto denoiser = referenceCapture->oidnStatus();
    const bool optix = referenceCapture->optixSelected();
    metadata["referencePath"] = {
        {"attemptedSamplesPerPixel", referenceCapture->attemptedSamples()},
        {"sampleLimit", svc_.config.pathSampleLimit},
        {"sampleLimitReached", referenceCapture->attemptedSamples() >= svc_.config.pathSampleLimit},
        {"samplesPerFrame", svc_.config.pathSamplesPerFrame},
        {"adaptiveTargetMs", svc_.config.pathTargetMs},
        {"lastSubmittedSamples", referenceCapture->lastSubmittedSamples()},
        {"display", referencePathDisplayName(referencePathDisplay())},
        {"fireflyClamp", referenceCapture->effectiveFireflyClamp()},
        {"oidnDisplaySamples", screenshot_.sceneLinear || optix ? 0u : denoiser.displayedSamples},
        {"optixDisplaySamples", screenshot_.sceneLinear || !optix ? 0u : denoiser.displayedSamples},
        {"ground", referenceGroundMetadata(ground)},
        {"maxBounces", svc_.config.pathMaxBounces},
        {"seed", svc_.config.pathSeed},
        {"apertureRadius", lens ? lens->apertureRadius : 0.0f},
        {"focusDistance", lens ? nlohmann::json(lens->focusDistance) : nlohmann::json(nullptr)},
        {"historyResets", referenceCapture->historyResets()},
        {"unsupportedSamples", referenceUnsupported_}, {"rejectedSamples", referenceRejected_},
        {"experimentalMaterialFallbacks", referenceExperimentalMaterialFallbackMetadata(referenceExperimentalMaterialFallbacks_)},
        {"hasDiagnostics", referenceUnsupported_ != 0 || referenceRejected_ != 0},
        {"status", referenceStatus_}};
    if (const auto target = screenshot_.referenceSampleTarget) {
      metadata["referencePath"]["requestedSamplesPerPixel"] = *target;
      metadata["referencePath"]["acceptedSamplesPerPixel"] = *target;
    }
  }
  const auto publication = (screenshot_.remote || (comfyUi_ && !comfyUi_->captureId.empty()))
      ? container::util::AtomicFilePublication::CreateNew :
      container::util::AtomicFilePublication::ReplaceExisting;
  if (screenshot_.sceneLinear) {
    unsigned char* encoded = nullptr;
    const char* error = nullptr;
    const int size = SaveEXRToMemory(radiance.data(), static_cast<int>(readback.extent.width),
        static_cast<int>(readback.extent.height), 3, 0, &encoded, &error);
    const std::unique_ptr<unsigned char, decltype(&std::free)> bytes(encoded, &std::free);
    if (size <= 0 || !bytes) {
      const std::string reason = error ? error : "unknown encoder error";
      if (error)
        FreeEXRErrorMessage(error);
      throw std::runtime_error("failed to encode scene-linear EXR: " + reason);
    }
    container::util::writeFileAtomically(screenshot_.outputPath,
        {reinterpret_cast<const char*>(bytes.get()), static_cast<size_t>(size)}, publication);
    metadata["source"] = shadowMatte ? "path-trace-shadow-matte" :
        (referenceCapture ? "path-trace-mean" : "raster-scene-composite");
    if (shadowMatte) metadata["colorSpace"] = "linear-mask";
    metadata["exposureApplied"] = false;
    metadata["toneMappingApplied"] = false;
  } else {
    std::string png;
    const auto append = [](void* context, void* data, int size) {
      static_cast<std::string*>(context)->append(static_cast<const char*>(data), size);
    };
    if (!stbi_write_png_to_func(append, &png, static_cast<int>(readback.extent.width),
                                static_cast<int>(readback.extent.height), 4, rgba.data(),
                                static_cast<int>(readback.extent.width * 4))) {
      throw std::runtime_error("failed to encode screenshot PNG: " + outputPath);
    }
    container::util::writeFileAtomically(screenshot_.outputPath, png, publication);
    const auto exposure = sanitizeExposureSettings(params.postProcess.exposureSettings);
    const bool automatic = exposure.mode == container::gpu::kExposureModeAuto;
    metadata["source"] = screenshot_.viewportOnly ? "viewport" : "window";
    metadata["displayMode"] = params.debug.displayMode;
    metadata["exposureMode"] = automatic ? "auto" : "manual";
    metadata["manualExposure"] = automatic ? nlohmann::json(nullptr) : nlohmann::json(exposure.manualExposure);
    metadata["toneMapper"] = static_cast<uint32_t>(params.postProcess.displayGrade.toneMapper);
    if (referenceCapture && referencePathDisplay() == ReferencePathDisplayMode::StandardError) {
      metadata["exposureApplied"] = false;
      metadata["toneMappingApplied"] = false;
    }
  }
  container::util::writeFileAtomically(metadataPath, metadata.dump(2) + '\n', publication);
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
                                                        uint32_t frameSlot,
                                                        bool pickIdWritten) {
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
  frame.extent = sceneRenderExtent();
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
  frame.pickIdValid = pickIdWritten;
  frame.valid = true;
}

bool RendererFrontend::depthVisibilityFrameMatchesCurrentState() const {
  const DepthVisibilityFrameSlot* frame =
      depthVisibilityFrameSlot(depthVisibility_.latestFrameSlot);
  if (frame == nullptr || !frame->valid || !subs_.sceneController) {
    return false;
  }

  const VkExtent2D extent = sceneRenderExtent();
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
  const auto metadata =
      subs_.bimManager->metadataForObject(objectIndex);
  if (!metadata) {
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
    if (const auto metadata =
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
  finishEditGestures();

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
        const auto metadata =
            subs_.bimManager->metadataForObject(objectIndex);
        if (snapshot.selectedBimType.empty() ||
            (metadata &&
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
      const auto metadata =
          subs_.bimManager->metadataForObject(snapshot.selectedBimObjectIndex);
      const bool guidCompatible =
          snapshot.selectedBimGuid.empty() ||
          (metadata && metadata->guid == snapshot.selectedBimGuid);
      const bool sourceIdCompatible =
          snapshot.selectedBimSourceId.empty() ||
          (metadata &&
           metadata->sourceId == snapshot.selectedBimSourceId);
      const bool typeCompatible =
          snapshot.selectedBimType.empty() ||
          (metadata && metadata->type == snapshot.selectedBimType);
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

static std::optional<glm::uvec2> scenePixelAtDisplayCursor(
    double x, double y, VkExtent2D display, VkExtent2D scene) {
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || y < 0.0 ||
      x >= display.width || y >= display.height || !scene.width || !scene.height)
    return std::nullopt;
  return glm::uvec2{
      std::min(static_cast<uint32_t>(x * scene.width / display.width), scene.width - 1u),
      std::min(static_cast<uint32_t>(y * scene.height / display.height), scene.height - 1u)};
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
  const auto pixel = scenePixelAtDisplayCursor(cursorX, cursorY,
      svc_.swapChainManager.extent(), frame->extent);
  if (depthImage == VK_NULL_HANDLE || !pixel) {
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

  const uint32_t x = pixel->x;
  const uint32_t y = pixel->y;

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
  toTransfer.subresourceRange = {
      depthReadbackBarrierAspects(frame->format), 0, 1, 0, 1};
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
  if (frame == nullptr || !frame->pickIdValid ||
      frame->imageIndex >= subs_.frameResourceManager->frameCount()) {
    return false;
  }

  const VkImage pickIdImage = deferredRasterRuntimeImage(
      subs_.frameResourceManager.get(), frame->imageIndex, "pick-id");
  const auto pixel = scenePixelAtDisplayCursor(cursorX, cursorY,
      svc_.swapChainManager.extent(), frame->extent);
  if (pickIdImage == VK_NULL_HANDLE || !pixel) {
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

  const uint32_t x = pixel->x;
  const uint32_t y = pixel->y;

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

void RendererFrontend::fitDdgiToActiveScene(uint32_t maxProbes) {
  if (!subs_.ddgiManager)
    throw std::invalid_argument(
        "DDGI manager is unavailable for scene fitting");
  // Fit is an explicit, one-shot CPU operation. Refresh current transforms;
  // import-time bounds and transformed spheres can be stale or miss shear.
  refreshSceneObjectData();
  std::vector<DrawCommand> meshDraws, bimDraws;
  std::vector<RaySceneProviderSource> sources;
  if (subs_.sceneController && subs_.sceneManager) {
    meshDraws = subs_.sceneController->opaqueDrawCommands();
    const auto &transparent = subs_.sceneController->transparentDrawCommands();
    meshDraws.insert(meshDraws.end(), transparent.begin(), transparent.end());
    sources.push_back({{"mesh-fit"},
                       subs_.sceneController->geometryRevision(),
                       subs_.sceneController->geometryVertices(),
                       subs_.sceneController->geometryIndices(),
                       subs_.sceneController->objectData(),
                       meshDraws});
  }
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    auto filter = currentBimDrawFilter();
    filter.drawBudgetEnabled = false;
    filter.drawBudgetMaxObjects = 0;
    auto append = [&](const auto &draws) {
      for (const auto &draw : draws)
        if (subs_.bimManager->objectMatchesFilter(draw.objectIndex, filter) &&
            bimObjectVisibleByLayer(draw.objectIndex))
          bimDraws.push_back(draw);
    };
    append(subs_.bimManager->opaqueDrawCommands());
    append(subs_.bimManager->transparentDrawCommands());
    sources.push_back({{"bim-fit"},
                       subs_.bimManager->geometryRevision(),
                       subs_.bimManager->vertices(),
                       subs_.bimManager->indices(),
                       subs_.bimManager->objectData(),
                       bimDraws});
  }
  const auto bounds = computeDdgiSceneBounds(sources);
  if (!bounds.valid)
    throw std::invalid_argument(
        "No visible triangle geometry to fit. Load a mesh/BIM/USD model or "
        "use manual probe-volume settings.");
  const auto fitted =
      fitDdgiVolume(subs_.ddgiManager->settings(), bounds.minimum,
                    bounds.maximum, {maxProbes});
  const auto previous = subs_.ddgiManager->settings();
  subs_.ddgiManager->applySettings(fitted.settings);
  if (previous != fitted.settings && subs_.temporalManager)
    subs_.temporalManager->reset("DDGI scene volume fitted");
  fittedDdgiVolume_ = fitted.settings.volume;
  ddgiFitBoundsMin_ = bounds.minimum;
  ddgiFitBoundsMax_ = bounds.maximum;
  ddgiFitProbeBudget_ = maxProbes;
  ddgiFitProbeCount_ = fitted.probeCount;
  ddgiFitPayloadBytes_ = fitted.payloadBytes;
  ddgiFitStatus_ =
      "Fitted " + std::to_string(fitted.probeCount) + " probes; spacing " +
      std::to_string(fitted.settings.volume.spacing.x) +
      " world units; probe payload " +
      std::to_string(double(fitted.payloadBytes) / (1024. * 1024.)) + " MiB.";
  if (subs_.guiManager)
    subs_.guiManager->acceptDdgiSceneFit(fitted.settings, ddgiFitStatus_);
}

void RendererFrontend::processPendingDdgiSceneFit() {
  if (subs_.guiManager)
    if (const auto request = subs_.guiManager->consumeDdgiSceneFitRequest())
      pendingDdgiSceneFit_ = request;
  if (!pendingDdgiSceneFit_)
    return;
  const auto budget = *std::exchange(pendingDdgiSceneFit_, std::nullopt);
  try {
    fitDdgiToActiveScene(budget);
  } catch (const std::invalid_argument &error) {
    ddgiFitStatus_ = "Scene fit rejected: " + std::string(error.what());
    if (subs_.guiManager && subs_.ddgiManager)
      subs_.guiManager->rejectDdgiSettings(subs_.ddgiManager->settings(),
                                           ddgiFitStatus_);
    // An invalid live request retains the last working lighting; startup fits
    // propagate their actionable error directly to the caller instead.
  }
}

void RendererFrontend::fitDdgiDetailToSelection(uint32_t maxProbes, std::string_view nodeName) {
  if (!subs_.ddgiManager)
    throw std::invalid_argument("DDGI manager is unavailable for detail fitting");
  refreshSceneObjectData();
  auto meshNode = sceneState_.selectedMeshNode;
  auto bimObject = selectedBimObjectIndex_;
  constexpr auto invalid = std::numeric_limits<uint32_t>::max();
  if (!nodeName.empty()) {
    meshNode = bimObject = invalid;
    for (uint32_t node = 0; node < sceneGraph_.nodeCount(); ++node)
      if (sceneGraph_.getNode(node)->name == nodeName) {
        if (meshNode != invalid) throw std::invalid_argument("DDGI detail node name is ambiguous");
        meshNode = node;
      }
    if (subs_.bimManager)
      for (uint32_t object = 0; object < subs_.bimManager->objectData().size(); ++object)
        if (const auto metadata = subs_.bimManager->metadataForObject(object);
            metadata && (metadata->displayName == nodeName || metadata->guid == nodeName)) {
          if (bimObject != invalid && !sameBimProductIdentity(
                *subs_.bimManager->metadataForObject(bimObject), *metadata))
            throw std::invalid_argument("DDGI detail element name is ambiguous; use its GUID");
          bimObject = object;
        }
    if ((meshNode == invalid) == (bimObject == invalid))
      throw std::invalid_argument("DDGI detail node/element is missing or ambiguous");
  }
  std::vector<DrawCommand> draws;
  std::vector<RaySceneProviderSource> sources;
  if (bimObject != invalid && subs_.bimManager && subs_.bimManager->hasScene()) {
    const auto selected = subs_.bimManager->metadataForObject(bimObject);
    if (!selected) throw std::invalid_argument("Selected BIM element has no metadata");
    auto append = [&](const auto &commands) {
      for (const auto &command : commands)
        for (uint32_t instance = 0; instance < command.instanceCount; ++instance) {
          const auto object = uint64_t(command.objectIndex) + instance;
          if (object >= subs_.bimManager->objectData().size()) continue;
          if (const auto metadata = subs_.bimManager->metadataForObject(uint32_t(object));
              metadata && sameBimProductIdentity(*selected, *metadata))
            draws.push_back({uint32_t(object), command.firstIndex, command.indexCount, 1});
        }
    };
    append(subs_.bimManager->opaqueDrawCommands());
    append(subs_.bimManager->transparentDrawCommands());
    sources.push_back({{"bim-detail"}, subs_.bimManager->geometryRevision(),
        subs_.bimManager->vertices(), subs_.bimManager->indices(),
        subs_.bimManager->objectData(), draws});
  } else if (meshNode != invalid && subs_.sceneController && subs_.sceneManager) {
    std::vector<uint32_t> nodes{meshNode};
    std::vector<DrawCommand> selected;
    for (size_t index = 0; index < nodes.size(); ++index) {
      const auto *node = sceneGraph_.getNode(nodes[index]);
      if (!node) continue;
      nodes.insert(nodes.end(), node->children.begin(), node->children.end());
      subs_.sceneController->collectDrawCommandsForNode(nodes[index], selected);
      draws.insert(draws.end(), selected.begin(), selected.end());
    }
    sources.push_back({{"mesh-detail"}, subs_.sceneController->geometryRevision(),
        subs_.sceneController->geometryVertices(), subs_.sceneController->geometryIndices(),
        subs_.sceneController->objectData(), draws});
  }
  const auto bounds = computeDdgiSceneBounds(sources);
  if (!bounds.valid)
    throw std::invalid_argument("Select a room or element with triangle geometry to fit a detail volume");
  const auto result = fitDdgiDetailVolume(subs_.ddgiManager->settings(), bounds, {maxProbes});
  subs_.ddgiManager->applySettings(result.settings);
  if (subs_.temporalManager) subs_.temporalManager->reset("DDGI detail volume fitted");
  ddgiFitStatus_ = "Added a padded detail volume with " + std::to_string(result.addedProbeCount) + " probes.";
  if (subs_.guiManager) {
    subs_.guiManager->acceptDdgiSceneFit(result.settings, ddgiFitStatus_);
    subs_.guiManager->selectDdgiVolume(result.volumeIndex);
  }
}

void RendererFrontend::processPendingDdgiDetailFit() {
  if (!subs_.guiManager || !subs_.ddgiManager) return;
  if (const auto request = subs_.guiManager->consumeDdgiDetailFitRequest()) {
    try { fitDdgiDetailToSelection(*request); }
    catch (const std::invalid_argument &error) {
      ddgiFitStatus_ = "Detail fit rejected: " + std::string(error.what());
      subs_.guiManager->rejectDdgiSettings(subs_.ddgiManager->settings(), ddgiFitStatus_);
    }
  }
}

std::string RendererFrontend::ddgiActiveSceneIdentity() const {
  return ddgiSceneIdentity(container::util::pathFromUtf8(activePrimaryModelPath_),
                           container::util::pathFromUtf8(activeAuxiliaryModelPath_),
                           activePrimaryImportScale_, activeAuxiliaryImportScale_);
}

void RendererFrontend::processPendingDdgiPreset() {
  if (!subs_.guiManager || !subs_.ddgiManager)
    return;
  const auto action = subs_.guiManager->consumeDdgiPresetRequest();
  if (!action)
    return;
  std::filesystem::path path;
  try {
    const auto identity = ddgiActiveSceneIdentity();
    path = ddgiScenePresetPath(identity);
    if (*action == container::ui::GuiManager::DdgiPresetAction::Save) {
      saveDdgiPreset(path, {identity, subs_.ddgiManager->settings()});
      subs_.guiManager->setDdgiPresetStatus(container::util::pathToUtf8(path),
                                            "Scene GI preset saved.");
    } else {
      const auto preset = loadDdgiPreset(path);
      if (preset.sceneIdentity != identity)
        throw std::invalid_argument("Preset belongs to a different scene or import scale.");
      const auto previous = subs_.ddgiManager->settings();
      subs_.ddgiManager->applySettings(preset.settings);
      subs_.guiManager->acceptDdgiSceneFit(preset.settings, "Loaded saved scene volumes.");
      if (previous != preset.settings && subs_.temporalManager)
        subs_.temporalManager->reset("DDGI scene preset loaded");
      fittedDdgiVolume_.reset();
      subs_.guiManager->setDdgiPresetStatus(container::util::pathToUtf8(path),
                                            "Scene GI preset loaded.");
    }
  } catch (const std::exception &error) {
    subs_.guiManager->setDdgiPresetStatus(container::util::pathToUtf8(path),
                                          "Preset operation failed: " + std::string(error.what()));
  }
}

void RendererFrontend::presentSceneControls() {
  if (!subs_.guiManager)
    return;
  ensureSceneEditHistory();
  const bool transforming = transformDragSession_.active || sceneEdits_->transformGestureActive();
  subs_.guiManager->setEditHistoryState(
      sceneEdits_->canUndo() && !transforming,
      sceneEdits_->canRedo() && !transforming,
      std::string(sceneEdits_->undoLabel()), std::string(sceneEdits_->redoLabel()));
  presentPhysicsControls();
  subs_.guiManager->setOpenVdbVolumeState(
      bool(subs_.openVdbVolumeRenderer),
      subs_.openVdbVolumeRenderer ? subs_.openVdbVolumeRenderer->settings()
                                  : svc_.config.vdbVolumeSettings,
      (svc_.config.vdbSequenceDirectory.empty() ? svc_.config.vdbVolumePath :
          svc_.config.vdbSequenceDirectory) +
          (subs_.openVdbVolumeRenderer ? " (" + std::to_string(subs_.openVdbVolumeRenderer->volumeCount()) + " media)" : ""),
      !svc_.config.vdbTemperatureGridName.empty());
  if (vdbPlayback_) {
    const auto& manifest = vdbAsyncSequence_ ? vdbAsyncSequence_->manifest() : vdbSequence_->manifest();
    subs_.guiManager->setOpenVdbSequenceState({
        .available = true,
        .paused = vdbPlayback_->options().paused,
        .loop = vdbPlayback_->options().loop,
        .speed = static_cast<float>(vdbPlayback_->options().speed),
        .frameIndex = static_cast<uint32_t>(vdbPlayback_->frameIndex()),
        .frameCount = static_cast<uint32_t>(manifest.frames.size()),
        .cachedBytes = vdbAsyncSequence_ ? vdbAsyncSequence_->status().residentBytes : vdbSequence_->cachedBytes(),
        .asynchronous = bool(vdbAsyncSequence_),
        .loading = !vdbLoadedFrame_ || *vdbLoadedFrame_ != vdbPlayback_->frameIndex(),
        .uploading = vdbUploadingFrame_.has_value(),
        .displayedFrameIndex = static_cast<uint32_t>(vdbLoadedFrame_.value_or(0))});
  }

  // A load replaces scene lighting/bloom defaults. Publish the new scene's
  // values below, otherwise GUI writeback restores the previous scene's values.

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
        {sceneRenderExtent().width, sceneRenderExtent().height},
        subs_.temporalManager->allocatedBytes(), [&] {
          if (subs_.rendererTelemetry)
            for (const auto &pass : subs_.rendererTelemetry->latest().passes)
              if (pass.name == "TemporalResolve" && pass.gpuTimed)
                return pass.gpuKnownMs;
          return 0.0f;
        }());
  if (subs_.ddgiManager) {
    const auto &gi = *subs_.ddgiManager;
    const auto batch = gi.lastBatch();
    container::ui::GuiDdgiRuntimeState runtime{
        .supported = gi.supported(),
        .active = gi.active(),
        .status = gi.status(),
        .allocatedBytes = gi.allocatedBytes(),
        .updateFrame = gi.frameNumber(),
        .contentEpoch = gi.contentEpoch(),
        .refreshRemainingProbes = gi.refreshRemainingProbes(),
        .schedulingCursor = gi.schedulingCursor(),
        .updatedProbes = batch.probeCount,
        .tracedRays = batch.rayCount};
    if (subs_.rendererTelemetry)
      for (const auto &pass : subs_.rendererTelemetry->latest().passes) {
        if (pass.gpuTimed && pass.name == "DdgiTrace")
          runtime.traceGpuMs = pass.gpuKnownMs;
        if (pass.gpuTimed && pass.name == "DdgiUpdate")
          runtime.updateGpuMs = pass.gpuKnownMs;
      }
    subs_.guiManager->setDdgiState(gi.settings(), std::move(runtime));
  }
  subs_.guiManager->setUpscalingStatus(subs_.temporalManager && subs_.temporalManager->upscalingActive()
      ? std::optional{upscaleResourceRequest_.selection.backend} : std::nullopt, upscaleViewportStatus_);
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
  container::ui::BimInspectionState bimInspection{};
  if (!subs_.bimManager || !subs_.bimManager->hasScene())
    bimInspectionCache_.clear();
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
    bimInspectionCache_.refresh(subs_.bimManager.get(),
                                subs_.bimManager->geometryRevision(),
                                elementMetadata);
    bimInspection.hasScene = true;
    bimInspection.metadataSource = subs_.bimManager.get();
    bimInspection.metadataRevision = subs_.bimManager->metadataRevision();
    bimInspection.elements = elementMetadata;
    bimInspection.selectObject = [this](uint32_t objectIndex) {
      selectBimObject(objectIndex);
    };
    bimInspection.focusObject = [this](uint32_t objectIndex) {
      selectBimObject(objectIndex);
      if (selectedBimObjectIndex_ != objectIndex || !subs_.cameraController)
        return;
      syncCameraSelectionPivotOverride();
      subs_.cameraController->frameNodeOrScene(
          container::scene::SceneGraph::kInvalidNode);
    };
    bimInspection.modelPath = subs_.bimManager->modelPath();
    bimInspection.importReport = &subs_.bimManager->importReport();
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
        bimInspectionCache_.byClassAndStorey();
    bimInspection.scheduleByTypeAndStoreyRows =
        bimInspectionCache_.byTypeAndStorey();
    bimInspection.scheduleByMaterialRows =
        bimInspectionCache_.byMaterial();
    bimInspection.modelCompareElements = bimInspectionCache_.compareElements();
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
    if (const auto metadata =
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
        (void)this->addScenePrimitive(kind);
      };
  refreshMaterialInspector();
  refreshCameraProjectionInspector();
  refreshCameraViewPoseInspector();
  refreshLightEditInspector();
  const auto* gltfSceneManager = subs_.sceneManager.get();
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
        if (!applySceneNodeTransform(sceneState_.rootNode, controls))
          return;
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
        pendingLightSelection_=id;
      },
      sceneState_.selectedMeshNode, bimInspection, currentViewpoint,
      [this](const container::ui::ViewpointSnapshotState &snapshot) {
        return restoreViewpointSnapshot(snapshot);
      },
      sceneState_.rootNode,
      [this](uint32_t nodeIndex) {
        finishEditGestures();
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
        finishEditGestures();
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
        finishEditGestures();
        ensureSceneEditHistory();
        if (!sceneEdits_->setVisible(nodeIndex, visible)) return;
        if (subs_.guiManager) {
          subs_.guiManager->setStatusMessage(
              std::string(visible ? "Showed" : "Hid") + " scene node " +
              std::to_string(nodeIndex));
        }
      },
      [this](uint32_t nodeIndex, std::optional<uint32_t> parentNode) {
        try {
          if (!reparentSceneNode(nodeIndex, parentNode)) return;
        } catch (const std::exception &error) {
          subs_.guiManager->setStatusMessage(
              "Cannot reparent scene node " + std::to_string(nodeIndex) +
              ": " + error.what());
          return;
        }
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
        if (!applySceneNodeTransform(nodeIndex, controls))
          return;
        if (nodeIndex == sceneState_.rootNode && subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        refreshSceneObjectData();
      },
      [this](uint32_t nodeIndex, bool selectable) {
        finishEditGestures();
        ensureSceneEditHistory();
        sceneEdits_->setSelectable(nodeIndex, selectable);
      },
      [this](uint32_t nodeIndex, bool hoverable) {
        finishEditGestures();
        ensureSceneEditHistory();
        sceneEdits_->setHoverable(nodeIndex, hoverable);
      },
      gltfSceneManager ? container::ui::GltfInspectorSource{
          &gltfSceneManager->animationSource(), gltfSceneManager->authoredMaterialResources().images,
          gltfSceneManager->generationEpoch()} : container::ui::GltfInspectorSource{});

  processLightEditRequests();
  processMaterialEditRequests();
  processCameraProjectionRequests();
  processCameraViewPoseRequests();
  if (!subs_.guiManager->sceneEditGestureActive() && !transformDragSession_.active)
    sceneEdits_->finishGesture();
  processSceneNodeAuthoringRequest();
  if (pendingSceneRetirement_) publishSceneEdit();
  applyEditHistoryRequest();
  processGltfSceneRequest();
  processMaterialVariantRequest();
  if (sceneEditPublicationPending_) publishSceneEdit();

  if (auto path = subs_.guiManager->consumeSceneSaveRequest()) {
    try {
      saveSceneModel(container::util::pathFromUtf8(*path));
      subs_.guiManager->acceptSceneSave(*path);
    } catch (const std::exception& error) {
      subs_.guiManager->setStatusMessage("Cannot save scene: " + std::string(error.what()));
    }
  }
  if (auto path = subs_.guiManager->consumeLinkedSceneSaveRequest()) {
    try {
      saveSceneModel(container::util::pathFromUtf8(*path), true);
      subs_.guiManager->setStatusMessage("Saved linked scene: " + *path);
    } catch (const std::exception& error) {
      subs_.guiManager->setStatusMessage("Cannot save linked scene: " + std::string(error.what()));
    }
  }

  if (auto requested = subs_.guiManager->consumeRenderEngineChange()) {
    requestRenderTechnique(*requested);
  }

  if (auto msaaRequest = subs_.guiManager->consumeMsaaSampleChange()) {
    const VkSampleCountFlagBits requestedSampleCount =
        clampMsaaSampleCount(*msaaRequest, msaaSampleMask_);
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

  drawComfyUi();
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
  if (subs_.openVdbVolumeRenderer) {
    subs_.openVdbVolumeRenderer->setSettings(
        subs_.guiManager->openVdbVolumeSettings());
  }
  if (vdbPlayback_) {
    const auto& manifest = vdbAsyncSequence_ ? vdbAsyncSequence_->manifest() : vdbSequence_->manifest();
    const auto& controls = subs_.guiManager->openVdbSequenceControls();
    vdbPlayback_->setPaused(controls.paused);
    vdbPlayback_->setLoop(controls.loop);
    if (std::isfinite(controls.speed)) vdbPlayback_->setSpeed(std::clamp(controls.speed, 0.0f, 16.0f));
    if (const auto seek = subs_.guiManager->consumeOpenVdbSequenceSeek())
      vdbPlayback_->seekSeconds(double(*seek) / manifest.framesPerSecond);
  }
  if (subs_.ddgiManager) {
    auto &gi = *subs_.ddgiManager;
    const auto previous = gi.settings();
    const auto resetsBefore = gi.historyResets();
    const auto frameBefore = gi.frameNumber();
    try {
      // The manager validates edits and retires GPU work before replacing
      // shared resources. Settings reach wantsScene/update in this frame.
      gi.applySettings(subs_.guiManager->ddgiSettings());
      const auto &current = gi.settings();
      const bool appearanceChanged =
          previous.enabled != current.enabled ||
          previous.intensity != current.intensity ||
          previous.volume != current.volume ||
          previous.additionalVolumes != current.additionalVolumes ||
          previous.volumeBlendCells != current.volumeBlendCells ||
          previous.thinGlassTransmission != current.thinGlassTransmission ||
          previous.dielectricTransmission != current.dielectricTransmission ||
          previous.cameraFollow != current.cameraFollow ||
          previous.resources.irradianceInteriorTexels !=
              current.resources.irradianceInteriorTexels ||
          previous.resources.visibilityInteriorTexels !=
              current.resources.visibilityInteriorTexels ||
          previous.resources.updates.raysPerProbe !=
              current.resources.updates.raysPerProbe ||
          resetsBefore != gi.historyResets() || frameBefore != gi.frameNumber();
      if (appearanceChanged && subs_.temporalManager)
        subs_.temporalManager->reset("DDGI settings changed");
    } catch (const std::invalid_argument &error) {
      subs_.guiManager->rejectDdgiSettings(gi.settings(), error.what());
    }
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
    const bool directionalIntensityChanged =
        guiLightingSettings.directionalIntensity !=
        currentLightingSettings.directionalIntensity;
    const bool environmentIntensityChanged =
        guiLightingSettings.environmentIntensity !=
        currentLightingSettings.environmentIntensity;
    const bool bounceIntensityChanged =
        guiLightingSettings.bounceIntensity != currentLightingSettings.bounceIntensity;
    const bool lightingSettingsChanged =
        guiLightingSettings.preset != currentLightingSettings.preset ||
        guiLightingSettings.density != currentLightingSettings.density ||
        guiLightingSettings.radiusScale !=
            currentLightingSettings.radiusScale ||
        guiLightingSettings.intensityScale !=
            currentLightingSettings.intensityScale ||
        directionalIntensityChanged || environmentIntensityChanged ||
        guiLightingSettings.environment != currentLightingSettings.environment ||
        bounceIntensityChanged ||
        guiLightingSettings.localShadowPointBudget !=
            currentLightingSettings.localShadowPointBudget ||
        guiLightingSettings.localShadowLayerBudget !=
            currentLightingSettings.localShadowLayerBudget ||
        guiLightingSettings.areaLightingMode !=
            currentLightingSettings.areaLightingMode ||
        guiLightingSettings.areaLightSampleCount !=
            currentLightingSettings.areaLightSampleCount ||
        guiLightingSettings.areaEmitterDebug !=
            currentLightingSettings.areaEmitterDebug;
    if (lightingSettingsChanged) {
      sceneLightingOverrides_.directionalIntensity |= directionalIntensityChanged;
      sceneLightingOverrides_.environmentIntensity |= environmentIntensityChanged;
      sceneLightingOverrides_.bounceIntensity |= bounceIntensityChanged;
      if (subs_.temporalManager)
        subs_.temporalManager->reset("lighting settings changed");
      if (subs_.ddgiManager && guiLightingSettings.environment.yawDegrees !=
                                   currentLightingSettings.environment.yawDegrees)
        subs_.ddgiManager->resetSceneHistory();
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
      useLocalAxes = interaction.transformSpace == container::ui::TransformSpace::Local &&
                     (light.type == EditableLightType::Area ||
                      light.type == EditableLightType::Spot ||
                      light.type == EditableLightType::Directional);
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
  publishReferenceColorBinding(imageIndex);
}

void RendererFrontend::ensureExtendedPbrResources() {
  if (!subs_.sceneManager || !subs_.frameResourceManager ||
      !subs_.sceneManager->hasExtendedPbrMaterials() ||
      (subs_.activeTechnique &&
       subs_.activeTechnique->id() != RenderTechniqueId::DeferredRaster) ||
      (subs_.pbrAuxiliaryPass && subs_.pbrAuxiliaryPass->ready())) {
    return;
  }
  const auto count = subs_.frameResourceManager->frameCount();
  if (count == 0) return;
  // First activation can follow an animation or material edit. Descriptor sets
  // are shared across frame slots, so retire every reader before publication.
  if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());
  std::vector<VkImageView> depthViews;
  depthViews.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const auto* frame = subs_.frameResourceManager->frame(i);
    depthViews.push_back(msaaSampleCount_ == VK_SAMPLE_COUNT_1_BIT
                            ? frame->depthStencil.view
                            : frame->depthStencilMsaa.view);
  }
  auto pass = std::make_unique<PbrAuxiliaryPass>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  pass->createResources(activeShaderReload_ ? activeShaderReload_->directory
                                           : container::util::executableDirectory(),
                        subs_.sceneManager->descriptorSetLayout(),
                        resources_.gBufferFormats.depthStencil,
                        msaaSampleCount_, depthViews,
                        sceneRenderExtent());
  subs_.pbrAuxiliaryPass = std::move(pass);
}

FrameRecordParams
RendererFrontend::buildFrameRecordParams(uint32_t imageIndex) {
  ensureExtendedPbrResources();
  publishFrameRuntimeResourceBindings(imageIndex);
  syncSceneProviders();

  FrameRecordParams p{};
  if (referencePathActive() && subs_.referencePathTracePass)
    p.services.referencePathTrace = [this](VkCommandBuffer command, const FrameRecordParams& frame) {
      return recordReferencePathTrace(command, frame);
    };
  p.services.pbrAuxiliaryPass = subs_.pbrAuxiliaryPass.get();
  p.runtime.frameSlot = frame_.currentFrame;
  p.runtime.imageIndex = imageIndex;
  p.runtime.sceneExtent = sceneRenderExtent();
  p.runtime.activeTechnique =
      subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                       : RenderTechniqueId::DeferredRaster;
  p.registries.resourceContracts = subs_.frameResourceRegistry.get();
  p.registries.pipelineRecipes = subs_.pipelineRegistry.get();
  p.registries.resourceBindings = subs_.frameRuntimeResourceRegistry.get();
  p.scene.vertexSlice = sceneState_.vertexSlice;
  p.scene.previousVertexSlice = p.scene.vertexSlice;
  gltfDeformationCurrentBoundToGpu_ = false;
  gltfDeformationPreviousBoundToGpu_ = false;
  if (subs_.gltfDeformationPass) {
    p.scene.vertexSlice = subs_.gltfDeformationPass->currentVertices(imageIndex);
    p.scene.previousVertexSlice = subs_.gltfDeformationPass->previousVertices(imageIndex);
    gltfDeformationCurrentBoundToGpu_ = p.scene.vertexSlice.buffer != VK_NULL_HANDLE;
    gltfDeformationPreviousBoundToGpu_ = p.scene.previousVertexSlice.buffer != VK_NULL_HANDLE;
  }
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
    const auto* floorPlanSettings = captureBimFloorPlan_ ? captureBimFloorPlan_.get()
        : subs_.guiManager ? &subs_.guiManager->bimFloorPlanOverlayState() : nullptr;
    if (floorPlanSettings) {
      const auto &floorPlan = *floorPlanSettings;
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
    }

    if (captureBimAnnotations_ || subs_.guiManager != nullptr) {
      const bool issueMarkers = captureBimAnnotations_
          ? captureBimAnnotations_->issuePin.has_value() : bimLayers.markupLayerVisible;
      const bool clashMarkers = captureBimAnnotations_
          ? captureBimAnnotations_->clash.has_value() : bimLayers.clashLayerVisible;
      if (issueMarkers || clashMarkers) {
        BimCoordinationOverlayIssuePin capturePin;
        BimCoordinationOverlayClashPair captureClash;
        std::span<const BimCoordinationOverlayIssuePin> issuePins;
        std::span<const BimCoordinationOverlayClashPair> clashPairs;
        if (captureBimAnnotations_) {
          if (const auto &pin = captureBimAnnotations_->issuePin) {
            capturePin.position = *pin;
            issuePins = {&capturePin, 1};
          }
          if (const auto &clash = captureBimAnnotations_->clash) {
            captureClash = {.primaryObjectIndex = clash->objects[0],
                            .secondaryObjectIndex = clash->objects[1],
                            .hasPosition = true,
                            .position = clash->position};
            clashPairs = {&captureClash, 1};
          }
        } else {
          issuePins = subs_.guiManager->bimIssueOverlayPins();
        }
        const BimCoordinationOverlayResult coordinationOverlay =
            subs_.bimManager->buildCoordinationOverlay(
                {.spacesEnabled = false,
                 .mepXrayEnabled = false,
                 .clashesEnabled = clashMarkers,
                 .issuePinsEnabled = issueMarkers},
                clashPairs, issuePins);
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
              issueMarkers && !markerDrawData.issuePinDrawCommands.empty();
          p.bim.coordinationMarkers.clashMarkersEnabled =
              clashMarkers && !markerDrawData.clashDrawCommands.empty();
        }
      } else {
        subs_.bimManager->clearCoordinationMarkerGeometry();
      }

      auto& technical = p.bim.technicalElevation;
      if (captureBimAnnotations_) {
        technical.hiddenLineOverlay = captureBimAnnotations_->hiddenLines;
        const auto depthTest = captureBimAnnotations_->technicalLines;
        technical.enabled = technical.hiddenLineOverlay || depthTest.has_value();
        technical.depthTestLines = depthTest.value_or(true);
        technical.sectionCapsEnabled = false;
        if (depthTest.has_value()) {
          technical.lineColor = glm::vec3(0.02f);
          technical.overlayIntensity = 0.70f;
        }
      } else if (subs_.guiManager->editorOverlaysEnabled() &&
                 subs_.guiManager->wireframeSupported()) {
        const auto elevationStyle = subs_.guiManager->appliedBimElevationStyle();
        const auto& wireframe = subs_.guiManager->wireframeSettings();
        technical.hiddenLineOverlay =
            elevationStyle == container::ui::BimElevationTechnicalStyle::HiddenLine;
        const bool shadedWithLines =
            elevationStyle == container::ui::BimElevationTechnicalStyle::ShadedWithLines &&
            wireframe.enabled && wireframe.mode == container::ui::WireframeMode::Overlay;
        technical.enabled = technical.hiddenLineOverlay || shadedWithLines;
        technical.depthTestLines = technical.hiddenLineOverlay || wireframe.depthTest;
        technical.sectionCapsEnabled = technical.hiddenLineOverlay;
        if (shadedWithLines) {
          technical.lineColor = wireframe.color;
          technical.lineWidth = wireframe.lineWidth;
          technical.overlayIntensity = wireframe.overlayIntensity;
        }
      }
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
  const RenderTechniqueId technique = p.runtime.activeTechnique;
  if (subs_.guiManager && subs_.guiManager->editorOverlaysEnabled() &&
      subs_.guiManager->wireframeSupported() &&
      !(screenshot_.pending && screenshot_.sceneLinear) &&
      (technique == RenderTechniqueId::ForwardRaster ||
       technique == RenderTechniqueId::DeferredRaster)) {
    const auto& wireframe = subs_.guiManager->wireframeSettings();
    p.debug.wireframe = {
        .enabled = wireframe.enabled && !p.bim.technicalElevation.enabled &&
                   p.pipelineHandle(technique, "wireframe-depth") != VK_NULL_HANDLE &&
                   p.pipelineHandle(technique, "wireframe-no-depth") != VK_NULL_HANDLE,
        .mode = wireframe.mode == container::ui::WireframeMode::Full
            ? DeferredLightingWireframeMode::Full : DeferredLightingWireframeMode::Overlay,
        .depthTest = wireframe.depthTest,
        .color = wireframe.color,
        .lineWidth = svc_.ctx.wireframeWideLinesSupported ? wireframe.lineWidth : 1.0f,
        .overlayIntensity = wireframe.overlayIntensity};
  }
  p.debug.wireframeRasterModeSupported = svc_.ctx.wireframeRasterModeSupported;
  p.debug.wireframeWideLinesSupported = svc_.ctx.wireframeWideLinesSupported;
  p.debug.lineListWideLinesSupported = svc_.ctx.deviceWrapper->enabledFeatures().wideLines == VK_TRUE;
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
  if (subs_.bimManager != nullptr && (captureBimAnnotations_ || subs_.guiManager != nullptr)) {
    auto sectionPlane = subs_.guiManager ? subs_.guiManager->sectionPlaneState()
                                         : container::ui::SectionPlaneState{};
    if (captureBimAnnotations_)
      sectionPlane.visualPlaneVisible = captureBimAnnotations_->sectionVisual;
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

    auto capUi = subs_.guiManager ? subs_.guiManager->bimClipCapHatchingUiState()
                                  : container::ui::BimClipCapHatchingUiState{};
    if (captureBimAnnotations_) {
      capUi.capPreview = captureBimAnnotations_->capFill;
      capUi.hatchingPreview = captureBimAnnotations_->capHatch;
      capUi.sectionMarkersPreview = captureBimAnnotations_->sectionMarkers;
    }
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
      gltfAnimationCamera_ ? gltfAnimationCamera_->orthographic :
      (subs_.cameraController != nullptr &&
       subs_.cameraController->isOrthographic());
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
    if (!subs_.guiManager)
      p.shadows.shadowSettings.areaShadowQuality =
          svc_.config.areaShadowQuality;
    p.shadows.localShadowLayerCount =
        subs_.shadowManager->localShadowLayerCount();
    p.shadows.shadowManager = subs_.shadowManager.get();
  }
  const auto displayMode = frontendDisplayMode(
      subs_.guiManager.get(), configuredDisplayMode(svc_.config));
  p.debug.temporalForceReactive =
      (subs_.guiManager && (subs_.guiManager->showGeometryOverlay() ||
                            subs_.guiManager->showNormalValidation()));
  // Reference diagnostics are already display colors. Use the postprocess
  // raw-color mode also used by the raster object-space normals view.
  p.debug.displayMode = referencePathActive()
      ? (referencePathDisplay() == ReferencePathDisplayMode::StandardError
            ? static_cast<uint32_t>(container::ui::GBufferViewMode::ObjectSpaceNormals) : 0u)
      : static_cast<uint32_t>(displayMode);
  p.debug.screenReflections = svc_.config.screenReflections;
  if (subs_.rayShadowManager && subs_.rayShadowManager->settings().mode != RayShadowMode::Raster &&
      subs_.rayShadowManager->settings().debugLayer != 0)
    p.debug.screenReflections = ScreenReflectionMode::Off;
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
  p.services.screenReflectionPass = subs_.screenReflectionPass.get();
  p.reflectionHistory = {
      .cameraEpoch = subs_.temporalManager ? subs_.temporalManager->state().epoch() : 0u,
      .materialRevision = subs_.sceneManager ? subs_.sceneManager->materialRevision() : 0u,
      .lightRevision = subs_.lightingManager ? subs_.lightingManager->lightEditRevision() : 0u};
  p.services.volumetricFogPass = subs_.volumetricFogPass.get();
  p.volumetricFog = {.settings = svc_.config.volumetricFog, .cameraData = &buffers_.cameraData,
      .lightSet = subs_.lightingManager ? subs_.lightingManager->lightDescriptorSet(imageIndex) : VK_NULL_HANDLE,
      .historyRevision = p.scene.objectDataRevision ^ (p.bim.scene.objectDataRevision << 1) ^
          (subs_.lightingManager ? subs_.lightingManager->lightEditRevision() << 2 : 0u) ^
          (subs_.temporalManager ? uint64_t{subs_.temporalManager->state().epoch()} << 32 : 0u) ^
          temporalClipRevision_.value_or(0u)};
  p.services.gpuCullManager = subs_.gpuCullManager.get();
  p.services.bimManager = subs_.bimManager.get();
  p.services.bloomManager = subs_.bloomManager.get();
  p.services.openVdbVolumeRenderer = subs_.openVdbVolumeRenderer.get();
  p.services.telemetry = subs_.rendererTelemetry.get();
  p.services.gpuProfiler = subs_.renderPassGpuProfiler.get();
  p.services.rayShadowManager = subs_.rayShadowManager.get();
  p.services.ddgiManager = subs_.ddgiManager.get();
  p.services.ddgiReadDescriptorSet = subs_.ddgiManager
      ? subs_.ddgiManager->readDescriptors(imageIndex) : VK_NULL_HANDLE;
  p.postProcess.exposureSettings = effectiveExposureSettings();
  p.postProcess.displayGrade = subs_.guiManager ? subs_.guiManager->displayGrade()
                                               : svc_.config.displayGrade;
  p.postProcess.renderPass = resources_.renderPasses.postProcess;
  p.postProcess.encodeSrgbOutput = svc_.swapChainManager.needsShaderSrgbEncoding();
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
    p.screenshot.viewportOnly = screenshot_.viewportOnly;
    p.screenshot.sceneLinear = screenshot_.sceneLinear;
    p.screenshot.sourceImage = svc_.swapChainManager.image(imageIndex);
    if (frame_.currentFrame < screenshot_.readbacks.size()) {
      p.screenshot.readbackBuffer =
          screenshot_.readbacks[frame_.currentFrame].readbackBuffer.buffer;
    }
    p.screenshot.extent = svc_.swapChainManager.extent();
  }
  if (subs_.rayShadowManager && subs_.lightingManager) {
    subs_.rayShadowManager->setGpuTransformProducer(subs_.gpuRayInstancePass.get());
    std::vector<RaySceneProviderSource> sources;
    const bool hasSceneContent = !sceneGraph_.renderableNodes().empty() ||
        (subs_.bimManager && subs_.bimManager->hasScene());
    const bool giCompatible = p.bim.semanticColorMode == 0u;
    const bool giScene = subs_.ddgiManager && subs_.ddgiManager->wantsScene() &&
                         giCompatible && hasSceneContent;
    const bool reflectionScene = hasSceneContent && giCompatible && p.services.screenReflectionPass &&
        p.services.screenReflectionPass->wantsRayScene(p);
    const bool surfaceScene = giScene || referencePathActive() || reflectionScene;
    subs_.rayShadowManager->requestSurfaceScene(
        surfaceScene, subs_.ddgiManager
                     ? subs_.ddgiManager->settings().surfaceBudgetBytes
                     : svc_.config.ddgi.surfaceBudgetBytes);
    if (subs_.guiManager)
      subs_.rayShadowManager->settings() =
          subs_.guiManager->rayShadowSettings();
    const auto meshProvider = subs_.sceneProviderRegistry->providersForKind(
        container::scene::SceneProviderKind::Mesh);
    std::vector<DrawCommand> meshRayDraws;
    std::vector<RayOpacityMicromapRange> opacityMicromaps;
    if (surfaceScene) {
      meshRayDraws = subs_.sceneController->opaqueDrawCommands();
      const auto &transparent =
          subs_.sceneController->transparentDrawCommands();
      meshRayDraws.insert(meshRayDraws.end(), transparent.begin(),
                          transparent.end());
    }
    if (!meshProvider.empty() && subs_.rayShadowManager->supported() &&
        (surfaceScene ||
         subs_.rayShadowManager->settings().mode != RayShadowMode::Raster)) {
      sources.push_back(
          {meshProvider.front()->snapshot().id,
           subs_.sceneController->geometryRevision(),
           subs_.sceneController->geometryVertices(), subs_.sceneController->geometryIndices(),
           subs_.sceneController->objectData(),
           surfaceScene ? std::span<const DrawCommand>(meshRayDraws)
                   : std::span<const DrawCommand>(
                         subs_.sceneController->opaqueDrawCommands())});
      auto& primary = sources.back();
      // Forward MSAA uses the TLAS to prove raster receiver identity. The
      // proof must see every masked triangle, even those removed by a map.
      const bool forwardMsaa = p.runtime.activeTechnique == RenderTechniqueId::ForwardRaster &&
          msaaSampleCount_ != VK_SAMPLE_COUNT_1_BIT;
      const auto& indices = subs_.sceneManager->indices();
      if (svc_.ctx.deviceWrapper->opacityMicromapsEnabled() && !forwardMsaa &&
          primary.indices.data() == indices.data() && primary.indices.size() == indices.size()) {
        opacityMicromaps = subs_.sceneManager->rayOpacityMicromaps(primary.draws, primary.objects);
        primary.opacityMicromaps = opacityMicromaps;
      }
    }
    if(gpuHierarchyPrepared_ && subs_.gpuRayInstancePass && !sources.empty() && !sources.back().draws.empty()) {
      sources.back().gpuObjects={buffers_.objects.at(imageIndex).buffer,0,
          uint64_t(subs_.sceneController->objectData().size())*sizeof(container::gpu::ObjectData)};
      sources.back().gpuTransformRevision=shaderReloadGeneration_;
    }
    std::vector<DrawCommand> bimDraws;
    if (subs_.bimManager && subs_.bimManager->hasScene() &&
        subs_.rayShadowManager->supported() &&
        (surfaceScene || subs_.rayShadowManager->settings().mode != RayShadowMode::Raster)) {
      auto filter = currentBimDrawFilter();
      // Draw budget/LOD and screen visibility limit raster work, not blockers.
      filter.drawBudgetEnabled = false;
      filter.drawBudgetMaxObjects = 0;
      for (const auto &draw : subs_.bimManager->opaqueDrawCommands())
        if (subs_.bimManager->objectMatchesFilter(draw.objectIndex, filter) &&
            bimObjectVisibleByLayer(draw.objectIndex))
          bimDraws.push_back(draw);
      if (surfaceScene)
        for (const auto &draw : subs_.bimManager->transparentDrawCommands())
          if (subs_.bimManager->objectMatchesFilter(draw.objectIndex, filter) &&
              bimObjectVisibleByLayer(draw.objectIndex))
            bimDraws.push_back(draw);
      const auto bimProvider = subs_.sceneProviderRegistry->providersForKind(
          container::scene::SceneProviderKind::Bim);
      if (!bimProvider.empty())
        sources.push_back({bimProvider.front()->snapshot().id,
                           subs_.bimManager->geometryRevision(),
                           subs_.bimManager->vertices(),
                           subs_.bimManager->indices(),
                           subs_.bimManager->objectData(), bimDraws});
    }
    bool rayGeometryCompatible = true;
    for (const auto &source : sources)
      for (const auto &draw : source.draws)
        for (uint32_t instance = 0; instance < draw.instanceCount; ++instance) {
          const auto object = size_t(draw.objectIndex) + instance;
          if (object >= source.objects.size() ||
              !subs_.sceneManager->rayGeometryCompatible(
                  source.objects[object].objectInfo.x))
            rayGeometryCompatible = false;
        }
    subs_.rayShadowManager->update(
        imageIndex, buffers_.cameraData,
        *subs_.frameResourceManager->frame(imageIndex),
        subs_.sceneManager->descriptorSet(imageIndex),
        subs_.lightingManager->lightDescriptorSet(imageIndex), sources,
        subs_.lightingManager->lightingData(),
        subs_.lightingManager->pointLightsSsbo(),
        subs_.lightingManager->areaLightsSsbo(),
        pushConstants_.bindless.sectionPlaneEnabled,
        pushConstants_.bindless.sectionPlane, sceneClipState,
        [this](uint32_t material) {
          return subs_.sceneManager->temporalMaterialRevision(material);
        },
        rayGeometryCompatible);
    if (subs_.ddgiManager) {
      DdgiSchedulingHints scheduling;
      scheduling.cameraPosition = glm::vec3(buffers_.cameraData.cameraWorldPosition);
      scheduling.cameraForward = glm::vec3(buffers_.cameraData.cameraForward);
      const auto sceneRevision = subs_.rayShadowManager->sceneRevision();
      if (giScene && sceneRevision != ddgiTrackedSceneRevision_) {
        try {
          scheduling.dirtyRegions = ddgiSceneChanges_.update(sources, [this](uint32_t material) {
            return subs_.sceneManager->temporalMaterialRevision(material);
          });
          // A blocker can change transport for probes outside its own bounds.
          const glm::vec3 influence(subs_.ddgiManager->settings().maxDistance);
          bool finite = true;
          for (auto &region : scheduling.dirtyRegions) {
            region.minimum -= influence;
            region.maximum += influence;
            for (int axis = 0; axis < 3; ++axis)
              finite &= std::isfinite(region.minimum[axis]) && std::isfinite(region.maximum[axis]);
          }
          if (!finite)
            scheduling.dirtyRegions.clear(); // Conservative global priority.
          ddgiTrackedSceneRevision_ = sceneRevision;
        } catch (const std::invalid_argument &) {
          // Surface extraction already reports a raster fallback for malformed
          // geometry. Priority hints must not turn that fallback into a crash.
          ddgiSceneChanges_.clear();
          scheduling.dirtyRegions.clear();
          ddgiTrackedSceneRevision_ = 0;
        }
      } else if (!giScene) {
        ddgiSceneChanges_.clear();
        ddgiTrackedSceneRevision_ = 0;
      }
      subs_.ddgiManager->update(
          imageIndex, subs_.sceneManager->descriptorSet(imageIndex),
          subs_.lightingManager->lightDescriptorSet(imageIndex),
          subs_.lightingManager->lightingData(),
          subs_.lightingManager->pointLightsSsbo(),
          subs_.lightingManager->areaLightsSsbo(),
          pushConstants_.bindless.sectionPlaneEnabled,
          pushConstants_.bindless.sectionPlane, sceneClipState,
          subs_.environmentManager->prefilteredView(),
          subs_.environmentManager->envSampler(), giCompatible, scheduling);
    }
    if (subs_.guiManager && subs_.rayShadowManager->supported()) {
      const auto &ray = *subs_.rayShadowManager;
      std::string status =
          "Supported; allocations " +
          std::to_string(ray.allocatedBytes() / (1024 * 1024)) + " MiB";
      if (!rayGeometryCompatible)
        status += "; raster fallback for height-displaced geometry";
      if (const auto *stats = ray.buildStats())
        status += "; BLAS built/reused " + std::to_string(stats->blasBuilt) +
                  "/" + std::to_string(stats->blasReused) + "; instances " +
                  std::to_string(stats->instanceCount);
      if (subs_.rendererTelemetry) {
        for (const auto &pass : subs_.rendererTelemetry->latest().passes)
          if (pass.name == "RaySceneBuild" || pass.name == "RayShadowTrace" ||
              pass.name == "RayShadowFilter")
            status += "\n" + pass.name + ": " +
                      std::to_string(pass.gpuKnownMs) + " ms";
      }
      subs_.guiManager->setRayShadowSupport(true, std::move(status));
    }
  }
  return p;
}

void RendererFrontend::setCaptureBimFloorPlan(container::ui::BimFloorPlanOverlayState state) {
  captureBimFloorPlan_ = std::make_shared<const container::ui::BimFloorPlanOverlayState>(state);
}

void RendererFrontend::attachActiveTechniqueLifecycle(
    FrameRecordParams &params) {
  if (subs_.deferredRasterFrameGraphContext == nullptr) {
    return;
  }

  switch (params.runtime.activeTechnique) {
  case RenderTechniqueId::DeferredRaster:
  case RenderTechniqueId::ForwardRaster:
  case RenderTechniqueId::PathTracing:
    params.lifecycle =
        subs_.deferredRasterFrameGraphContext->lifecycleHooks();
    break;
  default:
    params.lifecycle = {};
    break;
  }
  if (subs_.gltfDeformationPass) {
    auto previous = std::move(params.lifecycle.afterCommandBufferBegin);
    params.lifecycle.afterCommandBufferBegin =
        [this, previous = std::move(previous)](VkCommandBuffer command,
                                               const FrameRecordParams& frame) {
          if (previous) previous(command, frame);
          subs_.gltfDeformationPass->record(command, frame.runtime.imageIndex);
        };
  }
  if (gpuHierarchyPrepared_ && subs_.gpuHierarchyPass) {
    auto previous = std::move(params.lifecycle.afterCommandBufferBegin);
    params.lifecycle.afterCommandBufferBegin =
        [this, previous = std::move(previous)](VkCommandBuffer command, const FrameRecordParams& frame) {
          if (previous) previous(command, frame);
          subs_.gpuHierarchyPass->record(command, frame.runtime.imageIndex);
        };
  }
  if (referencePathActive() && subs_.referencePathTracePass) {
    auto previous = std::move(params.lifecycle.afterGraphExecution);
    params.lifecycle.afterGraphExecution =
        [this, previous = std::move(previous)](VkCommandBuffer command, const FrameRecordParams& frame) {
          if (previous) previous(command, frame);
          finishReferencePathTrace(command);
        };
  }

}

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------

void RendererFrontend::syncSceneProviders() {
  if (!subs_.sceneProviderRegistry || !subs_.sceneProviderSynchronizer) {
    return;
  }

  subs_.sceneProviderSynchronizer->sync(*subs_.sceneProviderRegistry,
                                        buildSceneProviderSyncInput());
}

SceneProviderSyncInput RendererFrontend::buildSceneProviderSyncInput(const MeshSceneProviderSyncInput* preparedMesh) {
  SceneProviderSyncInput syncInput{};

  if(preparedMesh) {
    syncInput.mesh=*preparedMesh;
  } else if (subs_.sceneManager) {
    const auto &sceneManager = *subs_.sceneManager;
    auto meshBounds =
        sceneProviderBoundsFromModelBounds(sceneManager.modelBounds());
    if (subs_.sceneController && sceneManager.deformationData()) {
      const auto bounds = subs_.sceneController->nodeWorldBounds(sceneState_.rootNode);
      if (bounds.valid) meshBounds = {bounds.min, bounds.max, true};
    }
    const std::size_t meshInstanceCount =
        subs_.sceneController ? subs_.sceneController->objectData().size() : 0u;
    std::optional<container::scene::MeshSceneProviderBindingView> meshBindings;
    if(subs_.sceneController) {
      const auto& controller = *subs_.sceneController;
      meshBindings = MakeMeshSceneProviderBindingView(
          controller.objectData(), controller.opaqueDrawCommands(),
          controller.transparentDrawCommands(), uint64_t(controller.geometryIndices().size()));
    }
    const auto &meshAsset = meshProviderAssetCache_.get({
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
            .bindings = meshBindings,
        }, {.geometry = sceneManager.geometryRevision(),
            .materials = sceneManager.materialRevision()}, &sceneManager);
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
        .geometryRevision = subs_.sceneController ? subs_.sceneController->geometryRevision()
                                                 : sceneManager.geometryRevision(),
        .materialRevision = sceneManager.materialRevision(),
        .instanceRevision = objectRevision,
        .displayName = sceneProviderDisplayName(activePrimaryModelPath_,
                                                "Primary mesh scene"),
        .triangleBatchesAuthoritative = meshAsset.triangleBatchesAuthoritative,
    };
  } else {
    meshProviderAssetCache_.clear();
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
        .geometryRevision = subs_.bimManager->geometryRevision(),
        .instanceRevision = objectRevision,
        .displayName =
            sceneProviderDisplayName(activeAuxiliaryModelPath_, "BIM scene"),
    };
  }

  return syncInput;
}

void RendererFrontend::syncSceneStateFromController() {
  if (!subs_.sceneController)
    return;
  sceneState_.vertexSlice = subs_.sceneController->vertexSlice();
  sceneState_.indexSlice = subs_.sceneController->indexSlice();
}

} // namespace container::renderer
