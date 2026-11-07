#include "Container/geometry/OpenVdbFieldResampling.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/tools/Count.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#include <type_traits>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB field resampling is unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = openvdb::Vec3d;
using C = openvdb::Coord;
using Box = openvdb::CoordBBox;
constexpr long double Fixed = 1024 * 1024;
constexpr std::array<double, 16> Identity{1, 0, 0, 0, 0, 1, 0, 0,
                                          0, 0, 1, 0, 0, 0, 0, 1};
double norm(const V &v) {
  return std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])});
}
double gap(double x) {
  return std::max(std::nextafter(x, INFINITY) - x,
                  x - std::nextafter(x, -INFINITY));
}
void budget(long double bytes, const ProcessingOptions &p,
            const char *message) {
  if (!std::isfinite(bytes) || bytes > p.maxInputBytes)
    throw std::invalid_argument(message);
}
long double metadata(const openvdb::MetaMap &map) {
  long double n = sizeof(map);
  for (auto i = map.beginMeta(); i != map.endMeta(); ++i)
    n += 512 + 8.0L * (i->first.size() + i->second->typeName().size() +
                       i->second->size());
  return n;
}
void matrix(const std::array<double, 16> &m) {
  for (double v : m)
    if (!std::isfinite(v))
      throw std::invalid_argument("field resampling matrices must be finite");
  if (m[12] != 0 || m[13] != 0 || m[14] != 0 || m[15] != 1)
    throw std::invalid_argument("field resampling matrices require row-major "
                                "COLUMN affine entries and final row0,0,0,1");
}
struct Sum {
  double value = 0, correction = 0, magnitude = 0;
  void add(double v) {
    const double next = value + v;
    correction += std::abs(value) >= std::abs(v) ? (value - next) + v
                                                 : (v - next) + value;
    value = next;
    magnitude += std::abs(v);
  }
  void product(double a, double b) {
    const double v = a * b;
    if (!std::isfinite(v) || (a != 0 && b != 0 && v == 0))
      throw std::invalid_argument(
          "field resampling arithmetic product overflows or underflows");
    add(v);
    add(std::fma(a, b, -v));
  }
  double get() const {
    const double v = value + correction;
    if (!std::isfinite(v))
      throw std::invalid_argument(
          "field resampling arithmetic sum is nonfinite");
    return v;
  }
  double error() const {
    return 64 * std::numeric_limits<double>::epsilon() *
               std::numeric_limits<double>::epsilon() * magnitude +
           gap(get());
  }
};
struct Map {
  V origin;
  std::array<V, 3> axes, inverse;
  double condition = 0, inverseNorm = 0;
  Map(const V &p, const std::array<V, 3> &a) : origin(p), axes(a) {
    auto unit = a;
    double lengths[3];
    for (int k = 0; k < 3; ++k) {
      lengths[k] = std::hypot(a[k][0], a[k][1], a[k][2]);
      if (!std::isfinite(p[k]) || !std::isfinite(lengths[k]) ||
          lengths[k] < 1e-6 || lengths[k] > 1e6)
        throw std::invalid_argument(
            "field resampling affine axes must span[1e-6,1e6]");
      unit[k] /= lengths[k];
    }
    const double det = unit[0].dot(unit[1].cross(unit[2]));
    if (!std::isfinite(det) || std::abs(det) < 1e-8)
      throw std::invalid_argument(
          "field resampling affine rank/conditioning is inadequate");
    inverse = {unit[1].cross(unit[2]) / (det * lengths[0]),
               unit[2].cross(unit[0]) / (det * lengths[1]),
               unit[0].cross(unit[1]) / (det * lengths[2])};
    double forwardNorm = 0;
    for (int k = 0; k < 3; ++k) {
      forwardNorm =
          std::max(forwardNorm,
                   std::abs(a[0][k]) + std::abs(a[1][k]) + std::abs(a[2][k]));
      inverseNorm = std::max(inverseNorm, std::abs(inverse[k][0]) +
                                              std::abs(inverse[k][1]) +
                                              std::abs(inverse[k][2]));
      for (int j = 0; j < 3; ++j)
        if (!std::isfinite(inverse[k][j]))
          throw std::invalid_argument(
              "field resampling affine inverse is nonfinite");
    }
    condition = forwardNorm * inverseNorm;
    if (!std::isfinite(condition) || condition > 1e8)
      throw std::invalid_argument(
          "field resampling affine inverse is insufficiently conditioned");
  }
  explicit Map(const std::array<double, 16> &m)
      : Map(V(m[3], m[7], m[11]),
            {V(m[0], m[4], m[8]), V(m[1], m[5], m[9]), V(m[2], m[6], m[10])}) {}
  explicit Map(const openvdb::math::Transform &t)
      : Map(t.baseMap()->applyMap(V(0)),
            {t.baseMap()->applyJacobian({1, 0, 0}),
             t.baseMap()->applyJacobian({0, 1, 0}),
             t.baseMap()->applyJacobian({0, 0, 1})}) {
    if (!t.isLinear())
      throw std::invalid_argument(
          "field resampling requires affine source/target maps");
  }
  V linear(const V &v) const {
    V r;
    for (int k = 0; k < 3; ++k) {
      Sum s;
      for (int j = 0; j < 3; ++j)
        s.product(axes[j][k], v[j]);
      r[k] = s.get();
    }
    return r;
  }
  V inv(const V &v) const {
    V r;
    for (int k = 0; k < 3; ++k) {
      Sum s;
      for (int j = 0; j < 3; ++j)
        s.product(inverse[k][j], v[j]);
      r[k] = s.get();
    }
    return r;
  }
};
openvdb::math::Transform::Ptr nativeMap(const std::array<double, 16> &values) {
  matrix(values);
  const Map checked(values);
  (void)checked;
  openvdb::Mat4d m;
  for (int a = 0; a < 4; ++a)
    for (int b = 0; b < 4; ++b)
      m[a][b] = values[4 * b + a];
  bool diagonal = true;
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b)
      if (a != b && m[a][b] != 0)
        diagonal = false;
  if (!diagonal)
    return detail::MakeAffineTransformExact(m);
  const V scale(m[0][0], m[1][1], m[2][2]), origin(m[3][0], m[3][1], m[3][2]),
      inv = V(1) / scale;
  std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
  record.exceptions(std::ios::badbit | std::ios::failbit);
  openvdb::writeString(record, openvdb::math::ScaleTranslateMap::mapType());
  origin.write(record);
  scale.write(record);
  V(std::abs(scale[0]), std::abs(scale[1]), std::abs(scale[2])).write(record);
  inv.write(record);
  V(inv * inv).write(record);
  V(inv * .5).write(record);
  record.seekg(0);
  openvdb::io::setCurrentVersion(record);
  auto result = std::make_shared<openvdb::math::Transform>();
  result->read(record);
  return result;
}
bool same(const openvdb::math::Transform &a,
          const openvdb::math::Transform &b) {
  auto x = a.baseMap(), y = b.baseMap();
  if (x->applyMap(V(0)) != y->applyMap(V(0)))
    return false;
  for (const V e : {V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)})
    if (x->applyJacobian(e) != y->applyJacobian(e))
      return false;
  return true;
}
struct Mapping {
  V offset;
  std::array<V, 3> columns;
  double constantError = 0, condition = 1;
  Mapping(const Map &source, const Map &target, const Map &deform, bool pull) {
    V delta;
    double error = 0;
    for (int row = 0; row < 3; ++row) {
      Sum s;
      s.add(pull ? target.origin[row] : -target.origin[row]);
      s.add(pull ? -deform.origin[row] : deform.origin[row]);
      for (int c = 0; c < 3; ++c)
        s.product((pull ? -1. : 1.) * deform.axes[c][row], source.origin[c]);
      delta[row] = s.get();
      error = std::max(error, s.error());
    }
    condition = source.condition * target.condition * deform.condition;
    if (pull) {
      offset = source.inv(deform.inv(delta));
      for (int k = 0; k < 3; ++k)
        columns[k] = source.inv(deform.inv(target.axes[k]));
      constantError = error * source.inverseNorm * deform.inverseNorm;
    } else {
      offset = target.inv(delta);
      for (int k = 0; k < 3; ++k)
        columns[k] = target.inv(deform.linear(source.axes[k]));
      constantError = error * target.inverseNorm;
    }
    if (!std::isfinite(condition) || !std::isfinite(constantError))
      throw std::invalid_argument(
          "field resampling joint map uncertainty is nonfinite");
  }
  V at(const V &index) const {
    V out;
    double envelope = std::max(1., norm(offset));
    for (int row = 0; row < 3; ++row) {
      Sum s;
      s.add(offset[row]);
      double terms = std::abs(offset[row]);
      for (int c = 0; c < 3; ++c) {
        s.product(columns[c][row], index[c]);
        terms += std::abs(columns[c][row] * index[c]);
      }
      out[row] = s.get();
      envelope = std::max(envelope, terms);
      if (s.error() > 1e-4)
        throw std::invalid_argument("field resampling coordinate sum loses "
                                    "source/target voxel precision");
    }
    const double error =
        constantError +
        256 * std::numeric_limits<double>::epsilon() * condition * envelope;
    if (!std::isfinite(error) || error > 1e-4)
      throw std::invalid_argument(
          "field resampling joint inverse/coordinate uncertainty exceeds1e-4 "
          "voxel; recenter/condition maps");
    for (int k = 0; k < 3; ++k)
      if (!std::isfinite(out[k]) || out[k] < double(INT32_MIN) + 8192 ||
          out[k] > double(INT32_MAX) - 8192 || gap(out[k]) > 1e-4)
        throw std::invalid_argument("field resampling source/target coordinate "
                                    "exceeds native precision/index margins");
    return out;
  }
};
template <class F> decltype(auto) dispatch(openvdb::GridBase &grid, F &&fn) {
  if (grid.isType<openvdb::FloatGrid>())
    return fn(static_cast<openvdb::FloatGrid &>(grid));
  if (grid.isType<openvdb::DoubleGrid>())
    return fn(static_cast<openvdb::DoubleGrid &>(grid));
  if (grid.isType<openvdb::BoolGrid>())
    return fn(static_cast<openvdb::BoolGrid &>(grid));
  if (grid.isType<openvdb::MaskGrid>())
    return fn(static_cast<openvdb::MaskGrid &>(grid));
  if (grid.isType<openvdb::Int32Grid>())
    return fn(static_cast<openvdb::Int32Grid &>(grid));
  if (grid.isType<openvdb::Int64Grid>())
    return fn(static_cast<openvdb::Int64Grid &>(grid));
  if (grid.isType<openvdb::Vec3IGrid>())
    return fn(static_cast<openvdb::Vec3IGrid &>(grid));
  if (grid.isType<openvdb::Vec3SGrid>())
    return fn(static_cast<openvdb::Vec3SGrid &>(grid));
  if (grid.isType<openvdb::Vec3DGrid>())
    return fn(static_cast<openvdb::Vec3DGrid &>(grid));
  throw std::invalid_argument(
      "field resampling supports nine native nonpoint grid types");
}
bool supported(openvdb::GridBase &grid) {
  return grid.isType<openvdb::FloatGrid>() ||
         grid.isType<openvdb::DoubleGrid>() ||
         grid.isType<openvdb::BoolGrid>() || grid.isType<openvdb::MaskGrid>() ||
         grid.isType<openvdb::Int32Grid>() ||
         grid.isType<openvdb::Int64Grid>() ||
         grid.isType<openvdb::Vec3IGrid>() ||
         grid.isType<openvdb::Vec3SGrid>() || grid.isType<openvdb::Vec3DGrid>();
}
void semantics(const openvdb::GridBase &grid) {
  for (const auto *key : {openvdb::GridBase::META_GRID_CLASS,
                          openvdb::GridBase::META_VECTOR_TYPE})
    if (grid[key] &&
        !std::dynamic_pointer_cast<const openvdb::StringMetadata>(grid[key]))
      throw std::invalid_argument(
          "field resampling semantic metadata has wrong native type");
  if (grid[openvdb::GridBase::META_IS_LOCAL_SPACE] &&
      !std::dynamic_pointer_cast<const openvdb::BoolMetadata>(
          grid[openvdb::GridBase::META_IS_LOCAL_SPACE]))
    throw std::invalid_argument(
        "field resampling world/local semantic metadata has wrong native type");
  if (const auto v = grid.getMetadata<openvdb::StringMetadata>(
          openvdb::GridBase::META_VECTOR_TYPE);
      v &&
      v->value() != openvdb::GridBase::vecTypeToString(grid.getVectorType()))
    throw std::invalid_argument(
        "field resampling vector semantic tag is unrecognized");
  if (const auto v = grid.getMetadata<openvdb::StringMetadata>(
          openvdb::GridBase::META_GRID_CLASS);
      v &&
      v->value() != openvdb::GridBase::gridClassToString(grid.getGridClass()))
    throw std::invalid_argument(
        "field resampling grid class tag is unrecognized");
  if ((grid.getGridClass() != openvdb::GRID_UNKNOWN &&
       grid.getGridClass() != openvdb::GRID_FOG_VOLUME) ||
      !grid.isInWorldSpace())
    throw std::invalid_argument("field resampling requires ordinary WORLD "
                                "grids; SDF/staggered/local semantics reject");
  dispatch(const_cast<openvdb::GridBase &>(grid), [&](const auto &typed) {
    using T = typename std::decay_t<decltype(typed)>::ValueType;
    if constexpr (!std::is_arithmetic_v<T>) {
      if (grid.getGridClass() != openvdb::GRID_UNKNOWN)
        throw std::invalid_argument(
            "field resampling vector grids require UNKNOWN class");
    } else if (grid.getVectorType() != openvdb::VEC_INVARIANT)
      throw std::invalid_argument(
          "field resampling scalar grids require invariant semantics");
    return 0;
  });
}
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument(
          "field resampling input is not a regular file");
    encoded = std::filesystem::file_size(input);
    budget(Fixed + static_cast<long double>(encoded), p,
           "field resampling encoded source exceeds byte cap");
    static std::atomic_uint64_t serial{};
    for (int n = 0; n < 64; ++n) {
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-field-resampling-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(candidate)) {
        directory = std::move(candidate);
        file = directory / "source.vdb";
        break;
      }
    }
    if (file.empty())
      throw std::runtime_error("cannot reserve field resampling snapshot");
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open field resampling snapshot");
      target.exceptions(std::ios::badbit | std::ios::failbit);
      std::array<char, 65536> buffer{};
      auto left = encoded;
      while (left) {
        const auto n =
            std::streamsize(std::min<uintmax_t>(left, buffer.size()));
        source.read(buffer.data(), n);
        if (source.gcount() != n)
          throw std::runtime_error(
              "field resampling source changed while snapshotting");
        target.write(buffer.data(), n);
        left -= uintmax_t(n);
      }
      if (source.bad() || source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "field resampling source grew while snapshotting");
      target.close();
    } catch (...) {
      cleanup();
      throw;
    }
  }
  void cleanup() const noexcept {
    std::error_code e;
    std::filesystem::remove(file, e);
    std::filesystem::remove(directory, e);
  }
  ~Snapshot() { cleanup(); }
};
class Archive : public openvdb::io::Archive {
public:
  void begin(std::istream &s, openvdb::io::StreamMetadata::Ptr &metadata) {
    readHeader(s);
    if (!inputHasGridOffsets() ||
        fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
        fileVersion() > openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument(
          "field resampling requires supported seekable VDB offsets");
    openvdb::io::setStreamMetadataPtr(s, metadata, false);
    setFormatVersion(s);
    setLibraryVersion(s);
    setDataCompression(s);
  }
  int32_t count(std::istream &s) { return readGridCount(s); }
  openvdb::GridBase::Ptr next(std::istream &s, openvdb::io::GridDescriptor &d,
                              uintmax_t bytes) {
    auto grid = d.read(s);
    const auto after = int64_t(s.tellg()), start = d.getGridPos(),
               end = d.getEndPos(), blocks = d.getBlockPos();
    if (!grid || start < after || end <= start || uintmax_t(end) > bytes ||
        (!d.isInstance() && (blocks < start || blocks > end)))
      throw std::invalid_argument(
          "field resampling descriptor offsets are outside encoded source");
    d.seekToGrid(s);
    readGridCompression(s);
    grid->readMeta(s);
    grid->readTransform(s);
    if (fileVersion() < openvdb::OPENVDB_FILE_VERSION_NO_GRIDMAP &&
        grid->getName().empty())
      grid->setName(d.gridName());
    if (int64_t(s.tellg()) > (d.isInstance() ? end : blocks))
      throw std::invalid_argument(
          "field resampling metadata crosses payload offset");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    d.seekToEnd(s);
    return grid;
  }
};
struct Source {
  std::unique_ptr<Snapshot> snapshot;
  openvdb::GridBase::Ptr grid;
  openvdb::MetaMap::Ptr fileMetadata;
  long double resident = 0, metadataBytes = 0, validationWork = 0;
};
Source load(const std::filesystem::path &input, const std::string &selector,
            const FieldResamplingOptions &o, const ProcessingOptions &p) {
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded field resampling requires delayed "
                                "loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded field resampling requires delayed-loading SDK support");
#else
  Source r;
  r.snapshot = std::make_unique<Snapshot>(input, p);
  detail::RequireSeekableFile(r.snapshot->file);
  auto sm = std::make_shared<openvdb::io::StreamMetadata>();
  sm->setSeekable(true);
  std::ifstream stream(r.snapshot->file, std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  Archive archive;
  archive.begin(stream, sm);
  r.fileMetadata = std::make_shared<openvdb::MetaMap>();
  r.fileMetadata->readMeta(stream);
  r.metadataBytes = metadata(*r.fileMetadata);
  long double catalog = static_cast<long double>(r.snapshot->encoded) + Fixed;
  const auto check = [&] {
    if (r.metadataBytes > o.maxMetadataBytes)
      throw std::invalid_argument(
          "field resampling aggregate file/grid metadata exceeds cap");
    budget(catalog + 4 * r.metadataBytes, p,
           "field resampling catalog/metadata exceeds byte cap");
  };
  check();
  const auto count = archive.count(stream);
  if (count < 0 || uint64_t(count) > o.maxGrids)
    throw std::invalid_argument("field resampling catalog exceeds maxGrids");
  std::set<std::string> names;
  std::string chosen;
  bool found = false;
  openvdb::math::Transform::Ptr exact;
  for (int32_t n = 0; n < count; ++n) {
    openvdb::io::GridDescriptor d;
    auto grid = archive.next(stream, d, r.snapshot->encoded);
    const auto name = openvdb::io::GridDescriptor::nameAsString(d.uniqueName());
    catalog += 4.0L * name.size() + 256;
    if (!names.insert(name).second)
      throw std::invalid_argument(
          "field resampling source has colliding descriptors");
    r.metadataBytes += metadata(*grid);
    check();
    if (!supported(*grid) ||
        !GridSelectionMatches(selector, name, grid->getName()))
      continue;
    if (selector.empty() && found)
      continue;
    if (found)
      throw std::invalid_argument(
          "field resampling grid selector is ambiguous");
    semantics(*grid);
    const auto memory = grid->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES),
               voxels = grid->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!memory || !voxels || memory->value() < 0 || voxels->value() < 0 ||
        uint64_t(voxels->value()) > p.maxVoxelCount)
      throw std::invalid_argument("field resampling requires valid recorded "
                                  "memory/voxel statistics within cap");
    budget(catalog + 4 * r.metadataBytes + memory->value(), p,
           "recorded field resampling source exceeds byte cap");
    chosen = name;
    found = true;
    exact = detail::CopyTransformExact(grid->transform());
  }
  if (!found)
    throw std::invalid_argument(
        "no matching native ordinary field for resampling");
  stream.close();
  openvdb::io::File file(r.snapshot->file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error("field resampling private snapshot changed");
  r.grid = file.readGrid(chosen);
  if (!same(r.grid->transform(), *exact))
    throw std::runtime_error("field resampling native read changed source map");
  r.grid->setTransform(exact);
  r.metadataBytes += metadata(*r.grid);
  check();
  semantics(*r.grid);
  const auto full = dispatch(*r.grid, [&](const auto &grid) -> long double {
    using Tree = typename std::decay_t<decltype(grid)>::TreeType;
    using Lower = typename Tree::RootNodeType::ChildNodeType::ChildNodeType;
    using Upper = typename Tree::RootNodeType::ChildNodeType;
    const auto &tree = grid.tree();
    if (tree.leafCount() > p.maxVoxelCount / 512 ||
        tree.activeVoxelCount() > p.maxVoxelCount)
      throw std::invalid_argument(
          "field resampling source leaf/activity cap exceeded");
    const auto nodes = tree.nodeCount();
    r.validationWork = tree.root().getTableSize() + nodes[0] * 512.0L +
                       nodes[1] * 1.0L * Lower::NUM_VALUES +
                       nodes[2] * 1.0L * Upper::NUM_VALUES;
    if (r.validationWork > o.maxWorkItems)
      throw std::invalid_argument(
          "field resampling source node-table validation exceeds work cap "
          "before traversal");
    return openvdb::tools::memUsageIfLoaded(tree, false);
  });
  r.resident = catalog + 8 * r.metadataBytes + full + Fixed;
  budget(r.resident + full, p,
         "field resampling full typed buffer exceeds byte cap before paging");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full)
    throw std::runtime_error(
        "field resampling payload exceeds full-buffer preflight");
  file.close();
  return r;
#endif
}
template <class T> bool sameBits(const T &a, const T &b) {
  if constexpr (std::is_floating_point_v<T>)
    return std::memcmp(&a, &b, sizeof(T)) == 0;
  else if constexpr (std::is_arithmetic_v<T>)
    return a == b;
  else {
    for (int k = 0; k < 3; ++k)
      if (!sameBits(a[k], b[k]))
        return false;
    return true;
  }
}
template <class T> void finiteValue(const T &value, bool fog) {
  if constexpr (std::is_arithmetic_v<T>) {
    bool negative = false;
    if constexpr (std::is_signed_v<T>)
      negative = value < 0;
    if (!std::isfinite(double(value)) || (fog && negative))
      throw std::invalid_argument(
          "field resampling requires finite/nonnegative fog values, including "
          "inactive/background");
  } else
    for (int k = 0; k < 3; ++k)
      finiteValue(value[k], false);
}
long double cells(const Box &b) {
  if (b.empty())
    return 0;
  long double n = 1;
  for (int k = 0; k < 3; ++k)
    n *= int64_t(b.max()[k]) - b.min()[k] + 1;
  return n;
}
int64_t block(int64_t x, int64_t width) {
  return x >= 0 ? x / width : (x + 1) / width - 1;
}
long double blocks(const Box &b, int64_t width) {
  if (b.empty())
    return 0;
  long double n = 1;
  for (int k = 0; k < 3; ++k)
    n *= block(b.max()[k], width) - block(b.min()[k], width) + 1;
  return n;
}
template <class Grid> long double treeBytes(const Box &b) {
  using Tree = typename Grid::TreeType;
  using Upper = typename Tree::RootNodeType::ChildNodeType;
  using Lower = typename Upper::ChildNodeType;
  using Leaf = typename Tree::LeafNodeType;
  return blocks(b, 8) * (sizeof(Leaf) + 4096.0L) +
         blocks(b, 128) * (sizeof(Lower) + 4096.0L) +
         blocks(b, 4096) * (sizeof(Upper) + 4096.0L) + Fixed;
}
std::optional<std::array<int32_t, 6>> bounds(const Box &box) {
  if (box.empty())
    return {};
  return std::array<int32_t, 6>{box.min()[0], box.min()[1], box.min()[2],
                                box.max()[0], box.max()[1], box.max()[2]};
}
Box targetBox(const Box &support, const Mapping &forward, int radius) {
  if (support.empty())
    return {};
  V minimum(INFINITY), maximum(-INFINITY);
  for (int mask = 0; mask < 8; ++mask) {
    V corner;
    for (int k = 0; k < 3; ++k)
      corner[k] = (mask & (1 << k)) ? double(support.max()[k]) + radius + 1
                                    : double(support.min()[k]) - radius - 1;
    const auto q = forward.at(corner);
    for (int k = 0; k < 3; ++k) {
      minimum[k] = std::min(minimum[k], q[k]);
      maximum[k] = std::max(maximum[k], q[k]);
    }
  }
  C low, high;
  for (int k = 0; k < 3; ++k) {
    low[k] = int32_t(std::floor(minimum[k])) - 1;
    high[k] = int32_t(std::ceil(maximum[k])) + 1;
  }
  return Box(low, high);
}
void preflightPull(const Mapping &pull, const Box &box) {
  if (box.empty())
    return;
  for (int mask = 0; mask < 8; ++mask) {
    V q;
    for (int k = 0; k < 3; ++k)
      q[k] = (mask & (1 << k)) ? box.max()[k] : box.min()[k];
    (void)pull.at(q);
  }
}
C nearest(const V &q) {
  C result = C::floor(q);
  for (int k = 0; k < 3; ++k)
    // floor(q + .5) can round a value just below the midpoint onto the next
    // integer. The half-integer comparison is exact throughout our Int32 range.
    if (q[k] >= double(result[k]) + .5)
      ++result[k];
  return result;
}
struct Footprint {
  C base;
  V fraction;
  explicit Footprint(const V &q)
      : base(C::floor(q)), fraction(q - base.asVec3d()) {
    for (int k = 0; k < 3; ++k)
      if (fraction[k] < 0 || fraction[k] >= 1 ||
          (fraction[k] == 0 && q[k] != double(base[k])))
        throw std::invalid_argument(
            "field resampling interpolation fraction loses coordinate "
            "precision; "
            "choose nearest or recenter the target map");
  }
};
double quadratic(double a, double b, double c, double t) {
  if (t == 0)
    return b;
  if (t == 1)
    return c;
  const double scale = std::max({std::abs(a), std::abs(b), std::abs(c)});
  if (scale == 0)
    return 0;
  const double square = t * t;
  const double weights[3]{.5 * t * (t - 1), 1 - square, .5 * t * (t + 1)},
      values[3]{a, b, c};
  if (square == 0 || std::fpclassify(square) == FP_SUBNORMAL ||
      weights[0] == 0 || weights[2] == 0 ||
      std::fpclassify(weights[0]) == FP_SUBNORMAL ||
      std::fpclassify(weights[2]) == FP_SUBNORMAL)
    throw std::invalid_argument(
        "quadratic field resampling kernel weights lose dynamic range; "
        "choose nearest/linear or recenter the target map");
  Sum sum;
  for (int k = 0; k < 3; ++k)
    if (values[k] != 0 && weights[k] != 0) {
      const double normalized = values[k] / scale,
                   product = normalized * weights[k];
      if (normalized == 0 || product == 0 ||
          std::fpclassify(normalized) == FP_SUBNORMAL ||
          std::fpclassify(product) == FP_SUBNORMAL)
        throw std::invalid_argument(
            "quadratic field resampling dynamic range loses contributions; "
            "choose nearest/linear or rescale");
      sum.product(normalized, weights[k]);
    }
  const double normalized = sum.get(), result = normalized * scale;
  if (!std::isfinite(result) || (normalized != 0 && result == 0))
    throw std::invalid_argument(
        "quadratic field resampling overflows or underflows");
  return result;
}
double linear(double a, double b, double t) {
  if (t == 0)
    return a;
  if (t == 1)
    return b;
  const double result = std::lerp(a, b, t);
  if (!std::isfinite(result))
    throw std::invalid_argument("linear field resampling overflows Double");
  if (result == 0 && ((a >= 0 && b >= 0) || (a <= 0 && b <= 0)) &&
      ((a != 0 && 1 - t != 0) || (b != 0 && t != 0)))
    throw std::invalid_argument(
        "linear field resampling nonzero result underflows Double");
  return result;
}
template <class T> double part(const T &value, int component) {
  if constexpr (std::is_arithmetic_v<T>) {
    (void)component;
    return double(value);
  } else
    return double(value[component]);
}
template <class Grid>
V interpolate(const Grid &grid, const V &q, FieldResamplingInterpolation mode) {
  const Footprint footprint(q);
  const auto &base = footprint.base;
  const auto &t = footprint.fraction;
  V result(0);
  constexpr int components =
      std::is_arithmetic_v<typename Grid::ValueType> ? 1 : 3;
  const auto get = [&](const C &c, int k) {
    return part(grid.tree().getValue(c), k);
  };
  for (int k = 0; k < components; ++k) {
    if (mode == FieldResamplingInterpolation::Linear) {
      double yz[2];
      for (int z = 0; z < 2; ++z) {
        double yv[2];
        for (int y = 0; y < 2; ++y)
          yv[y] = linear(get(base + C(0, y, z), k), get(base + C(1, y, z), k),
                         t[0]);
        yz[z] = linear(yv[0], yv[1], t[1]);
      }
      result[k] = linear(yz[0], yz[1], t[2]);
    } else {
      double yz[3];
      for (int z = -1; z <= 1; ++z) {
        double yv[3];
        for (int y = -1; y <= 1; ++y)
          yv[y + 1] =
              quadratic(get(base + C(-1, y, z), k), get(base + C(0, y, z), k),
                        get(base + C(1, y, z), k), t[0]);
        yz[z + 1] = quadratic(yv[0], yv[1], yv[2], t[1]);
      }
      result[k] = quadratic(yz[0], yz[1], yz[2], t[2]);
    }
  }
  return result;
}
template <class Grid>
bool active(const Grid &grid, const V &q, const FieldResamplingOptions &o) {
  if (o.interpolation == FieldResamplingInterpolation::Nearest ||
      o.activity == FieldResamplingActivity::NearestSource)
    return grid.tree().isValueOn(nearest(q));
  const Footprint footprint(q);
  const auto &base = footprint.base;
  const auto &t = footprint.fraction;
  const int low = o.interpolation == FieldResamplingInterpolation::Linear ? 0
                                                                          : -1,
            high = 1;
  for (int z = low; z <= high; ++z)
    for (int y = low; y <= high; ++y)
      for (int x = low; x <= high; ++x) {
        const int offset[3]{x, y, z};
        bool contributes = true;
        for (int k = 0; k < 3; ++k) {
          if (o.interpolation == FieldResamplingInterpolation::Linear)
            contributes &= offset[k] ? t[k] != 0 : t[k] != 1;
          else
            contributes &= offset[k] == 0    ? t[k] != 1
                           : offset[k] == -1 ? t[k] != 0 && t[k] != 1
                                             : t[k] != 0;
        }
        if (contributes && grid.tree().isValueOn(base + C(x, y, z)))
          return true;
      }
  return false;
}
struct VectorRule {
  bool apply = false, integerSafe = true;
  openvdb::VecType tag = openvdb::VEC_INVARIANT;
  std::array<V, 3> rows{};
  V translation;
  VectorRule(const openvdb::GridBase &grid, const Map &deformation,
             const FieldResamplingOptions &o)
      : tag(grid.getVectorType()), translation(0) {
    apply = o.worldFromWorld != Identity &&
            o.vectorValues == FieldVectorTransform::Semantic &&
            tag != openvdb::VEC_INVARIANT;
    const bool covariant = tag == openvdb::VEC_COVARIANT ||
                           tag == openvdb::VEC_COVARIANT_NORMALIZE;
    for (int row = 0; row < 3; ++row)
      for (int c = 0; c < 3; ++c)
        rows[row][c] =
            covariant ? deformation.inverse[c][row] : deformation.axes[c][row];
    if (tag == openvdb::VEC_CONTRAVARIANT_ABSOLUTE)
      translation = deformation.origin;
    // Exact Vec3I path: monomial power-of-two maps have exact inverse scales.
    int used = 0;
    for (int row = 0; row < 3; ++row) {
      int count = 0, which = 0;
      for (int c = 0; c < 3; ++c)
        if (deformation.axes[c][row] != 0) {
          ++count;
          which = c;
          int exponent = 0;
          if (std::frexp(std::abs(deformation.axes[c][row]), &exponent) != .5)
            integerSafe = false;
        }
      if (count != 1 || (used & (1 << which)))
        integerSafe = false;
      used |= 1 << which;
    }
  }
  V floating(const V &value) const {
    if (!apply)
      return value;
    V result;
    for (int k = 0; k < 3; ++k) {
      Sum sum;
      sum.add(translation[k]);
      for (int c = 0; c < 3; ++c)
        sum.product(rows[k][c], value[c]);
      result[k] = sum.get();
      const double scale = std::max({1., norm(value), norm(result)});
      if (sum.error() > scale * 1e-12)
        throw std::invalid_argument(
            "field resampling vector transformation loses component precision");
    }
    if (tag == openvdb::VEC_COVARIANT_NORMALIZE) {
      const double scale = norm(result);
      if (scale != 0) {
        const V normalized = result / scale;
        const double length =
            std::hypot(normalized[0], normalized[1], normalized[2]);
        for (int k = 0; k < 3; ++k) {
          if (result[k] != 0 && normalized[k] == 0)
            throw std::invalid_argument(
                "field resampling normal transformation loses nonzero "
                "component");
          result[k] = normalized[k] / length;
        }
      }
    }
    return result;
  }
  openvdb::Vec3i integer(const openvdb::Vec3i &value) const {
    if (!apply)
      return value;
    if (!integerSafe)
      throw std::invalid_argument(
          "Vec3I semantic deformation requires signed axis permutations with "
          "power-of-two scales; preserve raw components explicitly if "
          "intended");
    openvdb::Vec3i result(0);
    int nonzero = 0;
    for (int k = 0; k < 3; ++k) {
      double product = 0;
      for (int c = 0; c < 3; ++c)
        if (rows[k][c] != 0)
          product = rows[k][c] * double(value[c]);
      if (product != 0)
        ++nonzero;
      if (tag == openvdb::VEC_COVARIANT_NORMALIZE) {
        result[k] = product > 0 ? 1 : product < 0 ? -1 : 0;
        continue;
      }
      const double sum = product + translation[k],
                   virtualTranslation = sum - product;
      const double residual = (product - (sum - virtualTranslation)) +
                              (translation[k] - virtualTranslation);
      if (!std::isfinite(sum) || sum < INT32_MIN || sum > INT32_MAX ||
          std::trunc(sum) != sum || residual != 0)
        throw std::invalid_argument("Vec3I semantic deformation is not an "
                                    "exact dyadic integral Int32 result");
      result[k] = int32_t(sum);
    }
    if (tag == openvdb::VEC_COVARIANT_NORMALIZE && nonzero > 1)
      throw std::invalid_argument(
          "normalized Vec3I semantic deformation requires an exact zero/axis "
          "vector; rounded-to-axis values reject");
    return result;
  }
};
template <class T> T narrow(double value, FieldResamplingResult &report) {
  if (!std::isfinite(value))
    throw std::invalid_argument("field resampling sampled nonfinite value");
  const T stored = T(value);
  if (!std::isfinite(double(stored)) || (value != 0 && stored == 0))
    throw std::invalid_argument("field resampling native storage overflows or "
                                "underflows; no implicit promotion");
  if constexpr (std::is_same_v<T, float>)
    if (double(stored) != value) {
      ++report.roundedFloatComponents;
      report.maximumFloatAbsoluteError = std::max(
          report.maximumFloatAbsoluteError, std::abs(double(stored) - value));
    }
  return stored;
}
template <class T> T converted(const V &value, FieldResamplingResult &report) {
  if constexpr (std::is_arithmetic_v<T>)
    return narrow<T>(value[0], report);
  else {
    T out;
    for (int k = 0; k < 3; ++k)
      out[k] = narrow<typename T::ValueType>(value[k], report);
    return out;
  }
}
template <class Grid> struct ExactWrite : Grid {
  explicit ExactWrite(const Grid &source)
      : Grid(openvdb::ConstPtrCast<typename Grid::TreeType>(
            source.constTreePtr())) {
    this->insertMeta(source);
    this->setTransform(detail::CopyTransformExact(source.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, detail::CopyTransformExact(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactWrite &>(*this).copyGrid());
  }
};
void publish(const std::filesystem::path &source,
             const std::filesystem::path &output) {
  bool owned = false;
  std::ofstream target;
  try {
    std::ifstream input(source, std::ios::binary);
    if (!input)
      throw std::runtime_error("cannot read completed field resampling output");
    target.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    owned = target.is_open();
    if (!owned)
      throw std::runtime_error(
          "cannot exclusively create field resampling output");
    input.exceptions(std::ios::badbit);
    target.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
      target.write(buffer.data(), input.gcount());
    if (!input.eof())
      throw std::runtime_error("field resampling completed output read failed");
    target.close();
  } catch (...) {
    target.exceptions(std::ios::goodbit);
    if (target.is_open())
      target.close();
    if (owned) {
      std::error_code e;
      std::filesystem::remove(output, e);
    }
    throw;
  }
}
template <class Grid>
FieldResamplingResult
resample(Source &source, const Grid &input, const std::filesystem::path &output,
         const FieldResamplingOptions &o, const ProcessingOptions &p) {
  using T = typename Grid::ValueType;
  constexpr bool vector = !std::is_arithmetic_v<T>,
                 integerVector = std::is_same_v<Grid, openvdb::Vec3IGrid>;
  constexpr bool real = std::is_floating_point_v<T> ||
                        std::is_same_v<Grid, openvdb::Vec3SGrid> ||
                        std::is_same_v<Grid, openvdb::Vec3DGrid>;
  if (!real && o.interpolation != FieldResamplingInterpolation::Nearest)
    throw std::invalid_argument("integer/Bool/Mask/Vec3I field resampling "
                                "requires nearest interpolation");
  const bool fog = input.getGridClass() == openvdb::GRID_FOG_VOLUME;
  const auto target = o.targetIndexToWorld
                          ? nativeMap(*o.targetIndexToWorld)
                          : detail::CopyTransformExact(input.transform());
  const Map src(input.transform()), dst(*target), deformation(o.worldFromWorld);
  const VectorRule rule(input, deformation, o);
  if constexpr (integerVector)
    if (rule.apply && !rule.integerSafe)
      throw std::invalid_argument("Vec3I semantic deformation requires exact "
                                  "signed-axis power-of-two maps");
  FieldResamplingResult report;
  report.valueType = input.valueType();
  report.inputActiveVoxelCount = input.activeVoxelCount();
  report.inputStoredLeafCount = input.tree().leafCount();
  report.exactCopy =
      o.worldFromWorld == Identity && same(input.transform(), *target);
  finiteValue(input.background(), fog);
  Box support;
  long double activeCount = 0;
  for (auto i = input.cbeginValueAll(); i; ++i) {
    finiteValue(*i, fog);
    if (i.isValueOn()) {
      activeCount += i.getVoxelCount();
      if (activeCount > p.maxVoxelCount)
        throw std::invalid_argument(
            "field resampling logical source activity exceeds voxel cap");
    }
    if (i.isValueOn() || !sameBits(*i, input.background())) {
      Box b;
      i.getBoundingBox(b);
      support.expand(b);
    }
  }
  report.sourceSupportBounds = bounds(support);
  std::optional<Mapping> pull;
  Box domain = support;
  if (!report.exactCopy) {
    pull.emplace(src, dst, deformation, true);
    const Mapping forward(src, dst, deformation, false);
    domain = targetBox(
        support, forward,
        o.interpolation == FieldResamplingInterpolation::Quadratic ? 2
        : o.interpolation == FieldResamplingInterpolation::Linear  ? 1
                                                                   : 0);
    preflightPull(*pull, domain);
  }
  report.targetBounds = bounds(domain);
  const long double samples = report.exactCopy ? 0 : cells(domain),
                    outputSlots = report.exactCopy
                                      ? input.tree().leafCount() * 512.0L
                                      : blocks(domain, 8) * 512;
  if (input.tree().leafCount() * 512.0L + outputSlots > p.maxVoxelCount ||
      samples + report.inputActiveVoxelCount > p.maxVoxelCount)
    throw std::invalid_argument("field resampling joint source/output domain "
                                "and leaf slots exceed voxel cap");
  long double perSample = vector ? 1024 : 512;
  if constexpr (integerVector)
    perSample += 512;
  const long double nativeWork = source.validationWork + samples * perSample +
                                 outputSlots * 16 + source.metadataBytes * 8 +
                                 Fixed;
  if (nativeWork > o.maxWorkItems)
    throw std::invalid_argument(
        "field resampling aggregate source/sample/output work exceeds cap");
  report.workUpperBound = uint64_t(std::ceil(nativeWork));
  const long double outputBytes =
      report.exactCopy ? input.memUsage() : treeBytes<Grid>(domain);
  const long double reserve =
      source.resident + 12 * outputBytes + 16 * source.metadataBytes + Fixed;
  budget(reserve, p,
         "field resampling joint source/output/tree/writer workspace exceeds "
         "byte cap");
  if (source.metadataBytes + metadata(input) + metadata(*source.fileMetadata) +
          4096 + 16.0L * p.gridName.size() >
      o.maxMetadataBytes)
    throw std::invalid_argument("field resampling predicted joint output "
                                "metadata exceeds cap before tree allocation");
  T background = input.background();
  if constexpr (integerVector)
    background = rule.integer(background);
  else if constexpr (vector)
    background = converted<T>(rule.floating(V(background)), report);
  typename Grid::Ptr result;
  if (report.exactCopy)
    result = detail::DeepCopyGridExact(input);
  else {
    result = Grid::create(background);
    result->insertMeta(input);
    result->setTransform(target);
    if (!domain.empty())
      for (int64_t z = domain.min()[2]; z <= domain.max()[2]; ++z)
        for (int64_t y = domain.min()[1]; y <= domain.max()[1]; ++y)
          for (int64_t x = domain.min()[0]; x <= domain.max()[0]; ++x) {
            const C c{int32_t(x), int32_t(y), int32_t(z)};
            const V q = pull->at(c.asVec3d());
            T value;
            if (o.interpolation == FieldResamplingInterpolation::Nearest) {
              value = input.tree().getValue(nearest(q));
              if constexpr (integerVector)
                value = rule.integer(value);
              else if constexpr (vector)
                value = converted<T>(rule.floating(V(value)), report);
            } else {
              V sampled = interpolate(input, q, o.interpolation);
              if constexpr (vector)
                sampled = rule.floating(sampled);
              value = converted<T>(sampled, report);
            }
            finiteValue(value, fog);
            const bool on = active(input, q, o);
            auto *leaf = result->tree().touchLeaf(c);
            if constexpr (std::is_same_v<Grid, openvdb::MaskGrid>) {
              leaf->setActiveState(c, on);
            } else if (on)
              leaf->setValueOn(c, value);
            else
              leaf->setValueOff(c, value);
            ++report.sampledVoxelCount;
            if (!on)
              ++report.inactiveSampleCount;
          }
  }
  result->setName(p.gridName);
  if(!report.exactCopy)detail::RemoveBoundaryProjectionMetadata(*result);
  result->setTransform(target);
  result->setSaveFloatAsHalf(false);
  for (const char *key : {openvdb::GridBase::META_FILE_BBOX_MIN,
                          openvdb::GridBase::META_FILE_BBOX_MAX,
                          openvdb::GridBase::META_FILE_MEM_BYTES,
                          openvdb::GridBase::META_FILE_VOXEL_COUNT})
    result->removeMeta(key);
  result->removeMeta("container:field_resampling");
  result->insertMeta("container:field_resampling",
                     openvdb::StringMetadata(
                         "Single-pass conditioned native field pull sampling; "
                         "no antialias or mass-conservation guarantee"));
  report.outputStoredLeafCount = result->tree().leafCount();
  report.grid = {result->getName(),
                 openvdb::GridBase::gridClassToString(result->getGridClass()),
                 result->activeVoxelCount(),
                 std::hypot(dst.axes[0][0], dst.axes[0][1], dst.axes[0][2])};
  if ((report.inputStoredLeafCount * 1.0L + report.outputStoredLeafCount) *
              512 >
          p.maxVoxelCount ||
      report.grid.activeVoxelCount > p.maxVoxelCount)
    throw std::runtime_error(
        "field resampling output topology exceeds preflight cap");
  const auto finalMetadata = metadata(*result) + metadata(*source.fileMetadata);
  if (source.metadataBytes + finalMetadata > o.maxMetadataBytes)
    throw std::invalid_argument(
        "field resampling joint source/output metadata exceeds cap");
  budget(source.resident + 8 * result->memUsage() + 16 * finalMetadata + Fixed,
         p, "field resampling final native output exceeds byte cap");
  report.diagnostics = {
      report.exactCopy
          ? "Exact identity copies native topology and all value/activity bits."
          : "Single-pass center sampling over conservative kernel-padded "
            "finite support; background and inactive values participate.",
      "Nearest half ties use greater integer; activity is independent of "
      "sampled value. No antialias, signed-distance or mass-conservation "
      "claim.",
      o.vectorValues == FieldVectorTransform::PreserveWorldComponents
          ? "Explicit raw stored WORLD vector components retained; no vector "
            "semantic deformation applied."
          : "Nonidentity WORLD deformation applies native vector semantics, "
            "including transformed infinite background; identity retains raw "
            "components.",
      "Logical source/output/work/writer estimates are not a hostile-file heap "
      "or process-RSS sandbox."};
  const auto temporary = source.snapshot->directory / "resampled.vdb";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove(path, e);
    }
  } cleanup{temporary};
  openvdb::io::File writer(temporary.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(openvdb::GridPtrVec{std::make_shared<ExactWrite<Grid>>(*result)},
               *source.fileMetadata);
  writer.close();
  budget(reserve + std::filesystem::file_size(temporary), p,
         "field resampling encoded output and live source/writer exceed shared "
         "byte cap");
  static_assert(std::is_nothrow_move_constructible_v<FieldResamplingResult>);
  publish(temporary, output);
  return report;
}
#endif
} // namespace
FieldResamplingResult WriteResampledField(const std::filesystem::path &input,
                                          const std::filesystem::path &output,
                                          const FieldResamplingOptions &o,
                                          const std::string &selector,
                                          const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if (!p.maxInputBytes || !p.maxVoxelCount || p.gridName.empty() ||
      p.gridName.size() > 256 || p.gridName.find('\0') != std::string::npos ||
      !o.maxWorkItems || o.maxWorkItems > 10'000'000'000ULL || !o.maxGrids ||
      o.maxGrids > 4096 || !o.maxMetadataBytes ||
      o.interpolation < FieldResamplingInterpolation::Nearest ||
      o.interpolation > FieldResamplingInterpolation::Quadratic ||
      o.activity < FieldResamplingActivity::NearestSource ||
      o.activity > FieldResamplingActivity::AnyContributingStencil ||
      o.vectorValues < FieldVectorTransform::Semantic ||
      o.vectorValues > FieldVectorTransform::PreserveWorldComponents)
    throw std::invalid_argument(
        "invalid field resampling options/name/budgets");
  matrix(o.worldFromWorld);
  const Map deformation(o.worldFromWorld);
  (void)deformation;
  if (o.targetIndexToWorld) {
    matrix(*o.targetIndexToWorld);
    const Map target(*o.targetIndexToWorld);
    (void)target;
  }
  if (output.empty() || std::filesystem::exists(output) ||
      !std::filesystem::is_directory(output.has_parent_path()
                                         ? output.parent_path()
                                         : std::filesystem::path(".")))
    throw std::invalid_argument(
        "field resampling output must be new in an existing directory");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    auto source = load(input, selector, o, p);
    return dispatch(*source.grid, [&](const auto &grid) {
      return resample(source, grid, output, o, p);
    });
  });
#else
  (void)input;
  (void)output;
  (void)o;
  (void)selector;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
