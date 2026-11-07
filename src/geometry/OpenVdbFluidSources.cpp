#include "Container/geometry/OpenVdbFluidSources.h"
#include "OpenVdbProvenance.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <array>
#include <memory>
#include <mutex>
#include <numbers>
#include <openvdb/openvdb.h>
#include <set>
#include <tbb/task_arena.h>
#include <tuple>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB fluid sources are unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using Scalar = openvdb::DoubleGrid;
using Mac = openvdb::Vec3DGrid;
using Mask = openvdb::BoolGrid;
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
std::array<std::string, 5> names(const FluidSourcesOptions &s) {
  return {s.densityGridName, s.temperatureGridName, s.velocityGridName,
          s.pressureGridName, s.domainGridName};
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
std::pair<uint64_t, uint64_t> topology(const State &state,
                                       const ProcessingOptions &p) {
  uint64_t active = 0, leaves = 0;
  const uint64_t leafCap = p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0);
  for (const auto &grid : state.grids()) {
    const uint64_t count = grid->activeVoxelCount(),
                   storedLeaves = grid->baseTree().leafCount();
    if (count > p.maxVoxelCount - active || storedLeaves > leafCap - leaves)
      throw std::invalid_argument(
          "combined fluid source/sink topology exceeds voxel/leaf budgets");
    active += count;
    leaves += storedLeaves;
  }
  return {active, leaves};
}
State read(const std::filesystem::path &input, const FluidSourcesOptions &s,
           const ProcessingOptions &p, uintmax_t &encoded) {
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument(
        "fluid source/sink state is not a regular file");
  encoded = std::filesystem::file_size(input);
  budget(encoded + Fixed, p,
         "fluid source/sink encoded input exceeds memory budget");
  detail::RequireSeekableFile(input);
  openvdb::io::File file(input.string());
  file.open(false);
  std::set<std::string> catalog;
  long double catalogBytes = encoded + Fixed;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto descriptor = it.gridName();
    catalogBytes += 2.0L * descriptor.size() + 128;
    budget(catalogBytes, p,
           "fluid source/sink descriptor catalog exceeds memory budget");
    if (catalog.size() >= 4096 || !catalog.insert(descriptor).second)
      throw std::invalid_argument(
          "fluid source/sink input has excessive or colliding descriptors");
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
          throw std::invalid_argument("fluid source/sink state has ambiguous "
                                      "duplicate display names; rewrite "
                                      "one canonical named state");
        const bool valid = index == 2   ? metadata->isType<Mac>()
                           : index == 4 ? metadata->isType<Mask>()
                                        : metadata->isType<Scalar>();
        if (!valid)
          throw std::invalid_argument(
              "fluid source/sink needs canonical Double "
              "scalar/MAC and Bool domain grids");
        const auto count = metadata->getMetadata<openvdb::Int64Metadata>(
            openvdb::GridBase::META_FILE_VOXEL_COUNT);
        const auto bytes = metadata->getMetadata<openvdb::Int64Metadata>(
            openvdb::GridBase::META_FILE_MEM_BYTES);
        if (!count || !bytes || count->value() < 0 || bytes->value() < 0 ||
            uint64_t(count->value()) > p.maxVoxelCount)
          throw std::invalid_argument("fluid source/sink input needs "
                                      "nonnegative recorded grid statistics "
                                      "within voxel budget");
        recorded += bytes->value() + 4 * metadataBytes(*metadata);
        budget(recorded, p,
               "combined fluid source/sink grids exceed memory before loading");
        found[index] = true;
        descriptors[index] = it.gridName();
      }
  }
  for (bool exists : found)
    if (!exists)
      throw std::invalid_argument("fluid source/sink input is missing a "
                                  "configured canonical state grid");
  openvdb::GridPtrVec grids;
  long double actual = encoded + Fixed;
  for (const auto &descriptor : descriptors) {
    auto grid = file.readGrid(descriptor);
    actual += grid->memUsage() + 4 * metadataBytes(*grid);
    budget(actual, p,
           "combined loaded fluid source/sink state exceeds memory budget");
    grids.push_back(std::move(grid));
  }
  file.close();
  return {openvdb::gridPtrCast<Scalar>(grids[0]),
          openvdb::gridPtrCast<Scalar>(grids[1]),
          openvdb::gridPtrCast<Scalar>(grids[3]),
          openvdb::gridPtrCast<Mac>(grids[2]),
          openvdb::gridPtrCast<Mask>(grids[4])};
}
double validateState(const State &state, const FluidSourcesOptions &s,
                     const ProcessingOptions &p,
                     std::vector<openvdb::Coord> &cells) {
  const auto &mask = *state.domain;
  if (mask.background() || !mask.transform().isLinear() ||
      !mask.hasUniformVoxels())
    throw std::invalid_argument("fluid source/sink domain needs zero "
                                "background and uniform affine voxels");
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
          "fluid source/sink domain voxel sizes must be uniform in [1e-6,1e6]");
    if (!axis)
      h = length;
    axes[axis] /= length;
  }
  for (size_t a = 0; a < 3; ++a)
    for (size_t b = a + 1; b < 3; ++b)
      if (std::abs(axes[a].dot(axes[b])) > 1e-9)
        throw std::invalid_argument(
            "fluid source/sink domain axes must be orthogonal");
  for (const auto &grid : state.grids()) {
    if (grid->transform() != mask.transform())
      throw std::invalid_argument(
          "all fluid source/sink state transforms must exactly match");
  }
  (void)topology(state, p);
  if (mask.activeVoxelCount() > s.maxFluidCells)
    throw std::invalid_argument(
        "combined fluid source/sink state topology exceeds voxel/cell budgets");
  if (mask.tree().hasActiveTiles())
    throw std::invalid_argument(
        "canonical fluid source/sink domain must contain "
        "voxelized active cells, not active tiles");
  for (auto value = mask.cbeginValueOn(); value; ++value)
    if (*value) {
      const auto c = value.getCoord();
      for (size_t a = 0; a < 3; ++a)
        if (int64_t(c[a]) < int64_t(INT32_MIN) + 16 ||
            int64_t(c[a]) > int64_t(INT32_MAX) - 16)
          throw std::invalid_argument(
              "fluid source/sink domain exceeds safe stencil coordinates");
      cells.push_back(c);
    }
  if (cells.empty())
    throw std::invalid_argument(
        "fluid source/sink domain contains no active true cells");
  std::sort(cells.begin(), cells.end());
  const auto fluid = [&](const openvdb::Coord &c) {
    return mask.tree().isValueOn(c) && mask.tree().getValue(c);
  };
  const auto scalar = [&](const Scalar &grid, bool pressure) {
    if (grid.background() != 0)
      throw std::invalid_argument(
          "fluid source/sink scalar backgrounds must be zero");
    for (auto value = grid.cbeginValueAll(); value; ++value) {
      const double v = *value;
      if (!std::isfinite(v) || (!pressure && (v < 0 || v > 1e6)) ||
          (!value.isValueOn() && v != 0))
        throw std::invalid_argument("fluid source/sink scalar state has "
                                    "unsupported values/inactive payloads");
      if (v != 0) {
        const auto box = value.getBoundingBox();
        for (auto c = box.begin(); c; ++c)
          if (!fluid(*c))
            throw std::invalid_argument(
                "fluid source/sink scalar is nonzero outside fluid");
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
    throw std::invalid_argument("fluid source/sink scalar/domain classes must "
                                "match a canonical smoke state");
  const auto &velocity = *state.velocity;
  if (velocity.getGridClass() != openvdb::GRID_STAGGERED ||
      velocity.isInWorldSpace() ||
      velocity.getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE ||
      velocity.background() != openvdb::Vec3d(0))
    throw std::invalid_argument(
        "fluid source/sink velocity must be closed native "
        "local MAC with zero background");
  for (auto value = velocity.cbeginValueAll(); value; ++value) {
    bool nonzero = false;
    for (size_t a = 0; a < 3; ++a) {
      if (!std::isfinite((*value)[a]) || std::abs((*value)[a]) > 1e24)
        throw std::invalid_argument(
            "fluid source/sink MAC values exceed finite range");
      nonzero |= (*value)[a] != 0;
    }
    if (nonzero) {
      const auto box = value.getBoundingBox();
      for (auto c = box.begin(); c; ++c)
        if (!fluid(*c))
          throw std::invalid_argument(
              "fluid source/sink MAC has nonzero values outside fluid");
    }
  }
  for (const auto &c : cells)
    for (size_t a = 0; a < 3; ++a)
      if ((!fluid(c - offset(a)) && velocity.tree().getValue(c)[a] != 0) ||
          (!fluid(c + offset(a)) &&
           velocity.tree().getValue(c + offset(a))[a] != 0))
        throw std::invalid_argument("fluid source/sink input exterior normal "
                                    "faces must already be zero");
  openvdb::CoordBBox box;
  for (const auto &cell : cells)
    box.expand(cell);
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
            "fluid source/sink domain world precision is insufficient");
  }
  return h;
}

std::vector<FluidSourceSphere>
validateOptions(const FluidSourcesOptions &s, const ProcessingOptions &p,
                const std::filesystem::path &output) {
  if (!std::isfinite(s.timeStep) || s.timeStep < 1e-6 || s.timeStep > 3600 ||
      !std::isfinite(s.ambientTemperature) || s.ambientTemperature < 0 ||
      s.ambientTemperature > 1e6 || !s.maxFluidCells ||
      s.maxFluidCells > 1'000'000 || s.sources.size() > 64)
    throw std::invalid_argument(
        "fluid sources require dt [1e-6,3600], ambient Kelvin "
        "[0,1e6], cells [1,1000000] and at most 64 spheres");
  if (!p.maxInputBytes || !p.maxVoxelCount)
    throw std::invalid_argument("fluid source/sink budgets must be positive");
  std::set<std::string> unique;
  for (const auto &name : names(s))
    if (name.empty() || name.size() > 256 || !unique.insert(name).second)
      throw std::invalid_argument(
          "five fluid source/sink names must be distinct "
          "and contain 1..256 bytes");
  budget(Fixed + 2.0L * s.sources.size() * sizeof(FluidSourceSphere), p,
         "fluid source/sink option workspace exceeds memory budget");
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "fluid source/sink output must be a new path in an existing directory");
  auto sources = s.sources;
  for (auto &source : sources) {
    if (!std::isfinite(source.radius) || source.radius < 1e-6 ||
        source.radius > 1e6)
      throw std::invalid_argument(
          "fluid source/sink sphere radius must be in [1e-6,1e6]");
    for (double &value : source.center) {
      if (!std::isfinite(value) || std::abs(value) > 1e12)
        throw std::invalid_argument(
            "fluid source/sink centers must be finite and at most 1e12");
      if (value == 0)
        value = 0; // Canonicalize signed zero before sorting.
    }
    for (double *value :
         {&source.densityRate, &source.temperatureRate,
          &source.densityDecayRate, &source.temperatureRelaxationRate}) {
      if (!std::isfinite(*value) || *value < 0 || *value > 1e6)
        throw std::invalid_argument(
            "fluid source/sink rates must be finite in [0,1e6]");
      if (*value == 0)
        *value = 0;
    }
  }
  std::sort(sources.begin(), sources.end(), [](const auto &a, const auto &b) {
    return std::tie(a.center, a.radius, a.densityRate, a.temperatureRate,
                    a.densityDecayRate, a.temperatureRelaxationRate,
                    a.clearDensity, a.resetTemperature) <
           std::tie(b.center, b.radius, b.densityRate, b.temperatureRate,
                    b.densityDecayRate, b.temperatureRelaxationRate,
                    b.clearDensity, b.resetTemperature);
  });
  return sources;
}

struct Sum {
  double value{0}, correction{0};
  void add(double x) {
    const double next = value + x;
    correction += std::abs(value) >= std::abs(x) ? (value - next) + x
                                                 : (x - next) + value;
    value = next;
  }
  double total() const {
    const double result = value + correction;
    if (!std::isfinite(result) || result < 0)
      throw std::runtime_error(
          "fluid source/sink accumulation exceeds finite range");
    return result;
  }
};

double scaled(double value, std::initializer_list<double> factors,
              int binaryShift = 0, bool allowDiagnosticZero = false) {
  if (value == 0)
    return 0;
  int exponent;
  double mantissa = std::frexp(value, &exponent);
  exponent += binaryShift;
  for (double factor : factors) {
    if (factor == 0)
      return 0;
    int e;
    mantissa *= std::frexp(factor, &e);
    exponent += e;
  }
  const double result = std::ldexp(mantissa, exponent);
  if (!std::isfinite(result) || (result == 0 && !allowDiagnosticZero))
    throw std::runtime_error(
        "fluid source/sink nonzero term is outside Double range; "
        "reduce time/rates or use an explicit clear/reset");
  return result;
}

// Form value*exp(-optical) without first underflowing exp(-optical). This
// preserves e.g. 1e6*exp(-750), whose final Double remains representable.
double decay(double value, double optical) {
  if (value == 0 || optical == 0)
    return value;
  if (optical > 2048 * std::numbers::ln2)
    throw std::runtime_error("fluid source/sink stiff decay has a nonzero term "
                             "below Double range; use an explicit clear/reset");
  const int power = static_cast<int>(std::floor(optical / std::numbers::ln2));
  constexpr double ln2High = 0.693147180369123816490;
  constexpr double ln2Low = 1.90821492927058770002e-10;
  const double remainder = (optical - power * ln2High) - power * ln2Low;
  return scaled(value, {std::exp(-remainder)}, -power);
}

double gain(double rate, double timeStep) {
  if (rate == 0)
    return timeStep;
  const double optical = rate * timeStep;
  if (optical < 1e-4) {
    // phi(-x)=(1-exp(-x))/x has limit1 even if rate*dt underflows.
    const double phi =
        1 + optical * (-.5 + optical * (1. / 6 +
                                        optical * (-1. / 24 + optical / 120)));
    return timeStep * phi;
  }
  return -std::expm1(-optical) / rate;
}

double spacing(double value) {
  return std::max(std::abs(std::nextafter(value, INFINITY) - value),
                  std::abs(std::nextafter(value, -INFINITY) - value));
}
// A nonnegative constant-coefficient ODE stays between its initial value and
// equilibrium. Enforce that analytic convex hull only for a few ULPs of
// arithmetic overshoot, just as bounded interpolation does; this is not a
// field/mass cap.
double boundedRoundoff(double value, double initial, double equilibrium) {
  const double low = std::min(initial, equilibrium),
               high = std::max(initial, equilibrium);
  if (value < low) {
    if (low - value > 8 * spacing(low))
      throw std::runtime_error(
          "fluid source/sink ODE violated its lower equilibrium bound");
    return low;
  }
  if (value > high) {
    if (value - high > 8 * spacing(high))
      throw std::runtime_error(
          "fluid source/sink ODE violated its upper equilibrium bound");
    return high;
  }
  return value;
}
struct Membership {
  const openvdb::math::MapBase &map;
  double tolerance;
  openvdb::Vec3d origin;
  std::array<openvdb::Vec3d, 3> axes;
  Membership(const Mask &mask, double h,
             const std::vector<FluidSourceSphere> &sources)
      : map(*mask.transform().baseMap()), tolerance(h * 1e-4),
        origin(map.applyMap(openvdb::Vec3d(0))) {
    for (const auto &source : sources)
      tolerance = std::min(tolerance, source.radius * 1e-4);
    for (size_t axis = 0; axis < 3; ++axis) {
      openvdb::Vec3d unit(0);
      unit[axis] = 1;
      axes[axis] = map.applyJacobian(unit);
    }
    for (const auto &source : sources)
      for (double value : source.center)
        require(value);
  }
  void require(double value) const {
    if (!std::isfinite(value) || spacing(value) > tolerance)
      throw std::invalid_argument(
          "fluid source/sink world precision is insufficient "
          "relative to sphere radius and voxel size; recenter the "
          "state/source");
  }
  openvdb::Vec3d world(const openvdb::Coord &cell) const {
    const auto point =
        map.applyMap(openvdb::Vec3d(cell.x(), cell.y(), cell.z()));
    for (size_t axis = 0; axis < 3; ++axis) {
      // Include the affine terms, so cancellation of a large index/translation
      // cannot hide precision loss behind a small final world coordinate.
      require(origin[axis]);
      double error = spacing(origin[axis]) + spacing(point[axis]);
      for (size_t term = 0; term < 3; ++term)
        error += spacing(axes[term][axis] * double(cell[term]));
      if (error > tolerance)
        throw std::invalid_argument(
            "fluid source/sink affine cell-center precision "
            "is insufficient relative to radius/voxel size");
      require(point[axis]);
    }
    return point;
  }
};

struct Rates {
  std::array<Sum, 4> sums;
  bool affected{false}, clear{false}, reset{false};
};
Rates ratesAt(const openvdb::Vec3d &world,
              const std::vector<FluidSourceSphere> &sources) {
  Rates rates;
  for (const auto &source : sources) {
    const double distance =
        std::hypot(world.x() - source.center[0], world.y() - source.center[1],
                   world.z() - source.center[2]);
    if (distance > source.radius)
      continue;
    rates.affected = true;
    rates.clear |= source.clearDensity;
    rates.reset |= source.resetTemperature;
    rates.sums[0].add(source.densityRate);
    rates.sums[1].add(source.temperatureRate);
    rates.sums[2].add(source.densityDecayRate);
    rates.sums[3].add(source.temperatureRelaxationRate);
  }
  return rates;
}

std::array<double, 2> update(double density, double temperature,
                             const Rates &rates, const FluidSourcesOptions &s) {
  if (!rates.affected)
    return {density, temperature};
  std::array<double, 4> r;
  for (size_t i = 0; i < 4; ++i) {
    r[i] = rates.sums[i].total();
    if (r[i] > 1e6)
      throw std::invalid_argument(
          "overlapping fluid source/sink rates exceed 1e6 per second");
  }
  if (rates.clear)
    density = 0;
  if (rates.reset)
    temperature = s.ambientTemperature;
  Sum d, t;
  d.add(decay(density, r[2] * s.timeStep));
  d.add(scaled(r[0], {gain(r[2], s.timeStep)}));
  if (temperature == s.ambientTemperature)
    t.add(temperature);
  else {
    t.add(decay(temperature, r[3] * s.timeStep));
    // Combine ambient*rate*gain in exponent space: rate*dt can itself
    // underflow even when multiplication by a large ambient is representable.
    const double optical = r[3] * s.timeStep;
    t.add(optical < 1e-4
              ? scaled(s.ambientTemperature, {r[3], gain(r[3], s.timeStep)})
              : scaled(s.ambientTemperature, {-std::expm1(-optical)}));
  }
  double baseTemperature =
      boundedRoundoff(t.total(), temperature, s.ambientTemperature);
  Sum heated;
  heated.add(baseTemperature);
  heated.add(scaled(r[1], {gain(r[3], s.timeStep)}));
  double finalDensity = d.total(), finalTemperature = heated.total();
  if (r[2] > 0) {
    const double equilibrium = r[0] / r[2];
    if (r[0] != 0 && equilibrium == 0)
      throw std::runtime_error("fluid source/sink nonzero forcing equilibrium "
                               "is below Double range");
    if (std::isfinite(equilibrium))
      finalDensity = boundedRoundoff(finalDensity, density, equilibrium);
  }
  if (r[3] > 0) {
    const double forcingEquilibrium = r[1] / r[3];
    if (r[1] != 0 && forcingEquilibrium == 0)
      throw std::runtime_error("fluid source/sink nonzero heating equilibrium "
                               "is below Double range");
    const double equilibrium = s.ambientTemperature + forcingEquilibrium;
    if (std::isfinite(equilibrium))
      finalTemperature =
          boundedRoundoff(finalTemperature, temperature, equilibrium);
  }
  const std::array<double, 2> values{finalDensity, finalTemperature};
  for (double value : values)
    if (value > 1e6)
      throw std::invalid_argument(
          "fluid source/sink scalar result exceeds [0,1e6]; no output written");
  return values;
}
#endif
} // namespace

FluidSourcesResult WriteFluidSources(const std::filesystem::path &input,
                                     const std::filesystem::path &output,
                                     const FluidSourcesOptions &s,
                                     const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  const auto sources = validateOptions(s, p, output);
  static std::once_flag initialized;
  std::call_once(initialized, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    uintmax_t encoded;
    auto state = read(input, s, p, encoded);
    const auto active = state.domain->activeVoxelCount();
    budget(encoded + 3 * memory(state) + 160.0L * active + Fixed, p,
           "fluid source/sink combined state/cell workspace exceeds memory "
           "budget");
    std::vector<openvdb::Coord> cells;
    const double h = validateState(state, s, p, cells);
    if (!sources.empty()) {
      const long double prospective =
          8192.0L * state.domain->tree().leafCount() +
          512.0L * 1024 * state.domain->tree().nonLeafCount() + Fixed;
      budget(encoded + 3 * memory(state) + 160.0L * active + 2 * prospective +
                 Fixed,
             p,
             "fluid source/sink prospective scalar trees exceed shared memory "
             "budget");
    }
    FluidSourcesResult result;
    result.fluidCellCount = cells.size();
    std::vector<std::array<double, 2>> values;
    values.reserve(cells.size());
    Sum densityBefore, densityAfter, temperatureBefore, temperatureAfter;
    auto [prospectiveActive, prospectiveLeaves] = topology(state, p);
    std::set<openvdb::Coord> newDensityLeaves, newTemperatureLeaves;
    std::unique_ptr<Membership> membership;
    if (!sources.empty())
      membership = std::make_unique<Membership>(*state.domain, h, sources);
    for (const auto &cell : cells) {
      const double oldD = state.density->tree().getValue(cell),
                   oldT = state.temperature->tree().getValue(cell);
      const auto rates =
          membership ? ratesAt(membership->world(cell), sources) : Rates{};
      const auto value = update(oldD, oldT, rates, s);
      values.push_back(value);
      densityBefore.add(oldD);
      densityAfter.add(value[0]);
      temperatureBefore.add(oldT);
      temperatureAfter.add(value[1]);
      result.affectedCellCount += rates.affected;
      result.clearedDensityCells += rates.clear;
      result.resetTemperatureCells += rates.reset;
      result.changedCellCount += (value[0] != oldD || value[1] != oldT);
      openvdb::Coord origin = cell;
      for (size_t axis = 0; axis < 3; ++axis)
        origin[axis] &= ~7;
      if (value[0] != oldD) {
        if (!state.density->tree().isValueOn(cell)) {
          if (prospectiveActive == p.maxVoxelCount)
            throw std::invalid_argument("fluid source/sink output active "
                                        "topology exceeds shared voxel budget");
          ++prospectiveActive;
        }
        if (!state.density->tree().probeLeaf(cell))
          newDensityLeaves.insert(origin);
      }
      if (value[1] != oldT) {
        if (!state.temperature->tree().isValueOn(cell)) {
          if (prospectiveActive == p.maxVoxelCount)
            throw std::invalid_argument("fluid source/sink output active "
                                        "topology exceeds shared voxel budget");
          ++prospectiveActive;
        }
        if (!state.temperature->tree().probeLeaf(cell))
          newTemperatureLeaves.insert(origin);
      }
    }
    const uint64_t leafCap =
        p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0);
    if (newDensityLeaves.size() + newTemperatureLeaves.size() >
        leafCap - prospectiveLeaves)
      throw std::invalid_argument("fluid source/sink prospective scalar leaves "
                                  "exceed shared stored-leaf budget");
    result.densityMassBefore =
        scaled(densityBefore.total(), {h, h, h}, 0, true);
    result.densityMassAfter = scaled(densityAfter.total(), {h, h, h}, 0, true);
    result.temperatureIntegralBefore =
        scaled(temperatureBefore.total(), {h, h, h}, 0, true);
    result.temperatureIntegralAfter =
        scaled(temperatureAfter.total(), {h, h, h}, 0, true);
    for (size_t i = 0; i < cells.size(); ++i) {
      if (values[i][0] != state.density->tree().getValue(cells[i]))
        state.density->tree().setValue(cells[i], values[i][0]);
      if (values[i][1] != state.temperature->tree().getValue(cells[i]))
        state.temperature->tree().setValue(cells[i], values[i][1]);
    }
    (void)topology(state, p);
    for(const auto& grid:state.grids())detail::RemoveBoundaryProjectionMetadata(*grid);
    budget(encoded + 3 * memory(state) + 160.0L * active + Fixed, p,
           "fluid source/sink final writer workspace exceeds memory budget");
    detail::WriteFileExclusive(output, state.grids(), p.maxInputBytes,
                               openvdb::io::COMPRESS_ZIP);
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
