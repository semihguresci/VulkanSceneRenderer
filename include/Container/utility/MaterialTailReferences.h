#pragma once

#include <Container/geometry/GltfMaterialVariants.h>
#include <Container/utility/MaterialBindingAuthoring.h>

#include <cstdint>
#include <memory>
#include <span>

namespace container::scene {

struct MaterialReferenceView {
  const tinygltf::Model& authoredSource;
  const tinygltf::Model& evaluatedSource;
  const SceneGraph& graph;
  const container::geometry::gltf::MaterialVariantLibrary& variants;
  MaterialBindingContext context;
  // UINT32_MAX means an explicitly runtime-only row with no source owner.
  uint32_t sourceMaterial{UINT32_MAX},runtimeMaterial{};
  std::span<const uint32_t> protectedRuntimeMaterials;
};

struct MaterialTailReferenceView {
  const tinygltf::Model& authoredSource;
  const tinygltf::Model& evaluatedSource;
  const SceneGraph& graph;
  const container::geometry::gltf::MaterialVariantLibrary& variants;
  MaterialBindingContext context;
  uint32_t sourceTail{},runtimeTail{};
  // Supply every implicit/default/diagnostic runtime material retained by the
  // owner. Provider references and row-cookie ownership are separate proofs.
  std::span<const uint32_t> protectedRuntimeMaterials;
};

struct MaterialTailReferenceLimits {
  // Traversal totals aggregate BOTH source snapshots and the derived library;
  // they bound inspected reference/metadata domains, not binary texel/vertex
  // payloads (which are never read, copied, or decoded by this helper).
  uint64_t maxTableEntries{1'000'000},maxObjects{4'000'000},maxGraphRows{262144};
  uint64_t maxPrimitives{1'000'000},maxChannels{1'000'000},maxReferences{8'000'000};
  uint64_t maxMetadataValues{1'000'000},maxMetadataBytes{64ull*1024*1024};
  uint64_t maxTraversalSteps{16'000'000},maxOwnedBytes{16ull*1024*1024};
  uint32_t maxMetadataDepth{64},maxDeclarations{128};
};

struct MaterialTailReferenceStats {
  uint64_t graphRows{},objects{},primitives{},channels{},references{};
  uint64_t metadataValues{},metadataBytes{},traversalSteps{},ownedBytes{};
};

enum class MaterialTailAuditPhase : uint8_t { BeforeBindings,AfterBindings };
struct MaterialTailReferenceAccess;

// Copyable immutable audit authority. Owns compact ID arrays and the optional
// binding command, never Models, variant libraries, images or accessor data.
class MaterialTailReferenceAudit {
 public:
  MaterialTailReferenceAudit() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] MaterialTailReferenceStats stats() const noexcept;
  [[nodiscard]] uint64_t expectedGraphRevision() const noexcept;
  [[nodiscard]] uint64_t candidateGraphRevision() const noexcept;
 private:
  struct State;
  std::shared_ptr<const State> state_;
  friend struct MaterialTailReferenceAccess;
};

using MaterialReferenceLimits=MaterialTailReferenceLimits;
using MaterialReferenceStats=MaterialTailReferenceStats;
using MaterialReferenceAudit=MaterialTailReferenceAudit;
using MaterialReferencePhase=MaterialTailAuditPhase;

// Audits any selected source/runtime pair, including middle rows. The source
// mapping must explicitly associate the pair, and no other source row may own
// the selected runtime identity. A UINT32_MAX source target instead requires
// that NO source row owns the runtime identity (empty source tables are legal).
// No IDs are compacted or reused. All reference,
// schema, projection, ownership and bounded-recheck contracts below also apply.
// Outer lifecycle owners separately prove providers, history and prepared pins.
[[nodiscard]] MaterialReferenceAudit AuditUnusedMaterialReferences(
    const MaterialReferenceView&,
    const PreparedMaterialBindingChange* prospectiveBindings=nullptr,
    const MaterialReferenceLimits& limits={});
[[nodiscard]] bool CanRetireUnusedMaterial(const MaterialReferenceView&,
    const MaterialReferenceAudit&,MaterialReferencePhase phase) noexcept;

// Exact ONE last source/runtime material only. Audits BOTH source snapshots:
// every mesh primitive/default, raw and parsed variant mappings, retained
// material.lods/MSFT_lod.ids, and raw /materials/N animation pointers including
// unknown property suffixes. Graph traversal includes inactive/hidden/retired
// renderables; nonrenderable group material placeholders are not bindings.
// An optional current one-shot binding proof is projected without mutation and
// retained by owning command copy. Every graph row and override is validated.
//
// Known extension schemas are explicitly registered. Unknown retained extension
// objects (also nested outside extras) reject. Extras are bounded annotations,
// never interpreted as indices. Typed data is authoritative; raw JSON caches
// are bounded strings. The removed material's own outgoing references vanish.
// No I/O, decoding, generic index rewriting or external detached-history scan.
[[nodiscard]] MaterialTailReferenceAudit AuditMaterialTailRemoval(
    const MaterialTailReferenceView&,
    const PreparedMaterialBindingChange* prospectiveBindings=nullptr,
    const MaterialTailReferenceLimits& limits={});

// Rechecks reference absence with finite traversal and fixed-stack metadata
// scratch; no allocation, callbacks or mutation. Models/library/context MUST
// still be the BEFORE-removal tables in BOTH phases. AfterBindings means graph
// binding publication has happened, but manager/material/variant-tail removal
// has NOT. The outer owner separately proves cookies/providers/history pins,
// validates all stages before the first exchange, and synchronizes readers.
[[nodiscard]] bool CanRemoveMaterialTail(const MaterialTailReferenceView&,
    const MaterialTailReferenceAudit&,MaterialTailAuditPhase phase) noexcept;

} // namespace container::scene
