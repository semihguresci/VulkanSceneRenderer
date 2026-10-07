#pragma once

#include "Container/renderer/raytracing/RayScene.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace container::renderer {

// CPU geometry and semantic visibility, before camera/Hi-Z/draw-budget culling.
// The caller advances geometryRevision only when vertex/index data changes.
struct RaySceneProviderSource {
  container::scene::SceneProviderId provider;
  uint64_t geometryRevision{0};
  std::span<const container::geometry::Vertex> vertices;
  std::span<const uint32_t> indices;
  std::span<const container::gpu::ObjectData> objects;
  std::span<const DrawCommand> draws;
};

// UV0/UV1 at each triangle corner. Matches ray_visibility_common.slang.
struct RayTriangleData {
  std::array<glm::vec4, 3> uv{};
};
struct RayInstanceData {
  glm::uvec4 info{
      0}; // first triangle, GPU material, object flags, provider kind
};
static_assert(sizeof(RayTriangleData) == 48);
static_assert(sizeof(RayInstanceData) == 16);

// Owns compact source geometry; input() constructs spans after any moves.
struct ExtractedRayScene {
  struct Geometry {
    container::scene::SceneProviderId provider;
    uint64_t id{0}, revision{0};
    std::vector<container::geometry::Vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t firstTriangle{0};
    RaySceneStorageIdentity storageIdentity{};
  };
  std::vector<Geometry> geometry;
  std::vector<RaySceneInstance> instances;
  std::vector<RayTriangleData> triangles;
  std::vector<RayInstanceData> instanceData;
  [[nodiscard]] std::vector<RaySceneGeometry> geometryViews() const;
};

struct ExtractedRayInstances {
  std::vector<RaySceneInstance> instances;
  std::vector<RayInstanceData> instanceData;
};

[[nodiscard]] ExtractedRayScene
extractRayScene(std::span<const RaySceneProviderSource> providers);

// Refresh transforms/material metadata against already compacted geometry.
// Provider storage identities, revisions and referenced ranges must still
// match the snapshot.
[[nodiscard]] ExtractedRayInstances
extractRaySceneInstances(const ExtractedRayScene &geometry,
                         std::span<const RaySceneProviderSource> providers);

// Immutable geometry snapshots can be shared by CPU updates and in-flight GPU
// generations. Only provider/geometry revisions or draw topology replace them;
// transforms, material indices and sidedness refresh the instance snapshot.
class RaySceneExtractionCache {
public:
  struct Snapshot {
    // The cached scene holds only compact geometry/triangles; instance vectors
    // are returned separately so earlier snapshots remain immutable.
    std::shared_ptr<const ExtractedRayScene> geometry;
    ExtractedRayInstances instances;
  };
  [[nodiscard]] Snapshot
  update(std::span<const RaySceneProviderSource> providers);
  void clear();

private:
  uint64_t geometrySignature_{0};
  std::shared_ptr<const ExtractedRayScene> geometry_;
};

} // namespace container::renderer
