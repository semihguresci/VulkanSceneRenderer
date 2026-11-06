#pragma once

#include "Container/utility/PipelineManager.h"

#include <array>
#include <utility>

namespace container::renderer {

// Holds detached cull variants, or the retired variants after exchange.
class PreparedGraphicsPipelines {
  gpu::PipelineManager& manager_;

public:
  explicit PreparedGraphicsPipelines(gpu::PipelineManager& manager) : manager_(manager) {}
  PreparedGraphicsPipelines(const PreparedGraphicsPipelines&) = delete;
  PreparedGraphicsPipelines& operator=(const PreparedGraphicsPipelines&) = delete;
  PreparedGraphicsPipelines(PreparedGraphicsPipelines&& other) noexcept
      : manager_(other.manager_), handles(std::exchange(other.handles, {})) {}
  ~PreparedGraphicsPipelines() {
    for (auto& pipeline : handles)
      manager_.destroyPipeline(pipeline);
  }

  std::array<VkPipeline, 3> handles{};
};

} // namespace container::renderer
