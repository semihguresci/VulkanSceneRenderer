#pragma once

#include "Container/geometry/Vertex.h"
#include "Container/scene/SceneAssetIdentity.h"
#include "Container/scene/SceneProvider.h"

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace container::geometry::gltf { struct GltfOpacityMicromapAttachment; }

namespace container::renderer {

// An issued asset ID distinguishes replacement content even when the source
// allocator reuses addresses and the provider's revision restarts. Untagged
// sources use their backing addresses as a compatibility key.
struct RaySceneStorageIdentity {
  uintptr_t vertices{0}, indices{0};
  container::scene::SceneAssetId assetId{};
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
  // Immutable, active only when this geometry's alpha coverage still matches.
  std::shared_ptr<const container::geometry::gltf::GltfOpacityMicromapAttachment> opacityMicromap;
};

struct RaySceneInstance {
  uint32_t geometryIndex{0};
  uint32_t customIndex{0}; // 24-bit shader lookup into the consumer's metadata.
  glm::mat4 transform{1.0f};
  bool doubleSided{false};
  uint8_t mask{0xff};
  // Local vertex deformation can reverse authored winding independently of
  // the instance model transform, whose determinant never sets this flag.
  bool flipFacing{false};
  uint32_t gpuObjectIndex{UINT32_MAX};
  uint32_t sourceObjectIndex{UINT32_MAX};
};

struct RaySceneGpuTransforms;
struct RaySceneInput {
  std::span<const RaySceneGeometry> geometries{};
  std::span<const RaySceneInstance> instances{};
  const RaySceneGpuTransforms* gpuTransforms{};
};

// Validate before allocating resources or touching command buffers.
void validateRayScene(const RaySceneInput &input);
[[nodiscard]] VkTransformMatrixKHR
rayInstanceTransform(const glm::mat4 &transform);
[[nodiscard]] VkGeometryInstanceFlagsKHR rayInstanceFlags(
    bool doubleSided, bool flipFacing = false);

} // namespace container::renderer
