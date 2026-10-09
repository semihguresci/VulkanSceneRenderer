#include "Container/renderer/lighting/LtcLutData.h"

#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

namespace container::renderer {

LtcLutData loadLtcLutData(const std::filesystem::path &path) {
  constexpr size_t texelCount = LtcLutData::extent * LtcLutData::extent;
  constexpr size_t byteCount = 16 + texelCount * 4 * sizeof(float);
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream)
    throw std::runtime_error("LTC table is missing: " + path.string());
  if (stream.tellg() != std::streampos(byteCount))
    throw std::runtime_error("LTC table length is invalid: " + path.string());
  std::array<uint8_t, byteCount> bytes;
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
  if (!stream)
    throw std::runtime_error("LTC table read failed: " + path.string());
  auto word = [&](size_t offset) {
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
           (uint32_t(bytes[offset + 2]) << 16) |
           (uint32_t(bytes[offset + 3]) << 24);
  };
  if (bytes[0] != 'C' || bytes[1] != 'L' || bytes[2] != 'T' ||
      bytes[3] != 'C' || word(4) != 1 || word(8) != LtcLutData::extent ||
      word(12) != LtcLutData::extent)
    throw std::runtime_error("LTC table header is invalid: " + path.string());
  LtcLutData result;
  result.texels.resize(texelCount);
  for (size_t texel = 0; texel < texelCount; ++texel)
    for (size_t channel = 0; channel < 4; ++channel) {
      const float value =
          std::bit_cast<float>(word(16 + (texel * 4 + channel) * 4));
      if (!std::isfinite(value))
        throw std::runtime_error("LTC table contains nonfinite data: " +
                                 path.string());
      result.texels[texel][channel] = value;
    }
  return result;
}

void validateLtcLutPair(const LtcLutData &matrix,
                        const LtcLutData &amplitude) {
  constexpr size_t count = LtcLutData::extent * LtcLutData::extent;
  if (matrix.texels.size() != count || amplitude.texels.size() != count)
    throw std::runtime_error("LTC table pair dimensions do not match");
  for (size_t i = 0; i < count; ++i) {
    const auto &m = matrix.texels[i];
    const auto &a = amplitude.texels[i];
    for (int channel = 0; channel < 4; ++channel)
      if (!std::isfinite(m[channel]) || !std::isfinite(a[channel]))
        throw std::runtime_error("LTC table pair contains nonfinite data");
    const double determinant = double(m.x) * m.w - double(m.y) * m.z;
    if (!std::isfinite(determinant) || determinant <= 0)
      throw std::runtime_error("LTC matrix must have a positive determinant");
    if (a.x < 0 || a.y < 0 || a.y > a.x + 1e-5f ||
        a.z < 0 || a.z > 1 || a.w != 0)
      throw std::runtime_error("LTC amplitude moments are invalid");
  }
}

} // namespace container::renderer
