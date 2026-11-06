#include "Container/renderer/temporal/FsrUpscaler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <numbers>
#include <utility>
#include <vector>

#if CONTAINER_HAS_FSR_UPSCALER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

namespace container::renderer {

#if CONTAINER_HAS_FSR_UPSCALER
namespace {

bool sameExtent(VkExtent2D a, VkExtent2D b) {
  return a.width == b.width && a.height == b.height;
}

bool sameSettings(const FsrUpscalerSettings& a, const FsrUpscalerSettings& b) {
  return sameExtent(a.renderExtent, b.renderExtent) &&
      sameExtent(a.displayExtent, b.displayExtent) && a.infiniteFar == b.infiniteFar;
}

bool validSettings(const FsrUpscalerSettings& settings, uint32_t maxExtent) {
  const auto render = settings.renderExtent;
  const auto display = settings.displayExtent;
  return render.width >= 2 && render.height >= 2 && render.width <= display.width &&
      render.height <= display.height && display.width <= maxExtent && display.height <= maxExtent;
}

bool isImage(const FsrImage& image, VkFormat format, VkExtent2D extent) {
  return image.image && image.format == format && sameExtent(image.extent, extent);
}

bool validFrame(const FsrUpscalerFrame& frame, const FsrUpscalerSettings& settings) {
  const auto extent = settings.renderExtent;
  const std::array handles{frame.color.image, frame.depth.image, frame.motion.image,
      frame.reactive.image, frame.transparency.image, frame.output.image};
  // The SDK creates independent views for these read and write resources.
  for (auto it = handles.begin(); it != handles.end(); ++it) {
    if (*it && std::find(handles.begin(), it, *it) != it) return false;
  }
  return frame.commands && isImage(frame.color, VK_FORMAT_R16G16B16A16_SFLOAT, extent) &&
      isImage(frame.depth, VK_FORMAT_R32_SFLOAT, extent) &&
      (isImage(frame.motion, VK_FORMAT_R16G16_SFLOAT, extent) ||
       isImage(frame.motion, VK_FORMAT_R32G32_SFLOAT, extent)) &&
      (!frame.reactive.image || isImage(frame.reactive, VK_FORMAT_R8_UNORM, extent)) &&
      (!frame.transparency.image || isImage(frame.transparency, VK_FORMAT_R8_UNORM, extent)) &&
      isImage(frame.output, VK_FORMAT_R16G16B16A16_SFLOAT, settings.displayExtent) &&
      std::isfinite(frame.jitterPixels[0]) && std::abs(frame.jitterPixels[0]) <= 0.5f &&
      std::isfinite(frame.jitterPixels[1]) && std::abs(frame.jitterPixels[1]) <= 0.5f &&
      std::isfinite(frame.cameraNear) && frame.cameraNear > 0.0f &&
      (settings.infiniteFar || (std::isfinite(frame.cameraFar) && frame.cameraFar > frame.cameraNear)) &&
      std::isfinite(frame.verticalFovRadians) && frame.verticalFovRadians > 0.0f &&
      frame.verticalFovRadians < std::numbers::pi_v<float> &&
      std::isfinite(frame.frameMilliseconds) && frame.frameMilliseconds > 0.0f;
}

bool supportsFormats(VkPhysicalDevice physicalDevice) {
  constexpr auto sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  constexpr auto storage = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  constexpr auto linear = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  const auto supports = [&](VkFormat format, VkFormatFeatureFlags needed) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
    return (properties.optimalTilingFeatures & needed) == needed;
  };
  return supports(VK_FORMAT_R16G16B16A16_SFLOAT, sampled | storage | linear) &&
      supports(VK_FORMAT_R16G16B16A16_UNORM, sampled | storage) &&
      supports(VK_FORMAT_R16G16_SFLOAT, sampled | storage) &&
      supports(VK_FORMAT_R8_UNORM, sampled | storage) && supports(VK_FORMAT_R32_SFLOAT, sampled | storage);
}

FfxApiResource ffxImage(const FsrImage& image, bool output = false) {
  if (!image.image) return {};
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = image.format;
  info.extent = {image.extent.width, image.extent.height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.usage = output ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
  return ffxApiGetResourceVK(reinterpret_cast<void*>(image.image),
      ffxApiGetImageResourceDescriptionVK(image.image, info, 0), output
          ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

// The v1.1.4 Vulkan luma-instability pass has two SPIR-V permutations. In
// both, its set 0 binding 7 (rw_luma_history) is the sole variable using the
// affected image type. The SDK host creates that image as RGBA16F. Correct the
// GLSL format declaration in memory, leaving AMD's signed DLL intact.
// https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/issues/161
struct LumaHistoryShader {
  size_t words;
  uint64_t wordFingerprint;
  size_t formatOperand;
};
constexpr LumaHistoryShader lumaHistoryShaders[] = {
    {1838, 0x58fad9aa9e2ee807ull, 667},
    {1846, 0x5f2a2e3fe47034c8ull, 675},
};
thread_local bool* lumaHistoryCorrection{};

VKAPI_ATTR VkResult VKAPI_CALL ffxCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkShaderModule* module) {
  constexpr uint32_t opTypeImage = (9u << 16u) | 25u;
  constexpr uint32_t rgba8 = 4u;
  constexpr uint32_t rgba16f = 2u;
  for (const auto& variant : lumaHistoryShaders) {
    const auto offset = variant.formatOperand;
    if (info->codeSize != variant.words * sizeof(uint32_t) ||
        info->pCode[offset - 8] != opTypeImage || info->pCode[offset] != rgba8) continue;
    uint64_t fingerprint = 0xcbf29ce484222325ull;
    for (size_t word = 0; word < variant.words; ++word)
      fingerprint = (fingerprint ^ info->pCode[word]) * 0x100000001b3ull;
    if (fingerprint != variant.wordFingerprint) continue;
    try {
      std::vector<uint32_t> words(info->pCode, info->pCode + variant.words);
      words[offset] = rgba16f;
      auto corrected = *info;
      corrected.pCode = words.data();
      const auto result = vkCreateShaderModule(device, &corrected, allocator, module);
      if (result == VK_SUCCESS && lumaHistoryCorrection) *lumaHistoryCorrection = true;
      return result;
    } catch (const std::bad_alloc&) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  return vkCreateShaderModule(device, info, allocator, module);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ffxDeviceProcAddr(VkDevice device, const char* name) {
  // The pinned backend asks for the extension alias based on physical-device
  // enumeration. Vulkan 1.4 exposes the equivalent core entry point even when
  // the optional alias was not enabled on the logical device.
  if (std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
    return vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
  if (std::strcmp(name, "vkCreateShaderModule") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(ffxCreateShaderModule);
  return vkGetDeviceProcAddr(device, name);
}

} // namespace

struct FsrUpscaler::Backend {
  HMODULE module{};
  ffxContext context{};
  PfnFfxCreateContext create{};
  PfnFfxDestroyContext destroy{};
  PfnFfxQuery query{};
  PfnFfxDispatch dispatch{};
  ffxCreateBackendVKDesc deviceDesc{};
  ffxCreateContextDescUpscale contextDesc{};
  int32_t phaseCount{};

  ~Backend() {
    if (context && destroy) destroy(&context, nullptr);
    if (module) FreeLibrary(module);
  }

  bool open(const std::filesystem::path& directory) {
    std::error_code error;
    const auto path = std::filesystem::absolute(directory / L"amd_fidelityfx_vk.dll", error);
    if (error) return false;
    module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return false;
    create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
    destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
    query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
    dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
    return create && destroy && query && dispatch;
  }
};
#else
struct FsrUpscaler::Backend {};
#endif

FsrUpscaler::FsrUpscaler(VkPhysicalDevice physicalDevice, VkDevice device,
                         std::filesystem::path runtimeDirectory)
    : physicalDevice_(physicalDevice), device_(device), runtimeDirectory_(std::move(runtimeDirectory)),
      status_(builtWithSdk() ? "FSR 3.1.4 is idle" : "Built without the Vulkan FSR SDK") {}

FsrUpscaler::~FsrUpscaler() = default;

bool FsrUpscaler::builtWithSdk() noexcept {
#if CONTAINER_HAS_FSR_UPSCALER
  return true;
#else
  return false;
#endif
}

bool FsrUpscaler::available() const noexcept {
  return backend_ && !discarded_;
}

bool FsrUpscaler::prepare(const FsrUpscalerSettings& settings) {
#if CONTAINER_HAS_FSR_UPSCALER
  if (available() && sameSettings(settings_, settings)) return true;
  if (recorded_) {
    status_ = "Submit or discard the recorded FSR dispatch before recreating its context";
    return false;
  }
  if (!device_ || !physicalDevice_ || runtimeDirectory_.empty()) {
    status_ = "FSR requires a Vulkan device and a runtime directory";
    return false;
  }
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
  if (!validSettings(settings, properties.limits.maxImageDimension2D)) {
    status_ = "Invalid FSR render or presentation extent";
    return false;
  }
  if (!supportsFormats(physicalDevice_)) {
    status_ = "GPU lacks the required Vulkan FSR image formats";
    return false;
  }
  auto next = std::make_unique<Backend>();
  if (!next->open(runtimeDirectory_)) {
    status_ = "The pinned amd_fidelityfx_vk.dll is unavailable beside the executable";
    return false;
  }
  next->deviceDesc = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
      device_, physicalDevice_, ffxDeviceProcAddr};
  auto& desc = next->contextDesc;
  desc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &next->deviceDesc.header};
  desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
      FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  if (settings.infiniteFar) desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
  desc.maxRenderSize = {settings.renderExtent.width, settings.renderExtent.height};
  desc.maxUpscaleSize = {settings.displayExtent.width, settings.displayExtent.height};
  bool correctedLumaHistory = false;
  auto* previousCorrection = std::exchange(lumaHistoryCorrection, &correctedLumaHistory);
  const auto created = next->create(&next->context, &desc.header, nullptr);
  lumaHistoryCorrection = previousCorrection;
  if (created != FFX_API_RETURN_OK) {
    status_ = "Could not create the Vulkan FSR context (" + std::to_string(created) + ")";
    return false;
  }
  ffxQueryGetProviderVersion version{{FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
  if (next->query(&next->context, &version.header) != FFX_API_RETURN_OK || !version.versionName ||
      std::string_view(version.versionName) != "3.1.4") {
    status_ = "Loaded FidelityFX Vulkan runtime did not supply FSR 3.1.4";
    return false;
  }
  if (!correctedLumaHistory) {
    status_ = "The FSR runtime did not provide a pinned Vulkan luma-history shader";
    return false;
  }
  ffxQueryDescUpscaleGetJitterPhaseCount phase{{FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT},
      settings.renderExtent.width, settings.displayExtent.width, &next->phaseCount};
  if (next->query(&next->context, &phase.header) != FFX_API_RETURN_OK || next->phaseCount < 1) {
    status_ = "Could not query the FSR jitter phase count";
    return false;
  }
  backend_ = std::move(next);
  settings_ = settings;
  submittedFrames_ = 0;
  discarded_ = false;
  status_ = "FSR 3.1.4 is ready";
  return true;
#else
  (void)settings;
  return false;
#endif
}

std::optional<std::array<float, 2>> FsrUpscaler::nextJitterPixels() const {
#if CONTAINER_HAS_FSR_UPSCALER
  if (!available()) return std::nullopt;
  float x{}, y{};
  const auto phase = static_cast<int32_t>(submittedFrames_ % backend_->phaseCount);
  ffxQueryDescUpscaleGetJitterOffset offset{{FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET},
      phase, backend_->phaseCount, &x, &y};
  if (backend_->query(&backend_->context, &offset.header) == FFX_API_RETURN_OK) {
    // Match the SDK sample's projected pixel displacement and dispatch offset.
    return std::array{-x, -y};
  }
#endif
  return std::nullopt;
}

FsrDispatchResult FsrUpscaler::record(const FsrUpscalerFrame& frame) {
#if CONTAINER_HAS_FSR_UPSCALER
  if (!available()) return FsrDispatchResult::Unavailable;
  if (recorded_ || !validFrame(frame, settings_)) return FsrDispatchResult::InvalidInput;
  ffxDispatchDescUpscale desc{{FFX_API_DISPATCH_DESC_TYPE_UPSCALE}};
  desc.commandList = reinterpret_cast<void*>(frame.commands);
  desc.color = ffxImage(frame.color);
  desc.depth = ffxImage(frame.depth);
  desc.motionVectors = ffxImage(frame.motion);
  desc.reactive = ffxImage(frame.reactive);
  desc.transparencyAndComposition = ffxImage(frame.transparency);
  desc.output = ffxImage(frame.output, true);
  desc.jitterOffset = {frame.jitterPixels[0], frame.jitterPixels[1]};
  desc.motionVectorScale = {static_cast<float>(settings_.renderExtent.width),
      static_cast<float>(settings_.renderExtent.height)};
  desc.renderSize = {settings_.renderExtent.width, settings_.renderExtent.height};
  desc.upscaleSize = {settings_.displayExtent.width, settings_.displayExtent.height};
  desc.frameTimeDelta = frame.frameMilliseconds;
  desc.preExposure = 1.0f;
  desc.reset = frame.reset || submittedFrames_ == 0;
  desc.cameraNear = settings_.infiniteFar ? std::numeric_limits<float>::max() : frame.cameraFar;
  desc.cameraFar = frame.cameraNear;
  desc.cameraFovAngleVertical = frame.verticalFovRadians;
  desc.viewSpaceToMetersFactor = 1.0f;
  if (frame.debugView) desc.flags = FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
  const auto result = backend_->dispatch(&backend_->context, &desc.header);
  if (result != FFX_API_RETURN_OK) {
    discarded_ = true;
    status_ = "Vulkan FSR dispatch failed (" + std::to_string(result) + ")";
    return FsrDispatchResult::DiscardCommands;
  }
  recorded_ = true;
  return FsrDispatchResult::Recorded;
#else
  (void)frame;
  return FsrDispatchResult::Unavailable;
#endif
}

void FsrUpscaler::commit() noexcept {
  if (recorded_) ++submittedFrames_;
  recorded_ = false;
}

void FsrUpscaler::discard() noexcept {
  discarded_ = discarded_ || recorded_;
  recorded_ = false;
}

} // namespace container::renderer
