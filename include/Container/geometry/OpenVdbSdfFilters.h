#pragma once
#include "Container/geometry/OpenVdbTopology.h"
#include <optional>

namespace container::geometry::vdb {
enum class SdfFilterOperation {
  Gaussian, Mean, Median, MeanCurvature, Laplacian, Fillet, Offset,
  Normalize, Track, Prune, ResizeBand, DilateBand, ErodeBand
};
enum class SdfSpatialScheme { FirstOrder, SecondOrder, ThirdOrder, Weno5, HjWeno5 };
enum class SdfTemporalScheme { Rk1, Rk2, Rk3 };
enum class SdfTrimMode { None, Interior, Exterior, All };
struct SdfFilterOptions {
  uint32_t width{1}, iterations{1}, normalizationCount{3};
  SdfSpatialScheme spatial{SdfSpatialScheme::HjWeno5};
  SdfTemporalScheme temporal{SdfTemporalScheme::Rk1};
  SdfTrimMode trim{SdfTrimMode::All};
  double worldDistance{0}; // Positive Offset expands outward, per iteration.
  uint32_t bandWidth{3}, bandLayers{1}; // Resize target; dilation/erosion layers.
  std::filesystem::path maskInput;
  std::string maskGrid;
  double alphaMinimum{0}, alphaMaximum{1};
  bool invertAlpha{false};
  // Normalize only: mask ACTIVITY selects existing source active voxels.
  // Values/background, including NaN/active false, do not define selection.
  std::filesystem::path normalizationMaskInput;
  std::string normalizationMaskGrid;
  uint64_t maxWorkItems{200'000'000};
};
struct SdfFilterResult : TopologyResult {
  double halfWidthVoxels{0};
  uint64_t gradientSampleCount{0};
  std::optional<double> meanGradientError;
  std::optional<uint64_t> normalizationSelectedVoxels;
};
// Native Float/Double LEVEL_SET fields, finite distances/background, positive
// background and uniform orthogonal affine voxel axes in [1e-6,1e6]. Active
// tiles are rejected. Distances must be within 1024 source voxels; background
// half width is [1,64]. Band operations require near-integral input widths,
// allowing the Float parameter precision of upstream DoubleGrid generators.
// Optional native Float/Double alpha fields require exactly matching affine
// coefficients, finite values and a representable native alpha interval and
// reciprocal. Mask activity is irrelevant. Masks modulate filter updates;
// subsequent tracker normalization is global. Tracker-only operations reject
// alpha masks. Normalize separately accepts a matching-map native activity
// mask (the nine supported non-point types). Unselected world values/activity
// retain their exact bits; normalization does not expand the active domain.
// Native alpha bounds round once to the mask value type.
// Iterations are [1,32], normalization count [0,64] and kernel radius [1,4].
// Positive Offset expands outward, per iteration; nonzero magnitudes span
// [0.001,256] source voxels. Band target/layers are [1,64], with positive final
// width. Offset zero and Normalize with zero normalizations preserve value
// bits. Other processing uses unit-distance/local-index conditioning and can
// round native distances. Underflow/nonfinite results fail before publication.
// Shared byte/active/stored-slot/work limits reserve the full source bounding
// region plus growth, translated/output trees and buffers before processing.
// Remote inactive leaves can make this conservative. Input/output coordinates
// retain a 4096-cell plus stencil margin from Int32 extremes. One worker.
// Exact affine coefficients and source file/grid metadata survive; statistics
// and provenance regenerate, half saving is disabled. Positive width changes
// do not certify closure, reconstruct missing surfaces or validate an SDF.
// Quality uses active six-neighbor central-gradient stencils within one source
// voxel of zero; it is a resolution-dependent indicator, not a validity proof.
// Seekable supported-version files with recorded statistics are required.
// Native Unicode snapshots/publication require a usable narrow SDK temp path.
// Exclusive full-precision ZIP output and owned cleanup; disabled availability
// precedes paths/options. Metadata decoding precedes limits (not a heap sandbox).
[[nodiscard]] SdfFilterResult WriteSdfFiltered(
    const std::filesystem::path& input,const std::filesystem::path& output,
    SdfFilterOperation operation,const SdfFilterOptions& options={},
    const std::string& inputGrid={},const ProcessingOptions& processing={});
} // namespace container::geometry::vdb
