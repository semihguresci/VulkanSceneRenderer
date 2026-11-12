#include <Container/renderer/scene/GpuHierarchyPass.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneData.h>
#include <Container/utility/VulkanDevice.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace container::renderer {
namespace {
constexpr uint32_t maxNodes = 262144, maxLevels = 4096, maxObjects = 1000000;
struct alignas(16) LocalNode { glm::mat4 local; glm::uvec4 parentInfo; };
struct PushConstants { uint32_t phase, first, count, unused{}; };
static_assert(sizeof(LocalNode) == 80);
static_assert(sizeof(gpu::ObjectData) == 224);
constexpr uint32_t metadataStride=112;
static_assert(offsetof(gpu::ObjectData,objectInfo)==metadataStride);
static_assert(sizeof(glm::uvec2)==8);
[[noreturn]] void fail(const char* message) {
  throw std::invalid_argument(std::string("GPU hierarchy: ") + message);
}
bool affine(const glm::mat4& matrix) {
  for (int c=0;c<4;++c) for(int r=0;r<4;++r)
    if (!std::isfinite(matrix[c][r])) return false;
  return matrix[0][3]==0 && matrix[1][3]==0 && matrix[2][3]==0 && matrix[3][3]==1;
}
void barrier(VkCommandBuffer command, VkPipelineStageFlags from, VkPipelineStageFlags to,
             VkAccessFlags source, VkAccessFlags destination) {
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask=source; memory.dstAccessMask=destination;
  vkCmdPipelineBarrier(command,from,to,0,1,&memory,0,nullptr,0,nullptr);
}
}
struct GpuHierarchyPass::Impl {
  struct Frame {
    gpu::AllocatedBuffer locals{}, topology{}, worlds{}, mapping{}, metadata{};
    VkDeviceSize nodeCapacity{}, objectCapacity{};
    VkDeviceSize metadataCapacity{}, metadataBytes{};
    VkDescriptorSet set{};
    std::vector<LocalNode> cachedNodes;
    std::vector<uint32_t> cachedMapping, order, levels;
    std::vector<std::byte> cachedMetadata;
    gpu::BufferSlice objects{};
    HierarchyUploadStats stats{};
    bool prepared{}, worldsDirty{true};
    bool metadataReady{}, fullMetadataUpload{true};
    std::shared_ptr<const void> recordedPipelineContent, submittedPipelineContent;
  };
  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::VulkanMemoryManager& memory;
  gpu::PipelineManager& pipelines;
  VkPhysicalDeviceProperties properties{};
  VkDescriptorSetLayout setLayout{};
  VkPipelineLayout layout{};
  gpu::ComputePipeline pipeline{};
  VkDescriptorPool pool{};
  VkQueryPool queries{};
  uint32_t timestampBits{};
  std::vector<Frame> frames;
  Impl(std::shared_ptr<gpu::VulkanDevice> d,gpu::VulkanMemoryManager& m,gpu::PipelineManager& p)
      :device(std::move(d)),memory(m),pipelines(p) {
    if(!device) fail("missing device");
    vkGetPhysicalDeviceProperties(device->physicalDevice(),&properties);
    uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(),&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(),&count,families.data());
    timestampBits=families.at(device->queueFamilyIndices().graphicsFamily.value()).timestampValidBits;
  }
  ~Impl() {
    clear(); pipelines.destroyPipeline(pipeline); pipelines.destroyPipelineLayout(layout);
    pipelines.destroyDescriptorSetLayout(setLayout);
  }
  void clear() {
    if(queries) vkDestroyQueryPool(device->device(),queries,nullptr);queries={};
    pipelines.destroyDescriptorPool(pool);pool={};
    for(auto& f:frames) {
      memory.destroyBuffer(f.locals);memory.destroyBuffer(f.topology);
      memory.destroyBuffer(f.worlds);memory.destroyBuffer(f.mapping);
      memory.destroyBuffer(f.metadata);
    }
    frames.clear();
  }
  gpu::AllocatedBuffer host(VkDeviceSize bytes,bool gpuFrequent=false) {
    return memory.createBuffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        gpuFrequent?VMA_MEMORY_USAGE_AUTO:VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  }
  void upload(const gpu::AllocatedBuffer& buffer,const void* bytes,VkDeviceSize offset,VkDeviceSize count) {
    if(!count) return;
    std::memcpy(static_cast<std::byte*>(buffer.allocation_info.pMappedData)+offset,bytes,size_t(count));
    if(vmaFlushAllocation(memory.allocator(),buffer.allocation,offset,count)!=VK_SUCCESS)
      throw std::runtime_error("GPU hierarchy input flush failed");
  }
};
GpuHierarchyPass::GpuHierarchyPass(std::shared_ptr<gpu::VulkanDevice> d,gpu::VulkanMemoryManager& m,
                                 gpu::PipelineManager& p):impl_(std::make_unique<Impl>(std::move(d),m,p)) {}
GpuHierarchyPass::~GpuHierarchyPass()=default;
void GpuHierarchyPass::createResources(const std::filesystem::path& root,uint32_t count) {
  auto& s=*impl_;
  if(count==0 || count>64) fail("invalid image count");
  const auto& limits=s.properties.limits;
  if(limits.maxPerStageDescriptorStorageBuffers<6 || limits.maxDescriptorSetStorageBuffers<6 ||
     limits.maxComputeWorkGroupInvocations<64 || limits.maxComputeWorkGroupSize[0]<64)
    fail("device does not support the required compute storage buffers or workgroup size");
  if(!s.pipeline) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(uint32_t i=0;i<6;++i) bindings.push_back({i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    s.setLayout=s.pipelines.createDescriptorSetLayout(bindings,{});
    s.layout=s.pipelines.createPipelineLayout({s.setLayout},{{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PushConstants)}});
    s.pipeline=s.pipelines.createComputePipeline(root/"spv_shaders"/"hierarchy_transform.comp.spv",s.layout,"hierarchy_transform");
  }
  s.clear();s.frames.resize(count);
  s.pool=s.pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,count*6}},count);
  std::vector<VkDescriptorSetLayout> layouts(count,s.setLayout);
  std::vector<VkDescriptorSet> sets(count);
  VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  info.descriptorPool=s.pool;info.descriptorSetCount=count;info.pSetLayouts=layouts.data();
  if(vkAllocateDescriptorSets(s.device->device(),&info,sets.data())!=VK_SUCCESS)
    throw std::runtime_error("GPU hierarchy descriptor allocation failed");
  for(size_t i=0;i<count;++i) s.frames[i].set=sets[i];
  if(s.timestampBits) {
    VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query.queryType=VK_QUERY_TYPE_TIMESTAMP;query.queryCount=count*2;
    if(vkCreateQueryPool(s.device->device(),&query,nullptr,&s.queries)!=VK_SUCCESS)
      throw std::runtime_error("GPU hierarchy timestamp pool creation failed");
  }
}
void GpuHierarchyPass::prepareFrame(uint32_t image,std::span<const HierarchyLocalTransform> input,
    std::span<const uint32_t> mapping,gpu::BufferSlice objects) {
  const auto start=std::chrono::steady_clock::now();
  auto& s=*impl_;
  if(image>=s.frames.size()) fail("image out of range");
  if(input.size()>maxNodes || mapping.size()>maxObjects) fail("scene exceeds bounded capacity");
  if(!mapping.empty() && (!objects.buffer || objects.size<mapping.size()*sizeof(gpu::ObjectData) ||
       objects.offset%s.properties.limits.minStorageBufferOffsetAlignment)) fail("invalid object buffer");
  if(objects.size>s.properties.limits.maxStorageBufferRange || input.size()*sizeof(LocalNode)>s.properties.limits.maxStorageBufferRange)
    fail("storage buffer range exceeds device limit");
  auto& f=s.frames[image];
  double previousGpuMs=-1;
  if(s.queries && f.submittedPipelineContent) {
    std::array<uint64_t,4> values{};
    const auto result=vkGetQueryPoolResults(s.device->device(),s.queries,image*2,2,sizeof(values),values.data(),
        sizeof(uint64_t)*2,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if(result==VK_SUCCESS && values[1] && values[3]) {
      const uint64_t mask=s.timestampBits==64?UINT64_MAX:(uint64_t(1)<<s.timestampBits)-1;
      previousGpuMs=double((values[2]-values[0])&mask)*s.properties.limits.timestampPeriod/1e6;
    }
  }
  // Validate topology without recursion. Parent links, rather than editable
  // child lists, define the transform forest; arbitrary node ID order is legal.
  const bool sizeChanged=f.cachedNodes.size()!=input.size();
  bool topologyChanged=sizeChanged || !f.prepared;
  for(size_t i=0;i<input.size();++i) {
    if(!affine(input[i].local)) fail("non-finite or non-affine local transform");
    if(input[i].parent!=UINT32_MAX && input[i].parent>=input.size()) fail("parent out of range");
    topologyChanged|=!sizeChanged && f.cachedNodes[i].parentInfo.x!=input[i].parent;
  }
  for(const auto node:mapping) if(node!=UINT32_MAX && node>=input.size()) fail("object node out of range");
  const uint64_t metadataBytes=uint64_t(mapping.size()+std::count(mapping.begin(),mapping.end(),UINT32_MAX))*metadataStride;
  if(metadataBytes>s.properties.limits.maxStorageBufferRange) fail("object metadata exceeds device range");
  std::vector<uint32_t> order,levels;
  if(topologyChanged || !f.prepared) {
    std::vector<std::vector<uint32_t>> children(input.size());
    for(uint32_t i=0;i<input.size();++i)
      if(input[i].parent==UINT32_MAX) order.push_back(i);
      else children[input[i].parent].push_back(i);
    levels.push_back(0);
    uint32_t first=0;
    while(first<order.size()) {
      const auto end=uint32_t(order.size());levels.push_back(end);
      if(levels.size()>maxLevels+1) fail("hierarchy exceeds dispatch depth limit");
      for(uint32_t i=first;i<end;++i) for(const auto child:children[order[i]]) order.push_back(child);
      first=end;
    }
    if(order.size()!=input.size()) fail("cyclic hierarchy");
  }
  // Reject degeneracy for geometry before touching any uploaded frame state.
  // The CPU mirror otherwise produces nonfinite inverse-transpose matrices.
  // Only local determinants are needed: det(parent*local)=det(parent)*det(local).
  for(const auto& node:input) {
    const float determinant=glm::determinant(glm::mat3(node.local));
    if(!std::isfinite(determinant) || determinant==0) fail("singular local transform");
  }
  f.prepared=false;
  f.recordedPipelineContent.reset();
  const auto nodeCount=VkDeviceSize(std::max<size_t>(1,input.size()));
  const auto objectCount=VkDeviceSize(std::max<size_t>(1,mapping.size()));
  // Allocate replacements before releasing old buffers. Each frame is fence
  // retired, so resize never destroys another image's live compute inputs.
  if(nodeCount>f.nodeCapacity) {
    gpu::AllocatedBuffer locals{},topology{},worlds{};
    try {
      locals=s.host(nodeCount*sizeof(LocalNode));topology=s.host(nodeCount*sizeof(uint32_t));
      worlds=s.memory.createBuffer(nodeCount*sizeof(glm::mat4),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    } catch(...) {s.memory.destroyBuffer(locals);s.memory.destroyBuffer(topology);s.memory.destroyBuffer(worlds);throw;}
    s.memory.destroyBuffer(f.locals);s.memory.destroyBuffer(f.topology);s.memory.destroyBuffer(f.worlds);
    f.locals=locals;f.topology=topology;f.worlds=worlds;f.nodeCapacity=nodeCount;
    topologyChanged=true;
  }
  bool mappingChanged=f.cachedMapping.size()!=mapping.size() || !std::equal(mapping.begin(),mapping.end(),f.cachedMapping.begin());
  if(objectCount>f.objectCapacity) {
    auto replacement=s.host(objectCount*sizeof(glm::uvec2));
    s.memory.destroyBuffer(f.mapping);f.mapping=replacement;f.objectCapacity=objectCount;mappingChanged=true;
  }
  if(std::max<uint64_t>(16,metadataBytes)>f.metadataCapacity) {
    auto replacement=s.host(std::max<uint64_t>(16,metadataBytes),true);
    s.memory.destroyBuffer(f.metadata);f.metadata=replacement;
    f.metadataCapacity=std::max<uint64_t>(16,metadataBytes);f.fullMetadataUpload=true;
  }
  f.metadataBytes=metadataBytes;f.metadataReady=false;
  f.stats={};f.stats.nodes=uint32_t(input.size());f.stats.objects=uint32_t(mapping.size());
  f.stats.previousGpuMs=previousGpuMs;
  if(topologyChanged || f.levels.empty()) {
    // Capacity growth without a topology change still needs the cached order.
    if(levels.empty()) {order=f.order;levels=f.levels;}
    s.upload(f.topology,order.data(),0,order.size()*sizeof(uint32_t));
    f.stats.uploadedBytes+=order.size()*sizeof(uint32_t);
    f.order=std::move(order);f.levels=std::move(levels);
  }
  f.cachedNodes.resize(input.size());
  for(size_t i=0;i<input.size();++i) {
    const LocalNode value{input[i].local,glm::uvec4(input[i].parent,0,0,0)};
    if(topologyChanged || std::memcmp(&value,&f.cachedNodes[i],sizeof(value))!=0) {
      s.upload(f.locals,&value,i*sizeof(LocalNode),sizeof(value));
      f.cachedNodes[i]=value;++f.stats.dirtyLocals;f.stats.uploadedBytes+=sizeof(value);
    }
  }
  if(mappingChanged) {
    std::vector<glm::uvec2> bindings;bindings.reserve(mapping.size());
    uint32_t fallback=uint32_t(mapping.size())*metadataStride;
    for(const auto node:mapping) {
      bindings.emplace_back(node,node==UINT32_MAX?fallback:0u);
      if(node==UINT32_MAX) fallback+=metadataStride;
    }
    s.upload(f.mapping,bindings.data(),0,bindings.size()*sizeof(glm::uvec2));
    f.stats.uploadedBytes+=bindings.size()*sizeof(glm::uvec2);
    f.cachedMapping.assign(mapping.begin(),mapping.end());
  }
  f.objects=objects;
  f.worldsDirty |= topologyChanged || f.stats.dirtyLocals!=0;
  f.stats.levels=f.levels.empty()?0:uint32_t(f.levels.size()-1);
  if(!mapping.empty()) {
    const std::array<VkDescriptorBufferInfo,6> buffers{{
      {f.locals.buffer,0,f.nodeCapacity*sizeof(LocalNode)}, {f.topology.buffer,0,f.nodeCapacity*sizeof(uint32_t)},
      {f.worlds.buffer,0,f.nodeCapacity*sizeof(glm::mat4)}, {f.mapping.buffer,0,f.objectCapacity*sizeof(glm::uvec2)},
      {objects.buffer,objects.offset,objects.size},{f.metadata.buffer,0,f.metadataCapacity}}};
    std::array<VkWriteDescriptorSet,6> writes{};
    for(uint32_t i=0;i<6;++i) {
      writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=f.set;writes[i].dstBinding=i;
      writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i];
    }
    vkUpdateDescriptorSets(s.device->device(),6,writes.data(),0,nullptr);
  }
  f.prepared=true;
  f.stats.prepareMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
void GpuHierarchyPass::uploadObjectMetadata(uint32_t image,std::span<const gpu::ObjectData> objects) {
  const auto start=std::chrono::steady_clock::now();auto& s=*impl_;
  if(image>=s.frames.size() || !s.frames[image].prepared) fail("frame not prepared for metadata");
  auto& f=s.frames[image];
  if(objects.size()!=f.cachedMapping.size()) fail("object metadata count differs from bindings");
  f.metadataReady=false;
  try {
    const bool full=f.fullMetadataUpload || f.cachedMetadata.size()!=f.metadataBytes;
    f.cachedMetadata.resize(size_t(f.metadataBytes));
    auto* mapped=static_cast<std::byte*>(f.metadata.allocation_info.pMappedData);
    const auto copy=[&](uint64_t offset,const void* value) {
      auto* cache=f.cachedMetadata.data()+offset;
      if(full || std::memcmp(cache,value,metadataStride)!=0) {
        std::memcpy(cache,value,metadataStride);std::memcpy(mapped+offset,value,metadataStride);
        f.stats.objectMetadataBytes+=metadataStride;
      }
    };
    uint64_t fallback=objects.size()*metadataStride;
    for(size_t i=0;i<objects.size();++i) {
      const auto* bytes=reinterpret_cast<const std::byte*>(&objects[i]);
      copy(i*metadataStride,bytes+metadataStride);
      if(f.cachedMapping[i]==UINT32_MAX) {copy(fallback,bytes);fallback+=metadataStride;}
    }
    if(f.stats.objectMetadataBytes && vmaFlushAllocation(s.memory.allocator(),f.metadata.allocation,0,f.metadataBytes)!=VK_SUCCESS)
      throw std::runtime_error("GPU hierarchy metadata flush failed");
    f.fullMetadataUpload=false;f.metadataReady=true;
  } catch(...) {f.fullMetadataUpload=true;throw;}
  f.stats.objectMetadataMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
void GpuHierarchyPass::record(VkCommandBuffer command,uint32_t image) {
  auto& s=*impl_;
  if(!command || image>=s.frames.size() || !s.frames[image].prepared || !s.frames[image].metadataReady) fail("frame not prepared");
  auto& f=s.frames[image];
  if(!f.stats.objects) return;
  auto pipelineContent=s.pipeline.contentIdentity();
  if(s.queries) {
    vkCmdResetQueryPool(command,s.queries,image*2,2);
    vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,s.queries,image*2);
  }
  barrier(command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
  vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline);
  vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,s.layout,0,1,&f.set,0,nullptr);
  const auto dispatch=[&](uint32_t phase,uint32_t first,uint32_t count) {
    const uint32_t limit=s.properties.limits.maxComputeWorkGroupCount[0]*64;
    for(uint32_t offset=0;offset<count;) {
      const auto batch=std::min(limit,count-offset);
      const PushConstants pc{phase,first+offset,batch};
      vkCmdPushConstants(command,s.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
      vkCmdDispatch(command,(batch+63)/64,1,1);offset+=batch;
    }
  };
  const bool propagate=f.worldsDirty || f.submittedPipelineContent!=pipelineContent;
  f.stats.dispatchedLevels=propagate?f.stats.levels:0;
  if(propagate) for(size_t i=1;i<f.levels.size();++i) {
    dispatch(0,f.levels[i-1],f.levels[i]-f.levels[i-1]);
    barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
  }
  dispatch(1,0,f.stats.objects);
  barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
  if(s.queries) vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.queries,image*2+1);
  // Commit the shader used by this recording even if a reload publishes
  // another pipeline before commitFrame.
  f.recordedPipelineContent=std::move(pipelineContent);
}
void GpuHierarchyPass::commitFrame(uint32_t image) noexcept {
  auto& s=*impl_;
  if(image>=s.frames.size()) return;
  auto& f=s.frames[image];
  if(!f.recordedPipelineContent) return;
  f.worldsDirty=false;
  f.submittedPipelineContent=std::move(f.recordedPipelineContent);
}
gpu::BufferSlice GpuHierarchyPass::worldMatrices(uint32_t image) const {
  if(image>=impl_->frames.size()) return {};
  const auto& f=impl_->frames[image];return {f.worlds.buffer,0,uint64_t(f.stats.nodes)*sizeof(glm::mat4)};
}
HierarchyUploadStats GpuHierarchyPass::stats(uint32_t image) const {
  return image<impl_->frames.size()?impl_->frames[image].stats:HierarchyUploadStats{};
}
}  // namespace container::renderer
