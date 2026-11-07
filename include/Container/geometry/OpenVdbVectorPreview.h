#pragma once

#include "Container/geometry/OpenVdbPointPreview.h"

namespace container::geometry::vdb {

enum class VectorPreviewMode { Glyphs, Streamlines };

struct VectorPreviewOptions {
  VectorPreviewMode mode{VectorPreviewMode::Glyphs};
  bool hasBounds{false};
  std::array<double, 3> boundsMin{-1, -1, -1}, boundsMax{1, 1, 1};
  std::array<uint32_t, 3> seedCounts{4, 4, 4};
  double displaySeconds{.1}; // Glyph displacement=v*seconds before length cap.
  double maxGlyphLength{1};  // Shortening is counted/reported, never silent.
  double worldRadius{.01};
  double worldStep{.05}; // RK4 arc-length step, additionally limited by grid map.
  uint32_t maxSteps{128};
  bool bothDirections{true};
  size_t maxSeeds{4096}, maxTriangles{1'000'000};
  uint64_t maxWork{2'000'000}, maxActiveVoxels{8'000'000};
  uintmax_t maxInputBytes{512ull * 1024 * 1024};
  // Optional Bool/Float/Double positive active fluid mask in the SAME file,
  // with exactly the vector transform and zero background. Empty means bounds
  // clipping only. Inactive/zero mask cells are solid.
  std::string domainGrid{};
  PointPreviewColor colorMode{PointPreviewColor::VelocityDirection};
  std::array<float, 3> uniformColor{.72f, .8f, .87f};
  double velocityScale{1};
};

// One selected collocated WORLD Vec3S/Vec3D field, including gradient/curl
// covariant metadata, or an explicitly tagged M04/M05 native MAC field:
// GRID_STAGGERED, world=false, CONTRAVARIANT_RELATIVE,
// container:mac_layout="negative_faces", velocity_units="voxels_per_second".
// MAC uses upstream StaggeredBoxSampler's component +.5 offsets then Jacobian*u
// to world velocity. Native MAC transforms must be uniform orthogonal affine;
// collocated fields support finite conditioned affine shear/reflection too.
// Stored world components <=1e12, MAC <=1e24; all stored values/background finite.
// Descriptor selection and seekable/recorded input statistics are required.
//
// Bounds, seeds, radius, step and length are SOURCE-WORLD units, then positive
// importScale is applied to geometry only. Centered regular seed positions are
// deterministic. Nonzero inactive background or no active extent REQUIRES
// explicit bounds; no infinite field extent is invented. Automatic bounds are
// the world AABB of active cell support (not the integration domain topology).
//
// Glyphs are closed mesh arrows. Zero vectors are omitted with diagnostics;
// maxGlyphLength shortening is reported. Streamlines are instantaneous field
// lines, NOT time-dependent trajectories: normalized world-vector RK4 with
// effective step=min(worldStep,.5/max inverse-Jacobian row norm). Bounds and
// optional fluid-mask DDA clip traces and reject stencil leakage through walls.
// RK stages crossing a boundary conservatively clip an Euler segment and stop.
// Zero/stagnant, return-to-seed cycles, clipping and step exhaustion are counted
// in model properties/diagnostics. Tubes use capped six-sided mesh segments.
//
// Scalar lengths/seconds are [1e-6,1e6], maxSteps [1,4096], seed axes [1,256],
// total seeds <=4096 and maxSeeds. Work counts samples, trace crossings and
// generated triangles, and is capped at <=1e9. Input/tree/metadata plus retained
// geometry, allocation growth and serial scratch are budgeted; no RSS sandbox
// is claimed. Float vertices must retain offsets to .1% of local radius/length;
// unsupported world/index precision and nonfinite values fail explicitly.
// No file changes or hidden threads. OFF fails before filesystem/options access.
[[nodiscard]] dotbim::Model LoadVectorPreviewFromFile(const std::filesystem::path &input,
                                                      const std::string &selection = {},
                                                      float importScale = 1,
                                                      const VectorPreviewOptions &options = {});

} // namespace container::geometry::vdb
