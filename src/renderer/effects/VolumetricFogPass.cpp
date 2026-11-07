#include "Container/renderer/effects/VolumetricFogPass.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace container::renderer {
namespace {

constexpr uint64_t kMaxFroxels = 1'200'000;
constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

uint32_t groups(uint32_t count, uint32_t groupSize) {
  return count / groupSize + uint32_t(count % groupSize != 0);
}

struct FogGrid {
  uint32_t x, y, z;
  [[nodiscard]] uint64_t cells() const { return uint64_t(x) * y * z; }
  bool operator==(const FogGrid&) const = default;
};

FogGrid gridFor(VkExtent2D extent, VolumetricFogMode mode) {
  uint32_t tile = mode == VolumetricFogMode::Low ? 16u : 8u;
  const uint32_t slices = mode == VolumetricFogMode::Low ? 32u : 64u;
  FogGrid grid{groups(extent.width, tile), groups(extent.height, tile), slices};
  while (grid.cells() > kMaxFroxels) {
    tile *= 2;
    grid = {groups(extent.width, tile), groups(extent.height, tile), slices};
  }
  return grid;
}

struct alignas(16) FogUniform {
  gpu::CameraData cameraData;
  glm::mat4 previousViewProj{1.0f};
  glm::vec4 previousPosition{};
  glm::vec4 previousForward{};
  glm::uvec4 grid{};
  glm::uvec4 viewport{};
  glm::vec4 medium{};
  glm::vec4 temporal{};
};
static_assert(sizeof(FogUniform) == 624 && offsetof(FogUniform, previousViewProj) == 464 &&
              offsetof(FogUniform, grid) == 560 && offsetof(FogUniform, temporal) == 608);

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                   VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = sourceStage;
  barrier.srcAccessMask = sourceAccess;
  barrier.dstStageMask = destinationStage;
  barrier.dstAccessMask = destinationAccess;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

void colorBarrier(VkCommandBuffer cmd, VkImage image, bool begin, VkBuffer oitNodes = VK_NULL_HANDLE) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  constexpr VkAccessFlags2 generalAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  barrier.srcStageMask = begin ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.srcAccessMask = begin ? (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT) : generalAccess;
  barrier.dstStageMask = begin ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask = begin ? generalAccess : VK_ACCESS_2_MEMORY_READ_BIT;
  barrier.oldLayout = begin ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
  barrier.newLayout = begin ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkBufferMemoryBarrier2 oitBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  oitBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  oitBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  oitBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  oitBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
  oitBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  oitBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  oitBarrier.buffer = oitNodes;
  oitBarrier.size = VK_WHOLE_SIZE;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  if (oitNodes) {
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &oitBarrier;
  }
  vkCmdPipelineBarrier2(cmd, &dependency);
}

float limitDistance(const FrameRecordParams& frame) {
  const float distance = frame.volumetricFog.settings.distance;
  const float far = frame.camera.farPlane;
  return std::isfinite(far) && far > frame.camera.nearPlane ? std::min(far, distance) : distance;
}

} // namespace

struct VolumetricFogPass::Impl {
  struct Slot {
    gpu::AllocatedBuffer parameters;
    VkImage color{VK_NULL_HANDLE};
    VkBuffer oitNodes{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> sets{};
  };

  struct History {
    glm::mat4 viewProj;
    glm::vec4 position;
    glm::vec4 forward;
    VolumetricFogSettings settings;
    RenderTechniqueId technique;
    std::shared_ptr<const void> injectionShader;
    uint64_t sceneRevision;
    float nearPlane;
    float distance;
    bool orthographic;
    bool reprojected;
    uint32_t currentIndex;
    uint32_t ordinal;
  };

  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::AllocationManager& allocator;
  gpu::PipelineManager& pipelines;
  VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
  VkDescriptorPool pool{VK_NULL_HANDLE};
  VkPipelineLayout layout{VK_NULL_HANDLE};
  gpu::ComputePipeline injection;
  gpu::ComputePipeline integration;
  gpu::ComputePipeline composition;
  std::vector<Slot> slots;
  std::array<gpu::AllocatedBuffer, 2> froxels;
  gpu::AllocatedBuffer cumulative;
  VkExtent2D extent{};
  VkDeviceSize bytes{};
  std::optional<History> previous;
  std::optional<History> pending;

  Impl(std::shared_ptr<gpu::VulkanDevice> deviceIn, gpu::AllocationManager& allocatorIn,
       gpu::PipelineManager& pipelinesIn)
      : device(std::move(deviceIn)), allocator(allocatorIn), pipelines(pipelinesIn) {}

  ~Impl() {
    pipelines.destroyPipeline(composition);
    pipelines.destroyPipeline(integration);
    pipelines.destroyPipeline(injection);
    pipelines.destroyPipelineLayout(layout);
    pipelines.destroyDescriptorPool(pool);
    pipelines.destroyDescriptorSetLayout(setLayout);
    for (auto& slot : slots) allocator.destroyBuffer(slot.parameters);
    for (auto& buffer : froxels) allocator.destroyBuffer(buffer);
    allocator.destroyBuffer(cumulative);
  }

  void createSlot(uint32_t index, const FrameResources& frame, const ShadowManager& shadows, VkDeviceSize range) {
    if (frame.sceneColor.format != kSceneColorFormat || !frame.sceneColor.view || !frame.depthSamplingView ||
        !frame.oitHeadPointers.view || !frame.oitNodeBuffer.buffer || !frame.oitMetadataBuffer.buffer ||
        !shadows.shadowUbo(index).buffer || !shadows.localShadowUbo(index).buffer)
      throw std::runtime_error("volumetric fog requires resolved HDR, depth, transparency and raster shadow resources");
    auto& slot = slots[index];
    slot.color = frame.sceneColor.image;
    slot.oitNodes = frame.oitNodeBuffer.buffer;
    slot.parameters = allocator.createBuffer(sizeof(FogUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!slot.parameters.allocation_info.pMappedData)
      throw std::runtime_error("volumetric fog parameter mapping is unavailable");
    bytes += slot.parameters.allocation_info.size;
    const std::array layouts{setLayout, setLayout};
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device->device(), &allocate, slot.sets.data()) != VK_SUCCESS)
      throw std::runtime_error("could not allocate volumetric fog descriptors");

    const std::array<VkDescriptorImageInfo, 5> images{{
        {{}, frame.depthSamplingView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {{}, frame.sceneColor.view, VK_IMAGE_LAYOUT_GENERAL},
        {{}, shadows.shadowAtlasArrayView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {{}, shadows.localShadowAtlasArrayView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {{}, frame.oitHeadPointers.view, VK_IMAGE_LAYOUT_GENERAL}}};
    for (uint32_t parity = 0; parity < 2; ++parity) {
      const std::array<VkDescriptorBufferInfo, 8> buffers{{
          {slot.parameters.buffer, 0, sizeof(FogUniform)},
          {froxels[1u - parity].buffer, 0, range}, {froxels[parity].buffer, 0, range},
          {cumulative.buffer, 0, range}, {shadows.shadowUbo(index).buffer, 0, sizeof(gpu::ShadowData)},
          {shadows.localShadowUbo(index).buffer, 0, sizeof(gpu::LocalShadowData)},
          {slot.oitNodes, 0, VK_WHOLE_SIZE}, {frame.oitMetadataBuffer.buffer, 0, sizeof(OitMetadata)}}};
      std::array<VkWriteDescriptorSet, 13> writes{};
      uint32_t cursor = 0;
      auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image,
                       const VkDescriptorBufferInfo* buffer) {
        auto& dst = writes[cursor++];
        dst = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        dst.dstSet = slot.sets[parity];
        dst.dstBinding = binding;
        dst.descriptorCount = 1;
        dst.descriptorType = type;
        dst.pImageInfo = image;
        dst.pBufferInfo = buffer;
      };
      write(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buffers[0]);
      write(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &images[0], nullptr);
      write(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[1], nullptr);
      for (uint32_t i = 0; i < 3; ++i) write(3 + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[1 + i]);
      write(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[4], nullptr);
      write(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[6]);
      write(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buffers[7]);
      write(10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buffers[4]);
      write(11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &images[2], nullptr);
      write(12, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buffers[5]);
      write(13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &images[3], nullptr);
      vkUpdateDescriptorSets(device->device(), cursor, writes.data(), 0, nullptr);
    }
  }

  bool canReproject(const FrameRecordParams& frame, float distance) const {
    if (!previous) return false;
    const auto& past = *previous;
    const auto& now = frame.volumetricFog;
    if (past.sceneRevision != now.historyRevision || past.settings != now.settings ||
        past.technique != frame.runtime.activeTechnique || past.orthographic != frame.camera.orthographic ||
        past.nearPlane != frame.camera.nearPlane || past.distance != distance ||
        past.injectionShader != injection.contentIdentity()) return false;
    const glm::vec3 move = glm::vec3(now.cameraData->cameraWorldPosition - past.position);
    return glm::dot(move, move) <= distance * distance / 64.0f &&
           glm::dot(glm::vec3(now.cameraData->cameraForward), glm::vec3(past.forward)) >= 0.75f;
  }
};

VolumetricFogPass::VolumetricFogPass(std::shared_ptr<gpu::VulkanDevice> device,
    gpu::AllocationManager& allocator, gpu::PipelineManager& pipelines)
    : impl_(std::make_unique<Impl>(std::move(device), allocator, pipelines)) {}

VolumetricFogPass::~VolumetricFogPass() = default;

void VolumetricFogPass::createResources(const std::filesystem::path& shaderRoot, const FrameResourceManager& frames,
    VkExtent2D extent, const ShadowManager& shadows, VkDescriptorSetLayout lightLayout) {
  auto& state = *impl_;
  const auto frameCount = static_cast<uint32_t>(frames.frameCount());
  VkFormatProperties format{};
  vkGetPhysicalDeviceFormatProperties(state.device->physicalDevice(), kSceneColorFormat, &format);
  if (!extent.width || !extent.height || !frameCount || !lightLayout || !shadows.shadowAtlasArrayView() ||
      !shadows.localShadowAtlasArrayView() || shadows.shadowUbos().size() < frameCount ||
      shadows.localShadowUbos().size() < frameCount || !state.device->enabledFeatures().shaderStorageImageExtendedFormats ||
      !(format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
    throw std::runtime_error("volumetric fog requires storage-capable RGBA16F and raster shadow atlases");
  state.extent = extent;
  const std::vector<VkDescriptorSetLayoutBinding> bindings{
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {12, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  state.setLayout = state.pipelines.createDescriptorSetLayout(bindings, std::vector<VkDescriptorBindingFlags>(bindings.size()));
  state.layout = state.pipelines.createPipelineLayout({state.setLayout, lightLayout}, {});
  const auto shaders = shaderRoot / "spv_shaders";
  state.injection = state.pipelines.createComputePipeline(shaders / "volumetric_fog_inject.comp.spv", state.layout, "fog-injection");
  state.integration = state.pipelines.createComputePipeline(shaders / "volumetric_fog_integrate.comp.spv", state.layout, "fog-integration");
  state.composition = state.pipelines.createComputePipeline(shaders / "volumetric_fog_composite.comp.spv", state.layout, "fog-composition");
  const uint32_t setCount = frameCount * 2;
  state.pool = state.pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * setCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * setCount}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * setCount},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * setCount}}, setCount);
  const VkDeviceSize range = std::max(gridFor(extent, VolumetricFogMode::High).cells(),
      gridFor(extent, VolumetricFogMode::Low).cells()) * sizeof(glm::vec4);
  for (auto* buffer : {&state.froxels[0], &state.froxels[1], &state.cumulative}) {
    *buffer = state.allocator.createBuffer(range, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    state.bytes += buffer->allocation_info.size;
  }
  state.slots.resize(frameCount);
  for (uint32_t i = 0; i < frameCount; ++i) state.createSlot(i, *frames.frame(i), shadows, range);
}

bool VolumetricFogPass::ready(const FrameRecordParams& frame) const {
  return impl_->injection && frame.runtime.imageIndex < impl_->slots.size() && frame.debug.displayMode == 0 &&
      (frame.runtime.activeTechnique == RenderTechniqueId::DeferredRaster ||
       frame.runtime.activeTechnique == RenderTechniqueId::ForwardRaster) &&
      frame.volumetricFog.settings.enabled() && frame.volumetricFog.cameraData && frame.volumetricFog.lightSet &&
      limitDistance(frame) > frame.camera.nearPlane;
}

void VolumetricFogPass::record(VkCommandBuffer command, const FrameRecordParams& frame, bool transparentOitActive) {
  if (!ready(frame)) return;
  auto& state = *impl_;
  auto& slot = state.slots[frame.runtime.imageIndex];
  const auto& input = frame.volumetricFog;
  const auto grid = gridFor(state.extent, input.settings.mode);
  const float distance = limitDistance(frame);
  const bool reproject = state.canReproject(frame, distance);
  const uint32_t parity = state.previous ? 1u - state.previous->currentIndex : 0u;
  const uint32_t ordinal = state.previous ? state.previous->ordinal + 1u : 0u;
  FogUniform uniform;
  uniform.cameraData = *input.cameraData;
  if (reproject) {
    uniform.previousViewProj = state.previous->viewProj;
    uniform.previousPosition = state.previous->position;
    uniform.previousForward = state.previous->forward;
  }
  uniform.grid = {grid.x, grid.y, grid.z, ordinal};
  uniform.viewport = {state.extent.width, state.extent.height, static_cast<uint32_t>(input.settings.mode),
                      input.settings.mode == VolumetricFogMode::Low ? 16u : 32u};
  uniform.medium = {input.settings.density, input.settings.albedo, input.settings.anisotropy, distance};
  uniform.temporal = {reproject ? 0.8f : 0.0f, frame.camera.nearPlane,
                      frame.camera.orthographic ? 1.0f : 0.0f, transparentOitActive ? 1.0f : 0.0f};
  std::memcpy(slot.parameters.allocation_info.pMappedData, &uniform, sizeof(uniform));
  if (vmaFlushAllocation(state.allocator.memoryManager()->allocator(), slot.parameters.allocation, 0, sizeof(uniform)) != VK_SUCCESS)
    throw std::runtime_error("could not publish volumetric fog parameters");

  // The same graphics queue owns all fog buffers. This dependency retires
  // previous readers and makes the submitted history, raster shadows and
  // resolved scene depth available to compute.
  constexpr auto compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  constexpr VkAccessFlags2 storage = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  memoryBarrier(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
      compute, storage | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT);
  const std::array sets{slot.sets[parity], input.lightSet};
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.layout, 0,
      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.injection);
  vkCmdDispatch(command, groups(grid.x, 4), groups(grid.y, 4), groups(grid.z, 4));
  memoryBarrier(command, compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, compute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.integration);
  vkCmdDispatch(command, groups(grid.x, 8), groups(grid.y, 8), 1);
  memoryBarrier(command, compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, compute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  colorBarrier(command, slot.color, true);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.composition);
  vkCmdDispatch(command, groups(state.extent.width, 8), groups(state.extent.height, 8), 1);
  // Both the early temporal resolve and the later native resolve read the
  // surface nodes. Their original barriers cover the raster producer only.
  colorBarrier(command, slot.color, false, slot.oitNodes);

  state.pending = Impl::History{input.cameraData->viewProj, input.cameraData->cameraWorldPosition,
      input.cameraData->cameraForward, input.settings, frame.runtime.activeTechnique, state.injection.contentIdentity(),
      input.historyRevision, frame.camera.nearPlane, distance, frame.camera.orthographic, reproject, parity, ordinal};
}

void VolumetricFogPass::onSubmitted(bool recorded) noexcept {
  auto& state = *impl_;
  if (recorded) state.previous = std::move(state.pending);
  else state.previous.reset();
  state.pending.reset();
}

VkDeviceSize VolumetricFogPass::allocationBytes() const { return impl_->bytes; }

bool VolumetricFogPass::historyReprojected() const noexcept {
  return impl_->previous && impl_->previous->reprojected;
}

void registerVolumetricFogPass(RenderGraphBuilder& graph, RenderPassId predecessor,
    const DeferredRasterFrameGraphContext* context) {
  graph.addPass(RenderPassId::VolumetricFog, {predecessor},
      [context](VkCommandBuffer cmd, const FrameRecordParams& frame) {
        const bool transparentOitActive = context && context->isPassActive(RenderPassId::OitClear) &&
            context->isPassActive(RenderPassId::OitResolve);
        frame.services.volumetricFogPass->record(cmd, frame, transparentOitActive);
      });
  graph.setPassReadiness(RenderPassId::VolumetricFog, [](const FrameRecordParams& frame) {
    return frame.services.volumetricFogPass && frame.services.volumetricFogPass->ready(frame)
        ? renderPassReady() : renderPassNotNeeded();
  });
  graph.setPassResourceAccess(RenderPassId::VolumetricFog,
      {RenderResourceId::SceneColor, RenderResourceId::SceneDepth, RenderResourceId::CameraBuffer, RenderResourceId::LightingData},
      {RenderResourceId::ShadowAtlas, RenderResourceId::LocalShadowAtlas, RenderResourceId::OitStorage},
      {RenderResourceId::SceneColor, RenderResourceId::OitStorage});
  graph.setPassResourceTransitions(RenderPassId::VolumetricFog, {});
}

} // namespace container::renderer
