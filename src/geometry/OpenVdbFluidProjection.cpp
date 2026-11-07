#include "Container/geometry/OpenVdbFluidProjection.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <memory>
#include <mutex>
#include <openvdb/math/ConjGradient.h>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/util/NullInterrupter.h>
#include <set>
#include <tbb/task_arena.h>
#include <type_traits>
#include <vector>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB fluid projection is unavailable in this build. Configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using MacGrid = openvdb::Vec3DGrid;
using MaskGrid = openvdb::BoolGrid;
using IndexGrid = openvdb::Int32Grid;
using Matrix = openvdb::math::pcg::SparseStencilMatrix<double, 7>;
using Vector = openvdb::math::pcg::Vector<double>;

long double metadataBytes(const openvdb::GridBase &grid) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto field = grid.beginMeta(); field != grid.endMeta(); ++field)
    bytes += 2.0L * (field->first.size() + field->second->typeName().size() +
                     static_cast<long double>(field->second->size())) +
             256;
  return bytes;
}

void validateOptions(const FluidProjectionOptions &p, const ProcessingOptions &options) {
  if (!std::isfinite(p.timeStep) || p.timeStep < 1e-6 || p.timeStep > 3600 ||
      !std::isfinite(p.tolerance) || p.tolerance < 1e-12 || p.tolerance > 1e-2 ||
      p.maxIterations < 1 || p.maxIterations > 10000 || p.maxFluidCells < 1 ||
      p.maxFluidCells > 1'000'000)
    throw std::invalid_argument("projection requires dt in [1e-6,3600], tolerance in [1e-12,1e-2], "
                                "iterations in [1,10000] and cells in [1,1000000]");
  for (double force : p.acceleration)
    if (!std::isfinite(force) || std::abs(force) > 1e12)
      throw std::invalid_argument(
          "projection world acceleration components must be finite with magnitude at most 1e12");
  const std::array<std::string, 3> names{p.velocityGridName, p.pressureGridName, p.domainGridName};
  for (const auto &name : names)
    if (name.empty() || name.size() > 256)
      throw std::invalid_argument("projection output names must contain 1..256 bytes");
  if (names[0] == names[1] || names[0] == names[2] || names[1] == names[2])
    throw std::invalid_argument("projection output names must be distinct");
  if (!options.maxVoxelCount || !options.maxInputBytes)
    throw std::invalid_argument("projection voxel and input-memory budgets must be positive");
}

void validateOutput(const std::filesystem::path &output) {
  if (output.empty())
    throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("output already exists; choose a new .vdb path");
  if (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}

uintmax_t fileBytes(const std::filesystem::path &input, const ProcessingOptions &options) {
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("projection input is not a regular file");
  const auto bytes = std::filesystem::file_size(input);
  if (bytes > options.maxInputBytes)
    throw std::invalid_argument("projection input exceeds the file-size budget");
  return bytes;
}

openvdb::GridBase::Ptr readSelected(const std::filesystem::path &input,
                                    const std::string &selection, bool velocity, uintmax_t encoded,
                                    long double resident, const FluidProjectionOptions &projection,
                                    const ProcessingOptions &options) {
  detail::RequireSeekableFile(input);
  openvdb::io::File file(input.string());
  file.open(false);
  {
    std::set<std::string> names;
    long double bytes = static_cast<long double>(encoded) + resident;
    for (auto entry = file.beginName(); entry != file.endName(); ++entry) {
      const auto name = entry.gridName();
      bytes += 2.0L * name.size() + 128;
      if (names.size() >= 4096 || bytes > options.maxInputBytes)
        throw std::invalid_argument("projection descriptor catalog exceeds processing budgets");
      if (!names.insert(name).second)
        throw std::invalid_argument(
            "projection grid descriptors collide; rename bracket-suffixed grids");
    }
  }
  std::string selected;
  bool found = false;
  for (auto entry = file.beginName(); entry != file.endName(); ++entry) {
    const auto name = entry.gridName();
    const auto metadata = file.readGridMetadata(name);
    if (!GridSelectionMatches(selection, name, metadata->getName()))
      continue;
    const bool supported =
        velocity
            ? metadata->isType<openvdb::Vec3SGrid>() || metadata->isType<openvdb::Vec3DGrid>()
            : metadata->isType<openvdb::BoolGrid>() || metadata->isType<openvdb::FloatGrid>() ||
                  metadata->isType<openvdb::DoubleGrid>();
    if (!supported) {
      if (!selection.empty())
        throw std::invalid_argument("selected projection grid has an unsupported value type");
      continue;
    }
    if (found)
      throw std::invalid_argument(
          "projection input grid name is ambiguous; use descriptor:<exact descriptor name>");
    const auto count =
        metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    const auto memory =
        metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!count || !memory || count->value() < 0 || memory->value() < 0)
      throw std::invalid_argument(
          "projection requires recorded nonnegative grid count/memory statistics");
    const auto cap = velocity ? options.maxVoxelCount
                              : std::min(options.maxVoxelCount, projection.maxFluidCells);
    if (uint64_t(count->value()) > cap ||
        static_cast<long double>(encoded) + resident + memory->value() + metadataBytes(*metadata) >
            options.maxInputBytes)
      throw std::invalid_argument(
          "projection recorded topology or memory exceeds budgets before loading");
    selected = name;
    found = true;
    if (selection.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument(velocity ? "no matching Vec3S/Vec3D velocity field found"
                                         : "no matching Bool/Float/Double fluid domain found");
  auto result = file.readGrid(selected);
  file.close();
  return result;
}

template <class Value> bool finiteValue(const Value &value, double maximum = 1e12) {
  if constexpr (std::is_arithmetic_v<Value>)
    return std::isfinite(double(value)) && std::abs(double(value)) <= maximum;
  else {
    for (size_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(double(value[axis])) || std::abs(double(value[axis])) > maximum)
        return false;
    return true;
  }
}

template <class Grid>
void validateStored(const Grid &grid, const ProcessingOptions &options, double maximum = 1e12) {
  constexpr uint64_t values = Grid::TreeType::LeafNodeType::NUM_VALUES;
  const auto leaves = options.maxVoxelCount / values + (options.maxVoxelCount % values != 0);
  if (grid.activeVoxelCount() > options.maxVoxelCount || grid.tree().leafCount() > leaves ||
      grid.memUsage() + metadataBytes(grid) > options.maxInputBytes)
    throw std::invalid_argument("projection actual stored topology exceeds processing budgets");
  if (!finiteValue(grid.background(), maximum))
    throw std::invalid_argument(
        "projection stored values/background exceed the supported finite range");
  for (auto value = grid.cbeginValueAll(); value; ++value)
    if (!finiteValue(*value, maximum))
      throw std::invalid_argument(
          "projection stored values/background exceed the supported finite range");
}

double validateTransform(const openvdb::GridBase &grid, bool uniform) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument("projection requires nonsingular affine grid transforms");
  const auto map = grid.transform().baseMap();
  const auto origin = map->applyMap(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d, 3> axes;
  std::array<double, 3> lengths;
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d unit(0);
    unit[axis] = 1;
    axes[axis] = map->applyJacobian(unit);
    lengths[axis] = std::hypot(axes[axis].x(), axes[axis].y(), axes[axis].z());
    if (!std::isfinite(origin[axis]) || !std::isfinite(lengths[axis]) || lengths[axis] < 1e-6 ||
        lengths[axis] > 1e6)
      throw std::invalid_argument("projection voxel axes must span [1e-6,1e6] world units");
    axes[axis] /= lengths[axis];
    const auto inverse = map->applyInverseJacobian(unit);
    for (size_t a = 0; a < 3; ++a)
      if (!std::isfinite(inverse[a]) || std::abs(inverse[a]) > 1e8)
        throw std::invalid_argument("projection transform is singular or ill-conditioned");
  }
  const double determinant = axes[0].dot(axes[1].cross(axes[2]));
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8)
    throw std::invalid_argument("projection transform is singular or ill-conditioned");
  if (uniform &&
      (!grid.hasUniformVoxels() || std::abs(lengths[1] / lengths[0] - 1) > 1e-9 ||
       std::abs(lengths[2] / lengths[0] - 1) > 1e-9 || std::abs(axes[0].dot(axes[1])) > 1e-9 ||
       std::abs(axes[0].dot(axes[2])) > 1e-9 || std::abs(axes[1].dot(axes[2])) > 1e-9))
    throw std::invalid_argument("fluid domains require uniform orthogonal affine voxels");
  return lengths[0];
}

template <class Grid>
void preflight(const Grid &domain, const openvdb::GridBase &velocity, uintmax_t encoded,
               const FluidProjectionOptions &p, const ProcessingOptions &options) {
  if (domain.activeVoxelCount() > std::min(p.maxFluidCells, options.maxVoxelCount))
    throw std::invalid_argument("fluid cell topology exceeds the cell budget");
  using Tree = openvdb::Vec3DTree;
  using Leaf = Tree::LeafNodeType;
  using Upper = Tree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  const auto nodes = domain.tree().nodeCount();
  long double leaves = nodes[0];
  for (auto value = domain.cbeginValueOn(); value; ++value) {
    if (double(*value) < 0)
      throw std::invalid_argument("fluid domain scalar values must be nonnegative");
    if (value.isTileValue())
      leaves += static_cast<long double>(value.getVoxelCount()) / Leaf::NUM_VALUES;
    const auto box = value.getBoundingBox();
    for (size_t axis = 0; axis < 3; ++axis)
      if (int64_t(box.min()[axis]) < int64_t(std::numeric_limits<int32_t>::min()) + 16 ||
          int64_t(box.max()[axis]) > int64_t(std::numeric_limits<int32_t>::max()) - 16)
        throw std::invalid_argument("projection faces/stencils exceed safe OpenVDB coordinates");
  }
  if (domain.background() != 0)
    throw std::invalid_argument("fluid domain background must be zero");
  for (auto value = domain.cbeginValueAll(); value; ++value)
    if (double(*value) < 0)
      throw std::invalid_argument("fluid domain scalar values must be nonnegative");
  const auto leafCap =
      options.maxVoxelCount / Leaf::NUM_VALUES + (options.maxVoxelCount % Leaf::NUM_VALUES != 0);
  if (4 * leaves > leafCap)
    throw std::invalid_argument("projection expanded face-leaf topology exceeds the voxel budget");
  const long double faceTree =
      4 * (leaves * (sizeof(Leaf) + Leaf::NUM_VALUES * sizeof(openvdb::Vec3d) + 128.0L) +
           nodes[1] * (sizeof(Lower) + 128.0L) + nodes[2] * (sizeof(Upper) + 384.0L)) +
      4096;
  const long double bytes =
      static_cast<long double>(encoded) + domain.memUsage() + velocity.memUsage() + 4 * faceTree +
      1536.0L * domain.activeVoxelCount() + 4 * (metadataBytes(domain) + metadataBytes(velocity)) +
      4.0L * 1024 * 1024;
  if (bytes > options.maxInputBytes)
    throw std::invalid_argument(
        "projection face/tree/matrix workspace exceeds the input-memory budget");
}

template <class Grid> MaskGrid::Ptr createMask(const Grid &domain) {
  auto mask = MaskGrid::create(false);
  mask->setTransform(detail::CopyTransformExact(domain.transform()));
  mask->insertMeta(domain);
  for (auto value = domain.cbeginValueOn(); value; ++value)
    if (double(*value) > 0)
      mask->tree().fill(value.getBoundingBox(), true, true);
  mask->tree().voxelizeActiveTiles();
  if (mask->empty())
    throw std::invalid_argument("fluid domain contains no active positive cells");
  return mask;
}

void validateWorldPrecision(const MaskGrid &mask, double voxel) {
  auto box = mask.evalActiveVoxelBoundingBox();
  box.expand(1);
  for (unsigned corner = 0; corner < 8; ++corner) {
    openvdb::Vec3d index;
    for (size_t axis = 0; axis < 3; ++axis)
      index[axis] = (corner & (1u << axis)) ? box.max()[axis] : box.min()[axis];
    const auto world = mask.indexToWorld(index);
    for (size_t axis = 0; axis < 3; ++axis) {
      const double spacing =
          std::max(std::abs(std::nextafter(world[axis], std::numeric_limits<double>::infinity()) -
                            world[axis]),
                   std::abs(world[axis] -
                            std::nextafter(world[axis], -std::numeric_limits<double>::infinity())));
      if (!std::isfinite(world[axis]) || !std::isfinite(spacing) || spacing > voxel * 1e-4)
        throw std::invalid_argument(
            "fluid domain world coordinates have insufficient double precision for its voxel size");
    }
  }
}

template <class Grid>
openvdb::Vec3d sampleWorldVector(const Grid &grid, const openvdb::Vec3d &world,
                                 double domainVoxel) {
  const auto map = grid.transform().baseMap();
  const auto index = map->applyInverseJacobian(world - map->applyMap(openvdb::Vec3d(0)));
  long double worldResolution = 0;
  for (size_t axis = 0; axis < 3; ++axis)
    if (!std::isfinite(index[axis]) ||
        index[axis] < double(std::numeric_limits<int32_t>::min()) + 2 ||
        index[axis] > double(std::numeric_limits<int32_t>::max()) - 2)
      throw std::invalid_argument("projection face sample exceeds source coordinate limits");
  for (size_t axis = 0; axis < 3; ++axis) {
    const double spacing =
        std::max(std::abs(std::nextafter(index[axis], std::numeric_limits<double>::infinity()) -
                          index[axis]),
                 std::abs(index[axis] -
                          std::nextafter(index[axis], -std::numeric_limits<double>::infinity())));
    openvdb::Vec3d unit(0);
    unit[axis] = 1;
    const auto sourceAxis = map->applyJacobian(unit);
    worldResolution += static_cast<long double>(spacing) *
                       std::hypot(sourceAxis.x(), sourceAxis.y(), sourceAxis.z());
  }
  if (!std::isfinite(double(worldResolution)) || worldResolution > domainVoxel * 1e-4)
    throw std::invalid_argument("collocated source index coordinates have insufficient precision "
                                "to resolve fluid-domain faces");
  auto accessor = grid.getConstAccessor();
  openvdb::tools::GridSampler<decltype(accessor), openvdb::tools::BoxSampler> sampler(
      accessor, grid.transform());
  const auto value = sampler.isSample(index);
  return {double(value[0]), double(value[1]), double(value[2])};
}

void setComponent(MacGrid::Accessor &access, const openvdb::Coord &coordinate, size_t axis,
                  double value) {
  auto vector = access.getValue(coordinate);
  vector[axis] = value;
  access.setValue(coordinate, vector);
}

double scaled(double value, std::initializer_list<double> multiply,
              std::initializer_list<double> divide);

openvdb::Vec3d worldToLocalVector(const openvdb::math::MapBase &map, const openvdb::Vec3d &vector,
                                  double factor) {
  std::array<openvdb::Vec3d, 3> inverseColumns;
  for (size_t worldAxis = 0; worldAxis < 3; ++worldAxis) {
    openvdb::Vec3d unit(0);
    unit[worldAxis] = 1;
    inverseColumns[worldAxis] = map.applyInverseJacobian(unit);
  }
  openvdb::Vec3d result(0);
  for (size_t localAxis = 0; localAxis < 3; ++localAxis) {
    // Scale each term independently: a large force on a closed axis must not
    // erase a tiny, representable force on another axis during normalization.
    // Neumaier accumulation also retains a small term across large cancellation.
    double sum = 0, correction = 0;
    for (size_t worldAxis = 0; worldAxis < 3; ++worldAxis) {
      const double coefficient = inverseColumns[worldAxis][localAxis];
      const double term = vector[worldAxis] == 0 || coefficient == 0
                              ? 0
                              : scaled(vector[worldAxis], {coefficient, factor}, {});
      const double next = sum + term;
      correction += std::abs(sum) >= std::abs(term) ? (sum - next) + term : (term - next) + sum;
      sum = next;
    }
    result[localAxis] = sum + correction;
    if (!std::isfinite(result[localAxis]))
      throw std::invalid_argument("world vector cannot be represented during MAC conversion");
  }
  return result;
}

template <class Grid>
MacGrid::Ptr createVelocity(const Grid &source, const MaskGrid &mask, double voxel,
                            const FluidProjectionOptions &p) {
  const bool native = source.getGridClass() == openvdb::GRID_STAGGERED;
  if (native) {
    if (source.isInWorldSpace() || source.getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE ||
        source.transform() != mask.transform())
      throw std::invalid_argument("native MAC input must be index-space contravariant-relative "
                                  "with exactly the fluid-domain transform");
  } else if (!source.isInWorldSpace())
    throw std::invalid_argument(
        "collocated projection input must contain world-space vector components");
  auto result = MacGrid::create(openvdb::Vec3d(0));
  result->setTransform(detail::CopyTransformExact(mask.transform()));
  result->insertMeta(source);
  const auto map = mask.transform().baseMap();
  const auto force = worldToLocalVector(
      *map, {p.acceleration[0], p.acceleration[1], p.acceleration[2]}, p.timeStep);
  auto out = result->getAccessor();
  for (auto cell = mask.cbeginValueOn(); cell; ++cell) {
    const auto coordinate = cell.getCoord();
    for (size_t axis = 0; axis < 3; ++axis) {
      auto previous = coordinate, next = coordinate;
      --previous[axis];
      ++next[axis];
      double value = 0;
      if (mask.tree().isValueOn(previous)) {
        if (native)
          value = double(source.tree().getValue(coordinate)[axis]);
        else {
          auto face = coordinate.asVec3d();
          face[axis] -= .5;
          value = worldToLocalVector(
              *map, sampleWorldVector(source, mask.indexToWorld(face), voxel), 1)[axis];
        }
        value += force[axis];
        if (!std::isfinite(value) || std::abs(value) > 1e24)
          throw std::invalid_argument("forced MAC velocity exceeds the supported finite range");
      }
      setComponent(out, coordinate, axis, value);
      if (!mask.tree().isValueOn(next))
        setComponent(out, next, axis, 0);
    }
  }
  return result;
}

double divergence(const MacGrid &velocity, const openvdb::Coord &cell) {
  // MSVC long double has Double precision. Compensating the six signed face
  // values avoids incompatible roundoff-only RHS terms from a discrete curl.
  double sum = 0, compensation = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    auto next = cell;
    ++next[axis];
    for (const double term : {velocity.tree().getValue(next)[axis],
                              -velocity.tree().getValue(cell)[axis]}) {
      const double value = sum + term;
      compensation += std::abs(sum) >= std::abs(term) ? (sum - value) + term
                                                     : (term - value) + sum;
      sum = value;
    }
  }
  const long double value = static_cast<long double>(sum) + compensation;
  const double result = double(value);
  if (!std::isfinite(result) || (value != 0 && result == 0))
    throw std::invalid_argument("MAC divergence cannot be represented in finite double precision");
  return result;
}

double divergenceRoundoff(const MacGrid &velocity, const openvdb::Coord &cell) {
  double sum = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    auto next = cell;
    ++next[axis];
    for (const double value : {velocity.tree().getValue(cell)[axis],
                               velocity.tree().getValue(next)[axis]})
      sum += std::max(std::nextafter(value, INFINITY) - value,
                      value - std::nextafter(value, -INFINITY));
  }
  return 8 * sum;
}

double scaled(double value, std::initializer_list<double> multiply,
              std::initializer_list<double> divide) {
  if (value == 0)
    return 0;
  int exponent = 0;
  double mantissa = std::frexp(value, &exponent);
  for (double factor : multiply) {
    int e;
    mantissa *= std::frexp(factor, &e);
    exponent += e;
  }
  for (double factor : divide) {
    int e;
    mantissa /= std::frexp(factor, &e);
    exponent -= e;
  }
  const double result = std::ldexp(mantissa, exponent);
  if (!std::isfinite(result) || result == 0)
    throw std::invalid_argument(
        "projection pressure/correction cannot be represented in finite double precision");
  return result;
}

struct Domain {
  std::vector<openvdb::Coord> cells;
  std::vector<std::vector<int32_t>> components;
  IndexGrid::Ptr index = IndexGrid::create(-1);
};

Domain indexDomain(const MaskGrid &mask) {
  Domain domain;
  domain.cells.reserve(static_cast<size_t>(mask.activeVoxelCount()));
  for (auto cell = mask.cbeginValueOn(); cell; ++cell)
    domain.cells.push_back(cell.getCoord());
  std::sort(domain.cells.begin(), domain.cells.end());
  auto index = domain.index->getAccessor();
  for (size_t i = 0; i < domain.cells.size(); ++i)
    index.setValue(domain.cells[i], static_cast<int32_t>(i));
  std::vector<bool> seen(domain.cells.size(), false);
  std::vector<int32_t> queue;
  for (size_t start = 0; start < domain.cells.size(); ++start) {
    if (seen[start])
      continue;
    domain.components.emplace_back();
    queue.push_back(static_cast<int32_t>(start));
    seen[start] = true;
    while (!queue.empty()) {
      const int32_t cell = queue.back();
      queue.pop_back();
      domain.components.back().push_back(cell);
      for (size_t axis = 0; axis < 3; ++axis)
        for (int step : {-1, 1}) {
          auto next = domain.cells[cell];
          next[axis] += step;
          const auto n = index.getValue(next);
          if (n >= 0 && !seen[n]) {
            seen[n] = true;
            queue.push_back(n);
          }
        }
    }
    // Upstream incomplete Cholesky stores a four-entry triangular stencil:
    // diagonal plus at most three lower neighbors. Lexicographic coordinate
    // ordering guarantees that bound; traversal/BFS ordering does not.
    std::sort(domain.components.back().begin(), domain.components.back().end());
  }
  return domain;
}

FluidProjectionResult project(MacGrid &velocity, openvdb::DoubleGrid &pressure,
                              const MaskGrid &mask, double voxel, const FluidProjectionOptions &p) {
  auto domain = indexDomain(mask);
  auto indices = domain.index->getConstAccessor();
  std::vector<int32_t> local(domain.cells.size(), -1);
  std::vector<double> originalDiv(domain.cells.size(), 0);
  auto outPressure = pressure.getAccessor();
  auto outVelocity = velocity.getAccessor();
  FluidProjectionResult result;
  result.fluidCellCount = domain.cells.size();
  result.componentCount = domain.components.size();
  for (size_t i = 0; i < domain.cells.size(); ++i) {
    originalDiv[i] = divergence(velocity, domain.cells[i]);
    result.maxDivergenceBefore = std::max(result.maxDivergenceBefore, std::abs(originalDiv[i]));
    outPressure.setValue(domain.cells[i], 0);
    for (size_t axis = 0; axis < 3; ++axis) {
      auto previous = domain.cells[i];
      --previous[axis];
      if (indices.getValue(previous) >= 0)
        ++result.faceCount;
    }
  }
  for (const auto &component : domain.components) {
    double peak = 0;
    long double fluxSum = 0, fluxMagnitude = 0;
    for (size_t i = 0; i < component.size(); ++i) {
      const auto cell = component[i];
      local[cell] = static_cast<int32_t>(i);
      peak = std::max(peak, std::abs(originalDiv[cell]));
      fluxSum += originalDiv[cell];
      const auto coordinate = domain.cells[cell];
      for (size_t axis = 0; axis < 3; ++axis) {
        auto next = coordinate;
        ++next[axis];
        fluxMagnitude += std::abs(velocity.tree().getValue(coordinate)[axis]) +
                         std::abs(velocity.tree().getValue(next)[axis]);
      }
    }
    if (std::abs(fluxSum) > 256 * std::numeric_limits<double>::epsilon() * fluxMagnitude)
      throw std::runtime_error("closed-domain Neumann flux is incompatible; no output written");
    if (peak == 0)
      continue;
    Matrix matrix(static_cast<openvdb::math::pcg::SizeType>(component.size()));
    Vector rhs(static_cast<openvdb::math::pcg::SizeType>(component.size()), 0),
        solution(rhs.size(), 0);
    for (size_t row = 0; row < component.size(); ++row) {
      const auto cell = component[row];
      auto edit = matrix.getRowEditor(static_cast<openvdb::math::pcg::SizeType>(row));
      if (row == 0) {
        edit.setValue(0, 1);
        continue;
      }
      const double normalized = -originalDiv[cell] / peak;
      if (originalDiv[cell] != 0 && normalized == 0)
        throw std::invalid_argument(
            "projection RHS dynamic range underflows; split the domain or reduce its range");
      rhs[row] = normalized;
      int degree = 0;
      for (size_t axis = 0; axis < 3; ++axis)
        for (int step : {-1, 1}) {
          auto next = domain.cells[cell];
          next[axis] += step;
          const auto neighbor = indices.getValue(next);
          if (neighbor < 0)
            continue;
          ++degree;
          const auto column = local[neighbor];
          if (column != 0)
            edit.setValue(column, -1);
        }
      if (!degree)
        throw std::logic_error("nonzero divergence in an isolated fluid cell");
      edit.setValue(static_cast<openvdb::math::pcg::SizeType>(row), double(degree));
    }
    auto termination = openvdb::math::pcg::terminationDefaults<double>();
    termination.relativeError = p.tolerance * .05 / std::sqrt(double(component.size()));
    termination.absoluteError = 0;
    std::unique_ptr<openvdb::math::pcg::Preconditioner<double>> preconditioner(
        new openvdb::math::pcg::IncompleteCholeskyPreconditioner<Matrix>(matrix));
    if (!preconditioner->isValid())
      preconditioner.reset(new openvdb::math::pcg::JacobiPreconditioner<Matrix>(matrix));
    openvdb::util::NullInterrupter interrupter;
    auto state = termination;
    uint32_t iterations = 0;
    // Upstream PCG aborts if the Euclidean residual exceeds twice ANY previous
    // minimum. SPD conjugate-gradient residuals need not decrease monotonically;
    // an irregular closed domain can hit that heuristic after only five steps.
    // Restart from the current finite iterate, using Jacobi after an early
    // failure, while charging every attempt to the original iteration budget.
    // Upstream convergence and the independent stored-field checks below remain
    // required; a restart never turns an unconverged iterate into success.
    do {
      termination.iterations = static_cast<int>(p.maxIterations - iterations);
      state = openvdb::math::pcg::solve(matrix, rhs, solution, *preconditioner, interrupter,
                                      termination);
      iterations += static_cast<uint32_t>(state.iterations);
      if (state.success || !solution.isFinite() || !std::isfinite(state.relativeError) ||
          state.iterations <= 0 || iterations >= p.maxIterations)
        break;
      preconditioner.reset(new openvdb::math::pcg::JacobiPreconditioner<Matrix>(matrix));
    } while (iterations < p.maxIterations);
    if (!state.success || !solution.isFinite())
      throw std::runtime_error("fluid projection solver did not converge after " +
                               std::to_string(iterations) + " iterations (relative error " +
                               std::to_string(state.relativeError) + "); no output written");
    result.iterations = std::max(result.iterations, iterations);
    // Store physical pressure first, then use its representable values for
    // the independent residual and correction, including the pinned cell.
    for (size_t row = 0; row < component.size(); ++row) {
      const double value = scaled(solution[row], {peak, voxel, voxel}, {p.timeStep});
      outPressure.setValue(domain.cells[component[row]], value);
      solution[row] = scaled(value, {p.timeStep}, {peak, voxel, voxel});
    }
    for (size_t row = 0; row < component.size(); ++row) {
      const auto cell = component[row];
      long double aq = 0;
      for (size_t axis = 0; axis < 3; ++axis)
        for (int step : {-1, 1}) {
          auto next = domain.cells[cell];
          next[axis] += step;
          const auto neighbor = indices.getValue(next);
          if (neighbor >= 0)
            aq += static_cast<long double>(solution[row]) - solution[local[neighbor]];
        }
      const double normalized = -originalDiv[cell] / peak;
      if (originalDiv[cell] != 0 && normalized == 0)
        throw std::invalid_argument("projection RHS dynamic range underflows at its gauge cell");
      const double residual = double(std::abs(aq - normalized));
      result.residual = std::max(result.residual, residual);
      if (!std::isfinite(residual) || residual > p.tolerance * .5)
        throw std::runtime_error(
            "fluid projection full Neumann pressure residual exceeds tolerance; no output written");
    }
    for (size_t row = 0; row < component.size(); ++row) {
      const auto coordinate = domain.cells[component[row]];
      for (size_t axis = 0; axis < 3; ++axis) {
        auto previous = coordinate;
        --previous[axis];
        const auto neighbor = indices.getValue(previous);
        if (neighbor < 0)
          continue;
        const double difference = solution[row] - solution[local[neighbor]];
        const double correction = difference == 0 ? 0 : scaled(difference, {peak}, {});
        const double value =
            double(static_cast<long double>(outVelocity.getValue(coordinate)[axis]) - correction);
        if (!std::isfinite(value) || std::abs(value) > 1e24)
          throw std::invalid_argument("projected MAC velocity exceeds the supported finite range");
        setComponent(outVelocity, coordinate, axis, value);
      }
    }
    for (const auto cell : component) {
      const double after = std::abs(divergence(velocity, domain.cells[cell]));
      const double roundoff = divergenceRoundoff(velocity, domain.cells[cell]);
      result.maxDivergenceAfter = std::max(result.maxDivergenceAfter, after);
      result.divergenceRoundoffBound = std::max(result.divergenceRoundoffBound, roundoff);
      const long double target = static_cast<long double>(p.tolerance) * peak;
      result.roundoffLimited |= after > target;
      if (static_cast<long double>(after) > target + roundoff)
        throw std::runtime_error(
            "stored MAC divergence cannot meet projection tolerance; no output written");
    }
  }
  result.converged = true;
  return result;
}

template <class DomainGrid>
FluidProjectionResult run(const openvdb::GridBase::Ptr &source, const DomainGrid &domain,
                          uintmax_t encoded, const std::filesystem::path &output,
                          const FluidProjectionOptions &p, const ProcessingOptions &options) {
  validateStored(domain, options, std::numeric_limits<double>::max());
  const double voxel = validateTransform(domain, true);
  preflight(domain, *source, encoded, p, options);
  auto mask = createMask(domain);
  validateWorldPrecision(*mask, voxel);
  MacGrid::Ptr velocity;
  if (auto input = openvdb::gridPtrCast<openvdb::Vec3SGrid>(source))
    velocity = createVelocity(*input, *mask, voxel, p);
  else
    velocity = createVelocity(*openvdb::gridPtrCast<openvdb::Vec3DGrid>(source), *mask, voxel, p);
  auto pressure = openvdb::DoubleGrid::create(0);
  pressure->setTransform(detail::CopyTransformExact(mask->transform()));
  pressure->insertMeta(domain);
  auto result = project(*velocity, *pressure, *mask, voxel, p);
  // Source custom metadata is retained, while cached stats and structural
  // tags from a previous representation must not mislabel derived outputs.
  for (const auto &grid : openvdb::GridPtrVec{velocity, pressure, mask}) {
    detail::RemoveBoundaryProjectionMetadata(*grid);
    const auto statistics = grid->getStatsMetadata();
    for (auto field = statistics->beginMeta(); field != statistics->endMeta(); ++field)
      grid->removeMeta(field->first);
    for (const char *field :
         {"container:mac_layout", "container:velocity_units", "container:projection_dt",
          "container:pressure_gauge", "container:pressure_units"})
      grid->removeMeta(field);
  }
  velocity->setName(p.velocityGridName);
  velocity->setGridClass(openvdb::GRID_STAGGERED);
  velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  velocity->setIsInWorldSpace(false);
  velocity->insertMeta("container:mac_layout", openvdb::StringMetadata("negative_faces"));
  velocity->insertMeta("container:velocity_units", openvdb::StringMetadata("voxels_per_second"));
  velocity->insertMeta("container:projection_dt", openvdb::DoubleMetadata(p.timeStep));
  pressure->setName(p.pressureGridName);
  pressure->setGridClass(openvdb::GRID_UNKNOWN);
  pressure->setVectorType(openvdb::VEC_INVARIANT);
  pressure->insertMeta("container:pressure_gauge",
                       openvdb::StringMetadata("one_zero_cell_per_component"));
  pressure->insertMeta("container:pressure_units",
                       openvdb::StringMetadata("world_distance_squared_per_second_squared"));
  mask->setName(p.domainGridName);
  mask->setGridClass(openvdb::GRID_UNKNOWN);
  mask->setVectorType(openvdb::VEC_INVARIANT);
  openvdb::GridPtrVec outputs{velocity, pressure};
  if (p.writeDomain)
    outputs.push_back(mask);
  uint64_t leaves = 0, active = 0;
  long double bytes = encoded + source->memUsage() + domain.memUsage() + metadataBytes(*source) +
                      metadataBytes(domain);
  for (const auto &grid : outputs) {
    leaves += grid->baseTree().leafCount();
    active += grid->activeVoxelCount();
    bytes += grid->memUsage() + 2 * metadataBytes(*grid);
  }
  constexpr uint64_t leafValues = MacGrid::TreeType::LeafNodeType::NUM_VALUES;
  const auto leafCap =
      options.maxVoxelCount / leafValues + (options.maxVoxelCount % leafValues != 0);
  if (leaves > leafCap || active > options.maxVoxelCount || bytes > options.maxInputBytes)
    throw std::invalid_argument(
        "summed projection output topology or memory exceeds processing budgets");
  detail::WriteFileExclusive(output, outputs, options.maxInputBytes);
  result.velocityGridName = p.velocityGridName;
  result.pressureGridName = p.pressureGridName;
  if (p.writeDomain)
    result.domainGridName = p.domainGridName;
  return result;
}
#endif
} // namespace

FluidProjectionResult
WriteProjectedVelocity(const std::filesystem::path &velocity, const std::filesystem::path &domain,
                       const std::filesystem::path &output,
                       const FluidProjectionOptions &projection, const std::string &velocityGrid,
                       const std::string &domainGrid, const ProcessingOptions &processing) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(projection, processing);
  validateOutput(output);
  const auto velocityBytes = fileBytes(velocity, processing),
             domainBytes = fileBytes(domain, processing);
  if (static_cast<long double>(velocityBytes) + domainBytes > processing.maxInputBytes)
    throw std::invalid_argument("summed projection encoded inputs exceed the input-memory budget");
  const auto encoded = velocityBytes + domainBytes;
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    auto source = readSelected(velocity, velocityGrid, true, encoded, 0, projection, processing);
    const double velocityCap = source->getGridClass() == openvdb::GRID_STAGGERED ? 1e24 : 1e12;
    if (auto grid = openvdb::gridPtrCast<openvdb::Vec3SGrid>(source))
      validateStored(*grid, processing, velocityCap);
    else
      validateStored(*openvdb::gridPtrCast<openvdb::Vec3DGrid>(source), processing, velocityCap);
    (void)validateTransform(*source, false);
    auto mask = readSelected(domain, domainGrid, false, encoded,
                             source->memUsage() + metadataBytes(*source), projection, processing);
    if (auto grid = openvdb::gridPtrCast<openvdb::BoolGrid>(mask))
      return run(source, *grid, encoded, output, projection, processing);
    if (auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(mask))
      return run(source, *grid, encoded, output, projection, processing);
    return run(source, *openvdb::gridPtrCast<openvdb::DoubleGrid>(mask), encoded, output,
               projection, processing);
  });
#else
  (void)velocity;
  (void)domain;
  (void)output;
  (void)projection;
  (void)velocityGrid;
  (void)domainGrid;
  (void)processing;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
