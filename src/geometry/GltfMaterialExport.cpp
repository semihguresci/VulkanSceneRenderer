#include <Container/geometry/GltfMaterialExport.h>
#include <Container/geometry/GltfImageSources.h>
#include <Container/geometry/GltfMaterialTextureAuthoring.h>
#include <Container/geometry/GltfLegacyMaterialSerialization.h>

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace container::geometry::gltf {
namespace {
using Value = tinygltf::Value;
using Object = Value::Object;
constexpr auto kInvalidTexture = std::numeric_limits<uint32_t>::max();
using TextureMember = uint32_t material::Material::*;
constexpr std::array<TextureMember, 23> kTextureMembers{
    &material::Material::baseColorTextureIndex, &material::Material::normalTextureIndex,
    &material::Material::occlusionTextureIndex, &material::Material::emissiveTextureIndex,
    &material::Material::metallicRoughnessTextureIndex, &material::Material::roughnessTextureIndex,
    &material::Material::metalnessTextureIndex, &material::Material::specularTextureIndex,
    &material::Material::heightTextureIndex, &material::Material::opacityTextureIndex,
    &material::Material::transmissionTextureIndex, &material::Material::specularColorTextureIndex,
    &material::Material::clearcoatTextureIndex, &material::Material::clearcoatRoughnessTextureIndex,
    &material::Material::clearcoatNormalTextureIndex, &material::Material::thicknessTextureIndex,
    &material::Material::sheenColorTextureIndex, &material::Material::sheenRoughnessTextureIndex,
    &material::Material::iridescenceTextureIndex, &material::Material::iridescenceThicknessTextureIndex,
    &material::Material::anisotropyTextureIndex, &material::Material::diffuseTransmissionTextureIndex,
    &material::Material::diffuseTransmissionColorTextureIndex};

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error("Scene material export: " + std::string(message));
}

void finite(double value, std::string_view name) {
  if (!std::isfinite(value)) fail(std::string(name) + " must be finite");
}

void range(double value, double minimum, double maximum, std::string_view name) {
  finite(value, name);
  if (value < minimum || value > maximum)
    fail(std::string(name) + " is outside the glTF schema range");
}

int checkedIndex(int index, size_t count, int offset, std::string_view name) {
  if (index == -1) return -1;
  if (index < 0 || static_cast<size_t>(index) >= count ||
      index > std::numeric_limits<int>::max() - offset)
    fail(std::string(name) + " reference is out of range");
  return index + offset;
}

int arrayOffset(size_t destination, size_t source) {
  if (destination > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      source > static_cast<size_t>(std::numeric_limits<int>::max()) - destination)
    fail("combined resource table exceeds the glTF index range");
  return static_cast<int>(destination);
}

template<class V> Value array(const V& v, size_t count) {
  Value::Array values;
  for (size_t i = 0; i < count; ++i) {
    finite(v[i], "material array component");
    values.emplace_back(double(v[i]));
  }
  return Value(std::move(values));
}

void remapTextureObjects(Value& value, int offset, size_t sourceTextureCount) {
  if (value.IsObject()) {
    auto& object = value.Get<Object>();
    for (auto& [key, child] : object) {
      if (key.ends_with("Texture") && child.IsObject()) {
        auto& info = child.Get<Object>();
        if (auto it = info.find("index"); it != info.end()) {
          if (!it->second.IsInt()) fail("extension texture index must be an integer");
          const auto index = checkedIndex(it->second.Get<int>(), sourceTextureCount,
                                           offset, "extension texture");
          if (index < 0) fail("extension texture index must be nonnegative");
          it->second = Value(index);
        }
      }
      remapTextureObjects(child, offset, sourceTextureCount);
    }
  } else if (value.IsArray()) {
    for (auto& child : value.Get<Value::Array>())
      remapTextureObjects(child, offset, sourceTextureCount);
  }
}

template<class Info> void offsetTexture(Info& info, int offset, size_t count) {
  info.index = checkedIndex(info.index, count, offset, "material texture");
}

void absoluteUri(std::string& uri, const std::filesystem::path& path) {
  if (uri.empty() || uri.starts_with("data:") || uri.find("://") != std::string::npos)
    return;
  std::string decoded;
  if (!tinygltf::URIDecode(uri, &decoded, nullptr))
    throw std::runtime_error("Cannot resolve imported image URI for export");
  auto absolute = std::filesystem::absolute(path.parent_path() /
                                           std::filesystem::u8path(decoded));
  const auto utf8 = absolute.generic_u8string();
  // This remains a URI, consumed by one further URI decode during packing.
  // Encode '%' and '+' as well as spaces so literal source filenames survive.
  constexpr char hex[] = "0123456789ABCDEF";
  uri.clear();
  for (const auto codeUnit : utf8) {
    const auto c = static_cast<unsigned char>(codeUnit);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '.' ||
        c == '-' || c == '_' || c == '~')
      uri.push_back(static_cast<char>(c));
    else {
      uri.push_back('%');
      uri.push_back(hex[c >> 4]);
      uri.push_back(hex[c & 15]);
    }
  }
}

int textureIndex(uint32_t index, std::span<const int32_t> mapping, int importedIndex = -1) {
  if (index == std::numeric_limits<uint32_t>::max()) return -1;
  if (importedIndex >= 0) return importedIndex;
  if (index >= mapping.size() || mapping[index] < 0)
    throw std::runtime_error("Scene save cannot resolve a material texture resource");
  return mapping[index];
}

Object transformParameters(const material::TextureTransform& transform) {
  return Object{{"offset", array(transform.offset, 2)}, {"scale", array(transform.scale, 2)},
                {"rotation", Value(double(transform.rotation))}};
}

Object reconcileTransform(const Value* imported, const Object& parameters) {
  Object result;
  if (imported && imported->IsObject()) result = imported->Get<Object>();
  // The runtime UV selection is written on the enclosing texture info. An
  // imported extension override must not override that current selection.
  for (const auto* key : {"offset", "scale", "rotation", "texCoord"}) result.erase(key);
  for (const auto& [key, value] : parameters) result[key] = value;
  return result;
}

template<class Info> void setTexture(Info& info, uint32_t index,
                                    const material::TextureTransform& transform,
                                    std::span<const int32_t> mapping, int importedIndex = -1) {
  info.index = textureIndex(index, mapping, importedIndex);
  if (info.index >= 0) {
    if (transform.texCoord > 1) fail("only runtime UV0 and UV1 can be exported");
    finite(transform.rotation, "texture rotation");
    for (int i = 0; i < 2; ++i) {
      finite(transform.offset[i], "texture offset");
      finite(transform.scale[i], "texture scale");
    }
  }
  info.texCoord = static_cast<int>(transform.texCoord);
  Value importedTransform;
  if (const auto found = info.extensions.find("KHR_texture_transform"); found != info.extensions.end())
    importedTransform = found->second;
  info.extensions.erase("KHR_texture_transform");
  info.extensions_json_string.clear();
  if (info.index < 0) return;
  if (transform.offset != glm::vec2(0.0f) || transform.scale != glm::vec2(1.0f) ||
      transform.rotation != 0.0f || importedTransform.IsObject()) {
    info.extensions["KHR_texture_transform"] =
        Value(reconcileTransform(&importedTransform, transformParameters(transform)));
  }
}

Value texture(uint32_t index, const material::TextureTransform& transform,
              std::span<const int32_t> mapping, int importedIndex = -1) {
  tinygltf::TextureInfo info;
  setTexture(info, index, transform, mapping, importedIndex);
  Object object{{"index", Value(info.index)}, {"texCoord", Value(info.texCoord)}};
  if (!info.extensions.empty()) object["extensions"] = Value(info.extensions);
  return Value(std::move(object));
}

void scalarChannel(uint32_t index, const material::TextureTransform& transform,
                    uint32_t channel, std::string_view name) {
  if (index != kInvalidTexture && transform.channel <= 3 && transform.channel != channel)
    fail(std::string(name) + " uses a custom channel and requires texture baking");
}

void validateMaterial(const material::Material& m,const std::array<bool,23>& legacy = {}) {
  for (int i = 0; i < 4; ++i) range(m.baseColor[i], 0, 1, "baseColor");
  for (int i = 0; i < 3; ++i) {
    range(m.emissiveColor[i], 0, std::numeric_limits<float>::max(), "emissiveColor");
    range(m.specularColorFactor[i], 0, 1, "specularColorFactor");
    range(m.attenuationColor[i], 0, 1, "attenuationColor");
    range(m.sheenColorFactor[i], 0, 1, "sheenColorFactor");
    range(m.diffuseTransmissionColorFactor[i], 0, 1, "diffuseTransmissionColorFactor");
  }
  for (const auto [value, name] : std::array{
      std::pair{m.metallicFactor, "metallicFactor"},
      std::pair{m.roughnessFactor, "roughnessFactor"},
      std::pair{m.occlusionStrength, "occlusionStrength"},
      std::pair{m.specularFactor, "specularFactor"},
      std::pair{m.transmissionFactor, "transmissionFactor"},
      std::pair{m.clearcoatFactor, "clearcoatFactor"},
      std::pair{m.clearcoatRoughnessFactor, "clearcoatRoughnessFactor"},
      std::pair{m.sheenRoughnessFactor, "sheenRoughnessFactor"},
      std::pair{m.iridescenceFactor, "iridescenceFactor"},
      std::pair{m.anisotropyStrength, "anisotropyStrength"},
      std::pair{m.diffuseTransmissionFactor, "diffuseTransmissionFactor"}})
    range(value, 0, 1, name);
  for (const auto [value, name] : std::array{
      std::pair{m.alphaCutoff, "alphaCutoff"},
      std::pair{m.dispersion, "dispersion"},
      std::pair{m.emissiveStrength, "emissiveStrength"},
      std::pair{m.thicknessFactor, "thicknessFactor"},
      std::pair{m.iridescenceThicknessMinimum, "iridescenceThicknessMinimum"},
      std::pair{m.iridescenceThicknessMaximum, "iridescenceThicknessMaximum"}})
    range(value, 0, std::numeric_limits<float>::max(), name);
  finite(m.ior, "ior");
  if (m.ior != 0 && m.ior < 1) fail("ior must be zero or at least one");
  range(m.iridescenceIor, 1, std::numeric_limits<float>::max(), "iridescenceIor");
  finite(m.normalTextureScale, "normalTextureScale");
  finite(m.clearcoatNormalTextureScale, "clearcoatNormalTextureScale");
  finite(m.opacityFactor, "opacityFactor");
  finite(m.anisotropyRotation, "anisotropyRotation");
  if (std::isnan(m.attenuationDistance) || m.attenuationDistance <= 0)
    fail("attenuationDistance must be positive, or positive infinity");
  if (m.alphaMode != material::AlphaMode::Opaque && m.alphaMode != material::AlphaMode::Mask &&
      m.alphaMode != material::AlphaMode::Blend)
    fail("alphaMode is invalid");
  if(!legacy[size_t(GltfMaterialTextureSlot::Specular)]) scalarChannel(m.specularTextureIndex, m.specularTextureTransform, 3, "specularTexture");
  if(!legacy[size_t(GltfMaterialTextureSlot::Transmission)]) scalarChannel(m.transmissionTextureIndex, m.transmissionTextureTransform, 0, "transmissionTexture");
  scalarChannel(m.clearcoatTextureIndex, m.clearcoatTextureTransform, 0, "clearcoatTexture");
  scalarChannel(m.clearcoatRoughnessTextureIndex, m.clearcoatRoughnessTextureTransform, 1, "clearcoatRoughnessTexture");
  scalarChannel(m.thicknessTextureIndex, m.thicknessTextureTransform, 1, "thicknessTexture");
  scalarChannel(m.sheenRoughnessTextureIndex, m.sheenRoughnessTextureTransform, 3, "sheenRoughnessTexture");
  scalarChannel(m.iridescenceTextureIndex, m.iridescenceTextureTransform, 0, "iridescenceTexture");
  scalarChannel(m.iridescenceThicknessTextureIndex, m.iridescenceThicknessTextureTransform, 1, "iridescenceThicknessTexture");
  scalarChannel(m.diffuseTransmissionTextureIndex, m.diffuseTransmissionTextureTransform, 3, "diffuseTransmissionTexture");
  for (const auto& [index, channel] : {std::pair{m.anisotropyTextureIndex, m.anisotropyTextureTransform.channel},
                                      std::pair{m.diffuseTransmissionColorTextureIndex, m.diffuseTransmissionColorTextureTransform.channel}})
    if (index != kInvalidTexture && channel <= 3)
      fail("anisotropy/color textures use packed RGB and custom scalar channels require texture baking");
}
using Slot=GltfMaterialTextureSlot;
using TransformMember=material::TextureTransform material::Material::*;
struct LegacySlot {Slot slot;TextureMember texture;TransformMember transform;std::array<const char*,2> names;bool custom{};};
constexpr std::array legacySlots{
 LegacySlot{Slot::Roughness,&material::Material::roughnessTextureIndex,&material::Material::roughnessTextureTransform,{"roughnessTexture",nullptr},true},
 LegacySlot{Slot::Metalness,&material::Material::metalnessTextureIndex,&material::Material::metalnessTextureTransform,{"metalnessTexture",nullptr},true},
 LegacySlot{Slot::Specular,&material::Material::specularTextureIndex,&material::Material::specularTextureTransform,{"specularTexture",nullptr}},
 LegacySlot{Slot::Height,&material::Material::heightTextureIndex,&material::Material::heightTextureTransform,{"heightTexture","displacementTexture"},true},
 LegacySlot{Slot::Opacity,&material::Material::opacityTextureIndex,&material::Material::opacityTextureTransform,{"opacityTexture",nullptr},true},
 LegacySlot{Slot::Transmission,&material::Material::transmissionTextureIndex,&material::Material::transmissionTextureTransform,{"refractionTexture","transmissionTexture"}}
};
std::array<bool,23> proveLegacyBindings(const material::Material& runtime,const tinygltf::Material* source,
    const ImportedMaterialTextureBindings* bindings) {
 std::array<bool,23> result{};
 for(const auto& slot:legacySlots) {
  const auto resource=runtime.*slot.texture;if(resource==kInvalidTexture) continue;
  const auto selected=source?ReadGltfMaterialTextureBinding(*source,slot.slot):GltfMaterialTextureBinding{};
  if(selected.index>=0 && selected.legacy) {
   if(!bindings || bindings->runtimeIndices[size_t(slot.slot)]!=resource) fail("legacy texture export requires its exact retained source/runtime binding proof");
   result[size_t(slot.slot)]=true;
  } else if(slot.custom) fail("custom roughness/metalness/height/opacity export requires an explicit retained source binding");
 }return result;
}
tinygltf::Parameter* winningLegacy(tinygltf::Material& source,const LegacySlot& slot) {
 for(const auto* name:slot.names) if(name) for(auto* map:{&source.values,&source.additionalValues})
  if(const auto found=map->find(name);found!=map->end()) return &found->second;
 return nullptr;
}
tinygltf::Parameter* winningFactor(tinygltf::Material& source,const char* name) {
 tinygltf::Parameter* result{};for(auto* map:{&source.values,&source.additionalValues})
  if(const auto found=map->find(name);found!=map->end()) result=&found->second;
 return result;
}
void syncLegacy(tinygltf::Material& source,const material::Material& runtime,const std::array<bool,23>& proven) {
 for(const auto& slot:legacySlots) {
  if(runtime.*slot.texture==kInvalidTexture) {
   for(const auto* name:slot.names) if(name) {source.values.erase(name);source.additionalValues.erase(name);}continue;
  }
  if(!proven[size_t(slot.slot)]) continue;
  auto* parameter=winningLegacy(source,slot);if(!parameter) fail("proved legacy source definition disappeared");
  const auto& transform=runtime.*slot.transform;
  if(transform.texCoord>1 || transform.channel>4) fail("legacy texture UV/channel is outside its supported range");
  auto& values=parameter->json_double_value;
  const auto integer=[&](const char* name,uint32_t value,uint32_t defaultValue) {
   const auto existing=values.find(name);if(existing==values.end() && value==defaultValue) return;
   if(existing==values.end() || existing->second!=double(value)) values[name]=double(value);
  };
  const auto component=[&](const char* name,float value,float defaultValue) {
   finite(value,name);const auto existing=values.find(name);
   if(existing==values.end() && std::bit_cast<uint32_t>(value)==std::bit_cast<uint32_t>(defaultValue)) return;
   if(existing!=values.end() && std::bit_cast<uint32_t>(float(existing->second))==std::bit_cast<uint32_t>(value)) return;
   values[name]=double(value);
  };
  // The exact source index is retained under the opaque binding witness above.
  // A deduplicated GPU resource or a reverse mapping never chooses this ID.
  integer("texCoord",transform.texCoord,0);integer("channel",transform.channel,4);
  component("offsetX",transform.offset.x,0);component("offsetY",transform.offset.y,0);
  component("scaleX",transform.scale.x,1);component("scaleY",transform.scale.y,1);component("rotation",transform.rotation,0);
 }
 const auto factor=[&](const char* name,float value,bool unit) {
  auto* parameter=winningFactor(source,name);if(!parameter) return;
  finite(value,name);if(unit) range(value,0,1,name);
  if(std::bit_cast<uint32_t>(float(parameter->number_value))!=std::bit_cast<uint32_t>(value)) parameter->number_value=double(value);
 };
 factor("opacity",runtime.opacityFactor,true);factor("heightScale",runtime.heightScale,false);factor("heightOffset",runtime.heightOffset,false);
 if(!source.extensions.contains("KHR_materials_specular")) factor("specular",runtime.specularFactor,true);
 if(!source.extensions.contains("KHR_materials_transmission")) factor("refraction",runtime.transmissionFactor,true);
}

} // namespace

ImportedMaterialTextureBindings snapshotMaterialTextureBindings(const material::Material& material) {
  ImportedMaterialTextureBindings result;
  for (size_t i = 0; i < kTextureMembers.size(); ++i) result.runtimeIndices[i] = material.*kTextureMembers[i];
  return result;
}

std::vector<tinygltf::Material> appendExportResources(
    tinygltf::Model& destination, const tinygltf::Model& source,
    const std::filesystem::path& sourcePath) {
  if (&destination == &source) fail("cannot append resource tables to themselves");
  const int bufferOffset = arrayOffset(destination.buffers.size(), source.buffers.size());
  const int viewOffset = arrayOffset(destination.bufferViews.size(), source.bufferViews.size());
  const int imageOffset = arrayOffset(destination.images.size(), source.images.size());
  const int samplerOffset = arrayOffset(destination.samplers.size(), source.samplers.size());
  const int textureOffset = arrayOffset(destination.textures.size(), source.textures.size());
  // Validate and prepare independent payloads before mutating the destination.
  auto buffers = source.buffers;
  auto views = source.bufferViews;
  auto images = source.images;
  auto textures = source.textures;
  auto materials = source.materials;
  for (auto& buffer : buffers) {
    if (buffer.data.empty()) absoluteUri(buffer.uri, sourcePath);
  }
  for (auto& view : views) {
    view.buffer = checkedIndex(view.buffer, source.buffers.size(), bufferOffset, "buffer view");
    if (view.buffer < 0) fail("buffer view has no source buffer");
  }
  for (auto& image : images) {
    image.bufferView = checkedIndex(image.bufferView, source.bufferViews.size(), viewOffset, "image buffer view");
    absoluteUri(image.uri, sourcePath);
  }
  for (auto& texture : textures) {
    texture.source = checkedIndex(texture.source, source.images.size(), imageOffset, "texture image");
    texture.sampler = checkedIndex(texture.sampler, source.samplers.size(), samplerOffset, "texture sampler");
    // Alternate image extensions carry image indices, not texture indices.
    for (auto& [name, extension] : texture.extensions) {
      if (name != "KHR_texture_basisu" && name != "MSFT_texture_dds" && name != "EXT_texture_webp" && name != "EXT_texture_avif")
        continue;
      if (!extension.IsObject()) continue;
      auto& object = extension.Get<Object>();
      if (auto it = object.find("source"); it != object.end()) {
        if (!it->second.IsInt()) fail("alternate texture source must be an integer");
        const auto index = checkedIndex(it->second.Get<int>(), source.images.size(), imageOffset, "alternate texture image");
        if (index < 0) fail("alternate texture image must be nonnegative");
        it->second = Value(index);
      }
    }
  }
  for (auto& m : materials) {
    offsetTexture(m.pbrMetallicRoughness.baseColorTexture, textureOffset, source.textures.size());
    offsetTexture(m.pbrMetallicRoughness.metallicRoughnessTexture, textureOffset, source.textures.size());
    offsetTexture(m.normalTexture, textureOffset, source.textures.size());
    offsetTexture(m.occlusionTexture, textureOffset, source.textures.size());
    offsetTexture(m.emissiveTexture, textureOffset, source.textures.size());
    for (auto& [_, extension] : m.extensions)
      remapTextureObjects(extension, textureOffset, source.textures.size());
    for(auto* values:{&m.values,&m.additionalValues}) for(const auto name:KnownLegacyMaterialTextureFields())
      if(const auto found=values->find(std::string(name));found!=values->end()) {
        const auto index=found->second.json_double_value.find("index");
        if(index==found->second.json_double_value.end() || !std::isfinite(index->second) || index->second<0 || index->second>INT_MAX || std::trunc(index->second)!=index->second)
          fail("legacy texture source index is malformed");
        index->second=checkedIndex(int(index->second),source.textures.size(),textureOffset,"legacy material texture");
      }
  }
  destination.buffers.insert(destination.buffers.end(), std::make_move_iterator(buffers.begin()), std::make_move_iterator(buffers.end()));
  destination.bufferViews.insert(destination.bufferViews.end(), std::make_move_iterator(views.begin()), std::make_move_iterator(views.end()));
  destination.images.insert(destination.images.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
  destination.samplers.insert(destination.samplers.end(), source.samplers.begin(), source.samplers.end());
  destination.textures.insert(destination.textures.end(), std::make_move_iterator(textures.begin()), std::make_move_iterator(textures.end()));
  for (const auto& name : source.extensionsUsed)
    if (std::ranges::find(destination.extensionsUsed, name) == destination.extensionsUsed.end())
      destination.extensionsUsed.push_back(name);
  for (const auto& name : source.extensionsRequired) {
    if (std::ranges::find(destination.extensionsRequired, name) == destination.extensionsRequired.end())
      destination.extensionsRequired.push_back(name);
    if (std::ranges::find(destination.extensionsUsed, name) == destination.extensionsUsed.end())
      destination.extensionsUsed.push_back(name);
  }
  return materials;
}

int32_t appendRepackedScalarTexture(tinygltf::Model& model, int32_t sourceTexture,
                                   uint32_t fromChannel, uint32_t toChannel,
                                   uint64_t& remainingPixelBytes) {
  if (fromChannel > 3 || toChannel > 3 || fromChannel == toChannel ||
      sourceTexture < 0 || size_t(sourceTexture) >= model.textures.size() ||
      model.images.size() >= INT_MAX || model.textures.size() >= INT_MAX)
    fail("invalid scalar texture repacking request");
  const auto& source = model.textures[size_t(sourceTexture)];
  if (source.sampler < -1 || (source.sampler >= 0 && size_t(source.sampler) >= model.samplers.size()))
    fail("scalar texture sampler is out of range");
  const auto selected = SelectTextureImageSource(model, sourceTexture);
  if (selected.kind != TextureImageSourceKind::Core)
    fail("scalar texture repacking requires a core image source");
  material::TextureDecodeOptions options;
  options.maxDecodedBytes = std::min(options.maxDecodedBytes, remainingPixelBytes);
  options.allowedFormats = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_UNORM};
  auto decoded = DecodeSelectedTextureImage(model, selected, TextureImageRole::Data, options);
  const auto& swizzle = decoded.componentMapping;
  if (decoded.authoredMipChain || decoded.mips.size() != 1 ||
      swizzle.r != VK_COMPONENT_SWIZZLE_IDENTITY || swizzle.g != VK_COMPONENT_SWIZZLE_IDENTITY ||
      swizzle.b != VK_COMPONENT_SWIZZLE_IDENTITY || swizzle.a != VK_COMPONENT_SWIZZLE_IDENTITY)
    fail("scalar texture repacking requires an unswizzled image without authored mip levels");
  const auto& mip = decoded.mips.front();
  const size_t componentBytes = decoded.format == VK_FORMAT_R16G16B16A16_UNORM ? 2 : 1;
  if (mip.bytes.size() > remainingPixelBytes ||
      mip.bytes.size() != uint64_t(mip.width) * mip.height * 4 * componentBytes)
    fail("scalar texture repacking exceeds its pixel budget");
  tinygltf::Image image;
  image.width = int(mip.width);
  image.height = int(mip.height);
  image.component = 4;
  image.bits = int(componentBytes * 8);
  image.pixel_type = componentBytes == 2 ? TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
                                         : TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.mimeType = "image/png";
  const auto* bytes = reinterpret_cast<const unsigned char*>(mip.bytes.data());
  image.image.assign(bytes, bytes + mip.bytes.size());
  for (size_t offset = 0; offset < image.image.size(); offset += 4 * componentBytes)
    std::copy_n(bytes + offset + fromChannel * componentBytes, componentBytes,
                image.image.data() + offset + toChannel * componentBytes);
  tinygltf::Texture texture;
  texture.source = int(model.images.size());
  texture.sampler = source.sampler;
  model.images.reserve(model.images.size() + 1);
  model.textures.reserve(model.textures.size() + 1);
  model.images.push_back(std::move(image));
  model.textures.push_back(std::move(texture));
  remainingPixelBytes -= mip.bytes.size();
  return int32_t(model.textures.size() - 1);
}

tinygltf::Material exportMaterial(const material::Material& m,
                                  std::span<const int32_t> mapping,
                                  const tinygltf::Material* imported,
                                  const ImportedMaterialTextureBindings* importedBindings) {
  if(imported) (void)ValidateLegacyMaterialSerializationInput(std::span<const tinygltf::Material>(imported,1),size_t(INT_MAX));
  const auto legacy=proveLegacyBindings(m,imported,importedBindings);
  validateMaterial(m,legacy);
  tinygltf::Material result = imported ? *imported : tinygltf::Material{};
  if (m.specularGlossinessWorkflow)
    fail("specular-glossiness runtime approximations require texture baking before export");
  if (!result.lods.empty()) fail("material level-of-detail references require an explicit material mapping");
  result.extensions.erase("KHR_materials_pbrSpecularGlossiness");
  result.extensions_json_string.clear();
  // Recognized legacy inputs retain their proven source fields. The project
  // writer serializes those fields without overriding standard PBR factors.
  constexpr auto invalid = std::numeric_limits<uint32_t>::max();
  const bool retainedOpacity=winningFactor(result,"opacity")!=nullptr;
  syncLegacy(result,m,legacy);
  result.pbrMetallicRoughness.baseColorFactor = {m.baseColor.r,m.baseColor.g,m.baseColor.b,
      retainedOpacity?double(m.baseColor.a):double(m.baseColor.a) * std::clamp(double(m.opacityFactor), 0.0, 1.0)};
  result.pbrMetallicRoughness.metallicFactor = m.metallicFactor;
  result.pbrMetallicRoughness.roughnessFactor = m.roughnessFactor;
  const double emissionScale = std::max({1.0, double(m.emissiveColor.r),
                                        double(m.emissiveColor.g), double(m.emissiveColor.b)});
  result.emissiveFactor = {m.emissiveColor.r / emissionScale,
                           m.emissiveColor.g / emissionScale, m.emissiveColor.b / emissionScale};
  result.alphaMode = m.alphaMode == material::AlphaMode::Mask ? "MASK" : m.alphaMode == material::AlphaMode::Blend ? "BLEND" : "OPAQUE";
  result.alphaCutoff = m.alphaCutoff;
  result.doubleSided = m.doubleSided;
  const auto retainedIndex = [&](TextureMember member, int sourceIndex) {
    if (!imported || !importedBindings || sourceIndex < 0 || m.*member == kInvalidTexture) return -1;
    const auto found = std::ranges::find(kTextureMembers, member);
    const auto slot = static_cast<size_t>(found - kTextureMembers.begin());
    return slot < kTextureMembers.size() && importedBindings->runtimeIndices[slot] == m.*member ? sourceIndex : -1;
  };
  const auto setImportedTexture = [&](auto& info, TextureMember member, const auto& transform) {
    setTexture(info, m.*member, transform, mapping, retainedIndex(member, info.index));
  };
  setImportedTexture(result.pbrMetallicRoughness.baseColorTexture,&material::Material::baseColorTextureIndex,m.baseColorTextureTransform);
  setImportedTexture(result.pbrMetallicRoughness.metallicRoughnessTexture,&material::Material::metallicRoughnessTextureIndex,m.metallicRoughnessTextureTransform);
  setImportedTexture(result.normalTexture,&material::Material::normalTextureIndex,m.normalTextureTransform);
  result.normalTexture.scale = m.normalTextureScale;
  setImportedTexture(result.occlusionTexture,&material::Material::occlusionTextureIndex,m.occlusionTextureTransform);
  result.occlusionTexture.strength = m.occlusionStrength;
  setImportedTexture(result.emissiveTexture,&material::Material::emissiveTextureIndex,m.emissiveTextureTransform);
  auto extension = [&](const char* name, Object values, bool active) {
    if (!active && !result.extensions.contains(name)) return;
    if (auto found = result.extensions.find(name); found != result.extensions.end() && found->second.IsObject()) {
      auto retained = found->second.Get<Object>();
      for (auto& [key,value] : values) {
        if (value.Type() == tinygltf::NULL_TYPE) retained.erase(key);
        else if (key.ends_with("Texture") && value.IsObject() &&
                 retained.contains(key) && retained[key].IsObject()) {
          auto merged = retained[key].Get<Object>();
          Object extensions;
          if (merged.contains("extensions") && merged["extensions"].IsObject())
            extensions = merged["extensions"].Get<Object>();
          Value importedTransform;
          if (const auto old = extensions.find("KHR_texture_transform"); old != extensions.end())
            importedTransform = old->second;
          extensions.erase("KHR_texture_transform");
          for (const auto& [property, replacement] : value.Get<Object>()) {
            if (property == "extensions" && replacement.IsObject()) {
              for (const auto& [extensionName, extensionValue] : replacement.Get<Object>()) {
                if (extensionName == "KHR_texture_transform" && extensionValue.IsObject())
                  extensions[extensionName] = Value(reconcileTransform(&importedTransform, extensionValue.Get<Object>()));
                else extensions[extensionName] = extensionValue;
              }
            } else merged[property] = replacement;
          }
          if (!extensions.contains("KHR_texture_transform") && importedTransform.IsObject())
            extensions["KHR_texture_transform"] = Value(reconcileTransform(
                &importedTransform, transformParameters(material::TextureTransform{})));
          if (extensions.empty()) merged.erase("extensions");
          else merged["extensions"] = Value(std::move(extensions));
          retained[key] = Value(std::move(merged));
        } else retained[key] = std::move(value);
      }
      result.extensions[name] = Value(std::move(retained));
    } else {
      std::erase_if(values, [](const auto& entry) {
        return entry.second.Type() == tinygltf::NULL_TYPE;
      });
      result.extensions[name] = Value(std::move(values));
    }
  };
  const auto sourceExtensionIndex = [&](const char* extensionName, const char* name) {
    if (!imported) return -1;
    const auto found = imported->extensions.find(extensionName);
    if (found == imported->extensions.end() || !found->second.IsObject()) return -1;
    const auto& info = found->second.Get(name);
    if (!info.IsObject() || !info.Get("index").IsInt()) return -1;
    return info.Get("index").Get<int>();
  };
  auto optionalTexture = [&](Object& object, const char* extensionName, const char* name,
                             TextureMember member, const material::TextureTransform& transform) {
    const auto position=std::ranges::find(kTextureMembers,member);
    if(position!=kTextureMembers.end() && legacy[size_t(position-kTextureMembers.begin())]) {object[name]=Value();return;}
    const auto index = m.*member;
    if (index != invalid) object[name] = texture(index,transform,mapping,
        retainedIndex(member, sourceExtensionIndex(extensionName, name)));
    // A missing runtime texture deliberately removes its imported binding.
    else object[name] = Value();
  };
  if (m.unlit) {
    // The renderer adds emission to unlit base color, while glTF unlit ignores
    // emission and excludes emissive_strength. Preserving that combination
    // requires baking both texture/vertex-color inputs into an unlit base map.
    if (m.emissiveStrength > 0 && m.emissiveColor != glm::vec3(0))
      fail("unlit materials with additive emission require texture baking before export");
    result.extensions["KHR_materials_unlit"] = Value(Object{});
    // KHR material lighting extensions exclude unlit materials. Their inputs
    // are inactive in the renderer; retaining them would emit invalid glTF.
    for (const auto* name : {"KHR_materials_ior", "KHR_materials_dispersion",
        "KHR_materials_emissive_strength", "KHR_materials_specular",
        "KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_clearcoat",
        "KHR_materials_sheen", "KHR_materials_iridescence", "KHR_materials_anisotropy",
        "KHR_materials_diffuse_transmission"})
      result.extensions.erase(name);
    return result;
  }
  else result.extensions.erase("KHR_materials_unlit");
  extension("KHR_materials_ior", {{"ior",Value(double(m.ior))}}, m.ior != 1.5f);
  extension("KHR_materials_dispersion", {{"dispersion",Value(double(m.dispersion))}}, m.dispersion != 0);
  const double emissionStrength = double(m.emissiveStrength) * emissionScale;
  if (emissionStrength > std::numeric_limits<float>::max()) fail("emissive radiance exceeds the runtime numeric range");
  extension("KHR_materials_emissive_strength", {{"emissiveStrength",Value(emissionStrength)}}, emissionStrength != 1);
  Object specular{{"specularFactor",Value(double(m.specularFactor))},{"specularColorFactor",array(m.specularColorFactor,3)}};
  optionalTexture(specular,"KHR_materials_specular","specularTexture",&material::Material::specularTextureIndex,m.specularTextureTransform);
  optionalTexture(specular,"KHR_materials_specular","specularColorTexture",&material::Material::specularColorTextureIndex,m.specularColorTextureTransform);
  if (m.specularColorTextureIndex == invalid && m.specularTextureIndex != invalid && !legacy[size_t(Slot::Specular)])
    specular["specularColorTexture"] = texture(m.specularTextureIndex, m.specularTextureTransform, mapping,
        retainedIndex(&material::Material::specularTextureIndex,
                      sourceExtensionIndex("KHR_materials_specular", "specularTexture")));
  extension("KHR_materials_specular",std::move(specular),
      m.specularFactor != 1 || m.specularColorFactor != glm::vec3(1) ||
      m.specularTextureIndex != invalid || m.specularColorTextureIndex != invalid);
  // glTF factors multiply their texture samples. Keeping a texture binding
  // must never turn an authored zero factor or black color into one.
  Object transmission{{"transmissionFactor",Value(double(m.transmissionFactor))}};
  optionalTexture(transmission,"KHR_materials_transmission","transmissionTexture",&material::Material::transmissionTextureIndex,m.transmissionTextureTransform);
  extension("KHR_materials_transmission",std::move(transmission), m.transmissionFactor != 0 || m.transmissionTextureIndex != invalid);
  Object volume{{"thicknessFactor",Value(double(m.thicknessFactor))},{"attenuationColor",array(m.attenuationColor,3)}};
  if (std::isfinite(m.attenuationDistance)) volume["attenuationDistance"] = Value(double(m.attenuationDistance));
  else volume["attenuationDistance"] = Value();
  optionalTexture(volume,"KHR_materials_volume","thicknessTexture",&material::Material::thicknessTextureIndex,m.thicknessTextureTransform);
  extension("KHR_materials_volume",std::move(volume), m.thicknessFactor != 0 ||
      m.thicknessTextureIndex != invalid || m.attenuationColor != glm::vec3(1) || std::isfinite(m.attenuationDistance));
  Object clearcoat{{"clearcoatFactor",Value(double(m.clearcoatFactor))},{"clearcoatRoughnessFactor",Value(double(m.clearcoatRoughnessFactor))}};
  optionalTexture(clearcoat,"KHR_materials_clearcoat","clearcoatTexture",&material::Material::clearcoatTextureIndex,m.clearcoatTextureTransform);
  optionalTexture(clearcoat,"KHR_materials_clearcoat","clearcoatRoughnessTexture",&material::Material::clearcoatRoughnessTextureIndex,m.clearcoatRoughnessTextureTransform);
  optionalTexture(clearcoat,"KHR_materials_clearcoat","clearcoatNormalTexture",&material::Material::clearcoatNormalTextureIndex,m.clearcoatNormalTextureTransform);
  if (m.clearcoatNormalTextureIndex != invalid) clearcoat["clearcoatNormalTexture"].Get<Object>()["scale"] = Value(double(m.clearcoatNormalTextureScale));
  extension("KHR_materials_clearcoat",std::move(clearcoat), m.clearcoatFactor != 0 ||
      m.clearcoatRoughnessFactor != 0 || m.clearcoatTextureIndex != invalid ||
      m.clearcoatRoughnessTextureIndex != invalid || m.clearcoatNormalTextureIndex != invalid);
  Object sheen{{"sheenColorFactor",array(m.sheenColorFactor,3)},{"sheenRoughnessFactor",Value(double(m.sheenRoughnessFactor))}};
  optionalTexture(sheen,"KHR_materials_sheen","sheenColorTexture",&material::Material::sheenColorTextureIndex,m.sheenColorTextureTransform);
  optionalTexture(sheen,"KHR_materials_sheen","sheenRoughnessTexture",&material::Material::sheenRoughnessTextureIndex,m.sheenRoughnessTextureTransform);
  extension("KHR_materials_sheen",std::move(sheen), m.sheenColorFactor != glm::vec3(0) ||
      m.sheenRoughnessFactor != 0 || m.sheenColorTextureIndex != invalid || m.sheenRoughnessTextureIndex != invalid);
  Object iridescence{{"iridescenceFactor",Value(double(m.iridescenceFactor))},{"iridescenceIor",Value(double(m.iridescenceIor))},{"iridescenceThicknessMinimum",Value(double(m.iridescenceThicknessMinimum))},{"iridescenceThicknessMaximum",Value(double(m.iridescenceThicknessMaximum))}};
  optionalTexture(iridescence,"KHR_materials_iridescence","iridescenceTexture",&material::Material::iridescenceTextureIndex,m.iridescenceTextureTransform);
  optionalTexture(iridescence,"KHR_materials_iridescence","iridescenceThicknessTexture",&material::Material::iridescenceThicknessTextureIndex,m.iridescenceThicknessTextureTransform);
  extension("KHR_materials_iridescence",std::move(iridescence), m.iridescenceFactor != 0 ||
      m.iridescenceIor != 1.3f || m.iridescenceThicknessMinimum != 100 ||
      m.iridescenceThicknessMaximum != 400 || m.iridescenceTextureIndex != invalid ||
      m.iridescenceThicknessTextureIndex != invalid);
  // Keep the source's explicit/default leaf presence. These extensions use
  // strict multiplication, like the standard material slots above.
  const auto authored = [&](const char* name, const char* property) {
    const auto found = result.extensions.find(name);
    return found != result.extensions.end() && found->second.IsObject() && found->second.Has(property);
  };
  Object anisotropy;
  if (m.anisotropyStrength != 0 || authored("KHR_materials_anisotropy", "anisotropyStrength"))
    anisotropy["anisotropyStrength"] = Value(double(m.anisotropyStrength));
  if (m.anisotropyRotation != 0 || authored("KHR_materials_anisotropy", "anisotropyRotation"))
    anisotropy["anisotropyRotation"] = Value(double(m.anisotropyRotation));
  optionalTexture(anisotropy,"KHR_materials_anisotropy","anisotropyTexture",
      &material::Material::anisotropyTextureIndex,m.anisotropyTextureTransform);
  extension("KHR_materials_anisotropy",std::move(anisotropy), m.anisotropyStrength != 0 ||
      m.anisotropyRotation != 0 || m.anisotropyTextureIndex != invalid);
  // KHR_materials_diffuse_transmission is a Release Candidate. Preserve its
  // current standard fields and unknown metadata without claiming full future
  // revisions of the extension.
  Object diffuseTransmission;
  if (m.diffuseTransmissionFactor != 0 || authored("KHR_materials_diffuse_transmission", "diffuseTransmissionFactor"))
    diffuseTransmission["diffuseTransmissionFactor"] = Value(double(m.diffuseTransmissionFactor));
  if (m.diffuseTransmissionColorFactor != glm::vec3(1) || authored("KHR_materials_diffuse_transmission", "diffuseTransmissionColorFactor"))
    diffuseTransmission["diffuseTransmissionColorFactor"] = array(m.diffuseTransmissionColorFactor,3);
  optionalTexture(diffuseTransmission,"KHR_materials_diffuse_transmission","diffuseTransmissionTexture",
      &material::Material::diffuseTransmissionTextureIndex,m.diffuseTransmissionTextureTransform);
  optionalTexture(diffuseTransmission,"KHR_materials_diffuse_transmission","diffuseTransmissionColorTexture",
      &material::Material::diffuseTransmissionColorTextureIndex,m.diffuseTransmissionColorTextureTransform);
  extension("KHR_materials_diffuse_transmission",std::move(diffuseTransmission),
      m.diffuseTransmissionFactor != 0 || m.diffuseTransmissionColorFactor != glm::vec3(1) ||
      m.diffuseTransmissionTextureIndex != invalid || m.diffuseTransmissionColorTextureIndex != invalid);
  return result;
}

void appendMaterialExtensionDeclarations(tinygltf::Model& model) {
  auto add = [&](const std::string& name) {
    if (std::ranges::find(model.extensionsUsed, name) == model.extensionsUsed.end())
      model.extensionsUsed.push_back(name);
  };
  const auto collect = [&](const auto& self, const Value& value) -> void {
    if (value.IsObject()) {
      for (const auto& [key, child] : value.Get<Object>()) {
        if (key == "extensions" && child.IsObject())
          for (const auto& [name, extension] : child.Get<Object>()) {
            add(name);
            self(self, extension);
          }
        else self(self, child);
      }
    } else if (value.IsArray()) {
      for (const auto& child : value.Get<Value::Array>()) self(self, child);
    }
  };
  const auto info = [&](const auto& textureInfo) {
    if (textureInfo.index < 0) return;
    for (const auto& [name, extension] : textureInfo.extensions) {
      add(name);
      collect(collect, extension);
    }
  };
  for (const auto& material : model.materials) {
    for (const auto& [name, extension] : material.extensions) {
      add(name);
      collect(collect, extension);
    }
    info(material.pbrMetallicRoughness.baseColorTexture);
    info(material.pbrMetallicRoughness.metallicRoughnessTexture);
    info(material.normalTexture);
    info(material.occlusionTexture);
    info(material.emissiveTexture);
  }
}
} // namespace container::geometry::gltf
