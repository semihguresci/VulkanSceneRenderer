
#include "Container/app/AppConfig.h"
#include "Container/app/Application.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

bool isAuxiliaryRenderModelPath(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos) {
    return false;
  }

  const std::string extension = lowerAscii(path.substr(dot));
  return extension == ".bim" || extension == ".ifc" || extension == ".ifcx" ||
         extension == ".usd" || extension == ".usda" ||
         extension == ".usdc" || extension == ".usdz";
}

std::string_view requireValue(int argc, char** argv, int& index,
                              std::string_view option) {
  if (index + 1 >= argc || argv[index + 1] == nullptr) {
    throw std::runtime_error("missing value for " + std::string(option));
  }
  ++index;
  return argv[index];
}

float parseFloat(std::string_view value, std::string_view option) {
  try {
    size_t consumed = 0;
    const float result = std::stof(std::string(value), &consumed);
    if (consumed != value.size() || !std::isfinite(result))
      throw std::invalid_argument("finite float required");
    return result;
  } catch (...) {
    throw std::runtime_error("invalid float for " + std::string(option) +
                             ": " + std::string(value));
  }
}

uint32_t parseUint(std::string_view value, std::string_view option) {
  try {
    size_t consumed = 0;
    const auto result = std::stoull(std::string(value), &consumed);
    if (value.starts_with("-") || consumed != value.size() ||
        result > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("uint32 required");
    return static_cast<uint32_t>(result);
  } catch (...) {
    throw std::runtime_error("invalid integer for " + std::string(option) +
                             ": " + std::string(value));
  }
}

std::array<float, 3> parseVec3(int argc, char** argv, int& index,
                               std::string_view option) {
  std::array<float, 3> result{};
  for (float& channel : result) {
    channel = parseFloat(requireValue(argc, argv, index, option), option);
  }
  return result;
}

void applyCommandLine(container::app::AppConfig& config, int argc,
                      char** argv) {
  bool positionalModelConsumed = false;
  bool explicitBimModel = false;
  bool explicitBimImportScale = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] ? std::string_view(argv[i]) : "";
    if (arg == "--model") {
      config.modelPath = std::string(requireValue(argc, argv, i, arg));
      positionalModelConsumed = true;
    } else if (arg == "--width") {
      config.windowWidth = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--height") {
      config.windowHeight = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa") {
      config.taa.enabled = true;
    } else if (arg == "--no-taa") {
      config.taa.enabled = false;
    } else if (arg == "--taa-history-weight") {
      config.taa.historyWeight =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-depth-absolute") {
      config.taa.depthAbsoluteTolerance =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-depth-relative") {
      config.taa.depthRelativeTolerance =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-variance-gamma") {
      config.taa.varianceGamma =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-jitter-seed") {
      config.taa.jitterSeed = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-reset-frame") {
      config.taaResetFrame = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--msaa") {
      config.msaaSamples = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--ray-shadows") {
      const auto mode = requireValue(argc, argv, i, arg);
      if (mode == "raster")
        config.rayShadows.mode = container::renderer::RayShadowMode::Raster;
      else if (mode == "hard")
        config.rayShadows.mode = container::renderer::RayShadowMode::Hard;
      else if (mode == "soft")
        config.rayShadows.mode = container::renderer::RayShadowMode::Soft;
      else
        throw std::invalid_argument(
            "Ray shadow mode must be raster, hard or soft");
    } else if (arg == "--ray-shadow-samples") {
      config.rayShadows.areaSamples =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (!config.rayShadows.areaSamples || config.rayShadows.areaSamples > 32u)
        throw std::invalid_argument("Ray shadow samples must be in [1,32]");
    } else if (arg == "--ray-shadow-light-budget") {
      config.rayShadows.localLightBudget =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.rayShadows.localLightBudget > 8u)
        throw std::invalid_argument(
            "Ray shadow local light budget must be in [0,8]");
    } else if (arg == "--no-ray-shadow-denoise") {
      config.rayShadows.denoise = false;
    } else if (arg == "--ray-shadow-debug-layer") {
      config.rayShadows.debugLayer =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.rayShadows.debugLayer > 10u)
        throw std::invalid_argument("Ray shadow debug view must be in [0,10]");
    } else if (arg == "--area-shadow-quality") {
      config.areaShadowQuality =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.areaShadowQuality > 3u)
        throw std::invalid_argument("Area shadow quality must be in [0,3]");
    } else if (arg == "--import-scale") {
      config.importScale = parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--bim-model") {
      config.bimModelPath = std::string(requireValue(argc, argv, i, arg));
      explicitBimModel = true;
    } else if (arg == "--bim-import-scale") {
      config.bimImportScale =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      explicitBimImportScale = true;
    } else if (arg == "--visual-regression-capture" || arg == "--screenshot") {
      config.screenshotCapturePath =
          std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--capture-sequence") {
      config.temporalCaptureSequencePath =
          std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--gfxrecon-session") {
      config.gfxrecon.sessionPath = std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--warmup-frames") {
      config.screenshotWarmupFrames =
          parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--capture-frame") {
      config.screenshotCaptureFrame =
          parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--fixed-dt") {
      config.screenshotFixedTimestepSeconds =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--camera-position") {
      config.cameraPosition = parseVec3(argc, argv, i, arg);
      config.hasCameraOverride = true;
    } else if (arg == "--camera-target") {
      config.cameraTarget = parseVec3(argc, argv, i, arg);
      config.hasCameraOverride = true;
    } else if (arg == "--camera-fov") {
      config.cameraVerticalFovDegrees =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasCameraOverride = true;
    } else if (arg == "--exposure") {
      config.manualExposure = parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasManualExposureOverride = true;
    } else if (arg == "--environment-intensity") {
      config.environmentIntensity =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasEnvironmentIntensityOverride = true;
    } else if (arg == "--directional-intensity") {
      config.directionalIntensity =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasDirectionalIntensityOverride = true;
    } else if (arg == "--directional-direction") {
      config.directionalDirection = parseVec3(argc, argv, i, arg);
      config.hasDirectionalDirectionOverride = true;
    } else if (arg == "--directional-color") {
      config.directionalColor = parseVec3(argc, argv, i, arg);
      config.hasDirectionalColorOverride = true;
    } else if (arg == "--display-mode" || arg == "--render-mode") {
      config.displayModeOverride =
          std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--render-technique") {
      config.renderTechnique = std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--no-bloom") {
      config.bloomEnabled = false;
      config.hasBloomEnabledOverride = true;
    } else if (arg == "--bloom") {
      config.bloomEnabled = true;
      config.hasBloomEnabledOverride = true;
    } else if (arg == "--no-ray-query") {
      config.enableRayQueries = false;
    } else if (arg == "--validation") {
      config.enableValidationLayers = true;
    } else if (arg == "--no-validation") {
      config.enableValidationLayers = false;
    } else if (arg == "--no-ui") {
      config.enableGui = false;
    } else if (arg == "--hidden") {
      config.windowVisible = false;
    } else if (!arg.starts_with("--") && !positionalModelConsumed) {
      config.modelPath = std::string(arg);
      positionalModelConsumed = true;
    } else {
      throw std::runtime_error("unknown argument: " + std::string(arg));
    }
  }

  container::temporal::validateSettings(config.taa, config.msaaSamples);

  if (!explicitBimModel && isAuxiliaryRenderModelPath(config.modelPath)) {
    config.bimModelPath = config.modelPath;
    if (!explicitBimImportScale) {
      config.bimImportScale = config.importScale;
    }
    config.modelPath.clear();
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    auto config = container::app::DefaultAppConfig();
    applyCommandLine(config, argc, argv);
    container::app::Application application{std::move(config)};
    application.run();
  } catch (const std::exception& e) {
    std::println(stderr, "{}", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
