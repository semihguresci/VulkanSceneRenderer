#pragma once

#include "Container/renderer/raytracing/RayScene.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace container::geometry::gltf { struct GltfOpacityMicromapAttachment; }

namespace container::renderer {

struct RayOpacityMicromapRange {
  uint32_t firstIndex{}, indexCount{};
  std::shared_ptr<const container::geometry::gltf::GltfOpacityMicromapAttachment> attachment;
};

// CPU geometry and semantic visibility, before camera/Hi-Z/draw-budget culling.
// The caller advances geometryRevision only when vertex/index data changes.
struct RaySceneProviderSource {
  container::scene::SceneProviderId provider;
  uint64_t geometryRevision{0};
  std::span<const container::geometry::Vertex> vertices;
  std::span<const uint32_t> indices;
  std::span<const container::gpu::ObjectData> objects;
  std::span<const DrawCommand> draws;
  // Optional GPU model stream, reconciled to these CPU objects before AS build.
  container::gpu::BufferSlice gpuObjects{};
  uint64_t gpuTransformRevision{};
  // Sorted, unique draw ranges whose current material matches baked coverage.
  std::span<const RayOpacityMicromapRange> opacityMicromaps{};
};

// UV0/UV1 and COLOR_0 alpha at each corner. Matches ray_scene_data.slang.
struct RayTriangleData {
  std::array<glm::vec4, 3> uv{};
  glm::vec4 vertexAlpha{1.0f}; // xyz = triangle corners; w reserved
};
struct RayInstanceData {
  glm::uvec4 info{
      0}; // first triangle, GPU material, object flags, triangle count
};
static_assert(sizeof(RayTriangleData) == 64);
static_assert(sizeof(RayInstanceData) == 16);
static_assert(offsetof(RayTriangleData, uv) == 0);
static_assert(offsetof(RayTriangleData, vertexAlpha) == 48);
static_assert(offsetof(RayInstanceData, info) == 0);

// Optional closest-hit attributes, separate from the small shadow metadata.
// Matches ray_surface_common.slang; all attributes remain in object space.
struct RaySurfaceTriangleData {
  alignas(16) std::array<glm::vec4, 3> positions{};
  alignas(16) std::array<glm::vec4, 3> normals{};
  alignas(16) std::array<glm::vec4, 3> tangents{};
  alignas(16) std::array<glm::vec4, 3> colors{};
};
struct RaySurfaceInstanceData {
  alignas(16) glm::mat4 model{1.0f};
  // Inverse-transpose columns. Column zero's w stores model handedness (+/-1).
  alignas(16) std::array<glm::vec4, 3> normalColumns{
      glm::vec4(1, 0, 0, 1), glm::vec4(0, 1, 0, 0), glm::vec4(0, 0, 1, 0)};
};
static_assert(sizeof(RaySurfaceTriangleData) == 192);
static_assert(offsetof(RaySurfaceTriangleData, positions) == 0);
static_assert(offsetof(RaySurfaceTriangleData, normals) == 48);
static_assert(offsetof(RaySurfaceTriangleData, tangents) == 96);
static_assert(offsetof(RaySurfaceTriangleData, colors) == 144);
static_assert(sizeof(RaySurfaceInstanceData) == 112);
static_assert(offsetof(RaySurfaceInstanceData, model) == 0);
static_assert(offsetof(RaySurfaceInstanceData, normalColumns) == 64);

struct RaySceneExtractionOptions {
  bool includeSurfaceData{false};
};

// Pure attribute reconstruction, independent of candidate acceptance/material
// evaluation. Normals are canonical and unflipped; frontFace remains explicit
// for probe relocation and consumer-specific sidedness policy. The geometric
// normal uses authored object winding, including under mirrored instances.
struct RaySurfacePoint {
  glm::vec3 position{};
  glm::vec3 geometricNormal{};
  glm::vec3 shadingNormal{};
  glm::vec4 tangent{};
  glm::vec3 bitangent{};
  glm::vec3 color{};
  float colorAlpha{};
  glm::vec4 uv{};
  uint32_t materialIndex{0}, objectFlags{0};
  bool frontFace{true};
};
[[nodiscard]] std::optional<RaySurfacePoint> reconstructRaySurface(
    const RayTriangleData &uv, const RaySurfaceTriangleData &triangle,
    const RayInstanceData &metadata, const RaySurfaceInstanceData &instance,
    glm::vec2 barycentrics, bool frontFace);

// Owns compact source geometry; input() constructs spans after any moves.
struct ExtractedRayScene {
  struct Geometry {
    container::scene::SceneProviderId provider;
    uint64_t id{0}, revision{0};
    std::vector<container::geometry::Vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t firstTriangle{0};
    RaySceneStorageIdentity storageIdentity{};
    std::shared_ptr<const container::geometry::gltf::GltfOpacityMicromapAttachment> opacityMicromap;
  };
  std::vector<Geometry> geometry;
  std::vector<RaySceneInstance> instances;
  std::vector<RayTriangleData> triangles;
  std::vector<RayInstanceData> instanceData;
  bool hasSurfaceData{false};
  std::vector<RaySurfaceTriangleData> surfaceTriangles;
  std::vector<RaySurfaceInstanceData> surfaceInstanceData;
  [[nodiscard]] std::vector<RaySceneGeometry> geometryViews() const;
};

struct ExtractedRayInstances {
  std::vector<RaySceneInstance> instances;
  std::vector<RayInstanceData> instanceData;
  std::vector<RaySurfaceInstanceData> surfaceInstanceData;
};

[[nodiscard]] ExtractedRayScene
extractRayScene(std::span<const RaySceneProviderSource> providers,
                RaySceneExtractionOptions options = {});

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
  update(std::span<const RaySceneProviderSource> providers,
         RaySceneExtractionOptions options = {});
  void clear();

private:
  uint64_t geometrySignature_{0};
  std::shared_ptr<const ExtractedRayScene> geometry_;
};

} // namespace container::renderer
