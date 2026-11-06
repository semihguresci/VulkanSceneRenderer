#include <Container/geometry/GltfAnimationRuntime.h>

#include <Container/common/CommonMath.h>
#include <Container/utility/Material.h>
#include <Container/utility/SceneGraph.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::geometry::gltf {
namespace {
glm::mat4 localTransform(const tinygltf::Node& node) {
  if (!node.matrix.empty()) {
    if (node.matrix.size() != 16) throw std::invalid_argument("invalid animated node matrix size");
    glm::mat4 matrix(1);
    for (int i = 0; i < 16; ++i) matrix[i/4][i%4] = static_cast<float>(node.matrix[static_cast<size_t>(i)]);
    return matrix;
  }
  glm::vec3 translation(0), scale(1);
  glm::quat rotation(1,0,0,0);
  if (!node.translation.empty()) {
    if (node.translation.size() != 3) throw std::invalid_argument("invalid animated translation size");
    for (int i = 0; i < 3; ++i) translation[i] = static_cast<float>(node.translation[static_cast<size_t>(i)]);
  }
  if (!node.scale.empty()) {
    if (node.scale.size() != 3) throw std::invalid_argument("invalid animated scale size");
    for (int i = 0; i < 3; ++i) scale[i] = static_cast<float>(node.scale[static_cast<size_t>(i)]);
  }
  if (!node.rotation.empty()) {
    if (node.rotation.size() != 4) throw std::invalid_argument("invalid animated rotation size");
    rotation = glm::normalize(glm::quat(static_cast<float>(node.rotation[3]),
      static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2])));
  }
  return glm::translate(glm::mat4(1),translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1),scale);
}
} // namespace

bool ApplyAnimatedNodeTransforms(const tinygltf::Model& source,
                                 const AnimationDirtySet& dirty,
                                 container::scene::SceneGraph& graph) {
  if (dirty.nodes.empty()) return false;
  std::vector<bool> affected(source.nodes.size(), false);
  for (size_t index : dirty.nodes) {
    if (index >= source.nodes.size()) throw std::invalid_argument("animated source node index out of range");
    affected[index] = true;
  }
  bool changed = false;
  for (uint32_t index = 0; index < graph.nodeCount(); ++index) {
    const auto* node = graph.getNode(index);
    if (!node || node->retired || node->sourceNodeIndex < 0 || static_cast<size_t>(node->sourceNodeIndex) >= affected.size() ||
        !affected[static_cast<size_t>(node->sourceNodeIndex)]) continue;
    const auto transform = localTransform(source.nodes[static_cast<size_t>(node->sourceNodeIndex)]);
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row)
      if (!std::isfinite(transform[column][row])) throw std::invalid_argument("non-finite animated runtime node transform");
    if (node->localTransform != transform) { graph.setLocalTransform(index,transform); changed = true; }
  }
  if (changed) graph.updateWorldTransforms();
  return changed;
}
std::optional<AnimatedCameraState> EvaluateAnimatedCamera(
  const tinygltf::Model& source, const container::scene::SceneGraph& graph,
  size_t sourceNode, float viewportAspect) {
  if (sourceNode >= source.nodes.size()) return {};
  const int cameraIndex = source.nodes[sourceNode].camera;
  if (cameraIndex < 0 || static_cast<size_t>(cameraIndex) >= source.cameras.size()) return {};
  const container::scene::SceneNode* binding = nullptr;
  for (uint32_t i = 0; i < graph.nodeCount(); ++i) {
    const auto* node = graph.getNode(i);
    if (node && node->sourceNodeIndex >= 0 && static_cast<size_t>(node->sourceNodeIndex) == sourceNode) {
      // Context ancestors remain in the editor hierarchy, but only nodes in
      // the selected scene may supply its authored camera.
      if (!graph.isNodeActive(i)) return {};
      binding = node;
      break;
    }
  }
  if (!binding) return {};
  return EvaluateAnimatedCamera(source.cameras[static_cast<size_t>(cameraIndex)],binding->worldTransform,viewportAspect);
}
} // namespace container::geometry::gltf
