#pragma once

#include "Container/geometry/DotBimLoader.h"
#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/BimWalkability.h"
#include "Container/robotics/RoboticsScene.h"
#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/DerivedAssetCache.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace container::robotics {

struct BimAssetSourceLayer {
  // Resolved authoring input, in importer consultation order.
  std::filesystem::path path;
  util::ContentFingerprint fingerprint;
};

struct BimAssetPreprocessOptions {
  RoboticsSceneOptions scene;
  BimCollisionCookOptions cook;
  double maxSlopeDegrees{30.0};
};

struct BimAssetPreprocessTimings {
  std::chrono::nanoseconds sourceReadAndFingerprint{};
  // Preprocessing is bypassed on a cache hit.
  std::optional<std::chrono::nanoseconds> bimImport;
  std::optional<std::chrono::nanoseconds> semanticScene;
  std::optional<std::chrono::nanoseconds> collisionCook;
  std::optional<std::chrono::nanoseconds> walkableSurfaces;
};

struct BimPreprocessedAssets {
  std::string cacheKey;
  util::DerivedAssetCacheStatus cacheStatus{util::DerivedAssetCacheStatus::Disabled};
  uint64_t cacheBytesWritten{};
  std::string producerRevision;
  std::string cacheDiagnostic;
  std::vector<BimAssetSourceLayer> sourceLayers;

  // CPU render geometry, draw occurrences and basic PBR factors in original
  // renderer coordinates. Read authoritative IDs, units and relationships
  // from the semantic scene.
  geometry::dotbim::Model renderModel;
  RoboticsScene scene;
  CookedBimCollision collision;
  std::vector<BimWalkableSurface> walkableSurfaces;
};

// Reuses static CPU assets only. Door state, robot queries and Jolt shapes are
// evaluated by callers against the returned immutable source revision.
[[nodiscard]] BimPreprocessedAssets loadBimPreprocessedAssets(
    const std::filesystem::path& source, const std::filesystem::path& cacheDirectory,
    const BimAssetPreprocessOptions& options, std::stop_token stop = {},
    BimAssetPreprocessTimings* timings = nullptr);

void writeBimCachedRenderPreview(const std::filesystem::path& destination,
                                const geometry::dotbim::Model& model,
                                const RoboticsScene& scene);

} // namespace container::robotics
