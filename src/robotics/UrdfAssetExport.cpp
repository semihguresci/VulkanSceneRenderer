#include "Container/robotics/UrdfAssetExport.h"

#include "Container/geometry/GltfPackedSceneExport.h"
#include "UrdfAssetGeometry.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace container::robotics {
namespace {

using Object = tinygltf::Value::Object;
using Array = tinygltf::Value::Array;

constexpr size_t kMaxNodes = 262'144;
constexpr size_t kMaxBufferBytes = 256 * 1024 * 1024;
constexpr size_t kMaxMotionKeys = 2'000'000;
constexpr uint64_t kMaxTicks = 100'000;
constexpr uint64_t kMaxPoseEvaluations = 32'000'000;

static_assert(std::endian::native == std::endian::little && sizeof(float) == 4);

tinygltf::Value text(std::string_view value) {
  return tinygltf::Value(std::string(value));
}

tinygltf::Value vectorValue(const glm::dvec3& value) {
  return tinygltf::Value(Array{tinygltf::Value(value.x), tinygltf::Value(value.y),
                             tinygltf::Value(value.z)});
}

int checkedIndex(size_t count) {
  if (count > static_cast<size_t>(std::numeric_limits<int>::max()))
    throw std::length_error("URDF asset exceeds the glTF index range");
  return static_cast<int>(count);
}

float component(double value) {
  if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
    throw std::invalid_argument("URDF asset contains a value outside the renderable float range");
  return static_cast<float>(value);
}

std::array<float, 3> components(const glm::dvec3& value) {
  return {component(value.x), component(value.y), component(value.z)};
}

std::array<float, 4> components(const glm::dquat& value) {
  return {component(value.x), component(value.y), component(value.z), component(value.w)};
}

template<class T>
int appendAccessor(tinygltf::Model& output, const std::vector<T>& values,
                   int type, int componentType, int target = 0) {
  auto& buffer = output.buffers.front().data;
  const size_t aligned = (buffer.size() + 3) & ~size_t{3};
  if (values.empty() || aligned > kMaxBufferBytes ||
      values.size() > (kMaxBufferBytes - aligned) / sizeof(T))
    throw std::length_error("URDF asset exceeds the packed glTF buffer budget");

  const size_t bytes = values.size() * sizeof(T);
  buffer.resize(aligned + bytes);
  std::memcpy(buffer.data() + aligned, values.data(), bytes);
  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = aligned;
  view.byteLength = bytes;
  view.target = target;

  tinygltf::Accessor accessor;
  accessor.bufferView = checkedIndex(output.bufferViews.size());
  accessor.componentType = componentType;
  accessor.count = values.size();
  accessor.type = type;
  const int index = checkedIndex(output.accessors.size());
  output.bufferViews.push_back(std::move(view));
  output.accessors.push_back(std::move(accessor));
  return index;
}

int appendNode(tinygltf::Model& model, std::string name, Object identity, int parent = -1) {
  if (model.nodes.size() >= kMaxNodes)
    throw std::length_error("URDF asset exceeds the scene node budget");
  const int index = checkedIndex(model.nodes.size());
  tinygltf::Node node;
  node.name = std::move(name);
  node.extras = tinygltf::Value(Object{{"urdf", tinygltf::Value(std::move(identity))}});
  model.nodes.push_back(std::move(node));
  if (parent >= 0) model.nodes.at(parent).children.push_back(index);
  return index;
}

void setPose(tinygltf::Node& node, const UrdfPose& pose) {
  const auto matrix = urdfPoseMatrix(pose);
  const auto translation = components(pose.position);
  const auto rotation = components(glm::quat_cast(glm::dmat3(matrix)));
  node.translation.assign(translation.begin(), translation.end());
  node.rotation.assign(rotation.begin(), rotation.end());
}

std::string_view jointType(UrdfJointType type) {
  using enum UrdfJointType;
  switch (type) {
  case Fixed: return "fixed";
  case Revolute: return "revolute";
  case Continuous: return "continuous";
  case Prismatic: return "prismatic";
  }
  throw std::invalid_argument("URDF asset contains an unsupported joint type");
}

// UrdfRuntime validates the same axis before producing the sampled positions.
glm::dvec3 unitAxis(const UrdfJoint& joint) {
  const glm::dvec3 magnitude = glm::abs(joint.axis);
  const auto largest = std::max({magnitude.x, magnitude.y, magnitude.z});
  return glm::normalize(joint.axis / largest);
}

glm::dquat rotationAt(const glm::dvec3& axis, double position) {
  return glm::angleAxis(std::remainder(position, 2 * std::numbers::pi), axis);
}

void setJointMotion(tinygltf::Node& node, const UrdfJoint& joint, double position) {
  if (joint.type == UrdfJointType::Fixed) return;
  const auto axis = unitAxis(joint);
  if (joint.type == UrdfJointType::Prismatic) {
    const auto translation = components(axis * position);
    node.translation.assign(translation.begin(), translation.end());
  } else {
    const auto rotation = components(rotationAt(axis, position));
    node.rotation.assign(rotation.begin(), rotation.end());
  }
}

int appendMesh(tinygltf::Model& output, const detail::UrdfAssetGeometry& geometry,
               const std::string& name, int material) {
  const auto& positions = geometry.positions;
  if (positions.empty() || positions.size() != geometry.normals.size() ||
      geometry.indices.empty() || geometry.indices.size() % 3 != 0 ||
      std::ranges::any_of(geometry.indices, [&](uint32_t index) { return index >= positions.size(); }))
    throw std::invalid_argument("URDF asset contains invalid triangle topology");

  static_assert(sizeof(std::array<float, 3>) == 3 * sizeof(float));
  std::vector<std::array<float, 3>> vertexPositions, normals;
  vertexPositions.reserve(positions.size());
  normals.reserve(positions.size());
  glm::vec3 low = positions.front(), high = low;
  for (size_t index = 0; index < positions.size(); ++index) {
    const auto& p = positions[index];
    vertexPositions.push_back(components(glm::dvec3(p)));
    normals.push_back(components(glm::dvec3(geometry.normals[index])));
    low = glm::min(low, p);
    high = glm::max(high, p);
  }

  tinygltf::Primitive primitive;
  primitive.mode = TINYGLTF_MODE_TRIANGLES;
  primitive.material = material;
  const int position = appendAccessor(output, vertexPositions, TINYGLTF_TYPE_VEC3,
      TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TARGET_ARRAY_BUFFER);
  output.accessors[position].minValues = {low.x, low.y, low.z};
  output.accessors[position].maxValues = {high.x, high.y, high.z};
  primitive.attributes.emplace("POSITION", position);
  primitive.attributes.emplace("NORMAL", appendAccessor(output, normals, TINYGLTF_TYPE_VEC3,
      TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TARGET_ARRAY_BUFFER));
  primitive.indices = appendAccessor(output, geometry.indices, TINYGLTF_TYPE_SCALAR,
      TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
  tinygltf::Mesh mesh;
  mesh.name = name;
  mesh.primitives.push_back(std::move(primitive));
  const int index = checkedIndex(output.meshes.size());
  output.meshes.push_back(std::move(mesh));
  return index;
}

using MaterialKey = std::pair<std::string, std::array<double, 4>>;

int materialIndex(tinygltf::Model& model, std::map<MaterialKey, int>& known,
                  const UrdfMaterial* material, bool collision) {
  const auto rgba = collision ? glm::dvec4(.28, .58, .80, 1)
      : material && material->rgba ? *material->rgba : glm::dvec4(.62, .68, .76, 1);
  const std::array<double, 4> color{rgba.r, rgba.g, rgba.b, rgba.a};
  if (std::ranges::any_of(color, [](double x) { return !std::isfinite(x) || x < 0 || x > 1; }))
    throw std::invalid_argument("URDF asset material RGBA must be finite and in [0, 1]");
  const auto name = material && !material->name.empty() ? material->name
      : collision ? "URDF collision proxy" : "URDF visual";
  const auto [at, inserted] = known.try_emplace(MaterialKey{name, color}, checkedIndex(model.materials.size()));
  if (inserted) {
    tinygltf::Material entry;
    entry.name = name;
    entry.alphaMode = rgba.a < 1 ? "BLEND" : "OPAQUE";
    entry.pbrMetallicRoughness.baseColorFactor.assign(color.begin(), color.end());
    entry.pbrMetallicRoughness.metallicFactor = 0;
    entry.pbrMetallicRoughness.roughnessFactor = .65;
    model.materials.push_back(std::move(entry));
  }
  return at->second;
}

struct AssetWriter {
  tinygltf::Model model;
  std::map<MaterialKey, int> materials;
  UrdfAssetExportResult result;
  const std::filesystem::path& urdfPath;
  std::span<const std::filesystem::path> packageRoots;

  void appendShape(int parent, const std::string& link, size_t index,
                   const std::string& authoredName, const UrdfPose& pose,
                   const UrdfGeometry& shape, const UrdfMaterial* material,
                   bool collision) {
    const std::string role = collision ? "collision" : "visual";
    const auto name = authoredName.empty() ? role + " " + std::to_string(index + 1) : authoredName;
    try {
      const int node = appendNode(model, name, {{"role", text(role)}, {"name", text(authoredName)},
          {"link", text(link)}, {"index", tinygltf::Value(checkedIndex(index))}}, parent);
      setPose(model.nodes[node], pose);
      if (const auto* source = std::get_if<UrdfMesh>(&shape)) {
        const auto scale = components(source->scale);
        if (std::ranges::any_of(scale, [](float value) { return value == 0; }))
          throw std::invalid_argument("mesh scale collapses an axis in the renderer");
        model.nodes[node].scale.assign(scale.begin(), scale.end());
      }
      if (material && !material->textureUri.empty())
        result.diagnostics.push_back("Texture omitted for " + role + " '" + name + "' on link '" + link + "'");

      const auto geometry = detail::loadUrdfAssetGeometry(shape, urdfPath, packageRoots);
      const int materialId = materialIndex(model, materials, material, collision);
      model.nodes[node].mesh = appendMesh(model, geometry, name, materialId);
    } catch (const std::exception& error) {
      throw std::invalid_argument("URDF link '" + link + "' " + role + " '" + name + "': " + error.what());
    }
  }

  std::vector<int> appendScene(const UrdfRuntime& robot, const UrdfPose& rootPose, bool collision) {
    const auto& source = robot.model();
    const auto kind = collision ? "collision" : "visual";
    const int frame = appendNode(model, source.name, {{"role", text("frame")},
        {"name", text(source.name)}, {"scene", text(kind)}});
    // Keep all descendants in authored link coordinates. The wrapper takes
    // metric REP-103 (x, y, z) to renderer coordinates (-y, z, -x).
    model.nodes[frame].matrix = {0, 0, -1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};

    std::vector<int> links;
    links.reserve(source.links.size());
    for (const auto& link : source.links)
      links.push_back(appendNode(model, link.name, {{"role", text("link")}, {"name", text(link.name)}}));
    model.nodes[frame].children.push_back(links.at(source.rootLink));
    setPose(model.nodes[links[source.rootLink]], rootPose);

    std::vector<int> joints(source.joints.size());
    for (size_t index = 0; index < source.joints.size(); ++index) {
      const auto& joint = source.joints[index];
      const auto identity = [&](std::string_view role) {
        return Object{{"role", text(role)}, {"name", text(joint.name)}, {"type", text(jointType(joint.type))},
                      {"axis", vectorValue(joint.axis)}};
      };
      const int origin = appendNode(model, "joint-origin/" + joint.name, identity("jointOrigin"), links[joint.parentLink]);
      setPose(model.nodes[origin], joint.parentFromJoint);
      const int motion = appendNode(model, "joint-motion/" + joint.name, identity("jointMotion"), origin);
      setJointMotion(model.nodes[motion], joint, robot.joints()[index].position);
      model.nodes[motion].children.push_back(links[joint.childLink]);
      joints[index] = motion;
    }

    for (size_t index = 0; index < source.links.size(); ++index) {
      const auto& link = source.links[index];
      if (collision) {
        for (size_t i = 0; i < link.collisions.size(); ++i) {
          const auto& shape = link.collisions[i];
          appendShape(links[index], link.name, i, shape.name, shape.linkFromCollision, shape.geometry, nullptr, true);
          ++result.collisionCount;
        }
      } else {
        for (size_t i = 0; i < link.visuals.size(); ++i) {
          const auto& shape = link.visuals[i];
          appendShape(links[index], link.name, i, shape.name, shape.linkFromVisual,
                      shape.geometry, shape.material ? &*shape.material : nullptr, false);
          ++result.visualCount;
        }
      }
    }

    tinygltf::Scene scene;
    scene.name = collision ? "Collision" : "Visual";
    scene.nodes.push_back(frame);
    model.scenes.push_back(std::move(scene));
    return joints;
  }
};

struct MotionTrack {
  size_t joint{};
  std::vector<double> positions;
};

struct MotionKey {
  float time{};
  double position{};
};

std::vector<MotionTrack> sampleMotion(UrdfRuntime& robot, uint64_t ticks) {
  if (ticks == 0) return {};
  const auto& source = robot.model();
  uint64_t visitCount = source.links.size() + source.joints.size();
  for (const auto& link : source.links)
    visitCount += link.visuals.size() + link.collisions.size() + size_t(bool(link.inertial));
  if (visitCount > kMaxPoseEvaluations / ticks)
    throw std::length_error("URDF asset motion exceeds the fixed-step evaluation budget");

  std::vector<MotionTrack> tracks;
  for (size_t i = 0; i < robot.joints().size(); ++i) {
    const auto& state = robot.joints()[i];
    if (state.position == state.target) continue;
    if (tracks.size() >= kMaxMotionKeys / (ticks + 1))
      throw std::length_error("URDF asset motion exceeds the sampled key budget");
    tracks.push_back({i, {state.position}});
    tracks.back().positions.reserve(static_cast<size_t>(ticks + 1));
  }
  if (tracks.empty()) return tracks;

  for (uint64_t i = 0; i < ticks; ++i) {
    robot.step();
    for (auto& track : tracks) track.positions.push_back(robot.joints()[track.joint].position);
  }
  std::erase_if(tracks, [](const MotionTrack& track) {
    return track.positions.front() == track.positions.back();
  });
  return tracks;
}

std::vector<MotionKey> expandTrack(const MotionTrack& track, double secondsPerTick,
                                  bool rotation, size_t maxKeys) {
  if (maxKeys == 0) throw std::length_error("URDF asset motion exceeds the exported key budget");
  std::vector<MotionKey> keys{{0, track.positions.front()}};
  for (size_t i = 1; i < track.positions.size(); ++i) {
    const auto before = track.positions[i - 1], after = track.positions[i];
    // glTF interpolates rotations along the shortest quaternion arc. Split
    // a long authored step so that even a complete turn remains observable.
    const auto pieces = rotation ? std::max(1., std::ceil(std::abs(after - before) / (std::numbers::pi / 2))) : 1.;
    if (!std::isfinite(pieces) || pieces > static_cast<double>(maxKeys - keys.size()))
      throw std::length_error("URDF asset motion exceeds the exported key budget");
    const size_t count = static_cast<size_t>(pieces);
    for (size_t part = 1; part <= count; ++part) {
      const double fraction = static_cast<double>(part) / pieces;
      const float time = component((static_cast<double>(i - 1) + fraction) * secondsPerTick);
      if (time <= keys.back().time)
        throw std::invalid_argument("URDF asset motion keys exceed glTF time precision");
      keys.push_back({time, part == count ? after : std::lerp(before, after, fraction)});
    }
  }
  return keys;
}

void appendAnimation(AssetWriter& writer, const UrdfRuntime& robot, std::span<const MotionTrack> tracks,
                     std::span<const int> visualJoints, std::span<const int> collisionJoints) {
  if (tracks.empty()) return;
  tinygltf::Animation animation;
  animation.name = "URDF joint motion";
  for (const auto& track : tracks) {
    const auto& joint = robot.model().joints[track.joint];
    const bool rotation = joint.type != UrdfJointType::Prismatic;
    const auto keys = expandTrack(track, robot.clock().tickSeconds(),
                                  rotation, kMaxMotionKeys - writer.result.keyframeCount);
    writer.result.keyframeCount += keys.size();

    const auto axis = unitAxis(joint);
    std::vector<float> times;
    times.reserve(keys.size());
    for (const auto& key : keys) times.push_back(key.time);
    tinygltf::AnimationSampler sampler;
    sampler.interpolation = "LINEAR";
    sampler.input = appendAccessor(writer.model, times, TINYGLTF_TYPE_SCALAR, TINYGLTF_COMPONENT_TYPE_FLOAT);
    writer.model.accessors[sampler.input].minValues = {times.front()};
    writer.model.accessors[sampler.input].maxValues = {times.back()};
    if (rotation) {
      static_assert(sizeof(std::array<float, 4>) == 4 * sizeof(float));
      std::vector<std::array<float, 4>> values;
      values.reserve(keys.size());
      auto previous = rotationAt(axis, keys.front().position);
      for (const auto& key : keys) {
        auto value = rotationAt(axis, key.position);
        if (glm::dot(previous, value) < 0) value = -value;
        values.push_back(components(value));
        previous = value;
      }
      sampler.output = appendAccessor(writer.model, values, TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_FLOAT);
    } else {
      std::vector<std::array<float, 3>> values;
      values.reserve(keys.size());
      for (const auto& key : keys) values.push_back(components(axis * key.position));
      sampler.output = appendAccessor(writer.model, values, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
    }

    const auto index = checkedIndex(animation.samplers.size());
    animation.samplers.push_back(std::move(sampler));
    const auto connect = [&](int node) {
      tinygltf::AnimationChannel channel;
      channel.sampler = index;
      channel.target_node = node;
      channel.target_path = rotation ? "rotation" : "translation";
      animation.channels.push_back(std::move(channel));
    };
    connect(visualJoints[track.joint]);
    if (!collisionJoints.empty()) connect(collisionJoints[track.joint]);
  }
  writer.model.animations.push_back(std::move(animation));
}

} // namespace

UrdfAssetExportResult writeUrdfAsset(const std::filesystem::path& output,
    const std::filesystem::path& urdfPath, const UrdfModel& model, const UrdfAssetScenario& scenario,
    std::span<const std::filesystem::path> packageRoots) {
  if (output.extension() != ".glb")
    throw std::invalid_argument("URDF asset destination must use .glb");
  if (scenario.ticks > kMaxTicks)
    throw std::invalid_argument("URDF asset motion exceeds the supported tick count");
  std::error_code error;
  if (std::filesystem::equivalent(output, urdfPath, error))
    throw std::invalid_argument("URDF asset destination must differ from the source");

  UrdfRuntime robot(model, scenario.worldFromRoot);
  robot.setPositions(scenario.positions);
  robot.setTargets(scenario.targets);

  AssetWriter writer{.urdfPath = urdfPath, .packageRoots = packageRoots};
  writer.model.asset.version = "2.0";
  writer.model.asset.generator = "VulkanSceneRenderer BimRoboticsUrdf";
  writer.model.defaultScene = 0;
  writer.model.buffers.emplace_back();
  const auto visuals = writer.appendScene(robot, scenario.worldFromRoot, false);
  std::vector<int> collisions;
  if (std::ranges::any_of(robot.model().links, [](const auto& link) { return !link.collisions.empty(); }))
    collisions = writer.appendScene(robot, scenario.worldFromRoot, true);
  if (writer.model.meshes.empty())
    throw std::invalid_argument("URDF asset has no renderable visual or collision geometry");
  if (writer.result.visualCount == 0) writer.model.defaultScene = 1;

  const auto tracks = sampleMotion(robot, scenario.ticks);
  appendAnimation(writer, robot, tracks, visuals, collisions);
  geometry::gltf::writeExportModel(writer.model, output);
  return std::move(writer.result);
}

} // namespace container::robotics
