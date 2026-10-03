#include <Container/utility/VulkanMemoryManager.h>

#include <cstring>
#include <algorithm>
#include <stdexcept>

#include "Container/utility/VulkanAlignment.h"

namespace container::gpu {

/* ========================= StagingBuffer ========================= */

StagingBuffer::StagingBuffer(VulkanMemoryManager& manager, VkDeviceSize size,
                             VmaAllocationCreateFlags allocation_flags)
    : manager_(&manager), size_(size) {
  buffer_ = manager_->createBuffer(size_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VMA_MEMORY_USAGE_AUTO, allocation_flags);

  mapped_data_ = buffer_.allocation_info.pMappedData;
}

StagingBuffer::~StagingBuffer() {
  if (manager_ && buffer_.allocation) {
    if (mapped_here_ && mapped_data_) {
      vmaUnmapMemory(manager_->allocator(), buffer_.allocation);
    }
    manager_->destroyBuffer(buffer_);
  }
}

StagingBuffer::StagingBuffer(StagingBuffer&& other) noexcept
    : manager_(other.manager_),
      buffer_(other.buffer_),
      size_(other.size_),
      mapped_data_(other.mapped_data_),
      mapped_here_(other.mapped_here_) {
  other.manager_ = nullptr;
  other.buffer_ = {};
  other.size_ = 0;
  other.mapped_data_ = nullptr;
  other.mapped_here_ = false;
}

StagingBuffer& StagingBuffer::operator=(StagingBuffer&& other) noexcept {
  if (this != &other) {
    this->~StagingBuffer();
    manager_ = other.manager_;
    buffer_ = other.buffer_;
    size_ = other.size_;
    mapped_data_ = other.mapped_data_;
    mapped_here_ = other.mapped_here_;

    other.manager_ = nullptr;
    other.buffer_ = {};
    other.size_ = 0;
    other.mapped_data_ = nullptr;
    other.mapped_here_ = false;
  }
  return *this;
}

void* StagingBuffer::data() {
  if (mapped_data_) return mapped_data_;

  if (!manager_ || !buffer_.allocation) {
    throw std::runtime_error("StagingBuffer not initialized");
  }

  if (vmaMapMemory(manager_->allocator(), buffer_.allocation, &mapped_data_) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to map staging buffer");
  }

  mapped_here_ = true;
  return mapped_data_;
}

void StagingBuffer::upload(std::span<const std::byte> bytes) {
  if (bytes.size_bytes() > size_) {
    throw std::runtime_error("StagingBuffer upload exceeds buffer size");
  }

  void* dst = data();
  std::memcpy(dst, bytes.data(), bytes.size_bytes());
  if (vmaFlushAllocation(manager_->allocator(), buffer_.allocation, 0,
                         static_cast<VkDeviceSize>(bytes.size_bytes())) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to flush staging buffer upload");
  }
}

/* ===================== VulkanMemoryManager ====================== */

VulkanMemoryManager::VulkanMemoryManager(VkInstance instance,
                                         VkPhysicalDevice physical_device,
                                         VkDevice device,
                                         uint32_t vulkan_api_version)
    : device_(device) {
  VmaVulkanFunctions vulkanFunctions{};
  vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo info{};
  info.instance = instance;
  info.physicalDevice = physical_device;
  info.device = device;
  // The bundled VMA dispatch table supports core entry points through 1.3.
  // Those entry points are also valid on the required Vulkan 1.4 device.
  info.vulkanApiVersion = std::min(vulkan_api_version, VK_API_VERSION_1_3);
  info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  info.pVulkanFunctions = &vulkanFunctions;

  if (vmaCreateAllocator(&info, &allocator_) != VK_SUCCESS) {
    throw std::runtime_error("Failed to create VMA allocator");
  }
}

VulkanMemoryManager::~VulkanMemoryManager() { cleanup(); }

VulkanMemoryManager::VulkanMemoryManager(VulkanMemoryManager&& other) noexcept
    : allocator_(other.allocator_), device_(other.device_) {
  other.allocator_ = VK_NULL_HANDLE;
  other.device_ = VK_NULL_HANDLE;
}

VulkanMemoryManager& VulkanMemoryManager::operator=(
    VulkanMemoryManager&& other) noexcept {
  if (this != &other) {
    cleanup();
    allocator_ = other.allocator_;
    device_ = other.device_;
    other.allocator_ = VK_NULL_HANDLE;
    other.device_ = VK_NULL_HANDLE;
  }
  return *this;
}

AllocatedBuffer VulkanMemoryManager::createBuffer(
    VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
    VmaAllocationCreateFlags allocation_flags, VkSharingMode sharing_mode) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = sharing_mode;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = memory_usage;
  allocInfo.flags = allocation_flags;

  AllocatedBuffer buffer{};

  if (vmaCreateBuffer(allocator_, &bufferInfo, &allocInfo, &buffer.buffer,
                      &buffer.allocation,
                      &buffer.allocation_info) != VK_SUCCESS) {
    throw std::runtime_error("Failed to allocate Vulkan buffer");
  }

  return buffer;
}

void VulkanMemoryManager::destroyBuffer(AllocatedBuffer& buffer) {
  if (allocator_ && buffer.buffer && buffer.allocation) {
    vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
  }
  buffer = {};
}

void VulkanMemoryManager::cleanup() {
  if (allocator_) {
    vmaDestroyAllocator(allocator_);
    allocator_ = VK_NULL_HANDLE;
  }
  device_ = VK_NULL_HANDLE;
}

/* ========================= BufferArena ========================= */

BufferArena::BufferArena(VulkanMemoryManager& manager, VkDeviceSize total_size,
                         VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
                         VmaAllocationCreateFlags allocation_flags,
                         VkSharingMode sharing_mode)
    : manager_(&manager), total_size_(total_size) {
  buffer_ = manager_->createBuffer(total_size_, usage, memory_usage,
                                   allocation_flags, sharing_mode);
}

BufferArena::~BufferArena() {
  if (manager_ && buffer_.buffer) {
    manager_->destroyBuffer(buffer_);
  }
}

BufferArena::BufferArena(BufferArena&& other) noexcept
    : manager_(other.manager_),
      buffer_(other.buffer_),
      total_size_(other.total_size_),
      next_offset_(other.next_offset_) {
  other.manager_ = nullptr;
  other.buffer_ = {};
  other.total_size_ = 0;
  other.next_offset_ = 0;
}

BufferArena& BufferArena::operator=(BufferArena&& other) noexcept {
  if (this != &other) {
    if (manager_ && buffer_.buffer) {
      manager_->destroyBuffer(buffer_);
    }
    manager_ = other.manager_;
    buffer_ = other.buffer_;
    total_size_ = other.total_size_;
    next_offset_ = other.next_offset_;

    other.manager_ = nullptr;
    other.buffer_ = {};
    other.total_size_ = 0;
    other.next_offset_ = 0;
  }
  return *this;
}

BufferSlice BufferArena::allocate(VkDeviceSize size, VkDeviceSize alignment) {
  if (alignment == 0) alignment = 1;

  VkDeviceSize aligned = VulkanAlignment::alignUp(next_offset_, alignment);

  if (aligned + size > total_size_) {
    throw std::runtime_error("BufferArena out of space");
  }

  BufferSlice slice{buffer_.buffer, aligned, size};
  next_offset_ = aligned + size;
  return slice;
}

void BufferArena::reset() { next_offset_ = 0; }

}  // namespace container::gpu
