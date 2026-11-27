#include "BimAssetCacheCodec.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace container::robotics::asset_cache {
namespace {

constexpr size_t maxColliders = 32'768;
constexpr size_t maxTriangles = 4'000'000;
constexpr size_t maxElements = 3 * maxTriangles;
constexpr size_t maxDiagnostics = 1'000'000;
constexpr size_t maxTextBytes = 64u * 1024u * 1024u;
constexpr size_t maxTextLength = 64u * 1024u;
constexpr size_t maxSurfaceCandidateTests = 32'000'000;
constexpr uint32_t noMesh = std::numeric_limits<uint32_t>::max();

bool finite(glm::dvec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

struct TextBudget {
  size_t bytes{};

  void accept(std::string_view value) {
    require(value.size() <= maxTextLength && value.size() <= maxTextBytes - bytes);
    bytes += value.size();
  }

  void write(Writer& writer, std::string_view value) {
    accept(value);
    writer.text(value);
  }

  std::string read(Reader& reader) {
    auto result = reader.text();
    accept(result);
    return result;
  }
};

void writePoint(Writer& writer, glm::dvec3 value) {
  writer.f64(value.x); writer.f64(value.y); writer.f64(value.z);
}

void writeVector(Writer& writer, glm::vec3 value) {
  writer.f32(value.x); writer.f32(value.y); writer.f32(value.z);
}

glm::dvec3 readPoint(Reader& reader) {
  return {reader.f64(), reader.f64(), reader.f64()};
}

glm::vec3 readVector(Reader& reader) {
  return {reader.f32(), reader.f32(), reader.f32()};
}

void validateMesh(const BimCollisionMesh& mesh) {
  require(mesh.vertices.size() >= 3 && mesh.vertices.size() <= maxElements &&
          !mesh.indices.empty() && mesh.indices.size() <= maxElements &&
          mesh.indices.size() % 3 == 0);
  for (const auto vertex : mesh.vertices)
    require(finite(glm::dvec3(vertex)));
  for (size_t index = 0; index < mesh.indices.size(); index += 3) {
    const auto a = mesh.indices[index], b = mesh.indices[index + 1], c = mesh.indices[index + 2];
    require(a < mesh.vertices.size() && b < mesh.vertices.size() && c < mesh.vertices.size());
    const auto normal = glm::cross(glm::dvec3(mesh.vertices[b]) - glm::dvec3(mesh.vertices[a]),
                                    glm::dvec3(mesh.vertices[c]) - glm::dvec3(mesh.vertices[a]));
    const double area = glm::length(normal);
    require(std::isfinite(area) && area > 0);
  }
}

void validateRow(const BimCollider& row) {
  require(!row.entityId.empty() && finite(row.originMeters) && finite(glm::dvec3(row.halfExtents)));
  switch (row.kind) {
  case BimColliderKind::StaticMesh:
  case BimColliderKind::DoorMesh:
    require(row.mesh && !row.representationId.empty() && row.halfExtents == glm::vec3(0));
    require(row.kind != BimColliderKind::DoorMesh || !row.supportCandidate);
    for (const auto vertex : row.mesh->vertices)
      require(finite(row.originMeters + glm::dvec3(vertex)));
    break;
  case BimColliderKind::DynamicBox:
    require(!row.mesh && row.representationId.empty() && row.sourceMeshId == 0 &&
            !row.supportCandidate && glm::all(glm::greaterThan(row.halfExtents, glm::vec3(0))));
    require(finite(row.originMeters - glm::dvec3(row.halfExtents)) &&
            finite(row.originMeters + glm::dvec3(row.halfExtents)));
    break;
  default:
    require(false);
  }
}

std::string upper(std::string value) {
  for (auto& character : value)
    if (character >= 'a' && character <= 'z')
      character = static_cast<char>(character - 'a' + 'A');
  return value;
}

bool supportClass(const SemanticEntity& entity) {
  const auto type = upper(entity.ifcClass);
  return entity.kind == SemanticKind::Slab || type == "IFCRAMP" || type == "IFCRAMPFLIGHT" ||
         type == "IFCSTAIR" || type == "IFCSTAIRFLIGHT";
}

bool furnishingClass(const SemanticEntity& entity) {
  const auto type = upper(entity.ifcClass);
  return type == "IFCFURNISHINGELEMENT" || type == "IFCFURNITURE" || type == "IFCSYSTEMFURNITUREELEMENT";
}

using Entities = std::map<std::string_view, const SemanticEntity*, std::less<>>;

void validateIdentity(const BimCollider& row, const Entities& entities) {
  const auto found = entities.find(row.entityId);
  require(found != entities.end());
  const auto& entity = *found->second;
  if (row.kind == BimColliderKind::DynamicBox) {
    require(furnishingClass(entity));
    return;
  }
  require((row.kind == BimColliderKind::DoorMesh) == (entity.kind == SemanticKind::Door));
  require(entity.kind == SemanticKind::Wall || entity.kind == SemanticKind::Slab ||
          entity.kind == SemanticKind::Door || entity.kind == SemanticKind::Other);
  require(row.supportCandidate == (row.kind == BimColliderKind::StaticMesh && supportClass(entity)));
  require(std::ranges::any_of(entity.representations, [&](const auto& representation) {
    return representation.sourceId == row.representationId && representation.meshId == row.sourceMeshId &&
           representation.geometryKind == SemanticGeometryKind::Mesh && representation.roboticsWorldFromLocal;
  }));
}

struct Row {
  const BimCollider* collider;
  uint32_t meshIndex;
};

BimWalkableSurface surfaceAt(const BimCollider& row, uint32_t triangle) {
  require(row.supportCandidate && row.mesh && triangle < row.mesh->indices.size() / 3);
  BimWalkableSurface surface{.entityId = row.entityId, .representationId = row.representationId,
                             .triangleIndex = triangle};
  for (size_t corner = 0; corner < 3; ++corner) {
    const auto index = row.mesh->indices[size_t(triangle) * 3 + corner];
    require(index < row.mesh->vertices.size());
    surface.vertices[corner] = row.originMeters + glm::dvec3(row.mesh->vertices[index]);
    require(finite(surface.vertices[corner]));
  }
  const auto normal = glm::cross(surface.vertices[1] - surface.vertices[0],
                                 surface.vertices[2] - surface.vertices[0]);
  const double length = glm::length(normal);
  require(std::isfinite(length) && length > 0);
  surface.normal = normal / length;
  return surface;
}

bool sameSurface(const BimWalkableSurface& first, const BimWalkableSurface& second) {
  return first.entityId == second.entityId && first.representationId == second.representationId &&
         first.triangleIndex == second.triangleIndex && first.vertices == second.vertices &&
         first.normal == second.normal;
}

bool surfaceLess(const BimWalkableSurface& first, const BimWalkableSurface& second) {
  const auto a = std::tie(first.entityId, first.representationId, first.triangleIndex);
  const auto b = std::tie(second.entityId, second.representationId, second.triangleIndex);
  if (a != b)
    return a < b;
  for (size_t corner = 0; corner < 3; ++corner)
    for (glm::length_t axis = 0; axis < 3; ++axis)
      if (first.vertices[corner][axis] != second.vertices[corner][axis])
        return first.vertices[corner][axis] < second.vertices[corner][axis];
  return false;
}

using SurfaceReference = std::pair<uint32_t, uint32_t>;

std::vector<SurfaceReference> surfaceReferences(
    std::span<const Row> rows, std::span<const BimWalkableSurface> surfaces) {
  require(surfaces.size() <= maxTriangles);
  std::map<std::pair<std::string_view, std::string_view>, std::vector<uint32_t>> groups;
  std::vector<std::vector<uint8_t>> used(rows.size());
  for (size_t index = 0; index < rows.size(); ++index) {
    const auto& row = *rows[index].collider;
    if (row.supportCandidate) {
      groups[{row.entityId, row.representationId}].push_back(static_cast<uint32_t>(index));
      used[index].resize(row.mesh->indices.size() / 3);
    }
  }
  size_t candidateTests = 0;
  std::vector<SurfaceReference> references;
  references.reserve(surfaces.size());
  for (const auto& surface : surfaces) {
    const auto group = groups.find({surface.entityId, surface.representationId});
    require(group != groups.end());
    bool found = false;
    for (const auto rowIndex : group->second) {
      require(++candidateTests <= maxSurfaceCandidateTests);
      if (surface.triangleIndex >= used[rowIndex].size() || used[rowIndex][surface.triangleIndex])
        continue;
      const auto actual = surfaceAt(*rows[rowIndex].collider, surface.triangleIndex);
      if (!sameSurface(surface, actual))
        continue;
      require(actual.normal.z > 0);
      used[rowIndex][surface.triangleIndex] = 1;
      references.emplace_back(rowIndex, surface.triangleIndex);
      found = true;
      break;
    }
    require(found);
  }
  std::sort(references.begin(), references.end());
  return references;
}

void validateDiagnostic(const BimCollisionDiagnostic& diagnostic, const Entities& entities) {
  require(!diagnostic.code.empty() && !diagnostic.message.empty());
  if (diagnostic.entityId.empty()) {
    require(diagnostic.representationId.empty());
    return;
  }
  const auto entity = entities.find(diagnostic.entityId);
  require(entity != entities.end());
  if (!diagnostic.representationId.empty())
    require(std::ranges::any_of(entity->second->representations, [&](const auto& representation) {
      return representation.sourceId == diagnostic.representationId;
    }));
}

} // namespace

void writeCollision(Writer& writer, const CookedBimCollision& collision,
                     std::span<const BimWalkableSurface> surfaces) {
  require(collision.colliders.size() <= maxColliders && collision.diagnostics.size() <= maxDiagnostics);
  std::map<const BimCollisionMesh*, uint32_t> meshIds;
  std::vector<const BimCollisionMesh*> pool;
  size_t triangleReferences = 0;
  for (const auto& row : collision.colliders) {
    validateRow(row);
    if (!row.mesh)
      continue;
    const bool inserted = meshIds.try_emplace(row.mesh.get(), static_cast<uint32_t>(pool.size())).second;
    if (inserted) {
      validateMesh(*row.mesh);
      pool.push_back(row.mesh.get());
    }
    require(row.mesh->indices.size() / 3 <= maxTriangles - triangleReferences);
    triangleReferences += row.mesh->indices.size() / 3;
  }
  require(pool.size() <= maxColliders);
  size_t vertices = 0, indices = 0;
  writer.count(pool.size());
  for (const auto* mesh : pool) {
    require(mesh->vertices.size() <= maxElements - vertices && mesh->indices.size() <= maxElements - indices);
    vertices += mesh->vertices.size(); indices += mesh->indices.size();
    writer.count(mesh->vertices.size());
    for (const auto vertex : mesh->vertices)
      writeVector(writer, vertex);
    writer.count(mesh->indices.size());
    for (const auto value : mesh->indices)
      writer.u32(value);
  }
  std::vector<Row> rows;
  rows.reserve(collision.colliders.size());
  for (const auto& row : collision.colliders)
    rows.push_back({&row, row.mesh ? meshIds.at(row.mesh.get()) : noMesh});
  TextBudget text;
  writer.count(rows.size());
  for (const auto& row : rows) {
    const auto& value = *row.collider;
    text.write(writer, value.entityId);
    text.write(writer, value.representationId);
    writer.u32(value.sourceMeshId);
    writer.u32(static_cast<uint32_t>(value.kind));
    writer.u32(value.supportCandidate ? 1 : 0);
    writePoint(writer, value.originMeters);
    writeVector(writer, value.halfExtents);
    writer.u32(row.meshIndex);
  }
  std::vector<const BimCollisionDiagnostic*> diagnostics;
  diagnostics.reserve(collision.diagnostics.size());
  for (const auto& diagnostic : collision.diagnostics) {
    require(!diagnostic.code.empty() && !diagnostic.message.empty() &&
            (!diagnostic.entityId.empty() || diagnostic.representationId.empty()));
    diagnostics.push_back(&diagnostic);
  }
  writer.count(diagnostics.size());
  for (const auto* diagnostic : diagnostics) {
    text.write(writer, diagnostic->entityId); text.write(writer, diagnostic->representationId);
    text.write(writer, diagnostic->code); text.write(writer, diagnostic->message);
  }
  const auto references = surfaceReferences(rows, surfaces);
  writer.count(references.size());
  for (const auto& [row, triangle] : references) {
    writer.u32(row); writer.u32(triangle);
  }
}

void readCollision(Reader& reader, const RoboticsScene& scene, double maxSlopeDegrees,
                    CookedBimCollision& collision, std::vector<BimWalkableSurface>& surfaces) {
  require(std::isfinite(maxSlopeDegrees) && maxSlopeDegrees >= 0 && maxSlopeDegrees < 90);
  Entities entities;
  for (const auto& entity : scene.entities)
    require(!entity.id.empty() && entities.emplace(entity.id, &entity).second);
  const auto meshCount = reader.count(8);
  require(meshCount <= maxColliders);
  std::vector<std::shared_ptr<const BimCollisionMesh>> pool;
  pool.reserve(meshCount);
  size_t vertices = 0, indices = 0;
  for (uint32_t index = 0; index < meshCount; ++index) {
    auto mesh = std::make_shared<BimCollisionMesh>();
    const auto vertexCount = reader.count(12);
    require(vertexCount <= maxElements - vertices);
    vertices += vertexCount;
    mesh->vertices.resize(vertexCount);
    for (auto& vertex : mesh->vertices)
      vertex = readVector(reader);
    const auto indexCount = reader.count(4);
    require(indexCount <= maxElements - indices);
    indices += indexCount;
    mesh->indices.resize(indexCount);
    for (auto& value : mesh->indices)
      value = reader.u32();
    validateMesh(*mesh);
    pool.push_back(std::move(mesh));
  }
  CookedBimCollision decoded;
  const auto colliderCount = reader.count(60);
  require(colliderCount <= maxColliders);
  if (colliderCount) {
    require(scene.frame.metersPerRendererUnit && scene.frame.roboticsFromRenderer &&
            std::isfinite(*scene.frame.metersPerRendererUnit) && *scene.frame.metersPerRendererUnit > 0 &&
            *scene.frame.roboticsFromRenderer == rendererToRosTransform(*scene.frame.metersPerRendererUnit));
  }
  decoded.colliders.reserve(colliderCount);
  std::vector<uint8_t> usedMeshes(pool.size());
  TextBudget text;
  size_t triangleReferences = 0;
  for (uint32_t index = 0; index < colliderCount; ++index) {
    BimCollider row;
    row.entityId = text.read(reader);
    row.representationId = text.read(reader);
    row.sourceMeshId = reader.u32();
    const auto kind = reader.u32(), support = reader.u32();
    require(kind <= static_cast<uint32_t>(BimColliderKind::DynamicBox) && support <= 1);
    row.kind = static_cast<BimColliderKind>(kind);
    row.supportCandidate = support != 0;
    row.originMeters = readPoint(reader);
    row.halfExtents = readVector(reader);
    const auto mesh = reader.u32();
    if (mesh != noMesh) {
      require(mesh < pool.size());
      row.mesh = pool[mesh];
      usedMeshes[mesh] = 1;
      require(row.mesh->indices.size() / 3 <= maxTriangles - triangleReferences);
      triangleReferences += row.mesh->indices.size() / 3;
    }
    validateRow(row);
    validateIdentity(row, entities);
    decoded.colliders.push_back(std::move(row));
  }
  require(std::ranges::all_of(usedMeshes, [](uint8_t used) { return used != 0; }));
  const auto diagnosticCount = reader.count(16);
  require(diagnosticCount <= maxDiagnostics);
  decoded.diagnostics.reserve(diagnosticCount);
  for (uint32_t index = 0; index < diagnosticCount; ++index) {
    BimCollisionDiagnostic diagnostic{text.read(reader), text.read(reader), text.read(reader), text.read(reader)};
    validateDiagnostic(diagnostic, entities);
    decoded.diagnostics.push_back(std::move(diagnostic));
  }
  const auto surfaceCount = reader.count(8);
  require(surfaceCount <= maxTriangles);
  std::vector<BimWalkableSurface> restored;
  restored.reserve(surfaceCount);
  std::optional<SurfaceReference> previous;
  for (uint32_t index = 0; index < surfaceCount; ++index) {
    const SurfaceReference reference{reader.u32(), reader.u32()};
    require(reference.first < decoded.colliders.size() && (!previous || *previous < reference));
    restored.push_back(surfaceAt(decoded.colliders[reference.first], reference.second));
    previous = reference;
  }
  auto expected = extractWalkableSurfaces(decoded, maxSlopeDegrees);
  std::sort(restored.begin(), restored.end(), surfaceLess);
  require(restored.size() == expected.size() && std::equal(restored.begin(), restored.end(), expected.begin(), sameSurface));
  collision = std::move(decoded);
  surfaces = std::move(expected);
}

} // namespace container::robotics::asset_cache
