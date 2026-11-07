#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <vector>

namespace container::geometry::vdb {

enum class RegionsOperation {
  ActiveComponents,
  SignedInterior,
  EnclosedRegion,
  Cavities
};
struct RegionsOptions {
  uint32_t maxComponents{64};         // [1,1024], ActiveComponents only.
  uint64_t maxWorkItems{200'000'000}; // [1,10^10], shared across stages.
  double isovalue{0}; // Signed operations only; exactly native representable.
  uint32_t maxGrids{256}; // [1,4096], scanned descriptors, not just matches.
  uint64_t maxMetadataBytes{1024 * 1024}; // Aggregate file + scanned grids.
};
struct RegionsResult {
  std::vector<ProcessingResult> grids;
  uint64_t sourceActiveVoxelCount{0}, totalActiveVoxelCount{0}, workItems{0};
  std::vector<std::string> diagnostics;
};

// Writes true-valued BoolGrid occupancy masks with inactive false background.
// ActiveComponents labels FACE/6-connected active states, independent of stored
// values (active false/zero/NaN are occupied).
// Float/Double/Bool/Mask/Int32/Int64/ Vec3I/Vec3S/Vec3D are supported. Active
// tiles are explicitly voxelized after expanded-domain/node/scratch preflight.
// PointDataGrid uses its dedicated mask API. Components sort by descending
// count, then lexicographic index bounds and are named processing.gridName_000,
// _001, ... . Empty input writes one empty _000 mask, with an explicit
// diagnostic; this is not a nonempty component.
//
// Signed operations require Float/Double GRID_LEVEL_SET with positive exterior
// background, finite stored distances, no active tiles, and an isovalue
// strictly inside the symmetric background band. Inactive stored distances must
// equal either signed background: malformed/unclassified dense fields are
// rejected. SignedInterior is the represented field's strict value < isovalue,
// INCLUDING inactive negative tiles/voxels. EnclosedRegion fills positive
// regions unreachable from the positive exterior as well as signed interiors.
// It adapts native enclosed-region semantics with a bounded FACE/6-connected
// finite-domain exterior flood; the upstream convergence tracer has no bounded
// work hook. Cavities = EnclosedRegion minus
// SignedInterior; it does not segment zero surfaces or mistake
// negative-exterior surfaces for physical cavities. This is a voxel-topological
// diagnostic, not proof that an original mesh was watertight. Open positive
// passages remain exterior; an empty cavity result is diagnosed. Fully inactive
// source leaves are safe and never passed to the unsafe native boundary tracer.
// Optional upstream fill-mask
// constraints and native value-preserving component exports are not provided by
// this mask-only API.
//
// Finite nonsingular affine maps are copied EXACTLY; coordinates/values are not
// resampled and world-distance units do not enter the topology calculation.
// Custom grid/file metadata survives; native name/class/vector/world/statistics
// and known source structural keys are regenerated for an UNKNOWN/invariant/
// world mask. Nondefault maxComponents is rejected outside ActiveComponents;
// nonzero isovalue is rejected for ActiveComponents. Only processing gridName,
// maxVoxelCount/maxInputBytes apply; generation/mesh settings are ignored.
//
// One worker; encoded snapshot, full decoded buffers/metadata, full prospective
// finite domain, segmentation scratch/component copies and ZIP writer share
// caps before expansion. Full bounding boxes are deliberately conservative and
// may reject widely separated sparse inputs. Components reserve that full box
// per output against the aggregate stored-leaf cap before native splitting.
// Upstream metadata/topology
// decoding precedes checks: these are workspace estimates, not a hostile-file
// RSS sandbox. Delayed-loading support must be enabled. Unicode source/output
// parents use an owned native-path system-temp snapshot; the upstream temp path
// must be usable. Exclusive new output, source bytes unchanged, cleanup on
// failure. OFF throws availability before inspecting options or paths.
[[nodiscard]] RegionsResult WriteRegions(
    const std::filesystem::path &input, const std::filesystem::path &output,
    RegionsOperation operation = RegionsOperation::ActiveComponents,
    const RegionsOptions &options = {}, const std::string &inputGrid = {},
    const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
