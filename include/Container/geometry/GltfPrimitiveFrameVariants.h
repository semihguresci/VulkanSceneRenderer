#pragma once

#include <Container/geometry/GltfDeformationFrames.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace container::geometry::gltf {

// Capture once when the canonical primitive is imported/created, before any
// material assignment or source material edit. In particular bindNormalTexCoord
// must never be reconstructed from a subsequently edited material. A new bind
// geometry generation needs a new bindRevision, even if its IDs are unchanged.
class CanonicalPrimitiveFrameMetadata {
 public:
  CanonicalPrimitiveFrameMetadata(uint32_t runtimePrimitive,
      std::optional<uint32_t> sourceFlatPrimitive, bool authoredNormal,
      bool authoredTangent, uint32_t bindNormalTexCoord,
      uint8_t availableTexCoordMask, uint64_t bindRevision);

  [[nodiscard]] uint32_t runtimePrimitive() const noexcept { return runtimePrimitive_; }
  [[nodiscard]] std::optional<uint32_t> sourceFlatPrimitive() const noexcept { return sourceFlatPrimitive_; }
  [[nodiscard]] bool authoredNormal() const noexcept { return authoredNormal_; }
  [[nodiscard]] bool authoredTangent() const noexcept { return authoredTangent_; }
  [[nodiscard]] uint32_t bindNormalTexCoord() const noexcept { return bindNormalTexCoord_; }
  // Bit 0/1 records actual TEXCOORD_0/1 presence, not loader-filled zero values.
  [[nodiscard]] uint8_t availableTexCoordMask() const noexcept { return availableTexCoordMask_; }
  [[nodiscard]] uint64_t bindRevision() const noexcept { return bindRevision_; }

 private:
  uint32_t runtimePrimitive_;
  std::optional<uint32_t> sourceFlatPrimitive_;
  bool authoredNormal_, authoredTangent_;
  uint32_t bindNormalTexCoord_;
  uint8_t availableTexCoordMask_;
  uint64_t bindRevision_;
};

enum class PrimitiveFrameVariantKind : uint8_t { Canonical, GeneratedUV0, GeneratedUV1 };

struct PrimitiveFrameVariantKey {
  uint32_t runtimePrimitive{};
  uint64_t bindRevision{};
  PrimitiveFrameVariantKind kind{PrimitiveFrameVariantKind::Canonical};
  bool operator==(const PrimitiveFrameVariantKey&) const = default;
};

struct PrimitiveFrameBindingRequest {
  // Actual graph binding identity, never a source mesh owner or material ID.
  uint32_t graphNode{};
  // Effective normal texture selector, including KHR_texture_transform's
  // override. No normal map means nullopt: preserve the canonical basis and
  // do not require texture coordinates on an untextured primitive.
  std::optional<uint32_t> normalTexCoord;
};

struct PrimitiveFrameExportPolicy {
  bool retainBaseNormal{}, retainBaseTangent{};
  bool retainTargetNormals{}, retainTargetTangents{};
  bool operator==(const PrimitiveFrameExportPolicy&) const = default;
};

struct PrimitiveFrameVariantSelection {
  uint32_t graphNode{};
  PrimitiveFrameVariantKey key;
  // Empty means reuse existing canonical geometry/deformation behavior. This
  // includes the baseline missing-NORMAL/TANGENT fallback, with no new stream.
  std::optional<uint32_t> templateIndex;
  PrimitiveFrameExportPolicy exportPolicy;
};

struct PrimitiveFrameVariantLimits {
  uint64_t maxInputVertices{16'000'000};
  uint64_t maxTriangleCorners{16'000'000};
  uint64_t maxBindings{1'000'000};
  uint64_t maxMorphTargets{1024};
  uint64_t maxInfluences{64'000'000};
  uint64_t maxInputBytes{256ull * 1024 * 1024};
  // Conservative logical owned payload, including capacity growth allowance
  // for the existing preparer's nested vectors; excludes allocator overhead.
  uint64_t maxOutputBytes{256ull * 1024 * 1024};
  // Sorting request IDs plus the preparer's temporary EvaluatePrimitive vertex
  // copy. Does NOT cover later EvaluateDeformationFrames/MikkTSpace scratch.
  uint64_t maxWorkspaceBytes{128ull * 1024 * 1024};
};

class PrimitiveFrameVariantPlan;

class PrimitiveFrameVariantTemplate {
 public:
  [[nodiscard]] const PrimitiveFrameVariantKey& key() const noexcept { return key_; }
  [[nodiscard]] const PreparedDeformationFrames& frames() const noexcept { return frames_; }

 private:
  PrimitiveFrameVariantKey key_;
  PreparedDeformationFrames frames_;
  friend class PrimitiveFrameVariantPlan;
  friend PrimitiveFrameVariantPlan PreparePrimitiveFrameVariants(
      const CanonicalPrimitiveFrameMetadata&, const DeformationPrimitive&,
      std::span<const uint32_t>, std::span<const PrimitiveFrameBindingRequest>,
      const PrimitiveFrameVariantLimits&);
};

class PrimitiveFrameVariantPlan {
 public:
  [[nodiscard]] const CanonicalPrimitiveFrameMetadata& canonical() const noexcept { return canonical_; }
  [[nodiscard]] std::span<const PrimitiveFrameVariantSelection> selections() const noexcept { return selections_; }
  // At most two generated UV templates per canonical primitive. Material,
  // transform, source owner and instance identity never enter the cache key.
  [[nodiscard]] std::span<const PrimitiveFrameVariantTemplate> templates() const noexcept { return templates_; }
  [[nodiscard]] uint64_t outputBytes() const noexcept { return outputBytes_; }
  [[nodiscard]] uint64_t workspaceBytes() const noexcept { return workspaceBytes_; }

 private:
  explicit PrimitiveFrameVariantPlan(CanonicalPrimitiveFrameMetadata metadata) : canonical_(metadata) {}
  CanonicalPrimitiveFrameMetadata canonical_;
  std::vector<PrimitiveFrameVariantSelection> selections_;
  std::vector<PrimitiveFrameVariantTemplate> templates_;
  uint64_t outputBytes_{}, workspaceBytes_{};
  friend PrimitiveFrameVariantPlan PreparePrimitiveFrameVariants(
      const CanonicalPrimitiveFrameMetadata&, const DeformationPrimitive&,
      std::span<const uint32_t>, std::span<const PrimitiveFrameBindingRequest>,
      const PrimitiveFrameVariantLimits&);
};

// Pure preparation for ONE immutable canonical primitive, without copying a
// Model, decoding accessors/images, changing graph/primitive IDs or evaluating
// MikkTSpace. Same-UV/no-map bindings alias canonical behavior. A UV replacement
// regenerates tangents even when the source provided valid authored tangents.
// Complete influences, source corner maps, POSITION/NORMAL morphs and defaults
// remain aligned. Missing base NORMAL ignores base/target tangents and target
// normals, following PrepareDeformationFrames. All count/output/workspace caps
// are checked before output payload copies; exceptions leave every input intact.
//
// Static/posed consumers can EvaluateDeformationFrames(template.frames()), but
// must separately bound actual evaluator/Mikk work before publication. This
// helper makes no linear scratch claim for Mikk's high-valence subgroup work.
//
// Export MUST serialize the original canonical vertex/index topology, NOT the
// corner-expanded template. Apply exportPolicy to original base/target attrs:
// generated replacements omit base+target TANGENT, while other source attrs,
// skin, POSITION/NORMAL morphs, animation and resource metadata stay canonical.
// Cache exported geometry by canonical primitive AND explicit frame key.
[[nodiscard]] PrimitiveFrameVariantPlan PreparePrimitiveFrameVariants(
    const CanonicalPrimitiveFrameMetadata& metadata,
    const DeformationPrimitive& canonicalPrimitive,
    std::span<const uint32_t> localTriangleIndices,
    std::span<const PrimitiveFrameBindingRequest> bindings,
    const PrimitiveFrameVariantLimits& limits = {});

} // namespace container::geometry::gltf
