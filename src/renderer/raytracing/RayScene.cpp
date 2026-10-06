#include "Container/renderer/raytracing/RayScene.h"

#include <cmath>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <limits>
#include <set>
#include <stdexcept>

namespace container::renderer {

VkTransformMatrixKHR rayInstanceTransform(const glm::mat4 &transform) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      if (!std::isfinite(transform[c][r]))
        throw std::invalid_argument("ray instance transform must be finite");
  if (transform[0][3] != 0.0f || transform[1][3] != 0.0f ||
      transform[2][3] != 0.0f || transform[3][3] != 1.0f)
    throw std::invalid_argument("ray instance transform must be affine");
  const glm::dmat3 basis(transform);
  const double volume =
      glm::length(basis[0]) * glm::length(basis[1]) * glm::length(basis[2]);
  if (volume == 0.0 || std::abs(glm::determinant(basis)) <= 1e-12 * volume)
    throw std::invalid_argument("ray instance transform must be invertible");
  VkTransformMatrixKHR result{};
  // Vulkan's AS ABI requires row-major 3x4 storage. Slang matrices continue to
  // use the engine's direct column-major upload convention.
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      result.matrix[r][c] = transform[c][r];
  return result;
}

VkGeometryInstanceFlagsKHR rayInstanceFlags(bool doubleSided) {
  // Facing is evaluated in object space. Mirrored instance transforms must not
  // flip this flag; raster's front-cull variant preserves that same local face.
  return doubleSided ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR
                     : 0;
}

void validateRayScene(const RaySceneInput &input) {
  if (input.geometries.size() > std::numeric_limits<uint32_t>::max() ||
      input.instances.size() > std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("ray scene exceeds Vulkan count limits");
  std::set<std::pair<std::string, uint64_t>> identities;
  for (const auto &geometry : input.geometries) {
    if (geometry.provider.value.empty() ||
        !identities.emplace(geometry.provider.value, geometry.geometryId)
             .second)
      throw std::invalid_argument(
          "ray geometry requires a unique provider-local identity");
    if (geometry.vertices.empty() || geometry.indices.empty() ||
        geometry.indices.size() % 3 != 0 ||
        geometry.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        geometry.indices.size() / 3 > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument(
          "ray geometry requires an indexed triangle list");
    for (const auto &vertex : geometry.vertices)
      for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(vertex.position[axis]))
          throw std::invalid_argument("ray geometry positions must be finite");
    for (uint32_t index : geometry.indices)
      if (index >= geometry.vertices.size())
        throw std::invalid_argument("ray geometry index exceeds vertex range");
  }
  for (const auto &instance : input.instances) {
    if (instance.geometryIndex >= input.geometries.size() ||
        instance.customIndex > 0xffffffu)
      throw std::invalid_argument(
          "ray instance geometry or custom index is out of range");
    (void)rayInstanceTransform(instance.transform);
  }
}

} // namespace container::renderer
