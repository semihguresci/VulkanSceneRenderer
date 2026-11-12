#include "Container/app/Application.h"

#include "Container/renderer/temporal/TemporalCapture.h"
#include <fstream>
#include <iomanip>
#include <sstream>

#include "Container/common/CommonGLFW.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/WindowManager.h"

#include "Container/renderer/core/RendererFrontend.h"
#include "Container/renderer/platform/VulkanContext.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/platform/WindowInputBridge.h"
#include "Container/renderer/resources/CommandBufferManager.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace container::app {

Application::Application(AppConfig config)
    : config_(std::move(config)) {
  if (config_.gfxrecon.enabled())
    config_.gfxrecon = container::capture::loadSession(config_.gfxrecon.sessionPath);
}

Application::~Application() {
  if (vulkanContext_) {
    try { cleanup(); } catch (...) {}
  }
}

void Application::run() {
  initWindow();
  initVulkan();
  mainLoop();
  cleanup();
}

// ---------------------------------------------------------------------------
// Lifecycle stages
// ---------------------------------------------------------------------------

void Application::initWindow() {
  windowManager_ = std::make_unique<container::window::WindowManager>();
  container::window::WindowManager::WindowConfig windowConfig{};
  windowConfig.width = config_.windowWidth;
  windowConfig.height = config_.windowHeight;
  windowConfig.title = "Vulkan";
  windowConfig.visible = config_.windowVisible;
  windowConfig.focused = config_.windowVisible;
  windowConfig.focusOnShow = config_.windowVisible;
  window_ = windowManager_->createWindow(windowConfig);

  GLFWwindow* nativeWindow = window_->getNativeWindow();
  inputManager_ = std::make_unique<container::window::InputManager>();
  inputManager_->setWindow(nativeWindow);
  windowInputBridge_ = std::make_unique<container::renderer::WindowInputBridge>(
      nativeWindow, *inputManager_, framebufferResized_);

  onInitWindow();
}

void Application::initVulkan() {
  const container::renderer::VulkanContextInitializer ctxInit{config_};
  vulkanContext_ = std::make_unique<container::renderer::VulkanContext>(
      ctxInit.initialize(
          windowManager_->getRequiredInstanceExtensions(),
          window_->getNativeWindow()),
      config_.enableValidationLayers);

  auto& ctx = vulkanContext_->result();

  pipelineManager_ = std::make_unique<container::gpu::PipelineManager>(
      ctx.deviceWrapper->device());

  swapChainManager_ = std::make_unique<container::gpu::SwapChainManager>(
      window_->getNativeWindow(),
      ctx.deviceWrapper->physicalDevice(),
      ctx.deviceWrapper->device(),
      ctx.surface);
  swapChainManager_->initialize();

  commandBufferManager_ = std::make_unique<container::renderer::CommandBufferManager>(
      ctx.deviceWrapper,
      ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value());

  allocationManager_ = std::make_unique<container::gpu::AllocationManager>();
  allocationManager_->initialize(
      ctx.instance,
      ctx.deviceWrapper->physicalDevice(),
      ctx.deviceWrapper->device(),
      ctx.deviceWrapper->graphicsQueue(),
      commandBufferManager_->pool(), config_);
  allocationManager_->setTextureCompressionFeatures(ctx.deviceWrapper->enabledFeatures());

  renderer_ = std::make_unique<container::renderer::RendererFrontend>(
      container::renderer::RendererFrontendCreateInfo{
          .ctx                  = &ctx,
          .pipelineManager      = pipelineManager_.get(),
          .allocationManager    = allocationManager_.get(),
          .swapChainManager     = swapChainManager_.get(),
          .commandBufferManager = commandBufferManager_.get(),
          .config               = &config_,
          .nativeWindow         = window_->getNativeWindow(),
          .inputManager         = inputManager_.get()});
  renderer_->initialize();
  renderer_->startGfxCapture();

  onInitVulkan();
}

void Application::mainLoop() {
  if (!config_.screenshotCapturePath.empty() ||
      !config_.temporalCaptureSequencePath.empty()) {
    screenshotCaptureLoop();
    return;
  }

  lastFrameTimeSeconds_ = windowManager_->getTime();
  uint64_t captureTick = 0;
  while (!window_->shouldClose()) {
    const double now = windowManager_->getTime();
    // Resource commits may present their own loading frames. Avoid applying
    // their entire elapsed duration to camera motion on the next normal tick.
    const float dt = std::min(static_cast<float>(now - lastFrameTimeSeconds_), 0.1f);
    lastFrameTimeSeconds_ = now;

    window_->pollEvents();
    renderer_->processInput(dt);
    renderer_->gfxCaptureTick(++captureTick, false);
    renderer_->drawFrame(framebufferResized_);
    if (renderer_->gfxCaptureComplete()) break;
  }
  vkDeviceWaitIdle(vulkanContext_->result().deviceWrapper->device());

  if (!config_.sceneExportPath.empty())
    renderer_->saveSceneModel(std::filesystem::u8path(config_.sceneExportPath), config_.sceneExportKeepLinks);

  onMainLoop();
}

void Application::screenshotCaptureLoop() {
  std::optional<container::temporal::CaptureSequence> sequence;
  if (!config_.temporalCaptureSequencePath.empty()) {
    std::ifstream stream(config_.temporalCaptureSequencePath);
    if (!stream)
      throw std::runtime_error("Cannot open capture sequence");
    sequence.emplace(nlohmann::json::parse(stream));
    if (sequence->hasTelemetryFrames() && config_.screenshotCapturePath.empty())
      throw std::invalid_argument("Capture telemetry frames require a screenshot output path");
  }
  bool minimized = false;
  const uint32_t captureFrame =
      sequence ? sequence->frameCount
               : std::max(config_.screenshotCaptureFrame,
               config_.screenshotWarmupFrames + 1u);
  const auto& sampleSeries = config_.pathSampleSeries;
  const bool captureSampleSeries = !sampleSeries.empty();
  auto checkpoint = sampleSeries.begin();
  uint32_t seriesSamples = 0;
  const float fixedDt = config_.screenshotFixedTimestepSeconds > 0.0f
                            ? config_.screenshotFixedTimestepSeconds
                            : 1.0f / 60.0f;
  const bool scriptedViewport = sequence && (sequence->hasViewportPointer() || sequence->hasNodeGizmo());

  for (uint32_t frameNumber = 1; !window_->shouldClose() &&
       (captureSampleSeries ? checkpoint != sampleSeries.end() : frameNumber <= captureFrame);
       ++frameNumber) {
    window_->pollEvents();
    renderer_->processInput(captureSampleSeries ? 0.0f : fixedDt, !scriptedViewport);
    bool skip = false;
    bool waitForComfyUi = false;
    std::optional<glm::uvec2> depthProbe;
    std::optional<container::temporal::CaptureViewportPointer> viewportPointer;
    if (sequence) {
      const auto sample = sequence->sample(frameNumber);
      renderer_->applyTemporalCapture(sample);
      waitForComfyUi = sample.event.comfyUi && sample.event.comfyUi->wait;
      depthProbe = sample.event.depthProbe;
      viewportPointer = sample.event.viewportPointer;
      if (sample.event.minimized) {
        minimized = *sample.event.minimized;
        if (minimized)
          window_->iconify();
        else
          window_->restore();
      }
      if (sample.event.resize) {
        window_->setSize(static_cast<int>(sample.event.resize->x),
                         static_cast<int>(sample.event.resize->y));
        window_->pollEvents();
        renderer_->handleResize();
        framebufferResized_ = false;
      }
      skip = sample.event.skip || minimized;
    }
    if (skip && viewportPointer)
      throw std::runtime_error("Cannot dispatch a viewport pointer on a skipped capture frame");
    if (waitForComfyUi) {
      using namespace std::chrono_literals;
      if (skip) throw std::runtime_error("Cannot wait for ComfyUI on a skipped capture frame");
      const auto deadline = std::chrono::steady_clock::now() + 30s;
      while (renderer_->comfyUiCapturePending()) {
        window_->pollEvents();
        if (window_->shouldClose() || std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("Capture timed out waiting for ComfyUI");
        renderer_->drawFrame(framebufferResized_);
        std::this_thread::sleep_for(10ms);
      }
    }
    // Fixed timesteps can render much faster than wall time. Settle the
    // requested volume frame before advancing a deterministic capture tick.
    if (!skip && config_.vdbSequenceAsync)
      renderer_->settlePendingModelLoads();
    renderer_->gfxCaptureTick(frameNumber, skip);
    if (skip)
      continue;
    std::filesystem::path capturePath;
    std::optional<uint32_t> sampleTarget;
    if (checkpoint != sampleSeries.end()) {
      seriesSamples += std::min(config_.pathSamplesPerFrame, *checkpoint - seriesSamples);
      if (seriesSamples == *checkpoint) {
        sampleTarget = *checkpoint;
        ++checkpoint;
      }
    }
    const bool captureThisFrame = captureSampleSeries ? sampleTarget.has_value()
        : sequence ? sequence->captures(frameNumber) : frameNumber == captureFrame;
    const bool telemetryThisFrame =
        sequence && sequence->capturesTelemetry(frameNumber);
    if ((captureThisFrame || telemetryThisFrame) &&
        !config_.screenshotCapturePath.empty()) {
      capturePath = container::util::pathFromUtf8(config_.screenshotCapturePath);
      if (sampleTarget || (sequence && frameNumber != captureFrame)) {
        std::ostringstream suffix;
        if (sampleTarget)
          suffix << ".samples-" << std::setw(8) << std::setfill('0') << *sampleTarget;
        else
          suffix << ".frame-" << std::setw(4) << std::setfill('0') << frameNumber;
        auto filename = capturePath.stem();
        filename += suffix.str();
        filename += capturePath.extension();
        capturePath.replace_filename(filename);
      }
      if (captureThisFrame)
        renderer_->requestScreenshot(capturePath, config_.screenshotViewportOnly,
                                     sampleTarget);
    }
    bool submitted = false;
    for (uint32_t retry = 0; retry < 4 && !submitted; ++retry)
      submitted = renderer_->drawFrame(framebufferResized_);
    if (!submitted)
      throw std::runtime_error(
          "Capture could not submit after swapchain recovery");
    if (viewportPointer)
      renderer_->applyCaptureViewportPointer(*viewportPointer);
    if (!capturePath.empty()) {
      auto telemetryPath = capturePath;
      telemetryPath.replace_extension(".telemetry.json");
      renderer_->writeCaptureTelemetry(telemetryPath, depthProbe);
    }
    if (renderer_->gfxCaptureComplete()) break;
  }
  vkDeviceWaitIdle(vulkanContext_->result().deviceWrapper->device());
  if (checkpoint != sampleSeries.end())
    throw std::runtime_error("Reference sample series ended before all checkpoints were exported");

  if (!config_.sceneExportPath.empty())
    renderer_->saveSceneModel(std::filesystem::u8path(config_.sceneExportPath), config_.sceneExportKeepLinks);

  onMainLoop();
}

void Application::cleanup() {
  onCleanup();

  renderer_.reset();
  allocationManager_.reset();
  commandBufferManager_.reset();
  pipelineManager_.reset();
  swapChainManager_.reset();

  if (vulkanContext_) {
    vulkanContext_->result().deviceWrapper.reset();
    vulkanContext_.reset();
  }

  windowInputBridge_.reset();
  inputManager_.reset();
  window_.reset();
  windowManager_.reset();
}

// ---------------------------------------------------------------------------
// Virtual override points — default implementations are no-ops.
// ---------------------------------------------------------------------------

void Application::onInitWindow() {}
void Application::onInitVulkan() {}
void Application::onMainLoop() {}
void Application::onCleanup() {}

}  // namespace container::app
