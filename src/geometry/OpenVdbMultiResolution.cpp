#include "Container/geometry/OpenVdbMultiResolution.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <mutex>
#include <openvdb/openvdb.h>
#include <openvdb/tools/MultiResGrid.h>
#include <set>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB multiresolution is unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed = 4.0L * 1024 * 1024;
using V = openvdb::Vec3d;
void budget(long double bytes, const ProcessingOptions &p, const char *message) {
  if (!std::isfinite(bytes) || bytes > p.maxInputBytes)
    throw std::invalid_argument(message);
}
void options(const ProcessingOptions &p) {
  if (!p.maxVoxelCount || !p.maxInputBytes)
    throw std::invalid_argument("multiresolution budgets must be positive");
}
long double metadataBytes(const openvdb::MetaMap &map) {
  long double n = sizeof(openvdb::MetaMap);
  for (auto it = map.beginMeta(); it != map.endMeta(); ++it)
    n += 2.0L * (it->first.size() + it->second->typeName().size() + it->second->size()) + 256;
  return n;
}
double length(const V &v) { return std::hypot(v.x(), v.y(), v.z()); }
double gap(double x) {
  return std::max(std::nextafter(x, INFINITY) - x, x - std::nextafter(x, -INFINITY));
}
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
}
template <class T> void number(T value) {
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(value) ||
        (value != 0 && (std::abs(value) < 128 * std::numeric_limits<T>::min() ||
                        std::abs(value) > std::numeric_limits<T>::max() / 128)))
      throw std::invalid_argument(
          "multiresolution native values exceed the finite filter dynamic range");
  } else
    for (int k = 0; k < 3; ++k)
      number(value[k]);
}
template <class T> void finiteValue(T value) {
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(value))
      throw std::invalid_argument("multiresolution filter/sample produced a nonfinite value");
  } else
    for (int k = 0; k < 3; ++k)
      finiteValue(value[k]);
}
template <class T> DenseValue publicValue(const T &value) {
  if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
    return value;
  else {
    using S = std::remove_cvref_t<decltype(value[0])>;
    return std::array<S, 3>{value[0], value[1], value[2]};
  }
}
bool finite(const V &v) {
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}
void transform(const openvdb::GridBase &grid) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument("multiresolution requires a nonsingular affine transform");
  const auto map = grid.transform().baseMap();
  std::array<V, 3> axes{map->applyJacobian(V(1, 0, 0)), map->applyJacobian(V(0, 1, 0)),
                        map->applyJacobian(V(0, 0, 1))};
  double scale = 1, minSize = INFINITY;
  for (const auto &v : axes) {
    const double n = length(v);
    if (!finite(v) || n < 1e-6 || n > 1e6)
      throw std::invalid_argument("multiresolution source voxel axes must span [1e-6,1e6]");
    scale *= n;
    minSize = std::min(minSize, n);
  }
  const double determinant = axes[0].dot(axes[1].cross(axes[2]));
  if (!std::isfinite(determinant) || std::abs(determinant) < 3e-15 ||
      std::abs(determinant / scale) < 1e-12)
    throw std::invalid_argument(
        "multiresolution transform is singular/ill-conditioned or below inverse-map scale");
  const auto origin = map->applyMap(V(0));
  if (!finite(origin))
    throw std::invalid_argument("multiresolution transform origin is not finite");
  const auto bounds = grid.evalActiveVoxelBoundingBox();
  if (bounds.empty())
    return;
  for (unsigned corner = 0; corner < 8; ++corner) {
    V index;
    for (int k = 0; k < 3; ++k)
      index[k] = (corner & (1u << k)) ? int64_t(bounds.max()[k]) + 1 : int64_t(bounds.min()[k]) - 1;
    const auto world = map->applyMap(index), restored = map->applyInverseJacobian(world - origin);
    for (int k = 0; k < 3; ++k) {
      double error = gap(origin[k]), partial = 0;
      for (int c = 0; c < 3; ++c) {
        const double term = axes[c][k] * index[c];
        partial += term;
        error += gap(term) + gap(partial);
      }
      error += gap(partial + origin[k]);
      if (!std::isfinite(world[k]) || gap(world[k]) > minSize * 1e-4 || !std::isfinite(error) ||
          error > minSize * 1e-4 || !std::isfinite(restored[k]) ||
          std::abs(restored[k] - index[k]) > 1e-4)
        throw std::invalid_argument(
            "multiresolution source has insufficient world/index precision; recenter it");
    }
  }
}
openvdb::GridBase::Ptr read(const std::filesystem::path &input, const std::string &selection,
                            const ProcessingOptions &p, uint64_t &encoded) {
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("multiresolution input is not a regular file");
  encoded = std::filesystem::file_size(input);
  budget(encoded + Fixed, p, "multiresolution encoded source exceeds memory budget");
  detail::RequireSeekableFile(input);
  openvdb::io::File file(input.string());
  file.open(false);
  std::set<std::string> names;
  long double retained = encoded + Fixed;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto name = it.gridName();
    retained += name.size() * 2.0L + 128;
    budget(retained, p, "multiresolution descriptor catalog exceeds memory budget");
    if (!names.insert(name).second)
      throw std::invalid_argument(
          "multiresolution source has colliding descriptors; rename grids first");
  }
  std::string name;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto meta = file.readGridMetadata(it.gridName());
    if (!(meta->isType<openvdb::FloatGrid>() || meta->isType<openvdb::DoubleGrid>() ||
          meta->isType<openvdb::Vec3SGrid>() || meta->isType<openvdb::Vec3DGrid>()) ||
        !GridSelectionMatches(selection, it.gridName(), meta->getName()))
      continue;
    if (found)
      throw std::invalid_argument(
          "multiresolution selector is ambiguous; use descriptor:<exact name>");
    const auto count = meta->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT),
               memory = meta->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!count || !memory || count->value() < 0 || memory->value() < 0 ||
        uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument(
          "multiresolution requires recorded topology/memory within budgets");
    budget(retained + 3.0L * memory->value() + 4 * metadataBytes(*meta), p,
           "multiresolution source recorded memory exceeds budget");
    name = it.gridName();
    found = true;
    if (selection.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument("no matching Float/Double/Vec3S/Vec3D grid for multiresolution");
  auto grid = file.readGrid(name);
  file.close();
  budget(retained + 3.0L * grid->memUsage() + 4 * metadataBytes(*grid), p,
         "multiresolution loaded source exceeds memory budget");
  if (grid->activeVoxelCount() > p.maxVoxelCount ||
      grid->baseTree().leafCount() > p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument("multiresolution source topology exceeds voxel budget");
  return grid;
}
template <class Grid> void validate(const Grid &grid, bool filtered) {
  transform(grid);
  using T = typename Grid::ValueType;
  constexpr bool scalar = std::is_floating_point_v<T>;
  if (grid.getName().empty() || grid.getName().size() > 256 ||
      grid.getName().find('\0') != std::string::npos)
    throw std::invalid_argument(
        "multiresolution source name must contain 1..256 bytes without NUL");
  if (grid.getGridClass() != openvdb::GRID_UNKNOWN &&
      !(scalar && grid.getGridClass() == openvdb::GRID_FOG_VOLUME))
    throw std::invalid_argument("multiresolution supports unknown scalar/vector fields and scalar "
                                "fog; SDF/MAC are unsupported");
  if constexpr (!scalar)
    if (!grid.isInWorldSpace() || (grid.getVectorType() != openvdb::VEC_INVARIANT &&
                                   grid.getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE))
      throw std::invalid_argument("multiresolution vectors require collocated world-space "
                                  "invariant or contravariant-relative components");
  if (filtered)
    number(grid.background());
  else
    finiteValue(grid.background());
  if constexpr (scalar)
    if (grid.getGridClass() == openvdb::GRID_FOG_VOLUME && grid.background() != T(0))
      throw std::invalid_argument("multiresolution fog requires zero background");
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    if (filtered)
      number(*value);
    else
      finiteValue(*value);
    if (!value.isValueOn() && *value != grid.background())
      throw std::invalid_argument("multiresolution inactive values must equal background");
    if constexpr (scalar)
      if (grid.getGridClass() == openvdb::GRID_FOG_VOLUME && *value < T(0))
        throw std::invalid_argument("multiresolution fog must be nonnegative");
  }
}
int64_t floorDiv(int64_t x, int64_t d) { return x >= 0 ? x / d : (x + 1) / d - 1; }
struct Region {
  std::array<int64_t, 3> lo{}, hi{};
  bool empty = true;
};
Region region(const openvdb::CoordBBox &b) {
  Region r;
  r.empty = b.empty();
  if (!r.empty)
    for (int k = 0; k < 3; ++k) {
      r.lo[k] = b.min()[k];
      r.hi[k] = b.max()[k];
    }
  return r;
}
long double nodes(const Region &r, int d, int padding = 0) {
  if (r.empty)
    return 0;
  long double n = 1;
  for (int k = 0; k < 3; ++k)
    n *= floorDiv(r.hi[k] + padding, d) - floorDiv(r.lo[k] - padding, d) + 1;
  return n;
}
long double voxels(const Region &r, int padding = 0) {
  if (r.empty)
    return 0;
  long double n = 1;
  for (int k = 0; k < 3; ++k) {
    if (r.lo[k] - padding < INT32_MIN + 16ll || r.hi[k] + padding > INT32_MAX - 16ll)
      throw std::invalid_argument("multiresolution topology exceeds safe integer coordinates");
    n *= r.hi[k] - r.lo[k] + 1 + 2 * padding;
  }
  return n;
}
template <class Grid> long double treeBytes(const Region &r, int padding = 0) {
  using Tree = typename Grid::TreeType;
  using Leaf = typename Tree::LeafNodeType;
  using Upper = typename Tree::RootNodeType::ChildNodeType;
  using Lower = typename Upper::ChildNodeType;
  return nodes(r, 8, padding) * (sizeof(Leaf) + 512.0L * sizeof(typename Grid::ValueType) + 512) +
         nodes(r, 128, padding) * (sizeof(Lower) + 1024.0L) +
         nodes(r, 4096, padding) * (sizeof(Upper) + 1024.0L);
}
struct Preflight {
  long double retained;
  uint64_t slots;
};
template <class Grid>
Preflight preflight(const Grid &grid, uint64_t encoded, const MultiResolutionOptions &m,
                    const ProcessingOptions &p) {
  Region r = region(grid.evalActiveVoxelBoundingBox());
  long double retainedTrees = 0, maxScratch = 0, slots = 0, work = 1, totalVoxels = 0;
  // The source can retain inactive background leaves outside its active bbox.
  // They survive at level 0 even though restriction only propagates activity.
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
    bool outside = r.empty;
    if (!outside)
      for (int k = 0; k < 3; ++k)
        outside |= floorDiv(leaf->origin()[k], 8) < floorDiv(r.lo[k], 8) ||
                   floorDiv(leaf->origin()[k], 8) > floorDiv(r.hi[k], 8);
    if (outside) {
      slots += 512;
      work += 1024;
    }
  }
  for (uint32_t level = 0; level < m.levels; ++level) {
    const auto count = voxels(r);
    totalVoxels += count;
    const auto leafSlots = nodes(r, 8) * 512;
    slots += leafSlots;
    retainedTrees += treeBytes<Grid>(r);
    work += leafSlots * (level == 0 ? 2 : 28);
    if (level + 1 < m.levels) {
      const auto maskSlots = nodes(r, 8, 1) * 512;
      voxels(r, 1);
      maxScratch = std::max(maxScratch, 8 * treeBytes<Grid>(r, 1));
      work += maskSlots * 4;
      for (int k = 0; k < 3; ++k) {
        r.lo[k] = -floorDiv(-(r.lo[k] - 1), 2);
        r.hi[k] = floorDiv(r.hi[k] + 1, 2);
      }
    }
  }
  if (totalVoxels > p.maxVoxelCount ||
      slots > 512.0L * (p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0)))
    throw std::invalid_argument(
        "all retained multiresolution levels exceed the aggregate voxel/leaf budget");
  if (work > m.maxWork)
    throw std::invalid_argument("multiresolution full-weighting work preflight exceeds maxWork");
  const auto meta = metadataBytes(grid);
  // MaskOp topology-copies the complete source, including inactive remote
  // internal nodes. Reserve its serial morphology/leaf-manager scratch too,
  // independently of the prospective active region.
  const long double bytes = encoded + 11.0L * grid.memUsage() + 3 * retainedTrees + maxScratch +
                            (4.0L * m.levels + 4) * meta + Fixed;
  budget(bytes, p,
         "multiresolution tile/retained-level/mask/metadata workspace exceeds memory budget");
  return {encoded + grid.memUsage() + retainedTrees + (2.0L * m.levels + 2) * meta + Fixed,
          uint64_t(slots)};
}
template <class Grid>
DenseValue sample(const Grid &finest,
                  const std::function<typename Grid::ValueType(const V &, double)> &sampler,
                  const std::array<double, 3> &world, double level) {
  V p(world.data());
  if (!finite(p))
    throw std::invalid_argument("multiresolution sample position must be finite");
  const auto map = finest.transform().baseMap();
  const V index = map->applyInverseJacobian(p - map->applyMap(V(0)));
  if (!finite(index))
    throw std::invalid_argument("multiresolution sample inverse is not finite");
  for (int k = 0; k < 3; ++k)
    if (index[k] < double(INT32_MIN) + 8 || index[k] > double(INT32_MAX) - 8 ||
        gap(index[k]) > 1e-8)
      throw std::invalid_argument(
          "multiresolution sample lacks finest-index fractional precision; recenter source");
  const auto value = sampler(index, level);
  finiteValue(value);
  return publicValue(value);
}
#endif
} // namespace

struct MultiResolutionVolume::Impl {
  std::vector<MultiResolutionLevelInfo> info;
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  openvdb::GridPtrVec grids;
  std::function<DenseValue(const std::array<double, 3> &, double)> sample;
  long double retained{};
  uint64_t slots{};
#endif
};
MultiResolutionVolume::MultiResolutionVolume(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
MultiResolutionVolume::~MultiResolutionVolume() = default;
const std::vector<MultiResolutionLevelInfo> &MultiResolutionVolume::levels() const noexcept {
  return impl_->info;
}
DenseValue MultiResolutionVolume::sampleWorld(const std::array<double, 3> &position,
                                              double level) const {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if (!std::isfinite(level) || level < 0 || level > double(impl_->grids.size() - 1))
    throw std::invalid_argument("multiresolution sample level out of range");
  return impl_->sample(position, level);
#else
  (void)position;
  (void)level;
  unavailable();
#endif
}
ProcessingResult MultiResolutionVolume::writeLevel(const std::filesystem::path &output,
                                                   uint32_t level,
                                                   const ProcessingOptions &p) const {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(p);
  if (level >= impl_->grids.size())
    throw std::invalid_argument("multiresolution output level out of range");
  if (impl_->slots / 512 > p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument("retained multiresolution hierarchy exceeds writer voxel budget");
  const auto &grid = impl_->grids[level];
  budget(impl_->retained + 3.0L * grid->memUsage() + 4 * metadataBytes(*grid) + Fixed, p,
         "multiresolution retained hierarchy/writer exceeds memory budget");
  detail::WriteFileExclusive(output, {grid}, p.maxInputBytes, openvdb::io::COMPRESS_ZIP);
  return {grid->getName(), openvdb::GridBase::gridClassToString(grid->getGridClass()),
          grid->activeVoxelCount(), grid->voxelSize().x()};
#else
  (void)output;
  (void)level;
  (void)p;
  unavailable();
#endif
}
void MultiResolutionVolume::writeAll(const std::filesystem::path &output,
                                     const ProcessingOptions &p) const {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(p);
  if (impl_->slots / 512 > p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument("retained multiresolution hierarchy exceeds writer voxel budget");
  long double bytes = impl_->retained + Fixed;
  for (const auto &grid : impl_->grids)
    bytes += 3.0L * grid->memUsage() + 4 * metadataBytes(*grid);
  budget(bytes, p, "all retained multiresolution levels/writer exceed memory budget");
  detail::WriteFileExclusive(output, impl_->grids, p.maxInputBytes, openvdb::io::COMPRESS_ZIP);
#else
  (void)output;
  (void)p;
  unavailable();
#endif
}
std::shared_ptr<const MultiResolutionVolume>
BuildMultiResolution(const std::filesystem::path &input, const MultiResolutionOptions &m,
                     const std::string &selection, const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(p);
  if (m.levels < 1 || m.levels > 8 || m.maxWork < 1 || m.maxWork > 10'000'000'000ull)
    throw std::invalid_argument("multiresolution levels/work limits out of range");
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&]() -> std::shared_ptr<const MultiResolutionVolume> {
    uint64_t encoded{};
    auto source = read(input, selection, p, encoded);
    auto out = std::make_unique<MultiResolutionVolume::Impl>();
    const auto build = [&]<class Grid>(typename Grid::Ptr grid) {
      validate(*grid, m.levels > 1);
      if (m.levels > 1 && grid->getName().size() > 248)
        throw std::invalid_argument("multiresolution level names would exceed 256 bytes");
      const auto reserved = preflight(*grid, encoded, m, p);
      out->retained = reserved.retained;
      out->slots = reserved.slots;
      grid->removeMeta("MultiResGrid_Levels");
      grid->removeMeta("MultiResGrid_Level");
      grid->removeMeta("container:multires_filter");
      grid->insertMeta("container:multires_filter",
                       openvdb::StringMetadata(
                           m.levels == 1 ? "passthrough" : "27-point full weighting [1,2,1]^3/64"));
      if (m.levels == 1) {
        grid->setSaveFloatAsHalf(false);
        out->grids.push_back(grid);
        out->sample = [grid](const auto &position, double level) {
          const std::function<typename Grid::ValueType(const V &, double)> function =
              [grid](const V &p, double) {
                return openvdb::tools::BoxSampler::sample(grid->tree(), p);
              };
          return sample(*grid, function, position, level);
        };
      } else {
        using M = openvdb::tools::MultiResGrid<typename Grid::TreeType>;
        auto hierarchy = std::make_shared<M>(m.levels, grid, false);
        for (uint32_t level = 0; level < m.levels; ++level) {
          auto g = hierarchy->grid(level);
          detail::RemoveBoundaryProjectionMetadata(*g);
          auto matrix=grid->transform().baseMap()->getAffineMap()->getMat4();
          for(size_t r=0;r<3;++r) for(size_t c=0;c<3;++c) matrix[r][c]=std::ldexp(matrix[r][c],int(level));
          g->setTransform(detail::MakeAffineTransformExact(matrix));
          g->setSaveFloatAsHalf(false);
          out->grids.push_back(g);
        }
        auto finest = hierarchy->grid(0);
        finest->setTransform(detail::CopyTransformExact(grid->transform()));
        out->sample = [hierarchy, finest](const auto &position, double level) {
          const std::function<typename Grid::ValueType(const V &, double)> function =
              [hierarchy](const V &p, double l) {
                return hierarchy->template sampleValue<1>(p, l);
              };
          return sample(*finest, function, position, level);
        };
      }
      long double actual = encoded + Fixed;
      uint64_t slots = 0;
      for (uint32_t level = 0; level < out->grids.size(); ++level) {
        auto g = openvdb::gridPtrCast<Grid>(out->grids[level]);
        for (auto value = g->cbeginValueAll(); value; ++value)
          finiteValue(*value);
        finiteValue(g->background());
        slots += uint64_t(g->tree().leafCount()) * 512;
        actual += g->memUsage() + 2 * metadataBytes(*g);
        const auto dx = g->voxelSize();
        out->info.push_back({level,
                             g->getName(),
                             g->valueType(),
                             g->activeVoxelCount(),
                             g->tree().leafCount(),
                             {dx.x(), dx.y(), dx.z()}});
      }
      if (slots > out->slots)
        throw std::runtime_error("multiresolution actual retained topology exceeds preflight");
      budget(actual, p, "actual retained multiresolution hierarchy exceeds memory budget");
      out->retained = std::max(out->retained, actual);
    };
    if (source->isType<openvdb::FloatGrid>())
      build.operator()<openvdb::FloatGrid>(openvdb::gridPtrCast<openvdb::FloatGrid>(source));
    else if (source->isType<openvdb::DoubleGrid>())
      build.operator()<openvdb::DoubleGrid>(openvdb::gridPtrCast<openvdb::DoubleGrid>(source));
    else if (source->isType<openvdb::Vec3SGrid>())
      build.operator()<openvdb::Vec3SGrid>(openvdb::gridPtrCast<openvdb::Vec3SGrid>(source));
    else
      build.operator()<openvdb::Vec3DGrid>(openvdb::gridPtrCast<openvdb::Vec3DGrid>(source));
    return std::shared_ptr<const MultiResolutionVolume>(new MultiResolutionVolume(std::move(out)));
  });
#else
  (void)input;
  (void)m;
  (void)selection;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
