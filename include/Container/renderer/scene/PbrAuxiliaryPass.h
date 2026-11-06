#pragma once

#include <Container/common/CommonVulkan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

// Replays opaque G-buffer coverage into two scene-linear material attachments.
// The caller records every opaque Scene draw followed by every opaque BIM draw,
// in their original G-buffer order, even for materials without either feature.
// This makes the last equal-depth surface own both the G-buffer and auxiliary
// values. Transparent rendering uses its own forward material evaluation.
class PbrAuxiliaryPass {
 public:
  PbrAuxiliaryPass(std::shared_ptr<gpu::VulkanDevice> device,
                   gpu::AllocationManager& allocator,
                   gpu::PipelineManager& pipelines);
  ~PbrAuxiliaryPass();
  PbrAuxiliaryPass(const PbrAuxiliaryPass&) = delete;
  PbrAuxiliaryPass& operator=(const PbrAuxiliaryPass&) = delete;

  // shaderDir contains spv_shaders/extended_material_aux.{vert,frag}.spv.
  // Each depth view must have depthFormat, the requested sample count, extent,
  // and one layer. With MSAA, pass the original multisampled G-buffer depth,
  // rather than its single-sample resolve. Depth enters and leaves this pass
  // in DEPTH_STENCIL_ATTACHMENT_OPTIMAL and is read-only inside the scope.
  // Caller retires all frame-slot readers before recreation/destruction, and
  // keeps allocator, pipelines, scene layout, and depth views alive meanwhile.
  void createResources(const std::filesystem::path& shaderDir,
                       VkDescriptorSetLayout sceneDescriptorLayout,
                       VkFormat depthFormat,
                       VkSampleCountFlagBits msaa,
                       std::span<const VkImageView> depthViews,
                       VkExtent2D extent);

  // Caller waits for the frame-slot fence, begins outside any rendering scope,
  // sets the scene negative-height viewport/scissor, then records opaque draws
  // with these pipelines/layout and the original scene descriptor set.
  // begin clears both outputs to zero; end publishes SHADER_READ_ONLY_OPTIMAL.
  void begin(VkCommandBuffer command, uint32_t frameIndex);
  void end(VkCommandBuffer command);

  [[nodiscard]] VkPipelineLayout pipelineLayout() const;
  [[nodiscard]] VkPipeline backCullPipeline() const;
  [[nodiscard]] VkPipeline frontCullPipeline() const;
  [[nodiscard]] VkPipeline noCullPipeline() const;
  // RGBA16F: world tangent.xyz + sampled anisotropy strength.
  [[nodiscard]] VkImageView anisotropyView(uint32_t frameIndex) const;
  // RGBA16F: scene-linear color.rgb * sampled factor, plus sampled factor.a.
  [[nodiscard]] VkImageView diffuseTransmissionView(uint32_t frameIndex) const;
  [[nodiscard]] bool ready() const;
  [[nodiscard]] size_t frameCount() const;
  [[nodiscard]] VkExtent2D extent() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace container::renderer
