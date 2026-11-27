#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/robotics/BimCollisionOverlay.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/robotics/RoboticsScene.h"
#include "Container/utility/AtomicFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace {

using Json = nlohmann::json;
namespace robotics = container::robotics;

constexpr std::string_view usage =
    "Usage: BimRoboticsCollision INPUT.ifc|INPUT.ifcx OUTPUT.json|OUTPUT.glb SOURCE_NAMESPACE [QUERIES.json]\n";

struct Config {
  std::optional<double> metersPerRendererUnit;
  std::vector<std::string> openDoorIds;
  robotics::BimWalkabilitySettings agent;
  robotics::BimCollisionCookOptions cook;
  Json raycasts = Json::array();
  Json walks = Json::array();
};

void checkKeys(const Json& object, std::initializer_list<std::string_view> allowed) {
  if (!object.is_object())
    throw std::invalid_argument("Expected a collision configuration object");
  for (auto value = object.begin(); value != object.end(); ++value)
    if (std::ranges::find(allowed, value.key()) == allowed.end())
      throw std::invalid_argument("Unknown collision configuration field: " + value.key());
}

size_t readLimit(const Json& settings, const char* key, size_t fallback) {
  const auto found = settings.find(key);
  if (found == settings.end())
    return fallback;
  if (!found->is_number_unsigned() || found->get<uint64_t>() == 0 ||
      found->get<uint64_t>() > std::numeric_limits<size_t>::max())
    throw std::invalid_argument(std::string(key) + " must be a positive integer in range");
  return found->get<size_t>();
}

Config readConfig(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Could not open the BIM collision query file");
  const auto values = Json::parse(stream);
  checkKeys(values, {"version", "metersPerRendererUnit", "openDoorIds", "agent", "cook", "raycasts", "walks"});
  if (values.at("version") != 1)
    throw std::invalid_argument("Unsupported BIM collision query file version");
  Config config;
  if (values.contains("metersPerRendererUnit"))
    config.metersPerRendererUnit = values.at("metersPerRendererUnit").get<double>();
  config.openDoorIds = values.value("openDoorIds", config.openDoorIds);
  config.raycasts = values.value("raycasts", config.raycasts);
  config.walks = values.value("walks", config.walks);
  if (!config.raycasts.is_array() || !config.walks.is_array())
    throw std::invalid_argument("Raycasts and walks must be arrays of queries");
  if (values.contains("agent")) {
    const auto& agent = values.at("agent");
    checkKeys(agent, {"radius", "height", "maxSlopeDegrees", "maxStepHeight", "maxDropHeight",
                      "sampleSpacing", "skin", "elevationTolerance", "maxSamples"});
    config.agent.radius = agent.value("radius", config.agent.radius);
    config.agent.height = agent.value("height", config.agent.height);
    config.agent.maxSlopeDegrees = agent.value("maxSlopeDegrees", config.agent.maxSlopeDegrees);
    config.agent.maxStepHeight = agent.value("maxStepHeight", config.agent.maxStepHeight);
    config.agent.maxDropHeight = agent.value("maxDropHeight", config.agent.maxDropHeight);
    config.agent.sampleSpacing = agent.value("sampleSpacing", config.agent.sampleSpacing);
    config.agent.skin = agent.value("skin", config.agent.skin);
    config.agent.elevationTolerance = agent.value("elevationTolerance", config.agent.elevationTolerance);
    config.agent.maxSamples = readLimit(agent, "maxSamples", config.agent.maxSamples);
  }
  if (values.contains("cook")) {
    const auto& cook = values.at("cook");
    checkKeys(cook, {"maxColliders", "maxTriangleReferences"});
    config.cook.maxColliders = readLimit(cook, "maxColliders", config.cook.maxColliders);
    config.cook.maxTriangleReferences = readLimit(cook, "maxTriangleReferences", config.cook.maxTriangleReferences);
  }
  return config;
}

glm::dvec3 point(const Json& value) {
  if (!value.is_array() || value.size() != 3)
    throw std::invalid_argument("A collision query point must be [x, y, z] in metres");
  glm::dvec3 result;
  for (glm::length_t axis = 0; axis < 3; ++axis) {
    result[axis] = value.at(axis).get<double>();
    if (!std::isfinite(result[axis]))
      throw std::invalid_argument("A collision query point must be finite");
  }
  return result;
}

Json xyz(glm::dvec3 value) { return Json::array({value.x, value.y, value.z}); }

Json describeHit(const std::optional<robotics::BimWalkabilityHit>& hit) {
  if (!hit)
    return nullptr;
  return {{"fraction", hit->fraction}, {"position", xyz(hit->position)}, {"normal", xyz(hit->normal)},
          {"entityId", hit->entityId}, {"walkableSupport", hit->walkableSupport}};
}

std::string_view walkFailure(robotics::BimWalkabilityFailure result) {
  using enum robotics::BimWalkabilityFailure;
  switch (result) {
  case None: return "none";
  case MissingSupport: return "missing-support";
  case UnsupportedSurface: return "unsupported-surface";
  case SteepSurface: return "steep-surface";
  case StepTooHigh: return "step-too-high";
  case DropTooFar: return "drop-too-far";
  case Blocked: return "blocked";
  case WrongElevation: return "wrong-elevation";
  case SampleLimit: return "sample-limit";
  }
  throw std::logic_error("Unknown BIM walkability result");
}

std::string_view kindName(robotics::BimColliderKind kind) {
  using enum robotics::BimColliderKind;
  switch (kind) {
  case StaticMesh: return "static-mesh";
  case DoorMesh: return "door-mesh";
  case DynamicBox: return "dynamic-box";
  }
  throw std::logic_error("Unknown BIM collider kind");
}

Json report(const robotics::BimCollisionWorld& world, const robotics::RoboticsScene& scene,
            const Config& config) {
  const auto queries = world.walkabilityQueries();
  Json result{{"schema", "container.bim-collision-report"}, {"version", 1},
              {"coordinates", "ROS REP-103, metres"}, {"sourceNamespace", scene.sourceNamespace},
              {"unitProvenance", scene.frame.unitProvenance},
              {"openDoorIds", config.openDoorIds},
              {"agent", {{"radius", config.agent.radius}, {"height", config.agent.height},
                         {"maxSlopeDegrees", config.agent.maxSlopeDegrees}, {"maxStepHeight", config.agent.maxStepHeight},
                         {"maxDropHeight", config.agent.maxDropHeight}, {"sampleSpacing", config.agent.sampleSpacing},
                         {"skin", config.agent.skin}, {"elevationTolerance", config.agent.elevationTolerance},
                         {"maxSamples", config.agent.maxSamples}}},
              {"activeColliders", world.activeColliderCount()}, {"cachedStaticMeshes", world.cachedStaticMeshCount()},
              {"walkableTriangles", robotics::extractWalkableSurfaces(world.geometry(), config.agent.maxSlopeDegrees).size()},
              {"colliders", Json::array()}, {"diagnostics", Json::array()},
              {"raycasts", Json::array()}, {"walks", Json::array()}};
  for (const auto& row : world.geometry().colliders) {
    const bool active = row.kind != robotics::BimColliderKind::DoorMesh || !world.isDoorOpen(row.entityId);
    result["colliders"].push_back({{"entityId", row.entityId}, {"representationId", row.representationId},
        {"kind", kindName(row.kind)}, {"active", active}, {"supportCandidate", row.supportCandidate},
        {"origin", xyz(row.originMeters)}, {"triangles", row.mesh ? row.mesh->indices.size() / 3 : 0},
        {"halfExtents", xyz(glm::dvec3(row.halfExtents))}});
  }
  for (const auto& diagnostic : world.geometry().diagnostics)
    result["diagnostics"].push_back({{"entityId", diagnostic.entityId}, {"representationId", diagnostic.representationId},
                                      {"code", diagnostic.code}, {"message", diagnostic.message}});
  for (const auto& query : config.raycasts) {
    checkKeys(query, {"from", "to"});
    const auto from = point(query.at("from")), to = point(query.at("to"));
    result["raycasts"].push_back({{"from", xyz(from)}, {"to", xyz(to)},
                                    {"hit", describeHit(queries.castSegment(from, to - from))}});
  }
  for (const auto& query : config.walks) {
    checkKeys(query, {"from", "to"});
    const auto from = point(query.at("from")), to = point(query.at("to"));
    const auto walk = robotics::traceWalkablePath(from, to, queries, config.agent);
    Json samples = Json::array();
    for (const auto& sample : walk.samples)
      samples.push_back({{"entityId", sample.entityId}, {"feet", xyz(sample.feet)}, {"normal", xyz(sample.normal)}});
    result["walks"].push_back({{"from", xyz(from)}, {"to", xyz(to)}, {"reachable", walk.reachable},
        {"reason", walkFailure(walk.reason)}, {"reachedFeet", xyz(walk.reachedFeet)},
        {"blockingHit", describeHit(walk.blockingHit)}, {"supportSamples", std::move(samples)}});
  }
  return result;
}

template <class Char>
int run(int count, Char** args) {
  if (count == 2 && std::filesystem::path(args[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (count != 4 && count != 5) {
    std::cerr << usage;
    return 2;
  }
  try {
    const std::filesystem::path input(args[1]), output(args[2]);
    std::string extension = input.extension().string();
    std::ranges::transform(extension, extension.begin(), [](char ch) {
      return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    });
    if (extension != ".ifc" && extension != ".ifcx")
      throw std::invalid_argument("Input must be an IFC STEP or IFCX model");
    if (output.extension() != ".json" && output.extension() != ".glb")
      throw std::invalid_argument("Output must be a .json query report or a .glb collision overlay");
    const Config config = count == 5 ? readConfig(std::filesystem::path(args[4])) : Config{};
    const auto key = std::filesystem::path(args[3]).u8string();
    const robotics::RoboticsSceneOptions options{
        .sourceNamespace = std::string(key.begin(), key.end()),
        .metersPerRendererUnit = config.metersPerRendererUnit};
    const auto model = extension == ".ifc" ? container::geometry::ifc::LoadFromFile(input)
                                             : container::geometry::ifcx::LoadFromFile(input);
    const auto scene = robotics::buildRoboticsScene(model, options);
    const robotics::BimCollisionWorld world(robotics::cookBimCollisionGeometry(model, scene, config.cook),
                                            config.openDoorIds);
    if (output.extension() == ".glb")
      robotics::writeBimCollisionOverlay(output, world, scene.frame, config.agent.maxSlopeDegrees);
    else
      container::util::writeFileAtomically(output, report(world, scene, config).dump(2) + '\n');
    std::cout << "Prepared " << world.activeColliderCount() << " active BIM colliders\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsCollision: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
