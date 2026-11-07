#include "Container/geometry/OpenVdbLevelSetTransport.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <cstring>
#include <functional>
#include <mutex>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/LevelSetAdvect.h>
#include <openvdb/tools/LevelSetMorph.h>
#include <set>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB level-set transport is unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = openvdb::Vec3d;
constexpr long double fixedBytes = 4.0L * 1024 * 1024;
void budget(long double value, const ProcessingOptions &p, const char *reason) {
  if (!std::isfinite(value) || value > p.maxInputBytes)
    throw std::invalid_argument(reason);
}
long double metadataBytes(const openvdb::MetaMap &map) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto it = map.beginMeta(); it != map.endMeta(); ++it)
    bytes += 2.0L * (it->first.size() + it->second->typeName().size() + it->second->size()) + 256;
  return bytes;
}
double gap(double value) {
  return std::max(std::nextafter(value, INFINITY) - value,
                  value - std::nextafter(value, -INFINITY));
}
double trackerResolution(float band) {
  return 8.0 * std::max(double(std::nextafter(band, INFINITY)) - band,
                        double(band) - std::nextafter(band, -INFINITY));
}
bool finite(const V &v) {
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}
double length(const V &v) { return std::hypot(v.x(), v.y(), v.z()); }
V inverseVector(const openvdb::math::MapBase &map, const V &value) {
  const std::array<V, 3> columns{map.applyInverseJacobian(V(1, 0, 0)),
                                 map.applyInverseJacobian(V(0, 1, 0)),
                                 map.applyInverseJacobian(V(0, 0, 1))};
  V result(0);
  for (int k = 0; k < 3; ++k) {
    double sum = 0, correction = 0;
    for (int j = 0; j < 3; ++j) {
      const double term = columns[j][k] * value[j];
      if (!std::isfinite(term) || (columns[j][k] != 0 && value[j] != 0 && term == 0))
        throw std::invalid_argument("level-set velocity frame conversion overflows or underflows");
      const double next = sum + term;
      correction += std::abs(sum) >= std::abs(term) ? (sum - next) + term : (term - next) + sum;
      sum = next;
    }
    result[k] = sum + correction;
  }
  return result;
}
void validateOptions(const std::filesystem::path &output, const LevelSetTransportOptions &t,
                     const ProcessingOptions &p, bool morph) {
  if (!std::isfinite(t.timeStep) || std::abs(t.timeStep) > 3600 ||
      (morph && (t.timeStep < 0 || t.timeStep > 8)))
    throw std::invalid_argument(morph ? "level-set morph pseudo-time must be in [0,8]"
                                      : "level-set advection time must be in [-3600,3600] seconds");
  if (t.normalizationIterations < 1 || t.normalizationIterations > 8 || t.maxCflSteps < 1 ||
      t.maxCflSteps > 4096 || t.maxWork < 1 || t.maxWork > 10'000'000'000ull)
    throw std::invalid_argument("invalid level-set tracker/CFL/work limits");
  if (!p.maxVoxelCount || !p.maxInputBytes || p.gridName.empty() || p.gridName.size() > 256)
    throw std::invalid_argument(
        "level-set transport requires positive budgets and a 1..256-byte output name");
  if (output.empty())
    throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("output already exists; choose a new .vdb path");
  if (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}
uintmax_t encodedBytes(const std::filesystem::path &path, const ProcessingOptions &p) {
  if (!std::filesystem::is_regular_file(path))
    throw std::invalid_argument("level-set input is not a regular file");
  const auto n = std::filesystem::file_size(path);
  budget(n + fixedBytes, p, "level-set transport encoded input exceeds the memory budget");
  detail::RequireSeekableFile(path);
  return n;
}
openvdb::GridBase::Ptr read(const std::filesystem::path &path, const std::string &selection,
                            bool velocity, const ProcessingOptions &p, long double resident) {
  openvdb::io::File file(path.string());
  file.open(false);
  std::set<std::string> catalog;
  long double catalogBytes = resident + fixedBytes;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto name = it.gridName();
    catalogBytes += name.size() * 2.0L + 128;
    budget(catalogBytes, p, "level-set transport descriptor catalog exceeds the memory budget");
    if (!catalog.insert(name).second)
      throw std::invalid_argument(
          "level-set transport input has colliding descriptors; rename grids first");
  }
  std::string descriptor;
  openvdb::GridBase::Ptr selected;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    auto meta = file.readGridMetadata(it.gridName());
    const bool supported =
        velocity ? (meta->isType<openvdb::Vec3SGrid>() || meta->isType<openvdb::Vec3DGrid>())
                 : (meta->isType<openvdb::FloatGrid>() &&
                    meta->getGridClass() == openvdb::GRID_LEVEL_SET);
    if (!supported || !GridSelectionMatches(selection, it.gridName(), meta->getName()))
      continue;
    if (selected)
      throw std::invalid_argument(
          "level-set transport selector is ambiguous; use descriptor:<exact name>");
    const auto count =
        meta->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    const auto memory =
        meta->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!count || !memory || count->value() < 0 || memory->value() < 0)
      throw std::invalid_argument(
          "level-set transport requires recorded topology/memory statistics");
    if (uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument("level-set transport stored voxel count exceeds the budget");
    budget(catalogBytes + 3.0L * memory->value() + 4 * metadataBytes(*meta), p,
           "level-set transport stored memory exceeds the budget");
    selected = std::move(meta);
    descriptor = it.gridName();
    if (selection.empty())
      break;
  }
  if (!selected)
    throw std::invalid_argument(velocity ? "no matching Vec3S/Vec3D velocity grid found"
                                         : "no matching FloatGrid level set found");
  auto grid = file.readGrid(descriptor);
  file.close();
  budget(resident + 3.0L * grid->memUsage() + 4 * metadataBytes(*grid) + fixedBytes, p,
         "level-set transport loaded memory exceeds the budget");
  const uint64_t maxLeaves = p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0);
  if (grid->activeVoxelCount() > p.maxVoxelCount || grid->baseTree().leafCount() > maxLeaves)
    throw std::invalid_argument("level-set transport stored topology exceeds the voxel budget");
  return grid;
}
double transform(const openvdb::GridBase &grid, bool uniform) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument("level-set transport requires nonsingular affine transforms");
  const auto map = grid.transform().baseMap();
  const V origin = map->applyMap(V(0));
  if (!finite(origin))
    throw std::invalid_argument("nonfinite level-set transport transform");
  std::array<V, 3> basis{map->applyJacobian(V(1, 0, 0)), map->applyJacobian(V(0, 1, 0)),
                         map->applyJacobian(V(0, 0, 1))};
  std::array<double, 3> sizes;
  for (int k = 0; k < 3; ++k) {
    sizes[k] = length(basis[k]);
    if (!finite(basis[k]) || sizes[k] < 1e-6 || sizes[k] > 1e6)
      throw std::invalid_argument(
          "level-set transport voxel axes must span [1e-6,1e6] world units");
    basis[k] /= sizes[k];
  }
  if (std::abs(basis[0].dot(basis[1].cross(basis[2]))) < 1e-12)
    throw std::invalid_argument("level-set transport transform is singular or ill-conditioned");
  if (uniform &&
      (std::abs(sizes[1] / sizes[0] - 1) > 1e-6 || std::abs(sizes[2] / sizes[0] - 1) > 1e-6 ||
       std::abs(basis[0].dot(basis[1])) > 1e-6 || std::abs(basis[0].dot(basis[2])) > 1e-6 ||
       std::abs(basis[1].dot(basis[2])) > 1e-6))
    throw std::invalid_argument(
        "level-set transport requires uniform orthogonal source voxels; resample first");
  return sizes[0];
}
void bounds(const openvdb::CoordBBox &box, const ProcessingOptions &p, int padding = 0) {
  if (box.empty())
    return;
  long double count = 1;
  for (int k = 0; k < 3; ++k) {
    if (int64_t(box.min()[k]) - padding < INT32_MIN + 16ll ||
        int64_t(box.max()[k]) + padding > INT32_MAX - 16ll)
      throw std::invalid_argument("level-set transport exceeds safe index coordinates");
    count *= static_cast<long double>(box.max()[k]) - box.min()[k] + 1 + 2 * padding;
    if (count > p.maxVoxelCount)
      throw std::invalid_argument("level-set transport bounding topology exceeds the voxel budget");
  }
}
void validateSdf(const openvdb::FloatGrid &grid, const ProcessingOptions &p, double dx) {
  const double band = grid.background(), width = band / dx;
  if (!std::isfinite(band) || band <= 0 || width < 3 - 1e-6 || width > 64 + 1e-6)
    throw std::invalid_argument(
        "level-set transport requires a positive exterior band of 3..64 source voxels");
  const auto box = grid.evalActiveVoxelBoundingBox();
  bounds(box, p, 4);
  const double tolerance = band * 2e-6;
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    const double v = *value;
    if (!std::isfinite(v) || std::abs(v) > band + tolerance)
      throw std::invalid_argument(
          "level-set stored distances must be finite and inside the preserved band");
    if (value.isValueOn() && value.getLevel() != 0)
      throw std::invalid_argument(
          "level-set transport does not accept active tiles; voxelize the narrow band first");
    if (!value.isValueOn()) {
      if (std::abs(std::abs(v) - band) > tolerance)
        throw std::invalid_argument(
            "level-set inactive values must equal the signed band background");
      if (v < 0) {
        const auto negative = value.getBoundingBox();
        if (box.empty() || !box.isInside(negative.min()) || !box.isInside(negative.max()))
          throw std::invalid_argument(
              "level-set transport requires a bounded negative interior and positive exterior");
      }
    }
  }
}
template <class Grid>
double velocityMaximum(const Grid &grid, const openvdb::math::MapBase &sourceMap,
                       const ProcessingOptions &p) {
  (void)transform(grid, false);
  if (!grid.isInWorldSpace() || grid.getGridClass() == openvdb::GRID_STAGGERED ||
      (grid.getVectorType() != openvdb::VEC_INVARIANT &&
       grid.getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE))
    throw std::invalid_argument("level-set velocity must be collocated world-space invariant or "
                                "contravariant-relative vectors");
  auto box = grid.evalActiveVoxelBoundingBox();
  double maximum = 0;
  const auto check = [&](const auto &v) {
    for (int k = 0; k < 3; ++k)
      if (!std::isfinite(double(v[k])) || std::abs(double(v[k])) > 1e12)
        throw std::invalid_argument(
            "level-set velocity components must be finite with magnitude <=1e12");
    const V local = inverseVector(sourceMap, V(v));
    if (!finite(local))
      throw std::invalid_argument("level-set local velocity is not finite");
    maximum = std::max(maximum, length(local));
  };
  check(grid.background());
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    check(*value);
    if (*value != grid.background())
      box.expand(value.getBoundingBox());
  }
  bounds(box, p);
  return maximum;
}
int64_t floorBlock(int64_t v, int64_t size) { return v >= 0 ? v / size : (v + 1) / size - 1; }
long double nodeCount(const openvdb::CoordBBox &box, int64_t size, int padding) {
  long double n = 1;
  for (int k = 0; k < 3; ++k)
    n *= floorBlock(int64_t(box.max()[k]) + padding, size) -
         floorBlock(int64_t(box.min()[k]) - padding, size) + 1;
  return n;
}
struct Progress {
  openvdb::FloatGrid &grid;
  const ProcessingOptions &options;
  const LevelSetTransportOptions &transport;
  long double retained;
  bool morph;
  uint32_t cfl{}, morphPhase{};
  uint64_t work{};
  void charge(uint64_t count) {
    if (count > transport.maxWork - work)
      throw std::invalid_argument("level-set transport work budget exceeded; no partial output");
    work += count;
  }
  void preflight() const {
    const auto box = grid.evalActiveVoxelBoundingBox();
    budget(retained + 4.0L * grid.memUsage() + 4 * metadataBytes(grid) + fixedBytes, options,
           "level-set transport retained workspace exceeds the memory budget");
    if (box.empty())
      return;
    bounds(box, options, 4);
    // One tracker dilation plus radius-three WENO stencils, rounded to actual
    // leaf/internal boundaries. Existing remote inactive nodes remain charged
    // through grid.memUsage; double-counting overlap is conservative.
    const auto leaves = nodeCount(box, 8, 4), lower = nodeCount(box, 128, 4),
               upper = nodeCount(box, 4096, 4);
    const long double maxLeaves = options.maxVoxelCount / 512 + (options.maxVoxelCount % 512 != 0);
    uint64_t remote = 0;
    for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
      const auto origin = leaf->origin();
      bool outside = false;
      for (int k = 0; k < 3; ++k)
        outside |= floorBlock(origin[k], 8) < floorBlock(int64_t(box.min()[k]) - 4, 8) ||
                   floorBlock(origin[k], 8) > floorBlock(int64_t(box.max()[k]) + 4, 8);
      if (outside)
        ++remote;
    }
    if (leaves + remote > maxLeaves)
      throw std::invalid_argument(
          "level-set transport prospective leaf slots exceed the voxel budget");
    using Tree = openvdb::FloatTree;
    using Leaf = Tree::LeafNodeType;
    using Upper = Tree::RootNodeType::ChildNodeType;
    using Lower = Upper::ChildNodeType;
    const long double prospective = leaves * (4.0L * sizeof(Leaf) + 512.0L * 64) +
                                    lower * (3.0L * sizeof(Lower) + 1024) +
                                    upper * (3.0L * sizeof(Upper) + 1024);
    budget(retained + 4.0L * grid.memUsage() + 4 * metadataBytes(grid) + prospective + fixedBytes,
           options,
           "level-set transport leaf-aligned tracker/velocity/auxiliary workspace exceeds the "
           "memory budget");
  }
  void start(const char *phase = nullptr) {
    preflight();
    if (phase &&
        ((!morph && std::strcmp(phase, "Sampling advection field") == 0) ||
         (morph && std::strcmp(phase, "Morphing level set") == 0 && morphPhase++ % 3 == 0))) {
      if (cfl == transport.maxCflSteps)
        throw std::invalid_argument(
            "level-set transport CFL step budget exceeded; no partial output");
      ++cfl;
    }
    charge(std::max<uint64_t>(1, uint64_t(grid.tree().leafCount()) * 512));
  }
  void end() { preflight(); }
  bool wasInterrupted(int = -1) { return false; }
};
void worldPrecision(const openvdb::math::Transform &original, const openvdb::CoordBBox &box,
                    double dx) {
  if (box.empty())
    return;
  for (unsigned corner = 0; corner < 8; ++corner) {
    V index;
    for (int k = 0; k < 3; ++k)
      index[k] = (corner & (1u << k)) ? int64_t(box.max()[k]) + 4 : int64_t(box.min()[k]) - 4;
    const V world = original.indexToWorld(index);
    for (int k = 0; k < 3; ++k)
      if (!std::isfinite(world[k]) || gap(world[k]) > dx * 1e-4)
        throw std::invalid_argument(
            "level-set transport lacks world precision; recenter the source");
  }
}
template <class Grid> struct DoubleView {
  using ValueType = V;
  const Grid &grid;
  V getValue(const openvdb::Coord &c) const { return V(grid.tree().getValue(c)); }
};
template <class Grid> struct Field {
  using VectorType = openvdb::Vec3f;
  const Grid &velocity;
  const openvdb::math::Transform &sourceTransform;
  const openvdb::math::Transform &workingTransform;
  Progress &progress;
  double maximum, dx;
  const openvdb::math::Transform &transform() const { return workingTransform; }
  VectorType operator()(const openvdb::Coord &c, float) const { return at(c.asVec3d()); }
  VectorType operator()(const V &p, float) const { return at(workingTransform.worldToIndex(p)); }
  VectorType at(const V &coordinate) const {
    progress.charge(1);
    const V world = sourceTransform.indexToWorld(coordinate);
    const auto map = velocity.transform().baseMap();
    const V origin = map->applyMap(V(0));
    const V index = map->applyInverseJacobian(world - origin);
    if (!finite(index))
      throw std::invalid_argument("level-set velocity query is not finite");
    for (int k = 0; k < 3; ++k) {
      if (index[k] < double(INT32_MIN) + 8 || index[k] > double(INT32_MAX) - 8)
        throw std::invalid_argument("level-set velocity sampling exceeds safe index coordinates");
      V delta(0);
      delta[k] = gap(index[k]);
      if (length(map->applyJacobian(delta)) > dx * 1e-4 || gap(world[k]) > dx * 1e-4)
        throw std::invalid_argument(
            "level-set velocity sampling lacks source-index/world precision; recenter grids");
    }
    const V recovered = map->applyMap(index);
    if (!finite(recovered) || length(recovered - world) > dx * 1e-4)
      throw std::invalid_argument(
          "level-set velocity sampling loses world offsets; recenter grids");
    const V sampled = openvdb::tools::BoxSampler::sample(DoubleView<Grid>{velocity}, index);
    const V unscaled = inverseVector(*sourceTransform.baseMap(), sampled);
    const V local = unscaled / maximum;
    if (!finite(local))
      throw std::invalid_argument("level-set normalized velocity is not finite");
    VectorType result;
    for (int k = 0; k < 3; ++k) {
      result[k] = float(local[k]);
      if (!std::isfinite(result[k]) || (unscaled[k] != 0 && (local[k] == 0 || result[k] == 0)))
        throw std::invalid_argument("level-set normalized velocity loses Float32 dynamic range");
    }
    return result;
  }
};
void rescale(openvdb::FloatGrid &grid, double factor, float background) {
  const float oldBackground = grid.background();
  for (auto value = grid.beginValueAll(); value; ++value) {
    const bool on = value.isValueOn();
    const float old = *value;
    const float result = old == oldBackground    ? background
                         : old == -oldBackground ? -background
                                                 : float(double(old) * factor);
    if (!std::isfinite(result) || (old != 0 && result == 0))
      throw std::invalid_argument("level-set distance scaling overflows or underflows");
    value.setValue(result);
    value.setActiveState(on);
  }
  grid.tree().root().setBackground(background, false);
}
openvdb::math::Transform::Ptr workingTransform(const openvdb::FloatGrid &grid) {
  auto t = openvdb::math::Transform::createLinearTransform(1.0);
  const auto box = grid.evalActiveVoxelBoundingBox();
  if (!box.empty()) {
    V center;
    for (int k = 0; k < 3; ++k)
      center[k] = std::floor((double(box.min()[k]) + box.max()[k]) * .5);
    t->postTranslate(-center);
  }
  return t;
}
template <class T> void replace(openvdb::GridBase &grid, const char *key, const T &value) {
  grid.removeMeta(key);
  grid.insertMeta(key, value);
}
LevelSetTransportResult publish(openvdb::FloatGrid::Ptr grid, const std::filesystem::path &output,
                                const ProcessingOptions &p, const LevelSetTransportOptions &t,
                                Progress &progress, const openvdb::math::Transform::Ptr &original,
                                double dx, float background, uint32_t cfl, bool normalized,
                                const char *method) {
  detail::RemoveBoundaryProjectionMetadata(*grid);
  if (normalized)
    rescale(*grid, dx, background);
  grid->setTransform(original);
  grid->setName(p.gridName);
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  validateSdf(*grid, p, dx);
  worldPrecision(*original, grid->evalActiveVoxelBoundingBox(), dx);
  replace(*grid, "container:level_set_transport", openvdb::StringMetadata(method));
  replace(*grid, "container:level_set_transport_time", openvdb::DoubleMetadata(t.timeStep));
  replace(*grid, "container:level_set_transport_cfl_steps", openvdb::Int64Metadata(cfl));
  replace(*grid, "container:level_set_transport_normalization",
          openvdb::Int32Metadata(t.normalizationIterations));
  replace(*grid, "container:level_set_transport_schemes",
          openvdb::StringMetadata("HJWENO5/TVD_RK2; tracker HJWENO5/TVD_RK2"));
  progress.preflight();
  detail::WriteFileExclusive(output, {grid}, p.maxInputBytes);
  return {{grid->getName(), "level set", grid->activeVoxelCount(), dx}, cfl, progress.work};
}
void initialize() {
  static std::once_flag flag;
  std::call_once(flag, [] { openvdb::initialize(); });
}
LevelSetTransportResult run(const std::filesystem::path &input,
                            const std::filesystem::path &companion,
                            const std::filesystem::path &output, const LevelSetTransportOptions &t,
                            const std::string &inputGrid, const std::string &otherGrid,
                            const ProcessingOptions &p, bool morph) {
  validateOptions(output, t, p, morph);
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto sourceBytes = encodedBytes(input, p), otherBytes = encodedBytes(companion, p);
    const long double encoded = static_cast<long double>(sourceBytes) + otherBytes;
    budget(encoded + fixedBytes, p,
           "level-set transport combined encoded inputs exceed the memory budget");
    auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(read(input, inputGrid, false, p, encoded));
    const double dx = transform(*grid, true);
    validateSdf(*grid, p, dx);
    const auto original = detail::CopyTransformExact(grid->transform());
    const float background = grid->background();
    auto other = read(companion, otherGrid, !morph, p,
                      encoded + grid->memUsage() + 4 * metadataBytes(*grid));
    const long double retained = encoded + 3.0L * other->memUsage() + 4 * metadataBytes(*other);
    Progress progress{*grid, p, t, retained, morph};
    progress.preflight();
    worldPrecision(*original, grid->evalActiveVoxelBoundingBox(), dx);
    const auto workTransform = workingTransform(*grid);
    if (morph) {
      auto target = openvdb::gridPtrCast<openvdb::FloatGrid>(other);
      const auto targetDx = transform(*target, true);
      validateSdf(*target, p, targetDx);
      if (target->transform() != *original || target->background() != background)
        throw std::invalid_argument(
            "level-set morph source/target transforms and bands must match");
      if (2.0 * (background / dx) * t.timeStep > 256)
        throw std::invalid_argument("level-set morph conservative travel exceeds 256 voxels");
      if (t.timeStep == 0)
        return publish(grid, output, p, t, progress, original, dx, background, 0, false,
                       "OpenVDB level-set morph");
      if (grid->empty() || target->empty())
        throw std::invalid_argument(
            "nonzero level-set morph requires nonempty source and target interfaces");
      double driving = 0;
      for (auto value = grid->cbeginValueOn(); value; ++value) {
        progress.charge(1);
        driving = std::max(
            driving, std::abs(double(*value) - target->tree().getValue(value.getCoord())) / dx);
      }
      if (driving == 0)
        return publish(grid, output, p, t, progress, original, dx, background, 0, false,
                       "OpenVDB level-set morph");
      if (driving * t.timeStep < trackerResolution(float(background / dx)))
        throw std::invalid_argument("level-set morph movement is below Float32 band resolution; "
                                    "increase time or shape separation");
      rescale(*grid, 1.0 / dx, float(background / dx));
      rescale(*target, 1.0 / dx, float(background / dx));
      grid->setTransform(workTransform);
      target->setTransform(detail::CopyTransformExact(*workTransform));
      openvdb::tools::LevelSetMorphing<openvdb::FloatGrid, Progress> engine(*grid, *target,
                                                                            &progress);
      engine.setSpatialScheme(openvdb::math::HJWENO5_BIAS);
      engine.setTemporalScheme(openvdb::math::TVD_RK2);
      engine.setTrackerSpatialScheme(openvdb::math::HJWENO5_BIAS);
      engine.setTrackerTemporalScheme(openvdb::math::TVD_RK2);
      engine.setNormCount(int(t.normalizationIterations));
      engine.setGrainSize(0);
      const float end = float(t.timeStep);
      if (end == 0)
        throw std::invalid_argument("level-set morph time is below Float32 progress");
      const auto count = engine.advect(0, end);
      if (count == 0)
        throw std::invalid_argument("nonzero level-set morph time/speed is below the tracker "
                                    "progress threshold; rescale fields");
      return publish(grid, output, p, t, progress, original, dx, background, uint32_t(count), true,
                     "OpenVDB level-set morph");
    }
    const auto invoke = [&]<class Grid>(const Grid &velocity) {
      const double maximum = velocityMaximum(velocity, *original->baseMap(), p);
      const double duration = maximum * std::abs(t.timeStep);
      if (!std::isfinite(duration) || duration > 256)
        throw std::invalid_argument("level-set advection maximum travel exceeds 256 source voxels");
      if (t.timeStep == 0 || maximum == 0 || grid->empty())
        return publish(grid, output, p, t, progress, original, dx, background, 0, false,
                       "OpenVDB level-set advection");
      if (duration < trackerResolution(float(background / dx)))
        throw std::invalid_argument("level-set advection movement is below Float32 band "
                                    "resolution; increase time or velocity");
      const float end = float(std::copysign(duration, t.timeStep));
      if (end == 0 || !std::isfinite(end))
        throw std::invalid_argument(
            "level-set advection time/displacement is below Float32 progress");
      rescale(*grid, 1.0 / dx, float(background / dx));
      grid->setTransform(workTransform);
      Field<Grid> field{velocity, *original, *workTransform, progress, maximum, dx};
      openvdb::tools::LevelSetAdvection<openvdb::FloatGrid, Field<Grid>, Progress> engine(
          *grid, field, &progress);
      engine.setSpatialScheme(openvdb::math::HJWENO5_BIAS);
      engine.setTemporalScheme(openvdb::math::TVD_RK2);
      engine.setTrackerSpatialScheme(openvdb::math::HJWENO5_BIAS);
      engine.setTrackerTemporalScheme(openvdb::math::TVD_RK2);
      engine.setNormCount(int(t.normalizationIterations));
      engine.setGrainSize(0);
      const auto count = engine.advect(0, end);
      if (count == 0)
        throw std::invalid_argument("nonzero level-set velocity/time is below the tracker progress "
                                    "threshold; rescale fields");
      return publish(grid, output, p, t, progress, original, dx, background, uint32_t(count), true,
                     "OpenVDB level-set advection");
    };
    if (other->isType<openvdb::Vec3SGrid>())
      return invoke(*openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(other));
    return invoke(*openvdb::gridConstPtrCast<openvdb::Vec3DGrid>(other));
  });
}
#endif
} // namespace

LevelSetTransportResult
WriteAdvectedLevelSet(const std::filesystem::path &input, const std::filesystem::path &velocity,
                      const std::filesystem::path &output, const LevelSetTransportOptions &t,
                      const std::string &inputGrid, const std::string &velocityGrid,
                      const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return run(input, velocity, output, t, inputGrid, velocityGrid, p, false);
#else
  (void)input;
  (void)velocity;
  (void)output;
  (void)t;
  (void)inputGrid;
  (void)velocityGrid;
  (void)p;
  unavailable();
#endif
}
LevelSetTransportResult
WriteMorphedLevelSet(const std::filesystem::path &input, const std::filesystem::path &target,
                     const std::filesystem::path &output, const LevelSetTransportOptions &t,
                     const std::string &inputGrid, const std::string &targetGrid,
                     const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return run(input, target, output, t, inputGrid, targetGrid, p, true);
#else
  (void)input;
  (void)target;
  (void)output;
  (void)t;
  (void)inputGrid;
  (void)targetGrid;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
