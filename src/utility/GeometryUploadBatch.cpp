#include "Container/utility/GeometryUploadBatch.h"

#include "GeometryUploadPlan.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace container::gpu {
namespace {

void checkUpload(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed (VkResult " +
                             std::to_string(result) + ")");
}

void makeGeometryVisible(VkCommandBuffer command, VkBuffer buffer,
                         VkDeviceSize offset, VkDeviceSize size,
                         VkBufferUsageFlags usage) {
  VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
    barrier.dstStageMask |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    barrier.dstAccessMask |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
  }
  if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
    barrier.dstStageMask |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    barrier.dstAccessMask |= VK_ACCESS_2_INDEX_READ_BIT;
  }
  if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) {
    barrier.dstStageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.dstAccessMask |= VK_ACCESS_2_TRANSFER_READ_BIT;
  }
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer;
  barrier.offset = offset;
  barrier.size = size;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.bufferMemoryBarrierCount = 1;
  dependency.pBufferMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(command, &dependency);
}

} // namespace

GeometryUploadResult::GeometryUploadResult(
    VulkanMemoryManager& memory, std::vector<GeometryUploadTarget>&& targets) noexcept
    : memory_(&memory), targets_(std::move(targets)) {}

GeometryUploadResult::~GeometryUploadResult() {
  clear();
}

GeometryUploadResult::GeometryUploadResult(GeometryUploadResult&& other) noexcept
    : memory_(std::exchange(other.memory_, nullptr)), targets_(std::move(other.targets_)) {}

GeometryUploadResult& GeometryUploadResult::operator=(GeometryUploadResult&& other) noexcept {
  if (this != &other) {
    clear();
    memory_ = std::exchange(other.memory_, nullptr);
    targets_ = std::move(other.targets_);
  }
  return *this;
}

AllocatedBuffer GeometryUploadResult::release(size_t index) {
  return std::exchange(targets_.at(index).buffer, {});
}

void GeometryUploadResult::clear() noexcept {
  if (memory_) {
    for (auto& target : targets_)
      memory_->destroyBuffer(target.buffer);
    targets_.clear();
  }
}

GeometryUploadBatch::GeometryUploadBatch(VulkanMemoryManager& memory, VkDevice device,
                                         VkQueue graphicsQueue, uint32_t graphicsFamily) noexcept
    : memory_(memory), device_(device), graphicsQueue_(graphicsQueue), graphicsFamily_(graphicsFamily) {}

GeometryUploadBatch::~GeometryUploadBatch() {
  drain();
}

void GeometryUploadBatch::begin(std::span<const GeometryUploadSource> sources,
                                VkDeviceSize chunkCap) {
  if (state_ != GeometryUploadState::Idle)
    throw std::logic_error("Geometry upload batch is already in use");
  const auto plan = detail::planGeometryUpload(sources, chunkCap);
  if (plan.totalBytes && (!device_ || !graphicsQueue_ || !memory_.allocator()))
    throw std::invalid_argument("Geometry upload requires an initialized graphics queue and allocator");

  try {
    targets_.resize(sources.size());
    if (plan.totalBytes == 0) {
      state_ = GeometryUploadState::Ready;
      return;
    }
    sources_.assign(sources.begin(), sources.end());
    for (size_t i = 0; i < sources.size(); ++i) {
      const auto& source = sources[i];
      if (source.bytes.empty())
        continue;
      auto& target = targets_[i];
      target.logicalSize = source.bytes.size();
      target.buffer = memory_.createBuffer(
          detail::paddedGeometrySize(target.logicalSize),
          source.usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
          VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
    }
    staging_ = std::make_unique<StagingBuffer>(memory_, plan.stagingBytes);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = graphicsFamily_;
    checkUpload(createOwnedCommandPool(device_, &pool, nullptr, &commandPool_),
                "Creating geometry upload command pool");

    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = commandPool_;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    checkUpload(vkAllocateCommandBuffers(device_, &allocation, &commandBuffer_),
                "Allocating geometry upload commands");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    checkUpload(createOwnedFence(device_, &fence, nullptr, &fence_),
                "Creating geometry upload fence");

    progress_.totalBytes = plan.totalBytes;
    progress_.stagingBytes = staging_->buffer().allocation_info.size;
    state_ = GeometryUploadState::Uploading;
    skipFinishedTargets();
  } catch (...) {
    clear();
    throw;
  }
}

VkDeviceSize GeometryUploadBatch::tick(VkDeviceSize budget) {
  try {
    if (pendingBytes_) {
      const auto status = vkGetFenceStatus(device_, fence_);
      if (status == VK_NOT_READY)
        return 0;
      if (status == VK_ERROR_DEVICE_LOST)
        pendingBytes_ = 0;
      checkUpload(status, "Polling geometry upload fence");
      const auto completed = std::exchange(pendingBytes_, 0);
      if (state_ == GeometryUploadState::Retiring) {
        clear();
        return 0;
      }
      progress_.completedBytes += completed;
      targetOffset_ += completed;
      skipFinishedTargets();
      if (targetIndex_ == sources_.size()) {
        sources_.clear();
        releaseSubmission();
        state_ = GeometryUploadState::Ready;
      }
    }
    if (state_ != GeometryUploadState::Uploading)
      return 0;

    const auto& target = targets_[targetIndex_];
    const auto& source = sources_[targetIndex_];
    const auto chunk = detail::planGeometryUploadChunk(
        target.logicalSize, targetOffset_, staging_->size(), budget);
    if (!chunk.copyBytes)
      return 0;
    auto* mapped = static_cast<std::byte*>(staging_->data());
    const auto logicalSize = static_cast<size_t>(chunk.logicalBytes);
    std::memcpy(mapped, source.bytes.data() + static_cast<size_t>(targetOffset_), logicalSize);
    std::memset(mapped + logicalSize, 0, static_cast<size_t>(chunk.copyBytes - chunk.logicalBytes));
    checkUpload(vmaFlushAllocation(memory_.allocator(), staging_->buffer().allocation,
                                   0, chunk.copyBytes),
                "Flushing geometry upload staging");
    checkUpload(vkResetCommandPool(device_, commandPool_, 0),
                "Resetting geometry upload command pool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkUpload(vkBeginCommandBuffer(commandBuffer_, &begin),
                "Beginning geometry upload commands");
    const VkBufferCopy copy{0, targetOffset_, chunk.copyBytes};
    vkCmdCopyBuffer(commandBuffer_, staging_->buffer().buffer, target.buffer.buffer, 1, &copy);
    makeGeometryVisible(commandBuffer_, target.buffer.buffer, copy.dstOffset, copy.size, source.usage);
    checkUpload(vkEndCommandBuffer(commandBuffer_), "Ending geometry upload commands");
    checkUpload(vkResetFences(device_, 1, &fence_), "Resetting geometry upload fence");
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commandBuffer_;
    checkUpload(vkQueueSubmit(graphicsQueue_, 1, &submission, fence_),
                "Submitting geometry upload commands");
    pendingBytes_ = chunk.logicalBytes;
    progress_.submittedBytes += chunk.copyBytes;
    return chunk.copyBytes;
  } catch (...) {
    cancel();
    throw;
  }
}

GeometryUploadResult GeometryUploadBatch::takeReady() {
  if (!ready())
    throw std::logic_error("Geometry upload batch has not completed");
  GeometryUploadResult result(memory_, std::move(targets_));
  clear();
  return result;
}

void GeometryUploadBatch::cancel() noexcept {
  sources_.clear();
  if (pendingBytes_)
    state_ = GeometryUploadState::Retiring;
  else
    clear();
}

void GeometryUploadBatch::drain() noexcept {
  cancel();
  while (pendingBytes_) {
    const auto status = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    if (status == VK_SUCCESS || status == VK_ERROR_DEVICE_LOST) {
      pendingBytes_ = 0;
      break;
    }
    // An unsuccessful wait other than device loss does not retire references.
    std::this_thread::yield();
  }
  clear();
}

void GeometryUploadBatch::releaseSubmission() noexcept {
  if (fence_)
    destroyOwnedFence(device_, std::exchange(fence_, VK_NULL_HANDLE), nullptr);
  if (commandPool_)
    destroyOwnedCommandPool(device_, std::exchange(commandPool_, VK_NULL_HANDLE), nullptr);
  commandBuffer_ = VK_NULL_HANDLE;
  staging_.reset();
  progress_.stagingBytes = 0;
}

void GeometryUploadBatch::clear() noexcept {
  releaseSubmission();
  for (auto& target : targets_)
    memory_.destroyBuffer(target.buffer);
  targets_.clear();
  sources_.clear();
  targetIndex_ = 0;
  targetOffset_ = 0;
  progress_ = {};
  state_ = GeometryUploadState::Idle;
}

void GeometryUploadBatch::skipFinishedTargets() noexcept {
  while (targetIndex_ < targets_.size() && targetOffset_ == targets_[targetIndex_].logicalSize) {
    ++targetIndex_;
    targetOffset_ = 0;
  }
}

} // namespace container::gpu
