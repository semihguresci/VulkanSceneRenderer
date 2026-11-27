#pragma once

#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/RoboticsScene.h"

#include <glm/vec4.hpp>

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace container::robotics::detail {

struct OpenUsdMaterial {
  glm::vec4 color{1.0f};
  glm::vec3 emissive{0.0f};
  float metallic{};
  float roughness{1.0f};
  float ior{1.5f};
  std::optional<float> opacityThreshold;
};

struct OpenUsdMesh {
  std::string representationId;
  uint32_t sourceMeshId{};
  glm::dmat4 parentFromMesh{1.0};
  bool doubleSided{};
  OpenUsdMaterial material;
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> normals;
  std::vector<uint32_t> indices;
  // Final color and alpha after combining the vertex and material factors.
  // An empty stream uses the corresponding constant from the material.
  std::vector<glm::vec3> displayColors;
  std::vector<float> displayOpacities;
};

struct OpenUsdEntity {
  std::string primName;
  std::string primPath;
  std::vector<size_t> children;
  glm::dmat4 worldFromLocal{1.0};
  glm::dmat4 parentFromLocal{1.0};
  std::vector<OpenUsdMesh> meshes;
};

struct OpenUsdData {
  RoboticsScene scene;
  // Aligned with scene.entities.
  std::vector<OpenUsdEntity> entities;
  std::vector<size_t> roots;
  std::vector<OpenUsdMesh> standaloneMeshes;
  std::vector<BimCollider> colliders;
  std::vector<RoboticsDiagnostic> diagnostics;
};

void appendOpenUsdMeshes(const geometry::dotbim::Model& model, OpenUsdData& data);
void writeOpenUsdLayer(std::ostream& out, const OpenUsdData& data);

} // namespace container::robotics::detail
