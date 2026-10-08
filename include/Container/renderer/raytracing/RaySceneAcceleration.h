#pragma once

#include "Container/renderer/raytracing/RayScene.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <memory>

namespace container::renderer {

struct RaySceneBuildStats {
  uint32_t blasBuilt{0};
  uint32_t blasReused{0};
  uint32_t instanceCount{0};
  VkDeviceSize accelerationBytes{0};
  VkDeviceSize inputBytes{0};
  VkDeviceSize scratchBytes{0};
  VkDeviceSize accelerationAllocatedBytes{0}, inputAllocatedBytes{0},
      scratchAllocatedBytes{0};
};

// Immutable generations permit an old frame to keep tracing while a new TLAS
// is built. Keep each generation alive until all its GPU submissions retire.
// The VulkanDevice and VulkanMemoryManager must outlive every generation.
class RaySceneGeneration {
public:
  ~RaySceneGeneration();
  RaySceneGeneration(const RaySceneGeneration &) = delete;
  RaySceneGeneration &operator=(const RaySceneGeneration &) = delete;
  [[nodiscard]] VkAccelerationStructureKHR tlas() const;
  [[nodiscard]] const RaySceneBuildStats &stats() const;
  // Shared BLAS allocations are counted once across all retained generations.
  [[nodiscard]] static VkDeviceSize retainedAllocatedBytes(
      std::span<const std::shared_ptr<const RaySceneGeneration>> generations);

private:
  friend class RaySceneAcceleration;
  struct Impl;
  RaySceneGeneration();
  std::unique_ptr<Impl> impl_;
};

class RaySceneAcceleration {
public:
  RaySceneAcceleration(const container::gpu::VulkanDevice &device,
                       container::gpu::VulkanMemoryManager &memory);
  [[nodiscard]] bool supported() const { return device_.rayQueriesEnabled(); }

  // Record outside rendering on the graphics queue. A previous generation may
  // be reused only after its builds were submitted on that same queue. Returned
  // resources own input and scratch data; abandon the command buffer before
  // releasing them if recording/submission fails. No device-wide idle is added.
  // Reuse is decided independently for each provider-local geometry using its
  // source storage identity, revision, topology and opacity classification.
  [[nodiscard]] std::shared_ptr<const RaySceneGeneration> recordBuild(
      const vk::raii::CommandBuffer &command, const RaySceneInput &input,
      const std::shared_ptr<const RaySceneGeneration> &previous = {}) const;

private:
  const container::gpu::VulkanDevice &device_;
  container::gpu::VulkanMemoryManager &memory_;
  VkPhysicalDeviceAccelerationStructurePropertiesKHR properties_{};
};

} // namespace container::renderer
