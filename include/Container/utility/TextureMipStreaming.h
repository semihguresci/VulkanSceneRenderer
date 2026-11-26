#pragma once

#include "Container/utility/TextureImageData.h"

#include <memory>
#include <optional>
#include <stop_token>

namespace container::material {

// Retains an explicitly supplied chain byte-for-byte. Ordinary RGBA8 and
// BGRA8 images receive a box-filtered pyramid with linear-light color and
// linear alpha. An empty result keeps the image on the normal upload path.
[[nodiscard]] std::shared_ptr<const TextureImageData> PrepareTextureMipSource(
    const TextureImageData& image, uint64_t maxBytes, std::stop_token stop = {});

// The finest authored level that fits the fallback extent, or the coarsest
// authored level when the source pyramid ends at a larger resolution.
[[nodiscard]] uint32_t TextureFallbackFirstMip(const TextureImageData& source,
                                               uint32_t maxExtent);

// Rebase the requested source level to image level zero. This owns only the
// selected levels and can go through the regular typed texture uploader.
[[nodiscard]] TextureImageData CopyTextureMipTail(const TextureImageData& source,
                                                   uint32_t firstMip);

struct PreparedTextureMipFallback {
  std::shared_ptr<const TextureImageData> source;
  TextureImageData fallback;
  uint32_t firstMip{};
  uint64_t retainedBytes{};
};

// The combined retained source and fallback pixel storage fits maxBytes.
[[nodiscard]] std::optional<PreparedTextureMipFallback> PrepareTextureMipFallback(
    const TextureImageData& image, uint32_t maxExtent, uint64_t maxBytes,
    std::stop_token stop = {});

} // namespace container::material
