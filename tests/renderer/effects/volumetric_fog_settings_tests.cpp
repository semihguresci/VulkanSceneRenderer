#include "Container/renderer/effects/VolumetricFogSettings.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
using container::renderer::VolumetricFogMode;
using container::renderer::VolumetricFogSettings;

constexpr std::array kEnabledModes{VolumetricFogMode::Low, VolumetricFogMode::High,
    VolumetricFogMode::Transmittance, VolumetricFogMode::Scattering, VolumetricFogMode::Froxels};

struct SettingRange {
  const char* name;
  float VolumetricFogSettings::*field;
  float minimum;
  float maximum;
};

constexpr std::array<SettingRange, 4> kSettingRanges{{
    {"density", &VolumetricFogSettings::density, 0.0f, 4.0f},
    {"albedo", &VolumetricFogSettings::albedo, 0.0f, 1.0f},
    {"anisotropy", &VolumetricFogSettings::anisotropy, -0.9f, 0.9f},
    {"distance", &VolumetricFogSettings::distance, 0.5f, 5000.0f},
}};

TEST(VolumetricFogSettings, DefaultsAreValidAndOptIn) {
  const VolumetricFogSettings settings;
  EXPECT_EQ(settings.mode, VolumetricFogMode::Off);
  EXPECT_TRUE(settings.valid());
  EXPECT_FALSE(settings.enabled());
}

TEST(VolumetricFogSettings, QualityAndDiagnosticModesEnableFogWithPositiveDensity) {
  for (const auto mode : kEnabledModes) {
    SCOPED_TRACE(static_cast<uint32_t>(mode));
    VolumetricFogSettings settings{.mode = mode};
    EXPECT_TRUE(settings.valid());
    EXPECT_TRUE(settings.enabled());

    // A purely absorbing medium still attenuates background light.
    settings.albedo = 0.0f;
    EXPECT_TRUE(settings.valid());
    EXPECT_TRUE(settings.enabled());
  }
}

TEST(VolumetricFogSettings, ZeroDensityBypassesEveryActiveMode) {
  for (const auto mode : kEnabledModes) {
    SCOPED_TRACE(static_cast<uint32_t>(mode));
    for (float zero : {0.0f, -0.0f}) {
      const VolumetricFogSettings settings{.mode = mode, .density = zero};
      EXPECT_TRUE(settings.valid());
      EXPECT_FALSE(settings.enabled());
    }
  }
}

TEST(VolumetricFogSettings, ParameterBoundsAreInclusive) {
  constexpr float infinity = std::numeric_limits<float>::infinity();
  for (const auto& range : kSettingRanges) {
    SCOPED_TRACE(range.name);
    VolumetricFogSettings settings{.mode = VolumetricFogMode::High};
    settings.*range.field = range.minimum;
    EXPECT_TRUE(settings.valid());
    settings.*range.field = range.maximum;
    EXPECT_TRUE(settings.valid());
    EXPECT_TRUE(settings.enabled());

    for (float outside : {std::nextafter(range.minimum, -infinity),
                           std::nextafter(range.maximum, infinity)}) {
      settings.*range.field = outside;
      EXPECT_FALSE(settings.valid());
      EXPECT_FALSE(settings.enabled());
    }
  }
}

TEST(VolumetricFogSettings, RejectsNonfiniteParameters) {
  constexpr float infinity = std::numeric_limits<float>::infinity();
  for (const auto& range : kSettingRanges) {
    SCOPED_TRACE(range.name);
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), infinity, -infinity}) {
      SCOPED_TRACE(invalid);
      VolumetricFogSettings settings{.mode = VolumetricFogMode::Low};
      settings.*range.field = invalid;
      EXPECT_FALSE(settings.valid());
      EXPECT_FALSE(settings.enabled());
    }
  }
}

TEST(VolumetricFogSettings, RejectsUnknownModes) {
  for (const uint32_t invalid : {static_cast<uint32_t>(VolumetricFogMode::Froxels) + 1u,
                                  std::numeric_limits<uint32_t>::max()}) {
    SCOPED_TRACE(invalid);
    const VolumetricFogSettings settings{.mode = static_cast<VolumetricFogMode>(invalid)};
    EXPECT_FALSE(settings.valid());
    EXPECT_FALSE(settings.enabled());
  }
}
}  // namespace
