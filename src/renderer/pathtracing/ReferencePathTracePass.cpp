#include <Container/renderer/pathtracing/ReferencePathTracePass.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <tuple>

namespace container::renderer {
namespace {
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
    Image(std::shared_ptr<gpu::VulkanDevice> device,gpu::VulkanMemoryManager& m,VkExtent2D extent):memory(m) {
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R32G32B32A32_SFLOAT;
      info.extent={extent.width,extent.height,1};info.mipLevels=1;info.arrayLayers=1;
      info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
      info.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
    gpu::AllocatedBuffer uniform{},diagnostic{};
    VkDescriptorSet set{};
    ReferencePathTraceInput input{};
    ReferencePathTraceData key{},data{};
    bool prepared{},recorded{},submitted{},reset{};
    VkPipeline boundPipeline{};
    std::shared_ptr<const void> boundContent;
    uint64_t epoch{},submission{};
  };
  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::VulkanMemoryManager& memory;gpu::PipelineManager& pipelines;
  gpu::ComputePipeline pipeline{},emptyPipeline{};
  VkDescriptorSetLayout localLayout{},materialLayout{},lightLayout{};
  VkPipelineLayout layout{};VkDescriptorPool pool{};
  std::unique_ptr<Image> mean,moment,display;
  std::vector<Frame> frames;
  VkExtent2D extent{};VkPhysicalDeviceProperties properties{};
  bool initialized{},historyValid{};
  uint32_t samples{},pendingImage{UINT32_MAX};uint64_t resets{},submissionSerial{};
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
    pipelines.destroyDescriptorPool(pool);pool={};
    for(auto& f:frames) {memory.destroyBuffer(f.uniform);memory.destroyBuffer(f.diagnostic);}
    frames.clear();mean.reset();moment.reset();display.reset();submittedContent.reset();
    initialized=false;historyValid=false;samples=0;pendingImage=UINT32_MAX;
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
    VkDescriptorSetLayout materials,VkDescriptorSetLayout lights,uint64_t budget) {
  auto& s=*impl_;const auto& limits=s.properties.limits;
  if(!materials || !lights || !count || count>64 || !extent.width || !extent.height ||
      extent.width>limits.maxImageDimension2D || extent.height>limits.maxImageDimension2D ||
      (extent.width+7)/8>limits.maxComputeWorkGroupCount[0] || (extent.height+7)/8>limits.maxComputeWorkGroupCount[1] ||
      limits.maxComputeWorkGroupInvocations<64 || limits.maxComputeWorkGroupSize[0]<8 || limits.maxComputeWorkGroupSize[1]<8 ||
      limits.maxPerStageDescriptorStorageBuffers<9 || limits.maxDescriptorSetStorageBuffers<9 ||
      limits.maxPerStageDescriptorStorageImages<3 || limits.maxDescriptorSetStorageImages<3 ||
      limits.maxPerStageDescriptorSampledImages<gpu::kMaterialTextureDescriptorCapacity+1 ||
      limits.maxDescriptorSetSampledImages<gpu::kMaterialTextureDescriptorCapacity+1 ||
      uint64_t(extent.width)*extent.height*48>budget) fail("extent, descriptors, or image budget exceeds supported limits");
  VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(s.device->physicalDevice(),VK_FORMAT_R32G32B32A32_SFLOAT,&format);
  if((format.optimalTilingFeatures&(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))!=
      (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) fail("RGBA32F storage images are unavailable");
  if(s.pipeline && (s.materialLayout!=materials || s.lightLayout!=lights)) fail("shared descriptor layouts changed; recreate the pass");
  if(!s.pipeline) {
    const std::array<VkDescriptorType,12> types{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
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
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5*count},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3*count},
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
      f.diagnostic=s.memory.createBuffer(sizeof(ReferencePathTraceDiagnostics),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    }
  } catch(...) {s.clear();throw;}
}
void ReferencePathTracePass::prepareFrame(uint32_t image,const ReferencePathTraceInput& input) {
  auto& s=*impl_;
  if(image>=s.frames.size() || !s.mean) fail("frame resources are not prepared");
  const auto& settings=input.settings;
  if(!settings.samplesPerFrame || settings.samplesPerFrame>64 || !settings.sampleLimit || settings.sampleLimit>16777216 ||
      !settings.maxBounces || settings.maxBounces>32 || settings.rouletteDepth>32 || !std::isfinite(settings.rayBias) || settings.rayBias<=0 ||
      !std::isfinite(settings.maxDistance) || settings.maxDistance<=settings.rayBias*4) fail("invalid sampling settings");
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
  auto& f=s.frames[image];f.prepared=false;f.recorded=false;
  f.input=input;f.key=input.data;
  f.key.limits.x=source.instanceCount;f.key.limits.y=source.triangleCount;
  f.key.extentSamples={s.extent.width,s.extent.height,0,0};
  f.key.sampling={settings.maxBounces,settings.rouletteDepth,settings.seed,0};
  f.key.trace={settings.rayBias,settings.maxDistance,0,0};
  f.boundPipeline=input.emptyScene?static_cast<VkPipeline>(s.emptyPipeline):static_cast<VkPipeline>(s.pipeline);
  f.boundContent=input.emptyScene?s.emptyPipeline.contentIdentity():s.pipeline.contentIdentity();
  f.reset=!s.historyValid || settings.sampleLimit<s.samples || std::memcmp(&f.key,&s.submittedKey,sizeof(f.key))!=0 ||
      sceneKey(source)!=sceneKey(s.submittedScene) || input.contentRevision!=s.submittedRevision ||
      input.environment!=s.submittedEnvironment || input.environmentSampler!=s.submittedSampler ||
      f.boundPipeline!=s.submittedPipeline || f.boundContent!=s.submittedContent;
  f.data=f.key;
  const uint32_t first=f.reset?0:s.samples;
  f.data.extentSamples.z=first;f.data.extentSamples.w=std::min(settings.samplesPerFrame,settings.sampleLimit-first);
  s.write(f.uniform,&f.data,sizeof(f.data));
  const ReferencePathTraceDiagnostics zero{};s.write(f.diagnostic,&zero,sizeof(zero));
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
  s.buffer(f.set,11,f.diagnostic.buffer,sizeof(ReferencePathTraceDiagnostics),VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
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
  }
  barrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_UNIFORM_READ_BIT);
  const std::array sets{f.input.materials,f.input.lights,f.set};
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,f.boundPipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.layout,0,3,sets.data(),0,nullptr);
  vkCmdDispatch(cmd,(s.extent.width+7)/8,(s.extent.height+7)/8,1);
  barrier(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,
      VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT);
  f.recorded=true;
}
void ReferencePathTracePass::commitFrame(uint32_t image) noexcept {
  auto& s=*impl_;
  if(image>=s.frames.size() || s.pendingImage!=image || !s.frames[image].recorded) return;
  auto& f=s.frames[image];
  if(f.reset) ++s.resets;
  s.samples=f.data.extentSamples.z+f.data.extentSamples.w;
  s.submittedKey=f.key;s.submittedScene=f.input.scene;s.submittedRevision=f.input.contentRevision;
  s.submittedEnvironment=f.input.environment;s.submittedSampler=f.input.environmentSampler;s.submittedPipeline=f.boundPipeline;
  s.submittedContent=f.boundContent;
  s.historyValid=true;s.initialized=true;f.recorded=false;f.submitted=true;s.pendingImage=UINT32_MAX;
  f.epoch=s.resets;f.submission=++s.submissionSerial;
}
void ReferencePathTracePass::invalidateHistory() noexcept {impl_->historyValid=false;impl_->pendingImage=UINT32_MAX;}
uint64_t ReferencePathTracePass::diagnosticEpoch(uint32_t image) const {return image<impl_->frames.size()?impl_->frames[image].epoch:0;}
uint64_t ReferencePathTracePass::diagnosticSubmission(uint32_t image) const {return image<impl_->frames.size()?impl_->frames[image].submission:0;}
uint32_t ReferencePathTracePass::attemptedSamples() const {return impl_->samples;}
uint64_t ReferencePathTracePass::historyResets() const {return impl_->resets;}
uint64_t ReferencePathTracePass::allocatedBytes() const {
  auto& s=*impl_;uint64_t total=0;VmaAllocationInfo info{};
  for(const auto* image:{s.mean.get(),s.moment.get(),s.display.get()}) if(image) {vmaGetAllocationInfo(s.memory.allocator(),image->allocation,&info);total+=info.size;}
  for(const auto& frame:s.frames) for(const auto& buffer:{frame.uniform,frame.diagnostic}) {vmaGetAllocationInfo(s.memory.allocator(),buffer.allocation,&info);total+=info.size;}
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
