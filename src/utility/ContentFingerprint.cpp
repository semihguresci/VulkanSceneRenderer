#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <streambuf>
#include <string_view>

namespace container::util {
namespace {

constexpr size_t kReadChunkBytes = 65536;

// SHA-256 compression and padding per FIPS 180-4. The file reader only needs
// to retain a block and the running state, including for multi-gigabyte GLBs.
class Sha256 {
public:
  void Add(std::span<const unsigned char> input) {
    if (input.size() > std::numeric_limits<uint64_t>::max() / 8 - size_) {
      throw std::length_error("Source is too large to fingerprint with SHA-256");
    }
    size_ += input.size();
    while (!input.empty()) {
      const size_t count = std::min(input.size(), block_.size() - used_);
      std::copy_n(input.begin(), count, block_.begin() + used_);
      used_ += count;
      input = input.subspan(count);
      if (used_ == block_.size()) {
        Compress();
        used_ = 0;
      }
    }
  }

  [[nodiscard]] uint64_t Size() const { return size_; }

  [[nodiscard]] std::string Finish() {
    block_[used_++] = 0x80;
    if (used_ > 56) {
      std::fill(block_.begin() + used_, block_.end(), 0);
      Compress();
      used_ = 0;
    }
    std::fill(block_.begin() + used_, block_.begin() + 56, 0);
    const uint64_t bits = size_ * 8;
    for (int i = 0; i < 8; ++i) {
      block_[63 - i] = static_cast<uint8_t>(bits >> (i * 8));
    }
    Compress();
    std::string digest;
    digest.reserve(64);
    constexpr std::string_view hex = "0123456789abcdef";
    for (uint32_t word : state_) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        digest.push_back(hex[(word >> shift) & 15]);
      }
    }
    return digest;
  }

private:
  void Compress() {
    constexpr std::array<uint32_t, 64> constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<uint32_t, 64> words{};
    for (size_t i = 0; i < 16; ++i) {
      words[i] = (uint32_t(block_[4 * i]) << 24) | (uint32_t(block_[4 * i + 1]) << 16) |
                 (uint32_t(block_[4 * i + 2]) << 8) | block_[4 * i + 3];
    }
    for (size_t i = 16; i < words.size(); ++i) {
      const auto a = words[i - 15];
      const auto b = words[i - 2];
      const auto sigma0 = std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3);
      const auto sigma1 = std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10);
      words[i] = words[i - 16] + sigma0 + words[i - 7] + sigma1;
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (size_t i = 0; i < words.size(); ++i) {
      const auto sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto t1 = h + sigma1 + choose + constants[i] + words[i];
      const auto t2 = sigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    const std::array result{a, b, c, d, e, f, g, h};
    for (size_t i = 0; i < state_.size(); ++i) {
      state_[i] += result[i];
    }
  }

  std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<uint8_t, 64> block_{};
  size_t used_{};
  uint64_t size_{};
};

class FingerprintedInput final : public std::streambuf {
public:
  FingerprintedInput(std::ifstream& file, std::stop_token stop) : file_(file), stop_(stop) {}

  ContentFingerprint finish() {
    checkModelImportCancellation(stop_);
    if (!traits_type::eq_int_type(sgetc(), traits_type::eof()))
      throw std::invalid_argument("Source consumer did not read the entire file");
    return {hash_.Finish(), hash_.Size()};
  }

protected:
  int_type underflow() override {
    checkModelImportCancellation(stop_);
    if (file_.eof()) return traits_type::eof();
    file_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    if (file_.bad() || (!file_ && !file_.eof()))
      throw std::invalid_argument("Source file could not be read");
    const auto received = static_cast<size_t>(file_.gcount());
    if (received == 0) return traits_type::eof();
    hash_.Add({reinterpret_cast<const unsigned char*>(buffer_.data()), received});
    setg(buffer_.data(), buffer_.data(), buffer_.data() + received);
    return traits_type::to_int_type(*gptr());
  }

private:
  std::ifstream& file_;
  std::stop_token stop_;
  Sha256 hash_;
  std::array<char, kReadChunkBytes> buffer_;
};

} // namespace

ContentFingerprint FingerprintContent(std::span<const unsigned char> bytes, std::stop_token stop) {
  Sha256 hash;
  while (!bytes.empty()) {
    checkModelImportCancellation(stop);
    const auto count = std::min(bytes.size(), kReadChunkBytes);
    hash.Add(bytes.first(count));
    bytes = bytes.subspan(count);
  }
  checkModelImportCancellation(stop);
  return {hash.Finish(), hash.Size()};
}

ContentFingerprint FingerprintFileContent(const std::filesystem::path& path, std::stop_token stop) {
  checkModelImportCancellation(stop);
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    throw std::invalid_argument("Source is not a readable file");
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::invalid_argument("Source file could not be opened");
  }
  Sha256 hash;
  std::array<unsigned char, kReadChunkBytes> bytes;
  auto* buffer = reinterpret_cast<char*>(bytes.data());
  const auto capacity = static_cast<std::streamsize>(bytes.size());
  while (file.read(buffer, capacity)) {
    checkModelImportCancellation(stop);
    hash.Add(bytes);
  }
  checkModelImportCancellation(stop);
  if (file.bad() || !file.eof()) {
    throw std::invalid_argument("Source file could not be read");
  }
  hash.Add(std::span(bytes).first(static_cast<size_t>(file.gcount())));
  return {hash.Finish(), hash.Size()};
}

ContentFingerprint FingerprintFileWhileReading(const std::filesystem::path& path,
                                                const std::function<void(std::istream&)>& consume,
                                                std::stop_token stop) {
  checkModelImportCancellation(stop);
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error))
    throw std::invalid_argument("Source is not a readable file");
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::invalid_argument("Source file could not be opened");
  FingerprintedInput buffer(file, stop);
  std::istream input(&buffer);
  consume(input);
  if (input.bad()) throw std::invalid_argument("Source file could not be read");
  return buffer.finish();
}

} // namespace container::util
