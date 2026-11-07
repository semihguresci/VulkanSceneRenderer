#pragma once

#include <Container/common/CommonVulkan.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace container::material {

inline constexpr uint32_t kMaterialSamplerKeyCount = 108;
inline constexpr uint32_t kReservedMaterialPipelineSamplers = 8;
inline constexpr uint32_t kInvalidMaterialSamplerSlot = std::numeric_limits<uint32_t>::max();

struct MaterialSamplerKey {
  uint32_t value{0};
  bool operator==(const MaterialSamplerKey&) const = default;
};

struct MaterialSamplerDescription {
  int minFilter{9987}; // LINEAR_MIPMAP_LINEAR
  int magFilter{9729}; // LINEAR
  int wrapS{10497};   // REPEAT
  int wrapT{10497};
};

// Omitted/unknown filters use linear magnification and trilinear minification;
// omitted/unknown wrap modes use repeat. Keys 0..8 retain the existing wrap
// encoding, with those default filters. Valid explicit policies remain distinct.
[[nodiscard]] MaterialSamplerKey NormalizeMaterialSamplerKey(
    int minFilter = -1, int magFilter = -1,
    int wrapS = 10497, int wrapT = 10497);
[[nodiscard]] MaterialSamplerDescription DecodeMaterialSamplerKey(MaterialSamplerKey key);

// Explicit NEAREST/LINEAR minification stays on mip zero with maxLod .25,
// allowing the minification filter to differ from the magnification filter.
// Anisotropy is disabled to retain the requested texel/mipmap filter behavior.
[[nodiscard]] VkSamplerCreateInfo MakeMaterialSamplerCreateInfo(MaterialSamplerKey key);

// Includes the other sets in the largest scene/light/frame/DDGI pipeline layout.
// Zero means the device has no remaining material sampler budget.
[[nodiscard]] uint32_t MaterialSamplerDescriptorCapacity(
    uint32_t perStageLimit, uint32_t pipelineLayoutLimit,
    uint32_t reserved = kReservedMaterialPipelineSamplers);

struct MaterialSamplerSlotPlan {
  // Dense descriptor order, with the default repeat sampler at slot zero.
  std::vector<MaterialSamplerKey> keys;
  std::array<uint32_t, kMaterialSamplerKeyCount> slots = [] {
    std::array<uint32_t, kMaterialSamplerKeyCount> result;
    result.fill(kInvalidMaterialSamplerSlot);
    return result;
  }();
  [[nodiscard]] uint32_t slot(MaterialSamplerKey key) const;
};

// Rebuild for the current active texture table; prior scenes never consume its
// descriptor budget. First-use order is stable and duplicate policies share a
// slot. Invalid keys or insufficient capacity throw before a plan is returned.
[[nodiscard]] MaterialSamplerSlotPlan PlanMaterialSamplerSlots(
    std::span<const MaterialSamplerKey> keys, uint32_t capacity);

}  // namespace container::material
