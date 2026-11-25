#include "Container/utility/TextureMipUpload.h"

#include "Container/utility/AllocationManager.h"
#include "Container/utility/TextureUploadPlan.h"
#include "TextureMipUploadPlan.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace container::gpu {
namespace {

void checkTextureUpload(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed (VkResult " +
                             std::to_string(result) + ")");
}

void transitionTail(VkCommandBuffer command, VkImage image, uint32_t levels,
                    VkImageLayout before, VkImageLayout after,
                    VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                    VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = sourceStage;
  barrier.srcAccessMask = sourceAccess;
  barrier.dstStageMask = destinationStage;
  barrier.dstAccessMask = destinationAccess;
  barrier.oldLayout = before;
  barrier.newLayout = after;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(command, &dependency);
}

VkImageCreateInfo tailImageInfo(const material::TextureImageData& source, uint32_t firstMip,
                                std::span<const uint32_t> queueFamilies,
                                uint32_t graphicsFamily) {
  if (firstMip >= source.mips.size() || source.mips.size() > 32)
    throw std::invalid_argument("Texture mip tail starts outside the source image");
  if (!queueFamilies.empty()) {
    if (queueFamilies.size() < 2 || std::ranges::find(queueFamilies, graphicsFamily) == queueFamilies.end())
      throw std::invalid_argument("Concurrent texture mip upload requires the graphics and reader families");
    for (size_t i = 0; i < queueFamilies.size(); ++i) {
      const auto previous = queueFamilies.first(i);
      if (std::ranges::find(previous, queueFamilies[i]) != previous.end())
        throw std::invalid_argument("Concurrent texture mip upload queue families must be distinct");
    }
  }

  const auto& base = source.mips[firstMip];
  const auto mipCount = static_cast<uint32_t>(source.mips.size()) - firstMip;
  if (!base.width || !base.height || source.format == VK_FORMAT_UNDEFINED ||
      mipCount > static_cast<uint32_t>(std::bit_width(std::max(base.width, base.height))))
    throw std::invalid_argument("Texture mip tail has invalid dimensions or format");
  VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = source.format;
  image.extent = {base.width, base.height, 1};
  image.mipLevels = mipCount;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image.sharingMode = queueFamilies.empty() ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
  image.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size());
  image.pQueueFamilyIndices = queueFamilies.data();
  return image;
}

} // namespace

UploadedTextureMipTail::~UploadedTextureMipTail() {
  if (view_)
    destroyVulkanImageView(device_, view_, nullptr);
  if (image_)
    vmaDestroyImage(allocator_, image_, allocation_);
}

TextureMipUpload::TextureMipUpload(AllocationManager& allocations,
                                   uint32_t graphicsFamily) noexcept
    : allocations_(allocations), graphicsFamily_(graphicsFamily) {}

TextureMipUpload::~TextureMipUpload() {
  drain();
}

VkDeviceSize TextureMipUpload::estimateResidentBytes(
    const material::TextureImageData& source, uint32_t firstMip,
    std::span<const uint32_t> queueFamilies) const {
  if (!allocations_.device_)
    throw std::invalid_argument("Texture mip estimate requires an initialized device");
  return allocations_.textureAllocationBytes(tailImageInfo(source, firstMip, queueFamilies, graphicsFamily_));
}

void TextureMipUpload::begin(std::shared_ptr<const material::TextureImageData> source,
                             uint32_t firstMip, VkDeviceSize chunkCap,
                             std::span<const uint32_t> queueFamilies) {
  if (state_ != TextureMipUploadState::Idle)
    throw std::logic_error("Texture mip upload is already in use");
  if (!source || !allocations_.memoryManager_ ||
      !allocations_.device_ || !allocations_.graphicsQueue_)
    throw std::invalid_argument("Texture mip upload requires a source level and initialized graphics queue");
  const auto image = tailImageInfo(*source, firstMip, queueFamilies, graphicsFamily_);
  const auto block = material::TextureBlockShape(source->format);
  if (chunkCap < block.bytes)
    throw std::invalid_argument("Texture upload cap is smaller than one texel block");
  const auto options = allocations_.textureDecodeOptions(false);
  if (std::ranges::find(options.allowedFormats, source->format) == options.allowedFormats.end())
    throw std::invalid_argument("Texture mip source format is unavailable on the logical device");
  (void)PlanTextureImageUpload(*source, options.maxDimension, false, VK_WHOLE_SIZE);
  const auto sourceLevels = std::span(source->mips).subspan(firstMip);
  VkDeviceSize totalBytes = 0;
  VkDeviceSize largestMip = 0;
  for (const auto& mip : sourceLevels) {
    totalBytes += mip.bytes.size();
    largestMip = std::max(largestMip, VkDeviceSize(mip.bytes.size()));
  }

  const auto residentBytes = allocations_.textureAllocationBytes(image);
  auto& memory = *allocations_.memoryManager_;
  const auto snapshot = allocations_.assetBudget_.snapshot();
  const auto available = snapshot.limits.uploadBytes
      ? snapshot.limits.uploadBytes - snapshot.used.uploadBytes : VK_WHOLE_SIZE;
  auto stagingBytes = std::max<VkDeviceSize>(block.bytes, std::min({chunkCap, largestMip, available}));
  auto stagingRequirement = memory.bufferAllocationBytes(stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  while (stagingRequirement > available && stagingBytes > block.bytes) {
    stagingBytes = std::max<VkDeviceSize>(block.bytes, stagingBytes / 2);
    stagingRequirement = memory.bufferAllocationBytes(stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  }
  allocations_.assetBudget_.check({residentBytes, stagingRequirement});

  try {
    target_.reset(new UploadedTextureMipTail);
    target_->device_ = allocations_.device_;
    target_->allocator_ = memory.allocator();
    target_->format_ = source->format;
    target_->extent_ = {image.extent.width, image.extent.height};
    target_->firstMip_ = firstMip;
    target_->mipCount_ = image.mipLevels;
    target_->budget_ = allocations_.assetBudget_.reserve({residentBytes, 0});
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationInfo allocated{};
    checkTextureUpload(vmaCreateImage(memory.allocator(), &image, &allocation,
                                     &target_->image_, &target_->allocation_, &allocated),
                       "Allocating detached texture mip tail");
    target_->residentBytes_ = allocated.size;
    if (allocated.size != residentBytes) {
      target_->budget_.reset();
      allocations_.assetBudget_.check({allocated.size, stagingRequirement});
      target_->budget_ = allocations_.assetBudget_.reserve({allocated.size, 0});
    }

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = target_->image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.components = source->componentMapping;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mipLevels, 0, 1};
    checkTextureUpload(createVulkanImageView(allocations_.device_, &view, nullptr, &target_->view_),
                       "Creating detached texture mip view");

    staging_ = std::make_unique<StagingBuffer>(memory, stagingBytes);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = graphicsFamily_;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    checkTextureUpload(createOwnedCommandPool(allocations_.device_, &pool, nullptr, &pool_),
                       "Creating texture upload command pool");
    VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commands.commandPool = pool_;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    checkTextureUpload(vkAllocateCommandBuffers(allocations_.device_, &commands, &command_),
                       "Allocating texture upload command");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    checkTextureUpload(createOwnedFence(allocations_.device_, &fence, nullptr, &fence_),
                       "Creating texture upload fence");

    block_ = block;
    source_ = std::move(source);
    sourceMip_ = firstMip;
    progress_.stagingBytes = staging_->buffer().allocation_info.size;
    progress_.totalBytes = totalBytes;
    state_ = TextureMipUploadState::Uploading;
  } catch (...) {
    clear();
    throw;
  }
}

VkDeviceSize TextureMipUpload::tick(VkDeviceSize budget) {
  const auto device = allocations_.device_;
  try {
    if (pendingBytes_) {
      const auto status = vkGetFenceStatus(device, fence_);
      if (status == VK_NOT_READY)
        return 0;
      if (status == VK_ERROR_DEVICE_LOST)
        pendingBytes_ = 0;
      checkTextureUpload(status, "Polling texture mip upload fence");
      const auto completed = std::exchange(pendingBytes_, 0);
      if (state_ == TextureMipUploadState::Retiring) {
        clear();
        return 0;
      }
      progress_.completedBytes += completed;
      sourceOffset_ += completed;
      if (sourceOffset_ == source_->mips[sourceMip_].bytes.size()) {
        sourceOffset_ = 0;
        ++sourceMip_;
      }
      if (sourceMip_ == source_->mips.size()) {
        source_.reset();
        releaseSubmission();
        state_ = TextureMipUploadState::Ready;
      }
    }
    if (state_ != TextureMipUploadState::Uploading)
      return 0;

    const auto& level = source_->mips[sourceMip_];
    const auto tile = detail::planTextureMipTile(
        block_, {level.width, level.height}, static_cast<uint32_t>(sourceMip_) - target_->firstMip_,
        sourceOffset_, std::min(staging_->size(), budget));
    if (!tile.byteCount)
      return 0;
    staging_->upload(std::span(level.bytes).subspan(static_cast<size_t>(sourceOffset_),
                                                   static_cast<size_t>(tile.byteCount)));
    checkTextureUpload(vkResetCommandPool(device, pool_, 0), "Resetting texture upload command pool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkTextureUpload(vkBeginCommandBuffer(command_, &begin), "Beginning texture upload command");
    if (progress_.submittedBytes == 0)
      transitionTail(command_, target_->image_, target_->mipCount_,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(command_, staging_->buffer().buffer, target_->image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tile.copy);
    if (progress_.submittedBytes + tile.byteCount == progress_.totalBytes)
      transitionTail(command_, target_->image_, target_->mipCount_,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    checkTextureUpload(vkEndCommandBuffer(command_), "Ending texture upload command");
    checkTextureUpload(vkResetFences(device, 1, &fence_), "Resetting texture upload fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    checkTextureUpload(vkQueueSubmit(allocations_.graphicsQueue_, 1, &submit, fence_),
                       "Submitting texture mip upload");
    pendingBytes_ = tile.byteCount;
    progress_.submittedBytes += tile.byteCount;
    return tile.byteCount;
  } catch (...) {
    cancel();
    throw;
  }
}

std::unique_ptr<UploadedTextureMipTail> TextureMipUpload::takeReady() {
  if (!ready())
    throw std::logic_error("Texture mip upload has not completed");
  auto result = std::move(target_);
  clear();
  return result;
}

void TextureMipUpload::cancel() noexcept {
  source_.reset();
  if (pendingBytes_)
    state_ = TextureMipUploadState::Retiring;
  else
    clear();
}

void TextureMipUpload::drain() noexcept {
  cancel();
  while (pendingBytes_) {
    const auto status = vkWaitForFences(allocations_.device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    if (status == VK_SUCCESS || status == VK_ERROR_DEVICE_LOST) {
      pendingBytes_ = 0;
      break;
    }
    // An unsuccessful wait other than device loss does not retire references.
    std::this_thread::yield();
  }
  clear();
}

void TextureMipUpload::releaseSubmission() noexcept {
  if (fence_)
    destroyOwnedFence(allocations_.device_, std::exchange(fence_, VK_NULL_HANDLE), nullptr);
  if (pool_)
    destroyOwnedCommandPool(allocations_.device_, std::exchange(pool_, VK_NULL_HANDLE), nullptr);
  command_ = VK_NULL_HANDLE;
  staging_.reset();
  progress_.stagingBytes = 0;
}

void TextureMipUpload::clear() noexcept {
  releaseSubmission();
  target_.reset();
  source_.reset();
  sourceMip_ = 0;
  sourceOffset_ = 0;
  progress_ = {};
  state_ = TextureMipUploadState::Idle;
}

} // namespace container::gpu
