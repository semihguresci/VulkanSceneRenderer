#include "Container/geometry/OpenVdbFieldComposite.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/ValueTransformer.h>
#include <openvdb/util/Name.h>
#include <set>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB field compositing is unavailable; "
                           "configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr uintmax_t Fixed = 4ull * 1024 * 1024;
void add(uintmax_t &n, uintmax_t x, uintmax_t cap, const char *why) {
  if (n > cap || x > cap - n)
    throw std::invalid_argument(why);
  n += x;
}
struct Budget {
  const ProcessingOptions &p;
  uint64_t workCap;
  uintmax_t encoded{0}, resident{0}, metadata{0}, slots{0}, active{0}, work{0};
  void check(uintmax_t prospective = 0) const {
    uintmax_t total = 0;
    const auto why =
        "combined field composite encoded/decoded/union/clone/writer workspace "
        "exceeds maxInputBytes";
    add(total, Fixed, p.maxInputBytes, why);
    add(total, encoded, p.maxInputBytes, why);
    for (unsigned n = 0; n < 16; ++n) {
      add(total, resident, p.maxInputBytes, why);
      add(total, prospective, p.maxInputBytes, why);
    }
    for (unsigned n = 0; n < 8; ++n)
      add(total, metadata, p.maxInputBytes, why);
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
  void item() {
    add(work, 1, workCap, "field composite/transform exceeds maxWorkItems");
  }
};
void validate(const ProcessingOptions &p, uint64_t work) {
  if (!p.maxInputBytes || !p.maxVoxelCount || !work ||
      work > 10'000'000'000ull || p.gridName.empty() ||
      p.gridName.size() > 256 || p.gridName.find('\0') != std::string::npos)
    throw std::invalid_argument(
        "invalid field composite byte/voxel/work/output-name limits");
}
void validate(const FieldCompositeOptions &o, const ProcessingOptions &p,
              FieldCompositeOperation operation) {
  validate(p, o.maxWorkItems);
  if (!std::isfinite(o.blendWeight) || o.blendWeight < 0 || o.blendWeight > 1)
    throw std::invalid_argument("field blend weight must be finite in [0,1]");
  if (operation != FieldCompositeOperation::Sum &&
      operation != FieldCompositeOperation::Product &&
      operation != FieldCompositeOperation::Divide &&
      operation != FieldCompositeOperation::Minimum &&
      operation != FieldCompositeOperation::Maximum &&
      operation != FieldCompositeOperation::Blend &&
      operation != FieldCompositeOperation::MergeActive)
    throw std::invalid_argument("invalid field composite operation");
  if (o.activity != FieldActivity::Union &&
      o.activity != FieldActivity::Intersection &&
      o.activity != FieldActivity::Left && o.activity != FieldActivity::Right)
    throw std::invalid_argument("invalid composite activity policy");
  if (o.mergePreference != FieldMergePreference::Left &&
      o.mergePreference != FieldMergePreference::Right)
    throw std::invalid_argument("invalid active merge preference");
  if (o.vectorExtrema != FieldVectorExtrema::Componentwise &&
      o.vectorExtrema != FieldVectorExtrema::ScaledMagnitude)
    throw std::invalid_argument("invalid vector extrema policy");
  if (operation == FieldCompositeOperation::MergeActive &&
      o.activity != FieldActivity::Union)
    throw std::invalid_argument("MergeActive requires Union activity");
}
constexpr std::array<double, 3> Scale{1, 1, 1}, Offset{};
constexpr std::array<double, 3> Minimum{std::numeric_limits<double>::lowest(),
                                        std::numeric_limits<double>::lowest(),
                                        std::numeric_limits<double>::lowest()};
constexpr std::array<double, 3> Maximum{std::numeric_limits<double>::max(),
                                        std::numeric_limits<double>::max(),
                                        std::numeric_limits<double>::max()};
void validate(const FieldValueTransformOptions &o, const ProcessingOptions &p) {
  validate(p, o.maxWorkItems);
  if (o.operation != FieldValueOperation::Copy &&
      o.operation != FieldValueOperation::Affine &&
      o.operation != FieldValueOperation::Absolute &&
      o.operation != FieldValueOperation::Clamp)
    throw std::invalid_argument("invalid field value transform operation");
  if (o.scope != FieldValueScope::ActiveOnly &&
      o.scope != FieldValueScope::AllValues)
    throw std::invalid_argument("invalid field value transform scope");
  for (size_t axis = 0; axis < 3; ++axis)
    if (!std::isfinite(o.scale[axis]) || !std::isfinite(o.offset[axis]) ||
        !std::isfinite(o.minimum[axis]) || !std::isfinite(o.maximum[axis]) ||
        o.minimum[axis] > o.maximum[axis])
      throw std::invalid_argument(
          "value transform parameters must be finite with minimum<=maximum");
  if (o.operation != FieldValueOperation::Affine &&
      (o.scale != Scale || o.offset != Offset))
    throw std::invalid_argument(
        "scale/offset options require Affine operation");
  if (o.operation != FieldValueOperation::Clamp &&
      (o.minimum != Minimum || o.maximum != Maximum))
    throw std::invalid_argument("clamp bounds require Clamp operation");
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
          (".container-field-composite-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        return;
      }
    }
    throw std::runtime_error(
        "cannot reserve field composite snapshot directory");
  }
  ~Temporary() {
    std::error_code ignored;
    std::filesystem::remove(directory / "a.vdb", ignored);
    std::filesystem::remove(directory / "b.vdb", ignored);
    std::filesystem::remove(directory, ignored);
  }
};
void outputPath(const std::filesystem::path &path) {
  if (path.empty() || std::filesystem::exists(path) ||
      (path.has_parent_path() &&
       !std::filesystem::is_directory(path.parent_path())))
    throw std::invalid_argument(
        "field composite output must be a new path in an existing directory");
}
uintmax_t inputBytes(const std::filesystem::path &path, Budget &budget) {
  if (!std::filesystem::is_regular_file(path))
    throw std::invalid_argument("field input must be a regular file");
  const auto size = std::filesystem::file_size(path);
  add(budget.encoded, size, budget.p.maxInputBytes,
      "combined field encoded bytes exceed maxInputBytes");
  budget.check();
  return size;
}
void snapshot(const std::filesystem::path &input,
              const std::filesystem::path &output, uintmax_t size) {
  std::ifstream source(input, std::ios::binary);
  std::ofstream target(output,
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!source || !target)
    throw std::runtime_error("cannot open field composite snapshot");
  std::array<char, 64 * 1024> bytes{};
  while (size) {
    const auto count = std::streamsize(std::min<uintmax_t>(size, bytes.size()));
    source.read(bytes.data(), count);
    if (source.gcount() != count)
      throw std::runtime_error("field source changed size during snapshot");
    target.write(bytes.data(), count);
    if (!target)
      throw std::runtime_error("field snapshot write failed");
    size -= uintmax_t(count);
  }
  target.close();
  if (!target || source.bad() || source.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("field source changed size or snapshot failed");
}
bool supported(const openvdb::GridBase &g) {
  return g.isType<openvdb::FloatGrid>() || g.isType<openvdb::DoubleGrid>() ||
         g.isType<openvdb::Vec3SGrid>() || g.isType<openvdb::Vec3DGrid>();
}
template <class Grid> uintmax_t memory(const Grid &g) {
  return openvdb::tools::memUsageIfLoaded(g.tree(), false);
}
uintmax_t memory(const openvdb::GridBase &g) {
  if (g.isType<openvdb::FloatGrid>())
    return memory(static_cast<const openvdb::FloatGrid &>(g));
  if (g.isType<openvdb::DoubleGrid>())
    return memory(static_cast<const openvdb::DoubleGrid &>(g));
  if (g.isType<openvdb::Vec3SGrid>())
    return memory(static_cast<const openvdb::Vec3SGrid &>(g));
  if (g.isType<openvdb::Vec3DGrid>())
    return memory(static_cast<const openvdb::Vec3DGrid &>(g));
  throw std::logic_error("unsupported field reached memory preflight");
}
openvdb::GridBase::Ptr load(const std::filesystem::path &path,
                            const std::string &selection, Budget &budget) {
  detail::RequireSeekableFile(path);
  openvdb::io::File file(path.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  budget.meta(*file.getMetadata());
  std::set<std::string> names;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto n = it.gridName();
    add(budget.metadata, 2 * n.size() + 128, budget.p.maxInputBytes,
        "field descriptor catalog exceeds budget");
    budget.check();
    if (!names.insert(n).second)
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
    if (selection.empty() && !supported(*meta))
      continue;
    if (found)
      throw std::invalid_argument(
          "field selector is ambiguous; use descriptor:<exact descriptor>");
    if (!supported(*meta))
      throw std::invalid_argument(
          "field composite requires Float/Double/Vec3S/Vec3D grids");
    const auto mem = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto count = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!mem || !count || mem->value() < 0 || count->value() < 0 ||
        uintmax_t(count->value()) > budget.p.maxVoxelCount)
      throw std::invalid_argument("field composite requires valid recorded "
                                  "memory/voxel statistics within budget");
    budget.check(uintmax_t(mem->value()));
    chosen = it.gridName();
    found = true;
    if (selection.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument("no supported field matches selector");
  auto result = file.readGrid(chosen);
  const auto bytes = memory(*result);
  add(budget.resident, bytes, budget.p.maxInputBytes,
      "fully decoded field trees exceed byte budget");
  add(budget.active, result->activeVoxelCount(), budget.p.maxVoxelCount,
      "aggregate field active voxels exceed maxVoxelCount");
  const auto leaves = result->baseTree().leafCount();
  if (leaves > budget.p.maxVoxelCount / 512)
    throw std::invalid_argument("stored field leaf slots exceed maxVoxelCount");
  add(budget.slots, leaves * 512, budget.p.maxVoxelCount,
      "aggregate stored field leaf slots exceed maxVoxelCount");
  budget.check();
  result->readNonresidentBuffers();
  if (result->memUsage() > bytes)
    throw std::runtime_error("decoded field exceeded preflight estimate");
  file.close();
  return result;
}
void tags(const openvdb::GridBase &grid) {
  const auto known = [&](const char *key,
                         std::initializer_list<std::string> allowed) {
    const auto m = grid[key];
    if (!m)
      return;
    const auto text =
        std::dynamic_pointer_cast<const openvdb::StringMetadata>(m);
    if (!text)
      throw std::invalid_argument(
          "field class/vector tag must have native String type");
    auto value = text->value();
    openvdb::string::trim(value);
    openvdb::string::to_lower(value);
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
      throw std::invalid_argument("unrecognized field class/vector semantics");
  };
  known(openvdb::GridBase::META_GRID_CLASS,
        {"unknown", "level set", "fog volume", "staggered"});
  known(openvdb::GridBase::META_VECTOR_TYPE,
        {"invariant", "covariant", "covariant normalize",
         "contravariant relative", "contravariant absolute"});
  if (const auto m = grid[openvdb::GridBase::META_IS_LOCAL_SPACE];
      m && !std::dynamic_pointer_cast<const openvdb::BoolMetadata>(m))
    throw std::invalid_argument(
        "field local/world tag must have native Bool type");
  if (!grid.isInWorldSpace() ||
      grid.getGridClass() == openvdb::GRID_LEVEL_SET ||
      grid.getGridClass() == openvdb::GRID_STAGGERED)
    throw std::invalid_argument(
        "ordinary field composite rejects SDF/staggered/index-space semantics; "
        "use dedicated operators");
  const bool vector =
      grid.isType<openvdb::Vec3SGrid>() || grid.isType<openvdb::Vec3DGrid>();
  if ((vector && (grid.getGridClass() != openvdb::GRID_UNKNOWN ||
                  grid.getVectorType() == openvdb::VEC_COVARIANT_NORMALIZE)) ||
      (!vector && grid.getVectorType() != openvdb::VEC_INVARIANT))
    throw std::invalid_argument(
        "unsupported ordinary field class/vector semantics; use file authoring "
        "for raw passthrough");
}
double affineMap(const openvdb::GridBase &grid) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument("field composite requires affine transforms");
  const auto &map = *grid.transform().baseMap();
  const auto origin = map.applyMap(openvdb::Vec3d(0));
  for (size_t n = 0; n < 3; ++n)
    if (!std::isfinite(origin[n]))
      throw std::invalid_argument("nonfinite affine field origin");
  std::array<openvdb::Vec3d, 3> unit;
  double first = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d e(0);
    e[axis] = 1;
    const auto j = map.applyJacobian(e), inverse = map.applyInverseJacobian(e);
    for (size_t c = 0; c < 3; ++c)
      if (!std::isfinite(j[c]) || !std::isfinite(inverse[c]))
        throw std::invalid_argument("nonfinite/singular affine field map");
    const double h = std::hypot(j.x(), j.y(), j.z());
    if (!(h > 0) || !std::isfinite(h))
      throw std::invalid_argument("invalid affine field axis");
    unit[axis] = j / h;
    if (!axis)
      first = h;
  }
  const auto determinant = unit[0].dot(unit[1].cross(unit[2]));
  if (!std::isfinite(determinant) || determinant == 0)
    throw std::invalid_argument(
        "affine field map is singular or numerically unresolved");
  return first;
}
void compatible(const openvdb::GridBase &a, const openvdb::GridBase &b) {
  if (a.type() != b.type() || a.getGridClass() != b.getGridClass() ||
      a.getVectorType() != b.getVectorType() ||
      a.isInWorldSpace() != b.isInWorldSpace())
    throw std::invalid_argument("field composites require identical native "
                                "type/class/vector/world semantics");
  const auto &x = *a.transform().baseMap(), &y = *b.transform().baseMap();
  const auto equal = [](const auto &u, const auto &v) {
    return u.x() == v.x() && u.y() == v.y() && u.z() == v.z();
  };
  if (!equal(x.applyMap(openvdb::Vec3d(0)), y.applyMap(openvdb::Vec3d(0))))
    throw std::invalid_argument(
        "field affine transforms must match exactly; resample explicitly");
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d e(0);
    e[axis] = 1;
    if (!equal(x.applyJacobian(e), y.applyJacobian(e)))
      throw std::invalid_argument(
          "field affine transforms must match exactly; resample explicitly");
  }
}
template <class Value>
constexpr bool vectorValue = openvdb::VecTraits<Value>::IsVec;
template <class Value, class Function>
void components(const Value &value, Function function) {
  if constexpr (vectorValue<Value>)
    for (size_t n = 0; n < 3; ++n)
      function(value[n]);
  else
    function(value);
}
template <class Value> void finite(const Value &value, bool fog = false) {
  components(value, [&](auto c) {
    if (!std::isfinite(c))
      throw std::invalid_argument(
          "field arithmetic requires finite operated values/background");
    if (fog && c < 0)
      throw std::invalid_argument("field operation contains/produces negative "
                                  "fog density; no output written");
  });
}
template <class Grid>
void numericalInput(const Grid &grid, Budget &budget, bool all,
                    bool denominator = false) {
  const bool fog = grid.getGridClass() == openvdb::GRID_FOG_VOLUME;
  const auto validateValue = [&](const auto &value) {
    finite(value, fog);
    if (denominator)
      components(value, [](auto component) {
        if (component == 0)
          throw std::invalid_argument(
              "field division requires nonzero denominator components "
              "everywhere, including inactive values and background");
      });
  };
  if (all) {
    validateValue(grid.background());
    for (auto v = grid.cbeginValueAll(); v; ++v) {
      budget.item();
      validateValue(*v);
    }
  } else
    for (auto v = grid.cbeginValueOn(); v; ++v) {
      budget.item();
      validateValue(*v);
    }
}
template <class T> bool sameBits(T a, T b) {
  if constexpr (std::is_same_v<T, float>)
    return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
  else
    return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}
template <class Scalar> Scalar represent(double value) {
  const auto converted = static_cast<Scalar>(value);
  if (!std::isfinite(converted))
    throw std::invalid_argument(
        "field arithmetic overflow; use a wider native type or smaller values");
  if (value != 0 && converted == 0)
    throw std::invalid_argument("nonzero field result is lost to native "
                                "numeric range; use a wider type");
  return converted;
}
struct Term {
  double a{0}, b{0};
  int exponent{0};
  bool zero{true};
};
Term term(double a, double b) {
  if (a == 0 || b == 0)
    return {};
  int ea = 0, eb = 0;
  const auto ma = std::frexp(a, &ea), mb = std::frexp(b, &eb);
  return {ma, mb, ea + eb, false};
}
// Compute a*scale+b*bScale with normalized products and their FMA remainders.
// Physical exponents are restored only AFTER addition, retaining products that
// individually underflow/overflow but whose final result is representable.
double linear(double a, double scale, double b, double bScale) {
  const auto x = term(a, scale), y = term(b, bScale);
  if (x.zero && y.zero)
    return a * scale + b * bScale;
  const int exponent = x.zero   ? y.exponent
                       : y.zero ? x.exponent
                                : std::max(x.exponent, y.exponent);
  const double ax = x.zero ? 0 : std::ldexp(x.a, x.exponent - exponent),
               ay = y.zero ? 0 : std::ldexp(y.a, y.exponent - exponent);
  const double px = ax * x.b, py = ay * y.b;
  const double lowx = std::fma(ax, x.b, -px), lowy = std::fma(ay, y.b, -py);
  const double sum = px + py, z = sum - px;
  const double correction = ((px - (sum - z)) + (py - z)) + lowx + lowy;
  const double normalized = sum + correction;
  const double result = std::ldexp(normalized, exponent);
  if (!std::isfinite(result))
    throw std::invalid_argument(
        "field arithmetic overflow after scaled accumulation");
  if (normalized != 0 && result == 0)
    throw std::invalid_argument(
        "nonzero field result is below Double numeric range");
  return result;
}
bool activity(bool a, bool b, FieldActivity policy) {
  switch (policy) {
  case FieldActivity::Union:
    return a || b;
  case FieldActivity::Intersection:
    return a && b;
  case FieldActivity::Left:
    return a;
  case FieldActivity::Right:
    return b;
  }
  throw std::logic_error("invalid activity reached composite kernel");
}
template <class Value> int rank(const Value &a, const Value &b) {
  double scale = 0;
  for (size_t n = 0; n < 3; ++n)
    scale = std::max({scale, std::abs(double(a[n])), std::abs(double(b[n]))});
  if (scale != 0) {
    const auto x = std::hypot(double(a[0]) / scale, double(a[1]) / scale,
                              double(a[2]) / scale);
    const auto y = std::hypot(double(b[0]) / scale, double(b[1]) / scale,
                              double(b[2]) / scale);
    if (x < y)
      return -1;
    if (x > y)
      return 1;
  }
  for (size_t n = 0; n < 3; ++n) {
    if (a[n] < b[n])
      return -1;
    if (a[n] > b[n])
      return 1;
  }
  return 0;
}
template <class Scalar>
Scalar combineScalar(Scalar a, Scalar b, FieldCompositeOperation op,
                     const FieldCompositeOptions &o) {
  switch (op) {
  case FieldCompositeOperation::Sum:
    return represent<Scalar>(linear(a, 1, b, 1));
  case FieldCompositeOperation::Product:
    if (a == 0 || b == 0)
      return a * b;
    return represent<Scalar>(linear(a, b, 0, 0));
  case FieldCompositeOperation::Divide: {
    if (b == 0)
      throw std::invalid_argument("field division by zero is unsupported");
    const double quotient = double(a) / double(b);
    if (a != 0 && quotient == 0)
      throw std::invalid_argument(
          "nonzero field quotient is below Double numeric range");
    return represent<Scalar>(quotient);
  }
  case FieldCompositeOperation::Minimum:
    return b < a ? b : a;
  case FieldCompositeOperation::Maximum:
    return a < b ? b : a;
  case FieldCompositeOperation::Blend:
    if (o.blendWeight == 0 || sameBits(a, b))
      return a;
    if (o.blendWeight == 1)
      return b;
    return represent<Scalar>(linear(a, 1 - o.blendWeight, b, o.blendWeight));
  case FieldCompositeOperation::MergeActive:
    break;
  }
  throw std::logic_error("merge reached numerical scalar kernel");
}
template <class Value>
Value combineValue(const Value &a, const Value &b, FieldCompositeOperation op,
                   const FieldCompositeOptions &o) {
  if constexpr (vectorValue<Value>) {
    if ((op == FieldCompositeOperation::Minimum ||
         op == FieldCompositeOperation::Maximum) &&
        o.vectorExtrema == FieldVectorExtrema::ScaledMagnitude) {
      const auto order = rank(a, b);
      return op == FieldCompositeOperation::Minimum ? (order <= 0 ? a : b)
                                                    : (order >= 0 ? a : b);
    }
    Value result;
    for (size_t n = 0; n < 3; ++n)
      result[n] = combineScalar(a[n], b[n], op, o);
    return result;
  } else
    return combineScalar(a, b, op, o);
}
template <class Grid>
FieldCompositeResult
binary(const Grid &a, const Grid &b, const std::filesystem::path &output,
       FieldCompositeOperation operation, const FieldCompositeOptions &o,
       Budget &budget, double h) {
  using Value = typename Grid::ValueType;
  const bool copying = operation == FieldCompositeOperation::MergeActive;
  if (!copying) {
    numericalInput(a, budget, true);
    numericalInput(b, budget, true,
                   operation == FieldCompositeOperation::Divide);
  }
  budget.check();
  auto result = detail::CopyGridWithNewTreeExact(a);
  result->setTree(std::make_shared<typename Grid::TreeType>(a.background()));
  const bool fog = a.getGridClass() == openvdb::GRID_FOG_VOLUME;
  auto kernel = [&](openvdb::CombineArgs<Value> &args) {
    budget.item();
    if (copying) {
      if (o.mergePreference == FieldMergePreference::Left)
        args.setResult(args.aIsActive()   ? args.a()
                       : args.bIsActive() ? args.b()
                                          : args.a());
      else
        args.setResult(args.bIsActive() ? args.b() : args.a());
      args.setResultIsActive(args.aIsActive() || args.bIsActive());
    } else {
      const auto value = combineValue(args.a(), args.b(), operation, o);
      finite(value, fog);
      args.setResult(value);
      args.setResultIsActive(
          activity(args.aIsActive(), args.bIsActive(), o.activity));
    }
  };
  // New immutable output, no operand stealing and no pruning/canonicalization.
  result->tree().combine2Extended(a.tree(), b.tree(), kernel, false);
  if (copying)
    result->tree().root().setBackground(a.background(), false);
  detail::RemoveBoundaryProjectionMetadata(*result);
  result->setName(budget.p.gridName);
  result->setSaveFloatAsHalf(false);
  FieldCompositeResult report;
  report.grid = {result->getName(),
                 openvdb::GridBase::gridClassToString(result->getGridClass()),
                 result->activeVoxelCount(), h};
  report.inputActiveVoxelCountA = a.activeVoxelCount();
  report.inputActiveVoxelCountB = b.activeVoxelCount();
  report.storedLeafCount = result->tree().leafCount();
  report.workItems = budget.work;
  report.diagnostics.emplace_back(
      "source A metadata retained; source B metadata is not merged");
  if (copying)
    report.diagnostics.emplace_back(
        "MergeActive ignores B inactive values and preserves A background; "
        "structural topology is unioned");
  if constexpr (vectorValue<Value>)
    if (o.vectorExtrema == FieldVectorExtrema::ScaledMagnitude &&
        (operation == FieldCompositeOperation::Minimum ||
         operation == FieldCompositeOperation::Maximum))
      report.diagnostics.emplace_back(
          "whole-vector extrema use rounded scaled Double magnitude rank with "
          "lexicographic ties");
  budget.check();
  detail::WriteFileExclusive(output, {result}, budget.p.maxInputBytes,
                             openvdb::io::COMPRESS_ZIP);
  return report;
}
template <class Scalar>
Scalar transformScalar(Scalar value, size_t axis,
                       const FieldValueTransformOptions &o) {
  switch (o.operation) {
  case FieldValueOperation::Copy:
    return value;
  case FieldValueOperation::Absolute:
    return std::abs(value);
  case FieldValueOperation::Affine:
    if (o.scale[axis] == 1 && o.offset[axis] == 0)
      return value;
    if (o.scale[axis] == 0)
      return represent<Scalar>(o.offset[axis]);
    return represent<Scalar>(linear(value, o.scale[axis], o.offset[axis], 1));
  case FieldValueOperation::Clamp: {
    if (double(value) >= o.minimum[axis] && double(value) <= o.maximum[axis])
      return value;
    const bool low = double(value) < o.minimum[axis];
    const auto bound = low ? o.minimum[axis] : o.maximum[axis];
    auto converted = represent<Scalar>(bound);
    if (low && double(converted) < bound)
      converted =
          std::nextafter(converted, std::numeric_limits<Scalar>::infinity());
    if (!low && double(converted) > bound)
      converted =
          std::nextafter(converted, -std::numeric_limits<Scalar>::infinity());
    if (!std::isfinite(converted) || double(converted) < o.minimum[axis] ||
        double(converted) > o.maximum[axis])
      throw std::invalid_argument(
          "clamp interval has no representable native result");
    return converted;
  }
  }
  throw std::logic_error("invalid value operation reached kernel");
}
template <class Value>
Value transformed(const Value &value, const FieldValueTransformOptions &o) {
  if constexpr (vectorValue<Value>) {
    Value result;
    for (size_t n = 0; n < 3; ++n)
      result[n] = transformScalar(value[n], n, o);
    return result;
  } else
    return transformScalar(value, 0, o);
}
template <class Grid>
FieldCompositeResult
unary(const Grid &source, const std::filesystem::path &output,
      const FieldValueTransformOptions &o, Budget &budget, double h) {
  using Value = typename Grid::ValueType;
  if constexpr (!vectorValue<Value>)
    for (size_t n = 1; n < 3; ++n)
      if (o.scale[n] != 1 || o.offset[n] != 0 || o.minimum[n] != Minimum[n] ||
          o.maximum[n] != Maximum[n])
        throw std::invalid_argument(
            "scalar transforms require unused Y/Z parameter defaults");
  const bool copy = o.operation == FieldValueOperation::Copy,
             all = o.scope == FieldValueScope::AllValues;
  if (!copy)
    numericalInput(source, budget, all);
  budget.check();
  auto result = detail::DeepCopyGridExact(source);
  const bool fog = source.getGridClass() == openvdb::GRID_FOG_VOLUME;
  if (!copy) {
    auto kernel = [&](const auto &iter) {
      budget.item();
      const auto value = transformed(*iter, o);
      finite(value, fog);
      iter.setValue(value);
    };
    if (all) {
      openvdb::tools::foreach (result->beginValueAll(), kernel, false);
      const auto background = transformed(source.background(), o);
      finite(background, fog);
      result->tree().root().setBackground(background, false);
    } else
      openvdb::tools::foreach (result->beginValueOn(), kernel, false);
  } else
    for (auto value = source.cbeginValueAll(); value; ++value)
      budget.item();
  if(!copy)detail::RemoveBoundaryProjectionMetadata(*result);
  result->setName(budget.p.gridName);
  result->setSaveFloatAsHalf(false);
  FieldCompositeResult report;
  report.grid = {result->getName(),
                 openvdb::GridBase::gridClassToString(result->getGridClass()),
                 result->activeVoxelCount(), h};
  report.inputActiveVoxelCountA = source.activeVoxelCount();
  report.storedLeafCount = result->tree().leafCount();
  report.workItems = budget.work;
  if (!copy && all)
    report.diagnostics.emplace_back(
        "all stored inactive values and the background were transformed; "
        "activity/topology retained");
  budget.check();
  detail::WriteFileExclusive(output, {result}, budget.p.maxInputBytes,
                             openvdb::io::COMPRESS_ZIP);
  return report;
}
void initialize() {
#ifndef OPENVDB_USE_ZLIB
  throw std::invalid_argument("bounded field compositing requires an OpenVDB "
                              "build with ZIP/Zlib compression");
#endif
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded field compositing requires delayed "
                                "loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
}
#endif
} // namespace

FieldCompositeResult WriteCompositeField(
    const std::filesystem::path &inputA, const std::filesystem::path &inputB,
    const std::filesystem::path &output, FieldCompositeOperation operation,
    const FieldCompositeOptions &o, const std::string &gridA,
    const std::string &gridB, const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(o, p, operation);
  outputPath(output);
  initialize();
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded field compositing requires delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p, o.maxWorkItems};
    const auto na = inputBytes(inputA, budget), nb = inputBytes(inputB, budget);
    Temporary temporary(output);
    snapshot(inputA, temporary.directory / "a.vdb", na);
    snapshot(inputB, temporary.directory / "b.vdb", nb);
    auto a = load(temporary.directory / "a.vdb", gridA, budget),
         b = load(temporary.directory / "b.vdb", gridB, budget);
    tags(*a);
    tags(*b);
    const auto h = affineMap(*a);
    (void)affineMap(*b);
    compatible(*a, *b);
    if (a->isType<openvdb::FloatGrid>())
      return binary(*openvdb::gridPtrCast<openvdb::FloatGrid>(a),
                    *openvdb::gridPtrCast<openvdb::FloatGrid>(b), output,
                    operation, o, budget, h);
    if (a->isType<openvdb::DoubleGrid>())
      return binary(*openvdb::gridPtrCast<openvdb::DoubleGrid>(a),
                    *openvdb::gridPtrCast<openvdb::DoubleGrid>(b), output,
                    operation, o, budget, h);
    if (a->isType<openvdb::Vec3SGrid>())
      return binary(*openvdb::gridPtrCast<openvdb::Vec3SGrid>(a),
                    *openvdb::gridPtrCast<openvdb::Vec3SGrid>(b), output,
                    operation, o, budget, h);
    return binary(*openvdb::gridPtrCast<openvdb::Vec3DGrid>(a),
                  *openvdb::gridPtrCast<openvdb::Vec3DGrid>(b), output,
                  operation, o, budget, h);
  });
#endif
#else
  (void)inputA;
  (void)inputB;
  (void)output;
  (void)operation;
  (void)o;
  (void)gridA;
  (void)gridB;
  (void)p;
  unavailable();
#endif
}
FieldCompositeResult WriteTransformedField(const std::filesystem::path &input,
                                           const std::filesystem::path &output,
                                           const FieldValueTransformOptions &o,
                                           const std::string &inputGrid,
                                           const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(o, p);
  outputPath(output);
  initialize();
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded field transforms require delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p, o.maxWorkItems};
    const auto size = inputBytes(input, budget);
    Temporary temporary(output);
    snapshot(input, temporary.directory / "a.vdb", size);
    auto source = load(temporary.directory / "a.vdb", inputGrid, budget);
    tags(*source);
    const auto h = affineMap(*source);
    if (source->isType<openvdb::FloatGrid>())
      return unary(*openvdb::gridPtrCast<openvdb::FloatGrid>(source), output, o,
                   budget, h);
    if (source->isType<openvdb::DoubleGrid>())
      return unary(*openvdb::gridPtrCast<openvdb::DoubleGrid>(source), output,
                   o, budget, h);
    if (source->isType<openvdb::Vec3SGrid>())
      return unary(*openvdb::gridPtrCast<openvdb::Vec3SGrid>(source), output, o,
                   budget, h);
    return unary(*openvdb::gridPtrCast<openvdb::Vec3DGrid>(source), output, o,
                 budget, h);
  });
#endif
#else
  (void)input;
  (void)output;
  (void)o;
  (void)inputGrid;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
