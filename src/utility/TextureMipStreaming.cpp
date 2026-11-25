#include "Container/utility/TextureMipStreaming.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace container::material {
namespace {

const auto& srgbToLinear() {
  static const auto table = [] {
    std::array<double, 256> values{};
    for (size_t i = 0; i < values.size(); ++i) {
      const double x = static_cast<double>(i) / 255.0;
      values[i] = x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    }
    return values;
  }();
  return table;
}

std::byte linearToSrgb(double x) {
  const auto srgb = x <= 0.0031308 ? x * 12.92 : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
  return std::byte(std::lround(std::clamp(srgb, 0.0, 1.0) * 255.0));
}

TextureMipLevel downsample(const TextureMipLevel& base, bool srgb) {
  const auto width = std::max(base.width >> 1, 1u);
  const auto height = std::max(base.height >> 1, 1u);
  constexpr size_t channels = 4;
  TextureMipLevel result{width, height, std::vector<std::byte>(size_t(width) * height * channels)};
  // Weight every source texel by area so odd dimensions include the edge.
  const double scaleX = double(base.width) / width;
  const double scaleY = double(base.height) / height;
  for (uint32_t y = 0; y < height; ++y) {
    const double top = y * scaleY, bottom = (y + 1) * scaleY;
    for (uint32_t x = 0; x < width; ++x) {
      const double left = x * scaleX, right = (x + 1) * scaleX;
      std::array<double, channels> accum{};
      double area = 0.0;
      for (auto srcY = uint32_t(top); srcY < base.height && srcY < bottom; ++srcY) {
        const auto vy = std::min(bottom, double(srcY + 1)) - std::max(top, double(srcY));
        for (auto srcX = uint32_t(left); srcX < base.width && srcX < right; ++srcX) {
          const auto weight = vy * (std::min(right, double(srcX + 1)) - std::max(left, double(srcX)));
          const auto offset = (size_t(srcY) * base.width + srcX) * channels;
          for (size_t channel = 0; channel < channels; ++channel) {
            const auto code = std::to_integer<uint8_t>(base.bytes[offset + channel]);
            const double value = srgb && channel < 3 ? srgbToLinear()[code] : code / 255.0;
            accum[channel] += value * weight;
          }
          area += weight;
        }
      }
      const auto offset = (size_t(y) * width + x) * channels;
      for (size_t channel = 0; channel < channels; ++channel) {
        const auto value = accum[channel] / area;
        result.bytes[offset + channel] = srgb && channel < 3 ? linearToSrgb(value)
            : std::byte(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
      }
    }
  }
  return result;
}

} // namespace

std::shared_ptr<const TextureImageData> PrepareTextureMipSource(
    const TextureImageData& image, uint64_t maxBytes) {
  if (image.mips.empty() || maxBytes == 0)
    return {};
  const auto& base = image.mips.front();
  if (base.width == 0 || base.height == 0)
    throw std::invalid_argument("Texture mip source has an empty base level");
  const auto fullCount = static_cast<size_t>(std::bit_width(std::max(base.width, base.height)));
  if (image.mips.size() > fullCount)
    throw std::invalid_argument("Texture mip source has too many levels");
  const bool generated = !image.authoredMipChain && image.mips.size() == 1;
  const bool srgb = image.format == VK_FORMAT_R8G8B8A8_SRGB || image.format == VK_FORMAT_B8G8R8A8_SRGB;
  const bool supportedGeneration = srgb || image.format == VK_FORMAT_R8G8B8A8_UNORM ||
      image.format == VK_FORMAT_B8G8R8A8_UNORM;
  const auto levelCount = generated && supportedGeneration ? fullCount : image.mips.size();
  if (levelCount < 2)
    return {};

  for (size_t level = 0; level < levelCount; ++level) {
    const auto width = std::max(base.width >> level, 1u);
    const auto height = std::max(base.height >> level, 1u);
    const auto bytes = TextureMipByteSize(image.format, width, height);
    if (level < image.mips.size()) {
      const auto& mip = image.mips[level];
      if (mip.width != width || mip.height != height || mip.bytes.size() != bytes)
        throw std::invalid_argument("Texture mip source does not match its format or expected extent");
    }
    if (bytes > maxBytes)
      return {};
    maxBytes -= bytes;
  }
  auto source = std::make_shared<TextureImageData>(image);
  source->mips.reserve(levelCount);
  while (source->mips.size() < levelCount)
    source->mips.push_back(downsample(source->mips.back(), srgb));
  source->authoredMipChain = true;
  return source;
}

uint32_t TextureFallbackFirstMip(const TextureImageData& source, uint32_t maxExtent) {
  if (source.mips.empty() || maxExtent == 0)
    throw std::invalid_argument("Texture fallback requires an existing mip and nonzero extent");
  uint32_t level = 0;
  while (level + 1 < source.mips.size() &&
         std::max(source.mips[level].width, source.mips[level].height) > maxExtent)
    ++level;
  return level;
}

TextureImageData CopyTextureMipTail(const TextureImageData& source, uint32_t firstMip) {
  if (firstMip >= source.mips.size())
    throw std::out_of_range("Texture mip tail starts outside the source image");
  TextureImageData tail;
  tail.format = source.format;
  tail.componentMapping = source.componentMapping;
  tail.mips.assign(source.mips.begin() + firstMip, source.mips.end());
  tail.authoredMipChain = true;
  return tail;
}

std::optional<PreparedTextureMipFallback> PrepareTextureMipFallback(
    const TextureImageData& image, uint32_t maxExtent, uint64_t maxBytes) {
  if (!maxExtent || image.mips.empty() ||
      std::max(image.mips.front().width, image.mips.front().height) <= maxExtent) {
    return std::nullopt;
  }
  auto source = PrepareTextureMipSource(image, maxBytes);
  if (!source) {
    return std::nullopt;
  }
  const uint32_t first = TextureFallbackFirstMip(*source, maxExtent);
  uint64_t retainedBytes = 0;
  for (size_t i = 0; i < source->mips.size(); ++i) {
    const auto bytes = source->mips[i].bytes.size();
    if (bytes > maxBytes - retainedBytes) {
      return std::nullopt;
    }
    retainedBytes += bytes;
    if (i >= first) {
      if (bytes > maxBytes - retainedBytes) {
        return std::nullopt;
      }
      retainedBytes += bytes;
    }
  }
  auto fallback = CopyTextureMipTail(*source, first);
  return PreparedTextureMipFallback{std::move(source), std::move(fallback), first, retainedBytes};
}

} // namespace container::material
