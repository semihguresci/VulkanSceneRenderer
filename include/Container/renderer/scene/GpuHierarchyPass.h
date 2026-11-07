#pragma once

#include <Container/utility/VulkanMemoryManager.h>
#include <glm/glm.hpp>
#include <filesystem>
#include <memory>
#include <span>

namespace container::gpu {
class PipelineManager;
class VulkanDevice;
struct ObjectData;
}
namespace container::renderer {

struct HierarchyLocalTransform {
  glm::mat4 local{1.0f};
  uint32_t parent{UINT32_MAX};
};
struct HierarchyUploadStats {
  uint32_t nodes{}, objects{}, levels{}, dirtyLocals{};
  uint32_t dispatchedLevels{};
  uint64_t uploadedBytes{};
  uint64_t objectMetadataBytes{};
  double objectMetadataMs{};
  double prepareMs{};
  double previousGpuMs{-1.0};
};

// Optional graphics-queue hierarchy propagation. Each image owns its input,
// topology and world buffers; preparing an image requires its fence to retire.
// The CPU mirror remains authoritative for editing, bounds, cull routing and
// ray-scene extraction. Compute publishes full object records from propagated
// matrices and compact CPU metadata, preserving submitted temporal history.
class GpuHierarchyPass {
 public:
  GpuHierarchyPass(std::shared_ptr<gpu::VulkanDevice>, gpu::VulkanMemoryManager&,
                   gpu::PipelineManager&);
  ~GpuHierarchyPass();
  GpuHierarchyPass(const GpuHierarchyPass&) = delete;
  GpuHierarchyPass& operator=(const GpuHierarchyPass&) = delete;
  // Retire all pass readers before recreation or destruction.
  void createResources(const std::filesystem::path& shaderDirectory, uint32_t imageCount);
  // UINT32_MAX object bindings (diagnostic objects) retain their CPU matrices.
  // Invalid/cyclic/non-affine input is rejected before changing the frame.
  void prepareFrame(uint32_t image, std::span<const HierarchyLocalTransform>,
                    std::span<const uint32_t> objectNodes, gpu::BufferSlice objects);
  // After prepareFrame and temporal-history preparation, before record. Only
  // changed 112-byte metadata rows are uploaded; CPU-only diagnostic objects
  // additionally carry their 112-byte current model/normal prefix.
  void uploadObjectMetadata(uint32_t image,std::span<const gpu::ObjectData>);
  void record(VkCommandBuffer, uint32_t image);
  // A discarded recording must not advance the retained-world cache. Call
  // once after successful graphics submission, before preparing this image again.
  void commitFrame(uint32_t image) noexcept;
  [[nodiscard]] gpu::BufferSlice worldMatrices(uint32_t image) const;
  [[nodiscard]] HierarchyUploadStats stats(uint32_t image) const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace container::renderer
