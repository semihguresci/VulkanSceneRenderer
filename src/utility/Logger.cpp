#include <Container/utility/Logger.h>
#include <Container/utility/RendererMcpLogs.h>
#include <Container/utility/VkResultToString.h>

#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace container::log {
namespace {

class McpLogSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  McpLogSink() { set_level(spdlog::level::off); }

  void enable(bool enabled) {
    const std::lock_guard lock(mutex_);
    enabled_ = enabled;
    if (!enabled_) size_ = next_ = 0;
    set_level(enabled ? spdlog::level::info : spdlog::level::off);
  }

  std::vector<RendererMcpLogEntry> recent(size_t limit) {
    const std::lock_guard lock(mutex_);
    const auto count = std::min({limit, size_, size_t{32}});
    std::vector<RendererMcpLogEntry> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const auto index = (next_ + entries_.size() - count + i) % entries_.size();
      result.push_back(entries_[index]);
    }
    return result;
  }

 private:
  void sink_it_(const spdlog::details::log_msg& message) override {
    if (!enabled_) return;
    const std::string_view source(message.logger_name.data(), message.logger_name.size());
    const std::string_view payload(message.payload.data(), message.payload.size());
    const auto level = spdlog::level::to_string_view(message.level);
    std::string_view event = message.level >= spdlog::level::err ? "Application error" :
        message.level >= spdlog::level::warn ? "Application warning" : "Application event";
    if (payload.starts_with("Failed to load model")) event = "Model load failed";
    else if (payload.starts_with("Loaded model")) event = "Model loaded";
    else if (payload.starts_with("Active render technique")) event = "Render technique changed";
    else if (payload.starts_with("Vulkan error")) event = "Vulkan operation failed";
    entries_[next_] = {source == "Vulkan" ? "Vulkan" : "Renderer",
        {level.data(), level.size()}, event,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            message.time.time_since_epoch()).count()};
    next_ = (next_ + 1) % entries_.size();
    size_ = std::min(size_ + 1, entries_.size());
  }
  void flush_() override {}

  std::array<RendererMcpLogEntry, 64> entries_{};
  size_t size_{}, next_{};
  bool enabled_{false};
};

std::shared_ptr<McpLogSink>& mcpLogSink() {
  static std::shared_ptr<McpLogSink> sink;
  return sink;
}

} // namespace

ContainerLogger& ContainerLogger::instance() {
  static ContainerLogger instance;
  return instance;
}

ContainerLogger::ContainerLogger() {
  // Create console sink
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_level(spdlog::level::trace);

  std::vector<SpdSink> sinks{console_sink};
  const char* port = std::getenv("CONTAINER_MCP_PORT");
  const char* token = std::getenv("CONTAINER_MCP_TOKEN");
  if (port && *port && token && *token) {
    mcpLogSink() = std::make_shared<McpLogSink>();
    sinks.push_back(mcpLogSink());
  }
  renderer_logger_ = std::make_shared<spdlog::logger>("Renderer", sinks.begin(), sinks.end());
  vulkan_logger_ = std::make_shared<spdlog::logger>("Vulkan", sinks.begin(), sinks.end());

  spdlog::register_logger(renderer_logger_);
  spdlog::register_logger(vulkan_logger_);

  renderer_logger_->set_level(spdlog::level::trace);
  vulkan_logger_->set_level(spdlog::level::trace);
}

ContainerLogger::~ContainerLogger() { spdlog::drop_all(); }

SpdLogger& ContainerLogger::renderer() { return renderer_logger_; }
SpdLogger& ContainerLogger::vulkan() { return vulkan_logger_; }

void setRendererMcpLogCapture(bool enabled) {
  (void)ContainerLogger::instance();
  if (const auto& sink = mcpLogSink()) sink->enable(enabled);
}

std::vector<RendererMcpLogEntry> recentRendererMcpLogs(size_t limit) {
  (void)ContainerLogger::instance();
  const auto& sink = mcpLogSink();
  return sink ? sink->recent(limit) : std::vector<RendererMcpLogEntry>{};
}

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
