#pragma once

#include <Container/geometry/GltfAnimation.h>
#include <Container/utility/SceneData.h>

#include <optional>

namespace container::scene { class SceneGraph; }
namespace container::material { struct Material; }

namespace container::geometry::gltf {

struct AnimatedCameraState {
  container::gpu::CameraData data;
  glm::mat4 view{1.0f};
  glm::mat4 projection{1.0f};
  // Cascade reconstruction needs finite far corners even when the main
  // authored perspective projection has an infinite far plane.
  glm::mat4 shadowProjection{1.0f};
  float nearPlane{0.1f};
  float farPlane{1000.0f};
  bool orthographic{};
};

// Canonical binary32 node evaluator used by playback and detached camera-pose
// preparation. Matrix representation takes precedence; TRS normalizes the
// quaternion and composes T*R*S. Rejects malformed or non-finite results.
[[nodiscard]] glm::mat4 EvaluateAnimatedNodeLocalTransform(const tinygltf::Node&);

// Uses sourceNodeIndex associations, including non-renderable joint/groups;
// primitive children retain their own local transforms and identities. Only
// the local node flags explicitly changed by animation are published.
bool ApplyAnimatedNodeTransforms(const tinygltf::Model& source,
                                 const AnimationDirtySet& dirty,
                                 container::scene::SceneGraph& graph);

// Copies shader-supported numeric properties and per-slot UV offset/scale/
// rotation. Uploaded texture handles, sampler choices, UV set/channel selection
// and material classification remain unchanged.
void UpdateAnimatedMaterialProperties(const tinygltf::Material& source,
                                      container::material::Material& material);

// Camera selection identifies a source node, so multiple instances of one
// camera retain distinct transforms. Returns nullopt for a missing binding.
// Infinite perspective frusta use an exact infinite projection and a finite
// 1e6-unit lighting/culling span, without altering the source camera values.
std::optional<AnimatedCameraState> EvaluateAnimatedCamera(
  const tinygltf::Model& source, const container::scene::SceneGraph& graph,
  size_t sourceNode, float viewportAspect);
// The same renderer projection/view convention for a detached selected lens.
// No Model/graph copy, mutation, allocation or GPU work is required.
std::optional<AnimatedCameraState> EvaluateAnimatedCamera(
  const tinygltf::Camera&,const glm::mat4& world,float viewportAspect);

} // namespace container::geometry::gltf
