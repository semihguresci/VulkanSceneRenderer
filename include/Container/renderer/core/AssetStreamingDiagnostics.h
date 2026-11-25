#pragma once

#include "Container/common/CommonMath.h"
#include "Container/utility/SceneAssetBudget.h"

#include <array>
#include <cstdint>
#include <optional>

namespace container::renderer {

enum class AssetStreamingOverlay : uint8_t { Off, GeometryLod, TextureResidency };

struct AssetStreamingMarker {
  glm::vec2 uv{};
  uint32_t level{};
  bool waitingForDetail{};
};

struct AssetStreamingSummary {
  gpu::SceneAssetBudget::Snapshot budget;
  std::array<uint64_t, 3> geometryLevels{};
  uint64_t baseTriangles{}, submittedTriangles{}, geometryIndexBytes{};
  uint64_t textureImages{}, textureFallbackImages{}, textureWaitingImages{};
  uint64_t texturePromotions{}, textureEvictions{};
  uint64_t texturePendingBytes{}, texturePeakCopyBytes{};
  uint64_t geometryPendingBytes{}, geometryPeakCopyBytes{};
  bool assetCacheEnabled{};
};

struct AssetRayBuildSample {
  uint64_t frame{};
  uint64_t generation{};
  uint32_t blasBuilt{}, blasReused{}, instances{};
  uint64_t asAllocatedBytes{}, inputAllocatedBytes{}, scratchAllocatedBytes{};
  std::optional<float> gpuMs;
};

}  // namespace container::renderer
