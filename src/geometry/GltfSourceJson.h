#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace container::geometry::gltf::detail {

inline constexpr size_t kGlbJsonOffset = 20;

// The input may be just the prefix of a streamed file. Compare its header to
// the complete size before any caller uses the JSON chunk as a byte range.
inline std::optional<uint32_t> GlbJsonChunkLength(std::span<const unsigned char> prefix, uint64_t fileSize) {
  if (prefix.size() < kGlbJsonOffset || fileSize < kGlbJsonOffset) return std::nullopt;
  const auto word = [&](size_t offset) {
    return uint32_t(prefix[offset]) | (uint32_t(prefix[offset + 1]) << 8) |
           (uint32_t(prefix[offset + 2]) << 16) | (uint32_t(prefix[offset + 3]) << 24);
  };
  const auto length = word(12);
  if (word(0) != 0x46546c67 || word(4) != 2 || word(8) != fileSize || word(16) != 0x4e4f534a ||
      length > fileSize - kGlbJsonOffset) return std::nullopt;
  return length;
}

} // namespace container::geometry::gltf::detail
