#pragma once

#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace container::geometry::gltf {

// Each call describes ONE source namespace. Numeric IDs in independent archive
// and evaluated sources have no implied relationship. Empty masks mean active;
// nonempty masks must cover the exact table and contain only zero/one values.
struct GltfTextureResourceMasks {
  std::span<const uint8_t> materials,textures,images,samplers;
};
enum class GltfTextureResourceReferencePolicy : uint8_t { RequireComplete,RecordIncomplete };
struct GltfTextureResourceReferenceLimits {
  uint64_t maxTableEntries{1'000'000},maxObjects{4'000'000},maxReferences{8'000'000};
  uint64_t maxMetadataValues{1'000'000},maxMetadataBytes{32ull*1024*1024};
  uint64_t maxTraversalSteps{32'000'000},maxOwnedBytes{64ull*1024*1024};
  uint32_t maxMetadataDepth{64},maxDeclarations{256},maxIncompleteReasons{4096};
  GltfTextureResourceReferencePolicy policy{GltfTextureResourceReferencePolicy::RequireComplete};
};
struct GltfTextureResourceReferenceStats {
  uint64_t objects{},references{},metadataValues{},metadataBytes{},traversalSteps{},ownedBytes{};
};
enum class GltfTextureResourceReferenceKind : uint8_t { Image,Texture,Sampler };
struct GltfTextureResourceReference {
  GltfTextureResourceReferenceKind kind{};
  uint32_t index{};
  bool operator==(const GltfTextureResourceReference&) const noexcept=default;
};
enum class GltfTextureResourceIncompleteKind : uint8_t {
  UnknownExtension,UnknownAnimationPointer,AnimatedResourceIndex
};
struct GltfTextureResourceIncompleteReason {
  GltfTextureResourceIncompleteKind kind{};
  std::string schema;
};
struct GltfTextureResourceReferenceAccess;
// Immutable sorted-unique dependency rows plus exact bounded reference/schema
// witnesses. Retired owners contribute no outgoing references. References from
// active owners to retired/missing targets reject. No Model, image, buffer or
// accessor payload is retained; raw JSON strings are bounded caches, while typed
// extension objects are authoritative. Extras are bounded annotations.
class GltfTextureResourceReferences {
 public:
  GltfTextureResourceReferences() noexcept=default;
  explicit operator bool() const noexcept;
  // RecordIncomplete preserves accepted opaque metadata and known references,
  // but an incomplete result MUST NOT authorize unused-resource retirement.
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::span<const GltfTextureResourceIncompleteReason> incompleteReasons() const noexcept;
  [[nodiscard]] size_t materialCount() const noexcept;
  [[nodiscard]] size_t textureCount() const noexcept;
  [[nodiscard]] size_t imageCount() const noexcept;
  [[nodiscard]] size_t samplerCount() const noexcept;
  [[nodiscard]] bool materialActive(uint32_t) const noexcept;
  [[nodiscard]] bool textureActive(uint32_t) const noexcept;
  [[nodiscard]] bool imageActive(uint32_t) const noexcept;
  [[nodiscard]] bool samplerActive(uint32_t) const noexcept;
  [[nodiscard]] std::span<const uint32_t> materialTextures(uint32_t) const noexcept;
  [[nodiscard]] std::span<const uint32_t> textureImages(uint32_t) const noexcept;
  [[nodiscard]] std::span<const uint32_t> textureSamplers(uint32_t) const noexcept;
  // Incoming source-level references, e.g. raw animation pointer targets.
  [[nodiscard]] std::span<const GltfTextureResourceReference> sourceReferences() const noexcept;
  [[nodiscard]] GltfTextureResourceReferenceStats stats() const noexcept;
 private:
  struct State;
  std::shared_ptr<const State> state_;
  friend struct GltfTextureResourceReferenceAccess;
};
[[nodiscard]] GltfTextureResourceReferences ExtractGltfTextureResourceReferences(
    const tinygltf::Model&,GltfTextureResourceMasks masks={},
    const GltfTextureResourceReferenceLimits& limits={});
// Rewalks the same bounded reference/schema metadata and compares exact stored
// witnesses, phases and dependencies. No allocation, callbacks or payload I/O.
// This is a source proof only: provider/private-cache/history authority and
// logical incarnation checks remain the scene owner's responsibility.
[[nodiscard]] bool GltfTextureResourceReferencesMatch(const tinygltf::Model&,
    GltfTextureResourceMasks,const GltfTextureResourceReferences&) noexcept;
// Shared supported non-resource pointer grammar. Owner bounds/component shapes
// are checked; source phase/reference and animation sample proofs stay external.
// Unsupported resource/index/opaque suffixes and malformed paths return false.
[[nodiscard]] bool IsSupportedGltfNonResourceAnimationPointer(const tinygltf::Model&,std::string_view) noexcept;

// Detached material endpoints and private export caches need the same RAW
// extraction, including shadowed legacy/core/extension bindings. This result
// owns only one selected record's dependency IDs and bounded metadata witness.
class GltfMaterialRawTextureReferences {
 public:
  GltfMaterialRawTextureReferences() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::span<const GltfTextureResourceIncompleteReason> incompleteReasons() const noexcept;
  [[nodiscard]] std::span<const uint32_t> textures() const noexcept;
  [[nodiscard]] GltfTextureResourceReferenceStats stats() const noexcept;
 private:
  struct State;
  std::shared_ptr<const State> state_;
  friend struct GltfTextureResourceReferenceAccess;
};
[[nodiscard]] GltfMaterialRawTextureReferences ExtractGltfMaterialRawTextureReferences(
    const tinygltf::Material&,size_t textureCount,std::span<const uint8_t> activeTextures={},
    const GltfTextureResourceReferenceLimits& limits={});
[[nodiscard]] bool GltfMaterialRawTextureReferencesMatch(const tinygltf::Material&,
    size_t textureCount,std::span<const uint8_t> activeTextures,
    const GltfMaterialRawTextureReferences&) noexcept;

class GltfTextureRawResourceReferences {
 public:
  GltfTextureRawResourceReferences() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::span<const GltfTextureResourceIncompleteReason> incompleteReasons() const noexcept;
  [[nodiscard]] std::span<const uint32_t> images() const noexcept;
  [[nodiscard]] std::span<const uint32_t> samplers() const noexcept;
  [[nodiscard]] GltfTextureResourceReferenceStats stats() const noexcept;
 private:
  struct State;
  std::shared_ptr<const State> state_;
  friend struct GltfTextureResourceReferenceAccess;
};
[[nodiscard]] GltfTextureRawResourceReferences ExtractGltfTextureRawResourceReferences(
    const tinygltf::Texture&,size_t imageCount,size_t samplerCount,
    std::span<const uint8_t> imageActive={},std::span<const uint8_t> samplerActive={},
    const GltfTextureResourceReferenceLimits& limits={});
[[nodiscard]] bool GltfTextureRawResourceReferencesMatch(const tinygltf::Texture&,
    size_t imageCount,size_t samplerCount,std::span<const uint8_t> imageActive,
    std::span<const uint8_t> samplerActive,const GltfTextureRawResourceReferences&) noexcept;

} // namespace container::geometry::gltf
