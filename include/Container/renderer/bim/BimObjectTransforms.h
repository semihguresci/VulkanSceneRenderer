#pragma once

#include "Container/renderer/bim/BimMetadata.h"
#include "Container/utility/ObjectTransform.h"
#include "Container/utility/SceneData.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace container::scene {
struct SceneProviderBounds;
}

namespace container::renderer {

// Runtime transforms are independent of immutable imported text/geometry. Only
// moved objects retain an original transform/bounds and a current bounds row.
class BimObjectTransforms {
public:
  // Use the same preparation/validation as apply without publishing models,
  // bounds or runtime originals. Only the requested rows are prepared.
  void validate(std::span<const gpu::ObjectData> objects, BimMetadataTableView metadata,
                std::span<const gpu::ObjectTransformUpdate> updates,
                glm::vec3 rootTranslation = glm::vec3{0}) const;
  // Validate and prepare the entire batch before changing any object. Invalid
  // indices, duplicate indices, affine transforms or winding changes throw.
  // Imported bounds/models remain the reference, avoiding AABB inflation when
  // an object repeatedly rotates or returns to its original pose.
  bool apply(std::span<gpu::ObjectData> objects, BimMetadataTableView metadata,
             std::span<const gpu::ObjectTransformUpdate> updates,
             glm::vec3 rootTranslation = glm::vec3{0});
  [[nodiscard]] BimElementBounds boundsForObject(
      uint32_t objectIndex, BimElementBounds importedBounds,
      glm::vec3 rootTranslation = glm::vec3{0}) const;
  // The global translation moves live overlays, never their import references.
  void translate(glm::vec3 delta);
  // Retain the static import union and extend it to contain moved objects.
  // This is conservative rather than a tight union after objects move inward.
  void extendSceneBounds(scene::SceneProviderBounds &bounds) const;
  void clear() noexcept { decltype(originals_){}.swap(originals_); }
  [[nodiscard]] size_t changedObjectCount() const noexcept {
    return originals_.size();
  }

private:
  struct Original {
    glm::mat4 model{1.0f};
    BimElementBounds bounds{};
    BimElementBounds currentBounds{};
  };
  struct Pending {
    uint32_t objectIndex;
    Original original;
    gpu::ObjectData object;
  };
  [[nodiscard]] std::vector<Pending>
  prepare(std::span<const gpu::ObjectData> objects, BimMetadataTableView metadata,
           std::span<const gpu::ObjectTransformUpdate> updates,
           glm::vec3 rootTranslation) const;
  std::unordered_map<uint32_t, Original> originals_;
};

} // namespace container::renderer
