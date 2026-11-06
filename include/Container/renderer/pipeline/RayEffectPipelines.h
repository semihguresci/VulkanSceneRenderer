#pragma once

#include "Container/utility/VulkanDevice.h"

#include <filesystem>

namespace container::renderer {

// Detached pipelines use the owner's existing layout. Install only after the
// graphics and asynchronous compute queues have retired their current work.
struct RayEffectPipelines {
  vk::raii::Pipeline trace{nullptr};
  vk::raii::Pipeline auxiliary{nullptr};
};

[[nodiscard]] vk::raii::Pipeline loadRayEffectComputePipeline(
    const vk::raii::Device& device, const std::filesystem::path& shader,
    vk::PipelineLayout layout);

} // namespace container::renderer
