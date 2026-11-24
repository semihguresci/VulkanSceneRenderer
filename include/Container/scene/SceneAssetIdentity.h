#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace container::scene {

// Owners issue identities for actual asset incarnations. The identity survives
// content edits and is independent of provider names, addresses and GPU handles.
class SceneAssetId {
public:
  constexpr SceneAssetId() noexcept = default;

  [[nodiscard]] static SceneAssetId mint() {
    static std::atomic<uint64_t> last{0};
    auto current = last.load(std::memory_order_relaxed);
    for (;;) {
      if (current == (std::numeric_limits<uint64_t>::max)())
        throw std::overflow_error("scene asset identities exhausted");
      if (last.compare_exchange_weak(current, current + 1,
                                     std::memory_order_relaxed))
        return SceneAssetId(current + 1);
    }
  }

  [[nodiscard]] explicit constexpr operator bool() const noexcept {
    return value_ != 0;
  }
  [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }
  constexpr bool operator==(const SceneAssetId &) const noexcept = default;

private:
  explicit constexpr SceneAssetId(uint64_t value) noexcept : value_(value) {}
  uint64_t value_{0};
};

} // namespace container::scene
