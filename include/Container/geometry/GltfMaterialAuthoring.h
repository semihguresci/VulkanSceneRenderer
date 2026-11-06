#pragma once

#include <Container/geometry/GltfMaterialTextureAuthoring.h>
#include <tiny_gltf.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace container::geometry::gltf {

enum class GltfMaterialProperty : uint8_t {
  BaseColor, Metallic, Roughness, Emissive, AlphaCutoff, NormalScale, OcclusionStrength,
  EmissiveStrength, Ior, Dispersion, Transmission, Thickness, AttenuationDistance, AttenuationColor,
  Clearcoat, ClearcoatRoughness, ClearcoatNormalScale, SheenColor, SheenRoughness,
  Specular, SpecularColor, Iridescence, IridescenceIor, IridescenceThicknessMinimum,
  IridescenceThicknessMaximum, Anisotropy, AnisotropyRotation, DiffuseTransmission,
  DiffuseTransmissionColor, SpecularGlossinessDiffuse, SpecularGlossinessSpecular, Glossiness,
  Count
};
enum class GltfMaterialExtension : uint8_t {
  Unlit, EmissiveStrength, Ior, Dispersion, Transmission, Volume, Clearcoat, Sheen,
  Specular, Iridescence, Anisotropy, DiffuseTransmission, SpecularGlossiness, Count
};
enum class GltfMaterialAlphaMode : uint8_t { Opaque, Mask, Blend };

struct GltfMaterialPropertyInfo {
  GltfMaterialProperty property;
  std::string_view path;
  uint8_t components;
  std::optional<GltfMaterialExtension> extension;
};
[[nodiscard]] std::span<const GltfMaterialPropertyInfo> GltfMaterialPropertyRegistry() noexcept;
[[nodiscard]] std::string_view GltfMaterialExtensionName(GltfMaterialExtension extension);

// monostate resets to the schema default, removing an extension leaf. In
// particular attenuationDistance's infinite default is represented by omission,
// never a nonfinite JSON number.
using GltfMaterialNumericValue = std::variant<std::monostate, double, std::array<double, 3>, std::array<double, 4>>;
struct GltfMaterialNumericEdit { GltfMaterialProperty property; GltfMaterialNumericValue value; };
struct GltfMaterialExtensionEdit { GltfMaterialExtension extension; bool enabled; };
struct GltfMaterialPatch {
  std::vector<GltfMaterialNumericEdit> values;
  // Enabling creates only a missing empty object; numeric leaves keep schema
  // defaults. Explicit disabling removes that known object, retained in Before
  // for undo. An extension targeted on an edited existing ID cannot be
  // disabled. A duplicate's new ID receives no channels. Unknown extensions
  // cannot be named by this enum and are always retained.
  std::vector<GltfMaterialExtensionEdit> extensions;
  std::optional<GltfMaterialAlphaMode> alphaMode;
  std::optional<bool> doubleSided;
  std::optional<std::string> name;
  std::vector<GltfMaterialTextureEdit> textures;
};
struct GltfMaterialAuthoringLimits {
  uint64_t maxOutputBytes{16ull * 1024 * 1024};
  uint64_t maxMetadataValues{262144};
  uint32_t maxMetadataDepth{64};
  uint64_t maxAnimationChannels{1'000'000};
  uint64_t maxPatchOperations{128};
};
struct GltfMaterialAnimationBinding {
  size_t clip{}, channel{};
  // A texture-transform/unknown future property can still block extension
  // removal; property is empty when it is not in the numeric factor registry.
  std::optional<GltfMaterialProperty> property;
  std::string path;
  std::optional<uint32_t> component;
};
enum class GltfMaterialPlanKind : uint8_t { Edit, Duplicate, Append, MakeUnique };
// The selected material pair is passed separately. These spans never own or
// duplicate the model's resource/animation tables.
struct GltfMaterialAuthoringSourceView {
  size_t materialCount{}, textureCount{};
  std::span<const std::string> extensionsUsed;
  std::span<const tinygltf::Animation> animations;
};
[[nodiscard]] GltfMaterialAuthoringSourceView GltfMaterialSourceView(const tinygltf::Model&) noexcept;
// Bounded validation of one selected resident record, without a plan/receipt or
// model/resource-table copy. Missing declarations may be appended by the caller
// after publication, as for newly enabled material extensions.
void ValidateGltfMaterialRecord(
    const tinygltf::Material&, GltfMaterialAuthoringSourceView,
    const GltfMaterialAuthoringLimits& limits = {});
// Logical selected-record copy storage, including opaque/legacy metadata.
// Traverses without allocating; finite value/depth/byte limits apply before a
// caller copies a larger table. Does not validate resource identities.
[[nodiscard]] uint64_t GltfMaterialRecordStorageBytes(
    const tinygltf::Material&, const GltfMaterialAuthoringLimits& limits = {});
// Exact numeric bits, optional types, raw JSON caches and opaque metadata.
// Inputs must already have bounded resident metadata (as above). No allocation.
[[nodiscard]] bool GltfMaterialRecordsEqual(
    const tinygltf::Material&, const tinygltf::Material&) noexcept;
class GltfMaterialAuthoringReceipt;
struct GltfMaterialAuthoringAccess;
struct GltfMaterialAuthoringPlan {
  GltfMaterialPlanKind kind{};
  // Absent for procedural/implicit-default materialization. No runtime index
  // is inferred from this ID; SceneManager must append/retain its explicit map.
  std::optional<int32_t> sourceMaterial;
  int32_t destinationMaterial{};
  size_t originalMaterialCount{};
  tinygltf::Material authoredBefore, authoredAfter;
  tinygltf::Material evaluatedBefore, evaluatedAfter;
  std::vector<GltfMaterialProperty> touchedProperties;
  std::vector<GltfMaterialTextureSlot> touchedTextures;
  std::vector<GltfMaterialExtension> touchedExtensions;
  // Original source provenance; duplicates do not create destination bindings.
  std::vector<GltfMaterialAnimationBinding> animationBindings;
  // Append these to extensionsUsed if missing. Never erase global declarations
  // or change extensionsRequired: other materials/tracks may still use them.
  std::vector<std::string> extensionsToDeclare;
  bool changed{};
  uint64_t peakOwnedBytes{};
 private:
  // Copies remain valid receipts, but no caller can mint or modify the proof.
  // Every public field is checked bit-exactly before a consumer may use it.
  std::shared_ptr<const GltfMaterialAuthoringReceipt> receipt_;
  friend struct GltfMaterialAuthoringAccess;
};

struct GltfMaterialAuthoringContextWitness {
  size_t materialCount{}, authoredTextureCount{}, evaluatedTextureCount{};
  // Receipt-owned relevant declaration spans; valid while a plan copy lives.
  std::span<const std::string> authoredDeclarations, evaluatedDeclarations;
  size_t afterAuthoredTextureCount{}, afterEvaluatedTextureCount{};
  bool resourceTransition{};
  std::span<const std::string> afterAuthoredDeclarations, afterEvaluatedDeclarations;
};
// Checks every public plan field before exposing the immutable input witness.
[[nodiscard]] GltfMaterialAuthoringContextWitness GltfMaterialAuthoringContext(const GltfMaterialAuthoringPlan&);

void ValidateGltfMaterialAuthoringPlan(
    const GltfMaterialAuthoringPlan&, GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated, const GltfMaterialAuthoringLimits& limits = {});
// Publication precondition for a proved, possibly rebased EDIT. Compares the
// immediate before records, including raw metadata and floating-point bits.
void ValidateGltfMaterialEditBefore(
    const GltfMaterialAuthoringPlan&, const tinygltf::Material& currentAuthored,
    const tinygltf::Material& currentEvaluated, GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated, const GltfMaterialAuthoringLimits& limits = {});

// Allocation-free publication predicates for the same immutable plan/context
// proofs. Invalid, stale or publicly modified input returns false, including
// under allocation denial. Animation binding identities are compared in place;
// decoded curve/raw backing equality remains the animation player's witness.
[[nodiscard]] bool GltfMaterialAuthoringPlanMatches(
    const GltfMaterialAuthoringPlan&, GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated, const GltfMaterialAuthoringLimits& limits = {}) noexcept;
[[nodiscard]] bool GltfMaterialEditBeforeMatches(
    const GltfMaterialAuthoringPlan&, const tinygltf::Material& currentAuthored,
    const tinygltf::Material& currentEvaluated, GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated, const GltfMaterialAuthoringLimits& limits = {}) noexcept;

// Explicit resource-domain transition. Before and After counts/declarations are
// independently proved; ordinary material-plan validation never widens a
// resource domain to accept one of these receipts. The outer resource command
// additionally proves source cookies, stable prefixes and resource ownership.
[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialTextureEdit(
    GltfMaterialAuthoringSourceView beforeAuthored, GltfMaterialAuthoringSourceView beforeEvaluated,
    GltfMaterialAuthoringSourceView afterAuthored, GltfMaterialAuthoringSourceView afterEvaluated,
    int32_t sourceMaterial, const tinygltf::Material& authoredMaterial,
    const tinygltf::Material& evaluatedMaterial, const GltfMaterialPatch& patch,
    const GltfMaterialAuthoringLimits& limits = {});
void ValidateGltfMaterialEditTransition(
    const GltfMaterialAuthoringPlan&, const tinygltf::Material& currentAuthored,
    const tinygltf::Material& currentEvaluated,
    GltfMaterialAuthoringSourceView beforeAuthored, GltfMaterialAuthoringSourceView beforeEvaluated,
    GltfMaterialAuthoringSourceView afterAuthored, GltfMaterialAuthoringSourceView afterEvaluated,
    const GltfMaterialAuthoringLimits& limits = {});
[[nodiscard]] bool GltfMaterialEditTransitionMatches(
    const GltfMaterialAuthoringPlan&, const tinygltf::Material& currentAuthored,
    const tinygltf::Material& currentEvaluated,
    GltfMaterialAuthoringSourceView beforeAuthored, GltfMaterialAuthoringSourceView beforeEvaluated,
    GltfMaterialAuthoringSourceView afterAuthored, GltfMaterialAuthoringSourceView afterEvaluated,
    const GltfMaterialAuthoringLimits& limits = {}) noexcept;

// Compose direct EDIT previews into one immutable history command. The exact
// authored chain must join; evaluated values may advance with animation. Keeps
// the first Before and latest After records, including raw caches and optional
// leaf types. Intermediate extension presence changes retain whole-object
// ownership even if the final presence matches the first. No preview chain is
// retained. Rebases/inverses and append/duplicate plans cannot be coalesced.
// Stage this receipt before publishing `next`, then replace the pending command
// without allocation. Publish `next`, not this cumulative history plan.
[[nodiscard]] GltfMaterialAuthoringPlan CoalesceGltfMaterialEdit(
    const GltfMaterialAuthoringPlan& cumulative, const GltfMaterialAuthoringPlan& next,
    GltfMaterialAuthoringSourceView authored, GltfMaterialAuthoringSourceView evaluated,
    const GltfMaterialAuthoringLimits& limits = {});

enum class GltfMaterialEditSide : uint8_t { Before, After };
[[nodiscard]] GltfMaterialAuthoringPlan RebaseGltfMaterialTextureEdit(
    const GltfMaterialAuthoringPlan&, GltfMaterialEditSide,
    const tinygltf::Material& currentAuthored, const tinygltf::Material& currentEvaluated,
    GltfMaterialAuthoringSourceView beforeAuthored, GltfMaterialAuthoringSourceView beforeEvaluated,
    GltfMaterialAuthoringSourceView afterAuthored, GltfMaterialAuthoringSourceView afterEvaluated,
    const GltfMaterialAuthoringLimits& limits = {});
// EDIT only. The authored input must be exactly the opposite trusted side.
// Command-owned evaluated leaves come from the trusted requested side; all
// other CURRENT evaluated leaves survive. Original curves are never changed.
// Explicit extension enable/disable owns the whole extension object. The
// returned plan is newly proved and retains the original command's identity.
[[nodiscard]] GltfMaterialAuthoringPlan RebaseGltfMaterialEdit(
    const GltfMaterialAuthoringPlan&, GltfMaterialEditSide,
    const tinygltf::Material& currentAuthored, const tinygltf::Material& currentEvaluated,
    GltfMaterialAuthoringSourceView authored, GltfMaterialAuthoringSourceView evaluated,
    const GltfMaterialAuthoringLimits& limits = {});

// Inputs are validated resident source models with the same material identity
// table. Resource counts may differ (e.g. archive-only manual resource suffixes);
// each material is validated in its own texture index space. No model, buffer,
// image, sampler, animation channel or resource index is modified/copied into
// the result. Only the selected material records/metadata are owned here.
//
// A touched numeric value is assigned independently to BOTH snapshots. Every
// untouched evaluated value and original authored rest value remains separate.
// Original curves remain authoritative when sampled: this is an authored-rest
// edit, NOT an absolute animation override. UI must disclose this distinction;
// runtime override/export policy belongs to the later integration layer.
[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialEdit(
    const tinygltf::Model& authoredSource, const tinygltf::Model& evaluatedSource,
    int32_t sourceMaterial, const GltfMaterialPatch& patch,
    const GltfMaterialAuthoringLimits& limits = {});

// Append a new source definition sharing all original texture bindings and
// opaque metadata. Does not assign any primitive/variant or copy any animation
// channels. Incoming provenance still reports the original source identity.
[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialDuplicate(
    const tinygltf::Model& authoredSource, const tinygltf::Model& evaluatedSource,
    int32_t sourceMaterial, const GltfMaterialPatch& patch = {},
    const GltfMaterialAuthoringLimits& limits = {});

// Freeze the current evaluated appearance into BOTH new rests, retaining exact
// original A/E before witnesses and source provenance for stale-origin checks.
// Does not copy channels or assign a graph binding. Patches apply after freeze.
[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialMakeUnique(
    const tinygltf::Model& authoredSource, const tinygltf::Model& evaluatedSource,
    int32_t sourceMaterial, const GltfMaterialPatch& patch = {},
    const GltfMaterialAuthoringLimits& limits = {});

// Explicit materialization for procedural/runtime/default bindings with no
// source definition. Caller supplies both valid snapshots and the source
// texture identity space; zero decoding/upload/remapping is performed here.
[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialAppend(
    const tinygltf::Model& authoredSource, const tinygltf::Model& evaluatedSource,
    const tinygltf::Material& authoredMaterial, const tinygltf::Material& evaluatedMaterial,
    const GltfMaterialPatch& patch = {}, const GltfMaterialAuthoringLimits& limits = {});

[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialEdit(
    GltfMaterialAuthoringSourceView authoredSource, GltfMaterialAuthoringSourceView evaluatedSource,
    int32_t sourceMaterial, const tinygltf::Material& authoredMaterial,
    const tinygltf::Material& evaluatedMaterial, const GltfMaterialPatch& patch,
    const GltfMaterialAuthoringLimits& limits = {});
[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialDuplicate(
    GltfMaterialAuthoringSourceView authoredSource, GltfMaterialAuthoringSourceView evaluatedSource,
    int32_t sourceMaterial, const tinygltf::Material& authoredMaterial,
    const tinygltf::Material& evaluatedMaterial, const GltfMaterialPatch& patch = {},
    const GltfMaterialAuthoringLimits& limits = {});
[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialMakeUnique(
    GltfMaterialAuthoringSourceView authoredSource, GltfMaterialAuthoringSourceView evaluatedSource,
    int32_t sourceMaterial, const tinygltf::Material& authoredMaterial,
    const tinygltf::Material& evaluatedMaterial, const GltfMaterialPatch& patch = {},
    const GltfMaterialAuthoringLimits& limits = {});
[[nodiscard]] GltfMaterialAuthoringPlan PlanGltfMaterialAppend(
    GltfMaterialAuthoringSourceView authoredSource, GltfMaterialAuthoringSourceView evaluatedSource,
    const tinygltf::Material& authoredMaterial, const tinygltf::Material& evaluatedMaterial,
    const GltfMaterialPatch& patch = {}, const GltfMaterialAuthoringLimits& limits = {});

} // namespace container::geometry::gltf
