#include "Container/renderer/core/RendererFrontend.h"

#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/SceneDataCapture.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <glm/matrix.hpp>
#include <limits>
#include <stdexcept>

namespace container::renderer {
namespace {

constexpr uint32_t kSceneDataGroupSize = 8;
struct SceneDataPushConstants {
  glm::mat4 inverseProjection;
  uint32_t width;
  uint32_t height;
};
static_assert(offsetof(SceneDataPushConstants, width) == 64);
static_assert(offsetof(SceneDataPushConstants, height) == 68);

}  // namespace

void RendererFrontend::prepareSceneDataCapture(FrameRecordParams& frame,
                                              const glm::mat4& projection) {
  const auto extent = frame.screenshot.extent;
  if (frame.runtime.activeTechnique != RenderTechniqueId::DeferredRaster ||
      msaaSampleCount_ != VK_SAMPLE_COUNT_1_BIT || subs_.temporalManager->settings().enabled ||
      subs_.temporalManager->upscalingActive() ||
      extent.width != frame.runtime.sceneExtent.width || extent.height != frame.runtime.sceneExtent.height)
    throw std::runtime_error("Scene data requires Deferred raster, 1× MSAA and native resolution with TAA disabled");
  auto extension = container::util::pathToUtf8(
      container::util::pathFromUtf8(activePrimaryModelPath_).extension());
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if ((extension != ".gltf" && extension != ".glb") ||
      (subs_.bimManager && subs_.bimManager->hasScene()) || frame.debug.fullWireframe() ||
      frame.scene.diagCubeObjectIndex != std::numeric_limits<uint32_t>::max())
    throw std::runtime_error("Scene data requires a filled primary glTF scene without auxiliary models");

  auto& capture = sceneDataCapture_;
  capture.recorded = false;
  const auto device = svc_.ctx.deviceWrapper->device();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(svc_.ctx.deviceWrapper->physicalDevice(), &properties);
  const VkDeviceSize pixelCount = VkDeviceSize{extent.width} * extent.height;
  if (pixelCount == 0 || pixelCount > properties.limits.maxStorageBufferRange / sizeof(SceneDataCapturePixel))
    throw std::runtime_error("Scene data capture exceeds the Vulkan storage buffer size limit");
  const auto byteCount = pixelCount * sizeof(SceneDataCapturePixel);

  const std::array views{
      deferredRasterImageView(frame, DeferredRasterImageId::DepthSamplingView),
      deferredRasterImageView(frame, DeferredRasterImageId::Normal),
      deferredRasterImageView(frame, DeferredRasterImageId::PickId)};
  if (views[0] == VK_NULL_HANDLE || views[1] == VK_NULL_HANDLE || views[2] == VK_NULL_HANDLE)
    throw std::runtime_error("Scene data capture could not access the Deferred G-buffer");

  if (!capture.pipeline) {
    try {
      const auto image = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      const auto compute = VK_SHADER_STAGE_COMPUTE_BIT;
      capture.descriptorLayout = svc_.pipelineManager.createDescriptorSetLayout({
          {0, image, 1, compute, nullptr}, {1, image, 1, compute, nullptr},
          {2, image, 1, compute, nullptr}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, compute, nullptr}}, {});
      capture.pipelineLayout = svc_.pipelineManager.createPipelineLayout(
          {capture.descriptorLayout}, {{compute, 0, sizeof(SceneDataPushConstants)}});
      capture.pipeline = svc_.pipelineManager.createComputePipeline(
          container::util::executableDirectory() / "spv_shaders" / "scene_data_capture.comp.spv",
          capture.pipelineLayout, "scene-data-capture");
      capture.descriptorPool = svc_.pipelineManager.createDescriptorPool(
          {{image, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}, 1);
      VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocation.descriptorPool = capture.descriptorPool;
      allocation.descriptorSetCount = 1;
      allocation.pSetLayouts = &capture.descriptorLayout;
      if (vkAllocateDescriptorSets(device, &allocation, &capture.descriptorSet) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate the scene data capture descriptor set");
    } catch (...) {
      destroySceneDataCapture();
      throw;
    }
  }

  auto& readback = capture.readback;
  if (!readback.readbackBuffer.buffer || readback.readbackSize != byteCount) {
    auto replacement = svc_.allocationManager.createBuffer(
        byteCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (readback.readbackBuffer.buffer)
      svc_.allocationManager.destroyBuffer(readback.readbackBuffer);
    readback.readbackBuffer = replacement;
    readback.readbackSize = byteCount;
  }
  readback.extent = extent;

  const std::array<VkDescriptorImageInfo, 3> images{{
      {VK_NULL_HANDLE, views[0], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL},
      {VK_NULL_HANDLE, views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, views[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
  const VkDescriptorBufferInfo output{readback.readbackBuffer.buffer, 0, byteCount};
  std::array<VkWriteDescriptorSet, 4> writes{};
  for (uint32_t binding = 0; binding < writes.size(); ++binding) {
    auto& write = writes[binding];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = capture.descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    if (binding < images.size()) {
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &images[binding];
    } else {
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &output;
    }
  }
  vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  const SceneDataPushConstants push{glm::inverse(projection), extent.width, extent.height};
  frame.services.sceneDataCapture = [this, push](VkCommandBuffer command, const FrameRecordParams&) {
    auto& state = sceneDataCapture_;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout,
                            0, 1, &state.descriptorSet, 0, nullptr);
    vkCmdPushConstants(command, state.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command, (push.width + kSceneDataGroupSize - 1) / kSceneDataGroupSize,
                  (push.height + kSceneDataGroupSize - 1) / kSceneDataGroupSize, 1);
    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = state.readback.readbackBuffer.buffer;
    barrier.size = state.readback.readbackSize;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
    state.recorded = true;
  };
}

void RendererFrontend::writeSceneDataCapture() {
  const auto& readback = sceneDataCapture_.readback;
  if (!sceneDataCapture_.recorded)
    throw std::runtime_error("The Deferred G-buffer did not record scene data for this frame");
  const auto& buffer = readback.readbackBuffer;
  // The screenshot fence has completed both the data pass and the RGB readback.
  const auto* pixels = static_cast<const SceneDataCapturePixel*>(buffer.allocation_info.pMappedData);
  if (!pixels || vmaInvalidateAllocation(svc_.allocationManager.memoryManager()->allocator(),
          buffer.allocation, 0, readback.readbackSize) != VK_SUCCESS)
    throw std::runtime_error("Failed to read the scene data capture buffer");
  const auto encoded = encodeSceneDataCapture(
      {pixels, size_t(readback.extent.width) * readback.extent.height},
      readback.extent.width, readback.extent.height, *subs_.sceneController, sceneGraph_);
  container::util::writeFileAtomically(sceneDataExrPath(screenshot_.outputPath), encoded.exrBytes());
  container::util::writeFileAtomically(sceneDataLookupPath(screenshot_.outputPath), encoded.lookupJson);
}

void RendererFrontend::destroySceneDataCapture() {
  auto& capture = sceneDataCapture_;
  svc_.pipelineManager.destroyDescriptorPool(capture.descriptorPool);
  svc_.pipelineManager.destroyPipeline(capture.pipeline);
  svc_.pipelineManager.destroyPipelineLayout(capture.pipelineLayout);
  svc_.pipelineManager.destroyDescriptorSetLayout(capture.descriptorLayout);
  if (capture.readback.readbackBuffer.buffer)
    svc_.allocationManager.destroyBuffer(capture.readback.readbackBuffer);
  capture = {};
}

}  // namespace container::renderer
