#include "Container/renderer/lighting/DdgiSceneChanges.h"
#include "Container/renderer/lighting/DdgiSceneBounds.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace container::renderer {
std::vector<DdgiDirtyRegion> DdgiSceneChangeTracker::update(
    std::span<const RaySceneProviderSource> sources,
    const std::function<uint64_t(uint32_t)> &materialRevision) {
  decltype(previous_) current;
  std::vector<DdgiDirtyRegion> dirty;
  std::map<std::pair<std::string, uint32_t>, std::pair<const RaySceneProviderSource *,
                                                    std::vector<DrawCommand>>> geometry;
  for (const auto &source : sources)
    for (const auto &draw : source.draws) {
      if (draw.instanceCount == 0 || draw.indexCount == 0)
        continue;
      if (uint64_t(draw.objectIndex) + draw.instanceCount > source.objects.size())
        throw std::invalid_argument("DDGI scene changes: draw instances exceed objects");
      for (uint32_t instance = 0; instance < draw.instanceCount; ++instance) {
        const uint64_t index = uint64_t(draw.objectIndex) + instance;
        if (index >= source.objects.size() || index > std::numeric_limits<uint32_t>::max())
          continue;
        const auto &object = source.objects[index];
        const auto key = std::make_pair(source.provider.value, uint32_t(index));
        auto entry = current.try_emplace(key,
          Object{object.model, object.objectInfo, source.geometryRevision,
                 materialRevision ? materialRevision(object.objectInfo.x) : 0}).first;
        auto &topology = entry->second.topology;
        topology = (topology ^ draw.firstIndex) * 1099511628211ull;
        topology = (topology ^ draw.indexCount) * 1099511628211ull;
        auto &ranges = geometry[key];
        ranges.first = &source;
        ranges.second.push_back({uint32_t(index), draw.firstIndex, draw.indexCount, 1});
      }
    }
  for (auto &[key, object] : current) {
    const auto old = previous_.find(key);
    if (old != previous_.end() && old->second.sameContent(object)) {
      object.bounds = old->second.bounds;
      continue;
    }
    if (old != previous_.end() && old->second.bounds)
      dirty.push_back(*old->second.bounds);
    const auto &[source, draws] = geometry.at(key);
    auto view = *source;
    view.draws = draws;
    const auto bounds = computeDdgiSceneBounds(std::span(&view, 1));
    if (bounds.valid) {
      object.bounds = DdgiDirtyRegion{bounds.minimum, bounds.maximum};
      dirty.push_back(*object.bounds);
    }
  }
  for (const auto &[key, object] : previous_)
    if (!current.contains(key) && object.bounds)
      dirty.push_back(*object.bounds);
  previous_ = std::move(current);
  return dirty;
}
} // namespace container::renderer
