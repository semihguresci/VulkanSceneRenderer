#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace container::geometry::gltf::detail {

// glTF node matrices must be decomposable to TRS. Orthogonalize the linear
// columns by right multiplication, then keep that rotation as a second node.
[[nodiscard]] inline std::optional<std::array<glm::dmat4, 2>> factorSkinExportAffine(
    const glm::dmat4& input) {
  constexpr double floatLimit = std::numeric_limits<float>::max();
  constexpr double epsilon = std::numeric_limits<double>::epsilon();
  constexpr double orthogonalityTolerance = 128.0 * epsilon;

  const auto fitsFloat = [floatLimit](const glm::dmat4& matrix) {
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        const double value = matrix[column][row];
        if (!std::isfinite(value) || std::abs(value) > floatLimit) return false;
      }
    }
    return true;
  };
  if (input[0].w != 0.0 || input[1].w != 0.0 || input[2].w != 0.0 || input[3].w != 1.0 ||
      !fitsFloat(input)) return std::nullopt;
  const glm::mat4 roundedInput(input);
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 3; ++row)
      if (input[column][row] != 0.0 && roundedInput[column][row] == 0.0f) return std::nullopt;

  constexpr std::array pairs{std::array{0, 1}, std::array{0, 2}, std::array{1, 2}};
  const auto orthogonal = [&pairs, orthogonalityTolerance](const glm::dmat3& matrix) {
    for (const auto& [first, second] : pairs) {
      const auto& a = matrix[first];
      const auto& b = matrix[second];
      if (std::abs(glm::dot(a, b)) > orthogonalityTolerance * glm::length(a) * glm::length(b))
        return false;
    }
    return true;
  };

  glm::dmat3 columns(input);
  if (orthogonal(columns)) return std::array{input, glm::dmat4(1.0)};

  // One-sided Jacobi gives A * V with orthogonal columns and V orthogonal.
  // Fixed pair order makes the choice deterministic for repeated eigenvalues.
  glm::dmat3 rotation(1.0);
  for (int sweep = 0; sweep < 24; ++sweep) {
    bool rotated = false;
    for (const auto& [first, second] : pairs) {
      const auto a = columns[first];
      const auto b = columns[second];
      const double aa = glm::dot(a, a);
      const double bb = glm::dot(b, b);
      const double ab = glm::dot(a, b);
      if (std::abs(ab) <= 16.0 * epsilon * std::sqrt(aa) * std::sqrt(bb)) continue;
      const double delta = 0.5 * (bb - aa);
      const double tangent = ab / (delta + std::copysign(std::hypot(delta, ab), delta));
      const double cosine = 1.0 / std::hypot(1.0, tangent);
      const double sine = cosine * tangent;
      columns[first] = cosine * a - sine * b;
      columns[second] = sine * a + cosine * b;
      const auto u = rotation[first];
      const auto v = rotation[second];
      rotation[first] = cosine * u - sine * v;
      rotation[second] = sine * u + cosine * v;
      rotated = true;
    }
    if (!rotated) break;
  }

  // A rank-deficient matrix may leave roundoff in a numerically zero column.
  // Drop only those residuals and check that neither reconstruction loses a
  // small but meaningful original axis.
  if (!orthogonal(columns)) {
    const double maxLength = std::max({glm::length(columns[0]), glm::length(columns[1]),
                                      glm::length(columns[2])});
    for (int column = 0; column < 3; ++column)
      if (glm::length(columns[column]) <= orthogonalityTolerance * maxLength)
        columns[column] = glm::dvec3(0.0);
  }
  const auto rightLinear = glm::transpose(rotation);
  if (!orthogonal(columns) || !orthogonal(rightLinear)) return std::nullopt;

  glm::dmat4 left(columns);
  left[3] = input[3];
  const glm::dmat4 right(rightLinear);
  if (!fitsFloat(left) || !fitsFloat(right)) return std::nullopt;
  const auto reconstructs = [](const glm::dmat4& expected, const glm::dmat4& actual, double tolerance) {
    for (int column = 0; column < 4; ++column) {
      const auto& axis = expected[column];
      const double scale = std::max({std::abs(axis.x), std::abs(axis.y), std::abs(axis.z), std::abs(axis.w)});
      for (int row = 0; row < 4; ++row) {
        const double error = std::abs(actual[column][row] - axis[row]);
        if (!std::isfinite(error) || error > tolerance * scale) return false;
      }
    }
    return true;
  };
  if (!reconstructs(input, left * right, 4096.0 * epsilon) ||
      !reconstructs(glm::dmat4(roundedInput), glm::dmat4(glm::mat4(left) * glm::mat4(right)),
                    16.0 * std::numeric_limits<float>::epsilon())) return std::nullopt;
  return std::array{left, right};
}

}  // namespace container::geometry::gltf::detail
