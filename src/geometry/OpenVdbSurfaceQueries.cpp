#include "Container/geometry/OpenVdbSurfaceQueries.h"
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
#include <openvdb/math/Stencils.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/VolumeToSpheres.h>
#include <set>
#include <tbb/task_arena.h>
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB surface queries are unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
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
      throw std::invalid_argument("surface-query input is not a regular file");
    encoded = std::filesystem::file_size(source);
    budget(encoded + Fixed, p,
           "surface-query encoded input exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned n = 0; n < 64; ++n) {
      directory =
          std::filesystem::temp_directory_path() /
          ("container-surface-queries-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(directory)) {
        file = directory / "source.vdb";
        break;
      }
    }
    if (file.empty())
      throw std::runtime_error("cannot create surface-query private snapshot");
    try {
      std::ifstream input(source, std::ios::binary);
      std::ofstream output(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!input || !output)
        throw std::runtime_error("cannot open surface-query snapshot");
      std::array<char, 65536> buffer{};
      auto remaining = encoded;
      while (remaining) {
        const auto count =
            std::streamsize(std::min<uintmax_t>(remaining, buffer.size()));
        input.read(buffer.data(), count);
        if (input.gcount() != count)
          throw std::runtime_error(
              "surface-query input changed while snapshotting");
        output.write(buffer.data(), count);
        remaining -= count;
      }
      output.close();
      if (!output || input.bad() ||
          input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "surface-query snapshot failed or input changed size");
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
            const SurfaceQueryOptions &o, const ProcessingOptions &p) {
  Loaded r;
  r.snapshot = std::make_unique<Snapshot>(input, p);
  detail::RequireSeekableFile(r.snapshot->file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded surface queries requires delayed "
                                "loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded surface queries requires OpenVDB delayed-loading support");
#else
  openvdb::io::File f(r.snapshot->file.string());
  // The immutable private snapshot outlives native materialization and fitting.
  f.setCopyMaxBytes(0);
  f.open(true);
  r.fileMetadata = f.getMetadata();
  r.metadata = metaBytes(*r.fileMetadata);
  if (r.metadata > o.maxMetadataBytes)
    throw std::invalid_argument(
        "surface-query file metadata exceeds maxMetadataBytes");
  std::set<std::string> names;
  long double catalog = r.snapshot->encoded + Fixed;
  budget(catalog + 8 * r.metadata, p,
         "surface-query file metadata exceeds memory budget");
  for (auto i = f.beginName(); i != f.endName(); ++i) {
    if (names.size() >= o.maxGrids)
      throw std::invalid_argument("surface-query catalog exceeds maxGrids");
    const auto n = i.gridName();
    catalog += 4.0L * n.size() + 256;
    budget(catalog + 8 * r.metadata, p,
           "surface-query catalog exceeds memory budget");
    if (!names.insert(n).second)
      throw std::invalid_argument(
          "surface-query input has colliding descriptors; rename grids first");
  }
  std::string chosen;
  bool found = false;
  for (auto i = f.beginName(); i != f.endName(); ++i) {
    const auto m = f.readGridMetadata(i.gridName());
    const auto bytes = metaBytes(*m);
    r.metadata += bytes;
    if (r.metadata > o.maxMetadataBytes)
      throw std::invalid_argument("aggregate surface-query file/grid metadata "
                                  "exceeds maxMetadataBytes");
    budget(catalog + 8 * r.metadata, p,
           "surface-query catalog metadata exceeds memory budget");
    if ((!m->isType<openvdb::FloatGrid>() &&
         !m->isType<openvdb::DoubleGrid>()) ||
        (m->getGridClass() != openvdb::GRID_LEVEL_SET) ||
        !GridSelectionMatches(selector, i.gridName(), m->getName()))
      continue;
    if (found)
      throw std::invalid_argument(
          "surface-query selector is ambiguous; use descriptor:<exact name>");
    const auto mem = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES),
               voxels = m->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!mem || !voxels || mem->value() < 0 || voxels->value() < 0 ||
        uint64_t(voxels->value()) > p.maxVoxelCount)
      throw std::invalid_argument("surface queries requires valid recorded "
                                  "memory/voxel statistics within budgets");
    if (uint64_t(voxels->value()) > o.maxWork)
      throw std::invalid_argument(
          "recorded surface-query validation exceeds maxWork");
    budget(catalog + mem->value() + 8 * r.metadata, p,
           "recorded surface-query source exceeds memory budget");
    chosen = i.gridName();
    found = true;
    if (selector.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument(
        "no matching Float/Double level set found for surface queries");
  r.grid = f.readGrid(chosen);
  if (r.grid->activeVoxelCount() > p.maxVoxelCount ||
      r.grid->baseTree().leafCount() >
          p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument("surface-query native leaf/voxel topology "
                                "exceeds budget before materialization");
  if (r.grid->baseTree().leafCount() > o.maxWork / 1024)
    throw std::invalid_argument("surface-query native validation work exceeds "
                                "maxWork before materialization");
  const auto full =
      r.grid->isType<openvdb::FloatGrid>()
          ? openvdb::tools::memUsageIfLoaded(
                openvdb::gridPtrCast<openvdb::FloatGrid>(r.grid)->tree(), false)
          : openvdb::tools::memUsageIfLoaded(
                openvdb::gridPtrCast<openvdb::DoubleGrid>(r.grid)->tree(),
                false);
  r.resident = catalog + full + 8 * r.metadata;
  budget(r.resident, p,
         "fully loaded surface-query tree exceeds memory budget before leaf "
         "materialization");
  r.grid->readNonresidentBuffers();
  if (r.grid->memUsage() > full)
    throw std::runtime_error(
        "surface-query tree exceeded full decoded memory estimate");
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
          "surface queries requires uniform orthogonal affine voxels");
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
            "surface-query affine axes must be finite in [1e-6,1e6]");
      if (a == 0)
        dx = length;
      else if (std::abs(length / dx - 1) > 1e-8)
        throw std::invalid_argument("surface queries requires uniform "
                                    "orthogonal voxels; resample first");
      for (int j = 0; j < a; ++j)
        if (std::abs(basis[a].dot(basis[j])) > dx * dx * 1e-8)
          throw std::invalid_argument("surface queries requires uniform "
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
        throw std::invalid_argument("surface-query world position cannot "
                                    "retain source voxel precision; "
                                    "recenter source transform");
    }
    return out;
  }
};

struct Domain {
  openvdb::Coord offset;
  openvdb::CoordBBox local;
};
double units(double value, double factor) {
  const double out = value / factor;
  if (!std::isfinite(out) || (value != 0 && out == 0))
    throw std::invalid_argument(
        "surface-query normalization/restoration loses Double range");
  return out;
}
template <class G>
Domain validate(const G &source, const Map &map, const ProcessingOptions &p) {
  const double bg = source.background(), band = bg / map.dx;
  if (source.getGridClass() != openvdb::GRID_LEVEL_SET ||
      !source.isInWorldSpace() ||
      source.getVectorType() != openvdb::VEC_INVARIANT || source.empty() ||
      source.tree().hasActiveTiles() || !std::isfinite(bg) || bg <= 0 ||
      band < 3 - 1e-6 || band > 64 + 1e-6)
    throw std::invalid_argument(
        "surface queries require nonempty voxel-only symmetric narrow-band "
        "LEVEL_SET (halfwidth3..64)");
  auto box = source.evalActiveVoxelBoundingBox();
  for (auto v = source.cbeginValueAll(); v; ++v) {
    const double x = *v;
    if (!std::isfinite(x) || std::abs(x) > bg * (1 + 1e-6) ||
        (!v.isValueOn() && std::abs(std::abs(x) - bg) > bg * 1e-6))
      throw std::invalid_argument("surface-query SDF values must be finite "
                                  "with symmetric inactive band");
    if (x != bg)
      box.expand(v.getBoundingBox());
  }
  Domain d;
  for (size_t a = 0; a < 3; ++a) {
    const int64_t lo = box.min()[a], hi = box.max()[a];
    if (lo < INT32_MIN + 16LL || hi > INT32_MAX - 16LL)
      throw std::invalid_argument(
          "surface-query source leaves insufficient stencil halo");
    d.offset[a] = int32_t(lo + (hi - lo) / 2);
  }
  d.local = box;
  d.local.translate(-d.offset);
  d.local.expand(3);
  return d;
}
template <class G>
openvdb::DoubleGrid::Ptr normalize(const G &source, const Domain &d,
                                   const Map &map) {
  auto local = openvdb::DoubleGrid::create(units(source.background(), map.dx));
  local->setGridClass(openvdb::GRID_LEVEL_SET);
  local->setTransform(openvdb::math::Transform::createLinearTransform(1));
  for (auto v = source.cbeginValueAll(); v; ++v) {
    if (v.isVoxelValue() || *v == source.background())
      continue;
    auto box = v.getBoundingBox();
    box.translate(-d.offset);
    local->tree().fill(box, units(double(*v), map.dx), v.isValueOn());
  }
  auto a = local->getAccessor();
  for (auto v = source.cbeginValueAll(); v; ++v) {
    if (!v.isVoxelValue() || (!v.isValueOn() && *v == source.background()))
      continue;
    const auto c = v.getCoord() - d.offset;
    if (v.isValueOn())
      a.setValue(c, units(double(*v), map.dx));
    else
      a.setValueOff(c, units(double(*v), map.dx));
  }
  bool crossing = false;
  for (auto v = local->cbeginValueOn(); v && !crossing; ++v)
    for (size_t k = 0; k < 3; ++k) {
      auto n = v.getCoord();
      ++n[k];
      const auto x = *v, y = a.getValue(n);
      crossing |= (x < 0 && y >= 0) || (x >= 0 && y < 0);
    }
  if (!crossing)
    throw std::invalid_argument(
        "surface-query source has no resolved zero crossing");
  return local;
}
std::vector<SurfaceQueryMetadata> metadata(const openvdb::MetaMap &map) {
  std::vector<SurfaceQueryMetadata> out;
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
void floatPrecision(const V &p) {
  for (size_t a = 0; a < 3; ++a) {
    if (!std::isfinite(p[a]) || p[a] < INT32_MIN + 4. || p[a] > INT32_MAX - 4.)
      throw std::invalid_argument(
          "surface-query coordinate exceeds native range");
    const float f = float(p[a]);
    const double spacing =
        std::max(std::abs(double(std::nextafter(f, INFINITY)) - f),
                 std::abs(double(f) - std::nextafter(f, -INFINITY)));
    if (!std::isfinite(f) || spacing > 1e-4)
      throw std::invalid_argument(
          "native closest-surface Float precision requires smaller "
          "extents/closer queries or recentering");
  }
}
V index(const V &world, const openvdb::GridBase &source, const Map &map,
        const openvdb::Coord &offset) {
  std::array<V, 3> inverse;
  for (size_t k = 0; k < 3; ++k) {
    V e(0);
    e[k] = 1;
    inverse[k] = source.transform().baseMap()->applyInverseJacobian(e);
  }
  // Center in the FORWARD world map first. Inverse*(world-origin)-offset
  // amplifies the stored inverse coefficient error by remote integer offsets,
  // even if that dot product itself uses exact-product compensation.
  const V center = map.world(V(0), offset), delta = world - center;
  V q;
  for (size_t row = 0; row < 3; ++row) {
    const double spacing =
        std::max(std::abs(std::nextafter(world[row], INFINITY) - world[row]),
                 std::abs(world[row] - std::nextafter(world[row], -INFINITY)));
    if (!std::isfinite(world[row]) || spacing > map.dx * 1e-4)
      throw std::invalid_argument(
          "surface-query world coordinates lose source voxel precision");
    double sum = 0, error = 0;
    const auto add = [&](double x) {
      const double next = sum + x;
      error +=
          std::abs(sum) >= std::abs(x) ? (sum - next) + x : (x - next) + sum;
      sum = next;
    };
    for (size_t k = 0; k < 3; ++k) {
      const double product = inverse[k][row] * delta[k];
      add(product);
      add(std::fma(inverse[k][row], delta[k], -product));
    }
    q[row] = sum + error;
  }
  floatPrecision(q);
  return q;
}
template <class F> double interpolate(const V &p, F &&fetch) {
  const auto c = openvdb::Coord::floor(p);
  const V f = p - c.asVec3d();
  double yz[2];
  for (int z = 0; z < 2; ++z) {
    double yv[2];
    for (int y = 0; y < 2; ++y)
      yv[y] = std::lerp(fetch(c + openvdb::Coord(0, y, z)),
                        fetch(c + openvdb::Coord(1, y, z)), f[0]);
    yz[z] = std::lerp(yv[0], yv[1], f[1]);
  }
  return std::lerp(yz[0], yz[1], f[2]);
}
void localGeometry(const openvdb::DoubleGrid &local, const V &p, const Map &map,
                   SurfaceQueryResult &result) {
  const auto c = openvdb::Coord::floor(p);
  auto a = local.getConstAccessor();
  openvdb::math::CurvatureStencil<openvdb::DoubleGrid> stencil(local);
  std::array<V, 8> gradients;
  std::array<double, 8> means, gaussians;
  for (int z = 0; z < 2; ++z)
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x) {
        const auto center = c + openvdb::Coord(x, y, z);
        for (int dz = -1; dz <= 1; ++dz)
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
              if (std::abs(dx) + std::abs(dy) + std::abs(dz) <= 2 &&
                  !a.isValueOn(center + openvdb::Coord(dx, dy, dz)))
                return;
        stencil.moveTo(center);
        const size_t n = x + 2 * y + 4 * z;
        gradients[n] = stencil.gradient();
        if (!(gradients[n].lengthSqr() >
              openvdb::math::Tolerance<double>::value()))
          return;
        stencil.curvatures(means[n], gaussians[n]);
        if (!std::isfinite(means[n]) || !std::isfinite(gaussians[n]))
          return;
      }
  const auto blend = [&](const auto &values) {
    return interpolate(p, [&](const openvdb::Coord &n) {
      const auto delta = n - c;
      return values[size_t(delta[0] + 2 * delta[1] + 4 * delta[2])];
    });
  };
  V grad;
  for (size_t axis = 0; axis < 3; ++axis) {
    std::array<double, 8> component;
    for (size_t n = 0; n < 8; ++n)
      component[n] = gradients[n][axis];
    grad[axis] = blend(component);
  }
  const double length = std::hypot(grad[0], grad[1], grad[2]);
  if (!(length > 1e-12) || !std::isfinite(length))
    return;
  grad /= length;
  V normal(0);
  for (size_t axis = 0; axis < 3; ++axis)
    normal += map.basis[axis] * (grad[axis] / map.dx);
  const double normalLength = std::hypot(normal[0], normal[1], normal[2]);
  if (!std::isfinite(normalLength) || normalLength <= 0)
    return;
  normal /= normalLength;
  const double mean = units(blend(means), map.dx),
               gauss = units(units(blend(gaussians), map.dx), map.dx);
  result.normal = {normal[0], normal[1], normal[2]};
  result.meanCurvature = mean;
  result.gaussianCurvature = gauss;
  result.geometryValid = true;
}
template <class G>
SurfaceQueryBatch query(const G &source, const Loaded &loaded,
                        std::span<const std::array<double, 3>> points,
                        const SurfaceQueryOptions &o,
                        const ProcessingOptions &p, long double resultReserve) {
  const Map map(source);
  const auto d = validate(source, map, p);
  SurfaceQueryBatch result;
  result.gridName = source.getName();
  result.valueType = source.valueType();
  result.voxelSize = map.dx;
  result.metadata = metadata(source);
  result.fileMetadata = metadata(*loaded.fileMetadata);
  result.diagnostic =
      "Approximate nearest extracted SURFACE POINT CLOUD, not exact "
      "triangle/implicit closest. Local curvature/normal at returned point "
      "require full active19-point footprints; flags gate all local/SDF "
      "quantities. Native small-grid accuracy can be poorer; no automatic "
      "resampling. Source unchanged.";
  const long double cells = blocks(d.local, 1);
  if (cells > p.maxVoxelCount ||
      blocks(d.local, 8) >
          p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0) ||
      cells >= double(UINT32_MAX) / 8)
    throw std::invalid_argument(
        "surface-query full padded domain exceeds voxel/leaf/index limits");
  const long double work = cells * (8.0L * points.size() + 256) +
                           source.tree().leafCount() * 1024.0L +
                           points.size() * 2048.0L;
  if (work > o.maxWork)
    throw std::invalid_argument(
        "surface-query worst point-cloud/stencil work exceeds maxWork");
  budget(loaded.resident + 16 * loaded.metadata + 12 * treeBytes(d.local) +
             cells * 256 + resultReserve,
         p,
         "surface-query index/normalized-tree/output workspace exceeds shared "
         "memory budget");
  auto local = normalize(source, d, map);
  if (points.empty())
    return result;
  floatPrecision(d.local.min().asVec3d());
  floatPrecision(d.local.max().asVec3d());
  std::vector<V> positions;
  positions.reserve(points.size());
  for (const auto &point : points)
    positions.push_back(index(V(point.data()), source, map, d.offset));
  const auto originals = positions;
  auto closest =
      openvdb::tools::ClosestSurfacePoint<openvdb::DoubleGrid>::create(*local);
  if (!closest)
    throw std::invalid_argument(
        "native closest-surface index could not be constructed");
  std::vector<float> nativeDistances;
  if (!closest->searchAndReplace(positions, nativeDistances) ||
      positions.size() != points.size() ||
      nativeDistances.size() != points.size())
    throw std::runtime_error("native closest-surface query failed");
  result.queries.reserve(points.size());
  const auto a = local->getConstAccessor();
  for (size_t n = 0; n < points.size(); ++n) {
    floatPrecision(positions[n]);
    if (!std::isfinite(nativeDistances[n]) || nativeDistances[n] < 0)
      throw std::invalid_argument(
          "native closest-surface query returned invalid distance");
    const auto world = map.world(positions[n], d.offset),
               delta = originals[n] - positions[n];
    SurfaceQueryResult item;
    item.query = points[n];
    item.closestPosition = {world[0], world[1], world[2]};
    const double localDistance = std::hypot(delta[0], delta[1], delta[2]);
    item.distance = localDistance * map.dx;
    if (!std::isfinite(item.distance) ||
        (localDistance != 0 && item.distance == 0))
      throw std::invalid_argument(
          "surface-query distance exceeds Double world range");
    const auto q = openvdb::Coord::floor(originals[n]);
    item.querySdfValid = true;
    for (int z = 0; z < 2; ++z)
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
          item.querySdfValid &= a.isValueOn(q + openvdb::Coord(x, y, z));
    if (item.querySdfValid) {
      const double value = interpolate(
          originals[n], [&](const auto &c) { return a.getValue(c); });
      item.querySdf = value * map.dx;
      if (!std::isfinite(item.querySdf) || (value != 0 && item.querySdf == 0))
        throw std::invalid_argument(
            "surface-query interpolated SDF exceeds Double world range");
    }
    localGeometry(*local, positions[n], map, item);
    result.queries.push_back(item);
  }
  return result;
}
#endif
} // namespace
SurfaceQueryBatch
QueryLevelSetSurface(const std::filesystem::path &input,
                     std::span<const std::array<double, 3>> points,
                     const SurfaceQueryOptions &o, const std::string &selector,
                     const ProcessingOptions &p) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;
  (void)points;
  (void)o;
  (void)selector;
  (void)p;
  unavailable();
#else
  if (!p.maxVoxelCount || !p.maxInputBytes || !o.maxQueries ||
      o.maxQueries > 1'000'000 || points.size() > o.maxQueries || !o.maxWork ||
      !o.maxGrids || o.maxGrids > 4096 || !o.maxMetadataBytes)
    throw std::invalid_argument(
        "invalid surface-query batch/work/catalog/metadata/memory limits");
  for (const auto &point : points)
    for (double x : point)
      if (!std::isfinite(x))
        throw std::invalid_argument("surface-query positions must be finite");
  const long double resultReserve =
      Fixed +
      points.size() * (8.0L * sizeof(SurfaceQueryResult) + 8 * sizeof(V));
  budget(resultReserve + 1, p,
         "surface-query input/output buffers exceed shared memory budget");
  auto sourceBudget = p;
  sourceBudget.maxInputBytes -= uintmax_t(std::ceil(resultReserve));
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto loaded = load(input, selector, o, sourceBudget);
    if (loaded.grid->isType<openvdb::FloatGrid>())
      return query(*openvdb::gridPtrCast<openvdb::FloatGrid>(loaded.grid),
                   loaded, points, o, p, resultReserve);
    return query(*openvdb::gridPtrCast<openvdb::DoubleGrid>(loaded.grid),
                 loaded, points, o, p, resultReserve);
  });
#endif
}
} // namespace container::geometry::vdb
