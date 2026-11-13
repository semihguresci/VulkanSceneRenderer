#include "Container/utility/SceneManager.h"
#include "Container/renderer/raytracing/RaySceneExtraction.h"

#include <algorithm>
#include <cmath>
#include <print>
#include <unordered_set>
#include <utility>

namespace container::scene {
namespace {

using container::material::AlphaMode;
using container::material::Material;
using container::material::TextureManager;
using container::renderer::RayOpacityMicromapRange;
using container::geometry::gltf::GltfOpacityMicromap;

struct Coverage {
  float alpha{}, cutoff{};
  uint32_t texture{UINT32_MAX};
  glm::vec2 offset{}, scale{1.0f};
  float rotation{};
  uint32_t texCoord{}, sampler{};
  container::gpu::TextureAllocationIdentity image;

  bool operator==(const Coverage&) const = default;
};

std::optional<Coverage> coverage(const Material* material, const TextureManager& textures) {
  if (!material || material->alphaMode != AlphaMode::Mask || material->specularGlossinessWorkflow ||
      material->opacityFactor != 1.0f || material->opacityTextureIndex != UINT32_MAX ||
      material->transmissionFactor != 0.0f || !std::isfinite(material->baseColor.a) ||
      !std::isfinite(material->alphaCutoff)) return std::nullopt;
  Coverage result{material->baseColor.a, material->alphaCutoff, material->baseColorTextureIndex};
  if (result.texture == UINT32_MAX) return result;
  const auto* texture = textures.getTexture(result.texture);
  const auto& transform = material->baseColorTextureTransform;
  if (!texture || !texture->allocationIdentity || transform.texCoord > 1 ||
      (texture->format != VK_FORMAT_R8G8B8A8_UNORM && texture->format != VK_FORMAT_R8G8B8A8_SRGB))
    return std::nullopt;
  result.offset = transform.offset;
  result.scale = transform.scale;
  result.rotation = transform.rotation;
  result.texCoord = transform.texCoord;
  result.sampler = texture->samplerIndex;
  result.image = texture->allocationIdentity;
  return result;
}

bool authoredTextureMatches(const tinygltf::Model& model, int sourceTexture, const Coverage& witness,
                            const container::material::GltfTextureResources& resources,
                            const TextureManager& textures) {
  // A textureless mask has analytically constant coverage and does not need
  // authored per-triangle classification.
  if (sourceTexture < 0) return false;
  const auto index = static_cast<size_t>(sourceTexture);
  if (index >= model.textures.size() || index >= resources.color.size() ||
      witness.texture == UINT32_MAX || resources.color[index] != witness.texture) return false;
  const auto& original = model.textures[index];
  if (original.source < 0 || size_t(original.source) >= model.images.size() ||
      original.extensions.contains("KHR_texture_basisu") || original.extensions.contains("EXT_texture_webp") ||
      original.extensions.contains("MSFT_texture_dds")) return false;
  const auto& image = model.images[size_t(original.source)];
  const auto* published = textures.getTexture(witness.texture);
  if (!published || image.image.empty()) return false;
  // The ordinary scene loader retains encoded images until exact role-aware
  // upload. Their TinyGLTF width and height are intentionally unset.
  if (image.as_is) {
    const auto mime = container::geometry::gltf::EncodedTextureMimeType(image.image);
    return mime == "image/png" || mime == "image/jpeg";
  }
  return image.width > 0 && image.height > 0 && published->width == uint32_t(image.width) &&
      published->height == uint32_t(image.height);
}

auto drawKey(uint32_t firstIndex, uint32_t indexCount) {
  return std::pair{firstIndex, indexCount};
}

} // namespace

struct SceneManager::OpacityMicromapState {
  struct Candidate {
    RayOpacityMicromapRange geometry;
    uint32_t material{};
    Coverage witness;
  };
  uint64_t retainedBytes{};
  std::vector<Candidate> candidates;
};

void SceneManager::retainOpacityMicromaps(const container::geometry::gltf::GltfOpacityMicromapAssets& assets) {
  if (!config_.experimentalOpacityMicromaps) return;
  for (const auto& diagnostic : assets.diagnostics)
    std::println(stderr, "glTF opacity micromap {}: {}", diagnostic.context, diagnostic.message);
  if (assets.diagnosticsOmitted)
    std::println(stderr, "glTF opacity micromaps: {} additional diagnostics", assets.diagnosticsOmitted);
  if (assets.primitives.empty()) return;

  auto state = std::make_shared<OpacityMicromapState>();
  size_t sourcePrimitive = 0;
  for (size_t mesh = 0; mesh < gltfModel_.meshes.size(); ++mesh) {
    const auto& primitives = gltfModel_.meshes[mesh].primitives;
    for (size_t primitive = 0; primitive < primitives.size(); ++primitive, ++sourcePrimitive) {
      auto attachment = assets.find(mesh, primitive);
      if (!attachment || !attachment->map || sourcePrimitive >= sourcePrimitiveToRuntime_.size()) continue;
      const auto& authored = primitives[primitive];
      if (!authored.targets.empty() || authored.attributes.contains("COLOR_0") || authored.material < 0 ||
          size_t(authored.material) >= gltfModel_.materials.size()) continue;
      const auto& sourceMaterial = gltfModel_.materials[size_t(authored.material)];
      if (sourceMaterial.alphaMode != "MASK" || sourceMaterial.pbrMetallicRoughness.baseColorFactor.size() != 4)
        continue;
      const uint32_t runtimeMaterial = resolveLoadedMaterialIndex(authored.material);
      auto witness = coverage(materialManager_.getMaterial(runtimeMaterial), textureManager_);
      if (!witness || witness->alpha != float(sourceMaterial.pbrMetallicRoughness.baseColorFactor[3]) ||
          witness->cutoff != float(sourceMaterial.alphaCutoff) ||
          !authoredTextureMatches(gltfModel_, sourceMaterial.pbrMetallicRoughness.baseColorTexture.index,
                                 *witness, gltfTextureResources_, textureManager_)) continue;
      const auto runtime = sourcePrimitiveToRuntime_[sourcePrimitive];
      if (runtime >= primitiveRanges().size()) continue;
      const auto& range = primitiveRanges()[runtime];
      if (!range.indexCount || range.indexCount % 3 || attachment->indices.size() != range.indexCount / 3) continue;
      state->candidates.push_back({{range.firstIndex, range.indexCount, std::move(attachment)},
                                   runtimeMaterial, std::move(*witness)});
    }
  }
  if (state->candidates.empty()) return;
  std::ranges::sort(state->candidates, {}, [](const auto& candidate) {
    return drawKey(candidate.geometry.firstIndex, candidate.geometry.indexCount);
  });
  std::unordered_set<const GltfOpacityMicromap*> maps;
  uint64_t bytes = sizeof(OpacityMicromapState) + state->candidates.capacity() * sizeof(OpacityMicromapState::Candidate);
  for (const auto& candidate : state->candidates) {
    const auto& attachment = *candidate.geometry.attachment;
    bytes += sizeof(attachment) + attachment.indices.capacity() * sizeof(attachment.indices[0]) +
        attachment.usage.capacity() * sizeof(attachment.usage[0]);
    const auto& map = *attachment.map;
    if (maps.insert(&map).second)
      bytes += sizeof(map) + map.data.capacity() + map.triangles.capacity() * sizeof(map.triangles[0]) +
          map.usage.capacity() * sizeof(map.usage[0]);
  }
  state->retainedBytes = bytes;
  opacityMicromapState_ = std::move(state);
  opacityMicromapGeometryRevision_ = geometryRevision_;
}

uint64_t SceneManager::retainedOpacityMicromapBytes() const noexcept {
  return opacityMicromapState_ ? opacityMicromapState_->retainedBytes : 0;
}

std::vector<RayOpacityMicromapRange> SceneManager::rayOpacityMicromaps(
    std::span<const container::renderer::DrawCommand> draws,
    std::span<const container::gpu::ObjectData> objects) const {
  if (!opacityMicromapState_ || opacityMicromapGeometryRevision_ != geometryRevision_) return {};
  const auto& candidates = opacityMicromapState_->candidates;
  enum class Eligibility { Unused, Eligible, Rejected };
  std::vector<Eligibility> eligibility(candidates.size(), Eligibility::Unused);
  for (const auto& draw : draws) {
    if (draw.instanceCount == 0) continue;
    const auto key = drawKey(draw.firstIndex, draw.indexCount);
    const auto found = std::ranges::lower_bound(candidates, key, {}, [](const auto& candidate) {
      return drawKey(candidate.geometry.firstIndex, candidate.geometry.indexCount);
    });
    if (found == candidates.end() || drawKey(found->geometry.firstIndex, found->geometry.indexCount) != key) continue;
    auto& state = eligibility[size_t(found - candidates.begin())];
    if (state == Eligibility::Rejected) continue;
    if (draw.objectIndex > objects.size() || draw.instanceCount > objects.size() - draw.objectIndex ||
        (state == Eligibility::Unused && coverage(materialManager_.getMaterial(found->material), textureManager_) != found->witness)) {
      state = Eligibility::Rejected;
      continue;
    }
    // One BLAS is shared by all instances of the range. A single altered
    // material or facing/deformation route must disable it for the whole range.
    const auto instances = objects.subspan(draw.objectIndex, draw.instanceCount);
    state = std::ranges::all_of(instances, [&](const auto& object) {
      return object.objectInfo.x == found->material &&
          (object.objectInfo.y & (container::gpu::kObjectFlagAlphaBlend |
                                  container::gpu::kObjectFlagDeformationWindingFlipped)) == 0u;
    }) ? Eligibility::Eligible : Eligibility::Rejected;
  }
  std::vector<RayOpacityMicromapRange> result;
  for (size_t i = 0; i < candidates.size(); ++i)
    if (eligibility[i] == Eligibility::Eligible) result.push_back(candidates[i].geometry);
  return result;
}

} // namespace container::scene
