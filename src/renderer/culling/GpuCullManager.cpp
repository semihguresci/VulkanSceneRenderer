#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/culling/GpuCullDrawUploadPlanner.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace container::renderer {

using container::gpu::CullPushConstants;
using container::gpu::GpuDrawIndexedIndirectCommand;
using container::gpu::HiZPushConstants;

namespace {

void destroyBufferIfAllocated(
    container::gpu::AllocationManager& allocationManager,
    container::gpu::AllocatedBuffer& buffer) {
  if (buffer.buffer == VK_NULL_HANDLE) return;
  allocationManager.destroyBuffer(buffer);
  buffer = {};
}

void destroyBuffers(
    container::gpu::AllocationManager& allocationManager,
    std::vector<container::gpu::AllocatedBuffer>& buffers) {
  for (auto& buffer : buffers) {
    destroyBufferIfAllocated(allocationManager, buffer);
  }
  buffers.clear();
}

bool bufferReadyAt(
    const std::vector<container::gpu::AllocatedBuffer>& buffers,
    uint32_t imageIndex) {
  return imageIndex < buffers.size() &&
         buffers[imageIndex].buffer != VK_NULL_HANDLE;
}

} // namespace

GpuCullManager::GpuCullManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&            pipelineManager)
    : device_(std::move(device))
    , allocationManager_(allocationManager)
    , pipelineManager_(pipelineManager) {
}

GpuCullManager::~GpuCullManager() {
  destroyCullBuffers();
  destroyFrozenCameraBuffers();

  pipelineManager_.destroyPipeline(frustumCullPipeline_);
  pipelineManager_.destroyPipelineLayout(frustumCullPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(frustumCullPool_);
  pipelineManager_.destroyDescriptorSetLayout(frustumCullSetLayout_);

  destroyHiZImage();
  if (hizSampler_ != VK_NULL_HANDLE)
    destroyOwnedSampler(device_->device(), hizSampler_, nullptr);
  pipelineManager_.destroyPipeline(hizPipeline_);
  pipelineManager_.destroyPipelineLayout(hizPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(hizPool_);
  pipelineManager_.destroyDescriptorSetLayout(hizSetLayout_);

  pipelineManager_.destroyPipeline(occlusionCullPipeline_);
  pipelineManager_.destroyPipelineLayout(occlusionCullPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(occlusionCullPool_);
  pipelineManager_.destroyDescriptorSetLayout(occlusionCullSetLayout_);
}

bool GpuCullManager::isReady() const {
  return frustumCullPipeline_ != VK_NULL_HANDLE &&
         device_->enabledFeatures().drawIndirectFirstInstance == VK_TRUE &&
         device_->enabledFeatures().multiDrawIndirect == VK_TRUE &&
         device_->enabledVulkan12Features().drawIndirectCount == VK_TRUE;
}

bool GpuCullManager::occlusionCullResourcesReady(uint32_t imageIndex) const {
  if (imageIndex >= hizFrames_.size()) return false;
  const auto& hizFrame = hizFrames_[imageIndex];
  return isReady() &&
         occlusionCullPipeline_ != VK_NULL_HANDLE &&
         imageIndex < occlusionCullSets_.size() &&
         occlusionCullSets_[imageIndex] != VK_NULL_HANDLE &&
         bufferReadyAt(occlusionIndirectBuffers_, imageIndex) &&
         bufferReadyAt(occlusionCountBuffers_, imageIndex) &&
         bufferReadyAt(drawCountBuffers_, imageIndex) &&
         bufferReadyAt(indirectDrawBuffers_, imageIndex) &&
         hizFrame.fullView != VK_NULL_HANDLE &&
         hizSampler_ != VK_NULL_HANDLE &&
         !hizFrame.mipViews.empty() &&
         hizFrame.descriptorSets.size() == hizMipLevels_;
}

bool GpuCullManager::canRecordOcclusionCull(uint32_t imageIndex) const {
  return occlusionCullResourcesReady(imageIndex) &&
         frustumDrawsValid(imageIndex) &&
         hizGeneratedThisFrame(imageIndex);
}

void GpuCullManager::beginFrameCulling(uint32_t imageIndex) {
  if (imageIndex < frustumDrawsValid_.size()) {
    frustumDrawsValid_[imageIndex] = false;
  }
  if (imageIndex < hizFrames_.size()) {
    hizFrames_[imageIndex].generatedThisFrame = false;
  }
  if (imageIndex < occlusionDrawsValid_.size()) {
    occlusionDrawsValid_[imageIndex] = false;
  }
}

// ---------------------------------------------------------------------------
// Resource creation
// ---------------------------------------------------------------------------

void GpuCullManager::createResources(const std::filesystem::path& shaderDir,
                                     uint32_t descriptorSetCount) {
  createFrustumCullPipeline(shaderDir);
  createHiZPipeline(shaderDir);
  createOcclusionCullPipeline(shaderDir);
  recreatePerFrameResources(descriptorSetCount);
}

void GpuCullManager::destroyCullBuffers() {
  destroyBuffers(allocationManager_, inputDrawBuffers_);
  destroyBuffers(allocationManager_, indirectDrawBuffers_);
  destroyBuffers(allocationManager_, drawCountBuffers_);
  destroyBuffers(allocationManager_, occlusionIndirectBuffers_);
  destroyBuffers(allocationManager_, occlusionCountBuffers_);
  destroyBuffers(allocationManager_, statsReadbackBuffers_);
}

void GpuCullManager::destroyFrozenCameraBuffers() {
  destroyBuffers(allocationManager_, frozenCameraBuffers_);
}

void GpuCullManager::resizePerImageState(uint32_t imageCount) {
  const uint32_t count = std::max<uint32_t>(1u, imageCount);
  objectSsboBuffers_.assign(count, VK_NULL_HANDLE);
  objectSsboSizes_.assign(count, 0);
  lastUploadSourceData_.assign(count, nullptr);
  lastUploadSourceSize_.assign(count, 0u);
  lastUploadSourceRevision_.assign(count, 0u);
  frustumDrawsValid_.assign(count, false);
  occlusionDrawsValid_.assign(count, false);
  statsReadbackSubmitted_.assign(count, false);
}

void GpuCullManager::recreatePerFrameResources(uint32_t descriptorSetCount) {
  const uint32_t setCount = std::max<uint32_t>(1u, descriptorSetCount);
  const std::vector<VkBuffer> previousObjectBuffers = objectSsboBuffers_;
  const std::vector<VkDeviceSize> previousObjectSizes = objectSsboSizes_;
  const uint32_t previousCapacity = maxObjectCount_;
  if (frustumCullSetLayout_ != VK_NULL_HANDLE) {
    allocateFrustumCullDescriptorSets(setCount);
  }
  if (occlusionCullSetLayout_ != VK_NULL_HANDLE) {
    allocateOcclusionCullDescriptorSets(setCount);
  }
  destroyHiZImage();
  destroyCullBuffers();
  maxObjectCount_ = 0u;
  resizePerImageState(setCount);
  if (previousCapacity > 0u) {
    static_cast<void>(ensureBufferCapacity(previousCapacity));
  }
  for (uint32_t imageIndex = 0; imageIndex < setCount; ++imageIndex) {
    if (previousObjectBuffers.empty() || previousObjectSizes.empty()) break;
    const uint32_t sourceIndex = std::min<uint32_t>(
        imageIndex, static_cast<uint32_t>(previousObjectBuffers.size() - 1u));
    if (sourceIndex >= previousObjectSizes.size()) continue;
    updateObjectSsboDescriptor(imageIndex, previousObjectBuffers[sourceIndex],
                               previousObjectSizes[sourceIndex]);
  }
}

bool GpuCullManager::ensureBufferCapacity(uint32_t maxObjectCount) {
  const uint32_t imageCount = std::max<uint32_t>(
      1u, static_cast<uint32_t>(std::max(frustumCullSets_.size(),
                                         occlusionCullSets_.size())));
  const bool buffersReady =
      bufferReadyAt(inputDrawBuffers_, imageCount - 1u) &&
      bufferReadyAt(indirectDrawBuffers_, imageCount - 1u) &&
      bufferReadyAt(drawCountBuffers_, imageCount - 1u) &&
      bufferReadyAt(occlusionIndirectBuffers_, imageCount - 1u) &&
      bufferReadyAt(occlusionCountBuffers_, imageCount - 1u) &&
      bufferReadyAt(statsReadbackBuffers_, imageCount - 1u);
  if (maxObjectCount <= maxObjectCount_ && buffersReady) {
    return false;
  }

  const uint32_t capacity = std::max(maxObjectCount, 64u);
  destroyCullBuffers();
  inputDrawBuffers_.assign(imageCount, {});
  indirectDrawBuffers_.assign(imageCount, {});
  drawCountBuffers_.assign(imageCount, {});
  occlusionIndirectBuffers_.assign(imageCount, {});
  occlusionCountBuffers_.assign(imageCount, {});
  statsReadbackBuffers_.assign(imageCount, {});

  for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
    // Input draw command SSBO (CPU-writable).
    inputDrawBuffers_[imageIndex] = allocationManager_.createBuffer(
        sizeof(GpuDrawIndexedIndirectCommand) * capacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);

    // Output indirect draw buffer (GPU-only, also usable as indirect source).
    indirectDrawBuffers_[imageIndex] = allocationManager_.createBuffer(
        sizeof(GpuDrawIndexedIndirectCommand) * capacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

    // Draw count buffer (single uint32, GPU-only + indirect).
    drawCountBuffers_[imageIndex] = allocationManager_.createBuffer(
        sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

    // Occlusion-culled output (second pass for G-Buffer).
    occlusionIndirectBuffers_[imageIndex] = allocationManager_.createBuffer(
        sizeof(GpuDrawIndexedIndirectCommand) * capacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

    occlusionCountBuffers_[imageIndex] = allocationManager_.createBuffer(
        sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

    // Stats readback buffer: 2 x uint32_t (frustum count + occlusion count).
    statsReadbackBuffers_[imageIndex] = allocationManager_.createBuffer(
        sizeof(uint32_t) * 2,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }

  maxObjectCount_ = capacity;
  resizePerImageState(imageCount);

  writeDescriptorSets();
  return true;
}

// ---------------------------------------------------------------------------
// Upload draw commands
// ---------------------------------------------------------------------------

void GpuCullManager::uploadDrawCommands(
    uint32_t imageIndex,
    const std::vector<DrawCommand>& commands,
    uint64_t sourceRevision) {
  const GpuCullDrawUploadCacheView uploadCache{
      .sourceData = std::span<const DrawCommand *const>(
          lastUploadSourceData_.data(), lastUploadSourceData_.size()),
      .sourceSizes = std::span<const size_t>(
          lastUploadSourceSize_.data(), lastUploadSourceSize_.size()),
      .sourceRevisions = std::span<const uint64_t>(
          lastUploadSourceRevision_.data(), lastUploadSourceRevision_.size())};
  const GpuCullDrawUploadPlan uploadPlan = buildGpuCullDrawUploadPlan(
      {.inputBufferReady = bufferReadyAt(inputDrawBuffers_, imageIndex),
       .cache = uploadCache,
       .imageIndex = imageIndex,
       .sourceData = commands.data(),
       .sourceSize = commands.size(),
       .sourceRevision = sourceRevision,
       .maxObjectCount = maxObjectCount_});
  if (!uploadPlan.updatesInputCount()) {
    return;
  }

  const uint32_t count = uploadPlan.drawCount;
  lastStats_.totalInputCount = count;
  if (!uploadPlan.uploadsBuffer()) {
    return;
  }

  // Convert DrawCommand to GpuDrawIndexedIndirectCommand.
  // objectIndex is encoded in firstInstance so vertex shaders can read it.
  uploadScratch_.resize(count);
  auto& gpuCmds = uploadScratch_;
  for (uint32_t i = 0; i < count; ++i) {
    gpuCmds[i].indexCount    = commands[i].indexCount;
    gpuCmds[i].instanceCount =
        std::max(commands[i].instanceCount, 1u);
    gpuCmds[i].firstIndex    = commands[i].firstIndex;
    gpuCmds[i].vertexOffset  = 0;
    gpuCmds[i].firstInstance = commands[i].objectIndex;
  }

  SceneController::writeToBuffer(allocationManager_,
                                 inputDrawBuffers_[imageIndex],
                                 gpuCmds.data(),
                                 sizeof(GpuDrawIndexedIndirectCommand) * count);

  lastStats_.totalInputCount = count;
  lastUploadSourceData_[imageIndex] = commands.data();
  lastUploadSourceSize_[imageIndex] = commands.size();
  lastUploadSourceRevision_[imageIndex] = sourceRevision;
}

// ---------------------------------------------------------------------------
// Frustum cull dispatch
// ---------------------------------------------------------------------------

void GpuCullManager::dispatchFrustumCull(VkCommandBuffer cmd,
                                          uint32_t imageIndex,
                                          VkBuffer cameraBuffer,
                                          VkDeviceSize cameraBufferSize,
                                          uint32_t objectCount) {
  if (imageIndex >= frustumCullSets_.size() ||
      imageIndex >= frustumDrawsValid_.size()) return;
  frustumDrawsValid_[imageIndex] = false;
  const VkDescriptorSet frustumCullSet = frustumCullSets_[imageIndex];
  if (frustumCullPipeline_ == VK_NULL_HANDLE ||
      !bufferReadyAt(indirectDrawBuffers_, imageIndex) ||
      !bufferReadyAt(drawCountBuffers_, imageIndex) ||
      frustumCullSet == VK_NULL_HANDLE ||
      objectCount == 0) return;
  const auto& drawCountBuffer = drawCountBuffers_[imageIndex];

  // Zero the draw count buffer.
  vkCmdFillBuffer(cmd, drawCountBuffer.buffer, 0, sizeof(uint32_t), 0);

  VkMemoryBarrier fillBarrier{};
  fillBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

  // Update camera buffer descriptor for this frame.
  // When culling is frozen, use the snapshot buffer instead of the live one.
  {
    const VkBuffer frozenCamera = frozenCameraBuffer(imageIndex);
    const VkBuffer activeCam =
        frozenCamera != VK_NULL_HANDLE ? frozenCamera : cameraBuffer;
    VkDescriptorBufferInfo camInfo{activeCam, 0, cameraBufferSize};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = frustumCullSet;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo     = &camInfo;
    vkUpdateDescriptorSets(device_->device(), 1, &w, 0, nullptr);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, frustumCullPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          frustumCullPipelineLayout_, 0, 1,
                          &frustumCullSet, 0, nullptr);

  CullPushConstants pc{};
  pc.objectCount = objectCount;
  vkCmdPushConstants(cmd, frustumCullPipelineLayout_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(CullPushConstants), &pc);

  const uint32_t groupCount = (objectCount + 63) / 64;
  vkCmdDispatch(cmd, groupCount, 1, 1);

  // Barrier: compute writes → indirect draw reads + occlusion cull reads.
  VkMemoryBarrier barrier{};
  barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                          VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);
  frustumDrawsValid_[imageIndex] = true;
}

// ---------------------------------------------------------------------------
// Hi-Z generation
// ---------------------------------------------------------------------------

void GpuCullManager::createHiZSampler() {
  if (hizSampler_ != VK_NULL_HANDLE) {
    destroyOwnedSampler(device_->device(), hizSampler_, nullptr);
    hizSampler_ = VK_NULL_HANDLE;
  }

  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = VK_FILTER_NEAREST;
  si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.minLod = 0.0f;
  si.maxLod = static_cast<float>(hizMipLevels_ - 1u);
  if (createOwnedSampler(device_->device(), &si, nullptr, &hizSampler_) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create Hi-Z sampler");
  }
}

void GpuCullManager::ensureHiZImage(uint32_t imageIndex, uint32_t width,
                                    uint32_t height) {
  if (width == 0 || height == 0) return;

  const uint32_t imageCount = std::max<uint32_t>(
      imageIndex + 1u,
      std::max<uint32_t>(
          1u, static_cast<uint32_t>(std::max(frustumCullSets_.size(),
                                             occlusionCullSets_.size()))));
  const bool needsRecreate =
      width != hizWidth_ || height != hizHeight_ ||
      hizFrames_.size() != imageCount || hizMipLevels_ == 0u;
  if (!needsRecreate && imageIndex < hizFrames_.size() &&
      hizFrames_[imageIndex].image != VK_NULL_HANDLE) {
    return;
  }

  destroyHiZImage();

  hizWidth_ = width;
  hizHeight_ = height;
  hizMipLevels_ = static_cast<uint32_t>(
      std::floor(std::log2(static_cast<float>(std::max(width, height))))) + 1;
  hizFrames_.assign(imageCount, {});

  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R32_SFLOAT;
  ci.extent = {width, height, 1};
  ci.mipLevels = hizMipLevels_;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  for (uint32_t frameIndex = 0; frameIndex < imageCount; ++frameIndex) {
    auto& hizFrame = hizFrames_[frameIndex];
    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(), &ci,
                       &ai, &hizFrame.image, &hizFrame.allocation,
                       nullptr) != VK_SUCCESS) {
      throw std::runtime_error("failed to create Hi-Z image");
    }

    // Full-mip view for sampling in occlusion cull.
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = hizFrame.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMipLevels_, 0, 1};
    if (createVulkanImageView(device_->device(), &vi, nullptr,
                          &hizFrame.fullView) != VK_SUCCESS) {
      throw std::runtime_error("failed to create Hi-Z full image view");
    }

    // Per-mip views for storage writes.
    hizFrame.mipViews.resize(hizMipLevels_);
    for (uint32_t m = 0; m < hizMipLevels_; ++m) {
      VkImageViewCreateInfo mvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      mvi.image = hizFrame.image;
      mvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      mvi.format = VK_FORMAT_R32_SFLOAT;
      mvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
      if (createVulkanImageView(device_->device(), &mvi, nullptr,
                            &hizFrame.mipViews[m]) != VK_SUCCESS) {
        throw std::runtime_error("failed to create Hi-Z mip view " +
                                 std::to_string(m));
      }
    }
  }

  createHiZSampler();
  createHiZDescriptorSets();
}

void GpuCullManager::destroyHiZImage() {
  VkDevice dev = device_->device();
  pipelineManager_.destroyDescriptorPool(hizPool_);

  for (auto& hizFrame : hizFrames_) {
    hizFrame.descriptorSets.clear();
    for (auto view : hizFrame.mipViews) {
      if (view != VK_NULL_HANDLE) destroyVulkanImageView(dev, view, nullptr);
    }
    hizFrame.mipViews.clear();
    if (hizFrame.fullView != VK_NULL_HANDLE) {
      destroyVulkanImageView(dev, hizFrame.fullView, nullptr);
      hizFrame.fullView = VK_NULL_HANDLE;
    }
    if (hizFrame.image != VK_NULL_HANDLE) {
      vmaDestroyImage(allocationManager_.memoryManager()->allocator(),
                      hizFrame.image, hizFrame.allocation);
      hizFrame.image = VK_NULL_HANDLE;
      hizFrame.allocation = nullptr;
    }
    hizFrame.initialized = false;
    hizFrame.generatedThisFrame = false;
  }
  hizFrames_.clear();
  hizWidth_ = 0;
  hizHeight_ = 0;
  hizMipLevels_ = 0;
}

void GpuCullManager::dispatchHiZGenerate(VkCommandBuffer cmd,
                                          uint32_t imageIndex,
                                          VkImageView depthView,
                                          VkSampler depthSampler,
                                          uint32_t width, uint32_t height) {
  if (imageIndex >= hizFrames_.size()) return;
  auto& hizFrame = hizFrames_[imageIndex];
  hizFrame.generatedThisFrame = false;
  if (width == 0 || height == 0) return;

  if (hizPipeline_ == VK_NULL_HANDLE || hizFrame.image == VK_NULL_HANDLE ||
      hizFrame.mipViews.empty() ||
      hizFrame.descriptorSets.size() != hizMipLevels_ ||
      depthView == VK_NULL_HANDLE || depthSampler == VK_NULL_HANDLE) {
    return;
  }

  // Transition Hi-Z image to GENERAL for storage writes.
  {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = hizFrame.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = hizFrame.initialized
                      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                      : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = hizFrame.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMipLevels_, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         hizFrame.initialized
                             ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizPipeline_);

  uint32_t srcW = width;
  uint32_t srcH = height;

  for (uint32_t mip = 0; mip < hizMipLevels_; ++mip) {
    // Update descriptor set for this mip level.
    VkDescriptorImageInfo srcInfo{};
    srcInfo.imageLayout = (mip == 0)
        ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
        : VK_IMAGE_LAYOUT_GENERAL;
    srcInfo.imageView = (mip == 0) ? depthView : hizFrame.mipViews[mip - 1];

    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = (mip == 0) ? depthSampler : hizSampler_;

    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstInfo.imageView = hizFrame.mipViews[mip];

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = hizFrame.descriptorSets[mip];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &srcInfo;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = hizFrame.descriptorSets[mip];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &samplerInfo;
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = hizFrame.descriptorSets[mip];
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &dstInfo;

    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            hizPipelineLayout_, 0, 1,
                            &hizFrame.descriptorSets[mip], 0, nullptr);

    HiZPushConstants hpc{};
    hpc.srcWidth = srcW;
    hpc.srcHeight = srcH;
    hpc.dstMipLevel = mip;
    vkCmdPushConstants(cmd, hizPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(HiZPushConstants), &hpc);

    const uint32_t dstW =
        (mip == 0) ? srcW : std::max((srcW + 1u) >> 1, 1u);
    const uint32_t dstH =
        (mip == 0) ? srcH : std::max((srcH + 1u) >> 1, 1u);
    vkCmdDispatch(cmd, (dstW + 7) / 8, (dstH + 7) / 8, 1);

    // Barrier between mip levels.
    if (mip + 1 < hizMipLevels_) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.image = hizFrame.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &b);
    }

    srcW = dstW;
    srcH = dstH;
  }

  // Final barrier: Hi-Z image is ready for sampling in occlusion cull.
  {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.image = hizFrame.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMipLevels_, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    hizFrame.initialized = true;
    hizFrame.generatedThisFrame = true;
  }
}
// ---------------------------------------------------------------------------
// Occlusion cull
// ---------------------------------------------------------------------------

void GpuCullManager::dispatchOcclusionCull(VkCommandBuffer cmd,
                                            uint32_t imageIndex,
                                            VkBuffer cameraBuffer,
                                            VkDeviceSize cameraBufferSize,
                                            uint32_t objectCount) {
  if (imageIndex >= occlusionCullSets_.size() ||
      imageIndex >= occlusionDrawsValid_.size()) return;
  occlusionDrawsValid_[imageIndex] = false;
  const VkDescriptorSet occlusionCullSet = occlusionCullSets_[imageIndex];
  if (occlusionCullSet == VK_NULL_HANDLE) return;
  if (!canRecordOcclusionCull(imageIndex) || objectCount == 0) return;
  const auto& indirectDrawBuffer = indirectDrawBuffers_[imageIndex];
  const auto& drawCountBuffer = drawCountBuffers_[imageIndex];
  const auto& occlusionIndirectBuffer = occlusionIndirectBuffers_[imageIndex];
  const auto& occlusionCountBuffer = occlusionCountBuffers_[imageIndex];

  // Zero the occlusion draw count.
  vkCmdFillBuffer(cmd, occlusionCountBuffer.buffer, 0, sizeof(uint32_t), 0);

  VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

  // Update occlusion cull descriptor set.
  // When culling is frozen, use the snapshot buffer instead of the live one.
  {
    const VkBuffer frozenCamera = frozenCameraBuffer(imageIndex);
    const VkBuffer activeCam =
        frozenCamera != VK_NULL_HANDLE ? frozenCamera : cameraBuffer;
    VkDescriptorBufferInfo camInfo{activeCam, 0, cameraBufferSize};

    // Binding 2: input draws from frustum-culled indirect buffer.
    VkDescriptorBufferInfo inputInfo{
        indirectDrawBuffer.buffer, 0,
        sizeof(GpuDrawIndexedIndirectCommand) * maxObjectCount_};
    // Binding 3: output draws (occlusion-culled).
    VkDescriptorBufferInfo outputInfo{
        occlusionIndirectBuffer.buffer, 0,
        sizeof(GpuDrawIndexedIndirectCommand) * maxObjectCount_};
    // Binding 4: occlusion draw count (RW).
    VkDescriptorBufferInfo countInfo{
        occlusionCountBuffer.buffer, 0, sizeof(uint32_t)};
    // Binding 5: Hi-Z pyramid (sampled image).
    VkDescriptorImageInfo hizInfo{};
    hizInfo.imageView   = hizFrames_[imageIndex].fullView;
    hizInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Binding 6: frustum-culled draw count (read-only, from frustum cull pass).
    VkDescriptorBufferInfo frustumCountInfo{
        drawCountBuffer.buffer, 0, sizeof(uint32_t)};
    // Binding 7: Hi-Z sampler.
    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = hizSampler_;

    std::array<VkWriteDescriptorSet, 7> writes{};
    // Binding 0: camera UBO.
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = occlusionCullSet; writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &camInfo;
    // Binding 1: object SSBO — written via updateObjectSsboDescriptor; skip here.
    // Binding 2: input draws (frustum-culled output).
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = occlusionCullSet; writes[1].dstBinding = 2;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo     = &inputInfo;
    // Binding 3: output draws (occlusion-culled).
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = occlusionCullSet; writes[2].dstBinding = 3;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo     = &outputInfo;
    // Binding 4: occlusion draw count.
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[3].dstSet = occlusionCullSet; writes[3].dstBinding = 4;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo     = &countInfo;
    // Binding 5: Hi-Z sampled image.
    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[4].dstSet = occlusionCullSet; writes[4].dstBinding = 5;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[4].pImageInfo      = &hizInfo;
    // Binding 6: frustum-culled draw count (read-only).
    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[5].dstSet = occlusionCullSet; writes[5].dstBinding = 6;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo     = &frustumCountInfo;
    // Binding 7: Hi-Z sampler.
    writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[6].dstSet = occlusionCullSet; writes[6].dstBinding = 7;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[6].pImageInfo      = &samplerInfo;

    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, occlusionCullPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          occlusionCullPipelineLayout_, 0, 1,
                          &occlusionCullSet, 0, nullptr);

  CullPushConstants pc{};
  pc.objectCount = objectCount;
  vkCmdPushConstants(cmd, occlusionCullPipelineLayout_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(CullPushConstants), &pc);

  const uint32_t groupCount = (objectCount + 63) / 64;
  vkCmdDispatch(cmd, groupCount, 1, 1);

  // Barrier: compute writes → indirect draw reads.
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                          VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);
  occlusionDrawsValid_[imageIndex] = true;
}

// ---------------------------------------------------------------------------
// Indirect draw
// ---------------------------------------------------------------------------

void GpuCullManager::drawIndirect(VkCommandBuffer cmd,
                                  uint32_t imageIndex) const {
  if (!bufferReadyAt(indirectDrawBuffers_, imageIndex) ||
      !bufferReadyAt(drawCountBuffers_, imageIndex) ||
      !frustumDrawsValid(imageIndex)) return;

  vkCmdDrawIndexedIndirectCount(
      cmd,
      indirectDrawBuffers_[imageIndex].buffer, 0,
      drawCountBuffers_[imageIndex].buffer, 0,
      maxObjectCount_,
      sizeof(GpuDrawIndexedIndirectCommand));
}

void GpuCullManager::drawIndirectOccluded(VkCommandBuffer cmd,
                                          uint32_t imageIndex) const {
  if (!bufferReadyAt(occlusionIndirectBuffers_, imageIndex) ||
      !bufferReadyAt(occlusionCountBuffers_, imageIndex) ||
      !occlusionDrawsValid(imageIndex)) return;

  vkCmdDrawIndexedIndirectCount(
      cmd,
      occlusionIndirectBuffers_[imageIndex].buffer, 0,
      occlusionCountBuffers_[imageIndex].buffer, 0,
      maxObjectCount_,
      sizeof(GpuDrawIndexedIndirectCommand));
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void GpuCullManager::createFrustumCullPipeline(
    const std::filesystem::path& shaderDir) {
  // Descriptor set layout: binding 0 = camera UBO, 1 = object SSBO,
  // 2 = input draw SSBO, 3 = output draw SSBO, 4 = draw count SSBO.
  {
    const std::array<VkDescriptorSetLayoutBinding, 5> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    frustumCullSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.size       = sizeof(CullPushConstants);

  frustumCullPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {frustumCullSetLayout_}, {pcRange});

  auto compPath = shaderDir / "spv_shaders" / "frustum_cull.comp.spv";
  const auto spvData = container::util::readFile(compPath);
  VkShaderModule compModule =
      container::gpu::createShaderModule(device_->device(), spvData);

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = compModule;
  stage.pName  = "computeMain";

  VkComputePipelineCreateInfo ci{};
  ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  ci.stage  = stage;
  ci.layout = frustumCullPipelineLayout_;

  frustumCullPipeline_ =
      pipelineManager_.createComputePipeline(ci, "frustum_cull");

  destroyOwnedShaderModule(device_->device(), compModule, nullptr);
}

void GpuCullManager::createHiZPipeline(
    const std::filesystem::path& shaderDir) {
  auto compPath = shaderDir / "spv_shaders" / "hiz_generate.comp.spv";
  if (!std::filesystem::exists(compPath)) return;

  {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    hizSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.size       = sizeof(HiZPushConstants);

  hizPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {hizSetLayout_}, {pcRange});

  const auto spvData = container::util::readFile(compPath);
  VkShaderModule compModule =
      container::gpu::createShaderModule(device_->device(), spvData);

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = compModule;
  stage.pName  = "computeMain";

  VkComputePipelineCreateInfo ci{};
  ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  ci.stage  = stage;
  ci.layout = hizPipelineLayout_;

  hizPipeline_ = pipelineManager_.createComputePipeline(ci, "hiz_generate");
  destroyOwnedShaderModule(device_->device(), compModule, nullptr);
}

void GpuCullManager::createHiZDescriptorSets() {
  if (hizSetLayout_ == VK_NULL_HANDLE || hizMipLevels_ == 0 ||
      hizFrames_.empty()) {
    return;
  }

  pipelineManager_.destroyDescriptorPool(hizPool_);
  for (auto& hizFrame : hizFrames_) {
    hizFrame.descriptorSets.clear();
  }

  const uint32_t imageCount = static_cast<uint32_t>(hizFrames_.size());
  const uint32_t totalSetCount =
      hizMipLevels_ * imageCount;
  hizPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, totalSetCount},
       {VK_DESCRIPTOR_TYPE_SAMPLER, totalSetCount},
       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, totalSetCount}},
      totalSetCount, 0);

  std::vector<VkDescriptorSetLayout> layouts(totalSetCount, hizSetLayout_);
  std::vector<VkDescriptorSet> descriptorSets(totalSetCount, VK_NULL_HANDLE);

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = hizPool_;
  ai.descriptorSetCount = totalSetCount;
  ai.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(device_->device(), &ai, descriptorSets.data()) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to allocate Hi-Z descriptor sets");
  }

  uint32_t descriptorIndex = 0u;
  for (auto& hizFrame : hizFrames_) {
    hizFrame.descriptorSets.assign(hizMipLevels_, VK_NULL_HANDLE);
    for (uint32_t mip = 0; mip < hizMipLevels_; ++mip) {
      hizFrame.descriptorSets[mip] = descriptorSets[descriptorIndex++];
    }
  }
}
void GpuCullManager::createOcclusionCullPipeline(
    const std::filesystem::path& shaderDir) {
  auto compPath = shaderDir / "spv_shaders" / "occlusion_cull.comp.spv";
  if (!std::filesystem::exists(compPath)) return;

  {
    const std::array<VkDescriptorSetLayoutBinding, 8> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    occlusionCullSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.size       = sizeof(CullPushConstants);

  occlusionCullPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {occlusionCullSetLayout_}, {pcRange});

  const auto spvData = container::util::readFile(compPath);
  VkShaderModule compModule =
      container::gpu::createShaderModule(device_->device(), spvData);

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = compModule;
  stage.pName  = "computeMain";

  VkComputePipelineCreateInfo ci{};
  ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  ci.stage  = stage;
  ci.layout = occlusionCullPipelineLayout_;

  occlusionCullPipeline_ =
      pipelineManager_.createComputePipeline(ci, "occlusion_cull");
  destroyOwnedShaderModule(device_->device(), compModule, nullptr);
}

void GpuCullManager::allocateFrustumCullDescriptorSets(
    uint32_t descriptorSetCount) {
  const uint32_t setCount = std::max<uint32_t>(1u, descriptorSetCount);
  pipelineManager_.destroyDescriptorPool(frustumCullPool_);
  frustumCullSets_.clear();

  frustumCullPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount},
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * 4}},
      setCount, 0);

  std::vector<VkDescriptorSetLayout> layouts(setCount, frustumCullSetLayout_);
  frustumCullSets_.assign(setCount, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool     = frustumCullPool_;
  ai.descriptorSetCount = setCount;
  ai.pSetLayouts        = layouts.data();
  if (vkAllocateDescriptorSets(device_->device(), &ai,
                               frustumCullSets_.data()) != VK_SUCCESS) {
    frustumCullSets_.clear();
    throw std::runtime_error("failed to allocate frustum cull descriptor sets");
  }
}

void GpuCullManager::allocateOcclusionCullDescriptorSets(
    uint32_t descriptorSetCount) {
  const uint32_t setCount = std::max<uint32_t>(1u, descriptorSetCount);
  pipelineManager_.destroyDescriptorPool(occlusionCullPool_);
  occlusionCullSets_.clear();

  occlusionCullPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount},
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * 5},
       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, setCount},
       {VK_DESCRIPTOR_TYPE_SAMPLER, setCount}},
      setCount, 0);

  std::vector<VkDescriptorSetLayout> layouts(setCount, occlusionCullSetLayout_);
  occlusionCullSets_.assign(setCount, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool     = occlusionCullPool_;
  ai.descriptorSetCount = setCount;
  ai.pSetLayouts        = layouts.data();
  if (vkAllocateDescriptorSets(device_->device(), &ai,
                               occlusionCullSets_.data()) != VK_SUCCESS) {
    occlusionCullSets_.clear();
    throw std::runtime_error("failed to allocate occlusion cull descriptor sets");
  }
}

// ---------------------------------------------------------------------------
// Descriptor writes
// ---------------------------------------------------------------------------

void GpuCullManager::writeDescriptorSets() {
  if (frustumCullSets_.empty()) return;
  // Bindings 1-4: object SSBO, input draw, output draw, draw count.
  // Binding 0 (camera) is updated per-frame in dispatchFrustumCull.
  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(frustumCullSets_.size());
       ++imageIndex) {
    const VkDescriptorSet frustumCullSet = frustumCullSets_[imageIndex];
    if (frustumCullSet == VK_NULL_HANDLE) continue;
    if (!bufferReadyAt(inputDrawBuffers_, imageIndex) ||
        !bufferReadyAt(indirectDrawBuffers_, imageIndex) ||
        !bufferReadyAt(drawCountBuffers_, imageIndex) ||
        maxObjectCount_ == 0) {
      continue;
    }

    VkDescriptorBufferInfo inputInfo{
        inputDrawBuffers_[imageIndex].buffer, 0,
        sizeof(GpuDrawIndexedIndirectCommand) * maxObjectCount_};
    VkDescriptorBufferInfo outputInfo{
        indirectDrawBuffers_[imageIndex].buffer, 0,
        sizeof(GpuDrawIndexedIndirectCommand) * maxObjectCount_};
    VkDescriptorBufferInfo countInfo{
        drawCountBuffers_[imageIndex].buffer, 0, sizeof(uint32_t)};

    std::array<VkWriteDescriptorSet, 3> writes{};
    // Binding 2: input draw commands.
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = frustumCullSet;
    writes[0].dstBinding      = 2;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo     = &inputInfo;
    // Binding 3: output indirect commands.
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = frustumCullSet;
    writes[1].dstBinding      = 3;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo     = &outputInfo;
    // Binding 4: draw count.
    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = frustumCullSet;
    writes[2].dstBinding      = 4;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo     = &countInfo;

    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }
}

void GpuCullManager::updateObjectSsboDescriptor(
    VkBuffer objectBuffer, VkDeviceSize objectBufferSize) {
  for (uint32_t i = 0; i < frustumCullSets_.size(); ++i) {
    updateObjectSsboDescriptor(i, objectBuffer, objectBufferSize);
  }
}

void GpuCullManager::updateObjectSsboDescriptor(
    uint32_t imageIndex,
    VkBuffer objectBuffer,
    VkDeviceSize objectBufferSize) {
  if (imageIndex >= objectSsboBuffers_.size() ||
      imageIndex >= objectSsboSizes_.size()) {
    return;
  }
  if (objectSsboBuffers_[imageIndex] == objectBuffer &&
      objectSsboSizes_[imageIndex] == objectBufferSize) {
    return;
  }
  objectSsboBuffers_[imageIndex] = objectBuffer;
  objectSsboSizes_[imageIndex] = objectBufferSize;

  VkDescriptorBufferInfo objInfo{objectBuffer, 0, objectBufferSize};

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(2);

  if (imageIndex < frustumCullSets_.size() &&
      frustumCullSets_[imageIndex] != VK_NULL_HANDLE) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = frustumCullSets_[imageIndex];
    w.dstBinding      = 1;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo     = &objInfo;
    writes.push_back(w);
  }

  if (imageIndex < occlusionCullSets_.size() &&
      occlusionCullSets_[imageIndex] != VK_NULL_HANDLE) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = occlusionCullSets_[imageIndex];
    w.dstBinding      = 1;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo     = &objInfo;
    writes.push_back(w);
  }

  if (!writes.empty())
    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

// ---------------------------------------------------------------------------
// Stats readback
// ---------------------------------------------------------------------------

void GpuCullManager::scheduleStatsReadback(VkCommandBuffer cmd,
                                           uint32_t imageIndex) {
  if (!bufferReadyAt(statsReadbackBuffers_, imageIndex)) return;
  if (imageIndex < statsReadbackSubmitted_.size()) {
    statsReadbackSubmitted_[imageIndex] = true;
  }
  const auto& statsReadbackBuffer = statsReadbackBuffers_[imageIndex];

  if (!frustumDrawsValid(imageIndex) ||
      !bufferReadyAt(drawCountBuffers_, imageIndex)) {
    vkCmdFillBuffer(cmd, statsReadbackBuffer.buffer, 0,
                    sizeof(uint32_t) * 2, 0);

    VkMemoryBarrier postBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    postBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &postBarrier, 0, nullptr, 0, nullptr);
    return;
  }

  if (!bufferReadyAt(occlusionCountBuffers_, imageIndex)) return;

  // Barrier: ensure all compute writes to count buffers are visible to transfer.
  VkMemoryBarrier preBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  preBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  preBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &preBarrier, 0, nullptr, 0, nullptr);

  // Copy frustum-culled draw count -> statsReadbackBuffer[0].
  VkBufferCopy frustumCopy{};
  frustumCopy.srcOffset = 0;
  frustumCopy.dstOffset = 0;
  frustumCopy.size      = sizeof(uint32_t);
  vkCmdCopyBuffer(cmd, drawCountBuffers_[imageIndex].buffer,
                  statsReadbackBuffer.buffer, 1, &frustumCopy);

  // Copy occlusion-culled draw count -> statsReadbackBuffer[1].
  const VkBuffer occlusionStatsSource =
      occlusionDrawsValid(imageIndex)
          ? occlusionCountBuffers_[imageIndex].buffer
          : drawCountBuffers_[imageIndex].buffer;

  VkBufferCopy occlusionCopy{};
  occlusionCopy.srcOffset = 0;
  occlusionCopy.dstOffset = sizeof(uint32_t);
  occlusionCopy.size      = sizeof(uint32_t);
  vkCmdCopyBuffer(cmd, occlusionStatsSource,
                  statsReadbackBuffer.buffer, 1, &occlusionCopy);

  // Barrier: transfer writes -> host reads (visible after fence).
  VkMemoryBarrier postBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  postBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &postBarrier, 0, nullptr, 0, nullptr);
}

void GpuCullManager::collectStats() {
  bool readAny = false;
  uint32_t frustumPassedCount = 0u;
  uint32_t occlusionPassedCount = 0u;

  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(statsReadbackBuffers_.size());
       ++imageIndex) {
    if (imageIndex < statsReadbackSubmitted_.size() &&
        !statsReadbackSubmitted_[imageIndex]) {
      continue;
    }
    auto& statsReadbackBuffer = statsReadbackBuffers_[imageIndex];
    if (statsReadbackBuffer.buffer == VK_NULL_HANDLE ||
        statsReadbackBuffer.allocation == nullptr) {
      continue;
    }

    VmaAllocationInfo allocInfo{};
    vmaGetAllocationInfo(allocationManager_.memoryManager()->allocator(),
                         statsReadbackBuffer.allocation, &allocInfo);
    if (allocInfo.pMappedData == nullptr) continue;

    vmaInvalidateAllocation(allocationManager_.memoryManager()->allocator(),
                            statsReadbackBuffer.allocation, 0,
                            sizeof(uint32_t) * 2);

    const auto* data = static_cast<const uint32_t*>(allocInfo.pMappedData);
    if (!readAny || data[0] != 0u || data[1] != 0u) {
      frustumPassedCount = data[0];
      occlusionPassedCount = data[1];
    }
    readAny = true;
  }

  if (!readAny) return;
  lastStats_.frustumPassedCount = frustumPassedCount;
  lastStats_.occlusionPassedCount = occlusionPassedCount;
}
// ---------------------------------------------------------------------------
// Freeze-culling
// ---------------------------------------------------------------------------

void GpuCullManager::freezeCulling(uint32_t imageIndex,
                                    VkCommandBuffer cmd,
                                    VkBuffer liveCameraBuffer,
                                    VkDeviceSize cameraBufferSize) {
  if (liveCameraBuffer == VK_NULL_HANDLE || cameraBufferSize == 0) return;

  // (Re)create the frozen camera buffer if needed.
  const uint32_t imageCount = std::max<uint32_t>(
      imageIndex + 1u,
      std::max<uint32_t>(
          1u, static_cast<uint32_t>(std::max(frustumCullSets_.size(),
                                             occlusionCullSets_.size()))));
  if (frozenCameraBuffers_.size() < imageCount) {
    frozenCameraBuffers_.resize(imageCount);
  }

  // Copy the live camera buffer → frozen buffer.
  VkBufferCopy copy{};
  copy.size = cameraBufferSize;
  for (uint32_t bufferIndex = 0; bufferIndex < imageCount; ++bufferIndex) {
    auto& frozenCameraBuffer = frozenCameraBuffers_[bufferIndex];
    if (frozenCameraBuffer.buffer == VK_NULL_HANDLE) {
      frozenCameraBuffer = allocationManager_.createBuffer(
          cameraBufferSize,
          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    }
    vkCmdCopyBuffer(cmd, liveCameraBuffer, frozenCameraBuffer.buffer, 1, &copy);
  }

  // Barrier: transfer → uniform read (for subsequent cull dispatches).
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, 1, &barrier, 0, nullptr, 0, nullptr);

  cullingFrozen_ = true;
}

void GpuCullManager::unfreezeCulling() {
  cullingFrozen_ = false;
}

}  // namespace container::renderer
