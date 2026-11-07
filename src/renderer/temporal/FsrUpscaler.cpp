#include "Container/renderer/temporal/FsrUpscaler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>

#if CONTAINER_HAS_FSR_UPSCALER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <upscalers/include/ffx_upscale.h>
#include <api/include/vk/ffx_api_vk.h>
#include "FsrVulkanImageAdapter.h"
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

bool validFrame(const FsrUpscalerFrame& frame, const FsrUpscalerSettings& settings, VkFormat outputFormat) {
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
      isImage(frame.output, outputFormat, settings.displayExtent) &&
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

// Reject a same-name module from another runtime directory before AMD's
// loader can reuse it. Keep owned module leases until the context is gone.
bool moduleAtPath(HMODULE module, const std::filesystem::path& expected) {
  std::array<wchar_t, 32768> location{};
  const auto count = GetModuleFileNameW(module, location.data(), static_cast<DWORD>(location.size()));
  if (!count || count >= location.size()) return false;
  std::error_code error;
  return std::filesystem::equivalent(expected, std::filesystem::path(location.data()), error) && !error;
}

HMODULE loadRuntimeModule(const std::filesystem::path& path) {
  if (auto existing = GetModuleHandleW(path.filename().c_str()); existing && !moduleAtPath(existing, path)) return nullptr;
  auto module = LoadLibraryExW(path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (module && !moduleAtPath(module, path)) { FreeLibrary(module); return nullptr; }
  return module;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ffxDeviceProcAddr(VkDevice device, const char* name) {
  // The pinned backend asks for the extension alias based on physical-device
  // enumeration. Vulkan 1.4 exposes the equivalent core entry point even when
  // the optional alias was not enabled on the logical device.
  if (std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
    return vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
  return vkGetDeviceProcAddr(device, name);
}

} // namespace

struct FsrUpscaler::Backend {
  HMODULE effectModule{};
  HMODULE loaderModule{};
  ffxContext context{};
  PfnFfxCreateContext create{};
  PfnFfxDestroyContext destroy{};
  PfnFfxQuery query{};
  PfnFfxDispatch dispatch{};
  ffxCreateBackendVKDesc deviceDesc{};
  ffxCreateContextDescUpscaleVersion apiVersion{};
  ffxOverrideVersion providerOverride{};
  ffxCreateContextDescUpscale contextDesc{};
  std::optional<detail::FsrVulkanImageAdapter> imageAdapter;
  std::string providerVersion;
  int32_t phaseCount{};

  ~Backend() {
    if (destroyContext() != FFX_API_RETURN_OK || (imageAdapter && !imageAdapter->empty())) {
      // Keep module code resident if SDK destruction did not release its
      // objects. Do not report or simulate successful cleanup of a live context.
      std::fprintf(stderr, "FSR SDK did not completely release its Vulkan context; retaining runtime module leases\n");
      return;
    }
    if (loaderModule) FreeLibrary(loaderModule);
    if (effectModule) FreeLibrary(effectModule);
  }

  ffxReturnCode_t createContext() {
    if (imageAdapter) {
      detail::FsrVulkanImageAdapter::Scope scope(*imageAdapter);
      return create(&context, &contextDesc.header, nullptr);
    }
    return create(&context, &contextDesc.header, nullptr);
  }

  ffxReturnCode_t queryContext(ffxQueryDescHeader* desc, bool versions = false) {
    if (imageAdapter) {
      detail::FsrVulkanImageAdapter::Scope scope(*imageAdapter);
      return query(versions ? nullptr : &context, desc);
    }
    return query(versions ? nullptr : &context, desc);
  }

  ffxReturnCode_t dispatchContext(ffxDispatchDescHeader* desc) {
    if (imageAdapter) {
      detail::FsrVulkanImageAdapter::Scope scope(*imageAdapter);
      return dispatch(&context, desc);
    }
    return dispatch(&context, desc);
  }

  ffxReturnCode_t destroyContext() {
    if (!context || !destroy) return FFX_API_RETURN_OK;
    ffxReturnCode_t result;
    if (imageAdapter) {
      detail::FsrVulkanImageAdapter::Scope scope(*imageAdapter);
      result = destroy(&context, nullptr);
    } else {
      result = destroy(&context, nullptr);
    }
    if (result == FFX_API_RETURN_OK) context = nullptr;
    return result;
  }

  bool open(const std::filesystem::path& directory) {
    std::error_code error;
    const auto root = std::filesystem::absolute(directory, error);
    if (error) return false;
    effectModule = loadRuntimeModule(root / L"amd_fidelityfx_upscaler_vk.dll");
    if (!effectModule) return false;
    loaderModule = loadRuntimeModule(root / L"amd_fidelityfx_loader_vk.dll");
    if (!loaderModule) return false;
    create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loaderModule, "ffxCreateContext"));
    destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loaderModule, "ffxDestroyContext"));
    query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(loaderModule, "ffxQuery"));
    dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(loaderModule, "ffxDispatch"));
    return create && destroy && query && dispatch;
  }
};
#else
struct FsrUpscaler::Backend {};
#endif

FsrUpscaler::FsrUpscaler(VkPhysicalDevice physicalDevice, VkDevice device,
                         std::filesystem::path runtimeDirectory)
    : physicalDevice_(physicalDevice), device_(device), runtimeDirectory_(std::move(runtimeDirectory)),
      status_(builtWithSdk() ? "FSR SDK 2.4.0 is idle" : "Built without the Vulkan FSR SDK") {}

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

VkFormat FsrUpscaler::outputFormat() const noexcept {
#if CONTAINER_HAS_FSR_UPSCALER
  if (backend_ && backend_->imageAdapter) return VK_FORMAT_R32G32B32A32_SFLOAT;
#endif
  return VK_FORMAT_R16G16B16A16_SFLOAT;
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
    status_ = "The pinned FSR SDK 2.4.0 loader/upscaler DLLs are unavailable beside the executable";
    return false;
  }
  next->apiVersion = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION}, FFX_UPSCALER_VERSION};
  next->deviceDesc = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &next->apiVersion.header},
      {device_, physicalDevice_, ffxDeviceProcAddr}};
  auto& desc = next->contextDesc;
  desc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &next->deviceDesc.header};
  desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
      FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  if (settings.infiniteFar) desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
  desc.maxRenderSize = {settings.renderExtent.width, settings.renderExtent.height};
  desc.maxUpscaleSize = {settings.displayExtent.width, settings.displayExtent.height};
  const auto created = next->createContext();
  if (created != FFX_API_RETURN_OK) {
    status_ = "Could not create the Vulkan FSR context (" + std::to_string(created) + ")";
    return false;
  }
  ffxQueryGetProviderVersion version{{FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
  if (next->queryContext(&version.header) != FFX_API_RETURN_OK || !version.versionName || !version.versionId) {
    status_ = "Could not query the selected FSR provider";
    return false;
  }
  const auto length = strnlen_s(version.versionName, 128);
  if (!length || length == 128) {
    status_ = "The selected FSR provider returned an invalid version name";
    return false;
  }
  // Copy the actual provider name. SDK 2.4.0 selects analytical 3.1.6 on
  // compatible non-AMD hardware, and a supported newer provider on AMD GPUs.
  next->providerVersion.assign(version.versionName, length);
  if (next->providerVersion == "3.1.6") {
    // Pinned public ffx_provider_fsr3upscale.cpp provider ID. Do not apply the
    // analytical source contract to an ML or driver-supplied provider.
    constexpr uint64_t analyticalId = (uint64_t{0xF5A5CA1E} << 32) | (3u << 22) | (1u << 12) | 6u;
    if (version.versionId != analyticalId) {
      status_ = "The selected FSR 3.1.6 provider is outside the pinned analytical image contract";
      return false;
    }
    std::array<uint64_t, 16> ids{};
    std::array<const char*, 16> names{};
    uint64_t count = ids.size();
    ffxQueryDescGetVersions versions{{FFX_API_QUERY_DESC_TYPE_GET_VERSIONS},
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &next->deviceDesc.deviceContext, &count, ids.data(), names.data()};
    if (next->queryContext(&versions.header, true) != FFX_API_RETURN_OK || count > ids.size()) {
      status_ = "Could not enumerate the pinned analytical FSR provider";
      return false;
    }
    bool enumerated = false;
    for (size_t index = 0; index < count; ++index)
      if (ids[index] == analyticalId && names[index] && strnlen_s(names[index], 128) == 5 &&
          std::strcmp(names[index], "3.1.6") == 0) enumerated = true;
    if (!enumerated) {
      status_ = "The selected analytical FSR provider is absent from version enumeration";
      return false;
    }
    if (!detail::FsrVulkanImageAdapter::supports(physicalDevice_, settings.renderExtent, settings.displayExtent,
            vkGetPhysicalDeviceFormatProperties, vkGetPhysicalDeviceImageFormatProperties)) {
      status_ = "GPU lacks the exact image formats required by the analytical FSR provider";
      return false;
    }
    // The passthrough probe preserves automatic provider choice. Only its exact
    // analytical result is rebuilt; an automatically selected ML provider keeps
    // its original unmodified context and Vulkan dispatch table.
    const auto destroyed = next->destroyContext();
    if (destroyed != FFX_API_RETURN_OK) {
      status_ = "Could not destroy the analytical FSR probe context (" + std::to_string(destroyed) + ")";
      return false;
    }
    next->providerOverride = {{FFX_API_DESC_TYPE_OVERRIDE_VERSION}, analyticalId};
    next->apiVersion.header.pNext = &next->providerOverride.header;
    next->imageAdapter.emplace(device_, settings.renderExtent, settings.displayExtent, ffxDeviceProcAddr);
    next->deviceDesc.deviceContext.vkDeviceProcAddr = detail::FsrVulkanImageAdapter::getDeviceProcAddr;
    const auto rebuilt = next->createContext();
    if (rebuilt != FFX_API_RETURN_OK) {
      status_ = "Could not create the analytical Vulkan FSR context (" + std::to_string(rebuilt) + ")";
      return false;
    }
    ffxQueryGetProviderVersion verified{{FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    if (next->queryContext(&verified.header) != FFX_API_RETURN_OK || verified.versionId != analyticalId ||
        !verified.versionName || strnlen_s(verified.versionName, 128) != 5 ||
        std::strcmp(verified.versionName, "3.1.6") != 0) {
      status_ = "The recreated FSR context did not retain the selected analytical provider";
      return false;
    }
    if (!next->imageAdapter->ready()) {
      status_ = next->imageAdapter->failed() ? next->imageAdapter->failure() :
          "The analytical FSR provider did not create its fifteen expected internal images";
      return false;
    }
  }
  ffxQueryDescUpscaleGetJitterPhaseCount phase{{FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT},
      settings.renderExtent.width, settings.displayExtent.width, &next->phaseCount};
  if (next->queryContext(&phase.header) != FFX_API_RETURN_OK || next->phaseCount < 1) {
    status_ = "Could not query the FSR jitter phase count";
    return false;
  }
  if (next->imageAdapter && next->imageAdapter->failed()) {
    status_ = next->imageAdapter->failure();
    return false;
  }
  backend_ = std::move(next);
  settings_ = settings;
  submittedFrames_ = 0;
  discarded_ = false;
  status_ = "FSR " + backend_->providerVersion + " is ready (SDK 2.4.0)";
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
  if (backend_->queryContext(&offset.header) == FFX_API_RETURN_OK &&
      (!backend_->imageAdapter || !backend_->imageAdapter->failed())) {
    // Match the SDK sample's projected pixel displacement and dispatch offset.
    return std::array{-x, -y};
  }
#endif
  return std::nullopt;
}

FsrDispatchResult FsrUpscaler::record(const FsrUpscalerFrame& frame) {
#if CONTAINER_HAS_FSR_UPSCALER
  if (!available()) return FsrDispatchResult::Unavailable;
  if (recorded_ || !validFrame(frame, settings_, outputFormat())) return FsrDispatchResult::InvalidInput;
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
  const auto result = backend_->dispatchContext(&desc.header);
  if (result != FFX_API_RETURN_OK || (backend_->imageAdapter && backend_->imageAdapter->failed())) {
    discarded_ = true;
    status_ = result != FFX_API_RETURN_OK ? "Vulkan FSR dispatch failed (" + std::to_string(result) + ")" :
        backend_->imageAdapter->failure();
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
