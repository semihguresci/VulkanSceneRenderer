#include "Container/geometry/OpenVdbFluidSimulation.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbFluidViscosity.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <openvdb/openvdb.h>
#include <set>
#include <tbb/task_arena.h>
#include <vector>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB fluid simulation is unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using Scalar = openvdb::DoubleGrid;
using Mac = openvdb::Vec3DGrid;
using Mask = openvdb::BoolGrid;
constexpr long double Fixed = 8.0L * 1024 * 1024;
long double metadataBytes(const openvdb::MetaMap &map) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto it = map.beginMeta(); it != map.endMeta(); ++it)
    bytes += 2.0L * (it->first.size() + it->second->typeName().size() +
                     it->second->size()) +
             256;
  return bytes;
}
void budget(long double bytes, const ProcessingOptions &p, const char *reason) {
  if (!std::isfinite(bytes) || bytes > p.maxInputBytes)
    throw std::invalid_argument(reason);
}
openvdb::Coord axisOffset(size_t axis, int value = 1) {
  openvdb::Coord offset(0);
  offset[axis] = value;
  return offset;
}
void options(const FluidSimulationOptions &s, const ProcessingOptions &p,
             const std::filesystem::path &output) {
  if (!std::isfinite(s.timeStep) || s.timeStep < 1e-6 || s.timeStep > 3600 ||
      s.substeps < 1 || s.substeps > 64 || s.maxSubsteps < s.substeps ||
      s.maxSubsteps > 64 || s.timeStep / s.substeps < 1e-6 ||
      !std::isfinite(s.maxCfl) || s.maxCfl <= 0 || s.maxCfl > .5 ||
      !std::isfinite(s.tolerance) || s.tolerance < 1e-12 ||
      s.tolerance > 1e-2 || s.maxIterations < 1 || s.maxIterations > 10000 ||
      s.maxFluidCells < 1 || s.maxFluidCells > 1'000'000)
    throw std::invalid_argument(
        "smoke step requires finite dt [1e-6,3600], actual steps >=1e-6, 1..64 "
        "substeps, CFL (0,.5], valid projection limits");
  for (double g : s.gravity)
    if (!std::isfinite(g) || std::abs(g) > 1e12)
      throw std::invalid_argument(
          "smoke world gravity components must be finite and at most 1e12");
  for (double coefficient : {s.buoyancyDensity, s.buoyancyTemperature})
    if (!std::isfinite(coefficient) || coefficient < 0 || coefficient > 1e6)
      throw std::invalid_argument(
          "smoke buoyancy coefficients must be in [0,1e6]");
  if (!std::isfinite(s.ambientTemperature) || s.ambientTemperature < 0 ||
      s.ambientTemperature > 1e6)
    throw std::invalid_argument("smoke ambient temperature must be in [0,1e6]");
  if (!std::isfinite(s.kinematicViscosity) || s.kinematicViscosity < 0 ||
      s.kinematicViscosity > 1e6 || s.sources.size() > 64)
    throw std::invalid_argument(
        "smoke viscosity must be in [0,1e6] and source spheres <=64");
  double directionLength = 0;
  for (double component : s.buoyancyDirection) {
    if (!std::isfinite(component))
      throw std::invalid_argument(
          "buoyancy direction must be finite and unit length");
    directionLength = std::hypot(directionLength, component);
  }
  if (std::abs(directionLength - 1) > 1e-10)
    throw std::invalid_argument("buoyancy direction must have unit length");
  std::set<std::string> names;
  for (const auto &name :
       {s.densityGridName, s.temperatureGridName, s.velocityGridName,
        s.pressureGridName, s.domainGridName})
    if (name.empty() || name.size() > 256 || !names.insert(name).second)
      throw std::invalid_argument("all five smoke output names must be "
                                  "distinct and contain 1..256 bytes");
  if (!p.maxVoxelCount || !p.maxInputBytes)
    throw std::invalid_argument("smoke processing budgets must be positive");
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "smoke output must be a new path in an existing directory");
}
enum class Kind { Density, Temperature, Velocity, Domain };
openvdb::GridBase::Ptr load(const std::filesystem::path &path,
                            const std::string &selection, Kind kind,
                            long double resident, const ProcessingOptions &p) {
  detail::RequireSeekableFile(path);
  openvdb::io::File input(path.string());
  input.open(false);
  std::set<std::string> descriptors;
  long double catalog = resident + Fixed;
  for (auto it = input.beginName(); it != input.endName(); ++it) {
    const auto name = it.gridName();
    catalog += 2.0L * name.size() + 128;
    budget(catalog, p, "smoke descriptor catalog exceeds memory budget");
    if (descriptors.size() >= 4096 || !descriptors.insert(name).second)
      throw std::invalid_argument("smoke input has too many or colliding grid "
                                  "descriptors; rename bracket-suffixed grids");
  }
  std::string chosen;
  bool found = false;
  for (auto it = input.beginName(); it != input.endName(); ++it) {
    const auto meta = input.readGridMetadata(it.gridName());
    if (!GridSelectionMatches(selection, it.gridName(), meta->getName()))
      continue;
    const bool scalar = meta->isType<openvdb::FloatGrid>() ||
                        meta->isType<openvdb::DoubleGrid>();
    const bool supported =
        kind == Kind::Velocity ? meta->isType<openvdb::Vec3SGrid>() ||
                                     meta->isType<openvdb::Vec3DGrid>()
        : kind == Kind::Domain
            ? scalar || meta->isType<Mask>()
            : scalar && (kind != Kind::Density ||
                         meta->getGridClass() == openvdb::GRID_FOG_VOLUME);
    if (!supported) {
      if (!selection.empty())
        throw std::invalid_argument(
            "selected smoke grid has an unsupported type/class");
      continue;
    }
    if (found)
      throw std::invalid_argument("smoke grid selection is ambiguous; use "
                                  "descriptor:<exact descriptor name>");
    const auto count = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_VOXEL_COUNT);
    const auto bytes = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!count || !bytes || count->value() < 0 || bytes->value() < 0 ||
        uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument("smoke input needs valid recorded grid "
                                  "statistics within the voxel budget");
    budget(resident + bytes->value() + 4 * metadataBytes(*meta) + Fixed, p,
           "smoke recorded input exceeds memory budget before loading");
    chosen = it.gridName();
    found = true;
    if (selection.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument("no matching smoke input grid found");
  auto result = input.readGrid(chosen);
  input.close();
  budget(resident + result->memUsage() + 4 * metadataBytes(*result) + Fixed, p,
         "loaded smoke input exceeds memory budget");
  constexpr uint64_t leafValues = 512;
  if (result->activeVoxelCount() > p.maxVoxelCount ||
      result->baseTree().leafCount() >
          p.maxVoxelCount / leafValues + (p.maxVoxelCount % leafValues != 0))
    throw std::invalid_argument(
        "stored smoke input topology exceeds voxel budget");
  return result;
}
double transform(const openvdb::GridBase &grid) {
  if (!grid.transform().isLinear() || !grid.hasUniformVoxels())
    throw std::invalid_argument(
        "smoke domain requires uniform orthogonal affine voxels");
  const auto map = grid.transform().baseMap();
  std::array<openvdb::Vec3d, 3> axes;
  double h = 0;
  for (size_t a = 0; a < 3; ++a) {
    openvdb::Vec3d unit(0);
    unit[a] = 1;
    axes[a] = map->applyJacobian(unit);
    const double length = std::hypot(axes[a].x(), axes[a].y(), axes[a].z());
    if (!std::isfinite(length) || length < 1e-6 || length > 1e6 ||
        (a && std::abs(length / h - 1) > 1e-9))
      throw std::invalid_argument(
          "smoke voxel axes must be uniform and in [1e-6,1e6]");
    if (!a)
      h = length;
    axes[a] /= length;
  }
  for (size_t a = 0; a < 3; ++a)
    for (size_t b = a + 1; b < 3; ++b)
      if (std::abs(axes[a].dot(axes[b])) > 1e-9)
        throw std::invalid_argument("smoke domain axes must be orthogonal");
  return h;
}
template <class Grid>
Mask::Ptr maskFrom(const Grid &source, const FluidSimulationOptions &s,
                   const ProcessingOptions &p, long double resident) {
  if (source.background() != 0)
    throw std::invalid_argument("smoke domain background must be zero");
  if (source.activeVoxelCount() > std::min(s.maxFluidCells, p.maxVoxelCount))
    throw std::invalid_argument(
        "smoke domain active topology exceeds cell cap");
  for (auto value = source.cbeginValueAll(); value; ++value)
    if (!std::isfinite(double(*value)) || double(*value) < 0)
      throw std::invalid_argument(
          "smoke domain stored values must be finite nonnegative");
  using Tree = Mac::TreeType;
  using Leaf = Tree::LeafNodeType;
  using Upper = Tree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  const auto nodes = source.tree().nodeCount();
  long double leaves = nodes[0];
  for (auto value = source.cbeginValueOn(); value; ++value) {
    if (value.isTileValue())
      leaves += static_cast<long double>(value.getVoxelCount()) / 512;
    const auto bounds = value.getBoundingBox();
    for (size_t a = 0; a < 3; ++a)
      if (int64_t(bounds.min()[a]) < int64_t(INT32_MIN) + 16 ||
          int64_t(bounds.max()[a]) > int64_t(INT32_MAX) - 16)
        throw std::invalid_argument(
            "smoke domain exceeds safe stencil coordinates");
  }
  const long double prospective =
      16 * (leaves * (sizeof(Leaf) + 512 * sizeof(openvdb::Vec3d) + 256.0L) +
            nodes[1] * (sizeof(Lower) + 256.0L) +
            nodes[2] * (sizeof(Upper) + 512.0L));
  budget(resident + prospective + source.memUsage() +
             4 * metadataBytes(source) + source.activeVoxelCount() * 2048.0L +
             Fixed,
         p, "smoke expanded state/transport workspace exceeds budget");
  if (source.activeVoxelCount() > p.maxVoxelCount / 8)
    throw std::invalid_argument(
        "smoke expanded state topology exceeds summed voxel budget");
  auto mask = Mask::create(false);
  mask->setTransform(detail::CopyTransformExact(source.transform()));
  mask->insertMeta(source);
  for (auto value = source.cbeginValueOn(); value; ++value)
    if (double(*value) > 0)
      mask->tree().fill(value.getBoundingBox(), true, true);
  mask->tree().voxelizeActiveTiles();
  if (mask->empty())
    throw std::invalid_argument(
        "smoke domain contains no positive active cells");
  return mask;
}
void worldPrecision(const Mask &mask, double h) {
  auto bounds = mask.evalActiveVoxelBoundingBox();
  bounds.expand(1);
  for (unsigned corner = 0; corner < 8; ++corner) {
    openvdb::Vec3d index;
    for (size_t a = 0; a < 3; ++a)
      index[a] = corner & (1u << a) ? bounds.max()[a] : bounds.min()[a];
    const auto world = mask.indexToWorld(index);
    for (size_t a = 0; a < 3; ++a) {
      const double gap =
          std::max(std::abs(std::nextafter(world[a], INFINITY) - world[a]),
                   std::abs(std::nextafter(world[a], -INFINITY) - world[a]));
      if (!std::isfinite(world[a]) || !std::isfinite(gap) || gap > h * 1e-4)
        throw std::invalid_argument(
            "smoke world coordinates have insufficient precision");
    }
  }
}
template <class Grid>
Scalar::Ptr scalarFrom(const Grid &source, const Mask &mask) {
  if (source.transform() != mask.transform() || source.background() != 0)
    throw std::invalid_argument(
        "smoke scalars require the exact domain transform and zero background");
  for (auto value = source.cbeginValueAll(); value; ++value) {
    const double v = double(*value);
    if (!std::isfinite(v) || v < 0 || v > 1e6 || (!value.isValueOn() && v != 0))
      throw std::invalid_argument(
          "smoke scalar values must be in [0,1e6], with inactive values zero");
    if (v != 0) {
      const auto bounds = value.getBoundingBox();
      for (auto c = bounds.begin(); c; ++c)
        if (!mask.tree().isValueOn(*c))
          throw std::invalid_argument(
              "smoke scalar has nonzero values outside fluid domain");
    }
  }
  auto output = Scalar::create(0);
  output->setTransform(detail::CopyTransformExact(mask.transform()));
  output->insertMeta(source);
  for (auto cell = mask.cbeginValueOn(); cell; ++cell)
    output->tree().setValue(cell.getCoord(),
                            double(source.tree().getValue(cell.getCoord())));
  return output;
}
template <class Grid> Mac::Ptr macFrom(const Grid &source, const Mask &mask) {
  if (source.transform() != mask.transform() ||
      source.getGridClass() != openvdb::GRID_STAGGERED ||
      source.isInWorldSpace() ||
      source.getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE)
    throw std::invalid_argument(
        "smoke velocity must use native M04 negative-face local MAC with exact "
        "domain transform; prepare world fields with --project");
  const auto finite = [](const auto &v) {
    for (size_t a = 0; a < 3; ++a)
      if (!std::isfinite(double(v[a])) || std::abs(double(v[a])) > 1e24)
        return false;
    return true;
  };
  if (!finite(source.background()))
    throw std::invalid_argument("smoke MAC background exceeds finite range");
  for (auto value = source.cbeginValueAll(); value; ++value)
    if (!finite(*value))
      throw std::invalid_argument("smoke MAC values exceed finite range");
  auto output = Mac::create(openvdb::Vec3d(0));
  output->setTransform(detail::CopyTransformExact(mask.transform()));
  output->insertMeta(source);
  for (auto cell = mask.cbeginValueOn(); cell; ++cell) {
    const auto c = cell.getCoord();
    openvdb::Vec3d v(0);
    for (size_t a = 0; a < 3; ++a)
      if (mask.tree().isValueOn(c - axisOffset(a)))
        v[a] = double(source.tree().getValue(c)[a]);
    output->tree().setValue(c, v);
    for (size_t a = 0; a < 3; ++a)
      if (!mask.tree().isValueOn(c + axisOffset(a)))
        output->tree().setValueOff(c + axisOffset(a), openvdb::Vec3d(0));
  }
  return output;
}
struct State {
  Mask::Ptr mask;
  Mac::Ptr velocity;
  Scalar::Ptr density, temperature, pressure;
  openvdb::Int32Grid::Ptr components;
};
void labelComponents(State &state) {
  state.components = openvdb::Int32Grid::create(-1);
  std::vector<openvdb::Coord> queue;
  int32_t component = 0;
  for (auto value = state.mask->cbeginValueOn(); value; ++value) {
    const auto start = value.getCoord();
    if (state.components->tree().getValue(start) >= 0)
      continue;
    queue.push_back(start);
    state.components->tree().setValue(start, component);
    while (!queue.empty()) {
      const auto c = queue.back();
      queue.pop_back();
      for (size_t a = 0; a < 3; ++a)
        for (int sign : {-1, 1}) {
          const auto next = c + axisOffset(a, sign);
          if (state.mask->tree().isValueOn(next) &&
              state.components->tree().getValue(next) < 0) {
            state.components->tree().setValue(next, component);
            queue.push_back(next);
          }
        }
    }
    ++component;
  }
}
long double memory(const State &state) {
  long double bytes = 0;
  for (const openvdb::GridBase::Ptr &grid :
       openvdb::GridPtrVec{state.mask, state.velocity, state.density,
                           state.temperature, state.pressure, state.components})
    if (grid)
      bytes += grid->memUsage() + 4 * metadataBytes(*grid);
  return bytes;
}
void allocationBudget(const State &state, const openvdb::GridBase &source,
                      long double encoded, const ProcessingOptions &p) {
  using Tree = Mac::TreeType;
  using Leaf = Tree::LeafNodeType;
  using Upper = Tree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  const auto nodes = state.mask->tree().nodeCount();
  const long double prospective =
      4 * (nodes[0] * (sizeof(Leaf) + 512 * sizeof(openvdb::Vec3d) + 256.0L) +
           nodes[1] * (sizeof(Lower) + 256.0L) +
           nodes[2] * (sizeof(Upper) + 512.0L));
  budget(encoded + memory(state) + source.memUsage() +
             4 * metadataBytes(source) + prospective + Fixed,
         p,
         "smoke input conversion and expanded state exceed memory budget "
         "before allocation");
}
void stateBudget(const State &state, long double encoded,
                 const ProcessingOptions &p, bool scratch = true) {
  budget(encoded + (scratch ? 3 : 1) * memory(state) + Fixed, p,
         "smoke retained state/transport workspace exceeds memory budget");
  uint64_t leaves = 0, active = 0;
  for (const openvdb::GridBase::Ptr &grid :
       openvdb::GridPtrVec{state.mask, state.velocity, state.density,
                           state.temperature, state.pressure})
    if (grid) {
      leaves += grid->baseTree().leafCount();
      active += grid->activeVoxelCount();
    }
  if (active > p.maxVoxelCount ||
      leaves > p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument(
        "summed smoke state topology exceeds voxel budget");
}
openvdb::Coord cellAt(const openvdb::Vec3d &p) {
  return {int32_t(std::floor(p.x() + .5)), int32_t(std::floor(p.y() + .5)),
          int32_t(std::floor(p.z() + .5))};
}
struct TraceHit {
  bool blocked;
  double time;
};
TraceHit traceHit(const Mask &mask, const openvdb::Vec3d &start,
                  const openvdb::Vec3d &end) {
  auto cell = cellAt(start);
  const auto delta = end - start;
  if (!mask.tree().isValueOn(cell))
    throw std::logic_error("smoke trace starts outside fluid");
  std::array<double, 3> next, increment;
  for (size_t a = 0; a < 3; ++a) {
    if (delta[a] == 0)
      next[a] = increment[a] = INFINITY;
    else {
      next[a] = std::max(
          0.0,
          (double(cell[a]) + (delta[a] > 0 ? .5 : -.5) - start[a]) / delta[a]);
      increment[a] = 1 / std::abs(delta[a]);
    }
  }
  for (unsigned crossing = 0; crossing < 32; ++crossing) {
    const double t = std::min({next[0], next[1], next[2]});
    if (t > 1 || !std::isfinite(t))
      return {false, 1};
    auto candidate = cell;
    unsigned crossed = 0;
    for (size_t a = 0; a < 3; ++a)
      if (next[a] <= t) {
        const int step = delta[a] > 0 ? 1 : -1;
        candidate[a] += step;
        crossed |= 1u << a;
      }
    // A corner cannot tunnel past a solid face/edge neighbor. Triple ties
    // require all seven nonempty subsets, not just singles and the diagonal.
    for (unsigned subset = crossed; subset; subset = (subset - 1) & crossed) {
      auto neighbor = cell;
      for (size_t a = 0; a < 3; ++a)
        if (subset & (1u << a))
          neighbor[a] += delta[a] > 0 ? 1 : -1;
      if (!mask.tree().isValueOn(neighbor))
        return {true, t};
    }
    for (size_t a = 0; a < 3; ++a)
      if (next[a] <= t)
        next[a] += increment[a];
    cell = candidate;
  }
  throw std::invalid_argument(
      "smoke characteristic exceeds bounded DDA crossings");
}
openvdb::Vec3d clipTrace(const Mask &mask, const openvdb::Vec3d &start,
                         const openvdb::Vec3d &end, uint64_t &clipped) {
  const auto hit = traceHit(mask, start, end);
  if (!hit.blocked)
    return end;
  ++clipped;
  auto position = start + (end - start) * std::nextafter(hit.time, 0.0);
  // Parameter nextafter alone can disappear when added to a large index.
  // Move index coordinates toward the known fluid start until inside again.
  for (unsigned attempt = 0;
       attempt < 4 && !mask.tree().isValueOn(cellAt(position)); ++attempt)
    for (size_t a = 0; a < 3; ++a)
      position[a] = std::nextafter(position[a], start[a]);
  if (!mask.tree().isValueOn(cellAt(position)))
    return start;
  return position;
}
bool visible(const Mask &mask, const openvdb::Vec3d &start,
             const openvdb::Vec3d &end) {
  const auto hit = traceHit(mask, start, end);
  return !hit.blocked ||
         hit.time >= 1; // A boundary face itself is a valid zero normal sample.
}
template <class Value>
Value interpolate(const Mask &mask, const openvdb::Vec3d &point,
                  const auto &valueAt, const auto &validAt) {
  const openvdb::Coord base(int32_t(std::floor(point.x())),
                            int32_t(std::floor(point.y())),
                            int32_t(std::floor(point.z())));
  const auto fraction = point - base.asVec3d();
  Value sum(0);
  double weights = 0;
  Value minimum = std::numeric_limits<Value>::infinity(), maximum = -minimum;
  for (int x = 0; x < 2; ++x)
    for (int y = 0; y < 2; ++y)
      for (int z = 0; z < 2; ++z) {
        const auto c = base + openvdb::Coord(x, y, z);
        const double w = (x ? fraction.x() : 1 - fraction.x()) *
                         (y ? fraction.y() : 1 - fraction.y()) *
                         (z ? fraction.z() : 1 - fraction.z());
        if (w > 0 && validAt(c)) {
          const Value value = valueAt(c);
          sum += value * w;
          weights += w;
          minimum = std::min(minimum, value);
          maximum = std::max(maximum, value);
        }
      }
  (void)mask;
  if (!(weights > 0))
    return Value(0);
  if (minimum == maximum)
    return minimum; // Preserve constants exactly, including range endpoints.
  const Value value = sum / weights;
  if (!std::isfinite(value))
    throw std::invalid_argument("smoke interpolation is not finite");
  return std::clamp(value, minimum, maximum);
}
openvdb::Vec3d sampleMac(const State &state, const openvdb::Vec3d &point,
                         int32_t component) {
  openvdb::Vec3d result;
  for (size_t a = 0; a < 3; ++a) {
    auto position = point;
    position[a] += .5; // StaggeredBoxSampler's negative-face convention.
    result[a] = interpolate<double>(
        *state.mask, position,
        [&](const auto &c) { return state.velocity->tree().getValue(c)[a]; },
        [&](const auto &c) {
          if (state.components->tree().getValue(c) != component &&
              state.components->tree().getValue(c - axisOffset(a)) != component)
            return false;
          auto face = c.asVec3d();
          face[a] -= .5;
          return visible(*state.mask, point, face);
        });
  }
  return result;
}
openvdb::Vec3d departure(const State &state, const openvdb::Vec3d &position,
                         int32_t component, double dt, uint64_t &clipped) {
  const auto midpoint = clipTrace(
      *state.mask, position,
      position - sampleMac(state, position, component) * (dt * .5), clipped);
  return clipTrace(*state.mask, position,
                   position - sampleMac(state, midpoint, component) * dt,
                   clipped);
}
void transport(State &state, double dt, uint64_t &clipped) {
  auto velocity = Mac::create(openvdb::Vec3d(0));
  velocity->setTransform(detail::CopyTransformExact(state.mask->transform()));
  velocity->insertMeta(*state.velocity);
  auto density = Scalar::create(0), temperature = Scalar::create(0);
  density->setTransform(detail::CopyTransformExact(state.mask->transform()));
  temperature->setTransform(detail::CopyTransformExact(state.mask->transform()));
  density->insertMeta(*state.density);
  temperature->insertMeta(*state.temperature);
  for (auto cell = state.mask->cbeginValueOn(); cell; ++cell) {
    const auto c = cell.getCoord();
    const auto component = state.components->tree().getValue(c);
    const auto back = departure(state, c.asVec3d(), component, dt, clipped);
    const auto valid = [&](const auto &p) {
      return state.components->tree().getValue(p) == component &&
             visible(*state.mask, back, p.asVec3d());
    };
    density->tree().setValue(
        c, interpolate<double>(
               *state.mask, back,
               [&](const auto &p) { return state.density->tree().getValue(p); },
               valid));
    temperature->tree().setValue(
        c, interpolate<double>(
               *state.mask, back,
               [&](const auto &p) {
                 return state.temperature->tree().getValue(p);
               },
               valid));
    openvdb::Vec3d value(0);
    for (size_t a = 0; a < 3; ++a)
      if (state.mask->tree().isValueOn(c - axisOffset(a))) {
        auto face = c.asVec3d();
        face[a] -= .5;
        value[a] =
            sampleMac(state, departure(state, face, component, dt, clipped),
                      component)[a];
      }
    velocity->tree().setValue(c, value);
  }
  state.velocity = velocity;
  state.density = density;
  state.temperature = temperature;
}

double product(double value, std::initializer_list<double> factors) {
  if (value == 0)
    return 0;
  int exponent = 0;
  double mantissa = std::frexp(value, &exponent);
  for (double factor : factors) {
    int e;
    mantissa *= std::frexp(factor, &e);
    exponent += e;
  }
  const double result = std::ldexp(mantissa, exponent);
  if (!std::isfinite(result) || result == 0)
    throw std::invalid_argument(
        "smoke force cannot be represented in finite precision");
  return result;
}
openvdb::Vec3d localVector(const openvdb::math::MapBase &map,
                           const openvdb::Vec3d &world, double dt) {
  std::array<openvdb::Vec3d, 3> inverse;
  for (size_t a = 0; a < 3; ++a) {
    openvdb::Vec3d unit(0);
    unit[a] = 1;
    inverse[a] = map.applyInverseJacobian(unit);
  }
  openvdb::Vec3d output(0);
  for (size_t a = 0; a < 3; ++a) {
    double sum = 0, correction = 0;
    for (size_t b = 0; b < 3; ++b) {
      const double term = world[b] == 0 || inverse[b][a] == 0
                              ? 0
                              : product(world[b], {inverse[b][a], dt});
      const double next = sum + term;
      correction += std::abs(sum) >= std::abs(term) ? (sum - next) + term
                                                    : (term - next) + sum;
      sum = next;
    }
    output[a] = sum + correction;
    if (!std::isfinite(output[a]))
      throw std::invalid_argument("smoke force sum is not finite");
  }
  return output;
}
openvdb::Vec3d acceleration(const State &state, const openvdb::Coord &c,
                            const FluidSimulationOptions &s) {
  const double temperature =
      state.temperature->tree().getValue(c) - s.ambientTemperature;
  const double density = state.density->tree().getValue(c);
  const double thermal = s.buoyancyTemperature == 0 || temperature == 0
                             ? 0
                             : product(s.buoyancyTemperature, {temperature});
  const double weight = s.buoyancyDensity == 0 || density == 0
                            ? 0
                            : product(s.buoyancyDensity, {density});
  const double buoyancy = thermal - weight;
  openvdb::Vec3d result;
  for (size_t a = 0; a < 3; ++a) {
    result[a] = s.gravity[a] + s.buoyancyDirection[a] * buoyancy;
    if (!std::isfinite(result[a]) || std::abs(result[a]) > 1e12)
      throw std::invalid_argument(
          "combined smoke acceleration components exceed 1e12");
  }
  return result;
}
double forceBound(const State &state, const FluidSimulationOptions &s) {
  std::array<double, 3> maximum{};
  for (auto cell = state.mask->cbeginValueOn(); cell; ++cell) {
    const auto force = acceleration(state, cell.getCoord(), s);
    for (size_t a = 0; a < 3; ++a)
      maximum[a] = std::max(maximum[a], std::abs(force[a]));
  }
  const auto map = state.mask->transform().baseMap();
  openvdb::Vec3d bound(0);
  for (size_t b = 0; b < 3; ++b) {
    openvdb::Vec3d unit(0);
    unit[b] = 1;
    const auto inverse = map->applyInverseJacobian(unit);
    for (size_t a = 0; a < 3; ++a)
      if (maximum[b] != 0 && inverse[a] != 0)
        bound[a] += product(maximum[b], {std::abs(inverse[a])});
  }
  return std::max({bound.x(), bound.y(), bound.z()});
}
void forces(State &state, const FluidSimulationOptions &s, double dt) {
  const auto map = state.mask->transform().baseMap();
  for (auto cell = state.mask->cbeginValueOn(); cell; ++cell) {
    const auto c = cell.getCoord();
    auto v = state.velocity->tree().getValue(c);
    for (size_t a = 0; a < 3; ++a)
      if (state.mask->tree().isValueOn(c - axisOffset(a))) {
        const auto left = localVector(*map, acceleration(state, c, s), dt * .5);
        const auto right = localVector(
            *map, acceleration(state, c - axisOffset(a), s), dt * .5);
        v[a] += left[a] + right[a];
        if (!std::isfinite(v[a]) || std::abs(v[a]) > 1e24)
          throw std::invalid_argument(
              "smoke predictor MAC exceeds supported finite range");
      }
    state.velocity->tree().setValue(c, v);
  }
}
double maximumLocal(const State &state) {
  double maximum = 0;
  for (auto value = state.velocity->cbeginValueOn(); value; ++value)
    for (size_t a = 0; a < 3; ++a)
      maximum = std::max(maximum, std::abs((*value)[a]));
  return maximum;
}
double maximumWorld(const State &state) {
  double maximum = 0;
  const auto map = state.mask->transform().baseMap();
  for (auto cell = state.mask->cbeginValueOn(); cell; ++cell) {
    const auto vector = map->applyJacobian(
        sampleMac(state, cell.getCoord().asVec3d(),
                  state.components->tree().getValue(cell.getCoord())));
    maximum = std::max(maximum, std::hypot(vector.x(), vector.y(), vector.z()));
  }
  return maximum;
}
double integral(const Scalar &scalar, const Mask &mask, double h) {
  long double sum = 0;
  for (auto cell = mask.cbeginValueOn(); cell; ++cell)
    sum += scalar.tree().getValue(cell.getCoord());
  const double result = double(sum * static_cast<long double>(h) * h * h);
  if (!std::isfinite(result))
    throw std::invalid_argument("smoke diagnostic integral is not finite");
  return result;
}
struct Temporary {
  std::filesystem::path directory;
  std::vector<std::filesystem::path> files;
  explicit Temporary(const std::filesystem::path &output) {
    static std::atomic_uint64_t serial{0};
    const auto parent = output.has_parent_path() ? output.parent_path()
                                                 : std::filesystem::path(".");
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      directory =
          parent /
          (".container-fluid-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(directory))
        return;
    }
    throw std::runtime_error("cannot create owned smoke temporary directory");
  }
  std::filesystem::path add(const std::string &name) {
    auto path = directory / name;
    files.push_back(path);
    return path;
  }
  ~Temporary() {
    std::error_code ignored;
    for (const auto &file : files)
      std::filesystem::remove(file, ignored);
    std::filesystem::remove(directory, ignored);
  }
};
void projection(State &state, Temporary &temporary, uint32_t step, double dt,
                long double encoded, const FluidSimulationOptions &s,
                const ProcessingOptions &p, FluidSimulationResult &result) {
  state.pressure.reset();
  const long double reserve = encoded + memory(state) + Fixed;
  budget(reserve + 1, p, "smoke retained fields leave no projection workspace");
  auto limits = p;
  limits.maxInputBytes = static_cast<uintmax_t>(
      static_cast<long double>(p.maxInputBytes) - reserve);
  const auto predictor =
      temporary.add("predictor-" + std::to_string(step) + ".vdb");
  const auto projected =
      temporary.add("projected-" + std::to_string(step) + ".vdb");
  state.velocity->setName(s.velocityGridName);
  state.mask->setName(s.domainGridName);
  for(const auto& grid:openvdb::GridPtrVec{state.velocity,state.mask})
    detail::RemoveBoundaryProjectionMetadata(*grid);
  detail::WriteFileExclusive(predictor, {state.velocity, state.mask},
                             limits.maxInputBytes);
  FluidProjectionOptions options;
  options.timeStep = dt;
  options.tolerance = s.tolerance;
  options.maxIterations = s.maxIterations;
  options.maxFluidCells = s.maxFluidCells;
  options.velocityGridName = s.velocityGridName;
  options.pressureGridName = s.pressureGridName;
  options.domainGridName = s.domainGridName;
  const auto projectedResult =
      WriteProjectedVelocity(predictor, predictor, projected, options,
                             "descriptor:" + s.velocityGridName,
                             "descriptor:" + s.domainGridName, limits);
  state.velocity.reset();
  const auto projectedBytes = std::filesystem::file_size(projected);
  state.velocity = openvdb::gridPtrCast<Mac>(
      load(projected, "descriptor:" + s.velocityGridName, Kind::Velocity,
           encoded + projectedBytes + memory(state), p));
  state.pressure = openvdb::gridPtrCast<Scalar>(
      load(projected, "descriptor:" + s.pressureGridName, Kind::Temperature,
           encoded + projectedBytes + memory(state), p));
  if (!state.velocity || !state.pressure)
    throw std::logic_error("projection returned incompatible smoke state");
  result.maxProjectionIterations =
      std::max(result.maxProjectionIterations, projectedResult.iterations);
  result.maxDivergenceBefore =
      std::max(result.maxDivergenceBefore, projectedResult.maxDivergenceBefore);
  result.maxDivergenceAfter = projectedResult.maxDivergenceAfter;
  result.residual = std::max(result.residual, projectedResult.residual);
  result.divergenceRoundoffBound = std::max(
      result.divergenceRoundoffBound, projectedResult.divergenceRoundoffBound);
  result.roundoffLimited |= projectedResult.roundoffLimited;
  std::filesystem::remove(predictor);
  std::filesystem::remove(projected);
}
void structure(openvdb::GridBase &grid, const std::string &name,
               openvdb::GridClass kind) {
  detail::RemoveBoundaryProjectionMetadata(grid);
  const auto statistics = grid.getStatsMetadata();
  for (auto it = statistics->beginMeta(); it != statistics->endMeta(); ++it)
    grid.removeMeta(it->first);
  for (const char *key : {"container:mac_layout", "container:velocity_units",
                          "container:projection_dt", "container:pressure_gauge",
                          "container:pressure_units"})
    grid.removeMeta(key);
  grid.setName(name);
  grid.setGridClass(kind);
  grid.setVectorType(openvdb::VEC_INVARIANT);
  grid.setIsInWorldSpace(true);
}

struct DiagnosticSum {
  double value{0}, correction{0};
  void add(double contribution) {
    const double next = value + contribution;
    correction += std::abs(value) >= std::abs(contribution)
                      ? (value - next) + contribution
                      : (contribution - next) + value;
    value = next;
    if (!std::isfinite(value) || !std::isfinite(correction))
      throw std::invalid_argument(
          "smoke added-work diagnostic sum is not finite");
  }
  double total() const {
    const double result = value + correction;
    if (!std::isfinite(result))
      throw std::invalid_argument(
          "smoke added-work diagnostic sum is not finite");
    return result;
  }
};

void canonicalBridge(State &state, const FluidSimulationOptions &s,
                     long double encoded, const ProcessingOptions &p) {
  structure(*state.density, s.densityGridName, openvdb::GRID_FOG_VOLUME);
  structure(*state.temperature, s.temperatureGridName, openvdb::GRID_UNKNOWN);
  structure(*state.mask, s.domainGridName, openvdb::GRID_UNKNOWN);
  state.velocity->setName(s.velocityGridName);
  if (!state.pressure) {
    budget(encoded + memory(state) + 4 * metadataBytes(*state.mask) + Fixed, p,
           "smoke canonical pressure metadata exceeds bridge memory budget");
    state.pressure = Scalar::create(0);
    state.pressure->insertMeta(*state.mask);
    state.pressure->setTransform(detail::CopyTransformExact(state.mask->transform()));
    structure(*state.pressure, s.pressureGridName, openvdb::GRID_UNKNOWN);
  }
  state.pressure->setName(s.pressureGridName);
  for(const auto& grid:openvdb::GridPtrVec{state.density,state.temperature,state.velocity,state.pressure,state.mask})
    detail::RemoveBoundaryProjectionMetadata(*grid);
  stateBudget(state, encoded, p);
}

openvdb::GridPtrVec bridgeGrids(const State &state) {
  return {state.density, state.temperature, state.velocity, state.pressure,
          state.mask};
}
ProcessingOptions bridgeLimits(const State &state, long double encoded,
                               const ProcessingOptions &p) {
  const long double reserve = encoded + memory(state) + Fixed;
  budget(reserve + 1, p,
         "smoke retained state leaves no addon bridge workspace");
  auto limits = p;
  limits.maxInputBytes = static_cast<uintmax_t>(
      static_cast<long double>(p.maxInputBytes) - reserve);
  return limits;
}

void viscousProjection(State &state, Temporary &temporary, uint32_t step,
                       double dt, long double encoded,
                       const FluidSimulationOptions &s,
                       const ProcessingOptions &p,
                       FluidSimulationResult &result,
                       DiagnosticSum &dissipated) {
  canonicalBridge(state, s, encoded, p);
  auto limits = bridgeLimits(state, encoded, p);
  const auto predictor =
      temporary.add("viscous-input-" + std::to_string(step) + ".vdb");
  const auto projected =
      temporary.add("viscous-output-" + std::to_string(step) + ".vdb");
  detail::WriteFileExclusive(predictor, bridgeGrids(state),
                             limits.maxInputBytes);
  FluidViscosityOptions options;
  options.timeStep = dt;
  options.kinematicViscosity = s.kinematicViscosity;
  options.tolerance = s.tolerance;
  options.maxIterations = s.maxIterations;
  options.maxFluidCells = s.maxFluidCells;
  options.densityGridName = s.densityGridName;
  options.temperatureGridName = s.temperatureGridName;
  options.velocityGridName = s.velocityGridName;
  options.pressureGridName = s.pressureGridName;
  options.domainGridName = s.domainGridName;
  const auto solved =
      WriteViscousFluidState(predictor, projected, options, limits);
  // The helper has retired its input file. Drop that encoded bridge before
  // loading the result, while retained original sources remain charged.
  std::filesystem::remove(predictor);
  const auto bytes = std::filesystem::file_size(projected);
  state.velocity.reset();
  state.pressure.reset();
  state.velocity = openvdb::gridPtrCast<Mac>(
      load(projected, "descriptor:" + s.velocityGridName, Kind::Velocity,
           encoded + bytes + memory(state), p));
  state.pressure = openvdb::gridPtrCast<Scalar>(
      load(projected, "descriptor:" + s.pressureGridName, Kind::Temperature,
           encoded + bytes + memory(state), p));
  if (!state.velocity || !state.pressure)
    throw std::logic_error("viscosity returned incompatible smoke state");
  ++result.viscositySubsteps;
  result.maxDiffusionIterations =
      std::max(result.maxDiffusionIterations, solved.diffusionIterations);
  result.maxDiffusionResidual =
      std::max(result.maxDiffusionResidual, solved.diffusionResidual);
  // Tiny negative differences can only reflect the primitive's documented
  // roundoff allowance, not added dissipation. Do not invent negative loss.
  dissipated.add(std::max(0.0, solved.kineticEnergyBefore -
                                   solved.kineticEnergyAfterDiffusion));
  result.maxProjectionIterations =
      std::max(result.maxProjectionIterations, solved.projectionIterations);
  result.maxDivergenceBefore =
      std::max(result.maxDivergenceBefore, solved.maxDivergenceBefore);
  result.maxDivergenceAfter = solved.maxDivergenceAfter;
  result.residual = std::max(result.residual, solved.pressureResidual);
  result.divergenceRoundoffBound =
      std::max(result.divergenceRoundoffBound, solved.divergenceRoundoffBound);
  result.roundoffLimited |= solved.roundoffLimited;
  stateBudget(state, encoded + bytes, p, false);
  std::filesystem::remove(projected);
}

double changedIntegral(const Scalar &before, const Scalar &after,
                       const Mask &mask, double h) {
  DiagnosticSum sum;
  for (auto cell = mask.cbeginValueOn(); cell; ++cell)
    sum.add(after.tree().getValue(cell.getCoord()) -
            before.tree().getValue(cell.getCoord()));
  try {
    return product(sum.total(), {h, h, h});
  } catch (const std::invalid_argument &) {
    throw std::invalid_argument("smoke nonzero source integral delta cannot be "
                                "represented; rescale the state");
  }
}

void scalarSources(State &state, Temporary &temporary, uint32_t step, double dt,
                   long double encoded, double h,
                   const FluidSimulationOptions &s, const ProcessingOptions &p,
                   FluidSimulationResult &result, DiagnosticSum &massChange,
                   DiagnosticSum &temperatureChange) {
  canonicalBridge(state, s, encoded, p);
  auto limits = bridgeLimits(state, encoded, p);
  const auto predictor =
      temporary.add("sources-input-" + std::to_string(step) + ".vdb");
  const auto edited =
      temporary.add("sources-output-" + std::to_string(step) + ".vdb");
  detail::WriteFileExclusive(predictor, bridgeGrids(state),
                             limits.maxInputBytes);
  FluidSourcesOptions options;
  options.timeStep = dt;
  options.ambientTemperature = s.ambientTemperature;
  options.maxFluidCells = s.maxFluidCells;
  options.sources = s.sources;
  options.densityGridName = s.densityGridName;
  options.temperatureGridName = s.temperatureGridName;
  options.velocityGridName = s.velocityGridName;
  options.pressureGridName = s.pressureGridName;
  options.domainGridName = s.domainGridName;
  const auto changed = WriteFluidSources(predictor, edited, options, limits);
  std::filesystem::remove(predictor);
  const auto bytes = std::filesystem::file_size(edited);
  auto density = openvdb::gridPtrCast<Scalar>(
      load(edited, "descriptor:" + s.densityGridName, Kind::Density,
           encoded + bytes + memory(state), p));
  if (!density)
    throw std::logic_error("sources returned incompatible smoke density");
  auto temperature = openvdb::gridPtrCast<Scalar>(
      load(edited, "descriptor:" + s.temperatureGridName, Kind::Temperature,
           encoded + bytes + memory(state) + density->memUsage() +
               4 * metadataBytes(*density),
           p));
  if (!density || !temperature)
    throw std::logic_error("sources returned incompatible smoke scalar state");
  massChange.add(changedIntegral(*state.density, *density, *state.mask, h));
  temperatureChange.add(
      changedIntegral(*state.temperature, *temperature, *state.mask, h));
  state.density = std::move(density);
  state.temperature = std::move(temperature);
  ++result.sourceSubsteps;
  result.sourceAffectedCellApplications += changed.affectedCellCount;
  result.sourceChangedCellApplications += changed.changedCellCount;
  stateBudget(state, encoded + bytes, p, false);
  std::filesystem::remove(edited);
}
#endif
} // namespace

FluidSimulationResult WriteFluidStep(const FluidSimulationInputs &input,
                                     const std::filesystem::path &output,
                                     const FluidSimulationOptions &s,
                                     const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(s, p, output);
  long double encoded = 0;
  for (const auto &path :
       {input.density, input.velocity, input.domain, input.temperature})
    if (!path.empty()) {
      if (!std::filesystem::is_regular_file(path))
        throw std::invalid_argument("smoke input is not a regular file");
      encoded += std::filesystem::file_size(path);
      budget(encoded + Fixed, p,
             "summed smoke input files exceed memory budget");
    }
  if (input.density.empty() || input.velocity.empty() || input.domain.empty())
    throw std::invalid_argument(
        "smoke density, velocity and domain paths are required");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    State state;
    auto domain =
        load(input.domain, input.domainGrid, Kind::Domain, encoded, p);
    const double h = transform(*domain);
    if (auto grid = openvdb::gridPtrCast<Mask>(domain))
      state.mask = maskFrom(*grid, s, p, encoded);
    else if (auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(domain))
      state.mask = maskFrom(*grid, s, p, encoded);
    else
      state.mask =
          maskFrom(*openvdb::gridPtrCast<Scalar>(domain), s, p, encoded);
    worldPrecision(*state.mask, h);
    domain.reset();
    labelComponents(state);
    auto density = load(input.density, input.densityGrid, Kind::Density,
                        encoded + memory(state), p);
    allocationBudget(state, *density, encoded, p);
    if (auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(density))
      state.density = scalarFrom(*grid, *state.mask);
    else
      state.density =
          scalarFrom(*openvdb::gridPtrCast<Scalar>(density), *state.mask);
    density.reset();
    if (!input.temperature.empty()) {
      auto temperature = load(input.temperature, input.temperatureGrid,
                              Kind::Temperature, encoded + memory(state), p);
      allocationBudget(state, *temperature, encoded, p);
      if (auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(temperature))
        state.temperature = scalarFrom(*grid, *state.mask);
      else
        state.temperature =
            scalarFrom(*openvdb::gridPtrCast<Scalar>(temperature), *state.mask);
    } else {
      allocationBudget(state, *state.mask, encoded, p);
      state.temperature = Scalar::create(0);
      state.temperature->setTransform(detail::CopyTransformExact(state.mask->transform()));
      for (auto cell = state.mask->cbeginValueOn(); cell; ++cell)
        state.temperature->tree().setValue(cell.getCoord(),
                                           s.ambientTemperature);
    }
    auto velocity = load(input.velocity, input.velocityGrid, Kind::Velocity,
                         encoded + memory(state), p);
    allocationBudget(state, *velocity, encoded, p);
    if (auto grid = openvdb::gridPtrCast<openvdb::Vec3SGrid>(velocity))
      state.velocity = macFrom(*grid, *state.mask);
    else
      state.velocity =
          macFrom(*openvdb::gridPtrCast<Mac>(velocity), *state.mask);
    velocity.reset();
    stateBudget(state, encoded, p);
    FluidSimulationResult result;
    result.fluidCellCount = state.mask->activeVoxelCount();
    result.requestedSubsteps = s.substeps;
    result.densityMassBefore = integral(*state.density, *state.mask, h);
    result.temperatureIntegralBefore =
        integral(*state.temperature, *state.mask, h);
    result.maxWorldSpeedBefore = maximumWorld(state);
    DiagnosticSum dissipated, sourceMass, sourceTemperature;
    Temporary temporary(output);
    const double interval = s.timeStep / s.substeps;
    for (uint32_t requested = 0; requested < s.substeps; ++requested) {
      double remaining = interval;
      while (remaining > 0) {
        if (result.actualSubsteps >= s.maxSubsteps)
          throw std::invalid_argument(
              "smoke CFL requires more than maxSubsteps; reduce time/force or "
              "increase the step budget");
        const double speed = maximumLocal(state), force = forceBound(state, s);
        const double cflDt =
            speed == 0 && force == 0
                ? INFINITY
                : 2 * s.maxCfl /
                      (speed + std::hypot(speed, 2 * std::sqrt(force) *
                                                     std::sqrt(s.maxCfl)));
        const double needed = std::max(1.0, std::ceil(remaining / cflDt));
        if (!std::isfinite(needed) ||
            needed > s.maxSubsteps - result.actualSubsteps)
          throw std::invalid_argument(
              "smoke CFL requires more than maxSubsteps; reduce time/force or "
              "increase the step budget");
        // Split the complete remaining interval, avoiding a tiny final sliver
        // when a CFL-limited step is only one ULP shorter than the interval.
        const double dt = remaining / needed;
        if (!std::isfinite(dt) || dt < 1e-6 || remaining - dt == remaining)
          throw std::invalid_argument(
              "smoke CFL requires an unsupported timestep below "
              "numeric/projection precision");
        stateBudget(state, encoded, p);
        transport(state, dt, result.clippedTraceCount);
        forces(state, s, dt);
        if (s.kinematicViscosity > 0)
          viscousProjection(state, temporary, result.actualSubsteps, dt,
                            encoded, s, p, result, dissipated);
        else
          projection(state, temporary, result.actualSubsteps, dt, encoded, s, p,
                     result);
        if (!s.sources.empty())
          scalarSources(state, temporary, result.actualSubsteps, dt, encoded, h,
                        s, p, result, sourceMass, sourceTemperature);
        ++result.actualSubsteps;
        result.simulatedTime += dt;
        if (dt == remaining)
          remaining = 0;
        else
          remaining -= dt;
      }
    }
    result.densityMassAfter = integral(*state.density, *state.mask, h);
    result.temperatureIntegralAfter =
        integral(*state.temperature, *state.mask, h);
    result.maxWorldSpeedAfter = maximumWorld(state);
    result.viscosityEnergyDissipated = dissipated.total();
    result.sourceDensityMassChange = sourceMass.total();
    result.sourceTemperatureIntegralChange = sourceTemperature.total();
    result.converged = true;
    structure(*state.density, s.densityGridName, openvdb::GRID_FOG_VOLUME);
    structure(*state.temperature, s.temperatureGridName, openvdb::GRID_UNKNOWN);
    // The last M04 pressure/velocity retain their documented structural tags.
    state.pressure->setName(s.pressureGridName);
    structure(*state.mask, s.domainGridName, openvdb::GRID_UNKNOWN);
    for(const auto& grid:openvdb::GridPtrVec{state.density,state.temperature,state.velocity,state.pressure,state.mask})
      detail::RemoveBoundaryProjectionMetadata(*grid);
    stateBudget(state, encoded, p, false);
    detail::WriteFileExclusive(output,
                               {state.density, state.temperature,
                                state.velocity, state.pressure, state.mask},
                               p.maxInputBytes);
    return result;
  });
#else
  (void)input;
  (void)output;
  (void)s;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
