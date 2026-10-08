#include "Container/utility/MaterialManager.h"
#include "Container/utility/MaterialXIntegration.h"
#include "Container/utility/SceneData.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
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
  EXPECT_EQ(material->alphaMode, container::material::AlphaMode::Blend);
}

TEST(MaterialXIntegration, DefaultsUnspecifiedMetallicToDielectric) {
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
  EXPECT_NEAR(material->metallicFactor, 0.0f, 1e-6f);
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
