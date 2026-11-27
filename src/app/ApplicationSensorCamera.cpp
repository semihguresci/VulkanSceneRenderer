#include "Container/app/Application.h"

#include "Container/renderer/core/RendererFrontend.h"
#include "Container/robotics/SensorCamera.h"
#include "Container/utility/Platform.h"
#include "Container/utility/WindowManager.h"

#include <filesystem>
#include <stdexcept>

namespace container::app {

void Application::sensorCameraCaptureLoop() {
  auto directory = std::filesystem::absolute(config_.sensorDatasetPath).lexically_normal();
  if (!directory.has_filename())
    directory = directory.parent_path();
  std::filesystem::create_directories(directory.parent_path());
  if (!std::filesystem::create_directory(directory))
    throw std::runtime_error("Sensor camera output directory already exists: " +
                             container::util::pathToUtf8(directory));

  // Each frame is published atomically. The dataset manifest is the final
  // commit marker, so interrupted capture cannot look like a complete dataset.
  renderer_->settlePendingModelLoads();
  renderer_->beginSensorCameraCapture(directory);
  for (const auto& frame : config_.sensorCameraPlayback->frames) {
    window_->pollEvents();
    if (window_->shouldClose())
      throw std::runtime_error("Window closed before sensor camera playback was complete");
    renderer_->requestSensorCameraFrame(frame);
    bool submitted = false;
    for (uint32_t retry = 0; retry < 4 && !submitted; ++retry)
      submitted = renderer_->drawFrame(framebufferResized_);
    if (!submitted)
      throw std::runtime_error("Sensor camera could not submit after swapchain recovery");
  }
  renderer_->finishSensorCameraCapture();
  onMainLoop();
}

}  // namespace container::app
