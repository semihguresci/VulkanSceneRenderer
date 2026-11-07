#pragma once

#include <Container/geometry/GltfTextureResourceReferences.h>
#include <tiny_gltf.h>
#include <cstdint>
#include <memory>
#include <span>

namespace container::geometry::gltf {

// Empty means all active; otherwise exact table length and values zero/one.
// Material/node IDs describe the already projected detached export, not source
// or runtime IDs from another domain. Inactive owners are canonicalized and
// their channels omitted; their later owner compaction remains caller-owned.
struct GltfResourceExportMasks {
  std::span<const uint8_t> images,textures,samplers,materials,nodes;
};
struct GltfResourceExportCompactionLimits {
  uint64_t maxTableEntries{1'000'000},maxOwnedBytes{256ull*1024*1024};
  uint64_t maxPayloadBytes{512ull*1024*1024},maxTraversalSteps{64'000'000};
  GltfTextureResourceReferenceLimits references{};
};
struct GltfResourceExportCompactionStats {
  uint64_t retiredImages{},retiredTextures{},retiredSamplers{},references{},ownedBytes{},payloadBytes{};
};
struct GltfResourceExportCompactionAccess;
class GltfResourceExportCompactionPlan {
 public:
  GltfResourceExportCompactionPlan() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] std::span<const int32_t> oldToNewImages() const noexcept;
  [[nodiscard]] std::span<const int32_t> oldToNewTextures() const noexcept;
  [[nodiscard]] std::span<const int32_t> oldToNewSamplers() const noexcept;
  [[nodiscard]] std::span<const int32_t> newToOldImages() const noexcept;
  [[nodiscard]] std::span<const int32_t> newToOldTextures() const noexcept;
  [[nodiscard]] std::span<const int32_t> newToOldSamplers() const noexcept;
  [[nodiscard]] GltfResourceExportCompactionStats stats() const noexcept;
 private:
  struct State;
  std::shared_ptr<const State> state_;
  friend struct GltfResourceExportCompactionAccess;
};

// Pure bounded plan; no image decoding, URI/file access or binary copy. Exact
// metadata/reference witnesses and source storage identities are rechecked at
// apply. Unknown active normative schemas and active references to removed
// targets reject. Extras are bounded annotations and remain unchanged.
[[nodiscard]] GltfResourceExportCompactionPlan PlanGltfResourceExportCompaction(
    const tinygltf::Model&,GltfResourceExportMasks={},const GltfResourceExportCompactionLimits& limits={});
// Preallocates all replacements before edits. Surviving resource order is
// stable. Buffers, views/accessors and surviving image byte allocations retain
// identity. Does not mutate live snapshots or reclaim unused shared buffer bytes.
void ApplyGltfResourceExportCompaction(tinygltf::Model&,const GltfResourceExportCompactionPlan&);

} // namespace container::geometry::gltf
