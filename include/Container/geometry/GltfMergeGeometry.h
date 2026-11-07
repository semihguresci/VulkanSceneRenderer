#pragma once

#include <Container/geometry/GltfMerge.h>
#include <Container/geometry/Model.h>

#include <cstdint>
#include <span>

namespace container::geometry::gltf {

struct GltfMergeGeometryInput {
  const Model& runtime;
  std::span<const uint32_t> sourcePrimitiveToRuntime;
  std::span<const int32_t> runtimePrimitiveToSource;
};

struct GltfMergeGeometryLimits {
  uint64_t maxPrimitives{1'000'000};
  uint64_t maxVertices{UINT32_MAX};
  uint64_t maxIndices{UINT32_MAX};
  // Logical output storage includes mesh payloads AND flattened GPU streams.
  // The caller separately accounts for existing inputs and other session data.
  uint64_t maxOutputBytes{512ull * 1024 * 1024};
};

struct GltfMergeGeometry {
  Model runtime;
  std::vector<uint32_t> sourcePrimitiveToRuntime;
  std::vector<int32_t> runtimePrimitiveToSource;
  uint64_t outputBytes{};
};

// Preserve every old runtime mesh/index, including procedural gaps and edited
// bind frames. Append each incoming source primitive once, irrespective of
// canonical/placed owner count. Incoming nonnegative material IDs become GLOBAL
// source IDs through the merge plan; -1 remains implicit. Procedural materials
// already use runtime IDs and are never translated. Inputs must be loader or
// procedural triangle meshes; all counts, indices, attributes and used maps are
// checked before geometry copies. No transforms bake, publication, I/O or GPU.
[[nodiscard]] GltfMergeGeometry ComposeGltfMergeGeometry(
    const GltfMergeGeometryInput& base, const Model& incoming,
    const GltfMergePlan& plan, const GltfMergeGeometryLimits& limits = {});

} // namespace container::geometry::gltf
