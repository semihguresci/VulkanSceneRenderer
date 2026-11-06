#include "Container/renderer/shadow/ShadowCullManager.h"

#include "Container/renderer/culling/GpuCullDrawUploadPlanner.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <limits>
#include <type_traits>
#include <utility>

namespace container::renderer {

using container::gpu::GpuDrawIndexedIndirectCommand;
using container::gpu::ShadowCullData;
using container::gpu::ShadowCullPushConstants;

namespace {

void destroyBufferIfAllocated(
	container::gpu::AllocationManager& allocationManager,
	container::gpu::AllocatedBuffer& buffer) {
  if (buffer.buffer == VK_NULL_HANDLE) return;
  allocationManager.destroyBuffer(buffer);
  buffer = {};
}

void destroyBuffers(
	container::gpu::AllocationManager& allocationManager,
	std::vector<container::gpu::AllocatedBuffer>& buffers) {
  for (auto& buffer : buffers) {
	destroyBufferIfAllocated(allocationManager, buffer);
  }
  buffers.clear();
}

void destroyCascadeBuffers(
	container::gpu::AllocationManager& allocationManager,
	std::vector<std::array<container::gpu::AllocatedBuffer,
	                       container::gpu::kShadowCascadeCount>>& buffers) {
  for (auto& imageBuffers : buffers) {
	for (auto& buffer : imageBuffers) {
	  destroyBufferIfAllocated(allocationManager, buffer);
	}
  }
  buffers.clear();
}

void destroyCascadeBuffersAt(
	container::gpu::AllocationManager& allocationManager,
	std::array<container::gpu::AllocatedBuffer,
	           container::gpu::kShadowCascadeCount>& buffers) {
  for (auto& buffer : buffers) {
	destroyBufferIfAllocated(allocationManager, buffer);
  }
}

bool bufferReadyAt(
	const std::vector<container::gpu::AllocatedBuffer>& buffers,
	uint32_t imageIndex) {
  return imageIndex < buffers.size() &&
	     buffers[imageIndex].buffer != VK_NULL_HANDLE;
}

bool cascadeBufferReadyAt(
	const std::vector<std::array<container::gpu::AllocatedBuffer,
	                             container::gpu::kShadowCascadeCount>>& buffers,
	uint32_t imageIndex,
	uint32_t cascadeIndex) {
  return imageIndex < buffers.size() &&
	     cascadeIndex < container::gpu::kShadowCascadeCount &&
	     buffers[imageIndex][cascadeIndex].buffer != VK_NULL_HANDLE;
}

} // namespace

struct ShadowCullManager::PreparedGeneration::State {
  const ShadowCullManager* owner{};
  std::unique_ptr<ShadowCullManager> candidate;
  VkDescriptorPool pool{};
  std::vector<VkBuffer> objects,shadows;
  std::vector<VkDeviceSize> objectSizes;
  std::vector<uint32_t> capacities;
  VkDeviceSize shadowBytes{};
  bool published{};
};
ShadowCullManager::PreparedGeneration::PreparedGeneration() noexcept=default;
ShadowCullManager::PreparedGeneration::~PreparedGeneration()=default;
ShadowCullManager::PreparedGeneration::PreparedGeneration(PreparedGeneration&&) noexcept=default;
ShadowCullManager::PreparedGeneration& ShadowCullManager::PreparedGeneration::operator=(PreparedGeneration&&) noexcept=default;
bool ShadowCullManager::PreparedGeneration::ready() const noexcept {return state_ && state_->candidate;}
uint32_t ShadowCullManager::PreparedGeneration::maxDrawCount(uint32_t image) const noexcept {return ready()?state_->candidate->maxDrawCount(image):0;}
VkBuffer ShadowCullManager::PreparedGeneration::indirectDrawBuffer(uint32_t image,uint32_t cascade) const noexcept {return ready()?state_->candidate->indirectDrawBuffer(image,cascade):VK_NULL_HANDLE;}
VkBuffer ShadowCullManager::PreparedGeneration::drawCountBuffer(uint32_t image,uint32_t cascade) const noexcept {return ready()?state_->candidate->drawCountBuffer(image,cascade):VK_NULL_HANDLE;}
ShadowCullManager& ShadowCullManager::PreparedGeneration::manager() {
  if(!ready()) throw std::logic_error("empty shadow cull generation");return *state_->candidate;
}

ShadowCullManager::ShadowCullManager(
	std::shared_ptr<container::gpu::VulkanDevice> device,
	container::gpu::AllocationManager&            allocationManager,
	container::gpu::PipelineManager&              pipelineManager)
	: device_(std::move(device))
	, allocationManager_(allocationManager)
	, pipelineManager_(pipelineManager) {
}

ShadowCullManager::~ShadowCullManager() {
	if (ownedShadowCullUbo_.buffer != VK_NULL_HANDLE)
	allocationManager_.destroyBuffer(ownedShadowCullUbo_);
  destroyBuffers(allocationManager_, inputDrawBuffers_);
  destroyCascadeBuffers(allocationManager_, indirectDrawBuffers_);
  destroyCascadeBuffers(allocationManager_, drawCountBuffers_);

  destroyGenerationPool();
  if(!ownsSharedResources_) return;

  if (shadowCullPipeline_ != VK_NULL_HANDLE)
	pipelineManager_.destroyPipeline(shadowCullPipeline_);
  if (shadowCullPipelineLayout_ != VK_NULL_HANDLE)
	pipelineManager_.destroyPipelineLayout(shadowCullPipelineLayout_);
  if (shadowCullSetLayout_ != VK_NULL_HANDLE)
	pipelineManager_.destroyDescriptorSetLayout(shadowCullSetLayout_);
}

void ShadowCullManager::destroyGenerationPool() {
  if(ownsGenerationPool_) {
    if(shadowCullPool_) destroyOwnedDescriptorPool(device_->device(),shadowCullPool_,nullptr);
    shadowCullPool_=VK_NULL_HANDLE;
  } else pipelineManager_.destroyDescriptorPool(shadowCullPool_);
}

ShadowCullManager::PreparedGeneration ShadowCullManager::prepareGeneration(
    std::span<const container::gpu::AllocatedBuffer> objects,std::span<const size_t> capacities,
    std::span<const VkBuffer> shadows,VkDeviceSize shadowBytes,
    std::span<const DrawCommand> commands,uint64_t sourceRevision) const {
  const auto images=objects.size();
  if(!images || images>UINT32_MAX/(4u*container::gpu::kShadowCascadeCount) || images!=capacities.size() ||
      images!=shadows.size() || images!=objectSsboBuffers_.size() || shadowBytes<sizeof(ShadowCullData) || commands.size()>UINT32_MAX)
    throw std::invalid_argument("invalid detached shadow cull input tables");
  VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(device_->physicalDevice(),&properties);
  const auto count=std::max<size_t>(commands.size(),64);
  if(shadowBytes>properties.limits.maxUniformBufferRange || count>properties.limits.maxDrawIndirectCount ||
      count>properties.limits.maxStorageBufferRange/sizeof(GpuDrawIndexedIndirectCommand))
    throw std::length_error("detached shadow cull buffers exceed device limits");
  size_t minimumCapacity=SIZE_MAX;
  for(size_t image=0;image<images;++image) {
    if(!objects[image].buffer || !shadows[image] || !capacities[image] ||
        capacities[image]>properties.limits.maxStorageBufferRange/sizeof(container::gpu::ObjectData))
      throw std::invalid_argument("invalid detached shadow cull object/uniform binding");
    minimumCapacity=std::min(minimumCapacity,capacities[image]);
  }
  for(const auto& command:commands)
    if(command.objectIndex>=minimumCapacity || std::max(command.instanceCount,1u)>minimumCapacity-command.objectIndex)
      throw std::invalid_argument("detached shadow cull command exceeds object table");
  PreparedGeneration result;result.state_=std::make_unique<PreparedGeneration::State>();
  auto& state=*result.state_;state.owner=this;state.pool=shadowCullPool_;
  state.objects=objectSsboBuffers_;state.objectSizes=objectSsboSizes_;state.capacities=drawCapacities_;
  state.shadows=shadowCullBuffers_;state.shadowBytes=shadowCullUboSize_;
  state.candidate=std::make_unique<ShadowCullManager>(device_,allocationManager_,pipelineManager_);
  auto& next=*state.candidate;next.ownsSharedResources_=false;next.ownsGenerationPool_=true;
  next.shadowCullPipeline_=shadowCullPipeline_.share();next.shadowCullPipelineLayout_=shadowCullPipelineLayout_;
  next.shadowCullSetLayout_=shadowCullSetLayout_;next.shadowCullUboSize_=shadowBytes;
  if(!isReady()) {
    next.objectSsboBuffers_.assign(images,VK_NULL_HANDLE);next.objectSsboSizes_.assign(images,0);
    next.objectCounts_.assign(images,0);next.shadowCullBuffers_.assign(images,VK_NULL_HANDLE);
    next.resizePerImageBufferState(uint32_t(images));return result;
  }
  next.recreatePerFrameResources(uint32_t(images));next.uploadScratch_.reserve(count);
  next.ensureBufferCapacity(uint32_t(commands.size()));
  for(uint32_t image=0;image<images;++image) {
    next.updateShadowCullDescriptor(image,shadows[image],shadowBytes);
    next.updateObjectSsboDescriptor(image,objects[image].buffer,capacities[image]*sizeof(container::gpu::ObjectData));
    next.uploadDrawCommands(image,commands,sourceRevision);
  }
  return result;
}

bool ShadowCullManager::canExchangeGeneration(const PreparedGeneration& prepared) const noexcept {
  if(!prepared.ready() || prepared.state_->owner!=this) return false;
  const auto& state=*prepared.state_;const auto& other=*state.candidate;
  if(other.device_.get()!=device_.get() || other.shadowCullPipeline_!=shadowCullPipeline_ ||
      other.objectSsboBuffers_.size()!=objectSsboBuffers_.size()) return false;
  return state.published || (state.pool==shadowCullPool_ && state.objects==objectSsboBuffers_ &&
      state.objectSizes==objectSsboSizes_ && state.capacities==drawCapacities_ &&
      state.shadows==shadowCullBuffers_ && state.shadowBytes==shadowCullUboSize_);
}

bool ShadowCullManager::exchangeGeneration(PreparedGeneration& prepared) noexcept {
  if(!canExchangeGeneration(prepared)) return false;
  auto& other=*prepared.state_->candidate;using std::swap;
#define SHADOW_CULL_EXCHANGE(field) static_assert(std::is_nothrow_swappable_v<decltype(field)>);swap(field,other.field)
  SHADOW_CULL_EXCHANGE(maxDrawCount_);SHADOW_CULL_EXCHANGE(shadowCullUboSize_);SHADOW_CULL_EXCHANGE(uploadScratch_);
  SHADOW_CULL_EXCHANGE(lastUploadSourceData_);SHADOW_CULL_EXCHANGE(lastUploadSourceSize_);SHADOW_CULL_EXCHANGE(lastUploadSourceRevision_);
  SHADOW_CULL_EXCHANGE(shadowCullBuffers_);SHADOW_CULL_EXCHANGE(objectSsboBuffers_);SHADOW_CULL_EXCHANGE(objectSsboSizes_);
  SHADOW_CULL_EXCHANGE(objectCounts_);SHADOW_CULL_EXCHANGE(inputDrawBuffers_);SHADOW_CULL_EXCHANGE(indirectDrawBuffers_);
  SHADOW_CULL_EXCHANGE(drawCountBuffers_);SHADOW_CULL_EXCHANGE(drawCapacities_);SHADOW_CULL_EXCHANGE(shadowCullPool_);
  SHADOW_CULL_EXCHANGE(shadowCullSets_);SHADOW_CULL_EXCHANGE(ownsGenerationPool_);
#undef SHADOW_CULL_EXCHANGE
  prepared.state_->published=true;return true;
}

bool ShadowCullManager::isReady() const {
  return shadowCullPipeline_ != VK_NULL_HANDLE &&
         device_->enabledFeatures().drawIndirectFirstInstance == VK_TRUE &&
         device_->enabledFeatures().multiDrawIndirect == VK_TRUE &&
         device_->enabledVulkan12Features().drawIndirectCount == VK_TRUE;
}

void ShadowCullManager::createResources(const std::filesystem::path& shaderDir,
										uint32_t descriptorSetCount) {
  if (shadowCullSetLayout_ == VK_NULL_HANDLE) {
	const std::array<VkDescriptorSetLayoutBinding, 5> bindings{{
		{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	}};
	const std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(), 0);
	shadowCullSetLayout_ = pipelineManager_.createDescriptorSetLayout(
		{bindings.begin(), bindings.end()}, bindingFlags);
  }

	if (ownedShadowCullUbo_.buffer == VK_NULL_HANDLE) {
	ownedShadowCullUbo_ = allocationManager_.createBuffer(
		sizeof(ShadowCullData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		VMA_MEMORY_USAGE_AUTO,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			VMA_ALLOCATION_CREATE_MAPPED_BIT);
	shadowCullUboSize_ = sizeof(ShadowCullData);
  }

	recreatePerFrameResources(descriptorSetCount);
	createShadowCullPipeline(shaderDir);
	for (uint32_t i = 0; i < shadowCullBuffers_.size(); ++i) {
	  writeDescriptorSets(i);
  }
}

void ShadowCullManager::recreatePerFrameResources(uint32_t descriptorSetCount) {
	const uint32_t setCount = std::max<uint32_t>(1u, descriptorSetCount);
	const std::vector<uint32_t> previousCapacities = drawCapacities_;
	const uint32_t previousCapacity = maxDrawCount_;
  destroyGenerationPool();
  const uint32_t total=setCount*container::gpu::kShadowCascadeCount;
  if(ownsGenerationPool_) {
    const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,total},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,total*4}}};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets=total;info.poolSizeCount=uint32_t(sizes.size());info.pPoolSizes=sizes.data();
    if(createOwnedDescriptorPool(device_->device(),&info,nullptr,&shadowCullPool_)!=VK_SUCCESS)
      throw std::runtime_error("failed to create detached shadow cull descriptor pool");
  } else shadowCullPool_=pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,total},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,total*4}},total,0);

	const VkBuffer fallbackObjectBuffer =
		objectSsboBuffers_.empty() ? VK_NULL_HANDLE : objectSsboBuffers_.back();
	const VkDeviceSize fallbackObjectSize =
		objectSsboSizes_.empty() ? 0 : objectSsboSizes_.back();
	const uint32_t fallbackObjectCount =
		objectCounts_.empty() ? 0 : objectCounts_.back();
	shadowCullSets_.assign(setCount * container::gpu::kShadowCascadeCount, VK_NULL_HANDLE);
	shadowCullBuffers_.assign(setCount, ownedShadowCullUbo_.buffer);
	objectSsboBuffers_.resize(setCount, fallbackObjectBuffer);
	objectSsboSizes_.resize(setCount, fallbackObjectSize);
	objectCounts_.resize(setCount, fallbackObjectCount);
	resizePerImageBufferState(setCount);
	lastUploadSourceData_.assign(setCount, nullptr);
	lastUploadSourceSize_.assign(setCount, 0u);
	lastUploadSourceRevision_.assign(setCount, 0u);
	std::vector<VkDescriptorSetLayout> layouts(shadowCullSets_.size(), shadowCullSetLayout_);
	VkDescriptorSetAllocateInfo allocInfo{
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	allocInfo.descriptorPool     = shadowCullPool_;
	allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
	allocInfo.pSetLayouts        = layouts.data();
	if (vkAllocateDescriptorSets(device_->device(), &allocInfo,
							 shadowCullSets_.data()) != VK_SUCCESS) {
	  throw std::runtime_error("failed to allocate shadow cull descriptor sets");
	}
	for (uint32_t imageIndex = 0; imageIndex < setCount; ++imageIndex) {
	  uint32_t capacity = previousCapacity;
	  if (!previousCapacities.empty()) {
		const uint32_t sourceIndex = std::min<uint32_t>(
			imageIndex, static_cast<uint32_t>(previousCapacities.size() - 1u));
		capacity = previousCapacities[sourceIndex];
	  }
	  if (capacity > 0u) {
		static_cast<void>(ensureBufferCapacity(imageIndex, capacity));
	  }
	}
	for (uint32_t imageIndex = 0; imageIndex < setCount; ++imageIndex) {
	  writeDescriptorSets(imageIndex);
	}
}

bool ShadowCullManager::ensureBufferCapacity(uint32_t maxDrawCount) {
  const uint32_t imageCount =
	  std::max<uint32_t>(1u, static_cast<uint32_t>(shadowCullBuffers_.size()));
  bool resized = false;
  for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
	resized = ensureBufferCapacity(imageIndex, maxDrawCount) || resized;
  }
  return resized;
}

bool ShadowCullManager::ensureBufferCapacity(uint32_t imageIndex,
                                             uint32_t maxDrawCount) {
  const uint32_t imageCount =
	  std::max<uint32_t>(1u, static_cast<uint32_t>(shadowCullBuffers_.size()));
  if (imageIndex >= imageCount) {
	return false;
  }
  resizePerImageBufferState(imageCount);

  const uint32_t capacity = std::max(maxDrawCount, 64u);
  const bool buffersReady =
	  bufferReadyAt(inputDrawBuffers_, imageIndex) &&
	  cascadeBufferReadyAt(indirectDrawBuffers_, imageIndex,
	                       container::gpu::kShadowCascadeCount - 1u) &&
	  cascadeBufferReadyAt(drawCountBuffers_, imageIndex,
	                       container::gpu::kShadowCascadeCount - 1u);
  if (imageIndex < drawCapacities_.size() &&
	  maxDrawCount <= drawCapacities_[imageIndex] && buffersReady) {
	return false;
  }

  destroyBufferIfAllocated(allocationManager_, inputDrawBuffers_[imageIndex]);
  destroyCascadeBuffersAt(allocationManager_, indirectDrawBuffers_[imageIndex]);
  destroyCascadeBuffersAt(allocationManager_, drawCountBuffers_[imageIndex]);

	inputDrawBuffers_[imageIndex] = allocationManager_.createBuffer(
		sizeof(GpuDrawIndexedIndirectCommand) * capacity,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VMA_MEMORY_USAGE_AUTO,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			VMA_ALLOCATION_CREATE_MAPPED_BIT);

	for (uint32_t cascadeIndex = 0;
		 cascadeIndex < container::gpu::kShadowCascadeCount;
		 ++cascadeIndex) {
	  indirectDrawBuffers_[imageIndex][cascadeIndex] =
		  allocationManager_.createBuffer(
			  sizeof(GpuDrawIndexedIndirectCommand) * capacity,
			  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
				  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	  drawCountBuffers_[imageIndex][cascadeIndex] =
		  allocationManager_.createBuffer(
			  sizeof(uint32_t),
			  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
				  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
				  VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	}

  drawCapacities_[imageIndex] = capacity;
  updateGlobalDrawCapacity();
  if (imageIndex < lastUploadSourceData_.size()) {
	lastUploadSourceData_[imageIndex] = nullptr;
	lastUploadSourceSize_[imageIndex] = 0u;
	lastUploadSourceRevision_[imageIndex] = 0u;
  }
  writeDescriptorSets(imageIndex);
  return true;
}

void ShadowCullManager::uploadDrawCommands(
	uint32_t imageIndex,
	const std::vector<DrawCommand>& commands,
	uint64_t sourceRevision) {
  uploadDrawCommands(imageIndex,std::span<const DrawCommand>(commands),sourceRevision);
}

void ShadowCullManager::uploadDrawCommands(uint32_t imageIndex,
    std::span<const DrawCommand> commands,uint64_t sourceRevision) {
  const GpuCullDrawUploadCacheView uploadCache{
	  .sourceData = std::span<const DrawCommand *const>(
		  lastUploadSourceData_.data(), lastUploadSourceData_.size()),
	  .sourceSizes = std::span<const size_t>(
		  lastUploadSourceSize_.data(), lastUploadSourceSize_.size()),
	  .sourceRevisions = std::span<const uint64_t>(
		  lastUploadSourceRevision_.data(), lastUploadSourceRevision_.size())};
  const GpuCullDrawUploadPlan uploadPlan = buildGpuCullDrawUploadPlan(
	  {.inputBufferReady = bufferReadyAt(inputDrawBuffers_, imageIndex),
	   .cache = uploadCache,
	   .imageIndex = imageIndex,
	   .sourceData = commands.data(),
	   .sourceSize = commands.size(),
	   .sourceRevision = sourceRevision,
	   .maxObjectCount = maxDrawCount(imageIndex)});
  if (!uploadPlan.uploadsBuffer()) return;

  const uint32_t count = uploadPlan.drawCount;
  uploadScratch_.resize(count);
  auto& gpuCmds = uploadScratch_;
  for (uint32_t i = 0; i < count; ++i) {
	gpuCmds[i].indexCount    = commands[i].indexCount;
	gpuCmds[i].instanceCount = std::max(commands[i].instanceCount, 1u);
	gpuCmds[i].firstIndex    = commands[i].firstIndex;
	gpuCmds[i].vertexOffset  = 0;
	gpuCmds[i].firstInstance = commands[i].objectIndex;
  }

  SceneController::writeToBuffer(allocationManager_,
								 inputDrawBuffers_[imageIndex],
								 gpuCmds.data(),
								 sizeof(GpuDrawIndexedIndirectCommand) * count);
  lastUploadSourceData_[imageIndex] = commands.data();
  lastUploadSourceSize_[imageIndex] = commands.size();
  lastUploadSourceRevision_[imageIndex] = sourceRevision;
}

void ShadowCullManager::updateObjectSsboDescriptor(
	VkBuffer objectBuffer,
	VkDeviceSize objectBufferSize) noexcept {
	for (uint32_t i = 0; i < shadowCullBuffers_.size(); ++i) {
	updateObjectSsboDescriptor(i, objectBuffer, objectBufferSize);
  }
}

void ShadowCullManager::updateObjectSsboDescriptor(
	uint32_t imageIndex,
	VkBuffer objectBuffer,
	VkDeviceSize objectBufferSize) noexcept {
	if (imageIndex >= objectSsboBuffers_.size() ||
		imageIndex >= objectSsboSizes_.size() ||
		imageIndex >= objectCounts_.size()) {
	  return;
	}
  if (objectSsboBuffers_[imageIndex] == objectBuffer &&
      objectSsboSizes_[imageIndex] == objectBufferSize) {
    return;
  }
	objectSsboBuffers_[imageIndex] = objectBuffer;
	objectSsboSizes_[imageIndex]   = objectBufferSize;
	objectCounts_[imageIndex]      = static_cast<uint32_t>(
		objectBufferSize / sizeof(container::gpu::ObjectData));
	writeDescriptorSets(imageIndex);
}

void ShadowCullManager::updateShadowCullDescriptor(
	uint32_t imageIndex,
	VkBuffer shadowCullBuffer,
	VkDeviceSize shadowCullBufferSize) {
	if (imageIndex >= shadowCullBuffers_.size()) return;
  if (shadowCullBuffers_[imageIndex] == shadowCullBuffer &&
      shadowCullUboSize_ == shadowCullBufferSize) {
    return;
  }
	shadowCullBuffers_[imageIndex] = shadowCullBuffer;
	shadowCullUboSize_    = shadowCullBufferSize;
  writeDescriptorSets(imageIndex);
}

bool ShadowCullManager::canDispatchCascadeCull(uint32_t imageIndex,
                                               uint32_t cascadeIndex) const {
  if (!isReady() ||
	  imageIndex >= shadowCullBuffers_.size() ||
	  imageIndex >= objectSsboBuffers_.size() ||
	  imageIndex >= objectCounts_.size() ||
	  cascadeIndex >= container::gpu::kShadowCascadeCount ||
	  shadowCullBuffers_[imageIndex] == VK_NULL_HANDLE ||
	  objectSsboBuffers_[imageIndex] == VK_NULL_HANDLE ||
	  objectCounts_[imageIndex] == 0u ||
	  !bufferReadyAt(inputDrawBuffers_, imageIndex) ||
	  !cascadeBufferReadyAt(indirectDrawBuffers_, imageIndex, cascadeIndex) ||
	  !cascadeBufferReadyAt(drawCountBuffers_, imageIndex, cascadeIndex) ||
	  maxDrawCount(imageIndex) == 0u) {
	return false;
  }
  const size_t setIndex = descriptorSetIndex(imageIndex, cascadeIndex);
  return setIndex < shadowCullSets_.size() &&
	     shadowCullSets_[setIndex] != VK_NULL_HANDLE;
}

bool ShadowCullManager::dispatchCascadeCull(VkCommandBuffer cmd,
									uint32_t imageIndex,
											uint32_t cascadeIndex,
											uint32_t drawCount,
											uint32_t outputOffset) {
	if (!canDispatchCascadeCull(imageIndex, cascadeIndex)) {
	return false;
  }
	const auto& drawCountBuffer = drawCountBuffers_[imageIndex][cascadeIndex];

	vkCmdFillBuffer(cmd, drawCountBuffer.buffer, 0, sizeof(uint32_t), 0);

	VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	fillBarrier.dstAccessMask =
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
					 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					 0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

	const size_t setIndex = descriptorSetIndex(imageIndex, cascadeIndex);
	const VkDescriptorSet descriptorSet = shadowCullSets_[setIndex];
	const uint32_t groupCount = (drawCount + 63u) / 64u;
	if (groupCount == 0u) {
	  return false;
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadowCullPipeline_);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
					shadowCullPipelineLayout_, 0, 1,
					&descriptorSet, 0, nullptr);

	ShadowCullPushConstants pc{};
	pc.drawCount    = drawCount;
	pc.cascadeIndex = cascadeIndex;
	pc.outputOffset = outputOffset;
	pc.objectCount  = objectCounts_[imageIndex];
	vkCmdPushConstants(cmd, shadowCullPipelineLayout_,
				   VK_SHADER_STAGE_COMPUTE_BIT, 0,
				   sizeof(ShadowCullPushConstants), &pc);

	vkCmdDispatch(cmd, groupCount, 1, 1);

	VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
					  VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd,
				 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
					 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
					 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				 0, 1, &barrier, 0, nullptr, 0, nullptr);
	return true;
}

void ShadowCullManager::createShadowCullPipeline(
	const std::filesystem::path& shaderDir) {
	auto compPath = shaderDir / "spv_shaders" / "shadow_cull.comp.spv";
	if (!std::filesystem::exists(compPath)) return;

	VkPushConstantRange pcRange{};
	pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcRange.size       = sizeof(ShadowCullPushConstants);

	if (shadowCullSetLayout_ == VK_NULL_HANDLE) return;

	if (shadowCullPipelineLayout_ == VK_NULL_HANDLE) {
	  shadowCullPipelineLayout_ = pipelineManager_.createPipelineLayout(
		  {shadowCullSetLayout_}, {pcRange});
	}

	if (shadowCullPipelineLayout_ == VK_NULL_HANDLE) return;

	shadowCullPipeline_ = pipelineManager_.createComputePipeline(
	    compPath, shadowCullPipelineLayout_, "shadow_cull");
}

size_t ShadowCullManager::descriptorSetIndex(uint32_t imageIndex,
											 uint32_t cascadeIndex) const {
  return static_cast<size_t>(imageIndex) * container::gpu::kShadowCascadeCount +
		 static_cast<size_t>(cascadeIndex);
}

void ShadowCullManager::resizePerImageBufferState(uint32_t imageCount) {
  const uint32_t count = std::max<uint32_t>(1u, imageCount);
  for (uint32_t i = count; i < inputDrawBuffers_.size(); ++i) {
	destroyBufferIfAllocated(allocationManager_, inputDrawBuffers_[i]);
  }
  for (uint32_t i = count; i < indirectDrawBuffers_.size(); ++i) {
	destroyCascadeBuffersAt(allocationManager_, indirectDrawBuffers_[i]);
  }
  for (uint32_t i = count; i < drawCountBuffers_.size(); ++i) {
	destroyCascadeBuffersAt(allocationManager_, drawCountBuffers_[i]);
  }

  inputDrawBuffers_.resize(count);
  indirectDrawBuffers_.resize(count);
  drawCountBuffers_.resize(count);
  drawCapacities_.resize(count, 0u);
  updateGlobalDrawCapacity();
}

void ShadowCullManager::updateGlobalDrawCapacity() {
  maxDrawCount_ = 0u;
  for (const uint32_t capacity : drawCapacities_) {
	maxDrawCount_ = std::max(maxDrawCount_, capacity);
  }
}

void ShadowCullManager::writeDescriptorSets(uint32_t imageIndex) noexcept {
  if (imageIndex >= shadowCullBuffers_.size()) return;
	if (imageIndex >= objectSsboBuffers_.size() ||
		imageIndex >= objectSsboSizes_.size()) {
	  return;
	}
	const VkBuffer objectSsboBuffer = objectSsboBuffers_[imageIndex];
	const VkDeviceSize objectSsboSize = objectSsboSizes_[imageIndex];
  for (uint32_t cascadeIndex = 0;
	   cascadeIndex < container::gpu::kShadowCascadeCount; ++cascadeIndex) {
	const size_t setIndex = descriptorSetIndex(imageIndex, cascadeIndex);
	if (setIndex >= shadowCullSets_.size() ||
		shadowCullSets_[setIndex] == VK_NULL_HANDLE) continue;

  VkDescriptorBufferInfo shadowCullInfo{
		shadowCullBuffers_[imageIndex], 0,
	  shadowCullUboSize_ > 0 ? shadowCullUboSize_ : sizeof(ShadowCullData)};
  VkDescriptorBufferInfo objectInfo{
		objectSsboBuffer, 0, objectSsboSize};
  VkDescriptorBufferInfo inputDrawInfo{
	  bufferReadyAt(inputDrawBuffers_, imageIndex)
		  ? inputDrawBuffers_[imageIndex].buffer
		  : VK_NULL_HANDLE,
	  0,
	  sizeof(GpuDrawIndexedIndirectCommand) *
		  std::max(maxDrawCount(imageIndex), 1u)};
  VkDescriptorBufferInfo outputDrawInfo{
	  cascadeBufferReadyAt(indirectDrawBuffers_, imageIndex, cascadeIndex)
		  ? indirectDrawBuffers_[imageIndex][cascadeIndex].buffer
		  : VK_NULL_HANDLE,
	  0,
	  sizeof(GpuDrawIndexedIndirectCommand) *
		  std::max(maxDrawCount(imageIndex), 1u)};
  VkDescriptorBufferInfo drawCountInfo{
	  cascadeBufferReadyAt(drawCountBuffers_, imageIndex, cascadeIndex)
		  ? drawCountBuffers_[imageIndex][cascadeIndex].buffer
		  : VK_NULL_HANDLE,
	  0, sizeof(uint32_t)};

    std::array<VkWriteDescriptorSet,5> writes{};
    uint32_t writeCount{};

  const auto addBufferWrite = [&](uint32_t binding,
                                  VkDescriptorType descriptorType,
                                  const VkDescriptorBufferInfo& bufferInfo) {
    if (bufferInfo.buffer == VK_NULL_HANDLE || bufferInfo.range == 0) return;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet          = shadowCullSets_[setIndex];
    write.dstBinding      = binding;
    write.descriptorCount = 1;
    write.descriptorType  = descriptorType;
    write.pBufferInfo     = &bufferInfo;
      writes[writeCount++]=write;
  };

  addBufferWrite(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, shadowCullInfo);
  addBufferWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, objectInfo);
  addBufferWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, inputDrawInfo);
  addBufferWrite(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, outputDrawInfo);
  addBufferWrite(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, drawCountInfo);

    if (writeCount) {
    vkUpdateDescriptorSets(device_->device(),
                             writeCount,
                           writes.data(), 0, nullptr);
  }
  }
}

}  // namespace container::renderer
