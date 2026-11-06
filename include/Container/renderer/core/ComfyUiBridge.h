#pragma once

#include "Container/utility/TextureImageData.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace container::renderer {

enum class ComfyUiJobKind { Hdri, Restyle };
enum class ComfyUiJobPhase { Idle, Preparing, Capture, Queued, Running, Ready, Failed, Cancelled };

struct ComfyUiJobStatus {
  std::string id;
  ComfyUiJobPhase phase{ComfyUiJobPhase::Idle};
  std::string message;
  float progress{};

  [[nodiscard]] bool busy() const;
};

struct ComfyUiCaptureRequest {
  std::string id;
  std::filesystem::path path;
};

struct ComfyUiResult {
  std::string id;
  ComfyUiJobKind kind{};
  std::filesystem::path path;
  std::shared_ptr<const std::vector<std::byte>> encoded;
  container::material::TextureImageData preview;
};

// The worker owns all bridge filesystem I/O and preview decoding. The caller
// owns capture, GPU upload and environment publication on the render thread.
class ComfyUiBridge {
 public:
  explicit ComfyUiBridge(std::filesystem::path root);
  ~ComfyUiBridge();
  ComfyUiBridge(const ComfyUiBridge&) = delete;
  ComfyUiBridge& operator=(const ComfyUiBridge&) = delete;

  bool submit(ComfyUiJobKind kind, std::string prompt, uint64_t seed);
  void cancel();
  [[nodiscard]] ComfyUiJobStatus status() const;
  [[nodiscard]] std::optional<ComfyUiCaptureRequest> takeCaptureRequest();
  void completeCapture(const std::string& id, std::string error = {});
  [[nodiscard]] std::optional<ComfyUiResult> takeResult();

 private:
  struct Request {
    std::string id;
    ComfyUiJobKind kind{};
    std::string prompt;
    uint64_t seed{};
  };

  void run(std::stop_token stop);
  void process(std::stop_token stop, const Request& request);
  [[nodiscard]] bool isCurrentLocked(const std::string& id) const;
  void update(const std::string& id, ComfyUiJobPhase phase, std::string message, float progress);

  const std::filesystem::path root_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  ComfyUiJobStatus status_;
  std::optional<Request> request_;
  std::optional<ComfyUiCaptureRequest> capture_;
  std::optional<std::string> captureError_;
  std::optional<ComfyUiResult> result_;
  uint64_t serial_{};
  std::jthread worker_;
};

}  // namespace container::renderer
