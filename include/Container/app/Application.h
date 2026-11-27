#pragma once

#include "Container/app/AppConfig.h"
#include "Container/common/CommonVulkan.h"

#include <memory>
#include <string>

// Forward declarations — keeps the header lightweight.
struct GLFWwindow;

namespace container::gpu {
class SwapChainManager;
class AllocationManager;
class PipelineManager;
}

namespace container::window {
class WindowManager;
class Window;
}  // namespace container::window

namespace container::window {
class InputManager;
}

namespace container::renderer {
class VulkanContext;
class CommandBufferManager;
class RendererFrontend;
class WindowInputBridge;
}  // namespace container::renderer

namespace container::app {

// Reusable Vulkan application shell.
//
// Owns the complete lifecycle:  window → Vulkan context → renderer → main loop.
// Construct with an AppConfig, then call run().
//
// Designed to be subclassed or composed into higher-level frameworks.
class Application {
 public:
  explicit Application(AppConfig config);
  virtual ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;
  Application(Application&&) = delete;
  Application& operator=(Application&&) = delete;

  // Blocking: initialises, enters the main loop, and cleans up on return.
  void run();

 protected:
  // Override points for derived applications.
  virtual void onInitWindow();
  virtual void onInitVulkan();
  virtual void onMainLoop();
  virtual void onCleanup();

  AppConfig config_;

 private:
  void initWindow();
  void initVulkan();
  void mainLoop();
  void screenshotCaptureLoop();
  void sensorCameraCaptureLoop();
  void cleanup();

  std::unique_ptr<container::window::WindowManager> windowManager_;
  std::unique_ptr<container::window::Window>        window_;
  bool framebufferResized_{false};

  std::unique_ptr<container::window::InputManager> inputManager_;
  std::unique_ptr<container::renderer::WindowInputBridge>  windowInputBridge_;

  std::unique_ptr<container::renderer::VulkanContext>             vulkanContext_;
  std::unique_ptr<container::gpu::PipelineManager>  pipelineManager_;
  std::unique_ptr<container::gpu::SwapChainManager>           swapChainManager_;
  std::unique_ptr<container::renderer::CommandBufferManager>      commandBufferManager_;
  std::unique_ptr<container::gpu::AllocationManager>  allocationManager_;

  std::unique_ptr<container::renderer::RendererFrontend> renderer_;
  double lastFrameTimeSeconds_{0.0};
};

}  // namespace container::app
