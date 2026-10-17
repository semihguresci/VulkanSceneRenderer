#pragma once

#include "Container/scene/SceneProvider.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct RasterDrawBatchDesc {
  container::scene::SceneProviderId providerId{};
  container::scene::SceneProviderKind providerKind{
      container::scene::SceneProviderKind::Mesh};
  std::size_t opaqueBatchCount{0};
  std::size_t transparentBatchCount{0};
  bool usesGpuVisibility{false};
};

struct RayTracingGeometryBuildInput {
  container::scene::SceneProviderId providerId{};
  std::size_t triangleGeometryCount{0};
  std::size_t instanceCount{0};
  uint64_t geometryRevision{0};
  uint64_t materialRevision{0};
  uint64_t instanceRevision{0};
  container::scene::SceneProviderBounds bounds{};
  bool hasOpaqueGeometry{false};
  container::scene::ImmutableSceneTriangleBatches triangleBatches{};
};

struct SplattingDispatchInput {
  container::scene::SceneProviderId providerId{};
  std::size_t splatCount{0};
  uint64_t geometryRevision{0};
  uint32_t sphericalHarmonicCoefficientCount{0};
  container::scene::SceneProviderBounds bounds{};
  bool requiresDepthComposite{true};
};

struct RadianceFieldDispatchInput {
  container::scene::SceneProviderId providerId{};
  uint32_t fieldCount{0};
  uint64_t geometryRevision{0};
  container::scene::SceneProviderBounds bounds{};
  bool usesOccupancyData{false};
};

class MeshRasterExtractor {
 public:
  virtual ~MeshRasterExtractor() = default;

  [[nodiscard]] virtual RasterDrawBatchDesc extract(
      const container::scene::SceneProviderSnapshot& provider) const = 0;
};

class BimRasterExtractor {
 public:
  virtual ~BimRasterExtractor() = default;

  [[nodiscard]] virtual RasterDrawBatchDesc extract(
      const container::scene::SceneProviderSnapshot& provider) const = 0;
};

class RayTracingSceneExtractor {
 public:
  virtual ~RayTracingSceneExtractor() = default;

  [[nodiscard]] virtual std::vector<RayTracingGeometryBuildInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const = 0;
};

class SplattingExtractor {
 public:
  virtual ~SplattingExtractor() = default;

  [[nodiscard]] virtual std::vector<SplattingDispatchInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const = 0;
};

class RadianceFieldExtractor {
 public:
  virtual ~RadianceFieldExtractor() = default;

  [[nodiscard]] virtual std::vector<RadianceFieldDispatchInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const = 0;
};

struct ProviderSceneExtraction {
  std::vector<container::scene::SceneProviderSnapshot> snapshots{};
  std::vector<RasterDrawBatchDesc> rasterBatches{};
  std::vector<RayTracingGeometryBuildInput> rayTracingBuildInputs{};
  std::vector<SplattingDispatchInput> splattingDispatchInputs{};
  std::vector<RadianceFieldDispatchInput> radianceFieldDispatchInputs{};
};

class ProviderBackedMeshRasterExtractor final : public MeshRasterExtractor {
 public:
  [[nodiscard]] RasterDrawBatchDesc extract(
      const container::scene::SceneProviderSnapshot& provider) const override {
    if (provider.kind != container::scene::SceneProviderKind::Mesh) {
      return {.providerId = provider.id, .providerKind = provider.kind};
    }
    const std::size_t primitiveCount =
        provider.primitiveCount > 0 ? provider.primitiveCount
                                    : provider.elementCount;
    return {.providerId = provider.id,
            .providerKind = provider.kind,
            .opaqueBatchCount = primitiveCount,
            .transparentBatchCount = 0,
            .usesGpuVisibility = false};
  }
};

class ProviderBackedBimRasterExtractor final : public BimRasterExtractor {
 public:
  [[nodiscard]] RasterDrawBatchDesc extract(
      const container::scene::SceneProviderSnapshot& provider) const override {
    if (provider.kind != container::scene::SceneProviderKind::Bim) {
      return {.providerId = provider.id, .providerKind = provider.kind};
    }
    const std::size_t opaqueBatchCount =
        provider.opaqueBatchCount > 0 ? provider.opaqueBatchCount
                                      : provider.primitiveCount;
    return {.providerId = provider.id,
            .providerKind = provider.kind,
            .opaqueBatchCount = opaqueBatchCount,
            .transparentBatchCount = provider.transparentBatchCount,
            .usesGpuVisibility = false};
  }
};

class ProviderBackedRayTracingSceneExtractor final
    : public RayTracingSceneExtractor {
 public:
  [[nodiscard]] std::vector<RayTracingGeometryBuildInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const override {
    std::vector<RayTracingGeometryBuildInput> inputs;
    for (const container::scene::SceneProviderSnapshot& provider : providers) {
      if (provider.kind != container::scene::SceneProviderKind::Mesh &&
          provider.kind != container::scene::SceneProviderKind::Bim) {
        continue;
      }
      const std::size_t primitiveCount =
          provider.primitiveCount > 0 ? provider.primitiveCount
                                      : provider.elementCount;
      const std::size_t instanceCount =
          provider.instanceCount > 0 ? provider.instanceCount
                                     : (provider.elementCount > 0 ? 1u : 0u);
      const std::size_t triangleGeometryCount =
          !provider.triangleBatches.empty() ? provider.triangleBatches.size()
                                            : primitiveCount;
      inputs.push_back({
          .providerId = provider.id,
          .triangleGeometryCount = triangleGeometryCount,
          .instanceCount = instanceCount,
          .geometryRevision = provider.revision.geometry,
          .materialRevision = provider.revision.materials,
          .instanceRevision = provider.revision.instances,
          .bounds = provider.bounds,
          .hasOpaqueGeometry = triangleGeometryCount > 0,
          .triangleBatches = provider.triangleBatches,
      });
    }
    return inputs;
  }
};

class ProviderBackedSplattingExtractor final : public SplattingExtractor {
 public:
  [[nodiscard]] std::vector<SplattingDispatchInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const override {
    std::vector<SplattingDispatchInput> inputs;
    for (const container::scene::SceneProviderSnapshot& provider : providers) {
      if (provider.kind !=
          container::scene::SceneProviderKind::GaussianSplatting) {
        continue;
      }
      const std::size_t splatCount =
          provider.splatCount > 0 ? provider.splatCount : provider.elementCount;
      inputs.push_back({.providerId = provider.id,
                        .splatCount = splatCount,
                        .geometryRevision = provider.revision.geometry,
                        .sphericalHarmonicCoefficientCount =
                            provider.sphericalHarmonicCoefficientCount,
                        .bounds = provider.bounds});
    }
    return inputs;
  }
};

class ProviderBackedRadianceFieldExtractor final
    : public RadianceFieldExtractor {
 public:
  [[nodiscard]] std::vector<RadianceFieldDispatchInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const override {
    std::vector<RadianceFieldDispatchInput> inputs;
    for (const container::scene::SceneProviderSnapshot& provider : providers) {
      if (provider.kind != container::scene::SceneProviderKind::RadianceField) {
        continue;
      }
      const uint32_t fieldCount =
          provider.fieldCount > 0
              ? provider.fieldCount
              : static_cast<uint32_t>(provider.elementCount);
      inputs.push_back({.providerId = provider.id,
                        .fieldCount = fieldCount,
                        .geometryRevision = provider.revision.geometry,
                        .bounds = provider.bounds,
                        .usesOccupancyData = provider.hasOccupancyData});
    }
    return inputs;
  }
};

[[nodiscard]] inline ProviderSceneExtraction extractProviderSceneFrameInputs(
    const container::scene::SceneProviderRegistry& registry) {
  ProviderSceneExtraction extraction{};
  extraction.snapshots = registry.snapshots();

  const ProviderBackedMeshRasterExtractor meshRasterExtractor;
  const ProviderBackedBimRasterExtractor bimRasterExtractor;
  for (const container::scene::SceneProviderSnapshot& provider :
       extraction.snapshots) {
    if (provider.kind == container::scene::SceneProviderKind::Mesh) {
      extraction.rasterBatches.push_back(meshRasterExtractor.extract(provider));
    } else if (provider.kind == container::scene::SceneProviderKind::Bim) {
      extraction.rasterBatches.push_back(bimRasterExtractor.extract(provider));
    }
  }

  const ProviderBackedRayTracingSceneExtractor rayTracingExtractor;
  const ProviderBackedSplattingExtractor splattingExtractor;
  const ProviderBackedRadianceFieldExtractor radianceFieldExtractor;
  extraction.rayTracingBuildInputs =
      rayTracingExtractor.extract(extraction.snapshots);
  extraction.splattingDispatchInputs =
      splattingExtractor.extract(extraction.snapshots);
  extraction.radianceFieldDispatchInputs =
      radianceFieldExtractor.extract(extraction.snapshots);
  return extraction;
}

}  // namespace container::renderer
