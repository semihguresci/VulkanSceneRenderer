#pragma once

#include <Container/renderer/temporal/UpscaleMode.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace container::renderer {

// Admission cap, not an eager allocation. Full-HD OptiX guides, staging and
// queried working memory can exceed 512 MiB; keep one shared finite default.
inline constexpr uint64_t kDefaultReferenceImageBudgetBytes = 1024ull * 1024 * 1024;

[[nodiscard]] constexpr bool referenceOptixImageBudgetFits(
    uint64_t denoiserBytes, uint64_t rgbaImageBytes, uint64_t budgetBytes) noexcept {
  // Include the three reference accumulation/display images without permitting
  // an overflowing sum to pass the same complete-image memory admission check.
  return budgetBytes && rgbaImageBytes <= budgetBytes / 3 &&
      denoiserBytes <= budgetBytes - 3 * rgbaImageBytes;
}

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
  bool optix{};
  UpscaleMode dlss{UpscaleMode::Native};
  static constexpr uint32_t maxSampleInterval = 1024;
};

[[nodiscard]] constexpr std::string_view referenceDlssQualityName(UpscaleMode mode) noexcept {
  return mode == UpscaleMode::Native ? "off" : upscaleModeName(mode);
}

[[nodiscard]] constexpr std::optional<UpscaleMode> parseReferenceDlssQuality(std::string_view name) noexcept {
  if (name == "off") return UpscaleMode::Native;
  if (name == "native") return std::nullopt;
  return parseUpscaleMode(name);
}

} // namespace container::renderer
