#include "Container/geometry/OpenVdbVolumeSpheres.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/OpenVdbGridSelection.h"
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/VolumeToSpheres.h>
#include <set>
#include <tbb/task_arena.h>
#include <tuple>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB volume-to-spheres is unavailable; "
                           "configure with -DENABLE_OPENVDB=ON.");
}
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
void options(const VolumeSpheresOptions &o, const ProcessingOptions &p) {
  if (!p.maxVoxelCount || !p.maxInputBytes || !o.maxWork ||
      !o.maxMetadataBytes || !o.maxGrids || o.maxGrids > 4096 || !o.maxCount ||
      o.maxCount > 4096 || o.minCount > o.maxCount || !o.candidateCount ||
      o.candidateCount > 100000 || !std::isfinite(o.minRadiusWorld) ||
      !std::isfinite(o.maxRadiusWorld) || o.minRadiusWorld < 0 ||
      o.maxRadiusWorld <= 0 || o.minRadiusWorld > o.maxRadiusWorld ||
      (o.isovalue && !std::isfinite(*o.isovalue)))
    throw std::invalid_argument(
        "invalid sphere count/candidate/radius/isovalue/work/catalog/memory "
        "limits");
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
      throw std::invalid_argument("sphere-fitting input is not a regular file");
    encoded = std::filesystem::file_size(source);
    budget(encoded + Fixed, p,
           "sphere-fitting encoded input exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned n = 0; n < 64; ++n) {
      directory =
          std::filesystem::temp_directory_path() /
          ("container-volume-spheres-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(directory)) {
        file = directory / "source.vdb";
        break;
      }
    }
    if (file.empty())
      throw std::runtime_error("cannot create sphere-fitting private snapshot");
    try {
      std::ifstream input(source, std::ios::binary);
      std::ofstream output(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!input || !output)
        throw std::runtime_error("cannot open sphere-fitting snapshot");
      std::array<char, 65536> buffer{};
      auto remaining = encoded;
      while (remaining) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(remaining, buffer.size()));
        input.read(buffer.data(), count);
        if (input.gcount() != count)
          throw std::runtime_error(
              "sphere-fitting input changed while snapshotting");
        output.write(buffer.data(), count);
        remaining -= count;
      }
      output.close();
      if (!output || input.bad() ||
          input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "sphere-fitting snapshot failed or input changed size");
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
Loaded load(const std::filesystem::path &input, const std::string &selector,
            const VolumeSpheresOptions &o, const ProcessingOptions &p) {
  Loaded r;
  r.snapshot = std::make_unique<Snapshot>(input, p);
  detail::RequireSeekableFile(r.snapshot->file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded sphere fitting requires delayed "
                                "loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded sphere fitting requires OpenVDB delayed-loading support");
#else
  openvdb::io::File f(r.snapshot->file.string());
  // The immutable private snapshot outlives native materialization and fitting.
  f.setCopyMaxBytes(0);
  f.open(true);
  r.fileMetadata = f.getMetadata();
  r.metadata = metaBytes(*r.fileMetadata);
  if (r.metadata > o.maxMetadataBytes)
    throw std::invalid_argument(
        "sphere-fitting file metadata exceeds maxMetadataBytes");
  std::set<std::string> names;
  long double catalog = r.snapshot->encoded + Fixed;
  budget(catalog + 8 * r.metadata, p,
         "sphere-fitting file metadata exceeds memory budget");
  for (auto i = f.beginName(); i != f.endName(); ++i) {
    if (names.size() >= o.maxGrids)
      throw std::invalid_argument("sphere-fitting catalog exceeds maxGrids");
    const auto n = i.gridName();
    catalog += 4.0L * n.size() + 256;
    budget(catalog + 8 * r.metadata, p,
           "sphere-fitting catalog exceeds memory budget");
    if (!names.insert(n).second)
      throw std::invalid_argument(
          "sphere-fitting input has colliding descriptors; rename grids first");
  }
  std::string chosen;
  bool found = false;
  for (auto i = f.beginName(); i != f.endName(); ++i) {
    const auto m = f.readGridMetadata(i.gridName());
    const auto bytes = metaBytes(*m);
    r.metadata += bytes;
    if (r.metadata > o.maxMetadataBytes)
      throw std::invalid_argument("aggregate sphere-fitting file/grid metadata "
                                  "exceeds maxMetadataBytes");
    budget(catalog + 8 * r.metadata, p,
           "sphere-fitting catalog metadata exceeds memory budget");
    if ((!m->isType<openvdb::FloatGrid>() &&
         !m->isType<openvdb::DoubleGrid>()) ||
        (m->getGridClass() != openvdb::GRID_LEVEL_SET &&
         m->getGridClass() != openvdb::GRID_FOG_VOLUME) ||
        !GridSelectionMatches(selector, i.gridName(), m->getName()))
      continue;
    if (found)
      throw std::invalid_argument(
          "sphere-fitting selector is ambiguous; use descriptor:<exact name>");
    const auto mem = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES),
               voxels = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!mem || !voxels || mem->value() < 0 || voxels->value() < 0 ||
        uint64_t(voxels->value()) > p.maxVoxelCount)
      throw std::invalid_argument("sphere fitting requires valid recorded "
                                  "memory/voxel statistics within budgets");
    budget(catalog + mem->value() + 8 * r.metadata, p,
           "recorded sphere-fitting source exceeds memory budget");
    chosen = i.gridName();
    found = true;
    if (selector.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument(
        "no matching Float/Double level set or fog found for sphere fitting");
  r.grid = f.readGrid(chosen);
  if (r.grid->activeVoxelCount() > p.maxVoxelCount ||
      r.grid->baseTree().leafCount() >
          p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument("sphere-fitting native leaf/voxel topology "
                                "exceeds budget before materialization");
  const auto full =
      r.grid->isType<openvdb::FloatGrid>()
          ? openvdb::tools::memUsageIfLoaded(
                openvdb::gridPtrCast<openvdb::FloatGrid>(r.grid)->tree(), false)
          : openvdb::tools::memUsageIfLoaded(
                openvdb::gridPtrCast<openvdb::DoubleGrid>(r.grid)->tree(),
                false);
  r.resident = catalog + full + 8 * r.metadata;
  budget(r.resident, p,
         "fully loaded sphere-fitting tree exceeds memory budget before leaf "
         "materialization");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full)
    throw std::runtime_error(
        "sphere-fitting tree exceeded full decoded memory estimate");
  f.close();
  return r;
#endif
}
int64_t block(int64_t x, int64_t d) { return x >= 0 ? x / d : (x + 1) / d - 1; }
long double blocks(const openvdb::CoordBBox &b, int64_t d) {
  long double n = 1;
  for (int a = 0; a < 3; ++a)
    n *= block(b.max()[a], d) - block(b.min()[a], d) + 1;
  return n;
}
long double treeBytes(const openvdb::CoordBBox &b) {
  return blocks(b, 8) * (sizeof(Leaf) + 512 * sizeof(double) + 256.0L) +
         blocks(b, Lower::DIM) * (sizeof(Lower) + 256.0L) +
         blocks(b, Upper::DIM) * (sizeof(Upper) + 512.0L);
}
struct Map {
  V origin;
  std::array<V, 3> basis;
  double dx = 0;
  explicit Map(const openvdb::GridBase &grid) {
    if (!grid.transform().isLinear())
      throw std::invalid_argument(
          "sphere fitting requires uniform orthogonal affine voxels");
    const auto m = grid.transform().baseMap();
    origin = m->applyMap(V(0));
    for (int a = 0; a < 3; ++a) {
      V e(0);
      e[a] = 1;
      basis[a] = m->applyJacobian(e);
      const double length = std::hypot(basis[a][0], basis[a][1], basis[a][2]);
      if (!std::isfinite(origin[a]) || !std::isfinite(length) ||
          length < 1e-6 || length > 1e6)
        throw std::invalid_argument(
            "sphere-fitting affine axes must be finite in [1e-6,1e6]");
      if (a == 0)
        dx = length;
      else if (std::abs(length / dx - 1) > 1e-8)
        throw std::invalid_argument("sphere fitting requires uniform "
                                    "orthogonal voxels; resample first");
      for (int j = 0; j < a; ++j)
        if (std::abs(basis[a].dot(basis[j])) > dx * dx * 1e-8)
          throw std::invalid_argument("sphere fitting requires uniform "
                                      "orthogonal voxels; resample first");
    }
  }
  V world(const V &local, const openvdb::Coord &offset) const {
    V out;
    for (int row = 0; row < 3; ++row) {
      double sum = origin[row], error = 0;
      const auto add = [&](double x) {
        const double next = sum + x;
        error +=
            std::abs(sum) >= std::abs(x) ? (sum - next) + x : (x - next) + sum;
        sum = next;
      };
      for (int a = 0; a < 3; ++a)
        for (double x : {double(offset[a]), local[a]}) {
          const double product = basis[a][row] * x;
          add(product);
          add(std::fma(basis[a][row], x, -product));
        }
      out[row] = sum + error;
      const double gap =
          std::max(std::abs(std::nextafter(out[row], INFINITY) - out[row]),
                   std::abs(out[row] - std::nextafter(out[row], -INFINITY)));
      if (!std::isfinite(out[row]) || gap > dx * 1e-4)
        throw std::invalid_argument(
            "sphere world center cannot retain source voxel precision; "
            "recenter source transform");
    }
    return out;
  }
};
struct Domain {
  openvdb::Coord offset;
  openvdb::CoordBBox local;
  double iso;
  bool fog;
};
template <class G>
Domain validate(const G &grid, const Map &map, const VolumeSpheresOptions &o,
                const ProcessingOptions &p) {
  const bool fog = grid.getGridClass() == openvdb::GRID_FOG_VOLUME;
  const double bg = grid.background(), iso = o.isovalue.value_or(fog ? .5 : 0);
  if (!std::isfinite(bg) || (fog ? bg != 0 : bg <= 0))
    throw std::invalid_argument("sphere fitting requires zero-background fog "
                                "or positive-background SDF");
  if (fog && (iso < 1e-4 || iso > 1 - 1e-4))
    throw std::invalid_argument(
        "fog sphere-fitting isovalue must be in [0.0001,0.9999]");
  const double band = bg / map.dx;
  if (!fog && (!std::isfinite(band) || band < 3 - 1e-6 || band > 64 + 1e-6 ||
               std::abs(iso) >= bg - map.dx * 1e-4))
    throw std::invalid_argument("sphere fitting requires SDF half width [3,64] "
                                "and isovalue strictly inside band");
  if (grid.empty() || (!fog && grid.tree().hasActiveTiles()))
    throw std::invalid_argument("sphere fitting requires nonempty active "
                                "voxels (SDF active tiles are unsupported)");
  const uint64_t leaves = p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0);
  if (grid.activeVoxelCount() > p.maxVoxelCount ||
      grid.tree().leafCount() > leaves)
    throw std::invalid_argument(
        "sphere-fitting source exceeds voxel/leaf budget");
  auto box = grid.evalActiveVoxelBoundingBox();
  for (auto v = grid.cbeginValueAll(); v; ++v) {
    const double x = *v;
    if (!std::isfinite(x) ||
        (fog ? (x < 0 || x > 1 || (!v.isValueOn() && x != 0))
             : (std::abs(x) > bg * (1 + 1e-6) ||
                (!v.isValueOn() && std::abs(std::abs(x) - bg) > bg * 1e-6))))
      throw std::invalid_argument(
          "sphere source requires finite [0,1] fog/inactive zero or symmetric "
          "narrow-band SDF values");
    if (x != bg)
      box.expand(v.getBoundingBox());
  }
  Domain d;
  d.iso = iso;
  d.fog = fog;
  for (int a = 0; a < 3; ++a) {
    const int64_t low = box.min()[a], high = box.max()[a];
    if (low < INT32_MIN + 32LL || high > INT32_MAX - 32LL)
      throw std::invalid_argument(
          "sphere-fitting source coordinates lack safe native stencil halo");
    d.offset[a] = int32_t(low + (high - low) / 2);
  }
  d.local = box;
  d.local.translate(-d.offset);
  d.local.expand(8);
  return d;
}
long double preflight(const openvdb::CoordBBox &box, double scale,
                      uint64_t candidates, const VolumeSpheresOptions &o,
                      const ProcessingOptions &p, long double resident,
                      bool fitting, long double &reservedWork) {
  openvdb::Coord lo, hi;
  for (int a = 0; a < 3; ++a) {
    const double low = std::floor(double(box.min()[a]) / scale) - 8,
                 high = std::ceil(double(box.max()[a]) / scale) + 8;
    if (!std::isfinite(low) || !std::isfinite(high) || low < INT32_MIN + 16.0 ||
        high > INT32_MAX - 16.0)
      throw std::invalid_argument(
          "adaptive sphere-fitting coordinates exceed native range");
    lo[a] = int32_t(low);
    hi[a] = int32_t(high);
  }
  const openvdb::CoordBBox domain(lo, hi);
  const auto n = blocks(domain, 1), leaves = blocks(domain, 8);
  if (n > p.maxVoxelCount ||
      leaves > p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0) ||
      n >= double(UINT32_MAX) / 8)
    throw std::invalid_argument(
        "sphere-fitting padded/adaptive domain exceeds voxel/leaf/index "
        "budgets; refine limits or reduce domain");
  const long double work =
      n * (fitting ? 8.0L * candidates + 128 : 128) +
      static_cast<long double>(o.maxCount) * candidates * 64;
  if (!std::isfinite(work) || reservedWork + work > o.maxWork)
    throw std::invalid_argument(
        "sphere-fitting conservative candidate/surface work exceeds maxWork");
  const long double bytes = resident + 12 * treeBytes(domain) + n * 512 +
                            candidates * 512.0L + o.maxCount * 1024.0L + Fixed;
  budget(bytes, p,
         "sphere-fitting full-domain trees/mesh/candidate workspace exceeds "
         "memory budget");
  // The normalization/fog conversion and adaptive fitting are both executed.
  // Each stage reserves work once; the cap is shared across the entire call.
  reservedWork += work;
  return bytes;
}
double unitDistance(double x, double voxelSize) {
  const double value = x / voxelSize;
  if (!std::isfinite(value) || (x != 0 && value == 0))
    throw std::invalid_argument(
        "sphere-fitting distance normalization overflows or underflows Double");
  return value;
}
template <class G>
openvdb::DoubleGrid::Ptr normalize(const G &source, const Domain &d,
                                   const Map &map) {
  auto grid = openvdb::DoubleGrid::create(
      d.fog ? 0 : unitDistance(double(source.background()), map.dx));
  grid->setGridClass(source.getGridClass());
  grid->setTransform(openvdb::math::Transform::createLinearTransform(1));
  for (auto v = source.cbeginValueAll(); v; ++v) {
    if (v.isVoxelValue() || *v == source.background())
      continue;
    auto b = v.getBoundingBox();
    b.translate(-d.offset);
    grid->tree().fill(b, d.fog ? double(*v) : unitDistance(double(*v), map.dx),
                      v.isValueOn());
  }
  auto a = grid->getAccessor();
  for (auto v = source.cbeginValueAll(); v; ++v) {
    if (!v.isVoxelValue() || (!v.isValueOn() && *v == source.background()))
      continue;
    const auto c = v.getCoord() - d.offset;
    const double x = d.fog ? double(*v) : unitDistance(double(*v), map.dx);
    if (v.isValueOn())
      a.setValue(c, x);
    else
      a.setValueOff(c, x);
  }
  return grid;
}
bool crossing(const openvdb::DoubleGrid &grid, double iso) {
  const auto a = grid.getConstAccessor();
  for (auto v = grid.cbeginValueOn(); v; ++v)
    for (int k = 0; k < 3; ++k) {
      auto n = v.getCoord();
      ++n[k];
      const double x = *v, y = a.getValue(n);
      if ((x < iso && y >= iso) || (x >= iso && y < iso))
        return true;
    }
  return false;
}
std::vector<VolumeSphereMetadata> metadata(const openvdb::MetaMap &map) {
  std::vector<VolumeSphereMetadata> out;
  for (auto i = map.beginMeta(); i != map.endMeta(); ++i) {
    const auto &m = *i->second;
    const bool known = dynamic_cast<const openvdb::StringMetadata *>(&m) ||
                       dynamic_cast<const openvdb::BoolMetadata *>(&m) ||
                       dynamic_cast<const openvdb::Int32Metadata *>(&m) ||
                       dynamic_cast<const openvdb::Int64Metadata *>(&m) ||
                       dynamic_cast<const openvdb::FloatMetadata *>(&m) ||
                       dynamic_cast<const openvdb::DoubleMetadata *>(&m) ||
                       dynamic_cast<const openvdb::Vec3SMetadata *>(&m) ||
                       dynamic_cast<const openvdb::Vec3DMetadata *>(&m);
    out.push_back({i->first, m.typeName(), known ? m.str() : std::string{},
                   m.size(), !known});
  }
  return out;
}
template <class G>
VolumeSpheresResult fit(const G &source, const Loaded &loaded,
                        const VolumeSpheresOptions &o,
                        const ProcessingOptions &p) {
  const Map map(source);
  const auto domain = validate(source, map, o, p);
  for (int corner = 0; corner < 8; ++corner) {
    V local;
    for (int axis = 0; axis < 3; ++axis)
      local[axis] = (corner & (1 << axis)) ? domain.local.max()[axis]
                                           : domain.local.min()[axis];
    (void)map.world(local, domain.offset);
  }
  const uint64_t candidates = std::max(o.candidateCount, o.maxCount);
  const auto reserve =
      loaded.resident + 16 * loaded.metadata + o.maxCount * 1024.0L;
  long double reservedWork = 0;
  preflight(domain.local, 1, candidates, o, p, reserve, false, reservedWork);
  auto local = normalize(source, domain, map);
  if (!crossing(*local,
                domain.fog ? domain.iso : unitDistance(domain.iso, map.dx)))
    throw std::invalid_argument(
        "sphere fitting requires a resolved finite closed isosurface crossing");
  double iso = domain.fog ? domain.iso : unitDistance(domain.iso, map.dx);
  const float nativeIso = float(iso);
  if (!std::isfinite(nativeIso) || (iso != 0 && nativeIso == 0) ||
      std::abs(double(nativeIso) - iso) > 1e-6 * std::max(1., std::abs(iso)))
    throw std::invalid_argument(
        "sphere-fitting isovalue loses native Float precision");
  if (domain.fog) {
    const auto rebuilt = openvdb::tools::levelSetRebuild(
        *local, nativeIso, 3, 3, &local->transform());
    if (!rebuilt || rebuilt->empty())
      throw std::invalid_argument(
          "fog sphere-fitting rebuild produced an empty surface");
    local = rebuilt;
    iso = 0;
  }
  const uint64_t active = local->activeVoxelCount();
  if (!active)
    throw std::invalid_argument("sphere-fitting conditioned surface is empty");
  auto box = local->evalActiveVoxelBoundingBox();
  for (auto leaf = local->tree().cbeginLeaf(); leaf; ++leaf)
    box.expand(leaf->getNodeBoundingBox());
  const double scale =
      active < 10000 ? 1 / std::cbrt(20000. / double(active)) : 1;
  preflight(box, scale, candidates, o, p, reserve + 3.0L * local->memUsage(),
            true, reservedWork);
  // Float surface points and squared distances are used inside
  // ClosestSurfacePoint. A centered domain makes the range limit depend on
  // object extent, not origin.
  for (int a = 0; a < 3; ++a)
    for (double x :
         {double(box.min()[a]) / scale, double(box.max()[a]) / scale}) {
      const float f = float(x);
      const double spacing = std::abs(double(std::nextafter(f, INFINITY)) - f);
      if (!std::isfinite(f) || spacing * scale > 1e-4)
        throw std::invalid_argument(
            "sphere-fitting object extent exceeds native Float voxel "
            "precision; partition the volume");
    }
  std::vector<openvdb::Vec4s> spheres;
  openvdb::tools::fillWithSpheres(
      *local, spheres, openvdb::Vec2i(0, int(o.maxCount)), o.overlapping, 0,
      std::numeric_limits<float>::max() / 2, float(iso), int(candidates));
  VolumeSpheresResult result;
  result.gridName = source.getName();
  result.valueType = source.valueType();
  result.voxelSize = map.dx;
  result.isovalue = domain.iso;
  result.activeVoxelCount = source.activeVoxelCount();
  result.metadata = metadata(source);
  result.fileMetadata = metadata(*loaded.fileMetadata);
  result.diagnostic =
      "Approximate OpenVDB12 seed0 fitting; native Float geometry, optional "
      "adaptive rebuild, radius post-clamp/filter. No exact containment, "
      "optimal packing or coverage guarantee. Source unchanged.";
  result.spheres.reserve(spheres.size());
  for (const auto &s : spheres) {
    if (!std::isfinite(s[0]) || !std::isfinite(s[1]) || !std::isfinite(s[2]) ||
        !std::isfinite(s[3]) || s[3] < 0)
      throw std::invalid_argument(
          "native sphere fitting returned invalid geometry");
    const double nativeWorldRadius = double(s[3]) * map.dx;
    if (!std::isfinite(nativeWorldRadius) ||
        (s[3] != 0 && nativeWorldRadius == 0))
      throw std::invalid_argument(
          "sphere world radius overflows or underflows Double");
    const double radius = std::min(o.maxRadiusWorld, nativeWorldRadius);
    if (radius <= 0 || radius < o.minRadiusWorld)
      continue;
    const auto world = map.world(V(s[0], s[1], s[2]), domain.offset);
    if (!std::isfinite(radius))
      throw std::invalid_argument("sphere world radius overflows");
    result.spheres.push_back({{world[0], world[1], world[2]}, radius});
  }
  std::sort(result.spheres.begin(), result.spheres.end(),
            [](const auto &a, const auto &b) {
              if (a.radius != b.radius)
                return a.radius > b.radius;
              return a.position < b.position;
            });
  if (result.spheres.size() < o.minCount)
    throw std::invalid_argument(
        "sphere fitting could not meet minCount after world-radius filtering; "
        "lower count/radius or increase candidates");
  return result;
}
VolumeSpheresResult run(const std::filesystem::path &input,
                        const VolumeSpheresOptions &o,
                        const std::string &selector,
                        const ProcessingOptions &p) {
  options(o, p);
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto loaded = load(input, selector, o, p);
    if (loaded.grid->isType<openvdb::FloatGrid>())
      return fit(*openvdb::gridPtrCast<openvdb::FloatGrid>(loaded.grid), loaded,
                 o, p);
    return fit(*openvdb::gridPtrCast<openvdb::DoubleGrid>(loaded.grid), loaded,
               o, p);
  });
}
#endif
} // namespace

VolumeSpheresResult FitVolumeSpheres(const std::filesystem::path &input,
                                     const VolumeSpheresOptions &o,
                                     const std::string &selector,
                                     const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return run(input, o, selector, p);
#else
  (void)input;
  (void)o;
  (void)selector;
  (void)p;
  unavailable();
#endif
}
VolumeSpheresResult ExportVolumeSpheres(const std::filesystem::path &input,
                                        const std::filesystem::path &output,
                                        const VolumeSpheresOptions &o,
                                        const std::string &selector,
                                        const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(o, p);
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "sphere JSON output must be a new path in an existing directory");
  auto result = run(input, o, selector, p);
  budget(Fixed + 16.0L * o.maxMetadataBytes + result.spheres.size() * 4096.0L,
         p, "sphere JSON result/encoding exceeds memory budget");
  nlohmann::json metadata = nlohmann::json::array(),
                 fileMetadata = nlohmann::json::array(),
                 spheres = nlohmann::json::array();
  for (const auto &m : result.metadata)
    metadata.push_back({{"name", m.name},
                        {"type", m.type},
                        {"value", m.value},
                        {"nativeBytes", m.nativeBytes},
                        {"opaque", m.opaque}});
  for (const auto &m : result.fileMetadata)
    fileMetadata.push_back({{"name", m.name},
                            {"type", m.type},
                            {"value", m.value},
                            {"nativeBytes", m.nativeBytes},
                            {"opaque", m.opaque}});
  for (const auto &s : result.spheres)
    spheres.push_back({{"position", s.position}, {"radius", s.radius}});
  const nlohmann::json document = {
      {"schemaVersion", 1},
      {"source",
       {{"gridName", result.gridName},
        {"valueType", result.valueType},
        {"voxelSize", result.voxelSize},
        {"isovalue", result.isovalue},
        {"activeVoxelCount", result.activeVoxelCount},
        {"metadata", std::move(metadata)},
        {"fileMetadata", std::move(fileMetadata)}}},
      {"diagnostic", result.diagnostic},
      {"spheres", std::move(spheres)}};
  const auto encoded = document.dump(2);
  if (encoded.size() >= p.maxInputBytes)
    throw std::invalid_argument(
        "sphere JSON output exceeds encoded byte budget");
  std::ofstream file(output,
                     std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!file)
    throw std::runtime_error("could not exclusively create sphere JSON output");
  try {
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(encoded.data(), std::streamsize(encoded.size()));
    file.put('\n');
    file.close();
  } catch (...) {
    file.exceptions(std::ios::goodbit);
    if (file.is_open())
      file.close();
    std::error_code e;
    std::filesystem::remove(output, e);
    throw;
  }
  return result;
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
