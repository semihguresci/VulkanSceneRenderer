#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace container::geometry::dotbim { struct Model; }

namespace container::geometry::vdb {

struct ProcessingOptions {
  double voxelSize{0.1};
  double halfWidth{3.0};
  // Conservative bounding-box budget checked before voxelization or fog fill.
  uint64_t maxVoxelCount{16'000'000};
  size_t maxTriangles{1'000'000};
  // Encoded input, loaded grid topology and conservative filter workspace cap.
  uintmax_t maxInputBytes{512ull * 1024ull * 1024ull};
  std::string gridName{"surface"};
};

struct SphereOptions {
  double radius{1.0};
  std::array<double, 3> center{0.0, 0.0, 0.0};
};

struct ProcessingResult {
  std::string gridName;
  std::string gridClass;
  uint64_t activeVoxelCount{0};
  double voxelSize{0.0};
};

enum class CsgOperation { Union, Intersection, Difference };
enum class SmoothingOperation { Gaussian, Mean, Median };

struct SmoothingOptions {
  uint32_t width{1}; // Kernel radius in source voxels, in [1,4].
  uint32_t iterations{1}; // In [1,32]; Gaussian uses four tracked box passes.
};

// All operations throw when OpenVDB was disabled at build time. Output paths
// must not already exist. Mesh inputs must describe closed triangle surfaces.
// Stored scalar values must be finite and grid transforms nonsingular affine.
// Input grid selectors accept descriptor:<exact descriptor name> to resolve
// duplicate display names; plain ambiguous names fail.
[[nodiscard]] ProcessingResult WriteSphere(
    const std::filesystem::path& output, const SphereOptions& sphere = {},
    const ProcessingOptions& options = {});
[[nodiscard]] ProcessingResult WriteMeshLevelSet(
    const dotbim::Model& model, const std::filesystem::path& output,
    const ProcessingOptions& options = {});
// Reuses the .bim, .ifc, .ifcx, USD-family and glTF/GLB importers. glTF uses
// active-scene world instances. Incomplete imports and non-mesh objects fail.
[[nodiscard]] ProcessingResult VoxelizeModel(
    const std::filesystem::path& input, const std::filesystem::path& output,
    float importScale = 1.0f, const ProcessingOptions& options = {});
// CSG requires FloatGrid level sets with identical transforms and band widths.
[[nodiscard]] ProcessingResult WriteCsg(
    const std::filesystem::path& inputA, const std::filesystem::path& inputB,
    const std::filesystem::path& output, CsgOperation operation,
    const std::string& gridA = {}, const std::string& gridB = {},
    const ProcessingOptions& options = {});
[[nodiscard]] ProcessingResult WriteFog(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const std::string& inputGrid = {}, const ProcessingOptions& options = {});
// Resamples FloatGrid level sets or fog to an origin-aligned isotropic world
// transform. Level sets are rebuilt from their zero surface, preserving world
// distance units and using options.halfWidth at options.voxelSize. Rebuilds
// follow OpenVDB's topology semantics and can remove interior bubbles.
[[nodiscard]] ProcessingResult WriteResampled(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const std::string& inputGrid = {}, const ProcessingOptions& options = {});
// Edit a FloatGrid level set using OpenVDB's interface-tracking filters. The
// source transform, narrow-band width and custom metadata are retained. Uniform
// voxels in [1e-6,1e6] world units, a band half width in [3,64] voxels and stored
// distances inside that band are required for Float32 tracking. Budgets also
// bound the conservative filter workspace before each topology expansion.
// Positive worldDistance dilates outward; negative values erode inward. The
// Nonzero magnitudes span [0.001,256] source voxels; zero writes an unchanged
// copy. Distances remain in world units regardless of the source voxel size.
[[nodiscard]] ProcessingResult WriteOffset(
    const std::filesystem::path& input, const std::filesystem::path& output,
    double worldDistance, const std::string& inputGrid = {},
    const ProcessingOptions& options = {});
[[nodiscard]] ProcessingResult WriteSmoothed(
    const std::filesystem::path& input, const std::filesystem::path& output,
    SmoothingOperation operation, const SmoothingOptions& smoothing = {},
    const std::string& inputGrid = {}, const ProcessingOptions& options = {});
// Nearest-voxel FloatGrid/DoubleGrid sample in world coordinates, useful for
// SDF/occupancy and derived scalar-field queries. Generation/output options
// (voxelSize, halfWidth, gridName, maxTriangles) are ignored; only the voxel and
// input-memory budgets apply. Supports finite nonsingular affine transforms.
[[nodiscard]] double SampleScalar(
    const std::filesystem::path& input, const std::array<double, 3>& position,
    const std::string& inputGrid = {}, const ProcessingOptions& options = {});

}  // namespace container::geometry::vdb
