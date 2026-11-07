#pragma once

#include <Container/geometry/GltfMerge.h>

namespace container::geometry::gltf {

struct GltfSessionMergePlan {
  // One authoritative plan: model contains the complete authored archive.
  GltfMergePlan merge;
  // Shares the plan's index spaces, with the previous evaluated pose retained.
  tinygltf::Model evaluatedSource;
  uint64_t peakResidentBytes{}, metadataBytes{};
};

// Pure CPU preparation; no I/O, sampling, publication or input mutation.
// Old authoring identities must agree. The archive may append resource-only
// suffixes, including accessors, but may not replace old geometry or bindings.
// Archive resources must be resident and authoritative. Image pixels/encoding
// and URI spelling need not match the live representation: corresponding image
// slots are a caller-provided provenance invariant, checked through stable
// image metadata and texture/image/sampler bindings, without image decoding.
// Old TRS/weights and material/light/camera numeric values may be evaluated;
// other old metadata, references, geometry and clips retain their identities.
// Incoming objects still contain authored values. The omitted destination is
// the live selected default scene; explicit options remain authoritative.
// The payload budget covers three retained inputs and both detached outputs;
// metadata reserves both planner output estimates and the live overlay.
[[nodiscard]] GltfSessionMergePlan PlanGltfSessionSources(
    const tinygltf::Model& evaluatedSource,
    const tinygltf::Model& authoredArchive,
    const tinygltf::Model& incoming,
    const GltfMergeOptions& options = {}, const GltfMergeLimits& limits = {});

} // namespace container::geometry::gltf
