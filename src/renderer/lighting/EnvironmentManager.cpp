#include "Container/renderer/lighting/EnvironmentManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/TextureImageData.h"
#include "Container/utility/VulkanDevice.h"

#include <tinyexr.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <vector>

namespace container::renderer {

using container::gpu::BlurPushConstants;
using container::gpu::EquirectPushConstants;
using container::gpu::GtaoPushConstants;
using container::gpu::IrradiancePushConstants;
using container::gpu::PrefilterPushConstants;

struct EnvironmentManager::EnvironmentGeneration {
  struct Image {
    VkImage image{};
    VmaAllocation allocation{};
    VkImageView view{};
  };

  EnvironmentManager& owner;
  std::filesystem::path path;
  std::string readyStatus;
  SkyMode skyMode{SkyMode::Hdr};
  AtmosphereSun sun{};
  AtmosphereSettings atmosphere{};
  AtmosphereLut atmosphereLut{};
  SkySettings skySettings{};
  std::shared_ptr<const std::vector<std::byte>> encoded;
  container::gpu::AllocatedBuffer staging{};
  Image equirect{}, cube{}, irradiance{}, prefiltered{};
  VkImageView cubeStorage{}, irradianceStorage{};
  // Match the HDR radiance cube at smooth roughness, retaining an 8x8 final
  // level for roughness1. Storage views, descriptors and publication all use
  // this same count; surface shaders obtain it through prefilteredMipCount().
  static constexpr uint32_t prefilterSize = 512;
  static constexpr uint32_t mipCount = 7;
  std::array<VkImageView, mipCount> prefilteredStorage{};
  VkDescriptorPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  VkQueryPool timestamps{};
  uint32_t timestampBits{};
  float timestampPeriod{};
  bool submitted{};

  ~EnvironmentGeneration() {
    const auto dev = owner.device_->device();
    if (submitted)
      vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    if (fence)
      vkDestroyFence(dev, fence, nullptr);
    if (timestamps)
      vkDestroyQueryPool(dev, timestamps, nullptr);
    if (command)
      vkFreeCommandBuffers(dev, owner.commandPool_, 1, &command);
    if (pool)
      owner.pipelineManager_.destroyDescriptorPool(pool);
    for (auto view : prefilteredStorage)
      if (view) destroyVulkanImageView(dev, view, nullptr);
    for (auto view : {cubeStorage, irradianceStorage})
      if (view) destroyVulkanImageView(dev, view, nullptr);
    const auto allocator = owner.allocationManager_.memoryManager()->allocator();
    for (const auto* image : {&equirect, &cube, &irradiance, &prefiltered}) {
      if (image->view) destroyVulkanImageView(dev, image->view, nullptr);
      if (image->image) vmaDestroyImage(allocator, image->image, image->allocation);
    }
    if (staging.buffer)
      owner.allocationManager_.destroyBuffer(staging);
  }
};

struct EnvironmentManager::PreparedShaderBake::State {
  container::gpu::PipelineManager pipelines;
  EnvironmentManager baked;
  bool brdf{}, environment{};
  const EnvironmentManager* owner;
  VkImageView previousBrdf{}, previousEnvironment{};
  uint64_t previousRevision{};

  explicit State(EnvironmentManager& owner)
      : pipelines(owner.device_->device()),
        baked(owner.device_, owner.allocationManager_, pipelines, owner.commandPool_),
      owner(&owner), previousBrdf(owner.brdfLutView_), previousEnvironment(owner.prefilteredCubeView_),
      previousRevision(owner.publicationRevision_) {}
};

EnvironmentManager::PreparedShaderBake::PreparedShaderBake() = default;
EnvironmentManager::PreparedShaderBake::~PreparedShaderBake() = default;

EnvironmentManager::EnvironmentManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&              pipelineManager,
    VkCommandPool                                  commandPool)
    : device_(std::move(device)),
      allocationManager_(allocationManager),
      pipelineManager_(pipelineManager),
      commandPool_(commandPool) {}

EnvironmentManager::~EnvironmentManager() {
  destroy();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void EnvironmentManager::createResources(const std::filesystem::path& shaderDir) {
  createSamplers();
  createBrdfLut(shaderDir);
  createIblPipelines(shaderDir);
  createPlaceholderCubemaps();
  usingPlaceholderEnvironment_ = true;
  environmentStatus_ = "Environment: placeholder cubemaps active";
  ++publicationRevision_;
  ++environmentRevision_;
}

void EnvironmentManager::createGtaoResources(
    const std::filesystem::path& shaderDir,
    uint32_t fullWidth, uint32_t fullHeight) {
  createGtaoPipelines(shaderDir);
  gtaoFormat_ = selectGtaoStorageFormat();
  createGtaoTextures(fullWidth / 2, fullHeight / 2);
}

void EnvironmentManager::recreateGtaoTextures(uint32_t fullWidth,
                                               uint32_t fullHeight) {
  destroyGtaoTextures();
  if (gtaoPipeline_ != VK_NULL_HANDLE) {
    createGtaoTextures(fullWidth / 2, fullHeight / 2);
  }
}

void EnvironmentManager::destroy() {
  ++publicationRevision_;
  ++environmentRevision_;
  environmentGeneration_.reset();
  hdrDecode_ = {};
  activeHdrBytes_.reset();
  VkDevice dev = device_->device();
  usingPlaceholderEnvironment_ = true;
  environmentStatus_ = "Environment: destroyed";

  destroyGtaoTextures();

  pipelineManager_.destroyPipeline(gtaoPipeline_);
  pipelineManager_.destroyPipeline(gtaoBlurPipeline_);
  pipelineManager_.destroyPipelineLayout(gtaoPipelineLayout_);
  pipelineManager_.destroyPipelineLayout(gtaoBlurPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(gtaoDescriptorPool_);
  pipelineManager_.destroyDescriptorPool(gtaoBlurDescriptorPool_);
  pipelineManager_.destroyDescriptorSetLayout(gtaoSetLayout_);
  pipelineManager_.destroyDescriptorSetLayout(gtaoBlurSetLayout_);
  gtaoSet_ = gtaoBlurSet_ = VK_NULL_HANDLE;

  auto destroyView = [&](VkImageView& v) {
    if (v != VK_NULL_HANDLE) { destroyVulkanImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
  };
  auto destroySampler = [&](VkSampler& s) {
    if (s != VK_NULL_HANDLE) { destroyOwnedSampler(dev, s, nullptr); s = VK_NULL_HANDLE; }
  };
  auto destroyImage = [&](VkImage& img, VmaAllocation& alloc) {
    if (img != VK_NULL_HANDLE && alloc != nullptr) {
      vmaDestroyImage(allocationManager_.memoryManager()->allocator(), img, alloc);
    }
    img = VK_NULL_HANDLE; alloc = nullptr;
  };

  destroyView(brdfLutView_);
  destroyImage(brdfLutImage_, brdfLutAlloc_);
  pipelineManager_.destroyPipeline(brdfLutPipeline_);
  pipelineManager_.destroyPipelineLayout(brdfLutPipelineLayout_);
  pipelineManager_.destroyDescriptorSetLayout(brdfLutSetLayout_);
  destroySampler(brdfLutSampler_);

  destroyView(radianceCubeView_);
  destroyImage(radianceCubeImage_, radianceCubeAlloc_);
  destroyView(irradianceCubeView_);
  destroyImage(irradianceCubeImage_, irradianceCubeAlloc_);

  destroyView(prefilteredCubeView_);
  destroyImage(prefilteredCubeImage_, prefilteredCubeAlloc_);

  pipelineManager_.destroyPipeline(prefilterPipeline_);
  pipelineManager_.destroyPipeline(irradiancePipeline_);
  pipelineManager_.destroyPipeline(equirectToCubemapPipeline_);
  pipelineManager_.destroyPipelineLayout(prefilterPipelineLayout_);
  pipelineManager_.destroyPipelineLayout(irradiancePipelineLayout_);
  pipelineManager_.destroyPipelineLayout(equirectToCubemapPipelineLayout_);
  pipelineManager_.destroyDescriptorSetLayout(iblSetLayout_);

  destroySampler(envSampler_);
  destroySampler(gtaoSampler_);
}

// ---------------------------------------------------------------------------
// BRDF LUT generation (one-time compute dispatch)
// ---------------------------------------------------------------------------

void EnvironmentManager::createBrdfLut(const std::filesystem::path& shaderDir) {
  VkDevice dev = device_->device();
  constexpr uint32_t kLutSize = 512;

  // Create the LUT image.
  {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R16G16_SFLOAT;
    ii.extent      = {kLutSize, kLutSize, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &brdfLutImage_, &brdfLutAlloc_, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create BRDF LUT image");
  }

  // Create image view.
  {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = brdfLutImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R16G16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (createVulkanImageView(dev, &vi, nullptr, &brdfLutView_) != VK_SUCCESS)
      throw std::runtime_error("failed to create BRDF LUT view");
  }

  // Create compute pipeline for BRDF LUT generation.
  VkDescriptorSetLayout lutSetLayout = VK_NULL_HANDLE;
  {
    const VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    lutSetLayout = pipelineManager_.createDescriptorSetLayout({binding}, {0});
    brdfLutSetLayout_ = lutSetLayout;
  }

  struct TemporaryPool {
    container::gpu::PipelineManager& manager;
    VkDescriptorPool pool;
    ~TemporaryPool() { manager.destroyDescriptorPool(pool); }
  };
  TemporaryPool pool{pipelineManager_, pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}}, 1, 0)};
  const auto lutPool = pool.pool;

  VkDescriptorSet lutSet = VK_NULL_HANDLE;
  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = lutPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &lutSetLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &lutSet) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate BRDF LUT descriptor set");
  }

  brdfLutPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {lutSetLayout}, {});

  std::filesystem::path spvPath = shaderDir / "spv_shaders" / "brdf_lut.comp.spv";
  brdfLutPipeline_ = pipelineManager_.createComputePipeline(
      spvPath, brdfLutPipelineLayout_, "brdf_lut");

  allocationManager_.executeStagedCommands([&](VkCommandBuffer cmd) {
  // Transition to GENERAL.
  {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = brdfLutImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // Write descriptor.
  {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView   = brdfLutView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = lutSet;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  }

  // Dispatch.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brdfLutPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brdfLutPipelineLayout_,
                          0, 1, &lutSet, 0, nullptr);
  vkCmdDispatch(cmd, (kLutSize + 15) / 16, (kLutSize + 15) / 16, 1);

  // Transition to SHADER_READ_ONLY.
  {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image         = brdfLutImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  });

}

// ---------------------------------------------------------------------------
// Destroy the current irradiance/prefiltered cubemaps so they can be rebuilt
// ---------------------------------------------------------------------------

void EnvironmentManager::destroyEnvironmentCubemaps() {
  VkDevice dev = device_->device();
  auto destroyView = [&](VkImageView& v) {
    if (v != VK_NULL_HANDLE) { destroyVulkanImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
  };
  auto destroyImage = [&](VkImage& img, VmaAllocation& alloc) {
    if (img != VK_NULL_HANDLE && alloc != nullptr) {
      vmaDestroyImage(allocationManager_.memoryManager()->allocator(), img, alloc);
    }
    img = VK_NULL_HANDLE; alloc = nullptr;
  };
  destroyView(radianceCubeView_);
  destroyImage(radianceCubeImage_, radianceCubeAlloc_);
  destroyView(irradianceCubeView_);
  destroyImage(irradianceCubeImage_, irradianceCubeAlloc_);
  destroyView(prefilteredCubeView_);
  destroyImage(prefilteredCubeImage_, prefilteredCubeAlloc_);
  prefilteredMipCount_ = 1;
}

// ---------------------------------------------------------------------------
// HDR environment loading: equirect EXR -> cubemap -> irradiance + prefilter
// ---------------------------------------------------------------------------

namespace {

AtmosphereSun evaluateAtmosphereSun(const AtmosphereSettings& air) {
  const auto angles = air.localSun.enabled ? solarPosition(air.localSun) :
      SolarAngles{air.sunAzimuthDegrees, air.sunElevationDegrees};
  const double azimuth = glm::radians(angles.azimuthDegrees);
  const double elevation = glm::radians(angles.elevationDegrees);
  const glm::dvec3 direction(std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
                            std::cos(elevation) * std::cos(azimuth));
  AtmosphereSun result{
      .direction = {float(direction.x), float(direction.y), float(direction.z)},
      .lightScene = air.lightScene,
  };
  const double radius = air.planetRadiusKm;
  const double observer = radius + air.observerHeightKm;
  const double top = radius + air.atmosphereHeightKm;
  const double projection = observer * direction.y;
  if (projection < 0.0 && projection * projection >= (observer - radius) * (observer + radius))
    return result;
  const double length = std::sqrt(projection * projection + (top - observer) * (top + observer))
                      - projection;
  glm::dvec3 column(0.0);
  constexpr int samples = 96;
  double previous = 0.0;
  for (int i = 1; i <= samples; ++i) {
    const double u = double(i) / samples;
    const double next = length * u * u;
    const double distance = 0.5 * (previous + next);
    const double altitude = std::max(glm::length(glm::dvec3(0.0, observer, 0.0) +
                                                  direction * distance) - radius, 0.0);
    const glm::dvec3 density(std::exp(-altitude / 8.0), std::exp(-altitude / 1.2),
                             std::max(1.0 - std::abs(altitude - 25.0) / 15.0, 0.0));
    column += density * (next - previous);
    previous = next;
  }
  const glm::dvec3 opticalDepth =
      glm::dvec3(0.005802, 0.013558, 0.033100) * (column.x * air.rayleighScale) +
      glm::dvec3(0.004440) * (column.y * air.mieScale) +
      glm::dvec3(0.000650, 0.001881, 0.000085) * (column.z * air.ozoneScale);
  for (size_t i = 0; i < result.irradiance.size(); ++i)
    result.irradiance[i] = float(air.solarIrradiance * std::exp(-opticalDepth[static_cast<int>(i)]));
  return result;
}

std::string EnvironmentStatusWithPath(std::string_view state,
                                      const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return std::string(state) + ": " + std::string(utf8.begin(), utf8.end());
}

container::material::TextureMipLevel decodeEnvironmentImage(
    const std::filesystem::path& path, std::shared_ptr<const std::vector<std::byte>>& encoded) {
  constexpr uint64_t maxBytes = 256ull * 1024 * 1024;
  constexpr uint32_t maxDimension = 16384;
  auto extension = path.extension().u8string();
  for (auto& c : extension)
    if (c >= u8'A' && c <= u8'Z') c += u8'a' - u8'A';
  if (extension != u8".exr" && extension != u8".hdr")
    throw std::runtime_error("expected an OpenEXR (.exr) or Radiance (.hdr) file");

  if (!encoded) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
      throw std::runtime_error("could not open the HDR file");
    const auto size = file.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(maxBytes))
      throw std::runtime_error("encoded HDR file must contain 1 to 256 MiB of data");
    auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(bytes->size())) ||
        file.peek() != std::char_traits<char>::eof())
      throw std::runtime_error("HDR file changed or could not be read");
    encoded = std::move(bytes);
  }
  const auto& bytes = *encoded;
  if (bytes.empty() || bytes.size() > maxBytes)
    throw std::runtime_error("encoded HDR data must contain 1 to 256 MiB of data");

  if (extension == u8".hdr") {
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    const std::string_view prefix(data, std::min<size_t>(bytes.size(), 10));
    if (!prefix.starts_with("#?RADIANCE") && !prefix.starts_with("#?RGBE"))
      throw std::runtime_error("missing Radiance HDR signature");
    container::material::TextureDecodeOptions options;
    options.allowedFormats = {VK_FORMAT_R32G32B32A32_SFLOAT};
    auto decoded = container::material::DecodeTextureImage(bytes, options);
    return std::move(decoded.mips.front());
  }

  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  EXRVersion version{};
  if (ParseEXRVersionFromMemory(&version, data, bytes.size()) != TINYEXR_SUCCESS ||
      version.multipart || version.non_image)
    throw std::runtime_error("expected a single-part, non-deep OpenEXR image");
  EXRHeader header;
  InitEXRHeader(&header);
  const char* error = nullptr;
  const int headerResult = ParseEXRHeaderFromMemory(&header, &version, data, bytes.size(), &error);
  const auto window = header.data_window;
  const int channels = header.num_channels;
  FreeEXRHeader(&header);
  const auto checkResult = [&](int result) {
    const std::unique_ptr<const char, decltype(&FreeEXRErrorMessage)> message(error, FreeEXRErrorMessage);
    error = nullptr;
    if (result != TINYEXR_SUCCESS)
      throw std::runtime_error(message ? message.get() : "OpenEXR decoder failed");
  };
  checkResult(headerResult);
  const int64_t width = int64_t(window.max_x) - window.min_x + 1;
  const int64_t height = int64_t(window.max_y) - window.min_y + 1;
  if (width <= 0 || height <= 0 || width > maxDimension || height > maxDimension ||
      channels <= 0 || channels > 16 ||
      uint64_t(width) * height * std::max(4, channels) * sizeof(float) > maxBytes)
    throw std::runtime_error("OpenEXR dimensions exceed the HDR image budget");

  float* raw = nullptr;
  int decodedWidth = 0, decodedHeight = 0;
  const int result = LoadEXRFromMemory(&raw, &decodedWidth, &decodedHeight, data, bytes.size(), &error);
  const std::unique_ptr<float, decltype(&std::free)> pixels(raw, std::free);
  checkResult(result);
  if (!pixels || decodedWidth != width || decodedHeight != height)
    throw std::runtime_error("OpenEXR decoder returned inconsistent dimensions");
  const auto pixelBytes = static_cast<size_t>(width * height) * 4 * sizeof(float);
  container::material::TextureMipLevel image{
      static_cast<uint32_t>(width), static_cast<uint32_t>(height), std::vector<std::byte>(pixelBytes)};
  std::memcpy(image.bytes.data(), pixels.get(), pixelBytes);
  return image;
}

container::material::TextureMipLevel decodeValidatedEnvironmentImage(
    const std::filesystem::path& path, std::shared_ptr<const std::vector<std::byte>>& encoded) {
  auto source = decodeEnvironmentImage(path, encoded);
  for (size_t offset = 0; offset < source.bytes.size(); offset += sizeof(float)) {
    float component;
    std::memcpy(&component, source.bytes.data() + offset, sizeof(component));
    if (!std::isfinite(component) || std::abs(component) > 65504.0f)
      throw std::runtime_error("HDR values must be finite and fit the Vulkan half-float cubemap");
  }
  return source;
}



}  // namespace

void EnvironmentManager::createIblPipelines(
    const std::filesystem::path& shaderDir) {
  if (iblSetLayout_ != VK_NULL_HANDLE &&
      equirectToCubemapPipeline_ != VK_NULL_HANDLE &&
      irradiancePipeline_ != VK_NULL_HANDLE &&
      prefilterPipeline_ != VK_NULL_HANDLE) {
    return;
  }

  const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
      {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
  iblSetLayout_ = pipelineManager_.createDescriptorSetLayout(
      {bindings.begin(), bindings.end()}, flags);

  auto buildPipeline = [&](const std::filesystem::path& spv,
                           size_t pushConstantSize,
                           VkPipelineLayout& outLayout) {
    VkPushConstantRange pushConstants{};
    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.size = static_cast<uint32_t>(pushConstantSize);
    outLayout = pipelineManager_.createPipelineLayout(
        {iblSetLayout_}, {pushConstants});

    return pipelineManager_.createComputePipeline(spv, outLayout, spv.filename().string());
  };

  equirectToCubemapPipeline_ = buildPipeline(
      shaderDir / "spv_shaders" / "equirect_to_cubemap.comp.spv",
      sizeof(EquirectPushConstants), equirectToCubemapPipelineLayout_);
  irradiancePipeline_ = buildPipeline(
      shaderDir / "spv_shaders" / "irradiance_convolution.comp.spv",
      sizeof(IrradiancePushConstants), irradiancePipelineLayout_);
  prefilterPipeline_ = buildPipeline(
      shaderDir / "spv_shaders" / "prefilter_specular.comp.spv",
      sizeof(PrefilterPushConstants), prefilterPipelineLayout_);
}

std::unique_ptr<EnvironmentManager::PreparedShaderBake> EnvironmentManager::prepareShaderBake(
    const std::filesystem::path& shaders, bool brdf, bool environment) {
  environment = environment && !usingPlaceholderEnvironment_;
  if (!brdf && !environment)
    return {};
  if (environmentLoading())
    throw std::runtime_error("Finish the pending environment load before reloading bake shaders");
  auto result = std::unique_ptr<PreparedShaderBake>(new PreparedShaderBake());
  result->state_ = std::make_unique<PreparedShaderBake::State>(*this);
  auto& state = *result->state_;
  if (brdf) {
    state.baked.createBrdfLut(shaders);
    state.brdf = true;
  }
  if (environment) {
    state.baked.createSamplers();
    state.baked.createIblPipelines(shaders);
    bool submitted;
    if (activeSkyMode_ == SkyMode::Hdr) {
      if (!activeHdrBytes_)
        throw std::runtime_error("The active HDR source snapshot is unavailable");
      auto bytes = activeHdrBytes_;
      auto image = decodeValidatedEnvironmentImage(activeHdrPath_, bytes);
      submitted = state.baked.submitEnvironment(activeHdrPath_, std::move(image), nullptr, std::move(bytes));
    } else {
      submitted = state.baked.requestSkyEnvironment(activeSkySettings_);
    }
    if (!submitted || !state.baked.publishEnvironment(true, {}))
      throw std::runtime_error(state.baked.environmentStatus());
    state.environment = true;
  }
  return result;
}

bool EnvironmentManager::canPublishShaderBake(const PreparedShaderBake& candidate) const noexcept {
  return candidate.state_ && candidate.state_->owner == this && !environmentLoading() &&
         candidate.state_->previousRevision == publicationRevision_ &&
         candidate.state_->previousBrdf == brdfLutView_ &&
         candidate.state_->previousEnvironment == prefilteredCubeView_;
}

void EnvironmentManager::publishShaderBake(PreparedShaderBake& candidate) noexcept {
  auto& state = *candidate.state_;
  auto& previous = state.baked;
  if (state.brdf) {
    std::swap(brdfLutImage_, previous.brdfLutImage_);
    std::swap(brdfLutAlloc_, previous.brdfLutAlloc_);
    std::swap(brdfLutView_, previous.brdfLutView_);
  }
  if (state.environment) {
    std::swap(radianceCubeImage_, previous.radianceCubeImage_);
    std::swap(radianceCubeAlloc_, previous.radianceCubeAlloc_);
    std::swap(radianceCubeView_, previous.radianceCubeView_);
    std::swap(irradianceCubeImage_, previous.irradianceCubeImage_);
    std::swap(irradianceCubeAlloc_, previous.irradianceCubeAlloc_);
    std::swap(irradianceCubeView_, previous.irradianceCubeView_);
    std::swap(prefilteredCubeImage_, previous.prefilteredCubeImage_);
    std::swap(prefilteredCubeAlloc_, previous.prefilteredCubeAlloc_);
    std::swap(prefilteredCubeView_, previous.prefilteredCubeView_);
    std::swap(prefilteredMipCount_, previous.prefilteredMipCount_);
    ++environmentRevision_;
  }
  ++publicationRevision_;
}

bool EnvironmentManager::loadHdrEnvironment(const std::filesystem::path& path) {
  try {
    std::shared_ptr<const std::vector<std::byte>> encoded;
    auto image = decodeValidatedEnvironmentImage(path, encoded);
    return submitEnvironment(path, std::move(image), nullptr, std::move(encoded)) &&
           publishEnvironment(true, {});
  } catch (const std::exception& error) {
    environmentStatus_ = EnvironmentStatusWithPath(error.what(), path);
    std::println(stderr, "[HDR] {}", environmentStatus_);
    return false;
  }
}

bool EnvironmentManager::requestHdrEnvironment(const std::filesystem::path& path,
    std::shared_ptr<const std::vector<std::byte>> encoded) {
  if (environmentLoading())
    return false;
  try {
    pendingHdrPath_ = path;
    environmentStatus_ = EnvironmentStatusWithPath("Preparing HDR environment", path);
    hdrDecode_ = std::async(std::launch::async, [path, encoded = std::move(encoded)] {
      HdrSource source;
      source.encoded = encoded;
      source.image = decodeValidatedEnvironmentImage(path, source.encoded);
      if (encoded && source.image.width != 2 * source.image.height)
        throw std::runtime_error("Generated HDR environments must be 2:1 equirectangular images");
      return source;
    });
    return true;
  } catch (const std::exception& error) {
    environmentStatus_ = EnvironmentStatusWithPath(error.what(), path);
    pendingHdrPath_.clear();
    return false;
  }
}

bool EnvironmentManager::loadSkyEnvironment(const SkySettings& settings) {
  return requestSkyEnvironment(settings) && publishEnvironment(true, {});
}

bool EnvironmentManager::requestSkyEnvironment(const SkySettings& settings) {
  if (environmentLoading())
    return false;
  const auto validColor = [](const std::array<float, 3>& color) {
    return std::ranges::all_of(color, [](float channel) {
      return std::isfinite(channel) && channel >= 0.0f && channel <= 1.0f;
    });
  };
  if ((settings.mode != SkyMode::Plain && settings.mode != SkyMode::Gradient &&
       settings.mode != SkyMode::Atmosphere) ||
      !validColor(settings.horizon) || !validColor(settings.zenith) || !validColor(settings.ground) ||
      (settings.mode == SkyMode::Gradient && !validGradient(settings)) ||
      (settings.mode == SkyMode::Atmosphere && !validAtmosphere(settings.atmosphere))) {
    environmentStatus_ = "Sky colors or atmosphere settings are outside the supported range.";
    return false;
  }
  try {
    // Keep the sampled-image descriptor valid in the shared compute layout.
    // The procedural branch writes its radiance directly to the cubemap.
    return submitEnvironment({}, {1, 1, std::vector<std::byte>(4 * sizeof(float))}, &settings);
  } catch (const std::exception& error) {
    environmentStatus_ = error.what();
    return false;
  }
}

bool EnvironmentManager::pollEnvironment(
    bool wait, const std::function<void()>& beforePublish) {
  if (environmentGeneration_)
    return publishEnvironment(wait, beforePublish);
  if (!hdrDecode_.valid() ||
      (!wait && hdrDecode_.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
    return false;
  auto path = std::move(pendingHdrPath_);
  pendingHdrPath_.clear();
  try {
    auto source = hdrDecode_.get();
    if (!submitEnvironment(path, std::move(source.image), nullptr, std::move(source.encoded)))
      return false;
    // Normal frames observe the submitted generation on a later poll.
    return wait && publishEnvironment(true, beforePublish);
  } catch (const std::exception& error) {
    environmentStatus_ = EnvironmentStatusWithPath(error.what(), path);
    std::println(stderr, "[HDR] {}", environmentStatus_);
    return false;
  }
}

bool EnvironmentManager::submitEnvironment(
    const std::filesystem::path& hdrPath, container::material::TextureMipLevel source,
    const SkySettings* sky, std::shared_ptr<const std::vector<std::byte>> encoded) {
  const auto readyStatus = sky ? (sky->mode == SkyMode::Atmosphere ? "Atmosphere active" :
      sky->mode == SkyMode::Plain ? "Plain sky active" : "Gradient sky active")
      : EnvironmentStatusWithPath("Environment HDR loaded and IBL generated", hdrPath);
  auto generation = std::make_unique<EnvironmentGeneration>(
      *this, hdrPath, readyStatus);
  generation->encoded = std::move(encoded);
  if (sky) {
    generation->skyMode = sky->mode;
    generation->skySettings = *sky;
  }
  if (sky && sky->mode == SkyMode::Atmosphere) {
    generation->atmosphere = sky->atmosphere;
    generation->sun = evaluateAtmosphereSun(sky->atmosphere);
    if (sky->atmosphere.multipleScattering) {
      generation->atmosphereLut = buildAtmosphereLut(*sky);
      source.width = static_cast<uint32_t>(kAtmosphereLutWidth);
      source.height = static_cast<uint32_t>(kAtmosphereLutHeight);
      source.bytes.resize(sizeof(AtmosphereLut));
      std::memcpy(source.bytes.data(), generation->atmosphereLut.data(), source.bytes.size());
    }
  }
  const auto dev = device_->device();
  const auto allocator = allocationManager_.memoryManager()->allocator();
  const auto failGeneration = [&](std::string_view reason) {
    environmentStatus_ = sky ? std::string(reason) : EnvironmentStatusWithPath(reason, hdrPath);
    return false;
  };

  const VkDeviceSize equirectBytes = source.bytes.size();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device_->physicalDevice(), &properties);
  if (std::max(source.width, source.height) > properties.limits.maxImageDimension2D) {
    environmentStatus_ = EnvironmentStatusWithPath("HDR dimensions exceed the Vulkan device limit", hdrPath);
    return false;
  }
  if (!sky)
    std::println("[HDR] {}", EnvironmentStatusWithPath(
        std::format("Loaded {}x{}", source.width, source.height), hdrPath));

  // ---- 2. Upload equirect to a sampled texture -------------------------
  auto& staging = generation->staging;
  staging =
      allocationManager_.createBuffer(
          equirectBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT);
  if (staging.allocation_info.pMappedData == nullptr)
    return failGeneration("Environment staging buffer mapping failed");
  std::memcpy(staging.allocation_info.pMappedData, source.bytes.data(),
              static_cast<size_t>(equirectBytes));
  if (vmaFlushAllocation(allocator, staging.allocation, 0, equirectBytes) != VK_SUCCESS)
    return failGeneration("Environment staging buffer flush failed");

  auto& [equirectImage, equirectAlloc, equirectView] = generation->equirect;
  {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent      = {source.width,
                      source.height, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator, &ii, &ai, &equirectImage,
                       &equirectAlloc, nullptr) != VK_SUCCESS)
      return failGeneration("Environment equirect image creation failed");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = equirectImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (createVulkanImageView(dev, &vi, nullptr, &equirectView) != VK_SUCCESS)
      return failGeneration("Environment equirect image view creation failed");
  }

  // ---- 3. Create the three cubemap targets ----------------------------
  const uint32_t kEnvSize = sky && sky->mode == SkyMode::Atmosphere ? 128u : 512u;
  const uint32_t     kEnvMips        =
      static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(kEnvSize)))) + 1u;
  constexpr uint32_t kIrradianceSize = 32;
  constexpr uint32_t kPrefilterSize  = EnvironmentGeneration::prefilterSize;
  constexpr uint32_t kPrefilterMips  = EnvironmentGeneration::mipCount;

  auto createCube = [&](uint32_t size, uint32_t mips,
                        VkImageUsageFlags usage,
                        VkImage& image, VmaAllocation& alloc,
                        VkImageView& cubeView) -> bool {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent      = {size, size, 1};
    ii.mipLevels   = mips;
    ii.arrayLayers = 6;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator, &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS)
      return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6};
    return createVulkanImageView(dev, &vi, nullptr, &cubeView) == VK_SUCCESS;
  };

  auto& [envImage, envAlloc, envCubeView] = generation->cube;
  auto& [newIrradianceImage, newIrradianceAlloc, newIrradianceView] = generation->irradiance;
  auto& [newPrefilteredImage, newPrefilteredAlloc, newPrefilteredView] = generation->prefiltered;
  if (!createCube(kEnvSize, kEnvMips,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  envImage, envAlloc, envCubeView) ||
      !createCube(kIrradianceSize, 1,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  newIrradianceImage,
                  newIrradianceAlloc, newIrradianceView) ||
      !createCube(kPrefilterSize, kPrefilterMips,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  newPrefilteredImage,
                  newPrefilteredAlloc, newPrefilteredView))
    return failGeneration("Environment cubemap creation failed");

  auto makeArrayView = [&](VkImage img, uint32_t mip) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
    VkImageView v = VK_NULL_HANDLE;
    createVulkanImageView(dev, &vi, nullptr, &v);
    return v;
  };

  auto& envStorageView = generation->cubeStorage;
  auto& irradianceStorageView = generation->irradianceStorage;
  auto& prefilterStorageViews = generation->prefilteredStorage;
  envStorageView = makeArrayView(envImage, 0);
  irradianceStorageView = makeArrayView(newIrradianceImage, 0);
  for (uint32_t m = 0; m < kPrefilterMips; ++m)
    prefilterStorageViews[m] = makeArrayView(newPrefilteredImage, m);

  auto& pool = generation->pool;
  auto& cmd = generation->command;
  if (!envStorageView || !irradianceStorageView ||
      std::ranges::find(prefilterStorageViews, VK_NULL_HANDLE) != prefilterStorageViews.end())
    return failGeneration("Environment storage image view creation failed");

  // ---- 4. Allocate transient descriptors for the persistent IBL pipelines ---
  // One equirect set, one irradiance set and one set per prefilter mip.
  constexpr uint32_t kTotalSets = 1 + 1 + kPrefilterMips;
  try {
    pool = pipelineManager_.createDescriptorPool(
        {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTotalSets},
         {VK_DESCRIPTOR_TYPE_SAMPLER,        kTotalSets},
         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  kTotalSets}},
        kTotalSets, 0);
  } catch (const std::exception& error) {
    return failGeneration(error.what());
  }

  auto allocSet = [&]() {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &iblSetLayout_;
    VkDescriptorSet s = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(dev, &ai, &s);
    return s;
  };
  VkDescriptorSet equirectSet   = allocSet();
  VkDescriptorSet irradianceSet = allocSet();
  std::array<VkDescriptorSet, kPrefilterMips> prefilterSets{};
  for (auto& s : prefilterSets) s = allocSet();
  if (!equirectSet || !irradianceSet ||
      std::ranges::find(prefilterSets, VK_NULL_HANDLE) != prefilterSets.end())
    return failGeneration("Environment descriptor allocation failed");

  auto writeSet = [&](VkDescriptorSet set, VkImageView sampledView,
                      VkImageLayout sampledLayout, VkImageView storageView) {
    VkDescriptorImageInfo sampledInfo{};
    sampledInfo.imageView   = sampledView;
    sampledInfo.imageLayout = sampledLayout;
    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = envSampler_;
    VkDescriptorImageInfo storageInfo{};
    storageInfo.imageView   = storageView;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    std::array<VkWriteDescriptorSet, 3> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &sampledInfo;
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &samplerInfo;
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = set; w[2].dstBinding = 2; w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[2].pImageInfo = &storageInfo;
    vkUpdateDescriptorSets(dev, 3, w.data(), 0, nullptr);
  };

  writeSet(equirectSet,   equirectView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, envStorageView);
  writeSet(irradianceSet, envCubeView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, irradianceStorageView);
  for (uint32_t m = 0; m < kPrefilterMips; ++m)
    writeSet(prefilterSets[m], envCubeView,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             prefilterStorageViews[m]);

  // ---- 5. Record everything on a single-use command buffer ------------
  {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS)
      return failGeneration("Environment command buffer allocation failed");
  }
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
    return failGeneration("Environment command buffer recording failed");

  if (sky && sky->mode == SkyMode::Atmosphere) {
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device_->physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device_->physicalDevice(), &familyCount, families.data());
    const auto family = device_->queueFamilyIndices().graphicsFamily.value();
    if (family < familyCount && families[family].timestampValidBits > 0) {
      VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      query.queryType = VK_QUERY_TYPE_TIMESTAMP;
      query.queryCount = 2;
      if (vkCreateQueryPool(dev, &query, nullptr, &generation->timestamps) == VK_SUCCESS) {
        generation->timestampBits = families[family].timestampValidBits;
        generation->timestampPeriod = properties.limits.timestampPeriod;
        vkCmdResetQueryPool(cmd, generation->timestamps, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, generation->timestamps, 0);
      }
    }
  }

  auto barrier = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                      VkAccessFlags srcA, VkAccessFlags dstA,
                      VkPipelineStageFlags srcS, VkPipelineStageFlags dstS,
                      uint32_t baseMipLevel, uint32_t levelCount,
                      uint32_t baseArrayLayer, uint32_t layerCount) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL; b.newLayout = newL;
    b.image = img;
    b.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, baseMipLevel, levelCount,
        baseArrayLayer, layerCount};
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
  };

  // Equirect upload: UNDEFINED -> TRANSFER_DST -> copy -> SHADER_READ_ONLY
  barrier(equirectImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          0, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 1, 0, 1);
  {
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {source.width, source.height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, equirectImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  }
  barrier(equirectImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 1, 0, 1);

  // Transition cubemap targets to GENERAL for storage writes.
  barrier(envImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          0, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, kEnvMips, 0, 6);
  barrier(newIrradianceImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          0, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 1, 0, 6);
  barrier(newPrefilteredImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          0, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, kPrefilterMips, 0, 6);

  // -- Equirect -> Env cubemap (dispatch per face) --
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, equirectToCubemapPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, equirectToCubemapPipelineLayout_,
                          0, 1, &equirectSet, 0, nullptr);
  for (uint32_t face = 0; face < 6; ++face) {
    EquirectPushConstants push{face, kEnvSize};
    if (sky) {
      const auto rgb = [](const std::array<float, 3>& color) {
        return glm::vec4(color[0], color[1], color[2], 0.0f);
      };
      push.skyMode = static_cast<uint32_t>(sky->mode);
      push.horizon = rgb(sky->horizon);
      push.zenith = rgb(sky->zenith);
      push.ground = rgb(sky->ground);
      if (sky->mode == SkyMode::Gradient) {
        push.zenith.w = sky->zenithCurve;
        push.ground.w = sky->groundCurve;
        const auto& up = sky->gradientUp;
        push.atmosphereGeometry = {up[0], up[1], up[2], 0.0f};
      }
      if (sky->mode == SkyMode::Atmosphere) {
        const auto& air = sky->atmosphere;
        const auto& direction = generation->sun.direction;
        push.sun = {direction[0], direction[1], direction[2], air.solarIrradiance};
        push.atmosphereGeometry = {air.planetRadiusKm, air.atmosphereHeightKm,
                                   air.observerHeightKm, 0.0f};
        push.atmosphereDensity = {air.rayleighScale, air.mieScale, air.ozoneScale,
                                  air.multipleScattering ? 1.0f : 0.0f};
      }
    }
    vkCmdPushConstants(cmd, equirectToCubemapPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    const uint32_t g = (kEnvSize + 15) / 16;
    vkCmdDispatch(cmd, g, g, 1);
  }

  // Env cube mip chain generation: compute writes mip 0, then blit down the
  // remaining mip levels face-by-face.
  barrier(envImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 1, 0, 6);
  if (kEnvMips > 1) {
    barrier(envImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            1, kEnvMips - 1, 0, 6);
  }

  for (uint32_t mip = 1; mip < kEnvMips; ++mip) {
    const int32_t srcSize = static_cast<int32_t>(std::max(1u, kEnvSize >> (mip - 1)));
    const int32_t dstSize = static_cast<int32_t>(std::max(1u, kEnvSize >> mip));
    for (uint32_t face = 0; face < 6; ++face) {
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, face, 1};
      blit.srcOffsets[1]  = {srcSize, srcSize, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1};
      blit.dstOffsets[1]  = {dstSize, dstSize, 1};
      vkCmdBlitImage(cmd,
                     envImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     envImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, VK_FILTER_LINEAR);
    }

    barrier(envImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            mip, 1, 0, 6);
  }

  // Env cube: full mip chain -> SHADER_READ_ONLY for sampling by filters.
  barrier(envImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, kEnvMips, 0, 6);

  // -- Env -> Irradiance --
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiancePipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiancePipelineLayout_,
                          0, 1, &irradianceSet, 0, nullptr);
  for (uint32_t face = 0; face < 6; ++face) {
    IrradiancePushConstants push{face, kIrradianceSize};
    vkCmdPushConstants(cmd, irradiancePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    const uint32_t g = (kIrradianceSize + 7) / 8;
    vkCmdDispatch(cmd, g, g, 1);
  }

  // -- Env -> Prefiltered specular (per mip, per face) --
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilterPipeline_);
  for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilterPipelineLayout_,
                            0, 1, &prefilterSets[mip], 0, nullptr);
    const uint32_t mipSize = std::max(1u, kPrefilterSize >> mip);
    const float roughness  = (kPrefilterMips <= 1) ? 0.0f :
        static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
    for (uint32_t face = 0; face < 6; ++face) {
      PrefilterPushConstants push{face, mipSize, roughness, kEnvMips};
      vkCmdPushConstants(cmd, prefilterPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                         0, sizeof(push), &push);
      const uint32_t g = (mipSize + 15) / 16;
      vkCmdDispatch(cmd, g, g, 1);
    }
  }

  // Final transitions to SHADER_READ_ONLY for raster lighting and DDGI.
  barrier(newIrradianceImage, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 1, 0, 6);
  barrier(newPrefilteredImage, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, kPrefilterMips, 0, 6);

  if (generation->timestamps)
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, generation->timestamps, 1);
  if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
    return failGeneration("Environment command buffer finalization failed");
  auto buildingStatus = sky ? std::string("Generating sky lighting maps")
                            : EnvironmentStatusWithPath("Generating HDR lighting maps", hdrPath);
  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(dev, &fenceInfo, nullptr, &generation->fence) != VK_SUCCESS)
    return failGeneration("Environment generation fence creation failed");
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  if (vkQueueSubmit(device_->graphicsQueue(), 1, &si, generation->fence) != VK_SUCCESS)
    return failGeneration("Environment queue submission failed");
  generation->submitted = true;
  environmentGeneration_ = std::move(generation);
  environmentStatus_ = std::move(buildingStatus);
  return true;
}

bool EnvironmentManager::publishEnvironment(
    bool wait, const std::function<void()>& beforePublish) {
  auto& pending = *environmentGeneration_;
  const auto dev = device_->device();
  const auto result = wait
      ? vkWaitForFences(dev, 1, &pending.fence, VK_TRUE, UINT64_MAX)
      : vkGetFenceStatus(dev, pending.fence);
  if (result == VK_NOT_READY)
    return false;
  auto generation = std::move(environmentGeneration_);
  if (result != VK_SUCCESS) {
    environmentStatus_ = EnvironmentStatusWithPath("Environment generation fence failed", generation->path);
    return false;
  }
  generation->submitted = false;
  if (generation->timestamps) {
    std::array<uint64_t, 2> ticks{};
    if (vkGetQueryPoolResults(dev, generation->timestamps, 0, 2, sizeof(ticks),
                              ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const auto mask = UINT64_MAX >> (64 - generation->timestampBits);
      const double milliseconds = double((ticks[1] - ticks[0]) & mask) *
                                  generation->timestampPeriod * 1e-6;
      std::println("[Sky] Atmosphere map generation GPU time: {:.3f} ms", milliseconds);
    }
  }
  try {
    if (beforePublish)
      beforePublish();
  } catch (const std::exception& error) {
    environmentStatus_ = EnvironmentStatusWithPath(error.what(), generation->path);
    return false;
  }

  destroyEnvironmentCubemaps();
  auto [radianceImage, radianceAlloc, radianceView] = std::exchange(generation->cube, {});
  radianceCubeImage_=radianceImage;radianceCubeAlloc_=radianceAlloc;radianceCubeView_=radianceView;
  auto [irradianceImage, irradianceAlloc, irradianceView] =
      std::exchange(generation->irradiance, {});
  auto [prefilteredImage, prefilteredAlloc, prefilteredView] =
      std::exchange(generation->prefiltered, {});
  irradianceCubeImage_ = irradianceImage;
  irradianceCubeAlloc_ = irradianceAlloc;
  irradianceCubeView_ = irradianceView;
  prefilteredCubeImage_ = prefilteredImage;
  prefilteredCubeAlloc_ = prefilteredAlloc;
  prefilteredCubeView_ = prefilteredView;
  prefilteredMipCount_ = EnvironmentGeneration::mipCount;
  environmentStatus_ = std::move(generation->readyStatus);
  activeHdrPath_ = std::move(generation->path);
  activeSkyMode_ = generation->skyMode;
  activeSun_ = generation->sun;
  activeAtmosphere_ = generation->atmosphere;
  activeAtmosphereLut_ = generation->atmosphereLut;
  activeSkySettings_ = generation->skySettings;
  activeHdrBytes_ = std::move(generation->encoded);
  usingPlaceholderEnvironment_ = false;
  ++publicationRevision_;
  ++environmentRevision_;
  std::println("[HDR] IBL generation complete");
  return true;
}

// ---------------------------------------------------------------------------
// Placeholder cubemaps (white 1×1 cubemaps — replaced when HDR env loaded)
// ---------------------------------------------------------------------------

void EnvironmentManager::createPlaceholderCubemaps() {
  VkDevice dev = device_->device();
  usingPlaceholderEnvironment_ = true;
  environmentStatus_ = "Environment: placeholder cubemaps active";

  auto createCube = [&](uint32_t size, VkImage& image, VmaAllocation& alloc,
                        VkImageView& view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent      = {size, size, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 6;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_DST so we can vkCmdClearColorImage() to a non-zero gray below.
    // Without a real HDR IBL loaded, sampling undefined memory produces zero
    // and the deferred lighting's ambient term collapses to black in shadow.
    ii.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create placeholder cubemap image");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    if (createVulkanImageView(dev, &vi, nullptr, &view) != VK_SUCCESS)
      throw std::runtime_error("failed to create placeholder cubemap view");
  };

  createCube(1, irradianceCubeImage_, irradianceCubeAlloc_, irradianceCubeView_);
  createCube(1, radianceCubeImage_, radianceCubeAlloc_, radianceCubeView_);
  createCube(1, prefilteredCubeImage_, prefilteredCubeAlloc_, prefilteredCubeView_);
  prefilteredMipCount_ = 1;

  // Transition both to SHADER_READ_ONLY.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = commandPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate placeholder cubemap command buffer");
  }
  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
    throw std::runtime_error("failed to begin placeholder cubemap command buffer");
  }

  auto transitionCube = [&](VkImage img, VkImageLayout oldLayout,
                            VkImageLayout newLayout,
                            VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                            VkPipelineStageFlags srcStage,
                            VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = oldLayout;
    barrier.newLayout     = newLayout;
    barrier.image         = img;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  };

  // Transition to TRANSFER_DST_OPTIMAL so we can clear the cubemaps.
  transitionCube(radianceCubeImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  transitionCube(irradianceCubeImage_,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  transitionCube(prefilteredCubeImage_,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

  // Clear both cubemaps to a mid-gray so the deferred lighting ambient term
  // has something meaningful to sample until a real HDR environment is
  // loaded. Values chosen in linear space; the tonemapper will map them to
  // a neutral ambient.
  VkClearColorValue clearColor{};
  clearColor.float32[0] = 0.25f;
  clearColor.float32[1] = 0.25f;
  clearColor.float32[2] = 0.25f;
  clearColor.float32[3] = 1.0f;
  VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  vkCmdClearColorImage(cmd, radianceCubeImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);
  vkCmdClearColorImage(cmd, irradianceCubeImage_,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);
  vkCmdClearColorImage(cmd, prefilteredCubeImage_,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);

  // Transition to SHADER_READ_ONLY for sampling in the lighting pass.
  transitionCube(radianceCubeImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transitionCube(irradianceCubeImage_,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  transitionCube(prefilteredCubeImage_,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
    throw std::runtime_error("failed to end placeholder cubemap command buffer");
  }
  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmd;
  if (vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
    throw std::runtime_error("failed to submit placeholder cubemap command buffer");
  }
  vkQueueWaitIdle(device_->graphicsQueue());
  vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
}

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

void EnvironmentManager::createSamplers() {
  VkDevice dev = device_->device();

  // BRDF LUT sampler (clamp-to-edge, linear).
  {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod       = 0.0f;
    si.maxLod       = 0.0f;
    si.maxAnisotropy = 1.0f;
    if (createOwnedSampler(dev, &si, nullptr, &brdfLutSampler_) != VK_SUCCESS)
      throw std::runtime_error("failed to create BRDF LUT sampler");
  }

  // Environment cubemap sampler (linear, mip-mapped for prefiltered specular).
  {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod       = 0.0f;
    si.maxLod       = 16.0f;  // supports runtime prefiltered sampling and compute-time env mip sampling
    si.maxAnisotropy = 1.0f;
    if (createOwnedSampler(dev, &si, nullptr, &envSampler_) != VK_SUCCESS)
      throw std::runtime_error("failed to create environment sampler");
  }

  // GTAO sampler (linear, clamp-to-edge).
  {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod       = 0.0f;
    si.maxLod       = 0.0f;
    si.maxAnisotropy = 1.0f;
    if (createOwnedSampler(dev, &si, nullptr, &gtaoSampler_) != VK_SUCCESS)
      throw std::runtime_error("failed to create GTAO sampler");
  }
}

// ---------------------------------------------------------------------------
// GTAO pipelines
// ---------------------------------------------------------------------------

void EnvironmentManager::createGtaoPipelines(const std::filesystem::path& shaderDir) {
  VkDevice dev = device_->device();

  // GTAO compute pipeline.
  {
    const std::array<VkDescriptorSetLayoutBinding, 6> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    gtaoSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size       = sizeof(GtaoPushConstants);

    gtaoPipelineLayout_ = pipelineManager_.createPipelineLayout(
        {gtaoSetLayout_}, {pcRange});

    std::filesystem::path spvPath = shaderDir / "spv_shaders" / "gtao.comp.spv";
    gtaoPipeline_ = pipelineManager_.createComputePipeline(
        spvPath, gtaoPipelineLayout_, "gtao");
  }

  // GTAO blur pipeline.
  {
    const std::array<VkDescriptorSetLayoutBinding, 5> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    gtaoBlurSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size       = sizeof(BlurPushConstants);

    gtaoBlurPipelineLayout_ = pipelineManager_.createPipelineLayout(
        {gtaoBlurSetLayout_}, {pcRange});

    std::filesystem::path spvPath = shaderDir / "spv_shaders" / "gtao_blur.comp.spv";
    gtaoBlurPipeline_ = pipelineManager_.createComputePipeline(
        spvPath, gtaoBlurPipelineLayout_, "gtao_blur");
  }

  // Descriptor pools.
  gtaoDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
       {VK_DESCRIPTOR_TYPE_SAMPLER, 2},
       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
      1, 0);
  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = gtaoDescriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &gtaoSetLayout_;
    if (vkAllocateDescriptorSets(dev, &ai, &gtaoSet_) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate GTAO descriptor set");
  }

  gtaoBlurDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
       {VK_DESCRIPTOR_TYPE_SAMPLER, 2},
       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
      1, 0);
  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = gtaoBlurDescriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &gtaoBlurSetLayout_;
    if (vkAllocateDescriptorSets(dev, &ai, &gtaoBlurSet_) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate GTAO blur descriptor set");
  }
}

// ---------------------------------------------------------------------------
// GTAO textures (half-resolution AO)
// ---------------------------------------------------------------------------

VkFormat EnvironmentManager::selectGtaoStorageFormat() const {
  constexpr std::array<VkFormat, 4> kCandidates{
      VK_FORMAT_R8_UNORM,
      VK_FORMAT_R16_UNORM,
      VK_FORMAT_R16_SFLOAT,
      VK_FORMAT_R32_SFLOAT,
  };
  constexpr VkFormatFeatureFlags kRequiredFeatures =
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

  for (const VkFormat format : kCandidates) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), format,
                                        &props);
    if ((props.optimalTilingFeatures & kRequiredFeatures) ==
        kRequiredFeatures) {
      return format;
    }
  }

  throw std::runtime_error(
      "GPU does not support a sampled storage image format for GTAO");
}

void EnvironmentManager::createGtaoTextures(uint32_t halfWidth,
                                             uint32_t halfHeight) {
  VkDevice dev = device_->device();
  if (gtaoFormat_ == VK_FORMAT_UNDEFINED) {
    gtaoFormat_ = selectGtaoStorageFormat();
  }
  gtaoWidth_  = std::max(halfWidth, 1u);
  gtaoHeight_ = std::max(halfHeight, 1u);

  auto createAoImage = [&](VkImage& image, VmaAllocation& alloc,
                           VkImageView& view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = gtaoFormat_;
    ii.extent      = {gtaoWidth_, gtaoHeight_, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create GTAO texture");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = gtaoFormat_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (createVulkanImageView(dev, &vi, nullptr, &view) != VK_SUCCESS)
      throw std::runtime_error("failed to create GTAO texture view");
  };

  createAoImage(gtaoImage_, gtaoAlloc_, gtaoView_);
  createAoImage(gtaoBlurredImage_, gtaoBlurredAlloc_, gtaoBlurredView_);

  // Transition both to GENERAL for compute write.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = commandPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate GTAO texture command buffer");
  }
  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
    throw std::runtime_error("failed to begin GTAO texture command buffer");
  }

  auto transition = [&](VkImage img) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = img;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  };
  transition(gtaoImage_);
  transition(gtaoBlurredImage_);

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
    throw std::runtime_error("failed to end GTAO texture command buffer");
  }
  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmd;
  if (vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
    throw std::runtime_error("failed to submit GTAO texture command buffer");
  }
  vkQueueWaitIdle(device_->graphicsQueue());
  vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);

  // Write blur set descriptors (inAO = gtaoView_, outAO = gtaoBlurredView_).
  // The GTAO main set is written per-frame in dispatchGtao.
  {
    VkDescriptorImageInfo aoInInfo{};
    aoInInfo.imageView   = gtaoView_;
    aoInInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo aoSampInfo{};
    aoSampInfo.sampler = gtaoSampler_;

    VkDescriptorImageInfo outInfo{};
    outInfo.imageView   = gtaoBlurredView_;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = gtaoBlurSet_; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &aoInInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = gtaoBlurSet_; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &aoSampInfo;

    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = gtaoBlurSet_; w[2].dstBinding = 4;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[2].pImageInfo = &outInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
  }
}

void EnvironmentManager::destroyGtaoTextures() {
  VkDevice dev = device_->device();

  auto destroyView = [&](VkImageView& v) {
    if (v != VK_NULL_HANDLE) { destroyVulkanImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
  };
  auto destroyImage = [&](VkImage& img, VmaAllocation& alloc) {
    if (img != VK_NULL_HANDLE && alloc != nullptr) {
      vmaDestroyImage(allocationManager_.memoryManager()->allocator(), img, alloc);
    }
    img = VK_NULL_HANDLE; alloc = nullptr;
  };

  destroyView(gtaoView_);
  destroyImage(gtaoImage_, gtaoAlloc_);
  destroyView(gtaoBlurredView_);
  destroyImage(gtaoBlurredImage_, gtaoBlurredAlloc_);

  gtaoWidth_ = 0;
  gtaoHeight_ = 0;
}

// ---------------------------------------------------------------------------
// Per-frame GTAO dispatch
// ---------------------------------------------------------------------------

void EnvironmentManager::dispatchGtao(
    VkCommandBuffer cmd,
    uint32_t fullWidth, uint32_t fullHeight,
    VkBuffer cameraBuffer, VkDeviceSize cameraBufferSize,
    VkImageView depthView, VkSampler depthSampler,
    VkImageView normalView, VkSampler normalSampler) const {
  if (gtaoPipeline_ == VK_NULL_HANDLE || !aoEnabled_) return;
  if (gtaoView_ == VK_NULL_HANDLE) return;

  VkDevice dev = device_->device();

  // Update GTAO descriptor set with per-frame resources.
  {
    VkDescriptorBufferInfo camInfo{cameraBuffer, 0, cameraBufferSize};
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    VkDescriptorImageInfo depthSampInfo{};
    depthSampInfo.sampler = depthSampler;
    VkDescriptorImageInfo normalInfo{};
    normalInfo.imageView   = normalView;
    normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo normalSampInfo{};
    normalSampInfo.sampler = normalSampler;
    VkDescriptorImageInfo outInfo{};
    outInfo.imageView   = gtaoView_;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 6> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = gtaoSet_; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &camInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = gtaoSet_; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[1].pImageInfo = &depthInfo;

    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = gtaoSet_; w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[2].pImageInfo = &depthSampInfo;

    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[3].dstSet = gtaoSet_; w[3].dstBinding = 3;
    w[3].descriptorCount = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[3].pImageInfo = &normalInfo;

    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[4].dstSet = gtaoSet_; w[4].dstBinding = 4;
    w[4].descriptorCount = 1;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[4].pImageInfo = &normalSampInfo;

    w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[5].dstSet = gtaoSet_; w[5].dstBinding = 5;
    w[5].descriptorCount = 1;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[5].pImageInfo = &outInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
  }

  GtaoPushConstants pc{};
  pc.aoRadius    = aoRadius_;
  pc.aoIntensity = aoIntensity_;
  pc.sampleCount = aoSampleCount_;
  pc.fullWidth   = fullWidth;
  pc.fullHeight  = fullHeight;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoPipelineLayout_,
                          0, 1, &gtaoSet_, 0, nullptr);
  vkCmdPushConstants(cmd, gtaoPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(GtaoPushConstants), &pc);

  uint32_t dispatchX = (gtaoWidth_ + 7) / 8;
  uint32_t dispatchY = (gtaoHeight_ + 7) / 8;
  vkCmdDispatch(cmd, dispatchX, dispatchY, 1);

  // Barrier: GTAO write → blur read.
  {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = gtaoImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

void EnvironmentManager::dispatchGtaoBlur(VkCommandBuffer cmd,
                                          VkImageView depthView,
                                          VkSampler depthSampler,
                                          float cameraNear,
                                          float cameraFar,
                                          bool orthographicDepth) const {
  if (gtaoBlurPipeline_ == VK_NULL_HANDLE || !aoEnabled_) return;
  if (gtaoBlurredView_ == VK_NULL_HANDLE) return;
  if (depthView == VK_NULL_HANDLE || depthSampler == VK_NULL_HANDLE) return;

  // Update blur depth descriptors (bindings 2,3) at dispatch time.
  {
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    VkDescriptorImageInfo depthSampInfo{};
    depthSampInfo.sampler = depthSampler;

    std::array<VkWriteDescriptorSet, 2> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = gtaoBlurSet_; w[0].dstBinding = 2;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &depthInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = gtaoBlurSet_; w[1].dstBinding = 3;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &depthSampInfo;

    vkUpdateDescriptorSets(device_->device(), static_cast<uint32_t>(w.size()),
                           w.data(), 0, nullptr);
  }

  BlurPushConstants pc{};
  pc.width          = gtaoWidth_;
  pc.height         = gtaoHeight_;
  pc.depthThreshold = 0.08f;
  pc.cameraNear     = cameraNear;
  pc.cameraFar      = cameraFar;
  pc.orthographicDepth = orthographicDepth ? 1u : 0u;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoBlurPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoBlurPipelineLayout_,
                          0, 1, &gtaoBlurSet_, 0, nullptr);
  vkCmdPushConstants(cmd, gtaoBlurPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(BlurPushConstants), &pc);

  uint32_t dispatchX = (gtaoWidth_ + 7) / 8;
  uint32_t dispatchY = (gtaoHeight_ + 7) / 8;
  vkCmdDispatch(cmd, dispatchX, dispatchY, 1);

  // Barrier: blur write → fragment read in lighting.
  {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = gtaoBlurredImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

}  // namespace container::renderer
