#pragma once

#include "Container/renderer/lighting/DdgiProbeVolume.h"

#include <optional>
#include <vector>

namespace container::renderer {

struct DdgiDirtyRegion {
  glm::vec3 minimum{};
  glm::vec3 maximum{};
};

// Bounds describe the influence of a changed light or both the old and new
// geometry bounds, expanded by the relevant transport distance by the caller.
// Empty bounds on a changed revision mean global dirtiness. Camera motion alone
// changes priority, never irradiance validity or the content epoch.
struct DdgiSchedulingHints {
  std::optional<glm::vec3> cameraPosition;
  glm::vec3 cameraForward{0.f, 0.f, -1.f};
  std::vector<DdgiDirtyRegion> dirtyRegions;
};

struct DdgiPriorityPlan {
  DdgiUpdateBatch batch{};
  std::vector<DdgiProbeUpdate> updates;
  // Transaction metadata: commit clears only dirtiness present when planned.
  std::vector<uint64_t> dirtyStamps;
  uint64_t resetGeneration = 0;
  uint64_t submissionSequence = 0;
};

// CPU-owned, flattened probe indices may span several volumes. At least half
// each nonzero budget advances a persistent round-robin sweep; other slots
// favor dirty probes, then visible/near probes, then older submitted samples.
// Planning and dirtiness changes do not advance time or the fairness cursor.
class DdgiPriorityScheduler {
public:
  void reset(uint32_t probeCount);
  // Retains mapped probe age/dirtiness while exposing new nodes as dirty. A
  // UINT32_MAX source means a newly exposed node. Retire planned work first.
  void remap(std::span<const uint32_t> previousIndexForNewProbe);
  void markDirty(std::span<const glm::vec3> positions,
                 std::span<const DdgiDirtyRegion> regions,
                 uint64_t contentRevision);
  [[nodiscard]] DdgiPriorityPlan
  plan(std::span<const glm::vec3> positions, const DdgiUpdateBudget &budget,
       const DdgiSchedulingHints &hints = {}) const;
  // Call only for a successfully submitted complete plan. Stale/duplicate
  // commits throw before state changes; newly dirtied probes remain dirty.
  void commit(const DdgiPriorityPlan &plan);
  [[nodiscard]] uint32_t refreshRemaining() const noexcept;
  [[nodiscard]] uint32_t nextProbe() const noexcept { return nextProbe_; }

private:
  std::vector<uint64_t> dirtyStamps_;
  std::vector<uint64_t> lastSubmitted_;
  uint64_t revision_ = 0;
  uint64_t dirtySequence_ = 0;
  uint64_t resetGeneration_ = 0;
  uint64_t submissionSequence_ = 0;
  uint32_t nextProbe_ = 0;
};

} // namespace container::renderer
