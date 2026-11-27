#pragma once

#include "Container/geometry/DotBimLoader.h"
#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/BimWalkability.h"
#include "Container/robotics/RoboticsScene.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::robotics::asset_cache {

inline constexpr size_t maxPayloadBytes = 256u * 1024u * 1024u;

inline void require(bool valid) {
  if (!valid)
    throw std::invalid_argument("Malformed or oversized BIM preprocessing payload");
}

class Writer {
public:
  void u32(uint32_t value) { integer(value, sizeof(value)); }
  void u64(uint64_t value) { integer(value, sizeof(value)); }

  void f32(float value) {
    require(std::isfinite(value));
    u32(std::bit_cast<uint32_t>(value));
  }

  void f64(double value) {
    require(std::isfinite(value));
    u64(std::bit_cast<uint64_t>(value));
  }

  void count(size_t value) {
    require(value <= std::numeric_limits<uint32_t>::max());
    u32(static_cast<uint32_t>(value));
  }

  void text(std::string_view value) {
    count(value.size());
    require(value.size() <= maxPayloadBytes - data.size());
    data.append(value);
  }

  std::string data;

private:
  void integer(uint64_t value, size_t bytes) {
    require(bytes <= maxPayloadBytes - data.size());
    for (size_t i = 0; i < bytes; ++i)
      data.push_back(static_cast<char>(static_cast<unsigned char>(value >> (8 * i))));
  }
};

class Reader {
public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  uint32_t u32() { return static_cast<uint32_t>(integer(sizeof(uint32_t))); }
  uint64_t u64() { return integer(sizeof(uint64_t)); }

  float f32() {
    const auto value = std::bit_cast<float>(u32());
    require(std::isfinite(value));
    return value;
  }

  double f64() {
    const auto value = std::bit_cast<double>(u64());
    require(std::isfinite(value));
    return value;
  }

  uint32_t count(size_t minItemBytes) {
    const auto size = u32();
    require(minItemBytes > 0 && size <= bytes_.size() / minItemBytes);
    return size;
  }

  std::string text() {
    return std::string(view(count(1)));
  }

  std::string_view view(size_t bytes) {
    require(bytes <= bytes_.size());
    const auto result = bytes_.substr(0, bytes);
    bytes_.remove_prefix(bytes);
    return result;
  }

  [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

private:
  uint64_t integer(size_t count) {
    require(count <= bytes_.size());
    uint64_t result = 0;
    for (size_t i = 0; i < count; ++i)
      result |= uint64_t(static_cast<unsigned char>(bytes_[i])) << (8 * i);
    bytes_.remove_prefix(count);
    return result;
  }

  std::string_view bytes_;
};

void writeRender(Writer&, const geometry::dotbim::Model&);
[[nodiscard]] geometry::dotbim::Model readRender(Reader&);
void writeCollision(Writer&, const CookedBimCollision&, std::span<const BimWalkableSurface>);
void readCollision(Reader&, const RoboticsScene&, double maxSlopeDegrees,
                   CookedBimCollision&, std::vector<BimWalkableSurface>&);

} // namespace container::robotics::asset_cache
