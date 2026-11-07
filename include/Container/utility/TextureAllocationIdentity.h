#pragma once

#include <cstdint>
#include <memory>

namespace container::gpu {
class AllocationManager;

// Non-owning capability minted for one allocator-owned image incarnation.
class TextureAllocationIdentity {
 public:
  TextureAllocationIdentity() noexcept = default;
  explicit operator bool() const noexcept { return incarnation_!=0 && !owner_.expired(); }
  [[nodiscard]] uint64_t incarnation() const noexcept { return incarnation_; }
  bool operator==(const TextureAllocationIdentity& other) const noexcept {
    return incarnation_==other.incarnation_ && !owner_.owner_before(other.owner_) && !other.owner_.owner_before(owner_);
  }
 private:
  friend class AllocationManager;
  std::weak_ptr<const void> owner_;
  uint64_t incarnation_{};
};
} // namespace container::gpu
