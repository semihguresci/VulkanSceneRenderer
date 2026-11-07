#pragma once

#include <tiny_gltf.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace container::scene {

struct GltfSceneEntry {
  std::string name;
  std::vector<int32_t> sourceRoots;
};
struct GltfSceneCatalog {
  std::vector<GltfSceneEntry> entries;
  size_t defaultScene{};
  size_t sourceNodeCount{};
  bool authoredScenes{};
};
struct GltfSceneCatalogLimits {
  uint64_t maxScenes{65536};
  uint64_t maxRootReferences{1'000'000};
};

// Pure source metadata validation, independent of live scene graphs and Vulkan.
// Duplicate display names and parentless roots shared between scenes are valid.
[[nodiscard]] GltfSceneCatalog BuildGltfSceneCatalog(
    const tinygltf::Model& source, const GltfSceneCatalogLimits& limits = {});

} // namespace container::scene
