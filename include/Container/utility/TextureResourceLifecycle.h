#pragma once

#include <Container/utility/ResourceLifecycle.h>
#include <Container/utility/TextureManager.h>
#include <tiny_gltf.h>

#include <cstdint>
#include <memory>
#include <span>

namespace container::scene {

struct TextureResourceSourceTables {
  std::span<const tinygltf::Image> images;
  std::span<const tinygltf::Texture> textures;
  std::span<const tinygltf::Sampler> samplers;
  std::span<const tinygltf::BufferView> bufferViews;
  std::span<const tinygltf::Buffer> buffers;
  // Explicit source Texture -> RuntimeTexture roles. Both empty means that
  // this caller has no role table; otherwise both cover the exact texture table.
  std::span<const uint32_t> colorResources,dataResources;
  bool rolesKnown{false};
  // Empty means all active. Otherwise each mask covers its exact source table
  // and contains only 0 (retired) or 1 (active); records keep stable row IDs.
  std::span<const uint8_t> imageActive,textureActive,samplerActive;
};
struct TextureResourceLifecycleView {
  TextureResourceSourceTables archive,evaluated;
  const container::material::TextureManager& runtime;
  std::span<const int32_t> archiveToEvaluated;
  std::span<const std::shared_ptr<const uint8_t>> archiveTextureIdentities;
};
struct TextureResourceSelection {
  container::util::ResourceDomainKind domain{};
  container::util::ResourceKind kind{};
  uint32_t row{};
  bool operator==(const TextureResourceSelection&) const noexcept=default;
};
struct TextureResourceLifecycleLimits {
  uint64_t maxRows{1'000'000},maxOwnedBytes{512ull*1024*1024},maxWork{512ull*1024*1024};
  container::util::ResourceRetirementPolicy retirementPolicy{container::util::ResourceRetirementPolicy::BlockPreparations};
  uint64_t maxLiveOwners{4'000'000},maxLiveReferences{4'000'000};
};

class TextureResourceLifecycleGeneration;
class TextureResourceReferenceOwner;
class TextureResourceLifecycleCommand;
class TextureResourceRetirementCommand;
class PreparedTextureResourceLifecycle;
class TextureResourceLiveOwner;
using TextureResourceLifecycleGenerationPtr=std::shared_ptr<const TextureResourceLifecycleGeneration>;
using TextureResourceReferenceOwnerPtr=std::shared_ptr<const TextureResourceReferenceOwner>;
using TextureResourceLiveOwnerPtr=std::shared_ptr<const TextureResourceLiveOwner>;
enum class TextureResourceLiveOwnerKind : uint8_t {
  SourceTexture,SourceRecord,ColorRole,DataRole,Material,PrivateCache,Rest,Provider
};
struct TextureResourceLiveOwnerKey {
  container::util::ResourceHandle owner;
  TextureResourceLiveOwnerKind kind{};
  uint64_t version{};
  bool operator==(const TextureResourceLiveOwnerKey&) const noexcept=default;
};
// One independently replaceable LIVE edge set. This never wraps a History
// owner or a full source generation. Exact outgoing controls stay active until
// the same composite catalog operation publishes their successors.
class TextureResourceLiveOwner {
 public:
  [[nodiscard]] TextureResourceLiveOwnerKey key() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool pending() const noexcept;
  [[nodiscard]] bool matches(const TextureResourceLifecycleGenerationPtr&) const noexcept;
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] std::span<const container::util::ResourceHandle> resources() const noexcept;
 private:
  struct State;
  std::shared_ptr<State> state_;
  friend struct TextureResourceLifecycleAccess;
};

// Numeric table addresses map to separately issued logical incarnations. A
// selected owner retains selected records/bytes, never its full parent table.
class TextureResourceReferenceOwner {
 public:
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] bool valid() const noexcept; // Retired identities remain retained.
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool resourceDependenciesActive() const noexcept;
  // Exact catalog/incarnation membership; compatible source metadata versions
  // preserve identity and do not rewind unrelated metadata during history.
  [[nodiscard]] bool resourceDependenciesMatch(const TextureResourceLifecycleGenerationPtr&) const noexcept;
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::span<const container::util::ResourceHandle> resources() const noexcept;
 private:
  struct State;
  std::shared_ptr<State> state_;
  friend struct TextureResourceLifecycleAccess;
};

class TextureResourceLifecycleGeneration {
 public:
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] uint64_t revision() const noexcept;
  // Independent opaque endpoint controls block future arbitrary retirement
  // audits even when their known selected resource set is empty.
  [[nodiscard]] uint64_t opaqueReferenceOwnerCount() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool matches(const TextureResourceLifecycleView&) const noexcept;
  [[nodiscard]] bool liveDependenciesValid() const noexcept;
  [[nodiscard]] bool sharesAuthority(const TextureResourceLifecycleGenerationPtr&) const noexcept;
  [[nodiscard]] std::span<const TextureResourceLiveOwnerPtr> sourceLiveOwners() const noexcept;
  [[nodiscard]] container::util::ResourceHandle resource(TextureResourceSelection) const noexcept;
  // Prospective generations report their intended phase; published generations
  // also require the exact catalog handle to remain active.
  [[nodiscard]] bool active(TextureResourceSelection) const noexcept;
  [[nodiscard]] size_t rowCount(container::util::ResourceDomainKind,container::util::ResourceKind) const noexcept;
  [[nodiscard]] container::util::ResourceCatalogSnapshot inventory() const;
  // Logical registrations and retained references are prerequisites only.
  // Arbitrary Delete cannot rely on this generation until ALL live dependency
  // producers and bounded external reference authorities are integrated.
  [[nodiscard]] bool dependencyAuthorityComplete() const noexcept { return false; }
 private:
  struct State;
  std::shared_ptr<State> state_;
  friend struct TextureResourceLifecycleAccess;
};

class TextureResourceLifecycleCommand {
 public:
  TextureResourceLifecycleCommand() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] TextureResourceReferenceOwnerPtr references() const noexcept;
 private:
  struct State;
  std::shared_ptr<State> state_;
  friend struct TextureResourceLifecycleAccess;
};

// An arbitrary selected-row phase receipt, separate from append/suffix history.
// Only this original receipt can restore its exact retired incarnations.
class TextureResourceRetirementCommand {
 public:
  TextureResourceRetirementCommand() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] TextureResourceReferenceOwnerPtr references() const noexcept;
  [[nodiscard]] std::span<const TextureResourceSelection> selections() const noexcept;
 private:
  struct State;
  std::shared_ptr<State> state_;
  friend struct TextureResourceLifecycleAccess;
};

class PreparedTextureResourceLifecycle {
 public:
  PreparedTextureResourceLifecycle() noexcept;
  ~PreparedTextureResourceLifecycle();
  PreparedTextureResourceLifecycle(PreparedTextureResourceLifecycle&&) noexcept;
  PreparedTextureResourceLifecycle& operator=(PreparedTextureResourceLifecycle&&) noexcept;
  PreparedTextureResourceLifecycle(const PreparedTextureResourceLifecycle&)=delete;
  PreparedTextureResourceLifecycle& operator=(const PreparedTextureResourceLifecycle&)=delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] const TextureResourceLifecycleGenerationPtr& next() const noexcept;
  [[nodiscard]] TextureResourceLifecycleCommand command() const noexcept;
  [[nodiscard]] TextureResourceRetirementCommand retirementCommand() const noexcept;
  [[nodiscard]] bool sealed() const noexcept;
  [[nodiscard]] bool liveOwnerWillBeActive(const TextureResourceLiveOwnerPtr&) const noexcept;
  [[nodiscard]] TextureResourceLiveOwnerPtr stageLiveOwner(TextureResourceLiveOwnerKey,
      std::span<const TextureResourceSelection>,const TextureResourceLiveOwnerPtr& outgoing={},
      uint64_t maxCpuBytes=64ull*1024*1024,bool opaque=false);
  // version zero requests a helper-issued monotonic version. Exact old control
  // identity and its captured pin, rather than a numeric label, authorize release.
  void releaseLiveOwners(std::span<const TextureResourceLiveOwnerPtr>);
  // Seals ONE registration/activation/retirement/dependency mutation. A blocked
  // retirement is deliberately deferred until the paired material plan supplies
  // its exact outgoing controls. No public publication readiness before seal.
  void seal();
  [[nodiscard]] TextureResourceReferenceOwnerPtr select(std::span<const TextureResourceSelection>,
      container::util::ResourcePinClass,uint64_t maxCpuBytes=64ull*1024*1024,bool opaque=false) const;
  [[nodiscard]] TextureResourceReferenceOwnerPtr selectAll(container::util::ResourcePinClass,
      uint64_t maxCpuBytes=64ull*1024*1024) const;
 private:
  struct State;
  std::unique_ptr<State> state_;
  friend struct TextureResourceLifecycleAccess;
};

// Empty before starts a fresh independent generation (initial load/full merge).
// Existing generation updates preserve row identities; appends issue monotonic
// internal slots even when an undone suffix reuses a numeric table address.
[[nodiscard]] PreparedTextureResourceLifecycle PrepareTextureResourceLifecycle(
    const TextureResourceLifecycleGenerationPtr& before,const TextureResourceLifecycleView&,
    const TextureResourceLifecycleLimits& limits={});
// Trusted source-cache maintenance only, after an externally authorized source
// mutation. Preserves existing identities for metadata-only record versions,
// issues fresh monotonic identities for appends, and retires removed source
// suffix identities while invalidating old preparations. Typed bindings/bytes
// cannot be replaced, and no unused-resource or Undo authority is issued.
[[nodiscard]] PreparedTextureResourceLifecycle ReconcileTextureResourceLifecycle(
    const TextureResourceLifecycleGenerationPtr& before,const TextureResourceLifecycleView& currentView,
    const TextureResourceLifecycleLimits& limits={});
[[nodiscard]] PreparedTextureResourceLifecycle PrepareTextureResourceLifecycleReplay(
    const TextureResourceLifecycleGenerationPtr& before,const TextureResourceLifecycleView&,
    const TextureResourceLifecycleCommand&,bool forward,const TextureResourceLifecycleLimits& limits={});
// NEXT tables retain every source record/address. Only selected source phases
// and their own cleared/restored roles/correspondence may change. Selection is
// explicit A/E Image/Texture/Sampler or RuntimeTexture; no implicit phase
// coupling. Selected runtime rows may clear only their exact source role cells,
// keeping those source owners/other roles/correspondence active and unchanged.
// This supplies phase authority, not the still-incomplete unused-resource proof.
[[nodiscard]] PreparedTextureResourceLifecycle PrepareTextureResourceRetirement(
    const TextureResourceLifecycleGenerationPtr& before,const TextureResourceLifecycleView& nextView,
    std::span<const TextureResourceSelection>,const TextureResourceLifecycleLimits& limits={});
[[nodiscard]] PreparedTextureResourceLifecycle PrepareTextureResourceRetirementReplay(
    const TextureResourceLifecycleGenerationPtr& before,const TextureResourceLifecycleView& nextView,
    const TextureResourceRetirementCommand&,container::util::ResourceLifecyclePhase target,
    const TextureResourceLifecycleLimits& limits={});
[[nodiscard]] bool CanExchangeTextureResourceLifecycle(const PreparedTextureResourceLifecycle&,
    const TextureResourceLifecycleGenerationPtr& current,const TextureResourceLifecycleView&) noexcept;
[[nodiscard]] bool CanPrepareTextureResourceLifecycle(const PreparedTextureResourceLifecycle&,
    const TextureResourceLifecycleGenerationPtr& current,const TextureResourceLifecycleView&) noexcept;
// The view arguments above/below describe the immutable NEXT tables. The scene
// owner separately rechecks its current source/reference witnesses before the
// outer publication; this helper proves exact next payloads and phase authority.
// Caller retains old/new payload owners in the consumed token until after the
// surrounding publication. No I/O, payload copy or last-image release occurs.
[[nodiscard]] bool ExchangeTextureResourceLifecycle(PreparedTextureResourceLifecycle&,
    TextureResourceLifecycleGenerationPtr& current,const TextureResourceLifecycleView&) noexcept;
[[nodiscard]] TextureResourceReferenceOwnerPtr RetainTextureResourceReferences(
    const TextureResourceLifecycleGenerationPtr&,std::span<const TextureResourceSelection>,
    container::util::ResourcePinClass,uint64_t maxCpuBytes=64ull*1024*1024,bool opaque=false);
[[nodiscard]] TextureResourceReferenceOwnerPtr RetainAllTextureResourceReferences(
    const TextureResourceLifecycleGenerationPtr&,container::util::ResourcePinClass,
    uint64_t maxCpuBytes=64ull*1024*1024);
[[nodiscard]] TextureResourceReferenceOwnerPtr CombineTextureResourceReferences(
    std::span<const TextureResourceReferenceOwnerPtr>,uint64_t maxCpuBytes=64ull*1024*1024);
// Retains an independently staged material pin alongside selected resource
// records. Does not wrap/retain the original owner: only selected rows and pins
// are shared into the returned immutable holder.
[[nodiscard]] TextureResourceReferenceOwnerPtr AddTextureResourceReferencePin(
    const TextureResourceReferenceOwnerPtr&,container::util::ResourcePin&&,
    uint64_t maxCpuBytes=64ull*1024*1024);

} // namespace container::scene
