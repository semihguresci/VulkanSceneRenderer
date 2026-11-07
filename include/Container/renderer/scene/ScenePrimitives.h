#pragma once

#include "Container/geometry/Mesh.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <glm/vec3.hpp>

namespace container::renderer {

enum class ScenePrimitiveKind : uint32_t {
  Quad = 0,
  Plane,
  Cube,
  Sphere,
  TriangularPrism,
  Pyramid,
  Torus,
};

struct ScenePrimitivePlaneDesc {
  glm::vec3 center{0.0f};
  glm::vec3 uAxis{1.0f, 0.0f, 0.0f};
  glm::vec3 vAxis{0.0f, 1.0f, 0.0f};
  float width{1.0f};
  float height{1.0f};
  int32_t materialIndex{-1};
  bool disableBackfaceCulling{false};
};

struct ScenePrimitiveSphereDesc {
  float radius{0.5f};
  uint32_t latitudeSegments{16};
  uint32_t longitudeSegments{24};
  int32_t materialIndex{-1};
};

struct ScenePrimitivePrismDesc {
  float radius{0.5f};
  float depth{1.0f};
  int32_t materialIndex{-1};
};

struct ScenePrimitivePyramidDesc {
  float baseSize{1.0f};
  float height{1.0f};
  int32_t materialIndex{-1};
};

struct ScenePrimitiveTorusDesc {
  float majorRadius{0.5f};
  float minorRadius{0.15f};
  uint32_t majorSegments{32};
  uint32_t minorSegments{12};
  int32_t materialIndex{-1};
};

[[nodiscard]] std::string_view scenePrimitiveKindLabel(
    ScenePrimitiveKind kind);

[[nodiscard]] container::geometry::Mesh makeScenePrimitive(
    ScenePrimitiveKind kind, int32_t materialIndex = -1);

// Built-in instances may share identical immutable runtime geometry. Authored
// primitives retain their source identity even when their geometry matches.
[[nodiscard]] std::optional<uint32_t> findReusableScenePrimitive(
    const container::geometry::Mesh& geometry,
    std::span<const container::geometry::Mesh> meshes,
    std::span<const int32_t> runtimeToSource);

[[nodiscard]] container::geometry::Mesh makeScenePrimitiveQuad(
    const ScenePrimitivePlaneDesc &desc = {});

[[nodiscard]] container::geometry::Mesh makeScenePrimitivePlane(
    const ScenePrimitivePlaneDesc &desc = {});

[[nodiscard]] container::geometry::Mesh makeScenePrimitiveCube(
    float size = 1.0f, int32_t materialIndex = -1);

[[nodiscard]] container::geometry::Mesh makeScenePrimitiveSphere(
    const ScenePrimitiveSphereDesc &desc = {});

[[nodiscard]] container::geometry::Mesh makeScenePrimitiveTriangularPrism(
    const ScenePrimitivePrismDesc &desc = {});

[[nodiscard]] container::geometry::Mesh makeScenePrimitivePyramid(
    const ScenePrimitivePyramidDesc &desc = {});

[[nodiscard]] container::geometry::Mesh makeScenePrimitiveTorus(
    const ScenePrimitiveTorusDesc &desc = {});

} // namespace container::renderer
