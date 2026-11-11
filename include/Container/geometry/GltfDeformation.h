#pragma once

#include <Container/geometry/Mesh.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace container::geometry::gltf {

struct JointInfluence {
  // Index into the skin's joints array, not an index into the node array.
  uint32_t joint{0};
  float weight{0.0f};
};

struct VertexSkinInfluences {
  // All JOINTS_n / WEIGHTS_n sets, in set/component order. Weights are
  // normalized across sets, including normalized integer source accessors.
  std::vector<JointInfluence> values;
};

struct MorphTarget {
  // An omitted attribute is represented by an empty vector (zero deltas).
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> normals;
  std::vector<glm::vec3> tangents;
};

struct DeformationPrimitive {
  size_t sourceMesh{0};
  size_t sourcePrimitive{0};
  std::vector<Vertex> bindVertices;
  std::vector<VertexSkinInfluences> influences;
  std::vector<MorphTarget> morphTargets;
  std::vector<float> meshDefaultWeights;
  // Source provenance is independent of the sanitized bind frame generated
  // by the static loader. Morphing must regenerate missing base frames.
  bool authoredBaseNormal{true};
  bool authoredBaseTangent{true};
  uint32_t normalTextureTexCoord{0};

  [[nodiscard]] bool hasSkinning() const { return !influences.empty(); }
  [[nodiscard]] bool hasMorphing() const { return !morphTargets.empty(); }
};

struct DeformationSkin {
  std::vector<uint32_t> joints;
  std::vector<glm::mat4> inverseBindMatrices;
  int skeleton{-1};
};

struct DeformationNode {
  int sourceMesh{-1};
  int skin{-1};
  // Node weights override mesh weights. Omitted weights resolve to zero.
  std::vector<float> defaultMorphWeights;
};

struct DeformationData {
  // Same mesh-major, primitive-major order as Model::meshes(). Vertex order
  // remains the source accessor order, even when triangle winding is repaired.
  std::vector<DeformationPrimitive> primitives;
  // Both arrays retain the source glTF index space.
  std::vector<DeformationSkin> skins;
  std::vector<DeformationNode> nodes;
};

// Runtime meshes supply the loader's sanitized normals/tangents and untouched
// auxiliary vertex attributes. Their count and source vertex counts must match.
// Invalid accessors, skin references and non-finite values throw runtime_error.
DeformationData LoadDeformationData(
    const tinygltf::Model& source,
    std::span<const Mesh> runtimePrimitives,
    const std::stop_token& stop = {});

// Palette maps bind mesh coordinates into the owning mesh node's local space:
// inverse(meshNodeWorld) * jointWorld * inverseBind. Applying meshNodeWorld
// afterward cancels the mesh node transform, as required for glTF skinning.
std::vector<glm::mat4> ComputeSkinPalette(
    const DeformationSkin& skin, const glm::mat4& meshNodeWorld,
    std::span<const glm::mat4> nodeWorldMatrices);

// Numerical CPU reference: add morph deltas first, then linear-blend skinning.
// Empty morphWeights uses the mesh defaults; pass the node's resolved weights
// for node overrides or evaluated animation weights. An empty palette leaves a
// primitive unskinned, including a mesh instantiated by a node without a skin.
// Positions remain node-local. Normals use inverse-transpose of the blended
// linear transform, tangents use its linear transform then Gram-Schmidt, and
// tangent.w includes the sign of its determinant. The later node transform
// must account for its own determinant as in the ordinary rendering path.
// Singular blends use the same finite frame fallback as the GPU reference.
// This evaluator consumes existing bind frames/deltas. For missing source
// NORMAL/TANGENT morph frames, use Prepare/EvaluateDeformationFrames instead.
std::vector<Vertex> EvaluatePrimitive(
    const DeformationPrimitive& primitive,
    std::span<const float> morphWeights = {},
    std::span<const glm::mat4> skinPalette = {});

}  // namespace container::geometry::gltf
