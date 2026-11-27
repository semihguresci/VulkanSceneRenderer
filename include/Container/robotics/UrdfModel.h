#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace container::robotics {

struct UrdfPose {
  glm::dvec3 position{0.0};
  glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
};

[[nodiscard]] UrdfPose urdfPoseFromRpy(glm::dvec3 position, glm::dvec3 rpy);
[[nodiscard]] glm::dmat4 urdfPoseMatrix(const UrdfPose& pose);

struct UrdfBox { glm::dvec3 size; };
struct UrdfCylinder { double radius; double length; };
struct UrdfSphere { double radius; };
struct UrdfMesh { std::string uri; glm::dvec3 scale{1.0}; };
using UrdfGeometry = std::variant<UrdfBox, UrdfCylinder, UrdfSphere, UrdfMesh>;

struct UrdfMaterial {
  std::string name;
  std::optional<glm::dvec4> rgba;
  std::string textureUri;
};

struct UrdfVisual {
  std::string name;
  UrdfPose linkFromVisual;
  UrdfGeometry geometry;
  std::optional<UrdfMaterial> material;
};

struct UrdfCollision {
  std::string name;
  UrdfPose linkFromCollision;
  UrdfGeometry geometry;
};

struct UrdfInertial {
  UrdfPose linkFromInertial;
  double mass{};
  // Symmetric inertia tensor about the COM, in inertial-frame kg m².
  glm::dmat3 inertia{0.0};
};

struct UrdfLink {
  std::string name;
  std::optional<UrdfInertial> inertial;
  std::vector<UrdfVisual> visuals;
  std::vector<UrdfCollision> collisions;
  int sourceLine{};
};

enum class UrdfJointType { Fixed, Revolute, Continuous, Prismatic };

struct UrdfJointLimits {
  std::optional<double> lower;
  std::optional<double> upper;
  // Velocity and effort use SI units for the joint type.
  double velocity{};
  double effort{};
};

struct UrdfJoint {
  std::string name;
  UrdfJointType type{UrdfJointType::Fixed};
  size_t parentLink{};
  size_t childLink{};
  UrdfPose parentFromJoint;
  glm::dvec3 axis{1, 0, 0};
  std::optional<UrdfJointLimits> limits;
  double damping{};
  double friction{};
  int sourceLine{};
};

struct UrdfModel {
  std::string name;
  std::vector<UrdfLink> links;
  std::vector<UrdfJoint> joints;
  size_t rootLink{};
  std::vector<size_t> jointOrder;
};

// Header, shapes, materials and a connected single-root articulation tree.
// Mesh and texture URIs remain authored resource references.
[[nodiscard]] UrdfModel parseUrdf(std::string_view xml);
[[nodiscard]] UrdfModel loadUrdf(const std::filesystem::path& path);
// Validate structure and rebuild rootLink/jointOrder in parent-first order.
void validateUrdfTopology(UrdfModel& model);

} // namespace container::robotics
