#pragma once

#include "Container/renderer/temporal/UpscaleMode.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

struct DlssExtensionRequirements {
  std::vector<std::string> names;
  std::string status;
};

struct DlssImage {
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkExtent2D extent{};
};

struct DlssUpscalerFrame {
  VkCommandBuffer commands{VK_NULL_HANDLE};
  DlssImage color;
  DlssImage depth;
  DlssImage motion;
  DlssImage output;
  std::array<float, 2> jitterPixels{}; // Projected displacement, +X right and +Y down.
  bool reset{};
  float frameMilliseconds{};
};

enum class DlssDispatchResult {
  Recorded,
  Unavailable,
  InvalidInput,
  DiscardCommands,
};

class DlssUpscaler {
 public:
  DlssUpscaler(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
      VkQueue queue, uint32_t queueFamily, std::filesystem::path runtimeDirectory);
  ~DlssUpscaler();
  DlssUpscaler(const DlssUpscaler&) = delete;
  DlssUpscaler& operator=(const DlssUpscaler&) = delete;

  [[nodiscard]] static bool builtWithSdk() noexcept;
  // An empty status means discovery succeeded, including an empty requirement list.
  [[nodiscard]] static DlssExtensionRequirements queryInstanceExtensions(const std::filesystem::path& runtimeDirectory);
  [[nodiscard]] static DlssExtensionRequirements queryDeviceExtensions(VkInstance, VkPhysicalDevice,
      const std::filesystem::path& runtimeDirectory);

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] std::string_view status() const noexcept { return status_; }
  [[nodiscard]] VkExtent2D renderExtent() const noexcept { return renderExtent_; }
  [[nodiscard]] std::optional<uint64_t> allocatedBytes() const noexcept;

  // Retire prior work before changing the mode or extent or recovering an
  // abandoned evaluation. Initialization submits a one-shot graphics command.
  [[nodiscard]] bool prepare(UpscaleMode mode, VkExtent2D display);
  // Inputs are sampled images in SHADER_READ_ONLY_OPTIMAL. Output is a storage
  // image in GENERAL. Motion is current to previous in top-left UV coordinates.
  [[nodiscard]] DlssDispatchResult record(const DlssUpscalerFrame& frame);
  void commit() noexcept;
  void discard() noexcept;

 private:
  struct Backend;
  std::unique_ptr<Backend> backend_;
  VkInstance instance_{};
  VkPhysicalDevice physicalDevice_{};
  VkDevice device_{};
  VkQueue queue_{};
  uint32_t queueFamily_{};
  std::filesystem::path runtimeDirectory_;
  UpscaleMode mode_{UpscaleMode::Native};
  VkExtent2D displayExtent_{}, renderExtent_{};
  std::string status_;
  bool recorded_{}, discarded_{};
};

} // namespace container::renderer
