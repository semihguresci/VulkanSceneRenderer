#pragma once
#include "Container/geometry/OpenVdbTopology.h"
#include <optional>

namespace container::geometry::vdb {
enum class SdfBackgroundOperation { BackgroundOnly, FloodFill };
struct SdfBackgroundOptions {
  // Positive WORLD-distance magnitudes. Default outside=source background;
  // default inside=requested outside (symmetric). No active-band expansion.
  std::optional<double> outsideWidth, insideWidth;
  uint64_t maxWorkItems{200'000'000};
};
// Native Float/Double LEVEL_SET inputs, finite values, positive background,
// uniform orthogonal affine voxels [1e-6,1e6] and no active tiles are required.
// Active native values/activity, maps and file/grid metadata survive exactly;
// requested widths must contain every existing active positive/negative value.
// Output uses the rounded native widths, reported in diagnostics. Existing
// half saving is disabled and file statistics/provenance are regenerated.
// BackgroundOnly sets every inactive stored value to -insideWidth if v<0,
// +outsideWidth otherwise, and changes the infinite background. -0 is outside.
// FloodFill first propagates signs from active narrow-band voxels using native
// signed flood fill, then normalizes inactive root/child values to these widths.
// It assumes a CLOSED signed field; this does not certify closure, repair a
// missing surface, or reinitialize distances. Inactive-only leaf/node signs
// remain seeds, so arbitrary corrupted interiors cannot be recovered this way.
// Changing widths does not expand activity or create a wider distance band.
// Root scanline growth is reserved before filling. Root child-coordinate spans
// must fit signed Int32 differences, avoiding upstream Coord subtraction UB.
// One worker, shared input/tree/clone/root-growth/writer byte/voxel/work caps,
// new exclusive full-precision ZIP output, owned cleanup on failure. Seekable
// supported files with recorded stats, native Unicode snapshots/publication
// subject to a usable narrow upstream temp path. Metadata decoding precedes
// limits (not a hostile-file heap sandbox). OFF precedes options/paths.
[[nodiscard]] TopologyResult WriteSdfBackground(
    const std::filesystem::path &input, const std::filesystem::path &output,
    SdfBackgroundOperation operation = SdfBackgroundOperation::BackgroundOnly,
    const SdfBackgroundOptions &options = {}, const std::string &inputGrid = {},
    const ProcessingOptions &processing = {});
} // namespace container::geometry::vdb
