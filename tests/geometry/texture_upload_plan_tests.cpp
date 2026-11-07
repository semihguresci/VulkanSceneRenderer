#include <Container/utility/TextureUploadPlan.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <stdexcept>

namespace {
using namespace container::material;
using namespace container::gpu;
TextureImageData image(VkFormat format, uint32_t width, uint32_t height, size_t levels, bool authored = true) {
  TextureImageData result;
  result.format = format; result.authoredMipChain = authored;
  for (size_t level = 0; level < levels; ++level) {
    result.mips.push_back({width, height, std::vector<std::byte>(TextureMipByteSize(format, width, height))});
    width = std::max(width / 2, 1u); height = std::max(height / 2, 1u);
  }
  return result;
}
} // namespace

TEST(TextureUploadPlan, PreservesAuthoredSingleAndPartialMipChains) {
  for (const size_t levels : {size_t(1), size_t(2), size_t(4)}) {
    const auto data = image(VK_FORMAT_R8G8B8A8_UNORM, 8, 4, levels);
    const auto plan = PlanTextureImageUpload(data, 4096, true);
    EXPECT_EQ(plan.suppliedMipLevels, levels); EXPECT_EQ(plan.allocatedMipLevels, levels);
    EXPECT_FALSE(plan.generateMipmaps); ASSERT_EQ(plan.copies.size(), levels);
    for (size_t level = 0; level < levels; ++level) {
      EXPECT_EQ(plan.copies[level].imageExtent.width, data.mips[level].width);
      EXPECT_EQ(plan.copies[level].imageExtent.height, data.mips[level].height);
      EXPECT_EQ(plan.copies[level].imageSubresource.mipLevel, level);
    }
  }
}
TEST(TextureUploadPlan, GeneratesOnlyForSingleUnauthoredLevelWithSupportedBlits) {
  const auto data = image(VK_FORMAT_R16_UNORM, 9, 3, 1, false);
  const auto generated = PlanTextureImageUpload(data, 4096, true);
  EXPECT_TRUE(generated.generateMipmaps); EXPECT_EQ(generated.allocatedMipLevels, 4u);
  EXPECT_EQ(generated.stagingBytes, 54u);
  EXPECT_EQ(PlanTextureImageUpload(data, 4096, false).allocatedMipLevels, 1u);
  EXPECT_FALSE(PlanTextureImageUpload(image(VK_FORMAT_R16_UNORM, 1, 1, 1, false), 4096, true).generateMipmaps);
}
TEST(TextureUploadPlan, AlignsEveryMipForNarrowTexelsAndCompressedBlocks) {
  const auto narrow = PlanTextureImageUpload(image(VK_FORMAT_R16_UNORM, 3, 3, 2), 4096, false, 256, 16);
  EXPECT_EQ(narrow.copies[0].bufferOffset, 0u); EXPECT_EQ(narrow.copies[1].bufferOffset, 32u);
  EXPECT_EQ(narrow.stagingBytes, 34u);
  const auto blocks = PlanTextureImageUpload(image(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 5, 7, 3), 4096, false);
  EXPECT_EQ(blocks.copies[1].bufferOffset, 32u); EXPECT_EQ(blocks.copies[2].bufferOffset, 40u);
  EXPECT_EQ(blocks.stagingBytes, 48u);
}
TEST(TextureUploadPlan, RejectsMalformedChainsAndBudgetOverflowBeforeAllocation) {
  EXPECT_THROW((void)PlanTextureImageUpload({}, 4096, false), std::invalid_argument);
  auto data = image(VK_FORMAT_R8G8B8A8_UNORM, 8, 4, 2);
  data.mips[1].width = 5;
  EXPECT_THROW((void)PlanTextureImageUpload(data, 4096, false), std::invalid_argument);
  data = image(VK_FORMAT_R8G8B8A8_UNORM, 8, 4, 2); data.mips[1].bytes.pop_back();
  EXPECT_THROW((void)PlanTextureImageUpload(data, 4096, false), std::invalid_argument);
  data = image(VK_FORMAT_R16_UNORM, 3, 3, 2);
  EXPECT_THROW((void)PlanTextureImageUpload(data, 4096, false, 33, 16), std::invalid_argument);
  EXPECT_THROW((void)PlanTextureImageUpload(data, 2, false), std::invalid_argument);
  EXPECT_THROW((void)PlanTextureImageUpload(data, 4096, false, 256, 0), std::invalid_argument);
  data.mips.push_back(data.mips.back());
  EXPECT_THROW((void)PlanTextureImageUpload(data, 4096, false), std::invalid_argument);
}

TEST(TextureUploadPlan, RejectsInvalidViewSwizzlesBeforeVulkanAllocation) {
  for (auto member : {&VkComponentMapping::r, &VkComponentMapping::g,
                      &VkComponentMapping::b, &VkComponentMapping::a}) {
    auto data = image(VK_FORMAT_R16_UNORM, 1, 1, 1);
    data.componentMapping.*member = static_cast<VkComponentSwizzle>(99);
    EXPECT_THROW((void)PlanTextureImageUpload(data, 4096, false), std::invalid_argument);
  }
}
