#pragma once

#include <tiny_gltf.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace container::geometry::gltf {

// Compatibility fields already consumed by this renderer. They are emitted at
// their original source location; no standard PBR factor is overridden.
[[nodiscard]] std::span<const std::string_view> KnownLegacyMaterialTextureFields() noexcept;
[[nodiscard]] std::span<const std::string_view> KnownLegacyMaterialFactorFields() noexcept;
struct GltfLegacyMaterialSerializationLimits {
  uint64_t maxMaterials{1'000'000};
  uint64_t maxParameterValues{1'000'000};
  uint64_t maxParameterBytes{64ull*1024*1024};
  uint64_t maxJsonBytes{256ull*1024*1024};
  uint64_t maxJsonValues{16'000'000};
  uint32_t maxJsonDepth{128};
  uint32_t maxGlbChunks{64};
  uint64_t maxOwnedBytes{768ull*1024*1024};
  uint64_t maxCameras{1'000'000};
};
// Preflight before the caller copies an export model. No records are copied.
// Returns false when the ordinary TinyGLTF serializer needs no compatibility
// projection. Limits count logical data/work, not private codec/parser scratch.
[[nodiscard]] bool ValidateLegacyMaterialSerializationInput(
    std::span<const tinygltf::Material>, size_t textureCount,
    const GltfLegacyMaterialSerializationLimits& = {});
// Combined shared-writer preflight. Perspective zfar==0 is the typed omitted
// far-plane sentinel and needs canonical JSON omission even without legacy
// material fields. Positive finite far values are never removed.
[[nodiscard]] bool ValidateSceneSerializationCompatibilityInput(
    const tinygltf::Model&,const GltfLegacyMaterialSerializationLimits& = {});
// The file MUST belong to the caller's unpublished staging directory. A bounded
// SAX rewrite injects only recognized legacy fields and omits the typed zero
// perspective far sentinel at its exact schema location. GLB's entire remaining
// chunk stream is copied byte-for-byte. Returns a separate closed staged file;
// the caller performs its existing atomic staged/destination replacements.
[[nodiscard]] std::optional<std::filesystem::path> StageLegacyMaterialSerialization(
    const tinygltf::Model&, const std::filesystem::path& stagedFile, bool binary,
    const GltfLegacyMaterialSerializationLimits& = {});

} // namespace container::geometry::gltf
