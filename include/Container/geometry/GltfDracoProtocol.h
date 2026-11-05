#pragma once

// Internal, versioned subprocess protocol. Neither this header nor its client
// requires Draco; the official decoder is linked only into the worker target.
#include <Container/geometry/GltfDracoWorker.h>

#include <string_view>

namespace container::geometry::gltf::draco_worker {

inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr uint32_t kMaximumAttributes = 1024;
inline constexpr uint64_t kRequestHeaderBytes = 72;
inline constexpr uint64_t kResponseHeaderBytes = 48;
inline constexpr uint64_t kAttributeHeaderBytes = 24;

struct Request {
  DracoDecodeLimits limits;
  std::vector<DracoAttributeRequest> attributes;
  std::vector<std::byte> compressed;
};

void ValidateLimits(const DracoDecodeLimits& limits);
void ValidateRequest(std::span<const std::byte> compressed,
                     std::span<const DracoAttributeRequest> attributes,
                     const DracoDecodeLimits& limits);
[[nodiscard]] uint64_t DecodedResponseBytes(const DracoDecodedMesh& mesh);
void WriteRequest(const std::filesystem::path& path,
                  std::span<const std::byte> compressed,
                  std::span<const DracoAttributeRequest> attributes,
                  const DracoDecodeLimits& limits);
[[nodiscard]] Request ReadRequest(const std::filesystem::path& path);
void WriteResponse(const std::filesystem::path& path,
                   const DracoDecodedMesh& mesh,
                   const DracoDecodeLimits& limits);
void WriteFailure(const std::filesystem::path& path, std::string_view message,
                  uint64_t maxOutputBytes);
[[nodiscard]] DracoDecodedMesh ReadResponse(
    const std::filesystem::path& path,
    std::span<const DracoAttributeRequest> attributes,
    const DracoDecodeLimits& limits);

} // namespace container::geometry::gltf::draco_worker
