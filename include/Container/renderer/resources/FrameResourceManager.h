#pragma once

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/resources/FrameResources.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class SwapChainManager;
class VulkanDevice;
struct AllocatedBuffer;
}  // namespace container::gpu

namespace container::renderer {

// Per-node data in the OIT linked list. The layout is consumed directly by
// shaders/oit_common.slang; keep alignment and field order in sync with
// TransparentNode.
struct OitNode {
  alignas(16) glm::vec4 color{0.0f};
  alignas(4)  float     depth{0.0f};
  alignas(4)  uint32_t  next{~0u};
  alignas(8)  glm::vec2 padding{0.0f};
};

// Uniform uploaded per-frame for the OIT resolve pass.
struct OitMetadata {
  alignas(4) uint32_t nodeCapacity{0};
  alignas(4) uint32_t viewportWidth{0};
  alignas(4) uint32_t viewportHeight{0};
  alignas(4) uint32_t reserved{0};
};

static_assert(sizeof(OitNode) == 32,
              "OitNode must match shaders/oit_common.slang TransparentNode.");
static_assert(offsetof(OitNode, color) == 0, "OitNode.color offset");
static_assert(offsetof(OitNode, depth) == 16, "OitNode.depth offset");
static_assert(offsetof(OitNode, next) == 20, "OitNode.next offset");
static_assert(offsetof(OitNode, padding) == 24, "OitNode.padding offset");
static_assert(sizeof(OitMetadata) == 16,
              "OitMetadata must match shaders/oit_common.slang OitMetadataBuffer.");
static_assert(offsetof(OitMetadata, nodeCapacity) == 0,
              "OitMetadata.nodeCapacity offset");
static_assert(offsetof(OitMetadata, viewportWidth) == 4,
              "OitMetadata.viewportWidth offset");
static_assert(offsetof(OitMetadata, viewportHeight) == 8,
              "OitMetadata.viewportHeight offset");
static_assert(offsetof(OitMetadata, reserved) == 12,
              "OitMetadata.reserved offset");

// Aggregated format bundle passed into FrameResourceManager::create().
struct GBufferFormats {
  VkFormat depthStencil{VK_FORMAT_UNDEFINED};
  VkFormat sceneColor{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat albedo{VK_FORMAT_R8G8B8A8_UNORM};
  VkFormat normal{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat material{VK_FORMAT_R32G32B32A32_SFLOAT};
  VkFormat emissive{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat specular{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat pickId{VK_FORMAT_R32_UINT};
  VkFormat oitHeadPointer{VK_FORMAT_R32_UINT};

  // Returns a GBufferFormats initialised with all defaults except
  // depthStencil, which must be queried from RenderPassManager at runtime.
  [[nodiscard]] static GBufferFormats defaults() {
    return {};  // all member initialisers above already hold the defaults
  }
};

class FrameResourceManager {
 public:
  FrameResourceManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::PipelineManager&            pipelineManager,
      container::gpu::SwapChainManager&                     swapChainManager,
      VkCommandPool                                  commandPool);

  ~FrameResourceManager();
  FrameResourceManager(const FrameResourceManager&) = delete;
  FrameResourceManager& operator=(const FrameResourceManager&) = delete;

  // Call once after pipeline manager is ready.
  void createDescriptorSetLayouts();
  void createGBufferSampler();

  [[nodiscard]] VkDescriptorSetLayout lightingLayout()    const { return lightingLayout_; }
  [[nodiscard]] VkDescriptorSetLayout postProcessLayout() const { return postProcessLayout_; }
  [[nodiscard]] VkDescriptorSetLayout oitLayout()         const { return oitLayout_; }
  [[nodiscard]] VkSampler             gBufferSampler()    const { return gBufferSampler_; }

  // Create / recreate per-swapchain-image attachments, descriptor sets, and
  // OIT storage. Call after swapchain resize or any layout-affecting resource
  // change.
  void create(const GBufferFormats&                    formats,
              RenderingPassHandle                             depthPrepassPass,
              RenderingPassHandle                             bimDepthPrepassPass,
              RenderingPassHandle                             gBufferPass,
              RenderingPassHandle                             bimGBufferPass,
              RenderingPassHandle                             transparentPickPass,
              RenderingPassHandle                             lightingPass,
              RenderingPassHandle                             forwardLightingPass,
              RenderingPassHandle                             forwardTransparentPass,
              RenderingPassHandle                             transformGizmoPass,
              VkSampleCountFlagBits                    msaaSampleCount,
              std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
              const container::gpu::AllocatedBuffer& objectBuffer);

  void destroy();
  void ensureTemporalAttachments();

  void updateDescriptorSets(std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
                            const container::gpu::AllocatedBuffer& objectBuffer,
                            VkImageView shadowAtlasView = VK_NULL_HANDLE,
                            VkSampler   shadowSampler   = VK_NULL_HANDLE,
                            std::span<const container::gpu::AllocatedBuffer> shadowUbos = {},
                            VkImageView localShadowAtlasView = VK_NULL_HANDLE,
                            VkSampler   localShadowSampler = VK_NULL_HANDLE,
                            std::span<const container::gpu::AllocatedBuffer> localShadowUbos = {},
                            VkImageView irradianceView    = VK_NULL_HANDLE,
                            VkImageView prefilteredView   = VK_NULL_HANDLE,
                            VkImageView brdfLutView       = VK_NULL_HANDLE,
                            VkSampler   envSampler        = VK_NULL_HANDLE,
                            VkSampler   brdfLutSampler    = VK_NULL_HANDLE,
                            VkImageView aoTextureView     = VK_NULL_HANDLE,
                            VkSampler   aoSampler         = VK_NULL_HANDLE,
                            VkImageView bloomTextureView  = VK_NULL_HANDLE,
                            VkSampler   bloomSampler      = VK_NULL_HANDLE,
                            VkBuffer    tileGridBuffer    = VK_NULL_HANDLE,
                            VkDeviceSize tileGridBufferSize = 0,
                            std::span<const container::gpu::AllocatedBuffer> exposureStateBuffers = {},
                            VkDeviceSize exposureStateBufferSize = 0);

  void validateOitFormatSupport() const;
  void validatePickIdFormatSupport() const;

  // Returns true if the OIT node pool was grown (caller must recreate).
  bool growOitPoolIfNeeded(uint32_t imageIndex);

  [[nodiscard]] uint32_t computeOitNodeCapacity() const;
  [[nodiscard]] uint32_t oitNodeCapacityFloor() const {
    return oitNodeCapacityFloor_;
  }

  [[nodiscard]] uint32_t frameCount() const {
    return static_cast<uint32_t>(frames_.size());
  }
  [[nodiscard]] const FrameResourceRegistry& resourceRegistry() const {
    return resourceRegistry_;
  }
  [[nodiscard]] const FrameResources* frame(uint32_t imageIndex) const;
  [[nodiscard]] FrameResources*       frame(uint32_t imageIndex);

 private:
  AttachmentImage createAttachment(VkFormat fmt, VkImageUsageFlags usage,
                                   VkImageAspectFlags aspect,
                                   VkSampleCountFlagBits samples =
                                       VK_SAMPLE_COUNT_1_BIT) const;
  void            destroyAttachment(AttachmentImage& a) const;
  void            transitionToGeneral(VkImage image, VkImageAspectFlags mask) const;
  void            transitionToDepthAttachment(VkImage image,
                                              VkImageAspectFlags mask) const;
  void            transitionToShaderReadOnly(VkImage image,
                                             VkImageAspectFlags mask,
                                             uint32_t layerCount) const;
  void            writeOitMetadata(FrameResources& frame) const;
  void            publishFrameResourceBindings();
  void            ensureFallbackTileGridBuffer();
  void            ensureFallbackExposureStateBuffer();
  void            ensureFallbackShadowDataBuffer();
  void            ensureFallbackShadowResources();
  void            ensureFallbackLocalShadowDataBuffer();
  void            ensureFallbackLocalShadowResources();
  VkCommandBuffer beginImmediate() const;
  void            endImmediate(VkCommandBuffer cmd) const;

  struct DescriptorUpdateKey {
    std::vector<VkBuffer> cameraBuffers{};
    std::vector<VkBuffer> shadowUboBuffers{};
    std::vector<VkBuffer> localShadowUboBuffers{};
    VkImageView shadowAtlasView{VK_NULL_HANDLE};
    VkSampler   shadowSampler{VK_NULL_HANDLE};
    VkImageView localShadowAtlasView{VK_NULL_HANDLE};
    VkSampler   localShadowSampler{VK_NULL_HANDLE};
    VkImageView irradianceView{VK_NULL_HANDLE};
    VkImageView prefilteredView{VK_NULL_HANDLE};
    VkImageView brdfLutView{VK_NULL_HANDLE};
    VkSampler   envSampler{VK_NULL_HANDLE};
    VkSampler   brdfLutSampler{VK_NULL_HANDLE};
    VkImageView aoTextureView{VK_NULL_HANDLE};
    VkSampler   aoSampler{VK_NULL_HANDLE};
    VkImageView bloomTextureView{VK_NULL_HANDLE};
    VkSampler   bloomSampler{VK_NULL_HANDLE};
    VkBuffer    tileGridBuffer{VK_NULL_HANDLE};
    VkDeviceSize tileGridBufferSize{0};
    std::vector<VkBuffer> exposureStateBuffers{};
    VkDeviceSize exposureStateBufferSize{0};

    bool operator==(const DescriptorUpdateKey&) const = default;
  };

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager*            allocationMgr_{nullptr};
  container::gpu::PipelineManager*            pipelineMgr_{nullptr};
  container::gpu::SwapChainManager*                     swapChain_{nullptr};
  VkCommandPool                                  commandPool_{VK_NULL_HANDLE};

  VkDescriptorSetLayout lightingLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout postProcessLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout oitLayout_{VK_NULL_HANDLE};
  VkSampler             gBufferSampler_{VK_NULL_HANDLE};

  VkDescriptorPool lightingPool_{VK_NULL_HANDLE};
  VkDescriptorPool postProcessPool_{VK_NULL_HANDLE};
  VkDescriptorPool oitPool_{VK_NULL_HANDLE};
  container::gpu::AllocatedBuffer fallbackTileGridBuffer_{};
  container::gpu::AllocatedBuffer fallbackExposureStateBuffer_{};
  container::gpu::AllocatedBuffer fallbackShadowDataBuffer_{};
  AttachmentImage                 fallbackShadowAtlas_{};
  VkSampler                       fallbackShadowSampler_{VK_NULL_HANDLE};
  container::gpu::AllocatedBuffer fallbackLocalShadowDataBuffer_{};
  AttachmentImage                 fallbackLocalShadowAtlas_{};
  VkSampler                       fallbackLocalShadowSampler_{VK_NULL_HANDLE};

  GBufferFormats formats_{};
  RenderingPassHandle   depthPrepassPass_{VK_NULL_HANDLE};
  RenderingPassHandle   bimDepthPrepassPass_{VK_NULL_HANDLE};
  RenderingPassHandle   gBufferPass_{VK_NULL_HANDLE};
  RenderingPassHandle   bimGBufferPass_{VK_NULL_HANDLE};
  RenderingPassHandle   transparentPickPass_{VK_NULL_HANDLE};
  RenderingPassHandle   lightingPass_{VK_NULL_HANDLE};
  RenderingPassHandle   forwardLightingPass_{VK_NULL_HANDLE};
  RenderingPassHandle   forwardTransparentPass_{VK_NULL_HANDLE};
  RenderingPassHandle   transformGizmoPass_{VK_NULL_HANDLE};
  VkSampleCountFlagBits sampleCount_{VK_SAMPLE_COUNT_1_BIT};

  std::vector<FrameResources> frames_;
  FrameResourceRegistry resourceRegistry_{};
  DescriptorUpdateKey descriptorUpdateKey_{};
  bool descriptorUpdateKeyValid_{false};
  uint32_t oitNodeCapacityFloor_{0};

  static constexpr uint32_t kOitAvgNodesPerPixel = 2u;
};

}  // namespace container::renderer
