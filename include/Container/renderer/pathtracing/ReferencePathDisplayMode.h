#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace container::renderer {

enum class ReferencePathDisplayMode : uint32_t { Beauty, StandardError };

constexpr std::string_view referencePathDisplayName(ReferencePathDisplayMode mode) {
  return mode == ReferencePathDisplayMode::StandardError ? "standard-error" : "beauty";
}

constexpr std::optional<ReferencePathDisplayMode> parseReferencePathDisplay(std::string_view mode) {
  if (mode == "beauty") return ReferencePathDisplayMode::Beauty;
  if (mode == "standard-error") return ReferencePathDisplayMode::StandardError;
  return std::nullopt;
}

} // namespace container::renderer
