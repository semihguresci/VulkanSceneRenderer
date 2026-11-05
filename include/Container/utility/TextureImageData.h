#pragma once

#include <Container/common/CommonVulkan.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace container::material {

struct TextureMipLevel {
  uint32_t width{}, height{};
  // Tightly packed texel rows or block rows, without container padding.
  std::vector<std::byte> bytes;
};

struct TextureImageData {
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkComponentMapping componentMapping{
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  std::vector<TextureMipLevel> mips;
  // DDS/KTX levels, including partial pyramids and single authored levels,
  // must be uploaded exactly; the caller must not regenerate/replace them.
  bool authoredMipChain{};
};

struct TextureDecodeOptions {
  bool isSrgb{};
  // Apply KHR_texture_basisu's ETC1S/UASTC, DFD, 2D, metadata and size rules.
  // General DDS/KTX callers leave this false.
  bool strictKhrBasisu{};
  uint64_t maxEncodedBytes{256ull * 1024 * 1024};
  uint64_t maxDecodedBytes{256ull * 1024 * 1024};
  uint32_t maxDimension{16'384};
  uint32_t maxMipLevels{32};
  // Empty permits precision-preserving uncompressed results only. Native
  // GPU blocks require an explicit entry. A nonempty list restricts every
  // result format, including uncompressed fallbacks and float HDR output.
  std::vector<VkFormat> allowedFormats;
};

[[nodiscard]] bool HasKtxTextureCodec() noexcept;
[[nodiscard]] bool HasWebpTextureCodec() noexcept;
[[nodiscard]] uint64_t TextureMipByteSize(VkFormat format, uint32_t width, uint32_t height);

// CPU decoding only: no Vulkan objects, device access or global STB settings.
// Checks encoded/output/dimension/mip limits before output allocation. Codec
// internals may use additional scratch memory; these are data-size limits,
// not an OS-enforced process-memory ceiling.
[[nodiscard]] TextureImageData DecodeTextureImage(
    std::span<const std::byte> encoded, const TextureDecodeOptions& options = {});

} // namespace container::material
