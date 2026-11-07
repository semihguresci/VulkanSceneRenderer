#pragma once

#include "Container/geometry/DotBimLoader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace container::geometry::vdb {

enum class PointPreviewColor { Uniform, VelocityDirection, VelocityMagnitude };

struct PointPreviewOptions {
  size_t maxPoints{1'000'000};
  size_t maxGrids{256};
  uint64_t maxActiveVoxels{8'000'000};
  size_t maxTriangles{2'000'000};
  uintmax_t maxInputBytes{512ull * 1024 * 1024};
  // Shared conservative raw/text/property metadata reserve across grids.
  size_t maxMetadataBytes{1'048'576};
  PointPreviewColor colorMode{PointPreviewColor::VelocityDirection};
  std::array<float, 3> uniformColor{.72f, .8f, .87f};
  // Magnitude mode uses a blue-to-red linear speed ramp, saturated at this
  // positive finite speed in SOURCE world units/second. Import scale affects
  // marker position/radius; it does not change the source velocity color scale.
  double velocityScale{1.0};
};

// Preview all standard PointDataGrids, or one explicitly selected grid. Plain
// ambiguous names fail; descriptor:<exact name> resolves duplicate names.
// Reuses strict ReadPoints P:Vec3f/v:Vec3d/pscale:double NullCodec validation.
// Every stored record, including inactive-voxel records, becomes a marker;
// per-point selection/groups and arbitrary attributes are not previewed.
//
// Each marker has six vertices and eight triangles. Positive importScale is
// applied to world P and world pscale radius. GeometryKind::Mesh deliberately
// uses triangle rendering without point-style/global pixel overlays; the
// element type remains OpenVDB Points. Point-style controls do not apply.
// Vertex colors show uniform color, signed velocity direction
// (zero is neutral gray), or the explicit speed ramp. Source units/attributes,
// transform and bounded grid metadata are reported as element properties.
// Float32 coordinates/radii must preserve marker offsets to 0.1% of radius;
// unusable large origins/tiny radii fail with a recenter/rescale diagnostic.
// Counts, stored topology, metadata and triangle/output workspace budgets are
// shared across selected grids. Estimates are not a hard process-RSS bound;
// ReadPoints' seekable/delayed-loading requirements apply. Empty grids produce
// diagnostics; an all-empty selection returns a model without elements.
// Source files remain unchanged. Disabled builds fail availability first.
[[nodiscard]] dotbim::Model LoadPointsPreviewFromFile(
    const std::filesystem::path& input, const std::string& selection = {},
    float importScale = 1.0f, const PointPreviewOptions& options = {});

} // namespace container::geometry::vdb
