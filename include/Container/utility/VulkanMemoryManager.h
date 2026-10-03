#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "Container/common/CommonVMA.h"
#include "Container/common/CommonVulkan.h"

namespace container::gpu {

class VulkanMemoryManager;

struct AllocatedBuffer {
  VkBuffer buffer{VK_NULL_HANDLE};
  VmaAllocation allocation{nullptr};
  VmaAllocationInfo allocation_info{};
};

struct BufferSlice {
  VkBuffer buffer{VK_NULL_HANDLE};
  VkDeviceSize offset{0};
  VkDeviceSize size{0};
};

class StagingBuffer {
 public:
  StagingBuffer(VulkanMemoryManager& manager, VkDeviceSize size,
                VmaAllocationCreateFlags allocation_flags =
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT);
  ~StagingBuffer();

  StagingBuffer(const StagingBuffer&) = delete;
  StagingBuffer& operator=(const StagingBuffer&) = delete;
  StagingBuffer(StagingBuffer&& other) noexcept;
  StagingBuffer& operator=(StagingBuffer&& other) noexcept;

  [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
  [[nodiscard]] const AllocatedBuffer& buffer() const noexcept { return buffer_; }

  [[nodiscard]] void* data();
  void upload(std::span<const std::byte> bytes);

 private:
  VulkanMemoryManager* manager_{nullptr};
  AllocatedBuffer buffer_{};
  VkDeviceSize size_{0};
  void* mapped_data_{nullptr};
  bool mapped_here_{false};
};

class VulkanMemoryManager {
 public:
  VulkanMemoryManager(VkInstance instance, VkPhysicalDevice physical_device,
                      VkDevice device,
                      uint32_t vulkan_api_version = VK_API_VERSION_1_4);
  ~VulkanMemoryManager();

  VulkanMemoryManager(const VulkanMemoryManager&) = delete;
  VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;
  VulkanMemoryManager(VulkanMemoryManager&& other) noexcept;
  VulkanMemoryManager& operator=(VulkanMemoryManager&& other) noexcept;

  [[nodiscard]] AllocatedBuffer createBuffer(
      VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
      VmaAllocationCreateFlags allocation_flags = 0,
      VkSharingMode sharing_mode = VK_SHARING_MODE_EXCLUSIVE);

  void destroyBuffer(AllocatedBuffer& buffer);

  [[nodiscard]] VmaAllocator allocator() const noexcept { return allocator_; }

 private:
  void cleanup();

  VmaAllocator allocator_{nullptr};
  VkDevice device_{VK_NULL_HANDLE};
};

class BufferArena {
 public:
  BufferArena(VulkanMemoryManager& manager, VkDeviceSize total_size,
              VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
              VmaAllocationCreateFlags allocation_flags = 0,
              VkSharingMode sharing_mode = VK_SHARING_MODE_EXCLUSIVE);
  ~BufferArena();

  BufferArena(const BufferArena&) = delete;
  BufferArena& operator=(const BufferArena&) = delete;
  BufferArena(BufferArena&& other) noexcept;
  BufferArena& operator=(BufferArena&& other) noexcept;

  [[nodiscard]] BufferSlice allocate(VkDeviceSize size,
                                     VkDeviceSize alignment = 16);
  void reset();

  [[nodiscard]] const AllocatedBuffer& backingBuffer() const noexcept { return buffer_; }
  [[nodiscard]] VkDeviceSize totalSize() const noexcept { return total_size_; }
  [[nodiscard]] VkDeviceSize remainingSize() const noexcept {
    return total_size_ - next_offset_;
  }

 private:
  VulkanMemoryManager* manager_{nullptr};
  AllocatedBuffer buffer_{};
  VkDeviceSize total_size_{0};
  VkDeviceSize next_offset_{0};
};

}  // namespace container::gpu

