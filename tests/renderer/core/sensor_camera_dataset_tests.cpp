#include "Container/renderer/core/SensorCameraDataset.h"
#include "Container/utility/SceneData.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <tinyexr.h>

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace container::renderer;
using namespace container::robotics;
constexpr uint32_t bim = container::gpu::kPickIdBimMask;

struct Decoded {
  EXRHeader header;
  EXRImage image;
  const char* error{};
  Decoded() { InitEXRHeader(&header); InitEXRImage(&image); }
  ~Decoded() {
    FreeEXRImage(&image);
    FreeEXRHeader(&header);
    if (error) FreeEXRErrorMessage(error);
  }
  void read(const EncodedSensorCameraFrame& encoded) {
    EXRVersion version{};
    if (ParseEXRVersionFromMemory(&version, encoded.exr.get(), encoded.exrSize) != TINYEXR_SUCCESS ||
        ParseEXRHeaderFromMemory(&header, &version, encoded.exr.get(), encoded.exrSize, &error) != TINYEXR_SUCCESS)
      throw std::runtime_error(error ? error : "Could not parse dataset EXR");
    for (int channel = 0; channel < header.num_channels; ++channel)
      header.requested_pixel_types[channel] = header.pixel_types[channel];
    if (LoadEXRImageFromMemory(&image, &header, encoded.exr.get(), encoded.exrSize, &error) != TINYEXR_SUCCESS)
      throw std::runtime_error(error ? error : "Could not load dataset EXR");
  }
  int index(const char* name, int type) const {
    for (int channel = 0; channel < header.num_channels; ++channel)
      if (std::strcmp(header.channels[channel].name, name) == 0) {
        EXPECT_EQ(header.pixel_types[channel], type) << name;
        return channel;
      }
    throw std::runtime_error(std::string("Missing dataset channel ") + name);
  }
  const float* floats(const char* name) const {
    return reinterpret_cast<const float*>(image.images[index(name, TINYEXR_PIXELTYPE_FLOAT)]);
  }
  const uint32_t* integers(const char* name) const {
    return reinterpret_cast<const uint32_t*>(image.images[index(name, TINYEXR_PIXELTYPE_UINT)]);
  }
};

SensorCameraSettings camera(uint32_t width, uint32_t height) {
  SensorCameraSettings result;
  result.width = width;
  result.height = height;
  result.fx = result.fy = 1;
  result.cx = result.cy = .5;
  result.nearMeters = .1;
  result.farMeters = 10;
  return result;
}

std::vector<SceneDataCapturePixel> surfaces(std::initializer_list<float> depths) {
  std::vector<SceneDataCapturePixel> result;
  for (const auto depth : depths)
    result.push_back({.depth = depth, .objectId = bim | uint32_t(result.size() + 1)});
  return result;
}

TEST(SensorCameraDataset, RoundTripsAlignedFloatRgbDepthAndExactUintLabelsWithIndependentMetadata) {
  auto settings = camera(2, 2);
  SensorCameraFrame frame{.tick = 12, .seconds = .2, .sampleIndex = 2};
  frame.worldFromCamera[3] = {10, 20, 30, 1};
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  const std::array<float, 16> rgba{-.5f, .25f, .75f, nan, 2, 3, 4, 1, 1000, 16, 8, 1, 4, 8, 16, 1};
  const std::array<SceneDataCapturePixel, 4> pixels{{
      {nan, nan, nan, nan, 0}, {2.25f, 0, 0, 0, bim | 1},
      {3.5f, 0, 0, 0, bim | 2}, {4.75f, 0, 0, 0, bim | 1}}};
  const std::array labels{BimSensorLabel{0xf1234567u, 0xe2345678u}, BimSensorLabel{42, 7}};
  const auto encoded = encodeSensorCameraFrame(rgba, pixels, labels, settings, frame);
  Decoded image;
  ASSERT_NO_THROW(image.read(encoded));
  EXPECT_EQ(image.image.width, 2);
  EXPECT_EQ(image.image.height, 2);
  EXPECT_EQ(image.header.num_channels, 6);
  for (size_t pixel = 0; pixel < 4; ++pixel) {
    EXPECT_FLOAT_EQ(image.floats("R")[pixel], rgba[4 * pixel]);
    EXPECT_FLOAT_EQ(image.floats("G")[pixel], rgba[4 * pixel + 1]);
    EXPECT_FLOAT_EQ(image.floats("B")[pixel], rgba[4 * pixel + 2]);
  }
  const std::array<float, 4> depth{0, 2.25f, 3.5f, 4.75f};
  const std::array<uint32_t, 4> instances{0, 0xf1234567u, 42, 0xf1234567u};
  const std::array<uint32_t, 4> semantics{0, 0xe2345678u, 7, 0xe2345678u};
  for (size_t pixel = 0; pixel < 4; ++pixel) {
    EXPECT_FLOAT_EQ(image.floats("Z")[pixel], depth[pixel]);
    EXPECT_EQ(image.integers("InstanceID")[pixel], instances[pixel]);
    EXPECT_EQ(image.integers("SemanticID")[pixel], semantics[pixel]);
  }
  const auto metadata = nlohmann::json::parse(encoded.metadataJson);
  EXPECT_EQ(metadata.at("schema"), "container.sensor-camera-frame");
  EXPECT_EQ(metadata.at("tick"), 12);
  EXPECT_EQ(metadata.at("sampleIndex"), 2);
  EXPECT_DOUBLE_EQ(metadata.at("seconds").get<double>(), .2);
  EXPECT_EQ(metadata.at("worldFromCamera").at(3), nlohmann::json::array({10., 20., 30., 1.}));
  EXPECT_EQ(metadata.at("channels").at("InstanceID"), "UINT");
  EXPECT_EQ(metadata.at("backgroundId"), 0);
}

TEST(SensorCameraDataset, InverseLensPullUsesBilinearRgbAndOneNearestDepthLabelCoordinate) {
  auto settings = camera(3, 1);
  settings.distortion = SensorCameraDistortion{.k1 = 1};
  const std::array<float, 12> rgba{0, 0, 0, 1, 10, 20, 30, 1, 20, 40, 60, 1};
  const auto pixels = surfaces({1, 2, 3});
  const std::array labels{BimSensorLabel{11, 21}, BimSensorLabel{12, 22}, BimSensorLabel{13, 23}};
  const auto encoded = encodeSensorCameraFrame(rgba, pixels, labels, settings, {});
  Decoded image;
  ASSERT_NO_THROW(image.read(encoded));
  // At output x=2.5, distorted normalized x=2; ideal x=1 solves x+x^3=2.
  EXPECT_NEAR(image.floats("R")[2], 10, 1e-5);
  EXPECT_NEAR(image.floats("G")[2], 20, 1e-5);
  EXPECT_NEAR(image.floats("B")[2], 30, 1e-5);
  EXPECT_FLOAT_EQ(image.floats("Z")[2], 2);
  EXPECT_EQ(image.integers("InstanceID")[2], 12u);
  EXPECT_EQ(image.integers("SemanticID")[2], 22u);
  // The middle output solves x+x^3=1; it mixes RGB but selects source pixel1.
  EXPECT_NEAR(image.floats("R")[1], 6.823278038280193, 1e-5);
  EXPECT_FLOAT_EQ(image.floats("Z")[1], 2);
  EXPECT_EQ(image.integers("InstanceID")[1], 12u);
}

TEST(SensorCameraDataset, ZeroDistortionIsExactAndFoldedUnmappedRegionsAreFullyZero) {
  auto settings = camera(3, 1);
  const std::array<float, 12> rgba{1, 2, 3, 1, 4, 5, 6, 1, 7, 8, 9, 1};
  const auto pixels = surfaces({1, 2, 3});
  const std::array labels{BimSensorLabel{1, 1}, BimSensorLabel{2, 2}, BimSensorLabel{3, 3}};
  const auto ideal = encodeSensorCameraFrame(rgba, pixels, labels, settings, {});
  settings.distortion = SensorCameraDistortion{};
  const auto identity = encodeSensorCameraFrame(rgba, pixels, labels, settings, {});
  EXPECT_EQ(ideal.exrBytes(), identity.exrBytes());
  settings.distortion->k1 = -1;
  const auto folded = encodeSensorCameraFrame(rgba, pixels, labels, settings, {});
  Decoded image;
  ASSERT_NO_THROW(image.read(folded));
  for (size_t pixel : {1u, 2u}) {
    for (const auto channel : {"R", "G", "B", "Z"}) EXPECT_FLOAT_EQ(image.floats(channel)[pixel], 0);
    EXPECT_EQ(image.integers("InstanceID")[pixel], 0u);
    EXPECT_EQ(image.integers("SemanticID")[pixel], 0u);
  }
  EXPECT_EQ(nlohmann::json::parse(folded.metadataJson).at("unmappedPixelCount"), 2);
}

TEST(SensorCameraDataset, RangeInvalidationZerosDepthAndLabelsWhileRetainingRgb) {
  const auto settings = camera(3, 1);
  const std::array<float, 12> rgba{1, 2, 3, 1, 4, 5, 6, 1, 7, 8, 9, 1};
  const auto pixels = surfaces({.05f, 20, 3});
  const std::array labels{BimSensorLabel{1, 1}, BimSensorLabel{2, 2}, BimSensorLabel{3, 3}};
  const auto encoded = encodeSensorCameraFrame(rgba, pixels, labels, settings, {});
  Decoded image;
  ASSERT_NO_THROW(image.read(encoded));
  for (size_t pixel : {0u, 1u}) {
    EXPECT_FLOAT_EQ(image.floats("R")[pixel], rgba[4 * pixel]);
    EXPECT_FLOAT_EQ(image.floats("Z")[pixel], 0);
    EXPECT_EQ(image.integers("InstanceID")[pixel], 0u);
    EXPECT_EQ(image.integers("SemanticID")[pixel], 0u);
  }
  EXPECT_FLOAT_EQ(image.floats("Z")[2], 3);
  EXPECT_EQ(nlohmann::json::parse(encoded.metadataJson).at("rangeRejectedPixelCount"), 2);
}

TEST(SensorCameraDataset, SeededDepthNoiseReplaysPerPixelAndSampleWithoutChangingValidLabels) {
  auto settings = camera(4, 1);
  settings.depthNoiseStdDevMeters = .1;
  settings.depthNoiseSeed = 17;
  const std::array<float, 16> rgba{};
  const std::array<SceneDataCapturePixel, 4> pixels{{
      {2, 0, 0, 0, bim | 1}, {2, 0, 0, 0, bim | 1},
      {2, 0, 0, 0, bim | 1}, {2, 0, 0, 0, bim | 1}}};
  const std::array labels{BimSensorLabel{123, 456}};
  const auto first = encodeSensorCameraFrame(rgba, pixels, labels, settings, {});
  const auto repeated = encodeSensorCameraFrame(rgba, pixels, labels, settings, {});
  EXPECT_EQ(first.exrBytes(), repeated.exrBytes());
  EXPECT_EQ(first.metadataJson, repeated.metadataJson);
  const auto next = encodeSensorCameraFrame(rgba, pixels, labels, settings, {.sampleIndex = 1});
  EXPECT_NE(first.exrBytes(), next.exrBytes());
  ++settings.depthNoiseSeed;
  EXPECT_NE(first.exrBytes(), encodeSensorCameraFrame(rgba, pixels, labels, settings, {}).exrBytes());
  Decoded image;
  ASSERT_NO_THROW(image.read(first));
  for (size_t pixel = 0; pixel < 4; ++pixel) {
    EXPECT_GT(image.floats("Z")[pixel], .1f);
    EXPECT_LT(image.floats("Z")[pixel], 10.f);
    EXPECT_EQ(image.integers("InstanceID")[pixel], 123u);
    EXPECT_EQ(image.integers("SemanticID")[pixel], 456u);
  }
  settings.depthNoiseStdDevMeters = 1e6;
  const auto invalid = encodeSensorCameraFrame(rgba, pixels, labels, settings, {});
  Decoded clipped;
  ASSERT_NO_THROW(clipped.read(invalid));
  EXPECT_GT(nlohmann::json::parse(invalid.metadataJson).at("rangeRejectedPixelCount"), 0);
  for (size_t pixel = 0; pixel < 4; ++pixel)
    if (clipped.floats("Z")[pixel] == 0) {
      EXPECT_EQ(clipped.integers("InstanceID")[pixel], 0u);
      EXPECT_EQ(clipped.integers("SemanticID")[pixel], 0u);
    }
}

TEST(SensorCameraDataset, RejectsMisalignedNonfiniteOrUnlabelledForegroundBeforePublishingBytes) {
  const auto settings = camera(1, 1);
  std::array<float, 4> rgba{1, 2, 3, 1};
  auto pixels = surfaces({2});
  std::array labels{BimSensorLabel{1, 1}};
  EXPECT_THROW((void)encodeSensorCameraFrame(std::span(rgba).first(3), pixels, labels, settings, {}), std::invalid_argument);
  EXPECT_THROW((void)encodeSensorCameraFrame(rgba, {}, labels, settings, {}), std::invalid_argument);
  for (const auto badId : {1u, bim, bim | 2u, bim | container::gpu::kPickIdLightMask | 1u}) {
    pixels[0].objectId = badId;
    EXPECT_THROW((void)encodeSensorCameraFrame(rgba, pixels, labels, settings, {}), std::invalid_argument);
  }
  pixels[0].objectId = bim | 1;
  labels[0] = {0, 1};
  EXPECT_THROW((void)encodeSensorCameraFrame(rgba, pixels, labels, settings, {}), std::invalid_argument);
  labels[0] = {1, 0};
  EXPECT_THROW((void)encodeSensorCameraFrame(rgba, pixels, labels, settings, {}), std::invalid_argument);
  labels[0] = {1, 1};
  rgba[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)encodeSensorCameraFrame(rgba, pixels, labels, settings, {}), std::invalid_argument);
  rgba[0] = 1;
  pixels[0].depth = std::numeric_limits<float>::infinity();
  EXPECT_THROW((void)encodeSensorCameraFrame(rgba, pixels, labels, settings, {}), std::invalid_argument);
  pixels[0].depth = 2;
  SensorCameraFrame scaled;
  scaled.worldFromCamera[0][0] = 2;
  EXPECT_THROW((void)encodeSensorCameraFrame(rgba, pixels, labels, settings, scaled), std::invalid_argument);
}

} // namespace
