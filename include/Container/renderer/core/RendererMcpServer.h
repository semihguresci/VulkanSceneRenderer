#pragma once

#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

struct RendererPassGpuTiming;

// A measurement advances only on previously unseen retired recording serials.
class RendererMcpMeasurement {
 public:
  using Clock = std::chrono::steady_clock;
  enum class Status { Running, Complete, Cancelled, TimedOut };

  RendererMcpMeasurement(uint64_t previousSerial, uint32_t warmup, uint32_t samples,
                         Clock::time_point deadline);
  void observe(uint64_t serial, std::optional<double> milliseconds, Clock::time_point now);
  void cancel(Clock::time_point now);
  [[nodiscard]] Status status(Clock::time_point now) const;
  [[nodiscard]] nlohmann::json report(Clock::time_point now) const;

 private:
  uint64_t lastSerial_, firstSampleSerial_{}, lastSampleSerial_{};
  uint32_t warmup_, target_;
  Clock::time_point deadline_;
  bool cancelled_{};
  std::vector<double> samples_;
};

class RendererMcpServer {
 public:
  // The handler runs synchronously on the renderer owner thread. Network and
  // request validation run on the server worker before a command is queued.
  using OwnerHandler = std::function<nlohmann::json(std::string_view, const nlohmann::json&)>;

  static std::unique_ptr<RendererMcpServer> startFromEnvironment();
  RendererMcpServer(uint16_t port, std::string token, std::filesystem::path root = {});
  ~RendererMcpServer();
  RendererMcpServer(const RendererMcpServer&) = delete;
  RendererMcpServer& operator=(const RendererMcpServer&) = delete;

  [[nodiscard]] uint16_t port() const;
  void pump(const OwnerHandler& handler);
  void observeGpuTimings(uint64_t serial, std::string_view source,
                        std::span<const RendererPassGpuTiming> timings);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace container::renderer
