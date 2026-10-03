#pragma once

#include "Container/renderer/resources/FrameResources.h"
#include "Container/renderer/temporal/TemporalState.h"
#include <array>
#include <filesystem>
#include <memory>
#include <vector>

namespace container::gpu {
class VulkanDevice;
class AllocationManager;
class PipelineManager;
} // namespace container::gpu
namespace container::renderer {
struct FrameRecordParams;
class RenderGraphBuilder;
class FrameResourceManager;

// All histories use the graphics queue. Synchronization2 barriers order both
// RAW and WAR accesses across submissions; descriptor sets are per
// image/parity.
class TemporalManager {
public:
  TemporalManager(std::shared_ptr<container::gpu::VulkanDevice> device,
                  container::gpu::AllocationManager &allocator,
                  container::gpu::PipelineManager &pipelines);
  ~TemporalManager();
  TemporalManager(const TemporalManager &) = delete;
  TemporalManager &operator=(const TemporalManager &) = delete;
  container::temporal::Settings &settings() { return settings_; }
  const container::temporal::Settings &settings() const { return settings_; }
  container::temporal::State &state() { return state_; }
  const container::temporal::State &state() const { return state_; }
  void createPipelines(const std::filesystem::path &root,
                       VkDescriptorSetLayout sceneLayout, VkFormat depthFormat);
  void prepare(FrameResourceManager &frames, VkExtent2D extent,
               uint32_t imageCount);
  void updateDescriptors(uint32_t image, const FrameResources &frame,
                         const container::gpu::AllocatedBuffer &camera,
                         bool resolveThisFrame);
  void recordVelocity(VkCommandBuffer cmd, const FrameRecordParams &frame);
  void recordResolve(VkCommandBuffer cmd, const FrameRecordParams &frame);
  void commit();
  void reset(std::string reason) { state_.reset(std::move(reason)); }
  bool active() const { return settings_.enabled && !inputs_.empty(); }
  VkImageView outputView() const { return histories_[writeSlot()].color.view; }
  VkImageView diagnosticView(uint32_t image) const;
  uint64_t memoryBytes() const;
  uint64_t allocatedBytes() const;
  void destroyImages();

private:
  struct History {
    AttachmentImage color, depth, identity;
    bool initialized{false};
  };
  struct Input {
    FrameResources frame{};
    std::array<VkDescriptorSet, 2> resolve{};
    VkDescriptorSet compose{};
    bool initialized{false};
  };
  uint32_t writeSlot() const {
    return static_cast<uint32_t>(state_.frameId() & 1u);
  }
  AttachmentImage image(VkFormat format, VkImageUsageFlags usage);
  void destroyImage(AttachmentImage &image);
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager &allocator_;
  container::gpu::PipelineManager &pipelines_;
  container::temporal::Settings settings_{};
  container::temporal::State state_{};
  std::array<History, 2> histories_{};
  std::vector<Input> inputs_;
  VkExtent2D extent_{};
  VkDescriptorSetLayout resolveSetLayout_{}, composeSetLayout_{};
  VkPipelineLayout resolveLayout_{}, composeLayout_{}, velocityLayout_{};
  VkPipeline resolvePipeline_{}, composePipeline_{};
  std::array<VkPipeline, 3> velocityPipelines_{};
  VkDescriptorPool pool_{};
  VkSampler sampler_{};
  bool resolveRecorded_{false};
};

void registerTemporalPasses(RenderGraphBuilder &graph);
VkImageLayout temporalSceneColorLayout(const FrameRecordParams &frame);
VkImageView temporalSceneColorView(const FrameRecordParams &frame,
                                   VkImageView nativeView);

} // namespace container::renderer
