#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <exception>

namespace container::renderer::detail {

// SDK 2.4.0 analytical 3.1.6 only. Its fifteen internal narrow-format images are
// accessed through explicit Float32 scalar/vector shader images. Correct the
// host image/view formats, without changing signed shaders or imported images.
class FsrVulkanImageAdapter {
public:
  FsrVulkanImageAdapter(VkDevice device, VkExtent2D render, VkExtent2D display,
                       PFN_vkGetDeviceProcAddr next) noexcept
      : device_(device), next_(next), images_(descriptions(render, display)) {}
  FsrVulkanImageAdapter(const FsrVulkanImageAdapter&) = delete;
  FsrVulkanImageAdapter& operator=(const FsrVulkanImageAdapter&) = delete;

  class Scope {
  public:
    explicit Scope(FsrVulkanImageAdapter& owner) noexcept : previous_(active_) { active_ = &owner; }
    ~Scope() { active_ = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  private:
    FsrVulkanImageAdapter* previous_;
  };

  // Query each actual format/usage combination, including linear filtering:
  // the promoted scalar/vector formats have linearly sampled SRVs in the SDK.
  static bool supports(VkPhysicalDevice physical, VkExtent2D render, VkExtent2D display,
                       PFN_vkGetPhysicalDeviceFormatProperties formatProperties,
                       PFN_vkGetPhysicalDeviceImageFormatProperties imageProperties) noexcept {
    if (!physical || render.width < 2 || render.height < 2 || display.width < render.width ||
        display.height < render.height || !formatProperties || !imageProperties) return false;
    const auto supported = [&](const Image& image) {
      VkFormatProperties format{};
      formatProperties(physical, image.actual, &format);
      auto needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      if (image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) needed |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
      if ((format.optimalTilingFeatures & needed) != needed) return false;
      VkImageFormatProperties limits{};
      if (imageProperties(physical, image.actual, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
              image.usage, 0, &limits) != VK_SUCCESS ||
          limits.maxExtent.width < image.extent.width || limits.maxExtent.height < image.extent.height ||
          limits.maxExtent.depth < 1 || limits.maxMipLevels < image.mips || limits.maxArrayLayers < 1 ||
          !(limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) return false;
      return true;
    };
    for (const auto& image : descriptions(render, display)) if (!supported(image)) return false;
    // This image is caller-owned. Check its real output format, but never add
    // it to the private SDK image ledger or reinterpret any imported view.
    if (!supported({VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, display, 1,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT})) return false;
    return true;
  }

  bool ready() const noexcept {
    return !failed() && std::all_of(images_.begin(), images_.end(), [](const auto& image) { return bool(image.handle); });
  }
  bool failed() const noexcept { return failure_ != nullptr; }
  const char* failure() const noexcept { return failure_ ? failure_ : ""; }
  bool empty() const noexcept {
    return std::none_of(images_.begin(), images_.end(), [](const auto& image) { return bool(image.handle); });
  }

  static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getDeviceProcAddr(VkDevice device, const char* name) noexcept {
    if (!active_) return nullptr; // All SDK calls using this table require Scope.
    if (device != active_->device_) active_->latch("FSR used a different Vulkan device");
    if (std::strcmp(name, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(createImage);
    if (std::strcmp(name, "vkCreateImageView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(createImageView);
    if (std::strcmp(name, "vkDestroyImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(destroyImage);
    return active_->next_(device, name);
  }

private:
  struct Image {
    VkFormat original;
    VkFormat actual;
    VkExtent2D extent;
    uint32_t mips;
    VkImageUsageFlags usage;
    VkImage handle{};
  };
  static std::array<Image, 15> descriptions(VkExtent2D render, VkExtent2D display) noexcept {
    constexpr VkImageUsageFlags common = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr auto attachment = common | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const VkExtent2D half{render.width / 2, render.height / 2};
    return {{{VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, render, 1, attachment}, // Luma1
        {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, render, 1, attachment}, // Luma2
        {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, render, 1, common}, // IntermediateFp16x1
        {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_SFLOAT, half,
            static_cast<uint32_t>(std::bit_width(std::max(half.width, half.height))), common}, // SpdMips
        {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, half, 1, common}, // FarthestDepthMip1
        {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_SFLOAT, render, 1, attachment}, // DilatedVelocity
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SFLOAT, render, 1, attachment}, // Accumulation1
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SFLOAT, render, 1, attachment}, // Accumulation2
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SFLOAT, half, 1, common}, // ShadingChange
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SFLOAT, display, 1, common}, // NewLocks
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, display, 1, attachment}, // InternalUpscaled1
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, display, 1, attachment}, // InternalUpscaled2
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, render, 1, attachment}, // LumaHistory1
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, render, 1, attachment}, // LumaHistory2
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT, render, 1, common}}}; // DilatedReactiveMasks
  }
  void latch(const char* reason) noexcept { if (!failure_) failure_ = reason; }
  template<class Function> Function real(VkDevice device, const char* name) const noexcept {
    return reinterpret_cast<Function>(next_(device, name));
  }
  Image* owned(VkImage handle) noexcept {
    for (auto& image : images_) if (image.handle && image.handle == handle) return &image;
    return nullptr;
  }
  static bool matches(const Image& expected, const VkImageCreateInfo& info) noexcept {
    return !expected.handle && info.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO && !info.pNext && !info.flags &&
        info.imageType == VK_IMAGE_TYPE_2D && info.format == expected.original &&
        info.extent.width == expected.extent.width && info.extent.height == expected.extent.height &&
        info.extent.depth == 1 && info.mipLevels == expected.mips && info.arrayLayers == 1 &&
        info.samples == VK_SAMPLE_COUNT_1_BIT && info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        info.usage == expected.usage && info.sharingMode == VK_SHARING_MODE_EXCLUSIVE &&
        info.queueFamilyIndexCount == 0 && info.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED;
  }
  static bool matches(const Image& expected, const VkImageViewCreateInfo& info) noexcept {
    const auto& range = info.subresourceRange;
    return info.sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO && !info.pNext && !info.flags &&
        info.viewType == VK_IMAGE_VIEW_TYPE_2D &&
        (info.format == expected.original || info.format == expected.actual) &&
        info.components.r == VK_COMPONENT_SWIZZLE_IDENTITY && info.components.g == VK_COMPONENT_SWIZZLE_IDENTITY &&
        info.components.b == VK_COMPONENT_SWIZZLE_IDENTITY && info.components.a == VK_COMPONENT_SWIZZLE_IDENTITY &&
        range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && range.baseArrayLayer == 0 &&
        (range.layerCount == 1 || range.layerCount == VK_REMAINING_ARRAY_LAYERS) &&
        range.baseMipLevel < expected.mips &&
        (range.levelCount == VK_REMAINING_MIP_LEVELS ||
            (range.levelCount && range.levelCount <= expected.mips - range.baseMipLevel));
  }

  static VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice device, const VkImageCreateInfo* info,
      const VkAllocationCallbacks* allocator, VkImage* result) noexcept {
    if (!active_) return VK_ERROR_INITIALIZATION_FAILED;
    auto& self = *active_;
    if (device != self.device_) self.latch("FSR used a different Vulkan device");
    const auto call = self.real<PFN_vkCreateImage>(device, "vkCreateImage");
    Image* selected = nullptr;
    VkImageCreateInfo adapted{};
    if (device == self.device_ && info && (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
        (info->format == VK_FORMAT_R16_SFLOAT || info->format == VK_FORMAT_R16G16_SFLOAT ||
         info->format == VK_FORMAT_R8_UNORM || info->format == VK_FORMAT_R8G8B8A8_UNORM ||
         info->format == VK_FORMAT_R16G16B16A16_SFLOAT)) {
      for (auto& image : self.images_) if (matches(image, *info)) { selected = &image; break; }
      if (selected) {
        adapted = *info;
        adapted.format = selected->actual;
      } else {
        // Do not introduce a driver failure during partial SDK construction.
        // Let the SDK publish a destructible context; the caller then rejects it.
        self.latch("FSR requested an unrecognized internal narrow-format storage image");
      }
    }
    const auto status = call(device, selected ? &adapted : info, allocator, result);
    if (status != VK_SUCCESS) self.latch("FSR Vulkan image creation failed");
    else if (selected) selected->handle = *result;
    return status;
  }

  static VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice device, const VkImageViewCreateInfo* info,
      const VkAllocationCallbacks* allocator, VkImageView* result) noexcept {
    if (!active_) return VK_ERROR_INITIALIZATION_FAILED;
    auto& self = *active_;
    if (device != self.device_) self.latch("FSR used a different Vulkan device");
    const auto call = self.real<PFN_vkCreateImageView>(device, "vkCreateImageView");
    const auto* image = device == self.device_ && info ? self.owned(info->image) : nullptr;
    VkImageViewCreateInfo adapted{};
    if (image) {
      if (matches(*image, *info)) {
        adapted = *info;
        adapted.format = image->actual;
      } else {
        self.latch("FSR requested an unrecognized view of a promoted image");
        // A legal placeholder lets normal SDK destruction clean the candidate.
        // This view is never submitted: prepare rejects or record discards it.
        adapted.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        adapted.image = image->handle;
        adapted.viewType = VK_IMAGE_VIEW_TYPE_2D;
        adapted.format = image->actual;
        adapted.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      }
    }
    const auto status = call(device, image ? &adapted : info, allocator, result);
    if (status != VK_SUCCESS) self.latch("FSR Vulkan image view creation failed");
    return status;
  }

  static VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice device, VkImage image,
      const VkAllocationCallbacks* allocator) noexcept {
    // An unscoped asynchronous SDK callback violates this analytical backend's
    // synchronous API contract. Never pretend a void destroy released an image.
    if (!active_) std::terminate();
    auto& self = *active_;
    if (device != self.device_) self.latch("FSR used a different Vulkan device");
    self.real<PFN_vkDestroyImage>(device, "vkDestroyImage")(device, image, allocator);
    if (device == self.device_) if (auto* record = self.owned(image)) record->handle = VK_NULL_HANDLE;
  }

  inline static thread_local FsrVulkanImageAdapter* active_{};
  VkDevice device_;
  PFN_vkGetDeviceProcAddr next_;
  std::array<Image, 15> images_;
  const char* failure_{};
};

} // namespace container::renderer::detail
