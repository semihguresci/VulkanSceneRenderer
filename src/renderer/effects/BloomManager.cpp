#include "Container/renderer/effects/BloomManager.h"
#include "Container/renderer/lighting/SubmittedUploadWait.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vulkan/vulkan_raii.hpp>

namespace container::renderer {

using container::gpu::BloomDownsamplePushConstants;
using container::gpu::BloomUpsamplePushConstants;

BloomManager::BloomManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&              pipelineManager,
    VkCommandPool                                  commandPool)
    : device_(std::move(device)),
      allocationManager_(allocationManager),
      pipelineManager_(pipelineManager),
      commandPool_(commandPool) {}

BloomManager::~BloomManager() {
  destroy();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BloomManager::createResources(const std::filesystem::path& shaderDir) {
  createPipelines(shaderDir);

  // Create linear clamp sampler for bloom texture sampling.
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter    = VK_FILTER_LINEAR;
  si.minFilter    = VK_FILTER_LINEAR;
  si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (createOwnedSampler(device_->device(), &si, nullptr, &linearSampler_) != VK_SUCCESS)
    throw std::runtime_error("failed to create bloom sampler");
}

void BloomManager::createMip(MipLevel& mip) {
  const VkImageCreateInfo imageInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R16G16B16A16_SFLOAT,
      .extent = {mip.width, mip.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VmaAllocationCreateInfo allocationInfo{};
  allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  const auto allocator = allocationManager_.memoryManager()->allocator();
  if (vmaCreateImage(allocator, &imageInfo, &allocationInfo, &mip.image,
                     &mip.allocation, nullptr) != VK_SUCCESS) {
    throw std::runtime_error("failed to create bloom mip image");
  }
  const VkImageViewCreateInfo viewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = mip.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = imageInfo.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  if (createVulkanImageView(device_->device(), &viewInfo, nullptr,
                           &mip.view) != VK_SUCCESS) {
    throw std::runtime_error("failed to create bloom mip view");
  }
}

void BloomManager::createTextures(uint32_t width, uint32_t height,
                                  uint32_t imageCount) {
  std::vector<VkExtent2D> mipExtents;
  for (uint32_t w = width / 2, h = height / 2;
       w >= 2 && h >= 2 && mipExtents.size() < kMaxBloomMips;
       w /= 2, h /= 2) {
    mipExtents.push_back({w, h});
  }
  if (mipExtents.empty() || imageCount == 0) {
    destroyOwnedDescriptorPool(device_->device(),
                               std::exchange(descriptorPool_, VK_NULL_HANDLE), nullptr);
    destroyTextures(frames_);
    resultViews_.clear();
    return;
  }

  const uint32_t downsampleCount = static_cast<uint32_t>(mipExtents.size());
  const uint32_t upsampleCount = downsampleCount - 1;
  const uint32_t setsPerImage = downsampleCount + upsampleCount;
  const uint32_t sampledPerImage = downsampleCount + upsampleCount * 2;
  if (imageCount > std::numeric_limits<uint32_t>::max() / sampledPerImage)
    throw std::length_error("bloom descriptor count is too large");

  std::vector<FrameTextures> frames(imageCount);
  std::vector<VkImageView> resultViews(imageCount);
  const auto deviceHandle = device_->device();
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  try {
    const auto& device = device_->raii();
    const std::array<VkDescriptorPoolSize, 3> poolSizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampledPerImage * imageCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, setsPerImage * imageCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setsPerImage * imageCount}}};
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = setsPerImage * imageCount,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data()};
    if (createOwnedDescriptorPool(deviceHandle, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
      descriptorPool = VK_NULL_HANDLE;
      throw std::runtime_error("failed to create bloom descriptor pool");
    }

    std::vector<VkDescriptorSetLayout> layouts(downsampleCount, downsampleSetLayout_);
    layouts.resize(setsPerImage, upsampleSetLayout_);
    const VkDescriptorSetAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = setsPerImage,
        .pSetLayouts = layouts.data()};
    std::vector<VkDescriptorSet> sets(setsPerImage);
    std::vector<VkImageMemoryBarrier2> transitions;
    transitions.reserve(size_t(imageCount) * setsPerImage);

    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
      auto& frame = frames[imageIndex];
      frame.downsample.resize(downsampleCount);
      frame.upsample.resize(upsampleCount);
      const auto makeChain = [&](std::vector<MipLevel>& chain) {
        for (size_t level = 0; level < chain.size(); ++level) {
          auto& mip = chain[level];
          mip.width = mipExtents[level].width;
          mip.height = mipExtents[level].height;
          createMip(mip);
          transitions.push_back({
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = mip.image,
              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
        }
      };
      makeChain(frame.downsample);
      makeChain(frame.upsample);
      resultViews[imageIndex] = frame.upsample.empty() ? frame.downsample.front().view
                                                      : frame.upsample.front().view;

      if (vkAllocateDescriptorSets(deviceHandle, &allocateInfo, sets.data()) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate bloom descriptor sets");
      frame.downsampleSets.assign(sets.begin(), sets.begin() + downsampleCount);
      frame.upsampleSets.assign(sets.begin() + downsampleCount, sets.end());
    }

    // Retire the single initialization submission before exposing any result
    // view to the frame descriptor sets.
    auto commands = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo(
        commandPool_, vk::CommandBufferLevel::ePrimary, 1));
    auto& command = commands.front();
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32_t>(transitions.size()),
        .pImageMemoryBarriers = transitions.data()};
    vkCmdPipelineBarrier2(*command, &dependency);
    command.end();
    const vk::raii::Fence fence(device, vk::FenceCreateInfo{});
    const vk::raii::Queue queue(device, device_->graphicsQueue());
    const vk::CommandBufferSubmitInfo commandInfo(*command);
    vk::SubmitInfo2 submit{};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;
    queue.submit2(submit, *fence);
    detail::waitForSubmittedUpload(
        [&] {
          if (device.waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
            throw std::runtime_error("bloom image initialization fence wait failed");
        },
        [&] { queue.waitIdle(); });
  } catch (...) {
    destroyOwnedDescriptorPool(deviceHandle, descriptorPool, nullptr);
    destroyTextures(frames);
    throw;
  }

  // Retain the old generation if preparing the replacement failed.
  destroyOwnedDescriptorPool(deviceHandle,
                             std::exchange(descriptorPool_, descriptorPool), nullptr);
  frames_.swap(frames);
  resultViews_.swap(resultViews);
  destroyTextures(frames);
}

void BloomManager::dispatch(VkCommandBuffer cmd, uint32_t imageIndex,
                            VkImageView sceneColorView, uint32_t sceneWidth,
                            uint32_t sceneHeight, VkImageLayout sourceLayout) const {
  if (!enabled_ || !isReady() || imageIndex >= frames_.size())
    return;

  const auto& frame = frames_[imageIndex];
  const uint32_t mipCount = static_cast<uint32_t>(frame.downsample.size());
  VkDevice dev = device_->device();

  // ---- Downsample chain ----
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsamplePipeline_);

  for (uint32_t i = 0; i < mipCount; ++i) {
    // Source: scene color (i==0) or previous mip
    VkDescriptorImageInfo srcInfo{};
    srcInfo.imageView   = (i == 0) ? sceneColorView : frame.downsample[i - 1].view;
    srcInfo.imageLayout = (i == 0) ? sourceLayout : VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo sampInfo{};
    sampInfo.sampler = linearSampler_;

    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageView   = frame.downsample[i].view;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = frame.downsampleSets[i]; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo      = &srcInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = frame.downsampleSets[i]; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo      = &sampInfo;

    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = frame.downsampleSets[i]; w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[2].pImageInfo      = &dstInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsamplePipelineLayout_,
                            0, 1, &frame.downsampleSets[i], 0, nullptr);

    uint32_t srcW = (i == 0) ? sceneWidth : frame.downsample[i - 1].width;
    uint32_t srcH = (i == 0) ? sceneHeight : frame.downsample[i - 1].height;

    BloomDownsamplePushConstants pc{};
    pc.srcWidth  = srcW;
    pc.srcHeight = srcH;
    pc.dstWidth  = frame.downsample[i].width;
    pc.dstHeight = frame.downsample[i].height;
    pc.threshold = threshold_;
    pc.knee      = knee_;
    pc.mipLevel  = i;

    vkCmdPushConstants(cmd, downsamplePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(BloomDownsamplePushConstants), &pc);

    uint32_t dispatchX = (frame.downsample[i].width + 7) / 8;
    uint32_t dispatchY = (frame.downsample[i].height + 7) / 8;
    vkCmdDispatch(cmd, dispatchX, dispatchY, 1);

    // Barrier: downsample write → next downsample read (or upsample read).
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = frame.downsample[i].image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    const VkPipelineStageFlags nextStages =
        mipCount == 1 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                      : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, nextStages,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // ---- Upsample chain ----
  if (frame.upsample.empty()) return;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upsamplePipeline_);

  for (uint32_t i = mipCount - 1; i >= 1; --i) {
    uint32_t srcIdx = i;       // lower-res mip being upsampled
    uint32_t dstIdx = i - 1;   // higher-res mip to blend into
    uint32_t setIdx = mipCount - 1 - i;  // upsample set index

    VkDescriptorImageInfo srcInfo{};
    srcInfo.imageView   = srcIdx < frame.upsample.size()
                              ? frame.upsample[srcIdx].view
                              : frame.downsample[srcIdx].view;
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo sampInfo{};
    sampInfo.sampler = linearSampler_;

    VkDescriptorImageInfo dstReadInfo{};
    dstReadInfo.imageView   = frame.downsample[dstIdx].view;
    dstReadInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo dstWriteInfo{};
    dstWriteInfo.imageView   = frame.upsample[dstIdx].view;
    dstWriteInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 4> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = frame.upsampleSets[setIdx]; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo      = &srcInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = frame.upsampleSets[setIdx]; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo      = &sampInfo;

    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = frame.upsampleSets[setIdx]; w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[2].pImageInfo      = &dstReadInfo;

    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[3].dstSet = frame.upsampleSets[setIdx]; w[3].dstBinding = 3;
    w[3].descriptorCount = 1;
    w[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[3].pImageInfo      = &dstWriteInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upsamplePipelineLayout_,
                            0, 1, &frame.upsampleSets[setIdx], 0, nullptr);

    BloomUpsamplePushConstants pc{};
    pc.srcWidth       = frame.downsample[srcIdx].width;
    pc.srcHeight      = frame.downsample[srcIdx].height;
    pc.dstWidth       = frame.downsample[dstIdx].width;
    pc.dstHeight      = frame.downsample[dstIdx].height;
    pc.filterRadius   = filterRadius_;
    pc.bloomIntensity = intensity_;
    pc.isFinalPass    = (dstIdx == 0) ? 1 : 0;

    vkCmdPushConstants(cmd, upsamplePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(BloomUpsamplePushConstants), &pc);

    uint32_t dispatchX = (frame.downsample[dstIdx].width + 7) / 8;
    uint32_t dispatchY = (frame.downsample[dstIdx].height + 7) / 8;
    vkCmdDispatch(cmd, dispatchX, dispatchY, 1);

    // Barrier: upsample write → next upsample read.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = frame.upsample[dstIdx].image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

void BloomManager::destroy() {
  VkDevice dev = device_->device();
  destroyOwnedDescriptorPool(dev, std::exchange(descriptorPool_, VK_NULL_HANDLE), nullptr);
  destroyTextures(frames_);
  resultViews_.clear();

  if (linearSampler_ != VK_NULL_HANDLE) {
    destroyOwnedSampler(dev, linearSampler_, nullptr);
    linearSampler_ = VK_NULL_HANDLE;
  }
  // Pipelines, layouts, and set layouts are managed by PipelineManager.
  downsamplePipeline_.reset();
  downsamplePipelineLayout_ = VK_NULL_HANDLE;
  downsampleSetLayout_ = VK_NULL_HANDLE;
  upsamplePipeline_.reset();
  upsamplePipelineLayout_ = VK_NULL_HANDLE;
  upsampleSetLayout_ = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void BloomManager::createPipelines(const std::filesystem::path& shaderDir) {
  // ---- Downsample pipeline ----
  {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    downsampleSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size       = sizeof(BloomDownsamplePushConstants);

    downsamplePipelineLayout_ = pipelineManager_.createPipelineLayout(
        {downsampleSetLayout_}, {pcRange});

    std::filesystem::path spvPath = shaderDir / "spv_shaders" / "bloom_downsample.comp.spv";
    downsamplePipeline_ = pipelineManager_.createComputePipeline(
        spvPath, downsamplePipelineLayout_, "bloom_downsample");
  }

  // ---- Upsample pipeline ----
  {
    const std::array<VkDescriptorSetLayoutBinding, 4> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    upsampleSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size       = sizeof(BloomUpsamplePushConstants);

    upsamplePipelineLayout_ = pipelineManager_.createPipelineLayout(
        {upsampleSetLayout_}, {pcRange});

    std::filesystem::path spvPath = shaderDir / "spv_shaders" / "bloom_upsample.comp.spv";
    upsamplePipeline_ = pipelineManager_.createComputePipeline(
        spvPath, upsamplePipelineLayout_, "bloom_upsample");
  }
}

void BloomManager::destroyTextures(std::vector<FrameTextures>& frames) noexcept {
  if (frames.empty()) return;

  const auto device = device_->device();
  const auto allocator = allocationManager_.memoryManager()->allocator();
  const auto destroyChain = [&](const std::vector<MipLevel>& chain) {
    for (const auto& mip : chain) {
      if (mip.view != VK_NULL_HANDLE)
        destroyVulkanImageView(device, mip.view, nullptr);
      if (mip.image != VK_NULL_HANDLE)
        vmaDestroyImage(allocator, mip.image, mip.allocation);
    }
  };
  for (const auto& frame : frames) {
    destroyChain(frame.downsample);
    destroyChain(frame.upsample);
  }
  frames.clear();
}

}  // namespace container::renderer
