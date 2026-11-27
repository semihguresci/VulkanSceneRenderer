#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>
#include <stop_token>
#include <string>

namespace container::util {

struct ContentFingerprint {
  std::string digest; // lowercase SHA-256
  uint64_t byteSize{};
  bool operator==(const ContentFingerprint&) const = default;
};

[[nodiscard]] ContentFingerprint FingerprintContent(std::span<const unsigned char> bytes,
                                                    std::stop_token stop = {});
[[nodiscard]] ContentFingerprint FingerprintFileContent(const std::filesystem::path&,
                                                        std::stop_token stop = {});
// The consumer must read to EOF. The returned digest covers exactly the
// binary stream supplied to the consumer, with a fixed-size input buffer.
[[nodiscard]] ContentFingerprint FingerprintFileWhileReading(
    const std::filesystem::path&, const std::function<void(std::istream&)>& consume,
    std::stop_token stop = {});

} // namespace container::util
