#pragma once

#include "Container/scene/SceneProvider.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace container::renderer {

struct MeshSceneProviderSyncInput {
  bool available{false};
  std::size_t primitiveCount{0};
  std::size_t materialCount{0};
  std::size_t instanceCount{0};
  container::scene::ImmutableSceneTriangleBatches triangleBatches{};
  container::scene::SceneProviderBounds bounds{};
  uint64_t geometryRevision{0};
  uint64_t materialRevision{0};
  uint64_t instanceRevision{0};
  std::string displayName{};
};

struct BimSceneProviderSyncInput {
  bool available{false};
  std::size_t elementCount{0};
  std::size_t meshPrimitiveCount{0};
  std::size_t meshOpaqueBatchCount{0};
  std::size_t meshTransparentBatchCount{0};
  std::size_t nativePointRangeCount{0};
  std::size_t nativeCurveRangeCount{0};
  std::size_t nativePointOpaqueRangeCount{0};
  std::size_t nativePointTransparentRangeCount{0};
  std::size_t nativeCurveOpaqueRangeCount{0};
  std::size_t nativeCurveTransparentRangeCount{0};
  container::scene::ImmutableSceneTriangleBatches triangleBatches{};
  container::scene::SceneProviderBounds bounds{};
  uint64_t geometryRevision{0};
  uint64_t materialRevision{0};
  uint64_t instanceRevision{0};
  std::string displayName{};
};

struct GaussianSplatSceneProviderSyncInput {
  bool available{false};
  std::size_t splatCount{0};
  uint32_t sphericalHarmonicCoefficientCount{0};
  container::scene::SceneProviderBounds bounds{};
  uint64_t geometryRevision{0};
  std::string displayName{};
};

struct RadianceFieldSceneProviderSyncInput {
  bool available{false};
  uint32_t fieldCount{0};
  bool hasOccupancyData{false};
  container::scene::SceneProviderBounds bounds{};
  uint64_t geometryRevision{0};
  std::string displayName{};
};

struct SceneProviderSyncInput {
  MeshSceneProviderSyncInput mesh{};
  BimSceneProviderSyncInput bim{};
  GaussianSplatSceneProviderSyncInput gaussianSplats{};
  RadianceFieldSceneProviderSyncInput radianceField{};
};

class SceneProviderSynchronizer {
 public:
  void sync(container::scene::SceneProviderRegistry& registry,
            const SceneProviderSyncInput& input) {
    registry.clear();
    syncMeshProvider(registry, input.mesh);
    syncBimProvider(registry, input.bim);
    syncGaussianSplatProvider(registry, input.gaussianSplats);
    syncRadianceFieldProvider(registry, input.radianceField);
  }

 private:
  void syncMeshProvider(container::scene::SceneProviderRegistry& registry,
                        const MeshSceneProviderSyncInput& input) {
    if (!input.available ||
        (input.primitiveCount == 0 && !input.bounds.valid)) {
      meshProvider_.reset();
      return;
    }

    if (!meshProvider_) {
      meshProvider_ = std::make_unique<container::scene::MeshSceneProvider>(
          container::scene::SceneProviderId{"primary-mesh-scene"},
          "Primary mesh scene");
    }

    const uint64_t geometryRevision =
        input.geometryRevision > 0 ? input.geometryRevision : 1u;
    meshProvider_->update(
        container::scene::MeshSceneAsset{
            .primitiveCount = input.primitiveCount,
            .materialCount = input.materialCount,
            .instanceCount = input.instanceCount,
            .triangleBatches = input.triangleBatches,
            .bounds = input.bounds,
        },
        container::scene::SceneProviderRevision{
            .geometry = geometryRevision,
            .materials = input.materialRevision,
            .instances = input.instanceRevision,
        },
        input.displayName);
    registry.registerProvider(*meshProvider_);
  }

  void syncBimProvider(container::scene::SceneProviderRegistry& registry,
                       const BimSceneProviderSyncInput& input) {
    if (!input.available) {
      bimProvider_.reset();
      return;
    }

    if (!bimProvider_) {
      bimProvider_ = std::make_unique<container::scene::BimSceneProvider>(
          container::scene::SceneProviderId{"auxiliary-bim-scene"},
          "BIM scene");
    }

    const uint64_t geometryRevision =
        input.geometryRevision > 0 ? input.geometryRevision : 1u;
    bimProvider_->update(
        container::scene::BimSceneAsset{
            .elementCount = input.elementCount,
            .meshPrimitiveCount = input.meshPrimitiveCount,
            .meshOpaqueBatchCount = input.meshOpaqueBatchCount,
            .meshTransparentBatchCount = input.meshTransparentBatchCount,
            .nativePointRangeCount = input.nativePointRangeCount,
            .nativeCurveRangeCount = input.nativeCurveRangeCount,
            .nativePointOpaqueRangeCount = input.nativePointOpaqueRangeCount,
            .nativePointTransparentRangeCount =
                input.nativePointTransparentRangeCount,
            .nativeCurveOpaqueRangeCount = input.nativeCurveOpaqueRangeCount,
            .nativeCurveTransparentRangeCount =
                input.nativeCurveTransparentRangeCount,
            .triangleBatches = input.triangleBatches,
            .bounds = input.bounds,
        },
        container::scene::SceneProviderRevision{
            .geometry = geometryRevision,
            .materials = input.materialRevision,
            .instances = input.instanceRevision,
        },
        input.displayName);
    registry.registerProvider(*bimProvider_);
  }

  void syncGaussianSplatProvider(
      container::scene::SceneProviderRegistry& registry,
      const GaussianSplatSceneProviderSyncInput& input) {
    if (!input.available || (input.splatCount == 0 && !input.bounds.valid)) {
      gaussianSplatProvider_.reset();
      return;
    }

    if (!gaussianSplatProvider_) {
      gaussianSplatProvider_ =
          std::make_unique<container::scene::GaussianSplatSceneProvider>(
              container::scene::SceneProviderId{"gaussian-splat-scene"},
              "Gaussian splat scene");
    }

    const uint64_t geometryRevision =
        input.geometryRevision > 0 ? input.geometryRevision : 1u;
    gaussianSplatProvider_->update(
        container::scene::GaussianSplatSceneAsset{
            .splatCount = input.splatCount,
            .sphericalHarmonicCoefficientCount =
                input.sphericalHarmonicCoefficientCount,
            .bounds = input.bounds,
        },
        container::scene::SceneProviderRevision{
            .geometry = geometryRevision,
        },
        input.displayName);
    registry.registerProvider(*gaussianSplatProvider_);
  }

  void syncRadianceFieldProvider(
      container::scene::SceneProviderRegistry& registry,
      const RadianceFieldSceneProviderSyncInput& input) {
    if (!input.available || (input.fieldCount == 0 && !input.bounds.valid)) {
      radianceFieldProvider_.reset();
      return;
    }

    if (!radianceFieldProvider_) {
      radianceFieldProvider_ =
          std::make_unique<container::scene::RadianceFieldSceneProvider>(
              container::scene::SceneProviderId{"radiance-field-scene"},
              "Radiance field scene");
    }

    const uint64_t geometryRevision =
        input.geometryRevision > 0 ? input.geometryRevision : 1u;
    radianceFieldProvider_->update(
        container::scene::RadianceFieldSceneAsset{
            .fieldCount = input.fieldCount,
            .hasOccupancyData = input.hasOccupancyData,
            .bounds = input.bounds,
        },
        container::scene::SceneProviderRevision{
            .geometry = geometryRevision,
        },
        input.displayName);
    registry.registerProvider(*radianceFieldProvider_);
  }

  std::unique_ptr<container::scene::MeshSceneProvider> meshProvider_{};
  std::unique_ptr<container::scene::BimSceneProvider> bimProvider_{};
  std::unique_ptr<container::scene::GaussianSplatSceneProvider>
      gaussianSplatProvider_{};
  std::unique_ptr<container::scene::RadianceFieldSceneProvider>
      radianceFieldProvider_{};
};

}  // namespace container::renderer
