#include "Container/renderer/effects/OitManager.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/utility/PipelineManager.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::renderer {

OitNativeComposeInputs oitNativeComposeInputs(const FrameRecordParams& frame) {
  const auto technique = frame.runtime.activeTechnique;
  const auto* color = frame.imageBinding(technique, "scene-color");
  const auto* heads = frame.imageBinding(technique, "oit-head-pointers");
  const auto* nodes = frame.bufferBinding(technique, "oit-node-buffer");
  const auto* metadata = frame.bufferBinding(technique, "oit-metadata-buffer");
  if (!color || !heads || !nodes || !metadata ||
      color->extent.width != heads->extent.width ||
      color->extent.height != heads->extent.height ||
      color->format != VK_FORMAT_R16G16B16A16_SFLOAT ||
      heads->format != VK_FORMAT_R32_UINT ||
      (color->usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0 ||
      (heads->usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0 ||
      (nodes->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0 ||
      (metadata->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) == 0 ||
      metadata->size < sizeof(OitMetadata))
    return {};

  return {.imageIndex = frame.runtime.imageIndex,
          .sceneColor = color->image,
          .sceneColorView = color->view,
          .extent = {color->extent.width, color->extent.height},
          .headPointerImage = heads->image,
          .headPointerView = heads->view,
          .nodeBuffer = nodes->buffer,
          .nodeBufferSize = nodes->size,
          .metadataBuffer = metadata->buffer};
}

VkBufferMemoryBarrier2 makeOitCounterReadBarrier(VkBuffer counterBuffer) {
  // The retired frame's mapped counter is also read by the host. Include the
  // transfer clear, which is the last write when no fragments reach the pool.
  return {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_CLEAR_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = counterBuffer,
      .size = kOitCounterBufferSize};
}

uint64_t requiredOitNodeCapacityAfterOverflow(
    std::span<const uint32_t, kOitCounterWordCount> counters,
    uint32_t currentCapacity) {
  if (counters[kOitCounterOverflowIndex] == 0)
    return 0;

  uint64_t required = uint64_t{currentCapacity} + 1;
  for (uint32_t stripe = 0; stripe < kOitCounterStripeCount; ++stripe) {
    const uint32_t attempts = counters[stripe];
    if (attempts == 0)
      continue;
    // Stripe s allocates nodes s, s + stripeCount, and so on. Capacity is
    // one past the largest attempted index across the stripes.
    const uint64_t stripeRequired =
        uint64_t{attempts - 1} * kOitCounterStripeCount + stripe + 1;
    required = std::max(required, stripeRequired);
  }
  return required;
}

OitManager::OitManager(std::shared_ptr<container::gpu::VulkanDevice> device)
    : device_(std::move(device)) {}

OitManager::~OitManager() {
  if (pipelines_ == nullptr)
    return;
  composePipeline_.reset();
  pipelines_->destroyDescriptorPool(composePool_);
  pipelines_->destroyPipelineLayout(composeLayout_);
  pipelines_->destroyDescriptorSetLayout(composeSetLayout_);
}

void OitManager::prepareNativeComposeResources(
    container::gpu::PipelineManager& pipelines,
    const std::filesystem::path& shaderRoot, uint32_t imageCount) {
  if (imageCount == 0 || imageCount > std::numeric_limits<uint32_t>::max() / 2)
    throw std::invalid_argument("Invalid native OIT composition image count");
  if (pipelines_ != nullptr && pipelines_ != &pipelines)
    throw std::invalid_argument("Cannot replace the native OIT pipeline manager");
  pipelines_ = &pipelines;

  if (composeSetLayout_ == VK_NULL_HANDLE) {
    composeSetLayout_ = pipelines.createDescriptorSetLayout(
        {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
        {});
  }
  if (composeLayout_ == VK_NULL_HANDLE)
    composeLayout_ = pipelines.createPipelineLayout({composeSetLayout_}, {});
  if (composePipeline_ == VK_NULL_HANDLE) {
    composePipeline_ = pipelines.createComputePipeline(
        shaderRoot / "spv_shaders" / "oit_native_compose.comp.spv",
        composeLayout_, "oit-native-compose");
  }
  if (composeSets_.size() == imageCount)
    return;

  const std::vector<VkDescriptorSetLayout> layouts(imageCount, composeSetLayout_);
  std::vector<VkDescriptorSet> sets(imageCount);
  VkDescriptorPool pool = pipelines.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * imageCount},
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, imageCount},
       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount}}, imageCount);
  const VkDescriptorSetAllocateInfo allocation{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = imageCount,
      .pSetLayouts = layouts.data()};
  if (vkAllocateDescriptorSets(device_->device(), &allocation, sets.data()) != VK_SUCCESS) {
    pipelines.destroyDescriptorPool(pool);
    throw std::runtime_error("Failed to allocate native OIT composition descriptors");
  }
  pipelines.destroyDescriptorPool(composePool_);
  composePool_ = pool;
  composeSets_ = std::move(sets);
}

bool OitManager::nativeCompositionReady(const OitNativeComposeInputs& inputs) const {
  return composePipeline_ != VK_NULL_HANDLE && inputs.imageIndex < composeSets_.size() &&
         inputs.sceneColor != VK_NULL_HANDLE && inputs.sceneColorView != VK_NULL_HANDLE &&
         inputs.extent.width > 0 && inputs.extent.height > 0 &&
         inputs.headPointerImage != VK_NULL_HANDLE && inputs.headPointerView != VK_NULL_HANDLE &&
         inputs.nodeBuffer != VK_NULL_HANDLE && inputs.nodeBufferSize >= sizeof(OitNode) &&
         inputs.metadataBuffer != VK_NULL_HANDLE;
}

bool OitManager::recordNativeComposition(
    VkCommandBuffer cmd, const OitNativeComposeInputs& inputs) const {
  if (cmd == VK_NULL_HANDLE || !nativeCompositionReady(inputs))
    return false;

  const VkDescriptorSet set = composeSets_[inputs.imageIndex];
  const VkDescriptorImageInfo sceneColor{VK_NULL_HANDLE, inputs.sceneColorView, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorImageInfo heads{VK_NULL_HANDLE, inputs.headPointerView, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo nodes{inputs.nodeBuffer, 0, inputs.nodeBufferSize};
  const VkDescriptorBufferInfo metadata{inputs.metadataBuffer, 0, sizeof(OitMetadata)};
  const std::array<VkWriteDescriptorSet, 4> writes{{
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &sceneColor, nullptr, nullptr},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &heads, nullptr, nullptr},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &nodes, nullptr},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1,
       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &metadata, nullptr}}};
  vkUpdateDescriptorSets(device_->device(), static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  constexpr VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  constexpr auto compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  constexpr auto storageRead = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
  constexpr auto storageWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  constexpr auto oitProducerStages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | compute;

  // Lighting writes HDR and the lists. Fog can update both, and OpenVDB can
  // consume heads. Wait for the clear even where no fragment inserted a node.
  const std::array<VkImageMemoryBarrier2, 2> images{{
      {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
       .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | compute,
       .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | storageRead | storageWrite,
       .dstStageMask = compute,
       .dstAccessMask = storageRead | storageWrite,
       .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
       .newLayout = VK_IMAGE_LAYOUT_GENERAL,
       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .image = inputs.sceneColor,
       .subresourceRange = colorRange},
      {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
       .srcStageMask = oitProducerStages | VK_PIPELINE_STAGE_2_CLEAR_BIT,
       .srcAccessMask = storageWrite | VK_ACCESS_2_TRANSFER_WRITE_BIT,
       .dstStageMask = compute,
       .dstAccessMask = storageRead,
       .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
       .newLayout = VK_IMAGE_LAYOUT_GENERAL,
       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .image = inputs.headPointerImage,
       .subresourceRange = colorRange}}};
  const VkBufferMemoryBarrier2 nodeBarrier{
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = oitProducerStages,
      .srcAccessMask = storageWrite,
      .dstStageMask = compute,
      .dstAccessMask = storageRead,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = inputs.nodeBuffer,
      .size = inputs.nodeBufferSize};
  const VkDependencyInfo before{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers = &nodeBarrier,
      .imageMemoryBarrierCount = static_cast<uint32_t>(images.size()),
      .pImageMemoryBarriers = images.data()};
  vkCmdPipelineBarrier2(cmd, &before);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composePipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composeLayout_,
                          0, 1, &set, 0, nullptr);
  vkCmdDispatch(cmd, (inputs.extent.width + 7) / 8,
                (inputs.extent.height + 7) / 8, 1);

  VkImageMemoryBarrier2 colorReady = images[0];
  colorReady.srcStageMask = compute;
  colorReady.srcAccessMask = storageWrite;
  colorReady.dstStageMask = compute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  colorReady.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  std::swap(colorReady.oldLayout, colorReady.newLayout);
  const VkDependencyInfo after{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &colorReady};
  vkCmdPipelineBarrier2(cmd, &after);
  return true;
}

void OitManager::clearResources(VkCommandBuffer cmd,
                                const OitFrameResources& resources,
                                uint32_t invalidNodeIndex) const {
  if (cmd == VK_NULL_HANDLE ||
      resources.headPointerImage == VK_NULL_HANDLE ||
      resources.counterBuffer == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier headClear{};
  headClear.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  headClear.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
  headClear.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  headClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  headClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  headClear.image               = resources.headPointerImage;
  headClear.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  headClear.srcAccessMask       = 0;
  headClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

  VkBufferMemoryBarrier counterClear{};
  counterClear.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  counterClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  counterClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  counterClear.buffer              = resources.counterBuffer;
  counterClear.offset              = 0;
  counterClear.size                = kOitCounterBufferSize;
  counterClear.srcAccessMask       = 0;
  counterClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                       1, &counterClear, 1, &headClear);

  VkClearColorValue clearColor{};
  clearColor.uint32[0] = invalidNodeIndex;
  const VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, resources.headPointerImage,
                       VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &clearRange);
  vkCmdFillBuffer(cmd, resources.counterBuffer, 0, kOitCounterBufferSize, 0u);

  VkImageMemoryBarrier headReady = headClear;
  headReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  headReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

  VkBufferMemoryBarrier counterReady = counterClear;
  counterReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  counterReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                       1, &counterReady, 1, &headReady);
}

void OitManager::prepareResolve(VkCommandBuffer cmd,
                                const OitFrameResources& resources) const {
  if (cmd == VK_NULL_HANDLE ||
      resources.headPointerImage == VK_NULL_HANDLE ||
      resources.nodeBuffer == VK_NULL_HANDLE ||
      resources.counterBuffer == VK_NULL_HANDLE) {
    return;
  }

  constexpr VkPipelineStageFlags2 fragment =
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags2 shaderWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  constexpr VkAccessFlags2 shaderReadWrite =
      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | shaderWrite;
  const VkImageMemoryBarrier2 headBarrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = fragment,
      .srcAccessMask = shaderWrite,
      .dstStageMask = fragment,
      .dstAccessMask = shaderReadWrite,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resources.headPointerImage,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  const std::array<VkBufferMemoryBarrier2, 2> bufferBarriers{{
      {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
       .srcStageMask = fragment,
       .srcAccessMask = shaderWrite,
       .dstStageMask = fragment,
       .dstAccessMask = shaderReadWrite,
       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .buffer = resources.nodeBuffer,
       .size = VK_WHOLE_SIZE},
      makeOitCounterReadBarrier(resources.counterBuffer)}};
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
      .pBufferMemoryBarriers = bufferBarriers.data(),
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &headBarrier};
  vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace container::renderer
