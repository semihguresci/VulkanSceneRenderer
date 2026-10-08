#include "Container/renderer/raytracing/RaySceneExtraction.h"

#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace container::renderer {
namespace {

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
                     uint32_t firstTriangle) {
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
  }
}

uint64_t geometrySignature(std::span<const RaySceneProviderSource> providers) {
  uint64_t hash = 1469598103934665603ull;
  auto bytes = [&](const void *data, size_t count) {
    const auto *value = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < count; ++i)
      hash = (hash ^ value[i]) * 1099511628211ull;
  };
  auto value = [&](const auto &item) { bytes(&item, sizeof(item)); };
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
extractRayScene(std::span<const RaySceneProviderSource> providers) {
  ExtractedRayScene scene;
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
        }
      }
      appendInstances(scene.instances, scene.instanceData, provider, draw,
                      geometryIndex,
                      scene.geometry[geometryIndex].firstTriangle);
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
      appendInstances(snapshot.instances, snapshot.instanceData, provider, draw,
                      found->second,
                      geometry.geometry[found->second].firstTriangle);
    }
  }
  return snapshot;
}

RaySceneExtractionCache::Snapshot RaySceneExtractionCache::update(
    std::span<const RaySceneProviderSource> providers) {
  const auto signature = geometrySignature(providers);
  if (!geometry_ || signature != geometrySignature_) {
    auto geometry =
        std::make_shared<ExtractedRayScene>(extractRayScene(providers));
    ExtractedRayInstances instances{std::move(geometry->instances),
                                    std::move(geometry->instanceData)};
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
