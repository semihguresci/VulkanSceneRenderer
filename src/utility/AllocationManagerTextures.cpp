#include <Container/utility/AllocationManager.h>
#include <Container/utility/TextureUploadPlan.h>
#include <Container/utility/Platform.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace container::gpu {
namespace {
bool supported(VkPhysicalDevice device, VkFormat format) {
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(device, format, &properties);
  constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
  return (properties.optimalTilingFeatures & needed) == needed;
}
bool canBlit(VkPhysicalDevice device, VkFormat format) {
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(device, format, &properties);
  constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
      VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  return (properties.optimalTilingFeatures & needed) == needed;
}
void barrier(VkCommandBuffer command, VkImage image, VkImageLayout before,
             VkImageLayout after, uint32_t first, uint32_t count,
             VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
             VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
  VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  transition.oldLayout = before; transition.newLayout = after;
  transition.srcAccessMask = sourceAccess; transition.dstAccessMask = destinationAccess;
  transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  transition.image = image;
  transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, first, count, 0, 1};
  vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr,
                       0, nullptr, 1, &transition);
}
} // namespace

container::material::TextureDecodeOptions AllocationManager::textureDecodeOptions(
    bool isSrgb, bool strictKhrBasisu) const {
  if (!physicalDevice_) throw std::runtime_error("texture device is not initialized");
  container::material::TextureDecodeOptions options;
  options.isSrgb = isSrgb; options.strictKhrBasisu = strictKhrBasisu;
  VkPhysicalDeviceProperties limits{};
  vkGetPhysicalDeviceProperties(physicalDevice_, &limits);
  options.maxDimension = std::min(options.maxDimension, limits.limits.maxImageDimension2D);
  const auto add = [&](VkFormat format) {
    if (supported(physicalDevice_, format)) options.allowedFormats.push_back(format);
  };
  for (const auto format : {
      VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM,
      VK_FORMAT_R8G8B8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
      VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM,
      VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
      VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
      VK_FORMAT_R32G32B32A32_SFLOAT}) add(format);
  if (enabledTextureFeatures_.textureCompressionBC)
    for (int format = VK_FORMAT_BC1_RGB_UNORM_BLOCK; format <= VK_FORMAT_BC7_SRGB_BLOCK; ++format)
      add(static_cast<VkFormat>(format));
  if (enabledTextureFeatures_.textureCompressionETC2)
    for (int format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK; format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK; ++format)
      add(static_cast<VkFormat>(format));
  if (enabledTextureFeatures_.textureCompressionASTC_LDR)
    for (int format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK; format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++format)
      add(static_cast<VkFormat>(format));
  if (options.allowedFormats.empty()) throw std::runtime_error("device exposes no supported sampled texture output format");
  return options;
}

container::material::TextureResource AllocationManager::createTextureFromImageData(
    const std::string& textureName, const container::material::TextureImageData& imageData) {
  const UploadOperation operation(*this);
  if (!memoryManager_ || !device_ || !physicalDevice_)
    throw std::runtime_error("texture allocation is not initialized: " + textureName);
  const auto batchGroup = textureAllocationGroup(TextureAllocationLifetime::Scene);
  container::material::TextureResource resource;
  resource.name = textureName;
  const auto profile = textureDecodeOptions(false);
  if (std::ranges::find(profile.allowedFormats, imageData.format) == profile.allowedFormats.end())
    throw std::runtime_error("texture format is not enabled and filterable on this device: " + textureName);
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
  const auto plan = PlanTextureImageUpload(imageData, properties.limits.maxImageDimension2D,
      canBlit(physicalDevice_, imageData.format), 256ull * 1024 * 1024,
      std::max<VkDeviceSize>(4, properties.limits.optimalBufferCopyOffsetAlignment));
  StagingBuffer staging(*memoryManager_, plan.stagingBytes);
  auto* destination = static_cast<std::byte*>(staging.data());
  for (size_t level = 0; level < imageData.mips.size(); ++level)
    std::memcpy(destination + plan.copies[level].bufferOffset,
                imageData.mips[level].bytes.data(), imageData.mips[level].bytes.size());
  if (vmaFlushAllocation(memoryManager_->allocator(), staging.buffer().allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
    throw std::runtime_error("failed to flush texture staging bytes: " + textureName);

  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D; info.format = imageData.format;
  info.extent = plan.extent; info.mipLevels = plan.allocatedMipLevels;
  info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo allocationInfo{};
  allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VkImage image{}; VmaAllocation allocation{}; VkImageView view{};
  if (vmaCreateImage(memoryManager_->allocator(), &info, &allocationInfo, &image, &allocation, nullptr) != VK_SUCCESS)
    throw std::runtime_error("failed to allocate typed texture: " + textureName);
  try {
    const auto command = beginSingleTimeCommands();
    barrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, plan.allocatedMipLevels, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(command, staging.buffer().buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(plan.copies.size()), plan.copies.data());
    if (plan.generateMipmaps) {
      int32_t width = static_cast<int32_t>(plan.extent.width), height = static_cast<int32_t>(plan.extent.height);
      for (uint32_t level = 1; level < plan.allocatedMipLevels; ++level) {
        barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            level - 1, 1, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.srcOffsets[1] = {width, height, 1};
        blit.dstOffsets[1] = {std::max(width / 2, 1), std::max(height / 2, 1), 1};
        vkCmdBlitImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        // Every raster and compute ray/DDGI consumer receives the upload.
        barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            level - 1, 1, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        width = std::max(width / 2, 1); height = std::max(height / 2, 1);
      }
      barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          plan.allocatedMipLevels - 1, 1, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    } else {
      barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          0, plan.allocatedMipLevels, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    endSingleTimeCommands(command);
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageData.format; viewInfo.components = imageData.componentMapping;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, plan.allocatedMipLevels, 0, 1};
    if (createVulkanImageView(device_, &viewInfo, nullptr, &view) != VK_SUCCESS)
      throw std::runtime_error("failed to create typed texture view: " + textureName);
    resource.allocationIdentity=registerTextureAllocation({image, view, allocation,
                               TextureAllocationLifetime::Scene, batchGroup});
  } catch (...) {
    if (view) destroyVulkanImageView(device_, view, nullptr);
    vmaDestroyImage(memoryManager_->allocator(), image, allocation);
    throw;
  }
  resource.image = image; resource.imageView = view;
  resource.format = imageData.format; resource.width = plan.extent.width;
  resource.height = plan.extent.height; resource.mipLevels = plan.allocatedMipLevels;
  return resource;
}
} // namespace container::gpu
