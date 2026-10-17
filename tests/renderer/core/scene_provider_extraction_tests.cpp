#include "Container/renderer/core/RenderExtraction.h"
#include "Container/renderer/debug/RendererDebugModels.h"
#include "Container/renderer/scene/SceneProviderSynchronizer.h"
#include "Container/scene/MeshSceneProviderAssetAdapter.h"
#include "Container/scene/MeshSceneProviderBuilder.h"
#include "Container/scene/SceneProvider.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using container::renderer::RadianceFieldDispatchInput;
using container::renderer::RadianceFieldExtractor;
using container::renderer::RayTracingGeometryBuildInput;
using container::renderer::RayTracingSceneExtractor;
using container::renderer::ProviderBackedBimRasterExtractor;
using container::renderer::ProviderBackedMeshRasterExtractor;
using container::renderer::ProviderBackedRadianceFieldExtractor;
using container::renderer::ProviderBackedRayTracingSceneExtractor;
using container::renderer::ProviderBackedSplattingExtractor;
using container::renderer::ProviderSceneExtraction;
using container::renderer::RasterDrawBatchDesc;
using container::renderer::SceneProviderSyncInput;
using container::renderer::SceneProviderSynchronizer;
using container::renderer::SplattingDispatchInput;
using container::renderer::SplattingExtractor;
using container::renderer::extractProviderSceneFrameInputs;
using container::scene::BimSceneAsset;
using container::scene::BimSceneProvider;
using container::scene::GaussianSplatSceneAsset;
using container::scene::GaussianSplatSceneProvider;
using container::scene::IRenderSceneProvider;
using container::scene::MeshSceneAsset;
using container::scene::MeshSceneMaterialProperties;
using container::scene::MeshSceneProviderMaterialFacts;
using container::scene::MeshSceneProvider;
using container::scene::MeshSceneProviderPrimitive;
using container::scene::RadianceFieldSceneAsset;
using container::scene::RadianceFieldSceneProvider;
using container::scene::SceneProviderId;
using container::scene::SceneProviderKind;
using container::scene::SceneProviderRegistry;
using container::scene::SceneProviderRevision;
using container::scene::SceneProviderSnapshot;
using container::scene::SceneProviderTriangleBatch;
using container::scene::buildMeshSceneProviderAsset;
using container::scene::buildMeshSceneAsset;

class TestProvider final : public IRenderSceneProvider {
 public:
  TestProvider(SceneProviderKind kind, std::string id, std::size_t elements)
      : snapshot_{.kind = kind,
                  .id = SceneProviderId{std::move(id)},
                  .elementCount = elements} {
    snapshot_.displayName = snapshot_.id.value;
  }

  [[nodiscard]] SceneProviderSnapshot snapshot() const override {
    return snapshot_;
  }

 private:
  SceneProviderSnapshot snapshot_{};
};

class TestRayTracingExtractor final : public RayTracingSceneExtractor {
 public:
  [[nodiscard]] std::vector<RayTracingGeometryBuildInput> extract(
      const std::vector<SceneProviderSnapshot>& providers) const override {
    std::vector<RayTracingGeometryBuildInput> inputs;
    for (const SceneProviderSnapshot& provider : providers) {
      if (provider.kind == SceneProviderKind::Mesh ||
          provider.kind == SceneProviderKind::Bim) {
        inputs.push_back({
            .providerId = provider.id,
            .triangleGeometryCount = provider.elementCount,
            .instanceCount = provider.elementCount > 0 ? 1u : 0u,
            .geometryRevision = provider.revision.geometry,
            .hasOpaqueGeometry = provider.elementCount > 0,
        });
      }
    }
    return inputs;
  }
};

class TestSplattingExtractor final : public SplattingExtractor {
 public:
  [[nodiscard]] std::vector<SplattingDispatchInput> extract(
      const std::vector<SceneProviderSnapshot>& providers) const override {
    std::vector<SplattingDispatchInput> inputs;
    for (const SceneProviderSnapshot& provider : providers) {
      if (provider.kind == SceneProviderKind::GaussianSplatting) {
        inputs.push_back({.providerId = provider.id,
                          .splatCount = provider.elementCount,
                          .geometryRevision = provider.revision.geometry});
      }
    }
    return inputs;
  }
};

class TestRadianceFieldExtractor final : public RadianceFieldExtractor {
 public:
  [[nodiscard]] std::vector<RadianceFieldDispatchInput> extract(
      const std::vector<SceneProviderSnapshot>& providers) const override {
    std::vector<RadianceFieldDispatchInput> inputs;
    for (const SceneProviderSnapshot& provider : providers) {
      if (provider.kind == SceneProviderKind::RadianceField) {
        inputs.push_back({.providerId = provider.id,
                          .fieldCount = 1u,
                          .geometryRevision = provider.revision.geometry,
                          .usesOccupancyData = true});
      }
    }
    return inputs;
  }
};

std::string readRepoTextFile(const std::filesystem::path& relativePath) {
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / relativePath;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST(SceneProviderRegistryTests, RegistersAndFiltersBackendNeutralProviders) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider bim{SceneProviderKind::Bim, "bim-scene", 12};
  TestProvider splats{SceneProviderKind::GaussianSplatting, "splats", 2048};

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);
  registry.registerProvider(bim);
  registry.registerProvider(splats);

  ASSERT_NE(registry.find(SceneProviderId{"bim-scene"}), nullptr);
  EXPECT_EQ(registry.providersForKind(SceneProviderKind::Mesh).size(), 1u);
  EXPECT_EQ(registry.providersForKind(SceneProviderKind::Bim).size(), 1u);
  EXPECT_EQ(registry.providersForKind(SceneProviderKind::GaussianSplatting)
                .size(),
            1u);
  EXPECT_EQ(registry.snapshots().size(), 3u);
}

TEST(SceneProviderRegistryTests, RejectsEmptyAndDuplicateProviderIds) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider duplicate{SceneProviderKind::Mesh, "mesh-scene", 8};
  TestProvider empty{SceneProviderKind::Mesh, "", 1};

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);

  EXPECT_THROW(registry.registerProvider(duplicate), std::invalid_argument);
  EXPECT_THROW(registry.registerProvider(empty), std::invalid_argument);
}

TEST(SceneProviderAdapterTests, MeshAndBimProvidersExposeNeutralSnapshots) {
  MeshSceneProvider mesh{SceneProviderId{"primary-mesh-scene"},
                         "Primary mesh"};
  mesh.update(MeshSceneAsset{
                  .primitiveCount = 7,
                  .materialCount = 3,
                  .instanceCount = 2,
                  .triangleBatches =
                      {
                          {.firstIndex = 3,
                           .indexCount = 30,
                           .materialIndex = 1,
                           .doubleSided = false,
                           .transparent = false,
                           .instanceCount = 2},
                          {.firstIndex = 33,
                           .indexCount = 12,
                           .materialIndex = 2,
                           .doubleSided = true,
                           .transparent = true,
                           .instanceCount = 1},
                      },
                  .bounds = {.min = {-1.0f, -2.0f, -3.0f},
                             .max = {1.0f, 2.0f, 3.0f},
                             .valid = true},
              },
              SceneProviderRevision{
                  .geometry = 11, .materials = 12, .instances = 13});

  BimSceneProvider bim{SceneProviderId{"auxiliary-bim-scene"}, "BIM"};
  bim.update(BimSceneAsset{
                 .elementCount = 42,
                 .meshPrimitiveCount = 12,
                 .meshOpaqueBatchCount = 8,
                 .meshTransparentBatchCount = 4,
                 .nativePointRangeCount = 4,
                 .nativeCurveRangeCount = 5,
                 .nativePointOpaqueRangeCount = 1,
                 .nativePointTransparentRangeCount = 3,
                 .nativeCurveOpaqueRangeCount = 2,
                 .nativeCurveTransparentRangeCount = 3,
                 .triangleBatches =
                     {
                         {.firstIndex = 9,
                          .indexCount = 21,
                          .materialIndex = 5,
                          .doubleSided = true,
                          .transparent = false,
                          .instanceCount = 4},
                     },
                 .bounds = {.min = {-4.0f, -5.0f, -6.0f},
                            .max = {4.0f, 5.0f, 6.0f},
                            .valid = true},
             },
             SceneProviderRevision{
                 .geometry = 17, .materials = 18, .instances = 19});

  const SceneProviderSnapshot meshSnapshot = mesh.snapshot();
  EXPECT_EQ(meshSnapshot.kind, SceneProviderKind::Mesh);
  EXPECT_EQ(meshSnapshot.id.value, "primary-mesh-scene");
  EXPECT_EQ(meshSnapshot.elementCount, 7u);
  EXPECT_EQ(meshSnapshot.revision.geometry, 11u);
  ASSERT_EQ(meshSnapshot.triangleBatches.size(), 2u);
  EXPECT_EQ(meshSnapshot.triangleBatches[0].firstIndex, 3u);
  EXPECT_EQ(meshSnapshot.triangleBatches[0].indexCount, 30u);
  EXPECT_EQ(meshSnapshot.triangleBatches[0].materialIndex, 1);
  EXPECT_FALSE(meshSnapshot.triangleBatches[0].doubleSided);
  EXPECT_FALSE(meshSnapshot.triangleBatches[0].transparent);
  EXPECT_EQ(meshSnapshot.triangleBatches[1].materialIndex, 2);
  EXPECT_TRUE(meshSnapshot.triangleBatches[1].doubleSided);
  EXPECT_TRUE(meshSnapshot.triangleBatches[1].transparent);
  EXPECT_TRUE(meshSnapshot.bounds.valid);

  const SceneProviderSnapshot bimSnapshot = bim.snapshot();
  EXPECT_EQ(bimSnapshot.kind, SceneProviderKind::Bim);
  EXPECT_EQ(bimSnapshot.id.value, "auxiliary-bim-scene");
  EXPECT_EQ(bimSnapshot.elementCount, 42u);
  EXPECT_EQ(bimSnapshot.primitiveCount, 12u);
  EXPECT_EQ(bimSnapshot.opaqueBatchCount, 8u);
  EXPECT_EQ(bimSnapshot.transparentBatchCount, 4u);
  EXPECT_EQ(bimSnapshot.nativePointRangeCount, 4u);
  EXPECT_EQ(bimSnapshot.nativeCurveRangeCount, 5u);
  EXPECT_EQ(bimSnapshot.nativePointOpaqueRangeCount, 1u);
  EXPECT_EQ(bimSnapshot.nativePointTransparentRangeCount, 3u);
  EXPECT_EQ(bimSnapshot.nativeCurveOpaqueRangeCount, 2u);
  EXPECT_EQ(bimSnapshot.nativeCurveTransparentRangeCount, 3u);
  ASSERT_EQ(bimSnapshot.triangleBatches.size(), 1u);
  EXPECT_EQ(bimSnapshot.triangleBatches.front().firstIndex, 9u);
  EXPECT_EQ(bimSnapshot.triangleBatches.front().instanceCount, 4u);
  EXPECT_EQ(bimSnapshot.revision.instances, 19u);
  EXPECT_TRUE(bimSnapshot.bounds.valid);
}

TEST(SceneProviderAssetBuilderTests,
     BuildsMeshAssetFromCpuPrimitiveFactsWithoutRendererState) {
  const std::vector<MeshSceneProviderPrimitive> primitives{
      {.firstIndex = 0,
       .indexCount = 36,
       .materialIndex = 0,
       .doubleSided = false},
      {.firstIndex = 36,
       .indexCount = 12,
       .materialIndex = 1,
       .doubleSided = true},
      {.firstIndex = 48,
       .indexCount = 0,
       .materialIndex = 1,
       .doubleSided = false},
  };
  const std::vector<MeshSceneMaterialProperties> materials{
      {.transparent = false, .doubleSided = false},
      {.transparent = true, .doubleSided = true},
  };

  const MeshSceneAsset asset = buildMeshSceneAsset({
      .primitives = primitives,
      .materials = materials,
      .instanceCount = 5,
      .bounds = {.min = {-1.0f, -2.0f, -3.0f},
                 .max = {1.0f, 2.0f, 3.0f},
                 .valid = true},
  });

  EXPECT_EQ(asset.primitiveCount, 3u);
  EXPECT_EQ(asset.materialCount, 2u);
  EXPECT_EQ(asset.instanceCount, 5u);
  EXPECT_TRUE(asset.bounds.valid);
  ASSERT_EQ(asset.triangleBatches.size(), 2u);
  EXPECT_EQ(asset.triangleBatches[0].firstIndex, 0u);
  EXPECT_EQ(asset.triangleBatches[0].indexCount, 36u);
  EXPECT_EQ(asset.triangleBatches[0].materialIndex, 0);
  EXPECT_FALSE(asset.triangleBatches[0].doubleSided);
  EXPECT_FALSE(asset.triangleBatches[0].transparent);
  EXPECT_EQ(asset.triangleBatches[1].firstIndex, 36u);
  EXPECT_TRUE(asset.triangleBatches[1].doubleSided);
  EXPECT_TRUE(asset.triangleBatches[1].transparent);
}

TEST(SceneProviderAssetBuilderTests,
     AdaptsImportedMeshFactsWithoutRendererFrontendState) {
  const std::vector<container::geometry::PrimitiveRange> primitiveRanges{
      {.firstIndex = 4,
       .indexCount = 24,
       .materialIndex = 0,
       .disableBackfaceCulling = false},
      {.firstIndex = 28,
       .indexCount = 18,
       .materialIndex = 1,
       .disableBackfaceCulling = false},
      {.firstIndex = 46,
       .indexCount = 6,
       .materialIndex = 2,
       .disableBackfaceCulling = true},
  };
  const std::vector<MeshSceneProviderMaterialFacts> materialFacts{
      {.transparent = false, .doubleSided = false},
      {.transparent = true, .doubleSided = false},
      {.transparent = false, .doubleSided = false},
  };

  const MeshSceneAsset asset = buildMeshSceneProviderAsset({
      .primitiveRanges = primitiveRanges,
      .materialCount = materialFacts.size(),
      .instanceCount = 9,
      .bounds = {.min = {-3.0f, -2.0f, -1.0f},
                 .max = {3.0f, 2.0f, 1.0f},
                 .valid = true},
      .materialFactsAt =
          [&materialFacts](uint32_t materialIndex) {
            return materialIndex < materialFacts.size()
                       ? materialFacts[materialIndex]
                       : MeshSceneProviderMaterialFacts{};
          },
  });

  EXPECT_EQ(asset.primitiveCount, 3u);
  EXPECT_EQ(asset.materialCount, 3u);
  EXPECT_EQ(asset.instanceCount, 9u);
  EXPECT_TRUE(asset.bounds.valid);
  ASSERT_EQ(asset.triangleBatches.size(), 3u);
  EXPECT_EQ(asset.triangleBatches[0].firstIndex, 4u);
  EXPECT_FALSE(asset.triangleBatches[0].transparent);
  EXPECT_FALSE(asset.triangleBatches[0].doubleSided);
  EXPECT_TRUE(asset.triangleBatches[1].transparent);
  EXPECT_FALSE(asset.triangleBatches[1].doubleSided);
  EXPECT_FALSE(asset.triangleBatches[2].transparent);
  EXPECT_TRUE(asset.triangleBatches[2].doubleSided);
}

TEST(SceneProviderAdapterTests,
     SplatAndRadianceFieldProvidersExposeNonTriangleSnapshots) {
  GaussianSplatSceneProvider splats{SceneProviderId{"survey-splats"},
                                    "Survey splats"};
  splats.update(GaussianSplatSceneAsset{
                    .splatCount = 2048,
                    .sphericalHarmonicCoefficientCount = 16,
                    .bounds = {.min = {-10.0f, -2.0f, -3.0f},
                               .max = {10.0f, 2.0f, 3.0f},
                               .valid = true},
                },
                SceneProviderRevision{.geometry = 31});

  RadianceFieldSceneProvider field{SceneProviderId{"radiance-volume"},
                                   "Radiance volume"};
  field.update(RadianceFieldSceneAsset{
                   .fieldCount = 3,
                   .hasOccupancyData = true,
                   .bounds = {.min = {-4.0f, -5.0f, -6.0f},
                              .max = {4.0f, 5.0f, 6.0f},
                              .valid = true},
               },
               SceneProviderRevision{.geometry = 41});

  const SceneProviderSnapshot splatSnapshot = splats.snapshot();
  EXPECT_EQ(splatSnapshot.kind, SceneProviderKind::GaussianSplatting);
  EXPECT_EQ(splatSnapshot.id.value, "survey-splats");
  EXPECT_EQ(splatSnapshot.elementCount, 2048u);
  EXPECT_EQ(splatSnapshot.splatCount, 2048u);
  EXPECT_EQ(splatSnapshot.sphericalHarmonicCoefficientCount, 16u);
  EXPECT_TRUE(splatSnapshot.triangleBatches.empty());
  EXPECT_TRUE(splatSnapshot.bounds.valid);

  const SceneProviderSnapshot fieldSnapshot = field.snapshot();
  EXPECT_EQ(fieldSnapshot.kind, SceneProviderKind::RadianceField);
  EXPECT_EQ(fieldSnapshot.id.value, "radiance-volume");
  EXPECT_EQ(fieldSnapshot.elementCount, 3u);
  EXPECT_EQ(fieldSnapshot.fieldCount, 3u);
  EXPECT_TRUE(fieldSnapshot.hasOccupancyData);
  EXPECT_TRUE(fieldSnapshot.triangleBatches.empty());
  EXPECT_TRUE(fieldSnapshot.bounds.valid);
}

TEST(RenderExtractionTests, ExtractorsRouteProvidersByRepresentation) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider bim{SceneProviderKind::Bim, "bim-scene", 12};
  TestProvider splats{SceneProviderKind::GaussianSplatting, "splats", 2048};
  TestProvider field{SceneProviderKind::RadianceField, "field", 1};

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);
  registry.registerProvider(bim);
  registry.registerProvider(splats);
  registry.registerProvider(field);
  const std::vector<SceneProviderSnapshot> snapshots = registry.snapshots();

  const TestRayTracingExtractor rayTracingExtractor;
  const TestSplattingExtractor splattingExtractor;
  const TestRadianceFieldExtractor radianceFieldExtractor;

  const std::vector<RayTracingGeometryBuildInput> rayInputs =
      rayTracingExtractor.extract(snapshots);
  const std::vector<SplattingDispatchInput> splatInputs =
      splattingExtractor.extract(snapshots);
  const std::vector<RadianceFieldDispatchInput> fieldInputs =
      radianceFieldExtractor.extract(snapshots);

  EXPECT_EQ(rayInputs.size(), 2u);
  EXPECT_EQ(splatInputs.size(), 1u);
  EXPECT_EQ(splatInputs.front().splatCount, 2048u);
  EXPECT_EQ(fieldInputs.size(), 1u);
  EXPECT_TRUE(fieldInputs.front().usesOccupancyData);
}

TEST(RenderExtractionTests,
     ProductionExtractorsConsumeMeshAndBimProviderSnapshots) {
  MeshSceneProvider mesh{SceneProviderId{"primary-mesh-scene"},
                         "Primary mesh"};
  mesh.update(MeshSceneAsset{
                  .primitiveCount = 7,
                  .materialCount = 3,
                  .instanceCount = 2,
                  .triangleBatches =
                      {
                          {.firstIndex = 3,
                           .indexCount = 30,
                           .materialIndex = 1,
                           .doubleSided = false,
                           .transparent = false,
                           .instanceCount = 2},
                          {.firstIndex = 33,
                           .indexCount = 12,
                           .materialIndex = 2,
                           .doubleSided = true,
                           .transparent = true,
                           .instanceCount = 1},
                      },
                  .bounds = {.min = {-1.0f, -2.0f, -3.0f},
                             .max = {1.0f, 2.0f, 3.0f},
                             .valid = true},
              },
              SceneProviderRevision{
                  .geometry = 11, .materials = 12, .instances = 13});

  BimSceneProvider bim{SceneProviderId{"auxiliary-bim-scene"}, "BIM"};
  bim.update(BimSceneAsset{
                 .elementCount = 42,
                 .meshPrimitiveCount = 12,
                 .meshOpaqueBatchCount = 8,
                 .meshTransparentBatchCount = 4,
                 .nativePointRangeCount = 4,
                 .nativeCurveRangeCount = 5,
                 .nativePointOpaqueRangeCount = 1,
                 .nativePointTransparentRangeCount = 3,
                 .nativeCurveOpaqueRangeCount = 2,
                 .nativeCurveTransparentRangeCount = 3,
                 .triangleBatches =
                     {
                         {.firstIndex = 9,
                          .indexCount = 21,
                          .materialIndex = 5,
                          .doubleSided = true,
                          .transparent = false,
                          .instanceCount = 4},
                     },
                 .bounds = {.min = {-4.0f, -5.0f, -6.0f},
                            .max = {4.0f, 5.0f, 6.0f},
                            .valid = true},
             },
             SceneProviderRevision{
                 .geometry = 17, .materials = 18, .instances = 19});

  const ProviderBackedMeshRasterExtractor meshRasterExtractor;
  const RasterDrawBatchDesc meshRaster =
      meshRasterExtractor.extract(mesh.snapshot());
  EXPECT_EQ(meshRaster.providerId.value, "primary-mesh-scene");
  EXPECT_EQ(meshRaster.opaqueBatchCount, 7u);
  EXPECT_EQ(meshRaster.transparentBatchCount, 0u);

  const ProviderBackedBimRasterExtractor bimRasterExtractor;
  const RasterDrawBatchDesc bimRaster =
      bimRasterExtractor.extract(bim.snapshot());
  EXPECT_EQ(bimRaster.providerId.value, "auxiliary-bim-scene");
  EXPECT_EQ(bimRaster.opaqueBatchCount, 8u);
  EXPECT_EQ(bimRaster.transparentBatchCount, 4u);

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);
  registry.registerProvider(bim);

  const ProviderBackedRayTracingSceneExtractor rayTracingExtractor;
  const std::vector<RayTracingGeometryBuildInput> rayInputs =
      rayTracingExtractor.extract(registry.snapshots());
  ASSERT_EQ(rayInputs.size(), 2u);
  EXPECT_EQ(rayInputs[0].providerId.value, "primary-mesh-scene");
  EXPECT_EQ(rayInputs[0].triangleGeometryCount, 2u);
  EXPECT_EQ(rayInputs[0].instanceCount, 2u);
  EXPECT_EQ(rayInputs[0].geometryRevision, 11u);
  EXPECT_EQ(rayInputs[0].materialRevision, 12u);
  EXPECT_EQ(rayInputs[0].instanceRevision, 13u);
  EXPECT_TRUE(rayInputs[0].bounds.valid);
  ASSERT_EQ(rayInputs[0].triangleBatches.size(), 2u);
  EXPECT_EQ(rayInputs[0].triangleBatches[0].firstIndex, 3u);
  EXPECT_EQ(rayInputs[0].triangleBatches[0].indexCount, 30u);
  EXPECT_EQ(rayInputs[0].triangleBatches[1].materialIndex, 2);
  EXPECT_EQ(rayInputs[1].providerId.value, "auxiliary-bim-scene");
  EXPECT_EQ(rayInputs[1].triangleGeometryCount, 1u);
  EXPECT_EQ(rayInputs[1].instanceCount, 42u);
  EXPECT_EQ(rayInputs[1].geometryRevision, 17u);
  EXPECT_EQ(rayInputs[1].materialRevision, 18u);
  EXPECT_EQ(rayInputs[1].instanceRevision, 19u);
  EXPECT_TRUE(rayInputs[1].bounds.valid);
  ASSERT_EQ(rayInputs[1].triangleBatches.size(), 1u);
  EXPECT_EQ(rayInputs[1].triangleBatches.front().materialIndex, 5);
}

TEST(RenderExtractionTests,
     ProviderFrameExtractionAggregatesRasterAndFutureTechniqueInputs) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider bim{SceneProviderKind::Bim, "bim-scene", 12};
  GaussianSplatSceneProvider splats{SceneProviderId{"splats"}, "Splats"};
  splats.update(GaussianSplatSceneAsset{
                    .splatCount = 2048,
                    .sphericalHarmonicCoefficientCount = 16,
                    .bounds = {.min = {-10.0f, -2.0f, -3.0f},
                               .max = {10.0f, 2.0f, 3.0f},
                               .valid = true},
                },
                SceneProviderRevision{.geometry = 21});
  RadianceFieldSceneProvider field{SceneProviderId{"field"}, "Field"};
  field.update(RadianceFieldSceneAsset{
                   .fieldCount = 2,
                   .hasOccupancyData = true,
                   .bounds = {.min = {-4.0f, -5.0f, -6.0f},
                              .max = {4.0f, 5.0f, 6.0f},
                              .valid = true},
               },
               SceneProviderRevision{.geometry = 22});

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);
  registry.registerProvider(bim);
  registry.registerProvider(splats);
  registry.registerProvider(field);

  const ProviderSceneExtraction extraction =
      extractProviderSceneFrameInputs(registry);

  EXPECT_EQ(extraction.snapshots.size(), 4u);
  EXPECT_EQ(extraction.rasterBatches.size(), 2u);
  EXPECT_EQ(extraction.rayTracingBuildInputs.size(), 2u);
  ASSERT_EQ(extraction.splattingDispatchInputs.size(), 1u);
  EXPECT_EQ(extraction.splattingDispatchInputs.front().providerId.value,
            "splats");
  EXPECT_EQ(extraction.splattingDispatchInputs.front().splatCount, 2048u);
  EXPECT_EQ(extraction.splattingDispatchInputs.front()
                .sphericalHarmonicCoefficientCount,
            16u);
  EXPECT_TRUE(extraction.splattingDispatchInputs.front().bounds.valid);
  ASSERT_EQ(extraction.radianceFieldDispatchInputs.size(), 1u);
  EXPECT_EQ(extraction.radianceFieldDispatchInputs.front().providerId.value,
            "field");
  EXPECT_EQ(extraction.radianceFieldDispatchInputs.front().fieldCount, 2u);
  EXPECT_TRUE(extraction.radianceFieldDispatchInputs.front().usesOccupancyData);
  EXPECT_TRUE(extraction.radianceFieldDispatchInputs.front().bounds.valid);
}

TEST(SceneProviderSynchronizerTests,
     SyncsProductionMeshAndBimProvidersFromNeutralInputs) {
  SceneProviderSynchronizer synchronizer;
  SceneProviderRegistry registry;

  synchronizer.sync(
      registry,
      SceneProviderSyncInput{
          .mesh =
              {
                  .available = true,
                  .primitiveCount = 7,
                  .materialCount = 3,
                  .instanceCount = 2,
                  .triangleBatches =
                      {
                          {.firstIndex = 3,
                           .indexCount = 30,
                           .materialIndex = 1,
                           .instanceCount = 2},
                      },
                  .bounds = {.min = {-1.0f, -2.0f, -3.0f},
                             .max = {1.0f, 2.0f, 3.0f},
                             .valid = true},
                  .geometryRevision = 11,
                  .materialRevision = 12,
                  .instanceRevision = 13,
                  .displayName = "Primary mesh",
              },
          .bim =
              {
                  .available = true,
                  .elementCount = 42,
                  .meshPrimitiveCount = 12,
                  .meshOpaqueBatchCount = 8,
                  .meshTransparentBatchCount = 4,
                  .nativePointRangeCount = 4,
                  .nativeCurveRangeCount = 5,
                  .nativePointOpaqueRangeCount = 1,
                  .nativePointTransparentRangeCount = 3,
                  .nativeCurveOpaqueRangeCount = 2,
                  .nativeCurveTransparentRangeCount = 3,
                  .triangleBatches =
                      {
                          {.firstIndex = 9,
                           .indexCount = 21,
                           .materialIndex = 5,
                           .doubleSided = true,
                           .instanceCount = 4},
                      },
                  .bounds = {.min = {-4.0f, -5.0f, -6.0f},
                             .max = {4.0f, 5.0f, 6.0f},
                             .valid = true},
                  .geometryRevision = 17,
                  .instanceRevision = 19,
                  .displayName = "BIM",
              },
      });

  const std::vector<SceneProviderSnapshot> snapshots = registry.snapshots();
  ASSERT_EQ(snapshots.size(), 2u);
  EXPECT_EQ(snapshots[0].id.value, "primary-mesh-scene");
  EXPECT_EQ(snapshots[0].materialCount, 3u);
  ASSERT_EQ(snapshots[0].triangleBatches.size(), 1u);
  EXPECT_EQ(snapshots[0].triangleBatches.front().firstIndex, 3u);
  EXPECT_EQ(snapshots[0].revision.materials, 12u);
  EXPECT_EQ(snapshots[1].id.value, "auxiliary-bim-scene");
  EXPECT_EQ(snapshots[1].opaqueBatchCount, 8u);
  EXPECT_EQ(snapshots[1].transparentBatchCount, 4u);
  EXPECT_EQ(snapshots[1].nativePointOpaqueRangeCount, 1u);
  EXPECT_EQ(snapshots[1].nativePointTransparentRangeCount, 3u);
  EXPECT_EQ(snapshots[1].nativeCurveOpaqueRangeCount, 2u);
  EXPECT_EQ(snapshots[1].nativeCurveTransparentRangeCount, 3u);
  ASSERT_EQ(snapshots[1].triangleBatches.size(), 1u);
  EXPECT_EQ(snapshots[1].triangleBatches.front().indexCount, 21u);
  EXPECT_EQ(snapshots[1].revision.instances, 19u);

  synchronizer.sync(
      registry,
      SceneProviderSyncInput{
          .mesh =
              {
                  .available = true,
                  .primitiveCount = 1,
                  .materialCount = 1,
                  .instanceCount = 1,
                  .geometryRevision = 21,
                  .displayName = "Only mesh",
              },
      });

  const std::vector<SceneProviderSnapshot> meshOnly = registry.snapshots();
  ASSERT_EQ(meshOnly.size(), 1u);
  EXPECT_EQ(meshOnly.front().id.value, "primary-mesh-scene");
  EXPECT_EQ(meshOnly.front().primitiveCount, 1u);
  EXPECT_EQ(meshOnly.front().displayName, "Only mesh");
}

TEST(RenderExtractionTests,
     BimNativeOnlyProviderDoesNotProduceRasterSurfaceBatches) {
  BimSceneProvider bim{SceneProviderId{"native-only-bim"}, "Native only BIM"};
  bim.update(BimSceneAsset{
                 .elementCount = 9,
                 .nativePointRangeCount = 4,
                 .nativeCurveRangeCount = 5,
                 .nativePointOpaqueRangeCount = 2,
                 .nativePointTransparentRangeCount = 2,
                 .nativeCurveOpaqueRangeCount = 3,
                 .nativeCurveTransparentRangeCount = 2,
                 .bounds = {.min = {-1.0f, -1.0f, -1.0f},
                            .max = {1.0f, 1.0f, 1.0f},
                            .valid = true},
             },
             SceneProviderRevision{.geometry = 51});

  const SceneProviderSnapshot snapshot = bim.snapshot();
  EXPECT_EQ(snapshot.primitiveCount, 0u);
  EXPECT_EQ(snapshot.opaqueBatchCount, 0u);
  EXPECT_EQ(snapshot.transparentBatchCount, 0u);
  EXPECT_EQ(snapshot.nativePointOpaqueRangeCount, 2u);
  EXPECT_EQ(snapshot.nativeCurveOpaqueRangeCount, 3u);

  const ProviderBackedBimRasterExtractor extractor;
  const RasterDrawBatchDesc raster = extractor.extract(snapshot);
  EXPECT_EQ(raster.opaqueBatchCount, 0u);
  EXPECT_EQ(raster.transparentBatchCount, 0u);
}

TEST(SceneProviderSynchronizerTests,
     SyncsFutureSplatAndRadianceFieldProvidersFromNeutralInputs) {
  SceneProviderSynchronizer synchronizer;
  SceneProviderRegistry registry;

  synchronizer.sync(
      registry,
      SceneProviderSyncInput{
          .gaussianSplats =
              {
                  .available = true,
                  .splatCount = 2048,
                  .sphericalHarmonicCoefficientCount = 16,
                  .bounds = {.min = {-8.0f, -2.0f, -3.0f},
                             .max = {8.0f, 2.0f, 3.0f},
                             .valid = true},
                  .geometryRevision = 31,
                  .displayName = "Survey splats",
              },
          .radianceField =
              {
                  .available = true,
                  .fieldCount = 2,
                  .hasOccupancyData = true,
                  .bounds = {.min = {-4.0f, -5.0f, -6.0f},
                             .max = {4.0f, 5.0f, 6.0f},
                             .valid = true},
                  .geometryRevision = 41,
                  .displayName = "Radiance volume",
              },
      });

  const std::vector<SceneProviderSnapshot> snapshots = registry.snapshots();
  ASSERT_EQ(snapshots.size(), 2u);
  EXPECT_EQ(snapshots[0].kind, SceneProviderKind::GaussianSplatting);
  EXPECT_EQ(snapshots[0].id.value, "gaussian-splat-scene");
  EXPECT_EQ(snapshots[0].displayName, "Survey splats");
  EXPECT_EQ(snapshots[0].splatCount, 2048u);
  EXPECT_EQ(snapshots[0].sphericalHarmonicCoefficientCount, 16u);
  EXPECT_TRUE(snapshots[0].triangleBatches.empty());
  EXPECT_TRUE(snapshots[0].bounds.valid);
  EXPECT_EQ(snapshots[1].kind, SceneProviderKind::RadianceField);
  EXPECT_EQ(snapshots[1].id.value, "radiance-field-scene");
  EXPECT_EQ(snapshots[1].displayName, "Radiance volume");
  EXPECT_EQ(snapshots[1].fieldCount, 2u);
  EXPECT_TRUE(snapshots[1].hasOccupancyData);
  EXPECT_TRUE(snapshots[1].triangleBatches.empty());
  EXPECT_EQ(snapshots[1].revision.geometry, 41u);

  const ProviderSceneExtraction extraction =
      extractProviderSceneFrameInputs(registry);
  EXPECT_TRUE(extraction.rasterBatches.empty());
  EXPECT_TRUE(extraction.rayTracingBuildInputs.empty());
  ASSERT_EQ(extraction.splattingDispatchInputs.size(), 1u);
  EXPECT_EQ(extraction.splattingDispatchInputs.front().providerId.value,
            "gaussian-splat-scene");
  ASSERT_EQ(extraction.radianceFieldDispatchInputs.size(), 1u);
  EXPECT_EQ(extraction.radianceFieldDispatchInputs.front().providerId.value,
            "radiance-field-scene");

  synchronizer.sync(registry, SceneProviderSyncInput{});
  EXPECT_TRUE(registry.snapshots().empty());
}

TEST(SceneDebugModelTests, BuildsUiNeutralProviderDebugState) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider splats{SceneProviderKind::GaussianSplatting, "splats", 2048};

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);
  registry.registerProvider(splats);

  const container::renderer::SceneDebugModel debugModel =
      container::renderer::buildSceneDebugModel(registry.snapshots());

  ASSERT_EQ(debugModel.providers.size(), 2u);
  EXPECT_EQ(debugModel.providers[0].providerId, "mesh-scene");
  EXPECT_EQ(debugModel.providers[0].kind, "mesh");
  EXPECT_EQ(debugModel.providers[0].elementCount, 4u);
  EXPECT_EQ(debugModel.providers[1].providerId, "splats");
  EXPECT_EQ(debugModel.providers[1].kind, "gaussian-splatting");
}

TEST(MeshSceneProviderCacheTests, ReusesTopologyAndPublishesLiveInstanceMetadata) {
  std::vector<container::geometry::PrimitiveRange> ranges{
      {.firstIndex = 3, .indexCount = 6, .materialIndex = 0},
      {.firstIndex = 9, .indexCount = 0, .materialIndex = 0}};
  int resolverCalls = 0;
  container::scene::MeshSceneProviderAssetAdapterInput input{
      .primitiveRanges = ranges,
      .materialCount = 1,
      .instanceCount = 2,
      .bounds = {.min = {-1, -1, -1}, .max = {1, 1, 1}, .valid = true},
      .materialFactsAt = [&](uint32_t) {
        ++resolverCalls;
        return MeshSceneProviderMaterialFacts{.doubleSided = true};
      }};
  container::scene::MeshSceneProviderAssetCache cache;
  const auto first = cache.get(input, {.geometry = 1, .materials = 2}, &ranges);
  ASSERT_EQ(first.triangleBatches.size(), 1u);
  EXPECT_EQ(resolverCalls, 1);
  input.instanceCount = 7;
  input.bounds.max = {9, 8, 7};
  for (uint64_t instanceRevision = 3; instanceRevision < 10; ++instanceRevision) {
    const auto& next = cache.get(input, {.geometry = 1, .materials = 2,
                                        .instances = instanceRevision}, &ranges);
    EXPECT_EQ(next.triangleBatches.data(), first.triangleBatches.data());
    EXPECT_EQ(next.instanceCount, 7u);
    EXPECT_EQ(next.bounds.max, glm::vec3(9, 8, 7));
    EXPECT_TRUE(next.triangleBatches.front().doubleSided);
  }
  EXPECT_EQ(resolverCalls, 1);
  EXPECT_EQ(first.instanceCount, 2u);
}

TEST(MeshSceneProviderCacheTests, RebuildsEqualSizedGeometryAndMaterialEdits) {
  std::vector<container::geometry::PrimitiveRange> ranges{
      {.firstIndex = 3, .indexCount = 6, .materialIndex = 0}};
  MeshSceneProviderMaterialFacts facts{};
  int resolverCalls = 0;
  container::scene::MeshSceneProviderAssetAdapterInput input{
      .primitiveRanges = ranges,
      .materialCount = 1,
      .materialFactsAt = [&](uint32_t) { ++resolverCalls; return facts; }};
  container::scene::MeshSceneProviderAssetCache cache;
  const auto original = cache.get(input, {.geometry = 1, .materials = 1}, &ranges);
  ranges.front().firstIndex = 90;
  const auto geometry = cache.get(input, {.geometry = 2, .materials = 1}, &ranges);
  EXPECT_NE(geometry.triangleBatches.data(), original.triangleBatches.data());
  EXPECT_EQ(geometry.triangleBatches.front().firstIndex, 90u);
  facts = {.transparent = true, .doubleSided = true};
  const auto material = cache.get(input, {.geometry = 2, .materials = 2}, &ranges);
  EXPECT_NE(material.triangleBatches.data(), geometry.triangleBatches.data());
  EXPECT_TRUE(material.triangleBatches.front().transparent);
  EXPECT_TRUE(material.triangleBatches.front().doubleSided);
  EXPECT_FALSE(original.triangleBatches.front().transparent);
  EXPECT_EQ(resolverCalls, 3);
  input.materialCount = 2;
  const auto addedMaterial = cache.get(input, {.geometry = 2, .materials = 2}, &ranges);
  EXPECT_NE(addedMaterial.triangleBatches.data(), material.triangleBatches.data());
  EXPECT_EQ(resolverCalls, 5);
}

TEST(MeshSceneProviderCacheTests, InvalidatesSourceReplacementAndExplicitUnload) {
  std::vector<container::geometry::PrimitiveRange> ranges{
      {.firstIndex = 3, .indexCount = 6}};
  container::scene::MeshSceneProviderAssetAdapterInput input{.primitiveRanges = ranges};
  container::scene::MeshSceneProviderAssetCache cache;
  int sourceA = 0;
  int sourceB = 0;
  const auto first = cache.get(input, {.geometry = 1}, &sourceA);
  ranges.front().firstIndex = 9;
  const auto replacement = cache.get(input, {.geometry = 1}, &sourceB);
  EXPECT_NE(first.triangleBatches.data(), replacement.triangleBatches.data());
  EXPECT_EQ(replacement.triangleBatches.front().firstIndex, 9u);
  cache.clear();
  ranges.front().firstIndex = 15;
  const auto reloaded = cache.get(input, {.geometry = 1}, &sourceB);
  EXPECT_NE(reloaded.triangleBatches.data(), replacement.triangleBatches.data());
  EXPECT_EQ(reloaded.triangleBatches.front().firstIndex, 15u);
  EXPECT_EQ(first.triangleBatches.front().firstIndex, 3u);
}

TEST(SceneProviderSynchronizerTests, SharesTopologyThroughRepeatedExtraction) {
  std::vector<SceneProviderTriangleBatch> batches(8790);
  for (uint32_t i = 0; i < batches.size(); ++i) {
    batches[i] = {.firstIndex = i * 3u, .indexCount = 3, .materialIndex = 2};
  }
  const auto* originalData = batches.data();
  SceneProviderSyncInput input{.bim = {
      .available = true, .elementCount = 8790, .meshPrimitiveCount = 8790,
      .meshOpaqueBatchCount = 8790, .triangleBatches = std::move(batches),
      .geometryRevision = 1}};
  SceneProviderSynchronizer synchronizer;
  SceneProviderRegistry registry;
  ProviderSceneExtraction retained;
  for (uint64_t revision = 1; revision <= 3; ++revision) {
    input.bim.instanceRevision = revision;
    input.bim.bounds = {.min = {-1, -1, -1}, .max = {float(revision), 2, 3}, .valid = true};
    synchronizer.sync(registry, input);
    auto extraction = extractProviderSceneFrameInputs(registry);
    ASSERT_EQ(extraction.snapshots.size(), 1u);
    ASSERT_EQ(extraction.rayTracingBuildInputs.size(), 1u);
    EXPECT_EQ(extraction.snapshots.front().triangleBatches.data(), originalData);
    EXPECT_EQ(extraction.rayTracingBuildInputs.front().triangleBatches.data(), originalData);
    EXPECT_EQ(extraction.snapshots.front().revision.instances, revision);
    EXPECT_EQ(extraction.snapshots.front().bounds.max.x, float(revision));
    EXPECT_EQ(extraction.rayTracingBuildInputs.front().triangleBatches[8789].firstIndex, 26367u);
    retained = std::move(extraction);
  }
  input = {};
  synchronizer.sync(registry, input);
  EXPECT_TRUE(registry.snapshots().empty());
  EXPECT_EQ(retained.snapshots.front().triangleBatches.data(), originalData);
  EXPECT_EQ(retained.rayTracingBuildInputs.front().triangleBatches[8789].firstIndex, 26367u);
}

TEST(SceneProviderSynchronizerTests, RetainedFramesSurviveReplacementWithSameRevisions) {
  SceneProviderSyncInput input{.mesh = {
      .available = true, .primitiveCount = 1, .instanceCount = 1,
      .triangleBatches = {{.firstIndex = 3, .indexCount = 6}}, .geometryRevision = 1}};
  SceneProviderSynchronizer synchronizer;
  SceneProviderRegistry registry;
  synchronizer.sync(registry, input);
  const auto oldFrame = extractProviderSceneFrameInputs(registry);
  input.mesh.triangleBatches = {{.firstIndex = 9, .indexCount = 12, .transparent = true}};
  input.mesh.instanceRevision = 2;
  synchronizer.sync(registry, input);
  const auto newFrame = extractProviderSceneFrameInputs(registry);
  EXPECT_EQ(oldFrame.snapshots.front().triangleBatches.front().firstIndex, 3u);
  EXPECT_EQ(oldFrame.rayTracingBuildInputs.front().triangleBatches.front().indexCount, 6u);
  EXPECT_EQ(newFrame.snapshots.front().triangleBatches.front().firstIndex, 9u);
  EXPECT_TRUE(newFrame.rayTracingBuildInputs.front().triangleBatches.front().transparent);
  EXPECT_NE(newFrame.snapshots.front().triangleBatches.data(),
            oldFrame.snapshots.front().triangleBatches.data());
}

TEST(RenderExtractionGuardrails, ProviderContractsStayBackendNeutral) {
  const std::string providerHeader =
      readRepoTextFile("include/Container/scene/SceneProvider.h");
  const std::string meshBuilderHeader =
      readRepoTextFile("include/Container/scene/MeshSceneProviderBuilder.h");
  const std::string meshAssetAdapterHeader = readRepoTextFile(
      "include/Container/scene/MeshSceneProviderAssetAdapter.h");
  const std::string extractionHeader =
      readRepoTextFile("include/Container/renderer/core/RenderExtraction.h");
  const std::string debugModelHeader =
      readRepoTextFile("include/Container/renderer/debug/RendererDebugModels.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");

  for (const std::string& forbidden :
       {"Vk", "DrawCommand", "FrameRecordParams", "PrimitiveRange"}) {
    EXPECT_FALSE(contains(providerHeader, forbidden))
        << "Scene providers must stay backend-neutral.";
  }
  for (const std::string& forbidden : {"Vk", "DrawCommand", "FrameRecordParams"}) {
    EXPECT_FALSE(contains(meshBuilderHeader, forbidden))
        << "Mesh scene provider builder must stay renderer-independent.";
  }
  for (const std::string& forbidden :
       {"Vk", "DrawCommand", "FrameRecordParams", "RendererFrontend",
        "SceneManager"}) {
    EXPECT_FALSE(contains(meshAssetAdapterHeader, forbidden))
        << "Mesh scene provider asset adapter must stay import/frontend-neutral.";
  }
  EXPECT_FALSE(contains(extractionHeader, "FrameRecordParams"));
  EXPECT_FALSE(contains(extractionHeader, "GBuffer"));
  EXPECT_FALSE(contains(debugModelHeader, "GuiManager"));
  EXPECT_TRUE(contains(rendererFrontend, "SceneProviderRegistry"));
  EXPECT_TRUE(contains(rendererFrontend,
                       ".sceneProviders = subs_.sceneProviderRegistry.get()"));
  EXPECT_TRUE(contains(rendererFrontend, "syncSceneProviders()"));
  EXPECT_FALSE(contains(rendererFrontend, "registerProvider"));
  EXPECT_TRUE(contains(rendererFrontend, "SceneProviderSynchronizer"));
  EXPECT_TRUE(contains(rendererFrontend, "subs_.sceneProviderSynchronizer"));
  EXPECT_TRUE(contains(extractionHeader, "ProviderBackedRayTracingSceneExtractor"));
  EXPECT_TRUE(contains(extractionHeader, "extractProviderSceneFrameInputs"));
  EXPECT_TRUE(contains(providerHeader, "SceneProviderTriangleBatch"));
  EXPECT_TRUE(contains(providerHeader, "class GaussianSplatSceneProvider"));
  EXPECT_TRUE(contains(providerHeader, "class RadianceFieldSceneProvider"));
  EXPECT_TRUE(contains(meshBuilderHeader, "buildMeshSceneAsset"));
  EXPECT_TRUE(contains(meshAssetAdapterHeader, "buildMeshSceneProviderAsset"));
  EXPECT_TRUE(contains(meshAssetAdapterHeader,
                       "MeshSceneProviderMaterialResolver"));
  EXPECT_TRUE(contains(extractionHeader, "triangleBatches"));
  EXPECT_FALSE(contains(rendererFrontend, "buildMeshSceneAsset"));
  EXPECT_TRUE(contains(readRepoTextFile(
                           "include/Container/renderer/scene/SceneProviderSynchronizer.h"),
                       "class SceneProviderSynchronizer"));
  EXPECT_TRUE(contains(readRepoTextFile(
                           "include/Container/renderer/scene/SceneProviderSynchronizer.h"),
                       "registerProvider"));
  EXPECT_TRUE(contains(readRepoTextFile(
                           "include/Container/renderer/bim/BimManager.h"),
                       "sceneProviderTriangleBatches() const"));
  EXPECT_TRUE(contains(sceneManagerHeader, "materialCount() const"));
  EXPECT_FALSE(contains(rendererFrontend,
                        "meshSceneProviderPrimitivesFromSceneManager"));
  EXPECT_FALSE(contains(rendererFrontend,
                        "meshSceneMaterialPropertiesFromSceneManager"));
  EXPECT_FALSE(contains(rendererFrontend, "meshSceneAssetFromSceneManager"));
  EXPECT_TRUE(contains(rendererFrontend, ".materialCount = meshAsset.materialCount"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "p.sceneExtraction = extractProviderSceneFrameInputs"));
  EXPECT_TRUE(contains(readRepoTextFile(
                           "include/Container/renderer/core/FrameRecorder.h"),
                       "ProviderSceneExtraction sceneExtraction"));
  EXPECT_TRUE(contains(deferredTechnique, "p.sceneExtraction.rasterBatches"));
  EXPECT_TRUE(contains(deferredTechnique,
                       "deferredRasterBimProviderRasterBatchReady"));
}
