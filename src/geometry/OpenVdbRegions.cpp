#include "Container/geometry/OpenVdbRegions.h"
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
#include <mutex>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/LevelSetUtil.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB regions are unavailable; configure with -DENABLE_OPENVDB=ON.");
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
void validate(RegionsOperation op, const RegionsOptions &o,
              const ProcessingOptions &p) {
  if (op != RegionsOperation::ActiveComponents &&
      op != RegionsOperation::SignedInterior &&
      op != RegionsOperation::EnclosedRegion &&
      op != RegionsOperation::Cavities)
    throw std::invalid_argument("invalid regions operation");
  if (!o.maxComponents || o.maxComponents > 1024 || !o.maxWorkItems ||
      o.maxWorkItems > 10'000'000'000ull || !std::isfinite(o.isovalue) ||
      !o.maxGrids || o.maxGrids > 4096 || !o.maxMetadataBytes ||
      !p.maxInputBytes || !p.maxVoxelCount || p.gridName.empty() ||
      p.gridName.size() > 256 || p.gridName.find('\0') != std::string::npos)
    throw std::invalid_argument(
        "invalid regions component/work/catalog/metadata/byte/voxel/name "
        "options");
  if ((op == RegionsOperation::ActiveComponents && o.isovalue != 0) ||
      (op != RegionsOperation::ActiveComponents && o.maxComponents != 64))
    throw std::invalid_argument(
        "isovalue applies only to signed regions; maxComponents applies only "
        "to active components");
}
long double metadata(const openvdb::MetaMap &m) {
  long double n = sizeof(m);
  for (auto it = m.beginMeta(); it != m.endMeta(); ++it) {
    if (!it->second)
      throw std::invalid_argument("null regions metadata");
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
      throw std::invalid_argument("regions operation exceeds maxWorkItems");
    used += uint64_t(std::ceil(n));
  }
};
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument("regions input must be a regular file");
    encoded = std::filesystem::file_size(input);
    bytes(Fixed + encoded, p, "encoded regions source exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned n = 0; n < 64; ++n) {
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-regions-" +
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
      throw std::runtime_error("cannot reserve regions snapshot");
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open regions snapshot");
      std::array<char, 65536> buffer{};
      auto left = encoded;
      while (left) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(left, buffer.size()));
        source.read(buffer.data(), count);
        if (source.gcount() != count)
          throw std::runtime_error("regions source changed length");
        target.write(buffer.data(), count);
        left -= count;
      }
      target.close();
      if (!target || source.bad() ||
          source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "regions snapshot failed or source changed length");
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
      "regions supports Float/Double/Bool/Mask/Int32/Int64/Vec3I/Vec3S/Vec3D; "
      "use point masks for PointDataGrid");
}
bool supported(const openvdb::GridBase &g) {
  return g.isType<openvdb::FloatGrid>() || g.isType<openvdb::DoubleGrid>() ||
         g.isType<openvdb::BoolGrid>() || g.isType<openvdb::MaskGrid>() ||
         g.isType<openvdb::Int32Grid>() || g.isType<openvdb::Int64Grid>() ||
         g.isType<openvdb::Vec3IGrid>() || g.isType<openvdb::Vec3SGrid>() ||
         g.isType<openvdb::Vec3DGrid>();
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
      throw std::invalid_argument(
          "regions require supported seekable metadata-readable VDB files");
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
          "regions grid descriptor offsets are outside the encoded range");
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
          "regions metadata/transform crosses grid payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};
Loaded load(const Snapshot &s, RegionsOperation op, const RegionsOptions &o,
            const std::string &selector, const ProcessingOptions &p) {
  detail::RequireSeekableFile(s.file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded regions require delayed loading; "
                                "unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded regions require OpenVDB delayed-loading support");
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
      throw std::invalid_argument(
          "aggregate regions file/grid metadata exceeds maxMetadataBytes");
    bytes(catalog + 8 * r.meta, p, "regions catalog exceeds memory budget");
  };
  check();
  std::set<std::string> descriptors;
  const int32_t gridCount = archive.count(stream);
  if (gridCount < 0 || uint32_t(gridCount) > o.maxGrids)
    throw std::invalid_argument("regions catalog exceeds maxGrids");
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
          "regions input has colliding descriptors; rename grids first");
    r.meta += metadata(*m);
    check();
    if (!GridSelectionMatches(selector, name, m->getName()))
      continue;
    const bool usable = op == RegionsOperation::ActiveComponents
                            ? supported(*m)
                            : signedType(*m);
    if (!usable) {
      if (!selector.empty())
        throw std::invalid_argument(
            "regions selected unsupported native type/class");
      continue;
    }
    if (found)
      throw std::invalid_argument(
          "ambiguous regions selector; use descriptor:<exact descriptor>");
    const auto mem = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES),
               count = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!mem || !count || mem->value() < 0 || count->value() < 0 ||
        uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument("regions needs valid recorded memory/voxel "
                                  "statistics within budgets");
    bytes(catalog + 8 * r.meta + mem->value(), p,
          "recorded regions tree exceeds memory budget");
    found = true;
    chosen = name;
  }
  if (!found)
    throw std::invalid_argument("no matching supported regions grid");
  stream.close();
  openvdb::io::File file(s.file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error("regions snapshot changed before decoded loading");
  r.grid = file.readGrid(chosen);
  if (r.grid->activeVoxelCount() > p.maxVoxelCount ||
      r.grid->baseTree().leafCount() >
          p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument(
        "regions source exceeds native active/leaf caps");
  const auto full = visit(*r.grid, [](const auto &g) {
    return openvdb::tools::memUsageIfLoaded(g.tree(), false);
  });
  r.resident = catalog + 8 * r.meta + full;
  bytes(r.resident, p,
        "fully decoded regions source exceeds memory budget before "
        "materialization");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full)
    throw std::runtime_error("regions tree exceeded decoded reservation");
  file.close();
  return r;
#endif
}
// Archive::writeGrid also calls Grid::copyGrid, which normally simplifies its
// map. Only its metadata copy is required: retain the original immutable map
// and shared tree through the SDK's exact replacement constructor instead.
class ExactWriteMask : public Mask {
public:
  explicit ExactWriteMask(const Mask &g)
      : Mask(openvdb::ConstPtrCast<Tree>(g.constTreePtr())) {
    insertMeta(g);
    setTransform(detail::CopyTransformExact(g.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return copyReplacingMetadataAndTransform(
        *this, detail::CopyTransformExact(transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactWriteMask &>(*this).copyGrid());
  }
};
double map(const openvdb::GridBase &g) {
  if (!g.transform().isLinear())
    throw std::invalid_argument(
        "regions require finite nonsingular affine maps");
  const auto m = g.transform().baseMap();
  const auto origin = m->applyMap(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d, 3> normalized;
  double voxelSize = 0;
  for (int a = 0; a < 3; ++a) {
    openvdb::Vec3d e(0);
    e[a] = 1;
    auto axis = m->applyJacobian(e);
    const double length = std::hypot(axis[0], axis[1], axis[2]);
    if (!std::isfinite(origin[a]) || !std::isfinite(length) || length <= 0)
      throw std::invalid_argument(
          "regions affine map is not finite/nonsingular");
    normalized[a] = axis / length;
    if (a == 0)
      voxelSize = length;
  }
  const double det = normalized[0].dot(normalized[1].cross(normalized[2]));
  if (!std::isfinite(det) || det == 0)
    throw std::invalid_argument("regions affine map is singular");
  return voxelSize;
}
int64_t block(int64_t x, int64_t d) { return x >= 0 ? x / d : (x + 1) / d - 1; }
long double blocks(const openvdb::CoordBBox &b, int64_t d) {
  long double n = 1;
  for (int a = 0; a < 3; ++a)
    n *= block(b.max()[a], d) - block(b.min()[a], d) + 1;
  return n;
}
long double treeBytes(const openvdb::CoordBBox &b) {
  return blocks(b, 8) * (sizeof(Leaf) + 512 + 256.0L) +
         blocks(b, Lower::DIM) * (sizeof(Lower) + 256.0L) +
         blocks(b, Upper::DIM) * (sizeof(Upper) + 512.0L);
}
struct Domain {
  openvdb::CoordBBox box;
  long double slots = 0, tree = 0;
};
Domain domain(const openvdb::GridBase &g, RegionsOperation op,
              const ProcessingOptions &p) {
  Domain d;
  d.box = g.evalActiveVoxelBoundingBox();
  if (op != RegionsOperation::ActiveComponents)
    visit(g, [&](const auto &grid) {
      for (auto v = grid.cbeginValueAll(); v; ++v)
        if (v.isValueOn() || *v != grid.background())
          d.box.expand(v.getBoundingBox());
      return 0;
    });
  if (d.box.empty())
    return d;
  for (int a = 0; a < 3; ++a)
    if (d.box.min()[a] < INT32_MIN + 4096LL ||
        d.box.max()[a] > INT32_MAX - 4096LL)
      throw std::invalid_argument(
          "regions coordinates lack safe native node/stencil margins");
  d.box.expand(1);
  d.slots = blocks(d.box, 8) * 512;
  d.tree = treeBytes(d.box);
  if (d.slots > p.maxVoxelCount || d.slots > UINT32_MAX / 4)
    throw std::invalid_argument(
        "regions full prospective domain exceeds voxel/leaf/index caps");
  return d;
}
void reserve(const Loaded &l, const Domain &d, uint64_t components,
             const ProcessingOptions &p) {
  // A leaf may contain 256 separate local segments that connect elsewhere.
  // Upstream reserves up to 256 connection pointers per local segment even
  // when there is only one global component; charge that independently.
  bytes(l.resident * 8 + (16 + 4 * components) * l.meta +
            components * (8192.0L + 8.0L * p.gridName.size()) +
            d.tree * (16 + components) +
            d.slots * (2048.0L + components * 16.0L) + Fixed,
        p,
        "regions full-domain/component/scratch/writer workspace exceeds memory "
        "budget");
}
template <class G> void signedValues(const G &g, double iso) {
  const double bg = g.background();
  using V = typename G::ValueType;
  if (!std::isfinite(bg) || bg <= 0 || std::abs(iso) >= bg ||
      double(V(iso)) != iso || g.tree().hasActiveTiles())
    throw std::invalid_argument(
        "signed regions need native-representable isovalue inside positive "
        "band and no active tiles");
  for (auto v = g.cbeginValueAll(); v; ++v) {
    const double x = *v;
    if (!std::isfinite(x) || std::abs(x) > bg ||
        (!v.isValueOn() && x != bg && x != -bg))
      throw std::invalid_argument("signed regions need finite symmetric-band "
                                  "distances and inactive signed background");
  }
}
Mask::Ptr occupancy(const openvdb::GridBase &g) {
  return visit(g, [](const auto &grid) {
    auto m = Mask::create(false);
    m->tree().topologyUnion(grid.tree());
    for (auto v = m->beginValueOn(); v; ++v)
      v.setValue(true);
    return m;
  });
}
std::vector<Mask::Ptr> components(Mask &mask, const RegionsOptions &o,
                                  const Loaded &l, const Domain &d,
                                  const ProcessingOptions &p, Work &w) {
  // Coordinate-set insertion/lookup costs are bounded for the admitted native
  // index range; reserve the whole prepass before tile expansion/allocation.
  w.add(d.slots * 256);
  mask.tree().voxelizeActiveTiles();
  // Bounded component prepass BEFORE upstream reserves total node-segment count
  // for EACH component. Uses coordinate sets instead of modifying native masks.
  std::set<openvdb::Coord> remaining;
  for (auto v = mask.cbeginValueOn(); v; ++v)
    remaining.insert(v.getCoord());
  uint64_t count = 0;
  std::vector<openvdb::Coord> queue;
  while (!remaining.empty()) {
    if (++count > o.maxComponents)
      throw std::invalid_argument(
          "regions active component count exceeds maxComponents");
    queue.clear();
    queue.push_back(*remaining.begin());
    remaining.erase(remaining.begin());
    for (size_t i = 0; i < queue.size(); ++i)
      for (int a = 0; a < 3; ++a)
        for (int sign : {-1, 1}) {
          auto c = queue[i];
          c[a] += sign;
          auto it = remaining.find(c);
          if (it != remaining.end()) {
            queue.push_back(c);
            remaining.erase(it);
          }
        }
  }
  if (d.slots * count > p.maxVoxelCount)
    throw std::invalid_argument("regions combined prospective component leaf "
                                "slots exceed voxel budget");
  reserve(l, d, count, p);
  w.add(d.slots * (1024.0L + count * 8.0L));
  std::vector<Mask::Ptr> out;
  openvdb::tools::extractActiveVoxelSegmentMasks(mask, out);
  if (out.size() != count)
    throw std::runtime_error(
        "native regions component count disagrees with bounded prepass");
  if (out.empty())
    out.push_back(Mask::create(false));
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
    const auto ac = a->activeVoxelCount(), bc = b->activeVoxelCount();
    if (ac != bc)
      return ac > bc;
    const auto ab = a->evalActiveVoxelBoundingBox(),
               bb = b->evalActiveVoxelBoundingBox();
    if (ab.min() != bb.min())
      return ab.min() < bb.min();
    return ab.max() < bb.max();
  });
  return out;
}
template <class G>
std::vector<Mask::Ptr> signedMasks(const G &g, RegionsOperation op, double iso,
                                   Work &w, const Domain &d) {
  signedValues(g, iso);
  w.add(d.slots * 32);
  // Tree overload avoids GridOrTreeConstructor's Transform::copy(), whose map
  // simplification can destroy a tiny scale against a large world origin.
  auto interior = Mask::create(
      openvdb::tools::sdfInteriorMask(g.tree(), typename G::ValueType(iso)));
  // Upstream starts from qualifying leaves, so standalone negative tiles can
  // be omitted, and it clamps the threshold by an absolute native tolerance.
  // Restore strict represented-value semantics over every stored partition.
  for (auto v = g.cbeginValueAll(); v; ++v)
    if (double(*v) < iso)
      interior->tree().fill(v.getBoundingBox(), true, true);
  if (op == RegionsOperation::SignedInterior)
    return {interior};
  // The native trace-to-convergence implementation has no work/interruption
  // hook and dereferences null boundary pointers for fully inactive leaves.
  // A finite, preflighted 6-neighbor flood has <=6N edge probes and preserves
  // every represented negative cell, including inactive tiles/remote branches.
  w.add(d.slots * 64);
  auto enclosed = Mask::create(false);
  if (d.box.empty())
    return {enclosed};
  const uint32_t nx = uint32_t(int64_t(d.box.max().x()) - d.box.min().x() + 1),
                 ny = uint32_t(int64_t(d.box.max().y()) - d.box.min().y() + 1),
                 nz = uint32_t(int64_t(d.box.max().z()) - d.box.min().z() + 1);
  const uint32_t yz = ny * nz, total = nx * yz;
  std::vector<uint8_t> state(total, 0);
  std::vector<uint32_t> queue;
  queue.reserve(total);
  auto ia = interior->getConstAccessor();
  for (uint32_t x = 0; x < nx; ++x)
    for (uint32_t y = 0; y < ny; ++y)
      for (uint32_t z = 0; z < nz; ++z) {
        const uint32_t i = (x * ny + y) * nz + z;
        const openvdb::Coord c(d.box.min().x() + int32_t(x),
                               d.box.min().y() + int32_t(y),
                               d.box.min().z() + int32_t(z));
        if (!ia.isValueOn(c)) {
          state[i] = 1;
          if (x == 0 || y == 0 || z == 0 || x + 1 == nx || y + 1 == ny ||
              z + 1 == nz) {
            state[i] = 2;
            queue.push_back(i);
          }
        }
      }
  const auto enter = [&](uint32_t i) {
    if (state[i] == 1) {
      state[i] = 2;
      queue.push_back(i);
    }
  };
  for (size_t head = 0; head < queue.size(); ++head) {
    const uint32_t i = queue[head], x = i / yz, y = (i % yz) / nz, z = i % nz;
    if (x)
      enter(i - yz);
    if (x + 1 < nx)
      enter(i + yz);
    if (y)
      enter(i - nz);
    if (y + 1 < ny)
      enter(i + nz);
    if (z)
      enter(i - 1);
    if (z + 1 < nz)
      enter(i + 1);
  }
  auto out = enclosed->getAccessor();
  for (uint32_t x = 0; x < nx; ++x)
    for (uint32_t y = 0; y < ny; ++y)
      for (uint32_t z = 0; z < nz; ++z) {
        const auto value = state[(x * ny + y) * nz + z];
        if (value == 1 ||
            (value == 0 && op == RegionsOperation::EnclosedRegion))
          out.setValue(openvdb::Coord(d.box.min().x() + int32_t(x),
                                      d.box.min().y() + int32_t(y),
                                      d.box.min().z() + int32_t(z)),
                       true);
      }
  return {enclosed};
}
void structure(Mask &g, const openvdb::GridBase &source,
               const std::string &name, RegionsOperation op) {
  g.insertMeta(source);
  detail::RemoveBoundaryProjectionMetadata(g);
  g.setTransform(detail::CopyTransformExact(source.transform()));
  for (const auto key :
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
        "container:pressure_gauge", "container:projection_dt",
        "container:pressure_units", "container:point_count",
        "container:point_empty_manifest_v1",
        "container:point_empty_descriptor_v1",
        "container:point_empty_leaf_flags_v1", "container:regions_operation",
        "container:topology_operation", "container:topology_source_grid",
        "container:topology_occupancy"})
    g.removeMeta(key);
  g.setName(name);
  g.setGridClass(openvdb::GRID_UNKNOWN);
  g.setVectorType(openvdb::VEC_INVARIANT);
  g.setIsInWorldSpace(true);
  g.setSaveFloatAsHalf(false);
  for (auto v = g.beginValueOn(); v; ++v)
    v.setValue(true);
  for (auto v = g.beginValueOff(); v; ++v)
    v.setValue(false);
  g.insertMeta("container:regions_operation",
               openvdb::StringMetadata(
                   op == RegionsOperation::ActiveComponents
                       ? "active_components"
                   : op == RegionsOperation::SignedInterior ? "signed_interior"
                   : op == RegionsOperation::EnclosedRegion ? "enclosed_region"
                                                            : "cavities"));
}
void publish(const Snapshot &s, const std::filesystem::path &output,
             const openvdb::GridPtrVec &grids, const openvdb::MetaMap &meta,
             const Loaded &l, const Domain &d, const ProcessingOptions &p) {
  const auto temp = s.directory / "result.vdb";
  if (std::filesystem::exists(temp))
    throw std::logic_error("regions private output already exists");
  openvdb::io::File file(temp.string());
  file.setCompression(openvdb::io::COMPRESS_ZIP);
  openvdb::GridPtrVec views;
  views.reserve(grids.size());
  for (const auto &g : grids)
    views.push_back(
        std::make_shared<ExactWriteMask>(*openvdb::gridPtrCast<Mask>(g)));
  file.write(views, meta);
  file.close();
  long double encoded = std::filesystem::file_size(temp);
  bytes(l.resident * 8 + 16 * l.meta + d.tree * 16 + encoded + Fixed, p,
        "regions encoded output and retained workspace exceed memory budget");
  bool created = false;
  std::ofstream target;
  try {
    std::ifstream source(temp, std::ios::binary);
    if (!source)
      throw std::runtime_error("cannot read regions private output");
    target.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    created = target.is_open();
    if (!created)
      throw std::runtime_error("cannot exclusively create regions output");
    target.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 65536> buffer{};
    while (source.read(buffer.data(), std::streamsize(buffer.size())) ||
           source.gcount())
      target.write(buffer.data(), source.gcount());
    if (!source.eof())
      throw std::runtime_error("regions private output read failed");
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
#endif
} // namespace

RegionsResult WriteRegions(const std::filesystem::path &input,
                           const std::filesystem::path &output,
                           RegionsOperation op, const RegionsOptions &o,
                           const std::string &selector,
                           const ProcessingOptions &p) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;
  (void)output;
  (void)op;
  (void)o;
  (void)selector;
  (void)p;
  unavailable();
#else
  validate(op, o, p);
#ifndef OPENVDB_USE_ZLIB
  throw std::invalid_argument(
      "regions require OpenVDB ZIP compression support");
#endif
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "regions output must be a new path in an existing directory");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Snapshot snapshot(input, p);
    auto loaded = load(snapshot, op, o, selector, p);
    const auto voxelSize = map(*loaded.grid);
    auto d = domain(*loaded.grid, op, p);
    reserve(loaded, d, 1, p);
    Work work{0, o.maxWorkItems};
    work.add(loaded.grid->memUsage() * 16.0L +
             loaded.grid->baseTree().leafCount() * 512.0L + 1);
    std::vector<Mask::Ptr> masks;
    if (op == RegionsOperation::ActiveComponents) {
      auto m = occupancy(*loaded.grid);
      masks = components(*m, o, loaded, d, p, work);
    } else if (loaded.grid->isType<openvdb::FloatGrid>())
      masks =
          signedMasks(*openvdb::gridPtrCast<openvdb::FloatGrid>(loaded.grid),
                      op, o.isovalue, work, d);
    else
      masks =
          signedMasks(*openvdb::gridPtrCast<openvdb::DoubleGrid>(loaded.grid),
                      op, o.isovalue, work, d);
    RegionsResult r;
    r.sourceActiveVoxelCount = loaded.grid->activeVoxelCount();
    r.workItems = work.used;
    openvdb::GridPtrVec outputGrids;
    long double actual = 0, storedSlots = 0;
    for (size_t n = 0; n < masks.size(); ++n) {
      std::ostringstream name;
      name.imbue(std::locale::classic());
      name << p.gridName;
      if (op == RegionsOperation::ActiveComponents)
        name << '_' << std::setfill('0') << std::setw(3) << n;
      structure(*masks[n], *loaded.grid, name.str(), op);
      const auto count = masks[n]->activeVoxelCount();
      if (count > p.maxVoxelCount - r.totalActiveVoxelCount)
        throw std::invalid_argument(
            "combined regions output exceeds voxel budget");
      r.totalActiveVoxelCount += count;
      storedSlots += masks[n]->tree().leafCount() * 512.0L;
      if (storedSlots > p.maxVoxelCount)
        throw std::invalid_argument(
            "combined regions stored output leaves exceed voxel budget");
      actual += masks[n]->memUsage() + metadata(*masks[n]);
      r.grids.push_back({name.str(), "unknown", count, voxelSize});
      outputGrids.push_back(masks[n]);
    }
    bytes(loaded.resident * 8 + 16 * loaded.meta + actual * 4 + Fixed, p,
          "summed regions output/metadata/writer exceeds memory budget");
    if (op != RegionsOperation::ActiveComponents)
      r.diagnostics.push_back(
          "Voxel-topological signed/enclosed masks; this does not certify "
          "source-mesh watertightness. Open passages remain exterior.");
    if (!r.totalActiveVoxelCount)
      r.diagnostics.push_back(
          op == RegionsOperation::Cavities
              ? "No topologically enclosed positive cavity was found; open "
                "regions remain exterior."
              : "No occupied regions found; wrote a valid empty mask.");
    publish(snapshot, output, outputGrids, *loaded.fileMeta, loaded, d, p);
    return r;
  });
#endif
}
} // namespace container::geometry::vdb
