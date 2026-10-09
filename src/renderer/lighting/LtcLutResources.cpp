#include "Container/renderer/lighting/LtcLutResources.h"

#include "Container/renderer/lighting/LtcLutData.h"
#include "Container/renderer/lighting/SubmittedUploadWait.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace container::renderer {
namespace {

struct LutImage {
  container::gpu::VulkanMemoryManager &memory;
  VkImage image{};
  VmaAllocation allocation{};
  VmaAllocationInfo allocationInfo{};
  vk::raii::ImageView view{nullptr};
  uint32_t extent;

  LutImage(const vk::raii::Device &device,
           container::gpu::VulkanMemoryManager &memory, vk::Format format,
           uint32_t extent)
      : memory(memory), extent(extent) {
    vk::ImageCreateInfo create{};
    create.imageType = vk::ImageType::e2D;
    create.format = format;
    create.extent = vk::Extent3D(extent, extent, 1);
    create.mipLevels = create.arrayLayers = 1;
    create.samples = vk::SampleCountFlagBits::e1;
    create.tiling = vk::ImageTiling::eOptimal;
    create.usage = vk::ImageUsageFlagBits::eTransferDst |
                   vk::ImageUsageFlagBits::eSampled;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(memory.allocator(),
                       reinterpret_cast<const VkImageCreateInfo *>(&create),
                       &alloc, &image, &allocation,
                       &allocationInfo) != VK_SUCCESS)
      throw std::runtime_error("LTC image allocation failed");
    try {
      vk::ImageViewCreateInfo info{};
      info.image = image;
      info.viewType = vk::ImageViewType::e2D;
      info.format = format;
      info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
      view = vk::raii::ImageView(device, info);
    } catch (...) {
      vmaDestroyImage(memory.allocator(), image, allocation);
      throw;
    }
  }
  ~LutImage() {
    view.clear();
    vmaDestroyImage(memory.allocator(), image, allocation);
  }
};

bool supportsLutFormat(VkPhysicalDevice physical, vk::Format format) {
  const auto properties =
      vk::PhysicalDevice(physical).getFormatProperties(format);
  constexpr auto required = vk::FormatFeatureFlagBits::eSampledImage |
                            vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
                            vk::FormatFeatureFlagBits::eTransferDst;
  return (properties.optimalTilingFeatures & required) == required;
}

struct Upload {
  LutImage *image;
  std::span<const std::byte> bytes;
};

void upload(const container::gpu::VulkanDevice &device,
            container::gpu::VulkanMemoryManager &memory,
            std::span<const Upload> uploads) {
  const auto &vk = device.raii();
  const uint32_t family = device.queueFamilyIndices().graphicsFamily.value();
  vk::raii::CommandPool pool(vk, vk::CommandPoolCreateInfo(
                                    vk::CommandPoolCreateFlagBits::eTransient,
                                    family));
  auto commands = vk.allocateCommandBuffers(vk::CommandBufferAllocateInfo(
      *pool, vk::CommandBufferLevel::ePrimary, 1));
  auto &command = commands.front();
  std::vector<std::unique_ptr<container::gpu::StagingBuffer>> staging;
  staging.reserve(uploads.size());
  command.begin(vk::CommandBufferBeginInfo(
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  for (const auto &item : uploads) {
    auto buffer = std::make_unique<container::gpu::StagingBuffer>(
        memory, item.bytes.size());
    buffer->upload(item.bytes);
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.image = item.image->image;
    barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::DependencyInfo dependency{};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    command.pipelineBarrier2(dependency);
    vk::BufferImageCopy region{};
    region.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.imageExtent = vk::Extent3D(item.image->extent, item.image->extent, 1);
    command.copyBufferToImage(buffer->buffer().buffer, item.image->image,
                              vk::ImageLayout::eTransferDstOptimal, region);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    command.pipelineBarrier2(dependency);
    staging.push_back(std::move(buffer));
  }
  command.end();
  vk::raii::Fence fence(vk, vk::FenceCreateInfo{});
  vk::raii::Queue queue(vk, family, 0);
  const vk::CommandBufferSubmitInfo commandInfo(*command);
  vk::SubmitInfo2 submit{};
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &commandInfo;
  queue.submit2(submit, *fence);
  detail::waitForSubmittedUpload(
      [&] {
        if (vk.waitForFences(*fence, true,
                             std::numeric_limits<uint64_t>::max()) !=
            vk::Result::eSuccess)
          throw std::runtime_error("LTC upload fence wait failed");
      },
      [&] { queue.waitIdle(); });
}

} // namespace

struct LtcLutResources::Impl {
  std::shared_ptr<container::gpu::VulkanDevice> device;
  container::gpu::VulkanMemoryManager &memory;
  std::unique_ptr<LutImage> matrix, amplitude;
  vk::raii::Sampler sampler{nullptr};
  bool supported{false}, ready{false};
  std::string status{"Sampled area lighting; LTC tables not loaded"};

  Impl(std::shared_ptr<container::gpu::VulkanDevice> device,
        container::gpu::VulkanMemoryManager &memory)
      : device(std::move(device)), memory(memory) {
    supported = supportsLutFormat(this->device->physicalDevice(),
                                  vk::Format::eR32G32B32A32Sfloat);
    const auto format = supported ? vk::Format::eR32G32B32A32Sfloat
                                   : vk::Format::eR8G8B8A8Unorm;
    if (!supportsLutFormat(this->device->physicalDevice(), format))
      throw std::runtime_error("Device cannot sample LTC fallback images");
    matrix = std::make_unique<LutImage>(this->device->raii(), memory, format, 1);
    amplitude =
        std::make_unique<LutImage>(this->device->raii(), memory, format, 1);
    const std::array<float, 4> dummy{1, 0, 0, 1};
    const std::array<uint8_t, 4> dummyUnorm{255, 0, 0, 255};
    const std::span<const std::byte> bytes =
        supported ? std::span<const std::byte>(std::as_bytes(std::span(dummy)))
                  : std::span<const std::byte>(
                        std::as_bytes(std::span(dummyUnorm)));
    const std::array uploads{Upload{matrix.get(), bytes},
                              Upload{amplitude.get(), bytes}};
    upload(*this->device, memory, uploads);
    vk::SamplerCreateInfo create{};
    create.magFilter = create.minFilter = vk::Filter::eLinear;
    create.mipmapMode = vk::SamplerMipmapMode::eNearest;
    create.addressModeU = create.addressModeV = create.addressModeW =
        vk::SamplerAddressMode::eClampToEdge;
    create.minLod = create.maxLod = 0;
    sampler = vk::raii::Sampler(this->device->raii(), create);
  }
};

LtcLutResources::LtcLutResources(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::VulkanMemoryManager &memory)
    : impl_(std::make_unique<Impl>(std::move(device), memory)) {}
LtcLutResources::~LtcLutResources() = default;

void LtcLutResources::load(const std::filesystem::path &assetRoot) {
  auto &p = *impl_;
  if (p.ready)
    return;
  if (!p.supported) {
    p.status = "Sampled area lighting; device lacks linear float32 LTC sampling";
    return;
  }
  std::optional<LtcLutData> matrix, amplitude;
  try {
    matrix = loadLtcLutData(assetRoot / "materials/ltc/matrix.bin");
    amplitude = loadLtcLutData(assetRoot / "materials/ltc/amplitude.bin");
    validateLtcLutPair(*matrix, *amplitude);
  } catch (const std::runtime_error &error) {
    p.status = std::string("Sampled area lighting; ") + error.what();
    return;
  }
  // Only optional asset failures fall back. Vulkan allocation, submission and
  // fence failures must propagate so the application cannot keep drawing after
  // an unsuccessful upload that may still have resources in flight.
  auto matrixImage = std::make_unique<LutImage>(
      p.device->raii(), p.memory, vk::Format::eR32G32B32A32Sfloat,
      LtcLutData::extent);
  auto amplitudeImage = std::make_unique<LutImage>(
      p.device->raii(), p.memory, vk::Format::eR32G32B32A32Sfloat,
      LtcLutData::extent);
  static_assert(sizeof(glm::vec4) == 4 * sizeof(float));
  const std::array uploads{
      Upload{matrixImage.get(), std::as_bytes(std::span(matrix->texels))},
      Upload{amplitudeImage.get(), std::as_bytes(std::span(amplitude->texels))}};
  upload(*p.device, p.memory, uploads);
  p.matrix = std::move(matrixImage);
  p.amplitude = std::move(amplitudeImage);
  p.ready = true;
  p.status = "LTC tables ready (64x64 float32)";
}

bool LtcLutResources::ready() const { return impl_->ready; }
const std::string &LtcLutResources::status() const { return impl_->status; }
uint64_t LtcLutResources::allocatedBytes() const {
  return impl_->matrix->allocationInfo.size +
         impl_->amplitude->allocationInfo.size;
}
VkImageView LtcLutResources::matrixView() const {
  return static_cast<VkImageView>(*impl_->matrix->view);
}
VkImageView LtcLutResources::amplitudeView() const {
  return static_cast<VkImageView>(*impl_->amplitude->view);
}
VkSampler LtcLutResources::sampler() const {
  return static_cast<VkSampler>(*impl_->sampler);
}

} // namespace container::renderer
