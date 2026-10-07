#pragma once

#include "Container/geometry/Vertex.h"
#include "Container/scene/SceneProvider.h"

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <span>
#include <string>
#include <vector>

namespace container::renderer {

// Source buffers distinguish a replacement provider whose geometry revision
// restarts. Keep this identity local to each provider so unrelated providers
// remain eligible for BLAS reuse. Direct backend callers may leave it zero
// when their revisions already uniquely identify all geometry changes.
struct RaySceneStorageIdentity {
  uintptr_t vertices{0}, indices{0};
  bool operator==(const RaySceneStorageIdentity &) const = default;
};

// Provider-local stable identity plus revision. Views need only survive the
// build call; the resulting generation owns its GPU copies of the geometry.
struct RaySceneGeometry {
  container::scene::SceneProviderId provider{};
  uint64_t geometryId{0};
  uint64_t revision{0};
  std::span<const container::geometry::Vertex> vertices{};
  std::span<const uint32_t> indices{};
  bool opaque{true}; // Alpha-tested geometry must remain non-opaque.
  RaySceneStorageIdentity storageIdentity{};
};

struct RaySceneInstance {
  uint32_t geometryIndex{0};
  uint32_t customIndex{0}; // 24-bit shader lookup into the consumer's metadata.
  glm::mat4 transform{1.0f};
  bool doubleSided{false};
  uint8_t mask{0xff};
};

struct RaySceneInput {
  std::span<const RaySceneGeometry> geometries{};
  std::span<const RaySceneInstance> instances{};
};

// Validate before allocating resources or touching command buffers.
void validateRayScene(const RaySceneInput &input);
[[nodiscard]] VkTransformMatrixKHR
rayInstanceTransform(const glm::mat4 &transform);
[[nodiscard]] VkGeometryInstanceFlagsKHR rayInstanceFlags(bool doubleSided);

} // namespace container::renderer
