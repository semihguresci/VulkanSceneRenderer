#include "Container/geometry/OpenVdbRayQueries.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/OpenVdbGridSelection.h"
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/math/Stencils.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/Prune.h>
#include <openvdb/tools/RayIntersector.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#include <type_traits>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB ray queries are unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed = 4.0L * 1024 * 1024;
using Mask = openvdb::BoolGrid;
using Tree = Mask::TreeType;
using Leaf = Tree::LeafNodeType;
using Upper = Tree::RootNodeType::ChildNodeType;
using Lower = Upper::ChildNodeType;
void bytes(long double n, const ProcessingOptions &p, const char *why) {
  if (!std::isfinite(n) || n > p.maxInputBytes)
    throw std::invalid_argument(why);
}
long double metadata(const openvdb::MetaMap &m) {
  long double n = sizeof(m);
  for (auto it = m.beginMeta(); it != m.endMeta(); ++it) {
    if (!it->second)
      throw std::invalid_argument("null ray queries metadata");
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
      throw std::invalid_argument("ray queries operation exceeds maxWorkItems");
    used += uint64_t(std::ceil(n));
  }
};
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p,
           Work &work) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument("ray queries input must be a regular file");
    encoded = std::filesystem::file_size(input);
    work.add(encoded * 4.0L); // snapshot I/O plus subsequent catalog reads
    bytes(Fixed + encoded, p,
          "encoded ray queries source exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned n = 0; n < 64; ++n) {
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-ray-query-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        file = directory / "source.vdb";
        break;
      }
    }
    if (file.empty())
      throw std::runtime_error("cannot reserve ray queries snapshot");
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open ray queries snapshot");
      std::array<char, 65536> buffer{};
      auto left = encoded;
      while (left) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(left, buffer.size()));
        source.read(buffer.data(), count);
        if (source.gcount() != count)
          throw std::runtime_error("ray queries source changed length");
        target.write(buffer.data(), count);
        left -= count;
      }
      target.close();
      if (!target || source.bad() ||
          source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "ray queries snapshot failed or source changed length");
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
template <class F>
auto visit(const openvdb::GridBase &g, F f)
    -> decltype(f(static_cast<const openvdb::FloatGrid &>(g))) {
#define T(G)                                                                   \
  if (g.isType<openvdb::G>())                                                  \
  return f(static_cast<const openvdb::G &>(g))
  T(FloatGrid);
  T(DoubleGrid);
  T(BoolGrid);
  T(MaskGrid);
  T(Int32Grid);
  T(Int64Grid);
  T(Vec3IGrid);
  T(Vec3SGrid);
  T(Vec3DGrid);
#undef T
  throw std::invalid_argument(
      "ray queries support the nine standard native grid types");
}
bool signedType(const openvdb::GridBase &g) {
  return (g.isType<openvdb::FloatGrid>() || g.isType<openvdb::DoubleGrid>()) &&
         g.getGridClass() == openvdb::GRID_LEVEL_SET;
}
bool standardType(const openvdb::GridBase &g) {
#define TYPE(G)                                                                \
  if (g.isType<openvdb::G>())                                                  \
  return true
  TYPE(FloatGrid);
  TYPE(DoubleGrid);
  TYPE(BoolGrid);
  TYPE(MaskGrid);
  TYPE(Int32Grid);
  TYPE(Int64Grid);
  TYPE(Vec3IGrid);
  TYPE(Vec3SGrid);
  TYPE(Vec3DGrid);
#undef TYPE
  return false;
}
void structuralSemantics(const openvdb::GridBase &g) {
  if (const auto value = g[openvdb::GridBase::META_VECTOR_TYPE]) {
    const auto text =
        std::dynamic_pointer_cast<const openvdb::StringMetadata>(value);
    if (!text)
      throw std::invalid_argument("ray-query vector semantic metadata must "
                                  "have native string type");
    auto semantic = text->value();
    openvdb::string::trim(semantic);
    openvdb::string::to_lower(semantic);
    if (semantic != openvdb::GridBase::vecTypeToString(openvdb::VEC_INVARIANT))
      throw std::invalid_argument(
          "ray queries require recognized invariant scalar semantics");
  }
  if (const auto value = g[openvdb::GridBase::META_IS_LOCAL_SPACE];
      value && !std::dynamic_pointer_cast<const openvdb::BoolMetadata>(value))
    throw std::invalid_argument("ray-query world/local semantic metadata "
                                "must have native Bool type");
  if (!g.isInWorldSpace())
    throw std::invalid_argument("ray queries require world-space scalars");
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
      throw std::invalid_argument("ray queries require supported seekable "
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
      throw std::invalid_argument("ray queries grid descriptor offsets are "
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
          "ray queries metadata/transform crosses grid payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};
Loaded load(const Snapshot &s, const RayQueryLimits &o,
            const std::string &selector, const ProcessingOptions &p, bool sdf,
            Work &work) {
  detail::RequireSeekableFile(s.file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded ray queries require delayed loading; "
                                "unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded ray queries require OpenVDB delayed-loading support");
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
  long double catalog = s.encoded + Fixed;
  auto check = [&] {
    if (r.meta > o.maxMetadataBytes)
      throw std::invalid_argument("aggregate ray queries file/grid metadata "
                                  "exceeds maxMetadataBytes");
    bytes(catalog + 8 * r.meta, p, "ray queries catalog exceeds memory budget");
  };
  check();
  std::set<std::string> descriptors;
  const int32_t gridCount = archive.count(stream);
  if (gridCount < 0 || uint32_t(gridCount) > o.maxGrids)
    throw std::invalid_argument("ray queries catalog exceeds maxGrids");
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
      throw std::invalid_argument(
          "ray queries input has colliding descriptors; rename grids first");
    const auto gridMetadata = metadata(*m);
    r.meta += gridMetadata;
    work.add(gridMetadata * 4);
    check();
    if (!GridSelectionMatches(selector, name, m->getName()))
      continue;
    const bool usable = sdf ? signedType(*m) : standardType(*m);
    if (!usable) {
      if (!selector.empty())
        throw std::invalid_argument(
            "ray queries selected unsupported native type/class");
      continue;
    }
    if (sdf)
      structuralSemantics(*m);
    if (found)
      throw std::invalid_argument("ambiguous ray queries selector; use "
                                  "descriptor:<exact descriptor>");
    const auto rawMemory = (*m)[openvdb::GridBase::META_FILE_MEM_BYTES],
               rawCount = (*m)[openvdb::GridBase::META_FILE_VOXEL_COUNT];
    const auto mem = std::dynamic_pointer_cast<const openvdb::Int64Metadata>(
                   rawMemory),
               count = std::dynamic_pointer_cast<const openvdb::Int64Metadata>(
                   rawCount);
    if ((rawMemory && !mem) || (rawCount && !count) ||
        (mem && mem->value() < 0) || (count && count->value() < 0))
      throw std::invalid_argument("ray-query optional saved statistics must be "
                                  "nonnegative native Int64 values");
    if (count && uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument(
          "recorded ray-query count exceeds maxVoxelCount");
    if (count && uint64_t(count->value()) > o.maxWorkItems)
      throw std::invalid_argument(
          "recorded ray queries source exceeds maxWorkItems before loading");
    bytes(catalog + 8 * r.meta + (mem ? mem->value() : 0), p,
          "recorded ray queries tree exceeds memory budget");
    found = true;
    chosen = name;
    r.descriptor = name;
    r.recorded = mem ? uint64_t(mem->value()) : 0;
    r.hasActiveHint = bool(count);
    r.recordedActive = count ? uint64_t(count->value()) : 0;
  }
  if (!found)
    throw std::invalid_argument("no matching supported ray queries grid");
  stream.close();
  work.add(r.recorded * 8.0L); // before decoded topology allocation
  openvdb::io::File file(s.file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error(
        "ray queries snapshot changed before decoded loading");
  r.grid = file.readGrid(chosen);
  const auto full = visit(*r.grid, [](const auto &g) {
    return openvdb::tools::memUsageIfLoaded(g.tree(), false);
  });
  if (full > r.recorded)
    work.add((full - r.recorded) * 8.0L); // before selected buffer paging
  r.resident = catalog + 8 * r.meta + full;
  if (static_cast<long double>(full) * 8 > o.maxWorkItems)
    throw std::invalid_argument("ray queries decoded source exceeds "
                                "maxWorkItems before materialization");
  bytes(r.resident, p,
        "fully decoded ray queries source exceeds memory budget before "
        "materialization");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full + 4 * r.meta)
    throw std::runtime_error("ray queries tree exceeded decoded reservation");
  file.close();
  return r;
#endif
}

using V = openvdb::Vec3d;
using Scalar = openvdb::DoubleGrid;
using Ray = openvdb::math::Ray<double>;
uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a)
    throw std::invalid_argument("ray query count overflows UInt64");
  return a + b;
}
uint64_t multiply(uint64_t a, uint64_t b) {
  if (a && b > UINT64_MAX / a)
    throw std::invalid_argument("ray query count overflows UInt64");
  return a * b;
}
uint64_t volume(const openvdb::CoordBBox &box) {
  uint64_t n = 1;
  for (int a = 0; a < 3; ++a) {
    const int64_t extent = int64_t(box.max()[a]) - box.min()[a] + 1;
    if (extent <= 0)
      throw std::invalid_argument("invalid ray-query cell box");
    n = multiply(n, uint64_t(extent));
  }
  return n;
}
double scaled(double value, double factor = 1, double divisor = 1) {
  if (!std::isfinite(value) || !std::isfinite(factor) ||
      !std::isfinite(divisor) || divisor == 0)
    throw std::invalid_argument("invalid ray-query numeric scaling");
  if (value == 0 || factor == 0)
    return std::copysign(0., value * factor);
  int a = 0, b = 0, c = 0, adjust = 0;
  double m =
      std::frexp(value, &a) * std::frexp(factor, &b) / std::frexp(divisor, &c);
  m = std::frexp(m, &adjust);
  const double out = std::ldexp(m, a + b - c + adjust);
  if (!std::isfinite(out) || out == 0)
    throw std::invalid_argument("ray-query conditioning/restoration loses "
                                "nonzero Double range or overflows");
  return out;
}
struct Sum {
  double value = 0, error = 0;
  void append(double x) {
    const double next = value + x;
    error += std::abs(value) >= std::abs(x) ? (value - next) + x
                                            : (x - next) + value;
    value = next;
  }
  double get() const {
    const double out = value + error;
    if (!std::isfinite(out))
      throw std::invalid_argument("ray-query sum overflows");
    return out;
  }
};
double gap(double x) {
  return std::max(std::abs(std::nextafter(x, INFINITY) - x),
                  std::abs(x - std::nextafter(x, -INFINITY)));
}
std::array<double, 3> array(const V &x) { return {x[0], x[1], x[2]}; }
V normalized(V x) {
  double peak = 0;
  for (int a = 0; a < 3; ++a) {
    if (!std::isfinite(x[a]))
      throw std::invalid_argument("nonfinite ray direction");
    peak = std::max(peak, std::abs(x[a]));
  }
  if (peak == 0)
    throw std::invalid_argument("ray direction must be nonzero");
  for (int a = 0; a < 3; ++a)
    x[a] = scaled(x[a], 1, peak);
  const double length = std::hypot(x[0], x[1], x[2]);
  for (int a = 0; a < 3; ++a)
    x[a] = scaled(x[a], 1, length);
  return x;
}
struct Map {
  V origin;
  std::array<V, 3> basis, inverse;
  double h = 0;
  explicit Map(const openvdb::GridBase &g) {
    if (!g.transform().isLinear())
      throw std::invalid_argument(
          "ray queries require uniform orthogonal affine maps");
    const auto map = g.transform().baseMap();
    origin = map->applyMap(V(0));
    for (int a = 0; a < 3; ++a) {
      V e(0);
      e[a] = 1;
      basis[a] = map->applyJacobian(e);
      inverse[a] = map->applyInverseJacobian(e);
      const double length = std::hypot(basis[a][0], basis[a][1], basis[a][2]);
      if (!std::isfinite(origin[a]) || !std::isfinite(length) ||
          length < 1e-6 || length > 1e6)
        throw std::invalid_argument(
            "ray-query affine axes must be finite in [1e-6,1e6]");
      if (a == 0)
        h = length;
      else if (std::abs(length / h - 1) > 1e-12)
        throw std::invalid_argument("ray queries require uniform voxels");
      V unit;
      for (int r = 0; r < 3; ++r) {
        unit[r] = scaled(basis[a][r], 1, h);
        if (!std::isfinite(inverse[a][r]))
          throw std::invalid_argument("nonfinite ray-query inverse Jacobian");
      }
      for (int b = 0; b < a; ++b) {
        V other;
        for (int r = 0; r < 3; ++r)
          other[r] = scaled(basis[b][r], 1, h);
        if (std::abs(unit.dot(other)) > 1e-12)
          throw std::invalid_argument("ray queries require orthogonal voxels");
      }
    }
  }
  void spacing(const V &x) const {
    for (int a = 0; a < 3; ++a)
      if (!std::isfinite(x[a]) || gap(x[a]) > h * 1e-4)
        throw std::invalid_argument(
            "ray world coordinates lack 1e-4 voxel precision");
  }
  V indexVector(const V &world) const {
    V result;
    for (int r = 0; r < 3; ++r) {
      Sum sum;
      for (int c = 0; c < 3; ++c)
        sum.append(scaled(world[c], inverse[c][r]));
      result[r] = sum.get();
    }
    return result;
  }
  V indexPosition(const V &world) const {
    spacing(world);
    spacing(origin);
    V delta;
    for (int a = 0; a < 3; ++a)
      delta[a] = world[a] - origin[a];
    return indexVector(
        delta); // centered inverse, no affine translation cancellation
  }
  V position(const V &index) const {
    V result;
    for (int r = 0; r < 3; ++r) {
      Sum sum;
      sum.append(origin[r]);
      for (int c = 0; c < 3; ++c) {
        const double product = scaled(index[c], basis[c][r]);
        sum.append(product);
        sum.append(std::fma(index[c], basis[c][r], -product));
        if (gap(product) > h * 1e-4)
          throw std::invalid_argument(
              "ray affine products lose 1e-4 voxel precision");
      }
      result[r] = sum.get();
    }
    spacing(result);
    return result;
  }
  V normal(const V &gradient) const {
    V result;
    for (int r = 0; r < 3; ++r) {
      Sum sum;
      for (int c = 0; c < 3; ++c)
        sum.append(scaled(gradient[c], inverse[r][c]));
      result[r] = sum.get();
    }
    return normalized(result);
  }
};
void validate(const RayQueryLimits &o, std::span<const WorldRay> rays,
              const ProcessingOptions &p) {
  if (!p.maxInputBytes || !p.maxVoxelCount || !o.maxRays ||
      o.maxRays > 1'000'000 || rays.size() > o.maxRays || !o.maxGrids ||
      o.maxGrids > 4096 || !o.maxMetadataBytes || !o.maxWorkItems ||
      o.maxWorkItems > 10'000'000'000ull)
    throw std::invalid_argument("invalid ray-query limits or ray count");
}
struct Census {
  uint64_t active = 0, tiles = 0, leaves = 0, slots = 0;
  openvdb::CoordBBox stored, activeBox;
  openvdb::Coord anchor{0};
  long double treeUpper = 0, reserve = 0;
};
template <class G>
Census census(const G &g, const Loaded &loaded, const Map &map,
              const ProcessingOptions &p, Work &work, uint64_t resultBytes) {
  Census c;
  // A typed full-memory byte scan pays for every bit/slot before iteration,
  // including wholly inactive remote internal tables and leaf buffers.
  const auto full = openvdb::tools::memUsageIfLoaded(g.tree(), false);
  (void)full; // full typed byte/table work was charged before paging
  const auto nodes = g.tree().nodeCount();
  using DTree = Scalar::TreeType;
  using DL = DTree::LeafNodeType;
  using DU = DTree::RootNodeType::ChildNodeType;
  using DD = DU::ChildNodeType;
  uint64_t roots = 0;
  for (auto it = g.tree().cbeginRootDense(); it; ++it)
    roots = add(roots, 1);
  c.leaves = nodes[0];
  c.slots = multiply(c.leaves, 512);
  if (c.slots > p.maxVoxelCount)
    throw std::invalid_argument(
        "ray-query stored leaf slots exceed maxVoxelCount");
  c.treeUpper = sizeof(DTree) + sizeof(DTree::RootNodeType) + roots * 512.0L +
                nodes[0] * (sizeof(DL) + 512.0L * sizeof(double) + 512) +
                nodes[1] * (sizeof(DD) + 512.0L) +
                nodes[2] * (sizeof(DU) + 512.0L);
  // Root-aligned recentering preserves all node/leaf alignment. Rebuilding can
  // use <=one tree of these counts; Boolean topology copy <=one; its native
  // intersector clone <=one. Serial prune node-pointer caches fit another tree.
  // No dilation, resampling, leaf expansion, output writer or simultaneous
  // second recentering is performed. 16*T bounds these <=4*T actual trees,
  // allocator/root-container slack and all fixed accessors/stencils (<Fixed).
  c.reserve = loaded.resident + 8 * loaded.meta + 16 * c.treeUpper + Fixed +
              resultBytes;
  bytes(c.reserve, p,
        "ray-query prospective trees/caches/results exceed shared bytes");
  work.add(64.0L *
           (roots + nodes[0] * 512.0L + nodes[1] * DD::NUM_VALUES * 1.0L +
            nodes[2] * DU::NUM_VALUES * 1.0L));
  for (auto value = g.cbeginValueAll(); value; ++value) {
    const auto box = value.getBoundingBox();
    for (int a = 0; a < 3; ++a)
      if (box.min()[a] < INT32_MIN + 8192LL ||
          box.max()[a] > INT32_MAX - 8192LL)
        throw std::invalid_argument(
            "ray-query source coordinates lack native node margins");
    c.stored.expand(box);
    if (value.isValueOn()) {
      c.active = add(c.active, volume(box));
      if (!value.isVoxelValue())
        c.tiles = add(c.tiles, 1);
      c.activeBox.expand(box);
    }
  }
  if (c.active > p.maxVoxelCount)
    throw std::invalid_argument(
        "ray-query weighted active count exceeds maxVoxelCount");
  if (loaded.hasActiveHint && loaded.recordedActive != c.active)
    throw std::invalid_argument(
        "saved ray-query active count disagrees with checked topology");
  if (!c.stored.empty())
    for (int a = 0; a < 3; ++a) {
      const int64_t center =
          int64_t(c.stored.min()[a]) +
          (int64_t(c.stored.max()[a]) - c.stored.min()[a]) / 2;
      const int64_t root =
          center >= 0 ? center / 4096 : (center + 1) / 4096 - 1;
      c.anchor[a] = int32_t(root * 4096);
      for (const int64_t endpoint :
           {int64_t(c.stored.min()[a]), int64_t(c.stored.max()[a])})
        if (endpoint - c.anchor[a] < INT32_MIN + 8192LL ||
            endpoint - c.anchor[a] > INT32_MAX - 8192LL)
          throw std::invalid_argument(
              "recentered ray-query coordinates exceed safe native range");
    }
  (void)map;
  return c;
}
std::vector<RayQueryMetadata> summaries(const openvdb::MetaMap &meta) {
  std::vector<RayQueryMetadata> result;
  for (auto it = meta.beginMeta(); it != meta.endMeta(); ++it) {
    const auto &m = *it->second;
    RayQueryMetadata item{
        it->first, m.typeName(), {}, uint64_t(m.size()), false};
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
    result.push_back(std::move(item));
  }
  return result;
}
RayQuerySource sourceInfo(const Loaded &loaded, const Map &map,
                          const Census &c) {
  RayQuerySource out;
  out.descriptorName = loaded.descriptor;
  out.gridName = loaded.grid->getName();
  out.valueType = loaded.grid->valueType();
  out.empty = c.active == 0;
  out.inputActiveVoxelCount = c.active;
  out.inputActiveTileCount = c.tiles;
  out.inputStoredLeafCount = c.leaves;
  for (int r = 0; r < 3; ++r) {
    for (int col = 0; col < 3; ++col)
      out.indexToWorld[4 * r + col] = map.basis[col][r];
    out.indexToWorld[4 * r + 3] = map.origin[r];
  }
  out.indexToWorld[15] = 1;
  out.metadata = summaries(*loaded.grid);
  out.fileMetadata = summaries(*loaded.fileMeta);
  return out;
}

openvdb::Coord local(const openvdb::Coord &coord, const Census &c) {
  openvdb::Coord out;
  for (int a = 0; a < 3; ++a) {
    const int64_t n = int64_t(coord[a]) - c.anchor[a];
    if (n < INT32_MIN + 8192LL || n > INT32_MAX - 8192LL)
      throw std::invalid_argument(
          "local ray node coordinate lacks native margin");
    out[a] = int32_t(n);
  }
  return out;
}
class ConditionedGrid final : public Scalar {
public:
  explicit ConditionedGrid(double background)
      : Scalar(std::make_shared<Scalar::TreeType>(background)) {}
  // Owned by one batch/one native intersector on its single worker. This is
  // search output, not source metadata; no global/thread-local callback state.
  mutable SdfRayHitKind lastKind{SdfRayHitKind::Miss};
};
template <class G>
Scalar::Ptr conditioned(const G &g, const Map &map, const Census &c,
                        const LevelSetRayOptions &o, Work &work) {
  const double bg = double(g.background());
  const double band = scaled(bg, 1, map.h), iso = scaled(o.isovalue, 1, map.h);
  if (!std::isfinite(bg) || bg <= 0 || band < 3 - 1e-6 || band > 64 + 1e-6 ||
      c.tiles || std::abs(iso) > band - 2)
    throw std::invalid_argument(
        "SDF rays require symmetric band[3,64], no active "
        "tiles and isovalue at least two voxels inside band");
  work.add(c.slots * 32.0L + c.active * 64.0L);
  bool negative = false;
  for (auto value = g.cbeginValueAll(); value; ++value) {
    const double x = double(*value);
    if (!std::isfinite(x) || std::abs(x) > bg * (1 + 1e-6) ||
        (!value.isValueOn() && std::abs(x) != bg))
      throw std::invalid_argument(
          "SDF rays require finite distances and signed "
          "inactive band backgrounds");
    negative |= x < 0;
  }
  if (!c.active && negative)
    throw std::invalid_argument(
        "inactive negative topology without active SDF is not empty");
  auto result = std::make_shared<ConditionedGrid>(band);
  result->setGridClass(openvdb::GRID_LEVEL_SET);
  result->insertMeta("container:ray_refinements",
                     openvdb::Int32Metadata(o.refinementSteps));
  auto out = result->getAccessor();
  for (auto value = g.cbeginValueAll(); value; ++value) {
    if (!value.isValueOn() && double(*value) == bg)
      continue;
    const double x = scaled(double(*value), 1, map.h);
    if (value.isVoxelValue()) {
      if (value.isValueOn())
        out.setValue(local(value.getCoord(), c), x);
      else
        out.setValueOff(local(value.getCoord(), c), x);
    } else {
      const auto box = value.getBoundingBox();
      result->tree().fill(
          openvdb::CoordBBox(local(box.min(), c), local(box.max(), c)), x,
          value.isValueOn());
    }
  }
  if (c.active) {
    bool crossing = false;
    for (auto it = result->cbeginValueOn(); it && !crossing; ++it)
      for (int a = 0; a < 3; ++a)
        for (int sign : {-1, 1}) {
          auto next = it.getCoord();
          next[a] += sign;
          const double x = *it, y = out.getValue(next);
          crossing |= x == 0 || y == 0 || std::signbit(x) != std::signbit(y);
        }
    if (!crossing)
      throw std::invalid_argument(
          "nonempty ray SDF lacks a represented signed crossing");
  }
  return result;
}
template <class G>
Mask::Ptr occupancy(const G &g, const Census &c, Work &work) {
  work.add(c.slots * 16.0L + c.active * 0.0L + 1024);
  auto mask = Mask::create(false);
  auto out = mask->getAccessor();
  // Only active states are copied. Inactive-only leaves never survive into the
  // native volume leaf-existence test; active false/NaN still create true
  // cells.
  for (auto value = g.cbeginValueOn(); value; ++value) {
    if (value.isVoxelValue())
      out.setValue(local(value.getCoord(), c), true);
    else {
      const auto box = value.getBoundingBox();
      mask->tree().fill(
          openvdb::CoordBBox(local(box.min(), c), local(box.max(), c)), true,
          true);
    }
  }
  openvdb::tools::pruneInactive(mask->tree(), false);
  return mask;
}

struct Prepared {
  V direction{}, indexDirection{}, indexStart{};
  double start = 0, end = 0, indexPerWorld = 1;
  Ray ray;
  uint64_t bound = 0;
  bool hit = false;
};

double interpolation(openvdb::math::BoxStencil<Scalar> &stencil,
                     const V &position, double isovalue) {
  stencil.moveTo(openvdb::Coord::floor(position));
  std::array<double, 8> values;
  double peak = 0;
  for (unsigned i = 0; i < 8; ++i) {
    values[i] = stencil.getValue(i) - isovalue;
    peak = std::max(peak, std::abs(values[i]));
  }
  if (peak == 0)
    return 0;
  for (auto &v : values)
    v = scaled(v, 1, peak);
  stencil.template setValue<0, 0, 0>(values[0]);
  stencil.template setValue<0, 0, 1>(values[1]);
  stencil.template setValue<0, 1, 1>(values[2]);
  stencil.template setValue<0, 1, 0>(values[3]);
  stencil.template setValue<1, 0, 0>(values[4]);
  stencil.template setValue<1, 0, 1>(values[5]);
  stencil.template setValue<1, 1, 1>(values[6]);
  stencil.template setValue<1, 1, 0>(values[7]);
  return scaled(stencil.interpolation(position), peak);
}
Prepared prepare(const WorldRay &input, const Map &map, const Census &c,
                 bool volumeQuery, uint32_t refinements, Work &work) {
  Prepared p;
  if (!std::isfinite(input.minimumDistance) ||
      !std::isfinite(input.maximumDistance) || input.minimumDistance < 0 ||
      input.maximumDistance <= input.minimumDistance)
    throw std::invalid_argument(
        "ray distances must be finite, nonnegative and increasing");
  if (gap(input.maximumDistance) > map.h * 1e-4)
    throw std::invalid_argument(
        "ray distance spacing lacks 1e-4 voxel precision");
  p.direction = normalized(V(input.direction.data()));
  const V origin = map.indexPosition(V(input.origin.data()));
  const V derivative = map.indexVector(p.direction);
  p.indexPerWorld = std::hypot(derivative[0], derivative[1], derivative[2]);
  if (!std::isfinite(p.indexPerWorld) || p.indexPerWorld <= 0)
    throw std::invalid_argument("invalid transformed ray direction");
  p.indexDirection = normalized(derivative);
  for (int a = 0; a < 3; ++a)
    if (p.indexDirection[a] != 0 && !std::isfinite(1 / p.indexDirection[a]))
      throw std::invalid_argument(
          "ray direction reciprocal exceeds native DDA range");
  // Even miss/empty queries validate requested endpoints and arithmetic. Native
  // queries only traverse the separately clipped interval below.
  for (double s : {input.minimumDistance, input.maximumDistance}) {
    V world;
    for (int a = 0; a < 3; ++a)
      world[a] = std::fma(p.direction[a], s, input.origin[a]);
    map.spacing(world);
  }
  p.start = input.minimumDistance;
  p.end = input.maximumDistance;
  if (!c.active)
    return p;
  V lo, hi;
  for (int a = 0; a < 3; ++a) {
    const int64_t min = c.activeBox.min()[a], max = c.activeBox.max()[a];
    const auto leaf = [](int64_t x) {
      return (x >= 0 ? x / 8 : (x + 1) / 8 - 1) * 8;
    };
    lo[a] = double(leaf(min)) - (volumeQuery ? .5 : 0);
    hi[a] = double(leaf(max) + 7) + (volumeQuery ? .5 : 0);
    if (derivative[a] == 0) {
      if (origin[a] < lo[a] || origin[a] > hi[a])
        return p;
    } else {
      double a0 = (lo[a] - origin[a]) / derivative[a];
      double a1 = (hi[a] - origin[a]) / derivative[a];
      if (a0 > a1)
        std::swap(a0, a1);
      p.start = std::max(p.start, a0);
      p.end = std::min(p.end, a1);
      if (p.end < p.start)
        return p;
    }
  }
  if (p.end <= p.start)
    return p; // grazing single point: native interval unavailable
  const double length = scaled(p.end - p.start, p.indexPerWorld);
  if (length <= 2e-9)
    throw std::invalid_argument(
        "ray clipped interval is below native index-distance resolution");
  V end, eye;
  p.bound = 8;
  for (int a = 0; a < 3; ++a) {
    const double x = std::fma(derivative[a], p.start, origin[a]);
    const double y = std::fma(derivative[a], p.end, origin[a]);
    const double error = gap(origin[a]) + gap(derivative[a] * p.end) + gap(y);
    if (!std::isfinite(x) || !std::isfinite(y) || error > 1e-4 ||
        x < INT32_MIN + 8192. || y < INT32_MIN + 8192. ||
        x > INT32_MAX - 8192. || y > INT32_MAX - 8192.)
      throw std::invalid_argument(
          "ray endpoints lack safe native coordinate precision");
    p.indexStart[a] = x - c.anchor[a] + (volumeQuery ? .5 : 0);
    end[a] = y - c.anchor[a] + (volumeQuery ? .5 : 0);
    if (p.indexStart[a] < INT32_MIN + 8192. || end[a] < INT32_MIN + 8192. ||
        p.indexStart[a] > INT32_MAX - 8192. || end[a] > INT32_MAX - 8192.)
      throw std::invalid_argument(
          "recentered ray endpoints lack native margins");
    eye[a] = p.indexStart[a] - p.indexDirection[a];
    const double n = std::ceil(std::abs(end[a] - p.indexStart[a])) + 2;
    if (!std::isfinite(n) || n > double(UINT64_MAX))
      throw std::invalid_argument("ray traversal bound overflows");
    p.bound = add(p.bound, uint64_t(n));
  }
  const double endTime = 1 + length;
  if (!std::isfinite(endTime) || gap(endTime) > 1e-4 || endTime <= 1)
    throw std::invalid_argument(
        "native ray time cannot progress at voxel resolution");
  p.ray = Ray(eye, p.indexDirection, 1, endTime);
  // At each HDDA level every recursive segment is a subinterval of this line.
  // Summed fine-grid crossings plus at most two visits per parent boundary are
  // <16*B over all four levels (4096,128,8,1). Unit direction ensures each
  // positive axis DDA increment >=1, larger than checked time ULP. Each leaf
  // test/refinement touches at most8 corners plus bounded scalar work. 512 per
  // refinement phase exceeds these native/accessor/stencil/result operations.
  work.add(p.bound * 512.0L * (volumeQuery ? 1 : refinements + 2));
  p.hit = true;
  return p;
}

// Adapted from OpenVDB12 tools/RayIntersector.h LinearSearchImpl.
// Copyright Contributors to the OpenVDB Project. SPDX-License-Identifier:
// Apache-2.0 Native traversal and interpolation remain approximate; checked
// signs replace native ZeroCrossing(a,b)'s underflow-prone product. Never call
// time-only API.
class CheckedSearch {
public:
  using RayT = Ray;
  using VecT = V;
  using TreeT = Scalar::TreeType;
  explicit CheckedSearch(const Scalar &grid, double iso = 0)
      : stencil(grid), isovalue(iso), minimum(iso - 2), maximum(iso + 2),
        context(dynamic_cast<const ConditionedGrid *>(&grid)) {
    if (!context)
      throw std::logic_error("ray search needs an owned conditioned grid");
    grid.tree().root().evalActiveBoundingBox(box, false);
    const auto n =
        grid.getMetadata<openvdb::Int32Metadata>("container:ray_refinements");
    refinements = n ? uint32_t(n->value()) : 4;
  }
  bool setIndexRay(const Ray &input) {
    current = input;
    return current.clip(box);
  }
  bool setWorldRay(const Ray &input) { return setIndexRay(input); }
  const Ray &ray() const { return current; }
  template <class Node> bool hasNode(const openvdb::Coord &c) {
    return stencil.accessor().template probeConstNode<Node>(c) != nullptr;
  }
  void setRange(double a, double b) { current.setTimes(a, b); }
  void init(double t) {
    times[0] = t;
    values[0] = sample(t);
  }
  bool operator()(const openvdb::Coord &ijk, double t) {
    double voxel = 0;
    if (!stencil.accessor().probeValue(ijk, voxel) || voxel <= minimum ||
        voxel >= maximum)
      return false;
    times[1] = t;
    values[1] = sample(t);
    if (values[0] == 0) {
      time = times[0];
      context->lastKind = SdfRayHitKind::SampledZero;
      return true;
    }
    if (values[1] == 0) {
      time = times[1];
      context->lastKind = SdfRayHitKind::SampledZero;
      return true;
    }
    if (std::signbit(values[0]) == std::signbit(values[1])) {
      times[0] = times[1];
      values[0] = values[1];
      return false;
    }
    context->lastKind = SdfRayHitKind::BracketedCrossing;
    time = chord();
    for (uint32_t i = 0; i < refinements; ++i) {
      const double v = sample(time);
      if (v == 0)
        break;
      const int side = std::signbit(v) == std::signbit(values[0]) ? 0 : 1;
      const double previous = time;
      values[side] = v;
      times[side] = time;
      time = chord();
      if (time == previous || time == times[0] || time == times[1])
        break;
    }
    return true;
  }
  void getIndexPos(V &position) const { position = current(time); }
  void getWorldPos(V &position) const { getIndexPos(position); }
  double getIndexTime() const { return time; }
  double getWorldTime() const { return time; }

private:
  double sample(double t) {
    return interpolation(stencil, current(t), isovalue);
  }
  double chord() const {
    const double peak = std::max(std::abs(values[0]), std::abs(values[1]));
    const double a = scaled(std::abs(values[0]), 1, peak);
    const double b = scaled(std::abs(values[1]), 1, peak);
    const double ratio = a / (a + b);
    const double t = std::fma(times[1] - times[0], ratio, times[0]);
    if (!std::isfinite(t) || t < times[0] || t > times[1])
      throw std::runtime_error("native ray chord escaped its checked bracket");
    return t;
  }
  openvdb::math::BoxStencil<Scalar> stencil;
  const double isovalue, minimum, maximum;
  const ConditionedGrid *context;
  openvdb::CoordBBox box;
  Ray current;
  double time = 0, times[2]{}, values[2]{};
  uint32_t refinements = 4;
};

V worldPosition(const WorldRay &ray, const Prepared &prepared, double distance,
                const Map &map) {
  V position;
  for (int a = 0; a < 3; ++a)
    position[a] = std::fma(prepared.direction[a], distance, ray.origin[a]);
  map.spacing(position);
  return position;
}
double worldDistance(double nativeTime, const Prepared &p, const Map &map) {
  const double distance = p.start + (nativeTime - 1) / p.indexPerWorld;
  const double tolerance = map.h * 1e-4;
  if (!std::isfinite(distance) || distance < p.start - tolerance ||
      distance > p.end + tolerance || gap(distance) > tolerance)
    throw std::runtime_error(
        "native ray time escaped checked world interval/precision");
  return std::clamp(distance, p.start, p.end);
}
bool activeFootprint(const Scalar &grid, const V &p) {
  const auto c = openvdb::Coord::floor(p);
  const auto a = grid.getConstAccessor();
  for (int x = 0; x < 2; ++x)
    for (int y = 0; y < 2; ++y)
      for (int z = 0; z < 2; ++z)
        if (!a.isValueOn(c + openvdb::Coord(x, y, z)))
          return false;
  return true;
}
V globalIndex(const V &position, const Census &c, bool volumeQuery = false) {
  V result;
  for (int a = 0; a < 3; ++a)
    result[a] = position[a] + c.anchor[a] - (volumeQuery ? .5 : 0);
  return result;
}
void consistent(const V &a, const V &b, const Map &map) {
  for (int axis = 0; axis < 3; ++axis)
    if (!std::isfinite(a[axis]) || !std::isfinite(b[axis]) ||
        std::abs(a[axis] - b[axis]) > map.h * 1e-4)
      throw std::runtime_error(
          "native ray position/time reconstruction disagrees");
}
uint64_t resultReservation(size_t count, uint64_t spans = 0) {
  return add(65536, add(multiply(count, sizeof(WorldRay) + sizeof(Prepared) +
                                            sizeof(SdfRayHit) +
                                            sizeof(VolumeRayResult) + 256),
                        multiply(spans, sizeof(VolumeRaySpan))));
}
template <class G>
LevelSetRayBatch
levelSetBatch(const G &g, const Loaded &loaded, const Map &map,
              std::span<const WorldRay> rays, const LevelSetRayOptions &o,
              const ProcessingOptions &processing, Work &work) {
  const auto counts =
      census(g, loaded, map, processing, work, resultReservation(rays.size()));
  // Source validation/conditioning is not skipped for empty batches. It remains
  // bounded by the same preflight and catches hidden negative empty topology.
  auto unit = conditioned(g, map, counts, o, work);
  std::vector<Prepared> plans;
  plans.reserve(rays.size());
  for (const auto &ray : rays)
    plans.push_back(prepare(ray, map, counts, false, o.refinementSteps, work));
  LevelSetRayBatch batch;
  batch.source = sourceInfo(loaded, map, counts);
  batch.rays.reserve(rays.size());
  batch.diagnostics.emplace_back(
      "Approximate native HDDA/one-dimensional chord hits; "
      "tangencies and multiple roots between samples may be missed.");
  if (!counts.active)
    batch.diagnostics.emplace_back(
        "Empty positive-background SDF: no resolved surface.");
  // Avoid constructing an empty native intersector: it has no valid native
  // bbox.
  std::unique_ptr<openvdb::tools::LevelSetRayIntersector<Scalar, CheckedSearch>>
      intersector;
  if (counts.active && !rays.empty())
    intersector = std::make_unique<
        openvdb::tools::LevelSetRayIntersector<Scalar, CheckedSearch>>(
        *unit, scaled(o.isovalue, 1, map.h));
  openvdb::math::BoxStencil<Scalar> sample(*unit), gradient(*unit);
  for (size_t i = 0; i < rays.size(); ++i) {
    const auto &p = plans[i];
    SdfRayHit result;
    result.normalizedDirection = array(p.direction);
    result.normalStatus = o.computeNormals ? RayNormalStatus::NoHit
                                           : RayNormalStatus::NotRequested;
    const V startIndex = map.indexPosition(worldPosition(
                             rays[i], p, rays[i].minimumDistance, map)) -
                         V(double(counts.anchor[0]), double(counts.anchor[1]),
                           double(counts.anchor[2]));
    // Accessors must never receive an out-of-range requested start far outside
    // the source; such a sample is unavailable, not a fabricated background
    // sign.
    bool safeStart = true;
    for (int a = 0; a < 3; ++a)
      safeStart &= startIndex[a] > INT32_MIN + 8192. &&
                   startIndex[a] < INT32_MAX - 8192.;
    result.startSampleValid = safeStart && activeFootprint(*unit, startIndex);
    if (result.startSampleValid)
      result.startedInside =
          interpolation(sample, startIndex, scaled(o.isovalue, 1, map.h)) < 0;
    V position;
    double t = 0;
    auto &context = static_cast<const ConditionedGrid &>(*unit);
    context.lastKind = SdfRayHitKind::Miss;
    if (p.hit && intersector && intersector->intersectsIS(p.ray, position, t)) {
      result.kind = context.lastKind;
      result.distance = worldDistance(t, p, map);
      const V world = worldPosition(rays[i], p, result.distance, map);
      consistent(world, map.position(globalIndex(position, counts)), map);
      result.position = array(world);
      result.sampledResidual = scaled(
          interpolation(sample, position, scaled(o.isovalue, 1, map.h)), map.h);
      if (o.computeNormals) {
        if (!activeFootprint(*unit, position))
          result.normalStatus = RayNormalStatus::MissingActiveFootprint;
        else {
          gradient.moveTo(openvdb::Coord::floor(position));
          const V n = gradient.gradient(position);
          const double length = std::hypot(n[0], n[1], n[2]);
          if (!std::isfinite(length))
            throw std::invalid_argument("ray normal gradient overflows");
          if (length <= 1e-7)
            result.normalStatus = RayNormalStatus::DegenerateGradient;
          else {
            result.normal = array(map.normal(n));
            result.normalStatus = RayNormalStatus::Available;
          }
        }
      }
    }
    batch.rays.push_back(result);
  }
  batch.source.workItems = work.used;
  return batch;
}
struct SpanList {
  using Time = Ray::TimeSpan;
  std::vector<Time> values;
  uint64_t capacity;
  explicit SpanList(uint64_t n) : capacity(n) { values.reserve(size_t(n)); }
  void clear() { values.clear(); }
  void push_back(const Time &time) {
    if (values.size() >= capacity)
      throw std::runtime_error(
          "native volume span count exceeded prospective traversal bound");
    values.push_back(time);
  }
};
template <class G>
VolumeRayBatch volumeBatch(const G &g, const Loaded &loaded, const Map &map,
                           std::span<const WorldRay> rays,
                           const VolumeRayOptions &o,
                           const ProcessingOptions &processing, Work &work) {
  const auto counts = census(g, loaded, map, processing, work,
                             resultReservation(rays.size(), o.maxSpans));
  std::vector<Prepared> plans;
  plans.reserve(rays.size());
  uint64_t spanBound = 0;
  for (const auto &ray : rays) {
    plans.push_back(prepare(ray, map, counts, true, 0, work));
    spanBound = add(spanBound, plans.back().bound);
  }
  if (spanBound > o.maxSpans)
    throw std::invalid_argument(
        "prospective volume ray spans exceed maxSpans; no truncation");
  // One native temporary list and complete retained public spans coexist.
  bytes(counts.reserve + spanBound * sizeof(Ray::TimeSpan), processing,
        "volume ray native span list exceeds shared byte cap");
  VolumeRayBatch batch;
  batch.source = sourceInfo(loaded, map, counts);
  batch.rays.reserve(rays.size());
  batch.diagnostics.emplace_back(
      "Coarse active leaf/tile centered-cell spans, not occupied-voxel "
      "or density intervals; native intervals<=1e-9 index units omitted.");
  // Source census is mandatory even for no rays; the no-query path needs no
  // topology/intersector copy and cannot make an inactive leaf look occupied.
  Mask::Ptr mask;
  std::unique_ptr<openvdb::tools::VolumeRayIntersector<Mask>> intersector;
  if (counts.active && !rays.empty()) {
    mask = occupancy(g, counts, work);
    intersector =
        std::make_unique<openvdb::tools::VolumeRayIntersector<Mask>>(*mask, 0);
  }
  for (size_t i = 0; i < rays.size(); ++i) {
    const auto &p = plans[i];
    VolumeRayResult result;
    result.normalizedDirection = array(p.direction);
    if (p.hit && intersector && intersector->setIndexRay(p.ray)) {
      SpanList times(p.bound);
      intersector->hits(times);
      result.spans.reserve(times.values.size());
      for (const auto &t : times.values) {
        if (!std::isfinite(t.t0) || !std::isfinite(t.t1) || t.t1 <= t.t0)
          throw std::runtime_error("invalid native volume span");
        VolumeRaySpan span;
        span.entryDistance = worldDistance(t.t0, p, map);
        span.exitDistance = worldDistance(t.t1, p, map);
        if (span.exitDistance <= span.entryDistance)
          throw std::runtime_error(
              "native volume span lost positive world extent");
        const V entry = worldPosition(rays[i], p, span.entryDistance, map);
        const V exit = worldPosition(rays[i], p, span.exitDistance, map);
        consistent(entry, map.position(globalIndex(p.ray(t.t0), counts, true)),
                   map);
        consistent(exit, map.position(globalIndex(p.ray(t.t1), counts, true)),
                   map);
        span.entryPosition = array(entry);
        span.exitPosition = array(exit);
        result.spans.push_back(span);
      }
      batch.totalSpanCount = add(batch.totalSpanCount, result.spans.size());
    }
    batch.rays.push_back(std::move(result));
  }
  batch.source.workItems = work.used;
  return batch;
}
#endif
} // namespace

LevelSetRayBatch QueryLevelSetRays(const std::filesystem::path &input,
                                   std::span<const WorldRay> rays,
                                   const LevelSetRayOptions &options,
                                   const std::string &inputGrid,
                                   const ProcessingOptions &processing) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(options.limits, rays, processing);
  if (!std::isfinite(options.isovalue) || options.refinementSteps > 8)
    throw std::invalid_argument("invalid SDF ray isovalue or refinement count");
  tbb::task_arena arena(1);
  return arena.execute([&] {
    openvdb::initialize();
    Work work{0, options.limits.maxWorkItems};
    Snapshot snapshot(input, processing, work);
    auto loaded =
        load(snapshot, options.limits, inputGrid, processing, true, work);
    const Map map(*loaded.grid);
    if (loaded.grid->isType<openvdb::FloatGrid>())
      return levelSetBatch(
          static_cast<const openvdb::FloatGrid &>(*loaded.grid), loaded, map,
          rays, options, processing, work);
    return levelSetBatch(static_cast<const openvdb::DoubleGrid &>(*loaded.grid),
                         loaded, map, rays, options, processing, work);
  });
#else
  (void)input;
  (void)rays;
  (void)options;
  (void)inputGrid;
  (void)processing;
  unavailable();
#endif
}
VolumeRayBatch QueryActiveVolumeRays(const std::filesystem::path &input,
                                     std::span<const WorldRay> rays,
                                     const VolumeRayOptions &options,
                                     const std::string &inputGrid,
                                     const ProcessingOptions &processing) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(options.limits, rays, processing);
  if (!options.maxSpans || options.maxSpans > 1'000'000)
    throw std::invalid_argument("volume maxSpans must be in[1,1M]");
  tbb::task_arena arena(1);
  return arena.execute([&] {
    openvdb::initialize();
    Work work{0, options.limits.maxWorkItems};
    Snapshot snapshot(input, processing, work);
    auto loaded =
        load(snapshot, options.limits, inputGrid, processing, false, work);
    const Map map(*loaded.grid);
    return visit(*loaded.grid, [&](const auto &grid) {
      return volumeBatch(grid, loaded, map, rays, options, processing, work);
    });
  });
#else
  (void)input;
  (void)rays;
  (void)options;
  (void)inputGrid;
  (void)processing;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
