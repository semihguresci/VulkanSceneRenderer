#include "Container/renderer/core/RendererFrontend.h"

#include "Container/app/AppConfig.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderGraph.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/raytracing/RaySceneAcceleration.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include "Container/renderer/temporal/TemporalConventions.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/GuiManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <ranges>

namespace container::renderer {

AssetStreamingSummary RendererFrontend::assetStreamingSummary() {
  AssetStreamingSummary summary{
      .budget = svc_.allocationManager.sceneAssetBudget(),
      .assetCacheEnabled = !svc_.config.assetCacheDirectory.empty(),
  };
  addPrimaryGeometryDiagnostics(summary);
  addTextureStreamingDiagnostics(summary);
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const auto lod = subs_.bimManager->geometryLodStats(currentBimDrawFilter());
    summary.baseTriangles += lod.baseTriangles;
    summary.submittedTriangles += lod.triangles;
    summary.geometryIndexBytes += lod.extraIndexBytes;
    for (size_t level = 0; level < std::min(summary.geometryLevels.size(), lod.levels.size()); ++level)
      summary.geometryLevels[level] += lod.levels[level];
  }
  return summary;
}

void RendererFrontend::drawAssetStreamingOverlay(const FrameRecordParams& frame,
                                                  const glm::mat4& viewProjection) {
  const auto overlay = subs_.guiManager->assetStreamingOverlay();
  const auto textureMarkers = overlay == AssetStreamingOverlay::TextureResidency
      ? textureMaterialMarkers() : std::vector<std::optional<AssetStreamingMarker>>{};
  constexpr uint32_t columns = 16, rows = 10;
  struct Cell {
    std::optional<AssetStreamingMarker> marker;
    float depth{};
  };
  std::array<Cell, columns * rows> cells;
  const auto bimFilter = currentBimDrawFilter();
  const auto collect = [&](const FrameSceneGeometry& scene, const FrameDrawLists& lists,
                           bool bim) {
    if (!scene.objectData)
      return;
    for (const auto* draws : {lists.opaqueDrawCommands, lists.transparentDrawCommands}) {
      if (!draws)
        continue;
      for (const auto& draw : *draws) {
        for (uint32_t instance = 0; instance < draw.instanceCount; ++instance) {
          const size_t index = size_t(draw.objectIndex) + instance;
          if (index >= scene.objectData->size() ||
              (!bim && index == sceneState_.diagCubeObjectIndex) ||
              (bim && !subs_.bimManager->objectMatchesFilter(static_cast<uint32_t>(index), bimFilter)))
            continue;
          const auto& object = (*scene.objectData)[index];
          const auto pos = temporal::projectSceneClip(
              viewProjection * glm::vec4(glm::vec3(object.boundingSphere), 1.0f));
          if (!pos.valid || pos.uv.x < 0 || pos.uv.x >= 1 || pos.uv.y < 0 || pos.uv.y >= 1)
            continue;
          auto& cell = cells[static_cast<uint32_t>(pos.uv.y * rows) * columns +
                             static_cast<uint32_t>(pos.uv.x * columns)];
          if (cell.marker && cell.depth >= pos.reverseZDepth)
            continue;
          AssetStreamingMarker marker{.uv = pos.uv};
          if (overlay == AssetStreamingOverlay::TextureResidency) {
            const auto material = object.objectInfo.x;
            if (material >= textureMarkers.size() || !textureMarkers[material])
              continue;
            marker.level = textureMarkers[material]->level;
            marker.waitingForDetail = textureMarkers[material]->waitingForDetail;
          } else if (bim) {
            if (const auto info = subs_.bimManager->geometryLodDrawInfo(static_cast<uint32_t>(index)))
              marker.level = info->level;
          } else if (primaryGeometryLods_.active && index < primaryGeometryLods_.choices.size()) {
            marker.level = primaryGeometryLods_.choices[index].level;
          }
          cell = {.marker = marker, .depth = pos.reverseZDepth};
        }
      }
    }
  };
  collect(frame.scene, frame.draws, false);
  if (subs_.bimManager && subs_.bimManager->hasScene())
    collect(frame.bim.scene, frame.bim.draws, true);
  std::vector<AssetStreamingMarker> markers;
  markers.reserve(cells.size());
  for (const auto& cell : cells) {
    if (cell.marker)
      markers.push_back(*cell.marker);
  }
  subs_.guiManager->drawAssetStreamingOverlay(markers);
}

void RendererFrontend::recordAssetRayBuild(uint64_t previousGeneration) {
  const auto& ray = *subs_.rayShadowManager;
  const auto* stats = ray.buildStats();
  if (!stats || ray.sceneGeneration() == previousGeneration)
    return;
  constexpr size_t historySize = 64;
  if (assetRayBuilds_.size() == historySize)
    assetRayBuilds_.erase(assetRayBuilds_.begin());
  assetRayBuilds_.push_back({
      .frame = frame_.submittedFrameCount,
      .generation = ray.sceneGeneration(),
      .blasBuilt = stats->blasBuilt,
      .blasReused = stats->blasReused,
      .instances = stats->instanceCount,
      .asAllocatedBytes = stats->accelerationAllocatedBytes,
      .inputAllocatedBytes = stats->inputAllocatedBytes,
      .scratchAllocatedBytes = stats->scratchAllocatedBytes,
  });
}

void RendererFrontend::collectAssetRayBuildTiming(uint64_t frame,
                                                  std::span<const RendererPassGpuTiming> timings) {
  const auto sample = std::ranges::find(assetRayBuilds_ | std::views::reverse,
                                         frame, &AssetRayBuildSample::frame);
  if (sample == (assetRayBuilds_ | std::views::reverse).end())
    return;
  const auto pass = std::ranges::find(timings, RenderPassId::RaySceneBuild,
                                    &RendererPassGpuTiming::id);
  if (pass != timings.end())
    sample->gpuMs = pass->milliseconds;
}

void RendererFrontend::writeAssetStreamingTelemetry(nlohmann::json& json) const {
  const bool cacheEnabled = !svc_.config.assetCacheDirectory.empty();
  const auto& cacheReport = primaryGeometryLods_.cacheReport;
  const auto cache = cacheReport.value_or(geometry::GeometryLodCacheStats{});
  const std::string_view geometryStatus = !cacheReport && cacheEnabled &&
      svc_.config.geometryLodErrorPixels > 0.0f && !activePrimaryModelPath_.empty()
      ? "pending" : util::derivedAssetCacheStatusName(cache.status);
  auto& cacheJson = json["assetCache"];
  cacheJson = {{"enabled", cacheEnabled}, {"geometryLod", {
      {"status", geometryStatus}, {"key", cache.key}, {"bytesRead", cache.bytesRead},
      {"bytesWritten", cache.bytesWritten}, {"keyMs", cache.keyMilliseconds},
      {"readMs", cache.readMilliseconds}, {"generationMs", cache.generationMilliseconds},
      {"writeMs", cache.writeMilliseconds},
      {"elapsedMs", cache.keyMilliseconds + cache.readMilliseconds +
                    cache.generationMilliseconds + cache.writeMilliseconds},
  }}};
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const auto& bim = subs_.bimManager->derivedCacheReport();
    cacheJson["bim"] = {{"status", util::derivedAssetCacheStatusName(bim.status)},
                         {"key", bim.key}, {"bytesRead", bim.bytesRead},
                         {"bytesWritten", bim.bytesWritten}, {"elapsedMs", bim.elapsedMs}};
  }
  json["assetStartup"] = {
      {"initialSceneMs", initialSceneMs_},
      {"firstSubmittedMs", firstSubmittedMs_ ? nlohmann::json(*firstSubmittedMs_) : nlohmann::json(nullptr)},
  };
  auto builds = nlohmann::json::array();
  for (const auto& sample : assetRayBuilds_) {
    builds.push_back({{"frame", sample.frame}, {"generation", sample.generation},
        {"blasBuilt", sample.blasBuilt}, {"blasReused", sample.blasReused},
        {"instances", sample.instances}, {"asAllocatedBytes", sample.asAllocatedBytes},
        {"inputAllocatedBytes", sample.inputAllocatedBytes}, {"scratchAllocatedBytes", sample.scratchAllocatedBytes},
        {"gpuMs", sample.gpuMs ? nlohmann::json(*sample.gpuMs) : nlohmann::json(nullptr)}});
  }
  json["raySceneBuildCosts"] = std::move(builds);
}

}  // namespace container::renderer
