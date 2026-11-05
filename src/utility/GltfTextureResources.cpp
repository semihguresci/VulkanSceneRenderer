#include <Container/utility/GltfTextureResources.h>
#include <Container/geometry/GltfExtendedMaterials.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <set>
#include <stdexcept>

namespace container::material {
namespace {
constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();
constexpr unsigned kColor = 1, kData = 2;
int extensionTexture(const tinygltf::Material& material, const char* extension, const char* field) {
  const auto found = material.extensions.find(extension);
  if (found == material.extensions.end() || !found->second.IsObject() || !found->second.Has(field)) return -1;
  const auto& info = found->second.Get(field);
  if (!info.IsObject() || !info.Has("index")) return -1;
  const auto& index = info.Get("index");
  if (!index.IsInt()) throw std::runtime_error(std::string("invalid texture index in ") + extension + "." + field);
  return index.Get<int>();
}
std::vector<unsigned> roles(const tinygltf::Model& model) {
  const auto extended = geometry::gltf::ParseGltfExtendedMaterials(model);
  std::vector<unsigned> result(model.textures.size());
  const auto mark = [&](int index, unsigned role) {
    if (index == -1) return;
    if (index < 0 || size_t(index) >= result.size()) throw std::runtime_error("material texture index is out of range");
    result[size_t(index)] |= role;
  };
  for (size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex) {
    const auto& material = model.materials[materialIndex];
    const auto& extensions = extended.materials[materialIndex];
    mark(material.pbrMetallicRoughness.baseColorTexture.index, kColor);
    mark(material.emissiveTexture.index, kColor);
    mark(material.normalTexture.index, kData);
    mark(material.occlusionTexture.index, kData);
    mark(material.pbrMetallicRoughness.metallicRoughnessTexture.index, kData);
    mark(extensions.anisotropyTexture.sourceTexture, kData);
    mark(extensions.diffuseTransmissionTexture.sourceTexture, kData);
    mark(extensions.diffuseTransmissionColorTexture.sourceTexture, kColor);
    for (const auto& slot : std::array<std::array<const char*, 2>, 4>{{
        {"KHR_materials_pbrSpecularGlossiness", "diffuseTexture"},
        {"KHR_materials_pbrSpecularGlossiness", "specularGlossinessTexture"},
        {"KHR_materials_specular", "specularColorTexture"},
        {"KHR_materials_sheen", "sheenColorTexture"}}})
      mark(extensionTexture(material, slot[0], slot[1]), kColor);
    for (const auto& slot : std::array<std::array<const char*, 2>, 9>{{
        {"KHR_materials_specular", "specularTexture"},
        {"KHR_materials_transmission", "transmissionTexture"},
        {"KHR_materials_volume", "thicknessTexture"},
        {"KHR_materials_clearcoat", "clearcoatTexture"},
        {"KHR_materials_clearcoat", "clearcoatRoughnessTexture"},
        {"KHR_materials_clearcoat", "clearcoatNormalTexture"},
        {"KHR_materials_sheen", "sheenRoughnessTexture"},
        {"KHR_materials_iridescence", "iridescenceTexture"},
        {"KHR_materials_iridescence", "iridescenceThicknessTexture"}}})
      mark(extensionTexture(material, slot[0], slot[1]), kData);
    for (const auto* values : {&material.values, &material.additionalValues})
      for (const auto* field : {"roughnessTexture", "metalnessTexture", "specularTexture",
                                "heightTexture", "displacementTexture", "opacityTexture",
                                "refractionTexture", "transmissionTexture"}) {
        const auto found = values->find(field);
        if (found != values->end()) mark(found->second.TextureIndex(), kData);
      }
  }
  // Unused textures retain their encoded source resources. Their color-space
  // role is not invented: a later authoring assignment chooses it explicitly.
  return result;
}
std::string contentKey(const TextureImageData& image, bool color) {
  std::array<uint64_t, 2> hash{14695981039346656037ull, 7809847782465536322ull};
  const auto byte = [&](unsigned char value) {
    hash[0] = (hash[0] ^ value) * 1099511628211ull;
    hash[1] = (hash[1] ^ value) * 14029467366897019727ull;
  };
  const auto word = [&](uint64_t value) { for (unsigned i = 0; i < 8; ++i) byte(static_cast<unsigned char>(value >> (i * 8))); };
  word(image.format); word(color); word(image.authoredMipChain);
  word(image.componentMapping.r); word(image.componentMapping.g);
  word(image.componentMapping.b); word(image.componentMapping.a); word(image.mips.size());
  for (const auto& mip : image.mips) {
    word(mip.width); word(mip.height); word(mip.bytes.size());
    for (const auto value : mip.bytes) byte(std::to_integer<unsigned char>(value));
  }
  std::string result = "gltf-image:";
  for (const auto value : hash) {
    std::array<char, 16> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
    result.append(16 - static_cast<size_t>(converted.ptr - digits.data()), '0');
    result.append(digits.data(), converted.ptr);
  }
  return result;
}
bool sameImage(const TextureImageData& first, const TextureImageData& second) {
  if (first.format != second.format || first.authoredMipChain != second.authoredMipChain ||
      first.componentMapping.r != second.componentMapping.r || first.componentMapping.g != second.componentMapping.g ||
      first.componentMapping.b != second.componentMapping.b || first.componentMapping.a != second.componentMapping.a ||
      first.mips.size() != second.mips.size()) return false;
  for (size_t mip = 0; mip < first.mips.size(); ++mip)
    if (first.mips[mip].width != second.mips[mip].width || first.mips[mip].height != second.mips[mip].height ||
        first.mips[mip].bytes != second.mips[mip].bytes) return false;
  return true;
}
} // namespace

PreparedGltfTextures PrepareGltfTextures(const tinygltf::Model& model,
    const TextureDecodeOptions& deviceProfile, uint64_t maxPreparedBytes) {
  if (!maxPreparedBytes) throw std::invalid_argument("glTF texture preparation budget must be positive");
  const auto plan = container::geometry::gltf::SelectTextureImageSources(model);
  const auto requiredRoles = roles(model);
  PreparedGltfTextures prepared;
  prepared.textureCount = model.textures.size();
  struct CachedSource { int source{}; bool color{}, strict{}; size_t decoded{}; };
  std::vector<CachedSource> sourceCache;
  std::vector<bool> strictImages(model.images.size());
  for (const auto& selected : plan.textures)
    if (selected.strictKhrBasisu()) strictImages[size_t(selected.imageIndex)] = true;
  uint64_t remaining = maxPreparedBytes;
  for (size_t texture = 0; texture < model.textures.size(); ++texture) {
    MaterialSamplerKey sampler;
    const auto sourceSampler = model.textures[texture].sampler;
    if (sourceSampler < -1) throw std::runtime_error("glTF texture sampler index is out of range");
    if (sourceSampler >= 0) {
      if (size_t(sourceSampler) >= model.samplers.size()) throw std::runtime_error("glTF texture sampler index is out of range");
      const auto& value = model.samplers[size_t(sourceSampler)];
      sampler = NormalizeMaterialSamplerKey(value.minFilter, value.magFilter, value.wrapS, value.wrapT);
    }
    for (const unsigned role : {kColor, kData}) {
      if (!(requiredRoles[texture] & role)) continue;
      auto selection = plan.textures[texture];
      const bool strict = strictImages[size_t(selection.imageIndex)];
      const auto& source = model.images[size_t(selection.imageIndex)];
      // Sharing pixels cannot bypass the validation required by this binding's
      // source extension. Do this before looking up a decoded core alias.
      if (selection.kind != container::geometry::gltf::TextureImageSourceKind::Core && !source.as_is)
        throw std::runtime_error("texture[" + std::to_string(texture) + "] alternate source must retain its encoded image");
      if (selection.kind == container::geometry::gltf::TextureImageSourceKind::Webp &&
          container::geometry::gltf::EncodedTextureMimeType(source.image) != "image/webp")
        throw std::runtime_error("texture[" + std::to_string(texture) + "] EXT_texture_webp requires a WebP image");
      if (selection.kind == container::geometry::gltf::TextureImageSourceKind::Dds &&
          container::geometry::gltf::EncodedTextureMimeType(source.image) != "image/vnd-ms.dds")
        throw std::runtime_error("texture[" + std::to_string(texture) + "] MSFT_texture_dds requires a DDS image");
      const auto cached = std::ranges::find_if(sourceCache, [&](const auto& entry) {
        const auto& prior = model.images[size_t(entry.source)];
        return entry.color == (role == kColor) && entry.strict == strict &&
            source.as_is == prior.as_is && source.bits == prior.bits && source.pixel_type == prior.pixel_type &&
            source.component == prior.component && source.width == prior.width && source.height == prior.height &&
            source.image == prior.image;
      });
      if (cached != sourceCache.end()) {
        prepared.bindings.push_back({texture, cached->decoded, role == kColor, sampler});
        continue;
      }
      auto options = deviceProfile;
      options.strictKhrBasisu = strict;
      options.maxDecodedBytes = std::min(options.maxDecodedBytes, remaining);
      auto decoded = container::geometry::gltf::DecodeSelectedTextureImage(model, selection,
          role == kColor ? container::geometry::gltf::TextureImageRole::Color : container::geometry::gltf::TextureImageRole::Data,
          options);
      auto key = contentKey(decoded, role == kColor);
      auto existing = prepared.images.end();
      for (auto candidate = prepared.images.begin(); candidate != prepared.images.end(); ++candidate)
        if (candidate->contentKey == key && sameImage(candidate->image, decoded)) { existing = candidate; break; }
      size_t imageIndex{};
      if (existing != prepared.images.end()) imageIndex = static_cast<size_t>(existing - prepared.images.begin());
      else {
        uint64_t bytes{};
        for (const auto& mip : decoded.mips) {
          if (mip.bytes.size() > remaining - bytes) throw std::runtime_error("glTF textures exceed the aggregate decoded budget");
          bytes += mip.bytes.size();
        }
        remaining -= bytes;
        // Keep distinct keys even if an input intentionally collides with the
        // compact digest. Pixel/format equality decides within this scene.
        if (std::ranges::any_of(prepared.images, [&](const auto& image) { return image.contentKey == key; }))
          key += ":collision:" + std::to_string(prepared.images.size());
        imageIndex = prepared.images.size();
        prepared.images.push_back({std::move(decoded), std::move(key)});
      }
      sourceCache.push_back({selection.imageIndex, role == kColor, strict, imageIndex});
      prepared.bindings.push_back({texture, imageIndex, role == kColor, sampler});
    }
  }
  return prepared;
}

size_t CountPreparedGltfTextureResources(const PreparedGltfTextures& prepared) {
  if(prepared.textureCount>1'000'000 || prepared.bindings.size()>2'000'000)
    throw std::length_error("Prepared glTF texture table exceeds its finite capacity");
  std::map<std::pair<size_t, bool>, size_t> assigned;
  std::set<std::pair<std::string_view,uint32_t>> slots;
  for (const auto& binding : prepared.bindings) {
    if (binding.imageIndex >= prepared.images.size() || binding.textureIndex >= prepared.textureCount)
      throw std::runtime_error("invalid prepared glTF texture binding");
    (void)DecodeMaterialSamplerKey(binding.sampler);
    if (!assigned.emplace(std::pair{binding.textureIndex, binding.isSrgb}, binding.imageIndex).second)
      throw std::runtime_error("duplicate prepared glTF texture role binding");
    const auto& key=prepared.images[binding.imageIndex].contentKey;
    if(key.empty()) throw std::runtime_error("prepared glTF texture content identity is empty");
    slots.emplace(key,binding.sampler.value);
  }
  return slots.size();
}

GltfTextureResources UploadPreparedGltfTextures(const PreparedGltfTextures& prepared,
    TextureManager& manager,
    const std::function<TextureResource(const std::string&, const TextureImageData&)>& uploader) {
  (void)CountPreparedGltfTextureResources(prepared);
  GltfTextureResources resources;
  resources.color.assign(prepared.textureCount, kInvalid);
  resources.data.assign(prepared.textureCount, kInvalid);
  for (const auto& binding : prepared.bindings) {
    const auto& image = prepared.images[binding.imageIndex];
    const auto key = image.contentKey + "|sampler=" + std::to_string(binding.sampler.value);
    uint32_t resourceIndex{};
    if (const auto cached = manager.findTextureIndex(key)) resourceIndex = *cached;
    else {
      TextureResource resource;
      if (const auto cachedImage = manager.findTextureImage(image.contentKey)) resource = *cachedImage;
      else resource = uploader(image.contentKey, image.image);
      resource.contentKey = image.contentKey; resource.name = key;
      resource.samplerIndex = binding.sampler.value;
      resourceIndex = manager.registerTexture(resource);
    }
    (binding.isSrgb ? resources.color : resources.data)[binding.textureIndex] = resourceIndex;
  }
  return resources;
}
} // namespace container::material
