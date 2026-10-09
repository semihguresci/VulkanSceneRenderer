#pragma once

#include <cstdint>
#include <filesystem>
#include <glm/vec4.hpp>
#include <vector>

namespace container::renderer {

// CLTC v1: 16-byte little-endian header (magic, version, width, height),
// followed by exactly 64x64 row-major RGBA float32 texels. Both tables use
// this layout; shader-side channel interpretation is documented by the asset.
struct LtcLutData {
  static constexpr uint32_t extent = 64;
  std::vector<glm::vec4> texels;
};

// Reject missing, malformed, truncated, oversized and nonfinite tables before
// allocating GPU resources. Callers can then retain the sampled-light fallback.
[[nodiscard]] LtcLutData loadLtcLutData(const std::filesystem::path &path);

// Packed matrix is [m00,m02,m20,m22] with m11=1. Amplitude stores the unit
// Fresnel GGX integral, its Schlick pow5 moment, and the diffuse pow5 moment.
// Validate physical invariants before marking a loaded pair ready for LTC.
void validateLtcLutPair(const LtcLutData &matrix, const LtcLutData &amplitude);

} // namespace container::renderer
