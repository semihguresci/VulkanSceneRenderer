#include "Container/renderer/core/SensorCameraDataset.h"
#include "Container/utility/SceneData.h"

#include <nlohmann/json.hpp>
#include <tinyexr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <vector>

namespace container::renderer {
namespace {

using Json = nlohmann::json;

struct Coordinate { double x, y; };
struct Distorted {
  double x, y, xx, xy, yy;
  bool finite() const {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(xx) &&
           std::isfinite(xy) && std::isfinite(yy);
  }
};

Distorted forward(Coordinate point, const robotics::SensorCameraDistortion& lens) {
  const auto x = point.x, y = point.y, radius = x * x + y * y;
  const auto radial = 1 + radius * (lens.k1 + radius * (lens.k2 + radius * lens.k3));
  const auto derivative = 2 * (lens.k1 + radius * (2 * lens.k2 + radius * 3 * lens.k3));
  return {x * radial + 2 * lens.p1 * x * y + lens.p2 * (radius + 2 * x * x),
          y * radial + lens.p1 * (radius + 2 * y * y) + 2 * lens.p2 * x * y,
          radial + x * x * derivative + 2 * lens.p1 * y + 6 * lens.p2 * x,
          x * y * derivative + 2 * lens.p1 * x + 2 * lens.p2 * y,
          radial + y * y * derivative + 6 * lens.p1 * y + 2 * lens.p2 * x};
}

std::optional<Coordinate> sourceCoordinate(uint32_t x, uint32_t y,
                                           const robotics::SensorCameraSettings& camera) {
  const Coordinate pixel{double(x) + .5, double(y) + .5};
  if (!camera.distortion) return pixel;
  const auto& lens = *camera.distortion;
  if (lens.k1 == 0 && lens.k2 == 0 && lens.k3 == 0 && lens.p1 == 0 && lens.p2 == 0) return pixel;
  const Coordinate target{(pixel.x - camera.cx) / camera.fx, (pixel.y - camera.cy) / camera.fy};
  auto ideal = target;
  // Pull distorted output centres back into the ideal rendered image. A
  // bounded damped Newton solve rejects singular/folded local mappings.
  for (unsigned iteration = 0; iteration < 24; ++iteration) {
    const auto value = forward(ideal, lens);
    const auto determinant = value.xx * value.yy - value.xy * value.xy;
    if (!value.finite() || !(value.xx > 0 && value.yy > 0 && determinant > 1e-14) ||
        !std::isfinite(determinant)) return std::nullopt;
    const auto dx = value.x - target.x, dy = value.y - target.y;
    const auto error = std::max(std::abs(dx * camera.fx), std::abs(dy * camera.fy));
    if (error <= 1e-7) {
      const Coordinate source{ideal.x * camera.fx + camera.cx, ideal.y * camera.fy + camera.cy};
      if (std::isfinite(source.x) && std::isfinite(source.y) && source.x >= 0 && source.y >= 0 &&
          source.x < camera.width && source.y < camera.height) return source;
      return std::nullopt;
    }
    const Coordinate delta{(value.yy * dx - value.xy * dy) / determinant,
                            (value.xx * dy - value.xy * dx) / determinant};
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y)) return std::nullopt;
    bool improved = false;
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
      const auto scale = std::ldexp(1.0, -static_cast<int>(attempt));
      const Coordinate next{ideal.x - scale * delta.x, ideal.y - scale * delta.y};
      const auto projected = forward(next, lens);
      const auto nextError = std::max(std::abs((projected.x - target.x) * camera.fx),
                                      std::abs((projected.y - target.y) * camera.fy));
      if (projected.finite() && std::isfinite(nextError) && nextError < error) {
        ideal = next;
        improved = true;
        break;
      }
    }
    if (!improved) return std::nullopt;
  }
  return std::nullopt;
}

robotics::BimSensorLabel labelFor(uint32_t id, std::span<const robotics::BimSensorLabel> labels) {
  if (id == gpu::kPickIdNone) return {};
  const auto slot = id & gpu::kPickIdObjectMask;
  if ((id & ~gpu::kPickIdObjectMask) != gpu::kPickIdBimMask || slot == 0 || slot > labels.size())
    throw std::invalid_argument("Sensor camera surface contains an unmapped BIM pick ID");
  const auto label = labels[slot - 1];
  if (label.instanceId == 0 || label.semanticId == 0)
    throw std::invalid_argument("Sensor camera BIM surface lacks an instance or semantic label");
  return label;
}

float rgb(std::span<const float> pixels, uint32_t width, uint32_t height, Coordinate source, size_t channel) {
  const auto x = std::clamp(source.x - .5, 0.0, double(width - 1));
  const auto y = std::clamp(source.y - .5, 0.0, double(height - 1));
  const auto x0 = static_cast<uint32_t>(x), y0 = static_cast<uint32_t>(y);
  const auto x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
  const auto sample = [&](uint32_t column, uint32_t row) { return double(pixels[4 * (size_t(row) * width + column) + channel]); };
  return static_cast<float>(std::lerp(std::lerp(sample(x0, y0), sample(x1, y0), x - x0),
                                      std::lerp(sample(x0, y1), sample(x1, y1), x - x0), y - y0));
}

uint64_t mix(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

double normal(uint64_t key) {
  const auto first = double(mix(key) >> 11) * 0x1.0p-53;
  const auto second = double(mix(key + 0x9e3779b97f4a7c15ULL) >> 11) * 0x1.0p-53;
  return std::sqrt(-2 * std::log(1 - first)) * std::cos(2 * std::numbers::pi * second);
}

// Separate streams so changing RGB noise or dropout never shifts legacy
// depth noise on any other output pixel.
constexpr uint64_t rgbNoiseStream = 0x7267622d6e6f6973ULL;
constexpr uint64_t depthDropoutStream = 0x64657074682d6472ULL;

Json matrix(const glm::dmat4& value) {
  Json columns = Json::array();
  for (glm::length_t column = 0; column < 4; ++column)
    columns.push_back({value[column].x, value[column].y, value[column].z, value[column].w});
  return columns;
}

} // namespace

void EncodedSensorCameraFrame::FreeExr::operator()(unsigned char* bytes) const noexcept { std::free(bytes); }

EncodedSensorCameraFrame encodeSensorCameraFrame(
    std::span<const float> rgba, std::span<const SceneDataCapturePixel> surfaces,
    std::span<const robotics::BimSensorLabel> objectLabels,
    const robotics::SensorCameraSettings& settings, const robotics::SensorCameraFrame& frame) {
  robotics::validateSensorCameraSettings(settings);
  robotics::validateSensorCameraFrame(frame);
  const bool rgbNoiseEnabled = settings.rgbNoiseStdDevLinear > 0;
  const bool appearanceEnabled = settings.appearance &&
      (settings.appearance->lightingVariation > 0 || settings.appearance->materialVariation > 0);
  const bool groundTruthEnabled = rgbNoiseEnabled || settings.depthDropoutProbability > 0 || appearanceEnabled;
  constexpr auto maximum = std::numeric_limits<int>::max();
  const auto bytesPerPixel = groundTruthEnabled ? 28 : 24;
  const auto count = uint64_t(settings.width) * settings.height;
  if (settings.width > uint32_t(maximum / bytesPerPixel) || settings.height > uint32_t(maximum) ||
      surfaces.size() != count || rgba.size() != count * 4)
    throw std::invalid_argument("Sensor camera input dimensions do not match its aligned host buffers");
  // Fail before encoding even when distortion would crop an invalid input ID.
  for (size_t pixel = 0; pixel < surfaces.size(); ++pixel) {
    for (size_t channel = 0; channel < 3; ++channel)
      if (!std::isfinite(rgba[4 * pixel + channel]))
        throw std::invalid_argument("Sensor camera scene-linear RGB must be finite");
    (void)labelFor(surfaces[pixel].objectId, objectLabels);
    if (surfaces[pixel].objectId != 0 &&
        (!(surfaces[pixel].depth > 0) || !std::isfinite(surfaces[pixel].depth)))
      throw std::invalid_argument("Sensor camera BIM surface has invalid axial metric depth");
  }
  std::array<std::vector<float>, 4> colorsAndDepth;
  for (auto& plane : colorsAndDepth) plane.resize(static_cast<size_t>(count));
  std::vector<float> groundTruthDepth(groundTruthEnabled ? static_cast<size_t>(count) : 0);
  std::vector<uint32_t> instances(static_cast<size_t>(count)), semantics(static_cast<size_t>(count));
  uint64_t unmapped = 0, rangeRejected = 0, depthDropped = 0;
  const auto depthKey = mix(mix(settings.depthNoiseSeed) ^ frame.sampleIndex);
  const auto dropoutKey = mix(depthKey ^ depthDropoutStream);
  const auto rgbKey = mix(mix(settings.rgbNoiseSeed ^ rgbNoiseStream) ^ frame.sampleIndex);
  for (uint32_t y = 0; y < settings.height; ++y) {
    for (uint32_t x = 0; x < settings.width; ++x) {
      const auto destination = size_t(y) * settings.width + x;
      const auto source = sourceCoordinate(x, y, settings);
      if (!source) { ++unmapped; continue; }
      for (size_t channel = 0; channel < 3; ++channel) {
        auto sample = rgb(rgba, settings.width, settings.height, *source, channel);
        if (rgbNoiseEnabled) {
          const auto error = settings.rgbNoiseStdDevLinear * normal(mix(rgbKey ^ uint64_t(3 * destination + channel)));
          sample = static_cast<float>(std::clamp(sample + error, 0.0, double(std::numeric_limits<float>::max())));
        }
        colorsAndDepth[channel][destination] = sample;
      }
      // Pixel centres use edge coordinates; floor selects the nearest centre,
      // with exact half-way ties toward increasing image coordinates.
      const auto sourcePixel = size_t(static_cast<uint32_t>(source->y)) * settings.width +
                               static_cast<uint32_t>(source->x);
      const auto& surface = surfaces[sourcePixel];
      if (surface.objectId == 0) continue;
      const auto label = labelFor(surface.objectId, objectLabels);
      double depth = surface.depth;
      if (depth < settings.nearMeters || depth > settings.farMeters) { ++rangeRejected; continue; }
      if (groundTruthEnabled) {
        groundTruthDepth[destination] = surface.depth;
        instances[destination] = label.instanceId;
        semantics[destination] = label.semanticId;
      }
      if (settings.depthDropoutProbability > 0) {
        const auto uniform = double(mix(dropoutKey ^ uint64_t(destination)) >> 11) * 0x1.0p-53;
        if (uniform < settings.depthDropoutProbability) { ++depthDropped; continue; }
      }
      if (settings.depthNoiseStdDevMeters > 0)
        depth += settings.depthNoiseStdDevMeters * normal(mix(depthKey ^ uint64_t(destination)));
      if (!std::isfinite(depth) || depth < settings.nearMeters || depth > settings.farMeters ||
          depth > std::numeric_limits<float>::max()) { ++rangeRejected; continue; }
      const auto storedDepth = static_cast<float>(depth);
      if (!(storedDepth > 0) || storedDepth < settings.nearMeters || storedDepth > settings.farMeters) {
        ++rangeRejected;
        continue;
      }
      colorsAndDepth[3][destination] = storedDepth;
      instances[destination] = label.instanceId;
      semantics[destination] = label.semanticId;
    }
  }

  std::array<EXRChannelInfo, 7> channels{};
  std::array<int, 7> types{};
  std::array<unsigned char*, 7> images{};
  int channelCount = 0;
  const auto addChannel = [&](const char* name, int type, auto& plane) {
    std::memcpy(channels[channelCount].name, name, std::strlen(name) + 1);
    types[channelCount] = type;
    images[channelCount] = reinterpret_cast<unsigned char*>(plane.data());
    ++channelCount;
  };
  addChannel("B", TINYEXR_PIXELTYPE_FLOAT, colorsAndDepth[2]);
  addChannel("G", TINYEXR_PIXELTYPE_FLOAT, colorsAndDepth[1]);
  if (groundTruthEnabled) addChannel("GroundTruthZ", TINYEXR_PIXELTYPE_FLOAT, groundTruthDepth);
  addChannel("InstanceID", TINYEXR_PIXELTYPE_UINT, instances);
  addChannel("R", TINYEXR_PIXELTYPE_FLOAT, colorsAndDepth[0]);
  addChannel("SemanticID", TINYEXR_PIXELTYPE_UINT, semantics);
  addChannel("Z", TINYEXR_PIXELTYPE_FLOAT, colorsAndDepth[3]);
  EXRHeader header;
  InitEXRHeader(&header);
  header.num_channels = channelCount;
  header.channels = channels.data();
  header.pixel_types = types.data();
  header.requested_pixel_types = types.data();
  header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
  EXRImage image;
  InitEXRImage(&image);
  image.width = int(settings.width);
  image.height = int(settings.height);
  image.num_channels = header.num_channels;
  image.images = images.data();
  EncodedSensorCameraFrame encoded;
  unsigned char* bytes = nullptr;
  const char* error = nullptr;
  encoded.exrSize = SaveEXRImageToMemory(&image, &header, &bytes, &error);
  encoded.exr.reset(bytes);
  const std::unique_ptr<const char, decltype(&FreeEXRErrorMessage)> message(error, &FreeEXRErrorMessage);
  if (!encoded.exr || encoded.exrSize == 0)
    throw std::runtime_error(std::string("Sensor camera EXR encoding failed: ") + (message ? message.get() : "unknown error"));
  Json distortion = nullptr;
  if (settings.distortion) {
    const auto& lens = *settings.distortion;
    distortion = {{"model", "Brown-Conrady"}, {"k1", lens.k1}, {"k2", lens.k2},
                   {"p1", lens.p1}, {"p2", lens.p2}, {"k3", lens.k3}};
  }
  Json metadata{{"schema", "container.sensor-camera-frame"}, {"version", 1},
      {"tick", frame.tick}, {"seconds", frame.seconds}, {"sampleIndex", frame.sampleIndex},
      {"intrinsics", {{"width", settings.width}, {"height", settings.height}, {"fx", settings.fx},
                       {"fy", settings.fy}, {"cx", settings.cx}, {"cy", settings.cy},
                       {"nearMeters", settings.nearMeters}, {"farMeters", settings.farMeters}}},
      {"worldFromCamera", matrix(frame.worldFromCamera)}, {"worldFrame", "REP-103 metres"},
      {"cameraAxes", "+X right, +Y up, -Z forward"}, {"matrixLayout", "column-major"},
      {"imageOrigin", "top-left"}, {"pixelCenters", "i+0.5,j+0.5"}, {"backgroundId", 0},
      {"channels", {{"R", "scene-linear FLOAT"}, {"G", "scene-linear FLOAT"}, {"B", "scene-linear FLOAT"},
                    {"Z", "axial metres FLOAT; zero invalid/background"}, {"InstanceID", "UINT"}, {"SemanticID", "UINT"}}},
      {"distortion", distortion}, {"resampling", {{"RGB", "bilinear"}, {"depthAndLabels", "nearest, same source coordinate"},
                                                  {"unmapped", "all channels zero"}}},
      {"depthNoise", {{"model", "independent Gaussian"}, {"stdDevMeters", settings.depthNoiseStdDevMeters},
                      {"seed", settings.depthNoiseSeed}, {"key", "seed,sampleIndex,destinationPixel"},
                      {"outOfRange", "zero depth and labels; RGB retained"}}},
      {"unmappedPixelCount", unmapped}, {"rangeRejectedPixelCount", rangeRejected}};
  if (groundTruthEnabled) {
    metadata["channels"]["GroundTruthZ"] = "axial metres FLOAT before observation noise; zero invalid/background";
    metadata["depthNoise"]["dropoutProbability"] = settings.depthDropoutProbability;
    metadata["depthNoise"]["outOfRange"] = "zero observed depth; ground truth and labels retained";
    metadata["depthDroppedPixelCount"] = depthDropped;
  }
  if (rgbNoiseEnabled)
    metadata["rgbNoise"] = {{"model", "independent Gaussian read noise"}, {"stdDevLinear", settings.rgbNoiseStdDevLinear},
                            {"seed", settings.rgbNoiseSeed}, {"key", "seed,sampleIndex,destinationPixel,channel"}};
  if (settings.appearance)
    metadata["appearance"] = {{"seed", settings.appearance->seed},
                              {"lightingVariation", settings.appearance->lightingVariation},
                              {"materialVariation", settings.appearance->materialVariation}};
  encoded.metadataJson = metadata.dump(2) + '\n';
  return encoded;
}

} // namespace container::renderer
