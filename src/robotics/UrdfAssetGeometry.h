#pragma once

#include "Container/robotics/UrdfModel.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace container::robotics::detail {

struct UrdfAssetGeometry {
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> normals;
  std::vector<uint32_t> indices;
};

// Geometry stays in the authored shape frame. The occurrence owns the origin
// and signed mesh scale independently of these vertices.
[[nodiscard]] UrdfAssetGeometry loadUrdfAssetGeometry(
    const UrdfGeometry& shape, const std::filesystem::path& urdfPath,
    std::span<const std::filesystem::path> packageRoots);

} // namespace container::robotics::detail
