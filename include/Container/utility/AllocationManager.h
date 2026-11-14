#pragma once

#include "Container/app/AppConfig.h"
#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonVMA.h"
#include "Container/geometry/Vertex.h"
#include "Container/utility/MaterialManager.h"
#include "Container/utility/TextureResource.h"
#include "Container/utility/TextureImageData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace container::gpu {

enum class TextureAllocationLifetime : uint32_t {
  Scene = 0,
  Persistent = 1,
};

enum class TextureAllocationResetScope : uint32_t {
  SceneOnly = 0,
  All = 1,
};

struct TextureAllocation {
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  VmaAllocation allocation{nullptr};
  TextureAllocationLifetime lifetime{TextureAllocationLifetime::Scene};
  // Zero is an ordinary allocation; nonzero IDs are never reused by an owner.
  uint64_t batchGroup{0};
  uint64_t incarnation{};
  uint32_t imageLeaseCount{};
  bool aggregateRetained{};
  bool individuallyManaged{};
};

class AllocationManager {
  struct TextureBatchControl;

 public:
  class TextureAllocationBatch;
  class TextureImageAllocationLease {
   public:
    ~TextureImageAllocationLease();
    TextureImageAllocationLease(const TextureImageAllocationLease&) = delete;
    TextureImageAllocationLease& operator=(const TextureImageAllocationLease&) = delete;
    [[nodiscard]] const TextureAllocationIdentity& identity() const noexcept { return identity_; }
   private:
    friend class AllocationManager;
    TextureImageAllocationLease(std::weak_ptr<TextureBatchControl>,TextureAllocationIdentity) noexcept;
    std::weak_ptr<TextureBatchControl> control_;
    TextureAllocationIdentity identity_;
    bool armed_{};
  };
  class TextureAllocationLease {
   public:
    ~TextureAllocationLease();
    TextureAllocationLease(const TextureAllocationLease&) = delete;
    TextureAllocationLease& operator=(const TextureAllocationLease&) = delete;
    [[nodiscard]] uint64_t groupId() const noexcept { return group_; }

   private:
    friend class AllocationManager;
    friend class TextureAllocationBatch;
    TextureAllocationLease(std::weak_ptr<TextureBatchControl> control,
                           uint64_t group) noexcept;
    std::weak_ptr<TextureBatchControl> control_;
    uint64_t group_{};
    bool committed_{};
  };

  class TextureAllocationBatch {
   public:
    TextureAllocationBatch() = default;
    ~TextureAllocationBatch() = default;
    TextureAllocationBatch(const TextureAllocationBatch&) = delete;
    TextureAllocationBatch& operator=(const TextureAllocationBatch&) = delete;
    TextureAllocationBatch(TextureAllocationBatch&&) noexcept = default;
    TextureAllocationBatch& operator=(TextureAllocationBatch&&) noexcept = default;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] uint64_t groupId() const noexcept;
    // The valid success path uses the lease allocated by begin: no allocation,
    // upload, callback or GPU synchronization. Invalid/stale commits throw.
    [[nodiscard]] std::shared_ptr<const TextureAllocationLease> commit();

   private:
    friend class AllocationManager;
    explicit TextureAllocationBatch(std::shared_ptr<TextureAllocationLease> lease)
        noexcept : lease_(std::move(lease)) {}
    std::shared_ptr<TextureAllocationLease> lease_;
  };

  AllocationManager() = default;
  ~AllocationManager();

  void initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
                  VkDevice device, VkQueue graphicsQueue,
                  VkCommandPool commandPool, const app::AppConfig& config);

  void cleanup();
  // False pumps events during a transfer; true may draw the loading view.
  void setLoadProgressCallback(std::function<void(bool)> callback) {
    loadProgress_ = std::move(callback);
  }

  BufferSlice uploadVertices(std::span<const geometry::Vertex> vertices);
  BufferSlice uploadIndices(std::span<const uint32_t> indices);
  // Caller must retire all GPU readers before reusing scene geometry storage.
  void resetGeometryAllocations();
  [[nodiscard]] VkDeviceSize geometryArenaCapacityBytes() const;
  AllocatedBuffer uploadBuffer(std::span<const std::byte> bytes,
                               VkBufferUsageFlags usage);
  // Records detached resource preparation on the graphics queue and waits for
  // completion before returning, including when a progress callback throws.
  // The caller retains every resource referenced by record through this call.
  // A recording exception frees the unsubmitted command without executing it.
  void executeStagedCommands(const std::function<void(VkCommandBuffer)>& record);

  [[nodiscard]] AllocatedBuffer createBuffer(
      VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage,
      VmaAllocationCreateFlags allocationFlags = 0,
      VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      std::span<const uint32_t> queueFamilies = {});

  void destroyBuffer(AllocatedBuffer& buffer);

  container::material::TextureResource createTextureFromFile(
      const std::string& texturePath,
      VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);
  container::material::TextureResource createTextureFromEncodedBytes(
      const std::string& textureName,
      std::span<const std::byte> encodedBytes,
      VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);
  // Compression features must be the enabled logical-device features, rather
  // than only physical-device support. Unspecified features use CPU fallbacks.
  void setTextureCompressionFeatures(const VkPhysicalDeviceFeatures& enabled) {
    enabledTextureFeatures_ = enabled;
  }
  [[nodiscard]] container::material::TextureDecodeOptions textureDecodeOptions(
      bool isSrgb, bool strictKhrBasisu = false) const;
  // A nonempty family list includes the graphics upload queue and the other
  // consumers. It must contain at least two distinct queue families.
  container::material::TextureResource createTextureFromImageData(
      const std::string& textureName,
      const container::material::TextureImageData& image,
      std::span<const uint32_t> queueFamilies = {});
  container::material::TextureResource
  createTextureFromRgbaPixels(const std::string &textureName,
                              std::span<const std::byte> rgbaPixels,
                              uint32_t width, uint32_t height, VkFormat format,
                              std::span<const uint32_t> queueFamilies = {});
  container::material::TextureArrayResource createTexture2DArrayFromRgbaPixels(
      const std::string& textureName,
      std::span<const std::byte> rgbaPixels,
      uint32_t width,
      uint32_t height,
      uint32_t layerCount,
      VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
      TextureAllocationLifetime lifetime = TextureAllocationLifetime::Scene);

  // Force-reset semantics also apply to committed groups: retire GPU readers
  // and history first, then never republish aliases in the reset scope.
  // Resets/cleanup reject an active batch or upload before changing any state.
  void resetTextureAllocations(
      TextureAllocationResetScope scope =
          TextureAllocationResetScope::SceneOnly);

  // One owner-thread/external-synchronization transaction at a time. New scene
  // uploads belong to its group. Persistent uploads are rejected in a batch.
  // Uncommitted textures must never be published to GPU readers. Destruction
  // rolls back only that group and invokes no loading/progress callbacks.
  // After commit, generations/history retain the lease until readers retire;
  // final lease release waits for device idle before destroying the group.
  // TextureResource copies are non-owning aliases and do not retain this lease.
  // A lease/batch may outlive this allocator safely, without retaining Vulkan.
  [[nodiscard]] TextureAllocationBatch beginTextureAllocationBatch();
  // Owner-thread preparation only; may allocate. A committed batch lease still
  // protects every member. After it releases, only independently retained images
  // survive. Group-zero Scene images keep their reset-only owner until the
  // explicit adoption commit below, so abandoned preparations cannot free the
  // existing published scene. Raw resource/identity copies are not pins.
  // Rejects active uploads/batches, Persistent, stale/foreign/default identities
  // and mismatched image/view pairs. Final shared_ptr moves/swaps do not allocate.
  [[nodiscard]] std::shared_ptr<const TextureImageAllocationLease> retainTextureImage(
      const container::material::TextureResource&);
  // Non-owning, allocation-free proof for descriptor staging and stale-token
  // validation. Requires an exact live Scene image capability from this owner;
  // rejects uploads/uncommitted batches, force-reset images and handle ABA.
  // Success does not retain the image: the caller must already hold its owner.
  [[nodiscard]] bool textureImageMatches(
      const container::material::TextureResource&) const noexcept;
  // Sorted, unique image incarnations from this owner only, all Scene/group0
  // and not already adopted. Empty/invalid/stale/duplicate/foreign lists reject.
  // Caller proves EVERY published alias now has a lease and all outer guards
  // passed. The commit changes ownership flags only; it does not synchronize,
  // allocate, free, or partially adopt on failure. Last lease release after a
  // successful adoption can retire the image and wait for its GPU readers.
  [[nodiscard]] bool canAdoptTextureImages(
      std::span<const std::shared_ptr<const TextureImageAllocationLease>>) const noexcept;
  [[nodiscard]] bool adoptTextureImages(
      std::span<const std::shared_ptr<const TextureImageAllocationLease>>) noexcept;

  [[nodiscard]] VulkanMemoryManager* memoryManager() const { return memoryManager_.get(); }

 private:
  // Declared before staging/image locals so the allocator remains protected
  // until their normal destruction or complete exception unwinding.
  class UploadOperation {
   public:
    explicit UploadOperation(AllocationManager& owner);
    ~UploadOperation();
    UploadOperation(const UploadOperation&) = delete;
    UploadOperation& operator=(const UploadOperation&) = delete;
   private:
    AllocationManager& owner_;
  };

  VkCommandBuffer beginSingleTimeCommands();
  void endSingleTimeCommands(VkCommandBuffer commandBuffer);

  void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size,
                  VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);

  void transitionImageLayout(VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout,
                             uint32_t layerCount = 1u,
                             uint32_t baseMipLevel = 0u,
                             uint32_t levelCount = 1u);

  void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                         uint32_t height, uint32_t layerCount = 1u);
  void generateTextureMipmaps(VkImage image, VkFormat format, uint32_t width,
                              uint32_t height, uint32_t mipLevels);

  VkImageView createImageView(VkImage image, VkFormat format,
                              VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D,
                              uint32_t layerCount = 1u,
                              uint32_t levelCount = 1u);

  [[nodiscard]] uint64_t textureAllocationGroup(
      TextureAllocationLifetime lifetime) const;
  [[nodiscard]] TextureAllocationIdentity registerTextureAllocation(TextureAllocation allocation);
  void destroyTextureAllocation(TextureAllocation& allocation) noexcept;
  void retireTextureAllocationGroup(uint64_t group, bool committed) noexcept;
  void retireTextureImage(uint64_t incarnation) noexcept;
  [[nodiscard]] bool textureImagesAdoptable(
      std::span<const std::shared_ptr<const TextureImageAllocationLease>>) const noexcept;
  void invalidateTextureBatchControl() noexcept;

  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphicsQueue_{VK_NULL_HANDLE};
  VkCommandPool commandPool_{VK_NULL_HANDLE};
  app::AppConfig config_{};

  std::unique_ptr<VulkanMemoryManager> memoryManager_;
  std::unique_ptr<BufferArena> vertexArena_;
  std::unique_ptr<BufferArena> indexArena_;

  std::vector<TextureAllocation> textureAllocations_{};
  std::shared_ptr<TextureBatchControl> textureBatchControl_;
  uint64_t activeTextureBatchGroup_{};
  uint64_t nextTextureBatchGroup_{1};
  uint64_t nextTextureAllocationIncarnation_{1};
  bool textureRetirementInProgress_{};
  uint32_t activeUploadOperations_{};
  std::function<void(bool)> loadProgress_;
  VkPhysicalDeviceFeatures enabledTextureFeatures_{};
};

}  // namespace container::gpu
