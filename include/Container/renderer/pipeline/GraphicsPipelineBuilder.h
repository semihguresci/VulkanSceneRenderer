#pragma once

#include "Container/renderer/pipeline/PipelineTypes.h"

#include <filesystem>
#include <memory>
#include <string>

namespace container::gpu {
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

// Builds all graphics pipelines from shader SPIR-V files on disk.
// Stateless: call build() once per (re)creation of pipelines.
class GraphicsPipelineBuilder {
 public:
  GraphicsPipelineBuilder(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::PipelineManager&            pipelineManager);

  ~GraphicsPipelineBuilder() = default;
  GraphicsPipelineBuilder(const GraphicsPipelineBuilder&) = delete;
  GraphicsPipelineBuilder& operator=(const GraphicsPipelineBuilder&) = delete;

  // Creates all pipeline layouts and graphics pipelines.
  // shaderDir: directory containing the spv_shaders/ sub-folder.
  PipelineBuildResult build(
      const std::filesystem::path&         shaderDir,
      const PipelineDescriptorLayouts&     descriptorLayouts,
      const PipelineRenderPasses&          renderPasses,
      VkSampleCountFlagBits                sceneSampleCount) const;
  void destroy(PipelineBuildResult& result) const noexcept;

 private:
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::PipelineManager&            pipelineManager_;
};

}  // namespace container::renderer
