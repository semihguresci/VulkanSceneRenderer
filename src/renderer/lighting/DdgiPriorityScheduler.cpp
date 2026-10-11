#include "Container/renderer/lighting/DdgiPriorityScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace container::renderer {
namespace {
bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}
void validatePositions(std::span<const glm::vec3> positions, size_t count) {
  if (positions.size() != count)
    throw std::invalid_argument("DDGI priority probe position count");
  if (!std::all_of(positions.begin(), positions.end(), finite))
    throw std::invalid_argument("DDGI priority nonfinite probe position");
}
double distanceSquared(glm::vec3 a, glm::vec3 b) {
  const double x = double(a.x) - b.x, y = double(a.y) - b.y,
               z = double(a.z) - b.z;
  return x * x + y * y + z * z;
}
} // namespace

void DdgiPriorityScheduler::reset(uint32_t probeCount) {
  if (resetGeneration_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("DDGI priority reset generation overflow");
  // Allocate both arrays before replacing any state.
  std::vector<uint64_t> dirty(probeCount, 1), ages(probeCount, 0);
  dirtyStamps_.swap(dirty);
  lastSubmitted_.swap(ages);
  ++resetGeneration_;
  revision_ = submissionSequence_ = nextProbe_ = 0;
  dirtySequence_ = 1;
}

void DdgiPriorityScheduler::remap(std::span<const uint32_t> mapping) {
  if (mapping.size() != dirtyStamps_.size() ||
      std::any_of(mapping.begin(), mapping.end(), [&](uint32_t index) {
        return index != UINT32_MAX && index >= dirtyStamps_.size();
      }))
    throw std::invalid_argument("DDGI priority invalid scroll mapping");
  if (resetGeneration_ == UINT64_MAX || dirtySequence_ == UINT64_MAX)
    throw std::overflow_error("DDGI priority scroll generation overflow");
  std::vector<uint64_t> dirty(mapping.size()), ages(mapping.size());
  const auto exposedStamp = dirtySequence_ + 1;
  for (size_t index = 0; index < mapping.size(); ++index) {
    const auto previous = mapping[index];
    dirty[index] = previous == UINT32_MAX ? exposedStamp : dirtyStamps_[previous];
    ages[index] = previous == UINT32_MAX ? 0 : lastSubmitted_[previous];
  }
  dirtyStamps_.swap(dirty);
  lastSubmitted_.swap(ages);
  ++dirtySequence_;
  ++resetGeneration_;
  // Numerical fairness advances across every move, including when the old
  // cursor's physical probe has scrolled out of coverage.
}

void DdgiPriorityScheduler::markDirty(
    std::span<const glm::vec3> positions,
    std::span<const DdgiDirtyRegion> regions, uint64_t contentRevision) {
  validatePositions(positions, dirtyStamps_.size());
  for (const auto &region : regions)
    if (!finite(region.minimum) || !finite(region.maximum) ||
        region.minimum.x > region.maximum.x ||
        region.minimum.y > region.maximum.y ||
        region.minimum.z > region.maximum.z)
      throw std::invalid_argument("DDGI priority invalid dirty region");
  if (revision_ == contentRevision) return;
  if (dirtySequence_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("DDGI priority dirty sequence overflow");
  revision_ = contentRevision;
  ++dirtySequence_;
  for (size_t index = 0; index < positions.size(); ++index) {
    const auto p = positions[index];
    const bool affected = regions.empty() || std::any_of(
        regions.begin(), regions.end(), [p](const DdgiDirtyRegion &region) {
          return p.x >= region.minimum.x && p.x <= region.maximum.x &&
                 p.y >= region.minimum.y && p.y <= region.maximum.y &&
                 p.z >= region.minimum.z && p.z <= region.maximum.z;
        });
    if (affected) dirtyStamps_[index] = dirtySequence_;
  }
}

DdgiPriorityPlan DdgiPriorityScheduler::plan(
    std::span<const glm::vec3> positions, const DdgiUpdateBudget &budget,
    const DdgiSchedulingHints &hints) const {
  validatePositions(positions, dirtyStamps_.size());
  if (hints.cameraPosition && (!finite(*hints.cameraPosition) ||
                               !finite(hints.cameraForward)))
    throw std::invalid_argument("DDGI priority nonfinite camera");
  if (budget.raysPerProbe == 0)
    throw std::invalid_argument("DDGI priority zero rays per probe");
  DdgiPriorityPlan result;
  result.resetGeneration = resetGeneration_;
  result.submissionSequence = submissionSequence_;
  const auto count = static_cast<uint32_t>(positions.size());
  auto &batch = result.batch;
  batch.volumeProbeCount = count;
  batch.firstProbe = batch.nextProbe = nextProbe_;
  batch.raysPerProbe = budget.raysPerProbe;
  batch.probeCount = std::min({count, budget.maxProbesPerFrame,
                             budget.maxRaysPerFrame / budget.raysPerProbe});
  batch.rayCount = batch.probeCount * budget.raysPerProbe;
  if (batch.probeCount == 0) return result;
  result.updates.reserve(batch.probeCount);
  result.dirtyStamps.reserve(batch.probeCount);
  std::vector<bool> selected(count, false);
  auto append = [&](uint32_t index) {
    result.updates.push_back({index,
                             static_cast<uint32_t>(result.updates.size()) *
                                 budget.raysPerProbe,
                             budget.raysPerProbe, 0});
    result.dirtyStamps.push_back(dirtyStamps_[index]);
    selected[index] = true;
  };
  const uint32_t fairSlots = batch.probeCount / 2u + batch.probeCount % 2u;
  uint32_t cursor = nextProbe_;
  for (uint32_t slot = 0; slot < fairSlots; ++slot) {
    append(cursor);
    cursor = cursor + 1u == count ? 0u : cursor + 1u;
  }
  std::vector<uint32_t> priority;
  priority.reserve(count - fairSlots);
  for (uint32_t index = 0; index < count; ++index)
    if (!selected[index]) priority.push_back(index);
  auto inFront = [&](uint32_t index) {
    if (!hints.cameraPosition) return false;
    const auto p = positions[index], camera = *hints.cameraPosition;
    return (double(p.x) - camera.x) * hints.cameraForward.x +
               (double(p.y) - camera.y) * hints.cameraForward.y +
               (double(p.z) - camera.z) * hints.cameraForward.z >
           0.0;
  };
  auto higher = [&](uint32_t a, uint32_t b) {
    const bool dirtyA = dirtyStamps_[a] != 0, dirtyB = dirtyStamps_[b] != 0;
    if (dirtyA != dirtyB) return dirtyA;
    if (hints.cameraPosition) {
      const bool frontA = inFront(a), frontB = inFront(b);
      if (frontA != frontB) return frontA;
    }
    if (lastSubmitted_[a] != lastSubmitted_[b])
      return lastSubmitted_[a] < lastSubmitted_[b];
    if (hints.cameraPosition) {
      const auto distanceA = distanceSquared(positions[a], *hints.cameraPosition),
                 distanceB = distanceSquared(positions[b], *hints.cameraPosition);
      if (distanceA != distanceB) return distanceA < distanceB;
    }
    // Prefer round-robin proximity for deterministic no-camera behavior.
    const auto offsetA = (uint64_t(a) + count - nextProbe_) % count,
               offsetB = (uint64_t(b) + count - nextProbe_) % count;
    return offsetA < offsetB;
  };
  const uint32_t prioritySlots = batch.probeCount - fairSlots;
  std::partial_sort(priority.begin(), priority.begin() + prioritySlots,
                    priority.end(), higher);
  for (uint32_t slot = 0; slot < prioritySlots; ++slot) append(priority[slot]);
  // Skip priority probes contiguous with the fairness sweep, without allowing
  // a priority island farther ahead to skip any unsubmitted probe.
  for (uint32_t scanned = 0; scanned < count && selected[cursor]; ++scanned)
    cursor = cursor + 1u == count ? 0u : cursor + 1u;
  if (batch.probeCount == count) cursor = nextProbe_;
  batch.nextProbe = cursor;
  return result;
}

void DdgiPriorityScheduler::commit(const DdgiPriorityPlan &plan) {
  const auto &batch = plan.batch;
  if (plan.resetGeneration != resetGeneration_ ||
      plan.submissionSequence != submissionSequence_ ||
      batch.volumeProbeCount != dirtyStamps_.size() ||
      batch.firstProbe != nextProbe_ ||
      plan.updates.size() != batch.probeCount ||
      plan.dirtyStamps.size() != batch.probeCount ||
      batch.probeCount > batch.volumeProbeCount ||
      batch.raysPerProbe == 0 ||
      uint64_t(batch.probeCount) * batch.raysPerProbe != batch.rayCount ||
      (batch.volumeProbeCount != 0 && batch.nextProbe >= batch.volumeProbeCount))
    throw std::invalid_argument("DDGI priority stale or malformed plan");
  if (batch.probeCount == 0) return;
  if (submissionSequence_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("DDGI priority submission sequence overflow");
  std::vector<bool> selected(dirtyStamps_.size(), false);
  for (size_t slot = 0; slot < plan.updates.size(); ++slot) {
    const auto &update = plan.updates[slot];
    if (update.probeIndex >= selected.size() || selected[update.probeIndex] ||
        update.firstRay != slot * uint64_t(batch.raysPerProbe) ||
        update.rayCount != batch.raysPerProbe)
      throw std::invalid_argument("DDGI priority malformed update records");
    selected[update.probeIndex] = true;
  }
  ++submissionSequence_;
  for (size_t slot = 0; slot < plan.updates.size(); ++slot) {
    const auto index = plan.updates[slot].probeIndex;
    lastSubmitted_[index] = submissionSequence_;
    if (dirtyStamps_[index] == plan.dirtyStamps[slot]) dirtyStamps_[index] = 0;
  }
  nextProbe_ = batch.nextProbe;
}

uint32_t DdgiPriorityScheduler::refreshRemaining() const noexcept {
  return static_cast<uint32_t>(std::count_if(
      dirtyStamps_.begin(), dirtyStamps_.end(),
      [](uint64_t stamp) { return stamp != 0; }));
}

} // namespace container::renderer
