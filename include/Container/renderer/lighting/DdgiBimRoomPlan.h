#pragma once

#include "Container/renderer/lighting/DdgiVolumePlacement.h"

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

inline constexpr size_t kMaxDdgiBimRooms = 16384;

struct DdgiBimRoom {
  std::string identity, label, storey;
  DdgiSceneBounds bounds;
  bool ambiguous{};
  std::string reason;
  std::string storeyIdentity;
};

struct DdgiBimRoomCoverage {
  enum class Status { CoveredExisting, CoveredPlanned, MissingGeometry, Ambiguous, Uncovered };

  std::string identity, label, storey;
  DdgiSceneBounds bounds;
  Status status{Status::Uncovered};
  std::optional<uint32_t> volumeIndex;
  std::string reason;
  std::string storeyIdentity;
};

struct DdgiBimPlannedVolume {
  uint32_t volumeIndex{};
  uint32_t probeCount{};
  glm::vec3 boundsMin{}, boundsMax{};
  std::vector<size_t> roomIndices;
};

struct DdgiBimRoomPlan {
  DdgiSettings settings;
  std::vector<DdgiBimRoomCoverage> rooms;
  std::vector<DdgiBimPlannedVolume> volumes;
  uint32_t baseProbeCount{}, totalProbeCount{};
  uint64_t basePayloadBytes{}, payloadBytes{};
  // Lower bound using the configured shared probe and ray caps. A zero cap
  // yields zero. Runtime pause, camera priority and GPU submission affect cadence.
  uint64_t framesPerProbeCycle{};
  bool cancelled{};
};

[[nodiscard]] std::string_view ddgiBimRoomCoverageName(DdgiBimRoomCoverage::Status) noexcept;

// Each input row describes a stable BIM room product and its complete world
// bounds. Missing geometry and ambiguous identity do not enter the fit.
// Geometric coverage requires the complete room AABB in one finer detail
// lattice's full blend region. It depends on valid runtime probe support.
// The result retains the configured primary and existing detail volumes.
// Cancellation returns the base settings and discards partial proposals.
[[nodiscard]] DdgiBimRoomPlan planDdgiBimRooms(const DdgiSettings& base,
                                              std::span<const DdgiBimRoom> rooms,
                                              DdgiDetailVolumePlacementOptions options = {},
                                              std::stop_token stop = {});

} // namespace container::renderer
