#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace container::geometry::dotbim {
struct Model;
}

namespace container::robotics {

struct RoboticsScene;

enum class BimColliderKind : uint8_t { StaticMesh, DoorMesh, DynamicBox };

struct BimCollisionMesh {
  std::vector<glm::vec3> vertices;
  std::vector<uint32_t> indices;
};

struct BimCollider {
  std::string entityId;
  std::string representationId;
  uint32_t sourceMeshId{};
  BimColliderKind kind{BimColliderKind::StaticMesh};
  // Vertices and half extents are local to this world origin, in REP-103 metres.
  glm::dvec3 originMeters{0.0};
  std::shared_ptr<const BimCollisionMesh> mesh;
  glm::vec3 halfExtents{0.f};
  bool supportCandidate{false};
};

struct BimCollisionDiagnostic {
  std::string entityId;
  std::string representationId;
  std::string code;
  std::string message;
};

struct CookedBimCollision {
  std::vector<BimCollider> colliders;
  std::vector<BimCollisionDiagnostic> diagnostics;
};

struct BimCollisionCookOptions {
  size_t maxColliders{32'768};
  // Counts source triangles per occurrence, including shared mesh instances.
  size_t maxTriangleReferences{4'000'000};
};

// The scene and geometry must come from the same import. Door meshes remain
// separate so the collision world can choose the state of each door instance.
[[nodiscard]] CookedBimCollision cookBimCollisionGeometry(
    const geometry::dotbim::Model& model, const RoboticsScene& scene,
    const BimCollisionCookOptions& options = {});

} // namespace container::robotics
