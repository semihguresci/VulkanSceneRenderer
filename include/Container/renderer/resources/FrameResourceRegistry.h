#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/RenderTechnique.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

struct TechniqueResourceKey {
  RenderTechniqueId technique{RenderTechniqueId::DeferredRaster};
  std::string name{};

  [[nodiscard]] bool operator==(const TechniqueResourceKey& other) const {
    return technique == other.technique && name == other.name;
  }
};

enum class FrameResourceKind {
  Image,
  Buffer,
  Framebuffer,
  DescriptorSet,
  Sampler,
  External,
};

enum class FrameResourceLifetime {
  PerFrame,
  Persistent,
  Imported,
};

struct FrameImageDesc {
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkExtent3D extent{0, 0, 1};
  VkImageUsageFlags usage{0};
  VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
};

struct FrameBufferDesc {
  VkDeviceSize size{0};
  VkBufferUsageFlags usage{0};
};

struct FrameFramebufferDesc {
  uint32_t attachmentCount{0};
};

struct FrameSamplerDesc {};

struct FrameResourceDesc {
  TechniqueResourceKey key{};
  FrameResourceKind kind{FrameResourceKind::External};
  FrameResourceLifetime lifetime{FrameResourceLifetime::PerFrame};
  FrameImageDesc image{};
  FrameBufferDesc buffer{};
  FrameFramebufferDesc framebuffer{};
  FrameSamplerDesc sampler{};
};

struct FrameResourceHandle {
  static constexpr std::size_t kInvalidId =
      std::numeric_limits<std::size_t>::max();

  std::size_t id{kInvalidId};

  [[nodiscard]] bool valid() const { return id != kInvalidId; }
  [[nodiscard]] bool operator==(const FrameResourceHandle&) const = default;
};

struct FrameImageBinding {
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkExtent3D extent{0, 0, 1};
  VkImageUsageFlags usage{0};
  VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
};

struct FrameBufferBinding {
  VkBuffer buffer{VK_NULL_HANDLE};
  VkDeviceSize size{0};
  VkBufferUsageFlags usage{0};
};

struct FrameFramebufferBinding {
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  VkRenderPass renderPass{VK_NULL_HANDLE};
  VkExtent2D extent{};
  uint32_t attachmentCount{0};
};

struct FrameDescriptorBinding {
  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
};

struct FrameSamplerBinding {
  VkSampler sampler{VK_NULL_HANDLE};
};

struct FrameResourceBinding {
  FrameResourceHandle handle{};
  TechniqueResourceKey key{};
  uint32_t frameIndex{0};
  FrameResourceKind kind{FrameResourceKind::External};
  FrameImageBinding image{};
  FrameBufferBinding buffer{};
  FrameFramebufferBinding framebuffer{};
  FrameDescriptorBinding descriptor{};
  FrameSamplerBinding sampler{};
};

class FrameResourceRegistry {
 public:
  void registerResource(FrameResourceDesc desc);
  void registerImage(RenderTechniqueId technique, std::string name,
                     FrameImageDesc desc,
                     FrameResourceLifetime lifetime =
                         FrameResourceLifetime::PerFrame);
  void registerBuffer(RenderTechniqueId technique, std::string name,
                      FrameBufferDesc desc,
                      FrameResourceLifetime lifetime =
                          FrameResourceLifetime::PerFrame);
  void registerFramebuffer(RenderTechniqueId technique, std::string name,
                           FrameFramebufferDesc desc,
                           FrameResourceLifetime lifetime =
                               FrameResourceLifetime::PerFrame);
  void registerDescriptorSet(
      RenderTechniqueId technique, std::string name,
      FrameResourceLifetime lifetime = FrameResourceLifetime::PerFrame);
  void registerSampler(RenderTechniqueId technique, std::string name,
                       FrameSamplerDesc desc,
                       FrameResourceLifetime lifetime =
                           FrameResourceLifetime::PerFrame);
  void registerExternal(RenderTechniqueId technique, std::string name);

  [[nodiscard]] const FrameResourceDesc* find(
      const TechniqueResourceKey& key) const;
  [[nodiscard]] bool contains(const TechniqueResourceKey& key) const {
    return find(key) != nullptr;
  }
  [[nodiscard]] std::vector<const FrameResourceDesc*> resourcesForTechnique(
      RenderTechniqueId technique) const;
  [[nodiscard]] std::size_t size() const { return resources_.size(); }

  FrameResourceHandle bindImage(RenderTechniqueId technique, std::string name,
                                uint32_t frameIndex, FrameImageBinding image);
  FrameResourceHandle bindBuffer(RenderTechniqueId technique, std::string name,
                                 uint32_t frameIndex,
                                 FrameBufferBinding buffer);
  FrameResourceHandle bindFramebuffer(RenderTechniqueId technique,
                                      std::string name, uint32_t frameIndex,
                                      FrameFramebufferBinding framebuffer);
  FrameResourceHandle bindDescriptorSet(RenderTechniqueId technique,
                                        std::string name, uint32_t frameIndex,
                                        FrameDescriptorBinding descriptor);
  FrameResourceHandle bindSampler(RenderTechniqueId technique,
                                  std::string name, uint32_t frameIndex,
                                  FrameSamplerBinding sampler);

  [[nodiscard]] const FrameResourceBinding* findBinding(
      FrameResourceHandle handle) const;
  [[nodiscard]] const FrameResourceBinding* findBinding(
      const TechniqueResourceKey& key, uint32_t frameIndex) const;
  [[nodiscard]] std::vector<const FrameResourceBinding*> bindingsForTechnique(
      RenderTechniqueId technique) const;
  [[nodiscard]] std::vector<const FrameResourceBinding*> bindingsForFrame(
      RenderTechniqueId technique, uint32_t frameIndex) const;
  template <typename Visitor>
  void forEachBindingForFrame(RenderTechniqueId technique, uint32_t frameIndex,
                              Visitor visitor) const {
    for (const FrameResourceBinding& binding : bindings_) {
      if (binding.key.technique == technique &&
          binding.frameIndex == frameIndex) {
        visitor(binding);
      }
    }
  }
  [[nodiscard]] std::size_t bindingCount() const { return bindings_.size(); }

  void clearTechnique(RenderTechniqueId technique);
  void clearBindings();
  void clear();

 private:
  [[nodiscard]] std::size_t findIndex(const TechniqueResourceKey& key) const;
  [[nodiscard]] std::size_t findBindingIndex(
      const TechniqueResourceKey& key, uint32_t frameIndex) const;

  std::vector<FrameResourceDesc> resources_{};
  std::vector<FrameResourceBinding> bindings_{};
  std::size_t nextBindingHandleId_{0};
};

}  // namespace container::renderer
