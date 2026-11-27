#include "Container/robotics/UrdfModel.h"

#include <tinyxml2.h>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace container::robotics {
namespace {

using tinyxml2::XMLElement;
using tinyxml2::XMLNode;

constexpr size_t maxUrdfBytes = 8u * 1024u * 1024u;
constexpr std::string_view whitespace = " \t\r\n";

[[noreturn]] void fail(int line, std::string_view message) {
  auto error = line > 0 ? "URDF line " + std::to_string(line) + ": " : std::string("URDF: ");
  throw std::invalid_argument(error + std::string(message));
}

[[noreturn]] void fail(const XMLNode& node, std::string_view message) {
  fail(node.GetLineNum(), message);
}

std::string_view trim(std::string_view value) {
  const auto begin = value.find_first_not_of(whitespace);
  return begin == std::string_view::npos ? std::string_view{}
      : value.substr(begin, value.find_last_not_of(whitespace) - begin + 1);
}

int lineAt(std::string_view xml, size_t offset) {
  return 1 + static_cast<int>(std::ranges::count(xml.substr(0, offset), '\n'));
}

bool allows(std::initializer_list<std::string_view> names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

void checkChildren(const XMLElement& element, std::initializer_list<std::string_view> names) {
  for (auto* node = element.FirstChild(); node; node = node->NextSibling()) {
    if (const auto* child = node->ToElement(); child && !allows(names, child->Name()))
      fail(*child, "unsupported <" + std::string(child->Name()) + "> in <" + element.Name() + ">");
    if (const auto* text = node->ToText(); text && !trim(text->Value()).empty())
      fail(*text, "unexpected text in <" + std::string(element.Name()) + ">");
  }
}

void checkAttributes(const XMLElement& element, std::initializer_list<std::string_view> names) {
  for (const auto* attribute = element.FirstAttribute(); attribute; attribute = attribute->Next())
    if (!allows(names, attribute->Name()))
      fail(attribute->GetLineNum(), "unsupported attribute '" + std::string(attribute->Name()) +
          "' on <" + element.Name() + ">");
}

const XMLElement* singleChild(const XMLElement& parent, const char* name, bool required = false) {
  const auto* child = parent.FirstChildElement(name);
  if (!child) {
    if (required) fail(parent, "missing <" + std::string(name) + "> in <" + parent.Name() + ">");
  } else if (const auto* duplicate = child->NextSiblingElement(name)) {
    fail(*duplicate, "duplicate <" + std::string(name) + "> in <" + parent.Name() + ">");
  }
  return child;
}

std::string_view requiredAttribute(const XMLElement& element, const char* name) {
  const auto* value = element.Attribute(name);
  if (!value || trim(value).empty())
    fail(element, "missing or empty '" + std::string(name) + "' on <" + element.Name() + ">");
  return value;
}

std::string optionalName(const XMLElement& element) {
  const auto* value = element.Attribute("name");
  return value ? std::string(value) : std::string{};
}

double number(std::string_view token, const XMLElement& element, const char* name) {
  token = trim(token);
  if (token.starts_with('+')) {
    token.remove_prefix(1);
    if (token.starts_with('-')) token = {};
  }
  double result{};
  if (!token.empty()) {
    const auto end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data(), end, result);
    if (parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(result)) return result;
  }
  fail(element, "'" + std::string(name) + "' on <" + element.Name() + "> must contain finite numbers");
}

double scalar(const XMLElement& element, const char* name) {
  return number(requiredAttribute(element, name), element, name);
}

double scalarOr(const XMLElement& element, const char* name, double fallback = 0.0) {
  const auto* value = element.Attribute(name);
  return value ? number(value, element, name) : fallback;
}

template<class Vector>
Vector vectorAttribute(const XMLElement& element, const char* name) {
  auto input = requiredAttribute(element, name);
  Vector result(0.0);
  for (glm::length_t i = 0; i < Vector::length(); ++i) {
    input = trim(input);
    if (input.empty())
      fail(element, "'" + std::string(name) + "' must contain " + std::to_string(Vector::length()) + " numbers");
    const auto separator = input.find_first_of(whitespace);
    result[i] = number(input.substr(0, separator), element, name);
    input.remove_prefix(separator == std::string_view::npos ? input.size() : separator);
  }
  if (!trim(input).empty())
    fail(element, "'" + std::string(name) + "' must contain " + std::to_string(Vector::length()) + " numbers");
  return result;
}

glm::dvec3 vectorOr(const XMLElement& element, const char* name, glm::dvec3 fallback) {
  return element.Attribute(name) ? vectorAttribute<glm::dvec3>(element, name) : fallback;
}

double positive(const XMLElement& element, const char* name) {
  const auto value = scalar(element, name);
  if (value <= 0.0) fail(element, "'" + std::string(name) + "' must be positive");
  return value;
}

bool finite(glm::dvec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

UrdfPose readOrigin(const XMLElement& owner) {
  const auto* origin = singleChild(owner, "origin");
  if (!origin) return {};
  checkAttributes(*origin, {"xyz", "rpy"});
  checkChildren(*origin, {});
  return urdfPoseFromRpy(vectorOr(*origin, "xyz", glm::dvec3(0.0)), vectorOr(*origin, "rpy", glm::dvec3(0.0)));
}

UrdfGeometry readGeometry(const XMLElement& owner) {
  const auto& geometry = *singleChild(owner, "geometry", true);
  checkChildren(geometry, {"box", "cylinder", "sphere", "mesh"});
  const auto* shape = geometry.FirstChildElement();
  if (!shape || shape->NextSiblingElement()) fail(geometry, "<geometry> must specify exactly one shape");
  checkChildren(*shape, {});
  const auto kind = std::string_view(shape->Name());
  if (kind == "box") {
    checkAttributes(*shape, {"size"});
    const auto size = vectorAttribute<glm::dvec3>(*shape, "size");
    if (size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0) fail(*shape, "box dimensions must be positive");
    return UrdfBox{size};
  }
  if (kind == "cylinder") {
    checkAttributes(*shape, {"radius", "length"});
    return UrdfCylinder{positive(*shape, "radius"), positive(*shape, "length")};
  }
  if (kind == "sphere") {
    checkAttributes(*shape, {"radius"});
    return UrdfSphere{positive(*shape, "radius")};
  }
  checkAttributes(*shape, {"filename", "scale"});
  const auto scale = vectorOr(*shape, "scale", glm::dvec3(1.0));
  if (scale.x == 0.0 || scale.y == 0.0 || scale.z == 0.0) fail(*shape, "mesh scale components must be nonzero");
  return UrdfMesh{std::string(requiredAttribute(*shape, "filename")), scale};
}

UrdfMaterial readMaterial(const XMLElement& element) {
  checkAttributes(element, {"name"});
  checkChildren(element, {"color", "texture"});
  UrdfMaterial material;
  material.name = requiredAttribute(element, "name");
  if (const auto* color = singleChild(element, "color")) {
    checkAttributes(*color, {"rgba"});
    checkChildren(*color, {});
    const auto rgba = vectorAttribute<glm::dvec4>(*color, "rgba");
    for (glm::length_t i = 0; i < rgba.length(); ++i)
      if (rgba[i] < 0.0 || rgba[i] > 1.0) fail(*color, "material RGBA components must be in [0, 1]");
    material.rgba = rgba;
  }
  if (const auto* texture = singleChild(element, "texture")) {
    checkAttributes(*texture, {"filename"});
    checkChildren(*texture, {});
    material.textureUri = requiredAttribute(*texture, "filename");
  }
  return material;
}

using Materials = std::unordered_map<std::string, UrdfMaterial>;

void registerMaterial(Materials& definitions, const XMLElement& element, bool topLevel) {
  auto material = readMaterial(element);
  if (!material.rgba && material.textureUri.empty()) {
    if (topLevel) fail(element, "a global material definition requires a color or texture");
    return;
  }
  if (const auto found = definitions.find(material.name); found != definitions.end()) {
    if (topLevel || found->second.rgba != material.rgba || found->second.textureUri != material.textureUri)
      fail(element, "duplicate or conflicting material definition '" + material.name + "'");
  } else {
    auto name = material.name;
    definitions.emplace(std::move(name), std::move(material));
  }
}

Materials collectMaterials(const XMLElement& robot) {
  Materials materials;
  for (const auto* item = robot.FirstChildElement("material"); item; item = item->NextSiblingElement("material"))
    registerMaterial(materials, *item, true);
  for (const auto* link = robot.FirstChildElement("link"); link; link = link->NextSiblingElement("link"))
    for (const auto* visual = link->FirstChildElement("visual"); visual; visual = visual->NextSiblingElement("visual"))
      if (const auto* item = singleChild(*visual, "material")) registerMaterial(materials, *item, false);
  return materials;
}

UrdfVisual readVisual(const XMLElement& element, const Materials& materials) {
  checkChildren(element, {"origin", "geometry", "material"});
  UrdfVisual visual{.name = optionalName(element), .linkFromVisual = readOrigin(element), .geometry = readGeometry(element)};
  if (const auto* item = singleChild(element, "material")) {
    const std::string name(requiredAttribute(*item, "name"));
    const auto found = materials.find(name);
    if (found == materials.end()) fail(*item, "unknown material '" + name + "'");
    visual.material = found->second;
  }
  return visual;
}

void validateInertia(const XMLElement& element, const glm::dmat3& inertia) {
  double scale = 0.0;
  for (glm::length_t col = 0; col < 3; ++col)
    for (glm::length_t row = 0; row < 3; ++row) scale = std::max(scale, std::abs(inertia[col][row]));
  if (scale == 0.0) fail(element, "inertia tensor must be positive definite");
  const auto tensor = inertia / scale;
  if (!(tensor[0][0] > 0.0 && tensor[0][0] * tensor[1][1] > tensor[0][1] * tensor[0][1] &&
        glm::determinant(tensor) > 0.0))
    fail(element, "inertia tensor must be positive definite");

  // The implied second moments are positive semidefinite if and only if the
  // principal moments of inertia satisfy the triangle inequalities.
  const auto halfTrace = (tensor[0][0] + tensor[1][1] + tensor[2][2]) * 0.5;
  const auto secondMoments = glm::dmat3(halfTrace) - tensor;
  constexpr double tolerance = 1e-12;
  for (glm::length_t i = 0; i < 3; ++i) {
    const auto j = (i + 1) % 3;
    if (secondMoments[i][i] < -tolerance ||
        secondMoments[i][i] * secondMoments[j][j] - secondMoments[i][j] * secondMoments[i][j] < -tolerance)
      fail(element, "inertia principal moments violate the triangle inequalities");
  }
  if (glm::determinant(secondMoments) < -tolerance)
    fail(element, "inertia principal moments violate the triangle inequalities");
}

UrdfInertial readInertial(const XMLElement& element) {
  checkChildren(element, {"origin", "mass", "inertia"});
  const auto& mass = *singleChild(element, "mass", true);
  checkAttributes(mass, {"value"});
  checkChildren(mass, {});
  const auto& inertia = *singleChild(element, "inertia", true);
  checkAttributes(inertia, {"ixx", "ixy", "ixz", "iyy", "iyz", "izz"});
  checkChildren(inertia, {});
  const auto xx = scalar(inertia, "ixx"), xy = scalar(inertia, "ixy"), xz = scalar(inertia, "ixz");
  const auto yy = scalar(inertia, "iyy"), yz = scalar(inertia, "iyz"), zz = scalar(inertia, "izz");
  const glm::dmat3 tensor(glm::dvec3(xx, xy, xz), glm::dvec3(xy, yy, yz), glm::dvec3(xz, yz, zz));
  validateInertia(inertia, tensor);
  return {.linkFromInertial = readOrigin(element), .mass = positive(mass, "value"), .inertia = tensor};
}

UrdfLink readLink(const XMLElement& element, const Materials& materials) {
  checkChildren(element, {"inertial", "visual", "collision"});
  UrdfLink link;
  link.name = requiredAttribute(element, "name");
  link.sourceLine = element.GetLineNum();
  if (const auto* inertial = singleChild(element, "inertial")) link.inertial = readInertial(*inertial);
  std::unordered_set<std::string> visuals, collisions;
  for (const auto* item = element.FirstChildElement("visual"); item; item = item->NextSiblingElement("visual")) {
    auto visual = readVisual(*item, materials);
    if (!visual.name.empty() && !visuals.insert(visual.name).second)
      fail(*item, "duplicate visual name '" + visual.name + "'");
    link.visuals.push_back(std::move(visual));
  }
  for (const auto* item = element.FirstChildElement("collision"); item; item = item->NextSiblingElement("collision")) {
    checkChildren(*item, {"origin", "geometry"});
    auto name = optionalName(*item);
    if (!name.empty() && !collisions.insert(name).second) fail(*item, "duplicate collision name '" + name + "'");
    link.collisions.push_back({std::move(name), readOrigin(*item), readGeometry(*item)});
  }
  return link;
}

UrdfJointType readJointType(const XMLElement& joint) {
  const auto kind = requiredAttribute(joint, "type");
  if (kind == "fixed") return UrdfJointType::Fixed;
  if (kind == "revolute") return UrdfJointType::Revolute;
  if (kind == "continuous") return UrdfJointType::Continuous;
  if (kind == "prismatic") return UrdfJointType::Prismatic;
  fail(joint, "unsupported joint type '" + std::string(kind) + "'");
}

using LinkIndex = std::unordered_map<std::string, size_t>;

size_t readEndpoint(const XMLElement& joint, const char* name, const LinkIndex& links) {
  const auto& endpoint = *singleChild(joint, name, true);
  checkAttributes(endpoint, {"link"});
  checkChildren(endpoint, {});
  const std::string link(requiredAttribute(endpoint, "link"));
  const auto found = links.find(link);
  if (found == links.end()) fail(endpoint, "unknown " + std::string(name) + " link '" + link + "'");
  return found->second;
}

UrdfJointLimits readLimits(const XMLElement& limit, bool bounded) {
  checkAttributes(limit, {"lower", "upper", "effort", "velocity"});
  checkChildren(limit, {});
  // URDF 1.0 defaults omitted endpoints to zero. They have no meaning on
  // continuous and fixed joints, but any supplied values must be valid numbers.
  const auto lower = scalarOr(limit, "lower"), upper = scalarOr(limit, "upper");
  UrdfJointLimits values;
  values.velocity = scalar(limit, "velocity");
  values.effort = scalar(limit, "effort");
  if (values.velocity < 0.0 || values.effort < 0.0) fail(limit, "joint effort and velocity cannot be negative");
  if (bounded) {
    if (lower > upper) fail(limit, "joint lower position limit exceeds upper limit");
    values.lower = lower;
    values.upper = upper;
  }
  return values;
}

UrdfJoint readJoint(const XMLElement& element, const LinkIndex& links) {
  checkAttributes(element, {"name", "type"});
  checkChildren(element, {"origin", "parent", "child", "axis", "limit", "dynamics"});
  UrdfJoint joint;
  joint.name = requiredAttribute(element, "name");
  joint.type = readJointType(element);
  joint.sourceLine = element.GetLineNum();
  joint.parentLink = readEndpoint(element, "parent", links);
  joint.childLink = readEndpoint(element, "child", links);
  joint.parentFromJoint = readOrigin(element);
  if (const auto* axis = singleChild(element, "axis")) {
    checkAttributes(*axis, {"xyz"});
    checkChildren(*axis, {});
    const auto value = vectorAttribute<glm::dvec3>(*axis, "xyz");
    if (joint.type != UrdfJointType::Fixed) {
      const auto scale = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
      if (scale == 0.0) fail(*axis, "joint axis must be nonzero");
      joint.axis = glm::normalize(value / scale);
    }
  }
  const bool bounded = joint.type == UrdfJointType::Revolute || joint.type == UrdfJointType::Prismatic;
  if (const auto* limit = singleChild(element, "limit", bounded)) joint.limits = readLimits(*limit, bounded);
  if (const auto* dynamics = singleChild(element, "dynamics")) {
    checkAttributes(*dynamics, {"damping", "friction"});
    checkChildren(*dynamics, {});
    if (!dynamics->Attribute("damping") && !dynamics->Attribute("friction"))
      fail(*dynamics, "<dynamics> requires damping or friction");
    joint.damping = scalarOr(*dynamics, "damping");
    joint.friction = scalarOr(*dynamics, "friction");
    if (joint.damping < 0.0 || joint.friction < 0.0) fail(*dynamics, "joint damping and friction cannot be negative");
  }
  return joint;
}

void checkXmlDeclarations(const XMLNode& parent) {
  for (const auto* node = parent.FirstChild(); node; node = node->NextSibling()) {
    if (node->ToUnknown()) fail(*node, "XML doctype and unknown declarations are unsupported");
    checkXmlDeclarations(*node);
  }
}

void checkCharacterReferences(std::string_view xml) {
  // TinyXML2 decodes numeric references before exposing C strings, including
  // forbidden XML control values. Validate references before that conversion.
  for (size_t offset = 0; (offset = xml.find_first_of("<&", offset)) != std::string_view::npos;) {
    const auto rest = xml.substr(offset);
    if (rest.starts_with("<!DOCTYPE")) fail(lineAt(xml, offset), "XML doctype is unsupported");
    std::string_view close;
    if (rest.starts_with("<!--")) close = "-->";
    else if (rest.starts_with("<![CDATA[")) close = "]]>";
    else if (rest.starts_with("<?")) close = "?>";
    if (!close.empty()) {
      const auto end = xml.find(close, offset + 2);
      if (end == std::string_view::npos) return; // The parser diagnoses unterminated markup.
      offset = end + close.size();
      continue;
    }
    if (xml[offset] == '<') {
      ++offset;
      continue;
    }
    const auto end = xml.find_first_of(";<& \t\r\n\"'", offset + 1);
    if (end == std::string_view::npos || xml[end] != ';') fail(lineAt(xml, offset), "malformed XML entity reference");
    const auto reference = xml.substr(offset + 1, end - offset - 1);
    if (reference.starts_with('#')) {
      const bool hex = reference.starts_with("#x");
      const auto digits = reference.substr(hex ? 2 : 1);
      unsigned codepoint{};
      const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), codepoint, hex ? 16 : 10);
      const bool legal = codepoint == 0x9 || codepoint == 0xa || codepoint == 0xd ||
          (codepoint >= 0x20 && codepoint <= 0xd7ff) || (codepoint >= 0xe000 && codepoint <= 0xfffd) ||
          (codepoint >= 0x10000 && codepoint <= 0x10ffff);
      if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || !legal)
        fail(lineAt(xml, offset), "invalid XML character reference");
    } else if (!allows({"amp", "lt", "gt", "quot", "apos"}, reference)) {
      fail(lineAt(xml, offset), "unknown XML entity reference");
    }
    offset = end + 1;
  }
}

} // namespace

UrdfPose urdfPoseFromRpy(glm::dvec3 position, glm::dvec3 rpy) {
  if (!finite(position) || !finite(rpy)) fail(0, "pose position and RPY angles must be finite");
  const auto roll = glm::angleAxis(rpy.x, glm::dvec3(1, 0, 0));
  const auto pitch = glm::angleAxis(rpy.y, glm::dvec3(0, 1, 0));
  const auto yaw = glm::angleAxis(rpy.z, glm::dvec3(0, 0, 1));
  return {position, glm::normalize(yaw * pitch * roll)};
}

glm::dmat4 urdfPoseMatrix(const UrdfPose& pose) {
  const auto& rotation = pose.rotation;
  if (!finite(pose.position) || !finite({rotation.x, rotation.y, rotation.z}) || !std::isfinite(rotation.w))
    fail(0, "pose position and quaternion must be finite");
  const auto scale = std::max({std::abs(rotation.x), std::abs(rotation.y), std::abs(rotation.z), std::abs(rotation.w)});
  if (scale == 0.0) fail(0, "pose quaternion must be nonzero");
  auto matrix = glm::mat4_cast(glm::normalize(rotation / scale));
  matrix[3] = glm::dvec4(pose.position, 1.0);
  return matrix;
}

void validateUrdfTopology(UrdfModel& model) {
  if (trim(model.name).empty()) fail(0, "robot name is required");
  if (model.links.empty()) fail(0, "robot must contain at least one link");
  std::unordered_set<std::string_view> linkNames, jointNames;
  for (const auto& link : model.links)
    if (trim(link.name).empty() || !linkNames.insert(link.name).second)
      fail(link.sourceLine, "empty or duplicate link name '" + link.name + "'");

  std::vector<bool> hasParent(model.links.size());
  std::vector<std::vector<size_t>> outgoing(model.links.size());
  for (size_t index = 0; index < model.joints.size(); ++index) {
    const auto& joint = model.joints[index];
    if (trim(joint.name).empty() || !jointNames.insert(joint.name).second)
      fail(joint.sourceLine, "empty or duplicate joint name '" + joint.name + "'");
    if (joint.parentLink >= model.links.size() || joint.childLink >= model.links.size())
      fail(joint.sourceLine, "joint '" + joint.name + "' references a link outside the model");
    if (joint.parentLink == joint.childLink)
      fail(joint.sourceLine, "joint '" + joint.name + "' connects a link to itself");
    if (hasParent[joint.childLink])
      fail(joint.sourceLine, "link '" + model.links[joint.childLink].name + "' has multiple parent joints");
    hasParent[joint.childLink] = true;
    outgoing[joint.parentLink].push_back(index);
  }

  std::optional<size_t> root;
  for (size_t index = 0; index < model.links.size(); ++index) {
    if (hasParent[index]) continue;
    if (root) fail(model.links[index].sourceLine, "multiple root links: '" + model.links[*root].name +
                   "' and '" + model.links[index].name + "'");
    root = index;
  }
  if (!root) fail(model.links.front().sourceLine, "no root link: the joint graph contains a cycle");

  std::vector<size_t> traversal{*root};
  std::vector<size_t> jointOrder;
  jointOrder.reserve(model.joints.size());
  for (size_t i = 0; i < traversal.size(); ++i) {
    for (const auto joint : outgoing[traversal[i]]) {
      jointOrder.push_back(joint);
      traversal.push_back(model.joints[joint].childLink);
    }
  }
  if (traversal.size() != model.links.size()) {
    std::vector<bool> reached(model.links.size());
    for (const auto link : traversal) reached[link] = true;
    const auto first = static_cast<size_t>(std::find(reached.begin(), reached.end(), false) - reached.begin());
    fail(model.links[first].sourceLine, "link '" + model.links[first].name + "' is disconnected from the root by a cycle");
  }
  model.rootLink = *root;
  model.jointOrder = std::move(jointOrder);
}

UrdfModel parseUrdf(std::string_view xml) {
  if (xml.size() > maxUrdfBytes) fail(1, "document exceeds the input size limit");
  if (const auto nul = xml.find('\0'); nul != std::string_view::npos)
    fail(lineAt(xml, nul), "embedded NUL in XML");
  if (xml.empty()) fail(1, "empty XML document");
  checkCharacterReferences(xml);
  tinyxml2::XMLDocument document;
  if (document.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS)
    fail(std::max(1, document.ErrorLineNum()), document.ErrorStr());
  checkXmlDeclarations(document);
  for (const auto* node = document.FirstChild(); node; node = node->NextSibling())
    if (const auto* text = node->ToText(); text && !trim(text->Value()).empty())
      fail(*text, "unexpected text outside <robot>");
  const auto* robot = document.FirstChildElement();
  if (!robot) fail(1, "missing <robot> document root");
  if (std::string_view(robot->Name()) != "robot") fail(*robot, "expected <robot> document root");
  if (const auto* second = robot->NextSiblingElement()) fail(*second, "document must have a single <robot> root");
  if (const auto* version = robot->Attribute("version"); version && std::string_view(version) != "1.0")
    fail(*robot, "unsupported version '" + std::string(version) + "'; expected URDF 1.0");
  checkChildren(*robot, {"link", "joint", "material", "gazebo", "transmission", "ros2_control"});

  UrdfModel model;
  model.name = requiredAttribute(*robot, "name");
  const auto materials = collectMaterials(*robot);
  LinkIndex linkIndices;
  for (const auto* item = robot->FirstChildElement("link"); item; item = item->NextSiblingElement("link")) {
    auto link = readLink(*item, materials);
    if (!linkIndices.emplace(link.name, model.links.size()).second) fail(*item, "duplicate link name '" + link.name + "'");
    model.links.push_back(std::move(link));
  }
  if (model.links.empty()) fail(*robot, "robot must contain at least one link");
  for (const auto* item = robot->FirstChildElement("joint"); item; item = item->NextSiblingElement("joint"))
    model.joints.push_back(readJoint(*item, linkIndices));
  validateUrdfTopology(model);
  return model;
}

UrdfModel loadUrdf(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Could not open the URDF file");
  const auto bytes = input.tellg();
  if (bytes < 0) throw std::runtime_error("Could not determine the URDF file size");
  if (bytes > static_cast<std::streamoff>(maxUrdfBytes)) fail(1, "document exceeds the input size limit");
  input.seekg(0);
  std::string xml(static_cast<size_t>(bytes), '\0');
  if (!input.read(xml.data(), static_cast<std::streamsize>(xml.size())))
    throw std::runtime_error("Could not read the URDF file");
  return parseUrdf(xml);
}

} // namespace container::robotics
