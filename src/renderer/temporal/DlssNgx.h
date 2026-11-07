#pragma once

#include "Container/renderer/temporal/DlssUpscaler.h"

namespace container::renderer::ngx {

inline bool sameExtent(VkExtent2D a, VkExtent2D b) noexcept {
  return a.width == b.width && a.height == b.height;
}

} // namespace container::renderer::ngx

#if CONTAINER_HAS_DLSS_SDK
#include <nvsdk_ngx_helpers_vk.h>
#include "DlssRrClearSynchronization.h"

#include <mutex>

namespace container::renderer::ngx {

enum class Feature { SuperResolution, RayReconstruction };

// NGX owns process-wide Vulkan state. Both feature wrappers use this mutex
// for vendor calls and share the initialized device through Session.
extern std::mutex mutex;

[[nodiscard]] bool nvidiaAdapter(VkPhysicalDevice physicalDevice);
[[nodiscard]] NVSDK_NGX_PerfQuality_Value quality(UpscaleMode mode);
[[nodiscard]] DlssExtensionRequirements queryInstanceExtensions(Feature, const std::filesystem::path&);
[[nodiscard]] DlssExtensionRequirements queryDeviceExtensions(Feature, VkInstance, VkPhysicalDevice,
    const std::filesystem::path&);
[[nodiscard]] bool imageMatches(const DlssImage&, VkFormat, VkExtent2D) noexcept;
[[nodiscard]] NVSDK_NGX_Resource_VK image(const DlssImage&, bool storage = false);
void check(NVSDK_NGX_Result, std::string_view operation);

using ScopedRrClearSynchronization = rr_clear_sync::Scope;

class Session {
 public:
  Session() = default;
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void initialize(Feature, VkInstance, VkPhysicalDevice, VkDevice, const std::filesystem::path&);
  [[nodiscard]] VkDevice device() const noexcept { return device_; }
  [[nodiscard]] NVSDK_NGX_Parameter* parameters() const noexcept { return parameters_; }

 private:
  VkDevice device_{};
  NVSDK_NGX_Parameter* parameters_{};
};

struct FeatureCommands {
  VkDevice device;
  VkCommandPool pool{};
  VkFence fence{};

  ~FeatureCommands();
  VkCommandBuffer begin(uint32_t queueFamily);
  void submit(VkQueue, VkCommandBuffer);
};

} // namespace container::renderer::ngx
#endif
