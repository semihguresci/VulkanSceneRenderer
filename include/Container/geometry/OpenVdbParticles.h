#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>

namespace container::geometry::vdb {

struct Particle {
  std::array<double, 3> position{0.0, 0.0, 0.0};
  double radius{1.0};
};

struct ParticleRasterizationOptions {
  size_t maxParticles{1'000'000};
};

// Rasterize the union of particle spheres as a FloatGrid level set. Positions,
// radii and stored signed distances use world units. The output transform is
// origin-aligned with processing.voxelSize in [1.5e-5,1e6] (OpenVDB's scale-map
// constructor rejects smaller determinants). Every radius must be
// in [1.5,100] source voxels, matching OpenVDB's default particle cutoffs;
// rejected particles are reported rather than silently omitted. A conservative
// aggregate box, node topology and single-worker workspace are checked against
// processing.maxVoxelCount and processing.maxInputBytes before rasterization.
// The span is borrowed only for this call. Output paths must not already exist.
[[nodiscard]] ProcessingResult WriteParticlesLevelSet(
    std::span<const Particle> particles, const std::filesystem::path& output,
    const ProcessingOptions& processing = {},
    const ParticleRasterizationOptions& options = {});

// Stream a bounded JSON array of {"position":[x,y,z],"radius":r} objects.
// Unknown or duplicate fields, nonnumeric values and incomplete objects fail.
// Raw JSON tokens are limited to 128 bytes before reaching the SAX lexer.
// Parsing reserves three encoded-file sizes for upstream SAX token capacity
// growth plus three decoded-particle sizes under processing.maxInputBytes.
// The encoded file, decoded particles and resulting rasterization are bounded;
// OpenVDB-disabled builds report availability before attempting file access.
[[nodiscard]] ProcessingResult WriteParticlesLevelSetFile(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const ProcessingOptions& processing = {},
    const ParticleRasterizationOptions& options = {});

}  // namespace container::geometry::vdb
