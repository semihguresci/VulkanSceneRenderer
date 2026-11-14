#include "Container/renderer/lighting/DdgiBimRoomPlan.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace container::renderer {
namespace {

using Status = DdgiBimRoomCoverage::Status;
constexpr size_t kMaxFitAnchors = 128;

DdgiResourceLimits PayloadLimits(uint64_t limit) {
  return {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint64_t>::max(), limit};
}

bool UsableBounds(const DdgiSceneBounds& bounds) {
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis]) ||
        bounds.minimum[axis] >= bounds.maximum[axis]) {
      return false;
    }
  }
  return bounds.valid;
}

double Bulk(const DdgiSceneBounds& bounds) {
  const auto extent = glm::dvec3(bounds.maximum) - glm::dvec3(bounds.minimum);
  return extent.x * extent.y * extent.z;
}

float CellWidth(const DdgiProbeVolumeDesc& volume) {
  return std::max({volume.spacing.x, volume.spacing.y, volume.spacing.z});
}

bool Covers(const DdgiProbeVolume& volume, const DdgiSceneBounds& bounds,
            double blendCells, float primarySpacing) {
  const auto& desc = volume.description();
  if (!(CellWidth(desc) < primarySpacing)) {
    return false;
  }
  for (int axis = 0; axis < 3; ++axis) {
    const double margin = blendCells * desc.spacing[axis];
    if (double(bounds.minimum[axis]) - volume.minimum()[axis] < margin ||
        double(volume.maximum()[axis]) - bounds.maximum[axis] < margin) {
      return false;
    }
  }
  return true;
}

DdgiSceneBounds Union(const DdgiSceneBounds& a, const DdgiSceneBounds& b) {
  return {glm::min(a.minimum, b.minimum), glm::max(a.maximum, b.maximum), true};
}

double NeighbourGap(const DdgiBimRoomCoverage& a, const DdgiBimRoomCoverage& b) {
  const bool sameStorey = a.storeyIdentity.empty() && b.storeyIdentity.empty()
      ? a.storey == b.storey : a.storeyIdentity == b.storeyIdentity;
  if (!sameStorey || a.bounds.minimum.y >= b.bounds.maximum.y ||
      b.bounds.minimum.y >= a.bounds.maximum.y) {
    return std::numeric_limits<double>::infinity();
  }
  const auto first = glm::dvec3(a.bounds.minimum) - glm::dvec3(b.bounds.maximum);
  const auto second = glm::dvec3(b.bounds.minimum) - glm::dvec3(a.bounds.maximum);
  return std::hypot(std::max({0.0, first.x, second.x}), std::max({0.0, first.z, second.z}));
}

std::optional<DdgiSceneBounds> CombinedCandidate(
    std::span<const DdgiBimRoomCoverage> rooms, std::span<const size_t> uncovered,
    size_t anchor, float primarySpacing, std::stop_token stop) {
  const auto& seed = rooms[anchor];
  const double seedBulk = Bulk(seed.bounds);
  double bestGap = 0.5 * primarySpacing;
  double bestBulk = std::numeric_limits<double>::infinity();
  std::optional<DdgiSceneBounds> merged;
  // A nearby room on a different storey never merges. Compact adjacent
  // rooms can share padding, but the union may not bridge a large empty area.
  for (const size_t index : uncovered) {
    if (stop.stop_requested()) return {};
    if (index == anchor) continue;
    const auto& room = rooms[index];
    const double gap = NeighbourGap(seed, room);
    if (gap > bestGap) continue;
    const auto candidate = Union(seed.bounds, room.bounds);
    const double bulk = Bulk(candidate);
    if (bulk > 2.0 * (seedBulk + Bulk(room.bounds)) ||
        (gap == bestGap && bulk >= bestBulk)) continue;
    bestGap = gap;
    bestBulk = bulk;
    merged = candidate;
  }
  return merged;
}

struct Proposal {
  DdgiDetailVolumePlacementResult fit;
  std::vector<size_t> covered;
};

std::optional<Proposal> TryFit(const DdgiSettings& settings, const DdgiSceneBounds& bounds,
                               std::span<const DdgiBimRoomCoverage> rooms,
                               std::span<const size_t> uncovered, size_t anchor,
                               DdgiDetailVolumePlacementOptions options, float primarySpacing,
                               std::stop_token stop, std::string* reason) {
  DdgiDetailVolumePlacementResult fit;
  try {
    fit = fitDdgiDetailVolume(settings, bounds, options);
  } catch (const std::invalid_argument& error) {
    if (reason) *reason = error.what();
    return {};
  }
  const DdgiProbeVolume volume(fit.settings.additionalVolumes.back());
  if (CellWidth(volume.description()) >= primarySpacing) {
    if (reason) *reason = "Available probes and payload cannot fit this room more finely than the primary volume";
    return {};
  }
  if (!Covers(volume, rooms[anchor].bounds, settings.volumeBlendCells, primarySpacing)) {
    if (reason) *reason = "Room cannot fit inside the detail volume's full blend region";
    return {};
  }
  Proposal proposal{std::move(fit), {}};
  for (const size_t index : uncovered) {
    if (stop.stop_requested()) return {};
    if (Covers(volume, rooms[index].bounds, settings.volumeBlendCells, primarySpacing)) {
      proposal.covered.push_back(index);
    }
  }
  return proposal;
}

bool IsBetter(const Proposal& left, const Proposal& right) {
  if (left.covered.size() != right.covered.size()) {
    return left.covered.size() > right.covered.size();
  }
  return std::tuple{CellWidth(left.fit.settings.additionalVolumes.back()), left.fit.addedProbeCount} <
         std::tuple{CellWidth(right.fit.settings.additionalVolumes.back()), right.fit.addedProbeCount};
}

std::optional<Proposal> FindProposal(const DdgiSettings& settings,
                                    std::span<DdgiBimRoomCoverage> rooms,
                                    std::span<const size_t> uncovered,
                                    DdgiDetailVolumePlacementOptions options,
                                    float primarySpacing, std::stop_token stop) {
  std::optional<Proposal> best;
  const auto consider = [&](std::optional<Proposal> fit) {
    if (fit && (!best || IsBetter(*fit, *best))) best = std::move(fit);
  };
  for (const size_t index : uncovered.first(std::min(uncovered.size(), kMaxFitAnchors))) {
    if (stop.stop_requested()) return {};
    auto& room = rooms[index];
    consider(TryFit(settings, room.bounds, rooms, uncovered, index, options,
                    primarySpacing, stop, &room.reason));
    if (const auto merged = CombinedCandidate(rooms, uncovered, index, primarySpacing, stop)) {
      consider(TryFit(settings, *merged, rooms, uncovered, index, options,
                      primarySpacing, stop, nullptr));
    }
  }
  return best;
}

} // namespace

std::string_view ddgiBimRoomCoverageName(DdgiBimRoomCoverage::Status status) noexcept {
  switch (status) {
  case Status::CoveredExisting: return "covered-existing";
  case Status::CoveredPlanned: return "covered-planned";
  case Status::MissingGeometry: return "missing-geometry";
  case Status::Ambiguous: return "ambiguous";
  case Status::Uncovered: return "uncovered";
  }
  return "uncovered";
}

DdgiBimRoomPlan planDdgiBimRooms(const DdgiSettings& base, std::span<const DdgiBimRoom> rooms,
                                DdgiDetailVolumePlacementOptions options, std::stop_token stop) {
  if (stop.stop_requested()) return {.settings = base, .cancelled = true};
  if (rooms.size() > kMaxDdgiBimRooms) {
    throw std::length_error("DDGI room planning exceeds its BIM room limit");
  }
  validateDdgiSettings(base);
  const DdgiVolumeSet existing(base);
  auto resources = planDdgiVolumeResources(base, PayloadLimits(base.payloadBudgetBytes));
  const auto primarySpacing = std::min({base.volume.spacing.x, base.volume.spacing.y, base.volume.spacing.z});
  DdgiBimRoomPlan result{.settings = base,
                         .baseProbeCount = existing.probeCount(),
                         .basePayloadBytes = resources.payloadBytes};

  std::vector<const DdgiBimRoom*> ordered;
  ordered.reserve(rooms.size());
  for (const auto& room : rooms) ordered.push_back(&room);
  std::ranges::sort(ordered, [](const auto* a, const auto* b) {
    return std::tie(a->storey, a->storeyIdentity, a->identity, a->label, a->reason) <
           std::tie(b->storey, b->storeyIdentity, b->identity, b->label, b->reason);
  });
  std::vector<std::string_view> identities;
  identities.reserve(ordered.size());
  for (const auto* room : ordered) identities.emplace_back(room->identity);
  std::ranges::sort(identities);
  result.rooms.reserve(ordered.size());
  std::vector<size_t> uncovered;
  for (const auto* room : ordered) {
    if (stop.stop_requested()) return {.settings = base, .cancelled = true};
    DdgiBimRoomCoverage row{.identity = room->identity, .label = room->label, .storey = room->storey,
                            .bounds = room->bounds, .reason = room->reason,
                            .storeyIdentity = room->storeyIdentity};
    const auto duplicates = std::ranges::equal_range(identities, room->identity);
    if (room->ambiguous || room->identity.empty() || duplicates.size() != 1) {
      row.status = Status::Ambiguous;
      if (row.reason.empty()) row.reason = "Room has no unique BIM product identity or boundary";
    } else if (!UsableBounds(room->bounds)) {
      row.status = Status::MissingGeometry;
      if (row.reason.empty()) row.reason = "No usable three-dimensional room triangle bounds";
    } else {
      row.reason.clear();
      for (uint32_t index = 1; index < existing.volumeCount(); ++index) {
        if (Covers(existing.volume(index), room->bounds, base.volumeBlendCells, primarySpacing)) {
          row.status = Status::CoveredExisting;
          row.volumeIndex = index;
          break;
        }
      }
      if (row.status == Status::Uncovered) uncovered.push_back(result.rooms.size());
    }
    result.rooms.push_back(std::move(row));
  }
  std::ranges::stable_sort(uncovered, [&](size_t a, size_t b) {
    return Bulk(result.rooms[a].bounds) < Bulk(result.rooms[b].bounds);
  });

  uint32_t probes = existing.probeCount();
  std::string finalReason;
  while (!uncovered.empty()) {
    if (stop.stop_requested()) return {.settings = base, .cancelled = true};
    const auto slots = kMaxDdgiVolumes - 1 - uint32_t(result.settings.additionalVolumes.size());
    if (slots == 0) {
      finalReason = "All four DDGI volume slots are occupied";
      break;
    }
    const auto padding = uint32_t(std::floor(2.0 * std::max(1.f, base.volumeBlendCells))) + 2;
    const uint32_t minimumProbes = padding * padding * padding;
    if (probes >= options.maxTotalProbes || minimumProbes > options.maxTotalProbes - probes ||
        minimumProbes > options.maxDetailProbes || minimumProbes > UINT32_MAX - probes) {
      finalReason = "The remaining probe budget cannot fit a padded detail volume";
      break;
    }
    try {
      (void)planDdgiResources(probes + minimumProbes, base.resources, PayloadLimits(base.payloadBudgetBytes));
    } catch (const std::invalid_argument&) {
      finalReason = "The remaining DDGI payload budget cannot fit a padded detail volume";
      break;
    }
    // Preserve a room-specific fit diagnosis when a subsequent slot exhausts
    // the common budget. Every new search starts with the current capacities.
    for (const size_t index : uncovered) result.rooms[index].reason.clear();

    std::optional<Proposal> selected;
    if (slots > 1 && uncovered.size() > 1) {
      // Keep capacity for other rooms if a fair share can already improve
      // the scene. Retry without that share if nothing useful fits.
      const auto shares = static_cast<uint32_t>(std::min<size_t>(slots, uncovered.size()));
      auto fair = result.settings;
      fair.payloadBudgetBytes = resources.payloadBytes + (base.payloadBudgetBytes - resources.payloadBytes) / shares;
      auto fairOptions = options;
      fairOptions.maxDetailProbes = std::min(options.maxDetailProbes, (options.maxTotalProbes - probes) / shares);
      selected = FindProposal(fair, result.rooms, uncovered, fairOptions, primarySpacing, stop);
    }
    if (!selected) {
      selected = FindProposal(result.settings, result.rooms, uncovered, options, primarySpacing, stop);
    }
    if (stop.stop_requested()) return {.settings = base, .cancelled = true};
    if (!selected) break;

    auto& proposal = *selected;
    auto& fit = proposal.fit;
    for (const size_t index : proposal.covered) {
      auto& row = result.rooms[index];
      row.status = Status::CoveredPlanned;
      row.volumeIndex = fit.volumeIndex;
      row.reason.clear();
    }
    auto& published = result.volumes.emplace_back(
        DdgiBimPlannedVolume{fit.volumeIndex, fit.addedProbeCount, fit.boundsMin, fit.boundsMax,
                             std::move(proposal.covered)});
    std::ranges::sort(published.roomIndices);
    fit.settings.payloadBudgetBytes = base.payloadBudgetBytes;
    result.settings = std::move(fit.settings);
    resources = planDdgiVolumeResources(result.settings, PayloadLimits(base.payloadBudgetBytes));
    probes = fit.totalProbeCount;
    std::erase_if(uncovered, [&](size_t index) { return result.rooms[index].status == Status::CoveredPlanned; });
  }

  for (const size_t index : uncovered) {
    auto& row = result.rooms[index];
    if (row.reason.empty()) {
      row.reason = finalReason.empty() ? "Room was outside the bounded fit search" : finalReason;
    }
  }
  result.totalProbeCount = probes;
  result.payloadBytes = resources.payloadBytes;
  const uint64_t updates = resources.updateCapacity.probeCount;
  if (updates) result.framesPerProbeCycle = (probes + updates - 1) / updates;
  if (stop.stop_requested()) return {.settings = base, .cancelled = true};
  return result;
}

} // namespace container::renderer
