#include "Container/renderer/effects/ScreenReflectionPass.h"

#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Logger.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/VulkanDevice.h"

#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace container::renderer {
namespace {
constexpr auto kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceLayers kColorLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

struct ReflectionConstants {
  uint32_t width, height, maxSteps, debugView;
};
static_assert(sizeof(ReflectionConstants) == 16);

struct alignas(16) ReflectionRayConstants {
  ReflectionConstants screen;
  glm::uvec4 limits{};
  glm::vec4 sectionPlane{};
};
static_assert(sizeof(ReflectionRayConstants) == 48);

void imageBarrier(VkCommandBuffer command, VkImage image,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                  VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = sourceStage;
  barrier.srcAccessMask = sourceAccess;
  barrier.dstStageMask = destinationStage;
  barrier.dstAccessMask = destinationAccess;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = kColorRange;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(command, &dependency);
}
}  // namespace

struct ScreenReflectionPass::Impl {
  struct ImageSlot {
    VkImage opaqueCopy{VK_NULL_HANDLE};
    VmaAllocation allocation{};
    VkImageView opaqueCopyView{VK_NULL_HANDLE};
    VkImage sceneColor{VK_NULL_HANDLE};
    VkDescriptorSet descriptors{VK_NULL_HANDLE};
    VkDescriptorSet rayDescriptors{VK_NULL_HANDLE};
  };

  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::AllocationManager& allocator;
  gpu::PipelineManager& pipelines;
  VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
  VkDescriptorPool pool{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  gpu::ComputePipeline pipeline;
  VkDescriptorSetLayout raySetLayout{VK_NULL_HANDLE};
  VkDescriptorPool rayPool{VK_NULL_HANDLE};
  VkPipelineLayout rayPipelineLayout{VK_NULL_HANDLE};
  gpu::ComputePipeline rayPipeline;
  bool rayActive{};
  std::vector<ImageSlot> images;
  VkExtent2D extent{};
  VkDeviceSize bytes{};

  Impl(std::shared_ptr<gpu::VulkanDevice> deviceIn,
       gpu::AllocationManager& allocatorIn, gpu::PipelineManager& pipelinesIn)
      : device(std::move(deviceIn)), allocator(allocatorIn), pipelines(pipelinesIn) {}

  ~Impl() {
    destroyRayResources();
    pipelines.destroyPipeline(pipeline);
    pipelines.destroyPipelineLayout(pipelineLayout);
    pipelines.destroyDescriptorPool(pool);
    pipelines.destroyDescriptorSetLayout(setLayout);
    for (const auto& image : images) {
      if (image.opaqueCopyView)
        destroyVulkanImageView(device->device(), image.opaqueCopyView, nullptr);
      if (image.opaqueCopy)
        vmaDestroyImage(allocator.memoryManager()->allocator(), image.opaqueCopy, image.allocation);
    }
  }

  void destroyRayResources() noexcept {
    pipelines.destroyPipeline(rayPipeline);
    pipelines.destroyPipelineLayout(rayPipelineLayout);
    pipelines.destroyDescriptorPool(rayPool);
    pipelines.destroyDescriptorSetLayout(raySetLayout);
    rayActive = false;
  }

  VkDescriptorSet createImageDescriptors(const ImageSlot& slot, const FrameResources& frame,
                                        VkDescriptorSetLayout layout, VkDescriptorPool descriptorPool) {
    VkDescriptorSet descriptors{};
    VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set.descriptorPool = descriptorPool;
    set.descriptorSetCount = 1;
    set.pSetLayouts = &layout;
    if (vkAllocateDescriptorSets(device->device(), &set, &descriptors) != VK_SUCCESS)
      throw std::runtime_error("could not allocate screen reflection descriptors");

    const std::array<VkDescriptorImageInfo, 3> resources{{
        {{}, slot.opaqueCopyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {{}, frame.sceneColor.view, VK_IMAGE_LAYOUT_GENERAL},
        // Lighting samples depth while stencil is writable. After lighting,
        // both aspects are read-only.
        {{}, frame.depthSamplingView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
    }};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
      writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[binding].dstSet = descriptors;
      writes[binding].dstBinding = binding;
      writes[binding].descriptorCount = 1;
      writes[binding].descriptorType = binding == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[binding].pImageInfo = &resources[binding];
    }
    vkUpdateDescriptorSets(device->device(), static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    return descriptors;
  }

  void createImage(ImageSlot& slot, const FrameResources& frame) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = kColorFormat;
    image.extent = {extent.width, extent.height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationInfo allocated{};
    if (vmaCreateImage(allocator.memoryManager()->allocator(), &image, &allocation,
                       &slot.opaqueCopy, &slot.allocation, &allocated) != VK_SUCCESS)
      throw std::runtime_error("could not allocate opaque reflection source");
    bytes += allocated.size;

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = slot.opaqueCopy;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = kColorFormat;
    view.subresourceRange = kColorRange;
    if (createVulkanImageView(device->device(), &view, nullptr, &slot.opaqueCopyView) != VK_SUCCESS)
      throw std::runtime_error("could not create opaque reflection source view");
    slot.sceneColor = frame.sceneColor.image;
    slot.descriptors = createImageDescriptors(slot, frame, setLayout, pool);
  }

  void createRayResources(const std::filesystem::path& shaderRoot,
                          const FrameResourceManager& frames, VkDescriptorSetLayout lightLayout,
                          VkDescriptorSetLayout sceneLayout,
                          std::vector<VkDescriptorSetLayoutBinding> bindings) {
    // Appending to the private set retains the four-set layout on devices
    // whose maxBoundDescriptorSets is Vulkan's minimum of four.
    bindings.push_back({3, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (uint32_t binding = 4; binding < 8; ++binding)
      bindings.push_back({binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    raySetLayout = pipelines.createDescriptorSetLayout(bindings, std::vector<VkDescriptorBindingFlags>(bindings.size()));
    rayPipelineLayout = pipelines.createPipelineLayout({frames.lightingLayout(),
        lightLayout, sceneLayout, raySetLayout}, {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReflectionRayConstants)}});
    rayPipeline = pipelines.createComputePipeline(shaderRoot / "spv_shaders/screen_reflections_ray.comp.spv",
                                                  rayPipelineLayout, "screen-reflections-ray");
    const auto count = static_cast<uint32_t>(images.size());
    rayPool = pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, count},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count * 4}}, count);
    for (uint32_t image = 0; image < count; ++image)
      images[image].rayDescriptors = createImageDescriptors(images[image], *frames.frame(image), raySetLayout, rayPool);
  }

  void bindRayScene(VkDescriptorSet set, const RaySurfaceGpuBindings& scene) {
    // The same image fence guards both the ray surface generation and this
    // descriptor set. The graph orders the new AS build before this pass.
    const std::array<VkDescriptorBufferInfo, 4> buffers{{
        {scene.triangles, 0, scene.trianglesSize}, {scene.instances, 0, scene.instancesSize},
        {scene.surfaceTriangles, 0, scene.surfaceTrianglesSize},
        {scene.surfaceInstances, 0, scene.surfaceInstancesSize},
    }};
    VkWriteDescriptorSetAccelerationStructureKHR acceleration{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, nullptr, 1, &scene.tlas};
    std::array<VkWriteDescriptorSet, 5> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &acceleration, set, 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    for (uint32_t index = 0; index < buffers.size(); ++index) {
      writes[index + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, index + 4, 0, 1,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[index]};
    }
    vkUpdateDescriptorSets(device->device(), static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }
};

ScreenReflectionPass::ScreenReflectionPass(std::shared_ptr<gpu::VulkanDevice> device,
                                           gpu::AllocationManager& allocator,
                                           gpu::PipelineManager& pipelines)
    : impl_(std::make_unique<Impl>(std::move(device), allocator, pipelines)) {}

ScreenReflectionPass::~ScreenReflectionPass() = default;

void ScreenReflectionPass::createResources(const std::filesystem::path& shaderRoot,
                                           const FrameResourceManager& frames, VkExtent2D extent,
                                           VkDescriptorSetLayout lightLayout,
                                           VkDescriptorSetLayout sceneLayout, bool requestRayFallback) {
  auto& state = *impl_;
  state.extent = extent;
  const std::vector<VkDescriptorSetLayoutBinding> bindings{
      {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  state.setLayout = state.pipelines.createDescriptorSetLayout(bindings, {0, 0, 0});
  state.pipelineLayout = state.pipelines.createPipelineLayout({frames.lightingLayout(),
      lightLayout, sceneLayout, state.setLayout},
      {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReflectionConstants)}});
  state.pipeline = state.pipelines.createComputePipeline(
      shaderRoot / "spv_shaders/screen_reflections.comp.spv", state.pipelineLayout,
      "screen-reflections");
  const auto count = static_cast<uint32_t>(frames.frameCount());
  state.pool = state.pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * 2},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count}}, count);
  state.images.resize(count);
  for (uint32_t image = 0; image < count; ++image)
    state.createImage(state.images[image], *frames.frame(image));
  if (!requestRayFallback || !state.device->rayQueriesEnabled()) return;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(state.device->physicalDevice(), &properties);
  // Scene (3), lighting (2) and the ray inputs (4).
  if (properties.limits.maxPerStageDescriptorStorageBuffers < 9 ||
      properties.limits.maxDescriptorSetStorageBuffers < 9) return;
  try {
    state.createRayResources(shaderRoot, frames, lightLayout, sceneLayout, bindings);
  } catch (const std::exception& error) {
    state.destroyRayResources();
    log::ContainerLogger::instance().renderer()->warn(
        "Screen reflection ray fallback unavailable: {}", error.what());
  }
}

bool ScreenReflectionPass::ready(const FrameRecordParams& frame) const {
  return impl_->pipeline && frame.runtime.imageIndex < impl_->images.size() &&
      frame.debug.screenReflections != ScreenReflectionMode::Off &&
      frame.runtime.activeTechnique == RenderTechniqueId::DeferredRaster &&
      frame.debug.displayMode == 0 &&
      deferredRasterDescriptorSet(frame, DeferredRasterDescriptorSetId::FrameLighting) &&
      deferredRasterDescriptorSet(frame, DeferredRasterDescriptorSetId::Light) &&
      deferredRasterDescriptorSet(frame, DeferredRasterDescriptorSetId::Scene);
}

bool ScreenReflectionPass::wantsRayScene(const FrameRecordParams& frame) const {
  return rayFallbackAvailable() && frame.debug.screenReflections != ScreenReflectionMode::Off &&
      frame.runtime.activeTechnique == RenderTechniqueId::DeferredRaster && frame.debug.displayMode == 0;
}

bool ScreenReflectionPass::rayFallbackAvailable() const { return static_cast<bool>(impl_->rayPipeline); }
bool ScreenReflectionPass::rayFallbackActive() const { return impl_->rayActive; }
VkDeviceSize ScreenReflectionPass::allocationBytes() const { return impl_->bytes; }

void ScreenReflectionPass::record(VkCommandBuffer command, const FrameRecordParams& frame) const {
  if (!ready(frame)) return;
  auto& state = *impl_;
  const auto& slot = state.images[frame.runtime.imageIndex];
  const auto mode = frame.debug.screenReflections;
  const auto rayScene = rayFallbackAvailable() && frame.services.rayShadowManager
      ? frame.services.rayShadowManager->surfaceBindings(frame.runtime.imageIndex) : RaySurfaceGpuBindings{};
  state.rayActive = static_cast<bool>(rayScene);
  const auto layout = state.rayActive ? state.rayPipelineLayout : state.pipelineLayout;
  if (state.rayActive) state.bindRayScene(slot.rayDescriptors, rayScene);
  imageBarrier(command, slot.opaqueCopy, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  imageBarrier(command, slot.sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

  const VkImageCopy region{kColorLayers, {}, kColorLayers, {},
      {state.extent.width, state.extent.height, 1}};
  vkCmdCopyImage(command, slot.sceneColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      slot.opaqueCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  imageBarrier(command, slot.opaqueCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  imageBarrier(command, slot.sceneColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  // G-buffer and GTAO producers previously exposed their attachments to the
  // lighting fragments. This consumer also needs compute-stage visibility.
  VkMemoryBarrier2 gbuffer{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  gbuffer.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  gbuffer.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  gbuffer.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  gbuffer.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &gbuffer;
  vkCmdPipelineBarrier2(command, &dependency);

  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    state.rayActive ? state.rayPipeline : state.pipeline);
  const std::array sets{deferredRasterDescriptorSet(frame, DeferredRasterDescriptorSetId::FrameLighting),
      deferredRasterDescriptorSet(frame, DeferredRasterDescriptorSetId::Light),
      deferredRasterDescriptorSet(frame, DeferredRasterDescriptorSetId::Scene),
      state.rayActive ? slot.rayDescriptors : slot.descriptors};
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
  const ReflectionConstants constants{state.extent.width, state.extent.height,
      mode == ScreenReflectionMode::Low ? 24u : 72u,
      mode == ScreenReflectionMode::Confidence ? 1u : mode == ScreenReflectionMode::Distance ? 2u :
      mode == ScreenReflectionMode::Source ? 3u : 0u};
  if (state.rayActive) {
    const auto* clip = frame.pushConstants.bindless;
    const ReflectionRayConstants rayConstants{constants,
        {rayScene.instanceCount, rayScene.triangleCount, clip ? clip->sectionPlaneEnabled : 0u, 0u},
        clip ? clip->sectionPlane : glm::vec4{}};
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rayConstants), &rayConstants);
  } else {
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  }
  vkCmdDispatch(command, (state.extent.width + 7u) / 8u, (state.extent.height + 7u) / 8u, 1);

  imageBarrier(command, slot.sceneColor, VK_IMAGE_LAYOUT_GENERAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

}  // namespace container::renderer
