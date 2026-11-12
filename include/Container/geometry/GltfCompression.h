#pragma once

#include <stop_token>

#include <tiny_gltf.h>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace container::geometry::gltf {

struct CompressionLimits {
  // Conservative aggregate decoded/temporary storage reservation. Existing
  // encoded and destination buffers are charged separately by input preflight.
  uint64_t maxDecodedBytes{256ull * 1024 * 1024};
  uint64_t dracoProcessMemoryBytes{512ull * 1024 * 1024};
  uint32_t dracoTimeoutMs{30'000};
  std::filesystem::path dracoWorkerExecutable;
  std::filesystem::path dracoTemporaryRoot;
};

struct CompressionPlaceholder {
  size_t bufferIndex{};
  uint64_t byteLength{};
};

struct CompressionSourcePlan {
  // URI-less meshopt destination buffers, excluding GLB's actual BIN buffer0.
  // TinyGLTF can read a one-byte stand-in instead of allocating these logical
  // destination sizes. Pass this original plan to DecodeCompressedGltf so it
  // validates logical ranges before replacing every referencing view.
  std::vector<CompressionPlaceholder> placeholders;
  uint64_t decodedBytes{};
  size_t meshoptViews{};
  size_t dracoPrimitives{};
  bool dracoAvailable{};
};

struct CompressionStatistics {
  uint64_t decodedBytes{};
  size_t meshoptViews{};
  size_t dracoPrimitives{};
  size_t fallbackViews{};
  size_t fallbackPrimitives{};
};

[[nodiscard]] bool HasMeshoptDecoder() noexcept;
[[nodiscard]] bool HasDracoDecoder() noexcept;
[[nodiscard]] bool HasAvailableDracoDecoder(const std::filesystem::path& workerExecutable = {}) noexcept;
[[nodiscard]] bool IsGeometryCompressionExtension(std::string_view name) noexcept;

// Runs before TinyGLTF allocates declared buffers. Checks compression metadata,
// declared encoded/output ranges, placeholder references and aggregate output
// sizes. Disabled optional backends select ordinary fallback; required ones
// fail clearly. No resource reads or allocations proportional to decoded data.
[[nodiscard]] CompressionSourcePlan InspectCompressedGltfSource(
    const nlohmann::json& source, bool binary,
    const CompressionLimits& limits = {}, const std::stop_token& stop = {});

// Ownership-taking normalization. Passing an lvalue copies it; callers can
// move a freshly loaded model to avoid an extra encoded-resource copy. Original
// buffer bytes, accessor numeric metadata and sparse data remain intact. Draco
// preserves decoded point order; an encoder may have reordered/deduplicated its
// input. Ordinary attributes/morph data must already use that decoded order.
// Only this detached model's references/consumed extension declarations
// change. Decoded bytes live in appended buffers; all ordinary checked accessor,
// animation, deformation and portable-export paths can then consume the result.
// Statistics are published only on complete success. Failure never edits an
// lvalue input or a live scene. TinyGLTF's eager Draco decoder must stay disabled.
// File callers pass the preflight Draco decision to preserve its output budget.
[[nodiscard]] tinygltf::Model DecodeCompressedGltf(
    tinygltf::Model source, const CompressionLimits& limits = {},
    CompressionStatistics* statistics = nullptr,
    std::span<const CompressionPlaceholder> placeholders = {},
    const std::stop_token& stop = {},
    std::optional<bool> preflightDracoAvailability = std::nullopt);

} // namespace container::geometry::gltf
