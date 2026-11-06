#include "Container/renderer/effects/OpenVdbVolumeSettings.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
using container::renderer::OpenVdbVolumeSettings;
using container::renderer::sanitizeOpenVdbVolumeSettings;

TEST(OpenVdbVolumeSettings, DefaultsKeepMultipleScatteringOptIn) {
  const OpenVdbVolumeSettings settings;
  EXPECT_FLOAT_EQ(settings.multipleScatteringStrength, 0.0f);
  EXPECT_EQ(settings.multipleScatteringOrders, 2u);
  EXPECT_EQ(sanitizeOpenVdbVolumeSettings(settings), settings);
}

TEST(OpenVdbVolumeSettings, ValidMultipleScatteringControlsPreserveEveryOtherSetting) {
  for (const float strength : {0.0f, 0.25f, 1.0f}) {
    for (uint32_t orders = 1; orders <= 4; ++orders) {
      SCOPED_TRACE(::testing::Message() << "strength=" << strength << ", orders=" << orders);
      OpenVdbVolumeSettings settings;
      settings.enabled = false;
      settings.receiveShadows = false;
      settings.densityScale = 2.5f;
      settings.scatteringColor = {0.0f, 0.4f, 1.0f};
      settings.anisotropy = -0.3f;
      settings.emissionColor = {0.1f, 12.0f, 500.0f};
      settings.emissionScale = 8.0f;
      settings.stepScale = 0.75f;
      settings.temperatureScale = 4000.0f;
      settings.multipleScatteringStrength = strength;
      settings.multipleScatteringOrders = orders;
      EXPECT_EQ(sanitizeOpenVdbVolumeSettings(settings), settings);
    }
  }
}

TEST(OpenVdbVolumeSettings, MultipleScatteringStrengthHasClosedFiniteBounds) {
  for (const float value : {-std::numeric_limits<float>::max(),
                             std::nextafter(0.0f, -1.0f)}) {
    OpenVdbVolumeSettings settings;
    settings.multipleScatteringStrength = value;
    EXPECT_FLOAT_EQ(sanitizeOpenVdbVolumeSettings(settings).multipleScatteringStrength, 0.0f);
  }
  for (const float value : {std::nextafter(1.0f, 2.0f), std::numeric_limits<float>::max()}) {
    OpenVdbVolumeSettings settings;
    settings.multipleScatteringStrength = value;
    EXPECT_FLOAT_EQ(sanitizeOpenVdbVolumeSettings(settings).multipleScatteringStrength, 1.0f);
  }
  OpenVdbVolumeSettings tiny;
  tiny.multipleScatteringStrength = std::numeric_limits<float>::min();
  EXPECT_FLOAT_EQ(sanitizeOpenVdbVolumeSettings(tiny).multipleScatteringStrength,
                  tiny.multipleScatteringStrength);
}

TEST(OpenVdbVolumeSettings, AdditionalOrderBudgetClampsWithoutUnsignedWraparound) {
  OpenVdbVolumeSettings settings;
  settings.multipleScatteringOrders = 0;
  EXPECT_EQ(sanitizeOpenVdbVolumeSettings(settings).multipleScatteringOrders, 1u);
  for (const uint32_t orders : {5u, std::numeric_limits<uint32_t>::max()}) {
    settings.multipleScatteringOrders = orders;
    EXPECT_EQ(sanitizeOpenVdbVolumeSettings(settings).multipleScatteringOrders, 4u);
  }
}

TEST(OpenVdbVolumeSettings, NonfiniteInputsRecoverFiniteDefaultsEvenWhenDisabled) {
  constexpr std::array fields{
      &OpenVdbVolumeSettings::densityScale,
      &OpenVdbVolumeSettings::anisotropy,
      &OpenVdbVolumeSettings::emissionScale,
      &OpenVdbVolumeSettings::stepScale,
      &OpenVdbVolumeSettings::temperatureScale,
      &OpenVdbVolumeSettings::multipleScatteringStrength};
  const OpenVdbVolumeSettings defaults;
  for (const float value : {std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()}) {
    for (const auto field : fields) {
      OpenVdbVolumeSettings settings;
      settings.enabled = false;
      settings.receiveShadows = false;
      settings.*field = value;
      const auto result = sanitizeOpenVdbVolumeSettings(settings);
      EXPECT_TRUE(std::isfinite(result.*field));
      EXPECT_FLOAT_EQ(result.*field, defaults.*field);
      EXPECT_FALSE(result.enabled);
      EXPECT_FALSE(result.receiveShadows);
    }
    for (size_t channel = 0; channel < 3; ++channel) {
      OpenVdbVolumeSettings settings;
      settings.scatteringColor[channel] = value;
      settings.emissionColor[channel] = value;
      EXPECT_EQ(sanitizeOpenVdbVolumeSettings(settings), defaults);
    }
  }
}

TEST(OpenVdbVolumeSettings, PhysicalInputBoundsStayFiniteAndSanitizationIsIdempotent) {
  OpenVdbVolumeSettings settings;
  settings.densityScale = -1;
  settings.scatteringColor = {-1, 0.3f, 2};
  settings.anisotropy = -2;
  settings.emissionColor = {-1, 17, 20000};
  settings.emissionScale = 20000;
  settings.stepScale = 0;
  settings.temperatureScale = 200000;
  settings.multipleScatteringStrength = 2;
  settings.multipleScatteringOrders = 5;
  const auto result = sanitizeOpenVdbVolumeSettings(settings);
  EXPECT_FLOAT_EQ(result.densityScale, 0);
  EXPECT_EQ(result.scatteringColor, (std::array<float, 3>{0, 0.3f, 1}));
  EXPECT_FLOAT_EQ(result.anisotropy, -0.95f);
  EXPECT_EQ(result.emissionColor, (std::array<float, 3>{0, 17, 10000}));
  EXPECT_FLOAT_EQ(result.emissionScale, 10000);
  EXPECT_FLOAT_EQ(result.stepScale, 0.25f);
  EXPECT_FLOAT_EQ(result.temperatureScale, 100000);
  EXPECT_FLOAT_EQ(result.multipleScatteringStrength, 1);
  EXPECT_EQ(result.multipleScatteringOrders, 4u);
  EXPECT_EQ(sanitizeOpenVdbVolumeSettings(result), result);
  // Albedo zero and extinction zero remain representable for vacuum/absorber
  // GPU invariants; adding the higher-order controls must not raise either.
  settings = {};
  settings.densityScale = 0;
  settings.scatteringColor = {0, 0, 0};
  settings.multipleScatteringStrength = 1;
  EXPECT_EQ(sanitizeOpenVdbVolumeSettings(settings), settings);
}
} // namespace
