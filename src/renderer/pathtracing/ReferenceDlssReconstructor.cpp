#include "Container/renderer/pathtracing/ReferenceDlssReconstructor.h"
#include "../temporal/DlssNgx.h"

#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#endif

#include <utility>

namespace container::renderer {
namespace {
constexpr auto sdkUnavailable = "The NVIDIA DLSS Ray Reconstruction 310.9.16 SDK was not enabled at build time";

#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
bool rgbaFloat(const DlssImage& image, VkExtent2D extent) noexcept {
  return ngx::imageMatches(image, VK_FORMAT_R32G32B32A32_SFLOAT, extent) ||
      ngx::imageMatches(image, VK_FORMAT_R16G16B16A16_SFLOAT, extent);
}

bool usableMatrix(const std::array<float, 16>& values) noexcept {
  return std::ranges::all_of(values, [](float value) { return std::isfinite(value); }) &&
      std::ranges::any_of(values, [](float value) { return value != 0.0f; });
}
#endif
} // namespace

struct ReferenceDlssReconstructor::Backend {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
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
    session.initialize(ngx::Feature::RayReconstruction, instance, physicalDevice, logicalDevice, runtimeDirectory);
    const auto parameters = session.parameters();
    std::scoped_lock lock(ngx::mutex);
    int needsUpdate{};
    if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsUpdate)) &&
        needsUpdate)
      throw std::runtime_error("The NVIDIA graphics driver needs an update for DLSS Ray Reconstruction");
    int supported{};
    ngx::check(parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &supported),
        "DLSS Ray Reconstruction availability query");
    if (!supported) throw std::runtime_error("DLSS Ray Reconstruction is unavailable on this NVIDIA adapter");
  }

  VkExtent2D optimalExtent(UpscaleMode mode, VkExtent2D output) {
    VkExtent2D input{}, maximum{}, minimum{};
    float sharpness{};
    std::scoped_lock lock(ngx::mutex);
    ngx::check(NGX_DLSSD_GET_OPTIMAL_SETTINGS(session.parameters(), output.width, output.height, ngx::quality(mode),
        &input.width, &input.height, &maximum.width, &maximum.height, &minimum.width, &minimum.height, &sharpness),
        "DLSS Ray Reconstruction optimal render extent query");
    if (!input.width || !input.height || input.width > output.width || input.height > output.height ||
        (mode == UpscaleMode::Native && !ngx::sameExtent(input, output)))
      throw std::runtime_error("NGX did not provide a valid render extent for the requested DLSS Ray Reconstruction mode");
    return input;
  }

  void refreshAllocatedBytes() {
    unsigned long long bytes{};
    if (NVSDK_NGX_SUCCEED(NGX_DLSSD_GET_STATS(session.parameters(), &bytes))) allocatedBytes = bytes;
  }

  void createFeature(UpscaleMode mode, VkExtent2D input, VkExtent2D output, VkQueue queue, uint32_t queueFamily) {
    std::scoped_lock lock(ngx::mutex);
    if (feature) {
      ngx::check(NVSDK_NGX_VULKAN_ReleaseFeature(feature), "DLSS Ray Reconstruction feature release");
      feature = nullptr;
    }
    ngx::FeatureCommands commands{session.device()};
    const auto command = commands.begin(queueFamily);
    NVSDK_NGX_DLSSD_Create_Params info{};
    info.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    info.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    info.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
    info.InWidth = input.width;
    info.InHeight = input.height;
    info.InTargetWidth = output.width;
    info.InTargetHeight = output.height;
    info.InPerfQualityValue = ngx::quality(mode);
    info.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    ngx::check(NGX_VULKAN_CREATE_DLSSD_EXT1(session.device(), command, 1, 1, &feature, session.parameters(), &info),
        "DLSS Ray Reconstruction feature creation");
    if (!feature) throw std::runtime_error("NGX returned no DLSS Ray Reconstruction feature");
    commands.submit(queue, command);
    firstEvaluation = true;
    allocatedBytes.reset();
    refreshAllocatedBytes();
  }
#endif
};

ReferenceDlssReconstructor::ReferenceDlssReconstructor(VkInstance instance, VkPhysicalDevice physicalDevice,
    VkDevice device, VkQueue queue, uint32_t queueFamily, std::filesystem::path runtimeDirectory)
    : instance_(instance), physicalDevice_(physicalDevice), device_(device), queue_(queue), queueFamily_(queueFamily),
      runtimeDirectory_(std::move(runtimeDirectory)), status_(builtWithSdk() ? "not initialized" : sdkUnavailable) {}

ReferenceDlssReconstructor::~ReferenceDlssReconstructor() = default;

bool ReferenceDlssReconstructor::builtWithSdk() noexcept {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  return true;
#else
  return false;
#endif
}

DlssExtensionRequirements ReferenceDlssReconstructor::queryInstanceExtensions(const std::filesystem::path& path) {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  return ngx::queryInstanceExtensions(ngx::Feature::RayReconstruction, path);
#else
  (void)path;
  return {.status = sdkUnavailable};
#endif
}

DlssExtensionRequirements ReferenceDlssReconstructor::queryDeviceExtensions(VkInstance instance,
    VkPhysicalDevice physicalDevice, const std::filesystem::path& path) {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  return ngx::queryDeviceExtensions(ngx::Feature::RayReconstruction, instance, physicalDevice, path);
#else
  (void)instance;
  (void)physicalDevice;
  (void)path;
  return {.status = sdkUnavailable};
#endif
}

bool ReferenceDlssReconstructor::available() const noexcept {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  return backend_ && backend_->feature && !discarded_;
#else
  return false;
#endif
}

std::optional<uint64_t> ReferenceDlssReconstructor::allocatedBytes() const noexcept {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  if (available()) return backend_->allocatedBytes;
#endif
  return std::nullopt;
}

#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
ReferenceDlssReconstructor::Backend& ReferenceDlssReconstructor::ensureBackend() {
  if (backend_) return *backend_;
  if (!instance_ || !device_ || !queue_ || !ngx::nvidiaAdapter(physicalDevice_))
    throw std::runtime_error("DLSS Ray Reconstruction requires a supported NVIDIA Vulkan graphics device");
  auto candidate = std::make_unique<Backend>();
  candidate->initialize(instance_, physicalDevice_, device_, runtimeDirectory_);
  backend_ = std::move(candidate);
  return *backend_;
}
#endif

std::optional<VkExtent2D> ReferenceDlssReconstructor::optimalRenderExtent(UpscaleMode mode, VkExtent2D output) {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  try {
    if (!output.width || !output.height) throw std::invalid_argument("Invalid DLSS Ray Reconstruction output extent");
    return ensureBackend().optimalExtent(mode, output);
  } catch (const std::exception& error) {
    status_ = error.what();
  }
#else
  (void)mode;
  (void)output;
#endif
  return std::nullopt;
}

bool ReferenceDlssReconstructor::prepare(UpscaleMode mode, VkExtent2D input, VkExtent2D output) {
  if (available() && mode_ == mode && ngx::sameExtent(inputExtent_, input) &&
      ngx::sameExtent(outputExtent_, output))
    return true;
  mode_ = mode;
  inputExtent_ = input;
  outputExtent_ = output;
  recorded_ = false;
  discarded_ = false;
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  try {
    if (!input.width || !input.height || input.width > output.width || input.height > output.height ||
        (mode == UpscaleMode::Native && !ngx::sameExtent(input, output)))
      throw std::invalid_argument("Invalid DLSS Ray Reconstruction extents");
    ensureBackend().createFeature(mode, input, output, queue_, queueFamily_);
    status_ = "DLSS Ray Reconstruction 310.9.16 is ready";
    return true;
  } catch (const std::exception& error) {
    backend_.reset();
    status_ = error.what();
  }
#endif
  return false;
}

DlssDispatchResult ReferenceDlssReconstructor::record(const ReferenceDlssFrame& frame) {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  if (!available()) return DlssDispatchResult::Unavailable;
  if (recorded_ || !frame.commands || !rgbaFloat(frame.color, inputExtent_) ||
      !ngx::imageMatches(frame.depth, VK_FORMAT_R32_SFLOAT, inputExtent_) ||
      !(ngx::imageMatches(frame.motion, VK_FORMAT_R16G16_SFLOAT, inputExtent_) ||
        ngx::imageMatches(frame.motion, VK_FORMAT_R32G32_SFLOAT, inputExtent_)) ||
      !rgbaFloat(frame.diffuseAlbedo, inputExtent_) || !rgbaFloat(frame.specularAlbedo, inputExtent_) ||
      !rgbaFloat(frame.normalRoughness, inputExtent_) ||
      !(ngx::imageMatches(frame.specularHitDistance, VK_FORMAT_R16_SFLOAT, inputExtent_) ||
        ngx::imageMatches(frame.specularHitDistance, VK_FORMAT_R32_SFLOAT, inputExtent_)) ||
      !rgbaFloat(frame.output, outputExtent_) || frame.output.image == frame.color.image ||
      frame.output.image == frame.diffuseAlbedo.image || frame.output.image == frame.specularAlbedo.image ||
      frame.output.image == frame.normalRoughness.image ||
      !std::ranges::all_of(frame.jitterPixels, [](float offset) { return std::isfinite(offset) && std::abs(offset) <= 0.5f; }) ||
      !usableMatrix(frame.worldToView) || !usableMatrix(frame.viewToClip))
    return DlssDispatchResult::InvalidInput;

  auto color = ngx::image(frame.color);
  auto depth = ngx::image(frame.depth);
  auto motion = ngx::image(frame.motion);
  auto diffuse = ngx::image(frame.diffuseAlbedo);
  auto specular = ngx::image(frame.specularAlbedo);
  auto normal = ngx::image(frame.normalRoughness);
  auto hitDistance = ngx::image(frame.specularHitDistance);
  auto output = ngx::image(frame.output, true);
  auto worldToView = frame.worldToView;
  auto viewToClip = frame.viewToClip;
  NVSDK_NGX_VK_DLSSD_Eval_Params eval{};
  eval.pInColor = &color;
  eval.pInOutput = &output;
  eval.pInDepth = &depth;
  eval.pInMotionVectors = &motion;
  eval.pInDiffuseAlbedo = &diffuse;
  eval.pInSpecularAlbedo = &specular;
  eval.pInNormals = &normal;
  eval.pInRoughness = &normal; // NGX still requires this pointer in packed mode.
  eval.pInSpecularHitDistance = &hitDistance;
  eval.pInWorldToViewMatrix = worldToView.data();
  eval.pInViewToClipMatrix = viewToClip.data();
  eval.InJitterOffsetX = frame.jitterPixels[0];
  eval.InJitterOffsetY = frame.jitterPixels[1];
  eval.InRenderSubrectDimensions = {inputExtent_.width, inputExtent_.height};
  eval.InMVScaleX = eval.InMVScaleY = 1.0f;
  eval.InPreExposure = eval.InExposureScale = 1.0f;
  eval.InFrameTimeDeltaInMsec = std::isfinite(frame.frameMilliseconds) && frame.frameMilliseconds > 0.0f
      ? frame.frameMilliseconds : 0.0f;
  eval.InReset = frame.reset || backend_->firstEvaluation;
  std::scoped_lock lock(ngx::mutex);
  try {
    {
      const ngx::ScopedRrClearSynchronization synchronization(frame.commands, backend_->firstEvaluation);
      ngx::check(NGX_VULKAN_EVALUATE_DLSSD_EXT(frame.commands, backend_->feature, backend_->session.parameters(), &eval),
          "DLSS Ray Reconstruction evaluation");
    }
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

void ReferenceDlssReconstructor::commit() noexcept {
#if CONTAINER_HAS_DLSS_RAY_RECONSTRUCTION
  if (recorded_ && backend_) backend_->firstEvaluation = false;
#endif
  recorded_ = false;
}

void ReferenceDlssReconstructor::discard() noexcept {
  if (!recorded_) return;
  recorded_ = false;
  discarded_ = true;
}

} // namespace container::renderer
