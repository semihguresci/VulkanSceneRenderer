#pragma once
#include <Container/renderer/raytracing/RayScene.h>
#include <Container/utility/VulkanMemoryManager.h>
#include <filesystem>
#include <memory>

namespace container::gpu { class PipelineManager; class VulkanDevice; }
namespace container::renderer {
class RayInstanceTransformPass;
struct RaySceneGpuTransforms {
  RayInstanceTransformPass* producer{};
  gpu::BufferSlice objects{};
  uint32_t objectCount{};
  // Optional output matching RaySurfaceInstanceData; generation-owned.
  gpu::BufferSlice surfaces{};
};

// Publishes private, immutable TLAS input and closest-hit transforms from the
// current graphics image's GPU ObjectData. CPU-only providers in the same TLAS
// carry an explicit fallback transform. The caller retains source objects only
// through this graphics submission; subsequent tracing reads private outputs.
// The memory and pipeline managers outlive every returned Publication.
class RayInstanceTransformPass {
  struct Impl;
 public:
  class Publication {
   public:
    Publication();
    ~Publication();
    Publication(Publication&&) noexcept;
    Publication& operator=(Publication&&) noexcept;
    Publication(const Publication&) = delete;
    Publication& operator=(const Publication&) = delete;
    [[nodiscard]] gpu::AllocatedBuffer inputBuffer() const;
    [[nodiscard]] uint64_t uploadedBytes() const;
   private:
    friend class RayInstanceTransformPass;
    struct State;
    std::unique_ptr<State> state_;
  };
  RayInstanceTransformPass(std::shared_ptr<gpu::VulkanDevice>,gpu::VulkanMemoryManager&,gpu::PipelineManager&);
  ~RayInstanceTransformPass();
  void createResources(const std::filesystem::path&);
  [[nodiscard]] VkDevice device() const noexcept;
  // Validate before allocating/recording any AS work.
  void validate(const RaySceneGpuTransforms&,std::span<const RaySceneInstance>) const;
  [[nodiscard]] Publication record(VkCommandBuffer, const RaySceneGpuTransforms&,
      std::span<const RaySceneInstance>,std::span<const VkAccelerationStructureInstanceKHR>,gpu::BufferSlice destination);
 private:
  std::shared_ptr<Impl> impl_;
};
}
