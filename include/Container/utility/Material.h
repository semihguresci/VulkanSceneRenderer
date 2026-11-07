#pragma once

#include "Container/common/CommonMath.h"

#include <array>
#include <cstdint>
#include <limits>

namespace container::material {

enum class AlphaMode : uint32_t {
  Opaque = 0,
  Mask = 1,
  Blend = 2,
};

struct TextureTransform {
  glm::vec2 offset{0.0f, 0.0f};
  glm::vec2 scale{1.0f, 1.0f};
  float rotation{0.0f};
  uint32_t texCoord{0};
  // 0-3 select RGBA. Values above 3 mean the shader should use the
  // material-model channel convention for that texture slot.
  uint32_t channel{4};
};

struct ExperimentalMaterialLayer {
  float retroreflection{};
  float scatterStrength{};
  float scatterAnisotropy{};
  glm::vec3 scatterColor{1.0f};
  uint32_t retroreflectionTexture{std::numeric_limits<uint32_t>::max()};
  uint32_t scatterStrengthTexture{std::numeric_limits<uint32_t>::max()};
  uint32_t scatterColorTexture{std::numeric_limits<uint32_t>::max()};
  TextureTransform retroreflectionTransform;
  TextureTransform scatterStrengthTransform;
  TextureTransform scatterColorTransform;

  [[nodiscard]] std::array<uint32_t, 3> textureResources() const {
    return {retroreflectionTexture, scatterStrengthTexture, scatterColorTexture};
  }
};

struct Material {
  glm::vec4 baseColor{1.0f};
  glm::vec3 emissiveColor{0.0f};
  float metallicFactor{0.0f};
  float roughnessFactor{1.0f};
  float alphaCutoff{0.5f};
  float normalTextureScale{1.0f};
  float occlusionStrength{1.0f};
  float opacityFactor{1.0f};
  float specularFactor{1.0f};
  float heightScale{0.0f};
  float heightOffset{-0.5f};
  float transmissionFactor{0.0f};
  float emissiveStrength{1.0f};
  glm::vec3 specularColorFactor{1.0f};
  float ior{1.5f};
  float dispersion{0.0f};
  float clearcoatFactor{0.0f};
  float clearcoatRoughnessFactor{0.0f};
  float clearcoatNormalTextureScale{1.0f};
  float thicknessFactor{0.0f};
  glm::vec3 attenuationColor{1.0f};
  float attenuationDistance{std::numeric_limits<float>::infinity()};
  glm::vec3 sheenColorFactor{0.0f};
  float sheenRoughnessFactor{0.0f};
  float iridescenceFactor{0.0f};
  float iridescenceIor{1.3f};
  float iridescenceThicknessMinimum{100.0f};
  float iridescenceThicknessMaximum{400.0f};
  float anisotropyStrength{0.0f};
  float anisotropyRotation{0.0f};
  float diffuseTransmissionFactor{0.0f};
  glm::vec3 diffuseTransmissionColorFactor{1.0f};
  uint32_t baseColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t normalTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t occlusionTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t emissiveTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t metallicRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t roughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t metalnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t specularTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t heightTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t opacityTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t transmissionTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t specularColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t clearcoatTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t clearcoatRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t clearcoatNormalTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t thicknessTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t sheenColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t sheenRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t iridescenceTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t iridescenceThicknessTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t anisotropyTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t diffuseTransmissionTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t diffuseTransmissionColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  TextureTransform baseColorTextureTransform{};
  TextureTransform normalTextureTransform{};
  TextureTransform occlusionTextureTransform{};
  TextureTransform emissiveTextureTransform{};
  TextureTransform metallicRoughnessTextureTransform{};
  TextureTransform roughnessTextureTransform{};
  TextureTransform metalnessTextureTransform{};
  TextureTransform specularTextureTransform{};
  TextureTransform heightTextureTransform{};
  TextureTransform opacityTextureTransform{};
  TextureTransform transmissionTextureTransform{};
  TextureTransform specularColorTextureTransform{};
  TextureTransform clearcoatTextureTransform{};
  TextureTransform clearcoatRoughnessTextureTransform{};
  TextureTransform clearcoatNormalTextureTransform{};
  TextureTransform thicknessTextureTransform{};
  TextureTransform sheenColorTextureTransform{};
  TextureTransform sheenRoughnessTextureTransform{};
  TextureTransform iridescenceTextureTransform{};
  TextureTransform iridescenceThicknessTextureTransform{};
  TextureTransform anisotropyTextureTransform{};
  TextureTransform diffuseTransmissionTextureTransform = [] {
    TextureTransform transform;
    transform.channel = 3;
    return transform;
  }();
  TextureTransform diffuseTransmissionColorTextureTransform{};
  AlphaMode alphaMode{AlphaMode::Opaque};
  bool doubleSided{false};
  bool specularGlossinessWorkflow{false};
  bool unlit{false};
  ExperimentalMaterialLayer experimental;
  // Authored SG inputs stay separate from the raster MR approximation.
  // Alpha and glossiness retain their existing baseColor.a/roughnessFactor owners.
  glm::vec3 specularGlossinessDiffuse{1.0f};
  glm::vec3 specularGlossinessSpecular{1.0f};
};
}  // namespace container::material
