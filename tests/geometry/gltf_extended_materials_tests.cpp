#include <Container/geometry/GltfExtendedMaterials.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <climits>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using Json = nlohmann::json;
constexpr const char* kAnisotropy = "KHR_materials_anisotropy";
constexpr const char* kDiffuse = "KHR_materials_diffuse_transmission";
constexpr const char* kTransform = "KHR_texture_transform";

Json fixture() {
  Json source;
  source["extensionsUsed"] = {kAnisotropy, kDiffuse, kTransform};
  source["textures"] = std::vector<Json>(3, Json::object());
  source["materials"] = std::vector<Json>(3, Json::object());
  for (const auto index : {1, 2})
    source["materials"][index]["extensions"] = {{kAnisotropy, Json::object()}, {kDiffuse, Json::object()}};
  auto& extensions = source["materials"][2]["extensions"];
  extensions[kAnisotropy] = {{"anisotropyStrength", .625}, {"anisotropyRotation", -.35}};
  auto& direction = extensions[kAnisotropy]["anisotropyTexture"];
  direction = {{"index", 0}, {"texCoord", 1}};
  direction["extensions"][kTransform] = {{"offset", {.25, -.5}}, {"scale", {-2, .75}},
                                        {"rotation", .125}, {"texCoord", 0}};
  extensions[kDiffuse] = {{"diffuseTransmissionFactor", .375},
      {"diffuseTransmissionColorFactor", {.2, .4, .8}},
      {"diffuseTransmissionTexture", {{"index", 1}, {"texCoord", 1}}},
      {"diffuseTransmissionColorTexture", {{"index", 2}}}};
  extensions[kDiffuse]["diffuseTransmissionColorTexture"]["extensions"][kTransform] = {{"texCoord", 1}};
  return source;
}

tinygltf::Value tinyValue(const Json& source) {
  if (source.is_null()) return {};
  if (source.is_boolean()) return tinygltf::Value(source.get<bool>());
  if (source.is_number_integer() && source.get<double>() >= INT_MIN && source.get<double>() <= INT_MAX)
    return tinygltf::Value(source.get<int>());
  if (source.is_number()) return tinygltf::Value(source.get<double>());
  if (source.is_string()) return tinygltf::Value(source.get<std::string>());
  if (source.is_array()) {
    tinygltf::Value::Array result;
    for (const auto& item : source) result.push_back(tinyValue(item));
    return tinygltf::Value(std::move(result));
  }
  tinygltf::Value::Object result;
  for (const auto& [name, item] : source.items()) result.emplace(name, tinyValue(item));
  return tinygltf::Value(std::move(result));
}

tinygltf::Model model(const Json& source) {
  tinygltf::Model result;
  if (source.contains("extensionsUsed")) result.extensionsUsed = source["extensionsUsed"].get<std::vector<std::string>>();
  result.textures.resize(source.value("textures", Json::array()).size());
  for (const auto& entry : source.value("materials", Json::array())) {
    tinygltf::Material material;
    if (entry.contains("extensions"))
      for (const auto& [name, extension] : entry["extensions"].items()) material.extensions[name] = tinyValue(extension);
    result.materials.push_back(std::move(material));
  }
  return result;
}

template <typename Operation>
void expectError(Operation&& operation, std::string_view expected) {
  try {
    operation();
    FAIL() << "Expected extended material metadata rejection";
  } catch (const std::runtime_error& error) {
    const std::string message(error.what());
    EXPECT_NE(message.find("glTF extended materials"), std::string::npos) << message;
    EXPECT_NE(message.find(expected), std::string::npos) << message;
  }
}

void rejectBoth(const Json& source, std::string_view expected) {
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source); }, expected);
  expectError([&] { (void)ParseGltfExtendedMaterials(model(source)); }, expected);
}

TEST(GltfExtendedMaterials, DefaultsAndAuthoredEmptyExtensionsRemainDistinct) {
  const auto source = fixture();
  const auto result = ParseGltfExtendedMaterials(model(source));
  ASSERT_EQ(result.materials.size(), 3u);
  for (const auto index : {0u, 1u}) {
    const auto& material = result.materials[index];
    EXPECT_FLOAT_EQ(material.anisotropyStrength, 0);
    EXPECT_FLOAT_EQ(material.anisotropyRotation, 0);
    EXPECT_FLOAT_EQ(material.diffuseTransmissionFactor, 0);
    EXPECT_EQ(material.diffuseTransmissionColorFactor, glm::vec3(1));
    EXPECT_EQ(material.anisotropyTexture.sourceTexture, -1);
    EXPECT_EQ(material.diffuseTransmissionTexture.sourceTexture, -1);
    EXPECT_EQ(material.diffuseTransmissionColorTexture.sourceTexture, -1);
    EXPECT_EQ(material.diffuseTransmissionTexture.transform.channel, 3u);
  }
  EXPECT_EQ(result.materials[0].authoredExtensions, 0u);
  EXPECT_TRUE(result.materials[1].hasExtension(GltfExtendedMaterialExtension::Anisotropy));
  EXPECT_TRUE(result.materials[1].hasExtension(GltfExtendedMaterialExtension::DiffuseTransmission));
  EXPECT_EQ(result.summary.materials, 3u);
  EXPECT_EQ(result.summary.anisotropyMaterials, 2u);
  EXPECT_EQ(result.summary.diffuseTransmissionMaterials, 2u);
  EXPECT_EQ(result.summary.textureInfos, 3u);
  EXPECT_EQ(result.summary.retainedBytes, 3 * sizeof(GltfExtendedMaterial));
  EXPECT_EQ(ValidateGltfExtendedMaterialsJson(source), result.summary);
  EXPECT_TRUE(ParseGltfExtendedMaterials(tinygltf::Model{}).materials.empty());
  EXPECT_EQ(ValidateGltfExtendedMaterialsJson(Json::object()).materials, 0u);
}

TEST(GltfExtendedMaterials, ThreeSlotsRetainIndependentSourceIdentityUvAndTransforms) {
  const auto result = ParseGltfExtendedMaterials(model(fixture()));
  const auto& material = result.materials[2];
  EXPECT_FLOAT_EQ(material.anisotropyStrength, .625f);
  EXPECT_FLOAT_EQ(material.anisotropyRotation, -.35f);
  EXPECT_FLOAT_EQ(material.diffuseTransmissionFactor, .375f);
  EXPECT_EQ(material.diffuseTransmissionColorFactor, glm::vec3(.2f, .4f, .8f));
  const auto& direction = material.anisotropyTexture;
  EXPECT_EQ(direction.sourceTexture, 0);
  EXPECT_EQ(direction.sourceTexCoord, 1u);
  EXPECT_TRUE(direction.hasTransform);
  EXPECT_EQ(direction.transform.texCoord, 0u);
  EXPECT_EQ(direction.transform.offset, glm::vec2(.25f, -.5f));
  EXPECT_EQ(direction.transform.scale, glm::vec2(-2, .75f));
  EXPECT_FLOAT_EQ(direction.transform.rotation, .125f);
  EXPECT_EQ(direction.transform.channel, 4u);
  const auto& strength = material.diffuseTransmissionTexture;
  EXPECT_EQ(strength.sourceTexture, 1);
  EXPECT_EQ(strength.sourceTexCoord, 1u);
  EXPECT_FALSE(strength.hasTransform);
  EXPECT_EQ(strength.transform.texCoord, 1u);
  EXPECT_EQ(strength.transform.channel, 3u);
  const auto& color = material.diffuseTransmissionColorTexture;
  EXPECT_EQ(color.sourceTexture, 2);
  EXPECT_EQ(color.sourceTexCoord, 0u);
  EXPECT_EQ(color.transform.texCoord, 1u);
  EXPECT_EQ(color.transform.channel, 4u);
  EXPECT_EQ(color.transform.offset, glm::vec2(0));
  EXPECT_EQ(color.transform.scale, glm::vec2(1));
  EXPECT_FLOAT_EQ(color.transform.rotation, 0);
}

TEST(GltfExtendedMaterials, TexturesNeverPromoteZeroStrengthsOrBlackColor) {
  auto source = fixture();
  auto& extensions = source["materials"][2]["extensions"];
  extensions[kAnisotropy].erase("anisotropyStrength");
  extensions[kDiffuse]["diffuseTransmissionFactor"] = 0;
  extensions[kDiffuse]["diffuseTransmissionColorFactor"] = {0, 0, 0};
  const auto result = ParseGltfExtendedMaterials(model(source));
  const auto& material = result.materials[2];
  EXPECT_FLOAT_EQ(material.anisotropyStrength, 0);
  EXPECT_FLOAT_EQ(material.diffuseTransmissionFactor, 0);
  EXPECT_EQ(material.diffuseTransmissionColorFactor, glm::vec3(0));
  EXPECT_EQ(result.summary.textureInfos, 3u);
  EXPECT_EQ(ValidateGltfExtendedMaterialsJson(source), result.summary);
}

TEST(GltfExtendedMaterials, ScalarTypesRangesAndBinary32LimitsRejectInBothRepresentations) {
  for (const auto& [extension, property] : std::array{
           std::pair{kAnisotropy, "anisotropyStrength"}, std::pair{kDiffuse, "diffuseTransmissionFactor"}}) {
    for (const auto& bad : std::vector<Json>{nullptr, true, "0.5", Json::object(), Json::array(), -.001, 1.001,
                                          std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
      auto source = fixture();
      source["materials"][2]["extensions"][extension][property] = bad;
      rejectBoth(source, property);
    }
  }
  for (const auto& bad : std::vector<Json>{nullptr, false, "1", Json::array(), 1e100,
                                         std::numeric_limits<double>::infinity()}) {
    auto source = fixture();
    source["materials"][2]["extensions"][kAnisotropy]["anisotropyRotation"] = bad;
    rejectBoth(source, "anisotropyRotation");
  }
}

TEST(GltfExtendedMaterials, TransmissionColorRequiresExactlyThreeUnitNumbers) {
  for (const auto& bad : std::vector<Json>{nullptr, 1, "red", Json::object(), Json::array(),
          {0, 1}, {0, 1, 0, 1}, {0, false, 1}, {0, "1", 1}, {-.1, 0, 0}, {0, 1.1, 0},
          {0, std::numeric_limits<double>::infinity(), 0}}) {
    auto source = fixture();
    source["materials"][2]["extensions"][kDiffuse]["diffuseTransmissionColorFactor"] = bad;
    rejectBoth(source, "diffuseTransmissionColorFactor");
  }
}

TEST(GltfExtendedMaterials, EveryTextureSlotRejectsMissingNarrowedOrNonIntegerIndices) {
  for (const auto& [extension, slot] : std::array{
           std::pair{kAnisotropy, "anisotropyTexture"}, std::pair{kDiffuse, "diffuseTransmissionTexture"},
           std::pair{kDiffuse, "diffuseTransmissionColorTexture"}}) {
    for (const auto& bad : std::vector<Json>{-1, 3, 4294967296ull, 4294967297ull, UINT64_MAX,
            0.0, .5, true, "0", nullptr, Json::array(), Json::object()}) {
      auto source = fixture();
      source["materials"][2]["extensions"][extension][slot] = {{"index", bad}};
      rejectBoth(source, std::string(slot) + ".index");
    }
    auto source = fixture();
    source["materials"][2]["extensions"][extension][slot] = Json::object();
    rejectBoth(source, "requires an index");
  }
}

TEST(GltfExtendedMaterials, EffectiveUvSelectionHonorsOverridesAndDiagnosesConsumerLimits) {
  auto source = fixture();
  auto& info = source["materials"][2]["extensions"][kAnisotropy]["anisotropyTexture"];
  info["texCoord"] = 2;
  info["extensions"][kTransform]["texCoord"] = 1;
  const auto parsed = ParseGltfExtendedMaterials(model(source));
  EXPECT_EQ(parsed.materials[2].anisotropyTexture.sourceTexCoord, 2u);
  EXPECT_EQ(parsed.materials[2].anisotropyTexture.transform.texCoord, 1u);
  EXPECT_EQ(ValidateGltfExtendedMaterialsJson(source), parsed.summary);
  info["extensions"][kTransform]["texCoord"] = 2;
  rejectBoth(source, "effective UV sets above 1 are unsupported");
  for (const auto& bad : std::vector<Json>{-1, 4294967296ull, 0.0, .25, true, "0", nullptr}) {
    auto broken = fixture();
    broken["materials"][2]["extensions"][kAnisotropy]["anisotropyTexture"]["texCoord"] = bad;
    rejectBoth(broken, ".texCoord");
    broken = fixture();
    broken["materials"][2]["extensions"][kAnisotropy]["anisotropyTexture"]["extensions"][kTransform]["texCoord"] = bad;
    rejectBoth(broken, ".texCoord");
  }
}

TEST(GltfExtendedMaterials, TextureTransformShapeTypesAndFiniteRangeAreStrict) {
  for (const char* field : {"offset", "scale"}) {
    for (const auto& bad : std::vector<Json>{nullptr, 1, Json::object(), {1}, {1, 2, 3},
                                           {false, 0}, {"1", 0}, {1e100, 0}}) {
      auto source = fixture();
      source["materials"][2]["extensions"][kAnisotropy]["anisotropyTexture"]["extensions"][kTransform][field] = bad;
      rejectBoth(source, std::string(kTransform) + "." + field);
    }
  }
  auto source = fixture();
  source["materials"][2]["extensions"][kAnisotropy]["anisotropyTexture"]["extensions"][kTransform]["rotation"] = 1e100;
  rejectBoth(source, std::string(kTransform) + ".rotation");
  source = fixture();
  source["materials"][2]["extensions"][kAnisotropy]["anisotropyTexture"]["extensions"] = nullptr;
  rejectBoth(source, "anisotropyTexture.extensions");
  source = fixture();
  source["materials"][2]["extensions"][kAnisotropy]["anisotropyTexture"]["extensions"][kTransform] = false;
  rejectBoth(source, kTransform);
}

TEST(GltfExtendedMaterials, ExtensionDeclarationsShapesAndExcludedWorkflowsAreChecked) {
  for (const char* extension : {kAnisotropy, kDiffuse}) {
    for (const char* excluded : {"KHR_materials_unlit", "KHR_materials_pbrSpecularGlossiness"}) {
      auto source = fixture();
      source["materials"][2]["extensions"].erase(extension == kAnisotropy ? kDiffuse : kAnisotropy);
      source["materials"][2]["extensions"][excluded] = Json::object();
      rejectBoth(source, "excludes KHR_materials_unlit and KHR_materials_pbrSpecularGlossiness");
    }
    auto source = fixture();
    source["materials"][2]["extensions"][extension] = false;
    rejectBoth(source, std::string(extension) + ": must be an object");
  }
  auto source = fixture();
  source["extensionsUsed"] = {kAnisotropy, kDiffuse};
  rejectBoth(source, "extension must be declared in extensionsUsed");
  source = fixture();
  source.erase("extensionsUsed");
  rejectBoth(source, "extensionsUsed");
  source = fixture();
  source["extensionsUsed"] = Json::object();
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source); }, "extensionsUsed");
  source = fixture();
  source["extensionsUsed"].push_back(1);
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source); }, "names must be strings");
}

TEST(GltfExtendedMaterials, DenseBudgetsApplyBeforeAnyMaterialPayloadInspection) {
  const auto source = fixture();
  const auto decoded = model(source);
  GltfExtendedMaterialLimits limits;
  limits.maxMaterials = 2;
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source, limits); }, "material count");
  expectError([&] { (void)ParseGltfExtendedMaterials(decoded, limits); }, "material count");
  limits.maxMaterials = 3;
  limits.maxRetainedBytes = 3 * sizeof(GltfExtendedMaterial) - 1;
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source, limits); }, "retained-byte budget");
  expectError([&] { (void)ParseGltfExtendedMaterials(decoded, limits); }, "retained-byte budget");
  ++limits.maxRetainedBytes;
  EXPECT_EQ(ValidateGltfExtendedMaterialsJson(source, limits).retainedBytes, limits.maxRetainedBytes);
  EXPECT_EQ(ParseGltfExtendedMaterials(decoded, limits).materials.size(), 3u);
  for (const auto invalid : std::array<GltfExtendedMaterialLimits, 4>{
           GltfExtendedMaterialLimits{0, 100}, GltfExtendedMaterialLimits{uint64_t(INT_MAX) + 1, 100},
           GltfExtendedMaterialLimits{3, 0}, GltfExtendedMaterialLimits{3, UINT64_MAX}}) {
    expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source, invalid); }, "limits");
    expectError([&] { (void)ParseGltfExtendedMaterials(decoded, invalid); }, "limits");
  }
}

TEST(GltfExtendedMaterials, InactiveMaterialAndOpaqueSourceMetadataRemainIntact) {
  auto source = fixture();
  source["scenes"] = {{{"nodes", Json::array()}}};
  source["scene"] = 0;
  source["buffers"] = {{{"uri", "must-not-be-opened.bin"}, {"byteLength", 1000000}}};
  auto& extension = source["materials"][2]["extensions"][kAnisotropy];
  extension["extras"] = {{"exactIdentity", 4294967297ull}, {"opaque", {{"deep", "kept"}}}};
  extension["futureProperty"] = {"uninterpreted", 17, true};
  extension["anisotropyTexture"]["extras"] = {{"slotIdentity", "preserved"}};
  const auto original = source;
  const auto decoded = model(source);
  const auto parsed = ParseGltfExtendedMaterials(decoded);
  EXPECT_EQ(ValidateGltfExtendedMaterialsJson(source), parsed.summary);
  EXPECT_EQ(source, original);
  EXPECT_EQ(decoded.materials[2].extensions.at(kAnisotropy).Get("extras").Get("opaque").Get("deep").Get<std::string>(), "kept");
  EXPECT_EQ(decoded.materials[2].extensions.at(kAnisotropy).Get("anisotropyTexture").Get("extras").Get("slotIdentity").Get<std::string>(), "preserved");
  extension["anisotropyStrength"] = 2;
  rejectBoth(source, "material[2].extensions.KHR_materials_anisotropy.anisotropyStrength");
}

TEST(GltfExtendedMaterials, RawMaterialAndTextureContainersRejectMalformedTypes) {
  auto source = fixture();
  source["materials"] = Json::object();
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source); }, "materials: must be an array");
  source = fixture();
  source["textures"] = nullptr;
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source); }, "textures: must be an array");
  source = fixture();
  source["materials"][2] = false;
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source); }, "material[2]: must be an object");
  source = fixture();
  source["materials"][2]["extensions"] = Json::array();
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(source); }, "material[2].extensions: must be an object");
  expectError([&] { (void)ValidateGltfExtendedMaterialsJson(Json::array()); }, "asset: source must be an object");
}

} // namespace
