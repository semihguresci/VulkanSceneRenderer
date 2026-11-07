#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace container::renderer {

// The caller owns the Vulkan staging buffers and waits for their producing frame.
class ReferenceOptixDenoiser {
 public:
  ReferenceOptixDenoiser(const std::array<uint8_t, 16>& deviceUuid, uint32_t width, uint32_t height);
  ~ReferenceOptixDenoiser();

  ReferenceOptixDenoiser(const ReferenceOptixDenoiser&) = delete;
  ReferenceOptixDenoiser& operator=(const ReferenceOptixDenoiser&) = delete;

  // Color, albedo, and output are RGBA32F. The camera-space normal is RG32F.
  double denoise(std::span<const float> color, std::span<const float> albedo,
      std::span<const float> normal, std::span<float> output);
  [[nodiscard]] size_t allocatedBytes() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace container::renderer
