#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace container::geometry {

struct OpenVdbVolumeOptions {
  std::string gridName{}; // Empty selects "density", then the first fog scalar.
  uint32_t maxResolution{128}; // Longest world-space axis; bounded to [16,256].
  float importScale{1.0f};
  uint64_t maxActiveVoxels{32ull * 1024 * 1024}; // Shared by selected source grids.
  uint64_t maxInputBytes{512ull * 1024 * 1024}; // Encoded file and combined grid memory.
  std::string temperatureGridName{}; // Empty disables optional Kelvin emission.
  bool sparse{false}; // Preserve native topology in CPU-created NanoVDB Float32.
};

// A bounded, world-aligned density preview with optional Kelvin temperature.
// Both channels share one texture lattice; X varies fastest.
// Native sparse data remains in the source .vdb file and is never overwritten.
struct OpenVdbVolume {
  std::string gridName{};
  std::array<uint32_t, 3> dimensions{};
  std::array<float, 3> boundsMin{}, boundsMax{};
  std::vector<float> density{};
  float maxDensity{0.0f};
  uint64_t sourceActiveVoxels{0};
  std::string temperatureGridName{};
  std::vector<float> temperature{}; // Kelvin on density's texture lattice.
  // Raw serialized NanoVDB Float32 words; CPU consumers must copy into an
  // aligned NanoVDB buffer before using its C++ pointer API. Dense arrays are
  // empty in this mode, and each native grid retains its own affine map.
  std::vector<uint32_t> sparseDensity{}, sparseTemperature{};
  float sparseImportScale{1.0f};
  float sparseStepSize{0.01f};
};

[[nodiscard]] bool openVdbVolumeAvailable() noexcept;
// Throws a diagnostic on disabled builds, invalid/unsupported grids or a preview
// with no positive density. Level sets must be converted to fog explicitly.
[[nodiscard]] OpenVdbVolume loadOpenVdbVolume(
    const std::filesystem::path& path,
    const OpenVdbVolumeOptions& options = {});

} // namespace container::geometry
