#include "Container/renderer/resources/CommandBufferManager.h"

#include <stdexcept>

namespace container::renderer {

CommandBufferManager::CommandBufferManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    uint32_t graphicsQueueFamily)
    : device_(std::move(device))
    , graphicsQueueFamily_(graphicsQueueFamily) {
  pool_ = createPool();
}

CommandBufferManager::~CommandBufferManager() {
  free();
  if (pool_ != VK_NULL_HANDLE) {
    destroyOwnedCommandPool(device_->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
  }
}

VkCommandPool CommandBufferManager::createPool() const {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = graphicsQueueFamily_;

  VkCommandPool commandPool{VK_NULL_HANDLE};
  if (createOwnedCommandPool(device_->device(), &poolInfo, nullptr, &commandPool) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create command pool!");
  }
  return commandPool;
}

void CommandBufferManager::allocateSecondary(size_t imageCount) {
  if (secondaryWorkerCount_ == 0 || secondaryBuffersPerWorker_ == 0 ||
      imageCount == 0) {
    return;
  }

  secondaryPools_.reserve(secondaryWorkerCount_);
  secondaryBuffersByWorker_.resize(secondaryWorkerCount_);
  const uint32_t buffersPerWorker =
      static_cast<uint32_t>(imageCount) * secondaryBuffersPerWorker_;

  for (uint32_t workerIndex = 0; workerIndex < secondaryWorkerCount_;
       ++workerIndex) {
    secondaryPools_.push_back(createPool());

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = secondaryPools_.back();
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocInfo.commandBufferCount = buffersPerWorker;

    auto& workerBuffers = secondaryBuffersByWorker_[workerIndex];
    workerBuffers.resize(buffersPerWorker);
    if (vkAllocateCommandBuffers(device_->device(), &allocInfo,
                                 workerBuffers.data()) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate secondary command buffers!");
    }
  }
}

void CommandBufferManager::freeSecondary() {
  for (size_t workerIndex = 0; workerIndex < secondaryPools_.size();
       ++workerIndex) {
    const VkCommandPool workerPool = secondaryPools_[workerIndex];
    auto& workerBuffers = secondaryBuffersByWorker_[workerIndex];
    if (workerPool != VK_NULL_HANDLE && !workerBuffers.empty()) {
      vkFreeCommandBuffers(device_->device(), workerPool,
                           static_cast<uint32_t>(workerBuffers.size()),
                           workerBuffers.data());
      workerBuffers.clear();
    }
    if (workerPool != VK_NULL_HANDLE) {
      destroyOwnedCommandPool(device_->device(), workerPool, nullptr);
    }
  }
  secondaryPools_.clear();
  secondaryBuffersByWorker_.clear();
}

void CommandBufferManager::allocate(size_t imageCount) {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool        = pool_;
  allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = static_cast<uint32_t>(imageCount);

  buffers_.resize(imageCount);
  if (vkAllocateCommandBuffers(device_->device(), &allocInfo,
                               buffers_.data()) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate command buffers!");
  }
  allocateSecondary(imageCount);
}

void CommandBufferManager::free() {
  freeSecondary();
  if (!buffers_.empty()) {
    vkFreeCommandBuffers(device_->device(), pool_,
                         static_cast<uint32_t>(buffers_.size()),
                         buffers_.data());
    buffers_.clear();
  }
}

void CommandBufferManager::reallocate(size_t imageCount) {
  free();
  allocate(imageCount);
}

void CommandBufferManager::configureSecondaryBuffers(
    uint32_t workerCount,
    uint32_t buffersPerWorker) {
  freeSecondary();
  secondaryWorkerCount_ = workerCount;
  secondaryBuffersPerWorker_ = buffersPerWorker;
  if (!buffers_.empty()) {
    allocateSecondary(buffers_.size());
  }
}

VkCommandBuffer CommandBufferManager::secondaryBuffer(
    size_t imageIndex,
    uint32_t workerIndex,
    uint32_t slotIndex) const {
  if (workerIndex >= secondaryBuffersByWorker_.size() ||
      slotIndex >= secondaryBuffersPerWorker_ ||
      imageIndex >= buffers_.size()) {
    return VK_NULL_HANDLE;
  }
  const size_t bufferIndex =
      imageIndex * secondaryBuffersPerWorker_ + slotIndex;
  const auto& workerBuffers = secondaryBuffersByWorker_[workerIndex];
  return bufferIndex < workerBuffers.size() ? workerBuffers[bufferIndex]
                                            : VK_NULL_HANDLE;
}

}  // namespace container::renderer
