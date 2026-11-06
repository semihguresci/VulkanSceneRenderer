#pragma once
#include "Container/geometry/OpenVdbTransportImage.h"
#include "Container/renderer/effects/OpenVdbTransportGpuReference.h"

namespace container::renderer {
[[nodiscard]] geometry::vdb::TransportImageResult
RenderTransportImageGpu(OpenVdbTransportGpuReference &, const geometry::vdb::TransportScene &,
                        const geometry::vdb::TransportPinholeCamera & = {},
                        const geometry::vdb::TransportImageOptions & = {},
                        const std::array<std::shared_ptr<const TransportGpuNativeSource>, 4> & = {},
                        uint32_t maxWorkPerPath = 4096);

// Dedicated compute-only Vulkan device/queue/memory. Never shares live viewer
// resources. Vulkan1.4 compute and bufferDeviceAddress are required by the
// existing VMA reference host; RTX/Float64/Int64 and a window are unnecessary.
// Calls are synchronous and single-threaded. Requested validation is strict.
class TransportImageGpuSession {
public:
  explicit TransportImageGpuSession(const std::filesystem::path &spirv, bool validation = false);
  ~TransportImageGpuSession();
  TransportImageGpuSession(const TransportImageGpuSession &) = delete;
  TransportImageGpuSession &operator=(const TransportImageGpuSession &) = delete;
  [[nodiscard]] geometry::vdb::TransportImageResult
  render(const geometry::vdb::TransportScene &, const geometry::vdb::TransportPinholeCamera & = {},
         const geometry::vdb::TransportImageOptions & = {},
         const std::array<std::shared_ptr<const TransportGpuNativeSource>, 4> & = {},
         uint32_t maxWorkPerPath = 4096);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace container::renderer
