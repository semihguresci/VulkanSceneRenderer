#pragma once

#include "Container/app/AppConfig.h"
#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonVMA.h"
#include "Container/geometry/Vertex.h"
#include "Container/utility/MaterialManager.h"
#include "Container/utility/TextureResource.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace container::gpu {

enum class TextureAllocationLifetime : uint32_t {
  Scene = 0,
  Persistent = 1,
};

enum class TextureAllocationResetScope : uint32_t {
  SceneOnly = 0,
  All = 1,
};

struct TextureAllocation {
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  VmaAllocation allocation{nullptr};
  TextureAllocationLifetime lifetime{TextureAllocationLifetime::Scene};
};

class AllocationManager {
 public:
  AllocationManager() = default;
  ~AllocationManager();

  void initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
                  VkDevice device, VkQueue graphicsQueue,
                  VkCommandPool commandPool, const app::AppConfig& config);

  void cleanup();

  BufferSlice uploadVertices(std::span<const geometry::Vertex> vertices);
  BufferSlice uploadIndices(std::span<const uint32_t> indices);
  AllocatedBuffer uploadBuffer(std::span<const std::byte> bytes,
                               VkBufferUsageFlags usage);

  [[nodiscard]] AllocatedBuffer createBuffer(
      VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage,
      VmaAllocationCreateFlags allocationFlags = 0,
      VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE);

  void destroyBuffer(AllocatedBuffer& buffer);

  container::material::TextureResource createTextureFromFile(
      const std::string& texturePath,
      VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);
  container::material::TextureResource createTextureFromEncodedBytes(
      const std::string& textureName,
      std::span<const std::byte> encodedBytes,
      VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);
  container::material::TextureArrayResource createTexture2DArrayFromRgbaPixels(
      const std::string& textureName,
      std::span<const std::byte> rgbaPixels,
      uint32_t width,
      uint32_t height,
      uint32_t layerCount,
      VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
      TextureAllocationLifetime lifetime = TextureAllocationLifetime::Scene);

  void resetTextureAllocations(
      TextureAllocationResetScope scope =
          TextureAllocationResetScope::SceneOnly);

  [[nodiscard]] VulkanMemoryManager* memoryManager() const { return memoryManager_.get(); }

 private:
  VkCommandBuffer beginSingleTimeCommands();
  void endSingleTimeCommands(VkCommandBuffer commandBuffer);

  void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size,
                  VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);

  void transitionImageLayout(VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout,
                             uint32_t layerCount = 1u,
                             uint32_t baseMipLevel = 0u,
                             uint32_t levelCount = 1u);

  void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                         uint32_t height, uint32_t layerCount = 1u);
  void generateTextureMipmaps(VkImage image, VkFormat format, uint32_t width,
                              uint32_t height, uint32_t mipLevels);

  VkImageView createImageView(VkImage image, VkFormat format,
                              VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D,
                              uint32_t layerCount = 1u,
                              uint32_t levelCount = 1u);
  container::material::TextureResource createTextureFromRgbaPixels(
      const std::string& textureName,
      std::span<const std::byte> rgbaPixels,
      uint32_t width,
      uint32_t height,
      VkFormat format);

  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphicsQueue_{VK_NULL_HANDLE};
  VkCommandPool commandPool_{VK_NULL_HANDLE};
  app::AppConfig config_{};

  std::unique_ptr<VulkanMemoryManager> memoryManager_;
  std::unique_ptr<BufferArena> vertexArena_;
  std::unique_ptr<BufferArena> indexArena_;

  std::vector<TextureAllocation> textureAllocations_{};
};

}  // namespace container::gpu
