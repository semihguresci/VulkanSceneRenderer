#pragma once

#include "Container/renderer/scene/DrawCommand.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace container::renderer {

enum class GpuCullDrawUploadAction : uint8_t {
  SkipUnavailable,
  ReuseCached,
  Upload,
};

struct GpuCullDrawUploadCacheView {
  std::span<const DrawCommand *const> sourceData{};
  std::span<const size_t> sourceSizes{};
  std::span<const uint64_t> sourceRevisions{};
};

struct GpuCullDrawUploadPlanInputs {
  bool inputBufferReady{false};
  GpuCullDrawUploadCacheView cache{};
  uint32_t imageIndex{0};
  const DrawCommand *sourceData{nullptr};
  size_t sourceSize{0};
  uint64_t sourceRevision{0};
  uint32_t maxObjectCount{0};
};

struct GpuCullDrawUploadPlan {
  GpuCullDrawUploadAction action{GpuCullDrawUploadAction::SkipUnavailable};
  uint32_t drawCount{0};

  [[nodiscard]] bool updatesInputCount() const {
    return action != GpuCullDrawUploadAction::SkipUnavailable;
  }

  [[nodiscard]] bool uploadsBuffer() const {
    return action == GpuCullDrawUploadAction::Upload && drawCount > 0u;
  }
};

[[nodiscard]] inline bool gpuCullDrawUploadCacheSlotAvailable(
    const GpuCullDrawUploadCacheView &cache,
    uint32_t imageIndex) {
  const size_t slotCount =
      std::min({cache.sourceData.size(),
                cache.sourceSizes.size(),
                cache.sourceRevisions.size()});
  return imageIndex < slotCount;
}

[[nodiscard]] inline GpuCullDrawUploadPlan buildGpuCullDrawUploadPlan(
    const GpuCullDrawUploadPlanInputs &inputs) {
  if (!inputs.inputBufferReady || inputs.sourceData == nullptr ||
      inputs.sourceSize == 0u ||
      !gpuCullDrawUploadCacheSlotAvailable(inputs.cache, inputs.imageIndex)) {
    return {};
  }

  const uint32_t drawCount = static_cast<uint32_t>(
      std::min<size_t>(inputs.sourceSize, inputs.maxObjectCount));
  if (inputs.cache.sourceData[inputs.imageIndex] == inputs.sourceData &&
      inputs.cache.sourceSizes[inputs.imageIndex] == inputs.sourceSize &&
      inputs.cache.sourceRevisions[inputs.imageIndex] ==
          inputs.sourceRevision) {
    return {.action = GpuCullDrawUploadAction::ReuseCached,
            .drawCount = drawCount};
  }

  return {.action = GpuCullDrawUploadAction::Upload,
          .drawCount = drawCount};
}

} // namespace container::renderer
