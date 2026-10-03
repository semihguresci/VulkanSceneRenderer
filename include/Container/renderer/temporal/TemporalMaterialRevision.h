#pragma once

#include "Container/utility/SceneData.h"
#include <cstddef>
#include <cstdint>

namespace container::temporal {
// Hash shader-visible fields only. C++ alignment padding is not a material
// revision and can differ between otherwise identical upload representations.
inline uint64_t materialRevision(const container::gpu::GpuMaterial &material) {
  using Material = container::gpu::GpuMaterial;
  const auto *bytes = reinterpret_cast<const unsigned char *>(&material);
  constexpr size_t scalarEnd =
      offsetof(Material, iridescenceThicknessMaximum) + sizeof(float);
  constexpr size_t vectorBegin = offsetof(Material, specularColorFactor);
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < sizeof(Material); ++i) {
    if (i >= scalarEnd && i < vectorBegin)
      continue;
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}
} // namespace container::temporal
