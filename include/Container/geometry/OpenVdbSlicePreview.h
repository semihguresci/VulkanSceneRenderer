#pragma once
#include "Container/geometry/DotBimLoader.h"
#include <array>
#include <optional>

namespace container::geometry::vdb {
enum class SliceValue { Scalar, VectorX, VectorY, VectorZ, VectorMagnitude };
enum class SliceColorMap { Gray, BlueRed, Diverging };
struct SlicePreviewOptions {
  std::array<double, 3> origin{}, axisU{1, 0, 0}, axisV{0, 1, 0};
  std::array<double, 2> worldExtent{2, 2};
  std::array<uint32_t, 2> sampleCounts{64, 64};
  SliceValue valueMode{SliceValue::Scalar};
  SliceColorMap colorMap{SliceColorMap::BlueRed};
  double colorMinimum{0}, colorMaximum{1};
  float opacity{1};
  size_t maxSamples{262144}, maxTriangles{524288};
  uint64_t maxWork{4'000'000}, maxActiveVoxels{8'000'000};
  uintmax_t maxInputBytes{512ull * 1024 * 1024};
  size_t maxMetadataBytes{1'048'576};
  // Integer scalar samples use this exact range, or their observed native
  // sample range by default. Bool/Mask defaults to [0,1]. Constant automatic
  // integer ranges use the middle color. Unsupported on floating/vector grids.
  // Integer scalar grids reject nondefault floating colorMinimum/colorMaximum.
  std::optional<std::array<int64_t, 2>> integerRange;
};
// Explicit source-world plane centered at origin. U/V are independently
// normalized, preserving their angle (oblique axes form a parallelogram).
// Nonparallel basis required; worldExtent gives full side lengths[1e-6,1e6].
// Samples include the four edges: N*M vertices,2(N-1)(M-1) shared-index
// triangles, +cross(U,V) normal, two-sided unlit mesh. Axes[2,1024],≤262144
// samples. No grid-bound inference, clipping to active topology or source
// edits. Positive importScale affects geometry only, never values or color
// units.
//
// Scalar Float/Double UNKNOWN/FOG/LEVEL_SET values are sampled unchanged;
// Bool/Mask/Int32/Int64 UNKNOWN scalars and collocated world Vec3I UNKNOWN
// vectors use nearest integer centers (half ties toward the greater index).
// Bool samples stored values; Mask samples its native activity/value bit.
// Integer scalar colors subtract in unsigned64 before floating normalization,
// preserving adjacent Int64 colors even above2^53. Raw min/max are exact text.
// classification is reported, not reinterpreted as surface geometry or fog.
// Collocated WORLD Vec3S/Vec3D/Vec3I UNKNOWN fields accept all five native
// semantic tags: Invariant, Covariant, CovariantNormalize,
// ContravariantRelative and ContravariantAbsolute. They display RAW STORED
// WORLD components: the tag is retained in model properties, without Jacobian
// multiplication, point translation or normal normalization. Float/Double
// vectors use trilinear sampling; Vec3I uses nearest centers as above. Tags
// describe native transform behavior, not a request to transform values while
// displaying them. Generic LOCAL vectors are rejected for every tag.
// Explicit native MAC requires
// GRID_STAGGERED,world=false,CONTRAVARIANT_RELATIVE,
// container:mac_layout="negative_faces",velocity_units="voxels_per_second".
// StaggeredBoxSampler(+.5 component offsets) then Jacobian*u reconstructs
// world components. Generic staggered/local vector semantics are rejected.
// Scalar/world components≤1e12,MAC components≤1e24, all stored/background
// finite. Affine/shear/reflection allowed; MAC requires uniform orthogonal
// affine. Source voxel axes[1e-6,1e6], nonsingular/conditioned inverse and
// centered Double inverse/sample precision checks apply. Float fetches are
// promoted before upstream trilinear interpolation. Background/inactive values
// remain part of interpolation. This is a display sample mesh, not a field
// resample.
//
// t=clamp((value-min)/(max-min),0,1), finite ordered range and span required.
// Gray=(t,t,t); BlueRed=(t,0,1-t); Diverging is blue→white→red, white at.5.
// Exact range endpoints are not clipped; strictly outside values are counted.
// Opacity[0,1] uses material alpha once, Blend below1/Opaque at1. Vertex
// alpha1. Native frame/type/class/map, plane, raw sample min/max, clipping and
// work diagnostics are retained as bounded element properties. Source metadata
// is copied only as bounded text properties. Plane/Float geometry must preserve
// local sample edges/altitude to0.1%; unsupported precision fails explicitly.
// Seekable files/recorded topology, descriptor uniqueness, joint file/scanned
// grid metadata and full typed leaf-buffer memory are checked before paging.
// Owned Unicode-safe snapshots, mandatory delayed loading and shared
// source/geometry/work budgets apply. Metadata decode and legacy eager layouts
// precede all heap checks: estimates are not a hostile-file/RSS sandbox.
// OFF reports availability before filesystem/options access.
[[nodiscard]] dotbim::Model LoadSlicePreviewFromFile(
    const std::filesystem::path &input, const std::string &selection = {},
    float importScale = 1, const SlicePreviewOptions &options = {});
} // namespace container::geometry::vdb
