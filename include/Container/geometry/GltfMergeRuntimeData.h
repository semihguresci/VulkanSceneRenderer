#pragma once

#include <Container/geometry/GltfDeformation.h>
#include <Container/geometry/GltfInstancing.h>
#include <Container/geometry/GltfMerge.h>

#include <cstdint>
#include <optional>
#include <span>

namespace container::geometry::gltf {

struct GltfMergeRuntimeDataInput {
  const tinygltf::Model& source;
  // Retained authored defaults and current variant-generated bind frames.
  // Never reconstruct these from an evaluated source pose before composition.
  const DeformationData* deformation{};
  const GltfInstancingData* instancing{};
  // Source mesh-major/primitive-major order, with procedural runtime gaps
  // removed. Required only for a static side without retained deformation
  // when the combined result contains skinning or morphing.
  std::span<const Mesh> sourceBindPrimitives{};
  // Optional current variant/frame UV policy in the same source-flat order.
  // Only UV0/UV1 are supported. Static synthesized definitions use this table;
  // otherwise they resolve the default source normal texture plus transform
  // override. If retained deformation is supplied, this must agree with it.
  std::span<const uint32_t> normalTextureTexCoords{};
};

struct GltfMergeRuntimeDataLimits {
  uint64_t maxNodes{1'000'000};
  uint64_t maxPrimitives{1'000'000};
  uint64_t maxSkins{1'000'000};
  uint64_t maxInstances{1'000'000};
  uint64_t maxExpandedPrimitives{4'000'000};
  // Logical owned output storage, including nested vectors and strings. The
  // caller separately budgets its already-resident inputs and merge model.
  uint64_t maxOutputBytes{256ull * 1024 * 1024};
  // Conservative temporary decoder/validation storage, in addition to output.
  uint64_t maxWorkspaceBytes{128ull * 1024 * 1024};
};

struct GltfMergeRuntimeData {
  // Null when neither source contains skins/morphs. Otherwise all primitives
  // remain in merged SOURCE order, once each regardless of placement count.
  std::optional<DeformationData> deformation;
  GltfInstancingData instancing;
  uint64_t outputBytes{};
  uint64_t workspaceBytes{};
};

// Detached CPU composition for a successful PlanGltfMerge. Checks used maps,
// typed attachments, retained layouts, finite values and aggregate output
// limits before copying payloads. Keeps old retained defaults/frames exactly,
// appends incoming primitive definitions once, and remaps canonical/placed
// skin joints, node attachments and instance accessor identities. Context-only
// clones have no geometry, deformation weights or instancing. Local instance
// matrix values are unchanged (the existing NodeInstancing type owns a vector
// per node); no vertex/index geometry is copied per placement. plan.model is authoritative
// for final mesh/skin attachments. Resources may have archive-only suffixes:
// imported accessor IDs come from explicit maps, never inferred offsets.
// Static sides without retained data reuse the existing deformation decoder to
// preserve dormant JOINTS_n/WEIGHTS_n, including integer IDs and normalization.
// All output/workspace bounds are checked first; malformed influence semantics
// may fail during bounded temporary decoder construction. Its primitive payloads
// move into output, without another geometry copy. Retained data is never
// reparsed. No source writes, renderer publication, playback or I/O.
[[nodiscard]] GltfMergeRuntimeData ComposeGltfMergeRuntimeData(
    const GltfMergeRuntimeDataInput& base,
    const GltfMergeRuntimeDataInput& incoming,
    const GltfMergePlan& plan,
    const GltfMergeRuntimeDataLimits& limits = {});

} // namespace container::geometry::gltf
