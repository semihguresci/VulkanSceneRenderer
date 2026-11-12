#pragma once

#include <stop_token>

#include <glm/mat4x4.hpp>
#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <cstdint>
#include <string>
#include <vector>

namespace container::geometry::gltf {

struct InstancingLimits {
  uint64_t maxInstances{1'000'000};
  uint64_t maxExpandedPrimitives{4'000'000};
  uint64_t maxDecodedBytes{128ull * 1024 * 1024};
};
struct InstancingSummary {
  uint64_t instancedNodes{}, instances{}, expandedPrimitives{}, decodedBytes{};
};
struct InstanceAttributeReference {
  std::string semantic;
  int sourceAccessor{-1};
};
struct SourceInstanceIdentity {
  uint32_t sourceNode{}, sourceMesh{}, sourcePrimitive{}, sourceInstance{};
  friend bool operator==(const SourceInstanceIdentity&, const SourceInstanceIdentity&) = default;
};
struct NodeInstancing {
  uint32_t sourceNode{}, sourceMesh{}, primitiveCount{};
  // One matrix per source instance, shared by every primitive of its mesh.
  // No vertex/index/BLAS data is copied by this CPU representation.
  std::vector<glm::mat4> localMatrices;
  // Application attributes stay source references, including exact integer IDs.
  std::vector<InstanceAttributeReference> attributes;
  [[nodiscard]] SourceInstanceIdentity identity(uint32_t instance, uint32_t primitive) const;
};
struct GltfInstancingData {
  // Sorted in source node order. Only nodes carrying the extension occur here.
  std::vector<NodeInstancing> nodes;
  InstancingSummary summary;
};

// Loader metadata gate: validates indices before
// TinyGLTF narrowing, schemas/counts and finite expansion/output budgets. It
// does not read buffers or decode values. Runtime publication also validates
// the active hierarchy, render-object budget and Vulkan transform limits.
[[nodiscard]] InstancingSummary ValidateMeshGpuInstancingJson(
    const nlohmann::json& source, const InstancingLimits& limits = {},
    const std::stop_token& stop = {});

// Same gate for the detached loader document. Canonicalizes integer-valued
// decimal/exponent encodings in instance references and their accessor storage.
[[nodiscard]] InstancingSummary NormalizeMeshGpuInstancingJson(
    nlohmann::json& source, bool& rewritten, const InstancingLimits& limits = {},
    const std::stop_token& stop = {});

// Immutable checked-accessor CPU parser, independent of the renderer/scene graph.
// FLOAT TRS and normalized signed BYTE/SHORT rotation are supported. Missing
// TRS components use identity defaults; all attributes (including custom ones)
// share one positive count. Quaternions must be unit within storage precision,
// then are normalized before constructing T*R*S.
[[nodiscard]] GltfInstancingData ParseMeshGpuInstancing(
    const tinygltf::Model& source, const InstancingLimits& limits = {},
    const std::stop_token& stop = {});
[[nodiscard]] const NodeInstancing* FindNodeInstancing(
    const GltfInstancingData& data, uint32_t sourceNode) noexcept;

// Runtime callers supply the node's current parent-composed world matrix.
// The extension is applied after that node: nodeWorld * instanceLocal. Finite
// affine input/output is required; mirrored and zero scales remain valid.
[[nodiscard]] glm::mat4 ComposeInstanceWorld(
    const NodeInstancing& node, uint32_t instance, const glm::mat4& nodeWorld);

} // namespace container::geometry::gltf
