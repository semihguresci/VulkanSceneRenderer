#include "Container/geometry/OpenVdbLevelSetRebuild.h"
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
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/LevelSetRebuild.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#endif
namespace container::geometry::vdb {
namespace {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = openvdb::Vec3d;
using Tree = openvdb::DoubleTree;
using Leaf = Tree::LeafNodeType;
using Upper = Tree::RootNodeType::ChildNodeType;
using Lower = Upper::ChildNodeType;
constexpr long double Fixed = 4.0L * 1024 * 1024;
void budget(long double bytes, const ProcessingOptions &p, const char *why) {
  if (!std::isfinite(bytes) || bytes > p.maxInputBytes)
    throw std::invalid_argument(why);
}
long double metaBytes(const openvdb::MetaMap &map) {
  long double bytes = sizeof(map);
  for (auto i = map.beginMeta(); i != map.endMeta(); ++i)
    bytes += 4.0L * (i->first.size() + i->second->typeName().size() +
                     i->second->size()) +
             512;
  return bytes;
}
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &source, const ProcessingOptions &p) {
    if (!std::filesystem::is_regular_file(source))
      throw std::invalid_argument(
          "level-set rebuild input is not a regular file");
    encoded = std::filesystem::file_size(source);
    budget(encoded + Fixed, p,
           "level-set rebuild encoded input exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned n = 0; n < 64; ++n) {
      directory =
          std::filesystem::temp_directory_path() /
          ("container-level-set-rebuild-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(directory)) {
        file = directory / "source.vdb";
        break;
      }
    }
    if (file.empty())
      throw std::runtime_error(
          "cannot create level-set rebuild private snapshot");
    try {
      std::ifstream input(source, std::ios::binary);
      std::ofstream output(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!input || !output)
        throw std::runtime_error("cannot open level-set rebuild snapshot");
      std::array<char, 65536> buffer{};
      auto remaining = encoded;
      while (remaining) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(remaining, buffer.size()));
        input.read(buffer.data(), count);
        if (input.gcount() != count)
          throw std::runtime_error(
              "level-set rebuild input changed while snapshotting");
        output.write(buffer.data(), count);
        remaining -= count;
      }
      output.close();
      if (!output || input.bad() ||
          input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "level-set rebuild snapshot failed or input changed size");
    } catch (...) {
      cleanup();
      throw;
    }
  }
  void cleanup() {
    std::error_code e;
    std::filesystem::remove(file, e);
    std::filesystem::remove(directory, e);
  }
  ~Snapshot() { cleanup(); }
};
struct Loaded {
  std::unique_ptr<Snapshot> snapshot;
  openvdb::GridBase::Ptr grid;
  openvdb::MetaMap::Ptr fileMetadata;
  long double resident = 0, metadata = 0;
};
class MetadataArchive : public openvdb::io::Archive {
public:
  void begin(std::istream &stream, openvdb::io::StreamMetadata::Ptr &metadata) {
    readHeader(stream);
    if (!inputHasGridOffsets() ||
        fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
        fileVersion() > openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument(
          "rebuild require supported seekable metadata-readable VDB files");
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
          "rebuild grid descriptor offsets are outside the encoded range");
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
          "rebuild metadata/transform crosses grid payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};
Loaded load(const std::filesystem::path &input, const std::string &selector,
            const LevelSetRebuildOptions &o, const ProcessingOptions &p) {
  Loaded r;
  r.snapshot = std::make_unique<Snapshot>(input, p);
  detail::RequireSeekableFile(r.snapshot->file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded level-set rebuild requires delayed "
                                "loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded level-set rebuild requires OpenVDB delayed-loading support");
#else
  auto streamMeta = std::make_shared<openvdb::io::StreamMetadata>();
  streamMeta->setSeekable(true);
  std::ifstream stream(r.snapshot->file, std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  MetadataArchive archive;
  archive.begin(stream, streamMeta);
  r.fileMetadata = std::make_shared<openvdb::MetaMap>();
  r.fileMetadata->readMeta(stream);
  r.metadata = metaBytes(*r.fileMetadata);
  long double catalog = r.snapshot->encoded + Fixed;
  const auto check = [&] {
    if (r.metadata > o.maxMetadataBytes)
      throw std::invalid_argument(
          "aggregate rebuild file/grid metadata exceeds maxMetadataBytes");
    budget(catalog + 8 * r.metadata, p,
           "rebuild catalog metadata exceeds memory budget");
  };
  check();
  const auto gridCount = archive.count(stream);
  if (gridCount < 0 || uint32_t(gridCount) > o.maxGrids)
    throw std::invalid_argument("rebuild catalog exceeds maxGrids");
  std::set<std::string> names;
  std::string chosen;
  bool found = false;
  for (int32_t n = 0; n < gridCount; ++n) {
    openvdb::io::GridDescriptor descriptor;
    const auto m = archive.next(stream, descriptor, r.snapshot->encoded);
    const auto name =
        openvdb::io::GridDescriptor::nameAsString(descriptor.uniqueName());
    catalog += 4.0L * name.size() + 256;
    if (!names.insert(name).second)
      throw std::invalid_argument(
          "rebuild source has colliding descriptors; rename grids first");
    r.metadata += metaBytes(*m);
    check();
    if ((!m->isType<openvdb::FloatGrid>() &&
         !m->isType<openvdb::DoubleGrid>()) ||
        (m->getGridClass() != openvdb::GRID_UNKNOWN &&
         m->getGridClass() != openvdb::GRID_LEVEL_SET &&
         m->getGridClass() != openvdb::GRID_FOG_VOLUME) ||
        !GridSelectionMatches(selector, name, m->getName()))
      continue;
    if (selector.empty() && found)
      continue;
    if (found)
      throw std::invalid_argument(
          "rebuild selector is ambiguous; use descriptor:<exact name>");
    const auto mem = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES),
               voxels = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!mem || !voxels || mem->value() < 0 || voxels->value() < 0 ||
        uint64_t(voxels->value()) > p.maxVoxelCount)
      throw std::invalid_argument(
          "rebuild requires recorded memory/voxel statistics within budgets");
    budget(catalog + mem->value() + 8 * r.metadata, p,
           "recorded rebuild source exceeds memory budget");
    chosen = name;
    found = true;
  }
  if (!found)
    throw std::invalid_argument(
        "no matching Float/Double scalar rebuild source");
  stream.close();
  openvdb::io::File f(r.snapshot->file.string());
  f.setCopyMaxBytes(0);
  f.open(true);
  if (f.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error(
        "rebuild immutable snapshot changed before tree read");
  r.grid = f.readGrid(chosen);
  if (r.grid->activeVoxelCount() > p.maxVoxelCount ||
      r.grid->baseTree().leafCount() >
          p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument("level-set rebuild native leaf/voxel topology "
                                "exceeds budget before materialization");
  const auto full =
      r.grid->isType<openvdb::FloatGrid>()
          ? openvdb::tools::memUsageIfLoaded(
                openvdb::gridPtrCast<openvdb::FloatGrid>(r.grid)->tree(), false)
          : openvdb::tools::memUsageIfLoaded(
                openvdb::gridPtrCast<openvdb::DoubleGrid>(r.grid)->tree(),
                false);
  r.resident = catalog + full + 8 * r.metadata;
  budget(
      r.resident, p,
      "fully loaded level-set rebuild tree exceeds memory budget before leaf "
      "materialization");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full)
    throw std::runtime_error(
        "level-set rebuild tree exceeded full decoded memory estimate");
  f.close();
  return r;
#endif
}
struct Sum {
  double sum{}, error{};
  void add(double x) {
    const double next = sum + x;
    if (!std::isfinite(x) || !std::isfinite(next))
      throw std::invalid_argument("rebuild affine arithmetic overflows");
    error += std::abs(sum) >= std::abs(x) ? (sum - next) + x : (x - next) + sum;
    sum = next;
  }
  void product(double a, double b) {
    const auto p = a * b;
    if (a != 0 && b != 0 && p == 0)
      throw std::invalid_argument("rebuild affine product underflows");
    add(p);
    add(std::fma(a, b, -p));
  }
  double get() const {
    const auto x = sum + error;
    if (!std::isfinite(x))
      throw std::invalid_argument("rebuild affine sum overflows");
    return x;
  }
};
struct Map {
  V origin;
  std::array<V, 3> axis, unit, inverse;
  std::array<double, 3> lengths;
  double h{};
  Map(const openvdb::math::Transform &transform) {
    if (!transform.isLinear())
      throw std::invalid_argument(
          "rebuild requires uniform orthogonal affine transforms");
    const auto m = transform.baseMap();
    origin = m->applyMap(V(0));
    for (int a = 0; a < 3; ++a) {
      V e(0);
      e[a] = 1;
      axis[a] = m->applyJacobian(e);
      const auto n = std::hypot(axis[a].x(), axis[a].y(), axis[a].z());
      if (!std::isfinite(origin[a]) || !std::isfinite(n) || n < 1e-6 || n > 1e6)
        throw std::invalid_argument(
            "rebuild axes/origin must be finite; voxel size in[1e-6,1e6]");
      if (a == 0)
        h = n;
      else if (std::abs(n / h - 1) > 1e-9)
        throw std::invalid_argument(
            "rebuild target/source voxels must be uniform");
      unit[a] = axis[a] / n;
      lengths[a] = n;
    }
    for (int a = 0; a < 3; ++a)
      for (int b = a + 1; b < 3; ++b)
        if (std::abs(unit[a].dot(unit[b])) > 1e-9)
          throw std::invalid_argument(
              "rebuild target/source axes must be orthogonal");
    const double determinant = unit[0].dot(unit[1].cross(unit[2]));
    for (int a = 0; a < 3; ++a)
      inverse[a] =
          unit[(a + 1) % 3].cross(unit[(a + 2) % 3]) / determinant / lengths[a];
  }
  V indexVector(const V &p) const {
    V q;
    for (int a = 0; a < 3; ++a) {
      Sum s;
      for (int c = 0; c < 3; ++c)
        s.product(inverse[a][c], p[c]);
      q[a] = s.get();
    }
    return q;
  }
  V relative(const Map &source, const V &index) const {
    V delta;
    for (int c = 0; c < 3; ++c) {
      Sum s;
      s.add(source.origin[c] - origin[c]);
      for (int a = 0; a < 3; ++a)
        s.product(source.axis[a][c], index[a]);
      delta[c] = s.get();
    }
    return indexVector(delta);
  }
};
openvdb::math::Transform::Ptr
transform(const std::array<double, 16> &publicMatrix) {
  for (double v : publicMatrix)
    if (!std::isfinite(v))
      throw std::invalid_argument("rebuild target matrix must be finite");
  if (publicMatrix[12] != 0 || publicMatrix[13] != 0 || publicMatrix[14] != 0 ||
      publicMatrix[15] != 1)
    throw std::invalid_argument(
        "rebuild target matrix must be affine column-vector row-major");
  openvdb::Mat4d m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      m[r][c] = publicMatrix[c * 4 + r];
  bool diagonal = true;
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b)
      if (a != b && m[a][b] != 0)
        diagonal = false;
  if (!diagonal)
    return detail::MakeAffineTransformExact(m);
  // Read stock ScaleTranslateMap fields directly: its ordinary constructor
  // rejects tiny determinants despite a finite well-conditioned diagonal map.
  V scale(m[0][0], m[1][1], m[2][2]), origin(m[3][0], m[3][1], m[3][2]);
  for (int a = 0; a < 3; ++a)
    if (scale[a] == 0)
      throw std::invalid_argument("rebuild target diagonal scale is zero");
  const V inverse = V(1) / scale;
  std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
  record.exceptions(std::ios::badbit | std::ios::failbit);
  openvdb::writeString(record, openvdb::math::ScaleTranslateMap::mapType());
  origin.write(record);
  scale.write(record);
  V(std::abs(scale.x()), std::abs(scale.y()), std::abs(scale.z()))
      .write(record);
  inverse.write(record);
  V(inverse * inverse).write(record);
  V(inverse * .5).write(record);
  record.seekg(0);
  openvdb::io::setCurrentVersion(record);
  auto out = std::make_shared<openvdb::math::Transform>();
  out->read(record);
  return out;
}
bool same(const openvdb::math::Transform &a,
          const openvdb::math::Transform &b) {
  auto x = a.baseMap(), y = b.baseMap();
  if (x->applyMap(V(0)) != y->applyMap(V(0)))
    return false;
  for (int k = 0; k < 3; ++k) {
    V e(0);
    e[k] = 1;
    if (x->applyJacobian(e) != y->applyJacobian(e))
      return false;
  }
  return true;
}
int64_t block(int64_t x, int64_t d) { return x >= 0 ? x / d : (x + 1) / d - 1; }
long double blocks(const openvdb::CoordBBox &b, int64_t n) {
  long double v = 1;
  for (int a = 0; a < 3; ++a)
    v *= block(b.max()[a], n) - block(b.min()[a], n) + 1;
  return v;
}
long double treeBytes(const openvdb::CoordBBox &b) {
  return blocks(b, 8) * (sizeof(Leaf) + 4096.0L + 512) +
         blocks(b, 128) * (sizeof(Lower) + 512.0L) +
         blocks(b, 4096) * (sizeof(Upper) + 1024.0L);
}
long double cells(const openvdb::CoordBBox &b) {
  long double n = 1;
  for (int a = 0; a < 3; ++a)
    n *= int64_t(b.max()[a]) - b.min()[a] + 1;
  return n;
}
openvdb::Coord center(const openvdb::CoordBBox &b) {
  openvdb::Coord c;
  for (int a = 0; a < 3; ++a)
    c[a] = int32_t((int64_t(b.min()[a]) + b.max()[a]) / 2);
  return c;
}
openvdb::Coord rounded(const V &q) {
  openvdb::Coord c;
  for (int a = 0; a < 3; ++a) {
    if (!std::isfinite(q[a]) || q[a] < double(INT32_MIN) + 8192 ||
        q[a] > double(INT32_MAX) - 8192)
      throw std::invalid_argument(
          "rebuild target coordinates require recentering/partitioning");
    c[a] = int32_t(std::round(q[a]));
  }
  return c;
}
template <class F> void visit(const openvdb::CoordBBox &b, F f) {
  for (int64_t x = b.min().x(); x <= b.max().x(); ++x)
    for (int64_t y = b.min().y(); y <= b.max().y(); ++y)
      for (int64_t z = b.min().z(); z <= b.max().z(); ++z)
        f(openvdb::Coord(int32_t(x), int32_t(y), int32_t(z)));
}
double floatGap(double x) {
  const float f = float(x);
  return std::max(double(std::nextafter(f, INFINITY)) - f,
                  double(f) - std::nextafter(f, -INFINITY));
}
template <class Grid> struct ExactWrite : Grid {
  explicit ExactWrite(const Grid &g)
      : Grid(openvdb::ConstPtrCast<typename Grid::TreeType>(g.constTreePtr())) {
    this->insertMeta(g);
    this->setTransform(detail::CopyTransformExact(g.transform()));
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
void publish(const std::filesystem::path &temporary,
             const std::filesystem::path &output) {
  std::ifstream source(temporary, std::ios::binary);
  std::ofstream target;
  bool owned = false;
  try {
    target.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    owned = target.is_open();
    if (!source || !owned)
      throw std::runtime_error("cannot exclusively publish rebuilt level set");
    source.exceptions(std::ios::badbit);
    target.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 65536> b{};
    while (source.read(b.data(), b.size()) || source.gcount())
      target.write(b.data(), source.gcount());
    if (!source.eof())
      throw std::runtime_error("rebuild private output read failed");
    target.flush();
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
LevelSetRebuildResult rebuild(const Loaded &loaded, const Grid &input,
                              const std::filesystem::path &output,
                              const LevelSetRebuildOptions &o,
                              const ProcessingOptions &p) {
  using T = typename Grid::ValueType;
  if (!input.isInWorldSpace() ||
      input.getVectorType() != openvdb::VEC_INVARIANT)
    throw std::invalid_argument(
        "rebuild source requires world/invariant scalar semantics");
  const Map source(input.transform());
  auto targetTransform = o.targetIndexToWorld
                             ? transform(*o.targetIndexToWorld)
                             : detail::CopyTransformExact(input.transform());
  const Map target(*targetTransform);
  const auto box = input.evalActiveVoxelBoundingBox();
  if (box.empty())
    throw std::invalid_argument("rebuild source is empty");
  const auto nodeCounts = input.tree().nodeCount();
  const long double validationWork =
      static_cast<long double>(input.tree().root().getTableSize()) +
      nodeCounts.at(0) * 512.0L +
      nodeCounts.at(1) * static_cast<long double>(Lower::NUM_VALUES) +
      nodeCounts.at(2) * static_cast<long double>(Upper::NUM_VALUES);
  if (validationWork > o.maxWork)
    throw std::invalid_argument("rebuild source validation exceeds maxWork");
  const auto sourceCenter = center(box);
  const auto targetCenter =
      rounded(target.relative(source, sourceCenter.asVec3d()));
  const double bg = double(input.background()) - o.isovalue;
  if (!std::isfinite(bg) || bg == 0)
    throw std::invalid_argument(
        "rebuild source background must differ finitely from isovalue");
  const double polarity = bg > 0 ? 1. : -1.;
  double normalization = std::abs(bg);
  bool negative = false, positive = false;
  const auto difference = [&](double value) {
    const double d = (value - o.isovalue) * polarity;
    if (!std::isfinite(value) || !std::isfinite(d) ||
        (value != o.isovalue && d == 0))
      throw std::invalid_argument(
          "rebuild isovalue subtraction loses native dynamic range");
    return d;
  };
  for (auto it = input.cbeginValueAll(); it; ++it) {
    const double d = difference(double(*it));
    normalization = std::max(normalization, std::abs(d));
    negative |= d < 0;
    positive |= d > 0;
    if (d < 0 && !box.isInside(it.getBoundingBox()))
      throw std::invalid_argument(
          "rebuild has an interior inactive region outside active bounds");
  }
  if (!negative || (!positive && bg <= 0))
    throw std::invalid_argument("rebuild has no closed isovalue crossing");
  const long double sourceCells = cells(box);
  if (sourceCells > p.maxVoxelCount)
    throw std::invalid_argument(
        "rebuild dense source domain exceeds maxVoxelCount");
  openvdb::CoordBBox targetBox;
  for (int corner = 0; corner < 8; ++corner) {
    V q;
    for (int a = 0; a < 3; ++a)
      q[a] = corner & (1 << a) ? box.max()[a] : box.min()[a];
    q = target.relative(source, q);
    openvdb::Coord lo, hi;
    for (int a = 0; a < 3; ++a) {
      const double pad =
          std::ceil(std::max(o.exteriorWidth, o.interiorWidth)) + 16;
      if (!std::isfinite(q[a]) || q[a] - pad < double(INT32_MIN) + 8192 ||
          q[a] + pad > double(INT32_MAX) - 8192)
        throw std::invalid_argument(
            "rebuild projected target domain exceeds native coordinates");
      lo[a] = int32_t(std::floor(q[a] - pad));
      hi[a] = int32_t(std::ceil(q[a] + pad));
    }
    targetBox.expand(lo);
    targetBox.expand(hi);
  }
  const long double targetCells = cells(targetBox),
                    meshTriangles = 12 * sourceCells;
  const openvdb::CoordBBox sourceLocal(box.min() - sourceCenter,
                                       box.max() - sourceCenter),
      targetLocal(targetBox.min() - targetCenter,
                  targetBox.max() - targetCenter);
  if (targetCells + sourceCells > p.maxVoxelCount ||
      512 * (blocks(box, 8) + blocks(sourceLocal, 8) + blocks(targetBox, 8) +
             blocks(targetLocal, 8)) >
          p.maxVoxelCount)
    throw std::invalid_argument(
        "rebuild aggregate dense/leaf topology exceeds maxVoxelCount");
  if (meshTriangles > p.maxTriangles || meshTriangles > UINT32_MAX / 4)
    throw std::invalid_argument(
        "rebuild conservative source mesh exceeds maxTriangles/native indices");
  const long double work = sourceCells * 1024 + targetCells * 512 +
                           meshTriangles * 64 + validationWork;
  if (work > o.maxWork)
    throw std::invalid_argument(
        "rebuild mesh/dense/writer work exceeds maxWork");
  budget(
      loaded.resident +
          16 * (treeBytes(box) + treeBytes(sourceLocal) + treeBytes(targetBox) +
                treeBytes(targetLocal)) +
          meshTriangles * 512 + 16 * loaded.metadata + Fixed,
      p,
      "rebuild mesh/multiple-tree/writer reserve exceeds shared memory budget");
  for (int a = 0; a < 3; ++a) {
    const double s =
        std::max(std::abs(double(int64_t(box.min()[a]) - sourceCenter[a])),
                 std::abs(double(int64_t(box.max()[a]) - sourceCenter[a]))) +
        2;
    const double t =
        std::max(
            std::abs(double(int64_t(targetBox.min()[a]) - targetCenter[a])),
            std::abs(double(int64_t(targetBox.max()[a]) - targetCenter[a]))) +
        2;
    if (8 * (floatGap(s) * source.h / target.h + floatGap(t)) > 1e-4)
      throw std::invalid_argument(
          "native rebuild Float mesh positions cannot resolve 1e-4 target "
          "voxel; partition/reduce resolution");
  }
  // All dense and native mesh growth has been reserved before these probes.
  for (int a = 0; a < 3; ++a)
    for (int side = 0; side < 2; ++side) {
      auto face = box;
      face.min()[a] = face.max()[a] = side ? box.max()[a] : box.min()[a];
      visit(face, [&](const openvdb::Coord &c) {
        if (!(difference(input.tree().getValue(c)) > 0))
          throw std::invalid_argument("rebuild requires closed positive "
                                      "bounding faces; open surfaces reject");
      });
    }
  const T outside = T(std::abs(bg) / normalization);
  if (!(outside > 0))
    throw std::invalid_argument(
        "rebuild scalar normalization loses background sign");
  auto conditioned = Grid::create(outside);
  conditioned->setTransform(openvdb::math::Transform::createLinearTransform(1));
  conditioned->setGridClass(openvdb::GRID_LEVEL_SET);
  visit(box, [&](const openvdb::Coord &c) {
    T value;
    const bool active = input.tree().probeValue(c, value);
    const double d = difference(value), q = d / normalization;
    const T normalized = T(q);
    if (!std::isfinite(normalized) || (d != 0 && normalized == 0))
      throw std::invalid_argument(
          "rebuild scalar normalization loses a nonzero native gradient/value");
    const auto local = c - sourceCenter;
    if (active)
      conditioned->tree().setValue(local, normalized);
    else if (normalized != conditioned->background())
      conditioned->tree().setValueOff(local, normalized);
  });
  openvdb::Mat4d relative = openvdb::Mat4d::identity();
  for (int a = 0; a < 3; ++a) {
    const V column = source.indexVector(target.axis[a]);
    for (int c = 0; c < 3; ++c)
      relative[a][c] = column[c];
  }
  const V shift =
      source.relative(target, targetCenter.asVec3d()) - sourceCenter.asVec3d();
  for (int c = 0; c < 3; ++c)
    relative[3][c] = shift[c];
  auto relativeTransform = detail::MakeAffineTransformExact(relative);
  if (!same(*relativeTransform, *relativeTransform->copy()))
    throw std::invalid_argument("native rebuild copying changes conditioned "
                                "target map; resample/recenter target");
  const float ex = float(o.exteriorWidth), in = float(o.interiorWidth);
  auto native = openvdb::tools::levelSetRebuild(*conditioned, 0.f, ex, in,
                                                relativeTransform.get());
  const auto nativeBox = native->evalActiveVoxelBoundingBox();
  const openvdb::CoordBBox localTarget(targetBox.min() - targetCenter,
                                       targetBox.max() - targetCenter);
  if (nativeBox.empty() || !localTarget.isInside(nativeBox) ||
      native->memUsage() > 8 * treeBytes(targetLocal) ||
      native->tree().leafCount() * 512.0L > p.maxVoxelCount)
    throw std::runtime_error(
        "native rebuild exceeded reserved topology or produced no surface");
  auto result = Grid::create(T(double(native->background()) * source.h));
  result->insertMeta(input);
  detail::RemoveBoundaryProjectionMetadata(*result);
  result->setTransform(targetTransform);
  result->setName(p.gridName);
  result->setGridClass(openvdb::GRID_LEVEL_SET);
  result->setVectorType(openvdb::VEC_INVARIANT);
  result->setIsInWorldSpace(true);
  result->setSaveFloatAsHalf(false);
  for (auto it = native->cbeginValueAll(); it; ++it)
    if ((it.isValueOn() || *it < 0) &&
        !localTarget.isInside(it.getBoundingBox()))
      throw std::runtime_error(
          "native rebuild escaped bounded target interior");
  visit(targetBox, [&](const openvdb::Coord &c) {
    T value;
    const bool active = native->tree().probeValue(c - targetCenter, value);
    const double scaled = double(value) * source.h;
    const T world = T(scaled);
    if (!std::isfinite(world) || (value != 0 && world == 0))
      throw std::invalid_argument("rebuild world-distance restoration "
                                  "overflows/underflows native type");
    if (active)
      result->tree().setValue(c, world);
    else if (world != result->background())
      result->tree().setValueOff(c, world);
  });
  if (result->activeVoxelCount() > p.maxVoxelCount ||
      result->tree().leafCount() * 512.0L > p.maxVoxelCount)
    throw std::runtime_error("rebuilt output exceeded reserved topology");
  for (const auto *key : {openvdb::GridBase::META_FILE_BBOX_MIN,
                          openvdb::GridBase::META_FILE_BBOX_MAX,
                          openvdb::GridBase::META_FILE_MEM_BYTES,
                          openvdb::GridBase::META_FILE_VOXEL_COUNT,
                          "container:level_set_rebuild"})
    result->removeMeta(key);
  result->insertMeta(
      "container:level_set_rebuild",
      openvdb::StringMetadata(
          "native typed grid-mesh-grid; iso-shifted unit conditioning; "
          "approximate; bubbles/overlaps may close"));
  LevelSetRebuildResult report;
  report.grid = {p.gridName, "level set", result->activeVoxelCount(), target.h};
  report.sourceValueType = input.valueType();
  report.inputActiveVoxelCount = input.activeVoxelCount();
  report.meshTriangleUpperBound = uint64_t(meshTriangles);
  report.workUpperBound = uint64_t(work);
  report.isovalue = o.isovalue;
  report.effectiveExteriorWidth = ex;
  report.effectiveInteriorWidth = in;
  report.sourceIndexOrigin = {sourceCenter[0], sourceCenter[1],
                              sourceCenter[2]};
  report.targetIndexOrigin = {targetCenter[0], targetCenter[1],
                              targetCenter[2]};
  report.diagnostics = {
      "Native typed VolumeToMesh -> MeshToVolume rebuild; Float mesh positions "
      "checked to1e-4 target voxels; geometric/distance accuracy remains "
      "approximate.",
      "Isovalue-shifted scalar and source/target index conditioning; exact "
      "target native coefficients, native storage precision and world-distance "
      "units.",
      "Asymmetric widths are Float-rounded target-voxel widths; internal "
      "bubbles/overlaps may close or merge; not topology preserving.",
      "Dense source/target, worst mesh and multiple native scratch trees are "
      "conservatively reserved; no process-RSS or hostile-file guarantee."};
  budget(loaded.resident + 8 * result->memUsage() + 16 * loaded.metadata +
             Fixed,
         p, "rebuilt output/writer exceeds memory budget");
  const auto temporary = loaded.snapshot->directory / "result.vdb";
  struct Cleanup {
    std::filesystem::path p;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove(p, e);
    }
  } cleanup{temporary};
  openvdb::io::File writer(temporary.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(openvdb::GridPtrVec{std::make_shared<ExactWrite<Grid>>(*result)},
               *loaded.fileMetadata);
  writer.close();
  budget(loaded.resident + 8 * result->memUsage() +
             std::filesystem::file_size(temporary) + Fixed,
         p, "rebuilt encoded output exceeds memory budget");
  static_assert(std::is_nothrow_move_constructible_v<LevelSetRebuildResult>);
  publish(temporary, output);
  return report;
}
#endif
} // namespace
LevelSetRebuildResult WriteRebuiltLevelSet(const std::filesystem::path &input,
                                           const std::filesystem::path &output,
                                           const LevelSetRebuildOptions &o,
                                           const std::string &selection,
                                           const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if (!std::isfinite(o.isovalue) || !std::isfinite(o.exteriorWidth) ||
      !std::isfinite(o.interiorWidth) || o.exteriorWidth < 1 ||
      o.exteriorWidth > 64 || o.interiorWidth < 1 || o.interiorWidth > 64 ||
      !o.maxWork || !o.maxGrids || o.maxGrids > 4096 || !o.maxMetadataBytes ||
      !p.maxInputBytes || !p.maxVoxelCount || !p.maxTriangles ||
      p.gridName.empty() || p.gridName.size() > 256 ||
      p.gridName.find('\0') != std::string::npos)
    throw std::invalid_argument(
        "invalid rebuild isovalue/bands/name/catalog/budgets");
  if (output.empty() || std::filesystem::exists(output) ||
      !std::filesystem::is_directory(output.has_parent_path()
                                         ? output.parent_path()
                                         : std::filesystem::path(".")))
    throw std::invalid_argument(
        "rebuild output must be new in an existing directory");
  openvdb::initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto data = load(input, selection, o, p);
    if (data.grid->isType<openvdb::FloatGrid>())
      return rebuild(data, static_cast<const openvdb::FloatGrid &>(*data.grid),
                     output, o, p);
    return rebuild(data, static_cast<const openvdb::DoubleGrid &>(*data.grid),
                   output, o, p);
  });
#else
  (void)input;
  (void)output;
  (void)o;
  (void)selection;
  (void)p;
  throw std::runtime_error("OpenVDB level-set rebuild is unavailable; "
                           "configure with -DENABLE_OPENVDB=ON.");
#endif
}
} // namespace container::geometry::vdb
