#include "Container/geometry/OpenVdbBoundaryFluidStep.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/OpenVdbGridSelection.h"
#include "OpenVdbFileUtils.h"
#include "OpenVdbMetadataCatalog.h"
#include "OpenVdbProvenance.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <openvdb/tools/Count.h>
#include <set>
#include <tbb/task_arena.h>
#include <type_traits>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB boundary smoke stepping is unavailable; "
                           "configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using C = openvdb::Coord;
using V = openvdb::Vec3d;
using S = openvdb::DoubleGrid;
using M = openvdb::Vec3DGrid;
using K = openvdb::Vec3IGrid;
using B = openvdb::BoolGrid;
constexpr uint64_t Fixed = 8ull * 1024 * 1024;
constexpr const char *Schema = "container:boundary_projection_schema";
constexpr const char *Layout = "container:boundary_face_channels";
constexpr const char *StateId = "container:boundary_projection_state_id";
constexpr const char *Clamps = "container:boundary_clamped_faces";
constexpr const char *MaximumClamp = "container:boundary_maximum_clamp";
void require(bool b, const char *message) {
  if (!b)
    throw std::invalid_argument(message);
}
uint64_t add(uint64_t a, uint64_t b) {
  require(b <= UINT64_MAX - a, "boundary smoke UInt64 sum exceeds capacity");
  return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  require(!a || b <= UINT64_MAX / a,
          "boundary smoke UInt64 product exceeds capacity");
  return a * b;
}
struct Budget {
  const BoundaryFluidStepOptions &o;
  uint64_t charged{}, peak{}, nodes{}, projectionIterations{};
  void bytes(uint64_t n) {
    require(n <= o.maxInputBytes,
            "boundary smoke joint source/state/report/solver workspace exceeds "
            "cap BEFORE allocation");
    peak = std::max(peak, n);
  }
  void work(uint64_t n) {
    require(n <= o.maxWork - charged,
            "boundary smoke aggregate work exhausted BEFORE operation");
    charged += n;
  }
  uint64_t remaining() const { return o.maxWork - charged; }
};

// Checked fixed 8192-bit unsigned limbs: no extra dependency, heap or modular
// arithmetic. Adapted from the independently proved point-transfer accumulator.
struct Big {
  static constexpr size_t Limbs = 256;
  std::array<uint32_t, Limbs> data{};
  Big(uint64_t n = 0) {
    data[0] = uint32_t(n);
    data[1] = uint32_t(n >> 32);
  }
  size_t bits() const {
    for (size_t i = Limbs; i--;)
      if (data[i])
        return 32 * i + 32 - std::countl_zero(data[i]);
    return 0;
  }
  bool zero() const { return bits() == 0; }
  int compare(const Big &b) const {
    for (size_t i = Limbs; i--;)
      if (data[i] != b.data[i])
        return data[i] < b.data[i] ? -1 : 1;
    return 0;
  }
  void add(const Big &b) {
    uint64_t carry = 0;
    for (size_t i = 0; i < Limbs; ++i) {
      const auto sum = uint64_t(data[i]) + b.data[i] + carry;
      data[i] = uint32_t(sum);
      carry = sum >> 32;
    }
    if (carry)
      throw std::runtime_error(
          "boundary smoke exact accumulator capacity exceeded");
  }
  void subtract(const Big &b) {
    if (compare(b) < 0)
      throw std::runtime_error("boundary smoke negative unsigned intermediate");
    uint64_t borrow = 0;
    for (size_t i = 0; i < Limbs; ++i) {
      const auto sub = uint64_t(b.data[i]) + borrow, old = uint64_t(data[i]);
      data[i] = uint32_t(old - sub);
      borrow = old < sub;
    }
  }
  void shift(size_t n) {
    if (!n || zero())
      return;
    if (bits() + n > Limbs * 32)
      throw std::runtime_error("boundary smoke exact shift capacity exceeded");
    const auto whole = n / 32, part = n % 32;
    for (size_t i = Limbs; i--;) {
      uint64_t value = 0;
      if (i >= whole) {
        value = uint64_t(data[i - whole]) << part;
        if (part && i > whole)
          value |= uint64_t(data[i - whole - 1]) >> (32 - part);
      }
      data[i] = uint32_t(value);
    }
  }
  size_t trailing() const {
    for (size_t i = 0; i < Limbs; ++i)
      if (data[i])
        return 32 * i + std::countr_zero(data[i]);
    return 0;
  }
  void right(size_t n) {
    const auto whole = n / 32, part = n % 32;
    for (size_t i = 0; i < Limbs; ++i) {
      uint64_t value = 0;
      if (i + whole < Limbs) {
        value = uint64_t(data[i + whole]) >> part;
        if (part && i + whole + 1 < Limbs)
          value |= uint64_t(data[i + whole + 1]) << (32 - part);
      }
      data[i] = uint32_t(value);
    }
  }
  static Big multiply(const Big &a, const Big &b) {
    Big result;
    const size_t an = (a.bits() + 31) / 32, bn = (b.bits() + 31) / 32;
    if (a.zero() || b.zero())
      return result;
    if (a.bits() + b.bits() > Limbs * 32 + 1)
      throw std::runtime_error(
          "boundary smoke exact product capacity exceeded");
    for (size_t i = 0; i < an; ++i) {
      uint64_t carry = 0;
      for (size_t j = 0; j < bn; ++j) {
        if (i + j >= Limbs)
          throw std::runtime_error(
              "boundary smoke exact product capacity exceeded");
        const auto product =
            uint64_t(a.data[i]) * b.data[j] + result.data[i + j] + carry;
        result.data[i + j] = uint32_t(product);
        carry = product >> 32;
      }
      if (carry) {
        if (i + bn >= Limbs)
          throw std::runtime_error(
              "boundary smoke exact product capacity exceeded");
        result.data[i + bn] = uint32_t(carry);
      }
    }
    return result;
  }
};

struct D {
  Big n;
  int e{};
  bool sign{};
  D() = default;
  explicit D(double value) {
    require(std::isfinite(value),
            "boundary smoke exact arithmetic requires finite operands");
    const auto bits = std::bit_cast<uint64_t>(value),
               exponent = (bits >> 52) & 2047;
    n = Big((bits & 0xfffffffffffffull) | (exponent ? 0x10000000000000ull : 0));
    e = exponent ? int(exponent) - 1075 : -1074;
    sign = bool(bits >> 63);
    normalize();
  }
  void normalize() {
    if (n.zero()) {
      e = 0;
      sign = false;
      return;
    }
    const auto z = n.trailing();
    n.right(z);
    e += int(z);
    require(e >= -7000 && e <= 7000,
            "boundary smoke exact exponent exceeds proved span");
  }
  bool zero() const { return n.zero(); }
  bool negative() const { return sign; }
  D operator-() const {
    D a = *this;
    if (!a.zero())
      a.sign = !a.sign;
    return a;
  }
  friend D operator+(const D &a, const D &b) {
    if (a.zero())
      return b;
    if (b.zero())
      return a;
    D r;
    r.e = std::min(a.e, b.e);
    auto x = a.n, y = b.n;
    x.shift(size_t(a.e - r.e));
    y.shift(size_t(b.e - r.e));
    r.sign = a.sign;
    if (a.sign == b.sign) {
      x.add(y);
      r.n = x;
    } else if (x.compare(y) >= 0) {
      x.subtract(y);
      r.n = x;
    } else {
      y.subtract(x);
      r.n = y;
      r.sign = b.sign;
    }
    r.normalize();
    return r;
  }
  friend D operator-(const D &a, const D &b) { return a + (-b); }
  friend D operator*(const D &a, const D &b) {
    D r;
    r.n = Big::multiply(a.n, b.n);
    r.e = a.e + b.e;
    r.sign = a.sign != b.sign;
    r.normalize();
    return r;
  }
  D &operator+=(const D &b) { return *this = *this + b; }
  D &operator-=(const D &b) { return *this = *this - b; }
  D absolute() const {
    D r = *this;
    r.sign = false;
    return r;
  }
  bool operator==(const D &b) const {
    return e == b.e && sign == b.sign && n.compare(b.n) == 0;
  }
  bool operator<(const D &b) const { return (*this - b).negative(); }
  double value(bool *rounded = nullptr,
               bool *diagnosticUnderflow = nullptr) const {
    if (zero())
      return 0;
    const int top = int(n.bits()) - 1 + e;
    require(top <= 1023, "boundary smoke final quantity overflows Double");
    const int target = std::max(top - 52, -1074), shift = target - e;
    auto q = n;
    if (shift > 0) {
      q.right(size_t(shift));
      auto restored = q;
      restored.shift(size_t(shift));
      auto remainder = n;
      remainder.subtract(restored);
      Big half(1);
      half.shift(size_t(shift - 1));
      const int order = remainder.compare(half);
      if (order > 0 || (order == 0 && (q.data[0] & 1)))
        q.add(Big(1));
    } else
      q.shift(size_t(-shift));
    if (q.zero()) {
      if (diagnosticUnderflow) {
        *diagnosticUnderflow = true;
        if (rounded)
          *rounded = true;
        return sign ? -0. : 0.;
      }
      throw std::invalid_argument(
          "boundary smoke nonzero FINAL field underflows Double");
    }
    require(q.bits() <= 54,
            "boundary smoke final significand exceeds proved rounding width");
    const uint64_t significand =
        uint64_t(q.data[0]) | (uint64_t(q.data[1]) << 32);
    const double magnitude = std::ldexp(double(significand), target);
    require(std::isfinite(magnitude),
            "boundary smoke final stored quantity overflows Double");
    const double out = sign ? -magnitude : magnitude;
    if (rounded)
      *rounded = !(D(out) == *this);
    return out;
  }
};
D absolute(const D &d) { return d.absolute(); }
double gap(double x) {
  return std::max(std::abs(std::nextafter(x, INFINITY) - x),
                  std::abs(x - std::nextafter(x, -INFINITY)));
}
double upper(const D &d) {
  if (d.zero())
    return 0;
  bool underflow = false;
  double x = absolute(d).value(nullptr, &underflow);
  if (underflow)
    return std::numeric_limits<double>::denorm_min();
  if (D(x) < absolute(d))
    x = std::nextafter(x, INFINITY);
  return x;
}
double field(const D &d, uint64_t *rounded = nullptr) {
  bool changed = false;
  const auto x = d.value(&changed);
  if (changed && rounded)
    ++*rounded;
  return x;
}

bool same(const openvdb::math::Transform &a,
          const openvdb::math::Transform &b) {
  auto x = a.baseMap(), y = b.baseMap();
  if (x->applyMap(V(0)) != y->applyMap(V(0)))
    return false;
  for (const auto v : {V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)})
    if (x->applyJacobian(v) != y->applyJacobian(v))
      return false;
  return true;
}
struct Frame {
  std::array<V, 3> axes, inverse;
  V origin;
  double h{};
  D volume;
  explicit Frame(const openvdb::math::Transform &t) {
    require(t.isLinear(),
            "boundary smoke requires a uniform orthogonal affine map");
    const auto map = t.baseMap();
    origin = map->applyMap(V(0));
    std::array<V, 3> unit;
    double lengths[3];
    for (int a = 0; a < 3; ++a) {
      V e(0);
      e[a] = 1;
      axes[a] = map->applyJacobian(e);
      lengths[a] = std::hypot(axes[a][0], axes[a][1], axes[a][2]);
      require(std::isfinite(origin[a]) && std::isfinite(lengths[a]) &&
                  lengths[a] >= 1e-6 && lengths[a] <= 1e6,
              "boundary smoke map axes must span[1e-6,1e6]");
      unit[a] = axes[a] / lengths[a];
    }
    h = lengths[0];
    const double det = unit[0].dot(unit[1].cross(unit[2]));
    require(std::isfinite(det) && std::abs(det) > .99999999,
            "boundary smoke map is not uniform orthogonal");
    for (int a = 0; a < 3; ++a) {
      require(std::abs(lengths[a] / h - 1) <= 1e-9,
              "boundary smoke map axes have unequal scales");
      for (int b = 0; b < a; ++b)
        require(std::abs(unit[a].dot(unit[b])) <= 1e-9,
                "boundary smoke map axes are not orthogonal");
    }
    inverse = {unit[1].cross(unit[2]) / (det * lengths[0]),
               unit[2].cross(unit[0]) / (det * lengths[1]),
               unit[0].cross(unit[1]) / (det * lengths[2])};
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b) {
        const int c = (a + 1) % 3, d = (a + 2) % 3;
        const int j = (b + 1) % 3, k = (b + 2) % 3;
        const auto cofactor =
            D(axes[c][j]) * D(axes[d][k]) - D(axes[c][k]) * D(axes[d][j]);
        require(std::isfinite(inverse[a][b]) &&
                    (cofactor.zero() == (inverse[a][b] == 0)),
                "boundary smoke Double inverse coefficient loses a nonzero "
                "represented-map cofactor or introduces a spurious component");
      }
    volume = absolute(D(axes[0][0]) * (D(axes[1][1]) * D(axes[2][2]) -
                                       D(axes[1][2]) * D(axes[2][1])) -
                      D(axes[1][0]) * (D(axes[0][1]) * D(axes[2][2]) -
                                       D(axes[0][2]) * D(axes[2][1])) +
                      D(axes[2][0]) * (D(axes[0][1]) * D(axes[1][2]) -
                                       D(axes[0][2]) * D(axes[1][1])));
    require(!volume.zero(),
            "boundary smoke native Jacobian determinant is zero");
  }
  std::array<D, 3> localForce(const std::array<D, 3> &world, double dt) const {
    std::array<D, 3> r;
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b)
        r[a] += world[b] * D(inverse[a][b]) * D(dt);
    return r;
  }
  double worldSpeed(const std::array<D, 3> &local,
                    bool &diagnosticUnderflow) const {
    V world;
    for (int a = 0; a < 3; ++a) {
      D sum;
      for (int b = 0; b < 3; ++b)
        sum += local[b] * D(axes[b][a]);
      world[a] = sum.value(nullptr, &diagnosticUnderflow);
    }
    return std::hypot(world[0], world[1], world[2]);
  }
};

template <class F> decltype(auto) visit(openvdb::GridBase &g, F &&f) {
#define TYPE(T)                                                                \
  if (g.isType<openvdb::T>())                                                  \
    return f(static_cast<openvdb::T &>(g));
  TYPE(BoolGrid)
  TYPE(FloatGrid)
  TYPE(DoubleGrid)
  TYPE(Vec3IGrid)
  TYPE(Vec3DGrid)
#undef TYPE
  throw std::invalid_argument(
      "boundary smoke selected native grid type is unsupported");
}
template <class T> bool finite(const T &value) {
  if constexpr (std::is_arithmetic_v<T>)
    return std::isfinite(double(value));
  else
    return std::isfinite(double(value[0])) && std::isfinite(double(value[1])) &&
           std::isfinite(double(value[2]));
}
uint64_t metadata(const openvdb::MetaMap &map) {
  uint64_t n = sizeof(map);
  for (auto i = map.beginMeta(); i != map.endMeta(); ++i) {
    require(bool(i->second), "boundary smoke null metadata");
    n = add(
        n,
        add(512, mul(8, add(add(i->first.size(), i->second->typeName().size()),
                            i->second->size()))));
  }
  return n;
}
struct Shape {
  uint64_t leaf{}, lower{}, upper{}, root{};
  uint64_t slots() const {
    return add(root,
               add(mul(leaf, 512), add(mul(lower, 4096), mul(upper, 32768))));
  }
  uint64_t nodes() const {
    return add(1, add(root, add(leaf, add(lower, upper))));
  }
};
template <class G> Shape shape(const G &g) {
  const auto n = g.tree().nodeCount();
  return {n[0], n[1], n[2], uint64_t(g.tree().root().getTableSize())};
}
uint64_t boxVolume(const openvdb::CoordBBox &box) {
  uint64_t n = 1;
  for (int a = 0; a < 3; ++a)
    n = mul(n, uint64_t(int64_t(box.max()[a]) - box.min()[a] + 1));
  return n;
}
template <class G> uint64_t census(const G &grid, uint64_t cap) {
  uint64_t n = 0;
  for (auto i = grid.cbeginValueOn(); i; ++i) {
    n = add(n, boxVolume(i.getBoundingBox()));
    require(n <= cap, "boundary smoke source active census exceeds cap BEFORE "
                      "paging/expansion");
  }
  return n;
}

struct Temporary {
  std::filesystem::path directory;
  std::vector<std::filesystem::path> files;
  Temporary() : directory(detail::CreateAsciiVdbTemporaryDirectory()) {}
  std::filesystem::path next(const char *stem) {
    const auto path =
        directory / (std::string(stem) + std::to_string(files.size()) + ".vdb");
    files.push_back(path);
    return path;
  }
  ~Temporary() {
    std::error_code e;
    for (const auto &file : files)
      std::filesystem::remove(file, e);
    std::filesystem::remove(directory, e);
  }
};
struct Bundle {
  std::filesystem::path snapshot;
  uint64_t encoded{};
  openvdb::MetaMap::Ptr metadata;
  std::vector<std::pair<std::string, openvdb::GridBase::Ptr>> headers;
  std::unique_ptr<openvdb::io::File> file;
};
enum class Role { Fluid, Mac, Kind, Values, Scalar };
struct Reader {
  Budget &budget;
  Temporary &temporary;
  std::map<std::filesystem::path, std::unique_ptr<Bundle>> bundles;
  std::map<std::pair<Bundle *, std::string>, openvdb::GridBase::Ptr> loaded;
  uint64_t encoded{}, meta{}, full{}, nodes{}, slots{}, catalog{};
  Reader(Budget &b, Temporary &t) : budget(b), temporary(t) {
    require(openvdb::io::Archive::isDelayedLoadingEnabled(),
            "boundary smoke bounded reads require delayed loading; unset "
            "OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
    throw std::invalid_argument(
        "boundary smoke requires delayed-loading SDK support");
#endif
  }
  uint64_t resident() const {
    return add(Fixed, add(mul(encoded, 3), add(mul(meta, 16), mul(full, 2))));
  }
  void check() {
    require(meta <= budget.o.maxMetadataBytes,
            "boundary smoke aggregate native metadata/header spans exceed cap");
    require(nodes <= budget.o.maxNodes && slots <= budget.o.maxStoredLeafVoxels,
            "boundary smoke aggregate source nodes/leaf slots exceed cap "
            "BEFORE paging");
    budget.bytes(resident());
  }
  Bundle &bundle(const std::filesystem::path &input) {
    const auto key = std::filesystem::absolute(input).lexically_normal();
    if (auto i = bundles.find(key); i != bundles.end())
      return *i->second;
    require(std::filesystem::is_regular_file(input),
            "boundary smoke input must be a regular seekable file");
    auto b = std::make_unique<Bundle>();
    b->encoded = std::filesystem::file_size(input);
    encoded = add(encoded, b->encoded);
    check();
    budget.work(mul(b->encoded, 2));
    b->snapshot = temporary.next("source");
    {
      std::ifstream from(input, std::ios::binary);
      std::ofstream to(b->snapshot,
                       std::ios::binary | std::ios::out | std::ios::noreplace);
      require(bool(from) && bool(to), "cannot open boundary smoke snapshot");
      std::array<char, 65536> buffer;
      auto left = b->encoded;
      while (left) {
        const auto n = std::streamsize(std::min<uint64_t>(left, buffer.size()));
        from.read(buffer.data(), n);
        require(from.gcount() == n,
                "boundary smoke source length changed during snapshot");
        to.write(buffer.data(), n);
        left -= uint64_t(n);
      }
      to.close();
      require(bool(to) && !from.bad() &&
                  from.peek() == std::char_traits<char>::eof(),
              "boundary smoke immutable snapshot failed");
    }
    detail::RequireSeekableFile(b->snapshot);
    std::ifstream in(b->snapshot, std::ios::binary);
    in.exceptions(std::ios::badbit | std::ios::failbit);
    auto sm = std::make_shared<openvdb::io::StreamMetadata>();
    sm->setSeekable(true);
    detail::ExactMetadataArchive archive;
    archive.begin(in, sm);
    b->metadata = std::make_shared<openvdb::MetaMap>();
    b->metadata->readMeta(in);
    meta = add(meta, metadata(*b->metadata));
    check();
    const int32_t n = archive.count(in);
    require(n >= 0 && uint64_t(n) <= budget.o.maxGrids - catalog,
            "boundary smoke aggregate source catalog exceeds maxGrids");
    catalog += uint64_t(n);
    std::set<std::string> names;
    for (int32_t i = 0; i < n; ++i) {
      openvdb::io::GridDescriptor d;
      auto g = archive.next(in, d, b->encoded);
      const auto end = d.isInstance() ? d.getEndPos() : d.getBlockPos();
      require(end >= d.getGridPos(),
              "boundary smoke native header span is negative");
      meta = add(meta, add(metadata(*g), uint64_t(end - d.getGridPos())));
      check();
      const auto name =
          openvdb::io::GridDescriptor::nameAsString(d.uniqueName());
      require(names.insert(name).second,
              "boundary smoke source descriptors collide");
      b->headers.emplace_back(name, std::move(g));
    }
    in.close();
    b->file = std::make_unique<openvdb::io::File>(b->snapshot.string());
    b->file->setCopyMaxBytes(0);
    b->file->open(true);
    require(b->file->getUniqueTag() == archive.getUniqueTag(),
            "boundary smoke private snapshot identifier changed");
    auto &result = *b;
    bundles.emplace(key, std::move(b));
    return result;
  }
  openvdb::GridBase::Ptr get(const BoundaryProjectionField &binding,
                             Role role) {
    require(!binding.file.empty(),
            "boundary smoke selected input requires a file");
    auto &b = bundle(binding.file);
    openvdb::GridBase::Ptr selected;
    std::string name;
    for (const auto &[descriptor, g] : b.headers) {
      if (!GridSelectionMatches(binding.grid, descriptor, g->getName()))
        continue;
      const bool type =
          role == Role::Fluid  ? g->isType<B>()
          : role == Role::Kind ? g->isType<K>()
          : role == Role::Scalar
              ? (g->isType<S>() || g->isType<openvdb::FloatGrid>())
              : g->isType<M>();
      if (!type) {
        if (!binding.grid.empty())
          throw std::invalid_argument(
              "boundary smoke selected type does not match its role");
        continue;
      }
      require(!selected, "boundary smoke grid selector is ambiguous");
      selected = g;
      name = descriptor;
    }
    require(bool(selected), "boundary smoke selected input grid was not found");
    auto key = std::make_pair(&b, name);
    if (auto i = loaded.find(key); i != loaded.end())
      return i->second;
    const auto memory = selected->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES),
               count = selected->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT);
    require(memory && count && memory->value() >= 0 && count->value() >= 0,
            "boundary smoke requires valid recorded native source memory/count "
            "statistics");
    budget.bytes(add(resident(), uint64_t(memory->value())));
    auto grid = b.file->readGrid(name);
    require(same(grid->transform(), selected->transform()),
            "boundary smoke native read changed exact map");
    grid->setTransform(detail::CopyTransformExact(selected->transform()));
    visit(*grid, [&](const auto &g) {
      const auto s = shape(g);
      nodes = add(nodes, s.nodes());
      slots = add(slots, mul(s.leaf, 512));
      budget.work(mul(s.slots(), 8));
      full = add(full, openvdb::tools::memUsageIfLoaded(g.tree(), false));
      (void)census(g, role == Role::Fluid ? budget.o.maxFluidCells
                                          : budget.o.maxStoredLeafVoxels);
    });
    meta = add(meta, metadata(*grid));
    check();
    loaded.emplace(key, grid);
    return grid;
  }
  void pageAll() {
    check();
    for (const auto &[key, grid] : loaded) {
      uint64_t expected = 0;
      visit(*grid, [&](const auto &g) {
        expected = openvdb::tools::memUsageIfLoaded(g.tree(), false);
      });
      grid->readNonresidentBuffers();
      require(grid->memUsage() <= expected,
              "boundary smoke typed paging exceeded preflight");
      visit(*grid, [&](const auto &g) {
        require(finite(g.background()),
                "boundary smoke nonfinite source background");
        for (auto i = g.cbeginValueAll(); i; ++i)
          require(finite(*i), "boundary smoke nonfinite stored source value");
      });
    }
  }
};

void validateOptions(const BoundaryFluidStepOptions &o,
                     const std::filesystem::path &output) {
  require(o.initialVelocity == BoundaryFluidInitialVelocity::TransportStored ||
              o.initialVelocity == BoundaryFluidInitialVelocity::RequireSolenoidal,
          "boundary smoke initial velocity policy is invalid");
  require(std::isfinite(o.timeStep) && o.timeStep >= 1e-6 &&
              o.timeStep <= 3600 && o.substeps >= 1 && o.substeps <= 64 &&
              o.maxSubsteps >= o.substeps && o.maxSubsteps <= 64 &&
              o.maxStepAttempts >= o.substeps && o.maxStepAttempts <= 128 &&
              o.timeStep / o.substeps >= 1e-6,
          "boundary smoke positive dt/substep/attempt limits are invalid");
  require(std::isfinite(o.maxCfl) && o.maxCfl > 0 && o.maxCfl <= .5 &&
              std::isfinite(o.tolerance) && o.tolerance >= 1e-12 &&
              o.tolerance <= 1e-2 && o.maxIterations &&
              o.maxIterations <= 10000,
          "boundary smoke CFL/tolerance/solver limits are invalid");
  require(o.maxFluidCells && o.maxFluidCells <= 1'000'000 &&
              o.maxStoredLeafVoxels && o.maxNodes && o.maxNodes <= 1'000'000 &&
              o.maxWork && o.maxWork <= 10'000'000'000ull && o.maxGrids &&
              o.maxGrids <= 4096 && o.maxMetadataBytes &&
              o.maxMetadataBytes <= 16ull * 1024 * 1024 && o.maxInputBytes &&
              o.maxInputBytes <= (16ull << 30) && o.maxOutputBytes &&
              o.maxOutputBytes <= (16ull << 30),
          "boundary smoke cell/leaf/node/work/catalog/byte limits are invalid");
  for (double g : o.gravity)
    require(std::isfinite(g) && std::abs(g) <= 1e12,
            "boundary smoke WORLD gravity exceeds finite1e12 cap");
  for (double v : {o.buoyancyDensity, o.buoyancyTemperature,
                   o.ambientTemperature, o.reservoirDensity,
                   o.reservoirTemperature.value_or(o.ambientTemperature)})
    require(std::isfinite(v) && v >= 0 && v <= 1e6,
            "boundary smoke scalar/buoyancy/reservoir settings require "
            "finite[0,1e6]");
  require(
      std::isfinite(std::hypot(o.buoyancyDirection[0], o.buoyancyDirection[1],
                               o.buoyancyDirection[2])) &&
          std::abs(std::hypot(o.buoyancyDirection[0], o.buoyancyDirection[1],
                              o.buoyancyDirection[2]) -
                   1) <= 1e-10,
      "boundary smoke buoyancy direction must be finite and unit WORLD length");
  std::set<std::string> names;
  for (const auto *s :
       {&o.names.density, &o.names.temperature, &o.names.velocity,
        &o.names.pressure, &o.names.fluid, &o.names.faceKind,
        &o.names.faceFraction, &o.names.facePressure, &o.names.wallVelocity})
    require(!s->empty() && s->size() <= 256 &&
                s->find('\0') == std::string::npos && names.insert(*s).second,
            "boundary smoke nine output names must be distinct1..256 non-NUL "
            "bytes");
  require(!output.empty() && !std::filesystem::exists(output) &&
              std::filesystem::is_directory(output.has_parent_path()
                                                ? output.parent_path()
                                                : std::filesystem::path(".")),
          "boundary smoke output must be new in an existing directory");
}
void tags(const openvdb::GridBase &g) {
  for (const auto *key : {openvdb::GridBase::META_GRID_CLASS,
                          openvdb::GridBase::META_VECTOR_TYPE})
    require(!g[key] || bool(g.getMetadata<openvdb::StringMetadata>(key)),
            "boundary smoke native semantic tag has wrong type");
  require(!g[openvdb::GridBase::META_IS_LOCAL_SPACE] ||
              bool(g.getMetadata<openvdb::BoolMetadata>(
                  openvdb::GridBase::META_IS_LOCAL_SPACE)),
          "boundary smoke native space tag has wrong type");
  if (const auto x = g.getMetadata<openvdb::StringMetadata>(
          openvdb::GridBase::META_GRID_CLASS))
    require(x->value() ==
                openvdb::GridBase::gridClassToString(g.getGridClass()),
            "boundary smoke native class spelling is unrecognized");
  if (const auto x = g.getMetadata<openvdb::StringMetadata>(
          openvdb::GridBase::META_VECTOR_TYPE))
    require(x->value() == openvdb::GridBase::vecTypeToString(g.getVectorType()),
            "boundary smoke native vector spelling is unrecognized");
}
std::string membership(const openvdb::GridBase &g) {
  const auto schema = g.getMetadata<openvdb::StringMetadata>(Schema),
             layout = g.getMetadata<openvdb::StringMetadata>(Layout),
             id = g.getMetadata<openvdb::StringMetadata>(StateId);
  require(schema && schema->value() == "negative_xyz_faces_v1" && layout &&
              layout->value() == "negative_faces_xyz" && id &&
              !id->value().empty() && id->value().size() <= 256,
          "boundary smoke input requires valid canonical M04 membership "
          "schema/layout/ID");
  return id->value();
}
void macTags(const openvdb::GridBase &g) {
  tags(g);
  const auto layout =
                 g.getMetadata<openvdb::StringMetadata>("container:mac_layout"),
             units = g.getMetadata<openvdb::StringMetadata>(
                 "container:velocity_units");
  require(g.getGridClass() == openvdb::GRID_STAGGERED && !g.isInWorldSpace() &&
              g.getVectorType() == openvdb::VEC_CONTRAVARIANT_RELATIVE &&
              layout && layout->value() == "negative_faces" && units &&
              units->value() == "voxels_per_second",
          "boundary smoke velocity/walls require canonical local negative-face "
          "MAC units");
}
void packedTags(const openvdb::GridBase &g) {
  tags(g);
  require(
      g.getGridClass() == openvdb::GRID_UNKNOWN && g.isInWorldSpace() &&
          g.getVectorType() == openvdb::VEC_INVARIANT,
      "boundary smoke packed data require WORLD invariant UNKNOWN raw tuples");
}
struct Sources {
  B::Ptr fluid;
  M::Ptr velocity, fraction, prescribed, wall;
  K::Ptr kind;
  openvdb::GridBase::Ptr density, temperature;
  openvdb::MetaMap::Ptr fileMetadata;
};
Sources sources(Reader &reader, const BoundaryFluidInputs &input) {
  const auto &b = input.boundary;
  Sources s;
  s.fluid =
      openvdb::gridPtrCast<B>(reader.get({b.state, b.fluidGrid}, Role::Fluid));
  s.velocity =
      openvdb::gridPtrCast<M>(reader.get({b.state, b.velocityGrid}, Role::Mac));
  s.kind = openvdb::gridPtrCast<K>(
      reader.get({b.state, b.faceKindGrid}, Role::Kind));
  s.fraction = openvdb::gridPtrCast<M>(
      reader.get({b.state, b.faceFractionGrid}, Role::Values));
  s.prescribed = openvdb::gridPtrCast<M>(
      reader.get({b.state, b.facePressureGrid}, Role::Values));
  s.wall = openvdb::gridPtrCast<M>(
      reader.get({b.state, b.wallVelocityGrid}, Role::Mac));
  s.density = reader.get(input.density, Role::Scalar);
  if (input.temperature)
    s.temperature = reader.get(*input.temperature, Role::Scalar);
  s.fileMetadata = reader.bundle(b.state).metadata;
  const auto id = membership(*s.fluid);
  packedTags(*s.fluid);
  macTags(*s.velocity);
  macTags(*s.wall);
  packedTags(*s.kind);
  packedTags(*s.fraction);
  packedTags(*s.prescribed);
  for (const auto &g : openvdb::GridPtrVec{s.velocity, s.kind, s.fraction,
                                           s.prescribed, s.wall})
    require(same(g->transform(), s.fluid->transform()) && membership(*g) == id,
            "boundary smoke canonical member map/ID mismatch");
  for (const auto &g : {s.density, s.temperature})
    if (g) {
      tags(*g);
      require(same(g->transform(), s.fluid->transform()) &&
                  g->isInWorldSpace() &&
                  g->getVectorType() == openvdb::VEC_INVARIANT,
              "boundary smoke scalar fields require exact-map WORLD invariant "
              "semantics");
    }
  require(s.density->getGridClass() == openvdb::GRID_FOG_VOLUME,
          "boundary smoke density requires native FOG_VOLUME");
  if (s.temperature)
    require(s.temperature->getGridClass() == openvdb::GRID_UNKNOWN,
            "boundary smoke temperature requires ordinary UNKNOWN scalar");
  return s;
}
struct Face {
  C coordinate;
  int axis{}, left{-1}, right{-1}, kind{};
  double theta{}, pD{};
  std::array<int, 6> neighbor{};
};
struct State {
  std::vector<C> cells;
  std::vector<std::array<int, 6>> incident;
  std::vector<Face> faces;
  std::vector<int> component;
  std::vector<double> velocity, density, temperature;
  std::set<C> faceCoordinates;
  std::map<std::pair<C, int>, int> faceIndex;
  uint64_t reserve{}, outputReserve{}, outputNodes{}, cellCount{};
  int index(const C &c) const {
    const auto i = std::lower_bound(cells.begin(), cells.end(), c);
    return i != cells.end() && *i == c ? int(i - cells.begin()) : -1;
  }
};
struct Catalog {
  std::set<C> leaf, lower, upper;
  void insert(const C &c) {
    leaf.insert(c & ~7);
    lower.insert(c & ~127);
    upper.insert(c & ~4095);
  }
  uint64_t nodes() const {
    return add(1, add(leaf.size(), add(lower.size(), mul(upper.size(), 2))));
  }
  uint64_t slots() const { return mul(leaf.size(), 512); }
  template <class G> uint64_t bytes() const {
    using T = typename G::TreeType;
    using U = typename T::RootNodeType::ChildNodeType;
    using L = typename U::ChildNodeType;
    using Leaf = typename L::ChildNodeType;
    return add(4096, add(mul(leaf.size(),
                             sizeof(Leaf) +
                                 512 * sizeof(typename G::ValueType) + 1024),
                         add(mul(lower.size(), sizeof(L) + 1024),
                             mul(upper.size(), sizeof(U) + 1280))));
  }
};
C axis(int a, int sign = 1) {
  C c(0);
  c[a] = sign;
  return c;
}
template <class G> void canonicalValues(const G &g, const State &q) {
  require(g.background() == openvdb::zeroVal<typename G::ValueType>(),
          "boundary smoke canonical backgrounds must be zero");
  for (auto i = g.cbeginValueOff(); i; ++i)
    require(*i == openvdb::zeroVal<typename G::ValueType>(),
            "boundary smoke inactive canonical payload must be zero");
  require(census(g, q.faceCoordinates.size()) == q.faceCoordinates.size(),
          "boundary smoke canonical face activity count differs from incident "
          "table");
  for (const auto &c : q.faceCoordinates)
    require(g.tree().isValueOn(c),
            "boundary smoke expected canonical face coordinate is inactive");
}
std::vector<double> scalarValues(const openvdb::GridBase &source,
                                 const State &q) {
  std::vector<double> result(q.cells.size());
  visit(const_cast<openvdb::GridBase &>(source), [&](const auto &g) {
    using T = typename std::decay_t<decltype(g)>::ValueType;
    if constexpr (std::is_floating_point_v<T>) {
      require(g.background() == 0,
              "boundary smoke scalar background must be zero");
      for (auto i = g.cbeginValueAll(); i; ++i) {
        require(*i >= 0 && *i <= 1e6,
                "boundary smoke scalar value is outside[0,1e6]");
        if (!i.isValueOn())
          require(*i == 0, "boundary smoke inactive scalar value must be zero");
        if (*i != 0) {
          const auto box = i.getBoundingBox();
          require(boxVolume(box) <= q.cells.size(),
                  "boundary smoke nonzero scalar tile escapes fluid domain");
          for (auto c = box.begin(); c; ++c)
            require(q.index(*c) >= 0,
                    "boundary smoke nonzero scalar outside fixed fluid domain");
        }
      }
      for (size_t i = 0; i < q.cells.size(); ++i)
        result[i] = double(g.tree().getValue(q.cells[i]));
    }
  });
  return result;
}
State plan(const Sources &source, Reader &reader, Budget &budget) {
  const auto count = census(*source.fluid, budget.o.maxFluidCells);
  require(count > 0, "boundary smoke fluid domain is empty");
  // A Double factor has minimum unit -1074. Three native Jacobian
  // factors put determinant units >=-3222; dt>=1e-6 has unit>=-72.
  // Scalar flux dt*U*rho therefore has unit>=-2220, and its physical
  // balance product with detJ >=-5442. Force beta*(T-Ta)*direction*
  // inverse*dt has unit>=-4368. Magnitudes stay below2^256 including
  // <=1M cells and64 accepted steps. Conservative6200+256+64 carry
  // bits fit8192; each limb operation also checks capacity.
  static_assert(6200 + 256 + 64 < 8192);
  require(budget.o.maxFluidCells <= 1'000'000 &&
              budget.o.maxStepAttempts <= 128,
          "boundary smoke exact accumulator count proof violated");
  const auto resident = reader.resident();
  const auto perState = mul(count, 8192);
  const auto metadataCopies = mul(reader.meta, 32);
  State q;
  q.cellCount = count;
  q.reserve = add(resident, add(perState, metadataCopies));
  budget.bytes(q.reserve);
  budget.work(mul(count, 4096));
  q.cells.reserve(size_t(count));
  require(!source.fluid->background(),
          "boundary smoke canonical fluid background must be false");
  for (auto i = source.fluid->cbeginValueOff(); i; ++i)
    require(!*i, "boundary smoke inactive fluid values must be false");
  for (auto i = source.fluid->cbeginValueOn(); i; ++i) {
    require(bool(*i), "boundary smoke canonical fluid has active false data");
    const auto box = i.getBoundingBox();
    for (int a = 0; a < 3; ++a)
      require(box.min()[a] >= INT32_MIN + 8192 &&
                  box.max()[a] <= INT32_MAX - 8192,
              "boundary smoke fluid coordinates exceed native stencil margins");
    for (auto c = box.begin(); c; ++c) {
      require(q.cells.size() < count,
              "boundary smoke fluid expansion exceeds exact census");
      q.cells.push_back(*c);
    }
  }
  std::sort(q.cells.begin(), q.cells.end());
  require(std::adjacent_find(q.cells.begin(), q.cells.end()) == q.cells.end(),
          "boundary smoke duplicate fluid coordinate");
  q.incident.resize(q.cells.size());
  for (auto &x : q.incident)
    x.fill(-1);
  q.faces.reserve(size_t(mul(count, 6)));
  for (size_t i = 0; i < q.cells.size(); ++i)
    for (int a = 0; a < 3; ++a) {
      const auto c = q.cells[i];
      const auto left = q.index(c - axis(a)), right = q.index(c + axis(a));
      Face f;
      f.coordinate = c;
      f.axis = a;
      f.left = left;
      f.right = int(i);
      int n = int(q.faces.size());
      q.faces.push_back(f);
      q.incident[i][2 * a] = n;
      if (left >= 0)
        q.incident[size_t(left)][2 * a + 1] = n;
      if (right < 0) {
        f.coordinate = c + axis(a);
        f.left = int(i);
        f.right = -1;
        n = int(q.faces.size());
        q.faces.push_back(f);
        q.incident[i][2 * a + 1] = n;
      }
    }
  for (size_t i = 0; i < q.faces.size(); ++i) {
    const auto &f = q.faces[i];
    require(q.faceIndex.emplace(std::make_pair(f.coordinate, f.axis), int(i))
                .second,
            "boundary smoke duplicate incident face");
    q.faceCoordinates.insert(f.coordinate);
  }
  Catalog cells, faces;
  for (const auto &c : q.cells)
    cells.insert(c);
  for (const auto &c : q.faceCoordinates)
    faces.insert(c);
  const auto outputNative = add(
      mul(cells.bytes<S>(), 3),
      add(cells.bytes<B>(), add(mul(faces.bytes<M>(), 4), faces.bytes<K>())));
  q.outputReserve = add(Fixed, add(mul(std::max(outputNative, reader.full), 2),
                                   mul(reader.meta, 8)));
  require(q.outputReserve <= budget.o.maxOutputBytes,
          "boundary smoke conservative nine-grid encoded output exceeds cap "
          "BEFORE native output allocation");
  q.reserve =
      add(q.reserve, add(mul(outputNative, 8),
                         add(mul(reader.full, 4), mul(q.outputReserve, 2))));
  budget.bytes(q.reserve);
  q.outputNodes = add(mul(reader.nodes, 3),
                      add(mul(cells.nodes(), 12), mul(faces.nodes(), 20)));
  require(q.outputNodes <= budget.o.maxNodes,
          "boundary smoke aggregate output/attempt native node bound exceeds "
          "cap BEFORE native output allocation");
  budget.nodes = q.outputNodes;
  require(
      add(reader.slots, add(mul(cells.slots(), 12), mul(faces.slots(), 20))) <=
          budget.o.maxStoredLeafVoxels,
      "boundary smoke aggregate output/attempt leaf slots exceed cap BEFORE "
      "native output allocation");
  canonicalValues(*source.velocity, q);
  canonicalValues(*source.kind, q);
  canonicalValues(*source.fraction, q);
  canonicalValues(*source.prescribed, q);
  canonicalValues(*source.wall, q);
  q.velocity.resize(q.faces.size());
  uint64_t freeSurfaces = 0;
  for (const auto &c : q.faceCoordinates)
    for (int a = 0; a < 3; ++a)
      if (!q.faceIndex.contains({c, a}))
        require(source.kind->tree().getValue(c)[a] == 0 &&
                    source.fraction->tree().getValue(c)[a] == 0 &&
                    source.prescribed->tree().getValue(c)[a] == 0 &&
                    source.wall->tree().getValue(c)[a] == 0 &&
                    source.velocity->tree().getValue(c)[a] == 0,
                "boundary smoke absent canonical axis contains hidden payload");
  for (size_t i = 0; i < q.faces.size(); ++i) {
    auto &f = q.faces[i];
    const auto c = f.coordinate;
    const auto a = f.axis;
    f.kind = source.kind->tree().getValue(c)[a];
    f.theta = source.fraction->tree().getValue(c)[a];
    f.pD = source.prescribed->tree().getValue(c)[a];
    const auto wall = source.wall->tree().getValue(c)[a];
    q.velocity[i] = source.velocity->tree().getValue(c)[a];
    require(std::abs(q.velocity[i]) <= 1e24,
            "boundary smoke MAC input exceeds1e24 cap");
    if (f.left >= 0 && f.right >= 0)
      require(f.kind == 1 && f.theta == 1 && f.pD == 0 && wall == 0,
              "boundary smoke fluid-fluid canonical face data is invalid");
    else if (f.kind == 2)
      require(f.theta == 0 && f.pD == 0 && wall == 0 && q.velocity[i] == 0,
              "boundary smoke requires stationary zero-normal walls; moving "
              "geometry is unsupported");
    else {
      require(f.kind >= 3 && f.kind <= 5 && wall == 0 && f.theta >= 1e-6 &&
                  f.theta <= 1 && (f.kind != 3 || f.theta == .5) &&
                  (f.kind != 4 || f.theta == 1),
              "boundary smoke pressure canonical face data is invalid");
      if (f.kind == 5)
        ++freeSurfaces;
    }
  }
  const auto clamp =
      source.fraction->getMetadata<openvdb::Int64Metadata>(Clamps);
  const auto maximum =
      source.fraction->getMetadata<openvdb::DoubleMetadata>(MaximumClamp);
  require(clamp && clamp->value() >= 0 &&
              uint64_t(clamp->value()) <= freeSurfaces && maximum &&
              std::isfinite(maximum->value()) && maximum->value() >= 0 &&
              maximum->value() <= 1 &&
              ((clamp->value() == 0) == (maximum->value() == 0)),
          "boundary smoke canonical interface diagnostics are invalid");
  for (auto &f : q.faces)
    for (int j = 0; j < 3; ++j)
      for (int sign : {-1, 1}) {
        const int side = 2 * j + (sign > 0);
        f.neighbor[size_t(side)] = -1;
        const auto found =
            q.faceIndex.find({f.coordinate + axis(j, sign), f.axis});
        if (found == q.faceIndex.end())
          continue;
        const auto &g = q.faces[size_t(found->second)];
        bool connected = false;
        if (j == f.axis)
          connected = (sign < 0 ? f.left : f.right) >= 0;
        else
          for (int cell : {f.left, f.right})
            if (cell >= 0) {
              const auto next = q.index(q.cells[size_t(cell)] + axis(j, sign));
              if (next >= 0 && (g.left == next || g.right == next))
                connected = true;
            }
        if (connected)
          f.neighbor[size_t(side)] = found->second;
      }
  q.component.assign(q.cells.size(), -1);
  std::vector<int> queue;
  queue.reserve(q.cells.size());
  int component = 0;
  for (size_t seed = 0; seed < q.cells.size(); ++seed)
    if (q.component[seed] < 0) {
      queue.clear();
      queue.push_back(int(seed));
      q.component[seed] = component;
      for (size_t h = 0; h < queue.size(); ++h) {
        const int cell = queue[h];
        for (int face : q.incident[size_t(cell)]) {
          const auto &f = q.faces[size_t(face)];
          if (f.kind != 1)
            continue;
          const int next = f.left == cell ? f.right : f.left;
          if (q.component[size_t(next)] < 0) {
            q.component[size_t(next)] = component;
            queue.push_back(next);
          }
        }
      }
      ++component;
    }
  q.density = scalarValues(*source.density, q);
  q.temperature =
      source.temperature
          ? scalarValues(*source.temperature, q)
          : std::vector<double>(q.cells.size(), budget.o.ambientTemperature);
  return q;
}

D divergence(const State &q, const std::vector<double> &velocity, size_t cell) {
  D r;
  for (int a = 0; a < 3; ++a) {
    r += D(velocity[size_t(q.incident[cell][2 * a + 1])]);
    r -= D(velocity[size_t(q.incident[cell][2 * a])]);
  }
  return r;
}
double checkDivergence(const State &q, const std::vector<double> &velocity,
                       double tolerance, double &allowance, bool &limited,
                       const std::vector<double> *predictor = nullptr,
                       bool enforce = true, bool *within = nullptr) {
  int groups = 0;
  for (int c : q.component)
    groups = std::max(groups, c + 1);
  std::vector<double> maximum(size_t(groups), 0);
  for (size_t i = 0; i < q.cells.size(); ++i)
    for (int f : q.incident[i])
      maximum[size_t(q.component[i])] = std::max(
          maximum[size_t(q.component[i])], std::abs(velocity[size_t(f)]));
  if (predictor)
    for (size_t i = 0; i < q.cells.size(); ++i)
      for (int f : q.incident[i])
        maximum[size_t(q.component[i])] = std::max(
            maximum[size_t(q.component[i])], std::abs((*predictor)[size_t(f)]));
  double measured = 0;
  for (size_t i = 0; i < q.cells.size(); ++i) {
    D ulps;
    for (int f : q.incident[i])
      ulps += D(gap(velocity[size_t(f)])) * D(8.);
    const auto error = absolute(divergence(q, velocity, i));
    const auto target = D(maximum[size_t(q.component[i])]) * D(tolerance);
    const bool accepted = !(target + ulps < error);
    if (within)
      *within &= accepted;
    require(
        !enforce || accepted,
        "boundary smoke initial/endpoint stored divergence exceeds selected "
        "per-component velocity tolerance and final six-face ULP allowance");
    measured = std::max(measured, upper(error));
    allowance = std::max(allowance, upper(ulps));
    limited |= accepted && target < error;
  }
  return measured;
}
std::array<D, 3> advectors(const State &q, const std::vector<double> &velocity,
                           const Face &f) {
  std::array<D, 3> a;
  a[size_t(f.axis)] =
      D(velocity[size_t(q.faceIndex.at({f.coordinate, f.axis}))]);
  const int count = (f.left >= 0) + (f.right >= 0);
  for (int j = 0; j < 3; ++j)
    if (j != f.axis) {
      for (int cell : {f.left, f.right})
        if (cell >= 0) {
          a[size_t(j)] += D(velocity[size_t(q.incident[size_t(cell)][2 * j])]);
          a[size_t(j)] +=
              D(velocity[size_t(q.incident[size_t(cell)][2 * j + 1])]);
        }
      a[size_t(j)] = a[size_t(j)] * D(count == 2 ? .25 : .5);
    }
  return a;
}
D rate(const State &q, const std::vector<double> &velocity) {
  D maximum;
  for (size_t cell = 0; cell < q.cells.size(); ++cell) {
    D outgoing;
    for (int a = 0; a < 3; ++a) {
      const double lo = velocity[size_t(q.incident[cell][2 * a])],
                   hi = velocity[size_t(q.incident[cell][2 * a + 1])];
      if (lo < 0)
        outgoing += D(-lo);
      if (hi > 0)
        outgoing += D(hi);
    }
    if (maximum < outgoing)
      maximum = outgoing;
  }
  for (const auto &f : q.faces)
    if (f.kind != 2) {
      D norm;
      for (const auto &a : advectors(q, velocity, f))
        norm += absolute(a);
      if (maximum < norm)
        maximum = norm;
    }
  return maximum;
}
double speed(const State &q, const std::vector<double> &velocity,
             const Frame &frame, bool &underflow) {
  double maximum = 0;
  for (size_t i = 0; i < q.cells.size(); ++i) {
    std::array<D, 3> center;
    for (int a = 0; a < 3; ++a)
      center[size_t(a)] = (D(velocity[size_t(q.incident[i][2 * a])]) +
                           D(velocity[size_t(q.incident[i][2 * a + 1])])) *
                          D(.5);
    maximum = std::max(maximum, frame.worldSpeed(center, underflow));
  }
  return maximum;
}
D total(const std::vector<double> &values) {
  D sum;
  for (double x : values)
    sum += D(x);
  return sum;
}
struct Trial {
  std::vector<double> density, temperature, velocity;
  D densityOut, temperatureOut, densityRound, temperatureRound;
  double cfl{}, constantDrift{};
  uint64_t reservoirs{}, outflows{}, roundedScalars{}, roundedMomentum{};
};
Trial transport(const State &q, double dt, const BoundaryFluidStepOptions &o,
                const Frame &frame, Budget &budget) {
  budget.work(mul(add(q.cells.size(), q.faces.size()), 65536));
  Trial result;
  result.density.resize(q.cells.size());
  result.temperature.resize(q.cells.size());
  result.velocity.resize(q.faces.size());
  const D time(dt);
  const auto courant = rate(q, q.velocity) * time;
  require(!(D(o.maxCfl) < courant),
          "boundary smoke donor/face CFL exceeds bound BEFORE transport");
  result.cfl = upper(courant);
  // One shared exact flux per face; the two adjacent rows consume it with
  // opposite signs. Fixed exact scratch is allocated per pass, not per cell.
  const auto scalar = [&](const std::vector<double> &input,
                          std::vector<double> &output, double reservoir,
                          D &outward, D &rounding) {
    for (size_t cell = 0; cell < q.cells.size(); ++cell) {
      D value(input[cell]);
      for (int index : q.incident[cell]) {
        const auto &f = q.faces[size_t(index)];
        if (f.kind == 2)
          continue;
        const double u = q.velocity[size_t(index)];
        const int donor = u >= 0 ? f.left : f.right;
        const D flux =
            time * D(u) * D(donor >= 0 ? input[size_t(donor)] : reservoir);
        value += (f.left == int(cell) ? -flux : flux);
      }
      require(!value.negative() && !(D(1e6) < value),
              "boundary smoke conservative scalar update lies outside[0,1e6]; "
              "no hidden clamp");
      if (value == D(input[cell]))
        output[cell] = input[cell];
      else
        output[cell] = field(value, &result.roundedScalars);
      rounding += absolute(D(output[cell]) - value);
    }
    for (size_t index = 0; index < q.faces.size(); ++index) {
      const auto &f = q.faces[index];
      if (f.kind < 3)
        continue;
      const double u = q.velocity[index];
      const int donor = u >= 0 ? f.left : f.right;
      const auto flux =
          time * D(u) * D(donor >= 0 ? input[size_t(donor)] : reservoir);
      outward += (f.left >= 0 ? flux : -flux);
    }
  };
  scalar(q.density, result.density, o.reservoirDensity, result.densityOut,
         result.densityRound);
  scalar(q.temperature, result.temperature,
         o.reservoirTemperature.value_or(o.ambientTemperature),
         result.temperatureOut, result.temperatureRound);
  for (size_t cell = 0; cell < q.cells.size(); ++cell)
    result.constantDrift =
        std::max(result.constantDrift,
                 upper(absolute(divergence(q, q.velocity, cell) * time)));
  for (size_t i = 0; i < q.faces.size(); ++i) {
    const auto &f = q.faces[i];
    if (f.kind == 2) {
      result.velocity[i] = q.velocity[i];
      continue;
    }
    const auto a = advectors(q, q.velocity, f);
    D value(q.velocity[i]);
    for (int j = 0; j < 3; ++j) {
      const auto &v = a[size_t(j)];
      if (v.zero())
        continue;
      const int side = 2 * j + (v.negative() ? 1 : 0),
                next = f.neighbor[size_t(side)];
      if (next >= 0)
        value -= time * absolute(v) *
                 (D(q.velocity[i]) - D(q.velocity[size_t(next)]));
    }
    require(!(D(1e24) < absolute(value)),
            "boundary smoke advected momentum exceeds1e24 cap");
    result.velocity[i] = value == D(q.velocity[i])
                             ? q.velocity[i]
                             : field(value, &result.roundedMomentum);
  }
  // Newly transported scalars drive buoyancy. Every product/average remains
  // dyadic through the WORLD-to-local inverse and actual dt.
  const auto acceleration = [&](size_t cell) {
    const D thermal = D(o.buoyancyTemperature) *
                      (D(result.temperature[cell]) - D(o.ambientTemperature));
    const D density = D(o.buoyancyDensity) * D(result.density[cell]);
    std::array<D, 3> world;
    for (int a = 0; a < 3; ++a) {
      world[size_t(a)] =
          D(o.gravity[size_t(a)]) +
          D(o.buoyancyDirection[size_t(a)]) * (thermal - density);
      require(!(D(1e12) < absolute(world[size_t(a)])),
              "boundary smoke combined WORLD acceleration exceeds1e12 cap");
    }
    return world;
  };
  for (size_t i = 0; i < q.faces.size(); ++i) {
    const auto &f = q.faces[i];
    if (f.kind == 2)
      continue;
    std::array<D, 3> world;
    int count = 0;
    for (int cell : {f.left, f.right})
      if (cell >= 0) {
        const auto a = acceleration(size_t(cell));
        for (int j = 0; j < 3; ++j)
          world[size_t(j)] += a[size_t(j)];
        ++count;
      }
    if (count == 2)
      for (auto &a : world)
        a = a * D(.5);
    const auto increment = frame.localForce(world, dt)[size_t(f.axis)];
    const auto exact = D(result.velocity[i]) + increment;
    require(!(D(1e24) < absolute(exact)),
            "boundary smoke force predictor exceeds1e24 MAC cap");
    const double value = field(exact, &result.roundedMomentum);
    require(increment.zero() || value != result.velocity[i],
            "boundary smoke nonzero WORLD force increment is unresolved in "
            "stored MAC precision");
    result.velocity[i] = value;
  }
  for (size_t i = 0; i < q.faces.size(); ++i)
    if (q.faces[i].kind >= 3 && q.velocity[i] != 0) {
      const bool outward = (q.faces[i].left >= 0) == (q.velocity[i] > 0);
      if (outward)
        ++result.outflows;
      else
        ++result.reservoirs;
    }
  return result;
}

template <class G> class ExactWrite final : public G {
public:
  explicit ExactWrite(const G &source)
      : G(openvdb::ConstPtrCast<typename G::TreeType>(source.constTreePtr())) {
    this->insertMeta(source);
    this->setTransform(detail::CopyTransformExact(source.transform()));
    this->setSaveFloatAsHalf(false);
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, detail::CopyTransformExact(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactWrite &>(*this).copyGrid());
  }
};
void write(const std::filesystem::path &file, const openvdb::GridPtrVec &grids,
           const openvdb::MetaMap &metadata, uint64_t cap) {
  require(!std::filesystem::exists(file),
          "boundary smoke owned staging filename already exists");
  openvdb::GridPtrVec views;
  views.reserve(grids.size());
  for (const auto &g : grids)
    visit(*g, [&](const auto &grid) {
      using G = std::decay_t<decltype(grid)>;
      views.push_back(std::make_shared<ExactWrite<G>>(grid));
    });
  openvdb::io::File writer(file.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(views, metadata);
  writer.close();
  require(std::filesystem::file_size(file) <= cap,
          "boundary smoke actual encoded staging output exceeds preflight cap");
}
M::Ptr velocityGrid(const State &q, const std::vector<double> &values,
                    const M &source) {
  auto grid = M::create(V(0));
  grid->insertMeta(source);
  grid->setTransform(detail::CopyTransformExact(source.transform()));
  grid->setSaveFloatAsHalf(false);
  for (size_t i = 0; i < q.faces.size(); ++i) {
    const auto &f = q.faces[i];
    auto v = grid->tree().getValue(f.coordinate);
    v[f.axis] = values[i];
    grid->tree().touchLeaf(f.coordinate)->setValueOn(f.coordinate, v);
  }
  return grid;
}
struct Projected {
  Sources sources;
  S::Ptr pressure;
  std::vector<double> velocity;
  BoundaryProjectionResult report;
};
Projected project(const State &q, const Sources &source, const Trial &trial,
                  double dt, Reader &original, Temporary &temporary,
                  Budget &budget) {
  const auto input = temporary.next("predictor"),
             output = temporary.next("projected");
  const auto remainingBytes = budget.o.maxInputBytes - q.reserve;
  require(
      remainingBytes > Fixed && budget.o.maxMetadataBytes > original.meta,
      "boundary smoke retained state leaves no projection workspace/metadata");
  require(original.catalog + 7 <= budget.o.maxGrids,
          "boundary smoke retained plus generated catalogs exceed maxGrids "
          "BEFORE projection");
  budget.work(mul(q.outputReserve, 6));
  auto velocity = velocityGrid(q, trial.velocity, *source.velocity);
  // Private role names are independent of duplicate/binary source display
  // names.
  auto fluid = std::make_shared<ExactWrite<B>>(*source.fluid);
  auto kind = std::make_shared<ExactWrite<K>>(*source.kind);
  auto fraction = std::make_shared<ExactWrite<M>>(*source.fraction);
  auto prescribed = std::make_shared<ExactWrite<M>>(*source.prescribed);
  auto wall = std::make_shared<ExactWrite<M>>(*source.wall);
  velocity->setName("__predictor");
  fluid->setName("__fluid");
  kind->setName("__kind");
  fraction->setName("__theta");
  prescribed->setName("__pD");
  wall->setName("__wall");
  write(input, {velocity, fluid, kind, fraction, prescribed, wall},
        *source.fileMetadata, q.outputReserve);
  BoundaryProjectionReplay replay;
  replay.state = input;
  replay.velocityGrid = "__predictor";
  replay.fluidGrid = "__fluid";
  replay.faceKindGrid = "__kind";
  replay.faceFractionGrid = "__theta";
  replay.facePressureGrid = "__pD";
  replay.wallVelocityGrid = "__wall";
  BoundaryProjectionOptions o;
  o.projection.timeStep = dt;
  o.projection.tolerance = budget.o.tolerance;
  o.projection.maxIterations = budget.o.maxIterations;
  o.projection.maxFluidCells = budget.o.maxFluidCells;
  o.projection.velocityGridName = budget.o.names.velocity;
  o.projection.pressureGridName = budget.o.names.pressure;
  o.projection.domainGridName = budget.o.names.fluid;
  o.faceKindGridName = budget.o.names.faceKind;
  o.faceFractionGridName = budget.o.names.faceFraction;
  o.facePressureGridName = budget.o.names.facePressure;
  o.wallVelocityGridName = budget.o.names.wallVelocity;
  o.maxWorkItems = budget.remaining();
  o.maxGrids = budget.o.maxGrids - original.catalog;
  o.maxMetadataBytes = budget.o.maxMetadataBytes - original.meta;
  ProcessingOptions p;
  p.maxInputBytes = remainingBytes;
  p.maxVoxelCount = budget.o.maxStoredLeafVoxels;
  Projected result;
  result.report = WriteReprojectedBoundaryState(replay, output, o, p);
  budget.work(result.report.workUpperBound);
  budget.projectionIterations =
      add(budget.projectionIterations, result.report.totalIterations);
  // The public M04 call has already preflighted its whole solver peak within
  // remainingBytes. Record that conservative reserved upper bound, not RSS.
  budget.peak = std::max(budget.peak, add(q.reserve, remainingBytes));
  auto limits = budget.o;
  limits.maxInputBytes = remainingBytes;
  limits.maxMetadataBytes -= original.meta;
  limits.maxGrids -= uint32_t(original.catalog);
  limits.maxWork = budget.remaining();
  limits.maxNodes -= std::min(limits.maxNodes - 1, original.nodes);
  Temporary localTemporary;
  Budget localBudget{limits};
  {
    Reader reader(localBudget, localTemporary);
    auto &s = result.sources;
    s.velocity = openvdb::gridPtrCast<M>(
        reader.get({output, "descriptor:" + limits.names.velocity}, Role::Mac));
    s.fluid = openvdb::gridPtrCast<B>(
        reader.get({output, "descriptor:" + limits.names.fluid}, Role::Fluid));
    s.kind = openvdb::gridPtrCast<K>(reader.get(
        {output, "descriptor:" + limits.names.faceKind}, Role::Kind));
    s.fraction = openvdb::gridPtrCast<M>(reader.get(
        {output, "descriptor:" + limits.names.faceFraction}, Role::Values));
    s.prescribed = openvdb::gridPtrCast<M>(reader.get(
        {output, "descriptor:" + limits.names.facePressure}, Role::Values));
    s.wall = openvdb::gridPtrCast<M>(reader.get(
        {output, "descriptor:" + limits.names.wallVelocity}, Role::Mac));
    result.pressure = openvdb::gridPtrCast<S>(reader.get(
        {output, "descriptor:" + limits.names.pressure}, Role::Scalar));
    s.fileMetadata = source.fileMetadata;
    s.density = source.density;
    s.temperature = source.temperature;
    reader.pageAll();
    require(same(s.velocity->transform(), source.velocity->transform()),
            "boundary smoke projection changed the native map");
    result.velocity.resize(q.faces.size());
    for (size_t i = 0; i < q.faces.size(); ++i)
      result.velocity[i] =
          s.velocity->tree().getValue(q.faces[i].coordinate)[q.faces[i].axis];
  }
  budget.work(localBudget.charged);
  budget.bytes(add(q.reserve, localBudget.peak));
  std::error_code error;
  std::filesystem::remove(input, error);
  require(!error, "boundary smoke could not retire its predictor scratch");
  std::filesystem::remove(output, error);
  require(!error, "boundary smoke could not retire its projected scratch");
  return result;
}

S::Ptr scalarGrid(const State &q, const std::vector<double> &values,
                  const openvdb::GridBase &source, const B &fluid,
                  const std::string &name, bool fog) {
  auto grid = S::create(0);
  grid->insertMeta(source);
  grid->setTransform(detail::CopyTransformExact(fluid.transform()));
  for (const auto *key :
       {openvdb::GridBase::META_GRID_NAME, openvdb::GridBase::META_GRID_CLASS,
        openvdb::GridBase::META_IS_LOCAL_SPACE,
        openvdb::GridBase::META_VECTOR_TYPE,
        openvdb::GridBase::META_SAVE_HALF_FLOAT,
        openvdb::GridBase::META_FILE_BBOX_MIN,
        openvdb::GridBase::META_FILE_BBOX_MAX,
        openvdb::GridBase::META_FILE_MEM_BYTES,
        openvdb::GridBase::META_FILE_VOXEL_COUNT,
        openvdb::GridBase::META_FILE_COMPRESSION,
        openvdb::GridBase::META_FILE_DELAYED_LOAD})
    grid->removeMeta(key);
  detail::RemoveBoundaryProjectionMetadata(*grid);
  grid->setName(name);
  grid->setGridClass(fog ? openvdb::GRID_FOG_VOLUME : openvdb::GRID_UNKNOWN);
  grid->setVectorType(openvdb::VEC_INVARIANT);
  grid->setIsInWorldSpace(true);
  grid->setSaveFloatAsHalf(false);
  for (size_t i = 0; i < q.cells.size(); ++i)
    grid->tree().touchLeaf(q.cells[i])->setValueOn(q.cells[i], values[i]);
  return grid;
}
void publish(const std::filesystem::path &source,
             const std::filesystem::path &output) {
  bool owned = false;
  std::ofstream to;
  try {
    std::ifstream from(source, std::ios::binary);
    require(bool(from), "cannot read completed boundary smoke output");
    to.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    owned = to.is_open();
    require(owned, "cannot exclusively create boundary smoke output");
    from.exceptions(std::ios::badbit);
    to.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 65536> buffer;
    while (from.read(buffer.data(), std::streamsize(buffer.size())) ||
           from.gcount())
      to.write(buffer.data(), from.gcount());
    require(from.eof(), "boundary smoke completed publication read failed");
    to.close();
  } catch (...) {
    to.exceptions(std::ios::goodbit);
    if (to.is_open())
      to.close();
    if (owned) {
      std::error_code error;
      std::filesystem::remove(output, error);
    }
    throw;
  }
}
double diagnostic(const D &d, BoundaryFluidStepResult &r) {
  return d.value(nullptr, &r.diagnosticUnderflow);
}
BoundaryFluidStepResult run(const BoundaryFluidInputs &input,
                            const std::filesystem::path &output,
                            const BoundaryFluidStepOptions &o) {
  Budget budget{o};
  budget.bytes(Fixed);
  Temporary temporary;
  Reader reader(budget, temporary);
  auto source = sources(reader, input);
  const Frame frame(source.fluid->transform());
  reader.pageAll();
  auto q = plan(source, reader, budget);
  BoundaryFluidStepResult result;
  result.steps.reserve(o.maxSubsteps);
  result.diagnostics = {
      "Fixed fluid cells and saved pressure ghost distances; stationary "
      "zero-normal walls, no moving geometry or evolving liquid phi.",
      "Scalar shared upwind flux uses constant reservoirs on pressure inflow "
      "and adjacent fluid donors on outflow. No normalization, range clamp or "
      "exact-constant claim for measured stored divergence.",
      o.initialVelocity == BoundaryFluidInitialVelocity::TransportStored
          ? "TransportStored initial velocity: actual finite stored MAC values "
            "and measured divergence enter donor transport unchanged; no initial "
            "projection or weak-value erasure."
          : "RequireSolenoidal initial velocity: stored-only relative/ULP "
            "tolerance is enforced at entry and every accepted endpoint; no "
            "initial cleanup projection.",
      "MAC momentum is first-order advective form with local incident-cell "
      "averages and no-fluid-path constant continuation; existing incident "
      "wall-normal neighbors retain prescribed zero. This is not conservative "
      "momentum, kinetic energy or tangential no-slip transport.",
      "One fresh pressure impulse per accepted dt. Endpoint-CFL retries "
      "restart the same private input; discarded solves/transport remain "
      "charged and add no state impulse.",
      "All9 output grids share budgets. Boundary-state file metadata and "
      "matching scalar grid metadata survive; separate scalar file metadata is "
      "not merged. Logical caps are not a hostile-file RSS/CPU sandbox."};
  result.fluidCells = q.cells.size();
  result.incidentFaces = q.faces.size();
  for (const auto &f : q.faces) {
    result.pressureFaces += f.kind >= 3;
    result.wallFaces += f.kind == 2;
  }
  result.requestedSubsteps = o.substeps;
  result.temperatureInitialized = !source.temperature;
  result.cellVolume = diagnostic(frame.volume, result);
  result.initialDivergenceWithinTolerance = true;
  bool initialLimited = false;
  result.maxDivergenceBefore = checkDivergence(
      q, q.velocity, o.tolerance, result.divergenceRoundoffBound,
      initialLimited, nullptr,
      o.initialVelocity == BoundaryFluidInitialVelocity::RequireSolenoidal,
      &result.initialDivergenceWithinTolerance);
  // A permissively transported compressible state is not a roundoff claim.
  result.roundoffLimited = result.initialDivergenceWithinTolerance && initialLimited;
  result.maxWorldSpeedBefore =
      speed(q, q.velocity, frame, result.diagnosticUnderflow);
  const auto densityBefore = total(q.density),
             temperatureBefore = total(q.temperature);
  D densityOut, temperatureOut, densityRound, temperatureRound, elapsed;
  S::Ptr pressure;
  double start = 0;
  for (uint32_t interval = 0; interval < o.substeps; ++interval) {
    const double end = interval + 1 == o.substeps
                           ? o.timeStep
                           : o.timeStep * double(interval + 1) / o.substeps;
    double time = start;
    while (time < end) {
      require(result.actualSubsteps < o.maxSubsteps,
              "boundary smoke accepted-step cap exhausted before full "
              "requested time");
      const double remaining = end - time;
      require(remaining >= 1e-6,
              "boundary smoke unresolved final time remainder below minimum "
              "dt; no partial time published");
      const auto oldRate = rate(q, q.velocity);
      double dt = remaining;
      const double bound = upper(oldRate);
      if (bound != 0 && D(o.maxCfl) < oldRate * D(dt))
        dt = std::min(dt, std::nextafter(o.maxCfl / bound, 0.));
      require(std::isfinite(dt) && dt >= 1e-6,
              "boundary smoke CFL requires dt below minimum1e-6");
      if (remaining - dt > 0 && remaining - dt < 1e-6) {
        dt = remaining * .5;
        require(dt >= 1e-6,
                "boundary smoke cannot partition final interval at valid dt");
      }
      for (;;) {
        require(
            result.attempts < o.maxStepAttempts,
            "boundary smoke trial-attempt cap exhausted before publication");
        ++result.attempts;
        auto trial = transport(q, dt, o, frame, budget);
        auto projected =
            project(q, source, trial, dt, reader, temporary, budget);
        const auto endpoint = rate(q, projected.velocity) * D(dt);
        if (D(o.maxCfl) < endpoint) {
          ++result.rejectedAttempts;
          dt *= .5;
          require(dt >= 1e-6, "boundary smoke force/pressure endpoint CFL "
                              "requires dt below minimum1e-6");
          continue;
        }
        double allowance = 0;
        bool limited = false;
        const double divergenceAfter =
            checkDivergence(q, projected.velocity, o.tolerance, allowance,
                            limited,
                            o.initialVelocity == BoundaryFluidInitialVelocity::RequireSolenoidal
                                ? nullptr : &trial.velocity);
        // Only this branch advances state, time, scalar exchange or pressure.
        q.density = std::move(trial.density);
        q.temperature = std::move(trial.temperature);
        q.velocity = std::move(projected.velocity);
        source = std::move(projected.sources);
        pressure = std::move(projected.pressure);
        densityOut += trial.densityOut;
        temperatureOut += trial.temperatureOut;
        densityRound += trial.densityRound;
        temperatureRound += trial.temperatureRound;
        result.reservoirFaceApplications += trial.reservoirs;
        result.outflowFaceApplications += trial.outflows;
        result.roundedScalarRows += trial.roundedScalars;
        result.roundedMomentumRows += trial.roundedMomentum;
        result.maxTransportCfl = std::max(result.maxTransportCfl, trial.cfl);
        result.maxEndpointCfl =
            std::max(result.maxEndpointCfl, upper(endpoint));
        result.maxConstantResponseDrift =
            std::max(result.maxConstantResponseDrift, trial.constantDrift);
        result.maxDivergenceAfter = divergenceAfter;
        result.divergenceRoundoffBound =
            std::max(result.divergenceRoundoffBound, allowance);
        result.roundoffLimited |=
            limited || projected.report.projection.roundoffLimited;
        result.pressureResidual = std::max(
            result.pressureResidual, projected.report.projection.residual);
        result.steps.push_back({dt, trial.cfl, upper(endpoint),
                                trial.constantDrift, divergenceAfter,
                                projected.report.projection.residual,
                                projected.report.totalIterations});
        ++result.actualSubsteps;
        elapsed += D(dt);
        const double next = time + dt;
        require(
            next > time && next <= end,
            "boundary smoke Double time progress is unresolved or overshoots");
        time = next;
        break;
      }
    }
    start = end;
  }
  result.totalProjectionIterations = budget.projectionIterations;
  result.simulatedTime = diagnostic(elapsed, result);
  result.timePartitionError = diagnostic(elapsed - D(o.timeStep), result);
  require(std::abs(result.timePartitionError) <= 8 * gap(o.timeStep),
          "boundary smoke accepted dt partition fails requested total time");
  result.maxWorldSpeedAfter =
      speed(q, q.velocity, frame, result.diagnosticUnderflow);
  result.densityMassBefore = diagnostic(densityBefore * frame.volume, result);
  result.temperatureVolumeBefore =
      diagnostic(temperatureBefore * frame.volume, result);
  const auto densityAfter = total(q.density),
             temperatureAfter = total(q.temperature);
  result.densityMassAfter = diagnostic(densityAfter * frame.volume, result);
  result.temperatureVolumeAfter =
      diagnostic(temperatureAfter * frame.volume, result);
  result.densityOutwardTransfer = diagnostic(densityOut * frame.volume, result);
  result.temperatureOutwardTransfer =
      diagnostic(temperatureOut * frame.volume, result);
  const auto densityError = absolute(
                 (densityAfter - densityBefore + densityOut) * frame.volume),
             temperatureError = absolute(
                 (temperatureAfter - temperatureBefore + temperatureOut) *
                 frame.volume);
  const auto densityBound = densityRound * frame.volume,
             temperatureBound = temperatureRound * frame.volume;
  require(!(densityBound < densityError) &&
              !(temperatureBound < temperatureError),
          "boundary smoke exact shared-flux stored balance exceeds accumulated "
          "row rounding");
  result.densityBalanceError = diagnostic(densityError, result);
  result.temperatureBalanceError = diagnostic(temperatureError, result);
  result.densityBalanceRoundoffBound = diagnostic(densityBound, result);
  result.temperatureBalanceRoundoffBound = diagnostic(temperatureBound, result);
  require(bool(pressure), "boundary smoke completed no pressure step");
  budget.work(add(mul(q.outputReserve, 4),
                  mul(add(q.cells.size(), q.faces.size()), 128)));
  auto density = scalarGrid(q, q.density, *source.density, *source.fluid,
                            o.names.density, true);
  auto temperature =
      scalarGrid(q, q.temperature,
                 source.temperature ? *source.temperature : *source.fluid,
                 *source.fluid, o.names.temperature, false);
  openvdb::GridPtrVec grids{source.velocity, pressure,        source.fluid,
                            source.kind,     source.fraction, source.prescribed,
                            source.wall,     density,         temperature};
  uint64_t nodes = reader.nodes, slots = reader.slots, full = 0,
           meta = metadata(*source.fileMetadata);
  for (const auto &g : grids) {
    visit(*g, [&](const auto &x) {
      const auto s = shape(x);
      nodes = add(nodes, s.nodes());
      slots = add(slots, mul(s.leaf, 512));
      full = add(full, x.memUsage());
    });
    meta = add(meta, metadata(*g));
  }
  require(nodes <= o.maxNodes && slots <= o.maxStoredLeafVoxels &&
              add(reader.meta, meta) <= o.maxMetadataBytes,
          "boundary smoke final joint node/leaf/metadata cap exceeded");
  budget.bytes(
      add(reader.resident(), add(mul(full, 8), mul(q.outputReserve, 2))));
  result.nodeUpperBound = std::max(q.outputNodes, nodes);
  result.workUpperBound = budget.charged;
  result.workspaceUpperBound = budget.peak;
  result.converged = true;
  const auto completed = temporary.next("complete");
  write(completed, grids, *source.fileMetadata, o.maxOutputBytes);
  static_assert(std::is_nothrow_move_constructible_v<BoundaryFluidStepResult>);
  publish(completed, output);
  return result;
}
#endif
} // namespace

BoundaryFluidStepResult
WriteBoundaryFluidStep(const BoundaryFluidInputs &input,
                       const std::filesystem::path &output,
                       const BoundaryFluidStepOptions &options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options, output);
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] { return run(input, output, options); });
#else
  (void)input;
  (void)output;
  (void)options;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
