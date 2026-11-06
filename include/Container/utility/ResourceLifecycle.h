#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace container::util {

enum class ResourceKind : uint8_t { Material, Image, Texture, Sampler, RuntimeTexture, Count };
enum class ResourceDomainKind : uint8_t { Archive, Evaluated, Runtime, Count };
enum class ResourcePinClass : uint8_t { LiveDependency, History, Preparation, GpuReader, Count };
enum class ResourceLifecyclePhase : uint8_t { Active, Retired };
enum class ResourceRetirementPolicy : uint8_t { BlockPreparations, InvalidatePreparations };

namespace resource_lifecycle_detail {
struct Domain;
struct Handle;
struct Catalog;
struct Pin;
struct Command;
struct Transition;
struct Registration;
struct Snapshot;
}

// Logical owned bytes are bounded, including retained snapshots and temporary
// normalization storage. Allocator bookkeeping/capacity padding is excluded.
// maxWork bounds preparation/inventory work; no-throw release/exchange visit
// only their bounded owned rows/registries. Other limits bound aggregate owners.
struct ResourceLifecycleLimits {
  uint64_t maxDomains{64}, maxSlots{1'000'000}, maxPinSets{4096};
  uint64_t maxPinnedReferences{4'000'000}, maxCommands{4096}, maxCommandReferences{1'000'000};
  uint64_t maxSnapshots{64}, maxSnapshotRows{2'000'000};
  uint64_t maxRegistrations{4096}, maxRegistrationReferences{1'000'000};
  uint64_t maxOwnedBytes{512ull * 1024 * 1024}, maxWork{64'000'000}, maxReasonBytes{1024};
};

// Domains are owner-issued namespaces. Two archive generations may have the
// same numeric slots while remaining independent. Public numbers are labels,
// never authority that a caller can use to construct a handle.
class ResourceLifecycleDomain {
 public:
  ResourceLifecycleDomain() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] ResourceDomainKind kind() const noexcept;
  [[nodiscard]] uint64_t identity() const noexcept;
  bool operator==(const ResourceLifecycleDomain&) const noexcept = default;
 private:
  friend class ResourceLifecycleCatalog;
  friend class ResourceHandle;
  friend struct resource_lifecycle_detail::Catalog;
  std::shared_ptr<const resource_lifecycle_detail::Domain> state_;
};

// A weak intent, not a pin. Keeping a handle or an unretained inventory alive
// does not prevent reuse. Every reuse receives a new immutable incarnation.
class ResourceHandle {
 public:
  ResourceHandle() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] ResourceKind kind() const noexcept;
  [[nodiscard]] ResourceLifecycleDomain domain() const noexcept;
  [[nodiscard]] uint32_t slot() const noexcept;
  [[nodiscard]] uint64_t incarnation() const noexcept;
  bool operator==(const ResourceHandle&) const noexcept = default;
 private:
  friend class ResourceLifecycleCatalog;
  friend struct resource_lifecycle_detail::Catalog;
  std::shared_ptr<const resource_lifecycle_detail::Handle> state_;
};

class ResourcePin {
 public:
  ResourcePin() noexcept;
  ~ResourcePin();
  ResourcePin(ResourcePin&&) noexcept;
  ResourcePin& operator=(ResourcePin&&) noexcept;
  ResourcePin(const ResourcePin&) = delete;
  ResourcePin& operator=(const ResourcePin&) = delete;
  // True for valid active OR transaction-pending ownership. A pending pin
  // reserves storage only; it is absent from every live inventory/count.
  explicit operator bool() const noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool resourcesActive() const noexcept;
  [[nodiscard]] bool pending() const noexcept;
  [[nodiscard]] ResourcePinClass pinClass() const noexcept;
  [[nodiscard]] std::span<const ResourceHandle> resources() const noexcept;
  void reset() noexcept;
 private:
  friend class ResourceLifecycleCatalog;
  friend struct resource_lifecycle_detail::Catalog;
  friend struct resource_lifecycle_detail::Snapshot;
  std::unique_ptr<resource_lifecycle_detail::Pin> state_;
};

// Retirement authority is separately issued and copyable for history. It owns
// the selected identities, not their payloads. Dropping every copy releases
// that authority; an immutable ResourceHandle cannot authorize resurrection.
class ResourceRetirementCommand {
 public:
  ResourceRetirementCommand() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] ResourceLifecyclePhase phase() const noexcept;
  [[nodiscard]] std::span<const ResourceHandle> resources() const noexcept;
 private:
  friend class ResourceLifecycleCatalog;
  std::shared_ptr<resource_lifecycle_detail::Command> state_;
};

// Original registration ownership, distinct from ordinary Delete authority.
// Only the exact complete issued set can be suspended/restored through it.
// A successful owned roundtrip preserves the previously active endpoint stamps
// for nested Delete history, while catalog and physical phase revisions advance.
class ResourceAppendCommand {
 public:
  ResourceAppendCommand() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] ResourceLifecyclePhase phase() const noexcept;
  [[nodiscard]] std::span<const ResourceHandle> resources() const noexcept;
 private:
  friend class ResourceLifecycleCatalog;
  std::shared_ptr<resource_lifecycle_detail::Command> state_;
};

class PreparedResourceTransition {
 public:
  PreparedResourceTransition() noexcept;
  ~PreparedResourceTransition();
  PreparedResourceTransition(PreparedResourceTransition&&) noexcept;
  PreparedResourceTransition& operator=(PreparedResourceTransition&&) noexcept;
  PreparedResourceTransition(const PreparedResourceTransition&) = delete;
  PreparedResourceTransition& operator=(const PreparedResourceTransition&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] ResourceRetirementCommand command() const noexcept;
  [[nodiscard]] uint64_t expectedRevision() const noexcept;
 private:
  friend class ResourceLifecycleCatalog;
  std::unique_ptr<resource_lifecycle_detail::Transition> state_;
};

struct ResourceRegistrationRequest {
  ResourceLifecycleDomain domain;
  ResourceKind kind{};
  uint32_t slot{};
};
// Prospective handles are not live before exchange. Multiple kinds/domains can
// publish atomically. Preparation may reserve invisible table capacity, but
// neither rows nor catalog revision change on failure/abandonment.
class PreparedResourceRegistration {
 public:
  PreparedResourceRegistration() noexcept;
  ~PreparedResourceRegistration();
  PreparedResourceRegistration(PreparedResourceRegistration&&) noexcept;
  PreparedResourceRegistration& operator=(PreparedResourceRegistration&&) noexcept;
  PreparedResourceRegistration(const PreparedResourceRegistration&) = delete;
  PreparedResourceRegistration& operator=(const PreparedResourceRegistration&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] std::span<const ResourceHandle> resources() const noexcept;
  [[nodiscard]] uint64_t expectedRevision() const noexcept;
 private:
  friend class ResourceLifecycleCatalog;
  std::unique_ptr<resource_lifecycle_detail::Registration> state_;
};

struct ResourceInventoryRow {
  ResourceHandle resource;
  bool active{}, retired{};
  std::array<uint64_t, std::size_t(ResourcePinClass::Count)> pins{};
  uint64_t commandOwners{};
};
struct ResourceInventoryReason {
  ResourcePinClass pinClass{};
  std::string reason;
  // Commands are reported separately from actual History-class pins.
  bool retirementCommand{};
  uint64_t ownerSerial{};
  std::vector<ResourceHandle> resources;
};

// Immutable copied diagnostics. inventory() is weak; retainSnapshot() also
// keeps one RAII pin over its selected exact incarnations. Snapshot copies
// share that one pin. Historical bitmaps never authorize later replay.
class ResourceCatalogSnapshot {
 public:
  ResourceCatalogSnapshot() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t revision() const noexcept;
  [[nodiscard]] std::span<const ResourceInventoryRow> rows() const noexcept;
  [[nodiscard]] std::span<const ResourceInventoryReason> reasons() const noexcept;
 private:
  friend class ResourceLifecycleCatalog;
  std::shared_ptr<const resource_lifecycle_detail::Snapshot> state_;
};

// Single-owner/thread bookkeeping only. No resource payload or Vulkan object
// is copied, resolved, freed or synchronized here. A pin inventory is NOT a
// source-reference proof: the scene owner must audit every source/provider,
// publish matching descriptor generations and retire GPU readers separately.
class ResourceLifecycleCatalog {
 public:
  explicit ResourceLifecycleCatalog(const ResourceLifecycleLimits& = {});
  ~ResourceLifecycleCatalog();
  ResourceLifecycleCatalog(const ResourceLifecycleCatalog&) = delete;
  ResourceLifecycleCatalog& operator=(const ResourceLifecycleCatalog&) = delete;
  ResourceLifecycleCatalog(ResourceLifecycleCatalog&&) = delete;
  ResourceLifecycleCatalog& operator=(ResourceLifecycleCatalog&&) = delete;
  [[nodiscard]] uint64_t revision() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] ResourceLifecycleDomain createDomain(ResourceDomainKind);
  // Initialization convenience. Transactional producers use prepareIssue.
  // Append at the next dense slot, or reuse an existing reclaimable retired
  // slot. Holes, active cells and cells retained by any pin/command reject.
  [[nodiscard]] ResourceHandle issue(const ResourceLifecycleDomain&, ResourceKind, uint32_t slot);
  [[nodiscard]] PreparedResourceRegistration prepareIssue(std::span<const ResourceRegistrationRequest>);
  // Fallible and once per exact unpublished registration. Abandoning/closing
  // the registration cancels escaped pending authority. Cannot be minted from
  // weak handles, an already published registration or another catalog.
  [[nodiscard]] ResourceAppendCommand retainAppendAuthority(PreparedResourceRegistration&,std::string_view reason);
  [[nodiscard]] bool canExchange(const PreparedResourceRegistration&) const noexcept;
  [[nodiscard]] bool exchange(PreparedResourceRegistration&) noexcept;
  [[nodiscard]] bool contains(const ResourceHandle&) const noexcept;
  [[nodiscard]] bool active(const ResourceHandle&) const noexcept;
  [[nodiscard]] bool reclaimable(const ResourceHandle&) const noexcept;
  [[nodiscard]] ResourcePin pin(std::span<const ResourceHandle>, ResourcePinClass, std::string_view reason);
  // Stages one sorted-unique pin over exact prospective handles from this
  // registration and/or existing ACTIVE handles. Allocates/reserves everything
  // now; registration exchange publishes all rows and staged pin classes at
  // once. Destruction before commit detaches the pin. Abandoning registration
  // cancels escaped pending tokens; no counters/reasons become live. Pending
  // sets/references/bytes consume the same aggregate pin limits as active pins.
  [[nodiscard]] ResourcePin stagePin(PreparedResourceRegistration&, std::span<const ResourceHandle>,
      ResourcePinClass, std::string_view reason);
  // Stages ownership for an exact phase/dependency transition. Handles must be
  // selected by that transition or currently ACTIVE. LiveDependency pins over
  // selected rows require an Active target. No pending pin becomes live until
  // exchange; abandoning/closing its transition cancels escaped tokens.
  [[nodiscard]] ResourcePin stagePin(PreparedResourceTransition&, std::span<const ResourceHandle>,
      ResourcePinClass, std::string_view reason);
  // Appends exact outgoing LiveDependency owner proofs to the same mutation.
  // Rows/pins may be staged before or after this call. Duplicates, foreign or
  // reset/replaced owners reject; allocation/validation failure preserves all
  // previously attached proofs, live owners and pending pins. Empty input is a
  // checked no-op. Release occurs only at the one successful exchange.
  void stageDependencyRelease(PreparedResourceRegistration&, std::span<const ResourcePin* const>);
  // The prospective combined proofs must still satisfy every retirement
  // residual-count and Preparation-policy check. This cannot create an initially
  // blocked retirement or grant a generic dependency exclusion.
  void stageDependencyRelease(PreparedResourceTransition&, std::span<const ResourcePin* const>);
  [[nodiscard]] ResourceCatalogSnapshot inventory() const;
  [[nodiscard]] ResourceCatalogSnapshot retainSnapshot(ResourcePinClass, std::string_view reason);
  // LiveDependency always blocks retirement. GpuReader/History only retain
  // identity/payload ownership. Preparation blocks by default; explicitly
  // invalidating it advances the catalog revision so old preparations reject.
  [[nodiscard]] PreparedResourceTransition prepareRetirement(std::span<const ResourceHandle>,
      std::string_view reason, ResourceRetirementPolicy = ResourceRetirementPolicy::BlockPreparations);
  // Exact active LiveDependency owners may be released together with a phase
  // change. Each owner is captured by immutable serial and exact rows, then
  // rechecked at exchange; residual external live dependencies still block.
  // Owners can move after preparation, but reset/replacement invalidates it.
  // Successful exchange deactivates the supplied tokens, without destroying
  // their retained storage. Failed/abandoned preparations release nothing.
  [[nodiscard]] PreparedResourceTransition prepareRetirement(std::span<const ResourceHandle>,
      std::span<const ResourcePin* const> outgoingLivePins, std::string_view reason,
      ResourceRetirementPolicy = ResourceRetirementPolicy::BlockPreparations);
  [[nodiscard]] PreparedResourceTransition prepareChange(const ResourceRetirementCommand&,
      ResourceLifecyclePhase, ResourceRetirementPolicy = ResourceRetirementPolicy::BlockPreparations);
  [[nodiscard]] PreparedResourceTransition prepareChange(const ResourceRetirementCommand&,
      ResourceLifecyclePhase, std::span<const ResourcePin* const> outgoingLivePins,
      ResourceRetirementPolicy = ResourceRetirementPolicy::BlockPreparations);
  // Replaces outgoing ownership without changing resource phases or issuing
  // retirement authority. Empty outgoing ownership permits a first pin set.
  // Future pins are staged above; exact old owners remain live until exchange.
  [[nodiscard]] PreparedResourceTransition prepareDependencyChange(
      std::span<const ResourcePin* const> outgoingLivePins, std::string_view reason);
  // Suspends/resumes only the original append's complete exact issued set.
  // Suspension captures today's active endpoints; its own resume restores
  // those endpoint stamps, never an older birth stamp. Ordinary other receipt
  // cycles still invalidate prior Delete commands. Residual live dependencies,
  // Preparation policy, pending pin staging and one noalloc exchange apply.
  [[nodiscard]] PreparedResourceTransition prepareAppendChange(const ResourceAppendCommand&,
      ResourceLifecyclePhase,std::span<const ResourcePin* const> outgoingLivePins={},
      ResourceRetirementPolicy = ResourceRetirementPolicy::BlockPreparations);
  [[nodiscard]] bool canExchange(const PreparedResourceTransition&) const noexcept;
  // Allocation-free, no-throw and one-shot. False leaves every phase unchanged.
  [[nodiscard]] bool exchange(PreparedResourceTransition&) noexcept;
 private:
  std::shared_ptr<resource_lifecycle_detail::Catalog> state_;
};

} // namespace container::util
