#include <Container/utility/GltfTextureResources.h>
#include <gtest/gtest.h>

namespace {
using namespace container::material;
tinygltf::Model model() {
  tinygltf::Model result;
  tinygltf::Image image;
  image.width = 1; image.height = 1; image.bits = 8; image.component = 4;
  image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.image = {128, 64, 240, 127};
  result.images.push_back(std::move(image));
  tinygltf::Texture texture; texture.source = 0;
  result.textures.push_back(texture);
  tinygltf::Material material; material.pbrMetallicRoughness.baseColorTexture.index = 0;
  result.materials.push_back(material);
  return result;
}
TextureResource resource(const std::string& name, const TextureImageData& image) {
  TextureResource value; value.name = name; value.format = image.format;
  value.width = image.mips[0].width; value.height = image.mips[0].height;
  value.mipLevels = static_cast<uint32_t>(image.mips.size());
  return value;
}
} // namespace

TEST(GltfTextureResources, OneTextureUsedForColorAndDataKeepsBothGpuFormats) {
  auto source = model(); source.materials[0].normalTexture.index = 0;
  const auto prepared = PrepareGltfTextures(source, {}, 8);
  ASSERT_EQ(prepared.images.size(), 2u); ASSERT_EQ(prepared.bindings.size(), 2u);
  TextureManager manager; size_t uploads{};
  const auto resources = UploadPreparedGltfTextures(prepared, manager,
      [&](const auto& name, const auto& data) { ++uploads; return resource(name, data); });
  EXPECT_EQ(uploads, 2u); EXPECT_NE(resources.color[0], resources.data[0]);
  EXPECT_EQ(manager.getTexture(resources.color[0])->format, VK_FORMAT_R8G8B8A8_SRGB);
  EXPECT_EQ(manager.getTexture(resources.data[0])->format, VK_FORMAT_R8G8B8A8_UNORM);
  EXPECT_NE(manager.getTexture(resources.color[0])->contentKey, manager.getTexture(resources.data[0])->contentKey);
}
TEST(GltfTextureResources, DifferentSamplerPoliciesShareOneDecodedAndUploadedImage) {
  auto source = model(); source.textures.push_back(source.textures[0]);
  source.materials.push_back(source.materials[0]);
  source.materials[1].pbrMetallicRoughness.baseColorTexture.index = 1;
  tinygltf::Sampler nearest; nearest.minFilter = 9728; nearest.magFilter = 9728;
  source.samplers.push_back(nearest); source.textures[1].sampler = 0;
  const auto prepared = PrepareGltfTextures(source, {}, 4);
  ASSERT_EQ(prepared.images.size(), 1u); ASSERT_EQ(prepared.bindings.size(), 2u);
  TextureManager manager; size_t uploads{};
  const auto upload = [&](const auto& name, const auto& data) { ++uploads; return resource(name, data); };
  const auto resources = UploadPreparedGltfTextures(prepared, manager, upload);
  EXPECT_EQ(uploads, 1u); EXPECT_NE(resources.color[0], resources.color[1]);
  EXPECT_NE(manager.getTexture(resources.color[0])->samplerIndex, manager.getTexture(resources.color[1])->samplerIndex);
  const auto repeated = UploadPreparedGltfTextures(prepared, manager, upload);
  EXPECT_EQ(uploads, 1u); EXPECT_EQ(resources.color, repeated.color);
}
TEST(GltfTextureResources, DeduplicatesIdenticalSourceContentAcrossImageIndices) {
  auto source = model(); source.images.push_back(source.images[0]);
  source.textures.push_back(source.textures[0]); source.textures[1].source = 1;
  source.materials.push_back(source.materials[0]);
  source.materials[1].pbrMetallicRoughness.baseColorTexture.index = 1;
  EXPECT_EQ(PrepareGltfTextures(source, {}, 4).images.size(), 1u);
  source.images[1].image[0] = 129;
  EXPECT_THROW((void)PrepareGltfTextures(source, {}, 4), std::runtime_error);
  EXPECT_EQ(PrepareGltfTextures(source, {}, 8).images.size(), 2u);
}
TEST(GltfTextureResources, IncludesUndrawnVariantMaterialsWithoutInventingUnusedTextureRoles) {
  auto source = model(); source.textures.push_back(source.textures[0]);
  source.textures.push_back(source.textures[0]);
  auto alternative = source.materials[0];
  alternative.pbrMetallicRoughness.baseColorTexture.index = -1;
  alternative.normalTexture.index = 1;
  source.materials.push_back(alternative);
  const auto prepared = PrepareGltfTextures(source, {}, 8);
  EXPECT_EQ(prepared.textureCount, 3u); EXPECT_EQ(prepared.bindings.size(), 2u);
  for (const auto& binding : prepared.bindings) EXPECT_NE(binding.textureIndex, 2u);
}
TEST(GltfTextureResources, InvalidNegativeSamplerRejectsEvenForUnusedTextures) {
  for(bool used:{false,true}) {
    auto source=model();source.textures[0].sampler=-2;
    if(!used) source.materials.clear();
    EXPECT_THROW((void)PrepareGltfTextures(source,{},8),std::runtime_error);
  }
}

TEST(GltfTextureResources, FreshDescriptorCountDeduplicatesRolesAndPoliciesExactlyLikeUpload) {
  auto source=model();source.textures.push_back(source.textures[0]);
  source.materials.push_back(source.materials[0]);source.materials[1].pbrMetallicRoughness.baseColorTexture.index=1;
  auto prepared=PrepareGltfTextures(source,{},16);
  EXPECT_EQ(CountPreparedGltfTextureResources(prepared),1u);
  source.materials[0].normalTexture.index=0;
  prepared=PrepareGltfTextures(source,{},16);EXPECT_EQ(CountPreparedGltfTextureResources(prepared),2u);
  source.samplers.resize(1);source.samplers[0].minFilter=source.samplers[0].magFilter=9728;
  source.textures[1].sampler=0;
  prepared=PrepareGltfTextures(source,{},16);EXPECT_EQ(CountPreparedGltfTextureResources(prepared),3u);
  size_t uploads{};TextureManager manager;
  const auto resources=UploadPreparedGltfTextures(prepared,manager,[&](const auto& name,const auto& image) {
    ++uploads;return resource(name,image);
  });
  EXPECT_EQ(manager.textureCount(),CountPreparedGltfTextureResources(prepared));
  EXPECT_EQ(uploads,2u);EXPECT_NE(resources.color[0],resources.color[1]);
}

TEST(GltfTextureResources, InvalidLaterBindingsRejectBeforeAnyUpload) {
  auto prepared = PrepareGltfTextures(model(), {}, 4);
  auto invalid = prepared.bindings[0]; invalid.textureIndex = 99;
  prepared.bindings.push_back(invalid);
  size_t uploads{}; TextureManager manager;
  EXPECT_THROW((void)UploadPreparedGltfTextures(prepared, manager,
      [&](const auto& name, const auto& data) { ++uploads; return resource(name, data); }), std::runtime_error);
  EXPECT_EQ(uploads, 0u); EXPECT_EQ(manager.textureCount(), 0u);
}

TEST(GltfTextureResources, CoreCacheAliasCannotBypassAlternateEncodingValidation) {
  for (const auto& [extension, mimeType] : {
      std::pair{"EXT_texture_webp", "image/webp"}, std::pair{"MSFT_texture_dds", "image/vnd-ms.dds"}}) {
  auto source = model();
  source.textures.push_back(source.textures[0]);
  source.textures[1].source = -1;
  source.textures[1].extensions.emplace(extension, tinygltf::Value(
      tinygltf::Value::Object{{"source", tinygltf::Value(0)}}));
  source.extensionsUsed = source.extensionsRequired = {extension};
  source.images[0].mimeType = mimeType;
  source.materials.push_back(source.materials[0]);
  source.materials[1].pbrMetallicRoughness.baseColorTexture.index = 1;
  // The first core texture could populate a decoded cache entry. The second
  // texture must still reject decoded pixels that are not a retained WebP.
  EXPECT_THROW((void)PrepareGltfTextures(source, {}, 8), std::runtime_error);
  std::string mime;
  ASSERT_TRUE(tinygltf::DecodeDataURI(&source.images[0].image, mime,
      "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==",
      0, false));
  source.images[0].as_is = true;
  // Retained core PNG bytes also cannot masquerade as a WebP cache alias.
  EXPECT_THROW((void)PrepareGltfTextures(source, {}, 8), std::runtime_error);
  }
}

TEST(GltfTextureResources, ExtendedZeroFactorSlotsKeepRolesAndSamplerIdentityAcrossSharedPixels) {
  auto source = model();
  source.materials[0].pbrMetallicRoughness.baseColorTexture.index = -1;
  source.extensionsUsed = {"KHR_materials_anisotropy", "KHR_materials_diffuse_transmission"};
  source.textures.resize(3, source.textures[0]);
  source.samplers.resize(2);
  source.samplers[0].minFilter = source.samplers[0].magFilter = 9728;
  source.samplers[0].wrapS = source.samplers[0].wrapT = 33071;
  source.samplers[1].minFilter = 9987;
  source.samplers[1].magFilter = 9729;
  source.samplers[1].wrapS = source.samplers[1].wrapT = 33648;
  source.textures[0].sampler = source.textures[2].sampler = 0;
  source.textures[1].sampler = 1;
  const auto info = [](int index) { return tinygltf::Value(tinygltf::Value::Object{{"index", tinygltf::Value(index)}}); };
  source.materials[0].extensions["KHR_materials_anisotropy"] = tinygltf::Value(tinygltf::Value::Object{
      {"anisotropyStrength", tinygltf::Value(0.0)}, {"anisotropyTexture", info(0)}});
  source.materials[0].extensions["KHR_materials_diffuse_transmission"] = tinygltf::Value(tinygltf::Value::Object{
      {"diffuseTransmissionFactor", tinygltf::Value(0.0)},
      {"diffuseTransmissionTexture", info(1)}, {"diffuseTransmissionColorTexture", info(2)}});
  const auto prepared = PrepareGltfTextures(source, {}, 8);
  ASSERT_EQ(prepared.images.size(), 2u);
  ASSERT_EQ(prepared.bindings.size(), 3u);
  for (const auto& image : prepared.images) {
    EXPECT_TRUE(image.image.format == VK_FORMAT_R8G8B8A8_UNORM || image.image.format == VK_FORMAT_R8G8B8A8_SRGB);
    EXPECT_EQ(image.image.mips[0].bytes, (std::vector<std::byte>{std::byte{128}, std::byte{64}, std::byte{240}, std::byte{127}}));
  }
  TextureManager manager;
  size_t uploads = 0;
  const auto uploader = [&](const auto& name, const auto& data) { ++uploads; return resource(name, data); };
  const auto resources = UploadPreparedGltfTextures(prepared, manager, uploader);
  EXPECT_EQ(uploads, 2u);
  EXPECT_NE(resources.data[0], resources.data[1]);
  EXPECT_EQ(manager.getTexture(resources.data[0])->contentKey, manager.getTexture(resources.data[1])->contentKey);
  EXPECT_NE(manager.getTexture(resources.data[0])->samplerIndex, manager.getTexture(resources.data[1])->samplerIndex);
  EXPECT_EQ(manager.getTexture(resources.data[0])->format, VK_FORMAT_R8G8B8A8_UNORM);
  EXPECT_EQ(manager.getTexture(resources.color[2])->format, VK_FORMAT_R8G8B8A8_SRGB);
  const auto repeat = UploadPreparedGltfTextures(prepared, manager, uploader);
  EXPECT_EQ(uploads, 2u);
  EXPECT_EQ(resources.data, repeat.data);
  EXPECT_EQ(resources.color, repeat.color);
}

TEST(GltfTextureResources, ExtendedVariantOnlyMaterialRolesRemainResident) {
  auto source = model();
  source.materials[0].pbrMetallicRoughness.baseColorTexture.index = -1;
  source.extensionsUsed = {"KHR_materials_anisotropy", "KHR_materials_diffuse_transmission"};
  source.textures.resize(2, source.textures[0]);
  auto inactive = source.materials[0];
  const auto info = [](int index) { return tinygltf::Value(tinygltf::Value::Object{{"index", tinygltf::Value(index)}}); };
  inactive.extensions["KHR_materials_anisotropy"] = tinygltf::Value(tinygltf::Value::Object{{"anisotropyTexture", info(0)}});
  inactive.extensions["KHR_materials_diffuse_transmission"] = tinygltf::Value(tinygltf::Value::Object{
      {"diffuseTransmissionTexture", info(0)}, {"diffuseTransmissionColorTexture", info(1)}});
  source.materials.push_back(inactive);
  const auto prepared = PrepareGltfTextures(source, {}, 8);
  EXPECT_EQ(prepared.images.size(), 2u);
  ASSERT_EQ(prepared.bindings.size(), 2u);
  EXPECT_EQ(prepared.bindings[0].textureIndex, 0u);
  EXPECT_FALSE(prepared.bindings[0].isSrgb);
  EXPECT_EQ(prepared.bindings[1].textureIndex, 1u);
  EXPECT_TRUE(prepared.bindings[1].isSrgb);
}

TEST(GltfTextureResources, InvalidExtendedMaterialRejectsBeforeDecodingImagePixels) {
  auto source = model();
  source.extensionsUsed = {"KHR_materials_diffuse_transmission"};
  source.materials[0].extensions["KHR_materials_diffuse_transmission"] = tinygltf::Value(tinygltf::Value::Object{
      {"diffuseTransmissionFactor", tinygltf::Value(2.0)}});
  source.images[0].image = {0};
  try {
    (void)PrepareGltfTextures(source, {}, 8);
    FAIL() << "Expected extended factor validation before truncated image decoding";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("diffuseTransmissionFactor"), std::string::npos);
  }
}
