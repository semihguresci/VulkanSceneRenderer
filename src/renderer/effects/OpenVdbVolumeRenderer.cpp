#include "Container/renderer/effects/OpenVdbVolumeRenderer.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {
struct VolumeConstants {
  glm::vec4 minimumStep;
  glm::vec4 maximumPadding;
  glm::uvec4 extentFlags; // width, height, orthographic, count | plane << 8
  glm::vec4 sectionPlane;
};
static_assert(sizeof(VolumeConstants) == 64);

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                  VkImageLayout newLayout, VkPipelineStageFlags2 source,
                  VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destination,
                  VkAccessFlags2 destinationAccess) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = source;
  barrier.srcAccessMask = sourceAccess;
  barrier.dstStageMask = destination;
  barrier.dstAccessMask = destinationAccess;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}
}

struct OpenVdbVolumeRenderer::PendingUpload {
  MediumResources medium;
  std::unique_ptr<container::gpu::StagingBuffer> staging;
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  VkFence fence{VK_NULL_HANDLE};
  uint32_t index{0};
  bool submitted{false};
};

struct OpenVdbVolumeRenderer::PreparedSceneBindings::State {
  struct Frame {
    VkDescriptorSet set{VK_NULL_HANDLE};
    std::array<VkDescriptorBufferInfo,4> buffers{};
  };
  const OpenVdbVolumeRenderer* owner{};
  std::shared_ptr<const uint8_t> identity;
  uint64_t epoch{};
  VkDescriptorPool pool{VK_NULL_HANDLE};
  VkDescriptorSetLayout layout{VK_NULL_HANDLE};
  VkPipeline pipeline{VK_NULL_HANDLE};
  std::vector<Frame> frames;
};

OpenVdbVolumeRenderer::PreparedSceneBindings::PreparedSceneBindings() noexcept=default;
OpenVdbVolumeRenderer::PreparedSceneBindings::~PreparedSceneBindings()=default;
OpenVdbVolumeRenderer::PreparedSceneBindings::PreparedSceneBindings(PreparedSceneBindings&&) noexcept=default;
OpenVdbVolumeRenderer::PreparedSceneBindings& OpenVdbVolumeRenderer::PreparedSceneBindings::operator=(
    PreparedSceneBindings&&) noexcept=default;
OpenVdbVolumeRenderer::PreparedSceneBindings::operator bool() const noexcept {
  return state_ && state_->owner;
}

OpenVdbVolumeRenderer::PreparedSceneBindings OpenVdbVolumeRenderer::prepareSceneBindings(
    std::span<const container::gpu::AllocatedBuffer> lighting,
    std::span<const container::gpu::AllocatedBuffer> sceneClips,
    std::span<const container::gpu::AllocatedBuffer> pointLights,
    std::span<const container::gpu::AllocatedBuffer> areaLights) const {
  if(!device_ || device_->device()==VK_NULL_HANDLE || sets_.size()>64 || lighting.size()>64 ||
      descriptorBindingEpoch_==UINT64_MAX)
    throw std::invalid_argument("OpenVDB scene bindings require a valid bounded descriptor table");
  if(!sets_.empty()) {
    if(!descriptorPool_ || !setLayout_ || !pipeline_ || mediumBuffers_.size()!=sets_.size() ||
        lighting.size()!=sets_.size() || sceneClips.size()!=sets_.size() ||
        pointLights.size()!=sets_.size() || areaLights.size()!=sets_.size())
      throw std::invalid_argument("OpenVDB lighting and clipping bindings must cover every prepared frame");
    const auto requireBuffer=[](const container::gpu::AllocatedBuffer& buffer,VkDeviceSize bytes) {
      // Typed renderer buffers provide the logical range/usage contract. The
      // VMA size is an additional lower-bound check, not a VkBuffer size query.
      if(!buffer.buffer || !buffer.allocation || buffer.allocation_info.size<bytes)
        throw std::invalid_argument("OpenVDB scene binding contains a null or undersized typed buffer");
    };
    for(size_t i=0;i<sets_.size();++i) {
      if(!sets_[i]) throw std::invalid_argument("OpenVDB scene binding contains a null destination set");
      requireBuffer(lighting[i],sizeof(container::gpu::LightingData));
      requireBuffer(sceneClips[i],sizeof(container::gpu::SceneClipState));
      requireBuffer(pointLights[i],sizeof(container::gpu::PointLightData)*container::gpu::kMaxClusteredLights);
      requireBuffer(areaLights[i],sizeof(container::gpu::AreaLightData)*container::gpu::kMaxAreaLights);
    }
  }
  PreparedSceneBindings result;
  result.state_=std::make_unique<PreparedSceneBindings::State>();
  auto& state=*result.state_;
  state.owner=this;state.identity=descriptorBindingOwner_;state.epoch=descriptorBindingEpoch_;
  state.pool=descriptorPool_;state.layout=setLayout_;state.pipeline=pipeline_;
  state.frames.reserve(sets_.size());
  for(size_t i=0;i<sets_.size();++i) {
    PreparedSceneBindings::State::Frame row;row.set=sets_[i];
    row.buffers={VkDescriptorBufferInfo{lighting[i].buffer,0,sizeof(container::gpu::LightingData)},
        VkDescriptorBufferInfo{sceneClips[i].buffer,0,sizeof(container::gpu::SceneClipState)},
        VkDescriptorBufferInfo{pointLights[i].buffer,0,sizeof(container::gpu::PointLightData)*container::gpu::kMaxClusteredLights},
        VkDescriptorBufferInfo{areaLights[i].buffer,0,sizeof(container::gpu::AreaLightData)*container::gpu::kMaxAreaLights}};
    state.frames.push_back(row);
  }
  return result;
}

bool OpenVdbVolumeRenderer::canPublishSceneBindings(const PreparedSceneBindings& value) const noexcept {
  if(!value) return false;
  const auto& state=*value.state_;
  if(state.owner!=this || state.identity!=descriptorBindingOwner_ || state.epoch!=descriptorBindingEpoch_ ||
      descriptorBindingEpoch_==UINT64_MAX || state.pool!=descriptorPool_ || state.layout!=setLayout_ ||
      state.pipeline!=pipeline_ || state.frames.size()!=sets_.size() ||
      (!sets_.empty() && mediumBuffers_.size()!=sets_.size())) return false;
  for(size_t i=0;i<sets_.size();++i) if(state.frames[i].set!=sets_[i]) return false;
  return true;
}

bool OpenVdbVolumeRenderer::publishSceneBindings(PreparedSceneBindings& value) noexcept {
  if(!canPublishSceneBindings(value)) return false;
  constexpr std::array<uint32_t,4> bindings{1,6,7,15};
  for(const auto& row:value.state_->frames) {
    std::array<VkWriteDescriptorSet,4> writes{};
    for(uint32_t i=0;i<writes.size();++i) {
      auto& write=writes[i];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet=row.set;write.dstBinding=bindings[i];write.descriptorCount=1;
      write.descriptorType=i<2?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo=&row.buffers[i];
    }
    vkUpdateDescriptorSets(device_->device(),static_cast<uint32_t>(writes.size()),writes.data(),0,nullptr);
  }
  ++descriptorBindingEpoch_;
  value.state_->owner=nullptr;
  return true;
}

OpenVdbVolumeRenderer::OpenVdbVolumeRenderer(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager& allocator,
    container::gpu::PipelineManager& pipelines, VkCommandPool /*commandPool*/)
    : device_(std::move(device)), allocator_(allocator), pipelines_(pipelines) {}

void OpenVdbVolumeRenderer::destroyMedium(MediumResources& medium) noexcept {
  const auto device = device_->device();
  if (medium.densityView) destroyVulkanImageView(device, medium.densityView, nullptr);
  if (medium.densityImage)
    vmaDestroyImage(allocator_.memoryManager()->allocator(), medium.densityImage, medium.densityAllocation);
  if (medium.temperatureView) destroyVulkanImageView(device, medium.temperatureView, nullptr);
  if (medium.temperatureImage)
    vmaDestroyImage(allocator_.memoryManager()->allocator(), medium.temperatureImage, medium.temperatureAllocation);
  allocator_.destroyBuffer(medium.sparseDensity);
  medium = {};
}

OpenVdbVolumeRenderer::~OpenVdbVolumeRenderer() {
  const auto device = device_->device();
  if (pendingUpload_) destroyUpload(*pendingUpload_, true);
  pendingUpload_.reset();
  if (commandPool_) destroyOwnedCommandPool(device, commandPool_, nullptr);
  if (descriptorPool_) destroyOwnedDescriptorPool(device, descriptorPool_, nullptr);
  for (auto& buffer : mediumBuffers_) allocator_.destroyBuffer(buffer);
  if (dummyOitHeadView_) destroyVulkanImageView(device, dummyOitHeadView_, nullptr);
  if (dummyOitHeadImage_)
    vmaDestroyImage(allocator_.memoryManager()->allocator(), dummyOitHeadImage_, dummyOitHeadAllocation_);
  allocator_.destroyBuffer(dummySparseBuffer_);
  pipelines_.destroyPipeline(pipeline_);
  pipelines_.destroyPipelineLayout(layout_);
  pipelines_.destroyDescriptorSetLayout(setLayout_);
  if (sampler_) destroyOwnedSampler(device, sampler_, nullptr);
  for (auto& medium : volumes_) destroyMedium(medium);
}

void OpenVdbVolumeRenderer::destroyUpload(PendingUpload& upload, bool waitForCompletion) noexcept {
  const auto device = device_->device();
  if (waitForCompletion && upload.submitted)
    vkWaitForFences(device, 1, &upload.fence, VK_TRUE, UINT64_MAX);
  if (upload.commandBuffer)
    vkFreeCommandBuffers(device, commandPool_, 1, &upload.commandBuffer);
  if (upload.fence) destroyOwnedFence(device, upload.fence, nullptr);
  upload.staging.reset();
  destroyMedium(upload.medium);
  upload.commandBuffer = VK_NULL_HANDLE;
  upload.fence = VK_NULL_HANDLE;
  upload.submitted = false;
}

std::unique_ptr<OpenVdbVolumeRenderer::PendingUpload> OpenVdbVolumeRenderer::submitVolumeUpload(
    const container::geometry::OpenVdbVolume& volume) {
  const bool sparse = !volume.sparseDensity.empty();
  uint64_t count = 1;
  for (int axis = 0; axis < 3; ++axis) {
    if (!volume.dimensions[axis] || volume.dimensions[axis] > 256 ||
        !std::isfinite(volume.boundsMin[axis]) || !std::isfinite(volume.boundsMax[axis]) ||
        !std::isfinite(volume.boundsMax[axis] - volume.boundsMin[axis]) ||
        volume.boundsMax[axis] <= volume.boundsMin[axis])
      throw std::invalid_argument("Invalid OpenVDB volume dimensions or bounds");
    count *= volume.dimensions[axis];
  }
  if (!sparse && volume.density.size() != count)
    throw std::invalid_argument("Invalid OpenVDB density payload size");
  if (!sparse && !volume.temperature.empty() && volume.temperature.size() != count)
    throw std::invalid_argument("Invalid OpenVDB temperature payload size");
  for (float value : volume.density)
    if (!std::isfinite(value) || value < 0.0f || value > 1.0e6f)
      throw std::invalid_argument("Invalid OpenVDB density sample");
  for (float value : volume.temperature)
    if (!std::isfinite(value) || value < 0.0f || value > 1.0e6f)
      throw std::invalid_argument("Invalid OpenVDB temperature sample");

  auto pending = std::make_unique<PendingUpload>();
  auto& result = pending->medium;
  result.minimum = volume.boundsMin;
  result.maximum = volume.boundsMax;
  result.stepSize = std::numeric_limits<float>::max();
  for (int axis = 0; axis < 3; ++axis)
    result.stepSize = std::min(result.stepSize, 0.75f * (result.maximum[axis] - result.minimum[axis]) /
                                                     float(volume.dimensions[axis]));
  if (sparse) {
    if (!std::isfinite(volume.sparseImportScale) || volume.sparseImportScale <= 0.0f ||
        !std::isfinite(volume.sparseStepSize) || volume.sparseStepSize <= 0.0f)
      throw std::invalid_argument("Invalid OpenVDB sparse scale or step");
    result.importScale = volume.sparseImportScale;
    result.stepSize = volume.sparseStepSize;
  }
  const auto device = device_->device();
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), VK_FORMAT_R32_SFLOAT, &properties);
  constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((properties.optimalTilingFeatures & required) != required)
    throw std::runtime_error("OpenVDB density preview requires filtered R32 3D textures");

  try {
    VkDeviceSize packedBytes = 0;
    size_t temperatureOffset = 0;
    if (sparse) {
      VkPhysicalDeviceProperties deviceProperties{};
      vkGetPhysicalDeviceProperties(device_->physicalDevice(), &deviceProperties);
      const auto maximumWords = deviceProperties.limits.maxStorageBufferRange / sizeof(uint32_t);
      if (volume.sparseDensity.size() > maximumWords)
        throw std::runtime_error("Packed NanoVDB density/temperature exceeds GPU maxStorageBufferRange");
      temperatureOffset = (volume.sparseDensity.size() + 7u) & ~size_t(7u);
      if (!volume.sparseTemperature.empty() &&
          (temperatureOffset > maximumWords || volume.sparseTemperature.size() > maximumWords - temperatureOffset))
        throw std::runtime_error("Packed NanoVDB density/temperature exceeds GPU maxStorageBufferRange");
      const size_t packedWords = volume.sparseTemperature.empty() ? volume.sparseDensity.size()
                                                                 : temperatureOffset + volume.sparseTemperature.size();
      packedBytes = VkDeviceSize(packedWords) * sizeof(uint32_t);
      if (!volume.sparseTemperature.empty())
        result.temperatureByteOffset = static_cast<uint32_t>(temperatureOffset * sizeof(uint32_t));
      result.sparseDensity = allocator_.createBuffer(
          packedBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_3D;
    info.format = VK_FORMAT_R32_SFLOAT;
    info.extent = sparse ? VkExtent3D{1, 1, 1}
                         : VkExtent3D{volume.dimensions[0], volume.dimensions[1], volume.dimensions[2]};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    const auto createTexture = [&](VkImage& image, VmaAllocation& memory, VkImageView& imageView) {
      if (vmaCreateImage(allocator_.memoryManager()->allocator(), &info, &allocation,
                         &image, &memory, nullptr) != VK_SUCCESS)
        throw std::runtime_error("Failed to create OpenVDB scalar texture");
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = image;
      view.viewType = VK_IMAGE_VIEW_TYPE_3D;
      view.format = info.format;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (createVulkanImageView(device, &view, nullptr, &imageView) != VK_SUCCESS)
        throw std::runtime_error("Failed to create OpenVDB scalar view");
    };
    createTexture(result.densityImage, result.densityAllocation, result.densityView);
    const bool hasTemperature = !sparse && !volume.temperature.empty();
    if (hasTemperature)
      createTexture(result.temperatureImage, result.temperatureAllocation, result.temperatureView);
    const VkDeviceSize byteCount = sparse ? sizeof(float) : count * sizeof(float);
    const VkDeviceSize textureBytes = byteCount * (hasTemperature ? 2 : 1);
    pending->staging = std::make_unique<container::gpu::StagingBuffer>(
        *allocator_.memoryManager(), textureBytes + packedBytes);
    auto& staging = *pending->staging;
    auto* mapped = static_cast<std::byte*>(staging.data());
    if (sparse) {
      // The one-texel density descriptor remains valid even in native mode.
      std::memset(mapped, 0, static_cast<size_t>(textureBytes));
      const auto densityBytes = volume.sparseDensity.size() * sizeof(uint32_t);
      std::memcpy(mapped + textureBytes, volume.sparseDensity.data(), densityBytes);
      if (!volume.sparseTemperature.empty()) {
        const auto temperatureBytes = temperatureOffset * sizeof(uint32_t);
        std::memset(mapped + textureBytes + densityBytes, 0, temperatureBytes - densityBytes);
        std::memcpy(mapped + textureBytes + temperatureBytes,
                    volume.sparseTemperature.data(), volume.sparseTemperature.size() * sizeof(uint32_t));
      }
    } else {
      std::memcpy(mapped, volume.density.data(), static_cast<size_t>(byteCount));
      if (hasTemperature)
        std::memcpy(mapped + byteCount, volume.temperature.data(), static_cast<size_t>(byteCount));
    }
    if (vmaFlushAllocation(allocator_.memoryManager()->allocator(), staging.buffer().allocation,
                           0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Failed to flush OpenVDB staged upload");
    if (!commandPool_) {
      VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      pool.queueFamilyIndex = device_->queueFamilyIndices().graphicsFamily.value();
      if (createOwnedCommandPool(device, &pool, nullptr, &commandPool_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create OpenVDB upload command pool");
    }
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &commandInfo, &pending->commandBuffer) != VK_SUCCESS)
      throw std::runtime_error("Failed to allocate OpenVDB upload command buffer");
    const auto cmd = pending->commandBuffer;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
      throw std::runtime_error("Failed to begin OpenVDB texture upload");
    const auto upload = [&](VkImage image, VkDeviceSize offset) {
      imageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
      VkBufferImageCopy copy{};
      copy.bufferOffset = offset;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = info.extent;
      vkCmdCopyBufferToImage(cmd, staging.buffer().buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    upload(result.densityImage, 0);
    if (hasTemperature) upload(result.temperatureImage, byteCount);
    if (sparse) {
      VkBufferCopy copy{textureBytes, 0, packedBytes};
      vkCmdCopyBuffer(cmd, staging.buffer().buffer, result.sparseDensity.buffer, 1, &copy);
      VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = result.sparseDensity.buffer;
      barrier.size = VK_WHOLE_SIZE;
      VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dependency.bufferMemoryBarrierCount = 1;
      dependency.pBufferMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd, &dependency);
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
      throw std::runtime_error("Failed to finish OpenVDB texture upload");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (createOwnedFence(device, &fenceInfo, nullptr, &pending->fence) != VK_SUCCESS)
      throw std::runtime_error("Failed to create OpenVDB upload fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (vkQueueSubmit(device_->graphicsQueue(), 1, &submit, pending->fence) != VK_SUCCESS)
      throw std::runtime_error("Failed to submit OpenVDB texture upload");
    pending->submitted = true;
  } catch (...) {
    destroyUpload(*pending, false);
    throw;
  }
  return pending;
}

OpenVdbVolumeRenderer::MediumResources OpenVdbVolumeRenderer::uploadVolume(
    const container::geometry::OpenVdbVolume& volume) {
  auto upload = submitVolumeUpload(volume);
  if (vkWaitForFences(device_->device(), 1, &upload->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
    destroyUpload(*upload, true);
    throw std::runtime_error("Failed to wait for OpenVDB upload");
  }
  auto medium = std::exchange(upload->medium, MediumResources{});
  destroyUpload(*upload, false);
  return medium;
}

bool OpenVdbVolumeRenderer::beginReplaceVolume(
    uint32_t index, const container::geometry::OpenVdbVolume& volume) {
  if (index >= volumes_.size()) throw std::out_of_range("OpenVDB volume index");
  if (pendingUpload_) return false;
  auto upload = submitVolumeUpload(volume);
  upload->index = index;
  pendingUpload_ = std::move(upload);
  return true;
}

bool OpenVdbVolumeRenderer::volumeReplacementPending() const noexcept {
  return bool(pendingUpload_);
}

bool OpenVdbVolumeRenderer::volumeReplacementReady() const {
  if (!pendingUpload_) return false;
  const auto result = vkGetFenceStatus(device_->device(), pendingUpload_->fence);
  if (result == VK_NOT_READY) return false;
  if (result != VK_SUCCESS) throw std::runtime_error("Failed to query OpenVDB upload fence");
  return true;
}

bool OpenVdbVolumeRenderer::publishVolumeReplacement() {
  if (!volumeReplacementReady()) return false;
  // The caller has retired graphics submissions that can reference any of the
  // descriptor sets. Upload completion alone does not retire later readers.
  std::swap(volumes_[pendingUpload_->index], pendingUpload_->medium);
  updateBounds();
  updateTextureDescriptors();
  destroyUpload(*pendingUpload_, false);
  pendingUpload_.reset();
  return true;
}

bool OpenVdbVolumeRenderer::discardVolumeReplacement() {
  if (!volumeReplacementReady()) return false;
  destroyUpload(*pendingUpload_, false);
  pendingUpload_.reset();
  return true;
}

void OpenVdbVolumeRenderer::updateBounds() {
  minimum_.fill(std::numeric_limits<float>::max());
  maximum_.fill(std::numeric_limits<float>::lowest());
  for (const auto& volume : volumes_)
    for (int axis = 0; axis < 3; ++axis) {
      minimum_[axis] = std::min(minimum_[axis], volume.minimum[axis]);
      maximum_[axis] = std::max(maximum_[axis], volume.maximum[axis]);
    }
}

void OpenVdbVolumeRenderer::replaceVolumes(
    std::span<const container::geometry::OpenVdbVolume> volumes) {
  if (pendingUpload_) throw std::logic_error("OpenVDB staged replacement is still pending");
  if (volumes.empty() || volumes.size() > kMaxOpenVdbVolumes)
    throw std::invalid_argument("OpenVDB renderer supports one to four volumes");
  if (vkQueueWaitIdle(device_->graphicsQueue()) != VK_SUCCESS)
    throw std::runtime_error("Failed to retire OpenVDB volume readers");
  std::vector<MediumResources> replacement;
  replacement.reserve(volumes.size());
  try {
    for (const auto& volume : volumes) replacement.push_back(uploadVolume(volume));
  } catch (...) {
    for (auto& medium : replacement) destroyMedium(medium);
    throw;
  }
  if (replacement.size() != volumes_.size()) volumeSettings_.fill(std::nullopt);
  volumes_.swap(replacement);
  updateBounds();
  updateTextureDescriptors();
  for (auto& medium : replacement) destroyMedium(medium);
}

void OpenVdbVolumeRenderer::replaceVolume(
    uint32_t index, const container::geometry::OpenVdbVolume& volume) {
  if (pendingUpload_) throw std::logic_error("OpenVDB staged replacement is still pending");
  if (index >= volumes_.size()) throw std::out_of_range("OpenVDB volume index");
  if (vkQueueWaitIdle(device_->graphicsQueue()) != VK_SUCCESS)
    throw std::runtime_error("Failed to retire OpenVDB volume readers");
  auto replacement = uploadVolume(volume);
  std::swap(replacement, volumes_[index]);
  updateBounds();
  updateTextureDescriptors();
  destroyMedium(replacement);
}

void OpenVdbVolumeRenderer::setVolumeSettings(uint32_t index, const OpenVdbVolumeSettings& settings) {
  if (index >= volumes_.size()) throw std::out_of_range("OpenVDB volume index");
  volumeSettings_[index] = sanitizeOpenVdbVolumeSettings(settings);
}
void OpenVdbVolumeRenderer::clearVolumeSettings(uint32_t index) {
  if (index >= volumes_.size()) throw std::out_of_range("OpenVDB volume index");
  volumeSettings_[index].reset();
}

void OpenVdbVolumeRenderer::initialize(
    const std::filesystem::path& shaderRoot,
    const container::geometry::OpenVdbVolume& volume, float densityScale) {
  if (!std::isfinite(densityScale) || densityScale < 0.0f || densityScale > 10000.0f)
    throw std::invalid_argument("OpenVDB density scale must be in [0,10000]");
  OpenVdbVolumeSettings settings;
  settings.densityScale = densityScale;
  initialize(shaderRoot, std::span(&volume, 1), settings);
}

void OpenVdbVolumeRenderer::initialize(
    const std::filesystem::path& shaderRoot,
    std::span<const container::geometry::OpenVdbVolume> volumes, const OpenVdbVolumeSettings& settings) {
  if (pipeline_ || !volumes_.empty()) throw std::logic_error("OpenVDB volume already initialized");
  setSettings(settings);
  replaceVolumes(volumes);
  const std::array<uint32_t, 8> zeros{};
  dummySparseBuffer_ = allocator_.uploadBuffer(std::as_bytes(std::span(zeros)),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  const auto device = device_->device();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device_->physicalDevice(), &properties);
  if (properties.limits.maxPerStageDescriptorStorageBuffers < 8 ||
      properties.limits.maxDescriptorSetStorageBuffers < 8 ||
      properties.limits.maxPerStageDescriptorStorageImages < 2 ||
      properties.limits.maxDescriptorSetStorageImages < 2 ||
      properties.limits.maxPerStageDescriptorUniformBuffers < 6 ||
      properties.limits.maxDescriptorSetUniformBuffers < 6 ||
      properties.limits.maxPerStageDescriptorSampledImages < 11 ||
      properties.limits.maxDescriptorSetSampledImages < 11)
    throw std::runtime_error("OpenVDB volume/transparency rendering requires 8 storage buffers, 2 storage images, 6 uniform buffers and 11 sampled images per compute stage");
  initializeTransparencyFallback();
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  if (createOwnedSampler(device, &sampler, nullptr, &sampler_) != VK_SUCCESS)
    throw std::runtime_error("Failed to create OpenVDB volume sampler");
  const std::vector<VkDescriptorSetLayoutBinding> bindings{
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxOpenVdbVolumes, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxOpenVdbVolumes, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {12, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxOpenVdbVolumes, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  setLayout_ = pipelines_.createDescriptorSetLayout(bindings, std::vector<VkDescriptorBindingFlags>(bindings.size(), 0));
  layout_ = pipelines_.createPipelineLayout({setLayout_}, {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VolumeConstants)}});
  pipeline_ = pipelines_.createComputePipeline(
      shaderRoot / "spv_shaders" / "openvdb_volume.comp.spv", layout_, "openvdb.volume");
}

void OpenVdbVolumeRenderer::initializeTransparencyFallback() {
  const auto device = device_->device();
  VkFormatProperties format{};
  vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), VK_FORMAT_R32_UINT, &format);
  if ((format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
    throw std::runtime_error("OpenVDB transparency requires an R32_UINT storage image");
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R32_UINT;
  info.extent = {1, 1, 1};
  info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo allocation{};
  allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateImage(allocator_.memoryManager()->allocator(), &info, &allocation,
                     &dummyOitHeadImage_, &dummyOitHeadAllocation_, nullptr) != VK_SUCCESS)
    throw std::runtime_error("Failed to create OpenVDB fallback OIT image");
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = dummyOitHeadImage_;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = info.format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (createVulkanImageView(device, &view, nullptr, &dummyOitHeadView_) != VK_SUCCESS)
    throw std::runtime_error("Failed to create OpenVDB fallback OIT view");
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool submitted = false;
  try {
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool = commandPool_;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &command, &cmd) != VK_SUCCESS)
      throw std::runtime_error("Failed to allocate OpenVDB fallback initialization commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
      throw std::runtime_error("Failed to begin OpenVDB fallback initialization");
    imageBarrier(cmd, dummyOitHeadImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
      throw std::runtime_error("Failed to finish OpenVDB fallback initialization");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (createOwnedFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
      throw std::runtime_error("Failed to create OpenVDB fallback initialization fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (vkQueueSubmit(device_->graphicsQueue(), 1, &submit, fence) != VK_SUCCESS)
      throw std::runtime_error("Failed to submit OpenVDB fallback initialization");
    submitted = true;
    if (vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
      throw std::runtime_error("Failed to wait for OpenVDB fallback initialization");
  } catch (...) {
    if (submitted) vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (fence) destroyOwnedFence(device, fence, nullptr);
    if (cmd) vkFreeCommandBuffers(device, commandPool_, 1, &cmd);
    throw;
  }
  destroyOwnedFence(device, fence, nullptr);
  vkFreeCommandBuffers(device, commandPool_, 1, &cmd);
}

void OpenVdbVolumeRenderer::updateTextureDescriptors() {
  if (sets_.empty() || volumes_.empty()) return;
  std::array<VkDescriptorImageInfo, kMaxOpenVdbVolumes> density{}, temperature{};
  std::array<VkDescriptorBufferInfo, kMaxOpenVdbVolumes> sparseDensity{};
  for (uint32_t index = 0; index < kMaxOpenVdbVolumes; ++index) {
    const auto& medium = volumes_[index < volumes_.size() ? index : 0];
    density[index] = {VK_NULL_HANDLE, medium.densityView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    temperature[index] = {VK_NULL_HANDLE, medium.temperatureView ? medium.temperatureView : medium.densityView,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    sparseDensity[index] = {medium.sparseDensity.buffer ? medium.sparseDensity.buffer : dummySparseBuffer_.buffer,
                            0, VK_WHOLE_SIZE};
  }
  for (const auto set : sets_) {
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t index = 0; index < 3; ++index) {
      writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[index].dstSet = set;
      writes[index].dstBinding = index == 0 ? 3 : index == 1 ? 8 : 14;
      writes[index].descriptorCount = kMaxOpenVdbVolumes;
      writes[index].descriptorType = index < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      if (index < 2) writes[index].pImageInfo = index == 0 ? density.data() : temperature.data();
      else writes[index].pBufferInfo = sparseDensity.data();
    }
    vkUpdateDescriptorSets(device_->device(), 3, writes, 0, nullptr);
  }
}

void OpenVdbVolumeRenderer::prepare(
    const FrameResourceManager& frames,
    std::span<const container::gpu::AllocatedBuffer> cameras,
    std::span<const container::gpu::AllocatedBuffer> lighting,
    std::span<const container::gpu::AllocatedBuffer> sceneClips,
    std::span<const container::gpu::AllocatedBuffer> pointLights,
    std::span<const container::gpu::AllocatedBuffer> areaLights,
    const ShadowManager& shadows) {
  if(descriptorBindingEpoch_!=UINT64_MAX) ++descriptorBindingEpoch_;
  const auto device = device_->device();
  if (descriptorPool_) destroyOwnedDescriptorPool(device, descriptorPool_, nullptr);
  descriptorPool_ = VK_NULL_HANDLE;
  sets_.clear();
  for (auto& buffer : mediumBuffers_) allocator_.destroyBuffer(buffer);
  mediumBuffers_.clear();
  const auto count = static_cast<uint32_t>(std::min(cameras.size(), lighting.size()));
  if (!pipeline_ || !count) return;
  if (sceneClips.size() != count ||
      std::ranges::any_of(sceneClips, [](const auto& buffer) {
        return !buffer.buffer || !buffer.allocation ||
               buffer.allocation_info.size < sizeof(container::gpu::SceneClipState);
      }) || pointLights.size() != count || areaLights.size() != count ||
      shadows.shadowUbos().size() < count ||
      shadows.localShadowUbos().size() < count || !shadows.shadowAtlasArrayView() || !shadows.localShadowAtlasArrayView())
    throw std::runtime_error("Missing OpenVDB clipping, light or shadow resources");
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6 * count},
      {VK_DESCRIPTOR_TYPE_SAMPLER, count},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (2 * kMaxOpenVdbVolumes + 3) * count},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * count},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (4 + kMaxOpenVdbVolumes) * count}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = count;
  pool.poolSizeCount = 5;
  pool.pPoolSizes = sizes;
  if (createOwnedDescriptorPool(device, &pool, nullptr, &descriptorPool_) != VK_SUCCESS)
    throw std::runtime_error("Failed to create OpenVDB descriptor pool");
  sets_.resize(count);
  const std::vector<VkDescriptorSetLayout> layouts(count, setLayout_);
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = descriptorPool_;
  allocate.descriptorSetCount = count;
  allocate.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(device, &allocate, sets_.data()) != VK_SUCCESS)
    throw std::runtime_error("Failed to allocate OpenVDB descriptors");
  mediumBuffers_.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    const auto* frame = frames.frame(index);
    if (!frame || !pointLights[index].buffer || !areaLights[index].buffer)
      throw std::runtime_error("Missing OpenVDB frame resources");
    mediumBuffers_.push_back(allocator_.createBuffer(
        sizeof(OpenVdbMediumGpuData) * kMaxOpenVdbVolumes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT));
    const VkDescriptorBufferInfo buffers[] = {
        {cameras[index].buffer, 0, sizeof(container::gpu::CameraData)},
        {lighting[index].buffer, 0, sizeof(container::gpu::LightingData)},
        {sceneClips[index].buffer, 0, sizeof(container::gpu::SceneClipState)},
        {pointLights[index].buffer, 0, sizeof(container::gpu::PointLightData) * container::gpu::kMaxClusteredLights},
        {mediumBuffers_.back().buffer, 0, sizeof(OpenVdbMediumGpuData) * kMaxOpenVdbVolumes},
        {shadows.shadowUbo(index).buffer, 0, sizeof(container::gpu::ShadowData)},
        {shadows.localShadowUbo(index).buffer, 0, sizeof(container::gpu::LocalShadowData)},
        {areaLights[index].buffer, 0, sizeof(container::gpu::AreaLightData) * container::gpu::kMaxAreaLights},
        {frame->oitNodeBuffer.buffer ? frame->oitNodeBuffer.buffer : dummySparseBuffer_.buffer, 0, VK_WHOLE_SIZE},
        {frame->oitMetadataBuffer.buffer ? frame->oitMetadataBuffer.buffer : dummySparseBuffer_.buffer, 0, sizeof(OitMetadata)}};
    const VkDescriptorImageInfo images[] = {
        {sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
        {VK_NULL_HANDLE, frame->depthSamplingView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, frame->sceneColor.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, shadows.shadowAtlasArrayView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, shadows.localShadowAtlasArrayView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, frame->oitHeadPointers.view ? frame->oitHeadPointers.view : dummyOitHeadView_, VK_IMAGE_LAYOUT_GENERAL}};
    const uint32_t bindings[] = {0, 1, 2, 4, 5, 6, 7, 9, 10, 11, 12, 13, 15, 16, 17, 18};
    VkWriteDescriptorSet writes[16]{};
    for (uint32_t i = 0; i < 16; ++i) {
      auto& write = writes[i];
      const auto binding = bindings[i];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = sets_[index];
      write.dstBinding = binding;
      write.descriptorCount = 1;
      write.descriptorType = binding == 0 || binding == 1 || binding == 6 || binding == 10 || binding == 12 || binding == 18
                                 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                             : binding == 2 ? VK_DESCRIPTOR_TYPE_SAMPLER
                             : binding == 5 || binding == 16 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : binding == 7 || binding == 9 || binding == 15 || binding == 17 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                          : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      switch (binding) {
      case 0: write.pBufferInfo = &buffers[0]; break;
      case 1: write.pBufferInfo = &buffers[1]; break;
      case 2: write.pImageInfo = &images[0]; break;
      case 4: write.pImageInfo = &images[1]; break;
      case 5: write.pImageInfo = &images[2]; break;
      case 6: write.pBufferInfo = &buffers[2]; break;
      case 7: write.pBufferInfo = &buffers[3]; break;
      case 9: write.pBufferInfo = &buffers[4]; break;
      case 10: write.pBufferInfo = &buffers[5]; break;
      case 11: write.pImageInfo = &images[3]; break;
      case 12: write.pBufferInfo = &buffers[6]; break;
      case 13: write.pImageInfo = &images[4]; break;
      case 15: write.pBufferInfo = &buffers[7]; break;
      case 16: write.pImageInfo = &images[5]; break;
      case 17: write.pBufferInfo = &buffers[8]; break;
      case 18: write.pBufferInfo = &buffers[9]; break;
      }
    }
    vkUpdateDescriptorSets(device, 16, writes, 0, nullptr);
  }
  updateTextureDescriptors();
}

bool OpenVdbVolumeRenderer::records(const FrameRecordParams& frame) const noexcept {
  if (!settings_.enabled || !pipeline_ || volumes_.empty() ||
      frame.runtime.imageIndex >= sets_.size() || frame.runtime.imageIndex >= mediumBuffers_.size() ||
      (frame.debug.displayMode != 0 && frame.debug.displayMode != 8 &&
       (frame.debug.displayMode < 100 || frame.debug.displayMode > 104))) return false;
  bool enabled = false;
  for (uint32_t index = 0; index < volumes_.size(); ++index)
    enabled |= volumeSettings_[index] ? volumeSettings_[index]->enabled : settings_.enabled;
  if (!enabled) return false;
  const auto* color = frame.imageBinding(frame.runtime.activeTechnique, "scene-color");
  return color && color->image && color->extent.width && color->extent.height;
}

void OpenVdbVolumeRenderer::record(VkCommandBuffer cmd, const FrameRecordParams& frame,
                                  bool transparentOitActive) const {
  if (!records(frame)) return;
  // OIT pool growth can replace storage. Refresh the retired current frame's
  // descriptors from live bindings rather than borrowing initial pool handles.
  const auto* oitHeads = frame.imageBinding(frame.runtime.activeTechnique, "oit-head-pointers");
  const auto* oitNodes = frame.bufferBinding(frame.runtime.activeTechnique, "oit-node-buffer");
  const auto* oitMetadata = frame.bufferBinding(frame.runtime.activeTechnique, "oit-metadata-buffer");
  const bool oitAvailable = oitHeads && oitHeads->image && oitHeads->view &&
                            oitNodes && oitNodes->buffer && oitNodes->size >= sizeof(OitNode) &&
                            oitMetadata && oitMetadata->buffer && oitMetadata->size >= sizeof(OitMetadata);
  // Overview also reads the original lists for its diagnostic panels. Bind
  // valid dormant resources whenever the producer/consumer is inactive, since
  // validation still checks statically-used image descriptor layouts.
  const bool interleaveTransparency = transparentOitActive && oitAvailable && frame.debug.displayMode != 8;
  const VkDescriptorImageInfo headDescriptor{VK_NULL_HANDLE,
      interleaveTransparency ? oitHeads->view : dummyOitHeadView_, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo nodeDescriptor{
      interleaveTransparency ? oitNodes->buffer : dummySparseBuffer_.buffer, 0, VK_WHOLE_SIZE};
  const VkDescriptorBufferInfo metadataDescriptor{
      interleaveTransparency ? oitMetadata->buffer : dummySparseBuffer_.buffer, 0, sizeof(OitMetadata)};
  VkWriteDescriptorSet oitWrites[3]{};
  for (uint32_t index = 0; index < 3; ++index) {
    auto& write = oitWrites[index];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[frame.runtime.imageIndex];
    write.dstBinding = 16 + index;
    write.descriptorCount = 1;
    write.descriptorType = index == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                           index == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (index == 0) write.pImageInfo = &headDescriptor;
    else write.pBufferInfo = index == 1 ? &nodeDescriptor : &metadataDescriptor;
  }
  vkUpdateDescriptorSets(device_->device(), 3, oitWrites, 0, nullptr);
  std::array<OpenVdbMediumGpuData, kMaxOpenVdbVolumes> media{};
  float stepSize = std::numeric_limits<float>::max();
  std::array<float, 3> activeMinimum{}, activeMaximum{};
  activeMinimum.fill(std::numeric_limits<float>::max());
  activeMaximum.fill(std::numeric_limits<float>::lowest());
  for (uint32_t index = 0; index < volumes_.size(); ++index) {
    const auto& medium = volumes_[index];
    const auto& settings = volumeSettings_[index] ? *volumeSettings_[index] : settings_;
    const auto& minimum = medium.minimum;
    const auto& maximum = medium.maximum;
    const auto step = medium.stepSize * settings.stepScale;
    if (!std::isfinite(step) || step < std::numeric_limits<float>::min())
      throw std::runtime_error("OpenVDB world-space sampling step exceeds GPU float precision");
    media[index] = {
        {minimum[0], minimum[1], minimum[2], settings.densityScale},
        {maximum[0], maximum[1], maximum[2], step},
        {settings.scatteringColor[0], settings.scatteringColor[1], settings.scatteringColor[2], settings.anisotropy},
        {settings.emissionColor[0], settings.emissionColor[1], settings.emissionColor[2], settings.emissionScale},
        {settings.temperatureScale, medium.importScale, settings.multipleScatteringStrength, 0.0f},
        {(settings.enabled ? 1u : 0u) | (medium.temperatureView || medium.temperatureByteOffset ? 2u : 0u) |
             (medium.sparseDensity.buffer ? 4u : 0u) |
             (settings.receiveShadows ? 16u : 0u), settings.multipleScatteringOrders, medium.temperatureByteOffset, 0u}};
    if (settings.enabled) {
      stepSize = std::min(stepSize, step);
      for (uint32_t axis = 0; axis < 3; ++axis) {
        activeMinimum[axis] = std::min(activeMinimum[axis], minimum[axis]);
        activeMaximum[axis] = std::max(activeMaximum[axis], maximum[axis]);
      }
    }
  }
  const auto& buffer = mediumBuffers_[frame.runtime.imageIndex];
  void* mapped = buffer.allocation_info.pMappedData;
  bool mapHere = mapped == nullptr;
  if (mapHere && vmaMapMemory(allocator_.memoryManager()->allocator(), buffer.allocation, &mapped) != VK_SUCCESS)
    throw std::runtime_error("Failed to map OpenVDB medium constants");
  std::memcpy(mapped, media.data(), sizeof(media));
  const auto flush = vmaFlushAllocation(allocator_.memoryManager()->allocator(), buffer.allocation, 0, sizeof(media));
  if (mapHere) vmaUnmapMemory(allocator_.memoryManager()->allocator(), buffer.allocation);
  if (flush != VK_SUCCESS) throw std::runtime_error("Failed to flush OpenVDB medium constants");

  const auto* color = frame.imageBinding(frame.runtime.activeTechnique, "scene-color");
  imageBarrier(cmd, color->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  VkMemoryBarrier2 depthDependency{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  depthDependency.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  depthDependency.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  depthDependency.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  depthDependency.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &depthDependency;
  vkCmdPipelineBarrier2(cmd, &dependency);
  const auto* clip = frame.pushConstants.bindless;
  const uint32_t flags = volumeCount() | (clip && clip->sectionPlaneEnabled ? 256u : 0u) |
                         (interleaveTransparency ? 512u : 0u);
  const VolumeConstants constants{
      {activeMinimum[0], activeMinimum[1], activeMinimum[2], stepSize},
      {activeMaximum[0], activeMaximum[1], activeMaximum[2], 0.0f},
      {color->extent.width, color->extent.height, frame.camera.orthographic ? 1u : 0u, flags},
      clip ? clip->sectionPlane : glm::vec4(0.0f)};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                          &sets_[frame.runtime.imageIndex], 0, nullptr);
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  vkCmdDispatch(cmd, (color->extent.width + 7) / 8, (color->extent.height + 7) / 8, 1);
  if (interleaveTransparency) {
    VkMemoryBarrier2 consumed{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    consumed.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    consumed.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    consumed.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    consumed.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo consumedDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    consumedDependency.memoryBarrierCount = 1;
    consumedDependency.pMemoryBarriers = &consumed;
    vkCmdPipelineBarrier2(cmd, &consumedDependency);
  }
  imageBarrier(cmd, color->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
} // namespace container::renderer
