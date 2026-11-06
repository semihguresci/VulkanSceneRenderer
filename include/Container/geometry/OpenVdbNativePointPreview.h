#pragma once

#include "Container/geometry/DotBimLoader.h"
#include "Container/geometry/OpenVdbPointSelection.h"

namespace container::geometry::vdb {

enum class NativePointColor { Uniform, ScalarRamp, VectorDirection, StringPalette };
struct NativePointPreviewOptions {
  PointGroupSelection groups;
  // Empty selects a physical constant radius. A chosen bool/int32/int64/Float/
  // Double scalar must be finite and positive; radiusScale applies in either
  // case. No automatic use of a field named pscale or v takes place.
  std::string radiusAttribute;
  double constantRadius{.05}, radiusScale{1.0};
  NativePointColor colorMode{NativePointColor::Uniform};
  std::string colorAttribute;
  std::array<float,3> uniformColor{.72f,.8f,.87f};
  float opacity{1.0f};
  size_t maxGrids{256}, maxMetadataBytes{1024ull*1024ull};
};

// First matching native PointDataGrid by default (same as P03). A nonempty
// selector must resolve one grid; descriptor:<exact name> handles duplicates.
// The complete catalog is bounded before grid metadata lookups. All native
// points, including inactive-voxel points, participate in the group predicate.
// P is a registered fixed-codec, stride-one Vec3f voxel-local offset; decoded
// selected offsets must be finite with |P|<=.500001f (native half-voxel
// boundary tolerance). Compensated affine reconstruction
// retains local offsets at remote integer origins. Nonlinear maps reject.
//
// ScalarRamp auto-ranges selected scalar values; bool uses false/true endpoints.
// Exact unsigned integer differences distinguish adjacent Int64 endpoints.
// Float/Double normalization avoids overflow for opposing finite extremes.
// A constant non-bool range is neutral midpoint. VectorDirection maps signed
// native Vec3f/Vec3d COMPONENT direction into RGB; zero is neutral gray (no
// world-space vector transform is implied). StringPalette hashes strict UTF-8
// bytes with FNV-1a into a stable 12-color palette; category collisions can occur.
// Chosen scalar/vector attributes accept registered fixed codecs, stride one;
// StringPalette accepts native full-width StringCodec. Chosen opaque/dynamic
// layouts fail explicitly. Unchosen values are not interpreted or made finite.
// P03 source schema/offset/group/streaming validation still applies.
//
// Six vertices/eight triangles per radius marker, GeometryKind::Mesh, source
// world radius/positions scaled by positive importScale. Float display rounding
// must preserve radius offsets to .1%; unusable scales/origins fail explicitly.
// Opacity<1 sets the existing Blend material contract; point-style controls do
// not apply. Grid/descriptor metadata and attribute/codecs/stride/hidden/groups
// remain bounded model properties (opaque metadata reported by type/byte size,
// never silently converted). Empty selections produce an informative diagnostic.
// Source/native/model/metadata/count/triangles share conservative preflight
// budgets before geometry allocation; estimates are not a hard RSS sandbox.
// No VDB is written and source bytes remain unchanged. One worker. OFF fails
// availability before any paths/options. No camera/renderer edits are required.
[[nodiscard]] dotbim::Model LoadNativePointsPreviewFromFile(
    const std::filesystem::path& input,const std::string& inputGrid={},
    float importScale=1.0f,const NativePointPreviewOptions& preview={},
    const ProcessingOptions& processing={},const PointAttributeOptions& options={});

} // namespace container::geometry::vdb
