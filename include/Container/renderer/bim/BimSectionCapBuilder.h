#pragma once

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/geometry/Vertex.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace container::renderer {

inline constexpr uint32_t kInvalidBimSectionCapMaterialIndex =
    std::numeric_limits<uint32_t>::max();

struct BimSectionCapMaterialStyle {
  uint32_t materialIndex{kInvalidBimSectionCapMaterialIndex};
  glm::vec3 fillColor{0.06f, 0.08f, 0.10f};
  float fillOpacity{0.82f};
  float hatchSpacing{0.25f};
  float hatchAngleRadians{0.7853982f};
  glm::vec3 hatchColor{0.08f};
};

struct BimSectionCapDrawStyle {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t materialIndex{kInvalidBimSectionCapMaterialIndex};
  glm::vec3 fillColor{0.06f, 0.08f, 0.10f};
  float fillOpacity{0.82f};
  float hatchSpacing{0.25f};
  float hatchAngleRadians{0.7853982f};
  glm::vec3 hatchColor{0.08f};
  float lineWidth{0.0f};
};

struct BimSectionMarkerLine {
  glm::vec3 a{0.0f};
  glm::vec3 b{0.0f};
  glm::vec3 color{0.95f, 0.62f, 0.12f};
  float lineWidth{2.0f};
  bool startArrow{false};
  bool endArrow{false};
  uint32_t sectionPlaneIndex{0};
};

struct BimSectionCapTriangle {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t materialIndex{kInvalidBimSectionCapMaterialIndex};
  glm::vec3 p0{0.0f};
  glm::vec3 p1{0.0f};
  glm::vec3 p2{0.0f};
};

inline constexpr size_t kBimSectionCapMaxPlanes = 7u;

struct BimSectionCapBuildOptions {
  glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  uint32_t clipPlaneCount{0};
  std::array<glm::vec4, kBimSectionCapMaxPlanes> clipPlanes{};
  bool invertedBoxClip{false};
  glm::vec3 fillColor{0.06f, 0.08f, 0.10f};
  float fillOpacity{0.82f};
  glm::vec3 hatchColor{0.08f, 0.08f, 0.08f};
  float hatchSpacing{0.25f};
  float hatchAngleRadians{0.7853982f};
  float capOffset{0.001f};
  bool crossHatch{false};
  std::vector<BimSectionCapMaterialStyle> materialStyles{};
  bool sectionMarkersEnabled{true};
  glm::vec3 sectionMarkerColor{0.95f, 0.62f, 0.12f};
  float sectionMarkerLineWidth{2.0f};
};

bool appendBimSectionCapClipPlane(BimSectionCapBuildOptions& options,
                                  glm::vec4 plane);

struct BimSectionCapGeneratedMesh {
  std::vector<container::geometry::Vertex> vertices{};
  std::vector<uint32_t> indices{};
  std::vector<DrawCommand> fillDrawCommands{};
  std::vector<DrawCommand> hatchDrawCommands{};
  std::vector<BimSectionCapDrawStyle> fillDrawStyles{};
  std::vector<BimSectionCapDrawStyle> hatchDrawStyles{};
  std::vector<BimSectionMarkerLine> sectionMarkerLines{};

  [[nodiscard]] bool valid() const {
    return !vertices.empty() && !indices.empty() &&
           (!fillDrawCommands.empty() || !hatchDrawCommands.empty());
  }
};

struct BimSectionClipCapDrawData {
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  std::vector<DrawCommand> fillDrawCommands{};
  std::vector<DrawCommand> hatchDrawCommands{};
  std::vector<BimSectionCapDrawStyle> fillDrawStyles{};
  std::vector<BimSectionCapDrawStyle> hatchDrawStyles{};
  std::vector<BimSectionMarkerLine> sectionMarkerLines{};

  [[nodiscard]] bool valid() const {
    return vertexSlice.buffer != VK_NULL_HANDLE &&
           indexSlice.buffer != VK_NULL_HANDLE &&
           (!fillDrawCommands.empty() || !hatchDrawCommands.empty());
  }
};

class BimSectionCapBuilder {
 public:
  [[nodiscard]] BimSectionCapGeneratedMesh build(
      std::span<const BimSectionCapTriangle> triangles,
      const BimSectionCapBuildOptions& options) const;
};

[[nodiscard]] glm::vec4 normalizedSectionCapPlane(glm::vec4 plane);

[[nodiscard]] BimSectionCapGeneratedMesh BuildBimSectionCapMesh(
    std::span<const BimSectionCapTriangle> triangles,
    const BimSectionCapBuildOptions& options);

}  // namespace container::renderer
