#pragma once

#include "Container/geometry/DotBimLoader.h"
#include "Container/renderer/bim/BimMetadata.h"
#include "Container/renderer/bim/BimRelationshipGraph.h"
#include "Container/renderer/lighting/DdgiBimRoomPlan.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/utility/SceneData.h"

#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace container::renderer {

class BimManager;

struct BimDdgiRoomSource {
  BimMetadataTableView metadata;
  std::span<const container::geometry::Vertex> vertices;
  std::span<const uint32_t> indices;
  std::span<const gpu::ObjectData> objects;
  std::span<const DrawCommand> opaqueDraws;
  std::span<const DrawCommand> transparentDraws;
  std::span<const BimRelationshipNode> relationshipNodes;
  std::span<const BimRelationshipEdge> relationships;
  std::span<const geometry::dotbim::SpaceWithoutDrawableGeometry> missingSpaces;
};

struct BimDdgiRoomExtraction {
  std::vector<DdgiBimRoom> rooms;
  std::string sourceUnits;
  std::optional<float> metersPerSourceUnit;
  std::optional<float> effectiveImportScale;
  size_t missingRooms{};
  size_t ambiguousRooms{};
};

// Unfiltered triangle geometry in current world coordinates. The result owns
// its strings and bounds and can be passed to a worker after model replacement.
[[nodiscard]] BimDdgiRoomExtraction ExtractBimDdgiRooms(
    const BimManager &model, std::stop_token stop = {});
[[nodiscard]] BimDdgiRoomExtraction ExtractBimDdgiRooms(
    const BimDdgiRoomSource &source, std::stop_token stop = {});

} // namespace container::renderer
