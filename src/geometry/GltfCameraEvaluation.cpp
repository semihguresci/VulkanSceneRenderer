#include <Container/geometry/GltfAnimationRuntime.h>

#include <Container/common/CommonMath.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

// Pure detached node/camera math is a separate archive member so CPU authoring
// consumers do not pull the SceneGraph runtime adapters into their link.
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

glm::mat4 EvaluateAnimatedNodeLocalTransform(const tinygltf::Node& node) {
  const auto transform = localTransform(node);
  for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row)
    if (!std::isfinite(transform[column][row])) throw std::invalid_argument("non-finite animated runtime node transform");
  return transform;
}

std::optional<AnimatedCameraState> EvaluateAnimatedCamera(
  const tinygltf::Camera& sourceCamera,const glm::mat4& world,float viewportAspect) {
  if (!std::isfinite(viewportAspect) || viewportAspect <= 0) throw std::invalid_argument("invalid animation camera viewport aspect");
  for(int column=0;column<4;++column) for(int row=0;row<4;++row)
    if(!std::isfinite(world[column][row])) throw std::invalid_argument("nonfinite animated camera transform");
  AnimatedCameraState state;
  if (sourceCamera.type == "perspective") {
    const auto& camera = sourceCamera.perspective;
    const float fov = static_cast<float>(camera.yfov);
    const float aspect = camera.aspectRatio > 0 ? static_cast<float>(camera.aspectRatio) : viewportAspect;
    state.nearPlane = static_cast<float>(camera.znear);
    state.farPlane = camera.zfar > 0 ? static_cast<float>(camera.zfar) : std::max(1e6f, state.nearPlane*2);
    if (!std::isfinite(fov) || fov <= 0 || fov >= 3.14159265358979323846f ||
        !std::isfinite(camera.zfar) || camera.zfar < 0 ||
        !std::isfinite(camera.aspectRatio) || camera.aspectRatio < 0 ||
        !std::isfinite(aspect) || aspect <= 0 || !std::isfinite(state.nearPlane) || state.nearPlane <= 0 ||
        !std::isfinite(state.farPlane) || state.farPlane <= state.nearPlane)
      throw std::invalid_argument("invalid animated perspective camera parameters");
    state.projection = container::math::perspectiveRH_ReverseZ(fov,aspect,state.nearPlane,state.farPlane);
    state.shadowProjection = state.projection;
    if (camera.zfar == 0) { state.projection[2][2] = 0; state.projection[3][2] = state.nearPlane; }
  } else if (sourceCamera.type == "orthographic") {
    state.orthographic = true;
    const auto& camera = sourceCamera.orthographic;
    const float xmag = static_cast<float>(camera.xmag), ymag = static_cast<float>(camera.ymag);
    state.nearPlane = static_cast<float>(camera.znear); state.farPlane = static_cast<float>(camera.zfar);
    if (!std::isfinite(xmag) || xmag <= 0 || !std::isfinite(ymag) || ymag <= 0 ||
        !std::isfinite(state.nearPlane) || state.nearPlane < 0 || !std::isfinite(state.farPlane) || state.farPlane <= state.nearPlane)
      throw std::invalid_argument("invalid animated orthographic camera parameters");
    state.projection = container::math::orthoRH_ReverseZ(-xmag,xmag,-ymag,ymag,state.nearPlane,state.farPlane);
    state.shadowProjection = state.projection;
  } else return {};
  const glm::vec3 position(world[3]);
  const glm::vec3 rawForward = -glm::vec3(world[2]), rawUp(world[1]);
  if (!(glm::length(rawForward) > 1e-8f) || !(glm::length(glm::cross(rawForward,rawUp)) > 1e-8f))
    throw std::invalid_argument("singular animated camera transform");
  const glm::vec3 forward = glm::normalize(rawForward);
  state.view = container::math::lookAt(position,position+forward,glm::normalize(rawUp));
  state.data.viewProj = state.projection*state.view;
  // Float cofactors/determinants can overflow even for finite, invertible
  // authored lenses. Compute the inverse in double, then validate the actual
  // float upload instead of accepting a finite all-zero inverse.
  const auto inverse=glm::inverse(glm::dmat4(state.data.viewProj));
  for(int column=0;column<4;++column) for(int row=0;row<4;++row)
    if(!std::isfinite(inverse[column][row]) || std::abs(inverse[column][row])>std::numeric_limits<float>::max())
      throw std::invalid_argument("unrepresentable animated camera inverse");
  state.data.inverseViewProj = glm::mat4(inverse);
  state.data.unjitteredViewProj = state.data.viewProj;
  state.data.cameraWorldPosition = glm::vec4(position,1);
  state.data.cameraForward = glm::vec4(forward,0);
  for(const auto* matrix:{&state.view,&state.projection,&state.shadowProjection,&state.data.viewProj,&state.data.inverseViewProj})
    for(int column=0;column<4;++column) for(int row=0;row<4;++row)
      if(!std::isfinite((*matrix)[column][row])) throw std::invalid_argument("unrepresentable animated camera state");
  for(int column=0;column<4;++column) for(int row=0;row<4;++row) {
    double value{},scale{};
    for(int inner=0;inner<4;++inner) {
      const double product=double(state.data.viewProj[inner][row])*double(state.data.inverseViewProj[column][inner]);
      value+=product;scale+=std::abs(product);
    }
    if(!std::isfinite(value) || std::abs(value-(row==column?1.:0.))>1e-4*std::max(1.,scale))
      throw std::invalid_argument("unstable animated camera inverse");
  }
  return state;
}
} // namespace container::geometry::gltf
