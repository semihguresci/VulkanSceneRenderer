#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Container/utility/AllocationManager.h"
#include "Container/utility/TextureManager.h"

namespace container::material {

using TextureImagePin = std::shared_ptr<const container::gpu::AllocationManager::TextureImageAllocationLease>;
class TextureImageOwnership;
using TextureImageOwnershipPtr = std::shared_ptr<const TextureImageOwnership>;

struct TextureImageCapability {
  container::gpu::TextureAllocationIdentity identity;
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  bool operator==(const TextureImageCapability&) const noexcept = default;
};
struct TextureImageOwnershipRow {
  uint32_t runtimeSlot{};
  uint32_t image{UINT32_MAX}; // Retired full-table rows own no image.
  bool operator==(const TextureImageOwnershipRow&) const noexcept = default;
};

// Immutable actual image retention. Logical source/reference authority remains
// separate. Selected sets never retain a full parent generation implicitly.
class TextureImageOwnership {
 public:
  TextureImageOwnership(const TextureImageOwnership&)=delete;
  TextureImageOwnership& operator=(const TextureImageOwnership&)=delete;
  [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] size_t slotCount() const noexcept { return slots_; }
  [[nodiscard]] size_t imageCount() const noexcept { return images_.size(); }
  [[nodiscard]] bool complete() const noexcept { return complete_; }
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] std::span<const TextureImagePin> pins() const noexcept { return pins_; }
  [[nodiscard]] std::span<const TextureImageCapability> images() const noexcept { return images_; }
  [[nodiscard]] std::span<const TextureImageOwnershipRow> rows() const noexcept { return rows_; }
 private:
  friend struct TextureImageOwnershipAccess;
  TextureImageOwnership()=default;
  const container::gpu::AllocationManager* owner_{};
  uint64_t revision_{};
  size_t slots_{};
  bool complete_{};
  std::vector<TextureImageCapability> images_;
  std::vector<TextureImagePin> pins_;
  std::vector<TextureImageOwnershipRow> rows_;
};

[[nodiscard]] uint64_t TextureImageOwnershipStorageBytes(size_t rows);
[[nodiscard]] TextureImageOwnershipPtr PrepareTextureImageOwnership(
    const TextureManager&, container::gpu::AllocationManager&,
    std::span<const TextureImageOwnershipPtr> inherited={},uint64_t maxCpuBytes=64ull*1024*1024);
[[nodiscard]] TextureImageOwnershipPtr SelectTextureImageOwnership(
    const TextureImageOwnershipPtr& complete,std::span<const uint32_t> runtimeSlots,
    const container::gpu::AllocationManager&,uint64_t maxCpuBytes=64ull*1024*1024);
[[nodiscard]] TextureImageOwnershipPtr CombineTextureImageOwnership(
    std::span<const TextureImageOwnershipPtr> selected,
    const container::gpu::AllocationManager&,uint64_t maxCpuBytes=64ull*1024*1024);
[[nodiscard]] bool TextureImageOwnershipValid(
    const TextureImageOwnershipPtr&,const container::gpu::AllocationManager&) noexcept;
[[nodiscard]] bool TextureImageOwnershipMatches(
    const TextureImageOwnershipPtr&,const TextureManager&,const container::gpu::AllocationManager&) noexcept;
[[nodiscard]] bool TextureImageOwnershipContains(
    const TextureImageOwnershipPtr&,const TextureResource&,const container::gpu::AllocationManager&) noexcept;

// Fallible preparation retains reset ownership of group-zero allocations. The
// exact complete table and eligible sorted pin list are revalidated at commit.
class TextureImageOwnershipAdoption {
 public:
  TextureImageOwnershipAdoption() noexcept=default;
  TextureImageOwnershipAdoption(TextureImageOwnershipAdoption&&) noexcept=default;
  TextureImageOwnershipAdoption& operator=(TextureImageOwnershipAdoption&&) noexcept=default;
  TextureImageOwnershipAdoption(const TextureImageOwnershipAdoption&)=delete;
  TextureImageOwnershipAdoption& operator=(const TextureImageOwnershipAdoption&)=delete;
  explicit operator bool() const noexcept { return ownership_ && !consumed_; }
  [[nodiscard]] const TextureImageOwnershipPtr& ownership() const noexcept { return ownership_; }
  [[nodiscard]] size_t pendingCount() const noexcept { return pins_.size(); }
 private:
  friend struct TextureImageOwnershipAccess;
  TextureImageOwnershipPtr ownership_;
  std::vector<TextureImagePin> pins_;
  bool consumed_{};
};
[[nodiscard]] TextureImageOwnershipAdoption PrepareTextureImageOwnershipAdoption(
    const TextureImageOwnershipPtr&,const TextureManager&,const container::gpu::AllocationManager&,
    uint64_t maxCpuBytes=64ull*1024*1024);
[[nodiscard]] bool CanAdoptTextureImageOwnership(
    const TextureImageOwnershipAdoption&,const TextureManager&,const container::gpu::AllocationManager&) noexcept;
[[nodiscard]] bool AdoptTextureImageOwnership(
    TextureImageOwnershipAdoption&,const TextureManager&,container::gpu::AllocationManager&) noexcept;

} // namespace container::material
