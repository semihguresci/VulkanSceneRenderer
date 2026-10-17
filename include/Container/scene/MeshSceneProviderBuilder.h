#pragma once

#include "Container/scene/SceneProvider.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace container::scene {

struct MeshSceneProviderPrimitive {
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  int32_t materialIndex{-1};
  bool doubleSided{false};
};

struct MeshSceneMaterialProperties {
  bool transparent{false};
  bool doubleSided{false};
};

struct MeshSceneAssetBuildInput {
  std::span<const MeshSceneProviderPrimitive> primitives{};
  std::span<const MeshSceneMaterialProperties> materials{};
  std::size_t materialCount{0};
  std::size_t instanceCount{0};
  SceneProviderBounds bounds{};
};

[[nodiscard]] inline MeshSceneAsset
buildMeshSceneAsset(const MeshSceneAssetBuildInput &input) {
  MeshSceneAsset asset{
      .primitiveCount = input.primitives.size(),
      .materialCount = input.materialCount > 0u ? input.materialCount
                                                : input.materials.size(),
      .instanceCount = input.instanceCount,
      .bounds = input.bounds,
  };
  std::vector<SceneProviderTriangleBatch> batches;
  batches.reserve(input.primitives.size());

  for (const MeshSceneProviderPrimitive &primitive : input.primitives) {
    if (primitive.indexCount == 0u) {
      continue;
    }

    MeshSceneMaterialProperties material{};
    if (primitive.materialIndex >= 0) {
      const std::size_t materialIndex =
          static_cast<std::size_t>(primitive.materialIndex);
      if (materialIndex < input.materials.size()) {
        material = input.materials[materialIndex];
      }
    }

    batches.push_back(SceneProviderTriangleBatch{
        .firstIndex = primitive.firstIndex,
        .indexCount = primitive.indexCount,
        .materialIndex = primitive.materialIndex,
        .doubleSided = primitive.doubleSided || material.doubleSided,
        .transparent = material.transparent,
        .instanceCount = 1u,
    });
  }

  asset.triangleBatches = ImmutableSceneTriangleBatches(std::move(batches));
  return asset;
}

} // namespace container::scene
