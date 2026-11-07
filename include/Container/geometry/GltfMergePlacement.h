#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace container::geometry::gltf {

// glTF-native column-major affine placement. Keep the authored doubles, but
// reject bases that the renderer cannot safely invert after binary32 upload.
// Shared by the detached C++ planner and the capture-request parser.
inline void ValidateGltfMergePlacement(const std::array<double, 16>& matrix) {
  std::array<float, 16> runtime{};
  for (std::size_t i = 0; i < matrix.size(); ++i) {
    if (!std::isfinite(matrix[i]) ||
        std::abs(matrix[i]) > std::numeric_limits<float>::max())
      throw std::invalid_argument("glTF merge placement: values must be finite and representable in binary32");
    runtime[i] = static_cast<float>(matrix[i]);
  }
  if (matrix[3] != 0 || matrix[7] != 0 || matrix[11] != 0 || matrix[15] != 1)
    throw std::invalid_argument("glTF merge placement: expected a column-major affine matrix");
  const auto determinant = [](const auto& m) {
    return m[0] * (m[5] * m[10] - m[9] * m[6]) -
           m[4] * (m[1] * m[10] - m[9] * m[2]) +
           m[8] * (m[1] * m[6] - m[5] * m[2]);
  };
  const double authoredDeterminant = determinant(matrix);
  const float runtimeDeterminant = determinant(runtime);
  if (!std::isfinite(authoredDeterminant) || std::abs(authoredDeterminant) <= 1e-12 ||
      !std::isfinite(runtimeDeterminant) || std::abs(double(runtimeDeterminant)) <= 1e-12)
    throw std::invalid_argument("glTF merge placement: basis must remain finite and nonsingular in binary32 (|det| > 1e-12)");
}

} // namespace container::geometry::gltf
