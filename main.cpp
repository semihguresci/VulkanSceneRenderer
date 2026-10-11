
#include "Container/app/AppConfig.h"
#include "Container/app/Application.h"
#include "Container/renderer/lighting/DdgiPreset.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

glm::uvec3 parseCounts(int argc, char** argv, int& index,
                       std::string_view option) {
  glm::uvec3 result{};
  for (int axis = 0; axis < 3; ++axis)
    result[axis] = parseUint(requireValue(argc, argv, index, option), option);
  return result;
}

void printCommandLineHelp() {
  std::println(R"(Usage: VulkanSceneRenderer [model] [options]
  --model PATH                  glTF model or auxiliary BIM/IFC/USD model
  --bim-model PATH              Additional BIM/IFC/USD model
  --import-scale F              Model import scale
  --bim-import-scale F          Auxiliary model import scale
  --width N --height N          Window dimensions
  --render-technique NAME       Render technique
  --display-mode NAME           Display mode (--render-mode is an alias)
  --msaa N                     MSAA sample count
  --taa / --no-taa              Enable/disable temporal antialiasing
  --taa-history-weight F        History blend [0,0.98]
  --taa-depth-absolute F         Absolute depth tolerance [0,1000]
  --taa-depth-relative F         Relative depth tolerance [0,1]
  --taa-variance-gamma F         Variance gamma [0.5,4]
  --taa-jitter-seed N            Jitter sequence seed
  --taa-reset-frame N            Reset history at a capture frame
  --ray-shadows raster|hard|soft Ray shadow mode
  --ray-shadow-samples N         Area samples [1,32]
  --ray-shadow-light-budget N    Local light budget [0,8]
  --ray-shadow-debug-layer N     Debug view [0,10]
  --no-ray-shadow-denoise        Disable ray shadow denoising
  --no-ray-query                Disable optional ray queries
  --area-lighting sampled|ltc    Area light integration mode
  --area-light-samples 9|25|64    Sampled area light count
  --area-shadow-quality N        Raster area shadow quality [0,3]
  --area-emitter-debug           Display area emitters
  --ddgi / --no-ddgi             Enable/disable diffuse probe GI (default off)
  --ddgi-pause                  Pause updates while retaining frame budgets
  --ddgi-preset PATH             Load this scene's saved configuration
                                Explicit DDGI options override the preset
  --ddgi-save-preset PATH        Save configuration after startup scene fitting
  --ddgi-thin-glass / --no-ddgi-thin-glass
  --ddgi-dielectric / --no-ddgi-dielectric
                                Refract rough/solid glass in secondary GI rays
  --ddgi-camera-follow / --no-ddgi-camera-follow
                                Scroll the primary grid with the camera
  --ddgi-async / --no-ddgi-async  Use an available second compute queue
  --ddgi-detail-node NAME        Fit a detail volume to a named scene node/element
  --ddgi-detail-probes N         Maximum new detail probes, >=27 (512)
                                Enable/disable supported thin-glass transmission
  --ddgi-fit-scene              Fit probes and trace settings once to scene bounds
  --ddgi-fit-probes N           Maximum fitted probes, >=27 (2048); requires fit
  --ddgi-debug VIEW             off|irradiance|visibility|classification|relocation
  --ddgi-probe-radius F         Diagnostic sphere radius in world units (0.06)
  --ddgi-origin X Y Z            First world-space probe (-1.5,-0.1,-1.5)
  --ddgi-spacing X Y Z           Positive world-space spacing (0.5,0.5,0.5)
  --ddgi-counts X Y Z            Positive lattice counts (7,5,7)
  --ddgi-rays N                  Rays per updated probe [1,4096] (128)
  --ddgi-probes-per-frame N      Maximum updated probes (128); 0 pauses
  --ddgi-ray-budget N            Maximum rays per frame (16384); 0 pauses
  --ddgi-max-distance F          Ray distance (0,1000000] world units (20)
  --ddgi-hysteresis F            Previous-history weight [0,0.99] (0.95)
  --ddgi-normal-bias F           Normal offset [0,max-distance] (0.03)
  --ddgi-view-bias F             View offset [0,max-distance] (0.01)
  --ddgi-visibility-bias F       Distance-test bias [0,max-distance] (0.05)
  --ddgi-intensity F             Indirect light multiplier [0,10000] (1)
  --ddgi-memory-mb N             Positive probe payload cap in MiB (64)
  --ddgi-surface-memory-mb N     Positive hit-surface payload cap in MiB (128)
                                Fit origin/spacing/counts to each scene's bounds
  --camera-position X Y Z        Override world-space camera position
  --camera-target X Y Z          Override world-space camera target
  --camera-fov F                 Camera vertical field of view in degrees
  --exposure F                   Manual exposure
  --environment-intensity F      Environment intensity
  --directional-intensity F      Directional light intensity
  --directional-direction X Y Z  Directional light direction
  --directional-color R G B      Directional light color
  --bloom / --no-bloom            Enable/disable bloom
  --screenshot PATH             Capture PNG (--visual-regression-capture alias)
  --capture-sequence PATH        Capture temporal sequence
  --warmup-frames N              Capture warmup frames
  --capture-frame N              Screenshot frame
  --fixed-dt F                   Capture timestep in seconds
  --gfxrecon-session PATH        GFXReconstruct session
  --validation / --no-validation Enable/disable Vulkan validation
  --no-ui                       Disable GUI
  --hidden                      Hide window
  --help                        Show this help without starting the renderer)");
}

void applyCommandLine(container::app::AppConfig& config, int argc,
                      char** argv) {
  bool positionalModelConsumed = false;
  bool explicitBimModel = false;
  bool explicitBimImportScale = false;
  bool explicitDdgiFitField = false;
  bool explicitDdgiFitBudget = false;
  bool explicitDdgiDetailBudget = false;
  std::vector<std::function<void(container::renderer::DdgiSettings &)>>
      ddgiOverrides;
  const auto rememberDdgi = [&](auto accessor) {
    const auto value = accessor(config.ddgi);
    ddgiOverrides.emplace_back([accessor, value](auto &settings) {
      accessor(settings) = value;
    });
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] ? std::string_view(argv[i]) : "";
    if (arg == "--model") {
      config.modelPath = std::string(requireValue(argc, argv, i, arg));
      positionalModelConsumed = true;
    } else if (arg == "--width") {
      config.windowWidth = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--height") {
      config.windowHeight = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--ddgi") {
      config.ddgi.enabled = true;
      rememberDdgi([](auto &s) -> auto & { return s.enabled; });
    } else if (arg == "--no-ddgi") {
      config.ddgi.enabled = false;
      rememberDdgi([](auto &s) -> auto & { return s.enabled; });
    } else if (arg == "--ddgi-preset") {
      if (!config.ddgiPresetPath.empty())
        throw std::invalid_argument("--ddgi-preset may be specified only once");
      config.ddgiPresetPath = std::string(requireValue(argc, argv, i, arg));
      if (config.ddgiPresetPath.empty())
        throw std::invalid_argument("--ddgi-preset requires a file path");
    } else if (arg == "--ddgi-save-preset") {
      config.ddgiSavePresetPath = std::string(requireValue(argc, argv, i, arg));
      if (config.ddgiSavePresetPath.empty())
        throw std::invalid_argument("--ddgi-save-preset requires a file path");
    } else if (arg == "--ddgi-thin-glass" || arg == "--no-ddgi-thin-glass") {
      config.ddgi.thinGlassTransmission = arg == "--ddgi-thin-glass";
      rememberDdgi([](auto &s) -> auto & { return s.thinGlassTransmission; });
    } else if (arg == "--ddgi-dielectric" || arg == "--no-ddgi-dielectric") {
      config.ddgi.dielectricTransmission = arg == "--ddgi-dielectric";
      rememberDdgi([](auto &s) -> auto & { return s.dielectricTransmission; });
    } else if (arg == "--ddgi-camera-follow" || arg == "--no-ddgi-camera-follow") {
      config.ddgi.cameraFollow = arg == "--ddgi-camera-follow";
      rememberDdgi([](auto &s) -> auto & { return s.cameraFollow; });
    } else if (arg == "--ddgi-async" || arg == "--no-ddgi-async") {
      config.ddgi.asyncCompute = arg == "--ddgi-async";
      rememberDdgi([](auto &s) -> auto & { return s.asyncCompute; });
    } else if (arg == "--ddgi-detail-node") {
      config.ddgiDetailNode = std::string(requireValue(argc, argv, i, arg));
      if (config.ddgiDetailNode.empty())
        throw std::invalid_argument("--ddgi-detail-node requires a nonempty node name");
    } else if (arg == "--ddgi-detail-probes") {
      explicitDdgiDetailBudget = true;
      config.ddgiDetailProbeBudget = parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.ddgiDetailProbeBudget < 27)
        throw std::invalid_argument("--ddgi-detail-probes must be at least 27");
    } else if (arg == "--ddgi-fit-scene") {
      config.ddgiFitScene = true;
    } else if (arg == "--ddgi-fit-probes") {
      config.ddgiFit.maxProbes = parseUint(requireValue(argc, argv, i, arg), arg);
      explicitDdgiFitBudget = true;
    } else if (arg == "--ddgi-origin" || arg == "--ddgi-spacing") {
      explicitDdgiFitField = true;
      const auto value = parseVec3(argc, argv, i, arg);
      const glm::vec3 vector(value[0], value[1], value[2]);
      if (arg == "--ddgi-origin") {
        config.ddgi.volume.origin = vector;
        rememberDdgi([](auto &s) -> auto & { return s.volume.origin; });
      } else {
        config.ddgi.volume.spacing = vector;
        rememberDdgi([](auto &s) -> auto & { return s.volume.spacing; });
      }
    } else if (arg == "--ddgi-counts") {
      explicitDdgiFitField = true;
      config.ddgi.volume.counts = parseCounts(argc, argv, i, arg);
      rememberDdgi([](auto &s) -> auto & { return s.volume.counts; });
    } else if (arg == "--ddgi-rays") {
      config.ddgi.resources.updates.raysPerProbe =
          parseUint(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.resources.updates.raysPerProbe; });
    } else if (arg == "--ddgi-probes-per-frame") {
      config.ddgi.resources.updates.maxProbesPerFrame =
          parseUint(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.resources.updates.maxProbesPerFrame; });
    } else if (arg == "--ddgi-ray-budget") {
      config.ddgi.resources.updates.maxRaysPerFrame =
          parseUint(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.resources.updates.maxRaysPerFrame; });
    } else if (arg == "--ddgi-max-distance") {
      explicitDdgiFitField = true;
      config.ddgi.maxDistance = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.maxDistance; });
    } else if (arg == "--ddgi-hysteresis") {
      config.ddgi.hysteresis = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.hysteresis; });
    } else if (arg == "--ddgi-normal-bias") {
      explicitDdgiFitField = true;
      config.ddgi.normalBias = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.normalBias; });
    } else if (arg == "--ddgi-view-bias") {
      explicitDdgiFitField = true;
      config.ddgi.viewBias = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.viewBias; });
    } else if (arg == "--ddgi-visibility-bias") {
      explicitDdgiFitField = true;
      config.ddgi.visibilityBias =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.visibilityBias; });
    } else if (arg == "--ddgi-intensity") {
      config.ddgi.intensity = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.intensity; });
    } else if (arg == "--ddgi-pause") {
      config.ddgi.paused = true;
      rememberDdgi([](auto &s) -> auto & { return s.paused; });
    } else if (arg == "--ddgi-debug") {
      const auto view = requireValue(argc, argv, i, arg);
      using container::renderer::DdgiDebugView;
      if (view == "off")
        config.ddgi.debugView = DdgiDebugView::Off;
      else if (view == "irradiance")
        config.ddgi.debugView = DdgiDebugView::Irradiance;
      else if (view == "visibility")
        config.ddgi.debugView = DdgiDebugView::Visibility;
      else if (view == "classification")
        config.ddgi.debugView = DdgiDebugView::Classification;
      else if (view == "relocation")
        config.ddgi.debugView = DdgiDebugView::Relocation;
      else
        throw std::invalid_argument("Unknown DDGI debug view");
      rememberDdgi([](auto &s) -> auto & { return s.debugView; });
    } else if (arg == "--ddgi-probe-radius") {
      explicitDdgiFitField = true;
      config.ddgi.debugProbeRadius =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.debugProbeRadius; });
    } else if (arg == "--ddgi-memory-mb") {
      config.ddgi.payloadBudgetBytes =
          uint64_t(parseUint(requireValue(argc, argv, i, arg), arg)) * 1024 * 1024;
      rememberDdgi([](auto &s) -> auto & { return s.payloadBudgetBytes; });
    } else if (arg == "--ddgi-surface-memory-mb") {
      config.ddgi.surfaceBudgetBytes =
          uint64_t(parseUint(requireValue(argc, argv, i, arg), arg)) * 1024 * 1024;
      rememberDdgi([](auto &s) -> auto & { return s.surfaceBudgetBytes; });
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
    } else if (arg == "--area-emitter-debug") {
      config.areaEmitterDebug = true;
    } else if (arg == "--area-lighting") {
      const auto mode = requireValue(argc, argv, i, arg);
      if (mode == "sampled")
        config.areaLightingMode = 0u;
      else if (mode == "ltc")
        config.areaLightingMode = 1u;
      else
        throw std::invalid_argument("Area lighting must be sampled or ltc");
    } else if (arg == "--area-light-samples") {
      config.areaLightSampleCount =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.areaLightSampleCount != 9u &&
          config.areaLightSampleCount != 25u &&
          config.areaLightSampleCount != 64u)
        throw std::invalid_argument("Area light samples must be 9, 25 or 64");
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

  if (!explicitBimModel && isAuxiliaryRenderModelPath(config.modelPath)) {
    config.bimModelPath = config.modelPath;
    if (!explicitBimImportScale)
      config.bimImportScale = config.importScale;
    config.modelPath.clear();
  }
  if (!config.ddgiPresetPath.empty()) {
    auto preset = container::renderer::loadDdgiPreset(
        container::util::pathFromUtf8(config.ddgiPresetPath));
    const auto sceneIdentity = container::renderer::ddgiSceneIdentity(
        container::util::pathFromUtf8(config.modelPath),
        container::util::pathFromUtf8(config.bimModelPath), config.importScale,
        config.bimImportScale);
    if (preset.sceneIdentity != sceneIdentity)
      throw std::invalid_argument(
          "DDGI preset scene paths/import scales do not match the selected scene");
    for (const auto &overrideSettings : ddgiOverrides)
      overrideSettings(preset.settings);
    config.ddgi = std::move(preset.settings);
  }

  container::temporal::validateSettings(config.taa, config.msaaSamples);
  if (config.ddgiFitScene && explicitDdgiFitField)
    throw std::invalid_argument(
        "--ddgi-fit-scene cannot be combined with explicit DDGI volume, trace "
        "distance, bias or probe-radius settings; use manual setup instead.");
  if (explicitDdgiFitBudget && !config.ddgiFitScene)
    throw std::invalid_argument("--ddgi-fit-probes requires --ddgi-fit-scene");
  if (explicitDdgiDetailBudget && config.ddgiDetailNode.empty())
    throw std::invalid_argument("--ddgi-detail-probes requires --ddgi-detail-node");
  if (config.ddgiFitScene && config.ddgiFit.maxProbes < 27)
    throw std::invalid_argument("--ddgi-fit-probes must be at least 27");
  // Scene bounds become available after importing. Validate/prepare the
  // minimum padded lattice meanwhile, so a small fit budget is not rejected
  // against the unrelated default volume or allocated before fitting.
  if (config.ddgiFitScene)
    config.ddgi.volume.counts = {3, 3, 3};
  container::renderer::validateDdgiSettings(config.ddgi);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    for (int i = 1; i < argc; ++i) {
      if (argv[i] && std::string_view(argv[i]) == "--help") {
        printCommandLineHelp();
        return EXIT_SUCCESS;
      }
    }
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
