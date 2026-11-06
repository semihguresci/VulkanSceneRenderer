#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <span>

namespace container::geometry::vdb {

struct MeshConversionOptions {
  bool unsignedDistance{true};
  bool writeTriangleIds{true};
  std::string triangleGridName{"triangle_id"};
};
struct MeshConversionResult {
  ProcessingResult distance;
  std::string triangleGridName; // Empty when writeTriangleIds is false.
  uint64_t triangleCount{0};
};

// Open, disconnected, intersecting and closed triangle meshes can produce an
// unsigned FloatGrid distance field. GRID_UNKNOWN is intentional: this is not a
// signed level set. Values/background are nonnegative world distances; the band
// is processing.halfWidth in [3,64] source voxels. unsignedDistance=false is
// currently rejected; WriteMeshLevelSet provides the closed-surface workflow.
//
// Optional native Int32 closest-triangle IDs have exactly the distance grid's
// active topology and -1 exterior background. IDs are zero-based triangle input
// order, with no omitted/renumbered triangles. Exact upstream distance ties choose
// the lower ID; comparisons retain OpenVDB's finite-precision distance arithmetic.
// Unsigned triangle corners are canonically ordered so permutation duplicates
// produce identical distance calculations, including reversed winding.
// Degenerate/nonfinite triangles and missing vertex indices fail explicitly.
//
// Double world positions are centered and converted to Double voxel coordinates
// before upstream allocation; an exact Double world map restores placement.
// World ULPs must fit 1e-4 voxel. Voxel size is [1.5e-5,1e6], matching installed
// OpenVDB affine-map limits. Full padded tree/workspace, input geometry, output
// metadata and all output grids share processing memory/topology budgets. These
// are conservative logical allocation estimates, not a hard process-RSS cap.
// Seekable output must be new; publication is exclusive and failures clean up.
// Disabled builds fail availability before inspecting inputs/options/paths.
[[nodiscard]] MeshConversionResult WriteMeshDistance(
    std::span<const std::array<double,3>> worldPositions,
    std::span<const std::array<uint32_t,3>> triangles,
    const std::filesystem::path& output, const ProcessingOptions& processing = {},
    const MeshConversionOptions& conversion = {});

// Every mesh instance is transformed into world space in Double. IDs follow
// model element order, then source range triangle order. Both output grids carry
// container:triangle_provenance: JSON [elementIndex,meshId,rangeTriangleIndex]
// triples in ID order. The bounded table is reserved before flattening. Partial
// imports, non-mesh elements, non-affine transforms and invalid ranges fail.
[[nodiscard]] MeshConversionResult WriteMeshDistance(
    const dotbim::Model& model, const std::filesystem::path& output,
    const ProcessingOptions& processing = {}, const MeshConversionOptions& conversion = {});

// Same scene-loader formats and active-scene world-instance rules as
// VoxelizeModel, accepting open surfaces. Encoded input is reserved alongside
// decoded geometry/conversion workspace. Loader allocations precede decoded
// geometry validation; the budget is not a hostile-file allocation sandbox.
[[nodiscard]] MeshConversionResult VoxelizeModelDistance(
    const std::filesystem::path& input, const std::filesystem::path& output,
    float importScale = 1, const ProcessingOptions& processing = {},
    const MeshConversionOptions& conversion = {});

} // namespace container::geometry::vdb
