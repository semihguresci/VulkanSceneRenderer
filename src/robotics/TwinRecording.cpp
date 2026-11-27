#include "Container/robotics/TwinTelemetry.h"

#include "Container/robotics/BimSceneSourceManifest.h"
#include "Container/utility/Platform.h"
#include "TwinRecordingJson.h"
#include "TwinTelemetryLimits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace container::robotics {
namespace {

using Json = nlohmann::json;
namespace limits = twin_telemetry_limits;
constexpr std::string_view schema = "container.bim-twin-telemetry";
constexpr uint32_t contactVersion = 4;
constexpr uint32_t currentVersion = contactVersion;
constexpr size_t maxRecordingBytes = 32 * 1024 * 1024;
constexpr size_t maxJsonNodes = 2 * 1024 * 1024;

[[noreturn]] void invalid(std::string_view field, std::string_view problem) {
  throw std::invalid_argument("BIM twin recording " + std::string(field) + ": " + std::string(problem));
}

std::string field(std::string_view context, std::string_view name) {
  return std::string(context) + "." + std::string(name);
}

void requireKeys(const Json& object, std::string_view context, std::initializer_list<std::string_view> keys,
                 std::initializer_list<std::string_view> optional = {}) {
  if (!object.is_object()) invalid(context, "must be an object");
  for (const auto& [key, value] : object.items()) {
    if (std::ranges::find(keys, key) == keys.end() && std::ranges::find(optional, key) == optional.end())
      invalid(field(context, key), "unknown field");
  }
  for (const auto key : keys) {
    if (!object.contains(key)) invalid(field(context, key), "is required");
  }
}

std::string text(const Json& value, std::string_view context) {
  if (!value.is_string()) invalid(context, "must be a string");
  return value.get<std::string>();
}

uint64_t integer(const Json& value, std::string_view context) {
  if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
    invalid(context, "must be a nonnegative 64-bit integer");
  return value.get<uint64_t>();
}

uint32_t dimension(const Json& value, std::string_view context) {
  const auto result = integer(value, context);
  if (result > (std::numeric_limits<uint32_t>::max)())
    invalid(context, "must be a nonnegative 32-bit integer");
  return static_cast<uint32_t>(result);
}

double number(const Json& value, std::string_view context) {
  if (!value.is_number() || !std::isfinite(value.get<double>()))
    invalid(context, "must be a finite number");
  return value.get<double>();
}

glm::dvec3 readPoint(const Json& value, std::string_view context) {
  if (!value.is_array() || value.size() != 3)
    invalid(context, "must be an XYZ array with three components");
  return {number(value[0], context), number(value[1], context), number(value[2], context)};
}

glm::dmat4 readMatrix(const Json& value, std::string_view context) {
  if (!value.is_array() || value.size() != 16)
    invalid(context, "must be a flat array of 16 column-major entries");
  glm::dmat4 result(1.0);
  for (glm::length_t column = 0; column < 4; ++column)
    for (glm::length_t row = 0; row < 4; ++row)
      result[column][row] = number(value[static_cast<size_t>(4 * column + row)], context);
  return result;
}

Json parseJson(std::string_view input) {
  if (input.empty() || input.size() > maxRecordingBytes)
    invalid("JSON", "must contain at most 32 MiB of data");
  if (input.find('\0') != std::string_view::npos)
    invalid("JSON", "must not contain a raw NUL byte");
  std::vector<std::set<std::string>> objectKeys;
  size_t nodeCount = 0;
  const Json::parser_callback_t guard = [&](int depth, Json::parse_event_t event, Json& parsed) {
    if (depth > 16) invalid("JSON", "nesting exceeds the schema limit");
    using enum Json::parse_event_t;
    if (event == object_start) objectKeys.emplace_back();
    if (event == object_end) objectKeys.pop_back();
    if (event == key) {
      const auto& name = parsed.get_ref<const std::string&>();
      auto& keys = objectKeys.back();
      if (name.size() > 128 || keys.size() >= 16) invalid("JSON", "object key limit exceeded");
      if (!keys.insert(name).second) invalid("JSON", "duplicate key " + name);
    }
    if ((event == value || event == object_start || event == array_start) && ++nodeCount > maxJsonNodes)
      invalid("JSON", "value count exceeds the schema limit");
    return true;
  };
  try {
    return Json::parse(input, guard);
  } catch (const Json::exception& error) {
    invalid("JSON", error.what());
  }
}

std::optional<double> optionalNumber(const Json& value, std::string_view key, std::string_view context) {
  const auto result = value.find(key);
  if (result == value.end() || result->is_null()) return std::nullopt;
  return number(*result, field(context, key));
}

TwinPayload readPayload(std::string_view kind, const Json& value, std::string_view context, uint32_t version) {
  if (kind == "robot-pose") {
    requireKeys(value, context, {"position", "yaw"}, {"contact"});
    TwinRobotPose pose{readPoint(value.at("position"), field(context, "position")),
                       number(value.at("yaw"), field(context, "yaw"))};
    if (const auto contact = value.find("contact"); contact != value.end()) {
      const auto contactContext = field(context, "contact");
      if (version < contactVersion) invalid(contactContext, "requires recording version 4");
      pose.contact = twin_json::parseRobotContact(*contact, contactContext);
    }
    return pose;
  }
  if (kind == "sensor-status" || kind == "bim-asset-state") {
    requireKeys(value, context, {"state"});
    auto state = text(value.at("state"), field(context, "state"));
    if (kind == "sensor-status") return TwinSensorStatus{std::move(state)};
    return TwinAssetState{std::move(state)};
  }
  if (kind == "planned-path") {
    requireKeys(value, context, {"points"});
    const auto& points = value.at("points");
    if (!points.is_array() || points.size() > limits::maxPlanPoints)
      invalid(field(context, "points"), "must be an array within the plan size limit");
    TwinPlannedPath result;
    result.points.reserve(points.size());
    const auto pointContext = field(context, "points");
    for (const auto& point : points) result.points.push_back(readPoint(point, pointContext));
    return result;
  }
  if (kind == "robot-health") {
    if (version < 2) invalid(context, "robot-health requires recording version 2");
    requireKeys(value, context, {"state"}, {"batteryPercent"});
    return TwinRobotHealth{text(value.at("state"), field(context, "state")),
                           optionalNumber(value, "batteryPercent", context)};
  }
  if (kind == "task-status") {
    if (version < 2) invalid(context, "task-status requires recording version 2");
    requireKeys(value, context, {"taskId", "state"}, {"progress"});
    return TwinTaskStatus{text(value.at("taskId"), field(context, "taskId")),
                          text(value.at("state"), field(context, "state")),
                          optionalNumber(value, "progress", context)};
  }
  if (kind == "operator-alarm") {
    if (version < 3) invalid(context, "operator-alarm requires recording version 3");
    requireKeys(value, context, {"alarmId", "severity", "message", "active", "position"});
    if (!value.at("active").is_boolean()) invalid(field(context, "active"), "must be a boolean");
    return TwinOperatorAlarm{
        text(value.at("alarmId"), field(context, "alarmId")),
        text(value.at("severity"), field(context, "severity")),
        text(value.at("message"), field(context, "message")),
        value.at("active").get<bool>(),
        readPoint(value.at("position"), field(context, "position"))};
  }
  if (kind == "sensor-frustum") {
    if (version < 3) invalid(context, "sensor-frustum requires recording version 3");
    requireKeys(value, context, {"width", "height", "fx", "fy", "cx", "cy", "nearMeters", "farMeters", "worldFromCamera"});
    return TwinSensorFrustum{
        .width = dimension(value.at("width"), field(context, "width")),
        .height = dimension(value.at("height"), field(context, "height")),
        .fx = number(value.at("fx"), field(context, "fx")),
        .fy = number(value.at("fy"), field(context, "fy")),
        .cx = number(value.at("cx"), field(context, "cx")),
        .cy = number(value.at("cy"), field(context, "cy")),
        .nearMeters = number(value.at("nearMeters"), field(context, "nearMeters")),
        .farMeters = number(value.at("farMeters"), field(context, "farMeters")),
        .worldFromCamera = readMatrix(value.at("worldFromCamera"), field(context, "worldFromCamera"))};
  }
  if (kind == "occupancy-grid") {
    if (version < 3) invalid(context, "occupancy-grid requires recording version 3");
    requireKeys(value, context, {"origin", "yaw", "resolutionMeters", "width", "height", "cells"});
    return TwinOccupancyGrid{
        .origin = readPoint(value.at("origin"), field(context, "origin")),
        .yaw = number(value.at("yaw"), field(context, "yaw")),
        .resolutionMeters = number(value.at("resolutionMeters"), field(context, "resolutionMeters")),
        .width = dimension(value.at("width"), field(context, "width")),
        .height = dimension(value.at("height"), field(context, "height")),
        .cells = text(value.at("cells"), field(context, "cells"))};
  }
  invalid(context, "unknown event kind");
}

TwinEvent readEvent(const Json& value, std::string_view context, uint32_t version) {
  requireKeys(value, context, {"timestampNanoseconds", "entityId", "source", "kind", "payload"});
  const auto kind = text(value.at("kind"), field(context, "kind"));
  return {integer(value.at("timestampNanoseconds"), field(context, "timestampNanoseconds")),
          text(value.at("entityId"), field(context, "entityId")),
          text(value.at("source"), field(context, "source")),
          readPayload(kind, value.at("payload"), field(context, "payload"), version)};
}

} // namespace

namespace twin_json {

BimSceneSource parseSceneSource(const Json& source) {
  requireKeys(source, "sceneSource", {"layers", "metersPerUnscaledRendererUnit"});
  const auto& layers = source.at("layers");
  if (!layers.is_array() || layers.empty() || layers.size() > limits::maxSceneLayers)
    invalid("sceneSource.layers", "must be a nonempty array within the source layer limit");
  BimSceneSource result;
  result.metersPerUnscaledRendererUnit =
      number(source.at("metersPerUnscaledRendererUnit"), "sceneSource.metersPerUnscaledRendererUnit");
  result.layers.reserve(layers.size());
  for (const auto& layer : layers) {
    requireKeys(layer, "sceneSource.layers[]", {"sha256", "byteSize"});
    result.layers.push_back({text(layer.at("sha256"), "sceneSource.layers[].sha256"),
                             integer(layer.at("byteSize"), "sceneSource.layers[].byteSize")});
  }
  return result;
}

Json point(const glm::dvec3& value) {
  return Json::array({value.x, value.y, value.z});
}

TwinRobotContact parseRobotContact(const Json& contact, std::string_view context) {
  requireKeys(contact, context, {"otherEntityId", "positionMeters"}, {"normal"});
  TwinRobotContact result{
      .otherEntityId = text(contact.at("otherEntityId"), field(context, "otherEntityId")),
      .position = readPoint(contact.at("positionMeters"), field(context, "positionMeters"))};
  if (const auto normal = contact.find("normal"); normal != contact.end())
    result.normal = readPoint(*normal, field(context, "normal"));
  return result;
}

Json event(const TwinEvent& value) {
  std::string_view kind;
  Json payload;
  std::visit([&](const auto& data) {
    using T = std::decay_t<decltype(data)>;
    if constexpr (std::is_same_v<T, TwinRobotPose>) {
      kind = "robot-pose";
      payload = {{"position", point(data.position)}, {"yaw", data.yaw}};
      if (data.contact) {
        auto& contact = payload["contact"];
        contact = {{"otherEntityId", data.contact->otherEntityId}, {"positionMeters", point(data.contact->position)}};
        if (data.contact->normal) contact["normal"] = point(*data.contact->normal);
      }
    } else if constexpr (std::is_same_v<T, TwinSensorStatus>) {
      kind = "sensor-status";
      payload = {{"state", data.state}};
    } else if constexpr (std::is_same_v<T, TwinAssetState>) {
      kind = "bim-asset-state";
      payload = {{"state", data.state}};
    } else if constexpr (std::is_same_v<T, TwinPlannedPath>) {
      kind = "planned-path";
      auto points = Json::array();
      for (const auto& entry : data.points) points.push_back(point(entry));
      payload = {{"points", std::move(points)}};
    } else if constexpr (std::is_same_v<T, TwinRobotHealth>) {
      kind = "robot-health";
      payload = {{"state", data.state}};
      if (data.batteryPercent) payload["batteryPercent"] = *data.batteryPercent;
    } else if constexpr (std::is_same_v<T, TwinTaskStatus>) {
      kind = "task-status";
      payload = {{"taskId", data.taskId}, {"state", data.state}};
      if (data.progress) payload["progress"] = *data.progress;
    } else if constexpr (std::is_same_v<T, TwinOperatorAlarm>) {
      kind = "operator-alarm";
      payload = {{"alarmId", data.alarmId}, {"severity", data.severity}, {"message", data.message},
                 {"active", data.active}, {"position", point(data.position)}};
    } else if constexpr (std::is_same_v<T, TwinSensorFrustum>) {
      kind = "sensor-frustum";
      auto matrix = Json::array();
      for (glm::length_t column = 0; column < 4; ++column)
        for (glm::length_t row = 0; row < 4; ++row)
          matrix.push_back(data.worldFromCamera[column][row]);
      payload = {{"width", data.width}, {"height", data.height},
                 {"fx", data.fx}, {"fy", data.fy}, {"cx", data.cx}, {"cy", data.cy},
                 {"nearMeters", data.nearMeters}, {"farMeters", data.farMeters},
                 {"worldFromCamera", std::move(matrix)}};
    } else if constexpr (std::is_same_v<T, TwinOccupancyGrid>) {
      kind = "occupancy-grid";
      payload = {{"origin", point(data.origin)}, {"yaw", data.yaw}, {"resolutionMeters", data.resolutionMeters},
                 {"width", data.width}, {"height", data.height}, {"cells", data.cells}};
    }
  }, value.payload);
  return {{"timestampNanoseconds", value.timestampNanoseconds}, {"entityId", value.entityId},
          {"source", value.source}, {"kind", kind}, {"payload", std::move(payload)}};
}

uint32_t recordingVersion(const TwinRecording& value) {
  uint32_t version = 1;
  for (const auto& entry : value.events) {
    if (const auto* pose = std::get_if<TwinRobotPose>(&entry.payload); pose && pose->contact)
      return contactVersion;
    if (std::holds_alternative<TwinOperatorAlarm>(entry.payload) ||
        std::holds_alternative<TwinSensorFrustum>(entry.payload) ||
        std::holds_alternative<TwinOccupancyGrid>(entry.payload))
      version = 3;
    else if (version < 2 &&
             (std::holds_alternative<TwinRobotHealth>(entry.payload) ||
              std::holds_alternative<TwinTaskStatus>(entry.payload)))
      version = 2;
  }
  return version;
}

TwinIngestMessage parseIngestMessage(std::string_view line) {
  if (line.size() > maxIngestLineBytes) invalid("JSONL row", "exceeds 512 KiB");
  const auto value = parseJson(line);
  requireKeys(value, "envelope", {"schema", "version", "clockId", "receivedTimestampNanoseconds", "event"},
              {"assetReference"});
  if (text(value.at("schema"), "envelope.schema") != "container.bim-twin-event" ||
      integer(value.at("version"), "envelope.version") != 1)
    invalid("envelope", "expected container.bim-twin-event version 1");
  TwinIngestMessage result{
      .clockId = text(value.at("clockId"), "envelope.clockId"),
      .receivedTimestampNanoseconds = integer(value.at("receivedTimestampNanoseconds"),
                                              "envelope.receivedTimestampNanoseconds"),
      .event = readEvent(value.at("event"), "envelope.event", currentVersion)};
  if (const auto reference = value.find("assetReference"); reference != value.end()) {
    const auto kind = text(*reference, "envelope.assetReference");
    if (!std::holds_alternative<TwinAssetState>(result.event.payload))
      invalid("envelope.assetReference", "applies only to bim-asset-state");
    if (kind == "ifc-guid") result.assetReference = TwinAssetReference::IfcGuid;
    else if (kind != "entity-id") invalid("envelope.assetReference", "expected entity-id or ifc-guid");
  }
  return result;
}

} // namespace twin_json

TwinRecording parseTwinRecording(std::string_view input) {
  const auto json = parseJson(input);
  requireKeys(json, "root", {"schema", "version", "sourceNamespace", "sceneSource", "clockId",
                             "staleAfterNanoseconds", "events"});
  const auto version = integer(json.at("version"), "version");
  if (text(json.at("schema"), "schema") != schema || version < 1 || version > currentVersion)
    invalid("schema", "expected container.bim-twin-telemetry version 1 through 4");
  TwinRecording result{
      .sourceNamespace = text(json.at("sourceNamespace"), "sourceNamespace"),
      .sceneSource = twin_json::parseSceneSource(json.at("sceneSource")),
      .clockId = text(json.at("clockId"), "clockId"),
      .staleAfterNanoseconds = integer(json.at("staleAfterNanoseconds"), "staleAfterNanoseconds")};
  const auto& events = json.at("events");
  if (!events.is_array() || events.empty() || events.size() > limits::maxEvents)
    invalid("events", "must be a nonempty array within the event count limit");
  result.events.reserve(events.size());
  for (const auto& entry : events) {
    const auto context = "events[" + std::to_string(result.events.size()) + "]";
    result.events.push_back(readEvent(entry, context, static_cast<uint32_t>(version)));
  }
  canonicalizeTwinRecording(result);
  return result;
}

TwinRecording loadTwinRecording(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto filename = util::pathToUtf8(path);
  if (!file) throw std::runtime_error("Could not open BIM twin recording " + filename);
  const auto length = file.tellg();
  if (length <= 0 || length > static_cast<std::streamoff>(maxRecordingBytes))
    invalid(filename, "must contain at most 32 MiB of data");
  std::string bytes(static_cast<size_t>(length), '\0');
  file.seekg(0);
  if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) || file.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Could not read a complete BIM twin recording " + filename);
  return parseTwinRecording(bytes);
}

std::string serializeTwinRecording(const TwinRecording& recording) {
  auto ordered = recording;
  canonicalizeTwinRecording(ordered);
  auto events = Json::array();
  for (const auto& entry : ordered.events) events.push_back(twin_json::event(entry));
  const Json json{{"schema", schema}, {"version", twin_json::recordingVersion(ordered)}, {"sourceNamespace", ordered.sourceNamespace},
      {"sceneSource", bimSceneSourceJson(ordered.sceneSource)}, {"clockId", ordered.clockId},
      {"staleAfterNanoseconds", ordered.staleAfterNanoseconds}, {"events", std::move(events)}};
  auto bytes = json.dump();
  bytes += '\n';
  if (bytes.size() > maxRecordingBytes) invalid("JSON", "encoded recording exceeds 32 MiB");
  return bytes;
}

} // namespace container::robotics
