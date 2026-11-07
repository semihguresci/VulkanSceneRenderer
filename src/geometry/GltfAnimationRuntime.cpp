#include <Container/geometry/GltfAnimationRuntime.h>

#include <Container/common/CommonMath.h>
#include <Container/utility/Material.h>
#include <Container/utility/SceneGraph.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::geometry::gltf {
bool ApplyAnimatedNodeTransforms(const tinygltf::Model& source,
                                 const AnimationDirtySet& dirty,
                                 container::scene::SceneGraph& graph) {
  if (dirty.nodes.empty() && dirty.nodeFlags.empty()) return false;
  std::vector<bool> affected(dirty.nodes.empty() ? 0 : source.nodes.size(), false);
  for (size_t index : dirty.nodes) {
    if (index >= source.nodes.size()) throw std::invalid_argument("animated source node index out of range");
    affected[index] = true;
  }
  struct FlagState {
    uint8_t mask{}, values{};
  };
  std::vector<FlagState> flags(dirty.nodeFlags.empty() ? 0 : source.nodes.size());
  constexpr auto visibility = static_cast<uint8_t>(GltfNodeFlag::Visibility);
  constexpr auto selectability = static_cast<uint8_t>(GltfNodeFlag::Selectability);
  constexpr auto hoverability = static_cast<uint8_t>(GltfNodeFlag::Hoverability);
  for (const auto& change : dirty.nodeFlags) {
    if (change.node >= flags.size() || (change.mask & ~(visibility | selectability | hoverability)))
      throw std::invalid_argument("animated node flag index or mask is invalid");
    const auto& node = source.nodes[change.node];
    auto& state = flags[change.node];
    state.mask |= change.mask;
    if ((change.mask & visibility) && ReadAnimatedNodeFlag(node, GltfNodeFlag::Visibility)) state.values |= visibility;
    if ((change.mask & selectability) && ReadAnimatedNodeFlag(node, GltfNodeFlag::Selectability)) state.values |= selectability;
    if ((change.mask & hoverability) && ReadAnimatedNodeFlag(node, GltfNodeFlag::Hoverability)) state.values |= hoverability;
  }
  bool transformsChanged = false;
  bool flagsChanged = false;
  for (uint32_t index = 0; index < graph.nodeCount(); ++index) {
    const auto* node = graph.getNode(index);
    if (!node || node->retired || node->sourceNodeIndex < 0) continue;
    const auto sourceIndex = static_cast<size_t>(node->sourceNodeIndex);
    if (sourceIndex < affected.size() && affected[sourceIndex]) {
      const auto transform = EvaluateAnimatedNodeLocalTransform(source.nodes[sourceIndex]);
      if (node->localTransform != transform) {
        graph.setLocalTransform(index, transform);
        transformsChanged = true;
      }
    }
    if (sourceIndex >= flags.size()) continue;
    const auto [mask, values] = flags[sourceIndex];
    if ((mask & visibility) && node->visible != bool(values & visibility)) {
      graph.setVisible(index, bool(values & visibility));
      flagsChanged = true;
    }
    if ((mask & selectability) && node->selectable != bool(values & selectability)) {
      graph.setSelectable(index, bool(values & selectability));
      flagsChanged = true;
    }
    if ((mask & hoverability) && node->hoverable != bool(values & hoverability)) {
      graph.setHoverable(index, bool(values & hoverability));
      flagsChanged = true;
    }
  }
  if (transformsChanged) graph.updateWorldTransforms();
  return transformsChanged || flagsChanged;
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
