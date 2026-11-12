#include "Container/utility/MaterialManager.h"
#include "Container/utility/MaterialXIntegration.h"
#include "Container/utility/MaterialSamplerKey.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/GltfTextureResources.h"
#include "Container/utility/GltfMaterialConversion.h"

#include <gtest/gtest.h>

#include <cmath>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

tinygltf::Value numberArray(std::initializer_list<double> values) {
  tinygltf::Value::Array array;
  array.reserve(values.size());
  for (double value : values) {
    array.emplace_back(value);
  }
  return tinygltf::Value(std::move(array));
}

tinygltf::Value textureInfo(int index) {
  tinygltf::Value::Object object;
  object.emplace("index", tinygltf::Value(index));
  return tinygltf::Value(std::move(object));
}

tinygltf::Value textureInfoWithScale(int index, double scale) {
  tinygltf::Value::Object object;
  object.emplace("index", tinygltf::Value(index));
  object.emplace("scale", tinygltf::Value(scale));
  return tinygltf::Value(std::move(object));
}

tinygltf::Value textureTransform(std::initializer_list<double> offset,
                                 double rotation,
                                 std::initializer_list<double> scale,
                                 int texCoord) {
  tinygltf::Value::Object object;
  object.emplace("offset", numberArray(offset));
  object.emplace("rotation", tinygltf::Value(rotation));
  object.emplace("scale", numberArray(scale));
  object.emplace("texCoord", tinygltf::Value(texCoord));
  return tinygltf::Value(std::move(object));
}

tinygltf::Parameter numberParameter(double value) {
  tinygltf::Parameter parameter;
  parameter.has_number_value = true;
  parameter.number_value = value;
  return parameter;
}
tinygltf::Parameter legacyMappingParameter(int index,uint32_t channel=2) {
  tinygltf::Parameter parameter;parameter.json_double_value={{"index",double(index)},{"texCoord",1},{"channel",double(channel)},
      {"offsetX",.125},{"offsetY",-.25},{"scaleX",2.},{"scaleY",-.5},{"rotation",.375}};
  return parameter;
}

glm::vec2 applyGltfTextureTransform(
    const container::material::TextureTransform& transform,
    const glm::vec2& texCoord) {
  const float cosRotation = std::cos(transform.rotation);
  const float sinRotation = std::sin(transform.rotation);
  return {
      transform.scale.x * cosRotation * texCoord.x -
          transform.scale.y * sinRotation * texCoord.y +
          transform.offset.x,
      transform.scale.x * sinRotation * texCoord.x +
          transform.scale.y * cosRotation * texCoord.y +
          transform.offset.y,
  };
}

}  // namespace

TEST(MaterialXIntegration, FileImportChecksDocumentCapabilities) {
  struct TemporaryAsset {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("container_materialx_gltf_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".gltf");
    ~TemporaryAsset() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } asset;
  auto write = [&](std::string_view contents) {
    std::ofstream file(asset.path, std::ios::binary);
    file << contents;
    ASSERT_TRUE(file.good());
  };

  const container::material::SlangMaterialXBridge bridge;
  write(R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_materials_unlit"],
      "materials":[{"name":"surface","extensions":{"KHR_materials_unlit":{}}}]})");
  EXPECT_EQ(bridge.loadGltfMaterials(asset.path.string()).size(), 1u);

  write(R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_materials_unlit"],
      "extensionsRequired":["KHR_materials_unlit"],"materials":[{"name":"surface"}]})");
  try {
    (void)bridge.loadGltfMaterials(asset.path.string());
    FAIL() << "Expected an unsupported required extension error";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(asset.path.filename().string()), std::string::npos);
    EXPECT_NE(message.find("unsupported_required_extension"), std::string::npos);
    EXPECT_NE(message.find("KHR_materials_unlit"), std::string::npos);
  }
}

TEST(MaterialXIntegration, BindsSeparateColorAndDataResourcesForTheSameSourceTexture) {
  using namespace container::material;
  tinygltf::Model model;
  model.textures.resize(1);
  model.materials.resize(1);
  auto& source = model.materials[0];
  source.pbrMetallicRoughness.baseColorTexture.index = 0;
  source.pbrMetallicRoughness.baseColorTexture.texCoord = 1;
  source.pbrMetallicRoughness.metallicRoughnessTexture.index = 0;
  source.normalTexture.index = source.occlusionTexture.index = source.emissiveTexture.index = 0;
  for (const auto& [extension, slots] : std::vector<std::pair<std::string, std::vector<std::string>>>{
      {"KHR_materials_specular", {"specularTexture", "specularColorTexture"}},
      {"KHR_materials_sheen", {"sheenColorTexture", "sheenRoughnessTexture"}},
      {"KHR_materials_clearcoat", {"clearcoatTexture", "clearcoatRoughnessTexture", "clearcoatNormalTexture"}},
      {"KHR_materials_transmission", {"transmissionTexture"}},
      {"KHR_materials_volume", {"thicknessTexture"}},
      {"KHR_materials_iridescence", {"iridescenceTexture", "iridescenceThicknessTexture"}}}) {
    tinygltf::Value::Object values;
    for (const auto& slot : slots) values.emplace(slot, textureInfo(0));
    source.extensions.emplace(extension, tinygltf::Value(std::move(values)));
  }
  MaterialManager materials;
  SlangMaterialXBridge bridge;
  uint32_t defaultMaterial{};
  bridge.loadMaterialsForGltfWithTextureRoles(model, GltfTextureResources{{17}, {23}}, materials, defaultMaterial);
  ASSERT_EQ(materials.materialCount(), 1u);
  const auto& material = *materials.getMaterial(defaultMaterial);
  for (auto resource : {material.baseColorTextureIndex, material.emissiveTextureIndex,
                        material.specularColorTextureIndex, material.sheenColorTextureIndex})
    EXPECT_EQ(resource, 17u);
  for (auto resource : {material.metallicRoughnessTextureIndex, material.normalTextureIndex,
                        material.occlusionTextureIndex, material.specularTextureIndex,
                        material.sheenRoughnessTextureIndex, material.clearcoatTextureIndex,
                        material.clearcoatRoughnessTextureIndex, material.clearcoatNormalTextureIndex,
                        material.transmissionTextureIndex, material.thicknessTextureIndex,
                        material.iridescenceTextureIndex, material.iridescenceThicknessTextureIndex})
    EXPECT_EQ(resource, 23u);
  EXPECT_EQ(material.baseColorTextureTransform.texCoord, 1u);
}

TEST(MaterialXIntegration, SpecularGlossinessKeepsColorRgbAndLinearAlphaRoles) {
  using namespace container::material;
  tinygltf::Model model;
  model.textures.resize(1);
  model.materials.resize(1);
  model.materials[0].extensions.emplace("KHR_materials_pbrSpecularGlossiness",
      tinygltf::Value(tinygltf::Value::Object{{"diffuseTexture", textureInfo(0)},
                                           {"specularGlossinessTexture", textureInfo(0)}}));
  MaterialManager materials;
  SlangMaterialXBridge bridge;
  uint32_t defaultMaterial{};
  bridge.loadMaterialsForGltfWithTextureRoles(model, GltfTextureResources{{17}, {23}}, materials, defaultMaterial);
  const auto& material = *materials.getMaterial(defaultMaterial);
  EXPECT_EQ(material.baseColorTextureIndex, 17u);
  EXPECT_EQ(material.specularColorTextureIndex, 17u);
  EXPECT_EQ(material.roughnessTextureIndex, 17u);
  // The SG shader selects linear alpha (channel 3) from this shared Color
  // resource; the automatic channel selector must remain intact.
  EXPECT_EQ(material.roughnessTextureTransform.channel, 4u);
  EXPECT_TRUE(material.specularGlossinessWorkflow);
  const auto count = materials.materialCount();
  EXPECT_THROW(bridge.loadMaterialsForGltfWithTextureRoles(model, GltfTextureResources{{}, {23}}, materials,
                                         defaultMaterial), std::invalid_argument);
  EXPECT_EQ(materials.materialCount(), count);
}

TEST(MaterialXIntegrationTests, SamplerReplacementPreflightUsesOnlyIncomingPolicies) {
  using namespace container::material;
  tinygltf::Model incoming;
  incoming.images.resize(1);
  incoming.images[0].uri = "resident.png";
  incoming.samplers.resize(1);
  incoming.samplers[0].minFilter = 9728;
  incoming.samplers[0].magFilter = 9728;
  incoming.textures.resize(2);
  incoming.textures[0].source = incoming.textures[1].source = 0;
  incoming.textures[0].sampler = incoming.textures[1].sampler = 0;
  const auto keys = CollectGltfMaterialSamplerKeys(incoming);
  ASSERT_EQ(keys.size(), 2u);
  const auto nearest = NormalizeMaterialSamplerKey(9728, 9728);
  EXPECT_EQ(keys[0], nearest);
  EXPECT_EQ(keys[1], nearest);

  const auto outgoingKey = NormalizeMaterialSamplerKey(9987, 9729, 33071);
  const auto outgoing = PlanMaterialSamplerSlots(std::array{outgoingKey}, 2);
  EXPECT_NO_THROW((void)PlanMaterialSamplerSlots(keys, 2));
  auto additive = keys;
  additive.push_back(outgoingKey);
  EXPECT_THROW((void)PlanMaterialSamplerSlots(additive, 2), std::runtime_error);

  // A rejected incoming table has no opportunity to retire or modify the old plan.
  EXPECT_THROW((void)PlanMaterialSamplerSlots(keys, 1), std::runtime_error);
  EXPECT_EQ(outgoing.slot(outgoingKey), 1u);
  EXPECT_EQ(outgoing.keys.size(), 2u);

  incoming.textures[1].sampler = -1;
  const auto withDefault = CollectGltfMaterialSamplerKeys(incoming);
  EXPECT_EQ(withDefault[1].value, 0u);
  EXPECT_EQ(PlanMaterialSamplerSlots(withDefault, 2).keys.size(), 2u);
  incoming.textures[1].source = 7;
  EXPECT_THROW((void)CollectGltfMaterialSamplerKeys(incoming), std::runtime_error);
}

TEST(MaterialXIntegrationTests, AlternateOnlyTexturesEnterSamplerBudgetBeforeLiveRetirement) {
  using namespace container::material;
  for (const auto& [extension, mime, available] : std::array{
      std::tuple{"KHR_texture_basisu", "image/ktx2", HasKtxTextureCodec()},
      std::tuple{"EXT_texture_webp", "image/webp", HasWebpTextureCodec()}}) {
    tinygltf::Model model;
    model.images.resize(1);
    model.images[0].uri = "resident-image";
    model.images[0].mimeType = mime;
    model.extensionsUsed = model.extensionsRequired = {extension};
    model.samplers.resize(1);
    model.samplers[0].minFilter = 9728;
    model.samplers[0].magFilter = 9728;
    model.textures.resize(2);
    for (auto& texture : model.textures) {
      texture.source = -1;
      texture.sampler = 0;
      texture.extensions.emplace(extension, tinygltf::Value(tinygltf::Value::Object{{"source", tinygltf::Value(0)}}));
    }
    if (!available) {
      EXPECT_THROW((void)CollectGltfMaterialSamplerKeys(model), std::runtime_error);
      continue;
    }
    const auto keys = CollectGltfMaterialSamplerKeys(model);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], NormalizeMaterialSamplerKey(9728, 9728));
    EXPECT_THROW((void)PlanMaterialSamplerSlots(keys, 1), std::runtime_error);
    EXPECT_EQ(PlanMaterialSamplerSlots(keys, 2).keys.size(), 2u);
  }
}

TEST(MaterialXIntegration, LoadsSeparateTextureResourcesForDistinctGltfSamplers) {
  tinygltf::Model model;
  model.images.resize(1);
  model.images[0].uri = "shared.png";
  model.textures.resize(3);
  for (auto& texture : model.textures) {
    texture.source = 0;
  }
  model.samplers.resize(3);
  model.samplers[0].wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
  model.samplers[0].wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
  model.samplers[1].wrapS = TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT;
  model.samplers[1].wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
  model.samplers[2].wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
  model.samplers[2].wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
  model.textures[0].sampler = 0;
  model.textures[1].sampler = 1;
  model.textures[2].sampler = 2;

  container::material::SlangMaterialXBridge bridge;
  container::material::TextureManager textureManager;
  uint32_t loadCount = 0;
  const auto textureToResource = bridge.loadTexturesForGltf(
      model, std::filesystem::path{}, textureManager,
      [&](const std::string&, bool) {
        ++loadCount;
        return container::material::TextureResource{};
      });

  ASSERT_EQ(textureToResource.size(), 3u);
  EXPECT_NE(textureToResource[0], textureToResource[1]);
  EXPECT_EQ(textureToResource[0], textureToResource[2]);
  EXPECT_EQ(loadCount, 2u);

  const auto* clampTexture = textureManager.getTexture(textureToResource[0]);
  const auto* mirrorTexture = textureManager.getTexture(textureToResource[1]);
  ASSERT_NE(clampTexture, nullptr);
  ASSERT_NE(mirrorTexture, nullptr);
  EXPECT_EQ(clampTexture->samplerIndex,
            container::gpu::kMaterialSamplerWrapClampToEdge);
  EXPECT_EQ(mirrorTexture->samplerIndex,
            container::gpu::kMaterialSamplerWrapMirroredRepeat);
}

TEST(MaterialXIntegration, SharedImageRetainsEveryAuthoredFilterPolicy) {
  tinygltf::Model model;
  model.images.resize(1);
  model.images[0].uri = "shared-filter-pattern.png";
  for (int min : {9728, 9729, 9984, 9985, 9986, 9987})
    for (int mag : {9728, 9729}) {
      tinygltf::Sampler sampler;
      sampler.minFilter = min; sampler.magFilter = mag;
      sampler.wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
      sampler.wrapT = TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT;
      tinygltf::Texture texture;
      texture.source = 0; texture.sampler = static_cast<int>(model.samplers.size());
      model.samplers.push_back(sampler); model.textures.push_back(texture);
    }
  auto omitted = model.samplers.back();
  omitted.minFilter = omitted.magFilter = -1;
  model.samplers.push_back(omitted);
  model.textures.push_back(model.textures.back());
  model.textures.back().sampler = 12;
  container::material::SlangMaterialXBridge bridge;
  container::material::TextureManager textures;
  uint32_t loads = 0;
  const auto resources = bridge.loadTexturesForGltf(model, {}, textures,
      [&](const std::string&, bool) { ++loads; return container::material::TextureResource{}; });
  ASSERT_EQ(resources.size(), 13u);
  EXPECT_EQ(loads, 12u);
  EXPECT_EQ(resources.back(), resources[11]);
  for (size_t i = 0; i < 12; ++i) {
    const auto* resource = textures.getTexture(resources[i]);
    ASSERT_NE(resource, nullptr);
    const auto description = container::material::DecodeMaterialSamplerKey({resource->samplerIndex});
    EXPECT_EQ(description.minFilter, model.samplers[i].minFilter);
    EXPECT_EQ(description.magFilter, model.samplers[i].magFilter);
    EXPECT_EQ(description.wrapS, TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);
    EXPECT_EQ(description.wrapT, TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT);
    for (size_t j = 0; j < i; ++j) EXPECT_NE(resources[i], resources[j]);
  }
}

TEST(MaterialXIntegration,
     UploadsEmbeddedPixelsWithAlphaAndRejectsTruncatedData) {
  container::material::SlangMaterialXBridge bridge;
  for (int components = 1; components <= 4; ++components) {
    tinygltf::Model model;
    model.images.resize(1);
    auto &image = model.images[0];
    image.width = 1;
    image.height = 1;
    image.bits = 8;
    image.component = components;
    image.image = {32, 64, 96, 128};
    image.image.resize(components);
    model.textures.resize(2);
    for (auto &texture : model.textures)
      texture.source = 0;
    container::material::TextureManager textures;
    uint32_t uploads = 0;
    auto upload = [&](const std::string &, std::span<const std::byte> rgba,
                      uint32_t width, uint32_t height, bool) {
      ++uploads;
      EXPECT_EQ(width, 1u);
      EXPECT_EQ(height, 1u);
      EXPECT_EQ(rgba.size(), 4u);
      EXPECT_EQ(std::to_integer<unsigned>(rgba[0]), 32u);
      EXPECT_EQ(std::to_integer<unsigned>(rgba[1]),
                components <= 2 ? 32u : 64u);
      EXPECT_EQ(std::to_integer<unsigned>(rgba[2]),
                components <= 2 ? 32u : 96u);
      EXPECT_EQ(std::to_integer<unsigned>(rgba[3]), components == 2   ? 64u
                                                    : components == 4 ? 128u
                                                                      : 255u);
      return container::material::TextureResource{};
    };
    auto external = [](const std::string &, bool) {
      ADD_FAILURE() << "Embedded glTF must not use filesystem loading";
      return container::material::TextureResource{};
    };
    const auto mapping =
        bridge.loadTexturesForGltf(model, {}, textures, external, upload);
    ASSERT_EQ(mapping.size(), 2u);
    EXPECT_NE(mapping[0], UINT32_MAX);
    EXPECT_EQ(mapping[0], mapping[1]);
    EXPECT_EQ(uploads, 1u);
    image.width = 2; // Raw payload no longer covers the declared dimensions.
    container::material::TextureManager fresh;
    const auto invalid =
        bridge.loadTexturesForGltf(model, {}, fresh, external, upload);
    EXPECT_EQ(invalid[0], UINT32_MAX);
    EXPECT_EQ(uploads, 1u);
  }
}

TEST(MaterialXIntegration, EmbeddedImageCacheIncludesContentAndColorSpace) {
  container::material::SlangMaterialXBridge bridge;
  container::material::TextureManager textures;
  tinygltf::Model model;
  model.images.resize(1);
  auto &image = model.images[0];
  image.width = image.height = 1;
  image.bits = 8;
  image.component = 4;
  image.image = {255, 0, 0, 255};
  model.textures.resize(1);
  model.textures[0].source = 0;
  std::vector<bool> uploadColorSpaces;
  auto external = [](const std::string &, bool) {
    ADD_FAILURE() << "Embedded image requested a file";
    return container::material::TextureResource{};
  };
  auto upload = [&](const std::string &, std::span<const std::byte>, uint32_t,
                    uint32_t, bool srgb) {
    uploadColorSpaces.push_back(srgb);
    return container::material::TextureResource{};
  };
  auto load = [&] {
    return bridge
        .loadTexturesForGltf(model, "shared", textures, external, upload)
        .at(0);
  };
  const auto red = load();
  EXPECT_EQ(load(), red);
  image.image = {0, 255, 0, 255};
  const auto green = load();
  EXPECT_NE(green, red);
  EXPECT_EQ(load(), green);
  model.materials.resize(1);
  model.materials[0].normalTexture.index = 0;
  EXPECT_NE(load(), green);
  EXPECT_EQ(uploadColorSpaces, (std::vector<bool>{true, true, false}));
}

TEST(MaterialXIntegration,
     ReusesDecodedExternalPixelsAndPreservesFileSamplerColorSpaceCache) {
  tinygltf::Model model;
  model.images.resize(1);
  auto &image = model.images[0];
  image.uri = "shared.png";
  image.width = image.height = 1;
  image.bits = 8;
  image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.component = 4;
  image.image = {32, 64, 96, 128};
  model.textures.resize(3);
  model.samplers.resize(3);
  for (size_t i = 0; i < model.textures.size(); ++i) {
    model.textures[i].source = 0;
    model.textures[i].sampler = static_cast<int>(i);
    model.samplers[i].wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
    model.samplers[i].wrapS = i == 1 ? TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT
                                    : TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
  }
  model.materials.resize(1);
  model.materials[0].pbrMetallicRoughness.baseColorTexture.index = 0;

  container::material::SlangMaterialXBridge bridge;
  container::material::TextureManager textures;
  std::vector<bool> uploadedColorSpaces;
  std::string uploadedName;
  const auto external = [](const std::string &, bool) {
    ADD_FAILURE() << "Prepared external pixels must not reopen the file";
    return container::material::TextureResource{};
  };
  const auto upload = [&](const std::string &name,
                          std::span<const std::byte> rgba, uint32_t width,
                          uint32_t height, bool srgb) {
    uploadedName = name;
    uploadedColorSpaces.push_back(srgb);
    EXPECT_EQ(std::filesystem::path(name).lexically_normal(),
              std::filesystem::path("assets/shared.png").lexically_normal());
    EXPECT_EQ(width, 1u);
    EXPECT_EQ(height, 1u);
    EXPECT_EQ(rgba.size(), 4u);
    EXPECT_EQ(rgba.data(),
              reinterpret_cast<const std::byte *>(image.image.data()));
    EXPECT_EQ(std::to_integer<unsigned>(rgba[0]), 32u);
    EXPECT_EQ(std::to_integer<unsigned>(rgba[3]), 128u);
    return container::material::TextureResource{};
  };
  const auto load = [&] {
    return bridge.loadTexturesForGltf(model, "assets", textures, external,
                                      upload);
  };
  const auto colorMapping = load();
  ASSERT_EQ(colorMapping.size(), 3u);
  EXPECT_NE(colorMapping[0], UINT32_MAX);
  EXPECT_NE(colorMapping[0], colorMapping[1]);
  EXPECT_EQ(colorMapping[0], colorMapping[2]);
  ASSERT_EQ(uploadedColorSpaces.size(), 2u);
  const auto *colorTexture = textures.getTexture(colorMapping[0]);
  ASSERT_NE(colorTexture, nullptr);
  EXPECT_EQ(colorTexture->name,
            "file:" + uploadedName + "|sampler=" +
                std::to_string(container::gpu::kMaterialSamplerWrapClampToEdge) +
                "|srgb=1");
  EXPECT_EQ(load(), colorMapping);

  // The former file-loading path must resolve to the same existing resources.
  image.image.clear();
  EXPECT_EQ(load(), colorMapping);
  EXPECT_EQ(uploadedColorSpaces.size(), 2u);
  image.image = {32, 64, 96, 128};
  model.materials[0].normalTexture.index = 0;
  const auto dataMapping = load();
  EXPECT_NE(dataMapping[0], colorMapping[0]);
  EXPECT_NE(dataMapping[1], colorMapping[1]);
  EXPECT_EQ(uploadedColorSpaces, (std::vector<bool>{true, true, false, false}));
}

TEST(MaterialXIntegration, ConvertsDecodedExternalGrayscaleAlphaAndRgbPixels) {
  container::material::SlangMaterialXBridge bridge;
  for (int components = 1; components <= 3; ++components) {
    SCOPED_TRACE(components);
    tinygltf::Model model;
    model.images.resize(1);
    auto &image = model.images[0];
    image.uri = "decoded.png";
    image.width = image.height = 1;
    image.bits = 8;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.component = components;
    image.image = {32, 64, 96};
    image.image.resize(components);
    model.textures.resize(1);
    model.textures[0].source = 0;
    container::material::TextureManager textures;
    uint32_t uploads = 0;
    const auto mapping = bridge.loadTexturesForGltf(
        model, {}, textures,
        [](const std::string &, bool) {
          ADD_FAILURE() << "Decoded external image requested a file";
          return container::material::TextureResource{};
        },
        [&](const std::string &, std::span<const std::byte> rgba, uint32_t,
            uint32_t, bool) {
          ++uploads;
          EXPECT_EQ(rgba.size(), 4u);
          EXPECT_EQ(std::to_integer<unsigned>(rgba[0]), 32u);
          EXPECT_EQ(std::to_integer<unsigned>(rgba[1]),
                    components <= 2 ? 32u : 64u);
          EXPECT_EQ(std::to_integer<unsigned>(rgba[2]),
                    components <= 2 ? 32u : 96u);
          EXPECT_EQ(std::to_integer<unsigned>(rgba[3]),
                    components == 2 ? 64u : 255u);
          return container::material::TextureResource{};
        });
    ASSERT_EQ(mapping.size(), 1u);
    EXPECT_NE(mapping[0], UINT32_MAX);
    EXPECT_EQ(uploads, 1u);
  }
}

TEST(MaterialXIntegration, FallsBackToFileForUnsupportedExternalDecodedPixels) {
  container::material::SlangMaterialXBridge bridge;
  for (int mode = 0; mode < 6; ++mode) {
    SCOPED_TRACE(mode);
    tinygltf::Model model;
    model.images.resize(1);
    auto &image = model.images[0];
    image.uri = "fallback.png";
    image.width = image.height = 1;
    image.bits = 8;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.component = 4;
    image.image = {32, 64, 96, 128};
    switch (mode) {
    case 0:
      image.bits = 16;
      image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
      image.image.resize(8);
      break;
    case 1:
      image.as_is = true;
      break;
    case 2:
      image.image.resize(3);
      break;
    case 3:
      image.image.clear();
      break;
    case 4:
      image.width = -1;
      break;
    case 5:
      image.pixel_type = TINYGLTF_COMPONENT_TYPE_FLOAT;
      break;
    }
    model.textures.resize(1);
    model.textures[0].source = 0;
    model.materials.resize(1);
    model.materials[0].normalTexture.index = 0;
    container::material::TextureManager textures;
    uint32_t fileLoads = 0;
    const auto mapping = bridge.loadTexturesForGltf(
        model, {}, textures,
        [&](const std::string &name, bool srgb) {
          ++fileLoads;
          EXPECT_EQ(name, "fallback.png");
          EXPECT_FALSE(srgb);
          return container::material::TextureResource{};
        },
        [](const std::string &, std::span<const std::byte>, uint32_t, uint32_t,
           bool) {
          ADD_FAILURE() << "Unsupported decoded payload requested pixel upload";
          return container::material::TextureResource{};
        });
    ASSERT_EQ(mapping.size(), 1u);
    EXPECT_NE(mapping[0], UINT32_MAX);
    EXPECT_EQ(fileLoads, 1u);
  }
}

TEST(MaterialXIntegration, ExternalDecodedPixelsStillSupportFileOnlyLoader) {
  tinygltf::Model model;
  model.images.resize(1);
  auto &image = model.images[0];
  image.uri = "fallback.png";
  image.width = image.height = 1;
  image.bits = 8;
  image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.component = 4;
  image.image = {32, 64, 96, 128};
  model.textures.resize(1);
  model.textures[0].source = 0;
  container::material::SlangMaterialXBridge bridge;
  container::material::TextureManager textures;
  uint32_t fileLoads = 0;
  const auto mapping = bridge.loadTexturesForGltf(
      model, {}, textures, [&](const std::string &, bool srgb) {
        ++fileLoads;
        EXPECT_TRUE(srgb);
        return container::material::TextureResource{};
      });
  ASSERT_EQ(mapping.size(), 1u);
  EXPECT_NE(mapping[0], UINT32_MAX);
  EXPECT_EQ(fileLoads, 1u);
}

TEST(MaterialXIntegration, ImportsKhrMaterialsPbrSpecularGlossiness) {
  tinygltf::Model model;
  model.images.resize(2);
  model.textures.resize(2);
  model.textures[0].source = 0;
  model.textures[1].source = 1;

  tinygltf::Material gltfMaterial;
  gltfMaterial.alphaMode = "BLEND";

  tinygltf::Value::Object specGloss;
  specGloss.emplace("diffuseFactor",
                    numberArray({0.0, 0.0, 0.0, 0.6}));
  specGloss.emplace("specularFactor",
                    numberArray({1.0, 0.766, 0.336}));
  specGloss.emplace("glossinessFactor", tinygltf::Value(0.9));
  specGloss.emplace("diffuseTexture", textureInfo(0));
  specGloss.emplace("specularGlossinessTexture", textureInfo(1));
  gltfMaterial.extensions.emplace(
      "KHR_materials_pbrSpecularGlossiness",
      tinygltf::Value(std::move(specGloss)));
  model.materials.push_back(std::move(gltfMaterial));

  container::material::SlangMaterialXBridge bridge;
  container::material::MaterialManager materialManager;
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();
  const std::vector<uint32_t> imageToTexture{7u, 11u};

  bridge.loadMaterialsForGltf(model, imageToTexture, materialManager,
                              defaultMaterialIndex);

  ASSERT_EQ(materialManager.materialCount(), 1u);
  const auto* material = materialManager.getMaterial(0);
  ASSERT_NE(material, nullptr);

  EXPECT_TRUE(material->specularGlossinessWorkflow);
  EXPECT_EQ(material->baseColorTextureIndex, 7u);
  EXPECT_EQ(material->roughnessTextureIndex, 11u);
  EXPECT_EQ(material->specularColorTextureIndex, 11u);
  EXPECT_EQ(material->specularTextureIndex,
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(material->metallicRoughnessTextureIndex,
            std::numeric_limits<uint32_t>::max());
  EXPECT_NEAR(material->baseColor.r, 1.0f, 1e-4f);
  EXPECT_NEAR(material->baseColor.g, 0.766f, 1e-4f);
  EXPECT_NEAR(material->baseColor.b, 0.336f, 1e-4f);
  EXPECT_NEAR(material->baseColor.a, 0.6f, 1e-4f);
  EXPECT_NEAR(material->metallicFactor, 1.0f, 1e-4f);
  EXPECT_NEAR(material->roughnessFactor, 0.9f, 1e-4f);
  EXPECT_EQ(material->alphaMode, container::material::AlphaMode::Blend);
  EXPECT_EQ(defaultMaterialIndex, 0u);
}

TEST(MaterialXIntegration, ImportsLayeredKhrPbrExtensions) {
  tinygltf::Model model;
  model.images.resize(11);
  model.textures.resize(11);
  for (size_t i = 0; i < model.textures.size(); ++i) {
    model.textures[i].source = static_cast<int>(i);
  }

  tinygltf::Material gltfMaterial;
  gltfMaterial.alphaMode = "OPAQUE";
  gltfMaterial.emissiveFactor = {0.25, 0.5, 0.75};

  tinygltf::Value::Object specular;
  specular.emplace("specularFactor", tinygltf::Value(0.5));
  specular.emplace("specularColorFactor",
                   numberArray({0.8, 0.7, 0.6}));
  specular.emplace("specularTexture", textureInfo(0));
  specular.emplace("specularColorTexture", textureInfo(1));
  gltfMaterial.extensions.emplace("KHR_materials_specular",
                                  tinygltf::Value(std::move(specular)));

  tinygltf::Value::Object clearcoat;
  clearcoat.emplace("clearcoatFactor", tinygltf::Value(0.7));
  clearcoat.emplace("clearcoatTexture", textureInfo(2));
  clearcoat.emplace("clearcoatRoughnessFactor", tinygltf::Value(0.2));
  clearcoat.emplace("clearcoatRoughnessTexture", textureInfo(3));
  clearcoat.emplace("clearcoatNormalTexture", textureInfoWithScale(4, 0.4));
  gltfMaterial.extensions.emplace("KHR_materials_clearcoat",
                                  tinygltf::Value(std::move(clearcoat)));

  tinygltf::Value::Object transmission;
  transmission.emplace("transmissionFactor", tinygltf::Value(0.6));
  transmission.emplace("transmissionTexture", textureInfo(5));
  gltfMaterial.extensions.emplace("KHR_materials_transmission",
                                  tinygltf::Value(std::move(transmission)));

  tinygltf::Value::Object volume;
  volume.emplace("thicknessFactor", tinygltf::Value(0.2));
  volume.emplace("thicknessTexture", textureInfo(6));
  volume.emplace("attenuationColor", numberArray({0.1, 0.2, 0.3}));
  volume.emplace("attenuationDistance", tinygltf::Value(4.0));
  gltfMaterial.extensions.emplace("KHR_materials_volume",
                                  tinygltf::Value(std::move(volume)));

  tinygltf::Value::Object sheen;
  sheen.emplace("sheenColorFactor", numberArray({0.3, 0.2, 0.1}));
  sheen.emplace("sheenColorTexture", textureInfo(7));
  sheen.emplace("sheenRoughnessFactor", tinygltf::Value(0.55));
  sheen.emplace("sheenRoughnessTexture", textureInfo(8));
  gltfMaterial.extensions.emplace("KHR_materials_sheen",
                                  tinygltf::Value(std::move(sheen)));

  tinygltf::Value::Object iridescence;
  iridescence.emplace("iridescenceFactor", tinygltf::Value(0.8));
  iridescence.emplace("iridescenceTexture", textureInfo(9));
  iridescence.emplace("iridescenceIor", tinygltf::Value(1.4));
  iridescence.emplace("iridescenceThicknessMinimum", tinygltf::Value(120.0));
  iridescence.emplace("iridescenceThicknessMaximum", tinygltf::Value(450.0));
  iridescence.emplace("iridescenceThicknessTexture", textureInfo(10));
  gltfMaterial.extensions.emplace("KHR_materials_iridescence",
                                  tinygltf::Value(std::move(iridescence)));

  tinygltf::Value::Object emissiveStrength;
  emissiveStrength.emplace("emissiveStrength", tinygltf::Value(2.5));
  gltfMaterial.extensions.emplace("KHR_materials_emissive_strength",
                                  tinygltf::Value(std::move(emissiveStrength)));

  tinygltf::Value::Object ior;
  ior.emplace("ior", tinygltf::Value(1.33));
  gltfMaterial.extensions.emplace("KHR_materials_ior",
                                  tinygltf::Value(std::move(ior)));

  tinygltf::Value::Object dispersion;
  dispersion.emplace("dispersion", tinygltf::Value(15.0));
  gltfMaterial.extensions.emplace("KHR_materials_dispersion",
                                  tinygltf::Value(std::move(dispersion)));

  gltfMaterial.extensions.emplace(
      "KHR_materials_unlit",
      tinygltf::Value(tinygltf::Value::Object{}));
  model.materials.push_back(std::move(gltfMaterial));

  container::material::SlangMaterialXBridge bridge;
  container::material::MaterialManager materialManager;
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();
  const std::vector<uint32_t> imageToTexture{
      20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u};

  bridge.loadMaterialsForGltf(model, imageToTexture, materialManager,
                              defaultMaterialIndex);

  ASSERT_EQ(materialManager.materialCount(), 1u);
  const auto* material = materialManager.getMaterial(0);
  ASSERT_NE(material, nullptr);

  EXPECT_NEAR(material->specularFactor, 0.5f, 1e-4f);
  EXPECT_NEAR(material->specularColorFactor.r, 0.8f, 1e-4f);
  EXPECT_EQ(material->specularTextureIndex, 20u);
  EXPECT_EQ(material->specularColorTextureIndex, 21u);
  EXPECT_NEAR(material->clearcoatFactor, 0.7f, 1e-4f);
  EXPECT_NEAR(material->clearcoatRoughnessFactor, 0.2f, 1e-4f);
  EXPECT_NEAR(material->clearcoatNormalTextureScale, 0.4f, 1e-4f);
  EXPECT_EQ(material->clearcoatTextureIndex, 22u);
  EXPECT_EQ(material->clearcoatRoughnessTextureIndex, 23u);
  EXPECT_EQ(material->clearcoatNormalTextureIndex, 24u);
  EXPECT_NEAR(material->transmissionFactor, 0.6f, 1e-4f);
  EXPECT_EQ(material->transmissionTextureIndex, 25u);
  EXPECT_NEAR(material->thicknessFactor, 0.2f, 1e-4f);
  EXPECT_EQ(material->thicknessTextureIndex, 26u);
  EXPECT_NEAR(material->attenuationColor.g, 0.2f, 1e-4f);
  EXPECT_NEAR(material->attenuationDistance, 4.0f, 1e-4f);
  EXPECT_NEAR(material->sheenColorFactor.r, 0.3f, 1e-4f);
  EXPECT_NEAR(material->sheenRoughnessFactor, 0.55f, 1e-4f);
  EXPECT_EQ(material->sheenColorTextureIndex, 27u);
  EXPECT_EQ(material->sheenRoughnessTextureIndex, 28u);
  EXPECT_NEAR(material->iridescenceFactor, 0.8f, 1e-4f);
  EXPECT_NEAR(material->iridescenceIor, 1.4f, 1e-4f);
  EXPECT_NEAR(material->iridescenceThicknessMinimum, 120.0f, 1e-4f);
  EXPECT_NEAR(material->iridescenceThicknessMaximum, 450.0f, 1e-4f);
  EXPECT_EQ(material->iridescenceTextureIndex, 29u);
  EXPECT_EQ(material->iridescenceThicknessTextureIndex, 30u);
  EXPECT_NEAR(material->emissiveStrength, 2.5f, 1e-4f);
  EXPECT_NEAR(material->ior, 1.33f, 1e-4f);
  EXPECT_NEAR(material->dispersion, 15.0f, 1e-4f);
  EXPECT_TRUE(material->unlit);
  EXPECT_EQ(material->alphaMode, container::material::AlphaMode::Opaque);
}

TEST(MaterialXIntegration, PhysicalTransmissionPreservesAuthoredAlphaCoverageMode) {
  using container::material::AlphaMode;
  for (const char* mode : {"OPAQUE","MASK","BLEND"}) {
    for (bool textured : {false,true}) {
      SCOPED_TRACE(mode);
      SCOPED_TRACE(textured);
      tinygltf::Model model; model.images.resize(1); model.textures.resize(1);
      model.textures[0].source = 0;
      tinygltf::Material input; input.alphaMode = mode;
      input.pbrMetallicRoughness.baseColorFactor = {1,1,1,.25};
      tinygltf::Value::Object transmission{{"transmissionFactor",tinygltf::Value(.6)}};
      if (textured) transmission["transmissionTexture"] = textureInfo(0);
      input.extensions["KHR_materials_transmission"] = tinygltf::Value(std::move(transmission));
      model.materials.push_back(std::move(input));
      container::material::MaterialManager manager;
      container::material::SlangMaterialXBridge bridge;
      uint32_t fallback = 0;
      bridge.loadMaterialsForGltf(model,std::vector<uint32_t>{7},manager,fallback);
      ASSERT_EQ(manager.materialCount(),1u);
      const auto* material = manager.getMaterial(0); ASSERT_NE(material,nullptr);
      const auto expected = std::string_view(mode) == "OPAQUE" ? AlphaMode::Opaque :
          std::string_view(mode) == "MASK" ? AlphaMode::Mask : AlphaMode::Blend;
      EXPECT_EQ(material->alphaMode,expected);
      EXPECT_FLOAT_EQ(material->transmissionFactor,.6f);
      if (textured) EXPECT_EQ(material->transmissionTextureIndex,7u);
    }
  }
}

TEST(MaterialXIntegration, PreservesGltfDefaultMetallicWithoutInventingDielectricOverride) {
  tinygltf::Model model;
  model.images.resize(1);
  model.textures.resize(1);
  model.textures[0].source = 0;

  tinygltf::Material gltfMaterial;
  gltfMaterial.alphaMode = "BLEND";
  gltfMaterial.doubleSided = true;
  gltfMaterial.pbrMetallicRoughness.baseColorTexture.index = 0;
  model.materials.push_back(std::move(gltfMaterial));

  container::material::SlangMaterialXBridge bridge;
  container::material::MaterialManager materialManager;
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();
  const std::vector<uint32_t> imageToTexture{42u};

  bridge.loadMaterialsForGltf(model, imageToTexture, materialManager,
                              defaultMaterialIndex);

  ASSERT_EQ(materialManager.materialCount(), 1u);
  const auto* material = materialManager.getMaterial(0);
  ASSERT_NE(material, nullptr);

  EXPECT_EQ(material->baseColorTextureIndex, 42u);
  EXPECT_EQ(material->metallicRoughnessTextureIndex,
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(material->metalnessTextureIndex,
            std::numeric_limits<uint32_t>::max());
  EXPECT_FLOAT_EQ(material->metallicFactor, 1.0f);
  EXPECT_NEAR(material->baseColorTextureTransform.offset.x, 0.0f, 1e-6f);
  EXPECT_NEAR(material->baseColorTextureTransform.offset.y, 0.0f, 1e-6f);
  EXPECT_NEAR(material->baseColorTextureTransform.scale.x, 1.0f, 1e-6f);
  EXPECT_NEAR(material->baseColorTextureTransform.scale.y, 1.0f, 1e-6f);
  EXPECT_NEAR(material->baseColorTextureTransform.rotation, 0.0f, 1e-6f);
  EXPECT_EQ(material->baseColorTextureTransform.texCoord, 0u);
}

TEST(MaterialXIntegration, PreservesAuthoredMetallicFactor) {
  tinygltf::Model model;

  tinygltf::Material gltfMaterial;
  gltfMaterial.pbrMetallicRoughness.metallicFactor = 1.0;
  gltfMaterial.values.emplace("metallicFactor", numberParameter(1.0));
  model.materials.push_back(std::move(gltfMaterial));

  container::material::SlangMaterialXBridge bridge;
  container::material::MaterialManager materialManager;
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();

  bridge.loadMaterialsForGltf(model, {}, materialManager, defaultMaterialIndex);

  ASSERT_EQ(materialManager.materialCount(), 1u);
  const auto* material = materialManager.getMaterial(0);
  ASSERT_NE(material, nullptr);

  EXPECT_NEAR(material->metallicFactor, 1.0f, 1e-6f);
}

TEST(MaterialXIntegration, ImportsCoreTextureTransformsAndTexCoordIndices) {
  tinygltf::Model model;
  model.images.resize(5);
  model.textures.resize(5);
  for (size_t i = 0; i < model.textures.size(); ++i) {
    model.textures[i].source = static_cast<int>(i);
  }

  tinygltf::Material gltfMaterial;
  gltfMaterial.pbrMetallicRoughness.baseColorTexture.index = 0;
  gltfMaterial.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
  gltfMaterial.pbrMetallicRoughness.baseColorTexture.extensions.emplace(
      "KHR_texture_transform",
      textureTransform({0.25, 0.5}, 1.25, {2.0, 3.0}, 1));

  gltfMaterial.normalTexture.index = 1;
  gltfMaterial.normalTexture.texCoord = 1;

  gltfMaterial.occlusionTexture.index = 2;
  gltfMaterial.occlusionTexture.texCoord = 1;
  gltfMaterial.occlusionTexture.extensions.emplace(
      "KHR_texture_transform",
      textureTransform({0.1, 0.2}, 0.5, {0.75, 0.5}, 0));

  gltfMaterial.emissiveTexture.index = 3;
  gltfMaterial.emissiveTexture.texCoord = 0;
  gltfMaterial.emissiveTexture.extensions.emplace(
      "KHR_texture_transform",
      textureTransform({0.0, 0.125}, 0.75, {1.0, 1.5}, 1));

  gltfMaterial.pbrMetallicRoughness.metallicRoughnessTexture.index = 4;
  gltfMaterial.pbrMetallicRoughness.metallicRoughnessTexture.texCoord = 1;
  gltfMaterial.pbrMetallicRoughness.metallicRoughnessTexture.extensions.emplace(
      "KHR_texture_transform",
      textureTransform({0.33, 0.44}, -0.25, {0.5, 0.25}, 1));
  model.materials.push_back(std::move(gltfMaterial));

  container::material::SlangMaterialXBridge bridge;
  container::material::MaterialManager materialManager;
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();
  const std::vector<uint32_t> imageToTexture{10u, 11u, 12u, 13u, 14u};

  bridge.loadMaterialsForGltf(model, imageToTexture, materialManager,
                              defaultMaterialIndex);

  ASSERT_EQ(materialManager.materialCount(), 1u);
  const auto* material = materialManager.getMaterial(0);
  ASSERT_NE(material, nullptr);

  EXPECT_EQ(material->baseColorTextureIndex, 10u);
  EXPECT_NEAR(material->baseColorTextureTransform.offset.x, 0.25f, 1e-6f);
  EXPECT_NEAR(material->baseColorTextureTransform.offset.y, 0.5f, 1e-6f);
  EXPECT_NEAR(material->baseColorTextureTransform.rotation, 1.25f, 1e-6f);
  EXPECT_NEAR(material->baseColorTextureTransform.scale.x, 2.0f, 1e-6f);
  EXPECT_NEAR(material->baseColorTextureTransform.scale.y, 3.0f, 1e-6f);
  EXPECT_EQ(material->baseColorTextureTransform.texCoord, 1u);
  const glm::vec2 transformedBaseUv = applyGltfTextureTransform(
      material->baseColorTextureTransform, {0.2f, 0.3f});
  const float cosBase = std::cos(1.25f);
  const float sinBase = std::sin(1.25f);
  EXPECT_NEAR(transformedBaseUv.x,
              2.0f * cosBase * 0.2f - 3.0f * sinBase * 0.3f + 0.25f,
              1e-6f);
  EXPECT_NEAR(transformedBaseUv.y,
              2.0f * sinBase * 0.2f + 3.0f * cosBase * 0.3f + 0.5f,
              1e-6f);

  EXPECT_EQ(material->normalTextureIndex, 11u);
  EXPECT_EQ(material->normalTextureTransform.texCoord, 1u);
  EXPECT_NEAR(material->normalTextureTransform.scale.x, 1.0f, 1e-6f);
  EXPECT_NEAR(material->normalTextureTransform.scale.y, 1.0f, 1e-6f);

  EXPECT_EQ(material->occlusionTextureIndex, 12u);
  EXPECT_EQ(material->occlusionTextureTransform.texCoord, 0u);
  EXPECT_NEAR(material->occlusionTextureTransform.offset.x, 0.1f, 1e-6f);
  EXPECT_NEAR(material->occlusionTextureTransform.offset.y, 0.2f, 1e-6f);
  EXPECT_NEAR(material->occlusionTextureTransform.rotation, 0.5f, 1e-6f);

  EXPECT_EQ(material->emissiveTextureIndex, 13u);
  EXPECT_EQ(material->emissiveTextureTransform.texCoord, 1u);
  EXPECT_NEAR(material->emissiveTextureTransform.rotation, 0.75f, 1e-6f);
  EXPECT_NEAR(material->emissiveTextureTransform.scale.y, 1.5f, 1e-6f);

  EXPECT_EQ(material->metallicRoughnessTextureIndex, 14u);
  EXPECT_EQ(material->metallicRoughnessTextureTransform.texCoord, 1u);
  EXPECT_NEAR(material->metallicRoughnessTextureTransform.offset.x,
              0.33f, 1e-6f);
  EXPECT_NEAR(material->metallicRoughnessTextureTransform.offset.y,
              0.44f, 1e-6f);
  EXPECT_NEAR(material->metallicRoughnessTextureTransform.rotation,
              -0.25f, 1e-6f);
  EXPECT_NEAR(material->metallicRoughnessTextureTransform.scale.x,
              0.5f, 1e-6f);
  EXPECT_NEAR(material->metallicRoughnessTextureTransform.scale.y,
              0.25f, 1e-6f);
}

TEST(MaterialXIntegration, ExtendedMaterialDefaultsAndTextureBackedZeroRemainExact) {
  using namespace container::material;
  tinygltf::Model model;
  model.extensionsUsed = {"KHR_materials_anisotropy", "KHR_materials_diffuse_transmission"};
  model.textures.resize(1);
  model.materials.resize(2);
  model.materials[1].extensions["KHR_materials_anisotropy"] = tinygltf::Value(tinygltf::Value::Object{
      {"anisotropyTexture", textureInfo(0)}});
  model.materials[1].extensions["KHR_materials_diffuse_transmission"] = tinygltf::Value(tinygltf::Value::Object{
      {"diffuseTransmissionFactor", tinygltf::Value(0.0)},
      {"diffuseTransmissionColorFactor", numberArray({0, 0, 0})},
      {"diffuseTransmissionTexture", textureInfo(0)}, {"diffuseTransmissionColorTexture", textureInfo(0)}});
  MaterialManager materials;
  materials.createMaterial(Material{});
  SlangMaterialXBridge bridge;
  uint32_t defaultMaterial = UINT32_MAX;
  bridge.loadMaterialsForGltfWithTextureRoles(model, GltfTextureResources{{17}, {23}}, materials, defaultMaterial);
  ASSERT_EQ(materials.materialCount(), 3u);
  ASSERT_EQ(defaultMaterial, 1u);
  const auto& defaults = *materials.getMaterial(1);
  EXPECT_FLOAT_EQ(defaults.anisotropyStrength, 0);
  EXPECT_FLOAT_EQ(defaults.anisotropyRotation, 0);
  EXPECT_FLOAT_EQ(defaults.diffuseTransmissionFactor, 0);
  EXPECT_EQ(defaults.diffuseTransmissionColorFactor, glm::vec3(1));
  EXPECT_EQ(defaults.anisotropyTextureIndex, UINT32_MAX);
  EXPECT_EQ(defaults.diffuseTransmissionTextureTransform.channel, 3u);
  const auto& textured = *materials.getMaterial(2);
  EXPECT_FLOAT_EQ(textured.anisotropyStrength, 0);
  EXPECT_FLOAT_EQ(textured.diffuseTransmissionFactor, 0);
  EXPECT_EQ(textured.diffuseTransmissionColorFactor, glm::vec3(0));
  EXPECT_EQ(textured.anisotropyTextureIndex, 23u);
  EXPECT_EQ(textured.diffuseTransmissionTextureIndex, 23u);
  EXPECT_EQ(textured.diffuseTransmissionColorTextureIndex, 17u);
  EXPECT_EQ(textured.alphaMode, AlphaMode::Opaque);
}

TEST(MaterialXIntegration, ExtendedSlotsKeepAuthoredTextureIdentityAndIndependentUvTransforms) {
  using namespace container::material;
  tinygltf::Model model;
  model.extensionsUsed = {"KHR_materials_anisotropy", "KHR_materials_diffuse_transmission", "KHR_texture_transform"};
  model.textures.resize(3);
  model.materials.resize(1);
  auto direction = textureInfo(2);
  direction.Get<tinygltf::Value::Object>()["texCoord"] = tinygltf::Value(0);
  direction.Get<tinygltf::Value::Object>()["extensions"] = tinygltf::Value(tinygltf::Value::Object{
      {"KHR_texture_transform", textureTransform({.125, -.25}, .3, {-2, .5}, 1)}});
  auto strength = textureInfo(0);
  strength.Get<tinygltf::Value::Object>()["texCoord"] = tinygltf::Value(1);
  auto color = textureInfo(1);
  color.Get<tinygltf::Value::Object>()["extensions"] = tinygltf::Value(tinygltf::Value::Object{
      {"KHR_texture_transform", textureTransform({.2, .4}, -.7, {.25, -2}, 0)}});
  model.materials[0].extensions["KHR_materials_anisotropy"] = tinygltf::Value(tinygltf::Value::Object{
      {"anisotropyStrength", tinygltf::Value(.7)}, {"anisotropyRotation", tinygltf::Value(-.4)},
      {"anisotropyTexture", direction}});
  model.materials[0].extensions["KHR_materials_diffuse_transmission"] = tinygltf::Value(tinygltf::Value::Object{
      {"diffuseTransmissionFactor", tinygltf::Value(.6)},
      {"diffuseTransmissionColorFactor", numberArray({.2, .3, .4})},
      {"diffuseTransmissionTexture", strength}, {"diffuseTransmissionColorTexture", color}});
  MaterialManager materials;
  SlangMaterialXBridge bridge;
  uint32_t first = UINT32_MAX;
  // Two source entries deliberately alias a data resource. Its reverse lookup
  // cannot identify which color resource belongs to the authored color slot.
  bridge.loadMaterialsForGltfWithTextureRoles(model, GltfTextureResources{{17, 19, 51}, {23, 23, 42}}, materials, first);
  const auto& material = *materials.getMaterial(first);
  EXPECT_FLOAT_EQ(material.anisotropyStrength, .7f);
  EXPECT_FLOAT_EQ(material.anisotropyRotation, -.4f);
  EXPECT_FLOAT_EQ(material.diffuseTransmissionFactor, .6f);
  EXPECT_EQ(material.diffuseTransmissionColorFactor, glm::vec3(.2f, .3f, .4f));
  EXPECT_EQ(material.anisotropyTextureIndex, 42u);
  EXPECT_EQ(material.diffuseTransmissionTextureIndex, 23u);
  EXPECT_EQ(material.diffuseTransmissionColorTextureIndex, 19u);
  EXPECT_EQ(material.anisotropyTextureTransform.offset, glm::vec2(.125f, -.25f));
  EXPECT_EQ(material.anisotropyTextureTransform.scale, glm::vec2(-2, .5f));
  EXPECT_FLOAT_EQ(material.anisotropyTextureTransform.rotation, .3f);
  EXPECT_EQ(material.anisotropyTextureTransform.texCoord, 1u);
  EXPECT_EQ(material.diffuseTransmissionTextureTransform.texCoord, 1u);
  EXPECT_EQ(material.diffuseTransmissionTextureTransform.channel, 3u);
  EXPECT_EQ(material.diffuseTransmissionColorTextureTransform.offset, glm::vec2(.2f, .4f));
  EXPECT_EQ(material.diffuseTransmissionColorTextureTransform.scale, glm::vec2(.25f, -2));
  EXPECT_FLOAT_EQ(material.diffuseTransmissionColorTextureTransform.rotation, -.7f);
  EXPECT_EQ(material.diffuseTransmissionColorTextureTransform.texCoord, 0u);
  EXPECT_EQ(material.alphaMode, AlphaMode::Opaque);
  EXPECT_EQ(model.materials[0].extensions.at("KHR_materials_anisotropy").Get("anisotropyTexture").Get("index").Get<int>(), 2);
}

TEST(MaterialXIntegration, ColorSlotsUseAuthoredTextureIdentityWhenDataHandlesAlias) {
  using namespace container::material;
  tinygltf::Model model;
  model.textures.resize(2);model.images.resize(2);model.materials.resize(2);
  for(int i=0;i<2;++i) {
    model.textures[size_t(i)].source=i;
    model.images[size_t(i)].uri="color-identity-"+std::to_string(i)+".png";
  }
  auto& core=model.materials[0];
  core.pbrMetallicRoughness.baseColorTexture.index=core.emissiveTexture.index=1;
  core.extensions["KHR_materials_specular"]=tinygltf::Value(tinygltf::Value::Object{
      {"specularColorTexture",textureInfo(1)}});
  core.extensions["KHR_materials_sheen"]=tinygltf::Value(tinygltf::Value::Object{
      {"sheenColorTexture",textureInfo(1)}});
  model.materials[1].extensions["KHR_materials_pbrSpecularGlossiness"]=tinygltf::Value(tinygltf::Value::Object{
      {"diffuseTexture",textureInfo(1)},{"specularGlossinessTexture",textureInfo(1)}});
  model.extensionsUsed={"KHR_materials_specular","KHR_materials_sheen","KHR_materials_pbrSpecularGlossiness"};
  MaterialManager materials;
  SlangMaterialXBridge bridge;
  uint32_t first=UINT32_MAX;
  bridge.loadMaterialsForGltfWithTextureRoles(model, GltfTextureResources{{17,19},{23,23}},materials,first);
  ASSERT_EQ(materials.materialCount(),2u);
  const auto& mr=*materials.getMaterial(first);
  EXPECT_EQ(mr.baseColorTextureIndex,19u);
  EXPECT_EQ(mr.emissiveTextureIndex,19u);
  EXPECT_EQ(mr.specularColorTextureIndex,19u);
  EXPECT_EQ(mr.sheenColorTextureIndex,19u);
  const auto& sg=*materials.getMaterial(first+1);
  EXPECT_EQ(sg.baseColorTextureIndex,19u);
  EXPECT_EQ(sg.specularColorTextureIndex,19u);
  EXPECT_EQ(sg.roughnessTextureIndex,19u);
}

TEST(MaterialXIntegration, PackedSpecularGlossinessAliasesOneColorResourceRegardlessOfOtherDataUses) {
  using namespace container::material;
  using Value=tinygltf::Value;using Object=Value::Object;
  for(const bool dataPresent:{false,true}) {
    SCOPED_TRACE(dataPresent);tinygltf::Model model;model.textures.resize(2);model.images.resize(2);model.materials.resize(dataPresent?2:1);
    for(int i=0;i<2;++i) {model.textures[size_t(i)].source=i;model.images[size_t(i)].uri="packed-role-"+std::to_string(i)+".png";}
    auto packed=textureInfo(1);packed.Get<Object>()["texCoord"]=Value(1);
    packed.Get<Object>()["extensions"]=Value(Object{{"KHR_texture_transform",Value(Object{
        {"offset",Value(Value::Array{Value(.25),Value(-.5)})}})}});
    model.materials[0].extensions["KHR_materials_pbrSpecularGlossiness"]=Value(Object{
        {"diffuseTexture",textureInfo(0)},{"specularGlossinessTexture",packed}});
    model.extensionsUsed={"KHR_materials_pbrSpecularGlossiness","KHR_texture_transform"};
    if(dataPresent) model.materials[1].pbrMetallicRoughness.metallicRoughnessTexture.index=1;
    MaterialManager materials;SlangMaterialXBridge bridge;uint32_t first=UINT32_MAX;
    bridge.loadMaterialsForGltfWithTextureRoles(model,GltfTextureResources{{17,19},{UINT32_MAX,dataPresent?23u:UINT32_MAX}},materials,first);
    ASSERT_EQ(materials.materialCount(),dataPresent?2u:1u);const auto& sg=*materials.getMaterial(first);
    EXPECT_TRUE(sg.specularGlossinessWorkflow);EXPECT_EQ(sg.baseColorTextureIndex,17u);
    EXPECT_EQ(sg.roughnessTextureIndex,19u);EXPECT_EQ(sg.specularColorTextureIndex,19u);
    EXPECT_EQ(sg.roughnessTextureTransform.texCoord,1u);EXPECT_EQ(sg.specularColorTextureTransform.texCoord,1u);
    EXPECT_EQ(sg.roughnessTextureTransform.offset,glm::vec2(.25f,-.5f));
    EXPECT_EQ(sg.specularColorTextureTransform.offset,glm::vec2(.25f,-.5f));
    if(dataPresent) EXPECT_EQ(materials.getMaterial(first+1)->metallicRoughnessTextureIndex,23u);
    EXPECT_EQ(model.materials[0].extensions.at("KHR_materials_pbrSpecularGlossiness").Get("specularGlossinessTexture"),packed);
  }
}

TEST(MaterialXIntegration, SpecularGlossinessWithoutPackedTextureKeepsLegacyRoughnessDataResource) {
  using namespace container::material;tinygltf::Model model;model.images.emplace_back();model.textures.emplace_back();model.materials.emplace_back();
  model.images[0].uri="independent-sg-legacy-roughness.png";model.textures[0].source=0;
  model.extensionsUsed={"KHR_materials_pbrSpecularGlossiness"};
  model.materials[0].extensions["KHR_materials_pbrSpecularGlossiness"]=tinygltf::Value(tinygltf::Value::Object{});
  model.materials[0].values["roughnessTexture"].json_double_value["index"]=0;
  MaterialManager materials;SlangMaterialXBridge bridge;uint32_t first=UINT32_MAX;
  bridge.loadMaterialsForGltfWithTextureRoles(model,GltfTextureResources{{19},{23}},materials,first);
  ASSERT_EQ(materials.materialCount(),1u);const auto& material=*materials.getMaterial(first);
  EXPECT_TRUE(material.specularGlossinessWorkflow);EXPECT_EQ(material.roughnessTextureIndex,23u);
  EXPECT_EQ(material.specularColorTextureIndex,UINT32_MAX);
}

TEST(MaterialXIntegration, EveryLegacyTextureAliasInBothMapsImportsCanonicalMappingAndExplicitSource) {
  using namespace container::material;
  struct Slot {const char* name;uint32_t Material::* texture;TextureTransform Material::* transform;};
  const std::array slots{
      Slot{"roughnessTexture",&Material::roughnessTextureIndex,&Material::roughnessTextureTransform},
      Slot{"metalnessTexture",&Material::metalnessTextureIndex,&Material::metalnessTextureTransform},
      Slot{"specularTexture",&Material::specularTextureIndex,&Material::specularTextureTransform},
      Slot{"heightTexture",&Material::heightTextureIndex,&Material::heightTextureTransform},
      Slot{"displacementTexture",&Material::heightTextureIndex,&Material::heightTextureTransform},
      Slot{"opacityTexture",&Material::opacityTextureIndex,&Material::opacityTextureTransform},
      Slot{"refractionTexture",&Material::transmissionTextureIndex,&Material::transmissionTextureTransform},
      Slot{"transmissionTexture",&Material::transmissionTextureIndex,&Material::transmissionTextureTransform}};
  for(const bool additional:{false,true}) for(const auto& slot:slots) {
    SCOPED_TRACE(additional);
    SCOPED_TRACE(slot.name);
    tinygltf::Model model;model.images.resize(3);model.textures.resize(3);model.materials.emplace_back();
    for(size_t i=0;i<3;++i) {model.textures[i].source=int(i);model.images[i].uri="canonical-legacy-"+std::to_string(i)+".png";}
    auto& source=model.materials[0];source.pbrMetallicRoughness.metallicFactor=.4;source.values["metallicFactor"]=numberParameter(.4);
    auto& map=additional?source.additionalValues:source.values;const auto original=legacyMappingParameter(1);map[slot.name]=original;
    const GltfTextureResources roles{{10,11,12},{30,31,32}};
    MaterialManager materials;SlangMaterialXBridge bridge;uint32_t first=UINT32_MAX;
    bridge.loadMaterialsForGltfWithTextureRoles(model,roles,materials,first);ASSERT_EQ(materials.materialCount(),1u);
    const auto& material=*materials.getMaterial(first);const auto& transform=material.*slot.transform;
    EXPECT_EQ(material.*slot.texture,31u);EXPECT_EQ(transform.texCoord,1u);EXPECT_EQ(transform.channel,2u);
    EXPECT_EQ(transform.offset,glm::vec2(.125f,-.25f));EXPECT_EQ(transform.scale,glm::vec2(2.f,-.5f));EXPECT_EQ(transform.rotation,.375f);
    EXPECT_NEAR(material.metallicFactor,.4f,1e-6f);EXPECT_EQ(map.at(slot.name).json_double_value,original.json_double_value);
    const auto live=ConvertGltfMaterial(source,{3,model.extensionsUsed,roles.color,roles.data,50}).value;
    EXPECT_EQ(live.*slot.texture,material.*slot.texture);EXPECT_EQ((live.*slot.transform).offset,transform.offset);
    EXPECT_EQ((live.*slot.transform).scale,transform.scale);EXPECT_EQ((live.*slot.transform).rotation,transform.rotation);
    EXPECT_EQ((live.*slot.transform).texCoord,transform.texCoord);EXPECT_EQ((live.*slot.transform).channel,transform.channel);
  }
}

TEST(MaterialXIntegration, LegacyAliasPriorityPrecedesMapLocationAndKeepsTheWinningTransform) {
  using namespace container::material;tinygltf::Model model;model.images.resize(3);model.textures.resize(3);model.materials.resize(3);
  for(size_t i=0;i<3;++i) {model.textures[i].source=int(i);model.images[i].uri="precedence-"+std::to_string(i)+".png";}
  for(auto& source:model.materials) {
    source.values["displacementTexture"]=legacyMappingParameter(2,3);
    source.additionalValues["displacementTexture"]=legacyMappingParameter(1,2);
    source.values["transmissionTexture"]=legacyMappingParameter(2,3);
    source.additionalValues["transmissionTexture"]=legacyMappingParameter(1,2);
  }
  model.materials[0].values["heightTexture"]=legacyMappingParameter(0,1);
  model.materials[0].additionalValues["heightTexture"]=legacyMappingParameter(1,2);
  model.materials[0].values["refractionTexture"]=legacyMappingParameter(0,1);
  model.materials[0].additionalValues["refractionTexture"]=legacyMappingParameter(1,2);
  model.materials[1].additionalValues["heightTexture"]=legacyMappingParameter(1,2);
  model.materials[1].additionalValues["refractionTexture"]=legacyMappingParameter(1,2);
  MaterialManager materials;SlangMaterialXBridge bridge;uint32_t first=UINT32_MAX;
  bridge.loadMaterialsForGltfWithTextureRoles(model,GltfTextureResources{{10,11,12},{30,31,32}},materials,first);
  ASSERT_EQ(materials.materialCount(),3u);
  for(uint32_t i=0;i<3;++i) {
    SCOPED_TRACE(i);const auto& material=*materials.getMaterial(first+i);
    EXPECT_EQ(material.heightTextureIndex,30+i);EXPECT_EQ(material.transmissionTextureIndex,30+i);
    EXPECT_EQ(material.heightTextureTransform.channel,1+i);EXPECT_EQ(material.transmissionTextureTransform.channel,1+i);
    EXPECT_EQ(material.heightTextureTransform.offset,glm::vec2(.125f,-.25f));
    EXPECT_EQ(material.transmissionTextureTransform.scale,glm::vec2(2.f,-.5f));
  }
}

TEST(MaterialXIntegration, PrimaryPhysicalAndPackedSgTextureTransformsOverrideLegacyFallbackMetadata) {
  using namespace container::material;using Value=tinygltf::Value;using Object=Value::Object;
  for(const bool sg:{false,true}) {
    SCOPED_TRACE(sg);tinygltf::Model model;model.images.resize(2);model.textures.resize(2);model.materials.emplace_back();
    for(size_t i=0;i<2;++i) {model.textures[i].source=int(i);model.images[i].uri="same-primary-and-fallback-uri.png";}
    auto primary=textureInfo(1);primary.Get<Object>()["extensions"]=Value(Object{{"KHR_texture_transform",textureTransform({.25,.5},.125,{.5,2.},1)}});
    auto& source=model.materials[0];source.values["roughnessTexture"]=legacyMappingParameter(0);
    source.values["specularTexture"]=legacyMappingParameter(0);source.additionalValues["refractionTexture"]=legacyMappingParameter(0);
    model.extensionsUsed={"KHR_texture_transform"};
    if(sg) {
      source.extensions["KHR_materials_pbrSpecularGlossiness"]=Value(Object{{"specularGlossinessTexture",primary}});
      model.extensionsUsed.push_back("KHR_materials_pbrSpecularGlossiness");
    } else {
      source.extensions["KHR_materials_specular"]=Value(Object{{"specularTexture",primary},{"specularFactor",Value(0.)}});
      source.extensions["KHR_materials_transmission"]=Value(Object{{"transmissionTexture",primary},{"transmissionFactor",Value(0.)}});
      model.extensionsUsed.push_back("KHR_materials_specular");model.extensionsUsed.push_back("KHR_materials_transmission");
    }
    MaterialManager materials;SlangMaterialXBridge bridge;uint32_t first=UINT32_MAX;
    bridge.loadMaterialsForGltfWithTextureRoles(model,GltfTextureResources{{10,11},{30,31}},materials,first);
    const auto& material=*materials.getMaterial(first);
    for(const auto* transform:sg?std::array{&material.roughnessTextureTransform,&material.specularColorTextureTransform}:
        std::array{&material.specularTextureTransform,&material.transmissionTextureTransform}) {
      EXPECT_EQ(transform->texCoord,1u);EXPECT_EQ(transform->channel,4u);EXPECT_EQ(transform->offset,glm::vec2(.25f,.5f));
      EXPECT_EQ(transform->scale,glm::vec2(.5f,2.f));EXPECT_EQ(transform->rotation,.125f);
    }
    if(sg) {EXPECT_EQ(material.roughnessTextureIndex,11u);EXPECT_EQ(material.specularColorTextureIndex,11u);}
    else {EXPECT_EQ(material.specularTextureIndex,31u);EXPECT_EQ(material.transmissionTextureIndex,31u);
      EXPECT_EQ(material.specularFactor,0.f);EXPECT_EQ(material.transmissionFactor,0.f);}
  }
}

TEST(MaterialXIntegration, InvalidLegacyMappingIncludingIgnoredFallbackRejectsBeforeAnyMaterialPublication) {
  using namespace container::material;
  for(size_t variant=0;variant<6;++variant) {
    SCOPED_TRACE(variant);tinygltf::Model model;model.images.resize(2);model.textures.resize(2);model.materials.resize(2);
    for(size_t i=0;i<2;++i) model.textures[i].source=int(i);
    auto& invalid=model.materials[1].additionalValues["refractionTexture"];invalid=legacyMappingParameter(1);
    if(variant==0) invalid.json_double_value["texCoord"]=2;
    if(variant==1) invalid.json_double_value["channel"]=5;
    if(variant==2) invalid.json_double_value["rotation"]=std::numeric_limits<double>::quiet_NaN();
    if(variant==3) invalid.json_double_value["scaleX"]=std::numeric_limits<double>::max();
    if(variant==4) invalid.json_double_value["index"]=.5;
    if(variant==5) invalid.json_double_value["index"]=2;
    model.materials[1].extensions["KHR_materials_transmission"]=tinygltf::Value(tinygltf::Value::Object{{"transmissionTexture",textureInfo(0)}});
    model.extensionsUsed={"KHR_materials_transmission"};
    MaterialManager materials;materials.createMaterial(Material{});SlangMaterialXBridge bridge;uint32_t first=77;
    EXPECT_THROW(bridge.loadMaterialsForGltfWithTextureRoles(model,GltfTextureResources{{10,11},{30,31}},materials,first),std::invalid_argument);
    EXPECT_EQ(materials.materialCount(),1u);EXPECT_EQ(first,77u);
  }
}

TEST(MaterialXIntegration, ExtendedMaterialValidationPrecedesCallbacksAndMaterialPublication) {
  using namespace container::material;
  tinygltf::Model model;
  model.extensionsUsed = {"KHR_materials_anisotropy"};
  model.images.resize(1);
  model.images[0].uri = "must-not-open-extended-material.png";
  model.textures.resize(1);
  model.textures[0].source = 0;
  model.materials.resize(2);
  model.materials[1].extensions["KHR_materials_anisotropy"] = tinygltf::Value(tinygltf::Value::Object{
      {"anisotropyStrength", tinygltf::Value(2.0)}, {"anisotropyTexture", textureInfo(0)}});
  MaterialManager materials;
  materials.createMaterial(Material{});
  uint32_t first = 0;
  SlangMaterialXBridge bridge;
  EXPECT_THROW(bridge.loadMaterialsForGltf(model, {3}, materials, first), std::runtime_error);
  EXPECT_EQ(materials.materialCount(), 1u);
  EXPECT_EQ(first, 0u);
  TextureManager textures;
  size_t callbacks = 0;
  EXPECT_THROW((void)bridge.loadTexturesForGltf(model, {}, textures,
      [&](const std::string&, bool) { ++callbacks; return TextureResource{}; }), std::runtime_error);
  EXPECT_EQ(callbacks, 0u);
  EXPECT_EQ(textures.textureCount(), 0u);
}

TEST(MaterialXIntegration, LegacyTextureLoaderClassifiesAllThreeExtendedSlots) {
  using namespace container::material;
  tinygltf::Model model;
  model.extensionsUsed = {"KHR_materials_anisotropy", "KHR_materials_diffuse_transmission"};
  model.images.resize(3);
  model.textures.resize(3);
  for (int i = 0; i < 3; ++i) {
    auto& image = model.images[size_t(i)];
    image.width = image.height = 1;
    image.bits = 8;
    image.component = 4;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image = {static_cast<unsigned char>(128 + i), 64, 240, 127};
    model.textures[size_t(i)].source = i;
  }
  model.materials.resize(1);
  model.materials[0].extensions["KHR_materials_anisotropy"] = tinygltf::Value(tinygltf::Value::Object{
      {"anisotropyTexture", textureInfo(0)}});
  model.materials[0].extensions["KHR_materials_diffuse_transmission"] = tinygltf::Value(tinygltf::Value::Object{
      {"diffuseTransmissionTexture", textureInfo(1)}, {"diffuseTransmissionColorTexture", textureInfo(2)}});
  SlangMaterialXBridge bridge;
  TextureManager textures;
  std::vector<bool> roles;
  const auto mapping = bridge.loadTexturesForGltf(model, {}, textures,
      [](const std::string&, bool) { ADD_FAILURE() << "Unexpected file callback"; return TextureResource{}; },
      [&](const std::string&, std::span<const std::byte>, uint32_t, uint32_t, bool isSrgb) {
        roles.push_back(isSrgb);
        return TextureResource{};
      });
  ASSERT_EQ(roles.size(), 3u);
  EXPECT_EQ(roles, (std::vector<bool>{false, false, true}));
  EXPECT_EQ(mapping.size(), 3u);
  for (const auto index : mapping) EXPECT_NE(index, UINT32_MAX);
}
