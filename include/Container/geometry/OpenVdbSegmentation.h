#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

#include <cstdint>
#include <vector>

namespace container::geometry::vdb {

struct SegmentationOptions {
  uint32_t maxComponents{64}; // Accepted range [1,1024].
};

// Separate 6-connected zero-crossing surfaces using OpenVDB segmentSDF and
// write all components into one new .vdb. This labels surfaces, so a nested
// cavity can become a separate component from the enclosing outer surface.
// Components with a negative exterior (such as inverted cavity surfaces) are
// rejected: retaining their signs would produce an unbounded exterior field.
// Isolate/rebuild these as closed positive-exterior level sets first.
// This does not assign physical solid-object labels. Empty/no-crossing inputs
// fail. Inputs must be FloatGrid narrow-band level sets, with uniform affine
// voxels in [1e-6,1e6], a [3,64]-voxel band, finite stored distances inside that
// band, and no active tiles. Source transforms and custom metadata are retained.
// Generation voxelSize/halfWidth options do not resample the source.
// Components are ordered by descending active count, then lexicographic index
// bounding box (minimum followed by maximum), and named gridName_000, _001, ...
// The output gridName base is limited to 256 bytes. Custom metadata copies and
// generated names are included in the component workspace budget.
// An exact zero-mask prepass checks maxComponents before upstream segmentation.
// Shared budgets bound encoded input, topology and conservative workspace
// estimates before each stage, and summed output topology afterward. Upstream
// allocation overhead means these estimates are not a hard process-RSS limit.
// Output paths must not exist. Disabled builds throw before reading input.
[[nodiscard]] std::vector<ProcessingResult> WriteSegmentedLevelSet(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const std::string& inputGrid = {}, const ProcessingOptions& options = {},
    const SegmentationOptions& segmentation = {});

}  // namespace container::geometry::vdb
