#pragma once

#include "Container/utility/TextureImageData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <memory>
#include <span>

namespace container::gpu {

class AllocationManager;

enum class TextureMipUploadState {
  Idle,
  Uploading,
  Ready,
  Retiring,
};

struct TextureMipUploadProgress {
  VkDeviceSize totalBytes{};
  VkDeviceSize submittedBytes{};
  VkDeviceSize completedBytes{};
  VkDeviceSize stagingBytes{};
};

// Detached physical allocation with a view covering all uploaded mip levels.
// Retire all descriptor readers before dropping its last owner.
class UploadedTextureMipTail {
 public:
  ~UploadedTextureMipTail();
  UploadedTextureMipTail(const UploadedTextureMipTail&) = delete;
  UploadedTextureMipTail& operator=(const UploadedTextureMipTail&) = delete;

  [[nodiscard]] VkImage image() const noexcept { return image_; }
  [[nodiscard]] VkImageView view() const noexcept { return view_; }
  [[nodiscard]] VkFormat format() const noexcept { return format_; }
  [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
  [[nodiscard]] uint32_t firstMip() const noexcept { return firstMip_; }
  [[nodiscard]] uint32_t mipCount() const noexcept { return mipCount_; }
  [[nodiscard]] VkDeviceSize residentBytes() const noexcept { return residentBytes_; }

 private:
  friend class TextureMipUpload;
  UploadedTextureMipTail() = default;

  VkDevice device_{VK_NULL_HANDLE};
  VmaAllocator allocator_{nullptr};
  VkImage image_{VK_NULL_HANDLE};
  VmaAllocation allocation_{nullptr};
  VkImageView view_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  VkExtent2D extent_{};
  uint32_t firstMip_{};
  uint32_t mipCount_{};
  VkDeviceSize residentBytes_{};
  SceneAssetBudget::Reservation budget_;
};

// Single upload job on AllocationManager's graphics queue. Call from its
// externally synchronized owner thread and retire results before destroying
// the allocator. Other queues require an ordered graphics-to-reader semaphore
// handoff in addition to concurrent sharing.
class TextureMipUpload {
 public:
  TextureMipUpload(AllocationManager& allocations, uint32_t graphicsFamily) noexcept;
  ~TextureMipUpload();
  TextureMipUpload(const TextureMipUpload&) = delete;
  TextureMipUpload& operator=(const TextureMipUpload&) = delete;

  // Device-memory requirement for the rebased image, excluding staging.
  [[nodiscard]] VkDeviceSize estimateResidentBytes(
      const material::TextureImageData& source, uint32_t firstMip,
      std::span<const uint32_t> queueFamilies = {}) const;
  // Allocates only the requested tail and one reusable staging slab.
  void begin(std::shared_ptr<const material::TextureImageData> source,
             uint32_t firstMip, VkDeviceSize chunkCap,
             std::span<const uint32_t> queueFamilies = {});
  // Poll the previous submission and submit at most one tile. A zero budget
  // only polls. Returns the exact bytes submitted this tick.
  [[nodiscard]] VkDeviceSize tick(VkDeviceSize budget = VK_WHOLE_SIZE);
  [[nodiscard]] TextureMipUploadState state() const noexcept { return state_; }
  [[nodiscard]] bool ready() const noexcept { return state_ == TextureMipUploadState::Ready; }
  [[nodiscard]] TextureMipUploadProgress progress() const noexcept { return progress_; }
  [[nodiscard]] std::unique_ptr<UploadedTextureMipTail> takeReady();

  // Stop taking CPU bytes now and retire any in-flight copy on a later tick.
  // drain() waits for at most the single outstanding submission at teardown.
  void cancel() noexcept;
  void drain() noexcept;

 private:
  void releaseSubmission() noexcept;
  void clear() noexcept;

  AllocationManager& allocations_;
  uint32_t graphicsFamily_;
  std::shared_ptr<const material::TextureImageData> source_;
  std::unique_ptr<UploadedTextureMipTail> target_;
  std::unique_ptr<StagingBuffer> staging_;
  VkCommandPool pool_{VK_NULL_HANDLE};
  VkCommandBuffer command_{VK_NULL_HANDLE};
  VkFence fence_{VK_NULL_HANDLE};
  TextureMipUploadState state_{TextureMipUploadState::Idle};
  TextureMipUploadProgress progress_{};
  material::TextureFormatBlock block_{};
  size_t sourceMip_{};
  VkDeviceSize sourceOffset_{};
  VkDeviceSize pendingBytes_{};
};

} // namespace container::gpu
