#include "Container/renderer/scene/TextureMipDemand.h"
#include "Container/utility/ModelImportCancellation.h"
#include "Container/utility/TextureMipStreaming.h"
#include "Container/utility/TextureUploadPlan.h"
#include "TextureMipUploadPlan.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <stdexcept>
#include <stop_token>

namespace container {
namespace {

using material::TextureImageData;
using material::TextureMipByteSize;
using material::TextureMipLevel;
using material::CopyTextureMipTail;
using material::PrepareTextureMipSource;
using material::TextureFallbackFirstMip;

TEST(TextureMipDemand, UsesReverseZFrustumAndPerspectiveFootprint) {
  const auto projection = math::perspectiveRH_ReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
  const renderer::TextureMipDemandView view(glm::mat4(1.0f), projection, {720, 720});
  EXPECT_GT(view.projectedPixels({0, 0, -2, 0.5f}), view.projectedPixels({0, 0, -8, 0.5f}));
  EXPECT_GT(view.projectedPixels({1.3f, 0, -2, 0.5f}), 0.0f);
  EXPECT_EQ(view.projectedPixels({5, 0, -2, 0.5f}), 0.0f);
  EXPECT_EQ(view.projectedPixels({0, 0, 2, 0.5f}), 0.0f);
  EXPECT_EQ(view.projectedPixels({0, 0, -102, 0.5f}), 0.0f);
  EXPECT_TRUE(std::isinf(view.projectedPixels({0, 0, -0.12f, 0.05f})));
}

TEST(TextureMipDemand, RespectsOrthographicZoomAndMipHysteresis) {
  const auto projection = math::orthoRH_ReverseZ(-4, 4, -4, 4, 0.1f, 100);
  const renderer::TextureMipDemandView view(glm::mat4(1.0f), projection, {720, 720});
  EXPECT_FLOAT_EQ(view.projectedPixels({0, 0, -2, 1}), 180.0f);
  EXPECT_FLOAT_EQ(view.projectedPixels({0, 0, -40, 1}), 180.0f);
  using renderer::textureMipForFootprint;
  EXPECT_EQ(textureMipForFootprint(600, {1024, 1024}, {1, 1}, 3, 3), 0u);
  EXPECT_EQ(textureMipForFootprint(500, {1024, 1024}, {1, 1}, 3, 0), 0u);
  EXPECT_EQ(textureMipForFootprint(400, {1024, 1024}, {1, 1}, 3, 0), 1u);
  EXPECT_EQ(textureMipForFootprint(500, {1024, 1024}, {0.5f, 1}, 3, 1), 0u);
  EXPECT_EQ(textureMipForFootprint(500, {1024, 1024}, {2, 2}, 3, 3), 2u);
  EXPECT_EQ(textureMipForFootprint(0, {1024, 1024}, {1, 1}, 3, 0), 3u);
}

TextureMipLevel solidMip(VkFormat format, uint32_t width, uint32_t height, std::byte code) {
  return {width, height,
          std::vector<std::byte>(static_cast<size_t>(TextureMipByteSize(format, width, height)), code)};
}

TEST(TextureMipStreaming, CancelledPreparationDoesNotPublishAMipSourceOrFallback) {
  constexpr auto format = VK_FORMAT_R8G8B8A8_SRGB;
  const TextureImageData image{.format = format, .mips = {solidMip(format, 256, 256, std::byte{0x7f})}};
  std::stop_source stop;
  stop.request_stop();

  EXPECT_THROW((void)PrepareTextureMipSource(image, 1024 * 1024, stop.get_token()),
               util::ModelImportCancelled);
  EXPECT_THROW((void)material::PrepareTextureMipFallback(image, 128, 1024 * 1024, stop.get_token()),
               util::ModelImportCancelled);
  EXPECT_EQ(image.mips.size(), 1u);
}

TEST(TextureMipStreaming, AveragesSrgbColorInLinearLightAndKeepsLinearAlpha) {
  const std::vector<std::byte> texels{
      std::byte{0}, std::byte{255}, std::byte{0}, std::byte{0},
      std::byte{255}, std::byte{0}, std::byte{0}, std::byte{64},
      std::byte{0}, std::byte{255}, std::byte{0}, std::byte{128},
      std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
  };
  TextureImageData image{.format = VK_FORMAT_R8G8B8A8_SRGB, .mips = {{2, 2, texels}}};
  image.componentMapping.a = VK_COMPONENT_SWIZZLE_A;
  const auto srgb = PrepareTextureMipSource(image, 20);
  ASSERT_TRUE(srgb);
  ASSERT_EQ(srgb->mips.size(), 2u);
  EXPECT_EQ(srgb->mips.front().bytes, texels);
  EXPECT_EQ(srgb->mips.back().bytes, (std::vector<std::byte>{
      std::byte{188}, std::byte{188}, std::byte{0}, std::byte{112}}));
  EXPECT_EQ(srgb->format, VK_FORMAT_R8G8B8A8_SRGB);
  EXPECT_EQ(srgb->componentMapping.a, VK_COMPONENT_SWIZZLE_A);
  EXPECT_TRUE(srgb->authoredMipChain);
  EXPECT_FALSE(PrepareTextureMipSource(image, 19));

  image.format = VK_FORMAT_R8G8B8A8_UNORM;
  const auto linear = PrepareTextureMipSource(image, 20);
  ASSERT_TRUE(linear);
  EXPECT_EQ(linear->mips.back().bytes, (std::vector<std::byte>{
      std::byte{128}, std::byte{128}, std::byte{0}, std::byte{112}}));
}

TEST(TextureMipStreaming, IncludesOddEdgeTexelsInTheFilteredMip) {
  TextureImageData image{.format = VK_FORMAT_R8G8B8A8_UNORM,
                         .mips = {solidMip(VK_FORMAT_R8G8B8A8_UNORM, 3, 1, std::byte{0})}};
  image.mips[0].bytes[8] = std::byte{255};
  const auto source = PrepareTextureMipSource(image, 100);
  ASSERT_TRUE(source);
  ASSERT_EQ(source->mips.size(), 2u);
  EXPECT_EQ(source->mips[1].width, 1u);
  EXPECT_EQ(source->mips[1].height, 1u);
  EXPECT_EQ(source->mips[1].bytes[0], std::byte{85});
}

TEST(TextureMipStreaming, CopiesAuthoredBcMipTailBytesAndSwizzle) {
  constexpr auto format = VK_FORMAT_BC3_SRGB_BLOCK;
  TextureImageData image{.format = format, .mips = {
      solidMip(format, 7, 9, std::byte{0x19}), solidMip(format, 3, 4, std::byte{0x2e}),
      solidMip(format, 1, 2, std::byte{0xab})},
      .authoredMipChain = true};
  image.componentMapping.r = VK_COMPONENT_SWIZZLE_B;
  const auto source = PrepareTextureMipSource(image, 128);
  ASSERT_TRUE(source);
  EXPECT_EQ(source->mips.size(), 3u);
  EXPECT_EQ(TextureFallbackFirstMip(*source, 9), 0u);
  EXPECT_EQ(TextureFallbackFirstMip(*source, 4), 1u);
  EXPECT_EQ(TextureFallbackFirstMip(*source, 1), 2u);
  const auto tail = CopyTextureMipTail(*source, 1);
  EXPECT_EQ(tail.format, format);
  EXPECT_EQ(tail.componentMapping.r, VK_COMPONENT_SWIZZLE_B);
  EXPECT_TRUE(tail.authoredMipChain);
  ASSERT_EQ(tail.mips.size(), 2u);
  EXPECT_EQ(tail.mips[0].bytes, image.mips[1].bytes);
  EXPECT_EQ(tail.mips[1].bytes, image.mips[2].bytes);

  const auto plan = gpu::PlanTextureImageUpload(tail, 4096, true);
  EXPECT_EQ(plan.extent.width, 3u);
  EXPECT_EQ(plan.extent.height, 4u);
  EXPECT_EQ(plan.allocatedMipLevels, 2u);
  EXPECT_FALSE(plan.generateMipmaps);
  EXPECT_EQ(plan.copies[0].imageSubresource.mipLevel, 0u);
}

TEST(TextureMipStreaming, LeavesUnsupportedSingleLevelsOnExistingUploadPath) {
  constexpr auto format = VK_FORMAT_R16G16B16A16_UNORM;
  TextureImageData highPrecision{.format = format,
      .mips = {solidMip(format, 4, 4, std::byte{})}};
  EXPECT_FALSE(PrepareTextureMipSource(highPrecision, 256));

  constexpr auto rgba = VK_FORMAT_R8G8B8A8_UNORM;
  TextureImageData authoredSingle{.format = rgba,
      .mips = {solidMip(rgba, 4, 4, std::byte{})}, .authoredMipChain = true};
  EXPECT_FALSE(PrepareTextureMipSource(authoredSingle, 256));
  EXPECT_THROW((void)CopyTextureMipTail(authoredSingle, 1), std::out_of_range);
  EXPECT_THROW((void)TextureFallbackFirstMip(authoredSingle, 0), std::invalid_argument);
  highPrecision.mips.push_back(solidMip(format, 3, 2, std::byte{}));
  EXPECT_THROW((void)PrepareTextureMipSource(highPrecision, 256), std::invalid_argument);
}

TEST(TextureMipUploadTile, SplitsCompressedBlockRowsAndTerminalNpotEdges) {
  const auto block = material::TextureBlockShape(VK_FORMAT_BC3_SRGB_BLOCK);
  constexpr VkExtent2D extent{7, 9};
  const auto tile = gpu::detail::planTextureMipTile(block, extent, 2, 0, 47);
  EXPECT_EQ(tile.byteCount, 32u);
  EXPECT_EQ(tile.copy.imageSubresource.mipLevel, 2u);
  EXPECT_EQ(tile.copy.bufferOffset, 0u);
  EXPECT_EQ(tile.copy.bufferRowLength, 0u);
  EXPECT_EQ(tile.copy.imageExtent.width, 7u);
  EXPECT_EQ(tile.copy.imageExtent.height, 4u);

  const auto narrow = gpu::detail::planTextureMipTile(block, extent, 2, 16, 63);
  EXPECT_EQ(narrow.copy.imageOffset.x, 4);
  EXPECT_EQ(narrow.copy.imageExtent.width, 3u);
  EXPECT_EQ(narrow.copy.imageExtent.height, 4u);
  EXPECT_EQ(narrow.byteCount, 16u);

  const auto final = gpu::detail::planTextureMipTile(block, extent, 2, 64, 32);
  EXPECT_EQ(final.copy.imageOffset.y, 8);
  EXPECT_EQ(final.copy.imageExtent.height, 1u);
  EXPECT_EQ(final.byteCount, 32u);
}

TEST(TextureMipUploadTile, CoversRectangularAstcBlocksInBoundedExactCopies) {
  const auto block = material::TextureBlockShape(VK_FORMAT_ASTC_5x4_UNORM_BLOCK);
  constexpr VkExtent2D extent{11, 5};
  const auto bottom = gpu::detail::planTextureMipTile(block, extent, 1, 48, 96);
  EXPECT_EQ(bottom.copy.imageOffset.y, 4);
  EXPECT_EQ(bottom.copy.imageExtent.width, 11u);
  EXPECT_EQ(bottom.copy.imageExtent.height, 1u);
  EXPECT_EQ(bottom.byteCount, 48u);

  VkDeviceSize offset = 0;
  for (const auto budget : std::array<VkDeviceSize, 4>{17, 34, 15, 48}) {
    const auto tile = gpu::detail::planTextureMipTile(block, extent, 1, offset, budget);
    EXPECT_LE(tile.byteCount, budget);
    offset += tile.byteCount;
  }
  EXPECT_EQ(offset, TextureMipByteSize(VK_FORMAT_ASTC_5x4_UNORM_BLOCK, 11, 5));
  EXPECT_EQ(gpu::detail::planTextureMipTile(block, extent, 1, offset, 96).byteCount, 0u);
  EXPECT_THROW((void)gpu::detail::planTextureMipTile(block, extent, 1, offset + 1, 96),
               std::invalid_argument);
}

TEST(TextureMipUploadTile, SupportsUncompressedLinearTexelsAndPauseBudget) {
  const auto block = material::TextureBlockShape(VK_FORMAT_R8G8B8_UNORM);
  constexpr VkExtent2D extent{8, 4};
  const auto rows = gpu::detail::planTextureMipTile(block, extent, 0, 0, 49);
  EXPECT_EQ(rows.copy.imageExtent.width, 8u);
  EXPECT_EQ(rows.copy.imageExtent.height, 2u);
  EXPECT_EQ(rows.byteCount, 48u);
  EXPECT_EQ(gpu::detail::planTextureMipTile(block, extent, 0, 48, 2).byteCount, 0u);
  const auto resume = gpu::detail::planTextureMipTile(block, extent, 0, 48, 6);
  EXPECT_EQ(resume.copy.imageOffset.y, 2);
  EXPECT_EQ(resume.copy.imageExtent.width, 2u);
  EXPECT_EQ(resume.copy.imageExtent.height, 1u);
  EXPECT_EQ(resume.byteCount, 6u);
}

} // namespace
} // namespace container
