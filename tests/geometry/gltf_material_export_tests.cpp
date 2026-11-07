#include <Container/geometry/GltfMaterialExport.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using container::geometry::gltf::appendExportResources;
using container::geometry::gltf::appendMaterialExtensionDeclarations;
using container::geometry::gltf::exportMaterial;
using container::geometry::gltf::snapshotMaterialTextureBindings;
using container::material::Material;
using Value = tinygltf::Value;
using Object = Value::Object;

double number(const tinygltf::Material& material, const char* extension, const char* key) {
  return material.extensions.at(extension).Get(key).GetNumberAsDouble();
}
}  // namespace

TEST(GltfMaterialExport, EmitsRuntimeEditsAndRetainsOptionalMetadata) {
  tinygltf::Material imported;
  imported.name = "Original name";
  imported.extras = Value(Object{{"label", Value(std::string("retain"))}});
  imported.extensions["VENDOR_material_note"] = Value(Object{{"value", Value(7)}});
  Material material;
  material.baseColor = {0.2f, 0.4f, 0.6f, 0.8f};
  material.metallicFactor = 0.3f;
  material.roughnessFactor = 0.7f;
  material.doubleSided = true;
  material.baseColorTextureIndex = 0;
  material.baseColorTextureTransform.texCoord = 1;
  material.baseColorTextureTransform.rotation = 0.25f;
  const std::vector<int32_t> mapping{4};
  const auto result = exportMaterial(material, mapping, &imported);
  EXPECT_EQ(result.name, imported.name);
  EXPECT_EQ(result.extras, imported.extras);
  EXPECT_TRUE(result.extensions.contains("VENDOR_material_note"));
  EXPECT_NEAR(result.pbrMetallicRoughness.baseColorFactor[0], 0.2, 1e-6);
  EXPECT_NEAR(result.pbrMetallicRoughness.metallicFactor, 0.3, 1e-6);
  EXPECT_NEAR(result.pbrMetallicRoughness.roughnessFactor, 0.7, 1e-6);
  EXPECT_TRUE(result.doubleSided);
  EXPECT_EQ(result.pbrMetallicRoughness.baseColorTexture.index, 4);
  EXPECT_EQ(result.pbrMetallicRoughness.baseColorTexture.texCoord, 1);
  EXPECT_TRUE(result.pbrMetallicRoughness.baseColorTexture.extensions.contains("KHR_texture_transform"));
}

TEST(GltfMaterialExport, RejectsNonfiniteAndUnrepresentableMaterialState) {
  Material material;
  material.baseColor.x = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(exportMaterial(material, {}), std::runtime_error);
  material.baseColor.x = 1;
  material.roughnessFactor = 2;
  EXPECT_THROW(exportMaterial(material, {}), std::runtime_error);
  material.roughnessFactor = 1;
  material.specularGlossinessWorkflow = true;
  tinygltf::Material imported;
  EXPECT_THROW(exportMaterial(material, {}, &imported), std::runtime_error);
  material.specularGlossinessWorkflow = false;
  material.transmissionTextureIndex = 0;
  material.transmissionTextureTransform.channel = 2;
  const std::vector<int32_t> mapping{0};
  EXPECT_THROW(exportMaterial(material, mapping), std::runtime_error);
  material.transmissionTextureTransform.channel = 0;
  material.transmissionTextureTransform.texCoord = 2;
  EXPECT_THROW(exportMaterial(material, mapping), std::runtime_error);
  material = Material{};
  material.unlit = true;
  material.emissiveColor = glm::vec3(0.5f);
  EXPECT_THROW(exportMaterial(material, {}), std::runtime_error);
}

TEST(GltfMaterialExport, RetainsTextureTransformMetadataWhileReplacingCurrentParameters) {
  const Value retained(Object{{"author", Value(std::string("preserve"))}});
  const Value original(Object{{"offset", Value(Value::Array{Value(.6), Value(.4)})},
      {"rotation", Value(.3)}, {"texCoord", Value(0)}, {"extras", retained},
      {"extensions", Value(Object{{"VENDOR_note", retained}})}});
  tinygltf::Material imported;
  imported.pbrMetallicRoughness.baseColorTexture.index = 0;
  imported.pbrMetallicRoughness.baseColorTexture.extensions["KHR_texture_transform"] = original;
  imported.extensions["KHR_materials_clearcoat"] = Value(Object{{"clearcoatTexture", Value(Object{
      {"index", Value(0)}, {"extensions", Value(Object{{"KHR_texture_transform", original}})}})}});
  for (float offset : {0.0f, .25f}) {
    Material material;
    material.baseColorTextureIndex = material.clearcoatTextureIndex = 0;
    material.baseColorTextureTransform.texCoord = material.clearcoatTextureTransform.texCoord = 1;
    material.baseColorTextureTransform.offset = material.clearcoatTextureTransform.offset = glm::vec2(offset);
    const std::vector<int32_t> mapping{0};
    const auto result = exportMaterial(material, mapping, &imported);
    const auto& core = result.pbrMetallicRoughness.baseColorTexture;
    const auto& nested = result.extensions.at("KHR_materials_clearcoat").Get("clearcoatTexture");
    EXPECT_EQ(core.texCoord, 1);
    EXPECT_EQ(nested.Get("texCoord").Get<int>(), 1);
    for (const auto* transform : {&core.extensions.at("KHR_texture_transform"),
                                  &nested.Get("extensions").Get("KHR_texture_transform")}) {
      EXPECT_EQ(transform->Get("extras"), retained);
      EXPECT_EQ(transform->Get("extensions").Get("VENDOR_note"), retained);
      EXPECT_FALSE(transform->Has("texCoord"));
      EXPECT_DOUBLE_EQ(transform->Get("offset").Get(0).GetNumberAsDouble(), double(offset));
      EXPECT_DOUBLE_EQ(transform->Get("rotation").GetNumberAsDouble(), 0);
    }
  }
}

TEST(GltfMaterialExport, RemovesResetTexturesAndInfiniteAttenuation) {
  tinygltf::Material imported;
  imported.extensions["KHR_materials_volume"] = Value(Object{
      {"attenuationDistance", Value(2.0)},
      {"thicknessTexture", Value(Object{{"index", Value(0)}})},
      {"extras", Value(Object{{"keep", Value(true)}})}});
  imported.extensions["KHR_materials_transmission"] = Value(Object{
      {"transmissionTexture", Value(Object{{"index", Value(0)}})}});
  const auto result = exportMaterial(Material{}, {}, &imported);
  EXPECT_FALSE(result.extensions.at("KHR_materials_volume").Has("attenuationDistance"));
  EXPECT_FALSE(result.extensions.at("KHR_materials_volume").Has("thicknessTexture"));
  EXPECT_TRUE(result.extensions.at("KHR_materials_volume").Has("extras"));
  EXPECT_FALSE(result.extensions.at("KHR_materials_transmission").Has("transmissionTexture"));
}

TEST(GltfMaterialExport, PreservesTexturedZeroFactorsAndSpecularTextureCompatibility) {
  Material material;
  material.opacityFactor = 0.5f;
  material.baseColor.a = 0.8f;
  material.transmissionTextureIndex = 0;
  material.clearcoatTextureIndex = 0;
  material.clearcoatRoughnessTextureIndex = 0;
  material.sheenColorTextureIndex = 0;
  material.sheenRoughnessTextureIndex = 0;
  material.iridescenceTextureIndex = 0;
  material.specularTextureIndex = 0;
  const std::vector<int32_t> mapping{3};
  const auto result = exportMaterial(material, mapping);
  EXPECT_NEAR(result.pbrMetallicRoughness.baseColorFactor[3], 0.4, 1e-6);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_transmission", "transmissionFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_clearcoat", "clearcoatFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_clearcoat", "clearcoatRoughnessFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_sheen", "sheenRoughnessFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_iridescence", "iridescenceFactor"), 0);
  const auto& sheen = result.extensions.at("KHR_materials_sheen").Get("sheenColorFactor");
  for (size_t i = 0; i < 3; ++i) EXPECT_DOUBLE_EQ(sheen.Get(i).GetNumberAsDouble(), 0);
  EXPECT_EQ(result.extensions.at("KHR_materials_specular").Get("specularColorTexture").Get("index").Get<int>(), 3);
}

TEST(GltfMaterialExport, TexturedCoreAndExtensionZeroFactorsAndBlackColorsExportWithoutBaking) {
  Material material;
  material.baseColor = {0, 0, 0, 1};
  material.metallicFactor = material.roughnessFactor = material.specularFactor = 0;
  material.specularColorFactor = glm::vec3(0);
  material.baseColorTextureIndex = material.metallicRoughnessTextureIndex =
      material.emissiveTextureIndex = material.specularTextureIndex = material.specularColorTextureIndex =
      material.transmissionTextureIndex = material.clearcoatTextureIndex = material.clearcoatRoughnessTextureIndex =
      material.sheenColorTextureIndex = material.sheenRoughnessTextureIndex = material.iridescenceTextureIndex = 0;
  const auto result = exportMaterial(material, std::array<int32_t, 1>{5});
  EXPECT_DOUBLE_EQ(result.pbrMetallicRoughness.metallicFactor, 0);
  EXPECT_DOUBLE_EQ(result.pbrMetallicRoughness.roughnessFactor, 0);
  EXPECT_EQ(result.pbrMetallicRoughness.baseColorFactor, (std::vector<double>{0, 0, 0, 1}));
  EXPECT_EQ(result.emissiveFactor, (std::vector<double>{0, 0, 0}));
  EXPECT_EQ(result.pbrMetallicRoughness.baseColorTexture.index, 5);
  EXPECT_EQ(result.pbrMetallicRoughness.metallicRoughnessTexture.index, 5);
  EXPECT_EQ(result.emissiveTexture.index, 5);
  for (const auto& [extension, leaf] : {
      std::pair{"KHR_materials_specular", "specularFactor"},
      std::pair{"KHR_materials_transmission", "transmissionFactor"},
      std::pair{"KHR_materials_clearcoat", "clearcoatFactor"},
      std::pair{"KHR_materials_clearcoat", "clearcoatRoughnessFactor"},
      std::pair{"KHR_materials_sheen", "sheenRoughnessFactor"},
      std::pair{"KHR_materials_iridescence", "iridescenceFactor"}})
    EXPECT_DOUBLE_EQ(number(result, extension, leaf), 0) << leaf;
  for (const auto& [extension, leaf] : {
      std::pair{"KHR_materials_specular", "specularColorFactor"},
      std::pair{"KHR_materials_sheen", "sheenColorFactor"}}) {
    const auto& color = result.extensions.at(extension).Get(leaf);
    for (size_t i = 0; i < 3; ++i) EXPECT_DOUBLE_EQ(color.Get(i).GetNumberAsDouble(), 0) << leaf;
  }
  for (const auto& [extension, slot] : {
      std::pair{"KHR_materials_specular", "specularTexture"},
      std::pair{"KHR_materials_specular", "specularColorTexture"},
      std::pair{"KHR_materials_transmission", "transmissionTexture"},
      std::pair{"KHR_materials_clearcoat", "clearcoatTexture"},
      std::pair{"KHR_materials_clearcoat", "clearcoatRoughnessTexture"},
      std::pair{"KHR_materials_sheen", "sheenColorTexture"},
      std::pair{"KHR_materials_sheen", "sheenRoughnessTexture"},
      std::pair{"KHR_materials_iridescence", "iridescenceTexture"}})
    EXPECT_EQ(result.extensions.at(extension).Get(slot).Get("index").Get<int>(), 5) << slot;
}

TEST(GltfMaterialExport, ZeroFactorsRetainSlotSpecificAliasProvenanceAndUnknownMetadata) {
  using Member = uint32_t Material::*;
  struct Slot { const char* extension; const char* name; Member member; };
  const std::array slots{
      Slot{"KHR_materials_transmission", "transmissionTexture", &Material::transmissionTextureIndex},
      Slot{"KHR_materials_clearcoat", "clearcoatTexture", &Material::clearcoatTextureIndex},
      Slot{"KHR_materials_clearcoat", "clearcoatRoughnessTexture", &Material::clearcoatRoughnessTextureIndex},
      Slot{"KHR_materials_sheen", "sheenColorTexture", &Material::sheenColorTextureIndex},
      Slot{"KHR_materials_sheen", "sheenRoughnessTexture", &Material::sheenRoughnessTextureIndex},
      Slot{"KHR_materials_iridescence", "iridescenceTexture", &Material::iridescenceTextureIndex},
      Slot{"KHR_materials_specular", "specularTexture", &Material::specularTextureIndex},
      Slot{"KHR_materials_specular", "specularColorTexture", &Material::specularColorTextureIndex}};
  const Value metadata(Object{{"owner", Value(std::string("retained source"))}, {"opaqueValue", Value(.125)}});
  tinygltf::Material imported;
  imported.extras = metadata;
  imported.extensions["VENDOR_material_identity"] = metadata;
  Material material;
  material.metallicFactor = material.roughnessFactor = material.specularFactor = 0;
  material.specularColorFactor = glm::vec3(0);
  material.metallicRoughnessTextureIndex = 7;
  imported.pbrMetallicRoughness.metallicRoughnessTexture.index = 2;
  imported.pbrMetallicRoughness.metallicRoughnessTexture.extras = metadata;
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    material.*slot.member = 7; // Different source texture objects share pixels.
    auto& extension = imported.extensions[slot.extension];
    if (!extension.IsObject()) extension = Value(Object{{"extras", metadata}, {"VENDOR_property", metadata}});
    extension.Get<Object>()[slot.name] = Value(Object{{"index", Value(int(i+3))}, {"extras", metadata},
        {"extensions", Value(Object{{"VENDOR_texture_info", metadata}})}});
  }
  const auto bindings = snapshotMaterialTextureBindings(material);
  std::vector<int32_t> mapping(9, -1); mapping[7] = 40; mapping[8] = 41;
  for (bool changed : {false, true}) {
    material.transmissionTextureIndex = changed ? 8 : 7;
    const auto result = exportMaterial(material, mapping, &imported, &bindings);
    EXPECT_DOUBLE_EQ(result.pbrMetallicRoughness.metallicFactor, 0);
    EXPECT_DOUBLE_EQ(result.pbrMetallicRoughness.roughnessFactor, 0);
    EXPECT_EQ(result.pbrMetallicRoughness.metallicRoughnessTexture.index, 2);
    EXPECT_EQ(result.pbrMetallicRoughness.metallicRoughnessTexture.extras, metadata);
    EXPECT_EQ(result.extras, metadata);
    EXPECT_EQ(result.extensions.at("VENDOR_material_identity"), metadata);
    for (size_t i = 0; i < slots.size(); ++i) {
      const auto& extension = result.extensions.at(slots[i].extension);
      const auto& texture = extension.Get(slots[i].name);
      EXPECT_EQ(texture.Get("index").Get<int>(), changed && i == 0 ? 41 : int(i+3));
      EXPECT_EQ(texture.Get("extras"), metadata);
      EXPECT_EQ(texture.Get("extensions").Get("VENDOR_texture_info"), metadata);
      EXPECT_EQ(extension.Get("extras"), metadata);
      EXPECT_EQ(extension.Get("VENDOR_property"), metadata);
    }
    EXPECT_DOUBLE_EQ(number(result, "KHR_materials_transmission", "transmissionFactor"), 0);
    EXPECT_DOUBLE_EQ(number(result, "KHR_materials_clearcoat", "clearcoatFactor"), 0);
    EXPECT_DOUBLE_EQ(number(result, "KHR_materials_clearcoat", "clearcoatRoughnessFactor"), 0);
    EXPECT_DOUBLE_EQ(number(result, "KHR_materials_sheen", "sheenRoughnessFactor"), 0);
    EXPECT_DOUBLE_EQ(number(result, "KHR_materials_iridescence", "iridescenceFactor"), 0);
    for (const auto& [extension, leaf] : {
        std::pair{"KHR_materials_sheen", "sheenColorFactor"},
        std::pair{"KHR_materials_specular", "specularColorFactor"}})
      for (size_t i = 0; i < 3; ++i)
        EXPECT_DOUBLE_EQ(result.extensions.at(extension).Get(leaf).Get(i).GetNumberAsDouble(), 0);
  }
  EXPECT_EQ(imported.extensions.at("KHR_materials_transmission").Get("transmissionTexture").Get("index").Get<int>(), 3);
  EXPECT_EQ(bindings.runtimeIndices[10], 7u);
}

TEST(GltfMaterialExport, EnabledExtensionsUseSchemaDefaultsWithoutInferringEffectsFromTextures) {
  const Value metadata(Object{{"author", Value(std::string("enabled at defaults"))}});
  tinygltf::Material imported;
  for (const auto* extension : {"KHR_materials_transmission", "KHR_materials_clearcoat",
      "KHR_materials_sheen", "KHR_materials_iridescence", "KHR_materials_specular"})
    imported.extensions[extension] = Value(Object{{"extras", metadata}});
  const auto result = exportMaterial(Material{}, {}, &imported);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_transmission", "transmissionFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_clearcoat", "clearcoatFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_clearcoat", "clearcoatRoughnessFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_sheen", "sheenRoughnessFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_iridescence", "iridescenceFactor"), 0);
  EXPECT_DOUBLE_EQ(number(result, "KHR_materials_specular", "specularFactor"), 1);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(result.extensions.at("KHR_materials_sheen").Get("sheenColorFactor").Get(i).GetNumberAsDouble(), 0);
    EXPECT_DOUBLE_EQ(result.extensions.at("KHR_materials_specular").Get("specularColorFactor").Get(i).GetNumberAsDouble(), 1);
  }
  for (const auto* extension : {"KHR_materials_transmission", "KHR_materials_clearcoat",
      "KHR_materials_sheen", "KHR_materials_iridescence", "KHR_materials_specular"})
    EXPECT_EQ(result.extensions.at(extension).Get("extras"), metadata);
  const auto absent = exportMaterial(Material{}, {});
  for (const auto* extension : {"KHR_materials_transmission", "KHR_materials_clearcoat",
      "KHR_materials_sheen", "KHR_materials_iridescence", "KHR_materials_specular"})
    EXPECT_FALSE(absent.extensions.contains(extension));
}

TEST(GltfMaterialExport, ZeroFactorsDoNotBypassCustomChannelOrInputBakingGuards) {
  Material material;
  material.transmissionTextureIndex = 0;
  material.transmissionTextureTransform.channel = 2;
  EXPECT_THROW(exportMaterial(material, std::array<int32_t, 1>{0}), std::runtime_error);
  material = Material{};
  material.clearcoatRoughnessTextureIndex = 0;
  material.clearcoatRoughnessTextureTransform.channel = 0;
  EXPECT_THROW(exportMaterial(material, std::array<int32_t, 1>{0}), std::runtime_error);
  material = Material{};
  material.roughnessFactor = material.metallicFactor = 0;
  material.metalnessTextureIndex = 0;
  EXPECT_THROW(exportMaterial(material, std::array<int32_t, 1>{0}), std::runtime_error);
}

TEST(GltfMaterialExport, CollectsGeneratedMaterialAndTextureTransformDeclarations) {
  Material material;
  material.transmissionFactor = 0.3f;
  material.baseColorTextureIndex = 0;
  material.baseColorTextureTransform.rotation = 0.3f;
  const std::vector<int32_t> mapping{0};
  tinygltf::Model model;
  model.materials.push_back(exportMaterial(material, mapping));
  appendMaterialExtensionDeclarations(model);
  EXPECT_NE(std::ranges::find(model.extensionsUsed, "KHR_materials_transmission"), model.extensionsUsed.end());
  EXPECT_NE(std::ranges::find(model.extensionsUsed, "KHR_texture_transform"), model.extensionsUsed.end());
  const auto count = model.extensionsUsed.size();
  appendMaterialExtensionDeclarations(model);
  EXPECT_EQ(model.extensionsUsed.size(), count);
}

TEST(GltfMaterialExport, RetainsNestedTextureMetadataWhileReplacingAuthoredTransform) {
  tinygltf::Material imported;
  Object textureExtensions;
  textureExtensions["KHR_texture_transform"] = Value(Object{{"rotation", Value(1.0)}});
  textureExtensions["VENDOR_texture_note"] = Value(Object{{"tag", Value(42)}});
  Object textureInfo{{"index", Value(8)}, {"texCoord", Value(0)},
      {"extras", Value(Object{{"keep", Value(true)}})},
      {"extensions", Value(std::move(textureExtensions))}};
  imported.extensions["KHR_materials_transmission"] = Value(Object{
      {"transmissionTexture", Value(std::move(textureInfo))}});
  Material material;
  material.transmissionTextureIndex = 0;
  material.transmissionTextureTransform.texCoord = 1;
  const std::vector<int32_t> mapping{3};
  const auto result = exportMaterial(material, mapping, &imported);
  const auto& texture = result.extensions.at("KHR_materials_transmission").Get("transmissionTexture");
  EXPECT_EQ(texture.Get("index").Get<int>(), 3);
  EXPECT_EQ(texture.Get("texCoord").Get<int>(), 1);
  EXPECT_TRUE(texture.Get("extras").Get("keep").Get<bool>());
  EXPECT_TRUE(texture.Get("extensions").Has("VENDOR_texture_note"));
  const auto& transform = texture.Get("extensions").Get("KHR_texture_transform");
  EXPECT_DOUBLE_EQ(transform.Get("rotation").GetNumberAsDouble(), 0);
  EXPECT_DOUBLE_EQ(transform.Get("scale").Get(0).GetNumberAsDouble(), 1);
}

TEST(GltfMaterialExport, RejectsInvalidResourceReferencesBeforeAppendingResources) {
  tinygltf::Model destination;
  tinygltf::Model source;
  source.buffers.emplace_back();
  source.bufferViews.emplace_back();
  source.bufferViews[0].buffer = 9;
  EXPECT_THROW(appendExportResources(destination, source, {}), std::runtime_error);
  EXPECT_TRUE(destination.buffers.empty());
  EXPECT_TRUE(destination.bufferViews.empty());
  EXPECT_THROW(appendExportResources(source, source, {}), std::runtime_error);
}

TEST(GltfMaterialExport, RemapsDdsImageSourceAndRetainsExtensionMetadata) {
  tinygltf::Model destination, source;
  destination.images.resize(3); destination.textures.resize(1);
  source.images.resize(2); source.textures.emplace_back();
  source.textures[0].source = 0;
  source.textures[0].extensions["MSFT_texture_dds"] = Value(Object{
      {"source", Value(1)}, {"extras", Value(Object{{"keep", Value(true)}})}});
  source.extensionsUsed = source.extensionsRequired = {"MSFT_texture_dds"};
  (void)appendExportResources(destination, source, {});
  EXPECT_EQ(destination.textures.at(1).source, 3);
  const auto& extension = destination.textures.at(1).extensions.at("MSFT_texture_dds");
  EXPECT_EQ(extension.Get("source").Get<int>(), 4);
  EXPECT_TRUE(extension.Get("extras").Get("keep").Get<bool>());
  EXPECT_EQ(source.textures[0].extensions.at("MSFT_texture_dds").Get("source").Get<int>(), 1);
  source.textures[0].extensions.at("MSFT_texture_dds").Get<Object>()["source"] = Value(2);
  const auto oldSize = destination.images.size();
  EXPECT_THROW((void)appendExportResources(destination, source, {}), std::runtime_error);
  EXPECT_EQ(destination.images.size(), oldSize);
}

TEST(GltfMaterialExport, RemapsCombinedImageSamplerTextureAndMaterialResources) {
  tinygltf::Model destination;
  destination.buffers.emplace_back();
  destination.bufferViews.emplace_back();
  destination.images.emplace_back();
  destination.samplers.emplace_back();
  destination.textures.emplace_back();
  tinygltf::Model source;
  source.buffers.emplace_back();
  source.buffers[0].data = {1, 2, 3, 4};
  source.bufferViews.emplace_back();
  source.bufferViews[0].buffer = 0;
  source.bufferViews[0].byteLength = 4;
  source.images.emplace_back();
  source.images[0].bufferView = 0;
  source.samplers.emplace_back();
  source.textures.emplace_back();
  source.textures[0].source = 0;
  source.textures[0].sampler = 0;
  source.materials.emplace_back();
  source.materials[0].pbrMetallicRoughness.baseColorTexture.index = 0;
  source.materials[0].extensions["KHR_materials_transmission"] = Value(Object{
      {"transmissionTexture", Value(Object{{"index", Value(0)}})}});
  source.extensionsUsed.push_back("KHR_materials_transmission");
  source.extensionsRequired.push_back("KHR_materials_transmission");
  const auto materials = appendExportResources(destination, source, {});
  EXPECT_EQ(destination.bufferViews[1].buffer, 1);
  EXPECT_EQ(destination.images[1].bufferView, 1);
  EXPECT_EQ(destination.textures[1].source, 1);
  EXPECT_EQ(destination.textures[1].sampler, 1);
  EXPECT_EQ(materials[0].pbrMetallicRoughness.baseColorTexture.index, 1);
  EXPECT_EQ(materials[0].extensions.at("KHR_materials_transmission").Get("transmissionTexture").Get("index").Get<int>(), 1);
  EXPECT_EQ(destination.extensionsRequired, source.extensionsRequired);
}

TEST(GltfMaterialExport, DeduplicatedResourcesRetainSlotSpecificTextureImageAndSamplerIdentity) {
  tinygltf::Model source;
  for (const auto* identity : {"normal", "occlusion"}) {
    const Value metadata(Object{{"identity", Value(std::string(identity))}});
    tinygltf::Image image;
    image.width = image.height = 1; image.bits = 8; image.component = 4;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE; image.image = {128, 64, 240, 127};
    image.name = identity; image.extras = metadata;
    image.extensions["VENDOR_image_identity"] = metadata;
    source.images.push_back(std::move(image));
    tinygltf::Sampler sampler;
    sampler.name = identity; sampler.extras = metadata;
    sampler.extensions["VENDOR_sampler_identity"] = metadata;
    source.samplers.push_back(std::move(sampler));
    tinygltf::Texture texture;
    texture.source = texture.sampler = static_cast<int>(source.textures.size());
    texture.name = identity; texture.extras = metadata;
    texture.extensions["VENDOR_texture_identity"] = metadata;
    source.textures.push_back(std::move(texture));
  }
  source.materials.emplace_back();
  source.materials[0].normalTexture.index = 0;
  source.materials[0].occlusionTexture.index = 1;
  tinygltf::Model destination;
  destination.images.emplace_back(); destination.samplers.emplace_back(); destination.textures.emplace_back();
  const auto originals = appendExportResources(destination, source, {});
  Material runtime;
  runtime.normalTextureIndex = runtime.occlusionTextureIndex = 7; // Equal pixels and sampler deduplicate.
  const auto provenance = snapshotMaterialTextureBindings(runtime);
  std::vector<int32_t> fallback(8, -1); fallback[7] = 2; // Last source wins in the lossy inverse map.
  const auto result = exportMaterial(runtime, fallback, &originals[0], &provenance);
  EXPECT_EQ(result.normalTexture.index, 1);
  EXPECT_EQ(result.occlusionTexture.index, 2);
  for (const auto& [index, expected] : {std::pair{result.normalTexture.index, 0},
                                      std::pair{result.occlusionTexture.index, 1}}) {
    const auto& texture = destination.textures.at(index);
    EXPECT_EQ(texture.name, source.textures[expected].name);
    EXPECT_EQ(texture.extras, source.textures[expected].extras);
    EXPECT_EQ(texture.extensions, source.textures[expected].extensions);
    EXPECT_EQ(destination.images.at(texture.source).extras, source.images[expected].extras);
    EXPECT_EQ(destination.images.at(texture.source).extensions, source.images[expected].extensions);
    EXPECT_EQ(destination.samplers.at(texture.sampler).extras, source.samplers[expected].extras);
    EXPECT_EQ(destination.samplers.at(texture.sampler).extensions, source.samplers[expected].extensions);
  }
  // A real binding edit uses the resource map, while untouched slots keep identity.
  runtime.normalTextureIndex = 3; fallback[3] = 0;
  const auto edited = exportMaterial(runtime, fallback, &originals[0], &provenance);
  EXPECT_EQ(edited.normalTexture.index, 0);
  EXPECT_EQ(edited.occlusionTexture.index, 2);
  EXPECT_EQ(originals[0].normalTexture.index, 1);
  EXPECT_EQ(source.materials[0].normalTexture.index, 0);
}

TEST(GltfMaterialExport, PreservesEveryImportedCoreAndExtensionSlotWithSharedRuntimeIdentity) {
  tinygltf::Material imported;
  Material runtime;
  int index{};
  imported.pbrMetallicRoughness.baseColorTexture.index = index++; runtime.baseColorTextureIndex = 0;
  imported.pbrMetallicRoughness.metallicRoughnessTexture.index = index++; runtime.metallicRoughnessTextureIndex = 0;
  imported.normalTexture.index = index++; runtime.normalTextureIndex = 0;
  imported.occlusionTexture.index = index++; runtime.occlusionTextureIndex = 0;
  imported.emissiveTexture.index = index++; runtime.emissiveTextureIndex = 0;
  using Member = uint32_t Material::*;
  struct Slot { const char* extension; const char* name; Member member; };
  const std::array slots{
      Slot{"KHR_materials_specular", "specularTexture", &Material::specularTextureIndex},
      Slot{"KHR_materials_specular", "specularColorTexture", &Material::specularColorTextureIndex},
      Slot{"KHR_materials_transmission", "transmissionTexture", &Material::transmissionTextureIndex},
      Slot{"KHR_materials_volume", "thicknessTexture", &Material::thicknessTextureIndex},
      Slot{"KHR_materials_clearcoat", "clearcoatTexture", &Material::clearcoatTextureIndex},
      Slot{"KHR_materials_clearcoat", "clearcoatRoughnessTexture", &Material::clearcoatRoughnessTextureIndex},
      Slot{"KHR_materials_clearcoat", "clearcoatNormalTexture", &Material::clearcoatNormalTextureIndex},
      Slot{"KHR_materials_sheen", "sheenColorTexture", &Material::sheenColorTextureIndex},
      Slot{"KHR_materials_sheen", "sheenRoughnessTexture", &Material::sheenRoughnessTextureIndex},
      Slot{"KHR_materials_iridescence", "iridescenceTexture", &Material::iridescenceTextureIndex},
      Slot{"KHR_materials_iridescence", "iridescenceThicknessTexture", &Material::iridescenceThicknessTextureIndex},
      Slot{"KHR_materials_anisotropy", "anisotropyTexture", &Material::anisotropyTextureIndex},
      Slot{"KHR_materials_diffuse_transmission", "diffuseTransmissionTexture", &Material::diffuseTransmissionTextureIndex},
      Slot{"KHR_materials_diffuse_transmission", "diffuseTransmissionColorTexture", &Material::diffuseTransmissionColorTextureIndex}};
  for (const auto& slot : slots) {
    auto& extension = imported.extensions[slot.extension];
    if (!extension.IsObject()) extension = Value(Object{});
    extension.Get<Object>()[slot.name] = Value(Object{{"index", Value(index++)}, {"extras", Value(Object{{"keep", Value(true)}})}});
    runtime.*slot.member = 0;
  }
  const auto provenance = snapshotMaterialTextureBindings(runtime);
  const auto result = exportMaterial(runtime, std::array<int32_t, 1>{99}, &imported, &provenance);
  EXPECT_EQ(result.pbrMetallicRoughness.baseColorTexture.index, 0);
  EXPECT_EQ(result.pbrMetallicRoughness.metallicRoughnessTexture.index, 1);
  EXPECT_EQ(result.normalTexture.index, 2); EXPECT_EQ(result.occlusionTexture.index, 3);
  EXPECT_EQ(result.emissiveTexture.index, 4);
  index = 5;
  for (const auto& slot : slots) {
    const auto& info = result.extensions.at(slot.extension).Get(slot.name);
    EXPECT_EQ(info.Get("index").Get<int>(), index++) << slot.name;
    EXPECT_TRUE(info.Get("extras").Get("keep").Get<bool>()) << slot.name;
  }
  runtime.transmissionTextureIndex = 2;
  const auto changed = exportMaterial(runtime, std::array<int32_t, 3>{99, -1, 37}, &imported, &provenance);
  EXPECT_EQ(changed.extensions.at("KHR_materials_transmission").Get("transmissionTexture").Get("index").Get<int>(), 37);
  runtime.transmissionTextureIndex = std::numeric_limits<uint32_t>::max();
  const auto cleared = exportMaterial(runtime, std::array<int32_t, 1>{99}, &imported, &provenance);
  EXPECT_FALSE(cleared.extensions.at("KHR_materials_transmission").Has("transmissionTexture"));
}

TEST(GltfMaterialExport, ExtendedPbrZeroFactorsAndBlackColorStayZeroWithTextures) {
  tinygltf::Material imported;
  imported.extensions["KHR_materials_anisotropy"] = Value(Object{
      {"anisotropyStrength",Value(0.0)}, {"anisotropyRotation",Value(0.0)}});
  imported.extensions["KHR_materials_diffuse_transmission"] = Value(Object{
      {"diffuseTransmissionFactor",Value(0.0)}});
  Material runtime;
  runtime.anisotropyTextureIndex = runtime.diffuseTransmissionTextureIndex =
      runtime.diffuseTransmissionColorTextureIndex = 0;
  runtime.diffuseTransmissionColorFactor = glm::vec3(0);
  const auto result = exportMaterial(runtime,std::array<int32_t,1>{7},&imported);
  EXPECT_DOUBLE_EQ(number(result,"KHR_materials_anisotropy","anisotropyStrength"),0);
  EXPECT_DOUBLE_EQ(number(result,"KHR_materials_diffuse_transmission","diffuseTransmissionFactor"),0);
  const auto& color = result.extensions.at("KHR_materials_diffuse_transmission").Get("diffuseTransmissionColorFactor");
  for (size_t i = 0; i < 3; ++i) EXPECT_DOUBLE_EQ(color.Get(i).GetNumberAsDouble(),0);
  for (const auto& [extension,texture] : {
      std::pair{"KHR_materials_anisotropy","anisotropyTexture"},
      std::pair{"KHR_materials_diffuse_transmission","diffuseTransmissionTexture"},
      std::pair{"KHR_materials_diffuse_transmission","diffuseTransmissionColorTexture"}})
    EXPECT_EQ(result.extensions.at(extension).Get(texture).Get("index").Get<int>(),7);
  tinygltf::Model model; model.materials.push_back(result);
  appendMaterialExtensionDeclarations(model);
  for (const auto* extension : {"KHR_materials_anisotropy","KHR_materials_diffuse_transmission"})
    EXPECT_NE(std::ranges::find(model.extensionsUsed,extension),model.extensionsUsed.end());
}

TEST(GltfMaterialExport, ExtendedPbrRetainsAuthoredDefaultPresenceAndUnknownMetadata) {
  const Value metadata(Object{{"keep",Value(std::string("authored"))}});
  tinygltf::Material imported;
  for (const auto* extension : {"KHR_materials_anisotropy","KHR_materials_diffuse_transmission"})
    imported.extensions[extension] = Value(Object{{"extras",metadata},{"VENDOR_property",metadata}});
  const auto absent = exportMaterial(Material{}, {}, &imported);
  EXPECT_EQ(absent.extensions,imported.extensions);
  imported.extensions["KHR_materials_anisotropy"].Get<Object>()["anisotropyStrength"] = Value(0.0);
  imported.extensions["KHR_materials_anisotropy"].Get<Object>()["anisotropyRotation"] = Value(0.0);
  imported.extensions["KHR_materials_diffuse_transmission"].Get<Object>()["diffuseTransmissionFactor"] = Value(0.0);
  imported.extensions["KHR_materials_diffuse_transmission"].Get<Object>()["diffuseTransmissionColorFactor"] =
      Value(Value::Array{Value(1.0),Value(1.0),Value(1.0)});
  const auto explicitDefaults = exportMaterial(Material{}, {}, &imported);
  EXPECT_EQ(explicitDefaults.extensions,imported.extensions);
  const auto generated = exportMaterial(Material{}, {});
  EXPECT_FALSE(generated.extensions.contains("KHR_materials_anisotropy"));
  EXPECT_FALSE(generated.extensions.contains("KHR_materials_diffuse_transmission"));
  Material unlit; unlit.unlit = true;
  const auto excluded = exportMaterial(unlit,{},&imported);
  EXPECT_FALSE(excluded.extensions.contains("KHR_materials_anisotropy"));
  EXPECT_FALSE(excluded.extensions.contains("KHR_materials_diffuse_transmission"));
}

TEST(GltfMaterialExport, ExtendedPbrRejectsNonfiniteRangesAndUnrepresentableChannels) {
  Material runtime;
  runtime.anisotropyStrength = -0.1f; EXPECT_THROW(exportMaterial(runtime,{}),std::runtime_error);
  runtime = Material{}; runtime.anisotropyStrength = 1.1f; EXPECT_THROW(exportMaterial(runtime,{}),std::runtime_error);
  runtime = Material{}; runtime.anisotropyRotation = std::numeric_limits<float>::infinity();
  EXPECT_THROW(exportMaterial(runtime,{}),std::runtime_error);
  runtime = Material{}; runtime.diffuseTransmissionFactor = 1.1f; EXPECT_THROW(exportMaterial(runtime,{}),std::runtime_error);
  runtime = Material{}; runtime.diffuseTransmissionColorFactor.y = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(exportMaterial(runtime,{}),std::runtime_error);
  runtime = Material{}; runtime.diffuseTransmissionColorFactor.z = -0.1f; EXPECT_THROW(exportMaterial(runtime,{}),std::runtime_error);
  const std::array<int32_t,1> mapping{0};
  runtime = Material{}; runtime.diffuseTransmissionTextureIndex = 0;
  for (uint32_t channel : {0u,1u,2u}) {
    runtime.diffuseTransmissionTextureTransform.channel = channel;
    EXPECT_THROW(exportMaterial(runtime,mapping),std::runtime_error);
  }
  for (uint32_t channel : {3u,4u}) {
    runtime.diffuseTransmissionTextureTransform.channel = channel;
    EXPECT_NO_THROW(exportMaterial(runtime,mapping));
  }
  runtime = Material{}; runtime.anisotropyTextureIndex = runtime.diffuseTransmissionColorTextureIndex = 0;
  for (uint32_t channel : {0u,1u,2u,3u}) {
    runtime.anisotropyTextureTransform.channel = channel;
    EXPECT_THROW(exportMaterial(runtime,mapping),std::runtime_error);
  }
  runtime.anisotropyTextureTransform.channel = 4;
  runtime.diffuseTransmissionColorTextureTransform.channel = 1;
  EXPECT_THROW(exportMaterial(runtime,mapping),std::runtime_error);
  runtime.diffuseTransmissionColorTextureTransform.channel = 4;
  runtime.anisotropyTextureTransform.texCoord = 2;
  EXPECT_THROW(exportMaterial(runtime,mapping),std::runtime_error);
}

TEST(GltfMaterialExport, ExtendedPbrDedupRetainsThreeSourceIdentitiesAndEditedBindingFallback) {
  tinygltf::Model source;
  source.materials.emplace_back();
  const std::array<std::pair<const char*,const char*>,3> slots{{
      {"KHR_materials_anisotropy","anisotropyTexture"},
      {"KHR_materials_diffuse_transmission","diffuseTransmissionTexture"},
      {"KHR_materials_diffuse_transmission","diffuseTransmissionColorTexture"}}};
  for (size_t i = 0; i < slots.size(); ++i) {
    const Value metadata(Object{{"source",Value(int(i))}});
    tinygltf::Image image; image.width = image.height = 1; image.bits = 8; image.component = 4;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image = {128,64,240,127}; image.extras = metadata; source.images.push_back(image);
    tinygltf::Sampler sampler; sampler.extras = metadata; source.samplers.push_back(sampler);
    tinygltf::Texture texture; texture.source = texture.sampler = int(i); texture.extras = metadata;
    texture.extensions["VENDOR_texture_identity"] = metadata; source.textures.push_back(texture);
    auto& extension = source.materials[0].extensions[slots[i].first];
    if (!extension.IsObject()) extension = Value(Object{});
    extension.Get<Object>()[slots[i].second] = Value(Object{{"index",Value(int(i))},{"extras",metadata},
        {"extensions",Value(Object{{"KHR_texture_transform",Value(Object{{"rotation",Value(.5)},{"extras",metadata}})}})}});
  }
  tinygltf::Model combined; combined.images.emplace_back(); combined.samplers.emplace_back(); combined.textures.emplace_back();
  const auto imported = appendExportResources(combined,source,{});
  Material runtime;
  runtime.anisotropyTextureIndex = runtime.diffuseTransmissionTextureIndex = runtime.diffuseTransmissionColorTextureIndex = 7;
  runtime.anisotropyTextureTransform.rotation = .25f;
  const auto provenance = snapshotMaterialTextureBindings(runtime);
  EXPECT_EQ(provenance.runtimeIndices.size(),23u);
  std::vector<int32_t> mapping(9,-1); mapping[7] = 3; mapping[8] = 0;
  for (bool changed : {false,true}) {
    runtime.anisotropyTextureIndex = changed ? 8 : 7;
    const auto result = exportMaterial(runtime,mapping,&imported[0],&provenance);
    for (size_t i = 0; i < slots.size(); ++i) {
      const auto& info = result.extensions.at(slots[i].first).Get(slots[i].second);
      EXPECT_EQ(info.Get("index").Get<int>(),changed && i == 0 ? 0 : int(i+1));
      EXPECT_EQ(info.Get("extras"),source.textures[i].extras);
      EXPECT_EQ(info.Get("extensions").Get("KHR_texture_transform").Get("extras"),source.textures[i].extras);
      const auto& retainedTexture = combined.textures[i+1];
      EXPECT_EQ(retainedTexture.extras,source.textures[i].extras);
      EXPECT_EQ(retainedTexture.extensions,source.textures[i].extensions);
      EXPECT_EQ(combined.images[retainedTexture.source].image,source.images[i].image);
      EXPECT_EQ(combined.images[retainedTexture.source].extras,source.images[i].extras);
      EXPECT_EQ(combined.samplers[retainedTexture.sampler].extras,source.samplers[i].extras);
    }
  }
  EXPECT_EQ(source.materials[0].extensions.at(slots[0].first).Get(slots[0].second).Get("index").Get<int>(),0);
}

TEST(GltfMaterialExport, RepackedScalarKeepsOriginalImageAndSampler) {
  tinygltf::Model model;
  tinygltf::Image image;
  image.width = 2;
  image.height = 1;
  image.component = 4;
  image.bits = 8;
  image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.image = {1, 2, 17, 4, 5, 6, 231, 8};
  model.images.push_back(image);
  model.samplers.emplace_back();
  tinygltf::Texture texture;
  texture.source = 0;
  texture.sampler = 0;
  model.textures.push_back(texture);
  uint64_t budget = 7;
  using container::geometry::gltf::appendRepackedScalarTexture;
  EXPECT_THROW(appendRepackedScalarTexture(model, 0, 2, 0, budget), std::exception);
  ASSERT_EQ(model.images.size(), 1u);
  ASSERT_EQ(model.textures.size(), 1u);
  budget = 8;
  EXPECT_EQ(appendRepackedScalarTexture(model, 0, 2, 0, budget), 1);
  ASSERT_EQ(model.images.size(), 2u);
  ASSERT_EQ(model.textures.size(), 2u);
  EXPECT_EQ(model.images[0].image, image.image);
  EXPECT_EQ(model.images[1].image, (std::vector<unsigned char>{17, 2, 17, 4, 231, 6, 231, 8}));
  EXPECT_EQ(model.textures[1].source, 1);
  EXPECT_EQ(model.textures[1].sampler, 0);
  EXPECT_EQ(budget, 0u);
}
