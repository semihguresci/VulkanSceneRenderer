#pragma once

#include <Container/utility/TextureImageData.h>
#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <span>
#include <string_view>
#include <vector>

namespace container::geometry::gltf {

enum class TextureImageSourceKind { Core, Basisu, Webp, Dds };
enum class TextureImageRole { Color, Data };

struct ImageSourceCapabilities {
  bool basisu{};
  bool webp{};
  bool dds{true};
};
[[nodiscard]] ImageSourceCapabilities DefaultImageSourceCapabilities() noexcept;

struct SelectedTextureImage {
  int textureIndex{-1};
  int imageIndex{-1};
  TextureImageSourceKind kind{TextureImageSourceKind::Core};
  [[nodiscard]] bool strictKhrBasisu() const noexcept {
    return kind == TextureImageSourceKind::Basisu;
  }
  [[nodiscard]] std::string_view extension() const noexcept;
};

struct ImageSourcePlan {
  // One selection per texture, in texture table order.
  std::vector<SelectedTextureImage> textures;
  std::vector<bool> selectedImages;
  // Images referenced solely by an unselected alternate are left undecoded by
  // the legacy loader. Core references and ordinary unreferenced images retain
  // their existing decoding behavior.
  std::vector<bool> unsupportedAlternateOnly;
};

// Metadata-only validation, suitable before any external buffer/image reads.
// Enabled BasisU is preferred to DDS, then WebP and the ordinary core source. Every
// authored source index is validated even when its codec is unavailable.
[[nodiscard]] ImageSourcePlan SelectTextureImageSources(
    const nlohmann::json& source,
    ImageSourceCapabilities capabilities = DefaultImageSourceCapabilities());
[[nodiscard]] ImageSourcePlan SelectTextureImageSources(
    const tinygltf::Model& source,
    ImageSourceCapabilities capabilities = DefaultImageSourceCapabilities());
[[nodiscard]] SelectedTextureImage SelectTextureImageSource(
    const tinygltf::Model& source, int textureIndex,
    ImageSourceCapabilities capabilities = DefaultImageSourceCapabilities());

// Retained images contain exact encoded bytes with as_is=true. Legacy decoded
// images remain supported without reducing 16-bit channels to eight bits.
// The caller supplies its physical-device format profile and role. Only a
// selected KHR_texture_basisu source enables strict BasisU/DFD validation.
// A caller may also request strict validation for a core alias of an image
// selected as BasisU by another texture; that request is preserved.
[[nodiscard]] container::material::TextureImageData DecodeSelectedTextureImage(
    const tinygltf::Model& source, const SelectedTextureImage& selection,
    TextureImageRole role,
    container::material::TextureDecodeOptions options = {});

// Recognizes containers unsupported by TinyGLTF's ordinary decoder, without
// decoding or allocating pixel output. MIME is advisory; magic is authoritative.
[[nodiscard]] bool RequiresEncodedTextureRetention(
    std::span<const unsigned char> bytes) noexcept;
[[nodiscard]] std::string_view EncodedTextureMimeType(
    std::span<const unsigned char> bytes) noexcept;

// Keep recognized containers byte-for-byte. Ordinary formats without a portable
// MIME signature (for example TGA/PNM/PSD) retain decoded 8/16-bit RGBA pixels so
// scene export can emit PNG without depending on the original file.
[[nodiscard]] tinygltf::Image RetainTextureImageForExport(
    std::span<const std::byte> bytes,
    container::material::TextureDecodeOptions options = {});

} // namespace container::geometry::gltf
