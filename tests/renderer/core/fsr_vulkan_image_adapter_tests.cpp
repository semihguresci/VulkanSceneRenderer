#include "renderer/temporal/FsrVulkanImageAdapter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using container::renderer::detail::FsrVulkanImageAdapter;

template<class Handle> Handle fakeHandle(uintptr_t value) {
  if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
  else return static_cast<Handle>(value);
}

constexpr VkExtent2D renderExtent{65, 49};
constexpr VkExtent2D displayExtent{130, 98};
constexpr VkImageUsageFlags computeUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags attachmentUsage = computeUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

// Independent SDK 3.1.6 resource contract: odd render dimensions deliberately
// distinguish integer-half SPD/farthest-depth extents from rounded-up extents.
struct Resource {
  const char* name;
  VkFormat authoredFormat;
  VkFormat shaderFormat;
  VkExtent2D extent;
  uint32_t mips;
  VkImageUsageFlags usage;
};
constexpr std::array<Resource, 15> resources{{
    {"Luma1", VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, {65, 49}, 1, attachmentUsage},
    {"Luma2", VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, {65, 49}, 1, attachmentUsage},
    {"IntermediateFp16x1", VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, {65, 49}, 1, computeUsage},
    {"SpdMips", VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_SFLOAT, {32, 24}, 6, computeUsage},
    {"FarthestDepthMip1", VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, {32, 24}, 1, computeUsage},
    {"DilatedVelocity", VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_SFLOAT, {65, 49}, 1, attachmentUsage},
    {"Accumulation1", VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SFLOAT, {65, 49}, 1, attachmentUsage},
    {"Accumulation2", VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SFLOAT, {65, 49}, 1, attachmentUsage},
    {"ShadingChange", VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SFLOAT, {32, 24}, 1, computeUsage},
    {"NewLocks", VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SFLOAT, {130, 98}, 1, computeUsage},
    {"InternalUpscaled1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, {130, 98}, 1, attachmentUsage},
    {"InternalUpscaled2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, {130, 98}, 1, attachmentUsage},
    {"LumaHistory1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, {65, 49}, 1, attachmentUsage},
    {"LumaHistory2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, {65, 49}, 1, attachmentUsage},
    {"DilatedReactiveMasks", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT, {65, 49}, 1, computeUsage},
}};

VkImageCreateInfo imageInfo(const Resource& resource) {
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = resource.authoredFormat;
  info.extent = {resource.extent.width, resource.extent.height, 1};
  info.mipLevels = resource.mips;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = resource.usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return info;
}

VkImageViewCreateInfo viewInfo(VkImage image, VkFormat format, uint32_t baseMip = 0,
                               uint32_t levelCount = 1) {
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = format;
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, levelCount, 0, 1};
  return info;
}

void expectSameImageInfo(const VkImageCreateInfo& actual, const VkImageCreateInfo& expected) {
  EXPECT_EQ(actual.sType, expected.sType);
  EXPECT_EQ(actual.pNext, expected.pNext);
  EXPECT_EQ(actual.flags, expected.flags);
  EXPECT_EQ(actual.imageType, expected.imageType);
  EXPECT_EQ(actual.format, expected.format);
  EXPECT_EQ(actual.extent.width, expected.extent.width);
  EXPECT_EQ(actual.extent.height, expected.extent.height);
  EXPECT_EQ(actual.extent.depth, expected.extent.depth);
  EXPECT_EQ(actual.mipLevels, expected.mipLevels);
  EXPECT_EQ(actual.arrayLayers, expected.arrayLayers);
  EXPECT_EQ(actual.samples, expected.samples);
  EXPECT_EQ(actual.tiling, expected.tiling);
  EXPECT_EQ(actual.usage, expected.usage);
  EXPECT_EQ(actual.sharingMode, expected.sharingMode);
  EXPECT_EQ(actual.queueFamilyIndexCount, expected.queueFamilyIndexCount);
  EXPECT_EQ(actual.pQueueFamilyIndices, expected.pQueueFamilyIndices);
  EXPECT_EQ(actual.initialLayout, expected.initialLayout);
}

void expectSameViewInfo(const VkImageViewCreateInfo& actual, const VkImageViewCreateInfo& expected) {
  EXPECT_EQ(actual.sType, expected.sType);
  EXPECT_EQ(actual.pNext, expected.pNext);
  EXPECT_EQ(actual.flags, expected.flags);
  EXPECT_EQ(actual.image, expected.image);
  EXPECT_EQ(actual.viewType, expected.viewType);
  EXPECT_EQ(actual.format, expected.format);
  EXPECT_EQ(actual.components.r, expected.components.r);
  EXPECT_EQ(actual.components.g, expected.components.g);
  EXPECT_EQ(actual.components.b, expected.components.b);
  EXPECT_EQ(actual.components.a, expected.components.a);
  EXPECT_EQ(actual.subresourceRange.aspectMask, expected.subresourceRange.aspectMask);
  EXPECT_EQ(actual.subresourceRange.baseMipLevel, expected.subresourceRange.baseMipLevel);
  EXPECT_EQ(actual.subresourceRange.levelCount, expected.subresourceRange.levelCount);
  EXPECT_EQ(actual.subresourceRange.baseArrayLayer, expected.subresourceRange.baseArrayLayer);
  EXPECT_EQ(actual.subresourceRange.layerCount, expected.subresourceRange.layerCount);
}

struct MockDriver {
  struct ImageCall {
    VkDevice device;
    const VkImageCreateInfo* address;
    VkImageCreateInfo info;
    const VkAllocationCallbacks* allocator;
  };
  struct ViewCall {
    VkDevice device;
    const VkImageViewCreateInfo* address;
    VkImageViewCreateInfo info;
    const VkAllocationCallbacks* allocator;
  };
  struct DestroyCall {
    VkDevice device;
    VkImage image;
    const VkAllocationCallbacks* allocator;
  };
  struct ImageQuery {
    VkPhysicalDevice physical;
    VkFormat format;
    VkImageType type;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    VkImageCreateFlags flags;
  };
  struct LiveImage { VkDevice device; VkImage image; VkImageCreateInfo info; };

  inline static thread_local MockDriver* current{};
  VkDevice device = fakeHandle<VkDevice>(0x100);
  VkPhysicalDevice physical = fakeHandle<VkPhysicalDevice>(0x200);
  VkResult imageResult = VK_SUCCESS;
  VkResult viewResult = VK_SUCCESS;
  VkResult propertiesResult = VK_SUCCESS;
  uintptr_t nextImage = 0x1000;
  uintptr_t nextView = 0x2000;
  std::vector<ImageCall> images;
  std::vector<ViewCall> views;
  std::vector<DestroyCall> destroys;
  std::vector<LiveImage> live;
  std::vector<std::pair<VkPhysicalDevice, VkFormat>> formatQueries;
  std::vector<ImageQuery> imageQueries;
  std::vector<std::pair<VkDevice, std::string>> otherQueries;
  VkFormatFeatureFlags scalarFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  VkFormatFeatureFlags vectorFeatures = scalarFeatures;
  VkFormatFeatureFlags rgbaFeatures = scalarFeatures;
  VkFormatFeatureFlags otherTilingFeatures{};
  VkImageFormatProperties limits{{130, 98, 1}, 6, 1, VK_SAMPLE_COUNT_1_BIT, 1ull << 30};
  VkImageFormatProperties outputLimits = limits;
  VkResult outputPropertiesResult = VK_SUCCESS;

  static VKAPI_ATTR void VKAPI_CALL sentinelA() {}
  static VKAPI_ATTR void VKAPI_CALL sentinelB() {}

  static PFN_vkVoidFunction resolve(VkDevice device, const char* name, PFN_vkVoidFunction fallback) {
    if (std::strcmp(name, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(createImage);
    if (std::strcmp(name, "vkCreateImageView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(createImageView);
    if (std::strcmp(name, "vkDestroyImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(destroyImage);
    current->otherQueries.emplace_back(device, name);
    return fallback;
  }
  static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getProcA(VkDevice device, const char* name) {
    return resolve(device, name, sentinelA);
  }
  static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getProcB(VkDevice device, const char* name) {
    return resolve(device, name, sentinelB);
  }
  static VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice device, const VkImageCreateInfo* info,
      const VkAllocationCallbacks* allocator, VkImage* image) {
    current->images.push_back({device, info, *info, allocator});
    if (current->imageResult != VK_SUCCESS) return current->imageResult;
    *image = fakeHandle<VkImage>(current->nextImage++);
    current->live.push_back({device, *image, *info});
    return VK_SUCCESS;
  }
  static VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice device, const VkImageViewCreateInfo* info,
      const VkAllocationCallbacks* allocator, VkImageView* view) {
    current->views.push_back({device, info, *info, allocator});
    for (const auto& image : current->live) {
      if (image.device != device || image.image != info->image) continue;
      // These checks are against what the mocked driver actually allocated,
      // independent of the adapter's original half-format resource metadata.
      EXPECT_EQ(info->format, image.info.format);
      EXPECT_EQ(info->viewType, VK_IMAGE_VIEW_TYPE_2D);
      EXPECT_EQ(info->subresourceRange.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
      EXPECT_LT(info->subresourceRange.baseMipLevel, image.info.mipLevels);
      EXPECT_TRUE(info->subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS ||
          (info->subresourceRange.levelCount > 0 && info->subresourceRange.levelCount <=
              image.info.mipLevels - info->subresourceRange.baseMipLevel));
      EXPECT_EQ(info->subresourceRange.baseArrayLayer, 0u);
      EXPECT_TRUE(info->subresourceRange.layerCount == 1 ||
          info->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS);
    }
    if (current->viewResult != VK_SUCCESS) return current->viewResult;
    *view = fakeHandle<VkImageView>(current->nextView++);
    return VK_SUCCESS;
  }
  static VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice device, VkImage image,
      const VkAllocationCallbacks* allocator) {
    current->destroys.push_back({device, image, allocator});
    std::erase_if(current->live, [&](const auto& live) { return live.device == device && live.image == image; });
  }
  static VKAPI_ATTR void VKAPI_CALL formatProperties(VkPhysicalDevice physical, VkFormat format,
      VkFormatProperties* properties) {
    current->formatQueries.emplace_back(physical, format);
    EXPECT_TRUE(format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT ||
        format == VK_FORMAT_R32G32B32A32_SFLOAT);
    *properties = {};
    properties->linearTilingFeatures = current->otherTilingFeatures;
    properties->bufferFeatures = current->otherTilingFeatures;
    properties->optimalTilingFeatures = format == VK_FORMAT_R32_SFLOAT ? current->scalarFeatures :
        format == VK_FORMAT_R32G32_SFLOAT ? current->vectorFeatures : current->rgbaFeatures;
  }
  static VKAPI_ATTR VkResult VKAPI_CALL imageProperties(VkPhysicalDevice physical, VkFormat format,
      VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
      VkImageFormatProperties* properties) {
    current->imageQueries.push_back({physical, format, type, tiling, usage, flags});
    *properties = current->limits;
    constexpr auto outputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (format == VK_FORMAT_R32G32B32A32_SFLOAT && usage == outputUsage) {
      *properties = current->outputLimits;
      return current->outputPropertiesResult;
    }
    return current->propertiesResult;
  }
};

class FsrVulkanImageAdapterTest : public ::testing::Test {
protected:
  void SetUp() override { MockDriver::current = &driver; }
  void TearDown() override { MockDriver::current = nullptr; }

  template<class Function> Function proc(VkDevice device, const char* name) {
    return reinterpret_cast<Function>(FsrVulkanImageAdapter::getDeviceProcAddr(device, name));
  }
  VkResult create(const VkImageCreateInfo& info, VkImage& image,
                  const VkAllocationCallbacks* allocator = nullptr) {
    return proc<PFN_vkCreateImage>(driver.device, "vkCreateImage")(driver.device, &info, allocator, &image);
  }
  VkResult view(const VkImageViewCreateInfo& info, VkImageView& imageView,
                const VkAllocationCallbacks* allocator = nullptr) {
    return proc<PFN_vkCreateImageView>(driver.device, "vkCreateImageView")(
        driver.device, &info, allocator, &imageView);
  }
  void destroy(VkImage image, const VkAllocationCallbacks* allocator = nullptr) {
    proc<PFN_vkDestroyImage>(driver.device, "vkDestroyImage")(driver.device, image, allocator);
  }
  bool supported(VkExtent2D extent = renderExtent, VkExtent2D display = displayExtent) {
    return FsrVulkanImageAdapter::supports(driver.physical, extent, display,
        MockDriver::formatProperties, MockDriver::imageProperties);
  }
  MockDriver driver;
};

TEST_F(FsrVulkanImageAdapterTest, PromotesExactlyTheFifteenSdkResourcesWithoutChangingCallerInputs) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter::Scope scope(adapter);
  VkAllocationCallbacks allocator{};
  allocator.pUserData = &driver;
  const uint32_t unusedQueue = 71;
  EXPECT_TRUE(adapter.empty());
  EXPECT_FALSE(adapter.ready());
  EXPECT_STREQ(adapter.failure(), "");
  std::array<VkImage, 15> handles{};
  for (size_t index = 0; index < resources.size(); ++index) {
    const auto& resource = resources[index];
    SCOPED_TRACE(resource.name);
    auto info = imageInfo(resource);
    info.pQueueFamilyIndices = &unusedQueue; // Ignored with a zero queue count; must survive the copy.
    const auto original = info;
    ASSERT_EQ(create(info, handles[index], &allocator), VK_SUCCESS);
    ASSERT_NE(handles[index], VK_NULL_HANDLE);
    auto promoted = original;
    promoted.format = resource.shaderFormat;
    expectSameImageInfo(driver.images.back().info, promoted);
    expectSameImageInfo(info, original);
    EXPECT_NE(driver.images.back().address, &info);
    EXPECT_EQ(driver.images.back().device, driver.device);
    EXPECT_EQ(driver.images.back().allocator, &allocator);
    EXPECT_FALSE(adapter.empty());
    EXPECT_EQ(adapter.ready(), index + 1 == resources.size());
    EXPECT_FALSE(adapter.failed());

    auto infoView = viewInfo(handles[index], resource.authoredFormat, 0, resource.mips);
    const auto originalView = infoView;
    VkImageView createdView{};
    ASSERT_EQ(view(infoView, createdView, &allocator), VK_SUCCESS);
    ASSERT_NE(createdView, VK_NULL_HANDLE);
    auto promotedView = originalView;
    promotedView.format = resource.shaderFormat;
    expectSameViewInfo(driver.views.back().info, promotedView);
    expectSameViewInfo(infoView, originalView);
    EXPECT_NE(driver.views.back().address, &infoView);
    EXPECT_EQ(driver.views.back().allocator, &allocator);
  }
  for (const auto image : handles) destroy(image, &allocator);
  EXPECT_TRUE(adapter.empty());
  EXPECT_FALSE(adapter.ready());
  EXPECT_FALSE(adapter.failed());
  ASSERT_EQ(driver.destroys.size(), handles.size());
  for (size_t index = 0; index < handles.size(); ++index) {
    EXPECT_EQ(driver.destroys[index].image, handles[index]);
    EXPECT_EQ(driver.destroys[index].allocator, &allocator);
  }
}

TEST_F(FsrVulkanImageAdapterTest, SupportsAllSpdMipViewsAndRepeatedDynamicReimportOfTheSameImage) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter::Scope scope(adapter);
  VkImage spd{}, velocity{};
  ASSERT_EQ(create(imageInfo(resources[3]), spd), VK_SUCCESS);
  ASSERT_EQ(create(imageInfo(resources[5]), velocity), VK_SUCCESS);
  for (uint32_t mip = 0; mip < 6; ++mip) {
    VkImageView created{};
    ASSERT_EQ(view(viewInfo(spd, VK_FORMAT_R16G16_SFLOAT, mip), created), VK_SUCCESS);
    EXPECT_EQ(driver.views.back().info.format, VK_FORMAT_R32G32_SFLOAT);
    EXPECT_EQ(driver.views.back().info.subresourceRange.baseMipLevel, mip);
    EXPECT_EQ(driver.views.back().info.subresourceRange.levelCount, 1u);
  }
  auto remaining = viewInfo(spd, VK_FORMAT_R16G16_SFLOAT, 2, VK_REMAINING_MIP_LEVELS);
  remaining.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
  VkImageView created{};
  ASSERT_EQ(view(remaining, created), VK_SUCCESS);
  auto expected = remaining;
  expected.format = VK_FORMAT_R32G32_SFLOAT;
  expectSameViewInfo(driver.views.back().info, expected);
  // The provider re-registers its private shared velocity handle every frame.
  // Both the original SDK metadata and already-promoted metadata are legal.
  for (const auto format : {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16_SFLOAT}) {
    ASSERT_EQ(view(viewInfo(velocity, format), created), VK_SUCCESS);
    EXPECT_EQ(driver.views.back().info.image, velocity);
    EXPECT_EQ(driver.views.back().info.format, VK_FORMAT_R32G32_SFLOAT);
  }
  EXPECT_FALSE(adapter.failed());
}

TEST_F(FsrVulkanImageAdapterTest, CallerOwnedViewsAndOtherSdkFormatsPassThroughUnchanged) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter::Scope scope(adapter);
  VkAllocationCallbacks allocator{};
  for (const auto format : {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8_UNORM,
                            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM}) {
    auto info = viewInfo(fakeHandle<VkImage>(0xabc), format);
    const auto original = info;
    VkImageView created{};
    ASSERT_EQ(view(info, created, &allocator), VK_SUCCESS);
    expectSameViewInfo(driver.views.back().info, original);
    EXPECT_EQ(driver.views.back().address, &info);
    EXPECT_EQ(driver.views.back().allocator, &allocator);
  }
  for (const auto format : {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16B16A16_UNORM,
                            VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}) {
    auto info = imageInfo(resources[0]);
    info.format = format;
    const auto original = info;
    VkImage created{};
    ASSERT_EQ(create(info, created), VK_SUCCESS);
    expectSameImageInfo(driver.images.back().info, original);
    EXPECT_EQ(driver.images.back().address, &info);
  }
  EXPECT_TRUE(adapter.empty());
  EXPECT_FALSE(adapter.failed());
}

TEST_F(FsrVulkanImageAdapterTest, SampledOnlyNarrowImagesPreserveTheirPackedUploadFormats) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter::Scope scope(adapter);
  // The 1x1 R8 default mask has a byte initialization upload. Also exercise
  // larger sampled-only images with exactly the dimensions of owned classes:
  // their transfer bytes still describe the original packed format.
  for (const size_t index : {size_t{0}, size_t{3}, size_t{6}, size_t{10}, size_t{14}}) {
    for (const bool oneTexel : {false, true}) {
      SCOPED_TRACE(resources[index].name);
      SCOPED_TRACE(oneTexel);
      auto info = imageInfo(resources[index]);
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.mipLevels = 1;
      if (oneTexel) info.extent = {1, 1, 1};
      const auto original = info;
      VkImage image{};
      ASSERT_EQ(create(info, image), VK_SUCCESS);
      expectSameImageInfo(driver.images.back().info, original);
      expectSameImageInfo(info, original);
      EXPECT_EQ(driver.images.back().address, &info);
      auto imageView = viewInfo(image, original.format);
      VkImageView created{};
      ASSERT_EQ(view(imageView, created), VK_SUCCESS);
      EXPECT_EQ(driver.views.back().address, &imageView);
      expectSameViewInfo(driver.views.back().info, imageView);
      destroy(image);
      EXPECT_TRUE(adapter.empty());
      EXPECT_FALSE(adapter.failed());
    }
  }
}

TEST_F(FsrVulkanImageAdapterTest, DestroyedHandleReuseDoesNotPromoteAnUnrelatedImageView) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter::Scope scope(adapter);
  VkImage first{};
  ASSERT_EQ(create(imageInfo(resources[0]), first), VK_SUCCESS);
  destroy(first);
  EXPECT_TRUE(adapter.empty());

  driver.nextImage = 0x1000;
  auto unrelated = imageInfo(resources[0]);
  unrelated.format = VK_FORMAT_R8_UNORM;
  unrelated.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImage reused{};
  ASSERT_EQ(create(unrelated, reused), VK_SUCCESS);
  ASSERT_EQ(reused, first);
  auto info = viewInfo(reused, VK_FORMAT_R8_UNORM);
  VkImageView created{};
  ASSERT_EQ(view(info, created), VK_SUCCESS);
  EXPECT_EQ(driver.views.back().address, &info);
  EXPECT_EQ(driver.views.back().info.format, VK_FORMAT_R8_UNORM);
  EXPECT_TRUE(adapter.empty());
  destroy(reused);

  driver.nextImage = 0x1000;
  VkImage promotedAgain{};
  ASSERT_EQ(create(imageInfo(resources[0]), promotedAgain), VK_SUCCESS);
  ASSERT_EQ(promotedAgain, first);
  ASSERT_EQ(view(viewInfo(promotedAgain, VK_FORMAT_R16_SFLOAT), created), VK_SUCCESS);
  EXPECT_EQ(driver.views.back().info.format, VK_FORMAT_R32_SFLOAT);
  EXPECT_FALSE(adapter.empty());
  EXPECT_FALSE(adapter.failed());
}

TEST_F(FsrVulkanImageAdapterTest, UnexpectedStorageImageShapesPassThroughAndLatchWithoutInventingAnError) {
  using Mutation = void (*)(VkImageCreateInfo&);
  const std::array<Mutation, 14> mutations{{
      [](auto& info) { info.arrayLayers = 2; },
      [](auto& info) { info.imageType = VK_IMAGE_TYPE_3D; },
      [](auto& info) { info.extent.depth = 2; },
      [](auto& info) { ++info.extent.width; },
      [](auto& info) { ++info.extent.height; },
      [](auto& info) { info.mipLevels = 2; },
      [](auto& info) { info.samples = VK_SAMPLE_COUNT_4_BIT; },
      [](auto& info) { info.tiling = VK_IMAGE_TILING_LINEAR; },
      [](auto& info) { info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; },
      [](auto& info) { info.pNext = reinterpret_cast<void*>(uintptr_t{1}); },
      [](auto& info) { info.sharingMode = VK_SHARING_MODE_CONCURRENT; },
      [](auto& info) { info.queueFamilyIndexCount = 1; },
      [](auto& info) { info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED; },
      [](auto& info) { info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; },
  }};
  for (const size_t resourceIndex : {size_t{0}, size_t{3}, size_t{6}, size_t{10}, size_t{14}}) {
    SCOPED_TRACE(resources[resourceIndex].name);
    for (size_t index = 0; index < mutations.size(); ++index) {
      SCOPED_TRACE(index);
      FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
      FsrVulkanImageAdapter::Scope scope(adapter);
      auto info = imageInfo(resources[resourceIndex]);
      mutations[index](info);
      const auto original = info;
      VkImage created{};
      ASSERT_EQ(create(info, created), VK_SUCCESS);
      ASSERT_NE(created, VK_NULL_HANDLE);
      expectSameImageInfo(driver.images.back().info, original);
      expectSameImageInfo(info, original);
      EXPECT_EQ(driver.images.back().address, &info);
      EXPECT_TRUE(adapter.failed());
      EXPECT_FALSE(adapter.ready());
      EXPECT_TRUE(adapter.empty());
      EXPECT_STRNE(adapter.failure(), "");
    }
  }
}

TEST_F(FsrVulkanImageAdapterTest, AnExtraMatchingImageIsNotSilentlyClaimedAsAnotherResource) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter::Scope scope(adapter);
  VkImage image{};
  ASSERT_EQ(create(imageInfo(resources[0]), image), VK_SUCCESS);
  ASSERT_EQ(create(imageInfo(resources[1]), image), VK_SUCCESS);
  EXPECT_FALSE(adapter.failed());
  const auto third = imageInfo(resources[0]);
  ASSERT_EQ(create(third, image), VK_SUCCESS);
  EXPECT_EQ(driver.images.back().address, &third);
  EXPECT_EQ(driver.images.back().info.format, VK_FORMAT_R16_SFLOAT);
  EXPECT_TRUE(adapter.failed());
}

TEST_F(FsrVulkanImageAdapterTest, UnexpectedPromotedViewsUseARealLegalPlaceholderAndLatch) {
  using Mutation = void (*)(VkImageViewCreateInfo&);
  const std::array<Mutation, 13> mutations{{
      [](auto& info) { info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; },
      [](auto& info) { info.format = VK_FORMAT_R32_UINT; },
      [](auto& info) { info.components.r = VK_COMPONENT_SWIZZLE_R; },
      [](auto& info) { info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; },
      [](auto& info) { info.subresourceRange.baseMipLevel = 6; },
      [](auto& info) { info.subresourceRange.levelCount = 0; },
      [](auto& info) { info.subresourceRange.levelCount = 7; },
      [](auto& info) { info.subresourceRange.baseArrayLayer = 1; },
      [](auto& info) { info.subresourceRange.layerCount = 2; },
      [](auto& info) { info.flags = VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT; },
      [](auto& info) { info.pNext = reinterpret_cast<void*>(uintptr_t{1}); },
      [](auto& info) { info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; },
      [](auto& info) { info.subresourceRange.baseMipLevel = 5; info.subresourceRange.levelCount = 2; },
  }};
  VkAllocationCallbacks allocator{};
  for (const size_t resourceIndex : {size_t{0}, size_t{3}, size_t{6}, size_t{10}, size_t{14}}) {
    SCOPED_TRACE(resources[resourceIndex].name);
    for (size_t index = 0; index < mutations.size(); ++index) {
      SCOPED_TRACE(index);
      FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
      FsrVulkanImageAdapter::Scope scope(adapter);
      VkImage image{};
      ASSERT_EQ(create(imageInfo(resources[resourceIndex]), image), VK_SUCCESS);
      auto info = viewInfo(image, resources[resourceIndex].authoredFormat);
      mutations[index](info);
      const auto original = info;
      VkImageView created{};
      ASSERT_EQ(view(info, created, &allocator), VK_SUCCESS);
      ASSERT_NE(created, VK_NULL_HANDLE); // The mocked driver's allocation, never synthetic success.
      ASSERT_EQ(created, fakeHandle<VkImageView>(driver.nextView - 1));
      expectSameViewInfo(driver.views.back().info, viewInfo(image, resources[resourceIndex].shaderFormat));
      expectSameViewInfo(info, original);
      EXPECT_EQ(driver.views.back().allocator, &allocator);
      EXPECT_TRUE(adapter.failed());
      EXPECT_FALSE(adapter.ready());
      destroy(image);
      EXPECT_TRUE(adapter.empty());
      EXPECT_TRUE(adapter.failed());
    }
  }
}

TEST_F(FsrVulkanImageAdapterTest, ImageDriverFailureIsPreservedAndDoesNotRegisterAnOutputHandle) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter::Scope scope(adapter);
  driver.imageResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  const auto sentinel = fakeHandle<VkImage>(0xdead);
  VkImage image = sentinel;
  ASSERT_EQ(create(imageInfo(resources[0]), image), VK_ERROR_OUT_OF_DEVICE_MEMORY);
  EXPECT_EQ(image, sentinel);
  EXPECT_EQ(driver.images.back().info.format, VK_FORMAT_R32_SFLOAT);
  EXPECT_TRUE(adapter.empty());
  ASSERT_TRUE(adapter.failed());
  const std::string firstFailure = adapter.failure();
  driver.imageResult = VK_SUCCESS;
  auto unexpected = imageInfo(resources[0]);
  unexpected.arrayLayers = 3;
  ASSERT_EQ(create(unexpected, image), VK_SUCCESS);
  EXPECT_EQ(firstFailure, adapter.failure());
}

TEST_F(FsrVulkanImageAdapterTest, BothValidAndPlaceholderViewDriverFailuresArePreserved) {
  for (const bool invalidView : {false, true}) {
    SCOPED_TRACE(invalidView);
    FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
    FsrVulkanImageAdapter::Scope scope(adapter);
    VkImage image{};
    ASSERT_EQ(create(imageInfo(resources[0]), image), VK_SUCCESS);
    driver.viewResult = invalidView ? VK_ERROR_FORMAT_NOT_SUPPORTED : VK_ERROR_OUT_OF_HOST_MEMORY;
    auto info = viewInfo(image, invalidView ? VK_FORMAT_R8_UNORM : VK_FORMAT_R16_SFLOAT);
    const auto sentinel = fakeHandle<VkImageView>(0xbeef);
    VkImageView created = sentinel;
    EXPECT_EQ(view(info, created), driver.viewResult);
    EXPECT_EQ(created, sentinel);
    EXPECT_EQ(driver.views.back().info.format, VK_FORMAT_R32_SFLOAT);
    EXPECT_TRUE(adapter.failed());
    EXPECT_FALSE(adapter.ready());
    destroy(image);
    EXPECT_TRUE(adapter.empty());
  }
}

TEST_F(FsrVulkanImageAdapterTest, PhysicalSupportChecksEveryPromotedUsageWithOptimalTiling) {
  ASSERT_TRUE(supported());
  ASSERT_EQ(driver.imageQueries.size(), resources.size() + 1);
  ASSERT_EQ(driver.formatQueries.size(), resources.size() + 1);
  for (size_t index = 0; index < resources.size(); ++index) {
    SCOPED_TRACE(resources[index].name);
    const auto& query = driver.imageQueries[index];
    EXPECT_EQ(query.physical, driver.physical);
    EXPECT_EQ(query.format, resources[index].shaderFormat);
    EXPECT_EQ(query.type, VK_IMAGE_TYPE_2D);
    EXPECT_EQ(query.tiling, VK_IMAGE_TILING_OPTIMAL);
    EXPECT_EQ(query.usage, resources[index].usage);
    EXPECT_EQ(query.flags, 0u);
    EXPECT_EQ(driver.formatQueries[index].first, driver.physical);
    EXPECT_EQ(driver.formatQueries[index].second, resources[index].shaderFormat);
  }
  const auto& output = driver.imageQueries.back();
  EXPECT_EQ(output.physical, driver.physical);
  EXPECT_EQ(output.format, VK_FORMAT_R32G32B32A32_SFLOAT);
  EXPECT_EQ(output.type, VK_IMAGE_TYPE_2D);
  EXPECT_EQ(output.tiling, VK_IMAGE_TILING_OPTIMAL);
  EXPECT_EQ(output.usage, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  EXPECT_EQ(output.flags, 0u);
}

TEST_F(FsrVulkanImageAdapterTest, PhysicalSupportRejectsMissingSamplingStorageLinearOrAttachmentFeatures) {
  const auto all = driver.scalarFeatures;
  for (const auto missing : {VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT}) {
    SCOPED_TRACE(missing);
    driver.scalarFeatures = all & ~missing;
    driver.vectorFeatures = all;
    driver.rgbaFeatures = all;
    EXPECT_FALSE(supported());
    driver.scalarFeatures = all;
    driver.vectorFeatures = all & ~missing;
    EXPECT_FALSE(supported());
    driver.vectorFeatures = all;
    driver.rgbaFeatures = all & ~missing;
    EXPECT_FALSE(supported());
  }
  // Linear-tiling or buffer capabilities do not satisfy optimal-image support.
  driver.otherTilingFeatures = all;
  driver.scalarFeatures = driver.vectorFeatures = driver.rgbaFeatures = 0;
  EXPECT_FALSE(supported());
}

TEST_F(FsrVulkanImageAdapterTest, PhysicalSupportRejectsInsufficientExtentsMipsLayersSamplesOrFormatResult) {
  const auto sufficient = driver.limits;
  using Mutation = void (*)(VkImageFormatProperties&);
  const std::array<Mutation, 8> mutations{{
      [](auto& value) { value.maxExtent.width = 64; },
      [](auto& value) { value.maxExtent.height = 48; },
      [](auto& value) { value.maxExtent.width = 129; },
      [](auto& value) { value.maxExtent.height = 97; },
      [](auto& value) { value.maxExtent.depth = 0; },
      [](auto& value) { value.maxMipLevels = 5; },
      [](auto& value) { value.maxArrayLayers = 0; },
      [](auto& value) { value.sampleCounts = VK_SAMPLE_COUNT_2_BIT; },
  }};
  for (size_t index = 0; index < mutations.size(); ++index) {
    SCOPED_TRACE(index);
    driver.limits = sufficient;
    mutations[index](driver.limits);
    EXPECT_FALSE(supported());
  }
  driver.limits = sufficient;
  driver.propertiesResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
  EXPECT_FALSE(supported());
  driver.propertiesResult = VK_ERROR_OUT_OF_HOST_MEMORY;
  EXPECT_FALSE(supported());
}

TEST_F(FsrVulkanImageAdapterTest, PhysicalSupportRejectsMissingEntryPointsAndDegenerateHalfExtents) {
  for (const auto extent : {VkExtent2D{0, 49}, VkExtent2D{65, 0}, VkExtent2D{1, 49}, VkExtent2D{65, 1}})
    EXPECT_FALSE(supported(extent));
  EXPECT_FALSE(supported(renderExtent, {64, 98}));
  EXPECT_FALSE(supported(renderExtent, {130, 48}));
  EXPECT_FALSE(supported(renderExtent, {0, 0}));
  EXPECT_FALSE(FsrVulkanImageAdapter::supports(VK_NULL_HANDLE, renderExtent, displayExtent,
      MockDriver::formatProperties, MockDriver::imageProperties));
  EXPECT_FALSE(FsrVulkanImageAdapter::supports(driver.physical, renderExtent, displayExtent,
      nullptr, MockDriver::imageProperties));
  EXPECT_FALSE(FsrVulkanImageAdapter::supports(driver.physical, renderExtent, displayExtent,
      MockDriver::formatProperties, nullptr));
  EXPECT_TRUE(driver.imageQueries.empty());
  EXPECT_TRUE(driver.formatQueries.empty());
  EXPECT_TRUE(supported({2, 2}));
}

TEST_F(FsrVulkanImageAdapterTest, PhysicalSupportChecksTheCallerOutputUsageSeparatelyFromInternalImages) {
  driver.outputLimits.maxExtent.width = 129;
  EXPECT_FALSE(supported());
  ASSERT_EQ(driver.imageQueries.size(), 16u);
  EXPECT_EQ(driver.imageQueries.back().format, VK_FORMAT_R32G32B32A32_SFLOAT);
  EXPECT_EQ(driver.imageQueries.back().usage, VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  // Histories have attachment usage. The smaller reactive mask shares output's
  // compute usage, but accepting its render extent cannot prove display limits.
  EXPECT_EQ(driver.imageQueries[10].usage, attachmentUsage);
  EXPECT_EQ(driver.imageQueries[14].usage, computeUsage);
  driver.outputPropertiesResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
  driver.outputLimits = driver.limits;
  driver.imageQueries.clear();
  EXPECT_FALSE(supported());
  ASSERT_EQ(driver.imageQueries.size(), 15u); // The shared mask/output usage is unsupported.
  driver.outputPropertiesResult = VK_SUCCESS;
  driver.outputLimits.maxExtent.width = 129;
  driver.imageQueries.clear();
  EXPECT_FALSE(supported());
  EXPECT_EQ(driver.imageQueries.size(), 16u);
  driver.outputLimits.maxExtent.width = 130;
  driver.outputLimits.maxExtent.height = 97;
  driver.imageQueries.clear();
  EXPECT_FALSE(supported());
  EXPECT_EQ(driver.imageQueries.size(), 16u);
}

TEST_F(FsrVulkanImageAdapterTest, NestedBackendScopesRestoreTheCorrectDeviceAndNextDispatchTable) {
  const auto secondDevice = fakeHandle<VkDevice>(0x101);
  FsrVulkanImageAdapter outer(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter inner(secondDevice, renderExtent, displayExtent, MockDriver::getProcB);
  EXPECT_EQ(FsrVulkanImageAdapter::getDeviceProcAddr(driver.device, "vkCreateImage"), nullptr);
  VkImage first{}, second{}, third{};
  {
    FsrVulkanImageAdapter::Scope outerScope(outer);
    EXPECT_EQ(FsrVulkanImageAdapter::getDeviceProcAddr(driver.device, "vkUnrelated"), MockDriver::sentinelA);
    const auto createOuter = proc<PFN_vkCreateImage>(driver.device, "vkCreateImage");
    const auto info = imageInfo(resources[0]);
    ASSERT_EQ(createOuter(driver.device, &info, nullptr, &first), VK_SUCCESS);
    {
      FsrVulkanImageAdapter::Scope innerScope(inner);
      EXPECT_EQ(FsrVulkanImageAdapter::getDeviceProcAddr(secondDevice, "vkUnrelated"), MockDriver::sentinelB);
      const auto createInner = proc<PFN_vkCreateImage>(secondDevice, "vkCreateImage");
      ASSERT_EQ(createInner(secondDevice, &info, nullptr, &second), VK_SUCCESS);
      EXPECT_EQ(driver.images.back().device, secondDevice);
      EXPECT_FALSE(inner.empty());
      EXPECT_FALSE(inner.failed());
      {
        FsrVulkanImageAdapter::Scope recursiveScope(inner);
        EXPECT_EQ(FsrVulkanImageAdapter::getDeviceProcAddr(secondDevice, "vkUnrelated"), MockDriver::sentinelB);
      }
      EXPECT_EQ(FsrVulkanImageAdapter::getDeviceProcAddr(secondDevice, "vkUnrelated"), MockDriver::sentinelB);
    }
    EXPECT_EQ(FsrVulkanImageAdapter::getDeviceProcAddr(driver.device, "vkUnrelated"), MockDriver::sentinelA);
    ASSERT_EQ(createOuter(driver.device, &info, nullptr, &third), VK_SUCCESS);
    EXPECT_EQ(driver.images.back().device, driver.device);
    EXPECT_EQ(driver.images.back().info.format, VK_FORMAT_R32_SFLOAT);
    EXPECT_FALSE(outer.failed());
  }
  EXPECT_EQ(FsrVulkanImageAdapter::getDeviceProcAddr(driver.device, "vkCreateImage"), nullptr);
  EXPECT_NE(first, second);
  EXPECT_NE(first, third);
  EXPECT_FALSE(outer.empty());
  EXPECT_FALSE(inner.empty());
}

TEST_F(FsrVulkanImageAdapterTest, MismatchedDeviceIsDiagnosedAndItsImageIsNotPromoted) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  FsrVulkanImageAdapter::Scope scope(adapter);
  const auto wrongDevice = fakeHandle<VkDevice>(0xbad);
  const auto createWrong = proc<PFN_vkCreateImage>(wrongDevice, "vkCreateImage");
  const auto info = imageInfo(resources[0]);
  VkImage created{};
  ASSERT_EQ(createWrong(wrongDevice, &info, nullptr, &created), VK_SUCCESS);
  expectSameImageInfo(driver.images.back().info, info);
  EXPECT_EQ(driver.images.back().address, &info);
  EXPECT_EQ(driver.images.back().device, wrongDevice);
  EXPECT_TRUE(adapter.failed());
  EXPECT_TRUE(adapter.empty());
}

TEST_F(FsrVulkanImageAdapterTest, CachedResultCallbacksRejectUnscopedCallsWithoutCallingTheDriver) {
  FsrVulkanImageAdapter adapter(driver.device, renderExtent, displayExtent, MockDriver::getProcA);
  PFN_vkCreateImage createImage{};
  PFN_vkCreateImageView createView{};
  {
    FsrVulkanImageAdapter::Scope scope(adapter);
    createImage = proc<PFN_vkCreateImage>(driver.device, "vkCreateImage");
    createView = proc<PFN_vkCreateImageView>(driver.device, "vkCreateImageView");
  }
  const auto imageSentinel = fakeHandle<VkImage>(0x1234);
  const auto viewSentinel = fakeHandle<VkImageView>(0x5678);
  VkImage image = imageSentinel;
  VkImageView imageView = viewSentinel;
  const auto info = imageInfo(resources[0]);
  const auto infoView = viewInfo(imageSentinel, VK_FORMAT_R16_SFLOAT);
  EXPECT_EQ(createImage(driver.device, &info, nullptr, &image), VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(createView(driver.device, &infoView, nullptr, &imageView), VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(image, imageSentinel);
  EXPECT_EQ(imageView, viewSentinel);
  EXPECT_TRUE(driver.images.empty());
  EXPECT_TRUE(driver.views.empty());
  EXPECT_TRUE(adapter.empty());
}
} // namespace
