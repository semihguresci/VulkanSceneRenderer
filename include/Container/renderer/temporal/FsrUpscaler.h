#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace container::renderer {

struct FsrUpscalerSettings {
  VkExtent2D renderExtent{};
  VkExtent2D displayExtent{};
  bool infiniteFar{};
};

struct FsrImage {
  VkImage image{VK_NULL_HANDLE};
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkExtent2D extent{};
};

struct FsrUpscalerFrame {
  VkCommandBuffer commands{VK_NULL_HANDLE};
  FsrImage color;
  FsrImage depth;
  FsrImage motion;
  FsrImage reactive;
  FsrImage transparency;
  FsrImage output;
  std::array<float, 2> jitterPixels{};  // Projected displacement, +X right and +Y down.
  float cameraNear{};
  float cameraFar{};
  float verticalFovRadians{};
  float frameMilliseconds{};
  bool reset{};
  bool debugView{};
};

enum class FsrDispatchResult {
  Recorded,
  Unavailable,
  InvalidInput,
  DiscardCommands,
};

class FsrUpscaler {
public:
  // The device must outlive this object. Enable the pinned backend's device
  // features before passing the handles, as documented in fsr-upscaling.md.
  FsrUpscaler(VkPhysicalDevice physicalDevice, VkDevice device,
              std::filesystem::path runtimeDirectory);
  ~FsrUpscaler();
  FsrUpscaler(const FsrUpscaler&) = delete;
  FsrUpscaler& operator=(const FsrUpscaler&) = delete;

  [[nodiscard]] static bool builtWithSdk() noexcept;
  [[nodiscard]] bool available() const noexcept;
  // Query after prepare: the pinned analytical shaders require RGBA32F output;
  // automatic ML providers and the inactive/default profile use RGBA16F.
  [[nodiscard]] VkFormat outputFormat() const noexcept;
  [[nodiscard]] std::string_view status() const noexcept { return status_; }

  // Retire queued work before changing settings or recovering a discarded
  // dispatch. Destroy the context before destroying previously imported images.
  [[nodiscard]] bool prepare(const FsrUpscalerSettings& settings);
  [[nodiscard]] std::optional<std::array<float, 2>> nextJitterPixels() const;

  // Supply single-layer, single-mip, single-sample images. Inputs are sampled
  // images in SHADER_READ_ONLY_OPTIMAL. Output is a storage image in GENERAL.
  // The caller owns producer/consumer barriers and uses one graphics queue.
  [[nodiscard]] FsrDispatchResult record(const FsrUpscalerFrame& frame);
  void commit() noexcept;   // After successful queue submission.
  void discard() noexcept;  // A recorded dispatch was abandoned.

private:
  struct Backend;
  std::unique_ptr<Backend> backend_;
  VkPhysicalDevice physicalDevice_{};
  VkDevice device_{};
  std::filesystem::path runtimeDirectory_;
  FsrUpscalerSettings settings_{};
  std::string status_;
  uint64_t submittedFrames_{};
  bool recorded_{};
  bool discarded_{};
};

} // namespace container::renderer
