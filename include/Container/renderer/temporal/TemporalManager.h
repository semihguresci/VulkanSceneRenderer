#pragma once

#include "Container/utility/ComputePipeline.h"

#include "Container/renderer/resources/FrameResources.h"
#include "Container/renderer/temporal/DlssUpscaler.h"
#include "Container/renderer/temporal/FsrUpscaler.h"
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
class PreparedGraphicsPipelines;
struct FrameRecordParams;
enum class RenderPassId : uint8_t;
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
  [[nodiscard]] bool prepareFsr(const std::filesystem::path& root,
                                const FsrUpscalerSettings& settings);
  [[nodiscard]] std::optional<VkExtent2D> prepareDlss(const std::filesystem::path& root,
                                                     UpscaleMode mode, VkExtent2D display);
  [[nodiscard]] bool fsrActive() const { return fsr_ && fsr_->available(); }
  [[nodiscard]] bool dlssActive() const { return dlss_ && dlss_->available(); }
  [[nodiscard]] std::optional<uint64_t> dlssAllocatedBytes() const {
    return dlss_ ? dlss_->allocatedBytes() : std::nullopt;
  }
  [[nodiscard]] bool upscalingActive() const { return fsrActive() || dlssActive(); }
  [[nodiscard]] std::string_view upscalingStatus() const { return upscalingStatus_; }
  void prepareCamera(container::gpu::CameraData& camera, glm::uvec2 extent,
                     const container::temporal::Settings& settings,
                     const glm::mat4& projection, float frameMilliseconds);
  [[nodiscard]] PreparedGraphicsPipelines prepareVelocityReload(const std::filesystem::path& root);
  void publishVelocityReload(PreparedGraphicsPipelines& candidate) noexcept;
  void prepare(FrameResourceManager &frames, VkExtent2D extent,
               uint32_t imageCount);
  void updateDescriptors(uint32_t image, const FrameResources &frame,
                         const container::gpu::AllocatedBuffer &camera,
                         bool resolveThisFrame);
  void recordVelocity(VkCommandBuffer cmd, const FrameRecordParams &frame);
  void recordResolve(VkCommandBuffer cmd, const FrameRecordParams &frame);
  void commit();
  void reset(std::string reason) { state_.reset(std::move(reason)); }
  bool active() const { return (settings_.enabled || upscalingActive()) && !inputs_.empty(); }
  VkImageView outputView(uint32_t image) const;
  VkImage outputImage(uint32_t image) const;
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
    VkDescriptorSet upscaleInputs{};
    VkDescriptorSet dlssTransparency{};
    AttachmentImage upscaleDepth, upscaleMotion, upscaleReactive, upscaleTransparency, upscaleOutput;
    AttachmentImage dlssOitLayer;
    bool initialized{false};
  };
  uint32_t writeSlot() const {
    return static_cast<uint32_t>(state_.frameId() & 1u);
  }
  AttachmentImage image(VkFormat format, VkImageUsageFlags usage, VkExtent2D extent);
  void destroyImage(AttachmentImage &image);
  void destroyFrameImages();
  void createUpscaleInputPipeline(const std::filesystem::path& root);
  void createDlssCompositionPipelines(const std::filesystem::path& root);
  void destroyDlssCompositionPipelines();
  void updateUpscaleDescriptors(const Input& input);
  void updateDlssTransparencyDescriptors(const Input& input);
  void recordUpscale(VkCommandBuffer cmd, const FrameRecordParams& frame, const Input& input,
                     bool hasTransparency);
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager &allocator_;
  container::gpu::PipelineManager &pipelines_;
  container::temporal::Settings settings_{};
  container::temporal::State state_{};
  std::array<History, 2> histories_{};
  std::vector<Input> inputs_;
  VkExtent2D extent_{};
  std::unique_ptr<FsrUpscaler> fsr_;
  std::unique_ptr<DlssUpscaler> dlss_;
  VkExtent2D upscaleRenderExtent_{}, upscaleDisplayExtent_{};
  std::string upscalingStatus_{"native resolution"};
  std::array<float, 2> upscaleJitter_{};
  float fsrVerticalFov_{}, upscaleFrameMilliseconds_{};
  VkDescriptorSetLayout resolveSetLayout_{}, composeSetLayout_{};
  VkDescriptorSetLayout upscaleInputsSetLayout_{};
  VkDescriptorSetLayout dlssComposeSetLayout_{}, dlssTransparencySetLayout_{};
  VkPipelineLayout resolveLayout_{}, composeLayout_{}, velocityLayout_{};
  VkPipelineLayout upscaleInputsLayout_{};
  VkPipelineLayout dlssComposeLayout_{}, dlssTransparencyLayout_{};
  container::gpu::ComputePipeline resolvePipeline_{}, composePipeline_{};
  container::gpu::ComputePipeline upscaleInputsPipeline_{};
  container::gpu::ComputePipeline dlssComposePipeline_{}, dlssTransparencyPipeline_{};
  std::array<VkPipeline, 3> velocityPipelines_{};
  VkFormat velocityDepthFormat_{VK_FORMAT_UNDEFINED};
  VkDescriptorPool pool_{};
  VkSampler sampler_{};
  bool resolveRecorded_{false};
};

void registerTemporalPasses(RenderGraphBuilder& graph, RenderPassId predecessor);
VkImageLayout temporalSceneColorLayout(const FrameRecordParams &frame);
VkImageView temporalSceneColorView(const FrameRecordParams &frame,
                                   VkImageView nativeView);

} // namespace container::renderer
