#pragma once

#include "Container/geometry/DotBimLoader.h"
#include "Container/geometry/OpenVdbNativePointPreview.h"
#include "Container/geometry/OpenVdbPointPreview.h"
#include "Container/geometry/OpenVdbSlicePreview.h"
#include "Container/geometry/OpenVdbVectorPreview.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace container::geometry::vdb {

struct ImportOptions {
  // Empty selects Float/Double surfaces and standard PointDataGrid previews,
  // or the first eligible field in an explicit preview mode. A name selects one
  // grid. Prefix descriptor: selects only an exact descriptor name, including
  // the first descriptor of duplicate display names (typically name[0]).
  std::string gridName{};
  // Defaults to zero for level sets, 0.5 for fog and other scalar volumes.
  std::optional<double> isoValue{};
  double adaptivity{0.0};
  // Checked against file metadata and the loaded topology before extraction.
  uint64_t maxActiveVoxels{8'000'000};
  // Caps the encoded file, recorded grid memory and loaded topology. Stored
  // leaves (including inactive voxels) also share the voxel budget above.
  uintmax_t maxInputBytes{512ull * 1024ull * 1024ull};
  // Total output budget across grids. OpenVDB extraction precedes this check;
  // maxActiveVoxels bounds the input to the mesher itself.
  size_t maxTriangles{2'000'000};
  // Scene viewing includes bounded world-radius point markers. Scalar-only
  // consumers such as OBJ isosurface export explicitly disable this route.
  bool includePoints{true};
  PointPreviewOptions pointPreview{};
  // Explicit vector-only mode. Empty retains scalar + optional point import.
  // Bounds/seeds/lengths use source-world units; the outer budgets also apply.
  std::optional<VectorPreviewOptions> vectorPreview{};
  // Explicit source-world sampled plane, using raw scalar or reconstructed
  // world-vector values. The outer voxel/triangle/byte caps also apply.
  std::optional<SlicePreviewOptions> slicePreview{};
  // Explicit native attribute/group preview. This route is independent of
  // includePoints and the standard pointPreview controls; nativePointLimits
  // bounds its source points/schema/strings. Outer voxel/triangle/byte caps
  // apply through ProcessingOptions. All three explicit modes are mutually
  // exclusive; empty retains the standard surface + optional point route.
  std::optional<NativePointPreviewOptions> nativePointPreview{};
  PointAttributeOptions nativePointLimits{};
};

[[nodiscard]] bool IsAvailable() noexcept;

// Extracts selectable scalar surfaces and optional point radius markers in
// grid world coordinates. importScale
// must be finite and positive. An explicit vector/slice/native-point mode
// dispatches only that preview. This does not perform volume ray marching.
[[nodiscard]] dotbim::Model LoadFromFile(const std::filesystem::path &path,
                                         float importScale = 1.0f,
                                         const ImportOptions &options = {});

} // namespace container::geometry::vdb
