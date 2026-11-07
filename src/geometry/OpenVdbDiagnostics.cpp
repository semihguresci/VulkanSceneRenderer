#include "Container/geometry/OpenVdbDiagnostics.h"
#include "OpenVdbProvenance.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbPointAttributesInternal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/math/Stencils.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/Diagnostics.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#include <type_traits>
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB diagnostics are unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using PD = openvdb::points::PointDataGrid;
namespace points = openvdb::points;
using C = openvdb::Coord;
using V = openvdb::Vec3d;
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
      throw std::invalid_argument("null diagnostics metadata");
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
      throw std::invalid_argument("diagnostics operation exceeds maxWorkItems");
    used += uint64_t(std::ceil(n));
  }
};
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p,
           uint64_t maxWork) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument("diagnostics input must be a regular file");
    encoded = std::filesystem::file_size(input);
    if (8.0L * encoded > maxWork)
      throw std::invalid_argument(
          "diagnostics snapshot/archive work exceeds maxWork before copying");
    bytes(Fixed + encoded, p,
          "encoded diagnostics source exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned n = 0; n < 64; ++n) {
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-diagnostics-" +
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
      throw std::runtime_error("cannot reserve diagnostics snapshot");
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open diagnostics snapshot");
      std::array<char, 65536> buffer{};
      auto left = encoded;
      while (left) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(left, buffer.size()));
        source.read(buffer.data(), count);
        if (source.gcount() != count)
          throw std::runtime_error("diagnostics source changed length");
        target.write(buffer.data(), count);
        left -= count;
      }
      target.close();
      if (!target || source.bad() ||
          source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "diagnostics snapshot failed or source changed length");
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

class MetadataArchive : public openvdb::io::Archive {
public:
  void begin(std::istream &stream, openvdb::io::StreamMetadata::Ptr &metadata) {
    readHeader(stream);
    if (!inputHasGridOffsets() ||
        fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
        fileVersion() > openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument("diagnostics require supported seekable "
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
      throw std::invalid_argument("diagnostics grid descriptor offsets are "
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
          "diagnostics metadata/transform crosses grid payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};

template <class F>
auto visit(const openvdb::GridBase &g, F f)
    -> decltype(f(static_cast<const openvdb::FloatGrid &>(g))) {
#define GRID(T)                                                                \
  if (g.isType<openvdb::T>())                                                  \
  return f(static_cast<const openvdb::T &>(g))
  GRID(FloatGrid);
  GRID(DoubleGrid);
  GRID(BoolGrid);
  GRID(MaskGrid);
  GRID(Int32Grid);
  GRID(Int64Grid);
  GRID(Vec3IGrid);
  GRID(Vec3SGrid);
  GRID(Vec3DGrid);
#undef GRID
  if (g.isType<PD>())
    return f(static_cast<const PD &>(g));
  throw std::invalid_argument(
      "diagnostics supports nine native field types and PointDataGrid");
}
void name(const std::string &n) {
  if (n.empty() || n.size() > 256 || n.find('\0') != std::string::npos)
    throw std::invalid_argument(
        "diagnostics names must contain1..256 bytes without NUL");
}
void validate(const DiagnosticsOptions &o, const ProcessingOptions &p,
              const PointAttributeOptions &points, bool writing) {
  point_attribute_detail::validateOptions(p, points);
  if (unsigned(o.profile) > unsigned(DiagnosticProfile::PointData) ||
      unsigned(o.rangeScope) > unsigned(DiagnosticScope::Stored) ||
      unsigned(o.gradient) > unsigned(DiagnosticGradient::WenoInterface) ||
      unsigned(o.footprint) >
          unsigned(DiagnosticFootprint::NativeStoredContinuation) ||
      !o.maxWork || o.maxWork > 10'000'000'000ULL || !o.maxGrids ||
      o.maxGrids > 4096 || !o.maxMetadataBytes || o.maxExamples > 128 ||
      !std::isfinite(o.magnitudeTolerance) || o.magnitudeTolerance < 0)
    throw std::invalid_argument(
        "invalid diagnostics profile/count/work/tolerance options");
  if (writing)
    name(o.maskGridName);
  if (o.componentRange && (!std::isfinite((*o.componentRange)[0]) ||
                           !std::isfinite((*o.componentRange)[1]) ||
                           (*o.componentRange)[0] > (*o.componentRange)[1]))
    throw std::invalid_argument(
        "diagnostics range requires finite ordered bounds");
  if (!o.componentRange && o.rangeScope != DiagnosticScope::Active)
    throw std::invalid_argument(
        "diagnostics stored range scope requires componentRange");
  const bool sdf = o.profile == DiagnosticProfile::SymmetricLevelSet,
             fog = o.profile == DiagnosticProfile::NormalizedFog,
             pd = o.profile == DiagnosticProfile::PointData;
  if (!sdf && o.gradient != DiagnosticGradient::None)
    throw std::invalid_argument(
        "diagnostics gradient checks require the symmetric level-set profile");
  if (!sdf && !fog && o.magnitudeTolerance != 0)
    throw std::invalid_argument(
        "diagnostics magnitude tolerance requires SDF/fog profile");
  if (o.gradient == DiagnosticGradient::None) {
    if (o.gradientNormRange != std::array<double, 2>{.5, 1.5} ||
        o.footprint != DiagnosticFootprint::RequireActive)
      throw std::invalid_argument(
          "diagnostics norm/footprint controls require gradient checks");
  } else {
    if (!std::isfinite(o.gradientNormRange[0]) ||
        !std::isfinite(o.gradientNormRange[1]) || o.gradientNormRange[0] < 0 ||
        o.gradientNormRange[0] > o.gradientNormRange[1])
      throw std::invalid_argument(
          "diagnostics norm interval must be finite nonnegative and ordered");
    for (double x : o.gradientNormRange)
      if (!std::isfinite(x * x) || (x != 0 && x * x == 0))
        throw std::invalid_argument(
            "diagnostics squared norm thresholds lose Double range");
  }
  if (!pd && (!o.pointAttributes.empty() || !o.pointGroups.anyOf.empty() ||
              !o.pointGroups.allOf.empty() || !o.pointGroups.noneOf.empty()))
    throw std::invalid_argument(
        "diagnostics point controls require PointData profile");
  if (pd && o.componentRange && o.pointAttributes.empty())
    throw std::invalid_argument(
        "diagnostics point range requires selected numeric attributes");
  if (pd && o.rangeScope != DiagnosticScope::Active)
    throw std::invalid_argument("diagnostics field range scope is not a "
                                "point-record selection control");
  if (o.pointAttributes.size() > points.maxAttributes)
    throw std::invalid_argument("too many diagnostic point attributes");
  std::set<std::string> unique;
  for (const auto &n : o.pointAttributes) {
    name(n);
    if (n == "P" || !unique.insert(n).second)
      throw std::invalid_argument("point diagnostics P is checked separately; "
                                  "attribute names must be unique");
  }
  size_t terms = 0;
  for (const auto *list :
       {&o.pointGroups.anyOf, &o.pointGroups.allOf, &o.pointGroups.noneOf})
    for (const auto &n : *list) {
      name(n);
      if (++terms > 3 * points.maxGroups)
        throw std::invalid_argument("diagnostics group predicate exceeds cap");
    }
}
void add(uint64_t &n, uint64_t amount = 1) {
  if (amount > UINT64_MAX - n)
    throw std::invalid_argument("diagnostics counter exceeds UInt64");
  n += amount;
}
uint64_t volume(const openvdb::CoordBBox &b) {
  uint64_t n = 1;
  for (int a = 0; a < 3; ++a) {
    const auto length = uint64_t(int64_t(b.max()[a]) - b.min()[a] + 1);
    if (length > UINT64_MAX / n)
      throw std::invalid_argument("diagnostics tile volume exceeds UInt64");
    n *= length;
  }
  return n;
}
struct Loaded {
  openvdb::GridBase::Ptr grid;
  openvdb::MetaMap::Ptr fileMetadata;
  std::string descriptor;
  long double resident = 0, meta = 0, full = 0, work = 0;
  uint64_t pointUpper = 0;
};
Loaded load(const Snapshot &s, const DiagnosticsOptions &o,
            const std::string &selector, const ProcessingOptions &p,
            const PointAttributeOptions &limits) {
  detail::RequireSeekableFile(s.file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded diagnostics requires delayed loading; "
                                "unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded diagnostics requires OpenVDB delayed-loading support");
#else
  auto sm = std::make_shared<openvdb::io::StreamMetadata>();
  sm->setSeekable(true);
  std::ifstream stream(s.file, std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  MetadataArchive archive;
  archive.begin(stream, sm);
  Loaded r;
  r.fileMetadata = std::make_shared<openvdb::MetaMap>();
  r.fileMetadata->readMeta(stream);
  r.meta = metadata(*r.fileMetadata);
  long double catalog = s.encoded + Fixed;
  const auto check = [&] {
    if (r.meta > o.maxMetadataBytes)
      throw std::invalid_argument(
          "diagnostics file/scanned metadata exceeds maxMetadataBytes");
    bytes(catalog + 12 * r.meta, p, "diagnostics catalog exceeds byte cap");
  };
  check();
  const int32_t n = archive.count(stream);
  if (n < 0 || uint32_t(n) > o.maxGrids)
    throw std::invalid_argument(
        "diagnostics scanned grid count exceeds maxGrids");
  std::set<std::string> descriptors;
  openvdb::GridBase::Ptr chosen;
  for (int32_t i = 0; i < n; ++i) {
    openvdb::io::GridDescriptor d;
    auto g = archive.next(stream, d, s.encoded);
    const auto descriptor =
        openvdb::io::GridDescriptor::nameAsString(d.uniqueName());
    catalog += 4.0L * descriptor.size() + 256;
    if (!descriptors.insert(descriptor).second)
      throw std::invalid_argument(
          "diagnostics colliding native descriptors; rename grids first");
    r.meta += metadata(*g);
    check();
    if (!GridSelectionMatches(selector, descriptor, g->getName()))
      continue;
    if (chosen)
      throw std::invalid_argument(
          "diagnostics grid is ambiguous; use descriptor:<exact name>");
    (void)visit(*g, [](const auto &) { return 0; });
    const auto m = g->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!m || m->value() < 0)
      throw std::invalid_argument(
          "diagnostics requires valid recorded source memory");
    bytes(catalog + 12 * r.meta + m->value(), p,
          "diagnostics recorded source exceeds byte cap");
    if (static_cast<long double>(m->value()) > o.maxWork)
      throw std::invalid_argument(
          "diagnostics recorded source exceeds maxWork before loading");
    chosen = g;
    r.descriptor = descriptor;
  }
  if (!chosen)
    throw std::invalid_argument(
        "diagnostics found no matching registered grid");
  stream.close();
  openvdb::io::File file(s.file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error("diagnostics snapshot archive changed");
  r.grid = file.readGrid(r.descriptor);
  r.grid->clearMetadata();
  r.grid->insertMeta(*chosen);
  r.grid->setTransform(detail::CopyTransformExact(chosen->transform()));
  if (r.grid->isType<PD>()) {
    const auto pointGrid = openvdb::gridPtrCast<PD>(r.grid);
    for (auto leaf = pointGrid->tree().cbeginLeaf(); leaf; ++leaf)
      for (size_t index = 0; index < leaf->attributeSet().size(); ++index) {
        const auto &array = leaf->constAttributeArray(index);
        if (array.hasConstantStride() &&
            ((!array.stride() && array.size()) ||
             (array.size() &&
              uint64_t(array.stride()) >
                  uint64_t(std::numeric_limits<openvdb::Index>::max()) /
                      array.size())))
          throw std::invalid_argument("point diagnostic array flat length "
                                      "exceeds native Index before paging");
      }
  }
  r.full = visit(*r.grid, [](const auto &g) {
    return static_cast<long double>(
        openvdb::tools::memUsageIfLoaded(g.tree(), false));
  });
  r.work = visit(*r.grid, [&](const auto &g) {
    const auto nodes = g.tree().nodeCount();
    using T = typename std::decay_t<decltype(g)>::TreeType;
    using U = typename T::RootNodeType::ChildNodeType;
    using D = typename U::ChildNodeType;
    if (g.tree().leafCount() > p.maxVoxelCount / 512)
      throw std::invalid_argument(
          "diagnostics source stored leaf slots exceed maxVoxelCount");
    return g.tree().root().getTableSize() + nodes[0] * 512.0L +
           nodes[1] * D::NUM_VALUES * 1.0L + nodes[2] * U::NUM_VALUES * 1.0L;
  });
  if (r.grid->isType<PD>()) {
    const auto g = openvdb::gridPtrCast<PD>(r.grid);
    for (auto leaf = g->tree().cbeginLeaf(); leaf; ++leaf) {
      const auto &descriptor = leaf->attributeSet().descriptor();
      if (leaf->attributeSet().size() > limits.maxAttributes ||
          descriptor.groupMap().size() > limits.maxGroups)
        throw std::invalid_argument(
            "diagnostics point schema exceeds attribute/group caps");
      uint64_t maximum = 0;
      for (size_t a = 0; a < leaf->attributeSet().size(); ++a)
        maximum =
            std::max(maximum, uint64_t(leaf->constAttributeArray(a).size()));
      add(r.pointUpper, maximum);
      if (r.pointUpper > limits.maxPoints)
        throw std::invalid_argument(
            "diagnostics point buffers exceed maxPoints");
      r.meta += metadata(descriptor.getMetadata());
      uintmax_t stringBytes = 0;
      for (auto it = descriptor.getMetadata().beginMeta();
           it != descriptor.getMetadata().endMeta(); ++it) {
        if (!it->first.starts_with("string:"))
          continue;
        const auto text =
            std::dynamic_pointer_cast<openvdb::StringMetadata>(it->second);
        if (text) {
          if (text->value().size() > limits.maxStringBytes - stringBytes)
            throw std::invalid_argument("point diagnostic string dictionary "
                                        "exceeds maxStringBytes before paging");
          stringBytes += text->value().size();
        }
      }
      check();
    }
  }
  const long double checks = 32 + 8 * o.pointAttributes.size();
  r.work = 8 * s.encoded + 8 * r.meta + 8 * r.full + 8 * checks * r.work +
           r.pointUpper *
               (4096.0L + 256 * o.pointAttributes.size() +
                4 * (o.pointGroups.anyOf.size() + o.pointGroups.allOf.size() +
                     o.pointGroups.noneOf.size()));
  if (o.gradient != DiagnosticGradient::None)
    r.work += r.grid->baseTree().leafCount() * 512.0L * 8192;
  if (r.work > o.maxWork)
    throw std::invalid_argument(
        "diagnostics decoded/check/stencil work exceeds maxWork before paging");
  r.resident = catalog + 32 * r.meta + 16 * r.full + r.pointUpper * 256.0L +
               Fixed + checks * (2048 + o.maxExamples * 1536.0L);
  bytes(r.resident, p,
        "diagnostics full source/mask/report/conditioned/writer reserve "
        "exceeds byte cap before paging");
  r.grid->readNonresidentBuffers();
  file.close();
  return r;
#endif
}

// Comparing a native integer to a floating bound must not round the integer.
int compareInteger(int64_t value, double bound) {
  if (bound >= 0x1p63)
    return -1;
  if (bound < -0x1p63)
    return 1;
  const auto integral = static_cast<int64_t>(bound);
  if (value < integral)
    return -1;
  if (value > integral)
    return 1;
  const double exact = double(integral);
  return exact < bound ? -1 : exact > bound ? 1 : 0;
}
template <class T> bool finiteValue(const T &value) {
  if constexpr (requires { value[0]; })
    return finiteValue(value[0]) && finiteValue(value[1]) &&
           finiteValue(value[2]);
  else if constexpr (std::is_floating_point_v<T>)
    return std::isfinite(value);
  else
    return true;
}
template <class T> bool exactRange(const T &value, double low, double high) {
  if constexpr (requires { value[0]; })
    return exactRange(value[0], low, high) && exactRange(value[1], low, high) &&
           exactRange(value[2], low, high);
  else if constexpr (std::is_integral_v<T>)
    return compareInteger(int64_t(value), low) >= 0 &&
           compareInteger(int64_t(value), high) <= 0;
  else
    return value >= low && value <= high;
}
struct Context {
  const DiagnosticsOptions &o;
  const ProcessingOptions &p;
  Loaded &loaded;
  bool writing;
  DiagnosticReport report;
  Mask::Ptr failures = Mask::create(false);
  uint64_t maskVolume = 0;
  Context(const DiagnosticsOptions &options,
          const ProcessingOptions &processing, Loaded &source, bool write)
      : o(options), p(processing), loaded(source), writing(write) {
    report.checks.reserve(32 + o.pointAttributes.size());
    report.diagnostics.reserve(8);
  }
  size_t check(std::string id, std::string description) {
    DiagnosticCheck c;
    c.id = std::move(id);
    c.description = std::move(description);
    c.examples.reserve(o.maxExamples);
    report.checks.push_back(std::move(c));
    return report.checks.size() - 1;
  }
  void example(size_t i, const std::string &detail, std::optional<C> coord = {},
               std::optional<uint64_t> ordinal = {}) {
    auto &c = report.checks[i];
    if (c.examples.size() >= o.maxExamples)
      return;
    DiagnosticExample e;
    e.detail = detail;
    e.pointOrdinal = ordinal;
    if (coord)
      e.index = std::array<int32_t, 3>{coord->x(), coord->y(), coord->z()};
    c.examples.push_back(std::move(e));
  }
  void global(size_t i, const std::string &detail) {
    report.checks[i].status = DiagnosticStatus::Fail;
    report.unlocatedFailures = true;
    example(i, detail);
  }
  void unavailable(size_t i, const std::string &detail) {
    auto &c = report.checks[i];
    c.coverageComplete = false;
    if (c.status != DiagnosticStatus::Fail)
      c.status = DiagnosticStatus::Unavailable;
    example(i, detail);
  }
  void location(const openvdb::CoordBBox &box) {
    // Source iterator partitions are disjoint; point and check overlaps are
    // handled by counting the mask only after all checks. Tile fill is compact.
    const auto size = volume(box);
    // All locations are native source value partitions, or original point /
    // active-gradient voxels within source leaves. A tile cannot partially
    // overlap a different partition, so its first coordinate identifies an
    // already recorded partition without expanding or iterating the tile.
    if (failures->tree().isValueOn(box.min()))
      return;
    if (size > UINT64_MAX - maskVolume)
      throw std::invalid_argument(
          "diagnostics union-mask count exceeds UInt64");
    if (writing && size > p.maxVoxelCount - maskVolume)
      throw std::invalid_argument(
          "diagnostics failure-mask semantic coverage exceeds maxVoxelCount");
    if (size == 1 && !failures->tree().probeConstLeaf(box.min()) &&
        failures->tree().leafCount() >= p.maxVoxelCount / 512)
      throw std::invalid_argument("diagnostics mask stored leaf slots exceed "
                                  "maxVoxelCount before allocation");
    failures->tree().fill(box, true, true);
    maskVolume += size;
    if (failures->tree().leafCount() > p.maxVoxelCount / 512)
      throw std::invalid_argument(
          "diagnostics mask stored leaf slots exceed maxVoxelCount");
  }
  template <class Iter>
  void failed(size_t i, const Iter &it, const std::string &detail) {
    auto &c = report.checks[i];
    c.status = DiagnosticStatus::Fail;
    if (it.isVoxelValue())
      add(c.failedLeafVoxels);
    else {
      add(c.failedTiles);
      add(c.failedTileVoxelVolume, volume(it.getBoundingBox()));
    }
    location(it.getBoundingBox());
    example(i, detail, it.getCoord());
  }
  template <class Iter> void examined(size_t i, const Iter &it) {
    auto &c = report.checks[i];
    if (it.isVoxelValue())
      add(c.examinedLeafVoxels);
    else
      add(c.examinedTiles);
  }
  void pointFailure(size_t i, C coord, uint64_t ordinal,
                    const std::string &detail) {
    auto &c = report.checks[i];
    c.status = DiagnosticStatus::Fail;
    add(c.failedPoints);
    location(openvdb::CoordBBox(coord, coord));
    example(i, detail, coord, ordinal);
  }
  template <class T>
  void background(size_t i, const T &, bool failed, const std::string &detail) {
    auto &c = report.checks[i];
    c.backgroundInspected = true;
    if (failed) {
      c.backgroundFailed = true;
      global(i, detail);
    }
  }
  void finish() {
    uint64_t count = 0;
    for (auto it = failures->cbeginValueOn(); it; ++it)
      add(count, it.isVoxelValue() ? 1 : volume(it.getBoundingBox()));
    report.maskVoxelCount = count;
    if (writing && count > p.maxVoxelCount)
      throw std::invalid_argument(
          "diagnostics union-mask semantic coverage exceeds maxVoxelCount");
    report.complete = true;
    report.allRequestedPassed = true;
    for (const auto &c : report.checks) {
      if (!c.coverageComplete || c.status == DiagnosticStatus::Unavailable)
        report.complete = false;
      if (c.status == DiagnosticStatus::Fail ||
          c.status == DiagnosticStatus::Unavailable || !c.coverageComplete)
        report.allRequestedPassed = false;
    }
  }
};
struct MapInfo {
  bool affine = false, finite = false, uniform = false;
  double h = 0;
  std::array<double, 16> matrix{};
  explicit MapInfo(const openvdb::GridBase &g) {
    affine = g.transform().isLinear();
    if (!affine)
      return;
    const auto map = g.transform().baseMap();
    const V origin = map->applyMap(V(0));
    const std::array<V, 3> columns{map->applyJacobian(V(1, 0, 0)),
                                   map->applyJacobian(V(0, 1, 0)),
                                   map->applyJacobian(V(0, 0, 1))};
    finite = finiteValue(origin);
    std::array<double, 3> length{};
    for (int a = 0; a < 3; ++a) {
      finite = finite && finiteValue(columns[a]);
      length[a] = std::hypot(columns[a][0], columns[a][1], columns[a][2]);
      finite = finite && std::isfinite(length[a]) && length[a] > 0;
    }
    matrix = {columns[0][0],
              columns[1][0],
              columns[2][0],
              origin[0],
              columns[0][1],
              columns[1][1],
              columns[2][1],
              origin[1],
              columns[0][2],
              columns[1][2],
              columns[2][2],
              origin[2],
              0,
              0,
              0,
              1};
    if (!finite)
      return;
    h = length[0];
    uniform = true;
    std::array<V, 3> unit;
    for (int a = 0; a < 3; ++a) {
      unit[a] = columns[a] / length[a];
      if (std::abs(length[a] / h - 1) > 1e-10)
        uniform = false;
    }
    for (int a = 0; a < 3; ++a)
      for (int b = a + 1; b < 3; ++b)
        if (std::abs(unit[a].dot(unit[b])) > 1e-10)
          uniform = false;
  }
};
void reportStructure(Context &c) {
  const auto &g = *c.loaded.grid;
  auto &r = c.report;
  r.descriptorName = c.loaded.descriptor;
  r.gridName = g.getName();
  r.valueType = g.valueType();
  r.gridClass = openvdb::GridBase::gridClassToString(g.getGridClass());
  r.vectorType = openvdb::GridBase::vecTypeToString(g.getVectorType());
  r.inWorldSpace = g.isInWorldSpace();
  const MapInfo map(g);
  r.affine = map.affine;
  if (map.affine && map.finite)
    r.indexToWorld = map.matrix;
  r.decodedBytes = uint64_t(c.loaded.full);
  r.workUpperBound = uint64_t(std::ceil(c.loaded.work));
  visit(g, [&](const auto &grid) {
    r.leafCount = grid.tree().leafCount();
    r.storedLeafSlots = r.leafCount * 512;
    for (auto it = grid.cbeginValueAll(); it; ++it) {
      if (!it.isVoxelValue())
        add(it.isValueOn() ? r.activeTileCount : r.inactiveTileCount);
      if (it.isValueOn())
        add(r.activeVoxelCount,
            it.isVoxelValue() ? 1 : volume(it.getBoundingBox()));
    }
    return 0;
  });
  const size_t structure =
      c.check("structure", "Typed native structural metadata");
  for (const auto &key :
       {openvdb::GridBase::META_GRID_CLASS, openvdb::GridBase::META_VECTOR_TYPE,
        openvdb::GridBase::META_GRID_NAME})
    if (g[key] && !g.getMetadata<openvdb::StringMetadata>(key))
      c.global(structure, "wrong-typed native string metadata");
  for (const auto &key : {openvdb::GridBase::META_IS_LOCAL_SPACE,
                          openvdb::GridBase::META_SAVE_HALF_FLOAT})
    if (g[key] && !g.getMetadata<openvdb::BoolMetadata>(key))
      c.global(structure, "wrong-typed native Bool metadata");
}
template <class G> void storedChecks(Context &c, const G &g) {
  const auto finite = c.check(
      "finite",
      "Native finite predicate over stored voxels/tiles and background");
  const std::optional<size_t> range =
      c.o.componentRange
          ? std::optional<size_t>(
                c.check("range", "Inclusive componentwise native-value range"))
          : std::nullopt;
  // Use the installed native predicate for floating fields; the remaining
  // registered integral families are finite by construction.
  const auto goodFinite = [&](const auto &value) {
    if constexpr (std::is_same_v<G, openvdb::MaskGrid>)
      return true;
    else {
      openvdb::tools::CheckFinite<G> native;
      return !native(value);
    }
  };
  c.background(finite, g.background(), !goodFinite(g.background()),
               "nonfinite background has no finite mask location");
  if (range && c.o.rangeScope == DiagnosticScope::Stored)
    c.background(*range, g.background(),
                 !exactRange(g.background(), (*c.o.componentRange)[0],
                             (*c.o.componentRange)[1]),
                 "background outside requested range");
  for (auto it = g.cbeginValueAll(); it; ++it) {
    c.examined(finite, it);
    if (!goodFinite(*it))
      c.failed(finite, it, "nonfinite stored value");
    if (range &&
        (it.isValueOn() || c.o.rangeScope == DiagnosticScope::Stored)) {
      c.examined(*range, it);
      if (!exactRange(*it, (*c.o.componentRange)[0], (*c.o.componentRange)[1]))
        c.failed(*range, it, "stored component outside inclusive range");
    }
  }
}
template <class G> void profileChecks(Context &c, const G &g) {
  if (c.o.profile != DiagnosticProfile::SymmetricLevelSet &&
      c.o.profile != DiagnosticProfile::NormalizedFog)
    return;
  constexpr bool scalar = std::is_same_v<G, openvdb::FloatGrid> ||
                          std::is_same_v<G, openvdb::DoubleGrid>;
  const bool sdf = c.o.profile == DiagnosticProfile::SymmetricLevelSet;
  const std::string prefix = sdf ? "sdf-" : "fog-";
  const auto type = c.check(prefix + "type", "Supported native scalar type");
  const auto klass = c.check(
      prefix + "class", "Native field class and world-invariant semantics");
  const auto background =
      c.check(prefix + "background", "Native background contract");
  const auto active =
      c.check(prefix + "active-range", "Active component range");
  const auto inactive = c.check(prefix + "inactive-magnitude",
                                "Inactive signed magnitude / zero contract");
  if (g.getGridClass() !=
          (sdf ? openvdb::GRID_LEVEL_SET : openvdb::GRID_FOG_VOLUME) ||
      !g.isInWorldSpace() || g.getVectorType() != openvdb::VEC_INVARIANT)
    c.global(klass,
             "class/world-invariant tags do not match requested profile");
  if constexpr (!scalar) {
    c.global(type, "profile requires FloatGrid or DoubleGrid");
    for (auto i : {background, active, inactive})
      c.unavailable(i, "scalar type prerequisite unavailable");
  } else {
    const double bg = g.background();
    const MapInfo map(g);
    if (sdf) {
      const auto mapping =
          c.check("sdf-map", "Finite uniform orthogonal affine map");
      if (!map.finite || !map.uniform)
        c.global(mapping,
                 "level-set map is not finite uniform orthogonal affine");
      const auto tiles =
          c.check("sdf-no-active-tiles",
                  "Native level-set active topology contains no tiles");
      for (auto it = g.cbeginValueOn(); it; ++it)
        if (!it.isVoxelValue()) {
          c.examined(tiles, it);
          c.failed(tiles, it, "active level-set tile");
        }
      const bool validBackground = std::isfinite(bg) && bg > 0;
      c.background(
          background, bg, !validBackground,
          "symmetric level-set background must be finite and positive");
      if (validBackground) {
        if (!map.finite || !map.uniform)
          c.unavailable(background, "background physical band width requires a "
                                    "finite uniform orthogonal affine map");
        else if (bg / map.h < 3)
          c.background(background, bg, true,
                       "background is narrower than three physical voxels");
      } else {
        c.unavailable(
            active,
            "active signed band bounds require a finite positive background");
        c.unavailable(
            inactive,
            "inactive signed magnitude requires a finite positive background");
        return;
      }
    } else
      c.background(background, bg,
                   !std::isfinite(bg) || std::abs(bg) > c.o.magnitudeTolerance,
                   "normalized fog background is nonzero/nonfinite");
    for (auto it = g.cbeginValueAll(); it; ++it) {
      if (it.isValueOn()) {
        c.examined(active, it);
        const double v = *it;
        const bool good =
            sdf ? (std::isfinite(bg) && bg >= 0 && v >= -bg && v <= bg)
                : (v >= 0 && v <= 1);
        if (!good)
          c.failed(active, it, "active value outside profile range");
      } else {
        c.examined(inactive, it);
        const double v = *it;
        const bool good =
            sdf ? (std::isfinite(v) && std::isfinite(bg) &&
                   std::abs(std::abs(v) - std::abs(bg)) <=
                       c.o.magnitudeTolerance)
                : (std::isfinite(v) && std::abs(v) <= c.o.magnitudeTolerance);
        if (!good)
          c.failed(inactive, it,
                   "inactive value violates signed-band magnitude / fog zero");
      }
    }
  }
}

template <class G> void gradientChecks(Context &c, const G &source) {
  if (c.o.gradient == DiagnosticGradient::None) {
    const auto index =
        c.check("gradient", "Native gradient check was not requested");
    c.report.checks[index].status = DiagnosticStatus::NotApplicable;
    return;
  }
  const auto index = c.check(
      "gradient", "Native conditioned world-unit Godunov gradient check");
  constexpr bool scalar = std::is_same_v<G, openvdb::FloatGrid> ||
                          std::is_same_v<G, openvdb::DoubleGrid>;
  if constexpr (!scalar) {
    c.unavailable(index, "gradient requires a scalar Float/Double grid");
    return;
  } else {
    const MapInfo map(source);
    if (!map.finite || !map.uniform || map.h < 1e-6 || map.h > 1e6) {
      c.unavailable(
          index,
          "gradient requires uniform orthogonal affine axes in [1e-6,1e6]");
      return;
    }
    if (c.report.activeTileCount) {
      c.unavailable(index, "gradient checks require voxelized active centers; "
                           "active tiles are not expanded");
      return;
    }
    // Keep the source index lattice. Native derivative operators act on a
    // distance/voxel-size conditioned Double field with an identity map; the
    // orthogonal world rotation/reflection does not change its gradient norm.
    const auto condition = [&](double v, double &out) {
      out = v / map.h;
      return !std::isfinite(v) || (std::isfinite(out) && (v == 0 || out != 0));
    };
    double bg = 0;
    if (!std::isfinite(source.background()) ||
        !condition(source.background(), bg)) {
      c.unavailable(
          index,
          "gradient background cannot be conditioned to finite Double units");
      return;
    }
    // Validate every stored value before allocating the conditioned tree.
    for (auto it = source.cbeginValueAll(); it; ++it) {
      double value = 0;
      if (!condition(double(*it), value)) {
        c.unavailable(index, "stored finite value loses Double range during "
                             "distance/voxel-size conditioning");
        return;
      }
    }
    auto unit = openvdb::DoubleGrid::create(bg);
    auto target = unit->getAccessor();
    for (auto it = source.cbeginValueAll(); it; ++it) {
      double value = 0;
      (void)condition(double(*it), value);
      if (it.isVoxelValue()) {
        target.setValueOnly(it.getCoord(), value);
        target.setActiveState(it.getCoord(), it.isValueOn());
      } else
        unit->tree().fill(it.getBoundingBox(), value, it.isValueOn());
    }
    const double lo = c.o.gradientNormRange[0], hi = c.o.gradientNormRange[1];
    openvdb::tools::CheckNormGrad<openvdb::DoubleGrid> first(*unit, lo, hi);
    openvdb::tools::CheckEikonal<openvdb::DoubleGrid> weno(*unit, lo * lo,
                                                           hi * hi);
    const auto acc = unit->getConstAccessor();
    const auto sourceAcc = source.getConstAccessor();
    auto &check = c.report.checks[index];
    for (auto leaf = unit->tree().cbeginLeaf(); leaf; ++leaf)
      for (auto it = leaf->cbeginValueOn(); it; ++it) {
        add(check.candidateCount);
        const C center = it.getCoord();
        const int radius =
            c.o.gradient == DiagnosticGradient::FirstOrder ? 1 : 3;
        bool coordinate = true;
        for (int a = 0; a < 3; ++a)
          if (int64_t(center[a]) - radius < INT32_MIN ||
              int64_t(center[a]) + radius > INT32_MAX)
            coordinate = false;
        if (!coordinate) {
          add(check.missingFootprintCount);
          c.unavailable(index,
                        "gradient footprint exceeds native coordinate range");
          continue;
        }
        bool finite = std::isfinite(*it), crossing = false;
        bool classificationMissing = !sourceAcc.isValueOn(center);
        const bool positive = *it > 0;
        for (int a = 0; a < 3; ++a)
          for (int sign : {-1, 1}) {
            C q = center;
            q[a] += sign;
            const double value = acc.getValue(q);
            classificationMissing =
                classificationMissing || !sourceAcc.isValueOn(q);
            finite = finite && std::isfinite(value);
            if ((positive && value < 0) || (!positive && value > 0))
              crossing = true;
          }
        if (!finite) {
          add(check.nonfiniteFootprintCount);
          c.unavailable(
              index,
              "nonfinite center/face prevents native gradient evaluation");
          continue;
        }
        if (classificationMissing &&
            c.o.footprint == DiagnosticFootprint::RequireActive) {
          add(check.missingFootprintCount);
          c.unavailable(index, "native gradient/crossing classification "
                               "includes inactive face samples");
          continue;
        }
        if (c.o.gradient == DiagnosticGradient::WenoInterface && !crossing)
          continue;
        bool missing = false, inactive = !sourceAcc.isValueOn(center);
        for (int a = 0; a < 3; ++a)
          for (int d = -radius; d <= radius; ++d) {
            if (d == 0)
              continue;
            C q = center;
            q[a] += d;
            const bool on = sourceAcc.isValueOn(q);
            inactive = inactive || !on;
            missing =
                missing ||
                (!on && c.o.footprint == DiagnosticFootprint::RequireActive);
            finite = finite && std::isfinite(acc.getValue(q));
          }
        if (!finite) {
          add(check.nonfiniteFootprintCount);
          c.unavailable(index, "nonfinite native gradient footprint");
          continue;
        }
        if (missing) {
          add(check.missingFootprintCount);
          c.unavailable(index, "native gradient footprint includes "
                               "inactive/unallocated samples");
          continue;
        }
        double norm2;
        if (c.o.gradient == DiagnosticGradient::FirstOrder)
          norm2 = openvdb::math::ISGradientNormSqrd<
              openvdb::math::FIRST_BIAS>::result(acc, center);
        else {
          weno.stencil.moveTo(it);
          norm2 = weno.stencil.normSqGrad();
        }
        if (!std::isfinite(norm2) || norm2 < 0) {
          add(check.nonfiniteFootprintCount);
          c.unavailable(index,
                        "finite inputs overflowed native gradient arithmetic");
          continue;
        }
        add(check.evaluatedCount);
        add(check.examinedLeafVoxels);
        if (inactive)
          add(check.inactiveContinuationCount);
        const bool failed = c.o.gradient == DiagnosticGradient::FirstOrder
                                ? first(it)
                                : weno(it);
        if (failed) {
          check.status = DiagnosticStatus::Fail;
          add(check.failedLeafVoxels);
          c.location(openvdb::CoordBBox(center, center));
          c.example(index, "native gradient norm outside requested interval",
                    center);
        }
      }
  }
}

template <class G> class ExactWriteGrid : public G {
public:
  explicit ExactWriteGrid(const G &g)
      : G(openvdb::ConstPtrCast<typename G::TreeType>(g.constTreePtr())) {
    this->insertMeta(g);
    this->setTransform(detail::CopyTransformExact(g.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, detail::CopyTransformExact(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactWriteGrid &>(*this).copyGrid());
  }
};
void maskStructure(Context &c) {
  auto &g = *c.failures;
  g.insertMeta(*c.loaded.grid);
  detail::RemoveBoundaryProjectionMetadata(g);
  g.setTransform(detail::CopyTransformExact(c.loaded.grid->transform()));
  for (const auto &key :
       {openvdb::GridBase::META_GRID_CLASS, openvdb::GridBase::META_VECTOR_TYPE,
        openvdb::GridBase::META_IS_LOCAL_SPACE,
        openvdb::GridBase::META_GRID_NAME,
        openvdb::GridBase::META_SAVE_HALF_FLOAT,
        openvdb::GridBase::META_FILE_BBOX_MIN,
        openvdb::GridBase::META_FILE_BBOX_MAX,
        openvdb::GridBase::META_FILE_MEM_BYTES,
        openvdb::GridBase::META_FILE_VOXEL_COUNT,
        openvdb::GridBase::META_FILE_COMPRESSION,
        openvdb::GridBase::META_FILE_DELAYED_LOAD})
    g.removeMeta(key);
  for (const char *key :
       {"container:mac_layout", "container:velocity_units",
        "container:pressure_units", "container:pressure_gauge",
        "container:projection_dt", "container:point_count",
        "container:point_empty_manifest_v1",
        "container:point_empty_descriptor_v1",
        "container:point_empty_leaf_flags_v1", "container:potential_flow_units",
        "container:potential_flow_boundary", "container:surface_field_units",
        "container:surface_field_validity", "container:surface_field_location"})
    g.removeMeta(key);
  g.removeMeta("container:diagnostic_mask");
  g.setName(c.o.maskGridName);
  g.setGridClass(openvdb::GRID_UNKNOWN);
  g.setVectorType(openvdb::VEC_INVARIANT);
  g.setIsInWorldSpace(true);
  g.setSaveFloatAsHalf(false);
  g.insertMeta("container:diagnostic_mask",
               openvdb::StringMetadata(
                   "union of finite located failures; background/global "
                   "failures have no location; no implicit repair"));
}
void publish(Context &c, const Snapshot &snapshot,
             const std::filesystem::path &output) {
  // File::write regenerates bounded native statistics/compression records.
  // Reserve their keys, typed payloads and map nodes before serialization.
  const long double newMetadata =
      metadata(*c.failures) + metadata(*c.loaded.fileMetadata) + 16.0L * 1024;
  if (c.loaded.meta + newMetadata > c.o.maxMetadataBytes)
    throw std::invalid_argument(
        "diagnostics source plus output metadata exceeds maxMetadataBytes");
  if (8 * newMetadata > c.o.maxWork - c.loaded.work)
    throw std::invalid_argument(
        "diagnostics output metadata exceeds shared maxWork");
  bytes(c.loaded.resident + 4.0L * c.failures->memUsage() + newMetadata, c.p,
        "diagnostics mask/metadata/writer exceeds shared byte cap before "
        "serialization");
  c.loaded.work += 8 * newMetadata;
  c.report.workUpperBound = uint64_t(std::ceil(c.loaded.work));
  const auto native = snapshot.directory / "result.vdb";
  auto view = std::make_shared<ExactWriteGrid<Mask>>(*c.failures);
  openvdb::io::File file(native.string());
  file.setCompression(openvdb::io::COMPRESS_ZIP);
  file.write({view}, *c.loaded.fileMetadata);
  file.close();
  bytes(c.loaded.resident + 4.0L * c.failures->memUsage() +
            std::filesystem::file_size(native),
        c.p, "diagnostics encoded output exceeds shared byte cap");
  bool created = false;
  std::ofstream target;
  try {
    std::ifstream input(native, std::ios::binary);
    if (!input)
      throw std::runtime_error("cannot read private diagnostic mask");
    target.open(output, std::ios::out | std::ios::binary | std::ios::noreplace);
    created = target.is_open();
    if (!created)
      throw std::runtime_error(
          "cannot exclusively create diagnostic mask output");
    target.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
      target.write(buffer.data(), input.gcount());
    if (!input.eof())
      throw std::runtime_error("diagnostic mask copy failed");
    target.flush();
    target.close();
  } catch (...) {
    target.exceptions(std::ios::goodbit);
    if (target.is_open())
      target.close();
    if (created) {
      std::error_code ignored;
      std::filesystem::remove(output, ignored);
    }
    throw;
  }
}

// Integration fragment, included inside the diagnostics implementation
// namespace. Requires Context/Loaded/Snapshot, finiteValue(), exactRange(),
// add(), bytes(), and the public point-selection API. No clone, writer or
// source mutation.
namespace point_diagnostic_detail {
using PointLeaf = PD::TreeType::LeafNodeType;

inline void charge(Context &c, long double amount) {
  if (!std::isfinite(amount) || amount < 0 ||
      amount > c.o.maxWork - std::min<long double>(c.loaded.work, c.o.maxWork))
    throw std::invalid_argument("point diagnostics exceeds shared work cap");
  c.loaded.work += amount;
  c.report.workUpperBound = uint64_t(std::ceil(c.loaded.work));
}

template <class T> bool range(const T &value, double low, double high) {
  if constexpr (openvdb::VecTraits<T>::IsVec) {
    for (int axis = 0; axis < openvdb::VecTraits<T>::Size; ++axis)
      if (!exactRange(value[axis], low, high))
        return false;
    return true;
  } else
    return exactRange(value, low, high);
}

inline bool numeric(const points::AttributeArray &array) {
  if (!array.hasConstantStride() || array.stride() != 1 ||
      points::isGroup(array) ||
      !points::AttributeArray::isRegistered(array.type()))
    return false;
  return array.hasValueType<bool>() || array.hasValueType<int32_t>() ||
         array.hasValueType<int64_t>() || array.hasValueType<float>() ||
         array.hasValueType<double>() || array.hasValueType<openvdb::Vec3i>() ||
         array.hasValueType<openvdb::Vec3f>() ||
         array.hasValueType<openvdb::Vec3d>();
}

struct Selection {
  std::array<std::vector<points::GroupHandle>, 3> handles;
  Selection(const PointLeaf &leaf, const PointGroupSelection &groups) {
    const std::array<const std::vector<std::string> *, 3> lists{
        &groups.anyOf, &groups.allOf, &groups.noneOf};
    for (size_t kind = 0; kind < lists.size(); ++kind) {
      handles[kind].reserve(lists[kind]->size());
      for (const auto &name : *lists[kind])
        handles[kind].push_back(leaf.groupHandle(name));
    }
  }
  bool selected(openvdb::Index index) const {
    bool any = handles[0].empty();
    for (const auto &handle : handles[0])
      any |= handle.get(index);
    if (!any)
      return false;
    for (const auto &handle : handles[1])
      if (!handle.get(index))
        return false;
    for (const auto &handle : handles[2])
      if (handle.get(index))
        return false;
    return true;
  }
};

template <class T>
void attribute(Context &c, const PointLeaf &leaf,
               const points::AttributeArray &array,
               const std::vector<uint8_t> &selected, uint64_t ordinalBase,
               size_t check) {
  // UnknownCodec dispatches the registered native codec without narrowing.
  // No collapse on destruction: this read-only check never changes encoding.
  points::AttributeHandle<T> handle(array, false);
  for (auto point = leaf.beginIndexAll(); point; ++point) {
    if (!selected[size_t(*point)])
      continue;
    auto &result = c.report.checks[check];
    add(result.examinedPoints);
    const T value = handle.get(*point);
    if (!finiteValue(value))
      c.pointFailure(check, point.getCoord(), ordinalBase + uint64_t(*point),
                     "selected native numeric value is not finite");
    else if (c.o.componentRange &&
             !range(value, (*c.o.componentRange)[0], (*c.o.componentRange)[1]))
      c.pointFailure(
          check, point.getCoord(), ordinalBase + uint64_t(*point),
          "selected native component is outside the inclusive range");
  }
}

inline void attribute(Context &c, const PointLeaf &leaf,
                      const points::AttributeArray &array,
                      const std::vector<uint8_t> &selected,
                      uint64_t ordinalBase, size_t check) {
#define POINT_DIAGNOSTIC_TYPE(T)                                               \
  if (array.hasValueType<T>()) {                                               \
    attribute<T>(c, leaf, array, selected, ordinalBase, check);                \
    return;                                                                    \
  }
  POINT_DIAGNOSTIC_TYPE(bool)
  POINT_DIAGNOSTIC_TYPE(int32_t)
  POINT_DIAGNOSTIC_TYPE(int64_t)
  POINT_DIAGNOSTIC_TYPE(float)
  POINT_DIAGNOSTIC_TYPE(double)
  POINT_DIAGNOSTIC_TYPE(openvdb::Vec3i)
  POINT_DIAGNOSTIC_TYPE(openvdb::Vec3f)
  POINT_DIAGNOSTIC_TYPE(openvdb::Vec3d)
#undef POINT_DIAGNOSTIC_TYPE
  c.unavailable(
      check, "chosen point attribute has no supported numeric runtime handle");
}

inline bool archivedSchema(Context &c, const Snapshot &snapshot,
                           const PointAttributeOptions &limits,
                           PointAttributeSchema &schema) {
  // The established helper validates the opaque archive by bounded public
  // manifest/hash/flags, never by deserializing an arbitrary Descriptor blob.
  const long double remaining =
      c.p.maxInputBytes - std::ceil(c.loaded.resident);
  if (remaining <= Fixed || remaining > std::numeric_limits<uintmax_t>::max())
    throw std::invalid_argument(
        "point diagnostic archive validation exceeds shared byte cap");
  auto nested = c.p;
  nested.maxInputBytes = uintmax_t(remaining);
  const auto result = CountSelectedPoints(
      snapshot.file, {}, "descriptor:" + c.loaded.descriptor, nested, limits);
  if (!result.archivedSchema || result.total)
    throw std::invalid_argument("invalid native empty-point schema archive");
  schema = result.schema;
  return true;
}
} // namespace point_diagnostic_detail

void pointChecks(Context &c, const PD &grid, const Snapshot &snapshot,
                 const PointAttributeOptions &limits) {
  namespace pointCheck = point_diagnostic_detail;
  const size_t layout =
      c.check("point-layout",
              "Native point offsets, arrays, descriptors and group backing");
  const size_t groups =
      c.check("point-groups",
              "Existence and safe evaluation of requested native point groups");
  const size_t position = c.check(
      "point-position",
      "Selected local P offsets are finite and within their stored voxel");
  std::vector<size_t> attributes;
  attributes.reserve(c.o.pointAttributes.size());
  for (const auto &name : c.o.pointAttributes)
    attributes.push_back(c.check("point-attribute:" + name,
                                 "Selected native numeric attribute finiteness "
                                 "and optional component range"));
  c.report.points.emplace();
  auto &counts = *c.report.points;
  const auto disable = [&](const std::string &why) {
    counts.countsValid = false;
    c.unavailable(position, why);
    for (size_t index : attributes)
      c.unavailable(index, why);
  };
  const size_t terms = c.o.pointGroups.anyOf.size() +
                       c.o.pointGroups.allOf.size() +
                       c.o.pointGroups.noneOf.size();
  bytes(c.loaded.resident + 128.0L * grid.tree().leafCount() + 128.0L * terms +
            128.0L * (limits.maxAttributes + limits.maxGroups) +
            c.loaded.pointUpper,
        c.p,
        "point diagnostic layout/selection scratch exceeds shared byte cap");
  pointCheck::charge(
      c, grid.tree().leafCount() *
             (512.0L + 64.0L * (limits.maxAttributes + limits.maxGroups)));

  std::vector<const pointCheck::PointLeaf *> leaves;
  leaves.reserve(grid.tree().leafCount());
  const points::AttributeSet::Descriptor *descriptor = nullptr;
  bool safe = true;
  const auto badLayout = [&](const std::string &why) {
    safe = false;
    c.global(layout, why);
  };
  if (grid.tree().hasActiveTiles())
    badLayout(
        "PointDataGrid contains active offset tiles without point arrays");
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
    leaves.push_back(&*leaf);
    bool leafSafe = true;
    const auto badLeaf = [&](const std::string &why) {
      leafSafe = false;
      badLayout(why);
    };
    const auto &set = leaf->attributeSet();
    const auto &current = set.descriptor();
    if (!descriptor)
      descriptor = &current;
    else if (*descriptor != current)
      badLeaf("point leaf descriptors/descriptor metadata are heterogeneous");
    if (set.size() != current.map().size() ||
        set.size() > limits.maxAttributes ||
        current.groupMap().size() > limits.maxGroups)
      badLeaf(
          "point attribute/group descriptor count is invalid or over its cap");
    std::set<size_t> indices;
    for (const auto &[name, index] : current.map()) {
      if (index >= set.size() || !indices.insert(index).second) {
        badLeaf(
            "point descriptor has out-of-range or aliased attribute indices");
        continue;
      }
      try {
        if (current.type(index) != leaf->constAttributeArray(index).type())
          badLeaf("point descriptor type disagrees with its native attribute "
                  "array");
      } catch (const openvdb::Exception &error) {
        badLeaf(error.what());
      }
    }
    // This validates all512 monotonically increasing offsets, equal arrays,
    // and final offset==array size. No AttributeHandle exists before this.
    try {
      leaf->validateOffsets();
    } catch (const openvdb::Exception &error) {
      badLeaf(error.what());
    }
    const uint64_t count = uint64_t(leaf->pointCount());
    if (count > std::numeric_limits<openvdb::Index>::max())
      badLeaf(
          "point leaf count cannot be represented by native attribute indices");
    size_t groupStorage = 0;
    for (size_t index = 0; index < set.size(); ++index) {
      const auto &array = leaf->constAttributeArray(index);
      if (array.size() != count)
        badLeaf("point attribute length disagrees with its validated voxel "
                "offsets");
      if (array.hasConstantStride() &&
          (!array.stride() ||
           count > std::numeric_limits<openvdb::Index>::max() / array.stride()))
        badLeaf("point constant-stride array has zero stride or overflowing "
                "native data length");
      groupStorage += size_t(points::isGroup(array));
    }
    const auto p = current.map().find("P");
    if (p == current.map().end() || p->second >= set.size())
      badLeaf("native point leaf lacks the mandatory P array");
    else {
      const auto &array = leaf->constAttributeArray(p->second);
      if (!array.hasValueType<openvdb::Vec3f>() || !array.hasConstantStride() ||
          array.stride() != 1 ||
          !points::AttributeArray::isRegistered(array.type()))
        badLeaf("P requires a registered native Vec3f codec and constant "
                "stride one");
    }
    std::set<size_t> bits;
    for (const auto &[name, offset] : current.groupMap()) {
      if (!bits.insert(offset).second) {
        badLeaf("point groups alias the same backing bit");
        continue;
      }
      if (offset / 8 >= groupStorage) {
        badLeaf("point group offset exceeds available native group storage");
        continue;
      }
      try {
        const auto index = set.groupIndex(name);
        if (index.first >= set.size() || index.second >= 8) {
          badLeaf("point group references an out-of-range backing array/bit");
          continue;
        }
        const auto &backing = leaf->constAttributeArray(index.first);
        if (!points::isGroup(backing) || !backing.hasConstantStride() ||
            backing.stride() != 1)
          badLeaf("point group requires native GroupAttributeArray backing "
                  "with constant stride one");
      } catch (const openvdb::Exception &error) {
        badLeaf(error.what());
      }
    }
    if (leafSafe) {
      add(counts.total, count);
      add(c.report.checks[layout].examinedPoints, count);
      if (counts.total > limits.maxPoints)
        throw std::invalid_argument(
            "validated diagnostic point count exceeds maxPoints");
    }
  }
  std::sort(leaves.begin(), leaves.end(), [](const auto *a, const auto *b) {
    return std::array<int32_t, 3>{a->origin().x(), a->origin().y(),
                                  a->origin().z()} <
           std::array<int32_t, 3>{b->origin().x(), b->origin().y(),
                                  b->origin().z()};
  });
  if (!safe) {
    c.unavailable(groups, "point layout is unsafe for group handles");
    disable("point layout is unsafe for position/attribute decoding");
    return;
  }

  // Native string IDs are structural indices into the shared descriptor,
  // rather than arbitrary opaque scalar values. Validate them without making
  // strings or calling a handle that assumes the dictionary entry exists.
  // Unknown unrelated codecs remain opaque and are not rejected merely for
  // being unselected.
  std::set<openvdb::Index> stringIds;
  uintmax_t stringBytes = 0;
  if (descriptor) {
    for (auto it = descriptor->getMetadata().beginMeta();
         it != descriptor->getMetadata().endMeta(); ++it) {
      if (!it->first.starts_with("string:"))
        continue;
      const auto value =
          std::dynamic_pointer_cast<openvdb::StringMetadata>(it->second);
      const std::string_view digits(it->first.data() + 7, it->first.size() - 7);
      uint64_t index = 0;
      bool valid =
          !digits.empty() && (digits.size() == 1 || digits.front() != '0');
      for (char digit : digits) {
        if (digit < '0' || digit > '9' ||
            index > (uint64_t(std::numeric_limits<openvdb::Index>::max()) - 1 -
                     unsigned(digit - '0')) /
                        10) {
          valid = false;
          break;
        }
        index = index * 10 + unsigned(digit - '0');
      }
      if (!value || !valid) {
        badLayout("native point string dictionary has invalid typed/canonical "
                  "indices");
        continue;
      }
      if (value->value().size() > limits.maxStringBytes - stringBytes)
        throw std::invalid_argument(
            "point diagnostic string dictionary exceeds maxStringBytes");
      stringBytes += value->value().size();
      stringIds.insert(openvdb::Index(index + 1));
    }
  }
  using TruncatedStrings =
      points::TypedAttributeArray<openvdb::Index, points::StringCodec<true>>;
  long double stringSlots = 0;
  for (const auto *leaf : leaves)
    for (size_t index = 0; index < leaf->attributeSet().size(); ++index) {
      const auto &array = leaf->constAttributeArray(index);
      if (array.isType<points::StringAttributeArray>() ||
          array.isType<TruncatedStrings>())
        stringSlots += array.isUniform()
                           ? std::min<openvdb::Index>(array.dataSize(), 1)
                           : array.dataSize();
    }
  pointCheck::charge(c, 8 * stringSlots + 32 * c.loaded.meta);
  if (safe)
    for (const auto *leaf : leaves)
      for (size_t index = 0; index < leaf->attributeSet().size(); ++index) {
        const auto &array = leaf->constAttributeArray(index);
        const auto validateIds = [&](const auto &typed) {
          const auto n = typed.isUniform()
                             ? std::min<openvdb::Index>(typed.dataSize(), 1)
                             : typed.dataSize();
          for (openvdb::Index slot = 0; slot < n; ++slot) {
            const auto id = typed.get(slot);
            if (id && !stringIds.contains(id)) {
              badLayout("native point string ID has no typed dictionary entry");
              break;
            }
          }
        };
        if (array.isType<points::StringAttributeArray>())
          validateIds(static_cast<const points::StringAttributeArray &>(array));
        else if (array.isType<TruncatedStrings>())
          validateIds(static_cast<const TruncatedStrings &>(array));
      }
  if (!safe) {
    c.unavailable(groups, "point string layout is unsafe for dependent checks");
    disable("point string layout is unsafe for dependent checks");
    return;
  }

  PointAttributeSchema archived;
  constexpr const char *archiveKeys[]{"container:point_empty_manifest_v1",
                                      "container:point_empty_descriptor_v1",
                                      "container:point_empty_leaf_flags_v1"};
  size_t archiveCount = 0;
  for (const char *key : archiveKeys)
    archiveCount += bool(grid[key]);
  if (archiveCount) {
    if (!leaves.empty() || counts.total ||
        archiveCount != std::size(archiveKeys)) {
      c.global(layout, "reserved empty-point archive keys conflict with native "
                       "leaves or are incomplete");
      c.unavailable(groups, "native point archive layout is invalid");
      disable("native point archive layout is invalid");
      return;
    }
    pointCheck::charge(c, 64 * c.loaded.meta);
    try {
      counts.archivedSchema =
          pointCheck::archivedSchema(c, snapshot, limits, archived);
    } catch (const std::invalid_argument &error) {
      // Resource-limit errors must propagate rather than masquerade as a bad
      // source. These are the established archive helper's two limit terms.
      const std::string why = error.what();
      if (why.find("budget") != std::string::npos ||
          why.find("memory") != std::string::npos ||
          why.find(" cap") != std::string::npos ||
          why.find("limits") != std::string::npos ||
          why.find("maxPoints") != std::string::npos ||
          why.find("maxStringBytes") != std::string::npos ||
          why.find("exceeds shared") != std::string::npos)
        throw;
      c.global(layout, why);
      c.unavailable(groups, "native point archive layout is invalid");
      disable("native point archive layout is invalid");
      return;
    }
  }
  bool groupsValid = true;
  for (const auto *list : {&c.o.pointGroups.anyOf, &c.o.pointGroups.allOf,
                           &c.o.pointGroups.noneOf})
    for (const auto &name : *list) {
      const bool found =
          descriptor ? descriptor->groupMap().contains(name)
                     : std::find(archived.groups.begin(), archived.groups.end(),
                                 name) != archived.groups.end();
      if (!found) {
        c.global(groups,
                 "requested native point group does not exist: " + name);
        groupsValid = false;
      }
    }
  if (!groupsValid) {
    disable("requested native point group is unavailable");
    return;
  }

  std::vector<bool> supported(attributes.size(), true);
  for (size_t item = 0; item < attributes.size(); ++item) {
    const auto &name = c.o.pointAttributes[item];
    for (const auto *leaf : leaves) {
      const auto found = leaf->attributeSet().descriptor().map().find(name);
      if (found == leaf->attributeSet().descriptor().map().end() ||
          !pointCheck::numeric(leaf->constAttributeArray(found->second))) {
        supported[item] = false;
        c.unavailable(attributes[item],
                      "chosen attribute is missing or is not a registered "
                      "stride-one numeric column: " +
                          name);
        break;
      }
    }
    if (leaves.empty()) {
      const auto found =
          std::find_if(archived.attributes.begin(), archived.attributes.end(),
                       [&](const auto &a) { return a.name == name; });
      if (found == archived.attributes.end() || found->groupStorage ||
          found->stride != 1) {
        supported[item] = false;
      } else {
        const auto &type = found->valueType;
        supported[item] = type == openvdb::typeNameAsString<bool>() ||
                          type == openvdb::typeNameAsString<int32_t>() ||
                          type == openvdb::typeNameAsString<int64_t>() ||
                          type == openvdb::typeNameAsString<float>() ||
                          type == openvdb::typeNameAsString<double>() ||
                          type == openvdb::typeNameAsString<openvdb::Vec3i>() ||
                          type == openvdb::typeNameAsString<openvdb::Vec3f>() ||
                          type == openvdb::typeNameAsString<openvdb::Vec3d>();
        supported[item] =
            supported[item] && points::AttributeArray::isRegistered(
                                   {found->valueType, found->codec});
      }
      if (!supported[item])
        c.unavailable(attributes[item], "empty native schema lacks the chosen "
                                        "supported numeric attribute: " +
                                            name);
    }
    if (supported[item])
      counts.checkedAttributes.push_back(name);
  }
  uint64_t ordinalBase = 0;
  for (const auto *leaf : leaves) {
    const auto count = uint64_t(leaf->pointCount());
    std::vector<uint8_t> selected(size_t(count), uint8_t(0));
    pointCheck::Selection predicate(*leaf, c.o.pointGroups);
    for (auto point = leaf->beginIndexAll(); point; ++point) {
      if (!predicate.selected(*point))
        continue;
      selected[size_t(*point)] = 1;
      add(counts.selected);
      add(leaf->isValueOn(point.getCoord()) ? counts.selectedActive
                                            : counts.selectedInactive);
      add(c.report.checks[groups].examinedPoints);
    }
    try {
      points::AttributeHandle<openvdb::Vec3f> positions(
          leaf->constAttributeArray("P"), false);
      for (auto point = leaf->beginIndexAll(); point; ++point) {
        if (!selected[size_t(*point)])
          continue;
        add(c.report.checks[position].examinedPoints);
        const auto value = positions.get(*point);
        if (!finiteValue(value) || std::abs(value.x()) > .5f ||
            std::abs(value.y()) > .5f || std::abs(value.z()) > .5f)
          c.pointFailure(position, point.getCoord(),
                         ordinalBase + uint64_t(*point),
                         "P is nonfinite or outside the stored voxel-local "
                         "offset interval");
      }
    } catch (const openvdb::Exception &error) {
      c.unavailable(position, error.what());
    }
    for (size_t item = 0; item < attributes.size(); ++item)
      if (supported[item]) {
        try {
          pointCheck::attribute(
              c, *leaf, leaf->constAttributeArray(c.o.pointAttributes[item]),
              selected, ordinalBase, attributes[item]);
        } catch (const openvdb::Exception &error) {
          c.unavailable(attributes[item], error.what());
        }
      }
    add(ordinalBase, count);
  }
}

DiagnosticReport execute(const std::filesystem::path &input,
                         const std::filesystem::path &output,
                         const DiagnosticsOptions &o,
                         const std::string &selector,
                         const ProcessingOptions &p,
                         const PointAttributeOptions &limits, bool writing) {
  validate(o, p, limits, writing);
  if (writing) {
#ifndef OPENVDB_USE_ZLIB
    throw std::invalid_argument(
        "diagnostic mask output requires OpenVDB ZIP support");
#endif
    if (output.empty() || std::filesystem::exists(output) ||
        (output.has_parent_path() &&
         !std::filesystem::is_directory(output.parent_path())))
      throw std::invalid_argument(
          "diagnostic mask output must be a new path in an existing directory");
  }
  point_attribute_detail::initialize();
  static std::once_flag numericTypes;
  std::call_once(numericTypes, [] {
    point_attribute_detail::registerType<openvdb::Vec3i>();
    using TruncatedStrings =
        points::TypedAttributeArray<openvdb::Index, points::StringCodec<true>>;
    if (!TruncatedStrings::isRegistered())
      TruncatedStrings::registerType();
  });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Snapshot snapshot(input, p, o.maxWork);
    auto loaded = load(snapshot, o, selector, p, limits);
    Context c(o, p, loaded, writing);
    reportStructure(c);
    if (loaded.grid->isType<PD>()) {
      if (o.profile != DiagnosticProfile::PointData) {
        const auto type =
            c.check("profile-type", "Requested field profile type");
        c.global(
            type,
            "PointData requires the explicit PointData diagnostic profile");
        const auto finite = c.check("finite", "Stored field finiteness");
        c.unavailable(finite,
                      "PointData payload requires the explicit point checks");
        const auto pointGrid = openvdb::gridPtrCast<PD>(loaded.grid);
        profileChecks(c, *pointGrid);
        gradientChecks(c, *pointGrid);
      } else
        pointChecks(c, *openvdb::gridPtrCast<PD>(loaded.grid), snapshot,
                    limits);
    } else {
      if (o.profile == DiagnosticProfile::PointData) {
        const auto type = c.check("point-layout", "Native PointData layout");
        c.global(type, "PointData profile requires PointDataGrid");
        for (const auto &item :
             {std::pair{"point-groups", "Point group selection"},
              std::pair{"point-position", "Selected voxel-local P offsets"}}) {
          const auto dependent = c.check(item.first, item.second);
          c.unavailable(dependent, "selected grid is not PointDataGrid");
        }
        for (const auto &attribute : o.pointAttributes) {
          const auto dependent =
              c.check("point-attribute:" + attribute,
                      "Selected native numeric point attribute");
          c.unavailable(dependent, "selected grid is not PointDataGrid");
        }
      } else {
        visit(*loaded.grid, [&](const auto &g) {
          using G = std::decay_t<decltype(g)>;
          if constexpr (!std::is_same_v<G, PD>) {
            storedChecks(c, g);
            profileChecks(c, g);
            gradientChecks(c, g);
          }
          return 0;
        });
      }
    }
    c.finish();
    if (writing) {
      maskStructure(c);
      const MapInfo map(*loaded.grid);
      c.report.mask =
          ProcessingResult{o.maskGridName, "unknown", c.report.maskVoxelCount,
                           map.finite ? map.h : 0};
      publish(c, snapshot, output);
    }
    return std::move(c.report);
  });
}
#endif
} // namespace

DiagnosticReport DiagnoseVdb(const std::filesystem::path &input,
                             const DiagnosticsOptions &o,
                             const std::string &selector,
                             const ProcessingOptions &p,
                             const PointAttributeOptions &limits) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return execute(input, {}, o, selector, p, limits, false);
#else
  (void)input;
  (void)o;
  (void)selector;
  (void)p;
  (void)limits;
  unavailable();
#endif
}
DiagnosticReport WriteVdbDiagnosticMask(const std::filesystem::path &input,
                                        const std::filesystem::path &output,
                                        const DiagnosticsOptions &o,
                                        const std::string &selector,
                                        const ProcessingOptions &p,
                                        const PointAttributeOptions &limits) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return execute(input, output, o, selector, p, limits, true);
#else
  (void)input;
  (void)output;
  (void)o;
  (void)selector;
  (void)p;
  (void)limits;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
