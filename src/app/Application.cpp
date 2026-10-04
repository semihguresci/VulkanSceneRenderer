#include "Container/app/Application.h"

#include "Container/renderer/temporal/TemporalCapture.h"
#include <fstream>
#include <iomanip>
#include <sstream>

#include "Container/common/CommonGLFW.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/WindowManager.h"

#include "Container/renderer/core/RendererFrontend.h"
#include "Container/renderer/platform/VulkanContext.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/platform/WindowInputBridge.h"
#include "Container/renderer/resources/CommandBufferManager.h"

#include <algorithm>
#include <stdexcept>
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
    const float  dt  = static_cast<float>(now - lastFrameTimeSeconds_);
    lastFrameTimeSeconds_ = now;

    window_->pollEvents();
    renderer_->processInput(dt);
    renderer_->gfxCaptureTick(++captureTick, false);
    renderer_->drawFrame(framebufferResized_);
    if (renderer_->gfxCaptureComplete()) break;
  }
  vkDeviceWaitIdle(vulkanContext_->result().deviceWrapper->device());

  onMainLoop();
}

void Application::screenshotCaptureLoop() {
  std::optional<container::temporal::CaptureSequence> sequence;
  if (!config_.temporalCaptureSequencePath.empty()) {
    std::ifstream stream(config_.temporalCaptureSequencePath);
    if (!stream)
      throw std::runtime_error("Cannot open capture sequence");
    sequence.emplace(nlohmann::json::parse(stream));
  }
  bool minimized = false;
  const uint32_t captureFrame =
      sequence ? sequence->frameCount
               : std::max(config_.screenshotCaptureFrame,
               config_.screenshotWarmupFrames + 1u);
  const float fixedDt = config_.screenshotFixedTimestepSeconds > 0.0f
                            ? config_.screenshotFixedTimestepSeconds
                            : 1.0f / 60.0f;

  for (uint32_t frameNumber = 1; frameNumber <= captureFrame &&
                                     !window_->shouldClose();
       ++frameNumber) {
    window_->pollEvents();
    renderer_->processInput(fixedDt);
    bool skip = false;
    if (sequence) {
      const auto sample = sequence->sample(frameNumber);
      renderer_->applyTemporalCapture(sample);
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
    renderer_->gfxCaptureTick(frameNumber, skip);
    if (skip)
      continue;
    std::filesystem::path capturePath;
    if (!config_.screenshotCapturePath.empty() &&
        (sequence ? sequence->captures(frameNumber)
                  : frameNumber == captureFrame)) {
      capturePath = config_.screenshotCapturePath;
      if (sequence && frameNumber != captureFrame) {
        std::ostringstream suffix;
        suffix << ".frame-" << std::setw(4) << std::setfill('0') << frameNumber;
        capturePath = capturePath.parent_path() /
                      (capturePath.stem().string() + suffix.str() +
                       capturePath.extension().string());
      }
      renderer_->requestScreenshot(capturePath);
    }
    bool submitted = false;
    for (uint32_t retry = 0; retry < 4 && !submitted; ++retry)
      submitted = renderer_->drawFrame(framebufferResized_);
    if (!submitted)
      throw std::runtime_error(
          "Capture could not submit after swapchain recovery");
    if (!capturePath.empty()) {
      auto telemetryPath = capturePath;
      telemetryPath.replace_extension(".telemetry.json");
      renderer_->writeCaptureTelemetry(telemetryPath);
    }
    if (renderer_->gfxCaptureComplete()) break;
  }
  vkDeviceWaitIdle(vulkanContext_->result().deviceWrapper->device());

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
