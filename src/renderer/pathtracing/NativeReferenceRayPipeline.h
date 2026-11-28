#pragma once

#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <filesystem>
#include <span>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace container::gpu {
class VulkanDevice;
}

namespace container::renderer {

class NativeReferenceRayPipeline {
 public:
  using ShaderCode = std::array<std::vector<char>, 4>;
  struct Compilation {
    vk::raii::Pipeline pipeline{nullptr};
    ShaderCode shaders;
  };

  NativeReferenceRayPipeline(const gpu::VulkanDevice& device, gpu::VulkanMemoryManager& memory);
  ~NativeReferenceRayPipeline();

  NativeReferenceRayPipeline(const NativeReferenceRayPipeline&) = delete;
  NativeReferenceRayPipeline& operator=(const NativeReferenceRayPipeline&) = delete;

  [[nodiscard]] static ShaderCode loadShaders(const std::filesystem::path& shaders);
  [[nodiscard]] static Compilation compile(
      const gpu::VulkanDevice& device, VkPipelineLayout layout, ShaderCode shaders);
  void adopt(Compilation compilation);
  [[nodiscard]] bool matchesShaders(const ShaderCode& shaders) const { return shaders_ == shaders; }
  void record(VkCommandBuffer command, VkPipelineLayout layout,
              std::span<const VkDescriptorSet> sets, VkExtent2D extent) const;
  [[nodiscard]] VkPipeline pipeline() const { return static_cast<VkPipeline>(*pipeline_); }
  [[nodiscard]] bool supportsExtent(VkExtent2D extent) const;

 private:
  VkDevice device_;
  gpu::VulkanMemoryManager& memory_;
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties_{};
  std::array<uint64_t, 2> maxDispatchExtent_{};
  PFN_vkCmdTraceRaysKHR traceRays_{};
  vk::raii::Pipeline pipeline_{nullptr};
  ShaderCode shaders_;
  gpu::AllocatedBuffer bindingTable_{};
  std::array<VkStridedDeviceAddressRegionKHR, 3> regions_{};
};

} // namespace container::renderer
