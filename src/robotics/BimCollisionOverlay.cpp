#include "Container/robotics/BimCollisionOverlay.h"

#include "Container/geometry/GltfPackedSceneExport.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/robotics/RoboticsScene.h"

#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace container::robotics {
namespace {

struct Layer {
  const char* label;
  std::array<double, 4> color;
  std::vector<float> positions;
  std::vector<uint32_t> indices;

  void append(glm::dvec3 position) {
    constexpr double limit = std::numeric_limits<float>::max();
    for (glm::length_t axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(position[axis]) || std::abs(position[axis]) > limit)
        throw std::invalid_argument("BIM collision overlay position exceeds glTF float range");
      positions.push_back(static_cast<float>(position[axis]));
    }
  }

  uint32_t startPrimitive(size_t vertexCount) const {
    const size_t first = positions.size() / 3;
    if (vertexCount > std::numeric_limits<uint32_t>::max() - first)
      throw std::invalid_argument("BIM collision overlay exceeds glTF index range");
    return static_cast<uint32_t>(first);
  }
};

glm::dvec3 toRenderer(const glm::dmat4& transform, glm::dvec3 point) {
  return glm::dvec3(transform * glm::dvec4(point, 1.0));
}

void addBox(Layer& layer, const glm::dmat4& transform, const BimCollider& collider) {
  constexpr std::array corners{
      glm::dvec3(-1, -1, -1), glm::dvec3(1, -1, -1), glm::dvec3(1, 1, -1), glm::dvec3(-1, 1, -1),
      glm::dvec3(-1, -1, 1), glm::dvec3(1, -1, 1), glm::dvec3(1, 1, 1), glm::dvec3(-1, 1, 1)};
  constexpr std::array<uint32_t, 36> faces{
      0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
      1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  const auto first = layer.startPrimitive(corners.size());
  for (const auto corner : corners) {
    layer.append(toRenderer(transform, collider.originMeters + corner * glm::dvec3(collider.halfExtents)));
  }
  for (auto index : faces)
    layer.indices.push_back(first + index);
}

void addMesh(Layer& layer, const glm::dmat4& transform, const BimCollider& collider) {
  const auto& mesh = *collider.mesh;
  const auto first = layer.startPrimitive(mesh.vertices.size());
  for (const auto vertex : mesh.vertices)
    layer.append(toRenderer(transform, collider.originMeters + glm::dvec3(vertex)));
  for (auto index : mesh.indices)
    layer.indices.push_back(first + index);
}

template <class T>
int addBufferView(tinygltf::Model& model, const std::vector<T>& values, int target) {
  static_assert(std::endian::native == std::endian::little, "glTF buffers are little-endian");
  auto& buffer = model.buffers.front().data;
  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = buffer.size();
  view.byteLength = values.size() * sizeof(T);
  view.target = target;
  const auto* bytes = reinterpret_cast<const unsigned char*>(values.data());
  buffer.insert(buffer.end(), bytes, bytes + view.byteLength);
  model.bufferViews.push_back(std::move(view));
  return static_cast<int>(model.bufferViews.size() - 1);
}

void addLayer(tinygltf::Model& model, const Layer& layer) {
  if (layer.indices.empty())
    return;
  tinygltf::Accessor positions;
  positions.bufferView = addBufferView(model, layer.positions, TINYGLTF_TARGET_ARRAY_BUFFER);
  positions.count = layer.positions.size() / 3;
  positions.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  positions.type = TINYGLTF_TYPE_VEC3;
  positions.minValues.assign(3, std::numeric_limits<double>::infinity());
  positions.maxValues.assign(3, -std::numeric_limits<double>::infinity());
  for (size_t i = 0; i < layer.positions.size(); ++i) {
    const size_t axis = i % 3;
    positions.minValues[axis] = std::min(positions.minValues[axis], double(layer.positions[i]));
    positions.maxValues[axis] = std::max(positions.maxValues[axis], double(layer.positions[i]));
  }
  const int positionId = static_cast<int>(model.accessors.size());
  model.accessors.push_back(std::move(positions));

  tinygltf::Accessor indices;
  indices.bufferView = addBufferView(model, layer.indices, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
  indices.count = layer.indices.size();
  indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
  indices.type = TINYGLTF_TYPE_SCALAR;
  const int indexId = static_cast<int>(model.accessors.size());
  model.accessors.push_back(std::move(indices));

  tinygltf::Material material;
  material.name = layer.label;
  material.pbrMetallicRoughness.baseColorFactor.assign(layer.color.begin(), layer.color.end());
  material.alphaMode = "BLEND";
  material.doubleSided = true;
  material.extensions.emplace("KHR_materials_unlit", tinygltf::Value(tinygltf::Value::Object{}));
  const int materialId = static_cast<int>(model.materials.size());
  model.materials.push_back(std::move(material));

  tinygltf::Primitive primitive;
  primitive.mode = TINYGLTF_MODE_TRIANGLES;
  primitive.attributes.emplace("POSITION", positionId);
  primitive.indices = indexId;
  primitive.material = materialId;
  tinygltf::Mesh mesh;
  mesh.name = layer.label;
  mesh.primitives.push_back(std::move(primitive));
  tinygltf::Node node;
  node.name = layer.label;
  node.mesh = static_cast<int>(model.meshes.size());
  model.meshes.push_back(std::move(mesh));
  model.scenes.front().nodes.push_back(static_cast<int>(model.nodes.size()));
  model.nodes.push_back(std::move(node));
}

}  // namespace

void writeBimCollisionOverlay(const std::filesystem::path& destination,
                              const BimCollisionWorld& world, const RoboticsFrame& frame,
                              double maxSlopeDegrees) {
  if (!frame.roboticsFromRenderer)
    throw std::invalid_argument("A BIM collision overlay requires a known source-to-metre scale");
  if (destination.extension() != ".glb")
    throw std::invalid_argument("A BIM collision overlay must use the .glb extension");
  const auto rendererFromRobotics = glm::inverse(*frame.roboticsFromRenderer);
  Layer obstacles{"Static collision mesh", {0.20, 0.39, 0.91, 0.38}};
  Layer doors{"Closed doors", {0.86, 0.19, 0.16, 0.65}};
  Layer proxies{"Dynamic box proxies", {1.0, 0.55, 0.16, 0.58}};
  Layer surfaces{"Walkable support candidates", {0.14, 0.84, 0.48, 0.72}};
  for (const auto& collider : world.geometry().colliders) {
    switch (collider.kind) {
    case BimColliderKind::StaticMesh:
      addMesh(obstacles, rendererFromRobotics, collider);
      break;
    case BimColliderKind::DoorMesh:
      if (!world.isDoorOpen(collider.entityId))
        addMesh(doors, rendererFromRobotics, collider);
      break;
    case BimColliderKind::DynamicBox:
      addBox(proxies, rendererFromRobotics, collider);
      break;
    }
  }
  for (const auto& triangle : extractWalkableSurfaces(world.geometry(), maxSlopeDegrees)) {
    const auto first = surfaces.startPrimitive(3);
    for (const auto v : triangle.vertices)
      surfaces.append(toRenderer(rendererFromRobotics, v));
    surfaces.indices.insert(surfaces.indices.end(), {first, first + 1, first + 2});
  }

  tinygltf::Model model;
  model.asset.version = "2.0";
  model.asset.generator = "Container BIM collision debug overlay";
  model.extensionsUsed.push_back("KHR_materials_unlit");
  model.buffers.emplace_back();
  model.scenes.emplace_back();
  model.defaultScene = 0;
  for (const auto* layer : {&obstacles, &doors, &proxies, &surfaces})
    addLayer(model, *layer);
  if (model.meshes.empty())
    throw std::invalid_argument("The BIM collision scene has no geometry to display");
  geometry::gltf::writeExportModel(model, destination);
}

}  // namespace container::robotics
