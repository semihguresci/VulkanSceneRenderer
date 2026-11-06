#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include <variant>
#include <vector>

namespace container::geometry::vdb {

using DenseValues =
    std::variant<std::vector<float>, std::vector<double>, std::vector<std::array<float, 3>>,
                 std::vector<std::array<double, 3>>>;
using DenseValue = std::variant<float, double, std::array<float, 3>, std::array<double, 3>>;
enum class DenseGridClass : uint32_t { Unknown, LevelSet, FogVolume, Staggered };
enum class DenseVectorType : uint32_t {
  Invariant,
  Covariant,
  CovariantNormalize,
  ContravariantRelative,
  ContravariantAbsolute
};
struct DenseRegion {
  std::array<int32_t, 3> origin{};
  std::array<uint32_t, 3> dimensions{1, 1, 1};
};
struct DenseInterchangeDiagnostics {
  uint64_t activeVoxelsOutside{};
  bool storedNonBackgroundOutside{};        // Includes inactive values and signed-zero differences.
  std::vector<std::string> omittedMetadata; // All nonstructural metadata keys.
};
struct DenseArrayVolume {
  DenseRegion region;
  // Row-major linear map: world[r]=sum_c(linear[3*r+c]*index[c])+translation[r].
  // Coordinates use ABSOLUTE integer indices, including region.origin.
  std::array<double, 9> linear{1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::array<double, 3> translation{};
  DenseValues values{std::vector<float>{0}};
  DenseValue background{0.0f}; // Must match values' variant alternative exactly.
  // X fastest: offset=((z*dimY)+y)*dimX+x. Active byte0/1 for EVERY cell.
  // Inactive nonbackground cells and signed zero remain explicit values.
  std::vector<uint8_t> active{0};
  std::string gridName{"field"};
  DenseGridClass gridClass{DenseGridClass::Unknown};
  DenseVectorType vectorType{DenseVectorType::Invariant};
  bool worldSpace{true};
  DenseInterchangeDiagnostics diagnostics;
};

// Bounded Float/Double/Vec3S/Vec3D native-type interchange. Region is mandatory;
// constant background grids acquire no invented active extent. Values and
// activity are exact inside the region; all outside losses/custom-metadata
// omissions are reported. Classification is a preserved tag: a cropped level
// set is not necessarily a globally valid signed-distance field.
[[nodiscard]] DenseArrayVolume ReadDenseVdb(const std::filesystem::path &input,
                                            const DenseRegion &region,
                                            const std::string &inputGrid = {},
                                            const ProcessingOptions &processing = {});
[[nodiscard]] ProcessingResult WriteDenseVdb(const std::filesystem::path &output,
                                             const DenseArrayVolume &volume,
                                             const ProcessingOptions &processing = {});

// CVDBARR\0 version1 binary schema, all integers little-endian, numeric values
// IEEE754 binary32/binary64. No compression, conversion, omitted active mask,
// padding, or implicit ordering. Header carries type/class/vector/world flags,
// origin/dimensions, the explicit affine above, typed background, cell/payload/
// total lengths and diagnostics. Length-prefixed name/omitted metadata keys
// precede x-fastest native component values, followed by one active byte/cell.
// Version/reserved flags, all lengths/counts/trailing bytes and enum values are
// checked before payload allocation. File I/O preserves finite native bits, including
// signed zero and subnormal values. VDB import explicitly stores inactive cells
// in allocated leaves; it does not infer activity from background tolerance.
[[nodiscard]] DenseArrayVolume ReadDenseArrayFile(const std::filesystem::path &input,
                                                  const ProcessingOptions &processing = {});
void WriteDenseArrayFile(const std::filesystem::path &output, const DenseArrayVolume &volume,
                         const ProcessingOptions &processing = {});

// Affine maps must be finite, nonsingular/conditioned, have voxel-axis lengths
// [1e-6,1e6], and resolve the represented region in world space.
// The absolute determinant must also exceed 3e-15 for OpenVDB's inverse-map
// construction. Metadata diagnostics are limited to 4096 keys/1MiB total bytes.
// Cell/leaf/node counts, encoded input, metadata, public/native arrays and output
// scratch are bounded. VDB output uses ZIP without active-mask compression to
// preserve inactive signed-zero bits.
// Custom metadata is intentionally not serialized; diagnostics list its keys.
// Dense values/background must all be finite. Generation options and gridName
// in ProcessingOptions are ignored: the volume supplies name/type/transform.
// Native-path VDB inputs are privately snapshotted before narrow-name upstream
// reads; the encoded bytes remain charged to the shared budget.
// One worker; seekable exclusive VDB output and exclusive array output. Input
// files are never overwritten; failures leave no newly created partial output.
// Every API reports ENABLE_OPENVDB=ON first when the optional feature is OFF.
} // namespace container::geometry::vdb
