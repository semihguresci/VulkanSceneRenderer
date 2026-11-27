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
  if (!config_.sensorBenchmarkPath.empty()) {
    auto& benchmark = config_.sensorBenchmarkPath;
    benchmark = std::filesystem::absolute(benchmark).lexically_normal();
    if (!benchmark.has_filename() || benchmark == directory ||
        std::filesystem::exists(std::filesystem::symlink_status(benchmark)))
      throw std::invalid_argument("--sensor-benchmark requires a new file: " +
                                  container::util::pathToUtf8(benchmark));
  }
  std::filesystem::create_directories(directory.parent_path());
  if (!std::filesystem::create_directory(directory))
    throw std::runtime_error("Sensor camera output directory already exists: " +
                             container::util::pathToUtf8(directory));
  if (!config_.sensorBenchmarkPath.empty())
    std::filesystem::create_directories(config_.sensorBenchmarkPath.parent_path());

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
