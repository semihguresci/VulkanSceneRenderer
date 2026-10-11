#include "Container/renderer/lighting/DdgiVolumeSet.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace container::renderer {
namespace {
float cellSize(const DdgiProbeVolume &volume) {
  const auto s = volume.description().spacing;
  return std::max({s.x, s.y, s.z});
}
float edgeWeight(const DdgiProbeVolume &volume, glm::vec3 world, float cells) {
  if (cells == 0.f) return 1.f;
  const auto &d = volume.description();
  float fraction = 1.f;
  for (int axis = 0; axis < 3; ++axis) {
    if (d.counts[axis] <= 1) continue;
    const float distance = std::min(world[axis] - volume.minimum()[axis],
                                    volume.maximum()[axis] - world[axis]);
    fraction = std::min(fraction, std::clamp(
        (distance / d.spacing[axis]) / cells, 0.f, 1.f));
  }
  return fraction * fraction * (3.f - 2.f * fraction);
}
} // namespace
DdgiVolumeSet::DdgiVolumeSet(const DdgiSettings &settings)
    : blendCells_(settings.volumeBlendCells) {
  if (settings.additionalVolumes.size() >= kMaxDdgiVolumes)
    throw std::invalid_argument("DDGI supports at most four probe volumes");
  if (!std::isfinite(blendCells_) || blendCells_ < 0.f || blendCells_ > 8.f)
    throw std::invalid_argument("DDGI volumeBlendCells must be in [0,8]");
  auto append = [&](const DdgiProbeVolumeDesc &desc) {
    DdgiProbeVolume volume(desc);
    if (volume.probeCount() > UINT32_MAX - probeCount_)
      throw std::invalid_argument("DDGI combined probe count overflows uint32");
    offsets_.push_back(probeCount_);
    probeCount_ += volume.probeCount();
    volumes_.push_back(volume);
  };
  append(settings.volume);
  for (const auto &v : settings.additionalVolumes) append(v);
  coarseToFine_.resize(volumes_.size());
  std::iota(coarseToFine_.begin(), coarseToFine_.end(), 0u);
  // Equal resolution gives earlier configured volumes final precedence.
  std::stable_sort(coarseToFine_.begin(), coarseToFine_.end(), [&](auto a, auto b) {
    const auto sa = cellSize(volumes_[a]), sb = cellSize(volumes_[b]);
    return sa != sb ? sa > sb : a > b;
  });
}
uint32_t DdgiVolumeSet::probeOffset(uint32_t index) const {
  return offsets_.at(index);
}
const DdgiProbeVolume &DdgiVolumeSet::volume(uint32_t index) const {
  return volumes_.at(index);
}
glm::vec3 DdgiVolumeSet::probePosition(uint32_t index) const {
  if (index >= probeCount_) throw std::out_of_range("DDGI packed probe index");
  const auto upper = std::upper_bound(offsets_.begin(), offsets_.end(), index);
  const auto v = size_t(upper - offsets_.begin() - 1);
  return volumes_[v].probePosition(index - offsets_[v]);
}
std::vector<glm::vec3> DdgiVolumeSet::positions() const {
  std::vector<glm::vec3> result(probeCount_);
  for (uint32_t v = 0; v < volumeCount(); ++v)
    for (uint32_t i = 0; i < volumes_[v].probeCount(); ++i)
      result[offsets_[v] + i] = volumes_[v].probePosition(i);
  return result;
}
std::array<DdgiGpuVolume, kMaxDdgiVolumes>
DdgiVolumeSet::gpuVolumes(const DdgiSettings &settings) const {
  std::array<DdgiGpuVolume, kMaxDdgiVolumes> result{};
  const auto p = settings.volume.spacing;
  const double primaryCell = std::min({p.x, p.y, p.z});
  for (uint32_t v = 0; v < volumeCount(); ++v) {
    const auto &d = volumes_[v].description();
    const double ratio = double(std::min({d.spacing.x, d.spacing.y, d.spacing.z})) / primaryCell;
    auto scaleBias = [&](float bias) {
      return float(std::min(double(bias) * ratio, double(settings.maxDistance)));
    };
    result[v].originBlend = {d.origin, blendCells_};
    result[v].spacingNormalBias = {d.spacing, scaleBias(settings.normalBias)};
    result[v].countsOffset = {d.counts, offsets_[v]};
    result[v].bias = {scaleBias(settings.viewBias),
                      scaleBias(settings.visibilityBias), 0.f, 0.f};
  }
  return result;
}
DdgiVolumeBlend DdgiVolumeSet::blend(glm::vec3 world,
                                     std::span<const bool> available) const {
  if (!available.empty() && available.size() != volumes_.size())
    throw std::invalid_argument("DDGI availability size differs from volumes");
  DdgiVolumeBlend result;
  bool found = false;
  for (const auto v : coarseToFine_) {
    if ((!available.empty() && !available[v]) || !volumes_[v].contains(world)) continue;
    const float weight = found ? edgeWeight(volumes_[v], world, blendCells_) : 1.f;
    for (auto &w : result.weights) w *= 1.f - weight;
    result.weights[v] += weight;
    found = true;
  }
  return result;
}
DdgiResourcePlan planDdgiVolumeResources(const DdgiSettings &settings,
                                         const DdgiResourceLimits &limits) {
  auto combinedLimits = limits;
  combinedLimits.maxPayloadBytes = std::min(limits.maxPayloadBytes, settings.payloadBudgetBytes);
  return planDdgiResources(DdgiVolumeSet(settings).probeCount(),
                           settings.resources, combinedLimits);
}
} // namespace container::renderer
