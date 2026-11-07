#include "Container/geometry/OpenVdbFluidViscosity.h"
#include "OpenVdbProvenance.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <openvdb/math/ConjGradient.h>
#include <openvdb/openvdb.h>
#include <openvdb/util/NullInterrupter.h>
#include <set>
#include <tbb/task_arena.h>
#include <vector>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB fluid viscosity is unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using Scalar = openvdb::DoubleGrid;
using Mac = openvdb::Vec3DGrid;
using Mask = openvdb::BoolGrid;
using Matrix = openvdb::math::pcg::SparseStencilMatrix<double, 7>;
using Vector = openvdb::math::pcg::Vector<double>;
constexpr long double Fixed = 8.0L * 1024 * 1024;
openvdb::Coord offset(size_t axis, int amount = 1) {
  openvdb::Coord result(0);
  result[axis] = amount;
  return result;
}
long double metadataBytes(const openvdb::MetaMap &map) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto it = map.beginMeta(); it != map.endMeta(); ++it)
    bytes += 4.0L * (it->first.size() + it->second->typeName().size() +
                     it->second->size()) +
             256;
  return bytes;
}
void budget(long double bytes, const ProcessingOptions &p, const char *reason) {
  if (!std::isfinite(bytes) || bytes > p.maxInputBytes)
    throw std::invalid_argument(reason);
}
std::array<std::string, 5> names(const FluidViscosityOptions &s) {
  return {s.densityGridName, s.temperatureGridName, s.velocityGridName,
          s.pressureGridName, s.domainGridName};
}
void validateOptions(const FluidViscosityOptions &s, const ProcessingOptions &p,
                     const std::filesystem::path &output) {
  if (!std::isfinite(s.timeStep) || s.timeStep < 1e-6 || s.timeStep > 3600 ||
      !std::isfinite(s.kinematicViscosity) || s.kinematicViscosity < 0 ||
      s.kinematicViscosity > 1e6 || !std::isfinite(s.tolerance) ||
      s.tolerance < 1e-12 || s.tolerance > 1e-2 || s.maxIterations < 1 ||
      s.maxIterations > 10000 || s.maxFluidCells < 1 ||
      s.maxFluidCells > 1'000'000)
    throw std::invalid_argument(
        "viscosity requires dt [1e-6,3600], nu [0,1e6], tolerance "
        "[1e-12,1e-2], iterations [1,10000] and cells [1,1000000]");
  std::set<std::string> unique;
  for (const auto &name : names(s))
    if (name.empty() || name.size() > 256 || !unique.insert(name).second)
      throw std::invalid_argument("five viscosity state names must be distinct "
                                  "and contain 1..256 bytes");
  if (!p.maxInputBytes || !p.maxVoxelCount)
    throw std::invalid_argument("viscosity budgets must be positive");
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "viscosity output must be a new path in an existing directory");
}
struct State {
  Scalar::Ptr density, temperature, pressure;
  Mac::Ptr velocity;
  Mask::Ptr domain;
  openvdb::GridPtrVec grids() const {
    return {density, temperature, velocity, pressure, domain};
  }
};
long double memory(const State &state) {
  long double bytes = 0;
  for (const auto &grid : state.grids())
    bytes += grid->memUsage() + 4 * metadataBytes(*grid);
  return bytes;
}
State read(const std::filesystem::path &input, const FluidViscosityOptions &s,
           const ProcessingOptions &p, uintmax_t &encoded) {
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("viscosity state is not a regular file");
  encoded = std::filesystem::file_size(input);
  budget(encoded + Fixed, p, "viscosity encoded input exceeds memory budget");
  detail::RequireSeekableFile(input);
  openvdb::io::File file(input.string());
  file.open(false);
  std::set<std::string> catalog;
  long double catalogBytes = encoded + Fixed;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto descriptor = it.gridName();
    catalogBytes += 2.0L * descriptor.size() + 128;
    budget(catalogBytes, p,
           "viscosity descriptor catalog exceeds memory budget");
    if (catalog.size() >= 4096 || !catalog.insert(descriptor).second)
      throw std::invalid_argument(
          "viscosity input has excessive or colliding descriptors");
  }
  const auto required = names(s);
  std::array<std::string, 5> descriptors{};
  std::array<bool, 5> found{};
  long double recorded = encoded + Fixed;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto metadata = file.readGridMetadata(it.gridName());
    for (size_t index = 0; index < required.size(); ++index)
      if (metadata->getName() == required[index]) {
        if (found[index])
          throw std::invalid_argument(
              "viscosity state has ambiguous duplicate display names; rewrite "
              "one canonical named state");
        const bool valid = index == 2   ? metadata->isType<Mac>()
                           : index == 4 ? metadata->isType<Mask>()
                                        : metadata->isType<Scalar>();
        if (!valid)
          throw std::invalid_argument("viscosity needs canonical Double "
                                      "scalar/MAC and Bool domain grids");
        const auto count = metadata->getMetadata<openvdb::Int64Metadata>(
            openvdb::GridBase::META_FILE_VOXEL_COUNT);
        const auto bytes = metadata->getMetadata<openvdb::Int64Metadata>(
            openvdb::GridBase::META_FILE_MEM_BYTES);
        if (!count || !bytes || count->value() < 0 || bytes->value() < 0 ||
            uint64_t(count->value()) > p.maxVoxelCount)
          throw std::invalid_argument(
              "viscosity input needs nonnegative recorded grid statistics "
              "within voxel budget");
        recorded += bytes->value() + 4 * metadataBytes(*metadata);
        budget(recorded, p,
               "combined viscosity grids exceed memory before loading");
        found[index] = true;
        descriptors[index] = it.gridName();
      }
  }
  for (bool exists : found)
    if (!exists)
      throw std::invalid_argument(
          "viscosity input is missing a configured canonical state grid");
  openvdb::GridPtrVec grids;
  long double actual = encoded + Fixed;
  for (const auto &descriptor : descriptors) {
    auto grid = file.readGrid(descriptor);
    actual += grid->memUsage() + 4 * metadataBytes(*grid);
    budget(actual, p, "combined loaded viscosity state exceeds memory budget");
    grids.push_back(std::move(grid));
  }
  file.close();
  return {openvdb::gridPtrCast<Scalar>(grids[0]),
          openvdb::gridPtrCast<Scalar>(grids[1]),
          openvdb::gridPtrCast<Scalar>(grids[3]),
          openvdb::gridPtrCast<Mac>(grids[2]),
          openvdb::gridPtrCast<Mask>(grids[4])};
}
double validateState(const State &state, const FluidViscosityOptions &s,
                     const ProcessingOptions &p,
                     std::vector<openvdb::Coord> &cells) {
  const auto &mask = *state.domain;
  if (mask.background() || !mask.transform().isLinear() ||
      !mask.hasUniformVoxels())
    throw std::invalid_argument(
        "viscosity domain needs zero background and uniform affine voxels");
  const auto map = mask.transform().baseMap();
  std::array<openvdb::Vec3d, 3> axes;
  double h = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d unit(0);
    unit[axis] = 1;
    axes[axis] = map->applyJacobian(unit);
    const double length =
        std::hypot(axes[axis].x(), axes[axis].y(), axes[axis].z());
    if (!std::isfinite(length) || length < 1e-6 || length > 1e6 ||
        (axis && std::abs(length / h - 1) > 1e-9))
      throw std::invalid_argument(
          "viscosity domain voxel sizes must be uniform in [1e-6,1e6]");
    if (!axis)
      h = length;
    axes[axis] /= length;
  }
  for (size_t a = 0; a < 3; ++a)
    for (size_t b = a + 1; b < 3; ++b)
      if (std::abs(axes[a].dot(axes[b])) > 1e-9)
        throw std::invalid_argument("viscosity domain axes must be orthogonal");
  uint64_t active = 0, leaves = 0;
  for (const auto &grid : state.grids()) {
    if (grid->transform() != mask.transform())
      throw std::invalid_argument(
          "all viscosity state transforms must exactly match");
    active += grid->activeVoxelCount();
    leaves += grid->baseTree().leafCount();
  }
  if (active > p.maxVoxelCount ||
      leaves > p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0) ||
      mask.activeVoxelCount() > s.maxFluidCells)
    throw std::invalid_argument(
        "combined viscosity state topology exceeds voxel/cell budgets");
  if (mask.tree().hasActiveTiles())
    throw std::invalid_argument("canonical viscosity domain must contain "
                                "voxelized active cells, not active tiles");
  for (auto value = mask.cbeginValueOn(); value; ++value)
    if (*value) {
      const auto c = value.getCoord();
      for (size_t a = 0; a < 3; ++a)
        if (int64_t(c[a]) < int64_t(INT32_MIN) + 16 ||
            int64_t(c[a]) > int64_t(INT32_MAX) - 16)
          throw std::invalid_argument(
              "viscosity domain exceeds safe stencil coordinates");
      cells.push_back(c);
    }
  if (cells.empty())
    throw std::invalid_argument(
        "viscosity domain contains no active true cells");
  std::sort(cells.begin(), cells.end());
  const auto fluid = [&](const openvdb::Coord &c) {
    return mask.tree().isValueOn(c) && mask.tree().getValue(c);
  };
  const auto scalar = [&](const Scalar &grid, bool pressure) {
    if (grid.background() != 0)
      throw std::invalid_argument("viscosity scalar backgrounds must be zero");
    for (auto value = grid.cbeginValueAll(); value; ++value) {
      const double v = *value;
      if (!std::isfinite(v) || (!pressure && (v < 0 || v > 1e6)) ||
          (!value.isValueOn() && v != 0))
        throw std::invalid_argument(
            "viscosity scalar state has unsupported values/inactive payloads");
      if (v != 0) {
        const auto box = value.getBoundingBox();
        for (auto c = box.begin(); c; ++c)
          if (!fluid(*c))
            throw std::invalid_argument(
                "viscosity scalar is nonzero outside fluid");
      }
    }
  };
  scalar(*state.density, false);
  scalar(*state.temperature, false);
  scalar(*state.pressure, true);
  if (state.density->getGridClass() != openvdb::GRID_FOG_VOLUME ||
      state.temperature->getGridClass() != openvdb::GRID_UNKNOWN ||
      state.pressure->getGridClass() != openvdb::GRID_UNKNOWN ||
      mask.getGridClass() != openvdb::GRID_UNKNOWN)
    throw std::invalid_argument(
        "viscosity scalar/domain classes must match a canonical smoke state");
  const auto &velocity = *state.velocity;
  if (velocity.getGridClass() != openvdb::GRID_STAGGERED ||
      velocity.isInWorldSpace() ||
      velocity.getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE ||
      velocity.background() != openvdb::Vec3d(0))
    throw std::invalid_argument("viscosity velocity must be closed native "
                                "local MAC with zero background");
  for (auto value = velocity.cbeginValueAll(); value; ++value) {
    bool nonzero = false;
    for (size_t a = 0; a < 3; ++a) {
      if (!std::isfinite((*value)[a]) || std::abs((*value)[a]) > 1e24)
        throw std::invalid_argument("viscosity MAC values exceed finite range");
      nonzero |= (*value)[a] != 0;
    }
    if (nonzero) {
      const auto box = value.getBoundingBox();
      for (auto c = box.begin(); c; ++c)
        if (!fluid(*c))
          throw std::invalid_argument(
              "viscosity MAC has nonzero values outside fluid");
    }
  }
  for (const auto &c : cells)
    for (size_t a = 0; a < 3; ++a)
      if ((!fluid(c - offset(a)) && velocity.tree().getValue(c)[a] != 0) ||
          (!fluid(c + offset(a)) &&
           velocity.tree().getValue(c + offset(a))[a] != 0))
        throw std::invalid_argument(
            "viscosity input exterior normal faces must already be zero");
  auto box = mask.evalActiveVoxelBoundingBox();
  box.expand(1);
  for (unsigned corner = 0; corner < 8; ++corner) {
    openvdb::Vec3d index;
    for (size_t a = 0; a < 3; ++a)
      index[a] = corner & (1u << a) ? box.max()[a] : box.min()[a];
    const auto world = mask.indexToWorld(index);
    for (size_t a = 0; a < 3; ++a)
      if (!std::isfinite(world[a]) ||
          std::max(std::abs(std::nextafter(world[a], INFINITY) - world[a]),
                   std::abs(std::nextafter(world[a], -INFINITY) - world[a])) >
              h * 1e-4)
        throw std::invalid_argument(
            "viscosity domain world precision is insufficient");
  }
  return h;
}
double scaled(double value, std::initializer_list<double> multiply,
              std::initializer_list<double> divide = {}) {
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
    throw std::invalid_argument("viscosity coefficient/field cannot be "
                                "represented in finite precision");
  return result;
}
double faceNorm(const Mac &velocity, const Mask &domain,
                const std::vector<openvdb::Coord> &cells) {
  double result = 0;
  for (const auto &c : cells)
    for (size_t a = 0; a < 3; ++a)
      if (domain.tree().isValueOn(c - offset(a)) &&
          domain.tree().getValue(c - offset(a)))
        result = std::hypot(result, velocity.tree().getValue(c)[a]);
  return result;
}
double energy(double norm, double h) {
  if (norm == 0)
    return 0;
  int exponent = 0;
  double mantissa = std::frexp(.5, &exponent);
  for (double factor : {norm, norm, h, h, h, h, h}) {
    int e;
    mantissa *= std::frexp(factor, &e);
    exponent += e;
  }
  const double value = std::ldexp(mantissa, exponent);
  if (!std::isfinite(value))
    throw std::invalid_argument("viscosity kinetic diagnostic is not finite");
  return value;
}
double maxDivergence(const Mac &velocity,
                     const std::vector<openvdb::Coord> &cells) {
  double maximum = 0;
  for (const auto &c : cells) {
    double sum = 0, correction = 0;
    for (size_t a = 0; a < 3; ++a)
      for (double term : {velocity.tree().getValue(c + offset(a))[a],
                          -velocity.tree().getValue(c)[a]}) {
        const double next = sum + term;
        correction += std::abs(sum) >= std::abs(term) ? (sum - next) + term
                                                      : (term - next) + sum;
        sum = next;
      }
    maximum = std::max(maximum, std::abs(sum + correction));
  }
  return maximum;
}
Mac::Ptr diffuse(const State &state, const std::vector<openvdb::Coord> &cells,
                 double coefficient, const FluidViscosityOptions &s,
                 FluidViscosityResult &result) {
  auto output = detail::DeepCopyGridExact(*state.velocity);
  const auto fluid = [&](const auto &c) {
    return state.domain->tree().isValueOn(c) &&
           state.domain->tree().getValue(c);
  };
  for (size_t axis = 0; axis < 3; ++axis) {
    std::vector<openvdb::Coord> faces;
    for (const auto &c : cells)
      if (fluid(c - offset(axis)))
        faces.push_back(c);
    result.faceCount += faces.size();
    if (faces.empty())
      continue;
    auto index = openvdb::Int32Grid::create(-1);
    for (size_t row = 0; row < faces.size(); ++row)
      index->tree().setValue(faces[row], static_cast<int32_t>(row));
    std::vector<bool> seen(faces.size(), false);
    std::vector<int32_t> local(faces.size(), -1), queue, component;
    for (size_t start = 0; start < faces.size(); ++start) {
      if (seen[start])
        continue;
      queue.push_back(static_cast<int32_t>(start));
      seen[start] = true;
      component.clear();
      while (!queue.empty()) {
        const auto row = queue.back();
        queue.pop_back();
        component.push_back(row);
        for (size_t a = 0; a < 3; ++a)
          for (int direction : {-1, 1}) {
            const auto neighbor =
                index->tree().getValue(faces[row] + offset(a, direction));
            if (neighbor >= 0 && !seen[neighbor]) {
              seen[neighbor] = true;
              queue.push_back(neighbor);
            }
          }
      }
      std::sort(component.begin(), component.end());
      double peak = 0;
      for (size_t row = 0; row < component.size(); ++row) {
        local[component[row]] = static_cast<int32_t>(row);
        peak = std::max(peak, std::abs(state.velocity->tree().getValue(
                                  faces[component[row]])[axis]));
      }
      if (peak == 0)
        continue;
      Matrix matrix(
          static_cast<openvdb::math::pcg::SizeType>(component.size()));
      Vector rhs(static_cast<openvdb::math::pcg::SizeType>(component.size()),
                 0),
          solution(rhs.size(), 0);
      const double diagonal = 1 + 6 * coefficient;
      for (size_t row = 0; row < component.size(); ++row) {
        const auto c = faces[component[row]];
        auto edit =
            matrix.getRowEditor(static_cast<openvdb::math::pcg::SizeType>(row));
        edit.setValue(static_cast<openvdb::math::pcg::SizeType>(row), diagonal);
        const double source = state.velocity->tree().getValue(c)[axis];
        rhs[row] = source / peak;
        if (source != 0 && (rhs[row] == 0 || rhs[row] / diagonal == 0))
          throw std::invalid_argument(
              "viscosity face-block dynamic range cannot preserve a nonzero "
              "term; partition its range");
        for (size_t a = 0; a < 3; ++a)
          for (int sign : {-1, 1}) {
            const auto neighbor = index->tree().getValue(c + offset(a, sign));
            if (neighbor >= 0)
              edit.setValue(local[neighbor], -coefficient);
          }
      }
      // Lexical face order gives at most three lower neighbors, as required by
      // upstream IC's four-slot triangular storage. BFS order is not
      // sufficient.
      std::unique_ptr<openvdb::math::pcg::Preconditioner<double>>
          preconditioner(
              new openvdb::math::pcg::IncompleteCholeskyPreconditioner<Matrix>(
                  matrix));
      if (!preconditioner->isValid())
        preconditioner.reset(
            new openvdb::math::pcg::JacobiPreconditioner<Matrix>(matrix));
      auto termination = openvdb::math::pcg::terminationDefaults<double>();
      termination.relativeError =
          s.tolerance * .025 / std::sqrt(double(component.size()));
      termination.absoluteError = 0;
      auto state = termination;
      uint32_t iterations = 0;
      openvdb::util::NullInterrupter interrupter;
      do {
        termination.iterations = static_cast<int>(s.maxIterations - iterations);
        state = openvdb::math::pcg::solve(
            matrix, rhs, solution, *preconditioner, interrupter, termination);
        if (state.iterations < 0)
          throw std::runtime_error(
              "viscosity solver returned invalid iteration count");
        iterations += static_cast<uint32_t>(state.iterations);
        if (state.success || !solution.isFinite() ||
            !std::isfinite(state.relativeError) || state.iterations == 0 ||
            iterations >= s.maxIterations)
          break;
        preconditioner.reset(
            new openvdb::math::pcg::JacobiPreconditioner<Matrix>(matrix));
      } while (iterations < s.maxIterations);
      if (!state.success || !solution.isFinite())
        throw std::runtime_error(
            "viscosity solver did not converge; no output written");
      result.diffusionIterations =
          std::max(result.diffusionIterations, iterations);
      for (size_t row = 0; row < component.size(); ++row) {
        auto value = output->tree().getValue(faces[component[row]]);
        value[axis] = scaled(solution[row], {peak});
        if (std::abs(value[axis]) > 1e24)
          throw std::invalid_argument(
              "viscous MAC output exceeds supported range");
        output->tree().setValue(faces[component[row]], value);
        solution[row] = value[axis] / peak;
        if (value[axis] != 0 && solution[row] == 0)
          throw std::invalid_argument(
              "stored viscosity normalization loses a nonzero field value");
      }
      for (size_t row = 0; row < component.size(); ++row) {
        const auto c = faces[component[row]];
        double laplacian = 6 * solution[row];
        for (size_t a = 0; a < 3; ++a)
          for (int sign : {-1, 1}) {
            const auto neighbor = index->tree().getValue(c + offset(a, sign));
            if (neighbor >= 0)
              laplacian -= solution[local[neighbor]];
          }
        const double residual =
            std::abs(solution[row] + coefficient * laplacian - rhs[row]);
        if (!std::isfinite(residual) || residual > s.tolerance)
          throw std::runtime_error("stored viscosity infinity residual exceeds "
                                   "tolerance; no output written");
        result.diffusionResidual = std::max(result.diffusionResidual, residual);
      }
    }
  }
  return output;
}
struct Temporary {
  std::filesystem::path directory, predictor, projected;
  explicit Temporary(const std::filesystem::path &output) {
    static std::atomic_uint64_t counter{0};
    const auto parent = output.has_parent_path() ? output.parent_path()
                                                 : std::filesystem::path(".");
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      directory =
          parent /
          (".container-viscosity-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++counter));
      if (std::filesystem::create_directory(directory)) {
        predictor = directory / "predictor.vdb";
        projected = directory / "projected.vdb";
        return;
      }
    }
    throw std::runtime_error(
        "cannot create owned viscosity temporary directory");
  }
  ~Temporary() {
    std::error_code ignored;
    std::filesystem::remove(predictor, ignored);
    std::filesystem::remove(projected, ignored);
    std::filesystem::remove(directory, ignored);
  }
};
void pressureMetadata(Scalar &pressure, const Scalar &original,
                      const FluidViscosityOptions &s) {
  for (auto it = original.beginMeta(); it != original.endMeta(); ++it) {
    pressure.removeMeta(it->first);
    pressure.insertMeta(it->first, *it->second);
  }
  const auto stats = pressure.getStatsMetadata();
  for (auto it = stats->beginMeta(); it != stats->endMeta(); ++it)
    pressure.removeMeta(it->first);
  for (const auto *key : {"container:mac_layout", "container:velocity_units",
                          "container:projection_dt", "container:pressure_gauge",
                          "container:pressure_units"})
    pressure.removeMeta(key);
  pressure.setName(s.pressureGridName);
  pressure.setGridClass(openvdb::GRID_UNKNOWN);
  pressure.setVectorType(openvdb::VEC_INVARIANT);
  pressure.setIsInWorldSpace(true);
  pressure.insertMeta("container:pressure_gauge",
                      openvdb::StringMetadata("one_zero_cell_per_component"));
  pressure.insertMeta(
      "container:pressure_units",
      openvdb::StringMetadata("world_distance_squared_per_second_squared"));
}
#endif
} // namespace

FluidViscosityResult WriteViscousFluidState(const std::filesystem::path &input,
                                            const std::filesystem::path &output,
                                            const FluidViscosityOptions &s,
                                            const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(s, p, output);
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    uintmax_t encoded = 0;
    auto state = read(input, s, p, encoded);
    budget(encoded + memory(state) + 64.0L * state.domain->activeVoxelCount() +
               Fixed,
           p, "viscosity cell-index preflight exceeds memory budget");
    std::vector<openvdb::Coord> cells;
    const double h = validateState(state, s, p, cells);
    FluidViscosityResult result;
    result.fluidCellCount = cells.size();
    const double initialNorm = faceNorm(*state.velocity, *state.domain, cells);
    result.kineticEnergyBefore = energy(initialNorm, h);
    result.maxDivergenceBefore = maxDivergence(*state.velocity, cells);
    if (s.kinematicViscosity == 0) {
      for (const auto &c : cells)
        for (size_t a = 0; a < 3; ++a)
          if (state.domain->tree().isValueOn(c - offset(a)) &&
              state.domain->tree().getValue(c - offset(a)))
            ++result.faceCount;
      result.skipped = true;
      result.converged = true;
      result.maxDivergenceAfter = result.maxDivergenceBefore;
      result.kineticEnergyAfterDiffusion = result.kineticEnergyAfterProjection =
          result.kineticEnergyBefore;
      for(const auto& grid:state.grids())detail::RemoveBoundaryProjectionMetadata(*grid);
      budget(encoded + 3 * memory(state) + Fixed, p,
             "viscosity copy/writer workspace exceeds budget");
      detail::WriteFileExclusive(output, state.grids(), p.maxInputBytes,
                                 openvdb::io::COMPRESS_ZIP);
      return result;
    }
    const double coefficient =
        scaled(s.kinematicViscosity, {s.timeStep}, {h, h});
    const long double solver =
        encoded + 3 * memory(state) + cells.size() * 3.0L * 2048 + Fixed;
    budget(solver, p,
           "viscosity face/matrix/preconditioner/workspace exceeds budget "
           "before allocation");
    auto velocity = diffuse(state, cells, coefficient, s, result);
    const double diffusedNorm = faceNorm(*velocity, *state.domain, cells);
    const double normAllowance = std::max(
        1e-12, 128 * std::numeric_limits<double>::epsilon() * result.faceCount);
    if (diffusedNorm > initialNorm * (1 + normAllowance))
      throw std::runtime_error(
          "stored viscosity violates discrete dissipation; no output written");
    result.kineticEnergyAfterDiffusion = energy(diffusedNorm, h);
    const long double reserve = encoded + memory(state) + velocity->memUsage() +
                                4 * metadataBytes(*velocity) + Fixed;
    budget(reserve + 1, p,
           "viscosity retained state leaves no projection budget");
    auto limits = p;
    limits.maxInputBytes = static_cast<uintmax_t>(
        static_cast<long double>(p.maxInputBytes) - reserve);
    Temporary temporary(output);
    for(const auto& grid:openvdb::GridPtrVec{velocity,state.domain})
      detail::RemoveBoundaryProjectionMetadata(*grid);
    detail::WriteFileExclusive(temporary.predictor, {velocity, state.domain},
                               limits.maxInputBytes, openvdb::io::COMPRESS_ZIP);
    FluidProjectionOptions projection;
    projection.timeStep = s.timeStep;
    projection.tolerance = s.tolerance;
    projection.maxIterations = s.maxIterations;
    projection.maxFluidCells = s.maxFluidCells;
    projection.velocityGridName = s.velocityGridName;
    projection.pressureGridName = s.pressureGridName;
    projection.domainGridName = s.domainGridName;
    const auto projected = WriteProjectedVelocity(
        temporary.predictor, temporary.predictor, temporary.projected,
        projection, "descriptor:" + s.velocityGridName,
        "descriptor:" + s.domainGridName, limits);
    openvdb::io::File file(temporary.projected.string());
    file.open(false);
    const auto bytes = std::filesystem::file_size(temporary.projected);
    long double loading = reserve + bytes;
    for (const auto &name : {s.velocityGridName, s.pressureGridName}) {
      const auto meta = file.readGridMetadata(name);
      const auto mem = meta->getMetadata<openvdb::Int64Metadata>(
          openvdb::GridBase::META_FILE_MEM_BYTES);
      if (!mem || mem->value() < 0)
        throw std::logic_error(
            "viscosity projection output is missing memory statistics");
      loading += mem->value() + 4 * metadataBytes(*meta);
      budget(loading, p,
             "viscosity projected-state loading exceeds memory budget");
    }
    auto projectedVelocity =
        openvdb::gridPtrCast<Mac>(file.readGrid(s.velocityGridName));
    auto pressure =
        openvdb::gridPtrCast<Scalar>(file.readGrid(s.pressureGridName));
    file.close();
    if (!projectedVelocity || !pressure)
      throw std::logic_error("viscosity projection returned an invalid state");
    pressureMetadata(*pressure, *state.pressure, s);
    const double projectedNorm =
        faceNorm(*projectedVelocity, *state.domain, cells);
    if (projectedNorm > diffusedNorm * (1 + normAllowance))
      throw std::runtime_error("viscosity pressure projection violates "
                               "discrete dissipation; no output written");
    result.kineticEnergyAfterProjection = energy(projectedNorm, h);
    result.projectionIterations = projected.iterations;
    result.pressureResidual = projected.residual;
    result.maxDivergenceAfter = projected.maxDivergenceAfter;
    result.divergenceRoundoffBound = projected.divergenceRoundoffBound;
    result.roundoffLimited = projected.roundoffLimited;
    for(const auto& grid:openvdb::GridPtrVec{state.density,state.temperature,projectedVelocity,pressure,state.domain})
      detail::RemoveBoundaryProjectionMetadata(*grid);
    budget(encoded + memory(state) + velocity->memUsage() +
               projectedVelocity->memUsage() + pressure->memUsage() +
               4 * (metadataBytes(*velocity) +
                    metadataBytes(*projectedVelocity) +
                    metadataBytes(*pressure)) +
               Fixed,
           p, "viscosity final state/writer workspace exceeds memory budget");
    detail::WriteFileExclusive(output,
                               {state.density, state.temperature,
                                projectedVelocity, pressure, state.domain},
                               p.maxInputBytes, openvdb::io::COMPRESS_ZIP);
    result.converged = true;
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
