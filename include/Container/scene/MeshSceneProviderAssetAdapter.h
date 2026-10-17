#pragma once

#include "Container/geometry/Model.h"
#include "Container/scene/MeshSceneProviderBuilder.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace container::scene {

struct MeshSceneProviderMaterialFacts {
  bool transparent{false};
  bool doubleSided{false};
};

using MeshSceneProviderMaterialResolver =
    std::function<MeshSceneProviderMaterialFacts(uint32_t)>;

struct MeshSceneProviderAssetAdapterInput {
  std::span<const container::geometry::PrimitiveRange> primitiveRanges{};
  std::size_t materialCount{0};
  std::size_t instanceCount{0};
  SceneProviderBounds bounds{};
  MeshSceneProviderMaterialResolver materialFactsAt{};
};

[[nodiscard]] inline std::vector<MeshSceneProviderPrimitive>
meshSceneProviderPrimitivesFromImport(
    std::span<const container::geometry::PrimitiveRange> primitiveRanges) {
  std::vector<MeshSceneProviderPrimitive> primitives;
  primitives.reserve(primitiveRanges.size());

  for (const container::geometry::PrimitiveRange &primitive : primitiveRanges) {
    primitives.push_back(MeshSceneProviderPrimitive{
        .firstIndex = primitive.firstIndex,
        .indexCount = primitive.indexCount,
        .materialIndex = primitive.materialIndex,
        .doubleSided = primitive.disableBackfaceCulling,
    });
  }

  return primitives;
}

[[nodiscard]] inline std::vector<MeshSceneMaterialProperties>
meshSceneMaterialPropertiesFromImport(
    std::size_t materialCount,
    const MeshSceneProviderMaterialResolver &materialFactsAt) {
  std::vector<MeshSceneMaterialProperties> materials;
  materials.reserve(materialCount);

  for (std::size_t materialIndex = 0; materialIndex < materialCount;
       ++materialIndex) {
    const MeshSceneProviderMaterialFacts facts =
        materialFactsAt ? materialFactsAt(static_cast<uint32_t>(materialIndex))
                        : MeshSceneProviderMaterialFacts{};
    materials.push_back(MeshSceneMaterialProperties{
        .transparent = facts.transparent,
        .doubleSided = facts.doubleSided,
    });
  }

  return materials;
}

[[nodiscard]] inline MeshSceneAsset
buildMeshSceneProviderAsset(const MeshSceneProviderAssetAdapterInput &input) {
  const std::vector<MeshSceneProviderPrimitive> primitives =
      meshSceneProviderPrimitivesFromImport(input.primitiveRanges);
  const std::vector<MeshSceneMaterialProperties> materials =
      meshSceneMaterialPropertiesFromImport(input.materialCount,
                                            input.materialFactsAt);

  return buildMeshSceneAsset({
      .primitives = primitives,
      .materials = materials,
      .materialCount = input.materialCount,
      .instanceCount = input.instanceCount,
      .bounds = input.bounds,
  });
}

// Instances and bounds are live metadata. Rebuild classification/topology only
// after an explicit source, geometry or material change, never on a transform.
class MeshSceneProviderAssetCache {
public:
  [[nodiscard]] const MeshSceneAsset &
  get(const MeshSceneProviderAssetAdapterInput &input,
      SceneProviderRevision revision, const void *sourceIdentity) {
    const Key next{sourceIdentity,
                   input.primitiveRanges.data(),
                   input.primitiveRanges.size(),
                   input.materialCount,
                   revision.geometry,
                   revision.materials};
    if (!key_ || *key_ != next) {
      asset_ = buildMeshSceneProviderAsset(input);
      key_ = next;
    }
    asset_.instanceCount = input.instanceCount;
    asset_.bounds = input.bounds;
    return asset_;
  }

  void clear() {
    asset_ = {};
    key_.reset();
  }

private:
  struct Key {
    const void *sourceIdentity{};
    const container::geometry::PrimitiveRange *ranges{};
    std::size_t primitiveCount{};
    std::size_t materialCount{};
    uint64_t geometryRevision{};
    uint64_t materialRevision{};
    bool operator==(const Key &) const = default;
  };
  std::optional<Key> key_{};
  MeshSceneAsset asset_{};
};

} // namespace container::scene
