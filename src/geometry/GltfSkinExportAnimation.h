#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace container::geometry::gltf::detail {

inline std::optional<std::vector<double>> inverseSkinExportAnimationSamples(
    std::vector<double> values, std::string_view property, std::string_view interpolation,
    std::optional<std::size_t> component = std::nullopt) {
  const bool rotation = property == "rotation";
  const bool scale = property == "scale";
  if (!rotation && !scale && property != "translation") return std::nullopt;
  const bool cubic = interpolation == "CUBICSPLINE";
  const bool step = interpolation == "STEP";
  if (!cubic && !step && interpolation != "LINEAR") return std::nullopt;

  const std::size_t propertyWidth = rotation ? 4 : 3;
  if (component && *component >= propertyWidth) return std::nullopt;
  const std::size_t width = component ? 1 : propertyWidth;
  const std::size_t valueOffset = cubic ? width : 0;
  const std::size_t keyStride = cubic ? 3 * width : width;
  if (values.empty() || values.size() % keyStride != 0) return std::nullopt;
  for (const double value : values)
    if (!std::isfinite(value)) return std::nullopt;

  if (!scale) {
    if (rotation && !component) {
      for (std::size_t key = 0; key < values.size(); key += keyStride) {
        const std::size_t value = key + valueOffset;
        if (values[value] == 0 && values[value + 1] == 0 &&
            values[value + 2] == 0 && values[value + 3] == 0) return std::nullopt;
      }
    }
    // Negation and quaternion conjugation are linear, so the same sign
    // change applies to both key values and cubic tangents.
    for (std::size_t i = 0; i < values.size(); ++i)
      if (!rotation || component.value_or(i % width) != 3) values[i] = -values[i];
    return values;
  }

  // A continuous varying scale generally has no exact reciprocal glTF
  // curve. STEP values can be inverted individually. A continuous scale
  // must be constant, including its tangents in the cubic layout.
  for (std::size_t key = 0; key < values.size(); key += keyStride) {
    for (std::size_t axis = 0; axis < width; ++axis) {
      const double value = values[key + valueOffset + axis];
      if (value == 0 || (!step && value != values[valueOffset + axis])) return std::nullopt;
      if (cubic && (values[key + axis] != 0 || values[key + 2 * width + axis] != 0))
        return std::nullopt;
    }
  }
  for (std::size_t key = 0; key < values.size(); key += keyStride) {
    for (std::size_t axis = 0; axis < width; ++axis) {
      double& value = values[key + valueOffset + axis];
      value = 1.0 / value;
      if (!std::isfinite(value)) return std::nullopt;
    }
  }
  return values;
}

} // namespace container::geometry::gltf::detail
