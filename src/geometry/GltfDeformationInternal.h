#pragma once

#include <Container/geometry/GltfDeformation.h>

namespace container::geometry::gltf::detail {

// Apply the ordinary skinning and frame normalization to an already morphed
// vertex stream without copying the generated frames or the skin influences.
void FinalizeDeformedFrames(std::span<Vertex> vertices,
                            std::span<const VertexSkinInfluences> influences,
                            std::span<const glm::mat4> skinPalette);

}  // namespace container::geometry::gltf::detail
