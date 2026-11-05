#pragma once

#include <Container/geometry/GltfDeformation.h>

#include <cstdint>
#include <span>
#include <vector>

namespace container::geometry::gltf {

// Authored attribute presence must come from the source primitive, before the
// ordinary loader fills missing normals/tangents. UV selection includes any
// normal texture texCoord override and currently supports TEXCOORD_0/1.
struct DeformationFramePolicy {
  bool authoredBaseNormal{true};
  bool authoredBaseTangent{true};
  uint32_t normalTextureTexCoord{0};
};

struct PreparedDeformationFrames {
  DeformationPrimitive primitive;
  // One independent vertex per triangle corner, in input index order. Separate
  // corners preserve flat normals and MikkTSpace orientation/texture seams.
  std::vector<uint32_t> indices;
  std::vector<uint32_t> sourceVertexMap;
  DeformationFramePolicy policy;
};

// Builds an independent corner-expanded primitive. All bind attributes, morph
// deltas and complete influence sets follow sourceVertexMap. With no authored
// normal, supplied tangent attributes/deltas are ignored, as required by glTF.
// The source primitive and local triangle indices are never changed.
[[nodiscard]] PreparedDeformationFrames PrepareDeformationFrames(
    const DeformationPrimitive& primitive,
    std::span<const uint32_t> localTriangleIndices,
    DeformationFramePolicy policy);

// Exceptional CPU fallback: morph first, generate missing flat normals and/or
// MikkTSpace tangents from the resulting unskinned pose, then skin through the
// ordinary evaluator. Authored normals with omitted target deltas are retained.
// Complete authored frames use the ordinary evaluator unchanged. The result is
// in prepared corner order; repeated evaluations never mutate prepared state.
[[nodiscard]] std::vector<Vertex> EvaluateDeformationFrames(
    const PreparedDeformationFrames& prepared,
    std::span<const float> morphWeights = {},
    std::span<const glm::mat4> skinPalette = {});

}  // namespace container::geometry::gltf
