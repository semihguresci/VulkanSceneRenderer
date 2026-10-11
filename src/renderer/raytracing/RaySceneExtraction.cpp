#include "Container/renderer/raytracing/RaySceneExtraction.h"

#include <cmath>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace container::renderer {
namespace {

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}
bool finite(glm::vec4 value) {
  return finite(glm::vec3(value)) && std::isfinite(value.w);
}
glm::vec3 surfaceNormalize(glm::vec3 value, glm::vec3 fallback) {
  if (!finite(value))
    return fallback;
  const float scale = glm::max(glm::max(glm::abs(value.x), glm::abs(value.y)),
                               glm::abs(value.z));
  if (scale <= 0.0f)
    return fallback;
  value /= scale;
  return value / glm::length(value);
}
glm::vec3 surfaceFallbackTangent(glm::vec3 normal) {
  const glm::vec3 axis =
      std::abs(normal.y) < 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  return surfaceNormalize(glm::cross(axis, normal), glm::vec3(1, 0, 0));
}
void validateSurfaceVertex(const container::geometry::Vertex &vertex) {
  if (!finite(vertex.position) || !finite(vertex.normal) ||
      !finite(vertex.tangent) || !finite(vertex.color) ||
      !finite(glm::vec4(vertex.texCoord, vertex.texCoord1)))
    throw std::invalid_argument("ray surface attributes must be finite");
}
RaySurfaceInstanceData surfaceInstance(const glm::mat4 &model) {
  // Existing validation checks finite, affine, invertible model transforms.
  // Derive in double precision, then reject an unrepresentable float normal
  // transform before any consumer can upload the optional metadata.
  const glm::dmat3 basis(model);
  const glm::dmat3 normal = glm::transpose(glm::inverse(basis));
  RaySurfaceInstanceData result;
  result.model = model;
  for (uint32_t i = 0; i < 3; ++i) {
    result.normalColumns[i] = glm::vec4(glm::vec3(normal[i]), 0.0f);
    if (!finite(result.normalColumns[i]))
      throw std::invalid_argument(
          "ray surface normal transform must be finite");
  }
  result.normalColumns[0].w = glm::determinant(basis) < 0.0 ? -1.0f : 1.0f;
  return result;
}

void validateProvider(const RaySceneProviderSource &provider,
                      std::set<std::string> &identities) {
  if (provider.provider.value.empty() ||
      !identities.insert(provider.provider.value).second)
    throw std::invalid_argument("invalid or duplicate ray scene provider");
}

void validateDraw(const RaySceneProviderSource &provider,
                  const DrawCommand &draw) {
  if (draw.indexCount % 3 != 0 || draw.firstIndex > provider.indices.size() ||
      draw.indexCount > provider.indices.size() - draw.firstIndex ||
      draw.objectIndex > provider.objects.size() ||
      draw.instanceCount > provider.objects.size() - draw.objectIndex)
    throw std::invalid_argument("ray provider draw range is out of bounds");
}

uint64_t geometryId(const DrawCommand &draw) {
  return (uint64_t(draw.firstIndex) << 32) | draw.indexCount;
}

void appendInstances(std::vector<RaySceneInstance> &instances,
                     std::vector<RayInstanceData> &metadata,
                     const RaySceneProviderSource &provider,
                     const DrawCommand &draw, uint32_t geometryIndex,
                     uint32_t firstTriangle,
                     std::vector<RaySurfaceInstanceData> *surfaces = nullptr) {
  for (uint32_t i = 0; i < draw.instanceCount; ++i) {
    if (instances.size() >= 0x1000000u)
      throw std::invalid_argument("ray instance metadata exceeds 24 bits");
    const auto &object = provider.objects[draw.objectIndex + i];
    (void)rayInstanceTransform(object.model);
    const auto customIndex = static_cast<uint32_t>(instances.size());
    instances.push_back({geometryIndex, customIndex, object.model,
                         (object.objectInfo.y & 4u) != 0u});
    metadata.push_back({glm::uvec4(firstTriangle, object.objectInfo.x,
                                   object.objectInfo.y, 0u)});
    if (surfaces)
      surfaces->push_back(surfaceInstance(object.model));
  }
}

uint64_t geometrySignature(std::span<const RaySceneProviderSource> providers,
                           RaySceneExtractionOptions options) {
  uint64_t hash = 1469598103934665603ull;
  auto bytes = [&](const void *data, size_t count) {
    const auto *value = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < count; ++i)
      hash = (hash ^ value[i]) * 1099511628211ull;
  };
  auto value = [&](const auto &item) { bytes(&item, sizeof(item)); };
  value(options.includeSurfaceData);
  value(providers.size());
  for (const auto &provider : providers) {
    value(provider.provider.value.size());
    bytes(provider.provider.value.data(), provider.provider.value.size());
    value(provider.geometryRevision);
    value(provider.vertices.size());
    value(provider.indices.size());
    // A replacement provider may restart its revision counter. Its source
    // storage identity must not accidentally reuse the previous compact copy.
    value(reinterpret_cast<uintptr_t>(provider.vertices.data()));
    value(reinterpret_cast<uintptr_t>(provider.indices.data()));
    value(provider.draws.size());
    for (const auto &draw : provider.draws) {
      value(draw.objectIndex);
      value(draw.firstIndex);
      value(draw.indexCount);
      value(draw.instanceCount);
    }
  }
  return hash;
}

} // namespace

std::optional<RaySurfacePoint> reconstructRaySurface(
    const RayTriangleData &uv, const RaySurfaceTriangleData &triangle,
    const RayInstanceData &metadata, const RaySurfaceInstanceData &instance,
    glm::vec2 barycentrics, bool frontFace) {
  for (uint32_t column = 0; column < 4; ++column)
    if (!finite(instance.model[column]))
      return std::nullopt;
  for (const auto &column : instance.normalColumns)
    if (!finite(column))
      return std::nullopt;
  if (!std::isfinite(barycentrics.x) || !std::isfinite(barycentrics.y) ||
      barycentrics.x < 0.0f || barycentrics.y < 0.0f ||
      barycentrics.x + barycentrics.y > 1.0f)
    return std::nullopt;
  const glm::vec3 weights(1.0f - barycentrics.x - barycentrics.y,
                          barycentrics.x, barycentrics.y);
  const glm::mat3 normalTransform(glm::vec3(instance.normalColumns[0]),
                                  glm::vec3(instance.normalColumns[1]),
                                  glm::vec3(instance.normalColumns[2]));
  const glm::vec3 edge0 = surfaceNormalize(
      glm::vec3(triangle.positions[1] - triangle.positions[0]), glm::vec3(0));
  const glm::vec3 edge1 = surfaceNormalize(
      glm::vec3(triangle.positions[2] - triangle.positions[0]), glm::vec3(0));
  const glm::vec3 objectGeometricNormal =
      surfaceNormalize(glm::cross(edge0, edge1), glm::vec3(0));
  if (glm::dot(objectGeometricNormal, objectGeometricNormal) == 0.0f)
    return std::nullopt;

  RaySurfacePoint result;
  result.geometricNormal =
      surfaceNormalize(normalTransform * objectGeometricNormal, glm::vec3(0));
  if (glm::dot(result.geometricNormal, result.geometricNormal) == 0.0f)
    return std::nullopt;
  glm::vec3 objectPosition(0), worldNormal(0), worldTangent(0);
  float tangentHandedness = 0.0f;
  for (uint32_t i = 0; i < 3; ++i) {
    if (!finite(triangle.positions[i]) || !finite(triangle.normals[i]) ||
        !finite(triangle.tangents[i]) || !finite(triangle.colors[i]) ||
        !finite(uv.uv[i]))
      return std::nullopt;
    objectPosition += glm::vec3(triangle.positions[i]) * weights[i];
    // Raster transforms/normalizes at vertices before interpolation. Retain
    // that ordering under nonuniform scales rather than normalizing just once.
    const glm::vec3 normal =
        surfaceNormalize(normalTransform * glm::vec3(triangle.normals[i]),
                         result.geometricNormal);
    glm::vec3 tangent =
        glm::mat3(instance.model) * glm::vec3(triangle.tangents[i]);
    tangent -= normal * glm::dot(normal, tangent);
    tangent = surfaceNormalize(tangent, surfaceFallbackTangent(normal));
    float handedness =
        std::abs(triangle.tangents[i].w) > 0.5f ? triangle.tangents[i].w : 1.0f;
    handedness *= instance.normalColumns[0].w;
    handedness = handedness < 0.0f ? -1.0f : 1.0f;
    worldNormal += normal * weights[i];
    worldTangent += tangent * weights[i];
    tangentHandedness += handedness * weights[i];
    result.color += glm::vec3(triangle.colors[i]) * weights[i];
    result.uv += uv.uv[i] * weights[i];
  }
  result.position = glm::vec3(instance.model * glm::vec4(objectPosition, 1));
  result.shadingNormal = surfaceNormalize(worldNormal, result.geometricNormal);
  worldTangent -=
      result.shadingNormal * glm::dot(result.shadingNormal, worldTangent);
  worldTangent = surfaceNormalize(worldTangent,
                                  surfaceFallbackTangent(result.shadingNormal));
  const float sign =
      std::abs(tangentHandedness) > 0.5f && tangentHandedness < 0.0f ? -1.0f
                                                                     : 1.0f;
  result.tangent = glm::vec4(worldTangent, sign);
  result.bitangent = surfaceNormalize(
      glm::cross(result.shadingNormal, worldTangent) * sign, glm::vec3(0));
  result.materialIndex = metadata.info.y;
  result.objectFlags = metadata.info.z;
  result.frontFace = frontFace;
  if (!finite(result.position) || !finite(result.geometricNormal) ||
      !finite(result.shadingNormal) || !finite(result.tangent) ||
      !finite(result.bitangent) || !finite(result.color) || !finite(result.uv))
    return std::nullopt;
  return result;
}

std::vector<RaySceneGeometry> ExtractedRayScene::geometryViews() const {
  std::vector<RaySceneGeometry> views;
  views.reserve(geometry.size());
  for (const auto &source : geometry)
    views.push_back({source.provider, source.id, source.revision,
                     source.vertices, source.indices, false,
                     source.storageIdentity});
  // Candidates remain non-opaque even for solid materials: section planes and
  // box clipping must be evaluated at the ray hit rather than at the instance.
  return views;
}

ExtractedRayScene
extractRayScene(std::span<const RaySceneProviderSource> providers,
                RaySceneExtractionOptions options) {
  ExtractedRayScene scene;
  scene.hasSurfaceData = options.includeSurfaceData;
  std::set<std::string> identities;
  for (const auto &provider : providers) {
    validateProvider(provider, identities);
    std::map<uint64_t, uint32_t> ranges;
    for (const auto &draw : provider.draws) {
      if (draw.indexCount == 0 || draw.instanceCount == 0)
        continue;
      validateDraw(provider, draw);
      const uint64_t id = geometryId(draw);
      auto found = ranges.find(id);
      uint32_t geometryIndex;
      if (found != ranges.end()) {
        geometryIndex = found->second;
      } else {
        if (scene.geometry.size() >= std::numeric_limits<uint32_t>::max() ||
            scene.triangles.size() + draw.indexCount / 3ull >
                std::numeric_limits<uint32_t>::max())
          throw std::invalid_argument("ray provider geometry is too large");
        geometryIndex = static_cast<uint32_t>(scene.geometry.size());
        ranges.emplace(id, geometryIndex);
        auto &geometry = scene.geometry.emplace_back();
        geometry.provider = provider.provider;
        geometry.id = id;
        geometry.revision = provider.geometryRevision;
        geometry.storageIdentity = {
            reinterpret_cast<uintptr_t>(provider.vertices.data()),
            reinterpret_cast<uintptr_t>(provider.indices.data())};
        geometry.firstTriangle = static_cast<uint32_t>(scene.triangles.size());
        std::unordered_map<uint32_t, uint32_t> remap;
        geometry.indices.reserve(draw.indexCount);
        for (uint32_t i = 0; i < draw.indexCount; ++i) {
          const uint32_t sourceIndex = provider.indices[draw.firstIndex + i];
          if (sourceIndex >= provider.vertices.size())
            throw std::invalid_argument("ray provider vertex is out of bounds");
          auto [entry, inserted] = remap.try_emplace(
              sourceIndex, static_cast<uint32_t>(geometry.vertices.size()));
          if (inserted)
            geometry.vertices.push_back(provider.vertices[sourceIndex]);
          geometry.indices.push_back(entry->second);
          if (i % 3 == 0)
            scene.triangles.emplace_back();
          const auto &vertex = provider.vertices[sourceIndex];
          scene.triangles.back().uv[i % 3] =
              glm::vec4(vertex.texCoord, vertex.texCoord1);
          if (options.includeSurfaceData) {
            validateSurfaceVertex(vertex);
            if (i % 3 == 0)
              scene.surfaceTriangles.emplace_back();
            auto &surface = scene.surfaceTriangles.back();
            surface.positions[i % 3] = glm::vec4(vertex.position, 1.0f);
            surface.normals[i % 3] = glm::vec4(vertex.normal, 0.0f);
            surface.tangents[i % 3] = vertex.tangent;
            surface.colors[i % 3] = glm::vec4(vertex.color, 1.0f);
          }
        }
      }
      appendInstances(
          scene.instances, scene.instanceData, provider, draw, geometryIndex,
          scene.geometry[geometryIndex].firstTriangle,
          options.includeSurfaceData ? &scene.surfaceInstanceData : nullptr);
    }
  }
  const auto views = scene.geometryViews();
  validateRayScene({views, scene.instances});
  return scene;
}

ExtractedRayInstances
extractRaySceneInstances(const ExtractedRayScene &geometry,
                         std::span<const RaySceneProviderSource> providers) {
  std::map<std::pair<std::string, uint64_t>, uint32_t> ranges;
  for (uint32_t i = 0; i < geometry.geometry.size(); ++i) {
    const auto &source = geometry.geometry[i];
    ranges.emplace(std::make_pair(source.provider.value, source.id), i);
  }
  ExtractedRayInstances snapshot;
  std::set<std::string> identities;
  for (const auto &provider : providers) {
    validateProvider(provider, identities);
    for (const auto &draw : provider.draws) {
      if (draw.indexCount == 0 || draw.instanceCount == 0)
        continue;
      validateDraw(provider, draw);
      const auto found =
          ranges.find({provider.provider.value, geometryId(draw)});
      const RaySceneStorageIdentity storage{
          reinterpret_cast<uintptr_t>(provider.vertices.data()),
          reinterpret_cast<uintptr_t>(provider.indices.data())};
      if (found == ranges.end() ||
          geometry.geometry[found->second].revision !=
              provider.geometryRevision ||
          geometry.geometry[found->second].storageIdentity != storage)
        throw std::invalid_argument("ray geometry snapshot is stale");
      appendInstances(
          snapshot.instances, snapshot.instanceData, provider, draw,
          found->second, geometry.geometry[found->second].firstTriangle,
          geometry.hasSurfaceData ? &snapshot.surfaceInstanceData : nullptr);
    }
  }
  return snapshot;
}

RaySceneExtractionCache::Snapshot RaySceneExtractionCache::update(
    std::span<const RaySceneProviderSource> providers,
    RaySceneExtractionOptions options) {
  const auto signature = geometrySignature(providers, options);
  if (!geometry_ || signature != geometrySignature_) {
    auto geometry = std::make_shared<ExtractedRayScene>(
        extractRayScene(providers, options));
    ExtractedRayInstances instances{std::move(geometry->instances),
                                    std::move(geometry->instanceData),
                                    std::move(geometry->surfaceInstanceData)};
    geometry_ = std::move(geometry);
    geometrySignature_ = signature;
    return {geometry_, std::move(instances)};
  }
  return {geometry_, extractRaySceneInstances(*geometry_, providers)};
}

void RaySceneExtractionCache::clear() {
  geometry_.reset();
  geometrySignature_ = 0;
}

} // namespace container::renderer
