#include <Container/renderer/raytracing/RayInstanceTransformPass.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneData.h>
#include <Container/utility/VulkanDevice.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace container::renderer {
namespace {
struct PushConstants { uint32_t count,first,hasSurfaces,unused{}; };
struct Header { std::array<uint32_t,4> tail; uint32_t object,fallback; };
static_assert(sizeof(Header)==24 && sizeof(VkAccelerationStructureInstanceKHR)==64);
static_assert(offsetof(VkAccelerationStructureInstanceKHR,accelerationStructureReference)==56);
[[noreturn]] void fail(const char* message) {throw std::invalid_argument(std::string("GPU ray instances: ")+message);}
}
struct RayInstanceTransformPass::Impl {
  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::VulkanMemoryManager& memory;
  gpu::PipelineManager& pipelines;
  VkPhysicalDeviceProperties properties{};
  VkDescriptorSetLayout setLayout{};
  VkPipelineLayout layout{};
  gpu::ComputePipeline pipeline;
  Impl(std::shared_ptr<gpu::VulkanDevice> d,gpu::VulkanMemoryManager& m,gpu::PipelineManager& p)
      :device(std::move(d)),memory(m),pipelines(p) {
    if(!device) fail("missing device");
    vkGetPhysicalDeviceProperties(device->physicalDevice(),&properties);
  }
  ~Impl() {
    pipelines.destroyPipeline(pipeline);pipelines.destroyPipelineLayout(layout);
    pipelines.destroyDescriptorSetLayout(setLayout);
  }
};
struct RayInstanceTransformPass::Publication::State {
  std::shared_ptr<Impl> owner;
  gpu::AllocatedBuffer input{};
  VkDescriptorPool pool{};
  uint64_t bytes{};
  ~State() {owner->pipelines.destroyDescriptorPool(pool);owner->memory.destroyBuffer(input);}
};
RayInstanceTransformPass::Publication::Publication()=default;
RayInstanceTransformPass::Publication::~Publication()=default;
RayInstanceTransformPass::Publication::Publication(Publication&&) noexcept=default;
RayInstanceTransformPass::Publication& RayInstanceTransformPass::Publication::operator=(Publication&&) noexcept=default;
gpu::AllocatedBuffer RayInstanceTransformPass::Publication::inputBuffer() const {return state_?state_->input:gpu::AllocatedBuffer{};}
uint64_t RayInstanceTransformPass::Publication::uploadedBytes() const {return state_?state_->bytes:0;}
RayInstanceTransformPass::RayInstanceTransformPass(std::shared_ptr<gpu::VulkanDevice> d,gpu::VulkanMemoryManager& m,gpu::PipelineManager& p)
    :impl_(std::make_shared<Impl>(std::move(d),m,p)) {}
RayInstanceTransformPass::~RayInstanceTransformPass()=default;
VkDevice RayInstanceTransformPass::device() const noexcept {return impl_->device->device();}
void RayInstanceTransformPass::createResources(const std::filesystem::path& root) {
  auto& s=*impl_;
  if(s.pipeline) return;
  if(!s.device->rayQueriesEnabled() || s.properties.limits.maxPerStageDescriptorStorageBuffers<4)
    fail("required ray-query or storage-buffer support is unavailable");
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  for(uint32_t i=0;i<4;++i) bindings.push_back({i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
  s.setLayout=s.pipelines.createDescriptorSetLayout(bindings,{});
  s.layout=s.pipelines.createPipelineLayout({s.setLayout},{{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PushConstants)}});
  s.pipeline=s.pipelines.createComputePipeline(root/"spv_shaders"/"ray_instance_transform.comp.spv",s.layout,"ray_instance_transform");
}
void RayInstanceTransformPass::validate(const RaySceneGpuTransforms& source,std::span<const RaySceneInstance> instances) const {
  const auto& s=*impl_;
  if(source.producer!=this || !s.pipeline) fail("producer is not ready");
  const auto range=s.properties.limits.maxStorageBufferRange;
  const auto alignment=s.properties.limits.minStorageBufferOffsetAlignment;
  if(!source.objects.buffer || !source.objectCount || source.objects.size<uint64_t(source.objectCount)*sizeof(gpu::ObjectData) ||
     source.objects.size>range || source.objects.offset%alignment) fail("invalid object stream");
  if(instances.size()>range/64 || instances.size()>range/sizeof(Header)) fail("instance stream exceeds device range");
  if(source.surfaces.buffer && (source.surfaces.size<instances.size()*112 || source.surfaces.size>range || source.surfaces.offset%alignment))
    fail("invalid closest-hit output");
  if(source.surfaces.buffer==source.objects.buffer) fail("closest-hit output aliases source objects");
  uint64_t bytes=instances.size()*sizeof(Header);
  for(const auto& instance:instances) {
    if(instance.gpuObjectIndex==UINT32_MAX) bytes+=48;
    else if(instance.gpuObjectIndex>=source.objectCount) fail("object index out of range");
  }
  if(bytes>range) fail("packed input exceeds device range");
}
RayInstanceTransformPass::Publication RayInstanceTransformPass::record(VkCommandBuffer cmd,const RaySceneGpuTransforms& source,
    std::span<const RaySceneInstance> sourceInstances,std::span<const VkAccelerationStructureInstanceKHR> templates,gpu::BufferSlice output) {
  validate(source,sourceInstances);
  auto& s=*impl_;
  if(!cmd || templates.size()!=sourceInstances.size() || templates.empty() || !output.buffer ||
     output.size<templates.size()*64 || output.size>s.properties.limits.maxStorageBufferRange ||
     output.offset%s.properties.limits.minStorageBufferOffsetAlignment) fail("invalid instance output");
  if(output.buffer==source.objects.buffer || (source.surfaces.buffer && output.buffer==source.surfaces.buffer))
    fail("instance output aliases an input or closest-hit output");
  std::vector<std::byte> packed(templates.size()*sizeof(Header));
  for(size_t i=0;i<templates.size();++i) {
    Header header{};
    std::memcpy(header.tail.data(),reinterpret_cast<const std::byte*>(&templates[i])+48,16);
    header.object=sourceInstances[i].gpuObjectIndex;
    if(header.object==UINT32_MAX) {
      header.fallback=uint32_t(packed.size());
      const auto* first=reinterpret_cast<const std::byte*>(&templates[i].transform);
      packed.insert(packed.end(),first,first+48);
    }
    std::memcpy(packed.data()+i*sizeof(Header),&header,sizeof(header));
  }
  Publication result;result.state_=std::make_unique<Publication::State>();auto& state=*result.state_;state.owner=impl_;
  state.bytes=packed.size();
  state.input=s.memory.createBuffer(packed.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
  std::memcpy(state.input.allocation_info.pMappedData,packed.data(),packed.size());
  if(vmaFlushAllocation(s.memory.allocator(),state.input.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
    throw std::runtime_error("GPU ray instance upload failed");
  state.pool=s.pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4}},1);
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool=state.pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&s.setLayout;
  VkDescriptorSet set{};
  if(vkAllocateDescriptorSets(s.device->device(),&allocate,&set)!=VK_SUCCESS) throw std::runtime_error("GPU ray instance descriptor allocation failed");
  const auto surfaces=source.surfaces.buffer?source.surfaces:output;
  const std::array<VkDescriptorBufferInfo,4> buffers{{
    {source.objects.buffer,source.objects.offset,source.objects.size}, {state.input.buffer,0,packed.size()},
    {output.buffer,output.offset,output.size}, {surfaces.buffer,surfaces.offset,surfaces.size}}};
  std::array<VkWriteDescriptorSet,4> writes{};
  for(uint32_t i=0;i<4;++i) {
    writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;
    writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i];
  }
  vkUpdateDescriptorSets(s.device->device(),4,writes.data(),0,nullptr);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0,1,&barrier,0,nullptr,0,nullptr);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.layout,0,1,&set,0,nullptr);
  const uint32_t limit=s.properties.limits.maxComputeWorkGroupCount[0]*64;
  for(uint32_t first=0;first<templates.size();) {
    const uint32_t count=std::min<uint32_t>(limit,uint32_t(templates.size())-first);
    const PushConstants pc{count,first,source.surfaces.buffer?1u:0u};
    vkCmdPushConstants(cmd,s.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
    vkCmdDispatch(cmd,(count+63)/64,1,1);first+=count;
  }
  barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
  return result;
}
}
