#pragma once

#include "Container/renderer/temporal/DlssUpscaler.h"

namespace container::renderer {

struct ReferenceDlssFrame {
  VkCommandBuffer commands{VK_NULL_HANDLE};
  DlssImage color;
  DlssImage depth;
  DlssImage motion;
  DlssImage diffuseAlbedo;
  DlssImage specularAlbedo;
  DlssImage normalRoughness;
  DlssImage specularHitDistance;
  DlssImage output;
  std::array<float, 2> jitterPixels{}; // Projected displacement, +X right and +Y down.
  std::array<float, 16> worldToView{};
  std::array<float, 16> viewToClip{}; // Unjittered projection. Both matrices are row-major.
  bool reset{};
  float frameMilliseconds{};
};

class ReferenceDlssReconstructor {
 public:
  ReferenceDlssReconstructor(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
      VkQueue queue, uint32_t queueFamily, std::filesystem::path runtimeDirectory);
  ~ReferenceDlssReconstructor();
  ReferenceDlssReconstructor(const ReferenceDlssReconstructor&) = delete;
  ReferenceDlssReconstructor& operator=(const ReferenceDlssReconstructor&) = delete;

  [[nodiscard]] static bool builtWithSdk() noexcept;
  [[nodiscard]] static DlssExtensionRequirements queryInstanceExtensions(const std::filesystem::path& runtimeDirectory);
  [[nodiscard]] static DlssExtensionRequirements queryDeviceExtensions(VkInstance, VkPhysicalDevice,
      const std::filesystem::path& runtimeDirectory);

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] std::string_view status() const noexcept { return status_; }
  [[nodiscard]] std::optional<uint64_t> allocatedBytes() const noexcept;

  // Native requests DLAA at the output extent. Other modes query the RR model.
  [[nodiscard]] std::optional<VkExtent2D> optimalRenderExtent(UpscaleMode mode, VkExtent2D output);
  // Retire previous work before changing dimensions or recovering a discarded
  // evaluation. Initialization submits a one-shot graphics command.
  [[nodiscard]] bool prepare(UpscaleMode mode, VkExtent2D input, VkExtent2D output);
  // Sampled inputs use SHADER_READ_ONLY_OPTIMAL. The storage output uses GENERAL.
  // Depth is positive linear view depth, motion is current-to-previous in input
  // pixels, and normalRoughness carries world normals and linear roughness in A.
  [[nodiscard]] DlssDispatchResult record(const ReferenceDlssFrame& frame);
  void commit() noexcept;
  void discard() noexcept;

 private:
  struct Backend;
  Backend& ensureBackend();
  std::unique_ptr<Backend> backend_;
  VkInstance instance_{};
  VkPhysicalDevice physicalDevice_{};
  VkDevice device_{};
  VkQueue queue_{};
  uint32_t queueFamily_{};
  std::filesystem::path runtimeDirectory_;
  UpscaleMode mode_{UpscaleMode::Native};
  VkExtent2D inputExtent_{}, outputExtent_{};
  std::string status_;
  bool recorded_{}, discarded_{};
};

} // namespace container::renderer
