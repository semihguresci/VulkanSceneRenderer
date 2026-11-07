#pragma once

#include <Container/geometry/GltfDeformation.h>
#include <Container/utility/VulkanMemoryManager.h>

#include <filesystem>
#include <memory>
#include <span>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

struct GltfDeformationInstance {
  const geometry::gltf::DeformationPrimitive* primitive{nullptr};
  uint32_t outputVertexOffset{0};
};

struct GltfDeformationPose {
  // Empty current weights select the primitive's mesh defaults. A node's
  // resolved default weights must be supplied explicitly for node overrides.
  std::span<const float> morphWeights{};
  std::span<const glm::mat4> skinPalette{};
  // Empty previous inputs select the corresponding current input.
  std::span<const float> previousMorphWeights{};
  std::span<const glm::mat4> previousSkinPalette{};
  bool resetHistory{false};
  // Optional final node-local vertices for primitives requiring regenerated
  // flat normals/MikkTSpace tangents. They bypass GPU morph/skin evaluation;
  // the compute pass copies their exact Vertex bytes into the same outputs.
  // Empty previous vertices select current vertices, including on a reset.
  std::span<const geometry::Vertex> evaluatedVertices{};
  std::span<const geometry::Vertex> previousEvaluatedVertices{};
};

// Records GPU morph-then-skin deformation for a complete scene vertex stream.
// Every instance occupies a separate, non-overlapping output vertex range.
// Unmapped vertices are copied unchanged; all auxiliary vertex bytes survive.
// The output has exactly geometry::Vertex's byte layout and can be consumed by
// ordinary raster draws, acceleration structure builds, and velocity passes.
class GltfDeformationPass {
 public:
  GltfDeformationPass(std::shared_ptr<gpu::VulkanDevice> device,
                       gpu::AllocationManager& allocator,
                       gpu::PipelineManager& pipelines);
  // Also useful for headless/standalone numerical validation with the same
  // production allocator and pipeline implementation.
  GltfDeformationPass(std::shared_ptr<gpu::VulkanDevice> device,
                       gpu::VulkanMemoryManager& memory,
                       gpu::PipelineManager& pipelines);
  ~GltfDeformationPass();
  GltfDeformationPass(const GltfDeformationPass&) = delete;
  GltfDeformationPass& operator=(const GltfDeformationPass&) = delete;

  // shaderDir contains spv_shaders/gltf_deformation.comp.spv (computeMain).
  // Caller retires all pass readers before recreation, setScene, or destruction.
  void createResources(const std::filesystem::path& shaderDir,
                        uint32_t frameCount);
  void setScene(std::span<const geometry::Vertex> initialVertices,
                 std::span<const GltfDeformationInstance> instances);

  // Caller first waits for this frame slot's fence. One pose per instance in
  // setScene order. Uploads are copied immediately; source spans may expire.
  void uploadFrame(uint32_t frameIndex,
                     std::span<const GltfDeformationPose> poses);

  // Record before raster, velocity, readback or acceleration structure builds
  // on the graphics queue. Includes input/reuse and output visibility barriers.
  // Reset history evaluates previous geometry at the current pose, producing
  // zero deformation velocity on first frames, seeks, and clip changes.
  void record(VkCommandBuffer command, uint32_t frameIndex);

  [[nodiscard]] gpu::BufferSlice currentVertices(uint32_t frameIndex) const;
  [[nodiscard]] gpu::BufferSlice previousVertices(uint32_t frameIndex) const;
  [[nodiscard]] uint32_t vertexCount() const;
  [[nodiscard]] size_t instanceCount() const;
  [[nodiscard]] bool isReady() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace container::renderer
