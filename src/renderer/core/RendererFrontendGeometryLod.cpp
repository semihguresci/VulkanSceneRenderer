#include "Container/renderer/core/RendererFrontend.h"

#include "Container/geometry/GeometryLod.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/Logger.h"
#include "Container/utility/SceneManager.h"

#include <nlohmann/json.hpp>

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

constexpr size_t maxIndexBufferElements = 128ull * 1024 * 1024 / sizeof(uint32_t);
constexpr size_t maxGenerationSourceIndices = 1'500'000;

} // namespace

void RendererFrontend::clearPrimaryGeometryLods() {
  auto& lod = primaryGeometryLods_;
  if (lod.buffer.buffer != VK_NULL_HANDLE)
    svc_.allocationManager.destroyBuffer(lod.buffer);
  const auto drawRevision = lod.drawRevision;
  lod = {};
  lod.drawRevision = drawRevision;
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
  }
  if (pixelLimit <= 0.0f || !asset || scene.deformationScene() ||
      frame.scene.indexType != VK_INDEX_TYPE_UINT32 || view.viewport.x == 0 || view.viewport.y == 0) {
    lod.choices.clear();
    return;
  }

  if (geometryChanged) {
    // Remember the attempted generation even if the meshes or current asset
    // budget cannot support an additional raster index buffer.
    lod.asset = asset;
    lod.geometryRevision = revision;
    const auto source = scene.gpuBindIndices();
    const auto vertices = scene.gpuBindVertices();
    const auto& primitives = subs_.sceneManager->primitiveRanges();
    if (source.size() >= maxIndexBufferElements)
      return;
    try {
      std::vector<PrimaryGeometryLods::Primitive> ranges(primitives.size());
      std::vector<uint32_t> indices;
      size_t generationBudget = maxGenerationSourceIndices;
      for (size_t i = 0; i < primitives.size(); ++i) {
        const auto& primitive = primitives[i];
        const auto first = size_t(primitive.firstIndex);
        const auto count = size_t(primitive.indexCount);
        if (first > source.size() || count > source.size() - first || count > generationBudget)
          continue;
        generationBudget -= count;
        auto levels = geometry::buildTriangleLods(vertices, source.subspan(first, count));
        if (levels.empty())
          continue;
        if (indices.empty())
          indices.assign(source.begin(), source.end());
        auto& range = ranges[i];
        range.firstIndex = primitive.firstIndex;
        range.indexCount = primitive.indexCount;
        for (const auto& level : levels) {
          if (level.indices.size() > maxIndexBufferElements - indices.size())
            break;
          range.levels.push_back({static_cast<uint32_t>(indices.size()),
                                  static_cast<uint32_t>(level.indices.size()), level.maxError});
          indices.insert(indices.end(), level.indices.begin(), level.indices.end());
        }
      }
      if (indices.size() > source.size()) {
        const auto bytes = std::as_bytes(std::span(indices));
        lod.buffer = svc_.allocationManager.uploadBuffer(bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        lod.indexBytes = bytes.size();
        lod.ranges = std::move(ranges);
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
    const auto level = primitive < lod.ranges.size() ? geometry::selectGeometryLod(
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

void RendererFrontend::writePrimaryGeometryLodTelemetry(nlohmann::json& json) const {
  const auto& lod = primaryGeometryLods_;
  uint64_t baseTriangles = 0;
  uint64_t triangles = 0;
  std::array<uint64_t, 3> levels{};
  if (subs_.sceneController) {
    const auto collect = [&](const std::vector<DrawCommand>& source, size_t list) {
      const auto& draws = lod.active ? lod.drawLists[list] : source;
      for (size_t i = 0; i < source.size(); ++i) {
        const auto& original = source[i];
        const auto& draw = draws[i];
        if (original.objectIndex == sceneState_.diagCubeObjectIndex)
          continue;
        baseTriangles += uint64_t(original.indexCount / 3) * original.instanceCount;
        triangles += uint64_t(draw.indexCount / 3) * draw.instanceCount;
        const auto level = draw.firstIndex == original.firstIndex ? 0u : lod.choices[draw.objectIndex].level;
        levels[level] += draw.instanceCount;
      }
    };
    collect(subs_.sceneController->opaqueDrawCommands(), 0);
    collect(subs_.sceneController->transparentDrawCommands(), 1);
  }
  json["geometryLod"]["errorPixels"] = svc_.config.geometryLodErrorPixels;
  json["geometryLod"]["primary"] = {{"baseTriangles", baseTriangles}, {"triangles", triangles},
                                    {"extraIndexBytes", lod.indexBytes}, {"levels", levels}};
}

} // namespace container::renderer
