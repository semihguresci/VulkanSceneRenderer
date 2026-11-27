#include "Container/robotics/BimAssetCache.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/robotics/BimNavigationMap.h"
#include "Container/robotics/BimSceneSourceManifest.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace robotics = container::robotics;
namespace util = container::util;

constexpr std::string_view usage = "Usage: BimRoboticsNavMap MODEL REQUEST.json NEW_OUTPUT_DIRECTORY\n";
constexpr std::string_view requestSchema = "container.bim-navigation-map-request";
constexpr std::streamoff maxRequestBytes = 64 * 1024;

struct Waypoint {
  std::string id;
  glm::dvec2 position;
  double yaw{};
};

struct Request {
  robotics::BimNavigationMapSettings map;
  robotics::BimAssetPreprocessOptions import;
  std::vector<std::string> openDoorIds;
  std::vector<Waypoint> waypoints;
};

Json parseJson(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream)
    throw std::invalid_argument("Cannot open map request " + util::pathToUtf8(path));
  const auto length = stream.tellg();
  if (length <= 0 || length > maxRequestBytes)
    throw std::invalid_argument("Map request must be nonempty and at most 64 KiB");
  stream.seekg(0);
  std::string bytes(static_cast<size_t>(length), '\0');
  if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
      stream.peek() != std::char_traits<char>::eof() || bytes.find('\0') != std::string::npos)
    throw std::invalid_argument("Cannot read complete map request JSON");
  std::vector<std::set<std::string>> keys;
  return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& parsed) {
    using enum Json::parse_event_t;
    if (depth > 8) throw std::invalid_argument("Map request exceeds its nesting limit");
    if (event == object_start) keys.emplace_back();
    if (event == object_end) keys.pop_back();
    if (event == key && !keys.back().insert(parsed.get_ref<const std::string&>()).second)
      throw std::invalid_argument("Map request contains a duplicate JSON field");
    return true;
  });
}

void checkKeys(const Json& value, std::initializer_list<std::string_view> keys) {
  if (!value.is_object())
    throw std::invalid_argument("Expected a map request object");
  for (const auto& entry : value.items())
    if (std::ranges::find(keys, entry.key()) == keys.end())
      throw std::invalid_argument("Unknown map request field " + entry.key());
}

double number(const Json& value, std::string_view field) {
  if (!value.is_number() || !std::isfinite(value.get<double>()))
    throw std::invalid_argument(std::string(field) + " must be a finite number");
  return value.get<double>();
}

double optionalNumber(const Json& value, const char* field, double fallback) {
  const auto found = value.find(field);
  return found == value.end() ? fallback : number(*found, field);
}

std::string text(const Json& value, std::string_view field) {
  if (!value.is_string())
    throw std::invalid_argument(std::string(field) + " must be a nonempty string");
  auto result = value.get<std::string>();
  if (result.empty() || result.find('\0') != std::string::npos)
    throw std::invalid_argument(std::string(field) + " must be a nonempty string without NUL");
  return result;
}

glm::dvec2 xy(const Json& value, std::string_view field) {
  if (!value.is_array() || value.size() != 2)
    throw std::invalid_argument(std::string(field) + " must contain two REP-103 coordinates in metres");
  return {number(value[0], field), number(value[1], field)};
}

uint32_t dimension(const Json& value) {
  if (!value.is_number_unsigned() || value == 0 || value.get<uint64_t>() > 4096)
    throw std::invalid_argument("sizeCells dimensions must be integers from 1 to 4096");
  return value.get<uint32_t>();
}

Request readRequest(const fs::path& path) {
  const auto value = parseJson(path);
  checkKeys(value, {"schema", "version", "sourceNamespace", "metersPerRendererUnit",
                    "originMeters", "sizeCells", "resolutionMeters", "floorMeters",
                    "floorToleranceMeters", "radiusMeters", "heightMeters", "openDoorIds", "waypoints"});
  const auto& version = value.at("version");
  if (text(value.at("schema"), "schema") != requestSchema || !version.is_number_integer() || version != 1)
    throw std::invalid_argument("Unsupported navigation map request schema or version");

  Request request;
  request.import.scene.sourceNamespace = text(value.at("sourceNamespace"), "sourceNamespace");
  if (const auto unit = value.find("metersPerRendererUnit"); unit != value.end()) {
    request.import.scene.metersPerRendererUnit = number(*unit, "metersPerRendererUnit");
    if (*request.import.scene.metersPerRendererUnit <= 0)
      throw std::invalid_argument("metersPerRendererUnit must be positive");
  }
  auto& settings = request.map;
  const auto& size = value.at("sizeCells");
  if (!size.is_array() || size.size() != 2)
    throw std::invalid_argument("sizeCells must contain width and height");
  settings.width = dimension(size[0]);
  settings.height = dimension(size[1]);
  if (uint64_t(settings.width) * settings.height > 4'194'304)
    throw std::invalid_argument("Map request exceeds 4,194,304 cells");
  settings.originMeters = xy(value.at("originMeters"), "originMeters");
  settings.resolutionMeters = number(value.at("resolutionMeters"), "resolutionMeters");
  settings.floorMeters = number(value.at("floorMeters"), "floorMeters");
  settings.floorToleranceMeters = optionalNumber(value, "floorToleranceMeters", settings.floorToleranceMeters);
  settings.robotRadiusMeters = optionalNumber(value, "radiusMeters", settings.robotRadiusMeters);
  settings.robotHeightMeters = optionalNumber(value, "heightMeters", settings.robotHeightMeters);
  if (settings.resolutionMeters <= 0.0 || settings.floorToleranceMeters < 0.0 ||
      settings.robotRadiusMeters <= 0.0 || settings.robotHeightMeters < 2.0 * settings.robotRadiusMeters)
    throw std::invalid_argument("Map pitch and robot radius must be positive, with nonnegative floor tolerance "
                                "and height at least twice the radius");

  if (const auto open = value.find("openDoorIds"); open != value.end()) {
    if (!open->is_array())
      throw std::invalid_argument("openDoorIds must be an array");
    for (const auto& id : *open) request.openDoorIds.push_back(text(id, "door ID"));
    std::ranges::sort(request.openDoorIds);
    if (std::ranges::adjacent_find(request.openDoorIds) != request.openDoorIds.end())
      throw std::invalid_argument("openDoorIds contains a duplicate ID");
  }
  const auto& waypoints = value.at("waypoints");
  if (!waypoints.is_array() || waypoints.empty() || waypoints.size() > 256)
    throw std::invalid_argument("waypoints must contain from 1 to 256 named map poses");
  std::set<std::string> identifiers;
  for (const auto& entry : waypoints) {
    checkKeys(entry, {"id", "positionMeters", "yawRadians"});
    auto id = text(entry.at("id"), "waypoint ID");
    if (!identifiers.insert(id).second) throw std::invalid_argument("Duplicate waypoint " + id);
    const auto yaw = optionalNumber(entry, "yawRadians", 0.0);
    if (std::abs(yaw) > std::numbers::pi)
      throw std::invalid_argument("Waypoint yawRadians must be in [-pi,pi]");
    request.waypoints.push_back({std::move(id), xy(entry.at("positionMeters"), "waypoint positionMeters"), yaw});
  }
  return request;
}

Json verifyWaypoints(const Request& request, const std::vector<robotics::BimNavigationCell>& cells,
                     const robotics::BimWalkabilityQueries& queries) {
  const auto& map = request.map;
  robotics::BimWalkabilitySettings agent{.radius = map.robotRadiusMeters, .height = map.robotHeightMeters};
  agent.elevationTolerance = map.floorToleranceMeters;
  auto result = Json::array();
  for (const auto& waypoint : request.waypoints) {
    const auto cell = (waypoint.position - map.originMeters) / map.resolutionMeters;
    if (!(cell.x >= 0.0 && cell.y >= 0.0 && cell.x < map.width && cell.y < map.height))
      throw std::invalid_argument("Waypoint " + waypoint.id + " is outside the map");
    const auto index = size_t(cell.y) * map.width + size_t(cell.x);
    if (cells[index] != robotics::BimNavigationCell::Free)
      throw std::invalid_argument("Waypoint " + waypoint.id + " is not in a free map cell");
    const glm::dvec3 point(waypoint.position, map.floorMeters);
    const auto walk = robotics::traceWalkablePath(point, point, queries, agent);
    if (!walk.reachable)
      throw std::invalid_argument("Waypoint " + waypoint.id + " has no supported capsule clearance on the selected floor" +
                                  (walk.blockingHit ? " near " + walk.blockingHit->entityId : ""));
    result.push_back({{"id", waypoint.id}, {"positionMeters", Json::array({point.x, point.y})},
                      {"yawRadians", waypoint.yaw}});
  }
  return result;
}

std::string buildImage(const robotics::BimNavigationMapSettings& map,
                        const std::vector<robotics::BimNavigationCell>& cells) {
  std::string pgm = "P5\n" + std::to_string(map.width) + " " + std::to_string(map.height) + "\n255\n";
  pgm.reserve(pgm.size() + cells.size());
  // Map cells are bottom-up. The PGM format stores the top row first.
  for (uint32_t row = map.height; row > 0; --row) {
    const auto* first = cells.data() + size_t(row - 1) * map.width;
    pgm.append(reinterpret_cast<const char*>(first), map.width);
  }
  return pgm;
}

void publish(const fs::path& destination, const std::string& pgm, const std::string& yaml, const std::string& manifest) {
  const auto absolute = fs::absolute(destination);
  fs::create_directories(absolute.parent_path());
  const std::array paths{absolute / "map.pgm", absolute / "map.yaml", absolute / "waypoints.json"};
  const std::array<std::string_view, 3> contents{pgm, yaml, manifest};
  if (!fs::create_directory(absolute))
    throw std::invalid_argument("Navigation map output must be a new directory");
  size_t published = 0;
  try {
    for (; published < paths.size(); ++published)
      util::writeFileAtomically(paths[published], contents[published], util::AtomicFilePublication::CreateNew);
  } catch (...) {
    std::error_code ignored;
    for (size_t i = 0; i < published; ++i) fs::remove(paths[i], ignored);
    fs::remove(absolute, ignored);
    throw;
  }
}

template<class Character>
int run(int argc, Character** argv) {
  if (argc == 2 && fs::path(argv[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (argc != 4) {
    std::cerr << usage;
    return 1;
  }
  try {
    const fs::path output(argv[3]);
    if (output.empty() || output.filename().empty() || fs::exists(fs::symlink_status(output)))
      throw std::invalid_argument("Navigation map output must be a new directory");
    const auto request = readRequest(fs::path(argv[2]));
    auto assets = robotics::loadBimPreprocessedAssets(fs::path(argv[1]), {}, request.import);
    const auto& map = request.map;
    const auto cells = robotics::rasterizeBimNavigationMap(assets.collision, assets.walkableSurfaces,
                                                           request.openDoorIds, map);
    const robotics::BimCollisionWorld world(std::move(assets.collision), request.openDoorIds, 1);
    auto waypoints = verifyWaypoints(request, cells, world.walkabilityQueries());
    std::vector<util::ContentFingerprint> layers;
    layers.reserve(assets.sourceLayers.size());
    for (auto& layer : assets.sourceLayers) layers.push_back(std::move(layer.fingerprint));
    const auto sceneSource = robotics::makeBimSceneSource(std::move(layers), assets.scene.frame);
    const auto pgm = buildImage(map, cells);
    const auto* bytes = reinterpret_cast<const unsigned char*>(pgm.data());
    const auto imageSha = util::FingerprintContent({bytes, pgm.size()}).digest;
    const auto origin = Json::array({map.originMeters.x, map.originMeters.y, 0});
    const auto yaml = "image: map.pgm\nmode: trinary\nresolution: " + Json(map.resolutionMeters).dump() +
                      "\norigin: " + origin.dump() + "\nnegate: 0\noccupied_thresh: 0.65\nfree_thresh: 0.25\n";
    using enum robotics::BimNavigationCell;
    const auto freeCount = std::ranges::count(cells, Free);
    const auto occupiedCount = std::ranges::count(cells, Occupied);
    const Json manifest{{"schema", "container.bim-navigation-map"}, {"version", 1},
        {"frameId", "map"}, {"sourceNamespace", assets.scene.sourceNamespace},
        {"sceneSource", robotics::bimSceneSourceJson(sceneSource)}, {"originMeters", origin},
        {"sizeCells", Json::array({map.width, map.height})}, {"resolutionMeters", map.resolutionMeters},
        {"floorMeters", map.floorMeters}, {"floorToleranceMeters", map.floorToleranceMeters},
        {"radiusMeters", map.robotRadiusMeters}, {"heightMeters", map.robotHeightMeters},
        {"openDoorIds", request.openDoorIds}, {"imageSha256", imageSha}, {"waypoints", std::move(waypoints)},
        {"cells", {{"free", freeCount}, {"occupied", occupiedCount},
                   {"unknown", cells.size() - freeCount - occupiedCount}}}};
    publish(output, pgm, yaml, manifest.dump(2) + "\n");
    std::cout << "Exported BIM navigation map " << util::pathToUtf8(output) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsNavMap: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return run(argc, argv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
