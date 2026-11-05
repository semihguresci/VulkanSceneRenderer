#include <Container/utility/MaterialSamplerKey.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace container::material {
namespace {
constexpr std::array minFilters{9987, 9728, 9729, 9984, 9985, 9986};
constexpr std::array magFilters{9729, 9728};
constexpr std::array wrapModes{10497, 33071, 33648};

template <size_t N>
uint32_t ordinal(const std::array<int, N>& values, int value) {
  const auto found = std::ranges::find(values, value);
  return found == values.end() ? 0 : static_cast<uint32_t>(found - values.begin());
}

VkSamplerAddressMode address(int wrap) {
  return wrap == 33071 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE :
         wrap == 33648 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
}  // namespace

MaterialSamplerKey NormalizeMaterialSamplerKey(int minFilter, int magFilter, int wrapS, int wrapT) {
  return {ordinal(wrapModes, wrapS) + 3 * ordinal(wrapModes, wrapT) +
          9 * (2 * ordinal(minFilters, minFilter) + ordinal(magFilters, magFilter))};
}

MaterialSamplerDescription DecodeMaterialSamplerKey(MaterialSamplerKey key) {
  if (key.value >= kMaterialSamplerKeyCount) throw std::invalid_argument("Invalid material sampler key");
  return {minFilters[key.value / 18], magFilters[(key.value / 9) % 2],
          wrapModes[key.value % 3], wrapModes[(key.value / 3) % 3]};
}

VkSamplerCreateInfo MakeMaterialSamplerCreateInfo(MaterialSamplerKey key) {
  const auto description = DecodeMaterialSamplerKey(key);
  VkSamplerCreateInfo result{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  result.magFilter = description.magFilter == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  result.minFilter = description.minFilter == 9728 || description.minFilter == 9984 || description.minFilter == 9986
      ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  const bool baseOnly = description.minFilter == 9728 || description.minFilter == 9729;
  result.mipmapMode = baseOnly || description.minFilter == 9984 || description.minFilter == 9985
      ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
  result.addressModeU = address(description.wrapS);
  result.addressModeV = address(description.wrapT);
  result.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  result.minLod = 0;
  result.maxLod = baseOnly ? 0.25f : VK_LOD_CLAMP_NONE;
  result.maxAnisotropy = 1;
  // Float transparent black is compatible with normalized/float material
  // textures and non-identity image-view swizzles without extra features.
  result.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  return result;
}

uint32_t MaterialSamplerDescriptorCapacity(uint32_t perStageLimit, uint32_t pipelineLayoutLimit,
                                           uint32_t reserved) {
  const auto limit = std::min(perStageLimit, pipelineLayoutLimit);
  return limit <= reserved ? 0 : std::min(kMaterialSamplerKeyCount, limit - reserved);
}

uint32_t MaterialSamplerSlotPlan::slot(MaterialSamplerKey key) const {
  if (key.value >= slots.size()) throw std::invalid_argument("Invalid material sampler key");
  if (slots[key.value] == kInvalidMaterialSamplerSlot) throw std::out_of_range("Material sampler key is not active");
  return slots[key.value];
}

MaterialSamplerSlotPlan PlanMaterialSamplerSlots(std::span<const MaterialSamplerKey> keys, uint32_t capacity) {
  MaterialSamplerSlotPlan result;
  // The finite policy domain bounds actual key storage as well as logical size;
  // descriptor preparation can charge it before the first allocation.
  result.keys.reserve(std::min(capacity,kMaterialSamplerKeyCount));
  const auto append = [&](MaterialSamplerKey key) {
    if (key.value >= kMaterialSamplerKeyCount) throw std::invalid_argument("Invalid material sampler key");
    if (result.slots[key.value] != kInvalidMaterialSamplerSlot) return;
    if (result.keys.size() >= capacity)
      throw std::runtime_error("Scene requires more distinct material sampler policies than the device pipeline supports");
    result.slots[key.value] = static_cast<uint32_t>(result.keys.size());
    result.keys.push_back(key);
  };
  append({0});
  for (auto key : keys) append(key);
  return result;
}

}  // namespace container::material
