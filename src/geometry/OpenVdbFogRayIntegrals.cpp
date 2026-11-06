#include "Container/geometry/OpenVdbFogRayIntegrals.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/OpenVdbGridSelection.h"
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/tools/Count.h>
#include <set>
#include <tbb/task_arena.h>

#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB fog ray integrals are unavailable; configure "
      "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed = 4.0L * 1024 * 1024;
void bytes(long double n, const ProcessingOptions &p, const char *why) {
  if (!std::isfinite(n) || n > p.maxInputBytes)
    throw std::invalid_argument(why);
}
long double metadata(const openvdb::MetaMap &m) {
  long double n = sizeof(m);
  for (auto it = m.beginMeta(); it != m.endMeta(); ++it) {
    if (!it->second)
      throw std::invalid_argument("null fog ray integrals metadata");
    n += 4.0L * (it->first.size() + it->second->typeName().size() +
                 it->second->size()) +
         512;
  }
  return n;
}
struct Work {
  uint64_t used = 0, cap;
  void add(long double n) {
    if (!std::isfinite(n) || n > cap - used)
      throw std::invalid_argument(
          "fog ray integrals operation exceeds maxWorkItems");
    used += uint64_t(std::ceil(n));
  }
};
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p,
           Work &work, uint64_t reserve) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument(
          "fog ray integrals input must be a regular file");
    encoded = std::filesystem::file_size(input);
    work.add(encoded * 4.0L); // snapshot I/O plus subsequent catalog reads
    bytes(Fixed + encoded + reserve, p,
          "encoded fog ray integrals source exceeds memory budget");
    // OpenVDB's native File interface takes narrow paths. TEMP may itself
    // contain a Unicode user name, so choose the shared checked ASCII private
    // scratch while reading the original source through its native path.
    directory = detail::CreateAsciiVdbTemporaryDirectory();
    file = directory / "source.vdb";
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open fog ray integrals snapshot");
      std::array<char, 65536> buffer{};
      auto left = encoded;
      while (left) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(left, buffer.size()));
        source.read(buffer.data(), count);
        if (source.gcount() != count)
          throw std::runtime_error("fog ray integrals source changed length");
        target.write(buffer.data(), count);
        left -= count;
      }
      target.close();
      if (!target || source.bad() ||
          source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "fog ray integrals snapshot failed or source changed length");
    } catch (...) {
      cleanup();
      throw;
    }
  }
  void cleanup() {
    std::error_code e;
    std::filesystem::remove(directory / "result.vdb", e);
    std::filesystem::remove(file, e);
    std::filesystem::remove(directory, e);
  }
  ~Snapshot() { cleanup(); }
};
bool supported(const openvdb::GridBase &g) {
  return (g.isType<openvdb::FloatGrid>() || g.isType<openvdb::DoubleGrid>()) &&
         (g.getGridClass() == openvdb::GRID_UNKNOWN ||
          g.getGridClass() == openvdb::GRID_FOG_VOLUME);
}
void structuralSemantics(const openvdb::GridBase &g) {
  const auto text = [&](const char *key, const std::string &expected) {
    if (const auto raw = g[key]) {
      const auto value =
          std::dynamic_pointer_cast<const openvdb::StringMetadata>(raw);
      if (!value)
        throw std::invalid_argument(
            "fog-ray structural labels require native String metadata");
      auto s = value->value();
      openvdb::string::trim(s);
      openvdb::string::to_lower(s);
      if (s != expected)
        throw std::invalid_argument("fog-ray scalar class/vector label is "
                                    "unrecognized or incompatible");
    }
  };
  text(openvdb::GridBase::META_GRID_CLASS,
       openvdb::GridBase::gridClassToString(g.getGridClass()));
  text(openvdb::GridBase::META_VECTOR_TYPE,
       openvdb::GridBase::vecTypeToString(openvdb::VEC_INVARIANT));
  if (const auto raw = g[openvdb::GridBase::META_IS_LOCAL_SPACE];
      raw && !std::dynamic_pointer_cast<const openvdb::BoolMetadata>(raw))
    throw std::invalid_argument(
        "fog-ray world/local label requires native Bool metadata");
  if (!g.isInWorldSpace())
    throw std::invalid_argument(
        "fog rays require WORLD/invariant scalar fields");
}
uint64_t fullMemory(const openvdb::GridBase &g) {
  if (g.isType<openvdb::FloatGrid>())
    return openvdb::tools::memUsageIfLoaded(
        static_cast<const openvdb::FloatGrid &>(g).tree(), false);
  if (g.isType<openvdb::DoubleGrid>())
    return openvdb::tools::memUsageIfLoaded(
        static_cast<const openvdb::DoubleGrid &>(g).tree(), false);
  throw std::invalid_argument("fog rays support Float/Double only");
}
struct Loaded {
  openvdb::GridBase::Ptr grid;
  openvdb::MetaMap::Ptr fileMeta;
  long double resident = 0, meta = 0;
  std::string descriptor;
  uint64_t recorded = 0;
  uint64_t recordedActive = 0;
  bool hasActiveHint = false;
};
class MetadataArchive : public openvdb::io::Archive {
public:
  void begin(std::istream &stream, openvdb::io::StreamMetadata::Ptr &metadata) {
    readHeader(stream);
    if (!inputHasGridOffsets() ||
        fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
        fileVersion() > openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument(
          "fog ray integrals require supported seekable "
          "metadata-readable VDB files");
    openvdb::io::setStreamMetadataPtr(stream, metadata, false);
    setFormatVersion(stream);
    setLibraryVersion(stream);
    setDataCompression(stream);
  }
  int32_t count(std::istream &stream) { return readGridCount(stream); }
  openvdb::GridBase::Ptr next(std::istream &stream,
                              openvdb::io::GridDescriptor &descriptor,
                              uintmax_t fileBytes) {
    auto grid = descriptor.read(stream);
    const auto afterDescriptor = int64_t(stream.tellg()),
               start = descriptor.getGridPos(), end = descriptor.getEndPos(),
               blocks = descriptor.getBlockPos();
    if (!grid || start < afterDescriptor || end <= start ||
        uintmax_t(end) > fileBytes ||
        (!descriptor.isInstance() && (blocks < start || blocks > end)))
      throw std::invalid_argument(
          "fog ray integrals grid descriptor offsets are "
          "outside the encoded range");
    descriptor.seekToGrid(stream);
    readGridCompression(stream);
    grid->readMeta(stream);
    grid->readTransform(stream);
    // Formats 216--218 can omit the grid display name from metadata. Match
    // native Archive::readGrid rather than losing bare-name ambiguity checks.
    if (fileVersion() < openvdb::OPENVDB_FILE_VERSION_NO_GRIDMAP &&
        grid->getName().empty())
      grid->setName(descriptor.gridName());
    if (int64_t(stream.tellg()) > (descriptor.isInstance() ? end : blocks))
      throw std::invalid_argument(
          "fog ray integrals metadata/transform crosses grid payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};
Loaded load(const Snapshot &s, const RayQueryLimits &o,
            const std::string &selector, const ProcessingOptions &p,
            uint64_t reserve, Work &work) {
  detail::RequireSeekableFile(s.file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument(
        "bounded fog ray integrals require delayed loading; "
        "unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded fog ray integrals require OpenVDB delayed-loading support");
#else
  // File::readGridMetadata makes a simplifying Transform::copy(). Read only
  // bounded native metadata records directly to retain original coefficients.
  auto streamMeta = std::make_shared<openvdb::io::StreamMetadata>();
  streamMeta->setSeekable(true);
  std::ifstream stream(s.file, std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  MetadataArchive archive;
  archive.begin(stream, streamMeta);
  Loaded r;
  r.fileMeta = std::make_shared<openvdb::MetaMap>();
  r.fileMeta->readMeta(stream);
  r.meta = metadata(*r.fileMeta);
  work.add(r.meta * 4);
  long double catalog = s.encoded + Fixed + reserve;
  auto check = [&] {
    if (r.meta > o.maxMetadataBytes)
      throw std::invalid_argument(
          "aggregate fog ray integrals file/grid metadata "
          "exceeds maxMetadataBytes");
    bytes(catalog + 8 * r.meta, p,
          "fog ray integrals catalog exceeds memory budget");
  };
  check();
  std::set<std::string> descriptors;
  const int32_t gridCount = archive.count(stream);
  if (gridCount < 0 || uint32_t(gridCount) > o.maxGrids)
    throw std::invalid_argument("fog ray integrals catalog exceeds maxGrids");
  std::string chosen;
  bool found = false;
  for (int32_t i = 0; i < gridCount; ++i) {
    openvdb::io::GridDescriptor descriptor;
    auto m = archive.next(stream, descriptor, s.encoded);
    const auto name =
        openvdb::io::GridDescriptor::nameAsString(descriptor.uniqueName());
    catalog += 4.0L * name.size() + 256;
    check();
    if (!descriptors.insert(name).second)
      throw std::invalid_argument("fog ray integrals input has colliding "
                                  "descriptors; rename grids first");
    const auto gridMetadata = metadata(*m);
    r.meta += gridMetadata;
    work.add(gridMetadata * 4);
    check();
    if (!GridSelectionMatches(selector, name, m->getName()))
      continue;
    const bool usable = supported(*m);
    if (!usable) {
      if (!selector.empty())
        throw std::invalid_argument(
            "fog ray integrals selected unsupported native type/class");
      continue;
    }
    structuralSemantics(*m);
    if (found)
      throw std::invalid_argument("ambiguous fog ray integrals selector; use "
                                  "descriptor:<exact descriptor>");
    const auto rawMemory = (*m)[openvdb::GridBase::META_FILE_MEM_BYTES],
               rawCount = (*m)[openvdb::GridBase::META_FILE_VOXEL_COUNT];
    const auto mem = std::dynamic_pointer_cast<const openvdb::Int64Metadata>(
                   rawMemory),
               count = std::dynamic_pointer_cast<const openvdb::Int64Metadata>(
                   rawCount);
    if ((rawMemory && !mem) || (rawCount && !count) ||
        (mem && mem->value() < 0) || (count && count->value() < 0))
      throw std::invalid_argument(
          "fog-ray-integral optional saved statistics must be "
          "nonnegative native Int64 values");
    if (count && uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument(
          "recorded fog-ray-integral count exceeds maxVoxelCount");
    if (count && uint64_t(count->value()) > o.maxWorkItems)
      throw std::invalid_argument("recorded fog ray integrals source exceeds "
                                  "maxWorkItems before loading");
    bytes(catalog + 8 * r.meta + (mem ? mem->value() : 0), p,
          "recorded fog ray integrals tree exceeds memory budget");
    found = true;
    chosen = name;
    r.descriptor = name;
    r.recorded = mem ? uint64_t(mem->value()) : 0;
    r.hasActiveHint = bool(count);
    r.recordedActive = count ? uint64_t(count->value()) : 0;
  }
  if (!found)
    throw std::invalid_argument("no matching supported fog ray integrals grid");
  stream.close();
  work.add(r.recorded * 8.0L); // before decoded topology allocation
  openvdb::io::File file(s.file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error(
        "fog ray integrals snapshot changed before decoded loading");
  r.grid = file.readGrid(chosen);
  const auto full = fullMemory(*r.grid);
  if (full > r.recorded)
    work.add((full - r.recorded) * 8.0L); // before selected buffer paging
  r.resident = catalog + 8 * r.meta + full;
  if (static_cast<long double>(full) * 8 > o.maxWorkItems)
    throw std::invalid_argument("fog ray integrals decoded source exceeds "
                                "maxWorkItems before materialization");
  bytes(r.resident, p,
        "fully decoded fog ray integrals source exceeds memory budget before "
        "materialization");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full + 4 * r.meta)
    throw std::runtime_error(
        "fog ray integrals tree exceeded decoded reservation");
  file.close();
  return r;
#endif
}

using V = openvdb::Vec3d;
// Local checked arithmetic adapted from the immutable PointTransfer integer
// accumulator. No Boost.Multiprecision headers or external arithmetic provider.
struct Big {
  static constexpr size_t Limbs = 2048;
  std::array<uint32_t, Limbs> data{};
  Big(uint64_t n = 0) {
    data[0] = uint32_t(n);
    data[1] = uint32_t(n >> 32);
  }
  size_t bits() const {
    for (size_t i = Limbs; i--;)
      if (data[i])
        return 32 * i + 32 - std::countl_zero(data[i]);
    return 0;
  }
  bool zero() const { return bits() == 0; }
  int compare(const Big &b) const {
    for (size_t i = Limbs; i--;)
      if (data[i] != b.data[i])
        return data[i] < b.data[i] ? -1 : 1;
    return 0;
  }
  void add(const Big &b) {
    uint64_t carry = 0;
    for (size_t i = 0; i < Limbs; ++i) {
      const auto sum = uint64_t(data[i]) + b.data[i] + carry;
      data[i] = uint32_t(sum);
      carry = sum >> 32;
    }
    if (carry)
      throw std::runtime_error(
          "fog integral exact accumulator capacity exceeded");
  }
  void subtract(const Big &b) {
    if (compare(b) < 0)
      throw std::runtime_error("fog integral negative unsigned intermediate");
    uint64_t borrow = 0;
    for (size_t i = 0; i < Limbs; ++i) {
      const auto sub = uint64_t(b.data[i]) + borrow, old = uint64_t(data[i]);
      data[i] = uint32_t(old - sub);
      borrow = old < sub;
    }
  }
  void shift(size_t n) {
    if (!n || zero())
      return;
    if (bits() + n > Limbs * 32)
      throw std::runtime_error("fog integral exact shift capacity exceeded");
    const auto whole = n / 32, part = n % 32;
    for (size_t i = Limbs; i--;) {
      uint64_t value = 0;
      if (i >= whole) {
        value = uint64_t(data[i - whole]) << part;
        if (part && i > whole)
          value |= uint64_t(data[i - whole - 1]) >> (32 - part);
      }
      data[i] = uint32_t(value);
    }
  }
  size_t trailing() const {
    for (size_t i = 0; i < Limbs; ++i)
      if (data[i])
        return 32 * i + std::countr_zero(data[i]);
    return 0;
  }
  void right(size_t n) {
    const auto whole = n / 32, part = n % 32;
    for (size_t i = 0; i < Limbs; ++i) {
      uint64_t value = 0;
      if (i + whole < Limbs) {
        value = uint64_t(data[i + whole]) >> part;
        if (part && i + whole + 1 < Limbs)
          value |= uint64_t(data[i + whole + 1]) << (32 - part);
      }
      data[i] = uint32_t(value);
    }
  }
  static Big multiply(const Big &a, const Big &b) {
    Big result;
    const size_t an = (a.bits() + 31) / 32, bn = (b.bits() + 31) / 32;
    if (a.zero() || b.zero())
      return result;
    if (a.bits() + b.bits() > Limbs * 32 + 1)
      throw std::runtime_error("fog integral exact product capacity exceeded");
    for (size_t i = 0; i < an; ++i) {
      uint64_t carry = 0;
      for (size_t j = 0; j < bn; ++j) {
        if (i + j >= Limbs)
          throw std::runtime_error(
              "fog integral exact product capacity exceeded");
        const auto product =
            uint64_t(a.data[i]) * b.data[j] + result.data[i + j] + carry;
        result.data[i + j] = uint32_t(product);
        carry = product >> 32;
      }
      if (carry) {
        if (i + bn >= Limbs)
          throw std::runtime_error(
              "fog integral exact product capacity exceeded");
        result.data[i + bn] = uint32_t(carry);
      }
    }
    return result;
  }
};
struct SignedBig {
  Big magnitude;
  bool negative = false;
  void add(const Big &b, bool sign = false) {
    if (sign == negative)
      magnitude.add(b);
    else if (magnitude.compare(b) >= 0)
      magnitude.subtract(b);
    else {
      auto next = b;
      next.subtract(magnitude);
      magnitude = next;
      negative = sign;
    }
    if (magnitude.zero())
      negative = false;
  }
};

struct Factor {
  uint64_t sig = 0;
  int unit = 0;
  bool negative = false;
};
Factor binary(double value) {
  if (!std::isfinite(value))
    throw std::invalid_argument("nonfinite fog integral factor");
  const auto b = std::bit_cast<uint64_t>(value), e = (b >> 52) & 2047;
  uint64_t sig =
      (b & UINT64_C(0xfffffffffffff)) | (e ? UINT64_C(0x10000000000000) : 0);
  if (!sig)
    return {};
  const auto zeros = std::countr_zero(sig);
  sig >>= zeros;
  return {sig, (e ? int(e) - 1075 : -1074) + int(zeros), bool(b >> 63)};
}
struct ExactFactor {
  Big sig;
  int unit = 0;
  bool negative = false;
};
ExactFactor normalized(SignedBig value, int unit) {
  if (value.magnitude.zero())
    return {};
  const auto zeros = value.magnitude.trailing();
  value.magnitude.right(zeros);
  return {value.magnitude, unit + int(zeros), value.negative};
}
Big units(const ExactFactor &f, int unit) {
  if (f.unit < unit)
    throw std::logic_error("fog mapped factor exponent proof failed");
  auto x = f.sig;
  x.shift(size_t(f.unit - unit));
  return x;
}
SignedBig signedUnits(const ExactFactor &f, int unit) {
  return {units(f, unit), f.negative && !f.sig.zero()};
}
Big units(const Factor &f, int unit) {
  if (f.unit < unit)
    throw std::logic_error("fog integral canonical exponent proof failed");
  Big x(f.sig);
  x.shift(size_t(f.unit - unit));
  return x;
}
SignedBig signedUnits(const Factor &f, int unit) {
  return {units(f, unit), f.negative && f.sig != 0};
}
int compare(const SignedBig &a, const SignedBig &b) {
  if (a.negative != b.negative)
    return a.negative ? -1 : 1;
  const int c = a.magnitude.compare(b.magnitude);
  return a.negative ? -c : c;
}
Big times(const Big &a, const Big &b) { return Big::multiply(a, b); }
Big times(const Big &a, uint64_t b) { return times(a, Big(b)); }
Big shifted(Big x, size_t n) {
  x.shift(n);
  return x;
}
SignedBig signedTimes(const SignedBig &a, const Big &b) {
  auto m = times(a.magnitude, b);
  return {m, a.negative && !m.zero()};
}
void append(SignedBig &a, const SignedBig &b) {
  a.add(b.magnitude, b.negative);
}
uint64_t magnitude(int64_t x) {
  return x < 0 ? uint64_t(-(x + 1)) + 1 : uint64_t(x);
}
Big quotientSmallExact(const Big &a, uint64_t divisor) {
  if (!divisor)
    throw std::logic_error("zero fog integral odd divisor");
  Big q;
  uint64_t r = 0;
  for (size_t bit = a.bits(); bit--;) {
    r = (r << 1) | ((a.data[bit / 32] >> (bit % 32)) & 1);
    if (r >= divisor) {
      r -= divisor;
      q.data[bit / 32] |= uint32_t(1) << (bit % 32);
    }
  }
  if (r)
    throw std::logic_error("inexact fog integral common-denominator division");
  return q;
}
uint64_t quotient64(Big numerator, const Big &denominator, bool up = false) {
  if (denominator.zero())
    throw std::logic_error("zero fog integral denominator");
  uint64_t q = 0;
  for (int bit = 63; bit >= 0; --bit) {
    auto d = shifted(denominator, size_t(bit));
    if (numerator.compare(d) >= 0) {
      numerator.subtract(d);
      q |= UINT64_C(1) << bit;
    }
  }
  if (numerator.compare(denominator) >= 0 ||
      (up && !numerator.zero() && q == UINT64_MAX))
    throw std::invalid_argument(
        "fog integral count/coordinate overflows UInt64");
  return q + uint64_t(up && !numerator.zero());
}
double ratio(const Big &numerator, const Big &denominator, int unit,
             bool residual = false) {
  if (numerator.zero())
    return 0;
  if (denominator.zero())
    throw std::runtime_error("fog integral zero exact denominator");
  {
    auto n = numerator, d = denominator;
    const int lowShift = unit + 1074;
    if (lowShift >= 0)
      n.shift(size_t(lowShift));
    else
      d.shift(size_t(-lowShift));
    if (n.compare(d) < 0)
      throw std::invalid_argument(
          "fog nonzero exact result is below smallest positive Double");
    n = numerator;
    d = times(denominator, UINT64_C(0x1fffffffffffff));
    const int highShift = unit - 971;
    if (highShift >= 0)
      n.shift(size_t(highShift));
    else
      d.shift(size_t(-highShift));
    if (n.compare(d) > 0)
      throw std::invalid_argument("fog exact result exceeds Double maximum");
  }
  int exponent = int(numerator.bits()) - int(denominator.bits());
  if (exponent >= 0) {
    auto comparison = denominator;
    comparison.shift(size_t(exponent));
    if (numerator.compare(comparison) < 0)
      --exponent;
  } else {
    auto comparison = numerator;
    comparison.shift(size_t(-exponent));
    if (comparison.compare(denominator) < 0)
      --exponent;
  }
  const int target = std::max(exponent + unit - 52, -1074),
            shift = unit - target;
  auto dividend = numerator, divisor = denominator;
  if (shift >= 0)
    dividend.shift(size_t(shift));
  else
    divisor.shift(size_t(-shift));
  uint64_t quotient = 0;
  for (int bit = 53; bit >= 0; --bit) {
    auto candidate = divisor;
    candidate.shift(size_t(bit));
    if (dividend.compare(candidate) >= 0) {
      dividend.subtract(candidate);
      quotient |= uint64_t(1) << bit;
    }
  }
  dividend.shift(1);
  const int order = dividend.compare(divisor);
  if (order > 0 || (order == 0 && (quotient & 1)))
    ++quotient;
  if (target > 971 || (target == 971 && quotient >= UINT64_C(0x20000000000000)))
    throw std::invalid_argument("fog integral native output overflows Double");
  const double result = std::scalbn(double(quotient), target);
  if (!std::isfinite(result))
    throw std::invalid_argument("fog integral native output overflows Double");
  if (result == 0) {
    if (residual)
      return std::numeric_limits<double>::denorm_min();
    throw std::invalid_argument(
        "fog integral nonzero native output underflows Double");
  }
  if (residual) {
    const double upper = std::nextafter(result, INFINITY);
    if (!std::isfinite(upper))
      throw std::invalid_argument(
          "fog integral mass-error bound exceeds Double range");
    return upper;
  }
  return result;
}
double signedRatio(const SignedBig &n, const Big &denominator, int unit) {
  const double result = ratio(n.magnitude, denominator, unit);
  return n.negative ? -result : result;
}

uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a)
    throw std::invalid_argument("fog integral count overflows UInt64");
  return a + b;
}
uint64_t product(uint64_t a, uint64_t b) {
  if (a && b > UINT64_MAX / a)
    throw std::invalid_argument("fog integral count overflows UInt64");
  return a * b;
}
uint64_t volume(const openvdb::CoordBBox &b) {
  uint64_t n = 1;
  for (int a = 0; a < 3; ++a)
    n = product(n, uint64_t(int64_t(b.max()[a]) - b.min()[a] + 1));
  return n;
}
double gap(double x) {
  return std::max(std::abs(std::nextafter(x, INFINITY) - x),
                  std::abs(x - std::nextafter(x, -INFINITY)));
}
void exactProduct(SignedBig &sum, double a, double b, bool negate = false) {
  const auto x = binary(a), y = binary(b);
  auto m = times(Big(x.sig), Big(y.sig));
  m.shift(size_t(x.unit + y.unit + 2148));
  sum.add(m, x.negative != y.negative != negate);
}
SignedBig exactIndex(const V &p, const V &origin,
                     const std::array<V, 3> &inverse, int row, bool position) {
  SignedBig sum;
  for (int c = 0; c < 3; ++c) {
    exactProduct(sum, p[c], inverse[c][row]);
    if (position)
      exactProduct(sum, origin[c], inverse[c][row], true);
  }
  return sum;
}
double exactEndpoint(double origin, double d, double t) {
  auto sum = signedUnits(binary(origin), -2148);
  exactProduct(sum, d, t);
  return signedRatio(sum, Big(1), -2148);
}
double scale(double a, double b) {
  if (!a)
    return std::copysign(0., a);
  const double out = a / b;
  if (!std::isfinite(out) || !out)
    throw std::invalid_argument(
        "fog integral normalization loses nonzero Double range");
  return out;
}
V direction(const std::array<double, 3> &input) {
  double peak = 0;
  for (double x : input) {
    if (!std::isfinite(x))
      throw std::invalid_argument("nonfinite fog direction");
    peak = std::max(peak, std::abs(x));
  }
  if (!peak)
    throw std::invalid_argument("fog ray direction must be nonzero");
  V v;
  for (int a = 0; a < 3; ++a)
    v[a] = scale(input[a], peak);
  const double length = std::hypot(v[0], v[1], v[2]);
  for (int a = 0; a < 3; ++a)
    v[a] = scale(v[a], length);
  return v;
}
struct Map {
  V origin;
  std::array<V, 3> basis, inverse;
  double h = 0;
  explicit Map(const openvdb::GridBase &grid) {
    if (!grid.transform().isLinear())
      throw std::invalid_argument(
          "fog rays require uniform orthogonal affine maps");
    const auto m = grid.transform().baseMap();
    origin = m->applyMap(V(0));
    for (int c = 0; c < 3; ++c) {
      V axis(0);
      axis[c] = 1;
      basis[c] = m->applyJacobian(axis);
      inverse[c] = m->applyInverseJacobian(axis);
      const double length = std::hypot(basis[c][0], basis[c][1], basis[c][2]);
      if (!std::isfinite(length) || length < 1e-6 || length > 1e6)
        throw std::invalid_argument("fog-ray axes must be in[1e-6,1e6]");
      if (!c)
        h = length;
      if (std::abs(length / h - 1) > 1e-12)
        throw std::invalid_argument("fog rays require uniform voxels");
      for (int r = 0; r < 3; ++r)
        if (!std::isfinite(origin[r]) || !std::isfinite(basis[c][r]) ||
            !std::isfinite(inverse[c][r]) || std::abs(inverse[c][r]) > 2 / h)
          throw std::invalid_argument("nonfinite fog affine map");
      for (int b = 0; b < c; ++b) {
        double dot = 0;
        for (int r = 0; r < 3; ++r)
          dot += (basis[c][r] / h) * (basis[b][r] / h);
        if (std::abs(dot) > 1e-12)
          throw std::invalid_argument("fog rays require orthogonal axes");
      }
    }
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) {
        // A native cached inverse may have rounded a nonzero cofactor to zero
        // before a later large WORLD vector can recover its contribution.
        // Detect that from original Double Jacobian products, not J*J^-1's
        // tolerance (which cannot see such tiny lost off-diagonal entries).
        std::array<int, 2> rows{}, cols{};
        int nr = 0, nc = 0;
        for (int a = 0; a < 3; ++a) {
          if (a != c)
            rows[nr++] = a;
          if (a != r)
            cols[nc++] = a;
        }
        SignedBig cofactor;
        exactProduct(cofactor, basis[cols[0]][rows[0]],
                     basis[cols[1]][rows[1]]);
        exactProduct(cofactor, basis[cols[1]][rows[0]], basis[cols[0]][rows[1]],
                     true);
        if (!cofactor.magnitude.zero() && inverse[c][r] == 0)
          throw std::invalid_argument("fog affine cached inverse loses a "
                                      "nonzero cofactor; condition the map");
        double s = 0;
        for (int k = 0; k < 3; ++k)
          s += inverse[k][r] * basis[c][k];
        if (!std::isfinite(s) || std::abs(s - (r == c ? 1. : 0.)) > 1e-12)
          throw std::invalid_argument("inconsistent fog affine inverse cache");
      }
    spacing(origin);
  }
  void spacing(const V &p) const {
    for (int a = 0; a < 3; ++a)
      if (!std::isfinite(p[a]) || gap(p[a]) > h * 1e-4)
        throw std::invalid_argument(
            "fog WORLD coordinates lack1e-4 voxel precision");
  }
};
struct Census {
  uint64_t active = 0, tiles = 0, leaves = 0;
  openvdb::CoordBBox deviation;
  int densityUnit = 0;
  size_t densityBits = 0;
};
template <class G>
Census census(const G &grid, const Loaded &loaded, const ProcessingOptions &p,
              Work &work) {
  Census c;
  using T = typename G::TreeType;
  using U = typename T::RootNodeType::ChildNodeType;
  using L = typename U::ChildNodeType;
  std::array<uint64_t, 3> nodes{};
  uint64_t roots = 0;
  for (auto i = grid.tree().cbeginRootDense(); i; ++i)
    roots = add(roots, 1);
  for (auto i = grid.tree().cbeginNode(); i; ++i)
    if (i.getLevel() < 3)
      nodes[i.getLevel()] = add(nodes[i.getLevel()], 1);
  c.leaves = nodes[0];
  const auto leafSlots = product(c.leaves, 512);
  if (leafSlots > p.maxVoxelCount)
    throw std::invalid_argument("fog stored leaf slots exceed maxVoxelCount");
  const auto slots =
      add(roots, add(leafSlots, add(product(nodes[1], L::NUM_VALUES),
                                    product(nodes[2], U::NUM_VALUES))));
  work.add(64.0L * slots);
  const double bg = double(grid.background());
  double maximum = 0;
  int minimum = 0;
  const auto value = [&](double x) {
    if (!std::isfinite(x) || x < 0)
      throw std::invalid_argument("all stored fog values/background must be "
                                  "finite nonnegative density");
    maximum = std::max(maximum, x);
    if (x)
      minimum = std::min(minimum, binary(x).unit);
  };
  value(bg);
  for (auto i = grid.cbeginValueAll(); i; ++i) {
    value(double(*i));
    const auto b = i.getBoundingBox();
    if (i.isValueOn()) {
      c.active = add(c.active, volume(b));
      if (!i.isVoxelValue())
        c.tiles = add(c.tiles, 1);
    }
    if (double(*i) != bg) {
      for (int a = 0; a < 3; ++a)
        if (b.min()[a] < INT32_MIN + 2LL || b.max()[a] > INT32_MAX - 2LL)
          throw std::invalid_argument(
              "fog interpolation support lacks Int32 margins");
      c.deviation.expand(b);
    }
  }
  if (c.active > p.maxVoxelCount)
    throw std::invalid_argument(
        "fog weighted active count exceeds maxVoxelCount");
  if (loaded.hasActiveHint && loaded.recordedActive != c.active)
    throw std::invalid_argument(
        "saved fog active count disagrees with checked topology");
  c.densityUnit = minimum;
  const auto f = binary(maximum);
  c.densityBits = f.sig ? std::bit_width(f.sig) + size_t(f.unit - minimum) : 0;
  return c;
}
struct Common {
  std::array<double, 3> world{}, direction{};
  double minimum = 0, maximum = 0;
};
Common common(const WorldRay &ray, const Map &map) {
  if (!std::isfinite(ray.minimumDistance) ||
      !std::isfinite(ray.maximumDistance) || ray.minimumDistance < 0 ||
      ray.maximumDistance <= ray.minimumDistance ||
      gap(ray.maximumDistance) > map.h * 1e-4)
    throw std::invalid_argument(
        "fog WORLD intervals must be finite increasing and resolved");
  Common c;
  map.spacing(V(ray.origin.data()));
  const auto d = direction(ray.direction);
  c.minimum = ray.minimumDistance;
  c.maximum = ray.maximumDistance;
  const Big bound = units(binary(1e12), -3222);
  for (int a = 0; a < 3; ++a) {
    c.world[a] = ray.origin[a];
    c.direction[a] = d[a];
    const auto q =
        exactIndex(V(ray.origin.data()), map.origin, map.inverse, a, true);
    const auto v = exactIndex(d, map.origin, map.inverse, a, false);
    auto origin = q.magnitude;
    origin.shift(1074);
    if (origin.compare(bound) > 0)
      throw std::invalid_argument("fog index origin exceeds1e12");
    for (double t : {c.minimum, c.maximum}) {
      const double x = exactEndpoint(ray.origin[a], d[a], t);
      if (gap(x) > map.h * 1e-4)
        throw std::invalid_argument("fog endpoint lacks voxel precision");
      SignedBig end{origin, q.negative};
      const auto f = binary(t);
      auto term = times(v.magnitude, Big(f.sig));
      term.shift(size_t(f.unit + 1074));
      end.add(term, v.negative != f.negative);
      if (end.magnitude.compare(bound) > 0)
        throw std::invalid_argument("fog index endpoint exceeds1e12");
    }
  }
  return c;
}
struct Plan {
  Common ray;
  std::array<ExactFactor, 3> q, d;
  std::array<Big, 3> planeFactors;
  std::array<SignedBig, 3> qBase, dScaled;
  Big lambda, weightDen, denominator, outsideFactor, begin, end;
  int fractionUnit = 0;
  uint64_t bound = 0, cellWork = 0;
  bool interior = false;
  SignedBig plane(int axis, int64_t cell) const {
    const int e = std::min(q[axis].unit, 0);
    SignedBig n;
    n.add(shifted(Big(magnitude(cell)), size_t(-e)), cell < 0);
    n.add(units(q[axis], e), !q[axis].negative);
    if (d[axis].negative && !n.magnitude.zero())
      n.negative = !n.negative;
    return signedTimes(n, planeFactors[axis]);
  }
};
Big timeUnits(double t, const Big &odd, int k) {
  const auto f = binary(t);
  auto v = times(Big(f.sig), odd);
  v.shift(size_t(f.unit + k));
  return v;
}
void prepare(Plan &p, const Common &ray, const Census &c, const Map &map) {
  p = Plan{};
  p.ray = ray;
  Big odd(1);
  int k = 0, F = 0;
  for (int a = 0; a < 3; ++a) {
    p.q[a] = normalized(
        exactIndex(V(ray.world.data()), map.origin, map.inverse, a, true),
        -2148);
    p.d[a] = normalized(
        exactIndex(V(ray.direction.data()), map.origin, map.inverse, a, false),
        -2148);
    F = std::max({F, -p.q[a].unit, -p.d[a].unit});
    if (!p.d[a].sig.zero()) {
      odd = times(odd, p.d[a].sig);
      k = std::max(k, p.d[a].unit + std::max(-p.q[a].unit, 0));
    }
  }
  k = std::max({k, -binary(ray.minimum).unit, -binary(ray.maximum).unit});
  p.lambda = shifted(odd, size_t(k));
  p.weightDen = shifted(p.lambda, size_t(F + 1));
  p.fractionUnit = F;
  const auto cube = times(times(p.weightDen, p.weightDen), p.weightDen);
  p.outsideFactor = times(cube, 6);
  p.denominator = times(p.lambda, p.outsideFactor);
  p.begin = timeUnits(ray.minimum, odd, k);
  p.end = timeUnits(ray.maximum, odd, k);
  SignedBig begin{p.begin, false}, end{p.end, false};
  for (int a = 0; a < 3; ++a) {
    p.qBase[a] = signedTimes(signedUnits(p.q[a], -F), p.lambda);
    p.dScaled[a] = signedUnits(p.d[a], -F);
    if (p.d[a].sig.zero()) {
      if (!c.deviation.empty()) {
        auto den = p.weightDen;
        den.right(1);
        const int64_t lo = int64_t(c.deviation.min()[a]) - 1,
                      hi = int64_t(c.deviation.max()[a]) + 1;
        if (compare(p.qBase[a], SignedBig{times(den, magnitude(lo)), lo < 0}) <
                0 ||
            compare(p.qBase[a], SignedBig{times(den, magnitude(hi)), hi < 0}) >
                0)
          return;
      }
      continue;
    }
    Big without(1);
    for (int b = 0; b < 3; ++b)
      if (b != a && !p.d[b].sig.zero())
        without = times(without, p.d[b].sig);
    const int e = std::min(p.q[a].unit, 0), shift = k + e - p.d[a].unit;
    if (shift < 0)
      throw std::logic_error("fog event scale proof failed");
    p.planeFactors[a] = shifted(without, size_t(shift));
    if (c.deviation.empty())
      continue;
    auto lo = p.plane(a, int64_t(c.deviation.min()[a]) - 1),
         hi = p.plane(a, int64_t(c.deviation.max()[a]) + 1);
    if (compare(lo, hi) > 0)
      std::swap(lo, hi);
    if (compare(lo, begin) > 0)
      begin = lo;
    if (compare(hi, end) < 0)
      end = hi;
    if (compare(begin, end) >= 0)
      return;
  }
  const size_t bitBound =
      std::max(p.denominator.bits(),
               c.densityBits + 3 * p.weightDen.bits() + p.end.bits() + 32) +
      2200;
  if (bitBound >= Big::Limbs * 32)
    throw std::invalid_argument(
        "fog rational workspace exceeds fixed integer proof");
  const uint64_t limbs = (bitBound + 31) / 32;
  p.cellWork = 512ull * Big::Limbs + 512ull * limbs * limbs;
  if (c.deviation.empty())
    return;
  if (begin.negative || end.negative)
    throw std::logic_error("negative clipped fog interval");
  p.begin = begin.magnitude;
  p.end = end.magnitude;
  p.interior = true;
  p.bound = 4;
  auto delta = p.end;
  delta.subtract(p.begin);
  auto denom = shifted(p.lambda, size_t(F));
  for (int a = 0; a < 3; ++a)
    p.bound = add(
        p.bound, quotient64(times(p.dScaled[a].magnitude, delta), denom, true));
}

SignedBig coordinate(const Plan &p, int axis, const Big &timeTwice) {
  auto q = signedTimes(p.qBase[axis], Big(2));
  append(q, signedTimes(p.dScaled[axis], timeTwice));
  return q;
}
int32_t floorCoordinate(const SignedBig &x, const Big &den,
                        bool negativeDirection) {
  const auto q = quotient64(x.magnitude, den);
  auto rem = x.magnitude;
  rem.subtract(times(den, q));
  if (q > uint64_t(INT32_MAX) + 1)
    throw std::invalid_argument("fog cell coordinate exceeds Int32");
  int64_t v = x.negative ? -int64_t(q) - int(!rem.zero()) : int64_t(q);
  if (negativeDirection && rem.zero())
    --v;
  if (v < INT32_MIN + 1LL || v > INT32_MAX - 1LL)
    throw std::invalid_argument("fog cell lacks Int32 corner margins");
  return int32_t(v);
}
Big polynomial(const Plan &p, const std::array<double, 8> &values,
               const Big &twiceTime, const openvdb::Coord &cell,
               int densityUnit) {
  std::array<std::array<Big, 2>, 3> w;
  for (int a = 0; a < 3; ++a) {
    auto n = coordinate(p, a, twiceTime);
    const int64_t base = cell[a];
    n.add(times(p.weightDen, magnitude(base)), base >= 0);
    if (n.negative || n.magnitude.compare(p.weightDen) > 0)
      throw std::invalid_argument("unresolved exact fog cell coordinate");
    w[a][1] = n.magnitude;
    w[a][0] = p.weightDen;
    w[a][0].subtract(n.magnitude);
  }
  Big sum;
  for (int i = 0; i < 8; ++i) {
    auto term = units(binary(values[i]), densityUnit);
    term = times(term, w[0][(i >> 2) & 1]);
    term = times(term, w[1][(i >> 1) & 1]);
    term = times(term, w[2][i & 1]);
    sum.add(term);
  }
  return sum;
}
template <class G>
FogRayIntegral integrate(const Plan &p, const G &grid, const Census &c,
                         const FogRayIntegralOptions &o) {
  FogRayIntegral out;
  out.normalizedDirection = p.ray.direction;
  Big total;
  const auto densityBG =
      units(binary(double(grid.background())), c.densityUnit);
  const auto outside = [&](Big length) {
    auto n = times(densityBG, length);
    n = times(n, p.outsideFactor);
    total.add(n);
  };
  if (!p.interior) {
    auto length = p.end;
    length.subtract(p.begin);
    outside(length);
  } else {
    // Endpoint time conversion uses the factored odd product already
    // represented by lambda; reconstruct Double endpoints exactly with exponent
    // scaling.
    const auto exactTime = [&](double t) {
      const auto f = binary(t);
      auto n = times(Big(f.sig), p.lambda);
      if (f.unit >= 0)
        n.shift(size_t(f.unit));
      else {
        const auto shift = size_t(-f.unit);
        if (n.trailing() < shift)
          throw std::logic_error("fog endpoint divisibility proof failed");
        n.right(shift);
      }
      return n;
    };
    auto prefix = p.begin;
    prefix.subtract(exactTime(p.ray.minimum));
    outside(prefix);
    auto suffix = exactTime(p.ray.maximum);
    suffix.subtract(p.end);
    outside(suffix);
    using Accessor =
        openvdb::tree::ValueAccessor<const typename G::TreeType, false>;
    Accessor accessor(grid.tree());
    openvdb::Coord cell;
    const auto initial = shifted(p.begin, 1);
    for (int a = 0; a < 3; ++a)
      cell[a] = floorCoordinate(coordinate(p, a, initial), p.weightDen,
                                p.d[a].negative);
    Big current = p.begin;
    while (current.compare(p.end) < 0) {
      if (++out.cellSegments > p.bound)
        throw std::logic_error("fog traversal exceeded prospective proof");
      Big next = p.end;
      std::array<SignedBig, 3> events;
      for (int a = 0; a < 3; ++a)
        if (!p.d[a].sig.zero()) {
          events[a] = p.plane(a, int64_t(cell[a]) + (!p.d[a].negative ? 1 : 0));
          if (events[a].negative || events[a].magnitude.compare(current) <= 0)
            throw std::invalid_argument(
                "nonprogressing exact fog lattice event");
          if (events[a].magnitude.compare(next) < 0)
            next = events[a].magnitude;
        }
      auto delta = next;
      delta.subtract(current);
      if (delta.zero())
        throw std::invalid_argument("unresolved positive fog sliver");
      std::array<double, 8> values;
      for (int i = 0; i < 8; ++i)
        values[i] = double(accessor.getValue(
            cell + openvdb::Coord((i >> 2) & 1, (i >> 1) & 1, i & 1)));
      bool constant = true;
      for (double x : values)
        constant &= x == values[0];
      if (constant) {
        auto n = times(units(binary(values[0]), c.densityUnit), delta);
        n = times(n, p.outsideFactor);
        total.add(n);
      } else {
        auto s =
            polynomial(p, values, shifted(current, 1), cell, c.densityUnit);
        auto middle = current;
        middle.add(next);
        auto m = polynomial(p, values, middle, cell, c.densityUnit);
        m.shift(2);
        s.add(m);
        s.add(polynomial(p, values, shifted(next, 1), cell, c.densityUnit));
        total.add(times(s, delta));
      }
      if (next.compare(p.end) == 0)
        break;
      for (int a = 0; a < 3; ++a)
        if (!p.d[a].sig.zero() && !events[a].negative &&
            events[a].magnitude.compare(next) == 0) {
          const int64_t updated =
              int64_t(cell[a]) + (!p.d[a].negative ? 1 : -1);
          if (updated < INT32_MIN + 1LL || updated > INT32_MAX - 1LL)
            throw std::invalid_argument("fog next cell lacks Int32 margins");
          cell[a] = int32_t(updated);
        }
      current = next;
    }
  }
  out.densityIntegral = ratio(total, p.denominator, c.densityUnit);
  const auto sigma = binary(o.extinction);
  out.opticalDepth = ratio(times(total, Big(sigma.sig)), p.denominator,
                           c.densityUnit + sigma.unit);
  out.transmittance = std::exp(-out.opticalDepth);
  out.absorbedFraction = -std::expm1(-out.opticalDepth);
  if (!out.transmittance) {
    if (o.transmissionPolicy == FogTransmissionPolicy::RejectUnderflow)
      throw std::invalid_argument(
          "fog transmission underflows; opt in to opaque underflow explicitly");
    out.transmissionStatus = FogTransmissionStatus::OpaqueUnderflow;
  }
  return out;
}
std::vector<RayQueryMetadata> summaries(const openvdb::MetaMap &meta) {
  std::vector<RayQueryMetadata> out;
  for (auto i = meta.beginMeta(); i != meta.endMeta(); ++i) {
    const auto &m = *i->second;
    RayQueryMetadata item{
        i->first, m.typeName(), {}, uint64_t(m.size()), false};
    if (dynamic_cast<const openvdb::StringMetadata *>(&m) ||
        dynamic_cast<const openvdb::BoolMetadata *>(&m) ||
        dynamic_cast<const openvdb::Int32Metadata *>(&m) ||
        dynamic_cast<const openvdb::Int64Metadata *>(&m) ||
        dynamic_cast<const openvdb::FloatMetadata *>(&m) ||
        dynamic_cast<const openvdb::DoubleMetadata *>(&m) ||
        dynamic_cast<const openvdb::Vec3IMetadata *>(&m) ||
        dynamic_cast<const openvdb::Vec3SMetadata *>(&m) ||
        dynamic_cast<const openvdb::Vec3DMetadata *>(&m))
      item.value = m.str();
    else
      item.opaque = true;
    out.push_back(std::move(item));
  }
  return out;
}
struct Description {
  Common ray;
  uint64_t bound = 0, work = 0;
};
template <class G>
FogRayIntegralBatch batch(const G &grid, const Loaded &loaded,
                          std::span<const WorldRay> rays,
                          const FogRayIntegralOptions &options,
                          const ProcessingOptions &p, Work &work) {
  constexpr uint64_t SetupWork = 4 * 1024 * 1024;
  work.add(SetupWork);
  const Map map(grid);
  const auto c = census(grid, loaded, p, work);
  std::vector<Description> plans;
  plans.reserve(rays.size());
  FogRayIntegralBatch out;
  out.rays.reserve(rays.size());
  bytes(loaded.resident + plans.capacity() * 1.0L * sizeof(Description) +
            out.rays.capacity() * 1.0L * sizeof(FogRayIntegral),
        p, "fog actual result/plan capacity exceeds shared bytes");
  // One heap-owned context, reused in both planning and integration passes.
  static_assert(sizeof(Plan) < 256 * 1024);
  auto context = std::make_unique<Plan>();
  for (const auto &r : rays) {
    work.add(4.0L * SetupWork);
    const auto d = common(r, map);
    prepare(*context, d, c, map);
    if (context->bound > options.maxCellSegmentsPerRay)
      throw std::invalid_argument(
          "fog prospective cells exceed maxCellSegmentsPerRay");
    out.cellSegmentUpperBound = add(out.cellSegmentUpperBound, context->bound);
    if (out.cellSegmentUpperBound > options.maxCellSegments)
      throw std::invalid_argument(
          "fog prospective cells exceed maxCellSegments");
    work.add(context->cellWork * 1.0L * context->bound);
    plans.push_back({d, context->bound, context->cellWork});
  }
  out.backgroundDensity = double(grid.background());
  out.numericallyConstant = c.deviation.empty();
  out.source.descriptorName = loaded.descriptor;
  out.source.gridName = grid.getName();
  out.source.valueType = grid.valueType();
  out.source.empty = c.active == 0;
  out.source.inputActiveVoxelCount = c.active;
  out.source.inputActiveTileCount = c.tiles;
  out.source.inputStoredLeafCount = c.leaves;
  for (int r = 0; r < 3; ++r) {
    for (int col = 0; col < 3; ++col)
      out.source.indexToWorld[4 * r + col] = map.basis[col][r];
    out.source.indexToWorld[4 * r + 3] = map.origin[r];
  }
  out.source.indexToWorld[15] = 1;
  out.source.metadata = summaries(grid);
  out.source.fileMetadata = summaries(*loaded.fileMeta);
  for (const auto &d : plans) {
    prepare(*context, d.ray, c, map);
    if (context->bound != d.bound || context->cellWork != d.work)
      throw std::logic_error("fog plan changed across bounded passes");
    auto result = integrate(*context, grid, c, options);
    out.cellSegments = add(out.cellSegments, result.cellSegments);
    out.rays.push_back(std::move(result));
  }
  out.workItems = out.source.workItems = work.used;
  return out;
}
#endif
} // namespace
FogRayIntegralBatch IntegrateFogRays(const std::filesystem::path &input,
                                     std::span<const WorldRay> rays,
                                     const FogRayIntegralOptions &options,
                                     const std::string &inputGrid,
                                     const ProcessingOptions &processing) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;
  (void)rays;
  (void)options;
  (void)inputGrid;
  (void)processing;
  unavailable();
#else
  if (!processing.maxInputBytes || !processing.maxVoxelCount ||
      !options.limits.maxRays || options.limits.maxRays > 1000000 ||
      rays.size() > options.limits.maxRays || !options.limits.maxGrids ||
      options.limits.maxGrids > 4096 || !options.limits.maxMetadataBytes ||
      !options.limits.maxWorkItems ||
      options.limits.maxWorkItems > 10000000000ull ||
      !options.maxCellSegments || options.maxCellSegments > 1000000 ||
      !options.maxCellSegmentsPerRay ||
      options.maxCellSegmentsPerRay > 1000000 ||
      !std::isfinite(options.extinction) || options.extinction < 0 ||
      (options.transmissionPolicy != FogTransmissionPolicy::RejectUnderflow &&
       options.transmissionPolicy !=
           FogTransmissionPolicy::AllowOpaqueUnderflow))
    throw std::invalid_argument("invalid fog integral options/limits");
  const long double reserve =
      2.0L * rays.size() * (sizeof(Description) + sizeof(FogRayIntegral)) +
      rays.size() * 1.0L * sizeof(WorldRay) + 8.0L * inputGrid.size() +
      16.0L * input.native().size() + 1024 * 1024;
  bytes(Fixed + reserve, processing,
        "fog query/coordinate workspace exceeds bytes");
  ProcessingOptions local;
  local.maxInputBytes = processing.maxInputBytes;
  local.maxVoxelCount = processing.maxVoxelCount;
  tbb::task_arena arena(1);
  return arena.execute([&] {
    openvdb::initialize();
    Work work{0, options.limits.maxWorkItems};
    work.add(reserve * 4);
    Snapshot snapshot(input, local, work, uint64_t(std::ceil(reserve)));
    auto loaded = load(snapshot, options.limits, inputGrid, local,
                       uint64_t(std::ceil(reserve)), work);
    if (loaded.grid->isType<openvdb::FloatGrid>())
      return batch(static_cast<const openvdb::FloatGrid &>(*loaded.grid),
                   loaded, rays, options, local, work);
    return batch(static_cast<const openvdb::DoubleGrid &>(*loaded.grid), loaded,
                 rays, options, local, work);
  });
#endif
}
} // namespace container::geometry::vdb
