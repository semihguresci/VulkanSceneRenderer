#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <vector>

namespace container::geometry::gltf {

// Wire scalar IDs are independent of Draco's enum and the glTF component enum.
// Values are raw little-endian components, with no normalization/conversion.
enum class DracoScalarType : uint32_t {
  Int8 = 1, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64,
  Float32, Float64, Bool
};

struct DracoAttributeRequest {
  uint32_t uniqueId{};
  uint32_t componentCount{};
};

struct DracoDecodeLimits {
  uint64_t maxInputBytes{64ull * 1024 * 1024};
  // Complete response, including indices, attribute headers and raw values.
  uint64_t maxOutputBytes{256ull * 1024 * 1024};
  uint64_t maxPoints{8ull * 1024 * 1024};
  uint64_t maxFaces{8ull * 1024 * 1024};
  uint64_t processMemoryBytes{512ull * 1024 * 1024};
  uint32_t timeoutMs{30'000};
};

struct DracoWorkerOptions {
  // Empty discovers ContainerDracoWorker.exe beside the running application.
  // Explicit overrides must be absolute paths. No PATH/shell lookup is used.
  std::filesystem::path workerExecutable;
  // Empty uses the system temporary directory. Each call creates/owns a fresh
  // child directory; cleanup never traverses outside that exact directory.
  std::filesystem::path tempRoot;
};

struct DracoDecodedAttribute {
  uint32_t uniqueId{};
  DracoScalarType scalarType{};
  uint32_t componentCount{};
  bool normalized{};
  // Densely mapped in Draco PointIndex order: pointCount * components * width.
  // The worker resolves mapped_index() for every point, preserving face indices
  // and exact signed/unsigned integer values without a floating-point roundtrip.
  std::vector<std::byte> values;
};

struct DracoDecodedMesh {
  uint32_t pointCount{};
  std::vector<uint32_t> faceIndices;
  std::vector<DracoDecodedAttribute> attributes;
};

[[nodiscard]] uint32_t DracoScalarByteWidth(DracoScalarType type);
[[nodiscard]] bool DracoWorkerPlatformSupported() noexcept;
[[nodiscard]] bool DracoWorkerExecutableAvailable(const std::filesystem::path& workerExecutable = {}) noexcept;

// No decoder is linked into this client. On Windows the child is suspended
// until a Job Object enforces its process-memory/CPU limits and process count.
// The caller's model is untouched on launch/decode/budget/protocol/timeout errors.
// Other platforms fail explicitly until an equivalent bounded launcher exists.
[[nodiscard]] DracoDecodedMesh DecodeDracoInWorker(
    std::span<const std::byte> compressed,
    std::span<const DracoAttributeRequest> attributes,
    const DracoDecodeLimits& limits = {},
    const DracoWorkerOptions& options = {},
    const std::stop_token& stop = {});

} // namespace container::geometry::gltf
