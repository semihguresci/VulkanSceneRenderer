#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Container/utility/TextureResource.h"

namespace container::material {

class RuntimeTextureRetirementCommand;
class PreparedRuntimeTextureRetirement;
class RuntimeTextureAppendCommand;
class PreparedRuntimeTextureAppend;
struct RuntimeTextureRetirementAccess;
enum class RuntimeTextureRetirementTarget : uint8_t { Active,Retired };
struct RuntimeTextureRetirementLimits {
  uint64_t maxRows{1'000'000},maxAliases{2'000'000};
  uint64_t maxOwnedBytes{512ull*1024*1024},maxWork{64'000'000};
};

/// Transparent hash for heterogeneous lookup on std::unordered_map<std::string, ...>
struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
};

class TextureManager {
 public:
  [[nodiscard]] static constexpr size_t rowStorageBytes() noexcept;
  // Exchanging an asset generation must not invoke an unordered_map move
  // constructor: that constructor may allocate on the MSVC STL. Member swaps
  // preserve the default allocators and transfer the existing buckets directly.
  friend void swap(TextureManager& a, TextureManager& b) noexcept {
    static_assert(noexcept(a.textures_.swap(b.textures_)));
    static_assert(noexcept(a.textureNameToIndex_.swap(b.textureNameToIndex_)));
    static_assert(noexcept(a.imageContentToIndex_.swap(b.imageContentToIndex_)));
    a.textures_.swap(b.textures_);
    a.textureNameToIndex_.swap(b.textureNameToIndex_);
    a.imageContentToIndex_.swap(b.imageContentToIndex_);
    a.active_.swap(b.active_);
    a.identities_.swap(b.identities_);a.ownerIdentity_.swap(b.ownerIdentity_);
    a.tailVacancy_.swap(b.tailVacancy_);
    std::swap(a.activeCount_,b.activeCount_);
    std::swap(a.revision_,b.revision_);
  }
  uint32_t registerTexture(const TextureResource& resource);
  [[nodiscard]] std::optional<uint32_t> findTextureIndex(
      std::string_view name) const;
  [[nodiscard]] const TextureResource* getTexture(uint32_t index) const;
  [[nodiscard]] const TextureResource* findTextureImage(std::string_view contentKey) const;
  // Retires an arbitrary slot without reindexing or reusing it. Every cache
  // alias targeting this slot is erased; no stale key is retargeted to a sibling.
  // The row's payload/capability is released. Raw rows do not own GPU images.
  [[nodiscard]] bool retireTexture(uint32_t index) noexcept;
  [[nodiscard]] bool isTextureActive(uint32_t index) const noexcept;
  [[nodiscard]] const TextureResource& getTextureOrFallback(
      uint32_t index,const TextureResource& fallback) const noexcept;
  // textureCount is the stable slot range, including empty retired slots.
  [[nodiscard]] size_t textureCount() const noexcept { return textures_.size(); }
  [[nodiscard]] size_t activeTextureCount() const noexcept { return activeCount_; }
  // Changes only after a new row or successful retirement is published.
  // Copies and exact detached prefixes retain their snapshot revision.
  [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
  // Preserve logical indices, inactive rows and the original surviving cache
  // aliases exactly. Re-registering a prefix can deduplicate rows or resurrect
  // a cache alias that retirement deliberately erased.
  [[nodiscard]] TextureManager copyPrefix(size_t count) const;

 private:
  struct Identity {
    std::shared_ptr<const uint8_t> incarnation,retiredBy;
    uint64_t phaseVersion{};
  };
  std::vector<TextureResource> textures_{};
  std::vector<uint8_t> active_{};
  std::vector<Identity> identities_{};
  std::shared_ptr<const uint8_t> ownerIdentity_;
  std::shared_ptr<const uint8_t> tailVacancy_;
  size_t activeCount_{};
  uint64_t revision_{};
  std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>>
      textureNameToIndex_{};
  std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>>
      imageContentToIndex_{};
  friend struct RuntimeTextureRetirementAccess;
};
constexpr size_t TextureManager::rowStorageBytes() noexcept {
  return sizeof(TextureResource)+sizeof(uint8_t)+sizeof(Identity);
}

// Selected payload/capability and exact original cache cells only. This receipt
// does not own VkImages; the scene coordinator must retain allocation leases.
class RuntimeTextureRetirementCommand {
 public:
  RuntimeTextureRetirementCommand() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] bool retired() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] std::span<const uint32_t> slots() const noexcept;
 private:
  struct State;
  std::shared_ptr<State> state_;
  friend struct RuntimeTextureRetirementAccess;
};
class PreparedRuntimeTextureRetirement {
 public:
  PreparedRuntimeTextureRetirement() noexcept;
  ~PreparedRuntimeTextureRetirement();
  PreparedRuntimeTextureRetirement(PreparedRuntimeTextureRetirement&&) noexcept;
  PreparedRuntimeTextureRetirement& operator=(PreparedRuntimeTextureRetirement&&) noexcept;
  PreparedRuntimeTextureRetirement(const PreparedRuntimeTextureRetirement&)=delete;
  PreparedRuntimeTextureRetirement& operator=(const PreparedRuntimeTextureRetirement&)=delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] const TextureManager& textures() const;
  [[nodiscard]] RuntimeTextureRetirementCommand command() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
 private:
  struct State;
  std::unique_ptr<State> state_;
  friend struct RuntimeTextureRetirementAccess;
};
[[nodiscard]] PreparedRuntimeTextureRetirement PrepareRuntimeTextureRetirement(
    const TextureManager&,std::span<const uint32_t>,const RuntimeTextureRetirementLimits& limits={});
[[nodiscard]] PreparedRuntimeTextureRetirement PrepareRuntimeTextureRetirementReplay(
    const TextureManager&,const RuntimeTextureRetirementCommand&,RuntimeTextureRetirementTarget,
    const RuntimeTextureRetirementLimits& limits={});
[[nodiscard]] bool CanExchangeRuntimeTextureRetirement(const PreparedRuntimeTextureRetirement&,
    const TextureManager&) noexcept;
// All candidate allocation precedes this one-shot noalloc exchange. The consumed
// token retains the old table; receipt phase/version changes only on success.
[[nodiscard]] bool ExchangeRuntimeTextureRetirement(PreparedRuntimeTextureRetirement&,TextureManager&) noexcept;

// Original append-history authority. Selected rows/aliases only; an original
// cut captures their current phase versions, then restores the same identities.
class RuntimeTextureAppendCommand {
 public:
  RuntimeTextureAppendCommand() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
 private:
  struct State;
  std::shared_ptr<State> state_;
  friend struct RuntimeTextureRetirementAccess;
};
class PreparedRuntimeTextureAppend {
 public:
  PreparedRuntimeTextureAppend() noexcept;
  ~PreparedRuntimeTextureAppend();
  PreparedRuntimeTextureAppend(PreparedRuntimeTextureAppend&&) noexcept;
  PreparedRuntimeTextureAppend& operator=(PreparedRuntimeTextureAppend&&) noexcept;
  PreparedRuntimeTextureAppend(const PreparedRuntimeTextureAppend&)=delete;
  PreparedRuntimeTextureAppend& operator=(const PreparedRuntimeTextureAppend&)=delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] const TextureManager& textures() const;
  [[nodiscard]] RuntimeTextureAppendCommand command() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
 private:
  struct State;
  std::unique_ptr<State> state_;
  friend struct RuntimeTextureRetirementAccess;
};
[[nodiscard]] PreparedRuntimeTextureAppend PrepareRuntimeTextureAppend(const TextureManager& before,
    const TextureManager& appendedCandidate,const RuntimeTextureRetirementLimits& limits={});
[[nodiscard]] PreparedRuntimeTextureAppend PrepareRuntimeTextureAppendReplay(const TextureManager& current,
    const RuntimeTextureAppendCommand&,bool present,const RuntimeTextureRetirementLimits& limits={});
[[nodiscard]] bool CanExchangeRuntimeTextureAppend(const PreparedRuntimeTextureAppend&,const TextureManager&) noexcept;
[[nodiscard]] bool ExchangeRuntimeTextureAppend(PreparedRuntimeTextureAppend&,TextureManager&) noexcept;

}  // namespace container::material
