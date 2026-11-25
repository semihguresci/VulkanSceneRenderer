#pragma once

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace container::util {

enum class DerivedAssetCacheStatus {
  Disabled,
  NotCacheable,
  Miss,
  Hit,
  Corrupt,
  IoError,
};

enum class DerivedAssetCacheKeyMismatch { Corrupt, Miss };

struct DerivedAssetCacheRead {
  DerivedAssetCacheStatus status{DerivedAssetCacheStatus::Miss};
  std::string payload;
  uint64_t bytesRead{};
};

[[nodiscard]] std::string_view derivedAssetCacheStatusName(DerivedAssetCacheStatus) noexcept;

// Reads a complete, size-limited entry with a matching version and SHA-256
// key. The payload is returned only after its checksum has been verified.
// Callers that replace an entry at a stable path can treat an older key as a miss.
[[nodiscard]] DerivedAssetCacheRead readDerivedAssetCache(
    const std::filesystem::path& file, uint32_t formatVersion, std::string_view key,
    uint64_t maxPayloadBytes, std::stop_token stop = {},
    DerivedAssetCacheKeyMismatch keyMismatch = DerivedAssetCacheKeyMismatch::Corrupt);

// Atomically replaces a cache entry, returning the file bytes written or zero
// when the optional cache cannot be written. Cancellation still propagates.
[[nodiscard]] uint64_t writeDerivedAssetCache(
    const std::filesystem::path& file, uint32_t formatVersion, std::string_view key,
    std::string_view payload, std::stop_token stop = {});

} // namespace container::util
