#pragma once

#include <Container/geometry/GltfSceneCatalog.h>
#include <Container/utility/SceneGraph.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace container::scene {

struct GltfSceneActivation {
  size_t scene{};
  std::vector<uint32_t> runtimeRoots;
  uint64_t renderableCount{};
};

// Prepares a view over a complete authoring graph, without changing visibility,
// names, source identities, topology, geometry or shared resource ownership.
[[nodiscard]] GltfSceneActivation PlanGltfSceneActivation(
    const GltfSceneCatalog& catalog, const SceneGraph& graph, size_t scene,
    uint64_t maxRenderableObjects = 4096,
    std::span<const uint32_t> additionalRoots = {});

} // namespace container::scene
