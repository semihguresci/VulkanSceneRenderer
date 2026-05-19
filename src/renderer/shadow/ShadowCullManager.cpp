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

  if (shadowCullPipeline_ != VK_NULL_HANDLE)
	pipelineManager_.destroyPipeline(shadowCullPipeline_);
  if (shadowCullPipelineLayout_ != VK_NULL_HANDLE)
	pipelineManager_.destroyPipelineLayout(shadowCullPipelineLayout_);
  if (shadowCullPool_ != VK_NULL_HANDLE)
	pipelineManager_.destroyDescriptorPool(shadowCullPool_);
  if (shadowCullSetLayout_ != VK_NULL_HANDLE)
	pipelineManager_.destroyDescriptorSetLayout(shadowCullSetLayout_);
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
	if (shadowCullPool_ != VK_NULL_HANDLE) {
	  pipelineManager_.destroyDescriptorPool(shadowCullPool_);
	}
	shadowCullPool_ = pipelineManager_.createDescriptorPool(
		{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount * container::gpu::kShadowCascadeCount},
		 {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * container::gpu::kShadowCascadeCount * 4}},
		setCount * container::gpu::kShadowCascadeCount, 0);

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
				  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
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
	VkDeviceSize objectBufferSize) {
	for (uint32_t i = 0; i < shadowCullBuffers_.size(); ++i) {
	updateObjectSsboDescriptor(i, objectBuffer, objectBufferSize);
  }
}

void ShadowCullManager::updateObjectSsboDescriptor(
	uint32_t imageIndex,
	VkBuffer objectBuffer,
	VkDeviceSize objectBufferSize) {
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

	const auto spvData = container::util::readFile(compPath);
	VkShaderModule compModule =
		container::gpu::createShaderModule(device_->device(), spvData);

	VkPipelineShaderStageCreateInfo stage{};
	stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = compModule;
	stage.pName  = "computeMain";

	VkComputePipelineCreateInfo ci{};
	ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	ci.stage  = stage;
	ci.layout = shadowCullPipelineLayout_;

	shadowCullPipeline_ =
		pipelineManager_.createComputePipeline(ci, "shadow_cull");
	vkDestroyShaderModule(device_->device(), compModule, nullptr);
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

void ShadowCullManager::writeDescriptorSets(uint32_t imageIndex) {
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

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(5);

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
    writes.push_back(write);
  };

  addBufferWrite(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, shadowCullInfo);
  addBufferWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, objectInfo);
  addBufferWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, inputDrawInfo);
  addBufferWrite(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, outputDrawInfo);
  addBufferWrite(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, drawCountInfo);

  if (!writes.empty()) {
    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }
  }
}

}  // namespace container::renderer
