#include "Container/geometry/OpenVdbFieldFilter.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Filter.h>
#include <openvdb/util/Name.h>
#include <set>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB field filtering is unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr uintmax_t Fixed = 4ull * 1024 * 1024;
void add(uintmax_t &n, uintmax_t value, uintmax_t cap, const char *why) {
  if (n > cap || value > cap - n)
    throw std::invalid_argument(why);
  n += value;
}
uintmax_t product(uintmax_t a, uintmax_t b, uintmax_t cap, const char *why) {
  if (b && a > cap / b)
    throw std::invalid_argument(why);
  return a * b;
}
struct Budget {
  const ProcessingOptions &p;
  const FieldFilterOptions &f;
  uintmax_t encoded{0}, resident{0}, metadata{0}, extra{0}, slots{0}, active{0},
      work{0};
  void check(uintmax_t prospective = 0) const {
    uintmax_t total = 0;
    const auto cap = p.maxInputBytes;
    const char *why =
        "combined field filter encoded/decoded/mask/buffer/writer workspace "
        "exceeds maxInputBytes";
    add(total, Fixed, cap, why);
    add(total, encoded, cap, why);
    add(total, extra, cap, why);
    for (int n = 0; n < 8; ++n) {
      add(total, resident, cap, why);
      add(total, prospective, cap, why);
      add(total, metadata, cap, why);
    }
  }
  void meta(const openvdb::MetaMap &map) {
    for (auto it = map.beginMeta(); it != map.endMeta(); ++it) {
      if (!it->second)
        throw std::invalid_argument("null field metadata");
      add(metadata, it->first.size() + it->second->typeName().size() + 256,
          p.maxInputBytes, "field metadata exceeds maxInputBytes");
      add(metadata, it->second->size(), p.maxInputBytes,
          "field metadata exceeds maxInputBytes");
    }
    check();
  }
  void items(uintmax_t count) {
    add(work, count, f.maxWorkItems, "field filter exceeds maxWorkItems");
  }
};
void validate(const FieldFilterOptions &f, const ProcessingOptions &p,
              FieldFilterOperation operation) {
  if (!p.maxInputBytes || !p.maxVoxelCount || p.gridName.empty() ||
      p.gridName.size() > 256 || p.gridName.find('\0') != std::string::npos ||
      !f.width || f.width > 4 || !f.iterations || f.iterations > 32 ||
      !f.maxWorkItems || f.maxWorkItems > 10'000'000'000ull)
    throw std::invalid_argument(
        "invalid field filter width/iterations/work/output-name/byte/voxel "
        "limits");
  if (operation != FieldFilterOperation::Mean &&
      operation != FieldFilterOperation::Gaussian &&
      operation != FieldFilterOperation::Median &&
      operation != FieldFilterOperation::Offset)
    throw std::invalid_argument("invalid field filter operation");
  for (const auto component : f.offset) {
    if (!std::isfinite(component))
      throw std::invalid_argument("field offset must be finite");
    if (operation != FieldFilterOperation::Offset && component != 0)
      throw std::invalid_argument("offset values require the Offset operation");
  }
  if (!std::isfinite(f.maskMinimum) || !std::isfinite(f.maskMaximum) ||
      !(f.maskMinimum < f.maskMaximum))
    throw std::invalid_argument(
        "field mask requires a finite minimum smaller than maximum");
  if (f.mask.empty() && (!f.maskGridName.empty() || f.invertMask ||
                         f.maskMinimum != 0 || f.maskMaximum != 1))
    throw std::invalid_argument(
        "mask selector/range/inversion requires a mask file");
}
struct Temporary {
  std::filesystem::path directory;
  explicit Temporary(const std::filesystem::path &output) {
    static std::atomic_uint64_t serial{0};
    const auto parent = output.has_parent_path() ? output.parent_path()
                                                 : std::filesystem::path(".");
    for (unsigned n = 0; n < 64; ++n) {
      const auto candidate =
          parent /
          (".container-field-filter-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        return;
      }
    }
    throw std::runtime_error("cannot reserve field filter snapshot directory");
  }
  ~Temporary() {
    std::error_code ignored;
    std::filesystem::remove(directory / "field.vdb", ignored);
    std::filesystem::remove(directory / "mask.vdb", ignored);
    std::filesystem::remove(directory, ignored);
  }
};
void snapshot(const std::filesystem::path &input,
              const std::filesystem::path &output, uintmax_t bytes) {
  std::ifstream source(input, std::ios::binary);
  std::ofstream target(output,
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!source || !target)
    throw std::runtime_error("cannot open field filter snapshot");
  std::array<char, 64 * 1024> buffer{};
  while (bytes) {
    const auto n = std::streamsize(std::min<uintmax_t>(bytes, buffer.size()));
    source.read(buffer.data(), n);
    if (source.gcount() != n)
      throw std::runtime_error(
          "field filter input changed size during snapshot");
    target.write(buffer.data(), n);
    if (!target)
      throw std::runtime_error("field filter snapshot write failed");
    bytes -= uintmax_t(n);
  }
  target.close();
  if (!target || source.bad() || source.peek() != std::char_traits<char>::eof())
    throw std::runtime_error(
        "field filter input changed size or snapshot failed");
}
bool supported(const openvdb::GridBase &grid, bool mask) {
  return grid.isType<openvdb::FloatGrid>() ||
         grid.isType<openvdb::DoubleGrid>() ||
         (!mask && (grid.isType<openvdb::Vec3SGrid>() ||
                    grid.isType<openvdb::Vec3DGrid>()));
}
template <class Grid> uintmax_t fullMemory(const Grid &grid) {
  return openvdb::tools::memUsageIfLoaded(grid.tree(), false);
}
uintmax_t fullMemory(const openvdb::GridBase &grid) {
  if (grid.isType<openvdb::FloatGrid>())
    return fullMemory(static_cast<const openvdb::FloatGrid &>(grid));
  if (grid.isType<openvdb::DoubleGrid>())
    return fullMemory(static_cast<const openvdb::DoubleGrid &>(grid));
  if (grid.isType<openvdb::Vec3SGrid>())
    return fullMemory(static_cast<const openvdb::Vec3SGrid &>(grid));
  if (grid.isType<openvdb::Vec3DGrid>())
    return fullMemory(static_cast<const openvdb::Vec3DGrid &>(grid));
  throw std::logic_error(
      "unsupported filter tree reached full-memory accounting");
}
openvdb::GridBase::Ptr load(const std::filesystem::path &path,
                            const std::string &selection, bool mask,
                            Budget &budget) {
  detail::RequireSeekableFile(path);
  openvdb::io::File file(path.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  budget.meta(*file.getMetadata());
  std::set<std::string> descriptors;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto descriptor = it.gridName();
    add(budget.metadata, 2 * descriptor.size() + 128, budget.p.maxInputBytes,
        "filter descriptor catalogue exceeds budget");
    budget.check();
    if (!descriptors.insert(descriptor).second)
      throw std::invalid_argument(
          "field input has colliding grid descriptors; rename grids first");
  }
  std::string chosen;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto meta = file.readGridMetadata(it.gridName());
    budget.meta(*meta);
    if (!GridSelectionMatches(selection, it.gridName(), meta->getName()))
      continue;
    if (selection.empty() && !supported(*meta, mask))
      continue;
    if (found)
      throw std::invalid_argument("field grid selection is ambiguous; use "
                                  "descriptor:<exact descriptor>");
    if (!supported(*meta, mask))
      throw std::invalid_argument(
          "field/mask selection has an unsupported grid type");
    const auto memory = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto count = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!memory || !count || memory->value() < 0 || count->value() < 0 ||
        uintmax_t(count->value()) > budget.p.maxVoxelCount)
      throw std::invalid_argument("field filter requires valid recorded "
                                  "memory/voxel statistics within budget");
    budget.check(uintmax_t(memory->value()));
    chosen = it.gridName();
    found = true;
    if (selection.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument(
        "no supported field/mask grid matches selection");
  auto grid = file.readGrid(chosen);
  const auto memory = fullMemory(*grid);
  add(budget.resident, memory, budget.p.maxInputBytes,
      "fully decoded filter trees exceed maxInputBytes");
  add(budget.active, grid->activeVoxelCount(), budget.p.maxVoxelCount,
      "aggregate field/mask active voxels exceed maxVoxelCount");
  const auto slots =
      product(grid->baseTree().leafCount(), 512, budget.p.maxVoxelCount,
              "filter stored leaf slots exceed maxVoxelCount");
  add(budget.slots, slots, budget.p.maxVoxelCount,
      "aggregate field/mask stored leaf slots exceed maxVoxelCount");
  budget.check();
  grid->readNonresidentBuffers();
  if (grid->memUsage() > memory)
    throw std::runtime_error(
        "decoded filter tree exceeded its preflight estimate");
  file.close();
  return grid;
}
double map(const openvdb::GridBase &grid) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument("field filtering requires an affine transform");
  const auto &transform = *grid.transform().baseMap();
  const auto origin = transform.applyMap(openvdb::Vec3d(0));
  for (const auto c : {origin.x(), origin.y(), origin.z()})
    if (!std::isfinite(c))
      throw std::invalid_argument("field has a nonfinite affine origin");
  std::array<openvdb::Vec3d, 3> unit;
  double h = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d e(0);
    e[axis] = 1;
    const auto j = transform.applyJacobian(e),
               inverse = transform.applyInverseJacobian(e);
    for (size_t c = 0; c < 3; ++c)
      if (!std::isfinite(j[c]) || !std::isfinite(inverse[c]))
        throw std::invalid_argument(
            "field has a singular/nonfinite affine map");
    const auto length = std::hypot(j.x(), j.y(), j.z());
    if (!(length > 0) || !std::isfinite(length))
      throw std::invalid_argument("field has an invalid affine axis");
    if (axis == 0)
      h = length;
    unit[axis] = j / length;
  }
  const auto determinant = unit[0].dot(unit[1].cross(unit[2]));
  if (!std::isfinite(determinant) || determinant == 0)
    throw std::invalid_argument(
        "field affine map is singular or numerically unresolved");
  return h;
}
bool sameAffineMap(const openvdb::GridBase &a, const openvdb::GridBase &b) {
  if (!a.transform().isLinear() || !b.transform().isLinear())
    return false;
  const auto &x = *a.transform().baseMap(), &y = *b.transform().baseMap();
  const auto same = [](const openvdb::Vec3d &u, const openvdb::Vec3d &v) {
    return u.x() == v.x() && u.y() == v.y() && u.z() == v.z();
  };
  if (!same(x.applyMap(openvdb::Vec3d(0)), y.applyMap(openvdb::Vec3d(0))))
    return false;
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d e(0);
    e[axis] = 1;
    if (!same(x.applyJacobian(e), y.applyJacobian(e)))
      return false;
  }
  return true;
}
template <class Grid>
constexpr bool vectorGrid = openvdb::VecTraits<typename Grid::ValueType>::IsVec;
void structuralSemantics(const openvdb::GridBase &grid) {
  const auto recognized = [&](const char *key,
                              std::initializer_list<std::string> allowed) {
    const auto metadata = grid[key];
    if (!metadata)
      return;
    const auto text =
        std::dynamic_pointer_cast<const openvdb::StringMetadata>(metadata);
    if (!text)
      throw std::invalid_argument(
          "field class/vector metadata must have native string type");
    auto value = text->value();
    openvdb::string::trim(value);
    openvdb::string::to_lower(value);
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
      throw std::invalid_argument(
          "field has unrecognized class/vector semantic metadata");
  };
  recognized(openvdb::GridBase::META_GRID_CLASS,
             {openvdb::GridBase::gridClassToString(openvdb::GRID_UNKNOWN),
              openvdb::GridBase::gridClassToString(openvdb::GRID_LEVEL_SET),
              openvdb::GridBase::gridClassToString(openvdb::GRID_FOG_VOLUME),
              openvdb::GridBase::gridClassToString(openvdb::GRID_STAGGERED)});
  recognized(
      openvdb::GridBase::META_VECTOR_TYPE,
      {openvdb::GridBase::vecTypeToString(openvdb::VEC_INVARIANT),
       openvdb::GridBase::vecTypeToString(openvdb::VEC_COVARIANT),
       openvdb::GridBase::vecTypeToString(openvdb::VEC_COVARIANT_NORMALIZE),
       openvdb::GridBase::vecTypeToString(openvdb::VEC_CONTRAVARIANT_RELATIVE),
       openvdb::GridBase::vecTypeToString(
           openvdb::VEC_CONTRAVARIANT_ABSOLUTE)});
  if (const auto metadata = grid[openvdb::GridBase::META_IS_LOCAL_SPACE];
      metadata &&
      !std::dynamic_pointer_cast<const openvdb::BoolMetadata>(metadata))
    throw std::invalid_argument(
        "field world/local semantic metadata must have native Bool type");
}
template <class Value, class Function>
void components(const Value &value, Function function) {
  if constexpr (openvdb::VecTraits<Value>::IsVec)
    for (size_t n = 0; n < 3; ++n)
      function(value[n]);
  else
    function(value);
}
template <class Grid>
void values(const Grid &grid, FieldFilterOperation operation,
            const FieldFilterOptions &f, Budget &budget, bool post = false) {
  using Value = typename Grid::ValueType;
  using Scalar = typename openvdb::VecTraits<Value>::ElementType;
  const double count = double(2 * f.width + 1);
  const double bound = std::numeric_limits<Scalar>::max() /
                       ((operation == FieldFilterOperation::Mean ||
                         operation == FieldFilterOperation::Gaussian)
                            ? 8 * count
                            : 8.0);
  const auto check = [&](const Value &value) {
    components(value, [&](auto c) {
      if (!std::isfinite(c))
        throw std::invalid_argument(
            "field filtering requires finite stored values/background");
      if (!post && std::abs(c) > bound)
        throw std::invalid_argument(
            "field value exceeds safe native filter sum/blend range");
      if (grid.getGridClass() == openvdb::GRID_FOG_VOLUME && c < 0)
        throw std::invalid_argument("fog filtering produced/contains negative "
                                    "density; no output written");
    });
  };
  check(grid.background());
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    budget.items(1);
    check(*value);
  }
}
float normalized(double value, const FieldFilterOptions &f) {
  if (!std::isfinite(value))
    throw std::invalid_argument(
        "field mask must have finite stored values/background");
  if (value <= f.maskMinimum)
    return 0;
  if (value >= f.maskMaximum)
    return 1;
  double t;
  const auto span = f.maskMaximum - f.maskMinimum;
  if (std::isfinite(span))
    t = (value - f.maskMinimum) / span;
  else
    t = (value * .5 - f.maskMinimum * .5) /
        (f.maskMaximum * .5 - f.maskMinimum * .5);
  const float result = static_cast<float>(t);
  const float alpha = openvdb::math::SmoothUnitStep(result);
  if (!(result > 0 && result < 1 && alpha > 0 && alpha < 1))
    throw std::invalid_argument("interior mask value/blend weight is lost in "
                                "native Float precision; adjust mask range");
  return result;
}
template <class Grid>
openvdb::FloatGrid::Ptr mask(const Grid &source, const FieldFilterOptions &f,
                             Budget &budget) {
  using Tree = openvdb::FloatTree;
  budget.check(fullMemory(source));
  auto result = openvdb::FloatGrid::create(
      std::make_shared<Tree>(source.tree(), 0.0f, openvdb::TopologyCopy{}));
  result->setTransform(detail::CopyTransformExact(source.transform()));
  result->tree().root().setBackground(normalized(source.background(), f),
                                      false);
  auto input = source.cbeginValueAll();
  for (auto output = result->beginValueAll(); output; ++output, ++input) {
    budget.items(1);
    if (!input || output.getCoord() != input.getCoord() ||
        output.getLevel() != input.getLevel())
      throw std::logic_error(
          "normalized filter mask topology did not match input");
    output.setValue(normalized(double(*input), f));
  }
  if (input)
    throw std::logic_error("normalized mask lost stored entries");
  add(budget.resident, fullMemory(*result), budget.p.maxInputBytes,
      "normalized mask exceeds maxInputBytes");
  budget.check();
  return result;
}
int64_t floorDivide(int64_t value, int64_t span) {
  const auto q = value / span, r = value % span;
  return q - (r < 0);
}
uintmax_t boxes(const openvdb::CoordBBox &bbox, int64_t span, uintmax_t cap) {
  uintmax_t count = 1;
  for (size_t axis = 0; axis < 3; ++axis) {
    const auto n = uintmax_t(floorDivide(bbox.max()[axis], span) -
                             floorDivide(bbox.min()[axis], span) + 1);
    count = product(count, n, cap,
                    "prospective filter topology exceeds voxel/memory budget");
  }
  return count;
}
template <class Grid> void expansion(const Grid &grid, Budget &budget) {
  if (!grid.tree().hasActiveTiles())
    return;
  const auto bbox = grid.evalActiveVoxelBoundingBox();
  if (bbox.empty())
    return;
  using Leaf = typename Grid::TreeType::LeafNodeType;
  using Upper = typename Grid::TreeType::RootNodeType::ChildNodeType;
  using Lower = typename Upper::ChildNodeType;
  const auto leaves = boxes(bbox, 8, budget.p.maxVoxelCount / 512);
  uintmax_t slots = product(leaves, 512, budget.p.maxVoxelCount,
                            "voxelized active tiles exceed maxVoxelCount");
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
    bool outside = false;
    for (size_t axis = 0; axis < 3; ++axis)
      outside |= floorDivide(leaf->origin()[axis], 8) <
                     floorDivide(bbox.min()[axis], 8) ||
                 floorDivide(leaf->origin()[axis], 8) >
                     floorDivide(bbox.max()[axis], 8);
    if (outside)
      add(slots, 512, budget.p.maxVoxelCount,
          "voxelized tiles plus remote inactive leaves exceed maxVoxelCount");
  }
  const auto other = budget.slots - grid.tree().leafCount() * 512;
  add(slots, other, budget.p.maxVoxelCount,
      "voxelized field plus retained mask slots exceed maxVoxelCount");
  uintmax_t memory = 0;
  add(memory,
      product(leaves,
              sizeof(Leaf) + 512 * sizeof(typename Grid::ValueType) + 64,
              budget.p.maxInputBytes, "voxelized leaf memory exceeds budget"),
      budget.p.maxInputBytes, "voxelized tree exceeds budget");
  add(memory,
      product(boxes(bbox, 128, budget.p.maxInputBytes), sizeof(Lower) + 64,
              budget.p.maxInputBytes, "voxelized lower nodes exceed budget"),
      budget.p.maxInputBytes, "voxelized tree exceeds budget");
  add(memory,
      product(boxes(bbox, 4096, budget.p.maxInputBytes), sizeof(Upper) + 128,
              budget.p.maxInputBytes, "voxelized upper nodes exceed budget"),
      budget.p.maxInputBytes, "voxelized tree exceeds budget");
  for (unsigned n = 0; n < 4; ++n)
    add(budget.extra, memory, budget.p.maxInputBytes,
        "voxelization/buffer scratch exceeds budget");
  budget.check();
}
template <class Grid>
typename Grid::ValueType offset(const FieldFilterOptions &f) {
  using Value = typename Grid::ValueType;
  using Scalar = typename openvdb::VecTraits<Value>::ElementType;
  Value result = openvdb::zeroVal<Value>();
  for (size_t axis = 0; axis < (vectorGrid<Grid> ? 3u : 1u); ++axis) {
    const auto v = static_cast<Scalar>(f.offset[axis]);
    if (!std::isfinite(v) || (f.offset[axis] != 0 && v == 0) ||
        std::abs(v) > std::numeric_limits<Scalar>::max() / (8.0 * f.iterations))
      throw std::invalid_argument(
          "offset exceeds representable/safe native filter range");
    if constexpr (vectorGrid<Grid>)
      result[axis] = v;
    else
      result = v;
  }
  if constexpr (!vectorGrid<Grid>)
    if (f.offset[1] != 0 || f.offset[2] != 0)
      throw std::invalid_argument("scalar offset requires y=z=0");
  return result;
}
template <class Grid>
FieldFilterResult run(const Grid &source, const openvdb::FloatGrid::Ptr &alpha,
                      const std::filesystem::path &output,
                      FieldFilterOperation operation,
                      const FieldFilterOptions &f, Budget &budget) {
  structuralSemantics(source);
  if (source.getGridClass() == openvdb::GRID_LEVEL_SET ||
      source.getGridClass() == openvdb::GRID_STAGGERED)
    throw std::invalid_argument("ordinary filters reject SDF/staggered fields; "
                                "use tracked SDF or MAC operators");
  if (!source.isInWorldSpace() ||
      (!vectorGrid<Grid> && source.getVectorType() != openvdb::VEC_INVARIANT) ||
      (vectorGrid<Grid> &&
       (source.getGridClass() != openvdb::GRID_UNKNOWN ||
        source.getVectorType() == openvdb::VEC_COVARIANT_NORMALIZE)) ||
      (!vectorGrid<Grid> && source.getGridClass() != openvdb::GRID_UNKNOWN &&
       source.getGridClass() != openvdb::GRID_FOG_VOLUME))
    throw std::invalid_argument(
        "ordinary field filter rejects unclear class/vector semantics");
  const auto h = map(source);
  const auto bbox = source.evalActiveVoxelBoundingBox();
  if (!bbox.empty())
    for (size_t axis = 0; axis < 3; ++axis)
      if (int64_t(bbox.min()[axis]) < int64_t(INT32_MIN) + f.width + 1 ||
          int64_t(bbox.max()[axis]) > int64_t(INT32_MAX) - f.width - 1)
        throw std::invalid_argument(
            "active filter stencil exceeds safe OpenVDB coordinate range");
  values(source, operation, f, budget);
  const auto delta = offset<Grid>(f);
  const uint32_t passes = (operation == FieldFilterOperation::Mean       ? 3
                           : operation == FieldFilterOperation::Gaussian ? 12
                                                                         : 1) *
                          f.iterations;
  uintmax_t perCell =
      operation == FieldFilterOperation::Offset ? 1 : 2 * f.width + 1;
  if (operation == FieldFilterOperation::Median)
    perCell *= perCell * perCell;
  const auto work =
      product(product(source.activeVoxelCount(), perCell, f.maxWorkItems,
                      "filter kernel work exceeds maxWorkItems"),
              passes, f.maxWorkItems, "filter iterations exceed maxWorkItems");
  budget.items(work);
  if (f.voxelizeActiveTiles)
    expansion(source, budget);
  budget.check();
  auto result = detail::DeepCopyGridExact(source);
  FieldFilterResult report;
  report.inputActiveVoxelCount = source.activeVoxelCount();
  report.activeTileCount = source.tree().activeTileCount();
  if (f.voxelizeActiveTiles && source.tree().hasActiveTiles()) {
    result->tree().voxelizeActiveTiles(false);
    report.voxelizedTileCount = report.activeTileCount;
  }
  openvdb::tools::Filter<Grid, openvdb::FloatGrid> filter(*result);
  filter.setGrainSize(0);
  filter.setProcessTiles(false);
  filter.setMaskRange(0, 1);
  filter.invertMask(f.invertMask);
  for (uint32_t iteration = 0; iteration < f.iterations; ++iteration) {
    budget.check();
    switch (operation) {
    case FieldFilterOperation::Mean:
      filter.mean(int(f.width), 1, alpha.get());
      break;
    case FieldFilterOperation::Gaussian:
      filter.gaussian(int(f.width), 1, alpha.get());
      break;
    case FieldFilterOperation::Median:
      filter.median(int(f.width), 1, alpha.get());
      break;
    case FieldFilterOperation::Offset:
      filter.offset(delta, alpha.get());
      break;
    }
    values(*result, operation, f, budget, true);
  }
  result->setName(budget.p.gridName);
  detail::RemoveBoundaryProjectionMetadata(*result);
  result->setSaveFloatAsHalf(false);
  report.grid = {result->getName(),
                 result->getGridClass() == openvdb::GRID_FOG_VOLUME
                     ? "fog volume"
                     : "unknown",
                 result->activeVoxelCount(), h};
  report.storedLeafCount = result->tree().leafCount();
  report.kernelPasses = passes;
  report.maskUsed = bool(alpha);
  if (operation == FieldFilterOperation::Gaussian)
    report.diagnostics.emplace_back(
        "Gaussian uses four native box iterations per iteration; mean "
        "reciprocal weights are Float precision");
  else if (operation == FieldFilterOperation::Mean)
    report.diagnostics.emplace_back(
        "native mean reciprocal weights use Float precision");
  if constexpr (vectorGrid<Grid>)
    if (operation == FieldFilterOperation::Median)
      report.diagnostics.emplace_back("vector median uses native lexicographic "
                                      "X,Y,Z order, not componentwise median");
  if (alpha)
    report.diagnostics.emplace_back(
        "SmoothStep Float alpha is applied at each native separable axis pass");
  detail::WriteFileExclusive(output, {result}, budget.p.maxInputBytes,
                             openvdb::io::COMPRESS_ZIP);
  return report;
}
#endif
} // namespace

FieldFilterResult WriteFilteredField(const std::filesystem::path &input,
                                     const std::filesystem::path &output,
                                     FieldFilterOperation operation,
                                     const FieldFilterOptions &f,
                                     const std::string &inputGrid,
                                     const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(f, p, operation);
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "filtered output must be a new path in an existing directory");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded filtering requires delayed loading; "
                                "unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded filtering requires delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p, f};
    const auto bytes = [&](const std::filesystem::path &path) {
      if (!std::filesystem::is_regular_file(path))
        throw std::invalid_argument("filter input/mask must be a regular file");
      const auto size = std::filesystem::file_size(path);
      add(budget.encoded, size, p.maxInputBytes,
          "combined field/mask encoded bytes exceed maxInputBytes");
      return size;
    };
    const auto fieldBytes = bytes(input);
    const auto maskBytes = f.mask.empty() ? 0 : bytes(f.mask);
    budget.check();
    Temporary temporary(output);
    snapshot(input, temporary.directory / "field.vdb", fieldBytes);
    auto source =
        load(temporary.directory / "field.vdb", inputGrid, false, budget);
    std::shared_ptr<openvdb::FloatGrid> alpha;
    if (!f.mask.empty()) {
      snapshot(f.mask, temporary.directory / "mask.vdb", maskBytes);
      auto rawMask =
          load(temporary.directory / "mask.vdb", f.maskGridName, true, budget);
      (void)map(*rawMask);
      if (!sameAffineMap(*rawMask, *source))
        throw std::invalid_argument("field mask requires exactly matching "
                                    "transforms; resample the mask first");
      if (rawMask->isType<openvdb::FloatGrid>())
        alpha =
            mask(*openvdb::gridPtrCast<openvdb::FloatGrid>(rawMask), f, budget);
      else
        alpha = mask(*openvdb::gridPtrCast<openvdb::DoubleGrid>(rawMask), f,
                     budget);
      // Transform::operator== uses upstream tolerances. Exact coefficients
      // were checked above; sharing this representation guarantees the native
      // DualGridSampler takes its aligned integer-coordinate path.
      alpha->setTransform(detail::CopyTransformExact(source->transform()));
    }
    if (source->isType<openvdb::FloatGrid>())
      return run(*openvdb::gridPtrCast<openvdb::FloatGrid>(source), alpha,
                 output, operation, f, budget);
    if (source->isType<openvdb::DoubleGrid>())
      return run(*openvdb::gridPtrCast<openvdb::DoubleGrid>(source), alpha,
                 output, operation, f, budget);
    if (source->isType<openvdb::Vec3SGrid>())
      return run(*openvdb::gridPtrCast<openvdb::Vec3SGrid>(source), alpha,
                 output, operation, f, budget);
    return run(*openvdb::gridPtrCast<openvdb::Vec3DGrid>(source), alpha, output,
               operation, f, budget);
  });
#endif
#else
  (void)input;
  (void)output;
  (void)operation;
  (void)f;
  (void)inputGrid;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
