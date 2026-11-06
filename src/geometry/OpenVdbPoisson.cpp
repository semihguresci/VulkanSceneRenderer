#include "Container/geometry/OpenVdbPoisson.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/tools/PoissonSolver.h>
#include <tbb/task_arena.h>
#include <mutex>
#include <set>
#endif

namespace container::geometry::vdb {
namespace {

[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB Poisson solving is unavailable in this build. Configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
void validateOptions(const PoissonOptions& poisson, const ProcessingOptions& processing) {
  if (poisson.maxIterations < 1 || poisson.maxIterations > 10000 ||
      !std::isfinite(poisson.tolerance) || poisson.tolerance < 1e-12 || poisson.tolerance > 1e-2 ||
      poisson.maxSolverVoxels < 1 || poisson.maxSolverVoxels > 1'000'000)
    throw std::invalid_argument("Poisson requires iterations in [1,10000], relative tolerance in [1e-12,1e-2] and solver voxels in [1,1000000]");
  if (!processing.maxVoxelCount || !processing.maxInputBytes || processing.gridName.empty() ||
      processing.gridName.size() > 256)
    throw std::invalid_argument("Poisson requires positive processing budgets and an output grid name of 1..256 bytes");
}

void validateOutput(const std::filesystem::path& output) {
  if (output.empty()) throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output)) throw std::invalid_argument("output already exists; choose a new .vdb path");
  if (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}

long double metadataBytes(const openvdb::GridBase& grid) {
  long double bytes = 0;
  for (auto field = grid.beginMeta(); field != grid.endMeta(); ++field)
    if (field->second) bytes += field->first.size() + field->second->typeName().size() +
        static_cast<long double>(field->second->size()) + 128.0L;
  return bytes;
}

openvdb::GridBase::Ptr readRhs(const std::filesystem::path& input, const std::string& selection,
                             const PoissonOptions& poisson, const ProcessingOptions& processing,
                             uintmax_t encoded) {
  detail::RequireSeekableFile(input);
  openvdb::io::File file(input.string());
  file.open(false);
  std::set<std::string> descriptors;
  long double namesBytes = encoded;
  for (auto name = file.beginName(); name != file.endName(); ++name) {
    const auto descriptor = name.gridName();
    namesBytes += 2.0L * descriptor.size() + 128.0L;
    if (descriptors.size() >= 4096 || namesBytes > processing.maxInputBytes)
      throw std::invalid_argument("Poisson descriptor count or names exceed the input-memory budget");
    if (!descriptors.insert(descriptor).second)
      throw std::invalid_argument("Poisson descriptor names collide; rename bracket-suffixed grids");
  }
  std::string selected;
  bool found = false;
  const uint64_t countBudget = std::min(poisson.maxSolverVoxels, processing.maxVoxelCount);
  for (auto name = file.beginName(); name != file.endName(); ++name) {
    const auto descriptor = name.gridName();
    const auto metadata = file.readGridMetadata(descriptor);
    if (!GridSelectionMatches(selection, descriptor, metadata->getName()) ||
        (!metadata->isType<openvdb::FloatGrid>() && !metadata->isType<openvdb::DoubleGrid>())) continue;
    if (found) throw std::invalid_argument("Poisson grid name is ambiguous; use descriptor:<unique descriptor name>");
    const auto count = metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    const auto memory = metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!count || !memory || count->value() < 0 || memory->value() < 0)
      throw std::invalid_argument("Poisson requires valid recorded count/memory metadata; rewrite with OpenVDB grid statistics enabled");
    if (uint64_t(count->value()) > countBudget ||
        namesBytes + metadataBytes(*metadata) + memory->value() > processing.maxInputBytes)
      throw std::invalid_argument("Poisson recorded topology or memory exceeds budgets before loading");
    selected = descriptor;
    found = true;
    if (selection.empty()) break;
  }
  if (!found) throw std::invalid_argument("Poisson requires a matching FloatGrid or DoubleGrid scalar RHS");
  auto grid = file.readGrid(selected);
  file.close();
  return grid;
}

double uniformVoxelSize(const openvdb::GridBase& grid) {
  if (!grid.transform().isLinear() || !grid.hasUniformVoxels())
    throw std::invalid_argument("Poisson requires uniform orthogonal affine voxels");
  const auto map = grid.transform().baseMap();
  const auto origin = map->applyMap(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d, 3> axes{
      map->applyJacobian(openvdb::Vec3d(1,0,0)),
      map->applyJacobian(openvdb::Vec3d(0,1,0)),
      map->applyJacobian(openvdb::Vec3d(0,0,1))};
  std::array<double, 3> lengths;
  for (size_t axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(origin[axis])) throw std::invalid_argument("Poisson transform has a non-finite origin");
    lengths[axis] = std::hypot(axes[axis].x(), axes[axis].y(), axes[axis].z());
    if (!std::isfinite(lengths[axis]) || lengths[axis] < 1e-6 || lengths[axis] > 1e6)
      throw std::invalid_argument("Poisson voxel axes must span [1e-6,1e6] world units");
    axes[axis] /= lengths[axis];
  }
  if (std::abs(lengths[1] / lengths[0] - 1.0) > 1e-9 ||
      std::abs(lengths[2] / lengths[0] - 1.0) > 1e-9 ||
      std::abs(axes[0].dot(axes[1])) > 1e-9 || std::abs(axes[0].dot(axes[2])) > 1e-9 ||
      std::abs(axes[1].dot(axes[2])) > 1e-9)
    throw std::invalid_argument("Poisson requires uniform orthogonal affine voxels");
  return lengths[0];
}

template<class Grid>
double validateSource(const Grid& grid, const PoissonOptions& poisson,
                      const ProcessingOptions& processing) {
  const uint64_t countBudget = std::min(poisson.maxSolverVoxels, processing.maxVoxelCount);
  constexpr uint64_t leafValues = Grid::TreeType::LeafNodeType::NUM_VALUES;
  const uint64_t leafBudget = processing.maxVoxelCount / leafValues +
      (processing.maxVoxelCount % leafValues != 0);
  if (grid.activeVoxelCount() > countBudget || uint64_t(grid.tree().leafCount()) > leafBudget ||
      grid.memUsage() > processing.maxInputBytes)
    throw std::invalid_argument("Poisson actual active/stored topology or memory exceeds budgets");
  if (!std::isfinite(double(grid.background())))
    throw std::invalid_argument("Poisson RHS background must be finite");
  double peak = 0.0;
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    const double scalar = static_cast<double>(*value);
    if (!std::isfinite(scalar)) throw std::invalid_argument("Poisson stored RHS values must be finite");
    if (value.isValueOn()) peak = std::max(peak, std::abs(scalar));
  }
  const auto bounds = grid.evalActiveVoxelBoundingBox();
  if (!bounds.empty())
    for (size_t axis = 0; axis < 3; ++axis) {
      // Upstream interior-mask erosion probes neighboring leaf origins, not
      // only the +/-1 pressure stencil. Leave one full leaf on both sides.
      const auto origin = [](int64_t coordinate) {
        constexpr int64_t dimension = Grid::TreeType::LeafNodeType::DIM;
        return (coordinate >= 0 ? coordinate / dimension : (coordinate + 1) / dimension - 1) * dimension;
      };
      if (origin(bounds.min()[axis]) - Grid::TreeType::LeafNodeType::DIM < std::numeric_limits<int32_t>::min() ||
          origin(bounds.max()[axis]) + Grid::TreeType::LeafNodeType::DIM > std::numeric_limits<int32_t>::max())
        throw std::invalid_argument("Poisson stencil exceeds OpenVDB coordinate limits");
    }
  return peak;
}

template<class Grid>
void preflight(const Grid& grid, double peak, uintmax_t encoded, const ProcessingOptions& processing) {
  using Leaf = openvdb::DoubleTree::LeafNodeType;
  using Upper = openvdb::DoubleTree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  long double leaves = grid.tree().leafCount();
  if (peak != 0.0)
    for (auto value = grid.cbeginValueOn(); value; ++value)
      if (value.isTileValue()) leaves += value.getVoxelCount() / Leaf::NUM_VALUES;
  const uint64_t leafBudget = processing.maxVoxelCount / Leaf::NUM_VALUES +
      (processing.maxVoxelCount % Leaf::NUM_VALUES != 0);
  if (leaves > leafBudget)
    throw std::invalid_argument("Poisson expanded stored-leaf topology exceeds the voxel budget");
  const long double doubleTree = leaves * (sizeof(Leaf) + Leaf::NUM_VALUES * sizeof(double) + 128.0L) +
      grid.tree().nonLeafCount() * (sizeof(Upper) + sizeof(Lower) + 512.0L) + 4096.0L;
  // Source + normalized RHS + index tree + mask + solution + output. The 1 KiB
  // per DoF reserve exceeds both 7-entry matrices/preconditioner, all PCG vectors,
  // matrix/transposition bookkeeping and reductions. Serial upstream execution
  // bounds morphology and per-thread caches. File writing is in the same arena.
  const long double workspace = encoded + grid.memUsage() +
      (peak == 0.0 ? 2.0L : 6.0L) * doubleTree +
      (peak == 0.0 ? 0.0L : 1024.0L * grid.activeVoxelCount()) +
      4.0L * metadataBytes(grid) + 4.0L * 1024 * 1024 + 8.0L * (processing.gridName.size() + 16);
  if (workspace > processing.maxInputBytes)
    throw std::invalid_argument("Poisson matrix/tree/preconditioner workspace exceeds the input-memory budget");
}

double physicalValue(double value, double peak, double voxel) {
  if (value == 0.0 || peak == 0.0) return 0.0;
  int valueExponent, peakExponent, voxelExponent;
  const double mantissa = std::frexp(value, &valueExponent) * std::frexp(peak, &peakExponent) *
      std::pow(std::frexp(voxel, &voxelExponent), 2);
  const double result = std::ldexp(mantissa, valueExponent + peakExponent + 2 * voxelExponent);
  if (!std::isfinite(result) || result == 0.0)
    throw std::invalid_argument("Poisson solution exceeds the finite representable scalar range");
  return result;
}

double normalizedValue(double value, double peak, double voxel) {
  if (value == 0.0) return 0.0;
  int valueExponent, peakExponent, voxelExponent;
  const double voxelMantissa = std::frexp(voxel, &voxelExponent);
  const double mantissa = std::frexp(value, &valueExponent) /
      (std::frexp(peak, &peakExponent) * voxelMantissa * voxelMantissa);
  const double result = std::ldexp(mantissa, valueExponent - peakExponent - 2 * voxelExponent);
  if (!std::isfinite(result))
    throw std::invalid_argument("Poisson stored solution cannot be checked in finite normalized units");
  return result;
}

template<class Grid>
PoissonResult solve(const Grid& source, const std::filesystem::path& output,
                    const PoissonOptions& poisson, const ProcessingOptions& processing,
                    uintmax_t encoded) {
  const double voxel = uniformVoxelSize(source);
  const double peak = validateSource(source, poisson, processing);
  preflight(source, peak, encoded, processing);
  tbb::task_arena arena(1);
  return arena.execute([&]() -> PoissonResult {
    openvdb::DoubleTree::Ptr normalized(new openvdb::DoubleTree(source.tree(), 0.0, openvdb::TopologyCopy()));
    auto target = normalized->beginValueAll();
    for (auto value = source.cbeginValueAll(); value; ++value, ++target) {
      if (!target || value.getLevel() != target.getLevel() || value.getCoord() != target.getCoord())
        throw std::logic_error("Poisson RHS topology changed during normalization");
      if (value.isValueOn()) {
        const double scalar = double(*value);
        const double unit = peak == 0.0 ? 0.0 : scalar / peak;
        if (scalar != 0.0 && unit == 0.0)
          throw std::invalid_argument("Poisson RHS dynamic range cannot be represented during normalization; reduce its range or solve disconnected regions separately");
        target.setValue(unit);
      }
    }
    openvdb::DoubleTree::Ptr solution;
    auto state = openvdb::math::pcg::terminationDefaults<double>();
    state.iterations = static_cast<int>(poisson.maxIterations);
    // Leave room for the independently evaluated residual after output rounding.
    state.relativeError = poisson.tolerance * .25;
    state.absoluteError = 0.0;
    if (peak == 0.0) {
      solution = normalized;
      state.success = true;
      state.iterations = 0;
      state.relativeError = 0.0;
    } else {
      solution = openvdb::tools::poisson::solve(*normalized, state, true);
      if (!state.success || !std::isfinite(state.relativeError) || state.relativeError > poisson.tolerance)
        throw std::runtime_error("Poisson solver did not converge within the requested iterations/tolerance; no output written");
    }
    auto result = Grid::create(); result->insertMeta(source);
    detail::RemoveBoundaryProjectionMetadata(*result);
    result->setTransform(detail::CopyTransformExact(source.transform()));
    using Tree = typename Grid::TreeType;
    result->setTree(typename Tree::Ptr(new Tree(*solution, typename Grid::ValueType(0), openvdb::TopologyCopy())));
    auto destination = result->beginValueAll();
    for (auto value = solution->cbeginValueAll(); value; ++value, ++destination) {
      if (!destination || value.getLevel() != destination.getLevel() || value.getCoord() != destination.getCoord())
        throw std::logic_error("Poisson solution topology changed during scalar conversion");
      if (!value.isValueOn()) continue;
      const double physical = physicalValue(*value, peak, voxel);
      const auto stored = static_cast<typename Grid::ValueType>(physical);
      if (!std::isfinite(double(stored)) || (physical != 0.0 && stored == 0))
        throw std::invalid_argument("Poisson solution cannot be represented in the source scalar precision");
      destination.setValue(stored);
    }
    result->setName(processing.gridName);
    result->setGridClass(openvdb::GRID_UNKNOWN);
    result->setVectorType(openvdb::VEC_INVARIANT);
    constexpr uint64_t leafValues = Grid::TreeType::LeafNodeType::NUM_VALUES;
    const uint64_t leafBudget = processing.maxVoxelCount / leafValues +
        (processing.maxVoxelCount % leafValues != 0);
    if (result->memUsage() > processing.maxInputBytes || result->activeVoxelCount() > processing.maxVoxelCount ||
        uint64_t(result->tree().leafCount()) > leafBudget)
      throw std::invalid_argument("Poisson retained output exceeds processing budgets");
    double residual = 0.0;
    if (peak != 0.0) {
      const auto stored = result->getConstAccessor();
      for (auto value = solution->cbeginValueOn(); value; ++value) {
        const auto coordinate = value.getCoord();
        double laplacian = -6.0 * normalizedValue(double(stored.getValue(coordinate)), peak, voxel);
        for (size_t axis = 0; axis < 3; ++axis) {
          auto previous = coordinate, next = coordinate;
          --previous[axis]; ++next[axis];
          laplacian += normalizedValue(double(stored.getValue(previous)), peak, voxel) +
              normalizedValue(double(stored.getValue(next)), peak, voxel);
        }
        residual = std::max(residual, std::abs(laplacian - normalized->getValue(coordinate)));
      }
      if (!std::isfinite(residual) || residual > poisson.tolerance)
        throw std::runtime_error("Poisson stored scalar precision or independently measured residual cannot meet the requested tolerance; no output written");
    }
    const ProcessingResult information{result->getName(), "scalar field", result->activeVoxelCount(), voxel};
    detail::WriteFileExclusive(output, openvdb::GridPtrVec{result}, processing.maxInputBytes);
    return {information, static_cast<uint32_t>(state.iterations), residual, true};
  });
}
#endif
} // namespace

PoissonResult WritePoissonSolution(const std::filesystem::path& inputRhs,
                                  const std::filesystem::path& output,
                                  const std::string& inputGrid, const PoissonOptions& poisson,
                                  const ProcessingOptions& processing) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(poisson, processing);
  validateOutput(output);
  if (!std::filesystem::is_regular_file(inputRhs)) throw std::invalid_argument("Poisson input is not a regular file");
  const auto encoded = std::filesystem::file_size(inputRhs);
  if (encoded > processing.maxInputBytes) throw std::invalid_argument("Poisson input exceeds the file-size budget");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  const auto grid = readRhs(inputRhs, inputGrid, poisson, processing, encoded);
  if (grid->isType<openvdb::FloatGrid>())
    return solve(*openvdb::gridPtrCast<openvdb::FloatGrid>(grid), output, poisson, processing, encoded);
  if (grid->isType<openvdb::DoubleGrid>())
    return solve(*openvdb::gridPtrCast<openvdb::DoubleGrid>(grid), output, poisson, processing, encoded);
  throw std::invalid_argument("Poisson input is not a supported scalar grid");
#else
  (void)inputRhs; (void)output; (void)inputGrid; (void)poisson; (void)processing;
  unavailable();
#endif
}

} // namespace container::geometry::vdb
