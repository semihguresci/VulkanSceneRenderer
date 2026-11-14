#include "Container/utility/AllocationManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "Container/utility/Platform.h"

namespace container::gpu {

namespace {

void EnsureArenaCapacity(std::unique_ptr<BufferArena>& arena,
                         VulkanMemoryManager& memoryManager,
                         VkDeviceSize requiredSize,
                         VkBufferUsageFlags usage,
                         VmaMemoryUsage memoryUsage,
                         VmaAllocationCreateFlags allocationFlags) {
  const VkDeviceSize safeRequiredSize = std::max<VkDeviceSize>(1, requiredSize);

  if (!arena || arena->remainingSize() < safeRequiredSize) {
    VkDeviceSize requestedSize = safeRequiredSize;
    if (arena) {
      requestedSize = std::max(safeRequiredSize, arena->totalSize() * 2);
    }
    arena = std::make_unique<BufferArena>(memoryManager, requestedSize, usage,
                                          memoryUsage, allocationFlags);
  }
}

bool textureFormatSupportsLinearBlit(VkPhysicalDevice physicalDevice,
                                     VkFormat format) {
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
  constexpr VkFormatFeatureFlags requiredFeatures =
      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  return (properties.optimalTilingFeatures & requiredFeatures) ==
         requiredFeatures;
}

struct TextureRetirementGuard {
  bool& flag;
  explicit TextureRetirementGuard(bool& value) noexcept : flag(value) { flag = true; }
  ~TextureRetirementGuard() { flag = false; }
};

struct ScopedUploadSubmission {
  VkDevice device{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  bool submitted{}, retired{};

  ScopedUploadSubmission(VkDevice ownerDevice, VkCommandPool ownerPool,
                         VkCommandBuffer ownedCommand) noexcept
      : device(ownerDevice), pool(ownerPool), command(ownedCommand) {}
  ScopedUploadSubmission(const ScopedUploadSubmission&) = delete;
  ScopedUploadSubmission& operator=(const ScopedUploadSubmission&) = delete;

  void release() noexcept {
    if (submitted && !retired) {
      // An exception from the event pump must not release a pending command,
      // image or staging allocation. Do not reenter callbacks while retiring.
      for (;;) {
        const auto wait = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (wait == VK_SUCCESS || wait == VK_ERROR_DEVICE_LOST) break;
        if (wait == VK_TIMEOUT) continue;
        const auto idle = vkDeviceWaitIdle(device);
        if (idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST) break;
        // Retry failed waits: freeing referenced allocations is not safe until
        // completion is established or the device reports loss.
      }
      retired = true;
    }
    if (fence) { destroyOwnedFence(device, fence, nullptr); fence = VK_NULL_HANDLE; }
    if (command) { vkFreeCommandBuffers(device, pool, 1, &command); command = VK_NULL_HANDLE; }
  }
  ~ScopedUploadSubmission() { release(); }
};

}  // namespace

struct AllocationManager::TextureBatchControl {
  std::mutex mutex;
  AllocationManager* owner{};
};

AllocationManager::UploadOperation::UploadOperation(AllocationManager& owner)
    : owner_(owner) {
  if (owner_.textureRetirementInProgress_)
    throw std::logic_error("cannot upload during texture retirement");
  if (owner_.activeUploadOperations_ == std::numeric_limits<uint32_t>::max())
    throw std::overflow_error("allocation upload nesting exceeds its limit");
  ++owner_.activeUploadOperations_;
}

AllocationManager::UploadOperation::~UploadOperation() {
  --owner_.activeUploadOperations_;
}

AllocationManager::TextureAllocationLease::TextureAllocationLease(
    std::weak_ptr<TextureBatchControl> control, uint64_t group) noexcept
    : control_(std::move(control)), group_(group) {}

AllocationManager::TextureAllocationLease::~TextureAllocationLease() {
  if (const auto control = control_.lock()) {
    std::lock_guard lock(control->mutex);
    if (control->owner)
      control->owner->retireTextureAllocationGroup(group_, committed_);
  }
}

AllocationManager::TextureImageAllocationLease::TextureImageAllocationLease(
    std::weak_ptr<TextureBatchControl> control,TextureAllocationIdentity identity) noexcept
    :control_(std::move(control)),identity_(std::move(identity)) {}

AllocationManager::TextureImageAllocationLease::~TextureImageAllocationLease() {
  if(!armed_) return;
  if(const auto control=control_.lock()) {
    std::lock_guard lock(control->mutex);
    if(control->owner) control->owner->retireTextureImage(identity_.incarnation());
  }
}

bool AllocationManager::TextureAllocationBatch::active() const noexcept {
  if (!lease_) return false;
  if (const auto control = lease_->control_.lock()) {
    std::lock_guard lock(control->mutex);
    return control->owner &&
           control->owner->activeTextureBatchGroup_ == lease_->group_;
  }
  return false;
}

uint64_t AllocationManager::TextureAllocationBatch::groupId() const noexcept {
  return lease_ ? lease_->group_ : 0;
}

std::shared_ptr<const AllocationManager::TextureAllocationLease>
AllocationManager::TextureAllocationBatch::commit() {
  if (!lease_) throw std::logic_error("texture allocation batch is empty");
  const auto control = lease_->control_.lock();
  if (!control) throw std::logic_error("texture allocation batch owner expired");
  std::lock_guard lock(control->mutex);
  if (!control->owner ||
      control->owner->activeTextureBatchGroup_ != lease_->group_)
    throw std::logic_error("texture allocation batch is no longer active");
  lease_->committed_ = true;
  control->owner->activeTextureBatchGroup_ = 0;
  return std::shared_ptr<const TextureAllocationLease>(std::move(lease_));
}

AllocationManager::TextureAllocationBatch
AllocationManager::beginTextureAllocationBatch() {
  if (!memoryManager_ || !device_)
    throw std::logic_error("texture allocation batch requires an initialized owner");
  if (activeTextureBatchGroup_)
    throw std::logic_error("a texture allocation batch is already active");
  if (textureRetirementInProgress_)
    throw std::logic_error("cannot begin a texture allocation batch during retirement");
  if (!nextTextureBatchGroup_)
    throw std::overflow_error("texture allocation batch IDs exhausted");
  if (!textureBatchControl_) {
    textureBatchControl_ = std::make_shared<TextureBatchControl>();
    textureBatchControl_->owner = this;
  }
  // Allocate both the object and its shared control before changing live state.
  auto lease = std::shared_ptr<TextureAllocationLease>(
      new TextureAllocationLease(textureBatchControl_, nextTextureBatchGroup_));
  std::lock_guard lock(textureBatchControl_->mutex);
  activeTextureBatchGroup_ = nextTextureBatchGroup_++;
  return TextureAllocationBatch(std::move(lease));
}

std::shared_ptr<const AllocationManager::TextureImageAllocationLease>
AllocationManager::retainTextureImage(const container::material::TextureResource& resource) {
  const auto owner=resource.allocationIdentity.owner_.lock();
  if(!owner || !textureBatchControl_ || owner.get()!=textureBatchControl_.get() || !resource.allocationIdentity.incarnation_)
    throw std::invalid_argument("texture image identity is absent or belongs to another allocator");
  const auto validate=[&]() -> TextureAllocation& {
    if(textureRetirementInProgress_ || activeTextureBatchGroup_ || activeUploadOperations_)
      throw std::logic_error("cannot retain a texture image during upload, batch preparation or retirement");
    if(textureBatchControl_->owner!=this) throw std::invalid_argument("texture image allocator has expired");
    const auto found=std::lower_bound(textureAllocations_.begin(),textureAllocations_.end(),resource.allocationIdentity.incarnation_,
        [](const auto& allocation,uint64_t id){return allocation.incarnation<id;});
    if(found==textureAllocations_.end() || found->incarnation!=resource.allocationIdentity.incarnation_ || found->image!=resource.image || found->imageView!=resource.imageView ||
        found->image==VK_NULL_HANDLE || found->imageView==VK_NULL_HANDLE || !found->allocation)
      throw std::invalid_argument("texture image identity is stale or its image/view pair differs");
    if(found->lifetime!=TextureAllocationLifetime::Scene)
      throw std::invalid_argument("persistent texture images cannot acquire scene image leases");
    if(found->imageLeaseCount==UINT32_MAX) throw std::overflow_error("texture image lease count is exhausted");
    return *found;
  };
  {std::lock_guard lock(textureBatchControl_->mutex);(void)validate();}
  // Neither object/control allocation failure nor revalidation failure can
  // release an image: the new lease is armed only after its pin is installed.
  auto lease=std::shared_ptr<TextureImageAllocationLease>(
      new TextureImageAllocationLease(textureBatchControl_,resource.allocationIdentity));
  {
    std::lock_guard lock(textureBatchControl_->mutex);auto& allocation=validate();
    ++allocation.imageLeaseCount;
    if(allocation.batchGroup) allocation.individuallyManaged=true;
    lease->armed_=true;
  }
  return lease;
}

bool AllocationManager::textureImageMatches(
    const container::material::TextureResource& resource) const noexcept {
  const auto owner=resource.allocationIdentity.owner_.lock();
  if(!owner || !textureBatchControl_ || owner.get()!=textureBatchControl_.get() ||
      !resource.allocationIdentity.incarnation_) return false;
  std::lock_guard lock(textureBatchControl_->mutex);
  if(textureBatchControl_->owner!=this || textureRetirementInProgress_ ||
      activeTextureBatchGroup_ || activeUploadOperations_) return false;
  const auto found=std::lower_bound(textureAllocations_.begin(),textureAllocations_.end(),
      resource.allocationIdentity.incarnation_,
      [](const auto& allocation,uint64_t id){return allocation.incarnation<id;});
  return found!=textureAllocations_.end() &&
      found->incarnation==resource.allocationIdentity.incarnation_ &&
      found->lifetime==TextureAllocationLifetime::Scene &&
      found->image!=VK_NULL_HANDLE && found->imageView!=VK_NULL_HANDLE && found->allocation &&
      found->image==resource.image && found->imageView==resource.imageView;
}

bool AllocationManager::textureImagesAdoptable(
    std::span<const std::shared_ptr<const TextureImageAllocationLease>> images) const noexcept {
  if(images.empty() || images.size()>textureAllocations_.size() || !textureBatchControl_ || textureBatchControl_->owner!=this || textureRetirementInProgress_ ||
      activeTextureBatchGroup_ || activeUploadOperations_) return false;
  uint64_t previous{};
  for(const auto& lease:images) {
    if(!lease || !lease->armed_) return false;
    const auto owner=lease->identity_.owner_.lock();const auto incarnation=lease->identity_.incarnation_;
    if(!owner || owner.get()!=textureBatchControl_.get() || incarnation<=previous) return false;
    const auto found=std::lower_bound(textureAllocations_.begin(),textureAllocations_.end(),incarnation,
        [](const auto& allocation,uint64_t id){return allocation.incarnation<id;});
    if(found==textureAllocations_.end() || found->incarnation!=incarnation || found->lifetime!=TextureAllocationLifetime::Scene ||
        found->batchGroup || found->individuallyManaged || !found->imageLeaseCount || !found->image || !found->imageView || !found->allocation)
      return false;
    previous=incarnation;
  }
  return true;
}

bool AllocationManager::canAdoptTextureImages(
    std::span<const std::shared_ptr<const TextureImageAllocationLease>> images) const noexcept {
  if(!textureBatchControl_) return false;
  std::lock_guard lock(textureBatchControl_->mutex);return textureImagesAdoptable(images);
}

bool AllocationManager::adoptTextureImages(
    std::span<const std::shared_ptr<const TextureImageAllocationLease>> images) noexcept {
  if(!textureBatchControl_) return false;
  std::lock_guard lock(textureBatchControl_->mutex);if(!textureImagesAdoptable(images)) return false;
  for(const auto& lease:images) {
    const auto found=std::lower_bound(textureAllocations_.begin(),textureAllocations_.end(),lease->identity_.incarnation_,
        [](const auto& allocation,uint64_t id){return allocation.incarnation<id;});
    found->individuallyManaged=true;
  }
  return true;
}

uint64_t AllocationManager::textureAllocationGroup(
    TextureAllocationLifetime lifetime) const {
  if (textureRetirementInProgress_)
    throw std::logic_error("cannot allocate textures during retirement");
  if (lifetime != TextureAllocationLifetime::Scene &&
      lifetime != TextureAllocationLifetime::Persistent)
    throw std::invalid_argument("invalid texture allocation lifetime");
  if (activeTextureBatchGroup_ && lifetime == TextureAllocationLifetime::Persistent)
    throw std::logic_error("persistent texture allocation is not allowed in a scene batch");
  return lifetime == TextureAllocationLifetime::Scene ? activeTextureBatchGroup_ : 0;
}

TextureAllocationIdentity AllocationManager::registerTextureAllocation(TextureAllocation allocation) {
  // Loading callbacks can pump events. A batch cancelled during an upload must
  // not acquire a late allocation after its only rollback owner has gone away.
  if (allocation.batchGroup != textureAllocationGroup(allocation.lifetime))
    throw std::logic_error("texture allocation batch changed during upload");
  if(!nextTextureAllocationIncarnation_) throw std::overflow_error("texture image allocation incarnations exhausted");
  if(!textureBatchControl_) {textureBatchControl_=std::make_shared<TextureBatchControl>();textureBatchControl_->owner=this;}
  TextureAllocationIdentity identity;identity.owner_=textureBatchControl_;identity.incarnation_=nextTextureAllocationIncarnation_++;
  allocation.incarnation=identity.incarnation_;allocation.aggregateRetained=allocation.batchGroup!=0;
  textureAllocations_.push_back(allocation);
  return identity;
}

void AllocationManager::destroyTextureAllocation(TextureAllocation& texture) noexcept {
  if (texture.imageView != VK_NULL_HANDLE) {
    destroyVulkanImageView(device_, texture.imageView, nullptr);
    texture.imageView = VK_NULL_HANDLE;
  }
  if (texture.image != VK_NULL_HANDLE && memoryManager_) {
    vmaDestroyImage(memoryManager_->allocator(), texture.image, texture.allocation);
    texture.image = VK_NULL_HANDLE;
    texture.allocation = nullptr;
  }
}

void AllocationManager::retireTextureAllocationGroup(uint64_t group,
                                                    bool committed) noexcept {
  if (activeTextureBatchGroup_ == group) activeTextureBatchGroup_ = 0;
  // Explicit resets retire every scene group themselves. A progress callback
  // releasing its last lease must not erase the vector during that iteration.
  if (textureRetirementInProgress_) return;
  const auto belongs = [group](const TextureAllocation& allocation) {
    return allocation.batchGroup == group;
  };
  if (!group || !std::ranges::any_of(textureAllocations_, belongs)) return;
  TextureRetirementGuard retiring(textureRetirementInProgress_);
  const bool reclaims=std::ranges::any_of(textureAllocations_,[&](const auto& allocation) {return belongs(allocation) && !allocation.imageLeaseCount;});
  if (reclaims && committed && device_) {
    const auto wait = vkDeviceWaitIdle(device_);
    // Keep ownership until an explicit synchronized reset if idle could not be
    // established. Device loss itself permits resource destruction.
    if (wait != VK_SUCCESS && wait != VK_ERROR_DEVICE_LOST) return;
  }
  for (auto& allocation : textureAllocations_) if(belongs(allocation)) {
    allocation.aggregateRetained=false;
    if(!allocation.imageLeaseCount) destroyTextureAllocation(allocation);
  }
  std::erase_if(textureAllocations_,[&](const auto& allocation) {return belongs(allocation) && !allocation.imageLeaseCount;});
}

void AllocationManager::retireTextureImage(uint64_t incarnation) noexcept {
  const auto found=std::lower_bound(textureAllocations_.begin(),textureAllocations_.end(),incarnation,
      [](const auto& allocation,uint64_t id){return allocation.incarnation<id;});
  if(found==textureAllocations_.end() || found->incarnation!=incarnation) return; // An explicit reset already retired it.
  if(found->imageLeaseCount) --found->imageLeaseCount;
  if(textureRetirementInProgress_ || found->aggregateRetained || !found->individuallyManaged || found->imageLeaseCount) return;
  TextureRetirementGuard retiring(textureRetirementInProgress_);
  if(device_) {
    const auto wait=vkDeviceWaitIdle(device_);
    if(wait!=VK_SUCCESS && wait!=VK_ERROR_DEVICE_LOST) return; // Keep ownership until a synchronized explicit reset.
  }
  destroyTextureAllocation(*found);textureAllocations_.erase(found);
}

void AllocationManager::invalidateTextureBatchControl() noexcept {
  if (textureBatchControl_) {
    {
      std::lock_guard lock(textureBatchControl_->mutex);
      textureBatchControl_->owner = nullptr;
    }
    textureBatchControl_.reset();
  }
}

AllocationManager::~AllocationManager() {
  invalidateTextureBatchControl();
  activeTextureBatchGroup_ = 0;
  loadProgress_ = {};
  if (device_ && !textureAllocations_.empty()) (void)vkDeviceWaitIdle(device_);
  cleanup();
}

void AllocationManager::initialize(VkInstance instance,
                                   VkPhysicalDevice physicalDevice,
                                   VkDevice device, VkQueue graphicsQueue,
                                   VkCommandPool commandPool,
                                   const container::app::AppConfig& config) {
  if (memoryManager_ || device_ || activeTextureBatchGroup_ || textureRetirementInProgress_ || activeUploadOperations_)
    throw std::logic_error("allocation manager must be cleaned up before initialization");
  instance_ = instance;
  physicalDevice_ = physicalDevice;
  device_ = device;
  graphicsQueue_ = graphicsQueue;
  commandPool_ = commandPool;
  config_ = config;
  enabledTextureFeatures_ = {};

  memoryManager_ = std::make_unique<VulkanMemoryManager>(
      instance_, physicalDevice_, device_);
}

void AllocationManager::cleanup() {
  if (activeUploadOperations_)
    throw std::logic_error("cannot clean up during an allocation upload");
  if (activeTextureBatchGroup_)
    throw std::logic_error("cannot clean up an active texture allocation batch");
  if (textureRetirementInProgress_)
    throw std::logic_error("cannot clean up during texture retirement");
  invalidateTextureBatchControl();
  loadProgress_ = {};
  resetTextureAllocations(TextureAllocationResetScope::All);
  indexArena_.reset();
  vertexArena_.reset();
  memoryManager_.reset();

  instance_ = VK_NULL_HANDLE;
  physicalDevice_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  graphicsQueue_ = VK_NULL_HANDLE;
  commandPool_ = VK_NULL_HANDLE;
  enabledTextureFeatures_ = {};
}

void AllocationManager::resetGeometryAllocations() {
  if (activeUploadOperations_)
    throw std::logic_error("cannot reset geometry during an allocation upload");
  if (vertexArena_)
    vertexArena_->reset();
  if (indexArena_)
    indexArena_->reset();
}

VkDeviceSize AllocationManager::geometryArenaCapacityBytes() const {
  return (vertexArena_ ? vertexArena_->totalSize() : 0u) +
         (indexArena_ ? indexArena_->totalSize() : 0u);
}

BufferSlice AllocationManager::uploadVertices(
    std::span<const container::geometry::Vertex> vertices) {
  if (vertices.empty()) return {};
  const UploadOperation operation(*this);
  VkDeviceSize bufferSize = sizeof(container::geometry::Vertex) * vertices.size();

  EnsureArenaCapacity(
      vertexArena_, *memoryManager_, bufferSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(vertices.data()),
                        static_cast<size_t>(bufferSize)});

  BufferSlice slice =
      vertexArena_->allocate(bufferSize, alignof(container::geometry::Vertex));

  copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0,
             slice.offset);

  return slice;
}

BufferSlice AllocationManager::uploadIndices(
    std::span<const uint32_t> indices) {
  if (indices.empty()) return {};
  const UploadOperation operation(*this);
  VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();

  EnsureArenaCapacity(
      indexArena_, *memoryManager_, bufferSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(indices.data()),
                        static_cast<size_t>(bufferSize)});

  VkDeviceSize alignment = std::max<VkDeviceSize>(sizeof(uint32_t), 4);
  BufferSlice slice = indexArena_->allocate(bufferSize, alignment);

  copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0,
             slice.offset);

  return slice;
}

AllocatedBuffer AllocationManager::uploadBuffer(
    std::span<const std::byte> bytes,
    VkBufferUsageFlags usage) {
  if (bytes.empty()) return {};
  const UploadOperation operation(*this);

  const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(bytes.size());
  AllocatedBuffer buffer = createBuffer(
      bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  try {
    StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
    stagingBuffer.upload(bytes);
    copyBuffer(stagingBuffer.buffer().buffer, buffer.buffer, bufferSize);
  } catch (...) {
    // copyBuffer retires submitted work before propagating a progress error.
    memoryManager_->destroyBuffer(buffer);
    throw;
  }

  return buffer;
}

AllocatedBuffer AllocationManager::createBuffer(
    VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage,
    VmaAllocationCreateFlags allocationFlags, VkSharingMode sharingMode,
    std::span<const uint32_t> queueFamilies) {
  return memoryManager_->createBuffer(size, usage, memoryUsage, allocationFlags,
                                      sharingMode, queueFamilies);
}

void AllocationManager::destroyBuffer(AllocatedBuffer& buffer) {
  if (memoryManager_) {
    memoryManager_->destroyBuffer(buffer);
  }
}

container::material::TextureResource AllocationManager::createTextureFromFile(
    const std::string& texturePath, VkFormat format) {
  const auto path = container::util::pathFromUtf8(texturePath);
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::runtime_error("failed to open texture: " + texturePath);
  const auto length = stream.tellg();
  constexpr auto maxEncodedBytes = 256ull * 1024 * 1024;
  if (length <= 0 || static_cast<uint64_t>(length) > maxEncodedBytes)
    throw std::runtime_error("texture file is empty or exceeds the encoded byte budget: " + texturePath);
  std::vector<std::byte> bytes(static_cast<size_t>(length));
  stream.seekg(0);
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("failed to read texture: " + texturePath);
  return createTextureFromEncodedBytes(container::util::pathToUtf8(path.lexically_normal()), bytes, format);
}

container::material::TextureResource AllocationManager::createTextureFromEncodedBytes(
    const std::string& textureName, std::span<const std::byte> encodedBytes, VkFormat format) {
  if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
    throw std::invalid_argument("encoded texture requires a linear or sRGB color-space request: " + textureName);
  try {
    const auto image = container::material::DecodeTextureImage(encodedBytes,
        textureDecodeOptions(format == VK_FORMAT_R8G8B8A8_SRGB));
    return createTextureFromImageData(textureName, image);
  } catch (const std::exception& error) {
    throw std::runtime_error("texture '" + textureName + "': " + error.what());
  }
}

container::material::TextureResource AllocationManager::createTextureFromRgbaPixels(
    const std::string& textureName, std::span<const std::byte> rgbaPixels,
    uint32_t width, uint32_t height, VkFormat format,
    std::span<const uint32_t> queueFamilies) {
  if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
    throw std::invalid_argument("RGBA8 pixel upload requires an RGBA8 format: " + textureName);
  const auto imageSize = container::material::TextureMipByteSize(format, width, height);
  if (imageSize > 256ull * 1024 * 1024 || imageSize > rgbaPixels.size())
    throw std::runtime_error("texture pixel payload is truncated or exceeds the decoded byte budget: " + textureName);
  container::material::TextureImageData image;
  image.format = format;
  image.mips.push_back({width, height, {rgbaPixels.begin(), rgbaPixels.begin() + static_cast<size_t>(imageSize)}});
  return createTextureFromImageData(textureName, image, queueFamilies);
}

container::material::TextureArrayResource
AllocationManager::createTexture2DArrayFromRgbaPixels(
    const std::string& textureName,
    std::span<const std::byte> rgbaPixels,
    uint32_t width,
    uint32_t height,
    uint32_t layerCount,
    VkFormat format,
    TextureAllocationLifetime lifetime) {
  const UploadOperation operation(*this);
  if (!memoryManager_ || !device_)
    throw std::runtime_error("texture array allocation is not initialized: " + textureName);
  const auto batchGroup = textureAllocationGroup(lifetime);
  if (width == 0u || height == 0u || layerCount == 0u) {
    throw std::runtime_error("texture array dimensions are invalid: " +
                             textureName);
  }

  const VkDeviceSize layerSize = static_cast<VkDeviceSize>(width) *
                                 static_cast<VkDeviceSize>(height) * 4;
  const VkDeviceSize imageSize =
      layerSize * static_cast<VkDeviceSize>(layerCount);
  if (rgbaPixels.size() < static_cast<size_t>(imageSize)) {
    throw std::runtime_error("texture array pixel payload is truncated: " +
                             textureName);
  }

  container::material::TextureArrayResource resource{};
  resource.name = textureName;

  StagingBuffer stagingBuffer(*memoryManager_, imageSize);
  stagingBuffer.upload(rgbaPixels.first(static_cast<size_t>(imageSize)));

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = layerCount;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  if (vmaCreateImage(memoryManager_->allocator(), &imageInfo, &allocInfo,
                     &image, &allocation, nullptr) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture array image");
  }

  VkImageView imageView = VK_NULL_HANDLE;
  bool registeredTexture = false;
  try {
    transitionImageLayout(image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount);
    copyBufferToImage(stagingBuffer.buffer().buffer, image, width, height,
                      layerCount);
    transitionImageLayout(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          layerCount);
    imageView = createImageView(image, format, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                layerCount);

    (void)registerTextureAllocation({image, imageView, allocation, lifetime, batchGroup});
    registeredTexture = true;
  } catch (...) {
    if (imageView != VK_NULL_HANDLE) {
      destroyVulkanImageView(device_, imageView, nullptr);
    }
    if (!registeredTexture && image != VK_NULL_HANDLE) {
      vmaDestroyImage(memoryManager_->allocator(), image, allocation);
    }
    throw;
  }

  resource.image = image;
  resource.imageView = imageView;
  resource.width = width;
  resource.height = height;
  resource.layerCount = layerCount;
  return resource;
}

void AllocationManager::resetTextureAllocations(
    TextureAllocationResetScope scope) {
  if (activeUploadOperations_)
    throw std::logic_error("cannot reset textures during an allocation upload");
  if (activeTextureBatchGroup_)
    throw std::logic_error("cannot reset an active texture allocation batch");
  if (textureRetirementInProgress_)
    throw std::logic_error("cannot reset during texture retirement");
  TextureRetirementGuard retiring(textureRetirementInProgress_);
  if (!memoryManager_) {
    textureAllocations_.clear();
    return;
  }

  const auto shouldReset = [scope](const TextureAllocation& texture) {
    return scope == TextureAllocationResetScope::All ||
           texture.lifetime == TextureAllocationLifetime::Scene;
  };

  size_t retired = 0;
  for (auto& texture : textureAllocations_) {
    if (!shouldReset(texture)) {
      continue;
    }

    destroyTextureAllocation(texture);
    if (loadProgress_ && ++retired % 32u == 0u) loadProgress_(true);
  }

  std::erase_if(textureAllocations_, shouldReset);
}

/* ---------- Command helpers ---------- */

void AllocationManager::executeStagedCommands(
    const std::function<void(VkCommandBuffer)>& record) {
  const UploadOperation operation(*this);
  if (!record || !device_ || !memoryManager_ || !graphicsQueue_ || !commandPool_)
    throw std::invalid_argument("staged commands require an initialized allocator and recorder");
  const auto command = beginSingleTimeCommands();
  try {
    record(command);
  } catch (...) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &command);
    throw;
  }
  // endSingleTimeCommands assumes command ownership immediately and retires
  // submitted work before unwinding a failing event/progress callback.
  endSingleTimeCommands(command);
}

VkCommandBuffer AllocationManager::beginSingleTimeCommands() {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool_;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to allocate single-use command buffer");
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    throw std::runtime_error("failed to begin single-use command buffer");
  }
  return commandBuffer;
}

void AllocationManager::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
  ScopedUploadSubmission submission{device_, commandPool_, commandBuffer};
  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to end single-use command buffer");
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (createOwnedFence(device_, &fenceInfo, nullptr, &submission.fence) != VK_SUCCESS) {
    throw std::runtime_error("failed to create upload fence");
  }
  if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, submission.fence) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to submit single-use command buffer");
  }
  submission.submitted = true;
  VkResult wait;
  do {
    wait = vkWaitForFences(device_, 1, &submission.fence, VK_TRUE, 4000000);
    if (wait == VK_TIMEOUT && loadProgress_) loadProgress_(false);
  } while (wait == VK_TIMEOUT);
  submission.retired = wait == VK_SUCCESS || wait == VK_ERROR_DEVICE_LOST;
  if (wait != VK_SUCCESS) {
    throw std::runtime_error("failed to wait for single-use command buffer");
  }

  submission.release();
  if (loadProgress_) loadProgress_(true);
}

void AllocationManager::copyBuffer(VkBuffer src, VkBuffer dst,
                                   VkDeviceSize size, VkDeviceSize srcOffset,
                                   VkDeviceSize dstOffset) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkBufferCopy region{};
  region.srcOffset = srcOffset;
  region.dstOffset = dstOffset;
  region.size = size;

  vkCmdCopyBuffer(cmd, src, dst, 1, &region);

  endSingleTimeCommands(cmd);
}

void AllocationManager::transitionImageLayout(VkImage image,
                                              VkImageLayout oldLayout,
                                              VkImageLayout newLayout,
                                              uint32_t layerCount,
                                              uint32_t baseMipLevel,
                                              uint32_t levelCount) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = baseMipLevel;
  barrier.subresourceRange.levelCount = levelCount;
  barrier.subresourceRange.layerCount = layerCount;

  VkPipelineStageFlags srcStage;
  VkPipelineStageFlags dstStage;

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  }

  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);

  endSingleTimeCommands(cmd);
}

void AllocationManager::copyBufferToImage(VkBuffer buffer, VkImage image,
                                          uint32_t width, uint32_t height,
                                          uint32_t layerCount) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = layerCount;
  region.imageExtent = {width, height, 1};

  vkCmdCopyBufferToImage(cmd, buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  endSingleTimeCommands(cmd);
}

void AllocationManager::generateTextureMipmaps(VkImage image, VkFormat format,
                                               uint32_t width, uint32_t height,
                                               uint32_t mipLevels) {
  if (mipLevels <= 1u) {
    return;
  }
  if (!textureFormatSupportsLinearBlit(physicalDevice_, format)) {
    throw std::runtime_error(
        "texture format does not support linear mipmap generation");
  }

  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.image = image;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.subresourceRange.levelCount = 1;

  int32_t mipWidth = static_cast<int32_t>(width);
  int32_t mipHeight = static_cast<int32_t>(height);

  for (uint32_t mip = 1u; mip < mipLevels; ++mip) {
    barrier.subresourceRange.baseMipLevel = mip - 1u;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkImageBlit blit{};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = mip - 1u;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {std::max(mipWidth / 2, 1),
                          std::max(mipHeight / 2, 1), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = mip;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;

    vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    mipWidth = std::max(mipWidth / 2, 1);
    mipHeight = std::max(mipHeight / 2, 1);
  }

  barrier.subresourceRange.baseMipLevel = mipLevels - 1u;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  endSingleTimeCommands(cmd);
}

VkImageView AllocationManager::createImageView(VkImage image, VkFormat format,
                                               VkImageViewType viewType,
                                               uint32_t layerCount,
                                               uint32_t levelCount) {
  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image;
  info.viewType = viewType;
  info.format = format;
  info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  info.subresourceRange.levelCount = levelCount;
  info.subresourceRange.layerCount = layerCount;

  VkImageView view = VK_NULL_HANDLE;
  if (createVulkanImageView(device_, &info, nullptr, &view) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image view");
  }
  return view;
}

}  // namespace container::gpu
