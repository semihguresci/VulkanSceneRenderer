#pragma once

#include "Container/utility/VulkanMemoryManager.h"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace container::gpu {

struct GeometryUploadSource {
  std::span<const std::byte> bytes{};
  VkBufferUsageFlags usage{};
};

struct GeometryUploadTarget {
  AllocatedBuffer buffer{};
  VkDeviceSize logicalSize{};
};

enum class GeometryUploadState {
  Idle,
  Uploading,
  Ready,
  Retiring,
};

struct GeometryUploadProgress {
  // Source byte counts exclude padding on the last copy of each target.
  VkDeviceSize totalBytes{};
  VkDeviceSize completedBytes{};
  // Physical bytes submitted and currently allocated for staging.
  VkDeviceSize submittedBytes{};
  VkDeviceSize stagingBytes{};
};

// Owns detached upload targets. Buffers bound by the caller must be released
// to that caller's GPU lifetime owner before this result goes out of scope.
class GeometryUploadResult {
 public:
  ~GeometryUploadResult();
  GeometryUploadResult(GeometryUploadResult&& other) noexcept;
  GeometryUploadResult& operator=(GeometryUploadResult&& other) noexcept;
  GeometryUploadResult(const GeometryUploadResult&) = delete;
  GeometryUploadResult& operator=(const GeometryUploadResult&) = delete;

  [[nodiscard]] std::span<const GeometryUploadTarget> targets() const noexcept {
    return targets_;
  }
  [[nodiscard]] AllocatedBuffer release(size_t index);

 private:
  friend class GeometryUploadBatch;
  GeometryUploadResult(VulkanMemoryManager& memory,
                       std::vector<GeometryUploadTarget>&& targets) noexcept;
  void clear() noexcept;

  VulkanMemoryManager* memory_;
  std::vector<GeometryUploadTarget> targets_;
};

// Called on the render thread that submits to graphicsQueue. The memory
// manager and device must outlive the batch and its results.
class GeometryUploadBatch {
 public:
  GeometryUploadBatch(VulkanMemoryManager& memory, VkDevice device,
                      VkQueue graphicsQueue, uint32_t graphicsFamily) noexcept;
  ~GeometryUploadBatch();
  GeometryUploadBatch(const GeometryUploadBatch&) = delete;
  GeometryUploadBatch& operator=(const GeometryUploadBatch&) = delete;

  // Exact budget charge while the detached targets and shared staging are live.
  [[nodiscard]] SceneAssetBytes requiredAssetBytes(
      std::span<const GeometryUploadSource> sources, VkDeviceSize chunkCap) const;

  // Borrows immutable source bytes through the last chunk submission or
  // cancel(). Empty sources preserve their result slots without allocating.
  void begin(std::span<const GeometryUploadSource> sources, VkDeviceSize chunkCap);
  // Polls retirement and submits up to one chunk, returning actual copy bytes.
  [[nodiscard]] VkDeviceSize tick(VkDeviceSize budget = VK_WHOLE_SIZE);
  [[nodiscard]] GeometryUploadState state() const noexcept { return state_; }
  [[nodiscard]] bool ready() const noexcept { return state_ == GeometryUploadState::Ready; }
  [[nodiscard]] GeometryUploadProgress progress() const noexcept { return progress_; }
  [[nodiscard]] GeometryUploadResult takeReady();

  // Drops all source views immediately. Keep ticking until a pending fence
  // retires, or call drain() during teardown.
  void cancel() noexcept;
  void drain() noexcept;

 private:
  void releaseSubmission() noexcept;
  void clear() noexcept;
  void skipFinishedTargets() noexcept;

  VulkanMemoryManager& memory_;
  VkDevice device_;
  VkQueue graphicsQueue_;
  uint32_t graphicsFamily_;
  std::vector<GeometryUploadSource> sources_;
  std::vector<GeometryUploadTarget> targets_;
  std::unique_ptr<StagingBuffer> staging_;
  VkCommandPool commandPool_{VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
  VkFence fence_{VK_NULL_HANDLE};
  GeometryUploadState state_{GeometryUploadState::Idle};
  GeometryUploadProgress progress_{};
  size_t targetIndex_{};
  VkDeviceSize targetOffset_{};
  VkDeviceSize pendingBytes_{};
};

} // namespace container::gpu
