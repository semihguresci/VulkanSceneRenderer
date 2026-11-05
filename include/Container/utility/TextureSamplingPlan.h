#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "Container/utility/MaterialSamplerKey.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/TextureManager.h"

namespace container::gpu { class AllocationManager; }

namespace container::material {

// Descriptor identity is distinct from its resolved view: several retired rows
// resolve to the same private fallback without becoming the same resource.
struct TextureSamplingRowState {
  bool active{};
  container::gpu::TextureAllocationIdentity allocationIdentity;
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  VkFormat format{VK_FORMAT_UNDEFINED};
  uint32_t samplerKey{},width{},height{},mipLevels{};
  bool operator==(const TextureSamplingRowState&) const noexcept = default;
};

struct TextureSamplingPlan {
  uint64_t textureRevision{};
  MaterialSamplerSlotPlan samplerSlots;
  std::vector<TextureSamplingRowState> rows;
  TextureSamplingRowState fallback;
  // N stable slots have N descriptors in the same order. An empty catalog has
  // one private fallback descriptor and metadata row, but still zero source IDs.
  std::vector<VkDescriptorImageInfo> images;
  std::vector<container::gpu::GpuTextureMetadata> metadata;
};
static_assert(std::is_nothrow_move_constructible_v<TextureSamplingPlan>);
static_assert(std::is_nothrow_move_assignable_v<TextureSamplingPlan>);
static_assert(std::is_nothrow_swappable_v<TextureSamplingPlan>);

// Conservative transient storage charge, including the policy key scratch.
// Throws before allocation for more than one million stable slots.
[[nodiscard]] uint64_t TextureSamplingPlanStorageBytes(size_t slots);

// Policy-only preflight deliberately accepts not-yet-uploaded active resources
// in a merge dry run. It validates keys and finite table/work bounds; it does
// not authorize those resources for descriptor publication.
[[nodiscard]] MaterialSamplerSlotPlan PlanTextureSamplerSlots(
    const TextureManager&, uint32_t capacity,
    std::span<const MaterialSamplerKey> additional = {});

// Full publication preparation checks every active and fallback allocation
// capability with its actual owner. Invalid active rows never use fallback.
[[nodiscard]] TextureSamplingPlan MakeTextureSamplingPlan(
    const TextureManager&, const TextureResource& fallback,
    const container::gpu::AllocationManager&, uint32_t imageCapacity,
    uint32_t samplerCapacity, uint64_t maxCpuBytes = 64ull*1024*1024);

// Nonallocating revalidation includes table revision, active mask, allocation
// incarnations, exact view/image/policy metadata and fallback identity.
[[nodiscard]] bool TextureSamplingPlanMatches(
    const TextureSamplingPlan&, const TextureManager&, const TextureResource& fallback,
    const container::gpu::AllocationManager&) noexcept;

// Resource restaging only: an unchanged published prefix may be followed by
// successful active appends awaiting new GPU metadata. The exact revision delta
// must equal their count, excluding retirement/restoration or hidden mutations.
// This does not authorize publishing the old descriptors for the longer table.
[[nodiscard]] bool TextureSamplingPlanPrefixMatches(
    const TextureSamplingPlan&, const TextureManager&, const TextureResource& fallback,
    const container::gpu::AllocationManager&) noexcept;

} // namespace container::material
