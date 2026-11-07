#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace container::geometry::vdb {

struct PointRecord {
  std::array<double, 3> position{0.0, 0.0, 0.0};
  std::array<double, 3> velocity{0.0, 0.0, 0.0};
  double radius{1.0};
};

struct PointOptions {
  size_t maxPoints{1'000'000};
};

struct PointResult {
  std::string gridName;
  uint64_t pointCount{0};
  double voxelSize{0.0};
};

// Write a real PointDataGrid with P:Vec3f, v:Vec3d and pscale:double, all
// NullCodec, constant stride one. Position, velocity and positive radius use
// world units. P is localized to its voxel; Float32 local offsets introduce
// voxel-relative quantization (normally below 1e-6 voxel plus world-double
// rounding). Generation uses an origin-aligned isotropic transform with voxel
// size [1.5e-5,1e6]. gridName is limited to 256 bytes. Empty inputs are supported.
// Budgets bound points, stored voxel topology and conservative allocation
// estimates; these are not a hard process-RSS limit. A single-worker arena
// limits upstream temporary trees. Existing output files are never overwritten.
[[nodiscard]] PointResult WritePoints(
    std::span<const PointRecord> points, const std::filesystem::path& output,
    const ProcessingOptions& processing = {}, const PointOptions& options = {});

// Strict bounded JSON array: {"position":[x,y,z],"velocity":[x,y,z],"radius":r}.
// Position is required; velocity/radius default to zero/one. Unknown/duplicate
// fields, nonfinite values and raw tokens exceeding 128 bytes are rejected.
// SAX parsing reserves encoded/decoded storage before appending records.
[[nodiscard]] PointResult WritePointsFile(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const ProcessingOptions& processing = {}, const PointOptions& options = {});

// Read the supported P/v/pscale layout, including inactive-voxel points. Missing
// or incompatible attributes, extra attributes and groups are rejected rather
// than silently discarded. Local P is transformed into world coordinates;
// velocity/radius are already world-unit payloads and are not transformed.
// Finite nonsingular affine transforms, source voxel axes [1e-6,1e6], safe
// integer coordinates and sufficient world-double precision are required.
// Returns deterministic lexicographic position/velocity/radius order, which
// need not match insertion order. Custom grid metadata is not exported to JSON.
// Seekable files with valid recorded file_mem_bytes/file_voxel_count statistics
// are required for the metadata preflight; custom point-count metadata is optional.
// OpenVDB delayed loading must be available and globally enabled. Paged
// attributes are checked before materialization; legacy nonpaged encodings can
// still allocate within the upstream reader before these checks. Conservative
// budgets do not provide a hostile-file heap sandbox.
[[nodiscard]] std::vector<PointRecord> ReadPoints(
    const std::filesystem::path& input, const std::string& inputGrid = {},
    const ProcessingOptions& processing = {}, const PointOptions& options = {});

[[nodiscard]] PointResult ExportPoints(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointOptions& options = {});

// All APIs report ENABLE_OPENVDB=ON availability before file access when the
// optional OpenVDB dependency was disabled at build time.
}  // namespace container::geometry::vdb
