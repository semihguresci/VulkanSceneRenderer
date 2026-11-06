#include "Container/geometry/OpenVdbSurfaceFields.h"
#include "OpenVdbProvenance.h"
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
#include <openvdb/tools/GridOperators.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#include <type_traits>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB surface fields are unavailable; configure "
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
      throw std::invalid_argument("null surface fields metadata");
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
          "surface fields operation exceeds maxWorkItems");
    used += uint64_t(std::ceil(n));
  }
};
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument(
          "surface fields input must be a regular file");
    encoded = std::filesystem::file_size(input);
    bytes(Fixed + encoded, p,
          "encoded surface fields source exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned n = 0; n < 64; ++n) {
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-surface-field-" +
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
      throw std::runtime_error("cannot reserve surface fields snapshot");
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open surface fields snapshot");
      std::array<char, 65536> buffer{};
      auto left = encoded;
      while (left) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(left, buffer.size()));
        source.read(buffer.data(), count);
        if (source.gcount() != count)
          throw std::runtime_error("surface fields source changed length");
        target.write(buffer.data(), count);
        left -= count;
      }
      target.close();
      if (!target || source.bad() ||
          source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "surface fields snapshot failed or source changed length");
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
#undef T
  throw std::invalid_argument(
      "surface fields supports Float/Double LEVEL_SET collider grids");
}
bool signedType(const openvdb::GridBase &g) {
  return (g.isType<openvdb::FloatGrid>() || g.isType<openvdb::DoubleGrid>()) &&
         g.getGridClass() == openvdb::GRID_LEVEL_SET;
}
void structuralSemantics(const openvdb::GridBase &g) {
  if (const auto value = g[openvdb::GridBase::META_VECTOR_TYPE]) {
    const auto text =
        std::dynamic_pointer_cast<const openvdb::StringMetadata>(value);
    if (!text)
      throw std::invalid_argument("surface-field vector semantic metadata must "
                                  "have native string type");
    auto semantic = text->value();
    openvdb::string::trim(semantic);
    openvdb::string::to_lower(semantic);
    if (semantic != openvdb::GridBase::vecTypeToString(openvdb::VEC_INVARIANT))
      throw std::invalid_argument(
          "surface fields require recognized invariant scalar semantics");
  }
  if (const auto value = g[openvdb::GridBase::META_IS_LOCAL_SPACE];
      value && !std::dynamic_pointer_cast<const openvdb::BoolMetadata>(value))
    throw std::invalid_argument("surface-field world/local semantic metadata "
                                "must have native Bool type");
  if (!g.isInWorldSpace())
    throw std::invalid_argument("surface fields require world-space scalars");
}
struct Loaded {
  openvdb::GridBase::Ptr grid;
  openvdb::MetaMap::Ptr fileMeta;
  long double resident = 0, meta = 0;
};
class MetadataArchive : public openvdb::io::Archive {
public:
  void begin(std::istream &stream, openvdb::io::StreamMetadata::Ptr &metadata) {
    readHeader(stream);
    if (!inputHasGridOffsets() ||
        fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
        fileVersion() > openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument("surface fields require supported seekable "
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
      throw std::invalid_argument("surface fields grid descriptor offsets are "
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
          "surface fields metadata/transform crosses grid payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};
Loaded load(const Snapshot &s, const SurfaceFieldsOptions &o,
            const std::string &selector, const ProcessingOptions &p) {
  detail::RequireSeekableFile(s.file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument(
        "bounded surface fields require delayed loading; "
        "unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded surface fields require OpenVDB delayed-loading support");
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
  long double catalog = s.encoded + Fixed;
  auto check = [&] {
    if (r.meta > o.maxMetadataBytes)
      throw std::invalid_argument("aggregate surface fields file/grid metadata "
                                  "exceeds maxMetadataBytes");
    bytes(catalog + 8 * r.meta, p,
          "surface fields catalog exceeds memory budget");
  };
  check();
  std::set<std::string> descriptors;
  const int32_t gridCount = archive.count(stream);
  if (gridCount < 0 || uint32_t(gridCount) > o.maxGrids)
    throw std::invalid_argument("surface fields catalog exceeds maxGrids");
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
          "surface fields input has colliding descriptors; rename grids first");
    r.meta += metadata(*m);
    check();
    if (!GridSelectionMatches(selector, name, m->getName()))
      continue;
    const bool usable = signedType(*m);
    if (!usable) {
      if (!selector.empty())
        throw std::invalid_argument(
            "surface fields selected unsupported native type/class");
      continue;
    }
    structuralSemantics(*m);
    if (found)
      throw std::invalid_argument("ambiguous surface fields selector; use "
                                  "descriptor:<exact descriptor>");
    const auto mem = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES),
               count = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!mem || !count || mem->value() < 0 || count->value() < 0 ||
        uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument(
          "surface fields needs valid recorded memory/voxel "
          "statistics within budgets");
    if (uint64_t(count->value()) > o.maxWorkItems)
      throw std::invalid_argument(
          "recorded surface fields source exceeds maxWorkItems before loading");
    bytes(catalog + 8 * r.meta + mem->value(), p,
          "recorded surface fields tree exceeds memory budget");
    found = true;
    chosen = name;
  }
  if (!found)
    throw std::invalid_argument("no matching supported surface fields grid");
  stream.close();
  openvdb::io::File file(s.file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error(
        "surface fields snapshot changed before decoded loading");
  r.grid = file.readGrid(chosen);
  if (r.grid->activeVoxelCount() > p.maxVoxelCount ||
      r.grid->baseTree().leafCount() >
          p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument(
        "surface fields source exceeds native active/leaf caps");
  if (r.grid->baseTree().leafCount() > o.maxWorkItems / 1024)
    throw std::invalid_argument("surface fields source leaves exceed "
                                "maxWorkItems before materialization");
  const auto full = visit(*r.grid, [](const auto &g) {
    return openvdb::tools::memUsageIfLoaded(g.tree(), false);
  });
  r.resident = catalog + 8 * r.meta + full;
  if (static_cast<long double>(full) * 16 > o.maxWorkItems)
    throw std::invalid_argument("surface fields decoded source exceeds "
                                "maxWorkItems before materialization");
  bytes(r.resident, p,
        "fully decoded surface fields source exceeds memory budget before "
        "materialization");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full)
    throw std::runtime_error(
        "surface fields tree exceeded decoded reservation");
  file.close();
  return r;
#endif
}
// Archive::writeGrid also calls Grid::copyGrid, which normally simplifies its
// map. Only its metadata copy is required: retain the original immutable map
// and shared tree through the SDK's exact replacement constructor instead.
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

using V = openvdb::Vec3d;
using Scalar = openvdb::DoubleGrid;
using Vector = openvdb::Vec3DGrid;
double scaled(double value, std::initializer_list<double> numerators,
              std::initializer_list<double> denominators = {}) {
  if (value == 0)
    return value;
  int exponent = 0;
  double mantissa = std::frexp(value, &exponent);
  for (const double factor : numerators) {
    if (factor == 0)
      return std::copysign(0., value * factor);
    int e = 0;
    mantissa *= std::frexp(factor, &e);
    exponent += e;
    int adjust = 0;
    mantissa = std::frexp(mantissa, &adjust);
    exponent += adjust;
  }
  for (const double factor : denominators) {
    if (factor == 0 || !std::isfinite(factor))
      throw std::invalid_argument("invalid surface-field scaling divisor");
    int e = 0;
    mantissa /= std::frexp(factor, &e);
    exponent -= e;
    int adjust = 0;
    mantissa = std::frexp(mantissa, &adjust);
    exponent += adjust;
  }
  const double out = std::ldexp(mantissa, exponent);
  if (!std::isfinite(out) || out == 0)
    throw std::invalid_argument("surface-field conditioning/restoration "
                                "overflows or loses nonzero Double range");
  return out;
}
struct Sum {
  double value = 0, error = 0;
  void add(double x) {
    const double next = value + x;
    error += std::abs(value) >= std::abs(x) ? (value - next) + x
                                            : (x - next) + value;
    value = next;
  }
  double get() const {
    const double out = value + error;
    if (!std::isfinite(out))
      throw std::invalid_argument("nonfinite surface-field sum");
    return out;
  }
};

int64_t block(int64_t x, int64_t d) { return x >= 0 ? x / d : (x + 1) / d - 1; }
long double blocks(const openvdb::CoordBBox &b, int64_t d) {
  long double n = 1;
  for (int a = 0; a < 3; ++a)
    n *= block(b.max()[a], d) - block(b.min()[a], d) + 1;
  return n;
}
template <class G> long double treeBytes(const openvdb::CoordBBox &b) {
  using T = typename G::TreeType;
  using L = typename T::LeafNodeType;
  using U = typename T::RootNodeType::ChildNodeType;
  using D = typename U::ChildNodeType;
  return blocks(b, 8) *
             (sizeof(L) + 512 * sizeof(typename G::ValueType) + 256.0L) +
         blocks(b, D::DIM) * (sizeof(D) + 256.0L) +
         blocks(b, U::DIM) * (sizeof(U) + 512.0L);
}

template <class G>
typename G::Ptr restoreIndexCoordinates(const G &source,
                                        const openvdb::Coord &offset) {
  auto result = G::create(source.background());
  auto a = result->getAccessor();
  const auto global = [&](const openvdb::Coord &local) {
    openvdb::Coord out;
    for (int axis = 0; axis < 3; ++axis) {
      const int64_t x = int64_t(local[axis]) + offset[axis];
      if (x < INT32_MIN + 4096LL || x > INT32_MAX - 4096LL)
        throw std::invalid_argument(
            "surface-field restored output index exceeds native node margins");
      out[axis] = int32_t(x);
    }
    return out;
  };
  for (auto v = source.cbeginValueOn(); v; ++v) {
    if (v.isVoxelValue())
      a.setValue(global(v.getCoord()), *v);
    else {
      const auto box = v.getBoundingBox();
      result->tree().fill(
          openvdb::CoordBBox(global(box.min()), global(box.max())), *v, true);
    }
  }
  return result;
}

bool scalarOperation(SurfaceFieldOperation operation) {
  return operation == SurfaceFieldOperation::MeanCurvature ||
         operation == SurfaceFieldOperation::GaussianCurvature;
}
void validate(SurfaceFieldOperation operation, const SurfaceFieldsOptions &o,
              const ProcessingOptions &p) {
  if (unsigned(operation) > unsigned(SurfaceFieldOperation::ClosestPoint) ||
      !p.maxInputBytes || !p.maxVoxelCount || !o.maxWorkItems ||
      o.maxWorkItems > 10'000'000'000ull || !o.maxGrids || o.maxGrids > 4096 ||
      !o.maxMetadataBytes)
    throw std::invalid_argument("invalid surface-field operation or limits");
  for (const auto *name : {&p.gridName, &o.validityGridName})
    if (name->empty() || name->size() > 256 ||
        name->find('\0') != std::string::npos)
      throw std::invalid_argument("surface-field names must be 1..256 bytes");
  if (p.gridName == o.validityGridName)
    throw std::invalid_argument("surface-field and validity names must differ");
}
struct Map {
  V origin;
  std::array<V, 3> basis, rotation;
  double h = 0;
  explicit Map(const openvdb::GridBase &g) {
    if (!g.transform().isLinear())
      throw std::invalid_argument(
          "surface fields require uniform orthogonal affine voxels");
    const auto m = g.transform().baseMap();
    origin = m->applyMap(V(0));
    for (int a = 0; a < 3; ++a) {
      V e(0);
      e[a] = 1;
      basis[a] = m->applyJacobian(e);
      const double length = std::hypot(basis[a][0], basis[a][1], basis[a][2]);
      if (!std::isfinite(origin[a]) || !std::isfinite(length) ||
          length < 1e-6 || length > 1e6)
        throw std::invalid_argument(
            "surface-field affine axes must be finite in [1e-6,1e6]");
      if (a == 0)
        h = length;
      else if (std::abs(length / h - 1) > 1e-12)
        throw std::invalid_argument("surface fields require uniform voxels");
      for (int row = 0; row < 3; ++row)
        rotation[a][row] = scaled(basis[a][row], {}, {h});
      for (int j = 0; j < a; ++j)
        if (std::abs(rotation[a].dot(rotation[j])) > 1e-12)
          throw std::invalid_argument("surface fields require orthogonal axes");
    }
  }
  V normal(const V &unit) const {
    V result(0);
    for (int row = 0; row < 3; ++row) {
      Sum sum;
      for (int col = 0; col < 3; ++col)
        sum.add(scaled(unit[col], {rotation[col][row]}));
      result[row] = sum.get();
    }
    return result;
  }
  V position(const V &local, const openvdb::Coord &offset) const {
    V result;
    for (int row = 0; row < 3; ++row) {
      Sum sum;
      sum.add(origin[row]);
      for (int col = 0; col < 3; ++col)
        for (const double x : {double(offset[col]), local[col]}) {
          const double product = basis[col][row] * x;
          if (!std::isfinite(product))
            throw std::invalid_argument("surface-field CPT position overflows");
          sum.add(product);
          sum.add(std::fma(basis[col][row], x, -product));
        }
      result[row] = sum.get();
    }
    return result;
  }
};
struct Bounds {
  openvdb::Coord offset;
  openvdb::CoordBBox local, global;
  long double reserved = 0;
};
template <class G>
Bounds bounds(const G &g, const Map &m, const Loaded &loaded,
              const ProcessingOptions &p, Work &work) {
  work.add(g.memUsage() * 16.0L);
  const double bg = g.background(), band = scaled(bg, {}, {m.h});
  if (!std::isfinite(bg) || bg <= 0 || band < 3 - 1e-6 || band > 64 + 1e-6 ||
      g.tree().hasActiveTiles() || !g.isInWorldSpace() ||
      g.getVectorType() != openvdb::VEC_INVARIANT)
    throw std::invalid_argument("surface fields require world/invariant voxel "
                                "LEVEL_SET with symmetric band [3,64]");
  auto box = g.evalActiveVoxelBoundingBox();
  bool hiddenNegative = false;
  for (auto value = g.cbeginValueAll(); value; ++value) {
    const double x = *value;
    if (!std::isfinite(x) || std::abs(x) > bg * (1 + 1e-6) ||
        (!value.isValueOn() && std::abs(x) != bg))
      throw std::invalid_argument("surface fields require finite band values "
                                  "and signed inactive backgrounds");
    if (x < 0)
      hiddenNegative = true;
    if (x != bg)
      box.expand(value.getBoundingBox());
  }
  Bounds result;
  if (g.activeVoxelCount() == 0) {
    if (hiddenNegative)
      throw std::invalid_argument(
          "inactive negative topology without an active "
          "surface is not an empty source");
    result.reserved = loaded.resident * 8 + loaded.meta * 24 + Fixed;
    bytes(result.reserved, p, "empty surface-field workspace exceeds byte cap");
    return result;
  }
  for (int a = 0; a < 3; ++a) {
    const int64_t lo = box.min()[a], hi = box.max()[a];
    if (lo < INT32_MIN + 8192LL || hi > INT32_MAX - 8192LL)
      throw std::invalid_argument(
          "surface-field coordinates lack native halos");
    const int64_t center = lo + (hi - lo) / 2;
    result.offset[a] = int32_t(block(center, 8) * 8);
  }
  result.global = box;
  result.global.expand(2);
  result.local = result.global;
  result.local.translate(-result.offset);
  const long double slots = blocks(result.local, 8) * 512,
                    outputSlots = blocks(result.global, 8) * 512;
  if (slots > p.maxVoxelCount || 2 * outputSlots > p.maxVoxelCount)
    throw std::invalid_argument("surface-field full prospective domain/output "
                                "stored leaves exceed voxel cap");
  work.add(slots * 32 + g.activeVoxelCount() * 2048.0L);
  result.reserved = loaded.resident * 8 + loaded.meta * 24 + Fixed +
                    8 * treeBytes<Scalar>(result.local) +
                    8 * treeBytes<Vector>(result.local) +
                    6 * treeBytes<Mask>(result.local) +
                    4 * treeBytes<Vector>(result.global) +
                    4 * treeBytes<Mask>(result.global) + slots * 128;
  bytes(result.reserved, p,
        "surface-field source/native/validity/output/writer workspace exceeds "
        "byte cap");
  return result;
}
template <class G>
Scalar::Ptr normalize(const G &g, const Map &m, const Bounds &b) {
  auto result = Scalar::create(scaled(double(g.background()), {}, {m.h}));
  result->setGridClass(openvdb::GRID_LEVEL_SET);
  auto out = result->getAccessor();
  for (auto it = g.cbeginValueAll(); it; ++it) {
    if (!it.isValueOn() && *it == g.background())
      continue;
    const double value = scaled(double(*it), {}, {m.h});
    if (it.isVoxelValue()) {
      if (it.isValueOn())
        out.setValue(it.getCoord() - b.offset, value);
      else
        out.setValueOff(it.getCoord() - b.offset, value);
    } else {
      auto region = it.getBoundingBox();
      region.translate(-b.offset);
      result->tree().fill(region, value, it.isValueOn());
    }
  }
  if (g.activeVoxelCount() != 0) {
    bool crossing = false;
    for (auto it = result->cbeginValueOn(); it && !crossing; ++it)
      for (int a = 0; a < 3; ++a)
        for (int sign : {-1, 1}) {
          auto next = it.getCoord();
          next[a] += sign;
          const double x = *it, y = out.getValue(next);
          crossing |= (x < 0 && y >= 0) || (x >= 0 && y < 0);
        }
    if (!crossing)
      throw std::invalid_argument("nonempty surface-field source lacks a "
                                  "represented signed crossing");
  }
  return result;
}
Mask::Ptr validity(const Scalar &unit, SurfaceFieldOperation operation,
                   SurfaceFieldsResult &result) {
  auto mask = Mask::create(false);
  auto valid = mask->getAccessor();
  const auto source = unit.getConstAccessor();
  const openvdb::math::UniformScaleMap identity(1.0);
  const bool curvature = scalarOperation(operation);
  for (auto it = unit.cbeginValueOn(); it; ++it) {
    const auto c = it.getCoord();
    bool complete = true;
    for (int x = -1; x <= 1 && complete; ++x)
      for (int y = -1; y <= 1 && complete; ++y)
        for (int z = -1; z <= 1; ++z) {
          const int distance = std::abs(x) + std::abs(y) + std::abs(z);
          if (distance > (curvature ? 2 : 1))
            continue;
          if (!source.isValueOn(c + openvdb::Coord(x, y, z))) {
            complete = false;
            break;
          }
        }
    if (!complete) {
      ++result.missingFootprintVoxelCount;
      continue;
    }
    const V gradient =
        openvdb::math::Gradient<openvdb::math::UniformScaleMap,
                                openvdb::math::CD_2ND>::result(identity, source,
                                                               c);
    const double length = std::hypot(gradient[0], gradient[1], gradient[2]);
    if (!std::isfinite(length))
      throw std::invalid_argument("nonfinite surface-field unit gradient");
    ++result.gradientSampleCount;
    result.maximumUnitGradientError =
        std::max(result.maximumUnitGradientError, std::abs(length - 1));
    if (length <= 1e-7) {
      ++result.degenerateGradientVoxelCount;
      continue;
    }
    valid.setValue(c, true);
    ++result.validVoxelCount;
  }
  return mask;
}
void structure(openvdb::GridBase &g, const openvdb::GridBase &source,
               const std::string &name, openvdb::VecType frame,
               const char *units) {
  g.insertMeta(source);
  detail::RemoveBoundaryProjectionMetadata(g);
  g.setTransform(detail::CopyTransformExact(source.transform()));
  for (auto key :
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
  for (auto key :
       {"container:mac_layout", "container:velocity_units",
        "container:pressure_units", "container:pressure_gauge",
        "container:projection_dt", "container:point_count",
        "container:point_empty_manifest_v1",
        "container:point_empty_descriptor_v1",
        "container:point_empty_leaf_flags_v1", "container:potential_flow_units",
        "container:potential_flow_boundary", "container:surface_field_units",
        "container:surface_field_validity", "container:surface_field_location"})
    g.removeMeta(key);
  g.setName(name);
  g.setGridClass(openvdb::GRID_UNKNOWN);
  g.setVectorType(frame);
  g.setIsInWorldSpace(true);
  g.setSaveFloatAsHalf(false);
  g.insertMeta("container:surface_field_units", openvdb::StringMetadata(units));
  g.insertMeta(
      "container:surface_field_validity",
      openvdb::StringMetadata("full active stencil; unit gradient >1e-7; "
                              "not a signed-distance certificate"));
  g.insertMeta("container:surface_field_location",
               openvdb::StringMetadata("original source cell center"));
}
openvdb::GridBase::Ptr writerView(const openvdb::GridBase::Ptr &g) {
#define VIEW(G)                                                                \
  if (g->isType<openvdb::G>())                                                 \
  return std::make_shared<ExactWriteGrid<openvdb::G>>(                         \
      *openvdb::gridPtrCast<openvdb::G>(g))
  VIEW(FloatGrid);
  VIEW(DoubleGrid);
  VIEW(Vec3SGrid);
  VIEW(Vec3DGrid);
  VIEW(BoolGrid);
#undef VIEW
  throw std::logic_error("unexpected surface-field output type");
}
void publish(const Snapshot &s, const std::filesystem::path &output,
             const openvdb::GridPtrVec &grids, const Loaded &loaded,
             const Bounds &b, const ProcessingOptions &p) {
  const auto native = s.directory / "result.vdb";
  if (std::filesystem::exists(native))
    throw std::logic_error("surface-field private output already exists");
  openvdb::GridPtrVec views;
  for (const auto &grid : grids)
    views.push_back(writerView(grid));
  openvdb::io::File writer(native.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(views, *loaded.fileMeta);
  writer.close();
  bytes(b.reserved + std::filesystem::file_size(native), p,
        "surface-field encoded output exceeds combined byte cap");
  bool created = false;
  std::ofstream target;
  try {
    std::ifstream source(native, std::ios::binary);
    if (!source)
      throw std::runtime_error("cannot read surface-field private output");
    target.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    created = target.is_open();
    if (!created)
      throw std::runtime_error(
          "cannot exclusively create surface-field output");
    target.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 65536> buffer{};
    while (source.read(buffer.data(), buffer.size()) || source.gcount())
      target.write(buffer.data(), source.gcount());
    if (!source.eof())
      throw std::runtime_error("surface-field output copy failed");
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
template <class T> T stored(double value) {
  const T out = static_cast<T>(value);
  if (!std::isfinite(value) || !std::isfinite(out) || (value != 0 && out == 0))
    throw std::invalid_argument(
        "surface-field output overflows or loses nonzero "
        "range; enable promoteToDouble when using Float");
  return out;
}
template <class T> T positionComponent(double value, double h) {
  const T out = stored<T>(value);
  const double gap = std::max(
      std::abs(double(std::nextafter(out, std::numeric_limits<T>::infinity())) -
               double(out)),
      std::abs(double(out) - double(std::nextafter(
                                 out, -std::numeric_limits<T>::infinity()))));
  if (!std::isfinite(gap) || gap > h * 1e-4 ||
      std::abs(double(out) - value) > h * 1e-4)
    throw std::invalid_argument("absolute surface-field CPT positions lack "
                                "1e-4 voxel precision; promote Float output "
                                "or recenter the source");
  return out;
}
template <class OutGrid>
typename OutGrid::Ptr convert(const openvdb::GridBase &native, const Map &map,
                              const Bounds &b,
                              SurfaceFieldOperation operation) {
  using T = typename OutGrid::ValueType;
  auto result = OutGrid::create(openvdb::zeroVal<T>());
  auto output = result->getAccessor();
  if constexpr (std::is_arithmetic_v<T>) {
    const auto &source = static_cast<const Scalar &>(native);
    for (auto it = source.cbeginValueOn(); it; ++it) {
      const double value = operation == SurfaceFieldOperation::MeanCurvature
                               ? scaled(*it, {}, {map.h})
                               : scaled(*it, {}, {map.h, map.h});
      if (it.isVoxelValue())
        output.setValue(it.getCoord(), stored<T>(value));
      else
        result->tree().fill(it.getBoundingBox(), stored<T>(value), true);
    }
  } else {
    using Component = typename T::ValueType;
    const auto &source = static_cast<const Vector &>(native);
    for (auto it = source.cbeginValueOn(); it; ++it) {
      const V value = operation == SurfaceFieldOperation::Normal
                          ? map.normal(*it)
                          : map.position(*it, b.offset);
      T out;
      for (int a = 0; a < 3; ++a)
        out[a] = operation == SurfaceFieldOperation::ClosestPoint
                     ? positionComponent<Component>(value[a], map.h)
                     : stored<Component>(value[a]);
      if (it.isVoxelValue())
        output.setValue(it.getCoord(), out);
      else
        result->tree().fill(it.getBoundingBox(), out, true);
    }
  }
  return restoreIndexCoordinates(*result, b.offset);
}
template <class G>
SurfaceFieldsResult
run(const G &source, const Loaded &loaded, const Snapshot &snapshot,
    const std::filesystem::path &output, SurfaceFieldOperation operation,
    const SurfaceFieldsOptions &o, const ProcessingOptions &p) {
  const Map map(source);
  Work work{0, o.maxWorkItems};
  const auto b = bounds(source, map, loaded, p, work);
  const auto local = normalize(source, map, b);
  SurfaceFieldsResult result;
  result.inputActiveVoxelCount = source.activeVoxelCount();
  auto mask = validity(*local, operation, result);
  openvdb::GridBase::Ptr native;
  if (operation == SurfaceFieldOperation::MeanCurvature)
    native = openvdb::tools::meanCurvature(*local, *mask, false);
  else if (operation == SurfaceFieldOperation::GaussianCurvature) {
    auto field = Scalar::create(0);
    auto target = field->getAccessor();
    openvdb::math::CurvatureStencil<Scalar> stencil(*local, 1.0);
    for (auto it = mask->cbeginValueOn(); it; ++it) {
      stencil.moveTo(it.getCoord());
      target.setValue(it.getCoord(), stencil.gaussianCurvature());
    }
    native = field;
  } else if (operation == SurfaceFieldOperation::Normal) {
    const auto gradient = openvdb::tools::gradient(*local, *mask, false);
    native = openvdb::tools::normalize(*gradient, false);
  } else
    native = openvdb::tools::cpt(*local, *mask, false);
  openvdb::GridBase::Ptr field;
  const bool useDouble =
      std::is_same_v<G, openvdb::DoubleGrid> || o.promoteToDouble;
  if (scalarOperation(operation)) {
    if (useDouble)
      field = convert<openvdb::DoubleGrid>(*native, map, b, operation);
    else
      field = convert<openvdb::FloatGrid>(*native, map, b, operation);
  } else if (useDouble)
    field = convert<openvdb::Vec3DGrid>(*native, map, b, operation);
  else
    field = convert<openvdb::Vec3SGrid>(*native, map, b, operation);
  auto publishedMask = restoreIndexCoordinates(*mask, b.offset);
  const auto frame = operation == SurfaceFieldOperation::Normal
                         ? openvdb::VEC_COVARIANT_NORMALIZE
                     : operation == SurfaceFieldOperation::ClosestPoint
                         ? openvdb::VEC_CONTRAVARIANT_ABSOLUTE
                         : openvdb::VEC_INVARIANT;
  const char *units =
      operation == SurfaceFieldOperation::MeanCurvature ? "inverse_world_length"
      : operation == SurfaceFieldOperation::GaussianCurvature
          ? "inverse_squared_world_length"
      : operation == SurfaceFieldOperation::Normal ? "unit_world_normal"
                                                   : "absolute_world_position";
  structure(*field, source, p.gridName, frame, units);
  structure(*publishedMask, source, o.validityGridName, openvdb::VEC_INVARIANT,
            "active_stencil_validity");
  const long double slots =
      (field->baseTree().leafCount() + publishedMask->tree().leafCount()) *
      512.0L;
  if (field->activeVoxelCount() != result.validVoxelCount ||
      publishedMask->activeVoxelCount() != result.validVoxelCount)
    throw std::runtime_error(
        "native surface-field topology disagrees with validity");
  if (slots > p.maxVoxelCount ||
      2.0L * result.validVoxelCount > p.maxVoxelCount)
    throw std::invalid_argument(
        "summed surface-field output topology exceeds cap");
  bytes(loaded.resident * 8 +
            4 * (field->memUsage() + publishedMask->memUsage()) +
            24 * loaded.meta + Fixed,
        p, "surface-field retained outputs/metadata/writer exceed byte cap");
  result.field = {p.gridName, "unknown", field->activeVoxelCount(), map.h};
  result.validity = {o.validityGridName, "unknown", result.validVoxelCount,
                     map.h};
  result.valueType = field->valueType();
  result.workItems = work.used;
  result.diagnostics.push_back(
      "Fields are evaluated at original source cells; validity marks only full "
      "active stencils and nondegenerate unit gradients, not SDF quality or "
      "closure.");
  if (operation == SurfaceFieldOperation::ClosestPoint)
    result.diagnostics.push_back(
        "Native local CPT is x-phi*unnormalized gradient; it is not a nearest "
        "point-cloud/triangle query, Newton iteration or exact surface "
        "projection.");
  if (source.activeVoxelCount() == 0)
    result.diagnostics.push_back(
        "Empty positive-background source yielded empty fields.");
  else if (!result.validVoxelCount)
    result.diagnostics.push_back(
        "Present source has no available nondegenerate full stencil; both "
        "outputs are empty.");
  publish(snapshot, output, {field, publishedMask}, loaded, b, p);
  return result;
}
#endif
} // namespace

SurfaceFieldsResult WriteSurfaceFields(const std::filesystem::path &input,
                                       const std::filesystem::path &output,
                                       SurfaceFieldOperation operation,
                                       const SurfaceFieldsOptions &o,
                                       const std::string &selector,
                                       const ProcessingOptions &p) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;
  (void)output;
  (void)operation;
  (void)o;
  (void)selector;
  (void)p;
  unavailable();
#else
  validate(operation, o, p);
#ifndef OPENVDB_USE_ZLIB
  throw std::invalid_argument("surface fields require OpenVDB ZIP support");
#endif
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "surface-field output must be a new path in an existing directory");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Snapshot snapshot(input, p);
    const auto loaded = load(snapshot, o, selector, p);
    if (loaded.grid->isType<openvdb::FloatGrid>())
      return run(*openvdb::gridPtrCast<openvdb::FloatGrid>(loaded.grid), loaded,
                 snapshot, output, operation, o, p);
    return run(*openvdb::gridPtrCast<openvdb::DoubleGrid>(loaded.grid), loaded,
               snapshot, output, operation, o, p);
  });
#endif
}
} // namespace container::geometry::vdb
