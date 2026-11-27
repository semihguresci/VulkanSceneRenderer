#include "Container/robotics/NavigationTrainingLayout.h"

#include "Container/robotics/BimAssetCache.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/Platform.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace container::robotics {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr auto sceneSchema = "container.navigation-training-scene";
constexpr auto layoutSchema = "container.navigation-training-layout";
constexpr size_t maxRecipeBytes = 1024 * 1024;
constexpr size_t maxWitnessWaypoints = 256;
constexpr uint32_t maxSampleAttempts = 32;
constexpr double minimumGoalDistance = 0.3;
constexpr double furnitureContactTolerance = 0.0001;
constexpr double furnitureSupportTolerance = 0.015;

struct Range {
  double minimum;
  double maximum;
};

struct PoseRecipe {
  glm::dvec3 position{0.0};
  double jitter{};
};

struct FurnitureRecipe {
  std::string guid;
  std::array<Range, 2> offsets;
  size_t index{};
};

Json readRecipe(const fs::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw std::runtime_error("Could not open navigation training recipe " + util::pathToUtf8(path));
  const auto size = input.tellg();
  if (size <= 0 || size > static_cast<std::streamoff>(maxRecipeBytes))
    throw std::invalid_argument("Navigation training recipe must contain at most 1 MiB of JSON");
  input.seekg(0);
  std::string bytes(static_cast<size_t>(size), '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
      input.peek() != std::char_traits<char>::eof() || bytes.find('\0') != std::string::npos)
    throw std::invalid_argument("Could not read complete navigation training recipe JSON");
  std::vector<std::set<std::string>> keys;
  return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& parsed) {
    if (depth > 12) throw std::invalid_argument("Navigation training recipe nesting exceeds its limit");
    using enum Json::parse_event_t;
    if (event == object_start) keys.emplace_back();
    if (event == object_end) keys.pop_back();
    if (event == key && !keys.back().insert(parsed.get_ref<const std::string&>()).second)
      throw std::invalid_argument("Navigation training recipe contains a duplicate JSON field");
    return true;
  });
}

void checkKeys(const Json& object, std::initializer_list<std::string_view> keys) {
  if (!object.is_object()) throw std::invalid_argument("Expected a navigation recipe object");
  for (const auto& entry : object.items())
    if (std::ranges::find(keys, entry.key()) == keys.end())
      throw std::invalid_argument("Unknown navigation training recipe field " + entry.key());
}

std::string nonemptyString(const Json& value, std::string_view name) {
  if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
      value.get_ref<const std::string&>().find('\0') != std::string::npos)
    throw std::invalid_argument(std::string(name) + " must be a nonempty string without NUL");
  return value.get<std::string>();
}

double finiteNumber(const Json& value, std::string_view name) {
  if (!value.is_number() || !std::isfinite(value.get<double>()))
    throw std::invalid_argument(std::string(name) + " must be a finite number");
  return value.get<double>();
}

glm::dvec3 parsePoint(const Json& value, std::string_view name) {
  if (!value.is_array() || value.size() != 3)
    throw std::invalid_argument(std::string(name) + " must contain three REP-103 coordinates in metres");
  return {finiteNumber(value[0], name), finiteNumber(value[1], name), finiteNumber(value[2], name)};
}

PoseRecipe parsePose(const Json& value, std::string_view name) {
  checkKeys(value, {"position", "jitterMeters"});
  PoseRecipe pose{parsePoint(value.at("position"), name)};
  if (const auto jitter = value.find("jitterMeters"); jitter != value.end())
    pose.jitter = finiteNumber(*jitter, "jitterMeters");
  if (pose.jitter < 0.0)
    throw std::invalid_argument(std::string(name) + " jitterMeters cannot be negative");
  return pose;
}

Range parseRange(const Json& range, std::string_view name, double minimum, double maximum) {
  if (!range.is_array() || range.size() != 2)
    throw std::invalid_argument(std::string(name) + " must contain a minimum and maximum");
  const Range result{finiteNumber(range[0], name), finiteNumber(range[1], name)};
  if (result.minimum < minimum || result.minimum > result.maximum || result.maximum > maximum)
    throw std::invalid_argument(std::string(name) + " has reversed bounds or exceeds its supported range");
  return result;
}

Json point(glm::dvec3 value) {
  return Json::array({value.x, value.y, value.z});
}

// Include every used triangle and box in a common metre frame. Quantize to
// millimetres and sort the coordinates so authoring units, triangle order,
// BIM identity, and a global translation cannot distinguish equivalent inputs.
std::string geometryFingerprint(const CookedBimCollision& geometry) {
  using GridPoint = std::array<int64_t, 3>;
  struct Primitive {
    uint8_t kind{};
    std::array<GridPoint, 3> points;
    auto operator<=>(const Primitive&) const = default;
  };
  const auto visit = [&](auto&& handle) {
    for (const auto& collider : geometry.colliders) {
      if (collider.kind == BimColliderKind::DynamicBox) {
        const auto minimum = collider.originMeters - glm::dvec3(collider.halfExtents);
        const auto maximum = collider.originMeters + glm::dvec3(collider.halfExtents);
        handle(uint8_t{1}, std::array{minimum, maximum, maximum});
        continue;
      }
      const auto& mesh = *collider.mesh;
      for (size_t i = 0; i < mesh.indices.size(); i += 3)
        handle(uint8_t{0}, std::array{
            collider.originMeters + glm::dvec3(mesh.vertices[mesh.indices[i]]),
            collider.originMeters + glm::dvec3(mesh.vertices[mesh.indices[i + 1]]),
            collider.originMeters + glm::dvec3(mesh.vertices[mesh.indices[i + 2]])});
    }
  };
  glm::dvec3 origin(std::numeric_limits<double>::infinity());
  visit([&](uint8_t, const auto& points) {
    for (const auto& coordinate : points) origin = glm::min(origin, coordinate);
  });
  std::vector<Primitive> primitives;
  visit([&](uint8_t kind, const auto& points) {
    Primitive primitive{.kind = kind, .points = {}};
    for (size_t i = 0; i < points.size(); ++i) {
      const auto grid = 1000.0 * (points[i] - origin);
      for (glm::length_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(grid[axis]) || grid[axis] >= static_cast<double>(std::numeric_limits<int64_t>::max()))
          throw std::invalid_argument("Navigation geometry exceeds its supported metre range");
        primitive.points[i][static_cast<size_t>(axis)] = std::llround(grid[axis]);
      }
    }
    std::ranges::sort(primitive.points);
    primitives.push_back(primitive);
  });
  if (primitives.empty()) throw std::invalid_argument("Navigation scene has no cooked collision geometry");
  std::ranges::sort(primitives);
  primitives.erase(std::unique(primitives.begin(), primitives.end()), primitives.end());
  std::vector<unsigned char> bytes{1};
  for (const auto& primitive : primitives) {
    bytes.push_back(primitive.kind);
    for (const auto& coordinate : primitive.points)
      for (const auto value : coordinate)
        for (unsigned shift = 0; shift < 64; shift += 8)
          bytes.push_back(static_cast<unsigned char>((static_cast<uint64_t>(value) >> shift) & 0xffu));
  }
  return util::FingerprintContent(bytes).digest;
}

// The triangle and axis-aligned box separate on a box axis, the triangle
// normal, or an edge crossed with a box axis. Testing the actual triangles
// avoids accepting corner penetration or rejecting a clear doorway by its AABB.
bool triangleOverlapsBox(const std::array<glm::dvec3, 3>& triangle, glm::dvec3 half) {
  constexpr std::array axes{glm::dvec3(1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, 0, 1)};
  const auto separates = [&](glm::dvec3 axis) {
    const auto [low, high] = std::minmax({glm::dot(triangle[0], axis), glm::dot(triangle[1], axis),
                                         glm::dot(triangle[2], axis)});
    const auto projectedRadius = glm::dot(glm::abs(axis), half);
    return low > projectedRadius || high < -projectedRadius;
  };
  if (std::ranges::any_of(axes, separates)) return false;
  const std::array edges{triangle[1] - triangle[0], triangle[2] - triangle[1], triangle[0] - triangle[2]};
  if (separates(glm::cross(edges[0], edges[1]))) return false;
  for (const auto& edge : edges)
    for (const auto& axis : axes)
      if (separates(glm::cross(edge, axis))) return false;
  return true;
}

bool intersectsStructure(const BimCollider& box, const BimCollider& structure) {
  const auto* mesh = structure.mesh.get();
  const auto half = glm::max(glm::dvec3(box.halfExtents) - furnitureContactTolerance, glm::dvec3(0));
  const auto delta = structure.originMeters - box.originMeters;
  for (size_t i = 0; i < mesh->indices.size(); i += 3)
    if (triangleOverlapsBox({delta + glm::dvec3(mesh->vertices[mesh->indices[i]]),
                            delta + glm::dvec3(mesh->vertices[mesh->indices[i + 1]]),
                            delta + glm::dvec3(mesh->vertices[mesh->indices[i + 2]])}, half))
      return true;
  return false;
}

bool furnitureSupported(const BimCollider& box, const BimWalkabilityQueries& queries) {
  const auto half = glm::dvec3(box.halfExtents);
  const auto bottom = box.originMeters - glm::dvec3(0, 0, half.z);
  for (const auto x : {-1, 0, 1}) {
    for (const auto y : {-1, 0, 1}) {
      const auto origin = bottom + glm::dvec3(x * half.x, y * half.y, furnitureSupportTolerance);
      const auto hit = queries.castSegment(origin, {0, 0, -2 * furnitureSupportTolerance});
      if (!hit || !hit->walkableSupport || hit->normal.z < 0.99 ||
          std::abs(hit->position.z - bottom.z) > furnitureSupportTolerance)
        return false;
    }
  }
  return true;
}

std::optional<std::string> invalidFurniture(const CookedBimCollision& geometry,
                                            const BimCollisionWorld& structure) {
  const auto queries = structure.walkabilityQueries();
  const auto& colliders = geometry.colliders;
  for (size_t i = 0; i < colliders.size(); ++i) {
    const auto& box = colliders[i];
    if (box.kind != BimColliderKind::DynamicBox) continue;
    if (!furnitureSupported(box, queries))
      return "Furniture " + box.entityId + " has no flat support under its footprint";
    for (const auto& obstacle : structure.geometry().colliders) {
      if (obstacle.kind == BimColliderKind::DoorMesh && structure.isDoorOpen(obstacle.entityId)) continue;
      if (intersectsStructure(box, obstacle))
        return "Furniture " + box.entityId + " intersects " + obstacle.entityId;
    }
    for (size_t j = i + 1; j < colliders.size(); ++j) {
      const auto& other = colliders[j];
      if (other.kind != BimColliderKind::DynamicBox) continue;
      const auto separation = glm::abs(box.originMeters - other.originMeters);
      const auto combinedHalf = glm::dvec3(box.halfExtents) + glm::dvec3(other.halfExtents);
      if (glm::all(glm::lessThan(separation, combinedHalf - furnitureContactTolerance)))
        return "Furniture " + box.entityId + " overlaps " + other.entityId;
    }
  }
  return std::nullopt;
}

std::optional<std::string> invalidRoute(const NavigationTrainingLayout& layout,
                                       std::span<const glm::dvec3> waypoints) {
  if (glm::length(layout.goal - layout.spawn.position) < minimumGoalDistance)
    return "Start and goal are too close to define a navigation episode";
  BimCollisionWorld world(layout.geometry, layout.openDoorIds, 1);
  const auto queries = world.walkabilityQueries();
  auto from = layout.spawn.position;
  if (!traceWalkablePath(from, from, queries).reachable)
    return "Start has no capsule clearance on its configured floor";
  for (size_t segment = 0; segment <= waypoints.size(); ++segment) {
    const auto destination = segment == waypoints.size() ? layout.goal : waypoints[segment];
    const auto witness = traceWalkablePath(from, destination, queries);
    if (!witness.reachable) {
      return "Unreachable navigation segment " + std::to_string(segment + 1) +
          (witness.blockingHit ? " at " + witness.blockingHit->entityId : " on the configured floor");
    }
    from = witness.reachedFeet;
  }
  return std::nullopt;
}

// Take the high 53 bits explicitly so bounded draws do not depend on a
// standard library's uniform_real_distribution implementation.
double draw(std::mt19937_64& random, Range range) {
  const auto fraction = static_cast<double>(random() >> 11) * 0x1.0p-53;
  return std::lerp(range.minimum, range.maximum, fraction);
}

glm::dvec3 samplePose(std::mt19937_64& random, const PoseRecipe& pose, bool randomize) {
  if (!randomize) return pose.position;
  const Range offset{-pose.jitter, pose.jitter};
  return pose.position + glm::dvec3{draw(random, offset), draw(random, offset), 0.0};
}

}  // namespace

struct NavigationTrainingScene::Impl {
  Json identity;
  CookedBimCollision geometry;
  PoseRecipe start;
  PoseRecipe goal;
  double initialYaw{};
  double yawJitter{};
  std::vector<glm::dvec3> waypoints;
  std::vector<std::string> openDoorIds;
  std::vector<FurnitureRecipe> furniture;
  Range ambient{0.25, 0.6};
  Range directional{0.35, 0.85};
  Range lightYaw{-std::numbers::pi, std::numbers::pi};
  Range rangeNoise{0.0, 0.025};
  Range encoderNoise{0.0, 0.015};
};

NavigationTrainingScene::NavigationTrainingScene(const fs::path& recipe) {
  const auto value = readRecipe(recipe);
  checkKeys(value, {"schema", "version", "id", "family", "split", "source", "sourceNamespace",
                    "start", "goal", "initialYaw", "yawJitterRadians", "witnessWaypoints",
                    "openDoorIds", "furniture", "ambientLight", "directionalLight", "lightYaw",
                    "rangeNoiseMeters", "encoderNoiseRadians"});
  const auto& version = value.at("version");
  if (value.at("schema") != sceneSchema || !version.is_number_integer() || version != 1)
    throw std::invalid_argument("Unsupported navigation training scene schema or version");
  auto result = std::make_shared<Impl>();
  auto split = nonemptyString(value.at("split"), "split");
  if (split != "train" && split != "held-out")
    throw std::invalid_argument("Navigation training split must be train or held-out");
  result->identity = {{"id", nonemptyString(value.at("id"), "id")},
                      {"family", nonemptyString(value.at("family"), "family")}, {"split", split}};
  result->start = parsePose(value.at("start"), "start");
  result->goal = parsePose(value.at("goal"), "goal");
  result->initialYaw = finiteNumber(value.at("initialYaw"), "initialYaw");
  if (std::abs(result->initialYaw) > std::numbers::pi)
    throw std::invalid_argument("initialYaw must be in [-pi,pi]");
  if (const auto jitter = value.find("yawJitterRadians"); jitter != value.end()) {
    result->yawJitter = finiteNumber(*jitter, "yawJitterRadians");
    if (result->yawJitter < 0.0 || result->yawJitter > std::numbers::pi)
      throw std::invalid_argument("yawJitterRadians must be in [0,pi]");
  }
  if (const auto points = value.find("witnessWaypoints"); points != value.end()) {
    if (!points->is_array() || points->size() > maxWitnessWaypoints)
      throw std::invalid_argument("witnessWaypoints must be an array of at most 256 points");
    for (const auto& waypoint : *points)
      result->waypoints.push_back(parsePoint(waypoint, "witnessWaypoint"));
  }
  if (const auto doors = value.find("openDoorIds"); doors != value.end()) {
    if (!doors->is_array()) throw std::invalid_argument("openDoorIds must be an array");
    std::set<std::string> seen;
    for (const auto& door : *doors) {
      auto id = nonemptyString(door, "door ID");
      if (!seen.insert(id).second) throw std::invalid_argument("Duplicate open door " + id);
      result->openDoorIds.push_back(std::move(id));
    }
  }
  const auto optionalRange = [&](const char* name, Range& field, double minimum, double maximum) {
    if (const auto range = value.find(name); range != value.end())
      field = parseRange(*range, name, minimum, maximum);
  };
  optionalRange("ambientLight", result->ambient, 0.0, 1.0);
  optionalRange("directionalLight", result->directional, 0.0, 1.0);
  optionalRange("lightYaw", result->lightYaw, -std::numbers::pi, std::numbers::pi);
  optionalRange("rangeNoiseMeters", result->rangeNoise, 0.0, 10.0);
  optionalRange("encoderNoiseRadians", result->encoderNoise, 0.0, 1.0);

  const auto source = fs::absolute(recipe.parent_path() /
      util::pathFromUtf8(nonemptyString(value.at("source"), "source"))).lexically_normal();
  const auto sourceNamespace = nonemptyString(value.at("sourceNamespace"), "sourceNamespace");
  BimAssetPreprocessOptions options;
  options.scene.sourceNamespace = sourceNamespace;
  auto assets = loadBimPreprocessedAssets(source, {}, options);
  const auto layer = std::ranges::find(assets.sourceLayers, source, &BimAssetSourceLayer::path);
  if (layer == assets.sourceLayers.end()) throw std::invalid_argument("Training scene has no source fingerprint");
  result->identity["sourceSha256"] = layer->fingerprint.digest;
  result->identity["geometryFingerprint"] = geometryFingerprint(assets.collision);
  result->identity["assetProducerRevision"] = assets.producerRevision;
  if (const auto list = value.find("furniture"); list != value.end()) {
    if (!list->is_array()) throw std::invalid_argument("furniture must be an array");
    std::set<std::string> seen;
    for (const auto& entry : *list) {
      checkKeys(entry, {"guid", "offsetX", "offsetY"});
      auto guid = nonemptyString(entry.at("guid"), "furniture GUID");
      if (!seen.insert(guid).second) throw std::invalid_argument("Duplicate furniture GUID " + guid);
      const auto entity = std::ranges::find(assets.scene.entities, guid, &SemanticEntity::guid);
      if (entity == assets.scene.entities.end() ||
          std::ranges::count(assets.scene.entities, guid, &SemanticEntity::guid) != 1)
        throw std::invalid_argument("Missing or ambiguous furniture GUID " + guid);
      const auto& colliders = assets.collision.colliders;
      const auto collider = std::ranges::find(colliders, entity->id, &BimCollider::entityId);
      if (collider == colliders.end() || collider->kind != BimColliderKind::DynamicBox ||
          std::ranges::count(colliders, entity->id, &BimCollider::entityId) != 1)
        throw std::invalid_argument("Furniture GUID must resolve to one movable box " + guid);
      const auto unbounded = std::numeric_limits<double>::max();
      result->furniture.push_back({.guid = std::move(guid),
          .offsets = {parseRange(entry.at("offsetX"), "offsetX", -unbounded, unbounded),
                      parseRange(entry.at("offsetY"), "offsetY", -unbounded, unbounded)},
          .index = static_cast<size_t>(collider - colliders.begin())});
    }
  }
  result->geometry = std::move(assets.collision);
  impl_ = std::move(result);
}

const nlohmann::json& NavigationTrainingScene::identity() const {
  return impl_->identity;
}

NavigationTrainingLayout NavigationTrainingScene::sample(uint64_t seed, bool randomize) const {
  const auto& scene = *impl_;
  std::unique_ptr<BimCollisionWorld> structure;
  if (std::ranges::any_of(scene.geometry.colliders, [](const auto& collider) {
        return collider.kind == BimColliderKind::DynamicBox;
      })) {
    auto staticGeometry = scene.geometry;
    std::erase_if(staticGeometry.colliders, [](const auto& collider) {
      return collider.kind == BimColliderKind::DynamicBox;
    });
    structure = std::make_unique<BimCollisionWorld>(std::move(staticGeometry), scene.openDoorIds, 1);
  }
  std::mt19937_64 random(seed);
  std::optional<std::string> failure;
  const auto sampleRange = [&](Range range, double fraction = 0.5) {
    return randomize ? draw(random, range) : std::lerp(range.minimum, range.maximum, fraction);
  };
  for (uint32_t attempt = 1; attempt <= (randomize ? maxSampleAttempts : 1); ++attempt) {
    NavigationTrainingLayout layout{
        .geometry = scene.geometry,
        .spawn = {samplePose(random, scene.start, randomize),
                  std::remainder(scene.initialYaw + sampleRange({-scene.yawJitter, scene.yawJitter}),
                                 2 * std::numbers::pi)},
        .goal = samplePose(random, scene.goal, randomize),
        .openDoorIds = scene.openDoorIds,
        .ambientLight = sampleRange(scene.ambient),
        .directionalLight = sampleRange(scene.directional),
        .lightYaw = sampleRange(scene.lightYaw),
        .rangeNoiseMeters = sampleRange(scene.rangeNoise, 0),
        .encoderNoiseRadians = sampleRange(scene.encoderNoise, 0),
        .manifest = scene.identity};
    Json furniture = Json::array();
    for (const auto& item : scene.furniture) {
      auto& collider = layout.geometry.colliders[item.index];
      const auto offset = randomize ? glm::dvec3{draw(random, item.offsets[0]), draw(random, item.offsets[1]), 0}
                                    : glm::dvec3(0);
      collider.originMeters += offset;
      furniture.push_back({{"guid", item.guid}, {"id", collider.entityId},
                           {"position", point(collider.originMeters)}, {"offsetMeters", point(offset)}});
    }
    if (structure && (failure = invalidFurniture(layout.geometry, *structure))) continue;
    if ((failure = invalidRoute(layout, scene.waypoints))) continue;
    auto waypoints = Json::array();
    for (const auto& waypoint : scene.waypoints) waypoints.push_back(point(waypoint));
    layout.manifest.update(Json{{"schema", layoutSchema}, {"version", 1}, {"seed", seed},
        {"randomized", randomize}, {"attempt", attempt}, {"start", point(layout.spawn.position)},
        {"yaw", layout.spawn.yaw}, {"goal", point(layout.goal)}, {"witnessWaypoints", std::move(waypoints)},
        {"openDoorIds", layout.openDoorIds}, {"furniture", std::move(furniture)},
        {"ambientLight", layout.ambientLight}, {"directionalLight", layout.directionalLight},
        {"lightYaw", layout.lightYaw}, {"rangeNoiseMeters", layout.rangeNoiseMeters},
        {"encoderNoiseRadians", layout.encoderNoiseRadians}});
    return layout;
  }
  throw std::invalid_argument("No collision-valid layout for " + scene.identity.at("id").get<std::string>() +
                              " after " + std::to_string(randomize ? maxSampleAttempts : 1) +
                              " candidate attempts: " + failure.value_or("invalid recipe"));
}

}  // namespace container::robotics
