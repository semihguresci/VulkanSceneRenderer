#pragma once
#include "Container/renderer/lighting/DdgiPriorityScheduler.h"
#include "Container/renderer/raytracing/RaySceneExtraction.h"
#include <functional>
#include <map>

namespace container::renderer {
// Tracks semantic ray-scene objects, independent of camera/Hi-Z culling. Old
// and new referenced triangle bounds are both dirtied on movement/removal. Camera
// motion alone never changes transport. Material revisions include texture edits.
class DdgiSceneChangeTracker {
public:
  [[nodiscard]] std::vector<DdgiDirtyRegion> update(
      std::span<const RaySceneProviderSource> sources,
      const std::function<uint64_t(uint32_t)> &materialRevision = {});
  void clear() { previous_.clear(); }
private:
  struct Object {
    glm::mat4 model{1};
    glm::uvec4 info{};
    uint64_t geometry{}, material{}, topology{};
    std::optional<DdgiDirtyRegion> bounds;
    bool sameContent(const Object &other) const {
      return model == other.model && info == other.info && geometry == other.geometry &&
             material == other.material && topology == other.topology;
    }
  };
  std::map<std::pair<std::string, uint32_t>, Object> previous_;
};
} // namespace container::renderer
