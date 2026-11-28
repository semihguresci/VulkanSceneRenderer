#include <Container/renderer/pathtracing/ReferencePathTracePass.h>
#include <Container/renderer/pathtracing/ReferenceFireflyClamp.h>
#include "NativeReferenceRayPipeline.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#if CONTAINER_HAS_VK_OIDN
#include <vk_oidn/vk_oidn.h>
#endif
#if CONTAINER_HAS_REFERENCE_OPTIX
#include <Container/renderer/pathtracing/ReferenceOptixDenoiser.h>
#endif
#if CONTAINER_HAS_DLSS_RR
#include "ReferenceDlssPass.h"
#endif

namespace container::renderer {
namespace {
struct alignas(16) ReferencePathReadback {
  ReferencePathTraceDiagnostics diagnostics{};
  glm::vec4 autofocus{};
};
static_assert(sizeof(ReferencePathReadback) == 32);

#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
using GuideReplay = std::array<uint32_t, 2>;
constexpr uint32_t kMaxGuideReplaySamples = 64;
#endif
#if CONTAINER_HAS_VK_OIDN
constexpr uint32_t kOidnIntermediateMemoryMiB = 256;
// vk_oidn's tiling cap covers intermediates. The pinned SDK embeds FP16 weights;
// reserve another 32 MiB as an estimate when admitting both display backends.
constexpr uint64_t kOidnJointAdmissionBytes = uint64_t{kOidnIntermediateMemoryMiB + 32} * 1024 * 1024;
#endif

[[noreturn]] void fail(const char* text) { throw std::invalid_argument(std::string("Reference path tracing: ")+text); }
void barrier(VkCommandBuffer cmd,VkPipelineStageFlags2 stage,VkAccessFlags2 access) {
  VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  b.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;
  b.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
  b.dstStageMask=stage;b.dstAccessMask=access;
  VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.memoryBarrierCount=1;d.pMemoryBarriers=&b;
  vkCmdPipelineBarrier2(cmd,&d);
}
bool finite(const glm::mat4& m) {
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(!std::isfinite(m[c][r])) return false;
  return true;
}
auto sceneKey(const RaySurfaceGpuBindings& scene) {
  return std::tuple(scene.tlas,scene.triangles,scene.instances,scene.surfaceTriangles,scene.surfaceInstances,
      scene.trianglesSize,scene.instancesSize,scene.surfaceTrianglesSize,scene.surfaceInstancesSize);
}
}
struct ReferencePathTracePass::Impl {
  struct Image {
    gpu::VulkanMemoryManager& memory;
    VkImage image{};VmaAllocation allocation{};vk::raii::ImageView view{nullptr};
    Image(std::shared_ptr<gpu::VulkanDevice> device,gpu::VulkanMemoryManager& m,VkExtent2D extent,
          VkImageUsageFlags extraUsage = 0):memory(m) {
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R32G32B32A32_SFLOAT;
      info.extent={extent.width,extent.height,1};info.mipLevels=1;info.arrayLayers=1;
      info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
      info.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|extraUsage;
      VmaAllocationCreateInfo a{};a.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      if(vmaCreateImage(m.allocator(),&info,&a,&image,&allocation,nullptr)!=VK_SUCCESS)
        throw std::runtime_error("Reference path image allocation failed");
      try {
        vk::ImageViewCreateInfo v{};v.image=image;v.viewType=vk::ImageViewType::e2D;
        v.format=vk::Format::eR32G32B32A32Sfloat;
        v.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
        view=vk::raii::ImageView(device->raii(),v);
      } catch(...) {vmaDestroyImage(m.allocator(),image,allocation);throw;}
    }
    ~Image() {view.clear();if(image) vmaDestroyImage(memory.allocator(),image,allocation);}
  };
  struct Frame {
    gpu::AllocatedBuffer uniform{}, diagnostic{}, meshEmitters{};
    VkDeviceSize meshEmitterBytes{};
    VkDescriptorSet set{};
    ReferencePathTraceInput input{};
    ReferencePathTraceData key{},data{};
    bool prepared{},recorded{},submitted{},reset{};
    VkPipeline boundPipeline{};
    std::shared_ptr<const void> boundContent;
    uint64_t epoch{},submission{};
    float handoffWeight{};
#if CONTAINER_HAS_DLSS_RR
    bool dlssEvaluated{}, dlssYielded{};
#endif
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
    uint32_t guideSamples{}, denoisedSamples{};
    bool guideReset{}, denoiseRecorded{}, denoiseTimingPending{};
    std::shared_ptr<const void> guideContent;
#endif
#if CONTAINER_HAS_REFERENCE_OPTIX
    uint32_t optixCapturedSamples{};
#endif
  };
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  struct Denoiser {
    std::shared_ptr<gpu::VulkanDevice> device;
    gpu::VulkanMemoryManager& memory;
    gpu::PipelineManager& pipelines;
    Image albedo, normal, filtered;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    gpu::ComputePipeline guided, emptyGuided, compose;
    bool initialized{}, failed{};
    uint32_t guideSamples{}, filteredSamples{};
    std::shared_ptr<const void> guideContent;
    std::optional<double> lastGpuMs;
#if CONTAINER_HAS_VK_OIDN
    VkQueryPool queries{};
    uint32_t timestampBits{};
    std::unique_ptr<vkoidn::Context> context;
    std::unique_ptr<vkoidn::Denoiser> network;
    bool weightsSubmitted{};
    uint64_t lastTimedSubmission{};
#endif
#if CONTAINER_HAS_REFERENCE_OPTIX
    std::unique_ptr<ReferenceOptixDenoiser> optix;
    gpu::AllocatedBuffer readback{}, upload{};
    std::vector<float> cameraNormals;
    VkDeviceSize imageBytes{};
    uint32_t stagedSamples{}, readySamples{};
#endif

    Denoiser(const std::shared_ptr<gpu::VulkanDevice>& device, gpu::VulkanMemoryManager& memory,
         gpu::PipelineManager& pipelines, VkExtent2D extent)
        : device(device), memory(memory), pipelines(pipelines), albedo(device, memory, extent),
          normal(device, memory, extent), filtered(device, memory, extent, VK_IMAGE_USAGE_TRANSFER_DST_BIT) {}

    ~Denoiser() {
      pipelines.destroyPipeline(guided);
      pipelines.destroyPipeline(emptyGuided);
      pipelines.destroyPipeline(compose);
      pipelines.destroyDescriptorPool(pool);
      pipelines.destroyPipelineLayout(pipelineLayout);
      pipelines.destroyDescriptorSetLayout(setLayout);
#if CONTAINER_HAS_VK_OIDN
      if (queries) vkDestroyQueryPool(device->device(), queries, nullptr);
#endif
#if CONTAINER_HAS_REFERENCE_OPTIX
      memory.destroyBuffer(readback);
      memory.destroyBuffer(upload);
#endif
    }

    void initialize(const std::filesystem::path& root,
                    VkDescriptorSetLayout materials, VkDescriptorSetLayout lights, VkDescriptorSetLayout local) {
      std::vector<VkDescriptorSetLayoutBinding> bindings;
      for (uint32_t binding = 0; binding < 3; ++binding)
        bindings.push_back({binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
      setLayout = pipelines.createDescriptorSetLayout(bindings, {});
      pipelineLayout = pipelines.createPipelineLayout({materials, lights, local, setLayout},
          {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GuideReplay)}});
      const auto shaders = root / "spv_shaders";
      guided = pipelines.createComputePipeline(shaders / "reference_guided_pathtrace.comp.spv", pipelineLayout,
          "reference_guided_pathtrace");
      emptyGuided = pipelines.createComputePipeline(shaders / "reference_guided_background.comp.spv", pipelineLayout,
          "reference_guided_background");
      compose = pipelines.createComputePipeline(shaders / "reference_denoise_compose.comp.spv", pipelineLayout,
          "reference_denoise_compose");
      pool = pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}}, 1);
      const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          nullptr, pool, 1, &setLayout};
      if (vkAllocateDescriptorSets(device->device(), &allocation, &set) != VK_SUCCESS)
        throw std::runtime_error("guide descriptor allocation failed");
      const std::array<VkDescriptorImageInfo, 3> images{{
          {VK_NULL_HANDLE, *albedo.view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, *normal.view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, *filtered.view, VK_IMAGE_LAYOUT_GENERAL}}};
      for (uint32_t binding = 0; binding < images.size(); ++binding) {
        const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[binding], nullptr, nullptr};
        vkUpdateDescriptorSets(device->device(), 1, &write, 0, nullptr);
      }
    }

#if CONTAINER_HAS_VK_OIDN
    void initializeOidn(uint32_t count) {
      const std::array extensions{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
      const vkoidn::ContextCreateInfo info{device->instance(), device->physicalDevice(), device->device(),
          vkGetInstanceProcAddr, vkGetDeviceProcAddr, extensions.data(),
          device->referenceOidnCooperativeMatrix() ? 2u : 1u};
      const auto result = vkoidn::Context::create(info, context);
      if (result != vkoidn::Result::eSuccess)
        throw std::runtime_error(vkoidn::getResultString(result));
      if (!context->areImageViewsSupported()) throw std::runtime_error("storage image views unavailable");
      uint32_t familyCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(), &familyCount, nullptr);
      std::vector<VkQueueFamilyProperties> families(familyCount);
      vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(), &familyCount, families.data());
      timestampBits = families.at(device->queueFamilyIndices().graphicsFamily.value()).timestampValidBits;
      if (timestampBits != 0) {
        const VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
            VK_QUERY_TYPE_TIMESTAMP, 2 * count, 0};
        if (vkCreateQueryPool(device->device(), &queryInfo, nullptr, &queries) != VK_SUCCESS) queries = {};
      }
    }
#endif

#if CONTAINER_HAS_REFERENCE_OPTIX
    void initializeOptix(VkExtent2D extent, uint64_t budget) {
      VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
      vkGetPhysicalDeviceProperties2(device->physicalDevice(), &props);
      if (props.properties.vendorID != 0x10de)
        throw std::runtime_error("OptiX requires a Vulkan device on an NVIDIA adapter");
      std::array<uint8_t, VK_UUID_SIZE> uuid{};
      std::copy_n(id.deviceUUID, uuid.size(), uuid.begin());
      imageBytes = VkDeviceSize(extent.width) * extent.height * sizeof(float) * 4;
      optix = std::make_unique<ReferenceOptixDenoiser>(uuid, extent.width, extent.height);
      readback = memory.createBuffer(3 * imageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
      upload = memory.createBuffer(imageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
      if (!readback.allocation_info.pMappedData || !upload.allocation_info.pMappedData)
        throw std::runtime_error("OptiX host staging buffers could not be mapped");
      cameraNormals.resize(static_cast<size_t>(imageBytes / (2 * sizeof(float))));
      if (!referenceOptixImageBudgetFits(allocatedBytes(), imageBytes, budget))
        throw std::runtime_error("OptiX staging and working memory exceed the reference image budget");
    }

    void runOptix(const glm::mat4& inverseView) {
      if (!stagedSamples || failed) return;
      if (vmaInvalidateAllocation(memory.allocator(), readback.allocation, 0, 3 * imageBytes) != VK_SUCCESS)
        throw std::runtime_error("OptiX Vulkan guide readback invalidate failed");
      const auto count = static_cast<size_t>(imageBytes / sizeof(float));
      const auto* input = static_cast<const float*>(readback.allocation_info.pMappedData);
      auto* output = static_cast<float*>(upload.allocation_info.pMappedData);
      const auto* normals = input + 2 * count;
      // OptiX's spatial HDR model takes camera X/Y. OIDN keeps using the
      // unmodified, accumulated world-space normal. Empty guides remain zero.
      const glm::mat3 worldToView = glm::transpose(glm::mat3(inverseView));
      for (size_t i = 0; i < count; i += 4) {
        const glm::vec3 normal = worldToView * glm::vec3(normals[i], normals[i + 1], normals[i + 2]);
        const float length = glm::length(normal);
        cameraNormals[i / 2] = length > 1e-6f ? normal.x / length : 0.0f;
        cameraNormals[i / 2 + 1] = length > 1e-6f ? normal.y / length : 0.0f;
      }
      lastGpuMs = optix->denoise({input, count}, {input + count, count}, cameraNormals, {output, count});
      if (vmaFlushAllocation(memory.allocator(), upload.allocation, 0, imageBytes) != VK_SUCCESS)
        throw std::runtime_error("OptiX Vulkan output upload flush failed");
      readySamples = stagedSamples;
      stagedSamples = 0;
    }

    static void transitionCopies(VkCommandBuffer cmd, std::span<const VkImage> images,
        VkImageLayout transferLayout, bool toTransfer) {
      std::array<VkImageMemoryBarrier2, 3> barriers{};
      const auto transferAccess = transferLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
          ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
      const auto generalAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      for (size_t i = 0; i < images.size(); ++i) {
        auto& b = barriers[i];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = toTransfer ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstStageMask = toTransfer ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = toTransfer ? generalAccess : transferAccess;
        b.dstAccessMask = toTransfer ? transferAccess : generalAccess;
        b.oldLayout = toTransfer ? VK_IMAGE_LAYOUT_GENERAL : transferLayout;
        b.newLayout = toTransfer ? transferLayout : VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[i];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      }
      VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      info.imageMemoryBarrierCount = static_cast<uint32_t>(images.size());
      info.pImageMemoryBarriers = barriers.data();
      vkCmdPipelineBarrier2(cmd, &info);
    }

    void recordOptixCopies(VkCommandBuffer cmd, VkExtent2D extent, VkImage mean, bool capture, bool publish) const {
      VkBufferImageCopy region{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .imageExtent = {extent.width, extent.height, 1}};
      if (publish) {
        const std::array images{filtered.image};
        const auto layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        transitionCopies(cmd, images, layout, true);
        vkCmdCopyBufferToImage(cmd, upload.buffer, filtered.image, layout, 1, &region);
        transitionCopies(cmd, images, layout, false);
      }
      if (capture) {
        const std::array images{mean, albedo.image, normal.image};
        const auto layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        transitionCopies(cmd, images, layout, true);
        for (size_t i = 0; i < images.size(); ++i) {
          region.bufferOffset = i * imageBytes;
          vkCmdCopyImageToBuffer(cmd, images[i], layout, readback.buffer, 1, &region);
        }
        transitionCopies(cmd, images, layout, false);
      }
    }
#endif

    [[nodiscard]] uint64_t guideImageBytes() const {
      uint64_t bytes{};
      for (const auto* image : {&albedo, &normal, &filtered}) {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(memory.allocator(), image->allocation, &info);
        bytes += info.size;
      }
      return bytes;
    }

    [[nodiscard]] uint64_t allocatedBytes() const {
      uint64_t bytes = guideImageBytes();
#if CONTAINER_HAS_VK_OIDN
      if (network) bytes += network->getMemoryUsage();
#endif
#if CONTAINER_HAS_REFERENCE_OPTIX
      if (optix) bytes += optix->allocatedBytes() + readback.allocation_info.size + upload.allocation_info.size +
          cameraNormals.size() * sizeof(float);
#endif
      return bytes;
    }

    void initializeImages(VkCommandBuffer command) const {
      const std::array images{albedo.image, normal.image, filtered.image};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      std::array<VkImageMemoryBarrier2, 3> barriers{};
      for (size_t i = 0; i < images.size(); ++i) {
        auto& b = barriers[i];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[i];
        b.subresourceRange = range;
      }
      VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      info.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
      info.pImageMemoryBarriers = barriers.data();
      vkCmdPipelineBarrier2(command, &info);
      const VkClearColorValue initial{{0.0f, 0.0f, 0.0f, 1.0f}};
      vkCmdClearColorImage(command, filtered.image, VK_IMAGE_LAYOUT_GENERAL, &initial, 1, &range);
    }
  };
#endif
  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::VulkanMemoryManager& memory;gpu::PipelineManager& pipelines;
  gpu::ComputePipeline pipeline{},emptyPipeline{};
  std::shared_ptr<NativeReferenceRayPipeline> nativeRays;
  std::future<NativeReferenceRayPipeline::Compilation> nativeCompilation;
  VkDescriptorSetLayout localLayout{},materialLayout{},lightLayout{};
  VkPipelineLayout layout{};VkDescriptorPool pool{};
  bool nativeLayout{};
  std::unique_ptr<Image> mean,moment,display;
  ReferenceOidnSettings oidnSettings{};
  std::string oidnMessage{"off"};
  std::string dlssMessage{"off"};
  uint64_t imageBudget{};
  float handoffWeight{};
#if CONTAINER_HAS_DLSS_RR
  std::unique_ptr<ReferenceDlssPass> dlss;
  bool dlssYielded{};
#endif
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  std::unique_ptr<Denoiser> denoiser;
#endif
  std::vector<Frame> frames;
  VkExtent2D extent{};VkPhysicalDeviceProperties properties{};
  bool initialized{},historyValid{};
  uint32_t samples{},pendingImage{UINT32_MAX};uint64_t resets{},submissionSerial{};
  uint32_t lastSubmittedSamples{};
  std::optional<uint32_t> autofocusImage;
  ReferencePathTraceData submittedKey{};RaySurfaceGpuBindings submittedScene{};
  uint64_t submittedRevision{};
  VkImageView submittedEnvironment{};VkSampler submittedSampler{};VkPipeline submittedPipeline{};
  std::shared_ptr<const void> submittedContent;
  Impl(std::shared_ptr<gpu::VulkanDevice> d,gpu::VulkanMemoryManager& m,gpu::PipelineManager& p)
      :device(std::move(d)),memory(m),pipelines(p) {
    if(!device || (!device->rayQueriesEnabled() && !device->rayTracingPipelineSupported()))
      fail("Vulkan ray traversal is unavailable");
    vkGetPhysicalDeviceProperties(device->physicalDevice(),&properties);
  }
  ~Impl() {
    // The driver may still be reading this pass's pipeline layout.
    if (nativeCompilation.valid()) {
      nativeCompilation.wait();
      nativeCompilation = {};
    }
    clear();
    nativeRays.reset();
    pipelines.destroyPipeline(pipeline);
    pipelines.destroyPipeline(emptyPipeline);
    pipelines.destroyPipelineLayout(layout);
    pipelines.destroyDescriptorSetLayout(localLayout);
  }
  void validateDescriptorLimits() const {
    const auto& limits = properties.limits;
    if (limits.maxPerStageDescriptorStorageBuffers < 10 || limits.maxDescriptorSetStorageBuffers < 10 ||
        limits.maxPerStageDescriptorStorageImages < 3 || limits.maxDescriptorSetStorageImages < 3 ||
        limits.maxPerStageDescriptorSampledImages < gpu::kMaterialTextureDescriptorCapacity + 1 ||
        limits.maxDescriptorSetSampledImages < gpu::kMaterialTextureDescriptorCapacity + 1)
      fail("extent, descriptors, or image budget exceeds supported limits");
  }
  void prepareLayout(VkDescriptorSetLayout materials, VkDescriptorSetLayout lights, bool native) {
    if (!materials || !lights) fail("shared descriptor layouts are required");
    if (layout) {
      if (materialLayout != materials || lightLayout != lights || nativeLayout != native)
        fail("shared descriptor layouts or traversal backend changed; recreate the pass");
      return;
    }

    const std::array types{
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(types.size());
    for (uint32_t i = 0; i < types.size(); ++i) {
      VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT;
      if (native) {
        stages |= VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        if (i == 0 || i == 2 || i == 3 || i == 11)
          stages |= VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
      }
      bindings.push_back({i, types[i], 1, stages, nullptr});
    }
    localLayout = pipelines.createDescriptorSetLayout(bindings, {});
    try {
      layout = pipelines.createPipelineLayout({materials, lights, localLayout}, {});
    } catch (...) {
      pipelines.destroyDescriptorSetLayout(localLayout);
      throw;
    }
    materialLayout = materials;
    lightLayout = lights;
    nativeLayout = native;
  }
  void finishNativeCompilation() {
    auto compiled = nativeCompilation.get();
    auto candidate = std::make_shared<NativeReferenceRayPipeline>(*device, memory);
    candidate->adopt(std::move(compiled));
    nativeRays = std::move(candidate);
  }
  void clear() {
#if CONTAINER_HAS_DLSS_RR
    dlss.reset();
    dlssYielded = false;
#endif
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
    denoiser.reset();
#endif
    pipelines.destroyDescriptorPool(pool);pool={};
    for (auto& f : frames) {
      memory.destroyBuffer(f.uniform);
      memory.destroyBuffer(f.diagnostic);
      memory.destroyBuffer(f.meshEmitters);
    }
    frames.clear();mean.reset();moment.reset();display.reset();submittedContent.reset();
    initialized=false;historyValid=false;samples=0;lastSubmittedSamples=0;pendingImage=UINT32_MAX;
    oidnSettings = {};
    oidnMessage = "off";
    dlssMessage = "off";
    handoffWeight = 0.0f;
    autofocusImage.reset();
  }
  void write(const gpu::AllocatedBuffer& buffer,const void* data,size_t bytes) {
    std::memcpy(buffer.allocation_info.pMappedData,data,bytes);
    if(vmaFlushAllocation(memory.allocator(),buffer.allocation,0,bytes)!=VK_SUCCESS)
      throw std::runtime_error("Reference path input flush failed");
  }
  void buffer(VkDescriptorSet set,uint32_t binding,VkBuffer buffer,VkDeviceSize bytes,VkDescriptorType type) {
    VkDescriptorBufferInfo info{buffer,0,bytes};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet=set;w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=type;w.pBufferInfo=&info;
    vkUpdateDescriptorSets(device->device(),1,&w,0,nullptr);
  }

  void configureSpatialDenoiser(const std::filesystem::path& root, uint64_t budget) {
    oidnMessage = "off";
    const auto& settings = oidnSettings;
    if (!settings.optix && settings.quality == ReferenceOidnQuality::Off) return;
    if (nativeRays && !device->rayQueriesEnabled()) {
      oidnMessage = "Denoiser guide replay requires enabled Vulkan ray queries";
      return;
    }
#if !CONTAINER_HAS_REFERENCE_OPTIX
    if (settings.optix) {
      oidnMessage = "OptiX 9.1.0 SDK was not enabled at build time";
      return;
    }
#endif
#if !CONTAINER_HAS_VK_OIDN
    if (!settings.optix) {
      oidnMessage = "vk_oidn 0.2.0 SDK was not enabled at build time";
      return;
    }
#endif
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
    if (!settings.optix && !device->referenceOidnEnabled()) {
      oidnMessage = "vk_oidn storage-image features unavailable on this device";
      return;
    }
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(device->physicalDevice(), VK_FORMAT_R32G32B32A32_SFLOAT, &format);
    const auto& limits = properties.limits;
    const uint64_t pixels = uint64_t{extent.width} * extent.height;
    if (limits.maxBoundDescriptorSets < 4 || limits.maxPerStageDescriptorStorageImages < 6 ||
        limits.maxDescriptorSetStorageImages < 6 || pixels > budget / (settings.optix ? 224u : 96u) ||
        (format.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0) {
      oidnMessage = "Denoiser guide images exceed device limits or the reference image budget";
      return;
    }
    try {
      auto backend = std::make_unique<Denoiser>(device, memory, pipelines, extent);
      backend->initialize(root, materialLayout, lightLayout, localLayout);
#if CONTAINER_HAS_REFERENCE_OPTIX
      if (settings.optix) backend->initializeOptix(extent, budget);
#endif
#if CONTAINER_HAS_VK_OIDN
      if (!settings.optix) {
        if (!referenceOptixImageBudgetFits(backend->guideImageBytes(), pixels * 16, budget))
          throw std::runtime_error("Guide allocation exceeds the reference image budget");
        backend->initializeOidn(static_cast<uint32_t>(frames.size()));
      }
#endif
      denoiser = std::move(backend);
      oidnMessage = "ready";
    } catch (const std::exception& error) {
      oidnMessage = std::string(settings.optix ? "OptiX setup failed: " : "vk_oidn setup failed: ") + error.what();
    }
#else
    (void)root;
    (void)budget;
#endif
  }

  void configureDlss(const std::filesystem::path& root, uint64_t budget) {
    dlssMessage = "off";
    const auto mode = oidnSettings.dlss;
    if (mode == UpscaleMode::Native) return;
    if (nativeRays) {
      dlssMessage = "Ray Reconstruction requires the inline ray-query backend";
      return;
    }
#if CONTAINER_HAS_DLSS_RR
    try {
      dlss = std::make_unique<ReferenceDlssPass>(device, memory, pipelines, root, mode, extent,
          static_cast<uint32_t>(frames.size()), materialLayout, lightLayout, localLayout, budget);
    } catch (const std::exception& error) {
      dlssMessage = error.what();
    }
#else
    (void)root;
    (void)budget;
    dlssMessage = "DLSS Ray Reconstruction SDK was not enabled at build time";
#endif
  }
};
ReferencePathTracePass::ReferencePathTracePass(std::shared_ptr<gpu::VulkanDevice> device,gpu::VulkanMemoryManager& memory,gpu::PipelineManager& pipelines)
    :impl_(std::make_unique<Impl>(std::move(device),memory,pipelines)) {}
ReferencePathTracePass::~ReferencePathTracePass()=default;

void ReferencePathTracePass::beginNativePipeline(const std::filesystem::path& shaderRoot,
    VkDescriptorSetLayout materials, VkDescriptorSetLayout lights) {
  auto& s = *impl_;
  if (!s.device->rayTracingPipelineSupported()) fail("Vulkan ray-tracing pipeline is unavailable");
  s.validateDescriptorLimits();
  s.prepareLayout(materials, lights, true);
  if (s.nativeRays || s.nativeCompilation.valid()) return;

  auto code = NativeReferenceRayPipeline::loadShaders(shaderRoot / "spv_shaders");
  s.nativeCompilation = std::async(std::launch::async,
      [device = s.device, layout = s.layout, code = std::move(code)]() mutable {
        return NativeReferenceRayPipeline::compile(*device, layout, std::move(code));
      });
}

bool ReferencePathTracePass::pollNativePipeline() {
  auto& s = *impl_;
  if (s.nativeRays) return true;
  if (!s.nativeCompilation.valid() ||
      s.nativeCompilation.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return false;
  s.finishNativeCompilation();
  return true;
}

void ReferencePathTracePass::createResources(const std::filesystem::path& root,uint32_t count,VkExtent2D extent,
    VkDescriptorSetLayout materials,VkDescriptorSetLayout lights,uint64_t budget, ReferenceOidnSettings oidn,
    bool nativeRayPipeline) {
  auto& s=*impl_;const auto& limits=s.properties.limits;
  if (nativeRayPipeline ? !s.device->rayTracingPipelineSupported() : !s.device->rayQueriesEnabled())
    fail(nativeRayPipeline ? "Vulkan ray-tracing pipeline is unavailable" : "Vulkan ray queries are unavailable");
  if(!materials || !lights || !count || count>64 || !extent.width || !extent.height ||
      extent.width>limits.maxImageDimension2D || extent.height>limits.maxImageDimension2D ||
      (extent.width+7)/8>limits.maxComputeWorkGroupCount[0] || (extent.height+7)/8>limits.maxComputeWorkGroupCount[1] ||
      limits.maxComputeWorkGroupInvocations<64 || limits.maxComputeWorkGroupSize[0]<8 || limits.maxComputeWorkGroupSize[1]<8 ||
      uint64_t(extent.width)*extent.height*48>budget) fail("extent, descriptors, or image budget exceeds supported limits");
  s.validateDescriptorLimits();
  VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(s.device->physicalDevice(),VK_FORMAT_R32G32B32A32_SFLOAT,&format);
  if((format.optimalTilingFeatures&(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))!=
      (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) fail("RGBA32F storage images are unavailable");
  s.prepareLayout(materials, lights, nativeRayPipeline);
  const auto shaderDirectory = root / "spv_shaders";
  if (nativeRayPipeline) {
    if (!s.nativeRays && s.nativeCompilation.valid()) s.finishNativeCompilation();
    if (!s.nativeRays) {
      auto candidate = std::make_shared<NativeReferenceRayPipeline>(*s.device, s.memory);
      if (!candidate->supportsExtent(extent)) fail("extent exceeds the Vulkan ray dispatch limit");
      candidate->adopt(NativeReferenceRayPipeline::compile(
          *s.device, s.layout, NativeReferenceRayPipeline::loadShaders(shaderDirectory)));
      s.nativeRays = std::move(candidate);
    }
    if (!s.nativeRays->supportsExtent(extent)) fail("extent exceeds the Vulkan ray dispatch limit");
  } else if (!s.pipeline) {
    s.pipeline = s.pipelines.createComputePipeline(
        shaderDirectory / "reference_pathtrace.comp.spv", s.layout, "reference_pathtrace");
  }
  if (!s.emptyPipeline)
    s.emptyPipeline = s.pipelines.createComputePipeline(
        shaderDirectory / "reference_background.comp.spv", s.layout, "reference_background");
  s.clear();s.extent=extent;s.imageBudget=budget;
  try {
    s.mean=std::make_unique<Impl::Image>(s.device,s.memory,extent);
    s.moment=std::make_unique<Impl::Image>(s.device,s.memory,extent);
    s.display=std::make_unique<Impl::Image>(s.device,s.memory,extent);
    s.pool=s.pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,count},{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,count},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,6*count},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3*count},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,count},{VK_DESCRIPTOR_TYPE_SAMPLER,count}},count);
    std::vector<VkDescriptorSetLayout> layouts(count,s.localLayout);std::vector<VkDescriptorSet> sets(count);
    VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=s.pool;
    a.descriptorSetCount=count;a.pSetLayouts=layouts.data();
    if(vkAllocateDescriptorSets(s.device->device(),&a,sets.data())!=VK_SUCCESS) throw std::runtime_error("Reference path descriptor allocation failed");
    s.frames.resize(count);
    for(uint32_t i=0;i<count;++i) {
      auto& f=s.frames[i];f.set=sets[i];
      f.uniform=s.memory.createBuffer(sizeof(ReferencePathTraceData),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
      f.diagnostic=s.memory.createBuffer(sizeof(ReferencePathReadback),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    }
  } catch(...) {s.clear();throw;}
  setDenoising(root, oidn);
}

void ReferencePathTracePass::releaseFrameResources() { impl_->clear(); }

void ReferencePathTracePass::prepareFrame(uint32_t image, const ReferencePathTraceInput& input,
    std::span<const gpu::GpuMaterial> materialData, std::span<const RayInstanceData> instanceData) {
  auto& s=*impl_;
  if(image>=s.frames.size() || !s.mean) fail("frame resources are not prepared");
  const auto& settings=input.settings;
  const uint32_t initialBatch = settings.resetSamplesPerFrame.value_or(settings.samplesPerFrame);
  if(!settings.samplesPerFrame || settings.samplesPerFrame>64 || !initialBatch || initialBatch>64 ||
       !settings.sampleLimit || settings.sampleLimit>16777216 ||
      !settings.maxBounces || settings.maxBounces>32 || settings.rouletteDepth>32 || !std::isfinite(settings.rayBias) || settings.rayBias<=0 ||
      !std::isfinite(settings.maxDistance) || settings.maxDistance<=settings.rayBias*4) fail("invalid sampling settings");
  if (!settings.lens.valid()) fail("invalid thin-lens settings");
  if (!settings.ground.valid()) fail("invalid analytic ground settings");
  if (!validReferenceFireflyClamp(settings.fireflyClamp))
    fail("invalid firefly clamp");
  if((!input.emptyScene && (!input.scene || !input.scene.instanceCount || !input.scene.triangleCount || !input.data.limits.z)) ||
      (input.emptyScene && (input.scene.instanceCount || input.scene.triangleCount)) || !input.materials || !input.lights ||
      !input.environment || !input.environmentSampler) fail("requires a complete ray scene or an explicitly empty scene, with shared descriptors");
  if(!finite(input.data.inverseProjection) || !finite(input.data.inverseView) || input.data.boxClip.boxClipPlaneCount>6 ||
      (input.data.limits.w&~31u)) fail("invalid camera or clipping state");
  const auto& source=input.scene;
  const std::array ranges{source.trianglesSize,source.instancesSize,source.surfaceTrianglesSize,source.surfaceInstancesSize};
  const std::array<uint64_t,4> required{uint64_t(source.triangleCount)*sizeof(RayTriangleData),uint64_t(source.instanceCount)*sizeof(RayInstanceData),
      uint64_t(source.triangleCount)*sizeof(RaySurfaceTriangleData),uint64_t(source.instanceCount)*sizeof(RaySurfaceInstanceData)};
  if(!input.emptyScene) for(size_t i=0;i<ranges.size();++i) if(ranges[i]<required[i] || ranges[i]>s.properties.limits.maxStorageBufferRange) fail("surface binding exceeds its storage range");
  std::vector<uint32_t> emitters;
  if (!input.emptyScene && (!materialData.empty() || !instanceData.empty())) {
    if (materialData.size() != input.data.limits.z || instanceData.size() != source.instanceCount)
      fail("mesh emitter views do not match the bound ray scene");
    for (uint32_t index = 0; index < instanceData.size(); ++index) {
      const auto info = instanceData[index].info;
      if (!info.w || info.x >= source.triangleCount || info.w > source.triangleCount - info.x ||
          info.y >= materialData.size())
        fail("mesh emitter instance range is invalid");
      const auto& material = materialData[info.y];
      const bool unlit = ((info.z | material.flags) & gpu::kObjectFlagUnlit) != 0u;
      if (!unlit && material.thicknessFactor > 0.0f && material.transmissionFactor > 0.0f)
        continue;
      const bool emissive = material.emissiveStrength > 0.0f &&
          glm::any(glm::greaterThan(material.emissiveColor, glm::vec3(0)));
      if (unlit || emissive)
        emitters.push_back(index);
    }
  }
  auto& f=s.frames[image];f.prepared=false;f.recorded=false;
  if (s.autofocusImage == image) s.autofocusImage.reset();
  f.input=input;f.key=input.data;
  f.key.limits.x=source.instanceCount;f.key.limits.y=source.triangleCount;
  f.key.extentSamples={s.extent.width,s.extent.height,0,0};
  f.key.sampling={settings.maxBounces,settings.rouletteDepth,settings.seed,uint32_t(emitters.size())};
  const bool lensActive = (input.data.limits.w & 2u) == 0 && settings.lens.apertureRadius > 0.0f;
  f.key.trace={settings.rayBias,settings.maxDistance,
      lensActive ? settings.lens.apertureRadius : 0.0f,
      lensActive ? settings.lens.focusDistance : 0.0f};
  const auto& ground = settings.ground;
  const bool matte = ground.mode == ReferenceGroundMode::ShadowMatte;
  const bool standardError = !matte && input.displayMode == ReferencePathDisplayMode::StandardError;
  f.key.controls = {matte ? 0.0f : settings.fireflyClamp, 0.0f, 0.0f, 0.0f};
  f.key.ground = {};
  f.key.groundColor = {};
  if (ground.mode != ReferenceGroundMode::Off) {
    f.key.ground = {static_cast<float>(ground.mode), ground.height, 0.0f, 0.0f};
    if (!matte) {
      f.key.ground.z = ground.metallic;
      f.key.ground.w = ground.roughness;
      f.key.groundColor = glm::vec4(ground.color, 0.0f);
    }
  }
  f.boundPipeline = input.emptyScene ? static_cast<VkPipeline>(s.emptyPipeline)
      : s.nativeRays ? s.nativeRays->pipeline() : static_cast<VkPipeline>(s.pipeline);
  f.boundContent=input.emptyScene?s.emptyPipeline.contentIdentity():s.pipeline.contentIdentity();
  f.reset=!s.historyValid || settings.sampleLimit<s.samples || std::memcmp(&f.key,&s.submittedKey,sizeof(f.key))!=0 ||
      sceneKey(source)!=sceneKey(s.submittedScene) || input.contentRevision!=s.submittedRevision ||
      input.environment!=s.submittedEnvironment || input.environmentSampler!=s.submittedSampler ||
      f.boundPipeline!=s.submittedPipeline || f.boundContent!=s.submittedContent;
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  if (const auto* denoiser = s.denoiser.get()) {
    const auto& guided = input.emptyScene ? denoiser->emptyGuided : denoiser->guided;
    f.guideContent = guided.contentIdentity();
    f.guideReset = f.reset || f.guideContent != denoiser->guideContent;
  } else {
    f.guideContent.reset();
    f.guideReset = false;
  }
#endif
#if CONTAINER_HAS_REFERENCE_OPTIX
  // The serialized graphics frame policy retires the producing command before
  // prepareFrame. Match the same camera/scene epoch before accessing its guides.
  if (auto* denoiser = s.denoiser.get(); denoiser && denoiser->optix && !f.guideReset &&
      !matte && !standardError && (input.data.limits.w & 8u) == 0) {
    try {
      denoiser->runOptix(f.key.inverseView);
    } catch (const std::exception& error) {
      s.oidnMessage = std::string("OptiX dispatch failed: ") + error.what();
      denoiser->failed = true;
    }
  }
#endif
  f.data=f.key;
  f.data.controls.y = input.autofocusKey ? 1.0f : 0.0f;
  f.data.controls.z = standardError ? 1.0f : 0.0f;
  const uint32_t first=f.reset?0:s.samples;
  const uint32_t batch = f.reset ? initialBatch : settings.samplesPerFrame;
  f.data.extentSamples.z=first;f.data.extentSamples.w=std::min(batch,settings.sampleLimit-first);
  s.write(f.uniform,&f.data,sizeof(f.data));
  const ReferencePathReadback zero{};s.write(f.diagnostic,&zero,sizeof(zero));
  s.buffer(f.set,0,f.uniform.buffer,sizeof(f.data),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
  if(!input.emptyScene) {
  VkWriteDescriptorSetAccelerationStructureKHR acceleration{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  acceleration.accelerationStructureCount=1;acceleration.pAccelerationStructures=&source.tlas;
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.pNext=&acceleration;w.dstSet=f.set;w.dstBinding=1;
  w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;vkUpdateDescriptorSets(s.device->device(),1,&w,0,nullptr);
  const std::array buffers{source.triangles,source.instances,source.surfaceTriangles,source.surfaceInstances};
  for(uint32_t i=0;i<4;++i) s.buffer(f.set,i+2,buffers[i],ranges[i],VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  }
  const std::array<VkDescriptorImageInfo,5> images{{{VK_NULL_HANDLE,*s.mean->view,VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE,*s.moment->view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,*s.display->view,VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE,input.environment,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{input.environmentSampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED}}};
  for(uint32_t i=0;i<images.size();++i) {
    VkWriteDescriptorSet imageWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};imageWrite.dstSet=f.set;imageWrite.dstBinding=i+6;
    imageWrite.descriptorCount=1;imageWrite.descriptorType=i<3?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:i==3?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLER;
    imageWrite.pImageInfo=&images[i];vkUpdateDescriptorSets(s.device->device(),1,&imageWrite,0,nullptr);
  }
  s.buffer(f.set,11,f.diagnostic.buffer,sizeof(ReferencePathReadback),VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  if (emitters.empty())
    emitters.push_back(0); // Keep the descriptor valid when the proposal is inactive.
  const VkDeviceSize emitterBytes = emitters.size() * sizeof(emitters[0]);
  if (emitterBytes > s.properties.limits.maxStorageBufferRange)
    fail("mesh emitter table exceeds its storage range");
  if (f.meshEmitterBytes < emitterBytes) {
    auto buffer = s.memory.createBuffer(emitterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    s.memory.destroyBuffer(f.meshEmitters);
    f.meshEmitters = buffer;
    f.meshEmitterBytes = emitterBytes;
  }
  s.write(f.meshEmitters, emitters.data(), emitterBytes);
  s.buffer(f.set, 12, f.meshEmitters.buffer, emitterBytes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  s.pendingImage=image;f.prepared=true;
}
void ReferencePathTracePass::record(VkCommandBuffer cmd,uint32_t image) {
  auto& s=*impl_;
  if(!cmd || image>=s.frames.size() || s.pendingImage!=image || !s.frames[image].prepared) fail("frame is not prepared for recording");
  auto& f=s.frames[image];
  const bool traceNative = s.nativeRays && !f.input.emptyScene;
  const VkPipelineStageFlags2 sampleStage = traceNative
      ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  if(!s.initialized) {
    std::array<VkImageMemoryBarrier2,3> barriers{};const std::array images{s.mean->image,s.moment->image,s.display->image};
    for(size_t i=0;i<3;++i) {
      auto& b=barriers[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      b.dstStageMask=sampleStage;b.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT;
      b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=images[i];b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.imageMemoryBarrierCount=3;d.pImageMemoryBarriers=barriers.data();vkCmdPipelineBarrier2(cmd,&d);
  }
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  auto* denoiser = s.denoiser.get();
  if (denoiser && !denoiser->initialized) denoiser->initializeImages(cmd);
#endif
  barrier(cmd,sampleStage,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_UNIFORM_READ_BIT);
  std::array sets{f.input.materials, f.input.lights, f.set, VkDescriptorSet{}};
  const auto dispatch = [&](VkPipeline pipeline, VkPipelineLayout layout, uint32_t setCount) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, setCount, sets.data(), 0, nullptr);
    vkCmdDispatch(cmd, (s.extent.width + 7) / 8, (s.extent.height + 7) / 8, 1);
  };
  const auto dispatchSamples = [&] {
    if (traceNative)
      s.nativeRays->record(cmd, s.layout, std::span{sets}.first(3), s.extent);
    else
      dispatch(f.boundPipeline, s.layout, 3);
  };
  f.handoffWeight = 0.0f;
  float targetWeight = 0.0f;
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  const uint32_t firstSample = f.data.extentSamples.z;
  const uint32_t samples = firstSample + f.data.extentSamples.w;
  const uint32_t handoffTarget = std::min(s.oidnSettings.handoffSamples, f.input.settings.sampleLimit);
  f.guideSamples = denoiser && !f.guideReset ? denoiser->guideSamples : 0;
  const bool canGuide = denoiser && !denoiser->failed &&
      f.input.settings.ground.mode != ReferenceGroundMode::ShadowMatte && (f.input.data.limits.w & 8u) == 0;
  const auto dispatchGuided = [&](GuideReplay replay) {
    const auto& pipeline = f.input.emptyScene ? denoiser->emptyGuided : denoiser->guided;
    sets[3] = denoiser->set;
    vkCmdPushConstants(cmd, denoiser->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(replay), replay.data());
    dispatch(static_cast<VkPipeline>(pipeline), denoiser->pipelineLayout, 4);
  };
  if (canGuide && f.guideSamples == firstSample && !traceNative) {
    dispatchGuided({});
    f.guideSamples = samples;
  } else {
    dispatchSamples();
    if (canGuide) {
      // Replay missed guide addresses without changing any core sample or diagnostic.
      const auto work = std::min({samples - f.guideSamples,
          2u * f.input.settings.samplesPerFrame, kMaxGuideReplaySamples});
      barrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
      dispatchGuided({f.guideSamples, work});
      f.guideSamples += work;
    }
  }
#else
  dispatchSamples();
#endif
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  if (denoiser) {
    f.denoiseRecorded = false;
    const bool eligible = canGuide && f.data.controls.z == 0.0f && f.guideSamples == samples;
    f.denoisedSamples = f.guideReset || !eligible ? 0u : denoiser->filteredSamples;
    const auto interval = std::clamp(s.oidnSettings.sampleInterval, 1u, ReferenceOidnSettings::maxSampleInterval);
    const auto due = [&](uint32_t previous) {
      return eligible && samples > previous &&
          (samples - previous >= interval || samples == f.input.settings.sampleLimit ||
              (previous < handoffTarget && samples >= handoffTarget));
    };
#if CONTAINER_HAS_REFERENCE_OPTIX
    f.optixCapturedSamples = 0;
    if (s.oidnSettings.optix) {
      const bool publish = eligible && !f.guideReset && denoiser->readySamples > f.denoisedSamples;
      if (publish) f.denoisedSamples = denoiser->readySamples;
      if (due(f.denoisedSamples)) f.optixCapturedSamples = samples;
      denoiser->recordOptixCopies(cmd, s.extent, s.mean->image, f.optixCapturedSamples != 0, publish);
    }
#endif
#if CONTAINER_HAS_VK_OIDN
    if (!s.oidnSettings.optix && due(f.denoisedSamples)) {
      auto* oidn = denoiser;
      // A previously recorded but unsubmitted command never uploaded its weights.
      if (!oidn->weightsSubmitted) oidn->network.reset();
      if (!oidn->network) {
        const auto quality = s.oidnSettings.quality == ReferenceOidnQuality::Fast ? vkoidn::Quality::eFast :
            s.oidnSettings.quality == ReferenceOidnQuality::Balanced ? vkoidn::Quality::eBalanced : vkoidn::Quality::eHigh;
        const vkoidn::DenoiserCreateInfo info{.width = s.extent.width, .height = s.extent.height,
            .albedo = true, .normal = true, .quality = quality, .maxMemoryMB = kOidnIntermediateMemoryMiB};
        const auto result = vkoidn::Denoiser::create(*oidn->context, cmd, info, oidn->network);
        if (result != vkoidn::Result::eSuccess) {
          s.oidnMessage = std::string("vk_oidn network creation failed: ") + vkoidn::getResultString(result);
          oidn->failed = true;
          f.denoisedSamples = 0;
        }
      }
      if (!oidn->failed) {
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        if (oidn->queries) {
          vkCmdResetQueryPool(cmd, oidn->queries, image * 2, 2);
          vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, oidn->queries, image * 2);
        }
        const vkoidn::DenoiseInputs inputs{.colorView = {*s.mean->view}, .albedoView = {*oidn->albedo.view},
            .normalView = {*oidn->normal.view}, .outputView = {*oidn->filtered.view, VK_FORMAT_R32G32B32A32_SFLOAT}};
        const auto result = oidn->network->cmdDenoise(cmd, inputs);
        if (result == vkoidn::Result::eSuccess) {
          if (oidn->queries)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, oidn->queries, image * 2 + 1);
          f.denoisedSamples = samples;
          f.denoiseRecorded = true;
        } else {
          s.oidnMessage = std::string("vk_oidn dispatch failed: ") + vkoidn::getResultString(result);
          oidn->failed = true;
          f.denoisedSamples = 0;
        }
      }
    }
#endif
    if (f.denoisedSamples && handoffTarget) {
      targetWeight = static_cast<float>(std::min(f.denoisedSamples, handoffTarget)) / static_cast<float>(handoffTarget);
    }
  }
#endif
  bool freshDlss = false;
#if CONTAINER_HAS_DLSS_RR
  f.dlssEvaluated = s.dlss && f.data.controls.z == 0.0f && targetWeight < 1.0f;
  f.dlssYielded = s.dlss && targetWeight == 1.0f && (s.dlssYielded || s.dlss->status().active);
  if (f.dlssEvaluated) freshDlss = s.dlss->record(cmd, image, f.input, f.data, f.set);
#endif
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  if (denoiser && f.denoisedSamples) {
    const float weight = freshDlss ? targetWeight : 1.0f;
    if (s.oidnSettings.handoffSamples) f.handoffWeight = weight;
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    sets[3] = denoiser->set;
    vkCmdPushConstants(cmd, denoiser->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(weight), &weight);
    dispatch(static_cast<VkPipeline>(denoiser->compose), denoiser->pipelineLayout, 4);
  }
#else
  (void)freshDlss;
  (void)targetWeight;
#endif
  barrier(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,
      VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT);
  f.recorded=true;
}
void ReferencePathTracePass::commitFrame(uint32_t image) noexcept {
  auto& s=*impl_;
  if(image>=s.frames.size() || s.pendingImage!=image || !s.frames[image].recorded) return;
  auto& f=s.frames[image];
  if(f.reset) ++s.resets;
  s.lastSubmittedSamples=f.data.extentSamples.w;
  s.samples=f.data.extentSamples.z+s.lastSubmittedSamples;
  s.submittedKey=f.key;s.submittedScene=f.input.scene;s.submittedRevision=f.input.contentRevision;
  s.submittedEnvironment=f.input.environment;s.submittedSampler=f.input.environmentSampler;s.submittedPipeline=f.boundPipeline;
  s.submittedContent=f.boundContent;
  s.historyValid=true;s.initialized=true;f.recorded=false;f.submitted=true;s.pendingImage=UINT32_MAX;
  f.epoch=s.resets;f.submission=++s.submissionSerial;
  if (f.input.autofocusKey) s.autofocusImage = image;
#if CONTAINER_HAS_DLSS_RR
  s.dlssYielded = f.dlssYielded;
  if (s.dlss) {
    if (f.dlssEvaluated) s.dlss->commit(image);
    else s.dlss->invalidateHistory();
  }
#endif
  s.handoffWeight = f.handoffWeight;
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  if (auto* denoiser = s.denoiser.get()) {
    denoiser->initialized = true;
    denoiser->guideSamples = f.guideSamples;
    denoiser->guideContent = std::move(f.guideContent);
    denoiser->filteredSamples = f.denoisedSamples;
#if CONTAINER_HAS_VK_OIDN
    denoiser->weightsSubmitted = denoiser->network != nullptr;
    f.denoiseTimingPending = f.denoiseRecorded && denoiser->queries;
#endif
#if CONTAINER_HAS_REFERENCE_OPTIX
    if (denoiser->optix) {
      if (f.guideReset || f.data.controls.z != 0.0f) denoiser->readySamples = 0;
      denoiser->stagedSamples = f.optixCapturedSamples;
    }
#endif
  }
#endif
}
void ReferencePathTracePass::invalidateHistory() noexcept {
  impl_->historyValid=false;impl_->pendingImage=UINT32_MAX;
  impl_->handoffWeight = 0.0f;
#if CONTAINER_HAS_DLSS_RR
  impl_->dlssYielded = false;
  if (impl_->dlss) impl_->dlss->invalidateHistory();
#endif
}
uint64_t ReferencePathTracePass::diagnosticEpoch(uint32_t image) const {return image<impl_->frames.size()?impl_->frames[image].epoch:0;}
uint64_t ReferencePathTracePass::diagnosticSubmission(uint32_t image) const {return image<impl_->frames.size()?impl_->frames[image].submission:0;}
uint32_t ReferencePathTracePass::attemptedSamples() const {return impl_->samples;}
uint32_t ReferencePathTracePass::lastSubmittedSamples() const {return impl_->lastSubmittedSamples;}
uint64_t ReferencePathTracePass::submittedContentRevision() const {
  return impl_->historyValid ? impl_->submittedRevision : 0;
}
std::optional<ReferencePathLensSettings> ReferencePathTracePass::effectiveLens() const {
  if (!impl_->historyValid || impl_->submittedKey.trace.z == 0.0f) return {};
  return ReferencePathLensSettings{impl_->submittedKey.trace.z, impl_->submittedKey.trace.w};
}
float ReferencePathTracePass::effectiveFireflyClamp() const {
  return impl_->historyValid ? impl_->submittedKey.controls.x : 0.0f;
}
ReferenceGroundSettings ReferencePathTracePass::effectiveGround() const {
  ReferenceGroundSettings ground;
  if (!impl_->historyValid) return ground;
  const auto& key = impl_->submittedKey;
  ground.mode = static_cast<ReferenceGroundMode>(static_cast<uint32_t>(key.ground.x));
  ground.height = key.ground.y;
  if (ground.mode == ReferenceGroundMode::Surface) {
    ground.metallic = key.ground.z;
    ground.roughness = key.ground.w;
    ground.color = glm::vec3(key.groundColor);
  }
  return ground;
}
ReferenceOidnStatus ReferencePathTracePass::oidnStatus() const {
  const auto& s = *impl_;
  ReferenceOidnStatus status{.message = s.oidnMessage};
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  if (const auto* denoiser = s.denoiser.get()) {
    status.displayedSamples = s.historyValid ? denoiser->filteredSamples : 0;
    status.guideSamples = s.historyValid ? denoiser->guideSamples : 0;
    status.lastGpuMs = denoiser->lastGpuMs;
    status.allocatedBytes = denoiser->allocatedBytes();
  }
#endif
  return status;
}

ReferenceOidnQuality ReferencePathTracePass::oidnQuality() const {
  return impl_->oidnSettings.optix ? ReferenceOidnQuality::Off : impl_->oidnSettings.quality;
}

bool ReferencePathTracePass::optixSelected() const {
  return impl_->oidnSettings.optix;
}

UpscaleMode ReferencePathTracePass::dlssQuality() const {
  return impl_->oidnSettings.dlss;
}

ReferenceDlssStatus ReferencePathTracePass::dlssStatus() const {
  const auto& s = *impl_;
#if CONTAINER_HAS_DLSS_RR
  if (s.dlss) {
    auto status = s.dlss->status();
    if (s.dlssYielded) status.message = "handoff complete";
    return status;
  }
#endif
  return {.message = s.dlssMessage, .outputExtent = s.extent};
}

uint32_t ReferencePathTracePass::denoiserHandoffSamples() const noexcept {
  return impl_->oidnSettings.handoffSamples;
}

float ReferencePathTracePass::denoiserHandoffWeight() const noexcept {
  return impl_->handoffWeight;
}

void ReferencePathTracePass::setDenoising(const std::filesystem::path& root, ReferenceOidnSettings settings) {
  auto& s = *impl_;
  if (!s.mean) fail("reference images are not allocated");
  if (settings.handoffSamples > ReferenceOidnSettings::maxHandoffSamples) fail("invalid denoiser handoff sample count");
  if (settings.optix) settings.quality = ReferenceOidnQuality::Off;
  if (settings.dlss == UpscaleMode::Native || (!settings.optix && settings.quality == ReferenceOidnQuality::Off))
    settings.handoffSamples = 0;
  if (settings.dlss != UpscaleMode::Native && settings.handoffSamples == 0) {
    settings.optix = false;
    settings.quality = ReferenceOidnQuality::Off;
  }
  const auto previous = s.oidnSettings;
  s.oidnSettings = settings;
  const bool spatialChanged = previous.optix != settings.optix || previous.quality != settings.quality;
  const bool dlssChanged = previous.dlss != settings.dlss;
  const bool jointChanged = (previous.handoffSamples != 0) != (settings.handoffSamples != 0);
  if (!spatialChanged && !dlssChanged && !jointChanged) return;
#if CONTAINER_HAS_DLSS_RR
  if (dlssChanged) {
    s.dlss.reset();
    s.dlssYielded = false;
  }
#endif
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  if (spatialChanged) {
    for (auto& frame : s.frames) frame.denoiseTimingPending = false;
#if CONTAINER_HAS_VK_OIDN
    if (s.denoiser && !previous.optix && !settings.optix && settings.quality != ReferenceOidnQuality::Off) {
      // A different network quality uses the same aligned albedo/normal history.
      auto& denoiser = *s.denoiser;
      denoiser.network.reset();
      denoiser.weightsSubmitted = false;
      denoiser.filteredSamples = 0;
      denoiser.lastGpuMs.reset();
      denoiser.lastTimedSubmission = 0;
      denoiser.failed = false;
      s.oidnMessage = "ready";
    } else
#endif
      s.denoiser.reset();
  }
#endif

  uint64_t spatialBudget = s.imageBudget;
#if CONTAINER_HAS_DLSS_RR
  if (s.dlss) {
    spatialBudget -= std::min(spatialBudget, s.dlss->status().allocatedBytes);
#if CONTAINER_HAS_VK_OIDN
    if (!settings.optix) spatialBudget -= std::min(spatialBudget, kOidnJointAdmissionBytes);
#endif
  }
#endif
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  if (!s.denoiser)
#endif
    s.configureSpatialDenoiser(root, spatialBudget);

  uint64_t dlssBudget = s.imageBudget - uint64_t{s.extent.width} * s.extent.height * 48;
#if CONTAINER_HAS_VK_OIDN || CONTAINER_HAS_REFERENCE_OPTIX
  if (s.denoiser) {
    auto spatialBytes = s.denoiser->allocatedBytes();
#if CONTAINER_HAS_VK_OIDN
    if (!settings.optix)
      spatialBytes = std::max(spatialBytes, s.denoiser->guideImageBytes() + kOidnJointAdmissionBytes);
#endif
    dlssBudget -= std::min(dlssBudget, spatialBytes);
  }
#endif
#if CONTAINER_HAS_DLSS_RR
  if (!s.dlss)
#endif
    s.configureDlss(root, dlssBudget);
}

void ReferencePathTracePass::setOidnSampleInterval(uint32_t interval) {
  impl_->oidnSettings.sampleInterval = interval;
}

void ReferencePathTracePass::collectDenoiserTiming(uint32_t image) {
#if CONTAINER_HAS_DLSS_RR
  if (impl_->dlss) impl_->dlss->collectTiming(image);
#endif
#if CONTAINER_HAS_VK_OIDN
  auto& s = *impl_;
  auto* oidn = s.denoiser.get();
  if (!oidn || image >= s.frames.size() || !s.frames[image].denoiseTimingPending) return;
  auto& f = s.frames[image];
  if (f.submission <= oidn->lastTimedSubmission) {
    f.denoiseTimingPending = false;
    return;
  }
  std::array<uint64_t, 2> stamps{};
  const auto result = vkGetQueryPoolResults(s.device->device(), oidn->queries, image * 2, 2, sizeof(stamps),
      stamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
  if (result != VK_SUCCESS) return;
  const uint64_t mask = oidn->timestampBits == 64 ? UINT64_MAX : (uint64_t{1} << oidn->timestampBits) - 1;
  const uint64_t ticks = (stamps[1] - stamps[0]) & mask;
  oidn->lastGpuMs = static_cast<double>(ticks) * s.properties.limits.timestampPeriod / 1e6;
  oidn->lastTimedSubmission = f.submission;
  f.denoiseTimingPending = false;
#else
  (void)image;
#endif
}
std::optional<ReferenceAutofocusResult> ReferencePathTracePass::takeAutofocusResult() {
  auto& s = *impl_;
  if (!s.autofocusImage) return {};
  const auto& f = s.frames[*s.autofocusImage];
  if (vmaInvalidateAllocation(s.memory.allocator(), f.diagnostic.allocation,
          0, sizeof(ReferencePathReadback)) != VK_SUCCESS)
    throw std::runtime_error("Reference autofocus readback invalidate failed");
  ReferencePathReadback result{};
  std::memcpy(&result, f.diagnostic.allocation_info.pMappedData, sizeof(result));
  s.autofocusImage.reset();
  return ReferenceAutofocusResult{*f.input.autofocusKey, result.autofocus.x};
}
uint64_t ReferencePathTracePass::historyResets() const {return impl_->resets;}
uint64_t ReferencePathTracePass::allocatedBytes() const {
  auto& s=*impl_;uint64_t total=0;VmaAllocationInfo info{};
  for(const auto* image:{s.mean.get(),s.moment.get(),s.display.get()}) if(image) {vmaGetAllocationInfo(s.memory.allocator(),image->allocation,&info);total+=info.size;}
  for (const auto& frame : s.frames)
    for (const auto* buffer : {&frame.uniform, &frame.diagnostic, &frame.meshEmitters})
      if (buffer->allocation) {
        vmaGetAllocationInfo(s.memory.allocator(), buffer->allocation, &info);
        total += info.size;
      }
  return total;
}
ReferencePathTraceDiagnostics ReferencePathTracePass::diagnostics(uint32_t image) const {
  const auto& s=*impl_;
  if(image>=s.frames.size()) fail("diagnostic image out of range");
  const auto& f=s.frames[image];if(!f.submitted) return {};
  if(vmaInvalidateAllocation(s.memory.allocator(),f.diagnostic.allocation,0,sizeof(ReferencePathReadback))!=VK_SUCCESS)
    throw std::runtime_error("Reference path diagnostic invalidate failed");
  ReferencePathReadback result{};std::memcpy(&result,f.diagnostic.allocation_info.pMappedData,sizeof(result));
  uint32_t triangleWitness{};
  std::memcpy(&triangleWitness, &result.autofocus.w, sizeof(triangleWitness));
  if (result.diagnostics.rejectedSamples != 0 && triangleWitness != 0)
    std::fprintf(stderr, "[ReferencePath hit witness] triangle=%u bary=(%a,%a) failureStages=0x%08x\n",
        triangleWitness - 1, double(result.autofocus.y), double(result.autofocus.z),
        result.diagnostics.failureStages & ~ReferencePathTraceDiagnostics::kExperimentalMaterialFallbackMask);
  return result.diagnostics;
}
VkImage ReferencePathTracePass::displayImage() const {return impl_->display?impl_->display->image:VK_NULL_HANDLE;}
VkImage ReferencePathTracePass::meanImage() const {return impl_->mean?impl_->mean->image:VK_NULL_HANDLE;}
VkImage ReferencePathTracePass::momentImage() const {return impl_->moment?impl_->moment->image:VK_NULL_HANDLE;}
VkImageView ReferencePathTracePass::displayView() const {return impl_->display?static_cast<VkImageView>(*impl_->display->view):VK_NULL_HANDLE;}
VkImageView ReferencePathTracePass::meanView() const {return impl_->mean?static_cast<VkImageView>(*impl_->mean->view):VK_NULL_HANDLE;}
bool ReferencePathTracePass::usesNativeRayPipeline() const { return bool(impl_->nativeRays); }

std::shared_ptr<NativeReferenceRayPipeline> ReferencePathTracePass::prepareNativeRayReload(
    const std::filesystem::path& shaderRoot) const {
  const auto& s = *impl_;
  if (!s.nativeRays) return {};
  auto code = NativeReferenceRayPipeline::loadShaders(shaderRoot / "spv_shaders");
  if (s.nativeRays->matchesShaders(code)) return {};
  auto pipeline = std::make_shared<NativeReferenceRayPipeline>(*s.device, s.memory);
  pipeline->adopt(NativeReferenceRayPipeline::compile(*s.device, s.layout, std::move(code)));
  return pipeline;
}

void ReferencePathTracePass::publishNativeRayReload(
    std::shared_ptr<NativeReferenceRayPipeline> candidate) noexcept {
  if (candidate) impl_->nativeRays = std::move(candidate);
}
} // namespace container::renderer
