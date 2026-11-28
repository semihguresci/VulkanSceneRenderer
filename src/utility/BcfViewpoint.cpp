#include "Container/utility/BcfViewpoint.h"

#include "Container/renderer/bim/BimCoordinationOverlay.h"
#include "Container/renderer/bim/BimManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <miniz/miniz.h>

namespace container::ui::bcf {
namespace {

constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

std::string escapeXml(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char c : value) {
    switch (c) {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    case '\'':
      escaped += "&apos;";
      break;
    default:
      escaped.push_back(c);
      break;
    }
  }
  return escaped;
}

std::string unescapeXml(std::string value) {
  auto replaceAll = [](std::string& text, std::string_view from,
                       std::string_view to) {
    size_t offset = 0;
    while ((offset = text.find(from, offset)) != std::string::npos) {
      text.replace(offset, from.size(), to);
      offset += to.size();
    }
  };
  replaceAll(value, "&quot;", "\"");
  replaceAll(value, "&apos;", "'");
  replaceAll(value, "&gt;", ">");
  replaceAll(value, "&lt;", "<");
  replaceAll(value, "&amp;", "&");
  return value;
}

std::string scalar(float value) {
  std::ostringstream stream;
  stream << std::setprecision(9) << value;
  return stream.str();
}

std::string scalar(uint32_t value) {
  return value == kInvalidIndex ? "" : std::to_string(value);
}

std::string scalar(bool value) { return value ? "true" : "false"; }

uint64_t appendFnv1a(uint64_t hash, std::string_view value) {
  constexpr uint64_t kFnvPrime = 1099511628211ull;
  for (const char c : value) {
    hash ^= static_cast<unsigned char>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

std::string hex64(uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

std::string pinGuid(const BcfPin& pin, std::string_view topicGuid) {
  if (!pin.guid.empty()) {
    return pin.guid;
  }
  return StablePinGuid(pin.ifcGuid, pin.sourceId, topicGuid);
}

glm::vec3 cameraDirection(const TransformControls& camera) {
  const float yaw = glm::radians(camera.rotationDegrees.y);
  const float pitch = glm::radians(camera.rotationDegrees.x);
  glm::vec3 forward{};
  forward.x = std::cos(yaw) * std::cos(pitch);
  forward.y = std::sin(pitch);
  forward.z = -std::sin(yaw) * std::cos(pitch);
  const float length = glm::length(forward);
  return length > 0.0f ? forward / length : glm::vec3{0.0f, 0.0f, -1.0f};
}

glm::vec3 cameraUp(const glm::vec3& forward) {
  const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
  glm::vec3 right = glm::cross(forward, worldUp);
  if (glm::length(right) <= 1.0e-6f) {
    right = {1.0f, 0.0f, 0.0f};
  } else {
    right = glm::normalize(right);
  }
  return glm::normalize(glm::cross(right, forward));
}

bool finiteVector(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool nonDefaultVector(const glm::vec3& value) {
  return value.x != 0.0f || value.y != 0.0f || value.z != 0.0f;
}

std::string lowerAscii(std::string_view value) {
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

std::string_view disciplinePresetName(
    container::renderer::BimDisciplinePreset preset) {
  switch (preset) {
  case container::renderer::BimDisciplinePreset::None:
    return "None";
  case container::renderer::BimDisciplinePreset::Architecture:
    return "Architecture";
  case container::renderer::BimDisciplinePreset::MepXray:
    return "MepXray";
  }
  return "None";
}

container::renderer::BimDisciplinePreset disciplinePresetFromString(
    std::string_view value,
    container::renderer::BimDisciplinePreset fallback) {
  const std::string lowered = lowerAscii(value);
  if (lowered == "none" || lowered == "0") {
    return container::renderer::BimDisciplinePreset::None;
  }
  if (lowered == "architecture" || lowered == "1") {
    return container::renderer::BimDisciplinePreset::Architecture;
  }
  if (lowered == "mepxray" || lowered == "mep-xray" ||
      lowered == "mep_xray" || lowered == "2") {
    return container::renderer::BimDisciplinePreset::MepXray;
  }
  return fallback;
}

void writeVector(std::ostringstream& stream, std::string_view tag,
                 const glm::vec3& value, int indent) {
  const std::string spaces(static_cast<size_t>(indent), ' ');
  stream << spaces << '<' << tag << ">\n";
  stream << spaces << "  <X>" << scalar(value.x) << "</X>\n";
  stream << spaces << "  <Y>" << scalar(value.y) << "</Y>\n";
  stream << spaces << "  <Z>" << scalar(value.z) << "</Z>\n";
  stream << spaces << "</" << tag << ">\n";
}

void writeTextElement(std::ostringstream& stream, std::string_view tag,
                      std::string_view value, int indent) {
  const std::string spaces(static_cast<size_t>(indent), ' ');
  stream << spaces << '<' << tag << '>' << escapeXml(value) << "</" << tag
         << ">\n";
}

void writeTextElements(std::ostringstream& stream, std::string_view groupTag,
                       std::string_view itemTag,
                       const std::vector<std::string>& values, int indent) {
  if (values.empty()) {
    return;
  }
  const std::string spaces(static_cast<size_t>(indent), ' ');
  stream << spaces << '<' << groupTag << ">\n";
  for (const std::string& value : values) {
    writeTextElement(stream, itemTag, value, indent + 2);
  }
  stream << spaces << "</" << groupTag << ">\n";
}

void writeAttribute(std::ostringstream& stream, std::string_view name,
                    std::string_view value) {
  stream << ' ' << name << "=\"" << escapeXml(value) << '"';
}

void writeOptionalAttribute(std::ostringstream& stream, std::string_view name,
                            std::string_view value) {
  if (!value.empty()) {
    writeAttribute(stream, name, value);
  }
}

void writeAttribute(std::ostringstream& stream, std::string_view name,
                    bool value) {
  writeAttribute(stream, name, scalar(value));
}

void writeAttribute(std::ostringstream& stream, std::string_view name,
                    float value) {
  writeAttribute(stream, name, scalar(value));
}

void writeAttribute(std::ostringstream& stream, std::string_view name,
                    uint32_t value) {
  writeAttribute(stream, name, scalar(value));
}

bool hasContainerMarkup(const BcfViewpointMarkup& markup) {
  return !markup.guid.empty() || !markup.viewpointFile.empty() ||
         !markup.snapshotFile.empty() || !markup.pins.empty() ||
         !markup.lines.empty();
}

void writePin(std::ostringstream& stream, const BcfPin& pin, int indent,
              std::string_view topicGuid) {
  const std::string spaces(static_cast<size_t>(indent), ' ');
  const bool writeLocation =
      finiteVector(pin.location) &&
      (pin.hasLocation || nonDefaultVector(pin.location));
  stream << spaces << "<Pin";
  writeOptionalAttribute(stream, "Guid", pinGuid(pin, topicGuid));
  writeOptionalAttribute(stream, "Label", pin.label);
  writeOptionalAttribute(stream, "IfcGuid", pin.ifcGuid);
  writeOptionalAttribute(stream, "AuthoringToolId", pin.sourceId);
  if (!writeLocation) {
    stream << "/>\n";
    return;
  }
  stream << ">\n";
  writeVector(stream, "Location", pin.location, indent + 2);
  stream << spaces << "</Pin>\n";
}

void writeMarkupLine(std::ostringstream& stream, const BcfMarkupLine& line,
                     int indent) {
  const std::string spaces(static_cast<size_t>(indent), ' ');
  stream << spaces << "<MarkupLine";
  writeOptionalAttribute(stream, "Guid", line.guid);
  writeOptionalAttribute(stream, "Label", line.label);
  writeAttribute(stream, "ColorR", line.color.r);
  writeAttribute(stream, "ColorG", line.color.g);
  writeAttribute(stream, "ColorB", line.color.b);
  stream << ">\n";
  writeVector(stream, "Start", line.start, indent + 2);
  writeVector(stream, "End", line.end, indent + 2);
  stream << spaces << "</MarkupLine>\n";
}

void writeContainerMarkup(std::ostringstream& stream,
                          const BcfViewpointMarkup& markup, int indent,
                          std::string_view topicGuid = {}) {
  const std::string spaces(static_cast<size_t>(indent), ' ');
  stream << spaces << "<ContainerMarkup";
  writeOptionalAttribute(stream, "Guid", markup.guid);
  writeOptionalAttribute(stream, "Viewpoint", markup.viewpointFile);
  writeOptionalAttribute(stream, "Snapshot", markup.snapshotFile);
  if (markup.pins.empty() && markup.lines.empty()) {
    stream << "/>\n";
    return;
  }
  stream << ">\n";
  for (const BcfPin& pin : markup.pins) {
    writePin(stream, pin, indent + 2, topicGuid);
  }
  for (const BcfMarkupLine& line : markup.lines) {
    writeMarkupLine(stream, line, indent + 2);
  }
  stream << spaces << "</ContainerMarkup>\n";
}

void writeFilter(std::ostringstream& stream, const BimFilterState& filter) {
  stream << "    <BimFilter";
  writeAttribute(stream, "TypeEnabled", filter.typeFilterEnabled);
  writeAttribute(stream, "Type", filter.type);
  writeAttribute(stream, "StoreyEnabled", filter.storeyFilterEnabled);
  writeAttribute(stream, "Storey", filter.storey);
  writeAttribute(stream, "MaterialEnabled", filter.materialFilterEnabled);
  writeAttribute(stream, "Material", filter.material);
  writeAttribute(stream, "DisciplineEnabled", filter.disciplineFilterEnabled);
  writeAttribute(stream, "Discipline", filter.discipline);
  writeAttribute(stream, "DisciplinePreset",
                 disciplinePresetName(filter.disciplinePreset));
  writeAttribute(stream, "PhaseEnabled", filter.phaseFilterEnabled);
  writeAttribute(stream, "Phase", filter.phase);
  writeAttribute(stream, "FireRatingEnabled", filter.fireRatingFilterEnabled);
  writeAttribute(stream, "FireRating", filter.fireRating);
  writeAttribute(stream, "LoadBearingEnabled", filter.loadBearingFilterEnabled);
  writeAttribute(stream, "LoadBearing", filter.loadBearing);
  writeAttribute(stream, "StatusEnabled", filter.statusFilterEnabled);
  writeAttribute(stream, "Status", filter.status);
  writeAttribute(stream, "IsolateSelection", filter.isolateSelection);
  writeAttribute(stream, "HideSelection", filter.hideSelection);
  stream << "/>\n";
}

void writePhaseTimeline(std::ostringstream& stream,
                        const BimPhaseTimelineUiState& timeline) {
  stream << "    <PhaseTimeline";
  writeAttribute(stream, "Enabled", timeline.enabled);
  writeAttribute(stream, "ActivePhaseIndex", timeline.activePhaseIndex);
  writeAttribute(stream, "ShowExisting", timeline.showExisting);
  writeAttribute(stream, "ShowNew", timeline.showNew);
  writeAttribute(stream, "ShowDemolished", timeline.showDemolished);
  writeAttribute(stream, "GhostFuture", timeline.ghostFuture);
  stream << "/>\n";
}

std::optional<std::string_view> openTag(std::string_view xml,
                                        std::string_view tag) {
  const std::string opening = "<" + std::string(tag);
  size_t begin = 0;
  while ((begin = xml.find(opening, begin)) != std::string_view::npos) {
    const size_t afterName = begin + opening.size();
    if (afterName >= xml.size() || xml[afterName] == '>' ||
        xml[afterName] == '/' ||
        std::isspace(static_cast<unsigned char>(xml[afterName])) != 0) {
      const size_t end = xml.find('>', begin);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      return xml.substr(begin, end - begin + 1u);
    }
    begin = afterName;
  }
  return std::nullopt;
}

std::string trim(std::string_view value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1u])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

std::optional<std::string> attribute(std::string_view tag,
                                     std::string_view name) {
  const std::string pattern = std::string(name) + "=\"";
  size_t begin = tag.find(pattern);
  while (begin != std::string_view::npos) {
    if (begin == 0u ||
        std::isspace(static_cast<unsigned char>(tag[begin - 1u])) != 0 ||
        tag[begin - 1u] == '<') {
      break;
    }
    begin = tag.find(pattern, begin + 1u);
  }
  if (begin == std::string_view::npos) return std::nullopt;

  const size_t valueBegin = begin + pattern.size();
  const size_t valueEnd = tag.find('"', valueBegin);
  if (valueEnd == std::string_view::npos) {
    return std::nullopt;
  }
  return unescapeXml(std::string(tag.substr(valueBegin, valueEnd - valueBegin)));
}

float floatAttribute(std::string_view tag, std::string_view name,
                     float fallback) {
  const auto value = attribute(tag, name);
  if (!value || value->empty()) {
    return fallback;
  }
  char* end = nullptr;
  const double parsed = std::strtod(value->c_str(), &end);
  if (end == value->c_str() || end == nullptr || *end != '\0' ||
      !std::isfinite(parsed) ||
      std::abs(parsed) > std::numeric_limits<float>::max()) {
    return fallback;
  }
  return static_cast<float>(parsed);
}

uint32_t uintAttribute(std::string_view tag, std::string_view name,
                       uint32_t fallback) {
  const auto value = attribute(tag, name);
  if (!value || value->empty()) {
    return fallback;
  }
  try {
    const unsigned long parsed = std::stoul(*value);
    if (parsed <= std::numeric_limits<uint32_t>::max()) {
      return static_cast<uint32_t>(parsed);
    }
  } catch (...) {
  }
  return fallback;
}

bool boolAttribute(std::string_view tag, std::string_view name,
                   bool fallback) {
  auto value = attribute(tag, name);
  if (!value) {
    return fallback;
  }
  std::ranges::transform(*value, value->begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (*value == "true" || *value == "1") {
    return true;
  }
  if (*value == "false" || *value == "0") {
    return false;
  }
  return fallback;
}

std::string stringAttribute(std::string_view tag, std::string_view name) {
  return attribute(tag, name).value_or("");
}

std::string firstStringAttribute(std::string_view tag,
                                 std::initializer_list<std::string_view> names) {
  for (std::string_view name : names) {
    if (auto value = attribute(tag, name)) {
      return *value;
    }
  }
  return {};
}

void readFilter(std::string_view tag, BimFilterState& filter) {
  filter.typeFilterEnabled = boolAttribute(tag, "TypeEnabled", false);
  filter.type = stringAttribute(tag, "Type");
  filter.storeyFilterEnabled = boolAttribute(tag, "StoreyEnabled", false);
  filter.storey = stringAttribute(tag, "Storey");
  filter.materialFilterEnabled = boolAttribute(tag, "MaterialEnabled", false);
  filter.material = stringAttribute(tag, "Material");
  filter.disciplineFilterEnabled =
      boolAttribute(tag, "DisciplineEnabled", false);
  filter.discipline = stringAttribute(tag, "Discipline");
  filter.disciplinePreset = disciplinePresetFromString(
      stringAttribute(tag, "DisciplinePreset"), filter.disciplinePreset);
  filter.phaseFilterEnabled = boolAttribute(tag, "PhaseEnabled", false);
  filter.phase = stringAttribute(tag, "Phase");
  filter.fireRatingFilterEnabled =
      boolAttribute(tag, "FireRatingEnabled", false);
  filter.fireRating = stringAttribute(tag, "FireRating");
  filter.loadBearingFilterEnabled =
      boolAttribute(tag, "LoadBearingEnabled", false);
  filter.loadBearing = stringAttribute(tag, "LoadBearing");
  filter.statusFilterEnabled = boolAttribute(tag, "StatusEnabled", false);
  filter.status = stringAttribute(tag, "Status");
  filter.isolateSelection = boolAttribute(tag, "IsolateSelection", false);
  filter.hideSelection = boolAttribute(tag, "HideSelection", false);
}

void readPhaseTimeline(std::string_view tag,
                       BimPhaseTimelineUiState& timeline) {
  timeline.enabled = boolAttribute(tag, "Enabled", false);
  timeline.activePhaseIndex =
      uintAttribute(tag, "ActivePhaseIndex", timeline.activePhaseIndex);
  timeline.showExisting = boolAttribute(tag, "ShowExisting", true);
  timeline.showNew = boolAttribute(tag, "ShowNew", true);
  timeline.showDemolished = boolAttribute(tag, "ShowDemolished", false);
  timeline.ghostFuture = boolAttribute(tag, "GhostFuture", false);
}

std::optional<std::string_view> tagBody(std::string_view xml,
                                        std::string_view tag) {
  const std::string opening = "<" + std::string(tag);
  size_t begin = 0;
  while ((begin = xml.find(opening, begin)) != std::string_view::npos) {
    const size_t afterName = begin + opening.size();
    if (afterName < xml.size() && xml[afterName] != '>' &&
        xml[afterName] != '/' &&
        std::isspace(static_cast<unsigned char>(xml[afterName])) == 0) {
      begin = afterName;
      continue;
    }
    const size_t openEnd = xml.find('>', begin);
    if (openEnd == std::string_view::npos || xml[openEnd - 1u] == '/') {
      return std::nullopt;
    }
    const std::string closing = "</" + std::string(tag) + ">";
    const size_t closeBegin = xml.find(closing, openEnd + 1u);
    if (closeBegin == std::string_view::npos) {
      return std::nullopt;
    }
    return xml.substr(openEnd + 1u, closeBegin - openEnd - 1u);
  }
  return std::nullopt;
}

std::string stringElement(std::string_view xml, std::string_view tag) {
  const auto body = tagBody(xml, tag);
  return body ? unescapeXml(trim(*body)) : std::string{};
}

struct XmlElementView {
  std::string_view tag;
  std::string_view body;
};

bool tagNameBoundary(std::string_view xml, size_t afterName) {
  return afterName >= xml.size() || xml[afterName] == '>' ||
         xml[afterName] == '/' ||
         std::isspace(static_cast<unsigned char>(xml[afterName])) != 0;
}

std::vector<XmlElementView> elements(std::string_view xml,
                                     std::string_view tag) {
  std::vector<XmlElementView> views;
  const std::string opening = "<" + std::string(tag);
  const std::string closing = "</" + std::string(tag) + ">";
  size_t begin = 0;
  while ((begin = xml.find(opening, begin)) != std::string_view::npos) {
    const size_t afterName = begin + opening.size();
    if (!tagNameBoundary(xml, afterName)) {
      begin = afterName;
      continue;
    }
    const size_t openEnd = xml.find('>', begin);
    if (openEnd == std::string_view::npos) {
      break;
    }

    const std::string_view tagView = xml.substr(begin, openEnd - begin + 1u);
    if (openEnd > begin && xml[openEnd - 1u] == '/') {
      views.push_back({tagView, {}});
      begin = openEnd + 1u;
      continue;
    }

    size_t search = openEnd + 1u;
    size_t closeBegin = std::string_view::npos;
    uint32_t depth = 1u;
    while (depth > 0u) {
      size_t nextOpen = xml.find(opening, search);
      while (nextOpen != std::string_view::npos &&
             !tagNameBoundary(xml, nextOpen + opening.size())) {
        nextOpen = xml.find(opening, nextOpen + opening.size());
      }
      const size_t nextClose = xml.find(closing, search);
      if (nextClose == std::string_view::npos) {
        closeBegin = std::string_view::npos;
        break;
      }
      if (nextOpen != std::string_view::npos && nextOpen < nextClose) {
        ++depth;
        search = nextOpen + opening.size();
        continue;
      }
      --depth;
      if (depth == 0u) {
        closeBegin = nextClose;
      }
      search = nextClose + closing.size();
    }
    if (closeBegin == std::string_view::npos) {
      break;
    }
    views.push_back(
        {tagView, xml.substr(openEnd + 1u, closeBegin - openEnd - 1u)});
    begin = closeBegin + closing.size();
  }
  return views;
}

std::vector<std::string> stringElements(std::string_view xml,
                                        std::string_view tag) {
  std::vector<std::string> values;
  for (const XmlElementView element : elements(xml, tag)) {
    values.push_back(unescapeXml(trim(element.body)));
  }
  return values;
}

std::vector<std::string> groupedStringElements(std::string_view xml,
                                               std::string_view groupTag,
                                               std::string_view itemTag) {
  const auto body = tagBody(xml, groupTag);
  return body ? stringElements(*body, itemTag) : std::vector<std::string>{};
}

std::optional<glm::vec3> vectorElement(std::string_view xml,
                                       std::string_view tag);

std::vector<BcfPin> readPins(std::string_view xml) {
  std::vector<BcfPin> pins;
  for (const XmlElementView pinElement : elements(xml, "Pin")) {
    BcfPin pin{};
    pin.guid = stringAttribute(pinElement.tag, "Guid");
    pin.label = stringAttribute(pinElement.tag, "Label");
    pin.ifcGuid = firstStringAttribute(pinElement.tag, {"IfcGuid", "ifcGuid"});
    pin.sourceId = firstStringAttribute(
        pinElement.tag, {"AuthoringToolId", "SourceId", "OriginatingSystem"});
    if (const auto location = vectorElement(pinElement.body, "Location")) {
      pin.hasLocation = true;
      pin.location = *location;
    } else if (const auto point = vectorElement(pinElement.body, "Point")) {
      pin.hasLocation = true;
      pin.location = *point;
    }
    pins.push_back(std::move(pin));
  }
  return pins;
}

std::vector<BcfMarkupLine> readMarkupLines(std::string_view xml) {
  std::vector<BcfMarkupLine> lines;
  for (const XmlElementView lineElement : elements(xml, "MarkupLine")) {
    const auto start = vectorElement(lineElement.body, "Start");
    const auto end = vectorElement(lineElement.body, "End");
    if (!start || !end) {
      continue;
    }

    BcfMarkupLine line{};
    line.guid = stringAttribute(lineElement.tag, "Guid");
    line.label = stringAttribute(lineElement.tag, "Label");
    line.start = *start;
    line.end = *end;
    line.color.r = floatAttribute(lineElement.tag, "ColorR", 1.0f);
    line.color.g = floatAttribute(lineElement.tag, "ColorG", 1.0f);
    line.color.b = floatAttribute(lineElement.tag, "ColorB", 1.0f);
    lines.push_back(std::move(line));
  }
  return lines;
}

BcfViewpointMarkup readViewpointMarkup(std::string_view tag,
                                       std::string_view body) {
  BcfViewpointMarkup markup{};
  markup.guid = stringAttribute(tag, "Guid");
  markup.viewpointFile = stringAttribute(tag, "Viewpoint");
  markup.snapshotFile = stringAttribute(tag, "Snapshot");
  if (const auto containerBody = tagBody(body, "ContainerMarkup")) {
    if (const auto containerTag = openTag(body, "ContainerMarkup")) {
      if (markup.guid.empty()) {
        markup.guid = stringAttribute(*containerTag, "Guid");
      }
      if (markup.viewpointFile.empty()) {
        markup.viewpointFile = stringAttribute(*containerTag, "Viewpoint");
      }
      if (markup.snapshotFile.empty()) {
        markup.snapshotFile = stringAttribute(*containerTag, "Snapshot");
      }
    }
    markup.pins = readPins(*containerBody);
    markup.lines = readMarkupLines(*containerBody);
  } else {
    markup.pins = readPins(body);
    markup.lines = readMarkupLines(body);
  }
  return markup;
}

void ensurePinGuids(BcfMarkup& markup) {
  for (BcfViewpointMarkup& viewpoint : markup.viewpoints) {
    for (BcfPin& pin : viewpoint.pins) {
      if (pin.guid.empty()) {
        pin.guid = StablePinGuid(pin.ifcGuid, pin.sourceId, markup.topic.guid);
      }
    }
  }
}

std::optional<float> floatElement(std::string_view xml,
                                  std::string_view tag) {
  const auto body = tagBody(xml, tag);
  if (!body) {
    return std::nullopt;
  }
  std::string text(*body);
  char* end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || !std::isfinite(parsed) ||
      std::abs(parsed) > std::numeric_limits<float>::max()) {
    return std::nullopt;
  }
  while (*end != '\0') {
    if (std::isspace(static_cast<unsigned char>(*end)) == 0) {
      return std::nullopt;
    }
    ++end;
  }
  return static_cast<float>(parsed);
}

std::optional<glm::vec3> vectorElement(std::string_view xml,
                                       std::string_view tag) {
  const auto body = tagBody(xml, tag);
  if (!body) {
    return std::nullopt;
  }
  const auto x = floatElement(*body, "X");
  const auto y = floatElement(*body, "Y");
  const auto z = floatElement(*body, "Z");
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::vec3{*x, *y, *z};
}

bool readStandardPerspectiveCamera(std::string_view xml,
                                   TransformControls& camera) {
  const auto cameraBody = tagBody(xml, "PerspectiveCamera");
  if (!cameraBody) {
    return false;
  }
  const auto position = vectorElement(*cameraBody, "CameraViewPoint");
  const auto direction = vectorElement(*cameraBody, "CameraDirection");
  if (!position || !direction) {
    return false;
  }

  const float directionLength = glm::length(*direction);
  if (!std::isfinite(directionLength) || directionLength <= 1.0e-6f) {
    return false;
  }
  const glm::vec3 forward = *direction / directionLength;
  camera.position = *position;
  camera.rotationDegrees.x =
      glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
  camera.rotationDegrees.y = glm::degrees(std::atan2(-forward.z, forward.x));
  camera.rotationDegrees.z = 0.0f;
  camera.scale = {1.0f, 1.0f, 1.0f};
  return true;
}

void readStandardComponentSelection(std::string_view xml,
                                    ViewpointSnapshotState& snapshot) {
  const auto selection = tagBody(xml, "Selection");
  for (const auto& component : elements(selection.value_or(xml), "Component")) {
    ViewpointBimComponent identity;
    identity.guid = attribute(component.tag, "IfcGuid").value_or(stringAttribute(component.tag, "ifcGuid"));
    identity.sourceId = attribute(component.tag, "AuthoringToolId").value_or(stringAttribute(component.tag, "OriginatingSystem"));
    if (identity.guid.empty() && identity.sourceId.empty()) continue;
    if (snapshot.selectedBimGuid.empty() && snapshot.selectedBimSourceId.empty()) {
      snapshot.selectedBimGuid = std::move(identity.guid);
      snapshot.selectedBimSourceId = std::move(identity.sourceId);
    } else if (snapshot.selectedBimGuid != identity.guid || snapshot.selectedBimSourceId != identity.sourceId) {
      snapshot.additionalBimSelections.push_back(std::move(identity));
    }
  }
}

std::optional<std::string> readTextFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  try {
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
  } catch (const std::ios_base::failure&) {
    return std::nullopt;
  }
}

bool writeTextFile(const std::filesystem::path& path, std::string_view text) {
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return false;
    }
  }
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file << text;
  return static_cast<bool>(file);
}

bool safeRelativePath(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  for (const std::filesystem::path& part : path) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

std::string defaultViewpointFile(size_t index) {
  std::ostringstream stream;
  stream << "viewpoint-" << std::setfill('0') << std::setw(4)
         << (index + 1u) << ".bcfv";
  return stream.str();
}

BcfViewpointMarkup mergedViewpointMarkup(const BcfViewpointMarkup& markup,
                                         const BcfViewpointMarkup& fallback,
                                         size_t index) {
  BcfViewpointMarkup result = markup;
  if (result.guid.empty()) {
    result.guid = fallback.guid.empty()
                      ? "viewpoint-" + std::to_string(index + 1u)
                      : fallback.guid;
  }
  if (result.viewpointFile.empty()) {
    result.viewpointFile =
        fallback.viewpointFile.empty() ? defaultViewpointFile(index)
                                       : fallback.viewpointFile;
  }
  if (result.snapshotFile.empty()) {
    result.snapshotFile = fallback.snapshotFile;
  }
  if (result.pins.empty()) {
    result.pins = fallback.pins;
  }
  if (result.lines.empty()) {
    result.lines = fallback.lines;
  }
  return result;
}

std::optional<std::string> archiveRelativePath(
    const std::filesystem::path& path) {
  if (!safeRelativePath(path)) {
    return std::nullopt;
  }

  std::string value = path.generic_string();
  std::replace(value.begin(), value.end(), '\\', '/');
  if (value.empty() || value.front() == '/' ||
      value.find(':') != std::string::npos) {
    return std::nullopt;
  }

  size_t begin = 0;
  while (begin <= value.size()) {
    const size_t end = value.find('/', begin);
    const std::string_view part =
        end == std::string::npos
            ? std::string_view(value).substr(begin)
            : std::string_view(value).substr(begin, end - begin);
    if (part.empty() || part == "." || part == "..") {
      return std::nullopt;
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1u;
  }

  return value;
}

std::string archiveTopicFolderName(std::string_view topicGuid) {
  std::string folder;
  folder.reserve(topicGuid.empty() ? 5u : topicGuid.size());
  for (const unsigned char c : topicGuid) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.') {
      folder.push_back(static_cast<char>(c));
    } else {
      folder.push_back('_');
    }
  }
  if (folder.empty() || folder == "." || folder == "..") {
    return "topic";
  }
  return folder;
}

std::string archiveJoin(std::string_view folder, std::string_view relativePath) {
  if (folder.empty()) {
    return std::string(relativePath);
  }
  return std::string(folder) + "/" + std::string(relativePath);
}

BcfMarkup normalizedTopicMarkup(const BcfTopicFolder& topic) {
  BcfMarkup markup = topic.markup;
  const size_t viewpointCount =
      std::max(markup.viewpoints.size(), topic.viewpoints.size());
  markup.viewpoints.resize(viewpointCount);
  for (size_t i = 0; i < viewpointCount; ++i) {
    const BcfViewpointMarkup fallback =
        i < topic.viewpoints.size() ? topic.viewpoints[i].markup
                                    : BcfViewpointMarkup{};
    markup.viewpoints[i] =
        mergedViewpointMarkup(markup.viewpoints[i], fallback, i);
  }
  ensurePinGuids(markup);
  return markup;
}

struct BcfArchiveEntry {
  std::string name;
  std::string text;
};

bool containsEntryName(const std::vector<BcfArchiveEntry>& entries,
                       std::string_view name) {
  return std::any_of(entries.begin(), entries.end(),
                     [name](const BcfArchiveEntry& entry) {
                       return entry.name == name;
                     });
}

std::optional<std::vector<BcfArchiveEntry>> topicArchiveEntries(
    const BcfTopicFolder& topic) {
  const BcfMarkup markup = normalizedTopicMarkup(topic);
  const std::string topicFolder = archiveTopicFolderName(markup.topic.guid);
  for (const BcfViewpointMarkup& viewpoint : markup.viewpoints) {
    if (!archiveRelativePath(viewpoint.viewpointFile)) {
      return std::nullopt;
    }
  }

  std::vector<BcfArchiveEntry> entries;
  entries.push_back({archiveJoin(topicFolder, "markup.bcf"),
                     ExportMarkup(markup)});

  for (size_t i = 0; i < topic.viewpoints.size(); ++i) {
    if (!topic.viewpoints[i].snapshot) {
      continue;
    }
    const auto relativePath =
        archiveRelativePath(markup.viewpoints[i].viewpointFile);
    if (!relativePath) {
      return std::nullopt;
    }
    const std::string entryName = archiveJoin(topicFolder, *relativePath);
    if (containsEntryName(entries, entryName)) {
      return std::nullopt;
    }
    entries.push_back(
        {entryName, ExportVisualizationInfo(*topic.viewpoints[i].snapshot,
                                            markup.viewpoints[i])});
  }

  return entries;
}

std::optional<std::string> extractArchiveText(mz_zip_archive& zip,
                                              mz_uint fileIndex) {
  size_t size = 0u;
  void* data = mz_zip_reader_extract_to_heap(&zip, fileIndex, &size, 0);
  if (data == nullptr) {
    return std::nullopt;
  }
  std::string text(static_cast<const char*>(data), size);
  mz_free(data);
  return text;
}

std::string normalizeArchiveEntryName(std::string_view value) {
  std::string normalized(value);
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return normalized;
}

std::optional<mz_uint> findArchiveEntry(mz_zip_archive& zip,
                                        std::string_view name) {
  const mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < fileCount; ++i) {
    mz_zip_archive_file_stat stat{};
    if (mz_zip_reader_is_file_a_directory(&zip, i) ||
        !mz_zip_reader_file_stat(&zip, i, &stat)) {
      continue;
    }
    if (normalizeArchiveEntryName(stat.m_filename) == name) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<mz_uint> findArchiveMarkup(mz_zip_archive& zip,
                                         std::string& baseFolder) {
  const mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < fileCount; ++i) {
    mz_zip_archive_file_stat stat{};
    if (mz_zip_reader_is_file_a_directory(&zip, i) ||
        !mz_zip_reader_file_stat(&zip, i, &stat)) {
      continue;
    }
    const std::string name = normalizeArchiveEntryName(stat.m_filename);
    if (name == "markup.bcf" || name.ends_with("/markup.bcf")) {
      const size_t slash = name.rfind('/');
      baseFolder = slash == std::string::npos ? std::string{}
                                               : name.substr(0, slash);
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string StablePinGuid(std::string_view ifcGuid, std::string_view sourceId,
                          std::string_view topicGuid) {
  if (ifcGuid.empty() && sourceId.empty() && topicGuid.empty()) {
    return {};
  }

  constexpr uint64_t kFnvOffset = 14695981039346656037ull;
  uint64_t hash = appendFnv1a(kFnvOffset, "topic:");
  hash = appendFnv1a(hash, topicGuid);
  hash = appendFnv1a(hash, "\nifc:");
  hash = appendFnv1a(hash, ifcGuid);
  hash = appendFnv1a(hash, "\nsource:");
  hash = appendFnv1a(hash, sourceId);
  return "container-pin-" + hex64(hash);
}

std::string ExportVisualizationInfo(const ViewpointSnapshotState& snapshot) {
  return ExportVisualizationInfo(snapshot, BcfViewpointMarkup{});
}

std::string ExportVisualizationInfo(const ViewpointSnapshotState& snapshot,
                                    const BcfViewpointMarkup& markup) {
  std::ostringstream stream;
  const glm::vec3 direction = cameraDirection(snapshot.camera);
  const glm::vec3 up = cameraUp(direction);

  stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  stream << "<VisualizationInfo>\n";
  stream << "  <PerspectiveCamera>\n";
  writeVector(stream, "CameraViewPoint", snapshot.camera.position, 4);
  writeVector(stream, "CameraDirection", direction, 4);
  writeVector(stream, "CameraUpVector", up, 4);
  stream << "    <FieldOfView>60</FieldOfView>\n";
  stream << "  </PerspectiveCamera>\n";
  stream << "  <Components>\n";
  stream << "    <Selection>\n";
  const auto writeComponent = [&](std::string_view guid, std::string_view sourceId) {
    if (guid.empty() && sourceId.empty()) return;
    stream << "      <Component";
    writeAttribute(stream, "IfcGuid", guid);
    writeAttribute(stream, "AuthoringToolId", sourceId);
    stream << "/>\n";
  };
  writeComponent(snapshot.selectedBimGuid, snapshot.selectedBimSourceId);
  for (const auto& component : snapshot.additionalBimSelections)
    writeComponent(component.guid, component.sourceId);
  stream << "    </Selection>\n";
  stream << "    <Visibility DefaultVisibility=\"true\"/>\n";
  stream << "  </Components>\n";
  if (hasContainerMarkup(markup)) {
    writeContainerMarkup(stream, markup, 2);
  }
  stream << "  <ContainerSnapshot>\n";
  stream << "    <Label Value=\"" << escapeXml(snapshot.label) << "\"/>\n";
  stream << "    <Camera";
  writeAttribute(stream, "PositionX", snapshot.camera.position.x);
  writeAttribute(stream, "PositionY", snapshot.camera.position.y);
  writeAttribute(stream, "PositionZ", snapshot.camera.position.z);
  writeAttribute(stream, "RotationX", snapshot.camera.rotationDegrees.x);
  writeAttribute(stream, "RotationY", snapshot.camera.rotationDegrees.y);
  writeAttribute(stream, "RotationZ", snapshot.camera.rotationDegrees.z);
  writeAttribute(stream, "ScaleX", snapshot.camera.scale.x);
  writeAttribute(stream, "ScaleY", snapshot.camera.scale.y);
  writeAttribute(stream, "ScaleZ", snapshot.camera.scale.z);
  stream << "/>\n";
  stream << "    <SelectionState";
  writeAttribute(stream, "MeshNode", snapshot.selectedMeshNode);
  writeAttribute(stream, "BimObjectIndex", snapshot.selectedBimObjectIndex);
  writeAttribute(stream, "BimGuid", snapshot.selectedBimGuid);
  writeAttribute(stream, "BimType", snapshot.selectedBimType);
  writeAttribute(stream, "BimSourceId", snapshot.selectedBimSourceId);
  writeAttribute(stream, "BimModelPath", snapshot.bimModelPath);
  stream << "/>\n";
  writeFilter(stream, snapshot.bimFilter);
  writePhaseTimeline(stream, snapshot.phaseTimeline);
  stream << "  </ContainerSnapshot>\n";
  stream << "</VisualizationInfo>\n";
  return stream.str();
}

std::optional<ViewpointSnapshotState> ImportVisualizationInfo(
    std::string_view xml) {
  if (xml.find("<VisualizationInfo") == std::string_view::npos) {
    return std::nullopt;
  }

  ViewpointSnapshotState snapshot{};
  if (const auto tag = openTag(xml, "Label")) {
    snapshot.label = stringAttribute(*tag, "Value");
  }
  if (const auto tag = openTag(xml, "Camera")) {
    snapshot.camera.position.x = floatAttribute(*tag, "PositionX", 0.0f);
    snapshot.camera.position.y = floatAttribute(*tag, "PositionY", 0.0f);
    snapshot.camera.position.z = floatAttribute(*tag, "PositionZ", 0.0f);
    snapshot.camera.rotationDegrees.x =
        floatAttribute(*tag, "RotationX", 0.0f);
    snapshot.camera.rotationDegrees.y =
        floatAttribute(*tag, "RotationY", 0.0f);
    snapshot.camera.rotationDegrees.z =
        floatAttribute(*tag, "RotationZ", 0.0f);
    snapshot.camera.scale.x = floatAttribute(*tag, "ScaleX", 1.0f);
    snapshot.camera.scale.y = floatAttribute(*tag, "ScaleY", 1.0f);
    snapshot.camera.scale.z = floatAttribute(*tag, "ScaleZ", 1.0f);
  } else if (!readStandardPerspectiveCamera(xml, snapshot.camera)) {
    return std::nullopt;
  }
  if (const auto tag = openTag(xml, "SelectionState")) {
    snapshot.selectedMeshNode = uintAttribute(*tag, "MeshNode", kInvalidIndex);
    snapshot.selectedBimObjectIndex =
        uintAttribute(*tag, "BimObjectIndex", kInvalidIndex);
    snapshot.selectedBimGuid = stringAttribute(*tag, "BimGuid");
    snapshot.selectedBimType = stringAttribute(*tag, "BimType");
    snapshot.selectedBimSourceId = stringAttribute(*tag, "BimSourceId");
    snapshot.bimModelPath = stringAttribute(*tag, "BimModelPath");
  }
  readStandardComponentSelection(xml, snapshot);
  if (const auto tag = openTag(xml, "BimFilter")) {
    readFilter(*tag, snapshot.bimFilter);
  }
  if (const auto tag = openTag(xml, "PhaseTimeline")) {
    readPhaseTimeline(*tag, snapshot.phaseTimeline);
  }
  return snapshot;
}

std::optional<BcfViewpointMarkup> ImportVisualizationInfoMarkup(
    std::string_view xml) {
  return ImportVisualizationInfoMarkup(xml, std::string_view{});
}

std::optional<BcfViewpointMarkup> ImportVisualizationInfoMarkup(
    std::string_view xml, std::string_view topicGuid) {
  if (xml.find("<VisualizationInfo") == std::string_view::npos) {
    return std::nullopt;
  }

  const auto markupTag = openTag(xml, "ContainerMarkup");
  if (!markupTag) {
    return std::nullopt;
  }

  const auto markupBody = tagBody(xml, "ContainerMarkup");
  BcfViewpointMarkup markup = readViewpointMarkup(
      *markupTag, markupBody ? *markupBody : std::string_view{});
  for (BcfPin& pin : markup.pins) {
    if (pin.guid.empty()) {
      pin.guid = StablePinGuid(pin.ifcGuid, pin.sourceId, topicGuid);
    }
  }
  return markup;
}

bool SaveVisualizationInfo(const ViewpointSnapshotState& snapshot,
                           const std::filesystem::path& path) {
  return SaveVisualizationInfo(snapshot, BcfViewpointMarkup{}, path);
}

bool SaveVisualizationInfo(const ViewpointSnapshotState& snapshot,
                           const BcfViewpointMarkup& markup,
                           const std::filesystem::path& path) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file << ExportVisualizationInfo(snapshot, markup);
  file.close();
  return static_cast<bool>(file);
}

std::optional<ViewpointSnapshotState> LoadVisualizationInfo(
    const std::filesystem::path& path) {
  const auto xml = readTextFile(path);
  return xml ? ImportVisualizationInfo(*xml) : std::nullopt;
}

std::optional<BcfVisualizationInfo> LoadVisualizationInfoWithMarkup(
    const std::filesystem::path& path) {
  const auto xml = readTextFile(path);
  if (!xml) {
    return std::nullopt;
  }
  auto snapshot = ImportVisualizationInfo(*xml);
  if (!snapshot) {
    return std::nullopt;
  }
  return BcfVisualizationInfo{
      .snapshot = std::move(*snapshot),
      .markup =
          ImportVisualizationInfoMarkup(*xml).value_or(BcfViewpointMarkup{}),
  };
}

std::string ExportMarkup(const BcfMarkup& markup) {
  std::ostringstream stream;
  stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  stream << "<Markup>\n";
  stream << "  <Topic";
  writeOptionalAttribute(stream, "Guid", markup.topic.guid);
  writeOptionalAttribute(stream, "TopicStatus", markup.topic.status);
  stream << ">\n";
  writeTextElement(stream, "Title", markup.topic.title, 4);
  if (!markup.topic.priority.empty()) {
    writeTextElement(stream, "Priority", markup.topic.priority, 4);
  }
  writeTextElements(stream, "Labels", "Label", markup.topic.labels, 4);
  writeTextElements(stream, "ContainerTags", "Tag", markup.topic.tags, 4);
  stream << "  </Topic>\n";

  for (const BcfComment& comment : markup.comments) {
    stream << "  <Comment";
    writeOptionalAttribute(stream, "Guid", comment.guid);
    stream << ">\n";
    if (!comment.date.empty()) {
      writeTextElement(stream, "Date", comment.date, 4);
    }
    if (!comment.author.empty()) {
      writeTextElement(stream, "Author", comment.author, 4);
    }
    writeTextElement(stream, "Text", comment.text, 4);
    if (!comment.viewpointGuid.empty()) {
      stream << "    <Viewpoint";
      writeAttribute(stream, "Guid", comment.viewpointGuid);
      stream << "/>\n";
    }
    stream << "  </Comment>\n";
  }

  if (!markup.viewpoints.empty()) {
    stream << "  <Viewpoints>\n";
    for (const BcfViewpointMarkup& viewpoint : markup.viewpoints) {
      stream << "    <ViewPoint";
      writeOptionalAttribute(stream, "Guid", viewpoint.guid);
      writeOptionalAttribute(stream, "Viewpoint", viewpoint.viewpointFile);
      writeOptionalAttribute(stream, "Snapshot", viewpoint.snapshotFile);
      if (viewpoint.pins.empty() && viewpoint.lines.empty()) {
        stream << "/>\n";
      } else {
        stream << ">\n";
        writeContainerMarkup(stream, viewpoint, 6, markup.topic.guid);
        stream << "    </ViewPoint>\n";
      }
    }
    stream << "  </Viewpoints>\n";
  }

  stream << "</Markup>\n";
  return stream.str();
}

std::optional<BcfMarkup> ImportMarkup(std::string_view xml) {
  if (xml.find("<Markup") == std::string_view::npos) {
    return std::nullopt;
  }

  const auto markupBody = tagBody(xml, "Markup");
  if (!markupBody) {
    return std::nullopt;
  }

  BcfMarkup markup{};
  const std::vector<XmlElementView> topicElements =
      elements(*markupBody, "Topic");
  if (!topicElements.empty()) {
    const XmlElementView topicElement = topicElements.front();
    markup.topic.guid = stringAttribute(topicElement.tag, "Guid");
    markup.topic.status =
        firstStringAttribute(topicElement.tag, {"TopicStatus", "Status"});
    markup.topic.title = stringElement(topicElement.body, "Title");
    if (markup.topic.title.empty()) {
      markup.topic.title = stringAttribute(topicElement.tag, "Title");
    }
    markup.topic.priority = stringElement(topicElement.body, "Priority");
    if (markup.topic.priority.empty()) {
      markup.topic.priority = stringAttribute(topicElement.tag, "Priority");
    }
    markup.topic.labels =
        groupedStringElements(topicElement.body, "Labels", "Label");
    markup.topic.tags =
        groupedStringElements(topicElement.body, "ContainerTags", "Tag");
  }

  for (const XmlElementView commentElement : elements(*markupBody, "Comment")) {
    BcfComment comment{};
    comment.guid = stringAttribute(commentElement.tag, "Guid");
    comment.date = stringElement(commentElement.body, "Date");
    comment.author = stringElement(commentElement.body, "Author");
    comment.text = stringElement(commentElement.body, "Text");
    if (comment.text.empty()) {
      comment.text = stringElement(commentElement.body, "Comment");
    }
    comment.viewpointGuid = firstStringAttribute(
        commentElement.tag, {"ViewpointGuid", "Viewpoint"});
    if (comment.viewpointGuid.empty()) {
      if (const auto viewpointTag = openTag(commentElement.body, "Viewpoint")) {
        comment.viewpointGuid = stringAttribute(*viewpointTag, "Guid");
      }
    }
    markup.comments.push_back(std::move(comment));
  }

  if (const auto viewpointsBody = tagBody(*markupBody, "Viewpoints")) {
    for (const XmlElementView viewpointElement :
         elements(*viewpointsBody, "ViewPoint")) {
      markup.viewpoints.push_back(
          readViewpointMarkup(viewpointElement.tag, viewpointElement.body));
    }
  }

  ensurePinGuids(markup);
  return markup;
}

bool SaveMarkup(const BcfMarkup& markup, const std::filesystem::path& path) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file << ExportMarkup(markup);
  return static_cast<bool>(file);
}

std::optional<BcfMarkup> LoadMarkup(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  return ImportMarkup(text);
}

bool SaveTopicFolder(const BcfTopicFolder& topic,
                     const std::filesystem::path& folder) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (ec) {
    return false;
  }

  BcfMarkup markup = topic.markup;
  const size_t viewpointCount =
      std::max(markup.viewpoints.size(), topic.viewpoints.size());
  markup.viewpoints.resize(viewpointCount);
  for (size_t i = 0; i < viewpointCount; ++i) {
    const BcfViewpointMarkup fallback =
        i < topic.viewpoints.size() ? topic.viewpoints[i].markup
                                    : BcfViewpointMarkup{};
    markup.viewpoints[i] =
        mergedViewpointMarkup(markup.viewpoints[i], fallback, i);
    if (!safeRelativePath(markup.viewpoints[i].viewpointFile)) {
      return false;
    }
  }
  ensurePinGuids(markup);

  if (!writeTextFile(folder / "markup.bcf", ExportMarkup(markup))) {
    return false;
  }

  for (size_t i = 0; i < topic.viewpoints.size(); ++i) {
    if (!topic.viewpoints[i].snapshot) {
      continue;
    }
    const BcfViewpointMarkup& viewpoint = markup.viewpoints[i];
    const std::filesystem::path viewpointPath = folder / viewpoint.viewpointFile;
    if (!writeTextFile(viewpointPath,
                       ExportVisualizationInfo(*topic.viewpoints[i].snapshot,
                                               viewpoint))) {
      return false;
    }
  }

  return true;
}

std::optional<BcfTopicFolder> LoadTopicFolder(
    const std::filesystem::path& folder) {
  auto markup = LoadMarkup(folder / "markup.bcf");
  if (!markup) {
    return std::nullopt;
  }

  BcfTopicFolder topic{};
  topic.markup = std::move(*markup);
  topic.viewpoints.reserve(topic.markup.viewpoints.size());

  for (BcfViewpointMarkup& viewpoint : topic.markup.viewpoints) {
    BcfTopicViewpoint entry{};
    entry.markup = viewpoint;
    if (!entry.markup.viewpointFile.empty() &&
        safeRelativePath(entry.markup.viewpointFile)) {
      const std::filesystem::path viewpointPath =
          folder / entry.markup.viewpointFile;
      if (const auto text = readTextFile(viewpointPath)) {
        entry.snapshot = ImportVisualizationInfo(*text);
        if (auto viewpointMarkup =
                ImportVisualizationInfoMarkup(*text, topic.markup.topic.guid)) {
          entry.markup = mergedViewpointMarkup(entry.markup, *viewpointMarkup,
                                               topic.viewpoints.size());
        }
      }
    }
    for (BcfPin& pin : entry.markup.pins) {
      if (pin.guid.empty()) {
        pin.guid =
            StablePinGuid(pin.ifcGuid, pin.sourceId, topic.markup.topic.guid);
      }
    }
    viewpoint = entry.markup;
    topic.viewpoints.push_back(std::move(entry));
  }

  return topic;
}

bool SaveBcfArchive(const BcfTopicFolder& topic,
                    const std::filesystem::path& path) {
  const auto entries = topicArchiveEntries(topic);
  if (!entries) {
    return false;
  }

  if (path.has_parent_path()) {
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
      return false;
    }
  }

  std::error_code removeError;
  std::filesystem::remove(path, removeError);

  mz_zip_archive zip{};
  const std::string archivePath = path.string();
  if (!mz_zip_writer_init_file(&zip, archivePath.c_str(), 0)) {
    return false;
  }

  bool ok = true;
  for (const BcfArchiveEntry& entry : *entries) {
    if (!mz_zip_writer_add_mem(&zip, entry.name.c_str(), entry.text.data(),
                               entry.text.size(), MZ_BEST_COMPRESSION)) {
      ok = false;
      break;
    }
  }
  if (ok && !mz_zip_writer_finalize_archive(&zip)) {
    ok = false;
  }
  if (!mz_zip_writer_end(&zip)) {
    ok = false;
  }
  return ok;
}

std::optional<BcfTopicFolder> LoadBcfArchive(
    const std::filesystem::path& path) {
  mz_zip_archive zip{};
  const std::string archivePath = path.string();
  if (!mz_zip_reader_init_file(&zip, archivePath.c_str(), 0)) {
    return std::nullopt;
  }

  std::string baseFolder;
  const auto markupIndex = findArchiveMarkup(zip, baseFolder);
  if (!markupIndex) {
    mz_zip_reader_end(&zip);
    return std::nullopt;
  }

  const auto markupText = extractArchiveText(zip, *markupIndex);
  auto markup = markupText ? ImportMarkup(*markupText)
                           : std::optional<BcfMarkup>{};
  if (!markup) {
    mz_zip_reader_end(&zip);
    return std::nullopt;
  }

  BcfTopicFolder topic{};
  topic.markup = std::move(*markup);
  topic.viewpoints.reserve(topic.markup.viewpoints.size());

  for (BcfViewpointMarkup& viewpoint : topic.markup.viewpoints) {
    BcfTopicViewpoint entry{};
    entry.markup = viewpoint;

    if (!entry.markup.viewpointFile.empty()) {
      const auto relativePath =
          archiveRelativePath(entry.markup.viewpointFile);
      if (relativePath) {
        const std::string entryName = archiveJoin(baseFolder, *relativePath);
        if (const auto viewpointIndex = findArchiveEntry(zip, entryName)) {
          if (const auto text = extractArchiveText(zip, *viewpointIndex)) {
            entry.snapshot = ImportVisualizationInfo(*text);
            if (auto viewpointMarkup = ImportVisualizationInfoMarkup(
                    *text, topic.markup.topic.guid)) {
              entry.markup = mergedViewpointMarkup(
                  entry.markup, *viewpointMarkup, topic.viewpoints.size());
            }
          }
        }
      }
    }

    for (BcfPin& pin : entry.markup.pins) {
      if (pin.guid.empty()) {
        pin.guid =
            StablePinGuid(pin.ifcGuid, pin.sourceId, topic.markup.topic.guid);
      }
    }
    viewpoint = entry.markup;
    topic.viewpoints.push_back(std::move(entry));
  }

  if (!mz_zip_reader_end(&zip)) {
    return std::nullopt;
  }
  return topic;
}

}  // namespace container::ui::bcf

namespace container::renderer {
namespace {

[[nodiscard]] bool finiteIssuePinPosition(glm::vec3 position) {
  return std::isfinite(position.x) && std::isfinite(position.y) &&
         std::isfinite(position.z);
}

void appendBcfIssuePins(const container::ui::bcf::BcfViewpointMarkup& markup,
                       std::vector<BimCoordinationOverlayIssuePin>& issuePins) {
  issuePins.reserve(issuePins.size() + markup.pins.size());
  for (const container::ui::bcf::BcfPin& pin : markup.pins) {
    if (!pin.hasLocation || !finiteIssuePinPosition(pin.location)) {
      continue;
    }
    issuePins.push_back({.position = pin.location,
                         .label = pin.label,
                         .ifcGuid = pin.ifcGuid,
                         .sourceId = pin.sourceId,
                         .primaryObjectIndex =
                             kInvalidBimCoordinationOverlayObjectIndex,
                         .secondaryObjectIndex =
                             kInvalidBimCoordinationOverlayObjectIndex});
  }
}

} // namespace

std::vector<BimCoordinationOverlayIssuePin>
exportBcfPinsAsIssueOverlayMarkers(
    const container::ui::bcf::BcfMarkup &markup) {
  std::vector<BimCoordinationOverlayIssuePin> issuePins;
  for (const container::ui::bcf::BcfViewpointMarkup &viewpoint :
       markup.viewpoints) {
    appendBcfIssuePins(viewpoint, issuePins);
  }
  return issuePins;
}

std::vector<BimCoordinationOverlayIssuePin> exportBcfPinsAsIssueOverlayMarkers(
    const container::ui::bcf::BcfViewpointMarkup& markup) {
  std::vector<BimCoordinationOverlayIssuePin> issuePins;
  appendBcfIssuePins(markup, issuePins);
  return issuePins;
}

} // namespace container::renderer
