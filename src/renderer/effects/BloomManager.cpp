#include "Container/renderer/effects/BloomManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

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

void BloomManager::createTextures(uint32_t width, uint32_t height) {
  destroyTextures();

  // Compute mip chain dimensions starting at half resolution.
  uint32_t w = std::max(width / 2u, 1u);
  uint32_t h = std::max(height / 2u, 1u);
  mipCount_ = 0;

  while (w >= 2 && h >= 2 && mipCount_ < kMaxBloomMips) {
    MipLevel mip{};
    mip.width  = w;
    mip.height = h;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent      = {w, h, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(), &ii, &ai,
                       &mip.image, &mip.allocation, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create bloom mip image");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = mip.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (createVulkanImageView(device_->device(), &vi, nullptr, &mip.view) != VK_SUCCESS)
      throw std::runtime_error("failed to create bloom mip view");

    mips_.push_back(mip);
    mipViews_.push_back(mip.view);
    ++mipCount_;

    w = std::max(w / 2u, 1u);
    h = std::max(h / 2u, 1u);
  }

  for (uint32_t i = 0; i + 1 < mipCount_; ++i) {
    MipLevel mip{};
    mip.width = mips_[i].width;
    mip.height = mips_[i].height;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent      = {mip.width, mip.height, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(), &ii, &ai,
                       &mip.image, &mip.allocation, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create bloom upsample image");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = mip.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (createVulkanImageView(device_->device(), &vi, nullptr, &mip.view) != VK_SUCCESS)
      throw std::runtime_error("failed to create bloom upsample view");

    upsampleMips_.push_back(mip);
    upsampleViews_.push_back(mip.view);
  }

  // Transition all mip images to GENERAL layout.
  {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = commandPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_->device(), &ai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    auto transitionToGeneral = [cmd](const MipLevel& m) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
      barrier.image         = m.image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      vkCmdPipelineBarrier(cmd,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &barrier);
    };

    for (auto& m : mips_) transitionToGeneral(m);
    for (auto& m : upsampleMips_) transitionToGeneral(m);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si2{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si2.commandBufferCount = 1;
    si2.pCommandBuffers    = &cmd;
    vkQueueSubmit(device_->graphicsQueue(), 1, &si2, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_->graphicsQueue());
    vkFreeCommandBuffers(device_->device(), commandPool_, 1, &cmd);
  }

  // Allocate descriptor sets for each mip transition.
  // Downsample: mipCount_ sets (scene→mip0, mip0→mip1, ..., mipN-2→mipN-1)
  // Upsample: mipCount_-1 sets (mipN-1→mipN-2, ..., mip1→mip0)
  VkDevice dev = device_->device();

  if (downsampleDescriptorPool_ != VK_NULL_HANDLE) {
    destroyOwnedDescriptorPool(dev, downsampleDescriptorPool_, nullptr);
    downsampleDescriptorPool_ = VK_NULL_HANDLE;
  }
  if (upsampleDescriptorPool_ != VK_NULL_HANDLE) {
    destroyOwnedDescriptorPool(dev, upsampleDescriptorPool_, nullptr);
    upsampleDescriptorPool_ = VK_NULL_HANDLE;
  }
  downsampleSets_.clear();
  upsampleSets_.clear();

  if (mipCount_ == 0) return;

  // Downsample descriptor pool: mipCount_ sets, each with (sampled_image,
  // sampler, storage_image)
  {
    std::vector<VkDescriptorPoolSize> poolSizes = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  mipCount_},
        {VK_DESCRIPTOR_TYPE_SAMPLER,        mipCount_},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  mipCount_},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets       = mipCount_;
    pi.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pi.pPoolSizes    = poolSizes.data();
    if (createOwnedDescriptorPool(dev, &pi, nullptr, &downsampleDescriptorPool_) != VK_SUCCESS)
      throw std::runtime_error("failed to create bloom downsample descriptor pool");

    std::vector<VkDescriptorSetLayout> layouts(mipCount_, downsampleSetLayout_);
    downsampleSets_.resize(mipCount_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = downsampleDescriptorPool_;
    ai.descriptorSetCount = mipCount_;
    ai.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(dev, &ai, downsampleSets_.data()) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate bloom downsample descriptor sets");
  }

  // Upsample descriptor pool: (mipCount_-1) sets if mipCount_ > 1
  if (mipCount_ > 1) {
    uint32_t upsampleCount = mipCount_ - 1;
    std::vector<VkDescriptorPoolSize> poolSizes = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  upsampleCount * 2},
        {VK_DESCRIPTOR_TYPE_SAMPLER,        upsampleCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  upsampleCount},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets       = upsampleCount;
    pi.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pi.pPoolSizes    = poolSizes.data();
    if (createOwnedDescriptorPool(dev, &pi, nullptr, &upsampleDescriptorPool_) != VK_SUCCESS)
      throw std::runtime_error("failed to create bloom upsample descriptor pool");

    std::vector<VkDescriptorSetLayout> layouts(upsampleCount, upsampleSetLayout_);
    upsampleSets_.resize(upsampleCount);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = upsampleDescriptorPool_;
    ai.descriptorSetCount = upsampleCount;
    ai.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(dev, &ai, upsampleSets_.data()) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate bloom upsample descriptor sets");
  }
}

void BloomManager::dispatch(VkCommandBuffer cmd,
                            VkImageView     sceneColorView,
                            uint32_t        sceneWidth,
                            uint32_t        sceneHeight,
                            VkImageLayout sourceLayout) const {
  if (!enabled_ || downsamplePipeline_ == VK_NULL_HANDLE || mipCount_ == 0)
    return;

  VkDevice dev = device_->device();

  // ---- Downsample chain ----
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsamplePipeline_);

  for (uint32_t i = 0; i < mipCount_; ++i) {
    // Source: scene color (i==0) or previous mip
    VkDescriptorImageInfo srcInfo{};
    srcInfo.imageView   = (i == 0) ? sceneColorView : mipViews_[i - 1];
    srcInfo.imageLayout = (i == 0) ? sourceLayout : VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo sampInfo{};
    sampInfo.sampler = linearSampler_;

    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageView   = mipViews_[i];
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = downsampleSets_[i]; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo      = &srcInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = downsampleSets_[i]; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo      = &sampInfo;

    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = downsampleSets_[i]; w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[2].pImageInfo      = &dstInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsamplePipelineLayout_,
                            0, 1, &downsampleSets_[i], 0, nullptr);

    uint32_t srcW = (i == 0) ? sceneWidth : mips_[i - 1].width;
    uint32_t srcH = (i == 0) ? sceneHeight : mips_[i - 1].height;

    BloomDownsamplePushConstants pc{};
    pc.srcWidth  = srcW;
    pc.srcHeight = srcH;
    pc.dstWidth  = mips_[i].width;
    pc.dstHeight = mips_[i].height;
    pc.threshold = threshold_;
    pc.knee      = knee_;
    pc.mipLevel  = i;

    vkCmdPushConstants(cmd, downsamplePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(BloomDownsamplePushConstants), &pc);

    uint32_t dispatchX = (mips_[i].width + 7) / 8;
    uint32_t dispatchY = (mips_[i].height + 7) / 8;
    vkCmdDispatch(cmd, dispatchX, dispatchY, 1);

    // Barrier: downsample write → next downsample read (or upsample read).
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = mips_[i].image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // ---- Upsample chain ----
  if (mipCount_ < 2 || upsampleViews_.size() != mipCount_ - 1) return;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upsamplePipeline_);

  for (uint32_t i = mipCount_ - 1; i >= 1; --i) {
    uint32_t srcIdx = i;       // lower-res mip being upsampled
    uint32_t dstIdx = i - 1;   // higher-res mip to blend into
    uint32_t setIdx = mipCount_ - 1 - i;  // upsample set index

    VkDescriptorImageInfo srcInfo{};
    srcInfo.imageView   = srcIdx < upsampleViews_.size()
                              ? upsampleViews_[srcIdx]
                              : mipViews_[srcIdx];
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo sampInfo{};
    sampInfo.sampler = linearSampler_;

    VkDescriptorImageInfo dstReadInfo{};
    dstReadInfo.imageView   = mipViews_[dstIdx];
    dstReadInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo dstWriteInfo{};
    dstWriteInfo.imageView   = upsampleViews_[dstIdx];
    dstWriteInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 4> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = upsampleSets_[setIdx]; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo      = &srcInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = upsampleSets_[setIdx]; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo      = &sampInfo;

    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = upsampleSets_[setIdx]; w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[2].pImageInfo      = &dstReadInfo;

    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[3].dstSet = upsampleSets_[setIdx]; w[3].dstBinding = 3;
    w[3].descriptorCount = 1;
    w[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[3].pImageInfo      = &dstWriteInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upsamplePipelineLayout_,
                            0, 1, &upsampleSets_[setIdx], 0, nullptr);

    BloomUpsamplePushConstants pc{};
    pc.srcWidth       = mips_[srcIdx].width;
    pc.srcHeight      = mips_[srcIdx].height;
    pc.dstWidth       = mips_[dstIdx].width;
    pc.dstHeight      = mips_[dstIdx].height;
    pc.filterRadius   = filterRadius_;
    pc.bloomIntensity = intensity_;
    pc.isFinalPass    = (dstIdx == 0) ? 1 : 0;

    vkCmdPushConstants(cmd, upsamplePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(BloomUpsamplePushConstants), &pc);

    uint32_t dispatchX = (mips_[dstIdx].width + 7) / 8;
    uint32_t dispatchY = (mips_[dstIdx].height + 7) / 8;
    vkCmdDispatch(cmd, dispatchX, dispatchY, 1);

    // Barrier: upsample write → next upsample read.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = upsampleMips_[dstIdx].image;
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
  destroyTextures();

  VkDevice dev = device_->device();
  if (linearSampler_ != VK_NULL_HANDLE) {
    destroyOwnedSampler(dev, linearSampler_, nullptr);
    linearSampler_ = VK_NULL_HANDLE;
  }
  // Pipelines, layouts, pools, and set layouts are managed by PipelineManager.
  downsamplePipeline_ = VK_NULL_HANDLE;
  downsamplePipelineLayout_ = VK_NULL_HANDLE;
  downsampleSetLayout_ = VK_NULL_HANDLE;
  upsamplePipeline_ = VK_NULL_HANDLE;
  upsamplePipelineLayout_ = VK_NULL_HANDLE;
  upsampleSetLayout_ = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void BloomManager::createPipelines(const std::filesystem::path& shaderDir) {
  VkDevice dev = device_->device();

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
    const auto spvData = container::util::readFile(spvPath);
    VkShaderModule module = container::gpu::createShaderModule(dev, spvData);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName  = "computeMain";

    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = stage;
    ci.layout = downsamplePipelineLayout_;

    downsamplePipeline_ = pipelineManager_.createComputePipeline(ci, "bloom_downsample");
    destroyOwnedShaderModule(dev, module, nullptr);
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
    const auto spvData = container::util::readFile(spvPath);
    VkShaderModule module = container::gpu::createShaderModule(dev, spvData);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName  = "computeMain";

    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = stage;
    ci.layout = upsamplePipelineLayout_;

    upsamplePipeline_ = pipelineManager_.createComputePipeline(ci, "bloom_upsample");
    destroyOwnedShaderModule(dev, module, nullptr);
  }
}

void BloomManager::destroyTextures() {
  VkDevice dev = device_->device();

  if (downsampleDescriptorPool_ != VK_NULL_HANDLE) {
    destroyOwnedDescriptorPool(dev, downsampleDescriptorPool_, nullptr);
    downsampleDescriptorPool_ = VK_NULL_HANDLE;
  }
  if (upsampleDescriptorPool_ != VK_NULL_HANDLE) {
    destroyOwnedDescriptorPool(dev, upsampleDescriptorPool_, nullptr);
    upsampleDescriptorPool_ = VK_NULL_HANDLE;
  }
  downsampleSets_.clear();
  upsampleSets_.clear();

  for (auto& m : mips_) {
    if (m.view != VK_NULL_HANDLE) {
      destroyVulkanImageView(dev, m.view, nullptr);
    }
    if (m.image != VK_NULL_HANDLE) {
      vmaDestroyImage(allocationManager_.memoryManager()->allocator(), m.image, m.allocation);
    }
  }
  mips_.clear();
  mipViews_.clear();
  for (auto& m : upsampleMips_) {
    if (m.view != VK_NULL_HANDLE) {
      destroyVulkanImageView(dev, m.view, nullptr);
    }
    if (m.image != VK_NULL_HANDLE) {
      vmaDestroyImage(allocationManager_.memoryManager()->allocator(), m.image, m.allocation);
    }
  }
  upsampleMips_.clear();
  upsampleViews_.clear();
  mipCount_ = 0;
}

}  // namespace container::renderer
