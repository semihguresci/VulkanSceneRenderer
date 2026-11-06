#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace container::geometry::vdb {

enum class NanoVdbCodec { None, Zip, Blosc };
enum class NanoVdbChecksum { Disabled, Header, Full };
struct NanoVdbCapabilities {
  bool nativeIo{}, openVdbConversion{}, zip{}, blosc{};
};
struct NanoVdbLimits {
  uint64_t maxInputBytes{512ull * 1024 * 1024};
  uint64_t maxNativeBytes{512ull * 1024 * 1024};
  uint64_t maxWorkspaceBytes{1024ull * 1024 * 1024};
  uint64_t maxActiveVoxels{32ull * 1024 * 1024};
  uint64_t maxStoredLeafVoxels{32ull * 1024 * 1024};
  uint64_t maxNodes{1'000'000}, maxWork{250'000'000};
  uint32_t maxGrids{256}, maxNameBytes{4096};
  uint32_t maxMetadataBytes{1024 *
                            1024}; // Aggregate Container metadata extensions.
};
struct NanoVdbReadOptions {
  // Empty selects every grid; otherwise a unique name, name:<literal>, or
  // index:<zero-based file ordinal>. Names need not be unique in native files.
  std::string inputGrid;
  bool verifyChecksums{true}, requireFullChecksum{false};
};
struct NanoVdbWriteOptions {
  NanoVdbCodec codec{NanoVdbCodec::None};
  NanoVdbChecksum checksum{NanoVdbChecksum::Full};
  bool raw{false}; // Raw buffers cannot carry ZIP/BLOSC compression.
  bool allowFloatMapRounding{
      false}; // VDB conversion's derived Float map cache.
};
struct NanoVdbGridInfo {
  uint32_t index{};
  std::string name, valueType, gridClass, version;
  uint64_t nativeBytes{}, encodedBytes{}, activeVoxels{}, storedLeafVoxels{};
  uint64_t containerMetadataBytes{};
  std::array<uint32_t, 3> nodeCounts{}; // leaf, lower, upper
  NanoVdbCodec codec{NanoVdbCodec::None};
  NanoVdbChecksum checksum{NanoVdbChecksum::Disabled};
  bool supported{}, structureValidated{}, checksumModeKnown{}, checksumVerified{},
      checksumCoversAllData{},
      containerMetadata{};
};
struct NanoVdbFileInfo {
  uint64_t encodedBytes{}, nativeBytes{}, work{};
  bool raw{};
  std::vector<NanoVdbGridInfo> grids;
  std::vector<std::string> diagnostics;
};
struct NanoVdbArchiveData;
// Immutable, aligned, owned CPU payloads. Only the checked reader/converter can
// construct an archive; there is no unchecked public raw-byte entry point.
class NanoVdbArchive {
public:
  NanoVdbArchive() = default;
  [[nodiscard]] const NanoVdbFileInfo &info() const;
  [[nodiscard]] bool empty() const noexcept { return !data_; }

private:
  std::shared_ptr<const NanoVdbArchiveData> data_;
  explicit NanoVdbArchive(std::shared_ptr<const NanoVdbArchiveData> data)
      : data_(std::move(data)) {}
  friend NanoVdbArchive ReadNanoVdbFile(const std::filesystem::path &,
                                        const NanoVdbReadOptions &,
                                        const NanoVdbLimits &);
  friend NanoVdbFileInfo WriteNanoVdbFile(const NanoVdbArchive &,
                                          const std::filesystem::path &,
                                          const NanoVdbWriteOptions &,
                                          const NanoVdbLimits &);
  friend NanoVdbFileInfo WriteNanoVdbFromVdb(const std::filesystem::path &,
                                             const std::filesystem::path &,
                                             const std::string &,
                                             const NanoVdbWriteOptions &,
                                             const NanoVdbLimits &);
  friend NanoVdbFileInfo WriteVdbFromNanoVdb(const std::filesystem::path &,
                                             const std::filesystem::path &,
                                             const NanoVdbReadOptions &, bool,
                                             const NanoVdbLimits &);
  friend NanoVdbFileInfo RewriteNanoVdbChecksums(const std::filesystem::path &,
                                                 const std::filesystem::path &,
                                                 NanoVdbChecksum,
                                                 const NanoVdbLimits &);
};

[[nodiscard]] NanoVdbCapabilities NanoVdbInterchangeCapabilities() noexcept;
// Little-endian NanoVDB ABI32.6/32.7 raw buffers or segmented files. Catalog
// inspection checks every fixed header/name/encoded chunk and all aggregate
// limits BEFORE native allocation/decompression. It does not claim that payload
// topology or CRC is valid; those require Read/Validate. Unknown native types
// remain visible with supported=false, never silently converted.
[[nodiscard]] NanoVdbFileInfo InspectNanoVdbFile(const std::filesystem::path &,
                                                 const NanoVdbLimits & = {});
// Supported exact types: Float/Double/Int32/Int64/Vec3f/Vec3d/Boolean/Mask,
// ordinary fixed-size breadth-first 5/4/3 trees. Every offset/range/child mask,
// ownership/origin/count and finite stored value is checked before the upstream
// validator, CRC or converter dereferences tree pointers. Checksums are CRC32
// integrity checks, not authentication. Disabled/header-only checksums remain
// explicit. requireFullChecksum requires full mode AND coverage of every byte
// after the magic/checksum fields. Tiny blocked tails and legacy node-based
// checksums with blind data can have a coverage gap despite a full-mode flag;
// checksumCoversAllData and diagnostics identify that condition.
[[nodiscard]] NanoVdbArchive ReadNanoVdbFile(const std::filesystem::path &,
                                             const NanoVdbReadOptions & = {},
                                             const NanoVdbLimits & = {});
[[nodiscard]] NanoVdbFileInfo
ValidateNanoVdbFile(const std::filesystem::path &,
                    const NanoVdbReadOptions & = {},
                    const NanoVdbLimits & = {});
[[nodiscard]] NanoVdbFileInfo
WriteNanoVdbFile(const NanoVdbArchive &, const std::filesystem::path &output,
                 const NanoVdbWriteOptions & = {}, const NanoVdbLimits & = {});
// Explicit CRC repair ignores an old mismatch ONLY after complete structural
// validation. It cannot recover damaged values or authenticate their origin.
// Structural corruption still fails; diagnostics identify repaired mismatches.
[[nodiscard]] NanoVdbFileInfo RewriteNanoVdbChecksums(
    const std::filesystem::path &input, const std::filesystem::path &output,
    NanoVdbChecksum = NanoVdbChecksum::Full, const NanoVdbLimits & = {});
// Empty VDB selector converts every grid, rejecting unsupported grids instead
// of producing a partial archive. VDB selectors use existing descriptor: rules.
// Native values, inactive values/states, affine Double map, name/class and full
// raw metadata are retained. A versioned private Unknown blind-data extension
// preserves OpenVDB metadata/frame without changing the native tree ABI. Native
// consumers can ignore this extension. Float map fields are derived caches;
// original Double transform entries must be retained exactly. Nonlinear or
// numerically invalid maps, nonfinite values and unsupported type/classes fail.
[[nodiscard]] NanoVdbFileInfo WriteNanoVdbFromVdb(
    const std::filesystem::path &input, const std::filesystem::path &output,
    const std::string &inputGrid = {}, const NanoVdbWriteOptions & = {},
    const NanoVdbLimits & = {});
// Unknown third-party blind payloads and vector fields without preserved VDB
// frame metadata require explicit allowMetadataLoss. Every omission/default is
// named in diagnostics. This never interprets point/index/quantized formats as
// ordinary scalar/vector grids. VDB output uses ZIP without active-mask pruning
// so inactive nonbackground values and signed zero are retained.
[[nodiscard]] NanoVdbFileInfo
WriteVdbFromNanoVdb(const std::filesystem::path &input,
                    const std::filesystem::path &output,
                    const NanoVdbReadOptions & = {},
                    bool allowMetadataLoss = false, const NanoVdbLimits & = {});
// Native I/O availability is independent of OpenVDB conversion availability.
// Required-feature failures precede options/path access. All writes are
// exclusive and failures remove only owned partial outputs. A private immutable
// input snapshot prevents checked file metadata from changing during reading.
// Encoded/native/catalog/validation/codec/reverse-tree/writer memory share one
// conservative maxWorkspaceBytes cap, including retained archive payloads.
// OpenVDB conversion uses a single-worker TBB arena. Native CRC may use its
// configured upstream worker backend. Budgets are logical reserves, not a
// process-RSS sandbox (worker stacks/provider internals are outside the cap).
// Quantized/half/index/point/Vec4/packed/UInt variants and non-breadth-first
// trees are intentionally unsupported. CUDA/device allocation is not required.
} // namespace container::geometry::vdb
