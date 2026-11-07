#include <Container/geometry/GltfImageSources.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace container::geometry::gltf {
namespace {
using Json = nlohmann::json;
constexpr const char* kBasisu = "KHR_texture_basisu";
constexpr const char* kWebp = "EXT_texture_webp";
constexpr const char* kDds = "MSFT_texture_dds";
constexpr const char* kAvif = "EXT_texture_avif";
[[noreturn]] void fail(std::string_view context, std::string_view reason) {
  throw std::runtime_error(std::string(context) + ": " + std::string(reason));
}
bool listed(const Json& source, const char* table, std::string_view extension) {
  const auto found = source.find(table);
  if (found == source.end()) return false;
  if (!found->is_array()) fail(table, "must be an array");
  return std::any_of(found->begin(), found->end(), [&](const Json& value) {
    return value.is_string() && value.get_ref<const std::string&>() == extension;
  });
}
const Json& table(const Json& source, const char* name) {
  static const Json empty = Json::array();
  const auto found = source.find(name);
  if (found == source.end()) return empty;
  if (!found->is_array()) fail(name, "must be an array");
  return *found;
}
size_t index(const Json& value, size_t count, std::string_view context) {
  if (!value.is_number_integer()) fail(context, "must reference an existing image with a nonnegative int index");
  uint64_t result{};
  if (value.is_number_unsigned()) result = value.get<uint64_t>();
  else {
    const auto signedValue = value.get<int64_t>();
    if (signedValue < 0) fail(context, "must reference an existing image with a nonnegative int index");
    result = static_cast<uint64_t>(signedValue);
  }
  if (result >= count || result > INT_MAX)
    fail(context, "must reference an existing image with a nonnegative int index");
  return static_cast<size_t>(result);
}
void imageMetadata(const Json& image, std::string_view context) {
  if (!image.is_object()) fail(context, "image must be an object");
  const auto uri = image.find("uri");
  const auto view = image.find("bufferView");
  if (uri != image.end() && (!uri->is_string() || uri->get_ref<const std::string&>().empty()))
    fail(context, "image URI must be a nonempty string");
  if ((uri != image.end()) == (view != image.end()))
    fail(context, "image must have exactly one URI or bufferView");
  if (view != image.end()) {
    if (!view->is_number_integer() || (view->is_number_integer() && !view->is_number_unsigned() && view->get<int64_t>() < 0))
      fail(context, "image bufferView must be a nonnegative index");
    const auto mime = image.find("mimeType");
    if (mime == image.end() || !mime->is_string() || mime->get<std::string>().empty())
      fail(context, "bufferView image requires a MIME type");
  }
  if (const auto mime = image.find("mimeType"); mime != image.end() && !mime->is_string())
    fail(context, "image MIME type must be a string");
}
bool starts(std::span<const unsigned char> bytes, std::span<const unsigned char> magic) noexcept {
  return bytes.size() >= magic.size() && std::memcmp(bytes.data(), magic.data(), magic.size()) == 0;
}
Json metadata(const tinygltf::Model& source) {
  Json result{{"extensionsUsed", source.extensionsUsed}, {"extensionsRequired", source.extensionsRequired},
              {"images", Json::array()}, {"textures", Json::array()}};
  for (const auto& image : source.images) {
    Json entry = Json::object();
    if (image.bufferView >= 0) entry["bufferView"] = image.bufferView;
    // Selection needs resource presence, never URI contents. In particular do
    // not copy large encoded data URIs when decoded/encoded bytes are resident.
    else if (!image.image.empty()) entry["uri"] = "resident:";
    else if (!image.uri.empty()) entry["uri"] = "external:";
    if (!image.mimeType.empty()) entry["mimeType"] = image.mimeType;
    result["images"].push_back(std::move(entry));
  }
  for (const auto& texture : source.textures) {
    Json entry = Json::object();
    if (texture.source >= 0) entry["source"] = texture.source;
    for (const auto* name : {kBasisu, kDds, kWebp, kAvif}) {
      const auto found = texture.extensions.find(name);
      if (found == texture.extensions.end()) continue;
      const auto& extension = found->second;
      Json value = Json::object();
      if (!extension.IsObject()) value = nullptr;
      else if (extension.Has("source")) {
        const auto& sourceIndex = extension.Get("source");
        if (sourceIndex.IsInt()) value["source"] = sourceIndex.Get<int>();
        else if (sourceIndex.IsNumber()) value["source"] = sourceIndex.GetNumberAsDouble();
        else value["source"] = nullptr;
      }
      entry["extensions"][name] = std::move(value);
    }
    result["textures"].push_back(std::move(entry));
  }
  return result;
}
uint64_t product(uint64_t a, uint64_t b, std::string_view context) {
  if (a && b > UINT64_MAX / a) fail(context, "image byte count overflows");
  return a * b;
}
float linear(float value) {
  return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
}
container::material::TextureImageData decodedImage(const tinygltf::Image& image,
    const container::material::TextureDecodeOptions& options, std::string_view context) {
  if (!options.maxEncodedBytes || !options.maxDecodedBytes || !options.maxDimension || !options.maxMipLevels)
    fail(context, "texture decode limits must be positive");
  if (image.width <= 0 || image.height <= 0 || uint32_t(image.width) > options.maxDimension ||
      uint32_t(image.height) > options.maxDimension || image.component < 1 || image.component > 4)
    fail(context, "legacy decoded image dimensions/components are invalid");
  const bool wide = image.bits == 16 && image.pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
  const bool floating = image.bits == 32 && image.pixel_type == TINYGLTF_COMPONENT_TYPE_FLOAT;
  if (!wide && !floating && !(image.bits == 8 && image.pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE))
    fail(context, "legacy decoded image precision/type is unsupported");
  const auto pixels = product(uint32_t(image.width), uint32_t(image.height), context);
  const auto inputSize = product(product(pixels, uint32_t(image.component), context), uint32_t(image.bits / 8), context);
  if (inputSize != image.image.size()) fail(context, "legacy decoded image byte count disagrees with dimensions");
  const VkFormat format = floating || (wide && options.isSrgb) ? VK_FORMAT_R32G32B32A32_SFLOAT :
      wide ? VK_FORMAT_R16G16B16A16_UNORM : options.isSrgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  if (!options.allowedFormats.empty() && std::find(options.allowedFormats.begin(), options.allowedFormats.end(), format) == options.allowedFormats.end())
    fail(context, "precision-preserving decoded format is absent from the caller's format profile");
  const uint32_t outputWidth = floating || (wide && options.isSrgb) ? 4 : wide ? 2 : 1;
  const auto outputSize = product(product(pixels, 4, context), outputWidth, context);
  if (outputSize > options.maxDecodedBytes || outputSize > SIZE_MAX)
    fail(context, "legacy decoded image exceeds the decoded-byte limit");
  container::material::TextureMipLevel mip{uint32_t(image.width), uint32_t(image.height),
                                         std::vector<std::byte>(static_cast<size_t>(outputSize))};
  for (uint64_t pixel = 0; pixel < pixels; ++pixel) {
    for (uint32_t channel = 0; channel < 4; ++channel) {
      const bool alpha = channel == 3;
      const int sourceChannel = image.component <= 2 ? (alpha ? (image.component == 2 ? 1 : -1) : 0)
                                                   : channel < uint32_t(image.component) ? int(channel) : -1;
      const auto at = (pixel * uint32_t(image.component) + uint32_t(std::max(sourceChannel, 0))) * uint32_t(image.bits / 8);
      if (floating) {
        float value = 1;
        if (sourceChannel >= 0) std::memcpy(&value, image.image.data() + size_t(at), 4);
        if (!std::isfinite(value)) fail(context, "legacy decoded image contains nonfinite floating-point texels");
        std::memcpy(mip.bytes.data() + size_t((pixel * 4 + channel) * 4), &value, 4);
      } else if (wide) {
        uint16_t value = UINT16_MAX;
        if (sourceChannel >= 0) std::memcpy(&value, image.image.data() + size_t(at), 2);
        if (options.isSrgb) {
          const float converted = alpha ? value / 65535.f : linear(value / 65535.f);
          std::memcpy(mip.bytes.data() + size_t((pixel * 4 + channel) * 4), &converted, 4);
        } else std::memcpy(mip.bytes.data() + size_t((pixel * 4 + channel) * 2), &value, 2);
      } else mip.bytes[size_t(pixel * 4 + channel)] = std::byte(sourceChannel < 0 ? 255 : image.image[size_t(at)]);
    }
  }
  return {format, {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY}, {std::move(mip)}, false};
}
} // namespace

ImageSourceCapabilities DefaultImageSourceCapabilities() noexcept {
  return {container::material::HasKtxTextureCodec(), container::material::HasWebpTextureCodec(), true};
}
std::string_view SelectedTextureImage::extension() const noexcept {
  return kind == TextureImageSourceKind::Basisu ? kBasisu : kind == TextureImageSourceKind::Webp ? kWebp : kind == TextureImageSourceKind::Dds ? kDds : "";
}
ImageSourcePlan SelectTextureImageSources(const Json& source, ImageSourceCapabilities capabilities) {
  try {
    for (const auto* name : {kBasisu, kDds, kWebp, kAvif}) {
      const bool enabled = name == kBasisu ? capabilities.basisu : name == kDds ? capabilities.dds : name == kWebp && capabilities.webp;
      if (!enabled && listed(source, "extensionsRequired", name))
        fail(name, "required image decoder is unavailable");
    }
    const auto& images = table(source, "images");
    const auto& textures = table(source, "textures");
    ImageSourcePlan plan;
    plan.selectedImages.resize(images.size());
    plan.unsupportedAlternateOnly.resize(images.size());
    std::vector<bool> core(images.size()), alternate(images.size());
    for (size_t textureIndex = 0; textureIndex < textures.size(); ++textureIndex) {
      const auto context = "texture[" + std::to_string(textureIndex) + "]";
      const auto& texture = textures[textureIndex];
      if (!texture.is_object()) fail(context, "must be an object");
      std::optional<size_t> fallback, basisu, webp, dds;
      if (const auto value = texture.find("source"); value != texture.end()) {
        fallback = index(*value, images.size(), context + ".source (texture.source)");
        imageMetadata(images[*fallback], context + ".source.image[" + std::to_string(*fallback) + "]");
        core[*fallback] = true;
      }
      if (const auto extensions = texture.find("extensions"); extensions != texture.end()) {
        if (!extensions->is_object()) fail(context + ".extensions", "must be an object");
        for (const auto* name : {kBasisu, kDds, kWebp, kAvif}) {
          const auto extension = extensions->find(name);
          if (extension == extensions->end()) continue;
          const auto label = context + ".extensions." + name;
          if (!listed(source, "extensionsUsed", name)) fail(label, "extension is absent from extensionsUsed");
          if (!extension->is_object() || !extension->contains("source")) fail(label, "must specify an image source");
          const auto selected = index(extension->at("source"), images.size(), label + ".source");
          imageMetadata(images[selected], label + ".source.image[" + std::to_string(selected) + "]");
          alternate[selected] = true;
          if (name == kBasisu) basisu = selected;
          if (name == kWebp) webp = selected;
          if (name == kDds) dds = selected;
        }
      }
      SelectedTextureImage selected{int(textureIndex)};
      if (basisu && capabilities.basisu) selected = {int(textureIndex), int(*basisu), TextureImageSourceKind::Basisu};
      else if (dds && capabilities.dds) selected = {int(textureIndex), int(*dds), TextureImageSourceKind::Dds};
      else if (webp && capabilities.webp) selected = {int(textureIndex), int(*webp), TextureImageSourceKind::Webp};
      else if (fallback) selected.imageIndex = int(*fallback);
      else fail(context, "no valid core fallback or enabled alternate image source");
      if (!fallback && !listed(source, "extensionsRequired", std::string(selected.extension()).c_str()))
        fail(context, "an alternate without a core fallback must be listed in extensionsRequired");
      if (selected.kind != TextureImageSourceKind::Core) {
        const auto& image = images[size_t(selected.imageIndex)];
        const auto mime = image.find("mimeType");
        const auto expected = selected.kind == TextureImageSourceKind::Basisu ? "image/ktx2" : selected.kind == TextureImageSourceKind::Dds ? "image/vnd-ms.dds" : "image/webp";
        if (mime != image.end() && !mime->get<std::string>().empty() && mime->get<std::string>() != expected)
          fail(context + ".extensions." + std::string(selected.extension()), "selected image MIME type disagrees with its extension");
      }
      plan.selectedImages[size_t(selected.imageIndex)] = true;
      plan.textures.push_back(selected);
    }
    for (size_t image = 0; image < images.size(); ++image)
      plan.unsupportedAlternateOnly[image] = alternate[image] && !core[image] && !plan.selectedImages[image];
    return plan;
  } catch (const Json::exception& error) { fail("texture image sources", error.what()); }
}
ImageSourcePlan SelectTextureImageSources(const tinygltf::Model& source, ImageSourceCapabilities capabilities) {
  return SelectTextureImageSources(metadata(source), capabilities);
}
SelectedTextureImage SelectTextureImageSource(const tinygltf::Model& source, int textureIndex, ImageSourceCapabilities capabilities) {
  if (textureIndex < 0 || size_t(textureIndex) >= source.textures.size()) fail("texture source", "texture index is out of range");
  return SelectTextureImageSources(source, capabilities).textures[size_t(textureIndex)];
}
bool RequiresEncodedTextureRetention(std::span<const unsigned char> bytes) noexcept {
  const auto mime = EncodedTextureMimeType(bytes);
  return mime == "image/ktx" || mime == "image/ktx2" || mime == "image/vnd-ms.dds" || mime == "image/webp";
}
std::string_view EncodedTextureMimeType(std::span<const unsigned char> bytes) noexcept {
  constexpr std::array<unsigned char, 12> k1{0xab,'K','T','X',' ','1','1',0xbb,13,10,26,10};
  constexpr std::array<unsigned char, 12> k2{0xab,'K','T','X',' ','2','0',0xbb,13,10,26,10};
  constexpr std::array<unsigned char, 4> dds{'D','D','S',' '};
  constexpr std::array<unsigned char, 8> png{137,80,78,71,13,10,26,10};
  if (starts(bytes, k1)) return "image/ktx";
  if (starts(bytes, k2)) return "image/ktx2";
  if (starts(bytes, dds)) return "image/vnd-ms.dds";
  if (starts(bytes, png)) return "image/png";
  if (bytes.size() >= 10 && std::memcmp(bytes.data(), "#?RADIANCE", 10) == 0) return "image/vnd.radiance";
  if (bytes.size() >= 6 && std::memcmp(bytes.data(), "#?RGBE", 6) == 0) return "image/vnd.radiance";
  if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "image/bmp";
  if (bytes.size() >= 6 && (std::memcmp(bytes.data(), "GIF87a", 6) == 0 ||
                          std::memcmp(bytes.data(), "GIF89a", 6) == 0)) return "image/gif";
  if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) return "image/webp";
  if (bytes.size() >= 3 && bytes[0] == 255 && bytes[1] == 216 && bytes[2] == 255) return "image/jpeg";
  return {};
}
tinygltf::Image RetainTextureImageForExport(std::span<const std::byte> bytes,
    container::material::TextureDecodeOptions options) {
  if (bytes.empty() || bytes.size() > options.maxEncodedBytes)
    fail("texture archive", "encoded image is empty or exceeds its byte limit");
  tinygltf::Image image;
  const auto raw = std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  image.mimeType = EncodedTextureMimeType(raw);
  if (!image.mimeType.empty()) {
    image.as_is = true;
    image.image.assign(raw.begin(), raw.end());
    return image;
  }
  options.isSrgb = false;
  options.allowedFormats = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_UNORM};
  const auto decoded = container::material::DecodeTextureImage(bytes, options);
  if (decoded.mips.size() != 1 || (decoded.format != VK_FORMAT_R8G8B8A8_UNORM &&
      decoded.format != VK_FORMAT_R16G16B16A16_UNORM))
    fail("texture archive", "ordinary image cannot be represented as precision-preserving PNG pixels");
  const auto& mip = decoded.mips[0];
  image.width = static_cast<int>(mip.width); image.height = static_cast<int>(mip.height);
  image.component = 4;
  image.bits = decoded.format == VK_FORMAT_R16G16B16A16_UNORM ? 16 : 8;
  image.pixel_type = image.bits == 16 ? TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT : TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.image.assign(reinterpret_cast<const unsigned char*>(mip.bytes.data()),
                     reinterpret_cast<const unsigned char*>(mip.bytes.data() + mip.bytes.size()));
  return image;
}
container::material::TextureImageData DecodeSelectedTextureImage(const tinygltf::Model& source,
    const SelectedTextureImage& selection, TextureImageRole role, container::material::TextureDecodeOptions options) {
  const auto context = "texture[" + std::to_string(selection.textureIndex) + "].image[" + std::to_string(selection.imageIndex) + "]";
  if (selection.textureIndex < 0 || size_t(selection.textureIndex) >= source.textures.size() ||
      selection.imageIndex < 0 || size_t(selection.imageIndex) >= source.images.size()) fail(context, "selected texture/image is out of range");
  const auto expected = SelectTextureImageSource(source, selection.textureIndex);
  if (expected.imageIndex != selection.imageIndex || expected.kind != selection.kind)
    fail(context, "selection disagrees with the texture's enabled source");
  options.isSrgb = role == TextureImageRole::Color;
  options.strictKhrBasisu = options.strictKhrBasisu || selection.strictKhrBasisu();
  const auto& image = source.images[size_t(selection.imageIndex)];
  try {
    if (!image.as_is) {
      if (selection.kind != TextureImageSourceKind::Core || options.strictKhrBasisu)
        fail(context, "alternate/strict BasisU validation requires its retained encoded image");
      return decodedImage(image, options, context);
    }
    if (selection.kind == TextureImageSourceKind::Dds && EncodedTextureMimeType(image.image) != "image/vnd-ms.dds")
      fail(context, "MSFT_texture_dds requires a DDS image");
    if (selection.kind == TextureImageSourceKind::Webp && EncodedTextureMimeType(image.image) != "image/webp")
      fail(context, "EXT_texture_webp requires a WebP image");
    return container::material::DecodeTextureImage(
        {reinterpret_cast<const std::byte*>(image.image.data()), image.image.size()}, options);
  } catch (const std::exception& error) { fail(context, error.what()); }
}
} // namespace container::geometry::gltf
