#include "Container/renderer/core/RendererFrontend.h"
#include "Container/app/AppConfig.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/pathtracing/ReferencePathTracePass.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/lighting/EnvironmentManager.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/scene/CameraController.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/pipeline/RuntimeShaderCompiler.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/Logger.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <utility>

namespace container::renderer {
namespace {
void displayBarrier(VkCommandBuffer command, VkImage image, bool forReading) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = forReading ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = forReading ? VK_ACCESS_2_SHADER_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT;
  barrier.dstStageMask = forReading ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.dstAccessMask = forReading ? VK_ACCESS_2_MEMORY_READ_BIT : VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
  barrier.oldLayout = forReading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.newLayout = forReading ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(command, &dependency);
}
} // namespace

ReferencePathDisplayMode RendererFrontend::referencePathDisplay() const {
  const auto ground = subs_.guiManager ? subs_.guiManager->referenceGround() : svc_.config.pathGround;
  if (ground.mode == ReferenceGroundMode::ShadowMatte) return ReferencePathDisplayMode::Beauty;
  return subs_.guiManager ? subs_.guiManager->referencePathDisplay()
                          : referencePathDisplayOverride_.value_or(svc_.config.pathDisplay);
}

void RendererFrontend::ensureReferencePathResources() {
  referenceRecorded_ = false;
  if (!referencePathActive()) return;
  auto oidn = subs_.guiManager ? subs_.guiManager->referenceOidn() : svc_.config.pathOidn;
  if (!subs_.guiManager && referenceOidnQualityOverride_)
    oidn.quality = *referenceOidnQualityOverride_;
  if (!subs_.guiManager && referenceOptixOverride_)
    oidn.optix = *referenceOptixOverride_;
  if (!subs_.guiManager && referenceDlssOverride_)
    oidn.dlss = *referenceDlssOverride_;
  const auto shaderRoot = activeShaderReload_ ? activeShaderReload_->directory : container::util::executableDirectory();
  if (auto& current = subs_.referencePathTracePass) {
    if (current->optixSelected() == oidn.optix && (oidn.optix || current->oidnQuality() == oidn.quality)) {
      if (current->dlssQuality() != oidn.dlss) {
        subs_.frameSyncManager->waitForAllFrames();
        current->setDlssQuality(shaderRoot, oidn.dlss);
      }
      current->setOidnSampleInterval(oidn.sampleInterval);
      return;
    }
    subs_.frameSyncManager->waitForAllFrames();
    current.reset();
    referenceStatus_.clear();
  }
  if (!referenceStatus_.empty()) return;
  try {
    auto pass = std::make_unique<ReferencePathTracePass>(svc_.ctx.deviceWrapper,
        *svc_.allocationManager.memoryManager(), svc_.pipelineManager);
    pass->createResources(shaderRoot,
        uint32_t(svc_.swapChainManager.imageCount()), svc_.swapChainManager.extent(),
        subs_.sceneManager->descriptorSetLayout(), subs_.lightingManager->lightDescriptorSetLayout(),
        kDefaultReferenceImageBudgetBytes, oidn);
    subs_.referencePathTracePass = std::move(pass);
    ++referenceTimingGeneration_;
    referenceDiagnosticSeen_.assign(svc_.swapChainManager.imageCount(), 0);
    referenceDiagnosticEpoch_ = referenceUnsupported_ = referenceRejected_ = 0;
    referenceExperimentalMaterialFallbacks_ = 0;
    referenceAdaptiveBatch_.reset();
    referenceTimedLightingMs_.reset();
    referenceAutofocusStatus_.clear();
  } catch (const std::exception& error) {
    referenceStatus_ = std::string("Raster fallback: ") + error.what();
    container::log::ContainerLogger::instance().renderer()->warn("{}", referenceStatus_);
    initializeRenderTechnique(RenderTechniqueId::DeferredRaster, "path tracing resource fallback");
    syncGuiRenderEngineOptions();
    if (subs_.guiManager) subs_.guiManager->setStatusMessage(referenceStatus_);
  }
}

bool RendererFrontend::recordReferencePathTrace(VkCommandBuffer command, const FrameRecordParams& frame) {
  if (!referencePathActive() || !subs_.referencePathTracePass) return false;
  const auto image = frame.runtime.imageIndex;
  auto& pass = *subs_.referencePathTracePass;
  ReferencePathTraceInput input;
  input.displayMode = referencePathDisplay();
  input.settings.samplesPerFrame = referenceAdaptiveBatch_.value_or(svc_.config.pathSamplesPerFrame);
  input.settings.resetSamplesPerFrame = svc_.config.pathSamplesPerFrame;
  input.settings.sampleLimit = screenshot_.pending
      ? screenshot_.referenceSampleTarget.value_or(svc_.config.pathSampleLimit)
      : svc_.config.pathSampleLimit;
  input.settings.maxBounces = svc_.config.pathMaxBounces;
  input.settings.seed = svc_.config.pathSeed;
  input.settings.lens = subs_.guiManager ? subs_.guiManager->referencePathLens() : svc_.config.pathLens;
  if (!subs_.guiManager && referenceFocusDistanceOverride_)
    input.settings.lens.focusDistance = *referenceFocusDistanceOverride_;
  input.settings.fireflyClamp =
      subs_.guiManager ? subs_.guiManager->referenceFireflyClamp() : svc_.config.pathFireflyClamp;
  input.settings.ground = subs_.guiManager ? subs_.guiManager->referenceGround() : svc_.config.pathGround;
  const auto extent = svc_.swapChainManager.extent();
  const auto* camera = subs_.cameraController->camera();
  input.data.inverseProjection = glm::inverse(gltfAnimationCamera_ ? gltfAnimationCamera_->projection :
      camera->projectionMatrix(float(extent.width) / float(extent.height)));
  input.data.inverseView = glm::inverse(gltfAnimationCamera_ ? gltfAnimationCamera_->view : camera->viewMatrix());
  input.data.boxClip = subs_.sceneManager->sceneClipState();
  input.data.sectionPlane = pushConstants_.bindless.sectionPlane;
  input.data.limits.z = uint32_t(subs_.sceneManager->gpuMaterialCount());
  input.data.limits.w = pushConstants_.bindless.sectionPlaneEnabled ? 1u : 0u;
  if (frame.camera.orthographic) input.data.limits.w |= 2u;
  input.data.limits.w |= 4u;
  if (subs_.sceneManager->hasVolumeMaterials()) input.data.limits.w |= 16u;
  input.materials = subs_.sceneManager->descriptorSet(image);
  input.lights = subs_.lightingManager->lightDescriptorSet(image);
  input.environment = subs_.environmentManager->radianceView();
  input.environmentSampler = subs_.environmentManager->envSampler();
  input.scene = subs_.rayShadowManager->surfaceBindings(image);
  input.emptyScene = !input.scene;
  referenceStatus_.clear();
  if (input.emptyScene && (!sceneGraph_.renderableNodes().empty() || (subs_.bimManager && subs_.bimManager->hasScene()))) {
    input.data.limits.w |= 8u;
    referenceStatus_ = "Unsupported scene: " + subs_.rayShadowManager->surfaceUnsupportedReason();
  }
  // These paths cannot yet provide the same radiometric scene as raster.
  if (frame.bim.semanticColorMode || vdbLoadedFrame_) {
    input.data.limits.w |= 8u;
    referenceStatus_ = "Unsupported reference scene: semantic colors or volume transport";
  }
  if (subs_.environmentManager->activeSkyMode() == SkyMode::Atmosphere) {
    input.data.limits.w |= 8u;
    referenceStatus_ = "Unsupported reference scene: atmosphere and solar transport";
  }
  // Hash only radiometric inputs. Camera jitter, raster shadow controls, exposure
  // and the display grade must not discard the accumulated linear samples.
  uint64_t revision = 14695981039346656037ull;
  const auto hashValue = [](uint64_t& state, const auto& value) {
    for (auto byte : std::as_bytes(std::span(&value, 1))) {
      state ^= std::to_integer<uint8_t>(byte);
      state *= 1099511628211ull;
    }
  };
  const auto hash = [&](const auto& value) { hashValue(revision, value); };
  hash(shaderReloadGeneration_);
  const auto autofocusKey = [&] {
    uint64_t key = revision;
    hashValue(key, subs_.rayShadowManager->sceneRevision());
    hashValue(key, extent);
    hashValue(key, input.data.inverseProjection);
    hashValue(key, input.data.inverseView);
    hashValue(key, input.data.boxClip);
    hashValue(key, input.data.sectionPlane);
    hashValue(key, input.data.limits.w);
    hashValue(key, input.settings.lens.focusDistance);
    const bool focusOnGround = input.settings.ground.mode == ReferenceGroundMode::Surface;
    hashValue(key, focusOnGround);
    if (focusOnGround) hashValue(key, input.settings.ground.height);
    return key;
  };
  const auto autofocusStatus = [&](std::string status) {
    referenceAutofocusStatus_ = std::move(status);
    if (subs_.guiManager)
      subs_.guiManager->setStatusMessage(referenceAutofocusStatus_);
  };
  // The serialized graphics policy retires the previous probe before recording
  // this frame. Discard it if the visible geometry or manual focus changed.
  if (const auto probe = pass.takeAutofocusResult()) {
    auto lens = input.settings.lens;
    lens.focusDistance = probe->distance;
    if (probe->key != autofocusKey()) {
      autofocusStatus("Autofocus discarded after camera, scene, or focus changed");
    } else if (!lens.valid()) {
      autofocusStatus("No autofocus target at viewport center");
    } else {
      input.settings.lens = lens;
      if (subs_.guiManager) subs_.guiManager->setReferencePathLens(lens);
      else referenceFocusDistanceOverride_ = lens.focusDistance;
      autofocusStatus("Focused at viewport center");
    }
  }
  const bool guiAutofocus = subs_.guiManager && subs_.guiManager->consumeReferenceAutofocusRequest();
  if (std::exchange(referenceAutofocusRequested_, false) || guiAutofocus) {
    if (frame.camera.orthographic) autofocusStatus("Autofocus requires a perspective camera");
    else if ((input.emptyScene && input.settings.ground.mode != ReferenceGroundMode::Surface) ||
             (input.data.limits.w & 8u) != 0u)
      autofocusStatus("No autofocus target at viewport center");
    else {
      input.autofocusKey = autofocusKey();
      autofocusStatus("Measuring focus at viewport center");
    }
  }
  hash(subs_.environmentManager->environmentRevision());
  const auto& lighting = subs_.lightingManager->lightingData();
  hash(lighting.directionalDirection);
  hash(lighting.directionalColorIntensity);
  hash(lighting.environmentIntensity);
  hash(lighting.environmentOrientation.x);
  hash(lighting.environmentOrientation.y);
  hash(lighting.pointLightCount);
  hash(lighting.areaLightCount);
  for (const auto& light : subs_.lightingManager->pointLightsSsbo()) hash(light);
  for (const auto& light : subs_.lightingManager->areaLightsSsbo()) hash(light);
  input.contentRevision = revision;
  hashValue(input.contentRevision, subs_.rayShadowManager->sceneRevision());
  if (pass.dlssQuality() != UpscaleMode::Native) {
    input.dlssRevision = revision;
    hashValue(input.dlssRevision, sceneGraph_.identity());
    hashValue(input.dlssRevision, subs_.sceneManager->materialRevision());
    input.dlssMotion = subs_.rayShadowManager->surfaceMotionSnapshot(image);
    input.frameMilliseconds = temporalFrameMilliseconds_;
  }
  pass.prepareFrame(image, input, subs_.sceneManager->gpuMaterialData(),
      subs_.rayShadowManager->surfaceInstanceMetadata(image));
  pass.record(command, image);
  displayBarrier(command, pass.displayImage(), true);
  referenceRecorded_ = true;
  return true;
}

void RendererFrontend::finishReferencePathTrace(VkCommandBuffer command) {
  if (referenceRecorded_ && subs_.referencePathTracePass)
    displayBarrier(command, subs_.referencePathTracePass->displayImage(), false);
}

void RendererFrontend::collectReferenceDiagnostics(uint32_t image) {
  auto* pass = subs_.referencePathTracePass.get();
  if (!pass || image >= referenceDiagnosticSeen_.size()) return;
  const auto serial = pass->diagnosticSubmission(image);
  if (!serial || referenceDiagnosticSeen_[image] == serial) return;
  pass->collectDenoiserTiming(image);
  referenceDiagnosticSeen_[image] = serial;
  const auto epoch = pass->diagnosticEpoch(image);
  if (epoch != pass->historyResets()) return;
  if (referenceDiagnosticEpoch_ != epoch) {
    referenceDiagnosticEpoch_ = epoch;
    referenceUnsupported_ = referenceRejected_ = 0;
    referenceExperimentalMaterialFallbacks_ = 0;
  }
  const auto diagnostics = pass->diagnostics(image);
  referenceUnsupported_ += diagnostics.unsupportedSamples;
  referenceRejected_ += diagnostics.rejectedSamples;
  const uint32_t fallbacks = diagnostics.failureStages & ReferencePathTraceDiagnostics::kExperimentalMaterialFallbackMask;
  const uint32_t newFallbacks = fallbacks & ~referenceExperimentalMaterialFallbacks_;
  referenceExperimentalMaterialFallbacks_ |= fallbacks;
  if (newFallbacks & ReferencePathTraceDiagnostics::kClosedVolumeRetroreflectionFallback)
    container::log::ContainerLogger::instance().renderer()->warn(
        "Closed-volume glTF retroreflection is unsupported. Using ordinary volume transport.");
  if (newFallbacks & ReferencePathTraceDiagnostics::kClosedVolumeScatterFallback)
    container::log::ContainerLogger::instance().renderer()->warn(
        "Closed-volume glTF scatter is unsupported. Using ordinary volume transport.");
  if (diagnostics.rejectedSamples != 0)
    std::fprintf(stderr,
        "[ReferencePath] rejected=%u accepted=%u unsupported=%u failureStages=0x%08x epoch=%llu submission=%llu\n",
        diagnostics.rejectedSamples, diagnostics.acceptedSamples, diagnostics.unsupportedSamples,
        diagnostics.failureStages & ~ReferencePathTraceDiagnostics::kExperimentalMaterialFallbackMask,
        static_cast<unsigned long long>(epoch), static_cast<unsigned long long>(serial));
}

void RendererFrontend::updateReferenceSampleBudget(float lightingMs, uint32_t samples,
                                                    uint64_t generation, uint64_t epoch) {
  const auto* path = subs_.referencePathTracePass.get();
  const float targetMs = svc_.config.pathTargetMs;
  if (!referencePathActive() || !path || generation != referenceTimingGeneration_ ||
      epoch != path->historyResets() || samples == 0 ||
      path->attemptedSamples() == svc_.config.pathSampleLimit)
    return;

  referenceAdaptiveBatch_.reset();
  referenceTimedLightingMs_.reset();
  if (!std::isfinite(lightingMs) || lightingMs <= 0.0f)
    return;

  // Fit the next batch to the retired Lighting pass, changing by at most 2x.
  referenceTimedLightingMs_ = lightingMs;
  const double estimate = std::floor(double(samples) * targetMs / lightingMs);
  const double lower = double((samples + 1u) / 2u);
  const double upper = double(std::min(64u, samples * 2u));
  referenceAdaptiveBatch_ = static_cast<uint32_t>(std::clamp(estimate, lower, upper));
}

void RendererFrontend::updateReferenceColorSource(uint32_t image, bool sceneLinear) {
  if (!subs_.frameResourceManager || !subs_.frameResourceManager->frame(image)) return;
  if (referenceColorBound_.size() != svc_.swapChainManager.imageCount())
    referenceColorBound_.assign(svc_.swapChainManager.imageCount(), false);
  if (referencePathActive() && subs_.referencePathTracePass) {
    subs_.frameResourceManager->setPostProcessColorSource(image,
        sceneLinear ? subs_.referencePathTracePass->meanView() : subs_.referencePathTracePass->displayView(),
        sceneLinear ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    referenceColorBound_[image] = true;
  } else if (referenceColorBound_[image]) {
    subs_.frameResourceManager->setPostProcessColorSource(image,
        subs_.frameResourceManager->frame(image)->sceneColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    referenceColorBound_[image] = false;
  }
}

void RendererFrontend::publishReferenceColorBinding(uint32_t image) {
  if (!referencePathActive() || !subs_.referencePathTracePass) return;
  const auto extent = svc_.swapChainManager.extent();
  const FrameImageBinding binding{.image = subs_.referencePathTracePass->displayImage(),
      .view = subs_.referencePathTracePass->displayView(), .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .extent = {extent.width, extent.height, 1},
      .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
  for (auto technique : {RenderTechniqueId::DeferredRaster, RenderTechniqueId::ForwardRaster, RenderTechniqueId::PathTracing})
    subs_.frameRuntimeResourceRegistry->bindImage(technique, "scene-color", image, binding);
}
} // namespace container::renderer
