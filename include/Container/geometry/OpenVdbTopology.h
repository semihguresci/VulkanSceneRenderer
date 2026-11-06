#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <vector>

namespace container::geometry::vdb {

enum class TopologyOperation { ActiveMask, Dilate, Erode, Open, Close };
enum class TopologyBoolean { Union, Intersection, Difference };
enum class TopologyConnectivity { Faces6, FacesEdges18, Full26 };
enum class TopologyTilePolicy { Preserve, Expand, Ignore };
struct TopologyOptions {
  uint32_t iterations{1}; // Each morphology phase: [1,32] index-voxel steps.
  TopologyConnectivity connectivity{TopologyConnectivity::Faces6};
  TopologyTilePolicy tilePolicy{TopologyTilePolicy::Preserve};
  uint64_t maxWorkItems{200'000'000}; // Conservative sparse work cap [1,10^10].
};
struct TopologyResult {
  ProcessingResult grid;
  uint64_t inputActiveVoxelCountA{0}, inputActiveVoxelCountB{0};
  uint64_t activeTileCount{0}, storedLeafCount{0}, workItems{0};
  std::vector<std::string> diagnostics;
};

enum class ValueTopologyOperation {
  ActivateValue, DeactivateValue, PruneExact, FillBox, ClipBox, ReplaceBackground
};
struct ValueTopologyOptions {
  // Native decimal components: one scalar/bool or three vector components.
  // Integers are parsed directly, including every Int64 bit; true/false for Bool.
  // Finite real literals round once to the native Float/Double representation.
  std::vector<std::string> value{"0"};
  std::array<int32_t, 3> minimum{0, 0, 0}, maximum{0, 0, 0};
  bool fillActive{true};
  uint64_t maxWorkItems{200'000'000};
};

// Float/Double/Bool/Int32/Int64/Vec3I/Vec3S/Vec3D native values, activity,
// transform and file/grid metadata are retained outside the explicit edit.
// MaskGrid/point grids are rejected: use their dedicated topology/point APIs.
// Stored values/background must be finite, including inactive values.
// Activate/Deactivate matches BIT-EXACT stored values, including signed zero;
// implicit infinite background remains inactive. Activating default internal
// tiles can exceed the active cap even for a source with few leaves.
// PruneExact collapses only bit-identical, equal-state children; it never uses
// a tolerance or erases differently signed background tiles. No value is lost.
// FillBox replaces the inclusive INDEX box and its activity. ClipBox retains
// native values/activity inside it; outside becomes inactive background.
// Box operations conservatively bound all dense leaf slots/nodes in the box
// before expansion, including inactive fills. maximum <= INT32_MAX-4096 avoids
// upstream sparse-fill signed loop overflow at the final root block.
// ReplaceBackground updates the infinite background and only inactive stored
// values bit-identical to the old background; opposite signs/nearby values and
// active values remain unchanged. It does not repair SDF signs or band widths.
// SDF/staggered classes are accepted only by lossless PruneExact; other edits
// require ordinary/fog fields and produce class UNKNOWN, preserving vector
// frame tags. File statistics are regenerated, half saving is disabled, ZIP
// is used without active-mask value loss. container:value_topology_* provenance
// is regenerated; other file/grid metadata survives.
// A private native-path snapshot permits Unicode input/output parents; the
// underlying upstream narrow filename API still requires a usable temp path.
// One worker, shared encoded/decoded/clone/expansion/work/writer limits, new
// exclusive output, cleanup on failure. Recorded memory/voxel stats required;
// upstream metadata decoding precedes checks (not a hostile-file sandbox).
// Non-box options must keep default box/fill settings; no-value operations
// (PruneExact/ClipBox) require the default value. OFF fails before options/I/O.
[[nodiscard]] TopologyResult WriteValueTopology(
    const std::filesystem::path &input, const std::filesystem::path &output,
    ValueTopologyOperation operation, const ValueTopologyOptions &options = {},
    const std::string &inputGrid = {}, const ProcessingOptions &processing = {});

// Output is ALWAYS a native MaskGrid: true exactly at active cells, false
// elsewhere, independent of the source stored values/background/class. Active
// false Bool cells and active zero/NaN numeric cells are occupied; inactive
// nonzero values and inactive negative SDF interiors are unoccupied. This does
// not compute a solid interior or reinterpret PointDataGrid attributes.
// Standard Float/Double/Bool/Mask/Int32/Int64/Vec3I/Vec3S/Vec3D sources are
// supported. PointDataGrid uses the separate original-point-voxel mask API.
// Finite nonsingular affine transforms are copied exactly; unsupported maps
// fail clearly. Source custom metadata survives; native class/vector/world,
// name and recorded statistics are regenerated as UNKNOWN/invariant/world.
// Known source point/fluid structural metadata is not claimed to describe
// mask values; container:topology_* provenance describes this operation.
//
// ActiveMask copies sparse active states/tiles, including remote inactive
// structure; it rejects nondefault morphology iterations/connectivity/policy.
// Dilation/erosion use native 6/18/26-neighbor index-lattice morphology.
// Open=erosion(iterations) then dilation(iterations), Close=reverse. These are
// numerical voxel-mask operators, not geometric world-distance offsets.
// Preserve: tiles participate; boundary tiles may split, unchanged interiors
// stay tiled, constant regions may be pruned. Expand: fully voxelize active
// tiles before each phase and keep their voxelized storage. Ignore: only leaf
// voxels change; active tiles participate as neighbors in erosion but are
// never eroded/dilated. Consequently Ignore Open/Close need not obey standard
// whole-set morphological identities for tiled inputs. Native morphology may
// replace/prune inactive structural nodes; source storage is never modified.
//
// Encoded snapshots, full decoded trees/metadata, mask clones, candidate
// leaf/internal/root topology, serial morphology scratch and output writer
// share byte/voxel/work caps before expansion. Candidate leaves are bounded
// locally around active leaf/tile blocks, not by a remote-point global box.
// Conservative estimates can reject a sparse operation that would otherwise
// fit. Upstream metadata/topology decoding precedes caller checks; this is not
// a hostile-file heap sandbox. One worker, exclusive new seekable ZIP output;
// no published output on failure. OFF availability precedes options and I/O.
[[nodiscard]] TopologyResult WriteTopologyMask(
    const std::filesystem::path &input, const std::filesystem::path &output,
    TopologyOperation operation = TopologyOperation::ActiveMask,
    const TopologyOptions &topology = {}, const std::string &inputGrid = {},
    const ProcessingOptions &processing = {});

// Boolean occupancy uses A/B active states only: A OR B, A AND B, A AND NOT B.
// Mixed supported native types/classes are allowed. Exact affine origin and
// Jacobian coefficients must match; no approximate comparison/resampling.
// Source A custom metadata/transform are retained; B metadata is not merged.
// Sparse active tiles remain compact where possible; no tile voxelization or
// value arithmetic is requested. Limits/output/availability match above.
// maxWorkItems is a conservative union-topology visit cap [1,10^10].
[[nodiscard]] TopologyResult WriteTopologyBoolean(
    const std::filesystem::path &inputA, const std::filesystem::path &inputB,
    const std::filesystem::path &output, TopologyBoolean operation,
    const std::string &gridA = {}, const std::string &gridB = {},
    const ProcessingOptions &processing = {},
    uint64_t maxWorkItems = 200'000'000);

} // namespace container::geometry::vdb
