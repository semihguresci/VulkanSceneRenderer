#include "Container/renderer/lighting/LtcLutData.h"

#include <gtest/gtest.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace {
using namespace container::renderer;

class LtcLutDataTests : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic_uint counter{0};
    path = std::filesystem::temp_directory_path() /
           ("Container-ltc-lut-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(counter++) + ".bin");
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  static void word(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i)
      bytes[offset + i] = uint8_t(value >> (8 * i));
  }
  static float value(size_t texel, size_t channel) {
    // Include negative matrix coefficients and distinguish every channel/row.
    return (float(texel) + float(channel) * 0.125f) *
           (channel == 2 ? -1.f : 1.f);
  }
  static std::vector<uint8_t> validBytes() {
    std::vector<uint8_t> bytes(16 + LtcLutData::extent * LtcLutData::extent * 16);
    bytes[0] = 'C'; bytes[1] = 'L'; bytes[2] = 'T'; bytes[3] = 'C';
    word(bytes, 4, 1);
    word(bytes, 8, LtcLutData::extent);
    word(bytes, 12, LtcLutData::extent);
    for (size_t texel = 0; texel < LtcLutData::extent * LtcLutData::extent; ++texel)
      for (size_t channel = 0; channel < 4; ++channel)
        word(bytes, 16 + texel * 16 + channel * 4,
             std::bit_cast<uint32_t>(value(texel, channel)));
    return bytes;
  }
  void write(const std::vector<uint8_t> &bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream);
    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    ASSERT_TRUE(stream);
  }
  std::filesystem::path path;
};

TEST_F(LtcLutDataTests, PreservesEveryRgbaChannelAndRowOfLittleEndianData) {
  write(validBytes());
  const auto data = loadLtcLutData(path);
  ASSERT_EQ(data.texels.size(), 64u * 64u);
  for (const size_t texel : {0u, 1u, 63u, 64u, 65u, 4095u})
    for (size_t channel = 0; channel < 4; ++channel)
      EXPECT_FLOAT_EQ(data.texels[texel][channel], value(texel, channel))
          << "texel=" << texel << " channel=" << channel;
}

TEST_F(LtcLutDataTests, RejectsMissingFile) {
  EXPECT_THROW((void)loadLtcLutData(path), std::runtime_error);
}

TEST_F(LtcLutDataTests, RejectsIncorrectMagicVersionAndDimensions) {
  auto bytes = validBytes();
  bytes[0] = 'X';
  write(bytes);
  EXPECT_THROW((void)loadLtcLutData(path), std::runtime_error);
  for (const auto [offset, bad] :
       {std::pair<size_t, uint32_t>{4, 0}, {4, 2}, {8, 0}, {8, 63},
        {8, 128}, {12, 0}, {12, 63}, {12, 128}}) {
    bytes = validBytes();
    word(bytes, offset, bad);
    write(bytes);
    EXPECT_THROW((void)loadLtcLutData(path), std::runtime_error)
        << "offset=" << offset << " value=" << bad;
  }
}

TEST_F(LtcLutDataTests, RejectsTruncatedHeaderPayloadAndTrailingBytes) {
  const auto valid = validBytes();
  for (const size_t length : {size_t(0), size_t(3), size_t(15), size_t(16),
                              size_t(17), valid.size() - 1}) {
    auto bytes = valid;
    bytes.resize(length);
    write(bytes);
    EXPECT_THROW((void)loadLtcLutData(path), std::runtime_error)
        << "length=" << length;
  }
  auto trailing = valid;
  trailing.push_back(0);
  write(trailing);
  EXPECT_THROW((void)loadLtcLutData(path), std::runtime_error);
}

TEST_F(LtcLutDataTests, RejectsNonfiniteValuesAnywhereInEitherTable) {
  for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()})
    for (const size_t texel : {0u, 64u, 4095u}) {
      auto bytes = validBytes();
      word(bytes, 16 + texel * 16 + 3 * 4, std::bit_cast<uint32_t>(invalid));
      write(bytes);
      EXPECT_THROW((void)loadLtcLutData(path), std::runtime_error)
          << "texel=" << texel;
    }
}

TEST(LtcLutPairTests, RejectsFiniteButInvalidMatricesAndAmplitudeMoments) {
  LtcLutData matrix, amplitude;
  matrix.texels.assign(64u * 64u, glm::vec4(1, 0, 0, 1));
  amplitude.texels.assign(64u * 64u, glm::vec4(1, .05f, .02f, 0));
  EXPECT_NO_THROW(validateLtcLutPair(matrix, amplitude));
  const size_t badTexel = 4095;
  for (const auto invalid : {glm::vec4(0), glm::vec4(1, 2, 1, 1),
                              glm::vec4(1, 0, 0, -1)}) {
    matrix.texels[badTexel] = invalid;
    EXPECT_THROW(validateLtcLutPair(matrix, amplitude), std::runtime_error);
  }
  matrix.texels[badTexel] = {1, 0, 0, 1};
  for (const auto invalid : {glm::vec4(-1, 0, 0, 0), glm::vec4(1, -.1f, 0, 0),
                              glm::vec4(1, 1.1f, 0, 0), glm::vec4(1, 0, -1, 0),
                              glm::vec4(1, 0, 1.1f, 0), glm::vec4(1, 0, 0, 1)}) {
    amplitude.texels[badTexel] = invalid;
    EXPECT_THROW(validateLtcLutPair(matrix, amplitude), std::runtime_error);
  }
  amplitude.texels[badTexel] = {1, .05f, .02f, 0};
  EXPECT_NO_THROW(validateLtcLutPair(matrix, amplitude));
  amplitude.texels.pop_back();
  EXPECT_THROW(validateLtcLutPair(matrix, amplitude), std::runtime_error);
}

} // namespace
