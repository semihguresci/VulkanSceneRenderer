#pragma once

#include <Container/utility/SceneGraph.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace container::scene {

// Borrowed identity tables, never a material value/resource ownership table.
// assetIdentity is the stable lifetime/catalog identity supplied by the scene
// owner. Source definitions map injectively to runtime IDs; runtime-only and
// implicit materials have no source definition, but keep their ACTUAL ID.
struct MaterialBindingContext {
  uint64_t assetIdentity{};
  std::span<const uint32_t> sourceMaterialToRuntime;
  uint32_t runtimeMaterialCount{}, runtimePrimitiveCount{};
};

struct MaterialBindingRequest {
  uint32_t graphNode{}, expectedRuntimeMaterial{}, replacementRuntimeMaterial{};
};

// Describes exactly ONE prospective source/runtime tail append. The outer
// transaction must separately prove and prepare that material/resource payload.
// This helper does not create it or authorize publication of its GPU resources.
struct MaterialBindingAppendTarget {
  uint32_t sourceMaterial{}, runtimeMaterial{};
  bool operator==(const MaterialBindingAppendTarget&) const = default;
};

struct MaterialBindingAssignment {
  uint32_t graphNode{}, beforeRuntimeMaterial{}, afterRuntimeMaterial{};
  int32_t beforeSourceMaterial{-1}, afterSourceMaterial{-1};
  bool operator==(const MaterialBindingAssignment&) const = default;
};

struct MaterialBindingLimits {
  uint64_t maxBindings{65536}, maxSourceMaterials{1'000'000}, maxRuntimeMaterials{1'000'000};
  // Conservative logical output storage (including retained context maps),
  // and separate temporary sorting storage. Allocator bookkeeping is excluded.
  uint64_t maxOutputBytes{64ull * 1024 * 1024};
  uint64_t maxScratchBytes{32ull * 1024 * 1024};
};

enum class MaterialBindingSide : uint8_t { Before, After };
class PreparedMaterialBindingChange;
struct MaterialBindingAuthoringAccess;

// Copyable immutable history receipt. No mutable public plan fields can be
// forged. It owns only selected binding witnesses and source/runtime identity
// maps, never a graph, material, geometry, animation or image payload.
class MaterialBindingCommand {
 public:
  MaterialBindingCommand() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool changes() const noexcept;
  [[nodiscard]] uint64_t graphIdentity() const noexcept;
  [[nodiscard]] std::span<const MaterialBindingAssignment> assignments() const noexcept;
  [[nodiscard]] MaterialBindingContext context(MaterialBindingSide) const noexcept;
  [[nodiscard]] std::optional<MaterialBindingAppendTarget> appendedMaterial() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;

 private:
  struct State;
  std::shared_ptr<const State> state_;
  friend struct MaterialBindingAuthoringAccess;
};

// Fresh one-shot proof against an immediate graph revision and context. Its
// ordered assignments describe the requested replay direction, suitable for
// staging prospective object/material/frame routing without mutating a graph.
class PreparedMaterialBindingChange {
 public:
  PreparedMaterialBindingChange() noexcept;
  ~PreparedMaterialBindingChange();
  PreparedMaterialBindingChange(PreparedMaterialBindingChange&&) noexcept;
  PreparedMaterialBindingChange& operator=(PreparedMaterialBindingChange&&) noexcept;
  PreparedMaterialBindingChange(const PreparedMaterialBindingChange&) = delete;
  PreparedMaterialBindingChange& operator=(const PreparedMaterialBindingChange&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool changes() const noexcept;
  [[nodiscard]] uint64_t graphIdentity() const noexcept;
  [[nodiscard]] uint64_t expectedGraphRevision() const noexcept;
  [[nodiscard]] uint64_t candidateGraphRevision() const noexcept;
  [[nodiscard]] MaterialBindingContext expectedContext() const noexcept;
  [[nodiscard]] MaterialBindingContext targetContext() const noexcept;
  // Owning shared receipt copy, safe after this one-shot token is moved,
  // consumed or destroyed. Prospective renderer generations retain this.
  [[nodiscard]] MaterialBindingCommand command() const noexcept;
  [[nodiscard]] MaterialBindingSide targetSide() const noexcept;
  [[nodiscard]] std::span<const MaterialBindingAssignment> assignments() const noexcept;

 private:
  struct State;
  std::unique_ptr<State> state_;
  friend struct MaterialBindingAuthoringAccess;
};

// Batches MAY include inactive/hidden bindings. UI active-selection policy is
// separate. Every row must be a real, live renderable primitive. Requests are
// sorted by node ID in the receipt, duplicates reject (including no-op rows).
// All source/runtime/count/output/scratch bounds precede payload allocations.
// Invalid inputs throw; no input is ever modified.
[[nodiscard]] MaterialBindingCommand PlanMaterialBindings(
    const SceneGraph&, MaterialBindingContext,
    std::span<const MaterialBindingRequest>,
    std::optional<MaterialBindingAppendTarget> append = {},
    const MaterialBindingLimits& limits = {});

// Initial forward preparation expects Before. History replay to Before expects
// exact After IDs/context; replay to After expects exact Before IDs/context.
// Unrelated name, local/world pose and hierarchy changes may have advanced:
// selected primitive/source/instance/mesh-owner identities must still agree.
[[nodiscard]] PreparedMaterialBindingChange PrepareMaterialBindings(
    const SceneGraph&, MaterialBindingContext, const MaterialBindingCommand&,
    const MaterialBindingLimits& limits = {});
[[nodiscard]] PreparedMaterialBindingChange RebaseMaterialBindings(
    const SceneGraph&, MaterialBindingContext, const MaterialBindingCommand&,
    MaterialBindingSide target, const MaterialBindingLimits& limits = {});

// Allocation-free semantic side proof for an outer staged renderer generation.
// Does not pin a graph revision: that generation separately pins expected and
// candidate revisions. Checks the exact source/runtime context and every node's
// primitive/source/owner identity and material ID on the requested side.
[[nodiscard]] bool MatchesMaterialBindings(const SceneGraph&,
    const MaterialBindingCommand&, MaterialBindingContext, MaterialBindingSide) noexcept;

// No allocation, callbacks, cache refresh, world evaluation or ECS rebuild.
// The final predicate also rechecks raw mutable-node witnesses, not only the
// revision. Successful publication consumes even a no-op proof; a changed
// batch increments graph.revision exactly ONCE and never hierarchyRevision.
// Rejects stale/foreign/consumed/exhausted proofs without any partial writes.
//
// With a prospective append, publish bindings while currentContext is still
// Before, then publish the separately prepared material append in the SAME
// externally synchronized no-fail phase. Undo publishes old bindings before
// detaching that owned material tail. This token alone is not a renderer or
// resource transaction and must never expose the intermediate state to readers.
[[nodiscard]] bool CanPublishMaterialBindings(const SceneGraph&,
    const PreparedMaterialBindingChange&, MaterialBindingContext) noexcept;
bool PublishMaterialBindings(SceneGraph&, PreparedMaterialBindingChange&,
    MaterialBindingContext) noexcept;

} // namespace container::scene
