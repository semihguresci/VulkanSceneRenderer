#include "Container/renderer/raytracing/RayShadowManager.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderGraph.h"
#include "Container/renderer/core/RendererMsaa.h"
#include "Container/renderer/raytracing/RaySceneAcceleration.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/Logger.h"

#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace container::renderer {
namespace {
constexpr uint32_t layers = 9;
struct ReadData {
  glm::uvec4 limits{0};
  glm::uvec4 options{0};
};
struct TraceData {
  container::gpu::CameraData camera;
  ReadData read;
  glm::mat4 previousViewProj{1}, previousInverseViewProj{1};
  glm::vec4 sectionPlane{0};
  container::gpu::SceneClipState boxClip;
  glm::uvec4 frame{0};
};
static_assert(sizeof(ReadData) == 32);
static_assert(offsetof(TraceData, read) == sizeof(container::gpu::CameraData));

struct Buffer {
  container::gpu::VulkanMemoryManager &memory;
  container::gpu::AllocatedBuffer value;
  VkDeviceSize size;
  Buffer(container::gpu::VulkanMemoryManager &memory, VkDeviceSize size,
         VkBufferUsageFlags usage)
      : memory(memory),
        value(memory.createBuffer(
            std::max<VkDeviceSize>(size, 16), usage,
            VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT)),
        size(std::max<VkDeviceSize>(size, 16)) {}
  ~Buffer() { memory.destroyBuffer(value); }
  void write(const void *data, VkDeviceSize bytes) {
    if (bytes > size || !value.allocation_info.pMappedData)
      throw std::runtime_error("invalid ray shadow upload");
    if (bytes)
      std::memcpy(value.allocation_info.pMappedData, data, size_t(bytes));
    if (vmaFlushAllocation(memory.allocator(), value.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("ray shadow upload flush failed");
  }
  uint64_t allocated() const { return value.allocation_info.size; }
};
struct Image {
  container::gpu::VulkanMemoryManager &memory;
  VkImage value{};
  VmaAllocation allocation{};
  VmaAllocationInfo allocationInfo{};
  vk::raii::ImageView view{nullptr};
  uint32_t layerCount;
  bool initialized{false};
  Image(const vk::raii::Device &device,
        container::gpu::VulkanMemoryManager &memory, VkExtent2D extent,
        VkFormat format, uint32_t count, bool array)
      : memory(memory), layerCount(count) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = count;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(memory.allocator(), &info, &alloc, &value, &allocation,
                       &allocationInfo) != VK_SUCCESS)
      throw std::runtime_error("ray shadow image allocation failed");
    try {
      vk::ImageViewCreateInfo create{};
      create.image = value;
      create.format = static_cast<vk::Format>(format);
      create.viewType =
          array ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
      create.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                 count};
      view = vk::raii::ImageView(device, create);
    } catch (...) {
      vmaDestroyImage(memory.allocator(), value, allocation);
      throw;
    }
  }
  ~Image() {
    view = vk::raii::ImageView(nullptr);
    vmaDestroyImage(memory.allocator(), value, allocation);
  }
};

void imageBarrier(VkCommandBuffer cmd, Image &image) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = image.initialized
                             ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                             : VK_PIPELINE_STAGE_2_NONE;
  barrier.srcAccessMask = image.initialized ? VK_ACCESS_2_MEMORY_READ_BIT |
                                                  VK_ACCESS_2_MEMORY_WRITE_BIT
                                            : 0;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                          VK_ACCESS_2_SHADER_WRITE_BIT |
                          VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.oldLayout =
      image.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
      VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.value;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                              image.layerCount};
  VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  info.imageMemoryBarrierCount = 1;
  info.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &info);
  image.initialized = true;
}

uint64_t hashBytes(uint64_t hash, const void *data, size_t bytes) {
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < bytes; ++i)
    hash = (hash ^ p[i]) * 1099511628211ull;
  return hash;
}
template <typename T> uint64_t hashValue(uint64_t hash, const T &value) {
  return hashBytes(hash, &value, sizeof(value));
}
} // namespace

struct RayShadowManager::Impl {
  std::shared_ptr<container::gpu::VulkanDevice> device;
  container::gpu::VulkanMemoryManager &memory;
  RaySceneAcceleration acceleration;
  RayShadowSettings settings;
  RendererMsaaDeviceSupport msaaSupport;
  VkExtent2D extent{};
  struct Generation {
    std::shared_ptr<const RaySceneGeneration> as;
    std::weak_ptr<const ExtractedRayScene> geometry;
    std::shared_ptr<const Buffer> triangles;
    std::unique_ptr<Buffer> instances;
    std::shared_ptr<const Buffer> surfaceTriangles;
    std::unique_ptr<Buffer> surfaceInstances;
    bool hasSurfaceData{false};
  };
  struct Slot {
    std::unique_ptr<Buffer> read, trace;
    std::shared_ptr<Generation> generation;
    VkDescriptorSet traceSet{};
    std::array<VkDescriptorSet, 4> filterSets{};
    VkDescriptorSet sceneSet{}, lightSet{};
    VkImageView depth{};
  };
  std::vector<Slot> slots;
  std::shared_ptr<Generation> current;
  RaySceneExtractionCache extractionCache;
  std::optional<RaySceneExtractionCache::Snapshot> pending;
  std::unique_ptr<Image> fallback, raw, visibility;
  std::array<std::unique_ptr<Image>, 2> history, surface;
  vk::raii::DescriptorSetLayout traceSetLayout{nullptr},
      filterSetLayout{nullptr};
  vk::raii::PipelineLayout traceLayout{nullptr}, filterLayout{nullptr};
  vk::raii::Pipeline tracePipeline{nullptr}, filterPipeline{nullptr};
  vk::raii::DescriptorPool pool{nullptr};
  std::vector<vk::raii::DescriptorSet> ownedSets;
  uint64_t sceneHash{0}, lightingHash{0}, frameId{0}, generationId{0},
      resets{0}, sceneRevision{0};
  VkDeviceSize maxStorageBufferRange{0};
  uint64_t surfaceBudgetBytes{128ull * 1024 * 1024};
  std::string surfaceUnsupported;
  std::optional<uint64_t> rejectedSurfaceScene;
  std::string rejectedSurfaceReason;
  bool validHistory{false}, recorded{false}, enabled{false},
      surfaceRequested{false}, surfaceEnabled{false}, sceneEnabled{false};
  glm::mat4 previousViewProj{1}, previousInverseViewProj{1};
  TraceData data{};

  Impl(std::shared_ptr<container::gpu::VulkanDevice> device,
       container::gpu::VulkanMemoryManager &memory)
      : device(std::move(device)), memory(memory),
        acceleration(*this->device, memory),
        msaaSupport(
            queryRendererMsaaDeviceSupport(this->device->physicalDevice())) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(this->device->physicalDevice(), &properties);
    maxStorageBufferRange = properties.limits.maxStorageBufferRange;
  }
  VkDeviceSize storageBytes(size_t count, size_t stride) const {
    if (!stride || count > maxStorageBufferRange / stride ||
        maxStorageBufferRange < 16)
      throw std::length_error(
          "ray scene metadata exceeds storage buffer range");
    return VkDeviceSize(count) * stride;
  }
  std::string
  surfaceBudgetFailure(std::span<const RaySceneProviderSource> sources) const {
    uint64_t triangles = 0, instances = 0;
    for (const auto &source : sources) {
      std::unordered_set<uint64_t> ranges;
      for (const auto &draw : source.draws) {
        if (!draw.indexCount || !draw.instanceCount)
          continue;
        // This is the same provider-local deduplication used by extraction.
        const auto id = (uint64_t(draw.firstIndex) << 32) | draw.indexCount;
        if (ranges.insert(id).second)
          triangles += draw.indexCount / 3u;
        instances += draw.instanceCount;
        // Check before multiplication; bounded counts cannot overflow the sum
        // or the byte calculations even for malformed enormous provider lists.
        if (triangles >
                maxStorageBufferRange / sizeof(RaySurfaceTriangleData) ||
            instances >
                maxStorageBufferRange / sizeof(RaySurfaceInstanceData) ||
            maxStorageBufferRange < 16)
          return "DDGI surface metadata exceeds device maxStorageBufferRange";
        const auto bytes =
            std::max<uint64_t>(triangles * sizeof(RaySurfaceTriangleData), 16) +
            std::max<uint64_t>(instances * sizeof(RaySurfaceInstanceData), 16);
        if (bytes > surfaceBudgetBytes)
          return "DDGI surface metadata exceeds configured surface budget";
      }
    }
    if (surfaceBudgetBytes < 32)
      return "DDGI surface metadata exceeds configured surface budget";
    return {};
  }
  void rejectSurface(std::string reason) {
    if (reason != surfaceUnsupported)
      container::log::ContainerLogger::instance().renderer()->warn(
          "Ray surface scene unavailable: {}", reason);
    surfaceUnsupported = std::move(reason);
    surfaceEnabled = false;
  }
  void invalidate() {
    validHistory = false;
    ++resets;
  }
  void ensureImages() {
    if (raw)
      return;
    const auto &vk = device->raii();
    raw = std::make_unique<Image>(vk, memory, extent, VK_FORMAT_R32_SFLOAT,
                                  layers, true);
    visibility = std::make_unique<Image>(vk, memory, extent,
                                         VK_FORMAT_R32_SFLOAT, layers, true);
    for (uint32_t i = 0; i < 2; ++i) {
      history[i] = std::make_unique<Image>(vk, memory, extent,
                                           VK_FORMAT_R32_SFLOAT, layers, true);
      surface[i] = std::make_unique<Image>(
          vk, memory, extent, VK_FORMAT_R32G32B32A32_SFLOAT, 1, false);
    }
    invalidate();
  }
  void imageDescriptor(VkDescriptorSet set, uint32_t binding, VkImageView view,
                       VkDescriptorType type) {
    vk::DescriptorImageInfo image({}, view, vk::ImageLayout::eGeneral);
    vk::WriteDescriptorSet write(set, binding, 0, 1,
                                 static_cast<vk::DescriptorType>(type), &image);
    device->raii().updateDescriptorSets(write, {});
  }
  void bufferDescriptor(VkDescriptorSet set, uint32_t binding,
                        const Buffer &buffer, VkDescriptorType type) {
    vk::DescriptorBufferInfo info(buffer.value.buffer, 0, buffer.size);
    vk::WriteDescriptorSet write(
        set, binding, 0, 1, static_cast<vk::DescriptorType>(type), {}, &info);
    device->raii().updateDescriptorSets(write, {});
  }
  void traceDescriptors(uint32_t image) {
    if (!enabled)
      return;
    auto &slot = slots.at(image);
    if (!slot.generation)
      return;
    auto &generation = *slot.generation;
    vk::AccelerationStructureKHR as(generation.as->tlas());
    vk::WriteDescriptorSetAccelerationStructureKHR accelerationWrite(1, &as);
    vk::WriteDescriptorSet write;
    write.dstSet = slot.traceSet;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
    write.pNext = &accelerationWrite;
    device->raii().updateDescriptorSets(write, {});
    bufferDescriptor(slot.traceSet, 0, *slot.trace,
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    bufferDescriptor(slot.traceSet, 2, *generation.triangles,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bufferDescriptor(slot.traceSet, 3, *generation.instances,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    // Depth retains the engine's depth-read-only/stencil-attachment layout.
    vk::DescriptorImageInfo depth(
        {}, slot.depth,
        vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal);
    write = vk::WriteDescriptorSet(slot.traceSet, 4, 0, 1,
                                   vk::DescriptorType::eSampledImage, &depth);
    device->raii().updateDescriptorSets(write, {});
    imageDescriptor(slot.traceSet, 5, *raw->view,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    imageDescriptor(slot.traceSet, 6, *surface[frameId & 1]->view,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
  }
};

RayShadowManager::RayShadowManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::VulkanMemoryManager &memory)
    : impl_(std::make_unique<Impl>(std::move(device), memory)) {}
RayShadowManager::~RayShadowManager() = default;
RayShadowSettings &RayShadowManager::settings() { return impl_->settings; }
bool RayShadowManager::supported() const {
  return impl_->acceleration.supported();
}
bool RayShadowManager::active() const { return impl_->enabled; }
void RayShadowManager::requestSurfaceScene(bool requested,
                                           uint64_t budgetBytes) {
  impl_->surfaceRequested = requested;
  impl_->surfaceBudgetBytes = budgetBytes;
}
VkBuffer RayShadowManager::readSettings(uint32_t image) const {
  return impl_->slots.at(image).read->value.buffer;
}
VkDeviceSize RayShadowManager::readSettingsSize() const {
  return sizeof(ReadData);
}
VkImageView RayShadowManager::visibilityView() const {
  const auto &image = impl_->visibility ? impl_->visibility : impl_->fallback;
  return image ? static_cast<VkImageView>(*image->view) : VK_NULL_HANDLE;
}
VkDescriptorSetLayout RayShadowManager::traceDescriptorLayout() const {
  return supported()
             ? static_cast<VkDescriptorSetLayout>(*impl_->traceSetLayout)
             : VK_NULL_HANDLE;
}
VkDescriptorSet RayShadowManager::traceDescriptorSet(uint32_t image) const {
  return active() && image < impl_->slots.size() ? impl_->slots[image].traceSet
                                                 : VK_NULL_HANDLE;
}
RaySurfaceGpuBindings RayShadowManager::surfaceBindings(uint32_t image) const {
  const auto &p = *impl_;
  if (!p.sceneEnabled || !p.surfaceEnabled || image >= p.slots.size())
    return {};
  const auto &generation = p.slots[image].generation;
  if (!generation || !generation->hasSurfaceData)
    return {};
  return {generation->as->tlas(),
          generation->triangles->value.buffer,
          generation->triangles->size,
          generation->instances->value.buffer,
          generation->instances->size,
          generation->surfaceTriangles->value.buffer,
          generation->surfaceTriangles->size,
          generation->surfaceInstances->value.buffer,
          generation->surfaceInstances->size,
          uint32_t(generation->triangles->size / sizeof(RayTriangleData)),
          generation->as->stats().instanceCount};
}
const std::string &RayShadowManager::surfaceUnsupportedReason() const {
  return impl_->surfaceUnsupported;
}
uint64_t RayShadowManager::sceneGeneration() const {
  return impl_->generationId;
}
uint64_t RayShadowManager::sceneRevision() const {
  return impl_->sceneRevision;
}
uint64_t RayShadowManager::historyResets() const { return impl_->resets; }
const RaySceneBuildStats *RayShadowManager::buildStats() const {
  return impl_->current ? &impl_->current->as->stats() : nullptr;
}

void RayShadowManager::createPipelines(const std::filesystem::path &root,
                                       VkDescriptorSetLayout scene,
                                       VkDescriptorSetLayout light) {
  auto &p = *impl_;
  if (!supported() || *p.tracePipeline)
    return;
  const auto &vk = p.device->raii();
  std::vector<vk::DescriptorSetLayoutBinding> bindings;
  for (uint32_t i = 0; i < 7; ++i) {
    const auto type = i == 0   ? vk::DescriptorType::eUniformBuffer
                      : i == 1 ? vk::DescriptorType::eAccelerationStructureKHR
                      : i < 4  ? vk::DescriptorType::eStorageBuffer
                      : i == 4 ? vk::DescriptorType::eSampledImage
                               : vk::DescriptorType::eStorageImage;
    const auto stages =
        i < 4 ? vk::ShaderStageFlagBits::eCompute |
                    vk::ShaderStageFlagBits::eFragment
              : vk::ShaderStageFlags(vk::ShaderStageFlagBits::eCompute);
    bindings.emplace_back(i, type, 1, stages);
  }
  p.traceSetLayout = vk::raii::DescriptorSetLayout(
      vk, vk::DescriptorSetLayoutCreateInfo({}, bindings));
  bindings.clear();
  for (uint32_t i = 0; i < 6; ++i)
    bindings.emplace_back(i,
                          i == 0   ? vk::DescriptorType::eUniformBuffer
                          : i == 5 ? vk::DescriptorType::eStorageImage
                                   : vk::DescriptorType::eSampledImage,
                          1, vk::ShaderStageFlagBits::eCompute);
  p.filterSetLayout = vk::raii::DescriptorSetLayout(
      vk, vk::DescriptorSetLayoutCreateInfo({}, bindings));
  const std::array layouts{vk::DescriptorSetLayout(scene),
                           vk::DescriptorSetLayout(light), *p.traceSetLayout};
  p.traceLayout =
      vk::raii::PipelineLayout(vk, vk::PipelineLayoutCreateInfo({}, layouts));
  const vk::PushConstantRange push(vk::ShaderStageFlagBits::eCompute, 0, 4);
  p.filterLayout = vk::raii::PipelineLayout(
      vk, vk::PipelineLayoutCreateInfo({}, *p.filterSetLayout, push));
  auto compute = [&](const char *name, const vk::raii::PipelineLayout &layout) {
    const auto bytes = container::util::readFile(root / "spv_shaders" / name);
    // readFile byte storage may be unaligned; copy into the SPIR-V word ABI.
    if (bytes.size() % 4)
      throw std::runtime_error("invalid ray shadow SPIR-V");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    vk::raii::ShaderModule module(
        vk, vk::ShaderModuleCreateInfo({}, words.size() * 4, words.data()));
    vk::PipelineShaderStageCreateInfo stage(
        {}, vk::ShaderStageFlagBits::eCompute, *module, "computeMain");
    return vk::raii::Pipeline(
        vk, nullptr, vk::ComputePipelineCreateInfo({}, stage, *layout));
  };
  p.tracePipeline = compute("ray_shadow_trace.comp.spv", p.traceLayout);
  p.filterPipeline = compute("ray_shadow_filter.comp.spv", p.filterLayout);
}

void RayShadowManager::prepare(VkExtent2D extent, uint32_t imageCount) {
  auto &p = *impl_;
  if (!extent.width || !extent.height || !imageCount)
    throw std::invalid_argument("invalid ray shadow extent");
  // Caller has retired all frames before replacing images/descriptor pools.
  p.ownedSets.clear();
  p.pool = vk::raii::DescriptorPool(nullptr);
  p.slots.clear();
  p.current.reset();
  p.pending.reset();
  p.extractionCache.clear();
  p.raw.reset();
  p.visibility.reset();
  for (auto &image : p.history)
    image.reset();
  for (auto &image : p.surface)
    image.reset();
  p.extent = extent;
  p.frameId = 0;
  p.sceneHash = p.lightingHash = p.sceneRevision = 0;
  p.enabled = p.sceneEnabled = p.surfaceEnabled = false;
  p.surfaceUnsupported.clear();
  p.rejectedSurfaceScene.reset();
  p.rejectedSurfaceReason.clear();
  p.recorded = false;
  p.invalidate();
  if (!p.fallback)
    p.fallback =
        std::make_unique<Image>(p.device->raii(), p.memory, VkExtent2D{1, 1},
                                VK_FORMAT_R32_SFLOAT, 1, true);
  p.slots.resize(imageCount);
  for (auto &slot : p.slots) {
    slot.read = std::make_unique<Buffer>(p.memory, sizeof(ReadData),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    slot.trace = std::make_unique<Buffer>(p.memory, sizeof(TraceData),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    ReadData zero{};
    slot.read->write(&zero, sizeof(zero));
  }
  if (!supported())
    return;
  const std::array sizes{
      vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer,
                             imageCount * 5),
      vk::DescriptorPoolSize(vk::DescriptorType::eAccelerationStructureKHR,
                             imageCount),
      vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer,
                             imageCount * 2),
      vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage,
                             imageCount * 17),
      vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage,
                             imageCount * 6)};
  p.pool = vk::raii::DescriptorPool(
      p.device->raii(),
      vk::DescriptorPoolCreateInfo(
          vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, imageCount * 5,
          sizes));
  std::vector<vk::DescriptorSetLayout> layouts;
  for (uint32_t i = 0; i < imageCount; ++i) {
    layouts.push_back(*p.traceSetLayout);
    for (uint32_t j = 0; j < 4; ++j)
      layouts.push_back(*p.filterSetLayout);
  }
  p.ownedSets = p.device->raii().allocateDescriptorSets(
      vk::DescriptorSetAllocateInfo(*p.pool, layouts));
  for (uint32_t i = 0; i < imageCount; ++i) {
    p.slots[i].traceSet = *p.ownedSets[i * 5];
    for (uint32_t j = 0; j < 4; ++j)
      p.slots[i].filterSets[j] = *p.ownedSets[i * 5 + j + 1];
  }
}

void RayShadowManager::update(
    uint32_t image, const container::gpu::CameraData &camera,
    const FrameResources &frame, VkDescriptorSet sceneSet,
    VkDescriptorSet lightSet, std::span<const RaySceneProviderSource> sources,
    const container::gpu::LightingData &lighting,
    std::span<const container::gpu::PointLightData> points,
    std::span<const container::gpu::AreaLightData> areas,
    uint32_t sectionPlaneEnabled, glm::vec4 sectionPlane,
    const container::gpu::SceneClipState &boxClip,
    const std::function<uint64_t(uint32_t)> &materialRevision,
    bool geometryCompatible) {
  auto &p = *impl_;
  p.settings = sanitizeRayShadowSettings(p.settings);
  const bool hasRayGeometry = std::any_of(
      sources.begin(), sources.end(), [](const auto &source) {
        return std::any_of(source.draws.begin(), source.draws.end(),
                           [](const auto &draw) {
                             return draw.indexCount != 0u &&
                                    draw.instanceCount != 0u;
                           });
      });
  p.enabled = hasRayGeometry && supported() && geometryCompatible &&
              p.settings.mode != RayShadowMode::Raster && *p.tracePipeline;
  p.surfaceEnabled = p.surfaceRequested && hasRayGeometry;
  if (!hasRayGeometry) {
    // Unloading or filtering out every triangle is a normal empty scene, not
    // an optional-surface failure. Keep requests/modes for the next loaded model.
    p.rejectedSurfaceScene.reset();
    p.rejectedSurfaceReason.clear();
    p.surfaceUnsupported.clear();
  } else if (p.surfaceRequested) {
    if (!supported())
      p.rejectSurface("DDGI requires supported and enabled ray queries");
    else if (!geometryCompatible)
      p.rejectSurface("DDGI ray scene is incompatible with active geometry");
    else if (auto reason = p.surfaceBudgetFailure(sources); !reason.empty())
      p.rejectSurface(std::move(reason));
    else if (!p.rejectedSurfaceScene)
      p.surfaceUnsupported.clear();
  } else {
    p.surfaceUnsupported.clear();
  }
  p.sceneEnabled =
      supported() && geometryCompatible && (p.enabled || p.surfaceEnabled);
  auto &slot = p.slots.at(image);
  p.data = {};
  p.data.camera = camera;
  p.data.camera.renderExtent = {float(p.extent.width), float(p.extent.height),
                                1.0f / p.extent.width, 1.0f / p.extent.height};
  p.data.read.limits = {
      p.enabled ? 1u : 0u,
      std::min({uint32_t(points.size()), 4u, p.settings.localLightBudget}),
      std::min({uint32_t(areas.size()), 4u,
                p.settings.localLightBudget > 4u
                    ? p.settings.localLightBudget - 4u
                    : 0u}),
      p.settings.areaSamples};
  // Spend unused point slots on area lights without exceeding either 4-light
  // cap.
  p.data.read.limits.z =
      std::min({uint32_t(areas.size()), 4u,
                p.settings.localLightBudget - p.data.read.limits.y});
  const uint32_t samples =
      sampleCountToSamples(frame.depthStencilMsaa.image != VK_NULL_HANDLE
                               ? frame.depthStencilMsaa.samples
                               : frame.depthStencil.samples);
  const auto resolveMode =
      preferredDepthResolveMode(p.msaaSupport.depthResolveModes);
  // The shader may verify same-receiver coverage at the standard Vulkan sample
  // positions. Unknown patterns/counts keep conservative fragment queries.
  const uint32_t receiverSamples =
      p.msaaSupport.standardSampleLocations && samples >= 2u &&
              samples <= 16u &&
              (resolveMode == VK_RESOLVE_MODE_MAX_BIT ||
               resolveMode == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
          ? samples |
                (resolveMode == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT ? 0x100u : 0u)
          : 0u;
  p.data.read.options = {p.settings.denoise ? 1u : 0u, p.settings.debugLayer,
                         p.settings.mode == RayShadowMode::Soft ? 1u : 0u,
                         receiverSamples};
  const auto retireDisabledScene = [&] {
    if (p.validHistory)
      p.invalidate();
    p.sceneHash = p.sceneRevision = 0;
    p.lightingHash = 0;
    p.pending.reset();
    // Older image slots still retain submitted compact or surface generations
    // until their fences retire. Unload drops only idle current/cache references
    // and this already-retired image; never retain a stale compact buildStats.
    p.current.reset();
    p.extractionCache.clear();
    slot.read->write(&p.data.read, sizeof(ReadData));
    slot.generation.reset();
  };
  if (!p.sceneEnabled) {
    retireDisabledScene();
    return;
  }
  p.data.previousViewProj = p.previousViewProj;
  p.data.previousInverseViewProj = p.previousInverseViewProj;
  p.data.sectionPlane = sectionPlane;
  p.data.boxClip = boxClip;
  uint64_t sceneHash = 1469598103934665603ull;
  sceneHash = hashValue(sceneHash, sources.size());
  for (const auto &source : sources) {
    sceneHash = hashValue(sceneHash, source.provider.value.size());
    sceneHash = hashBytes(sceneHash, source.provider.value.data(),
                          source.provider.value.size());
    sceneHash = hashValue(sceneHash, source.geometryRevision);
    sceneHash = hashValue(sceneHash, source.vertices.size());
    sceneHash = hashValue(sceneHash, source.indices.size());
    sceneHash = hashValue(sceneHash,
                          reinterpret_cast<uintptr_t>(source.vertices.data()));
    sceneHash = hashValue(sceneHash,
                          reinterpret_cast<uintptr_t>(source.indices.data()));
    sceneHash = hashValue(sceneHash, source.draws.size());
    for (const auto &draw : source.draws) {
      sceneHash = hashValue(sceneHash, draw.objectIndex);
      sceneHash = hashValue(sceneHash, draw.firstIndex);
      sceneHash = hashValue(sceneHash, draw.indexCount);
      sceneHash = hashValue(sceneHash, draw.instanceCount);
      if (draw.objectIndex > source.objects.size() ||
          draw.instanceCount > source.objects.size() - draw.objectIndex)
        throw std::invalid_argument("ray shadow object range is invalid");
      for (uint32_t i = 0; i < draw.instanceCount; ++i) {
        const auto &object = source.objects[draw.objectIndex + i];
        sceneHash = hashValue(sceneHash, object.model);
        sceneHash = hashValue(sceneHash, object.objectInfo);
        if (materialRevision)
          sceneHash =
              hashValue(sceneHash, materialRevision(object.objectInfo.x));
      }
    }
  }
  if (p.surfaceEnabled) {
    if (p.rejectedSurfaceScene == sceneHash) {
      // Repeating a failed optional extraction every frame would rebuild the
      // compact shadow generation and discard otherwise reusable CPU data.
      // Geometry, topology, transforms and material revisions all enter this
      // hash, so repaired inputs get another attempt.
      p.rejectSurface(p.rejectedSurfaceReason);
      p.sceneEnabled = p.enabled;
      if (!p.sceneEnabled) {
        retireDisabledScene();
        return;
      }
    } else {
      p.rejectedSurfaceScene.reset();
      p.rejectedSurfaceReason.clear();
      p.surfaceUnsupported.clear();
    }
  }
  uint64_t lightHash = hashValue(1469598103934665603ull, p.data.read);
  lightHash = hashValue(lightHash, sectionPlaneEnabled);
  lightHash = hashValue(lightHash, sectionPlane);
  lightHash = hashValue(lightHash, boxClip);
  lightHash = hashValue(lightHash, lighting.directionalDirection);
  lightHash = hashValue(lightHash, lighting.directionalColorIntensity);
  for (const auto &light : points)
    lightHash = hashValue(lightHash, light);
  for (const auto &light : areas)
    lightHash = hashValue(lightHash, light);
  if ((p.enabled &&
       (sceneHash != p.sceneHash || lightHash != p.lightingHash)) ||
      (!p.enabled && p.validHistory))
    p.invalidate();
  if (!p.current || sceneHash != p.sceneHash ||
      p.current->hasSurfaceData != p.surfaceEnabled) {
    try {
      p.pending = p.extractionCache.update(sources, {p.surfaceEnabled});
    } catch (const std::exception &error) {
      if (!p.surfaceEnabled)
        throw;
      // Validate the required compact scene as well. Invalid positions/index
      // ranges/transforms still propagate; only optional surface failures may
      // fall back. The extraction cache publishes snapshots atomically.
      auto compact = p.extractionCache.update(sources);
      p.rejectSurface(std::string("DDGI surface metadata unavailable: ") +
                      error.what());
      p.rejectedSurfaceScene = sceneHash;
      p.rejectedSurfaceReason = p.surfaceUnsupported;
      p.sceneEnabled = p.enabled;
      if (!p.sceneEnabled) {
        p.current.reset();
        p.extractionCache.clear();
        retireDisabledScene();
        return;
      }
      p.pending = std::move(compact);
    }
  }
  p.sceneHash = sceneHash;
  // Scene clipping is semantic visibility even though candidate acceptance,
  // rather than AS topology, applies it. Camera motion and lighting changes
  // must not discard accumulated probe irradiance.
  p.sceneRevision = hashValue(sceneHash, sectionPlaneEnabled);
  p.sceneRevision = hashValue(p.sceneRevision, sectionPlane);
  p.sceneRevision = hashValue(p.sceneRevision, boxClip);
  p.lightingHash = lightHash;
  p.data.frame = {p.validHistory ? 1u : 0u, uint32_t(p.frameId),
                  sectionPlaneEnabled, 0u};
  slot.read->write(&p.data.read, sizeof(ReadData));
  slot.trace->write(&p.data, sizeof(TraceData));
  if (!p.enabled) {
    return;
  }
  p.ensureImages();
  slot.sceneSet = sceneSet;
  slot.lightSet = lightSet;
  slot.depth = frame.depthSamplingView;
  if (!slot.depth || !slot.sceneSet || !slot.lightSet)
    throw std::runtime_error("ray shadow frame inputs unavailable");
  // Updating this image's descriptors is safe after its previous fence retires.
  for (uint32_t parity = 0; parity < 2; ++parity)
    for (uint32_t spatial = 0; spatial < 2; ++spatial) {
      const auto set = slot.filterSets[parity * 2 + spatial];
      p.bufferDescriptor(set, 0, *slot.trace,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
      p.imageDescriptor(set, 1, *(spatial ? p.history[parity] : p.raw)->view,
                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
      p.imageDescriptor(set, 2, *p.history[1 - parity]->view,
                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
      p.imageDescriptor(set, 3, *p.surface[parity]->view,
                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
      p.imageDescriptor(set, 4, *p.surface[1 - parity]->view,
                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
      p.imageDescriptor(set, 5,
                        *(spatial ? p.visibility : p.history[parity])->view,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    }
}

void RayShadowManager::recordBuild(VkCommandBuffer cmd, uint32_t image) {
  auto &p = *impl_;
  if (!p.fallback->initialized) {
    imageBarrier(cmd, *p.fallback);
    VkClearColorValue white{{1, 1, 1, 1}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, p.fallback->value, VK_IMAGE_LAYOUT_GENERAL,
                         &white, 1, &range);
    imageBarrier(cmd, *p.fallback);
  }
  if (!p.sceneEnabled)
    return;
  if (p.pending) {
    auto generation = std::make_shared<Impl::Generation>();
    generation->geometry = p.pending->geometry;
    const auto &geometry = *p.pending->geometry;
    // Validate every descriptor range before allocating or recording AS work.
    const auto triangleBytes =
        p.storageBytes(geometry.triangles.size(), sizeof(RayTriangleData));
    const auto instanceBytes = p.storageBytes(
        p.pending->instances.instanceData.size(), sizeof(RayInstanceData));
    const auto surfaceTriangleBytes =
        geometry.hasSurfaceData
            ? p.storageBytes(geometry.surfaceTriangles.size(),
                             sizeof(RaySurfaceTriangleData))
            : 0;
    const auto surfaceInstanceBytes =
        geometry.hasSurfaceData
            ? p.storageBytes(p.pending->instances.surfaceInstanceData.size(),
                             sizeof(RaySurfaceInstanceData))
            : 0;
    if (geometry.hasSurfaceData &&
        (geometry.surfaceTriangles.size() != geometry.triangles.size() ||
         p.pending->instances.surfaceInstanceData.size() !=
             p.pending->instances.instanceData.size()))
      throw std::runtime_error("incomplete ray scene surface metadata");
    generation->hasSurfaceData = geometry.hasSurfaceData;
    // Triangle UV metadata is immutable with the compact geometry. Share it
    // through the same fence-retained generations as reused BLAS allocations.
    if (p.current && p.current->geometry.lock() == p.pending->geometry) {
      generation->triangles = p.current->triangles;
    } else {
      auto triangles = std::make_shared<Buffer>(
          p.memory, triangleBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      triangles->write(geometry.triangles.data(), triangleBytes);
      generation->triangles = std::move(triangles);
    }
    generation->instances = std::make_unique<Buffer>(
        p.memory, instanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    generation->instances->write(p.pending->instances.instanceData.data(),
                                 instanceBytes);
    if (geometry.hasSurfaceData) {
      if (p.current && p.current->hasSurfaceData &&
          p.current->geometry.lock() == p.pending->geometry) {
        generation->surfaceTriangles = p.current->surfaceTriangles;
      } else {
        auto triangles = std::make_shared<Buffer>(
            p.memory, surfaceTriangleBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        triangles->write(geometry.surfaceTriangles.data(),
                         surfaceTriangleBytes);
        generation->surfaceTriangles = std::move(triangles);
      }
      generation->surfaceInstances = std::make_unique<Buffer>(
          p.memory, surfaceInstanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      generation->surfaceInstances->write(
          p.pending->instances.surfaceInstanceData.data(),
          surfaceInstanceBytes);
    }
    const auto views = geometry.geometryViews();
    // The command buffer belongs to CommandBufferManager. Release the temporary
    // Vulkan-Hpp wrapper on both success and failure; it must never free it.
    vk::raii::CommandBuffer borrowed(p.device->raii(), cmd, VK_NULL_HANDLE);
    try {
      generation->as = p.acceleration.recordBuild(
          borrowed, {views, p.pending->instances.instances},
          p.current ? p.current->as : nullptr);
      (void)borrowed.release();
    } catch (...) {
      (void)borrowed.release();
      throw;
    }
    p.current = std::move(generation);
    p.pending.reset();
    ++p.generationId;
  }
  p.slots.at(image).generation = p.current;
  p.traceDescriptors(image);
}

void RayShadowManager::recordTrace(VkCommandBuffer cmd, uint32_t image) {
  auto &p = *impl_;
  if (!p.enabled)
    return;
  imageBarrier(cmd, *p.raw);
  imageBarrier(cmd, *p.surface[p.frameId & 1]);
  // The unused previous history must still have a legal sampled image layout.
  for (auto &history : p.history)
    imageBarrier(cmd, *history);
  imageBarrier(cmd, *p.surface[1 - (p.frameId & 1)]);
  const auto &slot = p.slots.at(image);
  const std::array sets{slot.sceneSet, slot.lightSet, slot.traceSet};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, *p.tracePipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, *p.traceLayout,
                          0, 3, sets.data(), 0, nullptr);
  vkCmdDispatch(cmd, (p.extent.width + 7) / 8, (p.extent.height + 7) / 8,
                layers);
  imageBarrier(cmd, *p.raw);
  imageBarrier(cmd, *p.surface[p.frameId & 1]);
}

void RayShadowManager::recordFilter(VkCommandBuffer cmd, uint32_t image) {
  auto &p = *impl_;
  if (!p.enabled)
    return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, *p.filterPipeline);
  for (uint32_t spatial = 0; spatial < 2; ++spatial) {
    Image &output = *(spatial ? p.visibility : p.history[p.frameId & 1]);
    imageBarrier(cmd, output);
    const auto set =
        p.slots.at(image).filterSets[(p.frameId & 1) * 2 + spatial];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            *p.filterLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, *p.filterLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(spatial), &spatial);
    vkCmdDispatch(cmd, (p.extent.width + 7) / 8, (p.extent.height + 7) / 8,
                  layers);
    imageBarrier(cmd, output);
  }
  p.recorded = true;
}
void RayShadowManager::commit() {
  auto &p = *impl_;
  if (p.recorded) {
    p.validHistory = true;
    p.previousViewProj = p.data.camera.viewProj;
    p.previousInverseViewProj = p.data.camera.inverseViewProj;
    ++p.frameId;
  }
  p.recorded = false;
}
uint64_t RayShadowManager::allocatedBytes() const {
  const auto &p = *impl_;
  uint64_t total = 0;
  for (const auto &slot : p.slots)
    total += slot.read->allocated() + slot.trace->allocated();
  auto add = [&](const auto &image) {
    if (image)
      total += image->allocationInfo.size;
  };
  add(p.fallback);
  add(p.raw);
  add(p.visibility);
  for (const auto &image : p.history)
    add(image);
  for (const auto &image : p.surface)
    add(image);
  // Count shared live metadata generations once rather than once per frame.
  std::vector<const Impl::Generation *> seen;
  std::unordered_set<const Buffer *> triangles;
  std::vector<std::shared_ptr<const RaySceneGeneration>> acceleration;
  auto generation = [&](const auto &entry) {
    if (!entry ||
        std::find(seen.begin(), seen.end(), entry.get()) != seen.end())
      return;
    seen.push_back(entry.get());
    if (triangles.insert(entry->triangles.get()).second)
      total += entry->triangles->allocated();
    total += entry->instances->allocated();
    if (entry->surfaceTriangles &&
        triangles.insert(entry->surfaceTriangles.get()).second)
      total += entry->surfaceTriangles->allocated();
    if (entry->surfaceInstances)
      total += entry->surfaceInstances->allocated();
    acceleration.push_back(entry->as);
  };
  generation(p.current);
  for (const auto &slot : p.slots)
    generation(slot.generation);
  return total + RaySceneGeneration::retainedAllocatedBytes(acceleration);
}

void registerRayShadowPasses(RenderGraphBuilder &graph) {
  graph.addPass(RenderPassId::RaySceneBuild, {},
                [](VkCommandBuffer cmd, const FrameRecordParams &frame) {
                  if (frame.services.rayShadowManager)
                    frame.services.rayShadowManager->recordBuild(
                        cmd, frame.runtime.imageIndex);
                });
  graph.setPassResourceAccess(
      RenderPassId::RaySceneBuild,
      {RenderResourceId::SceneGeometry, RenderResourceId::BimGeometry}, {},
      {RenderResourceId::RayScene});
  graph.addPass(RenderPassId::RayShadowTrace,
                {RenderPassId::RaySceneBuild, RenderPassId::DepthToReadOnly},
                [](VkCommandBuffer cmd, const FrameRecordParams &frame) {
                  if (frame.services.rayShadowManager)
                    frame.services.rayShadowManager->recordTrace(
                        cmd, frame.runtime.imageIndex);
                });
  graph.setPassResourceAccess(
      RenderPassId::RayShadowTrace,
      {RenderResourceId::RayScene, RenderResourceId::SceneDepth,
       RenderResourceId::CameraBuffer, RenderResourceId::LightingData},
      {}, {RenderResourceId::RayShadowRaw});
  graph.addPass(RenderPassId::RayShadowFilter, {RenderPassId::RayShadowTrace},
                [](VkCommandBuffer cmd, const FrameRecordParams &frame) {
                  if (frame.services.rayShadowManager)
                    frame.services.rayShadowManager->recordFilter(
                        cmd, frame.runtime.imageIndex);
                });
  graph.setPassResourceAccess(RenderPassId::RayShadowFilter,
                              {RenderResourceId::RayShadowRaw}, {},
                              {RenderResourceId::RayShadowVisibility});
}
} // namespace container::renderer
