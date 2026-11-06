#include <Container/renderer/pathtracing/ReferencePathTracePass.h>
#include <Container/renderer/pathtracing/ReferenceFireflyClamp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>
#if CONTAINER_HAS_VK_OIDN
#include <vk_oidn/vk_oidn.h>
#endif

namespace container::renderer {
namespace {
struct alignas(16) ReferencePathReadback {
  ReferencePathTraceDiagnostics diagnostics{};
  glm::vec4 autofocus{};
};
static_assert(sizeof(ReferencePathReadback) == 32);

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
#if CONTAINER_HAS_VK_OIDN
    uint32_t denoisedSamples{};
    bool denoiseRecorded{}, denoiseTimingPending{};
#endif
  };
#if CONTAINER_HAS_VK_OIDN
  struct Oidn {
    std::shared_ptr<gpu::VulkanDevice> device;
    gpu::PipelineManager& pipelines;
    Image albedo, normal, filtered;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    gpu::ComputePipeline guided, emptyGuided, compose;
    VkQueryPool queries{};
    uint32_t timestampBits{};
    std::unique_ptr<vkoidn::Context> context;
    std::unique_ptr<vkoidn::Denoiser> network;
    bool weightsSubmitted{}, failed{};
    uint32_t filteredSamples{};
    uint64_t lastTimedSubmission{};
    std::optional<double> lastGpuMs;

    Oidn(const std::shared_ptr<gpu::VulkanDevice>& device, gpu::VulkanMemoryManager& memory,
         gpu::PipelineManager& pipelines, VkExtent2D extent)
        : device(device), pipelines(pipelines), albedo(device, memory, extent),
          normal(device, memory, extent), filtered(device, memory, extent, VK_IMAGE_USAGE_TRANSFER_DST_BIT) {}

    ~Oidn() {
      pipelines.destroyPipeline(guided);
      pipelines.destroyPipeline(emptyGuided);
      pipelines.destroyPipeline(compose);
      pipelines.destroyDescriptorPool(pool);
      pipelines.destroyPipelineLayout(pipelineLayout);
      pipelines.destroyDescriptorSetLayout(setLayout);
      if (queries) vkDestroyQueryPool(device->device(), queries, nullptr);
    }

    void initialize(const std::filesystem::path& root, uint32_t count,
                    VkDescriptorSetLayout materials, VkDescriptorSetLayout lights, VkDescriptorSetLayout local) {
      const std::array extensions{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
      const vkoidn::ContextCreateInfo info{device->instance(), device->physicalDevice(), device->device(),
          vkGetInstanceProcAddr, vkGetDeviceProcAddr, extensions.data(),
          device->referenceOidnCooperativeMatrix() ? 2u : 1u};
      const auto result = vkoidn::Context::create(info, context);
      if (result != vkoidn::Result::eSuccess)
        throw std::runtime_error(vkoidn::getResultString(result));
      if (!context->areImageViewsSupported()) throw std::runtime_error("storage image views unavailable");

      std::vector<VkDescriptorSetLayoutBinding> bindings;
      for (uint32_t binding = 0; binding < 3; ++binding)
        bindings.push_back({binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
      setLayout = pipelines.createDescriptorSetLayout(bindings, {});
      pipelineLayout = pipelines.createPipelineLayout({materials, lights, local, setLayout}, {});
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
  VkDescriptorSetLayout localLayout{},materialLayout{},lightLayout{};
  VkPipelineLayout layout{};VkDescriptorPool pool{};
  std::unique_ptr<Image> mean,moment,display;
  ReferenceOidnSettings oidnSettings{};
  std::string oidnMessage{"off"};
#if CONTAINER_HAS_VK_OIDN
  std::unique_ptr<Oidn> oidn;
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
    if(!device || !device->rayQueriesEnabled()) fail("Vulkan ray queries are unavailable");
    vkGetPhysicalDeviceProperties(device->physicalDevice(),&properties);
  }
  ~Impl() {clear();pipelines.destroyPipeline(pipeline);pipelines.destroyPipeline(emptyPipeline);pipelines.destroyPipelineLayout(layout);pipelines.destroyDescriptorSetLayout(localLayout);}
  void clear() {
#if CONTAINER_HAS_VK_OIDN
    oidn.reset();
#endif
    pipelines.destroyDescriptorPool(pool);pool={};
    for (auto& f : frames) {
      memory.destroyBuffer(f.uniform);
      memory.destroyBuffer(f.diagnostic);
      memory.destroyBuffer(f.meshEmitters);
    }
    frames.clear();mean.reset();moment.reset();display.reset();submittedContent.reset();
    initialized=false;historyValid=false;samples=0;lastSubmittedSamples=0;pendingImage=UINT32_MAX;
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
};
ReferencePathTracePass::ReferencePathTracePass(std::shared_ptr<gpu::VulkanDevice> device,gpu::VulkanMemoryManager& memory,gpu::PipelineManager& pipelines)
    :impl_(std::make_unique<Impl>(std::move(device),memory,pipelines)) {}
ReferencePathTracePass::~ReferencePathTracePass()=default;
void ReferencePathTracePass::createResources(const std::filesystem::path& root,uint32_t count,VkExtent2D extent,
    VkDescriptorSetLayout materials,VkDescriptorSetLayout lights,uint64_t budget, ReferenceOidnSettings oidn) {
  auto& s=*impl_;const auto& limits=s.properties.limits;
  if(!materials || !lights || !count || count>64 || !extent.width || !extent.height ||
      extent.width>limits.maxImageDimension2D || extent.height>limits.maxImageDimension2D ||
      (extent.width+7)/8>limits.maxComputeWorkGroupCount[0] || (extent.height+7)/8>limits.maxComputeWorkGroupCount[1] ||
      limits.maxComputeWorkGroupInvocations<64 || limits.maxComputeWorkGroupSize[0]<8 || limits.maxComputeWorkGroupSize[1]<8 ||
      limits.maxPerStageDescriptorStorageBuffers<10 || limits.maxDescriptorSetStorageBuffers<10 ||
      limits.maxPerStageDescriptorStorageImages<3 || limits.maxDescriptorSetStorageImages<3 ||
      limits.maxPerStageDescriptorSampledImages<gpu::kMaterialTextureDescriptorCapacity+1 ||
      limits.maxDescriptorSetSampledImages<gpu::kMaterialTextureDescriptorCapacity+1 ||
      uint64_t(extent.width)*extent.height*48>budget) fail("extent, descriptors, or image budget exceeds supported limits");
  VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(s.device->physicalDevice(),VK_FORMAT_R32G32B32A32_SFLOAT,&format);
  if((format.optimalTilingFeatures&(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))!=
      (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) fail("RGBA32F storage images are unavailable");
  if(s.pipeline && (s.materialLayout!=materials || s.lightLayout!=lights)) fail("shared descriptor layouts changed; recreate the pass");
  if(!s.pipeline) {
    const std::array<VkDescriptorType,13> types{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(uint32_t i=0;i<types.size();++i) bindings.push_back({i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    try {
      s.localLayout=s.pipelines.createDescriptorSetLayout(bindings,{});
      s.layout=s.pipelines.createPipelineLayout({materials,lights,s.localLayout},{});
      s.pipeline=s.pipelines.createComputePipeline(root/"spv_shaders"/"reference_pathtrace.comp.spv",s.layout,"reference_pathtrace");
      s.emptyPipeline=s.pipelines.createComputePipeline(root/"spv_shaders"/"reference_background.comp.spv",s.layout,"reference_background");
    } catch(...) {
      s.pipelines.destroyPipeline(s.pipeline);s.pipelines.destroyPipeline(s.emptyPipeline);s.pipelines.destroyPipelineLayout(s.layout);
      s.pipelines.destroyDescriptorSetLayout(s.localLayout);throw;
    }
    s.materialLayout=materials;s.lightLayout=lights;
  }
  s.clear();s.extent=extent;
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
  s.oidnSettings = oidn;
  s.oidnMessage = "off";
  if (oidn.quality == ReferenceOidnQuality::Off) return;
#if CONTAINER_HAS_VK_OIDN
  if (!s.device->referenceOidnEnabled()) {
    s.oidnMessage = "vk_oidn storage-image features unavailable on this device";
  } else if (limits.maxBoundDescriptorSets < 4 || limits.maxPerStageDescriptorStorageImages < 6 ||
             limits.maxDescriptorSetStorageImages < 6 ||
             uint64_t(extent.width) * extent.height > budget / 96u ||
             (format.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0) {
    s.oidnMessage = "vk_oidn guide images exceed device limits or the reference image budget";
  } else {
    try {
      auto backend = std::make_unique<Impl::Oidn>(s.device, s.memory, s.pipelines, extent);
      backend->initialize(root, count, materials, lights, s.localLayout);
      s.oidn = std::move(backend);
      s.oidnMessage = "ready";
    } catch (const std::exception& error) {
      s.oidnMessage = std::string("vk_oidn setup failed: ") + error.what();
    }
  }
#else
  s.oidnMessage = "vk_oidn 0.2.0 SDK was not enabled at build time";
#endif
}
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
  f.boundPipeline=input.emptyScene?static_cast<VkPipeline>(s.emptyPipeline):static_cast<VkPipeline>(s.pipeline);
  f.boundContent=input.emptyScene?s.emptyPipeline.contentIdentity():s.pipeline.contentIdentity();
#if CONTAINER_HAS_VK_OIDN
  if (s.oidn) {
    const auto& guided = input.emptyScene ? s.oidn->emptyGuided : s.oidn->guided;
    f.boundPipeline = static_cast<VkPipeline>(guided);
    f.boundContent = guided.contentIdentity();
  }
#endif
  f.reset=!s.historyValid || settings.sampleLimit<s.samples || std::memcmp(&f.key,&s.submittedKey,sizeof(f.key))!=0 ||
      sceneKey(source)!=sceneKey(s.submittedScene) || input.contentRevision!=s.submittedRevision ||
      input.environment!=s.submittedEnvironment || input.environmentSampler!=s.submittedSampler ||
      f.boundPipeline!=s.submittedPipeline || f.boundContent!=s.submittedContent;
  f.data=f.key;
  f.data.controls.y = input.autofocusKey ? 1.0f : 0.0f;
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
  if(!s.initialized) {
    std::array<VkImageMemoryBarrier2,3> barriers{};const std::array images{s.mean->image,s.moment->image,s.display->image};
    for(size_t i=0;i<3;++i) {
      auto& b=barriers[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      b.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;b.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT;
      b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=images[i];b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.imageMemoryBarrierCount=3;d.pImageMemoryBarriers=barriers.data();vkCmdPipelineBarrier2(cmd,&d);
#if CONTAINER_HAS_VK_OIDN
    if (s.oidn) s.oidn->initializeImages(cmd);
#endif
  }
  barrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_UNIFORM_READ_BIT);
  VkPipelineLayout layout = s.layout;
  uint32_t setCount = 3;
  std::array sets{f.input.materials, f.input.lights, f.set, VkDescriptorSet{}};
#if CONTAINER_HAS_VK_OIDN
  if (s.oidn) {
    layout = s.oidn->pipelineLayout;
    sets[setCount++] = s.oidn->set;
  }
#endif
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,f.boundPipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,setCount,sets.data(),0,nullptr);
  vkCmdDispatch(cmd,(s.extent.width+7)/8,(s.extent.height+7)/8,1);
#if CONTAINER_HAS_VK_OIDN
  if (auto* oidn = s.oidn.get()) {
    f.denoiseRecorded = false;
    const uint32_t samples = f.data.extentSamples.z + f.data.extentSamples.w;
    const bool eligible = f.input.settings.ground.mode != ReferenceGroundMode::ShadowMatte &&
        (f.input.data.limits.w & 8u) == 0 && !oidn->failed;
    f.denoisedSamples = f.reset || !eligible ? 0u : oidn->filteredSamples;
    const auto interval = std::clamp(s.oidnSettings.sampleInterval, 1u, ReferenceOidnSettings::maxSampleInterval);
    const bool due = eligible && samples > f.denoisedSamples &&
        (samples - f.denoisedSamples >= interval || samples == f.input.settings.sampleLimit);
    if (due) {
      // A previously recorded but unsubmitted command never uploaded its weights.
      if (!oidn->weightsSubmitted) oidn->network.reset();
      if (!oidn->network) {
        const auto quality = s.oidnSettings.quality == ReferenceOidnQuality::Fast ? vkoidn::Quality::eFast :
            s.oidnSettings.quality == ReferenceOidnQuality::Balanced ? vkoidn::Quality::eBalanced : vkoidn::Quality::eHigh;
        const vkoidn::DenoiserCreateInfo info{.width = s.extent.width, .height = s.extent.height,
            .albedo = true, .normal = true, .quality = quality, .maxMemoryMB = 256};
        const auto result = vkoidn::Denoiser::create(*oidn->context, cmd, info, oidn->network);
        if (result != vkoidn::Result::eSuccess) {
          s.oidnMessage = std::string("vk_oidn network creation failed: ") + vkoidn::getResultString(result);
          oidn->failed = true;
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
    if (f.denoisedSamples) {
      barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, oidn->compose);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, setCount, sets.data(), 0, nullptr);
      vkCmdDispatch(cmd, (s.extent.width + 7) / 8, (s.extent.height + 7) / 8, 1);
    }
  }
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
#if CONTAINER_HAS_VK_OIDN
  if (auto* oidn = s.oidn.get()) {
    oidn->weightsSubmitted = oidn->network != nullptr;
    oidn->filteredSamples = f.denoisedSamples;
    f.denoiseTimingPending = f.denoiseRecorded && oidn->queries;
  }
#endif
}
void ReferencePathTracePass::invalidateHistory() noexcept {
  impl_->historyValid=false;impl_->pendingImage=UINT32_MAX;
}
uint64_t ReferencePathTracePass::diagnosticEpoch(uint32_t image) const {return image<impl_->frames.size()?impl_->frames[image].epoch:0;}
uint64_t ReferencePathTracePass::diagnosticSubmission(uint32_t image) const {return image<impl_->frames.size()?impl_->frames[image].submission:0;}
uint32_t ReferencePathTracePass::attemptedSamples() const {return impl_->samples;}
uint32_t ReferencePathTracePass::lastSubmittedSamples() const {return impl_->lastSubmittedSamples;}
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
#if CONTAINER_HAS_VK_OIDN
  if (const auto* oidn = s.oidn.get()) {
    status.displayedSamples = s.historyValid ? oidn->filteredSamples : 0;
    status.lastGpuMs = oidn->lastGpuMs;
    status.allocatedBytes = oidn->network ? oidn->network->getMemoryUsage() : 0;
    for (const auto* image : {&oidn->albedo, &oidn->normal, &oidn->filtered}) {
      VmaAllocationInfo info{};
      vmaGetAllocationInfo(s.memory.allocator(), image->allocation, &info);
      status.allocatedBytes += info.size;
    }
  }
#endif
  return status;
}

ReferenceOidnQuality ReferencePathTracePass::oidnQuality() const {
  return impl_->oidnSettings.quality;
}

void ReferencePathTracePass::setOidnSampleInterval(uint32_t interval) {
  impl_->oidnSettings.sampleInterval = interval;
}

void ReferencePathTracePass::collectOidnTiming(uint32_t image) {
#if CONTAINER_HAS_VK_OIDN
  auto& s = *impl_;
  auto* oidn = s.oidn.get();
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
  if(vmaInvalidateAllocation(s.memory.allocator(),f.diagnostic.allocation,0,sizeof(ReferencePathTraceDiagnostics))!=VK_SUCCESS)
    throw std::runtime_error("Reference path diagnostic invalidate failed");
  ReferencePathTraceDiagnostics result{};std::memcpy(&result,f.diagnostic.allocation_info.pMappedData,sizeof(result));return result;
}
VkImage ReferencePathTracePass::displayImage() const {return impl_->display?impl_->display->image:VK_NULL_HANDLE;}
VkImage ReferencePathTracePass::meanImage() const {return impl_->mean?impl_->mean->image:VK_NULL_HANDLE;}
VkImage ReferencePathTracePass::momentImage() const {return impl_->moment?impl_->moment->image:VK_NULL_HANDLE;}
VkImageView ReferencePathTracePass::displayView() const {return impl_->display?static_cast<VkImageView>(*impl_->display->view):VK_NULL_HANDLE;}
VkImageView ReferencePathTracePass::meanView() const {return impl_->mean?static_cast<VkImageView>(*impl_->mean->view):VK_NULL_HANDLE;}
} // namespace container::renderer
