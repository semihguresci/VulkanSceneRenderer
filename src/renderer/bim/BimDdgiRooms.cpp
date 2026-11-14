#include "Container/renderer/bim/BimDdgiRooms.h"

#include "Container/renderer/bim/BimManager.h"
#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace container::renderer {
namespace {

constexpr size_t kMaxRoomIndexVisits = 24'000'000;
constexpr size_t kMaxRoomFragments = 256'000;

bool equalAscii(std::string_view text, std::string_view lower) {
  return std::ranges::equal(text, lower, [](unsigned char a, unsigned char b) {
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    return a == b;
  });
}

bool roomType(std::string_view type) {
  return equalAscii(type, "ifcspace") || equalAscii(type, "space") ||
         equalAscii(type, "room") || equalAscii(type, "ifcroom");
}

bool storeyType(std::string_view type) {
  return equalAscii(type, "ifcbuildingstorey") || equalAscii(type, "buildingstorey") ||
         equalAscii(type, "storey");
}

bool finite(glm::vec3 point) {
  return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

void include(DdgiSceneBounds &bounds, glm::vec3 point) {
  if (!bounds.valid) {
    bounds = {point, point, true};
  } else {
    bounds.minimum = glm::min(bounds.minimum, point);
    bounds.maximum = glm::max(bounds.maximum, point);
  }
}

struct RoomRecord {
  DdgiBimRoom room;
  std::string storeyId, storeyName;
  std::string missingReason;
  bool storeyConflict{};
  bool invalidFragment{};
  bool hasMeshFragment{};
};

void assignStorey(RoomRecord &record, std::string_view id, std::string_view name) {
  if (!record.storeyId.empty() && !id.empty() && id != record.storeyId) {
    record.storeyConflict = true;
  }
  // Equal names do not identify distinct storeys when both have stable IDs.
  // Conversely, a shorter label cannot override the evidence from matching IDs.
  if ((id.empty() || record.storeyId.empty()) && !name.empty() &&
      !record.storeyName.empty() && name != record.storeyName) {
    record.storeyConflict = true;
  }
  if (record.storeyId.empty()) record.storeyId = id;
  if (record.storeyName.empty()) record.storeyName = name;
}

DdgiSceneBounds fragmentBounds(const BimDdgiRoomSource &source,
                              const DrawCommand &draw, uint32_t object,
                              size_t &remaining, std::stop_token stop) {
  DdgiSceneBounds bounds;
  if (draw.indexCount < 3 || draw.indexCount % 3 != 0 ||
      draw.firstIndex > source.indices.size() ||
      draw.indexCount > source.indices.size() - draw.firstIndex ||
      object >= source.objects.size()) return bounds;
  if (draw.indexCount > remaining) {
    throw std::length_error("BIM room geometry exceeds the DDGI index scan limit");
  }
  remaining -= draw.indexCount;
  const glm::dmat4 model(source.objects[object].model);
  if (model[0].w != 0.0 || model[1].w != 0.0 || model[2].w != 0.0 ||
      model[3].w != 1.0) return bounds;
  for (size_t offset = 0; offset < draw.indexCount; ++offset) {
    if ((offset % 4096) == 0) container::util::checkModelImportCancellation(stop);
    const auto vertex = source.indices[size_t(draw.firstIndex) + offset];
    if (vertex >= source.vertices.size()) return {};
    const glm::vec3 point(model * glm::dvec4(source.vertices[vertex].position, 1.0));
    if (!finite(point)) return {};
    include(bounds, point);
  }
  return bounds;
}

} // namespace

BimDdgiRoomExtraction ExtractBimDdgiRooms(const BimDdgiRoomSource &source,
                                         std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  std::map<std::string, RoomRecord> records;
  const auto addRoom = [&](std::string_view guid, std::string_view sourceId,
                           std::string_view name, uint32_t fallback,
                           std::string_view fallbackPrefix) -> RoomRecord & {
    const std::string_view id = guid.empty() ? sourceId : guid;
    const std::string key = !guid.empty() ? "guid:" + std::string(guid)
        : !sourceId.empty() ? "source:" + std::string(sourceId)
                            : std::string(fallbackPrefix) + std::to_string(fallback);
    auto [it, inserted] = records.try_emplace(key);
    if (records.size() > kMaxDdgiBimRooms) {
      throw std::length_error("BIM room count exceeds the DDGI extraction limit");
    }
    auto &record = it->second;
    if (inserted) {
      record.room.identity = id.empty() ? key : std::string(id);
      record.room.label = name.empty() ? record.room.identity : std::string(name);
      if (id.empty()) {
        record.room.ambiguous = true;
        record.room.reason = "Room has no stable product identity";
      }
    } else if (!name.empty() && record.room.label == record.room.identity) {
      record.room.label = name;
    }
    return record;
  };

  struct Fragment {
    RoomRecord *record;
    BimGeometryKind kind;
    bool knownStorey;
  };
  std::unordered_map<uint32_t, Fragment> fragments;
  for (const auto element : source.metadata) {
    container::util::checkModelImportCancellation(stop);
    if (!roomType(element.type)) continue;
    auto &record = addRoom(element.guid, element.sourceId, element.displayName,
                           element.objectIndex, "object:");
    assignStorey(record, element.storeyId, element.storeyName);
    record.storeyConflict |= std::ranges::any_of(element.properties, [](const auto &property) {
      return property.set == "Spatial containment" && property.name == "Storey" &&
             property.value == "Conflicting storey references";
    });
    if (element.geometryKind != BimGeometryKind::Mesh && record.missingReason.empty()) {
      record.missingReason = "Room only has point or curve geometry";
    }
    const auto [it, inserted] = fragments.try_emplace(element.objectIndex,
        Fragment{&record, element.geometryKind, !element.storeyId.empty() || !element.storeyName.empty()});
    if (fragments.size() > kMaxRoomFragments) {
      throw std::length_error("BIM room fragments exceed the DDGI extraction limit");
    }
    if (!inserted && it->second.record != &record) {
      record.room.ambiguous = it->second.record->room.ambiguous = true;
      record.room.reason = it->second.record->room.reason = "Room has conflicting geometry ownership";
    }
  }
  uint32_t missingIndex = 0;
  for (const auto &missing : source.missingSpaces) {
    container::util::checkModelImportCancellation(stop);
    auto &record = addRoom(missing.guid, missing.sourceId, missing.name, missingIndex++, "missing:");
    assignStorey(record, missing.storeyId, missing.storeyName);
    record.storeyConflict |= missing.ambiguousStorey;
    record.missingReason = missing.reason;
  }
  if (!fragments.empty()) {
    for (const auto &edge : source.relationships) {
      container::util::checkModelImportCancellation(stop);
      if (edge.kind != BimRelationshipKind::SpatialParent ||
          edge.from >= source.relationshipNodes.size() || edge.to >= source.relationshipNodes.size()) continue;
      const auto &parent = source.relationshipNodes[edge.from];
      const auto &child = source.relationshipNodes[edge.to];
      if (!storeyType(parent.ifcClass)) continue;
      const auto room = fragments.find(child.objectIndex);
      if (room == fragments.end() || room->second.knownStorey) continue;
      assignStorey(*room->second.record, parent.guid.empty() ? parent.sourceId : parent.guid,
                   parent.label);
    }

    size_t remainingVisits = kMaxRoomIndexVisits;
    const auto readDraws = [&](std::span<const DrawCommand> draws) {
      for (const DrawCommand &draw : draws) {
        container::util::checkModelImportCancellation(stop);
        if (draw.objectIndex >= source.objects.size()) continue;
        const size_t count = std::min<size_t>(draw.instanceCount, source.objects.size() - draw.objectIndex);
        for (size_t i = 0; i < count; ++i) {
          if ((i % 1024) == 0) container::util::checkModelImportCancellation(stop);
          const auto object = uint32_t(size_t(draw.objectIndex) + i);
          const auto fragment = fragments.find(object);
          if (fragment == fragments.end() || fragment->second.kind != BimGeometryKind::Mesh) continue;
          auto &record = *fragment->second.record;
          record.hasMeshFragment = true;
          if (const auto bounds = fragmentBounds(source, draw, object, remainingVisits, stop); bounds.valid) {
            include(record.room.bounds, bounds.minimum);
            include(record.room.bounds, bounds.maximum);
          } else {
            record.invalidFragment = true;
          }
        }
      }
    };
    readDraws(source.opaqueDraws);
    readDraws(source.transparentDraws);
  }

  BimDdgiRoomExtraction result;
  result.rooms.reserve(records.size());
  for (auto &[key, record] : records) {
    (void)key;
    container::util::checkModelImportCancellation(stop);
    auto &room = record.room;
    room.storey = record.storeyName.empty() ? record.storeyId : std::move(record.storeyName);
    room.storeyIdentity = std::move(record.storeyId);
    if (record.storeyConflict) {
      room.ambiguous = true;
      room.reason = "Room has conflicting storey references";
    } else if (record.invalidFragment && room.bounds.valid) {
      room.ambiguous = true;
      room.reason = "Room includes an invalid triangle fragment";
    } else if (!room.ambiguous && (!room.bounds.valid ||
               !glm::all(glm::greaterThan(room.bounds.maximum, room.bounds.minimum)))) {
      room.bounds = {};
      room.reason = record.missingReason.empty() || record.hasMeshFragment
                        ? "Room has no usable three-dimensional triangle bounds"
                        : std::move(record.missingReason);
    }
    result.ambiguousRooms += room.ambiguous;
    result.missingRooms += !room.ambiguous && !room.bounds.valid;
    result.rooms.push_back(std::move(room));
  }
  return result;
}

BimDdgiRoomExtraction ExtractBimDdgiRooms(const BimManager &model, std::stop_token stop) {
  const auto &graph = model.relationshipGraph();
  auto result = ExtractBimDdgiRooms({model.elementMetadata(), model.vertices(), model.indices(),
      model.objectData(), model.opaqueDrawCommands(), model.transparentDrawCommands(),
      graph.nodes(), graph.edges(), model.spacesWithoutDrawableGeometry()}, stop);
  const auto &units = model.modelUnitMetadata();
  if (units.hasSourceUnits) result.sourceUnits = units.sourceUnits;
  if (units.hasMetersPerUnit) result.metersPerSourceUnit = units.metersPerUnit;
  if (units.hasEffectiveImportScale) result.effectiveImportScale = units.effectiveImportScale;
  return result;
}

} // namespace container::renderer
