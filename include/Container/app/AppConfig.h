#pragma once

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Container/app/GfxCapture.h"
#include "Container/common/CommonVulkan.h"
#include "Container/renderer/raytracing/RayShadowSettings.h"
#include "Container/renderer/temporal/TemporalState.h"

namespace container::app {

// Sentinel used by SceneManager to build the local diagnostic scene from
// multiple generated/sample assets instead of loading a single glTF file.
inline constexpr std::string_view kDefaultSceneModelToken = "__default_test_scene__";
inline constexpr std::array<std::string_view, 3> kDefaultSceneModelRelativePaths = {{
    "models/glTF-Sample-Models/2.0/Triangle/glTF/Triangle.gltf",
    "models/glTF-Sample-Models/2.0/Cube/glTF/Cube.gltf",
    "__procedural_uv_sphere__",
}};

inline constexpr std::string_view kDefaultEnvironmentHdrRelativePath =
    "hdr/citrus_orchard_road_puresky_4k.exr";

// Runtime default for normal application startup. Tests can still exercise the
// diagnostic scene through kDefaultSceneModelToken without changing this path.
inline constexpr std::string_view kDefaultModelRelativePath =
    "models/validation/cornell_box_local_light.gltf";
inline constexpr float kDefaultAuthoredLocalLightEnvironmentIntensity = 0.0f;
inline constexpr float kDefaultAuthoredLocalLightDirectionalIntensity = 0.0f;
// This fixture tests direct-light occlusion. The artistic bounce approximation
// adds untraced radiance even to blocked receivers and would mask shadow errors.
inline constexpr float kDefaultAuthoredLocalLightBounceIntensity = 0.0f;
inline constexpr bool kDefaultAuthoredLocalLightBloomEnabled = false;
inline constexpr uint32_t kDefaultAuthoredLocalLightShadowLayerBudget = 24;

struct AppConfig {
  container::capture::Config gfxrecon{};
  uint32_t windowWidth{800};
  uint32_t windowHeight{600};
  uint32_t maxFramesInFlight{2};
  // Upper bound for the per-object SSBO. This is scene capacity, not a
  // draw-call budget.
  uint32_t maxSceneObjects{4096};
  bool enableValidationLayers{false};
  bool enableRayQueries{true};
  container::renderer::RayShadowSettings rayShadows{};
  bool enableGui{true};
  bool windowVisible{true};
  uint32_t msaaSamples{1};
  uint32_t areaShadowQuality{2};
  uint32_t areaLightingMode{1};
  uint32_t areaLightSampleCount{25};
  bool areaEmitterDebug{false};
  container::temporal::Settings taa{};
  uint32_t taaResetFrame{0};
  std::string renderTechnique{"deferred-raster"};
  std::string displayModeOverride{};
  std::string modelPath{std::string(kDefaultModelRelativePath)};
  float importScale{1.0f};
  std::string bimModelPath{};
  float bimImportScale{1.0f};
  std::string screenshotCapturePath{};
  std::string temporalCaptureSequencePath{};
  uint32_t screenshotWarmupFrames{8};
  uint32_t screenshotCaptureFrame{9};
  float screenshotFixedTimestepSeconds{1.0f / 60.0f};
  bool hasCameraOverride{false};
  std::array<float, 3> cameraPosition{0.0f, 0.0f, 3.0f};
  std::array<float, 3> cameraTarget{0.0f, 0.0f, 0.0f};
  float cameraVerticalFovDegrees{60.0f};
  bool hasManualExposureOverride{false};
  float manualExposure{0.25f};
  bool hasEnvironmentIntensityOverride{false};
  float environmentIntensity{1.0f};
  bool hasDirectionalIntensityOverride{false};
  float directionalIntensity{2.0f};
  bool hasDirectionalDirectionOverride{false};
  std::array<float, 3> directionalDirection{-0.45f, -1.0f, -0.3f};
  bool hasDirectionalColorOverride{false};
  std::array<float, 3> directionalColor{1.0f, 0.96f, 0.9f};
  bool hasBloomEnabledOverride{false};
  bool bloomEnabled{true};
  std::vector<const char*> validationLayers{"VK_LAYER_KHRONOS_validation"};
  std::vector<const char*> deviceExtensions{
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
      VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};
  std::vector<const char*> optionalDeviceExtensions{
      VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME};
};

[[nodiscard]] inline AppConfig DefaultAppConfig() { return AppConfig{}; }

[[nodiscard]] inline bool IsDefaultAuthoredLocalLightScene(
    std::string_view modelPath) {
  std::string normalized(modelPath);
  for (char &ch : normalized) {
    if (ch == '\\') {
      ch = '/';
      continue;
    }
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }

  constexpr std::array<std::string_view, 2> targets{
      kDefaultModelRelativePath,
      "models/validation/cornell_box_yellow_area_light.gltf"};
  for (const auto target : targets) {
    if (normalized == target)
      return true;
    if (normalized.size() > target.size()) {
      const size_t offset = normalized.size() - target.size();
      if (normalized[offset - 1] == '/' &&
          normalized.compare(offset, target.size(), target) == 0)
        return true;
    }
  }
  return false;
}

}  // namespace container::app
