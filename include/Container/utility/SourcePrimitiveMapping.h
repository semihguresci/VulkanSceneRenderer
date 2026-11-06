#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace container::scene {
inline void ValidateSourcePrimitiveIds(std::span<const uint32_t> ids, size_t sourceCount,
                                      uint64_t runtimeCount) {
  if (ids.size() != sourceCount || sourceCount > size_t(std::numeric_limits<int32_t>::max()))
    throw std::invalid_argument("Primitive mapping must cover every source primitive");
  for (const auto id : ids)
    if (id == std::numeric_limits<uint32_t>::max() || id >= runtimeCount)
      throw std::invalid_argument("Primitive mapping has an invalid or duplicate runtime identity");
  std::vector<uint32_t> sorted(ids.begin(), ids.end());
  std::ranges::sort(sorted);
  if (std::ranges::adjacent_find(sorted) != sorted.end())
    throw std::invalid_argument("Primitive mapping has an invalid or duplicate runtime identity");
}

struct SourcePrimitiveMapping {
  std::vector<uint32_t> sourceToRuntime;
  std::vector<int32_t> runtimeToSource; // -1 is procedural/unmapped.
};

inline SourcePrimitiveMapping MakeSourcePrimitiveMapping(size_t sourceCount, size_t runtimeCount,
    std::span<const uint32_t> sourceToRuntime = {}) {
  if (sourceCount > runtimeCount || sourceCount > size_t(std::numeric_limits<int32_t>::max()) ||
      runtimeCount >= std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("Primitive table exceeds its representable geometry range");
  SourcePrimitiveMapping result;
  if (sourceToRuntime.empty()) {
    result.sourceToRuntime.reserve(sourceCount);
    for (size_t i = 0; i < sourceCount; ++i) result.sourceToRuntime.push_back(uint32_t(i));
  } else {
    ValidateSourcePrimitiveIds(sourceToRuntime, sourceCount, runtimeCount);
    result.sourceToRuntime.assign(sourceToRuntime.begin(), sourceToRuntime.end());
  }
  result.runtimeToSource.assign(runtimeCount, -1);
  for (size_t i = 0; i < result.sourceToRuntime.size(); ++i)
    result.runtimeToSource[result.sourceToRuntime[i]] = int32_t(i);
  return result;
}

inline SourcePrimitiveMapping MakeRuntimePrimitiveMapping(size_t sourceCount, size_t runtimeCount,
    std::span<const int32_t> runtimeToSource = {}) {
  if (runtimeToSource.empty()) return MakeSourcePrimitiveMapping(sourceCount, runtimeCount);
  if (runtimeToSource.size() != runtimeCount || sourceCount > runtimeCount ||
      sourceCount > size_t(std::numeric_limits<int32_t>::max()) ||
      runtimeCount >= std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("Primitive inverse mapping must cover the runtime geometry table");
  SourcePrimitiveMapping result;
  result.sourceToRuntime.assign(sourceCount, std::numeric_limits<uint32_t>::max());
  for (size_t runtime = 0; runtime < runtimeCount; ++runtime) {
    const auto source = runtimeToSource[runtime];
    if (source == -1) continue;
    if (source < 0 || size_t(source) >= sourceCount ||
        result.sourceToRuntime[size_t(source)] != std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("Primitive inverse mapping has an invalid or duplicate source identity");
    result.sourceToRuntime[size_t(source)] = uint32_t(runtime);
  }
  ValidateSourcePrimitiveIds(result.sourceToRuntime, sourceCount, runtimeCount);
  result.runtimeToSource.assign(runtimeToSource.begin(), runtimeToSource.end());
  return result;
}
} // namespace container::scene
