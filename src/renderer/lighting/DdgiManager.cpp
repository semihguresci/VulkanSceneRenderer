#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/lighting/DdgiAsyncHistory.h"
#include "Container/renderer/lighting/DdgiVolumeScroll.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderGraph.h"
#include "Container/renderer/debug/DdgiDebugRenderer.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace container::renderer {
namespace {
using GpuData = DdgiGpuData;
struct alignas(16) TraceSceneData {
  glm::vec4 sectionPlane{};
  container::gpu::SceneClipState boxClip{};
  glm::uvec4 limits{};
};
static_assert(sizeof(GpuData) == 400);
static_assert(sizeof(TraceSceneData) == 144);
struct Buffer {
  container::gpu::VulkanMemoryManager &memory;
  container::gpu::AllocatedBuffer value;
  VkDeviceSize size;
  Buffer(container::gpu::VulkanMemoryManager &m, VkDeviceSize bytes,
         VkBufferUsageFlags usage, bool upload = false)
      : memory(m),
        value(m.createBuffer(
            std::max<VkDeviceSize>(bytes, 16), usage,
            upload ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                   : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT
                   : 0)),
        size(std::max<VkDeviceSize>(bytes, 16)) {}
  ~Buffer() { memory.destroyBuffer(value); }
  void write(const void *data, size_t bytes) {
    if (bytes > size || !value.allocation_info.pMappedData)
      throw std::runtime_error("invalid DDGI upload");
    if (bytes)
      std::memcpy(value.allocation_info.pMappedData, data, bytes);
    if (vmaFlushAllocation(memory.allocator(), value.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI upload flush failed");
  }
};
struct Image {
  container::gpu::VulkanMemoryManager &memory;
  VkImage value{};
  VmaAllocation allocation{};
  VmaAllocationInfo allocationInfo{};
  vk::raii::ImageView view{nullptr};
  VkExtent2D extent{};
  bool initialized = false;
  Image(const vk::raii::Device &device, container::gpu::VulkanMemoryManager &m,
        VkExtent2D e, VkFormat format)
      : memory(m), extent(e) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {e.width, e.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(m.allocator(), &info, &alloc, &value, &allocation,
                       &allocationInfo) != VK_SUCCESS)
      throw std::runtime_error("DDGI image allocation failed");
    try {
      vk::ImageViewCreateInfo create{};
      create.image = value;
      create.format = static_cast<vk::Format>(format);
      create.viewType = vk::ImageViewType::e2D;
      create.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
      view = vk::raii::ImageView(device, create);
    } catch (...) {
      vmaDestroyImage(m.allocator(), value, allocation);
      throw;
    }
  }
  ~Image() {
    view = vk::raii::ImageView(nullptr);
    vmaDestroyImage(memory.allocator(), value, allocation);
  }
};
// Queue-order alone does not make storage writes visible. Covers the global
// atlas/state/scratch reuse across frames as well as this frame's transfers.
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dst,
             VkAccessFlags2 access) {
  VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  memory.srcStageMask =
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
  memory.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT |
                         VK_ACCESS_2_MEMORY_WRITE_BIT |
                         VK_ACCESS_2_HOST_WRITE_BIT;
  memory.dstStageMask = dst;
  memory.dstAccessMask = access;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &memory;
  vkCmdPipelineBarrier2(cmd, &dep);
}
void initialize(VkCommandBuffer cmd, Image &image) {
  if (image.initialized)
    return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image.value;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
  image.initialized = true;
}
uint64_t hashBytes(uint64_t h, const void *p, size_t bytes) {
  auto data = static_cast<const uint8_t *>(p);
  for (size_t i = 0; i < bytes; ++i)
    h = (h ^ data[i]) * 1099511628211ull;
  return h;
}
template <class T> uint64_t hashValue(uint64_t h, const T &v) {
  return hashBytes(h, &v, sizeof(v));
}
} // namespace

struct DdgiManager::Impl {
  std::shared_ptr<container::gpu::VulkanDevice> device;
  container::gpu::VulkanMemoryManager &memory;
  RayShadowManager &rayScene;
  DdgiSettings settings;
  DdgiVolumeSet volumes;
  std::vector<glm::vec3> basePositions;
  DdgiPriorityScheduler scheduler;
  DdgiPriorityPlan priorityPlan;
  std::vector<bool> epochFresh;
  uint64_t globalRevision = 0;
  std::vector<container::gpu::PointLightData> previousPoints;
  std::vector<container::gpu::AreaLightData> previousAreas;
  DdgiResourcePlan plan{};
  VkPhysicalDeviceProperties properties{};
  bool support = false, ready = false, active = false, historyValid = false;
  bool readSupported = false, receiverCompatible = true;
  bool resetPending = true, initialized = false, traced = false,
       updated = false, recorded = false;
  uint32_t history = 0, cursor = 0;
  uint32_t contentEpoch = 0, refreshRemaining = 0;
  uint64_t revision = 0, frame = 0, resets = 0;
  std::string status{"DDGI disabled"};
  DdgiUpdateBatch batch{};
  vk::raii::DescriptorSetLayout layout{nullptr}, computeSetLayout{nullptr},
      sceneLayout{nullptr}, lightSnapshotLayout{nullptr};
  vk::raii::PipelineLayout traceLayout{nullptr}, updateLayout{nullptr};
  vk::raii::Pipeline tracePipeline{nullptr}, updatePipeline{nullptr};
  std::unique_ptr<DdgiDebugRenderer> debugRenderer;
  vk::raii::Sampler sampler{nullptr};
  vk::raii::DescriptorPool pool{nullptr};
  std::vector<vk::raii::DescriptorSet> sets;
  struct Slot {
    std::unique_ptr<Buffer> read, compute, updates, scene, positionSnapshot,
        lightingSnapshot, pointSnapshot, areaSnapshot;
    uint64_t positionRevision = UINT64_MAX;
    VkDescriptorSet readSet{}, computeSet{}, sceneSet{};
    VkDescriptorSet materials{}, lights{};
    VkImageView environment{};
    VkSampler envSampler{};
    GpuData data{};
    TraceSceneData sceneData{};
  };
  std::vector<Slot> slots;
  std::array<std::unique_ptr<Image>, 2> irradiance, visibility;
  std::unique_ptr<Buffer> states, alternateStates, rays;
  std::vector<uint32_t> scrollMapping;
  uint32_t scrollSource = 0, scrollTarget = 0, lastScrollPreserved = 0;
  uint64_t scrolls = 0, positionRevision = 0;
  std::string scrollError;
  bool asyncEnabled = false, recordingAsync = false;
  DdgiAsyncHistory asyncHistory;
  vk::raii::Semaphore graphicsTimeline{nullptr}, computeTimeline{nullptr};
  vk::raii::CommandPool asyncPool{nullptr};
  std::vector<vk::raii::CommandBuffer> asyncCommands;
  DdgiPriorityPlan submittedAsyncPlan;
  uint32_t submittedAsyncEpoch = 0;
  uint64_t expectedGraphicsSignal = 0;
  Buffer &state(uint32_t index) {
    return index && alternateStates ? *alternateStates : *states;
  }
  uint64_t completedCompute() const {
    return *computeTimeline ? computeTimeline.getCounterValue() : 0;
  }
  void publishAsync() {
    if (!asyncEnabled)
      return;
    if (asyncHistory.publish(completedCompute())) {
      history = asyncHistory.history();
      historyValid = true;
      if (submittedAsyncEpoch == contentEpoch)
        for (const auto &update : submittedAsyncPlan.updates)
          if (!epochFresh[update.probeIndex]) {
            epochFresh[update.probeIndex] = true;
            if (refreshRemaining) --refreshRemaining;
          }
      submittedAsyncPlan = {};
    }
  }
  Impl(std::shared_ptr<container::gpu::VulkanDevice> d,
       container::gpu::VulkanMemoryManager &m, RayShadowManager &r,
       DdgiSettings s)
      : device(std::move(d)), memory(m), rayScene(r), settings(s),
        volumes(s) {
    validateDdgiSettings(settings);
    vkGetPhysicalDeviceProperties(device->physicalDevice(), &properties);
    const auto &limits = properties.limits;
    // Aggregate resources exclude separate samplers and acceleration structures
    // but include fragment color attachments. The largest read layout is
    // scene/light/OIT/frame-lighting/DDGI: sampled images + UBOs + SSBOs +
    // OIT storage image + one color attachment. Clustered/forward-ray use 4134.
    constexpr uint32_t readStageResources = 4115 + 9 + 9 + 1 + 1;
    // Trace uses scene/light/surface/compute: sampled images + UBOs + SSBOs +
    // storage images. The update-only layout needs fewer resources.
    constexpr uint32_t traceStageResources = 4099 + 5 + 13 + 2;
    readSupported = limits.maxBoundDescriptorSets >= 5 &&
                    limits.maxPerStageResources >= readStageResources &&
                    limits.maxPerStageDescriptorStorageBuffers >= 10 &&
                    limits.maxDescriptorSetStorageBuffers >= 10 &&
                    limits.maxPerStageDescriptorSamplers >= 17 &&
                    limits.maxDescriptorSetSamplers >= 17 &&
                    limits.maxPerStageDescriptorSampledImages >= 4115 &&
                    limits.maxDescriptorSetSampledImages >= 4115;
    support = device->rayQueriesEnabled() && rayScene.supported() &&
              readSupported &&
              limits.maxPerStageResources >= traceStageResources &&
              properties.limits.maxPerStageDescriptorStorageBuffers >= 13 &&
              properties.limits.maxDescriptorSetStorageBuffers >= 13 &&
              properties.limits.maxPerStageDescriptorUniformBuffers >= 5 &&
              properties.limits.maxDescriptorSetUniformBuffers >= 6 &&
              device->enabledFeatures().shaderStorageImageExtendedFormats;
    for (auto f : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32_SFLOAT}) {
      VkFormatProperties fp{};
      vkGetPhysicalDeviceFormatProperties(device->physicalDevice(), f, &fp);
      constexpr auto required =
          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
          VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      support &= (fp.optimalTilingFeatures & required) == required;
    }
    if (!support)
      status = "DDGI unavailable: ray queries, descriptor limits or atlas "
               "formats unsupported";
  }
  uint64_t allocated() const {
    uint64_t bytes = 0;
    for (const auto *array : {&irradiance, &visibility})
      for (const auto &i : *array)
        if (i)
          bytes += i->allocationInfo.size;
    for (const auto *b : {states.get(), alternateStates.get(), rays.get()})
      if (b)
        bytes += b->value.allocation_info.size;
    for (const auto &s : slots)
      for (const auto *b :
           {s.read.get(), s.compute.get(), s.updates.get(), s.scene.get(), s.positionSnapshot.get(),
            s.lightingSnapshot.get(), s.pointSnapshot.get(), s.areaSnapshot.get()})
        if (b)
          bytes += b->value.allocation_info.size;
    return bytes;
  }
  void bufferDescriptor(VkDescriptorSet set, uint32_t binding, const Buffer &b,
                        bool uniform = false) {
    vk::DescriptorBufferInfo info(b.value.buffer, 0, b.size);
    vk::WriteDescriptorSet w(set, binding, 0, 1,
                             uniform ? vk::DescriptorType::eUniformBuffer
                                     : vk::DescriptorType::eStorageBuffer,
                             nullptr, &info);
    device->raii().updateDescriptorSets(w, {});
  }
  void imageDescriptor(VkDescriptorSet set, uint32_t binding, VkImageView view,
                       bool storage = false, bool environment = false) {
    vk::DescriptorImageInfo info({}, view,
                                 environment
                                     ? vk::ImageLayout::eShaderReadOnlyOptimal
                                     : vk::ImageLayout::eGeneral);
    vk::WriteDescriptorSet w(set, binding, 0, 1,
                             storage ? vk::DescriptorType::eStorageImage
                                     : vk::DescriptorType::eSampledImage,
                             &info);
    device->raii().updateDescriptorSets(w, {});
  }
  void samplerDescriptor(VkDescriptorSet set, uint32_t binding,
                         VkSampler value) {
    vk::DescriptorImageInfo info(value, {}, {});
    vk::WriteDescriptorSet w(set, binding, 0, 1, vk::DescriptorType::eSampler,
                             &info);
    device->raii().updateDescriptorSets(w, {});
  }
  void descriptors(Slot &slot, uint32_t previous, uint32_t next) {
    const auto readHistory = asyncEnabled ? previous : next;
    for (auto set : {slot.readSet, slot.computeSet}) {
      bool read = set == slot.readSet;
      bufferDescriptor(set, 0, read ? *slot.read : *slot.compute, true);
      imageDescriptor(
          set, 1,
          static_cast<VkImageView>(*irradiance[read ? readHistory : previous]->view));
      imageDescriptor(
          set, 2,
          static_cast<VkImageView>(*visibility[read ? readHistory : previous]->view));
      samplerDescriptor(set, 3, static_cast<VkSampler>(*sampler));
      bufferDescriptor(set, 4, state(read ? readHistory : next));
      bufferDescriptor(set, 11, *slot.positionSnapshot);
      if (!read) {
        bufferDescriptor(set, 5, *rays);
        bufferDescriptor(set, 6, *slot.updates);
        imageDescriptor(
            set, 7, static_cast<VkImageView>(*irradiance[next]->view), true);
        imageDescriptor(
            set, 8, static_cast<VkImageView>(*visibility[next]->view), true);
        imageDescriptor(set, 9, slot.environment, false, true);
        samplerDescriptor(set, 10, slot.envSampler);
      }
    }
  }
};
DdgiManager::DdgiManager(std::shared_ptr<container::gpu::VulkanDevice> d,
                         container::gpu::VulkanMemoryManager &m,
                         RayShadowManager &r, DdgiSettings s)
    : impl_(std::make_unique<Impl>(std::move(d), m, r, s)) {}
DdgiManager::~DdgiManager() {
  try { synchronizeAsync(); } catch (const std::exception &) {}
}
bool DdgiManager::asyncActive() const { return impl_->asyncEnabled; }
uint64_t DdgiManager::asyncSubmittedUpdates() const {
  return impl_->asyncEnabled ? impl_->asyncHistory.lastCompute() : 0;
}
uint64_t DdgiManager::asyncPublishedUpdates() const {
  return impl_->asyncEnabled ? impl_->asyncHistory.publication() : 0;
}
void DdgiManager::retireImage(uint32_t image) {
  auto &p = *impl_;
  if (!p.asyncEnabled)
    return;
  const auto value = p.asyncHistory.imageRetirement(image);
  if (value && p.completedCompute() < value) {
    const vk::Semaphore semaphore = *p.computeTimeline;
    const auto result = p.device->raii().waitSemaphores(
        vk::SemaphoreWaitInfo({}, 1, &semaphore, &value), UINT64_MAX);
    if (result != vk::Result::eSuccess)
      throw std::runtime_error("DDGI async image retirement failed");
  }
  p.publishAsync();
}
void DdgiManager::synchronizeAsync() {
  auto &p = *impl_;
  if (!p.asyncEnabled)
    return;
  const auto value = p.asyncHistory.lastCompute();
  if (value && p.completedCompute() < value) {
    const vk::Semaphore semaphore = *p.computeTimeline;
    const auto result = p.device->raii().waitSemaphores(
        vk::SemaphoreWaitInfo({}, 1, &semaphore, &value), UINT64_MAX);
    if (result != vk::Result::eSuccess)
      throw std::runtime_error("DDGI async synchronization failed");
  }
  p.publishAsync();
}
std::optional<VkSemaphoreSubmitInfo> DdgiManager::graphicsWait() const {
  const auto &p = *impl_;
  if (!p.asyncEnabled || !p.asyncHistory.publication())
    return {};
  VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  info.semaphore = static_cast<VkSemaphore>(*p.computeTimeline);
  info.value = p.asyncHistory.publication();
  info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  return info;
}
std::optional<VkSemaphoreSubmitInfo> DdgiManager::graphicsSignal() const {
  auto &p = *impl_;
  if (!p.asyncEnabled)
    return {};
  VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  info.semaphore = static_cast<VkSemaphore>(*p.graphicsTimeline);
  info.value = p.asyncHistory.graphicsSignal();
  info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  p.expectedGraphicsSignal = info.value;
  return info;
}
bool DdgiManager::supported() const { return impl_->support; }
bool DdgiManager::wantsScene() const {
  return impl_->support && impl_->ready && impl_->settings.enabled;
}
bool DdgiManager::active() const { return impl_->active; }
VkDescriptorSetLayout DdgiManager::descriptorLayout() const {
  return impl_->readSupported
             ? static_cast<VkDescriptorSetLayout>(*impl_->layout)
             : VK_NULL_HANDLE;
}
VkDescriptorSet DdgiManager::readDescriptors(uint32_t image) const {
  return impl_->slots.at(image).readSet;
}
uint64_t DdgiManager::allocatedBytes() const { return impl_->allocated(); }
uint64_t DdgiManager::historyResets() const { return impl_->resets; }
uint64_t DdgiManager::frameNumber() const { return impl_->frame; }
uint32_t DdgiManager::contentEpoch() const { return impl_->contentEpoch; }
uint32_t DdgiManager::refreshRemainingProbes() const {
  return impl_->refreshRemaining;
}
DdgiUpdateBatch DdgiManager::lastBatch() const { return impl_->batch; }
std::span<const DdgiProbeUpdate> DdgiManager::lastUpdates() const {
  return impl_->batch.probeCount ? std::span<const DdgiProbeUpdate>(impl_->priorityPlan.updates)
                               : std::span<const DdgiProbeUpdate>{};
}
uint32_t DdgiManager::schedulingCursor() const { return impl_->cursor; }
DdgiProbeVolumeDesc DdgiManager::currentPrimaryVolume() const {
  return impl_->volumes.volume(0).description();
}
uint64_t DdgiManager::scrollCount() const { return impl_->scrolls; }
uint32_t DdgiManager::lastScrollPreservedProbes() const {
  return impl_->lastScrollPreserved;
}
const DdgiSettings &DdgiManager::settings() const { return impl_->settings; }
const std::string &DdgiManager::status() const { return impl_->status; }

void DdgiManager::resetSceneHistory() {
  auto &p = *impl_;
  // Prepare replacement CPU scheduling state before disturbing a working field.
  DdgiPriorityScheduler scheduler;
  scheduler.reset(uint32_t(p.basePositions.size()));
  std::vector<bool> fresh(p.basePositions.size(), false);
  synchronizeAsync();
  // Keep live timeline values/parity: resetting them without replacing their
  // semaphore would let a later writer reuse an already-signaled value.
  p.scheduler = std::move(scheduler);
  p.epochFresh = std::move(fresh);
  p.historyValid = false;
  p.resetPending = true;
  p.active = false;
  p.revision = p.globalRevision = 0;
  p.previousPoints.clear();
  p.previousAreas.clear();
  p.priorityPlan = p.submittedAsyncPlan = {};
  p.batch = {};
  p.scrollMapping.clear();
  p.scrollError.clear();
  p.lastScrollPreserved = 0;
  p.cursor = 0;
  p.frame = 0;
  p.refreshRemaining = p.ready ? p.volumes.probeCount() : 0;
  p.recorded = p.traced = p.updated = false;
  ++p.resets;
  p.status = p.settings.enabled ? "DDGI waiting: scene history reset"
                                : "DDGI disabled";
}

void DdgiManager::applySettings(const DdgiSettings &settings) {
  // Validate first so a bad draft never disrupts the submitted field.
  validateDdgiSettings(settings);
  auto &p = *impl_;
  if (settings == p.settings)
    return;
  synchronizeAsync();
  auto runtimeSettings = settings;
  if (p.settings.cameraFollow && settings.cameraFollow &&
      p.settings.volume == settings.volume)
    runtimeSettings.volume = p.volumes.volume(0).description();
  const DdgiVolumeSet volumes(runtimeSettings);
  const auto plan = planDdgiVolumeResources(
      settings, {UINT32_MAX, UINT64_MAX, settings.payloadBudgetBytes});
  const auto &old = p.settings;
  const bool latticeChanged = old.volume.origin != settings.volume.origin ||
                              old.volume.spacing != settings.volume.spacing ||
                              old.volume.counts != settings.volume.counts ||
                              old.additionalVolumes != settings.additionalVolumes;
  const bool qualityChanged = old.resources.irradianceInteriorTexels !=
                                  settings.resources.irradianceInteriorTexels ||
                              old.resources.visibilityInteriorTexels !=
                                  settings.resources.visibilityInteriorTexels ||
                              old.resources.updates.raysPerProbe !=
                                  settings.resources.updates.raysPerProbe;
  const bool needsCapacity =
      settings.enabled && p.support &&
      (!p.ready || plan.rayScratchBytes > p.rays->size ||
       plan.updateRecordBytes > p.slots.front().updates->size);
  const bool tighterAllocationCap =
      p.ready && settings.payloadBudgetBytes < p.allocated();
  const bool reallocate =
      latticeChanged || qualityChanged || needsCapacity || tighterAllocationCap ||
      old.asyncCompute != settings.asyncCompute || old.cameraFollow != settings.cameraFollow;
  if (reallocate && !p.slots.empty()) {
    // Submitted frames may still refer to every atlas and descriptor set. The
    // existing prepare contract requires all fences retired before replacing
    // them, including toggling on after initially preparing disabled resources.
    p.device->raii().waitIdle();
    const auto imageCount = uint32_t(p.slots.size());
    const auto environment = p.slots.front().environment;
    const auto sampler = p.slots.front().envSampler;
    p.settings = settings;
    p.volumes = volumes;
    prepare(imageCount, environment, sampler);
  } else {
    p.settings = settings;
    p.volumes = volumes;
  }
  if (!settings.enabled) {
    p.active = false;
    p.status = "DDGI disabled";
  }
}

void DdgiManager::createPipelines(const std::filesystem::path &root,
                                  VkDescriptorSetLayout scene,
                                  VkDescriptorSetLayout light,
                                  VkDescriptorSetLayout postProcess,
                                  VkFormat colorFormat) {
  auto &p = *impl_;
  const auto &vk = p.device->raii();
  std::vector<vk::DescriptorSetLayoutBinding> bindings;
  for (uint32_t i = 0; i < 12; ++i) {
    auto type = i == 0 ? vk::DescriptorType::eUniformBuffer
                : i == 1 || i == 2 || i == 9 ? vk::DescriptorType::eSampledImage
                : i == 3 || i == 10          ? vk::DescriptorType::eSampler
                : i == 7 || i == 8           ? vk::DescriptorType::eStorageImage
                                   : vk::DescriptorType::eStorageBuffer;
    bindings.emplace_back(i, type, 1, vk::ShaderStageFlagBits::eCompute);
  }
  p.computeSetLayout = vk::raii::DescriptorSetLayout(
      vk, vk::DescriptorSetLayoutCreateInfo({}, bindings));
  std::vector<vk::DescriptorSetLayoutBinding> readBindings;
  for (auto b : bindings)
    if (b.binding == 0 || b.binding == 1 || b.binding == 2 || b.binding == 3 ||
        b.binding == 4 || b.binding == 11) {
      b.stageFlags =
          vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex;
      readBindings.push_back(b);
    }
  p.layout = vk::raii::DescriptorSetLayout(
      vk, vk::DescriptorSetLayoutCreateInfo({}, readBindings));
  vk::SamplerCreateInfo sampler{};
  sampler.magFilter = sampler.minFilter = vk::Filter::eLinear;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      vk::SamplerAddressMode::eClampToEdge;
  p.sampler = vk::raii::Sampler(vk, sampler);
  if (p.readSupported && postProcess && colorFormat != VK_FORMAT_UNDEFINED)
    p.debugRenderer = std::make_unique<DdgiDebugRenderer>(
        p.device, root, postProcess,
        static_cast<VkDescriptorSetLayout>(*p.layout), colorFormat);
  if (!p.support)
    return;
  bindings.clear();
  for (uint32_t i = 0; i < 6; ++i)
    bindings.emplace_back(i,
                          i == 0 ? vk::DescriptorType::eUniformBuffer
                          : i == 1
                              ? vk::DescriptorType::eAccelerationStructureKHR
                              : vk::DescriptorType::eStorageBuffer,
                          1, vk::ShaderStageFlagBits::eCompute);
  p.sceneLayout = vk::raii::DescriptorSetLayout(
      vk, vk::DescriptorSetLayoutCreateInfo({}, bindings));
  const std::array lightBindings{
      vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1,
                                     vk::ShaderStageFlagBits::eCompute),
      vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                     vk::ShaderStageFlagBits::eCompute),
      vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1,
                                     vk::ShaderStageFlagBits::eCompute)};
  p.lightSnapshotLayout = vk::raii::DescriptorSetLayout(
      vk, vk::DescriptorSetLayoutCreateInfo({}, lightBindings));
  (void)light;
  const std::array layouts{vk::DescriptorSetLayout(scene),
                           *p.lightSnapshotLayout, *p.sceneLayout,
                           *p.computeSetLayout};
  p.traceLayout =
      vk::raii::PipelineLayout(vk, vk::PipelineLayoutCreateInfo({}, layouts));
  p.updateLayout = vk::raii::PipelineLayout(
      vk, vk::PipelineLayoutCreateInfo({}, *p.computeSetLayout));
  auto pipeline = [&](const char *name,
                      const vk::raii::PipelineLayout &layout) {
    auto bytes = container::util::readFile(root / "spv_shaders" / name);
    if (bytes.empty() || bytes.size() % 4)
      throw std::runtime_error("invalid DDGI SPIR-V");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    vk::raii::ShaderModule module(
        vk, vk::ShaderModuleCreateInfo({}, bytes.size(), words.data()));
    vk::PipelineShaderStageCreateInfo stage(
        {}, vk::ShaderStageFlagBits::eCompute, *module, "computeMain");
    return vk::raii::Pipeline(
        vk, nullptr, vk::ComputePipelineCreateInfo({}, stage, *layout));
  };
  p.tracePipeline = pipeline("ddgi_trace.comp.spv", p.traceLayout);
  p.updatePipeline = pipeline("ddgi_update.comp.spv", p.updateLayout);
}

void DdgiManager::prepare(uint32_t imageCount, VkImageView environment,
                          VkSampler envSampler) {
  auto &p = *impl_;
  synchronizeAsync();
  const auto &vk = p.device->raii();
  p.asyncCommands.clear();
  p.asyncPool = vk::raii::CommandPool(nullptr);
  p.graphicsTimeline = vk::raii::Semaphore(nullptr);
  p.computeTimeline = vk::raii::Semaphore(nullptr);
  p.asyncHistory.reset(imageCount);
  p.asyncEnabled = p.settings.asyncCompute && p.device->asyncComputeQueue() &&
      p.device->enabledVulkan12Features().timelineSemaphore;
  p.expectedGraphicsSignal = 0;
  p.submittedAsyncPlan = {};
  p.sets.clear();
  p.pool = vk::raii::DescriptorPool(nullptr);
  p.slots.clear();
  p.irradiance = {};
  p.visibility = {};
  p.states.reset();
  p.alternateStates.reset();
  p.rays.reset();
  p.scrollMapping.clear();
  p.scrolls = p.positionRevision = p.lastScrollPreserved = 0;
  p.ready = false;
  p.active = false;
  p.historyValid = false;
  p.initialized = false;
  p.resetPending = true;
  p.history = 0;
  p.cursor = 0;
  p.revision = 0;
  p.frame = 0;
  p.contentEpoch = 0;
  p.refreshRemaining = 0;
  p.globalRevision = 0;
  p.previousPoints.clear();
  p.previousAreas.clear();
  p.priorityPlan = {};
  p.batch = {};
  p.recorded = p.traced = p.updated = false;
  if (!imageCount || !environment || !envSampler)
    throw std::invalid_argument(
        "DDGI requires initialized frame/environment resources");
  if (p.settings.enabled && p.support) {
    try {
      p.plan = planDdgiVolumeResources(p.settings,
                                 {p.properties.limits.maxImageDimension2D,
                                  p.properties.limits.maxStorageBufferRange,
                                  p.settings.payloadBudgetBytes});
      const auto &b = p.plan.updateCapacity;
      if (b.probeCount > p.properties.limits.maxComputeWorkGroupCount[0] ||
          (uint64_t(b.rayCount) + 63) / 64 >
              p.properties.limits.maxComputeWorkGroupCount[0])
        throw std::invalid_argument("DDGI dispatch exceeds device limits");
      p.ready = true;
    } catch (const std::exception &e) {
      p.status = std::string("DDGI fallback: ") + e.what();
    }
  }
  // Even the disabled path has initialized descriptors; no optional bindings
  // are left null on raster-only devices.
  auto allocate = [&] {
    auto ir =
        p.ready ? VkExtent2D{p.plan.irradiance.width, p.plan.irradiance.height}
                : VkExtent2D{1, 1};
    auto vis =
        p.ready ? VkExtent2D{p.plan.visibility.width, p.plan.visibility.height}
                : VkExtent2D{1, 1};
    for (uint32_t i = 0; i < 2; ++i) {
      p.irradiance[i] = std::make_unique<Image>(
          vk, p.memory, ir,
          p.ready ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);
      p.visibility[i] = std::make_unique<Image>(
          vk, p.memory, vis,
          p.ready ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);
    }
    p.states = std::make_unique<Buffer>(
        p.memory, p.ready ? p.plan.probeStateBytes : 32,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (p.asyncEnabled || p.settings.cameraFollow)
      p.alternateStates = std::make_unique<Buffer>(
          p.memory, p.ready ? p.plan.probeStateBytes : 32,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    p.rays = std::make_unique<Buffer>(p.memory,
                                      p.ready ? p.plan.rayScratchBytes : 32,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    p.basePositions = p.ready ? p.volumes.positions() : std::vector<glm::vec3>(1, glm::vec3(0));
    p.scheduler.reset(uint32_t(p.basePositions.size()));
    p.epochFresh.assign(p.basePositions.size(), false);
    std::vector<glm::vec4> positions(p.basePositions.size(),
                                     glm::vec4(0));
    if (p.ready)
      for (uint32_t i = 0; i < p.volumes.probeCount(); ++i)
        positions[i] = glm::vec4(p.basePositions[i], 0);
    p.slots.resize(imageCount);
    for (auto &s : p.slots) {
      s.positionSnapshot = std::make_unique<Buffer>(
          p.memory, p.ready ? p.plan.probePositionBytes : 16,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
      s.positionSnapshot->write(positions.data(), positions.size() * sizeof(glm::vec4));
      s.positionRevision = p.positionRevision;
      s.read = std::make_unique<Buffer>(
          p.memory, sizeof(GpuData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
      s.compute = std::make_unique<Buffer>(
          p.memory, sizeof(GpuData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
      s.updates = std::make_unique<Buffer>(
          p.memory, p.ready ? p.plan.updateRecordBytes : 16,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
      s.scene =
          std::make_unique<Buffer>(p.memory, sizeof(TraceSceneData),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
      if (p.support) {
        s.lightingSnapshot = std::make_unique<Buffer>(p.memory,
            sizeof(container::gpu::LightingData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        s.pointSnapshot = std::make_unique<Buffer>(p.memory,
            sizeof(container::gpu::PointLightData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        s.areaSnapshot = std::make_unique<Buffer>(p.memory,
            sizeof(container::gpu::AreaLightData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
      }
      s.environment = environment;
      s.envSampler = envSampler;
    }
  };
  try {
    allocate();
    if (p.ready && p.allocated() > p.settings.payloadBudgetBytes)
      throw std::runtime_error(
          "DDGI actual VMA allocations exceed memory budget");
  } catch (const std::exception &e) {
    if (!p.ready)
      throw;
    p.status = std::string("DDGI fallback: ") + e.what();
    p.ready = false;
    p.slots.clear();
    p.irradiance = {};
    p.visibility = {};
    p.states.reset();
    p.alternateStates.reset();
    p.rays.reset();
    allocate();
  }
  const std::array sizes{
      vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer,
                             imageCount * 4),
      vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, imageCount * 6),
      vk::DescriptorPoolSize(vk::DescriptorType::eSampler, imageCount * 4),
      vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer,
                             imageCount * 14),
      vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, imageCount * 4),
      vk::DescriptorPoolSize(vk::DescriptorType::eAccelerationStructureKHR,
                             imageCount)};
  std::vector<vk::DescriptorPoolSize> poolSizes(sizes.begin(), sizes.end());
  if (!p.support)
    poolSizes.pop_back();
  p.pool = vk::raii::DescriptorPool(
      vk, vk::DescriptorPoolCreateInfo(
              vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
              imageCount * 4, poolSizes));
  std::vector<vk::DescriptorSetLayout> layouts;
  for (uint32_t i = 0; i < imageCount; ++i) {
    layouts.push_back(*p.layout);
    layouts.push_back(*p.computeSetLayout);
    if (p.support) {
      layouts.push_back(*p.sceneLayout);
      layouts.push_back(*p.lightSnapshotLayout);
    }
  }
  p.sets = vk.allocateDescriptorSets(
      vk::DescriptorSetAllocateInfo(*p.pool, layouts));
  size_t index = 0;
  for (auto &s : p.slots) {
    s.readSet = static_cast<VkDescriptorSet>(*p.sets[index++]);
    s.computeSet = static_cast<VkDescriptorSet>(*p.sets[index++]);
    if (p.support) {
      s.sceneSet = static_cast<VkDescriptorSet>(*p.sets[index++]);
      s.lights = static_cast<VkDescriptorSet>(*p.sets[index++]);
      p.bufferDescriptor(s.lights, 0, *s.lightingSnapshot, true);
      p.bufferDescriptor(s.lights, 1, *s.pointSnapshot);
      p.bufferDescriptor(s.lights, 2, *s.areaSnapshot);
    }
    s.read->write(&s.data, sizeof(s.data));
    s.compute->write(&s.data, sizeof(s.data));
    p.descriptors(s, 0, 0);
  }
  if (p.ready)
    p.status = "DDGI ready: bounded world-space probe volumes";
  if (p.asyncEnabled) {
    const vk::SemaphoreTypeCreateInfo timeline(vk::SemaphoreType::eTimeline, 0);
    vk::SemaphoreCreateInfo info;
    info.pNext = &timeline;
    p.graphicsTimeline = vk::raii::Semaphore(vk, info);
    p.computeTimeline = vk::raii::Semaphore(vk, info);
    p.asyncPool = vk::raii::CommandPool(vk, vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        p.device->queueFamilyIndices().graphicsFamily.value()));
    p.asyncCommands = vk.allocateCommandBuffers(vk::CommandBufferAllocateInfo(
        *p.asyncPool, vk::CommandBufferLevel::ePrimary, imageCount));
  }
}

void DdgiManager::update(uint32_t image, VkDescriptorSet scene,
                         VkDescriptorSet light,
                         const container::gpu::LightingData &lighting,
                         std::span<const container::gpu::PointLightData> points,
                         std::span<const container::gpu::AreaLightData> areas,
                         uint32_t sectionEnabled, glm::vec4 sectionPlane,
                         const container::gpu::SceneClipState &boxClip,
                         VkImageView environment, VkSampler envSampler,
                         bool receiverCompatible,
                         const DdgiSchedulingHints &hints) {
  auto &p = *impl_;
  p.publishAsync();
  auto &s = p.slots.at(image);
  (void)light;
  p.scrollError.clear();
  p.traced = false;
  p.updated = false;
  p.recorded = false;
  p.receiverCompatible = receiverCompatible;
  p.active =
      wantsScene() && receiverCompatible && p.rayScene.sceneRevision() != 0;
  if (p.support) {
    try {
      if (lighting.pointLightCount > points.size() ||
          lighting.areaLightCount > areas.size())
        throw std::runtime_error("DDGI light counts exceed snapshot arrays");
      auto grow = [&](std::unique_ptr<Buffer> &buffer, VkDeviceSize bytes,
                       uint32_t binding) {
        if (bytes <= buffer->size) return;
        if (bytes > p.properties.limits.maxStorageBufferRange ||
            bytes > p.settings.payloadBudgetBytes)
          throw std::runtime_error("DDGI light snapshot exceeds storage budget");
        auto replacement = std::make_unique<Buffer>(p.memory, bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (p.allocated() - buffer->value.allocation_info.size +
            replacement->value.allocation_info.size > p.settings.payloadBudgetBytes)
          throw std::runtime_error("DDGI light snapshot exceeds allocation budget");
        buffer = std::move(replacement);
        p.bufferDescriptor(s.lights, binding, *buffer);
      };
      grow(s.pointSnapshot, points.size_bytes(), 1);
      grow(s.areaSnapshot, areas.size_bytes(), 2);
      s.lightingSnapshot->write(&lighting, sizeof(lighting));
      s.pointSnapshot->write(points.data(), points.size_bytes());
      s.areaSnapshot->write(areas.data(), areas.size_bytes());
    } catch (const std::exception &error) {
      p.active = false;
      p.status = std::string("DDGI fallback: ") + error.what();
    }
  }
  if (p.active && p.settings.cameraFollow && hints.cameraPosition) {
    try {
      const auto scroll = planDdgiVolumeScroll(
          p.volumes.volume(0).description(), *hints.cameraPosition,
          p.volumes.probeCount());
      if (scroll.changed) {
        // Cell moves retire only the outstanding GI writer. All previous
        // graphics readers precede this frame's remap on the graphics queue.
        synchronizeAsync();
        auto runtime = p.settings;
        runtime.volume = scroll.target;
        const DdgiVolumeSet moved(runtime);
        std::vector<uint32_t> mapping(p.volumes.probeCount());
        for (uint32_t index = 0; index < mapping.size(); ++index)
          mapping[index] = index < scroll.previousIndexForNewProbe.size()
              ? scroll.previousIndexForNewProbe[index] : index;
        std::vector<bool> fresh(mapping.size(), false);
        uint32_t preserved = 0;
        for (uint32_t index = 0; index < mapping.size(); ++index)
          if (mapping[index] != kDdgiNoPreviousProbe) {
            fresh[index] = p.epochFresh[mapping[index]];
            if (index < scroll.previousIndexForNewProbe.size()) ++preserved;
          }
        p.scheduler.remap(mapping);
        p.scrollMapping = std::move(mapping);
        p.scrollSource = p.history;
        p.scrollTarget = 1 - p.history;
        p.history = p.scrollTarget;
        if (p.asyncEnabled)
          p.asyncHistory.remapPublishedHistory(p.history);
        p.volumes = moved;
        p.basePositions = moved.positions();
        p.epochFresh = std::move(fresh);
        p.refreshRemaining = uint32_t(std::count(
            p.epochFresh.begin(), p.epochFresh.end(), false));
        p.lastScrollPreserved = preserved;
        ++p.scrolls;
        ++p.positionRevision;
      }
    } catch (const std::invalid_argument &error) {
      p.scrollError = std::string("camera-follow held: ") + error.what();
    } catch (const std::overflow_error &error) {
      p.scrollError = std::string("camera-follow held: ") + error.what();
    }
  }
  if (s.positionRevision != p.positionRevision) {
    std::vector<glm::vec4> positions(p.basePositions.size());
    for (uint32_t index = 0; index < positions.size(); ++index)
      positions[index] = glm::vec4(p.basePositions[index], 0);
    s.positionSnapshot->write(positions.data(), positions.size() * sizeof(glm::vec4));
    s.positionRevision = p.positionRevision;
  }
  if (wantsScene() && !receiverCompatible)
    p.status = "DDGI fallback: BIM semantic colors use raster lighting";
  // Local-light counts affect revision identity but not transport outside their
  // influence regions. Compare the remaining global lighting state separately.
  auto globalLighting = lighting;
  globalLighting.pointLightCount = 0;
  globalLighting.areaLightCount = 0;
  uint64_t globalHash = hashValue(14695981039346656037ull, globalLighting);
  globalHash = hashValue(globalHash, environment);
  globalHash = hashValue(globalHash, envSampler);
  globalHash = hashValue(globalHash, p.settings.maxDistance);
  globalHash = hashValue(globalHash, p.settings.normalBias);
  globalHash = hashValue(globalHash, p.settings.viewBias);
  globalHash = hashValue(globalHash, p.settings.visibilityBias);
  globalHash = hashValue(globalHash, p.settings.volumeBlendCells);
  globalHash = hashValue(globalHash, p.settings.thinGlassTransmission);
  globalHash = hashValue(globalHash, p.settings.dielectricTransmission);
  uint64_t hash = hashValue(globalHash, p.rayScene.sceneRevision());
  hash = hashBytes(hash, points.data(), points.size_bytes());
  hash = hashBytes(hash, areas.data(), areas.size_bytes());
  // Trace/lookup controls change secondary transport. Output intensity only
  // scales primary receivers; retain the field and its multi-bounce feedback
  // while adjusting it, like budget, pause, debug and hysteresis controls.
  if (p.active && hash != p.revision) {
    std::vector<DdgiDirtyRegion> dirty = hints.dirtyRegions;
    bool global = p.globalRevision != globalHash || p.revision == 0;
    auto addLightRegion = [&](glm::vec4 positionRange, float emitterRadius) {
      if (!std::isfinite(positionRange.w) || positionRange.w <= 0.f ||
          !std::isfinite(emitterRadius)) { global = true; return; }
      const float radius = positionRange.w + std::max(emitterRadius, 0.f) + p.settings.maxDistance;
      if (!std::isfinite(radius)) { global = true; return; }
      const glm::vec3 center(positionRange);
      if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) {
        global = true; return;
      }
      const glm::vec3 minimum = center - glm::vec3(radius),
                      maximum = center + glm::vec3(radius);
      for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis])) {
          global = true; return;
        }
      }
      dirty.push_back({minimum, maximum});
    };
    const auto pointCount = std::max(points.size(), p.previousPoints.size());
    for (size_t i = 0; i < pointCount; ++i) {
      const bool changed = i >= points.size() || i >= p.previousPoints.size() ||
          std::memcmp(&points[i], &p.previousPoints[i], sizeof(points[i])) != 0;
      if (!changed) continue;
      if (i < points.size()) addLightRegion(points[i].positionRadius, 0.f);
      if (i < p.previousPoints.size()) addLightRegion(p.previousPoints[i].positionRadius, 0.f);
    }
    const auto areaCount = std::max(areas.size(), p.previousAreas.size());
    for (size_t i = 0; i < areaCount; ++i) {
      const bool changed = i >= areas.size() || i >= p.previousAreas.size() ||
          std::memcmp(&areas[i], &p.previousAreas[i], sizeof(areas[i])) != 0;
      if (!changed) continue;
      auto add = [&](const auto &a) {
        addLightRegion(a.positionRange, std::hypot(a.tangentHalfSize.w, a.bitangentHalfSize.w));
      };
      if (i < areas.size()) add(areas[i]);
      if (i < p.previousAreas.size()) add(p.previousAreas[i]);
    }
    if (global || dirty.empty()) dirty.clear();
    p.scheduler.markDirty(p.basePositions, dirty, hash);
    p.revision = hash;
    p.globalRevision = globalHash;
    p.previousPoints.assign(points.begin(), points.end());
    p.previousAreas.assign(areas.begin(), areas.end());
    // Dynamic revisions refresh a bounded sweep without restarting its cursor.
    // Continuously moving geometry must not starve probes late in the volume.
    // Selected old-epoch samples bypass hysteresis; previous-bounce feedback is
    // disabled until a fresh direct-light sweep completes. Primary lighting can
    // retain the last valid response while unselected probes await their turn.
    p.refreshRemaining = p.volumes.probeCount();
    std::fill(p.epochFresh.begin(), p.epochFresh.end(), false);
    if (++p.contentEpoch == 0) {
      ++p.contentEpoch;
      p.resetPending = true;
      p.historyValid = false;
    }
    ++p.resets;
  }
  p.priorityPlan = p.active && !p.settings.paused && !p.asyncHistory.pending()
      ? p.scheduler.plan(p.basePositions, p.settings.resources.updates, hints)
      : DdgiPriorityPlan{};
  p.batch = p.priorityPlan.batch;
  s.materials = scene;
  s.environment = environment;
  s.envSampler = envSampler;
  s.sceneData.sectionPlane = sectionPlane;
  s.sceneData.boxClip = boxClip;
  s.sceneData.limits = {0, 0, 32, 4};
  s.data = {};
  s.data.originNormalBias = {p.volumes.volume(0).description().origin, p.settings.normalBias};
  s.data.spacingViewBias = {p.settings.volume.spacing, p.settings.viewBias};
  s.data.counts = {p.settings.volume.counts, p.volumes.probeCount()};
  s.data.volumes = p.volumes.gpuVolumes(p.settings);
  auto layout = [](const DdgiAtlasPlan &a) {
    return glm::uvec4(a.interiorTexels, a.tileTexels, a.tilesPerRow, a.width);
  };
  s.data.irradianceLayout = layout(p.plan.irradiance);
  s.data.visibilityLayout = layout(p.plan.visibility);
  s.data.extents = {p.plan.irradiance.height, p.plan.visibility.height,
                    p.batch.probeCount, p.batch.raysPerProbe};
  s.data.trace = {p.settings.maxDistance, p.settings.hysteresis, 50.f, 3.f};
  s.data.frame = {uint32_t(p.frame), p.active ? 1u : 0u,
                  p.historyValid && p.refreshRemaining == 0 ? 1u : 0u,
                  sectionEnabled};
  s.data.intensityBias = {p.settings.intensity, p.settings.visibilityBias,
      float(p.volumes.volumeCount()), float((p.settings.thinGlassTransmission ? 1u : 0u) |
                                           (p.settings.dielectricTransmission ? 2u : 0u))};
  s.compute->write(&s.data, sizeof(s.data));
  auto read = s.data;
  read.frame.z = (p.historyValid || (!p.asyncEnabled && p.batch.probeCount)) ? 1u : 0u;
  s.read->write(&read, sizeof(read));
  s.scene->write(&s.sceneData, sizeof(s.sceneData));
  if (p.batch.probeCount) {
    auto updates = p.priorityPlan.updates;
    for (auto &update : updates)
      update.reserved = p.contentEpoch;
    s.updates->write(updates.data(), updates.size() * sizeof(DdgiProbeUpdate));
  }
  p.descriptors(s, p.history, p.batch.probeCount ? 1 - p.history : p.history);
}

void DdgiManager::recordTrace(VkCommandBuffer cmd, uint32_t image) {
  auto &p = *impl_;
  auto &s = p.slots.at(image);
  barrier(cmd,
          VK_PIPELINE_STAGE_2_TRANSFER_BIT |
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
  for (auto *array : {&p.irradiance, &p.visibility})
    for (auto &i : *array)
      initialize(cmd, *i);
  if (p.resetPending || !p.initialized) {
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (auto *array : {&p.irradiance, &p.visibility})
      for (auto &i : *array)
        vkCmdClearColorImage(cmd, i->value, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &range);
    vkCmdFillBuffer(cmd, p.states->value.buffer, 0, p.states->size, 0);
    if (p.alternateStates)
      vkCmdFillBuffer(cmd, p.alternateStates->value.buffer, 0, p.alternateStates->size, 0);
    barrier(cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
  }
  if (!p.scrollMapping.empty() && !p.recordingAsync) {
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (auto *array : {&p.irradiance, &p.visibility})
      vkCmdClearColorImage(cmd, (*array)[p.scrollTarget]->value,
                           VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    vkCmdFillBuffer(cmd, p.state(p.scrollTarget).value.buffer,
                     0, p.state(p.scrollTarget).size, 0);
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (uint32_t atlas = 0; atlas < 2; ++atlas) {
      auto &images = atlas ? p.visibility : p.irradiance;
      const auto &layout = atlas ? p.plan.visibility : p.plan.irradiance;
      std::vector<VkImageCopy> copies;
      copies.reserve(p.scrollMapping.size());
      for (uint32_t next = 0; next < p.scrollMapping.size(); ++next) {
        const auto previous = p.scrollMapping[next];
        if (previous == kDdgiNoPreviousProbe) continue;
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.srcOffset = {int32_t((previous % layout.tilesPerRow) * layout.tileTexels),
                          int32_t((previous / layout.tilesPerRow) * layout.tileTexels), 0};
        copy.dstOffset = {int32_t((next % layout.tilesPerRow) * layout.tileTexels),
                          int32_t((next / layout.tilesPerRow) * layout.tileTexels), 0};
        copy.extent = {layout.tileTexels, layout.tileTexels, 1};
        copies.push_back(copy);
      }
      if (!copies.empty())
        vkCmdCopyImage(cmd, images[p.scrollSource]->value, VK_IMAGE_LAYOUT_GENERAL,
                       images[p.scrollTarget]->value, VK_IMAGE_LAYOUT_GENERAL,
                       uint32_t(copies.size()), copies.data());
    }
    std::vector<VkBufferCopy> stateCopies;
    stateCopies.reserve(p.scrollMapping.size());
    for (uint32_t next = 0; next < p.scrollMapping.size(); ++next)
      if (p.scrollMapping[next] != kDdgiNoPreviousProbe)
        stateCopies.push_back({VkDeviceSize(p.scrollMapping[next]) * sizeof(DdgiProbeState),
                               VkDeviceSize(next) * sizeof(DdgiProbeState), sizeof(DdgiProbeState)});
    if (!stateCopies.empty())
      vkCmdCopyBuffer(cmd, p.state(p.scrollSource).value.buffer,
                      p.state(p.scrollTarget).value.buffer,
                      uint32_t(stateCopies.size()), stateCopies.data());
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
  }
  p.recorded = true;
  auto bindings = p.rayScene.surfaceBindings(image);
  if (!p.active || !bindings) {
    if (!bindings && wantsScene() && p.receiverCompatible) {
      p.active = false;
      auto reason = p.rayScene.surfaceUnsupportedReason();
      p.status = "DDGI fallback: " +
                 (reason.empty() ? "surface scene unavailable" : reason);
      p.batch = {};
      s.data.frame.y = 0;
      s.read->write(&s.data, sizeof(s.data));
    }
    return;
  }
  p.status = p.batch.probeCount ? "DDGI active: bounded world-space probe volumes"
             : p.historyValid
                 ? "DDGI paused: using submitted probe history"
                 : "DDGI waiting: update budget paused, no probe history";
  if (p.asyncEnabled && !p.settings.paused && p.asyncHistory.pending())
    p.status = p.historyValid ? "DDGI active: asynchronous update in flight"
                             : "DDGI waiting: asynchronous update in flight";
  else if (!p.asyncEnabled && p.settings.asyncCompute)
    p.status += "; serialized fallback: no compatible second queue or timeline";
  if (!p.scrollError.empty()) p.status += "; " + p.scrollError;
  if (!p.batch.probeCount)
    return;
  s.sceneData.limits.x = bindings.instanceCount;
  s.sceneData.limits.y = bindings.triangleCount;
  s.scene->write(&s.sceneData, sizeof(s.sceneData));
  p.bufferDescriptor(s.sceneSet, 0, *s.scene, true);
  vk::AccelerationStructureKHR tlas(bindings.tlas);
  vk::WriteDescriptorSetAccelerationStructureKHR as(1, &tlas);
  vk::WriteDescriptorSet aw(s.sceneSet, 1, 0, 1,
                            vk::DescriptorType::eAccelerationStructureKHR);
  aw.pNext = &as;
  p.device->raii().updateDescriptorSets(aw, {});
  const std::array buffers{bindings.triangles, bindings.instances,
                           bindings.surfaceTriangles,
                           bindings.surfaceInstances};
  const std::array sizes{bindings.trianglesSize, bindings.instancesSize,
                         bindings.surfaceTrianglesSize,
                         bindings.surfaceInstancesSize};
  for (uint32_t i = 0; i < 4; ++i) {
    vk::DescriptorBufferInfo info(buffers[i], 0, sizes[i]);
    vk::WriteDescriptorSet w(s.sceneSet, i + 2, 0, 1,
                             vk::DescriptorType::eStorageBuffer, nullptr,
                             &info);
    p.device->raii().updateDescriptorSets(w, {});
  }
  if (p.asyncEnabled && !p.recordingAsync) {
    p.status = p.historyValid ? "DDGI active: asynchronous probe updates"
                             : "DDGI waiting: asynchronous probe history";
    if (!p.scrollError.empty()) p.status += "; " + p.scrollError;
    return;
  }
  if (p.alternateStates) {
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferCopy copy{0, 0, p.states->size};
    vkCmdCopyBuffer(cmd, p.state(p.history).value.buffer,
                    p.state(1 - p.history).value.buffer, 1, &copy);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
  }
  const std::array sets{s.materials, s.lights, s.sceneSet, s.computeSet};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    static_cast<VkPipeline>(*p.tracePipeline));
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          static_cast<VkPipelineLayout>(*p.traceLayout), 0,
                          uint32_t(sets.size()), sets.data(), 0, nullptr);
  vkCmdDispatch(cmd, (p.batch.rayCount + 63) / 64, 1, 1);
  p.traced = true;
}

void DdgiManager::recordUpdate(VkCommandBuffer cmd, uint32_t image) {
  auto &p = *impl_;
  if (!p.traced)
    return;
  auto &s = p.slots.at(image);
  barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
  uint32_t next = 1 - p.history;
  for (auto *array : {&p.irradiance, &p.visibility}) {
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {(*array)[p.history]->extent.width,
                   (*array)[p.history]->extent.height, 1};
    vkCmdCopyImage(cmd, (*array)[p.history]->value, VK_IMAGE_LAYOUT_GENERAL,
                   (*array)[next]->value, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
  }
  barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    static_cast<VkPipeline>(*p.updatePipeline));
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          static_cast<VkPipelineLayout>(*p.updateLayout), 0, 1,
                          &s.computeSet, 0, nullptr);
  vkCmdDispatch(cmd, p.batch.probeCount, 1, 1);
  barrier(cmd,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
              VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT);
  p.updated = true;
}
void DdgiManager::recordDebug(VkCommandBuffer cmd, uint32_t image,
                              VkDescriptorSet postProcess) {
  const auto &p = *impl_;
  if (!p.debugRenderer || !p.active || !p.ready ||
      p.settings.debugView == DdgiDebugView::Off)
    return;
  p.debugRenderer->record(cmd, postProcess, p.slots.at(image).readSet,
                          p.volumes.probeCount(), p.settings.debugView,
                          p.settings.debugProbeRadius);
}
void DdgiManager::commit() {
  auto &p = *impl_;
  if (p.asyncEnabled && p.expectedGraphicsSignal) {
    p.asyncHistory.commitGraphics(p.expectedGraphicsSignal);
    p.expectedGraphicsSignal = 0;
  }
  if (p.recorded) {
    p.initialized = true;
    p.resetPending = false;
    p.scrollMapping.clear();
  }
  if (p.updated) {
    p.history = 1 - p.history;
    p.scheduler.commit(p.priorityPlan);
    p.cursor = p.scheduler.nextProbe();
    p.historyValid = true;
    // Priority slots may revisit a probe already refreshed in this epoch.
    // Only distinct submitted indices count toward re-enabling bounce feedback.
    for (const auto &update : p.priorityPlan.updates) {
      if (!p.epochFresh[update.probeIndex]) {
        p.epochFresh[update.probeIndex] = true;
        if (p.refreshRemaining) --p.refreshRemaining;
      }
    }
    ++p.frame;
  }
  p.recorded = p.traced = p.updated = false;
}
void DdgiManager::submitAsync(uint32_t image) {
  auto &p = *impl_;
  if (!p.asyncEnabled || !p.active || !p.batch.probeCount ||
      p.asyncHistory.pending())
    return;
  const auto work = p.asyncHistory.plan(image);
  auto &cmd = p.asyncCommands.at(image);
  cmd.reset();
  cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  p.recordingAsync = true;
  try {
    recordTrace(static_cast<VkCommandBuffer>(*cmd), image);
    recordUpdate(static_cast<VkCommandBuffer>(*cmd), image);
    cmd.end();
    p.recordingAsync = false;
  } catch (...) {
    p.recordingAsync = false;
    throw;
  }
  if (!p.updated) {
    p.recorded = p.traced = p.updated = false;
    return;
  }
  const vk::SemaphoreSubmitInfo wait(*p.graphicsTimeline, work.graphicsReady,
                                     vk::PipelineStageFlagBits2::eAllCommands);
  const vk::SemaphoreSubmitInfo signal(*p.computeTimeline, work.completion,
                                       vk::PipelineStageFlagBits2::eAllCommands);
  const vk::CommandBufferSubmitInfo command(*cmd);
  const vk::SubmitInfo2 submit({}, wait, command, signal);
  vk::raii::Queue queue(p.device->raii(),
      p.device->queueFamilyIndices().graphicsFamily.value(), 1);
  queue.submit2(submit, nullptr);
  p.asyncHistory.commit(work);
  p.submittedAsyncPlan = p.priorityPlan;
  p.submittedAsyncEpoch = p.contentEpoch;
  p.scheduler.commit(p.priorityPlan);
  p.cursor = p.scheduler.nextProbe();
  ++p.frame;
  p.recorded = p.traced = p.updated = false;
}
void registerDdgiPasses(RenderGraphBuilder &graph) {
  graph.addPass(RenderPassId::DdgiTrace, {RenderPassId::RaySceneBuild},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  if (p.services.ddgiManager)
                    p.services.ddgiManager->recordTrace(cmd,
                                                        p.runtime.imageIndex);
                });
  graph.setPassResourceAccess(RenderPassId::DdgiTrace,
                              {RenderResourceId::RayScene,
                               RenderResourceId::LightingData,
                               RenderResourceId::EnvironmentMaps},
                              {}, {RenderResourceId::DdgiRays});
  graph.addPass(RenderPassId::DdgiUpdate, {RenderPassId::DdgiTrace},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  if (p.services.ddgiManager)
                    p.services.ddgiManager->recordUpdate(cmd,
                                                         p.runtime.imageIndex);
                });
  graph.setPassResourceAccess(RenderPassId::DdgiUpdate,
                              {RenderResourceId::DdgiRays}, {},
                              {RenderResourceId::DdgiIrradiance});
}
} // namespace container::renderer
