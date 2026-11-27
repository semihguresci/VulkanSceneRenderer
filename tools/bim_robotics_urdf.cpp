#include "Container/robotics/UrdfAssetExport.h"
#include "Container/utility/AtomicFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace robotics = container::robotics;

constexpr std::string_view usage =
    "Usage: BimRoboticsUrdf INPUT.urdf OUTPUT.json|OUTPUT.glb [JOINTS.json] [--package-root ROOT]...\n";

void checkKeys(const Json& object, std::initializer_list<std::string_view> keys) {
  if (!object.is_object())
    throw std::invalid_argument("Expected a URDF scenario object");
  for (auto field = object.begin(); field != object.end(); ++field)
    if (std::ranges::find(keys, std::string_view(field.key())) == keys.end())
      throw std::invalid_argument("Unknown URDF scenario field " + field.key());
}

double real(const Json& value) {
  if (!value.is_number() || !std::isfinite(value.get<double>()))
    throw std::invalid_argument("URDF scenario requires a finite number");
  return value.get<double>();
}

glm::dvec3 vector3(const Json& value) {
  if (!value.is_array() || value.size() != 3)
    throw std::invalid_argument("Expected a three-component URDF scenario vector");
  return {real(value[0]), real(value[1]), real(value[2])};
}

std::vector<robotics::UrdfJointValue> jointValues(const Json& value) {
  if (!value.is_object())
    throw std::invalid_argument("Joint settings must map joint names to positions");
  std::vector<robotics::UrdfJointValue> result;
  result.reserve(value.size());
  for (auto field = value.begin(); field != value.end(); ++field)
    result.push_back({field.key(), real(field.value())});
  return result;
}

Json readScenario(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream)
    throw std::runtime_error("Could not open the URDF scenario");
  const auto bytes = stream.tellg();
  if (bytes < 0 || bytes > std::streamoff{4 * 1024 * 1024})
    throw std::invalid_argument("URDF scenario exceeds its input size limit");
  stream.seekg(0);
  std::string contents(static_cast<size_t>(bytes), '\0');
  if (!stream.read(contents.data(), bytes))
    throw std::runtime_error("Could not read the URDF scenario");
  auto value = Json::parse(contents);
  checkKeys(value, {"version", "root", "positions", "targets", "ticks"});
  if (!value.at("version").is_number_integer() || value.at("version") != 1)
    throw std::invalid_argument("Unsupported URDF scenario version");
  return value;
}

Json matrix(const auto& transform) {
  Json columns = Json::array();
  for (glm::length_t column = 0; column < transform.length(); ++column) {
    Json values = Json::array();
    for (glm::length_t row = 0; row < transform[column].length(); ++row)
      values.push_back(transform[column][row]);
    columns.push_back(std::move(values));
  }
  return columns;
}

Json xyz(glm::dvec3 value) {
  return Json::array({value.x, value.y, value.z});
}

Json geometry(const robotics::UrdfGeometry& shape) {
  if (const auto* box = std::get_if<robotics::UrdfBox>(&shape))
    return {{"type", "box"}, {"size", xyz(box->size)}};
  if (const auto* cylinder = std::get_if<robotics::UrdfCylinder>(&shape))
    return {{"type", "cylinder"}, {"radius", cylinder->radius}, {"length", cylinder->length}};
  if (const auto* sphere = std::get_if<robotics::UrdfSphere>(&shape))
    return {{"type", "sphere"}, {"radius", sphere->radius}};
  const auto& mesh = std::get<robotics::UrdfMesh>(shape);
  return {{"type", "mesh"}, {"uri", mesh.uri}, {"scale", xyz(mesh.scale)}};
}

Json material(const std::optional<robotics::UrdfMaterial>& value) {
  if (!value)
    return nullptr;
  const auto rgba = value->rgba ? Json::array({value->rgba->r, value->rgba->g, value->rgba->b, value->rgba->a})
                                : Json(nullptr);
  return {{"name", value->name}, {"rgba", rgba}, {"textureUri", value->textureUri}};
}

std::string_view jointType(robotics::UrdfJointType type) {
  using enum robotics::UrdfJointType;
  switch (type) {
  case Fixed: return "fixed";
  case Revolute: return "revolute";
  case Continuous: return "continuous";
  case Prismatic: return "prismatic";
  }
  throw std::logic_error("Unknown URDF joint type");
}

Json limits(const std::optional<robotics::UrdfJointLimits>& value) {
  if (!value)
    return nullptr;
  return {{"lower", value->lower ? Json(*value->lower) : Json(nullptr)},
          {"upper", value->upper ? Json(*value->upper) : Json(nullptr)},
          {"velocity", value->velocity}, {"effort", value->effort}};
}

Json report(const robotics::UrdfRuntime& robot) {
  const auto& model = robot.model();
  const auto& clock = robot.clock();
  Json result{{"schema", "container.urdf-kinematic-state"}, {"version", 1},
              {"robot", model.name}, {"root", model.links.at(model.rootLink).name},
              {"coordinateConvention", "ROS REP-103"}, {"matrixLayout", "column-major"},
              {"ticks", clock.tick()}, {"seconds", clock.seconds()},
              {"links", Json::array()}, {"joints", Json::array()}};
  for (size_t index = 0; index < model.links.size(); ++index) {
    const auto& link = model.links[index];
    Json node{{"name", link.name}, {"worldFromLink", matrix(robot.worldFromLinks()[index])},
              {"inertial", nullptr}, {"visuals", Json::array()}, {"collisions", Json::array()}};
    if (link.inertial) {
      node["inertial"] = {{"mass", link.inertial->mass}, {"tensor", matrix(link.inertial->inertia)},
          {"linkFromInertial", matrix(robotics::urdfPoseMatrix(link.inertial->linkFromInertial))},
          {"worldFromInertial", matrix(*robot.worldFromInertial(index))}};
    }
    for (size_t i = 0; i < link.visuals.size(); ++i) {
      const auto& visual = link.visuals[i];
      node["visuals"].push_back({{"name", visual.name}, {"geometry", geometry(visual.geometry)},
          {"material", material(visual.material)},
          {"linkFromVisual", matrix(robotics::urdfPoseMatrix(visual.linkFromVisual))},
          {"worldFromVisual", matrix(robot.worldFromVisual(index, i))}});
    }
    for (size_t i = 0; i < link.collisions.size(); ++i) {
      const auto& shape = link.collisions[i];
      node["collisions"].push_back({{"name", shape.name}, {"geometry", geometry(shape.geometry)},
          {"linkFromCollision", matrix(robotics::urdfPoseMatrix(shape.linkFromCollision))},
          {"worldFromCollision", matrix(robot.worldFromCollision(index, i))}});
    }
    result["links"].push_back(std::move(node));
  }
  for (size_t index = 0; index < model.joints.size(); ++index) {
    const auto& joint = model.joints[index];
    const auto& state = robot.joints()[index];
    result["joints"].push_back({{"name", joint.name}, {"type", jointType(joint.type)},
        {"parent", model.links[joint.parentLink].name}, {"child", model.links[joint.childLink].name},
        {"axis", xyz(joint.axis)}, {"parentFromJoint", matrix(robotics::urdfPoseMatrix(joint.parentFromJoint))},
        {"limits", limits(joint.limits)}, {"position", state.position}, {"velocity", state.velocity},
        {"target", state.target}, {"damping", joint.damping}, {"friction", joint.friction}});
  }
  return result;
}

bool aliases(const std::filesystem::path& output, const std::filesystem::path& input) {
  std::error_code error;
  return std::filesystem::equivalent(output, input, error);
}

template<class Char>
int run(int argc, Char** argv) {
  if (argc == 2 && std::filesystem::path(argv[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (argc < 3) {
    std::cerr << usage;
    return 2;
  }
  try {
    const std::filesystem::path input(argv[1]), output(argv[2]);
    const bool exportAsset = output.extension() == ".glb";
    if (output.extension() != ".json" && !exportAsset)
      throw std::invalid_argument("URDF destination must use .json or .glb");
    std::optional<std::filesystem::path> scenarioPath;
    std::vector<std::filesystem::path> packageRoots;
    for (int i = 3; i < argc; ++i) {
      const std::filesystem::path argument(argv[i]);
      if (argument == "--package-root") {
        if (!exportAsset || i + 1 == argc)
          throw std::invalid_argument("--package-root requires a directory and .glb output");
        packageRoots.emplace_back(argv[++i]);
      } else if (scenarioPath || argument.generic_u8string().starts_with(u8"--")) {
        throw std::invalid_argument(std::string(usage));
      } else {
        scenarioPath = argument;
      }
    }
    if (aliases(output, input) || (scenarioPath && aliases(output, *scenarioPath)))
      throw std::invalid_argument("URDF destination must differ from each input");
    const auto scenario = scenarioPath ? readScenario(*scenarioPath) : Json::object();
    robotics::UrdfPose root;
    if (const auto value = scenario.find("root"); value != scenario.end()) {
      checkKeys(*value, {"xyz", "rpy"});
      root = robotics::urdfPoseFromRpy(vector3(value->value("xyz", Json::array({0, 0, 0}))),
                                      vector3(value->value("rpy", Json::array({0, 0, 0}))));
    }
    const auto count = scenario.value("ticks", Json(0));
    if (!count.is_number_integer() || count < 0 || count > 100'000)
      throw std::invalid_argument("URDF scenario ticks must be an integer from 0 to 100000");
    auto model = robotics::loadUrdf(input);
    const auto positions = scenario.contains("positions") ? jointValues(scenario.at("positions"))
                                                           : std::vector<robotics::UrdfJointValue>{};
    const auto targets = scenario.contains("targets") ? jointValues(scenario.at("targets"))
                                                       : std::vector<robotics::UrdfJointValue>{};
    const auto ticks = count.get<uint64_t>();
    if (exportAsset) {
      const auto result = robotics::writeUrdfAsset(output, input, model, {root, positions, targets, ticks}, packageRoots);
      for (const auto& warning : result.diagnostics)
        std::cerr << "BimRoboticsUrdf: " << warning << '\n';
      std::cout << "Exported " << result.visualCount << " visuals, " << result.collisionCount
                << " collision shapes and " << result.keyframeCount << " motion samples\n";
      return 0;
    }
    robotics::UrdfRuntime robot(std::move(model), root);
    if (!positions.empty()) robot.setPositions(positions);
    if (!targets.empty()) robot.setTargets(targets);
    for (uint64_t i = 0; i < ticks; ++i) robot.step();
    container::util::writeFileAtomically(output, report(robot).dump(2) + '\n');
    std::cout << "Loaded " << robot.model().links.size() << " links and " << robot.model().joints.size()
              << " joints at tick " << robot.clock().tick() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsUrdf: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return run(argc, argv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
