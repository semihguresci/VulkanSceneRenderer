#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace container::material {

inline constexpr uint32_t kDefaultGltfTextureMemoryMiB = 1024;
inline constexpr uint32_t kMaximumGltfTextureMemoryMiB = 16384;
inline constexpr uint64_t kMaximumGltfTextureMemoryBytes =
    uint64_t{kMaximumGltfTextureMemoryMiB} * 1024 * 1024;

// The preparation budget bounds decoded image payloads across one glTF load.
// It does not replace per-image decoder limits or promise physical GPU memory.
[[nodiscard]] constexpr uint64_t GltfTextureMemoryBytes(uint32_t mebibytes) {
  if (!mebibytes || mebibytes > kMaximumGltfTextureMemoryMiB)
    throw std::invalid_argument("--texture-memory-mb must be an integer in [1,16384] MiB");
  return uint64_t{mebibytes} * 1024 * 1024;
}

[[nodiscard]] inline uint32_t ParseGltfTextureMemoryMiB(std::string_view value) {
  if (value.empty())
    throw std::invalid_argument("--texture-memory-mb must be an integer in [1,16384] MiB");
  uint32_t mebibytes{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), mebibytes);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument("--texture-memory-mb must be an integer in [1,16384] MiB");
  (void)GltfTextureMemoryBytes(mebibytes);
  return mebibytes;
}

} // namespace container::material
