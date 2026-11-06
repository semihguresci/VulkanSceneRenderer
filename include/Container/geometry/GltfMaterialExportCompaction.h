#pragma once

#include <Container/geometry/GltfMerge.h>

#include <memory>
#include <span>

namespace container::geometry::gltf {

struct GltfMaterialExportCompactionLimits : GltfMergeLimits {
  uint64_t maxOwnedBytes{64ull*1024*1024};
  uint64_t maxMetadataValues{1'000'000},maxTraversalSteps{16'000'000};
  uint32_t maxDeclarations{128};
};
struct GltfMaterialExportCompactionStats {
  uint64_t retiredMaterials{},references{},metadataBytes{},payloadBytes{},ownedBytes{};
};
struct GltfMaterialExportCompactionAccess;

// Immutable authority for one borrowed, detached export model. Owns maps and
// exact reference/shape snapshots, never material records, Models or payloads.
class GltfMaterialExportCompactionPlan {
 public:
  GltfMaterialExportCompactionPlan() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] std::span<const int32_t> oldToNewMaterials() const noexcept;
  [[nodiscard]] std::span<const int32_t> newToOldMaterials() const noexcept;
  [[nodiscard]] GltfMaterialExportCompactionStats stats() const noexcept;
 private:
  struct State;
  std::shared_ptr<const State> state_;
  friend struct GltfMaterialExportCompactionAccess;
};

// IDs belong to the staged export material table, AFTER the caller explicitly
// projects source->runtime material indices. Arbitrary unique rows may retire;
// surviving rows retain order. Every incoming primitive, variant, material LOD
// or raw material animation pointer to a retired row rejects. Only the index
// prefix changes in surviving pointers, even for unsupported property suffixes.
// Extras stay opaque. Unknown normative reference schemas fail closed.
// Map scratch follows count/storage checks; complete metadata/reference/work
// preflight precedes allocation of the immutable reference snapshots.
[[nodiscard]] GltfMaterialExportCompactionPlan PlanGltfMaterialExportCompaction(
    const tinygltf::Model& stagedExport,std::span<const int32_t> retiredMaterials,
    const GltfMaterialExportCompactionLimits& limits={});

// Rechecks finite bounds, exact known references/table shape and model identity
// before edits. Allocates pointer replacements/survivor storage before the first
// mutation, then compacts material rows in the caller-owned unpublished model.
// Binary/image allocations and every geometry/resource identity stay unchanged.
// This does not mutate the live source, perform I/O, or implement user deletion.
void ApplyGltfMaterialExportCompaction(
    tinygltf::Model& stagedExport,const GltfMaterialExportCompactionPlan&);

} // namespace container::geometry::gltf
