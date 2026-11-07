#include <Container/utility/MaterialSamplerKey.h>

#include <gtest/gtest.h>
#include <array>
#include <set>
#include <stdexcept>

namespace {
namespace material = container::material;
}

TEST(MaterialSamplerKey, EveryValidFilterWrapCombinationIsDistinctAndReversible) {
  std::set<uint32_t> keys;
  for (int min : {9728, 9729, 9984, 9985, 9986, 9987})
    for (int mag : {9728, 9729})
      for (int s : {10497, 33071, 33648})
        for (int t : {10497, 33071, 33648}) {
          const auto key = material::NormalizeMaterialSamplerKey(min, mag, s, t);
          keys.insert(key.value);
          const auto decoded = material::DecodeMaterialSamplerKey(key);
          EXPECT_EQ(decoded.minFilter, min); EXPECT_EQ(decoded.magFilter, mag);
          EXPECT_EQ(decoded.wrapS, s); EXPECT_EQ(decoded.wrapT, t);
        }
  EXPECT_EQ(keys.size(), material::kMaterialSamplerKeyCount);
  EXPECT_EQ(*keys.begin(), 0u);
  EXPECT_EQ(*keys.rbegin(), material::kMaterialSamplerKeyCount - 1);
}

TEST(MaterialSamplerKey, PreservesLegacyWrapIdsAndOmittedDefaultPolicy) {
  constexpr std::array wraps{10497, 33071, 33648};
  for (uint32_t t = 0; t < 3; ++t)
    for (uint32_t s = 0; s < 3; ++s) {
      const auto omitted = material::NormalizeMaterialSamplerKey(-1, -1, wraps[s], wraps[t]);
      EXPECT_EQ(omitted.value, s + t * 3);
      EXPECT_EQ(omitted, material::NormalizeMaterialSamplerKey(9987, 9729, wraps[s], wraps[t]));
      const auto vk = material::MakeMaterialSamplerCreateInfo(omitted);
      EXPECT_EQ(vk.minFilter, VK_FILTER_LINEAR); EXPECT_EQ(vk.magFilter, VK_FILTER_LINEAR);
      EXPECT_EQ(vk.mipmapMode, VK_SAMPLER_MIPMAP_MODE_LINEAR);
      EXPECT_EQ(vk.maxLod, VK_LOD_CLAMP_NONE);
    }
}

TEST(MaterialSamplerKey, UnknownEnumsUseCompatibleDefaultsIndependently) {
  EXPECT_EQ(material::NormalizeMaterialSamplerKey(123, 456, 789, -99).value, 0u);
  const auto key = material::NormalizeMaterialSamplerKey(9984, 9984, 33071, 7);
  const auto decoded = material::DecodeMaterialSamplerKey(key);
  EXPECT_EQ(decoded.minFilter, 9984); EXPECT_EQ(decoded.magFilter, 9729);
  EXPECT_EQ(decoded.wrapS, 33071); EXPECT_EQ(decoded.wrapT, 10497);
  EXPECT_EQ(material::NormalizeMaterialSamplerKey(-1, 9728), material::NormalizeMaterialSamplerKey(9987, 9728));
}

TEST(MaterialSamplerKey, MapsAllMinificationAndMagnificationModesWithoutAnisotropy) {
  struct Expected { int source; VkFilter texel; VkSamplerMipmapMode mip; float maxLod; };
  constexpr std::array expected{
      Expected{9728, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, .25f},
      Expected{9729, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, .25f},
      Expected{9984, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_LOD_CLAMP_NONE},
      Expected{9985, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_LOD_CLAMP_NONE},
      Expected{9986, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_LOD_CLAMP_NONE},
      Expected{9987, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_LOD_CLAMP_NONE}};
  for (const auto row : expected)
    for (int mag : {9728, 9729}) {
      const auto vk = material::MakeMaterialSamplerCreateInfo(material::NormalizeMaterialSamplerKey(row.source, mag));
      EXPECT_EQ(vk.sType, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
      EXPECT_EQ(vk.minFilter, row.texel);
      EXPECT_EQ(vk.magFilter, mag == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
      EXPECT_EQ(vk.mipmapMode, row.mip); EXPECT_EQ(vk.maxLod, row.maxLod);
      EXPECT_FLOAT_EQ(vk.minLod, 0); EXPECT_FLOAT_EQ(vk.mipLodBias, 0);
      EXPECT_EQ(vk.anisotropyEnable, VK_FALSE); EXPECT_EQ(vk.compareEnable, VK_FALSE);
      EXPECT_EQ(vk.unnormalizedCoordinates, VK_FALSE);
    }
}

TEST(MaterialSamplerKey, MapsBothWrapAxesWithoutChangingFiltering) {
  for (const auto [source, expected] : std::array{
      std::pair{10497, VK_SAMPLER_ADDRESS_MODE_REPEAT},
      std::pair{33071, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
      std::pair{33648, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT}}) {
    const auto vk = material::MakeMaterialSamplerCreateInfo(material::NormalizeMaterialSamplerKey(9986, 9728, source, source));
    EXPECT_EQ(vk.addressModeU, expected); EXPECT_EQ(vk.addressModeV, expected);
    EXPECT_EQ(vk.addressModeW, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    EXPECT_EQ(vk.minFilter, VK_FILTER_NEAREST); EXPECT_EQ(vk.magFilter, VK_FILTER_NEAREST);
    EXPECT_EQ(vk.mipmapMode, VK_SAMPLER_MIPMAP_MODE_LINEAR);
  }
}

TEST(MaterialSamplerKey, ReservesAggregatePipelineDescriptorsAndChecksBothDeviceLimits) {
  EXPECT_EQ(material::MaterialSamplerDescriptorCapacity(16, 16), 8u);
  EXPECT_EQ(material::MaterialSamplerDescriptorCapacity(32, 16), 8u);
  EXPECT_EQ(material::MaterialSamplerDescriptorCapacity(16, 32), 8u);
  EXPECT_EQ(material::MaterialSamplerDescriptorCapacity(8, 1024), 0u);
  EXPECT_EQ(material::MaterialSamplerDescriptorCapacity(1, 1), 0u);
  EXPECT_EQ(material::MaterialSamplerDescriptorCapacity(1048576, 1048576), 108u);
  EXPECT_EQ(material::MaterialSamplerDescriptorCapacity(4, 7, 0), 4u);
}

TEST(MaterialSamplerKey, ActiveSlotPlansDeduplicateAndForgetPreviousScenePolicies) {
  const auto nearest = material::NormalizeMaterialSamplerKey(9728, 9728);
  const auto clamp = material::NormalizeMaterialSamplerKey(9987, 9729, 33071);
  const auto a = material::PlanMaterialSamplerSlots(std::array{nearest, nearest, clamp}, 3);
  ASSERT_EQ(a.keys.size(), 3u);
  EXPECT_EQ(a.slot({0}), 0u); EXPECT_EQ(a.slot(nearest), 1u); EXPECT_EQ(a.slot(clamp), 2u);
  const auto b = material::PlanMaterialSamplerSlots(std::array{clamp}, 2);
  ASSERT_EQ(b.keys.size(), 2u);
  EXPECT_EQ(b.slot({0}), 0u); EXPECT_EQ(b.slot(clamp), 1u);
  EXPECT_THROW((void)b.slot(nearest), std::out_of_range);
  EXPECT_EQ(a.slot(nearest), 1u);
}

TEST(MaterialSamplerKey, CapacityFailuresAndInvalidKeysNeverReturnPartialPlans) {
  const auto nearest = material::NormalizeMaterialSamplerKey(9728, 9728);
  EXPECT_THROW((void)material::PlanMaterialSamplerSlots({}, 0), std::runtime_error);
  EXPECT_THROW((void)material::PlanMaterialSamplerSlots(std::array{nearest}, 1), std::runtime_error);
  EXPECT_NO_THROW((void)material::PlanMaterialSamplerSlots(std::array{nearest, nearest}, 2));
  EXPECT_THROW((void)material::PlanMaterialSamplerSlots(std::array{material::MaterialSamplerKey{108}}, 108), std::invalid_argument);
  EXPECT_THROW((void)material::DecodeMaterialSamplerKey({108}), std::invalid_argument);
  EXPECT_THROW((void)material::MakeMaterialSamplerCreateInfo({UINT32_MAX}), std::invalid_argument);
}
