#include <Container/utility/Logger.h>
#include <Container/utility/VkResultToString.h>

namespace container::log {

ContainerLogger& ContainerLogger::instance() {
  static ContainerLogger instance;
  return instance;
}

ContainerLogger::ContainerLogger() {
  // Create console sink
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_level(spdlog::level::trace);

  // Create loggers
  renderer_logger_ = std::make_shared<spdlog::logger>("Renderer", console_sink);
  vulkan_logger_ = std::make_shared<spdlog::logger>("Vulkan", console_sink);

  spdlog::register_logger(renderer_logger_);
  spdlog::register_logger(vulkan_logger_);

  renderer_logger_->set_level(spdlog::level::trace);
  vulkan_logger_->set_level(spdlog::level::trace);
}

ContainerLogger::~ContainerLogger() { spdlog::drop_all(); }

SpdLogger& ContainerLogger::renderer() { return renderer_logger_; }
SpdLogger& ContainerLogger::vulkan() { return vulkan_logger_; }

void ContainerLogger::log_vk_result(VkResult result, std::string_view operation,
                                    const std::source_location& location) {
  if (result != VK_SUCCESS) {
    vulkan_logger_->error("Vulkan error in {} ({}:{}): {} ({})", operation,
                          location.file_name(), location.line(),
                          container::gpu::vkResultToString(result),
                          static_cast<int>(result));
  }
}

}  // namespace container::log
