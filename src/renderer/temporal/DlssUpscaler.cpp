#include "Container/renderer/temporal/DlssUpscaler.h"
#include "DlssNgx.h"

#if CONTAINER_HAS_DLSS_UPSCALER
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#endif

#include <utility>

namespace container::renderer {
namespace {
constexpr auto sdkUnavailable = "The NVIDIA DLSS 310.9.16 SDK was not enabled at build time";
} // namespace

struct DlssUpscaler::Backend {
#if CONTAINER_HAS_DLSS_UPSCALER
  ngx::Session session;
  NVSDK_NGX_Handle* feature{};
  std::optional<uint64_t> allocatedBytes;
  bool firstEvaluation{true};

  ~Backend() {
    std::scoped_lock lock(ngx::mutex);
    if (feature) NVSDK_NGX_VULKAN_ReleaseFeature(feature);
  }

  void initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice logicalDevice,
      const std::filesystem::path& runtimeDirectory) {
    session.initialize(ngx::Feature::SuperResolution, instance, physicalDevice, logicalDevice, runtimeDirectory);
    const auto parameters = session.parameters();
    std::scoped_lock lock(ngx::mutex);
    int needsUpdate{};
    if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdate)) &&
        needsUpdate)
      throw std::runtime_error("The NVIDIA graphics driver needs an update for DLSS Super Resolution");
    int supported{};
    ngx::check(parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported), "DLSS availability query");
    if (!supported) throw std::runtime_error("DLSS Super Resolution is unavailable on this NVIDIA adapter");
  }

  void refreshAllocatedBytes() {
    unsigned long long bytes{};
    if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(session.parameters(), &bytes))) allocatedBytes = bytes;
  }

  void releaseFeature() {
    std::scoped_lock lock(ngx::mutex);
    if (feature) {
      ngx::check(NVSDK_NGX_VULKAN_ReleaseFeature(feature), "DLSS feature release");
      feature = nullptr;
    }
  }

  VkExtent2D createFeature(UpscaleMode mode, VkExtent2D display, VkQueue queue, uint32_t queueFamily) {
    const auto quality = ngx::quality(mode);
    VkExtent2D render{}, maximum{}, minimum{};
    float sharpness{};
    std::scoped_lock lock(ngx::mutex);
    ngx::check(NGX_DLSS_GET_OPTIMAL_SETTINGS(session.parameters(), display.width, display.height, quality,
        &render.width, &render.height, &maximum.width, &maximum.height, &minimum.width, &minimum.height, &sharpness),
        "DLSS optimal render extent query");
    if (!render.width || !render.height || render.width > display.width || render.height > display.height ||
        ngx::sameExtent(render, display))
      throw std::runtime_error("NGX did not provide a reduced render extent for the requested DLSS mode");
    ngx::FeatureCommands commands{session.device()};
    const auto command = commands.begin(queueFamily);
    NVSDK_NGX_DLSS_Create_Params info{};
    info.Feature = {render.width, render.height, display.width, display.height, quality};
    info.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    ngx::check(NGX_VULKAN_CREATE_DLSS_EXT1(session.device(), command, 1, 1, &feature, session.parameters(), &info),
        "DLSS Super Resolution feature creation");
    if (!feature) throw std::runtime_error("NGX returned no DLSS Super Resolution feature");
    commands.submit(queue, command);
    firstEvaluation = true;
    refreshAllocatedBytes();
    return render;
  }
#endif
};

DlssUpscaler::DlssUpscaler(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
    VkQueue queue, uint32_t queueFamily, std::filesystem::path runtimeDirectory)
    : instance_(instance), physicalDevice_(physicalDevice), device_(device), queue_(queue), queueFamily_(queueFamily),
      runtimeDirectory_(std::move(runtimeDirectory)), status_(builtWithSdk() ? "native resolution" : sdkUnavailable) {}

DlssUpscaler::~DlssUpscaler() = default;

bool DlssUpscaler::builtWithSdk() noexcept {
#if CONTAINER_HAS_DLSS_UPSCALER
  return true;
#else
  return false;
#endif
}

DlssExtensionRequirements DlssUpscaler::queryInstanceExtensions(const std::filesystem::path& runtimeDirectory) {
#if CONTAINER_HAS_DLSS_UPSCALER
  return ngx::queryInstanceExtensions(ngx::Feature::SuperResolution, runtimeDirectory);
#else
  (void)runtimeDirectory;
  return {.status = sdkUnavailable};
#endif
}

DlssExtensionRequirements DlssUpscaler::queryDeviceExtensions(VkInstance instance, VkPhysicalDevice physicalDevice,
    const std::filesystem::path& runtimeDirectory) {
#if CONTAINER_HAS_DLSS_UPSCALER
  return ngx::queryDeviceExtensions(ngx::Feature::SuperResolution, instance, physicalDevice, runtimeDirectory);
#else
  (void)instance;
  (void)physicalDevice;
  (void)runtimeDirectory;
  return {.status = sdkUnavailable};
#endif
}

bool DlssUpscaler::available() const noexcept {
#if CONTAINER_HAS_DLSS_UPSCALER
  return backend_ && backend_->feature && !discarded_;
#else
  return false;
#endif
}

std::optional<uint64_t> DlssUpscaler::allocatedBytes() const noexcept {
#if CONTAINER_HAS_DLSS_UPSCALER
  if (available()) return backend_->allocatedBytes;
#endif
  return std::nullopt;
}

bool DlssUpscaler::prepare(UpscaleMode mode, VkExtent2D display) {
  if (available() && mode_ == mode && ngx::sameExtent(displayExtent_, display)) return true;
  renderExtent_ = display;
  displayExtent_ = display;
  mode_ = mode;
  recorded_ = false;
  discarded_ = false;
#if CONTAINER_HAS_DLSS_UPSCALER
  try {
    if (mode == UpscaleMode::Native) {
      backend_.reset();
      status_ = "native resolution";
      return false;
    }
    if (backend_) backend_->releaseFeature();
    if (!instance_ || !device_ || !queue_ || !ngx::nvidiaAdapter(physicalDevice_))
      throw std::runtime_error("DLSS Super Resolution requires a supported NVIDIA Vulkan graphics device");
    if (!display.width || !display.height) throw std::invalid_argument("Invalid DLSS display extent");
    if (!backend_) {
      auto candidate = std::make_unique<Backend>();
      candidate->initialize(instance_, physicalDevice_, device_, runtimeDirectory_);
      backend_ = std::move(candidate);
    }
    renderExtent_ = backend_->createFeature(mode, display, queue_, queueFamily_);
    status_ = "DLSS Super Resolution 310.9.16 is ready";
    return true;
  } catch (const std::exception& error) {
    backend_.reset();
    status_ = error.what();
  }
#else
  status_ = mode == UpscaleMode::Native ? "native resolution" : sdkUnavailable;
#endif
  return false;
}

DlssDispatchResult DlssUpscaler::record(const DlssUpscalerFrame& frame) {
#if CONTAINER_HAS_DLSS_UPSCALER
  if (!available()) return DlssDispatchResult::Unavailable;
  if (recorded_ || !frame.commands ||
      !ngx::imageMatches(frame.color, VK_FORMAT_R16G16B16A16_SFLOAT, renderExtent_) ||
      !ngx::imageMatches(frame.depth, VK_FORMAT_R32_SFLOAT, renderExtent_) ||
      !(ngx::imageMatches(frame.motion, VK_FORMAT_R16G16_SFLOAT, renderExtent_) ||
        ngx::imageMatches(frame.motion, VK_FORMAT_R32G32_SFLOAT, renderExtent_)) ||
      !ngx::imageMatches(frame.output, VK_FORMAT_R16G16B16A16_SFLOAT, displayExtent_) ||
      !std::ranges::all_of(frame.jitterPixels, [](float offset) { return std::isfinite(offset) && std::abs(offset) <= 0.5f; }))
    return DlssDispatchResult::InvalidInput;

  auto color = ngx::image(frame.color);
  auto depth = ngx::image(frame.depth);
  auto motion = ngx::image(frame.motion);
  auto output = ngx::image(frame.output, true);
  NVSDK_NGX_VK_DLSS_Eval_Params eval{};
  eval.Feature.pInColor = &color;
  eval.Feature.pInOutput = &output;
  eval.pInDepth = &depth;
  eval.pInMotionVectors = &motion;
  eval.InJitterOffsetX = frame.jitterPixels[0];
  eval.InJitterOffsetY = frame.jitterPixels[1];
  eval.InRenderSubrectDimensions = {renderExtent_.width, renderExtent_.height};
  eval.InMVScaleX = static_cast<float>(renderExtent_.width);
  eval.InMVScaleY = static_cast<float>(renderExtent_.height);
  eval.InPreExposure = 1.0f;
  eval.InExposureScale = 1.0f;
  eval.InFrameTimeDeltaInMsec = std::isfinite(frame.frameMilliseconds) && frame.frameMilliseconds > 0.0f
      ? frame.frameMilliseconds : 0.0f;
  eval.InReset = frame.reset || backend_->firstEvaluation;
  std::scoped_lock lock(ngx::mutex);
  try {
    ngx::check(NGX_VULKAN_EVALUATE_DLSS_EXT(frame.commands, backend_->feature, backend_->session.parameters(), &eval),
        "DLSS Super Resolution evaluation");
    if (backend_->firstEvaluation) backend_->refreshAllocatedBytes();
  } catch (const std::exception& error) {
    status_ = error.what();
    discarded_ = true;
    return DlssDispatchResult::DiscardCommands;
  }
  recorded_ = true;
  return DlssDispatchResult::Recorded;
#else
  (void)frame;
  return DlssDispatchResult::Unavailable;
#endif
}

void DlssUpscaler::commit() noexcept {
#if CONTAINER_HAS_DLSS_UPSCALER
  if (recorded_ && backend_) backend_->firstEvaluation = false;
#endif
  recorded_ = false;
}

void DlssUpscaler::discard() noexcept {
  if (!recorded_) return;
  recorded_ = false;
  discarded_ = true;
}

} // namespace container::renderer
