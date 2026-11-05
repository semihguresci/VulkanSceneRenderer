#pragma once

#include <Container/utility/Material.h>

#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <cstdint>
#include <vector>

namespace container::geometry::gltf {

struct GltfExtendedTextureInfo {
  int sourceTexture{-1};
  uint32_t sourceTexCoord{};
  material::TextureTransform transform;
  bool hasTransform{};
};

enum class GltfExtendedMaterialExtension : uint8_t {
  Anisotropy = 1,
  DiffuseTransmission = 2,
};

struct GltfExtendedMaterial {
  float anisotropyStrength{};
  float anisotropyRotation{};
  float diffuseTransmissionFactor{};
  glm::vec3 diffuseTransmissionColorFactor{1.0f};
  // Linear RGB: signed tangent-space direction in RG, strength in B.
  GltfExtendedTextureInfo anisotropyTexture;
  // Linear alpha multiplies diffuseTransmissionFactor, including zero.
  GltfExtendedTextureInfo diffuseTransmissionTexture = [] {
    GltfExtendedTextureInfo info;
    info.transform.channel = 3;
    return info;
  }();
  // sRGB RGB multiplies the linear color factor, including black.
  GltfExtendedTextureInfo diffuseTransmissionColorTexture;
  uint8_t authoredExtensions{};

  [[nodiscard]] bool hasExtension(GltfExtendedMaterialExtension extension) const noexcept {
    return (authoredExtensions & static_cast<uint8_t>(extension)) != 0;
  }
};

struct GltfExtendedMaterialLimits {
  uint64_t maxMaterials{1'000'000};
  uint64_t maxRetainedBytes{64ull * 1024 * 1024};
};
struct GltfExtendedMaterialSummary {
  uint64_t materials{}, anisotropyMaterials{}, diffuseTransmissionMaterials{}, textureInfos{}, retainedBytes{};
  friend bool operator==(const GltfExtendedMaterialSummary&, const GltfExtendedMaterialSummary&) = default;
};
struct GltfExtendedMaterialData {
  // Dense source material order, including materials unused by the active scene.
  std::vector<GltfExtendedMaterial> materials;
  GltfExtendedMaterialSummary summary;
};

// Read-only metadata gate before resource reads and TinyGLTF int narrowing.
// Both extensions exclude unlit and specular-glossiness. Texture indices and
// effective UV0/UV1 selections are checked, with KHR_texture_transform defaults
// and overrides retained. Higher effective UV sets receive an explicit consumer
// limitation diagnostic. Opaque extras/unknown properties remain untouched.
[[nodiscard]] GltfExtendedMaterialSummary ValidateGltfExtendedMaterialsJson(
    const nlohmann::json& source, const GltfExtendedMaterialLimits& limits = {});

// Detached local factors and source texture identities only. This does not
// decode texture pixels, choose samplers, evaluate shading or register support.
// Zero strengths and black colors are never promoted when a texture is present.
[[nodiscard]] GltfExtendedMaterialData ParseGltfExtendedMaterials(
    const tinygltf::Model& source, const GltfExtendedMaterialLimits& limits = {});

} // namespace container::geometry::gltf
