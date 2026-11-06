#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace container::renderer {

enum class ReferenceOidnQuality { Off, Fast, Balanced, High };

[[nodiscard]] constexpr const char* referenceOidnQualityName(ReferenceOidnQuality quality) {
  switch (quality) {
    case ReferenceOidnQuality::Fast: return "fast";
    case ReferenceOidnQuality::Balanced: return "balanced";
    case ReferenceOidnQuality::High: return "high";
    default: return "off";
  }
}

[[nodiscard]] constexpr std::optional<ReferenceOidnQuality> parseReferenceOidnQuality(std::string_view name) {
  if (name == "off") return ReferenceOidnQuality::Off;
  if (name == "fast") return ReferenceOidnQuality::Fast;
  if (name == "balanced") return ReferenceOidnQuality::Balanced;
  if (name == "high") return ReferenceOidnQuality::High;
  return std::nullopt;
}

struct ReferenceOidnSettings {
  ReferenceOidnQuality quality{ReferenceOidnQuality::Off};
  uint32_t sampleInterval{8};
  static constexpr uint32_t maxSampleInterval = 1024;
};

} // namespace container::renderer
