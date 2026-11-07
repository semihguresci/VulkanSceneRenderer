#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace container::scene {
namespace detail {
inline void ValidateSourceMaterialTableSize(std::span<const uint32_t> mapping,
                                            size_t sourceCount) {
  if (mapping.size() != sourceCount || sourceCount > size_t(std::numeric_limits<int32_t>::max()))
    throw std::invalid_argument("Source material mapping must cover the complete source table");
}
inline uint32_t ValidateRuntimeMaterialIndex(uint32_t runtime, size_t runtimeCount) {
  if (runtime >= runtimeCount || runtime > uint32_t(std::numeric_limits<int32_t>::max()))
    throw std::invalid_argument("Source material mapping references an unavailable runtime material");
  return runtime;
}
} // namespace detail

inline void ValidateSourceMaterialMapping(std::span<const uint32_t> mapping,
                                          size_t sourceCount, size_t runtimeCount) {
  detail::ValidateSourceMaterialTableSize(mapping, sourceCount);
  for (const auto runtime : mapping) detail::ValidateRuntimeMaterialIndex(runtime, runtimeCount);
}

// Only -1 denotes glTF's implicit material. A missing/malformed source binding
// must never silently resolve to material zero or the current fallback.
inline uint32_t ResolveSourceMaterialIndex(int32_t source, std::span<const uint32_t> mapping,
                                          size_t sourceCount, size_t runtimeCount,
                                          uint32_t implicitDefault) {
  detail::ValidateSourceMaterialTableSize(mapping, sourceCount);
  if (source == -1) return detail::ValidateRuntimeMaterialIndex(implicitDefault, runtimeCount);
  if (source < 0 || size_t(source) >= sourceCount)
    throw std::invalid_argument("Source material index is outside the source table");
  return detail::ValidateRuntimeMaterialIndex(mapping[size_t(source)], runtimeCount);
}

inline std::vector<int32_t> ExportSourceMaterialMapping(std::span<const uint32_t> mapping,
                                                       size_t sourceCount, size_t runtimeCount) {
  ValidateSourceMaterialMapping(mapping, sourceCount, runtimeCount);
  std::vector<int32_t> result;
  result.reserve(mapping.size());
  for (const auto runtime : mapping) result.push_back(static_cast<int32_t>(runtime));
  return result;
}

// Initial loaders currently append one runtime record per source material.
// Capture that interval once; later consumers use the explicit identities.
inline std::vector<uint32_t> MakeContiguousSourceMaterialMapping(size_t firstRuntime,
                                                                size_t sourceCount,
                                                                size_t runtimeCount) {
  if (sourceCount > size_t(std::numeric_limits<int32_t>::max()) ||
      firstRuntime > runtimeCount || sourceCount > runtimeCount - firstRuntime ||
      (sourceCount && (firstRuntime > size_t(std::numeric_limits<int32_t>::max()) ||
          sourceCount - 1 > size_t(std::numeric_limits<int32_t>::max()) - firstRuntime)))
    throw std::invalid_argument("Imported source material interval is outside the runtime table");
  std::vector<uint32_t> result;
  result.reserve(sourceCount);
  for (size_t source = 0; source < sourceCount; ++source)
    result.push_back(static_cast<uint32_t>(firstRuntime + source));
  return result;
}
} // namespace container::scene
