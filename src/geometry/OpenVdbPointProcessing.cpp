#include "Container/geometry/OpenVdbPointProcessing.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/tools/Interpolation.h>
#include <tbb/task_arena.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <set>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB point processing is unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using PointGrid = openvdb::points::PointDataGrid;
constexpr long double Fixed = 4.0L * 1024 * 1024;
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    openvdb::initialize();
    using V = openvdb::points::TypedAttributeArray<openvdb::Vec3d, openvdb::points::NullCodec>;
    using R = openvdb::points::TypedAttributeArray<double, openvdb::points::NullCodec>;
    if (!V::isRegistered()) V::registerType();
    if (!R::isRegistered()) R::registerType();
  });
}
void budget(long double bytes, const ProcessingOptions& p, const char* reason) {
  if (!std::isfinite(bytes) || bytes > p.maxInputBytes) throw std::invalid_argument(reason);
}
long double metaBytes(const openvdb::MetaMap& map) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto it = map.beginMeta(); it != map.endMeta(); ++it)
    bytes += 2.0L * (it->first.size() + it->second->typeName().size() + it->second->size()) + 256;
  return bytes;
}
void validateOptions(const ProcessingOptions& p, const PointOptions& points,
                     const std::filesystem::path& output) {
  if (!p.maxInputBytes || !p.maxVoxelCount || !points.maxPoints)
    throw std::invalid_argument("point processing budgets must be positive");
  if (!std::isfinite(p.voxelSize) || p.voxelSize < 1.5e-5 || p.voxelSize > 1e6)
    throw std::invalid_argument("point generation voxel size must be in [1.5e-5,1e6]");
  if (p.gridName.empty() || p.gridName.size() > 256)
    throw std::invalid_argument("point output grid name must contain 1..256 bytes");
  if (output.empty() || std::filesystem::exists(output))
    throw std::invalid_argument("point output must be a new nonempty path");
  if (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("point output directory does not exist");
}
void coordinate(const openvdb::Vec3d& index) {
  for (size_t axis = 0; axis < 3; ++axis)
    if (!std::isfinite(index[axis]) || index[axis] < double(INT32_MIN) + 16 || index[axis] > double(INT32_MAX) - 16)
      throw std::invalid_argument("point processing exceeds safe OpenVDB sampling coordinates");
}
double transform(const openvdb::GridBase& grid) {
  if (!grid.transform().isLinear()) throw std::invalid_argument("point processing requires affine grid transforms");
  const auto origin = grid.indexToWorld(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d, 3> axes;
  double minimum = std::numeric_limits<double>::infinity();
  for (size_t axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(origin[axis])) throw std::invalid_argument("grid translation must be finite");
    openvdb::Vec3d unit(0); unit[axis] = 1;
    axes[axis] = grid.transform().baseMap()->applyJacobian(unit);
    const double length = std::hypot(axes[axis].x(), axes[axis].y(), axes[axis].z());
    if (!std::isfinite(length) || length < 1e-6 || length > 1e6)
      throw std::invalid_argument("point processing grid axes must span [1e-6,1e6] world units");
    axes[axis] /= length; minimum = std::min(minimum, length);
  }
  const double determinant = axes[0].dot(axes[1].cross(axes[2]));
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8)
    throw std::invalid_argument("point processing grid transform is insufficiently conditioned");
  return minimum;
}
void precision(const openvdb::Vec3d& world, double dx) {
  for (size_t axis = 0; axis < 3; ++axis) {
    const double gap = std::abs(std::nextafter(world[axis], std::numeric_limits<double>::infinity()) - world[axis]);
    if (!std::isfinite(world[axis]) || !std::isfinite(gap) || gap > dx * 1e-4)
      throw std::invalid_argument("point world coordinates have insufficient integration precision; recenter the data");
  }
}
void indexPrecision(const openvdb::Vec3d& index, const openvdb::math::MapBase& map, double dx) {
  double resolution = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    const double gap = std::max(std::abs(std::nextafter(index[axis], std::numeric_limits<double>::infinity()) - index[axis]),
                                std::abs(std::nextafter(index[axis], -std::numeric_limits<double>::infinity()) - index[axis]));
    openvdb::Vec3d perturbation(0); perturbation[axis] = gap;
    const auto world = map.applyJacobian(perturbation);
    resolution += std::hypot(world.x(), world.y(), world.z());
  }
  if (!std::isfinite(resolution) || resolution > dx * 1e-4)
    throw std::invalid_argument("field sampling index coordinates have insufficient precision; recenter the field transform");
}
enum class Kind { Fog, Velocity, Points, Scalar };
struct Loaded {
  openvdb::GridBase::Ptr grid, metadata;
  uintmax_t encoded{};
};
Loaded load(const std::filesystem::path& path, const std::string& selection, Kind kind,
            const ProcessingOptions& p, long double resident = 0) {
  if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("point processing input is not a regular file");
  const auto encoded = std::filesystem::file_size(path);
  budget(resident + encoded + Fixed, p, "encoded inputs exceed the point processing memory budget");
  detail::RequireSeekableFile(path);
  openvdb::io::File file(path.string());
  if (kind == Kind::Points) {
    if (!openvdb::io::Archive::isDelayedLoadingEnabled())
      throw std::invalid_argument("bounded point processing requires OpenVDB delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifdef OPENVDB_USE_DELAYED_LOADING
    if (encoded >= std::numeric_limits<openvdb::Index64>::max()) throw std::invalid_argument("point input exceeds delayed-copy size range");
    file.setCopyMaxBytes(static_cast<openvdb::Index64>(encoded) + 1);
#endif
  }
  file.open(kind == Kind::Points);
  {
    std::set<std::string> names;
    long double catalog = resident + encoded + Fixed;
    for (auto it = file.beginName(); it != file.endName(); ++it) {
      const auto name = it.gridName(); catalog += 2.0L * name.size() + 128;
      budget(catalog, p, "grid descriptor catalog exceeds the point processing memory budget");
      if (!names.insert(name).second)
        throw std::invalid_argument("input has colliding grid descriptors; rename colliding grids before processing");
    }
  }
  std::string descriptor;
  openvdb::GridBase::Ptr selected;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto metadata = file.readGridMetadata(it.gridName());
    const bool scalar = metadata->isType<openvdb::FloatGrid>() || metadata->isType<openvdb::DoubleGrid>();
    const bool supported = kind == Kind::Points ? metadata->isType<PointGrid>() : kind == Kind::Fog
      ? scalar && metadata->getGridClass() == openvdb::GRID_FOG_VOLUME : kind == Kind::Scalar
      ? scalar : metadata->isType<openvdb::Vec3SGrid>() || metadata->isType<openvdb::Vec3DGrid>();
    if (!supported || !GridSelectionMatches(selection, it.gridName(), metadata->getName())) continue;
    if (selected) throw std::invalid_argument("grid name is ambiguous; use descriptor:<exact descriptor name>");
    const auto memory = metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto voxels = metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!memory || memory->value() < 0 || !voxels || voxels->value() < 0)
      throw std::invalid_argument("point processing requires recorded nonnegative file_mem_bytes and file_voxel_count statistics; rewrite with grid statistics enabled");
    if (uint64_t(voxels->value()) > p.maxVoxelCount) throw std::invalid_argument("field topology metadata exceeds the voxel budget");
    budget(resident + encoded + memory->value() + 4 * metaBytes(*metadata) + Fixed, p,
           "field metadata/topology exceeds the point processing memory budget");
    descriptor = it.gridName(); selected = metadata;
    if (selection.empty()) break;
  }
  if (!selected) throw std::invalid_argument(kind == Kind::Points ? "no matching PointDataGrid found" : kind == Kind::Fog ? "no matching Float/Double fog grid found" : kind == Kind::Scalar ? "no matching Float/Double scalar field found" : "no matching Vec3S/Vec3D velocity grid found");
  auto grid = file.readGrid(descriptor); file.close();
  budget(resident + encoded + grid->memUsage() + 4 * metaBytes(*grid) + Fixed, p,
         "loaded field exceeds the point processing memory budget");
  (void)transform(*grid);
  return {grid, selected, encoded};
}
template<class Grid> void topology(const Grid& grid, const ProcessingOptions& p) {
  constexpr uint64_t values = Grid::TreeType::LeafNodeType::NUM_VALUES;
  const auto maxLeaves = p.maxVoxelCount / values + (p.maxVoxelCount % values != 0);
  if (grid.activeVoxelCount() > p.maxVoxelCount || grid.tree().leafCount() > maxLeaves)
    throw std::invalid_argument("stored field topology exceeds the voxel budget");
}
void pointLayout(const PointGrid& grid, const ProcessingOptions& p) {
  topology(grid, p);
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
    leaf->validateOffsets();
    if (leaf->beginIndexOff())
      throw std::invalid_argument("point processing cannot preserve inactive point-voxel selections when repartitioning; activate the selected points first");
    const auto& set = leaf->attributeSet();
    if (set.descriptor().getMetadata().beginMeta() != set.descriptor().getMetadata().endMeta())
      throw std::invalid_argument("point processing cannot preserve attribute descriptor metadata; use the standard P/v/pscale layout without descriptor metadata");
    for (size_t index = 0; index < set.size(); ++index)
      if (set.get(index)->isHidden() || set.get(index)->isTransient())
        throw std::invalid_argument("point processing cannot preserve hidden or transient attribute flags");
  }
}
struct Temporary {
  std::filesystem::path directory, file;
  explicit Temporary(const std::filesystem::path& output) {
    static std::atomic_uint64_t serial{0};
    const auto parent = output.has_parent_path() ? output.parent_path() : std::filesystem::path(".");
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      directory = parent / (".container-points-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(directory)) { file = directory / "records.vdb"; return; }
    }
    throw std::runtime_error("could not create owned point processing temporary directory");
  }
  ~Temporary() { std::error_code ignored; std::filesystem::remove(file, ignored); std::filesystem::remove(directory, ignored); }
};
PointResult write(const std::vector<PointRecord>& records, const openvdb::GridBase& metadata,
                  const std::filesystem::path& output, const ProcessingOptions& p,
                  const PointOptions& points, long double encodedReserve) {
  const long double reserve = encodedReserve + 4 * metaBytes(metadata) + Fixed;
  budget(reserve + records.size() * 384.0L, p, "point metadata/record generation workspace exceeds the memory budget");
  auto generation = p;
  generation.maxInputBytes = static_cast<uintmax_t>(static_cast<long double>(p.maxInputBytes) - reserve);
  Temporary temporary(output);
  const auto result = WritePoints(records, temporary.file, generation, points);
  openvdb::io::File file(temporary.file.string()); file.open(false);
  auto grid = openvdb::gridPtrCast<PointGrid>(file.readGrid(p.gridName)); file.close();
  budget(reserve + records.size() * sizeof(PointRecord) + grid->memUsage() +
         std::filesystem::file_size(temporary.file), p, "point metadata publication workspace exceeds the memory budget");
  // Grid metadata is independent of the tree transform. Preserve custom fields,
  // then restore structural fields belonging to the newly generated point grid.
  grid->insertMeta(metadata);
  detail::RemoveBoundaryProjectionMetadata(*grid);
  grid->setName(p.gridName); grid->setGridClass(openvdb::GRID_UNKNOWN);
  grid->setVectorType(openvdb::VEC_INVARIANT); grid->setIsInWorldSpace(true);
  grid->insertMeta("container:point_count", openvdb::Int64Metadata(static_cast<int64_t>(records.size())));
  detail::WriteFileExclusive(output, openvdb::GridPtrVec{grid}, p.maxInputBytes);
  return result;
}
struct Mass { openvdb::CoordBBox bounds; double cumulative; };
template<class Grid>
std::vector<PointRecord> scatter(const Grid& grid, const ScatterOptions& s,
                                const ProcessingOptions& p, long double resident) {
  topology(grid, p);
  if (grid.background() != 0) throw std::invalid_argument("scatter fog must have zero background and inactive values");
  double peak = 0; size_t count = 0;
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    const double density = double(*value);
    if (!std::isfinite(density) || density < 0 || (!value.isValueOn() && density != 0))
      throw std::invalid_argument("scatter fog density must be finite nonnegative with inactive values zero");
    if (value.isValueOn() && density > 0) { peak = std::max(peak, density); ++count; }
  }
  budget(resident + count * (2.0L * sizeof(Mass)) + s.pointCount * (3.0L * sizeof(PointRecord)) + Fixed,
         p, "scatter CDF/decoded point workspace exceeds the memory budget");
  if (s.pointCount == 0) return {};
  if (!count) throw std::invalid_argument("scatter requires positive fog density mass for a nonzero point count");
  std::vector<Mass> masses; masses.reserve(count);
  double total = 0;
  for (auto value = grid.cbeginValueOn(); value; ++value) if (*value > 0) {
    const auto bounds = value.getBoundingBox();
    coordinate(bounds.min().asVec3d()); coordinate(bounds.max().asVec3d());
    const double weight = (double(*value) / peak) * double(value.getVoxelCount());
    if (!(weight > 0) || !std::isfinite(weight) || !(total + weight > total))
      throw std::invalid_argument("scatter density dynamic range loses normalized/CDF mass; rescale or partition the fog");
    total += weight; masses.push_back({bounds, total});
  }
  double previous = 0;
  for (const auto& mass : masses) {
    if ((mass.cumulative - previous) / total < 0x1.0p-53)
      throw std::invalid_argument("scatter density dynamic range is below the seeded generator probability resolution; partition the fog");
    previous = mass.cumulative;
  }
  std::mt19937_64 random(s.seed);
  const auto uniform = [&] { return double(random() >> 11) * 0x1.0p-53; };
  std::vector<PointRecord> records; records.reserve(static_cast<size_t>(s.pointCount));
  const double dx = transform(grid);
  for (uint64_t index = 0; index < s.pointCount; ++index) {
    const double target = std::min(uniform() * total, std::nextafter(total, 0.0));
    const auto chosen = std::upper_bound(masses.begin(), masses.end(), target,
      [](double value, const Mass& mass) { return value < mass.cumulative; });
    if (chosen == masses.end()) throw std::logic_error("scatter CDF selection failed");
    openvdb::Vec3d cell;
    for (size_t axis = 0; axis < 3; ++axis)
      cell[axis] = double(chosen->bounds.min()[axis]) - .5 + uniform() *
          (double(chosen->bounds.max()[axis]) - chosen->bounds.min()[axis] + 1);
    const auto world = grid.indexToWorld(cell); precision(world, dx);
    records.push_back({{world.x(), world.y(), world.z()}, {}, s.radius});
  }
  return records;
}
template<class Grid> double velocity(const Grid& grid, const ProcessingOptions& p) {
  topology(grid, p);
  if (!grid.isInWorldSpace() || grid.getGridClass() == openvdb::GRID_STAGGERED)
    throw std::invalid_argument("point velocity must contain collocated world-space vectors");
  const auto speed = [](const auto& value) {
    for (size_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(double(value[axis])) || std::abs(double(value[axis])) > 1e12)
        throw std::invalid_argument("point velocity components must be finite with magnitude at most 1e12");
    return std::hypot(double(value.x()), double(value.y()), double(value.z()));
  };
  double maximum = speed(grid.background());
  for (auto value = grid.cbeginValueAll(); value; ++value) maximum = std::max(maximum, speed(*value));
  return maximum;
}
openvdb::Vec3DGrid::Ptr promote(const openvdb::Vec3SGrid& source, const ProcessingOptions& p, long double resident) {
  using Tree = openvdb::Vec3DTree;
  using Leaf = Tree::LeafNodeType;
  using Upper = Tree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  const auto nodes = source.tree().nodeCount();
  budget(resident + source.memUsage() + nodes[0] * (static_cast<long double>(sizeof(Leaf)) + Leaf::NUM_VALUES * sizeof(openvdb::Vec3d) + 256) +
      nodes[1] * (static_cast<long double>(sizeof(Lower)) + 128) + nodes[2] * (static_cast<long double>(sizeof(Upper)) + 128) +
      4 * metaBytes(source) + Fixed, p, "Double velocity promotion exceeds the point processing workspace budget");
  auto result = openvdb::Vec3DGrid::create(); result->insertMeta(source);
  result->setTransform(detail::CopyTransformExact(source.transform()));
  const auto background = source.background();
  result->setTree(Tree::Ptr(new Tree(source.tree(), openvdb::Vec3d(background), openvdb::TopologyCopy())));
  auto target = result->beginValueAll();
  for (auto value = source.cbeginValueAll(); value; ++value, ++target) {
    if (!target || value.getLevel() != target.getLevel() || value.getCoord() != target.getCoord())
      throw std::logic_error("point velocity topology changed during promotion");
    target.setValue(openvdb::Vec3d(*value)); target.setActiveState(value.isValueOn());
  }
  if (target) throw std::logic_error("point velocity topology changed during promotion");
  return result;
}
openvdb::DoubleGrid::Ptr promote(const openvdb::FloatGrid& source, const ProcessingOptions& p, long double resident) {
  using Tree = openvdb::DoubleTree;
  using Leaf = Tree::LeafNodeType;
  using Upper = Tree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  const auto nodes = source.tree().nodeCount();
  budget(resident + source.memUsage() + nodes[0] * (static_cast<long double>(sizeof(Leaf)) + Leaf::NUM_VALUES * sizeof(double) + 256) +
      nodes[1] * (static_cast<long double>(sizeof(Lower)) + 128) + nodes[2] * (static_cast<long double>(sizeof(Upper)) + 128) +
      4 * metaBytes(source) + Fixed, p, "Double scalar promotion exceeds the point processing workspace budget");
  auto result = openvdb::DoubleGrid::create(); result->insertMeta(source);
  result->setTransform(detail::CopyTransformExact(source.transform()));
  result->setTree(Tree::Ptr(new Tree(source.tree(), double(source.background()), openvdb::TopologyCopy())));
  auto target = result->beginValueAll();
  for (auto value = source.cbeginValueAll(); value; ++value, ++target) {
    if (!target || value.getLevel() != target.getLevel() || value.getCoord() != target.getCoord())
      throw std::logic_error("point scalar topology changed during promotion");
    target.setValue(double(*value)); target.setActiveState(value.isValueOn());
  }
  if (target) throw std::logic_error("point scalar topology changed during promotion");
  return result;
}
template<class Grid> void scalar(const Grid& grid, const ProcessingOptions& p) {
  topology(grid, p);
  if (!std::isfinite(double(grid.background()))) throw std::invalid_argument("point scalar field must contain finite values");
  for (auto value = grid.cbeginValueAll(); value; ++value)
    if (!std::isfinite(double(*value))) throw std::invalid_argument("point scalar field must contain finite values");
}
template<class Accessor> double scalarSample(const Accessor& accessor, const openvdb::Vec3d& index) {
  const openvdb::Coord base(static_cast<int32_t>(std::floor(index.x())),
                           static_cast<int32_t>(std::floor(index.y())),
                           static_cast<int32_t>(std::floor(index.z())));
  const auto weight = index - base.asVec3d();
  double x[2][2];
  for (int z = 0; z < 2; ++z) for (int y = 0; y < 2; ++y)
    x[z][y] = std::lerp(accessor.getValue(base + openvdb::Coord(0,y,z)),
                         accessor.getValue(base + openvdb::Coord(1,y,z)), weight.x());
  // BoxSampler uses a+(b-a)*t. std::lerp also handles opposite-sign finite
  // endpoints whose difference overflows, while retaining convex interpolation.
  return std::lerp(std::lerp(x[0][0], x[0][1], weight.y()),
                   std::lerp(x[1][0], x[1][1], weight.y()), weight.z());
}
template<class Grid> void sampleRecords(std::vector<PointRecord>& records, const Grid& grid, double sourceDx) {
  auto accessor = grid.getConstAccessor();
  openvdb::tools::GridSampler<decltype(accessor), openvdb::tools::BoxSampler> sampler(accessor, grid.transform());
  const auto map = grid.transform().baseMap();
  const auto origin = map->applyMap(openvdb::Vec3d(0));
  const double dx = std::min(sourceDx, transform(grid));
  for (auto& record : records) {
    const openvdb::Vec3d world(record.position[0], record.position[1], record.position[2]);
    precision(world, dx);
    const auto index = map->applyInverseJacobian(world - origin); coordinate(index);
    indexPrecision(index, *map, dx);
    if constexpr (std::is_arithmetic_v<typename Grid::ValueType>) {
      const auto value = scalarSample(accessor, index);
      if (!std::isfinite(value) || value <= 0)
        throw std::invalid_argument("sampled point radius must be finite and positive; choose a positive scalar field covering every point");
      record.radius = value;
    } else {
      const auto value = sampler.isSample(index);
      for (size_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(value[axis])) throw std::invalid_argument("sampled point velocity must be finite");
      record.velocity = {value.x(), value.y(), value.z()};
    }
  }
}
void advect(std::vector<PointRecord>& records, const openvdb::Vec3DGrid& grid,
            const PointAdvectionOptions& a, double dx, double maximum) {
  const long double travel = std::abs(static_cast<long double>(a.timeStep)) * maximum;
  if (a.timeStep != 0 && maximum != 0 && travel == 0)
    throw std::invalid_argument("point integration displacement is below numeric precision");
  if (travel > 256.0L * dx || travel / a.steps > 16.0L * dx)
    throw std::invalid_argument("point advection exceeds 256 source voxels total or 16 per step; reduce time/velocity or increase steps");
  auto accessor = grid.getConstAccessor();
  openvdb::tools::GridSampler<decltype(accessor), openvdb::tools::BoxSampler> sampler(accessor, grid.transform());
  const auto map = grid.transform().baseMap();
  const auto origin = map->applyMap(openvdb::Vec3d(0));
  const double samplingDx = std::min(dx, transform(grid));
  const auto sample = [&](const openvdb::Vec3d& world) {
    precision(world, samplingDx);
    const auto index = map->applyInverseJacobian(world - origin);
    coordinate(index);
    indexPrecision(index, *map, samplingDx);
    const auto value = sampler.isSample(index);
    for (size_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(value[axis])) throw std::invalid_argument("point integration sampled non-finite velocity");
    return value;
  };
  const auto move = [&](const openvdb::Vec3d& position, const openvdb::Vec3d& delta) {
    const auto next = position + delta; precision(next, dx);
    for (size_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(delta[axis]) || (delta[axis] != 0 && next[axis] == position[axis]))
        throw std::invalid_argument("point integration displacement is below world-coordinate precision");
    return next;
  };
  const double h = a.timeStep / a.steps;
  if (a.timeStep != 0 && maximum != 0 && h == 0)
    throw std::invalid_argument("point integration step is below numeric precision");
  for (auto& record : records) {
    openvdb::Vec3d position(record.position[0], record.position[1], record.position[2]);
    if (h != 0 && maximum != 0) for (uint32_t step = 0; step < a.steps; ++step) {
      const auto k1 = sample(position);
      const auto k2 = sample(move(position, k1 * (h * .5)));
      if (a.integration == PointIntegration::RK2) position = move(position, k2 * h);
      else {
        const auto k3 = sample(move(position, k2 * (h * .5)));
        const auto k4 = sample(move(position, k3 * h));
        position = move(position, (k1 + 2 * k2 + 2 * k3 + k4) * (h / 6));
      }
    }
    record.position = {position.x(), position.y(), position.z()};
    if (a.updateVelocity) { const auto final = sample(position); record.velocity = {final.x(), final.y(), final.z()}; }
  }
}
#endif
} // namespace

PointResult ScatterPoints(const std::filesystem::path& inputFog, const std::filesystem::path& output,
                          const std::string& inputGrid, const ScatterOptions& s,
                          const ProcessingOptions& p, const PointOptions& points) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p, points, output);
  if (s.pointCount > points.maxPoints || s.pointCount >= std::numeric_limits<openvdb::Index32>::max())
    throw std::invalid_argument("scatter point count exceeds --max-points or OpenVDB index range");
  if (!std::isfinite(s.radius) || s.radius <= 0) throw std::invalid_argument("scatter radius must be finite and positive");
  initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    auto source = load(inputFog, inputGrid, Kind::Fog, p);
    const long double resident = source.encoded + source.grid->memUsage() + 4 * metaBytes(*source.metadata);
    auto records = source.grid->isType<openvdb::FloatGrid>()
      ? scatter(*openvdb::gridPtrCast<openvdb::FloatGrid>(source.grid), s, p, resident)
      : scatter(*openvdb::gridPtrCast<openvdb::DoubleGrid>(source.grid), s, p, resident);
    source.grid.reset();
    return write(records, *source.metadata, output, p, points, source.encoded);
  });
#else
  (void)inputFog; (void)output; (void)inputGrid; (void)s; (void)p; (void)points; unavailable();
#endif
}

PointResult WriteAdvectedPoints(const std::filesystem::path& inputPoints, const std::filesystem::path& velocityPath,
                               const std::filesystem::path& output, const std::string& inputGrid,
                               const std::string& velocityGrid, const PointAdvectionOptions& a,
                               const ProcessingOptions& p, const PointOptions& points) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p, points, output);
  if (!std::isfinite(a.timeStep) || std::abs(a.timeStep) > 3600 || a.steps < 1 || a.steps > 64 ||
      (a.integration != PointIntegration::RK2 && a.integration != PointIntegration::RK4))
    throw std::invalid_argument("point advection requires signed time in [-3600,3600], steps in [1,64] and RK2 or RK4");
  initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    auto source = load(inputPoints, inputGrid, Kind::Points, p);
    pointLayout(*openvdb::gridPtrCast<PointGrid>(source.grid), p);
    const double dx = transform(*source.grid);
    source.grid.reset();
    const long double retainedMetadata = 4 * metaBytes(*source.metadata);
    budget(retainedMetadata + Fixed, p, "retained point metadata exceeds the memory budget");
    auto readOptions = p; readOptions.maxInputBytes -= static_cast<uintmax_t>(retainedMetadata);
    auto records = ReadPoints(inputPoints, inputGrid, readOptions, points);
    const long double resident = source.encoded + retainedMetadata + records.capacity() * sizeof(PointRecord);
    auto field = load(velocityPath, velocityGrid, Kind::Velocity, p, resident);
    openvdb::Vec3DGrid::Ptr doubleField;
    double maximum;
    if (field.grid->isType<openvdb::Vec3SGrid>()) {
      const auto single = openvdb::gridPtrCast<openvdb::Vec3SGrid>(field.grid);
      maximum = velocity(*single, p);
      doubleField = promote(*single, p, resident + field.encoded + metaBytes(*field.metadata));
    } else {
      doubleField = openvdb::gridPtrCast<openvdb::Vec3DGrid>(field.grid);
      maximum = velocity(*doubleField, p);
    }
    budget(resident + field.encoded + doubleField->memUsage() + 4 * metaBytes(*doubleField) + Fixed, p,
           "point integration workspace exceeds the memory budget");
    field.grid.reset();
    advect(records, *doubleField, a, dx, maximum);
    doubleField.reset(); field.metadata.reset();
    return write(records, *source.metadata, output, p, points,
                 static_cast<long double>(source.encoded) + field.encoded);
  });
#else
  (void)inputPoints; (void)velocityPath; (void)output; (void)inputGrid; (void)velocityGrid; (void)a; (void)p; (void)points; unavailable();
#endif
}

PointResult WriteSampledPoints(const std::filesystem::path& inputPoints, const std::filesystem::path& fieldPath,
                              const std::filesystem::path& output, PointAttribute target,
                              const std::string& inputGrid, const std::string& fieldGrid,
                              const ProcessingOptions& p, const PointOptions& points) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p, points, output);
  if (target != PointAttribute::Velocity && target != PointAttribute::Radius)
    throw std::invalid_argument("point sampling target must be Velocity or Radius");
  initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    auto source = load(inputPoints, inputGrid, Kind::Points, p);
    pointLayout(*openvdb::gridPtrCast<PointGrid>(source.grid), p);
    const double dx = transform(*source.grid); source.grid.reset();
    const long double retainedMetadata = 4 * metaBytes(*source.metadata);
    budget(retainedMetadata + Fixed, p, "retained point metadata exceeds the memory budget");
    auto readOptions = p; readOptions.maxInputBytes -= static_cast<uintmax_t>(retainedMetadata);
    auto records = ReadPoints(inputPoints, inputGrid, readOptions, points);
    const long double resident = source.encoded + retainedMetadata + records.capacity() * sizeof(PointRecord);
    auto field = load(fieldPath, fieldGrid, target == PointAttribute::Velocity ? Kind::Velocity : Kind::Scalar, p, resident);
    if (target == PointAttribute::Velocity) {
      openvdb::Vec3DGrid::Ptr data;
      if (field.grid->isType<openvdb::Vec3SGrid>()) {
        const auto single = openvdb::gridPtrCast<openvdb::Vec3SGrid>(field.grid);
        (void)velocity(*single, p); data = promote(*single, p, resident + field.encoded + metaBytes(*field.metadata));
      } else { data = openvdb::gridPtrCast<openvdb::Vec3DGrid>(field.grid); (void)velocity(*data, p); }
      field.grid.reset();
      budget(resident + field.encoded + data->memUsage() + 4 * metaBytes(*data) + Fixed, p,
             "point vector sampling workspace exceeds the memory budget");
      sampleRecords(records, *data, dx);
    } else {
      openvdb::DoubleGrid::Ptr data;
      if (field.grid->isType<openvdb::FloatGrid>()) {
        const auto single = openvdb::gridPtrCast<openvdb::FloatGrid>(field.grid);
        scalar(*single, p); data = promote(*single, p, resident + field.encoded + metaBytes(*field.metadata));
      } else { data = openvdb::gridPtrCast<openvdb::DoubleGrid>(field.grid); scalar(*data, p); }
      field.grid.reset();
      budget(resident + field.encoded + data->memUsage() + 4 * metaBytes(*data) + Fixed, p,
             "point scalar sampling workspace exceeds the memory budget");
      sampleRecords(records, *data, dx);
    }
    field.grid.reset(); field.metadata.reset();
    return write(records, *source.metadata, output, p, points,
                 static_cast<long double>(source.encoded) + field.encoded);
  });
#else
  (void)inputPoints; (void)fieldPath; (void)output; (void)target; (void)inputGrid; (void)fieldGrid; (void)p; (void)points; unavailable();
#endif
}
} // namespace container::geometry::vdb
