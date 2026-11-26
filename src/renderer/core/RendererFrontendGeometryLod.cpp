#include "Container/renderer/core/RendererFrontend.h"

#include "Container/app/AppConfig.h"
#include "Container/geometry/GeometryLodCache.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/AsyncModelPreparation.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/GeometryUploadBatch.h"
#include "Container/utility/Logger.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/VulkanDevice.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace container::renderer {
namespace {

constexpr std::array drawListMembers{
    &FrameDrawLists::opaqueDrawCommands,
    &FrameDrawLists::transparentDrawCommands,
    &FrameDrawLists::opaqueSingleSidedDrawCommands,
    &FrameDrawLists::opaqueWindingFlippedDrawCommands,
    &FrameDrawLists::opaqueDoubleSidedDrawCommands,
    &FrameDrawLists::transparentSingleSidedDrawCommands,
    &FrameDrawLists::transparentWindingFlippedDrawCommands,
    &FrameDrawLists::transparentDoubleSidedDrawCommands,
    &FrameDrawLists::hoveredDrawCommands,
    &FrameDrawLists::selectedDrawCommands,
};

constexpr size_t maxIndexBufferElements = geometry::GeometryLodPreparationOptions{}.maxBufferIndexCount;
constexpr size_t maxSnapshotBytes = 192ull * 1024 * 1024;
constexpr size_t snapshotBytesPerFrame = 2ull * 1024 * 1024;

} // namespace

struct RendererFrontend::PrimaryGeometryLods::Streaming {
  struct Snapshot {
    std::vector<geometry::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<geometry::PrimitiveRange> primitives;

    bool copyNext(std::span<const geometry::Vertex> liveVertices,
                  std::span<const uint32_t> liveIndices,
                  std::span<const geometry::PrimitiveRange> livePrimitives) {
      size_t budget = snapshotBytesPerFrame;
      const auto append = [&budget]<class T>(std::vector<T>& copy, std::span<const T> live) {
        const auto count = std::min(live.size() - copy.size(), budget / sizeof(T));
        const auto part = live.subspan(copy.size(), count);
        copy.insert(copy.end(), part.begin(), part.end());
        budget -= part.size_bytes();
        return copy.size() == live.size();
      };
      return append(vertices, liveVertices) && append(indices, liveIndices) &&
             append(primitives, livePrimitives);
    }
  };

  using Prepared = geometry::PreparedGeometryLods;
  util::AsyncModelPreparation<std::unique_ptr<Prepared>> worker;
  std::unique_ptr<Snapshot> snapshot;
  uint64_t requestId{};
  std::unique_ptr<Prepared> prepared;
  std::unique_ptr<gpu::GeometryUploadBatch> upload;
  scene::SceneAssetId uploadAsset;
  uint64_t uploadRevision{};
  gpu::GeometryUploadProgress progress{};
  VkDeviceSize maxBytesPerTick{};
  VkDeviceSize peakStagingBytes{};
  uint64_t submittedChunks{};
  uint64_t staleCancellations{};
  uint64_t uploadPreemptions{};
  uint64_t publications{};
};

void RendererFrontend::PrimaryGeometryLods::StreamingDeleter::operator()(
    Streaming* state) const noexcept {
  delete state;
}

void RendererFrontend::clearPrimaryGeometryLods() {
  auto& lod = primaryGeometryLods_;
  if (auto* stream = lod.streaming.get()) {
    if (stream->snapshot || stream->requestId || stream->prepared)
      ++stream->staleCancellations;
    if (stream->upload)
      stream->upload->cancel();
    stream->worker.cancel();
    stream->snapshot.reset();
    stream->requestId = 0;
    if (stream->prepared)
      stream->worker.discard(std::move(stream->prepared));
    stream->progress = {};
    stream->maxBytesPerTick = 0;
    stream->peakStagingBytes = 0;
    stream->submittedChunks = 0;
  }
  if (lod.buffer.buffer != VK_NULL_HANDLE)
    svc_.allocationManager.destroyBuffer(lod.buffer);
  lod.asset = {};
  lod.geometryRevision = 0;
  lod.indexBytes = 0;
  lod.ranges.clear();
  lod.cacheReport.reset();
  lod.choices.clear();
  lod.attempted = false;
  lod.active = false;
}

void RendererFrontend::shutdownPrimaryGeometryLods() {
  clearPrimaryGeometryLods();
  primaryGeometryLods_.streaming.reset();
}

bool RendererFrontend::retirePrimaryGeometryLodUploadForModelLoad() {
  auto* stream = primaryGeometryLods_.streaming.get();
  if (!stream || !stream->upload)
    return true;
  auto& upload = *stream->upload;
  if (upload.state() == gpu::GeometryUploadState::Uploading || upload.ready()) {
    upload.cancel();
    ++stream->uploadPreemptions;
    stream->progress = {};
    stream->maxBytesPerTick = 0;
    stream->peakStagingBytes = 0;
    stream->submittedChunks = 0;
  }
  (void)upload.tick(0);
  return upload.state() == gpu::GeometryUploadState::Idle;
}

void RendererFrontend::updatePrimaryGeometryLodStreaming(bool allowWork) {
  auto& lod = primaryGeometryLods_;
  allowWork = allowWork && !modelLoadPending();
  if (!lod.streaming && (!allowWork || lod.attempted))
    return;
  try {
    if (!lod.streaming)
      lod.streaming.reset(new PrimaryGeometryLods::Streaming);
    auto& stream = *lod.streaming;
    if (stream.upload && (!allowWork || stream.upload->state() == gpu::GeometryUploadState::Retiring))
      (void)stream.upload->tick(0);
    if (!allowWork || (stream.upload && stream.upload->state() == gpu::GeometryUploadState::Retiring))
      return;

    if (!lod.attempted || stream.snapshot) {
      const auto& scene = *subs_.sceneController;
      const auto vertices = scene.gpuBindVertices();
      const auto indices = scene.gpuBindIndices();
      const std::span primitives(subs_.sceneManager->primitiveRanges());
      if (!lod.attempted) {
        lod.attempted = true;
        if (indices.size() >= maxIndexBufferElements ||
            vertices.size_bytes() > maxSnapshotBytes - indices.size_bytes() ||
            primitives.size_bytes() > maxSnapshotBytes - indices.size_bytes() - vertices.size_bytes() ||
            std::ranges::none_of(primitives, [](const auto& primitive) {
              return primitive.indexCount >= 96 * 3 && primitive.indexCount % 3 == 0;
            })) {
          if (!svc_.config.assetCacheDirectory.empty())
            lod.cacheReport.emplace().status = util::DerivedAssetCacheStatus::NotCacheable;
          return;
        }
        stream.snapshot = std::make_unique<PrimaryGeometryLods::Streaming::Snapshot>();
        stream.snapshot->vertices.reserve(vertices.size());
        stream.snapshot->indices.reserve(indices.size());
        stream.snapshot->primitives.reserve(primitives.size());
      }
      if (!stream.snapshot->copyNext(vertices, indices, primitives))
        return;
      stream.requestId = stream.worker.start(
          [snapshot = std::move(stream.snapshot),
           cacheDirectory = svc_.config.assetCacheDirectory](std::stop_token stop) {
            return std::make_unique<geometry::PreparedGeometryLods>(geometry::prepareGeometryLods(
                snapshot->vertices, snapshot->indices, snapshot->primitives, cacheDirectory, {}, stop));
          });
      return;
    }

    if (stream.requestId) {
      auto completion = stream.worker.tryTake();
      if (!completion || completion->requestId != stream.requestId)
        return;
      stream.requestId = 0;
      if (const auto* error = std::get_if<std::exception_ptr>(&completion->outcome))
        std::rethrow_exception(*error);
      stream.prepared = std::move(std::get<0>(completion->outcome));
      lod.cacheReport = stream.prepared->cache;
      if (stream.prepared->indices.empty())
        stream.worker.discard(std::move(stream.prepared));
    }
    if (!stream.prepared)
      return;

    if (!stream.upload) {
      const auto& device = *svc_.ctx.deviceWrapper;
      stream.upload = std::make_unique<gpu::GeometryUploadBatch>(
          *svc_.allocationManager.memoryManager(), device.device(), device.graphicsQueue(),
          device.queueFamilyIndices().graphicsFamily.value());
    }
    auto& upload = *stream.upload;
    if (upload.state() == gpu::GeometryUploadState::Idle) {
      auto chunkBytes = VkDeviceSize(svc_.config.geometryUploadChunkKiB) * 1024u;
      const auto budget = svc_.allocationManager.sceneAssetBudget();
      if (budget.limits.uploadBytes)
        chunkBytes = std::min(chunkBytes, budget.limits.uploadBytes - budget.used.uploadBytes);
      const std::array sources{gpu::GeometryUploadSource{
          std::as_bytes(std::span(stream.prepared->indices)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT}};
      upload.begin(sources, chunkBytes);
      stream.uploadAsset = lod.asset;
      stream.uploadRevision = lod.geometryRevision;
    }
    const auto submitted = upload.tick();
    stream.progress = upload.progress();
    stream.maxBytesPerTick = std::max(stream.maxBytesPerTick, submitted);
    stream.peakStagingBytes = std::max(stream.peakStagingBytes, stream.progress.stagingBytes);
    if (submitted)
      ++stream.submittedChunks;
    if (!upload.ready())
      return;

    auto result = upload.takeReady();
    lod.buffer = result.release(0);
    lod.indexBytes = result.targets()[0].logicalSize;
    lod.ranges = std::move(stream.prepared->ranges);
    ++stream.publications;
    stream.worker.discard(std::move(stream.prepared));
  } catch (const std::exception& error) {
    auto* stream = lod.streaming.get();
    lod.attempted = true;
    if (stream) {
      if (stream->upload)
        stream->upload->cancel();
      stream->snapshot.reset();
      stream->requestId = 0;
      stream->prepared.reset();
    }
    container::log::ContainerLogger::instance().renderer()->warn(
        "Using full primary geometry because optional LOD streaming failed: {}", error.what());
  }
}

void RendererFrontend::applyPrimaryGeometryLods(FrameRecordParams& frame,
                                                const geometry::GeometryLodView& view,
                                                float pixelLimit) {
  if (svc_.config.geometryLodErrorPixels <= 0.0f)
    return;

  const auto& scene = *subs_.sceneController;
  auto& lod = primaryGeometryLods_;
  lod.active = false;
  const auto asset = scene.geometryAssetId();
  const auto revision = scene.geometryRevision();
  const bool geometryChanged = lod.asset != asset || lod.geometryRevision != revision;
  if (geometryChanged) {
    if (lod.buffer.buffer != VK_NULL_HANDLE)
      subs_.frameSyncManager->waitForAllFrames();
    clearPrimaryGeometryLods();
    lod.asset = asset;
    lod.geometryRevision = revision;
  }
  if (pixelLimit <= 0.0f || !asset || scene.deformationScene() ||
      frame.scene.indexType != VK_INDEX_TYPE_UINT32 || view.viewport.x == 0 || view.viewport.y == 0) {
    lod.choices.clear();
    if (lod.streaming)
      updatePrimaryGeometryLodStreaming(false);
    return;
  }

  if (svc_.config.geometryUploadChunkKiB) {
    updatePrimaryGeometryLodStreaming(true);
  } else if (!lod.attempted) {
    lod.attempted = true;
    try {
      auto result = geometry::prepareGeometryLods(scene.gpuBindVertices(), scene.gpuBindIndices(),
                                                  subs_.sceneManager->primitiveRanges(),
                                                  svc_.config.assetCacheDirectory);
      lod.cacheReport = std::move(result.cache);
      if (!result.indices.empty()) {
        const auto bytes = std::as_bytes(std::span(result.indices));
        lod.buffer = svc_.allocationManager.uploadBuffer(bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        lod.indexBytes = bytes.size();
        lod.ranges = std::move(result.ranges);
      }
    } catch (const std::exception& error) {
      container::log::ContainerLogger::instance().renderer()->warn(
          "Using full primary geometry because optional LOD preparation failed: {}", error.what());
      return;
    }
  }

  if (lod.buffer.buffer == VK_NULL_HANDLE)
    return;

  const auto& objects = *frame.scene.objectData;
  lod.choices.resize(objects.size());
  bool anyCoarse = false;
  for (size_t i = 0; i < objects.size(); ++i) {
    const auto& object = objects[i];
    auto& choice = lod.choices[i];
    const auto node = object.temporalInfo.z;
    const auto primitive = object.temporalInfo.w;
    const auto previous = choice.node == node && choice.primitive == primitive ? choice.level : 0u;
    // LOD errors are measured on bind vertices. A height map can make a
    // removed vertex's raster position differ by more than that error.
    const bool eligible = primitive < lod.ranges.size() &&
                          subs_.sceneManager->rayGeometryCompatible(object.objectInfo.x);
    const auto level = eligible ? geometry::selectGeometryLod(
        lod.ranges[primitive].levels, view, object.boundingSphere, object.model, pixelLimit, previous) : 0u;
    choice = {node, primitive, level};
    anyCoarse |= level != 0u;
  }
  if (!anyCoarse)
    return;

  bool drawListsChanged = false;
  bool anyRemapped = false;
  for (size_t i = 0; i < drawListMembers.size(); ++i) {
    auto& draws = frame.draws.*drawListMembers[i];
    if (!draws)
      continue;
    auto& remapped = lod.drawLists[i];
    drawListsChanged |= remapped.size() != draws->size();
    remapped.resize(draws->size());
    for (size_t j = 0; j < draws->size(); ++j) {
      auto draw = (*draws)[j];
      if (draw.instanceCount == 1 && draw.objectIndex < lod.choices.size()) {
        const auto& choice = lod.choices[draw.objectIndex];
        if (choice.level > 0) {
          const auto& range = lod.ranges[choice.primitive];
          if (draw.firstIndex == range.firstIndex && draw.indexCount == range.indexCount) {
            const auto& reduced = range.levels[choice.level - 1];
            draw.firstIndex = reduced.firstIndex;
            draw.indexCount = reduced.indexCount;
            anyRemapped = true;
          }
        }
      }
      const auto& last = remapped[j];
      drawListsChanged |= last.objectIndex != draw.objectIndex || last.firstIndex != draw.firstIndex ||
                          last.indexCount != draw.indexCount || last.instanceCount != draw.instanceCount;
      remapped[j] = draw;
    }
  }
  if (!anyRemapped)
    return;

  for (size_t i = 0; i < drawListMembers.size(); ++i) {
    auto& draws = frame.draws.*drawListMembers[i];
    if (draws)
      draws = &lod.drawLists[i];
  }
  if (drawListsChanged)
    ++lod.drawRevision;
  frame.draws.sourceRevision = lod.drawRevision;
  frame.scene.indexSlice = {lod.buffer.buffer, 0, lod.indexBytes};
  lod.active = true;
}

void RendererFrontend::addPrimaryGeometryDiagnostics(AssetStreamingSummary& summary) const {
  const auto& lod = primaryGeometryLods_;
  if (subs_.sceneController) {
    const auto collect = [&](const std::vector<DrawCommand>& source, size_t list) {
      const auto& draws = lod.active ? lod.drawLists[list] : source;
      for (size_t i = 0; i < source.size(); ++i) {
        const auto& original = source[i];
        const auto& draw = draws[i];
        if (original.objectIndex == sceneState_.diagCubeObjectIndex)
          continue;
        summary.baseTriangles += uint64_t(original.indexCount / 3) * original.instanceCount;
        summary.submittedTriangles += uint64_t(draw.indexCount / 3) * draw.instanceCount;
        const auto level = draw.firstIndex == original.firstIndex ? 0u : lod.choices[draw.objectIndex].level;
        summary.geometryLevels[level] += draw.instanceCount;
      }
    };
    collect(subs_.sceneController->opaqueDrawCommands(), 0);
    collect(subs_.sceneController->transparentDrawCommands(), 1);
  }
  summary.geometryIndexBytes += lod.indexBytes;
  if (const auto* stream = lod.streaming.get()) {
    summary.geometryPendingBytes += stream->progress.totalBytes - stream->progress.completedBytes;
    summary.geometryPeakCopyBytes = stream->maxBytesPerTick;
  }
}

void RendererFrontend::writePrimaryGeometryLodTelemetry(nlohmann::json& json) const {
  const auto& lod = primaryGeometryLods_;
  AssetStreamingSummary summary;
  addPrimaryGeometryDiagnostics(summary);
  json["geometryLod"]["errorPixels"] = svc_.config.geometryLodErrorPixels;
  json["geometryLod"]["primary"] = {
      {"baseTriangles", summary.baseTriangles}, {"triangles", summary.submittedTriangles},
      {"extraIndexBytes", summary.geometryIndexBytes}, {"levels", summary.geometryLevels}};
  const auto* stream = lod.streaming.get();
  const auto uploadState = stream && stream->upload ? stream->upload->state()
                                                   : gpu::GeometryUploadState::Idle;
  const char* state = svc_.config.geometryUploadChunkKiB && svc_.config.geometryLodErrorPixels > 0.0f
                          ? "idle" : "disabled";
  if (stream) {
    if (uploadState == gpu::GeometryUploadState::Retiring)
      state = "retiring";
    else if (lod.buffer.buffer != VK_NULL_HANDLE)
      state = "ready";
    else if (stream->snapshot)
      state = "snapshot";
    else if (stream->requestId)
      state = "preparing";
    else if (stream->prepared)
      state = uploadState == gpu::GeometryUploadState::Uploading ? "uploading" : "prepared";
    else if (lod.attempted)
      state = "fallback";
  }
  const auto progress = stream ? stream->progress : gpu::GeometryUploadProgress{};
  const bool retiring = uploadState == gpu::GeometryUploadState::Retiring;
  json["geometryStreaming"]["primaryLod"] = {
      {"state", state}, {"assetId", retiring ? stream->uploadAsset.value() : lod.asset.value()},
      {"geometryRevision", retiring ? stream->uploadRevision : lod.geometryRevision},
      {"totalBytes", progress.totalBytes}, {"submittedBytes", progress.submittedBytes},
      {"completedBytes", progress.completedBytes}, {"submittedChunks", stream ? stream->submittedChunks : 0u},
      {"maxBytesPerTick", stream ? stream->maxBytesPerTick : 0u},
      {"peakStagingBytes", stream ? stream->peakStagingBytes : 0u},
      {"staleCancellations", stream ? stream->staleCancellations : 0u},
      {"uploadPreemptions", stream ? stream->uploadPreemptions : 0u},
      {"publications", stream ? stream->publications : 0u}};
}

} // namespace container::renderer
