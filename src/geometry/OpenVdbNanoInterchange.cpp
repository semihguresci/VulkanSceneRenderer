#include "Container/geometry/OpenVdbNanoInterchange.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
#include <nanovdb/HostBuffer.h>
#include <nanovdb/io/IO.h>
#include <nanovdb/tools/GridChecksum.h>
#include "OpenVdbNanoChecksum.h"
#include <nanovdb/tools/GridValidator.h>
#if defined(CONTAINER_NANOVDB_ZIP) && CONTAINER_NANOVDB_ZIP
#include <zlib.h>
#endif
#if defined(CONTAINER_NANOVDB_BLOSC) && CONTAINER_NANOVDB_BLOSC
#include <blosc.h>
#endif
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/OpenVdbGridSelection.h"
#include "OpenVdbFileUtils.h"
#include <mutex>
#include "Container/geometry/detail/OpenVdbNanoCompatibility.h"
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/NanoToOpenVDB.h>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Count.h>
#include <sstream>
#include <tbb/task_arena.h>
#endif
#endif

namespace container::geometry::vdb {
struct NanoVdbArchiveData {
  NanoVdbFileInfo info;
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  std::vector<nanovdb::HostBuffer> buffers;
#endif
};
namespace {
[[noreturn]] void unavailable(bool conversion = false) {
  throw std::runtime_error(conversion
                               ? "OpenVDB/NanoVDB conversion is unavailable; "
                                 "enable ENABLE_OPENVDB and ENABLE_NANOVDB_IO."
                               : "NanoVDB interchange is unavailable; enable "
                                 "portable NanoVDB file tools (ENABLE_NANOVDB_IO).");
}
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
namespace nv = nanovdb;
constexpr uint64_t Fixed = 4ull * 1024 * 1024;
constexpr const char *ExtensionName = "container:openvdb_metadata_v1";
constexpr uint64_t ExtensionMagic =
    0x4154454d42445643ull; // CVDBMETA, little endian
void require(bool condition, const char *error) {
  if (!condition)
    throw std::invalid_argument(error);
}
uint64_t add(uint64_t a, uint64_t b) {
  require(b <= UINT64_MAX - a, "NanoVDB size/count addition overflows");
  return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  require(!a || b <= UINT64_MAX / a, "NanoVDB size/count product overflows");
  return a * b;
}
void workspace(long double bytes, const NanoVdbLimits &l) {
  require(std::isfinite(bytes) && bytes <= l.maxWorkspaceBytes,
          "NanoVDB aggregate source/native/codec/metadata/workspace budget "
          "exceeded");
}
void limits(const NanoVdbLimits &l) {
  require(std::endian::native == std::endian::little,
          "NanoVDB interchange requires a little-endian host");
  require(l.maxInputBytes && l.maxInputBytes <= (1ull << 30) &&
              l.maxNativeBytes && l.maxNativeBytes <= (1ull << 30) &&
              l.maxWorkspaceBytes && l.maxWorkspaceBytes <= (16ull << 30) &&
              l.maxGrids && l.maxGrids <= 256 && l.maxNameBytes &&
              l.maxNameBytes <= 65536 && l.maxMetadataBytes &&
              l.maxMetadataBytes <= (16u << 20) && l.maxNodes &&
              l.maxNodes <= 1'000'000 && l.maxWork &&
              l.maxWork <= 4'000'000'000ull && l.maxActiveVoxels &&
              l.maxStoredLeafVoxels,
          "invalid NanoVDB limits (encoded/native<=1GiB, workspace<=16GiB, "
          "grids<=256, nodes<=1000000)");
}
void work(uint64_t &n, uint64_t amount, const NanoVdbLimits &l) {
  n = add(n, amount);
  require(n <= l.maxWork, "NanoVDB validation/conversion work budget exceeded");
}
void range(uint64_t offset, uint64_t size, uint64_t end) {
  require(offset <= end && size <= end - offset,
          "NanoVDB record/offset exceeds its bounded buffer");
}
uint64_t relative(uint64_t base, int64_t offset, uint64_t size, uint64_t end) {
  require(offset >= 0,
          "NanoVDB canonical layout contains a backward/negative offset");
  const auto out = add(base, uint64_t(offset));
  range(out, size, end);
  return out;
}
NanoVdbCodec codec(nv::io::Codec c) {
  require(c >= nv::io::Codec::NONE && c < nv::io::Codec::End,
          "invalid NanoVDB file codec");
  return static_cast<NanoVdbCodec>(c);
}
nv::CheckMode mode(NanoVdbChecksum c) {
  require(c >= NanoVdbChecksum::Disabled && c <= NanoVdbChecksum::Full,
          "invalid NanoVDB checksum mode");
  return c == NanoVdbChecksum::Disabled ? nv::CheckMode::Disable
         : c == NanoVdbChecksum::Header ? nv::CheckMode::Half
                                        : nv::CheckMode::Full;
}
NanoVdbChecksum checksum(const nv::Checksum &c) {
  return c.isEmpty()  ? NanoVdbChecksum::Disabled
         : c.isHalf() ? NanoVdbChecksum::Header
                      : NanoVdbChecksum::Full;
}
void codecAvailable(NanoVdbCodec c) {
  const auto caps = NanoVdbInterchangeCapabilities();
  require(c >= NanoVdbCodec::None && c <= NanoVdbCodec::Blosc,
          "invalid NanoVDB codec selection");
  require(c != NanoVdbCodec::Zip || caps.zip,
          "NanoVDB ZIP codec is unavailable; link an enabled zlib provider");
  require(c != NanoVdbCodec::Blosc || caps.blosc,
          "NanoVDB BLOSC codec is unavailable; link an enabled Blosc provider");
}
void version(const nv::Version &v) {
  require(
      v.getMajor() == 32 && v.getMinor() >= 6 && v.getMinor() <= 7,
      "unsupported NanoVDB version; this reader supports ABI32.6/32.7 only");
}
std::string versionText(const nv::Version &v) {
  return std::to_string(v.getMajor()) + "." + std::to_string(v.getMinor()) +
         "." + std::to_string(v.getPatch());
}
bool supported(nv::GridType t, nv::GridClass c) {
  const bool type = t == nv::GridType::Float || t == nv::GridType::Double ||
                    t == nv::GridType::Int32 || t == nv::GridType::Int64 ||
                    t == nv::GridType::Vec3f || t == nv::GridType::Vec3d ||
                    t == nv::GridType::Boolean || t == nv::GridType::Mask;
  return type && (c <= nv::GridClass::Staggered ||
                  (c == nv::GridClass::Topology && t == nv::GridType::Mask));
}
struct Temporary {
  std::filesystem::path directory, file;
  explicit Temporary(const char *suffix) {
    static std::atomic_uint64_t serial{};
    for (int i = 0; i < 64; ++i) {
      const auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-nano-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        file = directory / suffix;
        return;
      }
    }
    throw std::runtime_error(
        "cannot create private NanoVDB temporary directory");
  }
  ~Temporary() {
    std::error_code e;
    std::filesystem::remove(file, e);
    std::filesystem::remove(directory, e);
  }
};
void copyExclusive(const std::filesystem::path &from,
                   const std::filesystem::path &to, uint64_t bytes) {
  std::ifstream in(from, std::ios::binary);
  std::ofstream out;
  bool created = false;
  try {
    require(bool(in), "cannot open NanoVDB source copy");
    out.open(to, std::ios::binary | std::ios::out | std::ios::noreplace);
    created = out.is_open();
    require(created, "output already exists or cannot be exclusively created");
    out.exceptions(std::ios::failbit | std::ios::badbit);
    std::array<char, 64 * 1024> buffer{};
    for (auto left = bytes; left;) {
      const auto n = std::streamsize(std::min<uint64_t>(left, buffer.size()));
      in.read(buffer.data(), n);
      require(in.gcount() == n,
              "source changed or truncated during NanoVDB copy");
      out.write(buffer.data(), n);
      left -= uint64_t(n);
    }
    require(!in.bad() && in.peek() == std::char_traits<char>::eof(),
            "source size changed during NanoVDB copy");
    out.flush();
    out.close();
  } catch (...) {
    out.exceptions(std::ios::goodbit);
    if (out.is_open())
      out.close();
    if (created) {
      std::error_code e;
      std::filesystem::remove(to, e);
    }
    throw;
  }
}
struct Snapshot {
  Temporary temporary{"source.bin"};
  uint64_t size{};
  Snapshot(const std::filesystem::path &input, const NanoVdbLimits &l) {
    require(std::filesystem::is_regular_file(input),
            "NanoVDB input is not a regular file");
    size = std::filesystem::file_size(input);
    require(size && size <= l.maxInputBytes,
            "NanoVDB encoded input exceeds maxInputBytes or is empty");
    workspace(static_cast<long double>(size) + Fixed, l);
    copyExclusive(input, temporary.file, size);
  }
};
void readAt(std::ifstream &f, uint64_t at, void *data, size_t bytes,
            uint64_t end) {
  range(at, bytes, end);
  f.clear();
  f.seekg(std::streamoff(at));
  f.read(static_cast<char *>(data), std::streamsize(bytes));
  require(bool(f), "truncated NanoVDB fixed record/payload");
}
template <class T> T record(std::ifstream &f, uint64_t at, uint64_t end) {
  T value{};
  readAt(f, at, &value, sizeof(value), end);
  return value;
}
std::string nameAt(std::ifstream &f, uint64_t at, uint64_t size, uint64_t end,
                   const NanoVdbLimits &l) {
  require(size && size <= uint64_t(l.maxNameBytes) + 1,
          "NanoVDB grid name exceeds bounded name length");
  std::string out(size_t(size), '\0');
  readAt(f, at, out.data(), out.size(), end);
  require(out.back() == '\0' && out.find('\0') == out.size() - 1,
          "NanoVDB name is not exactly NUL-terminated");
  out.pop_back();
  return out;
}
struct Entry {
  NanoVdbGridInfo info;
  uint64_t payload{};
  nv::GridType type{};
  nv::GridClass gridClass{};
  std::optional<nv::io::FileMetaData> metadata;
};
struct Catalog {
  NanoVdbFileInfo info;
  std::vector<Entry> entries;
  uint64_t reserve{};
};
void counts(const std::array<uint32_t, 3> &n, uint64_t active,
            const NanoVdbLimits &l) {
  require(add(add(add(n[0], n[1]), n[2]), 1) <= l.maxNodes &&
              mul(n[0], 512) <= l.maxStoredLeafVoxels &&
              active <= l.maxActiveVoxels,
          "NanoVDB recorded node/leaf-slot/active-voxel counts exceed limits");
}
void addEntry(Catalog &c, Entry e, const NanoVdbLimits &l) {
  require(c.entries.size() < l.maxGrids, "NanoVDB file exceeds maxGrids");
  require(e.info.nativeBytes >= sizeof(nv::GridData) + sizeof(nv::TreeData) &&
              e.info.nativeBytes % 32 == 0,
          "NanoVDB native grid size is truncated or misaligned");
  counts(e.info.nodeCounts, e.info.activeVoxels, l);
  c.info.nativeBytes = add(c.info.nativeBytes, e.info.nativeBytes);
  require(c.info.nativeBytes <= l.maxNativeBytes,
          "NanoVDB aggregate native bytes exceed maxNativeBytes");
  e.info.index = uint32_t(c.entries.size());
  e.info.supported = supported(e.type, e.gridClass);
  char text[64];
  e.info.valueType = nv::toStr(text, e.type);
  e.info.gridClass = nv::toStr(text, e.gridClass);
  c.reserve = add(c.reserve, add(mul(e.info.name.size(), 8), 2048));
  workspace(static_cast<long double>(c.info.encodedBytes) + c.reserve + Fixed,
            l);
  c.info.grids.push_back(e.info);
  c.entries.push_back(std::move(e));
}
Catalog catalog(const Snapshot &s, const NanoVdbLimits &l) {
  Catalog c;
  c.info.encodedBytes = s.size;
  std::ifstream f(s.temporary.file, std::ios::binary);
  const auto magic = record<uint64_t>(f, 0, s.size);
  bool raw = magic == NANOVDB_MAGIC_GRID;
  if (magic == NANOVDB_MAGIC_NUMB && s.size >= sizeof(nv::GridData)) {
    const auto h = record<nv::GridData>(f, 0, s.size);
    raw = h.mData2 == NANOVDB_MAGIC_GRID ||
          (h.mGridSize >= 736 && h.mGridSize <= s.size && h.mGridCount > 0 &&
           h.mGridIndex < h.mGridCount);
  }
  c.info.raw = raw;
  uint64_t offset = 0;
  if (raw) {
    uint32_t expected = 0, total = 0;
    while (offset < s.size) {
      const auto h = record<nv::GridData>(f, offset, s.size);
      version(h.mVersion);
      require(
          (h.mMagic == NANOVDB_MAGIC_NUMB || h.mMagic == NANOVDB_MAGIC_GRID) &&
              h.mGridCount && h.mGridCount <= l.maxGrids &&
              h.mGridIndex < h.mGridCount,
          "invalid NanoVDB raw grid header");
      if (!expected)
        total = h.mGridCount;
      require(h.mGridCount == total && h.mGridIndex == expected,
              "NanoVDB raw grid indices/counts are inconsistent");
      range(offset, h.mGridSize, s.size);
      const auto t = record<nv::TreeData>(f, add(offset, sizeof(h)), s.size);
      Entry e;
      e.payload = offset;
      e.type = h.mGridType;
      e.gridClass = h.mGridClass;
      e.info.nativeBytes = e.info.encodedBytes = h.mGridSize;
      e.info.version = versionText(h.mVersion);
      e.info.checksum = checksum(h.mChecksum);
      e.info.checksumModeKnown = true;
      e.info.nodeCounts = {t.mNodeCount[0], t.mNodeCount[1], t.mNodeCount[2]};
      e.info.activeVoxels = t.mVoxelCount;
      if (h.mFlags.isMaskOn(nv::GridFlags::HasLongGridName)) {
        require(h.mBlindMetadataCount && h.mBlindMetadataCount <= 64,
                "invalid NanoVDB long-name metadata count");
        const auto meta =
            relative(0, h.mBlindMetadataOffset,
                     mul(h.mBlindMetadataCount, sizeof(nv::GridBlindMetaData)),
                     h.mGridSize);
        bool found = false;
        for (uint32_t i = 0; i < h.mBlindMetadataCount; ++i) {
          std::array<std::byte, sizeof(nv::GridBlindMetaData)> bytes;
          readAt(f, offset + meta + i * bytes.size(), bytes.data(),
                 bytes.size(), s.size);
          alignas(32) std::array<std::byte, sizeof(nv::GridBlindMetaData)>
              aligned = bytes;
          const auto &b =
              *reinterpret_cast<const nv::GridBlindMetaData *>(aligned.data());
          if (b.mDataClass != nv::GridBlindDataClass::GridName)
            continue;
          require(!found && b.mValueSize == 1,
                  "ambiguous NanoVDB long grid name");
          found = true;
          const auto p = relative(meta + i * bytes.size(), b.mDataOffset,
                                  b.mValueCount, h.mGridSize);
          e.info.name = nameAt(f, offset + p, b.mValueCount, s.size, l);
        }
        require(found, "NanoVDB long grid name is missing");
      } else {
        const auto end = static_cast<const char *>(
            std::memchr(h.mGridName, 0, sizeof(h.mGridName)));
        require(end, "NanoVDB inline grid name is not terminated");
        e.info.name.assign(h.mGridName, end);
        require(e.info.name.size() <= l.maxNameBytes,
                "NanoVDB name exceeds maxNameBytes");
      }
      addEntry(c, std::move(e), l);
      offset = add(offset, h.mGridSize);
      if (++expected == total)
        expected = 0;
    }
    require(expected == 0, "NanoVDB raw group is missing grids");
  } else {
    while (offset < s.size) {
      const auto h = record<nv::io::FileHeader>(f, offset, s.size);
      version(h.version);
      require(h.isValid() && h.gridCount && h.gridCount <= l.maxGrids,
              "invalid NanoVDB segment header/count");
      const auto segmentCodec = codec(h.codec);
      offset = add(offset, sizeof(h));
      std::vector<Entry> entries;
      entries.reserve(h.gridCount);
      for (uint32_t i = 0; i < h.gridCount; ++i) {
        const auto m = record<nv::io::FileMetaData>(f, offset, s.size);
        version(m.version);
        offset = add(offset, sizeof(m));
        require(m.gridType > nv::GridType::Unknown &&
                    m.gridType < nv::GridType::End &&
                    m.gridClass < nv::GridClass::End && m.nodeCount[3] == 1 &&
                    codec(m.codec) == segmentCodec,
                "invalid NanoVDB segment grid metadata");
        Entry e;
        e.type = m.gridType;
        e.gridClass = m.gridClass;
        e.metadata = m;
        e.info.name = nameAt(f, offset, m.nameSize, s.size, l);
        offset = add(offset, m.nameSize);
        require(m.nameKey == nv::io::stringHash(e.info.name),
                "NanoVDB name/hash mismatch");
        e.info.nativeBytes = m.gridSize;
        e.info.encodedBytes = m.fileSize;
        e.info.activeVoxels = m.voxelCount;
        e.info.nodeCounts = {m.nodeCount[0], m.nodeCount[1], m.nodeCount[2]};
        e.info.codec = segmentCodec;
        e.info.version = versionText(m.version);
        entries.push_back(std::move(e));
      }
      for (auto &e : entries) {
        e.payload = offset;
        range(offset, e.info.encodedBytes, s.size);
        if (e.info.codec == NanoVdbCodec::None)
          require(e.info.encodedBytes == e.info.nativeBytes,
                  "NanoVDB NONE encoded/native sizes disagree");
        else {
          require(e.info.encodedBytes > 8,
                  "NanoVDB compressed grid has no bounded chunk");
          const auto chunk = record<uint64_t>(f, offset, s.size);
          require(
              chunk == e.info.encodedBytes - 8 &&
                  chunk <=
                      add(add(e.info.nativeBytes, e.info.nativeBytes / 512),
                          65536),
              "NanoVDB compressed chunk size disagrees with bounded metadata");
        }
        offset = add(offset, e.info.encodedBytes);
        addEntry(c, std::move(e), l);
      }
    }
  }
  require(!c.entries.empty() && offset == s.size,
          "NanoVDB file is empty or has trailing/truncated bytes");
  uint64_t nodes = c.entries.size(), slots = 0, active = 0;
  for (const auto &e : c.entries) {
    for (auto n : e.info.nodeCounts)
      nodes = add(nodes, n);
    slots = add(slots, mul(e.info.nodeCounts[0], 512));
    active = add(active, e.info.activeVoxels);
  }
  require(nodes <= l.maxNodes && slots <= l.maxStoredLeafVoxels &&
              active <= l.maxActiveVoxels,
          "NanoVDB aggregate topology exceeds shared limits");
  return c;
}
std::vector<size_t> selected(const Catalog &c, const std::string &selection) {
  std::vector<size_t> out;
  if (selection.starts_with("index:")) {
    const auto text = selection.substr(6);
    require(!text.empty() && std::all_of(text.begin(), text.end(),
                                         [](unsigned char x) {
                                           return x >= '0' && x <= '9';
                                         }),
            "NanoVDB index selector requires decimal digits");
    size_t consumed = 0;
    const auto n = std::stoull(text, &consumed);
    require(n < c.entries.size(), "NanoVDB index selector is out of range");
    out.push_back(size_t(n));
  } else {
    const auto name =
        selection.starts_with("name:") ? selection.substr(5) : selection;
    for (size_t i = 0; i < c.entries.size(); ++i)
      if (selection.empty() || name == c.entries[i].info.name)
        out.push_back(i);
    require(!out.empty(), "no matching NanoVDB grid");
    require(selection.empty() || out.size() == 1,
            "NanoVDB grid name is ambiguous; use index:<ordinal>");
  }
  return out;
}
void decode(std::ifstream &f, const Entry &e, nv::HostBuffer &buffer,
            uint64_t end) {
  codecAvailable(e.info.codec);
  if (e.info.codec == NanoVdbCodec::None) {
    readAt(f, e.payload, buffer.data(), size_t(buffer.size()), end);
    return;
  }
  const uint64_t n = e.info.encodedBytes - 8;
  std::vector<unsigned char> data(size_t(n), 0);
  readAt(f, e.payload + 8, data.data(), data.size(), end);
  if (e.info.codec == NanoVdbCodec::Zip) {
#if defined(CONTAINER_NANOVDB_ZIP) && CONTAINER_NANOVDB_ZIP
    z_stream stream{};
    require(inflateInit(&stream) == Z_OK,
            "cannot initialize bounded ZIP decoder");
    stream.next_in = data.data();
    stream.avail_in = uInt(data.size());
    stream.next_out = static_cast<Bytef *>(buffer.data());
    stream.avail_out = uInt(buffer.size());
    const int status = inflate(&stream, Z_FINISH);
    const auto read = stream.total_in, written = stream.total_out;
    inflateEnd(&stream);
    require(status == Z_STREAM_END && read == data.size() &&
                written == buffer.size(),
            "NanoVDB ZIP payload is invalid, trailing or has the wrong decoded "
            "size");
    return;
#endif
  } else {
#if defined(CONTAINER_NANOVDB_BLOSC) && CONTAINER_NANOVDB_BLOSC
    size_t decoded = 0;
    require(data.size() >= BLOSC_MIN_HEADER_LENGTH &&
                blosc_cbuffer_validate(data.data(), data.size(), &decoded) == 0 &&
                decoded == buffer.size(),
            "NanoVDB BLOSC header/decoded size is invalid");
    require(blosc_decompress_ctx(data.data(), buffer.data(),
                                 size_t(buffer.size()),
                                 1) == int(buffer.size()),
            "NanoVDB BLOSC decompression failed");
    return;
#endif
  }
  throw std::logic_error("unreachable unavailable NanoVDB codec");
}
template <class T> void finiteValue(const T &v) {
  if constexpr (std::is_floating_point_v<T>)
    require(std::isfinite(v), "NanoVDB stored value/background is nonfinite");
  else if constexpr (!std::is_arithmetic_v<T>)
    for (int k = 0; k < 3; ++k)
      require(std::isfinite(v[k]),
              "NanoVDB vector value/background is nonfinite");
}
template <class T> void fieldValue(const T *p) {
  if constexpr (std::is_same_v<T, bool>)
    require(*reinterpret_cast<const uint8_t *>(p) <= 1,
            "NanoVDB boolean field has an invalid representation");
  finiteValue(*p);
}
void mapValid(const nv::Map &m) {
  require(m.mTaperD == 1 && m.mTaperF == 1,
          "NanoVDB nonlinear/tapered maps are unsupported");
  for (int k = 0; k < 9; ++k)
    require(std::isfinite(m.mMatD[k]) && std::isfinite(m.mInvMatD[k]) &&
                std::isfinite(m.mMatF[k]) && std::isfinite(m.mInvMatF[k]),
            "NanoVDB affine matrix/inverse is nonfinite");
  for (int k = 0; k < 3; ++k)
    require(std::isfinite(m.mVecD[k]) && std::isfinite(m.mVecF[k]),
            "NanoVDB translation is nonfinite");
  std::array<nv::Vec3d, 3> axes;
  for (int k = 0; k < 3; ++k) {
    nv::Vec3d e(0);
    e[k] = 1;
    axes[k] = m.applyJacobian(e);
    const double size = std::hypot(axes[k][0], axes[k][1], axes[k][2]);
    require(size >= 1e-6 && size <= 1e6,
            "NanoVDB affine voxel axes must span [1e-6,1e6]");
    axes[k] /= size;
    const auto inverse = m.applyInverseJacobian(m.applyJacobian(e));
    for (int j = 0; j < 3; ++j)
      require(std::isfinite(inverse[j]) && std::abs(inverse[j] - e[j]) < 1e-8 &&
                  std::abs(m.mInvMatD[k * 3 + j]) <= 1e8,
              "NanoVDB affine inverse is singular/inconsistent");
  }
  require(std::abs(axes[0].dot(axes[1].cross(axes[2]))) >= 1e-8,
          "NanoVDB affine map is insufficiently conditioned");
}
struct RawMetadata {
  std::string name, type;
  std::vector<uint8_t> value;
  bool file{};
};
uint32_t u32(std::span<const uint8_t> data, size_t &p) {
  range(p, 4, data.size());
  uint32_t v;
  std::memcpy(&v, data.data() + p, 4);
  p += 4;
  return v;
}
uint64_t u64(std::span<const uint8_t> data, size_t &p) {
  range(p, 8, data.size());
  uint64_t v;
  std::memcpy(&v, data.data() + p, 8);
  p += 8;
  return v;
}
std::vector<RawMetadata> parseMetadata(std::span<const uint8_t> data,
                                       const NanoVdbLimits &l) {
  require(data.size() <= l.maxMetadataBytes,
          "NanoVDB custom metadata extension exceeds maxMetadataBytes");
  size_t p = 0;
  require(u64(data, p) == ExtensionMagic && u32(data, p) == 1 &&
              u32(data, p) == 0x01020304,
          "NanoVDB reserved metadata extension has a magic/version/endianness "
          "collision");
  const auto count = u32(data, p);
  require(count <= 4096, "NanoVDB custom metadata entry count exceeds limit");
  std::vector<RawMetadata> result;
  result.reserve(count);
  std::set<std::pair<bool, std::string>> keys;
  for (uint32_t i = 0; i < count; ++i) {
    const auto scope = u32(data, p);
    require(scope <= 1, "NanoVDB custom metadata scope is invalid");
    const auto n = u32(data, p), t = u32(data, p), v = u32(data, p);
    require(n && n <= l.maxNameBytes && t && t <= l.maxNameBytes,
            "NanoVDB custom metadata name/type length is invalid");
    range(p, add(add(n, t), v), data.size());
    RawMetadata m;
    m.file = scope != 0;
    m.name.assign(reinterpret_cast<const char *>(data.data() + p), n);
    p += n;
    m.type.assign(reinterpret_cast<const char *>(data.data() + p), t);
    p += t;
    m.value.assign(data.begin() + p, data.begin() + p + v);
    p += v;
    require(m.name.find('\0') == std::string::npos &&
                m.type.find('\0') == std::string::npos,
            "NanoVDB custom metadata name/type contains NUL");
    require(keys.insert({m.file, m.name}).second,
            "NanoVDB custom metadata contains duplicate keys");
    result.push_back(std::move(m));
  }
  require(p == data.size(),
          "NanoVDB custom metadata extension has trailing bytes");
  return result;
}
struct Blind {
  std::string name;
  uint64_t offset{}, bytes{};
  nv::GridBlindDataClass kind{};
};
std::vector<Blind> blind(const nv::GridData &g, uint64_t end,
                         const NanoVdbLimits &l, uint64_t nodeEnd,
                         NanoVdbGridInfo &info) {
  require(g.mBlindMetadataCount <= 64,
          "NanoVDB blind metadata count exceeds64");
  const uint64_t base =
      relative(0, g.mBlindMetadataOffset,
               mul(g.mBlindMetadataCount, sizeof(nv::GridBlindMetaData)), end);
  require(base >= nodeEnd && base % 32 == 0,
          "NanoVDB blind metadata overlaps tree nodes");
  uint64_t next = base + g.mBlindMetadataCount * sizeof(nv::GridBlindMetaData);
  std::vector<Blind> result;
  bool extension = false;
  bool longName = false;
  for (uint32_t i = 0; i < g.mBlindMetadataCount; ++i) {
    const auto at = base + i * sizeof(nv::GridBlindMetaData);
    const auto &b = *reinterpret_cast<const nv::GridBlindMetaData *>(
        reinterpret_cast<const uint8_t *>(&g) + at);
    const char *zero =
        static_cast<const char *>(std::memchr(b.mName, 0, sizeof(b.mName)));
    require(zero, "NanoVDB blind name is unterminated");
    const std::string name(b.mName, zero);
    require(b.mDataClass < nv::GridBlindDataClass::End &&
                b.mSemantic < nv::GridBlindDataSemantic::End &&
                b.mDataType < nv::GridType::End,
            "NanoVDB blind data tags are invalid");
    const auto bytes = mul(b.mValueCount, b.mValueSize),
               offset = relative(at, b.mDataOffset, bytes, end);
    require(offset % 32 == 0 && offset >= next,
            "NanoVDB blind payload overlaps metadata or another payload");
    next = add(offset, add(bytes, 31) & ~uint64_t(31));
    range(offset, next - offset, end);
    if (b.mDataClass == nv::GridBlindDataClass::GridName) {
      require(!longName && g.mFlags.isMaskOn(nv::GridFlags::HasLongGridName) &&
                  b.mValueSize == 1 && bytes &&
                  bytes <= uint64_t(l.maxNameBytes) + 1,
              "NanoVDB long-name payload has invalid tags/count/flags");
      const auto *text = reinterpret_cast<const char *>(&g) + offset;
      const auto *zero = static_cast<const char *>(std::memchr(text, 0, size_t(bytes)));
      require(zero && zero == text + bytes - 1 &&
                  std::string_view(text, size_t(bytes - 1)) == info.name,
              "NanoVDB long-name payload is unterminated or disagrees with catalog");
      longName = true;
    }
    if (name == ExtensionName) {
      require(!extension && b.mDataType == nv::GridType::Unknown &&
                  b.mDataClass == nv::GridBlindDataClass::Unknown &&
                  b.mSemantic == nv::GridBlindDataSemantic::Unknown &&
                  b.mValueSize == 1,
              "NanoVDB reserved metadata extension has incompatible tags");
      (void)parseMetadata(
          {reinterpret_cast<const uint8_t *>(&g) + offset, size_t(bytes)}, l);
      extension = true;
      info.containerMetadataBytes = bytes;
    }
    result.push_back({name, offset, bytes, b.mDataClass});
  }
  require(next == end,
          "NanoVDB trailing bytes are not owned by tree/blind data");
  require(longName == g.mFlags.isMaskOn(nv::GridFlags::HasLongGridName),
          "NanoVDB long-name flag is missing its bounded payload");
  info.containerMetadata = extension;
  return result;
}
template <class T>
void validateTyped(const nv::GridData &data, NanoVdbGridInfo &info,
                   const NanoVdbLimits &l, uint64_t &amount) {
  using Root = nv::NanoRoot<T>;
  using Upper = nv::NanoUpper<T>;
  using Lower = nv::NanoLower<T>;
  using Leaf = nv::NanoLeaf<T>;
  using RD = typename Root::DataType;
  const auto end = data.mGridSize;
  const auto *bytes = reinterpret_cast<const uint8_t *>(&data);
  require(data.mFlags.isMaskOn(nv::GridFlags::IsBreadthFirst),
          "NanoVDB non-breadth-first trees are unsupported");
  const auto &tree =
      *reinterpret_cast<const nv::TreeData *>(bytes + sizeof(nv::GridData));
  require(tree.mNodeOffset[3] == int64_t(sizeof(nv::TreeData)),
          "NanoVDB root must immediately follow its tree header");
  uint64_t rootAt = sizeof(nv::GridData) + sizeof(nv::TreeData);
  range(rootAt, sizeof(RD), end);
  const auto &root = *reinterpret_cast<const RD *>(bytes + rootAt);
  require(root.mTableSize <= l.maxNodes,
          "NanoVDB root tile count exceeds node limit");
  uint64_t cursor = add(
      rootAt, add(sizeof(RD), mul(root.mTableSize, sizeof(typename RD::Tile))));
  range(rootAt, cursor - rootAt, end);
  const uint64_t upperAt = cursor;
  cursor = add(cursor, mul(tree.mNodeCount[2], sizeof(Upper)));
  const uint64_t lowerAt = cursor;
  cursor = add(cursor, mul(tree.mNodeCount[1], sizeof(Lower)));
  const uint64_t leafAt = cursor;
  cursor = add(cursor, mul(tree.mNodeCount[0], sizeof(Leaf)));
  range(0, cursor, end);
  const uint64_t starts[]{leafAt, lowerAt, upperAt};
  for (int k = 0; k < 3; ++k)
    require(
        tree.mNodeOffset[k] == (tree.mNodeCount[k]
                                    ? int64_t(starts[k] - sizeof(nv::GridData))
                                    : 0),
        "NanoVDB node ranges are not canonical, bounded breadth-first arrays");
  fieldValue(&root.mBackground);
  uint64_t i2 = 0, i1 = 0, i0 = 0, active = 0;
  std::array<uint64_t, 3> tiles{};
  std::set<uint64_t> rootKeys;
  auto leafCheck = [&](const Leaf &leaf, const nv::Coord &expected) {
    require(leaf.origin() == expected,
            "NanoVDB leaf origin disagrees with parent ownership");
    const auto *d = leaf.data();
    for (int k = 0; k < 3; ++k)
      require(d->mBBoxDif[k] <= 7, "NanoVDB leaf bbox extent exceeds its leaf");
    active = add(active, leaf.valueMask().countOn());
    work(amount, 512, l);
    for (uint32_t n = 0; n < 512; ++n)
      finiteValue(leaf.getValue(n));
  };
  auto internal = [&]<class Node, class Child>(
                      const Node &node, const nv::Coord &expected,
                      uint64_t target, uint64_t &childIndex, uint64_t count,
                      auto &&childCheck, int level) {
    const bool lostInactiveOrigin = node.origin() != expected;
    require(!lostInactiveOrigin || node.data()->mBBox.empty(),
            "NanoVDB internal origin disagrees with parent ownership");
    const auto previousActive = active;
    const auto previousTiles = tiles;
    const auto *d = node.data();
    work(amount, Node::SIZE, l);
    for (uint32_t n = 0; n < Node::SIZE; ++n) {
      if (d->mChildMask.isOn(n)) {
        require(!d->mValueMask.isOn(n) && childIndex < count,
                "NanoVDB child/value masks overlap or exceed node count");
        const auto expectedOffset = target + childIndex * sizeof(Child);
        const auto actual =
            relative(uint64_t(reinterpret_cast<const uint8_t *>(d) - bytes),
                     d->mTable[n].child, sizeof(Child), end);
        require(actual == expectedOffset,
                "NanoVDB child offset aliases, escapes or violates "
                "breadth-first ownership");
        // ABI32.7 GridStats replaces entirely inactive internal bboxes with a
        // generic empty box, losing origin(). Ownership still determines the
        // exact child origin; never use that invalid bbox for pointer traversal.
        const auto local = Node::OffsetToLocalCoord(n);
        const nv::Coord origin(
            int32_t(int64_t(expected[0]) + int64_t(local[0]) * Child::DIM),
            int32_t(int64_t(expected[1]) + int64_t(local[1]) * Child::DIM),
            int32_t(int64_t(expected[2]) + int64_t(local[2]) * Child::DIM));
        childCheck(*reinterpret_cast<const Child *>(bytes + actual), origin);
        ++childIndex;
      } else {
        fieldValue(&d->mTable[n].value);
        if (d->mValueMask.isOn(n))
          ++tiles[level];
      }
    }
    require(!lostInactiveOrigin ||
                (active == previousActive && tiles == previousTiles),
            "NanoVDB erased internal origin belongs to an active subtree");
  };
  auto lowerCheck = [&](const Lower &n, const nv::Coord &origin) {
    internal.template operator()<Lower, Leaf>(n, origin, leafAt, i0,
                                              tree.mNodeCount[0], leafCheck, 0);
  };
  auto upperCheck = [&](const Upper &n, const nv::Coord &origin) {
    internal.template operator()<Upper, Lower>(
        n, origin, lowerAt, i1, tree.mNodeCount[1], lowerCheck, 1);
  };
  work(amount, root.mTableSize, l);
  for (uint32_t n = 0; n < root.mTableSize; ++n) {
    const auto &tile = *root.tile(n);
    require(rootKeys.insert(tile.key).second &&
                RD::CoordToKey(tile.origin()) == tile.key,
            "NanoVDB root keys duplicate or contain unrepresentable bits");
    require(tile.state <= 1, "NanoVDB root tile state is invalid");
    if (tile.child) {
      require(!tile.state && i2 < tree.mNodeCount[2],
              "NanoVDB root child state/count is invalid");
      const auto actual = relative(rootAt, tile.child, sizeof(Upper), end);
      require(actual == upperAt + i2 * sizeof(Upper),
              "NanoVDB root child offset violates bounded ownership");
      upperCheck(*reinterpret_cast<const Upper *>(bytes + actual),
                 tile.origin());
      ++i2;
    } else {
      fieldValue(&tile.value);
      if (tile.state)
        ++tiles[2];
    }
  }
  require(i2 == tree.mNodeCount[2] && i1 == tree.mNodeCount[1] &&
              i0 == tree.mNodeCount[0],
          "NanoVDB node arrays contain unreachable nodes");
  for (int k = 0; k < 3; ++k) {
    require(tiles[k] == tree.mTileCount[k],
            "NanoVDB stored active tile counts disagree with masks");
    active = add(active, mul(tiles[k], uint64_t(1) << (k == 0   ? 9
                                                       : k == 1 ? 21
                                                                : 36)));
  }
  require(active == tree.mVoxelCount && active == info.activeVoxels,
          "NanoVDB active voxel count disagrees with its tree");
  (void)blind(data, end, l, cursor, info);
  // Only now are all pointers visited by the upstream validator proven safe.
  char error[256];
  // ABI32.7's full checker uses the next node array as an upper bound, even
  // when that array is absent. Valid tile-only trees then compare a real node
  // address against null. Our ownership/range/mask/count checks above cover the
  // entire tree independently; use the vendor header/root check in that case.
  const bool absentArray = (!tree.mNodeCount[0] && tree.mNodeCount[1]) ||
                           (!tree.mNodeCount[1] && tree.mNodeCount[2]);
  nv::tools::checkGrid(reinterpret_cast<const nv::NanoGrid<T> *>(&data), error,
                       absentArray ? nv::CheckMode::Half : nv::CheckMode::Full);
  if (*error)
    throw std::invalid_argument(std::string("NanoVDB upstream structural validator: ")+error);
}
void validateBuffer(nv::HostBuffer &buffer, const Entry &e,
                    NanoVdbGridInfo &info, const NanoVdbReadOptions &o,
                    const NanoVdbLimits &l, uint64_t &amount,
                    std::vector<std::string> &diagnostics) {
  const auto &h = *static_cast<const nv::GridData *>(buffer.data());
  version(h.mVersion);
  require((h.mMagic == NANOVDB_MAGIC_NUMB || h.mMagic == NANOVDB_MAGIC_GRID) &&
              h.mGridSize == buffer.size() && h.mGridCount &&
              h.mGridIndex < h.mGridCount && h.mGridCount <= l.maxGrids &&
              h.mGridType == e.type && h.mGridClass == e.gridClass,
          "NanoVDB payload header differs from its checked file metadata");
  require((h.mFlags.data() & ~(uint32_t(nv::GridFlags::End) - 1u)) == 0,
          "NanoVDB contains unknown grid flags");
  require(supported(h.mGridType, h.mGridClass),
          "NanoVDB native type/class is not supported by this exact reader");
  mapValid(h.mMap);
  const auto &t = *reinterpret_cast<const nv::TreeData *>(
      static_cast<const uint8_t *>(buffer.data()) + sizeof(h));
  counts({t.mNodeCount[0], t.mNodeCount[1], t.mNodeCount[2]}, t.mVoxelCount, l);
  for (int k = 0; k < 3; ++k)
    require(t.mNodeCount[k] == info.nodeCounts[k],
            "NanoVDB file/payload node counts disagree");
  workspace(static_cast<long double>(buffer.size()) +
                mul(l.maxMetadataBytes, 16) + mul(l.maxNodes, 80) + Fixed,
            l);
  switch (h.mGridType) {
  case nv::GridType::Float:
    validateTyped<float>(h, info, l, amount);
    break;
  case nv::GridType::Double:
    validateTyped<double>(h, info, l, amount);
    break;
  case nv::GridType::Int32:
    validateTyped<int32_t>(h, info, l, amount);
    break;
  case nv::GridType::Int64:
    validateTyped<int64_t>(h, info, l, amount);
    break;
  case nv::GridType::Vec3f:
    validateTyped<nv::Vec3f>(h, info, l, amount);
    break;
  case nv::GridType::Vec3d:
    validateTyped<nv::Vec3d>(h, info, l, amount);
    break;
  case nv::GridType::Boolean:
    validateTyped<bool>(h, info, l, amount);
    break;
  case nv::GridType::Mask:
    validateTyped<nv::ValueMask>(h, info, l, amount);
    break;
  default:
    throw std::invalid_argument("unsupported NanoVDB native representation");
  }
  require(info.name == h.gridName(),
          "NanoVDB payload grid name differs from file metadata");
  if (e.metadata)
    for (int k = 0; k < 3; ++k)
      require(e.metadata->tileCount[k] == t.mTileCount[k],
              "NanoVDB file/payload active tile counts disagree");
  info.storedLeafVoxels = mul(t.mNodeCount[0], 512);
  info.structureValidated = true;
  if ((!t.mNodeCount[0] && t.mNodeCount[1]) ||
      (!t.mNodeCount[1] && t.mNodeCount[2]))
    diagnostics.emplace_back("Tile-only node arrays validated by complete bounded ownership/mask/value checks; upstream ABI32.7's absent-array bounds bug requires its header/root-only check: "+info.name);
  info.version = versionText(h.mVersion);
  info.checksum = checksum(h.mChecksum);
  info.checksumModeKnown = true;
  work(amount, add(buffer.size() / 4, 1), l);
  if (info.checksum != NanoVdbChecksum::Disabled) {
    const auto integrity=detail::nano_integrity::verify(h);
    info.checksumVerified = integrity.matches;
    info.checksumCoversAllData = integrity.coversAllData;
    if (integrity.legacyNodeTail)
      diagnostics.emplace_back("Legacy ABI32.6 node-based checksum convention accepted: "+info.name);
    if (info.checksum==NanoVdbChecksum::Full && !info.checksumCoversAllData)
      diagnostics.emplace_back("Full-mode CRC has a format coverage gap (tiny blocked tail or legacy blind data); requireFullChecksum rejects this grid: "+info.name);
    require(info.checksumVerified || !o.verifyChecksums,
            "NanoVDB CRC32 checksum mismatch");
    if (!info.checksumVerified)
      diagnostics.push_back("CRC mismatch accepted only because checksum "
                            "verification was explicitly disabled: " +
                            info.name);
  }
  require(!o.requireFullChecksum || (info.checksum==NanoVdbChecksum::Full &&
                                    info.checksumCoversAllData),
          "NanoVDB full checksum coverage is required but missing/header-only or contains a format coverage gap");
}
#endif
} // namespace

const NanoVdbFileInfo &NanoVdbArchive::info() const {
  if (!data_)
    throw std::invalid_argument("NanoVDB archive is empty");
  return data_->info;
}
NanoVdbCapabilities NanoVdbInterchangeCapabilities() noexcept {
  NanoVdbCapabilities c;
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  c.nativeIo = true;
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  c.openVdbConversion = true;
#endif
#if defined(CONTAINER_NANOVDB_ZIP) && CONTAINER_NANOVDB_ZIP
  c.zip = true;
#endif
#if defined(CONTAINER_NANOVDB_BLOSC) && CONTAINER_NANOVDB_BLOSC
  c.blosc = true;
#endif
#endif
  return c;
}
NanoVdbFileInfo InspectNanoVdbFile(const std::filesystem::path &input,
                                   const NanoVdbLimits &l) {
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  limits(l);
  Snapshot s(input, l);
  return catalog(s, l).info;
#else
  (void)input;
  (void)l;
  unavailable();
#endif
}
NanoVdbArchive ReadNanoVdbFile(const std::filesystem::path &input,
                               const NanoVdbReadOptions &options,
                               const NanoVdbLimits &l) {
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  limits(l);
  Snapshot s(input, l);
  const auto c = catalog(s, l);
  const auto indices = selected(c, options.inputGrid);
  uint64_t maximum = 0, bytes = 0;
  for (auto i : indices) {
    const auto &e = c.entries[i];
    require(e.info.supported,
            "selected NanoVDB file contains an unsupported native type/class");
    codecAvailable(e.info.codec);
    bytes = add(bytes, e.info.nativeBytes);
    maximum = std::max(maximum, e.info.encodedBytes);
  }
  workspace(static_cast<long double>(s.size) + c.reserve + bytes + maximum +
                mul(l.maxMetadataBytes, 16) + mul(l.maxNodes, 80) + Fixed,
            l);
  auto data = std::make_shared<NanoVdbArchiveData>();
  data->info = c.info;
  data->info.grids.clear();
  data->info.nativeBytes = bytes;
  data->buffers.reserve(indices.size());
  std::ifstream f(s.temporary.file, std::ios::binary);
  uint64_t metadata = 0;
  for (auto i : indices) {
    const auto &e = c.entries[i];
    auto buffer = nv::HostBuffer::create(e.info.nativeBytes);
    decode(f, e, buffer, s.size);
    auto info = e.info;
    validateBuffer(buffer, e, info, options, l, data->info.work,
                   data->info.diagnostics);
    metadata = add(metadata, info.containerMetadataBytes);
    require(metadata <= l.maxMetadataBytes,
            "NanoVDB aggregate Container metadata exceeds maxMetadataBytes");
    data->info.grids.push_back(std::move(info));
    data->buffers.push_back(std::move(buffer));
  }
  if (indices.size() != c.entries.size())
    data->info.diagnostics.push_back(
        "Explicit selection omitted " +
        std::to_string(c.entries.size() - indices.size()) +
        " other native grids.");
  return NanoVdbArchive(std::move(data));
#else
  (void)input;
  (void)options;
  (void)l;
  unavailable();
#endif
}
NanoVdbFileInfo ValidateNanoVdbFile(const std::filesystem::path &input,
                                    const NanoVdbReadOptions &o,
                                    const NanoVdbLimits &l) {
  return ReadNanoVdbFile(input, o, l).info();
}

namespace {
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
std::vector<uint8_t> encode(const nv::HostBuffer &buffer, NanoVdbCodec codec) {
  if (codec == NanoVdbCodec::None)
    return {};
  codecAvailable(codec);
  if (codec == NanoVdbCodec::Zip) {
#if defined(CONTAINER_NANOVDB_ZIP) && CONTAINER_NANOVDB_ZIP
    uLongf size = compressBound(uLong(buffer.size()));
    std::vector<uint8_t> bytes(size_t(size), 0);
    require(compress2(bytes.data(), &size,
                      static_cast<const Bytef *>(buffer.data()),
                      uLong(buffer.size()), Z_DEFAULT_COMPRESSION) == Z_OK,
            "NanoVDB ZIP compression failed");
    bytes.resize(size_t(size));
    return bytes;
#endif
  } else {
#if defined(CONTAINER_NANOVDB_BLOSC) && CONTAINER_NANOVDB_BLOSC
    std::vector<uint8_t> bytes(size_t(buffer.size()) + BLOSC_MAX_OVERHEAD, 0);
    const int size = blosc_compress_ctx(
        5, BLOSC_NOSHUFFLE, 1, size_t(buffer.size()), buffer.data(),
        bytes.data(), bytes.size(), "lz4", 0, 1);
    require(size > 0, "NanoVDB BLOSC compression failed");
    bytes.resize(size_t(size));
    return bytes;
#endif
  }
  throw std::logic_error("unreachable unavailable NanoVDB writer codec");
}

NanoVdbFileInfo writeArchive(const NanoVdbArchiveData &data,
                             const std::filesystem::path &output,
                             const NanoVdbWriteOptions &options,
                             const NanoVdbLimits &l,
                             bool preserveCodecs = false,
                             uint64_t previousWork = 0) {
  limits(l);
  const auto checkMode = mode(options.checksum);
  require(!data.buffers.empty() &&
              data.buffers.size() == data.info.grids.size() &&
              data.buffers.size() <= l.maxGrids,
          "NanoVDB archive is empty or exceeds maxGrids");
  require(!output.empty() && !std::filesystem::exists(output),
          "NanoVDB output must be a new nonempty path");
  require(!options.raw || options.codec == NanoVdbCodec::None,
          "raw NanoVDB buffers cannot use ZIP/BLOSC compression");
  codecAvailable(options.codec);
  uint64_t total = 0, maximum = 0, nodes = 0, slots = 0, active = 0,
           metadata = 0;
  uint64_t catalogReserve = Fixed;
  bool compressed = false;
  for (size_t i = 0; i < data.buffers.size(); ++i) {
    const auto &buffer = data.buffers[i];
    const auto &info = data.info.grids[i];
    require(info.structureValidated && buffer.size() == info.nativeBytes,
            "NanoVDB archive contains an unchecked native payload");
    const auto selectedCodec = preserveCodecs ? info.codec : options.codec;
    codecAvailable(selectedCodec);
    compressed |= selectedCodec != NanoVdbCodec::None;
    require(!options.raw || selectedCodec == NanoVdbCodec::None,
            "raw NanoVDB checksum repair cannot retain compressed grids");
    require(info.name.size() <= l.maxNameBytes,
            "NanoVDB archive name exceeds maxNameBytes");
    total = add(total, buffer.size());
    maximum = std::max(maximum, buffer.size());
    active = add(active, info.activeVoxels);
    slots = add(slots, info.storedLeafVoxels);
    metadata = add(metadata, info.containerMetadataBytes);
    nodes = add(nodes, 1); // Every grid owns a root, including constant grids.
    for (auto n : info.nodeCounts)
      nodes = add(nodes, n);
    catalogReserve = add(catalogReserve, add(mul(info.name.size(), 16), 8192));
  }
  for (const auto &diagnostic : data.info.diagnostics)
    catalogReserve = add(catalogReserve, add(mul(diagnostic.size(), 4), 256));
  require(total <= l.maxNativeBytes && nodes <= l.maxNodes &&
              slots <= l.maxStoredLeafVoxels && active <= l.maxActiveVoxels &&
              metadata <= l.maxMetadataBytes,
          "NanoVDB archive exceeds aggregate native/topology/metadata limits");
  // Retained immutable archive + one mutable grid + one codec buffer + CRC
  // block table + report/catalog. No all-grid compressed copy is retained.
  const uint64_t codecReserve =
      compressed ? add(add(maximum, maximum / 512), 65536) : 0;
  workspace(static_cast<long double>(total) + maximum + codecReserve +
                mul(add(maximum / 4096, 1), 4) + catalogReserve,
            l);
  NanoVdbFileInfo report = data.info;
  report.work = previousWork;
  report.nativeBytes = total;
  report.raw = options.raw;
  report.diagnostics.emplace_back(
      "Native values, tree topology, maps and blind payload bytes retained; "
      "output checksums recomputed.");
  if (preserveCodecs) {
    const bool mismatch = std::any_of(
        data.info.grids.begin(), data.info.grids.end(), [](const auto &g) {
          return g.checksum != NanoVdbChecksum::Disabled && !g.checksumVerified;
        });
    report.diagnostics.emplace_back(
        mismatch ? "Explicit checksum repair accepted an old CRC mismatch only "
                   "after structural validation; data authenticity is not "
                   "established."
                 : "Explicit checksum rewrite retained the original "
                   "raw/segmented representation and per-grid codecs.");
  }
  Temporary temporary{"output.nvdb"};
  {
    std::ofstream stream(temporary.file, std::ios::binary | std::ios::out |
                                             std::ios::noreplace);
    require(bool(stream), "cannot open private NanoVDB output");
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    for (size_t i = 0; i < data.buffers.size(); ++i) {
      const auto &original = data.buffers[i];
      work(report.work, add(original.size() / 2, 1), l);
      auto buffer = nv::HostBuffer::create(original.size());
      std::memcpy(buffer.data(), original.data(), size_t(original.size()));
      auto &header = *static_cast<nv::GridData *>(buffer.data());
      header.mGridIndex = options.raw ? uint32_t(i) : 0;
      header.mGridCount = options.raw ? uint32_t(data.buffers.size()) : 1;
      const auto integrity=detail::nano_integrity::evaluate(header,checkMode);
      header.mChecksum=integrity.checksum;
      auto &info = report.grids[i];
      info.index = uint32_t(i);
      info.codec = preserveCodecs ? info.codec : options.codec;
      info.checksum = checksum(header.mChecksum);
      info.checksumModeKnown = true;
      info.checksumVerified = info.checksum != NanoVdbChecksum::Disabled;
      info.checksumCoversAllData=integrity.coversAllData;
      if (info.checksum!=options.checksum)
        report.diagnostics.emplace_back("Legacy empty-tail CRC is header-only because its header covers the entire payload: "+info.name);
      if (info.checksum==NanoVdbChecksum::Full && !info.checksumCoversAllData)
        report.diagnostics.emplace_back("Full-mode CRC retains the standard tiny-tail coverage gap; requireFullChecksum will reject this grid: "+info.name);
      const auto encoded = encode(buffer, info.codec);
      info.encodedBytes = encoded.empty()
                              ? buffer.size()
                              : add(encoded.size(), sizeof(uint64_t));
      if (!options.raw) {
        // A one-grid segment bounds compression scratch even for mixed-codec
        // archives. Standard NanoVDB readers accept consecutive segments.
        const nv::io::FileHeader segment{
            NANOVDB_MAGIC_FILE, nv::Version(), 1,
            static_cast<nv::io::Codec>(info.codec)};
        stream.write(reinterpret_cast<const char *>(&segment), sizeof(segment));
        nv::io::FileGridMetaData meta(
            buffer.size(), static_cast<nv::io::Codec>(info.codec), header);
        meta.fileSize = info.encodedBytes;
        meta.version = header.mVersion;
        meta.write(stream);
      }
      if (encoded.empty())
        stream.write(static_cast<const char *>(buffer.data()),
                     std::streamsize(buffer.size()));
      else {
        const uint64_t size = encoded.size();
        stream.write(reinterpret_cast<const char *>(&size), sizeof(size));
        stream.write(reinterpret_cast<const char *>(encoded.data()),
                     std::streamsize(size));
      }
    }
    stream.flush();
    stream.close();
  }
  report.encodedBytes = std::filesystem::file_size(temporary.file);
  require(report.encodedBytes <= l.maxInputBytes,
          "encoded NanoVDB output exceeds maxInputBytes");
  static_assert(std::is_nothrow_move_constructible_v<NanoVdbFileInfo>);
  copyExclusive(temporary.file, output, report.encodedBytes);
  return report;
}
#endif
} // namespace

NanoVdbFileInfo WriteNanoVdbFile(const NanoVdbArchive &archive,
                                 const std::filesystem::path &output,
                                 const NanoVdbWriteOptions &options,
                                 const NanoVdbLimits &l) {
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  require(!options.allowFloatMapRounding,
          "allowFloatMapRounding applies only to VDB conversion");
  require(bool(archive.data_), "NanoVDB archive is empty");
  return writeArchive(*archive.data_, output, options, l);
#else
  (void)archive;
  (void)output;
  (void)options;
  (void)l;
  unavailable();
#endif
}

NanoVdbFileInfo RewriteNanoVdbChecksums(const std::filesystem::path &input,
                                        const std::filesystem::path &output,
                                        NanoVdbChecksum desired,
                                        const NanoVdbLimits &l) {
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  (void)mode(desired);
  NanoVdbReadOptions read;
  read.verifyChecksums = false;
  auto archive = ReadNanoVdbFile(input, read, l);
  NanoVdbWriteOptions options;
  options.raw = archive.info().raw;
  options.checksum = desired;
  return writeArchive(*archive.data_, output, options, l, true,
                      archive.info().work);
#else
  (void)input;
  (void)output;
  (void)desired;
  (void)l;
  unavailable();
#endif
}

#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO &&               \
    defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbNanoConversion.inl"
#else
NanoVdbFileInfo WriteNanoVdbFromVdb(const std::filesystem::path &input,
                                    const std::filesystem::path &output,
                                    const std::string &grid,
                                    const NanoVdbWriteOptions &options,
                                    const NanoVdbLimits &l) {
  (void)input;
  (void)output;
  (void)grid;
  (void)options;
  (void)l;
  unavailable(true);
}
NanoVdbFileInfo WriteVdbFromNanoVdb(const std::filesystem::path &input,
                                    const std::filesystem::path &output,
                                    const NanoVdbReadOptions &options,
                                    bool allowMetadataLoss,
                                    const NanoVdbLimits &l) {
  (void)input;
  (void)output;
  (void)options;
  (void)allowMetadataLoss;
  (void)l;
  unavailable(true);
}
#endif
} // namespace container::geometry::vdb
