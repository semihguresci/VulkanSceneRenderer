#include "Container/geometry/OpenVdbPotentialFlow.h"
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
#include <map>
#include <mutex>
#include <numeric>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/PotentialFlow.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB potential flow are unavailable; configure "
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
      throw std::invalid_argument("null potential flow metadata");
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
          "potential flow operation exceeds maxWorkItems");
    used += uint64_t(std::ceil(n));
  }
};
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument(
          "potential flow input must be a regular file");
    encoded = std::filesystem::file_size(input);
    bytes(Fixed + encoded, p,
          "encoded potential flow source exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned n = 0; n < 64; ++n) {
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-potential-flow-" +
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
      throw std::runtime_error("cannot reserve potential flow snapshot");
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open potential flow snapshot");
      std::array<char, 65536> buffer{};
      auto left = encoded;
      while (left) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(left, buffer.size()));
        source.read(buffer.data(), count);
        if (source.gcount() != count)
          throw std::runtime_error("potential flow source changed length");
        target.write(buffer.data(), count);
        left -= count;
      }
      target.close();
      if (!target || source.bad() ||
          source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "potential flow snapshot failed or source changed length");
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
      "potential flow supports Float/Double LEVEL_SET collider grids");
}
bool signedType(const openvdb::GridBase &g) {
  return (g.isType<openvdb::FloatGrid>() || g.isType<openvdb::DoubleGrid>()) &&
         g.getGridClass() == openvdb::GRID_LEVEL_SET;
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
      throw std::invalid_argument("potential flow require supported seekable "
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
      throw std::invalid_argument("potential flow grid descriptor offsets are "
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
          "potential flow metadata/transform crosses grid payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};
Loaded load(const Snapshot &s, const PotentialFlowOptions &o,
            const std::string &selector, const ProcessingOptions &p) {
  detail::RequireSeekableFile(s.file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument(
        "bounded potential flow require delayed loading; "
        "unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded potential flow require OpenVDB delayed-loading support");
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
      throw std::invalid_argument("aggregate potential flow file/grid metadata "
                                  "exceeds maxMetadataBytes");
    bytes(catalog + 8 * r.meta, p,
          "potential flow catalog exceeds memory budget");
  };
  check();
  std::set<std::string> descriptors;
  const int32_t gridCount = archive.count(stream);
  if (gridCount < 0 || uint32_t(gridCount) > o.maxGrids)
    throw std::invalid_argument("potential flow catalog exceeds maxGrids");
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
          "potential flow input has colliding descriptors; rename grids first");
    r.meta += metadata(*m);
    check();
    if (!GridSelectionMatches(selector, name, m->getName()))
      continue;
    const bool usable = signedType(*m);
    if (!usable) {
      if (!selector.empty())
        throw std::invalid_argument(
            "potential flow selected unsupported native type/class");
      continue;
    }
    if (found)
      throw std::invalid_argument("ambiguous potential flow selector; use "
                                  "descriptor:<exact descriptor>");
    const auto mem = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES),
               count = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!mem || !count || mem->value() < 0 || count->value() < 0 ||
        uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument(
          "potential flow needs valid recorded memory/voxel "
          "statistics within budgets");
    if (uint64_t(count->value()) > o.maxWorkItems)
      throw std::invalid_argument(
          "recorded potential flow source exceeds maxWorkItems before loading");
    bytes(catalog + 8 * r.meta + mem->value(), p,
          "recorded potential flow tree exceeds memory budget");
    found = true;
    chosen = name;
  }
  if (!found)
    throw std::invalid_argument("no matching supported potential flow grid");
  stream.close();
  openvdb::io::File file(s.file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error(
        "potential flow snapshot changed before decoded loading");
  r.grid = file.readGrid(chosen);
  if (r.grid->activeVoxelCount() > p.maxVoxelCount ||
      r.grid->baseTree().leafCount() >
          p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument(
        "potential flow source exceeds native active/leaf caps");
  if (r.grid->baseTree().leafCount() > o.maxWorkItems / 1024)
    throw std::invalid_argument("potential flow source leaves exceed "
                                "maxWorkItems before materialization");
  const auto full = visit(*r.grid, [](const auto &g) {
    return openvdb::tools::memUsageIfLoaded(g.tree(), false);
  });
  r.resident = catalog + 8 * r.meta + full;
  if (static_cast<long double>(full) * 16 > o.maxWorkItems)
    throw std::invalid_argument("potential flow decoded source exceeds "
                                "maxWorkItems before materialization");
  bytes(r.resident, p,
        "fully decoded potential flow source exceeds memory budget before "
        "materialization");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full)
    throw std::runtime_error(
        "potential flow tree exceeded decoded reservation");
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
using Domain = openvdb::MaskGrid;

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
      throw std::invalid_argument("invalid potential-flow scaling divisor");
    int e = 0;
    mantissa /= std::frexp(factor, &e);
    exponent -= e;
    int adjust = 0;
    mantissa = std::frexp(mantissa, &adjust);
    exponent += adjust;
  }
  const double out = std::ldexp(mantissa, exponent);
  if (!std::isfinite(out) || out == 0)
    throw std::invalid_argument("potential-flow conditioning/restoration "
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
      throw std::invalid_argument("nonfinite potential-flow sum");
    return out;
  }
};
// Compare positive values without underflowing the requested precision scale.
bool exceedsProduct(double value, double a, double b) {
  int ev = 0, ea = 0, eb = 0, correction = 0;
  const double mv = std::frexp(value, &ev),
               product = std::frexp(std::frexp(a, &ea) * std::frexp(b, &eb),
                                    &correction);
  const int ep = ea + eb + correction;
  return ev > ep || (ev == ep && mv > product);
}
void validate(const PotentialFlowOptions &o, const ProcessingOptions &p) {
  if (!p.maxInputBytes || !p.maxVoxelCount || !o.exteriorWidth ||
      o.exteriorWidth > 128 || !o.maxSolverVoxels ||
      o.maxSolverVoxels > 1'000'000 || !o.maxIterations ||
      o.maxIterations > 10000 || !std::isfinite(o.tolerance) ||
      o.tolerance < 1e-12 || o.tolerance > 1e-2 || !o.maxWorkItems ||
      o.maxWorkItems > 1'000'000'000'000ull || !o.maxGrids ||
      o.maxGrids > 4096 || !o.maxMetadataBytes)
    throw std::invalid_argument(
        "invalid potential-flow width/solver/tolerance/work/catalog/byte/voxel "
        "options");
  std::set<std::string> names;
  for (const auto *name : {&o.velocityGridName, &o.potentialGridName,
                           &o.domainGridName, &o.neumannGridName}) {
    if (name->empty() || name->size() > 256 ||
        name->find('\0') != std::string::npos || !names.insert(*name).second)
      throw std::invalid_argument(
          "potential-flow output names must be distinct1..256-byte strings");
  }
  for (const auto &v : {o.farFieldVelocity, o.colliderVelocity})
    for (const double x : v)
      if (!std::isfinite(x) || std::abs(x) > 1e12)
        throw std::invalid_argument("potential-flow world velocity components "
                                    "must be finite with magnitude<=1e12");
}
struct Map {
  V origin;
  std::array<V, 3> basis, rotation;
  double h = 0;
  explicit Map(const openvdb::GridBase &g) {
    if (!g.transform().isLinear())
      throw std::invalid_argument(
          "potential flow requires uniform orthogonal affine voxels");
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
            "potential-flow affine axes must be finite in[1e-6,1e6]");
      if (a == 0)
        h = length;
      else if (std::abs(length / h - 1) > 1e-12)
        throw std::invalid_argument("potential flow requires uniform "
                                    "orthogonal voxels; resample first");
      rotation[a] = basis[a] / h;
      for (int j = 0; j < a; ++j)
        if (std::abs(rotation[a].dot(rotation[j])) > 1e-12)
          throw std::invalid_argument(
              "potential flow requires orthogonal voxel axes");
    }
  }
  V local(const std::array<double, 3> &world, double speed) const {
    V out(0);
    for (int a = 0; a < 3; ++a) {
      Sum sum;
      for (int r = 0; r < 3; ++r)
        sum.add(scaled(world[r], {rotation[a][r]}, {speed}));
      out[a] = sum.get();
    }
    return out;
  }
  V worldVector(const V &local, double speed) const {
    V out(0);
    for (int r = 0; r < 3; ++r) {
      Sum sum;
      for (int a = 0; a < 3; ++a)
        sum.add(scaled(local[a], {rotation[a][r], speed}));
      out[r] = sum.get();
    }
    return out;
  }
  V localVector(const V &world, double speed) const {
    V out(0);
    for (int a = 0; a < 3; ++a) {
      Sum sum;
      for (int r = 0; r < 3; ++r)
        sum.add(scaled(world[r], {rotation[a][r]}, {speed}));
      out[a] = sum.get();
    }
    return out;
  }
  void point(const openvdb::Coord &local, const openvdb::Coord &offset) const {
    for (int r = 0; r < 3; ++r) {
      Sum sum;
      sum.add(origin[r]);
      for (int a = 0; a < 3; ++a)
        for (double x : {double(offset[a]), double(local[a])}) {
          const double product = basis[a][r] * x;
          sum.add(product);
          sum.add(std::fma(basis[a][r], x, -product));
        }
      const double world = sum.get(),
                   gap = std::max(
                       std::abs(std::nextafter(world, INFINITY) - world),
                       std::abs(world - std::nextafter(world, -INFINITY)));
      if (gap > h * 1e-4)
        throw std::invalid_argument(
            "potential-flow world domain cannot retain source voxel precision; "
            "recenter transform");
    }
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
struct Bounds {
  openvdb::Coord offset;
  openvdb::CoordBBox box;
  long double slots = 0, reserved = 0;
};
template <class G>
Bounds bounds(const G &g, const Map &m, const Loaded &loaded,
              const PotentialFlowOptions &o, const ProcessingOptions &p,
              Work &w) {
  // Charge the source traversal before looking at individual stored values.
  w.add(g.memUsage() * 16.0L);
  const double bg = g.background(), band = bg / m.h;
  if (g.empty() || g.tree().hasActiveTiles() || !std::isfinite(bg) || bg <= 0 ||
      !std::isfinite(band) || band < 3 - 1e-6 || band > 64 + 1e-6)
    throw std::invalid_argument("potential flow requires nonempty voxel-only "
                                "symmetric LEVEL_SET band[3,64]");
  if (o.exteriorWidth <= std::ceil(band) + 1)
    throw std::invalid_argument(
        "potential-flow exteriorWidth must exceed ceil(source band)+1 for "
        "outer Dirichlet faces");
  auto box = g.evalActiveVoxelBoundingBox();
  for (auto v = g.cbeginValueAll(); v; ++v) {
    const double x = *v;
    if (!std::isfinite(x) || std::abs(x) > bg * (1 + 1e-6) ||
        (!v.isValueOn() && std::abs(std::abs(x) - bg) > bg * 1e-6))
      throw std::invalid_argument("potential-flow SDF needs finite distances "
                                  "and symmetric inactive band");
    if (x != bg)
      box.expand(v.getBoundingBox());
  }
  Bounds b;
  for (int a = 0; a < 3; ++a) {
    const int64_t lo = box.min()[a], hi = box.max()[a];
    if (lo < INT32_MIN + 8192LL || hi > INT32_MAX - 8192LL)
      throw std::invalid_argument(
          "potential-flow source lacks safe native coordinate/node halos");
    b.offset[a] = int32_t(lo + (hi - lo) / 2);
  }
  box.translate(-b.offset);
  box.expand(int(o.exteriorWidth) + 4);
  b.box = box;
  b.slots = blocks(box, 8) * 512;
  auto publishedBox = box;
  publishedBox.translate(b.offset);
  const long double publishedSlots = blocks(publishedBox, 8) * 512;
  if (b.slots > p.maxVoxelCount || publishedSlots * 4 > p.maxVoxelCount ||
      b.slots >= INT32_MAX / 4)
    throw std::invalid_argument("potential-flow full interior/exterior "
                                "expanded domain exceeds voxel/index cap");
  b.reserved = loaded.resident * 8 + 32 * loaded.meta +
               8 * treeBytes<Vector>(box) + 8 * treeBytes<Scalar>(box) +
               12 * treeBytes<Domain>(box) +
               4 * treeBytes<Vector>(publishedBox) +
               2 * treeBytes<Scalar>(publishedBox) +
               2 * treeBytes<Mask>(publishedBox) + b.slots * 768 + Fixed;
  bytes(b.reserved, p,
        "potential-flow full interior/domain/gradient/matrix/Krylov/writer "
        "workspace exceeds memory budget");
  w.add(b.slots * (128.0L + 32.0L * o.exteriorWidth));
  for (int corner = 0; corner < 8; ++corner) {
    openvdb::Coord c;
    for (int a = 0; a < 3; ++a)
      c[a] = (corner & (1 << a)) ? box.max()[a] : box.min()[a];
    m.point(c, b.offset);
  }
  return b;
}
template <class G>
Scalar::Ptr normalize(const G &g, const Map &m, const Bounds &b) {
  auto out = Scalar::create(scaled(g.background(), {}, {m.h}));
  out->setGridClass(openvdb::GRID_LEVEL_SET);
  auto a = out->getAccessor();
  for (auto v = g.cbeginValueAll(); v; ++v) {
    if (!v.isValueOn() && *v == g.background())
      continue;
    const double x = scaled(double(*v), {}, {m.h});
    if (v.isVoxelValue()) {
      const auto c = v.getCoord() - b.offset;
      if (v.isValueOn())
        a.setValue(c, x);
      else
        a.setValueOff(c, x);
    } else {
      auto box = v.getBoundingBox();
      box.translate(-b.offset);
      out->tree().fill(box, x, v.isValueOn());
    }
  }
  bool crossing = false;
  for (auto v = out->cbeginValueOn(); v && !crossing; ++v)
    for (int k = 0; k < 3; ++k) {
      auto c = v.getCoord();
      ++c[k];
      const double x = *v, y = a.getValue(c);
      crossing |= (x < 0 && y >= 0) || (x >= 0 && y < 0);
    }
  if (!crossing)
    throw std::invalid_argument(
        "potential-flow collider has no resolved zero crossing");
  return out;
}
struct Component {
  std::vector<openvdb::Coord> cells;
  double rhsPeak = 0;
};
double source(const Vector::ConstAccessor &n, const openvdb::Coord &c,
              const Domain::ConstAccessor &d) {
  if (!n.isValueOn(c))
    return 0;
  const auto v = n.getValue(c);
  Sum sum;
  for (int a = 0; a < 3; ++a)
    for (int sign : {-1, 1}) {
      auto q = c;
      q[a] += sign;
      if (!d.isValueOn(q))
        sum.add(-double(sign) * v[a]);
    }
  return sum.get();
}
std::vector<Component> components(const Domain &domain, const Vector &neumann,
                                  const Scalar &collider,
                                  const PotentialFlowOptions &o, Work &w) {
  const auto count = domain.activeVoxelCount();
  if (!count || count > o.maxSolverVoxels)
    throw std::invalid_argument(
        "potential-flow nonempty fixed domain exceeds maxSolverVoxels");
  w.add(static_cast<long double>(count) * (256.0L + 64.0L * o.maxIterations));
  std::set<openvdb::Coord> remaining;
  for (auto v = domain.cbeginValueOn(); v; ++v)
    remaining.insert(v.getCoord());
  const auto d = domain.getConstAccessor();
  const auto n = neumann.getConstAccessor();
  const auto s = collider.getConstAccessor();
  std::vector<Component> out;
  while (!remaining.empty()) {
    Component c;
    bool dirichlet = false;
    c.cells.push_back(*remaining.begin());
    remaining.erase(remaining.begin());
    for (size_t i = 0; i < c.cells.size(); ++i) {
      const auto p = c.cells[i];
      c.rhsPeak = std::max(c.rhsPeak, std::abs(source(n, p, d)));
      for (int a = 0; a < 3; ++a)
        for (int sign : {-1, 1}) {
          auto q = p;
          q[a] += sign;
          if (d.isValueOn(q)) {
            auto it = remaining.find(q);
            if (it != remaining.end()) {
              c.cells.push_back(q);
              remaining.erase(it);
            }
            continue;
          }
          const bool solid = s.getValue(q) < 0, active = n.isValueOn(p);
          if (solid && !active)
            throw std::invalid_argument(
                "potential-flow collider boundary has unresolved zero-gradient "
                "Neumann normals");
          if (!solid && active)
            throw std::invalid_argument(
                "potential-flow outer positive face is incorrectly "
                "Neumann-tagged; widen exterior domain or rebuild SDF");
          if (!active)
            dirichlet = true;
        }
    }
    if (!dirichlet)
      throw std::invalid_argument(
          "potential-flow pure-Neumann closed component needs a compatible "
          "gauge; closed-cavity solves are unsupported");
    std::sort(c.cells.begin(), c.cells.end());
    out.push_back(std::move(c));
  }
  return out;
}
struct BoundaryOp {
  const Vector &n;
  void operator()(const openvdb::Coord &c, const openvdb::Coord &q, double &rhs,
                  double &diagonal) const {
    const auto a = n.getConstAccessor();
    if (a.isValueOn(c)) {
      const auto v = a.getValue(c);
      const auto diff = c - q;
      for (int k = 0; k < 3; ++k)
        rhs += diff[k] * v[k];
    } else
      diagonal -= 1;
  }
};
void solve(const std::vector<Component> &components, const Domain &domain,
           const Vector &neumann, const Map &m, double speed,
           const PotentialFlowOptions &o, Scalar &physical, Scalar &unit,
           PotentialFlowResult &r) {
  const auto da = domain.getConstAccessor();
  const auto na = neumann.getConstAccessor();
  auto pa = physical.getAccessor(), ua = unit.getAccessor();
  for (const auto &component : components) {
    auto d = Domain::create();
    auto dv = d->getAccessor();
    auto n = Vector::create(V(0));
    auto nv = n->getAccessor();
    for (const auto &c : component.cells) {
      dv.setValueOn(c);
      if (na.isValueOn(c) && component.rhsPeak) {
        const auto value = na.getValue(c);
        V v;
        for (int a = 0; a < 3; ++a)
          v[a] = scaled(value[a], {}, {component.rhsPeak});
        nv.setValue(c, v);
      }
    }
    Scalar::Ptr q;
    if (component.rhsPeak == 0) {
      q = Scalar::create(0);
      auto a = q->getAccessor();
      for (const auto &c : component.cells)
        a.setValue(c, 0);
    } else {
      if (r.iterations >= o.maxIterations)
        throw std::runtime_error(
            "potential-flow iteration budget exhausted before all components "
            "converged; no output written");
      auto state = openvdb::math::pcg::terminationDefaults<double>();
      state.iterations = int(o.maxIterations - r.iterations);
      state.relativeError =
          o.tolerance * .125 / std::sqrt(double(component.cells.size()));
      state.absoluteError = 0;
      q = openvdb::tools::computeScalarPotential(*d, *n, state);
      r.iterations += uint32_t(std::max(1, state.iterations));
      if (!state.success && r.iterations < o.maxIterations) {
        // IC can stop early on irregular topology. A fresh Jacobi attempt
        // shares the SAME caller iteration cap; no relaxed success/residual
        // criterion.
        state.iterations = int(o.maxIterations - r.iterations);
        state.relativeError =
            o.tolerance * .125 / std::sqrt(double(component.cells.size()));
        state.absoluteError = 0;
        openvdb::DoubleTree zero(d->tree(), 0., openvdb::TopologyCopy());
        openvdb::util::NullInterrupter interrupt;
        using Jacobi = openvdb::math::pcg::JacobiPreconditioner<
            openvdb::tools::poisson::LaplacianMatrix>;
        auto tree = openvdb::tools::poisson::
            solveWithBoundaryConditionsAndPreconditioner<Jacobi>(
                zero, d->tree(), BoundaryOp{*n}, state, interrupt, true);
        q = Scalar::create(tree);
        r.iterations += uint32_t(std::max(1, state.iterations));
      }
      if (!state.success || r.iterations > o.maxIterations ||
          !std::isfinite(state.relativeError))
        throw std::runtime_error(
            "potential-flow native solver did not converge within iterations; "
            "no output written");
    }
    const auto qa = q->getConstAccessor();
    for (const auto &c : component.cells) {
      const double value =
          component.rhsPeak
              ? scaled(qa.getValue(c), {component.rhsPeak, speed, m.h})
              : 0.;
      pa.setValue(c, value);
      ua.setValue(c, scaled(value, {}, {speed, m.h}));
    }
    // Restore normalized q FROM STORED physical output before checking the FULL
    // mixed-BC operator. Tiny component RHS never disappears under a global
    // norm.
    if (component.rhsPeak) {
      const auto stored = physical.getConstAccessor();
      const auto value = [&](const openvdb::Coord &c) {
        return scaled(stored.getValue(c), {}, {speed, m.h, component.rhsPeak});
      };
      for (const auto &c : component.cells) {
        Sum lap;
        const double center = value(c);
        for (int a = 0; a < 3; ++a)
          for (int sign : {-1, 1}) {
            auto qcoord = c;
            qcoord[a] += sign;
            if (da.isValueOn(qcoord)) {
              lap.add(center);
              lap.add(-value(qcoord));
            } else if (!na.isValueOn(c))
              lap.add(center);
          }
        const double expected =
            -scaled(source(na, c, da), {}, {component.rhsPeak});
        const double error = std::abs(lap.get() - expected);
        r.residual = std::max(r.residual, error);
      }
      if (!std::isfinite(r.residual) || r.residual > o.tolerance)
        throw std::runtime_error(
            "potential-flow stored physical potential cannot meet the "
            "independently measured residual; no output written");
    }
  }
}
V normal(const Scalar &collider, const openvdb::Coord &c) {
  const auto a = collider.getConstAccessor();
  V gradient;
  for (int axis = 0; axis < 3; ++axis) {
    auto left = c, right = c;
    --left[axis];
    ++right[axis];
    gradient[axis] = .5 * (a.getValue(right) - a.getValue(left));
  }
  const double length = std::hypot(gradient[0], gradient[1], gradient[2]);
  if (!std::isfinite(length) || length <= 1e-12)
    throw std::invalid_argument(
        "potential-flow solid boundary has an unresolved normal");
  return gradient / length;
}
void diagnostics(const Scalar &collider, const Domain &domain,
                 const Vector &neumann, const Vector &physical, const Map &m,
                 double speed, const V &colliderVelocity, const V &wind,
                 const PotentialFlowOptions &o, PotentialFlowResult &r,
                 Work &w) {
  const auto d = domain.getConstAccessor();
  const auto n = neumann.getConstAccessor();
  const auto v = physical.getConstAccessor();
  w.add(static_cast<long double>(domain.activeVoxelCount()) * 512);
  for (auto cell = domain.cbeginValueOn(); cell; ++cell) {
    const auto c = cell.getCoord();
    bool boundary = false;
    for (int a = 0; a < 3; ++a)
      for (int sign : {-1, 1}) {
        auto q = c;
        q[a] += sign;
        if (!d.isValueOn(q))
          boundary = true;
      }
    if (boundary && n.isValueOn(c)) {
      const V local = m.localVector(v.getValue(c) - colliderVelocity, speed),
              unitNormal = normal(collider, c);
      Sum error;
      for (int a = 0; a < 3; ++a)
        error.add(local[a] * unitNormal[a]);
      const double relative = std::abs(error.get());
      if (relative >
          o.tolerance * 16 + 512 * std::numeric_limits<double>::epsilon())
        throw std::runtime_error("potential-flow stored boundary velocity does "
                                 "not satisfy normal condition");
      r.maxBoundaryNormalError =
          std::max(r.maxBoundaryNormalError, scaled(relative, {speed}));
    }
    bool complete = true;
    for (int x = -2; x <= 2 && complete; ++x)
      for (int y = -2; y <= 2 && complete; ++y)
        for (int z = -2; z <= 2; ++z)
          if (!d.isValueOn(c + openvdb::Coord(x, y, z))) {
            complete = false;
            break;
          }
    if (!complete)
      continue;
    ++r.interiorDiagnosticCellCount;
    V derivative[3];
    for (int a = 0; a < 3; ++a) {
      auto left = c, right = c;
      --left[a];
      ++right[a];
      derivative[a] = (m.localVector(v.getValue(right) - wind, speed) -
                       m.localVector(v.getValue(left) - wind, speed)) *
                      .5;
    }
    Sum div;
    for (int a = 0; a < 3; ++a)
      div.add(derivative[a][a]);
    const V curl(derivative[1][2] - derivative[2][1],
                 derivative[2][0] - derivative[0][2],
                 derivative[0][1] - derivative[1][0]);
    r.maxInteriorDivergence = std::max(
        r.maxInteriorDivergence, scaled(std::abs(div.get()), {speed}, {m.h}));
    r.maxInteriorCurl =
        std::max(r.maxInteriorCurl,
                 scaled(std::hypot(curl[0], curl[1], curl[2]), {speed}, {m.h}));
  }
}
void structure(openvdb::GridBase &g, const openvdb::GridBase &source,
               const std::string &name, bool vector, const char *units) {
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
        "container:potential_flow_boundary"})
    g.removeMeta(key);
  g.setName(name);
  g.setGridClass(openvdb::GRID_UNKNOWN);
  g.setVectorType(vector ? openvdb::VEC_CONTRAVARIANT_RELATIVE
                         : openvdb::VEC_INVARIANT);
  g.setIsInWorldSpace(true);
  g.setSaveFloatAsHalf(false);
  g.insertMeta("container:potential_flow_units",
               openvdb::StringMetadata(units));
  g.insertMeta("container:potential_flow_boundary",
               openvdb::StringMetadata(
                   "fixed18-neighbor exterior shell; projected Neumann "
                   "collider; zero-potential outer Dirichlet"));
}
void publish(const Snapshot &s, const std::filesystem::path &output,
             const openvdb::GridPtrVec &grids, const Loaded &l, const Bounds &b,
             const ProcessingOptions &p) {
  const auto filePath = s.directory / "result.vdb";
  if (std::filesystem::exists(filePath))
    throw std::logic_error("potential-flow private output already exists");
  openvdb::GridPtrVec views;
  views.reserve(grids.size());
  for (const auto &g : grids) {
    if (g->isType<Vector>())
      views.push_back(std::make_shared<ExactWriteGrid<Vector>>(
          *openvdb::gridPtrCast<Vector>(g)));
    else if (g->isType<Scalar>())
      views.push_back(std::make_shared<ExactWriteGrid<Scalar>>(
          *openvdb::gridPtrCast<Scalar>(g)));
    else
      views.push_back(std::make_shared<ExactWriteGrid<Mask>>(
          *openvdb::gridPtrCast<Mask>(g)));
  }
  openvdb::io::File writer(filePath.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(views, *l.fileMeta);
  writer.close();
  bytes(b.reserved + std::filesystem::file_size(filePath), p,
        "potential-flow encoded output exceeds combined workspace budget");
  bool created = false;
  std::ofstream target;
  try {
    std::ifstream source(filePath, std::ios::binary);
    if (!source)
      throw std::runtime_error("cannot read potential-flow private output");
    target.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    created = target.is_open();
    if (!created)
      throw std::runtime_error(
          "cannot exclusively create potential-flow output");
    target.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 65536> buffer{};
    while (source.read(buffer.data(), std::streamsize(buffer.size())) ||
           source.gcount())
      target.write(buffer.data(), source.gcount());
    if (!source.eof())
      throw std::runtime_error("potential-flow output read failed");
    target.flush();
    target.close();
  } catch (...) {
    target.exceptions(std::ios::goodbit);
    if (target.is_open())
      target.close();
    if (created) {
      std::error_code e;
      std::filesystem::remove(output, e);
    }
    throw;
  }
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
            "potential-flow restored output index exceeds native node margins");
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
template <class G>
PotentialFlowResult
run(const G &source, const Loaded &loaded, const Snapshot &snapshot,
    const std::filesystem::path &output, const PotentialFlowOptions &o,
    const ProcessingOptions &p) {
  Map m(source);
  Work work{0, o.maxWorkItems};
  const auto b = bounds(source, m, loaded, o, p, work);
  auto local = normalize(source, m, b);
  auto interior = Mask::create(false);
  for (auto v = local->cbeginValueAll(); v; ++v)
    if (*v < 0)
      interior->tree().fill(v.getBoundingBox(), true, true);
  auto domain = openvdb::tools::createPotentialFlowMask<Mask, Domain>(
      *interior, int(o.exteriorWidth));
  if (domain->activeVoxelCount() > o.maxSolverVoxels ||
      domain->activeVoxelCount() > p.maxVoxelCount)
    throw std::invalid_argument("potential-flow fixed domain exceeds "
                                "solver/voxel limits before expansion");
  domain->tree().voxelizeActiveTiles();
  std::array<double, 3> relativeWorld;
  double relativeSpeed = 0;
  for (int a = 0; a < 3; ++a) {
    // Subtract in WORLD units before rotation/conditioning. Separately rounding
    // two large common velocities can erase their representable difference.
    relativeWorld[a] = o.colliderVelocity[a] - o.farFieldVelocity[a];
    relativeSpeed = std::max(relativeSpeed, std::abs(relativeWorld[a]));
  }
  if (relativeSpeed != 0)
    for (double x : o.farFieldVelocity) {
      const double gap = std::max(std::abs(std::nextafter(x, INFINITY) - x),
                                  std::abs(x - std::nextafter(x, -INFINITY)));
      if (exceedsProduct(gap, relativeSpeed, o.tolerance * .125))
        throw std::invalid_argument(
            "potential-flow stored total velocity lacks precision for relative "
            "motion at the requested tolerance; reduce the common velocity");
    }
  const double speed = relativeSpeed == 0 ? 1 : relativeSpeed;
  const V wind(o.farFieldVelocity.data()),
      colliderVelocity(o.colliderVelocity.data()),
      relativeLocal = m.local(relativeWorld, speed);
  auto boundary = Vector::create(relativeLocal);
  boundary->tree().setValue(openvdb::Coord(0), relativeLocal);
  auto neumann = openvdb::tools::createPotentialFlowNeumannVelocities<V>(
      *local, *domain, boundary, V(0));
  auto connected = components(*domain, *neumann, *local, o, work);
  PotentialFlowResult r;
  r.domainCellCount = domain->activeVoxelCount();
  r.componentCount = connected.size();
  r.neumannCellCount = neumann->activeVoxelCount();
  auto potential = Scalar::create(0), unit = Scalar::create(0);
  solve(connected, *domain, *neumann, m, speed, o, *potential, *unit, r);
  auto nativeFlow =
      openvdb::tools::computePotentialFlow<Vector>(*unit, *neumann, V(0));
  auto flow = Vector::create(wind), normals = Vector::create(V(0));
  auto fa = flow->getAccessor(), na = normals->getAccessor();
  const auto nv = nativeFlow->getConstAccessor();
  for (auto c = domain->cbeginValueOn(); c; ++c) {
    V total = m.worldVector(nv.getValue(c.getCoord()), speed);
    for (int a = 0; a < 3; ++a) {
      Sum sum;
      sum.add(wind[a]);
      sum.add(total[a]);
      total[a] = sum.get();
    }
    fa.setValue(c.getCoord(), total);
  }
  for (auto v = neumann->cbeginValueOn(); v; ++v)
    na.setValue(v.getCoord(), m.worldVector(*v, speed));
  diagnostics(*local, *domain, *neumann, *flow, m, speed, colliderVelocity,
              wind, o, r, work);
  auto mask = Mask::create(false);
  mask->tree().topologyUnion(domain->tree());
  for (auto v = mask->beginValueOn(); v; ++v)
    v.setValue(true);
  // Unit solve coordinates were integer-recentered. Restore every active
  // partition before attaching the ORIGINAL exact source map. Keep original
  // grids alive until their native accessors are destroyed.
  auto publishedFlow = restoreIndexCoordinates(*flow, b.offset),
       publishedNormals = restoreIndexCoordinates(*normals, b.offset);
  auto publishedPotential = restoreIndexCoordinates(*potential, b.offset);
  auto publishedMask = restoreIndexCoordinates(*mask, b.offset);
  structure(*publishedFlow, source, o.velocityGridName, true,
            "world_length_per_second");
  structure(*publishedPotential, source, o.potentialGridName, false,
            "world_length_squared_per_second");
  structure(*publishedMask, source, o.domainGridName, false,
            "active_fluid_cell");
  structure(*publishedNormals, source, o.neumannGridName, true,
            "world_length_per_second_projected_perturbation");
  openvdb::GridPtrVec grids{publishedFlow, publishedPotential, publishedMask,
                            publishedNormals};
  long double retained = 0, slots = 0;
  uint64_t active = 0;
  for (const auto &g : grids) {
    retained += g->memUsage() + metadata(*g);
    slots += g->baseTree().leafCount() * 512.0L;
    if (g->activeVoxelCount() > p.maxVoxelCount - active)
      throw std::invalid_argument(
          "potential-flow summed output active cells exceed voxel cap");
    active += g->activeVoxelCount();
    r.grids.push_back({g->getName(), "unknown", g->activeVoxelCount(), m.h});
  }
  if (slots > p.maxVoxelCount)
    throw std::invalid_argument(
        "potential-flow summed output stored leaves exceed voxel cap");
  bytes(
      loaded.resident * 8 + retained * 4 + 32 * loaded.meta + Fixed, p,
      "potential-flow retained output/metadata/writer exceed shared byte cap");
  r.workItems = work.used;
  r.converged = true;
  r.diagnostics.push_back(
      "Fixed finite18-neighbor exterior shell; mixed seven-point potential "
      "solve, not arbitrary-field pressure projection. Native boundary normal "
      "substitution drops tangential potential derivative.");
  r.diagnostics.push_back(
      "Interior central divergence/curl are diagnostics on complete2-ring "
      "stencils, not MAC incompressibility or infinite-domain convergence "
      "proofs. Pure-Neumann closed components and spatial boundary velocity "
      "fields are unsupported.");
  if (!r.interiorDiagnosticCellCount)
    r.diagnostics.push_back(
        "No complete two-ring interior stencil was available; zero "
        "divergence/curl fields are unavailable placeholders.");
  publish(snapshot, output, grids, loaded, b, p);
  return r;
}
#endif
} // namespace

PotentialFlowResult WritePotentialFlow(const std::filesystem::path &input,
                                       const std::filesystem::path &output,
                                       const PotentialFlowOptions &o,
                                       const std::string &selector,
                                       const ProcessingOptions &p) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;
  (void)output;
  (void)o;
  (void)selector;
  (void)p;
  unavailable();
#else
  validate(o, p);
#ifndef OPENVDB_USE_ZLIB
  throw std::invalid_argument(
      "potential flow requires OpenVDB ZIP compression support");
#endif
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "potential-flow output must be a new path in an existing directory");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Snapshot snapshot(input, p);
    auto loaded = load(snapshot, o, selector, p);
    if (loaded.grid->isType<openvdb::FloatGrid>())
      return run(*openvdb::gridPtrCast<openvdb::FloatGrid>(loaded.grid), loaded,
                 snapshot, output, o, p);
    return run(*openvdb::gridPtrCast<openvdb::DoubleGrid>(loaded.grid), loaded,
               snapshot, output, o, p);
  });
#endif
}
} // namespace container::geometry::vdb
