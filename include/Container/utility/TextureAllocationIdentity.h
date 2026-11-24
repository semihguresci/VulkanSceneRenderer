#pragma once

#include "Container/scene/SceneAssetIdentity.h"

#include <cstdint>
#include <memory>

namespace container::gpu {
class AllocationManager;

// Non-owning capability minted for one allocator-owned image incarnation.
class TextureAllocationIdentity {
 public:
  TextureAllocationIdentity() noexcept = default;
  explicit operator bool() const noexcept { return bool(assetId_) && !owner_.expired(); }
  [[nodiscard]] container::scene::SceneAssetId assetId() const noexcept { return assetId_; }
  [[nodiscard]] uint64_t incarnation() const noexcept { return assetId().value(); }
  bool operator==(const TextureAllocationIdentity& other) const noexcept {
    return assetId_==other.assetId_ && !owner_.owner_before(other.owner_) && !other.owner_.owner_before(owner_);
  }
 private:
  friend class AllocationManager;
  std::weak_ptr<const void> owner_;
  container::scene::SceneAssetId assetId_{};
};
} // namespace container::gpu
