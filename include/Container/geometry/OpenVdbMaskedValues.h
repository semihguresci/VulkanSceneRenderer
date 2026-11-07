#pragma once
#include "Container/geometry/OpenVdbTopology.h"

namespace container::geometry::vdb {
enum class MaskedValuesOperation { Activate, Deactivate, IntersectActivity, ClipInside, ClipOutside };

// Source A is an ordinary/fog Float/Double/Bool/Int32/Int64/Vec3I/Vec3S/Vec3D
// grid with finite stored values/background. Source B supports those types and
// MaskGrid: ONLY its active state is used (active false/zero/NaN is selected).
// Finite nonsingular affine maps must match exactly; no mask resampling.
// Activate: A activity OR B activity; Deactivate: A AND NOT B; IntersectActivity:
// A AND B. These retain every logical A value, including inactive values and
// signed zero, even when native topology operators would discard a branch.
// ClipInside/Outside retain A values/activity where B is active/inactive;
// elsewhere use A's inactive background. No tolerance/numeric pruning occurs.
// A metadata, vector frame and transform survive; class becomes UNKNOWN and
// half saving is disabled. File statistics/provenance are regenerated. Source
// B metadata is not merged. Selectors/default selection match WriteValueTopology
// for A, WriteTopologyMask for B. Only one worker; inputs, projected output node
// sizes, stored slots, active counts and writer/workspace/work share limits.
// Encoded snapshots/publication accept Unicode paths/parents subject to a usable
// upstream narrow temp path. Seekable supported versions/recorded statistics
// required; metadata decoding precedes checks, not a hostile-file heap sandbox.
// New exclusive ZIP output, no active-mask value loss, cleanup on failure.
// OFF availability precedes paths/options. SDF repair is a separate operation.
[[nodiscard]] TopologyResult WriteMaskedValues(
    const std::filesystem::path &input, const std::filesystem::path &mask,
    const std::filesystem::path &output, MaskedValuesOperation operation,
    const std::string &inputGrid = {}, const std::string &maskGrid = {},
    const ProcessingOptions &processing = {}, uint64_t maxWorkItems = 200'000'000);
} // namespace container::geometry::vdb
