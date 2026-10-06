#include "Container/renderer/raytracing/RaySceneExtraction.h"

#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace container::renderer {

std::vector<RaySceneGeometry> ExtractedRayScene::geometryViews() const {
  std::vector<RaySceneGeometry> views;
  views.reserve(geometry.size());
  for (const auto &source : geometry)
    views.push_back({source.provider, source.id, source.revision,
                     source.vertices, source.indices, false});
  // Candidates remain non-opaque even for solid materials: section planes and
  // box clipping must be evaluated at the ray hit rather than at the instance.
  return views;
}

ExtractedRayScene
extractRayScene(std::span<const RaySceneProviderSource> providers) {
  ExtractedRayScene scene;
  std::set<std::string> identities;
  for (const auto &provider : providers) {
    if (provider.provider.value.empty() ||
        !identities.insert(provider.provider.value).second)
      throw std::invalid_argument("invalid or duplicate ray scene provider");
    std::map<uint64_t, uint32_t> ranges;
    for (const auto &draw : provider.draws) {
      if (draw.indexCount == 0 || draw.instanceCount == 0)
        continue;
      if (draw.indexCount % 3 != 0 ||
          draw.firstIndex > provider.indices.size() ||
          draw.indexCount > provider.indices.size() - draw.firstIndex ||
          draw.objectIndex > provider.objects.size() ||
          draw.instanceCount > provider.objects.size() - draw.objectIndex)
        throw std::invalid_argument("ray provider draw range is out of bounds");
      const uint64_t id = (uint64_t(draw.firstIndex) << 32) | draw.indexCount;
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
      for (uint32_t i = 0; i < draw.instanceCount; ++i) {
        if (scene.instances.size() >= 0x1000000u)
          throw std::invalid_argument("ray instance metadata exceeds 24 bits");
        const auto &object = provider.objects[draw.objectIndex + i];
        const auto customIndex = static_cast<uint32_t>(scene.instances.size());
        scene.instances.push_back({geometryIndex, customIndex, object.model,
                                   (object.objectInfo.y & 4u) != 0u});
        scene.instanceData.push_back(
            {glm::uvec4(scene.geometry[geometryIndex].firstTriangle,
                        object.objectInfo.x, object.objectInfo.y, 0u)});
      }
    }
  }
  const auto views = scene.geometryViews();
  validateRayScene({views, scene.instances});
  return scene;
}

} // namespace container::renderer
