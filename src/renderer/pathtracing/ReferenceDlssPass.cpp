#include "ReferenceDlssPass.h"

#include <Container/renderer/pathtracing/ReferenceDlssReconstructor.h>
#include <Container/renderer/temporal/TemporalState.h>
#include <Container/utility/Platform.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {

constexpr std::array guideFormats{VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT};
constexpr VkFormat outputFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

struct alignas(16) GuideUniform {
  glm::mat4 currentViewProj{1}, previousViewProj{1};
  glm::vec4 jitter{};
  glm::uvec4 extentSequence{};
};
static_assert(sizeof(GuideUniform) == 160);
static_assert(offsetof(GuideUniform, extentSequence) == 144);

void require(VkResult result, const char* message) {
  if (result != VK_SUCCESS) throw std::runtime_error(message);
}

std::array<float, 16> rowMajor(const glm::mat4& matrix) {
  std::array<float, 16> elements{};
  for (uint32_t row = 0; row < 4; ++row)
    for (uint32_t column = 0; column < 4; ++column)
      elements[4 * row + column] = matrix[column][row];
  return elements;
}

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
  return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, srcStage, srcAccess,
      dstStage, dstAccess, oldLayout, newLayout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
      image, colorRange};
}

void transition(VkCommandBuffer command, std::span<const VkImageMemoryBarrier2> barriers) {
  VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  info.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
  info.pImageMemoryBarriers = barriers.data();
  vkCmdPipelineBarrier2(command, &info);
}

class GuideImage {
 public:
  GuideImage(gpu::VulkanDevice& device, gpu::VulkanMemoryManager& memory, VkExtent2D extent,
      VkFormat format, VkImageUsageFlags usage) : memory_(memory) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    require(vmaCreateImage(memory.allocator(), &info, &allocationInfo, &image, &allocation_, nullptr),
        "DLSS RR image allocation failed");
    try {
      vk::ImageViewCreateInfo viewInfo{};
      viewInfo.image = image;
      viewInfo.viewType = vk::ImageViewType::e2D;
      viewInfo.format = static_cast<vk::Format>(format);
      viewInfo.subresourceRange = colorRange;
      view_ = vk::raii::ImageView(device.raii(), viewInfo);
    } catch (...) {
      vmaDestroyImage(memory.allocator(), image, allocation_);
      throw;
    }
    resource = {image, *view_, format, extent};
  }
  ~GuideImage() {
    view_.clear();
    vmaDestroyImage(memory_.allocator(), image, allocation_);
  }
  GuideImage(const GuideImage&) = delete;
  GuideImage& operator=(const GuideImage&) = delete;
  uint64_t bytes() const {
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(memory_.allocator(), allocation_, &info);
    return info.size;
  }
  DlssImage resource;
  VkImage image{};

 private:
  gpu::VulkanMemoryManager& memory_;
  VmaAllocation allocation_{};
  vk::raii::ImageView view_{nullptr};
};

} // namespace

struct ReferenceDlssPass::Impl {
  struct History {
    ReferencePathTraceData trace;
    glm::mat4 viewProj{1};
    RaySurfaceGpuBindings scene;
    RaySurfaceMotionSnapshot motion;
    uint64_t revision{};
  };
  struct Slot {
    VkDescriptorSet set{};
    VkCommandBuffer secondary{};
    gpu::AllocatedBuffer uniform{};
    std::shared_ptr<const void> previousLifetime;
    uint64_t timedSerial{};
  };

  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::VulkanMemoryManager& memory;
  gpu::PipelineManager& pipelines;
  std::vector<Slot> slots;
  std::array<std::unique_ptr<GuideImage>, guideFormats.size()> guides;
  std::unique_ptr<GuideImage> output;
  std::unique_ptr<ReferenceDlssReconstructor> sdk;
  gpu::ComputePipeline fullPipeline{}, emptyPipeline{}, composePipeline{};
  VkDescriptorSetLayout guideLayout{}, composeSetLayout{};
  VkPipelineLayout layout{}, composeLayout{};
  VkDescriptorSet composeSet{};
  VkDescriptorPool pool{};
  VkCommandPool commandPool{};
  VkQueryPool queries{};
  VkExtent2D renderExtent{}, outputExtent{};
  uint64_t allocated{}, resets{}, historyFrames{}, submitted{}, lastTimedSerial{};
  uint32_t timestampBits{};
  float timestampPeriod{};
  std::optional<double> lastGpuMs;
  std::string message{"ready"};
  std::optional<History> previous, pending;
  uint32_t pendingImage{UINT32_MAX};
  bool pendingReset{}, initialized{}, active{}, failed{};

  Impl(std::shared_ptr<gpu::VulkanDevice> d, gpu::VulkanMemoryManager& m, gpu::PipelineManager& p)
      : device(std::move(d)), memory(m), pipelines(p) {}

  ~Impl() {
    sdk.reset();
    if (queries) vkDestroyQueryPool(device->device(), queries, nullptr);
    if (commandPool) vkDestroyCommandPool(device->device(), commandPool, nullptr);
    pipelines.destroyPipeline(fullPipeline);
    pipelines.destroyPipeline(emptyPipeline);
    pipelines.destroyPipeline(composePipeline);
    pipelines.destroyPipelineLayout(layout);
    pipelines.destroyPipelineLayout(composeLayout);
    pipelines.destroyDescriptorPool(pool);
    pipelines.destroyDescriptorSetLayout(guideLayout);
    pipelines.destroyDescriptorSetLayout(composeSetLayout);
    for (auto& slot : slots) memory.destroyBuffer(slot.uniform);
  }

  void initialize(const std::filesystem::path& root, UpscaleMode mode, VkExtent2D extent, uint32_t count,
      VkDescriptorSetLayout materials, VkDescriptorSetLayout lights, VkDescriptorSetLayout scene,
      uint64_t budget) {
    if (!device->dlssRayReconstructionEnabled())
      throw std::runtime_error(std::string(device->dlssRayReconstructionStatus()));
    if (!device->enabledFeatures().shaderStorageImageExtendedFormats)
      throw std::runtime_error("DLSS RR requires extended storage image formats");
    const auto family = device->queueFamilyIndices().graphicsFamily.value();
    sdk = std::make_unique<ReferenceDlssReconstructor>(device->instance(), device->physicalDevice(),
        device->device(), device->graphicsQueue(), family, container::util::executableDirectory());
    const auto optimal = sdk->optimalRenderExtent(mode, extent);
    if (!optimal) throw std::runtime_error(std::string(sdk->status()));
    renderExtent = *optimal;
    outputExtent = extent;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device->physicalDevice(), &properties);
    const auto& limits = properties.limits;
    if (limits.maxBoundDescriptorSets < 4 || limits.maxPerStageDescriptorStorageImages < 10 ||
        limits.maxDescriptorSetStorageImages < 10 || limits.maxPerStageDescriptorStorageBuffers < 13 ||
        limits.maxDescriptorSetStorageBuffers < 13 || !renderExtent.width || !renderExtent.height ||
        renderExtent.width > limits.maxImageDimension2D || renderExtent.height > limits.maxImageDimension2D ||
        (renderExtent.width + 7) / 8 > limits.maxComputeWorkGroupCount[0] ||
        (renderExtent.height + 7) / 8 > limits.maxComputeWorkGroupCount[1])
      throw std::runtime_error("DLSS RR extent or descriptor limits exceeded");
    const uint64_t inputBytes = uint64_t(renderExtent.width) * renderExtent.height * 56;
    const uint64_t outputBytes = uint64_t(extent.width) * extent.height * 16;
    if (inputBytes + outputBytes + count * sizeof(GuideUniform) > budget)
      throw std::runtime_error("DLSS RR guide image budget exceeded");

    const auto requireFormat = [&](VkFormat format, VkFormatFeatureFlags features) {
      VkFormatProperties support{};
      vkGetPhysicalDeviceFormatProperties(device->physicalDevice(), format, &support);
      if ((support.optimalTilingFeatures & features) != features)
        throw std::runtime_error("Required DLSS RR image format features are unavailable");
    };
    for (const auto format : guideFormats)
      requireFormat(format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    for (size_t i = 0; i < guides.size(); ++i) {
      guides[i] = std::make_unique<GuideImage>(*device, memory, renderExtent, guideFormats[i],
          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
      allocated += guides[i]->bytes();
    }
    output = std::make_unique<GuideImage>(*device, memory, extent, outputFormat, VK_IMAGE_USAGE_STORAGE_BIT);
    allocated += output->bytes();
    if (allocated + count * sizeof(GuideUniform) > budget)
      throw std::runtime_error("DLSS RR guide allocation exceeds its image budget");

    std::vector<VkDescriptorSetLayoutBinding> bindings{{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    for (uint32_t i = 1; i <= guides.size(); ++i)
      bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (uint32_t i = 8; i <= 10; ++i)
      bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    guideLayout = pipelines.createDescriptorSetLayout(bindings, {});
    layout = pipelines.createPipelineLayout({materials, lights, scene, guideLayout}, {});
    composeSetLayout = pipelines.createDescriptorSetLayout({
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}, {});
    composeLayout = pipelines.createPipelineLayout({materials, lights, scene, composeSetLayout}, {});
    pool = pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count * uint32_t(guides.size()) + 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * count}}, count + 1);
    std::vector<VkDescriptorSetLayout> layouts(count, guideLayout);
    layouts.push_back(composeSetLayout);
    std::vector<VkDescriptorSet> sets(layouts.size());
    const VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool,
        static_cast<uint32_t>(layouts.size()), layouts.data()};
    require(vkAllocateDescriptorSets(device->device(), &setInfo, sets.data()), "DLSS RR descriptor allocation failed");
    composeSet = sets.back();
    const VkDescriptorImageInfo color{VK_NULL_HANDLE, output->resource.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet composeWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, composeSet, 2, 0, 1,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &color};
    vkUpdateDescriptorSets(device->device(), 1, &composeWrite, 0, nullptr);

    const VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    require(vkCreateCommandPool(device->device(), &poolInfo, nullptr, &commandPool),
        "DLSS RR command pool allocation failed");
    std::vector<VkCommandBuffer> commands(count);
    const VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
        commandPool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, count};
    require(vkAllocateCommandBuffers(device->device(), &commandInfo, commands.data()),
        "DLSS RR evaluation command allocation failed");
    slots.resize(count);
    for (uint32_t image = 0; image < count; ++image) {
      auto& slot = slots[image];
      slot.set = sets[image];
      slot.secondary = commands[image];
      slot.uniform = memory.createBuffer(sizeof(GuideUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
      allocated += slot.uniform.allocation_info.size;
      const VkDescriptorBufferInfo uniform{slot.uniform.buffer, 0, sizeof(GuideUniform)};
      std::array<VkDescriptorImageInfo, guideFormats.size()> images{};
      std::array<VkWriteDescriptorSet, guideFormats.size() + 1> writes{};
      writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, slot.set, 0, 0, 1,
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniform};
      for (uint32_t i = 0; i < guides.size(); ++i) {
        images[i] = {VK_NULL_HANDLE, guides[i]->resource.view, VK_IMAGE_LAYOUT_GENERAL};
        writes[i + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, slot.set, i + 1, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[i]};
      }
      vkUpdateDescriptorSets(device->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    if (allocated > budget)
      throw std::runtime_error("DLSS RR frame allocations exceed their budget");
    fullPipeline = pipelines.createComputePipeline(root / "spv_shaders/reference_pathtrace_rr.comp.spv",
        layout, "reference_pathtrace_rr");
    emptyPipeline = pipelines.createComputePipeline(root / "spv_shaders/reference_pathtrace_empty_rr.comp.spv",
        layout, "reference_pathtrace_empty_rr");
    composePipeline = pipelines.createComputePipeline(root / "spv_shaders/reference_denoise_compose.comp.spv",
        composeLayout, "reference_rr_compose");
    if (!sdk->prepare(mode, renderExtent, outputExtent))
      throw std::runtime_error(std::string(sdk->status()));

    uint32_t familyCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(), &familyCount, families.data());
    timestampBits = families.at(family).timestampValidBits;
    timestampPeriod = limits.timestampPeriod;
    if (timestampBits) {
      VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      info.queryCount = count * 2;
      if (vkCreateQueryPool(device->device(), &info, nullptr, &queries) != VK_SUCCESS) queries = {};
    }
  }

  void bindPrevious(const Slot& slot, const RaySurfaceGpuBindings& scene) const {
    const std::array<VkDescriptorBufferInfo, 3> buffers{{
        {scene.surfaceInstances, 0, scene.surfaceInstancesSize},
        {scene.surfaceTriangles, 0, scene.surfaceTrianglesSize},
        {scene.instances, 0, scene.instancesSize}}};
    std::array<VkWriteDescriptorSet, buffers.size()> writes{};
    for (uint32_t i = 0; i < buffers.size(); ++i)
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, slot.set, 8 + i, 0, 1,
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[i]};
    vkUpdateDescriptorSets(device->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  }

  bool compatible(const History& current) const {
    if (!previous || current.revision != previous->revision ||
        current.motion.topology != previous->motion.topology ||
        static_cast<bool>(current.scene) != static_cast<bool>(previous->scene))
      return false;
    const auto& a = current.trace;
    const auto& b = previous->trace;
    return a.inverseProjection == b.inverseProjection &&
        glm::dot(glm::vec3(a.inverseView[2]), glm::vec3(b.inverseView[2])) >= 0.5f &&
        glm::distance(glm::vec3(a.inverseView[3]), glm::vec3(b.inverseView[3])) <= 5.0f &&
        a.limits.w == b.limits.w && a.sectionPlane == b.sectionPlane &&
        std::memcmp(&a.boxClip, &b.boxClip, sizeof(a.boxClip)) == 0 &&
        a.sampling.x == b.sampling.x && a.sampling.y == b.sampling.y && a.sampling.z == b.sampling.z &&
        a.trace == b.trace && a.controls.x == b.controls.x &&
        a.ground == b.ground && a.groundColor == b.groundColor;
  }

  void imageLayouts(VkCommandBuffer command, bool toSampled) const {
    std::array<VkImageMemoryBarrier2, guideFormats.size()> barriers{};
    for (size_t i = 0; i < guides.size(); ++i) {
      barriers[i] = toSampled
          ? imageBarrier(guides[i]->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT)
          : imageBarrier(guides[i]->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    transition(command, barriers);
  }

  void initializeImages(VkCommandBuffer command) const {
    std::array<VkImageMemoryBarrier2, guideFormats.size() + 1> barriers{};
    for (size_t i = 0; i < guides.size(); ++i)
      barriers[i] = imageBarrier(guides[i]->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    barriers.back() = imageBarrier(output->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    transition(command, barriers);
  }

  void publish(VkCommandBuffer command, const ReferencePathTraceInput& input, VkDescriptorSet scene) const {
    // The existing composition shader preserves raw diagnostic pixels and
    // uses only finite filtered values for the viewport.
    const VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, composePipeline);
    const std::array sets{input.materials, input.lights, scene, composeSet};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, composeLayout, 0,
        static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdDispatch(command, (outputExtent.width + 7) / 8, (outputExtent.height + 7) / 8, 1);
  }
};

ReferenceDlssPass::ReferenceDlssPass(std::shared_ptr<gpu::VulkanDevice> device, gpu::VulkanMemoryManager& memory,
    gpu::PipelineManager& pipelines, const std::filesystem::path& root, UpscaleMode mode, VkExtent2D output,
    uint32_t count, VkDescriptorSetLayout materials, VkDescriptorSetLayout lights,
    VkDescriptorSetLayout scene, uint64_t budget)
    : impl_(std::make_unique<Impl>(std::move(device), memory, pipelines)) {
  impl_->initialize(root, mode, output, count, materials, lights, scene, budget);
}

ReferenceDlssPass::~ReferenceDlssPass() = default;

void ReferenceDlssPass::record(VkCommandBuffer command, uint32_t image, const ReferencePathTraceInput& input,
    const ReferencePathTraceData& trace, VkDescriptorSet sceneSet) {
  auto& s = *impl_;
  auto& slot = s.slots.at(image);
  slot.previousLifetime.reset();
  s.active = false;
  if (s.pending) {
    s.sdk->discard();
    s.failed = true;
    s.message = "Evaluation was not submitted. Reselect the reconstruction mode to retry.";
    s.pending.reset();
  }
  if (s.failed) return;
  if ((trace.limits.w & 8u) != 0 || input.settings.ground.mode == ReferenceGroundMode::ShadowMatte ||
      trace.trace.z > 0.0f || (!input.emptyScene && !input.dlssMotion.lifetime)) {
    s.previous.reset();
    s.historyFrames = 0;
    s.message = "Raw display while the scene, lens, or shadow matte is incompatible";
    return;
  }
  const auto projection = glm::inverse(trace.inverseProjection);
  const auto view = glm::inverse(trace.inverseView);
  Impl::History current{trace, projection * view, input.scene, input.dlssMotion, input.dlssRevision};
  const bool reset = !s.compatible(current);
  const auto sequence = reset ? 0u : static_cast<uint32_t>(s.historyFrames);
  const auto jitter = container::temporal::jitterPixels(sequence, trace.sampling.z);
  const bool previousGeometry = !reset && !input.emptyScene;
  const GuideUniform uniform{current.viewProj, reset ? current.viewProj : s.previous->viewProj,
      glm::vec4(jitter, 0.0f, 0.0f), {s.renderExtent.width, s.renderExtent.height, sequence,
          (reset ? 0u : 1u) | (previousGeometry ? 2u : 0u)}};
  std::memcpy(slot.uniform.allocation_info.pMappedData, &uniform, sizeof(uniform));
  require(vmaFlushAllocation(s.memory.allocator(), slot.uniform.allocation, 0, sizeof(uniform)),
      "DLSS RR guide uniform flush failed");
  if (!input.emptyScene)
    s.bindPrevious(slot, previousGeometry ? s.previous->scene : input.scene);
  if (previousGeometry) slot.previousLifetime = s.previous->motion.lifetime;
  if (!s.initialized) s.initializeImages(command);

  const VkMemoryBarrier2 uniformBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
      VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT};
  VkDependencyInfo uniformDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  uniformDependency.memoryBarrierCount = 1;
  uniformDependency.pMemoryBarriers = &uniformBarrier;
  vkCmdPipelineBarrier2(command, &uniformDependency);
  const auto pipeline = input.emptyScene ? static_cast<VkPipeline>(s.emptyPipeline) : static_cast<VkPipeline>(s.fullPipeline);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  const std::array sets{input.materials, input.lights, sceneSet, slot.set};
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0,
      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
  vkCmdDispatch(command, (s.renderExtent.width + 7) / 8, (s.renderExtent.height + 7) / 8, 1);
  s.imageLayouts(command, true);

  // Vendor failure can leave partial commands behind. Execute this secondary
  // buffer only when NGX accepted the evaluation, preserving the raw display.
  require(vkResetCommandBuffer(slot.secondary, 0), "DLSS RR evaluation command reset failed");
  const VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
  const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &inheritance};
  require(vkBeginCommandBuffer(slot.secondary, &begin), "DLSS RR evaluation command begin failed");
  const auto result = s.sdk->record({.commands = slot.secondary, .color = s.guides[0]->resource,
      .depth = s.guides[1]->resource, .motion = s.guides[2]->resource, .diffuseAlbedo = s.guides[3]->resource,
      .specularAlbedo = s.guides[4]->resource, .normalRoughness = s.guides[5]->resource,
      .specularHitDistance = s.guides[6]->resource, .output = s.output->resource,
      .jitterPixels = {-jitter.x, -jitter.y}, .worldToView = rowMajor(view), .viewToClip = rowMajor(projection),
      .reset = reset, .frameMilliseconds = input.frameMilliseconds});
  require(vkEndCommandBuffer(slot.secondary), "DLSS RR evaluation command end failed");
  if (result == DlssDispatchResult::Recorded) {
    if (s.queries) {
      vkCmdResetQueryPool(command, s.queries, image * 2, 2);
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, image * 2);
    }
    vkCmdExecuteCommands(command, 1, &slot.secondary);
    if (s.queries)
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, image * 2 + 1);
    s.publish(command, input, sceneSet);
    s.pending = std::move(current);
    s.pendingReset = reset;
  } else {
    s.sdk->discard();
    s.failed = true;
    s.message = result == DlssDispatchResult::InvalidInput
        ? "Invalid DLSS RR frame inputs. Raw display restored." : std::string(s.sdk->status());
  }
  s.imageLayouts(command, false);
  s.pendingImage = image;
}

void ReferenceDlssPass::commit(uint32_t image) noexcept {
  auto& s = *impl_;
  if (s.pendingImage != image) return;
  s.pendingImage = UINT32_MAX;
  s.initialized = true;
  if (!s.pending) return;
  s.sdk->commit();
  if (s.pendingReset) {
    ++s.resets;
    s.historyFrames = 0;
  }
  ++s.historyFrames;
  s.slots[image].timedSerial = ++s.submitted;
  s.previous = std::exchange(s.pending, std::nullopt);
  s.active = true;
  s.message = "active";
}

void ReferenceDlssPass::invalidateHistory() noexcept {
  impl_->previous.reset();
  impl_->historyFrames = 0;
  impl_->active = false;
}

void ReferenceDlssPass::collectTiming(uint32_t image) {
  auto& s = *impl_;
  const auto serial = s.slots.at(image).timedSerial;
  if (!s.queries || serial <= s.lastTimedSerial) return;
  std::array<uint64_t, 2> stamps{};
  if (vkGetQueryPoolResults(s.device->device(), s.queries, image * 2, 2, sizeof(stamps),
          stamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return;
  const uint64_t mask = s.timestampBits == 64 ? UINT64_MAX : (uint64_t{1} << s.timestampBits) - 1;
  s.lastGpuMs = static_cast<double>((stamps[1] - stamps[0]) & mask) * s.timestampPeriod / 1e6;
  s.lastTimedSerial = serial;
}

ReferenceDlssStatus ReferenceDlssPass::status() const {
  const auto& s = *impl_;
  return {.message = s.message, .active = s.active, .renderExtent = s.renderExtent, .outputExtent = s.outputExtent,
      .historyResets = s.resets, .historyFrames = s.historyFrames, .allocatedBytes = s.allocated,
      .vendorAllocatedBytes = s.sdk->allocatedBytes(), .lastGpuMs = s.lastGpuMs};
}

} // namespace container::renderer
