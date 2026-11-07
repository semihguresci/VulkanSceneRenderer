#pragma once

#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry::gltf {

inline constexpr std::string_view kGltfOpacityMicromapExtension = "EXT_mesh_opacity_micromap";

struct GltfOpacityMicromapTriangle {
  uint32_t dataOffset{};
  uint16_t subdivisionLevel{}, format{};
};
static_assert(sizeof(GltfOpacityMicromapTriangle) == 8);

struct GltfOpacityMicromapUsage {
  uint32_t count{}, subdivisionLevel{}, format{};
  friend bool operator==(const GltfOpacityMicromapUsage&, const GltfOpacityMicromapUsage&) = default;
};

struct GltfOpacityMicromap {
  std::vector<std::byte> data;
  std::vector<GltfOpacityMicromapTriangle> triangles;
  std::vector<GltfOpacityMicromapUsage> usage;
};

struct GltfOpacityMicromapAttachment {
  std::shared_ptr<const GltfOpacityMicromap> map;
  // One absolute lookup value per source triangle. UINT32_MAX through
  // UINT32_MAX-3 preserve the four special index encodings.
  std::vector<uint32_t> indices;
  // Counts the referenced triangle records, including repetitions, after
  // indirection. Special indices have no record and contribute no usage.
  std::vector<GltfOpacityMicromapUsage> usage;
};

struct GltfOpacityMicromapDiagnostic {
  std::string context;
  std::string message;
};

struct GltfOpacityMicromapLimits {
  size_t maxMaps{4096};
  size_t maxPrimitiveSlots{262144};
  size_t maxTriangleRecords{2'000'000};
  size_t maxGeometryTriangles{4'000'000};
  // Includes decoded payload, lookup tables and range-validation workspace.
  size_t maxPayloadBytes{128ull * 1024 * 1024};
  size_t maxDiagnostics{128};
  uint32_t maxSubdivisionLevel{12};
};

struct GltfOpacityMicromapAssets {
  // Indexed by source mesh and primitive. Null entries use sampled alpha.
  // Unreferenced meshes may have an empty inner table.
  std::vector<std::vector<std::shared_ptr<const GltfOpacityMicromapAttachment>>> primitives;
  std::vector<GltfOpacityMicromapDiagnostic> diagnostics;
  size_t diagnosticsOmitted{};

  [[nodiscard]] std::shared_ptr<const GltfOpacityMicromapAttachment> find(
      size_t mesh, size_t primitive) const noexcept;
};

// Detached CPU parsing of the asset layout used by vk_gltf_renderer at
// 174625d5c621. Invalid map/link records stay unavailable without rejecting
// the source scene. No Vulkan device, material or geometry policy is involved.
[[nodiscard]] GltfOpacityMicromapAssets ParseGltfOpacityMicromaps(
    const tinygltf::Model& source, const GltfOpacityMicromapLimits& limits = {});

// TinyGLTF narrows extension integers and drops certain array entries.
// Run this check on the original manifest before using detached attachments.
// A diagnostic means the source must use sampled alpha for all primitives.
[[nodiscard]] std::optional<GltfOpacityMicromapDiagnostic> ValidateGltfOpacityMicromapSourceJson(
    const nlohmann::json& source, const GltfOpacityMicromapLimits& limits = {});

} // namespace container::geometry::gltf
