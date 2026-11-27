#include "Container/robotics/BimCollisionGeometry.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/robotics/RoboticsScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace container::robotics {
namespace {

namespace bim = geometry::dotbim;

struct Recipe {
  BimColliderKind kind;
  bool supportCandidate{false};
};

std::optional<Recipe> recipeFor(const SemanticEntity& entity) {
  switch (entity.kind) {
  case SemanticKind::Wall: return Recipe{BimColliderKind::StaticMesh};
  case SemanticKind::Slab: return Recipe{BimColliderKind::StaticMesh, true};
  case SemanticKind::Door: return Recipe{BimColliderKind::DoorMesh};
  case SemanticKind::Other: break;
  default: return std::nullopt;
  }
  std::string type = entity.ifcClass;
  for (char& character : type) {
    if (character >= 'a' && character <= 'z')
      character = static_cast<char>(character - 'a' + 'A');
  }
  if (type == "IFCRAMP" || type == "IFCRAMPFLIGHT" ||
      type == "IFCSTAIR" || type == "IFCSTAIRFLIGHT")
    return Recipe{BimColliderKind::StaticMesh, true};
  if (type == "IFCFURNISHINGELEMENT" || type == "IFCFURNITURE" ||
      type == "IFCSYSTEMFURNITUREELEMENT")
    return Recipe{BimColliderKind::DynamicBox};
  static constexpr std::string_view structureTypes[]{
      "IFCBEAM", "IFCBEAMSTANDARDCASE", "IFCCOLUMN", "IFCCOLUMNSTANDARDCASE", "IFCCURTAINWALL",
      "IFCFOOTING", "IFCMEMBER", "IFCMEMBERSTANDARDCASE", "IFCPILE", "IFCPLATE",
      "IFCPLATESTANDARDCASE", "IFCRAILING", "IFCROOF", "IFCWINDOW", "IFCWINDOWSTANDARDCASE"};
  if (std::ranges::find(structureTypes, std::string_view(type)) != std::end(structureTypes))
    return Recipe{BimColliderKind::StaticMesh};
  return std::nullopt;
}

bool finite(const glm::dvec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

struct Bounds {
  glm::dvec3 minimum{std::numeric_limits<double>::infinity()};
  glm::dvec3 maximum{-std::numeric_limits<double>::infinity()};

  void add(const glm::dvec3& point) {
    minimum = glm::min(minimum, point);
    maximum = glm::max(maximum, point);
  }

  glm::dvec3 center() const {
    return {std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y),
            std::midpoint(minimum.z, maximum.z)};
  }
};

using VariantKey = std::tuple<uint32_t, std::array<double, 9>, bool>;

struct Variant {
  Bounds bounds;
  std::shared_ptr<const BimCollisionMesh> mesh;
  size_t droppedDegenerateTriangles{};
  std::string_view error;
};

std::optional<bool> mirroredTransform(const glm::dmat4& transform) {
  glm::dmat3 normalized(1.0);
  for (glm::length_t column = 0; column < 3; ++column) {
    const auto direction = glm::dvec3(transform[column]);
    const double length = std::hypot(direction.x, direction.y, direction.z);
    if (!std::isfinite(length) || length == 0.0 || transform[column].w != 0.0)
      return std::nullopt;
    normalized[column] = direction / length;
  }
  const double determinant = glm::determinant(normalized);
  if (determinant == 0.0 || !finite(glm::dvec3(transform[3])) || transform[3].w != 1.0)
    return std::nullopt;
  return determinant < 0.0;
}

VariantKey variantKey(uint32_t meshId, const glm::dmat3& linear, bool needsMesh) {
  std::array<double, 9> coefficients{};
  size_t next = 0;
  for (glm::length_t column = 0; column < 3; ++column)
    for (glm::length_t row = 0; row < 3; ++row)
      coefficients[next++] = linear[column][row];
  return {meshId, coefficients, needsMesh};
}

Variant buildVariant(const bim::Model& model, std::span<const uint32_t> indices,
                     const glm::dmat3& linear, bool mirrored, bool needsMesh) {
  Variant result;
  for (const uint32_t index : indices) {
    if (index >= model.vertices.size()) {
      result.error = "A source triangle index is outside the vertex array.";
      return result;
    }
    const auto point = linear * glm::dvec3(model.vertices[index].position);
    if (!finite(point)) {
      result.error = "The mesh contains nonfinite transformed vertices.";
      return result;
    }
    result.bounds.add(point);
  }
  if (!needsMesh)
    return result;

  const auto center = result.bounds.center();
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->indices.reserve(indices.size());
  // Weld only exactly equal local float positions. There is no tolerance that
  // could close a narrow opening or move an authored collision surface.
  std::map<std::array<float, 3>, uint32_t> vertexIds;
  for (size_t i = 0; i < indices.size(); i += 3) {
    std::array<glm::vec3, 3> triangle;
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto local = linear * glm::dvec3(model.vertices[indices[i + corner]].position) - center;
      if (!finite(local) || glm::any(glm::greaterThan(glm::abs(local),
              glm::dvec3(std::numeric_limits<float>::max())))) {
        result.error = "The rebased mesh vertices exceed local float precision range.";
        return result;
      }
      triangle[corner] = glm::vec3(local);
    }
    const auto cross = glm::cross(glm::dvec3(triangle[1]) - glm::dvec3(triangle[0]),
                                  glm::dvec3(triangle[2]) - glm::dvec3(triangle[0]));
    if (cross == glm::dvec3(0.0)) {
      ++result.droppedDegenerateTriangles;
      continue;
    }
    if (mirrored)
      std::swap(triangle[1], triangle[2]);
    for (const glm::vec3& point : triangle) {
      const auto [entry, inserted] = vertexIds.try_emplace(std::array{point.x, point.y, point.z},
                                                          static_cast<uint32_t>(mesh->vertices.size()));
      if (inserted)
        mesh->vertices.push_back(point);
      mesh->indices.push_back(entry->second);
    }
  }
  if (mesh->indices.empty()) {
    result.error = "The mesh has no nondegenerate collision triangles.";
    return result;
  }
  result.mesh = std::move(mesh);
  return result;
}

bool hasMeshRepresentation(const SemanticEntity& entity) {
  return std::ranges::any_of(entity.representations, [](const auto& representation) {
    return representation.geometryKind == SemanticGeometryKind::Mesh;
  });
}

} // namespace

CookedBimCollision cookBimCollisionGeometry(const bim::Model& model, const RoboticsScene& scene,
                                            const BimCollisionCookOptions& options) {
  if (options.maxColliders == 0 || options.maxTriangleReferences == 0)
    throw std::invalid_argument("BIM collision cooking requires positive collider and triangle budgets");

  CookedBimCollision result;
  if (!scene.frame.metersPerRendererUnit || !std::isfinite(*scene.frame.metersPerRendererUnit) ||
      *scene.frame.metersPerRendererUnit <= 0.0) {
    result.diagnostics.push_back({{}, {}, "metric-units-unavailable",
                                  "Physical metre units are required to cook BIM collision shapes."});
    return result;
  }

  std::map<uint32_t, const bim::MeshRange*> ranges;
  for (const auto& range : model.meshRanges) {
    const auto [entry, inserted] = ranges.try_emplace(range.meshId, &range);
    if (!inserted)
      entry->second = nullptr;
  }
  std::map<VariantKey, Variant> variants;
  size_t triangleReferences = 0;
  const auto appendCollider = [&](BimCollider collider) {
    if (result.colliders.size() == options.maxColliders)
      throw std::length_error("BIM collision collider budget exceeded");
    result.colliders.push_back(std::move(collider));
  };

  for (const auto& entity : scene.entities) {
    const auto recipe = recipeFor(entity);
    if (!recipe) {
      if ((entity.kind == SemanticKind::Other || entity.kind == SemanticKind::Unknown) &&
          hasMeshRepresentation(entity))
        result.diagnostics.push_back({entity.id, {}, "unsupported-collider-class",
                                      "No collision recipe is defined for this IFC class."});
      continue;
    }

    const bool dynamic = recipe->kind == BimColliderKind::DynamicBox;
    Bounds boxBounds;
    std::optional<glm::dvec3> boxReference;
    size_t accepted = 0;
    for (const auto& representation : entity.representations) {
      if (representation.geometryKind != SemanticGeometryKind::Mesh)
        continue;
      const auto diagnostic = [&](std::string_view code, std::string_view message) {
        result.diagnostics.push_back({entity.id, representation.sourceId,
                                      std::string(code), std::string(message)});
      };
      const auto found = ranges.find(representation.meshId);
      const auto* range = found == ranges.end() ? nullptr : found->second;
      if (!range || range->firstIndex > model.indices.size() || range->indexCount == 0 ||
          range->indexCount % 3 != 0 || range->indexCount > model.indices.size() - range->firstIndex) {
        diagnostic("invalid-mesh-range", "The source mesh range is missing, ambiguous or invalid.");
        continue;
      }
      if (!representation.roboticsWorldFromLocal) {
        diagnostic("metric-placement-unavailable", "The representation has no physical world placement.");
        continue;
      }
      const auto& transform = *representation.roboticsWorldFromLocal;
      const auto mirrored = mirroredTransform(transform);
      if (!mirrored) {
        diagnostic("invalid-metric-placement", "The metric placement is not a finite, nonsingular affine matrix.");
        continue;
      }

      const size_t triangles = range->indexCount / 3;
      if (triangles > options.maxTriangleReferences - triangleReferences)
        throw std::length_error("BIM collision source triangle budget exceeded");
      triangleReferences += triangles;
      const glm::dmat3 linear(transform);
      const glm::dvec3 translation(transform[3]);
      const auto [variantEntry, inserted] = variants.try_emplace(variantKey(range->meshId, linear, !dynamic));
      if (inserted) {
        const auto indices = std::span(model.indices).subspan(range->firstIndex, range->indexCount);
        variantEntry->second = buildVariant(model, indices, linear, *mirrored, !dynamic);
      }
      const auto& variant = variantEntry->second;
      if (!variant.error.empty()) {
        diagnostic("invalid-collision-geometry", variant.error);
        continue;
      }
      if (variant.droppedDegenerateTriangles != 0)
        diagnostic("degenerate-collision-triangles", "Triangles without local floating-point area were omitted.");

      const glm::dvec3 reference = dynamic ? boxReference.value_or(translation) : glm::dvec3(0.0);
      const auto offset = translation - reference;
      const auto lower = variant.bounds.minimum + offset;
      const auto upper = variant.bounds.maximum + offset;
      if (!finite(lower) || !finite(upper)) {
        diagnostic("invalid-collision-bounds", "The transformed mesh exceeds the finite world coordinate range.");
        continue;
      }
      if (dynamic) {
        boxBounds.add(lower);
        boxBounds.add(upper);
        boxReference = reference;
      } else {
        appendCollider({.entityId = entity.id,
                        .representationId = representation.sourceId,
                        .sourceMeshId = range->meshId,
                        .kind = recipe->kind,
                        .originMeters = variant.bounds.center() + translation,
                        .mesh = variant.mesh,
                        .supportCandidate = recipe->supportCandidate});
      }
      ++accepted;
    }

    if (boxReference) {
      const auto center = boxBounds.center();
      const auto origin = center + *boxReference;
      const auto localOrigin = origin - *boxReference;
      const auto half = glm::max(boxBounds.maximum - localOrigin, localOrigin - boxBounds.minimum);
      const auto maxFloat = glm::dvec3(std::numeric_limits<float>::max());
      if (finite(origin) && finite(half) && glm::all(glm::lessThanEqual(half, maxFloat))) {
        auto halfExtents = glm::vec3(half);
        for (glm::length_t axis = 0; axis < 3; ++axis) {
          if (static_cast<double>(halfExtents[axis]) < half[axis])
            halfExtents[axis] = std::nextafter(halfExtents[axis], std::numeric_limits<float>::infinity());
        }
        if (glm::all(glm::greaterThan(halfExtents, glm::vec3(0.f)))) {
          appendCollider({.entityId = entity.id,
                          .kind = BimColliderKind::DynamicBox,
                          .originMeters = origin,
                          .halfExtents = halfExtents});
          continue;
        }
      }
      result.diagnostics.push_back({entity.id, {}, "invalid-dynamic-box",
                                    "The model does not supply finite positive box extents."});
    } else if (accepted == 0) {
      result.diagnostics.push_back({entity.id, {}, "collision-representation-unavailable",
                                    "No authored mesh was usable for this collision recipe."});
    }
  }

  std::stable_sort(result.colliders.begin(), result.colliders.end(), [](const auto& first, const auto& second) {
    return std::tie(first.entityId, first.representationId, first.sourceMeshId,
                    first.originMeters.x, first.originMeters.y, first.originMeters.z) <
           std::tie(second.entityId, second.representationId, second.sourceMeshId,
                    second.originMeters.x, second.originMeters.y, second.originMeters.z);
  });
  return result;
}

} // namespace container::robotics
